/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FLIGHTRULES_H
#define _PLAYERBOT_FLIGHTRULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Flying legs over Outland and Northrend (upkeep plan WU9, PLAN 3.9): a coarse height map and a pure flight search
// over it. The search is a grid A* whose cost is the distance flown plus a surcharge per yard climbed, so a low ridge
// is flown over and a high range around or through a pass; grid zigzags are cut into straight segments where that
// passes no higher ground; the route holds its height in stretches (a climb, a few level stretches, a descent). The
// map holds terrain and liquid surfaces only, no buildings: the follower checks the real height at each waypoint;
// only the cells under a model floating in the air are closed (FloatsOver).
// Game-free: tests/flight-rules-selftest.cpp builds this header alone.
namespace FlightRules
{
// A cell no bot flies over: a no-fly or non-flying area, no terrain file, or no valid height
constexpr std::int16_t CLOSED = std::numeric_limits<std::int16_t>::min();
// A point this far above its open cell's ground floats on a platform (Dalaran over Crystalsong Forest): the map's
// terrain areas say nothing of the WMO's no-fly areas up there
constexpr float PLATFORM_YARDS = 100.0f;
// A WMO whose lowest point is this far above a cell's ground floats over it (WU11b fix 12: Zul'Drak's necropolis
// Voltarus, 95 yd over Zeramas; the Nexus' platforms over Coldarra): a plan at the surrounding ground's level flies
// into it, the core's indoor check takes the mount, the bot comes down
constexpr float FLOATING_MODEL_YARDS = 50.0f;

// The cell lies under a model floating in the air (its lowest point modelBottom): the flight map closes it, so routes
// pass around the model as around a no-fly area. A model standing on the ground (its bottom at or below the ground,
// or a bridge low over a river) leaves it to the follower's real-height check
inline bool FloatsOver(float modelBottom, float cellGround) { return modelBottom - cellGround > FLOATING_MODEL_YARDS; }

// A place on a platform where a bot may take off and land (WU11b fix 7, Krasus' Landing): a taxi node whose WMO area
// lets a bot mount a flyer there, found when the map is built (FlightMap.cpp)
struct TakeoffSpot
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct FlightPoint
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// The search's numbers. The server takes the first five from UpkeepConfig (PLAN 3.9's estimates); the defaults here
// are the same values, for the selftest
struct FlightParams
{
    float cellYards{67.0f};          // the height-map cell (the loader rounds it to a whole fraction of a grid)
    float stretchYards{500.0f};      // a stretch of the height profile
    float clearanceMinYards{30.0f};  // the clearance of the most daring bot
    float clearanceMaxYards{50.0f};  // and of the most careful one
    float groundHopYards{150.0f};    // a shorter hop stays on the ground
    float climbSurcharge{3.0f};      // the extra cost of a yard climbed, in yards flown
    float offsetMaxYards{30.0f};     // the widest sideways offset of a bot's line, to either side
    float offsetRampYards{200.0f};   // the offset grows over this distance from each end of the leg
    float waypointMaxYards{200.0f};  // the longest gap between two waypoints
    float cutSlackYards{10.0f};      // a cut corner may pass this much higher ground than the cells it replaces
    std::uint32_t holdLookahead{2};  // the stretches ahead a descent looks at, so the route does not dip into a valley
};

// One map's highest ground per cell, immutable once built
struct HeightMap
{
    std::uint32_t mapId{0};
    float originX{0.0f};  // the world corner of cell (0, 0): its lowest x and y
    float originY{0.0f};
    float cellYards{0.0f};
    std::int32_t width{0};   // cells along x
    std::int32_t height{0};  // cells along y
    // The highest ground (terrain or liquid surface) per cell in whole yards, rounded up, or CLOSED; y * width + x
    std::vector<std::int16_t> ground;
    std::vector<TakeoffSpot> takeoffs;  // the platforms' takeoff spots
    // Build facts for the log and the status lines
    std::uint32_t files{0};
    std::uint32_t buildMs{0};
    std::uint32_t openCells{0};
    std::uint32_t closedCells{0};    // inside the box
    std::uint32_t floatingCells{0};  // closed under a floating model (FloatsOver), before the crop
    std::uint32_t darkCells{0};      // closed over dark water (the sea's fatigue), before the crop
    std::int32_t minGround{0};
    std::int32_t maxGround{0};

