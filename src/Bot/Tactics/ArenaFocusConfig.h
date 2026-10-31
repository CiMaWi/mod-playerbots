/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAFOCUSCONFIG_H
#define _PLAYERBOT_ARENAFOCUSCONFIG_H

#include "Common.h"

// The numbers of the target hold (Playerbots.Tactics.Hold.*) and the team plan (Playerbots.Tactics.Focus.*), read once
// at config load (PlayerbotAIConfig::TacticFocus); the initial values are the conf.dist defaults (proposals, G6a tunes
// them). Kept apart from ArenaFocus.h so that the logic can change without rebuilding every PlayerbotAIConfig.h user.
struct FocusConfig
{
    // Hold: one bot's target (all PvP contexts)
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
    // Focus: the arena team's kill and crowd-control marks
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
    // The planner's Knowledge from which its snapshot carries addon-level knowledge (DT.12: defensives seen spent)
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

#endif
