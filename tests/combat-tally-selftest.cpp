/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the fight tally, second version (combat-foundation CFM2; src/Bot/Engine/CombatTally.h, MEASURES
// §2): a case for each share of the sort rule, the invariant that the five shares sum to the fight's length exactly in
// tenths, a fight without a tick (all idle, marked), the split at the bot's update every 50 minutes (CF.32), the
// 0.1 s rule (CF.30), getMSTime's wrap, the power gate's window, the packed rows' round trip with the mark (CF.31).
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
    twice.Tick(1200, Share::Idle, 1, false, full);
    Check(!twice.IsOpen() && !twice.Close(1300), "a tick without a fight does nothing");
}

void TestWritten()
{
    TickReading full;
    Check(!Written(0) && !Written(99) && Written(100) && Written(101),
          "0 and 99 ms are not written, 100 and 101 ms are");

    // A second fight opened and closed in the millisecond another closed
    Fight fight;
    std::uint32_t written = 0;
    fight.Open(1000, 1, full, false);
    if (std::optional<Summary> const a = fight.Close(6000); a && Written(a->lengthMs))
        ++written;
    fight.Open(6000, 1, full, false);
    if (std::optional<Summary> const b = fight.Close(6000); b && Written(b->lengthMs))
        ++written;
    Check(written == 1, "a fight opened and closed in the millisecond another closed leaves one written pair");

    // Without a tick: marked and all idle; one tick clears the mark
    Fight quiet;
    quiet.Open(200, 1, full, false);
    std::optional<Summary> const q = quiet.Close(3300);
    Check(q && !q->ticked && q->Of(Share::Idle) == 3100 && q->casts == 0 && AddsUp(*q),
          "a fight closed without a tick carries the mark and is all idle");
    Fight once;
    once.Open(200, 1, full, false);
    once.Tick(900, Share::Acting, 1, true, full);
    std::optional<Summary> const t = once.Close(3300);
    Check(t && t->ticked && t->Of(Share::Acting) == 3100 && AddsUp(*t), "one tick clears the mark");
}

void TestSplit()
{
    TickReading pull;
    pull.healthPct = 90;
    pull.powerPct = 10;
    TickReading later;
    later.healthPct = 55;
    later.powerPct = 70;

    // 101 minutes without a tick, an update every 1.5 s: two running parts, each all idle and marked
    Fight quiet;
    quiet.Open(100, 1, pull, false);
    std::uint32_t now = 100;
    std::uint32_t parts = 0;
    bool partsIdle = true;
    std::optional<Summary> first;
    while (now < 100 + 101 * 60 * 1000)
    {
        now += 1500;
        if (std::optional<Summary> const part = quiet.Split(now, later))
        {
            ++parts;
            partsIdle = partsIdle && !part->ticked && part->Of(Share::Idle) == part->lengthMs && AddsUp(*part) &&
                        part->lengthMs >= SPLIT_MS && part->lengthMs < SPLIT_MS + 1500 &&
                        Tenths(part->lengthMs) < TIME_MAX;
            if (!first)
                first = part;
        }
    }
    std::optional<Summary> const rest = quiet.Close(now + 500);
    Check(parts == 2, "a fight of 101 minutes without a tick writes two running parts at its updates");
    Check(partsIdle, "each part is 50 minutes, all idle, marked, below the cap, and its shares sum to its length");
    Check(first && first->pullHealthPct == 90 && first->pullPowerPct == 10, "the first part keeps the pull");
    Check(rest && !rest->ticked && rest->pullHealthPct == 55 && rest->pullPowerPct == 70 && AddsUp(*rest) &&
              rest->lengthMs < SPLIT_MS,
          "the sums start again at the split, the update's reading as the next part's pull");

    // Ticks for 10 minutes, then none: the part at the update at 50 minutes, the 40 minutes in the last tick's share
    Fight stalled;
    stalled.Open(0, 1, pull, false);
    now = 0;
    std::optional<Summary> written;
    std::uint32_t lastTickMs = 0;
    while (now < SPLIT_MS + 60000 && !written)
    {
        now += 1500;
        if (now <= 10 * 60 * 1000)
        {
            stalled.Tick(now, now <= 9 * 60 * 1000 ? Share::Acting : Share::Waiting, 1, false, later);
            lastTickMs = now;
        }
        written = stalled.Split(now, later);
    }
    Check(written && written->ticked && now >= SPLIT_MS && now < SPLIT_MS + 1500 && AddsUp(*written),
          "a fight that stops ticking writes its part at the update at 50 minutes");
    // the first waiting tick is the one after 9 minutes; the stretch before it belongs to the last acting tick
    Check(written && written->Of(Share::Waiting) == now - (9 * 60 * 1000 + 1500) &&
              now - lastTickMs >= 40 * 60 * 1000 - 1500,
          "the 40 minutes without a tick are in the last tick's share");
    std::optional<Summary> const next = stalled.Close(now + 20000);
    Check(next && !next->ticked && next->Of(Share::Idle) == 20000 && AddsUp(*next), "its next part is marked");

    // A tick at 50 minutes writes nothing by itself; Split before 50 minutes returns nothing
    Fight edge;
    edge.Open(1000, 1, pull, false);
    Check(!edge.Split(1000 + SPLIT_MS - 1, later) && !edge.SplitDue(1000 + SPLIT_MS - 1),
          "Split before 50 minutes returns nothing");
    edge.Tick(1000 + SPLIT_MS, Share::Acting, 1, true, later);
    std::optional<Summary> const due = edge.Split(1000 + SPLIT_MS, later);
    Check(due && due->lengthMs == SPLIT_MS && due->casts == 1 && due->ticked,
          "a tick at 50 minutes writes no part by itself (the update's Split does)");

    // A part that closes under 0.1 s after a split is not written
    std::optional<Summary> const tail = edge.Close(1000 + SPLIT_MS + 99);
    Check(tail && tail->lengthMs == 99 && !Written(tail->lengthMs),
          "a part closed under 0.1 s after a split is not written");
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
    std::uint32_t const marked = MoreId(80, 11, 2, 3, 12, 100, true);
    Check((marked & ~(1u << UNTICKED_SHIFT)) == moreId && (marked >> UNTICKED_SHIFT) == 1 && (moreId >> 30) == 0,
          "MoreId sets bit 30 for the mark and leaves the fields below it unchanged");
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
    TestWritten();
    TestSplit();
    TestGate();
    TestPacking();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
