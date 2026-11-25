/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONRULES_H
#define _PLAYERBOT_AUCTIONRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "GoalModel.h"
#include "ItemWorthRules.h"
#include "ItemWorthTypes.h"
#include "MaterialRules.h"
#include "RecipeIndex.h"

// The reference value of every tradeable item (auction house plan WA2a, PLAN 3.4, WA.1, WA.2): what a seller asks
// when there are no trades, in copper, from local data only (the item template, the vendors, the recipe index and the
// planner's rates). A material a bot can get costs what getting it costs a typical bot of the item's level band
// (professions' cost rule, MaterialRules.h); a made thing its inputs plus its casts (the same rule's recursion); what
// a vendor always stocks at most its vendor price; goods with no way of getting them on purpose the vendor price
// scaled by quality and item level; never below the vendor sell price. Pure and std only: the server gathers the
// rows (AuctionReference.cpp) and builds the table once on a planner worker; tests/auction-rules-selftest.cpp checks
// this code over the dumped real data.
// The shape of the rule for goods with no way of getting them (LevelMultiplier, the rarity multiplier, poor quality
// at the sell price) is adapted from cmangos/playerbots ItemUsageValue.cpp (GetItemBaseValue,
// GetLevelPriceMultiplier, GetRarityPriceMultiplier) at d83e54b7 (GPL v2); its drop-rarity input is replaced by a
// fixed stand-in per quality.
// Until professions' WRL5 the cost rule has no drop rows (WR20a) and counts the game's default skill-gain and loot
// rates (WR29).
namespace AuctionRules
{
constexpr std::uint32_t BAND_COUNT = WorldTravel::LEVEL_BAND_COUNT;
constexpr std::uint32_t SKILL_CAP = 450;  // the highest profession skill
constexpr std::uint64_t NO_COST = std::numeric_limits<std::uint64_t>::max();

// The item template's values the filter reads (ItemTemplate.h), kept here so the rules stay free of core types
constexpr std::uint32_t QUALITY_POOR = 0;
constexpr std::uint32_t BIND_ON_PICKUP = 1;
constexpr std::uint32_t BIND_QUEST = 4;
constexpr std::uint32_t BIND_QUEST_OTHER = 5;
constexpr std::uint32_t CLASS_QUEST = 12;
constexpr std::uint32_t FLAG_CONJURED = 0x00000002;
constexpr std::uint32_t FLAG_ACCOUNT_BOUND = 0x08000000;

// [E] cmangos's rarity input per quality (poor .. heirloom): lower for a rarer quality, so the multiplier rises with
// quality (0.92 common, 1.12 uncommon, 1.58 rare, 2.59 epic, 3.49 legendary and above)
constexpr std::array<double, 8> RARITY_STAND_IN = {140.0, 140.0, 100.0, 60.0, 20.0, 0.0, 0.0, 0.0};

enum class Basis : std::uint8_t
{
    Floor,      // only the vendor sell price
    VendorCap,  // a vendor always stocks it: its vendor price
    Gathered,
    Made,
    Processed,
    Dropped,  // empty until professions' WR20a gives drop rows
    Shaped    // no way of getting it on purpose: the vendor price scaled by quality and item level
};

inline char const* BasisWord(Basis basis)
{
    switch (basis)
    {
        case Basis::Floor:
            return "floor";
        case Basis::VendorCap:
            return "vendor";
        case Basis::Gathered:
            return "gathered";
        case Basis::Made:
            return "made";
        case Basis::Processed:
            return "processed";
        case Basis::Dropped:
            return "dropped";
        case Basis::Shaped:
            return "shaped";
    }
    return "?";
}

// Why an item template gets no row
enum class LeftOut : std::uint8_t
{
    None,
    BindOnPickup,  // Bonding 1 (also every account-bound heirloom)
    Quest,         // Bonding 4 or 5, or class 12
    Conjured,      // the auction house refuses conjured items
    AccountBound,  // flagged bound to the account
    Timed,         // a duration: the auction house refuses it
    NoValue,       // no source and neither a buy nor a sell price
    Count
};

inline char const* LeftOutWord(LeftOut reason)
{
    switch (reason)
    {
        case LeftOut::None:
            return "kept";
        case LeftOut::BindOnPickup:
            return "bind on pickup";
        case LeftOut::Quest:
            return "quest";
        case LeftOut::Conjured:
            return "conjured";
        case LeftOut::AccountBound:
            return "account bound";
        case LeftOut::Timed:
            return "timed";
        case LeftOut::NoValue:
            return "no value";
        case LeftOut::Count:
            break;
    }
    return "?";
}

// One item template as the rules read it
struct ItemInput
{
    std::uint32_t id{0};
    std::string name;
    std::uint32_t classId{0};
    std::uint32_t subClass{0};
    std::uint32_t inventoryType{0};
    std::uint32_t quality{0};
    std::uint32_t itemLevel{0};
    std::uint32_t requiredLevel{0};
    std::uint32_t bonding{0};
    std::uint32_t flags{0};
    std::uint32_t duration{0};
    std::uint32_t buyPrice{0};  // one buy stack
    std::uint32_t sellPrice{0};
    std::uint32_t buyCount{1};
    bool vendorStocked{false};  // a spawned vendor sells it for gold without a stock limit

