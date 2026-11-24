/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIELDMAP_H
#define _PLAYERBOT_FIELDMAP_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

// The map of fields (questing plan WQ13, PLAN 3.10 part 1): per map, cells of FIELD_CELL_YARDS with a row per creature
// or object entry (spawns, pool weight, levels, elites, median respawn, the member nearest the cell's centre) and a
// summary of the cell's hostile creatures. Built once on a worker from spawns the world thread gathered; immutable.
// No game header: tests/field-map-selftest.cpp builds it from made-up spawns
namespace Planner
{
constexpr float FIELD_CELL_YARDS = 100.0f;  // WQ.37 (1): a field is a cell with its eight neighbours
constexpr std::int32_t FIELD_CELL_BITS = 24;
constexpr std::uint64_t FIELD_CELL_MASK = (std::uint64_t{1} << FIELD_CELL_BITS) - 1;
constexpr std::uint64_t FIELD_MAP_MASK = 0xFFFF;

enum class FieldKind : std::uint8_t
{
    Creature = 0,
    Object = 1
};

// The cell of one coordinate; floor, so 100.0 is cell 1 and -0.01 is cell -1
inline std::int32_t FieldCellOf(float coordinate)
{
    return static_cast<std::int32_t>(std::floor(coordinate / FIELD_CELL_YARDS));
}

// Map and the two signed cell indices in 64 bits (16 + 24 + 24)
inline std::uint64_t FieldCellKey(std::uint32_t mapId, std::int32_t cellX, std::int32_t cellY)
{
    return ((static_cast<std::uint64_t>(mapId) & FIELD_MAP_MASK) << (2 * FIELD_CELL_BITS)) |
           ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellX)) & FIELD_CELL_MASK) << FIELD_CELL_BITS) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellY)) & FIELD_CELL_MASK);
}

inline std::uint64_t FieldCellKeyAt(std::uint32_t mapId, float x, float y)
{
    return FieldCellKey(mapId, FieldCellOf(x), FieldCellOf(y));
}

inline std::uint32_t FieldMapOf(std::uint64_t key) { return static_cast<std::uint32_t>(key >> (2 * FIELD_CELL_BITS)); }

inline std::int32_t FieldSignExtend(std::uint64_t bits)
{
    constexpr std::uint64_t SIGN = std::uint64_t{1} << (FIELD_CELL_BITS - 1);
    return static_cast<std::int32_t>(static_cast<std::int64_t>((bits ^ SIGN)) - static_cast<std::int64_t>(SIGN));
}

inline std::int32_t FieldCellXOf(std::uint64_t key)
{
    return FieldSignExtend((key >> FIELD_CELL_BITS) & FIELD_CELL_MASK);
}

inline std::int32_t FieldCellYOf(std::uint64_t key) { return FieldSignExtend(key & FIELD_CELL_MASK); }

// One spawn as the world thread gathered it: the template's levels and rank, the spawn's respawn and its pool share
struct FieldSpawn
{
    FieldKind kind{FieldKind::Creature};
    std::uint32_t entry{0};
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint8_t minLevel{0};  // objects 0
    std::uint8_t maxLevel{0};
    bool elite{false};
    bool hostile{false};           // a creature hostile to players that can be attacked (the cell summary)
    std::uint32_t respawnSecs{0};  // 0 = no respawn time
    float share{1.0f};             // the share of the time the spawn stands (pools); 1 outside a pool
};

// One entry in one cell
struct FieldRow
{
    std::uint64_t cell{0};
    std::uint32_t entry{0};
    std::uint32_t spawns{0};
    float weight{0.0f};              // the sum of the spawns' shares
    std::uint32_t medianRespawn{0};  // of the spawns with a respawn time, the lower median of an even count; 0 if none
    float x{0.0f};                   // the member nearest the cell's centre (2D; ties by x, y, z): the point to walk to
    float y{0.0f};
    float z{0.0f};
    std::uint16_t elites{0};
    FieldKind kind{FieldKind::Creature};
    std::uint8_t minLevel{0};
    std::uint8_t maxLevel{0};
};

