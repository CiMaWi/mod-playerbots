/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DESTROYITEMACTION_H
#define PLAYERBOTS_DESTROYITEMACTION_H

#include "InventoryAction.h"

class FindItemVisitor;
class Item;
class PlayerbotAI;

class DestroyItemAction : public InventoryAction
{
public:
    DestroyItemAction(PlayerbotAI* botAI, std::string const name = "destroy") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;

protected:
    // Destroys every match. An unsaved Item is deleted here, so callers must not hold Item* across this.
    void DestroyItem(FindItemVisitor* visitor);
    // Destroys this item unless it is worn PvP gear or displaced PvE gear; true when it is gone
    bool DestroyOne(Item* item);
};

class SmartDestroyItemAction : public DestroyItemAction
{
public:
    SmartDestroyItemAction(PlayerbotAI* botAI) : DestroyItemAction(botAI, "smart destroy") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    // Full bags of a persistent bot: destroys the lowest-worth item in a general slot when loot worth lootWorth beats
    // it (WorldTravel::ShouldDestroyFor) and records an Inventory event; true when a slot was freed
    bool DestroyForLoot(uint32 lootWorth);

private:
    bool DestroyUntilBagSpace(std::vector<uint32> const& itemIds);
};

#endif
