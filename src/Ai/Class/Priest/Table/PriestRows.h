/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PRIESTROWS_H
#define PLAYERBOTS_PRIESTROWS_H

#include "SpecTable.h"

// Priest row groups (combat-foundation INTERFACE §9), joined in place by both Holy tables: one group per band they
// fill, since a table lists its rows in band order

namespace priest
{
using spec::DispelOn;
using spec::DispelType;
using spec::HealthTier;
using spec::NotKnows;

// Survive: the shield on being attacked joins the bot's own survival; Fade drops threat
inline constexpr auto SurviveRows = spec::Rows(
    spec::Defensive("desperate prayer", HealthTier::Critical), spec::Defensive("power word: shield", HealthTier::Low),
    spec::Defensive("power word: shield", HealthTier::Low).Instead("being attacked"),
    spec::Defensive("fade", HealthTier::Low).Instead("medium threat"));

// Option "cure": Abolish Disease, Cure Disease below it
inline constexpr auto CureRows =
    spec::Rows(spec::Dispel("dispel magic", DispelOn::Self, DispelType::Magic).Option("cure"),
               spec::Dispel("dispel magic on party", DispelOn::Party, DispelType::Magic).Option("cure"),
               spec::Dispel("abolish disease", DispelOn::Self, DispelType::Disease).Option("cure"),
               spec::Dispel("cure disease", DispelOn::Self, DispelType::Disease)
                   .Option("cure")
                   .Requires(NotKnows("abolish disease")),
               spec::Dispel("abolish disease on party", DispelOn::Party, DispelType::Disease).Option("cure"),
               spec::Dispel("cure disease on party", DispelOn::Party, DispelType::Disease)
                   .Option("cure")
                   .Requires(NotKnows("abolish disease")));

// Rotation: Inner Focus at medium mana, as the old class list had it; Shadowfiend and Hymn of Hope when the mana is
// low, not at the first dip (PRIEST-05)
inline constexpr auto RotationRows =
    spec::Rows(spec::Pet("set pet stance").Instead("new pet"),
               spec::Cooldown("power infusion").Instead("power infusion").Option("boost"),
               spec::Control("shackle undead").Instead("shackle undead").Option("cc"),
               spec::Renewal("fear ward on main tank", spec::Subject::Party).Instead("fear ward on main tank"),
               spec::Recover("inner focus", spec::ManaTier::Medium), spec::Recover("shadowfiend", spec::ManaTier::Low),
               spec::Recover("hymn of hope", spec::ManaTier::Low));
}  // namespace priest

#endif
