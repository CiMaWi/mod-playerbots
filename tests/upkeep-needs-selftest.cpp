/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone harness of the upkeep needs rules (src/Ai/World/Rpg/UpkeepNeedRules.h): bots on a synthetic upkeep
// index, each scenario printing its decisions - repair before food when gold is short, ammo for hunters and no water
// for mages, training due by levels behind and thoroughness, riding by worth at 20 and 40 and later for a bot that
// rarely travels, artisan riding for a rich level-80 collector only, the reserve growing towards an expensive level
// and the shortfall, the unknown pet diet, and gold+currency mounts never priced. Class mounts and Swift Flight Form
// come with training once the riding rank is known; a collector's want never moves the reserve; a frugal bot never
// buys a rank and reports a shortfall in the same list.
#include <cstdint>
#include <iostream>
#include <string>
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

constexpr std::uint32_t TRAINER_WARRIOR = 100;
constexpr std::uint32_t TRAINER_MAGE = 110;
constexpr std::uint32_t TRAINER_HUNTER = 120;
constexpr std::uint32_t TRAINER_PALADIN = 130;
constexpr std::uint32_t TRAINER_DRUID = 140;
constexpr std::uint32_t TRAINER_WARLOCK = 150;
constexpr std::uint32_t TRAINER_RIDING = 300;
constexpr std::uint32_t VENDOR_INN = 5000;
constexpr std::uint32_t VENDOR_ARMS = 5001;
constexpr std::uint32_t VENDOR_REAGENT = 5002;
constexpr std::uint32_t VENDOR_STABLE = 5003;
constexpr std::uint32_t VENDOR_MOUNTS = 5004;

constexpr std::uint32_t ITEM_BREAD = 4540;
constexpr std::uint32_t ITEM_GOOD_BREAD = 4542;
constexpr std::uint32_t ITEM_WATER = 159;
constexpr std::uint32_t ITEM_ARROW = 2512;
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
    input.trainerEntries = {{TRAINER_WARRIOR, TRAINER_WARRIOR}, {TRAINER_MAGE, TRAINER_MAGE},
                            {TRAINER_HUNTER, TRAINER_HUNTER},   {TRAINER_PALADIN, TRAINER_PALADIN},
                            {TRAINER_DRUID, TRAINER_DRUID},     {TRAINER_WARLOCK, TRAINER_WARLOCK},
                            {TRAINER_RIDING, TRAINER_RIDING}};

    UpkeepItemFacts bread = Item(ITEM_BREAD, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 1);
    bread.spellCategory = CATEGORY_FOOD;
    UpkeepItemFacts goodBread = Item(ITEM_GOOD_BREAD, CLASS_CONSUMABLE, SUBCLASS_FOOD, 200, 1, 15);
    goodBread.spellCategory = CATEGORY_FOOD;
    UpkeepItemFacts water = Item(ITEM_WATER, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 1);
    water.spellCategory = CATEGORY_DRINK;
    UpkeepItemFacts meat = Item(ITEM_MEAT, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 1);
    meat.foodType = 1;
    UpkeepItemFacts arrow = Item(ITEM_ARROW, CLASS_PROJECTILE, SUBCLASS_ARROW, 10, 200, 1);
    UpkeepItemFacts powder = Item(ITEM_POWDER, CLASS_MISC, 1, 1000, 1, 0);
    input.items = {bread,
                   goodBread,
                   water,
                   meat,
                   arrow,
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
        {VENDOR_REAGENT, ITEM_POWDER, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_HORSE, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_WOLF, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_FAST_HORSE, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_GRYPHON, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_FAST_GRYPHON, 0, 0, 0, true},
        {VENDOR_MOUNTS, ITEM_BADGE_GRYPHON, 0, 0, 2400, true},  // gold and badges
    };
    input.reagents = {{ITEM_POWDER, CLASS_MAGE}};
    UpkeepIndex index = UpkeepIndex::Build(std::move(input));
    index.version = 7;
    return index;
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
                skill = std::max(skill, rank.skill);
    }
    if (skill)
        bot.skills.push_back({SKILL_RIDING, skill, skill});
    // Stock at its target unless a scenario says otherwise: only the need under test shows
    NeedRates const rates;
    for (UpkeepStock const kind : {UpkeepStock::Food, UpkeepStock::Water, UpkeepStock::Ammo, UpkeepStock::Reagent})
        if (std::uint32_t const target = StockTarget(bot, rates, kind))
            bot.stock.push_back({1000u + static_cast<std::uint32_t>(kind), target, StockBit(kind), 0});
    return bot;
}

