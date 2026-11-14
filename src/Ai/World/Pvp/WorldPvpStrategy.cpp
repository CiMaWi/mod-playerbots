/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldPvpStrategy.h"

#include "FightBook.h"
#include "FleeAttackersAction.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "TestConstraints.h"

void WorldPvpStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("world pvp retreat", {NextAction("flee attackers", ACTION_EMERGENCY + 1.0f)}));
}

bool WorldPvpRetreatTrigger::IsActive()
{
    float x = 0.0f;
    float y = 0.0f;
    bool const wants = botAI->GetBot()->IsAlive() &&
                       (TestConstraints::Flees(botAI->rpgInfo.constraints) || WorldPvp::RetreatFrom(botAI, x, y));
    return FleeAttackersAction::Due(botAI, wants);
}
