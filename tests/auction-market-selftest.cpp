/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the auction market simulation (auction house round 2 AH2, tests/AuctionMarketSim.h): model bots
// trade through the real pure headers in rounds. The model checks five invariants every round (copper and items
// conserved, no purchase above its ceiling, no listing below its floor, every listing ends or stands with its end in
// the future); here a hand market and a seeded search of small markets must keep them all and trade (sales, bids,
// expiries), the same seed must give the same run, and each negative control (a fault in the model's glue: a cut not
// paid, a ceiling ignored, an expiry skipped) must be caught by its own invariant. A preset runs one market under the
// invariants and prints it as markdown for the user. The default run also runs the preset night under the invariants
// and the path control (a fed sale makes a path line a bot buys), and checks in the hand market that the run feeds the
// sale chance's records and that a line taken up later buys nothing before its hour. The default run also runs the
// preset cold-start (the night's trade goods in an empty house, with assumed path buyers): with the standing time
// longer than the run no path line exists and no trade good sells; with the default standing time path lines are
// bought, and every kind with an item priced by its standing offers and asked for by a path is bought (AH.18: each
// kind's sales to the level bands 1 to 3 are printed, not required). The presets gear-no-buyer (the night, its gear per
// hour) and gear-buyers-arrive (the same, with assumed gear buyers from a later model hour on) run only when asked.
// Usage: auction-market-selftest <repo> [--preset hand|night|gear-no-buyer|gear-buyers-arrive|cold-start] [--hours H]
// [--seed S] [--print]. Exit code 1 on a failed check. A preset's print: `<exe> <repo> --preset night --print`
// (server-tools.md, Market simulation).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "AuctionMarketSim.h"
#include "AuctionRules.h"
#include "GearWorthRules.h"
#include "MarketRules.h"
#include "MaterialRules.h"
#include "PlannerSnapshot.h"
#include "ProfessionRules.h"
#include "RecipeIndex.h"
#include "SkillPathBridge.h"
#include "SkillPathRules.h"
#include "SkillPathTypes.h"

using namespace MarketSim;
using AuctionRules::LineOwner;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << '\n';
    }
}

// Every invariant held over a run
void CheckClean(Result const& result, std::string const& what)
{
    for (std::size_t invariant = 0; invariant < INVARIANT_COUNT; ++invariant)
        Check(!result.violations[invariant], what + ": invariant " + std::string(INVARIANT_WORDS[invariant]) +
                                                 " holds (" + std::to_string(result.violations[invariant]) +
                                                 " violations; first: " + result.first[invariant] + ")");
}

// The run's numbers folded into one value: the same seed must give the same one
std::uint64_t Digest(Result const& result)
{
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](std::uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    for (auto const& [kind, tally] : result.ByKind())
    {
        mix(kind);
        for (std::uint64_t const value : {tally.posts, tally.relists, tally.sales, tally.expiries, tally.bids,
                                          tally.buyouts, tally.won, tally.vendored, tally.gold, tally.depositsLost})
            mix(value);
    }
    mix(result.moneyEnd);
    mix(result.visits);
    mix(result.lotsCome);
    return hash;
}

struct Dice
{
    std::mt19937& rng;
    bool Pick(int percent) { return static_cast<int>(rng() % 100) < percent; }
    std::uint32_t Range(std::uint32_t low, std::uint32_t high) { return low + rng() % (high - low + 1); }
    double Real(double low, double high) { return std::uniform_real_distribution<double>(low, high)(rng); }
};

// ---- the hand market ----
// Ten band-0 bots over 30 hours with a stream of five goods: a cloth everyone lists in stacks of ten, which two bots
// buy out and two more and a path line want at less; a weapon nobody wants within its ask (a gear line under it); a
// grey item that is no stock; two bags, one under the bag lines' ceilings of bots with few free slots and one above
// them. Two band-1 bots with no stream: one carries a lone recipe, the other wants it above its start bid and under
// its ask, so it bids once the auction nears its end. Planted so the run sells, bids, wins, expires, holds, skips and
// vendors. Use lines hold their worth at the neutral exchange: every hand buyer holds at least twice its reserve,
// so it pays ten times that (AH.5: the flush share 0.1), and a vendor stocks both bags (their cap stands).
constexpr std::uint32_t HAND_BIG_BAG = 1001;
constexpr std::uint32_t HAND_SMALL_BAG = 1002;
constexpr std::uint32_t HAND_CLOTH = 1003;
// A path line's copper weight with the need factor equal to the exchange: its ceiling is the copper given
constexpr AuctionRules::CopperWeight AT_RESERVE{1.0f, 1.0f};
constexpr std::uint32_t HAND_WEAPON = 1004;
constexpr std::uint32_t HAND_GREY = 1005;
constexpr std::uint32_t HAND_RECIPE = 1006;

MarketSim::Market HandMarket(std::uint32_t seed)
{
    MarketSim::Market market;
    market.seed = seed;
    market.hours = 30.0;
    market.goods = {{HAND_BIG_BAG, 1, 1, 2500, 12000, 0, 15}, {HAND_SMALL_BAG, 1, 1, 625, 2000, 0, 10},
                    {HAND_CLOTH, 7, 1, 50, 150, 0, 10},       {HAND_WEAPON, 2, 2, 3000, 9000, 20, 25},
                    {HAND_GREY, 4, 0, 40, 40, 0, 5},          {HAND_RECIPE, 9, 2, 1000, 5000, 0, 20}};
    market.ladder = {{HAND_BIG_BAG, 10, 10000, true}, {HAND_SMALL_BAG, 8, 2500, true}};
    Band& band = market.bands[0];
    band.slots = WorldTravel::DEFAULT_SLOT_BANDS[0];
    band.visitsPerHour = 1.0;
    band.lotsPerHour = 0.6;
    band.mix = {{HAND_BIG_BAG, 1, 0.5},
                {HAND_SMALL_BAG, 1, 1.0},
                {HAND_CLOTH, 10, 3.0},
                {HAND_WEAPON, 1, 1.0},
                {HAND_GREY, 1, 1.0}};
    band.incomePerHour = 3000.0;
    band.spendPerHour = 2000.0;
    for (std::uint32_t index = 0; index < 10; ++index)
    {
        BotSetup bot;
        bot.money = 100000 + 10000 * index;
        bot.reserve = 20000;
        bot.freeSlots = index % 2 ? 4 : 12;
        bot.smallestBag = index < 8 ? 6 : 10;
        if (index < 2)
            bot.lines.push_back(AuctionRules::UseLine(HAND_CLOTH, 10, 16, 0, 50, {}));
        else if (index < 4)
            bot.lines.push_back(AuctionRules::UseLine(HAND_CLOTH, 10, 13, 0, 50, {}));
        else if (index == 4)
            bot.lines.push_back(AuctionRules::LineOf(LineOwner::Gear, HAND_WEAPON, 1, 4000.0, -1.0, 1.0f, 3000));
        else if (index == 5)
            bot.lines.push_back(AuctionRules::PathLine(HAND_CLOTH, 20, 140, 140, AT_RESERVE, 50, 0));
        market.bots.push_back(bot);
    }
    Band& lone = market.bands[1];
    lone.slots = WorldTravel::DEFAULT_SLOT_BANDS[1];
    lone.visitsPerHour = 2.0;
    BotSetup seller;
    seller.band = 1;
    seller.money = 50000;
    seller.stock.push_back({HAND_RECIPE, 1});
    market.bots.push_back(seller);
    BotSetup bidder;
    bidder.band = 1;
    bidder.money = 50000;
    bidder.reserve = 10000;
    bidder.smallestBag = 16;
    bidder.lines.push_back(AuctionRules::UseLine(HAND_RECIPE, 1, 450, 0, 1000, {}));
    market.bots.push_back(bidder);
    return market;
}

// ---- the seeded search ----
// Small markets: one to three bags, three to eight goods of several kinds, four to sixteen bots over up to three
// bands with lines of every owner, the house's numbers and fees drawn; 26 to 60 hours so listings end and bids
// (which start two hours before an end) come
MarketSim::Market RandomMarket(std::uint32_t seed)
{
    std::mt19937 rng(seed);
    Dice dice{rng};
    MarketSim::Market market;
    market.seed = seed;
    market.hours = dice.Range(26, 60);
    std::uint32_t next = 2000;
    for (std::uint32_t rung = dice.Range(1, 3); rung; --rung)
    {
        std::uint32_t const item = next++;
        std::uint32_t const slots = 6 + 2 * dice.Range(0, 5);
        std::uint32_t const buy = dice.Range(500, 40000);
        market.ladder.push_back({item, slots, buy, item % 2 == 0});  // every other bag a vendor stocks
        market.goods.push_back(
            {item, 1, 1, buy / 4, static_cast<std::uint64_t>(buy * dice.Real(0.3, 2.0)), 0, dice.Range(1, 80)});
    }
    std::array<std::uint32_t, 6> const classes = {0, 2, 4, 7, 7, 15};
    for (std::uint32_t good = dice.Range(3, 8); good; --good)
    {
        std::uint32_t const sell = dice.Pick(10) ? 0 : dice.Range(1, 4000);
        std::uint64_t const reference = dice.Pick(10) ? 0 : sell + dice.Range(0, 6 * sell + 300);
        std::uint32_t const required = dice.Pick(30) ? 0 : dice.Range(1, 80);
        market.goods.push_back(
            {next++, classes[dice.Range(0, 5)], dice.Range(0, 4), sell, reference, required, dice.Range(1, 200)});
    }
    std::uint32_t const firstBand = dice.Range(0, 2);
    std::uint32_t const lastBand = firstBand + dice.Range(0, 2);
    for (std::uint32_t index = firstBand; index <= lastBand; ++index)
    {
        Band& band = market.bands[index];
        band.slots = WorldTravel::DEFAULT_SLOT_BANDS[index];
        band.visitsPerHour = dice.Real(0.3, 3.0);
        band.lotsPerHour = dice.Real(0.2, 2.5);
        for (Goods const& goods : market.goods)
            if (dice.Pick(80))
                band.mix.push_back({goods.item, goods.classId == 7 ? dice.Range(1, 20) : 1, dice.Real(0.1, 1.0)});
        band.incomePerHour = dice.Pick(20) ? 0.0 : dice.Real(0.0, 20000.0);
        band.spendPerHour = dice.Pick(20) ? 0.0 : dice.Real(0.0, 15000.0);
    }
    std::array<std::uint32_t, 4> const smallest = {0, 6, 8, 10};
    for (std::uint32_t count = dice.Range(4, 16); count; --count)
    {
        BotSetup bot;
        bot.band = static_cast<std::uint8_t>(dice.Range(firstBand, lastBand));
        bot.money = dice.Range(0, 300000);
        bot.reserve = dice.Pick(30) ? 0 : dice.Range(0, static_cast<std::uint32_t>(bot.money));
        bot.freeSlots = dice.Range(0, 30);
        bot.smallestBag = smallest[dice.Range(0, 3)];
        for (std::uint32_t line = dice.Range(0, 3); line; --line)
        {
            Goods const& goods = market.goods[dice.Range(0, static_cast<std::uint32_t>(market.goods.size() - 1))];
            double const worth = static_cast<double>(std::max<std::uint64_t>(goods.reference, 1)) * dice.Real(0.3, 2.5);
            std::uint32_t const units = dice.Range(1, 10);
            std::uint32_t const owner = dice.Range(0, 2);
            if (!owner)
                bot.lines.push_back(AuctionRules::UseLine(goods.item, units, static_cast<std::uint32_t>(worth),
                                                          dice.Pick(30) ? goods.sellPrice * 4 : 0, goods.sellPrice,
                                                          {}));
            else if (owner == 1)
                bot.lines.push_back(AuctionRules::PathLine(goods.item, units, static_cast<std::uint32_t>(worth),
                                                           static_cast<std::uint32_t>(worth), AT_RESERVE,
                                                           goods.sellPrice, 0));
            else
                bot.lines.push_back(AuctionRules::LineOf(LineOwner::Gear, goods.item, 1, worth,
                                                         dice.Pick(50) ? -1.0 : worth * dice.Real(0.5, 1.5),
                                                         static_cast<float>(dice.Real(0.5, 2.0)), goods.sellPrice));
        }
        market.bots.push_back(bot);
    }
    if (dice.Pick(60))
    {
        market.config.UndercutPercent = dice.Range(0, 10);
        market.config.RelistStepPercent = dice.Range(1, 30);
        market.config.RelistLimit = dice.Range(0, 4);
        market.config.FirstDurationHours = AuctionRules::LISTING_HOURS[dice.Range(0, 2)];
        market.config.RelistDurationHours = AuctionRules::LISTING_HOURS[dice.Range(0, 2)];
        market.config.OpenBidsMax = dice.Range(0, 4);
        market.config.HistoryDays = dice.Range(1, 7);
    }
    std::array<std::uint32_t, 4> const publish = {60, 120, 300, 600};
    market.config.PublishSeconds = publish[dice.Range(0, 3)];
    if (dice.Pick(40))
    {
        market.fees.row.depositPercent = dice.Range(5, 15);
        market.fees.row.cutPercent = dice.Range(5, 15);
        market.fees.rates.deposit = static_cast<float>(dice.Real(0.5, 2.0));
        market.fees.rates.cut = static_cast<float>(dice.Real(0.5, 2.0));
        market.fees.rates.time = static_cast<float>(dice.Real(0.25, 1.0));
    }
    return market;
}

