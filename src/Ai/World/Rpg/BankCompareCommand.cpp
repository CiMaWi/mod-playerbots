/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// CS29b's compare: the bank's prediction and the item usage on every wearable item a bot may wear at its level, the
// differences listed by their cause. Deleted with CS29b's last commit.

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "BankCommands.h"
#include "BankRulesInput.h"
#include "Chat.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"

namespace BankMail
{
namespace
{
// The console reply names at most this many items of each cause
constexpr size_t EXAMPLES_SHOWN = 3;

// The equip action's set (EquipUpgradesAction): BAD_EQUIP is worn too, so the bank and the equip action agree
bool WearsNow(ItemUsage usage)
{
    return usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP;
}

// A fresh object, as SkillPathLive asks it: the bot's value cache keeps none of the thousands of entries asked
ItemUsage UsageOf(PlayerbotAI* botAI, uint32 entry)
{
    ItemUpgradeValue check(botAI);
    check.Qualify(std::to_string(entry));
    return check.Calculate();
}

bool IsAskedGear(ItemTemplate const& proto, uint32 level)
{
    return (proto.Class == ITEM_CLASS_WEAPON || proto.Class == ITEM_CLASS_ARMOR) &&
           proto.InventoryType != INVTYPE_NON_EQUIP && proto.InventoryType != INVTYPE_BAG &&
           proto.InventoryType != INVTYPE_QUIVER && proto.InventoryType != INVTYPE_AMMO && proto.RequiredLevel <= level;
}

bool PredictsOffHand(ComparedItem const& read)
{
    return read.firstSlot == EQUIPMENT_SLOT_OFFHAND || read.secondSlot == EQUIPMENT_SLOT_OFFHAND;
}

// Why the usage refuses an item the bot does not hold (it creates one, as QueryItemUsageForEquip does); empty when
// the core lets the bot equip it
std::string EquipRefusal(PlayerbotAI* botAI, Player* bot, ItemTemplate const* proto)
{
    Item* probe = Item::CreateItem(proto->ItemId, 1, bot);
    if (!probe)
        return "create";
    uint16 dest = 0;
    InventoryResult const result = botAI->CanEquipItem(NULL_SLOT, dest, probe, true, true);
    probe->RemoveFromUpdateQueueOf(bot);
    delete probe;
    if (result != EQUIP_ERR_OK && result != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
        return "equip_" + std::to_string(result);
    // QueryItemUsageForEquip reads a copy in the bank as "already equipped"
    bool const unique = result == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS || proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE);
    if (unique && bot->GetItemCount(proto->ItemId, true) > bot->GetItemCount(proto->ItemId, false))
        return "unique_in_bank";
    return "";
}

// The first cause that matches: the four the named change expects, else other:<word>
std::string CauseOf(PlayerbotAI* botAI, Player* bot, ItemTemplate const* proto, ComparedItem const& read,
                    ItemUsage usage, bool oldWears)
{
    if (proto->RequiredSkill && !bot->GetSkillValue(proto->RequiredSkill))
        return "skill_held";
    if (uint32 const skill = proto->GetSkill(); skill && !bot->GetSkillValue(skill))
        return "skill_held";
    if (proto->Class == ITEM_CLASS_WEAPON && PredictsOffHand(read) && !bot->CanDualWield())
        return "skill_held";
    if (proto->RequiredSkill && bot->GetSkillValue(proto->RequiredSkill) < proto->RequiredSkillRank)
        return "skill_rank";
    if (proto->RequiredSpell && !bot->HasSpell(proto->RequiredSpell))
        return "required_spell";
    bool const titanGripKind = proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                               proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                               proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2;
    if (bot->CanTitanGrip() && proto->InventoryType == INVTYPE_2HWEAPON && !titanGripKind)
        return "titans_grip";

    InventoryResult const canUse = bot->CanUseItem(proto);
    if (canUse != EQUIP_ERR_OK)
        return "other:can_use_" + std::to_string(canUse);
    if (usage == ITEM_USAGE_BROKEN_EQUIP)
        return "other:broken";
    if (usage == ITEM_USAGE_NONE)
    {
        std::string const refusal = EquipRefusal(botAI, bot, proto);
        if (!refusal.empty())
            return "other:" + refusal;
    }
    if (!read.item.wearable)
        return read.firstSlot == NULL_SLOT ? "other:no_predicted_slot" : "other:not_predicted_wearable";
    if (read.item.score <= 0.0f)
        return "other:zero_score";
    uint8 const usageSlot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
    if (usageSlot == NULL_SLOT)
        return "other:no_usage_slot";
    if (usageSlot != read.firstSlot && usageSlot != read.secondSlot)
        return "other:slot";
    if (read.secondSlot != NULL_SLOT)
        return oldWears ? "other:second_slot_old" : "other:second_slot_new";
    return oldWears ? "other:score_old" : "other:score_new";
}

struct CauseCount
{
    uint32 count = 0;
    std::vector<uint32> examples;
};
}  // namespace

bool HandleBankCompareCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->SendErrorMessage("Usage: playerbots bank compare <bot name> [item entry ...]");
        return false;
    }
    std::vector<uint32> named;
    for (uint32 entry = 0; in >> entry;)
        named.push_back(entry);
    std::string const asked = name;
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot && bot->IsInWorld() ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("BANK compare: no bot {} online", asked);
        return false;
    }

    // On the world thread between the map updates, like bank status: both answers read one state of the bot
    uint32 const level = bot->GetLevel();
    std::vector<ItemTemplate const*> items;
    for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        if (IsAskedGear(proto, level))
            items.push_back(&proto);
    std::sort(items.begin(), items.end(),
              [](ItemTemplate const* a, ItemTemplate const* b) { return a->ItemId < b->ItemId; });

    KeepBotIn const keepBot = BuildKeepBot(bot);
    std::vector<ComparedItem> const read = ReadForCompare(botAI, items);
    std::map<std::string, CauseCount> causes;
    uint32 differ = 0;
    for (size_t i = 0; i < items.size() && i < read.size(); ++i)
    {
        ItemTemplate const* proto = items[i];
        bool const oldWears = GearForLaterLevel(read[i].item, keepBot).keeps;
        ItemUsage const usage = UsageOf(botAI, proto->ItemId);
        if (oldWears == WearsNow(usage))
            continue;

        ++differ;
        std::string const cause = CauseOf(botAI, bot, proto, read[i], usage, oldWears);
        CauseCount& count = causes[cause];
        ++count.count;
        if (count.examples.size() < EXAMPLES_SHOWN)
            count.examples.push_back(proto->ItemId);
        LOG_INFO("playerbots",
                 "BANKCMP {} {} {} {} old={} new={} cause={} q={} inv={} sub={}/{} score={:.1f} worn={:.1f} empty={}",
                 bot->GetName(), bot->getClass(), level, proto->ItemId, oldWears ? 1 : 0, static_cast<uint32>(usage),
                 cause, proto->Quality, proto->InventoryType, proto->Class, proto->SubClass, read[i].item.score,
                 read[i].item.wornScore, read[i].item.slotEmpty ? 1 : 0);
    }

    std::ostringstream counts;
    for (auto const& [cause, count] : causes)
        counts << "; " << cause << " " << count.count;
    handler->PSendSysMessage("BANK compare {}: L{} class {}: asked {}, differ {}{}", bot->GetName(), level,
                             bot->getClass(), items.size(), differ, counts.str());
    for (auto const& [cause, count] : causes)
    {
        std::ostringstream examples;
        for (uint32 entry : count.examples)
            examples << " " << entry;
        handler->PSendSysMessage("  {} e.g.{}", cause, examples.str());
    }

    // Named items (asked whatever their level): every keep input, the bank's whole decision and the usage's answer
    std::vector<ItemTemplate const*> namedItems;
    for (uint32 entry : named)
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
            namedItems.push_back(proto);
    std::vector<ComparedItem> const namedRead = ReadForCompare(botAI, namedItems);
    for (size_t i = 0; i < namedItems.size() && i < namedRead.size(); ++i)
    {
        ItemTemplate const* proto = namedItems[i];
        KeepItemIn const& keep = namedRead[i].item;
        KeepAnswer const gear = GearForLaterLevel(keep, keepBot);
        KeepAnswer const decided = DecideKeep(keep, keepBot);
        std::string const text = Acore::StringFormat(
            "BANKCMP-ITEM {} {} {} {} rl={} wearable={} slots={},{} score={:.2f} worn={:.2f} empty={} use={} sell={} "
            "gear={} kept={} by={} later={} due={} new={} canUse={} dualWield={} titanGrip={}",
            bot->GetName(), bot->getClass(), level, proto->ItemId, proto->RequiredLevel, keep.wearable ? 1 : 0,
            namedRead[i].firstSlot, namedRead[i].secondSlot, keep.score, keep.wornScore, keep.slotEmpty ? 1 : 0,
            keep.useWorth, keep.sellNow, gear.keeps ? 1 : 0, decided.keeps ? 1 : 0, decided.provider,
            decided.laterWorth, decided.dueLevel, static_cast<uint32>(UsageOf(botAI, proto->ItemId)),
            static_cast<uint32>(bot->CanUseItem(proto)), bot->CanDualWield() ? 1 : 0, bot->CanTitanGrip() ? 1 : 0);
        LOG_INFO("playerbots", "{}", text);
        handler->PSendSysMessage("  {}", text);
    }
    return true;
}
}  // namespace BankMail
