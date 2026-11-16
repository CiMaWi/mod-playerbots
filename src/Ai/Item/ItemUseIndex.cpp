/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemUseIndex.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_set>

#include "CreatureData.h"
#include "DBCStores.h"
#include "ItemTemplate.h"
#include "ItemUseFacts.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SharedDefines.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "UnitDefines.h"

namespace Game = ItemUse::Game;

static_assert(Game::MASK_UNIT == TARGET_FLAG_UNIT && Game::MASK_UNIT_RAID == TARGET_FLAG_UNIT_RAID &&
                  Game::MASK_UNIT_PARTY == TARGET_FLAG_UNIT_PARTY && Game::MASK_ITEM == TARGET_FLAG_ITEM &&
                  Game::MASK_DEST_LOCATION == TARGET_FLAG_DEST_LOCATION &&
                  Game::MASK_UNIT_ENEMY == TARGET_FLAG_UNIT_ENEMY && Game::MASK_UNIT_ALLY == TARGET_FLAG_UNIT_ALLY &&
                  Game::MASK_CORPSE_ENEMY == TARGET_FLAG_CORPSE_ENEMY &&
                  Game::MASK_UNIT_DEAD == TARGET_FLAG_UNIT_DEAD && Game::MASK_GAMEOBJECT == TARGET_FLAG_GAMEOBJECT &&
                  Game::MASK_TRADE_ITEM == TARGET_FLAG_TRADE_ITEM &&
                  Game::MASK_GAMEOBJECT_ITEM == TARGET_FLAG_GAMEOBJECT_ITEM &&
                  Game::MASK_CORPSE_ALLY == TARGET_FLAG_CORPSE_ALLY &&
                  Game::MASK_UNIT_MINIPET == TARGET_FLAG_UNIT_MINIPET &&
                  Game::MASK_UNIT_PASSENGER == TARGET_FLAG_UNIT_PASSENGER &&
                  Game::MASK_UNITS == TARGET_FLAG_UNIT_MASK && Game::MASK_CORPSES == TARGET_FLAG_CORPSE_MASK,
              "the target flags ItemUseFacts.h mirrors");
static_assert(Game::OBJECT_NONE == TARGET_OBJECT_TYPE_NONE && Game::OBJECT_SRC == TARGET_OBJECT_TYPE_SRC &&
                  Game::OBJECT_DEST == TARGET_OBJECT_TYPE_DEST && Game::OBJECT_UNIT == TARGET_OBJECT_TYPE_UNIT &&
                  Game::OBJECT_UNIT_AND_DEST == TARGET_OBJECT_TYPE_UNIT_AND_DEST &&
                  Game::OBJECT_GOBJ == TARGET_OBJECT_TYPE_GOBJ &&
                  Game::OBJECT_GOBJ_ITEM == TARGET_OBJECT_TYPE_GOBJ_ITEM,
              "the target object types ItemUseFacts.h mirrors");
static_assert(Game::EFFECT_RESURRECT == SPELL_EFFECT_RESURRECT &&
                  Game::EFFECT_ENCHANT_ITEM_TEMPORARY == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY &&
                  Game::EFFECT_RESURRECT_NEW == SPELL_EFFECT_RESURRECT_NEW,
              "the spell effects ItemUseFacts.h mirrors");
static_assert(Game::ATTR0_ONLY_INDOORS == SPELL_ATTR0_ONLY_INDOORS &&
                  Game::ATTR0_ONLY_OUTDOORS == SPELL_ATTR0_ONLY_OUTDOORS &&
                  Game::ATTR0_NOT_SHAPESHIFTED == SPELL_ATTR0_NOT_SHAPESHIFTED &&
                  Game::ATTR0_NOT_IN_COMBAT == SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL &&
                  Game::ATTR4_NOT_IN_ARENA == SPELL_ATTR4_NOT_IN_ARENA_OR_RATED_BATTLEGROUND,
              "the spell attributes ItemUseFacts.h mirrors");
static_assert(Game::SPECIFIC_FOOD == SPELL_SPECIFIC_FOOD && Game::SPECIFIC_DRINK == SPELL_SPECIFIC_DRINK &&
                  Game::SPECIFIC_FOOD_AND_DRINK == SPELL_SPECIFIC_FOOD_AND_DRINK &&
                  Game::SPECIFIC_SCROLL == SPELL_SPECIFIC_SCROLL,
              "the spell specifics ItemUseFacts.h mirrors");
