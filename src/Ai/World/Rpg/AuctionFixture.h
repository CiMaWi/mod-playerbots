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
// - standin post|bid|buyout <bot> <item> ..: the same acts of a stand-in player; standin post takes an optional
//   property id after the hours (> 0 a random property, < 0 a suffix; none: the core's random one).
// - check <bot> <item> [offers N] [sales N] [expiries N] [unit C] [basis none|sales|offers] [bid C]
//   [bidder none|bot|standin] [itemsales L] [itemhours H] [kindsales L] [kindhours H] [p12 P] [p24 P] [p48 P]:
//   compares the index entry after a forced publish; the records' lots and lot hours within 0.01 and 0.1, the sale
//   chance in permille within 1 (AH6); basis is where the typical price comes from (AH8).
// - mail <bot> <item> <kind> <n>.. [money C]: counts the bot's delivered auction mails by kind.
// - grow <bot> <item> <units>: the bot's next listing of the item (a bot under a test constraint) finds its stack grown
//   by the units right after the sell packet is queued (AH3's forced case); needs no auctioneer near.
// - listed <bot> <item> <hours>: credits the hours of listing, unsold, to the item's record and its kind's (the kind
//   from its template), then publishes, so a case need not wait hours (AH6); needs no auctioneer near.
// - stood <bot> <item> <seconds>: the index takes the item's listed offers that many seconds earlier, then publishes,
//   so a case need not wait out Market.StandingSeconds for a standing price (AH8); needs no auctioneer near.
// Each answers one line `AUCTION fixture <verb> <bot> <item>: ok, ..` or `.. error, <why>`.
namespace AuctionFixture
{
bool HandleCommand(ChatHandler* handler, char const* args);
}  // namespace AuctionFixture

#endif