constexpr std::uint32_t SEARCH_MARKETS = 30;
constexpr std::uint32_t CONTROL_MARKETS = 8;

void CheckHand()
{
    MarketSim::Market const market = HandMarket(7);
    Result const result = Sim(market).Run();
    CheckClean(result, "hand market");
    KindTally const total = result.Total();
    std::uint64_t skips = 0;
    for (auto const& owner : total.skips)
        for (std::uint64_t const count : owner)
            skips += count;
    std::uint64_t holds = 0;
    for (std::uint64_t const count : total.holds)
        holds += count;
    Check(total.buyouts > 0 && total.bids > 0 && total.won > 0 && total.expiries > 0,
          "hand market: buyouts " + std::to_string(total.buyouts) + ", bids " + std::to_string(total.bids) + ", won " +
              std::to_string(total.won) + ", expiries " + std::to_string(total.expiries) + ", relists " +
              std::to_string(total.relists));
    Check(total.vendored > 0 && holds > 0 && skips > 0, "hand market: vendored " + std::to_string(total.vendored) +
                                                            ", holds " + std::to_string(holds) + ", skips " +
                                                            std::to_string(skips));
    Check(Digest(Sim(market).Run()) == Digest(result), "hand market: the same seed gives the same run");
    Check(Digest(Sim(HandMarket(8)).Run()) != Digest(result), "hand market: another seed gives another run");
    Check(std::any_of(result.endRecords.begin(), result.endRecords.end(),
                      [](auto const& kind) { return kind.second.sales > 0.0 && kind.second.hours > 0.0; }),
          "hand market: a kind's record holds sales and listing hours");
    // The recipe has one buyer, the last bot: its line taken up after the run buys nothing, from hour 10 on it does
    auto const recipeSales = [&market](double from)
    {
        MarketSim::Market later = market;
        BotSetup& bidder = later.bots.back();
        bidder.laterLines = std::move(bidder.lines);
        bidder.lines.clear();
        bidder.laterFromHours = from;
        Result const ran = Sim(later).Run();
        CheckClean(ran, "hand market, a line taken up later");
        auto const found = ran.items.find(HAND_RECIPE);
        return found == ran.items.end() ? std::uint64_t{0} : found->second.sales;
    };
    std::uint64_t const never = recipeSales(market.hours);
    std::uint64_t const tenth = recipeSales(10.0);
    Check(!never && tenth, "hand market: a line from the run's end sells " + std::to_string(never) +
                               " recipes, from hour 10 " + std::to_string(tenth));
}

void Search()
{
    KindTally total;
    for (std::uint32_t seed = 1; seed <= SEARCH_MARKETS; ++seed)
    {
        Result const result = Sim(RandomMarket(seed)).Run();
        CheckClean(result, "search market " + std::to_string(seed));
        total.Add(result.Total());
    }
    std::cout << "search: " << SEARCH_MARKETS << " markets, " << total.posts + total.relists << " listings, "
              << total.sales << " sales (" << total.buyouts << " buyouts, " << total.won << " won bids), " << total.bids
              << " bids, " << total.expiries << " expiries, " << total.relists << " relists\n";
    Check(total.sales > 0 && total.bids > 0 && total.won > 0 && total.expiries > 0,
          "search: sales, bids, won bids and expiries all occurred");
}

// Each control's fault must break its own invariant in the hand market or the first search markets
void Controls()
{
    struct Control
    {
        Fault fault;
        Invariant invariant;
        std::string_view what;
    };
    std::array<Control, 4> const controls = {
        {{Fault::CutNotPaid, Invariant::Copper, "a cut not paid"},
         {Fault::CeilingIgnored, Invariant::Ceiling, "a ceiling ignored"},
         {Fault::ExpirySkipped, Invariant::Ends, "an expiry skipped"},
         {Fault::KeptIgnored, Invariant::Kept, "a purse that ignores what the market keeps"}}};
    for (Control const& control : controls)
    {
        std::uint64_t caught = 0;
        std::string first;
        auto note = [&](Result const& result)
        {
            caught += result.violations[Slot(control.invariant)];
            if (first.empty())
                first = result.first[Slot(control.invariant)];
        };
        MarketSim::Market const hand = HandMarket(7);
        note(Sim(hand, control.fault).Run());
        for (std::uint32_t seed = 1; seed <= CONTROL_MARKETS; ++seed)
        {
            MarketSim::Market const market = RandomMarket(seed);
            note(Sim(market, control.fault).Run());
        }
        Check(caught > 0, "control " + std::string(control.what) + " is caught by invariant " +
                              std::string(INVARIANT_WORDS[Slot(control.invariant)]));
        std::cout << "control " << control.what << ": " << caught << " " << INVARIANT_WORDS[Slot(control.invariant)]
                  << " violations; first: " << first << '\n';
    }
}

// ---- the print ----
std::string_view KindWord(std::uint32_t classId)
{
    static std::array<std::string_view, 17> const words = {
        "consumable", "container", "weapon", "gem",   "armor", "reagent",   "projectile", "trade goods", "generic",
        "recipe",     "money",     "quiver", "quest", "key",   "permanent", "misc",       "glyph"};
    return classId < words.size() ? words[classId] : std::string_view("unknown");
}

std::string Reasons(std::array<std::uint64_t, AuctionWords::REASON_COUNT> const& counts)
{
    std::ostringstream text;
    for (std::size_t reason = 0; reason < counts.size(); ++reason)
        if (counts[reason])
            text << ((text.str().empty() ? "" : ", "))
                 << AuctionWords::ReasonWord(static_cast<AuctionWords::Reason>(reason)) << ' ' << counts[reason];
    return text.str().empty() ? "-" : text.str();
}

std::string Unbought(KindTally const& tally)
{
    std::ostringstream text;
    for (std::size_t owner = 0; owner < OWNER_COUNT; ++owner)
        for (std::size_t reason = 0; reason < AuctionWords::REASON_COUNT; ++reason)
            if (tally.skips[owner][reason])
                text << ((text.str().empty() ? "" : ", ")) << AuctionRules::LINE_OWNER_WORDS[owner] << ' '
                     << AuctionWords::ReasonWord(static_cast<AuctionWords::Reason>(reason)) << ' '
                     << tally.skips[owner][reason];
    for (std::size_t refusal = 0; refusal < REFUSAL_COUNT; ++refusal)
        if (tally.refused[refusal])
            text << ((text.str().empty() ? "" : ", ")) << "refused " << REFUSAL_WORDS[refusal] << ' '
                 << tally.refused[refusal];
    return text.str().empty() ? "-" : text.str();
}

std::string Gold(std::uint64_t copper)
{
    std::ostringstream text;
    text << copper / 10000 << "g " << copper / 100 % 100 << "s " << copper % 100 << 'c';
    return text.str();
}

// A kind of the sale chance in words, from its code (AuctionIndex::KindOf)
std::string LevelKindWord(std::uint32_t code)
{
    if (!code)
        return "unknown";
    std::uint32_t const low = (code & AuctionIndex::KIND_BAND_MAX) * AuctionIndex::KIND_BAND_LEVELS;
    std::ostringstream text;
    text << KindWord((code >> 16) & 0xFF) << " q" << ((code >> 8) & 0xFF)
         << (code & AuctionIndex::KIND_BY_ITEM_LEVEL ? " ilvl " : " req ") << low << '-'
         << low + AuctionIndex::KIND_BAND_LEVELS - 1;
    return text.str();
}

// A model hour's row label; the run's last hour may be a part of one
std::string HourWord(MarketSim::Market const& market, std::size_t hour)
{
    std::ostringstream text;
    text << hour;
    double const part = market.hours - static_cast<double>(hour);
    if (part < 0.999)
        text << " (" << std::fixed << std::setprecision(1) << part << " h)";
    return text.str();
}

void PrintLevelKinds(std::ostream& out, MarketSim::Market const& market, Result const& result)
{
    std::map<std::uint32_t, LevelKindTally> run;  // listings and sales over the run
    for (HourTally const& hour : result.hours)
        for (auto const& [kind, tally] : hour.levelKinds)
        {
            run[kind].listings += tally.listings;
            run[kind].sales += tally.sales;
        }
    auto const sold = std::count_if(run.begin(), run.end(), [](auto const& kind) { return kind.second.sales > 0; });
    out << "\n## Kinds of the sale chance (class, quality, level band) per model hour\n\n";
    out << run.size() << " kinds, " << sold << " with a sale in the run, " << run.size() - sold << " without.\n\n";
    out << "| hour | without a sale: listings | lots standing | vendored | with a sale: listings | sales | lots "
           "standing | vendored |\n|---|---|---|---|---|---|---|---|\n";
    for (std::size_t hour = 0; hour < result.hours.size(); ++hour)
    {
        std::array<LevelKindTally, 2> sums;  // without a sale, with one
        for (auto const& [kind, tally] : result.hours[hour].levelKinds)
        {
            LevelKindTally& sum = sums[run[kind].sales ? 1 : 0];
            sum.listings += tally.listings;
            sum.sales += tally.sales;
            sum.standing += tally.standing;
            sum.vendored += tally.vendored;
        }
        out << "| " << HourWord(market, hour) << " | " << sums[0].listings << " | " << sums[0].standing << " | "
            << sums[0].vendored << " | " << sums[1].listings << " | " << sums[1].sales << " | " << sums[1].standing
            << " | " << sums[1].vendored << " |\n";
    }
    out << "\n### Listings per model hour by kind (listings, or listings/sales in an hour with a sale)\n\n| kind |";
    for (std::size_t hour = 0; hour < result.hours.size(); ++hour)
        out << ' ' << hour << " |";
    out << " listings | sales |\n|---|";
    for (std::size_t column = 0; column < result.hours.size() + 2; ++column)
        out << "---|";
    out << '\n';
    for (auto const& [kind, total] : run)
    {
        if (!total.listings && !total.sales)
            continue;
        out << "| " << LevelKindWord(kind) << " |";
        for (HourTally const& hour : result.hours)
        {
            auto const found = hour.levelKinds.find(kind);
            LevelKindTally const tally = found == hour.levelKinds.end() ? LevelKindTally{} : found->second;
            out << ' ' << tally.listings;
            if (tally.sales)
                out << '/' << tally.sales;
            out << " |";
        }
        out << ' ' << total.listings << " | " << total.sales << " |\n";
    }
    out << "\n### Listed at the end, by kind and ask over vendor price\n\n"
           "Ask over vendor price: a standing lot's buyout / (the template's sell price x units). The record is the "
           "kind's in the last publication; the chance is what that record gives an item without a record of its own, "
           "listed for the first duration.\n\n";
    out << "| kind | lots | lowest | q1 | median | q3 | highest | no vendor price | sales in the run | record: lots "
           "sold | record: lot hours | chance |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    std::vector<std::pair<std::uint64_t, std::uint32_t>> standing;  // lots, kind
    for (auto const& [kind, total] : run)
    {
        auto const priced = result.endAsks.find(kind);
        auto const unpriced = result.endUnpriced.find(kind);
        std::uint64_t const lots = (priced == result.endAsks.end() ? 0 : priced->second.size()) +
                                   (unpriced == result.endUnpriced.end() ? 0 : unpriced->second);
        if (lots)
            standing.emplace_back(lots, kind);
    }
    std::sort(standing.rbegin(), standing.rend());
    double const listedHours = static_cast<double>(AuctionRules::ExpirySeconds(
                                   market.fees, AuctionRules::DurationHours(market.config.FirstDurationHours))) /
                               static_cast<double>(AuctionRules::SECONDS_PER_HOUR);
    for (auto const& [lots, kind] : standing)
    {
        auto const priced = result.endAsks.find(kind);
        std::vector<double> asks = priced == result.endAsks.end() ? std::vector<double>{} : priced->second;
        std::sort(asks.begin(), asks.end());
        out << "| " << LevelKindWord(kind) << " | " << lots << " |" << std::fixed << std::setprecision(2);
        for (std::size_t quarter = 0; quarter <= 4; ++quarter)
            if (asks.empty())
                out << " - |";
            else
                out << ' ' << asks[(asks.size() - 1) * quarter / 4] << " |";
        auto const unpriced = result.endUnpriced.find(kind);
        auto const found = result.endRecords.find(kind);
        AuctionIndex::Record const record = found == result.endRecords.end() ? AuctionIndex::Record{} : found->second;
        out << ' ' << (unpriced == result.endUnpriced.end() ? 0 : unpriced->second) << " | " << run[kind].sales << " | "
            << record.sales << " | " << std::setprecision(0) << record.hours << " | " << std::setprecision(4)
            << AuctionRules::SaleChance({}, record, listedHours, market.config) << " |\n";
    }
    out.unsetf(std::ios::floatfield);
    out << std::setprecision(6);
}

