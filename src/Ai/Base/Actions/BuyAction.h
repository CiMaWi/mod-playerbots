/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BUYACTION_H
#define PLAYERBOTS_BUYACTION_H

#include "InventoryAction.h"

class FindItemVisitor;
class ObjectGuid;
class Item;
class Player;
class PlayerbotAI;

struct ItemTemplate;
struct VendorItem;
struct VendorItemData;
class Creature;

class BuyAction : public InventoryAction
{
public:
    BuyAction(PlayerbotAI* botAI) : InventoryAction(botAI, "buy") {}

    bool Execute(Event event) override;

private:
    // What one visit's gear purchases leave for its next vendor (BuyAction.cpp)
    struct GearVisit;

    // A bot that keeps its possessions buys from its spare gold, per equipment slot, the vendor's upgrade worth most
    // above its price, and wears it (gear WG5)
    void BuyGearByWorth(Creature* vendor, ObjectGuid vendorguid, VendorItemData const* tItems, GearVisit& visit);
    bool BuyItem(VendorItemData const* tItems, ObjectGuid vendorguid, ItemTemplate const* proto);
    /** True if the vendor has stock and the bot can pay the extended cost; gold is checked by the core. */
    bool CanAfford(VendorItem const* tItem, ItemTemplate const* proto, Creature* vendor) const;
    bool TradeItem(FindItemVisitor* visitor, int8 slot);
    bool TradeItem(Item const* item, int8 slot);
};

#endif
