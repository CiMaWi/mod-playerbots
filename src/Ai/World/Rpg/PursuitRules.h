/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PURSUITRULES_H
#define _PLAYERBOT_PURSUITRULES_H

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "PursuitTypes.h"
#include "StatusEndTypes.h"

// One give-up clock per order (questing WT35c): it runs only while the order's executor runs it, starts again at every
// progress and gives the order up when a stretch without progress reaches the limit; a cause that waiting cannot
// change gives up at once. Pure, for the selftest (tests/quest-pursuit-selftest.cpp)
namespace Pursuit
{
// A pause between two running ticks counts this much at most (as QuestExecRules.h's CLOCK_GAP_MS): a fight, the loot
// or another action holds the clock, never restarts it
constexpr std::uint32_t CLOCK_GAP_MS = 5000;
constexpr std::uint32_t MINUTE_MS = 60000;
constexpr std::uint32_t SHORT_LIMIT_MS = 2 * MINUTE_MS;  // the limit of a bot under the test constraint shortgiveup

// The give-up's cause words (the Status row's word, the failure row's source). The executor's end words
// (QuestExecRules.h EndWord: unsupported:<kind>, no_place, no_target, locked) are causes too
constexpr char const* TIMEOUT = "timeout";    // the limit passed without progress
constexpr char const* NO_ENDER = "no_ender";  // no ender the bot can use, or the limit passed at the ender

// Every word a quest status may be given up with (config/wlog-checks/questing.sql's quest_giveup_without_fail lists
// the same): the clock's, the executor's end words, a walk's deadline (GoToRules.h's timeout) and the refusals
constexpr std::string_view GIVE_UP_WORDS[] = {"abandon", "abandoned", "no_place", "no_target", "locked",
                                              TIMEOUT,   NO_ENDER,    "no_item",  "no_spell"};
constexpr std::string_view GIVE_UP_PREFIXES[] = {"unsupported:", "refused:"};

inline bool IsGiveUpWord(char const* word)
{
    if (!word)
        return false;
    std::string_view const text(word);
    return std::find(std::begin(GIVE_UP_WORDS), std::end(GIVE_UP_WORDS), text) != std::end(GIVE_UP_WORDS) ||
           std::any_of(std::begin(GIVE_UP_PREFIXES), std::end(GIVE_UP_PREFIXES),
                       [text](std::string_view prefix) { return text.starts_with(prefix); });
}

// The words of a use that really fails (the executor's use ends, QuestUseExecRules.h): its item or its spell is
// missing, the game refused it, or the executor has no way to do it
constexpr std::string_view USE_FAIL_WORDS[] = {"no_item", "no_spell"};

inline bool IsUseFailWord(char const* word)
{
    if (!word)
        return false;
    std::string_view const text(word);
    return std::find(std::begin(USE_FAIL_WORDS), std::end(USE_FAIL_WORDS), text) != std::end(USE_FAIL_WORDS) ||
           std::any_of(std::begin(GIVE_UP_PREFIXES), std::end(GIVE_UP_PREFIXES),
                       [text](std::string_view prefix) { return text.starts_with(prefix); });
}

// Whether a quest status's end writes its failure row at the status door (questing WQ19b). On an order (WQ.43 point
// 6) a target that is not there writes none: only no_ender does, the failed hand-in, and a use that really fails
// (WQ19a: given up with a use's word, for the quest of the need being worked); any other quest status does when it is
// given up (Abandoned) or ends with a give-up word whatever its outcome (a walk's deadline, timeout, ends TimedOut)
inline bool WritesFailure(bool order, StatusEnd::Outcome outcome, char const* word)
{
    if (order)
        return word && (std::string_view(word) == NO_ENDER ||
                        (outcome == StatusEnd::Outcome::Abandoned && IsUseFailWord(word)));
    return outcome == StatusEnd::Outcome::Abandoned || IsGiveUpWord(word);
}

inline std::uint32_t LimitMs(std::uint32_t minutes, bool shortened)
{
    return shortened ? SHORT_LIMIT_MS : minutes * MINUTE_MS;
}

// The clock of an order: its slot, or the oldest slot taken anew, so the order a bot goes back to after another
// resumes its clock and a new order starts one
inline Clock& Take(Clocks& clocks, Key key)
{
    ++clocks.uses;
    Clock* oldest = &clocks.slots[0];
    for (Clock& clock : clocks.slots)
    {
        if (clock.key == key)
        {
            clock.lastUse = clocks.uses;
            return clock;
        }
        if (clock.lastUse < oldest->lastUse)
            oldest = &clock;
    }
    *oldest = Clock{};
    oldest->key = key;
    oldest->lastUse = clocks.uses;
    return *oldest;
}

// An order that ended for good (done, dropped): its clock goes, so a later order of the same target starts at zero
inline void Forget(Clocks& clocks, Key key)
{
    for (Clock& clock : clocks.slots)
        if (clock.key == key)
            clock = Clock{};
}

// One tick. `running`: the order's executor runs it this tick (an errand, a rest, a death or a trip to something
// else does not; the tick only holds the clock). `run`: the order's run; the first tick of a new run resumes the clock
// without counting the pause, and a run after a give-up starts it at zero. `sinceLastMs`: the time since the tick
// before. `progress`: the order's count (a quest: its log's counts summed); a rise restarts the idle time, a fall
// only lowers the mark
inline void Tick(Clock& clock, bool running, std::uint32_t run, std::uint32_t sinceLastMs, std::uint32_t progress)
{
    if (!running)
    {
        clock.ticking = false;
        return;
    }
    if (clock.givenUp && clock.run != run)
    {
        Key const key = clock.key;
        std::uint32_t const lastUse = clock.lastUse;
        clock = Clock{};
        clock.key = key;
        clock.lastUse = lastUse;
    }
    bool const counts = clock.ticking && clock.run == run;
    std::uint32_t const counted = counts ? std::min(sinceLastMs, CLOCK_GAP_MS) : 0;
    clock.pursuedMs += counted;
    if (clock.started && progress > clock.progress)
        clock.idleMs = 0;
    else
        clock.idleMs += counted;
    clock.progress = progress;
    clock.started = true;
    clock.run = run;
    clock.ticking = true;
}

// The limit is reached: exactly at it, never before
inline bool Over(Clock const& clock, std::uint32_t limitMs) { return clock.idleMs >= limitMs; }

// The cause the order is given up for now, nullptr while it goes on: `immediate` (a cause waiting cannot change, e.g.
// an executor's end word) at once, else `timeoutWord` once the limit is reached (NO_ENDER at the ender, else TIMEOUT)
inline char const* Decide(Clock const& clock, std::uint32_t limitMs, char const* immediate,
                          char const* timeoutWord = TIMEOUT)
{
    if (immediate)
        return immediate;
    return Over(clock, limitMs) ? timeoutWord : nullptr;
}

// The give-up of a run is reported: true only the first time, so the end's hook and the executor's own report of the
// same give-up write one failure row and one record update
inline bool Report(Clock& clock, std::uint32_t run)
{
    if (clock.givenUp && clock.run == run)
        return false;
    clock.givenUp = true;
    clock.run = run;
    clock.ticking = false;
    return true;
}

// The failure row's minutes: the pursuit's running time, whole minutes rounded down
inline std::uint32_t Minutes(Clock const& clock) { return clock.pursuedMs / MINUTE_MS; }
}  // namespace Pursuit

#endif