void Print(std::ostream& out, std::string_view name, MarketSim::Market const& market, Result const& result, double ms)
{
    out << "\n<!-- market -->\n# Market simulation: preset " << name << "\n\n";
    out << "- Printed by tests/auction-market-selftest.cpp: " << market.bots.size() << " bots, " << market.hours
        << " model hours, seed " << market.seed << ", " << result.rounds << " rounds of "
        << market.config.PublishSeconds << " s, " << result.publishes << " publications, " << result.visits
        << " visits, " << result.lotsCome << " lots came by (" << result.unitsCome << " units); run in "
        << static_cast<long>(ms) << " ms.\n";
    out << "- Invariants:";
    for (std::size_t invariant = 0; invariant < INVARIANT_COUNT; ++invariant)
        out << (invariant ? "," : "") << ' ' << INVARIANT_WORDS[invariant] << ' ' << result.violations[invariant];
    out << " violations.\n";
    out << "- Copper: start " << Gold(result.moneyStart) << ", income " << Gold(result.injected) << ", spent "
        << Gold(result.spent) << ", vendors paid " << Gold(result.vendorIncome) << ", end " << Gold(result.moneyEnd)
        << ".\n\n";
    out << "## Per model hour and kind\n\n";
    out << "| hour | kind | posts | relists | standing | sales | expiries | buyouts | bids | deposits paid | deposits "
           "lost | vendored | holds | unbought |\n";
    out << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (std::size_t hour = 0; hour < result.hours.size(); ++hour)
        for (auto const& [kind, tally] : result.hours[hour].kinds)
            out << "| " << hour << " | " << KindWord(kind) << " | " << tally.posts << " | " << tally.relists << " | "
                << tally.standing << " | " << tally.sales << " | " << tally.expiries << " | " << tally.buyouts << " | "
                << tally.bids << " | " << tally.depositsPaid << " | " << tally.depositsLost << " | " << tally.vendored
                << " | " << Reasons(tally.holds) << " | " << Unbought(tally) << " |\n";
    out << "\n## At the end, by kind\n\n";
    out << "| kind | listings | sales | won bids | expiries | sell-through | gold moved | cuts | standing at the end "
           "| vendored | holds | unbought |\n";
    out << "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    std::uint64_t moved = 0;
    for (auto const& [kind, tally] : result.ByKind())
    {
        std::uint64_t const ended = tally.sales + tally.expiries;
        auto const lots = result.houseLots.find(kind);
        out << "| " << KindWord(kind) << " | " << tally.posts + tally.relists << " | " << tally.sales << " | "
            << tally.won << " | " << tally.expiries << " | ";
        if (ended)
            out << std::fixed << std::setprecision(0)
                << 100.0 * static_cast<double>(tally.sales) / static_cast<double>(ended) << " %";
        else
            out << "-";
        out << " | " << Gold(tally.gold) << " | " << Gold(tally.cuts) << " | "
            << (lots == result.houseLots.end() ? 0 : lots->second) << " | " << tally.vendored << " | "
            << Reasons(tally.holds) << " | " << Unbought(tally) << " |\n";
        moved += tally.gold;
    }
    out << "\nGold moved through the house: " << Gold(moved) << ".\n";
    PrintLevelKinds(out, market, result);
}

// ---- the night (AH1's fixture tests/fixtures/auction-market-night.txt; PLAN 3.2, AH.3) ----
// Its inputs are observations of lab5's night; what the night cannot give is filled as the fixture's assumed sections
// say and marked so in the print. Nothing here is fitted to the night's outcome.
std::string ReadFile(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> Fields(std::string const& line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true)
    {
        std::size_t const tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            return fields;
        start = tab + 1;
    }
}

double Number(std::vector<std::string> const& fields, std::size_t at)
{
    return at < fields.size() ? std::strtod(fields[at].c_str(), nullptr) : 0.0;
}

std::uint32_t Whole(double value) { return static_cast<std::uint32_t>(std::max<long long>(std::llround(value), 0)); }

double Unit(std::mt19937& rng) { return (static_cast<double>(rng()) + 0.5) / 4294967296.0; }

// A draw between the observed quartiles, linear between them; the tails stay at the outer quartiles (assumed: the
// fixture holds no more of the distribution)
double Quartile(double unit, std::array<double, 3> const& quartiles)
{
    if (unit <= 0.25)
        return quartiles[0];
    if (unit <= 0.5)
        return quartiles[0] + (quartiles[1] - quartiles[0]) * (unit - 0.25) / 0.25;
    if (unit <= 0.75)
        return quartiles[1] + (quartiles[2] - quartiles[1]) * (unit - 0.5) / 0.25;
    return quartiles[2];
}

struct NightUse
{
    std::uint32_t item{0};
    double rows{0.0};
    std::uint32_t ceilingCopper{0};
};

struct NightBand
{
    std::uint32_t levelLow{0};
    std::uint32_t levelHigh{0};
    double bots{0.0};
    double botHours{0.0};
    double visits{0.0};
    double posts{0.0};
    double relists{0.0};
    double holds{0.0};
    double buys{0.0};
    std::array<double, 3> money{};
    std::array<double, 3> spendable{};
    std::array<double, 3> freeSlots{};
    double incomeIn{0.0};                                     // copper, every source but the auction
    double incomeOut{0.0};                                    // copper, every sink but the auction
    std::vector<std::pair<std::uint32_t, double>> smallBags;  // slots, bots
    std::vector<Arrival> mix;
    std::vector<NightUse> uses;
};

struct NightKind
{
    std::uint64_t posts{0};
    std::uint64_t relists{0};
    std::uint64_t holds{0};
    std::uint64_t sales{0};
    std::uint64_t buys{0};
    std::uint64_t houseLots{0};
    std::map<std::string, std::uint64_t> skips;  // decision rows by reason
};

// One item's listings (`item`) and its bag lines' decision rows by choice and reason (`bagline`)
struct NightItem
{
    std::uint64_t listings{0};
    std::uint64_t buys{0};
    std::map<std::string, std::uint64_t> skips;
};

// A bag's buying rows in one band (`bagline`): the over_ceiling skips' median ask and ceiling, and the rows
struct NightBag
{
    double askQ2{0.0};
    double ceilingQ2{0.0};
    double overRows{0.0};
    double buyRows{0.0};
};

struct Night
{
    std::string header;
    double hours{0.0};
    std::map<std::string, double> values;
    std::array<NightBand, WorldTravel::LEVEL_BAND_COUNT> bands;
    std::map<std::uint32_t, NightKind> kinds;  // by item class
    std::vector<BagRung> ladder;
    std::map<std::uint32_t, NightItem> items;
    std::map<std::pair<std::uint32_t, std::uint32_t>, NightBag> bags;  // by band and item
};

template <typename ClassOf>
void ReadNight(std::string const& text, Night& night, ClassOf const& classOf)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (night.header.empty() && line.rfind("# auction-market-night", 0) == 0)
        {
            night.header = line.substr(2);
            std::size_t const at = line.find(" hours ");
            night.hours = at == std::string::npos ? 0.0 : std::strtod(line.c_str() + at + 7, nullptr);
        }
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> const row = Fields(line);
        std::string const& tag = row[0];
        std::size_t const band = Whole(Number(row, 1));
        NightBand* const at = band < WorldTravel::LEVEL_BAND_COUNT ? &night.bands[band] : nullptr;
        if (tag == "night" && row.size() > 2)
            night.values[row[1]] = Number(row, 2);
        else if (tag == "band" && at)
        {
            at->levelLow = Whole(Number(row, 2));
            at->levelHigh = Whole(Number(row, 3));
            at->bots = Number(row, 4);
            at->botHours = Number(row, 5);
            at->visits = Number(row, 6);
            at->posts = Number(row, 11);
            at->relists = Number(row, 12);
            at->holds = Number(row, 13);
            at->buys = Number(row, 14);
        }
        else if (tag == "item" && at)
        {
            // arrivals: the item's posts (relists and holds are the rules' outcomes); a lot's units: its units over
            // its listings (C7)
            double const posts = Number(row, 3);
            double const listings = posts + Number(row, 4);
            night.items[Whole(Number(row, 2))].listings += Whole(listings);
            if (posts > 0.0)
                at->mix.push_back(
                    {Whole(Number(row, 2)), std::max<std::uint32_t>(Whole(Number(row, 6) / listings), 1), posts});
        }
        else if (tag == "kind")
        {
            NightKind& kind = night.kinds[Whole(Number(row, 2))];
            kind.posts += Whole(Number(row, 4));
            kind.relists += Whole(Number(row, 5));
            kind.holds += Whole(Number(row, 6));
        }
        else if (tag == "house")
            night.kinds[Whole(Number(row, 1))].houseLots += Whole(Number(row, 3));
        else if (tag == "bagline" && row.size() > 5)
        {
            NightKind& kind = night.kinds[classOf(Whole(Number(row, 2)))];
            NightItem& item = night.items[Whole(Number(row, 2))];
            NightBag& bag = night.bags[{static_cast<std::uint32_t>(band), Whole(Number(row, 2))}];
            if (row[3] == "buy")
            {
                kind.buys += Whole(Number(row, 5));
                item.buys += Whole(Number(row, 5));
                bag.buyRows += Number(row, 5);
            }
            else
            {
                kind.skips[row[4]] += Whole(Number(row, 5));
                item.skips[row[4]] += Whole(Number(row, 5));
                if (row[4] == "over_ceiling")
                {
                    bag.overRows += Number(row, 5);
                    bag.askQ2 = Number(row, 7);
                    bag.ceilingQ2 = Number(row, 10);
                }
            }
        }
        else if (tag == "useline" && at)
        {
            at->uses.push_back({Whole(Number(row, 2)), Number(row, 3), Whole(Number(row, 4))});
            night.kinds[classOf(Whole(Number(row, 2)))].skips["no_offer"] += Whole(Number(row, 3));
        }
        else if (tag == "carried" && at)
            for (std::size_t quartile = 0; quartile < 3; ++quartile)
            {
                at->money[quartile] = Number(row, 3 + quartile);
                at->spendable[quartile] = Number(row, 6 + quartile);
                at->freeSlots[quartile] = Number(row, 9 + quartile);
            }
        else if (tag == "smallbag" && at)
            at->smallBags.push_back({Whole(Number(row, 2)), Number(row, 3)});
        else if (tag == "income" && at && row.size() > 5 && row[3] != "auction")
            (row[2] == "in" ? at->incomeIn : at->incomeOut) += Number(row, 5);
        else if (tag == "ladder")
            night.ladder.push_back({Whole(Number(row, 1)), Whole(Number(row, 2)), Whole(Number(row, 3)), false});
    }
    night.kinds[1].sales = Whole(night.values["sales_containers"]);
}

// ---- the night's skill paths (assumed: the night's build wrote no path lines) ----
// One typical bot per band and craft, weighed by the real SkillPath::Weigh over the dumped recipe index with a market
// view of the model's published snapshot; a bot of the band follows one of them (its pathSlot)
constexpr std::size_t PATH_BOTS = std::size(ProfessionRules::CRAFTS);
constexpr std::array<std::uint32_t, 6> RANK_CAPS = {75, 150, 225, 300, 375, 450};
constexpr std::uint32_t SKILL_PER_LEVEL = 5;  // assumed: a typical crafter's skill at its level

std::string_view CraftName(ProfessionRules::Skill which)
{
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        if (info.Which == which)
            return info.Name;
    return "?";
}

class NightPaths : public PathSource
{
public:
    NightPaths(std::shared_ptr<Recipes::RecipeIndex const> index, std::unordered_map<std::uint32_t, std::uint32_t> sell,
               AuctionRules::Fees fees, AuctionConfig config, Night const& night)
        : _index(std::move(index)), _sell(std::move(sell)), _fees(fees), _config(config)
    {
        std::set<std::uint32_t> tools;
        std::set<std::uint32_t> categories;
        for (Recipes::Recipe const& recipe : _index->Recipes())
        {
            tools.insert(recipe.tools.begin(), recipe.tools.end());
            categories.insert(recipe.toolCategories.begin(), recipe.toolCategories.end());
        }
        for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        {
            NightBand const& row = night.bands[band];
            std::uint32_t const level = (row.levelLow + row.levelHigh) / 2;
            std::uint64_t const money = Whole(row.money[1]);
            std::uint64_t const reserve = row.money[1] > row.spendable[1] ? Whole(row.money[1] - row.spendable[1]) : 0;
            std::uint32_t const value = std::clamp<std::uint32_t>(level * SKILL_PER_LEVEL, 1, RANK_CAPS.back());
            auto const cap =
                std::find_if(RANK_CAPS.begin(), RANK_CAPS.end(), [value](std::uint32_t top) { return top > value; });
            std::uint32_t const top = cap == RANK_CAPS.end() ? RANK_CAPS.back() : *cap;
            for (std::size_t slot = 0; slot < PATH_BOTS; ++slot)
            {
                ProfessionRules::Craft const& craft = ProfessionRules::CRAFTS[slot];
                SkillPath::PathBot& bot = bots[band][slot];
                bot.level = level;
                for (std::uint8_t cls = 1; cls < 12 && !bot.cls; ++cls)
                    if (craft.Classes & ProfessionRules::ClassBit(cls))
                        bot.cls = cls;
                // SkillPathBridge.h BotOf: the money above the reserve and the rates of the bot's snapshot
                Planner::Snapshot snapshot;
                snapshot.level = static_cast<std::uint8_t>(level);
                snapshot.cls = bot.cls;
                snapshot.money = static_cast<std::uint32_t>(std::min<std::uint64_t>(money, UINT32_MAX));
                bot.money = money - reserve;
                bot.rates = SkillPathBridge::RatesOf(snapshot, reserve);
                bot.skills = {{static_cast<std::uint16_t>(craft.Which), value, top}};
                if (craft.Gatherer != ProfessionRules::Skill::None)
                    bot.skills.push_back({static_cast<std::uint16_t>(craft.Gatherer), value, top});
                bot.toolsHeld.assign(tools.begin(), tools.end());
                bot.categoriesHeld.assign(categories.begin(), categories.end());
            }
        }
    }

