/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUsageValue.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

#include "AiFactory.h"
#include "BankRulesInput.h"
#include "ChatHelper.h"
#include "GuildTaskMgr.h"
#include "Item.h"
#include "ItemDemand.h"
#include "ItemUseIndex.h"
#include "ItemUseKinds.h"
#include "ItemWorthRules.h"
#include "LootObjectStack.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "ProfessionRules.h"
#include "QuestAction.h"
#include "RandomItemMgr.h"
#include "ServerFacade.h"
#include "SkillPathLive.h"
#include "StatsWeightCalculator.h"
#include "Upkeep.h"
#include "UpkeepIndex.h"
#include "UseBuffsStrategy.h"

static bool IsRoguePoison(ItemTemplate const* proto);

// What a bot without demand keeps two stacks of (item-use WI19: the index's kind of the item): food and drink, heal,
// mana; a drink and a mana item only for a bot with mana. Its kind and sub; Kind::Unclassified: none of these
static std::pair<ItemUse::Kind, std::uint8_t> StockKindOf(uint32 itemId, bool hasMana)
{
    for (ItemUse::Record const& record : ItemUseIndex::Get().OfItem(itemId))
    {
        ItemUse::Classified const classified = ItemUse::Classify(record);
        ItemUse::Sub const sub = static_cast<ItemUse::Sub>(classified.sub);
        bool const stocked = classified.kind == ItemUse::Kind::Heal ||
                             (classified.kind == ItemUse::Kind::FoodDrink && (sub != ItemUse::Sub::Drink || hasMana)) ||
                             (classified.kind == ItemUse::Kind::Mana && sub == ItemUse::Sub::Mana && hasMana);
        if (stocked)
            return {classified.kind, classified.sub};
    }
    return {ItemUse::Kind::Unclassified, 0};
}

namespace
{
constexpr uint32 ITEM_HEARTHSTONE = 6948;
}  // namespace

