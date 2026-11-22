/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the item use facts (src/Ai/Item/ItemUseFacts.h, item-use plan WI2) over hand-built copies of the
// game's data: a case per target shape, consumed against charges, a shared cooldown category, a potion, the flask in
// both elixir groups and the other exclusion kinds, the dump line's round trip, the control cohort's hash and the
// types header's word tables. Build (Git Bash, module root):
// clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/Item tests/item-use-index-selftest.cpp

#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

#include "ItemUseFacts.h"

using namespace ItemUse;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& what)
{
    if (condition)
    {
        ++passed;
        std::cout << "ok: " << what << "\n";
        return;
    }

    ++failed;
    std::cout << "FAIL: " << what << "\n";
}

constexpr std::uint32_t BUY_PER_SELL = 4;  // WorldTravel::BUY_PER_SELL_PRICE, passed in as the server does

// The game's ids the hand cases use (SharedDefines.h, SpellInfo.h)
constexpr std::uint16_t EFFECT_HEAL = 10;
constexpr std::uint16_t EFFECT_SUMMON = 28;
constexpr std::uint16_t EFFECT_APPLY_AURA = 6;
constexpr std::uint16_t EFFECT_OPEN_LOCK = 33;
constexpr std::uint16_t EFFECT_TRIGGER_SPELL = 64;
constexpr std::uint16_t EFFECT_SCHOOL_DAMAGE = 2;
constexpr std::uint16_t AURA_MOD_STAT = 29;
constexpr std::uint8_t TARGET_UNIT_CASTER = 1;
constexpr std::uint8_t TARGET_DEST_CASTER = 18;
constexpr std::uint8_t TARGET_SRC_CASTER = 22;
constexpr std::uint8_t TARGET_UNIT_SRC_AREA_ALLY = 30;
constexpr std::uint8_t TARGET_UNIT_TARGET_ENEMY = 6;
constexpr std::uint8_t TARGET_UNIT_DEST_AREA_ENEMY = 16;
constexpr std::uint8_t TARGET_DEST_TARGET_ENEMY = 53;
constexpr std::uint8_t TARGET_UNIT_TARGET_ALLY = 21;
constexpr std::uint8_t TARGET_UNIT_TARGET_ANY = 25;
constexpr std::uint8_t TARGET_DEST_DEST = 87;
constexpr std::uint8_t TARGET_GAMEOBJECT_TARGET = 23;
constexpr std::uint8_t TARGET_GAMEOBJECT_ITEM_TARGET = 26;
constexpr std::uint8_t ITEM_CLASS_WEAPON = 2;

SpellEffectData Effect(std::uint16_t effect, std::uint8_t targetA, std::uint8_t objectA, std::uint8_t targetB = 0,
                       std::uint8_t objectB = Game::OBJECT_NONE)
{
    SpellEffectData data;
    data.effect = effect;
    data.targetA = targetA;
    data.objectA = objectA;
    data.targetB = targetB;
    data.objectB = objectB;
    return data;
}

SpellData Spell(std::uint32_t id, std::uint32_t mask, SpellEffectData const& effect)
{
    SpellData spell;
    spell.id = id;
    spell.explicitMask = mask;
    spell.effects.push_back(effect);
    return spell;
}

ItemData Item(std::uint32_t entry, std::int32_t charges)
{
    ItemData item;
    item.itemEntry = entry;
    item.charges = charges;
    return item;
}

