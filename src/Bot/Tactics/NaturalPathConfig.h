/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALPATHCONFIG_H
#define _PLAYERBOT_NATURALPATHCONFIG_H

#include "Common.h"
#include "Tactics.h"

// The numbers of the natural paths (Playerbots.Natural.Path.*, G2c), read once at config load
// (PlayerbotAIConfig::NaturalPath); the initial values are the conf.dist defaults (proposals). Kept apart from
// NaturalPath.h so that the policy can change without rebuilding every PlayerbotAIConfig.h user. Enable 0 (and each
// flag below) keeps today's straight mmap paths.
struct PathConfig
{
    bool Enable = false;
    // The contexts it acts in (Arena,Battleground,World); out of PvP only with OutsidePvpInSight
    uint8 Contexts =
        static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground) |
                           TacticContextBit(TacticContext::OpenWorldPvp));
    bool OutsidePvpInSight = false;  // 1: also out of PvP, for bots in a real player's sight
    uint32 InSightYards = 60;
    uint32 InSightCacheMs = 1000;
    // Lateral offsets of the interior waypoints (with Enable): the amplitude in yards by skill (larger at low skill)
    bool OffsetsEnable = true;
    float OffsetYardsAtSkill1 = 1.5f;
    float OffsetYardsAtSkill99 = 0.4f;
    uint32 OffsetSigmaPct = 30;  // the spread of the draw (per cent of the amplitude)
    uint32 MinLegYards = 12;     // a path shorter than this is walked straight
    // Each behind its own default-off flag
    bool CurvesEnable = false;  // catmullrom curves on open ground
    bool StrafeEnable = false;  // strafe splines for ranged bots (SetFacing), never while casting
    bool HopsEnable = false;    // cosmetic hops on flat validated ground while running or kiting
    bool WalkEnable = false;    // walking on short legs out of combat
    uint32 StrafeFromSkill = 50;
    uint32 StrafeLegMinYards = 2;
    uint32 StrafeLegMaxYards = 4;
    uint32 StrafeEveryMsAtSkill50 = 4000;
    uint32 StrafeEveryMsAtSkill99 = 1500;
    uint32 HopsFromSkill = 60;
    uint32 HopsEveryMsAtSkill60 = 12000;
    uint32 HopsEveryMsAtSkill99 = 5000;
    uint32 HopsEdgeYards = 8;  // never within this of an edge
    uint32 HopsMaxLegYards = 6;
    uint32 WalkMaxLegYards = 6;
};

#endif