// The bot knows the abilities off the lists that this class spell needs (FillOffList's bit)
void KnowsOffList(Snapshot& bot, UpkeepIndex const& index, std::uint32_t spellId)
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
    bot.stock.push_back({2000u + static_cast<std::uint32_t>(kind), count, StockBit(kind), diet});
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

NeedList Show(std::string const& who, Snapshot const& bot, UpkeepIndex const& index, NeedRates const& rates)
{
    NeedList const list = ComputeNeeds(bot, index, rates);
    std::cout << " " << who << " (level " << int(bot.level) << ", e " << rates.earnCopperPerMinute << " c/min, walk "
              << rates.walkMinutesPerHour << " min/h):\n";
    for (std::string const& line : DescribeAll(list))
        std::cout << "    " << line << "\n";
    return list;
}

Need const* Find(NeedList const& list, NeedOrder order, UpkeepStock stock = UpkeepStock::Count)
{
    for (Need const& need : list.needs)
        if (need.order == order && (stock == UpkeepStock::Count || need.stock == stock))
            return &need;
    return nullptr;
}

RankValue const* Rank(NeedList const& list, std::uint32_t spell)
{
    for (RankValue const& value : list.ranks)
        if (value.rank.spellId == spell)
            return &value;
    return nullptr;
}

bool Buys(NeedList const& list, std::uint32_t spell)
{
    RankValue const* value = Rank(list, spell);
    return value && value->bought && Find(list, NeedOrder::Riding);
}

void RepairBeforeFood(UpkeepIndex const& index)
{
    std::cout << "Scenario: broken gear and little gold\n";
    Snapshot bot = Bot(index, CLASS_WARRIOR, 30, 60, 30);
    bot.brokenItems = 1;
    bot.lowestDurabilityPercent = 0;
    bot.repairCost = 45;
    SetStock(bot, UpkeepStock::Food, 0);
    NeedList const list = Show("warrior, 60 copper, repair 45 copper, no food", bot, index, Rates(30));
    Need const* repair = Find(list, NeedOrder::Repair);
    Need const* food = Find(list, NeedOrder::FoodWater, UpkeepStock::Food);
    Check(repair && food && &list.needs.front() == repair, "repair is first in the list");
    Check(repair && repair->due && repair->funded == repair->copper, "the repair is due and paid in full");
    Check(food && food->due && food->funded == 15 && food->funded < food->copper,
          "food gets only what is left after the repair");
    Check(food && food->item == ITEM_GOOD_BREAD, "food is the best bread for the level");
}

void AmmoAndWater(UpkeepIndex const& index)
{
    std::cout << "Scenario: a hunter low on ammo, a mage\n";
    Snapshot hunter = Bot(index, CLASS_HUNTER, 30, 100 * GOLD, 30);
    SetStock(hunter, UpkeepStock::Ammo, 50);
    NeedList const hunterNeeds = Show("hunter, 50 arrows", hunter, index, Rates(30));
    Need const* ammo = Find(hunterNeeds, NeedOrder::Ammo);
    Check(ammo && ammo->due && ammo->units == StockTarget(hunter, NeedRates{}, UpkeepStock::Ammo) - 50 &&
              ammo->vendor == VENDOR_ARMS,
          "the hunter gets a due ammo need up to its target");
    Snapshot mage = Bot(index, CLASS_MAGE, 30, 100 * GOLD, 30);
    mage.stock.clear();
    NeedList const mageNeeds = Show("mage, no food or water at all", mage, index, Rates(30));
    Check(!Find(mageNeeds, NeedOrder::FoodWater), "the mage has no food or water need: it conjures");
    Check(Find(mageNeeds, NeedOrder::ReagentPoison, UpkeepStock::Reagent) != nullptr, "the mage needs its reagent");
    Snapshot warrior = Bot(index, CLASS_WARRIOR, 30, 100 * GOLD, 30);
    warrior.stock.clear();
    NeedList const warriorNeeds = Show("warrior, empty bags", warrior, index, Rates(30));
    Check(Find(warriorNeeds, NeedOrder::FoodWater, UpkeepStock::Food) &&
              !Find(warriorNeeds, NeedOrder::FoodWater, UpkeepStock::Water) && !Find(warriorNeeds, NeedOrder::Ammo),
          "a warrior needs food, no water and no ammo");
}

