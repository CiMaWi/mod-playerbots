/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the ghost's wait (src/Ai/World/Pvp/GhostWait.h, WP7): the plan by the enemy's distance and the
// time since the release, rows on a wait's start and end only, the recheck by timer, the wait counted from the
// release, a resurrection ending the wait, and the persistence cache's recheck. Build (Git Bash, module root):
//   clang-cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I src/Ai/World/Pvp tests/ghost-wait-selftest.cpp
#include <cstdint>
#include <cstdio>
#include <string>

#include "GhostWait.h"

using namespace WorldPvp;

namespace
{
constexpr uint32_t SECOND = 1000;
constexpr uint32_t MINUTE = 60 * SECOND;
constexpr uint32_t HOUR = 60 * MINUTE;
// A server clock well past its start, so "never" and "long ago" differ from small values
constexpr uint32_t LOGIN = 10 * HOUR;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void TestPlan()
{
    GhostValues const values;
    GhostPlan wait = PlanGhost(20.0f, MINUTE, values);
    Check(wait.step == GhostStep::Wait && wait.recheckMs == 15 * SECOND, "an enemy at the corpse: wait 15 s");
    wait = PlanGhost(20.0f, 5 * MINUTE - 5 * SECOND, values);
    Check(wait.step == GhostStep::Wait && wait.recheckMs == 5 * SECOND, "the last wait ends at the healer's time");
    Check(PlanGhost(41.0f, 2 * MINUTE, values).step == GhostStep::Corpse, "the enemy gone: to the corpse");
    Check(PlanGhost(FAR_YARDS, 0, values).step == GhostStep::Corpse, "no enemy: to the corpse");
    Check(PlanGhost(20.0f, 5 * MINUTE, values).step == GhostStep::SpiritHealer, "after 5 min the spirit healer");

    WorldPvpConfig config;
    config.GhostRecheckSeconds = 0;
    Check(GhostValues(config).recheckMs == SECOND, "a recheck of 0 s is one second");
}

// Rows on a wait's start and end only, the recheck by timer, the wait counted from the release
void TestTurns()
{
    GhostValues const values;
    uint32_t const t = LOGIN;
    PvpMemory memory;
    Check(GhostCheckDue(memory, t), "a new ghost checks at once");
    GhostTurn turn = GhostChecked(memory, FAR_YARDS, 0, t, values);
    Check(turn.plan.step == GhostStep::Corpse && !turn.row, "no enemy: to the corpse, no row");
    Check(!GhostCheckDue(memory, t + 14 * SECOND) && GhostCheckDue(memory, t + 15 * SECOND),
          "the next check comes after GhostRecheckSeconds");
    Check(GhostHeld(memory, t + SECOND).step == GhostStep::Corpse, "between checks the corpse run holds");

    turn = GhostChecked(memory, 12.4f, 15 * SECOND, t + 15 * SECOND, values);
    Check(turn.plan.step == GhostStep::Wait && turn.row == std::string(GHOST_ENEMY) && turn.amount == 12,
          "an enemy at the corpse: the wait starts with a row (its yards)");
    Check(GhostHeld(memory, t + 20 * SECOND).step == GhostStep::Wait &&
              GhostHeld(memory, t + 20 * SECOND).recheckMs == 10 * SECOND,
          "between checks the wait holds, the recheck left");
    turn = GhostChecked(memory, 12.0f, 30 * SECOND, t + 30 * SECOND, values);
    Check(turn.plan.step == GhostStep::Wait && !turn.row, "a recheck with the enemy still there writes no row");
    turn = GhostChecked(memory, FAR_YARDS, 45 * SECOND, t + 45 * SECOND, values);
    Check(turn.plan.step == GhostStep::Corpse && turn.row == std::string(GHOST_GONE) && turn.amount == 30,
          "the enemy gone: to the corpse, a row with the 30 s waited");

    PvpMemory camped;
    GhostChecked(camped, 10.0f, 0, t, values);
    turn = GhostChecked(camped, 10.0f, 4 * MINUTE + 55 * SECOND, t + 4 * MINUTE + 55 * SECOND, values);
    Check(turn.plan.step == GhostStep::Wait && turn.plan.recheckMs == 5 * SECOND,
          "the recheck is never later than the spirit healer's time");
    turn = GhostChecked(camped, 10.0f, 5 * MINUTE, t + 5 * MINUTE, values);
    Check(turn.plan.step == GhostStep::SpiritHealer && turn.row == std::string(GHOST_HEALER) && turn.amount == 300,
          "5 minutes after the release: the spirit healer, a row with the 300 s waited");
    Check(!GhostCheckDue(camped, t + 30 * MINUTE) && GhostHeld(camped, t + 30 * MINUTE).step == GhostStep::SpiritHealer,
          "on its way to the spirit healer it checks no more");
    GhostRevived(camped);
    Check(GhostCheckDue(camped, t + 31 * MINUTE) && GhostHeld(camped, t + 31 * MINUTE).step == GhostStep::Corpse,
          "a resurrection ends the wait; the next death checks at once");

    // The wait counts from the release (ghostMs, the corpse's ghost time), not from the wait's start
    PvpMemory late;
    turn = GhostChecked(late, 10.0f, 5 * MINUTE + SECOND, t, values);
    Check(turn.plan.step == GhostStep::SpiritHealer && turn.row == std::string(GHOST_HEALER) && turn.amount == 0,
          "an enemy found after 5 minutes as a ghost: the spirit healer at once, 0 s waited");
}

// The persistence cache: checked at once, then again only after PERSISTENT_RECHECK_MS (also across the clock's wrap)
void TestPersistence()
{
    PvpMemory memory;
    Check(PersistenceDue(memory, LOGIN), "an unknown persistence is checked at once");
    memory.persistentKnown = true;
    memory.persistentCheckMs = LOGIN;
    Check(!PersistenceDue(memory, LOGIN + PERSISTENT_RECHECK_MS - 1) &&
              PersistenceDue(memory, LOGIN + PERSISTENT_RECHECK_MS),
          "the persistence is checked again after a minute");
    memory.persistentCheckMs = UINT32_MAX - SECOND;
    Check(!PersistenceDue(memory, SECOND) && PersistenceDue(memory, PERSISTENT_RECHECK_MS),
          "the recheck survives the clock's wrap");
}
}  // namespace

int main()
{
    TestPlan();
    TestTurns();
    TestPersistence();
    std::printf("ghost-wait-selftest: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
