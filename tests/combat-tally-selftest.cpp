/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the fight tally's arithmetic (combat-foundation CF1; src/Bot/Engine/CombatTally.h): the four tick
// kinds and the uncovered time before the first tick add up to the fight's length; moving never exceeds the covered
// time; a fight without a tick is all uncovered; getMSTime's wrap; a repeated close (the core repeats the leave
// edge) and an open while open; the caps and the packed amount's round trip; each Classify row.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot/Engine tests/combat-tally-selftest.cpp

#include <cstdint>
#include <cstdio>
#include <string>

#include "CombatTally.h"

namespace
{
using namespace CombatTally;

int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name.c_str());
    (pass ? passed : failed)++;
}

bool Adds(Summary const& summary)
{
    return summary.Covered() + summary.Uncovered() == summary.lengthMs && summary.movingMs <= summary.Covered();
}

void TestSums()
{
    Fight fight;
    fight.Open(1000, 3);
    fight.Tick(1500, TickKind::Acted, true, 2, true);          // 1000-1500 uncovered
    fight.Tick(2000, TickKind::Idle, false, 4, false);         // 1500-2000 acted, moving
    fight.Tick(2700, TickKind::NoPower, true, 1, false);       // 2000-2700 idle
    fight.Tick(3000, TickKind::Busy, false, 1, false);         // 2700-3000 no power, moving
    fight.Tick(3400, TickKind::Acted, false, 3, true);         // 3000-3400 busy
    std::optional<Summary> const summary = fight.Close(4000);  // 3400-4000 acted
    Check(summary.has_value(), "sums: a close of an open fight returns its summary");
    if (!summary)
        return;
    Check(summary->lengthMs == 3000 && summary->Uncovered() == 500, "sums: length from the open, 500 ms uncovered");
    Check(summary->Of(TickKind::Acted) == 1100 && summary->Of(TickKind::Idle) == 700 &&
              summary->Of(TickKind::NoPower) == 300 && summary->Of(TickKind::Busy) == 400,
          "sums: each stretch belongs to the earlier tick's kind, the last one runs to the close");
    Check(summary->movingMs == 800, "sums: moving is the stretches whose earlier tick saw the bot move");
    Check(Adds(*summary), "sums: the four kinds and the uncovered time are the length; moving <= covered");
    Check(summary->casts == 2 && summary->maxEnemies == 4 && summary->groupSize == 3,
          "sums: casts counted, the largest enemy count kept, the group size from the open");
    Check(!fight.IsOpen(), "sums: the close resets the fight");
}

void TestNoTick()
{
    Fight fight;
    fight.Open(5000, 0);
    std::optional<Summary> const summary = fight.Close(9000);
    Check(summary && summary->lengthMs == 4000 && summary->Covered() == 0 && summary->Uncovered() == 4000 &&
              summary->movingMs == 0 && summary->casts == 0 && summary->maxEnemies == 0,
          "no tick: all zero but the length, which is all uncovered");
    Check(summary && summary->groupSize == 1, "no tick: a group size of 0 counts as alone (1)");
    Fight unopened;
    unopened.Tick(100, TickKind::Acted, true, 5, true);
    Check(!unopened.IsOpen() && !unopened.Close(200), "no open: a tick is ignored and a close returns nothing");
}

void TestWrap()
{
    std::uint32_t const start = UINT32_MAX - 999;  // 1000 ms before getMSTime wraps
    Fight fight;
    fight.Open(start, 1);
    fight.Tick(start + 500, TickKind::Idle, true, 1, false);    // still before the wrap
    fight.Tick(start + 1500, TickKind::Acted, false, 1, true);  // 500 ms past the wrap
    std::optional<Summary> const summary = fight.Close(start + 2500);
    Check(summary && summary->lengthMs == 2500 && summary->Of(TickKind::Idle) == 1000 &&
              summary->Of(TickKind::Acted) == 1000 && summary->movingMs == 1000 && summary->Uncovered() == 500 &&
              Adds(*summary),
          "wrap: getMSTime's wrap inside a fight changes no difference");
}

void TestEdges()
{
    Fight fight;
    fight.Open(1000, 2);
    fight.Tick(1200, TickKind::Acted, false, 1, true);
    fight.Open(1500, 5);  // a second enter edge while open
    std::optional<Summary> const first = fight.Close(2000);
    Check(first && first->lengthMs == 1000 && first->groupSize == 2 && first->casts == 1,
          "open while open: ignored, the start and group size stay the first open's");
    Check(!fight.Close(2100), "double close: the repeated leave edge returns nothing");
    fight.Open(3000, 1);
    std::optional<Summary> const second = fight.Close(3300);
    Check(second && second->lengthMs == 300 && second->casts == 0 && second->Covered() == 0,
          "after a close: the next open starts a fresh fight");
}

void TestPacking()
{
    std::uint64_t const amount = PackTimes(12345, 2000, 300, 49);
    Check(UnpackTime(amount, TimeField::Length) == 123 && UnpackTime(amount, TimeField::Idle) == 20 &&
              UnpackTime(amount, TimeField::NoPower) == 3 && UnpackTime(amount, TimeField::Moving) == 0,
          "pack: tenths of a second (rounded down), each field round trips");
    Check((amount & 32767) == 123 && ((amount >> 15) & 32767) == 20 && ((amount >> 30) & 32767) == 3 &&
              ((amount >> 45) & 32767) == 0,
          "pack: the SQL reading (amount >> 15n) & 32767 gives the same fields");
    std::uint64_t const capped = PackTimes(UINT32_MAX, 3276700, 3276800, UINT32_MAX);
    Check(UnpackTime(capped, TimeField::Length) == TIME_MAX && UnpackTime(capped, TimeField::Idle) == 32767 &&
              UnpackTime(capped, TimeField::NoPower) == TIME_MAX && UnpackTime(capped, TimeField::Moving) == TIME_MAX,
          "pack: each field capped at 32767 tenths, none spills into the next");
    Check(capped < (std::uint64_t{1} << 60) && capped <= static_cast<std::uint64_t>(INT64_MAX),
          "pack: 60 bits, a positive int64 amount");
    Check(PackTimes(0, 0, 0, 0) == 0, "pack: zero is zero");
}

void TestClassify()
{
    Check(Classify(true, false, false) == TickKind::Acted && Classify(true, true, true) == TickKind::Acted,
          "classify: an executed action is acted, whatever else");
    Check(Classify(false, true, false) == TickKind::NoPower && Classify(false, true, true) == TickKind::NoPower,
          "classify: nothing executed and a no-power failure is no power");
    Check(Classify(false, false, true) == TickKind::Idle, "classify: nothing executed, the bot could act: idle");
    Check(Classify(false, false, false) == TickKind::Busy, "classify: nothing executed, the bot could not act: busy");
}
}  // namespace

int main()
{
    TestSums();
    TestNoTick();
    TestWrap();
    TestEdges();
    TestPacking();
    TestClassify();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
