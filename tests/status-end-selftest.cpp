/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the status-end rules (consolidation CS14, design rpg-action-layer.md 2.6): every status has a
// row, the clean-up table equals design 2.3 cell by cell, Judged is false for exactly three outcomes, every outcome
// has its own word, the duration bit and the reset rule of design 6.2.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/status-end-selftest.cpp

#include <cstdio>
#include <cstring>

#include "StatusEndRules.h"

namespace
{
using StatusEnd::Outcome;

static_assert(StatusEnd::STATUS_COUNT == 12, "twelve RPG statuses (RPG_STATUS_END)");
static_assert(StatusEnd::STATUS_ERRANDS + 1 == StatusEnd::STATUS_COUNT, "errands is the last status");
static_assert(StatusEnd::OUTCOME_COUNT == static_cast<unsigned>(Outcome::Sick) + 1, "eight outcomes");
static_assert(!StatusEnd::CleansUp(StatusEnd::STATUS_COUNT, Outcome::Finished), "no row past the last status");

char const* const STATUS_NAMES[StatusEnd::STATUS_COUNT] = {
    "idle",          "go_grind", "go_camp",     "wander_random", "wander_npc", "do_quest",
    "travel_flight", "rest",     "outdoor_pvp", "do_gather",     "travel",     "errands",
};

// Design 2.3, written out: the columns are Arrived Finished TimedOut Stuck Abandoned Interrupted Ordered Sick. The
// cells without a site (errands Arrived, travel Finished) follow the rule "the clean-up runs at every end".
constexpr bool EXPECTED_CLEAN_UP[StatusEnd::STATUS_COUNT][StatusEnd::OUTCOME_COUNT] = {
    {false, false, false, false, false, false, false, false},  // idle
    {false, false, false, false, false, false, false, false},  // go grind
    {false, false, false, false, false, false, false, false},  // go camp
    {false, false, false, false, false, false, false, false},  // wander random
    {false, false, false, false, false, false, false, false},  // wander npc
    {false, false, false, false, false, false, false, false},  // do quest
    {false, false, false, false, false, false, false, false},  // travel flight
    {false, false, false, false, false, false, false, false},  // rest
    {false, false, false, false, false, false, false, false},  // outdoor PvP
    {false, false, false, false, false, false, false, false},  // do gather
    {true, true, true, true, true, false, true, true},         // travel: gap T1 (Interrupted)
    {true, true, true, false, true, false, false, true},       // errands: Stuck until CS14d, gaps E1, E2
};

constexpr bool EXPECTED_JUDGED[StatusEnd::OUTCOME_COUNT] = {true, true, true, true, true, false, false, false};

char const* const EXPECTED_WORDS[StatusEnd::OUTCOME_COUNT] = {"arrived",   "finished",    "timeout", "stuck",
                                                              "abandoned", "interrupted", "order",   "sick"};

constexpr bool EXPECTED_DURATION_NORMAL[StatusEnd::STATUS_COUNT] = {false, false, false, true,  true,  false,
                                                                    false, true,  true,  false, false, false};

struct Counts
{
    int passed = 0;
    int failed = 0;

    void Check(bool pass, char const* name, char const* detail)
    {
        std::printf("%s %s: %s\n", pass ? "PASS" : "FAIL", name, detail);
        (pass ? passed : failed)++;
    }
};
}  // namespace

