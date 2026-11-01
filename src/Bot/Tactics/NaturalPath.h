/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALPATH_H
#define _PLAYERBOT_NATURALPATH_H

#include "Common.h"

class PlayerbotAI;

// Natural paths (Playerbots.Natural.Path.*, G2c): lateral offsets of the interior waypoints, curves, strafes, hops
// and walking legs; the numbers are PathConfig's (NaturalPathConfig.h). G2c fills this header; 13f-0 only makes
// the names and the per-bot state exist.

// The per-bot state (PlayerbotAI::GetPathState; the bot's own update, tactics show on the world thread)
struct PathBotState
{
    uint32 InSightCheckedMs = 0;  // NaturalPathHere's real-player-in-sight cache (OutsidePvpInSight)
    bool InSight = false;
};

// Whether the bot walks natural paths now (PlayerbotAI.cpp; as NaturalTrackHere with the Path options)
bool NaturalPathHere(PlayerbotAI* botAI, uint32 nowMs);

#endif