void PetDiet(UpkeepIndex const& index)
{
    std::cout << "Scenario: a hunter's pet\n";
    Snapshot unknown = Bot(index, CLASS_HUNTER, 30, 100 * GOLD, 30);
    NeedList const without = Show("hunter, pet not summoned (diet 0 = unknown)", unknown, index, Rates(30));
    Check(!Find(without, NeedOrder::FoodWater, UpkeepStock::PetFood), "an unknown diet gives no pet food need");
    Snapshot known = unknown;
    known.petDiet = MEAT_DIET;
    NeedList const with = Show("hunter, meat-eating pet, no meat", known, index, Rates(30));
    Need const* petFood = Find(with, NeedOrder::FoodWater, UpkeepStock::PetFood);
    Check(petFood && petFood->due && petFood->item == ITEM_MEAT, "a known diet with no matching food is a need");
    known.stock.push_back({ITEM_MEAT, 50, StockBit(UpkeepStock::PetFood), MEAT_DIET});
    Check(!Find(ComputeNeeds(known, index, Rates(30)), NeedOrder::FoodWater, UpkeepStock::PetFood),
          "enough food of the diet: no need");
}

void Training(UpkeepIndex const& index)
{
    std::cout << "Scenario: training\n";
    Snapshot behind = Bot(index, CLASS_MAGE, 12, 100 * GOLD, 9);
    NeedList const two = Show("mage two levels behind", behind, index, Rates(12));
    Need const* need = Find(two, NeedOrder::Training);
    Check(need && need->due && need->levelsBehind == 2 && need->trainer == TRAINER_MAGE,
          "two levels behind: training is due");
    Check(need && need->units == 3,
          "the level-10 and level-12 spells and the second rank of the first count; the talent rank does not");
    Snapshot one = Bot(index, CLASS_MAGE, 11, 100 * GOLD, 9);
    NeedList const neutral = Show("mage one level behind, neutral", one, index, Rates(11));
    Check(Find(neutral, NeedOrder::Training) && !Find(neutral, NeedOrder::Training)->due,
          "one level behind: a need that joins a passing trip, not due");
    one.traits = Traits(0, TraitRules::MAX_VALUE, false);
    NeedList const thorough = Show("the same mage, thorough", one, index, Rates(11));
    Check(Find(thorough, NeedOrder::Training) && Find(thorough, NeedOrder::Training)->due,
          "a thorough bot's training is due earlier");
    one.traits = Traits(0, TraitRules::MIN_VALUE, false);
    Check(TrainingSpan(-1.0f) == TRAINING_SPAN_CARELESS && TrainingSpan(1.0f) == TRAINING_SPAN_THOROUGH &&
              TrainingSpan(0.0f) == TRAINING_SPAN_LEVELS,
          "the span runs 4 (careless), 2 (neutral), 1 (thorough)");
}

