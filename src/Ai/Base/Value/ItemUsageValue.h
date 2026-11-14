/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ITEMUSAGEVALUE_H
#define PLAYERBOTS_ITEMUSAGEVALUE_H

#include "NamedObjectContext.h"
#include "Value.h"

class Item;
class Player;
class PlayerbotAI;

struct ItemTemplate;
struct ParsedItemUsage
{
    uint32 itemId = 0;
    int32 randomPropertyId = 0;
};
enum ItemUsage : uint32
{
    ITEM_USAGE_NONE = 0,
    ITEM_USAGE_EQUIP = 1,
    ITEM_USAGE_REPLACE = 2,
    ITEM_USAGE_BAD_EQUIP = 3,
    ITEM_USAGE_BROKEN_EQUIP = 4,
    ITEM_USAGE_QUEST = 5,
    ITEM_USAGE_SKILL = 6,
    ITEM_USAGE_USE = 7,
    ITEM_USAGE_GUILD_TASK = 8,
    ITEM_USAGE_DISENCHANT = 9,
    ITEM_USAGE_AH = 10,
    ITEM_USAGE_KEEP = 11,
    ITEM_USAGE_VENDOR = 12,
    ITEM_USAGE_AMMO = 13,
    ITEM_USAGE_BANK = 14  // gear the bank rule keeps for a later level (BankRules.h); never sold
};

class ItemUsageValue : public CalculatedValue<ItemUsage>, public Qualified
{
public:
    ItemUsageValue(PlayerbotAI* botAI, std::string const name = "item usage") : CalculatedValue<ItemUsage>(botAI, name)
    {
    }

    ItemUsage Calculate() override;

protected:
    ItemUsage QueryItemUsageForEquip(ItemTemplate const* proto, int32 randomPropertyId = 0);
    ItemUsage QueryItemUsageForAmmo(ItemTemplate const* proto);
    ParsedItemUsage GetItemIdFromQualifier();

private:
    bool IsItemUsefulForQuest(Player* player, ItemTemplate const* proto);
    bool IsStartQuestItemUsefulFor(Player* player, ItemTemplate const* proto);
    bool IsItemNeededForSkill(ItemTemplate const* proto);
    bool IsItemUsefulForSkill(ItemTemplate const* proto);
    bool IsItemNeededForClassSpell(ItemTemplate const* proto);
    /** True for temporary weapon enhancements (stones, oils, poisons). */
    bool IsTemporaryWeaponEnchantment(ItemTemplate const* proto);
    bool HasItemsNeededForSpell(uint32 spellId, ItemTemplate const* proto);
    Item* CurrentItem(ItemTemplate const* proto);
    float CurrentStacks(ItemTemplate const* proto);
    float BetterStacks(ItemTemplate const* proto, std::string const usageType = "");

public:
    bool IsItemNeededForUsefullSpell(ItemTemplate const* proto, bool checkAllReagents = false);
    /** True if this poison is the highest item level of its family (use-spell SpellFamilyFlags). */
    bool IsBestPoison(ItemTemplate const* proto);

    static std::vector<uint32> SpellsUsingItem(uint32 itemId, Player* bot);
    static bool SpellGivesSkillUp(uint32 spellId, Player* bot);

    static std::string const GetConsumableType(ItemTemplate const* proto, bool hasMana);
    // Slots of the smallest equipped bag; 0 while a bag slot is empty
    static uint32 SmallestBagSize(Player* bot);
    // Worth in copper of count items with this usage (ItemWorthRules.h); sellMultiplier: what a vendor pays the bot
    // per copper of sell price. Quest and kept items, upgrades and the hearthstone are priceless. A bank item
    // (ITEM_USAGE_BANK) is worth keptWorthEach per item (BankMail::CarriedWorthEach), priceless while that is 0.
    static uint32 ItemWorth(ItemTemplate const* proto, ItemUsage usage, uint32 count, float sellMultiplier,
                            uint32 keptWorthEach = 0);
};

class ItemUpgradeValue : public ItemUsageValue
{
public:
    ItemUpgradeValue(PlayerbotAI* botAI, std::string const name = "item upgrade") : ItemUsageValue(botAI, name)
    {
    }

    ItemUsage Calculate() override;
};

#endif