    bool Empty() const { return width <= 0 || height <= 0 || ground.empty() || cellYards <= 0.0f; }
    std::int32_t CellX(float x) const { return static_cast<std::int32_t>(std::floor((x - originX) / cellYards)); }
    std::int32_t CellY(float y) const { return static_cast<std::int32_t>(std::floor((y - originY) / cellYards)); }
    float CentreX(std::int32_t cx) const { return originX + (static_cast<float>(cx) + 0.5f) * cellYards; }
    float CentreY(std::int32_t cy) const { return originY + (static_cast<float>(cy) + 0.5f) * cellYards; }

    std::int16_t Ground(std::int32_t cx, std::int32_t cy) const
    {
        if (cx < 0 || cy < 0 || cx >= width || cy >= height)
            return CLOSED;
        return ground[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(cx)];
    }

    bool Open(std::int32_t cx, std::int32_t cy) const { return Ground(cx, cy) != CLOSED; }
    bool IsOpen(float x, float y) const { return !Empty() && Open(CellX(x), CellY(y)); }
    std::size_t Bytes() const
    {
        return sizeof(HeightMap) + ground.capacity() * sizeof(std::int16_t) + takeoffs.capacity() * sizeof(TakeoffSpot);
    }

    // After the cells are filled: the open and closed counts and the ground range
    void Finish()
    {
        openCells = 0;
        closedCells = 0;
        minGround = std::numeric_limits<std::int32_t>::max();
        maxGround = std::numeric_limits<std::int32_t>::min();
        for (std::int16_t const cell : ground)
        {
            if (cell == CLOSED)
            {
                ++closedCells;
                continue;
            }
            ++openCells;
            minGround = std::min<std::int32_t>(minGround, cell);
            maxGround = std::max<std::int32_t>(maxGround, cell);
        }
        if (!openCells)
        {
            minGround = 0;
            maxGround = 0;
        }
    }
};

// The published flying maps (Outland, Northrend) and the numbers the server searches them with
struct FlightMapSet
{
    std::vector<HeightMap> maps;
    FlightParams params;
    std::uint32_t buildMs{0};

