/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MATERIALRULES_H
#define _PLAYERBOT_MATERIALRULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "GoalModel.h"
#include "RecipeIndex.h"

// Sources and costs of materials (professions plan WR9, PLAN 3.6 and 3.7): for an item, the ways a bot can get it
// and what one unit costs it, in the planner's value units (XP-equivalent). One number: the cheaper of buying and
// getting. Buying costs the copper weighed by the bot's need factor; getting costs its minutes at what a minute is
// worth to the bot, and a risky place makes each minute worth less and adds the expected cost of dying. A made
// material costs its inputs plus its casts, a processed one its input by the expected yield. No source means
// blocked, never free. Pure and std only: the caller fills the rates (WR11) and the place risk (WR13b).
// The pure-rules shape is adapted from Solfood/mod-playerbots CraftPlanRules.h at e8c3eb42, the bounded dependency
// expansion (depth, work and quantity bounds) from damienselarez/mod-playerbots ProfessionPlan.h at be691a93
// (GPL v2), without their order-dependent work counter: a query evaluates every (item, depth) it reaches once.
namespace Materials
{
enum class Kind : std::uint8_t
{
    Blocked,    // no way found
    Bought,     // a vendor that always stocks it, for gold
    Gathered,   // mining or herbalism nodes
    Made,       // a recipe of the index
    Processed,  // prospecting or milling (a disenchant's input is an item of the dropped kind)
    Fished,     // empty until WR16b
    Dropped,    // kill loot and skinning: questing's loot-source index (WT32), empty until WR20
    Auction     // empty until WR23
};

enum class Cause : std::uint8_t
{
    None,
    NoSource,
    TooDangerous,  // found only where the bot cannot go safely: out of reach for now
    Bound          // a cycle of recipes or a chain beyond the depth, work or quantity bound
};

constexpr std::uint32_t MAX_DEPTH = 8;        // recipe levels below the material asked for
constexpr std::size_t MAX_WORK = 2048;        // (item, depth) evaluations of one query
constexpr std::uint32_t MAX_QUANTITY = 1000;  // units of one material along an expansion
constexpr float CAST_FLOOR_MS = 1500.0f;      // [E] a cast takes at least the global cooldown
constexpr float PROCESS_CAST_MS = 3000.0f;    // [E] one prospect or mill cast
constexpr double COST_EPSILON = 1e-9;         // equal costs: the lower kind and source id win

inline char const* KindWord(Kind kind)
{
    switch (kind)
    {
        case Kind::Bought:
            return "bought";
        case Kind::Gathered:
            return "gathered";
        case Kind::Made:
            return "made";
        case Kind::Processed:
            return "processed";
        case Kind::Fished:
            return "fished";
        case Kind::Dropped:
            return "dropped";
        case Kind::Auction:
            return "auction";
        default:
            return "blocked";
    }
}

inline char const* CauseWord(Cause cause)
{
    switch (cause)
    {
        case Cause::NoSource:
            return "no_source";
        case Cause::TooDangerous:
            return "too_dangerous";
        case Cause::Bound:
            return "bound";
        default:
            return "none";
    }
}

// The danger of a place, the planner's risk inputs (GoalModel.h RiskInput) with deaths per hour of play there
struct PlaceRisk
{
    float levelsAbove{0.0f};
    float eliteShare{0.0f};
    float deathsPerHour{0.0f};
    float enemyShare{0.0f};
};

// The planner's own measures for this bot (GoalModel.h Need and ReferenceRate), in value units
struct Rates
{
    float exchange{0.0f};     // a copper's neutral worth (Need.exchange): shows a value as copper
    float factor{0.0f};       // the bot's copper weight (Need.factor): larger for a bot short of gold
    float minuteValue{0.0f};  // what a minute is worth (the planner's reference rate)
    float cautionWeight{1.0f};
    std::uint8_t groupSize{1};
    float runSpeed{Planner::Circuit::DEFAULT_RUN_SPEED};  // yards per second
};

struct Bot
{
    std::unordered_map<std::uint16_t, std::uint32_t> skills;  // skill line -> value
    Rates rates;
    PlaceRisk anyZone;  // the risk of a zone not in zones
    std::unordered_map<std::uint32_t, PlaceRisk> zones;

