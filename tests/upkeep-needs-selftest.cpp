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
// buys a rank and reports a shortfall in the same list. The repair need (WU5): due below 30 % or with a broken item,
// an errand of its own when the gold pays a worn item and the errand retry is over, and short gold repairs first.
// Shopping (WU6): ammo of the ranged weapon's type only, a due stock as an errand of its own when the gold pays a lot,
// the shopping list in the fixed order within each need's funded share, the vendor covering the most due needs (the
// nearest on a tie, repairers only for a repair), and the use per hour measured from stock falls between visits.
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
NeedRates Measured(std::uint8_t level, float walk, float sky = 0.0f, float north = 0.0f)
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

NeedList Show(std::string const& who, Snapshot const& bot, UpkeepIndex const& index, NeedRates const& rates)
{
    NeedList const list = ComputeNeeds(bot, index, rates);
    std::cout << " " << who << " (level " << int(bot.level) << ", e " << rates.earnCopperPerMinute << " c/min, walk "
              << rates.walkMinutesPerHour << " min/h, flight " << rates.flightMinutesPerHour << ", northrend "
              << rates.northrendFlightMinutesPerHour << "):\n";
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

void RepairDueRule()
{
    std::cout << "Scenario: the repair need's due rule and the repair errand\n";
    struct Case
    {
        std::uint8_t broken;
        std::uint8_t lowest;
        bool due;
        char const* what;
    };
    Case const cases[] = {
        {0, 29, true, "due below 30 %"},
        {1, 80, true, "due with a broken item"},
        {0, 30, false, "not due at 30 % with nothing broken"},
        {0, 100, false, "not due with unworn gear"},
    };
    for (Case const& c : cases)
    {
        std::cout << "    broken " << int(c.broken) << ", lowest " << int(c.lowest)
                  << " %: " << (RepairDue(c.broken, c.lowest) ? "due" : "not due") << "\n";
        Check(RepairDue(c.broken, c.lowest) == c.due, c.what);
    }

    struct ErrandCase
    {
        std::uint8_t broken;
        std::uint8_t lowest;
        std::uint64_t cheapest;
        std::uint64_t money;
        bool errand;
        char const* what;
    };
    ErrandCase const errands[] = {
        {0, 10, 120, 5000, true, "worn gear and the gold for it: an errand"},
        {1, 0, 120, 120, true, "a broken item and gold for exactly the cheapest item: an errand"},
        {1, 0, 120, 119, false, "gold below the cheapest worn item: no trip for nothing"},
        {1, 0, 120, 0, false, "no gold: no errand, the bot keeps playing"},
        {0, 60, 120, 5000, false, "gear not due: no errand"},
        {1, 0, 0, 5000, false, "nothing priced: no errand"},
    };
    for (ErrandCase const& c : errands)
    {
        bool const errand = RepairErrandDue(c.broken, c.lowest, c.cheapest, c.money);
        std::cout << "    broken " << int(c.broken) << ", lowest " << int(c.lowest) << " %, cheapest item "
                  << c.cheapest << " c, gold " << c.money << " c: " << (errand ? "errand" : "no errand") << "\n";
        Check(errand == c.errand, c.what);
    }

    // The trigger asks RepairErrandDue only after the errand retry time, as the bag errand does
    WorldTravel::SlotMeasure measure;
    Check(WorldTravel::ErrandRetryOver(measure, 1000), "never checked: the repair errand may start");
    WorldTravel::StampErrandCheck(measure, 1000);
    Check(!WorldTravel::ErrandRetryOver(measure, 1000 + WorldTravel::ERRAND_RETRY_MS - 1),
          "inside the retry time: the repair errand waits");
    Check(WorldTravel::ErrandRetryOver(measure, 1000 + WorldTravel::ERRAND_RETRY_MS),
          "after the retry time: the repair errand may start again");
    Check(!WorldTravel::ErrandDue(measure, 100, 1000 + WorldTravel::ERRAND_RETRY_MS - 1) &&
              WorldTravel::ErrandDue(measure, 100, 1000 + WorldTravel::ERRAND_RETRY_MS),
          "full bags keep the same retry time");
}

void PartialRepair(UpkeepIndex const& index)
{
    std::cout << "Scenario: a broken item, gold for a part of the repair, food and training waiting\n";
    Snapshot bot = Bot(index, CLASS_WARRIOR, 30, 30, 30);
    bot.brokenItems = 2;
    bot.lowestDurabilityPercent = 0;
    bot.repairCost = 450;
    SetStock(bot, UpkeepStock::Food, 0);
    NeedList const list = Show("warrior, 30 copper, repair 450 copper, no food", bot, index, Rates(30));
    Need const* repair = Find(list, NeedOrder::Repair);
    Need const* food = Find(list, NeedOrder::FoodWater, UpkeepStock::Food);
    Check(repair && &list.needs.front() == repair && repair->due, "repair is first and due");
    Check(repair && repair->funded == 30 && repair->funded < repair->copper, "all the gold goes to the repair");
    Check(food && food->funded == 0, "food waits until the repair is paid");
    Check(list.shortfall > 0, "the missing gold is a shortfall for the planner");

    bot.brokenItems = 0;
    bot.lowestDurabilityPercent = 30;
    NeedList const worn = ComputeNeeds(bot, index, Rates(30));
    Need const* wornRepair = Find(worn, NeedOrder::Repair);
    Check(wornRepair && !wornRepair->due && wornRepair->funded == 30,
          "worn gear at 30 % is a need, not due: it joins a trip that passes and is still funded first");
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
    known.stock.push_back({ITEM_MEAT, 50, StockBit(UpkeepStock::PetFood), MEAT_DIET, 0});
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

// The first level from 'from' on at which the bot buys the rank with these travel minutes (0 = never)
template <typename MakeBot, typename MakeRates>
std::uint8_t FirstBuy(UpkeepIndex const& index, std::uint8_t from, std::uint32_t spell, MakeBot makeBot,
                      MakeRates makeRates)
{
    for (std::uint8_t level = from; level <= LEVEL_CAP; ++level)
        if (Buys(ComputeNeeds(makeBot(level), index, makeRates(level)), spell))
            return level;
    return 0;
}

// The WU3 riding scenarios against the measured travel shares (WU14's exit), and where ranks show BUY with them
void MeasuredRiding(UpkeepIndex const& index)
{
    std::cout << "Scenario: riding by the measured travel shares\n";
    NeedRates unmeasured = Rates(40);
    unmeasured.walkMinutesPerHour = 0.0f;
    FillTravel(unmeasured, WorldTravel::SlotMeasure{});
    Check(unmeasured.walkMinutesPerHour == 0.0f && Rates(40).walkMinutesPerHour == DEFAULT_WALK_MINUTES_PER_HOUR,
          "without a closed window the measure changes nothing: the default walk share stays");
    NeedRates const half = Measured(40, 30.0f);
    Check(half.walkMinutesPerHour == 30.0f && half.flightMinutesPerHour == 0.0f, "30 minutes walked: 30 min/h");

    Snapshot const traveller = Bot(index, CLASS_WARRIOR, 40, 200 * GOLD, 40, {SPELL_APPRENTICE});
    Check(Buys(Show("warrior at 40 that walked 50 of the last 60 minutes", traveller, index, Measured(40, 50.0f)),
               SPELL_JOURNEYMAN),
          "a heavy walker buys journeyman riding at 40");
    Check(!Buys(ComputeNeeds(traveller, index, Measured(40, 30.0f)), SPELL_JOURNEYMAN),
          "one walking 30 min/h does not yet");
    std::cout << "  journeyman riding (200 gold, apprentice known) bought from level, by measured walk share:";
    for (float const walk : {20.0f, 30.0f, 40.0f, 48.0f, 50.0f, 60.0f})
        std::cout << " " << walk << " min/h -> "
                  << int(FirstBuy(
                         index, 40, SPELL_JOURNEYMAN, [&index](std::uint8_t level)
                         { return Bot(index, CLASS_WARRIOR, level, 200 * GOLD, level, {SPELL_APPRENTICE}); },
                         [walk](std::uint8_t level) { return Measured(level, walk); }));
    std::cout << " (0 = never)\n";
    std::cout << "  apprentice riding (200 gold) bought from level, by measured walk share:";
    std::uint8_t rare = 0;
    for (float const walk : {3.0f, 10.0f, 30.0f, 60.0f})
    {
        std::uint8_t const first = FirstBuy(
            index, 20, SPELL_APPRENTICE,
            [&index](std::uint8_t level) { return Bot(index, CLASS_WARRIOR, level, 200 * GOLD, level); },
            [walk](std::uint8_t level) { return Measured(level, walk); });
        std::cout << " " << walk << " min/h -> " << int(first);
        if (walk == 3.0f)
            rare = first;
    }
    std::cout << "\n";
    Check(
        Buys(ComputeNeeds(Bot(index, CLASS_WARRIOR, 20, 100 * GOLD, 20), index, Measured(20, 30.0f)), SPELL_APPRENTICE),
        "a bot walking 30 min/h buys apprentice riding at 20");
    Check(rare > 20, "one that rarely travels (3 min/h) buys it later");

    std::vector<std::uint32_t> const ground = {SPELL_APPRENTICE, SPELL_JOURNEYMAN};
    Snapshot const grounded = Bot(index, CLASS_WARRIOR, 60, 2000 * GOLD, 60, ground);
    NeedList const noSky =
        Show("warrior at 60, journeyman, never under open sky", grounded, index, Measured(60, 40.0f));
    Check(Rank(noSky, SPELL_EXPERT) && Rank(noSky, SPELL_EXPERT)->savedMinutes == 0.0f &&
              !Rank(noSky, SPELL_EXPERT)->bought,
          "no flight share: expert riding saves nothing");
    NeedList const sky = Show("the same warrior, 30 of 40 minutes walked in Outland's open sky", grounded, index,
                              Measured(60, 10.0f, 30.0f));
    Check(Rank(sky, SPELL_EXPERT) && Rank(sky, SPELL_EXPERT)->savedMinutes > 0.0f,
          "walks under open sky: expert riding saves the flight time");
    std::cout << "  expert riding (2000 gold, journeyman known) bought from level, by measured flight share:";
    for (float const flight : {10.0f, 30.0f, 50.0f})
        std::cout << " " << flight << " min/h -> "
                  << int(FirstBuy(
                         index, 60, SPELL_EXPERT, [&index, &ground](std::uint8_t level)
                         { return Bot(index, CLASS_WARRIOR, level, 2000 * GOLD, level, ground); },
                         [flight](std::uint8_t level) { return Measured(level, 0.0f, flight); }));
    std::cout << " (0 = never)\n";

    std::vector<std::uint32_t> const expert = {SPELL_APPRENTICE, SPELL_JOURNEYMAN, SPELL_EXPERT};
    NeedList const levelling =
        Show("warrior at 70 with expert riding, 6000 gold, flying 40 min/h",
             Bot(index, CLASS_WARRIOR, 70, 6000 * GOLD, 70, expert), index, Measured(70, 10.0f, 40.0f));
    Check(Rank(levelling, SPELL_ARTISAN) && Rank(levelling, SPELL_ARTISAN)->savedMinutes > 0.0f &&
              !Rank(levelling, SPELL_ARTISAN)->bought,
          "a levelling bot at 70 with 6000 gold does not buy artisan riding by its flight share");
    Snapshot const north = Bot(index, CLASS_WARRIOR, 77, 2000 * GOLD, 77, expert);
    NeedList const cold = Show("warrior at 77 with expert riding, 30 min/h under Northrend's sky", north, index,
                               Measured(77, 10.0f, 0.0f, 30.0f));
    Check(Rank(cold, SPELL_COLD_WEATHER) && Rank(cold, SPELL_COLD_WEATHER)->savedMinutes > 0.0f,
          "the Northrend share gives cold weather flying a worth");
    RankValue const* dry = Rank(ComputeNeeds(north, index, Measured(77, 40.0f)), SPELL_COLD_WEATHER);
    Check(dry && dry->savedMinutes == 0.0f, "without legs in Northrend it saves nothing");

    // WU3-fix reserve limits: with flight minutes the air rank waits for the ground rank below it, and two ranks of
    // one list share the gold
    Snapshot climber = Bot(index, CLASS_WARRIOR, 60, 2000 * GOLD, 60, {SPELL_APPRENTICE});
    climber.traits = Traits(0, 0, true);
    NeedList const both =
        Show("collector at 60 with apprentice riding, 2000 gold, walking 60 min/h (30 under open sky)", climber, index,
             Measured(60, 30.0f, 30.0f));
    Check(Buys(both, SPELL_JOURNEYMAN) && Rank(both, SPELL_EXPERT) && !Rank(both, SPELL_EXPERT)->bought,
          "journeyman first: expert riding needs its skill, whatever the flight share");
    Snapshot tight = climber;
    tight.money = static_cast<std::uint32_t>(Rank(both, SPELL_JOURNEYMAN)->Price()) + 1 * GOLD;
    NeedList const one =
        Show("the same collector with gold for journeyman alone", tight, index, Measured(60, 30.0f, 30.0f));
    std::uint64_t riding = 0;
    for (Need const& need : one.needs)
        if (need.order == NeedOrder::Riding)
            riding += need.copper;
    Check(Buys(one, SPELL_JOURNEYMAN) && riding <= tight.money, "only the nearer rank is bought, within the gold");
}

// The riding need stays until the bot has the rank and a mount for it (WU14): a known rank without its mount is the
// need, priced at the mount, with no trainer
void MountMissing(UpkeepIndex const& index)
{
    std::cout << "Scenario: a known rank without its mount\n";
    Snapshot walker = Bot(index, CLASS_WARRIOR, 20, 100 * GOLD, 20, {SPELL_APPRENTICE});
    walker.groundMountSkill = 0;
    NeedList const horse = Show("warrior at 20 with apprentice riding, no mount", walker, index, Rates(20));
    Need const* need = Find(horse, NeedOrder::Riding);
    RankValue const* apprentice = Rank(horse, SPELL_APPRENTICE);
    Check(need && need->item == SPELL_APPRENTICE && need->copper == 1 * GOLD && need->trainer == 0 &&
              need->vendor == VENDOR_MOUNTS && apprentice && apprentice->known && apprentice->training == 0,
          "the rank known, the horse missing: the need is the horse alone, at the mount vendor");
    Check(apprentice && apprentice->savedMinutes == DEFAULT_WALK_MINUTES_PER_HOUR * (1.0f - 1.0f / 1.6f),
          "a rank without its mount carries nobody: the horse saves the rank's whole time");
    Check(!Find(ComputeNeeds(Bot(index, CLASS_WARRIOR, 20, 100 * GOLD, 20, {SPELL_APPRENTICE}), index, Rates(20)),
                NeedOrder::Riding),
          "with rank and mount the need is done");

    Snapshot slow = Bot(index, CLASS_WARRIOR, 40, 200 * GOLD, 40, {SPELL_APPRENTICE, SPELL_JOURNEYMAN});
    slow.groundMountSkill = 75;
    NeedList const fast =
        Show("warrior at 40 with journeyman riding and the slow horse only", slow, index, Measured(40, 50.0f));
    need = Find(fast, NeedOrder::Riding);
    Check(need && need->item == SPELL_JOURNEYMAN && need->copper == 10 * GOLD && need->trainer == 0,
          "journeyman known on a slow horse: the fast horse is the need");
    Snapshot poor = slow;
    poor.money = 1 * GOLD;
    NeedList const unfunded = ComputeNeeds(poor, index, Measured(40, 50.0f));
    Check(!Find(unfunded, NeedOrder::Riding) && unfunded.reserveRiding == 10 * GOLD,
          "without the gold the mount is no purchase: it waits in the reserve");

    Snapshot flyer = Bot(index, CLASS_WARRIOR, 70, 1000 * GOLD, 70, {SPELL_APPRENTICE, SPELL_JOURNEYMAN, SPELL_EXPERT});
    flyer.flyingMountSkill = 0;
    NeedList const wings = Show("warrior at 70 with expert riding, no flyer, 40 min/h under open sky", flyer, index,
                                Measured(70, 10.0f, 40.0f));
    RankValue const* gryphon = Rank(wings, SPELL_EXPERT);
    Check(gryphon && gryphon->known && gryphon->Price() == 50 * GOLD && Buys(wings, SPELL_EXPERT) &&
              Rank(wings, SPELL_ARTISAN) && !Rank(wings, SPELL_ARTISAN)->bought,
          "expert known without a flyer: the gryphon is the need, artisan waits for it");
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

void AmmoType(UpkeepIndex const& index)
{
    std::cout << "Scenario: the ammo of the ranged weapon (WU3 review NOTE 8)\n";
    Check(AmmoFor(UpkeepItem::SUBCLASS_WEAPON_BOW) == UpkeepItem::SUBCLASS_ARROW &&
              AmmoFor(UpkeepItem::SUBCLASS_WEAPON_CROSSBOW) == UpkeepItem::SUBCLASS_ARROW &&
              AmmoFor(UpkeepItem::SUBCLASS_WEAPON_GUN) == UpkeepItem::SUBCLASS_BULLET && AmmoFor(16) == 0 &&
              AmmoFor(0) == 0,
          "bows and crossbows fire arrows, guns bullets, thrown weapons and wands none");
    Snapshot bow = Bot(index, CLASS_HUNTER, 30, 100 * GOLD, 30);
    SetStock(bow, UpkeepStock::Ammo, 0);
    bow.stock.push_back({ITEM_BULLET, 2000, StockBit(UpkeepStock::Ammo), 0,
                         static_cast<std::uint8_t>(UpkeepItem::SUBCLASS_BULLET), 1, DAMAGE_BULLET});
    NeedList const bowNeeds = Show("hunter with a bow, 2000 bullets and no arrows", bow, index, Rates(30));
    Need const* arrows = Find(bowNeeds, NeedOrder::Ammo);
    Check(Stock(bow, UpkeepStock::Ammo) == 0 && arrows && arrows->due && arrows->item == ITEM_RAZOR_ARROW,
          "the bullets do not count for a bow and the need buys arrows, not the cheaper bullets");
    bool bulletOffered = false;
    for (VendorOffer const& offer : index.Offers(UpkeepStock::Ammo))
        bulletOffered = bulletOffered || (offer.item == ITEM_BULLET && OfferFits(offer, bow, UpkeepStock::Ammo));
    Check(!bulletOffered, "no bullet offer fits a hunter with a bow");
    Snapshot gun = bow;
    gun.ammoSubClass = static_cast<std::uint8_t>(UpkeepItem::SUBCLASS_BULLET);
    gun.ammoDamage = DAMAGE_BULLET;
    Check(Stock(gun, UpkeepStock::Ammo) == 2000 && !Find(ComputeNeeds(gun, index, Rates(30)), NeedOrder::Ammo),
          "with a gun the bullets count: no ammo need");
    gun.stock.clear();
    NeedList const gunNeeds = ComputeNeeds(gun, index, Rates(30));
    Need const* bullets = Find(gunNeeds, NeedOrder::Ammo);
    Check(bullets && bullets->item == ITEM_BULLET, "an empty gun hunter buys bullets");
    Snapshot none = bow;
    none.ammoSubClass = 0;
    Check(!Find(ComputeNeeds(none, index, Rates(30)), NeedOrder::Ammo), "a hunter without a ranged weapon: no ammo");
}

void StockErrand(UpkeepIndex const& index)
{
    std::cout << "Scenario: a low stock makes the errand due (the same call as the repair)\n";
    Snapshot hunter = Bot(index, CLASS_HUNTER, 30, 100 * GOLD, 30);
    SetStock(hunter, UpkeepStock::Ammo, 50);
    NeedList const due = ComputeNeeds(hunter, index, Rates(30));
    Need const* ammo = Find(due, NeedOrder::Ammo);
    Check(ammo && ammo->lotSize == 200 && ammo->lotPrice == 10 && ammo->target > 0 && StockErrandDue(due) &&
              PayableKinds(due, true) == StockBit(UpkeepStock::Ammo),
          "ammo below a quarter of its target with gold for a lot: the errand is due");
    hunter.money = 5;
    NeedList const poor = ComputeNeeds(hunter, index, Rates(30));
    Check(!StockErrandDue(poor) && !PayableKinds(poor, true), "gold below one lot: no errand (it would buy nothing)");
    hunter.money = 100 * GOLD;
    SetStock(hunter, UpkeepStock::Ammo, StockTarget(hunter, NeedRates{}, UpkeepStock::Ammo) / 2);
    NeedList const half = ComputeNeeds(hunter, index, Rates(30));
    Check(Find(half, NeedOrder::Ammo) && !StockErrandDue(half) &&
              PayableKinds(half, false) == StockBit(UpkeepStock::Ammo),
          "half the target: a need that joins a trip, no errand of its own");
}

bool CopperWithinFunded(std::vector<ShopLine> const& lines)
{
    bool within = true;
    for (ShopLine const& line : lines)
    {
        within = within && line.copper <= line.funded && line.copper == std::uint64_t(line.lots) * line.lotPrice;
        std::cout << "    " << Describe(line) << "\n";
    }
    return within;
}

ShopLine const* Line(std::vector<ShopLine> const& lines, UpkeepStock kind)
{
    for (ShopLine const& line : lines)
        if (line.kind == kind)
            return &line;
    return nullptr;
}

void ShoppingOrder(UpkeepIndex const& index)
{
    std::cout << "Scenario: two needs and gold for one buy by the order (ammo, food and water, reagents)\n";
    Snapshot hunter = Bot(index, CLASS_HUNTER, 10, 100 * GOLD, 10);
    SetStock(hunter, UpkeepStock::Ammo, 0);
    SetStock(hunter, UpkeepStock::Food, 0);
    NeedList const rich = ComputeNeeds(hunter, index, Rates(10));
    Need const* ammoNeed = Find(rich, NeedOrder::Ammo);
    hunter.money = static_cast<std::uint32_t>(ammoNeed ? ammoNeed->copper : 0) + 20;  // under one 25c bread
    NeedList const hunterNeeds = Show("hunter, no arrows, no food, gold for the arrows", hunter, index, Rates(10));
    std::cout << "   at the general goods vendor (bread and arrows):\n";
    std::vector<ShopLine> const general = PlanShopping(hunterNeeds, index, hunter, VENDOR_GENERAL);
    bool const within = CopperWithinFunded(general);
    ShopLine const* ammo = Line(general, UpkeepStock::Ammo);
    ShopLine const* food = Line(general, UpkeepStock::Food);
    Check(ammo && ammo->outcome == ShopOutcome::Buy && ammo->item == ITEM_ARROW &&
              std::uint64_t(ammo->lots) * ammo->lotSize >= ammo->units && food &&
              food->outcome == ShopOutcome::Budget && food->funded < food->lotPrice,
          "the hunter buys the arrows to the target; the food gets less than a lot and is skipped for the budget");
    Check(within, "every line spends at most its funded share, in whole lots");
    std::vector<ShopLine> const arms = PlanShopping(hunterNeeds, index, hunter, VENDOR_ARMS);
    Check(Line(arms, UpkeepStock::Food) && Line(arms, UpkeepStock::Food)->outcome == ShopOutcome::NotSold,
          "the weapon vendor sells no food: not sold");

    Snapshot mage = Bot(index, CLASS_MAGE, 12, 100 * GOLD, 12);
    mage.stock.clear();
    NeedRates measured = Rates(12);
    measured.usePerHour[static_cast<std::size_t>(UpkeepStock::Food)] = 4.0f;  // a mage that eats bought food
    NeedList const mageRich = ComputeNeeds(mage, index, measured);
    Need const* foodNeed = Find(mageRich, NeedOrder::FoodWater, UpkeepStock::Food);
    mage.money = static_cast<std::uint32_t>(foodNeed ? foodNeed->copper : 0) + 500;  // under one 10s powder
    NeedList const mageNeeds =
        Show("mage with a measured food use, no food, no powder, gold for the food", mage, index, measured);
    Need const* reagent = Find(mageNeeds, NeedOrder::ReagentPoison, UpkeepStock::Reagent);
    std::cout << "   at the inn, then at the reagent vendor:\n";
    std::vector<ShopLine> const inn = PlanShopping(mageNeeds, index, mage, VENDOR_INN);
    std::vector<ShopLine> const reagents = PlanShopping(mageNeeds, index, mage, VENDOR_REAGENT);
    bool const mageWithin = CopperWithinFunded(inn) && CopperWithinFunded(reagents);
    Check(foodNeed && foodNeed->target == 12 && Line(inn, UpkeepStock::Food) &&
              Line(inn, UpkeepStock::Food)->outcome == ShopOutcome::Buy && reagent && reagent->funded == 500 &&
              Line(reagents, UpkeepStock::Reagent) &&
              Line(reagents, UpkeepStock::Reagent)->outcome == ShopOutcome::Budget,
          "the measured use replaces the mage's default 0; food before reagents, the powder skipped for the budget");
    Check(mageWithin, "the mage's lines spend at most their funded shares");
}

void KeptItems(UpkeepIndex const& index)
{
    std::cout << "Scenario: only what the generic sell keeps (WU6 review finding 1)\n";
    Snapshot novice = Bot(index, CLASS_MAGE, 9, 100 * GOLD, 9);
    novice.stock.clear();
    Snapshot adept = Bot(index, CLASS_MAGE, 12, 100 * GOLD, 12);
    adept.stock.clear();
    Snapshot warrior = Bot(index, CLASS_WARRIOR, 30, 100 * GOLD, 30);
    warrior.stock.clear();
    NeedList const noviceNeeds = Show("mage before the powder's spell, empty bags", novice, index, Rates(9));
    NeedList const adeptNeeds = ComputeNeeds(adept, index, Rates(12));
    NeedList const warriorNeeds = ComputeNeeds(warrior, index, Rates(30));
    Check(!ReagentKnown(index, novice, ITEM_POWDER) && !Find(noviceNeeds, NeedOrder::ReagentPoison) &&
              !StockErrandDue(noviceNeeds) &&
              !VendorCover(index, novice, noviceNeeds, StockBit(UpkeepStock::Reagent)).count(VENDOR_REAGENT),
          "a reagent of a spell the mage has not learned: no need, no errand, no vendor covers it");
    Check(ReagentKnown(index, adept, ITEM_POWDER) && Find(adeptNeeds, NeedOrder::ReagentPoison, UpkeepStock::Reagent),
          "the spell learned: the reagent need");
    Check(!Find(warriorNeeds, NeedOrder::ReagentPoison), "a class no spell of which uses the powder: no need");

    Snapshot hunter = Bot(index, CLASS_HUNTER, 30, 100 * GOLD, 30);
    SetStock(hunter, UpkeepStock::Ammo, 0);
    hunter.ammoDamage = 0;
    hunter.stock.push_back({ITEM_ARROW, 2000, StockBit(UpkeepStock::Ammo), 0, ARROWS, 1, DAMAGE_ARROW});
    NeedList const hunterNeeds =
        Show("hunter at 30 with 2000 level 1 arrows, no ammo equipped", hunter, index, Rates(30));
    Need const* ammo = Find(hunterNeeds, NeedOrder::Ammo);
    std::vector<ShopLine> const general = PlanShopping(hunterNeeds, index, hunter, VENDOR_GENERAL);
    std::vector<ShopLine> const arms = PlanShopping(hunterNeeds, index, hunter, VENDOR_ARMS);
    std::unordered_map<std::uint32_t, std::uint8_t> const ammoCover =
        VendorCover(index, hunter, hunterNeeds, StockBit(UpkeepStock::Ammo));
    std::cout << "   at the general goods vendor, then at the weapon vendor:\n";
    bool const within = CopperWithinFunded(general) && CopperWithinFunded(arms);
    Check(ammo && ammo->due && ammo->keptGrade == DAMAGE_RAZOR && ammo->item == ITEM_RAZOR_ARROW &&
              Stock(hunter, UpkeepStock::Ammo, ammo->keptGrade) == 0,
          "none equipped: the best offer's grade, the old arrows do not count, a due need of the new grade");
    Check(within && Line(general, UpkeepStock::Ammo) &&
              Line(general, UpkeepStock::Ammo)->outcome == ShopOutcome::NotSold && Line(arms, UpkeepStock::Ammo) &&
              Line(arms, UpkeepStock::Ammo)->outcome == ShopOutcome::Buy &&
              Line(arms, UpkeepStock::Ammo)->item == ITEM_RAZOR_ARROW && !ammoCover.count(VENDOR_GENERAL) &&
              ammoCover.count(VENDOR_ARMS),
          "the general vendor's old grade is not bought and does not cover the need; the weapon vendor's is");

    Snapshot rogue = Bot(index, CLASS_ROGUE, 30, 300, 30);
    SetStock(rogue, UpkeepStock::Poison, 0);
    rogue.stock.push_back({ITEM_INSTANT_1, 10, StockBit(UpkeepStock::Poison), 0, 0, 20});
    NeedList const rogueNeeds = Show("rogue at 30 with 10 rank 1 poisons, 3 silver", rogue, index, Rates(30));
    Need const* poison = Find(rogueNeeds, NeedOrder::ReagentPoison, UpkeepStock::Poison);
    std::unordered_map<std::uint32_t, std::uint8_t> const poisonCover =
        VendorCover(index, rogue, rogueNeeds, StockBit(UpkeepStock::Poison));
    std::vector<ShopLine> const stable = PlanShopping(rogueNeeds, index, rogue, VENDOR_STABLE);
    std::cout << "   at the stable (only the dearer lot of 5 for 6 silver):\n";
    CopperWithinFunded(stable);
    Check(poison && poison->item == ITEM_INSTANT_2 && poison->keptGrade == 28 && poison->lotPrice == 100 &&
              StockPayable(*poison) && poison->funded < 600,
          "rank 1 does not count beside the rank 2 grade; the gold pays the poison vendor's lot");
    Check(poisonCover.count(VENDOR_POISON) && !poisonCover.count(VENDOR_STABLE) && Line(stable, UpkeepStock::Poison) &&
              Line(stable, UpkeepStock::Poison)->outcome == ShopOutcome::Budget,
          "the stable's own lot is beyond the funded share: it covers nothing (a visit there would skip for budget)");
    rogue.stock.push_back({ITEM_INSTANT_2, 10, StockBit(UpkeepStock::Poison), 0, 0, 28});
    Check(!Find(ComputeNeeds(rogue, index, Rates(30)), NeedOrder::ReagentPoison, UpkeepStock::Poison),
          "10 of the kept grade: no poison need");
}

void EquippedAmmo(UpkeepIndex const& index)
{
    std::cout << "Scenario: ammo judged against the equipped ammo's damage (WU6 review 2 finding 1)\n";
    Snapshot hunter = Bot(index, CLASS_HUNTER, 76, 1000 * GOLD, 76);
    SetStock(hunter, UpkeepStock::Ammo, 0);
    hunter.ammoDamage = DAMAGE_SARONITE;
    hunter.stock.push_back({ITEM_SARONITE, 6000, StockBit(UpkeepStock::Ammo), 0, ARROWS, 72, DAMAGE_SARONITE});
    NeedList const needs =
        Show("hunter at 76 with 6000 equipped level 72 arrows (damage 67.5)", hunter, index, Rates(76));
    bool terrorshaftFits = false;
    for (VendorOffer const& offer : index.Offers(UpkeepStock::Ammo))
        terrorshaftFits =
            terrorshaftFits || (offer.item == ITEM_TERRORSHAFT && OfferFits(offer, hunter, UpkeepStock::Ammo));
    Check(!terrorshaftFits && !BestOffer(index, hunter, UpkeepStock::Ammo),
          "the vendor's level 75 arrows (damage 46.5) are worse than the equipped: no offer fits");
    Check(Stock(hunter, UpkeepStock::Ammo, KeptGrade(hunter, nullptr, UpkeepStock::Ammo)) == 6000 &&
              !Find(needs, NeedOrder::Ammo) && !StockErrandDue(needs) &&
              !VendorCover(index, hunter, needs, StockBit(UpkeepStock::Ammo)).count(VENDOR_ARMS) &&
              !Line(PlanShopping(needs, index, hunter, VENDOR_ARMS), UpkeepStock::Ammo),
          "the 6000 count; no ammo need, no errand, no cover, nothing to buy at the weapon vendor");
    SetStock(hunter, UpkeepStock::Ammo, 0);
    NeedList const empty = ComputeNeeds(hunter, index, Rates(76));
    Check(!Find(empty, NeedOrder::Ammo) && !StockErrandDue(empty),
          "the equipped arrows used up: still no need (the item usage would sell every offer)");

    Snapshot worse = Bot(index, CLASS_HUNTER, 76, 1000 * GOLD, 76);
    SetStock(worse, UpkeepStock::Ammo, 100);
    NeedList const worseNeeds =
        Show("hunter at 76 with 100 equipped level 1 arrows (damage 2.5)", worse, index, Rates(76));
    Need const* ammo = Find(worseNeeds, NeedOrder::Ammo);
    std::vector<ShopLine> const arms = PlanShopping(worseNeeds, index, worse, VENDOR_ARMS);
    std::cout << "   at the weapon vendor:\n";
    bool const within = CopperWithinFunded(arms);
    ShopLine const* line = Line(arms, UpkeepStock::Ammo);
    Check(ammo && ammo->due && ammo->item == ITEM_TERRORSHAFT && ammo->keptGrade == DAMAGE_ARROW &&
              Stock(worse, UpkeepStock::Ammo, ammo->keptGrade) == 100 &&
              VendorCover(index, worse, worseNeeds, StockBit(UpkeepStock::Ammo)).count(VENDOR_ARMS) && within && line &&
              line->outcome == ShopOutcome::Buy && line->item == ITEM_TERRORSHAFT && line->lots > 0,
          "worse ammo equipped: the due need buys the vendor's better arrows (the item usage equips them)");
    SetStock(worse, UpkeepStock::Ammo, 6000);
    Check(!Find(ComputeNeeds(worse, index, Rates(76)), NeedOrder::Ammo),
          "6000 of the worse equipped arrows count: no need, the better grade waits for a low stock");
}

void VendorChoice(UpkeepIndex const& index)
{
    std::cout << "Scenario: the vendor that covers the due needs\n";
    Snapshot hunter = Bot(index, CLASS_HUNTER, 10, 100 * GOLD, 10);
    SetStock(hunter, UpkeepStock::Ammo, 0);
    SetStock(hunter, UpkeepStock::Food, 0);
    NeedList const needs = ComputeNeeds(hunter, index, Rates(10));
    std::uint8_t const due = PayableKinds(needs, true);
    std::uint8_t const unmet = PayableKinds(needs, false);
    std::unordered_map<std::uint32_t, std::uint8_t> const cover = VendorCover(index, hunter, needs, due | unmet);
    auto const covers = [&cover](std::uint32_t entry)
    {
        auto const found = cover.find(entry);
        return found == cover.end() ? std::uint8_t(0) : found->second;
    };
    std::uint8_t const ammo = StockBit(UpkeepStock::Ammo);
    std::uint8_t const food = StockBit(UpkeepStock::Food);
    Check(due == (ammo | food) && covers(VENDOR_INN) == food && covers(VENDOR_ARMS) == ammo &&
              covers(VENDOR_GENERAL) == (ammo | food) && !covers(VENDOR_MOUNTS),
          "the cover: the inn food, the weapon vendor ammo, the general goods vendor both");
    constexpr float LIMIT = 10.0f;
    std::vector<VendorCandidate> near = {{VENDOR_INN, 2.0f, false, covers(VENDOR_INN)},
                                         {VENDOR_ARMS, 3.0f, true, covers(VENDOR_ARMS)},
                                         {VENDOR_GENERAL, 6.0f, false, covers(VENDOR_GENERAL)},
                                         {VENDOR_MOUNTS, 1.0f, false, covers(VENDOR_MOUNTS)}};
    auto const entry = [&near](std::size_t at) { return at == NO_VENDOR ? 0u : near[at].entry; };
    std::size_t const picked = PickVendor(near, false, due, unmet, LIMIT);
    std::cout << "    inn 2 min (food), weapons 3 min (ammo, repairs), general 6 min (both), mounts 1 min: "
              << entry(picked) << "\n";
    Check(entry(picked) == VENDOR_GENERAL, "the vendor covering both due needs wins over two nearer ones");
    Check(entry(PickVendor(near, true, due, unmet, LIMIT)) == VENDOR_ARMS,
          "a repair trip keeps to repairers (WU5), whatever the others cover");
    near[2].walkMinutes = 12.0f;
    Check(entry(PickVendor(near, false, due, unmet, LIMIT)) == VENDOR_INN,
          "the general vendor beyond the walk limit: a tie of one due need each goes to the nearest");
    Check(entry(PickVendor(near, false, 0, 0, std::numeric_limits<float>::max())) == VENDOR_MOUNTS,
          "no needs and no limit: the nearest vendor (the bags' errand)");
    std::vector<VendorCandidate> const far = {{VENDOR_INN, 11.0f, false, food}};
    Check(PickVendor(far, false, due, unmet, LIMIT) == NO_VENDOR, "nothing within the walk limit: no vendor");
}

void UseMeasure()
{
    std::cout << "Scenario: the use per hour from stock falls between visits\n";
    WorldTravel::SlotMeasure measure;
    StockLevels levels;
    levels.basis.fill(1);
    std::size_t const ammo = static_cast<std::size_t>(UpkeepStock::Ammo);
    std::size_t const food = static_cast<std::size_t>(UpkeepStock::Food);
    std::size_t const water = static_cast<std::size_t>(UpkeepStock::Water);
    std::size_t const petFood = static_cast<std::size_t>(UpkeepStock::PetFood);
    std::uint32_t const start = 0xFFFFFFFFu - 60000u;  // the millisecond clock wraps during the test
    constexpr std::uint32_t MINUTE_MS = 60000;
    levels.count[ammo] = 1000;
    levels.count[food] = 20;
    levels.count[water] = 20;
    levels.count[petFood] = 40;
    levels.basis[petFood] = MEAT_DIET;
    SampleUse(measure, levels, CLASS_HUNTER, 10, start);
    Check(measure.UsePerHour[ammo] == 0.0f, "no sample before the first visit's end");
    NoteStock(measure, levels, start);
    StockLevels later = levels;
    later.count[ammo] = 900;
    SampleUse(measure, later, CLASS_HUNTER, 10, start + 5 * MINUTE_MS);
    Check(measure.UsePerHour[ammo] == 0.0f, "5 minutes apart: no sample (window 10 min)");
    later.count[ammo] = 700;
    later.count[food] = 25;    // looted
    later.count[water] = 18;   // a mage would not count this
    later.count[petFood] = 0;  // the pet out of sight (mounted): no diet, no pet food counted
    later.basis[petFood] = 0;
    SampleUse(measure, later, CLASS_HUNTER, 10, start + 30 * MINUTE_MS);
    std::cout << "    after 30 min: ammo " << measure.UsePerHour[ammo] << "/h, food " << measure.UsePerHour[food]
              << "/h, water " << measure.UsePerHour[water] << "/h\n";
    Check(measure.UsePerHour[ammo] > 599.0f && measure.UsePerHour[ammo] < 601.0f,
          "300 arrows in 30 minutes across the clock wrap: 600 per hour");
    Check(measure.UsePerHour[food] == 0.0f && measure.UsePerHour[water] > 3.9f && measure.UsePerHour[water] < 4.1f,
          "the looted food gives no sample; 2 water in 30 minutes: 4 per hour");
    Check(measure.UsePerHour[petFood] == 0.0f, "the pet out of sight reads no pet food: no sample (basis 0)");
    NoteStock(measure, later, start + 30 * MINUTE_MS);
    StockLevels hour = later;
    hour.count[ammo] = 500;
    hour.count[petFood] = 30;
    hour.basis[petFood] = MEAT_DIET;
    SampleUse(measure, hour, CLASS_HUNTER, 10, start + 90 * MINUTE_MS);
    Check(measure.UsePerHour[ammo] > 399.0f && measure.UsePerHour[ammo] < 401.0f,
          "a second sample (200 per hour) averages with the first: 400 per hour");
    Check(measure.UsePerHour[petFood] == 0.0f, "the pet back: a rise from the noted 0 gives no sample either");
    NoteStock(measure, hour, start + 90 * MINUTE_MS);
    StockLevels gun = hour;
    gun.count[ammo] = 0;  // a gun now: the arrows no longer count
    gun.basis[ammo] = static_cast<std::uint32_t>(UpkeepItem::SUBCLASS_BULLET) | 1u << 8;
    SampleUse(measure, gun, CLASS_HUNTER, 10, start + 120 * MINUTE_MS);
    Check(measure.UsePerHour[ammo] > 399.0f && measure.UsePerHour[ammo] < 401.0f,
          "the ranged weapon changed type: the arrows' fall to 0 gives no sample");
    WorldTravel::SlotMeasure mage;
    NoteStock(mage, levels, start);
    SampleUse(mage, later, CLASS_MAGE, 10, start + 30 * MINUTE_MS);
    Check(mage.UsePerHour[water] == 0.0f && mage.UsePerHour[ammo] == 0.0f,
          "a mage's water and ammo are not measured (no default use)");
    Snapshot bot;
    bot.cls = CLASS_HUNTER;
    bot.level = 10;
    bot.ammoSubClass = static_cast<std::uint8_t>(UpkeepItem::SUBCLASS_ARROW);
    NeedRates rates;
    std::uint32_t const byDefault = StockTarget(bot, rates, UpkeepStock::Ammo);
    FillUse(rates, measure);
    std::uint32_t const byMeasure = StockTarget(bot, rates, UpkeepStock::Ammo);
    std::cout << "    ammo target: " << byDefault << " by default, " << byMeasure << " measured\n";
    Check(byDefault == 600 && byMeasure == 1200, "the measured use replaces the default in the target (3 h)");
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

// Due training makes a trip only when the funded gold pays the cheapest waiting spell (WU13: RepairErrandDue's rule)
void TrainingGold(UpkeepIndex const& index)
{
    std::cout << "Scenario: the gold test of due training\n";
    // Mage level 12, trained to 9: 2010 (2000), 2012 (2400) and rank 2 (2100, 900) wait, and are due
    Snapshot poor = Bot(index, CLASS_MAGE, 12, 800, 9);
    NeedList const list = ComputeNeeds(poor, index, Rates(12));
    Need const* need = Find(list, NeedOrder::Training);
    Check(need && need->due && need->cheapest == 2000 && need->funded == 800,
          "the need is due, its cheapest buyable spell costs 2000 (rank 2 at 900 needs 2010) and the gold pays 800");
    Check(need && !TrainingErrandDue(*need) && !AnyErrandDue(list), "gold below the cheapest spell: no errand");
    Snapshot rankOnly = Bot(index, CLASS_MAGE, 12, 900, 9);
    NeedList const behind = ComputeNeeds(rankOnly, index, Rates(12));
    need = Find(behind, NeedOrder::Training);
    Check(need && !TrainingErrandDue(*need) && !AnyErrandDue(behind),
          "gold for rank 2 only: no errand (the visit would buy nothing, so the trip would repeat)");
    Snapshot paying = Bot(index, CLASS_MAGE, 12, 2000, 9);
    NeedList const payable = ComputeNeeds(paying, index, Rates(12));
    need = Find(payable, NeedOrder::Training);
    Check(need && need->cheapest == 2000 && TrainingErrandDue(*need) && AnyErrandDue(payable),
          "gold for the first rank: the errand is due");
    Snapshot priest = Bot(index, CLASS_PRIEST, 58, 6500, 51);
    NeedList const priestPoor = ComputeNeeds(priest, index, Rates(58));
    need = Find(priestPoor, NeedOrder::Training);
    Check(need && need->due && need->cheapest == 42000 && !TrainingErrandDue(*need),
          "priest 25315 (6500) needs 10929 (42000): 6500 gold buys no trip");
    priest.money = 42000;
    NeedList const priestRich = ComputeNeeds(priest, index, Rates(58));
    need = Find(priestRich, NeedOrder::Training);
    Check(need && TrainingErrandDue(*need), "42000 pays the prerequisite: the trip is due");
    Snapshot close = Bot(index, CLASS_MAGE, 11, 100 * GOLD, 9);
    NeedList const notDue = ComputeNeeds(close, index, Rates(11));
    need = Find(notDue, NeedOrder::Training);
    Check(need && !need->due && need->cheapest > 0 && !TrainingErrandDue(*need) && !AnyErrandDue(notDue),
          "rich but not due: no errand of its own");
}

// What a visit buys: level order within the gold, at the game's price, a skipped spell stays a need
void TrainingBuying(UpkeepIndex const& index)
{
    std::cout << "Scenario: a trainer visit's purchase\n";
    Snapshot bot = Bot(index, CLASS_MAGE, 12, 0, 9);
    TrainerList const& list = *index.ClassList(CLASS_MAGE);
    std::vector<std::size_t> const learnable =
        Learnable(list, index.RidingList(bot.race), bot, bot.knownClassSpells, bot.canLearnClassSpells, 12);
    auto const spells = [&list](TrainingBuy const& buy)
    {
        std::string text;
        for (std::size_t const position : buy.positions)
            text += std::to_string(list.spells[position].spellId) + " ";
        return text;
    };
    Check(learnable.size() == 3, "three spells wait: 2010, 2012 and the second rank 2100");

    TrainingBuy buy = TrainingPurchase(list, learnable, 12, 100 * GOLD, 1.0f);
    Check(buy.positions.size() == 3 && buy.total == 5300 && spells(buy) == "2010 2012 2100 ",
          "rich: all three, in level order, at full price: " + spells(buy));

    buy = TrainingPurchase(list, learnable, 12, 2900, 1.0f);
    Check(
        spells(buy) == "2010 2100 " && buy.total == 2900 && learnable.size() - buy.positions.size() == 1,
        "gold for two of three: 2010 and rank 2 (2012 does not fit and is skipped), 2012 stays a need: " + spells(buy));

    buy = TrainingPurchase(list, learnable, 12, 1000, 1.0f);
    Check(buy.positions.empty() && buy.total == 0,
          "gold for rank 2 only: it is not bought without its first rank (the core would refuse it)");

    buy = TrainingPurchase(list, learnable, 12, 1500, 0.5f);
    Check(spells(buy) == "2010 2100 " && buy.total == 1450,
          "a reputation discount of half: 1000 + 450 fits 1500 where full price buys nothing: " + spells(buy));
    Check(TrainerPrice(901, 0.5f) == 450 && TrainerPrice(900, 1.0f) == 900,
          "the price rounds down, as the core's int32");

    buy = TrainingPurchase(list, learnable, 10, 100 * GOLD, 1.0f);
    Check(spells(buy) == "2010 ", "a spell above the bot's level is not bought");
    buy = TrainingPurchase(list, {}, 12, 100 * GOLD, 1.0f);
    Check(buy.positions.empty() && buy.total == 0, "nothing learnable: nothing bought");
}

void RidingFunded(UpkeepIndex const& index)
{
    std::cout << "Scenario: a riding rank bought against the scaled reserve but funded short is no errand\n";
    // A spendthrift's reserve is scaled down, Fund pays the unscaled needs first (a stock need that is not due
    // included): between the two the rank shows BUY with a funded share below its price
    std::uint64_t shortAt = 0;
    std::uint64_t paidAt = 0;
    bool consistent = true;
    for (std::uint64_t copper = 0; copper <= 300 * GOLD; copper += 50)
    {
        Snapshot bot = Bot(index, CLASS_WARRIOR, 40, static_cast<std::uint32_t>(copper), 40, {SPELL_APPRENTICE});
        bot.traits = Traits(TraitRules::MIN_VALUE, 0, false);
        SetStock(bot, UpkeepStock::Food, StockTarget(bot, NeedRates{}, UpkeepStock::Food) / 2);
        NeedList const list = ComputeNeeds(bot, index, Rates(40, 50.0f));
        Need const* riding = Find(list, NeedOrder::Riding);
        if (!riding)
            continue;
        bool const stockDue = StockErrandDue(list);
        bool const due = RidingErrandDue(*riding);
        consistent = consistent && due == (riding->funded >= riding->copper) && (stockDue || AnyErrandDue(list) == due);
        if (!due && !shortAt)
            shortAt = copper;
        if (due && !paidAt)
            paidAt = copper;
    }
    std::cout << "    shows BUY but funded short from " << Money(shortAt) << ", errand due from " << Money(paidAt)
              << "\n";
    Check(shortAt && paidAt > shortAt && consistent,
          "the rank shows BUY with a funded share below its price: not an errand; once funded whole it is");
}

int main()
{
    std::cout << "Tuning [E]:\n";
    for (std::string const& line : TuningLines())
        std::cout << "  " << line << "\n";
    std::cout << "  riding horizon: " << RidingHorizonHours(20) << " h at 20, " << RidingHorizonHours(40)
              << " h at 40, " << RidingHorizonHours(70) << " h at 70, " << RidingHorizonHours(80) << " h at 80\n";
    UpkeepIndex const index = Fixture();
    RepairBeforeFood(index);
    RepairDueRule();
    PartialRepair(index);
    AmmoAndWater(index);
    PetDiet(index);
    Training(index);
    TrainingGold(index);
    TrainingBuying(index);
    GroundRiding(index);
    FlyingRiding(index);
    MeasuredRiding(index);
    MountMissing(index);
    ReserveAndShortfall(index);
    ClassMounts(index);
    LearnSpellMount(index);
    WantNoFlip(index);
    RidingFunded(index);
    FrugalBuy(index);
    StaleBits(index);
    AmmoType(index);
    KeptItems(index);
    EquippedAmmo(index);
    StockErrand(index);
    ShoppingOrder(index);
    VendorChoice(index);
    UseMeasure();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
