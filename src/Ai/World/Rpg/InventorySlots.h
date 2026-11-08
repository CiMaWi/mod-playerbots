/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_INVENTORYSLOTS_H
#define _PLAYERBOT_INVENTORYSLOTS_H

#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"

// The bot's general slots: the backpack and plain bags, which take any loot (quivers, ammo pouches, soul, herb and
// other special bags do not count)
namespace WorldTravel
{
inline bool IsGeneralBag(Bag const* bag)
{
    return bag && bag->GetTemplate()->Class == ITEM_CLASS_CONTAINER &&
           bag->GetTemplate()->SubClass == ITEM_SUBCLASS_CONTAINER;
}

inline uint32 FreeGeneralSlots(Player* bot)
{
    uint32 free = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (IsGeneralBag(bag))
            free += bag->GetFreeSlots();
    }

    return free;
}

// An item in the backpack or a plain bag: destroying or selling it frees a slot that takes any loot
inline bool InGeneralSlot(Player* bot, Item const* item)
{
    uint8 const bagSlot = item->GetBagSlot();
    if (bagSlot == INVENTORY_SLOT_BAG_0)
        return item->GetSlot() >= INVENTORY_SLOT_ITEM_START && item->GetSlot() < INVENTORY_SLOT_ITEM_END;

    return IsGeneralBag(bot->GetBagByPos(bagSlot));
}

// A plain bag in the general slots, not equipped, with more slots than the smallest equipped bag: one the bot could
// not swap in yet (it lies inside the bag it would replace). The backpack is searched first.
inline Item* FindSpareBag(Player* bot, uint32 smallestBagSize)
{
    auto const isSpare = [smallestBagSize](Item const* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER &&
               proto->ContainerSlots > smallestBagSize;
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); isSpare(item))
            return item;

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (!IsGeneralBag(bag))
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (Item* item = bag->GetItemByPos(static_cast<uint8>(slot)); isSpare(item))
                return item;
    }

    return nullptr;
}

inline bool CarriesSpareBag(Player* bot, uint32 smallestBagSize) { return FindSpareBag(bot, smallestBagSize); }

// The server refuses to swap a bag in for the bag it lies in. Moves a spare bag held inside a bag to a free backpack
// slot, from where it can be equipped; true when one moved
inline bool MoveSpareBagToBackpack(Player* bot, uint32 smallestBagSize)
{
    Item const* const spare = FindSpareBag(bot, smallestBagSize);
    if (!spare || spare->GetBagSlot() == INVENTORY_SLOT_BAG_0)
        return false;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            continue;

        bot->SwapItem(spare->GetPos(), static_cast<uint16>((INVENTORY_SLOT_BAG_0 << 8) | slot));
        return bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) != nullptr;
    }

    return false;
}
}  // namespace WorldTravel

#endif
