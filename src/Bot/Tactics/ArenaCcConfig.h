/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACCCONFIG_H
#define _PLAYERBOT_ARENACCCONFIG_H

#include "Common.h"

// The numbers of crowd control (Playerbots.Tactics.Cc.*) and its avoidance (Playerbots.Tactics.Avoid.*), read once at
// config load (PlayerbotAIConfig::TacticCc); the initial values are the conf.dist defaults (proposals, G6a tunes them).
// Kept apart from ArenaCc.h so that the policy can change without rebuilding every PlayerbotAIConfig.h user.
struct CcConfig
{
    // Cc: the bot's own crowd control (the Cc tactic)
    uint32 CcDrAwareFromSkill = 40;   // Knowledge: from it the bot remembers the CC it saw land (addon-level, DT.12)
    uint32 CcChainsFromSkill = 65;    // Knowledge: from it the bot times its CC to the end of the one on the target
    uint32 CcMinExpectedMs = 3000;    // a DR-aware bot casts no CC that would last less (the third of a group)
    uint32 CcDrMissPctAtSkill1 = 40;  // a CC seen landing that the memory misses (Knowledge)
    uint32 CcDrMissPctAtSkill99 = 0;
    uint32 CcChainLeadMsAtSkill1 = 0;  // how long before the current CC's end the next one lands (Foresight)
    uint32 CcChainLeadMsAtSkill99 = 300;
    uint32 CcWrongTargetPctAtSkill1 = 50;  // the CC goes on whoever is near instead of the CC target (Wisdom)
    uint32 CcWrongTargetPctAtSkill99 = 0;
    uint32 CcPatientHoldMs = 3000;  // a patient bot (Patience 100) holds its CC this long for the kill window
    uint32 CcBehindStopPct = 30;    // a low-effort bot stops controlling this many health points behind
    // G4c: 1 = a chaining bot casts on the team's CC, timed to its end (0: no CC on a controlled target, as G4b)
    uint32 CcChainEnable = 0;
    // G4c-b: 1 = chains time by the enemy healer's dispel speed seen this match (the opponent memory)
    uint32 CcOpponentMemoryEnable = 0;
    uint32 CcOpponentMinEvents = 2;  // dispels (or trinkets) of an enemy seen before the memory is used
    uint32 CcDispelMarginMs = 300;   // the next CC lands this long before the remembered dispel
    // Avoid: the bot's own CC kept (OwnCc) and the enemy's broken (Trinket)
    uint32 AvoidOwnBreakPctAtSkill1 = 60;  // the bot's damage breaks its team's CC (Focus)
    uint32 AvoidOwnBreakPctAtSkill99 = 5;
    uint32 AvoidOwnBreakNeverFromSkill = 65;  // Focus: from it a bot never breaks its team's CC (G4b)
    uint32 AvoidWastedBreakPctAtSkill1 = 70;  // the bot breaks a CC that does not matter (Wisdom)
    uint32 AvoidWastedBreakPctAtSkill99 = 0;
    uint32 AvoidIgnoreRootsFromSkill = 65;       // Wisdom: from it a bot never breaks a root, snare or silence
    uint32 AvoidWastedBreakNeverFromSkill = 65;  // G5a, Wisdom: from it a bot breaks only a CC that matters
    uint32 AvoidMinLeftMs = 2500;                // a CC matters with this much or more left
    uint32 AvoidLowHealthPct = 40;               // the bot or its partner at or below it is low
    // G5a: Caution 100 adds this to LowHealthPct for the bot's own breaks, Caution 0 takes it off (cautious bots break
    // earlier; 0 = every bot at LowHealthPct)
    uint32 AvoidCautiousLowHealthPct = 0;
    // G5a, harness matches only: 1 = a bot without a PvP trinket wears one for the match (Human and Undead keep their
    // racial), 2 = no bot uses its PvP trinket (the fallback cell); 0 = as geared. .playerbots tactics ensuretrinket
    uint32 AvoidEnsureTrinket = 0;
    // G4c-b own-break guards of OwnCc (1 = on; 0 = G4b-b's behaviour)
    uint32 AvoidAoeGuardEnable = 0;       // no area, cone or chain spell whose reach covers a held enemy
    uint32 AvoidAoeGuardMarginYards = 2;  // added to the spell's radius (the enemy's movement and size)
    uint32 AvoidDotGuardEnable = 0;       // no damage-breakable CC on a target the team's periodic damage hits
    uint32 AvoidPetCallOffEnable = 0;     // the bot's pet stops attacking a held enemy
    uint32 AvoidExcludeFightEnemies = 0;  // the held enemies of "fight enemies" are kept out too (review NOTE 7)
};

#endif