// One cell: its rows and the summary of its hostile creatures
struct FieldCell
{
    std::uint64_t key{0};
    std::uint32_t firstRow{0};
    std::uint32_t rowCount{0};
    std::uint32_t hostiles{0};
    float hostileWeight{0.0f};
    std::uint16_t hostileElites{0};
    std::uint8_t hostileMinLevel{0};  // 0 when the cell has no hostile creature
    std::uint8_t hostileMaxLevel{0};
};

struct FieldMapStats
{
    std::uint32_t spawns{0};
    std::uint32_t rows{0};
    std::uint32_t cells{0};
    std::uint32_t creatureRows{0};
    std::uint32_t objectRows{0};
    std::uint32_t hostiles{0};
    std::uint32_t gatherMs{0};  // set by the caller
    std::uint32_t buildMs{0};
};

class FieldMap
{
public:
    static FieldMap Build(std::vector<FieldSpawn> spawns)
    {
        FieldMap map;
        map._stats.spawns = static_cast<std::uint32_t>(spawns.size());
        // A total order over every field, so the same spawns in any order give the same map, sums included
        auto const order = [](FieldSpawn const& s)
        {
            return std::make_tuple(FieldCellKeyAt(s.mapId, s.x, s.y), s.kind, s.entry, s.x, s.y, s.z, s.respawnSecs,
                                   s.minLevel, s.maxLevel, s.elite, s.hostile, s.share);
        };
        std::sort(spawns.begin(), spawns.end(),
                  [&order](FieldSpawn const& a, FieldSpawn const& b) { return order(a) < order(b); });
        std::vector<std::uint32_t> respawns;
        for (std::size_t first = 0; first < spawns.size();)
        {
            std::uint64_t const cell = FieldCellKeyAt(spawns[first].mapId, spawns[first].x, spawns[first].y);
            if (map._cells.empty() || map._cells.back().key != cell)
            {
                map._cells.push_back({});
                map._cells.back().key = cell;
                map._cells.back().firstRow = static_cast<std::uint32_t>(map._rows.size());
            }
            FieldCell& summary = map._cells.back();
            float const centreX = (static_cast<float>(FieldCellXOf(cell)) + 0.5f) * FIELD_CELL_YARDS;
            float const centreY = (static_cast<float>(FieldCellYOf(cell)) + 0.5f) * FIELD_CELL_YARDS;
            FieldRow row;
            row.cell = cell;
            row.kind = spawns[first].kind;
            row.entry = spawns[first].entry;
            row.minLevel = spawns[first].minLevel;
            row.maxLevel = spawns[first].maxLevel;
            respawns.clear();
            float nearest = -1.0f;
            std::size_t last = first;
            for (; last < spawns.size(); ++last)
            {
                FieldSpawn const& s = spawns[last];
                if (s.kind != row.kind || s.entry != row.entry || FieldCellKeyAt(s.mapId, s.x, s.y) != cell)
                    break;
                ++row.spawns;
                row.weight += s.share;
                row.minLevel = std::min(row.minLevel, s.minLevel);
                row.maxLevel = std::max(row.maxLevel, s.maxLevel);
                if (s.elite)
                    ++row.elites;
                if (s.respawnSecs)
                    respawns.push_back(s.respawnSecs);
                float const d = (s.x - centreX) * (s.x - centreX) + (s.y - centreY) * (s.y - centreY);
                if (nearest < 0.0f || d < nearest)  // sorted by x, y, z: the first of equals wins
                {
                    nearest = d;
                    row.x = s.x;
                    row.y = s.y;
                    row.z = s.z;
                }
                if (s.kind == FieldKind::Creature && s.hostile)
                {
                    summary.hostileMinLevel =
                        summary.hostiles ? std::min(summary.hostileMinLevel, s.minLevel) : s.minLevel;
                    summary.hostileMaxLevel = std::max(summary.hostileMaxLevel, s.maxLevel);
                    ++summary.hostiles;
                    summary.hostileWeight += s.share;
                    if (s.elite)
                        ++summary.hostileElites;
                    ++map._stats.hostiles;
                }
            }
            if (!respawns.empty())
            {
                std::size_t const middle = (respawns.size() - 1) / 2;
                std::nth_element(respawns.begin(), respawns.begin() + static_cast<std::ptrdiff_t>(middle),
                                 respawns.end());
                row.medianRespawn = respawns[middle];
            }
            ++(row.kind == FieldKind::Creature ? map._stats.creatureRows : map._stats.objectRows);
            map._rows.push_back(row);
            ++summary.rowCount;
            first = last;
        }
        map._entryCells.reserve(map._rows.size());
        for (FieldRow const& row : map._rows)
            map._entryCells.push_back({EntryKey(row.kind, row.entry), row.cell});
        std::sort(map._entryCells.begin(), map._entryCells.end());
        map._entryCellKeys.reserve(map._entryCells.size());
        for (auto const& entryCell : map._entryCells)
            map._entryCellKeys.push_back(entryCell.second);
        map._stats.rows = static_cast<std::uint32_t>(map._rows.size());
        map._stats.cells = static_cast<std::uint32_t>(map._cells.size());
        return map;
    }

