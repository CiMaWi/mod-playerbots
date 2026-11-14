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
//   (Params::transportWaitSeconds, half a period) plus SHIP_RIDE_SECONDS; a wait point's link to its stop (no object)
//   its walk. A stop with a wait point is reached and left only through the wait point and its rides
//   (TravelGraph::OffWaitStop), so a crossing runs wait point, stop, ride, stop, wait point
// - static portal: not searched yet (WT55d)
// - hearthstone: 10 s cast plus the remaining cooldown, from the bot's position to its bind point. Only from the start:
//   using it later arrives no earlier (max(cooldown, t) + 10 s is never less than cooldown + 10 s)
//   A cooldown above HEARTH_WAIT_CAP_MS (3 min) is no edge: the follower never waits that long
// - an edge flagged by Stuck events costs Params::stuckPenalty times more, and is skipped after STUCK_SKIP_FLAGS
// - the request's avoided nodes are no start or goal node, and noDirect drops the straight walk from the bot (the
//   bot's own stuck walks of the trip: their start or goal hop is no graph edge to flag); its avoided edges (the
//   trip's walks where the bot died twice) are not used
// - fly (own flying mount, WU10): no graph edge. The ground route's long walks on a map the bot may fly on are flown
//   when the flight (FlightRules::Search) plus FLY_OVERHEAD_SECONDS is quicker than their walk (AddFlyLegs)

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
#include "TravelGraph.h"

