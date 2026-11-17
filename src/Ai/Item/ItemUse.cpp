/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUse.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CensusMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Item.h"
#include "ItemUseIndex.h"
#include "ItemUseKinds.h"
#include "ItemUsePacket.h"
#include "ItemUseRules.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
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
constexpr std::int64_t KIND_FACTOR = 1000000;   // the row's amount: spell + KIND_FACTOR * kind

constexpr float ORDER_OBJECT_YARDS = 100.0f;  // `object <entry>`: the nearest of the template this far
constexpr float MOVED_YD = 0.25f;             // a timed use cancelled this far off its start: the bot moved

std::mutex ordersLock;
std::unordered_map<std::uint64_t, std::vector<Order>> orders;  // bot guid -> its waiting orders, in order
std::atomic<std::uint32_t> pendingUses{0};                     // timed uses between start and row, every map

// What Check found on the bot's map for one request
struct Found
{
    Record const* record{nullptr};
    SpellInfo const* spellInfo{nullptr};
    Item* item{nullptr};
    Unit* unit{nullptr};
    Item* itemTarget{nullptr};
    GameObject* object{nullptr};
    bool spot{false};
    Record worn;  // a worn piece's enchantment record as the piece's (PieceRecord)
};

BotItems* ItemsOf(PlayerbotAI* botAI) { return botAI ? botAI->botItems.get() : nullptr; }

bool SpotFound(Target const& target)
{
    return std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z);
}

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

BotView ViewOfBot(Player* bot, Record const& record, SpellInfo const* spellInfo, Item* item, bool pending)
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
    // cooldown is busy; so is a timed use until its row
    view.busy = pending || bot->IsNonMeleeSpellCast(false, false, true) || !bot->SpellQueue.empty() ||
                (spellInfo && bot->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo));
    view.alive = bot->IsAlive();
    view.lostControl = static_cast<std::uint8_t>(
        (bot->HasUnitState(UNIT_STATE_STUNNED) ? LostControl::Stunned : 0) |
        (bot->HasUnitState(UNIT_STATE_FLEEING) ? LostControl::Fleeing : 0) |
        (bot->HasUnitState(UNIT_STATE_CONFUSED) ? LostControl::Confused : 0) |
        (bot->HasUnitState(UNIT_STATE_LOST_CONTROL & ~UNIT_STATE_CONTROLLED) ? LostControl::Other : 0));
    view.inFight = bot->IsInCombat();
    view.inArena = bot->InArena();
    // a stance is no shapeshift for spells (SpellInfo::CheckShapeshift)
    ShapeshiftForm const form = bot->GetShapeshiftForm();
    SpellShapeshiftFormEntry const* shape = form != FORM_NONE ? sSpellShapeshiftFormStore.LookupEntry(form) : nullptr;
    view.shapeshifted = form != FORM_NONE && !(shape && (shape->flags1 & SHAPESHIFT_FLAG_STANCE));
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

// A spot: yards to it and the engine's line of sight to a destination (Spell::CheckCast)
TargetView ViewOfSpot(Player* bot, Target const& spot)
{
    TargetView view;
    view.kind = TargetKind::Spot;
    view.found = SpotFound(spot);
    if (!view.found)
        return view;
    view.yards = bot->GetExactDist(spot.x, spot.y, spot.z);
    view.inSight = bot->IsWithinLOS(spot.x, spot.y, spot.z, VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_ALL_CHECKS);
    return view;
}