static_assert(Game::GROUP_BATTLE_ELIXIR == SPELL_GROUP_ELIXIR_BATTLE &&
                  Game::GROUP_GUARDIAN_ELIXIR == SPELL_GROUP_ELIXIR_GUARDIAN,
              "the core's elixir groups ItemUseFacts.h mirrors");
static_assert(Game::CLASS_CONSUMABLE == ITEM_CLASS_CONSUMABLE && Game::SUBCLASS_POTION == ITEM_SUBCLASS_POTION &&
                  Game::CLASS_MASK_PLAYABLE == CLASSMASK_ALL_PLAYABLE,
              "the item values ItemUseFacts.h mirrors");

namespace
{
std::unique_ptr<ItemUseIndex const> published;  // written once by Build, before any map thread runs

template <class T>
T Clamped(std::uint64_t value)
{
    return static_cast<T>(std::min<std::uint64_t>(value, std::numeric_limits<T>::max()));
}

ItemUse::SpellEffectData EffectOf(SpellEffectInfo const& effect)
{
    ItemUse::SpellEffectData data;
    data.effect = Clamped<std::uint16_t>(effect.Effect);
    data.aura = Clamped<std::uint16_t>(effect.ApplyAuraName);
    data.basePoints = effect.BasePoints;
    data.dieSides = effect.DieSides;
    data.amplitude = effect.Amplitude;
    data.misc = effect.MiscValue;
    data.miscB = effect.MiscValueB;
    data.triggerSpell = effect.TriggerSpell;
    data.itemType = effect.ItemType;
    data.targetA = Clamped<std::uint8_t>(effect.TargetA.GetTarget());
    data.targetB = Clamped<std::uint8_t>(effect.TargetB.GetTarget());
    data.objectA = Clamped<std::uint8_t>(effect.TargetA.GetObjectType());
    data.objectB = Clamped<std::uint8_t>(effect.TargetB.GetObjectType());
    data.usedObject = Clamped<std::uint8_t>(effect.GetUsedTargetObjectType());
    data.mechanic = Clamped<std::uint8_t>(effect.Mechanic);
    data.radiusYards = effect.HasRadius() ? effect.CalcRadius() : 0.0f;
    return data;
}

// The spell_group ids of the spell's first rank (the store keeps only first ranks; groups of groups are expanded)
void AddGroups(SpellInfo const* spell, std::vector<std::uint32_t>& groups)
{
    SpellInfo const* first = spell->GetFirstRankSpell();
    SpellSpellGroupMapBounds const bounds = sSpellMgr->GetSpellSpellGroupMapBounds(first ? first->Id : spell->Id);
    for (auto group = bounds.first; group != bounds.second; ++group)
        groups.push_back(static_cast<std::uint32_t>(group->second));
}

ItemUse::SpellData SpellDataOf(SpellInfo const* spell)
{
    ItemUse::SpellData data;
    data.id = spell->Id;
    data.attributes = {spell->Attributes,    spell->AttributesEx,  spell->AttributesEx2, spell->AttributesEx3,
                       spell->AttributesEx4, spell->AttributesEx5, spell->AttributesEx6, spell->AttributesEx7};
    data.explicitMask = spell->GetExplicitTargetMask();
    AddGroups(spell, data.groups);
    for (SpellEffectInfo const& effect : spell->GetEffects())
    {
        if (!effect.IsEffect())
            continue;
        data.effects.push_back(EffectOf(effect));
        SpellInfo const* triggered = effect.TriggerSpell ? sSpellMgr->GetSpellInfo(effect.TriggerSpell) : nullptr;
        if (!triggered)
            continue;
        for (SpellEffectInfo const& triggeredEffect : triggered->GetEffects())
            if (triggeredEffect.IsEffect())
                data.triggered.push_back(EffectOf(triggeredEffect));
        AddGroups(triggered, data.groups);
    }
    data.minRangeHostile = spell->GetMinRange(false);
    data.minRangeFriend = spell->GetMinRange(true);
    data.maxRangeHostile = spell->GetMaxRange(false);
    data.maxRangeFriend = spell->GetMaxRange(true);
    data.castMs = spell->CalcCastTime();
    int32 const duration = spell->GetDuration();
    data.durationMs = duration < 0 ? Game::DURATION_ENDLESS : static_cast<std::uint32_t>(duration);
    data.category = spell->GetCategory();
    data.recoveryMs = spell->RecoveryTime;
    data.categoryRecoveryMs = spell->CategoryRecoveryTime;
    data.channeled = spell->IsChanneled();
    data.castMoveInterrupt = spell->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT;
    data.channelMoveInterrupt = spell->ChannelInterruptFlags & AURA_INTERRUPT_FLAG_MOVE;
    data.spellFocus = spell->RequiresSpellFocus;
    data.areaGroup = spell->AreaGroupId > 0 ? static_cast<std::uint32_t>(spell->AreaGroupId) : 0;
    data.baseLevel = Clamped<std::uint16_t>(spell->BaseLevel);
    data.maxLevel = Clamped<std::uint16_t>(spell->MaxLevel);
    data.equippedItemClass = spell->EquippedItemClass;
    data.equippedItemSubClassMask = static_cast<std::uint32_t>(std::max(spell->EquippedItemSubClassMask, 0));
    data.equippedItemInvTypeMask = static_cast<std::uint32_t>(std::max(spell->EquippedItemInventoryTypeMask, 0));
    data.schoolMask = spell->SchoolMask;
    data.dispel = Clamped<std::uint8_t>(spell->Dispel);
    data.specific = Clamped<std::uint8_t>(spell->GetSpellSpecific());
    data.positive = spell->IsPositive();
    data.cooldownOnEvent = spell->IsCooldownStartedOnEvent();
    data.requiresDead = spell->IsRequiringDeadTarget();
    data.allowsDead = spell->IsAllowingDeadTarget();
    return data;
}

// Items a vendor sells for gold, over every creature template with the vendor flag (the price is the item's,
// wherever its vendor stands)
std::unordered_set<std::uint32_t> VendorSoldItems()
{
    std::unordered_set<std::uint32_t> sold;
    for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
    {
        if (!(creature.npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
        if (!items)
            continue;
        for (VendorItem const* offer : items->m_items)
        {
            if (!offer)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer->item);
            if (proto && offer->IsGoldRequired(proto))
                sold.insert(offer->item);
        }
    }
    return sold;
}

ItemUse::ItemData ItemDataOf(ItemTemplate const& proto, std::uint8_t spellIndex, bool vendorSold)
{
    _Spell const& row = proto.Spells[spellIndex];
    ItemUse::ItemData data;
    data.source = ItemUse::Source::ItemSpell;
    data.itemEntry = proto.ItemId;
    data.spellIndex = spellIndex;
    data.itemClass = Clamped<std::uint8_t>(proto.Class);
    data.itemSubClass = Clamped<std::uint8_t>(proto.SubClass);
    data.inventoryType = Clamped<std::uint8_t>(proto.InventoryType);
    data.itemLevel = Clamped<std::uint16_t>(proto.ItemLevel);
    data.conjured = proto.HasFlag(ITEM_FLAG_CONJURED);
    data.notInArena = proto.HasFlag(ITEM_FLAG_NOT_USEABLE_IN_ARENA);
    data.requiredLevel = Clamped<std::uint8_t>(proto.RequiredLevel);
    data.skill = Clamped<std::uint16_t>(proto.RequiredSkill);
    data.skillRank = Clamped<std::uint16_t>(proto.RequiredSkillRank);
    data.classMask = proto.AllowableClass;
    data.raceMask = proto.AllowableRace;
    data.requiredSpell = proto.RequiredSpell;
    data.map = proto.Map;
    data.area = proto.Area;
    data.stackable = proto.Stackable;
    data.charges = row.SpellCharges;
    data.cooldownMs = row.SpellCooldown;
    data.category = row.SpellCategory;
    data.categoryCooldownMs = row.SpellCategoryCooldown;
    data.buyPrice = static_cast<std::uint32_t>(std::max(proto.BuyPrice, 0));
    data.buyCount = std::max<std::uint32_t>(proto.BuyCount, 1);
    data.sellPrice = proto.SellPrice;
    data.vendorSold = vendorSold;
    return data;
}

ItemUse::ItemData EnchantDataOf(SpellItemEnchantmentEntry const& enchant, std::uint8_t effectIndex)
{
    ItemUse::ItemData data;
    data.source = ItemUse::Source::EnchantSpell;
    data.enchantId = enchant.ID;
    data.spellIndex = effectIndex;
    data.requiredLevel = Clamped<std::uint8_t>(enchant.requiredLevel);
    data.skill = Clamped<std::uint16_t>(enchant.requiredSkill);
    data.skillRank = Clamped<std::uint16_t>(enchant.requiredSkillValue);
    data.charges = Clamped<std::int32_t>(enchant.charges);
    return data;
}

bool IsUseTrigger(std::uint32_t trigger)
{
    return trigger == ITEM_SPELLTRIGGER_ON_USE || trigger == ITEM_SPELLTRIGGER_ON_NO_DELAY_USE;
}
}  // namespace

void ItemUseIndex::Build()
{
    if (published)
        return;

    uint32 const startMs = getMSTime();
    std::unique_ptr<ItemUseIndex> built = std::make_unique<ItemUseIndex>();
    std::unordered_set<std::uint32_t> const vendorSold = VendorSoldItems();

    auto const add = [&built](ItemUse::ItemData const& item, SpellInfo const* spell, std::string name)
    {
        ItemUse::Record const record = ItemUse::MakeRecord(item, SpellDataOf(spell), WorldTravel::BUY_PER_SELL_PRICE);
        built->_stats.cut += record.effectsCut ? 1 : 0;
        built->_records.push_back(record);
        built->_names.push_back(std::move(name));
    };

    for (ItemTemplate const* proto : *sObjectMgr->GetItemTemplateStoreFast())
    {
        if (!proto)
            continue;
        std::uint32_t const first = static_cast<std::uint32_t>(built->_records.size());
        for (std::uint8_t slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            _Spell const& row = proto->Spells[slot];
            if (row.SpellId <= 0 || !IsUseTrigger(row.SpellTrigger))
                continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(row.SpellId);
            if (!spell)
            {
                ++built->_stats.missingSpells;
                continue;
            }
            add(ItemDataOf(*proto, slot, vendorSold.count(proto->ItemId) > 0), spell, proto->Name1);
        }
        std::uint32_t const count = static_cast<std::uint32_t>(built->_records.size()) - first;
        if (count)
            built->_items.emplace(proto->ItemId, Span{first, count});
    }
    built->_stats.itemRecords = static_cast<std::uint32_t>(built->_records.size());

    for (uint32 id = 0; id < sSpellItemEnchantmentStore.GetNumRows(); ++id)
    {
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(id);
        if (!enchant)
            continue;
        std::uint32_t const first = static_cast<std::uint32_t>(built->_records.size());
        for (std::uint8_t effect = 0; effect < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++effect)
        {
            if (enchant->type[effect] != ITEM_ENCHANTMENT_TYPE_USE_SPELL || !enchant->spellid[effect])
                continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(enchant->spellid[effect]);
            if (!spell)
            {
                ++built->_stats.missingSpells;
                continue;
            }
            char const* description = enchant->description[0];
            add(EnchantDataOf(*enchant, effect), spell, description ? description : "");
        }
        std::uint32_t const count = static_cast<std::uint32_t>(built->_records.size()) - first;
        if (count)
            built->_enchants.emplace(enchant->ID, Span{first, count});
    }
    built->_stats.enchantRecords = static_cast<std::uint32_t>(built->_records.size()) - built->_stats.itemRecords;
    built->_stats.buildMs = GetMSTimeDiffToNow(startMs);

    LOG_INFO("server.loading", "Item use index: {} records ({} item, {} enchantment), {} cut, {} without spell, {} ms",
             built->_records.size(), built->_stats.itemRecords, built->_stats.enchantRecords, built->_stats.cut,
             built->_stats.missingSpells, built->_stats.buildMs);
    published = std::move(built);
}

ItemUseIndex const& ItemUseIndex::Get()
{
    static ItemUseIndex const empty;
    return published ? *published : empty;
}

std::span<ItemUse::Record const> ItemUseIndex::SpanOf(std::unordered_map<std::uint32_t, Span> const& spans,
                                                      std::uint32_t key) const
{
    auto const found = spans.find(key);
    if (found == spans.end())
        return {};
    return std::span<ItemUse::Record const>(_records).subspan(found->second.first, found->second.second);
}

std::span<ItemUse::Record const> ItemUseIndex::OfItem(std::uint32_t itemEntry) const
{
    return SpanOf(_items, itemEntry);
}

std::span<ItemUse::Record const> ItemUseIndex::OfEnchant(std::uint32_t enchantId) const
{
    return SpanOf(_enchants, enchantId);
}

ItemUse::Record const* ItemUseIndex::At(std::uint32_t id) const
{
    return id < _records.size() ? &_records[id] : nullptr;
}

std::uint32_t ItemUseIndex::IdOf(ItemUse::Record const& record) const
{
    if (_records.empty() || &record < _records.data() || &record >= _records.data() + _records.size())
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(&record - _records.data());
}

std::string_view ItemUseIndex::NameOf(std::uint32_t id) const
{
    return id < _names.size() ? std::string_view(_names[id]) : std::string_view();
}