namespace TravelRoute
{
constexpr std::size_t TAXI_MASK_SIZE = 14;    // TaxiMaskSize (DBCStructure.h): 448 taxi nodes, PlayerTaxi's layout
constexpr std::size_t END_NODES = 5;          // graph nodes the bot, the goal and the bind point connect to
constexpr float DIRECT_YARDS = 100.0f;        // start and goal this close on one map may also be walked directly
constexpr float HEARTH_CAST_SECONDS = 10.0f;  // the hearthstone's cast
constexpr std::uint32_t HEARTH_WAIT_CAP_MS = 3 * 60 * 1000;  // a cooldown longer than this is no hearth edge
constexpr float FLIGHT_BOARDING_SECONDS = 5.0f;              // talking to the flight master, mounting and landing
constexpr float SHIP_RIDE_SECONDS = 120.0f;    // a ship or zeppelin crossing until WT19 knows the real periods
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
constexpr std::uint32_t FLY_SEARCHES_MAX = 3;  // flight searches per route at most (AddFlyLegs)
constexpr float FLY_OWN_SPOT_YARDS = 10.0f;    // route points this close to the bot are its own spot (a walk ends
                                               // within 5 yd of its node)
constexpr float FLY_PLATFORM_YARDS = FlightRules::PLATFORM_YARDS;  // this far above its cell's ground: no fly end
constexpr float FLY_TAKEOFF_WALK_YARDS = 500.0f;  // a platform point walks at most this far to a takeoff spot

struct Point
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

inline float Distance(Point const& a, Point const& b)
{
    float const dx = a.x - b.x;
    float const dy = a.y - b.y;
    float const dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A straight walk off the graph (start to goal, to or from an end's nearest nodes, bind point to goal) climbs at most
// its horizontal run plus a step: a cliff or a tower top is no straight walk however close it is (Thunder Bluff's
// mesa is 53 yd across from its base and 68 yd up)
constexpr float CLIMB_STEP_YARDS = 4.0f;

inline bool StraightWalk(Point const& a, Point const& b)
{
    return std::fabs(a.z - b.z) <= std::hypot(a.x - b.x, a.y - b.y) + CLIMB_STEP_YARDS;
}

// An off-graph walk's seconds price its climb: a straight line up a steep slope is no run at full speed, and a graph
// way (a ramp, an elevator) should win where there is one (Thunder Bluff's foot: 74 yd across and 66 yd up)
constexpr float CLIMB_COST_FACTOR = 2.0f;

inline float WalkSeconds(Point const& a, Point const& b, float runSpeed)
{
    return (Distance(a, b) + CLIMB_COST_FACTOR * std::fabs(a.z - b.z)) / runSpeed;
}

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
    // When not, a fly leg never starts at the bot's own spot (AddFlyLegs)
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
    // After a failed fly leg of the trip (TravelLegs::AfterFailure): no fly legs
    bool noFly{false};

    bool AvoidsEdge(std::uint32_t from, std::uint32_t to) const
    {
        for (auto const& [a, b] : avoidEdges)
            if ((a == from && b == to) || (a == to && b == from))
                return true;
        return false;
    }
};

// Tuning values (WorldTravelConfig) and the selftest's switch
struct Params
{
    float stuckPenalty{5.0f};            // Playerbots.WorldTravel.StuckPenalty
    float transportWaitSeconds{120.0f};  // Playerbots.WorldTravel.TransportWaitSeconds
    float flightSpeed{TravelGraph::FLIGHT_SPEED};
    bool heuristic{true};  // false: plain Dijkstra over the same costs (the selftest's reference)
    // The published height maps (FlightMap.h's Planner::FlightMaps()); null: no fly legs
    std::shared_ptr<FlightRules::FlightMapSet const> flightMaps;
};

enum class LegType : std::uint8_t
{
    Walk,
    Portal,
    Flight,  // a taxi
    Transport,
    Hearth,
    Fly  // on the bot's own flying mount
};

enum class Failure : std::uint8_t
{
    None,
    GraphNotReady,  // never waited for: no route now
    NoStartNode,    // no graph node on the bot's map (an instance, a map without travel nodes)
    NoGoalNode,
    NoRoute
};

struct Leg
{
    LegType type{LegType::Walk};
    Point from;
    Point to;
    // Graph node indices passed, in order: walk waypoints; a transport leg's stops, between the wait points where the
    // graph has them; the ends otherwise
    std::vector<std::uint32_t> nodes;
    std::uint32_t object{0};               // area trigger, first TaxiPath or transport entry
    std::vector<std::uint32_t> taxiNodes;  // flight legs: the taxi node chain for Player::ActivateTaxiPathTo
    float seconds{0.0f};
    std::uint32_t fareCopper{0};
    // Fly legs: the planned flight from the takeoff on the ground to the landing on the ground
    // (FlightRoute::waypoints), its yards (climbs included), its held heights and the bot's clearance it was planned
    // with
    std::vector<Point> flightPoints;
    float flightYards{0.0f};
    std::uint32_t flightLevels{0};
    float flightClearance{0.0f};
    // Transport legs: the index in nodes of the ride's first stop, the ride's stops, and whether an end is a wait point
    // (waitFrom, waitTo: from or to is then the wait point, else the stop itself; TravelLegs::StopFrom, StopTo)
    std::uint32_t rideAt{0};
    Point stopFrom;
    Point stopTo;
    bool waitFrom{false};
    bool waitTo{false};
};

struct Result
{
    bool ok{false};
    Failure failure{Failure::None};
    // Consecutive walk links merge into one leg, consecutive flights into one, a crossing with its wait points' links
    // into one transport leg
    std::vector<Leg> legs;
    float totalSeconds{0.0f};
    std::uint32_t expandedNodes{0};
    std::uint32_t flightSearches{0};  // AddFlyLegs' FlightRules::Search calls and their time
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
            return link.extraCost;
        case TravelLinkType::Transport:
            if (!bot.allowTransport)
                return -1.0f;
            if (TravelGraph::IsWaitPointLink(link))
                return link.distance / runSpeed;
            return link.extraCost >= 1.0f ? link.extraCost : params.transportWaitSeconds + SHIP_RIDE_SECONDS;
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
        case TravelLinkType::StaticPortal:
            return -1.0f;
    }
    return -1.0f;
}

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
// No fly legs: Search adds them to this route
inline Result GroundSearch(TravelGraph const& graph, Request const& request, StuckEdges const& stuck,
                           Params const& params)
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
    bool const direct = !request.noDirect && start.mapId == goal.mapId && Distance(start, goal) <= DIRECT_YARDS &&
                        StraightWalk(start, goal);
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
    float const fastest = std::max({runSpeed, swimSpeed, params.flightSpeed});

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
        if (!params.heuristic)
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
        Hearth
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
        if (StraightWalk(a, b))
            relax(from, to, WalkSeconds(a, b, runSpeed), NO_LINK, Step::Walk, 0);
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
                                 : bot.hearth.mapId == goal.mapId && Distance(bot.hearth, goal) <= DIRECT_YARDS &&
                                       StraightWalk(bot.hearth, goal))
                walkTo(vertex, target);
            if (vertex == source && hearth)
                relax(source, bind, HEARTH_CAST_SECONDS + static_cast<float>(bot.hearthCooldownMs) / 1000.0f, NO_LINK,
                      Step::Hearth, 0);
            continue;
        }

        for (TravelGraphLink const& link : graph.LinksOf(vertex))
        {
            std::uint32_t fare = 0;
            float seconds = LinkSeconds(graph, link, bot, params, runSpeed, swimSpeed, fare);
            if (seconds < 0.0f || graph.OffWaitStop(link))
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
    for (std::uint32_t const vertex : chain)
    {
        Via const& step = via[vertex];
        TravelGraphLink const* link = step.link != NO_LINK ? &graph.GetLink(step.link) : nullptr;
        LegType type = LegType::Walk;
        if (step.step == Step::Hearth)
            type = LegType::Hearth;
        else if (link && link->type == TravelLinkType::Portal)
            type = LegType::Portal;
        else if (link && link->type == TravelLinkType::Flight)
            type = LegType::Flight;
        else if (link && link->type == TravelLinkType::Transport)
            type = LegType::Transport;

        TravelTaxiPath const* path = link ? graph.TaxiPathOf(*link) : nullptr;
        // A crossing is one transport leg: the near wait point's link, one ride, the far wait point's link
        bool const waitLink = link && TravelGraph::IsWaitPointLink(*link);
        bool merge = !result.legs.empty() && result.legs.back().type == type &&
                     (type == LegType::Walk || type == LegType::Flight);
        if (!result.legs.empty() && result.legs.back().type == LegType::Transport && type == LegType::Transport)
            merge = waitLink ? !legLanded : !legRides;
        if (merge && type == LegType::Transport && !waitLink)
            result.legs.back().object = link->object;
        if (type == LegType::Transport)
        {
            legLanded = merge && waitLink && legRides;
            legRides = (merge && legRides) || !waitLink;
        }
        if (!merge)
        {
            Leg leg;
            leg.type = type;
            leg.from = pointOf(step.vertex);
            leg.object = link ? link->object : 0;
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
        }
        else if (type == LegType::Transport && legRides)
            leg.waitTo = true;
        leg.to = pointOf(vertex);
        leg.seconds += best[vertex] - best[step.vertex];
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
    result.totalSeconds = best[target];
    // One label per vertex: a vertex reopened with a new fare total can leave a successor's way through it on the old
    // total, so the rebuilt route may cost more than the purse. Then the route goes without paid flights (no loop: with
    // no money every paid flight link is pruned)
    if (RouteFare(result) > bot.money)
    {
        Request walking = request;
        walking.snapshot.money = 0;
        return GroundSearch(graph, walking, stuck, params);
    }
    return result;
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

// The ground route's long walks flown (WU10, PLAN 3.9). The A* stays a ground search (its Dijkstra equality holds);
// then each walk leg on a map the bot may fly on is tried as one flight between its first and last points (start,
// graph nodes, end) between fly ends (FlyEnd), so a walk out of a no-fly city keeps its part to the city edge. The
// flight replaces that part when its seconds plus FLY_OVERHEAD_SECONDS beat the part's share of the leg's seconds (by
// its length along the points); a hop under the ground hop, a failed flight search (closed ends, a void-ringed island)
// or a slower flight keeps the walk. The bot's own spot counts as closed when it cannot take off there (indoors, in
// water, a WMO no-fly area: Snapshot::takeoffHere), and so does every point of the walk within FLY_OWN_SPOT_YARDS of
// it: after a takeoff refused at a graph node the replan's walk passes that node again (TravelLegs::AfterFailure keeps
// flying). A walk that starts on a platform (WU11b fix 7: Dalaran, whose graph walks down an edge no bot can walk)
// goes straight to the nearest takeoff spot (PlatformSpot) and flies from there; one that ends on a platform lands at
// its nearest spot and walks on. A spot where the bot stands and cannot take off keeps the ground route. At most
// FLY_SEARCHES_MAX searches per route, on the route service's workers only
inline void AddFlyLegs(TravelGraph const& graph, Request const& request, Params const& params, Result& route)
{
    Snapshot const& bot = request.snapshot;
    if (!route.ok || request.noFly || !params.flightMaps)
        return;
    FlightRules::FlightMapSet const& maps = *params.flightMaps;
    using Clock = std::chrono::steady_clock;
    std::vector<Leg> legs;
    legs.reserve(route.legs.size() + 2);
    for (Leg& leg : route.legs)
    {
        FlightRules::HeightMap const* map = nullptr;
        if (leg.type == LegType::Walk && leg.from.mapId == leg.to.mapId && bot.MayFlyOn(leg.from.mapId) &&
            route.flightSearches < FLY_SEARCHES_MAX)
            map = maps.Find(leg.from.mapId);
        if (!map)
        {
            legs.push_back(std::move(leg));
            continue;
        }

        // points[k + 1] is graph node leg.nodes[k]
        std::vector<Point> points;
        points.reserve(leg.nodes.size() + 2);
        points.push_back(leg.from);
        for (std::uint32_t const node : leg.nodes)
        {
            TravelGraphNode const& at = graph.GetNode(node);
            points.push_back({at.mapId, at.x, at.y, at.z});
        }
        points.push_back(leg.to);
        auto const nearBot = [&bot](Point const& point)
        { return point.mapId == bot.position.mapId && Distance(point, bot.position) <= FLY_OWN_SPOT_YARDS; };
        std::optional<Point> takeoff = PlatformSpot(*map, points.front());
        if (takeoff && nearBot(*takeoff) && !bot.takeoffHere)
            takeoff.reset();
        std::optional<Point> const landing = PlatformSpot(*map, points.back());
        auto const startsAt = [&](Point const& point)
        { return FlyEnd(*map, point) && (bot.takeoffHere || !nearBot(point)); };
        std::size_t first = 0;
        while (!takeoff && first < points.size() && !startsAt(points[first]))
            ++first;
        std::size_t last = points.size() - 1;
        while (!landing && last > first && !FlyEnd(*map, points[last]))
            --last;
        std::vector<float> along(points.size(), 0.0f);
        for (std::size_t k = 1; k < points.size(); ++k)
            along[k] = along[k - 1] + Distance(points[k - 1], points[k]);
        float const length = along.back();
        if (first >= last || length <= 0.0f)
        {
            legs.push_back(std::move(leg));
            continue;
        }
        Point const start = takeoff ? *takeoff : points[first];
        Point const goal = landing ? *landing : points[last];
        if (FlightRules::IsGroundHop(std::hypot(goal.x - start.x, goal.y - start.y), maps.params))
        {
            legs.push_back(std::move(leg));
            continue;
        }

        FlightRules::FlightRequest flight;
        flight.mapId = leg.from.mapId;
        flight.startX = start.x;
        flight.startY = start.y;
        flight.startZ = start.z;
        flight.goalX = goal.x;
        flight.goalY = goal.y;
        flight.goalZ = goal.z;
        flight.speed = bot.flySpeed;
        flight.clearanceYards = bot.flightStyle.clearanceYards;
        flight.offsetYards = bot.flightStyle.offsetYards;
        Clock::time_point const started = Clock::now();
        FlightRules::FlightRoute const flown = FlightRules::Search(*map, flight, maps.params);
        route.flightMicros += std::chrono::duration<float, std::micro>(Clock::now() - started).count();
        ++route.flightSearches;
        float const walkSeconds = leg.seconds * (along[last] - along[first]) / length;
        float const flySeconds = flown.seconds + FLY_OVERHEAD_SECONDS;
        float const runSpeed = std::max(bot.runSpeed, 1.0f);
        float const toTakeoff = takeoff ? Distance(points.front(), start) / runSpeed : 0.0f;
        float const fromLanding = landing ? Distance(goal, points.back()) / runSpeed : 0.0f;
        if (!flown.ok || flown.waypoints.size() < 2 || flySeconds + toTakeoff + fromLanding >= walkSeconds)
        {
            legs.push_back(std::move(leg));
            continue;
        }

        auto const nodeOf = [&leg](std::size_t index)
        { return index >= 1 && index <= leg.nodes.size() ? leg.nodes[index - 1] : TravelGraph::NO_NODE; };
        auto const walkPart = [&](std::size_t from, std::size_t to)
        {
            Leg part;
            part.from = points[from];
            part.to = points[to];
            for (std::size_t k = from; k <= to; ++k)
                if (nodeOf(k) != TravelGraph::NO_NODE)
                    part.nodes.push_back(nodeOf(k));
            part.seconds = leg.seconds * (along[to] - along[from]) / length;
            return part;
        };
        auto const straight = [](Point const& from, Point const& to, float seconds)
        {
            Leg walk;
            walk.from = from;
            walk.to = to;
            walk.seconds = seconds;
            return walk;
        };
        if (takeoff)
            legs.push_back(straight(points.front(), start, toTakeoff));
        else if (first > 0)
            legs.push_back(walkPart(0, first));
        Leg fly;
        fly.type = LegType::Fly;
        fly.from = start;
        fly.to = goal;
        if (!takeoff && nodeOf(first) != TravelGraph::NO_NODE)
            fly.nodes.push_back(nodeOf(first));
        if (!landing && nodeOf(last) != TravelGraph::NO_NODE)
            fly.nodes.push_back(nodeOf(last));
        fly.seconds = flySeconds;
        fly.flightPoints.reserve(flown.waypoints.size());
        for (FlightRules::FlightPoint const& point : flown.waypoints)
            fly.flightPoints.push_back({leg.from.mapId, point.x, point.y, point.z});
        fly.flightYards = flown.distance;
        fly.flightLevels = static_cast<std::uint32_t>(flown.levels.size());
        fly.flightClearance = bot.flightStyle.clearanceYards;
        legs.push_back(std::move(fly));
        if (landing)
            legs.push_back(straight(goal, points.back(), fromLanding));
        else if (last + 1 < points.size())
            legs.push_back(walkPart(last, points.size() - 1));
        route.totalSeconds += flySeconds + toTakeoff + fromLanding - walkSeconds;
    }
    route.legs = std::move(legs);
}

// The route: the ground search, then its long walks flown where the bot may fly (AddFlyLegs)
inline Result Search(TravelGraph const& graph, Request const& request, StuckEdges const& stuck, Params const& params)
{
    Result route = GroundSearch(graph, request, stuck, params);
    AddFlyLegs(graph, request, params, route);
    return route;
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

// The checks shared by tests/route-rules-selftest.cpp and `playerbots route selftest` on the shipped graph with its
// TaxiPath data. Returns the number of checks; failures get one line each, notes the route summaries
inline std::uint32_t Selftest(TravelGraph const& graph, std::vector<std::string>& failures,
                              std::vector<std::string>& notes)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    auto const num = [](double value) { return std::to_string(std::llround(value)); };

    if (!check(graph.IsReady(), "graph ready"))
        return checks;
    if (!check(graph.Stats().taxiPaths > 0, "the graph has TaxiPath data (DBC)"))
        return checks;
    check(graph.Stats().flightsWithoutTaxi == 0,
          std::to_string(graph.Stats().flightsWithoutTaxi) + " flight links without a TaxiPath");

    Request notReady;
    TravelGraph const idle;
    check(Search(idle, notReady, StuckEdges{}, Params{}).failure == Failure::GraphNotReady,
          "a graph that is not ready gives no route");

    // Places by name nearest a known position (names are not unique), as in TravelGraph::Selftest
    auto const place = [&graph](char const* name, std::uint32_t mapId, float x, float y) -> Point
    {
        std::uint32_t const index = graph.FindNear(name, mapId, x, y);
        if (index == TravelGraph::NO_NODE)
            return {TravelGraph::ANY_MAP, 0.0f, 0.0f, 0.0f};
        TravelGraphNode const& node = graph.GetNode(index);
        return {node.mapId, node.x, node.y, node.z};
    };
    // The taxi node of a flight master node: the origin of its flight links
    auto const taxiNodeAt = [&graph](Point const& at) -> std::uint32_t
    {
        std::vector<std::uint32_t> const closest = graph.NearestNodes(at.mapId, at.x, at.y, at.z, 1);
        if (closest.empty())
            return 0;
        for (TravelGraphLink const& link : graph.LinksOf(closest.front()))
            if (TravelTaxiPath const* path = graph.TaxiPathOf(link))
                return path->from;
        return 0;
    };

    Point const goldshire = place("Goldshire innkeeper", 0, -9462.7f, 16.2f);
    Point const stormwindInn = place("Stormwind City innkeeper", 0, -8867.8f, 673.7f);
    Point const stormwindFlight = place("flightMaster", 0, -8835.8f, 490.1f);
    Point const ironforgeFlight = place("flightMaster", 0, -4821.8f, -1152.4f);
    Point const darnassus = place("Darnassus", 1, 9947.0f, 2482.0f);
    std::uint32_t const stormwindTaxi = taxiNodeAt(stormwindFlight);
    std::uint32_t const ironforgeTaxi = taxiNodeAt(ironforgeFlight);
    bool const placed = goldshire.mapId == 0 && stormwindInn.mapId == 0 && stormwindFlight.mapId == 0 &&
                        ironforgeFlight.mapId == 0 && darnassus.mapId == 1;
    if (!check(placed && stormwindTaxi && ironforgeTaxi, "fixture places and taxi nodes found"))
        return checks;

    // Every route: legs chain from the start to the goal and their seconds add up
    auto const describe = [&num](Result const& route)
    {
        std::string text = num(route.totalSeconds) + " s, " + std::to_string(route.expandedNodes) + " expanded:";
        for (Leg const& leg : route.legs)
            text += std::string(" ") + LegTypeWord(leg.type) + " " + num(leg.seconds) + " s" +
                    (leg.fareCopper ? " " + std::to_string(leg.fareCopper) + "c" : "") +
                    (leg.object ? " #" + std::to_string(leg.object) : "") + ";";
        return text;
    };
    auto const legCount = [](Result const& route, LegType type)
    {
        return static_cast<std::size_t>(
            std::count_if(route.legs.begin(), route.legs.end(), [type](Leg const& leg) { return leg.type == type; }));
    };
    auto const same = [](Point const& a, Point const& b) { return a.mapId == b.mapId && Distance(a, b) < 0.01f; };
    auto const sound = [&](Result const& route, Request const& request, std::string const& label)
    {
        if (!check(route.ok, label + ": no route (" + FailureWord(route.failure) + ")"))
            return false;
        bool chained = !route.legs.empty() && same(route.legs.front().from, request.snapshot.position) &&
                       same(route.legs.back().to, request.goal);
        float sum = 0.0f;
        for (std::size_t i = 0; i < route.legs.size(); ++i)
        {
            sum += route.legs[i].seconds;
            chained = chained && (i == 0 || same(route.legs[i - 1].to, route.legs[i].from));
        }
        check(chained, label + ": legs do not chain from start to goal");
        check(std::fabs(sum - route.totalSeconds) <= 0.001f * route.totalSeconds + 0.05f,
              label + ": legs sum to " + num(sum) + " s, not " + num(route.totalSeconds) + " s");
        notes.push_back(label + ": " + describe(route));
        return true;
    };
    Params const aStar;
    Params dijkstraParams;
    dijkstraParams.heuristic = false;
    auto const optimal =
        [&](Result const& route, Request const& request, StuckEdges const& stuck, std::string const& label)
    {
        Result const reference = Search(graph, request, stuck, dijkstraParams);
        check(reference.ok == route.ok &&
                  std::fabs(reference.totalSeconds - route.totalSeconds) <= 0.001f * reference.totalSeconds + 0.05f,
              label + ": A* " + num(route.totalSeconds) + " s, Dijkstra " + num(reference.totalSeconds) + " s");
        return reference;
    };

    // a) A level 20 human at the Goldshire inn, no flight points, bound in Stormwind (ready), to Ironforge. With no
    //    flights and no transports (the Deeprun Tram is one) the way is on foot through Redridge, the Burning Steppes,
    //    the Searing Gorge, the Badlands and Loch Modan or Dun Morogh, past mobs far above level 20: slow
    Snapshot human;
    human.position = goldshire;
    human.team = 0;
    human.level = 20;
    human.money = 10000;
    human.hasHearthstone = true;
    human.hearth = stormwindInn;
    Request toIronforge{human, ironforgeFlight};
    Result const walked = Search(graph, toIronforge, StuckEdges{}, aStar);
    if (sound(walked, toIronforge, "a) Goldshire -> Ironforge, no flight points"))
    {
        Result const reference = optimal(walked, toIronforge, StuckEdges{}, "a)");
        check(legCount(walked, LegType::Flight) == 0, "a) a flight leg without known flight points");
        check(legCount(walked, LegType::Transport) == 0, "a) a transport leg without allowTransport");
        check(legCount(walked, LegType::Hearth) == legCount(reference, LegType::Hearth),
              "a) A* and Dijkstra disagree on the hearth");
        check(walked.totalSeconds >= 1500.0f && walked.totalSeconds <= 20000.0f,
              "a) " + num(walked.totalSeconds) + " s outside 1500..20000 s");
        check(walked.expandedNodes <= reference.expandedNodes,
              "a) A* expanded " + std::to_string(walked.expandedNodes) + " nodes, Dijkstra " +
                  std::to_string(reference.expandedNodes));
    }

    // b) The same bot at the Stormwind flight master, Stormwind and Ironforge known: one flight of about 6500 yd
    //    (200 s, 3.5 min in the game) beats any walk; without money the flight is gone
    Snapshot flyer = human;
    flyer.position = stormwindFlight;
    flyer.LearnTaxiNode(stormwindTaxi);
    flyer.LearnTaxiNode(ironforgeTaxi);
    Request flight{flyer, ironforgeFlight};
    Result const flown = Search(graph, flight, StuckEdges{}, aStar);
    if (sound(flown, flight, "b) Stormwind -> Ironforge, both known"))
    {
        optimal(flown, flight, StuckEdges{}, "b)");
        auto const leg = std::find_if(flown.legs.begin(), flown.legs.end(),
                                      [](Leg const& candidate) { return candidate.type == LegType::Flight; });
        check(legCount(flown, LegType::Flight) == 1, "b) not exactly one flight leg");
        check(leg != flown.legs.end() && leg->seconds < 400.0f && leg->fareCopper > 0,
              "b) the flight leg is not under 400 s with a fare");
        check(leg != flown.legs.end() && leg->taxiNodes.size() == 2 && leg->taxiNodes.front() == stormwindTaxi &&
                  leg->taxiNodes.back() == ironforgeTaxi,
              "b) the flight leg's taxi nodes are not Stormwind -> Ironforge");
        check(flown.totalSeconds < 400.0f, "b) " + num(flown.totalSeconds) + " s, not under 400 s");
    }
    Request broke = flight;
    broke.snapshot.money = 0;
    Result const poor = Search(graph, broke, StuckEdges{}, aStar);
    check(legCount(poor, LegType::Flight) == 0, "b) a flight leg without money");
    check(!poor.ok || poor.totalSeconds > flown.totalSeconds, "b) the route without money is not dearer");
    // The follower's time-value rule and the search agree: the flown route (fare weighed in) is chosen exactly when
    // FlightWorthIt says the flight saves more than its fare weighs
    if (poor.ok && flown.ok)
    {
        std::uint32_t const fare = RouteFare(flown);
        float const pure = flown.totalSeconds - FareSeconds(fare, flyer.money, flyer.thrift);
        check(FlightWorthIt(poor.totalSeconds, pure, fare, flyer.money, flyer.thrift) ==
                  (flown.totalSeconds < poor.totalSeconds),
              "b) FlightWorthIt and the search disagree on Stormwind -> Ironforge");
    }
    check(FlightWorthIt(1000.0f, 200.0f, 100, 100000, 50), "b) FlightWorthIt: a cheap fare saves 800 s");
    check(!FlightWorthIt(1000.0f, 200.0f, 100000, 100000, 100), "b) FlightWorthIt: the whole purse weighs an hour");
    check(!FlightWorthIt(1000.0f, 200.0f, 500, 100, 50), "b) FlightWorthIt: an unaffordable fare");
    check(!FlightWorthIt(200.0f, 190.0f, 50000, 100000, 100), "b) FlightWorthIt: 10 s saved, half a purse spent");
    Request unknown = flight;
    unknown.snapshot.knownTaxi = {};
    unknown.snapshot.LearnTaxiNode(stormwindTaxi);
    check(legCount(Search(graph, unknown, StuckEdges{}, aStar), LegType::Flight) == 0,
          "b) a flight to an unknown node");
    Request learned = flight;
    learned.snapshot.knownTaxi = {};
    learned.snapshot.LearnTaxiNode(ironforgeTaxi);
    check(legCount(Search(graph, learned, StuckEdges{}, aStar), LegType::Flight) == 1,
          "b) the origin's node is not learned at its flight master");
    Request horde = flight;
    horde.snapshot.team = 1;
    check(legCount(Search(graph, horde, StuckEdges{}, aStar), LegType::Flight) == 0,
          "b) a Horde bot flies from an Alliance flight master");

    // c) At the Ironforge flight master, home in Stormwind: a ready hearth (10 s) beats the flight; with 20 minutes
    //    of cooldown the flight wins
    Request home{flyer, stormwindInn};
    home.snapshot.position = ironforgeFlight;
    Result const hearthed = Search(graph, home, StuckEdges{}, aStar);
    if (sound(hearthed, home, "c) Ironforge -> Stormwind inn, hearth ready"))
    {
        optimal(hearthed, home, StuckEdges{}, "c) ready");
        check(!hearthed.legs.empty() && hearthed.legs.front().type == LegType::Hearth,
              "c) the ready hearth is not the first leg");
        check(hearthed.totalSeconds < 60.0f, "c) " + num(hearthed.totalSeconds) + " s with a ready hearth");
    }
    Request cooling = home;
    cooling.snapshot.hearthCooldownMs = 20 * 60 * 1000;
    Result const flownHome = Search(graph, cooling, StuckEdges{}, aStar);
    if (sound(flownHome, cooling, "c) Ironforge -> Stormwind inn, hearth in 20 min"))
    {
        optimal(flownHome, cooling, StuckEdges{}, "c) cooling");
        check(legCount(flownHome, LegType::Hearth) == 0, "c) the cooling hearth is used");
        check(legCount(flownHome, LegType::Flight) == 1, "c) no flight home");
        check(flownHome.totalSeconds > hearthed.totalSeconds, "c) the cooling hearth's route is not dearer");
    }
    // A short cooldown is waited for (60 s + 10 s beats the 200 s flight); one past the cap is no edge at all
    Request shortWait = home;
    shortWait.snapshot.hearthCooldownMs = 60 * 1000;
    Result const waited = Search(graph, shortWait, StuckEdges{}, aStar);
    if (sound(waited, shortWait, "c) Ironforge -> Stormwind inn, hearth in 60 s"))
        check(legCount(waited, LegType::Hearth) == 1 && waited.totalSeconds < 120.0f,
              "c) a 60 s cooldown is not waited for (" + num(waited.totalSeconds) + " s)");
    Request pastCap = home;
    pastCap.snapshot.hearthCooldownMs = HEARTH_WAIT_CAP_MS + 1000;
    check(legCount(Search(graph, pastCap, StuckEdges{}, aStar), LegType::Hearth) == 0,
          "c) a cooldown past the wait cap is used");
    Request noStone = home;
    noStone.snapshot.hasHearthstone = false;
    check(legCount(Search(graph, noStone, StuckEdges{}, aStar), LegType::Hearth) == 0,
          "c) a hearth leg without a hearthstone");

    // d) Stuck edges: flag the first graph edge of the walk in a); one flag makes it dearer or avoided, three skip it,
    //    and the book's flags expire after the avoid window
    std::uint32_t flaggedFrom = TravelGraph::NO_NODE;
    std::uint32_t flaggedTo = TravelGraph::NO_NODE;
    for (Leg const& leg : walked.legs)
    {
        if (leg.type == LegType::Walk && leg.nodes.size() >= 2)
        {
            flaggedFrom = leg.nodes[0];
            flaggedTo = leg.nodes[1];
            break;
        }
    }
    auto const uses = [&](Result const& route)
    {
        for (Leg const& leg : route.legs)
            for (std::size_t i = 1; i < leg.nodes.size(); ++i)
                if (leg.nodes[i - 1] == flaggedFrom && leg.nodes[i] == flaggedTo)
                    return true;
        return false;
    };
    if (check(walked.ok && flaggedFrom != TravelGraph::NO_NODE, "d) a walk edge to flag"))
    {
        StuckEdges once;
        once.flags[EdgeKey(flaggedFrom, flaggedTo)] = 1;
        Result const penalised = Search(graph, toIronforge, once, aStar);
        optimal(penalised, toIronforge, once, "d) one flag");
        check(penalised.ok && (penalised.totalSeconds > walked.totalSeconds || !uses(penalised)),
              "d) one flag neither costs more nor changes the route");
        StuckEdges thrice;
        thrice.flags[EdgeKey(flaggedFrom, flaggedTo)] = STUCK_SKIP_FLAGS;
        Result const avoided = Search(graph, toIronforge, thrice, aStar);
        check(!avoided.ok || !uses(avoided), "d) a skipped edge is used");

        StuckBook book;
        TravelGraphNode const& a = graph.GetNode(flaggedFrom);
        TravelGraphNode const& b = graph.GetNode(flaggedTo);
        StuckBook::Report const report{{a.mapId, a.x, a.y, a.z}, {b.mapId, b.x, b.y, b.z}};
        std::uint32_t const avoidMs = 600 * 1000;
        std::uint32_t const nowMs = 0xFFFFF000u;  // the clock wraps during the window
        std::shared_ptr<StuckEdges const> const before = book.Current();
        book.Push(report);
        check(book.Update(graph, nowMs, avoidMs) == 1, "d) the report flags no edge");
        std::shared_ptr<StuckEdges const> const flagged = book.Current();
        check(before->flags.empty() && flagged != before && flagged->flags.size() == 1 &&
                  flagged->flags.count(EdgeKey(flaggedFrom, flaggedTo)),
              "d) the book does not publish the flagged edge (the old snapshot must stay as it was)");
        Result const fromBook = Search(graph, toIronforge, *flagged, aStar);
        check(std::fabs(fromBook.totalSeconds - penalised.totalSeconds) <= 0.05f,
              "d) the book's flag costs differ from a hand-made flag");
        book.Update(graph, nowMs + avoidMs - 1000, avoidMs);
        check(book.Current()->flags.size() == 1, "d) the flag expires before the avoid window ends");
        book.Update(graph, nowMs + avoidMs + 1000, avoidMs);
        check(book.Current()->flags.empty(), "d) the flag does not expire after the avoid window");
        check(Search(graph, toIronforge, *book.Current(), aStar).totalSeconds == walked.totalSeconds,
              "d) the route is not back after the flag expired");
        book.Push({{0, 0.0f, 0.0f, 0.0f}, {0, 1.0f, 1.0f, 0.0f}});
        check(book.Update(graph, nowMs, avoidMs) == 0, "d) a destination off the graph flags an edge");
        // A follower's report names the edge: flagged although the positions match no node
        StuckBook exact;
        StuckBook::Report named;
        named.bot = {0, 0.0f, 0.0f, 0.0f};
        named.destination = {0, 1.0f, 1.0f, 0.0f};
        named.fromNode = flaggedFrom;
        named.toNode = flaggedTo;
        exact.Push(named);
        check(exact.Update(graph, nowMs, avoidMs) == 1 && exact.Current()->flags.size() == 1 &&
                  exact.Current()->flags.count(EdgeKey(flaggedFrom, flaggedTo)),
              "d) a report naming its edge does not flag exactly that edge");
        notes.push_back("d) flagged " + std::string(graph.GetName(flaggedFrom)) + " -> " +
                        std::string(graph.GetName(flaggedTo)) + ": " + num(penalised.totalSeconds) +
                        " s with one flag");
    }

    // e) Darnassus -> Stormwind leaves Kalimdor only by ship: no transport legs by default, one or more with
    //    allowTransport
    Snapshot elf = human;
    elf.position = darnassus;
    elf.hearth = darnassus;
    Request crossing{elf, stormwindInn};
    Result const stayed = Search(graph, crossing, StuckEdges{}, aStar);
    check(legCount(stayed, LegType::Transport) == 0, "e) a transport leg without allowTransport");
    notes.push_back(std::string("e) Darnassus -> Stormwind inn without transports: ") +
                    (stayed.ok ? describe(stayed) : FailureWord(stayed.failure)));
    crossing.snapshot.allowTransport = true;
    Result const sailed = Search(graph, crossing, StuckEdges{}, aStar);
    if (sound(sailed, crossing, "e) Darnassus -> Stormwind inn with transports"))
    {
        optimal(sailed, crossing, StuckEdges{}, "e)");
        check(legCount(sailed, LegType::Transport) >= 1, "e) no transport leg with allowTransport");
    }

    // f) Determinism and A* = Dijkstra on 50 random pairs (fixed seed), every flight node known, transports allowed,
    //    both teams, the hearth ready or cooling
    std::mt19937 random(20261109u);
    std::uniform_int_distribution<std::uint32_t> anyNode(0, static_cast<std::uint32_t>(graph.NodeCount() - 1));
    std::uint32_t pairs = 0;
    std::uint32_t mismatches = 0;
    std::uint32_t unstable = 0;
    std::uint64_t expandedAStar = 0;
    std::uint64_t expandedDijkstra = 0;
    double microsAStar = 0.0;
    double microsDijkstra = 0.0;
    using Clock = std::chrono::steady_clock;
    auto const micros = [](Clock::time_point since)
    { return std::chrono::duration<double, std::micro>(Clock::now() - since).count(); };
    for (std::uint32_t i = 0; i < 50; ++i)
    {
        TravelGraphNode const& a = graph.GetNode(anyNode(random));
        TravelGraphNode const& b = graph.GetNode(anyNode(random));
        Snapshot traveller = human;
        traveller.position = {a.mapId, a.x, a.y, a.z};
        traveller.team = static_cast<std::uint8_t>(i % 2);
        traveller.level = static_cast<std::uint8_t>(10 + (i * 7) % 71);
        traveller.money = 1000000;
        traveller.knownTaxi.fill(0xFFFFFFFFu);
        traveller.allowTransport = true;
        traveller.hearthCooldownMs = (i % 3) ? 0 : 20 * 60 * 1000;
        Request const pair{traveller, {b.mapId, b.x, b.y, b.z}};
        Clock::time_point const started = Clock::now();
        Result const first = Search(graph, pair, StuckEdges{}, aStar);
        microsAStar += micros(started);
        Result const again = Search(graph, pair, StuckEdges{}, aStar);
        Clock::time_point const reset = Clock::now();
        Result const reference = Search(graph, pair, StuckEdges{}, dijkstraParams);
        microsDijkstra += micros(reset);
        bool stable =
            first.ok == again.ok && first.totalSeconds == again.totalSeconds && first.legs.size() == again.legs.size();
        for (std::size_t leg = 0; stable && leg < first.legs.size(); ++leg)
            stable = first.legs[leg].nodes == again.legs[leg].nodes && first.legs[leg].type == again.legs[leg].type;
        unstable += stable ? 0 : 1;
        if (first.ok != reference.ok ||
            std::fabs(first.totalSeconds - reference.totalSeconds) > 0.001f * reference.totalSeconds + 0.05f)
        {
            ++mismatches;
            notes.push_back("f) mismatch: pair " + std::to_string(i) + " " + std::to_string(a.id) + " -> " +
                            std::to_string(b.id) + ": A* " + num(first.totalSeconds) + " s, Dijkstra " +
                            num(reference.totalSeconds) + " s");
        }
        pairs += first.ok ? 1 : 0;
        expandedAStar += first.expandedNodes;
        expandedDijkstra += reference.expandedNodes;
    }
    check(unstable == 0, "f) " + std::to_string(unstable) + " pairs gave different routes twice");
    check(mismatches == 0, "f) " + std::to_string(mismatches) + " A*/Dijkstra mismatches (see the notes)");
    check(pairs >= 25, "f) only " + std::to_string(pairs) + " of 50 random pairs have a route");
    notes.push_back("f) 50 random pairs: " + std::to_string(pairs) + " routed, " + std::to_string(mismatches) +
                    " A*/Dijkstra mismatches, expanded A* " + std::to_string(expandedAStar / 50) + " / Dijkstra " +
                    std::to_string(expandedDijkstra / 50) + " per search, " + num(microsAStar / 50.0) + " / " +
                    num(microsDijkstra / 50.0) + " us per search");

    // h) The fare budget. The core charges a taxi chain's total up front and every flight of a route draws on the same
    //    purse: find a route with a flight leg of several hops (every flight node known, thrift 0 so fares weigh no
    //    time and only the purse decides), then give the bot one copper less than the route's fares. Every hop alone
    //    is still affordable, yet the new route's flights must fit the purse. A route with two flight legs gets the
    //    same check when the random pairs find one
    auto const dearestHop = [&graph](Leg const& leg)
    {
        std::uint32_t dearest = 0;
        for (std::size_t i = 1; leg.type == LegType::Flight && i < leg.nodes.size(); ++i)
            for (TravelGraphLink const& link : graph.LinksOf(leg.nodes[i - 1]))
                if (link.to == leg.nodes[i])
                    if (TravelTaxiPath const* path = graph.TaxiPathOf(link))
                        dearest = std::max(dearest, path->price);
        return dearest;
    };
    Snapshot spender = human;
    spender.money = 1000000;
    spender.thrift = 0;
    spender.knownTaxi.fill(0xFFFFFFFFu);
    spender.hasHearthstone = false;
    std::mt19937 fareRandom(20261110u);
    bool chained = false;
    bool twoLegs = false;
    for (std::uint32_t i = 0; i < 600 && !(chained && twoLegs); ++i)
    {
        TravelGraphNode const& a = graph.GetNode(anyNode(fareRandom));
        TravelGraphNode const& b = graph.GetNode(anyNode(fareRandom));
        Request request{spender, {b.mapId, b.x, b.y, b.z}};
        request.snapshot.position = {a.mapId, a.x, a.y, a.z};
        request.snapshot.team = static_cast<std::uint8_t>(i % 2);
        Result const free = Search(graph, request, StuckEdges{}, aStar);
        std::uint32_t const total = RouteFare(free);
        std::uint32_t hop = 0;
        bool severalHops = false;
        for (Leg const& leg : free.legs)
        {
            hop = std::max(hop, dearestHop(leg));
            severalHops = severalHops || (leg.type == LegType::Flight && leg.taxiNodes.size() >= 3);
        }
        bool const several = legCount(free, LegType::Flight) >= 2;
        if (!free.ok || total <= hop || ((!severalHops || chained) && (!several || twoLegs)))
            continue;
        request.snapshot.money = total - 1;
        Result const tight = Search(graph, request, StuckEdges{}, aStar);
        bool legsFit = true;
        for (Leg const& leg : tight.legs)
            legsFit = legsFit && leg.fareCopper <= request.snapshot.money;
        std::string const label = std::string("h) ") + (severalHops ? "a chain of hops" : "") +
                                  (severalHops && several ? ", " : "") + (several ? "two flight legs" : "");
        check(legsFit && RouteFare(tight) <= request.snapshot.money,
              label + ": a route paying " + std::to_string(RouteFare(tight)) + "c from a purse of " +
                  std::to_string(request.snapshot.money) + "c");
        notes.push_back(label + " " + std::to_string(a.id) + " -> " + std::to_string(b.id) + ", dearest hop " +
                        std::to_string(hop) + "c, fares " + std::to_string(total) + "c: " + describe(free) +
                        " | purse " + std::to_string(request.snapshot.money) +
                        "c: " + (tight.ok ? describe(tight) : FailureWord(tight.failure)));
        chained = chained || severalHops;
        twoLegs = twoLegs || several;
    }
    check(chained, "h) no route with a flight leg of several hops among 600 random pairs");
    if (!twoLegs)
        notes.push_back("h) no route with two separate flight legs among the random pairs");
    // j) Start areas and avoided edges (WT10e-2): a level-1 bot in each race's start area walks to its first town (a
    //    route the graph lacks is noted), and an edge the trip avoids after deaths is not used
    struct StartArea
    {
        char const* name;
        std::uint8_t team;
        Point start;
        Point town;
    };
    StartArea const starts[] = {
        {"Northshire -> Goldshire", 0, {0, -8949.95f, -132.49f, 83.53f}, goldshire},
        {"Coldridge Valley -> Kharanos", 0, {0, -6240.32f, 331.03f, 382.76f}, {0, -5601.6f, -531.2f, 399.65f}},
        {"Shadowglen -> Dolanaar", 0, {1, 10311.3f, 832.46f, 1326.41f}, {1, 9886.0f, 974.0f, 1313.0f}},
        {"Ammen Vale -> Azure Watch", 0, {530, -3961.64f, -13931.2f, 100.62f}, {530, -4190.0f, -12470.0f, 44.0f}},
        {"Valley of Trials -> Razor Hill", 1, {1, -618.52f, -4251.67f, 38.72f}, {1, 320.0f, -4740.0f, 10.0f}},
        {"Camp Narache -> Bloodhoof Village", 1, {1, -2917.58f, -257.98f, 53.0f}, {1, -2300.0f, -400.0f, -9.0f}},
        {"Deathknell -> Brill", 1, {0, 1676.71f, 1678.31f, 121.67f}, {0, 2260.0f, 290.0f, 34.0f}},
        {"Sunstrider Isle -> Falconwing Square", 1, {530, 10349.6f, -6357.29f, 33.4f}, {530, 9500.0f, -6830.0f, 17.0f}},
    };
    for (StartArea const& area : starts)
    {
        Snapshot fresh;
        fresh.position = area.start;
        fresh.team = area.team;
        fresh.level = 1;
        Request const request{fresh, area.town};
        Result const route = Search(graph, request, StuckEdges{}, aStar);
        std::string const label = std::string("j) level 1, ") + area.name;
        if (route.ok)
            sound(route, request, label);
        else
            notes.push_back(label + ": no route (" + FailureWord(route.failure) + ")");
    }
    Snapshot veteran;
    veteran.position = {0, 2.7f, -857.9f, 58.9f};  // Tarren Mill
    veteran.team = 1;
    veteran.level = 80;
    veteran.money = 10000;
    Request const veteranTrip{veteran, {530, 9738.0f, -7454.0f, 13.6f}};  // Silvermoon
    Result const unbounded = Search(graph, veteranTrip, StuckEdges{}, aStar);
    if (sound(unbounded, veteranTrip, "j) level 80, Tarren Mill -> Silvermoon"))
    {
        Request avoiding = veteranTrip;
        for (Leg const& leg : unbounded.legs)
            if (leg.type == LegType::Walk && leg.nodes.size() >= 2 && avoiding.avoidEdges.empty())
                avoiding.avoidEdges.push_back({leg.nodes[1], leg.nodes[0]});  // given the other way round
        Result const around = Search(graph, avoiding, StuckEdges{}, aStar);
        bool used = false;
        for (Leg const& leg : around.legs)
            for (std::size_t i = 1; leg.type == LegType::Walk && i < leg.nodes.size(); ++i)
                used = used || avoiding.AvoidsEdge(leg.nodes[i - 1], leg.nodes[i]);
        check(!avoiding.avoidEdges.empty() && !used, "j) an avoided edge is used");
        notes.push_back("j) avoided edge: " +
                        (around.ok ? describe(around) : std::string(FailureWord(around.failure))));
    }

    // k) Fly legs (WU10) over made-up flat height maps (ground 100 yd, open but where named) around Hellfire Peninsula
    //    (Honor Hold -> Thrallmar) and the Borean Tundra (Valiance Keep -> Fizzcrank Airstrip); the same request's
    //    ground route is the reference
    auto const nodeNear = [&graph](std::uint32_t mapId, float x, float y, float z) -> Point
    {
        std::vector<std::uint32_t> const nearest = graph.NearestNodes(mapId, x, y, z, 1);
        if (nearest.empty())
            return {TravelGraph::ANY_MAP, 0.0f, 0.0f, 0.0f};
        TravelGraphNode const& node = graph.GetNode(nearest.front());
        return {node.mapId, node.x, node.y, node.z};
    };
    Point const honorHold = nodeNear(FLY_MAP_OUTLAND, -683.0f, 2657.0f, 92.0f);
    Point const thrallmar = nodeNear(FLY_MAP_OUTLAND, 228.0f, 2633.0f, 87.0f);
    Point const valianceKeep = nodeNear(FLY_MAP_NORTHREND, 2272.0f, 5200.0f, 11.0f);
    Point const fizzcrank = nodeNear(FLY_MAP_NORTHREND, 4126.0f, 5303.0f, 28.0f);
    if (!check(honorHold.mapId == FLY_MAP_OUTLAND && thrallmar.mapId == FLY_MAP_OUTLAND &&
                   valianceKeep.mapId == FLY_MAP_NORTHREND && fizzcrank.mapId == FLY_MAP_NORTHREND,
               "k) fixture nodes on Outland and Northrend"))
        return checks;
    auto const groundMap = [](Point const& a, Point const& b, std::function<bool(float, float)> const& openAt,
                              std::function<float(float, float)> const& groundAt)
    {
        float const margin = 1500.0f;
        float const cell = 67.0f;
        std::int32_t const width = static_cast<std::int32_t>(std::ceil((std::fabs(a.x - b.x) + 2.0f * margin) / cell));
        std::int32_t const height = static_cast<std::int32_t>(std::ceil((std::fabs(a.y - b.y) + 2.0f * margin) / cell));
        return FlightRules::MakeMap(a.mapId, std::min(a.x, b.x) - margin, std::min(a.y, b.y) - margin, cell, width,
                                    height, groundAt, openAt);
    };
    auto const flatMap = [&groundMap](Point const& a, Point const& b, std::function<bool(float, float)> const& openAt)
    { return groundMap(a, b, openAt, [](float, float) { return 100.0f; }); };
    auto const mapSet = [](std::vector<FlightRules::HeightMap> maps)
    {
        auto set = std::make_shared<FlightRules::FlightMapSet>();
        set->maps = std::move(maps);
        return std::shared_ptr<FlightRules::FlightMapSet const>(std::move(set));
    };
    auto const flyLeg = [](Result const& route) -> Leg const*
    {
        for (Leg const& leg : route.legs)
            if (leg.type == LegType::Fly)
                return &leg;
        return nullptr;
    };
    auto const open = [](float, float) { return true; };
    auto const awayFrom = [](Point const& at, float x, float y) { return std::hypot(x - at.x, y - at.y); };

    Snapshot rider;
    rider.position = honorHold;
    rider.team = 0;
    rider.level = 70;
    rider.flyOutland = true;
    rider.flySpeed = 17.5f;  // 150% flying (riding 225)
    rider.flightStyle = FlightRules::BotFlightStyle(0.0f, 1001);
    Request const hellfire{rider, thrallmar};
    Params flying;
    flying.flightMaps = mapSet({flatMap(honorHold, thrallmar, open)});
    Result const ground = Search(graph, hellfire, StuckEdges{}, aStar);
    Result const flownOver = Search(graph, hellfire, StuckEdges{}, flying);
    bool const routed = sound(ground, hellfire, "k) Honor Hold -> Thrallmar on the ground");
    if (!routed || !sound(flownOver, hellfire, "k) Honor Hold -> Thrallmar flying"))
        return checks;
    Leg const* fly = flyLeg(flownOver);
    check(fly && legCount(flownOver, LegType::Fly) == 1, "k) no single fly leg on an open map");
    check(flownOver.totalSeconds < ground.totalSeconds, "k) flying " + num(flownOver.totalSeconds) +
                                                            " s is not quicker than walking " +
                                                            num(ground.totalSeconds) + " s");
    check(fly && fly->flightPoints.size() >= 3 && same(fly->flightPoints.front(), fly->from) &&
              same(fly->flightPoints.back(), fly->to) && fly->flightLevels >= 1 && fly->flightYards > 0.0f &&
              fly->flightClearance == rider.flightStyle.clearanceYards,
          "k) the fly leg's flight does not run from its start to its end");
    check(flownOver.flightSearches >= 1 && flownOver.flightSearches <= FLY_SEARCHES_MAX,
          "k) " + std::to_string(flownOver.flightSearches) + " flight searches");
    notes.push_back("k) " + std::to_string(flownOver.flightSearches) + " flight searches in " +
                    num(flownOver.flightMicros) + " us");
    Params flyingDijkstra = flying;
    flyingDijkstra.heuristic = false;
    Result const flownReference = Search(graph, hellfire, StuckEdges{}, flyingDijkstra);
    check(std::fabs(flownReference.totalSeconds - flownOver.totalSeconds) <= 0.001f * flownOver.totalSeconds + 0.05f,
          "k) flying: A* " + num(flownOver.totalSeconds) + " s, Dijkstra " + num(flownReference.totalSeconds) + " s");
    // g) for fly legs: the same request twice gives the same legs and flights
    Result const flownAgain = Search(graph, hellfire, StuckEdges{}, flying);
    bool stable = flownAgain.legs.size() == flownOver.legs.size() && flownAgain.totalSeconds == flownOver.totalSeconds;
    for (std::size_t i = 0; stable && i < flownOver.legs.size(); ++i)
    {
        Leg const& a = flownOver.legs[i];
        Leg const& b = flownAgain.legs[i];
        stable = a.type == b.type && a.nodes == b.nodes && a.flightPoints.size() == b.flightPoints.size();
        for (std::size_t p = 0; stable && p < a.flightPoints.size(); ++p)
            stable = a.flightPoints[p].x == b.flightPoints[p].x && a.flightPoints[p].y == b.flightPoints[p].y &&
                     a.flightPoints[p].z == b.flightPoints[p].z;
    }
    check(stable, "k) the same request twice gave different fly legs");

    // A bot that cannot take off where it stands (indoors, in water, a WMO no-fly area over open cells) walks to the
    // leg's first graph node and flies from there
    Snapshot indoors = rider;
    indoors.takeoffHere = false;
    Request const fromInside{indoors, thrallmar};
    Result const walkedOut = Search(graph, fromInside, StuckEdges{}, flying);
    if (sound(walkedOut, fromInside, "k) Honor Hold indoors -> Thrallmar flying"))
    {
        Leg const* out = flyLeg(walkedOut);
        check(out && walkedOut.legs.front().type == LegType::Walk && same(walkedOut.legs.front().from, honorHold) &&
                  awayFrom(honorHold, out->from.x, out->from.y) > FLY_OWN_SPOT_YARDS &&
                  same(out->flightPoints.front(), out->from),
              "k) no takeoff here: the fly leg does not start at the first graph node after a walk");
        if (out)
            notes.push_back("k) no takeoff at Honor Hold: walk " + num(walkedOut.legs.front().seconds) +
                            " s, then fly " + num(out->seconds) + " s from " +
                            num(awayFrom(honorHold, out->from.x, out->from.y)) + " yd out");
    }

    // The replan after a takeoff refused at a node (WU11b: Dalaran's portal node lies over open cells): the bot
    // stands where its walk ended, 4 yd from the node, and no flight starts there. The trip runs from Thrallmar
    // indoors, whose walk passes two graph nodes after the refused one: the walk from Honor Hold has only one left,
    // 110 yd short of Thrallmar, a ground hop (since l)'s priced climb it no longer ends at the node 193 yd out)
    Snapshot inThrallmar = indoors;
    inThrallmar.position = thrallmar;
    Request const fromThrallmar{inThrallmar, honorHold};
    Result const refusedAt = Search(graph, fromThrallmar, StuckEdges{}, flying);
    Leg const* refused =
        sound(refusedAt, fromThrallmar, "k) Thrallmar indoors -> Honor Hold flying") ? flyLeg(refusedAt) : nullptr;
    check(refused && awayFrom(thrallmar, refused->from.x, refused->from.y) > FLY_OWN_SPOT_YARDS,
          "k) no takeoff in Thrallmar: no fly leg after a walk");
    if (refused)
    {
        Snapshot atNode = inThrallmar;
        atNode.position = {refused->from.mapId, refused->from.x + 4.0f, refused->from.y, refused->from.z};
        Request const replan{atNode, honorHold};
        Result const onward = Search(graph, replan, StuckEdges{}, flying);
        Leg const* again = sound(onward, replan, "k) replan at the refused node") ? flyLeg(onward) : nullptr;
        check(again && awayFrom(refused->from, again->from.x, again->from.y) > FLY_OWN_SPOT_YARDS,
              "k) a replan at a node where the takeoff was refused flies from it again");
        if (again)
            notes.push_back("k) replan at the refused node: fly " + num(again->seconds) + " s from " +
                            num(awayFrom(refused->from, again->from.x, again->from.y)) + " yd on");
    }

    // No-fly ground at each end: the start's own cell (the start 40 yd west of Honor Hold's node: the map's origin
    //    lies 1500 yd west of that node, so the start falls in the next cell west) and 100 yd around Thrallmar's node
    //    (the shipped graph's walk passes one node 110 yd out). Walk to the first open node, fly, walk in
    Point const westOfHold{FLY_MAP_OUTLAND, honorHold.x - 40.0f, honorHold.y, honorHold.z};
    Params towns;
    towns.flightMaps = mapSet({flatMap(honorHold, thrallmar,
                                       [&](float x, float y)
                                       {
                                           bool const startCell = std::fabs(x - westOfHold.x) <= 33.5f &&
                                                                  std::fabs(y - westOfHold.y) <= 33.5f;
                                           return !startCell && awayFrom(thrallmar, x, y) > 100.0f;
                                       })});
    Snapshot townRider = rider;
    townRider.position = westOfHold;
    Request const fromWest{townRider, thrallmar};
    Result const groundWest = Search(graph, fromWest, StuckEdges{}, aStar);
    Result const fromTown = Search(graph, fromWest, StuckEdges{}, towns);
    if (sound(groundWest, fromWest, "k) west of Honor Hold -> Thrallmar on the ground") &&
        sound(fromTown, fromWest, "k) no-fly ground at both ends"))
    {
        Leg const* townFly = flyLeg(fromTown);
        FlightRules::HeightMap const& townMap = towns.flightMaps->maps.front();
        check(fromTown.legs.size() >= 3 && fromTown.legs.front().type == LegType::Walk &&
                  fromTown.legs.back().type == LegType::Walk && legCount(fromTown, LegType::Fly) == 1 && townFly &&
                  townMap.IsOpen(townFly->from.x, townFly->from.y) && townMap.IsOpen(townFly->to.x, townFly->to.y),
              "k) no-fly ends: not walk, fly, walk with open fly ends");
        check(fromTown.totalSeconds < groundWest.totalSeconds, "k) no-fly ends: flying is not quicker");
    }

    // A floating platform (WU11b, Dalaran): a point more than FLY_PLATFORM_YARDS above its cell's ground is no fly
    // end. The ground lies 400 yd lower within platformYards of Thrallmar (cell centres: the walk's last graph node,
    // 110 yd out, stays on the plain since l)'s priced climb), so its nodes there float: the flight ends before them
    // and the bot walks the rest
    float const platformYards = 100.0f;
    FlightRules::HeightMap const plain = flatMap(honorHold, thrallmar, open);
    check(FlyEnd(plain, {FLY_MAP_OUTLAND, honorHold.x, honorHold.y, 120.0f}) &&
              !FlyEnd(plain, {FLY_MAP_OUTLAND, honorHold.x, honorHold.y, 100.0f + FLY_PLATFORM_YARDS + 1.0f}),
          "k) platform: a point 20 yd above the ground is no fly end, or one 101 yd above is one");
    Params platform;
    platform.flightMaps =
        mapSet({groundMap(honorHold, thrallmar, open, [&](float x, float y)
                          { return awayFrom(thrallmar, x, y) <= platformYards ? -400.0f : 100.0f; })});
    Result const toPlatform = Search(graph, hellfire, StuckEdges{}, platform);
    if (sound(toPlatform, hellfire, "k) Honor Hold -> Thrallmar on a platform"))
    {
        Leg const* high = flyLeg(toPlatform);
        FlightRules::HeightMap const& sunk = platform.flightMaps->maps.front();
        check(high && toPlatform.legs.back().type == LegType::Walk && FlyEnd(sunk, high->from) &&
                  FlyEnd(sunk, high->to) && awayFrom(thrallmar, high->to.x, high->to.y) > platformYards,
              "k) platform: the fly leg does not end below the platform before a walk up");
        if (high)
            notes.push_back("k) platform at Thrallmar: the flight ends " +
                            num(awayFrom(thrallmar, high->to.x, high->to.y)) + " yd out, " +
                            num(toPlatform.legs.back().seconds) + " s walk");
    }

    // Takeoff spots on that platform (WU11b fix 7, Krasus' Landing): a bot on the platform walks straight to the spot
    // 40 yd off Thrallmar and flies from it; a flight to the platform lands there and walks in. Without a spot, or with
    // the bot standing at the spot unable to take off, the flight starts beyond the platform
    Point const spotAt{FLY_MAP_OUTLAND, thrallmar.x + 40.0f, thrallmar.y, thrallmar.z};
    Params spotted;
    {
        FlightRules::HeightMap withSpot = platform.flightMaps->maps.front();
        withSpot.takeoffs.push_back({spotAt.x, spotAt.y, spotAt.z});
        spotted.flightMaps = mapSet({withSpot});
    }
    Snapshot onPlatform = rider;
    onPlatform.position = thrallmar;
    onPlatform.takeoffHere = false;
    Request const fromPlatform{onPlatform, honorHold};
    auto const viaSpot = [&](Result const& route, std::size_t walkIdx, std::size_t flyIdx)
    {
        return route.legs.size() > std::max(walkIdx, flyIdx) && route.legs[walkIdx].type == LegType::Walk &&
               route.legs[walkIdx].nodes.empty() && route.legs[flyIdx].type == LegType::Fly;
    };
    Result const offSpot = Search(graph, fromPlatform, StuckEdges{}, spotted);
    if (sound(offSpot, fromPlatform, "k) Thrallmar on a platform with a takeoff spot -> Honor Hold"))
        check(viaSpot(offSpot, 0, 1) && same(offSpot.legs[0].from, thrallmar) && same(offSpot.legs[0].to, spotAt) &&
                  same(offSpot.legs[1].from, spotAt),
              "k) takeoff spot: the bot does not walk straight to the spot and fly from it");
    Result const ontoSpot = Search(graph, hellfire, StuckEdges{}, spotted);
    if (sound(ontoSpot, hellfire, "k) Honor Hold -> Thrallmar on a platform with a takeoff spot"))
    {
        std::size_t const n = ontoSpot.legs.size();
        check(n >= 2 && viaSpot(ontoSpot, n - 1, n - 2) && same(ontoSpot.legs[n - 2].to, spotAt) &&
                  same(ontoSpot.legs[n - 1].from, spotAt) && same(ontoSpot.legs[n - 1].to, thrallmar),
              "k) takeoff spot: the flight onto the platform does not land at the spot and walk in");
    }
    auto const offBeyond = [&](Request const& request, Params const& params, std::string const& label)
    {
        Result const walked = Search(graph, request, StuckEdges{}, params);
        if (!sound(walked, request, "k) " + label))
            return;
        Leg const* off = flyLeg(walked);
        check(off && walked.legs.front().type == LegType::Walk && !walked.legs.front().nodes.empty() &&
                  awayFrom(thrallmar, off->from.x, off->from.y) > platformYards,
              "k) " + label + ": the flight does not start beyond the platform");
    };
    Request mayTakeOff = fromPlatform;
    mayTakeOff.snapshot.takeoffHere = true;
    offBeyond(mayTakeOff, platform, "a platform without a spot");
    Request atSpot = fromPlatform;
    atSpot.snapshot.position = {FLY_MAP_OUTLAND, spotAt.x + 3.0f, spotAt.y, spotAt.z};
    offBeyond(atSpot, spotted, "the bot at the spot, unable to take off");

    // Dalaran on the shipped graph (WU11b fix 7): the walk from Runeweaver Square to Wintergarde Keep steps down from
    // the city to Crystalsong Forest, which no bot can walk. Over a flat map (ground 150 yd) with the takeoff spot of
    // Krasus' Landing (taxi node 310) the bot walks there straight and flies from it; without the spot no fly leg
    // starts on the city's platform
    Point const runeweaver{FLY_MAP_NORTHREND, 5818.7f, 642.9f, 647.9f};
    Point const wintergarde{FLY_MAP_NORTHREND, 3712.4f, -694.9f, 215.4f};
    Point const krasus{FLY_MAP_NORTHREND, 5813.9f, 449.1f, 658.8f};
    Snapshot dalaran = rider;
    dalaran.position = runeweaver;
    dalaran.level = 77;
    dalaran.flyNorthrend = true;
    dalaran.takeoffHere = false;
    Request const fromDalaran{dalaran, wintergarde};
    FlightRules::HeightMap crystalsongMap =
        groundMap(runeweaver, wintergarde, open, [](float, float) { return 150.0f; });
    Params crystalsong;
    crystalsong.flightMaps = mapSet({crystalsongMap});
    crystalsongMap.takeoffs.push_back({krasus.x, krasus.y, krasus.z});
    Params landing;
    landing.flightMaps = mapSet({crystalsongMap});
    Result const dalaranBare = Search(graph, fromDalaran, StuckEdges{}, crystalsong);
    Result const dalaranFlown = Search(graph, fromDalaran, StuckEdges{}, landing);
    if (sound(dalaranBare, fromDalaran, "k) Runeweaver Square -> Wintergarde without a takeoff spot"))
    {
        Leg const* bare = flyLeg(dalaranBare);
        check(!bare || FlyEnd(crystalsongMap, bare->from),
              "k) Dalaran without a takeoff spot: a fly leg starts on the city's platform");
    }
    if (sound(dalaranFlown, fromDalaran, "k) Runeweaver Square -> Wintergarde via Krasus' Landing"))
    {
        check(viaSpot(dalaranFlown, 0, 1) && same(dalaranFlown.legs[0].from, runeweaver) &&
                  same(dalaranFlown.legs[0].to, krasus) && same(dalaranFlown.legs[1].from, krasus),
              "k) Dalaran: the bot does not walk to Krasus' Landing and fly from there");
        if (viaSpot(dalaranFlown, 0, 1))
            notes.push_back("k) Dalaran: walk " + num(dalaranFlown.legs[0].seconds) + " s to Krasus' Landing, fly " +
                            num(dalaranFlown.legs[1].seconds) + " s, " + num(dalaranFlown.totalSeconds) + " s in all");
    }

    // A hop under 150 yd stays on the ground, straight (90 yd) or through nodes (140 yd), without a flight search
    for (float const yards : {90.0f, 140.0f})
    {
        Request const hop{rider, {FLY_MAP_OUTLAND, honorHold.x + yards, honorHold.y, honorHold.z}};
        Result const hopped = Search(graph, hop, StuckEdges{}, flying);
        check(hopped.ok && legCount(hopped, LegType::Fly) == 0 && hopped.flightSearches == 0,
              "k) a hop of " + num(yards) + " yd is flown or searched");
    }

    // The fallbacks keep the ground route: noFly, no flying speed, no height map of the bot's map, a closed map, and a
    // goal on a void-ringed island (open within 150 yd, closed from 150 to 450 yd: the flight search finds no path)
    auto const walksAsGround =
        [&](Request const& request, Params const& params, std::string const& label, std::uint32_t searches)
    {
        Result const kept = Search(graph, request, StuckEdges{}, params);
        check(kept.ok && legCount(kept, LegType::Fly) == 0 && kept.flightSearches == searches &&
                  std::fabs(kept.totalSeconds - ground.totalSeconds) < 0.01f,
              "k) " + label + ": not the ground route (" + std::to_string(kept.flightSearches) + " searches)");
    };
    Request grounded = hellfire;
    grounded.noFly = true;
    walksAsGround(grounded, flying, "noFly", 0);
    Request slow = hellfire;
    slow.snapshot.flySpeed = 0.0f;
    walksAsGround(slow, flying, "no flying speed", 0);
    Params elsewhere;
    elsewhere.flightMaps = mapSet({flatMap(valianceKeep, fizzcrank, open)});
    walksAsGround(hellfire, elsewhere, "no Outland height map", 0);
    Params closed;
    closed.flightMaps = mapSet({flatMap(honorHold, thrallmar, [](float, float) { return false; })});
    walksAsGround(hellfire, closed, "a closed map", 0);
    Params ringed;
    ringed.flightMaps = mapSet({flatMap(honorHold, thrallmar,
                                        [&](float x, float y)
                                        {
                                            float const yards = awayFrom(thrallmar, x, y);
                                            return yards < 150.0f || yards > 450.0f;
                                        })});
    walksAsGround(hellfire, ringed, "a void-ringed goal", 1);

    // Northrend: without Cold Weather Flying the snapshot allows no flying there; with it the tundra is flown
    Params both;
    both.flightMaps = mapSet({flatMap(honorHold, thrallmar, open), flatMap(valianceKeep, fizzcrank, open)});
    Snapshot northerner = rider;
    northerner.position = valianceKeep;
    Request const borean{northerner, fizzcrank};
    Result const noColdWeather = Search(graph, borean, StuckEdges{}, both);
    if (sound(noColdWeather, borean, "k) Valiance Keep -> Fizzcrank without Cold Weather Flying"))
        check(legCount(noColdWeather, LegType::Fly) == 0 && noColdWeather.flightSearches == 0,
              "k) Northrend without Cold Weather Flying: a fly leg");
    Request coldWeather = borean;
    coldWeather.snapshot.flyNorthrend = true;
    Result const flownNorth = Search(graph, coldWeather, StuckEdges{}, both);
    if (sound(flownNorth, coldWeather, "k) Valiance Keep -> Fizzcrank with Cold Weather Flying") && noColdWeather.ok)
        check(legCount(flownNorth, LegType::Fly) == 1 && flownNorth.totalSeconds < noColdWeather.totalSeconds,
              "k) Northrend with Cold Weather Flying: no quicker fly leg");

    // l) No straight walk climbs a cliff (WT43): from Thunder Bluff's foot to its mesa, 53 yd across and 68 yd up, the
    //    way is not one walk leg; a gentle slope of the same length still is. From the elevator scenario's start (74 yd
    //    across, 66 yd up: inside the cutoff) the priced climb makes the elevator win.
    Point const bluffFoot{1, -1290.7f, 172.0f, 67.4f};
    Point const liftStart{1, -1279.9f, 195.38f, 69.44f};
    Point const bluffTop{1, -1274.74f, 121.51f, 134.98f};
    check(!StraightWalk(bluffFoot, bluffTop) && !StraightWalk(bluffTop, bluffFoot),
          "l) a 68 yd climb over 53 yd is a straight walk");
    check(StraightWalk(bluffFoot, {1, -1274.74f, 121.51f, 100.0f}), "l) a 33 yd rise over 53 yd is no straight walk");
    check(StraightWalk({1, 0.0f, 0.0f, 0.0f}, {1, 0.5f, 0.0f, 3.0f}), "l) a step beside a node is no straight walk");
    Snapshot climber;
    climber.position = bluffFoot;
    climber.team = 1;
    climber.level = 10;
    climber.money = 10000;
    climber.allowTransport = true;
    Request const climb{climber, bluffTop};
    Result const climbed = Search(graph, climb, StuckEdges{}, aStar);
    auto const nodeAt = [&graph](std::uint32_t index) -> Point
    {
        TravelGraphNode const& node = graph.GetNode(index);
        return {node.mapId, node.x, node.y, node.z};
    };
    bool cliffWalk = false;
    for (Leg const& leg : climbed.legs)
    {
        if (leg.type != LegType::Walk)
            continue;
        // The off-graph pieces of a walk leg: its start to its first node and its last node to its end
        Point const first = leg.nodes.empty() ? leg.to : nodeAt(leg.nodes.front());
        Point const last = leg.nodes.empty() ? leg.from : nodeAt(leg.nodes.back());
        cliffWalk = cliffWalk || !StraightWalk(leg.from, first) || !StraightWalk(last, leg.to);
    }
    check(!cliffWalk, "l) Thunder Bluff foot -> mesa walks straight up the cliff");
    notes.push_back("l) Thunder Bluff foot -> mesa: " +
                    (climbed.ok ? describe(climbed) : std::string(FailureWord(climbed.failure))));
    check(WalkSeconds(liftStart, bluffTop, 7.0f) > 2.0f * Distance(liftStart, bluffTop) / 7.0f,
          "l) a steep walk is priced as a level one");
    climber.position = liftStart;
    Result const lifted = Search(graph, Request{climber, bluffTop}, StuckEdges{}, aStar);
    bool const byLift = lifted.ok && std::any_of(lifted.legs.begin(), lifted.legs.end(),
                                                 [](Leg const& leg) { return leg.type == LegType::Transport; });
    check(byLift, "l) the elevator scenario's start walks straight up to the mesa");
    notes.push_back("l) elevator scenario start -> mesa: " +
                    (lifted.ok ? describe(lifted) : std::string(FailureWord(lifted.failure))));
    return checks;
}
}  // namespace TravelRoute

#endif