// One spell per target shape, as the engine's explicit target mask and implicit targets give it
void CheckShapes()
{
    SpellData const summon =
        Spell(1, 0, Effect(EFFECT_SUMMON, TARGET_DEST_CASTER, Game::OBJECT_DEST));  // a pet: nothing aimed
    Check(ShapeOf(summon) == Shape::None, "shape none: a summon at the caster's feet");

    SpellData const buff = Spell(2, 0, Effect(EFFECT_APPLY_AURA, TARGET_UNIT_CASTER, Game::OBJECT_UNIT));
    Check(ShapeOf(buff) == Shape::Self, "shape self: an aura on the caster");

    SpellEffectData implicitHeal = Effect(EFFECT_HEAL, 0, Game::OBJECT_NONE);
    implicitHeal.usedObject = Game::OBJECT_UNIT;  // no target entries: the engine heals the caster
    Check(ShapeOf(Spell(3, 0, implicitHeal)) == Shape::Self, "shape self: an effect without target entries");

    SpellEffectData groupBuff =
        Effect(EFFECT_APPLY_AURA, TARGET_SRC_CASTER, Game::OBJECT_SRC, TARGET_UNIT_SRC_AREA_ALLY, Game::OBJECT_UNIT);
    groupBuff.radiusYards = 30.0f;
    SpellData const aroundCaster = Spell(4, 0, groupBuff);
    Record const aroundRecord = MakeRecord(Item(40, 0), aroundCaster, BUY_PER_SELL);
    Check(aroundRecord.shape == Shape::Self && aroundRecord.radiusYards == 30.0f,
          "shape self: an area around the caster keeps self and says so in its radius");

    SpellData const bandage =
        Spell(5, Game::MASK_UNIT_ALLY, Effect(EFFECT_APPLY_AURA, TARGET_UNIT_TARGET_ALLY, Game::OBJECT_UNIT));
    Check(ShapeOf(bandage) == Shape::Friend, "shape friend: an ally unit");

    SpellData const net =
        Spell(6, Game::MASK_UNIT_ENEMY, Effect(EFFECT_APPLY_AURA, TARGET_UNIT_TARGET_ENEMY, Game::OBJECT_UNIT));
    Check(ShapeOf(net) == Shape::Enemy, "shape enemy: an enemy unit");

    SpellEffectData aroundEnemy = Effect(EFFECT_SCHOOL_DAMAGE, TARGET_DEST_TARGET_ENEMY, Game::OBJECT_DEST,
                                         TARGET_UNIT_DEST_AREA_ENEMY, Game::OBJECT_UNIT);
    aroundEnemy.radiusYards = 8.0f;
    Record const aroundTarget = MakeRecord(Item(70, -1), Spell(7, Game::MASK_UNIT_ENEMY, aroundEnemy), BUY_PER_SELL);
    Check(aroundTarget.shape == Shape::Enemy && aroundTarget.radiusYards == 8.0f,
          "shape enemy: an area around the enemy keeps enemy and its radius");

    SpellData const anyUnit =
        Spell(8, Game::MASK_UNIT, Effect(EFFECT_APPLY_AURA, TARGET_UNIT_TARGET_ANY, Game::OBJECT_UNIT));
    Check(ShapeOf(anyUnit) == Shape::AnyUnit, "shape unit: any unit");
    SpellData const both = Spell(9, Game::MASK_UNIT_ALLY | Game::MASK_UNIT_ENEMY,
                                 Effect(EFFECT_APPLY_AURA, TARGET_UNIT_TARGET_ANY, Game::OBJECT_UNIT));
    Check(ShapeOf(both) == Shape::AnyUnit, "shape unit: an ally or an enemy");

    SpellData const bomb =
        Spell(10, Game::MASK_DEST_LOCATION, Effect(EFFECT_SCHOOL_DAMAGE, TARGET_DEST_DEST, Game::OBJECT_DEST));
    Check(ShapeOf(bomb) == Shape::Ground, "shape ground: a thrown bomb");

    SpellData const stone = Spell(11, Game::MASK_ITEM, Effect(Game::EFFECT_ENCHANT_ITEM_TEMPORARY, 0, 0));
    Check(ShapeOf(stone) == Shape::OwnItem, "shape item: a sharpening stone");

    SpellData const chestKey =
        Spell(12, Game::MASK_GAMEOBJECT, Effect(EFFECT_OPEN_LOCK, TARGET_GAMEOBJECT_TARGET, Game::OBJECT_GOBJ));
    Check(ShapeOf(chestKey) == Shape::Object, "shape object: a game object");
    SpellData const key = Spell(13, Game::MASK_GAMEOBJECT_ITEM,
                                Effect(EFFECT_OPEN_LOCK, TARGET_GAMEOBJECT_ITEM_TARGET, Game::OBJECT_GOBJ_ITEM));
    Check(ShapeOf(key) == Shape::Object, "shape object: a lock on an object or an item");

    SpellData cables = Spell(14, Game::MASK_CORPSE_ALLY | Game::MASK_UNIT_DEAD,
                             Effect(Game::EFFECT_RESURRECT, TARGET_UNIT_TARGET_ALLY, Game::OBJECT_UNIT));
    cables.allowsDead = true;
    Check(ShapeOf(cables) == Shape::Friend && LifeOf(cables) == Life::Dead,
          "shape friend, life dead: a resurrection of an ally's corpse");
    SpellData either = bandage;
    either.allowsDead = true;
    Check(LifeOf(bandage) == Life::Alive && LifeOf(either) == Life::Either, "life: alive, or either by the flag");

    std::set<Shape> seen;
    for (SpellData const& spell : {summon, buff, bandage, net, anyUnit, bomb, stone, chestKey})
        seen.insert(ShapeOf(spell));
    Check(seen.size() == static_cast<std::size_t>(Shape::Count), "every shape has a hand case");
}