    [[nodiscard]] std::uint32_t Skill(std::uint32_t skill) const
    {
        auto const found = skills.find(static_cast<std::uint16_t>(skill));
        return found == skills.end() ? 0u : found->second;
    }

    [[nodiscard]] PlaceRisk const& RiskOf(std::uint32_t zone) const
    {
        auto const found = zones.find(zone);
        return found == zones.end() ? anyZone : found->second;
    }
};

// What one unit costs the bot by its cheapest source
struct Cost
{
    Kind kind{Kind::Blocked};
    Cause cause{Cause::NoSource};
    double value{0.0};       // value units per unit
    double minutes{0.0};     // minutes one unit takes to get (gathering and casts, its inputs' included)
    double copperPaid{0.0};  // copper paid per unit (its inputs' included)
    std::uint32_t via{0};    // the item bought, the zone gathered in, the recipe spell or the processed item

    [[nodiscard]] bool Blocked() const { return kind == Kind::Blocked; }
    // The value as neutral copper
    [[nodiscard]] double CopperWorth(Rates const& rates) const
    {
        return rates.exchange > 0.0f ? value / rates.exchange : 0.0;
    }
};

// One step of an expansion: how many units of an item, from which source
struct Line
{
    std::uint32_t item{0};
    Kind kind{Kind::Blocked};
    std::uint32_t via{0};
    std::uint64_t quantity{0};
};

struct Expansion
{
    Cost unit;                 // the asked item's unit cost
    Cause cause{Cause::None};  // set when blocked
    std::vector<Line> lines;   // the asked item first, then its inputs, in the order they are reached

    [[nodiscard]] bool Blocked() const { return cause != Cause::None; }
};

inline Cost BlockedBy(Cause cause)
{
    Cost cost;
    cost.cause = cause;
    return cost;
}

inline bool Cheaper(Cost const& a, Cost const& b)
{
    if (b.Blocked())
        return !a.Blocked();
    if (a.Blocked())
        return false;
    if (std::abs(a.value - b.value) > COST_EPSILON * std::max(1.0, std::abs(b.value)))
        return a.value < b.value;
    return std::tie(a.kind, a.via) < std::tie(b.kind, b.via);
}

// The minutes a cast takes
inline double CastMinutes(float castMs) { return std::max(castMs, CAST_FLOOR_MS) / Planner::Goals::MINUTE_MS; }

// The value one minute at a place costs: the planner's risk term (a session there, weighted by caution) makes the
// minute worth less, and the expected cost of dying is added; negative when the place is refused
inline double MinuteCost(PlaceRisk const& risk, Rates const& rates)
{
    using namespace Planner::Goals;
    float const deathsPerSession = std::max(risk.deathsPerHour, 0.0f) * GRIND_MINUTES / WorldTravel::MINUTES_PER_HOUR;
    float const weighted =
        RawRisk({risk.levelsAbove, risk.eliteShare, deathsPerSession, risk.enemyShare}, rates.groupSize) *
        rates.cautionWeight;
    if (weighted > RISK_REFUSE)
        return -1.0;
    double const deathsPerMinute = std::max(risk.deathsPerHour, 0.0f) / WorldTravel::MINUTES_PER_HOUR;
    return rates.minuteValue * (1.0 + weighted + deathsPerMinute * DEATH_MINUTES);
}

class Pricer
{
public:
    Pricer(Recipes::RecipeIndex const& index, Bot const& bot) : _index(index), _bot(bot) {}

    // What one unit of an item costs the bot by its cheapest source
    [[nodiscard]] Cost UnitCost(std::uint32_t item) const
    {
        Solver solver{*this};
        Cost const cost = solver.Of(item, MAX_DEPTH);
        return solver.overWork ? BlockedBy(Cause::Bound) : cost;
    }

    // The steps and quantities for a quantity of an item along its cheapest sources
    [[nodiscard]] Expansion Expand(std::uint32_t item, std::uint32_t quantity) const
    {
        Solver solver{*this};
        Expansion expansion;
        expansion.unit = solver.Of(item, MAX_DEPTH);
        if (solver.overWork)
            expansion.unit = BlockedBy(Cause::Bound);
        if (expansion.unit.Blocked())
        {
            expansion.cause = expansion.unit.cause;
            return expansion;
        }
        if (!Walk(solver, item, quantity, MAX_DEPTH, expansion))
            expansion.cause = Cause::Bound;
        return expansion;
    }

private:
    struct Solver
    {
        Pricer const& pricer;
        std::unordered_map<std::uint64_t, Cost> memo{};
        bool overWork{false};

