/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALCASTCONFIG_H
#define _PLAYERBOT_NATURALCASTCONFIG_H

#include "Define.h"

template <typename Struct>
struct ConfigFamily;

// The numbers of the Fake tactic (Playerbots.Tactics.Fake.*, G5e), read once at config load
// (PlayerbotAIConfig::TacticFake); the initial values are the conf.dist defaults (proposals). Kept apart from
// NaturalCast.h so that the policy can change without rebuilding every PlayerbotAIConfig.h user.
// Fake casts (G5e; Playerbots.Tactics.Fake.Enable, scope Arena,Battleground,World). With it on, a bot from
// KnownFromSkill (Knowledge) that starts a kickable cast of at least MinCastMs sees whether an enemy player of its
// fight could kick it (its class has a kick aimed at one enemy; it is within that kick's reach (melee 5, Wind Shear
// 25, Counterspell and Silence 30, Silencing Shot 35 yards) capped at KickerRangeYards, plus KickerRangeSlackYards;
// and it faces the bot or targets it when KickerNeedFacing is 1) and has seen no kick used within KickSeenMs (bots
// from KickMemoryFromSkill remember the kicks they saw used within 40 yards); it then fakes with the chance
// ChancePctAtSkill70..99 per cent and cancels the cast at a drawn point of the bar (CancelPctAtPatience0..100 per cent
// of the cast time, log-normal spread CancelSigmaPct; never earlier than CancelMinMs into the cast and never within
// CancelMinLeftMs of its end), on the bot's first update at that instant, if the kicker still threatens it (else the
// cast finishes). The next fake is at least RepeatMsAtPatience0..100 ms later (jitter RepeatJitterPct per cent), at
// most MaxPerMinute a minute. A kick at the bot within BaitedWindowMs after the cancel counts as baited (counters
// fakes, kicksBaited, kickSeen). Nothing tells a kicker it faces a fake; its own memory of seen cancels is all it has
// (Playerbots.Tactics.Interrupt). HealGuardPct (13e-decide): a heal is never faked while its target is below this
// health, nor, by the group's only healer, while any friend is below it. Only fight casts are faked: in combat, and no
// conjure, created item, pet summon, enchant or trade skill (13e-polish).
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
    // 13e-decide: no fake of a heal whose target is below this health, or (the only healer) while any friend is
    uint32 HealGuardPct = 70;
};

extern ConfigFamily<FakeConfig> const FakeFamily;

// The one instance (NaturalCastConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.TacticFake.
FakeConfig& FakeConfigStorage();

#endif