    HeightMap const* Find(std::uint32_t mapId) const
    {
        for (HeightMap const& map : maps)
            if (map.mapId == mapId)
                return &map;
        return nullptr;
    }
};

// A bot's own way of flying: how high above the ground and how far beside the common line
struct FlightStyle
{
    float clearanceYards{40.0f};
    float offsetYards{0.0f};
};

struct FlightRequest
{
    std::uint32_t mapId{0};
    float startX{0.0f};  // the takeoff point on the ground
    float startY{0.0f};
    float startZ{0.0f};
    float goalX{0.0f};  // the landing point on the ground
    float goalY{0.0f};
    float goalZ{0.0f};
    float speed{0.0f};  // yards per second in the air; 0 leaves the route's seconds at 0
    float clearanceYards{40.0f};
    float offsetYards{0.0f};
};

enum class FlightFailure : std::uint8_t
{
    None,
    NoMap,  // no height map for the request's map
    StartClosed,
    GoalClosed,
    NoPath  // the goal is not reachable through open cells
};

inline char const* FailureName(FlightFailure failure)
{
    switch (failure)
    {
        case FlightFailure::None:
            return "none";
        case FlightFailure::NoMap:
            return "no map";
        case FlightFailure::StartClosed:
            return "start closed";
        case FlightFailure::GoalClosed:
            return "goal closed";
        case FlightFailure::NoPath:
            return "no path";
    }
    return "unknown";
}

struct FlightRoute
{
    bool ok{false};
    FlightFailure failure{FlightFailure::None};
    // The takeoff on the ground, the flight (one waypoint every waypointMaxYards at most) and the landing
    std::vector<FlightPoint> waypoints;
    std::vector<float> levels;       // the held height of each stretch
    float distance{0.0f};            // yards flown, climbs and descents included
    float seconds{0.0f};             // distance / speed
    float climbYards{0.0f};          // every climb added up, the takeoff's included
    std::uint32_t heightChanges{0};  // between stretches (the takeoff and the landing not counted)
    std::uint32_t segments{0};       // straight segments after the corner cut
    std::uint32_t expandedCells{0};  // the search's work
};

// A bot's clearance from its caution (-1 daring .. 1 careful, TraitRules::Unit) and its sideways offset from its guid
inline FlightStyle BotFlightStyle(float caution, std::uint32_t guid, FlightParams const& params = {})
{
    constexpr std::uint32_t GOLDEN = 2654435761u;  // Knuth's multiplicative hash, as the upkeep share rule
    constexpr double HASH_RANGE = 4294967296.0;
    float const unit = (std::clamp(caution, -1.0f, 1.0f) + 1.0f) / 2.0f;
    std::uint32_t const hash = guid * GOLDEN;
    double const spread = static_cast<double>(hash) / HASH_RANGE * 2.0 - 1.0;
    FlightStyle style;
    style.clearanceYards = params.clearanceMinYards + (params.clearanceMaxYards - params.clearanceMinYards) * unit;
    style.offsetYards = static_cast<float>(spread) * params.offsetMaxYards;
    return style;
}

// A hop this short stays on the ground (no search needed)
inline bool IsGroundHop(float distance, FlightParams const& params = {}) { return distance < params.groundHopYards; }

// Cheap: both ends may be flown from; the search may still find no path
inline bool CanFlyBetween(HeightMap const& map, FlightPoint const& a, FlightPoint const& b)
{
    return map.IsOpen(a.x, a.y) && map.IsOpen(b.x, b.y);
}

namespace Detail
{
constexpr std::int32_t CLOSED_SEGMENT = std::numeric_limits<std::int32_t>::min();

// One search's working memory, kept per thread so a search allocates only its result once the buffers have grown
struct Scratch
{
    std::vector<float> cost;
    std::vector<std::int32_t> parent;
    std::vector<std::uint32_t> seen;  // the generation that reached the cell
    std::vector<std::uint32_t> done;  // the generation that expanded it
    std::vector<std::pair<float, std::int32_t>> frontier;
    std::vector<std::int32_t> cells;
    std::vector<FlightPoint> points;  // the cell path as points: the start, the cell centres, the goal
    std::vector<FlightPoint> line;    // the corners left after the cut
    std::vector<FlightPoint> flat;    // the waypoints' x and y
    std::vector<float> along;         // each waypoint's distance along the line
    std::vector<std::int32_t> high;   // the highest ground under each gap between waypoints
    std::uint32_t generation{0};

