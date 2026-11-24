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
#include "Map.h"
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
#include "Timer.h"

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

// The spawn point's risk to a bot of the level (WR13b): its hostile creatures' level above the bot's, their elite
// share (GatherNodeMgr::Load; none within 40 yd: those within 120 yd; none there: 0)
Materials::PlaceRisk SpawnRisk(GatherNodeSpawn const& spawn, uint32 level)
{
    Materials::PlaceRisk risk;
    risk.levelsAbove = std::max(spawn.mobLevel - static_cast<float>(level), 0.0f);
    risk.eliteShare = spawn.eliteShare;
    return risk;
}

std::vector<GatherNodeSpawn> const* ZoneSpawns(uint32 mapId, uint32 zone)
{
    auto const& all = sGatherNodeMgr.Spawns();
    if (auto const map = all.find(mapId); map != all.end())
        if (auto const zoneSpawns = map->second.find(zone); zoneSpawns != map->second.end())
            return &zoneSpawns->second;
    return nullptr;
}

// The zone's spawn points as the gather rules' nodes: their yields (the recipe index's node rows) at the bot's prices
// and their risk to it
std::vector<Gather::Node> NodesOf(std::vector<GatherNodeSpawn> const& spawns, uint32 zone, uint32 level,
                                  SkillPath::Path const* path, PathData const& data)
{
    std::unordered_map<uint32, std::vector<Gather::Yield>> yieldsOf;
    for (Recipes::NodeRow const& row : data.recipes->Nodes())
        if (row.zone == zone)
        {
            SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, row.item) : nullptr;
            yieldsOf[row.entry].push_back(
                {row.item, row.perNode, material ? material->copperEach : 0u, SellCopper(row.item), 0u});
        }
    std::vector<Gather::Node> nodes;
    nodes.reserve(spawns.size());
    for (GatherNodeSpawn const& spawn : spawns)
    {
        Gather::Node node;
        node.id = spawn.spawnId;
        node.skill = static_cast<uint16>(spawn.skillId);
        node.required = spawn.reqSkillValue;
        node.at = {spawn.pos.GetPositionX(), spawn.pos.GetPositionY(), spawn.pos.GetPositionZ()};
        node.respawnSecs = spawn.respawnSecs;
        node.share = spawn.share;
        node.risk = SpawnRisk(spawn, level);
        if (auto const yields = yieldsOf.find(spawn.entry); yields != yieldsOf.end())
            node.yields = yields->second;
        nodes.push_back(std::move(node));
    }
    return nodes;
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
    std::vector<GatherNodeSpawn> const none;
    std::vector<GatherNodeSpawn> const* found = ZoneSpawns(mapId, zone);
    std::vector<GatherNodeSpawn> const& spawns = found ? *found : none;
    Planner::Circuit::Point const here{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    std::vector<Gather::Node> nodes = NodesOf(spawns, zone, snapshot.level, path.get(), data);
    Gather::ApplySight(nodes, snapshot.gatherUp, snapshot.gatherDown);
    std::unordered_map<uint32, GatherNodeSpawn const*> spawnOf;
    uint32 usable = 0;
    uint32 pooled = 0;
    for (GatherNodeSpawn const& spawn : spawns)
    {
        spawnOf[spawn.spawnId] = &spawn;
        pooled += spawn.share < 1.0f ? 1 : 0;
    }
    for (Gather::Node const& node : nodes)
        usable += Gather::Usable(node, gatherer) ? 1 : 0;
    std::sort(nodes.begin(), nodes.end(),
              [&here](Gather::Node const& a, Gather::Node const& b)
              {
                  float const ya = Planner::Circuit::Yards(here, a.at);
                  float const yb = Planner::Circuit::Yards(here, b.at);
                  return ya != yb ? ya < yb : a.id < b.id;
              });

    handler->PSendSysMessage(
        "Gather nodes of {} (level {}, zone {}, {}, {}, {}, run {:.1f} yd/s, need factor {:.2f}, "
        "minute {:.1f}): {} spawn points, {} usable, {} pooled, {} seen up, {} empty or taken",
        bot->GetName(), snapshot.level, zone, SkillText(gatherer, SKILL_HERBALISM, "herbalism"),
        SkillText(gatherer, SKILL_MINING, "mining"), SkillText(gatherer, SKILL_SKINNING, "skinning"),
        gatherer.rates.runSpeed, gatherer.rates.factor, gatherer.rates.minuteValue, nodes.size(), usable, pooled,
        snapshot.gatherUp.size(), snapshot.gatherDown.size());
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
            "  - spawn {} entry {} skill {} req {} at {:.0f} yd: respawn {} s, share {:.2f}, danger level {:.0f} "
            "(+{:.0f}, elite {:.2f}), {} yields, worth {:.1f}, net there and back {}",
            node.id, spawnOf[node.id]->entry, node.skill, node.required, yards, node.respawnSecs, node.share,
            spawnOf[node.id]->mobLevel, node.risk.levelsAbove, node.risk.eliteShare, node.yields.size(),
            Gather::NodeWorth(node, gatherer),
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
    Goals::GatherNode const option = Gather::CircuitOption(circuit, nodes, gatherer, mapId);
    if (option.id)
        handler->PSendSysMessage(
            "  planner option: first stop {}, {:.1f} min, copper {:.0f}, item worth {:.0f}, skill points {:.1f}, "
            "danger +{:.1f} (elite {:.2f})",
            option.id, option.gatherMinutes, option.copperWorth, option.itemWorth, option.skillPoints,
            option.levelsAbove, option.eliteShare);
    return true;
}

