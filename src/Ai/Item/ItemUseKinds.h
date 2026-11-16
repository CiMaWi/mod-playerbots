/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSEKINDS_H
#define _PLAYERBOT_ITEMUSEKINDS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ItemUseTypes.h"

// The kinds of use effect and the classifier (INTERFACE.md 1.7). WI2 writes the enum, the words and a Classify that
// answers Unclassified; lane 2 (WIL0 on) writes the real one. A kind may be appended before Count, never renamed or
// reordered.
namespace ItemUse
{
// Words, in this order: unclassified heal mana damage_one damage_area control move protect dispel boost long_buff
// food_drink coating summon resurrect transport upgrade learn open quest mount toy
enum class Kind : std::uint8_t
{
    Unclassified,
    Heal,
    Mana,
    DamageOne,
    DamageArea,
    Control,
    Move,
    Protect,
    Dispel,
    Boost,
    LongBuff,
    FoodDrink,
    Coating,
    Summon,
    Resurrect,
    Transport,
    Upgrade,
    Learn,
    Open,
    Quest,
    Mount,
    Toy,
    Count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Kind::Count)> KIND_WORDS = {
    "unclassified", "heal",  "mana",      "damage_one", "damage_area", "control", "move",      "protect",
    "dispel",       "boost", "long_buff", "food_drink", "coating",     "summon",  "resurrect", "transport",
    "upgrade",      "learn", "open",      "quest",      "mount",       "toy"};

constexpr std::string_view KindWord(Kind kind)
{
    std::size_t const index = static_cast<std::size_t>(kind);
    return index < KIND_WORDS.size() ? KIND_WORDS[index] : std::string_view("unclassified");
}

struct Classified
{
    Kind kind{Kind::Unclassified};
    std::uint8_t sub{0};   // a finer word inside the kind (summon: helper, decoy, repair; move: speed, fall, jump)
    std::int32_t rank{0};  // the amount that ranks two items of one kind: larger is stronger
    std::string_view why;  // unclassified: what made it so (a literal); else empty
};

// Pure, any thread. The stub of WI2: nothing is classified yet
inline Classified Classify(Record const&) { return {Kind::Unclassified, 0, 0, "no_classifier"}; }
}  // namespace ItemUse

#endif