ItemUsage ItemUsageValue::Calculate()
{
    ParsedItemUsage const parsed = GetItemIdFromQualifier();
    uint32 itemId = parsed.itemId;
    uint32 randomPropertyId = parsed.randomPropertyId;
    if (!itemId)
        return ITEM_USAGE_NONE;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return ITEM_USAGE_NONE;

    bool const realMaster = IsRealPlayer(botAI->GetMaster());
    if (realMaster)
    {
        if (IsItemUsefulForSkill(proto) || IsItemNeededForSkill(proto))
            return ITEM_USAGE_SKILL;

        if ((proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_MISC ||
             proto->Class == ITEM_CLASS_REAGENT) &&
            IsItemNeededForClassSpell(proto))
            return ITEM_USAGE_KEEP;
    }
    else
    {
        // A persistent bot with a skill path takes the usage of a material from the path (professions WR12): a
        // material the path uses is looked for until the bot holds its units and kept after, banked while the bot's
        // skill is short of the step that uses it (the bank rule's carried keeps, below)
        SkillPath::Path const* const path = botAI->KeepsPossessions() ? Planner::KeptPath(botAI) : nullptr;
        SkillPath::Material const* const material = path ? SkillPath::MaterialOf(*path, itemId) : nullptr;
        if (material && bot->GetSkillValue(material->skill) >= material->dueSkill)
            return bot->GetItemCount(itemId, false) < material->used ? ITEM_USAGE_SKILL : ITEM_USAGE_KEEP;

        if (KeepsByCount(proto, path))
        {
            // The bank asks with its own units counted too, so its moves never change the answer (",bank", NR12a)
            float stacks = parsed.countBank ? static_cast<float>(bot->GetItemCount(itemId, true)) /
                                                  static_cast<float>(proto->GetMaxStackSize())
                                            : CurrentStacks(proto);
            if (stacks < 1)
                return ITEM_USAGE_SKILL;  // Buy more.
            if (stacks <= KEPT_STACKS)
                return ITEM_USAGE_KEEP;
        }
    }

    if (proto->Class == ITEM_CLASS_KEY)
        return ITEM_USAGE_USE;

    const uint32_t maxCount = proto->MaxCount;

    // A persistent bot's consumables have one keeper per kind (upkeep WU37b, WU.27): upkeep's stock rule (WU28) for
    // ammo, reagents and pet food, up to the stock target its shopping list buys by, and item use's demand (WI9a,
    // ItemDemand.h) for the others, food, water and poison included, up to the wanted stock its shopping list buys
    // by. Meat that is food and pet food has both: the stock rule's pet food verdict, then demand's food verdict. What
    // neither keeps falls through to be sold: a consumable either rule covers skips the generic consumable checks
    Planner::StockDecision const stock = !realMaster && proto->Class == ITEM_CLASS_CONSUMABLE
                                             ? Planner::StockFor(bot, botAI, proto)
                                             : Planner::StockDecision{};
    if (stock.decided && stock.kept)
        return stock.buy ? ITEM_USAGE_USE : ITEM_USAGE_KEEP;

    bool const demandBot = ItemUse::DemandAnswers(botAI);
    if (demandBot && proto->Class == ITEM_CLASS_CONSUMABLE)
    {
        ItemUse::DemandVerdict const verdict = ItemUse::DemandVerdictOf(botAI, itemId);
        if (verdict == ItemUse::DemandVerdict::Use)
            return maxCount == 0 || bot->GetItemCount(itemId, false) < maxCount ? ITEM_USAGE_USE : ITEM_USAGE_KEEP;
        if (verdict == ItemUse::DemandVerdict::Keep)
            return ITEM_USAGE_KEEP;
    }

    if (!stock.decided && !demandBot && proto->Class == ITEM_CLASS_CONSUMABLE &&
        (maxCount == 0 || bot->GetItemCount(itemId, false) < maxCount))
    {
        bool const hasMana = bot->GetPower(POWER_MANA) > 0;
        if (StockKindOf(itemId, hasMana).first != ItemUse::Kind::Unclassified && bot->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            float stacks = BetterStockStacks(proto, hasMana);
            if (stacks < 2)
            {
                stacks += CurrentStacks(proto);

                if (stacks < 2)
                    return ITEM_USAGE_USE;  // Buy some to get to 2 stacks
                else if (stacks < 3)        // Keep the item if less than 3 stacks
                    return ITEM_USAGE_KEEP;
            }
        }
    }

    if (bot->GetGuildId() && GuildTaskMgr::instance().IsGuildTaskItem(itemId, bot->GetGuildId()))
        return ITEM_USAGE_GUILD_TASK;

    // Starters the bot can take stay in the bags and are used from there; the core deletes the
    // starting item on accept unless the quest also requires it, so they are never equipped.
    if (IsStartQuestItemUsefulFor(bot, proto))
        return ITEM_USAGE_QUEST;

    ItemUsage equip = QueryItemUsageForEquip(proto, randomPropertyId);
    if (equip != ITEM_USAGE_NONE)
        return equip;

    // A weapon the bot will learn to use at a weapon master stays in the bags (WU15a): its visit needs it carried
    if (!realMaster && Planner::WeaponToLearn(bot, proto))
        return ITEM_USAGE_KEEP;

    // Gear the bank rule keeps for a later level, read from what the bot remembers (BankMail::RefreshCarriedKeeps):
    // the only source of ITEM_USAGE_BANK, and no keep decision here. Never for a bot with a real master
    bool const keptForLater = !realMaster && BankMail::CarriedWorthEach(botAI, itemId);

    // Get item instance to check if it's soulbound
    Item* item = bot->GetItemByEntry(proto->ItemId);
    bool isSoulbound = item && item->IsSoulBound();

    if (!keptForLater && (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
        botAI->HasSkill(SKILL_ENCHANTING) &&
        proto->Quality >= ITEM_QUALITY_UNCOMMON)
    {
        // Retrieve the bot's Enchanting skill level
        uint32 enchantingSkill = bot->GetSkillValue(SKILL_ENCHANTING);

        // Only disenchant if skilled enough and binding allows it. A persistent bot breaks down any unwanted green or
        // blue, bound or not, but only when its materials are worth more than the vendor pays for it (professions
        // WR12, WR14d: the outlet in plain copper, as the material rules price the sale given up)
        bool const persistent = botAI->KeepsPossessions();
        if (enchantingSkill >= proto->RequiredDisenchantSkill &&
            (persistent || proto->Bonding == BIND_WHEN_PICKED_UP ||
             (proto->Bonding == BIND_WHEN_EQUIPPED && isSoulbound)) &&
            (!persistent || WorldTravel::DisenchantPays(DisenchantOutletOf(botAI, proto).worthEach,
                                                        ItemWorth(proto, ITEM_USAGE_VENDOR, 1, 1.0f))))
            return ITEM_USAGE_DISENCHANT;
    }

    Player* master = botAI->GetMaster();
    bool botNeedsItemForQuest = IsItemUsefulForQuest(bot, proto);
    bool masterNeedsItemForQuest = master && sPlayerbotAIConfig.SyncQuestWithPlayer && IsItemUsefulForQuest(master, proto);

    // Identify the source of loot
    LootObject lootObject = AI_VALUE(LootObject, "loot target");

    // Get GUID of loot source
    ObjectGuid lootGuid = lootObject.guid;

    // Check if loot source is an item
    bool isLootFromItem = lootGuid.IsItem();

    // If the loot is from an item in the bot’s bags, ignore syncQuestWithPlayer
    if (isLootFromItem && botNeedsItemForQuest)
        return ITEM_USAGE_QUEST;

    // If this is not a SelfBot acting alone and the master needs this quest item, defer to the master
    if (!IsSelfBot(bot) && masterNeedsItemForQuest)
        return ITEM_USAGE_NONE;

    // If the bot itself needs the item for a quest, allow looting
    if (botNeedsItemForQuest)
        return ITEM_USAGE_QUEST;

    // If the bot has the exact quest requirement amount, keep it to prevent a sell/buy cycle
    for (auto const& questPair : botAI->GetCurrentQuestsRequiringItemId(proto->ItemId))
    {
        Quest const* quest = questPair.first;
        if (!quest)
            continue;

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
        {
            if (quest->RequiredItemId[i] != proto->ItemId)
                continue;

            uint32 currentCount = AI_VALUE2(uint32, "item count", proto->Name1);
            uint32 requiredCount = quest->RequiredItemCount[i];

            if (currentCount == requiredCount)
                return ITEM_USAGE_KEEP;  // Have exact amount needed, keep it
        }
    }

    if (keptForLater)
        return ITEM_USAGE_BANK;

    if (proto->Class == ITEM_CLASS_PROJECTILE && bot->CanUseItem(proto) == EQUIP_ERR_OK)
    {
        ItemUsage ammoUsage = QueryItemUsageForAmmo(proto);
        if (ammoUsage != ITEM_USAGE_NONE)
            return ammoUsage;
    }

    // Keep only the best stone/oil per weapon and the best poison per family; obsolete or
    // mismatched enhancements fall through to VENDOR/AH.
    if (!stock.decided && !demandBot && proto->Class == ITEM_CLASS_CONSUMABLE && IsTemporaryWeaponEnchantment(proto) &&
        bot->CanUseItem(proto) == EQUIP_ERR_OK)
    {
        // Rogue poisons only: stones/oils share consumable subclass 8 but use family-less spells,
        // so they take the weapon branch below instead of being sold as obsolete poisons.
        if (IsRoguePoison(proto))
        {
            if (IsBestPoison(proto))
                return ITEM_USAGE_KEEP;
        }
        else
        {
            // the best for a hand is what the buff strategy picks for it (item-use WI19: one owner of the pick)
            std::array<std::uint32_t, 2> const picks = ItemUse::CoatingPicksOf(botAI);
            bool const isBest = picks[0] == proto->ItemId || picks[1] == proto->ItemId;
            if (isBest)
                return CurrentStacks(proto) < 1 ? ITEM_USAGE_USE : ITEM_USAGE_KEEP;
        }
    }

    // Need to add something like free bagspace or item value.
    if (proto->SellPrice > 0)
    {
        if (proto->Quality >= ITEM_QUALITY_NORMAL && !isSoulbound && proto->Bonding != BIND_WHEN_PICKED_UP)
            return ITEM_USAGE_AH;
        else
            return ITEM_USAGE_VENDOR;
    }

    return ITEM_USAGE_NONE;
}

bool ItemUsageValue::BeatsWornScore(float score, float wornScore)
{
    return score > std::max(wornScore, 0.0f) * sPlayerbotAIConfig.EquipUpgradeThreshold;
}

ItemUsage ItemUsageValue::QueryItemUsageForEquip(ItemTemplate const* itemProto, int32 randomPropertyId)
{
    if (bot->CanUseItem(itemProto) != EQUIP_ERR_OK)
        return ITEM_USAGE_NONE;

    if (itemProto->InventoryType == INVTYPE_NON_EQUIP)
        return ITEM_USAGE_NONE;

    Item* pItem = Item::CreateItem(itemProto->ItemId, 1, bot);
    if (!pItem)
        return ITEM_USAGE_NONE;

    uint16 dest;
    InventoryResult result = botAI->CanEquipItem(NULL_SLOT, dest, pItem, true, true);
    pItem->RemoveFromUpdateQueueOf(bot);
    delete pItem;

    if (result != EQUIP_ERR_OK && result != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
        return ITEM_USAGE_NONE;

    // Check if unique items are equipped or not
    bool needToCheckUnique = result == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS ||
         itemProto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE);

    // A worn copy makes a second one unwearable (the core refuses it); a copy in the bags or the bank does not
    if (needToCheckUnique && bot->HasItemOrGemWithIdEquipped(itemProto->ItemId, 1))
        return ITEM_USAGE_NONE;

    if (itemProto->Class == ITEM_CLASS_QUIVER && bot->getClass() != CLASS_HUNTER)
        return ITEM_USAGE_NONE;

    if (itemProto->Class == ITEM_CLASS_CONTAINER)
    {
        if (itemProto->SubClass != ITEM_SUBCLASS_CONTAINER)
            return ITEM_USAGE_NONE;  // Todo add logic for non-bag containers. We want to look at professions/class and
                                     // only replace if non-bag is larger than bag.
        if (SmallestBagSize(bot) >= itemProto->ContainerSlots)
            return ITEM_USAGE_NONE;

        return ITEM_USAGE_EQUIP;
    }

    if (itemProto->Class == ITEM_CLASS_WEAPON && itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MISC)
        return ITEM_USAGE_NONE;

    bool shouldEquip = false;
    // uint32 statWeight = sRandomItemMgr.GetLiveStatWeight(bot, itemProto->ItemId);
    StatsWeightCalculator calculator(bot);
    calculator.SetUpForEquipCompare();

    float itemScore = calculator.CalculateItem(itemProto->ItemId, randomPropertyId);
    bool const usefulStat = calculator.HasUsefulStat();

    if (itemScore > 0)
        shouldEquip = true;

    if (itemProto->Class == ITEM_CLASS_WEAPON && !sRandomItemMgr.CanEquipWeapon(itemProto, bot->getClass()))
        shouldEquip = false;
    if (itemProto->Class == ITEM_CLASS_ARMOR &&
        !sRandomItemMgr.CanEquipArmorKind(itemProto, bot->getClass(), bot->GetLevel()))
        shouldEquip = false;

    uint8 possibleSlots = 1;
    uint8 dstSlot = botAI->FindEquipSlot(itemProto, NULL_SLOT, true);
    // Check if dest wasn't set correctly by CanEquipItem and use FindEquipSlot instead
    // This occurs with unique items that are already in the bots bags when CanEquipItem is called
    if (dest == 0 && dstSlot != NULL_SLOT)
    {
        // Construct dest from dstSlot
        dest = (INVENTORY_SLOT_BAG_0 << 8) | dstSlot;
    }

    if (dstSlot == EQUIPMENT_SLOT_FINGER1 || dstSlot == EQUIPMENT_SLOT_TRINKET1)
        possibleSlots = 2;

    // Check weapon case separately to keep things a bit cleaner
    bool have2HWeapon = false;
    bool isValidTGWeapon = false;

    if (dstSlot == EQUIPMENT_SLOT_MAINHAND)
    {
        Item* currentWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        have2HWeapon = currentWeapon && currentWeapon->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;

        // Determine if the new weapon is a valid Titan Grip weapon
        isValidTGWeapon = (itemProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                           itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                           itemProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);

        // If the bot can Titan Grip, ignore any 2H weapon that isn't a 2H sword, mace, or axe.
        // If this weapon is 2H but not one of the valid TG weapon types, do not equip it at all.
        if (bot->CanTitanGrip() && itemProto->InventoryType == INVTYPE_2HWEAPON && !isValidTGWeapon)
            return ITEM_USAGE_NONE;

        // Now handle the logic for equipping and possible offhand slots
        // If the bot can Dual Wield and:
        // - The weapon is a one-hander (not 2H, not main-hand only) and we currently don't have a 2H weapon equipped
        // OR
        // - The bot can Titan Grip and it is a valid TG weapon
        // Then we can consider the offhand slot as well.
        if (bot->CanDualWield() && ((itemProto->InventoryType != INVTYPE_2HWEAPON &&
                                     itemProto->InventoryType != INVTYPE_WEAPONMAINHAND && !have2HWeapon) ||
                                    (bot->CanTitanGrip() && isValidTGWeapon)))
        {
            possibleSlots = 2;
        }
    }

    for (uint8 i = 0; i < possibleSlots; i++)
    {
        Item* oldItem = bot->GetItemByPos(dest + i);

        // No item equipped: an unfit item fills the slot only for a stat that serves the spec, or when no stat
        // carries a weight (a white chest piece on a naked bot); one of only foreign stats stays in the bag
        if (!oldItem)
        {
            if (shouldEquip)
                return ITEM_USAGE_EQUIP;
            if (usefulStat)
                return ITEM_USAGE_BAD_EQUIP;
            return ITEM_USAGE_NONE;
        }

        ItemTemplate const* oldItemProto = oldItem->GetTemplate();
        int32 const oldRandomPropertyId = oldItem->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID);
        float oldScore = calculator.CalculateItem(oldItemProto->ItemId, oldRandomPropertyId);

        // Bigger quiver
        if (itemProto->Class == ITEM_CLASS_QUIVER)
        {
            if (!oldItem || oldItemProto->ContainerSlots < itemProto->ContainerSlots)
                return ITEM_USAGE_EQUIP;
            else
                return ITEM_USAGE_NONE;
        }

        // A fitting item must beat the worn one's score times the threshold, whatever the worn item is; a worn
        // score at or below 0 counts as an empty slot. The same item with the same suffix never replaces itself.
        bool const sameItem = itemProto->ItemId == oldItemProto->ItemId && randomPropertyId == oldRandomPropertyId;
        bool const shouldEquipInSlot = shouldEquip && !sameItem && BeatsWornScore(itemScore, oldScore);

        Item* item = CurrentItem(itemProto);
        bool itemIsBroken =
            item && item->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0;
        bool oldItemIsBroken =
            oldItem->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && oldItem->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0;

        if (shouldEquipInSlot)
        {
            switch (itemProto->Class)
            {
                case ITEM_CLASS_ARMOR:
                    if (oldItemProto->SubClass <= itemProto->SubClass)
                    {
                        // Need to add some logic to check second slot before returning, but as it happens, both of
                        // these return vals will result in an attempted equip action so it wouldn't have much effect
                        // currently
                        if (itemIsBroken && !oldItemIsBroken)
                            return ITEM_USAGE_BROKEN_EQUIP;
                        else
                            return ITEM_USAGE_REPLACE;
                    }
                    // A lower-subclass armor piece is handled by the default case below.
                    [[fallthrough]];
                default:
                {
                    if (itemIsBroken && !oldItemIsBroken)
                        return ITEM_USAGE_BROKEN_EQUIP;
                    else
                        return ITEM_USAGE_EQUIP;
                }
            }
        }

        // Item is not better but current item is broken and new one is not: a broken worn item counts as none.
        if (shouldEquip && oldItemIsBroken && !itemIsBroken)
            return ITEM_USAGE_EQUIP;
    }
    return ITEM_USAGE_NONE;
}

