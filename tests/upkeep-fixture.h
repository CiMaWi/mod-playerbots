/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The upkeep needs fixture shared by the standalone harnesses (upkeep-needs-selftest, planner-need-selftest): a
// synthetic upkeep index, bots on it and the rates the caller passes. Test-only: one anonymous namespace per
// harness file
#ifndef _PLAYERBOT_TESTS_UPKEEP_FIXTURE_H
#define _PLAYERBOT_TESTS_UPKEEP_FIXTURE_H

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "ItemWorthRules.h"
#include "UpkeepNeedRules.h"

using namespace Planner;
using namespace UpkeepNeeds;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& what)
{
    if (condition)
    {
        ++passed;
        std::cout << "  ok: " << what << "\n";
        return;
    }

    ++failed;
    std::cout << "  FAIL: " << what << "\n";
}

constexpr std::uint8_t CLASS_WARRIOR = 1;
constexpr std::uint8_t RACE_HUMAN = 1;
constexpr std::uint8_t RACE_ORC = 2;

constexpr std::uint32_t TRAINER_WARRIOR_STARTER = 99;  // a starter trainer: the first levels only
constexpr std::uint32_t TRAINER_WARRIOR = 100;
constexpr std::uint32_t TRAINER_MAGE = 110;
constexpr std::uint32_t TRAINER_HUNTER = 120;
constexpr std::uint32_t TRAINER_PALADIN = 130;
constexpr std::uint32_t TRAINER_DRUID = 140;
constexpr std::uint32_t TRAINER_WARLOCK = 150;
constexpr std::uint32_t TRAINER_PRIEST = 160;
constexpr std::uint32_t TRAINER_RIDING = 300;
constexpr std::uint32_t VENDOR_INN = 5000;
constexpr std::uint32_t VENDOR_ARMS = 5001;
constexpr std::uint32_t VENDOR_REAGENT = 5002;
constexpr std::uint32_t VENDOR_STABLE = 5003;
constexpr std::uint32_t VENDOR_MOUNTS = 5004;
constexpr std::uint32_t VENDOR_GENERAL = 5005;  // bread and arrows
constexpr std::uint32_t VENDOR_POISON = 5006;   // two poisons of one grade, the bigger lot dearer per unit

constexpr std::uint32_t ITEM_BREAD = 4540;
constexpr std::uint32_t ITEM_GOOD_BREAD = 4542;
constexpr std::uint32_t ITEM_WATER = 159;
constexpr std::uint32_t ITEM_ARROW = 2512;
constexpr std::uint32_t ITEM_RAZOR_ARROW = 3030;   // level 25, at the weapon vendor only
constexpr std::uint32_t ITEM_TERRORSHAFT = 41586;  // level 75, at the weapon vendor, worse than the level 72 arrows
constexpr std::uint32_t ITEM_SARONITE = 41165;     // Saronite Razorheads: level 72, the handout's, sold by no vendor
constexpr std::uint32_t ITEM_INSTANT_1 = 6947;     // level 20
constexpr std::uint32_t ITEM_INSTANT_2 = 6949;     // level 28, lot 1
constexpr std::uint32_t ITEM_DEADLY_1 = 2892;      // level 28, lot 5, dearer per unit, at the stable only
constexpr std::uint32_t ITEM_BULLET = 2516;        // cheaper than the arrows: a bow must still never get them
constexpr std::uint32_t ITEM_MEAT = 117;
constexpr std::uint32_t ITEM_POWDER = 17020;
constexpr std::uint32_t ITEM_HORSE = 2414;
constexpr std::uint32_t ITEM_FAST_HORSE = 18776;
constexpr std::uint32_t ITEM_GRYPHON = 25470;
constexpr std::uint32_t ITEM_FAST_GRYPHON = 25473;
constexpr std::uint32_t ITEM_BADGE_GRYPHON = 44690;  // gold and currency
constexpr std::uint32_t ITEM_WOLF = 1132;            // orcs only

