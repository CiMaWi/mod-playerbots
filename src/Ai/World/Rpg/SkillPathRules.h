/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SKILLPATHRULES_H
#define _PLAYERBOT_SKILLPATHRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CraftRules.h"
#include "MarketRules.h"
#include "MaterialRules.h"
#include "ProfessionRules.h"
#include "RecipeIndex.h"
#include "SkillPathTypes.h"

// The skill path (professions plan WR10, PLAN 3.8, DECISIONS WR.4-WR.6, WR.11): whether a stretch of skill is worth
// its cost, and how its cheapest points are made. A bot weighs stretches from its skill to a milestone (a recipe that
// becomes makeable, a held recipe it could then learn, a node kind or skinnable creatures of its zones) and takes the
// one with the best net worth per minute when that is above zero; no target number, no share of time, nothing forced.
// A point through a recipe costs its materials (MaterialRules.h) and cast, less what the product fetches, over the
// engine's chance of a point; a recipe to learn adds its price spread over the points it serves; gatherers climb the
// same way over node colours and creature levels. The bot's claims come in order: what it uses, the intermediates for
// those, then skill; nothing is made only to be sold. Pure and std only, any thread, deterministic: the caller fills
// the inputs (WR11: the demand answer of item use as ConsumableWant, travel's worth of upgrades as UseWant, the rates
// and the place risk). MayLearn is the only gate for recipes and ranks (WR.11); it also stops the era's cap.
// The pure-rules shape and the reservation across claims are adapted from Solfood/mod-playerbots CraftPlanRules.h
// at e8c3eb42, the bounded expansion from damienselarez/mod-playerbots ProfessionPlan.h at be691a93 (GPL v2), with
// the choice made order independent (ties by kind and id) instead of their order-dependent work counter.
namespace SkillPath
{
// The engine's chances of a skill-up in per mille, as the core computes them (PlayerUpdates.cpp SkillGainChance,
// CraftSkillGainChance, getProbabilityOfLevelUp), at the server's SkillChance.* values (Recipes::ServerRates, WR29); a
// skill-up gives the server's SkillGain.* step, at most to the cap (Player::UpdateSkillPro)
constexpr int PER_MILLE = 1000;
constexpr std::uint32_t GATHER_YELLOW_FROM = 25;  // a node: yellow from its skill + 25
constexpr std::uint32_t GATHER_GREEN_FROM = 50;
constexpr std::uint32_t GATHER_GREY_FROM = 100;
constexpr std::array<std::uint32_t, 10> FISH_BOUNDS = {115, 135, 160, 190, 215, 295, 315, 355, 425, 450};
constexpr std::array<float, 11> FISH_CATCHES_PER_POINT = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f, 6.0f,
                                                          9.0f, 10.0f, 11.0f, 12.0f, 1.0f};
constexpr float FISH_PERCENT = 100.0f;
constexpr int PERCENT_TO_PER_MILLE = 10;

constexpr std::uint32_t EXPECTED_SKILL_PER_LEVEL = 5;  // what a bot of a level usually has (WR.5)
constexpr std::uint32_t NEARNESS_SPAN = 75;            // [E] a milestone counts nothing one rank above level x 5
constexpr std::uint32_t ITEM_RECIPE_YELLOW_GAP = 25;   // [E] a recipe item's skill: its yellow less this
constexpr double INCOME_CASTS = 5.0;                   // [E] casts of a product sold that income counts
constexpr double ACCESS_GATHERS = 10.0;                // [E] nodes or skins of a newly usable kind that access counts
constexpr std::size_t MAX_STRETCHES = 8;               // stretches weighed per profession
constexpr std::uint32_t MAX_POINTS = 450;              // points of one climb
constexpr double MIN_MINUTES = Materials::CAST_FLOOR_MS / Planner::Goals::MINUTE_MS;
constexpr std::uint32_t TOOL_DEPTH = 1;  // a made tool's own tools may be bought, not made in turn

// Deliberate errors the selftest's negative controls switch on (each must find a counterexample to its invariant);
// always None in the game
enum class Fault : std::uint8_t
{
    None,
    TakeNonPositive,  // 1: takes the best stretch even when its net worth is not above zero
    DearerCheaper,    // 2: counts a material's cost with the wrong sign
    PoorInverted,     // 3: weighs copper less the shorter of gold the bot is
    AllowGrey,        // 4: counts a grey cast as a point
    BlockedFree,      // 5: counts a material without a source as free
    FirstFound,       // 6: takes the first option found, not the cheapest
    ReserveUncapped,  // 7: reserves what the uses want, not what is held
    SkillFirst,       // 8: lets the skill crafts claim before the uses
    IgnoreLock,       // 9: ignores MayLearn
    OrderTie,         // 10: keeps the order of the recipe list and of the bot's wants
    NoExpand,         // 11: wants an intermediate the bot makes instead of its inputs
    ToolFree,         // 12: crafts without the tools the recipe asks for
    WantsOnly,        // 13: lists as the path's materials only what it wants, not what it holds
    IgnoreAbility,    // 14: lets a trainer teach a recipe without the abilities its row asks for (WR.27)
    RatesInverted,    // 15: a skill-up of the server's step gives one point at the step's cost (WR.28)
    AnyMarket         // 16: weighs a second gatherer for any market view, whether it prices the gatherer's yields
};

inline char const* FaultWord(Fault fault)
{
    switch (fault)
    {
        case Fault::TakeNonPositive:
            return "take_non_positive";
        case Fault::DearerCheaper:
            return "dearer_cheaper";
        case Fault::PoorInverted:
            return "poor_inverted";
        case Fault::AllowGrey:
            return "allow_grey";
        case Fault::BlockedFree:
            return "blocked_free";
        case Fault::FirstFound:
            return "first_found";
        case Fault::ReserveUncapped:
            return "reserve_uncapped";
        case Fault::SkillFirst:
            return "skill_first";
        case Fault::IgnoreLock:
            return "ignore_lock";
        case Fault::OrderTie:
            return "order_tie";
        case Fault::NoExpand:
            return "no_expand";
        case Fault::ToolFree:
            return "tool_free";
        case Fault::WantsOnly:
            return "wants_only";
        case Fault::IgnoreAbility:
            return "ignore_ability";
        case Fault::RatesInverted:
            return "rates_inverted";
        case Fault::AnyMarket:
            return "any_market";
        default:
            return "none";
    }
}

inline char const* ReasonWord(Reason reason)
{
    switch (reason)
    {
        case Reason::Open:
            return "open";
        case Reason::Chosen:
            return "chosen";
        case Reason::NotWorthIt:
            return "not_worth_it";
        case Reason::NoSource:
            return "no_source";
        case Reason::Locked:
            return "locked";
        case Reason::Level:
            return "level";
        case Reason::GoldShort:
            return "gold_short";
        case Reason::Bound:
            return "bound";
        default:
            return "no_stretch";
    }
}

inline char const* MilestoneWord(Milestone milestone)
{
    switch (milestone)
    {
        case Milestone::HeldRecipe:
            return "held_recipe";
        case Milestone::Node:
            return "node";
        case Milestone::Skinnable:
            return "skinnable";
        default:
            return "recipe";
    }
}

// A craft's chance falls in a straight line from orange at its yellow skill to grey at its grey skill (pure skill)
inline int CraftChancePerMille(std::uint32_t skill, std::uint32_t grey, std::uint32_t yellow,
                               Recipes::ServerRates const& rates)
{
    if (grey <= yellow)
        return skill < grey ? rates.orange : rates.grey;
    if (skill <= yellow)
        return rates.orange;
    if (skill >= grey)
        return rates.grey;
    return rates.grey +
           static_cast<int>(static_cast<std::int64_t>(grey - skill) * (rates.orange - rates.grey) / (grey - yellow));
}

// A node or creature whose own skill is red: orange below red + 25, yellow to + 50, green to + 100, then grey; mining
// and skinning halve it every MiningSteps or SkinningSteps points when those are set (Player::UpdateGatherSkill)
inline int GatherChancePerMille(std::uint32_t skill, std::uint32_t red, std::uint16_t skillLine,
                                Recipes::ServerRates const& rates)
{
    constexpr std::uint32_t SHIFT_LIMIT = 31;
    int const chance = skill >= red + GATHER_GREY_FROM     ? rates.grey
                       : skill >= red + GATHER_GREEN_FROM  ? rates.green
                       : skill >= red + GATHER_YELLOW_FROM ? rates.yellow
                                                           : rates.orange;
    std::uint32_t const steps = rates.ChanceSteps(skillLine);
    if (!steps || chance <= 0)
        return chance;
    std::uint32_t const shift = skill / steps;
    return shift >= SHIFT_LIMIT ? 0 : chance >> shift;
}

