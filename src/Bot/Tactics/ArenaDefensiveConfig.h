/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENADEFENSIVECONFIG_H
#define _PLAYERBOT_ARENADEFENSIVECONFIG_H

#include "Common.h"

// The numbers of the Defensive and Protect tactics (Playerbots.Tactics.Defensive.*, G5d), read once at config load
// (PlayerbotAIConfig::TacticDefensive); the initial values are the conf.dist defaults (proposals). Percents are of
// the unit's maximum health, "AtSkill1/99" values are lerped by the bot's skill. Kept apart from ArenaDefensive.h so
// that the policy can change without rebuilding every PlayerbotAIConfig.h user.
struct DefensiveConfig
{
    // The burst sampler: health lost within the window, from a ring of samples taken every SampleMs
    uint32 BurstWindowMs = 2000;
    uint32 BurstLossPctAtSkill1 = 55;  // health lost within the window that counts as burst, by skill
    uint32 BurstLossPctAtSkill99 = 30;
    uint32 SampleMs = 250;
    // "Focused": enemy players whose victim or cast target is the bot
    uint32 FocusedMinEnemies = 2;
    // The self-defensive trigger: health at or below HpPct AND (burst OR focused); focused adds FocusedHpBonusPct
    uint32 HpPctAtSkill1 = 25;
    uint32 HpPctAtSkill99 = 60;
    uint32 FocusedHpBonusPct = 15;
    uint32 CriticalHpPct = 20;  // a low-skill bot presses only at this health (or at the opener)
    // Low skill spends its cooldowns at the gates opening (per cent chance), high skill never
    uint32 OpenerPctAtSkill1 = 40;
    uint32 OpenerPctAtSkill99 = 0;
    // Effort: low effort waits this long after the trigger holds before it presses, high effort at once
    uint32 LateMsAtEffort0 = 1200;
    uint32 LateMsAtEffort100 = 0;
    uint32 CautionShiftPct = 10;  // Caution 100 presses this many points of health earlier, Caution 0 later
    uint32 MinGapMs = 1500;       // no second self-defensive within this of the last one
    uint32 PotionEnable = 1;      // healthstones and potions where the core allows them
    uint32 PotionHpPct = 35;
    // The partner protected (PartyMemberToProtect, PvP scope only)
    uint32 ProtectHpPctAtSkill1 = 20;
    uint32 ProtectHpPctAtSkill99 = 40;
    uint32 ProtectAttackedWithinMs = 3000;  // the partner must have been attacked this recently
    uint32 ProtectDistanceYards = 30;       // an attacker this close to the partner counts
    uint32 ProtectMinGapMs = 2000;
    // The opponent memory: the health at which each enemy used its defensives (Foresight)
    uint32 OpponentEnable = 1;
    uint32 OpponentLeadPctAtForesight99 = 20;  // burst timed this many points above the remembered health
    uint32 OpponentMinEvents = 1;
    uint32 OpponentFromSkill = 40;  // addon-level knowledge only from about this skill (DT.12)
    uint32 OpponentExpireMs = 120000;
};

#endif
