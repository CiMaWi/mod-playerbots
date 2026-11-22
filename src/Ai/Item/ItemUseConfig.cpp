/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUseConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using ItemUseRow = ConfigRow<ItemUseConfig>;

constexpr std::array<ItemUseRow, 1> ItemUseRows = {{
    {"UseEndGraceMs", &ItemUseConfig::UseEndGraceMs, 0, 10000},
}};

}  // namespace

ConfigFamily<ItemUseConfig> const ItemUseFamily{"Playerbots.ItemUse.", ItemUseRows};

ItemUseConfig& ItemUseConfigStorage()
{
    static ItemUseConfig values;
    return values;
}
