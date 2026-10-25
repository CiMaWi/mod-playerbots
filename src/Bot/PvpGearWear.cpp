/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Wearing the PvP loadout (Playerbots.PvpGear.Wear): the materialised swap. At the entry into an arena or a
// battleground the loadout's items are created and equipped and the PvE items they replace go to the bags; at the leave
// the PvP items are destroyed and the PvE items equipped again. Every worn slot is a row in playerbots_pvp_worn (per
// server, never copied), so a crash during a match is repaired at the bot's next login. The PvE items are only moved,
// never destroyed or recreated: their enchants, gems and durability stay.

#include <chrono>
#include <mutex>

#include "Battleground.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PlayerbotsDatabase.h"
#include "PvpGearMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"

namespace
{
constexpr uint32 RetryIntervalMs = 5000;
constexpr uint32 RetryAttempts = 60;  // five minutes; after that the bot's next login repairs

bool WearableSlot(uint8 slot) { return slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD; }

bool HasRow(PvpGearMgr::WornSlot const& row) { return row.PvpItem || row.PveItem; }

// A two-hander the bot cannot hold beside an off-hand item (the core's rule in Player::CanEquipItem)
bool NeedsEmptyOffhand(Player* bot, ItemTemplate const* proto)
{
    if (!proto || proto->InventoryType != INVTYPE_2HWEAPON)
        return false;

    return !bot->CanTitanGrip() || proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM ||
           proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF || proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
}

// CanEquipItem on a temporary item (as PlayerbotFactory::CanEquipUnseenItem) without the in-game checks (combat, an
// arena in progress): the swap is the bot's gear change at the entry and the leave, not a player's action
bool CanEquipEntry(Player* bot, uint8 slot, uint32 entry, uint16& dest)
{
    dest = 0;
    Item* item = Item::CreateItem(entry, 1, bot);
    if (!item)
        return false;

    InventoryResult const result = bot->CanEquipItem(slot, dest, item, true, false);
    item->RemoveFromUpdateQueueOf(bot);
    delete item;
    return result == EQUIP_ERR_OK;
}

bool HasBagRoom(Player* bot, Item* item)
{
    ItemPosCountVec dest;
    return item && bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK;
}

// The equipped item of the slot to a free bag slot; false (nothing moved) when the bags are full
bool MoveToBags(Player* bot, uint8 slot)
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    ItemPosCountVec dest;
    if (!item || bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
        return false;

    bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
    bot->StoreItem(dest, item, true);
    return true;
}

// A bag item back into the (empty) equipment slot; false when the core refuses it (the item stays in the bags)
bool EquipFromBags(Player* bot, uint8 slot, Item* item)
{
    uint16 dest = 0;
    if (!item || bot->CanEquipItem(slot, dest, item, false, false) != EQUIP_ERR_OK)
        return false;

    bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
    bot->EquipItem(dest, item, true);
    return true;
}

Item* ItemByCounter(Player* bot, uint32 counter)
{
    return counter ? bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(counter)) : nullptr;
}

uint64 MicrosecondsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

void WriteRows(uint32 guid, PvpGearMgr::Worn const* worn)
{
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_PVP_WORN);
    stmt->SetData(0, guid);
    transaction->Append(stmt);
    for (uint8 slot = 0; worn && slot < PvpGearMgr::SlotCount; ++slot)
    {
        PvpGearMgr::WornSlot const& row = (*worn)[slot];
        if (!HasRow(row))
            continue;

        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_REP_PVP_WORN);
        stmt->SetData(0, guid);
        stmt->SetData(1, slot);
        stmt->SetData(2, row.PvpItem);
        stmt->SetData(3, row.PveItem);
        transaction->Append(stmt);
    }

    PlayerbotsDatabase.CommitTransaction(transaction);
}
}  // namespace

