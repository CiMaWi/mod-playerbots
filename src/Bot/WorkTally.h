/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORKTALLY_H
#define _PLAYERBOT_WORKTALLY_H

#include "Define.h"

// The miss switch's work tally (Playerbots.Test.MissLog, labs only; CS5 design 6.5): the bot updates, those with work
// and each BotWork bit's count, all bots together on relaxed counters; every 2^20 updates one "Work tally:" line.
// Note returns its work word
namespace WorkTally
{
uint32 Note(uint32 work);
}  // namespace WorkTally

#endif
