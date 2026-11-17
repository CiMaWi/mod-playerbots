/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGCONFIG_H
#define _PLAYERBOT_WORLDLOGCONFIG_H

#include <cstdint>
#include <string>

// Playerbots.WorldLog.*: the values as configured (PlayerbotAIConfig::WorldLog); the initial values are the defaults.
// (the defaults equal WorldLogFormat's, WorldLogConfig.cpp checks it). WorldLog::LoadConfig turns them into the running
// settings (SegmentMinutes is rounded down to a divisor of 60 there). Player-free (the selftests include it).
struct WorldLogOptions
{
    // The output directory, empty = the world log is off; read at startup only (a reload keeps the first one)
    std::string Dir;
    uint32_t StateSeconds = 60;
    uint32_t SegmentMinutes = 60;
    uint32_t QueueRows = 200000;
    // 0 = the recorder is off
    uint32_t RecorderSeconds = 300;
    uint32_t RecorderActions = 128;
    uint32_t RecorderEvents = 64;
    uint32_t RecorderCooldownSeconds = 600;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (WorldLogConfig.cpp)
extern ConfigFamily<WorldLogOptions> const WorldLogFamily;

#endif