PvpGearMgr::WearStep PvpGearMgr::DecideWear(WearInput const& in)
{
    if (!in.LoadoutItem)
        return WearStep::Keep;  // an empty loadout slot keeps whatever the slot holds

    if (in.HasRow)
        return WearStep::AlreadyWorn;

    if (in.EquippedItem == in.LoadoutItem)
        return WearStep::Keep;  // the PvE gear already holds the loadout's item

    if (!in.CanEquip)
        return WearStep::CannotEquip;

    if (!in.EquippedItem)
        return WearStep::Equip;

    return in.BagRoom ? WearStep::Displace : WearStep::BagsFull;
}

PvpGearMgr::UnwearStep PvpGearMgr::DecideUnwear(UnwearInput const& in)
{
    if (!in.PveItem)
        return UnwearStep::NothingToRestore;  // the slot was empty before the swap

    if (!in.PveFound)
        return UnwearStep::Lost;

    if (in.PveEquipped)
        return UnwearStep::AlreadyEquipped;  // e.g. a crash before the swap was saved

    return in.SlotFree ? UnwearStep::Restore : UnwearStep::SlotTaken;
}

void PvpGearMgr::LoadWorn()
{
    uint32 rows = 0;
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_PVP_WORN_ALL);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 const slot = fields[1].Get<uint8>();
            if (slot >= SlotCount)
                continue;

            WornSlot& row = _worn[fields[0].Get<uint32>()][slot];
            row.PvpItem = fields[2].Get<uint32>();
            row.PveItem = fields[3].Get<uint32>();
            ++rows;
        } while (result->NextRow());
    }

    if (rows)
        LOG_INFO("server.loading", ">> {} bots still wore PvP gear at the last shutdown ({} slots): repaired at login",
                 _worn.size(), rows);
}

bool PvpGearMgr::IsWorn(Player const* bot) const
{
    if (!_wornLoaded || !bot)
        return false;

    std::shared_lock lock(_wornLock);
    return _worn.contains(bot->GetGUID().GetCounter());
}

bool PvpGearMgr::IsWornItem(Player const* bot, Item const* item) const
{
    if (!_wornLoaded || !bot || !item)
        return false;

    uint32 const itemGuid = item->GetGUID().GetCounter();
    std::shared_lock lock(_wornLock);
    auto const it = _worn.find(bot->GetGUID().GetCounter());
    if (it == _worn.end())
        return false;

    for (WornSlot const& row : it->second)
        if (row.PvpItem == itemGuid || row.PveItem == itemGuid)
            return true;

    return false;
}

void PvpGearMgr::QueueWear(Player* bot)
{
    if (!sPlayerbotAIConfig.PvpGearEnable || !sPlayerbotAIConfig.PvpGearWear || !Instance()._loaded ||
        !IsLadderBot(bot))
        return;

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<PvpGearWearOperation>(bot->GetGUID(), true));
}

void PvpGearMgr::QueueUnwear(Player* bot)
{
    if (!Instance().IsWorn(bot))  // also with PvP gear off: a bot repaired late (the retry) may queue again
        return;

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<PvpGearWearOperation>(bot->GetGUID(), false));
}

