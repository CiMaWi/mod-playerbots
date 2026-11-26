/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SKILLPATHTYPES_H
#define _PLAYERBOT_SKILLPATHTYPES_H

#include <algorithm>
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
    bool operator==(Brings const&) const = default;
};

// What a stretch costs, in value units
struct Costs
{
    double materials{0.0};  // materials at their cost (MaterialRules.h), less what the products fetch
    double time{0.0};       // casts, gathering and walking at what a minute is worth to the bot
    double fees{0.0};       // a recipe's price by its share of the points it serves, ranks in full

    [[nodiscard]] double Total() const { return materials + time + fees; }
    bool operator==(Costs const&) const = default;
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
    bool operator==(PathStep const&) const = default;
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
    bool operator==(Stretch const&) const = default;
};

// A recipe or rank to buy
struct Purchase
{
    StepKind kind{StepKind::Learn};
    std::uint32_t spell{0};
    std::uint32_t copper{0};
    std::uint32_t item{0};  // a recipe the path buys from a vendor (WR14b): its recipe item; 0: a trainer's, or none
    bool operator==(Purchase const&) const = default;
};

// A material the chosen stretch wants beyond what the bot holds for it
struct Want
{
    std::uint32_t item{0};
    std::uint32_t count{0};
    double worthEach{0.0};  // value units per unit (its cost by its cheapest source)
    bool operator==(Want const&) const = default;
};

// A material the path uses (WR12): what the chosen stretch's casts and tools use, expanded like the wants (the ore it
// smelts, not only the bars), held or not, and what the uses' crafts claim from the bags. The bot keeps it instead of
// selling it, and banks it until its skill reaches the step that uses it
struct Material
{
    std::uint32_t item{0};
    std::uint32_t used{0};        // units the path uses, held or not
    std::uint32_t wanted{0};      // of them, the units beyond what the bot holds (its Want)
    std::uint32_t copperEach{0};  // one unit's cost by its cheapest source, in copper
    std::uint16_t skill{0};       // the profession of the first step that uses it; 0: a use's craft
    std::uint16_t dueSkill{0};    // the skill that step starts at; 0: used now
    std::uint8_t source{0};       // that source's Materials::Kind (0: blocked)
    std::uint32_t via{0};         // its Materials::Cost::via: the item bought, the zone, the recipe or processed item
    // Auction house WA7b: a unit's cost by the cheapest source but the auction, in copper, when the auction is the
    // cheapest (the Path buy line's ceiling); 0: another source is the cheapest, or the auction is the only one
    std::uint32_t otherCopperEach{0};
    bool operator==(Material const&) const = default;
};

// A craft for the next stop, in the order of claims: uses, their intermediates, skill, then sale
struct Craft
{
    std::uint32_t spell{0};
    std::uint32_t casts{0};
    bool forSkill{false};
    std::uint32_t source{0};  // a prospect or mill cast's item (spell: the processing spell); 0 = a recipe
    bool forSale{false};      // auction house WA7b: products the bot sells, from what nothing else claims
    bool operator==(Craft const&) const = default;
};

// The second main profession when it is due (WR11, PLAN 3.2, 3.3): the person's craft once a stretch of it from its
// first point is worth it and paid for; for a bot short of gold, a second gatherer in its place when that brings more
struct SecondChoice
{
    std::uint16_t craft{0};            // the person's craft in its unlearned second slot; 0: no choice is due
    std::uint16_t gatherer{0};         // the best second gatherer weighed (a bot short of gold only); 0: none
    std::uint16_t take{0};             // the craft, the gatherer, or 0: the craft's learn waits
    Reason reason{Reason::NoStretch};  // Chosen (the craft), GoldShort (the gatherer, or the craft waits), NotWorthIt
    double craftWorth{0.0};            // the best net worth of the craft's stretches from its first point
    double gathererWorth{0.0};
    Brings brings;           // what the taken profession's best stretch brings
    std::uint64_t money{0};  // copper above the reserve before the professions
    bool operator==(SecondChoice const&) const = default;
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
    SecondChoice second;
    std::vector<Material> materials;  // by item: every material the path uses (WR12)
    // What the queue's crafts for the bot's own use bring (WR15, supply before skill; also without a chosen stretch):
    // their worth (value units: the uses' worth of the units they make) and the units of upkeep's food they make
    double useWorth{0.0};
    std::uint32_t foodUnits{0};
    bool operator==(Path const&) const = default;
};

