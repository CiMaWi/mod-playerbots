/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EquipAction.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Event.h"
#include "ItemCountValue.h"
#include "ItemPackets.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "RandomItemMgr.h"
#include "StatsWeightCalculator.h"
#include "WorldLog.h"

namespace
{
// A weapon or armour piece in an equipment slot (the world log's gear equip row)
struct WornItem
{
    ObjectGuid guid;
    uint32 entry = 0;
    int32 randomPropertyId = 0;
    bool broken = false;
};
using WornItems = std::array<WornItem, EQUIPMENT_SLOT_END>;
constexpr uint8 NO_ROW_SLOT = EQUIPMENT_SLOT_END;

WornItems ReadWorn(Player* bot)
{
    WornItems worn;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
            continue;

        bool const broken =
            item->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0;
        worn[slot] = {item->GetGUID(), proto->ItemId, item->GetItemRandomPropertyId(), broken};
    }
    return worn;
}

bool IsWorn(WornItems const& worn, ObjectGuid guid)
{
    return std::any_of(worn.begin(), worn.end(),
                       [guid](WornItem const& item) { return item.entry && item.guid == guid; });
}

std::string_view EquipReason(Player* bot, WornItem const& worn)
{
    if (!worn.entry)
        return "empty_slot";

    if (worn.broken)
        return "broken";

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(worn.entry);
    if (proto && proto->Class == ITEM_CLASS_WEAPON && !sRandomItemMgr.CanEquipWeapon(proto, bot->getClass()))
        return "kind";

    if (proto && proto->Class == ITEM_CLASS_ARMOR &&
        !sRandomItemMgr.CanEquipArmorKind(proto, bot->getClass(), bot->GetLevel()))
        return "kind";

    return "upgrade";
}

// One gear equip row per weapon or armour piece the pass newly wears: its score, the score of what its slot held
// before the pass and the entries of up to two items that left the equipment with it (one that left a hand with no
// row of its own goes to the other hand's row: a two-hander over two weapons, a main hand moved to the off hand)
void LogEquips(Player* bot, WornItems const& before)
{
    WornItems const after = ReadWorn(bot);
    std::array<bool, EQUIPMENT_SLOT_END> row{};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        row[slot] = after[slot].entry && !IsWorn(before, after[slot].guid);

    std::array<std::array<uint32, 2>, EQUIPMENT_SLOT_END> off{};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (!before[slot].entry || IsWorn(after, before[slot].guid))
            continue;

        uint8 rowSlot = row[slot] ? slot : NO_ROW_SLOT;
        if (rowSlot == NO_ROW_SLOT && slot == EQUIPMENT_SLOT_MAINHAND && row[EQUIPMENT_SLOT_OFFHAND])
            rowSlot = EQUIPMENT_SLOT_OFFHAND;
        if (rowSlot == NO_ROW_SLOT && slot == EQUIPMENT_SLOT_OFFHAND && row[EQUIPMENT_SLOT_MAINHAND])
            rowSlot = EQUIPMENT_SLOT_MAINHAND;
        if (rowSlot == NO_ROW_SLOT)
            continue;

        std::array<uint32, 2>& left = off[rowSlot];
        if (!left[0])
            left[0] = before[slot].entry;
        else if (!left[1])
            left[1] = before[slot].entry;
    }

    StatsWeightCalculator calculator(bot);
    calculator.SetUpForEquipCompare();
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (!row[slot])
            continue;

        WornItem const& worn = before[slot];
        float const score = calculator.CalculateItem(after[slot].entry, after[slot].randomPropertyId);
        float const wornScore = worn.entry ? calculator.CalculateItem(worn.entry, worn.randomPropertyId) : 0.0f;
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Gear, "equip", EquipReason(bot, worn), score,
                           {{"worn", wornScore},
                            {"off", static_cast<float>(off[slot][0])},
                            {"off2", static_cast<float>(off[slot][1])}},
                           after[slot].entry);
    }
}
}  // namespace

bool EquipAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemIds ids = chat->parseItems(text);
    EquipItems(ids);
    return true;
}

void EquipAction::EquipItems(ItemIds ids)
{
    for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
    {
        FindItemByIdVisitor visitor(*i);
        EquipItem(&visitor);
    }
}

void EquipAction::EquipItems(std::vector<ObjectGuid> const& guids)
{
    bool const logged = !guids.empty() && WorldLog::Active();
    WornItems before;
    if (logged)
        before = ReadWorn(bot);

    _passCandidates = &guids;
    for (ObjectGuid const& guid : guids)
        if (Item* item = bot->GetItemByGuid(guid))
            EquipItem(item);
    _passCandidates = nullptr;

    if (logged)
        LogEquips(bot, before);
}

