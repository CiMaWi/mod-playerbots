/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAKITE_H
#define _PLAYERBOT_ARENAKITE_H

#include "Common.h"

// The Kite tactic's pure parts (G3a, PLAN 3.15). 13g-0: the per-bot state only; G3a adds the verdicts and the selftest.
struct KiteBotState
{
    uint32 EpisodeStartMs = 0;  // the first step of the running episode (0: none)
    uint32 LastStepMs = 0;
};

#endif
