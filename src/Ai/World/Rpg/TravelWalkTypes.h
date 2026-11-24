/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELWALKTYPES_H
#define PLAYERBOTS_TRAVELWALKTYPES_H

#include <cstdint>
#include <limits>
#include <vector>

// A walk's state (TravelWalkRules.h): the link walked and the walk off the graph probed, as a trip holds
// them (TravelLegs::Trip). The rules that step them stay in TravelWalkRules.h.
namespace TravelWalk
{
constexpr std::uint32_t NO_NODE = std::numeric_limits<std::uint32_t>::max();  // TravelGraph::NO_NODE
constexpr std::uint32_t NO_LINK = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t NO_POINT = std::numeric_limits<std::uint32_t>::max();  // the order is the link's far node
// The polygons the bots' paths leave out: NavTerrain MAGMA | SLIME | GROUND_STEEP, the core's filter for a headless
// player (PathGenerator::CreateFilter). The module's own polygon queries and the link check's probe use it;
// RouteCheck.cpp asserts it against the engine's NavTerrain
constexpr std::uint16_t BOT_EXCLUDE_FLAGS = 0x02 | 0x04 | 0x10;

enum class Mode : std::uint8_t
{
    None,      // not on a graph link (the walks between the bot, the graph and the goal)
    Stored,    // the link's stored points
    Nodes,     // a link without stored points: to its far node
    Fallback,  // a stored point failed: the rest of the link to its far node
    Refused    // no path at the far node: the follower fails the leg
};

// One link walked (Trip::walk): its ends, whose points, the order now and the counts for the walk_link row
struct LinkWalk
{
    std::uint32_t from{NO_NODE};  // graph node indices
    std::uint32_t to{NO_NODE};
    std::uint32_t link{NO_LINK};  // the link whose stored points are walked
    bool reversed{false};         // its points walked from the last
    Mode mode{Mode::None};
    std::uint32_t point{NO_POINT};    // the stored point ordered now (index in walking order)
    std::uint32_t after{NO_POINT};    // the one after it
    std::uint32_t ordered{NO_POINT};  // the point of the last order: ordering it again is no progress
    bool orderedOnce{false};
    std::uint8_t strikes{0};
    std::uint16_t moves{0};
    std::uint16_t hops{0};

    bool Active() const { return mode != Mode::None; }
    bool On(std::uint32_t a, std::uint32_t b) const { return Active() && from == a && to == b; }
};

// A walk off the graph (WT56), probed on the navmesh when the bot starts it
struct ProbePoint
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

enum class Probe : std::uint8_t
{
    None,      // not probed
    Walk,      // a real path to the walk's end: the bot walks its points
    PathType,  // refused: a call's answer the judge does not carry (GoTo::ProbeCarried; WT64i)
    Stall,     // refused: a path gained under PROGRESS_YARDS on the point it asked from the last one's end
    Steps,     // refused: still farther than arriveYards (the waypoint's reach) from the end after PROBE_STEPS calls
    Fragment   // refused: an end lies on a navmesh fragment, which no path leaves (WT64i: no path call)
};

// Why a refused probe's search fell short of the walk's end (pathing PA4; TravelWalk::SearchWhy), read at the step
// that refused it; None where the search reached the end's polygon or none ran (a fragment end)
enum class ProbeWhy : std::uint8_t
{
    None,
    NoStart,  // no polygon at the bot: none in the engine's search boxes, or the nearest over 7 yd away
    NoEnd,    // no polygon at the place, the same way
    NoJoin,   // the search spent the piece of mesh the bot stands on, nodes to spare, short of the end's polygon
    Nodes     // the search ran out of nodes before it reached the end's polygon
};

// The walk off the graph the bot walks now (Trip::piece; StartLeg resets it): its leg and the waypoint it heads for,
// the probe's verdict, its path calls and the points to order, PieceTarget's index into them
struct PieceWalk
{
    std::uint32_t leg{NO_POINT};
    std::uint32_t next{NO_POINT};
    Probe probe{Probe::None};
    ProbeWhy why{ProbeWhy::None};  // a refusal's reason (ProbePiece)
    std::uint8_t steps{0};
    std::vector<ProbePoint> orders;
    std::uint32_t index{0};
    bool standEnd{false};  // the trip's goal walk ends at the stand point around the goal (LastWalkEnd), not on it
    ProbePoint end{};      // that stand point

    bool For(std::uint32_t legIdx, std::uint32_t waypoint) const
    {
        return probe != Probe::None && leg == legIdx && next == waypoint;
    }
    bool Refused() const { return probe != Probe::None && probe != Probe::Walk; }
};
}  // namespace TravelWalk

#endif
