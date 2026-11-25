/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONBUYLIST_H
#define _PLAYERBOT_AUCTIONBUYLIST_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "AuctionPricing.h"

// What a bot wants from the market (auction house plan WA4a, PLAN 3.6, WA.5, WA.6, WA.17): the buy list's need lines,
// each from its owner, the gold the market may spend, the bot's open bids and the choice for one line among the
// house's current offers. Pure and std only: tests/auction-rules-selftest.cpp checks it
namespace AuctionRules
{
// Who wants a line; the list is funded in this order. Path (the skill path's materials and recipes, WA7) and Gear
// (WA.6: no gear at auction yet) are declared for their later owners
enum class LineOwner : std::uint8_t
{
    Use,  // item use's demand (ItemDemandRules.h DemandLine: buyEntry, buyUnits, WorthEachOf)
    Bag,  // a bag by its worth (ItemWorthRules.h BagWorth)
    Path,
    Gear
};

constexpr std::array<std::string_view, 4> LINE_OWNER_WORDS = {"use", "bag", "path", "gear"};

inline std::string_view OwnerWord(LineOwner owner) { return LINE_OWNER_WORDS[static_cast<std::size_t>(owner)]; }

// One need line: units of an item, worth and getting per unit in the planner's value units (copper x factor)
struct BuyLine
{
    std::uint32_t item{0};
    LineOwner owner{LineOwner::Use};
    std::uint32_t units{0};
    CeilingInput ceiling;
    std::uint64_t excessUnitWorth{0};  // copper: a unit beyond the line, its vendor payout or keep worth
};

inline std::uint64_t CeilingOf(BuyLine const& line) { return Ceiling(line.ceiling); }

// Any owner's line from value units; gettingValue < 0: no other way of getting it
inline BuyLine LineOf(LineOwner owner, std::uint32_t item, std::uint32_t units, double worthValue, double gettingValue,
                      float factor, std::uint64_t excessUnitWorth)
{
    BuyLine line;
    line.item = item;
    line.owner = owner;
    line.units = units;
    line.ceiling = {worthValue, gettingValue, factor};
    line.excessUnitWorth = excessUnitWorth;
    return line;
}

// Item use's line: worth and the vendor's price are copper, so the factor cancels and stays 1 (an exact ceiling);
// vendorCopperEach 0: no vendor sells it
inline BuyLine UseLine(std::uint32_t item, std::uint32_t units, std::uint32_t worthCopperEach,
                       std::uint32_t vendorCopperEach, std::uint32_t payoutCopperEach)
{
    return LineOf(LineOwner::Use, item, units, worthCopperEach, vendorCopperEach ? vendorCopperEach : -1.0, 1.0f,
                  payoutCopperEach);
}

// A bag by the worth of the slots it adds (copper over the horizon); vendorCopper 0: no vendor sells it
inline BuyLine BagLine(std::uint32_t item, std::uint32_t worthCopper, std::uint32_t vendorCopper)
{
    return LineOf(LineOwner::Bag, item, 1, worthCopper, vendorCopper ? vendorCopper : -1.0, 1.0f, 0);
}

// A skill path's material (auction house WA7b, PLAN 3.7): worth and getting are both what getting a unit the
// cheapest other way costs (the path's saving bounds the price), copper, so the factor stays 1; otherCopperEach 0: no
// other way, no worth known, no line
inline BuyLine PathLine(std::uint32_t item, std::uint32_t units, std::uint32_t otherCopperEach,
                        std::uint32_t payoutCopperEach)
{
    return LineOf(LineOwner::Path, item, units, otherCopperEach, otherCopperEach ? otherCopperEach : -1.0, 1.0f,
                  payoutCopperEach);
}

// The skill path's lines: each material whose cheapest source is the auction (source == auctionKind, the pricer's
// Materials::Kind::Auction), for the units the bot lacks now: its want when the path was computed, at most what the
// path uses beyond the units the bot holds at this moment (heldNow(item): bags and bank, as the path counts them). The
// kept path is older than the bags, so a bot that gathered or was sent the material since buys no unit it already
// holds, and one whose bags emptied never asks for more than the path's want. Recipes and ranks stay with the trainers
// and vendors. payout(item): one unit's vendor payout in copper
template <typename MaterialList, typename Held, typename Payout>
std::vector<BuyLine> PathLines(MaterialList const& materials, std::uint8_t auctionKind, Held const& heldNow,
                               Payout const& payout)
{
    std::vector<BuyLine> lines;
    for (auto const& material : materials)
    {
        if (!material.wanted || material.source != auctionKind)
            continue;
        std::uint32_t const held = heldNow(material.item);
        std::uint32_t const lacking = material.used > held ? material.used - held : 0;
        if (std::uint32_t const units = std::min<std::uint32_t>(material.wanted, lacking))
            lines.push_back(PathLine(material.item, units, material.otherCopperEach, payout(material.item)));
    }
    return lines;
}

// The buy list: the lines with an item, units and a ceiling, in owner order (each owner's own order kept)
inline std::vector<BuyLine> BuyList(std::vector<BuyLine> lines)
{
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [](BuyLine const& line) { return !line.item || !line.units || !CeilingOf(line); }),
                lines.end());
    std::stable_sort(lines.begin(), lines.end(), [](BuyLine const& a, BuyLine const& b) { return a.owner < b.owner; });
    return lines;
}