constexpr std::uint32_t SPELL_APPRENTICE = 33388;
constexpr std::uint32_t SPELL_JOURNEYMAN = 33391;
constexpr std::uint32_t SPELL_EXPERT = 34090;
constexpr std::uint32_t SPELL_ARTISAN = 34091;
constexpr std::uint32_t SPELL_COLD_WEATHER = 54197;
constexpr std::uint32_t SPELL_WARHORSE = 13819;  // a quest spell, on no trainer list
constexpr std::uint32_t SPELL_CHARGER = 23214;
constexpr std::uint32_t SPELL_HOLY_SHIELD_1 = 20925;  // the talent
constexpr std::uint32_t SPELL_HOLY_SHIELD_2 = 20927;
constexpr std::uint32_t SPELL_FLIGHT_FORM = 33943;  // on no trainer list
constexpr std::uint32_t SPELL_SWIFT_FLIGHT_FORM = 40120;
constexpr std::uint32_t SPELL_FELSTEED = 1710;  // a learn spell
constexpr std::uint32_t SPELL_FELSTEED_TAUGHT = 5784;
constexpr std::uint32_t SPELL_DREADSTEED = 23161;
constexpr std::uint32_t SPELL_MAGE_POWDER = 2010;  // the mage's level 10 spell uses the powder

// Ammo damage x 1000 as the item usage compares it (world DB: Terrorshaft 46.5, Saronite Razorheads 67.5)
constexpr std::uint32_t DAMAGE_ARROW = 2500;
constexpr std::uint32_t DAMAGE_BULLET = 2500;
constexpr std::uint32_t DAMAGE_RAZOR = 7500;
constexpr std::uint32_t DAMAGE_TERRORSHAFT = 46500;
constexpr std::uint32_t DAMAGE_SARONITE = 67500;
constexpr std::uint8_t ARROWS = static_cast<std::uint8_t>(UpkeepItem::SUBCLASS_ARROW);

constexpr std::uint32_t GOLD = COPPER_PER_GOLD;
constexpr std::uint8_t MEAT_DIET = 1;  // FoodType 1

TrainerSpell Spell(std::uint32_t id, std::uint32_t cost, std::uint8_t level) { return {id, cost, 0, 0, {}, level, {}}; }

TrainerSpell RidingSpell(std::uint32_t id, std::uint32_t cost, std::uint8_t level, std::uint32_t skillRank)
{
    return {id, cost, skillRank ? SKILL_RIDING : 0u, skillRank, {}, level, {}};
}

UpkeepItemFacts Item(std::uint32_t id, std::uint32_t itemClass, std::uint32_t subClass, std::uint32_t price,
                     std::uint32_t count, std::uint32_t level)
{
    UpkeepItemFacts facts;
    facts.item = id;
    facts.itemClass = itemClass;
    facts.subClass = subClass;
    facts.allowableClass = 0xFFFFFFFF;
    facts.allowableRace = 0xFFFFFFFF;
    facts.buyPrice = price;
    facts.buyCount = count;
    facts.requiredLevel = level;
    return facts;
}

UpkeepItemFacts Mount(std::uint32_t id, std::uint32_t price, std::uint32_t skillRank, std::uint32_t race)
{
    UpkeepItemFacts mount = Item(id, UpkeepItem::CLASS_MISC, UpkeepItem::SUBCLASS_MOUNT, price, 1, 20);
    mount.requiredSkill = SKILL_RIDING;
    mount.requiredSkillRank = skillRank;
    mount.allowableRace = race ? 1u << (race - 1) : 0xFFFFFFFF;
    return mount;
}

