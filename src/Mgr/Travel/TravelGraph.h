/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TRAVELGRAPH_H
#define _PLAYERBOT_TRAVELGRAPH_H

// The travel graph (playerbots_travelnode, _link and _path) as flat, index-based arrays. One loader thread
// fills it once after startup (TravelGraph.cpp); once IsReady() is true nothing writes it again, so any thread reads it
// without locks. Server-free on purpose: tests/travel-graph-selftest.cpp builds it from the shipped SQL files through
// the same TravelGraphBuilder and runs the same Selftest().

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

class ChatHandler;

// playerbots_travelnode_link.type (TravelNodePathType in TravelNode.h)
enum class TravelLinkType : std::uint8_t
{
    Walk = 1,         // one map; distance in yards
    Portal = 2,       // object = area trigger id
    Transport = 3,    // object = transport gameobject entry (ships, zeppelins, elevators); 0 = a wait point's link to
                      // its stop, distance = the walk between them
    Flight = 4,       // object = TaxiPath id
    StaticPortal = 6  // object = the gameobject entry of a portal a player clicks; loaded, not searched yet (WT55d)
};

constexpr std::size_t TRAVEL_LINK_TYPES = 7;  // TravelLinkType values are below this; 5 is unused

struct TravelGraphNode
{
    std::uint32_t id{0};  // playerbots_travelnode.id
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint32_t firstLink{0};  // the node's links are links[firstLink, firstLink + linkCount), sorted by `to`
    std::uint32_t linkCount{0};
    // A transport stop with a wait point: routes reach and leave it only through its wait point and its rides
    // (TravelGraph::OffWaitStop)
    bool waitStop{false};
};

struct TravelGraphLink
{
    std::uint32_t from{0};  // node indices
    std::uint32_t to{0};
    TravelLinkType type{TravelLinkType::Walk};
    // max_creature_0..2 as stored: creature levels near the path (the legacy annoyance data)
    std::uint8_t maxCreature[3]{};
    std::uint32_t object{0};
    float distance{0.0f};  // yards for walk links; 0.1 for the others
    float swim{0.0f};      // the swum part of distance
    // As stored: seconds for portals (3) and elevators (5, 8.6, 15); NOT seconds for flights (path length / 3600) and
    // ships or zeppelins (0.0033, 0.005)
    float extraCost{0.0f};
    float pathLength{0.0f};      // yards along the stored points, summed over segments on one map
    std::uint32_t pathFirst{0};  // points[pathFirst, pathFirst + pathCount), in nr order
    std::uint32_t pathCount{0};
};

struct TravelGraphPoint
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint32_t mapId{0};  // a portal's, transport's or flight's points can change map midway
};

// A TaxiPath (TaxiPath.dbc) as a flight link needs it: the loader resolves it from the DBC stores, which are loaded
// before the graph starts; the search reads it without the stores
struct TravelTaxiPath
{
    std::uint32_t from{0};  // taxi node ids (TaxiNodes.dbc), the bit layout of PlayerTaxi's mask
    std::uint32_t to{0};
    std::uint32_t price{0};  // copper
    std::uint8_t teams{0};   // TAXI_ALLIANCE | TAXI_HORDE: who has a flight master at both ends
};

// A node some portal, transport or flight link arrives at, with the cheapest of them: the route search's lower bound
// for reaching a map. Built once with the graph, one per node, sorted by map then node
struct TravelGraphArrival
{
    std::uint32_t mapId{0};
    std::uint32_t node{0};
    float fixedSeconds{std::numeric_limits<float>::infinity()};  // a portal's stored seconds, 0 for a transport
    float flightYards{std::numeric_limits<float>::infinity()};   // the shortest flight path onto it
};

struct TravelGraphStats
{
    std::uint32_t nodes{0};
    std::uint32_t links{0};
    std::uint32_t points{0};
    std::uint32_t linksByType[TRAVEL_LINK_TYPES]{};  // index = TravelLinkType value
    std::uint32_t waitPointLinks{0};                 // transport links without an object: a wait point and its stop
    std::uint32_t waitStops{0};                      // stops with a wait point (TravelGraphNode::waitStop)
    std::uint32_t skippedLinks{0};                   // unknown node, a link to itself or an unknown type
    std::uint32_t orphanPoints{0};                   // points of a link that is not in the link table
    std::size_t memoryBytes{0};                      // capacity of all arrays
    std::uint32_t loadMs{0};
    std::uint32_t peakChunkRows{0};       // the largest path chunk the loader held at once
    std::uint32_t taxiPaths{0};           // TaxiPath rows known to the graph
    std::uint32_t flightsWithoutTaxi{0};  // flight links whose TaxiPath is unknown: the search never uses them
};

struct TravelGraphRoute
{
    bool found{false};
    float seconds{0.0f};
    std::vector<std::uint32_t> links;  // link indices in travel order
};

class TravelGraphBuilder;

class TravelGraph
{
public:
    static constexpr std::uint32_t ANY_MAP = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t NO_NODE = std::numeric_limits<std::uint32_t>::max();
    static constexpr float GRID_CELL = 200.0f;  // yards per cell of the nearest-node index
    static constexpr std::uint8_t TAXI_ALLIANCE = 1;
    static constexpr std::uint8_t TAXI_HORDE = 2;

