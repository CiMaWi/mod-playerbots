/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONMARKETSIM_H
#define _PLAYERBOT_AUCTIONMARKETSIM_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AuctionBuyList.h"
#include "AuctionConfig.h"
#include "AuctionIndexRules.h"
#include "AuctionMemory.h"
#include "AuctionPricing.h"
#include "GoalModel.h"
#include "ItemWorthRules.h"
#include "PlannerSnapshot.h"

// The auction market simulation (auction house round 2 AH2, PLAN 3.2, AH.3): model bots trade through the real pure
// headers in rounds of the house's publish interval. Every ask, floor, chance, ceiling and choice is a call into
// src/Ai/World/Rpg (R4); this file adds bots, a clock, the server glue the headers do not hold (each part cites the
// server lines it mirrors), the core's bid handler and settlement, the invariants and the tallies. Its inputs (the
// market) come from a preset: the goods, the bag ladder, per band the visits, the lots that come by and the income,
// and the bots with their purse, reserve, bags and lines. tests/auction-market-selftest.cpp holds the presets, the
// seeded search, the negative controls and the print. What it cannot show: anything the game engine decides (the
// visit, walks, mail, equipping a bag) and how often bots reach an auctioneer; the scenarios show the first, the
// nights the second.
namespace MarketSim
{
constexpr std::int64_t HOUR_SECONDS = 3600;
constexpr std::uint32_t UNKNOWN_CLASS = 255;
constexpr std::size_t OWNER_COUNT = AuctionRules::LINE_OWNER_WORDS.size();

// An item the market knows: its template's class, quality and sell price and its reference value
struct Goods
{
    std::uint32_t item{0};
    std::uint32_t classId{UNKNOWN_CLASS};
    std::uint32_t quality{0};
    std::uint32_t sellPrice{0};  // per unit
    std::uint64_t reference{0};  // per unit, ItemReference::copper; 0 none
    std::uint32_t requiredLevel{0};
    std::uint32_t itemLevel{0};
};

// A bag of the ladder (PlannerUpkeep.cpp BagCandidates): its slots, the template's buy price and whether a spawned
// vendor stocks it (AuctionRules::ItemInput::vendorStocked)
struct BagRung
{
    std::uint32_t item{0};
    std::uint32_t slots{0};
    std::uint32_t buyCopper{0};
    bool stocked{false};
};

// One way a lot comes by: the item, the units of the lot and its share of the band's lots
struct Arrival
{
    std::uint32_t item{0};
    std::uint32_t units{1};
    double weight{1.0};
};

// The inputs of a level band
struct Band
{
    WorldTravel::SlotBand slots;  // the slot band the bag worth reads
    double visitsPerHour{0.0};    // auctioneer visits per bot hour
    double lotsPerHour{0.0};      // lots a bot comes by per hour
    std::vector<Arrival> mix;
    double incomePerHour{0.0};  // copper in from outside the market per bot hour
    double spendPerHour{0.0};   // copper out per bot hour, at most the purse
};

// A lot a bot carries: the item and its units
struct Lot
{
    std::uint32_t item{0};
    std::uint32_t units{0};
};

struct BotSetup
{
    std::uint8_t band{0};
    std::uint64_t money{0};
    std::uint64_t reserve{0};  // what the market leaves in the purse (AuctionVisit.cpp MarketKeptNow)
    std::uint32_t freeSlots{0};
    std::uint32_t smallestBag{0};              // slots of the smallest equipped bag; 0 an empty bag slot
    std::vector<AuctionRules::BuyLine> lines;  // lines of any owner (use, path, gear); bag lines come at each visit
    std::vector<Lot> stock;                    // lots carried at the start
    std::vector<AuctionRules::BuyLine> laterLines;  // lines the bot takes up at its first visit from laterFromHours on
    double laterFromHours{0.0};
    std::uint32_t pathSlot{0};  // the band's path the bot follows (PathSource)
};

// The bots' skill paths (PlannerUpkeep.cpp:1051-1065): Weigh is called with the published snapshot at the start and
// at every model hour (the server keeps a computed path and reweighs it later); Lines are the path's buy lines at a
// visit, AuctionRules::PathLines over the materials with the units the bot holds now
class PathSource
{
public:
    virtual ~PathSource() = default;
    virtual void Weigh(AuctionIndex::Snapshot const& snapshot) = 0;
    virtual std::vector<AuctionRules::BuyLine> Lines(std::uint8_t band, std::uint32_t slot,
                                                     std::function<std::uint32_t(std::uint32_t)> const& held) const = 0;
};

struct Market
{
    std::vector<Goods> goods;
    std::vector<BagRung> ladder;
    std::array<Band, WorldTravel::LEVEL_BAND_COUNT> bands;
    std::vector<BotSetup> bots;
    std::shared_ptr<PathSource> paths;  // empty: no path lines
    AuctionConfig config;
    AuctionRules::Fees fees;  // the shared house's: 5 and 5 % at rates 1
    std::uint8_t houseId{AuctionRules::HOUSE_NEUTRAL};
    double hours{1.0};
    std::uint32_t seed{1};
};

// A broken model for a negative control: the fault sits in the glue, never in a rule
enum class Fault : std::uint8_t
{
    None,
    CutNotPaid,      // a sale pays the seller the price and the deposit without taking the cut
    CeilingIgnored,  // the buy step hands the rule the line without its ceiling
    ExpirySkipped,   // the settlement leaves an unsold auction standing past its end
    KeptIgnored      // the buy step's purse ignores what the market keeps
};

enum class Invariant : std::uint8_t
{
    Copper,   // purses, the house's escrow and deposits, the fees taken and the copper spent outside = the start,
              // the income and the vendors' payouts
    Items,    // bags, the house and what buyers received and vendors took = the units that came by
    Ceiling,  // no purchase or bid above its line's ceiling
    Floor,    // no listing below its floor
    Ends,     // after a settlement every listing stands with its end in the future
    Kept      // no purchase leaves the purse below what the market keeps (AuctionVisit.cpp MarketKeptNow)
};
constexpr std::size_t INVARIANT_COUNT = 6;
constexpr std::array<std::string_view, INVARIANT_COUNT> INVARIANT_WORDS = {"copper", "items", "ceiling",
                                                                           "floor",  "ends",  "kept"};

// Why the core refused a buyout or a bid
enum class Refusal : std::uint8_t
{
    Vanished,  // the auction left the house since the snapshot
    Own,       // the bot's own auction
    Outbid,    // under the house's next bid now
    Money      // the purse lacks it
};
constexpr std::size_t REFUSAL_COUNT = 4;
constexpr std::array<std::string_view, REFUSAL_COUNT> REFUSAL_WORDS = {"vanished", "own", "outbid", "money"};

template <typename Enum>
constexpr std::size_t Slot(Enum value)
{
    return static_cast<std::size_t>(value);
}

// One kind's (item class's) events in one model hour; copper amounts in copper
struct KindTally
{
    std::uint64_t posts{0};
    std::uint64_t relists{0};
    std::uint64_t sales{0};
    std::uint64_t expiries{0};
    std::uint64_t standing{0};  // lots in the house at the hour's end
    std::uint64_t buyouts{0};
    std::uint64_t bids{0};
    std::uint64_t won{0};       // auctions that ended with a bidder (also counted in sales)
    std::uint64_t vendored{0};  // lots a vendor took
    std::uint64_t depositsPaid{0};
    std::uint64_t depositsLost{0};
    std::uint64_t cuts{0};
    std::uint64_t gold{0};  // the sales' prices
    std::array<std::uint64_t, AuctionWords::REASON_COUNT> holds{};
    std::array<std::array<std::uint64_t, AuctionWords::REASON_COUNT>, OWNER_COUNT> skips{};  // by line owner
    std::array<std::uint64_t, REFUSAL_COUNT> refused{};