// Anything fills an empty slot. An equip asked for (chat command, outfit, script) replaces on any higher score. In
// the bot's own pass anything that scores above 0 replaces a broken item, as the upgrade rule counts it, and a worn
// item this pass equipped yields to any better candidate: the pass judged every candidate against the slots as it
// found them and equips them from the lowest score up, so the best of them stays worn.
bool EquipAction::Replaces(float score, Item const* worn, float wornScore) const
{
    if (!worn)
        return true;

    if (!_passCandidates)
        return score > wornScore;

    if (worn->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && worn->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0)
        return score > 0.0f;

    if (std::find(_passCandidates->begin(), _passCandidates->end(), worn->GetGUID()) != _passCandidates->end())
        return score > wornScore;

    return ItemUsageValue::BeatsWornScore(score, wornScore);
}

// Return bagslot with smalest bag.
uint8 EquipAction::GetSmallestBagSlot()
{
    int8 curBag = 0;
    uint32 curSlots = 0;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag const* const pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (pBag)
        {
            if (curBag > 0 && curSlots < pBag->GetBagSize())
                continue;

            curBag = bag;
            curSlots = pBag->GetBagSize();
        }
        else
            return bag;
    }

    return curBag;
}

void EquipAction::EquipItem(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    if (!items.empty())
        EquipItem(*items.begin());
}

