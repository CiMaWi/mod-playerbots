/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_DANGER_RULES_H
#define _PLAYERBOT_DANGER_RULES_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "AwarenessRules.h"

// Known danger (awareness, INTERFACE.md §4.1 to §4.3): what lives at a place by the world data, prepared offline by the
// hub's tools/awareness/danger-build.js and the same for every bot. A judgement writes nothing and decides nothing;
// the caller chooses. Game-free, so the selftest and the route rules can read it
namespace DangerRules
{
constexpr std::size_t RANK_COUNT = static_cast<std::size_t>(AwarenessRules::Rank::Count);
// Nothing attacks a bot this many levels below it: a detection range of 20 yd comes down to the engine's least, 5 yd
constexpr int QUIET_LEVELS_BELOW = 15;
// The table's geometry; the load refuses a build of other values
constexpr int CELL_YARDS = 25;
constexpr int HEIGHT_REACH_YARDS = 26;
// The maps of the table; every other map is quiet
constexpr std::array<std::uint32_t, 4> MAPS{0, 1, 530, 571};
// The core's TeamId: 0 Alliance, 1 Horde
constexpr std::uint8_t SIDE_COUNT = 2;

// Ordered: a larger value is more dangerous
enum class Ground : std::uint8_t
{
    Quiet,
    Passable,
    Deadly
};

struct Condition
{
    std::uint8_t healthPct = 100;
    bool sick = false;
};
inline constexpr Condition HEALTHY{100, false};
inline constexpr Condition SICK{50, true};  // as the spirit healer leaves a bot

struct CellFacts
{
    std::array<std::uint8_t, RANK_COUNT> level{};  // by AwarenessRules::Rank, the highest; 0 for none
    std::array<std::uint8_t, RANK_COUNT> count{};  // capped at 255
    bool guards = false;                           // a guard hostile to the side counts
};

// The highest level of a rank that counts, -1 when nothing counts
inline int HighestLevel(CellFacts const& facts)
{
    int top = -1;
    for (std::size_t r = 0; r < RANK_COUNT; ++r)
        if (facts.count[r])
            top = std::max(top, static_cast<int>(facts.level[r]));
    return top;
}

// The first line that matches (§4.2): guards, nothing within 15 levels, sick, the strongest outweighs the bot
inline Ground JudgeGround(CellFacts const& facts, std::uint8_t level, Condition condition)
{
    if (facts.guards)
        return Ground::Deadly;
    int const top = HighestLevel(facts);
    if (top < 0 || top + QUIET_LEVELS_BELOW <= level)
        return Ground::Quiet;
    if (condition.sick)
        return Ground::Deadly;
    std::uint32_t const bot = AwarenessRules::Strength(AwarenessRules::STRENGTH.player, 0, condition.healthPct);
    for (std::size_t r = 0; r < RANK_COUNT; ++r)
        if (facts.count[r] &&
            AwarenessRules::Strength(AwarenessRules::STRENGTH.rank[r], facts.level[r] - level, 100.0f) > bot)
            return Ground::Deadly;
    return Ground::Passable;
}

// The largest level difference at which a creature of the rank does not outweigh a healthy bot, from the strength
// rule itself; found once at the first use (std::pow is no constant expression)
inline int TakenUpTo(std::size_t rank)
{
    static std::array<int, RANK_COUNT> const takenUpTo = []
    {
        std::array<int, RANK_COUNT> out{};
        std::uint32_t const bot = AwarenessRules::Strength(AwarenessRules::STRENGTH.player, 0, 100.0f);
        int const span = AwarenessRules::STRENGTH.levelSpan;
        for (std::size_t r = 0; r < RANK_COUNT; ++r)
        {
            out[r] = -span - 1;
            for (int diff = -span; diff <= span; ++diff)
                if (AwarenessRules::Strength(AwarenessRules::STRENGTH.rank[r], diff, 100.0f) <= bot)
                    out[r] = diff;
        }
        return out;
    }();
    return takenUpTo[std::min(rank, RANK_COUNT - 1)];
}

// HEALTHY: deadly for every bot level below it; 255 with guards, 0 where nothing counts
inline std::uint8_t DeadlyBelow(CellFacts const& facts)
{
    if (facts.guards)
        return 255;
    int below = 0;
    for (std::size_t r = 0; r < RANK_COUNT; ++r)
        if (facts.count[r])
            below = std::max(below, facts.level[r] - TakenUpTo(r));
    return static_cast<std::uint8_t>(std::clamp(below, 0, 255));
}

// HEALTHY: not quiet for every bot level below it; 255 with guards, 0 where nothing counts
inline std::uint8_t PassableBelow(CellFacts const& facts)
{
    if (facts.guards)
        return 255;
    int const top = HighestLevel(facts);
    if (top < 0)
        return 0;
    return static_cast<std::uint8_t>(
        std::clamp(std::max(static_cast<int>(DeadlyBelow(facts)), top + QUIET_LEVELS_BELOW), 0, 255));
}

// Two sets of creatures together: per rank the higher level and the counts added (capped), guards of either
inline CellFacts Merge(CellFacts const& a, CellFacts const& b)
{
    CellFacts merged;
    for (std::size_t r = 0; r < RANK_COUNT; ++r)
    {
        merged.level[r] = std::max(a.level[r], b.level[r]);
        merged.count[r] = static_cast<std::uint8_t>(std::min(255, a.count[r] + b.count[r]));
    }
    merged.guards = a.guards || b.guards;
    return merged;
}

// One layer of a cell and side: the homes of what counts lie between zLo and zHi (yards, rounded down and up)
struct Layer
{
    std::int16_t zLo = 0;
    std::int16_t zHi = 0;
    CellFacts facts;
};

// A place at this height reads the layer
inline bool Reaches(Layer const& layer, float z)
{
    return static_cast<float>(layer.zLo - HEIGHT_REACH_YARDS) <= z &&
           z <= static_cast<float>(layer.zHi + HEIGHT_REACH_YARDS);
}

// What a height reads of a cell and side's layers (in order of height): the layers within reach, their facts merged
struct Reading
{
    CellFacts facts;
    std::uint8_t firstRead = 0;  // the layers read are firstRead and the readCount - 1 above it
    std::uint8_t readCount = 0;
};

inline Reading ReadLayers(Layer const* layers, std::size_t count, float z)
{
    Reading reading;
    for (std::size_t i = 0; i < count; ++i)
        if (Reaches(layers[i], z))
        {
            if (!reading.readCount)
                reading.firstRead = static_cast<std::uint8_t>(i);
            ++reading.readCount;
            reading.facts = Merge(reading.facts, layers[i].facts);
        }
    return reading;
}

// The table's index of a map, -1 for a map it does not cover
inline int MapIndex(std::uint32_t mapId)
{
    for (std::size_t i = 0; i < MAPS.size(); ++i)
        if (MAPS[i] == mapId)
            return static_cast<int>(i);
    return -1;
}

// The cell of a point, floor(x / 25) and floor(y / 25); false for a point that is no number or outside the table
inline bool CellOf(float x, float y, int& cx, int& cy)
{
    double const fx = std::floor(static_cast<double>(x) / CELL_YARDS);
    double const fy = std::floor(static_cast<double>(y) / CELL_YARDS);
    if (!(fx >= -32768.0 && fx <= 32767.0 && fy >= -32768.0 && fy <= 32767.0))
        return false;
    cx = static_cast<int>(fx);
    cy = static_cast<int>(fy);
    return true;
}

inline std::uint64_t CellKey(int mapIndex, int cx, int cy, std::uint8_t side)
{
    return (static_cast<std::uint64_t>(mapIndex) << 40) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(cx)) << 24) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(cy)) << 8) | side;
}

