/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACOVERCONFIG_H
#define _PLAYERBOT_ARENACOVERCONFIG_H

#include "Define.h"

template <typename Struct>
struct ConfigFamily;

// The numbers of the Los tactic (Playerbots.Tactics.Los.*, G2e) and the Pillar tactic (Playerbots.Tactics.Pillar.*,
// G2f), read once at config load (PlayerbotAIConfig::TacticLos, TacticPillar); the initial values are the conf.dist
// defaults (proposals). Kept apart from ArenaCover.h so that the policy can change without rebuilding every
// PlayerbotAIConfig.h user.
// Line of sight and pillar play (G2e, G2f; Playerbots.Tactics.Los.Enable, scope Arena,Battleground,World, and
// Playerbots.Tactics.Pillar.Enable, scope Arena). Los: a ranged bot that loses sight of its target, or a healer of an
// alive partner, moves to a point that sees it again instead of running into melee or not healing (cover points in
// arenas), known from KnownFromSkill; the lower the skill the later (LateMsAtSkill1..99 ms) and the more often into the
// open (IntoOpenPctAtSkill1..99); from HidesFromCasterFromSkill the point also hides the bot from the enemy caster; a
// healer below EffortMin stays put. A point is held at least HoldMs, at most MaxRelocs relocations per RelocWindowMs
// (else counter relocGuards); a cover search runs at most every SearchIntervalMs with at most MaxRaysPerSearch rays;
// the cover plan takes at most RaysPerSecond rays per bot and second; a regain not finished in TimeoutMs is a timeout.
// Pillar: a bot that an enemy casts at (a cast of HardCastMs or more, or crowd control) that no interrupt can stop
// steps behind a pillar (one hop of at most StepYards) and a melee chaser is pulled round it, from KnownFromSkill;
// below it the bot hides at random (RandomHidePctAtSkill1..59) or never; the cautious style hides CautiousEarlyMs
// earlier; Ring of Valor: no move in the first RvIdleMs (the elevators). The notice delay of these moves is the
// Positioning part of Playerbots.Natural.Reaction.*. Counters losRegains, losTimeouts, partnerLosLostMs, losBreaks,
// castsLosBroken, pillarSteps, relocGuards. Default: off.
struct LosConfig
{
    uint32 KnownFromSkill = 25;    // below it the bot never regains LOS on purpose
    uint32 LateMsAtSkill1 = 2500;  // the mean wait before the bot notices the lost LOS and moves, by skill
    uint32 LateMsAtSkill99 = 500;
    uint32 IntoOpenPctAtSkill1 = 25;  // the chance a regain walks into the open (the enemy sees it), by skill
    uint32 IntoOpenPctAtSkill99 = 0;
    uint32 HidesFromCasterFromSkill = 70;  // from here the point also hides the bot from the enemy caster
    uint32 EffortMin = 25;                 // a healer below this effort stays put
    uint32 HoldMs = 2500;                  // a point is held at least this long
    uint32 MaxRelocs = 3;                  // at most this many relocations ...
    uint32 RelocWindowMs = 10000;          // ... per window
    uint32 SearchIntervalMs = 1500;        // a cover search at most this often per bot
    uint32 MaxRaysPerSearch = 40;
    uint32 RaysPerSecond = 5;  // the cover plan's refresh budget per bot
    uint32 TimeoutMs = 6000;   // a regain not finished by then is a timeout
};

struct PillarConfig
{
    uint32 KnownFromSkill = 60;  // below it pillar play is random or never
    uint32 HardCastMs = 1500;    // a cast of at least this long (or a CC cast) at the bot is hard
    uint32 StepYards = 6;        // one pillar hop of at most this
    uint32 RandomHidePctAtSkill1 = 30;
    uint32 RandomHidePctAtSkill59 = 5;
    uint32 CautiousEarlyMs = 300;  // the cautious style hides this much earlier
    uint32 RvIdleMs = 21000;       // Ring of Valor: no move while the elevators carry the teams
};

extern ConfigFamily<LosConfig> const LosFamily;
extern ConfigFamily<PillarConfig> const PillarFamily;

#endif
