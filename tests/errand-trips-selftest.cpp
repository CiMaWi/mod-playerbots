/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of trips and errands (travel plan WT15, WT.6; the upkeep needs as providers, WU12): the scenarios of
// ErrandHarness.h on synthetic bots and stops, each about the stops a trip makes and the errands it does or refuses.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/errand-trips-selftest.cpp

#include <cstdio>

#include "ErrandHarness.h"

int main()
{
    int passed = 0;
    int failed = 0;
    for (WorldTravel::ErrandHarness::Check const& check : WorldTravel::ErrandHarness::SyntheticChecks())
    {
        std::printf("%s %s: %s\n", check.pass ? "PASS" : "FAIL", check.name.c_str(), check.text.c_str());
        (check.pass ? passed : failed)++;
    }
    std::printf("errand trips selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