// Warrior spells every other level with a costly rank at 20, a mage and a hunter list, the riding trainer at the
// game's prices and a gold+currency gryphon beside the gold ones
UpkeepIndex Fixture()
{
    using namespace UpkeepItem;
    UpkeepGather input;
    std::vector<TrainerSpell> warrior;
    for (std::uint8_t level = 2; level <= 40; level += 2)
        warrior.push_back(Spell(1000 + level, level == 20 ? 30000 : 100u * level, level));
    input.trainers.push_back(
        {TRAINER_WARRIOR_STARTER, TrainerKind::Class, CLASS_WARRIOR, {warrior.begin(), warrior.begin() + 3}});
    input.trainers.push_back({TRAINER_WARRIOR, TrainerKind::Class, CLASS_WARRIOR, warrior});
    std::vector<TrainerSpell> mage;
    for (std::uint8_t level = 4; level <= 20; level += 2)
        mage.push_back(Spell(2000 + level, 200u * level, level));
    TrainerSpell rank2 = Spell(2100, 900, 12);  // a second rank needs the first
    rank2.reqAbility[0] = 2010;
    TrainerSpell talentRank = Spell(2200, 900, 12);  // needs a talent off the list: not counted
    talentRank.reqAbility[0] = 99999;
    mage.push_back(rank2);
    mage.push_back(talentRank);
    input.trainers.push_back({TRAINER_MAGE, TrainerKind::Class, CLASS_MAGE, mage});
    input.trainers.push_back({TRAINER_HUNTER, TrainerKind::Class, CLASS_HUNTER, {Spell(3010, 500, 10)}});
    // trainer_spell rows of paladin trainer 3 and druid trainer 33: the charger needs Summon Warhorse (on no list)
    // and journeyman riding, Holy Shield rank 2 the talent, Swift Flight Form Flight Form (on no list) and riding 300
    TrainerSpell charger = Spell(SPELL_CHARGER, 3500, 40);
    charger.reqAbility[0] = SPELL_WARHORSE;
    charger.reqAbility[1] = SPELL_JOURNEYMAN;
    TrainerSpell holyShield = Spell(SPELL_HOLY_SHIELD_2, 1400, 50);
    holyShield.reqAbility[0] = SPELL_HOLY_SHIELD_1;
    input.trainers.push_back({TRAINER_PALADIN, TrainerKind::Class, CLASS_PALADIN, {holyShield, charger}});
    TrainerSpell swiftFlight = RidingSpell(SPELL_SWIFT_FLIGHT_FORM, 20 * GOLD, 71, 300);
    swiftFlight.reqAbility[0] = SPELL_FLIGHT_FORM;
    input.trainers.push_back({TRAINER_DRUID, TrainerKind::Class, CLASS_DRUID, {swiftFlight}});
    // warlock trainer 31: Felsteed 1710 teaches 5784, which Dreadsteed needs with journeyman riding
    TrainerSpell felsteed = Spell(SPELL_FELSTEED, 1000, 20);
    felsteed.teaches[0] = SPELL_FELSTEED_TAUGHT;
    TrainerSpell dreadsteed = Spell(SPELL_DREADSTEED, 5000, 40);
    dreadsteed.reqAbility[0] = SPELL_FELSTEED_TAUGHT;
    dreadsteed.reqAbility[1] = SPELL_JOURNEYMAN;
    input.trainers.push_back({TRAINER_WARLOCK, TrainerKind::Class, CLASS_WARLOCK, {felsteed, dreadsteed}});
    input.trainers.push_back(
        {TRAINER_RIDING,
         TrainerKind::Mount,
         0,
         {RidingSpell(SPELL_APPRENTICE, 4 * GOLD, 20, 0), RidingSpell(SPELL_JOURNEYMAN, 50 * GOLD, 40, 75),
          RidingSpell(SPELL_EXPERT, 250 * GOLD, 60, 150), RidingSpell(SPELL_ARTISAN, 5000 * GOLD, 70, 225),
          RidingSpell(SPELL_COLD_WEATHER, 1000 * GOLD, 77, 225)}});
    // priest trainer rows from acore_world: 25315 costs 6500 and needs 10929, which costs 42000
    TrainerSpell dearPrerequisite = Spell(10929, 42000, 52);
    TrainerSpell cheapRank = Spell(25315, 6500, 58);
    cheapRank.reqAbility[0] = 10929;
    input.trainers.push_back({TRAINER_PRIEST, TrainerKind::Class, CLASS_PRIEST, {dearPrerequisite, cheapRank}});
    input.trainerEntries = {{TRAINER_PRIEST, TRAINER_PRIEST},   {TRAINER_WARRIOR, TRAINER_WARRIOR},
                            {TRAINER_MAGE, TRAINER_MAGE},       {TRAINER_HUNTER, TRAINER_HUNTER},
                            {TRAINER_PALADIN, TRAINER_PALADIN}, {TRAINER_DRUID, TRAINER_DRUID},
                            {TRAINER_WARLOCK, TRAINER_WARLOCK}, {TRAINER_RIDING, TRAINER_RIDING}};
    input.trainerEntries.emplace_back(TRAINER_WARRIOR_STARTER, TRAINER_WARRIOR_STARTER);

    UpkeepItemFacts bread = Item(ITEM_BREAD, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 1);
    bread.spellCategory = CATEGORY_FOOD;
    UpkeepItemFacts goodBread = Item(ITEM_GOOD_BREAD, CLASS_CONSUMABLE, SUBCLASS_FOOD, 200, 1, 15);
    goodBread.spellCategory = CATEGORY_FOOD;
    UpkeepItemFacts water = Item(ITEM_WATER, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 1);
    water.spellCategory = CATEGORY_DRINK;
    UpkeepItemFacts meat = Item(ITEM_MEAT, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 1);
    meat.foodType = 1;
    UpkeepItemFacts arrow = Item(ITEM_ARROW, CLASS_PROJECTILE, SUBCLASS_ARROW, 10, 200, 1);
    arrow.ammoDamage = DAMAGE_ARROW;
    UpkeepItemFacts bullet = Item(ITEM_BULLET, CLASS_PROJECTILE, SUBCLASS_BULLET, 5, 200, 1);
    bullet.ammoDamage = DAMAGE_BULLET;
    UpkeepItemFacts razor = Item(ITEM_RAZOR_ARROW, CLASS_PROJECTILE, SUBCLASS_ARROW, 10, 200, 25);
    razor.ammoDamage = DAMAGE_RAZOR;
    UpkeepItemFacts terrorshaft = Item(ITEM_TERRORSHAFT, CLASS_PROJECTILE, SUBCLASS_ARROW, 1000, 200, 75);
    terrorshaft.ammoDamage = DAMAGE_TERRORSHAFT;
    std::vector<UpkeepItemFacts> poisons = {
        Item(ITEM_INSTANT_1, CLASS_CONSUMABLE, SUBCLASS_CONSUMABLE_OTHER, 50, 1, 20),
        Item(ITEM_INSTANT_2, CLASS_CONSUMABLE, SUBCLASS_CONSUMABLE_OTHER, 100, 1, 28),
        Item(ITEM_DEADLY_1, CLASS_CONSUMABLE, SUBCLASS_CONSUMABLE_OTHER, 600, 5, 28)};
    for (UpkeepItemFacts& poison : poisons)
        poison.allowableClass = CLASS_MASK_ROGUE;
    UpkeepItemFacts powder = Item(ITEM_POWDER, CLASS_MISC, 1, 1000, 1, 0);
    input.items = {bread,
                   goodBread,
                   water,
                   meat,
                   arrow,
                   bullet,
                   razor,
                   terrorshaft,
                   poisons[0],
                   poisons[1],
                   poisons[2],
                   powder,
                   Mount(ITEM_HORSE, 1 * GOLD, 75, RACE_HUMAN),
                   Mount(ITEM_WOLF, 1 * GOLD / 2, 75, RACE_ORC),
                   Mount(ITEM_FAST_HORSE, 10 * GOLD, 150, RACE_HUMAN),
                   Mount(ITEM_GRYPHON, 50 * GOLD, 225, 0),
                   Mount(ITEM_FAST_GRYPHON, 100 * GOLD, 300, 0),
                   Mount(ITEM_BADGE_GRYPHON, 1 * GOLD, 300, 0)};
    input.vendorItems = {
        {VENDOR_INN, ITEM_BREAD, 0, 0, 0, true},
        {VENDOR_INN, ITEM_GOOD_BREAD, 0, 0, 0, true},
        {VENDOR_INN, ITEM_WATER, 0, 0, 0, true},
        {VENDOR_STABLE, ITEM_MEAT, 0, 0, 0, true},
        {VENDOR_ARMS, ITEM_ARROW, 0, 0, 0, true},
        {VENDOR_ARMS, ITEM_BULLET, 0, 0, 0, true},
        {VENDOR_ARMS, ITEM_RAZOR_ARROW, 0, 0, 0, true},
        {VENDOR_ARMS, ITEM_TERRORSHAFT, 0, 0, 0, true},
        {VENDOR_POISON, ITEM_INSTANT_1, 0, 0, 0, true},
        {VENDOR_POISON, ITEM_INSTANT_2, 0, 0, 0, true},
        {VENDOR_STABLE, ITEM_DEADLY_1, 0, 0, 0, true},
        {VENDOR_GENERAL, ITEM_BREAD, 0, 0, 0, true},
        {VENDOR_GENERAL, ITEM_ARROW, 0, 0, 0, true},
        {VENDOR_REAGENT, ITEM_POWDER, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_HORSE, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_WOLF, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_FAST_HORSE, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_GRYPHON, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_FAST_GRYPHON, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_BADGE_GRYPHON, 0, 0, 2400, true},  // gold and badges
    };
    input.reagents = {{ITEM_POWDER, CLASS_MAGE, SPELL_MAGE_POWDER}};
    UpkeepIndex index = UpkeepIndex::Build(std::move(input));
    index.version = 7;
    return index;
}

