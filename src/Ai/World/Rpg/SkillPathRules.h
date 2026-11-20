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
// The engine's chances of a skill point in per mille, as the core computes them (PlayerUpdates.cpp SkillGainChance,
// CraftSkillGainChance, getProbabilityOfLevelUp): the worldserver's SkillChance.* defaults; its config can shift them
constexpr int PER_MILLE = 1000;
constexpr int CHANCE_ORANGE = 1000;
constexpr int CHANCE_YELLOW = 750;
constexpr int CHANCE_GREEN = 250;
constexpr int CHANCE_GREY = 0;
constexpr std::uint32_t GATHER_YELLOW_FROM = 25;  // a node: yellow from its skill + 25
constexpr std::uint32_t GATHER_GREEN_FROM = 50;
constexpr std::uint32_t GATHER_GREY_FROM = 100;
constexpr std::array<std::uint32_t, 10> FISH_BOUNDS = {115, 135, 160, 190, 215, 295, 315, 355, 425, 450};
constexpr std::array<float, 11> FISH_CATCHES_PER_POINT = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f, 6.0f,
                                                          9.0f, 10.0f, 11.0f, 12.0f, 1.0f};
constexpr float FISH_PERCENT = 100.0f;
constexpr int PERCENT_TO_PER_MILLE = 10;
constexpr std::uint32_t SKIN_FREE_BELOW = 10;  // creatures below level 10 ask no skill (SpellEffects.cpp)
constexpr std::uint32_t SKIN_LOW_BELOW = 20;
constexpr std::uint32_t SKIN_LOW_PER_LEVEL = 10;
constexpr std::uint32_t SKIN_PER_LEVEL = 5;

constexpr std::uint32_t EXPECTED_SKILL_PER_LEVEL = 5;  // what a bot of a level usually has (WR.5)
constexpr std::uint32_t NEARNESS_SPAN = 75;            // [E] a milestone counts nothing one rank above level x 5
constexpr std::uint32_t ITEM_RECIPE_YELLOW_GAP = 25;   // [E] a recipe item's skill: its yellow less this
constexpr double INCOME_CASTS = 5.0;                   // [E] casts of a product sold that income counts
constexpr double ACCESS_GATHERS = 10.0;                // [E] nodes or skins of a newly usable kind that access counts
constexpr std::size_t MAX_STRETCHES = 8;               // stretches weighed per profession
constexpr std::uint32_t MAX_POINTS = 450;              // points of one climb
constexpr double MIN_MINUTES = Materials::CAST_FLOOR_MS / Planner::Goals::MINUTE_MS;

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
    OrderTie          // 10: keeps the order of the recipe list and of the bot's wants
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

inline int CraftChancePerMille(std::uint32_t skill, std::uint32_t grey, std::uint32_t yellow)
{
    if (grey <= yellow)
        return skill < grey ? CHANCE_ORANGE : CHANCE_GREY;
    if (skill <= yellow)
        return CHANCE_ORANGE;
    if (skill >= grey)
        return CHANCE_GREY;
    return CHANCE_GREY +
           static_cast<int>(static_cast<std::int64_t>(grey - skill) * (CHANCE_ORANGE - CHANCE_GREY) / (grey - yellow));
}

// A node or creature whose own skill is red: orange below red + 25, yellow to + 50, green to + 100, then grey
inline int GatherChancePerMille(std::uint32_t skill, std::uint32_t red)
{
    if (skill >= red + GATHER_GREY_FROM)
        return CHANCE_GREY;
    if (skill >= red + GATHER_GREEN_FROM)
        return CHANCE_GREEN;
    if (skill >= red + GATHER_YELLOW_FROM)
        return CHANCE_YELLOW;
    return CHANCE_ORANGE;
}

inline int FishChancePerMille(std::uint32_t skill)
{
    if (!skill)
        return 0;
    auto const at = std::lower_bound(FISH_BOUNDS.begin(), FISH_BOUNDS.end(), skill);
    float const catches = FISH_CATCHES_PER_POINT[static_cast<std::size_t>(std::distance(FISH_BOUNDS.begin(), at))];
    return static_cast<int>(FISH_PERCENT / catches) * PERCENT_TO_PER_MILLE;
}

