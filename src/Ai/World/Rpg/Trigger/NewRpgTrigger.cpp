/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailService.h"
#include "BankMailState.h"
#include "BotTravel.h"
#include "ItemWorthRules.h"
#include "NewRpgTriggers.h"
#include "PlannerService.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TestConstraints.h"
#include "TestProbe.h"
#include "Timer.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WorldTravelRules.h"

bool NewRpgStatusTrigger::IsActive() { return status == botAI->rpgInfo.GetStatus(); }

bool FollowAboardTrigger::IsActive() { return botAI->GetTravel().FollowAboardDue(); }

bool MailVisitActiveTrigger::IsActive() { return botAI->bankMail.mailVisit.phase != BankMail::MailVisitPhase::None; }

bool BankVisitActiveTrigger::IsActive() { return botAI->bankMail.bankVisit.phase != BankMail::BankVisitPhase::None; }

namespace
{
// The first reason an errand may not start (its word for the test probe), nullptr when it may
char const* ErrandRefusal(PlayerbotAI* botAI)
{
    // A bot in a mailbox or banker visit finishes it first (the visit's action holds it)
    if (!TestConstraints::ErrandAllowed(botAI->testConstraints))
        return "allowed";
    if (BankMail::VisitRuns(botAI->bankMail))
        return "visit";
    Player* bot = botAI->GetBot();
    bool const sick = bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    if (!Upkeep::ErrandMayStart(botAI->rpgInfo.GetStatus(), sick))
        return sick && Upkeep::ErrandMayStart(botAI->rpgInfo.GetStatus(), false) ? "sick" : "status";
    if (!botAI->KeepsPossessions())
        return "keeps";
    if (bot->IsInCombat())
        return "combat";
    if (!bot->IsAlive())
        return "dead";
    if (bot->GetGroup())
        return "group";
    if (bot->GetMap()->Instanceable())
        return "instance";
    return nullptr;
}

// The test probe (planning WT63c): the errand check's answer of a test-constrained bot, one line per check
void ProbeCheck(PlayerbotAI* botAI, std::string const& word)
{
    LOG_INFO("playerbots", "[Probe] {} errand check {} rpg {}", botAI->GetBot()->GetName(), word,
             static_cast<uint32>(botAI->rpgInfo.GetStatus()));
}
}  // namespace

bool WorldTravel::ErrandMayStart(PlayerbotAI* botAI) { return !ErrandRefusal(botAI); }

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
    // (NoteBagSlots, looked at for every bot with planner goals, so a change while a test held the planner counts at
    // its release). Money is no edge: it grows slowly, and the planner's snapshot weighs the offers' prices
    Planner::NoteBagSlots(botAI);
    if (Planner::Owns(botAI))
    {
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

    bool const probe = TestProbe::On(botAI->testConstraints);
    if (char const* const refusal = ErrandRefusal(botAI))
    {
        if (probe)
            ProbeCheck(botAI, std::string("may_start:") + refusal);
        return false;
    }
    if (bagsDue)
    {
        if (probe)
            ProbeCheck(botAI, "fired bags");
        return true;
    }

    if (!WorldTravel::ErrandRetryOver(measure, nowMs))
    {
        if (probe)
            ProbeCheck(botAI, "retry " + std::to_string((nowMs - measure.ErrandCheckMs) / IN_MILLISECONDS));
        return false;
    }

    // Mail or a bank errand worth a trip of its own (WB8b: the same check, no trigger of its own; as full bags, it
    // does not end questing)
    if (BankMail::ErrandWaits(botAI->bankMail, bot->GetLevel()) && WorldTravel::ErrandMayStart(botAI))
    {
        if (probe)
            ProbeCheck(botAI, "fired bankmail");
        return true;
    }

    // The needs once per retry time while nothing is due (WU27: a full snapshot and the needs per check otherwise)
    bool const needsOver = WorldTravel::NeedsCheckOver(measure, nowMs);
    char const* dueWord = "none";
    bool const due = WorldTravel::NeedsCheckDue(measure, nowMs,
                                                [this, &dueWord]
                                                {
                                                    if (WorldTravel::RepairErrandWanted(bot))
                                                        dueWord = "repair";
                                                    else if (WorldTravel::NeedErrandWanted(botAI))
                                                        dueWord = "need";
                                                    else
                                                        return false;
                                                    return true;
                                                });
    if (probe)
        ProbeCheck(botAI, needsOver
                              ? Acore::StringFormat("due {} index {}", dueWord, Planner::UpkeepIndexReady() ? 1 : 0)
                              : "needs_checked " + std::to_string((nowMs - measure.NeedsCheckMs) / IN_MILLISECONDS));
    return due;
}

bool InnBindTrigger::IsActive()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    bool const sameMap = bot->m_homebindMapId == bot->GetMapId();
    float const yards = sameMap ? bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) : 0.0f;
    bool const bindFar = WorldTravel::BindIsFar(sameMap, yards);
    // A planner bot's bind is the planner's (planning 3.19): a bind seen near that turned far only tells it that the
    // situation changed, in any status; the planner weighs the inn and starts the bind (its own retry time after a
    // failure). Looked at for every bot with planner goals, so a change while a test held the planner counts at its
    // release
    Planner::NoteBindFar(botAI, bindFar);
    if (Planner::Owns(botAI))
        return false;

    return bindFar && WorldTravel::ErrandMayStart(botAI) &&
           WorldTravel::InnCheckDue(botAI->rpgMemos.lastInnMs, getMSTime());
}