    // The simple cost of SimpleRoute, in seconds (WT6's reachability check; WT7's search has the real costs)
    static constexpr float RUN_SPEED = 7.0f;      // yd/s, a player running
    static constexpr float FLIGHT_SPEED = 32.0f;  // yd/s, worldserver.conf TaxiFlightSpeed (default, Blizzlike)
    // A ship or zeppelin, whose stored cost is no time: half a typical 4-6 min period of waiting plus a 2-3 min ride
    static constexpr float SHIP_SECONDS = 180.0f;

    TravelGraph() = default;
    TravelGraph(TravelGraph const&) = delete;
    TravelGraph& operator=(TravelGraph const&) = delete;

    ~TravelGraph()
    {
        _stop.store(true);
        if (_loader.joinable())
            _loader.join();
    }

    // Server side (TravelGraph.cpp): the loaded graph, its loader thread and the `playerbots route` commands
    static TravelGraph& Instance();
    void Start();     // once; later calls do nothing
    void Shutdown();  // stops a running load and joins the loader
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleSelftestCommand(ChatHandler* handler, char const* args);

    bool IsReady() const { return _state.load(std::memory_order_acquire) == State::Ready; }

    // Everything below reads the graph: call it only after IsReady()
    std::size_t NodeCount() const { return _nodes.size(); }
    std::size_t LinkCount() const { return _links.size(); }
    TravelGraphNode const& GetNode(std::uint32_t index) const { return _nodes[index]; }
    std::string_view GetName(std::uint32_t index) const { return _names[index]; }
    TravelGraphLink const& GetLink(std::uint32_t index) const { return _links[index]; }
    TravelGraphStats const& Stats() const { return _stats; }

    std::span<TravelGraphLink const> LinksOf(std::uint32_t index) const
    {
        TravelGraphNode const& node = _nodes[index];
        return {_links.data() + node.firstLink, node.linkCount};
    }

    std::span<TravelGraphPoint const> PathOf(TravelGraphLink const& link) const
    {
        return {_points.data() + link.pathFirst, link.pathCount};
    }

    std::uint32_t IndexOfLink(TravelGraphLink const& link) const
    {
        return static_cast<std::uint32_t>(&link - _links.data());
    }

    // The TaxiPath of a flight link, nullptr when the link is no flight or its path is unknown
    TravelTaxiPath const* TaxiPathOf(TravelGraphLink const& link) const
    {
        if (link.type != TravelLinkType::Flight || link.object >= _taxiPaths.size() || !_taxiPaths[link.object].from)
            return nullptr;
        return &_taxiPaths[link.object];
    }

    // The nodes on mapId that a portal, transport or flight link arrives at
    std::span<TravelGraphArrival const> ArrivalsOnto(std::uint32_t mapId) const
    {
        auto const first = std::lower_bound(_arrivals.begin(), _arrivals.end(), mapId,
                                            [](TravelGraphArrival const& arrival, std::uint32_t value)
                                            { return arrival.mapId < value; });
        auto const last =
            std::upper_bound(first, _arrivals.end(), mapId, [](std::uint32_t value, TravelGraphArrival const& arrival)
                             { return value < arrival.mapId; });
        return {_arrivals.data() + (first - _arrivals.begin()), static_cast<std::size_t>(last - first)};
    }

    // The node index of a playerbots_travelnode.id, NO_NODE when there is none
    std::uint32_t IndexOfId(std::uint32_t id) const { return id < _indexOfId.size() ? _indexOfId[id] : NO_NODE; }

    // Up to k node indices on mapId, nearest first (3D distance); a ring search over the 200 yd grid
    std::vector<std::uint32_t> NearestNodes(std::uint32_t mapId, float x, float y, float z, std::size_t k) const
    {
        std::vector<std::pair<float, std::uint32_t>> seen;  // squared distance, node
        std::uint64_t const mapFirst = CellKey(mapId, INT16_MIN, INT16_MIN);
        std::uint64_t const mapLast = CellKey(mapId, INT16_MAX, INT16_MAX);
        std::size_t const onMap =
            static_cast<std::size_t>(std::upper_bound(_cellKeys.begin(), _cellKeys.end(), mapLast) -
                                     std::lower_bound(_cellKeys.begin(), _cellKeys.end(), mapFirst));
        if (!k || !onMap)
            return {};

        std::int32_t const cx = CellOf(x);
        std::int32_t const cy = CellOf(y);
        auto const visit = [&](std::int32_t cellX, std::int32_t cellY)
        {
            std::uint64_t const key = CellKey(mapId, cellX, cellY);
            auto const range = std::equal_range(_cellKeys.begin(), _cellKeys.end(), key);
            for (auto it = range.first; it != range.second; ++it)
            {
                std::uint32_t const index = _cellNodes[static_cast<std::size_t>(it - _cellKeys.begin())];
                TravelGraphNode const& node = _nodes[index];
                float const dx = node.x - x;
                float const dy = node.y - y;
                float const dz = node.z - z;
                seen.emplace_back(dx * dx + dy * dy + dz * dz, index);
            }
        };

        // Ring r holds the cells at Chebyshev distance r; any node beyond it is at least r cells away in 2D
        for (std::int32_t ring = 0; seen.size() < onMap && ring <= MAX_RING; ++ring)
        {
            for (std::int32_t dx = -ring; dx <= ring; ++dx)
            {
                if (dx == -ring || dx == ring)
                {
                    for (std::int32_t dy = -ring; dy <= ring; ++dy)
                        visit(cx + dx, cy + dy);
                }
                else
                {
                    visit(cx + dx, cy - ring);
                    visit(cx + dx, cy + ring);
                }
            }

            if (seen.size() >= k)
            {
                std::nth_element(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(k - 1), seen.end());
                float const reach = static_cast<float>(ring) * GRID_CELL;
                if (seen[k - 1].first <= reach * reach)
                    break;
            }
        }

        std::sort(seen.begin(), seen.end());
        if (seen.size() > k)
            seen.resize(k);

        std::vector<std::uint32_t> result;
        result.reserve(seen.size());
        for (auto const& entry : seen)
            result.push_back(entry.second);
        return result;
    }

