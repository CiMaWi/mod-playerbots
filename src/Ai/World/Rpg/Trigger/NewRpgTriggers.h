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

namespace WorldTravel
{
// A persistent bot alone in the open world, alive and out of combat, in an RPG status an errand may interrupt (the
// trigger and the destroy rule share it)
bool ErrandMayStart(PlayerbotAI* botAI);
}  // namespace WorldTravel

// A persistent bot with full bags in an RPG status it may leave, due for an errand check (ItemWorthRules.h ErrandDue)
class InventoryPressureTrigger : public Trigger
{
public:
    InventoryPressureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "inventory pressure", CHECK_SECONDS) {}
    bool IsActive() override;

private:
    static constexpr int32 CHECK_SECONDS = 30;
};

// A mailbox visit runs (BankMail::BeginMailVisit; started only by a command until the trips start one): a bool
class MailVisitActiveTrigger : public Trigger
{
public:
    MailVisitActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mail visit active") {}
    bool IsActive() override;
};

// A banker visit runs (BankMail::BeginBankVisit; started only by a command until the trips start one): a bool
class BankVisitActiveTrigger : public Trigger
{
public:
    BankVisitActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "bank visit active") {}
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
