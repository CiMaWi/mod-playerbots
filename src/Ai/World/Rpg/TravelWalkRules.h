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
// end, each the follower's own call (OrderStep: the smooth path its order asks) judged as the follower's order to the
// point the step asks (its aim: a corner on the way, or the walk's end; WT64i, WT.59: GoTo::ProbeCarried, one reader
// of the engine's answer) and done where the follower stops ordering (the waypoint's reach); the bot walks the points
// probed (PieceTarget), and a refused walk fails its leg at once (TravelLegs::FailReason::NoWalk), so that the replan
// leaves it out. A walk with an end on a navmesh fragment is refused without a call (StartPiece).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "TravelWalkTypes.h"

namespace TravelWalk
{
// PathType bits (core PathGenerator.h; NewRpgBaseAction.cpp and NewRpgRoute.cpp check them against the engine's enum)
constexpr std::uint32_t PATH_NORMAL = 0x01;
constexpr std::uint32_t PATH_INCOMPLETE = 0x04;  // a partial path: it ends closer to the goal
constexpr std::uint32_t PATH_NOPATH = 0x08;
constexpr std::uint32_t PATH_NOT_USING_PATH = 0x10;     // a player's answer when an end has no polygon: a line
constexpr std::uint32_t PATH_SHORT = 0x20;              // longer than the point limit: replaced by a straight line
constexpr std::uint32_t PATH_FARFROMPOLY_START = 0x40;  // the start lies more than 7 yd from a polygon
constexpr std::uint32_t PATH_FARFROMPOLY_END = 0x80;    // the end lies more than 7 yd from a polygon

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
constexpr std::uint8_t PROBE_STEPS = 10;         // steps a walk off the graph's probe makes at most (#2747)
// The probed points the bot orders at most; then the walk's end. The smooth path's points lie 4 yd apart, so the
// orders lie about SPACING_YARDS apart: 128 cover PROBE_STEPS steps aimed some 300 yd each (OrderStep)
constexpr std::size_t PROBE_ORDERS = 128;

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

// The engine's hop, the one hop test (MeshMove::Classify's and Judge's; PA7, G51): a partial path (INCOMPLETE) whose
// last step is longer than HOP_STEP_YARDS. The engine copies an unreached target in as the path's last point for a
// partial polygon path only (the core's PathGenerator::BuildPointPath); a NORMAL path's last step over the limit is a
// smooth 4 yd step measured on steep or rough ground (WT64g-2: 23 refused orders with last steps of 5.0 to 6.0 yd and
// none longer, against a median of 78 yd for the INCOMPLETE hops). Any other PathType bit (no path, a line, a
// shortcut, an end far from a polygon) is a class of its own that outranks the hop
inline bool IsHop(std::uint32_t type, float lastStep)
{
    return (type & PATH_INCOMPLETE) && !(type & ~(PATH_NORMAL | PATH_INCOMPLETE)) && lastStep > HOP_STEP_YARDS;
}

// The verdict on one order's answer. Stored points: no path, no polygon at the point or one more than 7 yd off falls
// back, and so do NO_PROGRESS_STRIKES orders without gain or ordered again. The far node: no path refuses the link,
// near it or far (WT64g: no stepping stone is sampled; a refusal is the leg's typed end). An answer that is the
// engine's hop (IsHop) is walked and counted
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
    if (IsHop(answer.type, answer.lastStep))
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
    std::optional<ProbePoint> aim;  // the point the call asked (OrderStep); without one, the walk's end
};