// The store: immutable once published, read lock-free from any thread
struct Known
{
    std::string build;
    std::uint8_t maxLayers = 0;
    std::vector<std::uint64_t> keys;    // CellKey of every cell and side with rows, ascending
    std::vector<std::uint32_t> firsts;  // keys.size() + 1: the layers of keys[i] are layers[firsts[i]..firsts[i + 1])
    std::vector<Layer> layers;          // per cell and side in order of height
    // AW37a adds the links' danger by link index with the graph's link count; its fold publishes a new Known

    std::size_t Bytes() const
    {
        return keys.capacity() * sizeof(std::uint64_t) + firsts.capacity() * sizeof(std::uint32_t) +
               layers.capacity() * sizeof(Layer);
    }
};

// What a place reads: its cell, the cell and side's layers (none without rows) and the reading at its height
struct PlaceReading
{
    bool cell = false;  // on a map of the table, with a cell
    int cx = 0;
    int cy = 0;
    Layer const* layers = nullptr;
    std::uint8_t layerCount = 0;
    Reading read;
};

// side: 0 Alliance, 1 Horde; any other value, a map the table does not cover and no store read nothing
inline PlaceReading ReadPlace(Known const* known, std::uint32_t mapId, float x, float y, float z, std::uint8_t side)
{
    PlaceReading place;
    int const mapIndex = MapIndex(mapId);
    if (mapIndex < 0 || !CellOf(x, y, place.cx, place.cy))
        return place;
    place.cell = true;
    if (!known || side >= SIDE_COUNT)
        return place;
    std::uint64_t const key = CellKey(mapIndex, place.cx, place.cy, side);
    auto const it = std::lower_bound(known->keys.begin(), known->keys.end(), key);
    if (it == known->keys.end() || *it != key)
        return place;
    std::size_t const index = static_cast<std::size_t>(it - known->keys.begin());
    place.layers = known->layers.data() + known->firsts[index];
    place.layerCount = static_cast<std::uint8_t>(known->firsts[index + 1] - known->firsts[index]);
    place.read = ReadLayers(place.layers, place.layerCount, z);
    return place;
}

