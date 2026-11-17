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
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "BridgeBotState.h"
#include "BridgeTexts.h"
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
    char const* const word =
        cls < std::size(BridgeTexts::Facts::CLASS_WORDS) ? BridgeTexts::Facts::CLASS_WORDS[cls] : "";
    return *word ? word : BridgeTexts::Lookups::NEEDS_ANY_CLASS;
}

// What the need buys, by name: "repairs", an item's name, "3 spells at the warrior trainer", a rank's name
inline std::string NeedWhat(NeedsInput const& in, UpkeepNeeds::Need const& need)
{
    using UpkeepNeeds::NeedOrder;
    namespace Lookups = BridgeTexts::Lookups;
    switch (need.order)
    {
        case NeedOrder::Repair:
            return Lookups::NEEDS_REPAIRS;
        case NeedOrder::Training:
            return std::to_string(need.units) + (need.units == 1 ? Lookups::NEEDS_ONE_SPELL : Lookups::NEEDS_SPELLS) +
                   Lookups::NEEDS_AT_THE + NeedClassName(in.cls) + Lookups::NEEDS_TRAINER;
        case NeedOrder::Riding:
            for (UpkeepNeeds::RidingRank const& rank : UpkeepNeeds::RIDING_RANKS)
                if (rank.spellId == need.item)
                    return rank.name;
            return in.spellName ? in.spellName(need.item) : std::string();
        case NeedOrder::Pet:
            return need.swap ? Lookups::NEEDS_OLD_PET_SLOT : Lookups::NEEDS_NEW_PET;
        default:
        {
            std::string const name = in.itemName ? in.itemName(need.item) : std::string();
            return name.empty() ? UpkeepNeeds::OrderWord(need.order) : name;
        }
    }
}

inline std::string NeedLine(NeedsInput const& in, UpkeepNeeds::Need const& need)
{
    namespace Lookups = BridgeTexts::Lookups;
    std::string what = NeedWhat(in, need);
    if (what.empty())
        what = UpkeepNeeds::OrderWord(need.order);
    if (need.copper == 0)  // a pet to tame, a free stable slot: no price to name
        return std::string(need.due ? Lookups::NEEDS_MUST_GET : Lookups::NEEDS_WILL_GET) + what +
               (need.due ? "" : Lookups::NEEDS_WHEN_PASSING) + Lookups::NEEDS_NO_COST + need.without + ".";
    std::string const set =
        need.funded >= need.copper ? std::string(Lookups::NEEDS_ALL_SET_ASIDE)
        : need.funded > 0 ? Lookups::NEEDS_PART_OPENS + UpkeepNeeds::Money(need.funded) + Lookups::NEEDS_PART_SET_ASIDE
                          : std::string(Lookups::NEEDS_NONE_SET_ASIDE);
    return std::string(need.due ? Lookups::NEEDS_MUST_PAY : Lookups::NEEDS_WILL_PAY) + what +
           (need.due ? "" : Lookups::NEEDS_WHEN_PASSING) + ": " + UpkeepNeeds::Money(need.copper) + ", " + set +
           Lookups::NEEDS_GOING_WITHOUT + need.without + ".";
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
    return BridgeTexts::Lookups::NEEDS_WHAT_IS_COMING;
}

// Provided; else nothing to pay for or the needs in §3.3's order (each due one first by its place in the list), then
// the reserve, then the saving entry only with a shortfall; at most NEEDS_SHOWN entries
inline void NeedsItems(NeedsInput const& in, std::vector<BotStateItem>& items)
{
    using UpkeepNeeds::Money;
    namespace Lookups = BridgeTexts::Lookups;
    items.clear();
    auto const one =
        [&items](char const* name, NeedRow row, std::string text, std::uint64_t value = 0, std::uint64_t extra = 0)
    { items.push_back({name, Clamp32(value), Clamp32(extra), static_cast<std::uint8_t>(row), std::move(text)}); };
    if (!in.governed)
    {
        one(Lookups::NEEDS_ROW_PROVIDED, NeedRow::Provided, Lookups::NEEDS_PROVIDED);
        return;
    }
    if (!in.ready || !in.list)
    {
        one(Lookups::NEEDS_ROW_PENDING, NeedRow::Pending, Lookups::NEEDS_PENDING);
        return;
    }
    UpkeepNeeds::NeedList const& list = *in.list;
    if (list.needs.empty())
    {
        one(Lookups::NEEDS_ROW_NOTHING, NeedRow::Nothing, Lookups::NEEDS_NOTHING);
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
        one(Lookups::NEEDS_ROW_RESERVE, NeedRow::Reserve,
            Lookups::NEEDS_RESERVE_KEEP + Money(list.reserve) + Lookups::NEEDS_RESERVE_HOLD + Money(list.money) + ".",
            list.reserve, list.money);
    if (saving)
        one(Lookups::NEEDS_ROW_SAVING, NeedRow::Saving,
            Lookups::NEEDS_SAVING + Money(list.shortfall) + Lookups::NEEDS_SHORT_FOR + ShortFor(in, list) + ".",
            list.shortfall, list.money);
}
}  // namespace PlayerbotBridge::Topics

#endif