void GroundRiding(UpkeepIndex const& index)
{
    std::cout << "Scenario: ground riding\n";
    NeedList const at20 =
        Show("human warrior at 20 with 100 gold", Bot(index, CLASS_WARRIOR, 20, 100 * GOLD, 20), index, Rates(20));
    Check(Buys(at20, SPELL_APPRENTICE) && Rank(at20, SPELL_APPRENTICE)->mountItem == ITEM_HORSE,
          "buys apprentice riding and the human's horse at 20");
    Check(!Rank(at20, SPELL_JOURNEYMAN)->bought, "not journeyman at 20");
    NeedList const poor =
        Show("the same warrior with 2 gold", Bot(index, CLASS_WARRIOR, 20, 2 * GOLD, 20), index, Rates(20));
    Check(!Find(poor, NeedOrder::Riding) && poor.reserveRiding > 0 && poor.shortfall > 0,
          "without the gold: no purchase, the rank is in the reserve and a shortfall appears");

    Snapshot traveller = Bot(index, CLASS_WARRIOR, 40, 200 * GOLD, 40, {SPELL_APPRENTICE});
    NeedList const at40 = Show("warrior at 40 that walks 50 min/h, 200 gold", traveller, index, Rates(40, 50.0f));
    Check(Buys(at40, SPELL_JOURNEYMAN) && Rank(at40, SPELL_JOURNEYMAN)->mountItem == ITEM_FAST_HORSE,
          "a bot that travels a lot buys journeyman riding at 40");
    std::cout << "  the default walk share buys journeyman at level:";
    std::uint8_t defaultLevel = 0;
    for (std::uint8_t level = 40; level <= LEVEL_CAP && !defaultLevel; ++level)
        if (Buys(ComputeNeeds(Bot(index, CLASS_WARRIOR, level, 200 * GOLD, level, {SPELL_APPRENTICE}), index,
                              Rates(level)),
                 SPELL_JOURNEYMAN))
            defaultLevel = level;
    std::cout << " " << int(defaultLevel) << "\n";

    std::uint8_t rare = 0;
    for (std::uint8_t level = 20; level <= LEVEL_CAP && !rare; ++level)
        if (Buys(ComputeNeeds(Bot(index, CLASS_WARRIOR, level, 200 * GOLD, level), index, Rates(level, 3.0f)),
                 SPELL_APPRENTICE))
            rare = level;
    Show("a warrior that walks 3 min/h, at 20", Bot(index, CLASS_WARRIOR, 20, 200 * GOLD, 20), index, Rates(20, 3.0f));
    std::cout << "  it buys apprentice riding at level " << int(rare) << "\n";
    Check(rare > 20, "a bot that rarely travels buys apprentice riding later");
}

void FlyingRiding(UpkeepIndex const& index)
{
    std::cout << "Scenario: artisan riding\n";
    std::vector<std::uint32_t> const expert = {SPELL_APPRENTICE, SPELL_JOURNEYMAN, SPELL_EXPERT};
    NeedList const levelling = Show("warrior at 70 with expert riding and 1000 gold",
                                    Bot(index, CLASS_WARRIOR, 70, 1000 * GOLD, 70, expert), index, Rates(70));
    Check(!Rank(levelling, SPELL_ARTISAN)->bought, "a levelling bot at 70 does not buy artisan riding");
    Snapshot rich = Bot(index, CLASS_WARRIOR, 80, 12000 * GOLD, 80, expert);
    rich.traits = Traits(0, 0, true);
    NeedList const collector = Show("level-80 mount collector with 12000 gold", rich, index, Rates(80));
    RankValue const* artisan = Rank(collector, SPELL_ARTISAN);
    Check(Buys(collector, SPELL_ARTISAN), "a rich level-80 collector buys artisan riding");
    Check(artisan && artisan->mountItem == ITEM_FAST_GRYPHON && artisan->mount == 100 * GOLD &&
              artisan->currencyMounts == 1,
          "the gold+currency gryphon is never the price: the 100-gold one is");
    Snapshot plain = rich;
    plain.traits.reset();
    Check(!Rank(ComputeNeeds(plain, index, Rates(80)), SPELL_ARTISAN)->bought,
          "the same rich bot without the interest does not buy it while flying legs save nothing");
    Check(!Rank(collector, SPELL_COLD_WEATHER)->eligible || !Rank(collector, SPELL_COLD_WEATHER)->bought ||
              Rank(collector, SPELL_COLD_WEATHER)->want > 0,
          "cold weather flying is worth nothing without flying legs, only a want");
}

