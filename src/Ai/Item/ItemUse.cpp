/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUse.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CensusMgr.h"
#include "Creature.h"
#include "Item.h"
#include "ItemUseIndex.h"
#include "ItemUseKinds.h"
#include "ItemUseRules.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace ItemUse
{
namespace
{
constexpr std::uint32_t ORDER_TTL_MS = 60 * IN_MILLISECONDS;
constexpr float ORDER_CREATURE_YARDS = 100.0f;  // `creature <entry>`: the nearest of the template this far
constexpr std::uint8_t CAST_COUNT = 1;          // the client's cast counter; the handler only echoes it
constexpr std::int64_t KIND_FACTOR = 1000000;   // the row's amount: spell + KIND_FACTOR * kind

std::mutex ordersLock;
std::unordered_map<std::uint64_t, std::vector<Order>> orders;  // bot guid -> its waiting orders, in order

// What Check found on the bot's map for one request
struct Found
{
    Record const* record{nullptr};
    SpellInfo const* spellInfo{nullptr};
    Item* item{nullptr};
    Unit* unit{nullptr};
    Item* itemTarget{nullptr};
};

Cause CauseOfInventory(InventoryResult result)
{
    switch (result)
    {
        case EQUIP_ERR_OK:
            return Cause::None;
        case EQUIP_ERR_CANT_EQUIP_LEVEL_I:
            return Cause::Level;
        case EQUIP_ERR_CANT_EQUIP_SKILL:
        case EQUIP_ERR_NO_REQUIRED_PROFICIENCY:
            return Cause::Skill;
        case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM:
            return Cause::Class;
        case EQUIP_ERR_YOU_ARE_DEAD:
            return Cause::Dead;
        default:
            return Cause::CantUse;
    }
}

// The stack a request means: the one named, the worn piece of the slot, else a worn piece of the entry before one
// in the bags (an item that works only worn)
Item* FindItem(Player* bot, ItemRef const& ref, Record const& record)
{
    if (ref.guid)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid(ref.guid));
        return item && item->GetEntry() == ref.entry ? item : nullptr;
    }

    if (ref.slot != SLOT_BAGS)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, ref.slot);
        return item && item->GetEntry() == ref.entry ? item : nullptr;
    }

    if (record.inventoryType)
        for (std::uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetEntry() == ref.entry)
                    return item;

    return bot->GetItemByEntry(ref.entry);
}

// The power the record's first energize effect restores, MAX_POWERS when it has none
Powers PowerOf(Record const& record)
{
    for (std::size_t i = 0; i < record.effectCount && i < record.effects.size(); ++i)
        if (record.effects[i].effect == RULE_EFFECT_ENERGIZE && record.effects[i].misc >= 0 &&
            record.effects[i].misc < MAX_POWERS)
            return Powers(record.effects[i].misc);
    return MAX_POWERS;
}

BotView ViewOfBot(Player* bot, Record const& record, SpellInfo const* spellInfo, Item* item)
{
    Needs const& needs = record.needs;
    BotView view;
    view.level = bot->GetLevel();
    view.classBit = bot->getClassMask();
    view.raceBit = bot->getRaceMask();
    view.skillValue = needs.skill ? bot->GetSkillValue(needs.skill) : 0;
    view.hasRequiredSpell = !needs.requiredSpell || bot->HasSpell(needs.requiredSpell);
    view.held = item ? std::max<std::uint32_t>(1, bot->GetItemCount(item->GetEntry())) : 0;
    view.equipped = item && item->IsEquipped();
    if (item)
    {
        view.canUseCause = CauseOfInventory(bot->CanUseItem(item));
        view.canUse = view.canUseCause == Cause::None;
        view.chargesLeft = item->GetSpellCharges(record.spellIndex);
        if (bot->HasSpellItemCooldown(record.spellId, item->GetEntry()))
            view.cooldownLeftMs = std::max<std::uint32_t>(1, bot->GetSpellCooldownDelay(record.spellId));
    }
    view.cooldownLeftMs = std::max(view.cooldownLeftMs, bot->GetSpellCooldownDelay(record.spellId));
    view.potionDrunk = bot->GetLastPotionId() != 0;
    // the handler queues a packet it cannot cast now (the spell queue), so a cast, a waiting request or the global
    // cooldown is busy
    view.busy = bot->IsNonMeleeSpellCast(false, false, true) || !bot->SpellQueue.empty() ||
                (spellInfo && bot->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo));
    view.alive = bot->IsAlive();
    view.inControl = !bot->HasUnitState(UNIT_STATE_LOST_CONTROL);
    view.inFight = bot->IsInCombat();
    view.inArena = bot->InArena();
    view.shapeshifted = bot->GetShapeshiftForm() != FORM_NONE;
    if (needs.outdoors || needs.indoors)
        view.outdoors = bot->IsOutdoors();
    view.placeFits = (!needs.map || needs.map == bot->GetMapId()) &&
                     (!needs.area || needs.area == bot->GetAreaId() || needs.area == bot->GetZoneId()) &&
                     (!spellInfo || spellInfo->CheckLocation(bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(),
                                                             bot) == SPELL_CAST_OK);
    view.x = bot->GetPositionX();
    view.y = bot->GetPositionY();
    view.z = bot->GetPositionZ();
    return view;
}

