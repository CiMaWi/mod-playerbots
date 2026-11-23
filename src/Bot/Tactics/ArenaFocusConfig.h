/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAFOCUSCONFIG_H
#define _PLAYERBOT_ARENAFOCUSCONFIG_H

#include "Define.h"

template <typename Struct>
struct ConfigFamily;

// The numbers of the target hold (Playerbots.Tactics.Hold.*) and the team plan (Playerbots.Tactics.Focus.*), read once
// at config load (PlayerbotAIConfig::TacticFocus); the initial values are the conf.dist defaults (proposals, G6a tunes
// them). Kept apart from ArenaFocus.h so that the logic can change without rebuilding every PlayerbotAIConfig.h user.
// Config table rows: ArenaFocusConfig.cpp (Hold, Focus, FocusTarget families; ".playerbots config Playerbots.Tactics."
// prints them).
// Target hold and team plan: the numbers of the Hold and Plan tactics (all keys optional, the defaults below; read only
// where those tactics are enabled). The defaults are proposals, not tuned yet (G6a tunes them). An ...AtSkill1 /
// ...AtSkill99 pair is lerped over the bot's Wisdom (judgement; the re-mark delay over its Speed) along
// Playerbots.Skill.CurveExponent; with the attribute defaults that is the skill, and skill 0 takes the skill-99 value.
// The personality traits move some values (neutral 50 moves nothing): Patience lengthens both holds (up to x1.25),
// Aggression raises both execute thresholds (up to 10 points: an earlier execute), Caution widens the swap off a
// defensive (up to 15 points), Teamwork raises the chance to follow the partner (up to x1.5), shortens the follow delay
// (down to x0.7) and raises the margin a bot other than the planner needs to re-mark (up to x1.5).
struct FocusConfig
{
    // Hold (one bot's target, every context in
    // Playerbots.Tactics.Hold.Scope): a bot keeps its target for HoldMs, then switches only to a candidate that scores
    // HysteresisPct points better (a point is one percent of health; a switch's reaction cost adds a point per 100 ms).
    // It always switches when the target dies, leaves range, is out of its sight for longer than LosLostMs or turns
    // immune. A candidate at or below ExecPct % health (its target above it, no defensive up) overrides the hold with
    // ExecChance % per 3 s window. Low skill shows tunnel vision (long hold, a hysteresis of 100 that health alone
    // never reaches, no execute): it switches only for death, range and immunity. Without marks, FollowChance % (one
    // answer per 10 s window) is the chance to take the partner's target once the partner has had it for FollowDelayMs:
    // none at low skill, sometimes and late at mid skill.
    uint32 HoldMsAtSkill1 = 20000;
    uint32 HoldMsAtSkill99 = 1500;
    uint32 HoldHysteresisPctAtSkill1 = 100;
    uint32 HoldHysteresisPctAtSkill99 = 8;
    uint32 HoldExecPctAtSkill1 = 15;
    uint32 HoldExecPctAtSkill99 = 25;
    uint32 HoldExecChanceAtSkill1 = 0;
    uint32 HoldExecChanceAtSkill99 = 90;
    uint32 HoldFollowChanceAtSkill1 = 0;
    uint32 HoldFollowChanceAtSkill99 = 80;
    uint32 HoldFollowDelayMsAtSkill1 = 4000;
    uint32 HoldFollowDelayMsAtSkill99 = 1000;
    uint32 HoldLosLostMs = 1500;
    // Focus (the team plan, arenas): the group's target icons
    // carry the plan, skull = kill target, moon = crowd-control target (moon only when the team can control). The
    // planner is the team's alive bot with the higher skill (ties: the lower guid) at or above
    // Playerbots.Tactics.Plan.KnownFromSkill (the Plan tactic's gate key above; 40 is the proposal when the plan is on;
    // there is no Focus.KnownFromSkill); with a real player in the group he marks and no bot plans. The planner
    // re-plans every PlanIntervalMs and keeps the kill target for HoldMs; after the hold another enemy must score
    // HysteresisPct points better (health, healer, range and sight of the team's bots, defensives, trinket, physical
    // immunity for a physical team, stun or fear). It switches at once when the kill target dies, turns immune to
    // everything, falls into the team's breakable crowd control (never a kill target) or is out of every own bot's
    // sight for longer than LosLostMs; an enemy at or below ExecPct % health (no defensive up) overrides the hold with
    // ExecChance % per 3 s window; a kill target with a defensive up is left for an enemy within DefensiveSwapPct
    // points of its health. Marks are set at the start and change only on events (the kill target dies or turns immune,
    // a big opening), at most once per MinRemarkSeconds, RemarkDelayMs after the event (a human delay), never within
    // HumanMarkRespectSeconds of a real player's mark. Any other bot, in a real player's group too, re-marks only when
    // its own judgement beats the marked target by StrongMargin score points, so rarely.
    uint32 FocusPlanIntervalMsAtSkill1 = 2300;
    uint32 FocusPlanIntervalMsAtSkill99 = 300;
    uint32 FocusHoldMsAtSkill1 = 5300;
    uint32 FocusHoldMsAtSkill99 = 2000;
    uint32 FocusHysteresisPctAtSkill1 = 28;
    uint32 FocusHysteresisPctAtSkill99 = 8;
    uint32 FocusExecPctAtSkill1 = 15;
    uint32 FocusExecPctAtSkill99 = 25;
    uint32 FocusExecChanceAtSkill1 = 30;
    uint32 FocusExecChanceAtSkill99 = 90;
    uint32 FocusRemarkDelayMsAtSkill1 = 2500;
    uint32 FocusRemarkDelayMsAtSkill99 = 800;
    uint32 FocusStrongMarginAtSkill1 = 50;
    uint32 FocusStrongMarginAtSkill99 = 30;
    uint32 FocusMinRemarkSeconds = 8;
    uint32 FocusHumanMarkRespectSeconds = 10;
    uint32 FocusLosLostMs = 1500;
    uint32 FocusDefensiveSwapPct = 25;
    // DT.12: The planner's snapshot of an enemy is what its team sees (health, auras, cast bars, sight); from
    // AddonKnowledgeFromSkill (the planner's Knowledge, the skill with the attribute defaults; skill 0 always) it also
    // remembers what an addon would: a defensive seen used this match counts as spent.
    uint32 FocusAddonKnowledgeFromSkill = 40;
    // The moon mark (G1b-c, ArenaFocus.h FOCUS_MOON_*): 1 = G1b's (whenever a class can control), 0 = never, 2 = only
    // for a team that plays the Cc tactic, released MoonGraceMs after it was set or last controlled without a CC
    // landing or cast at it, not set again for MoonRetryMs
    uint32 FocusMoonMode = 1;
    uint32 FocusMoonGraceMs = 5000;
    uint32 FocusMoonRetryMs = 10000;
    // The focus target (G4e, Playerbots.Tactics.FocusTarget.*): the chance the picker takes the plan's moon over the
    // enemy healer, lerped by Teamwork, and the score an interrupt on the focus gains over other casts of its kind
    uint32 FocusTargetMoonPctAtTeamwork1 = 50;
    uint32 FocusTargetMoonPctAtTeamwork99 = 100;
    uint32 FocusTargetKickBonus = 50;
};

extern ConfigFamily<FocusConfig> const HoldFamily;
extern ConfigFamily<FocusConfig> const FocusFamily;
extern ConfigFamily<FocusConfig> const FocusTargetFamily;

// The one instance (ArenaFocusConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.TacticFocus.
FocusConfig& FocusConfigStorage();

#endif
