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

#include "TravelCrossingTypes.h"

class ChatHandler;

// playerbots_travelnode_link.type (TravelNodePathType in TravelNode.h)
enum class TravelLinkType : std::uint8_t
{
    Walk = 1,          // one map; distance in yards
    Portal = 2,        // object = area trigger id
    Transport = 3,     // object = transport gameobject entry (ships, zeppelins, elevators); 0 = a wait point's link to
                       // its stop, distance = the walk between them
    Flight = 4,        // object = TaxiPath id
    StaticPortal = 6,  // object = the gameobject entry of a portal a player clicks (a static portal leg, WT55d)
    Air = 7            // one map, only a bot that may fly (WT61): distance = the straight yards, no path rows
};

constexpr std::size_t TRAVEL_LINK_TYPES = 8;  // TravelLinkType values are below this; 5 is unused

// Both sides (TravelGraph::TAXI_ALLIANCE | TravelGraph::TAXI_HORDE): who may use a link without a side
constexpr std::uint8_t TRAVEL_BOTH_TEAMS = 3;

// What the world database says of a portal link's use (PA38c; G145, G148, G333, G334, G337), read from the core's
// stores when the graph loads (TravelGraph::ReadPortalFacts). Plain data, so the gate below can be tried offline
struct TravelPortalFacts
{
    bool trigger{false};  // an area trigger link (type 2); else a static portal object (type 6)
    // An area trigger: it exists, its map, its areatrigger_teleport row, and whether it is bound to the core's generic
    // SmartTrigger script (that script takes every living player's trigger before the row is read: the row never
    // teleports)
    bool known{false};
    std::uint32_t triggerMap{0};
    bool hasRow{false};
    bool scripted{false};
    std::uint32_t targetMap{0};  // the row's target
    float targetX{0.0f};
    float targetY{0.0f};
    bool raid{false};           // the target map is a raid: the core asks for a raid group
    bool keyed{false};          // the target map's access asks for an item, quest or achievement
    std::uint32_t minLevel{0};  // a trigger: the target map's access level; an object: its spell's level condition
    std::uint8_t teams{TRAVEL_BOTH_TEAMS};  // an object: the sides its faction does not refuse
};

// Why a portal link stays out of the graph (Usable: it stays in)
enum class TravelPortalUse : std::uint8_t
{
    Usable,
    NoTrigger,     // no such area trigger
    OtherMap,      // the trigger is not on the link's from map
    NoRow,         // no areatrigger_teleport row: a script teleports, if anything (G333)
    Scripted,      // bound to SmartTrigger: the row is never reached (G337)
    WrongArrival,  // the row's target is not where the leg ends (G334)
    Count
};

inline char const* PortalUseWords(TravelPortalUse use)
{
    switch (use)
    {
        case TravelPortalUse::Usable:
            return "usable";
        case TravelPortalUse::NoTrigger:
            return "no such trigger";
        case TravelPortalUse::OtherMap:
            return "trigger on another map";
        case TravelPortalUse::NoRow:
            return "no teleport row";
        case TravelPortalUse::Scripted:
            return "SmartTrigger bound";
        case TravelPortalUse::WrongArrival:
            return "row target not the far node";
        default:
            return "?";
    }
}

struct TravelPortalGate
{
    TravelPortalUse use{TravelPortalUse::Usable};
    std::uint8_t teams{TRAVEL_BOTH_TEAMS};  // 0: no bot the search knows (a raid group or a key is needed)
    std::uint8_t level{0};                  // the lowest level the core lets use it
};

// The gate of one portal link: whether the link can work at all, and for whom. exitYards: on one map the leg ends
// within this of the far node (TravelLegs::PORTAL_EXIT_YARDS)
inline TravelPortalGate PortalGate(TravelPortalFacts const& facts, std::uint32_t fromMap, std::uint32_t toMap,
                                   float toX, float toY, float exitYards)
{
    TravelPortalGate gate;
    gate.teams = facts.teams;
    gate.level = static_cast<std::uint8_t>(std::min<std::uint32_t>(facts.minLevel, 255));
    if (!facts.trigger)
        return gate;
    if (!facts.known)
        gate.use = TravelPortalUse::NoTrigger;
    else if (facts.triggerMap != fromMap)
        gate.use = TravelPortalUse::OtherMap;
    else if (!facts.hasRow)
        gate.use = TravelPortalUse::NoRow;
    else if (facts.scripted)
        gate.use = TravelPortalUse::Scripted;
    else if (facts.targetMap != toMap ||
             (fromMap == toMap && std::hypot(facts.targetX - toX, facts.targetY - toY) > exitYards))
        gate.use = TravelPortalUse::WrongArrival;
    if (facts.raid || facts.keyed)
        gate.teams = 0;
    return gate;
}

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
    // playerbots_travelnode.outdoor (navigation G91): 1 outdoors, 0 indoors (no takeoff there: WT72), -1 = no answer
    // (NULL or the column missing)
    std::int8_t outdoor{-1};
};