void ReserveAndShortfall(UpkeepIndex const& index)
{
    std::cout << "Scenario: the reserve towards level 20 (a 3-gold rank and apprentice riding), 1 gold\n";
    std::uint64_t last = 0;
    bool grows = true;
    for (std::uint8_t level = 15; level <= 19; ++level)
    {
        NeedList const list = ComputeNeeds(Bot(index, CLASS_WARRIOR, level, 1 * GOLD, level), index, Rates(level));
        std::cout << "    level " << int(level) << ": reserve " << Money(list.reserve) << " (training "
                  << Money(list.reserveTraining) << ", riding " << Money(list.reserveRiding) << "), gold "
                  << Money(list.money) << ", shortfall " << Money(list.shortfall) << "\n";
        grows = grows && list.reserve >= last;
        if (level == 17)
            Check(list.reserveTraining < 30000 && list.reserveRiding == 0 && list.shortfall == 0,
                  "three levels before 20: neither the costly rank nor riding is in the reserve, no shortfall");
        if (level == 18)
            Check(list.reserveTraining >= 30000 && list.reserveRiding > 0,
                  "two levels before 20 (TrainingReserveLevels, RIDING_REACH_LEVELS) both join the reserve");
        if (level == 19)
            Check(list.shortfall == list.reserve - list.money, "below the reserve: the shortfall is the gap");
        last = list.reserve;
    }
    Check(grows, "the reserve grows as the expensive level approaches");
    NeedList const rich = ComputeNeeds(Bot(index, CLASS_WARRIOR, 19, 100 * GOLD, 19), index, Rates(19));
    Check(rich.shortfall == 0, "with gold above the reserve there is no shortfall");
    Snapshot frugal = Bot(index, CLASS_WARRIOR, 19, 1 * GOLD, 19);
    frugal.traits = Traits(TraitRules::MAX_VALUE, 0, false);
    Snapshot spendthrift = frugal;
    spendthrift.traits = Traits(TraitRules::MIN_VALUE, 0, false);
    NeedList const neutralList = ComputeNeeds(Bot(index, CLASS_WARRIOR, 19, 1 * GOLD, 19), index, Rates(19));
    NeedList const frugalList = Show("frugal warrior at 19", frugal, index, Rates(19));
    NeedList const spendList = Show("spendthrift warrior at 19", spendthrift, index, Rates(19));
    Check(frugalList.reserve > neutralList.reserve && spendList.reserve < neutralList.reserve,
          "thrift scales the reserve: frugal keeps more back, spendthrift less");
}

