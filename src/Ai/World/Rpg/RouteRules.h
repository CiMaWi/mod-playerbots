/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ROUTERULES_H
#define _PLAYERBOT_ROUTERULES_H

// Route search for one bot over the travel graph (TravelGraph.h), costed in seconds. Player-free on purpose: the bot is
// a plain-data Snapshot filled on its map thread, the graph is immutable once ready and the stuck edges are an
// immutable StuckEdges snapshot, so Search() is a pure function any thread may run. tests/route-rules-selftest.cpp and
// `playerbots route selftest` run the same Selftest().
//
// Costs (seconds):
// - walk: run part / run speed + swum part / swim speed, times the legacy annoyance of mobs (+10% per level they are
//   more than 10 above the bot) and of the other faction's guards and NPCs (+30% per level); never removed
// - portal (area trigger): its stored 3 s
// - flight: path length / flight speed + 5 s boarding + the fare weighed by the purse (FareSeconds). Only when the
//   bot's team has a flight master at both ends, the fare is affordable and the destination node is known. The origin
//   node need not be known: a bot reaching a flight master learns its node there (NewRpgTravelFlightAction does), and
//   a flight chain's later origins are earlier destinations, known already
// - transport (only with allowTransport; WT19): elevators their stored seconds, ships and zeppelins the expected wait
//   (half the vessel's period, read from the core when the graph loads) plus its ride
//   (TravelGraphLink::VesselSeconds); a wait point's link to its stop (no object)
//   its walk. A stop with a wait point is reached and left only through the wait point and its rides
//   (TravelGraph::OffWaitStop), so a crossing runs wait point, stop, ride, stop, wait point
// - static portal (a portal object a player clicks, WT55d): its stored 3 s, as an area trigger
// - hearthstone: 10 s cast plus the remaining cooldown, from the bot's position to its bind point. Only from the start:
//   using it later arrives no earlier (max(cooldown, t) + 10 s is never less than cooldown + 10 s)
//   A cooldown above HEARTH_WAIT_CAP_MS (3 min) is no edge: the follower never waits that long
// - an edge flagged by Stuck events costs Params::stuckPenalty times more, and is skipped after STUCK_SKIP_FLAGS
// - the request's avoided nodes are no start or goal node, and noDirect drops the straight walk from the bot (the
//   bot's own stuck walks of the trip: their start or goal hop is no graph edge to flag); its avoided edges (the
//   trip's walks where the bot died twice) are not used
// - fly (own flying mount, WU10, WT61b): for a bot that may fly on the goal's map and no noFly request, edges from
//   every vertex there that may take off to the goal and to the nodes nearest it that may land (FLY_EDGES_PER_NODE at
//   most), their straight yards at the bot's air speed plus FLY_OVERHEAD_SECONDS. With flight maps a takeoff or a
//   landing is a fly end (FlyEnd) or a platform's takeoff spot a straight walk off (PlatformSpot); the bot's own spot
//   takes off only with Snapshot::takeoffHere, and no spot the trip could not take off at does
//   (Request::noTakeoffs); a hop under the ground hop is no edge
// - air (graph link type 7, WT61): only for a bot that may fly on its map and no noFly request, its straight yards at
//   the bot's air speed plus FLY_OVERHEAD_SECONDS; never from a spot that takes off no fly edge
// The fly legs the search chose are then flown (Search, one FlightRules::Search each); a failed flight closes its air
// link or its fly edge's ends and the search runs again, at most FLY_TRIES times, then once with flying closed

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "FlightRules.h"
#include "RouteTypes.h"
#include "TravelGraph.h"

