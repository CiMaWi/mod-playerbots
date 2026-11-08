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
//   (Params::transportWaitSeconds, half a period) plus SHIP_RIDE_SECONDS
// - hearthstone: 10 s cast plus the remaining cooldown, from the bot's position to its bind point. Only from the start:
//   using it later arrives no earlier (max(cooldown, t) + 10 s is never less than cooldown + 10 s)
// - an edge flagged by Stuck events costs Params::stuckPenalty times more, and is skipped after STUCK_SKIP_FLAGS

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
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "TravelGraph.h"

namespace TravelRoute
{
constexpr std::size_t TAXI_MASK_SIZE = 14;       // TaxiMaskSize (DBCStructure.h): 448 taxi nodes, PlayerTaxi's layout
constexpr std::size_t END_NODES = 5;             // graph nodes the bot, the goal and the bind point connect to
constexpr float DIRECT_YARDS = 100.0f;           // start and goal this close on one map may also be walked directly
constexpr float HEARTH_CAST_SECONDS = 10.0f;     // the hearthstone's cast
constexpr float FLIGHT_BOARDING_SECONDS = 5.0f;  // talking to the flight master, mounting and landing
constexpr float SHIP_RIDE_SECONDS = 120.0f;      // a ship or zeppelin crossing until WT19 knows the real periods
constexpr float FULL_PURSE_SECONDS = 3600.0f;    // at thrift 100, a fare of all one's money weighs an hour of travel
constexpr std::uint16_t STUCK_SKIP_FLAGS = 3;    // an edge flagged this often within the avoid window is skipped
constexpr float STUCK_NODE_YARDS = 10.0f;        // a stuck walk's destination this close to a node was a graph leg
constexpr std::size_t STUCK_QUEUE_MAX = 4096;    // reports waiting for the world thread; more are dropped
constexpr std::uint32_t NO_LINK = std::numeric_limits<std::uint32_t>::max();
constexpr float UNREACHED = std::numeric_limits<float>::infinity();

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
    Snapshot snapshot;
    Point goal;
};

// Tuning values (WorldTravelConfig) and the selftest's switch
struct Params
{
    float stuckPenalty{5.0f};            // Playerbots.WorldTravel.StuckPenalty
    float transportWaitSeconds{120.0f};  // Playerbots.WorldTravel.TransportWaitSeconds
    float flightSpeed{TravelGraph::FLIGHT_SPEED};
    bool heuristic{true};  // false: plain Dijkstra over the same costs (the selftest's reference)
};

enum class LegType : std::uint8_t
{
    Walk,
    Portal,
    Flight,
    Transport,
    Hearth
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
    std::vector<std::uint32_t> nodes;      // graph node indices passed, in order (walk waypoints; the ends otherwise)
    std::uint32_t object{0};               // area trigger, first TaxiPath or transport entry
    std::vector<std::uint32_t> taxiNodes;  // flight legs: the taxi node chain for Player::ActivateTaxiPathTo
    float seconds{0.0f};
    std::uint32_t fareCopper{0};
};

struct Result
{
    bool ok{false};
    Failure failure{Failure::None};
    std::vector<Leg> legs;  // consecutive walk links merge into one leg, consecutive flights into one
    float totalSeconds{0.0f};
    std::uint32_t expandedNodes{0};
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
    std::uint32_t generation{0};  // counts the book's publications: a cached route is only as good as its flags
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
// way that is slower but cheaper in copper can be missed when the purse is tight (then the route walks more)
inline Result Search(TravelGraph const& graph, Request const& request, StuckEdges const& stuck, Params const& params)
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
    std::vector<std::uint32_t> const startNodes = graph.NearestNodes(start.mapId, start.x, start.y, start.z, END_NODES);
    std::vector<std::uint32_t> const goalNodes = graph.NearestNodes(goal.mapId, goal.x, goal.y, goal.z, END_NODES);
    bool const direct = start.mapId == goal.mapId && Distance(start, goal) <= DIRECT_YARDS;
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
    bool const hearth = bot.hasHearthstone && (!bindNodes.empty() || bot.hearth.mapId == goal.mapId);

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
    { relax(from, to, Distance(pointOf(from), pointOf(to)) / runSpeed, NO_LINK, Step::Walk, 0); };

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
            if (pointOf(vertex).mapId == goal.mapId && Distance(pointOf(vertex), goal) <= DIRECT_YARDS)
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
            if (seconds < 0.0f)
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
        bool const merge = !result.legs.empty() && result.legs.back().type == type &&
                           (type == LegType::Walk || type == LegType::Flight);
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

    result.ok = true;
    result.totalSeconds = best[target];
    // One label per vertex: a vertex reopened with a new fare total can leave a successor's way through it on the old
    // total, so the rebuilt route may cost more than the purse. Then the route goes without paid flights (no loop: with
    // no money every paid flight link is pruned)
    if (RouteFare(result) > bot.money)
    {
        Request walking = request;
        walking.snapshot.money = 0;
        return Search(graph, walking, stuck, params);
    }
    return result;
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
            edges->generation = ++_generation;
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
    std::uint32_t _generation{0};
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
    return checks;
}
}  // namespace TravelRoute

#endif