ItemUsage ItemUsageValue::QueryItemUsageForAmmo(ItemTemplate const* proto)
{
    if (bot->getClass() != CLASS_HUNTER && bot->getClass() != CLASS_ROGUE && bot->getClass() != CLASS_WARRIOR)
        return ITEM_USAGE_NONE;

    Item* rangedWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    uint32 requiredSubClass = 0;

    if (rangedWeapon)
    {
        switch (rangedWeapon->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:
                requiredSubClass = ITEM_SUBCLASS_BULLET;
                break;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                requiredSubClass = ITEM_SUBCLASS_ARROW;
                break;
        }
    }

    // Ensure the item is the correct ammo type for the equipped ranged weapon
    if (proto->SubClass == requiredSubClass)
    {
        uint32 currentAmmoId = Upkeep::EquippedAmmoId(bot);

        // Check if the bot has an ammo type assigned (and some of it left)
        if (currentAmmoId == 0)
            return ITEM_USAGE_EQUIP;  // Equip the ammo if no ammo
        // Compare new ammo vs current equipped ammo
        ItemTemplate const* currentAmmoProto = sObjectMgr->GetItemTemplate(currentAmmoId);
        if (currentAmmoProto)
        {
            uint32 const currentAmmoDPS =
                Planner::AmmoDamageOf(currentAmmoProto->Damage[0].DamageMin, currentAmmoProto->Damage[0].DamageMax);
            uint32 const newAmmoDPS = Planner::AmmoDamageOf(proto->Damage[0].DamageMin, proto->Damage[0].DamageMax);

            if (newAmmoDPS > currentAmmoDPS) // New ammo meets upgrade condition
                return ITEM_USAGE_EQUIP;

            if (newAmmoDPS < currentAmmoDPS) // New ammo is worse
                return ITEM_USAGE_NONE;
        }
        // A persistent bot's ammo (WU28): the stock rule its shopping list buys by
        Planner::StockDecision const stock = Planner::StockFor(bot, botAI, proto);
        if (stock.decided)
            return stock.buy ? ITEM_USAGE_AMMO : stock.kept ? ITEM_USAGE_KEEP : ITEM_USAGE_NONE;
        float ammoCount = BetterStacks(proto, "ammo");
        float const requiredAmmo = (bot->getClass() == CLASS_HUNTER) ? 8 : 2;  // Hunters get 8 stacks, others 2
        // Ensure we have enough ammo in the inventory
        if (ammoCount < requiredAmmo)
        {
            ammoCount += CurrentStacks(proto);

            if (ammoCount < requiredAmmo)  // Buy ammo to reach the proper supply
                return ITEM_USAGE_AMMO;
            else if (ammoCount < requiredAmmo + 1)
                return ITEM_USAGE_KEEP;  // Keep the ammo if we don't have too much.
        }
    }
    return ITEM_USAGE_NONE;
}