    // Node indices whose name contains part (ASCII case-insensitive), on mapId or on any map. A scan: commands only
    std::vector<std::uint32_t> FindByName(std::string_view part, std::uint32_t mapId = ANY_MAP) const
    {
        std::vector<std::uint32_t> result;
        for (std::uint32_t index = 0; index < _nodes.size(); ++index)
        {
            if (mapId != ANY_MAP && _nodes[index].mapId != mapId)
                continue;

            std::string_view const name = _names[index];
            auto const it = std::search(name.begin(), name.end(), part.begin(), part.end(),
                                        [](char a, char b) { return Lower(a) == Lower(b); });
            if (it != name.end() || part.empty())
                result.push_back(index);
        }
        return result;
    }

    // The FindByName match nearest to (x, y), NO_NODE when there is none
    std::uint32_t FindNear(std::string_view part, std::uint32_t mapId, float x, float y) const
    {
        std::uint32_t found = NO_NODE;
        float best = std::numeric_limits<float>::max();
        for (std::uint32_t const index : FindByName(part, mapId))
        {
            float const dx = _nodes[index].x - x;
            float const dy = _nodes[index].y - y;
            if (dx * dx + dy * dy < best)
            {
                best = dx * dx + dy * dy;
                found = index;
            }
        }
        return found;
    }

    // A transport link without an object joins a wait point to its stop: a walk, no ride
    static bool IsWaitPointLink(TravelGraphLink const& link)
    {
        return link.type == TravelLinkType::Transport && !link.object;
    }

    // A link other than a transport into or out of a stop with a wait point: no route uses it, the bot waits at the
    // wait point and crosses from there
    bool OffWaitStop(TravelGraphLink const& link) const
    {
        return link.type != TravelLinkType::Transport && (_nodes[link.from].waitStop || _nodes[link.to].waitStop);
    }

    // Negative for a link the simple search does not use
    static float SimpleCost(TravelGraphLink const& link)
    {
        switch (link.type)
        {
            case TravelLinkType::Walk:
                return link.distance / RUN_SPEED;
            case TravelLinkType::Portal:
                return link.extraCost;
            case TravelLinkType::Transport:
                if (!link.object)
                    return link.distance / RUN_SPEED;
                return link.extraCost >= 1.0f ? link.extraCost : SHIP_SECONDS;  // elevators store seconds
            case TravelLinkType::Flight:
                return link.pathLength / FLIGHT_SPEED;
            case TravelLinkType::StaticPortal:
                return -1.0f;
        }
        return link.extraCost;
    }

    // Dijkstra over SimpleCost, no bot state (flight nodes, faction, money): reachability, leg types and a plausible
    // total for the selftest. WT7 replaces it with the real search
    TravelGraphRoute SimpleRoute(std::uint32_t from, std::uint32_t to) const
    {
        TravelGraphRoute route;
        if (from >= _nodes.size() || to >= _nodes.size())
            return route;

        float const unreached = std::numeric_limits<float>::max();
        std::vector<float> best(_nodes.size(), unreached);
        std::vector<std::uint32_t> via(_nodes.size(), NO_NODE);
        using Entry = std::pair<float, std::uint32_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        best[from] = 0.0f;
        open.emplace(0.0f, from);
        while (!open.empty())
        {
            auto const [cost, node] = open.top();
            open.pop();
            if (cost > best[node])
                continue;
            if (node == to)
                break;

            for (TravelGraphLink const& link : LinksOf(node))
            {
                float const seconds = SimpleCost(link);
                if (seconds < 0.0f || OffWaitStop(link))
                    continue;
                float const next = cost + seconds;
                if (next < best[link.to])
                {
                    best[link.to] = next;
                    via[link.to] = IndexOfLink(link);
                    open.emplace(next, link.to);
                }
            }
        }

        if (best[to] == unreached)
            return route;

        for (std::uint32_t node = to; node != from; node = _links[via[node]].from)
            route.links.push_back(via[node]);
        std::reverse(route.links.begin(), route.links.end());
        route.found = true;
        route.seconds = best[to];
        return route;
    }

    // The checks shared by tests/travel-graph-selftest.cpp and `playerbots route selftest` on any graph: counts that
    // agree with each other, CSR consistency, the grid against a brute-force scan and three routes. Returns the number
    // of checks; failures get one line each, notes the route summaries
    std::uint32_t Selftest(std::vector<std::string>& failures, std::vector<std::string>& notes) const;

    // Sets the load figures and marks the graph ready; nothing writes it afterwards
    void Publish(std::uint32_t loadMs, std::uint32_t peakChunkRows)
    {
        _stats.loadMs = loadMs;
        _stats.peakChunkRows = peakChunkRows;
        _state.store(State::Ready, std::memory_order_release);
    }

private:
    friend class TravelGraphBuilder;

