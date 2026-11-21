/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERREACHHARNESS_H
#define _PLAYERBOT_PLANNERREACHHARNESS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "GoalModelHarness.h"
#include "PlannerRouteCost.h"
#include "PlannerRouteCostHarness.h"
#include "QuestCircuitLog.h"
#include "TravelGraph.h"

// Offline proof of planning WN1b (PLAN 3.20 (4)-(6)): the planner's candidates come from every map, each priced by its
// route, and a place without a route is no option. The sea world: a home continent (map 0) with the bot's zone, a
// near zone by road, the enemy's zone that fits by level and a zone no link reaches; a continent across the sea (map
// 1, one ship from the home dock) with a far zone and a class quest's giver; an island (map 2, its own ship to the home
// dock) whose one zone ends at level 20. Six cases, and three invariants searched for a counterexample over a grid of
// states (start, level, side, carried quest, transports, the cost: none, the cap, a small cap). tests/goal-model-
// selftest.cpp runs it
namespace Planner
{
namespace ReachHarness
{
using namespace Goals;
using GoalHarness::Add;
using GoalHarness::BOT_X;
using GoalHarness::BOT_Y;
using GoalHarness::Check;

constexpr std::uint32_t HOME_MAP = 0;
constexpr std::uint32_t FAR_MAP = 1;
constexpr std::uint32_t ISLAND_MAP = 2;
constexpr std::uint32_t Z_HOME = 2001;
constexpr std::uint32_t Z_NEAR = 2002;
constexpr std::uint32_t Z_ENEMY = 2003;
constexpr std::uint32_t Z_BLOCKED = 2004;
constexpr std::uint32_t Z_FAR = 2005;
constexpr std::uint32_t Z_ISLAND = 2006;
constexpr std::uint32_t SEA_QUESTS = 3000;    // quest ids of the sea world
constexpr std::uint32_t SEA_ENTRIES = 96000;  // its creature entries
constexpr std::uint32_t CLASS_QUEST = 1498;   // a warrior's class quest of ClassQuests::TABLE (class-quest-selftest)
constexpr std::uint32_t CLASS_PREV = 1505;
constexpr std::uint32_t HORDE_RACES = 690;
constexpr std::uint32_t CLASS_LEVEL_XP = 7600;
constexpr float CLASS_EAST = 1000.0f;  // the class giver's spot east of the far camp
constexpr std::uint32_t SHIP = 20808;  // the vessel to the far continent
constexpr std::uint32_t ISLAND_SHIP = 20809;
constexpr std::uint32_t SHIP_PERIOD_MS = 600000;
constexpr float SHIP_RIDE_SECONDS = 180.0f;
constexpr float SHIP_YARDS = 3000.0f;
constexpr float CLUSTER_YARDS = 15.0f;
constexpr float DOCK_YARDS = 40.0f;
constexpr std::uint32_t NODE_BASE = 1000;
constexpr std::uint32_t NODES_PER_CAMP = 10;  // camp x 10 + 1 centre, 2-5 around it, 6 dock
constexpr std::uint32_t CENTRE = 1;
constexpr std::uint32_t DOCK = 6;
constexpr std::uint32_t CLASS_NODE = NODE_BASE + 99;
constexpr std::uint32_t SMALL_CAP = 2;
constexpr float MINUTES_EPSILON = 0.001f;
constexpr std::uint8_t FIRST_LEVEL = 8;
constexpr std::uint8_t LAST_LEVEL = 32;
constexpr std::size_t SHOWN = 4;

struct SeaCamp
{
    char const* name;
    std::uint32_t map;
    float east;  // yards east of the bot's spot on its map
    std::uint32_t zone;
    std::int32_t questLevel;
    std::uint32_t minLevel;
};

constexpr SeaCamp SEA_CAMPS[] = {
    {"home", HOME_MAP, 0.0f, Z_HOME, 12, 8},       {"near", HOME_MAP, 800.0f, Z_NEAR, 21, 16},
    {"enemy", HOME_MAP, 1200.0f, Z_ENEMY, 21, 16}, {"blocked", HOME_MAP, 1600.0f, Z_BLOCKED, 27, 20},
    {"far", FAR_MAP, 0.0f, Z_FAR, 21, 16},         {"island", ISLAND_MAP, 0.0f, Z_ISLAND, 14, 10}};
constexpr std::size_t HOME = 0;
constexpr std::size_t NEAR = 1;
constexpr std::size_t ENEMY = 2;
constexpr std::size_t BLOCKED = 3;
constexpr std::size_t FAR = 4;
constexpr std::size_t ISLAND = 5;

struct SeaWorld
{
    QuestIndex quests;
    PlannerIndexes indexes;
    std::vector<std::uint32_t> hubs;  // per SEA_CAMPS entry
};

inline float CampX(std::size_t camp) { return BOT_X + SEA_CAMPS[camp].east; }

// A camp's first quest (its giver is its ender)
inline std::uint32_t QuestOf(std::size_t camp) { return SEA_QUESTS + 1 + 2 * static_cast<std::uint32_t>(camp); }

inline SeaWorld Build()
{
    GatherInput services;
    // The far zone ends at 22 while its quests stay green some levels above: there only the bracket's top keeps it out
    services.zones = {
        {Z_HOME, 10, 20, ZoneSide::Alliance, HOME_MAP}, {Z_NEAR, 18, 28, ZoneSide::Alliance, HOME_MAP},
        {Z_ENEMY, 18, 28, ZoneSide::Horde, HOME_MAP},   {Z_BLOCKED, 20, 32, ZoneSide::Alliance, HOME_MAP},
        {Z_FAR, 18, 22, ZoneSide::Alliance, FAR_MAP},   {Z_ISLAND, 10, 20, ZoneSide::Alliance, ISLAND_MAP}};
    QuestGather gather;
    std::uint32_t entry = SEA_ENTRIES;
    std::uint32_t questId = SEA_QUESTS;
    auto const quest = [&](std::uint32_t id, std::uint32_t map, float x, float y, std::uint32_t giver)
    {
        QuestRecord record;
        record.id = id;
        record.xpBase = 400;
        std::uint32_t const target = ++entry;
        record.objectives.push_back({ObjectiveKind::Kill, target, 6u, 0, {}});
        for (float const offset : {-8.0f, 0.0f, 8.0f})
            gather.targets.push_back({target, false, map, x + offset, y + offset, 0.0f});
        gather.relations.push_back({id, giver, false, false});
        gather.relations.push_back({id, giver, false, true});
        return record;
    };
    for (SeaCamp const& camp : SEA_CAMPS)
    {
        float const x = BOT_X + camp.east;
        for (float const dx : {-10.0f, 10.0f})
            for (float const dy : {-10.0f, 10.0f})
                services.spawns.push_back(
                    {camp.map, x + dx, BOT_Y + dy, 0.0f, ++entry, NpcFlag::VENDOR, 0, camp.zone, camp.zone, 0, false});
        std::uint32_t const giver = ++entry;
        gather.places.push_back({camp.map, x, BOT_Y + 5.0f, 0.0f, giver, false, camp.zone, camp.zone, NO_CAMP});
        for (float const north : {0.0f, 60.0f})
        {
            QuestRecord record = quest(++questId, camp.map, x + 60.0f, BOT_Y + north, giver);
            record.level = camp.questLevel;
            record.minLevel = camp.minLevel;
            gather.quests.push_back(std::move(record));
        }
    }
    // The class quest's giver across the sea, alone in its camp
    float const classX = CampX(FAR) + CLASS_EAST;
    std::uint32_t const classGiver = ++entry;
    gather.places.push_back({FAR_MAP, classX, BOT_Y + 5.0f, 0.0f, classGiver, false, Z_FAR, Z_FAR, NO_CAMP});
    QuestRecord record = quest(CLASS_QUEST, FAR_MAP, classX + 70.0f, BOT_Y + 30.0f, classGiver);
    record.level = 10;
    record.minLevel = 10;
    record.xpBase = 550;
    record.classMask = 1;
    record.raceMask = HORDE_RACES;
    record.prevAny = {static_cast<std::int32_t>(CLASS_PREV)};
    gather.quests.push_back(std::move(record));
    SeaWorld world{QuestIndex::Build(std::move(gather)), PlannerIndexes::Build(std::move(services)), {}};
    for (SeaCamp const& camp : SEA_CAMPS)
    {
        std::vector<Nearby> const hub = world.indexes.NearestHubs(camp.map, BOT_X + camp.east, BOT_Y, 0.0f, 1);
        world.hubs.push_back(hub.empty() ? NO_HUB : hub.front().index);
    }
    return world;
}

inline std::uint32_t NodeId(std::size_t camp, std::uint32_t slot)
{
    return NODE_BASE + static_cast<std::uint32_t>(camp) * NODES_PER_CAMP + slot;
}

// The road joins home, near and the enemy's zone on map 0 (the blocked zone has no link); the far camp and the class
// giver on map 1; a dock beside home, the far camp and the island, a ship from home to the far continent and one from
// the island to home
inline bool BuildGraph(TravelGraph& graph, std::string& error)
{
    TravelGraphBuilder builder;
    auto const link =
        [&builder](std::uint32_t a, std::uint32_t b, float yards, std::uint32_t type, std::uint32_t object)
    {
        for (auto const& [from, to] : {std::pair{a, b}, std::pair{b, a}})
        {
            TravelGraphBuilder::LinkRow row;
            row.fromId = from;
            row.toId = to;
            row.type = type;
            row.object = object;
            row.distance = yards;
            row.periodMs = type == 3 ? SHIP_PERIOD_MS : 0;
            row.rideSeconds = type == 3 ? SHIP_RIDE_SECONDS : 0.0f;
            builder.AddLink(row);
        }
    };
    for (std::size_t camp = 0; camp < std::size(SEA_CAMPS); ++camp)
    {
        float const x = CampX(camp);
        std::uint32_t const map = SEA_CAMPS[camp].map;
        builder.AddNode(NodeId(camp, CENTRE), SEA_CAMPS[camp].name, map, x, BOT_Y, 0.0f);
        std::uint32_t slot = CENTRE;
        for (auto const& [dx, dy] : {std::pair{CLUSTER_YARDS, 0.0f}, std::pair{-CLUSTER_YARDS, 0.0f},
                                     std::pair{0.0f, CLUSTER_YARDS}, std::pair{0.0f, -CLUSTER_YARDS}})
        {
            builder.AddNode(NodeId(camp, ++slot), SEA_CAMPS[camp].name, map, x + dx, BOT_Y + dy, 0.0f);
            link(NodeId(camp, CENTRE), NodeId(camp, slot), CLUSTER_YARDS, 1, 0);
        }
    }
    for (auto const& [a, b] : {std::pair{HOME, NEAR}, std::pair{NEAR, ENEMY}})
        link(NodeId(a, CENTRE), NodeId(b, CENTRE), CampX(b) - CampX(a), 1, 0);
    builder.AddNode(CLASS_NODE, "class giver", FAR_MAP, CampX(FAR) + CLASS_EAST, BOT_Y, 0.0f);
    link(NodeId(FAR, CENTRE), CLASS_NODE, CLASS_EAST, 1, 0);
    float const side = DOCK_YARDS * std::sqrt(2.0f);
    for (std::size_t const camp : {HOME, FAR, ISLAND})
    {
        builder.AddNode(NodeId(camp, DOCK), "dock", SEA_CAMPS[camp].map, CampX(camp) - DOCK_YARDS, BOT_Y - DOCK_YARDS,
                        0.0f);
        link(NodeId(camp, CENTRE), NodeId(camp, DOCK), side, 1, 0);
    }
    // A ship's crossing rows: its two stops, boarding and leaving at each
    auto const ship = [&builder, &link](std::uint32_t vessel, std::size_t a, std::size_t b)
    {
        std::uint32_t stop = 0;
        for (std::size_t const camp : {a, b})
        {
            for (std::uint32_t direction = 0; direction < 2; ++direction)
            {
                TravelGraphBuilder::CrossingRow row;
                row.entry = vessel;
                row.stop = stop;
                row.mapId = SEA_CAMPS[camp].map;
                row.x = CampX(camp) - DOCK_YARDS;
                row.y = BOT_Y - DOCK_YARDS;
                row.direction = direction;
                builder.AddCrossing(row);
            }
            ++stop;
        }
        link(NodeId(a, DOCK), NodeId(b, DOCK), SHIP_YARDS, 3, vessel);
    };
    ship(SHIP, HOME, FAR);
    ship(ISLAND_SHIP, ISLAND, HOME);
    if (!builder.EndLinks(error))
        return false;
    builder.AddPoint(NodeId(HOME, CENTRE), NodeId(NEAR, CENTRE), 0, HOME_MAP, CampX(HOME), BOT_Y, 0.0f);
    builder.AddPoint(NodeId(HOME, CENTRE), NodeId(NEAR, CENTRE), 1, HOME_MAP, CampX(NEAR), BOT_Y, 0.0f);
    if (!builder.Finish(graph, error))
        return false;
    graph.Publish(0, 0);
    return true;
}

// A bot of the side at a camp's spot, bound there
inline Snapshot BotAt(std::size_t camp, std::uint8_t level, std::uint8_t team)
{
    Snapshot bot = GoalHarness::Bot(level, GoalHarness::FLUSH);
    bot.position = {SEA_CAMPS[camp].map, CampX(camp), BOT_Y, 0.0f};
    bot.zone = SEA_CAMPS[camp].zone;
    bot.area = SEA_CAMPS[camp].zone;
    bot.hearth = bot.position;
    bot.team = team;
    if (team)
        bot.race = 2;  // orc
    return bot;
}

inline void Carry(Snapshot& bot, std::uint32_t quest)
{
    QuestSlot slot;
    slot.quest = quest;
    slot.state = Circuit::SLOT_COMPLETE;
    bot.quests.push_back(slot);
}

// One plan of the bot: its cost (none: hook-free), the plan, the next-zone hubs and deliveries the fills give
struct Run
{
    std::shared_ptr<RouteCost> cost;
    GoalPlan plan;
    Situation filled;
    std::vector<Delivery> deliveries;
};

inline Run Plan(SeaWorld const& world, TravelGraph const& graph, Snapshot bot, bool transport, std::uint32_t cap,
                Situation live = {})
{
    Run run;
    CircuitContext context;
    context.quests = &world.quests;
    context.indexes = &world.indexes;
    if (cap)
    {
        bot.route = RouteCostHarness::RouteOf(bot, false, transport);
        run.cost = std::make_shared<RouteCost>(graph, bot.route, RouteCostInputs{}, cap);
        context.travel = RouteCost::Hook(run.cost);
    }
    live.here.hub = NO_HUB;
    run.plan = PlanGoals(bot, context, GoalHarness::Reserved(live, bot.level));
    run.filled = live;
    GoalHarness::Filled(bot, context, run.filled);
    run.deliveries = Deliveries(bot, context, run.plan.circuit, bot.runSpeed);
    return run;
}

inline HubOption const* HubOf(Run const& run, SeaWorld const& world, std::size_t camp)
{
    for (HubOption const& hub : run.filled.hubs)
        if (hub.hub == world.hubs[camp])
            return &hub;
    return nullptr;
}

inline bool HasReason(Ranked const& ranked, std::string const& reason)
{
    return std::any_of(ranked.options.begin(), ranked.options.end(),
                       [&reason](Option const& option) { return option.reason == reason; });
}

inline std::string Names(SeaWorld const& world, std::vector<HubOption> const& hubs)
{
    std::string text;
    for (HubOption const& hub : hubs)
    {
        auto const at = std::find(world.hubs.begin(), world.hubs.end(), hub.hub);
        text +=
            (text.empty() ? "" : ",") +
            (at == world.hubs.end() ? std::to_string(hub.hub) : std::string(SEA_CAMPS[at - world.hubs.begin()].name));
    }
    return text.empty() ? "none" : text;
}

inline bool HasRow(std::vector<CircuitLog::Row> const& rows, char const* choice, char const* reason)
{
    return std::any_of(rows.begin(), rows.end(),
                       [&](CircuitLog::Row const& row) { return row.choice == choice && row.reason == reason; });
}

inline std::string RowText(std::vector<CircuitLog::Row> const& rows)
{
    std::string text;
    for (CircuitLog::Row const& row : rows)
        text += " " + row.choice + "/" + row.reason + "=" + std::to_string(static_cast<int>(row.value));
    return text.empty() ? " none" : text;
}

inline bool RouteMinutes(Run const& run, Snapshot const& bot, Place const& to, float minutes)
{
    std::optional<float> const seconds = run.cost ? run.cost->Answer(bot.position, to) : std::nullopt;
    return seconds && *seconds >= 0.0f && std::abs(minutes - *seconds / SECONDS_PER_MINUTE) <= MINUTES_EPSILON;
}

inline void Cases(std::vector<Check>& checks, SeaWorld const& world, TravelGraph const& graph)
{
    // 1) Past the island's bracket: zones across the sea by ship, where before (no cost) there was nowhere
    Snapshot const islander = BotAt(ISLAND, 22, 0);
    Run const island = Plan(world, graph, islander, true, ROUTE_SEARCHES_PER_JOB);
    Run const islandBare = Plan(world, graph, islander, true, 0);
    HubOption const* nearHub = HubOf(island, world, NEAR);
    HubOption const* farHub = HubOf(island, world, FAR);
    Add(checks, "reach 1: a bot past the island's bracket is offered zones across the sea, priced by their routes",
        nearHub && farHub && RouteMinutes(island, islander, nearHub->place, nearHub->travelMinutes) &&
            RouteMinutes(island, islander, farHub->place, farHub->travelMinutes) && islandBare.filled.hubs.empty() &&
            HasReason(island.plan.ranked, "outlevelled_other_map") && !island.plan.routeGaps.noNextZone,
        "hubs " + Names(world, island.filled.hubs) + ", without a cost " + Names(world, islandBare.filled.hubs) + "; " +
            GoalHarness::Summary(island.plan.ranked));

    // 2) A fitting zone by road and a farther one across the sea: both offered, the cost decides
    Snapshot const home = BotAt(HOME, 22, 0);
    Run const both = Plan(world, graph, home, true, ROUTE_SEARCHES_PER_JOB);
    HubOption const* byRoad = HubOf(both, world, NEAR);
    HubOption const* bySea = HubOf(both, world, FAR);
    Option const* top = GoalHarness::Top(both.plan.ranked);
    Add(checks, "reach 2: a zone by road and one across the sea are both offered; the cheaper route wins",
        byRoad && bySea && RouteMinutes(both, home, bySea->place, bySea->travelMinutes) &&
            byRoad->travelMinutes < bySea->travelMinutes && top && top->kind == GoalKind::MoveZone &&
            top->target == world.hubs[NEAR] && HasReason(both.plan.ranked, "outlevelled_other_map"),
        "hubs " + Names(world, both.filled.hubs) + "; " + GoalHarness::Summary(both.plan.ranked));

    // 3) The only fitting zone lies behind no link: no_route, no next zone, and the bot keeps its other goals (a
    // delivery across the sea)
    Snapshot stuck = BotAt(HOME, 30, 0);
    Carry(stuck, QuestOf(FAR));
    Run const blocked = Plan(world, graph, stuck, true, ROUTE_SEARCHES_PER_JOB);
    std::vector<CircuitLog::Row> const rows = CircuitLog::PlanRows(blocked.plan.routeGaps);
    Add(checks, "reach 3: the only fitting zone behind no link is no option, counted no_route; other goals stay",
        blocked.filled.hubs.empty() && blocked.plan.ranked.ok && HasReason(blocked.plan.ranked, "delivery_other_map") &&
            HasRow(rows, "plan:no_route", "next_zone") && HasRow(rows, "plan:no_next_zone", "outlevelled"),
        "hubs " + Names(world, blocked.filled.hubs) + "; rows" + RowText(rows) + "; " +
            GoalHarness::Summary(blocked.plan.ranked));

    // 4) A complete quest whose ender is across the sea: the turn-in priced by its route; no cost, no option
    Snapshot carrier = BotAt(HOME, 15, 0);
    Carry(carrier, QuestOf(FAR));
    Run const sea = Plan(world, graph, carrier, true, ROUTE_SEARCHES_PER_JOB);
    Run const seaBare = Plan(world, graph, carrier, true, 0);
    Run const ashore = Plan(world, graph, carrier, false, ROUTE_SEARCHES_PER_JOB);
    bool const delivered =
        sea.deliveries.size() == 1 && sea.deliveries.front().otherMap &&
        RouteMinutes(sea, carrier, sea.deliveries.front().place, sea.deliveries.front().travelMinutes);
    Add(checks, "reach 4: a turn-in across the sea is priced by its ship; without a route or a cost it is no option",
        delivered && HasReason(sea.plan.ranked, "delivery_other_map") && seaBare.deliveries.empty() &&
            ashore.deliveries.empty() &&
            HasRow(CircuitLog::PlanRows(ashore.plan.routeGaps), "plan:no_route", "delivery"),
        std::to_string(sea.deliveries.size()) + " deliveries, " +
            (sea.deliveries.empty() ? std::string() : std::to_string(sea.deliveries.front().travelMinutes) + " min") +
            "; without a cost " + std::to_string(seaBare.deliveries.size()) + ", without transports " +
            std::to_string(ashore.deliveries.size()) + RowText(CircuitLog::PlanRows(ashore.plan.routeGaps)));

    // 5) A class quest's giver across the sea: a trip priced by its route; no cost, no trip
    Snapshot orc = BotAt(HOME, 12, 1);
    orc.rewardedQuests = {CLASS_PREV};
    Situation governed;
    governed.governed = true;
    governed.levelXp = CLASS_LEVEL_XP;
    CircuitContext context;
    context.quests = &world.quests;
    context.indexes = &world.indexes;
    orc.route = RouteCostHarness::RouteOf(orc, false, true);
    auto const cost = std::make_shared<RouteCost>(graph, orc.route, RouteCostInputs{});
    CircuitContext priced = context;
    priced.travel = RouteCost::Hook(cost);
    Situation trips = governed;
    FillClassQuests(orc, priced, Prepare(orc, trips), trips);
    Situation bareTrips = governed;
    CircuitContext bare = context;
    FillClassQuests(orc, bare, Prepare(orc, bareTrips), bareTrips);
    Run const orcPlan = Plan(world, graph, orc, true, ROUTE_SEARCHES_PER_JOB, governed);
    std::optional<float> const giverSeconds =
        trips.classTrips.empty() ? std::nullopt : cost->Answer(orc.position, trips.classTrips.front().place);
    Add(checks, "reach 5: a class quest's giver across the sea is a trip priced by its route; no cost, no trip",
        trips.classTrips.size() == 1 && trips.classTrips.front().place.map == FAR_MAP && giverSeconds &&
            *giverSeconds >= 0.0f &&
            std::abs(trips.classTrips.front().travelMinutes - *giverSeconds / SECONDS_PER_MINUTE) <= MINUTES_EPSILON &&
            bareTrips.classTrips.empty() &&
            HasReason(orcPlan.plan.ranked, std::string(ClassQuests::REASON_WORD) + "_other_map"),
        std::to_string(trips.classTrips.size()) + " trips, without a cost " +
            std::to_string(bareTrips.classTrips.size()) + "; " + GoalHarness::Summary(orcPlan.plan.ranked));

    // 6) The enemy's zone fits by level: never offered, neither as a next zone nor as the bracket move
    Run const dry = Plan(world, graph, BotAt(HOME, 19, 0), true, ROUTE_SEARCHES_PER_JOB);
    Run const horde = Plan(world, graph, BotAt(HOME, 22, 1), true, ROUTE_SEARCHES_PER_JOB);
    CircuitMove const& move = dry.plan.circuit.moveOn;
    Add(checks, "reach 6: the enemy's zone that fits by level is never offered; to its own side it is",
        !HubOf(both, world, ENEMY) && move.reason == MoveReason::Bracket && move.zone == Z_NEAR &&
            HubOf(horde, world, ENEMY) && !HubOf(horde, world, NEAR) && !HubOf(horde, world, FAR),
        "alliance hubs " + Names(world, both.filled.hubs) + ", bracket move zone " + std::to_string(move.zone) +
            "; horde hubs " + Names(world, horde.filled.hubs));

    // The bind across the sea: the way back is the route's; without a cost nothing is saved
    Snapshot away = BotAt(HOME, 15, 0);
    away.hearth = {FAR_MAP, CampX(FAR), BOT_Y, 0.0f};
    Situation inn;
    inn.inns = {{701, {HOME_MAP, CampX(HOME) + 5.0f, BOT_Y - 20.0f, 0.0f}, 21.0f, true, true, false}};
    Run const bind = Plan(world, graph, away, true, ROUTE_SEARCHES_PER_JOB, inn);
    Run const bindBare = Plan(world, graph, away, true, 0, inn);
    std::optional<float> const back = bind.cost->Answer(away.hearth, away.position);
    auto const hearth = [](Ranked const& ranked)
    {
        return std::any_of(ranked.options.begin(), ranked.options.end(),
                           [](Option const& option) { return option.kind == GoalKind::Hearth; });
    };
    Add(checks, "reach bind: a bind across the sea saves its route back; without a cost no hearth option",
        back && *back > 0.0f && hearth(bind.plan.ranked) && !hearth(bindBare.plan.ranked),
        "way back " + std::to_string(back ? *back : -1.0f) + " s; " + GoalHarness::Summary(bind.plan.ranked));

    // The rows of a plan's way gaps
    RouteGaps gaps;
    gaps.Note(RouteCaller::NextZone, TRAVEL_NO_ROUTE);
    gaps.Note(RouteCaller::NextZone, TRAVEL_NO_ROUTE);
    gaps.Note(RouteCaller::Delivery, TRAVEL_CAPPED);
    std::vector<CircuitLog::Row> const gapRows = CircuitLog::PlanRows(gaps);
    Add(checks, "reach rows: a plan's way gaps as plan:no_route and plan:capped rows per asker",
        gapRows.size() == 2 && gapRows[0].choice == "plan:no_route" && gapRows[0].reason == "next_zone" &&
            gapRows[0].value == 2.0f && gapRows[1].choice == "plan:capped" && gapRows[1].reason == "delivery" &&
            CircuitLog::PlanRows(RouteGaps{}).empty(),
        RowText(gapRows));
}

struct Tally
{
    std::uint32_t states{0};
    std::uint32_t zones{0};      // offered zones checked (hubs, bracket moves)
    std::uint32_t places{0};     // offered places checked for a route
    std::uint32_t otherMap{0};   // offered places on another map
    std::uint32_t enemyFits{0};  // states where the enemy's zone fits the bot's level
    std::uint32_t bracket{0};    // bracket moves
    std::vector<std::string> broken;
};

// The three invariants over one state
inline void SearchState(SeaWorld const& world, TravelGraph const& graph, Snapshot const& bot, bool transport,
                        std::uint32_t cap, Tally& tally)
{
    Run const run = Plan(world, graph, bot, transport, cap);
    ++tally.states;
    std::string const state = "at " + std::string(SEA_CAMPS[bot.zone - Z_HOME].name) + " level " +
                              std::to_string(bot.level) + (bot.team ? " horde" : " alliance") +
                              (bot.quests.empty() ? "" : " carrying " + std::to_string(bot.quests.front().quest)) +
                              (transport ? ", transports" : "") + ", cap " + std::to_string(cap);
    auto const broke = [&tally, &state](std::string const& what)
    { tally.broken.push_back(tally.broken.size() < SHOWN ? state + ": " + what : std::string()); };
    // Invariant 1: every offered zone's bracket fits and its side is not the enemy's
    auto const zoneFits = [&](std::uint32_t zone, char const* what)
    {
        ++tally.zones;
        ZoneRecord const* record = world.indexes.FindZone(zone);  // checked apart from ZoneOffered, its own words
        ZoneSide const enemy = bot.team ? ZoneSide::Alliance : ZoneSide::Horde;
        if (!record || !record->high || bot.level < record->low || bot.level > record->high || record->side == enemy)
            broke(std::string(what) + " to zone " + std::to_string(zone) + " outside its bracket or the enemy's");
    };
    if (ZoneRecord const* enemy = world.indexes.FindZone(Z_ENEMY);
        enemy && bot.level >= enemy->low && bot.level <= enemy->high)
        ++tally.enemyFits;
    for (HubOption const& hub : run.filled.hubs)
        zoneFits(hub.zone, "next-zone hub");
    CircuitMove const& move = run.plan.circuit.moveOn;
    if (move.reason == MoveReason::Bracket)
    {
        ++tally.bracket;
        zoneFits(move.zone, "bracket move");
    }
    // Invariants 2 and 3: an offered place has a route; across maps its minutes are the route's, and without a cost
    // no place across maps is offered
    auto const routed = [&](Place const& place, std::optional<float> minutes, char const* what)
    {
        ++tally.places;
        bool const away = place.map != bot.position.map;
        tally.otherMap += away ? 1 : 0;
        if (!run.cost)
        {
            if (away)
                broke(std::string(what) + " on map " + std::to_string(place.map) + " offered without a cost");
            return;
        }
        std::optional<float> const seconds = run.cost->Answer(bot.position, place);
        if (seconds && *seconds < 0.0f)
            broke(std::string(what) + " offered without a route");
        else if (away && !seconds)
            broke(std::string(what) + " on another map offered unpriced");
        else if (away && minutes && std::abs(*minutes - *seconds / SECONDS_PER_MINUTE) > MINUTES_EPSILON)
            broke(std::string(what) + " " + std::to_string(*minutes) + " min, its route " + std::to_string(*seconds) +
                  " s");
    };
    for (HubOption const& hub : run.filled.hubs)
        routed(hub.place, hub.travelMinutes, "next-zone hub");
    for (Delivery const& delivery : run.deliveries)
        routed(delivery.place, delivery.travelMinutes, "delivery");
    if (move.camp != NO_CAMP)
        routed(move.place, move.seconds / SECONDS_PER_MINUTE, "circuit move");
    for (Option const& option : run.plan.ranked.options)
        if (option.kind != GoalKind::Hearth && option.kind != GoalKind::Rest)
            routed(option.place, std::nullopt, (std::string(KindWord(option.kind)) + " " + option.reason).c_str());
}

inline void Invariants(std::vector<Check>& checks, SeaWorld const& world, TravelGraph const& graph)
{
    std::vector<std::uint32_t> const carried = {0, QuestOf(NEAR), QuestOf(BLOCKED), QuestOf(FAR), QuestOf(ISLAND)};
    Tally tally;
    for (std::size_t const start : {HOME, FAR, ISLAND})
        for (std::uint8_t level = FIRST_LEVEL; level <= LAST_LEVEL; ++level)
            for (std::uint8_t const team : {0, 1})
                for (std::uint32_t const quest : carried)
                    for (bool const transport : {false, true})
                        for (std::uint32_t const cap : {0u, ROUTE_SEARCHES_PER_JOB, SMALL_CAP})
                        {
                            Snapshot bot = BotAt(start, level, team);
                            if (quest)
                                Carry(bot, quest);
                            SearchState(world, graph, bot, transport, cap, tally);
                        }
    std::string text = std::to_string(tally.states) + " states: " + std::to_string(tally.zones) + " offered zones, " +
                       std::to_string(tally.places) + " offered places (" + std::to_string(tally.otherMap) +
                       " on another map), " + std::to_string(tally.bracket) + " bracket moves, " +
                       std::to_string(tally.enemyFits) + " states where the enemy's zone fits";
    for (std::string const& example : tally.broken)
        if (!example.empty())
            text += "; " + example;
    if (tally.broken.size() > SHOWN)
        text += "; " + std::to_string(tally.broken.size()) + " counterexamples in all";
    Add(checks,
        "reach invariants: offered zones fit bracket and side, offered places have a route, no minutes across maps "
        "from "
        "yards",
        tally.broken.empty() && tally.zones && tally.otherMap && tally.bracket && tally.enemyFits, text);
}

inline std::vector<Check> ReachChecks()
{
    std::vector<Check> checks;
    SeaWorld const world = Build();
    TravelGraph graph;
    std::string error;
    bool const built = BuildGraph(graph, error);
    Add(checks, "reach: the sea world's graph builds", built, error);
    if (!built)
        return checks;
    Cases(checks, world, graph);
    Invariants(checks, world, graph);
    return checks;
}
}  // namespace ReachHarness
}  // namespace Planner

#endif