// Weapon masters (WU15a, world DB trainers 47-57): tradeskill trainers whose spells teach weapon skill lines at 1000
// copper each, and a profession trainer of the same kind; maces fit warriors of every race and never mages, daggers
// both. The gather repeats a skill once per master that teaches it
constexpr std::uint32_t TRAINER_WEAPON_MASTER = 400;
constexpr std::uint32_t TRAINER_PROFESSION = 410;
constexpr std::uint16_t SKILL_MACES = 54;
constexpr std::uint16_t SKILL_DAGGERS = 173;
constexpr std::uint16_t SKILL_BLACKSMITHING = 164;
constexpr std::uint32_t SPELL_MACES = 198;
constexpr std::uint32_t SPELL_DAGGERS = 1180;
constexpr std::uint32_t SPELL_BLACKSMITHING = 2018;
constexpr std::uint32_t WEAPON_PRICE = 1000;
constexpr std::uint32_t ALL_RACES = (1u << (UPKEEP_RACE_SLOTS - 1)) - 1;

inline UpkeepIndex WeaponFixture()
{
    UpkeepGather input;
    input.trainers.push_back({TRAINER_WARRIOR, TrainerKind::Class, CLASS_WARRIOR, {Spell(1002, 200, 2)}});
    input.trainers.push_back({TRAINER_RIDING, TrainerKind::Mount, 0, {RidingSpell(SPELL_APPRENTICE, 4 * GOLD, 20, 0)}});
    input.trainers.push_back({TRAINER_WEAPON_MASTER,
                              TrainerKind::Tradeskill,
                              0,
                              {Spell(SPELL_MACES, WEAPON_PRICE, 0), Spell(SPELL_DAGGERS, WEAPON_PRICE, 0)}});
    input.trainers.push_back({TRAINER_PROFESSION, TrainerKind::Tradeskill, 0, {Spell(SPELL_BLACKSMITHING, 10, 5)}});
    input.trainerEntries = {{TRAINER_WARRIOR, TRAINER_WARRIOR},
                            {TRAINER_RIDING, TRAINER_RIDING},
                            {TRAINER_WEAPON_MASTER, TRAINER_WEAPON_MASTER},
                            {TRAINER_PROFESSION, TRAINER_PROFESSION}};
    WeaponSkillFacts maces{SKILL_MACES, SPELL_MACES, {}};
    maces.races[CLASS_WARRIOR] = ALL_RACES;
    WeaponSkillFacts daggers{SKILL_DAGGERS, SPELL_DAGGERS, {}};
    daggers.races[CLASS_WARRIOR] = ALL_RACES;
    daggers.races[CLASS_MAGE] = ALL_RACES;
    input.weaponSkills = {daggers, maces, maces};
    UpkeepIndex index = UpkeepIndex::Build(std::move(input));
    index.version = 7;
    return index;
}