    enum class State : std::uint8_t
    {
        Idle,
        Loading,
        Ready,
        Failed,
        Stopped
    };

    static constexpr std::int32_t MAX_RING = 400;  // 80 km: past every map's extent

    static char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

    static std::int32_t CellOf(float coordinate)
    {
        float const cell = std::floor(coordinate / GRID_CELL);
        return static_cast<std::int32_t>(
            std::clamp(cell, static_cast<float>(INT16_MIN), static_cast<float>(INT16_MAX)));
    }

    static std::uint64_t CellKey(std::uint32_t mapId, std::int32_t cellX, std::int32_t cellY)
    {
        if (cellX < INT16_MIN || cellX > INT16_MAX || cellY < INT16_MIN || cellY > INT16_MAX)
            return std::numeric_limits<std::uint64_t>::max();  // no cell: matches nothing
        return (static_cast<std::uint64_t>(mapId) << 32) | (static_cast<std::uint64_t>(cellX - INT16_MIN) << 16) |
               static_cast<std::uint64_t>(cellY - INT16_MIN);
    }

    void BuildGrid()
    {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> cells;
        cells.reserve(_nodes.size());
        for (std::uint32_t index = 0; index < _nodes.size(); ++index)
        {
            TravelGraphNode const& node = _nodes[index];
            cells.emplace_back(CellKey(node.mapId, CellOf(node.x), CellOf(node.y)), index);
        }
        std::sort(cells.begin(), cells.end());
        _cellKeys.clear();
        _cellNodes.clear();
        for (auto const& cell : cells)
        {
            _cellKeys.push_back(cell.first);
            _cellNodes.push_back(cell.second);
        }
    }

    // A stop is a node with a ride (a transport link with an object); a wait-point link marks its stop ends
    void BuildWaitStops()
    {
        std::vector<bool> ride(_nodes.size(), false);
        for (TravelGraphLink const& link : _links)
            if (link.type == TravelLinkType::Transport && link.object)
                ride[link.from] = ride[link.to] = true;
        for (TravelGraphLink const& link : _links)
        {
            if (!IsWaitPointLink(link))
                continue;
            for (std::uint32_t const end : {link.from, link.to})
                if (ride[end] && !_nodes[end].waitStop)
                {
                    _nodes[end].waitStop = true;
                    ++_stats.waitStops;
                }
        }
    }

    void BuildArrivals()
    {
        _arrivals.clear();
        for (TravelGraphLink const& link : _links)
        {
            // No jump onto a map: walks, wait-point links on one map, and static portals the search leaves out
            if (link.type == TravelLinkType::Walk || link.type == TravelLinkType::StaticPortal ||
                (IsWaitPointLink(link) && _nodes[link.from].mapId == _nodes[link.to].mapId))
                continue;
            TravelGraphArrival arrival;
            arrival.mapId = _nodes[link.to].mapId;
            arrival.node = link.to;
            if (link.type == TravelLinkType::Portal)
                arrival.fixedSeconds = link.extraCost;
            else if (link.type == TravelLinkType::Transport)
                arrival.fixedSeconds = 0.0f;  // a ship's wait is a tuning value: no bound
            else
                arrival.flightYards = link.pathLength;
            _arrivals.push_back(arrival);
        }
        std::sort(_arrivals.begin(), _arrivals.end(), [](TravelGraphArrival const& a, TravelGraphArrival const& b)
                  { return a.mapId != b.mapId ? a.mapId < b.mapId : a.node < b.node; });
        std::size_t kept = 0;
        for (std::size_t index = 0; index < _arrivals.size(); ++index)
        {
            TravelGraphArrival const& arrival = _arrivals[index];
            if (kept && _arrivals[kept - 1].node == arrival.node)
            {
                TravelGraphArrival& merged = _arrivals[kept - 1];
                merged.fixedSeconds = std::min(merged.fixedSeconds, arrival.fixedSeconds);
                merged.flightYards = std::min(merged.flightYards, arrival.flightYards);
            }
            else
                _arrivals[kept++] = arrival;
        }
        _arrivals.resize(kept);
        _arrivals.shrink_to_fit();
    }

    std::size_t MemoryBytes() const
    {
        std::size_t bytes =
            _nodes.capacity() * sizeof(TravelGraphNode) + _links.capacity() * sizeof(TravelGraphLink) +
            _points.capacity() * sizeof(TravelGraphPoint) + _names.capacity() * sizeof(std::string) +
            _indexOfId.capacity() * sizeof(std::uint32_t) + _cellKeys.capacity() * sizeof(std::uint64_t) +
            _cellNodes.capacity() * sizeof(std::uint32_t) + _taxiPaths.capacity() * sizeof(TravelTaxiPath) +
            _arrivals.capacity() * sizeof(TravelGraphArrival);
        for (std::string const& name : _names)
            bytes += name.capacity() > 15 ? name.capacity() + 1 : 0;  // beyond the small-string buffer
        return bytes;
    }

    void Load();  // the loader thread (TravelGraph.cpp)

    std::vector<TravelGraphNode> _nodes;
    std::vector<std::string> _names;  // by node index
    std::vector<TravelGraphLink> _links;
    std::vector<TravelGraphPoint> _points;
    std::vector<std::uint32_t> _indexOfId;   // playerbots_travelnode.id -> node index
    std::vector<std::uint64_t> _cellKeys;    // sorted (map, cell x, cell y)
    std::vector<std::uint32_t> _cellNodes;   // the node of each key
    std::vector<TravelTaxiPath> _taxiPaths;  // by TaxiPath id; from 0 = no such path
    std::vector<TravelGraphArrival> _arrivals;
    TravelGraphStats _stats;

