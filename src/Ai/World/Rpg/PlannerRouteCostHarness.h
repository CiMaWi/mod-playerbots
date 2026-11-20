/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERROUTECOSTHARNESS_H
#define _PLAYERBOT_PLANNERROUTECOSTHARNESS_H

#include <algorithm>
#include <chrono>
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
#include "TravelGraph.h"

// Offline proof of the planner's route cost (planning WN1a, PlannerRouteCost.h) on the next-zone world
// (GoalModelHarness.h NextZones) with a fixture graph where the line and the route disagree: Redridge's hub lies
// across water, reached only by a ship from a dock at home; no road reaches Westfall's first hub; a flight runs from
// home to Stonetalon's second hub. Two invariants are searched for a counterexample over a grid of states (levels, a
// carried quest per camp, the flight known, transports, the cap): no offered option's place lacks a route, and a
// priced travel's minutes are its route's seconds over 60. tests/goal-model-selftest.cpp runs it
namespace Planner
{
namespace RouteCostHarness
{
using namespace Goals;
using GoalHarness::Add;
using GoalHarness::Check;
using GoalHarness::NEXT_ZONE_CAMPS;

constexpr std::uint32_t SHIP = 20808;  // the fixture's vessel entry
constexpr std::uint32_t SHIP_PERIOD_MS = 600000;
constexpr float SHIP_RIDE_SECONDS = 120.0f;
constexpr std::uint32_t HOME_TAXI = 1;  // taxi node ids of the fixture's flight
constexpr std::uint32_t S2_TAXI = 2;
constexpr std::uint32_t TAXI_PATH = 5;
constexpr std::uint32_t FARE = 100;  // copper
constexpr float CLUSTER_YARDS = 15.0f;
constexpr float SIDE_YARDS = 40.0f;           // a dock and a flight master stand this far off their camp's centre
constexpr std::uint32_t NODES_PER_CAMP = 10;  // node id: camp x 10 + 1 centre, 2-5 around it, 6 dock, 7 flight master
constexpr std::uint32_t CENTRE = 1;
constexpr std::uint32_t DOCK = 6;
constexpr std::uint32_t FLIGHT_MASTER = 7;
constexpr std::uint32_t SMALL_CAP = 2;
constexpr float MINUTES_EPSILON = 0.001f;
constexpr std::uint8_t FIRST_LEVEL = 8;
constexpr std::uint8_t LAST_LEVEL = 20;
constexpr std::uint8_t OUTLEVELLED = 16;

inline float CampX(std::size_t camp) { return GoalHarness::BOT_X + NEXT_ZONE_CAMPS[camp].east; }

inline std::uint32_t NodeId(std::size_t camp, std::uint32_t slot)
{
    return static_cast<std::uint32_t>(camp) * NODES_PER_CAMP + slot;
}

// The road: every camp on the bot's map a cluster of five nodes, the clusters joined west to east, except Westfall's
// first (no link at all) and Redridge's (its dock's ship to the home dock only); Darkshore has no node
inline bool BuildGraph(TravelGraph& graph, std::string& error)
{
    using GoalHarness::BOT_Y;
    using GoalHarness::EASTERN;
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
    float const side = SIDE_YARDS * std::sqrt(2.0f);
    std::size_t previous = std::size(NEXT_ZONE_CAMPS);
    for (std::size_t camp = 0; camp < std::size(NEXT_ZONE_CAMPS); ++camp)
    {
        if (NEXT_ZONE_CAMPS[camp].map != EASTERN)
            continue;
        float const x = CampX(camp);
        std::string const name = NEXT_ZONE_CAMPS[camp].name;
        builder.AddNode(NodeId(camp, CENTRE), name, EASTERN, x, BOT_Y, 0.0f);
        std::uint32_t slot = CENTRE;
        for (auto const& [dx, dy] : {std::pair{CLUSTER_YARDS, 0.0f}, std::pair{-CLUSTER_YARDS, 0.0f},
                                     std::pair{0.0f, CLUSTER_YARDS}, std::pair{0.0f, -CLUSTER_YARDS}})
        {
            builder.AddNode(NodeId(camp, ++slot), name, EASTERN, x + dx, BOT_Y + dy, 0.0f);
            link(NodeId(camp, CENTRE), NodeId(camp, slot), CLUSTER_YARDS, 1, 0);
        }
        if (camp == GoalHarness::W1_CAMP || camp == GoalHarness::R_CAMP)
            continue;
        if (previous < std::size(NEXT_ZONE_CAMPS))
            link(NodeId(previous, CENTRE), NodeId(camp, CENTRE), x - CampX(previous), 1, 0);
        previous = camp;
    }
    // The ship: a dock south-west of home and one of Redridge, with their crossing rows (no crossing, no ride)
    for (std::size_t const camp : {GoalHarness::HOME_CAMP, GoalHarness::R_CAMP})
    {
        float const x = CampX(camp) - SIDE_YARDS;
        builder.AddNode(NodeId(camp, DOCK), "dock", EASTERN, x, BOT_Y - SIDE_YARDS, 0.0f);
        link(NodeId(camp, CENTRE), NodeId(camp, DOCK), side, 1, 0);
        for (std::uint32_t direction = 0; direction < 2; ++direction)
        {
            TravelGraphBuilder::CrossingRow row;
            row.entry = SHIP;
            row.stop = camp == GoalHarness::HOME_CAMP ? 0 : 1;
            row.mapId = EASTERN;
            row.x = x;
            row.y = BOT_Y - SIDE_YARDS;
            row.direction = direction;
            builder.AddCrossing(row);
        }
    }
    link(NodeId(GoalHarness::HOME_CAMP, DOCK), NodeId(GoalHarness::R_CAMP, DOCK),
         CampX(GoalHarness::R_CAMP) - CampX(GoalHarness::HOME_CAMP), 3, SHIP);
    // The flight: flight masters north-east of home and of Stonetalon's second camp, one way out
    for (std::size_t const camp : {GoalHarness::HOME_CAMP, GoalHarness::S2_CAMP})
    {
        builder.AddNode(NodeId(camp, FLIGHT_MASTER), "flight master", EASTERN, CampX(camp) + SIDE_YARDS,
                        BOT_Y + SIDE_YARDS, 0.0f);
        link(NodeId(camp, CENTRE), NodeId(camp, FLIGHT_MASTER), side, 1, 0);
    }
    builder.AddTaxiNode(HOME_TAXI, true, false);
    builder.AddTaxiNode(S2_TAXI, true, false);
    builder.AddTaxiPath(TAXI_PATH, HOME_TAXI, S2_TAXI, FARE);
    TravelGraphBuilder::LinkRow flight;
    flight.fromId = NodeId(GoalHarness::HOME_CAMP, FLIGHT_MASTER);
    flight.toId = NodeId(GoalHarness::S2_CAMP, FLIGHT_MASTER);
    flight.type = 4;
    flight.object = TAXI_PATH;
    builder.AddLink(flight);
    if (!builder.EndLinks(error))
        return false;
    std::uint32_t nr = 0;
    for (std::size_t const camp : {GoalHarness::HOME_CAMP, GoalHarness::S2_CAMP})
        builder.AddPoint(flight.fromId, flight.toId, nr++, EASTERN, CampX(camp) + SIDE_YARDS, BOT_Y + SIDE_YARDS, 0.0f);
    if (!builder.Finish(graph, error))
        return false;
    graph.Publish(0, 0);
    return true;
}

// The route view a trip of the bot would send (PlannerService FillSnapshot): its place, purse and speed, transports
// as asked, and the flight's far end known or not
inline TravelRoute::Snapshot RouteOf(Snapshot const& bot, bool knowsFlight, bool transport)
{
    TravelRoute::Snapshot route;
    route.position = RoutePoint(bot.position);
    route.team = bot.team;
    route.level = bot.level;
    route.money = bot.money;
    route.runSpeed = bot.runSpeed;
    route.allowTransport = transport;
    if (knowsFlight)
        route.LearnTaxiNode(S2_TAXI);
    return route;
}

inline Place HubPlace(GoalHarness::NextZoneWorld const& world, std::size_t camp)
{
    Hub const& hub = world.indexes.hubs[world.hubs[camp]];
    return {hub.mapId, hub.x, hub.y, hub.z};
}

inline HubOption const* HubOf(std::vector<HubOption> const& hubs, std::uint32_t hub)
{
    for (HubOption const& option : hubs)
        if (option.hub == hub)
            return &option;
    return nullptr;
}

inline bool HasLeg(TravelRoute::Result const& route, TravelRoute::LegType type)
{
    return std::any_of(route.legs.begin(), route.legs.end(),
                       [type](TravelRoute::Leg const& leg) { return leg.type == type; });
}

inline bool SameMinutes(float minutes, float seconds)
{
    return std::abs(minutes - seconds / SECONDS_PER_MINUTE) <= MINUTES_EPSILON;
}

// The bot at home past Elwynn's bracket, its next-zone hubs priced by a fresh cost
struct Priced
{
    std::shared_ptr<RouteCost> cost;
    std::vector<HubOption> hubs;
};

inline Priced PriceHubs(GoalHarness::NextZoneWorld const& world, TravelGraph const& graph, Snapshot bot,
                        bool knowsFlight, bool transport)
{
    bot.route = RouteOf(bot, knowsFlight, transport);
    Priced priced{std::make_shared<RouteCost>(graph, bot.route, RouteCostInputs{}), {}};
    CircuitContext context;
    context.quests = &world.quests;
    context.indexes = &world.indexes;
    context.travel = RouteCost::Hook(priced.cost);
    Situation situation;
    situation.here.hub = NO_HUB;
    GoalHarness::Filled(bot, context, situation);
    priced.hubs = situation.hubs;
    return priced;
}

inline TravelRoute::Result RouteTo(TravelGraph const& graph, Snapshot const& bot, Place const& to, bool knowsFlight,
                                   bool transport)
{
    TravelRoute::Request const request(RouteOf(bot, knowsFlight, transport), RoutePoint(to));
    return TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, TravelRoute::Params{});
}

// The three fixture cases and the memo and cap
inline void Cases(std::vector<Check>& checks, GoalHarness::NextZoneWorld const& world, TravelGraph const& graph)
{
    using GoalHarness::CampNames;
    Snapshot const bot = GoalHarness::Bot(OUTLEVELLED, GoalHarness::FLUSH);
    std::uint32_t const w1 = world.hubs[GoalHarness::W1_CAMP];
    std::uint32_t const w2 = world.hubs[GoalHarness::W2_CAMP];
    std::uint32_t const redridge = world.hubs[GoalHarness::R_CAMP];
    std::uint32_t const stonetalon = world.hubs[GoalHarness::S2_CAMP];
    auto const names = [&world](std::vector<HubOption> const& hubs)
    {
        std::vector<std::uint32_t> ids;
        for (HubOption const& hub : hubs)
            ids.push_back(hub.hub);
        return CampNames(world, ids);
    };

    // a) Across water: the ship's route is the price, not the line
    Priced const ship = PriceHubs(world, graph, bot, false, true);
    Priced const ashore = PriceHubs(world, graph, bot, false, false);
    Place const redridgePlace = HubPlace(world, GoalHarness::R_CAMP);
    TravelRoute::Result const crossing = RouteTo(graph, bot, redridgePlace, false, true);
    HubOption const* byShip = HubOf(ship.hubs, redridge);
    float const lineMinutes = WorldTravel::WalkMinutes(
        std::hypot(redridgePlace.x - bot.position.x, redridgePlace.y - bot.position.y), bot.runSpeed);
    Add(checks, "route cost a: a hub across water is priced by its ship, not by the line",
        crossing.ok && HasLeg(crossing, TravelRoute::LegType::Transport) && byShip &&
            SameMinutes(byShip->travelMinutes, crossing.totalSeconds) && byShip->travelMinutes > lineMinutes &&
            !HubOf(ashore.hubs, redridge),
        "hubs " + names(ship.hubs) + "; Redridge " + std::to_string(byShip ? byShip->travelMinutes : -1.0f) +
            " min, line " + std::to_string(lineMinutes) + " min; without transports " + names(ashore.hubs));

    // b) A nearer hub without a way there is left out; a farther one of its zone with a route stands for it
    std::vector<std::uint32_t> const expected = {redridge, world.hubs[GoalHarness::L_CAMP], w2, stonetalon};
    std::vector<std::uint32_t> got;
    for (HubOption const& hub : ship.hubs)
        got.push_back(hub.hub);
    std::optional<float> const toW1 = ship.cost->Answer(bot.position, HubPlace(world, GoalHarness::W1_CAMP));
    Add(checks, "route cost b: a nearer hub with no route is not offered, a farther one with a route is",
        got == expected && !HubOf(ship.hubs, w1) && toW1 && *toW1 < 0.0f && ship.cost->Counts().noRoute >= 1,
        "hubs " + names(ship.hubs) + ", expected " + CampNames(world, expected));

    // c) A flight the bot knows against one it does not
    Priced const flown = PriceHubs(world, graph, bot, true, true);
    Place const stonetalonPlace = HubPlace(world, GoalHarness::S2_CAMP);
    TravelRoute::Result const flight = RouteTo(graph, bot, stonetalonPlace, true, true);
    TravelRoute::Result const road = RouteTo(graph, bot, stonetalonPlace, false, true);
    HubOption const* known = HubOf(flown.hubs, stonetalon);
    HubOption const* unknown = HubOf(ship.hubs, stonetalon);
    Add(checks, "route cost c: a known flight shortens the way, an unknown one is walked",
        known && unknown && flight.ok && road.ok && HasLeg(flight, TravelRoute::LegType::Flight) &&
            !HasLeg(road, TravelRoute::LegType::Flight) && SameMinutes(known->travelMinutes, flight.totalSeconds) &&
            SameMinutes(unknown->travelMinutes, road.totalSeconds) && known->travelMinutes < unknown->travelMinutes,
        "Stonetalon known " + std::to_string(known ? known->travelMinutes : -1.0f) + " min, unknown " +
            std::to_string(unknown ? unknown->travelMinutes : -1.0f) + " min");

    // d) The memo and the cap: a way asked twice is searched once; past the cap a way is left out, never by yards
    RouteCost cost(graph, RouteOf(bot, false, true), RouteCostInputs{}, 1);
    Place const westfall = HubPlace(world, GoalHarness::W2_CAMP);
    float const first = cost.Seconds(bot.position, westfall);
    float const again = cost.Seconds(bot.position, westfall);
    float const capped = cost.Seconds(bot.position, stonetalonPlace);
    float const cappedAgain = cost.Seconds(bot.position, stonetalonPlace);
    RouteCostCounts const counts = cost.Counts();
    Add(checks, "route cost d: an answer is kept for the job, a place past the cap is left out",
        first > 0.0f && again == first && capped < 0.0f && cappedAgain < 0.0f && counts.asked == 4 &&
            counts.searches == 1 && counts.capped == 1 && counts.noRoute == 0,
        "asked " + std::to_string(counts.asked) + ", searches " + std::to_string(counts.searches) + ", capped " +
            std::to_string(counts.capped));

    // e) No graph yet: no hook, the callers' straight yards as before
    TravelGraph unready;
    Add(checks, "route cost e: before the graph is ready the hook is empty",
        !RouteCost::Hook(std::make_shared<RouteCost>(unready, RouteOf(bot, false, true), RouteCostInputs{})), "");
}

// What one state's search found
struct Tally
{
    std::uint32_t states{0};
    std::uint32_t options{0};  // offered options at a priced place
    std::uint32_t ways{0};     // priced travels compared with their route
    std::uint64_t searches{0};
    std::uint64_t capped{0};
    double micros{0.0};
    std::vector<std::string> broken;  // the first counterexamples
};

constexpr std::size_t COUNTEREXAMPLES_SHOWN = 4;

// The two invariants over one state: the plan, its next-zone hubs, deliveries and circuit move, all priced by one cost
inline void SearchState(GoalHarness::NextZoneWorld const& world, TravelGraph const& graph, Snapshot bot,
                        bool knowsFlight, bool transport, std::uint32_t cap, Tally& tally)
{
    bot.route = RouteOf(bot, knowsFlight, transport);
    auto const cost = std::make_shared<RouteCost>(graph, bot.route, RouteCostInputs{}, cap);
    CircuitContext context;
    context.quests = &world.quests;
    context.indexes = &world.indexes;
    context.travel = RouteCost::Hook(cost);
    Situation live;
    live.here.hub = NO_HUB;
    auto const started = std::chrono::steady_clock::now();
    GoalPlan const plan = PlanGoals(bot, context, GoalHarness::Reserved(live, bot.level));
    tally.micros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
    Situation filled = live;
    GoalHarness::Filled(bot, context, filled);
    std::vector<Delivery> const deliveries = Deliveries(bot, context, plan.circuit, bot.runSpeed);
    ++tally.states;
    std::string const state = "level " + std::to_string(bot.level) +
                              (bot.quests.empty() ? "" : " carrying " + std::to_string(bot.quests.front().quest)) +
                              (knowsFlight ? ", flight" : "") + (transport ? ", transports" : "") + ", cap " +
                              std::to_string(cap);
    auto const broke = [&tally, &state](std::string const& what)
    {
        if (tally.broken.size() < COUNTEREXAMPLES_SHOWN)
            tally.broken.push_back(state + ": " + what);
        else
            tally.broken.emplace_back();
    };
    // Invariant 2: a priced travel's minutes are its route's seconds over 60, and it has a route (invariant 1)
    auto const priced = [&](Place const& to, float minutes, char const* what)
    {
        ++tally.ways;
        std::optional<float> const seconds = cost->Answer(bot.position, to);
        if (!seconds || *seconds < 0.0f)
            broke(std::string(what) + " offered without a route");
        else if (!SameMinutes(minutes, *seconds))
            broke(std::string(what) + " " + std::to_string(minutes) + " min, its route " + std::to_string(*seconds) +
                  " s");
    };
    for (HubOption const& hub : filled.hubs)
        priced(hub.place, hub.travelMinutes, "next-zone hub");
    for (Delivery const& delivery : deliveries)
        priced(delivery.place, delivery.travelMinutes, "delivery");
    CircuitMove const& move = plan.circuit.moveOn;
    std::optional<float> const moveSeconds =
        move.camp != NO_CAMP ? cost->Answer(bot.position, move.place) : std::nullopt;
    bool const moveRouted = moveSeconds && *moveSeconds >= 0.0f;
    if (moveRouted && std::abs(move.seconds - *moveSeconds) > MINUTES_EPSILON)
        broke("circuit move " + std::to_string(move.seconds) + " s, its route " + std::to_string(*moveSeconds) + " s");
    // Invariant 1: no offered option stands at a place asked without a route
    for (Option const& option : plan.ranked.options)
    {
        std::optional<float> const seconds = cost->Answer(bot.position, option.place);
        bool const dryMove = option.reason.rfind("circuit_dry", 0) == 0 ||
                             (option.targetType != TargetType::Quest && option.reason == "delivery");
        if (seconds)
            ++tally.options;
        if (seconds && *seconds < 0.0f)
            broke(std::string(KindWord(option.kind)) + " " + option.reason + " offered at a place without a route");
        if (dryMove && moveSeconds && !moveRouted)
            broke("the circuit's move offered to a camp without a route");
    }
    tally.searches += cost->Counts().searches;
    tally.capped += cost->Counts().capped;
}

inline void Invariants(std::vector<Check>& checks, GoalHarness::NextZoneWorld const& world, TravelGraph const& graph)
{
    // A carried quest per camp: none, the island without a road, the one across water, a road, the flight, another map
    std::vector<std::uint32_t> carried = {0};
    for (std::size_t const camp : {GoalHarness::W1_CAMP, GoalHarness::R_CAMP, GoalHarness::L_CAMP, GoalHarness::S2_CAMP,
                                   std::size(NEXT_ZONE_CAMPS) - 1})
        carried.push_back(GoalHarness::NEXT_ZONE_QUESTS + 1 + 2 * static_cast<std::uint32_t>(camp));
    Tally tally;
    for (std::uint8_t level = FIRST_LEVEL; level <= LAST_LEVEL; ++level)
        for (std::uint32_t const quest : carried)
            for (bool const knowsFlight : {false, true})
                for (bool const transport : {false, true})
                    for (std::uint32_t const cap : {ROUTE_SEARCHES_PER_JOB, SMALL_CAP})
                    {
                        Snapshot bot = GoalHarness::Bot(level, GoalHarness::FLUSH);
                        if (quest)
                        {
                            QuestSlot slot;
                            slot.quest = quest;
                            slot.state = Circuit::SLOT_COMPLETE;
                            bot.quests.push_back(slot);
                        }
                        SearchState(world, graph, bot, knowsFlight, transport, cap, tally);
                    }
    std::string text = std::to_string(tally.states) + " states, " + std::to_string(tally.options) +
                       " offered options at priced places, " + std::to_string(tally.ways) + " priced travels, " +
                       std::to_string(tally.searches) + " searches, " + std::to_string(tally.capped) +
                       " capped; plan with its searches " +
                       std::to_string(tally.states ? tally.micros / tally.states : 0.0) + " us";
    for (std::string const& example : tally.broken)
        if (!example.empty())
            text += "; " + example;
    Add(checks, "route cost invariants: no offered option lacks a route, travel minutes are route seconds / 60",
        tally.broken.empty() && tally.states && tally.ways && tally.options && tally.capped,
        text + (tally.broken.size() > COUNTEREXAMPLES_SHOWN
                    ? "; " + std::to_string(tally.broken.size()) + " counterexamples in all"
                    : ""));
}

inline std::vector<Check> RouteCostChecks()
{
    std::vector<Check> checks;
    GoalHarness::NextZoneWorld const world = GoalHarness::NextZones();
    TravelGraph graph;
    std::string error;
    bool const built = BuildGraph(graph, error);
    Add(checks, "route cost: the fixture graph builds", built, error);
    if (!built)
        return checks;
    Cases(checks, world, graph);
    Invariants(checks, world, graph);
    return checks;
}
}  // namespace RouteCostHarness
}  // namespace Planner

#endif