void CheckConsumption()
{
    SpellData const heal = Spell(20, 0, Effect(EFFECT_HEAL, TARGET_UNIT_CASTER, Game::OBJECT_UNIT));

    ItemData potion = Item(118, -1);
    potion.itemClass = Game::CLASS_CONSUMABLE;
    potion.itemSubClass = Game::SUBCLASS_POTION;
    potion.stackable = 5;
    potion.vendorSold = true;
    potion.buyPrice = 100;
    potion.buyCount = 1;
    potion.sellPrice = 25;
    Record const potionRecord = MakeRecord(potion, heal, BUY_PER_SELL);
    Check(potionRecord.consumed && potionRecord.charges == -1 && potionRecord.potion, "potion: consumed, a potion");
    Check(
        potionRecord.price == PriceSource::Vendor && potionRecord.replaceCopper == 100 && potionRecord.sellCopper == 25,
        "potion: the vendor's price");

    ItemData stack = potion;
    stack.buyPrice = 500;
    stack.buyCount = 5;
    Check(MakeRecord(stack, heal, BUY_PER_SELL).replaceCopper == 100, "a vendor's stack price is per item");

    ItemData wand = Item(5000, 3);  // three charges, the item stays when they are used up
    wand.stackable = 1;
    wand.sellPrice = 300;
    Record const wandRecord = MakeRecord(wand, heal, BUY_PER_SELL);
    Check(wandRecord.consumed && wandRecord.charges == 3, "charges above zero: the item stays, a use is consumed");
    Check(wandRecord.price == PriceSource::Derived && wandRecord.replaceCopper == 400,
          "derived price: sell price times the factor, divided by three uses");

    ItemData stone = Item(5001, -5);
    stone.stackable = 1;
    stone.sellPrice = 50;
    Check(MakeRecord(stone, heal, BUY_PER_SELL).replaceCopper == 40, "charges below zero: five uses per item");

    ItemData trinket = Item(5002, 0);
    trinket.sellPrice = 9000;
    trinket.inventoryType = 12;
    Record const trinketRecord = MakeRecord(trinket, heal, BUY_PER_SELL);
    Check(!trinketRecord.consumed && trinketRecord.charges == 0 && trinketRecord.replaceCopper == 0 &&
              trinketRecord.price == PriceSource::None && !trinketRecord.potion,
          "no charges: free, it costs only its cooldown");

    ItemData quest = Item(5003, -1);
    Record const questRecord = MakeRecord(quest, heal, BUY_PER_SELL);
    Check(questRecord.consumed && questRecord.price == PriceSource::None && questRecord.replaceCopper == 0,
          "consumed without a price: price none");

    ItemData tinker;
    tinker.source = Source::EnchantSpell;
    tinker.enchantId = 3606;
    tinker.spellIndex = 0;
    Record const tinkerRecord = MakeRecord(tinker, heal, BUY_PER_SELL);
    Check(tinkerRecord.source == Source::EnchantSpell && tinkerRecord.itemEntry == 0 &&
              tinkerRecord.enchantId == 3606 && !tinkerRecord.consumed && tinkerRecord.price == PriceSource::None,
          "an enchantment's use spell: free, no item, no price");

    SpellData event = heal;
    event.cooldownOnEvent = true;
    ItemData stoneOfHealth = Item(5512, -1);
    Check(MakeRecord(stoneOfHealth, event, BUY_PER_SELL).potion, "potion: by the cooldown that starts on an event");
}

void CheckCooldowns()
{
    SpellData spell = Spell(30, 0, Effect(EFFECT_HEAL, TARGET_UNIT_CASTER, Game::OBJECT_UNIT));
    spell.category = 30;
    spell.recoveryMs = 60000;
    spell.categoryRecoveryMs = 0;

    ItemData first = Item(118, -1);
    first.cooldownMs = -1;
    first.category = 4;
    first.categoryCooldownMs = 120000;
    ItemData second = Item(858, -1);
    second.cooldownMs = -1;
    second.category = 4;
    second.categoryCooldownMs = 120000;
    Record const a = MakeRecord(first, spell, BUY_PER_SELL);
    Record const b = MakeRecord(second, spell, BUY_PER_SELL);
    Check(a.category == 4 && b.category == a.category && a.categoryCooldownMs == 120000 && a.cooldownMs == 0,
          "two items share the item rows' cooldown category");

    ItemData plain = Item(119, -1);  // both item values -1: the spell's own
    Record const own = MakeRecord(plain, spell, BUY_PER_SELL);
    Check(own.category == 30 && own.cooldownMs == 60000 && own.categoryCooldownMs == 0,
          "no item cooldown: the spell's category and recovery");
}