void PvpGearMgr::Wear(Player* bot)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::Wear");
    if (!_loaded || !sPlayerbotAIConfig.PvpGearWear || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return;

    // Only while the bot is still in the arena or battleground (a short match may be over before this runs)
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetStatus() == STATUS_WAIT_LEAVE || !IsLadderBot(bot))
        return;

    auto const start = std::chrono::steady_clock::now();
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!Find(guid))
        OnBotLogin(bot);  // a bot that joined its arena team after its login gets the base tier now

    Loadout const* loadout = Find(guid);
    if (!loadout)
    {
        LOG_INFO("playerbots", "PvP gear: {} has no loadout, fights in its PvE gear", bot->GetName());
        return;
    }

    Worn worn{};
    {
        std::shared_lock lock(_wornLock);
        if (auto const it = _worn.find(guid); it != _worn.end())
            worn = it->second;
    }

    uint32 equipped = 0;
    uint32 displaced = 0;
    uint32 bagsFull = 0;
    uint32 refused = 0;
    bool changed = false;
    for (uint8 slot = 0; slot < SlotCount; ++slot)
    {
        uint32 const entry = (*loadout)[slot].Item;
        if (!WearableSlot(slot) || !entry)
            continue;

        WornSlot& row = worn[slot];
        Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        WearInput in;
        in.LoadoutItem = entry;
        in.HasRow = HasRow(row);
        in.EquippedItem = current ? current->GetEntry() : 0;

        // A two-hander the bot cannot hold beside its off-hand item: the off-hand item goes to the bags first
        Item* movedOffhand = nullptr;
        uint16 dest = 0;
        bool offhandStuck = false;
        if (!in.HasRow && in.EquippedItem != entry)
        {
            Item* offhand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (slot == EQUIPMENT_SLOT_MAINHAND && offhand && !HasRow(worn[EQUIPMENT_SLOT_OFFHAND]) &&
                NeedsEmptyOffhand(bot, sObjectMgr->GetItemTemplate(entry)))
            {
                if (MoveToBags(bot, EQUIPMENT_SLOT_OFFHAND))
                    movedOffhand = offhand;
                else
                    offhandStuck = true;
            }

            in.CanEquip = !offhandStuck && CanEquipEntry(bot, slot, entry, dest);
            in.BagRoom = HasBagRoom(bot, current);
        }

        WearStep step = DecideWear(in);
        if (offhandStuck)
            step = WearStep::BagsFull;

        Item* pvpItem = nullptr;
        uint32 pveGuid = 0;
        if (step == WearStep::Displace)
        {
            pveGuid = current->GetGUID().GetCounter();
            if (MoveToBags(bot, slot))
            {
                pvpItem = bot->EquipNewItem(dest, entry, true);
                if (!pvpItem)
                    EquipFromBags(bot, slot, current);  // it was just there: the slot keeps its PvE item
            }
        }
        else if (step == WearStep::Equip)
            pvpItem = bot->EquipNewItem(dest, entry, true);

        if (pvpItem)
        {
            row.PvpItem = pvpItem->GetGUID().GetCounter();
            row.PveItem = pveGuid;
            ++equipped;
            displaced += pveGuid ? 1 : 0;
            changed = true;
            if (movedOffhand)
            {
                worn[EQUIPMENT_SLOT_OFFHAND] = {0, movedOffhand->GetGUID().GetCounter()};
                ++displaced;
            }

            continue;
        }

        // Nothing equipped: the off-hand item moved for a two-hander comes back
        if (movedOffhand)
            EquipFromBags(bot, EQUIPMENT_SLOT_OFFHAND, movedOffhand);

        if (step == WearStep::BagsFull)
        {
            ++bagsFull;
            LOG_INFO("playerbots", "PvP gear: {} bags full, slot {} keeps its PvE item {}", bot->GetName(), slot,
                     current ? current->GetEntry() : 0);
        }
        else if (step == WearStep::CannotEquip || step == WearStep::Displace || step == WearStep::Equip)
        {
            ++refused;
            LOG_DEBUG("playerbots", "PvP gear: {} cannot equip item {} in slot {}", bot->GetName(), entry, slot);
        }
    }

    if (changed)
    {
        {
            std::unique_lock lock(_wornLock);
            _worn[guid] = worn;
        }

        WriteRows(guid, &worn);
        bot->AdditionalSavingAddMask(ADDITIONAL_SAVING_INVENTORY_AND_GOLD);
    }

    LOG_INFO("playerbots",
             "PvP gear: {} wears its loadout in {} {} ({} items equipped, {} PvE items to the bags, {} slots keep "
             "their PvE item: bags full, {} not equippable) in {} us",
             bot->GetName(), bg->isArena() ? "arena" : "battleground", bg->GetMapId(), equipped, displaced, bagsFull,
             refused, MicrosecondsSince(start));
}

