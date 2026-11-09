/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the planner pool (travel plan WT12a): Planner::PoolSelftest (PlannerPool.h) with 4 submitter
// threads x 1000 bots. Build (Git Bash, module root):
//   clang-cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I src/Ai/World/Rpg tests/planner-pool-selftest.cpp

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "PlannerPool.h"

int main()
{
    constexpr std::uint32_t BOTS_PER_THREAD = 1000;
    std::vector<std::string> failures;
    std::vector<std::string> notes;
    auto const started = std::chrono::steady_clock::now();
    std::uint32_t const checks = Planner::PoolSelftest(failures, notes, BOTS_PER_THREAD);
    auto const ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    for (std::string const& note : notes)
        std::cout << "  " << note << "\n";
    for (std::string const& failure : failures)
        std::cout << "FAIL: " << failure << "\n";
    std::cout << "planner-pool-selftest: " << (checks - failures.size()) << " passed, " << failures.size()
              << " failed (" << ms << " ms)\n";
    return failures.empty() ? 0 : 1;
}