    std::uint32_t SellOf(std::uint32_t item) const
    {
        auto const found = _sell.find(item);
        return found == _sell.end() ? 0 : found->second;
    }

    ::Market::View ViewOf(AuctionIndex::Snapshot const& snapshot) const
    {
        return {std::make_shared<AuctionIndex::Snapshot const>(snapshot), _fees, _config, {}};
    }

    // PathWorld as the skill path selftest builds it (WorldOf): the vendors' sell prices, no lock, no skinnables or
    // waters, the market view, no observed gather rates (the static rule)
    SkillPath::PathWorld WorldOf(::Market::View const& market) const
    {
        return {*_index, {},     [this](std::uint32_t item) { return static_cast<double>(SellOf(item)); },
                {},      {},     SkillPath::Fault::None,
                {},      market, {}};
    }

    // The path's lines at its bot's copper weight (PlannerUpkeep.cpp BuyListOf: the newest plan's need), at most a
    // stocking vendor's price (the index's vendor rows, as AuctionRules::Table's vendorStocked reads them)
    std::vector<AuctionRules::BuyLine> LinesOf(SkillPath::Path const& path, SkillPath::PathBot const& bot,
                                               std::function<std::uint32_t(std::uint32_t)> const& held) const
    {
        return AuctionRules::PathLines(
            path.materials, static_cast<std::uint8_t>(Materials::Kind::Auction), {bot.rates.exchange, bot.rates.factor},
            held, [this](std::uint32_t item) { return SellOf(item); },
            [this](std::uint32_t item)
            {
                Recipes::VendorRow const* vendor = _index->Vendor(item);
                return vendor && vendor->stocked ? vendor->buyCopper / std::max<std::uint32_t>(vendor->buyCount, 1)
                                                 : 0u;
            });
    }

    void Weigh(AuctionIndex::Snapshot const& snapshot) override
    {
        auto const start = std::chrono::steady_clock::now();
        view = ViewOf(snapshot);
        SkillPath::PathWorld const world = WorldOf(view);
        for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
            for (std::size_t slot = 0; slot < PATH_BOTS; ++slot)
            {
                paths[band][slot] = SkillPath::Weigh(bots[band][slot], world);
                linesSeen += LinesOf(paths[band][slot], bots[band][slot], [](std::uint32_t) { return 0u; }).size();
            }
        ++weighs;
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    std::vector<AuctionRules::BuyLine> Lines(std::uint8_t band, std::uint32_t slot,
                                             std::function<std::uint32_t(std::uint32_t)> const& held) const override
    {
        return LinesOf(paths[band][slot % PATH_BOTS], bots[band][slot % PATH_BOTS], held);
    }

    std::array<std::array<SkillPath::PathBot, PATH_BOTS>, WorldTravel::LEVEL_BAND_COUNT> bots;
    std::array<std::array<SkillPath::Path, PATH_BOTS>, WorldTravel::LEVEL_BAND_COUNT> paths;  // the last weigh's
    ::Market::View view;                                                                      // the last weigh's
    std::uint32_t weighs{0};
    std::uint64_t linesSeen{0};  // path lines at the full want, over all weighs
    double ms{0.0};

private:
    std::shared_ptr<Recipes::RecipeIndex const> _index;
    std::unordered_map<std::uint32_t, std::uint32_t> _sell;
    AuctionRules::Fees _fees;
    AuctionConfig _config;
};

// The control outside the preset: a material of a typical path (vendor, gathering or recipe its cheapest source) gets
// one fed sale at half that cost and one standing lot in a separate book; the path weighed over that book's snapshot
// must take it from the auction and make a line whose ceiling is its other-source cost, and the buy rule must take the
// standing lot
std::string PathControl(NightPaths const& night)
{
    SkillPath::PathWorld const bare = night.WorldOf(night.ViewOf(AuctionIndex::Snapshot{}));
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        for (std::size_t slot = 0; slot < PATH_BOTS; ++slot)
        {
            SkillPath::PathBot const& bot = night.bots[band][slot];
            SkillPath::Path const before = SkillPath::Weigh(bot, bare);
            auto const material =
                std::find_if(before.materials.begin(), before.materials.end(),
                             [](SkillPath::Material const& each)
                             {
                                 return each.wanted && each.copperEach > 1 &&
                                        each.source != static_cast<std::uint8_t>(Materials::Kind::Blocked);
                             });
            if (material == before.materials.end())
                continue;
            std::int64_t const now = 1000;
            std::uint32_t const fed = material->copperEach / 2;
            AuctionIndex::Book book;
            AuctionIndex::AuctionFacts sold;
            sold.id = 1;
            sold.item = material->item;
            sold.count = 1;
            sold.startBid = fed;
            sold.buyout = fed;
            sold.endTime = now + AuctionIndex::SECONDS_PER_DAY;
            sold.houseId = AuctionRules::HOUSE_NEUTRAL;
            book.Add(sold);
            sold.bid = fed;
            sold.bidder = 2;
            book.Sale(sold, now);
            book.Remove(sold.id);
            AuctionIndex::AuctionFacts standing = sold;
            standing.id = 2;
            standing.bid = 0;
            standing.bidder = 0;
            book.Add(standing);
            AuctionIndex::Snapshot snapshot;
            book.Fill(snapshot, now,
                      static_cast<std::int64_t>(AuctionConfig{}.HistoryDays) * AuctionIndex::SECONDS_PER_DAY);
            SkillPath::Path const after = SkillPath::Weigh(bot, night.WorldOf(night.ViewOf(snapshot)));
            auto const priced =
                std::find_if(after.materials.begin(), after.materials.end(),
                             [&material](SkillPath::Material const& each) { return each.item == material->item; });
            bool const auction = priced != after.materials.end() &&
                                 priced->source == static_cast<std::uint8_t>(Materials::Kind::Auction) &&
                                 priced->otherCopperEach;
            Check(auction, "path control: item " + std::to_string(material->item) + " fed at " + std::to_string(fed) +
                               " c is the path's auction material");
            if (!auction)
                return "path control: the fed material is not taken from the auction";
            std::vector<AuctionRules::BuyLine> const lines =
                night.LinesOf(after, bot, [](std::uint32_t) { return 0u; });
            auto const line = std::find_if(lines.begin(), lines.end(), [&material](AuctionRules::BuyLine const& each)
                                           { return each.item == material->item; });
            // the other-source cost over the bot's need factor against the exchange (AuctionBuyList.h PathLine)
            std::uint64_t const otherCeiling = AuctionRules::Ceiling(
                {static_cast<double>(priced->otherCopperEach), static_cast<double>(priced->otherCopperEach),
                 AuctionRules::ShareOf({bot.rates.exchange, bot.rates.factor})});
            bool const made = line != lines.end() && AuctionRules::CeilingOf(*line) == otherCeiling;
            Check(made, "path control: a path line with ceiling = otherCopperEach " +
                            std::to_string(priced->otherCopperEach) + " over the share, " +
                            std::to_string(otherCeiling));
            if (!made)
                return "path control: no path line at the other-source cost";
            AuctionRules::BuyInput buyer;
            buyer.spendable = bot.money;
            buyer.openBidsMax = AuctionConfig{}.OpenBidsMax;
            buyer.now = now;
            buyer.ownGuid = 3;
            AuctionIndex::ItemEntry const* entry = snapshot.Find(material->item);
            AuctionIndex::CurrentOffers const none;
            AuctionRules::BuyChoice const choice =
                AuctionRules::ChooseLine(entry ? entry->offers : none, *line, AuctionRules::HOUSE_NEUTRAL, {}, buyer);
            Check(choice.kind != AuctionRules::BuyKind::Skip, "path control: the bot buys the fed material");
            std::ostringstream text;
            text << "path control (band " << band << ", " << CraftName(ProfessionRules::CRAFTS[slot].Which)
                 << "): item " << material->item << ", cheapest " << material->copperEach << " c by "
                 << Materials::KindWord(static_cast<Materials::Kind>(material->source)) << "; one sale fed at " << fed
                 << " c makes it an auction material, its path line has " << line->units << " units at the ceiling "
                 << AuctionRules::CeilingOf(*line) << " c (otherCopperEach " << priced->otherCopperEach
                 << "), and the buy rule "
                 << (choice.kind == AuctionRules::BuyKind::Skip ? "skips it" : "buys the standing lot") << " at "
                 << choice.price << " c";
            return text.str();
        }
    Check(false, "path control: a typical path has a material with a source");
    return "path control: no typical path has a material with a source";
}

// ---- presets ----
// A preset is a row: its name, what it sets up and the market it builds from the repo's fixtures and the seed; after
// the run, its own checks and print (the night beside the model)
struct Built
{
    MarketSim::Market market;
    std::function<void(MarketSim::Market const&, Result const&, bool)> after;
};

struct Preset
{
    std::string_view name;
    std::string_view what;
    Built (*make)(std::string const& repo, std::uint32_t seed);
};

Built HandPreset(std::string const&, std::uint32_t seed) { return {HandMarket(seed), {}}; }

std::string Pair(double night, double model)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(0) << night << " / " << model;
    return text.str();
}

std::string NightSkips(std::map<std::string, std::uint64_t> const& skips)
{
    std::ostringstream text;
    for (auto const& [reason, rows] : skips)
        text << (text.str().empty() ? "" : ", ") << reason << ' ' << rows;
    return text.str().empty() ? "-" : text.str();
}

std::uint64_t Sum(std::array<std::uint64_t, AuctionWords::REASON_COUNT> const& counts)
{
    std::uint64_t total = 0;
    for (std::uint64_t const count : counts)
        total += count;
    return total;
}

double Value(Night const& night, std::string const& name)
{
    auto const found = night.values.find(name);
    return found == night.values.end() ? 0.0 : found->second;
}

