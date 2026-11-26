/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_EQUIPACTION_H
#define PLAYERBOTS_EQUIPACTION_H

#include <vector>

#include "ChatHelper.h"
#include "InventoryAction.h"
#include "Item.h"
#include "ObjectGuid.h"

class FindItemVisitor;
class Item;
class PlayerbotAI;

class EquipAction : public InventoryAction
{
public:
    EquipAction(PlayerbotAI* botAI, std::string const name = "equip") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;
    void EquipItems(ItemIds ids);
    // The bag items the upgrade rule judged, each copy by its own suffix, equipped as those copies
    void EquipItems(std::vector<ObjectGuid> const& guids);
    std::vector<ObjectGuid> SelectInventoryItemsToEquip();

private:
    void EquipItem(FindItemVisitor* visitor);
    uint8 GetSmallestBagSlot();
    void EquipItem(Item* item);
};

class EquipUpgradesPacketAction : public EquipAction
{
public:
    explicit EquipUpgradesPacketAction(PlayerbotAI* botAI, std::string const name = "equip upgrades packet action") : EquipAction(botAI, name) {}

    bool Execute(Event event) override;
};

class EquipUpgradeAction : public EquipAction
{
public:
    explicit EquipUpgradeAction(PlayerbotAI* botAI, std::string const name = "equip upgrade") : EquipAction(botAI, name) {}

    bool Execute(Event event) override;
};

#endif
