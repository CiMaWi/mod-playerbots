/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemWorthRules.h"
#include "NewRpgTriggers.h"
#include "PlannerService.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "UpkeepNeedRules.h"
#include "WorldTravelRules.h"

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

bool WorldTravel::RepairErrandWanted(Player* bot)
{
    // The wear first: the price pass runs only for gear that is due
    Planner::GearWear const wear = Planner::ReadGearWear(bot);
    if (!UpkeepNeeds::RepairDue(wear.broken, wear.lowestPercent))
        return false;

    return UpkeepNeeds::RepairErrandDue(wear.broken, wear.lowestPercent, Planner::PriceRepair(bot).cheapest,
                                        bot->GetMoney());
}

bool InventoryPressureTrigger::IsActive()
{
    if (!WorldTravel::ErrandMayStart(botAI))
        return false;

    // Full bags, or the repair need (upkeep §3.3: one call from the errand check), both after the retry time
    WorldTravel::SlotMeasure const& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    uint32 const nowMs = getMSTime();
    if (!WorldTravel::ErrandDue(measure, AI_VALUE(uint8, "bag space"), nowMs) &&
        !(WorldTravel::ErrandRetryOver(measure, nowMs) && WorldTravel::RepairErrandWanted(bot)))
        return false;

    return sRandomPlayerbotMgr.IsRandomBot(bot);
}

bool InnBindTrigger::IsActive()
{
    if (!WorldTravel::ErrandMayStart(botAI) || !WorldTravel::InnCheckDue(botAI->rpgInfo.lastInnMs, getMSTime()) ||
        !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    bool const sameMap = bot->m_homebindMapId == bot->GetMapId();
    float const yards = sameMap ? bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) : 0.0f;
    return WorldTravel::BindIsFar(sameMap, yards);
}
