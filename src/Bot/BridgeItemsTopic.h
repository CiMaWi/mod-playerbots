/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEITEMSTOPIC_H
#define PLAYERBOTS_BRIDGEITEMSTOPIC_H

// Item use WI10b: the `items` topic (BridgeBotState.h), Player-free. BridgeBotState.cpp gathers the bot's usable items
// by kind, its items on cooldown, its last uses (the ring in ItemUse::BotItems) and what it lacks for its role (the
// demand answer) on the world thread; this header words and bounds them. tests/bridge-bot-topics-selftest.cpp checks
// it.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BridgeBotState.h"
#include "BridgeBotTopics.h"

namespace PlayerbotBridge::Topics
{
// The row kinds (BotStateItem::code) of the items topic
enum class ItemsRow : std::uint8_t
{
    Carried,  // name = kind word, value = distinct items of it, extra = units; text lists the names
    Cooling,  // name = item name, value = seconds left
    Used,     // name = item name, value = seconds ago; text says how it went
    Lacks,    // name = slot word, value = units short
    Provided  // a bot that is not governed: its supplies are provided, it lacks nothing
};

// The most rows of each part, so that the part after it keeps its place (the sum is the topic's limit of twelve)
constexpr std::size_t ITEMS_CARRIED_SHOWN = 5;
constexpr std::size_t ITEMS_COOLING_SHOWN = 2;
constexpr std::size_t ITEMS_USED_SHOWN = 3;
constexpr std::size_t ITEMS_LACKS_SHOWN = 2;
constexpr std::size_t ITEMS_NAMES_SHOWN = 4;  // names listed in one kind's text

struct ItemsCarried
{
    std::string kind;  // the kind word; empty: skipped
    std::string name;
    std::uint32_t count{0};
};

struct ItemsCooling
{
    std::string name;
    std::uint32_t seconds{0};
};

struct ItemsUsed
{
    std::string name;
    std::string result;  // "used", "interrupted:<word>", "refused:<cause>"
    std::uint32_t secondsAgo{0};
};

struct ItemsLack
{
    std::string slot;  // the demand slot's word
    std::uint32_t missing{0};
};

struct ItemsInput
{
    bool governed{false};               // a persistent bot (BotKind::KeepsPossessions); else its supplies are provided
    std::vector<ItemsCarried> carried;  // in kind order
    std::vector<ItemsCooling> cooling;  // longest first
    std::vector<ItemsUsed> used;        // newest first
    std::vector<ItemsLack> lacks;       // in slot order
};

// An item's name without digits (C4: no number in a topic's text); an empty result stays empty
inline std::string ItemsName(std::string_view name)
{
    std::string out;
    for (char const c : name)
        if (c < '0' || c > '9')
            out.push_back(c);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    return out;
}

// A word of [a-z_] as plain words: "food_drink" -> "food drink"
inline std::string ItemsPlain(std::string_view word)
{
    std::string text = Words(word);
    std::replace(text.begin(), text.end(), '_', ' ');
    return text;
}

inline std::string ItemsKindText(std::string_view kind)
{
    std::string text = ItemsPlain(kind);
    return text.empty() ? std::string("other") : text;
}

// How a use went, as a clause: "it worked", "it was interrupted: <word>", "I could not: <cause>"
inline std::string ItemsResultText(std::string_view result)
{
    if (result == "used")
        return "it worked";
    if (result.substr(0, 12) == "interrupted:")
        return "it was interrupted: " + ItemsPlain(result.substr(12));
    if (result.substr(0, 8) == "refused:")
        return "I could not: " + ItemsPlain(result.substr(8));
    return ItemsPlain(result);
}

// Carried items by kind (one entry per kind, at most ITEMS_CARRIED_SHOWN), then the items on cooldown, the last uses,
// what the bot lacks; a bot that is not governed gets one provided entry for the last. At most twelve entries.
inline void ItemsItems(ItemsInput const& in, std::vector<BotStateItem>& items)
{
    items.clear();
    auto const one =
        [&items](std::string name, ItemsRow row, std::string text, std::uint32_t value = 0, std::uint32_t extra = 0)
    { items.push_back({std::move(name), value, extra, static_cast<std::uint8_t>(row), std::move(text)}); };

    std::size_t kinds = 0;
    for (std::size_t first = 0; first < in.carried.size() && kinds < ITEMS_CARRIED_SHOWN; ++first)
    {
        ItemsCarried const& head = in.carried[first];
        if (head.kind.empty())
            continue;
        bool seen = false;
        for (std::size_t before = 0; before < first && !seen; ++before)
            seen = in.carried[before].kind == head.kind;
        if (seen)
            continue;
        std::uint32_t distinct = 0;
        std::uint32_t units = 0;
        std::string names;
        for (std::size_t at = first; at < in.carried.size(); ++at)
        {
            ItemsCarried const& entry = in.carried[at];
            if (entry.kind != head.kind)
                continue;
            if (distinct < ITEMS_NAMES_SHOWN)
                names += (names.empty() ? "" : ", ") + ItemsName(entry.name);
            ++distinct;
            units += entry.count;
        }
        std::string const kind = ItemsKindText(head.kind);
        one(kind, ItemsRow::Carried,
            "I carry " + kind + " items: " + names + (distinct > ITEMS_NAMES_SHOWN ? " and more" : "") + ".", distinct,
            units);
        ++kinds;
    }
    for (std::size_t at = 0; at < in.cooling.size() && at < ITEMS_COOLING_SHOWN; ++at)
        one(ItemsName(in.cooling[at].name), ItemsRow::Cooling,
            "My " + ItemsName(in.cooling[at].name) + " is not ready again yet.", in.cooling[at].seconds);
    for (std::size_t at = 0; at < in.used.size() && at < ITEMS_USED_SHOWN; ++at)
        one(ItemsName(in.used[at].name), ItemsRow::Used,
            "I tried my " + ItemsName(in.used[at].name) + " not long ago: " + ItemsResultText(in.used[at].result) + ".",
            in.used[at].secondsAgo);
    if (!in.governed)
    {
        one("provided", ItemsRow::Provided,
            "My supplies are provided for; I lack nothing for my role and keep no stock of my own.");
        return;
    }
    for (std::size_t at = 0; at < in.lacks.size() && at < ITEMS_LACKS_SHOWN; ++at)
        one(Word(in.lacks[at].slot), ItemsRow::Lacks,
            "I am short of " + ItemsPlain(in.lacks[at].slot) + " supplies for my role.", in.lacks[at].missing);
}
}  // namespace PlayerbotBridge::Topics

#endif