    void Prepare(std::size_t count)
    {
        if (seen.size() < count)
        {
            cost.resize(count);
            parent.resize(count);
            seen.assign(count, 0u);
            done.assign(count, 0u);
            generation = 0;
        }
        if (++generation == 0)
        {
            std::fill(seen.begin(), seen.end(), 0u);
            std::fill(done.begin(), done.end(), 0u);
            generation = 1;
        }
        frontier.clear();
        cells.clear();
        points.clear();
        line.clear();
        flat.clear();
        along.clear();
        high.clear();
    }
};

inline Scratch& Buffers()
{
    thread_local Scratch scratch;
    return scratch;
}

inline float Distance2d(FlightPoint const& a, FlightPoint const& b) { return std::hypot(b.x - a.x, b.y - a.y); }

// Visits every cell the segment a-b passes, in order; stops at the first visit that returns false
template <typename Visit>
inline bool WalkCells(HeightMap const& map, FlightPoint const& a, FlightPoint const& b, Visit&& visit)
{
    float const fromX = (a.x - map.originX) / map.cellYards;
    float const fromY = (a.y - map.originY) / map.cellYards;
    float const toX = (b.x - map.originX) / map.cellYards;
    float const toY = (b.y - map.originY) / map.cellYards;
    std::int32_t cx = static_cast<std::int32_t>(std::floor(fromX));
    std::int32_t cy = static_cast<std::int32_t>(std::floor(fromY));
    std::int32_t const endX = static_cast<std::int32_t>(std::floor(toX));
    std::int32_t const endY = static_cast<std::int32_t>(std::floor(toY));
    float const dx = toX - fromX;
    float const dy = toY - fromY;
    float const never = std::numeric_limits<float>::infinity();
    std::int32_t const stepX = dx > 0.0f ? 1 : -1;
    std::int32_t const stepY = dy > 0.0f ? 1 : -1;
    float const deltaX = dx != 0.0f ? std::abs(1.0f / dx) : never;
    float const deltaY = dy != 0.0f ? std::abs(1.0f / dy) : never;
    float nextX = dx > 0.0f   ? (std::floor(fromX) + 1.0f - fromX) * deltaX
                  : dx < 0.0f ? (fromX - std::floor(fromX)) * deltaX
                              : never;
    float nextY = dy > 0.0f   ? (std::floor(fromY) + 1.0f - fromY) * deltaY
                  : dy < 0.0f ? (fromY - std::floor(fromY)) * deltaY
                              : never;
    if (!visit(cx, cy))
        return false;
    for (std::int32_t steps = std::abs(endX - cx) + std::abs(endY - cy); steps > 0; --steps)
    {
        if (cy == endY || (cx != endX && nextX < nextY))
        {
            cx += stepX;
            nextX += deltaX;
        }
        else
        {
            cy += stepY;
            nextY += deltaY;
        }
        if (!visit(cx, cy))
            return false;
    }
    return true;
}

// The highest ground under the segment, or CLOSED_SEGMENT when it crosses a closed cell
inline std::int32_t SegmentGround(HeightMap const& map, FlightPoint const& a, FlightPoint const& b)
{
    std::int32_t highest = std::numeric_limits<std::int32_t>::min() + 1;
    bool const open = WalkCells(map, a, b,
                                [&map, &highest](std::int32_t cx, std::int32_t cy)
                                {
                                    std::int16_t const cell = map.Ground(cx, cy);
                                    if (cell == CLOSED)
                                        return false;
                                    highest = std::max<std::int32_t>(highest, cell);
                                    return true;
                                });
    return open ? highest : CLOSED_SEGMENT;
}

// A* over the open cells (8 neighbours, no squeezing past a closed corner): distance plus the surcharge per yard
// climbed between cells; the straight distance is the estimate. Fills s.cells from start to goal
inline bool FindCells(HeightMap const& map, std::int32_t startX, std::int32_t startY, std::int32_t goalX,
                      std::int32_t goalY, FlightParams const& params, Scratch& s, std::uint32_t& expanded)
{
    constexpr std::int32_t NEIGHBOURS = 8;
    constexpr std::int32_t STEP_X[NEIGHBOURS] = {1, -1, 0, 0, 1, 1, -1, -1};
    constexpr std::int32_t STEP_Y[NEIGHBOURS] = {0, 0, 1, -1, 1, -1, 1, -1};
    s.Prepare(static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height));
    std::uint32_t const generation = s.generation;
    float const straight = map.cellYards;
    float const diagonal = map.cellYards * std::sqrt(2.0f);
    auto const index = [&map](std::int32_t cx, std::int32_t cy) { return cy * map.width + cx; };
    auto const estimate = [&map, goalX, goalY](std::int32_t cx, std::int32_t cy)
    { return std::hypot(static_cast<float>(goalX - cx), static_cast<float>(goalY - cy)) * map.cellYards; };
    auto const later = [](std::pair<float, std::int32_t> const& a, std::pair<float, std::int32_t> const& b)
    { return a.first > b.first; };