// The skinning skill a creature of a level asks for
constexpr std::uint32_t SkinRequirement(std::uint32_t level)
{
    return level < SKIN_FREE_BELOW  ? 0
           : level < SKIN_LOW_BELOW ? (level - SKIN_FREE_BELOW) * SKIN_LOW_PER_LEVEL
                                    : level * SKIN_PER_LEVEL;
}

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

struct PathBot
{
    std::uint32_t level{1};
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
};

struct PathWorld
{
    Recipes::RecipeIndex const& index;
    std::function<bool(std::uint32_t)> mayLearn;      // the lock seam (WR.11); empty: everything may be learned
    std::function<double(std::uint32_t)> sellCopper;  // what one unit fetches at a vendor; empty: nothing
    std::vector<Skinnable> skinnables;
    std::vector<Water> waters;
    Fault fault{Fault::None};
};

// One way to make the next skill point, per point
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
    std::uint32_t learn{0};  // the recipe to learn first
    std::uint32_t fee{0};    // its full price

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
                          return std::tie(a.item, a.wantedStock, a.held, a.worthCopperEach) <
                                 std::tie(b.item, b.wantedStock, b.held, b.worthCopperEach);
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
        if (recipe.trainers && _bot.level >= recipe.trainerLevel)
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

    // Every way to make the point from at to at + 1, in no particular order
    [[nodiscard]] std::vector<Option> Options(std::uint16_t skill, std::uint32_t at) const
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
                if (CraftOption(_world.index.Recipes()[position], at, cost, option))
                    options.push_back(option);
            }
        GatherOptions(skill, at, options);
        return options;
    }

    // The cheapest way to make the next point; false when there is none
    [[nodiscard]] bool Best(std::uint16_t skill, std::uint32_t at, Option& best) const
    {
        bool found = false;
        for (Option const& option : Options(skill, at))
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
            Option option;
            if (!Best(skill, at, option))
            {
                stretch.reason = Reason::NoSource;
                stretch.blockedAt = at;
                return stretch;
            }
            if (option.learn && learned.insert(option.learn).second)
                AddPurchase(stretch, StepKind::Learn, option.learn, at, option.fee);
            PathStep* step = stretch.steps.empty() ? nullptr : &stretch.steps.back();
            if (!step || step->kind != option.kind || step->id != option.id || step->zone != option.zone ||
                step->to != at)
            {
                stretch.steps.push_back({option.kind, option.id, at, at, 0.0, 0.0, 0.0, 0.0, option.zone});
                step = &stretch.steps.back();
            }
            step->to = at + 1;
            step->casts += option.Casts();
            step->copper += option.copper;
            step->minutes += option.minutes;
            step->value += option.value;
            stretch.cost.materials += option.materials;
            stretch.cost.time += option.time;
            stretch.cost.fees += option.share;
            stretch.copper += option.copper;
            stretch.minutes += option.minutes;
            ++at;
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
        if (auto const found = _nodesOf.find(skill); found != _nodesOf.end())
            for (NodeKind const& kind : found->second)
                if (Plays(kind.zone) && kind.reqSkill > from && kind.reqSkill <= to)
                    brings.access += kind.sellCopper * _rates.factor * ACCESS_GATHERS;
        if (skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Skinning))
            for (Skinnable const& creature : _world.skinnables)
            {
                std::uint32_t const need = SkinRequirement(creature.level);
                if (Plays(creature.zone) && need > from && need <= to)
                    brings.access += creature.copperPerSkin * _rates.factor * ACCESS_GATHERS;
            }
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
                else if (UseWorth(recipe) > 0.0 || IncomeWorth(recipe, learning.skill) > 0.0)
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
                    stretch.netWorth = stretch.brings.Total() - stretch.cost.Total();
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
        if (path.chosen == NONE)
        {
            path.reason = Reason::NoStretch;
            for (Stretch const& stretch : path.weighed)
                if (path.reason == Reason::NoStretch || stretch.reason == Reason::NotWorthIt)
                    path.reason = stretch.reason;
            return path;
        }
        Stretch& taken = path.weighed[path.chosen];
        taken.reason = Reason::Chosen;
        path.reason = Reason::Chosen;
        for (PathStep const& step : taken.steps)
            if (step.kind == StepKind::Learn || step.kind == StepKind::Rank)
                path.needs.push_back({step.kind, step.id, static_cast<std::uint32_t>(step.copper)});
        Claims const claims = Claim(path);
        path.queue = claims.queue;
        std::map<std::uint32_t, std::uint32_t> totals;
        for (PathStep const& step : taken.steps)
            if (Recipes::Recipe const* recipe = step.kind == StepKind::Craft ? RecipeOf(taken.skill, step.id) : nullptr)
                for (Recipes::ItemCount const& reagent : recipe->reagents)
                    totals[reagent.item] += static_cast<std::uint32_t>(std::ceil(step.casts)) * reagent.count;
        Materials::Bot const materials = MaterialBot(taken.skill, taken.from);
        Materials::Pricer const pricer(_world.index, materials);
        for (auto const& [item, total] : totals)
        {
            auto const left = claims.afterUses.find(item);
            std::uint32_t const held = left == claims.afterUses.end() ? 0u : left->second;
            if (total > held)
                path.wants.push_back({item, total - held, pricer.UnitCost(item).value});
        }
        return path;
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
        auto const take = [&](Recipes::Recipe const& recipe, std::uint32_t casts)
        {
            for (Recipes::ItemCount const& reagent : recipe.reagents)
            {
                std::uint32_t amount = reagent.count * casts;
                std::uint32_t& fresh = made[reagent.item];
                std::uint32_t const fromMade = std::min(fresh, amount);
                fresh -= fromMade;
                amount -= fromMade;
                std::uint32_t& held = left[reagent.item];
                std::uint32_t const fromHeld = _world.fault == Fault::ReserveUncapped ? amount : std::min(held, amount);
                held -= std::min(held, fromHeld);
                if (fromHeld)
                    claims.reserved[reagent.item] += fromHeld;
            }
        };
        auto const forUse = [&](std::uint32_t item, std::uint32_t missing)
        {
            Recipes::Recipe const* recipe = KnownMaker(item);
            if (!recipe || !missing)
                return;
            std::uint32_t const perCast = std::max<std::uint32_t>(recipe->productCount, 1);
            std::uint32_t const wanted = (missing + perCast - 1) / perCast;
            if (_world.fault != Fault::ReserveUncapped && possible(*recipe) < wanted)
                for (Recipes::ItemCount const& reagent : recipe->reagents)
                {
                    std::uint32_t const need = reagent.count * wanted;
                    Recipes::Recipe const* maker = have(reagent.item) < need ? KnownMaker(reagent.item) : nullptr;
                    if (!maker)
                        continue;
                    std::uint32_t const each = std::max<std::uint32_t>(maker->productCount, 1);
                    std::uint32_t const casts =
                        std::min((need - have(reagent.item) + each - 1) / each, possible(*maker));
                    if (!casts)
                        continue;
                    take(*maker, casts);
                    made[reagent.item] += casts * each;
                    claims.queue.push_back({maker->spell, casts, false});
                    claims.useCasts[maker->spell] += casts;
                }
            std::uint32_t const casts =
                _world.fault == Fault::ReserveUncapped ? wanted : std::min(wanted, possible(*recipe));
            if (!casts)
                return;
            take(*recipe, casts);
            claims.queue.push_back({recipe->spell, casts, false});
            claims.useCasts[recipe->spell] += casts;
        };
        auto const uses = [&]()
        {
            for (ConsumableWant const& want : _consumables)
                if (want.wantedStock > want.held)
                    forUse(want.item, want.wantedStock - want.held);
            for (UseWant const& want : _uses)
                if (want.item && !_held.count(want.item))
                    forUse(want.item, 1);
        };
        auto const skill = [&]()
        {
            if (path.chosen == NONE || path.chosen >= static_cast<int>(path.weighed.size()))
                return;
            Stretch const& taken = path.weighed[path.chosen];
            for (PathStep const& step : taken.steps)
            {
                Recipes::Recipe const* recipe = step.kind == StepKind::Craft ? RecipeOf(taken.skill, step.id) : nullptr;
                if (!recipe)
                    continue;
                std::uint32_t const casts =
                    std::min(static_cast<std::uint32_t>(std::ceil(step.casts)), possible(*recipe));
                if (!casts)
                    continue;
                take(*recipe, casts);
                claims.queue.push_back({recipe->spell, casts, true});
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

    [[nodiscard]] bool MayLearn(std::uint32_t spell) const
    {
        return _world.fault == Fault::IgnoreLock || !_world.mayLearn || _world.mayLearn(spell);
    }

    [[nodiscard]] double Sell(std::uint32_t item) const { return _world.sellCopper ? _world.sellCopper(item) : 0.0; }

    [[nodiscard]] bool Plays(std::uint32_t zone) const { return _zones.empty() || _zones.count(zone); }

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

    template <class CostOf>
    bool CraftOption(Recipes::Recipe const& recipe, std::uint32_t at, CostOf const& cost, Option& option) const
    {
        if (recipe.effect != Recipes::Effect::Make && recipe.effect != Recipes::Effect::Enchant)
            return false;
        Learning const learning = LearnOf(recipe);
        if (!learning.ok || at < learning.skill)
            return false;
        int chance = CraftChancePerMille(at, recipe.grey, recipe.yellow);
        if (chance <= 0)
        {
            if (_world.fault != Fault::AllowGrey)
                return false;
            chance = PER_MILLE;
        }
        for (std::uint32_t const tool : recipe.tools)
            if (!_tools.count(tool))
                return false;
        for (std::uint32_t const category : recipe.toolCategories)
            if (!_categories.count(category))
                return false;
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
        double const perPoint = static_cast<double>(PER_MILLE) / chance;
        option.kind = StepKind::Craft;
        option.id = recipe.spell;
        option.chance = chance;
        option.materials = (materials - sold * _rates.factor) * perPoint;
        option.time = cast * _rates.minuteValue * perPoint;
        option.copper = (copper - sold) * perPoint;
        option.minutes = (minutes + cast) * perPoint;
        if (learning.step)
        {
            option.learn = recipe.spell;
            option.fee = learning.fee;
            // the price spread over the points the recipe serves, from where it is learned to grey
            double const served = std::max<double>(recipe.grey > learning.skill ? recipe.grey - learning.skill : 1, 1);
            option.share = learning.fee * static_cast<double>(_rates.factor) / served;
            option.copper += learning.fee / served;
        }
        option.value = option.materials + option.time + option.share;
        return true;
    }

    void GatherOptions(std::uint16_t skill, std::uint32_t at, std::vector<Option>& options) const
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
            double const perPoint = static_cast<double>(PER_MILLE) / chance;
            Option option;
            option.kind = kind;
            option.id = id;
            option.zone = zone;
            option.chance = chance;
            option.time = minutes * minuteCost * perPoint;
            option.materials = -sold * _rates.factor * perPoint;
            option.copper = -sold * perPoint;
            option.minutes = minutes * perPoint;
            option.value = option.materials + option.time;
            options.push_back(option);
        };
        if (auto const found = _nodesOf.find(skill); found != _nodesOf.end())
            for (NodeKind const& kind : found->second)
                if (Plays(kind.zone) && at >= kind.reqSkill)
                    add(StepKind::Gather, kind.entry, kind.zone, GatherChancePerMille(at, kind.reqSkill),
                        kind.spacing / (std::max(_rates.runSpeed, 1.0f) * Planner::Goals::SECONDS_PER_MINUTE) +
                            Planner::Goals::GATHER_MINUTES,
                        kind.sellCopper);
        if (skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Skinning))
            for (Skinnable const& creature : _world.skinnables)
                if (Plays(creature.zone) && at >= SkinRequirement(creature.level))
                    add(StepKind::Skin, creature.level, creature.zone,
                        GatherChancePerMille(at, SkinRequirement(creature.level)), creature.minutesPerKill,
                        creature.copperPerSkin);
        if (skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Fishing))
            for (Water const& water : _world.waters)
                if (Plays(water.zone))
                    add(StepKind::Fish, 0, water.zone, FishChancePerMille(at), water.minutesPerCast, 0.0);
    }

    // The lower value wins; equal values go to the lower kind, id and zone (whatever the order of the lists)
    [[nodiscard]] bool Better(Option const& a, Option const& b) const
    {
        if (a.value != b.value || _world.fault == Fault::OrderTie)
            return a.value < b.value;
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