bool ItemUsageValue::KeepsByCount(ItemTemplate const* proto, SkillPath::Path const* path)
{
    if (IsItemNeededForSkill(proto))
        return true;
    if (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_MISC || proto->Class == ITEM_CLASS_REAGENT)
    {
        bool const lowBagSpace = AI_VALUE(uint8, "bag space") > 50;
        return path ? IsItemNeededForClassSpell(proto) : IsItemNeededForUsefullSpell(proto, lowBagSpace);
    }
    if (proto->Class == ITEM_CLASS_RECIPE)
        return !bot->HasSpell(proto->Spells[2].SpellId) && bot->CanUseItem(proto) == EQUIP_ERR_OK;
    return false;
}

ParsedItemUsage ItemUsageValue::GetItemIdFromQualifier()
{
    ParsedItemUsage parsed;
    parsed.countBank = qualifier.ends_with(BANK_COUNT_QUALIFIER);

    size_t const pos = qualifier.find(",");
    if (pos != std::string::npos)
    {
        parsed.itemId = atoi(qualifier.substr(0, pos).c_str());
        parsed.randomPropertyId = atoi(qualifier.substr(pos + 1).c_str());
        return parsed;
    }
    else
        parsed.itemId = atoi(qualifier.c_str());
    return parsed;
}
uint32 ItemUsageValue::SmallestBagSize(Player* bot)
{
    uint32 smallest = 0;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Bag const* const bag = bot->GetBagByPos(slot);
        if (!bag)
            return 0;

        if (slot == INVENTORY_SLOT_BAG_START || bag->GetBagSize() < smallest)
            smallest = bag->GetBagSize();
    }

    return smallest;
}

