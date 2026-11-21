/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CRAFTRULES_H
#define _PLAYERBOT_CRAFTRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>
#include <vector>

// Whether a recipe can be cast here and now, how many casts the bags allow and the order of a queue (professions plan
// WR6, PLAN 3.10). Pure and free of core types, so tests/craft-rules-selftest.cpp runs the code the server runs.
// The shape (a recipe with reagents, tools and a spell focus; the inventory reserved across recipes so a later one
// cannot use what an earlier one takes; a product entering the copy so a later recipe can use it) is adapted from
// Solfood/mod-playerbots CraftPlanRules.h at e8c3eb42, GPL v2 (WR.10, C7). Their want classes, batch limit, pair
// weights and auction assumptions are their realm's policy and are not taken: what to make is the planner's (WR14).
//
// The core's own checks (Spell::CheckSpellFocus, Spell::CheckItems) are the truth; these rules answer the same
// questions ahead of the cast so a visit never starts a cast the core would refuse.
//
// Invariant (searched in the selftest): every cast of a queue that Order returns passes Check when run in order on
// a copy of the bags (reagents out, product in), and CastsAllowed never exceeds what the bags hold.
namespace CraftRules
{
// Spell focus ids of SpellFocusObject.dbc that a profession recipe asks for (the recipe index dump's focus column)
namespace Focus
{
constexpr std::uint32_t ANVIL = 1;
constexpr std::uint32_t FORGE = 3;
constexpr std::uint32_t COOKING_FIRE = 4;
constexpr std::array<std::uint32_t, 10> STATIONS = {1, 3, 4, 543, 623, 663, 883, 1362, 1423, 1613};
}  // namespace Focus

// A station the bot lights where it stands (professions WR15, PLAN 3.11): Basic Campfire (818) summons a cooking fire
// (gameobject 29784, focus 4, 10 yd) with no tool and no reagent, a cooking skill spell; 0: none for the focus
constexpr std::uint32_t BASIC_CAMPFIRE = 818;
constexpr std::uint32_t OwnStationSpell(std::uint32_t focus)
{
    return focus == Focus::COOKING_FIRE ? BASIC_CAMPFIRE : 0;
}

inline bool IsStationFocus(std::uint32_t focus)
{
    return std::find(Focus::STATIONS.begin(), Focus::STATIONS.end(), focus) != Focus::STATIONS.end();
}

inline char const* FocusName(std::uint32_t focus)
{
    switch (focus)
    {
        case Focus::ANVIL:
            return "anvil";
        case Focus::FORGE:
            return "forge";
        case Focus::COOKING_FIRE:
            return "cooking fire";
        default:
            return "other";
    }
}

// The core's test: the caster is within the object's own range (IsWithinDistInMap)
inline bool InStationReach(float distance, float range) { return distance <= range; }

// No cast chain is longer than this (a bound for the simulation, far above any bag's reagent stacks)
constexpr std::uint32_t CAST_CEILING = 1000;

struct Reagent
{
    std::uint32_t item{0};
    std::uint32_t count{0};
};

struct RecipeView
{
    std::uint32_t spell{0};
    std::uint32_t skillNeeded{0};  // the recipe's orange value: the skill it is first castable at
    std::uint32_t product{0};      // 0 = no item made (an enchant, a process)
    std::uint32_t productCount{0};
    std::uint32_t focus{0};  // spell focus id, 0 = none
    std::vector<Reagent> reagents;
    std::vector<std::uint32_t> tools;           // item ids that must be in the bags
    std::vector<std::uint32_t> toolCategories;  // totem category ids that must be held
};

struct BagState
{
    std::map<std::uint32_t, std::uint32_t> items;     // item id -> count
    std::map<std::uint32_t, std::uint32_t> maxStack;  // item id -> stack size, absent = 1
    std::uint32_t capacity{0};                        // bag slots in all
    std::uint32_t skill{0};                           // the skill value of the recipe's profession
    std::set<std::uint32_t> categoriesHeld;           // totem categories a held tool covers
    std::set<std::uint32_t> focusInReach;             // spell focus ids with a spawned station in reach

