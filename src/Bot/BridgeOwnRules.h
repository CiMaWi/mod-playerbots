/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEOWNRULES_H
#define PLAYERBOTS_BRIDGEOWNRULES_H

// LC18c: what a bot can ride and what it can make, Player-free. `mounts` lists its riding as a rank word and the mounts
// of its spellbook; `recipes` answers from the crafts it has (the facts' words, BridgeBotFactsRules.h): a craft by its
// name, or a thing to make by the words of what a craft is good for. Real recipes are LC18f's. The readers in
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

// A craft as the reader found it: its name, what it is good for, and the bot's rank word (empty: the bot lacks it)
struct CraftFact
{
    std::string name;
    std::string useful;
    std::string rank;
};

// Whether a word of a "good for" phrase is what the key (a NameKey) asks for: the same word, or one that holds the
// other from MinPartialName letters on (potion, potions); the small joining words never count
inline bool UsefulHolds(std::string const& key, std::string_view useful)
{
    auto const stop = [](std::string_view word)
    {
        auto const& words = BridgeTexts::Lookups::USEFUL_STOP_WORDS;
        return std::find(words.begin(), words.end(), word) != words.end();
    };
    if (key.size() < MinPartialName || stop(key))
        return false;
    std::string word;
    auto const fits = [&key, &stop](std::string const& found)
    {
        return found.size() >= MinPartialName && !stop(found) &&
               (found == key || found.find(key) != std::string::npos || key.find(found) != std::string::npos);
    };
    for (char letter : useful)
    {
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter + ('a' - 'A'));
        if (letter >= 'a' && letter <= 'z')
        {
            word.push_back(letter);
            continue;
        }
        if (fits(word))
            return true;
        word.clear();
    }
    return fits(word);
}

// The recipes answer for the NameKey asked: the crafts the bot has whose name fits (row craft); else those it has that
// are good for the thing named (row recipe); an empty key lists its crafts; nothing: one line saying so
inline void RecipeItems(std::string const& key, std::vector<CraftFact> const& crafts, std::vector<BotStateItem>& items)
{
    namespace L = BridgeTexts::Lookups;
    items.clear();
    auto const line = [&items](char const* word, CraftFact const& craft)
    {
        if (items.size() < RECIPES_SHOWN)
            items.push_back(OwnLine(word, craft.name,
                                    craft.name + L::LOOKUP_SEP + craft.rank + L::LOOKUP_SEMI + L::OWN_GOOD_FOR +
                                        craft.useful + L::LOOKUP_END));
    };
    bool named = key.empty();
    for (CraftFact const& craft : crafts)
        if (key.empty() || Knowledge::NameFits(key, craft.name))
        {
            named = true;
            if (!craft.rank.empty())
                line(L::ROW_OWN_CRAFT, craft);
        }
    if (!named)
        for (CraftFact const& craft : crafts)
            if (!craft.rank.empty() && UsefulHolds(key, craft.useful))
                line(L::ROW_OWN_RECIPE, craft);
    if (items.empty())
        items.push_back(OwnLine(L::ROW_OWN_NONE, L::ROW_OWN_NONE, L::OWN_NO_RECIPE));
}
}  // namespace PlayerbotBridge::Own

#endif