ItemUsageValue::DisenchantOutlet ItemUsageValue::DisenchantOutletOf(PlayerbotAI* botAI, ItemTemplate const* proto)
{
    DisenchantOutlet outlet;
    if (!botAI || !proto || !proto->DisenchantID || !Planner::RecipeIndexReady())
        return outlet;
    std::shared_ptr<Recipes::RecipeIndex const> const index = Planner::CurrentRecipeIndex();
    Recipes::Process const* process = index ? index->DisenchantOf(proto->DisenchantID) : nullptr;
    if (!process)
        return outlet;

    SkillPath::Path const* path = Planner::KeptPath(botAI);
    Market::View const market = Planner::MarketView();
    std::vector<WorldTravel::OutletYield> yields;
    yields.reserve(process->yields.size());
    for (Recipes::Yield const& yield : process->yields)
    {
        ItemTemplate const* yieldProto = sObjectMgr->GetItemTemplate(yield.item);
        uint32 const vendor = yieldProto ? yieldProto->SellPrice : 0;  // plain copper, as the item's sale (WR14d)
        SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, yield.item) : nullptr;
        outlet.usedByPath = outlet.usedByPath || material;
        // the other buyer is the auction: a listing's proceeds (WA7a), 0 without a typical price
        yields.push_back({yield.expected, WorldTravel::YieldWorthEach(material ? material->copperEach : 0, vendor,
                                                                      Planner::MarketProceeds(market, yield.item))});
    }
    outlet.worthEach = WorldTravel::DisenchantWorth(yields);
    return outlet;
}

uint32 ItemUsageValue::ItemWorth(ItemTemplate const* proto, ItemUsage usage, uint32 count, float sellMultiplier,
                                 uint32 keptWorthEach, uint32 useWorthEach, uint32 outletWorthEach)
{
    if (!proto)
        return 0;

    WorldTravel::ItemFacts facts;
    switch (usage)
    {
        case ITEM_USAGE_QUEST:
        case ITEM_USAGE_GUILD_TASK:
            facts.Quest = true;
            break;
        case ITEM_USAGE_KEEP:
            facts.Kept = true;
            break;
        case ITEM_USAGE_BANK:
            facts.LaterWorthEach = keptWorthEach;
            facts.Kept = !keptWorthEach;
            break;
        case ITEM_USAGE_EQUIP:
        case ITEM_USAGE_REPLACE:
        case ITEM_USAGE_BAD_EQUIP:
        case ITEM_USAGE_BROKEN_EQUIP:
            facts.Upgrade = true;
            break;
        case ITEM_USAGE_USE:
        case ITEM_USAGE_SKILL:
        case ITEM_USAGE_AMMO:
            facts.Used = true;
            break;
        default:  // none, vendor, auction and disenchant items sell
            break;
    }

    facts.Quest = facts.Quest || proto->StartQuest;
    facts.Hearthstone = proto->ItemId == ITEM_HEARTHSTONE;
    return WorldTravel::ItemWorth(WorldTravel::ClassifyForSpace(facts), proto->SellPrice,
                                  static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0)), count, sellMultiplier,
                                  facts.LaterWorthEach, useWorthEach,
                                  usage == ITEM_USAGE_DISENCHANT ? outletWorthEach : 0);
}

bool ItemUsageValue::IsItemUsefulForQuest(Player* player, ItemTemplate const* proto)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return false;

    // Core loot gating predicate: also covers quest source items (ItemDrop,
    // e.g. quest keys). The local objective check below is kept because this
    // predicate skips non-raid quests for raid groups.
    if (player->HasQuestForItem(proto->ItemId))
        return true;

    for (uint16 slot = 0; slot < player->GetQuestLogSize(); ++slot)
    {
        uint32 entry = player->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(entry);
        if (!quest)
            continue;

        // Check if the item itself is needed for the quest
        for (uint8 i = 0; i < 4; i++)
        {
            if (quest->RequiredItemId[i] == proto->ItemId)
            {
                if (player->GetItemCount(proto->ItemId, false) >= quest->RequiredItemCount[i])
                    continue;

                return true; // Item is directly required for a quest
            }
        }

        // Check if the item has spells that create a required quest item
        for (uint8 i = 0; i < MAX_ITEM_SPELLS; i++)
        {
            uint32 spellId = proto->Spells[i].SpellId;
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            for (uint8 effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; effectIndex++)
            {
                if (spellInfo->Effects[effectIndex].Effect == SPELL_EFFECT_CREATE_ITEM)
                {
                    uint32 createdItemId = spellInfo->Effects[effectIndex].ItemType;

                    // Check if the created item is required for a quest
                    for (uint8 j = 0; j < 4; j++)
                    {
                        if (quest->RequiredItemId[j] == createdItemId)
                        {
                            if (player->GetItemCount(createdItemId, false) >= quest->RequiredItemCount[j])
                                continue;

                            return true; // Item is useful because it creates a required quest item
                        }
                    }
                }
            }
        }
    }

    return false; // Item is not useful for any active quests
}

bool ItemUsageValue::IsStartQuestItemUsefulFor(Player* player, ItemTemplate const* proto)
{
    if (!proto->StartQuest)
        return false;

    Quest const* quest = sObjectMgr->GetQuestTemplate(proto->StartQuest);
    return quest && player->GetQuestStatus(proto->StartQuest) == QUEST_STATUS_NONE &&
        QuestAction::CanAcceptQuest(player, quest);
}

