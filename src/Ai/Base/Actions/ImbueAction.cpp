/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ImbueAction.h"

#include "Event.h"
#include "ItemUse.h"
#include "Playerbots.h"

namespace
{
// A coating on the bot's worn weapon through the item use path (caller buff, item-use WI5). Check first: the trigger
// holds while the coating's cast runs, and a refusal then writes no row
void CoatWorn(Player* bot, Item* coating, uint8 slot)
{
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!weapon)
        return;

    ItemUse::Request const request = ItemUse::OnItem(coating, weapon, ItemUse::Caller::Buff);
    if (ItemUse::Check(bot, request) == ItemUse::Cause::None)
        ItemUse::Use(bot, request);
}
}  // namespace

ImbueWithPoisonAction::ImbueWithPoisonAction(PlayerbotAI* botAI) : Action(botAI, "apply poison") {}

bool ImbueWithPoisonAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat())
        return false;

    // remove stealth
    if (bot->HasAura(SPELL_AURA_MOD_STEALTH))
        bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    static const std::vector<uint32_t> prioritizedInstantPoisons = {
        INSTANT_POISON_IX, INSTANT_POISON_VIII, INSTANT_POISON_VII, INSTANT_POISON_VI, INSTANT_POISON_V, INSTANT_POISON_IV,
        INSTANT_POISON_III, INSTANT_POISON_II, INSTANT_POISON
    };

    static const std::vector<uint32_t> prioritizedDeadlyPoisons = {
        DEADLY_POISON_IX, DEADLY_POISON_VIII, DEADLY_POISON_VII, DEADLY_POISON_VI, DEADLY_POISON_V, DEADLY_POISON_IV,
        DEADLY_POISON_III, DEADLY_POISON_II, DEADLY_POISON
    };

    // Check if we have any deadly or instant poisons
    Item* deadlyPoison = nullptr;
    for (auto id : prioritizedDeadlyPoisons)
    {
        deadlyPoison = botAI->FindConsumable(id);
        if (deadlyPoison) break;
    }

    Item* instantPoison = nullptr;
    for (auto id : prioritizedInstantPoisons)
    {
        instantPoison = botAI->FindConsumable(id);
        if (instantPoison) break;
    }

    // Mainhand
    Item* poison = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        if (instantPoison && deadlyPoison)
        {
            poison = instantPoison;
        }
        else if (deadlyPoison)
        {
            poison = deadlyPoison;
        }
        else if (instantPoison)
        {
            poison = instantPoison;
        }

        if (poison)
        {
            CoatWorn(bot, poison, EQUIPMENT_SLOT_MAINHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    // Offhand
    poison = nullptr;
    weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        if (deadlyPoison && instantPoison)
        {
            poison = deadlyPoison;
        }
        else if (instantPoison)
        {
            poison = instantPoison;
        }
        else if (deadlyPoison)
        {
            poison = deadlyPoison;
        }

        if (poison)
        {
            CoatWorn(bot, poison, EQUIPMENT_SLOT_OFFHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    return true;
}

// Search and apply stone to weapons
ImbueWithStoneAction::ImbueWithStoneAction(PlayerbotAI* botAI) : Action(botAI, "apply stone") {}

bool ImbueWithStoneAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat())
        return false;

    // remove stealth
    if (bot->HasAura(SPELL_AURA_MOD_STEALTH))
        bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

    // hp check
    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // Search and apply stone to weapons
    // Mainhand ...
    Item* stone = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        stone = botAI->FindStoneFor(weapon);
        if (stone)
        {
            CoatWorn(bot, stone, EQUIPMENT_SLOT_MAINHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    //... and offhand
    weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        stone = botAI->FindStoneFor(weapon);
        if (stone)
        {
            CoatWorn(bot, stone, EQUIPMENT_SLOT_OFFHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    return true;
}

// Search and apply oil to weapons
ImbueWithOilAction::ImbueWithOilAction(PlayerbotAI* botAI) : Action(botAI, "apply oil") {}

bool ImbueWithOilAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat())
        return false;

    // remove stealth
    if (bot->HasAura(SPELL_AURA_MOD_STEALTH))
        bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

    // hp check
    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // Search and apply oil to weapons
    Item* oil = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        oil = botAI->FindOilFor(weapon);
        if (oil)
        {
            CoatWorn(bot, oil, EQUIPMENT_SLOT_MAINHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    return true;
}