TargetView ViewOfUnit(Player* bot, Record const& record, Unit* unit, TargetKind kind)
{
    TargetView view;
    view.kind = kind;
    view.found = unit != nullptr;
    if (!unit)
        return view;

    view.isBot = unit == bot;
    view.alive = unit->IsAlive();
    view.friendly = view.isBot || bot->IsFriendlyTo(unit);
    view.hostile = !view.isBot && bot->IsValidAttackTarget(unit);
    view.healthFull = unit->IsFullHealth();
    Powers const power = PowerOf(record);
    view.powerFull = power != MAX_POWERS && unit->GetPower(power) == unit->GetMaxPower(power);
    view.inSight = view.isBot || bot->IsWithinLOSInMap(unit);
    view.yards = view.isBot ? 0.0f : bot->GetDistance(unit);
    return view;
}

TargetView ViewOfItem(Item* target)
{
    TargetView view;
    view.kind = TargetKind::OwnItem;
    ItemTemplate const* proto = target ? target->GetTemplate() : nullptr;
    view.found = proto != nullptr;
    if (!proto)
        return view;

    view.itemClass = static_cast<std::int32_t>(proto->Class);
    view.itemSubClass = proto->SubClass;
    view.itemInvType = proto->InventoryType;
    view.itemLevel = static_cast<std::uint16_t>(proto->ItemLevel);
    view.itemHasCoating = target->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0;
    return view;
}