TargetView ViewOfObject(Player* bot, GameObject* object)
{
    TargetView view;
    view.kind = TargetKind::Object;
    view.found = object != nullptr;
    if (!object)
        return view;
    view.yards = bot->GetDistance(object);
    // the static world only: the object's own model would hide its centre (a chest's lid)
    view.inSight = bot->IsWithinLOSInMap(object, VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_CHECK_VMAP);
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
std::uint32_t EngineCheck(Player* bot, Found const& found, Target const& target)
{
    Unit* oldSelection = bot->GetSelectedUnit();
    Spell* spell = new Spell(bot, found.spellInfo, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    spell->m_CastItem = found.item;
    if (found.unit)
        spell->m_targets.SetUnitTarget(found.unit);
    if (found.itemTarget)
        spell->m_targets.SetItemTarget(found.itemTarget);
    if (found.object)
        spell->m_targets.SetGOTarget(found.object);
    if (found.spot)
        spell->m_targets.SetDst(target.x, target.y, target.z, bot->GetOrientation());
    SpellCastResult const result = spell->CheckCast(true);
    delete spell;
    if (oldSelection)
        bot->SetSelection(oldSelection->GetGUID());
    return static_cast<std::uint32_t>(result);
}

// A worn piece without a use of its own: a use spell of one of its enchantments (a tinker), as the piece's record
Record const* WornRecord(Player* bot, Request const& request, Found& found)
{
    if (!request.item.guid && request.item.slot == SLOT_BAGS)
        return nullptr;
    Item* piece = FindItem(bot, request.item, Record{});
    ItemTemplate const* proto = piece ? piece->GetTemplate() : nullptr;
    if (!proto)
        return nullptr;
    for (std::uint32_t slot = PERM_ENCHANTMENT_SLOT; slot < MAX_ENCHANTMENT_SLOT; ++slot)
        if (std::uint32_t const enchantId = piece->GetEnchantmentId(EnchantmentSlot(slot)))
            if (Record const* record = SelectRecord(ItemUseIndex::Get().OfEnchant(enchantId), request.spellId))
            {
                found.worn = PieceRecord(*record, piece->GetEntry(), static_cast<std::uint8_t>(proto->InventoryType));
                return &found.worn;
            }
    return nullptr;
}

Cause Evaluate(Player* bot, Request const& request, Found& found, std::uint32_t* engine)
{
    found.record = SelectRecord(ItemUseIndex::Get().OfItem(request.item.entry), request.spellId);
    if (!found.record)
        found.record = WornRecord(bot, request, found);
    if (!found.record)
        return Cause::NoEffect;

    Record const& record = *found.record;
    found.spellInfo = sSpellMgr->GetSpellInfo(record.spellId);
    if (!found.spellInfo)
        return Cause::NoEffect;

    found.item = FindItem(bot, request.item, record);
    BotItems const* items = ItemsOf(PlayerbotsMgr::instance().GetPlayerbotAI(bot));
    BotView const botView = ViewOfBot(bot, record, found.spellInfo, found.item, items && items->active);

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
        case TargetKind::Spot:
            found.spot = SpotFound(request.target);
            targetView = ViewOfSpot(bot, request.target);
            break;
        case TargetKind::Object:
            found.object =
                request.target.guid ? ObjectAccessor::GetGameObject(*bot, ObjectGuid(request.target.guid)) : nullptr;
            if (found.object && (!found.object->IsInWorld() || !found.object->isSpawned()))
                found.object = nullptr;
            targetView = ViewOfObject(bot, found.object);
            break;
    }

    Cause const cause = CheapCheck(record, request, botView, targetView);
    if (cause != Cause::None)
        return cause;

    std::uint32_t const result = EngineCheck(bot, found, request.target);
    Cause const mapped = CauseOfEngine(result);
    if (mapped == Cause::Engine && engine)
        *engine = result;
    return mapped;
}

static_assert(PACKET_TARGET_NONE == TARGET_FLAG_NONE && PACKET_TARGET_UNIT == TARGET_FLAG_UNIT &&
                  PACKET_TARGET_ITEM == TARGET_FLAG_ITEM && PACKET_TARGET_DEST == TARGET_FLAG_DEST_LOCATION &&
                  PACKET_TARGET_GAMEOBJECT == TARGET_FLAG_GAMEOBJECT,
              "ItemUsePacket.h's masks are the core's TARGET_FLAG_* values");

// The one builder of CMSG_USE_ITEM (its bytes: ItemUsePacket.h, which the parity selftest checks against the old
// builders): a spot is the destination with no transport, then x, y, z; a unit only for a record that names one
WorldPacket BuildPacket(Item const* item, std::uint32_t spellId, Record const& record, Found const& found,
                        Target const& target)
{
    PacketFields fields;
    fields.bag = item->GetBagSlot();
    fields.slot = item->GetSlot();
    fields.spellId = spellId;
    fields.itemGuid = item->GetGUID().GetRawValue();
    if (found.spot)
    {
        fields.targetMask = PACKET_TARGET_DEST;
        fields.x = target.x;
        fields.y = target.y;
        fields.z = target.z;
    }
    else if (found.object)
    {
        fields.targetMask = PACKET_TARGET_GAMEOBJECT;
        fields.targetGuid = found.object->GetGUID().GetRawValue();
    }
    else if (found.itemTarget)
    {
        fields.targetMask = PACKET_TARGET_ITEM;
        fields.targetGuid = found.itemTarget->GetGUID().GetRawValue();
    }
    else if (found.unit && record.shape != Shape::None && record.shape != Shape::Self)
    {
        fields.targetMask = PACKET_TARGET_UNIT;
        fields.targetGuid = found.unit->GetGUID().GetRawValue();
    }
    // else NONE: the spell's own targets take the caster
    std::vector<std::uint8_t> const bytes = UsePacketBytes(fields);
    WorldPacket packet(CMSG_USE_ITEM, bytes.size());
    packet.append(bytes.data(), bytes.size());
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

// The pending spell runs: the engine clears the spell's item guid when it takes a consumed item at the cast
// (Spell::TakeCastItem, before the cast hook and a channel's run), so an empty guid matches too
bool PendingRuns(Spell const* spell, std::uint32_t spellId, std::uint64_t itemGuid)
{
    return spell && spell->m_spellInfo->Id == spellId &&
           (spell->m_castItemGUID.GetRawValue() == itemGuid || spell->m_castItemGUID.IsEmpty());
}

bool CastRuns(Player* bot, std::uint32_t spellId, ObjectGuid itemGuid)
{
    for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        if (Spell const* spell = bot->GetCurrentSpell(type))
            if (spell->m_spellInfo->Id == spellId && spell->m_castItemGUID == itemGuid)
                return true;
    return false;
}

// A channel of the item's spell that the engine cast (past its cast time): the channel runs
bool ChannelCasting(Player* bot, std::uint32_t spellId, ObjectGuid itemGuid)
{
    Spell const* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    return spell && spell->m_spellInfo->Id == spellId && spell->m_castItemGUID == itemGuid &&
           spell->getState() == SPELL_STATE_CASTING;
}

void WriteRow(Player* bot, Caller caller, std::string const& result, std::uint32_t entry, std::uint32_t spellId,
              std::uint32_t kind)
{
    std::string const source = std::string(CallerWord(caller)) + ":" + result;
    EventLog::Emit(bot, WorldLogFormat::EVENT_ITEM_USE, source.c_str(), entry,
                   static_cast<std::int64_t>(spellId) + KIND_FACTOR * kind);
    if (caller == Caller::Test)
        LOG_INFO("playerbots", "ITEM use: {} {} spell {} -> {}", bot->GetName(), entry, spellId, source);
}

// The pending use's row and its end
void Finish(PlayerbotAI* botAI, Player* bot, BotItems& items, UseResult const& end)
{
    std::string const word =
        end.outcome == Outcome::Used ? std::string("used") : "interrupted:" + std::string(InterruptWord(end.interrupt));
    items.active = false;
    pendingUses.fetch_sub(1, std::memory_order_relaxed);
    botAI->EndWork(BotWork::ItemTimed);
    WriteRow(bot, items.pending.caller, word, items.pending.itemEntry, items.pending.spellId, items.kind);
}

// The pending use the spell belongs to (the same spell from the same item), null while none is
BotItems* PendingOf(Spell const* spell, Unit* caster)
{
    if (!pendingUses.load(std::memory_order_relaxed) || !caster || !spell)
        return nullptr;
    Player* player = caster->ToPlayer();
    BotItems* items = player ? ItemsOf(PlayerbotsMgr::instance().GetPlayerbotAI(player)) : nullptr;
    if (!items || !items->active || !PendingRuns(spell, items->pending.spellId, items->itemGuid))
        return nullptr;
    return items;
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
    float const nowhere = std::nanf("");
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
        case OrderTarget::Spot:
            request.target.kind = TargetKind::Spot;
            request.target.x = order.x;
            request.target.y = order.y;
            request.target.z = order.z;
            break;
        case OrderTarget::SpotCreature:
        {
            request.target.kind = TargetKind::Spot;
            Creature* creature = bot->FindNearestCreature(order.creatureEntry, ORDER_CREATURE_YARDS, true);
            request.target.x = creature ? creature->GetPositionX() : nowhere;
            request.target.y = creature ? creature->GetPositionY() : nowhere;
            request.target.z = creature ? creature->GetPositionZ() : nowhere;
            break;
        }
        case OrderTarget::Object:
            request.target.kind = TargetKind::Object;
            if (GameObject* object = bot->FindNearestGameObject(order.objectEntry, ORDER_OBJECT_YARDS))
                request.target.guid = object->GetGUID().GetRawValue();
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
    if (!Allowed(bot, request.caller))
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
        WriteRow(bot, request.caller, RefusalWord(cause, engine), request.item.entry, spellId, kind);
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
    WorldPacket packet = BuildPacket(found.item, spellId, record, found, request.target);
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
        WriteRow(bot, request.caller, RefusalWord(Cause::Handler, 0), request.item.entry, spellId, kind);
        return result;
    }

    // a cast time or a channel that runs: followed to its end (Update), the row comes then
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if ((record.castMs || record.channeled) && runs && botAI)
    {
        if (!botAI->botItems)
            botAI->botItems = std::make_unique<BotItems>();
        BotItems& items = *botAI->botItems;
        items.pending = PendingUse{request.item.entry, spellId, request.caller, getMSTime(),
                                   BoundOf(record, sPlayerbotAIConfig.ItemUse.UseEndGraceMs)};
        items.signs = PendingSigns{};
        items.signs.channeled = record.channeled;
        // a channel without a cast time was cast inside the handler, before its hook could see a pending use
        items.signs.castSeen = record.channeled && ChannelCasting(bot, spellId, itemGuid);
        items.itemGuid = itemGuid.GetRawValue();
        items.kind = kind;
        items.startX = bot->GetPositionX();
        items.startY = bot->GetPositionY();
        items.cancelLooked = false;
        items.active = true;
        pendingUses.fetch_add(1, std::memory_order_relaxed);
        botAI->AddWork(BotWork::ItemTimed);
        result.outcome = Outcome::Pending;
        return result;
    }

    result.outcome = Outcome::Used;
    WriteRow(bot, request.caller, "used", request.item.entry, spellId, kind);
    return result;
}

bool Allowed(Player* bot, Caller caller)
{
    if (!bot)
        return false;
    if (caller == Caller::Test)
        return true;
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    return !botAI || TestConstraints::UsesItemsOnItsOwn(botAI->testConstraints);
}

bool Pending(Player* bot)
{
    BotItems const* items = bot ? ItemsOf(PlayerbotsMgr::instance().GetPlayerbotAI(bot)) : nullptr;
    return items && items->active;
}

void Update(PlayerbotAI* botAI, Player* bot, std::uint32_t nowMs)
{
    BotItems* items = ItemsOf(botAI);
    if (!items || !items->active)
    {
        botAI->EndWork(BotWork::ItemTimed);
        return;
    }
    items->signs.alive = bot->IsAlive();
    items->signs.runs =
        PendingRuns(bot->GetCurrentSpell(CURRENT_GENERIC_SPELL), items->pending.spellId, items->itemGuid) ||
        PendingRuns(bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL), items->pending.spellId, items->itemGuid);
    // a cancel with the bot off its place is a move: the engine moves no casting bot by a walk order, and a throw (a
    // knockback, a jump) or a teleport stops the spell before the bot's first step, so the place is read one update on
    if (items->signs.cancelSeen && !items->signs.movedAtCancel && !bot->GetTransport())
    {
        if (bot->GetExactDist2d(items->startX, items->startY) > MOVED_YD)
            items->signs.movedAtCancel = true;
        else if (!items->cancelLooked)
        {
            items->cancelLooked = true;
            return;
        }
    }
    if (std::optional<UseResult> const end = Advance(items->pending, EventOf(items->signs), nowMs))
        Finish(botAI, bot, *items, *end);
}