    void Add(KindTally const& other)
    {
        posts += other.posts;
        relists += other.relists;
        sales += other.sales;
        expiries += other.expiries;
        standing += other.standing;
        buyouts += other.buyouts;
        bids += other.bids;
        won += other.won;
        vendored += other.vendored;
        depositsPaid += other.depositsPaid;
        depositsLost += other.depositsLost;
        cuts += other.cuts;
        gold += other.gold;
        for (std::size_t reason = 0; reason < holds.size(); ++reason)
        {
            holds[reason] += other.holds[reason];
            for (std::size_t owner = 0; owner < OWNER_COUNT; ++owner)
                skips[owner][reason] += other.skips[owner][reason];
        }
        for (std::size_t refusal = 0; refusal < REFUSAL_COUNT; ++refusal)
            refused[refusal] += other.refused[refusal];
    }
};

// One item's listings, sales and buy-list lines left unbought (all owners) over the run
struct ItemTally
{
    std::uint64_t listings{0};
    std::uint64_t sales{0};
    std::array<std::uint64_t, AuctionWords::REASON_COUNT> skips{};
    // By the buyer's level band: its sales, and its path lines' buyouts and bids the core took, those it refused and
    // the lines the buy rule skipped by reason
    std::array<std::uint64_t, WorldTravel::LEVEL_BAND_COUNT> bandSales{};
    std::array<std::uint64_t, WorldTravel::LEVEL_BAND_COUNT> pathPlaced{};
    std::array<std::uint64_t, WorldTravel::LEVEL_BAND_COUNT> pathRefused{};
    std::array<std::array<std::uint64_t, AuctionWords::REASON_COUNT>, WorldTravel::LEVEL_BAND_COUNT> pathSkips{};
    std::array<std::array<std::uint64_t, AuctionWords::REASON_COUNT>, WorldTravel::LEVEL_BAND_COUNT> bagSkips{};
};

// One kind of the sale chance (class, quality and level band, AuctionIndex::KindOf) in one model hour
struct LevelKindTally
{
    std::uint64_t listings{0};
    std::uint64_t sales{0};
    std::uint64_t vendored{0};  // lots a vendor took
    std::uint64_t standing{0};  // lots in the house at the hour's end
};

// An item's typical price in a publication
struct PricePoint
{
    std::uint32_t unitPrice{0};
    AuctionIndex::PriceBasis basis{AuctionIndex::PriceBasis::None};
};

struct HourTally
{
    std::map<std::uint32_t, KindTally> kinds;            // by item class
    std::map<std::uint32_t, LevelKindTally> levelKinds;  // by AuctionIndex::KindKey::code
    std::map<std::uint32_t, PricePoint> prices;          // the market's priced items at the hour's end, by item
};

struct Result
{
    std::array<std::uint64_t, INVARIANT_COUNT> violations{};
    std::array<std::string, INVARIANT_COUNT> first;  // the first violation of each, for the report
    std::vector<HourTally> hours;
    std::map<std::uint32_t, std::uint64_t> houseLots;  // by item class, at the end
    std::uint64_t unitsStart{0};                       // carried at the start
    std::uint64_t lotsCome{0};
    std::uint64_t unitsCome{0};
    std::uint64_t unitsVendored{0};
    std::uint64_t injected{0};      // income from outside the market
    std::uint64_t spent{0};         // copper out of the market, what the purses held
    std::uint64_t vendorIncome{0};  // vendors' payouts for lots that were no stock
    std::uint64_t moneyStart{0};
    std::uint64_t moneyEnd{0};
    std::uint64_t visits{0};
    std::uint64_t rounds{0};
    std::uint64_t publishes{0};
    std::array<std::uint64_t, WorldTravel::LEVEL_BAND_COUNT> bandVisits{};
    std::array<std::uint64_t, WorldTravel::LEVEL_BAND_COUNT> bandListings{};  // by the seller's band
    std::array<std::uint64_t, WorldTravel::LEVEL_BAND_COUNT> bandBuys{};      // sales by the buyer's band
    std::map<std::uint32_t, ItemTally> items;                                 // by item, over the run
    // At the end, by KindKey::code: each standing lot's buyout over the vendor price (sell price x units), the
    // standing lots without a sell price, and the kind's record in the last publication
    std::map<std::uint32_t, std::vector<double>> endAsks;
    std::map<std::uint32_t, std::uint64_t> endUnpriced;
    std::map<std::uint32_t, AuctionIndex::Record> endRecords;
    std::uint64_t gearUnitsWanted{0};  // units of the gear lines the bots held or took up
    std::uint64_t gearUnitsLeft{0};    // units of them unbought at the end

