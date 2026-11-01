/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACOVER_H
#define _PLAYERBOT_ARENACOVER_H

#include "Common.h"

// Arena cover (G2d): the pillar table per arena, FindCoverPoint, NextCirclePoint and CoverIsStillValid, and the
// per-bot cover plan and relocation guards of the Los and Pillar tactics (G2e, G2f); the numbers are LosConfig's and
// PillarConfig's (ArenaCoverConfig.h). G2d fills this header; 13f-0 only makes the names and the state exist.

// A bot's cover plan (the value "cover plan"; GUIDs and numbers only, never a Unit pointer)
struct CoverPlan
{
    uint32 UpdatedMs = 0;
};

// The per-bot state (PlayerbotAI::GetCoverState; the bot's own update, tactics show on the world thread): later the
// partner-LOS-lost start, the ring of the last relocations and the time of the last search
struct CoverBotState
{
    uint32 LastSearchMs = 0;
};

#endif
