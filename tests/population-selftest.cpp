/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the population rules (src/Bot/PopulationPolicy.h): the rotation schedule, the retirement of
// surplus bots, the roster's count change (".playerbots population resize"), the startup factory's refill, the
// roster in memory (src/Bot/PopulationRoster.h) and the logout rule, each selftest printing its own check count,
// then the creation fixture's plans 2000 -> 2050 -> 1950 as the console logs them. An argument "--full" runs the
// longer rotation and retirement simulations.
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "PopulationPolicySim.h"

int main(int argc, char** argv)
{
    bool const full = argc > 1 && std::strcmp(argv[1], "--full") == 0;
    std::vector<std::string> failures;
    uint32_t const rotation = PopulationPolicy::RotationSelftest(failures, full);
    uint32_t const retire = PopulationPolicy::RetireSelftest(failures, full);
    uint32_t const resize = PopulationPolicy::ResizeSelftest(failures);
    uint32_t const refill = PopulationPolicy::RefillSelftest(failures);
    uint32_t const roster = PopulationPolicy::RosterSelftest(failures);
    uint32_t const logout = PopulationPolicy::LogoutSelftest(failures);
    uint32_t const checks = rotation + retire + resize + refill + roster + logout;
    for (std::string const& failure : failures)
        std::cout << "FAIL " << failure << "\n";

    std::cout << "rotation: " << rotation << " checks\n";
    std::cout << "retire: " << retire << " checks\n";
    std::cout << "resize: " << resize << " checks\n";
    std::cout << "refill: " << refill << " checks\n";
    std::cout << "logout: " << logout << " checks\n";

    std::vector<PopulationPolicy::ResizeRange> grow = PopulationPolicy::ResizeCreationFixture();
    PopulationPolicy::ResizePlan(grow, 2050);
    std::cout << "plan 2000 -> 2050: " << PopulationPolicy::ResizePlanText(grow) << "\n";
    std::vector<PopulationPolicy::ResizeRange> shrink = PopulationPolicy::ResizeApply(grow);
    PopulationPolicy::ResizePlan(shrink, 1950);
    std::cout << "plan 2050 -> 1950: " << PopulationPolicy::ResizePlanText(shrink) << "\n";
    std::cout << "population selftest: " << checks << " checks, " << failures.size() << " failures"
              << (failures.empty() ? " (PASS)" : "") << "\n";
    return failures.empty() ? 0 : 1;
}
