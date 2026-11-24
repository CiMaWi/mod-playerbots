/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The map of fields (questing plan WQ13): cells, a cell border, negative coordinates, an entry spread over many cells,
// two entries in one cell, an elite, a spawn with no respawn time, pool weights, the hostile summary, the lookups,
// every spawn counted once and determinism under shuffled input, built from made-up spawns. Standalone:
//   clang-cl /std:c++20 /EHsc /W4 /I src/Ai/World/Rpg tests/field-map-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "FieldMap.h"

using namespace Planner;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool Near(float a, double b) { return std::fabs(static_cast<double>(a) - b) < 1e-4; }

constexpr std::uint32_t MAP = 0;
constexpr std::uint32_t OTHER_MAP = 1;
constexpr std::uint32_t GNOLL = 100;
constexpr std::uint32_t BRUTE = 101;
constexpr std::uint32_t BOAR = 102;
constexpr std::uint32_t VENDOR = 103;
constexpr std::uint32_t CHEST = 200;
constexpr std::uint32_t HERB = 201;

FieldSpawn Creature(std::uint32_t entry, float x, float y, std::uint8_t minLevel, std::uint8_t maxLevel,
                    std::uint32_t respawn, bool hostile = true, bool elite = false, std::uint32_t map = MAP)
{
    FieldSpawn s;
    s.kind = FieldKind::Creature;
    s.entry = entry;
    s.mapId = map;
    s.x = x;
    s.y = y;
    s.z = 10.0f;
    s.minLevel = minLevel;
    s.maxLevel = maxLevel;
    s.elite = elite;
    s.hostile = hostile;
    s.respawnSecs = respawn;
    return s;
}

FieldSpawn Object(std::uint32_t entry, float x, float y, std::uint32_t respawn, float share = 1.0f)
{
    FieldSpawn s;
    s.kind = FieldKind::Object;
    s.entry = entry;
    s.mapId = MAP;
    s.x = x;
    s.y = y;
    s.z = 5.0f;
    s.respawnSecs = respawn;
    s.share = share;
    return s;
}

std::vector<FieldSpawn> World()
{
    std::vector<FieldSpawn> in;
    // Cell (0, 0): gnolls 16-17 and 17-18, one elite brute, a friendly vendor; a chest without respawn time
    in.push_back(Creature(GNOLL, 10.0f, 10.0f, 16, 17, 300));
    in.push_back(Creature(GNOLL, 45.0f, 55.0f, 17, 18, 360));
    in.push_back(Creature(GNOLL, 90.0f, 90.0f, 16, 17, 420));
    in.push_back(Creature(GNOLL, 60.0f, 20.0f, 16, 17, 0));  // no respawn time: out of the median
    in.push_back(Creature(BRUTE, 70.0f, 70.0f, 19, 19, 600, true, true));
    in.push_back(Creature(VENDOR, 30.0f, 30.0f, 30, 30, 120, false));
    in.push_back(Object(CHEST, 20.0f, 80.0f, 0));
    // The border: 99.99 stays in cell 0, 100.0 is cell 1
    in.push_back(Creature(BOAR, 99.99f, 50.0f, 10, 11, 180));
    in.push_back(Creature(BOAR, 100.0f, 50.0f, 10, 11, 180));
    // Negative coordinates: -0.01 is cell -1, -100.0 is cell -1, -100.01 is cell -2
    in.push_back(Creature(BOAR, -0.01f, -0.01f, 12, 12, 240));
    in.push_back(Creature(BOAR, -100.0f, -50.0f, 12, 12, 240));
    in.push_back(Creature(BOAR, -100.01f, -50.0f, 12, 12, 240));
    // An herb over many cells, pooled: one of two stands at a time
    for (int i = 0; i < 6; ++i)
        in.push_back(Object(HERB, 250.0f + 300.0f * static_cast<float>(i), -350.0f, 120, 0.5f));
    // The same coordinates on another map
    in.push_back(Creature(GNOLL, 10.0f, 10.0f, 16, 17, 300, true, false, OTHER_MAP));
    return in;
}

