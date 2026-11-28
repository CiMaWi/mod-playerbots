/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the scheduler's rules (src/Bot/Engine/BotEvents.h): which events wake a bot when, the sleep
// after a check, the health bands a group hears of, the wait for energy and the chain of passes. Exit code 1 on a
// failed check.
#include <iostream>
#include <string>

#include "BotEvents.h"

namespace
{
int failed = 0;
int checks = 0;

void Check(bool ok, std::string const& what)
{
    ++checks;
    if (ok)
        return;
    ++failed;
    std::cout << "FAIL: " << what << "\n";
}
}  // namespace

int main()
{
    using namespace BotEvents;

    // No event, no wake
    Check(!WakeFor(0, 200).any, "no event wakes nothing");

    // A surprise waits one reaction time; with a foreseen event among them the bot looks at once
    Wake const hit = WakeFor(Damage, 200);
    Check(hit.any && !hit.urgent && hit.inMs == 200, "damage: one reaction time, no urgency");
    Check(WakeFor(Damage | AuraOn | EnemyCast | AllyHurt, 250).inMs == 250, "surprises together: one reaction time");
    Check(WakeFor(Damage | CastEnd, 200).inMs == 0, "a surprise with the cast's end: at once");
    for (std::uint32_t const foreseen : {AuraOwn, AuraOff, AuraGone, Kill, Hit, CastEnd, MoveEnd, VictimGone, Packet})
        Check(WakeFor(foreseen, 300).inMs == 0 && !WakeFor(foreseen, 300).urgent,
              std::string("foreseen, at once and not urgent: ") + Name(foreseen));
    // Where a model times the reaction the wake is at once
    Check(WakeFor(EnemyCast, 0).inMs == 0, "a surprise with a model: at once");

    // The combat edges and the death end a wait an action asked for
    for (std::uint32_t const urgent : {CombatBegin, CombatEnd, Died})
        Check(WakeFor(urgent, 300).urgent && WakeFor(urgent, 300).inMs == 0,
              std::string("urgent, at once: ") + Name(urgent));
    Check(WakeFor(Damage | CombatBegin, 300).urgent, "the first hit comes with the combat edge: urgent");

    // The wake's delay: not before the action's wait is over, not later than the check already due
    Check(WakeDelayMs(2000, 0, 0) == 0, "a regular wait of 2 s, a foreseen event: now");
    Check(WakeDelayMs(2000, 0, 200) == 200, "a regular wait of 2 s, a surprise: after the reaction time");
    Check(WakeDelayMs(2000, 800, 200) == 800, "a wait of 800 ms an action asked for stays");
    Check(WakeDelayMs(2000, 2000, 0) == 2000, "a meal is not cut by an event that is not urgent");
    Check(WakeDelayMs(100, 0, 200) == 100, "a check due in 100 ms is not pushed back by a surprise");
    Check(WakeDelayMs(0, 0, 200) == 0, "a check that is due stays due");

    // The sleep: the first timer, or the fallback
    bool byTimer = false;
    Check(SleepMs(1000, 400, byTimer) == 400 && byTimer, "a global cooldown's end in 400 ms before a 1 s fallback");
    Check(SleepMs(1000, 1000, byTimer) == 1000 && byTimer, "a timer at the fallback: the timer (no stagger)");
    Check(SleepMs(1000, 5000, byTimer) == 1000 && !byTimer, "a cooldown in 5 s: the fallback");
    Check(SleepMs(1000, UINT32_MAX, byTimer) == 1000 && !byTimer, "no timer: the fallback");
    Check(SleepMs(1000, 0, byTimer) == TIMER_MIN_MS && byTimer, "a timer that is due: the shortest sleep, no spin");
    Check(FALLBACK_COMBAT_MS == 1000, "the combat fallback is 1 s");

    // Health bands of 10 %: a group hears of a member's hit when it takes the health through an edge (full health
    // is its own band, so the first hit is heard)
    Check(CrossesHealthBand(1000, 50, 1000), "the first hit on full health: an edge");
    Check(!CrossesHealthBand(990, 50, 1000), "99 % to 94 %: the same band");
    Check(CrossesHealthBand(1000, 150, 1000), "100 % to 85 %: an edge");
    Check(CrossesHealthBand(910, 20, 1000), "91 % to 89 %: an edge");
    Check(!CrossesHealthBand(890, 20, 1000), "89 % to 87 %: the same band");
    Check(CrossesHealthBand(100, 100, 1000), "a killing blow");
    Check(CrossesHealthBand(100, 5000, 1000), "more damage than health");
    Check(!CrossesHealthBand(0, 100, 1000) && !CrossesHealthBand(500, 0, 1000) && !CrossesHealthBand(500, 10, 0),
          "no health, no damage, no maximum: nothing");
    int edges = 0;
    for (std::uint32_t health = 1000; health > 10; health -= 10)
        edges += CrossesHealthBand(health, 10, 1000) ? 1 : 0;
    Check(edges == 10, "a slow drain from full to 1 % crosses ten edges: full health and nine bands");

    // Energy: 10 a second; the wait is the missing energy's time plus a margin
    Check(PowerWaitMs(45, 45, 10.0f) == 0 && PowerWaitMs(60, 45, 10.0f) == 0, "the energy is there: no wait");
    Check(PowerWaitMs(35, 45, 10.0f) == 1000 + TIMER_MIN_MS, "10 energy short: 1 s and the margin");
    Check(PowerWaitMs(35, 45, 20.0f) == 500 + TIMER_MIN_MS, "at twice the rate: half");
    Check(PowerWaitMs(35, 45, 0.0f) == 0, "no regeneration: no timer");

    // The chain: a fresh action that took no time goes on, up to the last pass
    Check(ChainGoesOn(1, true, false), "a fresh step that took no time: on");
    Check(!ChainGoesOn(1, false, false), "the same action again (or none): the check ends");
    Check(!ChainGoesOn(1, true, true), "a step that took time: the check ends");
    Check(ChainGoesOn(CHAIN_PASSES - 1, true, false) && !ChainGoesOn(CHAIN_PASSES, true, false),
          "the last pass ends the check");

    // Names, for the watch's idle line
    Check(Names(0) == "none", "no events: none");
    Check(Names(Damage | AuraOn) == "damage+aura", "damage+aura");
    Check(Names(CombatBegin | Packet) == "combat+packet", "combat+packet");
    for (std::uint32_t bit = 1; bit < KindEnd; bit <<= 1)
        Check(std::string(Name(bit)) != "?", "every kind has a name");
    Check(std::string(CauseName(Cause::Event)) == "event" && std::string(CauseName(Cause::Fallback)) == "regular" &&
              std::string(CauseName(Cause::Hold)) == "wait",
          "cause names");
    // Every kind is urgent, a surprise or foreseen, and no kind is both urgent and a surprise
    Check((URGENT & SURPRISE) == 0, "urgent and surprise are apart");

    std::cout << "bot-events selftest: " << (checks - failed) << "/" << checks << " checks passed\n";
    return failed ? 1 : 0;
}