inline int FishChancePerMille(std::uint32_t skill)
{
    if (!skill)
        return 0;
    auto const at = std::lower_bound(FISH_BOUNDS.begin(), FISH_BOUNDS.end(), skill);
    float const catches = FISH_CATCHES_PER_POINT[static_cast<std::size_t>(std::distance(FISH_BOUNDS.begin(), at))];
    return static_cast<int>(FISH_PERCENT / catches) * PERCENT_TO_PER_MILLE;
}

// The skinning skill a creature of a level asks for (the material rules' since WR20a: a skin's drop source asks it)
using Materials::SkinRequirement;

// A milestone's weight: full up to what a bot of this level usually has (level x 5), nothing one rank above it
inline double Nearness(std::uint32_t target, std::uint32_t level)
{
    double const expected = static_cast<double>(level) * EXPECTED_SKILL_PER_LEVEL;
    if (target <= expected)
        return 1.0;
    double const over = target - expected;
    return over >= NEARNESS_SPAN ? 0.0 : 1.0 - over / NEARNESS_SPAN;
}

// A use's weight: full at or below the bot's level, less the further its level lies within the look-ahead
inline double UseWeight(std::uint32_t useLevel, std::uint32_t level)
{
    if (useLevel <= level)
        return 1.0;
    std::uint32_t const soon = ProfessionRules::SoonLevels(level);
    std::uint32_t const ahead = useLevel - level;
    return ahead > soon ? 0.0 : 1.0 - static_cast<double>(ahead) / (soon + 1);
}

// ---- inputs ----
struct SkillLevel
{
    std::uint16_t skill{0};
    std::uint32_t value{0};
    std::uint32_t max{0};
};

// Creatures of a level in a zone that a skinner meets
struct Skinnable
{
    std::uint32_t zone{0};
    std::uint32_t level{0};
    float minutesPerKill{0.0f};
    float copperPerSkin{0.0f};
};

struct Water
{
    std::uint32_t zone{0};
    float minutesPerCast{0.0f};
};

