/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FURYTABLE_H
#define PLAYERBOTS_FURYTABLE_H

#include "SpecTable.h"
#include "Strategy.h"

// The Fury warrior's combat (combat-foundation CF7), one table at every level: rows in band order, the rotation in
// the old list's order at level 80. Whirlwind decides the stance: before it a Fury warrior has no Berserker kit and
// fights from Battle Stance with the Arms rows it played until now (WARRIOR-13). A row whose spell wants a stance
// stands only on its side of Whirlwind, so no cast fails on the stance (WARRIOR-05)

namespace warrior
{
using spec::Knows;
using spec::NotKnows;

inline constexpr auto FuryRows = spec::Rows(
    // Survive
    spec::Defensive("berserker rage", spec::HealthTier::Critical)
        .Instead("fear sleep sap")
        .Requires(Knows("whirlwind")),
    // before Whirlwind, the Arms list's two answers to damage as it had them (CF.20): Retaliation while the bot's
    // health is between medium and almost full (its action asks for two melee attackers, WARRIOR-03), the fear at
    // critical health (WARRIOR-09)
    spec::Defensive("retaliation", spec::HealthTier::AlmostFull).Requires(NotKnows("whirlwind")),
    spec::Defensive("intimidating shout", spec::HealthTier::Critical).Requires(NotKnows("whirlwind")),
    spec::Defensive("enraged regeneration", spec::HealthTier::Critical),
    // Interrupt
    spec::Interrupt("pummel"), spec::Interrupt("pummel on enemy healer", spec::Who::EnemyHealer),
    // Move: Charge from Battle Stance, Intercept from Berserker Stance; walking is the role's node
    spec::MoveTool("charge", spec::Purpose::Close).Requires(NotKnows("whirlwind")),
    spec::MoveTool("intercept", spec::Purpose::Close).Requires(Knows("whirlwind")),
    // Rotation
    spec::Spender("victory rush").When(spec::Trigger("victory rush")),
    spec::Form("battle stance").Requires(NotKnows("whirlwind")),
    spec::Form("berserker stance").Requires(Knows("whirlwind")),
    // the old trigger also skips the shout when a stronger attack power buff is on the bot
    spec::Renewal("battle shout", spec::Subject::Self).Instead("battle shout"),
    spec::Renewal("bloodthirst", spec::Subject::Self), spec::Spender("whirlwind"),
    spec::Spender("slam").When(spec::Trigger("instant slam")),
    // before Whirlwind: the Arms rows
    spec::Renewal("rend", spec::Subject::TargetMine).Requires(NotKnows("whirlwind")),
    // the Arms list's Rend on the other attackers, which Fury played below Whirlwind (CF.19)
    spec::Renewal("rend on attacker", spec::Subject::Target)
        .Instead("rend on attacker")
        .Requires(NotKnows("whirlwind")),
    spec::Spender("overpower").When(spec::Trigger("overpower")).Requires(NotKnows("whirlwind")),
    spec::Aoe("thunder clap", spec::Centre::Self, 2).Option("aoe").Requires(NotKnows("whirlwind")),
    spec::Aoe("demoralizing shout without life time check", spec::Centre::Self, 2).Option("aoe"),
    spec::Aoe("cleave", spec::Centre::Target, 2).Option("aoe"), spec::Recover("bloodrage").Instead("bloodrage"),
    spec::Cooldown("death wish").Instead("death wish"), spec::Cooldown("recklessness").Instead("recklessness"),
    spec::Spender("slam").When(spec::PowerAtLeast(60)).Requires(NotKnows("whirlwind")), spec::Spender("bloodthirst"),
    // above Sunder Armor (WARRIOR-10); Execute works below a fifth of the target's health
    spec::Spender("execute").When(spec::TargetHealthBelow(20)),
    // before Whirlwind, the Arms list's snare of an enemy that runs or goes for a group member, answered with
    // Hamstring alone (WARRIOR-08, CF.20)
    spec::Control("hamstring").Instead("hamstring").Requires(NotKnows("whirlwind")), spec::Spender("sunder armor"),
    spec::Spender("heroic strike").When(spec::PowerAtLeast(40)),
    // Filler
    spec::Filler("melee"));

inline constexpr spec::Table FuryTable{"fury", spec::Role::Melee, STRATEGY_TYPE_DPS | STRATEGY_TYPE_MELEE, FuryRows};
}  // namespace warrior

#endif
