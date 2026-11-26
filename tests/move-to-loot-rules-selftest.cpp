/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Selftest of the move to loot's give-up (src/Ai/Base/Actions/MoveToLootRules.h, needs and errands NR5): a target the
// bot gets no nearer to is given up 5 s after its first call, also when the calls come 10 s or more apart (the old
// rule restarted the clock at such a gap and never gave up); a gain restarts the clock, jitter does not; a reached
// target is never given up; a new target starts its own clock. Where the bot stands is a live test (nr-loot-mounted).
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/Base/Actions tests/move-to-loot-rules-selftest.cpp

#include <cstdio>
#include <initializer_list>

#include "MoveToLootRules.h"

namespace
{
using namespace MoveToLootRules;

int passed = 0;
int failed = 0;

void Check(bool ok, char const* what)
{
    if (ok)
    {
        ++passed;
        return;
    }
    ++failed;
    std::printf("FAILED: %s\n", what);
}

constexpr float CONTACT = 0.45f;  // Playerbots.ContactDistance's default
constexpr std::uint64_t CORPSE = 0xF130000000001234ull;
constexpr std::uint64_t OTHER = 0xF130000000005678ull;

// Calls every `gapMs` at a fixed `yards` from the first call at `startMs`, for at most `forMs`: the ms after the
// first call at which the target was given up, or -1
long GivenUpAfter(Watch& watch, std::uint64_t target, float yards, std::uint32_t startMs, std::uint32_t gapMs,
                  std::uint32_t forMs)
{
    for (std::uint32_t t = 0; t <= forMs; t += gapMs)
        if (GiveUp(watch, target, yards, startMs + t, CONTACT))
            return static_cast<long>(t);
    return -1;
}
}  // namespace

int main()
{
    // Out of reach, calls on every tick (1 s): given up at the first call 5 s after the first
    {
        Watch watch;
        Check(GivenUpAfter(watch, CORPSE, 8.0f, 1000, 1000, 30000) == 5000, "ticks 1 s apart: given up at 5 s");
        Check(watch.target == 0, "a give-up clears the watch");
    }

    // Out of reach, calls 10 s and 12 s apart (another action claims the ticks between): the second call gives up
    for (std::uint32_t const gap : {10000u, 12000u, 30000u})
    {
        Watch watch;
        Check(GivenUpAfter(watch, CORPSE, 8.0f, 1000, gap, 120000) == static_cast<long>(gap),
              "calls 10 s or more apart: given up at the second call");
    }

    // The clock counts from the first call, not from a later one: no call restarts it without a gain
    {
        Watch watch;
        Check(!GiveUp(watch, CORPSE, 8.0f, 0, CONTACT), "first call keeps the target");
        Check(!GiveUp(watch, CORPSE, 8.0f, 4999, CONTACT), "4.999 s without a gain keeps it");
        Check(GiveUp(watch, CORPSE, 8.0f, 5000, CONTACT), "5 s without a gain gives it up");
    }

    // A gain of more than half a yard restarts the clock; jitter does not; a step back does not
    {
        Watch watch;
        GiveUp(watch, CORPSE, 10.0f, 0, CONTACT);
        Check(!GiveUp(watch, CORPSE, 9.0f, 4000, CONTACT), "a gain keeps the target");
        Check(!GiveUp(watch, CORPSE, 9.0f, 8999, CONTACT), "5 s counted from the gain");
        Check(GiveUp(watch, CORPSE, 9.0f, 9000, CONTACT), "5 s after the gain without another: given up");

        Watch jitter;
        GiveUp(jitter, CORPSE, 10.0f, 0, CONTACT);
        GiveUp(jitter, CORPSE, 9.6f, 3000, CONTACT);
        Check(GiveUp(jitter, CORPSE, 9.7f, 5000, CONTACT), "jitter under half a yard is no gain");

        Watch back;
        GiveUp(back, CORPSE, 6.0f, 0, CONTACT);
        Check(GiveUp(back, CORPSE, 9.0f, 5000, CONTACT), "pulled farther away: given up");
    }

    // A bot moving steadily nearer is never given up, however long the way
    {
        Watch watch;
        bool given = false;
        for (std::uint32_t t = 0; t <= 60000; t += 1000)
            given = given || GiveUp(watch, CORPSE, 14.0f - t / 6000.0f, t, CONTACT);
        Check(!given, "steady gains: never given up");
    }

    // At the target (within the contact distance and its margin) it is never given up
    {
        Watch watch;
        Check(GivenUpAfter(watch, CORPSE, CONTACT + REACHED_MARGIN_YARDS, 0, 1000, 60000) == -1,
              "at the margin: never given up");
        Check(GivenUpAfter(watch, CORPSE, 1.0f, 0, 12000, 60000) == -1, "at the target, sparse calls: never");
    }

    // A new target starts its own clock; the old one's time does not carry over
    {
        Watch watch;
        GiveUp(watch, CORPSE, 8.0f, 0, CONTACT);
        Check(!GiveUp(watch, OTHER, 8.0f, 4000, CONTACT), "a new target keeps its first call");
        Check(!GiveUp(watch, OTHER, 8.0f, 8999, CONTACT), "its clock starts at its own first call");
        Check(GiveUp(watch, OTHER, 8.0f, 9000, CONTACT), "and gives up 5 s after it");
        Check(!GiveUp(watch, CORPSE, 8.0f, 9500, CONTACT), "after a give-up the old target starts afresh");
    }

    // The millisecond clock wraps (getMSTime): the difference is unsigned
    {
        Watch watch;
        GiveUp(watch, CORPSE, 8.0f, 0xFFFFF000u, CONTACT);
        Check(GiveUp(watch, CORPSE, 8.0f, 0xFFFFF000u + 5000u, CONTACT), "a wrapped clock still gives up at 5 s");
    }

    std::printf("move-to-loot-rules: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