        Cost Of(std::uint32_t item, std::uint32_t depth)
        {
            std::uint64_t const key = static_cast<std::uint64_t>(item) * (MAX_DEPTH + 1) + depth;
            if (auto const found = memo.find(key); found != memo.end())
                return found->second;
            if (memo.size() >= MAX_WORK)
            {
                overWork = true;
                return BlockedBy(Cause::Bound);
            }
            Cost const cost = pricer.Cheapest(*this, item, depth);
            memo[key] = cost;
            return cost;
        }
    };

    struct Choice
    {
        Cost best = BlockedBy(Cause::NoSource);
        bool dangerous{false};
        bool bounded{false};

        void Offer(Cost const& cost)
        {
            if (cost.Blocked())
            {
                dangerous = dangerous || cost.cause == Cause::TooDangerous;
                bounded = bounded || cost.cause == Cause::Bound;
            }
            else if (Cheaper(cost, best))
                best = cost;
        }

        [[nodiscard]] Cost Result() const
        {
            if (!best.Blocked())
                return best;
            return BlockedBy(dangerous ? Cause::TooDangerous : bounded ? Cause::Bound : Cause::NoSource);
        }
    };

    Cost Cheapest(Solver& solver, std::uint32_t item, std::uint32_t depth) const
    {
        Choice choice;
        choice.Offer(Bought(item));
        for (std::size_t const at : _index.NodesOf(item))
            choice.Offer(Gathered(_index.Nodes()[at]));
        for (std::size_t const at : _index.MadeBy(item))
            choice.Offer(depth ? Made(solver, _index.Recipes()[at], depth - 1) : BlockedBy(Cause::Bound));
        for (std::size_t const at : _index.YieldedBy(item))
            choice.Offer(depth ? Processed(solver, _index.Processes()[at], item, depth - 1) : BlockedBy(Cause::Bound));
        return choice.Result();
    }

    // Only a vendor that always stocks the item counts, gold only (a limited stock is not counted on)
    [[nodiscard]] Cost Bought(std::uint32_t item) const
    {
        Recipes::VendorRow const* vendor = _index.Vendor(item);
        if (!vendor || !vendor->stocked || !vendor->buyCopper)
            return BlockedBy(Cause::NoSource);
        Cost cost;
        cost.kind = Kind::Bought;
        cost.cause = Cause::None;
        cost.copperPaid = static_cast<double>(vendor->buyCopper) / std::max<std::uint32_t>(vendor->buyCount, 1);
        cost.value = cost.copperPaid * _bot.rates.factor;
        cost.via = item;
        return cost;
    }

    // Minutes per unit: the walk to the next node of the kind and the gathering itself, over a node's yield
    [[nodiscard]] Cost Gathered(Recipes::NodeRow const& node) const
    {
        if (!node.nodes || node.perNode <= 0.0 || _bot.Skill(node.skill) < node.reqSkill)
            return BlockedBy(Cause::NoSource);
        double const minuteCost = MinuteCost(_bot.RiskOf(node.zone), _bot.rates);
        if (minuteCost < 0.0)
            return BlockedBy(Cause::TooDangerous);
        double const walkMinutes =
            node.spacing / (std::max(_bot.rates.runSpeed, 1.0f) * Planner::Goals::SECONDS_PER_MINUTE);
        Cost cost;
        cost.kind = Kind::Gathered;
        cost.cause = Cause::None;
        cost.minutes = (walkMinutes + Planner::Goals::GATHER_MINUTES) / node.perNode;
        cost.value = cost.minutes * minuteCost;
        cost.via = node.zone;
        return cost;
    }

