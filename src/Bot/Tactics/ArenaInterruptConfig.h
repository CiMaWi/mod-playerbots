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
};

#endif