void EquipAction::EquipItem(Item* item)
{
    uint8 bagIndex = item->GetBagSlot();
    uint8 slot = item->GetSlot();
    ItemTemplate const* itemProto = item->GetTemplate();
    uint32 itemId = itemProto->ItemId;
    uint8 invType = itemProto->InventoryType;

    // Handle ammunition separately
    if (invType == INVTYPE_AMMO)
    {
        bot->SetAmmo(itemId);
        std::ostringstream out;
        out << "equipping " << chat->FormatItem(itemProto);
        botAI->TellMaster(out);
        return;
    }

    // Handle bags first
    bool equippedBag = false;
    if (itemProto->Class == ITEM_CLASS_CONTAINER)
    {
        // Attempt to equip as a bag
        uint8 newBagSlot = GetSmallestBagSlot();

        if (newBagSlot > 0)
        {
            uint16 src = ((bagIndex << 8) | slot);
            uint16 dst = ((INVENTORY_SLOT_BAG_0 << 8) | newBagSlot);
            bot->SwapItem(src, dst);
            equippedBag = true;
        }
    }

    // If we didn't equip as a bag, try to equip as gear
    if (!equippedBag)
    {
        // Ranged weapons aren't handled by the rest of the weapon equip logic
        // Handle them early here to avoid issues.
        if (invType == INVTYPE_RANGED || invType == INVTYPE_THROWN || invType == INVTYPE_RANGEDRIGHT)
        {
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << uint8(EQUIPMENT_SLOT_RANGED);

            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

            std::ostringstream out;
            out << "Equipping " << chat->FormatItem(itemProto) << " in ranged slot";
            botAI->TellMaster(out);
            return;
        }

        uint8 dstSlot = botAI->FindEquipSlot(itemProto, NULL_SLOT, true);

        // Check if the item is a weapon and whether the bot can dual wield or use Titan Grip
        bool isWeapon = (itemProto->Class == ITEM_CLASS_WEAPON);
        bool canTitanGrip = bot->CanTitanGrip();
        bool canDualWield = bot->CanDualWield();

        bool isTwoHander = (invType == INVTYPE_2HWEAPON);
        bool isValidTGWeapon = false;
        if (canTitanGrip && isTwoHander)
        {
            // Titan Grip-valid 2H weapon subclasses: Axe2, Mace2, Sword2
            isValidTGWeapon = (itemProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
        }

        // Check if the main hand currently has a 2H weapon equipped
        Item* currentMHItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool have2HWeaponEquipped = (currentMHItem && currentMHItem->GetTemplate()->InventoryType == INVTYPE_2HWEAPON);

        // bool canDualWieldOrTG = (canDualWield || (canTitanGrip && isTwoHander));
        bool canDualWieldOrTG = (canDualWield || isTwoHander);

        // If this is a weapon and we can dual wield or Titan Grip, check if we can improve main/off-hand setup
        if (isWeapon && canDualWieldOrTG)
        {
            // Fetch current main hand and offhand items
            Item* mainHandItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item* offHandItem  = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

            // Set up the stats calculator once and reuse results for performance
            StatsWeightCalculator calculator(bot);
            calculator.SetUpForEquipCompare();

            // Calculate item scores once and store them; an empty hand takes any weapon (Replaces)
            float newItemScore = calculator.CalculateItem(itemId, item->GetItemRandomPropertyId());
            float mainHandScore = mainHandItem ? calculator.CalculateItem(mainHandItem->GetTemplate()->ItemId,
                                                                          mainHandItem->GetItemRandomPropertyId())
                                               : std::numeric_limits<float>::lowest();
            float offHandScore = offHandItem ? calculator.CalculateItem(offHandItem->GetTemplate()->ItemId,
                                                                        offHandItem->GetItemRandomPropertyId())
                                             : std::numeric_limits<float>::lowest();

            // Determine where this weapon can go
            bool canGoMain = (invType == INVTYPE_WEAPON ||
                              invType == INVTYPE_WEAPONMAINHAND ||
                              isTwoHander);

            bool canTGOff = false;
            if (canTitanGrip && isTwoHander && isValidTGWeapon)
                canTGOff = true;

            bool canGoOff = (invType == INVTYPE_WEAPON ||
                             invType == INVTYPE_WEAPONOFFHAND ||
                             canTGOff);

            // Check if the main hand item can go to offhand if needed
            bool mainHandCanGoOff = false;
            if (mainHandItem)
            {
                ItemTemplate const* mhProto = mainHandItem->GetTemplate();
                bool mhIsValidTG = false;
                if (canTitanGrip && mhProto->InventoryType == INVTYPE_2HWEAPON)
                {
                    mhIsValidTG = (mhProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
                }

                mainHandCanGoOff = (mhProto->InventoryType == INVTYPE_WEAPON ||
                                    mhProto->InventoryType == INVTYPE_WEAPONOFFHAND ||
                                    (mhProto->InventoryType == INVTYPE_2HWEAPON && mhIsValidTG));
            }

            // Priority 1: Replace main hand if the new weapon is an upgrade over it
            // and if conditions allow (e.g. no conflicting 2H logic)
            bool betterThanMH = Replaces(newItemScore, mainHandItem, mainHandScore);
            // If a one-handed weapon is better, we can still use it instead of a two-handed weapon
            bool mhConditionOK = (invType != INVTYPE_2HWEAPON ||
                      (isTwoHander && !canTitanGrip) ||
                      (canTitanGrip && isValidTGWeapon));

            if (canGoMain && betterThanMH && mhConditionOK)
            {
                // Equip new weapon in main hand
                {
                    WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid newItemGuid = item->GetGUID();
                    eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_MAINHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
                }

                // Try moving old main hand weapon to offhand if beneficial
                if (mainHandItem && mainHandCanGoOff && Replaces(mainHandScore, offHandItem, offHandScore))
                {
                    ItemTemplate const* oldMHProto = mainHandItem->GetTemplate();

                    WorldPacket offhandPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid oldMHGuid = mainHandItem->GetGUID();
                    offhandPacket << oldMHGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(offhandPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

                    std::ostringstream moveMsg;
                    moveMsg << "Main hand upgrade found. Moving " << chat->FormatItem(oldMHProto) << " to offhand";
                    botAI->TellMaster(moveMsg);
                }

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in main hand";
                botAI->TellMaster(out);
                return;
            }

            // Priority 2: If not better than main hand, check if better than offhand
            else if (canGoOff && Replaces(newItemScore, offHandItem, offHandScore))
            {
                // Equip in offhand
                WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                ObjectGuid newItemGuid = item->GetGUID();
                eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                nicePacket.Read();
                bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in offhand";
                botAI->TellMaster(out);
                return;
            }
            else
            {
                // No improvement, do nothing
                return;
            }
        }

        // If not a special dual-wield/TG scenario or no improvement found, fall back to original logic
        if (dstSlot == EQUIPMENT_SLOT_FINGER1 ||
            dstSlot == EQUIPMENT_SLOT_TRINKET1 ||
            (dstSlot == EQUIPMENT_SLOT_MAINHAND && canDualWield &&
                ((invType != INVTYPE_2HWEAPON && !have2HWeaponEquipped) || (canTitanGrip && isValidTGWeapon))))
        {
            // Handle ring/trinket dual-slot logic
            Item* const equippedItems[2] = {
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot),
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot + 1)
            };

            if (equippedItems[0])
            {
                if (equippedItems[1])
                {
                    // Both slots are full - pick the worst item to replace, but only if new item is better
                    StatsWeightCalculator calc(bot);
                    calc.SetUpForEquipCompare();

                    // Calculate new item score with random properties
                    int32 newItemRandomProp = item->GetItemRandomPropertyId();
                    float newItemScore = calc.CalculateItem(itemId, newItemRandomProp);

                    // Calculate equipped items scores with random properties
                    int32 firstRandomProp = equippedItems[0]->GetItemRandomPropertyId();
                    int32 secondRandomProp = equippedItems[1]->GetItemRandomPropertyId();
                    float firstItemScore = calc.CalculateItem(equippedItems[0]->GetTemplate()->ItemId, firstRandomProp);
                    float secondItemScore = calc.CalculateItem(equippedItems[1]->GetTemplate()->ItemId, secondRandomProp);

                    // Determine which slot (if any) should be replaced
                    bool betterThanFirst = Replaces(newItemScore, equippedItems[0], firstItemScore);
                    bool betterThanSecond = Replaces(newItemScore, equippedItems[1], secondItemScore);

                    // Early return if new item is not better than either equipped item
                    if (!betterThanFirst && !betterThanSecond)
                        return;

                    if (betterThanFirst && betterThanSecond)
                    {
                        // New item is better than both - replace the worse of the two equipped items
                        if (firstItemScore > secondItemScore)
                            dstSlot++; // Replace second slot (worse)
                        // else: keep dstSlot as-is (replace first slot)
                    }
                    else if (betterThanSecond)
                        dstSlot++; // Only better than second slot - replace it
                }
                else
                {
                    // Second slot empty, use it
                    dstSlot++;
                }
            }
        }

        // Equip the item in the chosen slot
        {
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << dstSlot;
            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
        }
    }

    std::ostringstream out;
    out << "Equipping " << chat->FormatItem(itemProto);
    botAI->TellMaster(out);
}

std::vector<ObjectGuid> EquipAction::SelectInventoryItemsToEquip()
{
    CollectItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    // Every candidate is judged against the worn items as they are before this pass, so two may claim one slot:
    // equipped from the lowest score up, the best of them ends worn
    StatsWeightCalculator calculator(bot);
    calculator.SetUpForEquipCompare();
    std::vector<std::pair<float, ObjectGuid>> candidates;
    std::unordered_set<std::string> judged;  // one copy per item and suffix
    for (auto i = visitor.items.begin(); i != visitor.items.end(); ++i)
    {
        Item* item = *i;
        if (!item)
            continue;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!itemTemplate)
            continue;

        //TODO Expand to Glyphs and Gems, that can be placed in equipment
        //Pre-filter non-equipable items
        if (itemTemplate->InventoryType == INVTYPE_NON_EQUIP)
            continue;

        int32 randomProperty = item->GetItemRandomPropertyId();
        uint32 itemId = item->GetTemplate()->ItemId;
        std::string itemUsageParam;
        if (randomProperty != 0)
            itemUsageParam = std::to_string(itemId) + "," + std::to_string(randomProperty);
        else
            itemUsageParam = std::to_string(itemId);

        if (!judged.insert(itemUsageParam).second)
            continue;

        ItemUsage usage = AI_VALUE2(ItemUsage, "item upgrade", itemUsageParam);
        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP)
            candidates.emplace_back(calculator.CalculateItem(itemId, randomProperty), item->GetGUID());
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<ObjectGuid> items;
    items.reserve(candidates.size());
    for (auto const& candidate : candidates)
        items.push_back(candidate.second);
    return items;
}

