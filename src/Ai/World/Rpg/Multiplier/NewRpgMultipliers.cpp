/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgMultipliers.h"

#include <variant>

#include "GoalInterface.h"
#include "PlannerService.h"
#include "Playerbots.h"
#include "TestConstraints.h"
#include "UpkeepRules.h"

float NewRpgGrindMultiplier::GetValue(Action* action)
{
    if (!action || action->getName() != "attack anything")
        return 1.0f;

    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    Planner::GoalApply::BotGoals const* goals = botAI->planner.goals.get();
    Planner::GoalApply::Active const none;
    Planner::GoalApply::GrindPick const pick = Planner::GoalApply::GrindPickOf(
        Planner::Owns(botAI), !bot->GetMap()->Instanceable(), goals ? goals->slot.Query() : none,
        TestConstraints::StartsFights(botAI->testConstraints), bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS),
        status == RPG_TRAVEL);
    if (pick != Planner::GoalApply::GrindPick::Upstream)
        return pick == Planner::GoalApply::GrindPick::Allow ? 1.0f : 0.0f;

    // Upstream's bots: the quest executor's target is the one it attacks, not whatever stands near
    auto const* doQuest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (doQuest && !doQuest->targetGuid.IsEmpty())
        return 0.0f;

    switch (status)
    {
        case RPG_GO_GRIND:
        case RPG_GO_CAMP:
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_DO_QUEST:
            break;
        default:
            return 1.0f;  // this status's action never visits a giver
    }

    // The giver visit runs first in the status action (3.0), below "attack anything" (4.0). Raising the status
    // actions would also put their movement above grinding, and a multiplier above 1 cannot reorder the queue:
    // the engine orders by base relevance and applies multipliers after the pop. So grinding is dropped for the
    // tick instead. SearchQuestGiverAndAcceptOrReward gives up on a giver after a minute, which empties the value.
    return AI_VALUE(ObjectGuid, "new rpg quest giver").IsEmpty() ? 1.0f : 0.0f;
}