// The engine's own CheckCast with the cast item and the target, as PlayerbotAI::CanCastSpell sets it up, without its
// leniency (range and sight refuse here)
std::uint32_t EngineCheck(Player* bot, Found const& found)
{
    Unit* oldSelection = bot->GetSelectedUnit();
    Spell* spell = new Spell(bot, found.spellInfo, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    spell->m_CastItem = found.item;
    if (found.unit)
        spell->m_targets.SetUnitTarget(found.unit);
    if (found.itemTarget)
        spell->m_targets.SetItemTarget(found.itemTarget);
    SpellCastResult const result = spell->CheckCast(true);
    delete spell;
    if (oldSelection)
        bot->SetSelection(oldSelection->GetGUID());
    return static_cast<std::uint32_t>(result);
}

Cause Evaluate(Player* bot, Request const& request, Found& found, std::uint32_t* engine)
{
    found.record = SelectRecord(ItemUseIndex::Get().OfItem(request.item.entry), request.spellId);
    if (!found.record)
        return Cause::NoEffect;

    Record const& record = *found.record;
    found.spellInfo = sSpellMgr->GetSpellInfo(record.spellId);
    if (!found.spellInfo)
        return Cause::NoEffect;

    found.item = FindItem(bot, request.item, record);
    BotView const botView = ViewOfBot(bot, record, found.spellInfo, found.item);

    TargetView targetView;
    switch (request.target.kind)
    {
        case TargetKind::None:
            // no target named: the bot, for an item on the bot or one without a target
            if (record.shape == Shape::None || record.shape == Shape::Self)
            {
                found.unit = bot;
                targetView = ViewOfUnit(bot, record, bot, TargetKind::Self);
            }
            break;
        case TargetKind::Self:
            found.unit = bot;
            targetView = ViewOfUnit(bot, record, bot, TargetKind::Self);
            break;
        case TargetKind::Unit:
            found.unit = request.target.guid ? ObjectAccessor::GetUnit(*bot, ObjectGuid(request.target.guid)) : nullptr;
            if (found.unit && !found.unit->IsInWorld())
                found.unit = nullptr;
            targetView = ViewOfUnit(bot, record, found.unit, TargetKind::Unit);
            break;
        case TargetKind::OwnItem:
            found.itemTarget = request.target.guid ? bot->GetItemByGuid(ObjectGuid(request.target.guid)) : nullptr;
            targetView = ViewOfItem(found.itemTarget);
            break;
        default:
            targetView.kind = request.target.kind;  // a spot and an object: WI4
            break;
    }

    Cause const cause = CheapCheck(record, request, botView, targetView);
    if (cause != Cause::None)
        return cause;

    std::uint32_t const result = EngineCheck(bot, found);
    Cause const mapped = CauseOfEngine(result);
    if (mapped == Cause::Engine && engine)
        *engine = result;
    return mapped;
}

// The one builder of CMSG_USE_ITEM: the fields in the handler's order, then the target flag and its packed guid
WorldPacket BuildPacket(Item const* item, std::uint32_t spellId, Record const& record, Unit const* unit,
                        Item const* itemTarget)
{
    WorldPacket packet(CMSG_USE_ITEM, 1 + 1 + 1 + 4 + 8 + 4 + 1 + 4 + 9);
    packet << item->GetBagSlot() << item->GetSlot() << CAST_COUNT << spellId << item->GetGUID() << std::uint32_t(0)
           << std::uint8_t(0);
    if (itemTarget)
        packet << std::uint32_t(TARGET_FLAG_ITEM) << itemTarget->GetGUID().WriteAsPacked();
    else if (unit && record.shape != Shape::None && record.shape != Shape::Self)
        packet << std::uint32_t(TARGET_FLAG_UNIT) << unit->GetGUID().WriteAsPacked();
    else
        packet << std::uint32_t(TARGET_FLAG_NONE);  // the spell's own targets take the caster
    return packet;
}

// What tells that the game cast after the send (Q4): a cast or channel of the spell from this item now runs, or
// the item, a charge, the cooldown, the global cooldown or the potion mark changed
struct Signs
{
    std::uint32_t count{0};
    std::int32_t charges{0};
    std::uint32_t cooldownMs{0};
    bool globalCooldown{false};
    std::uint32_t potion{0};
};

Signs SignsOf(Player* bot, Found const& found, ObjectGuid itemGuid)
{
    Signs signs;
    signs.count = bot->GetItemCount(found.record->itemEntry);
    Item* item = bot->GetItemByGuid(itemGuid);
    signs.charges = item ? item->GetSpellCharges(found.record->spellIndex) : 0;
    signs.cooldownMs = bot->GetSpellCooldownDelay(found.record->spellId);
    signs.globalCooldown = bot->GetGlobalCooldownMgr().HasGlobalCooldown(found.spellInfo);
    signs.potion = bot->GetLastPotionId();
    return signs;
}

bool CastRuns(Player* bot, std::uint32_t spellId, ObjectGuid itemGuid)
{
    for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        if (Spell const* spell = bot->GetCurrentSpell(type))
            if (spell->m_spellInfo->Id == spellId && spell->m_castItemGUID == itemGuid)
                return true;
    return false;
}

void WriteRow(Player* bot, Request const& request, std::string const& result, std::uint32_t entry,
              std::uint32_t spellId, std::uint32_t kind)
{
    std::string const source = std::string(CallerWord(request.caller)) + ":" + result;
    EventLog::Emit(bot, WorldLogFormat::EVENT_ITEM_USE, source.c_str(), entry,
                   static_cast<std::int64_t>(spellId) + KIND_FACTOR * kind);
    if (request.caller == Caller::Test)
        LOG_INFO("playerbots", "ITEM use: {} {} spell {} -> {}", bot->GetName(), entry, spellId, source);
}

std::string RefusalWord(Cause cause, std::uint32_t engine)
{
    if (cause == Cause::Engine)
        return "refused:engine:" + std::to_string(engine);
    return "refused:" + std::string(CauseWord(cause));
}

Request RequestOf(Player* bot, Order const& order)
{
    Request request;
    request.caller = Caller::Test;
    request.item.entry = order.entry;
    request.item.slot = order.slot;
    if (order.slot != SLOT_BAGS)
        if (Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, order.slot))
        {
            request.item.entry = worn->GetEntry();
            request.item.guid = worn->GetGUID().GetRawValue();
        }

    request.target.dead = order.dead;
    switch (order.target)
    {
        case OrderTarget::Shape:
            break;
        case OrderTarget::Self:
            request.target.kind = TargetKind::Self;
            break;
        case OrderTarget::Unit:
        {
            request.target.kind = TargetKind::Unit;
            Player* player = ObjectAccessor::FindPlayerByName(order.unitName, true);
            if (player && player->IsInWorld() && player->GetMap() == bot->GetMap())
                request.target.guid = player->GetGUID().GetRawValue();
            break;
        }
        case OrderTarget::Creature:
            request.target.kind = TargetKind::Unit;
            if (Creature* creature = bot->FindNearestCreature(order.creatureEntry, ORDER_CREATURE_YARDS, !order.dead))
                request.target.guid = creature->GetGUID().GetRawValue();
            break;
        case OrderTarget::Item:
            request.target.kind = TargetKind::OwnItem;
            if (Item* target = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, order.itemSlot))
                request.target.guid = target->GetGUID().GetRawValue();
            break;
    }
    return request;
}
}  // namespace

Cause Check(Player* bot, Request const& request, std::uint32_t* engine)
{
    if (!bot)
        return Cause::NoTarget;
    Found found;
    return Evaluate(bot, request, found, engine);
}

