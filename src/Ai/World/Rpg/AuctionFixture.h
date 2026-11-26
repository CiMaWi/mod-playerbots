/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONFIXTURE_H
#define _PLAYERBOT_AUCTIONFIXTURE_H

class ChatHandler;

// Test fixture verbs of the auction house plan (WA1c), GM console only; they run on the world thread. A scene is a
// bot standing at an auctioneer and an item entry: an auction is named by its item (the newest listing of that item
// in the auctioneer's house), so every bot of a test needs an item of its own.
// Verbs (playerbots auction fixture ...):
// - post <bot> <item> <count> <bid> <buyout> <hours 12|24|48>: queues CMSG_AUCTION_SELL_ITEM.
// - bid <bot> <item> <price>, buyout <bot> <item>: queue CMSG_AUCTION_PLACE_BID (buyout: at the auction's buyout).
// - cancel <bot> <item>: queues CMSG_AUCTION_REMOVE_ITEM.
// - end <bot> <item>: ends the auction now, by the core's own end path.
// - standin post|bid|buyout <bot> <item> ..: the same acts of a stand-in player.
// - check <bot> <item> [offers N] [sales N] [expiries N] [unit C] [bid C] [bidder none|bot|standin]: compares the
//   index entry after a forced publish.
// - mail <bot> <item> <kind> <n>.. [money C]: counts the bot's delivered auction mails by kind.
// - grow <bot> <item> <units>: the bot's next listing of the item (a bot under a test constraint) finds its stack grown
//   by the units right after the sell packet is queued (AH3's forced case); needs no auctioneer near.
// Each answers one line `AUCTION fixture <verb> <bot> <item>: ok, ..` or `.. error, <why>`.
namespace AuctionFixture
{
bool HandleCommand(ChatHandler* handler, char const* args);
}  // namespace AuctionFixture

#endif