void ClassMounts(UpkeepIndex const& index)
{
    std::cout << "Scenario: class mounts come with training\n";
    Snapshot paladin = Bot(index, CLASS_PALADIN, 50, 100 * GOLD, 0, {SPELL_APPRENTICE});
    KnowsOffList(paladin, index, SPELL_CHARGER);
    NeedList const before = Show("paladin at 50 with Summon Warhorse, apprentice riding", paladin, index, Rates(50));
    Check(!Find(before, NeedOrder::Training), "without journeyman riding the charger is no need yet");
    Snapshot ridden = Bot(index, CLASS_PALADIN, 50, 100 * GOLD, 0, {SPELL_APPRENTICE, SPELL_JOURNEYMAN});
    Check(!Find(ComputeNeeds(ridden, index, Rates(50)), NeedOrder::Training),
          "without Summon Warhorse (on no list, not known) no charger");
    KnowsOffList(ridden, index, SPELL_CHARGER);
    NeedList const charger = Show("the paladin with Summon Warhorse and journeyman riding", ridden, index, Rates(50));
    Need const* need = Find(charger, NeedOrder::Training);
    Check(need && need->units == 1 && need->item == SPELL_CHARGER && need->copper == 3500 &&
              need->trainer == TRAINER_PALADIN,
          "with journeyman riding the charger is a training need; Holy Shield rank 2 waits for its talent");
    KnowsOffList(ridden, index, SPELL_HOLY_SHIELD_2);
    NeedList const talent = ComputeNeeds(ridden, index, Rates(50));
    need = Find(talent, NeedOrder::Training);
    Check(need && need->units == 2, "with the talent known its next rank counts too");

    Snapshot druid = Bot(index, CLASS_DRUID, 71, 1000 * GOLD, 0);
    KnowsOffList(druid, index, SPELL_SWIFT_FLIGHT_FORM);
    Check(!Find(Show("druid at 71 with Flight Form, no riding skill at all", druid, index, Rates(71)),
                NeedOrder::Training),
          "a riding skill the snapshot lacks is 0, not met: no Swift Flight Form");
    Snapshot expert = Bot(index, CLASS_DRUID, 71, 1000 * GOLD, 0, {SPELL_APPRENTICE, SPELL_JOURNEYMAN, SPELL_EXPERT});
    KnowsOffList(expert, index, SPELL_SWIFT_FLIGHT_FORM);
    Check(!Find(ComputeNeeds(expert, index, Rates(71)), NeedOrder::Training), "riding 225: no Swift Flight Form yet");
    Snapshot artisan =
        Bot(index, CLASS_DRUID, 71, 1000 * GOLD, 0, {SPELL_APPRENTICE, SPELL_JOURNEYMAN, SPELL_EXPERT, SPELL_ARTISAN});
    KnowsOffList(artisan, index, SPELL_SWIFT_FLIGHT_FORM);
    NeedList const flight = Show("the druid with artisan riding", artisan, index, Rates(71));
    need = Find(flight, NeedOrder::Training);
    Check(need && need->item == SPELL_SWIFT_FLIGHT_FORM, "riding 300 and Flight Form: Swift Flight Form is a need");
}

// Journeyman at 40 (60 gold) is worth about 37 gold to a bot walking 30 min/h: only a want buys it
// The spell Felsteed teaches is no off-list ability: the rules read Felsteed's bit (FillKnown sets it once 5784 is
// known), so a bot without Felsteed counts both and one with it only Dreadsteed
void LearnSpellMount(UpkeepIndex const& index)
{
    std::cout << "Scenario: a class mount needs what a learn spell teaches\n";
    Snapshot fresh = Bot(index, CLASS_WARLOCK, 40, 100 * GOLD, 0, {SPELL_APPRENTICE, SPELL_JOURNEYMAN});
    NeedList const both = Show("warlock at 40 without Felsteed", fresh, index, Rates(40));
    Need const* need = Find(both, NeedOrder::Training);
    Check(need && need->units == 2 && need->copper == 6000 && need->item == SPELL_FELSTEED &&
              need->trainer == TRAINER_WARLOCK,
          "Felsteed and Dreadsteed both count: Dreadsteed's ability is taught by a counted spell");
    Snapshot felsteed = Bot(index, CLASS_WARLOCK, 40, 100 * GOLD, 20, {SPELL_APPRENTICE, SPELL_JOURNEYMAN});
    NeedList const one = Show("warlock at 40 with Felsteed", felsteed, index, Rates(40));
    need = Find(one, NeedOrder::Training);
    Check(need && need->units == 1 && need->item == SPELL_DREADSTEED, "with Felsteed known only Dreadsteed counts");
    Snapshot apprentice = Bot(index, CLASS_WARLOCK, 40, 100 * GOLD, 20, {SPELL_APPRENTICE});
    Check(!Find(ComputeNeeds(apprentice, index, Rates(40)), NeedOrder::Training),
          "without journeyman riding no Dreadsteed");
}

