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
#include "TestConstraints.h"
#include "Timer.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WorldTravelRules.h"

bool NewRpgStatusTrigger::IsActive() { return status == botAI->rpgInfo.GetStatus(); }

bool MailVisitActiveTrigger::IsActive()
{
    return botAI->rpgInfo.bankMail.mailVisit.phase != BankMail::MailVisitPhase::None;
}

bool BankVisitActiveTrigger::IsActive()
{
    return botAI->rpgInfo.bankMail.bankVisit.phase != BankMail::BankVisitPhase::None;
}

bool WorldTravel::ErrandMayStart(PlayerbotAI* botAI, bool needDue)
{
    if (!TestConstraints::ErrandAllowed(botAI->rpgInfo.constraints))
        return false;
    Player* bot = botAI->GetBot();
    if (!Upkeep::ErrandMayStart(botAI->rpgInfo.GetStatus(), needDue, bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS)))
        return false;

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

bool WorldTravel::NeedErrandWanted(PlayerbotAI* botAI)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    if (!index)
        return false;

    Planner::Snapshot const snapshot = Planner::FillSnapshot(botAI->GetBot(), 0);
    UpkeepNeeds::NeedList const list =
        UpkeepNeeds::ComputeNeeds(snapshot, *index, Planner::BotNeedRates(botAI, snapshot));
    return UpkeepNeeds::AnyErrandDue(list);
}

bool InventoryPressureTrigger::IsActive()
{
    // A due need may also end questing, full bags may not (Upkeep::ErrandMayStart)
    if (!WorldTravel::ErrandMayStart(botAI, true) || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Full bags, or the needs (upkeep §3.3, the errand check's providers) after the retry time: the repair first,
    // then a due stock, training or riding rank (one snapshot)
    WorldTravel::SlotMeasure const& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    uint32 const nowMs = getMSTime();
    if (WorldTravel::ErrandDue(measure, AI_VALUE(uint8, "bag space"), nowMs) && WorldTravel::ErrandMayStart(botAI))
        return true;

    return WorldTravel::ErrandRetryOver(measure, nowMs) &&
           (WorldTravel::RepairErrandWanted(bot) || WorldTravel::NeedErrandWanted(botAI));
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