// The bot wields or carries a weapon of the skill line (FillUpkeep's AddWeapon), knowing the skill or not
inline void Carries(Snapshot& bot, std::uint16_t skill, bool known = false)
{
    bot.ownedWeaponSkills.push_back({skill, known});
}

void Mark(std::vector<std::uint64_t>& bits, TrainerList const& list, std::uint32_t spellId)
{
    std::size_t const position = list.Find(spellId);
    if (position != TrainerList::NOT_FOUND)
        SetSpellBit(bits, position);
}

// A bot that knows its class spells up to trainedTo and the riding ranks given; every spell fits it
Snapshot Bot(UpkeepIndex const& index, std::uint8_t cls, std::uint8_t level, std::uint32_t money,
             std::uint8_t trainedTo, std::vector<std::uint32_t> const& riding = {})
{
    Snapshot bot;
    bot.cls = cls;
    bot.race = RACE_HUMAN;
    bot.level = level;
    bot.money = money;
    bot.upkeepVersion = index.version;
    if (TrainerList const* list = index.ClassList(cls))
    {
        bot.knownClassSpells = SpellBits(list->spells.size());
        bot.canLearnClassSpells = SpellBits(list->spells.size());
        for (std::size_t position = 0; position < list->spells.size(); ++position)
        {
            SetSpellBit(bot.canLearnClassSpells, position);
            if (list->spells[position].reqLevel <= trainedTo)
                SetSpellBit(bot.knownClassSpells, position);
        }
    }
    TrainerList const* ridingList = index.RidingList(bot.race);
    bot.knownRidingSpells = SpellBits(ridingList->spells.size());
    bot.canLearnRidingSpells = SpellBits(ridingList->spells.size());
    for (std::size_t position = 0; position < ridingList->spells.size(); ++position)
        SetSpellBit(bot.canLearnRidingSpells, position);
    std::uint16_t skill = 0;
    for (std::uint32_t const spell : riding)
    {
        Mark(bot.knownRidingSpells, *ridingList, spell);
        for (RidingRank const& rank : RIDING_RANKS)
            if (rank.spellId == spell)
            {
                skill = std::max(skill, rank.skill);
                // The mount of each rank it knows, as the factory gives it (a scenario takes it away)
                if (rank.kind == RideKind::Ground)
                    bot.groundMountSkill = std::max(bot.groundMountSkill, rank.skill);
                else if (rank.kind == RideKind::Air)
                    bot.flyingMountSkill = std::max(bot.flyingMountSkill, rank.skill);
            }
    }
    if (skill)
        bot.skills.push_back({SKILL_RIDING, skill, skill});
    // Hunters carry a bow with the level 1 arrows equipped; stock at its target unless a scenario says otherwise:
    // only the need under test shows
    if (cls == CLASS_HUNTER)
    {
        bot.ammoSubClass = ARROWS;
        bot.ammoDamage = DAMAGE_ARROW;
    }
    NeedRates const rates;
    for (UpkeepStock const kind : {UpkeepStock::Food, UpkeepStock::Water, UpkeepStock::Ammo, UpkeepStock::Reagent})
        if (std::uint32_t const target = StockTarget(bot, rates, kind))
            bot.stock.push_back({1000u + static_cast<std::uint32_t>(kind), target, StockBit(kind), 0,
                                 kind == UpkeepStock::Ammo ? bot.ammoSubClass : std::uint8_t(0), level,
                                 kind == UpkeepStock::Ammo ? bot.ammoDamage : 0u});
    return bot;
}

