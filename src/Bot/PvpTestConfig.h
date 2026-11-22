/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTESTCONFIG_H
#define _PLAYERBOT_PVPTESTCONFIG_H

#include <string>

#include "Define.h"

// The pvptest harness (Playerbots.PvpTest.*, PvpTest.cpp, PvpTestFight.cpp; PlayerbotAIConfig::PvpTest), read once
// at config load.

struct PvpTestConfig
{
    // Test: the pvptest harness hooks and commands (read once at startup)
    bool Enable = false;
    // 60..3600: seconds before a harness fight ends as a draw
    uint32 TimeoutSeconds = 900;
    // "<map> <x> <y> <z> <o>": where the duel and brawl fight; the parser fills the five members below, an empty text
    // keeps the default spot
    std::string FightLocation = "0 -436.657 -581.254 53.5944 1.25917";
    uint32 FightMap = 0;
    float FightX = -436.657f;
    float FightY = -581.254f;
    float FightZ = 53.5944f;
    float FightO = 1.25917f;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (PvpTestConfig.cpp)
extern ConfigFamily<PvpTestConfig> const PvpTestFamily;

// The one instance (PvpTestConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.PvpTest.
PvpTestConfig& PvpTestConfigStorage();

#endif