// Bag lines by band (AH9's exit): the night's over_ceiling and buy rows on bags, the model's over_ceiling skips and
// bag sales by the buyer's band, and the bags the model sold there
void PrintBagBands(std::ostream& out, Night const& night, MarketSim::Market const& market, Result const& result)
{
    out << "\n### Bag lines by band (night / model)\n\n";
    out << "| band | over_ceiling | buys or sales | budget skips (model) | bags sold (model, item x sales) |\n"
           "|---|---|---|---|---|\n";
    for (std::uint32_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
    {
        double nightOver = 0.0;
        double nightBuys = 0.0;
        for (auto const& [key, bag] : night.bags)
            if (key.first == band)
            {
                nightOver += bag.overRows;
                nightBuys += bag.buyRows;
            }
        std::uint64_t over = 0;
        std::uint64_t sales = 0;
        std::uint64_t budget = 0;
        std::ostringstream sold;
        for (BagRung const& rung : market.ladder)
        {
            auto const found = result.items.find(rung.item);
            if (found == result.items.end())
                continue;
            over += found->second.bagSkips[band][Slot(AuctionWords::Reason::OverCeiling)];
            budget += found->second.bagSkips[band][Slot(AuctionWords::Reason::Budget)];
            if (std::uint64_t const count = found->second.bandSales[band])
            {
                sales += count;
                sold << (sold.str().empty() ? "" : ", ") << rung.item << " (" << rung.slots << ") x" << count;
            }
        }
        out << "| " << band << " | " << Pair(nightOver, static_cast<double>(over)) << " | "
            << Pair(nightBuys, static_cast<double>(sales)) << " | " << budget << " | "
            << (sold.str().empty() ? "-" : sold.str()) << " |\n";
    }
}

// The bot's copper weight at a purse against its reserve, neutral traits, a level of the band (AuctionMarketSim.h
// WeightOf)
AuctionRules::CopperWeight WeightAt(std::uint32_t band, double money, double reserve, float greed = 0.0f)
{
    Planner::Snapshot bot;
    bot.level = static_cast<std::uint8_t>(WorldTravel::LEVEL_BAND_TOP[band]);
    bot.money = Whole(money);
    Planner::Goals::Weights weights;
    weights.greed = greed;
    Planner::Goals::Need const need = Planner::Goals::NeedFor(bot, weights, Whole(reserve));
    return {need.exchange, need.factor};
}

// The bag ladder per band (AH9's premise): each bag a band's bots met at the auctioneer (`bagline`), its time worth
// at the band's median free slots over its most worn smallest bag, the ceilings the rules give it there, and the
// night's over_ceiling rows (their median ceiling and ask). A comparison, no input
void PrintLadder(std::ostream& out, Night const& night, MarketSim::Market const& market)
{
    out << "\n### The bag ladder by band (AH9)\n\n";
    out << "Time worth: WorldTravel::BagWorth at the band's default rates, its median free slots (`carried` q2) and "
           "its "
           "most worn smallest bag (`smallbag`); vendor price only where a spawned vendor stocks the bag "
           "(ItemInput::vendorStocked). AH8: the old rule, min(time worth, template price). AH9 at the reserve (share "
           "1), for the band's median bot (`carried` q2 money against money less spendable; its share in brackets) "
           "and flush (twice the reserve), neutral greed. Flush multiple of the time worth by greed -1 / 0 / +1: ";
    for (float const greed : {-1.0f, 0.0f, 1.0f})
        out << (greed > -1.0f ? " / " : "") << std::fixed << std::setprecision(0)
            << 1.0f / AuctionRules::ShareOf(WeightAt(0, 2.0, 1.0, greed)) << "x";
    out << ". Night: the over_ceiling rows' median ceiling and ask (`bagline`, any free slots and bags). Copper.\n\n";
    out << "| band | bag | slots | vendor price | smallest / free | time worth | AH8 | at reserve | median bot | flush "
           "| night ceiling q2 | night ask q2 | over_ceiling rows | buy rows |\n"
           "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (std::uint32_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
    {
        NightBand const& row = night.bands[band];
        std::uint32_t smallest = 0;
        double most = -1.0;
        for (auto const& [slots, bots] : row.smallBags)
            if (bots > most)
            {
                most = bots;
                smallest = slots;
            }
        std::uint32_t const freeSlots = Whole(row.freeSlots[1]);
        AuctionRules::CopperWeight const median = WeightAt(band, row.money[1], row.money[1] - row.spendable[1]);
        AuctionRules::CopperWeight const atReserve = WeightAt(band, 1.0, 1.0);
        AuctionRules::CopperWeight const flush = WeightAt(band, 2.0, 1.0);
        WorldTravel::SlotBand const& slots = market.bands[band].slots;
        WorldTravel::SlotRates const rates = WorldTravel::CurrentRates(WorldTravel::SlotMeasure{}, slots);
        std::vector<BagRung> rungs;
        for (BagRung const& rung : market.ladder)
            if (night.bags.count({band, rung.item}))
                rungs.push_back(rung);
        std::stable_sort(rungs.begin(), rungs.end(),
                         [](BagRung const& left, BagRung const& right) { return left.slots < right.slots; });
        for (BagRung const& rung : rungs)
        {
            NightBag const& seen = night.bags.at({band, rung.item});
            std::uint32_t const worth =
                WorldTravel::BagWorth(rates, freeSlots, rung.slots, smallest, slots.BagHorizonHours);
            std::uint32_t const vendor = rung.stocked ? rung.buyCopper : 0;
            auto const at = [&](AuctionRules::CopperWeight weight)
            { return AuctionRules::CeilingOf(AuctionRules::BagLine(rung.item, worth, vendor, weight)); };
            out << "| " << band << " | " << rung.item << " | " << rung.slots << " | "
                << (vendor ? std::to_string(vendor) : std::string("-")) << " | " << smallest << " / " << freeSlots
                << " | " << worth << " | " << std::min(worth, rung.buyCopper) << " | " << at(atReserve) << " | "
                << at(median) << " (" << std::setprecision(2) << AuctionRules::ShareOf(median) << ") | " << at(flush)
                << " | " << std::fixed << std::setprecision(0) << seen.ceilingQ2 << " | " << seen.askQ2 << " | "
                << seen.overRows << " | " << seen.buyRows << " |\n";
        }
    }
}

void PrintNight(std::ostream& out, Night const& night, NightPaths const& paths, MarketSim::Market const& market,
                Result const& result, std::string const& control)
{
    out.unsetf(std::ios::floatfield);
    out << std::setprecision(6) << "\n## The night beside the model\n\n";
    out << "- Night: `" << night.header << "`: " << Value(night, "bot_hours") << " bot hours, " << Value(night, "bots")
        << " bots. Model: " << market.bots.size() << " bots (a band's bot hours over " << night.hours << " h), "
        << market.hours << " model hours.\n";
    out << "- From the night: visits per bot hour (`band` visits / bot_hours), lots per bot hour (`band` posts / "
           "bot_hours) and their mix (`item` posts, units per lot = units / listings), income and spending per bot "
           "hour (`income` without source auction), purse and spendable (`carried` quartiles, one draw per bot), free "
           "slots (`carried` quartiles), the bag ladder (`ladder`), use lines (`useline`: a bot has one with the "
           "chance rows / band visits, one unit, worth = ceiling_copper_q2, built by AuctionRules::UseLine at the "
           "bot's weight at each visit as PlannerUpkeep.cpp BuyListOf).\n";
    out << "- Assumed (the fixture's assumed sections): reserve = money less spendable at the bot's draw; the smallest "
           "bag from `smallbag` (the night's end); the stream of lots above (only listed lots are seen: lots the bots "
           "held or sold to vendors do not come by); between the quartiles a linear draw, the tails at q1 and q3; "
           "path lines from SkillPath::Weigh of one typical bot per band and craft (below); the house starts empty "
           "and the bots carry no stock (U2).\n";
    out << "- Not inputs: the bag lines' ceilings (`bagline` ceilings are a comparison only: the model's bag lines "
           "are AuctionRules::BagLine of WorldTravel::BagWorth at the bot's weight at each visit: NeedFor with its "
           "reserve, neutral traits, as PlannerUpkeep.cpp BuyListOf).\n\n";
    out << "### By kind (night / model)\n\n";
    out << "| kind | listings | posts | relists | holds | sales | buys | unbought: night | unbought: model | in the "
           "house at the end |\n";
    out << "|---|---|---|---|---|---|---|---|---|---|\n";
    std::map<std::uint32_t, KindTally> const model = result.ByKind();
    std::set<std::uint32_t> kinds;
    for (auto const& [kind, tally] : night.kinds)
        kinds.insert(kind);
    for (auto const& [kind, tally] : model)
        kinds.insert(kind);
    for (std::uint32_t const kind : kinds)
    {
        auto const seen = night.kinds.find(kind);
        NightKind const empty;
        NightKind const& at = seen == night.kinds.end() ? empty : seen->second;
        auto const ran = model.find(kind);
        KindTally const none;
        KindTally const& tally = ran == model.end() ? none : ran->second;
        auto const lots = result.houseLots.find(kind);
        out << "| " << KindWord(kind) << " | "
            << Pair(static_cast<double>(at.posts + at.relists), static_cast<double>(tally.posts + tally.relists))
            << " | " << Pair(static_cast<double>(at.posts), static_cast<double>(tally.posts)) << " | "
            << Pair(static_cast<double>(at.relists), static_cast<double>(tally.relists)) << " | "
            << Pair(static_cast<double>(at.holds), static_cast<double>(Sum(tally.holds))) << " | "
            << Pair(static_cast<double>(at.sales), static_cast<double>(tally.sales)) << " | "
            << Pair(static_cast<double>(at.buys), static_cast<double>(tally.sales)) << " | " << NightSkips(at.skips)
            << " | " << Unbought(tally) << " | "
            << Pair(static_cast<double>(at.houseLots),
                    static_cast<double>(lots == result.houseLots.end() ? 0 : lots->second))
            << " |\n";
    }
    KindTally const total = result.Total();
    std::uint64_t modelOver = 0;
    std::uint64_t modelNone = 0;
    for (auto const& owner : total.skips)
    {
        modelOver += owner[Slot(AuctionWords::Reason::OverCeiling)];
        modelNone += owner[Slot(AuctionWords::Reason::NoOffer)];
    }
    out << "\nNight sales are AuctionSale events of online sellers only; night buys are the `bagline` buy rows "
           "(AuctionBuy events: "
        << Value(night, "buys") << "); a model sale is a buy. Totals (night / model): listings "
        << Pair(Value(night, "listings"), static_cast<double>(total.posts + total.relists)) << ", sales "
        << Pair(Value(night, "sales"), static_cast<double>(total.sales)) << ", holds "
        << Pair(Value(night, "holds"), static_cast<double>(Sum(total.holds))) << ", unbought over_ceiling "
        << Pair(Value(night, "skip_over_ceiling"), static_cast<double>(modelOver)) << ", no_offer "
        << Pair(Value(night, "skip_no_offer"), static_cast<double>(modelNone)) << ", expiries - / " << total.expiries
        << ", visits " << Pair(Value(night, "visits"), static_cast<double>(result.visits)) << ".\n\n";
    out << "### By band (night / model)\n\n";
    out << "| band | levels | bot hours | visits | listings | buys |\n|---|---|---|---|---|---|\n";
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
    {
        NightBand const& row = night.bands[band];
        out << "| " << band << " | " << row.levelLow << "-" << row.levelHigh << " | " << std::fixed
            << std::setprecision(0) << row.botHours << " | "
            << Pair(row.visits, static_cast<double>(result.bandVisits[band])) << " | "
            << Pair(row.posts + row.relists, static_cast<double>(result.bandListings[band])) << " | "
            << Pair(row.buys, static_cast<double>(result.bandBuys[band])) << " |\n";
    }
    out << "\n### Bags (night / model)\n\n";
    out << "The ladder's bags with a listing or a bag line in either. Unbought: decision rows (night) and skips "
           "(model) "
           "by reason. A bag is a candidate only above the bot's smallest bag (PlannerUpkeep.cpp:1042); the smallest "
           "bag is the night's end state (`smallbag`, after the night's purchases), by band in slots: bots:";
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
    {
        out << (band ? ";" : "") << " band " << band;
        for (auto const& [slots, bots] : night.bands[band].smallBags)
            out << ' ' << slots << ':' << std::fixed << std::setprecision(0) << bots;
    }
    out << ".\n\n| bag | slots | vendor price | listings | sales or buys | unbought: night | unbought: model |\n"
           "|---|---|---|---|---|---|---|\n";
    std::vector<BagRung> ladder = night.ladder;
    std::stable_sort(ladder.begin(), ladder.end(),
                     [](BagRung const& left, BagRung const& right) { return left.slots < right.slots; });
    for (BagRung const& rung : ladder)
    {
        auto const seen = night.items.find(rung.item);
        auto const ran = result.items.find(rung.item);
        if (seen == night.items.end() && ran == result.items.end())
            continue;
        NightItem const noNight;
        ItemTally const noModel;
        NightItem const& at = seen == night.items.end() ? noNight : seen->second;
        ItemTally const& tally = ran == result.items.end() ? noModel : ran->second;
        out << "| " << rung.item << " | " << rung.slots << " | " << rung.buyCopper << " | "
            << Pair(static_cast<double>(at.listings), static_cast<double>(tally.listings)) << " | "
            << Pair(static_cast<double>(at.buys), static_cast<double>(tally.sales)) << " | " << NightSkips(at.skips)
            << " | " << Reasons(tally.skips) << " |\n";
    }
    PrintBagBands(out, night, market, result);
    PrintLadder(out, night, market);
    out << "\n### Path lines (assumed input)\n\n";
    out << "Weighed " << paths.weighs << " times (the start and every model hour), " << static_cast<long>(paths.ms)
        << " ms in all; path lines at the full want over all weighs: " << paths.linesSeen
        << ". One typical bot per band and craft: the band's middle level, the craft and its gatherer at "
        << SKILL_PER_LEVEL
        << " points per level (assumed), the band's median purse above its reserve, rates by "
           "SkillPathBridge::RatesOf, every tool. Materials of the last weigh as `item xunits: copper each by "
           "source`; a path line needs the auction as the cheapest source, which needs a typical price "
           "(SkillPathRules.h:948-968, AuctionBuyList.h PathLines).\n\n";
    out << "| band | craft | level | skill | path | materials | with a typical price | from the auction | path lines "
           "| materials (cheapest source) |\n|---|---|---|---|---|---|---|---|---|---|\n";
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        for (std::size_t slot = 0; slot < PATH_BOTS; ++slot)
        {
            SkillPath::PathBot const& bot = paths.bots[band][slot];
            SkillPath::Path const& path = paths.paths[band][slot];
            std::size_t typical = 0;
            std::size_t auction = 0;
            std::ostringstream list;
            for (SkillPath::Material const& material : path.materials)
            {
                typical += paths.view.Typical(material.item).unitPrice ? 1 : 0;
                auction += material.source == static_cast<std::uint8_t>(Materials::Kind::Auction) ? 1 : 0;
                list << (list.str().empty() ? "" : ", ") << material.item << " x" << material.used << ": "
                     << material.copperEach << " c by "
                     << Materials::KindWord(static_cast<Materials::Kind>(material.source));
            }
            out << "| " << band << " | " << CraftName(ProfessionRules::CRAFTS[slot].Which) << " | " << bot.level
                << " | " << bot.skills[0].value << "/" << bot.skills[0].max << " | "
                << SkillPath::ReasonWord(path.reason) << " | " << path.materials.size() << " | " << typical << " | "
                << auction << " | " << paths.LinesOf(path, bot, [](std::uint32_t) { return 0u; }).size() << " | "
                << (list.str().empty() ? "-" : list.str()) << " |\n";
        }
    out << "\n" << control << ".\n";
}

// An item's row of the market from its template and its reference value
Goods GoodsRow(std::unordered_map<std::uint32_t, AuctionRules::ItemInput> const& templates,
               AuctionRules::Table const& table, std::uint32_t item)
{
    auto const found = templates.find(item);
    AuctionRules::ItemReference const* reference = table.Find(item);
    Goods goods;
    goods.item = item;
    goods.reference = reference ? reference->copper : 0;
    if (found == templates.end())
        return goods;
    goods.classId = found->second.classId;
    goods.quality = found->second.quality;
    goods.sellPrice = found->second.sellPrice;
    goods.requiredLevel = found->second.requiredLevel;
    goods.itemLevel = found->second.itemLevel;
    return goods;
}

Built NightPreset(std::string const& repo, std::uint32_t seed)
{
    Built built;
    MarketSim::Market& market = built.market;
    market.seed = seed;
    auto const night = std::make_shared<Night>();
    std::string const text = ReadFile(repo + "/tests/fixtures/auction-market-night.txt");
    auto const index = std::make_shared<Recipes::RecipeIndex>();
    std::string error;
    bool const recipes =
        Recipes::RecipeIndex::Parse(ReadFile(repo + "/tests/fixtures/recipe-index-dump.txt"), *index, error);
    Check(recipes, "night: the recipe index dump parses: " + error);
    std::vector<AuctionRules::ItemInput> inputs;
    std::string core;
    bool const parsed =
        AuctionRules::ParseInputs(ReadFile(repo + "/tests/fixtures/auction-reference-inputs.txt"), inputs, core, error);
    Check(parsed, "night: the reference inputs parse: " + error);
    Check(!text.empty(), "night: the fixture reads");
    if (!recipes || !parsed || text.empty())
        return built;
    std::unordered_map<std::uint32_t, AuctionRules::ItemInput> templates;
    std::unordered_map<std::uint32_t, std::uint32_t> sell;
    for (AuctionRules::ItemInput const& input : inputs)
    {
        templates[input.id] = input;
        sell[input.id] = input.sellPrice;
    }
    auto const classOf = [&templates](std::uint32_t item)
    {
        auto const found = templates.find(item);
        return found == templates.end() ? UNKNOWN_CLASS : found->second.classId;
    };
    ReadNight(text, *night, classOf);
    Check(night->hours > 0.0 && night->values.count("bot_hours"), "night: the fixture's header and night rows read");
    if (!(night->hours > 0.0))
        return built;
    market.hours = night->hours;
    market.ladder = night->ladder;
    for (BagRung& rung : market.ladder)
    {
        auto const found = templates.find(rung.item);
        rung.stocked = found != templates.end() && found->second.vendorStocked;
    }
    // The reference value as AuctionRules::Tables builds it (auction-rules-selftest.cpp Tables)
    AuctionRules::Table const table = AuctionRules::Build(inputs, *index, WorldTravel::DEFAULT_SLOT_BANDS);
    std::set<std::uint32_t> items;
    for (BagRung const& rung : night->ladder)
        items.insert(rung.item);
    std::mt19937 rng(seed);
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
    {
        NightBand const& row = night->bands[band];
        Band& model = market.bands[band];
        model.slots = WorldTravel::DEFAULT_SLOT_BANDS[band];
        if (!(row.botHours > 0.0))
            continue;
        model.visitsPerHour = row.visits / row.botHours;
        model.lotsPerHour = row.posts / row.botHours;
        model.mix = row.mix;
        model.incomePerHour = row.incomeIn / row.botHours;
        model.spendPerHour = row.incomeOut / row.botHours;
        for (Arrival const& arrival : row.mix)
            items.insert(arrival.item);
        for (NightUse const& use : row.uses)
            items.insert(use.item);
        double bagBots = 0.0;
        for (auto const& [slots, bots] : row.smallBags)
            bagBots += bots;
        std::uint32_t const count = Whole(row.botHours / night->hours);
        for (std::uint32_t at = 0; at < count; ++at)
        {
            BotSetup bot;
            bot.band = static_cast<std::uint8_t>(band);
            double const unit = Unit(rng);
            double const money = Quartile(unit, row.money);
            double const spendable = Quartile(unit, row.spendable);
            bot.money = Whole(money);
            bot.reserve = money > spendable ? Whole(money - spendable) : 0;
            bot.freeSlots = Whole(Quartile(Unit(rng), row.freeSlots));
            double pick = Unit(rng) * bagBots;
            for (auto const& [slots, bots] : row.smallBags)
            {
                bot.smallestBag = slots;
                if ((pick -= bots) < 0.0)
                    break;
            }
            for (NightUse const& use : row.uses)
                if (Unit(rng) * row.visits < use.rows)
                {
                    // PlannerUpkeep.cpp BuyListOf: the price of one unit where a vendor stocks it, the template's
                    // sell price; priced at the bot's weight at each visit (AuctionMarketSim.h AtWeight)
                    auto const found = templates.find(use.item);
                    std::uint32_t const vendorEach =
                        found == templates.end() || !found->second.vendorStocked
                            ? 0
                            : found->second.buyPrice / std::max<std::uint32_t>(found->second.buyCount, 1);
                    bot.lines.push_back(AuctionRules::UseLine(use.item, 1, use.ceilingCopper, vendorEach,
                                                              found == templates.end() ? 0 : found->second.sellPrice,
                                                              {}));
                }
            bot.pathSlot = at % PATH_BOTS;
            market.bots.push_back(bot);
        }
    }
    for (std::uint32_t const item : items)
        market.goods.push_back(GoodsRow(templates, table, item));
    auto const paths = std::make_shared<NightPaths>(index, std::move(sell), market.fees, market.config, *night);
    market.paths = paths;
    built.after = [night, paths](MarketSim::Market const& ran, Result const& result, bool print)
    {
        std::string const control = PathControl(*paths);
        if (print)
            PrintNight(std::cout, *night, *paths, ran, result, control);
        else
            std::cout << "night paths: " << paths->weighs << " weighs in " << static_cast<long>(paths->ms)
                      << " ms, path lines over all weighs " << paths->linesSeen << "\n"
                      << control << '\n';
    };
    return built;
}

// ---- the gear presets (PLAN 3.2, AH5) ----
// gear-no-buyer is the night as it is: it has no gear line. gear-buyers-arrive adds gear buyers from a later model
// hour on; all of what they are is assumed, and the print says so. gear-buyers-worth (gear WG7) is the same draw with
// each line's worth from GearWorthRules.h (R3) instead of the item's reference value
constexpr std::uint32_t ITEM_CLASS_WEAPON = 2;
constexpr std::uint32_t ITEM_CLASS_ARMOR = 4;
constexpr std::size_t GEAR_LINES = 17;   // assumed: as many as the equipment slots; the draw ignores slots
constexpr double GEAR_FROM_HOUR = 7.0;   // assumed: the first full model hour after the night's 6.7
constexpr double GEAR_RUN_HOURS = 48.0;  // assumed: the longest listing duration, so the house turns over
// gear-buyers-worth: the share of the bot's gear strength a line adds, drawn per line (gear-worth-selftest's ladder
// gains); the summed score is any positive number, since the worth reads the share
constexpr std::array<double, 5> GEAR_GAINS = {0.01, 0.02, 0.03, 0.05, 0.10};
constexpr float GEAR_SCORE_SUM = 100.0f;

enum class GearBuyers : std::uint8_t
{
    None,         // gear-no-buyer
    ByReference,  // gear-buyers-arrive: worth = the item's reference value
    ByWorth       // gear-buyers-worth: worth = GearWorth::Worth
};

bool IsGear(std::uint32_t classId) { return classId == ITEM_CLASS_WEAPON || classId == ITEM_CLASS_ARMOR; }

// The sim's WeightOf (AuctionMarketSim.h) at the bot's starting purse and reserve
AuctionRules::CopperWeight SetupWeight(BotSetup const& bot)
{
    Planner::Snapshot snapshot;
    snapshot.level = static_cast<std::uint8_t>(WorldTravel::LEVEL_BAND_TOP[bot.band]);
    snapshot.money =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(bot.money, std::numeric_limits<std::uint32_t>::max()));
    Planner::Goals::Need const need = Planner::Goals::NeedFor(snapshot, Planner::Goals::Weights{}, bot.reserve);
    return {need.exchange, need.factor};
}

