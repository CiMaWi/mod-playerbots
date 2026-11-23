/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENADEFENSIVECONFIG_H
#define _PLAYERBOT_ARENADEFENSIVECONFIG_H

#include "Define.h"

template <typename Struct>
struct ConfigFamily;

// The numbers of the Defensive and Protect tactics (Playerbots.Tactics.Defensive.*, G5d), read once at config load
// (PlayerbotAIConfig::TacticDefensive); the initial values are the conf.dist defaults (proposals). Percents are of
// the unit's maximum health, "AtSkill1/99" values are lerped by the bot's skill. Kept apart from ArenaDefensive.h so
// that the policy can change without rebuilding every PlayerbotAIConfig.h user.
// Defensives on burst and the partner protected (G5d; Playerbots.Tactics.Defensive.Enable, scope All, and
// Playerbots.Tactics.Protect.Enable, scope Arena,Battleground,World). With Defensive on, the strategy "tactic
// defensive" presses the class's self-defensives (mage ice block, paladin divine shield, rogue cloak or evasion, death
// knight icebound fortitude, hunter deterrence, druid barkskin or survival instincts, disc priest pain suppression,
// shadow priest dispersion, protection warrior shield wall; healthstones and potions with PotionEnable) when health is
// at or below HpPctAtSkill1..99 per cent AND (the bot is under burst OR focused). Burst: health lost within
// BurstWindowMs of at least BurstLossPctAtSkill1..99 per cent, from a ring of 16 health samples taken every SampleMs.
// Focused: at least FocusedMinEnemies enemy players whose victim or cast target is the bot (what the combat log and the
// cast bars show; read in Arena, Battleground and open-world PvP only); a focused bot presses FocusedHpBonusPct points
// earlier. A low-skill bot presses only at CriticalHpPct, or at the gates opening with the chance
// OpenerPctAtSkill1..99; low effort waits LateMsAtEffort0..100 ms after the trigger holds; Caution moves the threshold
// by up to CautionShiftPct points; two presses are at least MinGapMs apart; no press while a defensive is up or every
// self-defensive is on cooldown. With Protect on, PartyMemberToProtect returns a partner (PvP scope only: hand of
// protection drops a tank's threat) that an enemy within Protect.DistanceYards attacks now (its victim or cast target),
// whose health is at or below Protect.HpPctAtSkill1..99 per cent and who has no defensive of its own up; the existing
// protect actions (hand of protection, pain suppression, intervene) use it with their own cooldowns. A disc priest
// with Protect on and a living partner keeps pain suppression for the partner: on itself only at CriticalHpPct (the
// Defensive tactic's opener, burst and focused presses skip it). Opponent memory (Foresight; from skill
// Opponent.FromSkill): the bot remembers when each enemy was last seen with a major defensive up (immunities: ice
// block, divine shield, deterrence, dispersion; physical stops: evasion, hand of protection; magic stops: cloak of
// shadows, anti-magic shell). It holds its burst while a defensive that stops its own damage is up (an immunity, or the
// stop of its damage type: melee and hunters physical, casters magic; minor reductions such as barkskin or pain
// suppression never hold), and while the enemy owns such a defensive not seen up within Opponent.ExpireMs (believed
// ready) until the enemy uses it: at most BurstHoldMaxMs from the first such hold since its last use (0: no such hold)
// and never with the enemy at or below BurstHoldFloorPct per cent health. A stop that is up (not an immunity) holds
// the burst under the same cap, counted per episode of the aura, and the same floor (13e-polish; an immunity holds
// while it lasts). "Focused" counts only against an enemy team of at least FocusedMinTeam players at the match's start
// (the arena's size; battlegrounds and the open world always): in 2v2 both enemies on the bot are the normal case, so
// there only burst and the plain line press. Opponent.Enable 0 turns the memory off. Protect.AttackedWithinMs,
// Protect.MinGapMs, Opponent.MinEvents and Opponent.LeadPctAtForesight99 (the lead of the hold before 13e-decide) are
// read but not used.
struct DefensiveConfig
{
    // The burst sampler: health lost within the window, from a ring of samples taken every SampleMs
    uint32 BurstWindowMs = 2000;
    uint32 BurstLossPctAtSkill1 = 55;  // health lost within the window that counts as burst, by skill
    uint32 BurstLossPctAtSkill99 = 30;
    uint32 SampleMs = 250;
    // "Focused": enemy players whose victim or cast target is the bot; only against an enemy team of at least
    // FocusedMinTeam players at the match's start (13e-decide: in 2v2 two enemies are the whole team)
    uint32 FocusedMinEnemies = 2;
    uint32 FocusedMinTeam = 3;
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
    // 13e-decide (reading B): the burst waits for an enemy's major defensive believed ready at most this long (from
    // the first hold since its last use; 0: no such hold), and never below this health
    uint32 BurstHoldMaxMs = 8000;
    uint32 BurstHoldFloorPct = 30;
};

extern ConfigFamily<DefensiveConfig> const DefensiveFamily;

// The one instance (ArenaDefensiveConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls
// this: readers use sPlayerbotAIConfig.TacticDefensive.
DefensiveConfig& DefensiveConfigStorage();

#endif