bool ItemUsageValue::IsItemNeededForSkill(ItemTemplate const* proto)
{
    // The professions' tool table (WR.12) first: every tool a recipe of the bot's professions asks for (WR14e: a
    // sale trip sold a scribe's Virtuoso Inking Set, so its inks had none)
    for (ProfessionRules::ToolRow const& row : ProfessionRules::TOOLS)
        if (row.Item == proto->ItemId && botAI->HasSkill(static_cast<SkillType>(row.Which)))
            return true;
    switch (proto->ItemId)
    {
        case 756:  // Tunnel Pick
            return botAI->HasSkill(SKILL_MINING);
        case 778:  // Kobold Excavation Pick
            return botAI->HasSkill(SKILL_MINING);
        case 1819:  // Gouging Pick
            return botAI->HasSkill(SKILL_MINING);
        case 1893:  // Miner's Revenge
            return botAI->HasSkill(SKILL_MINING);
        case 1959:  // Cold Iron Pick
            return botAI->HasSkill(SKILL_MINING);
        case 2901:  // Mining Pick
            return botAI->HasSkill(SKILL_MINING);
        case 9465:  // Digmaster 5000
            return botAI->HasSkill(SKILL_MINING);
        case 20723:  // Brann's Trusty Pick
            return botAI->HasSkill(SKILL_MINING);
        case 40772:  // Gnomish Army Knife
            return botAI->HasSkill(SKILL_MINING) || botAI->HasSkill(SKILL_ENGINEERING) || botAI->HasSkill(SKILL_BLACKSMITHING) || botAI->HasSkill(SKILL_COOKING) || botAI->HasSkill(SKILL_SKINNING);
        case 40892:  // Hammer Pick
            return botAI->HasSkill(SKILL_MINING) || botAI->HasSkill(SKILL_BLACKSMITHING);
        case 40893:  // Bladed Pickaxe
            return botAI->HasSkill(SKILL_MINING) || botAI->HasSkill(SKILL_SKINNING);
        case 5956:  // Blacksmith Hammer
            return botAI->HasSkill(SKILL_BLACKSMITHING) || botAI->HasSkill(SKILL_ENGINEERING);
        case 6219:  // Arclight Spanner
            return botAI->HasSkill(SKILL_ENGINEERING);
        case 6218:  // Runed copper rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 6339:  // Runed silver rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 11130:  // Runed golden rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 11145:  // Runed truesilver rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 16207:  // Runed Arcanite Rod
            return botAI->HasSkill(SKILL_ENCHANTING);
        case 7005:  // Skinning Knife
            return botAI->HasSkill(SKILL_SKINNING);
        case 12709:
            return botAI->HasSkill(SKILL_SKINNING);
        case 19901:
            return botAI->HasSkill(SKILL_SKINNING);
        case 4471:  // Flint and Tinder
            return botAI->HasSkill(SKILL_COOKING);
        case 4470:  // Simple Wood
            return botAI->HasSkill(SKILL_COOKING);
        case 6256:  // Fishing Rod
            return botAI->HasSkill(SKILL_FISHING);
    }

    return false;
}

bool ItemUsageValue::IsItemUsefulForSkill(ItemTemplate const* proto)
{
    switch (proto->Class)
    {
        case ITEM_CLASS_TRADE_GOODS:
        case ITEM_CLASS_MISC:
        case ITEM_CLASS_REAGENT:
        case ITEM_CLASS_GEM:
        {
            if (botAI->HasSkill(SKILL_TAILORING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_TAILORING))
                return true;
            if (botAI->HasSkill(SKILL_LEATHERWORKING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_LEATHERWORKING))
                return true;
            if (botAI->HasSkill(SKILL_ENGINEERING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_ENGINEERING))
                return true;
            if (botAI->HasSkill(SKILL_BLACKSMITHING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_BLACKSMITHING))
                return true;
            if (botAI->HasSkill(SKILL_ALCHEMY) && RandomItemMgr::IsUsedBySkill(proto, SKILL_ALCHEMY))
                return true;
            if (botAI->HasSkill(SKILL_ENCHANTING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_ENCHANTING))
                return true;
            if (botAI->HasSkill(SKILL_FISHING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_FISHING))
                return true;
            if (botAI->HasSkill(SKILL_FIRST_AID) && RandomItemMgr::IsUsedBySkill(proto, SKILL_FIRST_AID))
                return true;
            if (botAI->HasSkill(SKILL_COOKING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_COOKING))
                return true;
            if (botAI->HasSkill(SKILL_JEWELCRAFTING) && RandomItemMgr::IsUsedBySkill(proto, SKILL_JEWELCRAFTING))
                return true;
            if (botAI->HasSkill(SKILL_MINING) && (RandomItemMgr::IsUsedBySkill(proto, SKILL_MINING) ||
                                                  RandomItemMgr::IsUsedBySkill(proto, SKILL_BLACKSMITHING) ||
                                                  RandomItemMgr::IsUsedBySkill(proto, SKILL_JEWELCRAFTING) ||
                                                  RandomItemMgr::IsUsedBySkill(proto, SKILL_ENGINEERING)))
                return true;
            if (botAI->HasSkill(SKILL_SKINNING) && (RandomItemMgr::IsUsedBySkill(proto, SKILL_SKINNING) ||
                                                    RandomItemMgr::IsUsedBySkill(proto, SKILL_LEATHERWORKING)))
                return true;
            if (botAI->HasSkill(SKILL_HERBALISM) && (RandomItemMgr::IsUsedBySkill(proto, SKILL_HERBALISM) ||
                                                     RandomItemMgr::IsUsedBySkill(proto, SKILL_ALCHEMY)))
                return true;

            return false;
        }
        case ITEM_CLASS_RECIPE:
        {
            if (bot->HasSpell(proto->Spells[2].SpellId))
                break;

            switch (proto->SubClass)
            {
                case ITEM_SUBCLASS_LEATHERWORKING_PATTERN:
                    return botAI->HasSkill(SKILL_LEATHERWORKING);
                case ITEM_SUBCLASS_TAILORING_PATTERN:
                    return botAI->HasSkill(SKILL_TAILORING);
                case ITEM_SUBCLASS_ENGINEERING_SCHEMATIC:
                    return botAI->HasSkill(SKILL_ENGINEERING);
                case ITEM_SUBCLASS_BLACKSMITHING:
                    return botAI->HasSkill(SKILL_BLACKSMITHING);
                case ITEM_SUBCLASS_COOKING_RECIPE:
                    return botAI->HasSkill(SKILL_COOKING);
                case ITEM_SUBCLASS_ALCHEMY_RECIPE:
                    return botAI->HasSkill(SKILL_ALCHEMY);
                case ITEM_SUBCLASS_FIRST_AID_MANUAL:
                    return botAI->HasSkill(SKILL_FIRST_AID);
                case ITEM_SUBCLASS_ENCHANTING_FORMULA:
                    return botAI->HasSkill(SKILL_ENCHANTING);
                case ITEM_SUBCLASS_FISHING_MANUAL:
                    return botAI->HasSkill(SKILL_FISHING);
            }
        }
    }

    return false;
}