// The probe of a walk from `from` to `to`: step(start, to) -> ProbeAnswer calls the engine; a chain of such calls, each
// from the last one's end, as #2747's WorldPosition::getPathFromPath (a fresh path per step). accepts(answer, start,
// aim) is the judge: each step as the follower's order from the step's start to the point the step asked, its aim
// (GoTo::ProbeCarried, WT64i: GoTo::LegCarried, its class and its end; until WT64i the type flags of #2747, which let
// an end far from a polygon, a shortcut and the engine's one-line partial answer through). A step aimed at a corner
// on the way is judged, and its progress measured, against that corner (WT64i round 5: judged against the walk's end,
// a winding walk whose first corners lead away from the end was refused, though the follower, ordering the probed
// points within LOOKAHEAD_YARDS, walks it). Walk once a path ends within arriveYards of `to`, the waypoint's reach,
// where the follower stops ordering it (WT64i round 4: the probe stopped within REACH_YARDS, and the follower ordered
// the end again from a spot up to 10 yd short, where the engine answered its one line); refused at the first answer
// the judge does not carry, gains under PROGRESS_YARDS on its aim or is the PROBE_STEPS-th short of the end. Refused
// too (PathType) at a step that asked the walk's end itself and whose search fell short of it (no_join, nodes) and of
// arriveYards: the next step would ask the same end from the point that search found nearest, and gain nothing (pathing
// PA4c: with the wide pool each such step is a flood of up to 65,535 nodes; offline, on 2,449 pairs, the rule refuses
// no walk; a step aimed at a corner on the way goes on, since a later step from there can reach the end). The
// orders: the paths' points at least SPACING_YARDS apart, none within SPACING_YARDS / 2 of `to` (the follower orders
// the walk's end itself), at most PROBE_ORDERS
template <class From, class To, class Step, class Accepts>
PieceWalk ProbePiece(From const& from, To const& to, float arriveYards, Step const& step, Accepts const& accepts)
{
    PieceWalk piece;
    ProbePoint const end{to.x, to.y, to.z};
    ProbePoint at{from.x, from.y, from.z};
    ProbePoint kept = at;
    // A refusal names the reason of its last step (AnswerWhy: a StepAnswer's why, none for a plain answer; pathing PA4)
    ProbeWhy why = ProbeWhy::None;
    while (piece.probe == Probe::None)
    {
        auto const answer = step(at, end);
        ++piece.steps;
        why = AnswerWhy(answer);  // found at the call by its argument (defined below, with StepAnswer)
        ProbePoint const aim = answer.aim.value_or(end);
        if (!accepts(answer, at, aim))
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
        bool const askedEnd = Distance(aim, end) < 0.1f;  // StepAim's aim is the end itself, or a corner on the way
        if (Distance(reached, end) <= arriveYards)
            piece.probe = Probe::Walk;
        else if (askedEnd && (why == ProbeWhy::NoJoin || why == ProbeWhy::Nodes))
            piece.probe = Probe::PathType;
        else if (Distance(at, aim) - Distance(reached, aim) < PROGRESS_YARDS)
            piece.probe = Probe::Stall;
        else if (piece.steps >= PROBE_STEPS)
            piece.probe = Probe::Steps;
        at = reached;
    }
    if (piece.Refused())
    {
        piece.orders.clear();
        piece.why = why;
    }
    return piece;
}

// Where a probe step asks the smooth path: the walk's end, or, where the straight path's corners run longer than
// aimYards, the last corner within that length (the first corner where already that one lies beyond)
inline ProbePoint StepAim(ProbeAnswer const& corners, ProbePoint const& end, float aimYards)
{
    float along = 0.0f;
    for (std::size_t i = 1; i < corners.points.size(); ++i)
    {
        along += Distance(corners.points[i - 1], corners.points[i]);
        if (along > aimYards)
            return corners.points[i > 1 ? i - 1 : i];
    }
    return end;
}

// Why a search fell short (pathing PA4): Detour's status details (DetourStatus.h); NewRpgTravelWalk.cpp checks them
// against the engine's
constexpr std::uint32_t SEARCH_OUT_OF_NODES = 1u << 5;  // DT_OUT_OF_NODES: the search used its whole node pool
constexpr std::uint32_t SEARCH_PARTIAL = 1u << 6;       // DT_PARTIAL_RESULT: it ended short of the end's polygon

// One engine call's reason for falling short of its end, from its PathType and its search's Detour status
// (PathGenerator::GetSearchStatus): an end without a polygon in the engine's boxes gets the straight line
// (PATH_NOT_USING_PATH; startPoly says which end lacks it), one over 7 yd from its polygon a FARFROMPOLY bit; a search
// out of nodes is Nodes, one short of the end with nodes to spare spent the bot's piece of mesh (NoJoin). None: the
// search reached the end's polygon, or none ran
inline ProbeWhy SearchWhy(std::uint32_t type, std::uint32_t status, bool startPoly)
{
    if (type & PATH_NOT_USING_PATH)
        return startPoly ? ProbeWhy::NoEnd : ProbeWhy::NoStart;
    if (type & PATH_FARFROMPOLY_START)
        return ProbeWhy::NoStart;
    if (type & PATH_FARFROMPOLY_END)
        return ProbeWhy::NoEnd;
    if (status & SEARCH_OUT_OF_NODES)
        return ProbeWhy::Nodes;
    if (status & SEARCH_PARTIAL)
        return ProbeWhy::NoJoin;
    return ProbeWhy::None;
}

// A probe step's answer with its reason (SearchWhy), as the engine's walk probe asks it; an answer without one
// (ProbeAnswer) names none
struct StepAnswer : ProbeAnswer
{
    ProbeWhy why{ProbeWhy::None};
};

inline ProbeWhy AnswerWhy(ProbeAnswer const&) { return ProbeWhy::None; }
inline ProbeWhy AnswerWhy(StepAnswer const& answer) { return answer.why; }

