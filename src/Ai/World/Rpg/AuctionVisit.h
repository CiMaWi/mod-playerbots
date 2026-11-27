/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONVISIT_H
#define _PLAYERBOT_AUCTIONVISIT_H

#include <optional>

#include "AuctionMemory.h"
#include "Define.h"
#include "Visit.h"

struct AuctionEntry;
class ChatHandler;
class Player;
class PlayerbotAI;

// The auctioneer visit (auction house WA3b): a selling trip's stop at an auctioneer. Once in reach the bot lists its
// bag stock (the carried keeps of AuctionStock) one stack per step through the core's sell packet, at the price
// AuctionRules::Ask gives at this house (its fees, the cheapest competing buyout, the seller's expiry memory); a
// stack Ask holds gets a `hold` decision row and stays. A listing or a purchase the core took renews the visit's limit,
// and a visit that never came into reach ends `unreached`. The driver is Visit.h's; the row of the visit is
// AuctionVisitRow.
namespace BankMail
{
using AuctioneerSpot = VisitSpot;
// The nearest auctioneer serving the bot within yards of a place (an errand stop's own); distance is from the bot
std::optional<AuctioneerSpot> AuctioneerSpotNear(Player* bot, uint32 mapId, float x, float y, float z, float yards);
// World thread or the bot's map thread: starts the visit; false while a visit of any kind runs
bool BeginAuctionVisit(PlayerbotAI* botAI, AuctioneerSpot const& spot);
// GM console `playerbots auction visit <bot> [<item> ...]`: the visit at the nearest auctioneer serving the bot within
// the command's reach (VISIT_COMMAND_REACH_YARDS); answers `AUCTION visit <bot>: started ..` or `.. refused, <reason>`.
// Items (a test's filter, WA4b, WG7) make the buy step decide those lines alone and walk only those entries for gear
// (none of them weapon or armour: no gear step). `far` (a test's start, AH3) starts the
// visit at any distance the search finds, as an errand's visit starts wherever the stop's spot finds the bot
bool HandleAuctionVisitCommand(ChatHandler* handler, char const* args);
// World thread, the fixture's `grow` verb (a test seam, AH3): a bot under a test constraint whose next listing of the
// item goes out finds its stack grown by the units right after the sell packet is queued
void ArmAuctionGrow(Player* bot, uint32 entry, uint32 units);
}  // namespace BankMail

// The sellers' expiry memories (AuctionMemory.h), one per character that listed since the server start
namespace AuctionMemory
{
// Any thread: what the seller remembers of the item
ItemMemory Of(uint32 sellerLow, uint32 entry);
// What the core's hooks took of a bot's own packets, the visit's proof of an answer. Kept for the server's run, apart
// from the listings above, so a sale or a removal in between loses none of it
struct Taken
{
    uint32 listings{0};   // its listings the add hook saw
    uint32 listed{0};     // the item entry of the last of them
    uint32 boughtOut{0};  // the auction id of its last won buyout
};
// Any thread
Taken TakenOf(uint32 botLow);
// World thread, from the core's auction hooks (AuctionIndex.cpp): a bot's listing, and the sale, expiry or removal
// of a listing a bot made; each with the seller's event row when it is online (AuctionPost, AuctionSale,
// AuctionExpire)
void Listed(AuctionEntry const* auction);
void Sold(AuctionEntry const* auction);
void Expired(AuctionEntry const* auction);
void Removed(AuctionEntry const* auction);
// World thread, from the won and outbid mails' hooks (WA4b), online players only: AuctionBuy (a buyout) or AuctionWon
// (the leading bid at the end) of the buyer, AuctionOutbid of the bidder a higher bid displaced
void Won(AuctionEntry const* auction, Player* buyer);
void Outbid(AuctionEntry const* auction, Player* bidder, uint32 newPrice);
// The bot's map thread: its auctioneer visit held a stack of the item (AuctionMemory.h, SellerMemory::Held)
void Held(uint32 sellerLow, uint32 entry);
}  // namespace AuctionMemory

#endif