// The bot knows the abilities off the lists that this class spell needs (FillOffList's bit)
inline void KnowsOffList(Snapshot& bot, UpkeepIndex const& index, std::uint32_t spellId)
{
    TrainerList const* list = index.ClassList(bot.cls);
    bot.offListMetClassSpells.resize(SpellBits(list->spells.size()).size());
    Mark(bot.offListMetClassSpells, *list, spellId);
}

void SetStock(Snapshot& bot, UpkeepStock kind, std::uint32_t count, std::uint8_t diet = 0)
{
    for (StockItem& entry : bot.stock)
        if (entry.kinds & StockBit(kind))
            entry.count = 0;
    bool const ammo = kind == UpkeepStock::Ammo;
    bot.stock.push_back({2000u + static_cast<std::uint32_t>(kind), count, StockBit(kind), diet,
                         ammo ? bot.ammoSubClass : std::uint8_t(0), bot.level, ammo ? bot.ammoDamage : 0u});
}

TraitRules::BotTraits Traits(int thrift, int thoroughness, bool collector)
{
    TraitRules::BotTraits traits;
    traits.Values[TraitRules::Index(TraitRules::Trait::Thrift)] = static_cast<std::int8_t>(thrift);
    traits.Values[TraitRules::Index(TraitRules::Trait::Thoroughness)] = static_cast<std::int8_t>(thoroughness);
    if (collector)
        traits.Interests[0] = static_cast<std::uint8_t>(TraitRules::Interest::Collecting) + 1;
    return traits;
}