    // The cell's summary; null when no spawn lies in it
    FieldCell const* Cell(std::uint64_t key) const
    {
        auto const it = std::lower_bound(_cells.begin(), _cells.end(), key,
                                         [](FieldCell const& cell, std::uint64_t k) { return cell.key < k; });
        return it != _cells.end() && it->key == key ? &*it : nullptr;
    }

    // The rows of a cell, by kind then entry
    std::span<FieldRow const> RowsOf(std::uint64_t key) const
    {
        FieldCell const* cell = Cell(key);
        if (!cell)
            return {};
        return std::span<FieldRow const>(_rows).subspan(cell->firstRow, cell->rowCount);
    }

    // The cells an entry spawns in, sorted by key
    std::span<std::uint64_t const> CellsOf(FieldKind kind, std::uint32_t entry) const
    {
        std::uint64_t const key = EntryKey(kind, entry);
        auto const first =
            std::lower_bound(_entryCells.begin(), _entryCells.end(), std::make_pair(key, std::uint64_t{0}));
        auto const last = std::upper_bound(first, _entryCells.end(), std::make_pair(key, ~std::uint64_t{0}));
        return std::span<std::uint64_t const>(_entryCellKeys)
            .subspan(static_cast<std::size_t>(first - _entryCells.begin()), static_cast<std::size_t>(last - first));
    }

    // The nine cells around a point (its own and the eight neighbours) that hold a spawn
    std::vector<std::uint64_t> Around(std::uint32_t mapId, float x, float y) const
    {
        std::vector<std::uint64_t> keys;
        std::int32_t const cellX = FieldCellOf(x);
        std::int32_t const cellY = FieldCellOf(y);
        for (std::int32_t dx = -1; dx <= 1; ++dx)
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                if (std::uint64_t const key = FieldCellKey(mapId, cellX + dx, cellY + dy); Cell(key))
                    keys.push_back(key);
        return keys;
    }

    std::vector<FieldRow> const& Rows() const { return _rows; }
    std::vector<FieldCell> const& Cells() const { return _cells; }
    FieldMapStats const& Stats() const { return _stats; }
    FieldMapStats& MutableStats() { return _stats; }  // the caller's times, before the publish

    std::size_t ApproxBytes() const
    {
        return sizeof(*this) + _rows.capacity() * sizeof(FieldRow) + _cells.capacity() * sizeof(FieldCell) +
               _entryCells.capacity() * sizeof(_entryCells[0]) + _entryCellKeys.capacity() * sizeof(std::uint64_t);
    }

private:
    static std::uint64_t EntryKey(FieldKind kind, std::uint32_t entry)
    {
        return (static_cast<std::uint64_t>(kind) << 32) | entry;
    }

    std::vector<FieldRow> _rows;                                       // by (cell, kind, entry)
    std::vector<FieldCell> _cells;                                     // by key
    std::vector<std::pair<std::uint64_t, std::uint64_t>> _entryCells;  // (entry key, cell), sorted
    std::vector<std::uint64_t> _entryCellKeys;                         // the cells of _entryCells, aligned
    FieldMapStats _stats;
};
}  // namespace Planner

#endif
