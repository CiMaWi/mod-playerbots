/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAJUMP_H
#define _PLAYERBOT_ARENAJUMP_H

#include "Common.h"

// The Jump tactic's pure parts (G2g, PLAN 3.15). 13g-0: the per-bot state only; G2g-a adds JumpArc, the spot table
// and the test machine. Frozen: the jump test drives the bot, so UpdateAI runs no engine (PlayerbotAI::jumpTest).
struct JumpBotState
{
    bool Frozen = false;
};

#endif
