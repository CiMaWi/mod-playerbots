/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONINDEX_H
#define _PLAYERBOT_AUCTIONINDEX_H

#include <cstdint>
#include <memory>

#include "AuctionIndexRules.h"

class ChatHandler;

// The auction house price index (PLAN 3.3): fed on the world thread by the core's auction hooks and one walk at
// startup, published every Playerbots.Auction.Market.PublishSeconds as an immutable snapshot. Nothing but the world
// thread touches the core's auction maps; every other reader takes Current().
namespace AuctionIndex
{
// The latest snapshot, set from startup on; any thread
std::shared_ptr<Snapshot const> Current();

// World thread, from the module's world update: a timer add; every PublishSeconds a refresh and a publication
void Update(std::uint32_t diff);

// playerbots auction index <item id or link>: prints an item's entry from the snapshot (read only)
bool HandleIndexCommand(ChatHandler* handler, char const* args);
}  // namespace AuctionIndex

void AddSC_auction_index();

#endif
