/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemWorthRules.h"
#include "NewRpgTriggers.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"

bool NewRpgStatusTrigger::IsActive() { return status == botAI->rpgInfo.GetStatus(); }

bool WorldTravel::ErrandMayStart(PlayerbotAI* botAI)
{
    switch (botAI->rpgInfo.GetStatus())
    {
        case RPG_IDLE:
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_GO_GRIND:
        case RPG_GO_CAMP:
        case RPG_REST:
        case RPG_DO_GATHER:
            break;
        default:
            return false;
    }

    Player* bot = botAI->GetBot();
    return RandomPlayerbotMgr::HonestTravel() && !IsRealPlayer(botAI->GetMaster()) && !bot->IsInCombat() &&
           bot->IsAlive() && !bot->GetGroup() && !bot->GetMap()->Instanceable();
}

bool InventoryPressureTrigger::IsActive()
{
    if (!WorldTravel::ErrandMayStart(botAI))
        return false;

    WorldTravel::SlotMeasure const& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    return WorldTravel::ErrandDue(measure, AI_VALUE(uint8, "bag space"), getMSTime()) &&
           sRandomPlayerbotMgr.IsRandomBot(bot);
}
