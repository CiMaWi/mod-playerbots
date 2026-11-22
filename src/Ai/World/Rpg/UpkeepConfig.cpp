/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UpkeepConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using UpkeepRow = ConfigRow<UpkeepConfig>;

// HandoutShare: a test switch for comparisons and forced checks (0-100)
constexpr std::array<UpkeepRow, 1> UpkeepRows = {{
    {"HandoutShare", &UpkeepConfig::HandoutShare, 0, 100, ConfigKind::Test},
}};
}  // namespace

ConfigFamily<UpkeepConfig> const UpkeepFamily{"Playerbots.Upkeep.", UpkeepRows};

UpkeepConfig& UpkeepConfigStorage()
{
    static UpkeepConfig values;
    return values;
}
