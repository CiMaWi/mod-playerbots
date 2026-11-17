/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSEFACTS_H
#define _PLAYERBOT_ITEMUSEFACTS_H

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "ItemUseTypes.h"

// The pure half of the item use index (item-use plan WI2, INTERFACE.md 2.1): std-only copies of what the derivations
// read of a spell and of an item template or enchantment, the derivations that make a Record of them, and the dump's
// line format, which the server's dump and the selftests that read the committed fixture share. Std only, so
// tests/item-use-index-selftest.cpp builds it alone; ItemUseIndex.cpp fills the copies from the core's stores.
namespace ItemUse
{
// The game's values the derivations read, mirrored here; ItemUseIndex.cpp asserts each against the core's name
namespace Game
{
// SpellCastTargetFlags
constexpr std::uint32_t MASK_UNIT = 0x00000002;
constexpr std::uint32_t MASK_UNIT_RAID = 0x00000004;
constexpr std::uint32_t MASK_UNIT_PARTY = 0x00000008;
constexpr std::uint32_t MASK_ITEM = 0x00000010;
constexpr std::uint32_t MASK_DEST_LOCATION = 0x00000040;
constexpr std::uint32_t MASK_UNIT_ENEMY = 0x00000080;
constexpr std::uint32_t MASK_UNIT_ALLY = 0x00000100;
constexpr std::uint32_t MASK_CORPSE_ENEMY = 0x00000200;
constexpr std::uint32_t MASK_UNIT_DEAD = 0x00000400;
constexpr std::uint32_t MASK_GAMEOBJECT = 0x00000800;
constexpr std::uint32_t MASK_TRADE_ITEM = 0x00001000;
constexpr std::uint32_t MASK_GAMEOBJECT_ITEM = 0x00004000;
constexpr std::uint32_t MASK_CORPSE_ALLY = 0x00008000;
constexpr std::uint32_t MASK_UNIT_MINIPET = 0x00010000;
constexpr std::uint32_t MASK_UNIT_PASSENGER = 0x00100000;
constexpr std::uint32_t MASK_UNITS = MASK_UNIT | MASK_UNIT_RAID | MASK_UNIT_PARTY | MASK_UNIT_ENEMY | MASK_UNIT_ALLY |
                                     MASK_UNIT_DEAD | MASK_UNIT_MINIPET | MASK_UNIT_PASSENGER;
constexpr std::uint32_t MASK_CORPSES = MASK_CORPSE_ALLY | MASK_CORPSE_ENEMY;
constexpr std::uint32_t MASK_FRIENDLY =
    MASK_UNIT_RAID | MASK_UNIT_PARTY | MASK_UNIT_ALLY | MASK_CORPSE_ALLY | MASK_UNIT_MINIPET | MASK_UNIT_PASSENGER;
constexpr std::uint32_t MASK_HOSTILE = MASK_UNIT_ENEMY | MASK_CORPSE_ENEMY;

// SpellTargetObjectTypes
constexpr std::uint8_t OBJECT_NONE = 0;
constexpr std::uint8_t OBJECT_SRC = 1;
constexpr std::uint8_t OBJECT_DEST = 2;
constexpr std::uint8_t OBJECT_UNIT = 3;
constexpr std::uint8_t OBJECT_UNIT_AND_DEST = 4;
constexpr std::uint8_t OBJECT_GOBJ = 5;
constexpr std::uint8_t OBJECT_GOBJ_ITEM = 6;

// SpellEffects
constexpr std::uint16_t EFFECT_RESURRECT = 18;
constexpr std::uint16_t EFFECT_ENCHANT_ITEM_TEMPORARY = 54;
constexpr std::uint16_t EFFECT_RESURRECT_NEW = 113;

// Spell attributes (Attributes is word 0, AttributesEx4 word 4)
constexpr std::uint32_t ATTR0_ONLY_INDOORS = 0x00004000;
constexpr std::uint32_t ATTR0_ONLY_OUTDOORS = 0x00008000;
constexpr std::uint32_t ATTR0_NOT_SHAPESHIFTED = 0x00010000;
constexpr std::uint32_t ATTR0_NOT_IN_COMBAT = 0x10000000;  // SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL
constexpr std::uint32_t ATTR4_NOT_IN_ARENA = 0x00010000;   // SPELL_ATTR4_NOT_IN_ARENA_OR_RATED_BATTLEGROUND

// SpellSpecificType
constexpr std::uint8_t SPECIFIC_FOOD = 19;
constexpr std::uint8_t SPECIFIC_DRINK = 20;
constexpr std::uint8_t SPECIFIC_FOOD_AND_DRINK = 21;
constexpr std::uint8_t SPECIFIC_SCROLL = 24;

// spell_group ids: 1 and 2 are the core's SpellGroup enum, 1001 the world DB's well fed group; members come from the
// loaded stores at the build, never from a list here
constexpr std::uint32_t GROUP_BATTLE_ELIXIR = 1;
constexpr std::uint32_t GROUP_GUARDIAN_ELIXIR = 2;
constexpr std::uint32_t GROUP_WELL_FED = 1001;

// Items
constexpr std::uint8_t CLASS_CONSUMABLE = 0;
constexpr std::uint8_t SUBCLASS_POTION = 1;
constexpr std::uint32_t CLASS_MASK_PLAYABLE = 0x5FF;  // CLASSMASK_ALL_PLAYABLE
constexpr std::uint32_t RACE_MASK_PLAYABLE = 0x6FF;   // the ten playable races of 3.3.5

constexpr std::uint32_t DURATION_ENDLESS = std::numeric_limits<std::uint32_t>::max();  // SpellInfo::GetDuration -1
}  // namespace Game

// One effect of a spell, as SpellEffectInfo holds it
struct SpellEffectData
{
    std::uint16_t effect{0};
    std::uint16_t aura{0};
    std::int32_t basePoints{0};
    std::int32_t dieSides{0};
    std::uint32_t amplitude{0};
    std::int32_t misc{0};
    std::int32_t miscB{0};
    std::uint32_t triggerSpell{0};
    std::uint32_t itemType{0};
    std::uint8_t targetA{0};
    std::uint8_t targetB{0};
    std::uint8_t objectA{0};     // SpellTargetObjectTypes of TargetA
    std::uint8_t objectB{0};     // and of TargetB
    std::uint8_t usedObject{0};  // what the effect lands on without target entries (GetUsedTargetObjectType)
    std::uint8_t mechanic{0};
    float radiusYards{0.0f};  // CalcRadius without a caster, 0: no area
};

// What the derivations read of a SpellInfo
struct SpellData
{
    std::uint32_t id{0};
    std::array<std::uint32_t, ATTRIBUTE_WORDS> attributes{};
    std::uint32_t explicitMask{0};           // GetExplicitTargetMask: Targets merged with the effects' implicit ones
    std::vector<SpellEffectData> effects;    // its own effects, in index order
    std::vector<SpellEffectData> triggered;  // the effects of the spells its effects trigger, one level deep
    std::vector<std::uint32_t> groups;       // spell_group ids of its first rank and of its triggered spells'
    float minRangeHostile{0.0f};
    float minRangeFriend{0.0f};
    float maxRangeHostile{0.0f};
    float maxRangeFriend{0.0f};
    std::uint32_t castMs{0};
    std::uint32_t durationMs{0};  // Game::DURATION_ENDLESS: until cancelled
    std::uint32_t category{0};
    std::uint32_t recoveryMs{0};
    std::uint32_t categoryRecoveryMs{0};
    bool channeled{false};
    bool castMoveInterrupt{false};     // InterruptFlags: moving breaks the cast
    bool channelMoveInterrupt{false};  // ChannelInterruptFlags: moving breaks the channel
    std::uint32_t spellFocus{0};
    std::uint32_t areaGroup{0};
    std::uint16_t baseLevel{0};
    std::uint16_t maxLevel{0};
    bool allowLowLevelBuff{false};  // SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF: the engine sets no level floor on the target
    std::int32_t equippedItemClass{-1};
    std::uint32_t equippedItemSubClassMask{0};
    std::uint32_t equippedItemInvTypeMask{0};
    std::uint32_t schoolMask{0};
    std::uint8_t dispel{0};
    std::uint8_t specific{0};  // SpellSpecificType
    bool positive{true};
    bool cooldownOnEvent{false};  // IsCooldownStartedOnEvent
    bool requiresDead{false};     // IsRequiringDeadTarget
    bool allowsDead{false};       // IsAllowingDeadTarget
};

// What the derivations read of an item template and its spell row, or of an enchantment
struct ItemData
{
    Source source{Source::ItemSpell};
    std::uint32_t itemEntry{0};
    std::uint32_t enchantId{0};
    std::uint8_t spellIndex{0};
    std::uint8_t itemClass{0};
    std::uint8_t itemSubClass{0};
    std::uint8_t inventoryType{0};
    std::uint16_t itemLevel{0};
    bool conjured{false};
    bool notInArena{false};  // ITEM_FLAG_NOT_USEABLE_IN_ARENA
    std::uint8_t requiredLevel{0};
    std::uint16_t skill{0};
    std::uint16_t skillRank{0};
    std::uint32_t classMask{0};
    std::uint32_t raceMask{0};
    std::uint32_t requiredSpell{0};
    std::uint32_t map{0};
    std::uint32_t area{0};
    std::int32_t stackable{1};
    std::int32_t charges{0};              // the spell row's SpellCharges, or the enchantment's charges
    std::int32_t cooldownMs{-1};          // the spell row's SpellCooldown, -1: none of its own
    std::uint32_t category{0};            // SpellCategory
    std::int32_t categoryCooldownMs{-1};  // SpellCategoryCooldown, -1: none of its own
    std::uint32_t buyPrice{0};
    std::uint32_t buyCount{1};
    std::uint32_t sellPrice{0};
    bool vendorSold{false};  // a vendor sells it for gold
};

// The average of an effect's points, as a tooltip shows its range (BasePoints + 1 .. BasePoints + DieSides)
constexpr std::int32_t AverageAmount(SpellEffectData const& effect)
{
    return effect.dieSides > 0 ? effect.basePoints + (1 + effect.dieSides) / 2 : effect.basePoints;
}

// 0 when a class or race mask holds every playable one
constexpr std::uint32_t AnyIfAll(std::uint32_t mask, std::uint32_t all) { return (mask & all) == all ? 0 : mask; }

// What the packet's target must be (INTERFACE.md 1.1): the explicit target mask first, an own item before a unit,
// a unit before an object, an object before a spot; without one, Self when an effect lands on units from the caster
// (the caster, its group, an area around it), None when nothing is aimed (a summon, a spot at the caster's feet)
inline Shape ShapeOf(SpellData const& spell)
{
    std::uint32_t const mask = spell.explicitMask;
    if (mask & (Game::MASK_ITEM | Game::MASK_TRADE_ITEM))
        return Shape::OwnItem;
    if (mask & (Game::MASK_UNITS | Game::MASK_CORPSES))
    {
        bool const friendly = mask & Game::MASK_FRIENDLY;
        bool const hostile = mask & Game::MASK_HOSTILE;
        if (friendly && !hostile)
            return Shape::Friend;
        if (hostile && !friendly)
            return Shape::Enemy;
        return Shape::AnyUnit;
    }
    if (mask & (Game::MASK_GAMEOBJECT | Game::MASK_GAMEOBJECT_ITEM))
        return Shape::Object;
    if (mask & Game::MASK_DEST_LOCATION)
        return Shape::Ground;

    auto const isUnit = [](std::uint8_t object)
    { return object == Game::OBJECT_UNIT || object == Game::OBJECT_UNIT_AND_DEST; };
    for (SpellEffectData const& effect : spell.effects)
    {
        if (isUnit(effect.objectA) || isUnit(effect.objectB))
            return Shape::Self;
        if (!effect.targetA && !effect.targetB && isUnit(effect.usedObject))
            return Shape::Self;
    }
    return Shape::None;
}

// What a unit target must be: dead for a resurrection or a ghost-only spell, either when the spell allows the dead
inline Life LifeOf(SpellData const& spell)
{
    if (spell.requiresDead)
        return Life::Dead;
    for (SpellEffectData const& effect : spell.effects)
        if (effect.effect == Game::EFFECT_RESURRECT || effect.effect == Game::EFFECT_RESURRECT_NEW)
            return Life::Dead;
    return spell.allowsDead ? Life::Either : Life::Alive;
}

// What a long effect excludes by its kind (INTERFACE.md 1.2); a food that feeds well is WellFed (its buff's group)
inline Exclude ExcludeOf(SpellData const& spell, Shape shape)
{
    auto const inGroup = [&spell](std::uint32_t group)
    { return std::find(spell.groups.begin(), spell.groups.end(), group) != spell.groups.end(); };
    bool const battle = inGroup(Game::GROUP_BATTLE_ELIXIR);
    bool const guardian = inGroup(Game::GROUP_GUARDIAN_ELIXIR);
    if (battle && guardian)
        return Exclude::Flask;
    if (battle)
        return Exclude::BattleElixir;
    if (guardian)
        return Exclude::GuardianElixir;
    if (inGroup(Game::GROUP_WELL_FED))
        return Exclude::WellFed;
    if (spell.specific == Game::SPECIFIC_SCROLL)
        return Exclude::Scroll;
    if (shape == Shape::OwnItem)
        for (SpellEffectData const& effect : spell.effects)
            if (effect.effect == Game::EFFECT_ENCHANT_ITEM_TEMPORARY)
                return Exclude::Coating;
    if (spell.specific == Game::SPECIFIC_FOOD || spell.specific == Game::SPECIFIC_FOOD_AND_DRINK)
        return Exclude::Food;
    if (spell.specific == Game::SPECIFIC_DRINK)
        return Exclude::Drink;
    return Exclude::None;
}

// A use costs an item or a charge that does not come back: no item charge recharges in the core (Spell::TakeCastItem),
// so every limited charge count is consumed; 0 is unlimited
constexpr bool ConsumedOf(ItemData const& item) { return item.charges != 0; }

// One per fight: a potion, or a spell whose cooldown starts when the fight ends
constexpr bool PotionOf(ItemData const& item, SpellData const& spell)
{
    bool const potionItem = item.source == Source::ItemSpell && item.itemClass == Game::CLASS_CONSUMABLE &&
                            item.itemSubClass == Game::SUBCLASS_POTION;
    return potionItem || spell.cooldownOnEvent;
}

// Uses one item gives: its charge count; a stack's charges are not kept per item (Stackable above 1), so one use each
constexpr std::uint32_t UsesPerItem(ItemData const& item)
{
    if (item.charges == 0 || item.stackable != 1)
        return 1;
    return static_cast<std::uint32_t>(item.charges < 0 ? -static_cast<std::int64_t>(item.charges) : item.charges);
}

// Cooldown, category and category cooldown: the item row's when it has one of its own, else the spell's
// (Player::AddSpellAndCategoryCooldowns)
struct Readiness
{
    std::uint32_t cooldownMs{0};
    std::uint32_t category{0};
    std::uint32_t categoryCooldownMs{0};
};

constexpr Readiness ReadinessOf(ItemData const& item, SpellData const& spell)
{
    if (item.source != Source::ItemSpell || (item.cooldownMs < 0 && item.categoryCooldownMs < 0))
        return {spell.recoveryMs, spell.category, spell.categoryRecoveryMs};
    return {static_cast<std::uint32_t>(std::max(item.cooldownMs, 0)), item.category,
            static_cast<std::uint32_t>(std::max(item.categoryCooldownMs, 0))};
}

inline EffectFact FactOf(SpellEffectData const& effect, bool triggered)
{
    EffectFact fact;
    fact.effect = effect.effect;
    fact.aura = effect.aura;
    fact.amount = AverageAmount(effect);
    fact.misc = effect.misc;
    fact.miscB = effect.miscB;
    fact.periodMs = effect.amplitude;
    fact.trigger = effect.triggerSpell;
    fact.createItem = effect.itemType;
    fact.targetA = effect.targetA;
    fact.targetB = effect.targetB;
    fact.mechanic = effect.mechanic;
    fact.radiusYards = effect.radiusYards;
    fact.triggered = triggered;
    return fact;
}

// The record of one use effect. buyPerSellPrice is WorldTravel::BUY_PER_SELL_PRICE (ItemWorthRules.h), passed in so
// this header stays apart from the world rules
inline Record MakeRecord(ItemData const& item, SpellData const& spell, std::uint32_t buyPerSellPrice)
{
    Record record;
    record.source = item.source;
    record.itemEntry = item.source == Source::ItemSpell ? item.itemEntry : 0;
    record.enchantId = item.source == Source::EnchantSpell ? item.enchantId : 0;
    record.spellIndex = item.spellIndex;
    record.spellId = spell.id;
    record.itemClass = item.itemClass;
    record.itemSubClass = item.itemSubClass;
    record.inventoryType = item.inventoryType;
    record.itemLevel = item.itemLevel;
    record.conjured = item.conjured;

    record.shape = ShapeOf(spell);
    record.life = LifeOf(spell);
    record.positive = spell.positive;
    if (record.shape == Shape::OwnItem)
    {
        record.targetItemClass = spell.equippedItemClass;
        record.targetItemSubClassMask = spell.equippedItemSubClassMask;
        record.targetItemInvTypeMask = spell.equippedItemInvTypeMask;
        // Spell::CheckItems: a poison (the attribute) goes on a weapon of any level
        record.targetItemMinLevel = spell.allowLowLevelBuff ? 0 : spell.baseLevel;
        record.targetItemMaxLevel = spell.maxLevel;
    }

    record.minRangeYards = spell.positive ? spell.minRangeFriend : spell.minRangeHostile;
    record.rangeYards = spell.maxRangeHostile;
    record.rangeFriendYards = spell.maxRangeFriend;
    record.castMs = spell.castMs;
    record.durationMs = spell.durationMs;
    record.channeled = spell.channeled;
    record.moveInterrupts =
        (spell.castMs > 0 && spell.castMoveInterrupt) || (spell.channeled && spell.channelMoveInterrupt);

    Readiness const readiness = ReadinessOf(item, spell);
    record.cooldownMs = readiness.cooldownMs;
    record.category = readiness.category;
    record.categoryCooldownMs = readiness.categoryCooldownMs;
    record.potion = PotionOf(item, spell);

    record.charges = item.charges;
    record.consumed = ConsumedOf(item);

    Needs& needs = record.needs;
    needs.level = item.requiredLevel;
    needs.skill = item.skill;
    needs.skillRank = item.skillRank;
    needs.classMask = AnyIfAll(item.classMask, Game::CLASS_MASK_PLAYABLE);
    needs.raceMask = AnyIfAll(item.raceMask, Game::RACE_MASK_PLAYABLE);
    needs.requiredSpell = item.requiredSpell;
    needs.areaGroup = spell.areaGroup;
    needs.map = item.map;
    needs.area = item.area;
    needs.spellFocus = spell.spellFocus;
    needs.fight = (spell.attributes[0] & Game::ATTR0_NOT_IN_COMBAT) ? FightNeed::Peace : FightNeed::Any;
    needs.notInArena = item.notInArena || (spell.attributes[4] & Game::ATTR4_NOT_IN_ARENA);
    needs.outdoors = spell.attributes[0] & Game::ATTR0_ONLY_OUTDOORS;
    needs.indoors = spell.attributes[0] & Game::ATTR0_ONLY_INDOORS;
    needs.notShapeshifted = spell.attributes[0] & Game::ATTR0_NOT_SHAPESHIFTED;

    std::size_t total = 0;
    auto const addEffects = [&](std::vector<SpellEffectData> const& effects, bool triggered)
    {
        for (SpellEffectData const& effect : effects)
        {
            record.radiusYards = std::max(record.radiusYards, effect.radiusYards);
            if (total < MAX_EFFECTS)
                record.effects[total] = FactOf(effect, triggered);
            ++total;
        }
    };
    addEffects(spell.effects, false);
    addEffects(spell.triggered, true);
    record.effectCount = static_cast<std::uint8_t>(std::min(total, MAX_EFFECTS));
    record.effectsCut = total > MAX_EFFECTS;
    record.attributes = spell.attributes;
    record.schoolMask = spell.schoolMask;
    record.dispelType = spell.dispel;
    record.spellSpecific = spell.specific;

    std::vector<std::uint32_t> groups = spell.groups;
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    record.groupCount = static_cast<std::uint8_t>(std::min(groups.size(), MAX_GROUPS));
    std::copy_n(groups.begin(), record.groupCount, record.groups.begin());
    record.exclude = ExcludeOf(spell, record.shape);

    record.sellCopper = item.source == Source::ItemSpell ? item.sellPrice : 0;
    if (record.consumed && item.source == Source::ItemSpell)
    {
        std::uint64_t perItem = 0;
        if (item.vendorSold)
        {
            record.price = PriceSource::Vendor;
            perItem = item.buyPrice / std::max<std::uint32_t>(item.buyCount, 1);
        }
        else if (item.sellPrice > 0)
        {
            record.price = PriceSource::Derived;
            perItem = static_cast<std::uint64_t>(item.sellPrice) * buyPerSellPrice;
        }
        std::uint64_t const perUse = perItem / UsesPerItem(item);
        record.replaceCopper =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(perUse, std::numeric_limits<std::uint32_t>::max()));
    }
    return record;
}