struct TravelGraphLink
{
    std::uint32_t from{0};  // node indices
    std::uint32_t to{0};
    TravelLinkType type{TravelLinkType::Walk};
    // max_creature_0..2 as stored: creature levels near the path (the legacy annoyance data)
    std::uint8_t maxCreature[3]{};
    // A portal link's gate (PortalGate, filled when the graph loads): the sides and the lowest level the core lets
    // use it; the route search refuses it to other bots (LinkSeconds). Both sides and 0 for every other link
    std::uint8_t useTeams{TRAVEL_BOTH_TEAMS};
    std::uint8_t useLevel{0};
    std::uint32_t object{0};
    float distance{0.0f};  // yards for walk links; 0.1 for the others
    float swim{0.0f};      // the swum part of distance
    // As stored: seconds for portals (3) and elevators (5, 8.6, 15); NOT seconds for flights (path length / 3600) and
    // ships or zeppelins (0.0033, 0.005)
    float extraCost{0.0f};
    // Ships and zeppelins (a transport link with an object whose extraCost is no seconds), read from the core's
    // transport template when the graph loads: the vessel's whole period, and the stop-to-stop ride from its key
    // frames (0: the loader matched no stop frames). Elevators: both 0, their stored seconds are the cost
    std::uint32_t periodMs{0};
    float rideSeconds{0.0f};
    // A ride (a transport link with an object): the stop records of its two ends in the graph's crossing index
    // (TravelGraph::Crossings), matched when the graph loads; a ride without them is not in the graph (WT.53)
    std::uint32_t crossFrom{TravelCrossing::NO_STOP};
    std::uint32_t crossTo{TravelCrossing::NO_STOP};
    float pathLength{0.0f};      // yards along the stored points, summed over segments on one map
    std::uint32_t pathFirst{0};  // points[pathFirst, pathFirst + pathCount), in nr order
    std::uint32_t pathCount{0};

    // A vessel's cost in seconds: the expected wait at the dock (half the period) plus the ride; half a period
    // where no key frames matched (exact for a two-stop shuttle, whose period is there and back)
    float VesselSeconds() const
    {
        float const half = static_cast<float>(periodMs) / 2000.0f;
        return half + (rideSeconds > 0.0f ? rideSeconds : half);
    }
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

// The crossing table as loaded, and the rides it kept (the loader's line: CrossingLoadLine)
struct TravelCrossingStats
{
    std::uint32_t rows{0};              // crossing rows loaded
    std::uint32_t stops{0};             // stop records
    std::uint32_t vessels{0};           // vessels (entry and guid) with a record
    std::uint32_t otherDisplay{0};      // rows not loaded: measured on another model than the object template's
    std::uint32_t noPlayerJump{0};      // rows not loaded: a jump no player makes (TravelCrossing::IsPlayerJump)
    std::uint32_t overThree{0};         // rows not loaded: a fourth or later row of one stop and direction
    std::uint32_t ridesKept{0};         // ride links in the graph
    std::uint32_t ridesLeftOut{0};      // ride links not added: no stop pair, no boarding or no leaving row
    std::uint32_t ridesWithoutWait{0};  // kept rides whose from-stop no wait-point link reaches
};

// Why a ride link was left out (no crossing, no ride)
enum class TravelRideGap : std::uint8_t
{
    FromStop,  // no stop record of the entry at the from node
    ToStop,    // no stop record of the same vessel at the to node
    BoardRow,  // the from-stop has no boarding row
    LeaveRow   // the to-stop has no leaving row
};

struct TravelRideLeftOut
{
    std::uint32_t entry{0};
    std::uint32_t guid{0};    // the vessel of the stop found at the from node; 0 a ship, or none found
    std::uint32_t fromId{0};  // playerbots_travelnode.id
    std::uint32_t toId{0};
    TravelRideGap gap{TravelRideGap::FromStop};
};

inline char const* RideGapWords(TravelRideGap gap)
{
    switch (gap)
    {
        case TravelRideGap::FromStop:
            return "no stop at the from node";
        case TravelRideGap::ToStop:
            return "no stop of the same vessel at the to node";
        case TravelRideGap::BoardRow:
            return "no boarding row at the from-stop";
        case TravelRideGap::LeaveRow:
            return "no leaving row at the to-stop";
    }
    return "?";
}

// The loader's line about the crossings (the server's log and the offline twin print the same text)
inline std::string CrossingLoadLine(TravelCrossingStats const& stats)
{
    return "crossings: " + std::to_string(stats.rows) + " rows at " + std::to_string(stats.stops) + " stops of " +
           std::to_string(stats.vessels) + " vessels (" + std::to_string(stats.otherDisplay) + " other display, " +
           std::to_string(stats.noPlayerJump) + " no player jump, " + std::to_string(stats.overThree) +
           " over 3); rides: " + std::to_string(stats.ridesKept) + " kept, " + std::to_string(stats.ridesLeftOut) +
           " left out for no crossing, " + std::to_string(stats.ridesWithoutWait) + " without a wait node";
}

// One line per ride left out
inline std::string RideLeftOutLine(TravelRideLeftOut const& ride)
{
    return "ride left out: entry " + std::to_string(ride.entry) + " guid " + std::to_string(ride.guid) + " nodes " +
           std::to_string(ride.fromId) + " -> " + std::to_string(ride.toId) + " (" + RideGapWords(ride.gap) + ")";
}

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
    TravelCrossingStats crossings;
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
    static_assert(TRAVEL_BOTH_TEAMS == (TAXI_ALLIANCE | TAXI_HORDE));