// The creatures a skinner meets (WR20a), from the index's skinning rows: per zone and creature level (the top of a
// kind's levels, which its skin asks for), the kind whose skins fetch the most per minute of a kill, with that kill's
// minutes (Materials::KillOf: the walk to its next spawn, the fight, the skin) and the copper its skins fetch per kill.
// A creature friendly to the killer's side or a fight it refuses is none
inline std::vector<Skinnable> SkinnablesOf(Recipes::RecipeIndex const& index, Materials::Bot const& killer,
                                           std::function<double(std::uint32_t)> const& sellCopper)
{
    std::map<std::pair<std::uint32_t, std::uint32_t>, Skinnable> kinds;  // (zone, creature entry)
    for (Recipes::DropRow const& row : index.Drops())
    {
        if (row.kind != Recipes::DropKind::Skin ||
            row.skill != static_cast<std::uint16_t>(ProfessionRules::Skill::Skinning))
            continue;
        Materials::Kill const kill = Materials::KillOf(row, killer);
        if (kill.cause != Materials::Cause::None)
            continue;
        Skinnable& kind = kinds[{row.zone, row.entry}];
        kind.zone = row.zone;
        kind.level = row.maxLevel;
        kind.minutesPerKill = static_cast<float>(kill.minutes);
        kind.copperPerSkin += static_cast<float>(row.perKill * (sellCopper ? sellCopper(row.item) : 0.0));
    }
    // more copper per minute wins, then the shorter kill, then the lower entry (the map's order)
    auto const better = [](Skinnable const& a, Skinnable const& b)
    {
        double const left = static_cast<double>(a.copperPerSkin) * b.minutesPerKill;
        double const right = static_cast<double>(b.copperPerSkin) * a.minutesPerKill;
        return left != right ? left > right : a.minutesPerKill < b.minutesPerKill;
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, Skinnable> best;  // (zone, level)
    for (auto const& [key, kind] : kinds)
    {
        auto const [at, added] = best.try_emplace({kind.zone, kind.level}, kind);
        if (!added && better(kind, at->second))
            at->second = kind;
    }
    std::vector<Skinnable> skinnables;
    skinnables.reserve(best.size());
    for (auto const& [key, skinnable] : best)
        skinnables.push_back(skinnable);
    return skinnables;
}

struct PathBot
{
    std::uint32_t level{1};
    std::uint8_t side{Materials::BOTH_SIDES};  // its side's bit (Materials::SideBit); both: not known
    std::uint8_t cls{0};
    std::uint64_t money{0};  // copper
    Materials::Rates rates;
    Materials::PlaceRisk anyZone;
    std::unordered_map<std::uint32_t, Materials::PlaceRisk> risks;
    std::vector<std::uint32_t> zones;  // the zones it plays in; empty: every zone
    std::vector<SkillLevel> skills;
    std::vector<std::uint32_t> knownRecipes;
    std::vector<std::uint32_t> toolsHeld;
    std::vector<std::uint32_t> categoriesHeld;  // totem categories its tools cover
    std::vector<Recipes::ItemCount> held;       // bags and bank
    std::vector<std::uint32_t> recipeItems;     // recipe items held that it has not learned
    std::vector<ConsumableWant> consumables;
    std::vector<UseWant> uses;
    // a test's fixture (constraint anypath): every learnable recipe is a milestone and an open stretch is worth one
    // minute per point it climbs, so the fastest climb is taken whatever it brings
    bool anyStretch{false};
};

// A tool the recipes ask for (WR.12, PLAN 3.4): the item, the totem category it covers (0: asked for as the item), and
// its vendor price; 0: no vendor sells it, a recipe of the profession makes it when the index has one
struct ToolSource
{
    std::uint32_t item{0};
    std::uint32_t category{0};
    std::uint32_t copper{0};
};

struct PathWorld
{
    Recipes::RecipeIndex const& index;
    std::function<bool(std::uint32_t)> mayLearn;      // the lock seam (WR.11); empty: everything may be learned
    std::function<double(std::uint32_t)> sellCopper;  // what one unit fetches at a vendor; empty: nothing
    std::vector<Skinnable> skinnables;
    std::vector<Water> waters;
    Fault fault{Fault::None};
    std::vector<ToolSource> tools;  // empty: a missing tool has no source
    Market::View market;            // the typical prices (auction house WA7a); empty: only vendors buy and sell
};

// A missing tool an option gets before its cast, in full: bought (maker 0) or made by a recipe of the profession,
// learned first when learn is set
struct ToolGet
{
    std::uint32_t item{0};
    std::uint32_t maker{0};
    std::uint32_t learn{0};
    std::uint32_t fee{0};   // the learn's price
    double materials{0.0};  // value units: the maker's reagents
    double time{0.0};       // its cast
    double fees{0.0};       // the vendor's price and the learn's
    double copper{0.0};
    double minutes{0.0};

    [[nodiscard]] double Value() const { return materials + time + fees; }
};

// The tools a climb has got so far
struct Acquired
{
    std::set<std::uint32_t> items;
    std::set<std::uint32_t> categories;
};

// One way to make the next skill-up: its fields count what one skill-up costs (the casts or takes it takes), which
// gives `points` skill points (WR29: the server's step, at most to the cap)
struct Option
{
    StepKind kind{StepKind::Craft};
    std::uint32_t id{0};
    std::uint32_t zone{0};
    int chance{0};          // per mille
    double value{0.0};      // value units: materials + time + the fee's share
    double materials{0.0};  // materials less what the product fetches
    double time{0.0};
    double share{0.0};   // the fee's share (value units)
    double copper{0.0};  // copper paid less copper the product fetches, the fee's share included
    double minutes{0.0};
    std::uint32_t learn{0};      // the recipe to learn first
    std::uint32_t fee{0};        // its full price
    std::vector<ToolGet> tools;  // missing tools got first; their cost is in the fields above, once
    std::uint32_t points{1};     // the points the skill-up gives

    // The casts or takes of one skill-up
    [[nodiscard]] double Casts() const { return chance > 0 ? static_cast<double>(PER_MILLE) / chance : 0.0; }
};

// How a recipe is learned by this bot: the skill it can be learned at, the price, whether a step is needed
struct Learning
{
    bool ok{false};
    bool known{false};
    bool step{false};  // bought or used from a recipe item (not taught with the skill)
    std::uint32_t skill{0};
    std::uint32_t fee{0};
};

// What the claims of the next stop take from the bags
struct Claims
{
    std::vector<Craft> queue;                          // in cast order: intermediates, uses, then skill
    std::map<std::uint32_t, std::uint32_t> reserved;   // units taken from what is held
    std::map<std::uint32_t, std::uint32_t> useCasts;   // casts for uses and their intermediates, by spell
    std::map<std::uint32_t, std::uint32_t> afterUses;  // held units the skill crafts may take
    double useWorth{0.0};                              // value units: what the uses' crafts make is worth (WR15)
    std::uint32_t foodUnits{0};                        // units of upkeep's food the uses' crafts make (WR15)
};

// Where the weigher's netting records the path's materials (WR12): the map, the stretch's skill and the skill they are
// due at
struct MaterialRecord
{
    std::map<std::uint32_t, Material>* used{nullptr};
    std::uint16_t skill{0};
    std::uint16_t dueSkill{0};
};

class Weigher
{
public:
    Weigher(PathBot const& bot, PathWorld const& world) : _bot(bot), _world(world), _rates(bot.rates)
    {
        if (_world.fault == Fault::PoorInverted && _rates.factor > 0.0f)
            _rates.factor = _rates.exchange * _rates.exchange / _rates.factor;
        _known.insert(bot.knownRecipes.begin(), bot.knownRecipes.end());
        _recipeItems.insert(bot.recipeItems.begin(), bot.recipeItems.end());
        _tools.insert(bot.toolsHeld.begin(), bot.toolsHeld.end());
        _categories.insert(bot.categoriesHeld.begin(), bot.categoriesHeld.end());
        _zones.insert(bot.zones.begin(), bot.zones.end());
        for (Recipes::ItemCount const& item : bot.held)
            _held[item.item] += item.count;
        // the wants by item, so nothing depends on the order of the lists (the OrderTie fault keeps their order)
        bool const ordered = _world.fault != Fault::OrderTie;
        _consumables = bot.consumables;
        if (ordered)
            std::sort(_consumables.begin(), _consumables.end(),
                      [](ConsumableWant const& a, ConsumableWant const& b)
                      {
                          return std::tie(a.group, a.item, a.wantedStock, a.held, a.worthCopperEach, a.food) <
                                 std::tie(b.group, b.item, b.wantedStock, b.held, b.worthCopperEach, b.food);
                      });
        _uses = bot.uses;
        if (ordered)
            std::sort(_uses.begin(), _uses.end(),
                      [](UseWant const& a, UseWant const& b)
                      {
                          return std::tie(a.item, a.spell, a.level, a.worthCopperEach) <
                                 std::tie(b.item, b.spell, b.level, b.worthCopperEach);
                      });
        _skills = bot.skills;
        if (ordered)
            std::sort(_skills.begin(), _skills.end(),
                      [](SkillLevel const& a, SkillLevel const& b) { return a.skill < b.skill; });

        std::vector<Recipes::Recipe> const& recipes = _world.index.Recipes();
        for (std::size_t at = 0; at < recipes.size(); ++at)
            _recipesOf[recipes[at].skill].push_back(at);
        if (ordered)
            for (auto& [skill, positions] : _recipesOf)
                std::sort(positions.begin(), positions.end(),
                          [&recipes](std::size_t a, std::size_t b) { return recipes[a].spell < recipes[b].spell; });
        std::map<std::tuple<std::uint16_t, std::uint32_t, std::uint32_t>, NodeKind> kinds;
        for (Recipes::NodeRow const& row : _world.index.Nodes())
        {
            NodeKind& kind = kinds[{row.skill, row.zone, row.entry}];
            kind.zone = row.zone;
            kind.entry = row.entry;
            kind.reqSkill = row.reqSkill;
            kind.spacing = row.spacing;
            kind.sellCopper += row.perNode * Sell(row.item);
        }
        for (auto const& [key, kind] : kinds)
            _nodesOf[std::get<0>(key)].push_back(kind);
    }

    // The material rules' bot at a skill of one profession
    [[nodiscard]] Materials::Bot MaterialBot(std::uint16_t skill, std::uint32_t at) const
    {
        Materials::Bot materials;
        for (SkillLevel const& level : _skills)
            materials.skills[level.skill] = level.value;
        materials.skills[skill] = at;
        materials.rates = _rates;
        materials.anyZone = _bot.anyZone;
        materials.zones = _bot.risks;
        materials.reach.assign(_zones.begin(), _zones.end());  // a std::set: sorted
        materials.level = _bot.level;
        materials.side = _bot.side;
        materials.auctionCopper = _world.market.BuyCopperOf();
        return materials;
    }

    [[nodiscard]] Learning LearnOf(Recipes::Recipe const& recipe) const
    {
        Learning learning;
        if (_known.count(recipe.spell))
        {
            learning.ok = learning.known = true;
            return learning;
        }
        if (recipe.classMask && !(recipe.classMask & ProfessionRules::ClassBit(_bot.cls)))
            return learning;
        if (!MayLearn(recipe.spell))
            return learning;
        std::uint32_t const base = std::max<std::uint32_t>(recipe.minRank, 1);
        if (recipe.acquire == Recipes::ACQUIRE_ON_SKILL_LEARN ||
            (recipe.acquire == Recipes::ACQUIRE_ON_SKILL_VALUE && !recipe.trainers && recipe.items.empty()))
        {
            learning.ok = true;
            learning.skill = base;
            return learning;
        }
        auto const offer = [&learning](std::uint32_t skill, std::uint32_t fee)
        {
            if (!learning.ok || std::tie(fee, skill) < std::tie(learning.fee, learning.skill))
            {
                learning.ok = learning.step = true;
                learning.skill = skill;
                learning.fee = fee;
            }
        };
        if (Trains(recipe))
            offer(std::max(base, recipe.trainerRank), recipe.trainerCopper);
        std::uint32_t const itemSkill =
            std::max(base, recipe.yellow > ITEM_RECIPE_YELLOW_GAP ? recipe.yellow - ITEM_RECIPE_YELLOW_GAP : 1u);
        for (Recipes::RecipeSource const& source : recipe.items)
        {
            if (_recipeItems.count(source.item))
                offer(itemSkill, 0);
            else if (source.vendors && source.buyCopper)
                offer(itemSkill, source.buyCopper);
        }
        return learning;
    }

    // Every way to make the next skill-up from at with the tools got so far, in no particular order; max is the cap
    // its points stop at (0: none known, the full step)
    [[nodiscard]] std::vector<Option> Options(std::uint16_t skill, std::uint32_t at,
                                              Acquired const& acquired = Acquired{}, std::uint32_t max = 0) const
    {
        std::vector<Option> options;
        Materials::Bot const materials = MaterialBot(skill, at);
        Materials::Pricer const pricer(_world.index, materials);
        std::unordered_map<std::uint32_t, Materials::Cost> costs;
        auto const cost = [&](std::uint32_t item) -> Materials::Cost const&
        {
            auto found = costs.find(item);
            if (found == costs.end())
                found = costs.emplace(item, pricer.UnitCost(item)).first;
            return found->second;
        };
        if (auto const found = _recipesOf.find(skill); found != _recipesOf.end())
            for (std::size_t const position : found->second)
            {
                Option option;
                if (CraftOption(_world.index.Recipes()[position], at, cost, acquired, Points(true, at, max), option))
                    options.push_back(std::move(option));
            }
        GatherOptions(skill, at, Points(false, at, max), options);
        if (_world.fault == Fault::RatesInverted)
            for (Option& option : options)
            {
                double const slower = option.points;
                option.value *= slower;
                option.materials *= slower;
                option.time *= slower;
                option.share *= slower;
                option.copper *= slower;
                option.minutes *= slower;
                option.points = 1;
            }
        return options;
    }

    // The points a skill-up gives at a skill: the server's step of crafts or of gathering, at most to the cap (0: none)
    [[nodiscard]] std::uint32_t Points(bool craft, std::uint32_t at, std::uint32_t max) const
    {
        std::uint32_t const step = std::max<std::uint32_t>(_world.index.Rates().Step(craft), 1);
        return max > at ? std::min(step, max - at) : step;
    }

    // The cheapest way to make the next points; false when there is none
    [[nodiscard]] bool Best(std::uint16_t skill, std::uint32_t at, Option& best, Acquired const& acquired = Acquired{},
                            std::uint32_t max = 0) const
    {
        bool found = false;
        for (Option const& option : Options(skill, at, acquired, max))
            if (!found || Better(option, best))
            {
                best = option;
                found = true;
                if (_world.fault == Fault::FirstFound)
                    break;
            }
        return found;
    }

    // The cheapest climb from one skill to another: ranks at the cap, recipes learned when first used
    [[nodiscard]] Stretch Climb(std::uint16_t skill, std::uint32_t from, std::uint32_t to) const
    {
        Stretch stretch;
        stretch.skill = skill;
        stretch.from = from;
        stretch.to = to;
        if (to > MAX_POINTS || to - std::min(from, to) > MAX_POINTS)
        {
            stretch.reason = Reason::Bound;
            stretch.blockedAt = from;
            return stretch;
        }
        std::uint32_t max = SkillMax(skill);
        std::vector<Recipes::Rung> const ladder = _world.index.Ladder(skill);
        std::set<std::uint32_t> learned;
        Acquired acquired;
        std::uint32_t at = from;
        while (at < to)
        {
            if (at >= max)
            {
                Recipes::Rung const* next = nullptr;
                for (Recipes::Rung const& rung : ladder)
                    if (rung.cap > max && (!next || rung.cap < next->cap))
                        next = &rung;
                Reason const stop = !next || !MayLearn(next->spell) ? Reason::Locked
                                    : _bot.level < next->reqLevel   ? Reason::Level
                                                                    : Reason::Open;
                if (stop != Reason::Open)
                {
                    stretch.reason = stop;
                    stretch.blockedAt = at;
                    return stretch;
                }
                AddPurchase(stretch, StepKind::Rank, next->spell, at, next->copper);
                double const fee = next->copper * static_cast<double>(_rates.factor);
                stretch.cost.fees += fee;
                stretch.copper += next->copper;
                max = next->cap;
                continue;
            }
            // a skill-up's points stop at the cap and, for the stretch's costs, at its end: a last skill-up that climbs
            // past it counts the stretch's points (the fee's share), its casts in full
            Option option;
            if (!Best(skill, at, option, acquired, std::min(max, to)))
            {
                stretch.reason = Reason::NoSource;
                stretch.blockedAt = at;
                return stretch;
            }
            if (option.learn && learned.insert(option.learn).second)
                AddPurchase(stretch, StepKind::Learn, option.learn, at, option.fee);
            // a missing tool is a step of its own before the cast: bought (no casts) or made (one cast)
            double toolCopper = 0.0;
            double toolMinutes = 0.0;
            double toolValue = 0.0;
            for (ToolGet const& tool : option.tools)
            {
                if (tool.learn && learned.insert(tool.learn).second)
                    AddPurchase(stretch, StepKind::Learn, tool.learn, at, tool.fee);
                PathStep got;
                got.kind = StepKind::Tool;
                got.id = tool.item;
                got.from = at;
                got.to = at;
                got.casts = tool.maker ? 1.0 : 0.0;
                got.copper = tool.copper;
                got.minutes = tool.minutes;
                got.value = tool.Value();
                stretch.steps.push_back(got);
                Acquire(acquired, tool.item);
                toolCopper += tool.copper;
                toolMinutes += tool.minutes;
                toolValue += tool.Value();
            }
            PathStep* step = stretch.steps.empty() ? nullptr : &stretch.steps.back();
            if (!step || step->kind != option.kind || step->id != option.id || step->zone != option.zone ||
                step->to != at)
            {
                stretch.steps.push_back({option.kind, option.id, at, at, 0.0, 0.0, 0.0, 0.0, option.zone});
                step = &stretch.steps.back();
            }
            step->to = at + option.points;
            step->casts += option.Casts();
            step->copper += option.copper - toolCopper;
            step->minutes += option.minutes - toolMinutes;
            step->value += option.value - toolValue;
            stretch.cost.materials += option.materials;
            stretch.cost.time += option.time;
            stretch.cost.fees += option.share;
            stretch.copper += option.copper;
            stretch.minutes += option.minutes;
            at += option.points;  // the chance is taken at each skill the bot passes through (WR29)
        }
        return stretch;
    }

    // What the stretch from one skill to another brings, weighted by the nearness of its end
    [[nodiscard]] Brings BringsOf(std::uint16_t skill, std::uint32_t from, std::uint32_t to) const
    {
        Brings brings;
        std::set<std::uint32_t> counted;
        if (auto const found = _recipesOf.find(skill); found != _recipesOf.end())
            for (std::size_t const position : found->second)
            {
                Recipes::Recipe const& recipe = _world.index.Recipes()[position];
                Learning const learning = LearnOf(recipe);
                if (!learning.ok || learning.known || learning.skill <= from || learning.skill > to)
                    continue;
                if (!counted.insert(recipe.product ? recipe.product : recipe.spell).second)
                    continue;
                double const use = UseWorth(recipe);
                if (HeldRecipe(recipe))
                    brings.recipesHeld += std::max(use, IncomeWorth(recipe, learning.skill));
                else if (use > 0.0)
                    brings.useSoon += use;
                else
                    brings.income += IncomeWorth(recipe, learning.skill);
            }
        // a node kind or a creature level counts once, at the most one fetches in the zones the bot plays in, however
        // many of them it is found in (the WR10/WR11 review)
        std::map<std::uint32_t, double> kinds;
        if (auto const found = _nodesOf.find(skill); found != _nodesOf.end())
            for (NodeKind const& kind : found->second)
                if (Plays(kind.zone) && kind.reqSkill > from && kind.reqSkill <= to)
                    kinds[kind.entry] = std::max(kinds[kind.entry], kind.sellCopper);
        std::map<std::uint32_t, double> levels;
        if (skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Skinning))
            for (Skinnable const& creature : _world.skinnables)
            {
                std::uint32_t const need = SkinRequirement(creature.level);
                if (Plays(creature.zone) && need > from && need <= to)
                    levels[creature.level] =
                        std::max(levels[creature.level], static_cast<double>(creature.copperPerSkin));
            }
        for (auto const* counted : {&kinds, &levels})
            for (auto const& [id, copper] : *counted)
                brings.access += copper * _rates.factor * ACCESS_GATHERS;
        double const nearWeight = Nearness(to, _bot.level);
        brings.useSoon *= nearWeight;
        brings.recipesHeld *= nearWeight;
        brings.access *= nearWeight;
        brings.income *= nearWeight;
        return brings;
    }

    // The milestones above a skill, lowest first: skill -> (kind, id)
    [[nodiscard]] std::vector<std::tuple<std::uint32_t, Milestone, std::uint32_t>> Milestones(std::uint16_t skill,
                                                                                              std::uint32_t from) const
    {
        std::map<std::uint32_t, std::pair<Milestone, std::uint32_t>> at;
        auto const offer = [&at, from](std::uint32_t skillAt, Milestone kind, std::uint32_t id)
        {
            if (skillAt <= from)
                return;
            auto const found = at.find(skillAt);
            if (found == at.end() || std::make_pair(kind, id) < found->second)
                at[skillAt] = {kind, id};
        };
        if (auto const found = _recipesOf.find(skill); found != _recipesOf.end())
            for (std::size_t const position : found->second)
            {
                Recipes::Recipe const& recipe = _world.index.Recipes()[position];
                Learning const learning = LearnOf(recipe);
                if (!learning.ok || learning.known || learning.skill <= from)
                    continue;
                if (HeldRecipe(recipe))
                    offer(learning.skill, Milestone::HeldRecipe, recipe.spell);
                else if (_bot.anyStretch || UseWorth(recipe) > 0.0 || IncomeWorth(recipe, learning.skill) > 0.0)
                    offer(learning.skill, Milestone::Recipe, recipe.spell);
            }
        if (auto const found = _nodesOf.find(skill); found != _nodesOf.end())
            for (NodeKind const& kind : found->second)
                if (Plays(kind.zone))
                    offer(kind.reqSkill, Milestone::Node, kind.entry);
        if (skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Skinning))
            for (Skinnable const& creature : _world.skinnables)
                if (Plays(creature.zone))
                    offer(SkinRequirement(creature.level), Milestone::Skinnable, creature.level);
        std::vector<std::tuple<std::uint32_t, Milestone, std::uint32_t>> milestones;
        for (auto const& [skillAt, what] : at)
        {
            if (milestones.size() >= MAX_STRETCHES)
                break;
            milestones.emplace_back(skillAt, what.first, what.second);
        }
        return milestones;
    }

    // The stretches of every profession weighed, the one taken and what follows from it
    [[nodiscard]] Path Weigh() const
    {
        Path path;
        for (SkillLevel const& level : _skills)
        {
            if (!level.value)
                continue;
            for (auto const& [to, kind, id] : Milestones(level.skill, level.value))
            {
                Stretch stretch = Climb(level.skill, level.value, to);
                stretch.milestone = kind;
                stretch.milestoneId = id;
                stretch.nearness = Nearness(to, _bot.level);
                stretch.brings = BringsOf(level.skill, level.value, to);
                // a blocked climb's cost stops where it stops: it has no net worth
                if (stretch.reason == Reason::Open)
                {
                    stretch.netWorth = _bot.anyStretch ? static_cast<double>(stretch.to - stretch.from) *
                                                             std::max(static_cast<double>(_rates.minuteValue), 1.0)
                                                       : stretch.brings.Total() - stretch.cost.Total();
                    stretch.perMinute = stretch.netWorth / std::max(stretch.minutes, MIN_MINUTES);
                }
                if (stretch.reason == Reason::Open && stretch.copper > static_cast<double>(_bot.money))
                    stretch.reason = Reason::GoldShort;
                if (stretch.reason == Reason::Open && !(stretch.netWorth > 0.0) &&
                    _world.fault != Fault::TakeNonPositive)
                    stretch.reason = Reason::NotWorthIt;
                path.weighed.push_back(std::move(stretch));
            }
        }
        for (std::size_t at = 0; at < path.weighed.size(); ++at)
        {
            Stretch const& stretch = path.weighed[at];
            if (stretch.reason != Reason::Open)
                continue;
            if (path.chosen == NONE || stretch.perMinute > path.weighed[path.chosen].perMinute)
                path.chosen = static_cast<int>(at);
        }
        // Supply before skill (WR15, PLAN 3.11): the uses are claimed with no chosen stretch too
        if (path.chosen == NONE)
        {
            path.reason = Reason::NoStretch;
            for (Stretch const& stretch : path.weighed)
                if (path.reason == Reason::NoStretch || stretch.reason == Reason::NotWorthIt)
                    path.reason = stretch.reason;
            Claims const claims = Claim(path);
            TakeClaims(path, claims);
            return path;
        }
        Stretch& taken = path.weighed[path.chosen];
        taken.reason = Reason::Chosen;
        path.reason = Reason::Chosen;
        // the needs: recipes and ranks, and the tools a vendor sells (a made tool is a cast, its inputs are wants); a
        // recipe priced at a vendor's recipe item names that item (WR14b: the shop buys it)
        for (PathStep const& step : taken.steps)
            if (step.kind == StepKind::Learn || step.kind == StepKind::Rank ||
                (step.kind == StepKind::Tool && !(step.casts > 0.0)))
                path.needs.push_back({step.kind, step.id, static_cast<std::uint32_t>(step.copper),
                                      step.kind == StepKind::Learn ? VendorRecipeItem(taken.skill, step) : 0u});
        Claims const claims = Claim(path);
        TakeClaims(path, claims);
        return path;
    }

    // The claims' queue, wants and materials into the path, and what its uses bring
    void TakeClaims(Path& path, Claims const& claims) const
    {
        path.queue = claims.queue;
        std::vector<Material> materials;
        path.wants = Wants(path, claims, &materials);
        path.materials = std::move(materials);
        path.useWorth = claims.useWorth;
        path.foodUnits = claims.foodUnits;
    }

    // The chosen stretch's wants: what its casts and made tools use beyond what the uses leave, expanded through the
    // bot's own crafts and its prospecting and milling into the materials it gets (PLAN 3.8, Dependencies): the ore,
    // not the bars it smelts. materials (WR12): every material the path uses, by item, the uses' claims included
    [[nodiscard]] std::vector<Want> Wants(Path const& path, Claims const& claims,
                                          std::vector<Material>* materials = nullptr) const
    {
        std::vector<Want> wants;
        std::map<std::uint32_t, Material> used;
        for (auto const& [item, held] : _held)
        {
            auto const after = claims.afterUses.find(item);
            std::uint32_t const left = after == claims.afterUses.end() ? 0 : after->second;
            if (held > left && _world.fault != Fault::WantsOnly)
                Record(used, item, held - left, 0, 0, 0);
        }
        if (path.chosen != NONE && path.chosen < static_cast<int>(path.weighed.size()))
        {
            Stretch const& taken = path.weighed[path.chosen];
            std::set<std::uint32_t> learning;
            std::map<std::uint32_t, std::uint64_t> totals;
            std::map<std::uint32_t, std::uint32_t> firstSkill;  // the skill of the first step that uses an item
            auto const first = [&firstSkill](std::uint32_t item, std::uint32_t skill)
            {
                auto const [at, added] = firstSkill.emplace(item, skill);
                if (!added)
                    at->second = std::min(at->second, skill);
            };
            for (PathStep const& step : taken.steps)
            {
                if (step.kind == StepKind::Learn)
                    learning.insert(step.id);
                if (step.kind == StepKind::Tool && step.casts > 0.0)
                {
                    totals[step.id] += 1;
                    first(step.id, step.from);
                }
                if (Recipes::Recipe const* recipe =
                        step.kind == StepKind::Craft ? RecipeOf(taken.skill, step.id) : nullptr)
                    for (Recipes::ItemCount const& reagent : recipe->reagents)
                    {
                        totals[reagent.item] += static_cast<std::uint64_t>(std::ceil(step.casts)) * reagent.count;
                        first(reagent.item, step.from);
                    }
            }
            Materials::Bot const bot = MaterialBot(taken.skill, taken.from);
            Materials::Pricer const pricer(_world.index, bot);
            std::map<std::uint32_t, std::uint32_t> left = claims.afterUses;
            std::map<std::uint32_t, std::uint64_t> wanted;
            for (auto const& [item, total] : totals)
            {
                std::uint32_t const due = firstSkill[item] > taken.from ? firstSkill[item] : 0;
                Net(pricer, item, total, Materials::MAX_DEPTH, learning, left, wanted,
                    {&used, ClampSkill(taken.skill), ClampSkill(due)});
            }
            for (auto const& [item, count] : wanted)
                wants.push_back({item, static_cast<std::uint32_t>(std::min<std::uint64_t>(count, UINT32_MAX)),
                                 pricer.UnitCost(item).value});
            for (auto& [item, material] : used)
            {
                Materials::Cost const unit = pricer.UnitCost(item);
                material.copperEach = CopperOf(unit.value);
                material.source = static_cast<std::uint8_t>(unit.kind);
                material.via = unit.via;
            }
        }
        if (materials)
            for (auto const& [item, material] : used)
                materials->push_back(material);
        return wants;
    }

    static std::uint16_t ClampSkill(std::uint32_t skill)
    {
        return static_cast<std::uint16_t>(std::min<std::uint32_t>(skill, UINT16_MAX));
    }

    // Adds units of an item to the path's materials; the earliest due skill wins
    static void Record(std::map<std::uint32_t, Material>& used, std::uint32_t item, std::uint64_t units,
                       std::uint64_t wanted, std::uint16_t skill, std::uint16_t dueSkill)
    {
        auto const [at, added] = used.emplace(item, Material{item, 0, 0, 0, skill, dueSkill});
        Material& material = at->second;
        if (!added && dueSkill < material.dueSkill)
        {
            material.skill = skill;
            material.dueSkill = dueSkill;
        }
        material.used = static_cast<std::uint32_t>(std::min<std::uint64_t>(material.used + units, UINT32_MAX));
        material.wanted = static_cast<std::uint32_t>(std::min<std::uint64_t>(material.wanted + wanted, UINT32_MAX));
    }

    // A unit's value in copper at the bot's neutral exchange
    [[nodiscard]] std::uint32_t CopperOf(double value) const
    {
        if (!(_rates.exchange > 0.0f) || !(value > 0.0))
            return 0;
        return static_cast<std::uint32_t>(
            std::min(std::ceil(value / static_cast<double>(_rates.exchange)), static_cast<double>(UINT32_MAX)));
    }

    // Nets a quantity of an item against what is left in the bags, and expands the rest through a recipe the bot knows
    // or the stretch learns, or through prospecting and milling, as the material rules price it (bounded by their
    // depth and quantity); what stays is wanted
    void Net(Materials::Pricer const& pricer, std::uint32_t item, std::uint64_t quantity, std::uint32_t depth,
             std::set<std::uint32_t> const& learning, std::map<std::uint32_t, std::uint32_t>& left,
             std::map<std::uint32_t, std::uint64_t>& wanted, MaterialRecord const& record = {}) const
    {
        std::uint32_t& held = left[item];
        std::uint64_t const fromHeld = std::min<std::uint64_t>(held, quantity);
        held -= static_cast<std::uint32_t>(fromHeld);
        quantity -= fromHeld;
        if (record.used && fromHeld && _world.fault != Fault::WantsOnly)
            Record(*record.used, item, fromHeld, 0, record.skill, record.dueSkill);
        if (!quantity)
            return;
        if (depth && quantity <= Materials::MAX_QUANTITY && _world.fault != Fault::NoExpand)
        {
            Materials::Cost const unit = pricer.UnitCost(item);
            if (Recipes::Recipe const* recipe = unit.kind == Materials::Kind::Made ? MakerOf(item, unit.via) : nullptr;
                recipe && (_known.count(recipe->spell) || learning.count(recipe->spell)))
            {
                std::uint64_t const each = std::max<std::uint32_t>(recipe->productCount, 1);
                std::uint64_t const casts = (quantity + each - 1) / each;
                for (Recipes::ItemCount const& reagent : recipe->reagents)
                    Net(pricer, reagent.item, casts * reagent.count, depth - 1, learning, left, wanted, record);
                return;
            }
            if (unit.kind == Materials::Kind::Processed)
                for (std::size_t const at : _world.index.YieldedBy(item))
                {
                    Recipes::Process const& process = _world.index.Processes()[at];
                    if (process.source != unit.via || process.kind == Recipes::ProcessKind::Disenchant)
                        continue;
                    double expected = 0.0;
                    for (Recipes::Yield const& yield : process.yields)
                        if (yield.item == item)
                            expected += yield.expected;
                    if (!(expected > 0.0))
                        continue;
                    std::uint64_t const casts = static_cast<std::uint64_t>(std::ceil(quantity / expected));
                    Net(pricer, process.source, casts * std::max<std::uint32_t>(process.consumed, 1), depth - 1,
                        learning, left, wanted, record);
                    return;
                }
        }
        wanted[item] += quantity;
        if (record.used)
            Record(*record.used, item, quantity, quantity, record.skill, record.dueSkill);
    }

    // The tools got with one of the sources' items: the item and the category it covers
    void Acquire(Acquired& acquired, std::uint32_t item) const
    {
        acquired.items.insert(item);
        for (ToolSource const& source : _world.tools)
            if (source.item == item && source.category)
                acquired.categories.insert(source.category);
    }

    // The claims of the next stop on the bags: what the bot uses (consumables to their wanted stock, then upgrades),
    // the intermediates for those, then the chosen stretch's skill crafts from what is left
    [[nodiscard]] Claims Claim(Path const& path) const
    {
        Claims claims;
        std::map<std::uint32_t, std::uint32_t> left = _held;
        std::map<std::uint32_t, std::uint32_t> made;
        auto const have = [&](std::uint32_t item)
        {
            auto const a = left.find(item);
            auto const b = made.find(item);
            return (a == left.end() ? 0u : a->second) + (b == made.end() ? 0u : b->second);
        };
        auto const possible = [&](Recipes::Recipe const& recipe)
        {
            std::uint32_t casts = CraftRules::CAST_CEILING;
            for (Recipes::ItemCount const& reagent : recipe.reagents)
                casts = std::min(casts, reagent.count ? have(reagent.item) / reagent.count : casts);
            return casts;
        };
        auto const takeItem = [&](std::uint32_t item, std::uint32_t amount)
        {
            std::uint32_t& fresh = made[item];
            std::uint32_t const fromMade = std::min(fresh, amount);
            fresh -= fromMade;
            amount -= fromMade;
            std::uint32_t& held = left[item];
            std::uint32_t const fromHeld = _world.fault == Fault::ReserveUncapped ? amount : std::min(held, amount);
            held -= std::min(held, fromHeld);
            if (fromHeld)
                claims.reserved[item] += fromHeld;
        };
        auto const take = [&](Recipes::Recipe const& recipe, std::uint32_t casts)
        {
            for (Recipes::ItemCount const& reagent : recipe.reagents)
                takeItem(reagent.item, reagent.count * casts);
        };
        // Up to units of an item by prospecting or milling a held source the bot's skill takes, at the expected
        // yield, as the material rules price the item (PLAN 3.10: a cast on an item in the same visit); a yield is
        // random, so the live stop casts what the bags then hold
        auto const process = [&](std::uint32_t item, std::uint32_t units, bool forSkill)
        {
            for (std::size_t const at : _world.index.YieldedBy(item))
            {
                Recipes::Process const& row = _world.index.Processes()[at];
                double expected = 0.0;
                for (Recipes::Yield const& yield : row.yields)
                    if (yield.item == item)
                        expected += yield.expected;
                if (row.kind == Recipes::ProcessKind::Disenchant || SkillValue(row.skill) < row.rank ||
                    !(expected > 0.0))
                    continue;
                std::uint32_t const consumed = std::max<std::uint32_t>(row.consumed, 1);
                std::uint32_t const casts = static_cast<std::uint32_t>(
                    std::min<double>(std::ceil((units - have(item)) / expected), have(row.source) / consumed));
                if (!casts)
                    continue;
                takeItem(row.source, casts * consumed);
                for (Recipes::Yield const& yield : row.yields)
                    made[yield.item] += static_cast<std::uint32_t>(casts * yield.expected);
                std::uint32_t const spell = Recipes::ProcessSpell(row.kind);
                claims.queue.push_back({spell, casts, forSkill, row.source});
                if (!forSkill)
                    claims.useCasts[spell] += casts;
                if (have(item) >= units)
                    return;
            }
        };
        // Up to units of an item made from what is held by the recipe the bot knows, its own inputs made first
        // (bounded by the material rules' depth), else by processing; each cast queued before the craft that takes
        // its product
        std::function<void(std::uint32_t, std::uint32_t, bool, std::uint32_t)> make;
        make = [&](std::uint32_t item, std::uint32_t units, bool forSkill, std::uint32_t depth)
        {
            if (!depth || have(item) >= units)
                return;
            Recipes::Recipe const* maker = KnownMaker(item);
            if (!maker)
            {
                process(item, units, forSkill);
                return;
            }
            std::uint32_t const each = std::max<std::uint32_t>(maker->productCount, 1);
            std::uint32_t const wanted = (units - have(item) + each - 1) / each;
            if (possible(*maker) < wanted)
                for (Recipes::ItemCount const& reagent : maker->reagents)
                    make(reagent.item, reagent.count * wanted, forSkill, depth - 1);
            std::uint32_t const casts = std::min(wanted, possible(*maker));
            if (!casts)
                return;
            take(*maker, casts);
            made[item] += casts * each;
            claims.queue.push_back({maker->spell, casts, forSkill});
            if (!forSkill)
                claims.useCasts[maker->spell] += casts;
        };
        // A recipe's casts: its intermediates first, then as many casts as the bags then allow
        auto const cast = [&](Recipes::Recipe const& recipe, std::uint32_t wanted, bool forSkill)
        {
            if (_world.fault != Fault::ReserveUncapped && possible(recipe) < wanted)
                for (Recipes::ItemCount const& reagent : recipe.reagents)
                    make(reagent.item, reagent.count * wanted, forSkill, Materials::MAX_DEPTH);
            std::uint32_t const casts =
                _world.fault == Fault::ReserveUncapped && !forSkill ? wanted : std::min(wanted, possible(recipe));
            if (!casts)
                return;
            take(recipe, casts);
            claims.queue.push_back({recipe.spell, casts, forSkill});
            if (!forSkill)
                claims.useCasts[recipe.spell] += casts;
        };
        // Up to missing units of an item by its known recipe; the units the queued casts make
        auto const forUse = [&](std::uint32_t item, std::uint32_t missing) -> std::uint32_t
        {
            Recipes::Recipe const* recipe = KnownMaker(item);
            if (!recipe || !missing)
                return 0;
            std::uint32_t const perCast = std::max<std::uint32_t>(recipe->productCount, 1);
            std::size_t const before = claims.queue.size();
            cast(*recipe, (missing + perCast - 1) / perCast, false);
            if (claims.queue.size() == before || claims.queue.back().spell != recipe->spell)
                return 0;
            return std::min(missing, claims.queue.back().casts * perCast);
        };
        // The wants of one group share its stock (WR15: the slot's entries the bot makes, the lowest item first)
        auto const uses = [&]()
        {
            std::map<std::uint32_t, std::uint32_t> groupMissing;
            for (ConsumableWant const& want : _consumables)
            {
                if (want.wantedStock <= want.held)
                    continue;
                std::uint32_t missing = want.wantedStock - want.held;
                if (want.group)
                    missing = groupMissing.try_emplace(want.group, missing).first->second;
                std::uint32_t const units = forUse(want.item, missing);
                if (want.group)
                    groupMissing[want.group] = missing - units;
                claims.useWorth += static_cast<double>(want.worthCopperEach) * units * _rates.exchange;
                if (want.food)
                    claims.foodUnits += units;
            }
            for (UseWant const& want : _uses)
                if (want.item && !_held.count(want.item) && forUse(want.item, 1))
                    claims.useWorth +=
                        static_cast<double>(want.worthCopperEach) * _rates.exchange * UseWeight(want.level, _bot.level);
        };
        auto const skill = [&]()
        {
            if (path.chosen == NONE || path.chosen >= static_cast<int>(path.weighed.size()))
                return;
            Stretch const& taken = path.weighed[path.chosen];
            for (PathStep const& step : taken.steps)
            {
                if (step.kind == StepKind::Tool && step.casts > 0.0)
                    make(step.id, 1, true, Materials::MAX_DEPTH);  // a made tool before the casts that need it
                Recipes::Recipe const* recipe = step.kind == StepKind::Craft ? RecipeOf(taken.skill, step.id) : nullptr;
                if (recipe)
                    cast(*recipe, static_cast<std::uint32_t>(std::ceil(step.casts)), true);
            }
        };
        if (_world.fault == Fault::SkillFirst)
        {
            skill();
            claims.afterUses = left;
            uses();
        }
        else
        {
            uses();
            claims.afterUses = left;
            skill();
        }
        return claims;
    }