namespace
{
    // Crafting chains can be cyclic: Remember which items are already being evaluated
    // further up the stack and stop as soon as a cycle closes.
    thread_local std::unordered_set<uint32> itemsBeingEvaluated;

    class UsefullSpellRecursionGuard
    {
    public:
        explicit UsefullSpellRecursionGuard(uint32 itemId)
            : _itemId(itemId), _entered(itemsBeingEvaluated.insert(itemId).second)
        {
        }

        ~UsefullSpellRecursionGuard()
        {
            if (_entered)
                itemsBeingEvaluated.erase(_itemId);
        }

        UsefullSpellRecursionGuard(UsefullSpellRecursionGuard const&) = delete;
        UsefullSpellRecursionGuard& operator=(UsefullSpellRecursionGuard const&) = delete;

        bool Entered() const { return _entered; }

    private:
        uint32 _itemId;
        bool _entered;
    };
}

bool ItemUsageValue::IsItemNeededForUsefullSpell(ItemTemplate const* proto, bool checkAllReagents)
{
    UsefullSpellRecursionGuard guard(proto->ItemId);
    if (!guard.Entered())
        return false;

    for (auto spellId : SpellsUsingItem(proto->ItemId, bot))
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        if (checkAllReagents && !HasItemsNeededForSpell(spellId, proto))
            continue;

        // Class spells consume reagents without creating items; generic profession spells
        // must not make every trainer recipe's reagent worth hoarding.
        if (spellInfo->SpellFamilyName != SPELLFAMILY_GENERIC)
            return true;

        if (SpellGivesSkillUp(spellId, bot))
            return true;

        uint32 newItemId = spellInfo->Effects[EFFECT_0].ItemType;
        if (newItemId && newItemId != proto->ItemId)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", newItemId);

            if (usage != ITEM_USAGE_REPLACE && usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_AMMO &&
                usage != ITEM_USAGE_QUEST && usage != ITEM_USAGE_SKILL && usage != ITEM_USAGE_USE)
                continue;

            return true;
        }
    }

    return false;
}

bool ItemUsageValue::IsItemNeededForClassSpell(ItemTemplate const* proto)
{
    std::vector<uint32> const reagents = ClassSpellReagents(bot);
    return std::binary_search(reagents.begin(), reagents.end(), proto->ItemId);
}

std::vector<uint32> ItemUsageValue::ClassSpellReagents(Player* bot)
{
    std::vector<uint32> items;
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        // A spell the server charges no reagents for (Glyph of Renewed Life) uses none
        if (!info || info->SpellFamilyName == SPELLFAMILY_GENERIC || bot->CanNoReagentCast(info))
            continue;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (info->ReagentCount[i] > 0 && info->Reagent[i] > 0)
                items.push_back(static_cast<uint32>(info->Reagent[i]));
    }
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    return items;
}

static SpellInfo const* GetTemporaryEnchantSpell(ItemTemplate const* proto)
{
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        uint32 spellId = proto->Spells[i].SpellId;
        if (!spellId)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
            if (spellInfo->Effects[j].Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                return spellInfo;
    }

    return nullptr;
}

// True for rogue poisons: their temporary enchant spell is the rogue family and carries a
// per-poison family flag (stones/oils share the subclass but use family-less spells).
static bool IsRoguePoison(ItemTemplate const* proto)
{
    if (proto->SubClass != ITEM_SUBCLASS_CONSUMABLE_OTHER)
        return false;

    SpellInfo const* useSpell = GetTemporaryEnchantSpell(proto);
    return useSpell && useSpell->SpellFamilyName == SPELLFAMILY_ROGUE && !useSpell->SpellFamilyFlags.IsEqual(0, 0, 0);
}

bool ItemUsageValue::IsTemporaryWeaponEnchantment(ItemTemplate const* proto)
{
    return GetTemporaryEnchantSpell(proto) != nullptr;
}

bool ItemUsageValue::IsBestPoison(ItemTemplate const* proto)
{
    // Family comes from the use spell's SpellFamilyFlags (per poison type), not item ids.
    if (!IsRoguePoison(proto))
        return false;

    SpellInfo const* useSpell = GetTemporaryEnchantSpell(proto);

    // Keep only the highest-item-level poison per family (one family per weapon); lower ranks
    // fall through to VENDOR/AH.
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* other = item->GetTemplate();
        if (!other || other->ItemId == proto->ItemId)
            continue;
        if (!IsRoguePoison(other))
            continue;

        // Unusable higher ranks (level or class restriction) must not block a usable lower rank.
        if (bot->CanUseItem(other) != EQUIP_ERR_OK)
            continue;

        SpellInfo const* otherUseSpell = GetTemporaryEnchantSpell(other);
        if (otherUseSpell && otherUseSpell->SpellFamilyFlags == useSpell->SpellFamilyFlags &&
            other->ItemLevel > proto->ItemLevel)
            return false;
    }

    return true;
}