void WantNoFlip(UpkeepIndex const& index)
{
    std::cout << "Scenario: a want and growing gold, journeyman riding at 40\n";
    for (bool const spendthrift : {false, true})
    {
        std::string const who = spendthrift ? "spendthrift" : "collector";
        bool steady = true;
        bool falls = true;
        bool stays = true;
        bool bought = false;
        std::uint64_t reserve = 0;
        std::uint64_t lastShortfall = 0;
        std::uint32_t buyAt = 0;
        for (std::uint32_t gold = 0; gold <= 200; ++gold)
        {
            Snapshot bot = Bot(index, CLASS_WARRIOR, 40, gold * GOLD, 40, {SPELL_APPRENTICE});
            bot.traits = spendthrift ? Traits(TraitRules::MIN_VALUE, 0, false) : Traits(0, 0, true);
            NeedList const list = ComputeNeeds(bot, index, Rates(40));
            if (!gold)
            {
                reserve = list.reserve;
                lastShortfall = list.shortfall;
            }
            steady = steady && list.reserve == reserve;
            falls = falls && list.shortfall <= lastShortfall;
            lastShortfall = list.shortfall;
            bool const buys = Buys(list, SPELL_JOURNEYMAN);
            stays = stays && (!bought || buys);
            if (buys && !bought)
                buyAt = gold;
            bought = bought || buys;
        }
        std::cout << "    " << who << ": reserve " << Money(reserve) << " at every gold from 0 to 200 g, buys "
                  << "journeyman from " << buyAt << " g\n";
        Snapshot at = Bot(index, CLASS_WARRIOR, 40, buyAt * GOLD, 40, {SPELL_APPRENTICE});
        at.traits = spendthrift ? Traits(TraitRules::MIN_VALUE, 0, false) : Traits(0, 0, true);
        Show(who + " at the gold it buys with", at, index, Rates(40));
        Check(steady, who + ": the want never moves the reserve");
        Check(bought && falls && stays, who + ": more gold never brings a shortfall back or undoes the purchase");
    }
}

void FrugalBuy(UpkeepIndex const& index)
{
    std::cout << "Scenario: a frugal bot at 20 (reserve x1.5) and apprentice riding\n";
    bool clean = true;
    bool stays = true;
    bool bought = false;
    std::uint32_t buyAt = 0;
    for (std::uint32_t copper = 0; copper <= 30 * GOLD; copper += 10 * COPPER_PER_SILVER)
    {
        Snapshot bot = Bot(index, CLASS_WARRIOR, 20, copper, 20);
        bot.traits = Traits(TraitRules::MAX_VALUE, 0, false);
        NeedList const list = ComputeNeeds(bot, index, Rates(20));
        bool const buys = Buys(list, SPELL_APPRENTICE);
        clean = clean && !(buys && list.shortfall > 0);
        stays = stays && (!bought || buys);
        if (buys && !bought)
            buyAt = copper;
        bought = bought || buys;
    }
    Snapshot at = Bot(index, CLASS_WARRIOR, 20, buyAt, 20);
    at.traits = Traits(TraitRules::MAX_VALUE, 0, false);
    NeedList const list = Show("frugal warrior at the gold it buys with", at, index, Rates(20));
    Check(bought && stays && list.money >= list.reserve, "it buys once its gold covers the scaled reserve");
    Check(clean, "it never buys the rank and reports a shortfall in the same list");
}

void StaleBits(UpkeepIndex const& index)
{
    std::cout << "Scenario: a snapshot of an older index\n";
    Snapshot bot = Bot(index, CLASS_MAGE, 12, 100 * GOLD, 10);
    bot.upkeepVersion = index.version + 1;
    NeedList const list = ComputeNeeds(bot, index, Rates(12));
    Check(!Find(list, NeedOrder::Training) && list.ranks.empty(), "bit sets of another index version are not read");
}
}  // namespace

int main()
{
    std::cout << "Tuning [E]:\n";
    for (std::string const& line : TuningLines())
        std::cout << "  " << line << "\n";
    std::cout << "  riding horizon: " << RidingHorizonHours(20) << " h at 20, " << RidingHorizonHours(40)
              << " h at 40, " << RidingHorizonHours(70) << " h at 70, " << RidingHorizonHours(80) << " h at 80\n";
    UpkeepIndex const index = Fixture();
    RepairBeforeFood(index);
    AmmoAndWater(index);
    PetDiet(index);
    Training(index);
    GroundRiding(index);
    FlyingRiding(index);
    ReserveAndShortfall(index);
    ClassMounts(index);
    LearnSpellMount(index);
    WantNoFlip(index);
    FrugalBuy(index);
    StaleBits(index);

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
