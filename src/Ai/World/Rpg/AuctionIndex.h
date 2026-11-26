/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONINDEX_H
#define _PLAYERBOT_AUCTIONINDEX_H

#include <cstdint>
#include <memory>
#include <vector>

#include "AuctionIndexRules.h"

class AuctionHouseObject;
class ChatHandler;

// The auction house price index (PLAN 3.3): fed on the world thread by the core's auction hooks and one walk at
// startup, published every Playerbots.Auction.Market.PublishSeconds as an immutable snapshot. Nothing but the world
// thread touches the core's auction maps; every other reader takes Current().
namespace AuctionIndex
{
// The latest snapshot, set from startup on; any thread
std::shared_ptr<Snapshot const> Current();

// World thread: the distinct house objects; with AllowTwoSide.Interaction.Auction every id gives the neutral one, so a
// walk over the three ids would see each auction three times (WAL1)
std::vector<AuctionHouseObject*> Houses();

// World thread, from the module's world update: a timer add; every PublishSeconds a refresh and a publication
void Update(std::uint32_t diff);

// World thread: a publication now, the same work as Update's (the auction fixture's check reads the index right after
// an act, WA1c)
void PublishNow();

// An item entry's kind from its template (AH.4); none for an entry without one. Any thread (the templates are read
// only)
KindKey KindOfEntry(std::uint32_t item);

// World thread, the auction fixture's `listed` seam (AH6, test only): credits hours of listing to the item's record and
// its kind's, then publishes; false for an item without a kind or no hours
bool CreditListing(std::uint32_t item, double hours);

// playerbots auction index <item id or link>: prints an item's entry from the snapshot (read only) and its records with
// the sale chance at 12, 24 and 48 h
bool HandleIndexCommand(ChatHandler* handler, char const* args);
}  // namespace AuctionIndex

void AddSC_auction_index();

#endif
