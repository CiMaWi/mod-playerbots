/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TACTICCHECKS_H
#define PLAYERBOTS_TACTICCHECKS_H

#include <array>
#include <format>
#include <functional>
#include <string>

#include "ArenaCover.h"
#include "ArenaDefensive.h"
#include "ArenaDrink.h"
#include "ArenaFocusChecks.h"
#include "ArenaJump.h"
#include "ArenaKite.h"
#include "BotAttributesChecks.h"
#include "LosTactic.h"
#include "NaturalCast.h"
#include "NaturalMove.h"
#include "NaturalPath.h"
#include "NaturalReaction.h"
#include "NaturalReactionChecks.h"
#include "NaturalTurn.h"
#include "PillarTactic.h"
#include "SkillGrowthChecks.h"
#include "TacticFollowersChecks.h"
#include "TacticGate.h"
#include "TacticGateChecks.h"
#include "TacticStrategyChecks.h"

// One check of a suite (check(ok, what)) and one line it prints (say(line)); the runner counts and prefixes them
using TacticCheck = std::function<void(bool, std::string const&)>;
using TacticSay = std::function<void(std::string const&)>;

// The numbers a suite reads from the server's config (the reaction suite); the offline test keeps the defaults
struct TacticSuiteInputs
{
    ReactionParams Reaction;
    float CurveExponent = TacticGateParams().CurveExponent;
};

// One pure suite of `.playerbots tactics selftest <name>`, run by the server and by tests/tactics-selftest.cpp
struct TacticSuite
{
    char const* Name;
    void (*Run)(TacticCheck const& check, TacticSay const& say, TacticSuiteInputs const& inputs);
};

// The suites the offline test runs; scope, cc and interrupt need the server (the command's parser, the spell store),
// gate BotProfileMgr.cpp's percentile math: Tactics.cpp runs them
inline constexpr std::array<TacticSuite, 17> TACTIC_SUITES = {{
    {"reaction", [](TacticCheck const& check, TacticSay const& say, TacticSuiteInputs const& inputs)
     { RunReactionChecks(check, say, inputs.Reaction, inputs.CurveExponent); }},
    {"attributes",
     [](TacticCheck const& check, TacticSay const& say, TacticSuiteInputs const&) { RunAttributesChecks(check, say); }},
    {"focus", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunFocusChecks(check); }},
    {"defensive",
     [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunDefensiveChecks(check); }},
    {"fake", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunFakeChecks(check); }},
    {"turn", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunTurnChecks(check); }},
    {"track", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunTrackChecks(check); }},
    {"path", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunPathChecks(check); }},
    {"cover", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunCoverChecks(check); }},
    {"los", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunLosChecks(check); }},
    {"pillar", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunPillarChecks(check); }},
    {"jump", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunJumpChecks(check); }},
    {"drink", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunDrinkChecks(check); }},
    {"kite", [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunKiteChecks(check); }},
    {"growth",
     [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunSkillGrowthChecks(check); }},
    {"followers",
     [](TacticCheck const& check, TacticSay const&, TacticSuiteInputs const&) { RunFollowerChecks(check); }},
    {"strategy",
     [](TacticCheck const& check, TacticSay const& say, TacticSuiteInputs const&) { RunStrategyChecks(check, say); }},
}};

// Runs one suite: every failure and every line of the suite go to print with the prefix "TACTICS selftest <name>",
// then the total; true when no check failed
inline bool RunTacticSuite(TacticSuite const& suite, TacticSuiteInputs const& inputs,
                           std::function<void(std::string const&)> const& print)
{
    uint32 checks = 0;
    uint32 failures = 0;
    std::string const prefix = std::string("TACTICS selftest ") + suite.Name;
    suite.Run(
        [&](bool ok, std::string const& what)
        {
            ++checks;
            if (ok)
                return;

            ++failures;
            print(prefix + " FAILED: " + what);
        },
        [&](std::string const& line) { print(prefix + ": " + line); }, inputs);
    print(std::format("{}: {} ({} checks, {} failed)", prefix, failures ? "FAIL" : "PASS", checks, failures));
    return failures == 0;
}

#endif