    std::int32_t const start = index(startX, startY);
    std::int32_t const goal = index(goalX, goalY);
    s.cost[start] = 0.0f;
    s.parent[start] = -1;
    s.seen[start] = generation;
    s.frontier.emplace_back(estimate(startX, startY), start);
    bool reached = false;
    while (!s.frontier.empty())
    {
        std::pop_heap(s.frontier.begin(), s.frontier.end(), later);
        std::int32_t const node = s.frontier.back().second;
        s.frontier.pop_back();
        if (s.done[node] == generation)
            continue;
        s.done[node] = generation;
        ++expanded;
        if (node == goal)
        {
            reached = true;
            break;
        }
        std::int32_t const cx = node % map.width;
        std::int32_t const cy = node / map.width;
        std::int32_t const here = map.ground[node];
        for (std::int32_t direction = 0; direction < NEIGHBOURS; ++direction)
        {
            std::int32_t const nx = cx + STEP_X[direction];
            std::int32_t const ny = cy + STEP_Y[direction];
            std::int16_t const there = map.Ground(nx, ny);
            if (there == CLOSED)
                continue;
            bool const slanted = STEP_X[direction] && STEP_Y[direction];
            if (slanted && (!map.Open(nx, cy) || !map.Open(cx, ny)))
                continue;
            std::int32_t const next = index(nx, ny);
            if (s.done[next] == generation)
                continue;
            float const climb = static_cast<float>(std::max<std::int32_t>(0, there - here));
            float const cost = s.cost[node] + (slanted ? diagonal : straight) + params.climbSurcharge * climb;
            if (s.seen[next] == generation && cost >= s.cost[next])
                continue;
            s.seen[next] = generation;
            s.cost[next] = cost;
            s.parent[next] = node;
            s.frontier.emplace_back(cost + estimate(nx, ny), next);
            std::push_heap(s.frontier.begin(), s.frontier.end(), later);
        }
    }
    if (!reached)
        return false;
    for (std::int32_t node = goal; node != -1; node = s.parent[node])
        s.cells.push_back(node);
    std::reverse(s.cells.begin(), s.cells.end());
    if (s.cells.size() == 1)
        s.cells.push_back(s.cells.front());
    return true;
}

// String pulling: from each corner, the farthest later path point the straight line reaches through open cells
// without passing higher ground (beyond the slack) than the path's own cells between them. Fills s.line
inline void CutCorners(HeightMap const& map, FlightRequest const& request, FlightParams const& params, Scratch& s)
{
    std::size_t const last = s.cells.size() - 1;
    for (std::size_t i = 0; i <= last; ++i)
    {
        std::int32_t const cell = s.cells[i];
        FlightPoint point{map.CentreX(cell % map.width), map.CentreY(cell / map.width), 0.0f};
        if (i == 0)
            point = {request.startX, request.startY, 0.0f};
        else if (i == last)
            point = {request.goalX, request.goalY, 0.0f};
        s.points.push_back(point);
    }
    std::int32_t const slack = static_cast<std::int32_t>(std::lround(params.cutSlackYards));
    s.line.push_back(s.points.front());
    std::size_t anchor = 0;
    while (anchor < last)
    {
        std::size_t best = anchor + 1;
        std::int32_t pathHigh = std::max<std::int32_t>(map.ground[s.cells[anchor]], map.ground[s.cells[best]]);
        for (std::size_t j = anchor + 2; j <= last; ++j)
        {
            pathHigh = std::max<std::int32_t>(pathHigh, map.ground[s.cells[j]]);
            std::int32_t const lineHigh = SegmentGround(map, s.points[anchor], s.points[j]);
            if (lineHigh == CLOSED_SEGMENT || lineHigh > pathHigh + slack)
                break;
            best = j;
        }
        s.line.push_back(s.points[best]);
        anchor = best;
    }
}

// The waypoints along the cut line, the bot's sideways offset on the inner ones (growing from each end), and the
// highest ground under each gap. False when the offset line would cross a closed cell
inline bool LayWaypoints(HeightMap const& map, FlightRequest const& request, FlightParams const& params, float offset,
                         Scratch& s)
{
    s.flat.clear();
    s.along.clear();
    s.high.clear();
    float travelled = 0.0f;
    for (std::size_t k = 1; k < s.line.size(); ++k)
    {
        FlightPoint const& a = s.line[k - 1];
        FlightPoint const& b = s.line[k];
        float const gap = Distance2d(a, b);
        std::uint32_t pieces = std::max<std::uint32_t>(
            1u, static_cast<std::uint32_t>(std::ceil(gap / std::max(params.waypointMaxYards, 1.0f))));
        if (s.line.size() == 2)
            pieces = std::max<std::uint32_t>(pieces, 2u);  // at least one waypoint in the air
        for (std::uint32_t q = 0; q < pieces; ++q)
        {
            float const t = static_cast<float>(q) / static_cast<float>(pieces);
            s.flat.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, 0.0f});
            s.along.push_back(travelled + gap * t);
        }
        travelled += gap;
    }
    s.flat.push_back(s.line.back());
    s.along.push_back(travelled);

    if (offset != 0.0f)
    {
        float const dirX = request.goalX - request.startX;
        float const dirY = request.goalY - request.startY;
        float const length = std::hypot(dirX, dirY);
        float const sideX = length > 0.0f ? -dirY / length : 0.0f;
        float const sideY = length > 0.0f ? dirX / length : 0.0f;
        float const ramp = std::max(params.offsetRampYards, 1.0f);
        for (std::size_t i = 1; i + 1 < s.flat.size(); ++i)
        {
            float const weight = std::min({1.0f, s.along[i] / ramp, (travelled - s.along[i]) / ramp});
            FlightPoint const moved{s.flat[i].x + sideX * offset * weight, s.flat[i].y + sideY * offset * weight, 0.0f};
            if (map.IsOpen(moved.x, moved.y))
                s.flat[i] = moved;
        }
    }

    for (std::size_t i = 0; i + 1 < s.flat.size(); ++i)
    {
        std::int32_t ground = SegmentGround(map, s.flat[i], s.flat[i + 1]);
        if (ground == CLOSED_SEGMENT)
        {
            if (offset != 0.0f)
                return false;
            // A grid step past a closed corner at an end: the ground of the two cells
            ground = std::max<std::int32_t>(map.Ground(map.CellX(s.flat[i].x), map.CellY(s.flat[i].y)),
                                            map.Ground(map.CellX(s.flat[i + 1].x), map.CellY(s.flat[i + 1].y)));
        }
        s.high.push_back(ground);
    }
    return true;
}

