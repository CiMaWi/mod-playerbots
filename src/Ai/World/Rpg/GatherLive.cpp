/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherLive.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "BotTravel.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GatherNodeMgr.h"
#include "GatherRules.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SkillPathBridge.h"
#include "SkillPathLive.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "WorldSession.h"

namespace Planner
{
namespace
{
constexpr std::size_t NODES_SHOWN = 12;
constexpr std::size_t STOPS_SHOWN = 24;
constexpr float PASS_BY_YARDS = 100.0f;  // the nodes the command offers the pass-by rule: this near, there and back
constexpr float DETOUR_FACTOR = 2.0f;
constexpr uint32 OUTLAND_MAP = 530;  // the content band of a kill's XP (Acore::XP::BaseGain)
constexpr uint32 NORTHREND_MAP = 571;
constexpr uint32 RATE_PUBLISH_MS = 15000;  // the observed rate's publication, a few times a minute (WPC13b)

// The observed rate's tally (WPC13b): the map threads add spans, the world thread fades and publishes it
std::mutex rateMutex;
GatherRate::Book rateBook;  // under rateMutex
uint64 rateSpans = 0;       // under rateMutex: spans observed since the start
uint64 rateSkipped = 0;     // under rateMutex: ended circuits that were no observation (mixed ways, cut at once)
SharedSlot<GatherRate::Snapshot> rateSlot;  // any thread
uint32 rateTimer = 0;                       // world thread only
uint32 ratePublishedMs = 0;                 // world thread only: the last publication's time

char const* WayWord(GatherRate::Way way)
{
    switch (way)
    {
        case GatherRate::Way::Herbalism:
            return "herbalism";
        case GatherRate::Way::Mining:
            return "mining";
        case GatherRate::Way::Creatures:
            return "creatures";
        default:
            return "none";
    }
}

void PublishRates(std::shared_ptr<GatherRate::Snapshot const> snapshot)
{
    ratePublishedMs = getMSTime();
    rateSlot.Publish(std::move(snapshot));
}

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

Gather::Gatherer GathererOf(Player* bot, Materials::Rates const& rates, GatherRate::View const& gatherRates)
{
    Gather::Gatherer gatherer;
    gatherer.zone = bot->GetZoneId();
    gatherer.gatherRates = gatherRates;
    for (uint16 const skill : {SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING})
        gatherer.skills.push_back({skill, bot->GetSkillValue(skill), bot->GetMaxSkillValue(skill)});
    gatherer.rates = rates;
    gatherer.rates.runSpeed = bot->GetSpeed(MOVE_RUN);
    gatherer.server = sGatherNodeMgr.Rates();
    return gatherer;
}

std::string SkillText(Gather::Gatherer const& gatherer, uint16 skill, char const* name)
{
    Gather::SkillValue const* value = gatherer.Find(skill);
    return value ? Acore::StringFormat("{} {}/{}", name, value->value, value->max) : std::string(name) + " none";
}

// The zone's creature spawns as the gather rules' creature stops (professions WR20c, DECISIONS WR.24): the drop rows'
// kinds that give a material the bot's skill path pays for (a hide only with skinning) or, for a bot short of gold,
// any kind with a row it can take; each of their spawns on the bot's map within a circuit's reach of it. A kill's
// yields are the kind's corpse rows, its hide its skinning rows, its XP the engine's base gain at the kind's middle
// level; the rows carry no coins (0). Refused: when every kind that gives a wanted material is one the bot may not
// fight (Gather::MayFight; not for friendliness), the wanted item and the least of their levels above the bot, for
// the map thread's skip row
struct CreatureStops
{
    std::vector<Gather::Node> stops;
    uint32 refusedItem{0};
    float refusedLevels{0.0f};
};

CreatureStops CreatureStopsOf(Snapshot const& snapshot, SkillPath::Path const* path, PathData const& data,
                              Gather::Gatherer const& gatherer)
{
    CreatureStops out;
    bool const wants = path && !path->materials.empty();
    if (!wants && !gatherer.shortOfGold)
        return out;
    bool const skinner = gatherer.Find(Gather::SKINNING) != nullptr;
    Market::View const market = CountedMarketView("market view: creature stops (worker)");
    std::vector<Recipes::DropRow const*> rows;
    std::vector<uint32> chosen;
    std::vector<uint32> wanted;
    for (Recipes::DropRow const& row : data.recipes->Drops())
    {
        bool const skin = row.kind == Recipes::DropKind::Skin;
        if (row.zone != snapshot.zone || (skin && row.skill != Gather::SKINNING))
            continue;
        rows.push_back(&row);
        if (skin && !skinner)
            continue;
        bool const material = path && SkillPath::MaterialOf(*path, row.item);
        if (material)
            wanted.push_back(row.entry);
        if (material || gatherer.shortOfGold)
            chosen.push_back(row.entry);
    }
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    ContentLevels const content = snapshot.position.map == OUTLAND_MAP     ? CONTENT_61_70
                                  : snapshot.position.map == NORTHREND_MAP ? CONTENT_71_80
                                                                           : CONTENT_1_60;
    float const reach = Gather::CIRCUIT_YARDS * Gather::CIRCUIT_YARDS;
    bool fightable = false;
    for (uint32 const entry : chosen)
    {
        Gather::Node kind;
        kind.kind = Gather::StopKind::Creature;
        uint32 item = 0;
        for (Recipes::DropRow const* row : rows)
            if (row->entry == entry)
            {
                SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, row->item) : nullptr;
                Gather::Yield const yield{row->item, row->perKill, material ? material->copperEach : 0u,
                                          SellCopper(row->item), MarketProceeds(market, row->item)};
                (row->kind == Recipes::DropKind::Skin ? kind.creature.hide : kind.yields).push_back(yield);
                kind.creature.minLevel = row->minLevel;
                kind.creature.maxLevel = row->maxLevel;
                kind.creature.eliteShare = row->eliteShare;
                kind.creature.friendlyTo = row->friendlyTo;
                if (material && !item)
                    item = row->item;
            }
        if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry))
            kind.creature.coinCopper =
                Gather::CoinsPerKill(creature->mingold, creature->maxgold, data.recipes->Rates().money);
        uint32 const top = std::max(kind.creature.minLevel, kind.creature.maxLevel);
        uint32 const middle = (kind.creature.minLevel + top) / 2;
        kind.creature.xpPerKill =
            static_cast<float>(Acore::XP::BaseGain(static_cast<uint8>(std::min<uint32>(snapshot.level, UINT8_MAX)),
                                                   static_cast<uint8>(std::min<uint32>(middle, UINT8_MAX)), content));
        kind.risk.levelsAbove = std::max(static_cast<float>(top) - static_cast<float>(snapshot.level), 0.0f);
        kind.risk.eliteShare = kind.creature.eliteShare;
        bool const isWanted = std::find(wanted.begin(), wanted.end(), entry) != wanted.end();
        if (Gather::MayFight(kind, gatherer))
            fightable = fightable || isWanted;
        else if (isWanted && !(kind.creature.friendlyTo & gatherer.side) &&
                 (!out.refusedItem || kind.risk.levelsAbove < out.refusedLevels))
        {
            out.refusedItem = item;
            out.refusedLevels = kind.risk.levelsAbove;
        }
        for (Recipes::DropSpawn const& spawn : data.recipes->DropSpawnsOf(snapshot.zone, entry))
        {
            float const dx = spawn.x - snapshot.position.x;
            float const dy = spawn.y - snapshot.position.y;
            if (spawn.mapId != snapshot.position.map || dx * dx + dy * dy > reach)
                continue;
            Gather::Node stop = kind;
            stop.id = spawn.spawnId;
            stop.at = {spawn.x, spawn.y, spawn.z};
            stop.respawnSecs = spawn.respawnSecs;
            out.stops.push_back(std::move(stop));
        }
    }
    if (fightable)
        out = {std::move(out.stops), 0, 0.0f};
    return out;
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
    Gather::Gatherer const gatherer = GathererOf(
        bot, SkillPathBridge::RatesOf(snapshot, SkillPathBridge::ReserveBeforeProfession(needs)), data.gatherRates);

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
                       PathData const& data, uint32 competing, bool shortOfGold, Goals::Situation& situation)
{
    if (!data.recipes)
        return;
    Gather::Gatherer gatherer;
    for (Skill const& skill : snapshot.skills)
        if (skill.id == SKILL_HERBALISM || skill.id == SKILL_MINING || skill.id == SKILL_SKINNING)
            gatherer.skills.push_back({skill.id, skill.value, skill.max});
    gatherer.level = snapshot.level;
    gatherer.side = Materials::SideBit(snapshot.team);
    gatherer.shortOfGold = shortOfGold;
    gatherer.zone = snapshot.zone;
    gatherer.gatherRates = data.gatherRates;
    gatherer.rates = rates;
    if (snapshot.runSpeed > 0.0f)
        gatherer.rates.runSpeed = snapshot.runSpeed;
    gatherer.rates.groupSize = std::max<uint8>(snapshot.groupSize, 1);
    std::vector<Gather::Node> nodes;
    if (gatherer.Find(SKILL_HERBALISM) || gatherer.Find(SKILL_MINING))
        if (std::vector<GatherNodeSpawn> const* spawns = ZoneSpawns(snapshot.position.map, snapshot.zone))
            nodes = NodesOf(*spawns, snapshot.zone, snapshot.level, path, data);
    CreatureStops creatures = CreatureStopsOf(snapshot, path, data, gatherer);
    // No creature stop for a test's nofight bot or one without a weapon; a refusal is still named
    situation.gatherUnarmed = Gather::DropCreatureStops(creatures.stops, situation.fightsHeld, snapshot.unarmed);
    situation.gatherRefusedItem = creatures.refusedItem;
    situation.gatherRefusedLevels = creatures.refusedLevels;
    nodes.insert(nodes.end(), std::make_move_iterator(creatures.stops.begin()),
                 std::make_move_iterator(creatures.stops.end()));
    if (nodes.empty())
        return;
    // Above the status's bag limit the circuit would end at once: none is planned and the refusal is named (WPC9)
    if (std::uint32_t const used = Gather::BagUsedPercent(snapshot.bagSlots, snapshot.bagFree);
        Gather::BagsTooFull(used))
    {
        situation.gatherBagsFull = used;
        return;
    }
    Gather::DropRefused(nodes, snapshot.position.map, snapshot.gatherRefused);
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
    // The respawn memory is read for every bot (a farmer must not plan the creature spawns it just cleared); the zone's
    // sight only for a herbalist or miner (WR25a, checkpoint 5 review)
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    uint32 const now = getMSTime();
    snapshot.gatherRefused = Gather::RefusedPlaces(botAI->GetTravel().Failures(), now);
    for (Gather::Memory::Entry const& entry : botAI->rpgInfo.gatherMemory.ring)
        if (botAI->rpgInfo.gatherMemory.Away(entry.spawn, now))
            snapshot.gatherDown.push_back(entry.spawn);
    // A player sees a node on the minimap while it is within the client's sight: the map's visibility range
    if (std::vector<GatherNodeSpawn> const* spawns =
            gatherer ? ZoneSpawns(snapshot.position.map, snapshot.zone) : nullptr)
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

void NoteGatherSpan(GatherRate::Span span, uint32 startMs)
{
    if (!startMs)
        return;
    span.milliseconds = getMSTimeDiff(startMs, getMSTime());
    std::lock_guard<std::mutex> lock(rateMutex);
    ++(rateBook.Observe(span) ? rateSpans : rateSkipped);
}

void UpdateGatherRates(uint32 diff)
{
    rateTimer += diff;
    if (rateTimer < RATE_PUBLISH_MS)
        return;
    PLAYERBOT_CHECK_WORLD_THREAD("UpdateGatherRates");
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "gather rates");
    std::shared_ptr<GatherRate::Snapshot const> snapshot;
    {
        std::lock_guard<std::mutex> lock(rateMutex);
        rateBook.Fade(rateTimer / GatherRate::MS_PER_MINUTE);
        snapshot = rateBook.Publish();
    }
    rateTimer = 0;
    PublishRates(std::move(snapshot));
    if (pmo)
        pmo->finish();
}

