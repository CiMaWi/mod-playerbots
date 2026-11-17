/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaOnlyConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using ArenaOnlyRow = ConfigRow<ArenaOnlyConfig>;

constexpr std::array<ArenaOnlyRow, 2> ArenaOnlyRows = {{
    {"Enable", &ArenaOnlyConfig::Enable, 0, 0, ConfigKind::Role},
    {"SpreadContinents", &ArenaOnlyConfig::SpreadContinents, 0, 0, ConfigKind::Test},
}};

}  // namespace

ConfigFamily<ArenaOnlyConfig> const ArenaOnlyFamily{"Playerbots.ArenaOnly.", ArenaOnlyRows};