// The dump (INTERFACE.md 2.1): line 1 the header, line 2 the field names, then one record per line; fields are
// tab-separated in the record's order and the item's or enchantment's name comes last. Numbers are written exactly
// (floats shortest round-trip, no locale), bools 0/1, shapes and exclusions as their words, arrays as comma lists
// without their trailing defaults ("-" when empty), effects as ';'-separated comma lists.
template <class R, class Visit>
void ForEachField(R& record, Visit&& visit)
{
    visit("source", record.source);
    visit("itemEntry", record.itemEntry);
    visit("enchantId", record.enchantId);
    visit("spellIndex", record.spellIndex);
    visit("spellId", record.spellId);
    visit("itemClass", record.itemClass);
    visit("itemSubClass", record.itemSubClass);
    visit("inventoryType", record.inventoryType);
    visit("itemLevel", record.itemLevel);
    visit("conjured", record.conjured);
    visit("shape", record.shape);
    visit("life", record.life);
    visit("positive", record.positive);
    visit("targetItemClass", record.targetItemClass);
    visit("targetItemSubClassMask", record.targetItemSubClassMask);
    visit("targetItemInvTypeMask", record.targetItemInvTypeMask);
    visit("targetItemMinLevel", record.targetItemMinLevel);
    visit("targetItemMaxLevel", record.targetItemMaxLevel);
    visit("minRangeYards", record.minRangeYards);
    visit("rangeYards", record.rangeYards);
    visit("rangeFriendYards", record.rangeFriendYards);
    visit("radiusYards", record.radiusYards);
    visit("castMs", record.castMs);
    visit("durationMs", record.durationMs);
    visit("channeled", record.channeled);
    visit("moveInterrupts", record.moveInterrupts);
    visit("cooldownMs", record.cooldownMs);
    visit("category", record.category);
    visit("categoryCooldownMs", record.categoryCooldownMs);
    visit("potion", record.potion);
    visit("charges", record.charges);
    visit("consumed", record.consumed);
    visit("needs.level", record.needs.level);
    visit("needs.skill", record.needs.skill);
    visit("needs.skillRank", record.needs.skillRank);
    visit("needs.classMask", record.needs.classMask);
    visit("needs.raceMask", record.needs.raceMask);
    visit("needs.requiredSpell", record.needs.requiredSpell);
    visit("needs.areaGroup", record.needs.areaGroup);
    visit("needs.map", record.needs.map);
    visit("needs.area", record.needs.area);
    visit("needs.spellFocus", record.needs.spellFocus);
    visit("needs.fight", record.needs.fight);
    visit("needs.notInArena", record.needs.notInArena);
    visit("needs.outdoors", record.needs.outdoors);
    visit("needs.indoors", record.needs.indoors);
    visit("needs.notShapeshifted", record.needs.notShapeshifted);
    visit("effects", record.effects);
    visit("effectCount", record.effectCount);
    visit("effectsCut", record.effectsCut);
    visit("attributes", record.attributes);
    visit("schoolMask", record.schoolMask);
    visit("dispelType", record.dispelType);
    visit("spellSpecific", record.spellSpecific);
    visit("groups", record.groups);
    visit("groupCount", record.groupCount);
    visit("exclude", record.exclude);
    visit("replaceCopper", record.replaceCopper);
    visit("sellCopper", record.sellCopper);
    visit("price", record.price);
}