    [[nodiscard]] auto Key() const
    {
        return std::tie(id, name, classId, subClass, inventoryType, quality, itemLevel, requiredLevel, bonding, flags,
                        duration, buyPrice, sellPrice, buyCount, vendorStocked);
    }
};

// Sources the recipe index knows for an item, as bits of ItemReference::sources
constexpr std::uint8_t SOURCE_VENDOR = 1;
constexpr std::uint8_t SOURCE_NODES = 2;
constexpr std::uint8_t SOURCE_RECIPE = 4;
constexpr std::uint8_t SOURCE_PROCESS = 8;

struct ItemReference
{
    std::uint32_t id{0};
    std::uint64_t copper{0};  // the reference value of one unit
    Basis basis{Basis::Floor};
    std::uint8_t band{0};
    std::uint8_t sources{0};
    std::array<std::uint64_t, BAND_COUNT> byBand{};  // the getting cost at each band's typical bot; NO_COST: none
};

struct Counts
{
    std::array<std::uint32_t, static_cast<std::size_t>(Basis::Shaped) + 1> byBasis{};
    std::array<std::uint32_t, static_cast<std::size_t>(LeftOut::Count)> leftOut{};
    std::uint32_t bound{0};  // a source the recipe bounds cut off: valued by the shape or the floor instead

    [[nodiscard]] std::uint32_t Of(Basis basis) const { return byBasis[static_cast<std::size_t>(basis)]; }
    [[nodiscard]] std::uint32_t LeftOutTotal() const
    {
        std::uint32_t total = 0;
        for (std::uint32_t const n : leftOut)
            total += n;
        return total;
    }
};

// The published table: immutable once built
struct Table
{
    std::vector<ItemReference> rows;  // id order
    std::vector<ItemInput> inputs;    // every template read, id order: the dump's source
    Counts counts;
    std::string coreRevision;
    std::uint64_t sequence{0};
    std::uint32_t buildMs{0};

    [[nodiscard]] ItemReference const* Find(std::uint32_t id) const
    {
        auto const it = std::lower_bound(rows.begin(), rows.end(), id,
                                         [](ItemReference const& row, std::uint32_t key) { return row.id < key; });
        return it != rows.end() && it->id == id ? &*it : nullptr;
    }

