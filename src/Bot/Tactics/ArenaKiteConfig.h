/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAKITECONFIG_H
#define _PLAYERBOT_ARENAKITECONFIG_H

#include "Common.h"

// The numbers of the Kite tactic (Playerbots.Tactics.Kite.*, G3a), the Drink tactic (Playerbots.Tactics.Drink.*, G3b)
// and the Jump tactic (Playerbots.Tactics.Jump.*, G2g), read once at config load (PlayerbotAIConfig::TacticKite,
// TacticDrink, TacticJump); the initial values are the conf.dist defaults (proposals). Kept apart from the pure
// policy headers (ArenaKite.h, ArenaDrink.h, ArenaJump.h) so that the policy can change without rebuilding every
// PlayerbotAIConfig.h user.
struct KiteConfig
{
    uint32 KnownFromSkill = 40;    // below it the bot runs (flee) or keeps casting
    uint32 EffortMin = 20;         // below it the bot gives up after GiveUpMsAtEffort0
    uint32 MeleeYards = 8;         // "melee on me": an enemy melee within this that targets the bot
    uint32 StepYards = 9;          // one kite step towards the partner (8-10 yd)
    uint32 StepIntervalMs = 1500;  // a kite step at most this often
    uint32 EpisodeMs = 20000;      // a kite episode ends after this
    uint32 GiveUpMsAtEffort0 = 3000;
    uint32 GiveUpMsAtEffort100 = 12000;
    uint32 SnareFirst = 1;         // 1: the class snare before the steps
    uint32 PillarMaxYards = 12;    // a pillar point at most this far
    uint32 PartnerMinYards = 12;   // a step towards a partner at least this far (so that it can peel)
    uint32 JumpMinSavedYards = 6;  // a tested jump spot only when it saves this much way
    uint32 MemoryFromSkill = 60;   // Foresight: reads the opponent memory's LOS breaks
    uint32 MemoryWindowMs = 15000;
    uint32 PanicBelowSkill = 40;  // below it the bot panics (flee) instead of kiting
};

struct DrinkConfig
{
    uint32 KnownFromSkill = 45;
    uint32 EffortMin = 30;         // safe-window drinks need this effort
    uint32 PrepEffortMin = 10;     // preparation drinks need this effort
    uint32 PrepBelowPct = 95;      // preparation: mana below this
    uint32 PrepMinLeftMs = 12000;  // preparation: more than this left until the gates open
    uint32 SafeBelowPct = 60;      // safe window: mana below this
    uint32 SafeBelowPctHealer = 70;
    uint32 EnemyYards = 30;       // no enemy within this
    uint32 QuietMs = 2000;        // no enemy seen for this long
    uint32 PartnerMinHpPct = 50;  // the partner above this
    uint32 BreakEnemyYards = 20;  // an enemy within this stands the bot up
    uint32 NoDrinkLogMs = 60000;  // the "no drink" log line at most this often per bot
};

struct JumpConfig
{
    uint32 KnownFromSkill = 70;
    uint32 ChaseFromAggression = 60;  // aggressive bots jump down to chase
    uint32 EscapeFromCaution = 40;    // cautious bots jump to escape
    uint32 MinSavedYards = 6;
    uint32 LandingTolX10 = 20;  // landing tolerance in tenths of a yard (2.0 yd)
    uint32 TestJumps = 10;      // jumps per spot in "tactics jump test"
    uint32 TestWalkTimeoutMs = 12000;
    uint32 TestAirTimeoutMs = 6000;
    uint32 TestSettleMs = 1500;
    uint32 TestMaxStuckMs = 8000;
};

#endif
