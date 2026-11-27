/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGTRIGGERS_H
#define PLAYERBOTS_NEWRPGTRIGGERS_H

#include "NewRpgStrategy.h"
#include "Trigger.h"

class NewRpgStatusTrigger : public Trigger
{
public:
    NewRpgStatusTrigger(PlayerbotAI* botAI, NewRpgStatus status = RPG_IDLE)
        : Trigger(botAI, "new rpg status"), status(status)
    {
    }
    bool IsActive() override;

protected:
    NewRpgStatus status;
};

// The bot's status holds a trip (StatusEnd::WhoActsFirst, WT64f): the route follower ("new rpg travel") steps it
// ahead of the holder's executor until it ends
class TripHeldTrigger : public Trigger
{
public:
    TripHeldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "trip held") {}
    bool IsActive() override;
};

// A follower's step at its master's vessel has work (NewRpgInternal::FollowAboardDue, WT70d-b): the "follow aboard"
// action
class FollowAboardTrigger : public Trigger
{
public:
    FollowAboardTrigger(PlayerbotAI* botAI) : Trigger(botAI, "follow aboard") {}
    bool IsActive() override;
};

namespace WorldTravel
{
// A persistent bot alone in the open world, alive, out of combat and not sick, in an RPG status an errand may interrupt
// (UpkeepRules.h ErrandMayStart). The triggers and the destroy rule share it
bool ErrandMayStart(PlayerbotAI* botAI);
// Map thread, NR3: sick and awareness's recover done with the bot, on a clear point with health and mana restored
// (Upkeep::SickErrandOpen). The bot's point is judged only for a sick, restored bot
bool SickErrandOpen(PlayerbotAI* botAI);
// NR3 (NR.4): the status of a goal recover hands a sick bot over to runs: its errand, or its walk to the inn it binds
// at (StartBind's trip). The status update keeps it (Upkeep::SickEndsStatus), recover moves the bot no more
bool SickGoalRuns(PlayerbotAI* botAI);
// Map thread: the repair need makes an errand worth a trip of its own (UpkeepNeedRules.h RepairErrandDue)
bool RepairErrandWanted(Player* bot);
// Map thread: a due stock the gold pays a lot of, due training or a riding rank bought (UpkeepNeedRules.h
// AnyErrandDue); one snapshot and needs list, for the errand check after its retry time only
bool NeedErrandWanted(PlayerbotAI* botAI);
}  // namespace WorldTravel

// A persistent bot with full bags, gear due for repair or a due consumable stock, in an RPG status it may leave, due
// for an errand check (ItemWorthRules.h ErrandDue and ErrandRetryOver)
class InventoryPressureTrigger : public Trigger
{
public:
    InventoryPressureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "inventory pressure", CHECK_SECONDS) {}
    bool IsActive() override;

private:
    static constexpr int32 CHECK_SECONDS = 30;
};

// A mailbox, bank or craft visit runs (BankMail::BeginVisit: a command or an errand stop starts it): a bool
class VisitActiveTrigger : public Trigger
{
public:
    VisitActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "visit active") {}
    bool IsActive() override;
};

// A persistent bot whose bind is far (WorldTravel::BindIsFar) and whose inn check is due (every 30 minutes)
class InnBindTrigger : public Trigger
{
public:
    InnBindTrigger(PlayerbotAI* botAI) : Trigger(botAI, "inn bind", CHECK_SECONDS) {}
    bool IsActive() override;

private:
    static constexpr int32 CHECK_SECONDS = 60;
};

#endif
