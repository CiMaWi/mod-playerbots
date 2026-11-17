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

// Item use WI9b at the errand's vendor: how ShopAtVendor (NewRpgErrandVisit.cpp) names and checks one shopping line
namespace ItemUse
{
struct ShopLineView
{
    char const* word;      // the rows' word: a use item's names demand's slot (heal, mana, scroll, ...), a literal
    char const* fundWord;  // the bought row's last field: a use item's gold above the reserve, else its funded share
    std::uint64_t fund;
    bool demandKeeps;  // bought despite the item usage's sale verdict
};

// What demand keeps once bought is bought although the item usage would sell it: its use items, and a persistent
// bot's food and water, whose stock demand keeps to upkeep's target (a consumable the bot does not hold yet has no
// verdict, so the item usage calls it a sale). A limited row whose current stock fills no lot is "sold_out", apart
// from a vendor that never sells the item ("not_sold")
inline ShopLineView ViewShopLine(PlayerbotAI* botAI, UpkeepNeeds::ShopLine const& line)
{
    bool const use = line.kind == Planner::UpkeepStock::Use;
    bool const stock = line.kind == Planner::UpkeepStock::Food || line.kind == Planner::UpkeepStock::Water;
    return {use ? DemandSlotWord(static_cast<DemandSlot>(line.useSlot)).data() : UpkeepNeeds::ShopWord(line.kind),
            use ? "above" : "funded", use ? line.above : line.funded, use || (stock && DemandAnswers(botAI))};
}
}  // namespace ItemUse

#endif