FieldRow const* RowOf(FieldMap const& map, std::uint64_t cell, FieldKind kind, std::uint32_t entry)
{
    for (FieldRow const& row : map.RowsOf(cell))
        if (row.kind == kind && row.entry == entry)
            return &row;
    return nullptr;
}

void Cells()
{
    Check(FieldCellOf(0.0f) == 0, "0 is cell 0");
    Check(FieldCellOf(99.99f) == 0, "99.99 is cell 0");
    Check(FieldCellOf(100.0f) == 1, "100 is cell 1");
    Check(FieldCellOf(-0.01f) == -1, "-0.01 is cell -1");
    Check(FieldCellOf(-100.0f) == -1, "-100 is cell -1");
    Check(FieldCellOf(-100.01f) == -2, "-100.01 is cell -2");
    for (std::int32_t const cx : {-171, -2, -1, 0, 1, 170})
        for (std::int32_t const cy : {-170, -1, 0, 3, 171})
        {
            std::uint64_t const key = FieldCellKey(571, cx, cy);
            Check(FieldMapOf(key) == 571 && FieldCellXOf(key) == cx && FieldCellYOf(key) == cy,
                  "the key of map 571, cell " + std::to_string(cx) + "," + std::to_string(cy) + " decodes");
        }
    Check(FieldCellKey(MAP, 0, 0) != FieldCellKey(OTHER_MAP, 0, 0), "the map is part of the key");
}

void Rows(FieldMap const& map)
{
    std::uint64_t const home = FieldCellKey(MAP, 0, 0);
    auto const rows = map.RowsOf(home);
    Check(rows.size() == 5, "cell 0,0 has five rows (gnoll, brute, vendor, boar at 99.99, chest)");
    bool ordered = true;
    for (std::size_t i = 1; i < rows.size(); ++i)
        ordered = ordered &&
                  std::make_pair(rows[i - 1].kind, rows[i - 1].entry) < std::make_pair(rows[i].kind, rows[i].entry);
    Check(ordered, "a cell's rows go by kind, then entry");

    FieldRow const* gnoll = RowOf(map, home, FieldKind::Creature, GNOLL);
    Check(gnoll && gnoll->spawns == 4 && Near(gnoll->weight, 4.0), "four gnolls, weight 4");
    Check(gnoll && gnoll->minLevel == 16 && gnoll->maxLevel == 18, "the gnolls' levels 16 to 18");
    Check(gnoll && gnoll->elites == 0, "no elite gnoll");
    Check(gnoll && gnoll->medianRespawn == 360, "the median of 300, 360, 420; the spawn without respawn left out");
    Check(gnoll && Near(gnoll->x, 45.0) && Near(gnoll->y, 55.0) && Near(gnoll->z, 10.0),
          "the gnolls' point is the member nearest the centre");

    FieldRow const* brute = RowOf(map, home, FieldKind::Creature, BRUTE);
    Check(brute && brute->spawns == 1 && brute->elites == 1 && brute->minLevel == 19, "the elite brute");

    FieldRow const* chest = RowOf(map, home, FieldKind::Object, CHEST);
    Check(chest && chest->spawns == 1 && chest->medianRespawn == 0, "a chest without respawn time has median 0");
    Check(chest && chest->minLevel == 0 && chest->maxLevel == 0 && chest->elites == 0, "an object has no level");

    FieldRow const* inside = RowOf(map, home, FieldKind::Creature, BOAR);
    FieldRow const* across = RowOf(map, FieldCellKey(MAP, 1, 0), FieldKind::Creature, BOAR);
    Check(inside && inside->spawns == 1 && Near(inside->x, 99.99), "the boar at 99.99 stays in cell 0");
    Check(across && across->spawns == 1 && Near(across->x, 100.0), "the boar at 100 is in cell 1");

    FieldRow const* minusOne = RowOf(map, FieldCellKey(MAP, -1, -1), FieldKind::Creature, BOAR);
    FieldRow const* minusTwo = RowOf(map, FieldCellKey(MAP, -2, -1), FieldKind::Creature, BOAR);
    Check(minusOne && minusOne->spawns == 2, "-0.01,-0.01 and -100,-50 share cell -1,-1");
    Check(minusTwo && minusTwo->spawns == 1, "-100.01 is in cell -2");
    Check(minusOne && Near(minusOne->x, -100.0), "-100,-50 is 50 yd from -50,-50, nearer than -0.01,-0.01 (70.7)");

    Check(map.RowsOf(FieldCellKey(MAP, 50, 50)).empty() && !map.Cell(FieldCellKey(MAP, 50, 50)),
          "an empty cell has no rows and no summary");
    FieldRow const* other = RowOf(map, FieldCellKey(OTHER_MAP, 0, 0), FieldKind::Creature, GNOLL);
    Check(other && other->spawns == 1, "the other map's gnoll keeps its own cell");
}