UseResult Use(Player* bot, Request const& request)
{
    UseResult result;
    result.spellId = request.spellId;
    if (!bot)
        return result;

    // the test's nouse: the bot makes no request of its own (not a cause, no row)
    if (request.caller != Caller::Test)
        if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            if (!TestConstraints::UsesItemsOnItsOwn(botAI->testConstraints))
                return result;

    Found found;
    std::uint32_t engine = 0;
    Cause const cause = Evaluate(bot, request, found, &engine);
    std::uint32_t const spellId = found.record ? found.record->spellId : request.spellId;
    std::uint32_t const kind = found.record ? static_cast<std::uint32_t>(Classify(*found.record).kind) : 0;
    result.spellId = spellId;
    if (cause != Cause::None)
    {
        result.cause = cause;
        result.engine = engine;
        WriteRow(bot, request, RefusalWord(cause, engine), request.item.entry, spellId, kind);
        return result;
    }

    // as a client does: stand for a cast or a channel, face the unit
    Record const& record = *found.record;
    if ((record.castMs || record.channeled) && bot->isMoving())
        bot->StopMoving();
    if (found.unit && found.unit != bot)
        ServerFacade::instance().SetFacingTo(bot, found.unit);

    ObjectGuid const itemGuid = found.item->GetGUID();
    Signs const before = SignsOf(bot, found, itemGuid);
    WorldPacket packet = BuildPacket(found.item, spellId, record, found.unit, found.itemTarget);
    bot->GetSession()->HandleUseItemOpcode(packet);
    found.item = nullptr;  // a consumed item is gone now
    found.itemTarget = nullptr;

    Signs const after = SignsOf(bot, found, itemGuid);
    bool const runs = CastRuns(bot, spellId, itemGuid);
    bool const cast = runs || after.count < before.count || after.charges != before.charges ||
                      after.cooldownMs > before.cooldownMs || (after.globalCooldown && !before.globalCooldown) ||
                      after.potion != before.potion;
    if (request.caller == Caller::Test)
        LOG_INFO("playerbots",
                 "ITEM use: {} {} signs runs={} count {}->{} charges {}->{} cd {}->{} gcd {}->{} "
                 "potion {}->{}",
                 bot->GetName(), request.item.entry, runs, before.count, after.count, before.charges, after.charges,
                 before.cooldownMs, after.cooldownMs, before.globalCooldown, after.globalCooldown, before.potion,
                 after.potion);
    if (!cast)
    {
        result.cause = Cause::Handler;
        WriteRow(bot, request, RefusalWord(Cause::Handler, 0), request.item.entry, spellId, kind);
        return result;
    }

    result.outcome = Outcome::Used;
    WriteRow(bot, request, "used", request.item.entry, spellId, kind);
    return result;
}

Request OnSelf(Item* item, Caller caller)
{
    Request request;
    request.caller = caller;
    request.target.kind = TargetKind::Self;
    if (!item)
        return request;
    request.item.entry = item->GetEntry();
    request.item.guid = item->GetGUID().GetRawValue();
    request.item.slot = item->IsEquipped() ? item->GetSlot() : SLOT_BAGS;
    return request;
}

Request OnUnit(Item* item, Unit* target, Caller caller)
{
    Request request = OnSelf(item, caller);
    request.target.kind = TargetKind::Unit;
    request.target.guid = target ? target->GetGUID().GetRawValue() : 0;
    return request;
}

Request OnItem(Item* item, Item* target, Caller caller)
{
    Request request = OnSelf(item, caller);
    request.target.kind = TargetKind::OwnItem;
    request.target.guid = target ? target->GetGUID().GetRawValue() : 0;
    return request;
}

void PostOrder(PlayerbotAI* botAI, std::uint64_t botGuid, Order order)
{
    if (!botAI)
        return;
    order.postedMs = getMSTime();
    std::lock_guard<std::mutex> guard(ordersLock);
    orders[botGuid].push_back(std::move(order));
    botAI->AddWork(BotWork::ItemOrder);
}

void TakeOrders(PlayerbotAI* botAI, Player* bot)
{
    std::vector<Order> taken;
    {
        std::lock_guard<std::mutex> guard(ordersLock);
        auto const found = orders.find(bot->GetGUID().GetRawValue());
        if (found != orders.end())
        {
            taken = std::move(found->second);
            orders.erase(found);
        }
        botAI->EndWork(BotWork::ItemOrder);
    }

    for (Order const& order : taken)
    {
        if (GetMSTimeDiffToNow(order.postedMs) > ORDER_TTL_MS)
        {
            LOG_INFO("playerbots", "ITEM use: {} order {} dropped, not taken within {} s", bot->GetName(), order.entry,
                     ORDER_TTL_MS / IN_MILLISECONDS);
            continue;
        }
        Use(bot, RequestOf(bot, order));
    }
}
}  // namespace ItemUse
