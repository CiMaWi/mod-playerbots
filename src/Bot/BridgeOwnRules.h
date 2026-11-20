/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEOWNRULES_H
#define PLAYERBOTS_BRIDGEOWNRULES_H

// LC18c: what a bot can ride and what it can make, Player-free. `mounts` lists its riding as a rank word and the mounts
// of its spellbook; `recipes` answers from the crafts it has and the recipes it knows (LC18f, the facts' BotRecipe,
// BridgeBotFactsRules.h): a craft by its name, or a thing to make by the item's name. The readers in
// BridgeBotState.cpp turn the bot into the plain facts below on the world thread; only they and
// tests/bridge-own-selftest.cpp include this header.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BridgeBotFactsRules.h"
#include "BridgeBotState.h"
#include "BridgeKnowledgeRules.h"
#include "BridgeTexts.h"

namespace PlayerbotBridge::Own
{
// The riding skill (762) climbs in steps of 75: 75 apprentice, 150 journeyman, 225 expert, 300 artisan. Value 0: none.
constexpr std::uint32_t RIDING_STEP = 75;
constexpr std::size_t RIDING_RANKS = 4;

constexpr std::string_view RidingWord(std::uint32_t skillValue)
{
    std::size_t const rank = (skillValue - 1) / RIDING_STEP;
    return FactsRules::RANK_WORDS[rank < RIDING_RANKS ? rank : RIDING_RANKS - 1];
}

// The most lines of a mounts answer (the riding line and the mounts) and of a recipes answer (the topic rows' limits)
constexpr std::size_t MOUNTS_SHOWN = 6;
constexpr std::size_t RECIPES_SHOWN = 4;

inline BotStateItem OwnLine(char const* word, std::string name, std::string text)
{
    return {std::move(name), 0, 0, 0, std::move(text), word};
}

// The mounts answer: the riding line when the bot has learned to ride, then its mounts by name (one line each, the
// first of a name), at most MOUNTS_SHOWN lines; nothing at all: one line saying so
inline void MountItems(std::uint32_t ridingValue, std::vector<std::string> mounts, std::vector<BotStateItem>& items)
{
    namespace L = BridgeTexts::Lookups;
    items.clear();
    std::sort(mounts.begin(), mounts.end());
    mounts.erase(std::unique(mounts.begin(), mounts.end()), mounts.end());
    if (ridingValue)
        items.push_back(OwnLine(L::ROW_OWN_RIDING, L::OWN_RIDING_NAME,
                                std::string(L::OWN_RIDING_NAME)
                                    .append(L::LOOKUP_SEP)
                                    .append(RidingWord(ridingValue))
                                    .append(L::LOOKUP_END)));
    for (std::string const& mount : mounts)
        if (!mount.empty() && items.size() < MOUNTS_SHOWN)
            items.push_back(OwnLine(L::ROW_OWN_MOUNT, mount, mount + L::OWN_MOUNT_LINE));
    if (mounts.empty())
        items.push_back(OwnLine(L::ROW_OWN_NONE, L::ROW_OWN_NONE, ridingValue ? L::OWN_NO_MOUNT : L::OWN_NO_RIDING));
}

// A craft as the reader found it: its name and the bot's rank word (empty: the bot lacks it)
struct CraftFact
{
    std::string name;
    std::string rank;
};

// Whether a recipe's item answers the name asked (as NameKey): the same name, or a name that holds the asked one
// ("bandage" gives Linen Bandage). Not the other way round: a shorter item is not the thing asked for ("Heavy Linen
// Bandage" is not answered by Linen Bandage, which the bot may know without the heavy one) (LC18f repair).
inline bool RecipeFits(std::string const& key, std::string_view itemName)
{
    std::string const other = NameKey(itemName);
    return !key.empty() && (other == key || (key.size() >= MinPartialName && other.find(key) != std::string::npos));
}

// The recipes answer for the NameKey asked, from the crafts the bot has and the recipes it knows: no name lists its
// crafts (row craft, with the rank) and then its recipes; a craft's name gives that craft and its recipes; any other
// name gives the recipes whose item fits it (RecipeFits; row recipe, the item and the craft that makes it). Recipes
// come highest skill first, an item once. Nothing: one line saying so. At most RECIPES_SHOWN lines.
inline void RecipeItems(std::string const& key, std::vector<CraftFact> const& crafts, std::vector<BotRecipe> recipes,
                        std::vector<BotStateItem>& items)
{
    namespace L = BridgeTexts::Lookups;
    items.clear();
    FactsRules::BestFirst(recipes);
    auto const room = [&items] { return items.size() < RECIPES_SHOWN; };
    bool const all = key.empty();
    bool asked = false;
    std::vector<std::string_view> named;
    for (CraftFact const& craft : crafts)
    {
        if (!all && !Knowledge::NameFits(key, craft.name))
            continue;
        asked = true;
        if (craft.rank.empty())
            continue;
        named.push_back(craft.name);
        if (room())
            items.push_back(
                OwnLine(L::ROW_OWN_CRAFT, craft.name, craft.name + L::LOOKUP_SEP + craft.rank + L::LOOKUP_END));
    }
    std::vector<std::string> seen;
    for (BotRecipe const& recipe : recipes)
    {
        bool const wanted = asked ? std::find(named.begin(), named.end(), recipe.craft) != named.end()
                                  : RecipeFits(key, recipe.itemName);
        if (!wanted || !room() || std::find(seen.begin(), seen.end(), recipe.itemName) != seen.end())
            continue;
        seen.push_back(recipe.itemName);
        items.push_back(OwnLine(L::ROW_OWN_RECIPE, recipe.itemName,
                                recipe.itemName + L::LOOKUP_SEP + L::OWN_MADE_WITH + recipe.craft + L::LOOKUP_END));
    }
    if (items.empty())
        items.push_back(OwnLine(L::ROW_OWN_NONE, L::ROW_OWN_NONE, L::OWN_NO_RECIPE));
}
}  // namespace PlayerbotBridge::Own

#endif