void Summary(FieldMap const& map)
{
    FieldCell const* home = map.Cell(FieldCellKey(MAP, 0, 0));
    Check(home && home->hostiles == 6, "cell 0,0: four gnolls, the brute and a boar are hostile; the vendor is not");
    Check(home && home->hostileMinLevel == 10 && home->hostileMaxLevel == 19, "cell 0,0's hostiles are 10 to 19");
    Check(home && home->hostileElites == 1, "cell 0,0 has one elite");
    Check(home && Near(home->hostileWeight, 6.0), "cell 0,0's hostile weight 6");
    Check(home && home->rowCount == 5, "cell 0,0's summary counts its rows");
    FieldCell const* herbs = map.Cell(FieldCellKeyAt(MAP, 250.0f, -350.0f));
    Check(herbs && herbs->hostiles == 0 && herbs->hostileMinLevel == 0 && herbs->hostileMaxLevel == 0,
          "a cell of objects only has no hostiles");
}

void Lookups(FieldMap const& map)
{
    auto const herbCells = map.CellsOf(FieldKind::Object, HERB);
    Check(herbCells.size() == 6, "the herb lies in six cells");
    Check(std::is_sorted(herbCells.begin(), herbCells.end()), "an entry's cells are sorted");
    bool weighted = true;
    for (std::uint64_t const cell : herbCells)
    {
        FieldRow const* herb = RowOf(map, cell, FieldKind::Object, HERB);
        weighted = weighted && herb && herb->spawns == 1 && Near(herb->weight, 0.5) && herb->medianRespawn == 120;
    }
    Check(weighted, "each herb row: one spawn, weight 0.5 (its pool share)");
    Check(map.CellsOf(FieldKind::Creature, HERB).empty(), "the kind is part of an entry's key");
    Check(map.CellsOf(FieldKind::Creature, BOAR).size() == 4, "the boar lies in cells 0,0, 1,0, -1,-1 and -2,-1");
    Check(map.CellsOf(FieldKind::Creature, GNOLL).size() == 2, "the gnoll lies in a cell on each map");
    Check(map.CellsOf(FieldKind::Creature, 999).empty(), "an unknown entry has no cells");

    std::vector<std::uint64_t> const around = map.Around(MAP, 50.0f, 50.0f);
    std::vector<std::uint64_t> expected = {FieldCellKey(MAP, 0, 0), FieldCellKey(MAP, 1, 0), FieldCellKey(MAP, -1, -1)};
    std::vector<std::uint64_t> sortedAround = around;
    std::sort(sortedAround.begin(), sortedAround.end());
    std::sort(expected.begin(), expected.end());
    Check(sortedAround == expected, "the nine cells around 50,50 that hold spawns: 0,0, 1,0 and -1,-1");
    Check(map.Around(MAP, -150.0f, -50.0f).size() == 2, "around -150,-50: cells -2,-1 and -1,-1");
    Check(map.Around(OTHER_MAP, 150.0f, 50.0f).size() == 1, "around another map's point only its own cells");
    Check(map.Around(MAP, 5000.0f, 5000.0f).empty(), "nothing around an empty place");
}

bool SameRow(FieldRow const& a, FieldRow const& b)
{
    return a.cell == b.cell && a.kind == b.kind && a.entry == b.entry && a.spawns == b.spawns && a.weight == b.weight &&
           a.minLevel == b.minLevel && a.maxLevel == b.maxLevel && a.elites == b.elites &&
           a.medianRespawn == b.medianRespawn && a.x == b.x && a.y == b.y && a.z == b.z;
}