void FillGatherCircuit(Snapshot const& snapshot, SkillPath::Path const* path, Materials::Rates const& rates,
                       PathData const& data, uint32 competing, Goals::Situation& situation)
{
    if (!data.recipes)
        return;
    Gather::Gatherer gatherer;
    for (Skill const& skill : snapshot.skills)
        if (skill.id == SKILL_HERBALISM || skill.id == SKILL_MINING)
            gatherer.skills.push_back({skill.id, skill.value, skill.max});
    if (!gatherer.Find(SKILL_HERBALISM) && !gatherer.Find(SKILL_MINING))
        return;
    std::vector<GatherNodeSpawn> const* spawns = ZoneSpawns(snapshot.position.map, snapshot.zone);
    if (!spawns)
        return;
    gatherer.rates = rates;
    if (snapshot.runSpeed > 0.0f)
        gatherer.rates.runSpeed = snapshot.runSpeed;
    gatherer.rates.groupSize = std::max<uint8>(snapshot.groupSize, 1);
    std::vector<Gather::Node> nodes = NodesOf(*spawns, snapshot.zone, snapshot.level, path, data);
    Gather::ApplySight(nodes, snapshot.gatherUp, snapshot.gatherDown);
    Planner::Circuit::Point const here{snapshot.position.x, snapshot.position.y, snapshot.position.z};
    Gather::Circuit const circuit = Gather::BuildCircuit(here, nodes, gatherer);
    Goals::GatherNode option = Gather::CircuitOption(circuit, nodes, gatherer, snapshot.position.map);
    if (!option.id)
        return;
    option.competingBots = competing;
    situation.nodes.push_back(std::move(option));
}

void FillGatherSight(Player* bot, Snapshot& snapshot)
{
    bool gatherer = false;
    for (Skill const& skill : snapshot.skills)
        gatherer = gatherer || skill.id == SKILL_HERBALISM || skill.id == SKILL_MINING;
    PlayerbotAI* botAI = gatherer ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
        return;
    uint32 const now = getMSTime();
    for (Gather::Memory::Entry const& entry : botAI->rpgInfo.gatherMemory.ring)
        if (botAI->rpgInfo.gatherMemory.Away(entry.spawn, now))
            snapshot.gatherDown.push_back(entry.spawn);
    // A player sees a node on the minimap while it is within the client's sight: the map's visibility range
    if (std::vector<GatherNodeSpawn> const* spawns = ZoneSpawns(snapshot.position.map, snapshot.zone))
    {
        Map* map = bot->GetMap();
        WorldPosition at(bot);
        float const sight = map->GetVisibilityRange();
        for (GatherNodeSpawn const& spawn : *spawns)
            if (at.sqDistance(spawn.pos) <= sight * sight &&
                map->IsGridLoaded(spawn.pos.GetPositionX(), spawn.pos.GetPositionY()))
                (GatherNodeMgr::FindLiveNode(map, spawn.spawnId) ? snapshot.gatherUp : snapshot.gatherDown)
                    .push_back(spawn.spawnId);
    }
    std::sort(snapshot.gatherUp.begin(), snapshot.gatherUp.end());
    std::sort(snapshot.gatherDown.begin(), snapshot.gatherDown.end());
    snapshot.gatherDown.erase(std::unique(snapshot.gatherDown.begin(), snapshot.gatherDown.end()),
                              snapshot.gatherDown.end());
}
}  // namespace Planner