// A gear line worth what GearWorthRules.h gives a bot of the item's required level for the drawn gain into an empty
// slot: the band's configured earn rate (the user's open question on the earn rate changes this input only), the
// band's hours per level, the item's level; priced as the visit prices it (GearWorth::GearLine, vendor payout as the
// excess worth)
AuctionRules::BuyLine WorthLine(Goods const& goods, double gain, AuctionRules::CopperWeight weight)
{
    std::uint32_t const level = std::max<std::uint32_t>(1, goods.requiredLevel);
    GearWorth::WorthInput in;
    in.itemScore = static_cast<float>(gain) * GEAR_SCORE_SUM;
    in.wornScore = 0.0f;
    in.summedScore = GEAR_SCORE_SUM;
    in.level = level;
    in.itemLevel = goods.itemLevel;
    in.earnCopperPerMinute =
        static_cast<float>(WorldTravel::DEFAULT_SLOT_BANDS[WorldTravel::LevelBand(level)].EarnCopperPerMinute);
    in.hoursPerLevel = GearWorth::HoursPerLevel(level);
    AuctionRules::BuyLine line = GearWorth::GearLine(goods.item, GearWorth::Worth(in), 0, weight);
    line.excessUnitWorth = goods.sellPrice;
    return line;
}

// Gear per model hour; with buyers, what they are and when sales and listings came back
void PrintGear(std::ostream& out, MarketSim::Market const& market, Result const& result, GearBuyers buyers)
{
    std::set<std::uint32_t> soldKinds;  // gear kinds with a sale in the run
    for (HourTally const& hour : result.hours)
        for (auto const& [kind, tally] : hour.levelKinds)
            if (tally.sales && IsGear((kind >> 16) & 0xFF))
                soldKinds.insert(kind);
    struct GearHour
    {
        std::uint64_t listings{0};
        std::uint64_t sales{0};
        std::uint64_t standing{0};
        std::uint64_t vendored{0};
        std::uint64_t soldKindListings{0};
    };
    std::vector<GearHour> rows(result.hours.size());
    for (std::size_t hour = 0; hour < result.hours.size(); ++hour)
    {
        for (auto const& [classId, tally] : result.hours[hour].kinds)
            if (IsGear(classId))
            {
                rows[hour].listings += tally.posts + tally.relists;
                rows[hour].sales += tally.sales;
                rows[hour].standing += tally.standing;
                rows[hour].vendored += tally.vendored;
            }
        for (auto const& [kind, tally] : result.hours[hour].levelKinds)
            if (soldKinds.count(kind))
                rows[hour].soldKindListings += tally.listings;
    }
    out << "\n## Gear (weapons and armor) per model hour\n\n";
    if (buyers == GearBuyers::None)
        out << "- No bot holds a gear line (the night as it is): units wanted " << result.gearUnitsWanted << ".\n";
    else
    {
        out << "- Assumed buyers: each of the " << market.bots.size() << " model bots takes up " << GEAR_LINES
            << " gear lines (a count; the draw ignores slots) at its first visit from model hour " << GEAR_FROM_HOUR
            << " on. A line is one unit of one item, drawn without replacement from the gear of its band's lot stream "
               "by the lots' weights (items with a reference value). ";
        if (buyers == GearBuyers::ByReference)
            out << "Worth = the item's reference value, no other way of getting it (AuctionRules::LineOf, owner "
                   "gear). ";
        else
            out << "Worth = GearWorth::Worth (GearWorthRules.h) of a bot at the item's required level (at least 1) "
                   "into an empty slot: the gain an assumed share of its gear strength drawn per line from 1, 2, 3, 5 "
                   "and 10 %, the band's configured earn rate (DEFAULT_SLOT_BANDS), the band's hours per level, the "
                   "item's level; the line as the visit builds it (GearWorth::GearLine at the bot's weight at its "
                   "starting purse and reserve, the vendor payout as excess worth). ";
        out << "A bought line goes, so the demand is finite; money, visits, lots and income stay the night's. Units "
               "wanted "
            << result.gearUnitsWanted << ", unbought at the end " << result.gearUnitsLeft << ".\n";
    }
    out << "- Gear kinds with a sale in the run: " << soldKinds.size() << ".\n\n";
    out << "| hour | listings | sales | lots standing | vendored | listings of the gear kinds with a sale |\n"
           "|---|---|---|---|---|---|\n";
    for (std::size_t hour = 0; hour < rows.size(); ++hour)
        out << "| " << HourWord(market, hour) << " | " << rows[hour].listings << " | " << rows[hour].sales << " | "
            << rows[hour].standing << " | " << rows[hour].vendored << " | " << rows[hour].soldKindListings << " |\n";
    constexpr std::size_t from = static_cast<std::size_t>(GEAR_FROM_HOUR);
    if (buyers == GearBuyers::None || rows.size() <= from)
        return;
    auto const first = [&rows](auto const& holds)
    {
        for (std::size_t hour = from; hour < rows.size(); ++hour)
            if (holds(rows[hour]))
                return "hour " + std::to_string(hour) + " (model hour " + std::to_string(hour - from + 1) +
                       " after the buyers arrive)";
        return std::string("no hour of the run");
    };
    GearHour const before = rows[from - 1];
    out << "\n- First hour with a gear sale: " << first([](GearHour const& row) { return row.sales > 0; }) << ".\n";
    out << "- First hour with more gear listings than hour " << from - 1 << " (" << before.listings
        << "): " << first([&before](GearHour const& row) { return row.listings > before.listings; }) << ".\n";
    out << "- First hour with more listings of the gear kinds with a sale than hour " << from - 1 << " ("
        << before.soldKindListings
        << "): " << first([&before](GearHour const& row) { return row.soldKindListings > before.soldKindListings; })
        << ".\n";
}