void CheckExclusions()
{
    SpellData elixir = Spell(40, 0, Effect(EFFECT_APPLY_AURA, TARGET_UNIT_CASTER, Game::OBJECT_UNIT));
    SpellData flask = elixir;
    flask.groups = {Game::GROUP_BATTLE_ELIXIR, Game::GROUP_GUARDIAN_ELIXIR};
    Check(ExcludeOf(flask, ShapeOf(flask)) == Exclude::Flask, "the flask sits in both elixir groups: flask");
    SpellData battle = elixir;
    battle.groups = {Game::GROUP_BATTLE_ELIXIR};
    Check(ExcludeOf(battle, ShapeOf(battle)) == Exclude::BattleElixir, "battle elixir: group 1 only");
    SpellData guardian = elixir;
    guardian.groups = {Game::GROUP_GUARDIAN_ELIXIR};
    Check(ExcludeOf(guardian, ShapeOf(guardian)) == Exclude::GuardianElixir, "guardian elixir: group 2 only");

    SpellData food = elixir;
    food.specific = Game::SPECIFIC_FOOD;
    Check(ExcludeOf(food, ShapeOf(food)) == Exclude::Food, "food by its spell specific");
    SpellData wellFed = food;
    wellFed.groups = {Game::GROUP_WELL_FED};
    Check(ExcludeOf(wellFed, ShapeOf(wellFed)) == Exclude::WellFed, "a food that feeds well: well fed before food");
    SpellData drink = elixir;
    drink.specific = Game::SPECIFIC_DRINK;
    Check(ExcludeOf(drink, ShapeOf(drink)) == Exclude::Drink, "drink by its spell specific");
    SpellData both = elixir;
    both.specific = Game::SPECIFIC_FOOD_AND_DRINK;
    Check(ExcludeOf(both, ShapeOf(both)) == Exclude::Food, "food and drink counts as food");
    SpellData scroll = elixir;
    scroll.specific = Game::SPECIFIC_SCROLL;
    Check(ExcludeOf(scroll, ShapeOf(scroll)) == Exclude::Scroll, "scroll by its spell specific");

    SpellData coating = Spell(41, Game::MASK_ITEM, Effect(Game::EFFECT_ENCHANT_ITEM_TEMPORARY, 0, 0));
    coating.equippedItemClass = ITEM_CLASS_WEAPON;
    coating.equippedItemSubClassMask = 0x5;
    coating.baseLevel = 10;
    Record const coatingRecord = MakeRecord(Item(2862, -1), coating, BUY_PER_SELL);
    Check(coatingRecord.exclude == Exclude::Coating && coatingRecord.targetItemClass == ITEM_CLASS_WEAPON &&
              coatingRecord.targetItemSubClassMask == 0x5 && coatingRecord.targetItemMinLevel == 10,
          "coating: a temporary enchantment on an own weapon");
    SpellData poison = coating;
    poison.allowLowLevelBuff = true;
    Check(MakeRecord(Item(6947, -1), poison, BUY_PER_SELL).targetItemMinLevel == 0,
          "poison: the low level buff attribute sets no level floor on the weapon");
    Check(ExcludeOf(elixir, ShapeOf(elixir)) == Exclude::None, "a plain aura excludes nothing");

    SpellData many = elixir;
    many.groups = {1600, Game::GROUP_WELL_FED, Game::GROUP_GUARDIAN_ELIXIR, 1500, Game::GROUP_BATTLE_ELIXIR};
    Record const manyRecord = MakeRecord(Item(1, 0), many, BUY_PER_SELL);
    Check(manyRecord.groupCount == MAX_GROUPS && manyRecord.groups[0] == 1 && manyRecord.groups[1] == 2 &&
              manyRecord.groups[2] == 1001 && manyRecord.groups[3] == 1500 && manyRecord.exclude == Exclude::Flask,
          "groups are kept ascending, at most MAX_GROUPS");
}

