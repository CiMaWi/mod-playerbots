/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HOLYHEALTABLE_H
#define PLAYERBOTS_HOLYHEALTABLE_H

#include "PriestRows.h"
#include "SpecTable.h"
#include "Strategy.h"

// The Holy priest healer's combat (combat-foundation CF7), one table at every level. The heal rows are the old
// list's heals by tier and scope in its order; below Greater Heal the old fall-back chain (Heal, then Lesser Heal)
// stands as rows that only a bot without the better spell plays. No heal waits for a form: Holy has none

namespace priest
{
using spec::HealScope;

inline constexpr auto HolyHealRows = spec::Rows(
    // Survive
    SurviveRows,
    // Dispel
    CureRows,
    // Heal
    spec::Heal("divine hymn", HealthTier::Low, HealScope::GroupHard),
    spec::Heal("guardian spirit on party", HealthTier::Critical),
    spec::Heal("prayer of mending on party", HealthTier::Low, HealScope::GroupHard),
    spec::Heal("power word: shield on party", HealthTier::Critical),
    spec::Heal("circle of healing on party", HealthTier::Low, HealScope::GroupHard),
    spec::Heal("prayer of healing on party", HealthTier::Low, HealScope::GroupHard),
    spec::Heal("prayer of mending on party", HealthTier::Critical),
    spec::Heal("flash heal on party", HealthTier::Critical),
    spec::Heal("prayer of mending on party", HealthTier::Medium, HealScope::Group),
    spec::Heal("circle of healing on party", HealthTier::Medium, HealScope::Group),
    spec::Heal("greater heal on party", HealthTier::Medium),
    spec::Heal("heal on party", HealthTier::Medium).Requires(NotKnows("greater heal")),
    spec::Heal("lesser heal on party", HealthTier::Medium).Requires(NotKnows("heal")),
    spec::Heal("circle of healing on party", HealthTier::Low),
    spec::Heal("prayer of mending on party", HealthTier::Low),
    spec::Heal("greater heal on party", HealthTier::Critical),
    spec::Heal("heal on party", HealthTier::Critical).Requires(NotKnows("greater heal")),
    spec::Heal("lesser heal on party", HealthTier::Critical).Requires(NotKnows("heal")),
    spec::Heal("greater heal on party", HealthTier::Low),
    spec::Heal("heal on party", HealthTier::Low).Requires(NotKnows("greater heal")),
    spec::Heal("lesser heal on party", HealthTier::Low).Requires(NotKnows("heal")),
    spec::Heal("flash heal on party", HealthTier::Low), spec::Heal("circle of healing on party", HealthTier::Medium),
    spec::Heal("prayer of mending on party", HealthTier::Medium), spec::Heal("flash heal on party", HealthTier::Medium),
    spec::Heal("renew on party", HealthTier::AlmostFull),
    spec::Heal("prayer of mending on party", HealthTier::AlmostFull),
    // Rotation
    RotationRows,
    // option "healer dps": damage while nobody needs a heal; Mind Blast above Smite (PRIEST-12)
    spec::Aoe("mind sear", spec::Centre::Target, 3).When(spec::Trigger("healer should attack")).Option("healer dps"),
    spec::Renewal("shadow word: pain", spec::Subject::TargetMine)
        .When(spec::Trigger("healer should attack"))
        .Option("healer dps"),
    spec::Renewal("holy fire", spec::Subject::TargetMine)
        .When(spec::Trigger("healer should attack"))
        .Option("healer dps"),
    spec::Spender("mind blast").When(spec::Trigger("healer should attack")).Option("healer dps"),
    spec::Spender("smite").When(spec::Trigger("healer should attack")).Option("healer dps"),
    // Filler: the wand, then the staff or mace
    spec::Filler("shoot").Requires(spec::Wields(spec::Gear::Wand)), spec::Filler("melee"));

inline constexpr spec::Table HolyHealTable{"holy heal", spec::Role::Healer, STRATEGY_TYPE_HEAL | STRATEGY_TYPE_RANGED,
                                           HolyHealRows};
}  // namespace priest

#endif
