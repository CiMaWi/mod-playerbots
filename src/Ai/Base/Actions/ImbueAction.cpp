/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ImbueAction.h"

#include "Event.h"
#include "ItemUse.h"
#include "Playerbots.h"

static const uint32 uPrioritizedHealingItemIds[19] = {
    HEALTHSTONE,
    FEL_REGENERATION_POTION,
    SUPER_HEALING_POTION,
    CRYSTAL_HEALING_POTION,
    MAJOR_DREAMLESS_SLEEP_POTION,
    VOLATILE_HEALING_POTION,
    MAJOR_HEALING_POTION,
    WHIPPER_ROOT_TUBER,
    NIGHT_DRAGON_BREATH,
    LIMITED_INVULNERABILITY_POTION,
    GREATER_DREAMLESS_SLEEP_POTION,
    SUPERIOR_HEALING_POTION,
    CRYSTAL_RESTORE,
    DREAMLESS_SLEEP_POTION,
    GREATER_HEALING_POTION,
    HEALING_POTION,
    LESSER_HEALING_POTION,
    DISCOLORED_HEALING_POTION,
    MINOR_HEALING_POTION,
};

TryEmergencyAction::TryEmergencyAction(PlayerbotAI* botAI) : Action(botAI, "try emergency") {}

bool TryEmergencyAction::Execute(Event /*event*/)
{
    // Do not use consumable if bot can heal self
    if ((PlayerbotAI::IsHeal(bot)) && (bot->GetPowerPct(POWER_MANA) > 20))
        return false;

    // If bot does not have aggro: use bandage instead of potion/stone/crystal
    if ((!AI_VALUE(uint8, "my attacker count")) && !bot->HasAura(11196))  // Recently bandaged
    {
        if (Item* bandage = botAI->FindBandage())
        {
            botAI->ImbueItem(bandage, bot);
            botAI->SetNextCheckDelay(8);
        }
    }

    // Else loop over the list of health consumable to pick one
    for (uint8 i = 0; i < std::size(uPrioritizedHealingItemIds); ++i)
    {
        if (Item* healthItem = botAI->FindConsumable(uPrioritizedHealingItemIds[i]))
        {
            botAI->ImbueItem(healthItem);
        }
    }

    return true;
}
