/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TESTPLAYERCONFIG_H
#define _PLAYERBOT_TESTPLAYERCONFIG_H

#include <string>

#include "Define.h"

// The test player harness (Playerbots.TestPlayer.*, TestPlayer.cpp; PlayerbotAIConfig::TestPlayer), read once at
// config load.

struct TestPlayerConfig
{
    // Test: the test player harness (TestPlayer.cpp); main sets it, scenarios set both
    bool Enable = false;
    // The account the harness's characters belong to
    std::string Account = "WBTEST";
};

template <typename Struct>
struct ConfigFamily;

// The config table family (TestPlayerConfig.cpp)
extern ConfigFamily<TestPlayerConfig> const TestPlayerFamily;

// The one instance (TestPlayerConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.TestPlayer.
TestPlayerConfig& TestPlayerConfigStorage();

#endif