void CheckEffectsAndNeeds()
{
    SpellEffectData stat = Effect(EFFECT_APPLY_AURA, TARGET_UNIT_CASTER, Game::OBJECT_UNIT);
    stat.aura = AURA_MOD_STAT;
    stat.basePoints = 9;
    stat.dieSides = 11;  // 10 to 20
    stat.misc = 3;
    SpellEffectData heal = Effect(EFFECT_HEAL, TARGET_UNIT_CASTER, Game::OBJECT_UNIT);
    heal.basePoints = 99;
    heal.dieSides = 1;
    SpellEffectData trigger = Effect(EFFECT_TRIGGER_SPELL, TARGET_UNIT_CASTER, Game::OBJECT_UNIT);
    trigger.triggerSpell = 777;

    SpellData spell = Spell(50, 0, stat);
    spell.effects.push_back(heal);
    spell.effects.push_back(trigger);
    spell.triggered = {heal, heal, heal};
    Record const six = MakeRecord(Item(1, 0), spell, BUY_PER_SELL);
    Check(six.effectCount == 6 && !six.effectsCut && six.effects[0].amount == 15 && six.effects[1].amount == 100 &&
              six.effects[0].misc == 3 && six.effects[2].trigger == 777 && !six.effects[2].triggered &&
              six.effects[3].triggered,
          "effects: three own, three triggered, the average amount");
    spell.triggered.push_back(heal);
    Record const cut = MakeRecord(Item(1, 0), spell, BUY_PER_SELL);
    Check(cut.effectCount == MAX_EFFECTS && cut.effectsCut, "a seventh effect is cut and marked");

    SpellData timed = Spell(51, 0, heal);
    timed.castMs = 3000;
    timed.castMoveInterrupt = true;
    timed.attributes[0] = Game::ATTR0_ONLY_OUTDOORS | Game::ATTR0_NOT_IN_COMBAT;
    timed.attributes[4] = Game::ATTR4_NOT_IN_ARENA;
    timed.positive = true;
    timed.minRangeFriend = 1.0f;
    timed.minRangeHostile = 5.0f;
    timed.maxRangeFriend = 40.0f;
    timed.maxRangeHostile = 30.0f;
    ItemData item = Item(1, 0);
    item.classMask = Game::CLASS_MASK_PLAYABLE;
    item.raceMask = 0xFFFFFFFF;
    item.requiredLevel = 20;
    Record const timedRecord = MakeRecord(item, timed, BUY_PER_SELL);
    Check(timedRecord.moveInterrupts && timedRecord.castMs == 3000 && timedRecord.needs.outdoors &&
              timedRecord.needs.fight == FightNeed::Peace && timedRecord.needs.notInArena &&
              timedRecord.needs.classMask == 0 && timedRecord.needs.raceMask == 0 && timedRecord.needs.level == 20,
          "a cast that moving breaks; needs: outdoors, peace, no arena; masks of everyone are 0");
    Check(timedRecord.minRangeYards == 1.0f && timedRecord.rangeYards == 30.0f && timedRecord.rangeFriendYards == 40.0f,
          "ranges: the positive spell's minimum, both maxima");

    SpellData instant = timed;
    instant.castMs = 0;
    Check(!MakeRecord(item, instant, BUY_PER_SELL).moveInterrupts, "an instant cast is not broken by moving");
    SpellData channel = instant;
    channel.channeled = true;
    channel.channelMoveInterrupt = true;
    Check(MakeRecord(item, channel, BUY_PER_SELL).moveInterrupts, "a channel that moving breaks");
    ItemData arena = Item(1, 0);
    arena.notInArena = true;
    Check(MakeRecord(arena, Spell(52, 0, heal), BUY_PER_SELL).needs.notInArena, "the item's arena rule");
}

bool SameRecordText(Record const& a, Record const& b)
{
    return SameRecord(a, b) && FormatDumpLine(a, "x") == FormatDumpLine(b, "x");
}

