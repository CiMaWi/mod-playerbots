/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of `route check outdoor`'s rules (RouteCheckOutdoorRules.h, navigation plan NV32): the flying
// area rule (Dalaran's no-fly WMO area over flyable ground), the liquid words, the point lines and the counts.
// The engine's answer itself is proven on a lab.
// Usage: route-check-outdoor-selftest
#include <cstdint>
#include <iostream>
#include <string>

#include "RouteCheckOutdoorRules.h"

using namespace RouteCheckOutdoorRules;

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

    // Flying area: the OUTLAND bit allows it, the NO_FLY_ZONE bit forbids it, no entry forbids it
    constexpr std::uint32_t CITY = 0x138;  // "312 for all cities" (DBCStructure.h)
    check(AreaAllowsFlight(true, AREA_FLAG_OUTLAND_BIT), "an Outland/Northrend area allows flying");
    check(!AreaAllowsFlight(true, CITY), "an Eastern Kingdoms city (no OUTLAND bit) does not");
    check(!AreaAllowsFlight(true, 0), "no flags: no flying");
    check(!AreaAllowsFlight(true, AREA_FLAG_OUTLAND_BIT | AREA_FLAG_NO_FLY_ZONE_BIT | CITY),
          "Dalaran's city area: OUTLAND but NO_FLY_ZONE: no flying");
    check(!AreaAllowsFlight(false, AREA_FLAG_OUTLAND_BIT), "no area entry: no flying");

    // Liquid words
    check(LiquidWord(LIQUID_NONE) == "none" && LiquidWord(LIQUID_ABOVE) == "above" &&
              LiquidWord(LIQUID_WALK) == "walk" && LiquidWord(LIQUID_IN) == "in" && LiquidWord(LIQUID_UNDER) == "under",
          "liquid words");
    check(LiquidWord(0x5) == "5", "an unknown status prints its number");

    // Point lines
    PointSpec point;
    check(ParsePointLine("571 5804.5 624.25 647.75 Krasus landing\r", point) && point.mapId == 571 &&
              point.x == 5804.5f && point.y == 624.25f && point.z == 647.75f && point.label == "Krasus landing",
          "a point line with a label");
    check(ParsePointLine("\t530 1 2 3", point) && point.mapId == 530 && point.label.empty(), "no label");
    check(!ParsePointLine("# 571 1 2 3", point), "a comment");
    check(!ParsePointLine("", point), "an empty line");
    check(!ParsePointLine("571x 1 2 3", point), "a bad map word");
    check(!ParsePointLine("571 1 2", point), "too few numbers");

    // Counts: a takeoff needs outdoor, a flying area and no deep water
    Counts counts;
    Count(counts, true, true, true, LIQUID_NONE);     // open Northrend ground
    Count(counts, true, true, true, LIQUID_IN);       // swimming: no takeoff
    Count(counts, true, true, false, LIQUID_NONE);    // Dalaran's streets
    Count(counts, true, false, true, LIQUID_NONE);    // a cave in a flying zone
    Count(counts, true, true, true, LIQUID_ABOVE);    // above water: takeoff
    Count(counts, false, false, false, LIQUID_NONE);  // an instance map
    check(counts.points == 6 && counts.outdoors == 4 && counts.indoors == 1 && counts.withoutMap == 1,
          "outdoor counts");
    check(counts.flyable == 4 && counts.takeoff == 2, "flying area and takeoff counts");
    std::string const summary = SummaryLine(counts);
    check(summary ==
              "ROUTE check outdoor: 6 points, 4 outdoors, 1 indoors, 1 without map; 4 in a flying area, 2 "
              "where a flight may start",
          "summary line");
    std::cout << "  " << summary << "\n";

    std::cout << "route-check-outdoor: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
