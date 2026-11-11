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
// Map thread: the repair need makes an errand worth a trip of its own (UpkeepNeedRules.h RepairErrandDue)
bool RepairErrandWanted(Player* bot);
}  // namespace WorldTravel

// A persistent bot with full bags or gear due for repair, in an RPG status it may leave, due for an errand check
// (ItemWorthRules.h ErrandDue and ErrandRetryOver)
class InventoryPressureTrigger : public Trigger
{
public:
    InventoryPressureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "inventory pressure", CHECK_SECONDS) {}
    bool IsActive() override;

private:
    static constexpr int32 CHECK_SECONDS = 30;
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