    bool Clean() const
    {
        return std::all_of(violations.begin(), violations.end(), [](std::uint64_t count) { return count == 0; });
    }

    // Each kind over the run; standing is summed over the hours' ends, the end's house is houseLots
    std::map<std::uint32_t, KindTally> ByKind() const
    {
        std::map<std::uint32_t, KindTally> kinds;
        for (HourTally const& hour : hours)
            for (auto const& [kind, tally] : hour.kinds)
                kinds[kind].Add(tally);
        return kinds;
    }

    KindTally Total() const
    {
        KindTally total;
        for (auto const& [kind, tally] : ByKind())
            total.Add(tally);
        return total;
    }
};

class Sim
{
public:
    explicit Sim(Market const& market, Fault fault = Fault::None) : _market(market), _fault(fault), _rng(market.seed)
    {
        for (Goods const& goods : market.goods)
            _goods[goods.item] = goods;
        for (std::size_t band = 0; band < market.bands.size(); ++band)
        {
            double sum = 0.0;
            for (Arrival const& arrival : market.bands[band].mix)
                _mixSums[band].push_back(sum += std::max(arrival.weight, 0.0));
        }
        for (BotSetup const& bot : market.bots)
        {
            Bot model;
            model.band = std::min<std::uint8_t>(bot.band, WorldTravel::LEVEL_BAND_COUNT - 1);
            model.money = bot.money;
            model.reserve = bot.reserve;
            model.freeSlots = bot.freeSlots;
            model.smallestBag = bot.smallestBag;
            model.lines = bot.lines;
            model.stock = bot.stock;
            model.pathSlot = bot.pathSlot;
            model.laterLines = bot.laterLines;
            model.laterFrom = bot.laterFromHours * static_cast<double>(HOUR_SECONDS);
            _result.gearUnitsWanted += GearUnits(bot.lines);
            for (Lot const& lot : bot.stock)
                _result.unitsStart += lot.units;
            _bots.push_back(std::move(model));
            _result.moneyStart += bot.money;
        }
    }

    Result Run()
    {
        std::int64_t const end = std::max<std::int64_t>(std::llround(_market.hours * HOUR_SECONDS), 1);
        _end = static_cast<double>(end);
        _result.hours.resize(static_cast<std::size_t>((end + HOUR_SECONDS - 1) / HOUR_SECONDS));
        for (std::uint32_t index = 0; index < _bots.size(); ++index)
        {
            Schedule(index, false, 0.0);
            Schedule(index, true, 0.0);
        }
        Publish(0);
        WeighPaths();
        // A round is the house's publish interval: settle the ends, audit, the bots' events in time order (buyers
        // see the round's stale snapshot), publish
        std::int64_t const interval = std::max<std::int64_t>(_market.config.PublishSeconds, 1);
        std::int64_t settled = 0;
        for (std::int64_t start = 0; start < end; start += interval)
        {
            Settle(start);
            settled = start;
            Audit(settled);
            std::int64_t const roundEnd = std::min(start + interval, end);
            while (!_events.empty() && _events.top().at < static_cast<double>(roundEnd))
            {
                Event const next = _events.top();
                _events.pop();
                std::int64_t const now = static_cast<std::int64_t>(next.at);
                Flow(_bots[next.bot], next.at);
                if (next.visit)
                    Visit(next.bot, now);
                else
                    Arrive(next.bot, now);
                Schedule(next.bot, next.visit, next.at);
            }
            Publish(roundEnd);
            ++_result.rounds;
            if (roundEnd / HOUR_SECONDS != start / HOUR_SECONDS || roundEnd == end)
            {
                Standing(start / HOUR_SECONDS);
                if (roundEnd < end)
                    WeighPaths();
            }
        }
        for (Bot& bot : _bots)
            Flow(bot, _end);
        Audit(settled);
        for (Bot const& bot : _bots)
        {
            _result.moneyEnd += bot.money;
            _result.gearUnitsLeft += GearUnits(bot.lines);
        }
        for (auto const& [id, auction] : _house)
        {
            ++_result.houseLots[ClassOf(auction.facts.item)];
            std::uint32_t const kind = LevelKindOf(auction.facts.item).code;
            double const vendor = static_cast<double>(GoodsOf(auction.facts.item).sellPrice) *
                                  std::max<std::uint32_t>(auction.facts.count, 1);
            if (vendor > 0.0)
                _result.endAsks[kind].push_back(static_cast<double>(auction.facts.buyout) / vendor);
            else
                ++_result.endUnpriced[kind];
        }
        for (HourTally const& hour : _result.hours)
            for (auto const& [kind, tally] : hour.levelKinds)
                _result.endRecords[kind] = _snapshot.KindRecord({kind});
        return _result;
    }

private:
    struct Bot
    {
        std::uint8_t band{0};
        std::uint64_t money{0};
        std::uint64_t reserve{0};
        std::uint32_t freeSlots{0};
        std::uint32_t smallestBag{0};
        std::vector<AuctionRules::BuyLine> lines;
        std::vector<Lot> stock;  // lots the bot carries to sell
        std::uint32_t pathSlot{0};
        std::vector<AuctionRules::BuyLine> laterLines;
        double laterFrom{0.0};  // model seconds
        AuctionMemory::SellerMemory memory;
        std::unordered_map<std::uint32_t, std::uint32_t> received;  // units bought and received, by item
        std::uint64_t owned{0};                                     // units bought and received
        std::uint64_t incomeDue{0};                                 // income applied so far
        std::uint64_t spendingDue{0};                               // spending applied so far
    };

