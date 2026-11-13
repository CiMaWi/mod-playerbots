/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InvalidTargetValue.h"

#include "AttackersValue.h"
#include "FightBook.h"
#include "Playerbots.h"
#include "PvpValues.h"
#include "Unit.h"

bool InvalidTargetValue::Calculate()
{
    Unit* target = AI_VALUE(Unit*, qualifier);
    if (IsUnavailableCombatTarget(bot, target))
        return true;

    // World PvP (WP6): a winner stops chasing a retreated enemy at its chase limit, and a bot drops a target it leaves
    // alone (a retreated bot whose chaser stopped attacking it), a player or a player's pet
    if (target && qualifier == "current target" && target->GetCharmerOrOwnerPlayerOrPlayerItself() &&
        ((target->IsPlayer() && WorldPvp::ChaseEnds(botAI, target)) || WorldPvp::LeftAlone(botAI, target)))
        return true;

    Unit* enemy = AI_VALUE(Unit*, "enemy player target");
    if (target && enemy && target == enemy && target->IsAlive())
        return false;

    if (target && qualifier == "current target")
    {
        return target->GetMapId() != bot->GetMapId() || target->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) ||
               target->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || target->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE_2) ||
               !target->IsVisible() || !target->IsAlive() || target->IsPolymorphed() ||
               (target->IsCharmed() && target->IsPlayer() &&  // only a mind-controlled teammate is off limits
                target->ToPlayer()->GetTeamId(true) == bot->GetTeamId(true)) ||
               target->HasFearAura() || target->HasUnitState(UNIT_STATE_ISOLATED) || target->IsFriendlyTo(bot) ||
               !AttackersValue::IsValidTarget(target, bot);
    }

    return !target;
}
