/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOLYDPSTABLE_H
#define PLAYERBOTS_HOLYDPSTABLE_H

#include "PriestRows.h"
#include "SpecTable.h"
#include "Strategy.h"

// The Holy priest's damage combat (combat-foundation CF7; a solo Holy or Discipline priest plays it), one table at
// every level. It keeps its party heals; Mana Burn and Starshards are gone (PRIEST-11), Mind Blast stands above
// Smite (PRIEST-12)

namespace priest
{
inline constexpr auto HolyDpsRows = spec::Rows(
    // Survive
    SurviveRows,
    // Dispel
    CureRows,
    // Heal: below Flash Heal the old fall-back chain (Heal, then Lesser Heal)
    spec::Heal("power word: shield on party", HealthTier::Low), spec::Heal("flash heal on party", HealthTier::Low),
    spec::Heal("heal on party", HealthTier::Low).Requires(NotKnows("flash heal")),
    spec::Heal("lesser heal on party", HealthTier::Low).Requires(NotKnows("heal")),
    spec::Heal("renew on party", HealthTier::Medium),
    // Rotation
    RotationRows,
    // option "shadow aoe"
    spec::Spender("cancel channel").When(spec::Trigger("mind sear channel check")).Option("shadow aoe"),
    spec::Aoe("mind sear", spec::Centre::Target, 3).Option("shadow aoe"),
    // option "shadow debuff"
    spec::Renewal("vampiric touch", spec::Subject::TargetMine).Option("shadow debuff"),
    spec::Renewal("devouring plague", spec::Subject::TargetMine).Option("shadow debuff"),
    spec::Renewal("shadow word: pain", spec::Subject::TargetMine).Option("shadow debuff"),
    spec::Cooldown("shadowfiend").Instead("shadowfiend"), spec::Renewal("holy fire", spec::Subject::TargetMine),
    // option "shadow aoe": the other attackers
    spec::Renewal("shadow word: pain on attacker", spec::Subject::Target)
        .Instead("shadow word: pain on attacker")
        .Option("shadow aoe"),
    spec::Renewal("vampiric touch on attacker", spec::Subject::Target)
        .Instead("vampiric touch on attacker")
        .Option("shadow aoe"),
    spec::Spender("mind blast"), spec::Spender("smite"),
    // Filler: the wand, then the staff or mace
    spec::Filler("shoot").Requires(spec::Wields(spec::Gear::Wand)), spec::Filler("melee"));

inline constexpr spec::Table HolyDpsTable{"holy dps", spec::Role::Healer, STRATEGY_TYPE_DPS | STRATEGY_TYPE_RANGED,
                                          HolyDpsRows};
}  // namespace priest

#endif