    // The house's own record of an auction (the core's AuctionEntry): its facts with the live bid and bidder, the
    // seller and the deposit, and the ask that listed it
    struct Auction
    {
        AuctionIndex::AuctionFacts facts;
        std::uint32_t seller{0};
        std::uint32_t deposit{0};
        AuctionRules::AskResult ask;
    };

    struct Event
    {
        double at{0.0};  // model seconds
        std::uint64_t order{0};
        std::uint32_t bot{0};
        bool visit{false};

        bool operator>(Event const& other) const { return at != other.at ? at > other.at : order > other.order; }
    };

    using Ending = std::pair<std::int64_t, std::uint32_t>;  // end time, auction id

    static std::uint64_t Guid(std::uint32_t index) { return static_cast<std::uint64_t>(index) + 1; }

    static std::uint32_t BotOf(std::uint64_t guid) { return static_cast<std::uint32_t>(guid - 1); }

    static std::uint64_t GearUnits(std::vector<AuctionRules::BuyLine> const& lines)
    {
        std::uint64_t units = 0;
        for (AuctionRules::BuyLine const& line : lines)
            if (line.owner == AuctionRules::LineOwner::Gear)
                units += line.units;
        return units;
    }

    // The core's money fields are uint32 (AuctionVisit.cpp Clamped)
    static std::uint32_t Copper32(std::uint64_t copper)
    {
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(copper, std::numeric_limits<std::uint32_t>::max()));
    }

    Goods const& GoodsOf(std::uint32_t item) const
    {
        auto const found = _goods.find(item);
        return found == _goods.end() ? _unknown : found->second;
    }

    std::uint32_t ClassOf(std::uint32_t item) const { return GoodsOf(item).classId; }

    // The item's kind of the sale chance, from its template values as the server's callers resolve it
    AuctionIndex::KindKey LevelKindOf(std::uint32_t item) const
    {
        Goods const& goods = GoodsOf(item);
        if (goods.classId == UNKNOWN_CLASS)
            return {};
        return AuctionIndex::KindOf(goods.classId, goods.quality, goods.requiredLevel, goods.itemLevel);
    }

    HourTally& HourAt(std::int64_t now)
    {
        std::size_t const hour =
            std::min(static_cast<std::size_t>(std::max<std::int64_t>(now, 0) / HOUR_SECONDS), _result.hours.size() - 1);
        return _result.hours[hour];
    }

    KindTally& Tally(std::int64_t now, std::uint32_t item) { return HourAt(now).kinds[ClassOf(item)]; }

    LevelKindTally& LevelTally(std::int64_t now, std::uint32_t item)
    {
        return HourAt(now).levelKinds[LevelKindOf(item).code];
    }

    void Violate(Invariant invariant, std::string const& what)
    {
        if (!_result.violations[Slot(invariant)]++)
            _result.first[Slot(invariant)] = what;
    }

    double Uniform() { return (static_cast<double>(_rng()) + 0.5) / 4294967296.0; }

    // The next visit or lot of a bot: a Poisson process at its band's rate (model input, not a rule)
    void Schedule(std::uint32_t index, bool visit, double from)
    {
        Band const& band = _market.bands[_bots[index].band];
        double const rate = visit ? band.visitsPerHour : band.lotsPerHour;
        if (!(rate > 0.0) || (!visit && _mixSums[_bots[index].band].empty()))
            return;
        double const at = from - std::log(Uniform()) / rate * static_cast<double>(HOUR_SECONDS);
        if (at < _end)
            _events.push({at, ++_order, index, visit});
    }

    // The band's income and spending up to the moment; spending takes at most the purse and counts what it took
    void Flow(Bot& bot, double at)
    {
        Band const& band = _market.bands[bot.band];
        double const hours = at / static_cast<double>(HOUR_SECONDS);
        auto const income = static_cast<std::uint64_t>(std::floor(std::max(band.incomePerHour, 0.0) * hours));
        if (income > bot.incomeDue)
        {
            bot.money += income - bot.incomeDue;
            _result.injected += income - bot.incomeDue;
            bot.incomeDue = income;
        }
        auto const spending = static_cast<std::uint64_t>(std::floor(std::max(band.spendPerHour, 0.0) * hours));
        if (spending > bot.spendingDue)
        {
            std::uint64_t const took = std::min(bot.money, spending - bot.spendingDue);
            bot.money -= took;
            _result.spent += took;
            bot.spendingDue = spending;
        }
    }

    // A lot comes by; the bags' plan decides at once whether it is stock or a vendor's
    void Arrive(std::uint32_t index, std::int64_t now)
    {
        Bot& bot = _bots[index];
        std::vector<double> const& sums = _mixSums[bot.band];
        double const pick = Uniform() * sums.back();
        std::size_t const at = std::min<std::size_t>(
            static_cast<std::size_t>(std::upper_bound(sums.begin(), sums.end(), pick) - sums.begin()), sums.size() - 1);
        Arrival const& arrival = _market.bands[bot.band].mix[at];
        Lot const lot{arrival.item, std::max<std::uint32_t>(arrival.units, 1)};
        ++_result.lotsCome;
        _result.unitsCome += lot.units;
        if (Stock(bot, lot))
            bot.stock.push_back(lot);
        else
            Vendor(bot, lot, now);
    }