bool ItemUsageValue::HasItemsNeededForSpell(uint32 spellId, ItemTemplate const* proto)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; i++)
    {
        // Same validity check Spell::CheckItems uses: invalid/sentinel reagent ids are skipped.
        if (spellInfo->Reagent[i] <= 0)
            continue;

        if (proto && proto->ItemId == uint32(spellInfo->Reagent[i]) &&
            spellInfo->ReagentCount[i] == 1)  // If we only need 1 item then current item does not need to be
                                              // checked since we are looting/buying or already have it.
            continue;

        ItemTemplate const* reqProto = sObjectMgr->GetItemTemplate(spellInfo->Reagent[i]);
        if (!reqProto)
            continue;

        uint32 count = AI_VALUE2(uint32, "item count", reqProto->Name1);

        if (count < spellInfo->ReagentCount[i])
            return false;
    }

    return true;
}

Item* ItemUsageValue::CurrentItem(ItemTemplate const* proto)
{
    Item* bestItem = nullptr;
    std::vector<Item*> found = AI_VALUE2(std::vector<Item*>, "inventory items", chat->FormatItem(proto));
    for (auto item : found)
    {
        if (bestItem && item->GetUInt32Value(ITEM_FIELD_DURABILITY) < bestItem->GetUInt32Value(ITEM_FIELD_DURABILITY))
            continue;

        if (bestItem && item->GetCount() < bestItem->GetCount())
            continue;

        bestItem = item;
    }

    return bestItem;
}

float ItemUsageValue::CurrentStacks(ItemTemplate const* proto)
{
    uint32 maxStack = proto->GetMaxStackSize();

    std::vector<Item*> found = AI_VALUE2(std::vector<Item*>, "inventory items", chat->FormatItem(proto));

    float itemCount = 0;

    for (auto stack : found)
    {
        itemCount += stack->GetCount();
    }

    return itemCount / maxStack;
}

float ItemUsageValue::BetterStacks(ItemTemplate const* proto, std::string const itemType)
{
    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", itemType);

    float stacks = 0;

    for (auto& otherItem : items)
    {
        ItemTemplate const* otherProto = otherItem->GetTemplate();

        if (otherProto->Class != proto->Class || otherProto->SubClass != proto->SubClass)
            continue;

        if (otherProto->ItemLevel < proto->ItemLevel)
            continue;

        if (otherProto->ItemId == proto->ItemId)
            continue;

        stacks += CurrentStacks(otherProto);
    }

    return stacks;
}

float ItemUsageValue::BetterStockStacks(ItemTemplate const* proto, bool hasMana)
{
    std::pair<ItemUse::Kind, std::uint8_t> const kind = StockKindOf(proto->ItemId, hasMana);
    float stacks = 0;
    for (Item* otherItem : botAI->GetInventoryItems())
    {
        ItemTemplate const* otherProto = otherItem->GetTemplate();
        if (otherProto->Class != proto->Class || otherProto->SubClass != proto->SubClass ||
            otherProto->ItemLevel < proto->ItemLevel || otherProto->ItemId == proto->ItemId)
            continue;
        if (StockKindOf(otherProto->ItemId, hasMana) != kind)
            continue;
        stacks += CurrentStacks(otherProto);
    }
    return stacks;
}

std::vector<uint32> ItemUsageValue::SpellsUsingItem(uint32 itemId, Player* bot)
{
    std::vector<uint32> retSpells;

    PlayerSpellMap const& spellMap = bot->GetSpellMap();

    for (auto& spell : spellMap)
    {
        uint32 spellId = spell.first;

        if (spell.second->State == PLAYERSPELL_REMOVED || !spell.second->Active)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        // Skip only spells the server itself will not charge reagents for (Glyph of Renewed Life);
        // SPELL_ATTR_PASSIVE alone does not mean the reagent is unused.
        if (bot->CanNoReagentCast(spellInfo))
            continue;

        // Profession spells are generic-family; only their crafting spells (CREATE_ITEM) count.
        // Class spells (poisons, oils, water walking, ...) consume reagents without creating items.
        if (spellInfo->Effects[EFFECT_0].Effect != SPELL_EFFECT_CREATE_ITEM &&
            spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;

        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; i++)
            if (spellInfo->ReagentCount[i] > 0 && uint32(spellInfo->Reagent[i]) == itemId)
                retSpells.push_back(spellId);
    }

    return retSpells;
}

inline int32 SkillGainChance(uint32 SkillValue, uint32 GrayLevel, uint32 GreenLevel, uint32 YellowLevel)
{
    if (SkillValue >= GrayLevel)
        return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY) * 10;

    if (SkillValue >= GreenLevel)
        return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREEN) * 10;

    if (SkillValue >= YellowLevel)
        return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_YELLOW) * 10;

    return sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE) * 10;
}

bool ItemUsageValue::SpellGivesSkillUp(uint32 spellId, Player* bot)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);

    for (SkillLineAbilityMap::const_iterator _spell_idx = bounds.first; _spell_idx != bounds.second; ++_spell_idx)
    {
        SkillLineAbilityEntry const* skill = _spell_idx->second;
        if (skill->SkillLine)
        {
            uint32 SkillValue = bot->GetPureSkillValue(skill->SkillLine);

            if (SkillGainChance(SkillValue, skill->TrivialSkillLineRankHigh,
                                (skill->TrivialSkillLineRankHigh + skill->TrivialSkillLineRankLow) / 2,
                                skill->TrivialSkillLineRankLow) > 0)
                return true;
        }
    }

    return false;
}

ItemUsage ItemUpgradeValue::Calculate()
{
    ParsedItemUsage parsed = GetItemIdFromQualifier();
    uint32 itemId = parsed.itemId;
    uint32 randomPropertyId = parsed.randomPropertyId;
    if (!itemId)
        return ITEM_USAGE_NONE;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return ITEM_USAGE_NONE;

    ItemUsage equip = QueryItemUsageForEquip(proto, randomPropertyId);
    if (equip != ITEM_USAGE_NONE)
        return equip;

    if (proto->Class == ITEM_CLASS_PROJECTILE && bot->CanUseItem(proto) == EQUIP_ERR_OK)
        return QueryItemUsageForAmmo(proto);

    return ITEM_USAGE_NONE;
}
