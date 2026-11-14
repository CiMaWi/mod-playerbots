/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELWALKRULES_H
#define PLAYERBOTS_TRAVELWALKRULES_H

// A walk leg along graph links (WT58, WT.43), engine-free. Along a link the bot orders the link's stored points in
// order, thinned to SPACING_YARDS (the traversed link's own points; the opposite link's reversed where only it has
// them; a link without either walks to its far node). Each order is judged on the engine's answer before the move:
// a stored point off the mesh, or one the bot makes no progress towards, sends the rest of the link node to node
// (Fallback); an answer of "no path" is never walked as a straight line: at a stored point it falls back, at the
// far node it refuses the link (the follower fails the leg, the replan leaves the link out). The engine's hop, a
// straight last step of a walked path, is walked and counted. The points stay in the graph: the state is an index.
// NewRpgTravelAction and NewRpgBaseAction::MoveFarToImpl feed it on the bot's map thread;
// tests/travel-walk-selftest.cpp drives it with made-up links and answers.

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace TravelWalk
{
constexpr std::uint32_t NO_NODE = std::numeric_limits<std::uint32_t>::max();  // TravelGraph::NO_NODE
constexpr std::uint32_t NO_LINK = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t NO_POINT = std::numeric_limits<std::uint32_t>::max();  // the order is the link's far node

// PathType bits (core PathGenerator.h; NewRpgBaseAction.cpp checks them against the engine's enum)
constexpr std::uint32_t PATH_NOPATH = 0x08;
constexpr std::uint32_t PATH_NOT_USING_PATH = 0x10;   // a player's answer when an end has no polygon: a line
constexpr std::uint32_t PATH_FARFROMPOLY_END = 0x80;  // the end lies more than 7 yd from a polygon

// Tuning (WT.40, revisable from the nights' walk_link rows)
constexpr float SPACING_YARDS = 25.0f;  // stored points ordered at least this far apart
// The order is the furthest such point within LOOKAHEAD_YARDS of the bot, moved on at every follower tick: the
// follower ticks about every 5-6 s on a lab with 600 bots (WT58 run 1: 75 ticks in 474 s), and an order shorter than
// a tick's walk (6 s at 14 yd/s mounted: 84 yd) stops the bot at each point (25 yd orders: walks 1.6 times slower)
constexpr float LOOKAHEAD_YARDS = 90.0f;
constexpr float REACH_YARDS = 10.0f;  // a stored point counts as reached this close: the next order starts on the move
constexpr float HOP_STEP_YARDS = 5.0f;  // a path's last step longer than this is the engine's hop (smooth steps: 4 yd)
constexpr float PROGRESS_YARDS = 2.0f;  // an order whose path end gains less on its point makes no progress
constexpr std::uint8_t NO_PROGRESS_STRIKES = 3;  // orders without progress (or repeated) on one point: fallback

enum class Mode : std::uint8_t
{
    None,      // not on a graph link (the walks between the bot, the graph and the goal)
    Stored,    // the link's stored points
    Nodes,     // a link without stored points: to its far node
    Fallback,  // a stored point failed: the rest of the link to its far node
    Refused    // no path at the far node: the follower fails the leg
};

enum class Verdict : std::uint8_t
{
    Walk,
    Hop,       // walked; the path ends in a straight step longer than HOP_STEP_YARDS
    Fallback,  // not walked: the next order is the far node
    Refuse     // not walked: the link is refused
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

// The engine's answer to one order, read before the move
struct EngineAnswer
{
    bool result{false};     // CalculatePath's return
    std::uint32_t type{0};  // PathType bits
    float lastStep{0.0f};   // the path's last segment (a shortcut's whole line)
    float gain{0.0f};       // yards the path's end gains on the order's point (from the bot's position)
};

template <class A, class B>
float Distance(A const& a, B const& b)
{
    float const dx = a.x - b.x;
    float const dy = a.y - b.y;
    float const dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The point at index i in walking order
template <class Points>
auto const& PointAt(Points const& points, bool reversed, std::uint32_t i)
{
    return points[reversed ? points.size() - 1 - i : i];
}

// The order after index `after` (NO_POINT: from the link's start): the first point at least SPACING_YARDS from prev;
// NO_POINT (the far node) when none is, or that point lies within SPACING_YARDS / 2 of the far node
template <class Points, class Prev, class Far>
std::uint32_t NextPoint(Points const& points, bool reversed, std::uint32_t after, Prev const& prev, Far const& farAt)
{
    std::uint32_t const count = static_cast<std::uint32_t>(points.size());
    for (std::uint32_t i = after == NO_POINT ? 0 : after + 1; i < count; ++i)
    {
        auto const& at = PointAt(points, reversed, i);
        if (Distance(at, prev) < SPACING_YARDS)
            continue;
        return Distance(at, farAt) < SPACING_YARDS / 2.0f ? NO_POINT : i;
    }
    return NO_POINT;
}

// Every order of a link's points in walking order (the selftest's and the debug log's list)
template <class Points, class From, class Far>
std::vector<std::uint32_t> Thinned(Points const& points, bool reversed, From const& from, Far const& farAt)
{
    std::vector<std::uint32_t> orders;
    std::uint32_t i = NextPoint(points, reversed, NO_POINT, from, farAt);
    while (i != NO_POINT)
    {
        orders.push_back(i);
        i = NextPoint(points, reversed, i, PointAt(points, reversed, i), farAt);
    }
    return orders;
}

// Whose stored points a walk from -> to takes: the traversed link's own (a walkable link with points), else the
// opposite link's reversed, else none. linksOf(node) gives a node's links (members to, pathCount); walkable(link)
// leaves the rides out (a ship's stored points are its voyage)
template <class Link>
struct Rows
{
    Link const* link{nullptr};
    bool reversed{false};
};

template <class Link, class LinksOf, class Walkable>
Rows<Link> PickRows(LinksOf const& linksOf, std::uint32_t from, std::uint32_t to, Walkable const& walkable)
{
    for (Link const& link : linksOf(from))
        if (link.to == to && link.pathCount && walkable(link))
            return {&link, false};
    for (Link const& link : linksOf(to))
        if (link.to == from && link.pathCount && walkable(link))
            return {&link, true};
    return {};
}

// A new link: by its stored points when it has some, else by its nodes
template <class Points, class From, class Far>
void Begin(LinkWalk& walk, std::uint32_t from, std::uint32_t to, std::uint32_t link, Points const& points,
           bool reversed, From const& fromAt, Far const& toAt)
{
    walk = LinkWalk{};
    walk.from = from;
    walk.to = to;
    if (points.empty())
    {
        walk.mode = Mode::Nodes;
        return;
    }
    walk.mode = Mode::Stored;
    walk.link = link;
    walk.reversed = reversed;
    walk.point = NextPoint(points, reversed, NO_POINT, fromAt, toAt);
    if (walk.point != NO_POINT)
        walk.after = NextPoint(points, reversed, walk.point, PointAt(points, reversed, walk.point), toAt);
}

// The order now: the furthest stored point within LOOKAHEAD_YARDS of the bot (past the one it reached), or NO_POINT
// for the far node once the last point is reached or the far node lies within LOOKAHEAD_YARDS. Moving on clears the
// strikes
template <class Points, class Bot, class Far>
std::uint32_t Target(LinkWalk& walk, Points const& points, Bot const& bot, Far const& toAt)
{
    if (walk.mode != Mode::Stored)
        return NO_POINT;
    while (walk.point != NO_POINT)
    {
        bool const reached = Distance(bot, PointAt(points, walk.reversed, walk.point)) <= REACH_YARDS;
        bool const ahead = walk.after != NO_POINT
                               ? Distance(bot, PointAt(points, walk.reversed, walk.after)) <= LOOKAHEAD_YARDS
                               : Distance(bot, toAt) <= LOOKAHEAD_YARDS;
        if (!reached && !ahead)
            break;
        walk.point = walk.after;
        walk.after = walk.point == NO_POINT ? NO_POINT
                                            : NextPoint(points, walk.reversed, walk.point,
                                                        PointAt(points, walk.reversed, walk.point), toAt);
        walk.strikes = 0;
    }
    return walk.point;
}

// The verdict on one order's answer (direct: the order lies within the follower's direct range, so the engine would
// walk a "no path" answer as a straight line). Stored points: no path, no polygon at the point or one more than 7 yd
// off falls back, and so do NO_PROGRESS_STRIKES orders without gain or ordered again. The far node: no path near it
// refuses the link; far from it the follower's stepping stones are tried as before (no move counted)
inline Verdict Judge(LinkWalk& walk, EngineAnswer const& answer, bool direct)
{
    if (walk.mode == Mode::Refused)
        return Verdict::Refuse;
    bool const noPath = !answer.result || (answer.type & PATH_NOPATH);
    if (walk.mode == Mode::Stored)
    {
        bool const offMesh = noPath || (answer.type & (PATH_NOT_USING_PATH | PATH_FARFROMPOLY_END));
        bool const again = walk.orderedOnce && walk.ordered == walk.point;
        if (!offMesh && (again || answer.gain < PROGRESS_YARDS))
            ++walk.strikes;
        if (offMesh || walk.strikes >= NO_PROGRESS_STRIKES)
        {
            walk.mode = Mode::Fallback;
            return Verdict::Fallback;
        }
        walk.ordered = walk.point;
        walk.orderedOnce = true;
    }
    else if (noPath && direct)
    {
        walk.mode = Mode::Refused;
        return Verdict::Refuse;
    }
    else if (noPath)
        return Verdict::Walk;
    if (walk.moves < std::numeric_limits<std::uint16_t>::max())
        ++walk.moves;
    if (answer.lastStep > HOP_STEP_YARDS)
    {
        if (walk.hops < std::numeric_limits<std::uint16_t>::max())
            ++walk.hops;
        return Verdict::Hop;
    }
    return Verdict::Walk;
}

// A move the follower made without an answer to judge (a stepping stone towards the far node)
inline void NoteMove(LinkWalk& walk)
{
    if (walk.Active() && walk.moves < std::numeric_limits<std::uint16_t>::max())
        ++walk.moves;
}

// The walk_link row's reason
inline char const* RowWord(LinkWalk const& walk)
{
    switch (walk.mode)
    {
        case Mode::None:
            return "none";
        case Mode::Stored:
            return "stored";
        case Mode::Nodes:
            return "nodes";
        case Mode::Fallback:
            return "fallback";
        case Mode::Refused:
            return "refused";
    }
    return "?";
}
}  // namespace TravelWalk

#endif