bool SameCell(FieldCell const& a, FieldCell const& b)
{
    return a.key == b.key && a.firstRow == b.firstRow && a.rowCount == b.rowCount && a.hostiles == b.hostiles &&
           a.hostileWeight == b.hostileWeight && a.hostileElites == b.hostileElites &&
           a.hostileMinLevel == b.hostileMinLevel && a.hostileMaxLevel == b.hostileMaxLevel;
}

void Counted(FieldMap const& map, std::size_t inputs)
{
    std::size_t spawns = 0;
    for (FieldRow const& row : map.Rows())
        spawns += row.spawns;
    Check(spawns == inputs && map.Stats().spawns == inputs, "every spawn is counted once");
    std::size_t rows = 0;
    for (FieldCell const& cell : map.Cells())
        rows += cell.rowCount;
    Check(rows == map.Rows().size() && map.Stats().rows == rows, "every row belongs to one cell");
    Check(map.Stats().cells == map.Cells().size(), "the cell count");
    Check(map.Stats().creatureRows + map.Stats().objectRows == map.Stats().rows, "creature and object rows add up");
    Check(map.Stats().hostiles == 11, "eleven hostile creatures: 4 + 1 gnolls, the brute, five boars");
    Check(map.ApproxBytes() >= map.Rows().size() * sizeof(FieldRow), "ApproxBytes covers the rows");
}

void Determinism(FieldMap const& reference)
{
    std::mt19937 rng(13);
    bool same = true;
    for (int round = 0; round < 20; ++round)
    {
        std::vector<FieldSpawn> in = World();
        std::shuffle(in.begin(), in.end(), rng);
        FieldMap const map = FieldMap::Build(std::move(in));
        same = same && map.Rows().size() == reference.Rows().size() && map.Cells().size() == reference.Cells().size();
        for (std::size_t i = 0; same && i < map.Rows().size(); ++i)
            same = SameRow(map.Rows()[i], reference.Rows()[i]);
        for (std::size_t i = 0; same && i < map.Cells().size(); ++i)
            same = SameCell(map.Cells()[i], reference.Cells()[i]);
    }
    Check(same, "the same spawns in any order give the same map");
}

void Shapes()
{
    FieldMap const empty = FieldMap::Build({});
    Check(empty.Rows().empty() && empty.Cells().empty() && empty.RowsOf(FieldCellKey(MAP, 0, 0)).empty(),
          "no spawns, no rows");
    // An even count of respawns: the lower median
    std::vector<FieldSpawn> in;
    for (std::uint32_t const respawn : {600u, 60u, 300u, 120u})
        in.push_back(Creature(GNOLL, 10.0f, 10.0f, 16, 16, respawn));
    FieldMap const even = FieldMap::Build(std::move(in));
    Check(even.Rows().size() == 1 && even.Rows()[0].medianRespawn == 120, "60, 120, 300, 600: the lower median 120");
    // Every respawn missing
    FieldMap const none = FieldMap::Build({Creature(GNOLL, 1.0f, 1.0f, 5, 5, 0), Creature(GNOLL, 2.0f, 2.0f, 5, 5, 0)});
    Check(none.Rows().size() == 1 && none.Rows()[0].medianRespawn == 0 && none.Rows()[0].spawns == 2,
          "no spawn with a respawn time: median 0");
    // A pool member that never stands still counts as a spawn, with weight 0
    FieldMap const zero = FieldMap::Build({Object(CHEST, 1.0f, 1.0f, 60, 0.0f), Object(CHEST, 2.0f, 2.0f, 60, 0.25f)});
    Check(zero.Rows().size() == 1 && zero.Rows()[0].spawns == 2 && Near(zero.Rows()[0].weight, 0.25),
          "a share of 0 adds a spawn, not weight");
}
}  // namespace

int main()
{
    std::vector<FieldSpawn> const in = World();
    FieldMap const map = FieldMap::Build(in);
    Cells();
    Rows(map);
    Summary(map);
    Lookups(map);
    Counted(map, in.size());
    Determinism(map);
    Shapes();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