// The live check of the facts against the picture (scenario aw-danger): the facts the bot's picture gives its cell,
// by the build's test as near as the picture holds it. A creature counts that is hostile to the bot (no player, no
// pet), would attack it (a reach) or carries the guard flag, and whose home, else its place now, else (a patrol) its
// waypoint path lies within PICTURE_REACH_YARDS of the cell's square in the plane and within HEIGHT_REACH_YARDS of
// the height. The path is the engine's, which the caller reads by guid (the build reads the same rows)
constexpr float PICTURE_REACH_YARDS = 26.0f;  // the build's: a detection range of 20 yd and its LAY_REACH_YARDS 6
constexpr float PATH_STEP_YARDS = 2.0f;       // a path's segment is measured at points this far apart

struct PictureFacts
{
    CellFacts facts;
    std::uint8_t byHome = 0;
    std::uint8_t byPlace = 0;  // counted by its place now only: away from its home
    std::uint8_t byPath = 0;   // counted by its path only: a patrol elsewhere on its way
};

// The planar distance from a point to the cell's square, 0 inside it
inline float SquareDistance(int cx, int cy, float x, float y)
{
    float const lowX = static_cast<float>(cx * CELL_YARDS);
    float const lowY = static_cast<float>(cy * CELL_YARDS);
    float const dx = std::max({0.0f, lowX - x, x - (lowX + CELL_YARDS)});
    float const dy = std::max({0.0f, lowY - y, y - (lowY + CELL_YARDS)});
    return std::hypot(dx, dy);
}