// WorldTravel's band default e, as the caller passes it
NeedRates Rates(std::uint8_t level, float walkMinutesPerHour = DEFAULT_WALK_MINUTES_PER_HOUR)
{
    NeedRates rates;
    rates.earnCopperPerMinute =
        static_cast<float>(WorldTravel::DEFAULT_SLOT_BANDS[WorldTravel::LevelBand(level)].EarnCopperPerMinute);
    rates.walkMinutesPerHour = walkMinutesPerHour;
    return rates;
}

// The rates of a bot whose slot measure took an hour of legs (WU14): walk minutes on the ground outside flying areas,
// sky minutes walked in Outland's open flying areas (walking and where a flyer flies) and north minutes walked under
// Northrend's open sky without cold weather flying; the rest of the hour without travel
inline NeedRates Measured(std::uint8_t level, float walk, float sky = 0.0f, float north = 0.0f)
{
    using WorldTravel::LegKind;
    using WorldTravel::LegShares;
    constexpr std::uint32_t START = 1000;
    constexpr std::uint32_t MAP_OUTLAND = 530;
    NeedRates rates = Rates(level);
    WorldTravel::SlotMeasure measure;
    std::uint32_t now = START;
    auto const leg = [&measure, &now](std::uint8_t shares, float minutes)
    {
        std::uint32_t const ms = static_cast<std::uint32_t>(minutes * 60000.0f);
        now += ms;
        WorldTravel::NoteTravelLeg(measure, shares, ms, now);
    };
    leg(LegShares(LegKind::Walk, 0, false, false), walk);
    leg(LegShares(LegKind::Walk, MAP_OUTLAND, true, false), sky);
    leg(LegShares(LegKind::Walk, WorldTravel::TRAVEL_MAP_NORTHREND, true, false), north);
    WorldTravel::NoteTravelLeg(measure, 0, 0, START + WorldTravel::MS_PER_HOUR);
    FillTravel(rates, measure);
    return rates;
}

inline NeedList Show(std::string const& who, Snapshot const& bot, UpkeepIndex const& index, NeedRates const& rates)
{
    NeedList const list = ComputeNeeds(bot, index, rates);
    std::cout << " " << who << " (level " << int(bot.level) << ", e " << rates.earnCopperPerMinute << " c/min, walk "
              << rates.walkMinutesPerHour << " min/h, flight " << rates.flightMinutesPerHour << ", northrend "
              << rates.northrendFlightMinutesPerHour << "):\n";
    for (std::string const& line : DescribeAll(list))
        std::cout << "    " << line << "\n";
    return list;
}
}  // namespace

#endif