GatherRate::View GatherRatesNow() { return {rateSlot.Current()}; }

bool HandleRatesCommand(ChatHandler* handler, std::vector<std::string> const& args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("professions rates");
    if (!args.empty() && args[0] == "set")
    {
        WorldSession* const session = handler->GetSession();
        Optional<uint32> const zone = args.size() > 1 ? Acore::StringTo<uint32>(args[1]) : Optional<uint32>();
        GatherRate::Way way = GatherRate::Way::None;
        for (GatherRate::Way const each :
             {GatherRate::Way::Herbalism, GatherRate::Way::Mining, GatherRate::Way::Creatures})
            if (args.size() > 2 && args[2] == WayWord(each))
                way = each;
        Optional<double> const minutes = args.size() > 3 ? Acore::StringTo<double>(args[3]) : Optional<double>();
        Optional<double> const takes = args.size() > 4 ? Acore::StringTo<double>(args[4]) : Optional<double>();
        if ((session && session->GetSecurity() < SEC_GAMEMASTER) || !zone || !*zone || way == GatherRate::Way::None ||
            !minutes || !takes || *minutes < 0.0 || *takes < 0.0)
        {
            handler->SendErrorMessage(
                "Usage: playerbots professions rates set <zone> herbalism|mining|creatures "
                "<minutes> <takes> (a game master's test fixture)");
            return false;
        }
        std::shared_ptr<GatherRate::Snapshot const> snapshot;
        {
            std::lock_guard<std::mutex> lock(rateMutex);
            rateBook.tallies[GatherRate::KeyOf(*zone, way)] = {*minutes, *takes};
            snapshot = rateBook.Publish();
        }
        PublishRates(std::move(snapshot));
        handler->PSendSysMessage("Gather rates: zone {} {} set to {:.2f} min, {:.2f} takes, published", *zone,
                                 WayWord(way), *minutes, *takes);
        return true;
    }
    Optional<uint32> const only = args.empty() ? Optional<uint32>() : Acore::StringTo<uint32>(args[0]);
    if (!args.empty() && !only)
    {
        handler->SendErrorMessage("Usage: playerbots professions rates [zone]");
        return false;
    }
    std::map<uint64, GatherRate::Tally> tallies;
    uint64 spans = 0;
    uint64 skipped = 0;
    {
        std::lock_guard<std::mutex> lock(rateMutex);
        tallies.insert(rateBook.tallies.begin(), rateBook.tallies.end());
        spans = rateSpans;
        skipped = rateSkipped;
    }
    std::shared_ptr<GatherRate::Snapshot const> const published = rateSlot.Current();
    handler->PSendSysMessage(
        "Gather rates (WPC13b): {} spans observed since the start, {} not (mixed ways, cut at once or no zone); {} "
        "tallies, published {}; prior weight {:.0f} takes (observed from as many), half-life {:.0f} min",
        spans, skipped, tallies.size(),
        published ? Acore::StringFormat("{} s ago with {} tallies", getMSTimeDiff(ratePublishedMs, getMSTime()) / 1000,
                                        published->tallies.size())
                  : std::string("never (the static rule)"),
        GatherRate::PRIOR_TAKES, GatherRate::FADE_HALF_LIFE_MINUTES);
    for (auto const& [key, tally] : tallies)
    {
        uint32 const zone = static_cast<uint32>(key >> 8);
        if (only && *only != zone)
            continue;
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
        handler->PSendSysMessage(
            "  zone {} {}: {}, {:.1f} min, {:.2f} takes, {} min per take, {}", zone,
            area ? area->area_name[handler->GetSessionDbcLocale()] : "unknown",
            WayWord(static_cast<GatherRate::Way>(key & 0xFF)), tally.minutes, tally.takes,
            tally.takes > 0.0 ? Acore::StringFormat("{:.2f}", tally.minutes / tally.takes) : std::string("-"),
            tally.takes >= GatherRate::PRIOR_TAKES ? "observed" : "prior");
    }
    return true;
}
}  // namespace Planner
