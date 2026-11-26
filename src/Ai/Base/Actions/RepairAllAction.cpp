/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RepairAllAction.h"
#include "ChatHelper.h"
#include "Event.h"
#include "EventLog.h"
#include "Playerbots.h"

bool RepairAllAction::Execute(Event event)
{
    // An errand visit names the repairer it stands at (NR3c, as the sale since WR30): the nearest npcs filter by line
    // of sight, which a bot beside its repairer can lack (NR1 group C; run 1 of ne-sick-repair: found none at 1668)
    ObjectGuid const named = event.getObject();
    GuidVector const npcs = named.IsEmpty() ? AI_VALUE(GuidVector, "nearest npcs") : GuidVector{named};
    for (ObjectGuid const guid : npcs)
    {
        Creature* unit = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_REPAIR);
        if (!unit)
            continue;

        if (bot->HasUnitState(UNIT_STATE_DIED))
            bot->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

        bot->SetFacingToObject(unit);
        float discountMod = bot->GetReputationPriceDiscount(unit);

        uint32 botMoney = bot->GetMoney();
        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(10000000);
            EventLog::Honesty(bot, EventLog::HonestyKind::Gold, "cheat_gold", 0, int64(10000000) - int64(botMoney));
        }

        // Repair weapons first.
        uint32 totalCost = bot->DurabilityRepair(EQUIPMENT_SLOT_MAINHAND, true, discountMod, false);
        totalCost += bot->DurabilityRepair(EQUIPMENT_SLOT_RANGED, true, discountMod, false);
        totalCost += bot->DurabilityRepair(EQUIPMENT_SLOT_OFFHAND, true, discountMod, false);

        totalCost += bot->DurabilityRepairAll(true, discountMod, false);

        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        if (totalCost > 0)
        {
            std::ostringstream out;
            out << "Repair: " << chat->formatMoney(totalCost) << " (" << unit->GetName() << ")";
            botAI->TellMasterNoFacing(out.str());

            bot->PlayDistanceSound(1116);
        }

        context->GetValue<uint32>("death count")->Set(0);

        return true;
    }

    botAI->TellError("Cannot find any npc to repair at");
    return false;
}
