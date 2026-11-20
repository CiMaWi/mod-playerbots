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
// NewRpgTravelAction feeds it on the bot's map thread (the route in NewRpgRoute.cpp, the judged order of
// MoveFarToImpl in NewRpgTravelWalk.cpp); tests/travel-walk-selftest.cpp drives it with made-up links and answers.
// The walks off the graph (WT56: the bot's start to its first node, the last node to the goal, a direct walk) are
// probed on the navmesh once, when the bot starts them (ProbePiece): a chain of path calls, each from the last one's
// end, each answer accepted only where the follower's judge carries it (WT64i, WT.59: MeshMove::ProbeAccepts, one
// reader of the engine's answer); the bot walks the points probed (PieceTarget), and a refused walk fails its leg at
// once (TravelLegs::FailReason::NoWalk), so that the replan leaves it out. A walk with an end on a navmesh fragment is
// refused without a call (StartPiece).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "TravelWalkTypes.h"

namespace TravelWalk
{
// PathType bits (core PathGenerator.h; NewRpgBaseAction.cpp and NewRpgRoute.cpp check them against the engine's enum)
constexpr std::uint32_t PATH_NORMAL = 0x01;
constexpr std::uint32_t PATH_INCOMPLETE = 0x04;  // a partial path: it ends closer to the goal
constexpr std::uint32_t PATH_NOPATH = 0x08;
constexpr std::uint32_t PATH_NOT_USING_PATH = 0x10;   // a player's answer when an end has no polygon: a line
constexpr std::uint32_t PATH_SHORT = 0x20;            // longer than the point limit: replaced by a straight line
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
constexpr std::uint8_t PROBE_STEPS = 10;         // path calls a walk off the graph's probe makes at most (#2747)
constexpr std::size_t PROBE_ORDERS = 32;         // the probed points the bot orders at most; then the walk's end

enum class Verdict : std::uint8_t
{
    Walk,
    Hop,       // walked; the path ends in a straight step longer than HOP_STEP_YARDS
    Fallback,  // not walked: the next order is the far node
    Refuse     // not walked: the link is refused
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

// The verdict on one order's answer. Stored points: no path, no polygon at the point or one more than 7 yd off falls
// back, and so do NO_PROGRESS_STRIKES orders without gain or ordered again. The far node: no path refuses the link,
// near it or far (WT64g: no stepping stone is sampled; a refusal is the leg's typed end)
inline Verdict Judge(LinkWalk& walk, EngineAnswer const& answer)
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
    else if (noPath)
    {
        walk.mode = Mode::Refused;
        return Verdict::Refuse;
    }
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

// An accepted order that moved nothing counts as refused (WT64g; WT64b's followers that stood while the order to the
// same place was accepted every tick): the follower orders the same place again from within PROGRESS_YARDS of where its
// last accepted order left it, NO_MOTION_STRIKES times in a row. No clock: the walk's 90 s stays the progress
// observer's. movedYards: from where the last accepted order was given (none: a large value). A bot within
// AT_PLACE_YARDS of the place has nothing to move, so it is no strike (WT64g-2: a transport leg's approach orders its
// dock point every tick while the bot waits there for the car, 1 to 2 yd from it). True: refused
constexpr std::uint8_t NO_MOTION_STRIKES = 2;
constexpr float AT_PLACE_YARDS = 3.0f;  // WT.40: a tuning value
inline bool StillOrder(std::uint8_t& strikes, bool samePlace, float movedYards, float placeYards)
{
    if (!samePlace || movedYards >= PROGRESS_YARDS || placeYards <= AT_PLACE_YARDS)
    {
        strikes = 0;
        return false;
    }
    if (strikes < NO_MOTION_STRIKES)
        ++strikes;
    return strikes >= NO_MOTION_STRIKES;
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

// One path call's answer: CalculatePath's return, the path's type and its points (the first one is the call's start)
struct ProbeAnswer
{
    bool result{false};
    std::uint32_t type{0};
    std::vector<ProbePoint> points;
};

// The probe of a walk from `from` to `to`: step(start, to) -> ProbeAnswer calls the engine; a chain of such calls, each
// from the last one's end, as #2747's WorldPosition::getPathFromPath (a fresh path per step). accepts(answer) is the
// judge (MeshMove::ProbeAccepts, WT64i: the class the follower's order needs, Carried; until WT64i the type flags of
// #2747, which let an end far from a polygon, a shortcut and the engine's one-line partial answer through). Walk once
// a path ends within REACH_YARDS of `to`; refused at the first answer the judge does not carry, gains under
// PROGRESS_YARDS or is the PROBE_STEPS-th short of the end. The orders: the paths' points at least SPACING_YARDS apart,
// none within SPACING_YARDS / 2 of `to` (the follower orders the walk's end itself), at most PROBE_ORDERS
template <class From, class To, class Step, class Accepts>
PieceWalk ProbePiece(From const& from, To const& to, Step const& step, Accepts const& accepts)
{
    PieceWalk piece;
    ProbePoint const end{to.x, to.y, to.z};
    ProbePoint at{from.x, from.y, from.z};
    ProbePoint kept = at;
    while (piece.probe == Probe::None)
    {
        ProbeAnswer const answer = step(at, end);
        ++piece.steps;
        if (!accepts(answer))
        {
            piece.probe = Probe::PathType;
            break;
        }
        for (ProbePoint const& point : answer.points)
        {
            if (piece.orders.size() < PROBE_ORDERS && Distance(point, kept) >= SPACING_YARDS &&
                Distance(point, end) >= SPACING_YARDS / 2.0f)
            {
                piece.orders.push_back(point);
                kept = point;
            }
        }
        ProbePoint const reached = answer.points.back();
        if (Distance(reached, end) <= REACH_YARDS)
            piece.probe = Probe::Walk;
        else if (Distance(at, end) - Distance(reached, end) < PROGRESS_YARDS)
            piece.probe = Probe::Stall;
        else if (piece.steps >= PROBE_STEPS)
            piece.probe = Probe::Steps;
        at = reached;
    }
    if (piece.Refused())
        piece.orders.clear();
    return piece;
}

// A walk off the graph as the bot starts it (WT64i, WT.59): an end on a navmesh fragment (a connected piece of a few
// polygons, which no path leaves) is refused at once, without a path call (Fragment: not walked another way); an end
// on an unloaded tile or a map without a navmesh is not probed (None: the follower orders the waypoint itself, judged
// by its order); else the probe
template <class From, class To, class Step, class Accepts>
PieceWalk StartPiece(bool fragment, bool tilesLoaded, From const& from, To const& to, Step const& step,
                     Accepts const& accepts)
{
    if (fragment)
    {
        PieceWalk refused;
        refused.probe = Probe::Fragment;
        return refused;
    }
    return tilesLoaded ? ProbePiece(from, to, step, accepts) : PieceWalk{};
}

// The order on a probed walk: the furthest probed point within LOOKAHEAD_YARDS of the bot past the one it reached (as
// Target on a link's stored points), or NO_POINT for the walk's end once the last point is passed or the end lies
// within LOOKAHEAD_YARDS
template <class Bot, class End>
std::uint32_t PieceTarget(PieceWalk& piece, Bot const& bot, End const& end)
{
    std::uint32_t const count = static_cast<std::uint32_t>(piece.orders.size());
    while (piece.index < count)
    {
        bool const reached = Distance(bot, piece.orders[piece.index]) <= REACH_YARDS;
        bool const ahead = piece.index + 1 < count ? Distance(bot, piece.orders[piece.index + 1]) <= LOOKAHEAD_YARDS
                                                   : Distance(bot, end) <= LOOKAHEAD_YARDS;
        if (!reached && !ahead)
            break;
        ++piece.index;
    }
    return piece.index < count ? piece.index : NO_POINT;
}

// The probe's word (the refusal's log line)
inline char const* ProbeWord(Probe probe)
{
    switch (probe)
    {
        case Probe::None:
            return "none";
        case Probe::Walk:
            return "walk";
        case Probe::PathType:
            return "path_type";
        case Probe::Stall:
            return "stall";
        case Probe::Steps:
            return "steps";
        case Probe::Fragment:
            return "fragment";
    }
    return "?";
}
}  // namespace TravelWalk

#endif
