/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailService.h"
#include "BankMailState.h"
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

bool MailVisitActiveTrigger::IsActive() { return botAI->bankMail.mailVisit.phase != BankMail::MailVisitPhase::None; }

bool BankVisitActiveTrigger::IsActive() { return botAI->bankMail.bankVisit.phase != BankMail::BankVisitPhase::None; }

bool WorldTravel::ErrandMayStart(PlayerbotAI* botAI)
{
    // A bot in a mailbox or banker visit finishes it first (the visit's action holds it)
    if (!TestConstraints::ErrandAllowed(botAI->testConstraints) || BankMail::VisitRuns(botAI->bankMail))
        return false;
    Player* bot = botAI->GetBot();
    if (!Upkeep::ErrandMayStart(botAI->rpgInfo.GetStatus(), bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS)))
        return false;

    return botAI->KeepsPossessions() && !bot->IsInCombat() && bot->IsAlive() && !bot->GetGroup() &&
           !bot->GetMap()->Instanceable();
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
    UpkeepNeeds::NeedList const list = Planner::ComputeErrandNeeds(botAI, snapshot, *index);
    return UpkeepNeeds::AnyErrandDue(list);
}

bool InventoryPressureTrigger::IsActive()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Full bags, or the needs (upkeep §3.3, the errand check's providers) after the retry time: the repair first,
    // then a due stock, training or riding rank (one snapshot)
    WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    uint32 const nowMs = getMSTime();
    bool const bagsDue = WorldTravel::ErrandDue(measure, AI_VALUE(uint8, "bag space"), nowMs);
    // A planner bot's errands are the planner's (planning 3.19): full bags or gear due for repair only tell it that
    // the situation changed, once per retry time each, in any status; no needs list here. The gear is looked at every
    // PLANNER_WEAR_LOOK_MS while no repair is due (wear comes in fights; a death is the state key's edge), so a due
    // repair reaches the planner then, not at its next snapshot; a changed bag capacity reaches it at once
    // (NoteBagSlots). Money is no edge: it grows slowly, and the planner's snapshot weighs the offers' prices
    if (Planner::Owns(botAI))
    {
        Planner::NoteBagSlots(botAI);
        constexpr uint32 PLANNER_WEAR_LOOK_MS = 30 * IN_MILLISECONDS;
        bool repairDue = false;
        if (WorldTravel::NeedsCheckOver(measure, nowMs))
        {
            repairDue = WorldTravel::RepairErrandWanted(bot);
            measure.NeedsChecked = true;
            measure.NeedsCheckMs = repairDue ? nowMs : nowMs - (WorldTravel::ERRAND_RETRY_MS - PLANNER_WEAR_LOOK_MS);
        }
        if (bagsDue)
            WorldTravel::StampErrandCheck(measure, nowMs);
        if (bagsDue || repairDue)
            Planner::SituationChanged(botAI);
        return false;
    }

    if (!WorldTravel::ErrandMayStart(botAI))
        return false;
    if (bagsDue)
        return true;

    if (!WorldTravel::ErrandRetryOver(measure, nowMs))
        return false;

    // Mail or a bank errand worth a trip of its own (WB8b: the same check, no trigger of its own; as full bags, it
    // does not end questing)
    if (BankMail::ErrandWaits(botAI->bankMail, bot->GetLevel()) && WorldTravel::ErrandMayStart(botAI))
        return true;

    // The needs once per retry time while nothing is due (WU27: a full snapshot and the needs per check otherwise)
    return WorldTravel::NeedsCheckDue(
        measure, nowMs,
        [this] { return WorldTravel::RepairErrandWanted(bot) || WorldTravel::NeedErrandWanted(botAI); });
}

bool InnBindTrigger::IsActive()
{
    if (!WorldTravel::ErrandMayStart(botAI) || !WorldTravel::InnCheckDue(botAI->rpgMemos.lastInnMs, getMSTime()) ||
        !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    bool const sameMap = bot->m_homebindMapId == bot->GetMapId();
    float const yards = sameMap ? bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) : 0.0f;
    return WorldTravel::BindIsFar(sameMap, yards);
}