void EndPending(PlayerbotAI* botAI, Player* bot, UseEvent event)
{
    BotItems* items = ItemsOf(botAI);
    if (!bot || !items || !items->active)
        return;
    if (std::optional<UseResult> const end = Advance(items->pending, event, getMSTime()))
        Finish(botAI, bot, *items, *end);
}

void OnSpellCast(Spell const* spell, Unit* caster)
{
    if (BotItems* items = PendingOf(spell, caster))
        items->signs.castSeen = true;
}

void OnSpellCastCancel(Spell const* spell, Unit* caster)
{
    if (BotItems* items = PendingOf(spell, caster))
    {
        items->signs.cancelSeen = true;
        items->signs.movedAtCancel = caster->isMoving();
        items->signs.controlLostAtCancel = caster->HasUnitState(UNIT_STATE_LOST_CONTROL);
    }
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

Request AtSpot(Item* item, Position const& spot, Caller caller)
{
    Request request = OnSelf(item, caller);
    request.target.kind = TargetKind::Spot;
    request.target.x = spot.GetPositionX();
    request.target.y = spot.GetPositionY();
    request.target.z = spot.GetPositionZ();
    return request;
}

Request OnObject(Item* item, GameObject* target, Caller caller)
{
    Request request = OnSelf(item, caller);
    request.target.kind = TargetKind::Object;
    request.target.guid = target ? target->GetGUID().GetRawValue() : 0;
    return request;
}

Request Worn(Player* bot, std::uint8_t slot, Caller caller)
{
    Item* piece = bot && slot < EQUIPMENT_SLOT_END ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) : nullptr;
    Request request = OnSelf(piece, caller);
    request.target.kind = TargetKind::None;
    request.item.slot = slot;
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