    std::atomic<State> _state{State::Idle};
    std::atomic<bool> _stop{false};
    std::thread _loader;
    std::string _error;  // why the load failed; written before _state turns Failed
};

// The one row sink of the graph: the loader thread (database rows) and tests/travel-graph-selftest.cpp (the shipped SQL
// files) feed it the same way: all nodes and links, EndLinks(), then the path points grouped by link in nr order (the
// tables' primary key order), then Finish() and Publish()
class TravelGraphBuilder
{
public:
    struct LinkRow
    {
        std::uint32_t fromId{0};
        std::uint32_t toId{0};
        std::uint32_t type{0};
        std::uint32_t object{0};
        float distance{0.0f};
        float swim{0.0f};
        float extraCost{0.0f};
        std::uint32_t maxCreature[3]{};
    };

    void AddNode(std::uint32_t id, std::string_view name, std::uint32_t mapId, float x, float y, float z)
    {
        _nodes.push_back({id, mapId, x, y, z, 0, 0});
        _names.emplace_back(name);
    }

    void AddLink(LinkRow const& row) { _linkRows.push_back(row); }

    // Orders the nodes by id, resolves the links to node indices and sorts them into per-node ranges
    bool EndLinks(std::string& error)
    {
        if (_nodes.empty())
        {
            error = "no travel nodes";
            return false;
        }

        std::vector<std::uint32_t> order(_nodes.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(),
                  [this](std::uint32_t a, std::uint32_t b) { return _nodes[a].id < _nodes[b].id; });
        std::vector<TravelGraphNode> nodes;
        std::vector<std::string> names;
        nodes.reserve(order.size());
        names.reserve(order.size());
        for (std::uint32_t const index : order)
        {
            if (!nodes.empty() && nodes.back().id == _nodes[index].id)
            {
                error = "travel node id " + std::to_string(_nodes[index].id) + " twice";
                return false;
            }
            nodes.push_back(_nodes[index]);
            names.push_back(std::move(_names[index]));
        }
        _nodes = std::move(nodes);
        _names = std::move(names);

        std::uint32_t const maxId = _nodes.back().id;
        if (maxId > MAX_NODE_ID)
        {
            error = "travel node id " + std::to_string(maxId) + " is out of range";
            return false;
        }
        _indexOfId.assign(static_cast<std::size_t>(maxId) + 1, TravelGraph::NO_NODE);
        for (std::uint32_t index = 0; index < _nodes.size(); ++index)
            _indexOfId[_nodes[index].id] = index;

        _links.reserve(_linkRows.size());
        for (LinkRow const& row : _linkRows)
        {
            std::uint32_t const from = row.fromId <= maxId ? _indexOfId[row.fromId] : TravelGraph::NO_NODE;
            std::uint32_t const to = row.toId <= maxId ? _indexOfId[row.toId] : TravelGraph::NO_NODE;
            if (from == TravelGraph::NO_NODE || to == TravelGraph::NO_NODE || from == to || !KnownType(row.type))
            {
                ++_stats.skippedLinks;
                continue;
            }

            TravelGraphLink link;
            link.from = from;
            link.to = to;
            link.type = static_cast<TravelLinkType>(row.type);
            for (std::size_t i = 0; i < 3; ++i)
                link.maxCreature[i] = static_cast<std::uint8_t>(std::min<std::uint32_t>(row.maxCreature[i], 255));
            link.object = row.object;
            link.distance = row.distance;
            link.swim = row.swim;
            link.extraCost = row.extraCost;
            _links.push_back(link);
            ++_stats.linksByType[row.type];
            if (TravelGraph::IsWaitPointLink(link))
                ++_stats.waitPointLinks;
        }
        _linkRows.clear();
        _linkRows.shrink_to_fit();
        if (_links.empty())
        {
            error = "no travel node links";
            return false;
        }

        std::sort(_links.begin(), _links.end(), [](TravelGraphLink const& a, TravelGraphLink const& b)
                  { return a.from != b.from ? a.from < b.from : a.to < b.to; });
        for (std::uint32_t index = 0; index < _links.size(); ++index)
        {
            TravelGraphNode& node = _nodes[_links[index].from];
            if (!node.linkCount)
                node.firstLink = index;
            ++node.linkCount;
        }
        _linksDone = true;
        return true;
    }

    void ReservePoints(std::size_t count) { _points.reserve(count); }

    // TaxiNodes.dbc: which team has a flight master there (MountCreatureID[1] Alliance, [0] Horde not 0)
    void AddTaxiNode(std::uint32_t id, bool alliance, bool horde)
    {
        if (id >= MAX_TAXI_ID)
            return;
        if (id >= _taxiNodeTeams.size())
            _taxiNodeTeams.resize(id + 1, 0);
        _taxiNodeTeams[id] = static_cast<std::uint8_t>((alliance ? TravelGraph::TAXI_ALLIANCE : 0) |
                                                       (horde ? TravelGraph::TAXI_HORDE : 0));
    }

