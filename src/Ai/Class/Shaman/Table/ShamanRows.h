/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHAMANROWS_H
#define PLAYERBOTS_SHAMANROWS_H

#include "SpecTable.h"

// Shaman row groups (combat-foundation INTERFACE §9), joined in place by the shaman tables

namespace shaman
{
using spec::DispelOn;
using spec::DispelType;
using spec::NotKnows;
using spec::Subject;

// Option "cure": Cleanse Spirit (a Restoration talent) for poison, disease and curse, Cure Toxins below it
inline constexpr auto CureRows = spec::Rows(
    spec::Dispel("cleanse spirit", DispelOn::Self, DispelType::Poison).Option("cure"),
    spec::Dispel("cleanse spirit", DispelOn::Self, DispelType::Disease).Option("cure"),
    spec::Dispel("cleanse spirit", DispelOn::Self, DispelType::Curse).Option("cure"),
    spec::Dispel("cure toxins", DispelOn::Self, DispelType::Poison).Option("cure").Requires(NotKnows("cleanse spirit")),
    spec::Dispel("cure toxins", DispelOn::Self, DispelType::Disease)
        .Option("cure")
        .Requires(NotKnows("cleanse spirit")),
    spec::Dispel("cleanse spirit poison on party", DispelOn::Party, DispelType::Poison).Option("cure"),
    spec::Dispel("cleanse spirit disease on party", DispelOn::Party, DispelType::Disease).Option("cure"),
    spec::Dispel("cleanse spirit curse on party", DispelOn::Party, DispelType::Curse).Option("cure"),
    spec::Dispel("cure toxins poison on party", DispelOn::Party, DispelType::Poison)
        .Option("cure")
        .Requires(NotKnows("cleanse spirit")),
    spec::Dispel("cure toxins disease on party", DispelOn::Party, DispelType::Disease)
        .Option("cure")
        .Requires(NotKnows("cleanse spirit")));

// A totem row: its option is the totem strategy's name (one per element is on, its sibling group), its rule the old
// trigger until the totem phase step lands a Totem rule
constexpr spec::Row Totem(std::string_view action, std::string_view trigger, std::string_view option)
{
    return spec::Renewal(action, Subject::Totem).Instead(trigger).Option(option);
}

// The caster's totems: every set row (the totem on the bar) above every drop row (the totem in the world), as the
// old strategies' 60 and 55. A totem not learned yet falls through to the one its old strategy chose instead; Wrath
// of Air falls to Grounding, not to Windfury (a melee totem, SHAMAN-10)
inline constexpr auto CasterTotemRows = spec::Rows(
    // set: earth
    Totem("set strength of earth totem", "set strength of earth totem", "strength of earth"),
    Totem("set stoneskin totem", "set stoneskin totem", "stoneskin"),
    Totem("set tremor totem", "set tremor totem", "tremor"),
    Totem("set earthbind totem", "set earthbind totem", "earthbind"),
    // set: fire
    Totem("set searing totem", "set searing totem", "searing"), Totem("set magma totem", "set magma totem", "magma"),
    Totem("set flametongue totem", "set flametongue totem", "flametongue"),
    Totem("set totem of wrath", "set totem of wrath", "wrath"),
    Totem("set flametongue totem", "set flametongue totem", "wrath").Requires(NotKnows("totem of wrath")),
    Totem("set frost resistance totem", "set frost resistance totem", "frost resistance"),
    // set: water
    Totem("set healing stream totem", "set healing stream totem", "healing stream"),
    Totem("set mana spring totem", "set mana spring totem", "mana spring"),
    Totem("set cleansing totem", "set cleansing totem", "cleansing"),
    Totem("set mana spring totem", "set mana spring totem", "cleansing").Requires(NotKnows("cleansing totem")),
    Totem("set fire resistance totem", "set fire resistance totem", "fire resistance"),
    // set: air
    Totem("set wrath of air totem", "set wrath of air totem", "wrath of air"),
    Totem("set grounding totem", "set grounding totem", "wrath of air").Requires(NotKnows("wrath of air totem")),
    Totem("set windfury totem", "set windfury totem", "windfury"),
    Totem("set grounding totem", "set grounding totem", "windfury").Requires(NotKnows("windfury totem")),
    Totem("set nature resistance totem", "set nature resistance totem", "nature resistance"),
    Totem("set grounding totem", "set grounding totem", "grounding"),
    // drop: earth
    Totem("strength of earth totem", "no earth totem", "strength of earth"),
    Totem("stoneskin totem", "no earth totem", "strength of earth").Requires(NotKnows("strength of earth totem")),
    Totem("stoneskin totem", "no earth totem", "stoneskin"), Totem("tremor totem", "no earth totem", "tremor"),
    Totem("earthbind totem", "no earth totem", "earthbind"),
    // drop: fire
    Totem("searing totem", "no fire totem", "searing"), Totem("magma totem", "no fire totem", "magma"),
    Totem("searing totem", "no fire totem", "magma").Requires(NotKnows("magma totem")),
    Totem("flametongue totem", "no fire totem", "flametongue"),
    Totem("searing totem", "no fire totem", "flametongue").Requires(NotKnows("flametongue totem")),
    Totem("totem of wrath", "no fire totem", "wrath"),
    Totem("flametongue totem", "no fire totem", "wrath").Requires(NotKnows("totem of wrath")),
    Totem("searing totem", "no fire totem", "wrath")
        .Requires(NotKnows("totem of wrath"), NotKnows("flametongue totem")),
    Totem("frost resistance totem", "no fire totem", "frost resistance"),
    // drop: water
    Totem("healing stream totem", "no water totem", "healing stream"),
    Totem("mana spring totem", "no water totem", "mana spring"),
    Totem("cleansing totem", "no water totem", "cleansing"),
    Totem("mana spring totem", "no water totem", "cleansing").Requires(NotKnows("cleansing totem")),
    Totem("fire resistance totem", "no water totem", "fire resistance"),
    // drop: air
    Totem("wrath of air totem", "no air totem", "wrath of air"),
    Totem("grounding totem", "no air totem", "wrath of air").Requires(NotKnows("wrath of air totem")),
    Totem("windfury totem", "no air totem", "windfury"),
    Totem("grounding totem", "no air totem", "windfury").Requires(NotKnows("windfury totem")),
    Totem("nature resistance totem", "no air totem", "nature resistance"),
    Totem("grounding totem", "no air totem", "grounding"));
}  // namespace shaman

#endif