Built GearPreset(std::string const& repo, std::uint32_t seed, GearBuyers buyers)
{
    Built built = NightPreset(repo, seed);
    MarketSim::Market& market = built.market;
    if (buyers != GearBuyers::None)
    {
        market.hours = GEAR_RUN_HOURS;
        std::unordered_map<std::uint32_t, Goods> goods;
        for (Goods const& each : market.goods)
            goods[each.item] = each;
        // A generator of their own: the night's bots stay as they are; the gains another, so both presets draw the
        // same items
        std::mt19937 rng(seed ^ 0x9e3779b9u);
        std::mt19937 gains(seed ^ 0x51ed270bu);
        for (BotSetup& bot : market.bots)
        {
            AuctionRules::CopperWeight const weight = SetupWeight(bot);
            std::vector<std::pair<double, std::uint32_t>> drawn;  // key, item: the smallest keys are the draw
            for (Arrival const& arrival : market.bands[bot.band].mix)
            {
                Goods const& each = goods[arrival.item];
                if (IsGear(each.classId) && each.reference && arrival.weight > 0.0)
                    drawn.emplace_back(-std::log(Unit(rng)) / arrival.weight, arrival.item);
            }
            std::size_t const count = std::min(GEAR_LINES, drawn.size());
            std::partial_sort(drawn.begin(), drawn.begin() + count, drawn.end());
            for (std::size_t at = 0; at < count; ++at)
            {
                Goods const& each = goods[drawn[at].second];
                if (buyers == GearBuyers::ByWorth)
                    bot.laterLines.push_back(WorthLine(each, GEAR_GAINS[gains() % GEAR_GAINS.size()], weight));
                else
                    bot.laterLines.push_back(AuctionRules::LineOf(LineOwner::Gear, each.item, 1,
                                                                  static_cast<double>(each.reference), -1.0, 1.0f,
                                                                  each.sellPrice));
            }
            bot.laterFromHours = GEAR_FROM_HOUR;
        }
    }
    built.after = [buyers](MarketSim::Market const& ran, Result const& result, bool print)
    {
        if (buyers != GearBuyers::None && ran.hours > GEAR_FROM_HOUR)
            Check(result.gearUnitsWanted > 0, "gear buyers: the bots took up gear lines");
        if (print)
            PrintGear(std::cout, ran, result, buyers);
    };
    return built;
}

Built GearNoBuyerPreset(std::string const& repo, std::uint32_t seed)
{
    return GearPreset(repo, seed, GearBuyers::None);
}

Built GearBuyersPreset(std::string const& repo, std::uint32_t seed)
{
    return GearPreset(repo, seed, GearBuyers::ByReference);
}

Built GearWorthPreset(std::string const& repo, std::uint32_t seed)
{
    return GearPreset(repo, seed, GearBuyers::ByWorth);
}

// ---- the cold start (auction house round 2 AH8, PLAN 3.4) ----
// An empty history and house. The night's bots sell gathered surplus: every lot that comes by is one of the gathered
// goods of three kinds that their band's crafters need (assumed: the night listed no ore, bar or gathered herb); the
// same bots buy the lines of their skill path and nothing else. Two arms with the same seed:
// "before" sets the standing time longer than the run, so no offer ever prices an item (only a sale does, the rule
// before AH8); "after" keeps the default
constexpr std::uint32_t ITEM_CLASS_TRADE_GOODS = 7;
constexpr std::size_t TRADE_KIND_COUNT = 3;
constexpr std::array<std::uint32_t, TRADE_KIND_COUNT> TRADE_SUBCLASSES = {5, 7, 9};  // item_template's
constexpr std::array<std::string_view, TRADE_KIND_COUNT> TRADE_WORDS = {"cloth", "ore and bars", "herbs"};
constexpr double COLD_RUN_HOURS = 48.0;  // assumed: the longest listing duration, as the gear run's
constexpr std::size_t EXIT_BAND_FIRST = 1;
constexpr std::size_t EXIT_BAND_LAST = 3;
constexpr std::size_t PRICE_ITEMS = 2;  // per kind, the items whose price the print follows

struct TradeItem
{
    std::size_t kind{0};
    std::string name;
};

using TradeItems = std::map<std::uint32_t, TradeItem>;  // by item

struct ColdArm
{
    std::string_view name;
    std::uint32_t standingSeconds{0};
    Result const* result{nullptr};
    NightPaths const* paths{nullptr};
};

bool OfKind(TradeItems const& trade, std::uint32_t item, std::size_t kind)
{
    auto const found = trade.find(item);
    return found != trade.end() && found->second.kind == kind;
}

// A kind's sales to the buyers of the level bands first to last
std::uint64_t KindSales(TradeItems const& trade, Result const& result, std::size_t kind, std::size_t first,
                        std::size_t last)
{
    std::uint64_t total = 0;
    for (auto const& [item, tally] : result.items)
        if (OfKind(trade, item, kind))
            for (std::size_t band = first; band <= last; ++band)
                total += tally.bandSales[band];
    return total;
}

// A kind's path buyouts and bids, every band
std::uint64_t KindPlaced(TradeItems const& trade, Result const& result, std::size_t kind)
{
    std::uint64_t total = 0;
    for (auto const& [item, tally] : result.items)
        if (OfKind(trade, item, kind))
            for (std::uint64_t const count : tally.pathPlaced)
                total += count;
    return total;
}

// An item of the kind had a standing price (by offers) at some hour's end and a path line for it reached the
// auctioneer (bought, refused or skipped for any reason)
bool KindStandingAsked(TradeItems const& trade, Result const& result, std::size_t kind)
{
    for (auto const& [item, tally] : result.items)
    {
        if (!OfKind(trade, item, kind))
            continue;
        std::uint64_t asked = 0;
        for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
            asked += tally.pathPlaced[band] + tally.pathRefused[band] + Sum(tally.pathSkips[band]);
        bool const stood = std::any_of(result.hours.begin(), result.hours.end(),
                                       [item](MarketSim::HourTally const& hour)
                                       {
                                           auto const price = hour.prices.find(item);
                                           return price != hour.prices.end() &&
                                                  price->second.basis == AuctionIndex::PriceBasis::Offers;
                                       });
        if (asked && stood)
            return true;
    }
    return false;
}

std::uint64_t PathPlaced(Result const& result)
{
    std::uint64_t total = 0;
    for (auto const& [item, tally] : result.items)
        for (std::uint64_t const count : tally.pathPlaced)
            total += count;
    return total;
}

// A path's trade goods: units over the stretch, the cheapest source's cost and word
std::string TradeMaterials(TradeItems const& trade, NightPaths const& paths, SkillPath::Path const& path)
{
    std::ostringstream text;
    for (SkillPath::Material const& material : path.materials)
        if (auto const found = trade.find(material.item); found != trade.end())
            text << (text.str().empty() ? "" : ", ") << found->second.name << " x" << material.used << ": "
                 << material.copperEach << " c by "
                 << Materials::KindWord(static_cast<Materials::Kind>(material.source))
                 << (paths.view.Typical(material.item).unitPrice ? "" : " (no price)");
    return text.str().empty() ? "-" : text.str();
}

void PrintColdStart(std::ostream& out, MarketSim::Market const& market, TradeItems const& trade,
                    std::array<ColdArm, 2> const& arms)
{
    out.unsetf(std::ios::floatfield);
    out << std::setprecision(6) << "\n## The cold start: before and after\n\n";
    out << "- Arms (same bots, same seed, " << market.hours << " model hours, assumed): " << arms[0].name
        << " has the standing time " << arms[0].standingSeconds
        << " s, longer than the run, so only a sale prices an item; " << arms[1].name << " has the standing time "
        << arms[1].standingSeconds << " s (Playerbots.Auction.Market.StandingSeconds, the default). The sections above "
        << "this one are the " << arms[1].name << " arm.\n";
    out << "- From the night: the bots (count, purse, reserve, free slots), visits, income and lots per bot hour, and "
           "the units of a lot (the mean of its listings of the three kinds).\n";
    out << "- Assumed sellers (the night listed no ore, bar or herb a gatherer brings): every lot that comes by is "
           "gathered surplus of one of the three kinds (item class 7, subclass 5 cloth, 7 metal and stone, 9 herb), "
           "drawn evenly from the items of its band: what a recipe of one of the band's typical crafters needs "
           "(learnable by the rank's top, not grey at its skill, and what its made reagents are made of; the dumped "
           "recipe index), no vendor stocks, and either no recipe makes or a gatherer's does. By band:";
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        out << (band ? ";" : "") << " band " << band << ' ' << market.bands[band].mix.size() << " items";
    out << ". Items a vendor pays more for than a listing is worth go to the vendor, as the bags' plan says.\n";
    out << "- Assumed buyers (the night wrote no path lines): every bot follows one typical path bot of its band, "
           "which has its craft and no gathering skill (it buys what it does not gather) and climbs a stretch without "
           "asking what the climb brings (PathBot::anyStretch: the model prices no products, so a climb is worth "
           "nothing to it otherwise); level, skill at "
        << SKILL_PER_LEVEL << " points per level, purse and tools as the preset night's. No use or bag line.\n\n";

    out << "### Totals\n\n| arm | lots listed | sales | expiries | path lines over all weighs | path buyouts and bids "
           "| items priced at the end: by sales | by offers |\n|---|---|---|---|---|---|---|---|\n";
    for (ColdArm const& arm : arms)
    {
        KindTally const total = arm.result->Total();
        std::size_t bySales = 0;
        std::size_t byOffers = 0;
        for (auto const& [item, point] : arm.result->hours.back().prices)
            ++(point.basis == AuctionIndex::PriceBasis::Sales ? bySales : byOffers);
        out << "| " << arm.name << " | " << total.posts + total.relists << " | " << total.sales << " | "
            << total.expiries << " | " << arm.paths->linesSeen << " | " << PathPlaced(*arm.result) << " | " << bySales
            << " | " << byOffers << " |\n";
    }

    out << "\n### Sales per kind by the buyer's level band (before / after)\n\n| kind |";
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        out << " band " << band << " |";
    out << "\n|---|---|---|---|---|---|\n";
    for (std::size_t kind = 0; kind < TRADE_KIND_COUNT; ++kind)
    {
        out << "| " << TRADE_WORDS[kind] << " |";
        for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
            out << ' ' << KindSales(trade, *arms[0].result, kind, band, band) << " / "
                << KindSales(trade, *arms[1].result, kind, band, band) << " |";
        out << '\n';
    }

    out << "\n### Unbought path lines by cause\n\n";
    out << "Counted per visit and line: the buy rule's skips by reason, and the buys the core refused (the lot was "
           "gone, the bot's own, outbid or beyond the purse). An arm without a row had no path line: the cause is then "
           "in the paths below.\n\n";
    out << "| arm | kind | band | bought or bid | unbought by cause |\n|---|---|---|---|---|\n";
    for (ColdArm const& arm : arms)
        for (std::size_t kind = 0; kind < TRADE_KIND_COUNT; ++kind)
            for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
            {
                std::uint64_t placed = 0;
                std::uint64_t refused = 0;
                std::array<std::uint64_t, AuctionWords::REASON_COUNT> skips{};
                for (auto const& [item, tally] : arm.result->items)
                {
                    if (!OfKind(trade, item, kind))
                        continue;
                    placed += tally.pathPlaced[band];
                    refused += tally.pathRefused[band];
                    for (std::size_t reason = 0; reason < skips.size(); ++reason)
                        skips[reason] += tally.pathSkips[band][reason];
                }
                if (!placed && !refused && !Sum(skips))
                    continue;
                std::string causes = Sum(skips) ? Reasons(skips) : std::string();
                if (refused)
                    causes += (causes.empty() ? "refused " : ", refused ") + std::to_string(refused);
                out << "| " << arm.name << " | " << TRADE_WORDS[kind] << " | " << band << " | " << placed << " | "
                    << (causes.empty() ? "-" : causes) << " |\n";
            }

    out << "\nBy item (after), every trade good a path line asked for: its lots listed and sold, its price at the "
           "run's "
           "end (s: by sales, o: by offers), the path buys and the unbought lines by cause.\n\n";
    out << "| item | kind | listed | sold | price at the end | path buyouts and bids | unbought by cause |\n"
           "|---|---|---|---|---|---|---|\n";
    std::map<std::uint32_t, MarketSim::PricePoint> const& endPrices = arms[1].result->hours.back().prices;
    for (auto const& [item, tally] : arms[1].result->items)
    {
        std::uint64_t placed = 0;
        std::uint64_t refused = 0;
        std::array<std::uint64_t, AuctionWords::REASON_COUNT> skips{};
        for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        {
            placed += tally.pathPlaced[band];
            refused += tally.pathRefused[band];
            for (std::size_t reason = 0; reason < skips.size(); ++reason)
                skips[reason] += tally.pathSkips[band][reason];
        }
        auto const found = trade.find(item);
        if (found == trade.end() || (!placed && !refused && !Sum(skips)))
            continue;
        out << "| " << found->second.name << " | " << TRADE_WORDS[found->second.kind] << " | " << tally.listings
            << " | " << tally.sales << " | ";
        if (auto const price = endPrices.find(item); price == endPrices.end())
            out << '-';
        else
            out << price->second.unitPrice << (price->second.basis == AuctionIndex::PriceBasis::Sales ? " s" : " o");
        std::string causes = Sum(skips) ? Reasons(skips) : std::string();
        if (refused)
            causes += (causes.empty() ? "refused " : ", refused ") + std::to_string(refused);
        out << " | " << placed << " | " << (causes.empty() ? "-" : causes) << " |\n";
    }

    out << "\n### The paths' trade goods at the last weigh\n\n";
    out << "A path line needs the auction as the material's cheapest source, which needs a price. `item xunits: "
           "copper each by source`.\n\n";
    out << "| band | craft | before: path | before: trade goods | after: path | after: trade goods |\n"
           "|---|---|---|---|---|---|\n";
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        for (std::size_t slot = 0; slot < PATH_BOTS; ++slot)
        {
            out << "| " << band << " | " << CraftName(ProfessionRules::CRAFTS[slot].Which) << " |";
            for (ColdArm const& arm : arms)
            {
                SkillPath::Path const& path = arm.paths->paths[band][slot];
                out << ' ' << SkillPath::ReasonWord(path.reason) << " | " << TradeMaterials(trade, *arm.paths, path)
                    << " |";
            }
            out << '\n';
        }

    Result const& after = *arms[1].result;
    out << "\n### How the price moved (after)\n\n";
    out << "At each model hour's end: the trade goods with a price (by sales / by offers, the standing price), and the "
           "unit price in copper of each kind's "
        << PRICE_ITEMS << " most sold items (s: by sales, o: by offers, -: none).\n\n";
    std::vector<std::uint32_t> followed;
    for (std::size_t kind = 0; kind < TRADE_KIND_COUNT; ++kind)
    {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> sold;  // sales, item
        for (auto const& [item, tally] : after.items)
            if (OfKind(trade, item, kind) && tally.sales)
                sold.emplace_back(tally.sales, item);
        std::stable_sort(sold.begin(), sold.end(),
                         [](auto const& left, auto const& right) { return left.first > right.first; });
        for (std::size_t at = 0; at < std::min(PRICE_ITEMS, sold.size()); ++at)
            followed.push_back(sold[at].second);
    }
    out << "| hour |";
    for (std::string_view const word : TRADE_WORDS)
        out << ' ' << word << " priced |";
    for (std::uint32_t const item : followed)
        out << ' ' << trade.at(item).name << " |";
    out << "\n|---|";
    for (std::size_t column = 0; column < TRADE_KIND_COUNT + followed.size(); ++column)
        out << "---|";
    out << '\n';
    for (std::size_t hour = 0; hour < after.hours.size(); ++hour)
    {
        std::map<std::uint32_t, MarketSim::PricePoint> const& prices = after.hours[hour].prices;
        out << "| " << HourWord(market, hour) << " |";
        for (std::size_t kind = 0; kind < TRADE_KIND_COUNT; ++kind)
        {
            std::size_t bySales = 0;
            std::size_t byOffers = 0;
            for (auto const& [item, point] : prices)
                if (OfKind(trade, item, kind))
                    ++(point.basis == AuctionIndex::PriceBasis::Sales ? bySales : byOffers);
            out << ' ' << bySales << " / " << byOffers << " |";
        }
        for (std::uint32_t const item : followed)
        {
            auto const found = prices.find(item);
            if (found == prices.end())
                out << " - |";
            else
                out << ' ' << found->second.unitPrice
                    << (found->second.basis == AuctionIndex::PriceBasis::Sales ? " s |" : " o |");
        }
        out << '\n';
    }
}