int main()
{
    std::printf("status end selftest\n");
    Counts counts;
    char detail[128];

    for (std::uint32_t status = 0; status < StatusEnd::STATUS_COUNT; ++status)
    {
        for (std::uint32_t o = 0; o < StatusEnd::OUTCOME_COUNT; ++o)
        {
            Outcome const outcome = static_cast<Outcome>(o);
            bool const actual = StatusEnd::CleansUp(status, outcome);
            std::snprintf(detail, sizeof(detail), "%s x %s: %s", STATUS_NAMES[status], StatusEnd::Word(outcome),
                          actual ? "yes" : "no");
            counts.Check(actual == EXPECTED_CLEAN_UP[status][o], "clean-up", detail);
        }
    }

    for (std::uint32_t o = 0; o < StatusEnd::OUTCOME_COUNT; ++o)
    {
        Outcome const outcome = static_cast<Outcome>(o);
        char const* const word = StatusEnd::Word(outcome);
        bool unique = true;
        for (std::uint32_t other = 0; other < StatusEnd::OUTCOME_COUNT; ++other)
            if (other != o && std::strcmp(word, StatusEnd::Word(static_cast<Outcome>(other))) == 0)
                unique = false;
        std::snprintf(detail, sizeof(detail), "outcome %u is '%s'", static_cast<unsigned>(o), word);
        counts.Check(word[0] != '\0' && unique && std::strcmp(word, EXPECTED_WORDS[o]) == 0, "word", detail);

        bool const judged = StatusEnd::Judged(outcome);
        std::snprintf(detail, sizeof(detail), "%s: %s", word, judged ? "judged" : "not judged");
        counts.Check(judged == EXPECTED_JUDGED[o], "judged", detail);
    }

    for (std::uint32_t status = 0; status < StatusEnd::STATUS_COUNT; ++status)
    {
        bool const normal = StatusEnd::DurationIsNormalEnd(status);
        std::snprintf(detail, sizeof(detail), "%s: %s", STATUS_NAMES[status], normal ? "normal end" : "not done");
        counts.Check(normal == EXPECTED_DURATION_NORMAL[status], "timeout", detail);
    }

    for (int farTeleport = 0; farTeleport < 2; ++farTeleport)
    {
        for (int isTravel = 0; isTravel < 2; ++isTravel)
        {
            bool const keeps = StatusEnd::KeepsStatus(farTeleport != 0, isTravel != 0);
            std::snprintf(detail, sizeof(detail), "far teleport %d, trip %d: %s", farTeleport, isTravel,
                          keeps ? "keeps the status" : "ends it");
            counts.Check(keeps == (farTeleport != 0 && isTravel != 0), "reset", detail);
        }
    }

    for (std::uint32_t o = 0; o < StatusEnd::OUTCOME_COUNT; ++o)
    {
        Outcome const outcome = static_cast<Outcome>(o);
        char expected[32];
        std::snprintf(expected, sizeof(expected), "abort:%s", StatusEnd::Word(outcome));
        char const* const source = StatusEnd::TripAbortSource(outcome);
        std::snprintf(detail, sizeof(detail), "%s: %s", StatusEnd::Word(outcome), source);
        counts.Check(std::strcmp(source, expected) == 0, "trip abort source", detail);
    }

    for (std::uint32_t o = 0; o < StatusEnd::OUTCOME_COUNT; ++o)
    {
        Outcome const outcome = static_cast<Outcome>(o);
        bool const unnamed = StatusEnd::TripAbortLogsInfo(false, outcome);
        bool const named = StatusEnd::TripAbortLogsInfo(true, outcome);
        std::snprintf(detail, sizeof(detail), "%s: unnamed %s, named %s", StatusEnd::Word(outcome),
                      unnamed ? "info" : "debug", named ? "info" : "debug");
        counts.Check(named && unnamed == (outcome == Outcome::Ordered), "trip abort log level", detail);
    }

    // Change 3 (design 4.3): an ordered end of a trip (`rpg idle`, a bridge pause, a GM hold or stop) runs the trip's
    // abort arm: the Travel event abort:order (or the order's own source), the trip's abort decision and the INFO
    // line; an ordered end of errands still runs no clean-up (gap E2)
    counts.Check(StatusEnd::CleansUp(StatusEnd::STATUS_TRAVEL, Outcome::Ordered) &&
                     std::strcmp(StatusEnd::TripAbortSource(Outcome::Ordered), "abort:order") == 0 &&
                     StatusEnd::TripAbortLogsInfo(false, Outcome::Ordered),
                 "ordered trip end", "travel x order: abort:order, the abort decision, an INFO line");
    counts.Check(!StatusEnd::CleansUp(StatusEnd::STATUS_ERRANDS, Outcome::Ordered), "ordered errand end",
                 "errands x order: no clean-up");

    std::printf("status end selftest: %d passed, %d failed\n", counts.passed, counts.failed);
    return counts.failed ? 1 : 0;
}