// The fields of one effect, in its order (effect aura amount misc miscB periodMs trigger createItem targetA targetB
// mechanic radiusYards triggered)
template <class E, class Visit>
void ForEachEffectField(E& effect, Visit&& visit)
{
    visit(effect.effect);
    visit(effect.aura);
    visit(effect.amount);
    visit(effect.misc);
    visit(effect.miscB);
    visit(effect.periodMs);
    visit(effect.trigger);
    visit(effect.createItem);
    visit(effect.targetA);
    visit(effect.targetB);
    visit(effect.mechanic);
    visit(effect.radiusYards);
    visit(effect.triggered);
}

inline std::vector<std::string_view> Split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true)
    {
        std::size_t const end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos)
            return parts;
        start = end + 1;
    }
}

namespace Dump
{
template <class T>
void WriteNumber(std::string& out, T value)
{
    std::array<char, 32> buffer{};
    std::to_chars_result const result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

template <class T>
bool ReadNumber(std::string_view text, T& value)
{
    if (text.empty())
        return false;
    if constexpr (std::is_floating_point_v<T>)
    {
        T parsed{};
        std::from_chars_result const result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size())
            return false;
        value = parsed;
        return true;
    }
    else
    {
        std::int64_t parsed = 0;
        std::from_chars_result const result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size())
            return false;
        if (parsed < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
            parsed > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
            return false;
        value = static_cast<T>(parsed);
        return true;
    }
}

template <class Enum, std::size_t N>
bool ReadWord(std::string_view text, std::array<std::string_view, N> const& words, Enum& value)
{
    for (std::size_t index = 0; index < N; ++index)
        if (words[index] == text)
        {
            value = static_cast<Enum>(index);
            return true;
        }
    return false;
}

template <class T>
void WriteValue(std::string& out, T const& value);

inline void WriteValue(std::string& out, EffectFact const& effect)
{
    bool first = true;
    ForEachEffectField(effect,
                       [&](auto const& field)
                       {
                           if (!first)
                               out += ',';
                           first = false;
                           WriteValue(out, field);
                       });
}

template <class T>
bool ReadValue(std::string_view text, T& value);

inline bool ReadValue(std::string_view text, EffectFact& effect)
{
    std::vector<std::string_view> const parts = Split(text, ',');
    std::size_t index = 0;
    bool ok = true;
    ForEachEffectField(effect,
                       [&](auto& field)
                       {
                           ok = ok && index < parts.size() && ReadValue(parts[index], field);
                           ++index;
                       });
    return ok && index == parts.size();
}

inline bool IsDefault(std::uint32_t value) { return value == 0; }

inline bool IsDefault(EffectFact const& effect)
{
    std::string text;
    WriteValue(text, effect);
    std::string empty;
    WriteValue(empty, EffectFact{});
    return text == empty;
}

template <class T, std::size_t N>
void WriteValue(std::string& out, std::array<T, N> const& values)
{
    char const separator = std::is_same_v<T, EffectFact> ? ';' : ',';
    std::size_t used = N;
    while (used > 0 && IsDefault(values[used - 1]))
        --used;
    if (!used)
    {
        out += '-';
        return;
    }
    for (std::size_t index = 0; index < used; ++index)
    {
        if (index)
            out += separator;
        WriteValue(out, values[index]);
    }
}

template <class T, std::size_t N>
bool ReadValue(std::string_view text, std::array<T, N>& values)
{
    values = {};
    if (text == "-")
        return true;
    char const separator = std::is_same_v<T, EffectFact> ? ';' : ',';
    std::vector<std::string_view> const parts = Split(text, separator);
    if (parts.size() > N)
        return false;
    for (std::size_t index = 0; index < parts.size(); ++index)
        if (!ReadValue(parts[index], values[index]))
            return false;
    return true;
}

template <class T>
void WriteValue(std::string& out, T const& value)
{
    if constexpr (std::is_same_v<T, bool>)
        out += value ? '1' : '0';
    else if constexpr (std::is_same_v<T, Shape>)
        out += ShapeWord(value);
    else if constexpr (std::is_same_v<T, Exclude>)
        out += ExcludeWord(value);
    else if constexpr (std::is_enum_v<T>)
        WriteNumber(out, static_cast<std::uint32_t>(value));
    else if constexpr (std::is_floating_point_v<T>)
        WriteNumber(out, value);
    else
        WriteNumber(out, static_cast<std::int64_t>(value));
}

template <class T>
bool ReadValue(std::string_view text, T& value)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        if (text != "0" && text != "1")
            return false;
        value = text == "1";
        return true;
    }
    else if constexpr (std::is_same_v<T, Shape>)
        return ReadWord(text, SHAPE_WORDS, value);
    else if constexpr (std::is_same_v<T, Exclude>)
        return ReadWord(text, EXCLUDE_WORDS, value);
    else if constexpr (std::is_enum_v<T>)
    {
        std::underlying_type_t<T> raw{};
        if (!ReadNumber(text, raw))
            return false;
        value = static_cast<T>(raw);
        return true;
    }
    else
        return ReadNumber(text, value);
}

