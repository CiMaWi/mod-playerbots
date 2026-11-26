/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GEARLINES_H
#define _PLAYERBOT_GEARLINES_H

#include <cstdint>
#include <vector>

#include "AuctionBuyList.h"
#include "AuctionIndexRules.h"
#include "Define.h"
#include "GearWorthRules.h"

class PlayerbotAI;

namespace GearWorth
{
// What the house offers the bot for its gear (gear WG7, PLAN 3.3)
struct AuctionGear
{
    std::vector<AuctionRules::BuyLine> lines;  // one per slot, the largest surplus first, each naming its auction
    std::uint8_t noOfferSlot{NO_SLOT};         // without a line: the slot the visit's no_offer row names
};

// The bot's thread, once per auction visit (R7), at the visit's buy step (I4: not in Planner::BuyListOf, which is
// read away from the auctioneer). Walks the house's listed weapons and armour once (O(listed lots)), `only` limiting
// it to those entries (a test's filter; empty = all; naming no gear: nothing): per lot the worth with its suffix (one
// InputReader), the price and the ceiling; PickPerSlot keeps per slot the offer the upgrade rule would wear (R1, R4).
// Each pick is a gear line naming its auction, with the vendor payout as its excess worth. A slot the bot leads a
// bid for counts as filled. Nothing for a bot that keeps no possessions, may not auto-equip or wears a PvP set
AuctionGear LinesAt(PlayerbotAI* botAI, AuctionIndex::Snapshot const& snapshot, uint8 houseId,
                    std::vector<uint32> const& only, uint64 spendable);
}  // namespace GearWorth

#endif