bool PvpGearMgr::Unwear(Player* bot, char const* reason)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::Unwear");
    uint32 const guid = bot->GetGUID().GetCounter();
    Worn worn{};
    {
        std::shared_lock lock(_wornLock);
        auto const it = _worn.find(guid);
        if (it == _worn.end())
            return true;

        worn = it->second;
    }

    // Back in an arena or battleground (a retry after a quick requeue): the next leave takes the loadout off
    if (bot->GetBattleground())
        return true;

    // Items equipped by a dead or teleporting bot miss their stats: later (Update) or at the next login
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return false;

    auto const start = std::chrono::steady_clock::now();
    Loadout const* loadout = Find(guid);
    uint32 destroyed = 0;
    uint32 restored = 0;
    uint32 inBags = 0;
    uint32 lost = 0;

    // The PvP items first, so a two-hander can come back where a one-hander and an off hand were
    for (uint8 slot = 0; slot < SlotCount; ++slot)
    {
        Item* pvpItem = ItemByCounter(bot, worn[slot].PvpItem);
        if (!pvpItem)
            continue;  // never saved (a crash before the save) or already gone

        // The row's item in its slot is the one the swap created (the guards keep it there); elsewhere, it must be a
        // catalogue or loadout item (without a catalogue, PvP gear off, only the slot proves it)
        uint32 const entry = pvpItem->GetEntry();
        bool const inSlot = pvpItem->GetBagSlot() == INVENTORY_SLOT_BAG_0 && pvpItem->GetSlot() == slot;
        if (!inSlot && !FindItem(entry) && !(loadout && (*loadout)[slot].Item == entry))
        {
            LOG_ERROR("playerbots", "PvP gear: {} item {} (guid {}) of slot {} is not a PvP item: kept", bot->GetName(),
                      entry, worn[slot].PvpItem, slot);
            continue;
        }

        bot->DestroyItem(pvpItem->GetBagSlot(), pvpItem->GetSlot(), true);
        ++destroyed;
    }

    for (uint8 slot = 0; slot < SlotCount; ++slot)
    {
        WornSlot const& row = worn[slot];
        if (!HasRow(row))
            continue;

        Item* pveItem = ItemByCounter(bot, row.PveItem);
        UnwearInput in;
        in.PveItem = row.PveItem;
        in.PveFound = pveItem != nullptr;
        in.PveEquipped = pveItem && pveItem->GetBagSlot() == INVENTORY_SLOT_BAG_0 && pveItem->GetSlot() == slot;
        in.SlotFree = !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        switch (DecideUnwear(in))
        {
            case UnwearStep::Restore:
                if (EquipFromBags(bot, slot, pveItem))
                {
                    ++restored;
                    break;
                }

                [[fallthrough]];
            case UnwearStep::SlotTaken:
                ++inBags;
                LOG_INFO("playerbots", "PvP gear: {} PvE item {} of slot {} stays in the bags", bot->GetName(),
                         pveItem->GetEntry(), slot);
                break;
            case UnwearStep::Lost:
                ++lost;
                LOG_ERROR("playerbots", "PvP gear: {} PvE item lost (guid {}, slot {})", bot->GetName(), row.PveItem,
                          slot);
                break;
            case UnwearStep::AlreadyEquipped:
                ++restored;
                break;
            case UnwearStep::NothingToRestore:
                break;
        }
    }

    {
        std::unique_lock lock(_wornLock);
        _worn.erase(guid);
    }

    // The inventory is saved now and the rows deleted only after that commit: a crash in between leaves rows that the
    // next login repairs again (nothing left to do), never PvP gear without its rows
    CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
    bot->SaveInventoryAndGoldToDB(transaction);
    _saveCallbacks.AddCallback(CharacterDatabase.AsyncCommitTransaction(transaction))
        .AfterComplete(
            [guid](bool success)
            {
                if (!success)
                {
                    LOG_ERROR("playerbots", "PvP gear: inventory save of bot {} failed: rows kept for the next login",
                              guid);
                    return;
                }

                PvpGearMgr& mgr = Instance();
                {
                    std::shared_lock lock(mgr._wornLock);
                    if (mgr._worn.contains(guid))
                        return;  // worn again meanwhile: Wear has replaced the rows
                }

                WriteRows(guid, nullptr);
            });
    LOG_INFO("playerbots",
             "PvP gear: {} takes its loadout off ({}: {} PvP items destroyed, {} PvE items equipped, {} left in the "
             "bags, {} lost) in {} us",
             bot->GetName(), reason, destroyed, restored, inBags, lost, MicrosecondsSince(start));
    return true;
}