void CheckDump()
{
    std::vector<Record> records;
    SpellEffectData aura = Effect(EFFECT_APPLY_AURA, TARGET_UNIT_CASTER, Game::OBJECT_UNIT);
    aura.basePoints = -21;
    aura.dieSides = 1;
    aura.radiusYards = 0.1f;
    aura.amplitude = 3000;
    SpellData spell = Spell(60, Game::MASK_UNIT_ALLY, aura);
    spell.groups = {Game::GROUP_BATTLE_ELIXIR, Game::GROUP_GUARDIAN_ELIXIR, 1700};
    spell.attributes = {1, 2, 0xFFFFFFFF, 4, 5, 6, 7, 0};
    spell.minRangeFriend = 1e-7f;
    spell.maxRangeFriend = 30.5f;
    spell.maxRangeHostile = 41.25f;
    spell.durationMs = Game::DURATION_ENDLESS;
    spell.triggered = {aura};
    ItemData item = Item(4294967295u, -3);
    item.stackable = 1;
    item.sellPrice = 7;
    item.itemLevel = 65535;
    records.push_back(MakeRecord(item, spell, BUY_PER_SELL));
    records.push_back(Record{});
    ItemData enchant;
    enchant.source = Source::EnchantSpell;
    enchant.enchantId = 3601;
    enchant.spellIndex = 2;
    records.push_back(MakeRecord(enchant, Spell(61, Game::MASK_DEST_LOCATION, aura), BUY_PER_SELL));

    bool roundTrip = true;
    for (Record const& record : records)
    {
        std::optional<DumpRow> const row = ParseDumpLine(FormatDumpLine(record, "Elixir of Tests"));
        roundTrip = roundTrip && row && SameRecordText(row->record, record) && row->name == "Elixir of Tests";
    }
    Check(roundTrip, "dump lines round trip every hand record exactly");

    std::optional<DumpRow> const first = ParseDumpLine(FormatDumpLine(records[0], "a\tname"));
    Check(first && first->name == "a name" && first->record.itemEntry == 4294967295u &&
              first->record.effects[0].amount == -20 && first->record.minRangeYards == 1e-7f &&
              first->record.durationMs == Game::DURATION_ENDLESS && first->record.groupCount == 3 &&
              first->record.attributes[2] == 0xFFFFFFFF,
          "dump: a tab in the name becomes a space; extremes survive");

    std::string const line = FormatDumpLine(records[0], "x");
    std::string const names = DumpFieldNames();
    Check(Split(line, '\t').size() == Split(names, '\t').size() && names.rfind("source\t", 0) == 0 &&
              names.size() > 5 && names.substr(names.size() - 5) == "\tname",
          "dump: the field name line has a name per field");
    Check(DumpHeader("abc", "def", 12) == "# item-use-dump v1 module abc core def records 12", "dump: the header line");

    std::string wrongWord = line;
    std::size_t const shapeAt = wrongWord.find("\tfriend\t");
    if (shapeAt != std::string::npos)
        wrongWord.replace(shapeAt, 8, "\tfriendly\t");
    std::string tooBig = line;
    tooBig.replace(tooBig.find('\t', tooBig.find('\t', tooBig.find('\t') + 1) + 1) + 1, 1, "300");  // spellIndex
    Check(shapeAt != std::string::npos && !ParseDumpLine(wrongWord), "dump: an unknown word is refused");
    Check(!ParseDumpLine(tooBig), "dump: a value out of its type's range is refused");
    Check(!ParseDumpLine("") && !ParseDumpLine(DumpHeader("a", "b", 1)) && !ParseDumpLine(names) &&
              !ParseDumpLine(line.substr(0, line.size() - 2)) && !ParseDumpLine(line + "\textra") &&
              !ParseDumpLine("x" + line),
          "dump: malformed lines give nullopt");
    Check(ParseDumpLine(line + "\r").has_value(), "dump: a CR before the line end is ignored");
}

void CheckWords()
{
    std::set<std::string_view> causes;
    for (std::size_t cause = 0; cause < CAUSE_COUNT; ++cause)
        causes.insert(CauseWord(static_cast<Cause>(cause)));
    Check(causes.size() == CAUSE_COUNT && !causes.count("") && CauseWord(Cause::Handler) == "handler" &&
              CauseWord(Cause::BadTarget) == "target",
          "a distinct word per cause");
    std::set<std::string_view> shapes(SHAPE_WORDS.begin(), SHAPE_WORDS.end());
    std::set<std::string_view> excludes(EXCLUDE_WORDS.begin(), EXCLUDE_WORDS.end());
    Check(shapes.size() == static_cast<std::size_t>(Shape::Count) &&
              excludes.size() == static_cast<std::size_t>(Exclude::Count) &&
              CALLER_WORDS.size() == static_cast<std::size_t>(Caller::Count) &&
              INTERRUPT_WORDS.size() == static_cast<std::size_t>(Interrupt::Count),
          "word tables: one distinct word per value");
}
}  // namespace

int main()
{
    CheckShapes();
    CheckConsumption();
    CheckCooldowns();
    CheckExclusions();
    CheckEffectsAndNeeds();
    CheckDump();
    CheckWords();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
