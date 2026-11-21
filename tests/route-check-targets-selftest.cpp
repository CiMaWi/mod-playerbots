/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of `route check targets`' rules (RouteCheckTargetsRules.h, pathing plan PA8): the target lines, the
// order of the result classes, the row text and the counts. The engine's polygon answer itself is proven on a lab.
// Usage: route-check-targets-selftest
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "RouteCheckTargetsRules.h"

using namespace RouteCheckTargetsRules;

namespace
{
std::size_t Columns(std::string const& line)
{
    return static_cast<std::size_t>(std::count(line.begin(), line.end(), '\t')) + 1;
}

// The rule StandPoint applies to the nearest polygon point: within reach (3D), reach itself included
bool WithinReach(float yards, float reach) { return yards <= reach; }
}  // namespace

int main()
{
    int passed = 0;
    int failed = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        if (ok)
            ++passed;
        else
        {
            ++failed;
            std::cout << "FAIL: " << what << "\n";
        }
    };

    // Parse
    Target target;
    check(ParseTargetLine("0 -8916.5 -134.25 80.75 5.5 banker 5099 12345\r", target) && target.mapId == 0 &&
              target.x == -8916.5f && target.y == -134.25f && target.z == 80.75f && target.reach == 5.5f &&
              target.kind == "banker" && target.entry == "5099" && target.guid == "12345",
          "a full line with kind, entry and guid, and a trailing \\r");
    check(ParseTargetLine("571 1 2 3 4", target) && target.kind == "-" && target.entry == "-" && target.guid == "-",
          "a minimal line: kind, entry and guid are -");
    check(ParseTargetLine("530\t1\t2\t3\t4\tmailbox\t-\t77", target) && target.mapId == 530 &&
              target.kind == "mailbox" && target.entry == "-" && target.guid == "77",
          "tabs, and - passes through");
    check(ParseTargetLine("1 1 2 -3.5 0", target) && target.z == -3.5f && target.reach == 0.0f,
          "a negative z and a zero reach");
    check(!ParseTargetLine("# 571 1 2 3 4", target), "a comment");
    check(!ParseTargetLine("", target) && !ParseTargetLine("  \r", target), "blank lines");
    check(!ParseTargetLine("571x 1 2 3 4", target), "a bad map word");
    check(!ParseTargetLine("571 1 2 3", target), "too few numbers (no reach)");
    check(!ParseTargetLine("571 1 2 3 -1", target), "a negative reach");
    check(!ParseTargetLine("571 a 2 3 4", target), "a bad number");

    // Classification: the order of the tests
    Facts facts;
    facts.tileKnown = true;
    facts.stand = true;
    facts.steepNear = true;
    facts.otherFloor = true;
    check(Classify(facts) == Result::Stand, "stand wins over steep and other floor");
    facts.stand = false;
    check(Classify(facts) == Result::SteepOnly, "steep wins over other floor");
    facts.steepNear = false;
    check(Classify(facts) == Result::OtherFloor, "only a tall-box hit: other floor");
    facts.otherFloor = false;
    check(Classify(facts) == Result::NoPoly, "nothing near: no polygon");
    facts.stand = true;
    facts.steepNear = true;
    facts.otherFloor = true;
    facts.tileKnown = false;
    check(Classify(facts) == Result::NoTile, "no tile beats everything");
    check(std::string(Word(Result::Stand)) == "stand" && std::string(Word(Result::SteepOnly)) == "steep_only" &&
              std::string(Word(Result::OtherFloor)) == "other_floor" &&
              std::string(Word(Result::NoPoly)) == "no_poly" && std::string(Word(Result::NoTile)) == "no_tile",
          "result words");

    // The reach is inclusive, in 3D
    check(WithinReach(5.5f, 5.5f) && !WithinReach(5.5001f, 5.5f),
          "a stand point at exactly reach is accepted, beyond is not");

    // Row text
    Row row;
    row.index = 3;
    row.target.mapId = 1;
    row.target.x = 1.0f;
    row.target.y = 2.0f;
    row.target.z = 3.0f;
    row.target.reach = 5.5f;
    row.target.kind = "banker";
    row.target.entry = "5099";
    row.target.guid = "12";
    row.result = Result::Stand;
    row.sx = 1.5f;
    row.sy = 2.5f;
    row.sz = 3.5f;
    row.standYards = 0.8660f;
    row.fragment = true;
    row.nodes = {{10, 4.0f}, {11, 5.0f}, {12, 6.0f}, {13, 7.0f}, {14, 8.0f}, {15, 9.0f}};
    std::string text = FormatRow(row);
    check(Columns(text) == COLUMNS && Columns(ColumnLine()) == COLUMNS,
          "a stand row has 18 columns, as the column line");
    check(text.find("\tstand\t1.5000\t2.5000\t3.5000\t0.8660\t1\t10\t4.0000\t10:4.0000,11:5.0000,12:6.0000,13:7.0000,"
                    "14:8.0000") != std::string::npos &&
              text.find("15:") == std::string::npos,
          "a stand row lists the stand point, the fragment flag and five nodes, nearest first");
    row.fragment = false;
    check(FormatRow(row).find("\t0.8660\t0\t") != std::string::npos, "fragment 0 on a stand row");
    row.nodes.resize(2);
    text = FormatRow(row);
    check(Columns(text) == COLUMNS && text.find("\t10\t4.0000\t10:4.0000,11:5.0000") != std::string::npos &&
              text.back() == '0',
          "nodes5 with fewer than five nodes");
    row.nodes.clear();
    text = FormatRow(row);
    check(Columns(text) == COLUMNS && text.size() >= 6 && text.substr(text.size() - 6) == "\t-\t-\t-",
          "no nodes: - fill");
    row.result = Result::SteepOnly;
    text = FormatRow(row);
    check(Columns(text) == COLUMNS && text.find("\tsteep_only\t-\t-\t-\t-\t-\t") != std::string::npos,
          "a row with no stand point: sx sy sz stand_yd and fragment are -");
    row.fragment = true;
    check(FormatRow(row).find("\t-\t-\t-\t-\t-\t") != std::string::npos, "the fragment column only on stand rows");

    // Counts
    Counts counts;
    Row each;
    for (Result const result : {Result::Stand, Result::Stand, Result::SteepOnly, Result::OtherFloor, Result::NoPoly,
                                Result::NoPoly, Result::NoPoly, Result::NoTile})
    {
        each.result = result;
        each.fragment = result == Result::Stand && counts.stand == 0;
        Count(counts, each);
    }
    check(counts.rows == 8 && counts.stand == 2 && counts.fragment == 1 && counts.steepOnly == 1 &&
              counts.otherFloor == 1 && counts.noPoly == 3 && counts.noTile == 1,
          "counts per class");
    check(counts.stand + counts.steepOnly + counts.otherFloor + counts.noPoly + counts.noTile == counts.rows,
          "the classes add up to the rows");
    check(SummaryLine(counts) ==
              "ROUTE check targets: 8 rows: stand 2 (fragment 1), steep_only 1, other_floor 1, no_poly 3, no_tile 1",
          "summary line");

    // Written in input order after a run sorted by map (stable), as the command does
    std::vector<std::uint32_t> maps = {571, 0, 530, 0, 571, 1, 0};
    std::vector<std::size_t> order(maps.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return maps[a] < maps[b]; });
    check(order == std::vector<std::size_t>({1, 3, 6, 5, 2, 0, 4}), "the run's order is by map, stable");
    std::vector<std::string> rows(maps.size());
    for (std::size_t const index : order)
        rows[index] = std::to_string(index + 1);
    check(rows == std::vector<std::string>({"1", "2", "3", "4", "5", "6", "7"}), "the rows come back in input order");

    std::cout << "route-check-targets: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