    // A portal link's facts from the core's stores (PA38c): the loader's gate and `route links portal|staticportal`
    // read the same. Only once the world's stores are loaded (before OnBeforeWorldInitialized starts the loader)
    static TravelPortalFacts ReadPortalFacts(TravelLinkType type, std::uint32_t object);

    static constexpr float FLIGHT_SPEED = 32.0f;  // yd/s, worldserver.conf TaxiFlightSpeed (default, Blizzlike)

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

    // The vessels' stop records and their crossings (TravelCrossingTypes.h); a ride link and a leg carry indices into
    // it. The rides the loader left out for having no crossing (commands and the load lines only)
    TravelCrossing::Index const& Crossings() const { return _crossings; }
    std::span<TravelRideLeftOut const> RidesLeftOut() const { return _ridesLeftOut; }

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
            // No jump onto a map: walks, air links and wait-point links on one map
            if (link.type == TravelLinkType::Walk || link.type == TravelLinkType::Air ||
                (IsWaitPointLink(link) && _nodes[link.from].mapId == _nodes[link.to].mapId))
                continue;
            TravelGraphArrival arrival;
            arrival.mapId = _nodes[link.to].mapId;
            arrival.node = link.to;
            if (link.type == TravelLinkType::Portal || link.type == TravelLinkType::StaticPortal)
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
            _arrivals.capacity() * sizeof(TravelGraphArrival) + _crossings.MemoryBytes() +
            _ridesLeftOut.capacity() * sizeof(TravelRideLeftOut);
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
    TravelCrossing::Index _crossings;
    std::vector<TravelRideLeftOut> _ridesLeftOut;
    TravelGraphStats _stats;

    std::atomic<State> _state{State::Idle};
    std::atomic<bool> _stop{false};
    std::thread _loader;
    std::string _error;  // why the load failed; written before _state turns Failed
};