// A waypoint path (x, y, z per point, in order) reaches the cell at this height: a segment within the reach of the
// square in the plane, with one of its two points within the reach in height
inline bool PathReaches(std::vector<float> const& points, int cx, int cy, float z)
{
    std::size_t const count = points.size() / 3;
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        float const* a = &points[3 * i];
        float const* b = &points[3 * i + 3];
        if (std::fabs(a[2] - z) > HEIGHT_REACH_YARDS && std::fabs(b[2] - z) > HEIGHT_REACH_YARDS)
            continue;
        int const steps = std::max(1, static_cast<int>(std::hypot(b[0] - a[0], b[1] - a[1]) / PATH_STEP_YARDS));
        for (int s = 0; s <= steps; ++s)
        {
            float const t = static_cast<float>(s) / static_cast<float>(steps);
            if (SquareDistance(cx, cy, a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])) <= PICTURE_REACH_YARDS)
                return true;
        }
    }
    return count == 1 && SquareDistance(cx, cy, points[0], points[1]) <= PICTURE_REACH_YARDS &&
           std::fabs(points[2] - z) <= HEIGHT_REACH_YARDS;
}

// How a picture's unit lies for the cell at this height: 0 not counted, 1 by its home, 2 by its place now only, 3 by
// its path only (`pathReaches`: the caller's PathReaches of the unit's waypoint path)
inline int PictureCounts(AwarenessRules::Seen const& unit, int cx, int cy, float z, bool pathReaches = false)
{
    using AwarenessRules::Side;
    using AwarenessRules::UnitType;
    if (unit.type != UnitType::Creature || unit.side != Side::Enemy || !(unit.reachYards > 0.0f || unit.guard))
        return 0;
    if (SquareDistance(cx, cy, unit.homeX, unit.homeY) <= PICTURE_REACH_YARDS &&
        std::fabs(unit.homeZ - z) <= HEIGHT_REACH_YARDS)
        return 1;
    if (SquareDistance(cx, cy, unit.x, unit.y) <= PICTURE_REACH_YARDS && std::fabs(unit.z - z) <= HEIGHT_REACH_YARDS)
        return 2;
    return pathReaches ? 3 : 0;
}

// `pathReaches[i]`: the waypoint path of the picture's unit i reaches the cell (none without paths)
inline PictureFacts FactsOfPicture(AwarenessRules::Picture const& picture, int cx, int cy, float z,
                                   std::array<bool, AwarenessRules::PICTURE_SIZE> const& pathReaches = {})
{
    PictureFacts out;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        AwarenessRules::Seen const& unit = picture.units[i];
        int const how = PictureCounts(unit, cx, cy, z, pathReaches[i]);
        if (!how)
            continue;
        std::size_t const rank = static_cast<std::size_t>(unit.rank);
        int const level = std::clamp(int(picture.self.level) + int(unit.levelDiff), 1, 255);
        out.facts.level[rank] = std::max(out.facts.level[rank], static_cast<std::uint8_t>(level));
        out.facts.count[rank] = static_cast<std::uint8_t>(std::min(int(out.facts.count[rank]) + 1, 255));
        out.facts.guards = out.facts.guards || unit.guard;
        std::uint8_t& counted = how == 1 ? out.byHome : how == 2 ? out.byPlace : out.byPath;
        counted = static_cast<std::uint8_t>(std::min(int(counted) + 1, 255));
    }
    return out;
}

// Two answers for one place: the same, one step apart, or quiet against deadly
enum class Agreement : std::uint8_t
{
    Same,
    Near,
    Apart
};

inline Agreement Agree(Ground a, Ground b)
{
    int const steps = std::abs(int(a) - int(b));
    return steps == 0 ? Agreement::Same : steps == 1 ? Agreement::Near : Agreement::Apart;
}

// A row of playerbots_known_danger and the row of playerbots_known_danger_build, as the load reads them
struct Row
{
    std::uint16_t map = 0;
    std::int16_t cx = 0;
    std::int16_t cy = 0;
    std::uint8_t side = 0;
    std::uint8_t layer = 0;
    Layer value;
};

struct BuildRow
{
    std::string build;
    std::uint32_t cellYards = 0;
    std::uint32_t heightReachYards = 0;
    std::uint32_t rowCount = 0;
    std::uint32_t maxLayers = 0;
};

