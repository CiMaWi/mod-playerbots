/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALMOVECONFIG_H
#define _PLAYERBOT_NATURALMOVECONFIG_H

#include "Common.h"
#include "Tactics.h"

constexpr uint32 MinTrackRingSamples = 4;   // the bounds of TrackConfig::RingSamples
constexpr uint32 MaxTrackRingSamples = 32;  // (the state array holds the maximum)

// The numbers of the natural chasing and tracking (Playerbots.Natural.Track.*, G2b), read once at config load
// (PlayerbotAIConfig::NaturalTrack); the initial values are the conf.dist defaults (proposals). Kept apart from
// NaturalMove.h so that the policy can change without rebuilding every PlayerbotAIConfig.h user.
struct TrackConfig
{
    bool Enable = false;
    // The contexts it acts in (Arena,Battleground,World); out of PvP only with OutsidePvpInSight
    uint8 Contexts =
        static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground) |
                           TacticContextBit(TacticContext::OpenWorldPvp));
    bool OutsidePvpInSight = false;  // 1: also out of PvP, for bots in a real player's sight
    uint32 InSightYards = 60;
    uint32 InSightCacheMs = 1000;
    // The lag: the chaser sees the target as it was lag ms ago (log-normal, sigma per cent), by skill
    uint32 LagMsAtSkill1 = 500;
    uint32 LagMsAtSkill99 = 110;
    uint32 LagSigmaPct = 25;
    // The predictors: from VelocityFromSkill the velocity is extrapolated, from PatternFromSkill turns and patterns
    uint32 VelocityFromSkill = 35;
    uint32 PatternFromSkill = 65;
    uint32 TurnRateFromSkill = 50;  // from here the turn rate is read as well
    // The share of the true velocity the extrapolation uses (per cent), by skill, and its draw
    uint32 GainPctAtSkill1 = 0;
    uint32 GainPctAtSkill99 = 90;
    uint32 GainSigmaPct = 20;
    // The history a pattern is read from (ms), by skill
    uint32 HistoryMsAtSkill1 = 0;
    uint32 HistoryMsAtSkill99 = 1500;
    uint32 RingSamples = 16;  // samples kept (4-32)
    uint32 SampleMs = 50;     // the least gap between two ring samples
    uint32 MaxLeadYards = 6;  // the predicted point is at most this far from the lagged one
    uint32 SkipLeadRule = 1;  // 1: the old 3-yard lead rule of ReachCombatTo is skipped
    uint32 MinMoveYards = 1;  // a target that moved less than this since the last sample is standing
    uint32 JukeDeg = 60;      // a heading change this large between two samples is a juke
    uint32 StuckMs = 3000;    // a chaser that closed nothing for this long aims at the target's true position
    uint32 RefreshMs = 250;   // 13e-decide: a tracked chase may replace a waited-for move this old (0: the hold)
};

#endif