// The one row sink of the graph: the loader thread (database rows) and tests/travel-graph-selftest.cpp (the shipped SQL
// files) feed it the same way: all nodes, crossings and links, EndLinks(), then the path points grouped by link in nr
// order (the tables' primary key order), then Finish() and Publish()
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
        std::uint32_t periodMs{0};  // a vessel's period and ride (TravelGraphLink)
        float rideSeconds{0.0f};
        std::uint32_t maxCreature[3]{};
        std::uint8_t useTeams{TRAVEL_BOTH_TEAMS};  // a portal link's gate (TravelGraphLink)
        std::uint8_t useLevel{0};
    };

    void AddNode(std::uint32_t id, std::string_view name, std::uint32_t mapId, float x, float y, float z,
                 std::int8_t outdoor = -1)
    {
        _nodes.push_back({id, mapId, x, y, z, 0, 0, false, outdoor});
        _names.emplace_back(name);
    }

    void AddLink(LinkRow const& row) { _linkRows.push_back(row); }

    // A row of playerbots_travelnode_crossing as stored; the window is the loader's (the core's stop frame at the
    // pose), none for a car or offline
    struct CrossingRow
    {
        std::uint32_t entry{0};
        std::uint32_t guid{0};
        std::uint32_t stop{0};
        std::uint32_t mapId{0};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        float o{0.0f};
        std::uint32_t direction{0};  // 0 boarding, 1 leaving
        std::uint32_t seq{0};
        std::uint32_t kind{0};  // 0 walk, 1 jump
        TravelCrossing::Spot dock;
        TravelCrossing::Spot deck;
        float gap{0.0f};
        float rise{0.0f};
        bool hasWindow{false};
        std::uint32_t arriveMs{0};
        std::uint32_t departMs{0};
        std::uint32_t periodMs{0};
    };

    void AddCrossing(CrossingRow const& row) { _crossingRows.push_back(row); }

    // A crossing row the loader did not pass on: its display id is not the object template's
    void CountOtherDisplay() { ++_stats.crossings.otherDisplay; }

    // Orders the nodes by id, builds the stop records, resolves the links to node indices (a ride only with its two
    // stops and their rows) and sorts them into per-node ranges
    bool EndLinks(std::string& error)
    {
        if (_nodes.empty())
        {
            error = "no travel nodes";
            return false;
        }
        BuildStops();

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
            link.useTeams = row.useTeams;
            link.useLevel = row.useLevel;
            link.object = row.object;
            link.distance = row.distance;
            link.swim = row.swim;
            link.extraCost = row.extraCost;
            link.periodMs = row.periodMs;
            link.rideSeconds = row.rideSeconds;
            // No crossing, no ride (WT.53): a ride is added only with a stop record at each end and their rows
            if (link.type == TravelLinkType::Transport && link.object)
            {
                TravelRideLeftOut out;
                if (!MatchRide(link, out))
                {
                    out.fromId = row.fromId;
                    out.toId = row.toId;
                    _ridesLeftOut.push_back(out);
                    ++_stats.crossings.ridesLeftOut;
                    continue;
                }
                ++_stats.crossings.ridesKept;
            }
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

        // A kept ride whose from-stop no wait-point link reaches: counted for the navigation plan, not repaired
        std::vector<bool> waited(_nodes.size(), false);
        for (TravelGraphLink const& link : _links)
            if (TravelGraph::IsWaitPointLink(link))
                waited[link.to] = true;
        for (TravelGraphLink const& link : _links)
            if (link.type == TravelLinkType::Transport && link.object && !waited[link.from])
                ++_stats.crossings.ridesWithoutWait;
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
            // The points of a ride left out for having no crossing go with it: no orphans
            if (_current == NO_LINK && IsLeftOut(fromId, toId))
                _current = LEFT_OUT;
        }

        if (_current == LEFT_OUT)
            return;

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
        graph._crossings = std::move(_crossings);
        graph._ridesLeftOut = std::move(_ridesLeftOut);
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
    static constexpr std::uint32_t LEFT_OUT = NO_LINK - 2;

    static bool KnownType(std::uint32_t type)
    {
        return type == static_cast<std::uint32_t>(TravelLinkType::Walk) ||
               type == static_cast<std::uint32_t>(TravelLinkType::Portal) ||
               type == static_cast<std::uint32_t>(TravelLinkType::Transport) ||
               type == static_cast<std::uint32_t>(TravelLinkType::Flight) ||
               type == static_cast<std::uint32_t>(TravelLinkType::StaticPortal) ||
               type == static_cast<std::uint32_t>(TravelLinkType::Air);
    }

    // The stop records from the crossing rows, in the table's key order. Not loaded and counted: a jump no player
    // makes, then a fourth or later row of one stop and direction. A stop whose rows were all refused keeps its
    // record (its pose is still where the vessel stands), without rows
    void BuildStops()
    {
        std::stable_sort(_crossingRows.begin(), _crossingRows.end(),
                         [](CrossingRow const& a, CrossingRow const& b)
                         {
                             if (a.entry != b.entry)
                                 return a.entry < b.entry;
                             if (a.guid != b.guid)
                                 return a.guid < b.guid;
                             if (a.stop != b.stop)
                                 return a.stop < b.stop;
                             if (a.direction != b.direction)
                                 return a.direction < b.direction;
                             return a.seq < b.seq;
                         });
        std::vector<TravelCrossing::Stop> stops;
        for (CrossingRow const& row : _crossingRows)
        {
            if (stops.empty() || stops.back().entry != row.entry || stops.back().guid != row.guid ||
                stops.back().stop != row.stop)
            {
                if (stops.empty() || stops.back().entry != row.entry || stops.back().guid != row.guid)
                    ++_stats.crossings.vessels;
                TravelCrossing::Stop stop;
                stop.entry = row.entry;
                stop.guid = row.guid;
                stop.stop = row.stop;
                stop.mapId = row.mapId;
                stop.x = row.x;
                stop.y = row.y;
                stop.z = row.z;
                stop.o = row.o;
                stop.hasWindow = row.hasWindow;
                stop.arriveMs = row.arriveMs;
                stop.departMs = row.departMs;
                stop.periodMs = row.periodMs;
                stops.push_back(stop);
            }

            TravelCrossing::Stop& stop = stops.back();
            TravelCrossing::Direction const direction =
                row.direction ? TravelCrossing::Direction::Leave : TravelCrossing::Direction::Board;
            TravelCrossing::Row kept;
            kept.kind = row.kind ? TravelCrossing::Kind::Jump : TravelCrossing::Kind::Walk;
            kept.seq = static_cast<std::uint8_t>(std::min<std::uint32_t>(row.seq, 255));
            kept.dock = row.dock;
            kept.deck = row.deck;
            kept.gap = row.gap;
            kept.rise = row.rise;
            if (kept.kind == TravelCrossing::Kind::Jump &&
                !TravelCrossing::IsPlayerJump(TravelCrossing::RiseOf(kept, direction), kept.gap))
            {
                ++_stats.crossings.noPlayerJump;
                continue;
            }
            std::uint8_t& count = direction == TravelCrossing::Direction::Board ? stop.boardCount : stop.leaveCount;
            if (count >= TravelCrossing::MAX_ROWS)
            {
                ++_stats.crossings.overThree;
                continue;
            }
            (direction == TravelCrossing::Direction::Board ? stop.board : stop.leave)[count++] = kept;
            ++_stats.crossings.rows;
        }
        _crossingRows.clear();
        _crossingRows.shrink_to_fit();
        stops.shrink_to_fit();
        _stats.crossings.stops = static_cast<std::uint32_t>(stops.size());
        _crossings.Assign(std::move(stops));
    }

    // A ride link meets its stops: the from-stop is a record of the link's entry whose pose lies within
    // STOP_NODE_YARDS of the from node, the to-stop a record of the same vessel (entry and guid) within them of the to
    // node; among several cars the pair with the smallest sum. False with why: no such pair, no boarding row at the
    // from-stop or no leaving row at the to-stop
    bool MatchRide(TravelGraphLink& link, TravelRideLeftOut& out) const
    {
        auto const yards = [](TravelCrossing::Stop const& stop, TravelGraphNode const& node)
        {
            float const dx = stop.x - node.x;
            float const dy = stop.y - node.y;
            float const dz = stop.z - node.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        TravelGraphNode const& from = _nodes[link.from];
        TravelGraphNode const& to = _nodes[link.to];
        std::span<TravelCrossing::Stop const> const stops = _crossings.StopsOf(link.object);
        TravelCrossing::Stop const* bestFrom = nullptr;
        TravelCrossing::Stop const* bestTo = nullptr;
        float best = std::numeric_limits<float>::max();
        bool atFrom = false;
        out.entry = link.object;
        for (TravelCrossing::Stop const& fromStop : stops)
        {
            if (fromStop.mapId != from.mapId || yards(fromStop, from) > TravelCrossing::STOP_NODE_YARDS)
                continue;
            if (!atFrom)
                out.guid = fromStop.guid;
            atFrom = true;
            for (TravelCrossing::Stop const& toStop : stops)
            {
                if (toStop.guid != fromStop.guid || &toStop == &fromStop || toStop.mapId != to.mapId ||
                    yards(toStop, to) > TravelCrossing::STOP_NODE_YARDS)
                    continue;
                float const sum = yards(fromStop, from) + yards(toStop, to);
                if (sum < best)
                {
                    best = sum;
                    bestFrom = &fromStop;
                    bestTo = &toStop;
                }
            }
        }
        if (!bestFrom)
        {
            out.gap = atFrom ? TravelRideGap::ToStop : TravelRideGap::FromStop;
            return false;
        }
        out.guid = bestFrom->guid;
        if (!bestFrom->boardCount || !bestTo->leaveCount)
        {
            out.gap = bestFrom->boardCount ? TravelRideGap::LeaveRow : TravelRideGap::BoardRow;
            return false;
        }
        link.crossFrom = _crossings.IndexOf(*bestFrom);
        link.crossTo = _crossings.IndexOf(*bestTo);
        return true;
    }

    bool IsLeftOut(std::uint32_t fromId, std::uint32_t toId) const
    {
        return std::any_of(_ridesLeftOut.begin(), _ridesLeftOut.end(), [fromId, toId](TravelRideLeftOut const& ride)
                           { return ride.fromId == fromId && ride.toId == toId; });
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
    std::vector<CrossingRow> _crossingRows;
    TravelCrossing::Index _crossings;
    std::vector<TravelRideLeftOut> _ridesLeftOut;
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

#endif