inline std::string RowText(Row const& row)
{
    return "map " + std::to_string(row.map) + " cell " + std::to_string(row.cx) + " " + std::to_string(row.cy) +
           " side " + std::to_string(row.side) + " layer " + std::to_string(row.layer);
}

// The load's check (§4.1, H2): empty when the table is taken and `known` holds it; otherwise why it is refused (and
// `known` is to be dropped). An empty table would call the whole world quiet, so it is refused too
inline std::string Assemble(std::vector<BuildRow> const& builds, std::vector<Row> rows, Known& known)
{
    if (builds.size() != 1)
        return builds.empty() ? "no build row" : std::to_string(builds.size()) + " build rows";
    BuildRow const& build = builds.front();
    if (build.cellYards != CELL_YARDS)
        return "the build's cell_yards is " + std::to_string(build.cellYards) + ", the server's " +
               std::to_string(CELL_YARDS);
    if (build.heightReachYards != HEIGHT_REACH_YARDS)
        return "the build's height_reach_yards is " + std::to_string(build.heightReachYards) + ", the server's " +
               std::to_string(HEIGHT_REACH_YARDS);
    if (rows.empty())
        return "no rows";
    if (rows.size() != build.rowCount)
        return std::to_string(rows.size()) + " rows, the build row names " + std::to_string(build.rowCount);
    for (Row const& row : rows)
        if (MapIndex(row.map) < 0 || row.side >= SIDE_COUNT)
            return "a row of another map or side: " + RowText(row);
    auto const keyOf = [](Row const& row) { return CellKey(MapIndex(row.map), row.cx, row.cy, row.side); };
    std::sort(rows.begin(), rows.end(), [&keyOf](Row const& a, Row const& b)
              { return std::make_tuple(keyOf(a), a.layer) < std::make_tuple(keyOf(b), b.layer); });
    known = Known{};
    known.build = build.build;
    known.maxLayers = static_cast<std::uint8_t>(std::min<std::uint32_t>(build.maxLayers, 255));
    known.layers.reserve(rows.size());
    for (Row const& row : rows)
    {
        std::uint64_t const key = keyOf(row);
        bool const fresh = known.keys.empty() || known.keys.back() != key;
        if (fresh)
        {
            known.keys.push_back(key);
            known.firsts.push_back(static_cast<std::uint32_t>(known.layers.size()));
        }
        std::size_t const index = known.layers.size() - known.firsts.back();
        if (row.layer != index)
            return "layers out of order (" + std::to_string(index) + " due): " + RowText(row);
        if (row.value.zLo > row.value.zHi)
            return "z_lo above z_hi: " + RowText(row);
        if (index && row.value.zLo - known.layers.back().zHi <= HEIGHT_REACH_YARDS)
            return "a layer within " + std::to_string(HEIGHT_REACH_YARDS) + " yd of the one below: " + RowText(row);
        if (index + 1 > build.maxLayers)
            return "more layers than the build's max_layers " + std::to_string(build.maxLayers) + ": " + RowText(row);
        known.layers.push_back(row.value);
    }
    known.firsts.push_back(static_cast<std::uint32_t>(known.layers.size()));
    known.keys.shrink_to_fit();
    known.firsts.shrink_to_fit();
    return {};
}

// Every published store is kept until the server stops, so a reader's pointer stays good after a newer publish
namespace Store
{
inline std::atomic<Known const*> published{nullptr};
inline std::mutex keptLock;
inline std::vector<std::unique_ptr<Known const>> kept;
}  // namespace Store

// The store, null before the load: one atomic load
inline Known const* Published() { return Store::published.load(std::memory_order_acquire); }

inline Known const* Publish(std::unique_ptr<Known const> known)
{
    std::lock_guard<std::mutex> const lock(Store::keptLock);
    Known const* const store = known.get();
    Store::kept.push_back(std::move(known));
    Store::published.store(store, std::memory_order_release);
    return store;
}

// Reads the two tables once at the start (DangerLoad.cpp) and publishes the store; false logs why and refuses the start
bool Load();
}  // namespace DangerRules

#endif
