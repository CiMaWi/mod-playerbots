/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MATCHLOGCONFIG_H
#define _PLAYERBOT_MATCHLOGCONFIG_H

#include <string>

#include "Define.h"

// The match trace (Playerbots.MatchLog.*, MatchTrace.cpp; PlayerbotAIConfig::MatchLog), read once at config load.
// The members are named after their keys without the prefix.

struct MatchLogConfig
{
    // Test: 0 = no trace; N = trace every Nth match (the hooks return after one test with 0)
    uint32 TraceEvery = 0;
    // 16..1000000: events of one trace; 4096..67108864: the estimated bytes of its line
    uint32 TraceMaxEvents = 20000;
    uint32 TraceMaxBytes = 4194304;
};

#endif
