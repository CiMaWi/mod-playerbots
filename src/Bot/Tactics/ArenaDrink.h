/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENADRINK_H
#define _PLAYERBOT_ARENADRINK_H

#include "Common.h"

// The Drink tactic's pure parts (G3b, PLAN 3.15). 13g-0: the per-bot state only; G3b adds the verdicts and the
// selftest.
struct DrinkBotState
{
    uint32 StartMs = 0;  // when the running drink started (0: none)
    uint32 NoDrinkLogMs = 0;
    bool Prep = false;
};

#endif
