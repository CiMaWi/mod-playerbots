/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the roster's count change (src/Bot/PopulationPolicy.h, ".playerbots population resize"): the
// level ranges and their shares, which ranges lose and gain for fixture counts, the removable limit and the class pick
// of an added bot, and the startup factory's refill with the cap on and off (the checks ".playerbots population
// selftest" runs), then the creation fixture's plans 2000 -> 2050 -> 1950 as the console logs them.
#include <iostream>
#include <string>
#include <vector>

#include "PopulationPolicy.h"

int main()
{
    std::vector<std::string> failures;
    uint32_t const checks = PopulationPolicy::ResizeSelftest(failures) + PopulationPolicy::RefillSelftest(failures);
    for (std::string const& failure : failures)
        std::cout << "FAIL " << failure << "\n";

    std::vector<PopulationPolicy::ResizeRange> grow = PopulationPolicy::ResizeCreationFixture();
    PopulationPolicy::ResizePlan(grow, 2050);
    std::cout << "plan 2000 -> 2050: " << PopulationPolicy::ResizePlanText(grow) << "\n";
    std::vector<PopulationPolicy::ResizeRange> shrink = PopulationPolicy::ResizeApply(grow);
    PopulationPolicy::ResizePlan(shrink, 1950);
    std::cout << "plan 2050 -> 1950: " << PopulationPolicy::ResizePlanText(shrink) << "\n";
    std::cout << "population resize selftest: " << checks << " checks, " << failures.size() << " failures"
              << (failures.empty() ? " (PASS)" : "") << "\n";
    return failures.empty() ? 0 : 1;
}
