/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALCASTCONFIG_H
#define _PLAYERBOT_NATURALCASTCONFIG_H

#include "Common.h"

// The numbers of the Fake tactic (Playerbots.Tactics.Fake.*, G5e), read once at config load
// (PlayerbotAIConfig::TacticFake); the initial values are the conf.dist defaults (proposals). Kept apart from
// NaturalCast.h so that the policy can change without rebuilding every PlayerbotAIConfig.h user.
struct FakeConfig
{
    uint32 KnownFromSkill = 70;  // a bot below this skill never fakes
    uint32 MinCastMs = 1500;     // a bait is a cast at least this long
    // Where in the bar the cancel falls (per cent of the cast time), by Patience; the spread is a log-normal draw
    uint32 CancelPctAtPatience0 = 35;
    uint32 CancelPctAtPatience100 = 75;
    uint32 CancelSigmaPct = 15;
    uint32 CancelMinMs = 300;      // never earlier than this into the cast
    uint32 CancelMinLeftMs = 250;  // never within this of the cast's completion
    // The gap before the next fake, by Patience
    uint32 RepeatMsAtPatience0 = 4000;
    uint32 RepeatMsAtPatience100 = 10000;
    uint32 RepeatJitterPct = 30;
    uint32 MaxPerMinute = 6;
    // A fake needs a kicker the bot sees: in range (its kick range plus slack), facing or targeting the bot
    uint32 KickerRangeYards = 30;
    uint32 KickerRangeSlackYards = 3;
    uint32 KickerNeedFacing = 1;
    uint32 KickSeenMs = 8000;         // a kick seen used this recently makes a fake pointless
    uint32 KickMemoryFromSkill = 40;  // from this skill bots remember kicks seen used
    // The chance a fake is taken when a kicker is seen, by skill (70 to 99)
    uint32 ChancePctAtSkill70 = 40;
    uint32 ChancePctAtSkill99 = 90;
    uint32 BaitedWindowMs = 300;  // an enemy kick within this after the cancel counts as baited
};

#endif
