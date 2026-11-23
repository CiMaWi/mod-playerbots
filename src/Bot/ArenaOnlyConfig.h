/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAONLYCONFIG_H
#define _PLAYERBOT_ARENAONLYCONFIG_H

#include <string>

#include "Define.h"

// The arena-only role (Playerbots.ArenaOnly.*; PlayerbotAIConfig::ArenaOnly), read once at config load. The
// ladder role clears Enable at role 1 (PlayerbotAIConfig::ApplyLadderRole).

struct ArenaOnlyConfig
{
    // Role: random bots outside an arena only wait for their next match (an arena-only test server)
    bool Enable = false;
    // Test: the waiting bots spread over 4 continents
    bool SpreadContinents = true;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (ArenaOnlyConfig.cpp)
extern ConfigFamily<ArenaOnlyConfig> const ArenaOnlyFamily;

// The one instance (ArenaOnlyConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.ArenaOnly.
ArenaOnlyConfig& ArenaOnlyConfigStorage();

#endif
