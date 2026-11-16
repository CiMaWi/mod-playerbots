/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGENEEDSTOPIC_H
#define PLAYERBOTS_BRIDGENEEDSTOPIC_H

// WU23: the `needs` topic (BridgeBotState.h), Player-free. BridgeBotState.cpp computes the bot's NeedList on the world
// thread (Planner::BotNeedsNow, the code `playerbots planner needs` runs) and this header words it: what the bot must
// pay for, what it keeps back and what it is short for. Only BridgeBotState.cpp and the selftest include it, so the
// rule header stays out of the central headers. tests/bridge-bot-topics-selftest.cpp checks it.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "BridgeBotState.h"
#include "UpkeepNeedRules.h"

namespace PlayerbotBridge::Topics
{
// The row kinds (BotStateItem::code) of the needs topic
enum class NeedRow : std::uint8_t
{
    Provided,  // a bot the upkeep rules do not govern: its gear and gold are provided
    Nothing,   // nothing to pay for
    Pending,   // the upkeep index is not ready yet
    Due,       // a need worth its own trip: name = kind word, value = price, extra = what the bot's gold pays
    Passing,   // a need the bot pays when a trip passes: the same fields
    Reserve,   // value = the gold kept back, extra = the gold the bot holds
    Saving     // value = the shortfall: the first need its gold does not cover
};

// The most entries: the needs of the fixed order are cut so that the reserve and the saving entry stay
constexpr std::size_t NEEDS_SHOWN = 8;

struct NeedsInput
{
    bool governed{false};  // a persistent bot (BotKind::KeepsPossessions); else its needs are provided
    bool ready{true};      // the upkeep index is built and the list below is filled
    std::uint8_t cls{0};   // the class id, for "the warrior trainer"
    UpkeepNeeds::NeedList const* list{nullptr};
    std::function<std::string(std::uint32_t)> itemName;   // an item's name; empty for an unknown id
    std::function<std::string(std::uint32_t)> spellName;  // a spell's name; empty for an unknown id
};

inline std::uint32_t Clamp32(std::uint64_t value)
{
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

inline char const* NeedClassName(std::uint8_t cls)
{
    switch (cls)
    {
        case 1:
            return "warrior";
        case 2:
            return "paladin";
        case 3:
            return "hunter";
        case 4:
            return "rogue";
        case 5:
            return "priest";
        case 6:
            return "death knight";
        case 7:
            return "shaman";
        case 8:
            return "mage";
        case 9:
            return "warlock";
        case 11:
            return "druid";
        default:
            return "class";
    }
}

// What the need buys, by name: "repairs", an item's name, "3 spells at the warrior trainer", a rank's name
inline std::string NeedWhat(NeedsInput const& in, UpkeepNeeds::Need const& need)
{
    using UpkeepNeeds::NeedOrder;
    switch (need.order)
    {
        case NeedOrder::Repair:
            return "repairs";
        case NeedOrder::Training:
            return std::to_string(need.units) + (need.units == 1 ? " spell" : " spells") + " at the " +
                   NeedClassName(in.cls) + " trainer";
        case NeedOrder::Riding:
            for (UpkeepNeeds::RidingRank const& rank : UpkeepNeeds::RIDING_RANKS)
                if (rank.spellId == need.item)
                    return rank.name;
            return in.spellName ? in.spellName(need.item) : std::string();
        case NeedOrder::Pet:
            return need.swap ? "a stable slot for the old pet" : "a new pet";
        default:
        {
            std::string const name = in.itemName ? in.itemName(need.item) : std::string();
            return name.empty() ? UpkeepNeeds::OrderWord(need.order) : name;
        }
    }
}

inline std::string NeedLine(NeedsInput const& in, UpkeepNeeds::Need const& need)
{
    std::string what = NeedWhat(in, need);
    if (what.empty())
        what = UpkeepNeeds::OrderWord(need.order);
    if (need.copper == 0)  // a pet to tame, a free stable slot: no price to name
        return std::string(need.due ? "I must get " : "I will get ") + what + (need.due ? "" : " when I pass by") +
               ", at no cost; going without means " + need.without + ".";
    std::string const set = need.funded >= need.copper ? "the gold is set aside"
                            : need.funded > 0          ? "only " + UpkeepNeeds::Money(need.funded) + " is set aside"
                                                       : "none of it is set aside";
    return std::string(need.due ? "I must pay for " : "I will pay for ") + what + (need.due ? "" : " when I pass by") +
           ": " + UpkeepNeeds::Money(need.copper) + ", " + set + "; going without means " + need.without + ".";
}

// The first need the bot's gold does not cover, else the riding rank in the reserve that is not bought yet
inline std::string ShortFor(NeedsInput const& in, UpkeepNeeds::NeedList const& list)
{
    for (UpkeepNeeds::Need const& need : list.needs)
        if (need.funded < need.copper)
        {
            std::string const what = NeedWhat(in, need);
            return what.empty() ? UpkeepNeeds::OrderWord(need.order) : what;
        }
    for (UpkeepNeeds::RankValue const& value : list.ranks)
        if (value.reserved && !value.bought)
            return value.rank.name;
    return "what is coming";
}

// Provided; else nothing to pay for or the needs in §3.3's order (each due one first by its place in the list), then
// the reserve, then the saving entry only with a shortfall; at most NEEDS_SHOWN entries
inline void NeedsItems(NeedsInput const& in, std::vector<BotStateItem>& items)
{
    using UpkeepNeeds::Money;
    items.clear();
    auto const one =
        [&items](char const* name, NeedRow row, std::string text, std::uint64_t value = 0, std::uint64_t extra = 0)
    { items.push_back({name, Clamp32(value), Clamp32(extra), static_cast<std::uint8_t>(row), std::move(text)}); };
    if (!in.governed)
    {
        one("provided", NeedRow::Provided,
            "My gear and gold are provided for; I have nothing to pay for and nothing to save up for myself.");
        return;
    }
    if (!in.ready || !in.list)
    {
        one("pending", NeedRow::Pending, "I have not worked out what I need to pay for yet.");
        return;
    }
    UpkeepNeeds::NeedList const& list = *in.list;
    if (list.needs.empty())
    {
        one("nothing", NeedRow::Nothing, "I have nothing to pay for right now.");
        if (list.reserve == 0)
            return;
    }
    bool const saving = list.shortfall > 0;
    std::size_t const room = NEEDS_SHOWN - (list.reserve > 0 ? 1 : 0) - (saving ? 1 : 0);
    for (UpkeepNeeds::Need const& need : list.needs)
    {
        if (items.size() >= room)
            break;
        one(UpkeepNeeds::OrderWord(need.order), need.due ? NeedRow::Due : NeedRow::Passing, NeedLine(in, need),
            need.copper, need.funded);
    }
    if (list.reserve > 0)
        one("reserve", NeedRow::Reserve,
            "I keep " + Money(list.reserve) + " back for what is coming and hold " + Money(list.money) + ".",
            list.reserve, list.money);
    if (saving)
        one("saving", NeedRow::Saving,
            "I am saving: " + Money(list.shortfall) + " short for " + ShortFor(in, list) + ".", list.shortfall,
            list.money);
}
}  // namespace PlayerbotBridge::Topics

#endif
