/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMDEMANDSHOP_H
#define _PLAYERBOT_ITEMDEMANDSHOP_H

#include <cstdint>

#include "ItemDemand.h"
#include "ItemDemandRules.h"
#include "UpkeepNeedRules.h"

// Item use WI9b at the errand's vendor: how ShopAtVendor (NewRpgVendor.cpp) names one shopping line
namespace ItemUse
{
struct ShopLineView
{
    char const* word;      // the rows' word: a use item's names demand's slot (heal, mana, scroll, ...), a literal
    char const* fundWord;  // the bought row's last field: a use item's gold above the reserve, else its funded share
    std::uint64_t fund;
};

// A use item's row names demand's slot (heal, mana, scroll, ...) and its gold above the reserve; another stock's its
// kind and funded share. The item usage keeps what either stock rule buys (ItemUsageValue.cpp, upkeep WU28), so no
// line skips the sale check. A limited row whose current stock fills no lot is "sold_out", apart from a vendor that
// never sells the item ("not_sold")
inline ShopLineView ViewShopLine(UpkeepNeeds::ShopLine const& line)
{
    bool const use = line.kind == Planner::UpkeepStock::Use;
    return {use ? DemandSlotWord(static_cast<DemandSlot>(line.useSlot)).data() : UpkeepNeeds::ShopWord(line.kind),
            use ? "above" : "funded", use ? line.above : line.funded};
}
}  // namespace ItemUse

#endif
