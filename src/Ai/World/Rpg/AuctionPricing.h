/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONPRICING_H
#define _PLAYERBOT_AUCTIONPRICING_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

#include "AuctionConfig.h"
#include "AuctionIndexRules.h"

// What a bot asks and what it pays at most (auction house plan WA2b, PLAN 3.4 and 3.6, WA.2, WA.5, WA.17): the fees
// as the core computes them, whether a listing beats a vendor, the ask from the evidence and the bot's own outcomes,
// the ceiling, and buyout or bid over offers ranked by what the need costs. Pure and std only: AuctionReference.cpp
// copies the houses' fee rows and the rates out once at startup; tests/auction-rules-selftest.cpp checks this code.
// The fee formulas are re-implemented from AzerothCore's AuctionHouseMgr.cpp (GetAuctionDeposit, GetAuctionCut,
// CalculateAuctionOutBid; GPL v2) in the core's operation order and binary32 arithmetic.
namespace AuctionRules
{
// ---- fees ----
// The houses as the core stores them (AuctionHouseId)
constexpr std::uint8_t HOUSE_ALLIANCE = 2;
constexpr std::uint8_t HOUSE_HORDE = 6;
constexpr std::uint8_t HOUSE_NEUTRAL = 7;
constexpr std::uint32_t MINIMUM_DEPOSIT = 100;  // AuctionHouseMgr.cpp AH_MINIMUM_DEPOSIT
constexpr std::uint32_t OUTBID_PERCENT = 5;
constexpr std::uint32_t SECONDS_PER_HOUR = 3600;
constexpr std::array<std::uint32_t, 3> LISTING_HOURS = {12, 24, 48};                // the durations the client offers
constexpr std::uint64_t MAX_LOT_PRICE = std::numeric_limits<std::uint32_t>::max();  // the core's bid is a uint32
constexpr std::uint64_t NEVER_LISTS = std::numeric_limits<std::uint64_t>::max();

struct FeeRow  // AuctionHouseEntry::depositPercent, cutPercent
{
    std::uint32_t depositPercent{5};
    std::uint32_t cutPercent{5};
};

struct FeeRates  // Rate.Auction.Deposit, .Cut, .Time
{
    float deposit{1.0f};
    float cut{1.0f};
    float time{1.0f};
};

struct Fees  // one house, copied out at startup
{
    FeeRow row;
    FeeRates rates;
};

// The core's CalculatePct (Util.h) on a float base; one operation per statement keeps binary32 rounding
inline float PctOf(float base, std::uint32_t percent)
{
    float const scaled = base * static_cast<float>(percent);
    return scaled / 100.0f;
}

inline std::uint32_t FloatToCopper(float value)
{
    if (!(value > 0.0f))
        return 0;
    return value >= 4294967296.0f ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(value);
}

// The deposit before the minimum; durationSeconds as the client sends it (hours x 3600), sellPrice per unit
inline std::uint32_t RawDeposit(Fees const& fees, std::uint32_t sellPrice, std::uint32_t count,
                                std::uint32_t durationSeconds)
{
    float const multiplier = PctOf(static_cast<float>(fees.row.depositPercent), 3);
    std::uint32_t const twelveHours = durationSeconds / 60 / 60 / 12;
    float value = multiplier * static_cast<float>(sellPrice);
    value = value * static_cast<float>(count);
    value = value / 3.0f;
    value = value * static_cast<float>(twelveHours);
    value = value * 3.0f;
    value = value * fees.rates.deposit;
    return FloatToCopper(value);
}

// GetAuctionDeposit: the minimum scales with the rate, also for an item without a sell price
inline std::uint32_t Deposit(Fees const& fees, std::uint32_t sellPrice, std::uint32_t count,
                             std::uint32_t durationSeconds)
{
    float const minimum = static_cast<float>(MINIMUM_DEPOSIT) * fees.rates.deposit;
    if (!sellPrice)
        return FloatToCopper(minimum);
    std::uint32_t const deposit = RawDeposit(fees, sellPrice, count, durationSeconds);
    if (static_cast<float>(deposit) < minimum)
        return FloatToCopper(minimum);
    return deposit;
}

// GetAuctionCut: the percentage truncated first, then the rate and a second truncation
inline std::uint32_t Cut(Fees const& fees, std::uint32_t bid)
{
    std::uint32_t const base = FloatToCopper(PctOf(static_cast<float>(bid), fees.row.cutPercent));
    float const scaled = static_cast<float>(base) * fees.rates.cut;
    return FloatToCopper(scaled);
}

// CalculateAuctionOutBid: the least raise over a bid
inline std::uint32_t OutBid(std::uint32_t bid)
{
    std::uint32_t const raise = FloatToCopper(PctOf(static_cast<float>(bid), OUTBID_PERCENT));
    return raise ? raise : 1;
}

// The longest listing the client offers not above wanted, at least the shortest
inline std::uint32_t DurationHours(std::uint32_t wanted)
{
    std::uint32_t hours = LISTING_HOURS.front();
    for (std::uint32_t const offered : LISTING_HOURS)
        if (offered <= wanted)
            hours = offered;
    return hours;
}

// How long a listing really runs: the handler scales the nominal seconds by Rate.Auction.Time
inline std::uint32_t ExpirySeconds(Fees const& fees, std::uint32_t durationHours)
{
    float const nominal = static_cast<float>(durationHours * SECONDS_PER_HOUR);
    return FloatToCopper(nominal * fees.rates.time);
}

// ---- worth listing (WA.17) ----
// All amounts are for the whole lot in copper; V is what a vendor pays for the lot now (worn gear pays less)
inline double ListingValue(double p, std::uint64_t ask, std::uint64_t cut, std::uint64_t deposit, std::uint64_t vendor)
{
    double const sold = static_cast<double>(ask) - static_cast<double>(cut);
    double const expired = static_cast<double>(vendor) - static_cast<double>(deposit);
    return p * sold + (1.0 - p) * expired;
}

inline bool WorthListing(double p, std::uint64_t ask, std::uint64_t cut, std::uint64_t deposit, std::uint64_t vendor)
{
    return ListingValue(p, ask, cut, deposit, vendor) > static_cast<double>(vendor);
}

// The sale chance at which listing and the vendor are even; infinity when a sale nets no more than the vendor
inline double BreakEvenP(std::uint64_t ask, std::uint64_t cut, std::uint64_t deposit, std::uint64_t vendor)
{
    double const sold = static_cast<double>(ask) - static_cast<double>(cut);
    if (sold <= static_cast<double>(vendor))
        return std::numeric_limits<double>::infinity();
    return static_cast<double>(deposit) / (sold - static_cast<double>(vendor) + static_cast<double>(deposit));
}

constexpr double CUT_FLOAT_MARGIN = 1e-6;  // bounds the binary32 rounding of the cut from above
constexpr std::uint32_t FLOOR_SCAN = 64;   // copper the floor walks down from its safe bound

// The lowest lot ask from which every higher ask is worth listing, NEVER_LISTS when none is. The net of a sale is
// not monotone in the ask by a copper (the cut's two truncations), so the floor starts at a bound every higher ask
// meets and walks down while each ask stays worth listing
inline std::uint64_t ListFloor(Fees const& fees, double p, std::uint32_t sellPrice, std::uint32_t count,
                               std::uint64_t vendor, std::uint32_t hours)
{
    if (!(p > 0.0))
        return NEVER_LISTS;
    std::uint32_t const deposit = Deposit(fees, sellPrice, count, hours * SECONDS_PER_HOUR);
    double const share = static_cast<double>(fees.row.cutPercent) * fees.rates.cut / 100.0 * (1.0 + CUT_FLOAT_MARGIN);
    if (share >= 1.0)
        return NEVER_LISTS;
    // worth listing: p (B - cut) > p V + (1 - p) D, and cut <= B x share
    double const needed = static_cast<double>(vendor) + (1.0 - p) * static_cast<double>(deposit) / p;
    double const bound = std::floor(needed / (1.0 - share)) + 2.0;
    if (bound > static_cast<double>(MAX_LOT_PRICE))
        return NEVER_LISTS;
    auto worth = [&](std::uint64_t ask)
    { return WorthListing(p, ask, Cut(fees, static_cast<std::uint32_t>(ask)), deposit, vendor); };
    std::uint64_t floor = static_cast<std::uint64_t>(bound);
    if (!worth(floor))
        return NEVER_LISTS;
    for (std::uint32_t step = 0; step < FLOOR_SCAN && floor > 1 && worth(floor - 1); ++step)
        --floor;
    return floor;
}

// ---- the ask ----
constexpr std::uint32_t START_BID_PERCENT = 80;  // [E] the start bid's share of the buyout, never below the floor
constexpr std::uint32_t OUTCOME_STEPS_MAX = 64;  // expiries counted for the step down; more change nothing

// ---- the sale chance (PLAN 3.3, AH.4) ----
// Rates are sales (lots) per lot hour listed. A kind never listed sells at the starting rate, chosen so that a listing
// of Chance.EvenHours sells at even odds
inline double StartRate(AuctionConfig const& config)
{
    return std::log(2.0) / static_cast<double>(std::max<std::uint32_t>(config.ChanceEvenHours, 1));
}

// The kind's sales per listing hour, the starting rate standing in for its first Chance.KindPriorHours: a kind nobody
// buys falls, and drifts back to the start as its record decays
inline double KindRate(AuctionIndex::Record const& kind, AuctionConfig const& config)
{
    if (kind.Empty())
        return StartRate(config);
    double const prior = static_cast<double>(std::max<std::uint32_t>(config.ChanceKindPriorHours, 1));
    return (kind.sales + prior * StartRate(config)) / (kind.hours + prior);
}

// The item's, its kind's rate standing in for its first Chance.ItemPriorHours: an item with a long record of its own
// is judged by it, one without by its kind
inline double ItemRate(AuctionIndex::Record const& item, AuctionIndex::Record const& kind, AuctionConfig const& config)
{
    if (item.Empty())
        return KindRate(kind, config);
    double const prior = static_cast<double>(std::max<std::uint32_t>(config.ChanceItemPriorHours, 1));
    return (item.sales + prior * KindRate(kind, config)) / (item.hours + prior);
}

// The chance a listing standing `hours` (its real hours, ExpirySeconds) sells: 1 - e^(-rate x hours). Without any
// record, 1 - 2^(-hours / even hours) in exact steps: 1/2 at 24 h, 3/4 at 48 h
inline double SaleChance(AuctionIndex::Record const& item, AuctionIndex::Record const& kind, double hours,
                         AuctionConfig const& config)
{
    if (!(hours > 0.0))
        return 0.0;
    if (item.Empty() && kind.Empty())
        return 1.0 - std::exp2(-hours / static_cast<double>(std::max<std::uint32_t>(config.ChanceEvenHours, 1)));
    return 1.0 - std::exp(-ItemRate(item, kind, config) * hours);
}

struct AskInput
{
    std::uint32_t count{1};
    std::uint32_t sellPrice{0};     // per unit, the template's
    std::uint64_t vendorPayout{0};  // the lot, V: what a vendor pays for it now
    std::uint64_t keepWorth{0};     // the lot, to the bot
    std::uint64_t reference{0};     // per unit, ItemReference::copper; 0 none
    AuctionIndex::TypicalPrice typical;
    AuctionIndex::Record itemRecord;      // the sale chance's evidence (Snapshot::EvidenceOf)
    AuctionIndex::Record kindRecord;      // the item's kind's
    std::uint32_t cheapestUnitBuyout{0};  // at the auctioneer only; 0 none seen
    std::uint32_t expiredSteps{0};        // the bot's expiries of this item since its last sale
    bool quickSale{false};                // its last listing of the item sold quickly
    Fees fees;
};

struct AskResult
{
    enum class Kind : std::uint8_t
    {
        List,
        Hold
    };
    Kind kind{Kind::Hold};
    AuctionWords::Reason reason{AuctionWords::Reason::BelowFloor};
    std::uint64_t ask{0};  // the lot's buyout; also when holding: what it would ask
    std::uint64_t floor{0};
    std::uint64_t startBid{0};
    std::uint32_t deposit{0};
    std::uint32_t cut{0};  // of the buyout
    std::uint32_t hours{0};
    double p{0.0};
    double expected{0.0};  // ListingValue at the buyout
};

inline std::uint32_t Units(std::uint32_t count) { return std::max<std::uint32_t>(count, 1); }

inline std::uint64_t SellTotal(AskInput const& in)
{
    return static_cast<std::uint64_t>(in.sellPrice) * Units(in.count);
}

// The duration a listing wants: the first one, or the relist one after an expiry
inline std::uint32_t ListingHours(AuctionConfig const& config, std::uint32_t expiredSteps)
{
    return DurationHours(expiredSteps ? config.RelistDurationHours : config.FirstDurationHours);
}

inline std::uint32_t ShorterHours(AuctionConfig const& config)
{
    return std::min(DurationHours(config.FirstDurationHours), DurationHours(config.RelistDurationHours));
}

// The lot's sale chance listed for `hours` (a duration the client offers), at the real hours the core lets it stand
inline double ListingChance(AskInput const& in, AuctionConfig const& config, std::uint32_t hours)
{
    return SaleChance(in.itemRecord, in.kindRecord,
                      static_cast<double>(ExpirySeconds(in.fees, hours)) / static_cast<double>(SECONDS_PER_HOUR),
                      config);
}

// The floor holds at the shorter of the two durations, its chance and deposit, whatever the bot's expiries, so its
// own expiries never lift it; it reads no offers, so offers never lift it either. A listing runs the shorter duration
// when its ask is not worth the longer one's deposit
inline std::uint64_t AskFloor(AskInput const& in, AuctionConfig const& config)
{
    std::uint32_t const hours = ShorterHours(config);
    std::uint64_t const listing =
        ListFloor(in.fees, ListingChance(in, config, hours), in.sellPrice, Units(in.count), in.vendorPayout, hours);
    if (listing == NEVER_LISTS)
        return NEVER_LISTS;
    return std::max({listing, in.keepWorth, SellTotal(in)});
}

// The lot at the typical price, else at the reference value
inline std::uint64_t BaseAsk(AskInput const& in)
{
    std::uint64_t const unit = in.typical.unitPrice ? in.typical.unitPrice : in.reference;
    return std::min(unit * Units(in.count), MAX_LOT_PRICE);
}

// The base; an expiry lowers it by a step, a quick sale raises it by one
inline std::uint64_t EvidenceAsk(AskInput const& in, AuctionConfig const& config)
{
    double lot = static_cast<double>(BaseAsk(in));
    double const step = static_cast<double>(std::min<std::uint32_t>(config.RelistStepPercent, 100)) / 100.0;
    for (std::uint32_t n = std::min(in.expiredSteps, OUTCOME_STEPS_MAX); n; --n)
        lot *= 1.0 - step;
    if (in.quickSale)
        lot *= 1.0 + step;
    return lot >= static_cast<double>(MAX_LOT_PRICE) ? MAX_LOT_PRICE : static_cast<std::uint64_t>(lot);
}

// The cheapest competing buyout less the undercut, for the lot
inline std::uint64_t UndercutAsk(std::uint32_t cheapestUnitBuyout, std::uint32_t count, std::uint32_t percent)
{
    std::uint64_t const lot = static_cast<std::uint64_t>(cheapestUnitBuyout) * Units(count);
    return lot * (100 - std::min<std::uint32_t>(percent, 100)) / 100;
}

// The terms of a decided ask: duration, its sale chance, deposit, cut, start bid and the expected value; it lists
// unless the price or the offers sit below the floor, no ask is worth listing or the item expired more often than the
// relist limit
inline AskResult ListingTerms(AskInput const& in, AuctionConfig const& config, std::uint64_t floor, std::uint64_t ask,
                              bool belowFloor)
{
    AskResult result;
    result.floor = floor;
    result.ask = std::min(ask, MAX_LOT_PRICE);
    std::uint32_t const wanted = ListingHours(config, in.expiredSteps);
    std::uint32_t const shorter = ShorterHours(config);
    bool const wantedPays = wanted == shorter || ListFloor(in.fees, ListingChance(in, config, wanted), in.sellPrice,
                                                           Units(in.count), in.vendorPayout, wanted) <= result.ask;
    result.hours = wantedPays ? wanted : shorter;
    result.p = ListingChance(in, config, result.hours);
    result.deposit = Deposit(in.fees, in.sellPrice, Units(in.count), result.hours * SECONDS_PER_HOUR);
    result.cut = Cut(in.fees, static_cast<std::uint32_t>(result.ask));
    result.expected = ListingValue(result.p, result.ask, result.cut, result.deposit, in.vendorPayout);
    std::uint64_t const share = result.ask * START_BID_PERCENT / 100;
    result.startBid = std::clamp<std::uint64_t>(floor == NEVER_LISTS ? share : std::max(share, floor), 1,
                                                std::max<std::uint64_t>(result.ask, 1));
    if (floor == NEVER_LISTS || belowFloor || result.ask < floor)
        return result;
    if (in.expiredSteps > config.RelistLimit)
    {
        result.reason = AuctionWords::Reason::Expired;
        return result;
    }
    result.kind = AskResult::Kind::List;
    result.reason = in.expiredSteps ? AuctionWords::Reason::Expired : AuctionWords::Reason::AboveVendor;
    return result;
}

// What a bot asks for a lot (PLAN 3.4): the evidence, lowered to undercut the cheapest competing buyout seen, never
// below the floor (worth listing, the keep worth, the vendor sell price); offers below the floor make it hold. After
// an expiry the lot goes lower by a step down to the floor and is held below it, never lifted to it: the records count
// the unsold listing hours, which lower p and lift the floor, so lifting would raise the next ask (WA2 review)
inline AskResult Ask(AskInput const& in, AuctionConfig const& config)
{
    std::uint64_t const floor = AskFloor(in, config);
    std::uint64_t const lowest = floor == NEVER_LISTS ? SellTotal(in) : floor;
    std::uint64_t ask = EvidenceAsk(in, config);
    bool belowFloor = false;
    if (in.cheapestUnitBuyout)
    {
        std::uint64_t const undercut = UndercutAsk(in.cheapestUnitBuyout, in.count, config.UndercutPercent);
        belowFloor = undercut < lowest;
        ask = std::min(ask, undercut);
    }
    if (in.expiredSteps && ask < lowest)
        belowFloor = true;
    return ListingTerms(in, config, floor, std::max(ask, lowest), belowFloor);
}

// ---- what a vendor pays and stock (WA3a, WA.17) ----
// The core's deduction for a worn item (ItemHandler.cpp HandleSellItemOpcode, GPL v2): the lost durability times the
// item level's multiplier for its subclass (a uint32 product) and the quality's factor, rounded up, at least 1
inline std::uint32_t DurabilityDeduction(std::uint32_t lost, std::uint32_t multiplier, float qualityMod)
{
    double const deduction = std::ceil(static_cast<double>(lost * multiplier) * static_cast<double>(qualityMod));
    return std::max<std::uint32_t>(static_cast<std::uint32_t>(deduction), 1);
}

// V: what a vendor pays for count units now, in the core's order: the sell price, less the deduction when the item
// lost durability; a deduction above the price pays 1 copper, an equal one 0
inline std::uint32_t VendorPayout(std::uint32_t sellPrice, std::uint32_t count, std::uint32_t lost,
                                  std::uint32_t multiplier, float qualityMod)
{
    std::uint32_t const money = sellPrice * count;  // the core's uint32
    if (!lost)
        return money;
    std::uint32_t const deduction = DurabilityDeduction(lost, multiplier, qualityMod);
    return deduction > money ? 1 : money - deduction;
}

// What listing a lot as stock is worth (PLAN 3.5): Ask's expected value, rounded down, when its evidence (the typical
// price, else the reference value) is itself worth listing; 0 when the evidence lies below the floor, since Ask then
// asks the floor, a price nothing backs, or when it holds
inline std::uint32_t StockWorth(AskInput const& in, AuctionConfig const& config)
{
    AskResult const result = Ask(in, config);
    if (result.kind != AskResult::Kind::List || EvidenceAsk(in, config) < result.floor || !(result.expected > 0.0))
        return 0;
    return static_cast<std::uint32_t>(
        std::min(std::floor(result.expected), static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

// ---- what a bot pays at most, buyout or bid (PLAN 3.4, 3.6, WA.5) ----
constexpr std::int64_t BID_PATIENCE_SECONDS = 2 * 3600;  // [E] a bid only on an auction that ends this soon
constexpr std::uint32_t LOT_UNITS_MAX = 1u << 20;        // keeps the ranking's cross products in 64 bits

// Worth and getting in the planner's value units (MaterialRules.h: Cost::value = copper x factor); the ceiling is
// the lesser as copper at the bot's need factor, per unit. No use: 0; no way of getting it: the worth alone
struct CeilingInput
{
    double worthValue{0.0};     // what a unit is worth to the bot; 0 no use
    double gettingValue{-1.0};  // a vendor's price x factor, or its own getting cost; < 0 no way
    float factor{0.0f};         // Need.factor
};

inline std::uint64_t Ceiling(CeilingInput const& in)
{
    if (!(in.factor > 0.0f) || !(in.worthValue > 0.0))
        return 0;
    double const value = in.gettingValue >= 0.0 ? std::min(in.worthValue, in.gettingValue) : in.worthValue;
    double const copper = std::floor(value / static_cast<double>(in.factor));
    return copper >= static_cast<double>(MAX_LOT_PRICE) ? MAX_LOT_PRICE : static_cast<std::uint64_t>(copper);
}

// One auction for a line of a need. The caller leaves out the bot's own auctions (AuctionFacts has no owner)
struct OfferInput
{
    AuctionIndex::AuctionFacts line;
    std::uint32_t unitsNeeded{1};
    std::uint64_t excessUnitWorth{0};  // a unit beyond the need: its vendor payout or keep worth
};

struct BuyInput
{
    std::uint64_t ceilingPerUnit{0};
    std::uint64_t spendable{0};  // gold above the reserve, open bids already counted as spent
    std::uint32_t openBids{0};   // the index's auctions whose bidder the bot is
    std::uint32_t openBidsMax{3};
    std::int64_t now{0};
    std::int64_t patienceSeconds{BID_PATIENCE_SECONDS};
    std::uint64_t ownGuid{0};
};

enum class BuyKind : std::uint8_t
{
    Buyout,
    Bid,
    Skip
};

// reason: for a Skip why; for a Bid why not the buyout (no_offer: none); unused for a Buyout
struct BuyChoice
{
    BuyKind kind{BuyKind::Skip};
    std::uint64_t price{0};
    std::uint32_t auctionId{0};
    AuctionWords::Reason reason{AuctionWords::Reason::NoOffer};
};

inline std::uint32_t LotUnits(AuctionIndex::AuctionFacts const& line)
{
    return std::min(Units(line.count), LOT_UNITS_MAX);
}

// The units of the need a lot covers
inline std::uint32_t Covered(OfferInput const& offer)
{
    return std::min(LotUnits(offer.line), Units(offer.unitsNeeded));
}

// What a lot bought whole at price costs the line: the price less what its units beyond the need are worth
inline std::uint64_t LineCost(OfferInput const& offer, std::uint64_t price)
{
    std::uint64_t const beyond = LotUnits(offer.line) - Covered(offer);
    return price - std::min(price, beyond * offer.excessUnitWorth);
}

// The least bid the core takes now: the start bid, or the current bid plus the outbid step
inline std::uint64_t NextBid(AuctionIndex::AuctionFacts const& line)
{
    if (!line.bid)
        return std::max<std::uint32_t>(line.startBid, 1);
    std::uint64_t const raised = static_cast<std::uint64_t>(line.bid) + OutBid(line.bid);
    return std::max<std::uint64_t>(line.startBid, raised);
}

// The price an offer is ranked at: its buyout, else its next bid
inline std::uint64_t RankPrice(AuctionIndex::AuctionFacts const& line)
{
    return line.buyout ? line.buyout : NextBid(line);
}

// [E] Within the ceiling: the line cost at most the ceiling for each unit of the need the lot covers
inline bool WithinCeiling(OfferInput const& offer, std::uint64_t ceilingPerUnit, std::uint64_t price)
{
    return LineCost(offer, price) <= ceilingPerUnit * Covered(offer);
}

// a before b: less line cost per unit of the need covered, then the lower unit price, then the auction id
inline bool RankedBefore(OfferInput const& a, OfferInput const& b)
{
    std::uint64_t const costA = LineCost(a, RankPrice(a.line)) * Covered(b);
    std::uint64_t const costB = LineCost(b, RankPrice(b.line)) * Covered(a);
    if (costA != costB)
        return costA < costB;
    std::uint64_t const unitA = RankPrice(a.line) * LotUnits(b.line);
    std::uint64_t const unitB = RankPrice(b.line) * LotUnits(a.line);
    if (unitA != unitB)
        return unitA < unitB;
    return a.line.id < b.line.id;
}

// At most OFFER_LINES_MAX lines: a sort of a handful
inline std::vector<std::size_t> Rank(std::vector<OfferInput> const& offers)
{
    std::vector<std::size_t> order(offers.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(),
              [&offers](std::size_t a, std::size_t b) { return RankedBefore(offers[a], offers[b]); });
    return order;
}

// One offer: its buyout when within the ceiling and in hand; else a bid when the buyout is missing, over the
// ceiling or not in hand, the auction ends within the patience, a bid slot is free, the next bid is within the
// ceiling and in hand. The full price must be in hand (the core checks it so even for a raise or a leader's
// buyout). A bot that leads an auction does not raise its own bid
inline BuyChoice Consider(OfferInput const& offer, BuyInput const& in)
{
    using AuctionWords::Reason;
    AuctionIndex::AuctionFacts const& line = offer.line;
    BuyChoice choice;
    choice.auctionId = line.id;
    if (line.endTime < in.now)
        return choice;
    bool budget = false;
    if (line.buyout && WithinCeiling(offer, in.ceilingPerUnit, line.buyout))
    {
        if (in.spendable >= line.buyout)
        {
            choice.kind = BuyKind::Buyout;
            choice.price = line.buyout;
            return choice;
        }
        budget = true;
    }
    std::uint64_t const next = NextBid(line);
    bool const biddable = line.bidder != in.ownGuid && (!line.buyout || next < line.buyout) &&
                          line.endTime - in.now <= in.patienceSeconds;
    bool cap = false;
    if (biddable && WithinCeiling(offer, in.ceilingPerUnit, next))
    {
        if (in.openBids >= in.openBidsMax)
            cap = true;
        else if (in.spendable < next)
            budget = true;
        else
        {
            choice.kind = BuyKind::Bid;
            choice.price = next;
            choice.reason = !line.buyout ? Reason::NoOffer : budget ? Reason::Budget : Reason::OverCeiling;
            return choice;
        }
    }
    choice.reason = budget                      ? Reason::Budget
                    : cap                       ? Reason::BidCap
                    : (line.buyout || biddable) ? Reason::OverCeiling
                                                : Reason::NoOffer;
    return choice;
}

// How near a skip came to a purchase: a skip of the market names its nearest
inline int SkipWeight(AuctionWords::Reason reason)
{
    using AuctionWords::Reason;
    return reason == Reason::Budget ? 3 : reason == Reason::BidCap ? 2 : reason == Reason::OverCeiling ? 1 : 0;
}

// The first offer in rank order the bot buys out or bids on; else a skip with the nearest reason
inline BuyChoice ChooseBuy(std::vector<OfferInput> const& offers, BuyInput const& in)
{
    BuyChoice skip;
    bool seen = false;
    for (std::size_t const index : Rank(offers))
    {
        BuyChoice const choice = Consider(offers[index], in);
        if (choice.kind != BuyKind::Skip)
            return choice;
        if (!seen || SkipWeight(choice.reason) > SkipWeight(skip.reason))
            skip = choice;
        seen = true;
    }
    return skip;
}
}  // namespace AuctionRules

#endif