namespace TravelRoute
{
constexpr std::size_t TAXI_MASK_SIZE = 14;    // TaxiMaskSize (DBCStructure.h): 448 taxi nodes, PlayerTaxi's layout
constexpr std::size_t END_NODES = 5;          // graph nodes the bot, the goal and the bind point connect to
constexpr float DIRECT_YARDS = 100.0f;        // start and goal this close on one map may also be walked directly
constexpr float SAME_WALK_YARDS = 10.0f;      // a walk off the graph with both ends this close to a refused one's
constexpr float HEARTH_CAST_SECONDS = 10.0f;  // the hearthstone's cast
constexpr std::uint32_t HEARTH_WAIT_CAP_MS = 3 * 60 * 1000;  // a cooldown longer than this is no hearth edge
constexpr float FLIGHT_BOARDING_SECONDS = 5.0f;              // talking to the flight master, mounting and landing
constexpr float FULL_PURSE_SECONDS = 3600.0f;  // at thrift 100, a fare of all one's money weighs an hour of travel
// A route answer's life from its request: the service keeps it, the follower waits for it and applies it this long
constexpr std::uint32_t ANSWER_KEEP_MS = 60 * 1000;
constexpr std::uint16_t STUCK_SKIP_FLAGS = 3;  // an edge flagged this often within the avoid window is skipped
constexpr float STUCK_NODE_YARDS = 10.0f;      // a stuck walk's destination this close to a node was a graph leg
constexpr std::size_t STUCK_QUEUE_MAX = 4096;  // reports waiting for the world thread; more are dropped
constexpr std::uint32_t NO_LINK = std::numeric_limits<std::uint32_t>::max();
constexpr float UNREACHED = std::numeric_limits<float>::infinity();
constexpr std::uint32_t FLY_MAP_OUTLAND = 530;  // the two maps 3.3.5 lets players fly on
constexpr std::uint32_t FLY_MAP_NORTHREND = 571;
constexpr float FLY_OVERHEAD_SECONDS = 10.0f;  // a fly leg's mount cast, steep climb and descent, and dismount
constexpr std::size_t FLY_EDGES_PER_NODE = END_NODES + 1;  // a vertex's fly edges: to the goal and its landing nodes
constexpr std::size_t FLY_LANDING_SCAN = 4 * END_NODES;    // nodes nearest the goal looked at for landing nodes
constexpr std::uint32_t FLY_TRIES = 3;  // searches per route that may fly while flights fail (Search); then one walks
constexpr float FLY_OWN_SPOT_YARDS = 10.0f;  // route points this close to the bot are its own spot (a walk ends
                                             // within 5 yd of its node)
constexpr float FLY_PLATFORM_YARDS = FlightRules::PLATFORM_YARDS;  // this far above its cell's ground: no fly end
constexpr float FLY_TAKEOFF_WALK_YARDS = 500.0f;  // a platform point walks at most this far to a takeoff spot

inline float Distance(Point const& a, Point const& b)
{
    float const dx = a.x - b.x;
    float const dy = a.y - b.y;
    float const dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A walk off the graph (start to goal, to or from an end's nearest nodes, bind point to goal) is proposed by distance
// alone and priced as a run over it: the search has no navmesh. The bot's map thread probes such a walk on the navmesh
// when it starts it (TravelWalk::ProbePiece, WT56); a refused one fails its leg at once, and the replan leaves it out
inline float WalkSeconds(Point const& a, Point const& b, float runSpeed) { return Distance(a, b) / runSpeed; }

// What the search knows of the bot: plain data, filled on the bot's map thread (WT7b's FillSnapshot)
struct Snapshot
{
    std::uint32_t requestId{0};
    std::uint32_t takenMs{0};  // getMSTime() when filled: a result is applied only while young
    Point position;
    std::uint8_t team{0};  // TeamId: 0 Alliance, 1 Horde
    std::uint8_t level{1};
    std::uint32_t money{0};  // copper
    // PlayerTaxi's mask: taxi node n is bit (n - 1) % 32 of word (n - 1) / 32
    std::array<std::uint32_t, TAXI_MASK_SIZE> knownTaxi{};
    bool hasHearthstone{false};         // no hearthstone item, no hearth edge
    Point hearth;                       // the bind point
    std::uint32_t hearthCooldownMs{0};  // 0 = ready
    float runSpeed{7.0f};               // yd/s, the mount's speed folded in when mounted
    float swimSpeed{4.72f};
    bool mounted{false};
    std::uint32_t thrift{50};    // 0-100: how much a coin weighs against time
    bool allowTransport{false};  // ships, zeppelins and elevators (WT19)
    // Own flying (WU10): may the bot fly on Outland, on Northrend now (riding, a flying mount, Cold Weather Flying, no
    // battle in Wintergrasp), its speed in the air (0: no flying) and its style (FlightRules::BotFlightStyle)
    bool flyOutland{false};
    bool flyNorthrend{false};
    float flySpeed{0.0f};  // yd/s
    FlightRules::FlightStyle flightStyle;
    // The bot may mount a flyer and lift off where it stands: outdoors, not in water, in a flying area by the bot's
    // real area (WMO areas count: Dalaran's no-fly areas lie over flyable terrain, which the flight map holds open).
    // When not, no fly edge or air link starts at the bot's own spot
    bool takeoffHere{true};

    bool MayFlyOn(std::uint32_t mapId) const
    {
        return flySpeed > 0.0f &&
               ((mapId == FLY_MAP_OUTLAND && flyOutland) || (mapId == FLY_MAP_NORTHREND && flyNorthrend));
    }

    bool KnowsTaxiNode(std::uint32_t node) const
    {
        if (!node || node > TAXI_MASK_SIZE * 32)
            return false;
        return (knownTaxi[(node - 1) / 32] >> ((node - 1) % 32)) & 1u;
    }

    void LearnTaxiNode(std::uint32_t node)
    {
        if (node && node <= TAXI_MASK_SIZE * 32)
            knownTaxi[(node - 1) / 32] |= 1u << ((node - 1) % 32);
    }
};

struct Request
{
    Request() = default;
    Request(Snapshot const& bot, Point const& to) : snapshot(bot), goal(to) {}

    Snapshot snapshot;
    Point goal;
    // After stuck walks of the trip (TravelLegs::NoteStuck): nodes the route neither starts nor ends at, and no
    // straight walk from the bot to the goal
    std::vector<std::uint32_t> avoidNodes;
    bool noDirect{false};
    // After deaths on walks of the trip (TravelLegs::NoteDeath): graph edges (node indices, either way) not used
    std::vector<std::pair<std::uint32_t, std::uint32_t>> avoidEdges;
    // After walks off the graph the navmesh refused (TravelLegs::NoteWalkRefused, WT56): their start and end points;
    // no walk off the graph with both ends within SAME_WALK_YARDS of a pair's, in its direction
    std::vector<std::pair<Point, Point>> avoidWalks;
    // After a failed fly leg of the trip (TravelLegs::AfterFailure): no fly edges and no air links
    bool noFly{false};
    // A test's freeride (TestConstraints::RidesFree): every ride of a transport link costs no time, so the ride between
    // two stops is always the cheapest way. Its leg keeps the ride's own seconds
    bool freeRides{false};
    // The spots where the trip's bot could not take off (TravelLegs::NextRequest): no fly edge and no air link starts
    // within FLY_OWN_SPOT_YARDS of one. The snapshot closes only the spot the bot stands on now: without these the
    // replan after the next refusal took off at the spot before again (lab10: a cave's two nodes in turn, WT61b)
    std::vector<Point> noTakeoffs;

    bool AvoidsEdge(std::uint32_t from, std::uint32_t to) const
    {
        for (auto const& [a, b] : avoidEdges)
            if ((a == from && b == to) || (a == to && b == from))
                return true;
        return false;
    }
    bool AvoidsWalk(Point const& from, Point const& to) const
    {
        for (auto const& [a, b] : avoidWalks)
            if (a.mapId == from.mapId && Distance(a, from) <= SAME_WALK_YARDS && Distance(b, to) <= SAME_WALK_YARDS)
                return true;
        return false;
    }
    bool NoTakeoffAt(Point const& point) const
    {
        for (Point const& spot : noTakeoffs)
            if (spot.mapId == point.mapId && Distance(spot, point) <= FLY_OWN_SPOT_YARDS)
                return true;
        return false;
    }
};

// Tuning values (WorldTravelConfig) and the selftest's switch
struct Params
{
    float stuckPenalty{5.0f};  // Playerbots.WorldTravel.StuckPenalty
    float flightSpeed{TravelGraph::FLIGHT_SPEED};
    bool heuristic{true};  // false: plain Dijkstra over the same costs (the selftest's reference)
    // The published height maps (FlightMap.h's Planner::FlightMaps()); null: no fly legs
    std::shared_ptr<FlightRules::FlightMapSet const> flightMaps;
};

struct Result
{
    bool ok{false};
    Failure failure{Failure::None};
    // Consecutive walk links merge into one leg, consecutive flights into one, a crossing with its wait points' links
    // into one transport leg; a fly edge or an air link is a fly leg of its own
    std::vector<Leg> legs;
    float totalSeconds{0.0f};
    std::uint32_t expandedNodes{0};
    std::uint32_t flightSearches{0};  // the fly legs' FlightRules::Search calls and their time (Search)
    float flightMicros{0.0f};
};

// Copper the route's flights cost together: the search keeps it within the snapshot's money
inline std::uint32_t RouteFare(Result const& route)
{
    std::uint32_t total = 0;
    for (Leg const& leg : route.legs)
        total += leg.fareCopper;
    return total;
}

inline char const* LegTypeWord(LegType type)
{
    switch (type)
    {
        case LegType::Walk:
            return "walk";
        case LegType::Portal:
            return "portal";
        case LegType::Flight:
            return "flight";
        case LegType::Transport:
            return "transport";
        case LegType::Hearth:
            return "hearth";
        case LegType::Fly:
            return "fly";
        case LegType::StaticPortal:
            return "static_portal";
    }
    return "?";
}

inline char const* FailureWord(Failure failure)
{
    switch (failure)
    {
        case Failure::None:
            return "none";
        case Failure::GraphNotReady:
            return "graph not ready";
        case Failure::NoStartNode:
            return "no travel node on the start map";
        case Failure::NoGoalNode:
            return "no travel node on the goal map";
        case Failure::NoRoute:
            return "no route";
    }
    return "?";
}

inline std::uint64_t EdgeKey(std::uint32_t fromNode, std::uint32_t toNode)
{
    return (static_cast<std::uint64_t>(fromNode) << 32) | toNode;
}

// The edges flagged by Stuck events within the avoid window (node indices, EdgeKey) and how often; immutable once
// published by StuckBook
struct StuckEdges
{
    std::unordered_map<std::uint64_t, std::uint16_t> flags;
};

// Seconds a fare weighs: the share of the purse it takes, times an hour at thrift 100. Negative: unaffordable
inline float FareSeconds(std::uint32_t fare, std::uint32_t money, std::uint32_t thrift)
{
    if (!fare)
        return 0.0f;
    if (fare > money)
        return -1.0f;
    return static_cast<float>(fare) / static_cast<float>(money) * FULL_PURSE_SECONDS *
           static_cast<float>(std::min<std::uint32_t>(thrift, 100)) / 100.0f;
}

// Walk or fly (the time value): a flight is worth it when it saves more seconds than its fare weighs. The search weighs
// every flight link by the same FareSeconds, so this rule and the route's own choice agree (route-rules-selftest b)
inline bool FlightWorthIt(float walkSeconds, float flightSeconds, std::uint32_t fare, std::uint32_t money,
                          std::uint32_t thrift)
{
    float const weight = FareSeconds(fare, money, thrift);
    return weight >= 0.0f && walkSeconds - flightSeconds > weight;
}

// The legacy annoyance (TravelNodePath::getCost): maxCreature = {hostile mobs, Alliance-only friendly, Horde-only
// friendly}; the other faction's creatures annoy three times as much
inline float WalkModifier(TravelGraphLink const& link, Snapshot const& bot)
{
    int const level = bot.level;
    int const mobs = static_cast<int>(link.maxCreature[0]) - level - 10;
    int const enemies = static_cast<int>(link.maxCreature[bot.team == 0 ? 2 : 1]) - level - 10;
    float modifier = 1.0f;
    if (mobs > 0)
        modifier += 0.1f * static_cast<float>(mobs);
    if (enemies > 0)
        modifier += 0.3f * static_cast<float>(enemies);
    return modifier;
}

// A transport link's ride between two stops (a wait-point link is a walk to or from its stop)
inline bool IsRide(TravelGraphLink const& link)
{
    return link.type == TravelLinkType::Transport && !TravelGraph::IsWaitPointLink(link);
}

// The seconds of one graph link for this bot, before stuck flags; negative when the bot cannot use it. fare gets the
// copper a flight costs; a fare above the whole purse is refused here, Search also counts the fares paid before it
inline float LinkSeconds(TravelGraph const& graph, TravelGraphLink const& link, Snapshot const& bot,
                         Params const& params, float runSpeed, float swimSpeed, std::uint32_t& fare)
{
    fare = 0;
    switch (link.type)
    {
        case TravelLinkType::Walk:
        {
            float const swim = std::clamp(link.swim, 0.0f, link.distance);
            return ((link.distance - swim) / runSpeed + swim / swimSpeed) * WalkModifier(link, bot);
        }
        case TravelLinkType::Portal:
        case TravelLinkType::StaticPortal:
            return link.extraCost;
        case TravelLinkType::Transport:
            if (!bot.allowTransport)
                return -1.0f;
            if (TravelGraph::IsWaitPointLink(link))
                return link.distance / runSpeed;
            return link.extraCost >= 1.0f ? link.extraCost : link.VesselSeconds();
        case TravelLinkType::Flight:
        {
            TravelTaxiPath const* path = graph.TaxiPathOf(link);
            std::uint8_t const team = bot.team == 0 ? TravelGraph::TAXI_ALLIANCE : TravelGraph::TAXI_HORDE;
            if (!path || !(path->teams & team) || !bot.KnowsTaxiNode(path->to))
                return -1.0f;
            float const fareSeconds = FareSeconds(path->price, bot.money, bot.thrift);
            if (fareSeconds < 0.0f)
                return -1.0f;
            fare = path->price;
            return link.pathLength / params.flightSpeed + FLIGHT_BOARDING_SECONDS + fareSeconds;
        }
        case TravelLinkType::Air:
            // The straight yards are the flight's lower bound; Search prices the flight it plans
            if (!bot.MayFlyOn(graph.GetNode(link.from).mapId))
                return -1.0f;
            return link.distance / bot.flySpeed + FLY_OVERHEAD_SECONDS;
    }
    return -1.0f;
}

// A fly leg may start or end at the point: an open cell, and no more than FLY_PLATFORM_YARDS above the cell's ground
// (WU11b). The flight map knows the terrain's area per cell, blind to a WMO no-fly area over flyable ground: Dalaran
// floats some 450 yd above Crystalsong Forest's open cells, and a fly leg from its nodes failed its takeoff node by
// node
inline bool FlyEnd(FlightRules::HeightMap const& map, Point const& point)
{
    if (!map.IsOpen(point.x, point.y))
        return false;
    return point.z - static_cast<float>(map.Ground(map.CellX(point.x), map.CellY(point.y))) <= FLY_PLATFORM_YARDS;
}

// The nearest takeoff spot (HeightMap::takeoffs, Krasus' Landing) within FLY_TAKEOFF_WALK_YARDS of a point on a
// platform (an open cell, no FlyEnd); none for a point off the platforms or with no spot near
inline std::optional<Point> PlatformSpot(FlightRules::HeightMap const& map, Point const& point)
{
    if (!map.IsOpen(point.x, point.y) || FlyEnd(map, point))
        return std::nullopt;
    std::optional<Point> nearest;
    float nearestYards = FLY_TAKEOFF_WALK_YARDS;
    for (FlightRules::TakeoffSpot const& spot : map.takeoffs)
    {
        Point const at{point.mapId, spot.x, spot.y, spot.z};
        float const yards = Distance(point, at);
        if (yards <= nearestYards)
        {
            nearest = at;
            nearestYards = yards;
        }
    }
    return nearest;
}

// Fly edges closed after their flight failed (Search): no takeoff and no landing at these points
struct FlyClosures
{
    std::vector<Point> takeoffs;
    std::vector<Point> landings;
};

// A* over the graph plus three virtual vertices: the bot's position (linked to its END_NODES nearest nodes, the bind
// point and, when close, the goal), the goal (linked from its END_NODES nearest nodes) and the bind point. Straight
// virtual walks cost distance / run speed. The heuristic is a lower bound: on the goal's map the straight distance at
// the fastest speed, else (and capped by) the cheapest jump onto the goal's map (a portal, flight or transport link
// arriving there, plus the rest at the fastest speed). Reopening keeps the result optimal; the selftest compares it
// with Dijkstra (Params::heuristic false) on random pairs. The fare budget: every vertex carries the copper its best
// way paid for flights, and a flight link that would take that total above the purse is not relaxed, because the core
// charges a taxi chain's whole fare up front (Player::ActivateTaxiPathTo: money < totalcost fails) and every flight
// of a route draws on the same purse. The returned route's RouteFare is within the money; with one label per vertex a
// way that is slower but cheaper in copper can be missed when the purse is tight (then the route walks more).
// A bot that may fly has fly edges on the goal's map (the header's costs); their legs are not flown here (Search)
inline Result GraphSearch(TravelGraph const& graph, Request const& request, StuckEdges const& stuck,
                          Params const& params, FlyClosures const& closed)
{
    Result result;
    if (!graph.IsReady())
    {
        result.failure = Failure::GraphNotReady;
        return result;
    }

    Snapshot const& bot = request.snapshot;
    Point const& start = bot.position;
    Point const& goal = request.goal;
    std::vector<std::uint32_t> startNodes = graph.NearestNodes(start.mapId, start.x, start.y, start.z, END_NODES);
    std::vector<std::uint32_t> goalNodes = graph.NearestNodes(goal.mapId, goal.x, goal.y, goal.z, END_NODES);
    // A stop with a wait point is no end node: the bot walks to its wait point
    auto const avoided = [&](std::uint32_t node)
    {
        return graph.GetNode(node).waitStop ||
               std::find(request.avoidNodes.begin(), request.avoidNodes.end(), node) != request.avoidNodes.end();
    };
    std::erase_if(startNodes, avoided);
    std::erase_if(goalNodes, avoided);
    bool const direct = !request.noDirect && start.mapId == goal.mapId && Distance(start, goal) <= DIRECT_YARDS;
    if (startNodes.empty() && !direct)
    {
        result.failure = Failure::NoStartNode;
        return result;
    }
    if (goalNodes.empty() && !direct)
    {
        result.failure = Failure::NoGoalNode;
        return result;
    }

    std::vector<std::uint32_t> bindNodes;
    if (bot.hasHearthstone)
        bindNodes = graph.NearestNodes(bot.hearth.mapId, bot.hearth.x, bot.hearth.y, bot.hearth.z, END_NODES);
    std::erase_if(bindNodes, [&graph](std::uint32_t node) { return graph.GetNode(node).waitStop; });
    bool const hearth = bot.hasHearthstone && bot.hearthCooldownMs <= HEARTH_WAIT_CAP_MS &&
                        (!bindNodes.empty() || bot.hearth.mapId == goal.mapId);

    float const runSpeed = std::max(bot.runSpeed, 1.0f);
    float const swimSpeed = std::max(bot.swimSpeed, 1.0f);
    // Fly edges and air links fly at the bot's air speed: the bound must not be slower
    bool const mayFly = !request.noFly && (bot.flyOutland || bot.flyNorthrend);
    float const fastest = std::max({runSpeed, swimSpeed, params.flightSpeed, mayFly ? bot.flySpeed : 0.0f});
    // Fly edges on the goal's map; without flight maps every point may take off and land (the offline harnesses and
    // the map threads' checks: priced, never flown)
    FlightRules::FlightMapSet const* maps = params.flightMaps.get();
    FlightRules::HeightMap const* flyMap = maps ? maps->Find(goal.mapId) : nullptr;
    bool const flying = !request.noFly && bot.MayFlyOn(goal.mapId) && (!maps || flyMap);
    FlightRules::FlightParams const hop = maps ? maps->params : FlightRules::FlightParams{};
    // No takeoff at the point: the bot's own spot when it may not take off there, or a spot of the trip's before
    auto const noTakeoffAt = [&](Point const& point)
    {
        return (!bot.takeoffHere && point.mapId == start.mapId && Distance(point, start) <= FLY_OWN_SPOT_YARDS) ||
               request.NoTakeoffAt(point);
    };

    std::uint32_t const nodeCount = static_cast<std::uint32_t>(graph.NodeCount());
    std::uint32_t const source = nodeCount;
    std::uint32_t const target = nodeCount + 1;
    std::uint32_t const bind = nodeCount + 2;
    auto const pointOf = [&](std::uint32_t vertex) -> Point
    {
        if (vertex == source)
            return start;
        if (vertex == target)
            return goal;
        if (vertex == bind)
            return bot.hearth;
        TravelGraphNode const& node = graph.GetNode(vertex);
        return {node.mapId, node.x, node.y, node.z};
    };

    // The cheapest arrival by a jump onto the goal's map, a lower bound for any vertex off that map (the graph keeps
    // one arrival per node, so this reads a few dozen entries, not every link)
    float jump = UNREACHED;
    if (params.heuristic)
    {
        for (TravelGraphArrival const& arrival : graph.ArrivalsOnto(goal.mapId))
        {
            TravelGraphNode const& to = graph.GetNode(arrival.node);
            float const lowest =
                std::min(arrival.fixedSeconds, arrival.flightYards / params.flightSpeed + FLIGHT_BOARDING_SECONDS);
            jump = std::min(jump, lowest + Distance({to.mapId, to.x, to.y, to.z}, goal) / fastest);
        }
    }
    auto const estimate = [&](std::uint32_t vertex) -> float
    {
        if (!params.heuristic || request.freeRides)  // a free ride may cross a map at no time: no lower bound but 0
            return 0.0f;
        Point const at = pointOf(vertex);
        if (at.mapId != goal.mapId)
            return jump;
        return std::min(jump, Distance(at, goal) / fastest);
    };

    enum class Step : std::uint8_t
    {
        None,
        Link,
        Walk,
        Hearth,
        Fly
    };
    struct Via
    {
        std::uint32_t vertex{NO_LINK};
        std::uint32_t link{NO_LINK};
        Step step{Step::None};
    };
    struct Open
    {
        float f;
        float g;
        std::uint32_t vertex;
        bool operator>(Open const& other) const
        {
            return f != other.f ? f > other.f : vertex > other.vertex;  // ties by vertex: deterministic
        }
    };

    std::vector<float> best(nodeCount + 3, UNREACHED);
    std::vector<Via> via(nodeCount + 3);
    std::vector<std::uint32_t> spent(nodeCount + 3, 0);  // copper the best way to each vertex paid for flights
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    auto const relax =
        [&](std::uint32_t from, std::uint32_t to, float seconds, std::uint32_t link, Step step, std::uint32_t fare)
    {
        float const next = best[from] + seconds;
        if (next >= best[to])
            return;
        float const rest = estimate(to);
        if (rest == UNREACHED)
            return;  // off the goal's map with no portal, flight or transport onto it: the goal is out of reach
        best[to] = next;
        via[to] = {from, link, step};
        spent[to] = spent[from] + fare;
        open.push({next + rest, next, to});
    };
    auto const walkTo = [&](std::uint32_t from, std::uint32_t to)
    {
        Point const a = pointOf(from);
        Point const b = pointOf(to);
        if (!request.AvoidsWalk(a, b))
            relax(from, to, WalkSeconds(a, b, runSpeed), NO_LINK, Step::Walk, 0);
    };

    // A fly edge's end at a vertex: where the bot takes off or lands (the vertex's point, or a platform's takeoff spot)
    // and the walk between it and the vertex
    struct FlyPoint
    {
        std::uint32_t vertex;
        Point at;
        float walkSeconds;
    };
    auto const flyPointOf = [&](std::uint32_t vertex, bool takeoff) -> std::optional<FlyPoint>
    {
        Point const point = pointOf(vertex);
        FlyPoint end{vertex, point, 0.0f};
        if (flyMap && !FlyEnd(*flyMap, point))
        {
            std::optional<Point> const spot = PlatformSpot(*flyMap, point);
            if (!spot || request.AvoidsWalk(takeoff ? point : *spot, takeoff ? *spot : point))
                return std::nullopt;
            end.at = *spot;
            end.walkSeconds = WalkSeconds(point, *spot, runSpeed);
        }
        std::vector<Point> const& shut = takeoff ? closed.takeoffs : closed.landings;
        if (std::any_of(shut.begin(), shut.end(), [&end](Point const& at) { return Distance(at, end.at) < 0.5f; }))
            return std::nullopt;
        if (takeoff && noTakeoffAt(end.at))
            return std::nullopt;
        return end;
    };
    // The landings (FLY_EDGES_PER_NODE at most): the goal, and the nodes nearest it that are fly ends
    std::vector<FlyPoint> landings;
    if (flying)
    {
        if (std::optional<FlyPoint> const there = flyPointOf(target, false))
            landings.push_back(*there);
        std::vector<std::uint32_t> const nearest =
            flyMap ? graph.NearestNodes(goal.mapId, goal.x, goal.y, goal.z, FLY_LANDING_SCAN) : goalNodes;
        for (std::uint32_t const node : nearest)
        {
            if (landings.size() >= FLY_EDGES_PER_NODE)
                break;
            if (avoided(node) || (flyMap && !FlyEnd(*flyMap, pointOf(node))))
                continue;
            if (std::optional<FlyPoint> const landing = flyPointOf(node, false))
                landings.push_back(*landing);
        }
    }
    auto const landingOf = [&landings](std::uint32_t vertex)
    {
        return std::find_if(landings.begin(), landings.end(),
                            [vertex](FlyPoint const& landing) { return landing.vertex == vertex; });
    };
    auto const flyFrom = [&](std::uint32_t vertex)
    {
        if (landings.empty() || pointOf(vertex).mapId != goal.mapId)
            return;
        std::optional<FlyPoint> const takeoff = flyPointOf(vertex, true);
        if (!takeoff)
            return;
        for (FlyPoint const& landing : landings)
        {
            if (landing.vertex == vertex ||
                FlightRules::IsGroundHop(std::hypot(landing.at.x - takeoff->at.x, landing.at.y - takeoff->at.y), hop))
                continue;
            float const seconds = takeoff->walkSeconds + Distance(takeoff->at, landing.at) / bot.flySpeed +
                                  FLY_OVERHEAD_SECONDS + landing.walkSeconds;
            relax(vertex, landing.vertex, seconds, NO_LINK, Step::Fly, 0);
        }
    };

    best[source] = 0.0f;
    open.push({estimate(source), 0.0f, source});
    while (!open.empty())
    {
        Open const top = open.top();
        open.pop();
        if (top.g > best[top.vertex])
            continue;  // a stale entry: the vertex was reached cheaper since
        ++result.expandedNodes;
        std::uint32_t const vertex = top.vertex;
        if (vertex == target)
            break;

        if (vertex == source || vertex == bind)
        {
            for (std::uint32_t const node : vertex == source ? startNodes : bindNodes)
                walkTo(vertex, node);
            if (vertex == source ? direct
                                 : bot.hearth.mapId == goal.mapId && Distance(bot.hearth, goal) <= DIRECT_YARDS)
                walkTo(vertex, target);
            if (vertex == source && hearth)
                relax(source, bind, HEARTH_CAST_SECONDS + static_cast<float>(bot.hearthCooldownMs) / 1000.0f, NO_LINK,
                      Step::Hearth, 0);
            flyFrom(vertex);
            continue;
        }

        for (TravelGraphLink const& link : graph.LinksOf(vertex))
        {
            std::uint32_t fare = 0;
            float seconds = LinkSeconds(graph, link, bot, params, runSpeed, swimSpeed, fare);
            if (seconds < 0.0f || graph.OffWaitStop(link))
                continue;
            if (request.freeRides && IsRide(link))
                seconds = 0.0f;
            if (link.type == TravelLinkType::Air && (request.noFly || noTakeoffAt(pointOf(vertex))))
                continue;
            if (!request.avoidEdges.empty() && request.AvoidsEdge(link.from, link.to))
                continue;
            if (fare && static_cast<std::uint64_t>(spent[vertex]) + fare > bot.money)
                continue;  // the fares paid before this one leave too little for it
            if (!stuck.flags.empty())
            {
                auto const flagged = stuck.flags.find(EdgeKey(link.from, link.to));
                if (flagged != stuck.flags.end())
                {
                    if (flagged->second >= STUCK_SKIP_FLAGS)
                        continue;
                    seconds *= std::max(params.stuckPenalty, 1.0f);
                }
            }
            relax(vertex, link.to, seconds, graph.IndexOfLink(link), Step::Link, fare);
        }
        if (std::find(goalNodes.begin(), goalNodes.end(), vertex) != goalNodes.end())
            walkTo(vertex, target);
        flyFrom(vertex);
    }

    if (best[target] == UNREACHED)
    {
        result.failure = Failure::NoRoute;
        return result;
    }

    std::vector<std::uint32_t> chain;
    for (std::uint32_t vertex = target; vertex != source; vertex = via[vertex].vertex)
        chain.push_back(vertex);
    std::reverse(chain.begin(), chain.end());

    // The open transport leg: has it a ride, did its last link reach a wait point after the ride
    bool legRides = false;
    bool legLanded = false;
    float freeRideSeconds = 0.0f;  // what freeride's rides cost in time, left out of best[]
    auto const walkLeg = [](Point const& from, Point const& to, float seconds)
    {
        Leg walk;
        walk.from = from;
        walk.to = to;
        walk.seconds = seconds;
        return walk;
    };
    for (std::uint32_t const vertex : chain)
    {
        Via const& step = via[vertex];
        if (step.step == Step::Fly)
        {
            // A fly edge: its takeoff and landing as the search priced them, walks to and from a takeoff spot apart
            std::optional<FlyPoint> const takeoff = flyPointOf(step.vertex, true);
            auto const landing = landingOf(vertex);
            if (takeoff->walkSeconds > 0.0f)
                result.legs.push_back(walkLeg(pointOf(step.vertex), takeoff->at, takeoff->walkSeconds));
            Leg fly;
            fly.type = LegType::Fly;
            fly.from = takeoff->at;
            fly.to = landing->at;
            if (step.vertex < nodeCount && takeoff->walkSeconds <= 0.0f)
                fly.nodes.push_back(step.vertex);
            if (vertex < nodeCount && landing->walkSeconds <= 0.0f)
                fly.nodes.push_back(vertex);
            fly.seconds = best[vertex] - best[step.vertex] - takeoff->walkSeconds - landing->walkSeconds;
            result.legs.push_back(std::move(fly));
            if (landing->walkSeconds > 0.0f)
                result.legs.push_back(walkLeg(landing->at, pointOf(vertex), landing->walkSeconds));
            continue;
        }
        TravelGraphLink const* link = step.link != NO_LINK ? &graph.GetLink(step.link) : nullptr;
        LegType type = LegType::Walk;
        if (step.step == Step::Hearth)
            type = LegType::Hearth;
        else if (link && link->type == TravelLinkType::Portal)
            type = LegType::Portal;
        else if (link && link->type == TravelLinkType::StaticPortal)
            type = LegType::StaticPortal;
        else if (link && link->type == TravelLinkType::Flight)
            type = LegType::Flight;
        else if (link && link->type == TravelLinkType::Transport)
            type = LegType::Transport;
        else if (link && link->type == TravelLinkType::Air)
            type = LegType::Fly;

        TravelTaxiPath const* path = link ? graph.TaxiPathOf(*link) : nullptr;
        // A crossing is one transport leg: the near wait point's link, one ride, the far wait point's link
        bool const waitLink = link && TravelGraph::IsWaitPointLink(*link);
        bool const air = link && link->type == TravelLinkType::Air;
        bool merge = !result.legs.empty() && result.legs.back().type == type &&
                     (type == LegType::Walk || type == LegType::Flight);
        if (!result.legs.empty() && result.legs.back().type == LegType::Transport && type == LegType::Transport)
            merge = waitLink ? !legLanded : !legRides;
        if (merge && type == LegType::Transport && !waitLink)
        {
            result.legs.back().object = link->object;
            result.legs.back().periodMs = link->periodMs;
        }
        if (type == LegType::Transport)
        {
            legLanded = merge && waitLink && legRides;
            legRides = (merge && legRides) || !waitLink;
        }
        if (!merge)
        {
            Leg leg;
            leg.type = type;
            leg.air = air;
            leg.from = pointOf(step.vertex);
            leg.object = link ? link->object : 0;
            leg.periodMs = link ? link->periodMs : 0;
            if (step.vertex < nodeCount)
                leg.nodes.push_back(step.vertex);
            if (path)
                leg.taxiNodes.push_back(path->from);
            result.legs.push_back(std::move(leg));
        }

        Leg& leg = result.legs.back();
        if (type == LegType::Transport && !waitLink)
        {
            // The ride: its first stop is the node passed last, a node before it the near wait point
            leg.rideAt = leg.nodes.empty() ? 0 : static_cast<std::uint32_t>(leg.nodes.size() - 1);
            leg.waitFrom = leg.nodes.size() > 1;
            leg.stopFrom = pointOf(step.vertex);
            leg.stopTo = pointOf(vertex);
            leg.crossFrom = link->crossFrom;
            leg.crossTo = link->crossTo;
        }
        else if (type == LegType::Transport && legRides)
            leg.waitTo = true;
        leg.to = pointOf(vertex);
        float stepSeconds = best[vertex] - best[step.vertex];
        if (request.freeRides && link && IsRide(*link))
        {
            std::uint32_t fare = 0;
            stepSeconds = LinkSeconds(graph, *link, bot, params, runSpeed, swimSpeed, fare);
            freeRideSeconds += stepSeconds;
        }
        leg.seconds += stepSeconds;
        if (vertex < nodeCount)
            leg.nodes.push_back(vertex);
        if (path)
        {
            leg.taxiNodes.push_back(path->to);
            leg.fareCopper += path->price;
        }
    }

    // Wait-point links without a ride between them are a walk
    std::vector<Leg> legs;
    legs.reserve(result.legs.size());
    for (Leg& leg : result.legs)
    {
        if (leg.type == LegType::Transport && !leg.object)
            leg.type = LegType::Walk;
        if (leg.type != LegType::Walk || legs.empty() || legs.back().type != LegType::Walk)
        {
            legs.push_back(std::move(leg));
            continue;
        }
        Leg& walk = legs.back();
        auto first = leg.nodes.begin();
        if (first != leg.nodes.end() && !walk.nodes.empty() && walk.nodes.back() == *first)
            ++first;
        walk.nodes.insert(walk.nodes.end(), first, leg.nodes.end());
        walk.to = leg.to;
        walk.seconds += leg.seconds;
    }
    result.legs = std::move(legs);

    result.ok = true;
    result.totalSeconds = best[target] + freeRideSeconds;
    // One label per vertex: a vertex reopened with a new fare total can leave a successor's way through it on the old
    // total, so the rebuilt route may cost more than the purse. Then the route goes without paid flights (no loop: with
    // no money every paid flight link is pruned)
    if (RouteFare(result) > bot.money)
    {
        Request walking = request;
        walking.snapshot.money = 0;
        return GraphSearch(graph, walking, stuck, params, closed);
    }
    return result;
}

// A fly leg's flight from its takeoff to its landing, both in open cells (an air link's end may be a platform: the
// navigation plan put it there, the engine's takeoff and landing decide). False when it fails (a closed end, a
// void-ringed island, no path)
inline bool FlyLeg(FlightRules::FlightMapSet const& maps, Snapshot const& bot, Leg& leg, Result& route)
{
    FlightRules::HeightMap const* map = maps.Find(leg.from.mapId);
    if (!map || !map->IsOpen(leg.from.x, leg.from.y) || !map->IsOpen(leg.to.x, leg.to.y))
        return false;
    FlightRules::FlightRequest flight;
    flight.mapId = leg.from.mapId;
    flight.startX = leg.from.x;
    flight.startY = leg.from.y;
    flight.startZ = leg.from.z;
    flight.goalX = leg.to.x;
    flight.goalY = leg.to.y;
    flight.goalZ = leg.to.z;
    flight.speed = bot.flySpeed;
    flight.clearanceYards = bot.flightStyle.clearanceYards;
    flight.offsetYards = bot.flightStyle.offsetYards;
    using Clock = std::chrono::steady_clock;
    Clock::time_point const started = Clock::now();
    FlightRules::FlightRoute const flown = FlightRules::Search(*map, flight, maps.params);
    route.flightMicros += std::chrono::duration<float, std::micro>(Clock::now() - started).count();
    ++route.flightSearches;
    if (!flown.ok || flown.waypoints.size() < 2)
        return false;
    float const seconds = flown.seconds + FLY_OVERHEAD_SECONDS;
    route.totalSeconds += seconds - leg.seconds;
    leg.seconds = seconds;
    leg.flightPoints.clear();
    leg.flightPoints.reserve(flown.waypoints.size());
    for (FlightRules::FlightPoint const& point : flown.waypoints)
        leg.flightPoints.push_back({leg.from.mapId, point.x, point.y, point.z});
    leg.flightYards = flown.distance;
    leg.flightLevels = static_cast<std::uint32_t>(flown.levels.size());
    leg.flightClearance = bot.flightStyle.clearanceYards;
    return true;
}

// The route: the search with its fly edges and air links (GraphSearch), then each fly leg flown (FlyLeg). A failed
// flight closes its air link (an avoided edge) or its fly edge's takeoff and landing, and the search runs again; after
// FLY_TRIES searches with a failed flight the last one runs with flying closed, so a failed flight never hides a ground
// route. The searches' expanded nodes and flight searches add up in the result
inline Result Search(TravelGraph const& graph, Request const& request, StuckEdges const& stuck, Params const& params)
{
    std::optional<Request> changed;  // the request with the failed air links avoided, at the end with flying closed
    FlyClosures closed;
    Result spent;  // the counts of the searches before
    spent.failure = Failure::NoRoute;
    for (std::uint32_t attempt = 0; attempt <= FLY_TRIES; ++attempt)
    {
        if (attempt == FLY_TRIES)
        {
            if (!changed)
                changed = request;
            changed->noFly = true;
        }
        Request const& current = changed ? *changed : request;
        Result route = GraphSearch(graph, current, stuck, params, closed);
        route.expandedNodes += spent.expandedNodes;
        route.flightSearches += spent.flightSearches;
        route.flightMicros += spent.flightMicros;
        if (!route.ok || !params.flightMaps)
            return route;  // a later search has fewer edges: no route now is none then
        std::optional<std::size_t> failed;
        for (std::size_t i = 0; i < route.legs.size() && !failed; ++i)
            if (route.legs[i].type == LegType::Fly &&
                !FlyLeg(*params.flightMaps, current.snapshot, route.legs[i], route))
                failed = i;
        if (!failed)
            return route;
        spent.expandedNodes = route.expandedNodes;
        spent.flightSearches = route.flightSearches;
        spent.flightMicros = route.flightMicros;
        Leg const& leg = route.legs[*failed];
        if (leg.air && leg.nodes.size() == 2)
        {
            if (!changed)
                changed = request;
            changed->avoidEdges.push_back({leg.nodes.front(), leg.nodes.back()});
        }
        else
        {
            closed.takeoffs.push_back(leg.from);
            closed.landings.push_back(leg.to);
        }
    }
    return spent;  // not reached: the last search flies no edge
}

// The walk link a Stuck event was on: a link into the node at the given-up destination (within STUCK_NODE_YARDS)
// from one of the nodes nearest the bot. NO_LINK when the destination was no graph node (a grind spot, an NPC)
inline std::uint32_t StuckLink(TravelGraph const& graph, Point const& bot, Point const& destination)
{
    std::vector<std::uint32_t> const at =
        graph.NearestNodes(destination.mapId, destination.x, destination.y, destination.z, 1);
    if (at.empty())
        return NO_LINK;
    TravelGraphNode const& to = graph.GetNode(at.front());
    if (Distance({to.mapId, to.x, to.y, to.z}, destination) > STUCK_NODE_YARDS)
        return NO_LINK;

    for (std::uint32_t const from : graph.NearestNodes(bot.mapId, bot.x, bot.y, bot.z, END_NODES))
        for (TravelGraphLink const& link : graph.LinksOf(from))
            if (link.to == at.front() && link.type == TravelLinkType::Walk)
                return graph.IndexOfLink(link);
    return NO_LINK;
}

// The stuck edges, double buffered: map threads Push() reports (map and two positions, or the edge's two nodes when
// the bot followed a route), the world thread maps them to edges in Update() and publishes a new immutable StuckEdges;
// a search copies the shared_ptr once (Current()) and keeps its edges however often the book changes meanwhile
class StuckBook
{
public:
    struct Report
    {
        Point bot;
        Point destination;
        // A route follower knows the graph edge it walked (TravelLegs::StuckEdge): flagged as given, no position match
        std::uint32_t fromNode{TravelGraph::NO_NODE};
        std::uint32_t toNode{TravelGraph::NO_NODE};
    };

    static StuckBook& Instance()
    {
        static StuckBook book;
        return book;
    }

    // Any thread
    void Push(Report const& report)
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_queue.size() < STUCK_QUEUE_MAX)
            _queue.push_back(report);
    }

    // World thread only: queued reports become flags (dropped while the graph is not ready), flags older than avoidMs
    // expire. Returns the number of reports that flagged an edge
    std::uint32_t Update(TravelGraph const& graph, std::uint32_t nowMs, std::uint32_t avoidMs)
    {
        std::vector<Report> reports;
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            reports.swap(_queue);
        }

        std::uint32_t flagged = 0;
        bool changed = false;
        if (graph.IsReady())
        {
            for (Report const& report : reports)
            {
                std::uint32_t index = NO_LINK;
                if (report.fromNode < graph.NodeCount() && report.toNode < graph.NodeCount())
                {
                    for (TravelGraphLink const& link : graph.LinksOf(report.fromNode))
                    {
                        if (link.to == report.toNode)
                        {
                            index = graph.IndexOfLink(link);
                            break;
                        }
                    }
                }
                if (index == NO_LINK)
                    index = StuckLink(graph, report.bot, report.destination);
                if (index == NO_LINK)
                    continue;
                TravelGraphLink const& link = graph.GetLink(index);
                Mark& mark = _marks[EdgeKey(link.from, link.to)];
                mark.flags = static_cast<std::uint16_t>(std::min<std::uint32_t>(mark.flags + 1u, 0xFFFFu));
                mark.lastMs = nowMs;
                ++flagged;
                changed = true;
            }
        }

        if (!_marks.empty() && (changed || nowMs - _sweptMs >= SWEEP_MS))
        {
            _sweptMs = nowMs;
            for (auto it = _marks.begin(); it != _marks.end();)
            {
                if (nowMs - it->second.lastMs > avoidMs)
                {
                    it = _marks.erase(it);
                    changed = true;
                }
                else
                    ++it;
            }
        }

        if (changed)
        {
            auto edges = std::make_shared<StuckEdges>();
            for (auto const& [key, mark] : _marks)
                edges->flags.emplace(key, mark.flags);
            std::shared_ptr<StuckEdges const> published = std::move(edges);
            std::lock_guard<std::mutex> lock(_currentMutex);
            _current.swap(published);
        }
        return flagged;
    }

    // Any thread: the edges as last published
    std::shared_ptr<StuckEdges const> Current() const
    {
        std::lock_guard<std::mutex> lock(_currentMutex);
        return _current;
    }

private:
    static constexpr std::uint32_t SWEEP_MS = 1000;

    struct Mark
    {
        std::uint16_t flags{0};
        std::uint32_t lastMs{0};
    };

    std::mutex _queueMutex;
    std::vector<Report> _queue;
    mutable std::mutex _currentMutex;
    std::shared_ptr<StuckEdges const> _current{std::make_shared<StuckEdges const>()};
    std::unordered_map<std::uint64_t, Mark> _marks;  // world thread only
    std::uint32_t _sweptMs{0};
};
}  // namespace TravelRoute

#endif
