/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherLive.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Chat.h"
#include "DBCStores.h"
#include "GatherNodeMgr.h"
#include "GatherRules.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SkillPathBridge.h"
#include "SkillPathLive.h"
#include "StringFormat.h"
#include "ThreadCheck.h"

namespace Planner
{
namespace
{
constexpr std::size_t NODES_SHOWN = 12;
constexpr std::size_t STOPS_SHOWN = 24;
constexpr float PASS_BY_YARDS = 100.0f;  // the nodes the command offers the pass-by rule: this near, there and back
constexpr float DETOUR_FACTOR = 2.0f;

uint32 SellCopper(uint32 item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return proto ? proto->SellPrice : 0;
}

// The zone's risk the command assumes: its level above the bot's (WR11's stand-in; the place risk is WR13b's)
Materials::PlaceRisk ZoneRisk(uint32 zone, uint32 level)
{
    Materials::PlaceRisk risk;
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
    if (area && area->area_level > static_cast<int32>(level))
        risk.levelsAbove = static_cast<float>(area->area_level - static_cast<int32>(level));
    return risk;
}

Gather::Gatherer GathererOf(Player* bot, Materials::Rates const& rates)
{
    Gather::Gatherer gatherer;
    for (uint16 const skill : {SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING})
        gatherer.skills.push_back({skill, bot->GetSkillValue(skill), bot->GetMaxSkillValue(skill)});
    gatherer.rates = rates;
    gatherer.rates.runSpeed = bot->GetSpeed(MOVE_RUN);
    return gatherer;
}

std::string SkillText(Gather::Gatherer const& gatherer, uint16 skill, char const* name)
{
    Gather::SkillValue const* value = gatherer.Find(skill);
    return value ? Acore::StringFormat("{} {}/{}", name, value->value, value->max) : std::string(name) + " none";
}
}  // namespace

bool HandleNodesCommand(ChatHandler* handler, std::string name)
{
    Player* bot = !name.empty() && normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI || !bot->IsInWorld())
    {
        handler->SendErrorMessage("Usage: playerbots professions nodes <bot> (an online bot; {} is none)", name);
        return false;
    }
    PathData const data = PathDataNow();
    std::shared_ptr<UpkeepIndex const> const upkeep = UpkeepIndexReady() ? CurrentUpkeepIndex() : nullptr;
    if (!upkeep || !data.recipes)
    {
        handler->SendErrorMessage("Gather nodes: the upkeep or recipe index is not ready");
        return false;
    }
    // Commands run on the world thread outside the map updates: no map thread touches the bot (as `professions path`)
    PLAYERBOT_CHECK_WORLD_THREAD("professions nodes");
    Snapshot const snapshot = FillSnapshot(bot, 0);
    UpkeepNeeds::NeedList needs;
    std::shared_ptr<SkillPath::Path const> const path =
        NeedsWithPath(snapshot, *upkeep, BotNeedRates(botAI, snapshot), PathInputsOf(botAI), data, needs);
    Gather::Gatherer const gatherer =
        GathererOf(bot, SkillPathBridge::RatesOf(snapshot, SkillPathBridge::ReserveBeforeProfession(needs)));

    uint32 const mapId = bot->GetMapId();
    uint32 const zone = bot->GetZoneId();
    std::vector<GatherNodeSpawn> const* spawns = nullptr;
    auto const& all = sGatherNodeMgr.Spawns();
    if (auto const map = all.find(mapId); map != all.end())
        if (auto const zoneSpawns = map->second.find(zone); zoneSpawns != map->second.end())
            spawns = &zoneSpawns->second;