    [[nodiscard]] ItemInput const* Input(std::uint32_t id) const
    {
        auto const it = std::lower_bound(inputs.begin(), inputs.end(), id,
                                         [](ItemInput const& row, std::uint32_t key) { return row.id < key; });
        return it != inputs.end() && it->id == id ? &*it : nullptr;
    }
};

// ---- the filter ----
inline LeftOut LeftOutOf(ItemInput const& item)
{
    if (item.bonding == BIND_ON_PICKUP)
        return LeftOut::BindOnPickup;
    if (item.bonding == BIND_QUEST || item.bonding == BIND_QUEST_OTHER || item.classId == CLASS_QUEST)
        return LeftOut::Quest;
    if (item.flags & FLAG_CONJURED)
        return LeftOut::Conjured;
    if (item.flags & FLAG_ACCOUNT_BOUND)
        return LeftOut::AccountBound;
    if (item.duration)
        return LeftOut::Timed;
    return LeftOut::None;
}

inline std::string FilterText()
{
    return "an item template is left out when it binds on pickup (Bonding 1), is a quest item (Bonding 4 or 5, class "
           "12), is conjured (flag 0x2), bound to the account (flag 0x8000000) or timed (a duration), or has no "
           "source and neither a buy nor a sell price";
}

// ---- the typical bot of a band ----
// [E] Its skill in every profession line: it keeps pace with its level and reaches the cap at the top level
inline std::uint32_t TypicalSkill(std::uint32_t band)
{
    std::uint32_t const top = WorldTravel::LEVEL_BAND_TOP[std::min(band, BAND_COUNT - 1)];
    std::uint32_t const maxLevel = WorldTravel::LEVEL_BAND_TOP[BAND_COUNT - 1];
    return std::min(SKILL_CAP, (top * SKILL_CAP + maxLevel / 2) / maxLevel);
}

// The skill lines the index names: its nodes, recipes and processes
inline std::vector<std::uint16_t> SkillLines(Recipes::RecipeIndex const& index)
{
    std::vector<std::uint16_t> lines;
    for (Recipes::NodeRow const& node : index.Nodes())
        lines.push_back(node.skill);
    for (Recipes::Recipe const& recipe : index.Recipes())
        lines.push_back(recipe.skill);
    for (Recipes::Process const& process : index.Processes())
        lines.push_back(static_cast<std::uint16_t>(process.skill));
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

// A bot of the band at its reserve (the need factor is the plain exchange), every skill line at the band's typical
// skill, no zone dangerous, every zone in reach: one of its minutes is worth the band's earning rate in copper
inline Materials::Bot TypicalBot(std::vector<std::uint16_t> const& skillLines, WorldTravel::SlotBandTable const& bands,
                                 std::uint32_t band)
{
    Materials::Bot bot;
    for (std::uint16_t const skill : skillLines)
        bot.skills[skill] = TypicalSkill(band);
    float const earn = static_cast<float>(std::max<std::uint32_t>(bands[band].EarnCopperPerMinute, 1));
    bot.rates.minuteValue = Planner::Goals::BAND_XP_PER_MINUTE[band];
    bot.rates.exchange = bot.rates.minuteValue / earn;
    bot.rates.factor = bot.rates.exchange * Planner::Goals::NEED_AT_RESERVE;
    bot.rates.cautionWeight = 1.0f;
    bot.rates.groupSize = 1;
    return bot;
}

// The five typical bots and their pricers; the bots live as long as the pricers that point at them
class BandPricers
{
public:
    BandPricers(Recipes::RecipeIndex const& index, WorldTravel::SlotBandTable const& bands) : _index(index)
    {
        std::vector<std::uint16_t> const lines = SkillLines(index);
        for (std::uint32_t band = 0; band < BAND_COUNT; ++band)
            _bots[band] = TypicalBot(lines, bands, band);
    }

    BandPricers(BandPricers const&) = delete;
    BandPricers& operator=(BandPricers const&) = delete;

    [[nodiscard]] std::uint8_t SourcesOf(std::uint32_t item) const
    {
        Recipes::VendorRow const* vendor = _index.Vendor(item);
        return static_cast<std::uint8_t>(
            (vendor && vendor->stocked ? SOURCE_VENDOR : 0) | (_index.NodesOf(item).empty() ? 0 : SOURCE_NODES) |
            (_index.MadeBy(item).empty() ? 0 : SOURCE_RECIPE) | (_index.YieldedBy(item).empty() ? 0 : SOURCE_PROCESS));
    }

    [[nodiscard]] Materials::Rates const& RatesOf(std::uint32_t band) const { return _bots[band].rates; }

    // The getting cost at each band, its copper at the band's rates
    struct Costs
    {
        std::array<Materials::Cost, BAND_COUNT> cost{};
        std::array<double, BAND_COUNT> copper{};
    };

    [[nodiscard]] Costs Of(std::uint32_t item, std::uint8_t sources) const
    {
        Costs costs;
        for (std::uint32_t band = 0; band < BAND_COUNT; ++band)
        {
            costs.cost[band] = sources ? Materials::Pricer(_index, _bots[band]).UnitCost(item)
                                       : Materials::BlockedBy(Materials::Cause::NoSource);
            costs.copper[band] = costs.cost[band].Blocked() ? 0.0 : costs.cost[band].CopperWorth(_bots[band].rates);
        }
        return costs;
    }

private:
    Recipes::RecipeIndex const& _index;
    std::array<Materials::Bot, BAND_COUNT> _bots{};
};

using Costs = BandPricers::Costs;

// ---- the steps of the rule, in order (Decide) ----
struct Pick
{
    double copper{0.0};
    Basis basis{Basis::Floor};
    bool sourced{false};  // a value from a source or a vendor, not the shape
};

// The item's band: the lowest band from its required level's on whose typical bot gets it; its required level's
// band when none does
inline std::uint32_t ItemBand(Costs const& costs, std::uint32_t requiredLevel)
{
    std::uint32_t const floor = WorldTravel::LevelBand(requiredLevel);
    for (std::uint32_t band = floor; band < BAND_COUNT; ++band)
        if (!costs.cost[band].Blocked())
            return band;
    return floor;
}

// 1. The getting cost at the item's band
inline Pick FromSource(Materials::Cost const& cost, double copper)
{
    switch (cost.kind)
    {
        case Materials::Kind::Gathered:
        case Materials::Kind::Fished:
            return {copper, Basis::Gathered, true};
        case Materials::Kind::Made:
            return {copper, Basis::Made, true};
        case Materials::Kind::Processed:
            return {copper, Basis::Processed, true};
        case Materials::Kind::Bought:
            return {copper, Basis::VendorCap, true};
        case Materials::Kind::Dropped:
            return {copper, Basis::Dropped, true};
        case Materials::Kind::Blocked:
        case Materials::Kind::Auction:
            break;
    }
    return {};
}

// The vendor price of one unit; 0 when no vendor always stocks it for gold
inline double VendorPrice(ItemInput const& item)
{
    if (!item.vendorStocked || !item.buyPrice)
        return 0.0;
    return static_cast<double>(item.buyPrice) / std::max<std::uint32_t>(item.buyCount, 1);
}

// 2. What a vendor always stocks is worth at most its vendor price
inline Pick Cap(ItemInput const& item, Pick pick)
{
    double const vendor = VendorPrice(item);
    if (vendor > 0.0 && (!pick.sourced || vendor < pick.copper))
        return {vendor, Basis::VendorCap, true};
    return pick;
}

// cmangos's level shape: 1 at item level 0, 2.4 at 80, 7.5 at 200
inline double LevelMultiplier(std::uint32_t itemLevel) { return 0.5 + std::exp((0.1 + itemLevel) / 60.0) / 2.0; }

// cmangos's rarity shape over the stand-in of the quality
inline double QualityMultiplier(std::uint32_t quality)
{
    double const x = RARITY_STAND_IN[std::min<std::size_t>(quality, RARITY_STAND_IN.size() - 1)];
    return 0.75 + std::exp((140.0 - x) / 50.0) / 6.0;
}

// 3. No source and no vendor: the vendor price scaled by quality and item level; a poor item at its sell price
inline Pick Shaped(ItemInput const& item)
{
    if (item.quality == QUALITY_POOR)
        return {static_cast<double>(item.sellPrice), Basis::Floor, false};
    double const base = item.buyPrice ? static_cast<double>(item.buyPrice) / std::max<std::uint32_t>(item.buyCount, 1)
                                      : static_cast<double>(item.sellPrice) * WorldTravel::BUY_PER_SELL_PRICE;
    return {base * LevelMultiplier(item.itemLevel) * QualityMultiplier(item.quality), Basis::Shaped, false};
}

// 4. Never below the vendor sell price
inline Pick Clamp(ItemInput const& item, Pick pick)
{
    if (pick.copper < static_cast<double>(item.sellPrice))
        return {static_cast<double>(item.sellPrice), Basis::Floor, pick.sourced};
    return pick;
}

// Half up to whole copper
inline std::uint64_t Round(double copper)
{
    if (!(copper > 0.0))
        return 0;
    double const rounded = std::floor(copper + 0.5);
    return rounded >= 1.8e19 ? std::numeric_limits<std::uint64_t>::max() - 1 : static_cast<std::uint64_t>(rounded);
}

// The row of a decided value
inline ItemReference Reference(ItemInput const& item, Pick const& pick, std::uint32_t band, Costs const& costs,
                               std::uint8_t sources)
{
    ItemReference reference;
    reference.id = item.id;
    reference.copper = Round(pick.copper);
    reference.basis = pick.basis;
    reference.band = static_cast<std::uint8_t>(band);
    reference.sources = sources;
    for (std::uint32_t b = 0; b < BAND_COUNT; ++b)
        reference.byBand[b] = costs.cost[b].Blocked() ? NO_COST : Round(costs.copper[b]);
    return reference;
}

// The rule for one kept item: its band's getting cost, the vendor cap, the shape when neither applies, the floor
inline ItemReference Decide(ItemInput const& item, Costs const& costs, std::uint8_t sources)
{
    std::uint32_t const band = ItemBand(costs, item.requiredLevel);
    Pick pick = Cap(item, FromSource(costs.cost[band], costs.copper[band]));
    if (!pick.sourced)
        pick = Shaped(item);
    return Reference(item, Clamp(item, pick), band, costs, sources);
}

// One row per id, whatever the order: the smallest row of an id wins
inline void Deduplicate(std::vector<ItemInput>& inputs)
{
    std::sort(inputs.begin(), inputs.end(), [](ItemInput const& a, ItemInput const& b) { return a.Key() < b.Key(); });
    inputs.erase(
        std::unique(inputs.begin(), inputs.end(), [](ItemInput const& a, ItemInput const& b) { return a.id == b.id; }),
        inputs.end());
}

// The whole table: every item template read, the recipe index, the bands' earning rates
inline Table Build(std::vector<ItemInput> inputs, Recipes::RecipeIndex const& index,
                   WorldTravel::SlotBandTable const& bands)
{
    Deduplicate(inputs);
    BandPricers const pricers(index, bands);
    Table table;
    table.rows.reserve(inputs.size());
    for (ItemInput const& item : inputs)
    {
        LeftOut const reason = LeftOutOf(item);
        if (reason != LeftOut::None)
        {
            ++table.counts.leftOut[static_cast<std::size_t>(reason)];
            continue;
        }
        std::uint8_t const sources = pricers.SourcesOf(item.id);
        Costs const costs = pricers.Of(item.id, sources);
        ItemReference const reference = Decide(item, costs, sources);
        if (!reference.copper)
        {
            ++table.counts.leftOut[static_cast<std::size_t>(LeftOut::NoValue)];
            continue;
        }
        if (costs.cost[reference.band].cause == Materials::Cause::Bound &&
            (reference.basis == Basis::Shaped || reference.basis == Basis::Floor))
            ++table.counts.bound;
        ++table.counts.byBasis[static_cast<std::size_t>(reference.basis)];
        table.rows.push_back(reference);
    }
    table.inputs = std::move(inputs);
    return table;
}

// ---- text ----
inline std::string Money(std::uint64_t copper)
{
    std::ostringstream out;
    if (copper >= 10000)
        out << copper / 10000 << "g ";
    if (copper >= 100)
        out << copper / 100 % 100 << "s ";
    out << copper % 100 << 'c';
    return out.str();
}

inline std::string CostText(std::uint64_t copper) { return copper == NO_COST ? "-" : std::to_string(copper); }

// The command's line for one item (and the harness's, so the two compare)
inline std::string Line(ItemReference const& reference, ItemInput const& item)
{
    std::ostringstream out;
    out << "AUCTION ref " << item.id << ' ' << item.name << ": " << reference.copper << " (" << Money(reference.copper)
        << ") basis " << BasisWord(reference.basis) << " band " << static_cast<std::uint32_t>(reference.band)
        << " | vendor sell " << item.sellPrice << " buy " << item.buyPrice;
    if (item.buyCount > 1)
        out << " per " << item.buyCount;
    if (item.vendorStocked)
        out << " stocked";
    out << " | by band";
    for (std::uint64_t const copper : reference.byBand)
        out << ' ' << CostText(copper);
    return out.str();
}

// ---- the sample the harness and the command print: found by rule, not a list of ids ----
struct SampleEntry
{
    std::string group;
    std::uint32_t id{0};
};

constexpr std::uint32_t SKILL_MINING = 186;
constexpr std::uint32_t CLASS_CONSUMABLE = 0;
constexpr std::uint32_t SUB_POTION = 1;
constexpr std::uint32_t CLASS_CONTAINER = 1;
constexpr std::uint32_t CLASS_WEAPON = 2;
constexpr std::uint32_t CLASS_ARMOR = 4;
constexpr std::uint32_t CLASS_TRADE = 7;
constexpr std::uint32_t CLASS_RECIPE = 9;
constexpr std::uint32_t ANY_SUB = std::numeric_limits<std::uint32_t>::max();
constexpr std::array<std::uint32_t, 4> SAMPLE_ANCHORS = {2589, 2770, 2840, 2447};  // linen, copper ore, bar, peacebloom
constexpr std::array<std::uint32_t, 9> SAMPLE_GEAR_LEVELS = {10, 25, 40, 55, 70, 80, 115, 150, 187};

inline std::vector<SampleEntry> Sample(Table const& table, Recipes::RecipeIndex const& index)
{
    std::vector<SampleEntry> sample;
    auto add = [&](std::string group, std::uint32_t id)
    {
        if (!id || std::any_of(sample.begin(), sample.end(), [id](SampleEntry const& e) { return e.id == id; }))
            return;
        sample.push_back({std::move(group), id});
    };
    for (std::uint32_t const id : SAMPLE_ANCHORS)
        if (table.Find(id))
            add("anchor", id);

    // The lowest id of a cell per band (rows are in id order)
    auto firstOf = [&](auto const& match, std::uint32_t band) -> std::uint32_t
    {
        for (ItemReference const& row : table.rows)
            if (row.band == band)
                if (ItemInput const* item = table.Input(row.id); item && match(*item))
                    return row.id;
        return 0;
    };
    struct Cell
    {
        char const* group;
        std::uint32_t classId;
        std::uint32_t subClass;
    };
    Cell const cells[] = {{"cloth", CLASS_TRADE, 5},
                          {"leather", CLASS_TRADE, 6},
                          {"metal and stone", CLASS_TRADE, 7},
                          {"herb", CLASS_TRADE, 9},
                          {"potion", CLASS_CONSUMABLE, SUB_POTION},
                          {"bag", CLASS_CONTAINER, ANY_SUB},
                          {"recipe", CLASS_RECIPE, ANY_SUB}};
    for (Cell const& cell : cells)
        for (std::uint32_t band = 0; band < BAND_COUNT; ++band)
            add(cell.group, firstOf(
                                [&cell](ItemInput const& item)
                                {
                                    return item.classId == cell.classId &&
                                           (cell.subClass == ANY_SUB || item.subClass == cell.subClass) &&
                                           item.quality != QUALITY_POOR;
                                },
                                band));

    // Bars: products of the mining skill's recipes (smelting)
    std::vector<std::uint32_t> bars;
    for (Recipes::Recipe const& recipe : index.Recipes())
        if (recipe.skill == SKILL_MINING && recipe.product)
            bars.push_back(recipe.product);
    std::sort(bars.begin(), bars.end());
    for (std::uint32_t band = 0; band < BAND_COUNT; ++band)
        add("bar", firstOf([&bars](ItemInput const& item)
                           { return std::binary_search(bars.begin(), bars.end(), item.id); }, band));

    // Greens and blues of armor and weapons: the item level closest to each target, the lowest id on a tie
    for (std::uint32_t const quality : {2u, 3u})
        for (std::uint32_t const classId : {CLASS_ARMOR, CLASS_WEAPON})
            for (std::uint32_t const level : SAMPLE_GEAR_LEVELS)
            {
                std::uint32_t best = 0;
                std::uint32_t bestGap = std::numeric_limits<std::uint32_t>::max();
                for (ItemReference const& row : table.rows)
                {
                    ItemInput const* item = table.Input(row.id);
                    if (!item || item->quality != quality || item->classId != classId)
                        continue;
                    std::uint32_t const gap =
                        item->itemLevel > level ? item->itemLevel - level : level - item->itemLevel;
                    if (gap < bestGap)
                    {
                        best = row.id;
                        bestGap = gap;
                    }
                }
                add(std::string(quality == 2 ? "green " : "blue ") + (classId == CLASS_ARMOR ? "armor" : "weapon"),
                    best);
            }
    return sample;
}

// ---- the inputs as text: the harness's fixture (`playerbots auction ref dump`) ----
inline std::string CleanName(std::string text)
{
    for (char& c : text)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return text.empty() ? std::string("-") : text;
}

inline std::string DumpInputs(std::vector<ItemInput> const& inputs, std::string const& coreRevision)
{
    std::ostringstream out;
    out << "# auction-reference-inputs v1 core " << coreRevision << " items " << inputs.size() << '\n';
    out << "#I\tid\tname\tclass\tsubClass\tinventoryType\tquality\titemLevel\trequiredLevel\tbonding\tflags\t"
           "duration\tbuyPrice\tsellPrice\tbuyCount\tstocked\n";
    for (ItemInput const& i : inputs)
        out << "I\t" << i.id << '\t' << CleanName(i.name) << '\t' << i.classId << '\t' << i.subClass << '\t'
            << i.inventoryType << '\t' << i.quality << '\t' << i.itemLevel << '\t' << i.requiredLevel << '\t'
            << i.bonding << '\t' << i.flags << '\t' << i.duration << '\t' << i.buyPrice << '\t' << i.sellPrice << '\t'
            << i.buyCount << '\t' << (i.vendorStocked ? 1 : 0) << '\n';
    return out.str();
}

// Reads a dump back; false with the reason in error when a line is malformed or the count differs
inline bool ParseInputs(std::string const& text, std::vector<ItemInput>& inputs, std::string& coreRevision,
                        std::string& error)
{
    inputs.clear();
    std::istringstream in(text);
    std::string line;
    std::size_t number = 0;
    std::size_t declared = 0;
    bool header = false;
    while (std::getline(in, line))
    {
        ++number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.rfind("#I", 0) == 0)
            continue;
        if (line.rfind("# auction-reference-inputs v1 ", 0) == 0)
        {
            std::istringstream words(line);
            std::string word;
            while (words >> word)
                if (word == "core")
                    words >> coreRevision;
                else if (word == "items")
                    words >> declared;
            header = true;
            continue;
        }
        std::vector<std::string> fields;
        std::size_t start = 0;
        for (std::size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', start))
        {
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        fields.push_back(line.substr(start));
        if (fields.size() != 16 || fields[0] != "I")
        {
            error = "line " + std::to_string(number) + ": not an item row";
            return false;
        }
        try
        {
            auto u = [&fields](std::size_t at) { return static_cast<std::uint32_t>(std::stoul(fields[at])); };
            ItemInput item;
            item.id = u(1);
            item.name = fields[2];
            item.classId = u(3);
            item.subClass = u(4);
            item.inventoryType = u(5);
            item.quality = u(6);
            item.itemLevel = u(7);
            item.requiredLevel = u(8);
            item.bonding = u(9);
            item.flags = u(10);
            item.duration = u(11);
            item.buyPrice = u(12);
            item.sellPrice = u(13);
            item.buyCount = u(14);
            item.vendorStocked = u(15) != 0;
            inputs.push_back(std::move(item));
        }
        catch (std::exception const&)
        {
            error = "line " + std::to_string(number) + ": a number does not parse";
            return false;
        }
    }
    if (!header || declared != inputs.size())
    {
        error = header ? "the header names " + std::to_string(declared) + " items, the file holds " +
                             std::to_string(inputs.size())
                       : "no header";
        return false;
    }
    return true;
}
}  // namespace AuctionRules

#endif
