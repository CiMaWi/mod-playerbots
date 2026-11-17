/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestPlayerConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using TestPlayerRow = ConfigRow<TestPlayerConfig>;

constexpr std::array<TestPlayerRow, 2> TestPlayerRows = {{
    {"Enable", &TestPlayerConfig::Enable, 0, 0, ConfigKind::Test},
    {"Account", &TestPlayerConfig::Account, 0, 0, ConfigKind::Test},
}};

}  // namespace

ConfigFamily<TestPlayerConfig> const TestPlayerFamily{"Playerbots.TestPlayer.", TestPlayerRows};