// The height profile: per stretch the highest ground under it plus the clearance; the route holds its height and
// changes it only when a stretch needs more than half the clearance above the held height, or when the stretches
// ahead allow more than half the clearance lower. Fills the route's waypoints, levels and totals
inline void Profile(FlightRequest const& request, FlightParams const& params, Scratch const& s, FlightRoute& route)
{
    float const total = s.along.back();
    std::uint32_t const stretches = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(std::lround(total / std::max(params.stretchYards, 1.0f))));
    float const length = std::max(total / static_cast<float>(stretches), 1.0f);
    auto const stretchOf = [stretches, length](float along)
    { return std::min<std::uint32_t>(stretches - 1, static_cast<std::uint32_t>(along / length)); };

    std::vector<float>& levels = route.levels;
    levels.assign(stretches, -std::numeric_limits<float>::infinity());
    for (std::size_t i = 0; i < s.high.size(); ++i)
        for (std::uint32_t k = stretchOf(s.along[i]); k <= stretchOf(s.along[i + 1]); ++k)
            levels[k] = std::max(levels[k], static_cast<float>(s.high[i]));
    for (float& level : levels)
        level += request.clearanceYards;

    float const threshold = request.clearanceYards / 2.0f;
    auto const ahead = [&levels, &params, stretches](std::uint32_t k)
    {
        float highest = levels[k];
        for (std::uint32_t j = k + 1; j < stretches && j <= k + params.holdLookahead; ++j)
            highest = std::max(highest, levels[j]);
        return highest;
    };
    float held = ahead(0);
    for (std::uint32_t k = 0; k < stretches; ++k)
    {
        float const need = levels[k];
        float const next = ahead(k);  // reads only stretches not yet overwritten
        if (need > held + threshold || held - next > threshold)
        {
            held = next;
            if (k)
                ++route.heightChanges;
        }
        levels[k] = held;
    }

    std::size_t const count = s.flat.size();
    route.waypoints.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        float z = levels[stretchOf(s.along[i])];
        if (i == 0)
            z = request.startZ;
        else if (i + 1 == count)
            z = request.goalZ;
        route.waypoints.push_back({s.flat[i].x, s.flat[i].y, z});
    }
    for (std::size_t i = 1; i < count; ++i)
    {
        FlightPoint const& a = route.waypoints[i - 1];
        FlightPoint const& b = route.waypoints[i];
        float const rise = b.z - a.z;
        route.distance += std::hypot(Distance2d(a, b), rise);
        if (rise > 0.0f)
            route.climbYards += rise;
    }
    route.seconds = request.speed > 0.0f ? route.distance / request.speed : 0.0f;
}
}  // namespace Detail