// The path's material of an item, or null: a binary search over the few materials (the item usage's lookup)
inline Material const* MaterialOf(Path const& path, std::uint32_t item)
{
    auto const found = std::lower_bound(path.materials.begin(), path.materials.end(), item,
                                        [](Material const& material, std::uint32_t id) { return material.item < id; });
    return found != path.materials.end() && found->item == item ? &*found : nullptr;
}

// The queue's net worth (WR14), value units: what its crafts for the bot's own use bring (WR15) plus the chosen
// stretch's net worth by the share of its craft casts the queue's skill crafts make now (at most all of it; a
// processing cast is no craft cast); 0 without a queue. A craft for sale adds nothing: it rides on a stop made for the
// others (WA7b, the last claim)
inline double QueueWorth(Path const& path)
{
    if (std::all_of(path.queue.begin(), path.queue.end(), [](Craft const& craft) { return craft.forSale; }))
        return 0.0;
    double const uses = path.useWorth > 0.0 ? path.useWorth : 0.0;
    if (path.chosen < 0 || path.chosen >= static_cast<int>(path.weighed.size()))
        return uses;
    Stretch const& taken = path.weighed[path.chosen];
    double planned = 0.0;
    for (PathStep const& step : taken.steps)
        if (step.kind == StepKind::Craft && step.casts > 0.0)
            planned += step.casts;
    double queued = 0.0;
    for (Craft const& craft : path.queue)
        if (!craft.source && craft.forSkill)
            queued += craft.casts;
    if (!(taken.netWorth > 0.0) || !(planned > 0.0))
        return uses;
    return uses + taken.netWorth * std::min(1.0, queued / planned);
}

// The queue's craft of a recipe's spell is a craft for sale (WA7b: the craft row's reason); a sale craft's spell is
// never queued for another claim too
inline bool ForSale(Path const& path, std::uint32_t spell)
{
    for (Craft const& craft : path.queue)
        if (craft.spell == spell && !craft.source)
            return craft.forSale;
    return false;
}

// A consumable item use wants (ItemUse::DemandAnswer's keeps and lines, mapped by WR11 on the bot's thread; its makes
// by WR15)
struct ConsumableWant
{
    std::uint32_t item{0};
    std::uint32_t wantedStock{0};  // units
    std::uint32_t held{0};         // usable units held
    float worthCopperEach{0.0f};
    // WR15: the wants of one demand slot the bot makes share its stock (slot + 1; 0: the want's own); food: the slot
    // is upkeep's food, so what the claims make counts as its food stock (Path::foodUnits)
    std::uint32_t group{0};
    bool food{false};
    bool operator==(ConsumableWant const&) const = default;
};

// A product the bot would use once: an upgrade, a bag, a tool (travel's worth), or an enchant on its own gear (spell)
struct UseWant
{
    std::uint32_t item{0};
    std::uint32_t spell{0};
    float worthCopperEach{0.0f};
    std::uint32_t level{0};  // the level it is used from
    bool operator==(UseWant const&) const = default;
};

// What the bot's thread copies into a goals job for the path besides the snapshot (WR11): item use's answer is a
// map-thread cache, so the worker never reads it
struct PathInputs
{
    std::vector<ConsumableWant> consumables;
    std::vector<UseWant> uses;
    bool anyStretch{false};  // a test's constraint anypath (TestConstraints.h): PathBot::anyStretch
};
}  // namespace SkillPath

#endif