    // The ask's input as the server builds it (AuctionVisit.cpp AskFor at the auctioneer; BankRulesInput.cpp
    // ReadAuction away, without the offers): the lot, the template's sell price, the vendor's payout, the reference
    // value, the published evidence (typical price, the item's record and its kind's), the seller's memory, the
    // house's fees
    AuctionRules::AskInput AskOf(Bot const& bot, Lot const& lot, bool atAuctioneer) const
    {
        Goods const& goods = GoodsOf(lot.item);
        AuctionRules::AskInput in;
        in.count = std::max<std::uint32_t>(lot.units, 1);
        in.sellPrice = goods.sellPrice;
        in.vendorPayout = AuctionRules::VendorPayout(goods.sellPrice, in.count, 0, 0, 1.0f);
        in.reference = goods.reference;
        AuctionIndex::Evidence const evidence = _snapshot.EvidenceOf(lot.item, LevelKindOf(lot.item));
        in.typical = evidence.typical;
        in.itemRecord = evidence.item;
        in.kindRecord = evidence.kind;
        if (atAuctioneer)
            in.cheapestUnitBuyout = evidence.cheapestUnitBuyout;
        AuctionMemory::ItemMemory const memory = bot.memory.Of(lot.item);
        in.expiredSteps = memory.expiredSteps;
        in.quickSale = memory.quickSale;
        in.fees = _market.fees;
        return in;
    }

    // BankRulesInput.cpp ReadAuction: away from the auctioneer a lot is stock while listing it is worth more than what
    // a vendor pays; else it is the vendor's
    bool Stock(Bot const& bot, Lot const& lot) const
    {
        AuctionRules::AskInput const in = AskOf(bot, lot, false);
        return AuctionRules::StockWorth(in, _market.config) > in.vendorPayout;
    }

    void Vendor(Bot& bot, Lot const& lot, std::int64_t now)
    {
        std::uint32_t const payout =
            AuctionRules::VendorPayout(GoodsOf(lot.item).sellPrice, std::max<std::uint32_t>(lot.units, 1), 0, 0, 1.0f);
        bot.money += payout;
        _result.vendorIncome += payout;
        _result.unitsVendored += lot.units;
        ++Tally(now, lot.item).vendored;
        ++LevelTally(now, lot.item).vendored;
    }

    // The bags' plan over the whole stock after a visit or a returned lot: what is no stock any more goes to a vendor
    void Away(Bot& bot, std::int64_t now)
    {
        std::vector<Lot> remaining;
        for (Lot const& lot : bot.stock)
        {
            if (Stock(bot, lot))
                remaining.push_back(lot);
            else
                Vendor(bot, lot, now);
        }
        bot.stock = std::move(remaining);
    }

    // AuctionVisit.cpp ListNext then BuyNext: each stock lot asked once, a hold with the ask's reason (the deposit
    // above the purse: budget) marks the entry's other lots tried (MarkEntryTried: one hold and one expiry step per
    // entry and visit), then the buy list; afterwards the bags' plan
    void Visit(std::uint32_t index, std::int64_t now)
    {
        Bot& bot = _bots[index];
        ++_result.visits;
        ++_result.bandVisits[bot.band];
        std::vector<Lot> remaining;
        std::vector<std::uint32_t> heldEntries;
        for (Lot const& lot : bot.stock)
        {
            if (std::find(heldEntries.begin(), heldEntries.end(), lot.item) != heldEntries.end())
            {
                remaining.push_back(lot);
                continue;
            }
            AuctionRules::AskResult const ask = AuctionRules::Ask(AskOf(bot, lot, true), _market.config);
            bool const lists = lot.units && ask.kind == AuctionRules::AskResult::Kind::List;
            if (!lists || bot.money < ask.deposit)
            {
                ++Tally(now, lot.item).holds[Slot(lists ? AuctionWords::Reason::Budget : ask.reason)];
                bot.memory.Held(lot.item);
                heldEntries.push_back(lot.item);
                remaining.push_back(lot);
                continue;
            }
            List(index, lot, ask, now);
        }
        bot.stock = std::move(remaining);
        Buy(index, now);
        Away(bot, now);
    }

    // The core lists the lot for the ask's duration scaled by the time rate (ExpirySeconds) and takes the deposit;
    // the index's hook adds it under its kind and the seller's memory notes it (AuctionVisit.cpp Listed)
    void List(std::uint32_t index, Lot const& lot, AuctionRules::AskResult const& ask, std::int64_t now)
    {
        Bot& bot = _bots[index];
        if (ask.ask < ask.floor || !(ask.floor < AuctionRules::NEVER_LISTS))
            Violate(Invariant::Floor, "item " + std::to_string(lot.item) + " listed at " + std::to_string(ask.ask) +
                                          " under its floor " + std::to_string(ask.floor));
        AuctionIndex::AuctionFacts facts;
        facts.id = ++_lastId;
        facts.item = lot.item;
        facts.count = lot.units;
        facts.startBid = Copper32(ask.startBid);
        facts.buyout = Copper32(ask.ask);
        facts.endTime = now + AuctionRules::ExpirySeconds(_market.fees, ask.hours);
        facts.houseId = _market.houseId;
        facts.added = now;
        bool const relist = bot.memory.Of(lot.item).expiredSteps != 0;
        bot.money -= ask.deposit;
        _house[facts.id] = {facts, index, ask.deposit, ask};
        _book.Add(facts, LevelKindOf(lot.item));
        bot.memory.Listed(facts.id, now, facts.endTime);
        _endings.push({facts.endTime, facts.id});
        KindTally& tally = Tally(now, lot.item);
        ++(relist ? tally.relists : tally.posts);
        ++_result.items[lot.item].listings;
        ++LevelTally(now, lot.item).listings;
        tally.depositsPaid += ask.deposit;
        ++_result.bandListings[bot.band];
    }