    // The yields of the zone's node kinds (the recipe index's node rows), at the bot's prices
    std::unordered_map<uint32, std::vector<Gather::Yield>> yieldsOf;
    for (Recipes::NodeRow const& row : data.recipes->Nodes())
        if (row.zone == zone)
        {
            SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, row.item) : nullptr;
            yieldsOf[row.entry].push_back(
                {row.item, row.perNode, material ? material->copperEach : 0u, SellCopper(row.item), 0u});
        }

    Materials::PlaceRisk const risk = ZoneRisk(zone, snapshot.level);
    Planner::Circuit::Point const here{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    std::vector<Gather::Node> nodes;
    std::unordered_map<uint32, uint32> entryOf;
    uint32 usable = 0;
    uint32 pooled = 0;
    std::vector<GatherNodeSpawn> const none;
    for (GatherNodeSpawn const& spawn : spawns ? *spawns : none)
    {
        entryOf[spawn.spawnId] = spawn.entry;
        Gather::Node node;
        node.id = spawn.spawnId;
        node.skill = static_cast<uint16>(spawn.skillId);
        node.required = spawn.reqSkillValue;
        node.at = {spawn.pos.GetPositionX(), spawn.pos.GetPositionY(), spawn.pos.GetPositionZ()};
        node.respawnSecs = spawn.respawnSecs;
        node.share = spawn.share;
        node.risk = risk;
        if (auto const yields = yieldsOf.find(spawn.entry); yields != yieldsOf.end())
            node.yields = yields->second;
        usable += Gather::Usable(node, gatherer) ? 1 : 0;
        pooled += spawn.share < 1.0f ? 1 : 0;
        nodes.push_back(std::move(node));
    }
    std::sort(nodes.begin(), nodes.end(),
              [&here](Gather::Node const& a, Gather::Node const& b)
              {
                  float const ya = Planner::Circuit::Yards(here, a.at);
                  float const yb = Planner::Circuit::Yards(here, b.at);
                  return ya != yb ? ya < yb : a.id < b.id;
              });

    handler->PSendSysMessage(
        "Gather nodes of {} (level {}, zone {}, {}, {}, {}, run {:.1f} yd/s, need factor {:.2f}, "
        "minute {:.1f}): {} spawn points, {} usable, {} pooled",
        bot->GetName(), snapshot.level, zone, SkillText(gatherer, SKILL_HERBALISM, "herbalism"),
        SkillText(gatherer, SKILL_MINING, "mining"), SkillText(gatherer, SKILL_SKINNING, "skinning"),
        gatherer.rates.runSpeed, gatherer.rates.factor, gatherer.rates.minuteValue, nodes.size(), usable, pooled);
    std::vector<Gather::Seen> inReach;
    for (std::size_t at = 0; at < nodes.size(); ++at)
    {
        Gather::Node const& node = nodes[at];
        float const yards = Planner::Circuit::Yards(here, node.at);
        Gather::Seen const seen{node, yards * DETOUR_FACTOR};
        if (yards <= PASS_BY_YARDS)
            inReach.push_back(seen);
        if (at >= NODES_SHOWN)
            continue;
        double const net = Gather::Net(seen, gatherer);
        handler->PSendSysMessage(
            "  - spawn {} entry {} skill {} req {} at {:.0f} yd: respawn {} s, share {:.2f}, "
            "{} yields, worth {:.1f}, net there and back {}",
            node.id, entryOf[node.id], node.skill, node.required, yards, node.respawnSecs, node.share,
            node.yields.size(), Gather::NodeWorth(node, gatherer),
            net == Gather::REFUSED ? std::string("refused") : Acore::StringFormat("{:.1f}", net));
    }
    std::size_t const pick = Gather::PassBy(inReach, gatherer);
    handler->PSendSysMessage(
        "  pass by (within {:.0f} yd): {}", PASS_BY_YARDS,
        pick == Gather::NO_PICK ? std::string("none pays") : Acore::StringFormat("spawn {}", inReach[pick].node.id));

    Gather::Circuit const circuit = Gather::BuildCircuit(here, nodes, gatherer);
    std::string stops;
    for (std::size_t at = 0; at < circuit.stops.size() && at < STOPS_SHOWN; ++at)
        stops += Acore::StringFormat("{}{}", at ? " " : "", circuit.stops[at]);
    handler->PSendSysMessage(
        "  circuit from here: {} stops, lap {:.0f} yd, {:.1f} min, longest respawn {} s, {}, worth "
        "{:.1f} per lap{}{}",
        circuit.stops.size(), circuit.lapYards, circuit.lapMinutes, circuit.longestRespawnSecs,
        circuit.once ? std::string("walked once") : Acore::StringFormat("{} laps", circuit.laps), circuit.worthPerLap,
        stops.empty() ? "" : ": ", stops);
    return true;
}
}  // namespace Planner
