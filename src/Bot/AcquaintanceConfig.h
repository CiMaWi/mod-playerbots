/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ACQUAINTANCECONFIG_H
#define _PLAYERBOT_ACQUAINTANCECONFIG_H

#include <string>

#include "Define.h"

// The acquaintance record of real players with random bots (Playerbots.Acquaintance.*, AcquaintanceMgr.cpp;
// PlayerbotAIConfig::Acquaintance), read once at config load. The members are named after their keys without the
// prefix. The conf.dist text above the first key describes the record.

struct AcquaintanceConfig
{
    // 5..3600: seconds between two writes of the changed rows (one asynchronous transaction)
    uint32 FlushSeconds = 30;
    // 0..86400: seconds before the same pair counts another direct interaction (whisper, party chat, trade, duel)
    uint32 DirectCooldownSeconds = 60;
    // 0..86400: seconds before the same pair counts another group or guild join
    uint32 GroupJoinCooldownSeconds = 600;
    // Score points, each 0..10000: a direct interaction, a group join, a guild join, a PvP kill, a full group minute,
    // a full guild hour, a full minute near each other
    uint32 PointsDirect = 10;
    uint32 PointsGroupJoin = 20;
    uint32 PointsGuildJoin = 15;
    uint32 PointsPvpKill = 5;
    uint32 PointsPerGroupMinute = 5;
    uint32 PointsPerGuildHour = 6;
    uint32 PointsPerNearbyMinute = 1;
    // 1..2000000000: the cap of a pair's score
    uint32 MaxScore = 1000000;
    // 1..3600: seconds between the group time samples; 5..3600: the guild time samples
    uint32 GroupSampleSeconds = 10;
    uint32 GuildSampleSeconds = 60;
    // 0..3600 (0 = off): seconds between the nearby time samples; 1..100: their radius in yards
    uint32 NearbyIntervalSeconds = 10;
    float NearbyRadius = 30.0f;
};

#endif
