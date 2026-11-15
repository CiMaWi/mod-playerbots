/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ARENAFOCUSCHECKS_H
#define PLAYERBOTS_ARENAFOCUSCHECKS_H

#include <algorithm>
#include <array>
#include <format>
#include <string>

#include "ArenaFocus.h"
#include "ArenaFocusTarget.h"
#include "TacticGate.h"

// Pure checks of the target hold and the team plan (ArenaFocus.h) on synthetic enemies; the numbers are the conf.dist
// defaults (FocusConfig{}), never the server's config
template <typename Check>
void RunFocusChecks(Check&& check)
{
    constexpr uint64 Seed = 4242;
    constexpr float Linear = 1.0f;
    constexpr uint32 Now = 100000;
    constexpr uint32 Windows = 200;
    FocusConfig const config{};
    // The attribute defaults: every ability the skill (0 at skill 0), every trait 50
    auto const plain = [](uint8 skill)
    {
        BotAttributeValues values;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = index < BOT_ABILITY_COUNT ? skill : 50;

        return values;
    };
    auto const with = [&](uint8 skill, BotAttribute attribute, uint8 value)
    {
        BotAttributeValues values = plain(skill);
        values.Value[static_cast<uint32>(attribute)] = value;
        return values;
    };
    auto const enemy = [](uint8 hp, bool healer)
    {
        EnemyInfo info;
        info.HpPct = hp;
        info.Healer = healer;
        info.LosCount = 2;
        info.InRangeCount = 2;
        return info;
    };
    auto const within = [](uint32 value, uint32 target, uint32 tolerance)
    { return value + tolerance >= target && value <= target + tolerance; };

    // Parameters: skill 0, the attribute defaults, Wisdom as the lerp position, the research anchors, the traits
    FocusParams const top = ComputeFocusParams(99, plain(99), Linear, config);
    FocusParams const full = ComputeFocusParams(0, plain(0), Linear, config);
    check(full.Hold == top.Hold && full.Team == top.Team, "skill 0 does not hold and plan like skill 99");

    FocusParams const mid = ComputeFocusParams(60, plain(60), Linear, config);
    float const at60 = SkillPosition(60);
    auto const lerp60 = [&](uint32 atSkill1, uint32 atSkill99)
    { return SkillLerpRounded(atSkill1, atSkill99, at60, Linear); };
    check(mid.Hold.HoldMs == lerp60(config.HoldMsAtSkill1, config.HoldMsAtSkill99) &&
              mid.Hold.HysteresisPct == lerp60(config.HoldHysteresisPctAtSkill1, config.HoldHysteresisPctAtSkill99) &&
              mid.Hold.ExecPct == lerp60(config.HoldExecPctAtSkill1, config.HoldExecPctAtSkill99) &&
              mid.Hold.FollowChance == lerp60(config.HoldFollowChanceAtSkill1, config.HoldFollowChanceAtSkill99) &&
              mid.Hold.FollowDelayMs == lerp60(config.HoldFollowDelayMsAtSkill1, config.HoldFollowDelayMsAtSkill99) &&
              mid.Team.HoldMs == lerp60(config.FocusHoldMsAtSkill1, config.FocusHoldMsAtSkill99) &&
              mid.Team.ExecPct == lerp60(config.FocusExecPctAtSkill1, config.FocusExecPctAtSkill99) &&
              mid.Team.StrongMargin == lerp60(config.FocusStrongMarginAtSkill1, config.FocusStrongMarginAtSkill99) &&
              mid.Team.RemarkDelayMs == lerp60(config.FocusRemarkDelayMsAtSkill1, config.FocusRemarkDelayMsAtSkill99) &&
              mid.Team.DefensiveSwapPct == config.FocusDefensiveSwapPct,
          "the attribute defaults do not give the plain skill lerp");

    FocusParams const wise = ComputeFocusParams(60, with(60, BotAttribute::Wisdom, 90), Linear, config);
    FocusParams const ninety = ComputeFocusParams(90, plain(90), Linear, config);
    check(wise.Team.HoldMs == ninety.Team.HoldMs && wise.Team.PlanIntervalMs == ninety.Team.PlanIntervalMs &&
              wise.Team.HysteresisPct == ninety.Team.HysteresisPct && wise.Hold.HoldMs == ninety.Hold.HoldMs &&
              wise.Team.RemarkDelayMs == mid.Team.RemarkDelayMs,
          "a skill-60 bot with Wisdom 90 does not hold and plan like a skill-90 bot");

    FocusParams const forty = ComputeFocusParams(40, plain(40), Linear, config);
    check(within(forty.Team.PlanIntervalMs, 1500, 30) && within(forty.Team.HoldMs, 4000, 30) &&
              within(forty.Team.HysteresisPct, 20, 1) && top.Team.PlanIntervalMs == 300 && top.Team.HoldMs == 2000 &&
              top.Team.HysteresisPct == 8,
          "the plan misses the research anchors (skill 40: 1500 ms, 4 s, 20; skill 99: 300 ms, 2 s, 8)");

    FocusParams const aggressive = ComputeFocusParams(60, with(60, BotAttribute::Aggression, 100), Linear, config);
    FocusParams const cautious = ComputeFocusParams(60, with(60, BotAttribute::Caution, 100), Linear, config);
    FocusParams const patient = ComputeFocusParams(60, with(60, BotAttribute::Patience, 100), Linear, config);
    FocusParams const social = ComputeFocusParams(60, with(60, BotAttribute::Teamwork, 100), Linear, config);
    check(aggressive.Team.ExecPct > mid.Team.ExecPct && aggressive.Hold.ExecPct > mid.Hold.ExecPct,
          "Aggression does not raise the execute thresholds");
    check(cautious.Team.DefensiveSwapPct > mid.Team.DefensiveSwapPct,
          "Caution does not widen the swap off a defensive");
    check(patient.Team.HoldMs > mid.Team.HoldMs && patient.Hold.HoldMs > mid.Hold.HoldMs,
          "Patience does not lengthen the holds");
    check(social.Hold.FollowChance > mid.Hold.FollowChance && social.Hold.FollowDelayMs < mid.Hold.FollowDelayMs &&
              social.Team.StrongMargin > mid.Team.StrongMargin,
          "Teamwork does not raise following and the margin a non-planner needs");

    // The plan of a skill-99 planner whose team can control
    TeamParams team = top.Team;
    team.CcCapable = true;
    TeamParams sure = team;
    sure.ExecChance = 100;
    TeamParams never = team;
    never.ExecChance = 0;
    uint32 const hold = team.HoldMs;

    std::array<EnemyInfo, 2> const even{enemy(60, false), enemy(60, false)};
    PlanState const start = Decide(even.data(), 2, PlanState{}, 1000, team, Seed);
    check(start.Kill == 0 && start.Cc == 1 && start.Reason == PlanReason::Start && start.HeldUntilMs == 1000 + hold,
          "equal enemies: the first plan is not enemy 0 held for HoldMs, enemy 1 controlled");
    PlanState onSecond = start;
    onSecond.Kill = 1;
    onSecond.Cc = 0;
    PlanState const kept = Decide(even.data(), 2, onSecond, 1000 + hold - 1, team, Seed);
    PlanState const after = Decide(even.data(), 2, onSecond, 1000 + hold, team, Seed);
    check(
        kept.Kill == 1 && kept.Reason == PlanReason::Held && after.Kill == 1 && after.Reason == PlanReason::Hysteresis,
        "equal enemies do not keep the kill target until the hold ends (and after it)");
    std::array<EnemyInfo, 2> const drop{enemy(60, false), enemy(40, false)};
    PlanState const dropHeld = Decide(drop.data(), 2, start, 1000 + hold - 1, team, Seed);
    PlanState const dropFree = Decide(drop.data(), 2, start, 1000 + hold, team, Seed);
    check(dropHeld.Kill == 0 && dropFree.Kill == 1 && dropFree.Reason == PlanReason::Score &&
              dropFree.HeldUntilMs == 1000 + 2 * hold,
          "a better enemy takes the kill before the hold ends, or not when it ends");

    std::array<EnemyInfo, 2> bubble = even;
    bubble[0].ImmuneAll = true;
    PlanState const swapped = Decide(bubble.data(), 2, start, 1001, team, Seed);
    check(swapped.Kill == 1 && swapped.Reason == PlanReason::Immune && swapped.Cc == FOCUS_NONE &&
              swapped.HeldUntilMs == 1001 + hold,
          "an immune kill target is not swapped in the same call (the immune one is no crowd-control target)");
    check(Decide(even.data(), 2, swapped, 1001 + hold, team, Seed).Kill == 1,
          "an immune target that comes back takes the kill back on an equal score");
    std::array<EnemyInfo, 2> wall = even;
    wall[0].HpPct = 80;
    wall[1].DefensiveUp = true;
    PlanState const back = Decide(wall.data(), 2, swapped, 1002, team, Seed);
    check(back.Kill == 0 && back.Reason == PlanReason::Defensive,
          "an immune target that comes back does not take the kill through the defensive rule");
    wall[0].HpPct = 90;
    TeamParams careful = team;
    careful.DefensiveSwapPct = cautious.Team.DefensiveSwapPct;
    check(Decide(wall.data(), 2, swapped, 1002, team, Seed).Kill == 1 &&
              Decide(wall.data(), 2, swapped, 1002, careful, Seed).Kill == 0,
          "a cautious planner does not swap off a defensive sooner");

    std::array<EnemyInfo, 2> sheep{enemy(20, true), enemy(90, false)};
    sheep[0].CcBreakable = true;
    PlanState const sheepStart = Decide(sheep.data(), 2, PlanState{}, 1000, team, Seed);
    PlanState const sheepHeld = Decide(sheep.data(), 2, start, 1001, team, Seed);
    check(
        sheepStart.Kill == 1 && sheepStart.Cc == 0 && sheepHeld.Kill == 1 && sheepHeld.Reason == PlanReason::Controlled,
        "an enemy in the team's breakable crowd control is the kill target");
    check(Decide(sheep.data(), 2, onSecond, 1001, sure, Seed).Kill == 1,
          "an enemy in the team's breakable crowd control is taken as an execute");

    std::array<EnemyInfo, 2> low{enemy(60, false), enemy(10, false)};
    PlanState const executed = Decide(low.data(), 2, start, 1001, sure, Seed);
    check(executed.Kill == 1 && executed.Reason == PlanReason::Execute &&
              Decide(low.data(), 2, start, 1001, never, Seed).Reason == PlanReason::Held,
          "an execute does not override the hold at chance 100, or does at chance 0");
    low[1].DefensiveUp = true;
    check(Decide(low.data(), 2, start, 1001, sure, Seed).Kill == 0,
          "an execute is taken on an enemy with a defensive up");
    std::array<EnemyInfo, 2> flip{enemy(60, false), enemy(57, false)};
    check(Decide(flip.data(), 2, start, 1000 + hold, team, Seed).Reason == PlanReason::Hysteresis,
          "hysteresis does not stop a 3-point flip");
    flip[1].HpPct = static_cast<uint8>(60 - team.HysteresisPct);
    check(Decide(flip.data(), 2, start, 1000 + hold, team, Seed).Reason == PlanReason::Score,
          "a lead of the hysteresis does not switch after the hold");
    check(Decide(flip.data(), 2, start, 1000 + hold, team, Seed, 500).Reason == PlanReason::Hysteresis,
          "the switch cost does not raise the hysteresis");

    std::array<EnemyInfo, 2> pillar = even;
    pillar[0].LosCount = 0;
    pillar[0].LosLostMs = 1000;
    PlanState const glimpse = Decide(pillar.data(), 2, start, 1001, team, Seed);
    pillar[0].LosLostMs = 2000;
    PlanState const gone = Decide(pillar.data(), 2, start, 1001, team, Seed);
    check(glimpse.Kill == 0 && gone.Kill == 1 && gone.Reason == PlanReason::Lost,
          "line of sight lost for 1.0 s does not keep the target, or 2.0 s does not swap it");
    pillar[1].LosCount = 0;
    pillar[1].LosLostMs = 2000;
    check(Decide(pillar.data(), 2, start, 1001, team, Seed).Kill == 0,
          "a lost kill target is left for an enemy that is lost too");

    std::array<EnemyInfo, 2> order{enemy(60, false), enemy(70, true)};
    check(Decide(order.data(), 2, PlanState{}, 1000, team, Seed).Kill == 1,
          "a healer at 70 % does not outscore a damage dealer at 60 %");
    order[1].DefensiveUp = true;
    order[0].ImmunePhys = true;
    TeamParams physical = team;
    physical.PhysicalTeam = true;
    check(Decide(order.data(), 2, PlanState{}, 1000, team, Seed).Kill == 0 &&
              Decide(order.data(), 2, PlanState{}, 1000, physical, Seed).Kill == 1,
          "a defensive up or a physical immunity against a physical team does not lower the score");

    std::array<EnemyInfo, 3> three{enemy(30, false), enemy(80, true), enemy(80, false)};
    three[1].DrLevel = 2;
    PlanState const trio = Decide(three.data(), 3, PlanState{}, 1000, team, Seed);
    three[1].Healer = false;
    three[2].DrLevel = 1;
    PlanState const trioDr = Decide(three.data(), 3, PlanState{}, 1000, team, Seed);
    TeamParams noCc = team;
    noCc.CcCapable = false;
    check(trio.Kill == 0 && trio.Cc == 1 && trioDr.Kill == 0 && trioDr.Cc == 2,
          "3v3: the crowd-control target is not the healer first, then the lowest diminishing returns");
    check(Decide(three.data(), 3, PlanState{}, 1000, noCc, Seed).Cc == FOCUS_NONE,
          "a team that cannot control gets a crowd-control target");

    // The same input twice gives the same plan; a 50 % execute is taken in some windows, not in all
    TeamParams coin = team;
    coin.ExecChance = 50;
    std::array<EnemyInfo, 2> const coinPair{enemy(60, false), enemy(10, false)};
    bool same = true;
    uint32 executes = 0;
    for (uint32 window = 0; window < Windows; ++window)
    {
        uint32 const now = 1001 + window * FOCUS_EXECUTE_WINDOW_MS;
        PlanState held = start;
        held.HeldUntilMs = now + hold;
        PlanState const first = Decide(coinPair.data(), 2, held, now, coin, Seed);
        PlanState const second = Decide(coinPair.data(), 2, held, now, coin, Seed);
        same = same && first == second;
        executes += first.Reason == PlanReason::Execute ? 1 : 0;
    }
    check(same, "the same input twice gives two different plans");
    check(executes > 0 && executes < Windows, "a 50 % execute chance is taken in every window or in none");

    // One bot's hold: tunnel vision at skill 1, quick switches for a reason at skill 99
    HoldParams const low1 = ComputeFocusParams(1, plain(1), Linear, config).Hold;
    HoldInput swing;
    swing.HasTarget = true;
    swing.TargetHpPct = 70;
    swing.CandidateHpPct = 40;
    swing.Advantage = 30;
    HoldResult const swingLate = HoldDecision(swing, Now, low1, Seed);
    check(
        !swingLate.Allow && swingLate.Reason == HoldReason::Hysteresis && !HoldDecision(swing, 1000, low1, Seed).Allow,
        "a skill-1 hold does not keep its target through a 30-point health swing");
    HoldInput lowExecute = swing;
    lowExecute.CandidateHpPct = 5;
    lowExecute.Advantage = 65;
    check(!HoldDecision(lowExecute, Now, low1, Seed).Allow, "a skill-1 hold leaves its target for an execute");
    HoldInput dead = swing;
    dead.TargetAlive = false;
    HoldInput away = swing;
    away.TargetInRange = false;
    HoldInput immune = swing;
    immune.TargetImmune = true;
    HoldInput hidden = swing;
    hidden.TargetLosLostMs = 2000;
    HoldInput glance = swing;
    glance.TargetLosLostMs = 1000;
    check(HoldDecision(dead, 1000, low1, Seed).Reason == HoldReason::Dead &&
              HoldDecision(away, 1000, low1, Seed).Reason == HoldReason::OutOfRange &&
              HoldDecision(immune, 1000, low1, Seed).Reason == HoldReason::Immune &&
              HoldDecision(hidden, 1000, low1, Seed).Reason == HoldReason::LosLost &&
              !HoldDecision(glance, 1000, low1, Seed).Allow,
          "a skill-1 hold does not leave a dead, far, immune or lost (2.0 s; not 1.0 s) target");
    HoldParams const high = top.Hold;
    check(HoldDecision(swing, 1000, high, Seed).Reason == HoldReason::HoldActive &&
              HoldDecision(swing, high.HoldMs, high, Seed).Reason == HoldReason::Free,
          "a skill-99 hold does not switch for a reason after its hold");
    HoldParams sureHold = high;
    sureHold.ExecChance = 100;
    HoldInput highExecute = swing;
    highExecute.CandidateHpPct = 10;
    check(HoldDecision(highExecute, 100, sureHold, Seed).Reason == HoldReason::Execute &&
              HoldDecision(HoldInput{}, 100, high, Seed).Reason == HoldReason::NoTarget,
          "an execute does not override a bot's hold, or a bot without a target is held");

    // Following the partner's target without marks: never at skill 1, late, sometimes at mid skill
    HoldParams alwaysFollow = high;
    alwaysFollow.FollowChance = 100;
    check(!FollowPartnerDecision(0, Now, low1, Seed), "a skill-1 bot follows its partner's target");
    check(!FollowPartnerDecision(Now - alwaysFollow.FollowDelayMs + 1, Now, alwaysFollow, Seed) &&
              FollowPartnerDecision(Now - alwaysFollow.FollowDelayMs, Now, alwaysFollow, Seed),
          "following is not late by FollowDelayMs");
    uint32 follows = 0;
    for (uint32 window = 0; window < Windows; ++window)
        follows += FollowPartnerDecision(0, Now + window * FOCUS_FOLLOW_WINDOW_MS, mid.Hold, Seed) ? 1 : 0;

    check(follows > 0 && follows < Windows, "a skill-60 bot follows its partner always or never");

    // Re-marks: only on events, at most one per MinRemarkSeconds, after the human delay; a non-planner only past the
    // strong margin and never within HumanMarkRespectSeconds of a human's mark
    RemarkInput planner;
    planner.Planner = true;
    planner.Marked = true;
    planner.LastMarkMs = Now - team.MinRemarkMs;
    planner.EventAtMs = Now - team.RemarkDelayMs;
    check(RemarkDecision(planner, Now, team).Reason == RemarkReason::NoEvent, "a re-mark comes without an event");
    planner.Event = RemarkEvent::KillDead;
    RemarkInput soon = planner;
    soon.LastMarkMs = Now - team.MinRemarkMs + 1;
    RemarkInput early = planner;
    early.EventAtMs = Now - team.RemarkDelayMs + 1;
    RemarkInput first;
    first.Planner = true;
    first.Event = RemarkEvent::Start;
    first.EventAtMs = Now - team.RemarkDelayMs;
    check(RemarkDecision(planner, Now, team).Allow && RemarkDecision(first, Now, team).Allow,
          "the planner may not mark at the start or re-mark on an event");
    check(RemarkDecision(soon, Now, team).Reason == RemarkReason::TooSoon &&
              RemarkDecision(early, Now, team).Reason == RemarkReason::Delay,
          "a re-mark comes within MinRemarkSeconds or before the human delay");
    RemarkInput follower = planner;
    follower.Planner = false;
    follower.Margin = team.StrongMargin - 1;
    RemarkInput strong = follower;
    strong.Margin = team.StrongMargin;
    check(RemarkDecision(follower, Now, team).Reason == RemarkReason::Margin && RemarkDecision(strong, Now, team).Allow,
          "a non-planner re-marks below the strong margin, or not at it");
    RemarkInput human = strong;
    human.Margin = 1000;
    human.HumanMarked = true;
    human.LastHumanMarkMs = Now - team.HumanMarkRespectMs + 1;
    RemarkInput humanLater = human;
    humanLater.LastHumanMarkMs = Now - team.HumanMarkRespectMs;
    check(RemarkDecision(human, Now, team).Reason == RemarkReason::HumanMark &&
              RemarkDecision(humanLater, Now, team).Allow,
          "a re-mark comes within HumanMarkRespectSeconds of a human's mark, or never after it");

    // Who plans: the alive bot with the higher skill at or above the threshold, ties to the lower guid; none with a
    // real player in the team
    std::array<PlannerCandidate, 2> bots{PlannerCandidate{2001, 50, true, false},
                                         PlannerCandidate{2002, 70, true, false}};
    check(ElectPlanner(bots.data(), 2, 40) == 1, "the higher skill is not the planner");
    bots[1].Skill = 50;
    check(ElectPlanner(bots.data(), 2, 40) == 0, "a skill tie does not go to the lower guid");
    bots[0].Alive = false;
    check(ElectPlanner(bots.data(), 2, 40) == 1, "a dead bot plans");
    bots[0].Alive = true;
    bots[0].Skill = 30;
    bots[1].Skill = 30;
    check(ElectPlanner(bots.data(), 2, 40) == FOCUS_NONE && ElectPlanner(bots.data(), 2, 0) == 0,
          "the planner threshold is not applied (40) or not open (0)");
    bots[1].Skill = 0;
    check(ElectPlanner(bots.data(), 2, 40) == 1, "skill 0 (full ability) does not plan");
    bots[0].RealPlayer = true;
    check(ElectPlanner(bots.data(), 2, 0) == FOCUS_NONE, "a bot plans in a real player's team");

    // G1a review NOTE 1: the score rule never takes an enemy no own bot has seen for longer than LosLostMs
    std::array<EnemyInfo, 2> unseen{enemy(60, false), enemy(20, false)};
    unseen[1].LosCount = 0;
    unseen[1].LosLostMs = team.LosLostMs + 500;
    check(Decide(unseen.data(), 2, start, 1000 + hold, team, Seed).Kill == 0,
          "the score rule takes an enemy out of every own bot's sight");

    // NOTE 3: the plan's reasons as remark events; a switch by score alone never marks
    std::array<RemarkEvent, static_cast<size_t>(PlanReason::Count)> const events = {
        RemarkEvent::None,       RemarkEvent::None,    RemarkEvent::Start,   RemarkEvent::KillDead,
        RemarkEvent::KillImmune, RemarkEvent::Opening, RemarkEvent::Opening, RemarkEvent::Opening,
        RemarkEvent::Opening,    RemarkEvent::None,    RemarkEvent::None,    RemarkEvent::None};
    bool mapped = true;
    for (size_t index = 0; index < events.size(); ++index)
        mapped = mapped && EventOfPlanReason(static_cast<PlanReason>(index)) == events[index];

    check(mapped, "a plan reason maps to the wrong remark event (score must map to none)");

    // NOTE 2: addon-level knowledge from AddonKnowledgeFromSkill (Knowledge), always at skill 0
    check(FocusAddonKnowledge(0, 0, 40) && FocusAddonKnowledge(50, 40, 40) && !FocusAddonKnowledge(50, 39, 40),
          "addon-level knowledge does not start at the threshold, or skill 0 lacks it");

    // G1b-c: the moon by mode (0 never, 1 a class that can control, 2 a team that also plays CC), kept while its CC
    // lands, and the skull's hold off a moon nobody controls only in mode 2
    check(!FocusMoonCapable(FOCUS_MOON_OFF, true, true) && FocusMoonCapable(FOCUS_MOON_ALWAYS, true, false) &&
              !FocusMoonCapable(FOCUS_MOON_ALWAYS, false, true) && FocusMoonCapable(FOCUS_MOON_USED, true, true) &&
              !FocusMoonCapable(FOCUS_MOON_USED, true, false),
          "the moon by mode is not: never, a class that can control, a team that plays crowd control");
    check(FocusMoonKept(Now, Now + 5000, 5000) && !FocusMoonKept(Now, Now + 5001, 5000),
          "a moon without a CC landing is not released after the grace, or is released before");
    check(FocusMarkHolds(FOCUS_MOON_ALWAYS, true, false) && FocusMarkHolds(FOCUS_MOON_USED, false, false) &&
              FocusMarkHolds(FOCUS_MOON_USED, true, true) && !FocusMarkHolds(FOCUS_MOON_USED, true, false),
          "the skull's hold keeps a bot off a moon nobody controls in mode 2, or lets it go in mode 1");

    // G4e: the focus target (ArenaFocusTarget.h)
    ArenaFocusTargetTest::RunFocusTargetSelftest(check);
}

#endif