bool EquipUpgradesPacketAction::Execute(Event event)
{
    if (!sPlayerbotAIConfig.AutoEquipUpgradeLoot && !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // The PvP loadout is worn: no upgrade replaces a worn item or equips a displaced PvE item
    if (PvpGearMgr::Instance().IsWorn(bot))
        return false;
    std::string const source = event.GetSource();
    if (source == "trade status")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        uint32 status;
        p >> status;

        if (status != TRADE_STATUS_TRADE_ACCEPT)
            return false;
    }

    else if (source == "item push result")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        ObjectGuid playerGuid;
        uint32 received, created, sendChatMessage, itemSlot, itemId;
        uint8 bagSlot;

        p >> playerGuid;
        p >> received;
        p >> created;
        p >> sendChatMessage;
        p >> bagSlot;
        p >> itemSlot;
        p >> itemId;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
        if (item->InventoryType == INVTYPE_NON_EQUIP)
            return false;
    }

    std::vector<ObjectGuid> const items = SelectInventoryItemsToEquip();
    EquipItems(items);
    return true;
}

bool EquipUpgradeAction::Execute(Event /*event*/)
{
    if (PvpGearMgr::Instance().IsWorn(bot))
        return false;

    std::vector<ObjectGuid> const items = SelectInventoryItemsToEquip();
    EquipItems(items);
    return true;
}