// A probe step asks the follower's own call (WT64i fix round, WT.59): order(at, aim) -> the smooth path, as the order
// (GoTo::LegCarried's answer) asks it, so the probe and the order read one answer. Until then the probe asked the
// straight path, whose corners carried where the order's smooth path got the engine's one line (a 2-point shortcut),
// and the bot was refused on the piece it was sent along. shape(at, end) -> the straight path only aims the call: a
// smooth path holds a bounded count of 4 yd points (a longer one fails as that same one line), so a walk whose corners
// run longer than aimYards is asked to a corner within it (StepAim), and the chain goes on from the answer's end. The
// answer carries the point asked, which the probe judges it against (ProbePiece), and the reason of the shape's call
// to the walk's end, else of the order's (AnswerWhy; pathing PA4)
template <class Shape, class Order>
StepAnswer OrderStep(ProbePoint const& at, ProbePoint const& end, Shape const& shape, Order const& order,
                     float aimYards)
{
    auto const corners = shape(at, end);
    ProbePoint const aim = StepAim(corners, end, aimYards);
    StepAnswer answer{order(at, aim)};
    answer.aim = aim;
    if (ProbeWhy const toEnd = AnswerWhy(corners); toEnd != ProbeWhy::None)
        answer.why = toEnd;
    return answer;
}

// A walk off the graph as the bot starts it (WT64i, WT.59): an end on a navmesh fragment (a connected piece of a few
// polygons, which no path leaves) is refused at once, without a path call (Fragment: not walked another way); an end
// on an unloaded tile or a map without a navmesh is not probed (None: the follower orders the waypoint itself, judged
// by its order); else the probe, done within arriveYards of `to`
template <class From, class To, class Step, class Accepts>
PieceWalk StartPiece(bool fragment, bool tilesLoaded, From const& from, To const& to, float arriveYards,
                     Step const& step, Accepts const& accepts)
{
    if (fragment)
    {
        PieceWalk refused;
        refused.probe = Probe::Fragment;
        return refused;
    }
    return tilesLoaded ? ProbePiece(from, to, arriveYards, step, accepts) : PieceWalk{};
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

// An approach's judge asks the walk probe's wide search (pathing PA5): an approach (no trip), not a GoTo local piece,
// whose failure asks a trip, and never on an instance or a battleground, so that no such map makes its own wide query
inline bool ApproachAsksWide(bool trips, bool instanceable) { return !trips && !instanceable; }

// The order on an approach's judged path (pathing PA5, the follower's way): the furthest point whose length along the
// path lies within lookahead yards, so that the engine's point move, which searches its own path of 1,024 nodes, is
// asked a piece the wide search found walkable within that length; past a point within REACH_YARDS of the path's start
// (a path that doubles back by the bot), the end at last. NO_POINT for fewer than two points
template <class Points>
std::uint32_t OrderPoint(Points const& points, float lookahead)
{
    std::uint32_t const count = static_cast<std::uint32_t>(points.size());
    if (count < 2)
        return NO_POINT;
    std::uint32_t order = 1;
    float along = 0.0f;
    for (std::uint32_t i = 1; i < count; ++i)
    {
        along += Distance(points[i - 1], points[i]);
        if (along > lookahead)
            break;
        order = i;
    }
    while (order + 1 < count && Distance(points[0], points[order]) <= REACH_YARDS)
        ++order;
    return order;
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

// A refusal's reason word (pathing PA4: the refused walk's log line)
inline char const* WhyWord(ProbeWhy why)
{
    switch (why)
    {
        case ProbeWhy::None:
            return "none";
        case ProbeWhy::NoStart:
            return "no_start";
        case ProbeWhy::NoEnd:
            return "no_end";
        case ProbeWhy::NoJoin:
            return "no_join";
        case ProbeWhy::Nodes:
            return "nodes";
    }
    return "?";
}

// Who asked the walk that was refused (PA29): the trip's walk piece, an approach, a trip's local piece
enum class RefusalCaller : std::uint8_t
{
    TripWalk,
    Approach,
    LocalPiece
};

// The world-log row's choice for a refused walk (PA29): the caller's word
inline char const* RefusalChoice(RefusalCaller caller)
{
    switch (caller)
    {
        case RefusalCaller::TripWalk:
            return "refused_trip_walk";
        case RefusalCaller::Approach:
            return "refused_approach";
        case RefusalCaller::LocalPiece:
            return "refused_local_piece";
    }
    return "?";
}

// The row's reason (PA29): <probe word>:<reason word>, e.g. path_type:no_join, fragment:none
inline std::string RefusalReason(Probe probe, ProbeWhy why)
{
    return std::string(ProbeWord(probe)) + ":" + WhyWord(why);
}

// An approach refused by the point move's own path (the second judge): its reason is not read
inline std::string MovePathRefusalReason() { return "move_path:none"; }
}  // namespace TravelWalk

#endif