private:
    struct NodeKind
    {
        std::uint32_t zone{0};
        std::uint32_t entry{0};
        std::uint32_t reqSkill{0};
        float spacing{0.0f};
        double sellCopper{0.0};  // what one node's items fetch
    };

    // A trainer teaches the bot the recipe: it lists it, the bot has the level and knows every ability the trainer
    // asks for (WR.27: a specialisation's recipe is never planned for a bot without the specialisation)
    [[nodiscard]] bool Trains(Recipes::Recipe const& recipe) const
    {
        return recipe.trainers && _bot.level >= recipe.trainerLevel &&
               (_world.fault == Fault::IgnoreAbility ||
                Recipes::TrainerTeaches(recipe, [this](std::uint32_t spell) { return _known.count(spell) > 0; }));
    }

    [[nodiscard]] bool MayLearn(std::uint32_t spell) const
    {
        return _world.fault == Fault::IgnoreLock || !_world.mayLearn || _world.mayLearn(spell);
    }

    // What one unit fetches: a vendor's price, or a listing's proceeds where the market pays more (auction house
    // WA7a, PLAN 3.7: the path's income term); the vendor's alone without a market. Kept per item for the weigh
    [[nodiscard]] double Sell(std::uint32_t item) const
    {
        double const vendor = _world.sellCopper ? _world.sellCopper(item) : 0.0;
        if (_world.market.Empty())
            return vendor;
        auto const [at, added] = _sold.try_emplace(item, vendor);
        if (added)
            at->second = _world.market.SaleOf(item, vendor);
        return at->second;
    }

    [[nodiscard]] bool Plays(std::uint32_t zone) const { return _zones.empty() || _zones.count(zone); }

    [[nodiscard]] std::uint32_t SkillValue(std::uint32_t skill) const
    {
        for (SkillLevel const& level : _skills)
            if (level.skill == skill)
                return level.value;
        return 0;
    }

    [[nodiscard]] std::uint32_t SkillMax(std::uint16_t skill) const
    {
        for (SkillLevel const& level : _skills)
            if (level.skill == skill)
                return level.max;
        return 0;
    }

    [[nodiscard]] bool HeldRecipe(Recipes::Recipe const& recipe) const
    {
        for (Recipes::RecipeSource const& source : recipe.items)
            if (_recipeItems.count(source.item))
                return true;
        return false;
    }

    // The vendor's recipe item a learn step was priced at (Learning's offers: a trainer at its fee, a sold item at its
    // price); 0 when a trainer teaches it at that price, the bot holds the item (fee 0) or no vendor sells one
    [[nodiscard]] std::uint32_t VendorRecipeItem(std::uint16_t skill, PathStep const& step) const
    {
        Recipes::Recipe const* recipe = RecipeOf(skill, step.id);
        std::uint32_t const fee = static_cast<std::uint32_t>(step.copper);
        if (!recipe || !fee || (Trains(*recipe) && recipe->trainerCopper <= fee))
            return 0;
        for (Recipes::RecipeSource const& source : recipe->items)
            if (source.vendors && source.buyCopper == fee)
                return source.item;
        return 0;
    }

    [[nodiscard]] Recipes::Recipe const* RecipeOf(std::uint16_t skill, std::uint32_t spell) const
    {
        if (auto const found = _recipesOf.find(skill); found != _recipesOf.end())
            for (std::size_t const position : found->second)
                if (_world.index.Recipes()[position].spell == spell)
                    return &_world.index.Recipes()[position];
        return nullptr;
    }

    // The known recipe with the lowest spell id that makes an item, at the bot's skill
    [[nodiscard]] Recipes::Recipe const* KnownMaker(std::uint32_t item) const
    {
        Recipes::Recipe const* best = nullptr;
        for (std::size_t const position : _world.index.MadeBy(item))
        {
            Recipes::Recipe const& recipe = _world.index.Recipes()[position];
            if (_known.count(recipe.spell) && (!best || recipe.spell < best->spell))
                best = &recipe;
        }
        return best;
    }

    // What the bot's uses make a recipe's product worth, value units at the neutral exchange
    [[nodiscard]] double UseWorth(Recipes::Recipe const& recipe) const
    {
        double worth = 0.0;
        for (ConsumableWant const& want : _consumables)
            if (recipe.product && want.item == recipe.product && want.wantedStock > want.held)
                worth += static_cast<double>(want.worthCopperEach) * (want.wantedStock - want.held) * _rates.exchange;
        for (UseWant const& want : _uses)
            if ((want.item && want.item == recipe.product) || (want.spell && want.spell == recipe.spell))
                worth +=
                    static_cast<double>(want.worthCopperEach) * _rates.exchange * UseWeight(want.level, _bot.level);
        return worth;
    }

    // What selling a few casts of a product would bring over its materials and casts, at the bot's copper weight
    [[nodiscard]] double IncomeWorth(Recipes::Recipe const& recipe, std::uint32_t at) const
    {
        double const sold = recipe.product ? Sell(recipe.product) * std::max<std::uint32_t>(recipe.productCount, 1) : 0;
        if (!(sold > 0.0))
            return 0.0;
        Materials::Bot const materials = MaterialBot(recipe.skill, at);
        Materials::Pricer const pricer(_world.index, materials);
        double cost = Materials::CastMinutes(static_cast<float>(recipe.castMs)) * _rates.minuteValue;
        for (Recipes::ItemCount const& reagent : recipe.reagents)
        {
            Materials::Cost const unit = pricer.UnitCost(reagent.item);
            if (unit.Blocked())
                return 0.0;
            cost += unit.value * reagent.count;
        }
        return std::max(0.0, sold * _rates.factor - cost) * INCOME_CASTS;
    }

    // The recipe with this spell that makes an item
    [[nodiscard]] Recipes::Recipe const* MakerOf(std::uint32_t item, std::uint32_t spell) const
    {
        for (std::size_t const position : _world.index.MadeBy(item))
            if (_world.index.Recipes()[position].spell == spell)
                return &_world.index.Recipes()[position];
        return nullptr;
    }

    // The source of a tool asked for as an item (category 0) or by its totem category, the lowest item id first
    [[nodiscard]] ToolSource const* SourceOf(std::uint32_t item, std::uint32_t category) const
    {
        ToolSource const* found = nullptr;
        for (ToolSource const& source : _world.tools)
            if ((item ? source.item == item : source.category == category) && (!found || source.item < found->item))
                found = &source;
        return found;
    }

    [[nodiscard]] bool Holds(std::uint32_t item, std::uint32_t category, Acquired const& acquired) const
    {
        return item ? _tools.count(item) || acquired.items.count(item)
                    : _categories.count(category) || acquired.categories.count(category);
    }

    // A tool the cast asks for (WR.12, PLAN 3.4): held or got already, else bought from a vendor at its price, else
    // made by a recipe of the profession the bot may learn at this skill (learned first, its own tools bought); false
    // when it has none of these
    template <class CostOf>
    bool GetTool(std::uint32_t item, std::uint32_t category, std::uint16_t skill, std::uint32_t at,
                 Acquired const& acquired, CostOf const& cost, std::uint32_t depth, std::vector<ToolGet>& gets) const
    {
        if (Holds(item, category, acquired))
            return true;
        ToolSource const* source = SourceOf(item, category);
        if (!source)
            return false;
        if (std::any_of(gets.begin(), gets.end(), [source](ToolGet const& got) { return got.item == source->item; }))
            return true;
        ToolGet get;
        get.item = source->item;
        if (source->copper)
        {
            get.copper = source->copper;
            get.fees = source->copper * static_cast<double>(_rates.factor);
            gets.push_back(get);
            return true;
        }
        Recipes::Recipe const* maker = nullptr;
        if (auto const found = _recipesOf.find(skill); depth && found != _recipesOf.end())
            for (std::size_t const position : found->second)
            {
                Recipes::Recipe const& recipe = _world.index.Recipes()[position];
                if (recipe.product == source->item && recipe.effect == Recipes::Effect::Make &&
                    (!maker || recipe.spell < maker->spell))
                    maker = &recipe;
            }
        Learning const learning = maker ? LearnOf(*maker) : Learning{};
        if (!maker || !learning.ok || at < learning.skill)
            return false;
        std::vector<ToolGet> with = gets;
        for (std::uint32_t const tool : maker->tools)
            if (!GetTool(tool, 0, skill, at, acquired, cost, depth - 1, with))
                return false;
        for (std::uint32_t const needed : maker->toolCategories)
            if (!GetTool(0, needed, skill, at, acquired, cost, depth - 1, with))
                return false;
        for (Recipes::ItemCount const& reagent : maker->reagents)
        {
            Materials::Cost const& unit = cost(reagent.item);
            if (unit.Blocked())
                return false;
            get.materials += unit.value * reagent.count;
            get.minutes += unit.minutes * reagent.count;
            get.copper += unit.copperPaid * reagent.count;
        }
        double const castMinutes = Materials::CastMinutes(static_cast<float>(maker->castMs));
        get.maker = maker->spell;
        get.time = castMinutes * _rates.minuteValue;
        get.minutes += castMinutes;
        if (learning.step)
        {
            get.learn = maker->spell;
            get.fee = learning.fee;
            get.fees = learning.fee * static_cast<double>(_rates.factor);
            get.copper += learning.fee;
        }
        with.push_back(get);
        gets = std::move(with);
        return true;
    }

    template <class CostOf>
    bool CraftOption(Recipes::Recipe const& recipe, std::uint32_t at, CostOf const& cost, Acquired const& acquired,
                     std::uint32_t points, Option& option) const
    {
        if (recipe.effect != Recipes::Effect::Make && recipe.effect != Recipes::Effect::Enchant)
            return false;
        Learning const learning = LearnOf(recipe);
        if (!learning.ok || at < learning.skill)
            return false;
        int chance = CraftChancePerMille(at, recipe.grey, recipe.yellow, _world.index.Rates());
        if (chance <= 0)
        {
            if (_world.fault != Fault::AllowGrey)
                return false;
            chance = PER_MILLE;
        }
        if (_world.fault != Fault::ToolFree)
        {
            for (std::uint32_t const tool : recipe.tools)
                if (!GetTool(tool, 0, recipe.skill, at, acquired, cost, TOOL_DEPTH, option.tools))
                    return false;
            for (std::uint32_t const category : recipe.toolCategories)
                if (!GetTool(0, category, recipe.skill, at, acquired, cost, TOOL_DEPTH, option.tools))
                    return false;
        }
        double materials = 0.0;
        double minutes = 0.0;
        double copper = 0.0;
        for (Recipes::ItemCount const& reagent : recipe.reagents)
        {
            Materials::Cost const& unit = cost(reagent.item);
            if (unit.Blocked())
            {
                if (_world.fault == Fault::BlockedFree)
                    continue;
                return false;
            }
            materials += unit.value * reagent.count;
            minutes += unit.minutes * reagent.count;
            copper += unit.copperPaid * reagent.count;
        }
        if (_world.fault == Fault::DearerCheaper)
            materials = -materials;
        double const cast = Materials::CastMinutes(static_cast<float>(recipe.castMs));
        double const sold = recipe.product ? Sell(recipe.product) * std::max<std::uint32_t>(recipe.productCount, 1) : 0;
        double const perSkillUp = static_cast<double>(PER_MILLE) / chance;
        option.kind = StepKind::Craft;
        option.id = recipe.spell;
        option.chance = chance;
        option.points = points;
        option.materials = (materials - sold * _rates.factor) * perSkillUp;
        option.time = cast * _rates.minuteValue * perSkillUp;
        option.copper = (copper - sold) * perSkillUp;
        option.minutes = (minutes + cast) * perSkillUp;
        if (learning.step)
        {
            option.learn = recipe.spell;
            option.fee = learning.fee;
            // the price spread over the points the recipe serves, from where it is learned to grey: a skill-up's
            // points carry their shares
            double const served = std::max<double>(recipe.grey > learning.skill ? recipe.grey - learning.skill : 1, 1);
            option.share = learning.fee * static_cast<double>(_rates.factor) / served * points;
            option.copper += learning.fee / served * points;
        }
        // the tools it gets first count in full at this point; the climb has them from here on
        for (ToolGet const& tool : option.tools)
        {
            option.materials += tool.materials;
            option.time += tool.time;
            option.share += tool.fees;
            option.copper += tool.copper;
            option.minutes += tool.minutes;
        }
        option.value = option.materials + option.time + option.share;
        return true;
    }

    void GatherOptions(std::uint16_t skill, std::uint32_t at, std::uint32_t points, std::vector<Option>& options) const
    {
        auto const add =
            [&](StepKind kind, std::uint32_t id, std::uint32_t zone, int chance, double minutes, double sold)
        {
            if (chance <= 0)
            {
                if (_world.fault != Fault::AllowGrey)
                    return;
                chance = PER_MILLE;
            }
            auto const risk = _bot.risks.find(zone);
            double const minuteCost =
                Materials::MinuteCost(risk == _bot.risks.end() ? _bot.anyZone : risk->second, _rates);
            if (minuteCost < 0.0)
                return;
            double const perSkillUp = static_cast<double>(PER_MILLE) / chance;
            Option option;
            option.kind = kind;
            option.id = id;
            option.zone = zone;
            option.chance = chance;
            option.points = points;
            option.time = minutes * minuteCost * perSkillUp;
            option.materials = -sold * _rates.factor * perSkillUp;
            option.copper = -sold * perSkillUp;
            option.minutes = minutes * perSkillUp;
            option.value = option.materials + option.time;
            options.push_back(option);
        };
        if (auto const found = _nodesOf.find(skill); found != _nodesOf.end())
            for (NodeKind const& kind : found->second)
                if (Plays(kind.zone) && at >= kind.reqSkill)
                    add(StepKind::Gather, kind.entry, kind.zone,
                        GatherChancePerMille(at, kind.reqSkill, skill, _world.index.Rates()),
                        kind.spacing / (std::max(_rates.runSpeed, 1.0f) * Planner::Goals::SECONDS_PER_MINUTE) +
                            Planner::Goals::GATHER_MINUTES,
                        kind.sellCopper);
        if (skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Skinning))
            for (Skinnable const& creature : _world.skinnables)
                if (Plays(creature.zone) && at >= SkinRequirement(creature.level))
                    add(StepKind::Skin, creature.level, creature.zone,
                        GatherChancePerMille(at, SkinRequirement(creature.level), skill, _world.index.Rates()),
                        creature.minutesPerKill, creature.copperPerSkin);
        if (skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Fishing))
            for (Water const& water : _world.waters)
                if (Plays(water.zone))
                    add(StepKind::Fish, 0, water.zone, FishChancePerMille(at), water.minutesPerCast, 0.0);
    }

    // The lower value per point wins; equal values go to the lower kind, id and zone (whatever the order of the lists)
    [[nodiscard]] bool Better(Option const& a, Option const& b) const
    {
        double const left = a.value * b.points;
        double const right = b.value * a.points;
        if (left != right || _world.fault == Fault::OrderTie)
            return left < right;
        return std::tie(a.kind, a.id, a.zone) < std::tie(b.kind, b.id, b.zone);
    }

    static void AddPurchase(Stretch& stretch, StepKind kind, std::uint32_t spell, std::uint32_t at,
                            std::uint32_t copper)
    {
        PathStep step;
        step.kind = kind;
        step.id = spell;
        step.from = at;
        step.to = at;
        step.copper = copper;
        stretch.steps.push_back(step);
    }

    PathBot const& _bot;
    PathWorld const& _world;
    Materials::Rates _rates;  // the bot's rates (copper weight inverted only by the PoorInverted fault)
    std::set<std::uint32_t> _known;
    std::set<std::uint32_t> _recipeItems;
    std::set<std::uint32_t> _tools;
    std::set<std::uint32_t> _categories;
    std::set<std::uint32_t> _zones;
    std::map<std::uint32_t, std::uint32_t> _held;
    std::vector<ConsumableWant> _consumables;
    std::vector<UseWant> _uses;
    std::vector<SkillLevel> _skills;
    std::map<std::uint16_t, std::vector<std::size_t>> _recipesOf;  // positions in the index's recipes
    std::map<std::uint16_t, std::vector<NodeKind>> _nodesOf;       // by zone and entry
    mutable std::unordered_map<std::uint32_t, double> _sold;       // Sell's answers with a market
};

// The skill path of a bot: every stretch weighed, the one taken, its needs, wants and crafts
inline Path Weigh(PathBot const& bot, PathWorld const& world) { return Weigher(bot, world).Weigh(); }

// The crafts for the bot's next stop, in the order of claims
inline std::vector<Craft> NextCasts(Path const& path, PathBot const& bot, PathWorld const& world)
{
    return Weigher(bot, world).Claim(path).queue;
}
}  // namespace SkillPath

#endif
