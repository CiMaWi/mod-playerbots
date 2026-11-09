/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDTRAVELCONFIG_H
#define _PLAYERBOT_WORLDTRAVELCONFIG_H

#include "Define.h"
#include "ItemWorthRules.h"

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
    // Route search (RouteRules.h): worker threads, the cost factor of an edge flagged by a Stuck event, and the
    // expected wait for a ship or zeppelin (half its period) until WT19 knows the real periods
    uint32 RouteThreads = 1;
    uint32 StuckPenalty = 5;
    uint32 TransportWaitSeconds = 120;
    // Slot and bag worth by level band (ItemWorthRules.h) until a bot has measured its own loot rate
    WorldTravel::SlotBandTable SlotBands = WorldTravel::DEFAULT_SLOT_BANDS;
    // Percent of the money reserve (repairs, training, travel, ammo, guild) kept before buying a bag
    uint32 BagThriftPercent = 100;
    // Percent of the money above the reserve that one bag may cost
    uint32 BagBudgetPercent = 50;
    // Percent by which loot must out-worth the item a full bot destroys for it outside instances
    uint32 DestroyMarginPercent = 50;
    // Minutes until a bot would pass a vendor anyway; a trip just for selling must save more than it costs over them
    uint32 ErrandNextStopMinutes = 30;
    // WT11a: the test switch for `.playerbots traits <bot> set|reroll` (labs only)
    bool TraitsAllowEdit = false;
};

#endif
