/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ELEMENTALTABLE_H
#define PLAYERBOTS_ELEMENTALTABLE_H

#include "ShamanRows.h"
#include "SpecTable.h"
#include "Strategy.h"

// The Elemental shaman's combat (combat-foundation CF7), one table at every level: rows in band order, the rotation
// in the old list's order at level 80; a spell not learned yet stands unknown and the next row plays

namespace shaman
{
inline constexpr auto ElementalRows = spec::Rows(
    // Survive
    spec::Defensive("stoneclaw totem", spec::HealthTier::Low),
    // Dispel
    spec::Dispel("purge", spec::DispelOn::Enemy, spec::DispelType::Magic), CureRows,
    // Interrupt
    spec::Interrupt("wind shear"), spec::Interrupt("wind shear on enemy healer", spec::Who::EnemyHealer),
    // Move
    spec::MoveTool("thunderstorm", spec::Purpose::Escape),
    // Rotation
    spec::Pet("set pet stance").Instead("new pet"),
    spec::Renewal("call of the elements", Subject::Totem).Instead("call of the elements"), CasterTotemRows,
    spec::Cooldown("heroism").Instead("heroism").Option("boost"),
    spec::Cooldown("bloodlust").Instead("bloodlust").Option("boost"),
    spec::Cooldown("elemental mastery").When(spec::Trigger("elemental mastery")),
    spec::Cooldown("fire elemental totem").Instead("fire elemental totem").Option("boost"),
    spec::Aoe("fire nova", spec::Centre::Totem, 3).Option("aoe"), spec::Renewal("water shield", Subject::Self),
    spec::Renewal("lightning shield", Subject::Self).Requires(NotKnows("water shield")),
    spec::Recover("thunderstorm", spec::ManaTier::High),
    // the old "chain lightning no cd" fired on one target too: its cooldown is the cast gate's
    spec::Spender("chain lightning").Option("aoe"),
    spec::Spender("earth shock").When(spec::Trigger("earth shock execute")),
    // above Lava Burst, whose action wants the bot's own Flame Shock on the target (SHAMAN-08)
    spec::Renewal("flame shock", Subject::TargetMine), spec::Spender("lava burst"), spec::Spender("lightning bolt"),
    // Filler
    spec::Filler("melee"));

inline constexpr spec::Table ElementalTable{"ele", spec::Role::Ranged, STRATEGY_TYPE_DPS | STRATEGY_TYPE_RANGED,
                                            ElementalRows};
}  // namespace shaman

#endif