// The line's copper at its typical unit price (the index's TypicalPrice, known anywhere, WA.4) when that is under the
// ceiling: the market's claim in upkeep's order (UpkeepNeeds::MarketWant); 0 not wanted at that price
inline std::uint64_t TypicalCopper(BuyLine const& line, std::uint32_t typicalUnit)
{
    std::uint64_t const ceiling = CeilingOf(line);
    return typicalUnit && typicalUnit < ceiling ? static_cast<std::uint64_t>(typicalUnit) * line.units : 0;
}

// What the line adds to a stop's worth (WA4b, ErrandSnapshot::AuctionWantedWorth): the ceiling above the typical
// price for the units the spendable gold buys at it
inline std::uint64_t WantedWorth(BuyLine const& line, std::uint32_t typicalUnit, std::uint64_t spendable)
{
    std::uint64_t const ceiling = CeilingOf(line);
    if (!typicalUnit || typicalUnit >= ceiling)
        return 0;
    std::uint64_t const units = std::min<std::uint64_t>(line.units, spendable / typicalUnit);
    return (ceiling - typicalUnit) * units;
}

// The gold the market may spend now: the purse above the reserve, less what claims before it in upkeep's order
// still hold (the use items not bought yet). Open bids are not subtracted: the core takes a bid's copper from the
// purse when it is placed, so the purse already counts them as spent
inline std::uint64_t Spendable(std::uint64_t money, std::uint64_t reserve, std::uint64_t heldBefore)
{
    std::uint64_t const kept = reserve + heldBefore;
    return money > kept ? money - kept : 0;
}

// The bot's open bids: the index's listed auctions whose bidder it is (raw guid), in any house. Once per visit,
// O(listed lines). A held bid on a lot no short list keeps is not seen: the cap limits the bot, the core's own
// checks stay the guard
inline std::uint32_t OpenBids(AuctionIndex::Snapshot const& snapshot, std::uint64_t bidder)
{
    std::uint32_t open = 0;
    for (auto const& [item, entry] : snapshot.items)
        for (AuctionIndex::OfferLine const& line : entry.offers.cheapest)
            open += line.bidder == bidder ? 1 : 0;
    return open;
}

// The house's offers for a line: its lines of this house, without the bot's own auctions (AuctionFacts has no
// owner: the visit passes the ids it listed; the core refuses a bid on them too)
inline std::vector<OfferInput> OffersFor(AuctionIndex::CurrentOffers const& offers, BuyLine const& line,
                                         std::uint8_t houseId, std::vector<std::uint32_t> const& ownAuctions)
{
    std::vector<OfferInput> inputs;
    for (AuctionIndex::OfferLine const& offer : offers.cheapest)
        if (offer.houseId == houseId &&
            std::find(ownAuctions.begin(), ownAuctions.end(), offer.id) == ownAuctions.end())
            inputs.push_back({offer, line.units, line.excessUnitWorth});
    return inputs;
}

// One line at the auctioneer: buyout, bid or skip among the house's current offers. `buyer` holds the spendable gold,
// the open bids, the cap, the time and the bot's guid; the line sets the ceiling
inline BuyChoice ChooseLine(AuctionIndex::CurrentOffers const& offers, BuyLine const& line, std::uint8_t houseId,
                            std::vector<std::uint32_t> const& ownAuctions, BuyInput buyer)
{
    buyer.ceilingPerUnit = CeilingOf(line);
    return ChooseBuy(OffersFor(offers, line, houseId, ownAuctions), buyer);
}
}  // namespace AuctionRules

#endif
