/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SKILLPATHTYPES_H
#define _PLAYERBOT_SKILLPATHTYPES_H

#include <cstdint>
#include <vector>

// The plain result of the skill path (professions plan WR10, PLAN 3.8): the stretches a bot weighed for its
// professions, the one it takes, and what follows from it (recipes and ranks to buy, materials wanted, crafts for
// the next stop). State only, so a bot's goals can hold it; the rules are in SkillPathRules.h. Every value is in
// the planner's value units (XP-equivalent), every copper field in copper, every minutes field in minutes.
namespace SkillPath
{
constexpr std::uint32_t PATH_VERSION = 1;
constexpr int NONE = -1;

enum class StepKind : std::uint8_t
{
    Craft,   // casts of a recipe for skill points
    Learn,   // a recipe bought from a trainer or as a recipe item
    Rank,    // the next rank bought from a trainer
    Gather,  // mining or herbalism nodes for skill points
    Skin,    // creatures skinned for skill points
    Fish,    // casts of the fishing pole for skill points
    Tool     // a tool the recipes ask for
};

enum class Milestone : std::uint8_t
{
    Recipe,      // a recipe that becomes makeable
    HeldRecipe,  // a recipe item the bot holds that it cannot learn yet
    Node,        // a node kind that becomes usable in a zone the bot plays in
    Skinnable    // creatures of a level that become skinnable in a zone the bot plays in
};

// Why a stretch cannot be taken, or why none is chosen
enum class Reason : std::uint8_t
{
    Open,        // the stretch can be taken
    Chosen,      // the stretch taken
    NotWorthIt,  // its net worth is not above zero
    NoSource,    // a skill point has no recipe or node with all its materials
    Locked,      // a rank or the era's cap stops it (MayLearn)
    Level,       // a rank it needs asks for a higher level
    GoldShort,   // it costs more copper than the bot holds
    Bound,       // more points than the bound
    NoStretch    // no milestone above the skill
};

// What a stretch brings, in value units, weighted by nearness (the four parts of WR.5)
struct Brings
{
    double useSoon{0.0};      // products the bot uses now or within its look-ahead
    double recipesHeld{0.0};  // recipe items held that the stretch makes learnable
    double access{0.0};       // nodes and creatures in its zones that become usable, at what they fetch
    double income{0.0};       // products it could sell, at what they fetch over their materials

    [[nodiscard]] double Total() const { return useSoon + recipesHeld + access + income; }
};

// What a stretch costs, in value units
struct Costs
{
    double materials{0.0};  // materials at their cost (MaterialRules.h), less what the products fetch
    double time{0.0};       // casts, gathering and walking at what a minute is worth to the bot
    double fees{0.0};       // a recipe's price by its share of the points it serves, ranks in full

    [[nodiscard]] double Total() const { return materials + time + fees; }
};

// One run of the path: consecutive skill points from the same source, or one purchase
struct PathStep
{
    StepKind kind{StepKind::Craft};
    std::uint32_t id{0};    // the recipe or rank spell, the node entry, the creature level, 0 for fishing
    std::uint32_t from{0};  // skill at the first point
    std::uint32_t to{0};    // skill after the last point (from for a purchase)
    double casts{0.0};      // expected casts, nodes or creatures
    double copper{0.0};     // copper paid, less what the products fetch (a purchase: its full price)
    double minutes{0.0};    // minutes spent
    double value{0.0};      // the step's cost in value units
    std::uint32_t zone{0};  // where a gathering step gathers
};

struct Stretch
{
    std::uint16_t skill{0};
    std::uint32_t from{0};
    std::uint32_t to{0};
    Milestone milestone{Milestone::Recipe};
    std::uint32_t milestoneId{0};  // the recipe spell, the node entry or the creature level reached
    Brings brings;
    Costs cost;
    double copper{0.0};  // copper the stretch spends, less what its products fetch; fees as in cost.fees
    double minutes{0.0};
    double nearness{0.0};   // the weight of the milestone's skill against level x 5
    double netWorth{0.0};   // brings - cost, value units; 0 when the climb is blocked
    double perMinute{0.0};  // netWorth over minutes, the rank among the open stretches
    Reason reason{Reason::Open};
    std::uint32_t blockedAt{0};  // the skill a blocked stretch stops at
    std::vector<PathStep> steps;
};

// A recipe or rank to buy
struct Purchase
{
    StepKind kind{StepKind::Learn};
    std::uint32_t spell{0};
    std::uint32_t copper{0};
};

// A material the chosen stretch wants beyond what the bot holds for it
struct Want
{
    std::uint32_t item{0};
    std::uint32_t count{0};
    double worthEach{0.0};  // value units per unit (its cost by its cheapest source)
};

// A craft for the next stop, in the order of claims: uses, their intermediates, then skill
struct Craft
{
    std::uint32_t spell{0};
    std::uint32_t casts{0};
    bool forSkill{false};
};

struct Path
{
    std::uint32_t version{PATH_VERSION};
    std::vector<Stretch> weighed;
    int chosen{NONE};
    Reason reason{Reason::NoStretch};  // Chosen, or why none is
    std::vector<Purchase> needs;       // the chosen stretch's recipes and ranks at their full price
    std::vector<Want> wants;           // its materials beyond what the bags hold once the uses have claimed theirs
    std::vector<Craft> queue;          // the crafts the bags allow now
};

// A consumable item use wants (ItemUse::DemandAnswer's keeps and lines, mapped by WR11 on the bot's thread)
struct ConsumableWant
{
    std::uint32_t item{0};
    std::uint32_t wantedStock{0};  // units
    std::uint32_t held{0};         // usable units held
    float worthCopperEach{0.0f};
};

// A product the bot would use once: an upgrade, a bag, a tool (travel's worth), or an enchant on its own gear (spell)
struct UseWant
{
    std::uint32_t item{0};
    std::uint32_t spell{0};
    float worthCopperEach{0.0f};
    std::uint32_t level{0};  // the level it is used from
};

// What the bot's thread copies into a goals job for the path besides the snapshot (WR11): item use's answer is a
// map-thread cache, so the worker never reads it
struct PathInputs
{
    std::vector<ConsumableWant> consumables;
    std::vector<UseWant> uses;
};
}  // namespace SkillPath

#endif
