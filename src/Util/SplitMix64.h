/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPLITMIX64_H
#define PLAYERBOTS_SPLITMIX64_H

#include "Define.h"

// splitmix64 as a stateless hash: a fixed integer mix, the same on every build and standard library. The skill
// notice windows (SkillStrategy) and the tactic gate (TacticGate.h) hash a bot's guid, a salt and a window with it.
// BotProfileMgr's skill rolls step their own state with the same mix (SplitMix64Next there).
inline uint64 SplitMix64(uint64 state)
{
    state += 0x9E3779B97F4A7C15ULL;
    uint64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

#endif