    // PlannerUpkeep.cpp CopperWeightOf: the need factor at the purse now against the bot's reserve (the market's kept
    // copper stands in for upkeep's reserve), neutral traits, the band's top level
    static AuctionRules::CopperWeight WeightOf(Bot const& bot)
    {
        Planner::Snapshot snapshot;
        snapshot.level = static_cast<std::uint8_t>(WorldTravel::LEVEL_BAND_TOP[bot.band]);
        snapshot.money =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(bot.money, std::numeric_limits<std::uint32_t>::max()));
        Planner::Goals::Need const need = Planner::Goals::NeedFor(snapshot, Planner::Goals::Weights{}, bot.reserve);
        return {need.exchange, need.factor};
    }

    // A setup's use line holds copper at the reserve's share (built with CopperWeight{}): priced again at the bot's
    // weight now, as PlannerUpkeep.cpp BuyListOf builds it at every visit
    static AuctionRules::BuyLine AtWeight(AuctionRules::BuyLine const& line, AuctionRules::CopperWeight weight)
    {
        double const vendor = line.ceiling.gettingValue;
        return AuctionRules::UseLine(line.item, line.units, static_cast<std::uint32_t>(line.ceiling.worthValue),
                                     vendor > 0.0 ? static_cast<std::uint32_t>(vendor) : 0,
                                     static_cast<std::uint32_t>(line.excessUnitWorth), weight);
    }

    // PlannerUpkeep.cpp BuyListOf: the ladder's bags the index knows that add slots, each by its worth to the bot at
    // its weight, the ceiling at most the price of a vendor that stocks the bag
    void AddBagLines(Bot const& bot, AuctionRules::CopperWeight weight, std::vector<AuctionRules::BuyLine>& lines) const
    {
        WorldTravel::SlotBand const& band = _market.bands[bot.band].slots;
        WorldTravel::SlotRates const rates = WorldTravel::CurrentRates(WorldTravel::SlotMeasure{}, band);
        for (BagRung const& rung : _market.ladder)
        {
            if (rung.slots <= bot.smallestBag || !_snapshot.Find(rung.item))
                continue;
            std::uint32_t const worth =
                WorldTravel::BagWorth(rates, bot.freeSlots, rung.slots, bot.smallestBag, band.BagHorizonHours);
            if (worth)
                lines.push_back(AuctionRules::BagLine(rung.item, worth, rung.stocked ? rung.buyCopper : 0, weight));
        }
    }

    // The control's fault: the glue hands the rule a copy of the line without its ceiling
    static AuctionRules::BuyLine Unbounded(AuctionRules::BuyLine line)
    {
        line.ceiling.worthValue = static_cast<double>(AuctionRules::MAX_LOT_PRICE) * 4.0;
        line.ceiling.gettingValue = -1.0;
        line.ceiling.factor = 1.0f;
        return line;
    }

    // AuctionVisit.cpp BuyNext and AwaitPurchase: the buy list once per visit at the published offers and the bot's
    // weight then, one decision per item, one bag per visit, the purse re-read at every line above the reserve, the
    // house's offers without leaving out the bot's own (the core refuses them); a bid the core took counts as open
    void Buy(std::uint32_t index, std::int64_t now)
    {
        Bot& bot = _bots[index];
        if (!bot.laterLines.empty() && static_cast<double>(now) >= bot.laterFrom)
        {
            _result.gearUnitsWanted += GearUnits(bot.laterLines);
            bot.lines.insert(bot.lines.end(), bot.laterLines.begin(), bot.laterLines.end());
            bot.laterLines.clear();
        }
        AuctionRules::CopperWeight const weight = WeightOf(bot);
        std::vector<AuctionRules::BuyLine> lines;
        for (AuctionRules::BuyLine const& line : bot.lines)
            lines.push_back(line.owner == AuctionRules::LineOwner::Use ? AtWeight(line, weight) : line);
        AddBagLines(bot, weight, lines);
        if (_market.paths)
        {
            auto const held = [&bot](std::uint32_t item)
            {
                auto const found = bot.received.find(item);
                return found == bot.received.end() ? 0u : found->second;
            };
            for (AuctionRules::BuyLine const& line : _market.paths->Lines(bot.band, bot.pathSlot, held))
                lines.push_back(line);
        }
        lines = AuctionRules::BuyList(std::move(lines));
        std::uint64_t const guid = Guid(index);
        std::uint32_t openBids = AuctionRules::OpenBids(_snapshot, guid);
        std::vector<std::uint64_t> decided;
        bool bagTaken = false;
        AuctionIndex::CurrentOffers const none;
        for (AuctionRules::BuyLine const& line : lines)
        {
            std::uint64_t const key = AuctionRules::DecidedKey(line);
            if (std::find(decided.begin(), decided.end(), key) != decided.end())
                continue;
            decided.push_back(key);
            bool const bag = line.owner == AuctionRules::LineOwner::Bag;
            if (bag && bagTaken)
                continue;
            AuctionRules::BuyInput buyer;
            buyer.spendable = AuctionRules::Spendable(bot.money, _fault == Fault::KeptIgnored ? 0 : bot.reserve, 0);
            buyer.openBids = openBids;
            buyer.openBidsMax = _market.config.OpenBidsMax;
            buyer.now = now;
            buyer.ownGuid = guid;
            AuctionIndex::ItemEntry const* entry = _snapshot.Find(line.item);
            AuctionIndex::CurrentOffers const& offers = entry ? entry->offers : none;
            AuctionRules::BuyChoice const choice = AuctionRules::ChooseLine(
                offers, _fault == Fault::CeilingIgnored ? Unbounded(line) : line, _market.houseId, {}, buyer);
            if (choice.kind == AuctionRules::BuyKind::Skip)
            {
                ++Tally(now, line.item).skips[Slot(line.owner)][Slot(choice.reason)];
                ++_result.items[line.item].skips[Slot(choice.reason)];
                if (line.owner == AuctionRules::LineOwner::Path)
                    ++_result.items[line.item].pathSkips[bot.band][Slot(choice.reason)];
                if (bag)
                    ++_result.items[line.item].bagSkips[bot.band][Slot(choice.reason)];
                continue;
            }
            bool const placed = Place(index, line, choice, now);
            if (line.owner == AuctionRules::LineOwner::Path)
            {
                ItemTally& item = _result.items[line.item];
                ++(placed ? item.pathPlaced : item.pathRefused)[bot.band];
            }
            if (!placed)
                continue;
            bagTaken = bagTaken || bag;
            if (choice.kind == AuctionRules::BuyKind::Bid)
                ++openBids;
        }
    }

    bool Refuse(std::int64_t now, std::uint32_t item, Refusal refusal)
    {
        ++Tally(now, item).refused[Slot(refusal)];
        return false;
    }

    // The core's bid handler (AuctionHouseHandler.cpp HandleAuctionPlaceBid): refused when the auction left the house,
    // is the bot's own, the price is under the next bid and not the buyout, or the purse lacks it; a leader pays the
    // raise, an outbid bidder gets its bid back (the core mails it), a price at the buyout ends it as a sale
    bool Place(std::uint32_t index, AuctionRules::BuyLine const& line, AuctionRules::BuyChoice const& choice,
               std::int64_t now)
    {
        Bot& bot = _bots[index];
        auto const found = _house.find(choice.auctionId);
        if (found == _house.end())
            return Refuse(now, line.item, Refusal::Vanished);
        Auction& auction = found->second;
        if (auction.seller == index)
            return Refuse(now, line.item, Refusal::Own);
        std::uint64_t const price = choice.price;
        bool const buyout = auction.facts.buyout && price >= auction.facts.buyout;
        if (!buyout && price < AuctionRules::NextBid(auction.facts))
            return Refuse(now, line.item, Refusal::Outbid);
        std::uint64_t const guid = Guid(index);
        bool const leads = auction.facts.bidder == guid;
        std::uint64_t const due = leads ? price - auction.facts.bid : price;
        if (bot.money < due)
            return Refuse(now, line.item, Refusal::Money);
        AuctionRules::OfferInput const offer{auction.facts, line.units, line.excessUnitWorth};
        if (AuctionRules::LineCost(offer, price) > AuctionRules::CeilingOf(line) * AuctionRules::Covered(offer))
            Violate(Invariant::Ceiling, "item " + std::to_string(line.item) + " " +
                                            std::string(AuctionRules::OwnerWord(line.owner)) + " line paid " +
                                            std::to_string(price) + " over its ceiling " +
                                            std::to_string(AuctionRules::CeilingOf(line)) + " per unit");
        bot.money -= due;
        if (bot.money < bot.reserve)
            Violate(Invariant::Kept, "item " + std::to_string(line.item) + " paid " + std::to_string(due) +
                                         " and left " + std::to_string(bot.money) + " under the kept " +
                                         std::to_string(bot.reserve));
        if (!leads && auction.facts.bidder)
            _bots[BotOf(auction.facts.bidder)].money += auction.facts.bid;
        auction.facts.bid = Copper32(price);
        auction.facts.bidder = guid;
        KindTally& tally = Tally(now, line.item);
        if (buyout)
        {
            ++tally.buyouts;
            Sell(choice.auctionId, now);
        }
        else
            ++tally.bids;
        return true;
    }

    // A sale at the auction's bid (the core sets the bid to the buyout): the seller gets the bid and the deposit less
    // the cut, the buyer the lot; the index's hooks record the sale and remove it (AuctionVisit.cpp Sold)
    void Sell(std::uint32_t id, std::int64_t now)
    {
        auto const found = _house.find(id);
        Auction const auction = found->second;
        _house.erase(found);
        std::uint32_t const price = auction.facts.bid;
        std::uint32_t const cut = AuctionRules::Cut(_market.fees, price);
        Bot& seller = _bots[auction.seller];
        seller.money += static_cast<std::uint64_t>(price) + auction.deposit - (_fault == Fault::CutNotPaid ? 0 : cut);
        _feesKept += cut;
        _book.Sale(auction.facts, now);
        _book.Remove(id);
        seller.memory.Sold(id, auction.facts.item, now);
        Bot& buyer = _bots[BotOf(auction.facts.bidder)];
        ++_result.bandBuys[buyer.band];
        Deliver(buyer, auction.facts.item, auction.facts.count);
        KindTally& tally = Tally(now, auction.facts.item);
        ++tally.sales;
        tally.gold += price;
        ++_result.items[auction.facts.item].sales;
        ++_result.items[auction.facts.item].bandSales[buyer.band];
        ++LevelTally(now, auction.facts.item).sales;
        tally.cuts += cut;
    }

    // An unsold end: the house keeps the deposit, the lot goes back to the seller (the core mails it), the hooks
    // record the expiry and remove it (AuctionVisit.cpp Expired)
    void Expire(std::uint32_t id, std::int64_t now)
    {
        auto const found = _house.find(id);
        Auction const auction = found->second;
        _house.erase(found);
        _feesKept += auction.deposit;
        _book.Expire(auction.facts, now);
        _book.Remove(id);
        Bot& seller = _bots[auction.seller];
        seller.memory.Expired(id, auction.facts.item);
        seller.stock.push_back({auction.facts.item, auction.facts.count});
        KindTally& tally = Tally(now, auction.facts.item);
        ++tally.expiries;
        tally.depositsLost += auction.deposit;
        Away(seller, now);
    }

    // The buyer receives the lot: its lines of the item shrink by the units; a bag that adds slots replaces the
    // smallest one (assumed: the other bags are not smaller; equipping is the engine's)
    void Deliver(Bot& bot, std::uint32_t item, std::uint32_t units)
    {
        bot.owned += units;
        bot.received[item] += units;
        std::uint32_t left = units;
        for (AuctionRules::BuyLine& line : bot.lines)
        {
            if (line.item != item || !left)
                continue;
            std::uint32_t const taken = std::min(left, line.units);
            line.units -= taken;
            left -= taken;
        }
        std::erase_if(bot.lines, [](AuctionRules::BuyLine const& line) { return !line.units; });
        for (BagRung const& rung : _market.ladder)
            if (rung.item == item && rung.slots > bot.smallestBag)
            {
                bot.freeSlots += rung.slots - bot.smallestBag;
                bot.smallestBag = rung.slots;
            }
    }

    // The core's expiry pass: an auction at its end is a sale when it has a bidder, else an expiry
    void Settle(std::int64_t now)
    {
        while (!_endings.empty() && _endings.top().first <= now)
        {
            std::uint32_t const id = _endings.top().second;
            _endings.pop();
            auto const found = _house.find(id);
            if (found == _house.end())
                continue;
            if (found->second.facts.bidder)
            {
                ++Tally(now, found->second.facts.item).won;
                Sell(id, now);
                continue;
            }
            if (_fault == Fault::ExpirySkipped)
                continue;
            Expire(id, now);
        }
    }

    // AuctionIndex.cpp Publish: the alive auctions' bid and bidder, the standing prices (AH8), the history's window, a
    // new snapshot; and the listing hours of the lots standing for the model time since the last publication (the
    // server's own from AH6)
    void Publish(std::int64_t now)
    {
        _book.Refresh(
            [this](AuctionIndex::OfferLine& line)
            {
                auto const found = _house.find(line.id);
                if (found == _house.end())
                    return false;
                line.bid = found->second.facts.bid;
                line.bidder = found->second.facts.bidder;
                return true;
            });
        std::int64_t const window =
            static_cast<std::int64_t>(_market.config.HistoryDays) * AuctionIndex::SECONDS_PER_DAY;
        _book.TakeStanding(now, static_cast<std::int64_t>(_market.config.StandingSeconds));
        _book.Prune(now, window);
        _book.AccrueListingHours(now, now - _publishedAt, window);
        _publishedAt = now;
        _snapshot = AuctionIndex::Snapshot{};
        _book.Fill(_snapshot, now, window);
        _snapshot.sequence = ++_result.publishes;
    }

    void WeighPaths()
    {
        if (_market.paths)
            _market.paths->Weigh(_snapshot);
    }

    void Standing(std::int64_t hour)
    {
        HourTally& tally = _result.hours[std::min(static_cast<std::size_t>(hour), _result.hours.size() - 1)];
        for (auto const& [id, auction] : _house)
        {
            ++tally.kinds[ClassOf(auction.facts.item)].standing;
            ++tally.levelKinds[LevelKindOf(auction.facts.item).code].standing;
        }
        for (auto const& [item, goods] : _goods)
            if (AuctionIndex::ItemEntry const* entry = _snapshot.Find(item); entry && entry->typical.unitPrice)
                tally.prices[item] = {entry->typical.unitPrice, entry->typical.basis};
    }

    // The invariants over the whole market after a settlement
    void Audit(std::int64_t settled)
    {
        std::uint64_t escrow = 0;
        std::uint64_t deposits = 0;
        std::uint64_t listed = 0;
        for (auto const& [id, auction] : _house)
        {
            if (auction.facts.endTime <= settled)
                Violate(Invariant::Ends, "auction " + std::to_string(id) + " ended at " +
                                             std::to_string(auction.facts.endTime) + " s and stands at " +
                                             std::to_string(settled) + " s");
            escrow += auction.facts.bidder ? auction.facts.bid : 0;
            deposits += auction.deposit;
            listed += auction.facts.count;
        }
        std::uint64_t purses = 0;
        std::uint64_t carried = 0;
        std::uint64_t owned = 0;
        for (Bot const& bot : _bots)
        {
            purses += bot.money;
            owned += bot.owned;
            for (Lot const& lot : bot.stock)
                carried += lot.units;
        }
        std::uint64_t const held = purses + escrow + deposits + _feesKept + _result.spent;
        std::uint64_t const entered = _result.moneyStart + _result.injected + _result.vendorIncome;
        if (held != entered)
            Violate(Invariant::Copper, "at " + std::to_string(settled) + " s the market holds " + std::to_string(held) +
                                           " copper of " + std::to_string(entered));
        if (carried + listed + owned + _result.unitsVendored != _result.unitsStart + _result.unitsCome)
            Violate(Invariant::Items, "at " + std::to_string(settled) + " s " +
                                          std::to_string(carried + listed + owned + _result.unitsVendored) +
                                          " units of " + std::to_string(_result.unitsStart + _result.unitsCome));
    }

    Market const& _market;
    Fault _fault;
    std::mt19937 _rng;
    std::unordered_map<std::uint32_t, Goods> _goods;
    Goods _unknown;
    std::array<std::vector<double>, WorldTravel::LEVEL_BAND_COUNT> _mixSums;
    std::vector<Bot> _bots;
    std::unordered_map<std::uint32_t, Auction> _house;
    AuctionIndex::Book _book;
    AuctionIndex::Snapshot _snapshot;
    std::int64_t _publishedAt{0};  // the last publication, model seconds
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> _events;
    std::priority_queue<Ending, std::vector<Ending>, std::greater<Ending>> _endings;
    std::uint64_t _order{0};
    std::uint32_t _lastId{0};
    std::uint64_t _feesKept{0};  // cuts and lost deposits
    double _end{0.0};
    Result _result;
};
}  // namespace MarketSim

#endif
