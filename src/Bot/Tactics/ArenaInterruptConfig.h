/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAINTERRUPTCONFIG_H
#define _PLAYERBOT_ARENAINTERRUPTCONFIG_H

#include "Common.h"

// The numbers of the Interrupt tactic (Playerbots.Tactics.Interrupt.*, G5b), read once at config load
// (PlayerbotAIConfig::TacticInterrupt); the initial values are the conf.dist defaults (proposals, G6a tunes them).
// Shares are per cent of the enemy's cast bar, measured from its start. Kept apart from ArenaInterrupt.h so that the
// policy can change without rebuilding every PlayerbotAIConfig.h user.
struct InterruptConfig
{
    uint32 HealSharePctAtSkill1 = 30;  // where a judging bot commits its kick on a heal (Wisdom: later when skilled)
    uint32 HealSharePctAtSkill99 = 70;
    uint32 DamageSharePct = 50;    // the same for a damage cast
    uint32 CcSharePct = 0;         // and for a crowd control cast (no waiting: it must land before the cast ends)
    uint32 MaxSharePct = 90;       // no commit later than this
    uint32 SigmaPctAtSkill1 = 40;  // the log-normal noise of the commit share (per cent of sigma)
    uint32 SigmaPctAtSkill99 = 15;
    uint32 UrgentHpPct = 30;       // a heal on a friend at or below this health is kicked as a CC cast is
    uint32 CancelSharePct = 15;    // added at a fake rate of 1 (cancels seen per cast of the caster)
    uint32 CancelMinCasts = 3;     // casts seen of a caster before its fake rate counts
    uint32 PatienceSharePct = 10;  // Patience 100 adds it, Patience 0 takes it off (50: neutral)
    uint32 HoldForHealer = 1;      // 1 = no kick on a damage cast while an enemy healer is in reach
    uint32 HealerCacheMs = 200;    // "enemy healer target": how long its best target is kept
    uint32 ArmsPummel = 0;         // 1 = an Arms warrior swaps into Berserker stance to pummel (G5c)
    // G5c-c: the chance a bot notices an interruptible cast at all (else it lets the cast go), lerped by skill
    uint32 UsePctAtSkill1 = 100;
    uint32 UsePctAtSkill99 = 100;
    // G5c-c: the mean gap after the kick is ready before the bot notices casts (0 and 0: none), by skill, convex
    uint32 AttentionMsAtSkill1 = 0;
    uint32 AttentionMsAtSkill99 = 0;
    // G5c-c: the hold policy. 0 = HoldForHealer above; 1 = ArenaInterrupt::DecideHold: a damage cast is held only
    // while a cast heal is likely and its window reachable, diluted by Knowledge, at most HoldMsAtPatience* per episode
    uint32 HoldPolicy = 0;
    uint32 HoldKnowPctAtKnowledge1 = 40;  // the chance a bot knows an enemy spell (cast time, heal, instant)
    uint32 HoldKnowPctAtKnowledge99 = 100;
    uint32 HoldAssumedCastMs = 2000;     // the cast time a bot assumes for a heal it does not know
    uint32 HoldInstantSeenMs = 8000;     // instant heals and absorbs seen this recently count as cover
    uint32 HoldHpHighPct = 80;           // the need for a cast heal: none at or above this health
    uint32 HoldHpLowPct = 40;            // and full at or below this one
    uint32 HoldRecentCastHealMs = 6000;  // a healer seen starting a cast heal the bot recognises this recently
    uint32 HoldRecentHpShiftPct = 10;    // uses them: HoldHpHighPct this many points higher, instants no cover
    uint32 HoldInstantCoverPct = 80;     // recognised instant heals: HoldHpHighPct this share down to HoldHpLowPct
    uint32 HoldWindowMarginMs = 300;     // the expected kick must land this long before the heal's end
    uint32 HoldMotivationPctAtKnowledge1 = 30;
    uint32 HoldMotivationPctAtKnowledge99 = 100;
    uint32 HoldMsAtPatience0 = 1000;  // the longest hold episode, lerped by the Patience trait
    uint32 HoldMsAtPatience100 = 6000;
    uint32 HoldResetMs = 6000;  // an episode ends this long after its last hold reason
    // the need from the health projected this far ahead by its recent loss (about the kick's cooldown; 0: none)
    uint32 HoldTrendHorizonMs = 10000;
    uint32 HoldTrendPctAtKnowledge1 = 50;  // the share of the horizon a bot projects, lerped by Knowledge
    uint32 HoldTrendPctAtKnowledge99 = 100;
};

#endif