// The units of a lot: the mean over the night's listings of the three kinds
std::uint32_t TradeLotUnits(MarketSim::Market const& market, TradeItems const& trade)
{
    double lots = 0.0;
    double units = 0.0;
    for (MarketSim::Band const& band : market.bands)
        for (Arrival const& arrival : band.mix)
            if (trade.count(arrival.item))
            {
                lots += arrival.weight;
                units += arrival.weight * arrival.units;
            }
    return lots > 0.0 ? std::max<std::uint32_t>(Whole(units / lots), 1) : 1;
}

// The gathered goods of the three kinds a band's typical crafters need: the reagents of their recipes learnable by
// the rank's top and not grey at their skill, and the reagents behind a reagent that is itself made. Gathered: no
// vendor stocks it and either no recipe makes it or a gatherer's does (a miner's bar)
std::set<std::uint32_t> SurplusOf(Recipes::RecipeIndex const& index, TradeItems const& trade,
                                  std::unordered_map<std::uint32_t, AuctionRules::ItemInput> const& templates,
                                  std::array<SkillPath::PathBot, PATH_BOTS> const& bots)
{
    std::unordered_map<std::uint32_t, std::vector<Recipes::Recipe const*>> makers;
    for (Recipes::Recipe const& recipe : index.Recipes())
        if (recipe.product)
            makers[recipe.product].push_back(&recipe);
    std::vector<std::uint32_t> open;
    for (SkillPath::PathBot const& bot : bots)
        for (Recipes::Recipe const& recipe : index.Recipes())
        {
            SkillPath::SkillLevel const& craft = bot.skills.front();
            if (recipe.skill == craft.skill && recipe.minRank <= craft.max && recipe.grey > craft.value)
                for (Recipes::ItemCount const& reagent : recipe.reagents)
                    open.push_back(reagent.item);
        }
    auto const gatherer = [](Recipes::Recipe const* recipe)
    {
        return std::any_of(std::begin(ProfessionRules::GATHERERS), std::end(ProfessionRules::GATHERERS),
                           [recipe](ProfessionRules::Skill skill)
                           { return recipe->skill == static_cast<std::uint16_t>(skill); });
    };
    std::set<std::uint32_t> seen;
    std::set<std::uint32_t> items;
    while (!open.empty())
    {
        std::uint32_t const item = open.back();
        open.pop_back();
        if (!seen.insert(item).second)
            continue;
        auto const made = makers.find(item);
        if (made != makers.end())
            for (Recipes::Recipe const* recipe : made->second)
                for (Recipes::ItemCount const& reagent : recipe->reagents)
                    open.push_back(reagent.item);
        bool const gathered = made == makers.end() || std::any_of(made->second.begin(), made->second.end(), gatherer);
        auto const known = templates.find(item);
        if (gathered && trade.count(item) && known != templates.end() && !known->second.vendorStocked)
            items.insert(item);
    }
    return items;
}

Built ColdStartPreset(std::string const& repo, std::uint32_t seed)
{
    Built built = NightPreset(repo, seed);
    MarketSim::Market& market = built.market;
    std::vector<AuctionRules::ItemInput> inputs;
    std::string core;
    std::string error;
    bool const parsed =
        AuctionRules::ParseInputs(ReadFile(repo + "/tests/fixtures/auction-reference-inputs.txt"), inputs, core, error);
    Recipes::RecipeIndex index;
    bool const recipes =
        Recipes::RecipeIndex::Parse(ReadFile(repo + "/tests/fixtures/recipe-index-dump.txt"), index, error);
    Check(parsed && recipes && market.paths, "cold start: the night's market and the dumps: " + error);
    if (!parsed || !recipes || !market.paths)
        return built;
    std::unordered_map<std::uint32_t, AuctionRules::ItemInput> templates;
    auto const trade = std::make_shared<TradeItems>();
    for (AuctionRules::ItemInput const& input : inputs)
    {
        templates[input.id] = input;
        for (std::size_t kind = 0; kind < TRADE_KIND_COUNT; ++kind)
            if (input.classId == ITEM_CLASS_TRADE_GOODS && input.subClass == TRADE_SUBCLASSES[kind])
                (*trade)[input.id] = {kind, input.name};
    }
    market.hours = COLD_RUN_HOURS;
    auto const paths = std::static_pointer_cast<NightPaths>(market.paths);
    for (auto& band : paths->bots)
        for (SkillPath::PathBot& bot : band)
        {
            bot.skills.resize(1);  // the craft without its gatherer
            bot.anyStretch = true;
        }
    AuctionRules::Table const table = AuctionRules::Build(inputs, index, WorldTravel::DEFAULT_SLOT_BANDS);
    std::uint32_t const units = TradeLotUnits(market, *trade);
    std::set<std::uint32_t> known;
    for (Goods const& goods : market.goods)
        known.insert(goods.item);
    for (std::size_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
    {
        market.bands[band].mix.clear();
        for (std::uint32_t const item : SurplusOf(index, *trade, templates, paths->bots[band]))
        {
            market.bands[band].mix.push_back({item, units, 1.0});
            if (known.insert(item).second)
                market.goods.push_back(GoodsRow(templates, table, item));
        }
    }
    for (BotSetup& bot : market.bots)
        bot.lines.clear();
    auto const unrun = std::make_shared<NightPaths>(*paths);
    built.after = [trade, paths, unrun](MarketSim::Market const& ran, Result const& result, bool print)
    {
        MarketSim::Market before = ran;
        before.config.StandingSeconds =
            static_cast<std::uint32_t>(std::llround(ran.hours * MarketSim::HOUR_SECONDS) + MarketSim::HOUR_SECONDS);
        auto const beforePaths = std::make_shared<NightPaths>(*unrun);
        before.paths = beforePaths;
        Result const locked = Sim(before).Run();
        CheckClean(locked, "preset cold-start, before");
        std::uint64_t lockedSales = 0;
        for (std::size_t kind = 0; kind < TRADE_KIND_COUNT; ++kind)
            lockedSales += KindSales(*trade, locked, kind, 0, WorldTravel::LEVEL_BAND_COUNT - 1);
        Check(!beforePaths->linesSeen && !PathPlaced(locked) && !lockedSales,
              "cold start before: no path line, path buy or trade sale (" + std::to_string(beforePaths->linesSeen) +
                  " lines, " + std::to_string(PathPlaced(locked)) + " buys, " + std::to_string(lockedSales) +
                  " sales)");
        // After (AH.18): the rule guarantees path buys where an item stands priced and a path asks the auction for
        // it; each kind's sales to the bands 1 to 3 are printed, not required (they depend on the reference values
        // and the paths' own sources, which the fixture assumes)
        Check(PathPlaced(result) > 0,
              "cold start after: path lines are bought (" + std::to_string(PathPlaced(result)) + " buyouts and bids)");
        std::ostringstream sales;
        for (std::size_t kind = 0; kind < TRADE_KIND_COUNT; ++kind)
        {
            std::uint64_t const sold = KindSales(*trade, result, kind, EXIT_BAND_FIRST, EXIT_BAND_LAST);
            sales << ", " << TRADE_WORDS[kind] << ' ' << sold;
            if (KindStandingAsked(*trade, result, kind))
                Check(KindPlaced(*trade, result, kind) > 0,
                      "cold start after: " + std::string(TRADE_WORDS[kind]) +
                          " stood priced by its offers and a path asked the auction for it: it is bought");
        }
        if (print)
            PrintColdStart(std::cout, ran, *trade,
                           {{{"before", before.config.StandingSeconds, &locked, beforePaths.get()},
                             {"after", ran.config.StandingSeconds, &result, paths.get()}}});
        else
            std::cout << "cold start: before path lines " << beforePaths->linesSeen << ", trade sales " << lockedSales
                      << "; after path lines " << paths->linesSeen << ", sales to the bands 1 to 3" << sales.str()
                      << '\n';
    };
    return built;
}

std::array<Preset, 6> const PRESETS = {
    {{"hand", "the selftest's hand market", &HandPreset},
     {"night", "lab5's night of 2026-11-25 (AH1's fixture)", &NightPreset},
     {"gear-no-buyer", "the night, its gear per hour", &GearNoBuyerPreset},
     {"gear-buyers-arrive", "the night, with assumed gear buyers from a later hour on", &GearBuyersPreset},
     {"gear-buyers-worth", "gear-buyers-arrive with each line's worth from GearWorthRules.h", &GearWorthPreset},
     {"cold-start", "gathered trade goods in an empty house, with assumed sellers and path buyers", &ColdStartPreset}}};

void RunPreset(std::string const& repo, std::string const& name, double hours, std::uint32_t seed, bool print)
{
    auto const preset =
        std::find_if(PRESETS.begin(), PRESETS.end(), [&name](Preset const& row) { return row.name == name; });
    Check(preset != PRESETS.end(), "the preset " + name + " exists");
    if (preset == PRESETS.end())
        return;
    auto const setup = std::chrono::steady_clock::now();
    Built built = preset->make(repo, seed);
    MarketSim::Market& market = built.market;
    if (hours > 0.0)
        market.hours = hours;
    auto const start = std::chrono::steady_clock::now();
    Result const result = Sim(market).Run();
    double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    double const setupMs = std::chrono::duration<double, std::milli>(start - setup).count();
    CheckClean(result, "preset " + name);
    if (print)
        Print(std::cout, preset->name, market, result, ms);
    else
        std::cout << "preset " << name << ": " << market.bots.size() << " bots, " << result.visits << " visits, "
                  << result.Total().posts + result.Total().relists << " listings, " << result.Total().sales
                  << " sales, setup " << static_cast<long>(setupMs) << " ms, run " << static_cast<long>(ms)
                  << " ms, digest " << Digest(result) << '\n';
    if (built.after)
        built.after(market, result, print);
}
}  // namespace

int main(int argc, char** argv)
{
    std::string repo = ".";
    std::string preset;
    double hours = 0.0;
    std::uint32_t seed = 1;
    bool print = false;
    for (int index = 1; index < argc; ++index)
    {
        std::string const arg = argv[index];
        bool const valued = index + 1 < argc;
        if (arg == "--preset" && valued)
            preset = argv[++index];
        else if (arg == "--hours" && valued)
            hours = std::stod(argv[++index]);
        else if (arg == "--seed" && valued)
            seed = static_cast<std::uint32_t>(std::stoul(argv[++index]));
        else if (arg == "--print")
            print = true;
        else if (index == 1 && arg.rfind("--", 0) != 0)
            repo = arg;
        else
            Check(false, "unknown argument " + arg);
    }
    auto const start = std::chrono::steady_clock::now();
    if (preset.empty())
    {
        CheckHand();
        Search();
        Controls();
        RunPreset(repo, "night", 0.0, 1, false);
        RunPreset(repo, "cold-start", 0.0, 1, false);
    }
    else
        RunPreset(repo, preset, hours, seed, print);
    double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "time: " << static_cast<long>(ms) << " ms\n";
    std::cout << "auction-market: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
