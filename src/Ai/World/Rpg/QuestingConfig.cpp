/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestingConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using Row = ConfigRow<QuestingConfig>;

constexpr std::array<Row, 1> QuestingRows = {{
    {"GiveUpMinutes", &QuestingConfig::GiveUpMinutes, 1, 120},
}};
}  // namespace

ConfigFamily<QuestingConfig> const QuestingFamily{"Playerbots.Questing.", QuestingRows};
