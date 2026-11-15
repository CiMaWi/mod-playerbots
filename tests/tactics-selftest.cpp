/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone run of the pure tactic suites (src/Bot/Tactics/TacticChecks.h) with the conf.dist defaults: the same
// suites and the same lines as `.playerbots tactics selftest`, one "<name>: PASS (<n> checks, 0 failed)" per suite.
// The server's scope, cc and interrupt suites (the command parser, the spell store) and gate (BotProfileMgr.cpp's
// percentile math) are not run here.
#include <cstdio>
#include <string>

#include "TacticChecks.h"

int main()
{
    TacticSuiteInputs const inputs;
    unsigned failedSuites = 0;
    for (TacticSuite const& suite : TACTIC_SUITES)
    {
        if (!RunTacticSuite(suite, inputs, [](std::string const& line) { std::printf("%s\n", line.c_str()); }))
            ++failedSuites;
    }

    std::printf("TACTICS standalone: %u suites, %u failed (%s)\n", static_cast<unsigned>(TACTIC_SUITES.size()),
                failedSuites, failedSuites ? "FAIL" : "PASS");
    return failedSuites ? 1 : 0;
}