    [[nodiscard]] std::uint32_t StackOf(std::uint32_t item) const
    {
        auto const it = maxStack.find(item);
        return it == maxStack.end() || !it->second ? 1u : it->second;
    }
    [[nodiscard]] std::uint32_t Held(std::uint32_t item) const
    {
        auto const it = items.find(item);
        return it == items.end() ? 0u : it->second;
    }
    [[nodiscard]] std::uint32_t SlotsUsed() const
    {
        std::uint32_t used = 0;
        for (auto const& [item, count] : items)
            used += (count + StackOf(item) - 1) / StackOf(item);
        return used;
    }
    [[nodiscard]] std::uint32_t FreeSlots() const
    {
        std::uint32_t const used = SlotsUsed();
        return used >= capacity ? 0u : capacity - used;
    }
    // Room for count more of an item: its partly filled stack, then the free slots
    [[nodiscard]] std::uint32_t RoomFor(std::uint32_t item) const
    {
        std::uint32_t const stack = StackOf(item);
        std::uint32_t const partial = (stack - Held(item) % stack) % stack;
        return partial + FreeSlots() * stack;
    }
};

enum class Verdict : std::uint8_t
{
    Castable,
    NoSkill,
    NoMaterials,  // a reagent is short
    NoTool,
    BagsFull,  // no room for the product
    NoStation
};

inline std::string_view VerdictWord(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::Castable:
            return "castable";
        case Verdict::NoSkill:
            return "no_skill";
        case Verdict::NoMaterials:
            return "no_materials";
        case Verdict::NoTool:
            return "no_tool";
        case Verdict::BagsFull:
            return "bags_full";
        case Verdict::NoStation:
            return "no_station";
    }
    return "castable";
}

// Cast, or the first cause against it (in the order of the visit's ends)
inline Verdict Check(RecipeView const& recipe, BagState const& bags)
{
    if (bags.skill < recipe.skillNeeded)
        return Verdict::NoSkill;
    for (Reagent const& reagent : recipe.reagents)
        if (bags.Held(reagent.item) < reagent.count)
            return Verdict::NoMaterials;
    for (std::uint32_t const tool : recipe.tools)
        if (!bags.Held(tool))
            return Verdict::NoTool;
    for (std::uint32_t const category : recipe.toolCategories)
        if (!bags.categoriesHeld.count(category))
            return Verdict::NoTool;
    // The reagents leave the bags first: a recipe that eats the stack its product joins has room
    BagState after = bags;
    for (Reagent const& reagent : recipe.reagents)
    {
        std::uint32_t& held = after.items[reagent.item];
        held -= reagent.count;
        if (!held)
            after.items.erase(reagent.item);
    }
    if (recipe.product && recipe.productCount && after.RoomFor(recipe.product) < recipe.productCount)
        return Verdict::BagsFull;
    if (recipe.focus && !bags.focusInReach.count(recipe.focus))
        return Verdict::NoStation;
    return Verdict::Castable;
}

// One cast applied: reagents out, product in
inline void Apply(RecipeView const& recipe, BagState& bags)
{
    for (Reagent const& reagent : recipe.reagents)
    {
        auto const it = bags.items.find(reagent.item);
        if (it == bags.items.end())
            continue;
        it->second = it->second > reagent.count ? it->second - reagent.count : 0u;
        if (!it->second)
            bags.items.erase(it);
    }
    if (recipe.product && recipe.productCount)
        bags.items[recipe.product] += recipe.productCount;
}

// How many casts in a row the bags allow: each one passes Check on the bags the earlier ones left
inline std::uint32_t CastsAllowed(RecipeView const& recipe, BagState const& bags)
{
    BagState copy = bags;
    std::uint32_t casts = 0;
    while (casts < CAST_CEILING && Check(recipe, copy) == Verdict::Castable)
    {
        Apply(recipe, copy);
        ++casts;
    }
    return casts;
}

struct QueueEntry
{
    RecipeView recipe;
    std::uint32_t rank{0};    // lower first (the caller's: smelts before the crafts that use the bars)
    std::uint32_t wanted{0};  // casts asked for, 0 = as many as the bags allow
};

struct Planned
{
    std::uint32_t spell{0};
    std::uint32_t casts{0};
};

// The queue in order (rank, then spell id; stable), each entry with the casts it can have on what the earlier ones
// left. An entry that gets none is dropped
inline std::vector<Planned> Order(std::vector<QueueEntry> queue, BagState const& bags)
{
    std::stable_sort(queue.begin(), queue.end(), [](QueueEntry const& a, QueueEntry const& b)
                     { return a.rank != b.rank ? a.rank < b.rank : a.recipe.spell < b.recipe.spell; });
    BagState reserved = bags;
    std::vector<Planned> plan;
    for (QueueEntry const& entry : queue)
    {
        std::uint32_t casts = CastsAllowed(entry.recipe, reserved);
        if (entry.wanted)
            casts = std::min(casts, entry.wanted);
        if (!casts)
            continue;
        for (std::uint32_t i = 0; i < casts; ++i)
            Apply(entry.recipe, reserved);
        plan.push_back({entry.recipe.spell, casts});
    }
    return plan;
}
}  // namespace CraftRules

#endif