    // TaxiPath.dbc, after the taxi nodes: a path is usable by a team with a flight master at both ends
    void AddTaxiPath(std::uint32_t id, std::uint32_t from, std::uint32_t to, std::uint32_t price)
    {
        if (id >= MAX_TAXI_ID || !from || !to)
            return;
        if (id >= _taxiPaths.size())
            _taxiPaths.resize(id + 1);
        auto const teams = [this](std::uint32_t node)
        { return node < _taxiNodeTeams.size() ? _taxiNodeTeams[node] : std::uint8_t{0}; };
        _taxiPaths[id] = {from, to, price, static_cast<std::uint8_t>(teams(from) & teams(to))};
    }

    // The largest node id after EndLinks()
    std::uint32_t MaxNodeId() const
    {
        return _indexOfId.empty() ? 0 : static_cast<std::uint32_t>(_indexOfId.size() - 1);
    }

    void AddPoint(std::uint32_t fromId, std::uint32_t toId, std::uint32_t nr, std::uint32_t mapId, float x, float y,
                  float z)
    {
        if (!_linksDone)
        {
            ++_disorderedPoints;
            return;
        }

        if (fromId != _currentFromId || toId != _currentToId)
        {
            _currentFromId = fromId;
            _currentToId = toId;
            _current = FindLink(fromId, toId);
            // A link's points come as one group: a link that already has points is out of order
            if (_current != NO_LINK && _links[_current].pathCount)
                _current = DISORDERED;
        }

        if (_current == NO_LINK)
        {
            ++_stats.orphanPoints;
            return;
        }

        if (_current == DISORDERED || nr != _links[_current].pathCount)
        {
            ++_disorderedPoints;
            return;
        }

        TravelGraphLink& link = _links[_current];
        if (!link.pathCount)
            link.pathFirst = static_cast<std::uint32_t>(_points.size());
        else if (_points.back().mapId == mapId)
        {
            TravelGraphPoint const& last = _points.back();
            float const dx = x - last.x;
            float const dy = y - last.y;
            float const dz = z - last.z;
            link.pathLength += std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        _points.push_back({x, y, z, mapId});
        ++link.pathCount;
    }

    // Moves the arrays into the graph and builds its grid; the caller publishes it
    bool Finish(TravelGraph& graph, std::string& error)
    {
        if (!_linksDone)
        {
            error = "links not ended";
            return false;
        }
        if (_points.empty())
        {
            error = "no travel node path points";
            return false;
        }
        if (_disorderedPoints)
        {
            error = std::to_string(_disorderedPoints) + " path points out of order";
            return false;
        }

        _points.shrink_to_fit();
        for (TravelTaxiPath const& path : _taxiPaths)
            _stats.taxiPaths += path.from ? 1 : 0;
        for (TravelGraphLink const& link : _links)
            if (link.type == TravelLinkType::Flight &&
                (link.object >= _taxiPaths.size() || !_taxiPaths[link.object].from))
                ++_stats.flightsWithoutTaxi;
        _stats.nodes = static_cast<std::uint32_t>(_nodes.size());
        _stats.links = static_cast<std::uint32_t>(_links.size());
        _stats.points = static_cast<std::uint32_t>(_points.size());
        graph._nodes = std::move(_nodes);
        graph._names = std::move(_names);
        graph._links = std::move(_links);
        graph._points = std::move(_points);
        graph._indexOfId = std::move(_indexOfId);
        graph._taxiPaths = std::move(_taxiPaths);
        graph._stats = _stats;
        graph.BuildGrid();
        graph.BuildWaitStops();
        graph.BuildArrivals();
        graph._stats.memoryBytes = graph.MemoryBytes();
        return true;
    }

private:
    static constexpr std::uint32_t MAX_NODE_ID = 10000000;
    static constexpr std::uint32_t MAX_TAXI_ID = 100000;
    static constexpr std::uint32_t NO_LINK = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t DISORDERED = NO_LINK - 1;

    static bool KnownType(std::uint32_t type)
    {
        return type == static_cast<std::uint32_t>(TravelLinkType::Walk) ||
               type == static_cast<std::uint32_t>(TravelLinkType::Portal) ||
               type == static_cast<std::uint32_t>(TravelLinkType::Transport) ||
               type == static_cast<std::uint32_t>(TravelLinkType::Flight) ||
               type == static_cast<std::uint32_t>(TravelLinkType::StaticPortal);
    }

    std::uint32_t FindLink(std::uint32_t fromId, std::uint32_t toId) const
    {
        if (fromId >= _indexOfId.size() || toId >= _indexOfId.size())
            return NO_LINK;
        std::uint32_t const from = _indexOfId[fromId];
        std::uint32_t const to = _indexOfId[toId];
        if (from == TravelGraph::NO_NODE || to == TravelGraph::NO_NODE)
            return NO_LINK;

        TravelGraphNode const& node = _nodes[from];
        auto const first = _links.begin() + node.firstLink;
        auto const last = first + node.linkCount;
        auto const it = std::lower_bound(
            first, last, to, [](TravelGraphLink const& link, std::uint32_t value) { return link.to < value; });
        return it != last && it->to == to ? static_cast<std::uint32_t>(it - _links.begin()) : NO_LINK;
    }

    std::vector<TravelGraphNode> _nodes;
    std::vector<std::string> _names;
    std::vector<LinkRow> _linkRows;
    std::vector<TravelGraphLink> _links;
    std::vector<TravelGraphPoint> _points;
    std::vector<std::uint32_t> _indexOfId;
    std::vector<std::uint8_t> _taxiNodeTeams;  // by taxi node id
    std::vector<TravelTaxiPath> _taxiPaths;
    TravelGraphStats _stats;
    bool _linksDone{false};
    std::uint32_t _currentFromId{TravelGraph::NO_NODE};
    std::uint32_t _currentToId{TravelGraph::NO_NODE};
    std::uint32_t _current{NO_LINK};
    std::uint32_t _disorderedPoints{0};
};

// Any graph: the shipped one (data/sql/playerbots/base/playerbots_travelnode*.sql plus its updates) or one that
// replaces the three tables whole (the navigation plan's builds)
inline std::uint32_t TravelGraph::Selftest(std::vector<std::string>& failures, std::vector<std::string>& notes) const
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
    auto const expect = [&check](std::size_t actual, std::size_t wanted, char const* what)
    {
        return check(actual == wanted,
                     std::string(what) + " " + std::to_string(actual) + " (expected " + std::to_string(wanted) + ")");
    };