inline std::string NameField(std::string_view name)
{
    std::string text(name);
    std::replace_if(text.begin(), text.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return text;
}
}  // namespace Dump

struct DumpRow
{
    Record record;
    std::string name;
};

inline std::string DumpHeader(std::string_view moduleRev, std::string_view coreRev, std::size_t records)
{
    return "# item-use-dump v1 module " + std::string(moduleRev) + " core " + std::string(coreRev) + " records " +
           std::to_string(records);
}

inline std::string DumpFieldNames()
{
    std::string line;
    Record record;
    ForEachField(record,
                 [&line](std::string_view name, auto const&)
                 {
                     line += name;
                     line += '\t';
                 });
    return line + "name";
}

inline std::string FormatDumpLine(Record const& record, std::string_view name)
{
    std::string line;
    ForEachField(record,
                 [&line](std::string_view, auto const& value)
                 {
                     Dump::WriteValue(line, value);
                     line += '\t';
                 });
    return line + Dump::NameField(name);
}

inline std::optional<DumpRow> ParseDumpLine(std::string_view line)
{
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    std::vector<std::string_view> const fields = Split(line, '\t');
    DumpRow row;
    std::size_t index = 0;
    bool ok = true;
    ForEachField(row.record,
                 [&](std::string_view, auto& value)
                 {
                     ok = ok && index + 1 < fields.size() && Dump::ReadValue(fields[index], value);
                     ++index;
                 });
    if (!ok || index + 1 != fields.size() || row.record.effectCount > MAX_EFFECTS || row.record.groupCount > MAX_GROUPS)
        return std::nullopt;
    row.name = std::string(fields.back());
    return row;
}

// Field by field, as the dump writes them
inline bool SameRecord(Record const& a, Record const& b) { return FormatDumpLine(a, "") == FormatDumpLine(b, ""); }
}  // namespace ItemUse

#endif
