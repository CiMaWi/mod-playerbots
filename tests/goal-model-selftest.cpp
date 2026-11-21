/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the goal model (travel plan WT14, WT.21): the scenarios of GoalModelHarness.h on synthetic bots
// and places, each about the chosen goal and its reason, and the planner's route cost on a fixture graph
// (PlannerRouteCostHarness.h, planning WN1a) and candidates on every map (PlannerReachHarness.h, WN1b). The time per
// ranking is printed as information only. Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Mgr/Travel tests/goal-model-selftest.cpp

#include <cstdio>
#include <utility>
#include <vector>

#include "GoalModelHarness.h"
#include "PlannerReachHarness.h"
#include "PlannerRouteCostHarness.h"

int main()
{
    int passed = 0;
    int failed = 0;
    std::vector<Planner::GoalHarness::Check> checks = Planner::GoalHarness::SyntheticChecks();
    for (Planner::GoalHarness::Check& check : Planner::RouteCostHarness::RouteCostChecks())
        checks.push_back(std::move(check));
    for (Planner::GoalHarness::Check& check : Planner::ReachHarness::ReachChecks())
        checks.push_back(std::move(check));
    for (Planner::GoalHarness::Check const& check : checks)
    {
        std::printf("%s %s: %s\n", check.pass ? "PASS" : "FAIL", check.name.c_str(), check.text.c_str());
        (check.pass ? passed : failed)++;
    }
    std::printf("info: rank over 40 spots and 40 hubs %.1f us\n", Planner::GoalHarness::RankMicroseconds());
    std::printf("goal model selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
