/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the flight's destination choice (src/Mgr/Travel/FlightChoiceRules.h, pathing PA69: G358, G378) on
// a made-up taxi map: the capital draw is a preference that falls back to the level zones, and a zone counts through
// any of its nodes with a taxi path from the start node, so zones on another continent are passed over; G196: a master
// the bot's phase at its spawn cannot see is no candidate.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Mgr/Travel tests/flight-choice-selftest.cpp

#include <cstdint>
#include <iostream>
#include <map>
#include <string_view>
#include <vector>

#include "FlightChoiceRules.h"

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string_view description)
{
    if (condition)
        ++passed;
    else
    {
        ++failed;
        std::cerr << "FAIL " << description << '\n';
    }
}

// Zones and their nodes; a node with a path from the start node gets the path {start, node}
struct TaxiMap
{
    std::map<std::uint32_t, std::vector<std::uint32_t>> zones;
    std::vector<std::uint32_t> reachable;

    std::vector<std::uint32_t> NodesOf(std::uint32_t zone) const
    {
        auto const found = zones.find(zone);
        return found == zones.end() ? std::vector<std::uint32_t>{} : found->second;
    }

    std::vector<std::uint32_t> PathTo(std::uint32_t node) const
    {
        for (std::uint32_t const to : reachable)
            if (to == node)
                return {1, node};
        return {};
    }
};

// Every draw of index 0, then every draw of the last index: both ends of the random choice
std::vector<std::uint32_t> ChooseWith(TaxiMap const& map, std::vector<std::uint32_t> const& capitals,
                                      std::vector<std::uint32_t> const& levelZones, bool last)
{
    return FlightChoice::Choose(
        capitals, levelZones, [&map](std::uint32_t zone) { return map.NodesOf(zone); },
        [&map](std::uint32_t node) { return map.PathTo(node); }, [last](std::size_t n) { return last ? n - 1 : 0; });
}

void CheckCapitalFallback()
{
    // G358 (flightmaster-24851, -7823, -26560): the six capitals hold no node with a path from this continent
    TaxiMap map;
    map.zones = {{1519, {2}}, {1637, {3}}, {3487, {4}}, {4395, {}}, {65, {10, 11}}};
    map.reachable = {10, 11};
    for (bool const last : {false, true})
    {
        std::vector<std::uint32_t> const path = ChooseWith(map, {1519, 1637, 3487, 4395}, {65}, last);
        Check(path.size() == 2 && (path[1] == 10 || path[1] == 11),
              "the capital draw without a reachable capital falls back to the level zones");
    }
    map.reachable = {3, 10};
    Check(ChooseWith(map, {1519, 1637}, {65}, false) == std::vector<std::uint32_t>{1, 3},
          "a reachable capital is taken when drawn");
}

void CheckReachableZone()
{
    // G378 (flightmaster-16189): level zones on another continent have nodes but no path from the start node
    TaxiMap map;
    map.zones = {{3483, {20, 21}}, {3521, {22}}, {3433, {30, 31, 32}}};
    map.reachable = {32};
    for (bool const last : {false, true})
        Check(ChooseWith(map, {}, {3483, 3521, 3433}, last) == std::vector<std::uint32_t>{1, 32},
              "the zones without a path are passed over; the zone's reachable node is found, not only the one drawn");
    map.reachable = {};
    Check(ChooseWith(map, {}, {3483, 3521, 3433}, false).empty(), "no node reachable: no flight");
    Check(ChooseWith(map, {}, {}, false).empty(), "no zone: no flight");
}

// G196: a flight master in a quest phase. A row is one automatic phase row (spell_area) of the master's area: its
// phase mask and whether it fits the bot's quest state
struct Row
{
    std::uint32_t phaseMask;
    bool fits;
};

bool Seen(std::uint32_t spawnPhaseMask, std::vector<Row> const& rows)
{
    return FlightChoice::MasterSeen(spawnPhaseMask, rows, [](Row const& row) { return row.fits; });
}

void CheckMasterPhase()
{
    // The Shadow Vault's kind (30314): phase 2 from arrival until the area's quest is rewarded; the master is phase 1
    Check(!Seen(1, {{2, true}}), "a row that fits the bot moves it out of the master's phase: no candidate");
    Check(Seen(1, {{2, false}}), "the row no longer fits (quest rewarded): the normal phase sees the master");
    // Death's Rise's kind (31069): several rows of one chain, each until the next quest
    Check(!Seen(1, {{2, false}, {64, true}, {128, false}}), "one fitting row of a chain hides the phase 1 master");
    Check(Seen(1, {{2, false}, {64, false}, {128, false}}), "no row of the chain fits: seen");
    // Dun Niffelem's kind (32571): the master stands only in phase 4, which a taken quest's row brings
    Check(!Seen(4, {{4, false}}), "a master outside the normal phase without the bot's quest: no candidate");
    Check(Seen(4, {{4, true}, {64, true}}), "the quest's row brings the master's phase: seen");
    Check(!Seen(4, {}), "a master outside the normal phase in an area without rows: no candidate");
    // The common case: no rows, a normal-phase spawn
    Check(Seen(1, {}), "a normal-phase master without rows is seen");
    Check(Seen(3, {{2, true}}), "a spawn in both phases is seen from either");
}
}  // namespace

int main()
{
    CheckCapitalFallback();
    CheckReachableZone();
    CheckMasterPhase();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
