/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the fight tally, second version (combat-foundation CFM2; src/Bot/Engine/CombatTally.h, MEASURES
// §2): a case for each share of the sort rule, the invariant that the five shares sum to the fight's length exactly in
// tenths, a fight without a tick (all idle), a fight split at 50 minutes, getMSTime's wrap, the power gate's window,
// the packed rows' round trip.
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

std::uint64_t SumTenths(Summary const& s)
{
    std::uint64_t sum = 0;
    for (std::uint64_t const t : ShareTenths(s))
        sum += t;
    return sum;
}

bool AddsUp(Summary const& s)
{
    std::uint32_t ms = 0;
    for (std::uint32_t const part : s.shareMs)
        ms += part;
    return ms == s.lengthMs && SumTenths(s) == Tenths(s.lengthMs);
}

TickFacts InReach()
{
    TickFacts f;
    f.hasTarget = true;
    f.targetInReach = true;
    return f;
}

void TestShares()
{
    // A rage user without rage: nothing ran, the target in reach, a gate unmet (or a cast failed for lack of power)
    TickFacts rage = InReach();
    rage.gateUnmet = true;
    Check(Classify(rage) == Share::Waiting, "a rage user without rage is waiting (gate)");
    TickFacts noPower = InReach();
    noPower.noPowerFailed = true;
    Check(Classify(noPower) == Share::Waiting, "a cast failed for lack of power is waiting");
    TickFacts other = rage;
    other.otherFailed = true;
    Check(Classify(other) == Share::Idle, "a gate unmet but another candidate failed otherwise is idle");

    // A caster out of range: no target in reach, a move that leaves the target out of reach, a sight failure
    TickFacts far;
    far.hasTarget = true;
    Check(Classify(far) == Share::Reach, "a caster out of range is out of reach");
    TickFacts none;
    Check(Classify(none) == Share::Reach, "no target is out of reach");
    TickFacts moving = far;
    moving.executed = true;
    moving.executedMove = true;
    Check(Classify(moving) == Share::Reach, "a move toward a target out of reach is out of reach");
    TickFacts step = InReach();
    step.executed = true;
    step.executedMove = true;
    Check(Classify(step) == Share::Acting, "a move with the target in reach is acting");
    TickFacts sight = InReach();
    sight.sightFailed = true;
    sight.gateUnmet = true;
    Check(Classify(sight) == Share::Reach, "a cast test failed for sight is out of reach before waiting");

    // A stunned bot is controlled, also when an action ran; silenced only when nothing ran
    TickFacts stunned = InReach();
    stunned.lostControl = true;
    stunned.executed = true;
    Check(Classify(stunned) == Share::Controlled, "a stunned bot is controlled");
    TickFacts dead;
    dead.dead = true;
    Check(Classify(dead) == Share::Controlled, "a dead bot is controlled");
    TickFacts silenced = InReach();
    silenced.silenced = true;
    Check(Classify(silenced) == Share::Controlled, "a silenced bot that did nothing is controlled");
    silenced.executed = true;
    Check(Classify(silenced) == Share::Acting, "a silenced bot that hit is acting");

    // A bot that could pay and did nothing is idle
    Check(Classify(InReach()) == Share::Idle, "a bot that could pay and did nothing is idle");

    // A bot in a cast is acting; the global cooldown and an action that ran too
    TickFacts casting;
    casting.casting = true;
    Check(Classify(casting) == Share::Acting, "a bot in a cast is acting");
    TickFacts gcd;
    gcd.globalCooldown = true;
    gcd.gateUnmet = true;
    Check(Classify(gcd) == Share::Acting, "the global cooldown is acting");
    TickFacts cast;
    cast.executed = true;
    Check(Classify(cast) == Share::Acting, "a cast that ran is acting");

    // The one distance: only where the rule needs it
    Check(!NeedsReach(cast) && !NeedsReach(casting) && !NeedsReach(stunned) && !NeedsReach(gcd),
          "no distance on a tick that cast, casts, is controlled or on the global cooldown");
    Check(NeedsReach(TickFacts()) && NeedsReach(moving), "a distance on an empty tick and a move");
}

void TestFight()
{
    TickReading full;
    full.healthPct = 100;
    full.powerPct = 40;

    // Ticks of every share; the time before the first tick belongs to the first tick's share
    Fight fight;
    fight.Open(1000, 1, full, false);
    fight.Tick(1450, Share::Waiting, 1, false, full);  // 1000-1450 waiting, then 1450-2000 waiting
    TickReading hurt = full;
    hurt.healthPct = 37;
    hurt.atCap = true;
    fight.Tick(2000, Share::Acting, 2, true, hurt);       // 2000-2730 acting, at the cap
    fight.Tick(2730, Share::Reach, 1, false, full);       // 2730-3333
    fight.Tick(3333, Share::Controlled, 1, false, full);  // 3333-4001
    fight.Tick(4001, Share::Idle, 3, false, full);        // 4001-5099
    std::optional<Summary> const s = fight.Close(5099);
    Check(s && s->lengthMs == 4099, "the fight's length");
    Check(s && s->Of(Share::Waiting) == 1000 && s->Of(Share::Acting) == 730 && s->Of(Share::Reach) == 603 &&
              s->Of(Share::Controlled) == 668 && s->Of(Share::Idle) == 1098,
          "each stretch belongs to the earlier tick's share, the first to the first tick's");
    Check(s && AddsUp(*s), "the five shares sum to the length, in ms and exactly in tenths");
    Check(s && s->capMs == 730 && s->lowestHealthPct == 37 && s->maxEnemies == 3 && s->casts == 1,
          "cap time, lowest health, most enemies and casts");
    Check(s && ShareTenths(*s)[static_cast<std::size_t>(Share::Acting)] == 40 - 10 - 6 - 6 - 10,
          "acting holds the rounding rest");

    // The invariant over many odd stretches
    Fight odd;
    odd.Open(7, 1, full, true);
    std::uint32_t now = 7;
    for (std::uint32_t i = 0; i < 997; ++i)
    {
        now += 37 + (i * 7919) % 613;
        odd.Tick(now, static_cast<Share>(i % SHARE_COUNT), 1, false, full);
    }
    std::optional<Summary> const o = odd.Close(now + 99);
    Check(o && AddsUp(*o), "the invariant holds over 997 odd stretches");

    // A fight without a tick is all idle
    Fight quiet;
    quiet.Open(500, 2, full, false);
    std::optional<Summary> const q = quiet.Close(5600);
    Check(q && q->Of(Share::Idle) == 5100 && AddsUp(*q) && q->casts == 0 && q->groupSize == 2,
          "a fight without a tick is all idle");

    // getMSTime wraps
    Fight wrap;
    wrap.Open(0xFFFFFF00u, 1, full, false);
    wrap.Tick(0x00000100u, Share::Acting, 1, false, full);
    std::optional<Summary> const w = wrap.Close(0x00000300u);
    Check(w && w->lengthMs == 0x400 && w->Of(Share::Acting) == 0x400 && AddsUp(*w), "the wrap of getMSTime");

    // A repeated close and an open while open
    Fight twice;
    twice.Open(0, 1, full, false);
    twice.Open(400, 5, full, false);
    std::optional<Summary> const first = twice.Close(1000);
    Check(first && first->lengthMs == 1000 && first->groupSize == 1, "an open while open is ignored");
    Check(!twice.Close(1100) && !twice.IsOpen(), "a second close returns nothing");
    Check(!twice.Tick(1200, Share::Idle, 1, false, full), "a tick without a fight does nothing");
}

void TestSplit()
{
    TickReading pull;
    pull.healthPct = 90;
    pull.powerPct = 10;
    TickReading later;
    later.healthPct = 55;
    later.powerPct = 70;

    Fight fight;
    fight.Open(100, 1, pull, false);
    std::uint32_t now = 100;
    std::optional<Summary> running;
    std::uint32_t parts = 0;
    while (now < 100 + 2 * SPLIT_MS + 60000)
    {
        now += 1500;
        if (std::optional<Summary> part = fight.Tick(now, Share::Acting, 1, true, later))
        {
            if (!running)
                running = part;
            ++parts;
        }
    }
    std::optional<Summary> const rest = fight.Close(now + 500);
    Check(parts == 2, "a fight of 101 minutes writes two running parts");
    Check(running && running->lengthMs >= SPLIT_MS && running->lengthMs < SPLIT_MS + 1500 && AddsUp(*running) &&
              Tenths(running->lengthMs) < TIME_MAX,
          "a running part is 50 minutes, below the cap, and adds up");
    Check(running && running->pullHealthPct == 90 && running->pullPowerPct == 10, "the first part keeps the pull");
    Check(rest && rest->pullHealthPct == 55 && rest->pullPowerPct == 70 && AddsUp(*rest) && rest->lengthMs < SPLIT_MS,
          "the sums start again at the split, the split tick's reading as the new pull");
}

void TestGate()
{
    PowerGate gate;
    Check(!gate.Unmet(0, 0), "no gate noted: no waiting");
    gate.NewTick();
    gate.Note(40, 1000, 3);
    gate.Note(25, 1001);
    gate.Note(60, 1002);
    Check(gate.Unmet(1500, 24) && !gate.Unmet(1500, 25), "the lowest amount of a tick wins");
    Check(gate.Row() == NO_ROW, "the winning note's row");
    Check(!gate.Unmet(1001 + GATE_WINDOW_MS + 1, 0), "a gate older than its window is no evidence");
    gate.NewTick();
    gate.Note(60, 4000, 7);
    Check(gate.NotedThisTick() && gate.Unmet(4000, 59) && gate.Row() == 7, "a new tick replaces the note");
    gate.NewTick();
    Check(!gate.NotedThisTick() && gate.Unmet(4100, 59), "the note outlives its tick inside the window");
}

void TestPacking()
{
    Summary s;
    s.lengthMs = 123456;
    s.shareMs = {10099, 20050, 30001, 4099, 123456 - 10099 - 20050 - 30001 - 4099};
    s.capMs = 7000;
    s.pullHealthPct = 100;
    s.pullPowerPct = 33;
    std::uint64_t const fight = FightAmount(s);
    std::uint64_t const more = MoreAmount(s);
    Check(UnpackTime(fight, 0) == 1234 && UnpackTime(fight, 1) == 100 && UnpackTime(fight, 2) == 200 &&
              UnpackTime(fight, 3) == 300,
          "Fight amount: length, idle, waiting, out of reach");
    Check(UnpackTime(more, 0) == 40 && UnpackTime(more, 1) == 1234 - 100 - 200 - 300 - 40 &&
              UnpackTime(more, 2) == 70 && ((more >> 45) & PERCENT_MASK) == 100 && ((more >> 52) & PERCENT_MASK) == 33,
          "FightMore amount: controlled, acting, cap, health and power at the pull");
    std::uint32_t const id = FightId((5u) | (3u << 10) | (2u << 20));
    Check((id >> VERSION_SHIFT) == 1 && (id & 1023) == 5 && ((id >> 10) & 1023) == 3 && ((id >> 20) & 1023) == 2,
          "Fight id carries version 1 above the counts");
    std::uint32_t const moreId = MoreId(80, 11, 2, 3, 12, 100);
    Check((moreId & 127) == 80 && ((moreId >> 7) & 15) == 11 && ((moreId >> 11) & 3) == 2 &&
              ((moreId >> 13) & 7) == 3 && ((moreId >> 16) & 127) == 12 && ((moreId >> 23) & 127) == 100,
          "FightMore id: level, class, tab, power kind, lowest health, final power");
    Check(PowerPoints(PowerKind::Rage, 455, 1000) == 45 && PowerPoints(PowerKind::Mana, 500, 2000) == 25 &&
              PowerPoints(PowerKind::Energy, 70, 100) == 70,
          "power points as the gates compare them");
    Check(AtCap(PowerKind::Energy, 100, 100) && !AtCap(PowerKind::Mana, 100, 100) && !AtCap(PowerKind::Rage, 999, 1000),
          "the cap counts for energy, not for mana");
}
}  // namespace

int main()
{
    TestShares();
    TestFight();
    TestSplit();
    TestGate();
    TestPacking();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