void PvpGearMgr::RetryUnwear(uint32 guid, uint32 attempts)
{
    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::RetryUnwear");
    for (std::pair<uint32, uint32>& pending : _retry)
        if (pending.first == guid)
            return;

    _retry.emplace_back(guid, attempts);
}

void PvpGearMgr::Update(uint32 diff)
{
    _saveCallbacks.ProcessReadyCallbacks();  // returns at once when there are none
    if (_retry.empty())
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("PvpGearMgr::Update");
    _retryTimer += diff;
    if (_retryTimer < RetryIntervalMs)
        return;

    _retryTimer = 0;
    std::vector<std::pair<uint32, uint32>> pending;
    pending.swap(_retry);
    for (auto const& [guid, attempts] : pending)
    {
        // Logged out: the rows stay and the next login repairs
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!bot || Unwear(bot, "retry"))
            continue;

        if (attempts + 1 < RetryAttempts)
            _retry.emplace_back(guid, attempts + 1);
        else
            LOG_INFO("playerbots", "PvP gear: {} still cannot take its loadout off: repaired at its next login",
                     bot->GetName());
    }
}

void PvpGearMgr::SelftestWear(std::function<void(bool, std::string const&)> const& check)
{
    auto const wear = [](uint32 loadout, uint32 equipped, bool hasRow, bool canEquip, bool bagRoom)
    {
        WearInput in;
        in.LoadoutItem = loadout;
        in.EquippedItem = equipped;
        in.HasRow = hasRow;
        in.CanEquip = canEquip;
        in.BagRoom = bagRoom;
        return DecideWear(in);
    };
    check(wear(100, 50, false, true, true) == WearStep::Displace, "wear: PvE item to the bags, loadout item on");
    check(wear(100, 0, false, true, false) == WearStep::Equip, "wear: empty slot takes the loadout item");
    check(wear(100, 50, false, true, false) == WearStep::BagsFull, "wear: bags full, the slot keeps its PvE item");
    check(wear(0, 50, false, true, true) == WearStep::Keep, "wear: empty loadout slot keeps the PvE item");
    check(wear(100, 100, false, true, true) == WearStep::Keep, "wear: PvE item already the loadout's entry");
    check(wear(100, 100, true, true, true) == WearStep::AlreadyWorn, "wear: worn slot not swapped twice");
    check(wear(100, 50, false, false, true) == WearStep::CannotEquip, "wear: not equippable, PvE item kept");

    auto const unwear = [](uint32 pve, bool found, bool equipped, bool slotFree)
    {
        UnwearInput in;
        in.PveItem = pve;
        in.PveFound = found;
        in.PveEquipped = equipped;
        in.SlotFree = slotFree;
        return DecideUnwear(in);
    };
    check(unwear(7, true, false, true) == UnwearStep::Restore, "unwear: PvE item back from the bags");
    check(unwear(7, false, false, true) == UnwearStep::Lost, "unwear: PvE item gone, logged as lost");
    check(unwear(0, false, false, true) == UnwearStep::NothingToRestore, "unwear: slot was empty before");
    check(unwear(7, true, true, false) == UnwearStep::AlreadyEquipped,
          "crash before the save: PvE item still equipped, nothing to do");
    check(unwear(7, true, false, false) == UnwearStep::SlotTaken, "unwear: slot taken, PvE item stays in the bags");
}