    // Its inputs plus its cast, over the items one cast makes; the walk to a station counts as zero here
    [[nodiscard]] Cost Made(Solver& solver, Recipes::Recipe const& recipe, std::uint32_t depth) const
    {
        if (_bot.Skill(recipe.skill) < recipe.minRank)
            return BlockedBy(Cause::NoSource);
        Cost cost;
        for (Recipes::ItemCount const& reagent : recipe.reagents)
        {
            Cost const input = solver.Of(reagent.item, depth);
            if (input.Blocked())
                return input;
            cost.value += input.value * reagent.count;
            cost.minutes += input.minutes * reagent.count;
            cost.copperPaid += input.copperPaid * reagent.count;
        }
        double const cast = CastMinutes(static_cast<float>(recipe.castMs));
        cost.value += cast * _bot.rates.minuteValue;
        cost.minutes += cast;
        double const made = std::max<std::uint32_t>(recipe.productCount, 1);
        cost.value /= made;
        cost.minutes /= made;
        cost.copperPaid /= made;
        cost.kind = Kind::Made;
        cost.cause = Cause::None;
        cost.via = recipe.spell;
        return cost;
    }

    // The input of a cast over the expected yield of the item; the cast's whole cost goes to each of its yields
    [[nodiscard]] Cost Processed(Solver& solver, Recipes::Process const& process, std::uint32_t item,
                                 std::uint32_t depth) const
    {
        if (process.kind == Recipes::ProcessKind::Disenchant || _bot.Skill(process.skill) < process.rank)
            return BlockedBy(Cause::NoSource);
        double expected = 0.0;
        for (Recipes::Yield const& yield : process.yields)
            if (yield.item == item)
                expected += yield.expected;
        if (expected <= 0.0)
            return BlockedBy(Cause::NoSource);
        Cost const input = solver.Of(process.source, depth);
        if (input.Blocked())
            return input;
        double const consumed = std::max<std::uint32_t>(process.consumed, 1);
        double const cast = CastMinutes(PROCESS_CAST_MS);
        Cost cost;
        cost.kind = Kind::Processed;
        cost.cause = Cause::None;
        cost.value = (input.value * consumed + cast * _bot.rates.minuteValue) / expected;
        cost.minutes = (input.minutes * consumed + cast) / expected;
        cost.copperPaid = input.copperPaid * consumed / expected;
        cost.via = process.source;
        return cost;
    }

    // Adds the line of an item and its inputs; false when a quantity passes the bound
    bool Walk(Solver& solver, std::uint32_t item, std::uint64_t quantity, std::uint32_t depth,
              Expansion& expansion) const
    {
        if (quantity > MAX_QUANTITY)
            return false;
        Cost const cost = solver.Of(item, depth);
        auto const same = std::find_if(expansion.lines.begin(), expansion.lines.end(), [&](Line const& line)
                                       { return line.item == item && line.kind == cost.kind && line.via == cost.via; });
        if (same == expansion.lines.end())
            expansion.lines.push_back({item, cost.kind, cost.via, quantity});
        else if ((same->quantity += quantity) > MAX_QUANTITY)
            return false;

        if (cost.kind == Kind::Made)
            for (std::size_t const at : _index.MadeBy(item))
            {
                Recipes::Recipe const& recipe = _index.Recipes()[at];
                if (recipe.spell != cost.via)
                    continue;
                std::uint64_t const made = std::max<std::uint32_t>(recipe.productCount, 1);
                std::uint64_t const casts = (quantity + made - 1) / made;
                for (Recipes::ItemCount const& reagent : recipe.reagents)
                    if (!Walk(solver, reagent.item, casts * reagent.count, depth - 1, expansion))
                        return false;
                return true;
            }
        if (cost.kind == Kind::Processed)
            for (std::size_t const at : _index.YieldedBy(item))
            {
                Recipes::Process const& process = _index.Processes()[at];
                if (process.source != cost.via || process.kind == Recipes::ProcessKind::Disenchant)
                    continue;
                double expected = 0.0;
                for (Recipes::Yield const& yield : process.yields)
                    if (yield.item == item)
                        expected += yield.expected;
                double const casts = std::ceil(static_cast<double>(quantity) / expected);
                if (casts > MAX_QUANTITY)
                    return false;
                return Walk(solver, process.source,
                            static_cast<std::uint64_t>(casts) * std::max<std::uint32_t>(process.consumed, 1), depth - 1,
                            expansion);
            }
        return true;
    }

    Recipes::RecipeIndex const& _index;
    Bot const& _bot;
};
}  // namespace Materials

#endif
