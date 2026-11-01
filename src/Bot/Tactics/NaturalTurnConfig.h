/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALTURNCONFIG_H
#define _PLAYERBOT_NATURALTURNCONFIG_H

#include "Common.h"
#include "Tactics.h"

// The numbers of the natural facing (Playerbots.Natural.Facing.*, G2a), read once at config load
// (PlayerbotAIConfig::NaturalFacing); the initial values are the conf.dist defaults (proposals). Angles in degrees,
// durations in ms. Kept apart from NaturalTurn.h so that the policy can change without rebuilding every
// PlayerbotAIConfig.h user.
struct TurnConfig
{
    bool Enable = false;
    // The contexts it acts in (Arena,Battleground,World); out of PvP only with OutsidePvpInSight
    uint8 Contexts =
        static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground) |
                           TacticContextBit(TacticContext::OpenWorldPvp));
    bool OutsidePvpInSight = false;  // 1: also out of PvP, for bots in a real player's sight
    uint32 InSightYards = 60;        // that sight
    uint32 InSightCacheMs = 1000;    // how long the sight answer is kept
    uint32 Mode = 1;                 // 1 = orientation each step and heartbeat packets, 2 = a facing spline per flick
    // A minimum-jerk flick: the time for 180 degrees by skill, at least MinMs
    uint32 MsFor180AtSkill1 = 450;
    uint32 MsFor180AtSkill99 = 90;
    uint32 MinMs = 40;
    // The aim error drawn once per turn (log-normal, sigma per cent), by skill
    uint32 ErrorDegAtSkill1 = 14;
    uint32 ErrorDegAtSkill99 = 2;
    uint32 ErrorSigmaPct = 40;
    // The correction sub-flick after a turn: its chance by skill, its delay, the share of the error left after it
    uint32 CorrectionPctAtSkill1 = 70;
    uint32 CorrectionPctAtSkill99 = 35;
    uint32 CorrectionDelayMinMs = 120;
    uint32 CorrectionDelayMaxMs = 250;
    uint32 CorrectionResidualPct = 25;
    // Low skill sometimes turns like a keyboard turn: a constant rate
    uint32 KeyboardPctAtSkill1 = 35;
    uint32 KeyboardPctAtSkill99 = 0;
    uint32 KeyboardDegPerS = 180;
    // Packets only on a change of more than PacketMinDeg, at most every PacketMinMs
    uint32 PacketMinDeg = 8;
    uint32 PacketMinMs = 80;
    uint32 SplineMinDeg = 25;  // Mode 2: a turn below this is written as an orientation only
    uint32 CastRetryMs = 50;   // the next check a cast waits for the arc to hold
};

#endif
