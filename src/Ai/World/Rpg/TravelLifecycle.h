/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVEL_LIFECYCLE_H
#define PLAYERBOTS_TRAVEL_LIFECYCLE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace TravelLifecycle
{
constexpr std::uint32_t PASSENGER_INTERVAL_MS = 1000;
// A rider without a move of its own (no boarding or exit spline) leaves its deck on this many samples off it in a row:
// one probe off the surface while it stands still is the hull's model a step away from its passengers (WT54-6: two
// riders of 187568 dropped at sea on the same sample), not an exit
constexpr std::uint8_t OFF_SURFACE_SAMPLES = 2;

struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

constexpr float CROSSING_YARDS = 25.0f;
// The ring searches around a transport for a quay or an exit sample a bearing every 5 degrees (WT54-5): a zeppelin's
// gondola opens to its tower by a gangway a few yards wide, which bearings 15 degrees apart (5 yd at 20 yd) step over
constexpr unsigned CROSSING_BEARINGS = 72;
constexpr float CROSSING_BEARING_STEP = 6.28318531f / static_cast<float>(CROSSING_BEARINGS);
constexpr float DOCK_FLOOR_YARDS = 2.0f;
// A bot that waits above the platform beside a docked transport (a zeppelin tower's ramp or roof) finds its quay on
// the ring this far below or above its own floor
constexpr float QUAY_LEVEL_YARDS = 16.0f;
constexpr float CROSSING_BODY_YARDS = 0.65f;
// The step from a deck edge up or down to the quay on an exit: a jump (apex 1.64 yd at the client's 7.96 yd/s take-off
// and 19.29 yd/s^2 gravity) plus the navmesh's step (walkableClimb 4 cells x 0.267 yd = 1.07 yd) reaches 2.7 yd. Booty
// Bay's pier lies 2.04 yd above the docked ship's deck edge.
constexpr float EXIT_STEP_YARDS = 2.5f;
// How far inside the deck's edge a crossing starts. Boarding walks 4 yd onto the deck; an exit starts 1 yd inside the
// edge: 4 yd in from a 2-yd edge step lands on the centre of a zeppelin's gondola (about 7 yd radius), behind its rail.
constexpr float BOARD_DECK_INSET_YARDS = 4.0f;
constexpr float EXIT_DECK_INSET_YARDS = 1.0f;
// The highest step between two yard samples of a walk across a deck (the navmesh's walkableClimb, 4 cells)
constexpr float DECK_CLIMB_YARDS = 1.07f;
// A walk across a deck that the straight line cannot take (a mast on the centreline) is searched on a 1-yd grid: one
// search expands at most DECK_PATH_CELLS cells, one exit search at most DECK_SEARCH_CELLS over all its candidates
constexpr std::uint32_t DECK_PATH_CELLS = 256;
constexpr std::uint32_t DECK_SEARCH_CELLS = 768;
// The route's next point lies this near the ground mesh (a graph node's spot)
constexpr float ONWARD_SNAP_YARDS = 2.0f;

// The body ray of a boarding or exit crossing runs level, this high above the higher end. A ray from the lower end's
// body height to the higher end's would pass through the deck's own floor while still over it (a step up of 1.7 yd
// onto a zeppelin's deck from its tower crosses the deck plane before it ends).
inline float CrossingRayHeight(Point const& start, Point const& end)
{
    return std::max(start.Z, end.Z) + CROSSING_BODY_YARDS;
}

inline float DistanceSquared(Point const& a, Point const& b)
{
    float const dx = a.X - b.X;
    float const dy = a.Y - b.Y;
    float const dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

struct DockPose
{
    Point Position;
    float Orientation = 0.0f;
};

inline bool SameDock(DockPose const& issued, DockPose const& current)
{
    return DistanceSquared(issued.Position, current.Position) <= 0.2f * 0.2f &&
           std::cos(issued.Orientation - current.Orientation) >= 0.99995f;
}

struct Footprint
{
    Point Low;
    Point High;
    float Clearance = 0.0f;

    bool Outside(Point const& point) const
    {
        return point.X < Low.X - Clearance || point.X > High.X + Clearance || point.Y < Low.Y - Clearance ||
               point.Y > High.Y + Clearance;
    }
};

// The first check a boarding or exit candidate fails; the tally of a failed search goes to the debug trace.
enum class CrossingCheck : std::uint8_t
{
    Accepted,
    NoShore,
    NoDeck,
    OnHull,
    Height,
    Distance,
    Ground,
    Onward,
    Crossing,
    Corridor,
    DeckWalk,
    Count
};

struct CrossingTally
{
    struct Sample
    {
        Point From;
        Point To;
    };
    static constexpr std::size_t REASONS = static_cast<std::size_t>(CrossingCheck::Count);
    std::array<std::uint32_t, REASONS> Counts{};
    std::array<std::optional<Sample>, REASONS> First{};

    // The exit probe: per bearing of the exit search, the candidate that got furthest through the checks (the nearest
    // one of equal progress). The checks run in the enum's order.
    struct Furthest
    {
        CrossingCheck Check = CrossingCheck::NoShore;
        float Distance = 0.0f;
        Point From;
        Point To;
    };
    static constexpr std::size_t BEARINGS = CROSSING_BEARINGS;
    std::array<std::optional<Furthest>, BEARINGS> Bearings{};
    std::optional<Point> Onward;  // the exit search's onward point (OnwardPoint)
    std::uint32_t DeckCells = 0;  // the cells its deck walk searches expanded

    void NoteBearing(std::size_t bearing, CrossingCheck check, float distance, Point const& from, Point const& to)
    {
        auto& slot = Bearings[bearing];
        if (!slot || check > slot->Check)
            slot = Furthest{check, distance, from, to};
    }

    void Note(CrossingCheck check, Point const& from, Point const& to)
    {
        std::size_t const index = static_cast<std::size_t>(check);
        ++Counts[index];
        if (!First[index])
            First[index] = Sample{from, to};
    }
};

inline void Note(CrossingTally* tally, CrossingCheck check, Point const& from, Point const& to)
{
    if (tally)
        tally->Note(check, from, to);
}

// The deck point a crossing starts from: on the line from the shore toward a point over the deck (the hull's centre:
// a transport's origin need not lie over its deck), the first sample with deck under it, then the inset further in, no
// further than that point. The search steps no further than the inset, so the point lies between one and two insets
// inside the edge (boarding: 4 to 6 yd; exit: 1 to 2 yd). Without an edge on that line, the point aimed at.
template <class DeckAt>
std::optional<Point> DeckPointFrom(Point const& shore, Point const& toward, float inset, DeckAt const& deckAt)
{
    constexpr float STEP_YARDS = 2.0f;
    constexpr float PROBE_YARDS = 100.0f;
    float const step = std::min(STEP_YARDS, inset);
    float const dx = toward.X - shore.X;
    float const dy = toward.Y - shore.Y;
    float const yards = std::hypot(dx, dy);
    if (yards > PROBE_YARDS)
        return std::nullopt;
    for (float along = step; along < yards; along += step)
    {
        float const part = along / yards;
        auto const edge = deckAt(Point{shore.X + dx * part, shore.Y + dy * part, shore.Z});
        if (!edge)
            continue;
        float const inward = std::min((along + inset) / yards, 1.0f);
        Point const deck{shore.X + dx * inward, shore.Y + dy * inward, *edge};
        if (auto const height = deckAt(deck))
            return Point{deck.X, deck.Y, *height};
    }
    if (auto const height = deckAt(Point{toward.X, toward.Y, shore.Z}))
        return Point{toward.X, toward.Y, *height};
    return std::nullopt;
}

// The walk across a deck follows its surface yard by yard: the surface under each step, looked for from one climb above
// the last step down to a drop of two climbs, and the body rays between two steps level above the higher one. One level
// ray at the bot's height over a 16-yd walk runs into the deck's own camber or a raised quarterdeck. deckAt(top, drop)
// is the surface height at or below top within drop; blocks(a, b) is the transport's model between two points. The
// walk's end on the deck's surface, or nothing.
template <class DeckAt, class Blocks>
std::optional<Point> FollowDeckTo(Point const& start, Point const& end, float radius, DeckAt const& deckAt,
                                  Blocks const& blocks)
{
    float const dx = end.X - start.X;
    float const dy = end.Y - start.Y;
    float const length = std::hypot(dx, dy);
    if (length < 0.1f)
        return start;
    unsigned const steps = static_cast<unsigned>(std::ceil(length));
    Point last = start;
    for (unsigned step = 1; step <= steps; ++step)
    {
        float const part = static_cast<float>(step) / static_cast<float>(steps);
        Point const top{start.X + dx * part, start.Y + dy * part, last.Z + DECK_CLIMB_YARDS};
        auto const surface = deckAt(top, 3.0f * DECK_CLIMB_YARDS);
        if (!surface)
            return std::nullopt;
        Point const next{top.X, top.Y, *surface};
        float const level = CrossingRayHeight(last, next);
        for (float const side : {0.0f, -radius, radius})
        {
            float const x = -dy / length * side;
            float const y = dx / length * side;
            if (blocks(Point{last.X + x, last.Y + y, level}, Point{next.X + x, next.Y + y, level}))
                return std::nullopt;
        }
        last = next;
    }
    return last;
}

template <class DeckAt, class Blocks>
bool FollowDeck(Point const& start, Point const& end, float radius, DeckAt const& deckAt, Blocks const& blocks)
{
    return FollowDeckTo(start, end, radius, deckAt, blocks).has_value();
}

// The walk across a deck from start to end: its corners after start, ending at end. The straight walk when FollowDeck
// clears it; else a bounded A* over a 1-yd grid laid from start, each move FollowDeck's yard step to one of the eight
// neighbours (a cell's height is the surface its step found), reduced to the cells where the walk turns. Nothing when
// no walk is found within `cells` expanded cells; `expanded` counts them.
template <class DeckAt, class Blocks>
std::optional<std::vector<Point>> DeckPath(Point const& start, Point const& end, float radius, DeckAt const& deckAt,
                                           Blocks const& blocks, std::uint32_t cells, std::uint32_t& expanded)
{
    expanded = 0;
    if (FollowDeck(start, end, radius, deckAt, blocks))
        return std::vector<Point>{end};
    constexpr int SPAN = 64;  // grid cells to each side of start
    constexpr int ROW = 2 * SPAN + 1;
    constexpr float GOAL_YARDS = 1.5f;  // a cell this near the end tries the last step to it
    constexpr float DIAGONAL_YARDS = 1.41421356f;
    constexpr std::array<int, 8> STEP_X = {1, -1, 0, 0, 1, 1, -1, -1};
    constexpr std::array<int, 8> STEP_Y = {0, 0, 1, -1, 1, -1, 1, -1};
    struct Cell
    {
        Point At;
        float Cost = 0.0f;
        int Parent = -1;
        bool Closed = false;
    };
    auto const toEnd = [&end](Point const& at) { return std::hypot(end.X - at.X, end.Y - at.Y); };
    std::unordered_map<int, Cell> grid;
    using Open = std::pair<float, int>;  // the walk's estimate through a cell, the cell
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    int const first = SPAN * ROW + SPAN;
    grid[first] = Cell{start};
    open.push({toEnd(start), first});
    int goal = -1;
    while (!open.empty() && expanded < cells)
    {
        int const key = open.top().second;
        open.pop();
        Cell& cell = grid[key];
        if (cell.Closed)
            continue;
        cell.Closed = true;
        ++expanded;
        if (toEnd(cell.At) <= GOAL_YARDS && FollowDeck(cell.At, end, radius, deckAt, blocks))
        {
            goal = key;
            break;
        }
        for (std::size_t move = 0; move < STEP_X.size(); ++move)
        {
            int const column = key % ROW + STEP_X[move];
            int const row = key / ROW + STEP_Y[move];
            if (column < 0 || column >= ROW || row < 0 || row >= ROW)
                continue;
            int const next = row * ROW + column;
            float const cost = cell.Cost + (STEP_X[move] && STEP_Y[move] ? DIAGONAL_YARDS : 1.0f);
            auto const known = grid.find(next);
            if (known != grid.end() && (known->second.Closed || known->second.Cost <= cost))
                continue;
            Point const target{start.X + static_cast<float>(column - SPAN), start.Y + static_cast<float>(row - SPAN),
                               cell.At.Z};
            auto const step = FollowDeckTo(cell.At, target, radius, deckAt, blocks);
            if (!step)
                continue;
            grid[next] = Cell{*step, cost, key, false};
            open.push({cost + toEnd(*step), next});
        }
    }
    if (goal < 0)
        return std::nullopt;
    std::vector<Point> walk{end};
    for (int key = goal; grid[key].Parent >= 0; key = grid[key].Parent)
        walk.push_back(grid[key].At);
    walk.push_back(start);
    std::reverse(walk.begin(), walk.end());
    std::vector<Point> corners;
    for (std::size_t i = 1; i + 1 < walk.size(); ++i)
    {
        float const inX = walk[i].X - walk[i - 1].X;
        float const inY = walk[i].Y - walk[i - 1].Y;
        float const outX = walk[i + 1].X - walk[i].X;
        float const outY = walk[i + 1].Y - walk[i].Y;
        float const in = std::hypot(inX, inY);
        float const out = std::hypot(outX, outY);
        if (in < 0.01f || out < 0.01f)
            continue;
        if (std::fabs(inX * outY - inY * outX) > 0.01f * in * out || inX * outX + inY * outY <= 0.0f)
            corners.push_back(walk[i]);
    }
    corners.push_back(end);
    return corners;
}

template <class Geometry>
CrossingCheck CheckBoard(Geometry const& geometry, Point const& shore, Point const& deck)
{
    if (std::fabs(shore.Z - deck.Z) > DOCK_FLOOR_YARDS)
        return CrossingCheck::Height;
    if (DistanceSquared(shore, deck) > CROSSING_YARDS * CROSSING_YARDS)
        return CrossingCheck::Distance;
    if (!geometry.OnGround(shore))
        return CrossingCheck::Ground;
    if (!geometry.CrossingClear(shore, deck))
        return CrossingCheck::Crossing;
    return CrossingCheck::Accepted;
}

template <class Geometry>
bool AcceptBoard(Geometry const& geometry, Point const& shore, Point const& deck)
{
    return CheckBoard(geometry, shore, deck) == CrossingCheck::Accepted;
}

// The quay a bot boards from: where it stands, else on the ring around the transport's origin, at its floor or within
// QUAY_LEVEL_YARDS of it (a bot above the platform); its deck point lies toward the hull's centre (a zeppelin's origin
// need not lie over its deck: a line toward it ends on the rail). Water is no quay: it is rejected before the deck
// point's ray casts
template <class Geometry>
std::optional<Point> FindQuay(Geometry const& geometry, Point const& approach, Point const& origin, Point const& centre,
                              CrossingTally* tally = nullptr)
{
    auto const tryShore = [&](Point const& seed, float vertical) -> std::optional<Point>
    {
        auto const shore = geometry.ShoreAt(seed, vertical);
        if (!shore)
        {
            Note(tally, CrossingCheck::NoShore, approach, seed);
            return std::nullopt;
        }
        if (!geometry.OnGround(*shore))
        {
            Note(tally, CrossingCheck::Ground, approach, *shore);
            return std::nullopt;
        }
        auto const deck = geometry.DeckFrom(*shore, BOARD_DECK_INSET_YARDS, centre);
        if (!deck)
        {
            Note(tally, CrossingCheck::NoDeck, *shore, centre);
            return std::nullopt;
        }
        CrossingCheck check = CheckBoard(geometry, *shore, *deck);
        if (check == CrossingCheck::Accepted && !geometry.ConnectedGround(approach, *shore))
            check = CrossingCheck::Corridor;
        if (check == CrossingCheck::Accepted)
            return shore;
        Note(tally, check, *shore, *deck);
        return std::nullopt;
    };
    if (auto const shore = tryShore(approach, DOCK_FLOOR_YARDS))
        return shore;

    // Sample around the model horizontally, using the normal approach's floor as the height reference.
    for (unsigned bearing = 0; bearing < CROSSING_BEARINGS; ++bearing)
    {
        float const angle = static_cast<float>(bearing) * CROSSING_BEARING_STEP;
        float const dx = std::cos(angle);
        float const dy = std::sin(angle);
        for (float distance = 1.0f; distance <= CROSSING_YARDS; distance += 1.0f)
            if (auto const shore =
                    tryShore({origin.X + dx * distance, origin.Y + dy * distance, approach.Z}, QUAY_LEVEL_YARDS))
                return shore;
    }
    return std::nullopt;
}

// The mesh adapter supplies actual ground and a complete ground corridor. The deck crossing is checked separately.
// Off the hull means no transport surface under the point (the geometry's model test, not the model's bounding box).
template <class Geometry>
CrossingCheck CheckExit(Geometry const& geometry, Point const& from, Point const& ground, Point const& onward)
{
    if (!geometry.OffHull(ground) || !geometry.OffHull(onward))
        return CrossingCheck::OnHull;
    if (std::fabs(ground.Z - from.Z) > EXIT_STEP_YARDS)
        return CrossingCheck::Height;
    if (DistanceSquared(from, ground) > CROSSING_YARDS * CROSSING_YARDS)
        return CrossingCheck::Distance;
    if (!geometry.OnGround(ground))
        return CrossingCheck::Ground;
    if (!geometry.CrossingClear(from, ground))
        return CrossingCheck::Crossing;
    if (!geometry.GroundLeadsOn(ground, onward))
        return CrossingCheck::Corridor;
    return CrossingCheck::Accepted;
}

template <class Geometry>
bool AcceptExit(Geometry const& geometry, Point const& from, Point const& ground, Point const& onward)
{
    return CheckExit(geometry, from, ground, onward) == CrossingCheck::Accepted;
}

struct ExitPath
{
    Point Deck;               // the deck edge, reached across the deck
    Point Ground;             // the quay beyond it
    Point Onward;             // the route's ground the quay leads on to
    std::vector<Point> Walk;  // the deck walk's corners, ending at Deck
};

// The route's first point that lies off the hull on ground: where the bot goes after the quay.
template <class Geometry>
std::optional<Point> OnwardPoint(Geometry const& geometry, std::vector<Point> const& route)
{
    for (Point const& point : route)
        if (geometry.OffHull(point))
            if (auto const ground = geometry.SnapGround(point, ONWARD_SNAP_YARDS, DOCK_FLOOR_YARDS))
                if (geometry.OffHull(*ground))
                    return ground;
    return std::nullopt;
}

// The exit is the boarding search in reverse: a quay on the ring around the transport's origin at the deck's floor
// (bearings toward the route's onward point first), its deck edge (DeckFrom toward the hull's centre), the crossing
// from the edge to the quay, the quay's ground leading on to the onward point (GroundLeadsOn: a far point counts, an
// island does not), and a walk across the deck to the edge (DeckPath, around masts and fittings). The quay may lie a
// step (EXIT_STEP_YARDS) above or below the deck; the deck edge lies 1 yd inside the deck. The deck edge is searched
// toward the centre, not the origin: a transport's origin need not lie over its deck (a zeppelin's lies on its
// gondola's rail, so a line toward it ends on the rail, whose face blocks the crossing). The candidates keep their
// order; all their deck walk searches together expand at most DECK_SEARCH_CELLS cells.
template <class Geometry>
std::optional<ExitPath> FindExit(Geometry const& geometry, Point const& from, Point const& origin, Point const& centre,
                                 std::vector<Point> const& route, CrossingTally* tally = nullptr)
{
    constexpr unsigned BEARINGS = CrossingTally::BEARINGS;
    constexpr float BEARING_STEP = CROSSING_BEARING_STEP;
    auto const onward = OnwardPoint(geometry, route);
    if (tally)
        tally->Onward = onward;
    if (!onward)
    {
        Note(tally, CrossingCheck::Onward, from, route.empty() ? origin : route.front());
        return std::nullopt;
    }
    float const angle = std::atan2(onward->Y - origin.Y, onward->X - origin.X);
    std::uint32_t deckBudget = DECK_SEARCH_CELLS;
    std::vector<Point> walkless;  // deck edges no walk reaches
    for (unsigned turn = 0; turn < BEARINGS; ++turn)
    {
        int const side = static_cast<int>((turn + 1) / 2) * (turn % 2 ? 1 : -1);
        float const direction = angle + static_cast<float>(side) * BEARING_STEP;
        float const dx = std::cos(direction);
        float const dy = std::sin(direction);
        for (float distance = 1.0f; distance <= CROSSING_YARDS; distance += 1.0f)
        {
            auto const reject = [&](CrossingCheck check, Point const& start, Point const& end)
            {
                Note(tally, check, start, end);
                if (tally)
                    tally->NoteBearing(turn, check, distance, start, end);
            };
            Point const seed{origin.X + dx * distance, origin.Y + dy * distance, from.Z};
            auto const ground = geometry.ShoreAt(seed, EXIT_STEP_YARDS);
            if (!ground)
            {
                reject(CrossingCheck::NoShore, from, seed);
                continue;
            }
            auto const deck = geometry.DeckFrom(*ground, EXIT_DECK_INSET_YARDS, centre);
            if (!deck)
            {
                reject(CrossingCheck::NoDeck, *ground, centre);
                continue;
            }
            CrossingCheck const check = CheckExit(geometry, *deck, *ground, *onward);
            if (check != CrossingCheck::Accepted)
            {
                reject(check, *deck, *ground);
                continue;
            }
            bool const known = std::any_of(walkless.begin(), walkless.end(),
                                           [&deck](Point const& edge) { return DistanceSquared(edge, *deck) < 1.0f; });
            std::uint32_t expanded = 0;
            auto walk =
                known ? std::nullopt : geometry.DeckPath(from, *deck, std::min(deckBudget, DECK_PATH_CELLS), expanded);
            deckBudget -= std::min(deckBudget, expanded);
            if (tally)
                tally->DeckCells += expanded;
            if (!walk)
            {
                if (!known)
                    walkless.push_back(*deck);
                reject(CrossingCheck::DeckWalk, from, *deck);
                continue;
            }
            return ExitPath{*deck, *ground, *onward, std::move(*walk)};
        }
    }
    return std::nullopt;
}

// WT60: leaving is boarding in reverse. At a stop with a wait point the exit takes that point, the graph's ground: its
// deck point is the boarding's (DeckFrom toward the hull's centre, BOARD_DECK_INSET_YARDS in), the crossing between
// them passes the boarding's checks (CheckBoard), and the rider walks across the deck to that deck point (DeckPath).
// No ground to leave on is searched for, and the wait point is not tested off the hull: a zeppelin's gondola docks
// within a yard of its tower's wait point. gate: Accepted, or the first check that failed (NoDeck, Height, Distance,
// Ground, Crossing, DeckWalk). The exit's onward point is the wait point itself: the walk on is the next leg's
template <class Geometry>
std::optional<ExitPath> FindWaitExit(Geometry const& geometry, Point const& from, Point const& wait,
                                     Point const& centre, CrossingCheck& gate)
{
    auto const deck = geometry.DeckFrom(wait, BOARD_DECK_INSET_YARDS, centre);
    if (!deck)
    {
        gate = CrossingCheck::NoDeck;
        return std::nullopt;
    }
    gate = CheckBoard(geometry, wait, *deck);
    if (gate != CrossingCheck::Accepted)
        return std::nullopt;
    std::uint32_t expanded = 0;
    auto walk = geometry.DeckPath(from, *deck, DECK_PATH_CELLS, expanded);
    if (!walk)
    {
        gate = CrossingCheck::DeckWalk;
        return std::nullopt;
    }
    return ExitPath{*deck, wait, wait, std::move(*walk)};
}

enum class MotionKind : std::uint8_t
{
    Board,
    Exit
};
enum class MotionState : std::uint8_t
{
    None,
    Moving,
    Reached,
    Cancelled
};
enum class Outcome : std::uint8_t
{
    Pending,
    Boarded,
    Exited,
    Cancelled,
    Lost
};
enum class Contact : std::uint8_t
{
    Unavailable,
    OffSurface,
    AboveDeck,
    Feet
};
enum class DockState : std::uint8_t
{
    Unavailable,
    Still,
    Departed
};

struct PassengerMotion
{
    MotionKind Kind = MotionKind::Board;
    std::uint64_t Transport = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Map = 0;
    std::uint32_t Spline = 0;
    Point Endpoint;
    Outcome Result = Outcome::Pending;
    bool CompletionSampled = false;
    bool ContactOnly = false;  // the existing unseen hop has no issued spline
    DockPose Dock;
    Point ExitFrom;
    Point Onward;
    bool ToWaitPoint = false;  // an exit to the stop's wait point (FindWaitExit)
};

// An exit holds where the bot stands at its end, at the dock it was issued at: the found crossing's checks again
// (CheckExit from its deck edge), or for an exit to the wait point the boarding's from there (CheckBoard)
template <class Geometry>
bool ExitHolds(Geometry const& geometry, PassengerMotion const& motion, Point const& at)
{
    if (motion.ToWaitPoint)
        return CheckBoard(geometry, at, motion.ExitFrom) == CrossingCheck::Accepted;
    return AcceptExit(geometry, motion.ExitFrom, at, motion.Onward);
}

struct SplineObservation
{
    std::uint32_t Map = 0;
    std::uint32_t Id = 0;
    bool Local = false;
    bool Finalized = false;
    bool Eligible = false;
    Point Position;
};

inline bool MatchesSpline(PassengerMotion const& motion, SplineObservation const& spline)
{
    return spline.Map == motion.Map && spline.Id == motion.Spline && spline.Local == (motion.Kind == MotionKind::Exit);
}

inline MotionState ClassifyMotion(PassengerMotion const& motion, SplineObservation const& spline)
{
    if (!spline.Eligible || spline.Map != motion.Map || (!motion.ContactOnly && !MatchesSpline(motion, spline)))
        return MotionState::Cancelled;
    if (!spline.Finalized)
        return motion.ContactOnly ? MotionState::Cancelled : MotionState::Moving;

    constexpr float ENDPOINT_TOLERANCE_YARDS = 0.8f;
    return DistanceSquared(spline.Position, motion.Endpoint) <= ENDPOINT_TOLERANCE_YARDS * ENDPOINT_TOLERANCE_YARDS
               ? MotionState::Reached
               : MotionState::Cancelled;
}

template <class Adapter>
void SettleMotion(PassengerMotion const& motion, Adapter& adapter)
{
    if (!motion.ContactOnly)
    {
        SplineObservation const spline = adapter.ReadSpline();
        if (!spline.Finalized && MatchesSpline(motion, spline))
            adapter.SettleOldFrame();
    }
}

template <class Adapter>
void DiscardMotion(std::optional<PassengerMotion>& intent, Adapter& adapter)
{
    if (intent)
        SettleMotion(*intent, adapter);
    intent.reset();
}

template <class Adapter>
void ResetMotion(std::optional<PassengerMotion>& intent, bool continuingTrip, Adapter& adapter)
{
    if (!intent)
        return;
    SettleMotion(*intent, adapter);
    if (!continuingTrip || intent->Kind != MotionKind::Board || intent->Result != Outcome::Boarded)
        intent.reset();
}

struct Cadence
{
    std::uint32_t LastProbeMs = 0;
    std::uint32_t Map = 0;
    bool Sampled = false;
    bool Revalidate = false;
    std::uint8_t OffSurface = 0;  // the rider's samples off its deck in a row (OFF_SURFACE_SAMPLES)
};

struct Membership
{
    std::uint64_t Guid = 0;
    std::uint32_t Entry = 0;
    bool Available = false;
};

// Before a passenger attach (ported from mod-playerbots PR #2747, MovementAction::ClearTransportState, by Keleborn):
// the core's AddPassenger deadlocks the map when the bot still points at a transport whose passenger set lacks it, and
// RemovePassenger clears the pointer only for a listed passenger. A listed passenger of the target stays; any other
// prior transport frame is cleared first.
enum class AttachStep : std::uint8_t
{
    Attach,
    ClearThenAttach,
    Keep
};

inline AttachStep StepAttach(bool hasPrior, bool priorIsTarget, bool listedOnPrior)
{
    if (!hasPrior)
        return AttachStep::Attach;
    return priorIsTarget && listedOnPrior ? AttachStep::Keep : AttachStep::ClearThenAttach;
}

inline std::optional<Outcome> PassengerOutcome(PassengerMotion const& motion, Membership const& membership)
{
    if (motion.Result == Outcome::Pending || !membership.Available)
        return std::nullopt;
    if (motion.Result == Outcome::Boarded && (membership.Guid != motion.Transport || membership.Entry != motion.Entry))
        return Outcome::Lost;
    return motion.Result;
}

struct Surface
{
    std::uint64_t Guid = 0;
    std::uint32_t Entry = 0;
    Contact Where = Contact::Unavailable;
};

struct Sample
{
    Membership Before;
    Membership After;
    Surface Observed;
    MotionState Motion = MotionState::None;
    DockState Dock = DockState::Unavailable;
};

// The adapter resolves transport identities and applies the frame change on the bot's map thread.
template <class Adapter>
Sample Reconcile(Adapter& adapter, PassengerMotion* motion, MotionState state, std::uint64_t preferred = 0,
                 std::uint8_t* offSurfaceSamples = nullptr)
{
    Sample sample;
    sample.Before = sample.After = adapter.ReadMembership();
    sample.Motion = state;
    if (!sample.Before.Available)
        return sample;

    if (motion && state == MotionState::Cancelled)
        SettleMotion(*motion, adapter);

    if (motion && motion->Kind == MotionKind::Exit && sample.Before.Guid != motion->Transport)
    {
        SettleMotion(*motion, adapter);
        motion->Result = Outcome::Lost;
        return sample;
    }

    if (motion && (state == MotionState::Moving || state == MotionState::Reached))
    {
        sample.Dock = adapter.ObserveDock(*motion);
        if (sample.Dock == DockState::Unavailable)
            return sample;
        if (sample.Dock == DockState::Departed)
        {
            // Only a still-owned active spline can be stopped here. A replacement belongs to its new owner.
            SettleMotion(*motion, adapter);
            state = sample.Motion = MotionState::Cancelled;
        }
    }
    if (motion && state == MotionState::Cancelled)
        motion->Result = Outcome::Cancelled;

    // The issued spline stays in its frame until it completes or another owner replaces it.
    if (state == MotionState::Moving)
        return sample;

    std::uint64_t const expected = motion ? motion->Transport : preferred;
    sample.Observed = adapter.Probe(sample.Before.Guid ? sample.Before.Guid : expected, sample.Before.Guid != 0);

    bool const offSurface = sample.Before.Guid && sample.Observed.Guid == sample.Before.Guid &&
                            sample.Observed.Where == Contact::OffSurface && !preferred;
    bool const leaves = offSurface && (motion || !offSurfaceSamples || ++*offSurfaceSamples >= OFF_SURFACE_SAMPLES);
    if (offSurfaceSamples && (!offSurface || leaves))
        *offSurfaceSamples = 0;
    if (leaves)
    {
        bool const safeExit = motion && motion->Kind == MotionKind::Exit && state == MotionState::Reached &&
                              motion->Transport == sample.Before.Guid && adapter.ConfirmExit(*motion);
        adapter.SettleOldFrame();
        adapter.Detach(sample.Before.Guid);
        sample.After = adapter.ReadMembership();
        if (sample.After.Available && !sample.After.Guid)
        {
            adapter.PublishFrame();
            if (motion)
                motion->Result = safeExit ? Outcome::Exited : Outcome::Lost;
        }
    }
    else if (!sample.Before.Guid && sample.Observed.Guid && (!expected || sample.Observed.Guid == expected) &&
             (!motion || state == MotionState::Reached) && sample.Observed.Where == Contact::Feet)
    {
        adapter.SettleOldFrame();
        adapter.Attach(sample.Observed.Guid);
        sample.After = adapter.ReadMembership();
        if (sample.After.Available && sample.After.Guid == sample.Observed.Guid)
            adapter.PublishFrame();
    }

    if (motion && motion->Result == Outcome::Pending && state == MotionState::Reached)
    {
        if (motion->Kind == MotionKind::Board && sample.After.Available && sample.After.Guid == motion->Transport &&
            sample.Observed.Where == Contact::Feet)
            motion->Result = Outcome::Boarded;
        else if (sample.Observed.Where != Contact::Unavailable && sample.After.Available)
            motion->Result = Outcome::Cancelled;
    }
    return sample;
}

template <class Adapter>
void Maintain(Cadence& cadence, std::uint32_t nowMs, std::uint32_t map, PassengerMotion* motion, Adapter& adapter)
{
    if (motion && motion->Result != Outcome::Pending)
        motion = nullptr;

    MotionState const state = motion ? ClassifyMotion(*motion, adapter.ReadSpline()) : MotionState::None;
    bool const completed =
        motion && !motion->CompletionSampled && (state == MotionState::Reached || state == MotionState::Cancelled);
    std::uint32_t const gapMs = cadence.Sampled ? nowMs - cadence.LastProbeMs : 0;
    if (cadence.Sampled && gapMs < PASSENGER_INTERVAL_MS && !completed && !cadence.Revalidate && cadence.Map == map)
        return;

    cadence = {nowMs, map, true, false, cadence.OffSurface};
    if (completed)
        motion->CompletionSampled = true;

    adapter.Observe(nowMs, gapMs, motion, [&] { return Reconcile(adapter, motion, state, 0, &cadence.OffSurface); });
}
}  // namespace TravelLifecycle

#endif