    if (!check(IsReady(), "graph ready"))
        return checks;

    // 1. The counts agree: every link has a known type, every point a link
    std::size_t typed = 0;
    for (std::uint32_t const count : _stats.linksByType)
        typed += count;
    check(!_nodes.empty() && !_links.empty() && !_points.empty(), "a graph with nodes, links and points");
    expect(typed, _links.size(), "links by type");
    expect(_stats.linksByType[0] + _stats.linksByType[5], 0, "links of no type");
    expect(_stats.waitPointLinks,
           static_cast<std::size_t>(std::count_if(_links.begin(), _links.end(), IsWaitPointLink)), "wait-point links");
    expect(_stats.skippedLinks, 0, "skipped links");
    expect(_stats.orphanPoints, 0, "points without a link");

    // 2. Per-node link ranges: contiguous, sorted by target, in range, no link to itself; point ranges disjoint
    std::size_t badLinks = 0;
    std::size_t nextLink = 0;
    std::size_t pointsUsed = 0;
    std::size_t badPaths = 0;
    std::size_t flightsWithoutLength = 0;
    std::size_t thundercaller = 0;
    std::size_t waitStops = 0;
    for (std::uint32_t index = 0; index < _nodes.size(); ++index)
    {
        TravelGraphNode const& node = _nodes[index];
        if (node.linkCount && node.firstLink != nextLink)
            ++badLinks;
        waitStops += node.waitStop ? 1 : 0;
        nextLink += node.linkCount;
        std::uint32_t previousTo = NO_NODE;
        for (TravelGraphLink const& link : LinksOf(index))
        {
            if (link.from != index || link.to >= _nodes.size() || link.to == index ||
                (previousTo != NO_NODE && previousTo >= link.to))
                ++badLinks;
            previousTo = link.to;
            if (static_cast<std::size_t>(link.pathFirst) + link.pathCount > _points.size())
                ++badPaths;
            pointsUsed += link.pathCount;
            if (link.type == TravelLinkType::Flight && !(link.pathLength > 0.0f))
                ++flightsWithoutLength;
            // The zeppelin between Orgrimmar and Tirisfal Glades (The Thundercaller), both ways
            if (link.type == TravelLinkType::Transport && link.object == 164871)
                ++thundercaller;
        }
    }
    expect(nextLink, _links.size(), "links in node ranges");
    expect(badLinks, 0, "links out of place");
    expect(badPaths, 0, "point ranges out of bounds");
    expect(pointsUsed, _points.size(), "points in link ranges");
    expect(flightsWithoutLength, 0, "flight links without a path length");
    expect(waitStops, _stats.waitStops, "stops with a wait point");
    check(thundercaller >= 2, "The Thundercaller rides " + std::to_string(thundercaller) + " (expected both ways)");
    char const* const typeNames[TRAVEL_LINK_TYPES] = {"none",   "walk",   "portal",       "transport",
                                                      "flight", "type 5", "static portal"};
    std::string counts = "links by type:";
    for (std::size_t type = 1; type < TRAVEL_LINK_TYPES; ++type)
        if (_stats.linksByType[type])
            counts += std::string(" ") + typeNames[type] + " " + std::to_string(_stats.linksByType[type]);
    notes.push_back(counts + "; wait-point links " + std::to_string(_stats.waitPointLinks) +
                    ", stops with a wait point " + std::to_string(_stats.waitStops));