// The flight route between two points of a flyable map; pure and thread-safe (per-thread scratch buffers)
inline FlightRoute Search(HeightMap const& map, FlightRequest const& request, FlightParams const& params = {})
{
    FlightRoute route;
    if (map.Empty() || map.mapId != request.mapId)
    {
        route.failure = FlightFailure::NoMap;
        return route;
    }
    std::int32_t const startX = map.CellX(request.startX);
    std::int32_t const startY = map.CellY(request.startY);
    std::int32_t const goalX = map.CellX(request.goalX);
    std::int32_t const goalY = map.CellY(request.goalY);
    if (!map.Open(startX, startY))
    {
        route.failure = FlightFailure::StartClosed;
        return route;
    }
    if (!map.Open(goalX, goalY))
    {
        route.failure = FlightFailure::GoalClosed;
        return route;
    }
    Detail::Scratch& s = Detail::Buffers();
    if (!Detail::FindCells(map, startX, startY, goalX, goalY, params, s, route.expandedCells))
    {
        route.failure = FlightFailure::NoPath;
        return route;
    }
    Detail::CutCorners(map, request, params, s);
    route.segments = static_cast<std::uint32_t>(s.line.size() - 1);
    if (!Detail::LayWaypoints(map, request, params, request.offsetYards, s))
        Detail::LayWaypoints(map, request, params, 0.0f, s);
    Detail::Profile(request, params, s, route);
    route.ok = true;
    return route;
}

// A map from made-up terrain, for the selftest: height(x, y) and open(x, y) at each cell centre
template <typename HeightAt, typename OpenAt>
inline HeightMap MakeMap(std::uint32_t mapId, float originX, float originY, float cellYards, std::int32_t width,
                         std::int32_t height, HeightAt heightAt, OpenAt openAt)
{
    HeightMap map;
    map.mapId = mapId;
    map.originX = originX;
    map.originY = originY;
    map.cellYards = cellYards;
    map.width = width;
    map.height = height;
    map.ground.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), CLOSED);
    for (std::int32_t cy = 0; cy < height; ++cy)
        for (std::int32_t cx = 0; cx < width; ++cx)
        {
            float const x = map.CentreX(cx);
            float const y = map.CentreY(cy);
            if (!openAt(x, y))
                continue;
            float const ground = std::clamp(std::ceil(static_cast<float>(heightAt(x, y))), -32767.0f, 32767.0f);
            map.ground[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(cx)] =
                static_cast<std::int16_t>(ground);
        }
    map.Finish();
    return map;
}

// One line per route for the selftest's notes: totals, the stretch levels and the z of the waypoints in runs
inline std::string ProfileLine(FlightRoute const& route)
{
    auto const num = [](double value) { return std::to_string(std::llround(value)); };
    if (!route.ok)
        return std::string("no route (") + FailureName(route.failure) + "), " + num(route.expandedCells) + " cells";
    std::string text = num(route.distance) + " yd flown in " + num(route.seconds) + " s, climb " +
                       num(route.climbYards) + " yd, " + num(route.segments) + " segments, " +
                       num(static_cast<double>(route.waypoints.size())) + " waypoints, " + num(route.heightChanges) +
                       " height changes, " + num(route.expandedCells) + " cells; levels";
    for (float const level : route.levels)
        text += " " + num(level);
    text += "; z:";
    std::size_t i = 0;
    while (i < route.waypoints.size())
    {
        std::size_t run = 1;
        long long const z = std::llround(route.waypoints[i].z);
        while (i + run < route.waypoints.size() && std::llround(route.waypoints[i + run].z) == z)
            ++run;
        text += " " + std::to_string(z) + (run > 1 ? "x" + std::to_string(run) : "");
        i += run;
    }
    return text;
}
}  // namespace FlightRules

#endif
