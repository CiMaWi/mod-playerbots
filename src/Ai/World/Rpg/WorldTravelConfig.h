/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDTRAVELCONFIG_H
#define _PLAYERBOT_WORLDTRAVELCONFIG_H

#include "Define.h"

// The numbers of honest world travel for persistent bots (Playerbots.WorldTravel.*), read once at config load
// (PlayerbotAIConfig::WorldTravel); the initial values are the conf.dist defaults. Kept apart from PlayerbotAIConfig.h
// so that later members do not rebuild every user of that header. The rules using them are in WorldTravelRules.h.
struct WorldTravelConfig
{
    // A remaining teleport is skipped while a real player is within this many yards of the bot or the destination
    uint32 SightYards = 300;
    // Corpse runs that may fail (the bot dies again first) before a dead bot uses the spirit healer
    uint32 CorpseRunFailures = 2;
    // Seconds a destination given up after a Stuck event is not walked to again
    uint32 StuckAvoidSeconds = 600;
};

#endif