    // 3. The grid answers like a scan over the map's nodes (squared distances of the k nearest)
    std::size_t gridMisses = 0;
    std::size_t gridQueries = 0;
    for (std::uint32_t index = 0; index < _nodes.size(); index += 37)
    {
        TravelGraphNode const& node = _nodes[index];
        float const x = node.x + 37.5f;
        float const y = node.y - 23.25f;
        float const z = node.z + 4.0f;
        auto const squared = [x, y, z](TravelGraphNode const& other)
        {
            float const dx = other.x - x;
            float const dy = other.y - y;
            float const dz = other.z - z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::vector<float> scan;
        for (TravelGraphNode const& other : _nodes)
            if (other.mapId == node.mapId)
                scan.push_back(squared(other));
        std::sort(scan.begin(), scan.end());
        std::vector<std::uint32_t> const nearest = NearestNodes(node.mapId, x, y, z, 4);
        ++gridQueries;
        bool same = nearest.size() == std::min<std::size_t>(4, scan.size());
        for (std::size_t i = 0; same && i < nearest.size(); ++i)
            same = _nodes[nearest[i]].mapId == node.mapId && squared(_nodes[nearest[i]]) == scan[i];
        if (!same)
            ++gridMisses;
    }
    check(gridMisses == 0,
          "nearest nodes differ from the scan in " + std::to_string(gridMisses) + " of " + std::to_string(gridQueries));

    // 4. Three routes over the simple cost. Places by name, nearest to a known position (names are not unique)
    struct Place
    {
        char const* name;
        std::uint32_t mapId;
        float x;
        float y;
    };
    struct RouteCase
    {
        char const* label;
        Place from;
        Place to;
        TravelLinkType mustUse;
        std::uint32_t mustObject;  // the object of a mustUse link, 0 = any
        float minSeconds;
        float maxSeconds;
    };
    // Ranges (simple cost: run 7 yd/s, fly 32 yd/s, ship or zeppelin 180 s, portal 3 s; the shipped graph gives 373,
    // 468 and 967 s):
    // - Goldshire -> Ironforge: about 1200 yd of running to Stormwind's flight master (170 s) and the 6500 yd flight
    //   (200 s; about 3.5 min in the game); on foot through the Deeprun Tram the walk alone takes more than 5 min
    // - Orgrimmar -> Undercity: about 1900 yd of running (inn to tower, tower to inn with the Undercity elevators) and
    //   The Thundercaller (180 s)
    // - Darnassus -> Stormwind: the portal to Rut'theran Village, at least one ship and long flights; the shipped graph
    //   has no walk link onto The Bravery at Auberdine, so its route flies to Ratchet and sails to Booty Bay (The
    //   Maiden's Fancy). More than 30 min would be a detour
    RouteCase const cases[] = {
        {"Goldshire -> Ironforge",
         {"Goldshire innkeeper", 0, -9462.7f, 16.2f},
         {"flightMaster", 0, -4821.8f, -1152.4f},
         TravelLinkType::Flight,
         0,
         250.0f,
         600.0f},
        {"Orgrimmar -> Undercity",
         {"Orgrimmar innkeeper", 1, 1633.9f, -4439.4f},
         {"Undercity innkeeper", 0, 1637.0f, 220.0f},
         TravelLinkType::Transport,
         164871,
         300.0f,
         900.0f},
        {"Darnassus -> Stormwind",
         {"Darnassus", 1, 9947.0f, 2482.0f},
         {"Stormwind City innkeeper", 0, -8867.8f, 673.7f},
         TravelLinkType::Transport,
         0,
         600.0f,
         1800.0f},
    };

    for (RouteCase const& route : cases)
    {
        std::uint32_t const from = FindNear(route.from.name, route.from.mapId, route.from.x, route.from.y);
        std::uint32_t const to = FindNear(route.to.name, route.to.mapId, route.to.x, route.to.y);
        if (!check(from != NO_NODE && to != NO_NODE, std::string(route.label) + ": places not found"))
            continue;

        TravelGraphRoute const found = SimpleRoute(from, to);
        if (!check(found.found, std::string(route.label) + ": no route"))
            continue;

        std::uint32_t legs[TRAVEL_LINK_TYPES]{};
        float yards[TRAVEL_LINK_TYPES]{};
        bool usedObject = !route.mustObject;
        std::string chain;
        float sum = 0.0f;
        bool chained = true;
        std::uint32_t at = from;
        for (std::uint32_t const linkIndex : found.links)
        {
            TravelGraphLink const& link = _links[linkIndex];
            chained = chained && link.from == at;
            at = link.to;
            sum += SimpleCost(link);
            std::size_t const type = static_cast<std::size_t>(link.type);
            ++legs[type];
            yards[type] += link.type == TravelLinkType::Walk ? link.distance : link.pathLength;
            usedObject = usedObject || (link.type == route.mustUse && link.object == route.mustObject);
            if (link.type != TravelLinkType::Walk)
                chain += std::string("; ") + typeNames[type] + " " + std::to_string(link.object) + " " +
                         std::string(GetName(link.from)) + " -> " + std::string(GetName(link.to));
        }
        check(chained && at == to, std::string(route.label) + ": links do not chain from start to goal");
        check(std::fabs(sum - found.seconds) <= 0.01f * found.seconds + 0.1f,
              std::string(route.label) + ": cost " + num(found.seconds) + " s is not the sum of its links " + num(sum));
        check(legs[static_cast<std::size_t>(route.mustUse)] > 0,
              std::string(route.label) + ": no " + typeNames[static_cast<std::size_t>(route.mustUse)] + " link");
        check(usedObject, std::string(route.label) + ": no link of object " + std::to_string(route.mustObject));
        check(found.seconds >= route.minSeconds && found.seconds <= route.maxSeconds,
              std::string(route.label) + ": " + num(found.seconds) + " s outside " + num(route.minSeconds) + ".." +
                  num(route.maxSeconds) + " s");

        std::string note = std::string(route.label) + ": " + std::string(GetName(from)) + " (" +
                           std::to_string(_nodes[from].id) + ") -> " + std::string(GetName(to)) + " (" +
                           std::to_string(_nodes[to].id) + "), " + num(found.seconds) + " s over " +
                           std::to_string(found.links.size()) + " links:";
        for (std::size_t type = 1; type < TRAVEL_LINK_TYPES; ++type)
            if (legs[type])
                note += std::string(" ") + typeNames[type] + " " + std::to_string(legs[type]) + " (" +
                        num(yards[type]) + " yd)";
        notes.push_back(note + chain);
    }

    return checks;
}

#endif
