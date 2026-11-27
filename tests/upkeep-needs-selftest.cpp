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
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <random>

#include "NeedGoalBridge.h"
#include "upkeep-fixture.h"

namespace
{

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

// Professions WR15 (PLAN 3.11): the food a cook's path cooks from its own meat counts as carried
void CookedFood(UpkeepIndex const& index)
{
    std::cout << "Scenario: a cook with raw meat and no food\n";
    Snapshot bot = Bot(index, CLASS_WARRIOR, 30, 5000, 30);
    SetStock(bot, UpkeepStock::Food, 0);
    NeedRates rates = Rates(30);
    NeedList const none = Show("warrior, no food", bot, index, rates);
    Need const* hungry = Find(none, NeedOrder::FoodWater, UpkeepStock::Food);
    std::uint32_t const target = StockTarget(bot, rates, UpkeepStock::Food);
    Check(hungry && hungry->units == target, "without a path the food need is its whole target");
    rates.pathFood = target;
    NeedList const cooked = Show("warrior, no food, its path cooks the target", bot, index, rates);
    Check(!Find(cooked, NeedOrder::FoodWater, UpkeepStock::Food), "no food need while the cook makes its target");
    rates.pathFood = target / 2;
    NeedList const half = Show("warrior, no food, its path cooks half the target", bot, index, rates);
    Need const* rest = Find(half, NeedOrder::FoodWater, UpkeepStock::Food);
    Check(rest && rest->units == target - target / 2 && !rest->due,
          "the rest of the target is a need; not due while the cooked half covers the low share");
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

// Where the rank and its mount are bought (WU26c): the trainer that teaches the rank, and any vendor that sells a
// fitting mount at the price (each race's riding trainer and mount vendor are their own)
void RidingPlaces(UpkeepIndex const& index)
{
    std::cout << "Scenario: riding trainer and mount vendor\n";
    Check(TrainerFor(index, TrainerKind::Mount, 0, SPELL_EXPERT) == TRAINER_RIDING &&
              !TrainerFor(index, TrainerKind::Mount, 0, SPELL_FELSTEED) &&
              TrainerFor(index, TrainerKind::Mount, 0) == TRAINER_RIDING,
          "the riding trainer that teaches the rank; none for a spell no riding trainer teaches");
    NeedList const at20 = ComputeNeeds(Bot(index, CLASS_WARRIOR, 20, 100 * GOLD, 20), index, Rates(20));
    RankValue const* apprentice = BoughtRank(at20, SPELL_APPRENTICE);
    Check(apprentice && MountSoldBy(index, RACE_HUMAN, CLASS_WARRIOR, *apprentice, VENDOR_MOUNTS) == ITEM_HORSE &&
              !MountSoldBy(index, RACE_HUMAN, CLASS_WARRIOR, *apprentice, VENDOR_INN) &&
              !BoughtRank(at20, SPELL_JOURNEYMAN),
          "the vendor's own mount for the bought rank; none at a vendor without one");
}

void TrainingPlaces(UpkeepIndex const& index)
{
    std::cout << "Scenario: the class trainer that teaches the waiting spells\n";
    Check(TrainerFor(index, TrainerKind::Class, CLASS_WARRIOR) == TRAINER_WARRIOR_STARTER,
          "the fixture's first warrior trainer is the starter one");
    NeedList const level24 = ComputeNeeds(Bot(index, CLASS_WARRIOR, 24, 100 * GOLD, 20), index, Rates(24));
    Need const* at24 = Find(level24, NeedOrder::Training);
    Check(at24 && at24->lastSpell == 1024 && at24->trainer == TRAINER_WARRIOR,
          "a level-24 need goes to the trainer that teaches its level-24 spell, not the starter trainer");
    NeedList const level6 = ComputeNeeds(Bot(index, CLASS_WARRIOR, 6, 100 * GOLD, 2), index, Rates(6));
    Need const* at6 = Find(level6, NeedOrder::Training);
    Check(at6 && at6->lastSpell == 1006 && at6->trainer == TRAINER_WARRIOR_STARTER,
          "a level-6 need may go to the starter trainer, which teaches it");

    // acore_world's mage trainers (WU26c round 5): trainer 16 teaches 2121, 120 and 6127 at 24-30, the Darnassus
    // portal trainer 21 Teleport: Darnassus 3565 at 30, the level-30 spell with the lowest id
    constexpr std::uint32_t TRAINER_PORTAL = 105;
    UpkeepGather input;
    input.trainers.push_back({TRAINER_PORTAL, TrainerKind::Class, CLASS_MAGE, {Spell(3565, 8000, 30)}});
    input.trainers.push_back(
        {TRAINER_MAGE,
         TrainerKind::Class,
         CLASS_MAGE,
         {Spell(1953, 2000, 20), Spell(2121, 4000, 24), Spell(120, 5000, 26), Spell(6127, 8000, 30)}});
    input.trainers.push_back({TRAINER_RIDING, TrainerKind::Mount, 0, {RidingSpell(SPELL_APPRENTICE, 4 * GOLD, 20, 0)}});
    input.trainerEntries = {
        {TRAINER_PORTAL, TRAINER_PORTAL}, {TRAINER_MAGE, TRAINER_MAGE}, {TRAINER_RIDING, TRAINER_RIDING}};
    UpkeepIndex portals = UpkeepIndex::Build(std::move(input));
    portals.version = 7;
    NeedList const untrained = ComputeNeeds(Bot(portals, CLASS_MAGE, 30, 50 * GOLD, 22), portals, Rates(30));
    Need const* mage = Find(untrained, NeedOrder::Training);
    Check(mage && mage->trainer == TRAINER_MAGE && mage->lastSpell == 6127 && mage->units == 3 &&
              mage->copper == 17000 && mage->levelsBehind == 6 && mage->due && TrainingErrandDue(*mage),
          "a mage's need goes to the class trainer and counts its spells, not the portal trainer's teleport");
    Snapshot trained = Bot(portals, CLASS_MAGE, 30, 50 * GOLD, 22);
    for (std::uint32_t const spell : {2121u, 120u, 6127u})
        Mark(trained.knownClassSpells, *portals.ClassList(CLASS_MAGE), spell);
    NeedList const teleport = ComputeNeeds(trained, portals, Rates(30));
    Need const* portal = Find(teleport, NeedOrder::Training);
    Check(portal && portal->trainer == TRAINER_PORTAL && portal->lastSpell == 3565 && portal->units == 1,
          "after the class trainer's visit the teleport alone is a need at the portal trainer");
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
                         static_cast<std::uint8_t>(UpkeepItem::SUBCLASS_BULLET), DAMAGE_BULLET});
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
    Check(!ReagentKnown(novice, ITEM_POWDER) && !Find(noviceNeeds, NeedOrder::ReagentPoison) &&
              !StockErrandDue(noviceNeeds) &&
              !VendorCover(index, novice, noviceNeeds, StockBit(UpkeepStock::Reagent)).count(VENDOR_REAGENT),
          "a reagent of a spell the mage has not learned: no need, no errand, no vendor covers it");
    Check(ReagentKnown(adept, ITEM_POWDER) && Find(adeptNeeds, NeedOrder::ReagentPoison, UpkeepStock::Reagent),
          "the spell learned: the reagent need");
    Snapshot glyphed = adept;
    glyphed.knownReagents.clear();  // the live reading drops a spell cast without its reagent
    NeedList const glyphedNeeds = ComputeNeeds(glyphed, index, Rates(12));
    Check(!ReagentKnown(glyphed, ITEM_POWDER) && !Find(glyphedNeeds, NeedOrder::ReagentPoison) &&
              !StockErrandDue(glyphedNeeds),
          "the spell cast without its reagent (a glyph): no need, no errand (WU26a-fix)");
    Check(!Find(warriorNeeds, NeedOrder::ReagentPoison), "a class no spell of which uses the powder: no need");

    // A warlock's reagent of a quest's spell (on no trainer list): the generic line leaves it to the errand visit,
    // which lists it. The line reads the bot's spells only, so the index being ready or not changes nothing
    Snapshot warlock = Bot(index, CLASS_WARLOCK, 60, 100 * GOLD, 60);
    warlock.stock.clear();
    warlock.knownReagents = {ITEM_FIGURINE};
    for (bool const ready : {true, false})
    {
        Snapshot at = warlock;
        at.upkeepVersion = ready ? index.version : 0;
        Check(!GenericBuysSkillItem(true, at.knownReagents, ITEM_FIGURINE) &&
                  GenericBuysSkillItem(true, at.knownReagents, ITEM_POWDER) &&
                  GenericBuysSkillItem(false, at.knownReagents, ITEM_FIGURINE),
              "the generic line buys no reagent of a spell the bot knows, index ready or not");
    }
    NeedList const warlockNeeds = ComputeNeeds(warlock, index, Rates(60));
    std::vector<ShopLine> const warlockLines = PlanShopping(warlockNeeds, index, warlock, VENDOR_REAGENT);
    Check(Find(warlockNeeds, NeedOrder::ReagentPoison, UpkeepStock::Reagent) &&
              VendorCover(index, warlock, warlockNeeds, StockBit(UpkeepStock::Reagent)).count(VENDOR_REAGENT) &&
              Line(warlockLines, UpkeepStock::Reagent) &&
              Line(warlockLines, UpkeepStock::Reagent)->item == ITEM_FIGURINE,
          "the quest spell's reagent is on the visit's list");

    Snapshot hunter = Bot(index, CLASS_HUNTER, 30, 100 * GOLD, 30);
    SetStock(hunter, UpkeepStock::Ammo, 0);
    hunter.ammoDamage = 0;
    hunter.stock.push_back({ITEM_ARROW, 2000, StockBit(UpkeepStock::Ammo), 0, ARROWS, DAMAGE_ARROW});
    NeedList const hunterNeeds =
        Show("hunter at 30 with 2000 level 1 arrows, no ammo equipped", hunter, index, Rates(30));
    Need const* ammo = Find(hunterNeeds, NeedOrder::Ammo);
    std::vector<ShopLine> const general = PlanShopping(hunterNeeds, index, hunter, VENDOR_GENERAL);
    std::vector<ShopLine> const arms = PlanShopping(hunterNeeds, index, hunter, VENDOR_ARMS);
    std::unordered_map<std::uint32_t, StockMask> const ammoCover =
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

    // Demand does not answer this rogue (no stock want, as a granted kind): the best offer by level, no grade (WU37b)
    Snapshot rogue = Bot(index, CLASS_ROGUE, 30, 300, 30);
    SetStock(rogue, UpkeepStock::Poison, 0);
    NeedList const rogueNeeds =
        Show("rogue at 30 without poison, 3 silver, demand not answering", rogue, index, Rates(30));
    Need const* poison = Find(rogueNeeds, NeedOrder::ReagentPoison, UpkeepStock::Poison);
    std::unordered_map<std::uint32_t, StockMask> const poisonCover =
        VendorCover(index, rogue, rogueNeeds, StockBit(UpkeepStock::Poison));
    std::vector<ShopLine> const stable = PlanShopping(rogueNeeds, index, rogue, VENDOR_STABLE);
    std::cout << "   at the stable (only the dearer lot of 5 for 6 silver):\n";
    CopperWithinFunded(stable);
    Check(poison && poison->item == ITEM_INSTANT_2 && poison->keptGrade == 0 && poison->lotPrice == 100 &&
              StockPayable(*poison) && poison->funded < 600,
          "the best offer by level, no kept grade; the gold pays the poison vendor's lot");
    Check(poisonCover.count(VENDOR_POISON) && !poisonCover.count(VENDOR_STABLE) && Line(stable, UpkeepStock::Poison) &&
              Line(stable, UpkeepStock::Poison)->outcome == ShopOutcome::Budget,
          "the stable's own lot is beyond the funded share: it covers nothing (a visit there would skip for budget)");
    rogue.stock.push_back({ITEM_INSTANT_1, 10, StockBit(UpkeepStock::Poison)});
    Check(!Find(ComputeNeeds(rogue, index, Rates(30)), NeedOrder::ReagentPoison, UpkeepStock::Poison),
          "10 of a lower rank count (no poison grade): no poison need");
}

void EquippedAmmo(UpkeepIndex const& index)
{
    std::cout << "Scenario: ammo judged against the equipped ammo's damage (WU6 review 2 finding 1)\n";
    Snapshot hunter = Bot(index, CLASS_HUNTER, 76, 1000 * GOLD, 76);
    SetStock(hunter, UpkeepStock::Ammo, 0);
    hunter.ammoDamage = DAMAGE_SARONITE;
    hunter.stock.push_back({ITEM_SARONITE, 6000, StockBit(UpkeepStock::Ammo), 0, ARROWS, DAMAGE_SARONITE});
    NeedList const needs =
        Show("hunter at 76 with 6000 equipped level 72 arrows (damage 67.5)", hunter, index, Rates(76));
    bool terrorshaftFits = false;
    for (VendorOffer const& offer : index.Offers(UpkeepStock::Ammo))
        terrorshaftFits =
            terrorshaftFits || (offer.item == ITEM_TERRORSHAFT && OfferFits(offer, hunter, UpkeepStock::Ammo));
    Check(!terrorshaftFits && !BestOffer(index, hunter, UpkeepStock::Ammo),
          "the vendor's level 75 arrows (damage 46.5) are worse than the equipped: no offer fits");
    Check(Stock(hunter, UpkeepStock::Ammo, KeptGrade(&index, hunter)) == 6000 && !Find(needs, NeedOrder::Ammo) &&
              !StockErrandDue(needs) &&
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
    StockMask const due = PayableKinds(needs, true);
    StockMask const unmet = PayableKinds(needs, false);
    std::unordered_map<std::uint32_t, StockMask> const cover =
        VendorCover(index, hunter, needs, static_cast<StockMask>(due | unmet));
    auto const covers = [&cover](std::uint32_t entry)
    {
        auto const found = cover.find(entry);
        return found == cover.end() ? StockMask(0) : found->second;
    };
    StockMask const ammo = StockBit(UpkeepStock::Ammo);
    StockMask const food = StockBit(UpkeepStock::Food);
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

// Weapon skills (WU15a): a weapon the bot carries whose skill it lacks and its class may learn, at the weapon master
void WeaponMasters()
{
    std::cout << "Scenario: weapon skills at the weapon master\n";
    UpkeepIndex const index = WeaponFixture();
    auto const weaponNeed = [](NeedList const& list) -> Need const*
    {
        for (Need const& need : list.needs)
            if (need.kind == UpkeepKind::WeaponSkill)
                return &need;
        return nullptr;
    };

    Snapshot warrior = Bot(index, CLASS_WARRIOR, 15, 20 * GOLD, 15);
    Carries(warrior, SKILL_MACES);
    NeedList const mace = ComputeNeeds(warrior, index, Rates(15));
    Need const* need = weaponNeed(mace);
    Check(need && need->order == NeedOrder::Training && need->item == SPELL_MACES &&
              need->trainer == TRAINER_WEAPON_MASTER && need->copper == WEAPON_PRICE && need->funded >= need->copper &&
              need->due && WeaponErrandDue(*need) && AnyErrandDue(mace),
          "a warrior carrying a mace without Maces trips to its weapon master");
    Check(mace.reserveTraining >= WEAPON_PRICE && mace.reserve >= WEAPON_PRICE,
          "the skill's price joins the training reserve");

    Snapshot poor = warrior;
    poor.money = 500;
    NeedList const shortGold = ComputeNeeds(poor, index, Rates(15));
    Need const* unpaid = weaponNeed(shortGold);
    Check(unpaid && unpaid->funded < unpaid->copper && !WeaponErrandDue(*unpaid) && !AnyErrandDue(shortGold),
          "with 500 copper the need stays and makes no trip");

    Snapshot mage = Bot(index, CLASS_MAGE, 15, 20 * GOLD, 15);
    Carries(mage, SKILL_MACES);
    Check(!weaponNeed(ComputeNeeds(mage, index, Rates(15))),
          "a mage carrying a mace has no need: its class never learns");
    Carries(mage, SKILL_DAGGERS);
    Need const* dagger = nullptr;
    NeedList const daggers = ComputeNeeds(mage, index, Rates(15));
    dagger = weaponNeed(daggers);
    Check(dagger && dagger->item == SPELL_DAGGERS && dagger->trainer == TRAINER_WEAPON_MASTER,
          "a mage carrying a dagger without Daggers has the need");

    Snapshot unarmed = Bot(index, CLASS_WARRIOR, 15, 20 * GOLD, 15);
    Check(!weaponNeed(ComputeNeeds(unarmed, index, Rates(15))), "a bot without a weapon it cannot use has no need");
    Carries(unarmed, SKILL_MACES, true);
    Check(!weaponNeed(ComputeNeeds(unarmed, index, Rates(15))), "a bot that knows the skill has no need");
    Snapshot smith = Bot(index, CLASS_WARRIOR, 15, 20 * GOLD, 15);
    Carries(smith, SKILL_BLACKSMITHING);
    Check(!weaponNeed(ComputeNeeds(smith, index, Rates(15))),
          "a profession spell on the merged list is no weapon need");
}

// Profession needs (WR4a): learn, rank and tools of the person's professions, at the tradeskill trainers
void ProfessionNeeds()
{
    std::cout << "Scenario: profession needs (learn, rank, tools)\n";
    using ProfessionRules::Skill;
    constexpr std::uint32_t TRAINER_MINING = 501;
    constexpr std::uint32_t TRAINER_SMITH = 502;
    constexpr std::uint32_t MINING_APPRENTICE = 2575;
    constexpr std::uint32_t MINING_JOURNEYMAN = 2576;
    constexpr std::uint32_t SMITH_APPRENTICE = 2018;
    constexpr std::uint32_t SMITH_JOURNEYMAN = 3100;
    constexpr std::uint32_t SMITH_RECIPE = 2663;       // a recipe the smith trainer teaches (WR11)
    constexpr std::uint32_t SMITH_ITEM_RECIPE = 2667;  // a recipe only a recipe item teaches
    constexpr std::uint32_t SMITH_SOLD_RECIPE = 2668;  // a recipe whose item a vendor sells (WR14b)
    constexpr std::uint32_t SMITH_PLANS = 2881;        // its recipe item, at the smith's vendor
    constexpr std::uint32_t MINING_PICK = 2901;
    constexpr std::uint16_t MINING = 186;
    constexpr std::uint16_t SMITHING = 164;
    // A real trainer lists a rank as the learn spell that teaches it: Apprentice Cooking 2550 is sold as 2551
    constexpr std::uint32_t TRAINER_COOK = 503;
    constexpr std::uint32_t COOKING_APPRENTICE = 2550;
    constexpr std::uint32_t COOKING_SOLD = 2551;
    constexpr std::uint32_t FISHING_POLE = 6256;
    constexpr std::uint16_t COOKING = 185;
    constexpr std::uint16_t FISHING = 356;
    // Tool sellers (WR5b): the supplier sells the pick and the pole, the smith's vendor the pick; nobody the hammer
    constexpr std::uint32_t VENDOR_SUPPLIER = 601;
    constexpr std::uint32_t VENDOR_SMITH = 602;
    constexpr std::uint32_t VENDOR_INN = 603;
    constexpr std::uint32_t ITEM_BREAD = 604;
    // The jeweler's vendor sells its kit and grinder, which have no tool category (NR20: their TOOLS rows make them
    // tools)
    constexpr std::uint32_t VENDOR_JEWEL = 605;
    constexpr std::uint32_t JEWELER_KIT = 20815;
    constexpr std::uint32_t SIMPLE_GRINDER = 20824;
    constexpr std::uint16_t JEWELCRAFTING = 755;
    constexpr std::uint32_t WEAPON = 2;  // ITEM_CLASS_WEAPON
    UpkeepGather input;
    UpkeepItemFacts pickFacts = Item(MINING_PICK, WEAPON, 14, 90, 1, 1);
    pickFacts.tool = true;
    UpkeepItemFacts poleFacts = Item(FISHING_POLE, WEAPON, 20, 23, 1, 0);
    poleFacts.tool = true;
    UpkeepItemFacts bread = Item(ITEM_BREAD, UpkeepItem::CLASS_CONSUMABLE, UpkeepItem::SUBCLASS_FOOD, 25, 1, 1);
    bread.spellCategory = UpkeepItem::CATEGORY_FOOD;
    UpkeepItemFacts plansFacts = Item(SMITH_PLANS, 9, 4, 400, 1, 1);  // ITEM_CLASS_RECIPE, blacksmithing
    plansFacts.tool = true;
    UpkeepItemFacts kitFacts = Item(JEWELER_KIT, 7, 0, 60, 1, 1);  // ITEM_CLASS_TRADE_GOODS
    kitFacts.tool = true;
    UpkeepItemFacts grinderFacts = Item(SIMPLE_GRINDER, 7, 0, 40, 1, 1);
    grinderFacts.tool = true;
    input.items = {pickFacts, poleFacts, bread, plansFacts, kitFacts, grinderFacts};
    input.vendorItems = {{VENDOR_SUPPLIER, MINING_PICK}, {VENDOR_SUPPLIER, FISHING_POLE}, {VENDOR_SMITH, MINING_PICK},
                         {VENDOR_SMITH, SMITH_PLANS},    {VENDOR_INN, ITEM_BREAD},        {VENDOR_JEWEL, JEWELER_KIT},
                         {VENDOR_JEWEL, SIMPLE_GRINDER}};
    input.trainers.push_back({TRAINER_MINING,
                              TrainerKind::Tradeskill,
                              0,
                              {Spell(MINING_APPRENTICE, 10, 5), Spell(MINING_JOURNEYMAN, 100, 10)}});
    input.trainers.push_back(
        {TRAINER_SMITH,
         TrainerKind::Tradeskill,
         0,
         {Spell(SMITH_APPRENTICE, 5 * GOLD, 5), Spell(SMITH_JOURNEYMAN, 500, 10), Spell(SMITH_RECIPE, 300, 5)}});
    TrainerSpell cookingSold = Spell(COOKING_SOLD, 100, 0);
    cookingSold.teaches[0] = COOKING_APPRENTICE;
    input.trainers.push_back({TRAINER_COOK, TrainerKind::Tradeskill, 0, {cookingSold}});
    input.trainers.push_back({TRAINER_WARRIOR, TrainerKind::Class, CLASS_WARRIOR, {Spell(1002, 200, 2)}});
    input.trainers.push_back({TRAINER_RIDING, TrainerKind::Mount, 0, {RidingSpell(SPELL_APPRENTICE, 4 * GOLD, 20, 0)}});
    input.trainerEntries = {{TRAINER_MINING, TRAINER_MINING},
                            {TRAINER_SMITH, TRAINER_SMITH},
                            {TRAINER_COOK, TRAINER_COOK},
                            {TRAINER_WARRIOR, TRAINER_WARRIOR},
                            {TRAINER_RIDING, TRAINER_RIDING}};
    UpkeepIndex index = UpkeepIndex::Build(std::move(input));
    index.version = 7;
    Check(index.TradeskillTrainer(MINING_JOURNEYMAN) == TRAINER_MINING &&
              index.TradeskillTrainer(SMITH_APPRENTICE) == TRAINER_SMITH && index.TradeskillTrainer(1) == 0,
          "the index finds the tradeskill trainer of a rank spell");
    Check(index.TradeskillSpell(COOKING_APPRENTICE) == COOKING_SOLD &&
              index.TradeskillSpell(SMITH_APPRENTICE) == SMITH_APPRENTICE && index.TradeskillSpell(1) == 0 &&
              index.TradeskillTrainer(COOKING_APPRENTICE) == 0,
          "a rank the trainer sells as a learn spell is found by the spell it teaches (2550 as 2551)");

    auto const person = [](Skill gatherer, Skill craft)
    {
        ProfessionRules::BotProfessions result;
        result.Mains = {gatherer, craft};
        return result;
    };
    auto const ladders = [&](Snapshot& bot)
    {
        bot.ladders = {{MINING, MINING_APPRENTICE, 75, 10, 0, 5},
                       {MINING, MINING_JOURNEYMAN, 150, 100, 50, 10},
                       {SMITHING, SMITH_APPRENTICE, 75, 5 * GOLD, 0, 5}};
    };
    auto const need = [](NeedList const& list, ProfessionStep step, std::uint16_t skill) -> Need const*
    {
        for (Need const& candidate : list.needs)
            if (candidate.order == NeedOrder::Profession && candidate.step == step && candidate.skill == skill)
                return &candidate;
        return nullptr;
    };
    auto const hold = [](Snapshot& bot, std::uint16_t skill, std::uint16_t value, std::uint16_t cap)
    { bot.skills.push_back({skill, value, cap}); };

    Snapshot bot = Bot(index, CLASS_WARRIOR, 5, 20 * GOLD, 5);
    bot.person = person(Skill::Mining, Skill::Blacksmithing);
    ladders(bot);
    NeedList const fresh = ComputeNeeds(bot, index, Rates(5));
    Need const* learn = need(fresh, ProfessionStep::Learn, MINING);
    Check(learn && learn->kind == UpkeepKind::Profession && learn->item == MINING_APPRENTICE &&
              learn->trainer == TRAINER_MINING && learn->due && ProfessionErrandDue(*learn) && AnyErrandDue(fresh),
          "a bot with a person and no professions has a learn need for its gatherer at level 5");
    Check(need(fresh, ProfessionStep::Learn, SMITHING), "its craft is a need once the bot can pay for it (20 g)");
    Check(fresh.reserveProfession == 10 + 5 * GOLD && fresh.reserve >= fresh.reserveProfession,
          "the professions' prices are in the reserve (" + Money(fresh.reserveProfession) + ")");

    Snapshot young = bot;
    young.level = 4;
    Check(!need(ComputeNeeds(young, index, Rates(4)), ProfessionStep::Learn, MINING),
          "below the level the trainer asks there is no learn need");
    NeedRates late = Rates(5);
    late.gathererLevel = 12;
    Check(!need(ComputeNeeds(bot, index, late), ProfessionStep::Learn, MINING),
          "the config's gatherer level holds the learn back");

    Snapshot poor = bot;
    poor.money = 2 * GOLD;
    NeedList const poorList = ComputeNeeds(poor, index, Rates(5));
    Check(need(poorList, ProfessionStep::Learn, MINING) && !need(poorList, ProfessionStep::Learn, SMITHING),
          "with 2 g the gatherer is a need and the craft is not (no second gatherer in its place)");

    Snapshot capped = bot;
    hold(capped, MINING, 75, 75);
    capped.level = 12;
    NeedList const rank = ComputeNeeds(capped, index, Rates(12));
    Need const* journeyman = need(rank, ProfessionStep::Rank, MINING);
    Check(journeyman && journeyman->item == MINING_JOURNEYMAN && journeyman->copper == 100 &&
              !need(rank, ProfessionStep::Learn, MINING),
          "a skill at its cap with the level and skill the trainer asks makes the rank a need");
    Snapshot growing = capped;
    growing.skills.clear();
    hold(growing, MINING, 60, 75);
    Check(!need(ComputeNeeds(growing, index, Rates(12)), ProfessionStep::Rank, MINING),
          "a skill below its cap has no rank need");
    Snapshot lowLevel = capped;
    lowLevel.level = 9;
    Check(!need(ComputeNeeds(lowLevel, index, Rates(9)), ProfessionStep::Rank, MINING),
          "without the level the trainer asks there is no rank need");
    Snapshot lowSkill = capped;
    lowSkill.ladders[1].reqSkill = 80;
    Check(!need(ComputeNeeds(lowSkill, index, Rates(12)), ProfessionStep::Rank, MINING),
          "without the skill the trainer asks there is no rank need");

    // Short of gold the professions are funded last: repair takes the gold first
    Snapshot broke = bot;
    broke.person = person(Skill::Mining, Skill::None);
    broke.money = 300;
    broke.repairCost = 300;
    broke.brokenItems = 2;
    NeedList const last = ComputeNeeds(broke, index, Rates(5));
    Need const* repair = Find(last, NeedOrder::Repair);
    Need const* wait = need(last, ProfessionStep::Learn, MINING);
    Check(repair && wait && repair->funded == 300 && wait->funded == 0 && NeedOrder::Profession > NeedOrder::Use,
          "short of gold the repair takes it first and the profession is funded last");

    NeedList locked;
    locked.money = bot.money;
    AddProfessions(locked, index, bot, Rates(5), [](std::uint32_t) { return false; });
    NeedList open;
    open.money = bot.money;
    AddProfessions(open, index, bot, Rates(5), [](std::uint32_t spell) { return MayLearn(spell); });
    Check(locked.needs.empty() && locked.reserveProfession == 0 && !open.needs.empty(),
          "a locked spell is no need (MayLearn is the seam, true today)");

    Snapshot miner = bot;
    hold(miner, MINING, 20, 75);
    NeedList const toolless = ComputeNeeds(miner, index, Rates(5));
    Need const* pick = need(toolless, ProfessionStep::Tool, MINING);
    Check(index.Offers(UpkeepStock::Tool).size() == 6 && index.Offers(UpkeepStock::Food).size() == 1,
          "the index's tool sellers: every vendor row of a tool or recipe item, nothing else");
    Check(pick && pick->item == MINING_PICK && pick->copper == 90 && pick->stock == UpkeepStock::Tool &&
              pick->lotPrice == 90 && pick->units == 1 && pick->due && pick->vendor != 0 && StockPayable(*pick) &&
              !ProfessionErrandDue(*pick) && (PayableKinds(toolless, true) & StockBit(UpkeepStock::Tool)),
          "a miner without a pick has a due tool need of the stock kind Tool, its own trip once funded (WR5b)");
    std::unordered_map<std::uint32_t, StockMask> const toolCover =
        VendorCover(index, miner, toolless, StockBit(UpkeepStock::Tool));
    auto const sellsPick = [&toolCover](std::uint32_t vendor)
    {
        auto const found = toolCover.find(vendor);
        return found != toolCover.end() && (found->second & StockBit(UpkeepStock::Tool));
    };
    Check(sellsPick(VENDOR_SUPPLIER) && sellsPick(VENDOR_SMITH) && !sellsPick(VENDOR_INN),
          "every vendor that sells the pick covers the tool need, the innkeeper does not");
    auto const toolLine = [](std::vector<ShopLine> const& lines) -> ShopLine const*
    {
        for (ShopLine const& line : lines)
            if (line.kind == UpkeepStock::Tool)
                return &line;
        return nullptr;
    };
    std::vector<ShopLine> const atSmith = PlanShopping(toolless, index, miner, VENDOR_SMITH);
    std::vector<ShopLine> const atInn = PlanShopping(toolless, index, miner, VENDOR_INN);
    ShopLine const* bought = toolLine(atSmith);
    ShopLine const* unsold = toolLine(atInn);
    Check(bought && bought->outcome == ShopOutcome::Buy && bought->item == MINING_PICK && bought->lots == 1 &&
              bought->copper == 90 && unsold && unsold->outcome == ShopOutcome::NotSold && unsold->item == MINING_PICK,
          "the stop buys one pick where it is sold; elsewhere the line is not sold");
    Snapshot poorMiner = miner;
    poorMiner.money = 50;
    NeedList const poorTools = ComputeNeeds(poorMiner, index, Rates(5));
    Need const* unpaid = need(poorTools, ProfessionStep::Tool, MINING);
    std::vector<ShopLine> const poorAtSmith = PlanShopping(poorTools, index, poorMiner, VENDOR_SMITH);
    ShopLine const* shortLine = toolLine(poorAtSmith);
    Check(unpaid && unpaid->funded < 90 && !StockPayable(*unpaid) && !AnyErrandDue(poorTools) && shortLine &&
              shortLine->outcome == ShopOutcome::Budget,
          "a miner short of the pick's price takes no trip for it; at a stop it is gold short");
    // A jeweler's kit and grinder are tools by their TOOLS rows (NR20): held, neither is a need; without them, a need
    Snapshot jeweler = bot;
    jeweler.person = person(Skill::Mining, Skill::Jewelcrafting);
    hold(jeweler, JEWELCRAFTING, 20, 75);
    Need const* kit = need(ComputeNeeds(jeweler, index, Rates(5)), ProfessionStep::Tool, JEWELCRAFTING);
    Check(kit && kit->item == JEWELER_KIT && kit->due,
          "a jeweler without its kit and grinder has a due tool need for the kit");
    Snapshot equipped = jeweler;
    equipped.toolsHeld = {JEWELER_KIT, SIMPLE_GRINDER};
    Check(!need(ComputeNeeds(equipped, index, Rates(5)), ProfessionStep::Tool, JEWELCRAFTING),
          "a jeweler holding both tools has no tool need: it buys neither again");
    Snapshot smith = bot;
    hold(smith, SMITHING, 20, 75);
    Check(!need(ComputeNeeds(smith, index, Rates(5)), ProfessionStep::Tool, SMITHING),
          "a tool no vendor sells to the bot is no need (never handed out)");
    miner.toolsHeld = {MINING_PICK};
    Check(!need(ComputeNeeds(miner, index, Rates(5)), ProfessionStep::Tool, MINING),
          "a tool is a need only when the bot lacks it: with the pick in the bags there is none");
    Check(!need(fresh, ProfessionStep::Tool, MINING), "no tool need for a profession the bot does not hold");

    Snapshot none = bot;
    none.person.reset();
    Check(!need(ComputeNeeds(none, index, Rates(5)), ProfessionStep::Learn, MINING), "a bot without a person has none");

    // Secondary professions (WR5): learned like a craft once the bot can pay, at the trainer's learn spell
    Snapshot cook = bot;
    cook.person = person(Skill::None, Skill::None);
    cook.person->Secondary = ProfessionRules::SECONDARY_COOKING | ProfessionRules::SECONDARY_FISHING;
    cook.ladders = {{COOKING, COOKING_APPRENTICE, 75, 100, 0, 0}};
    NeedList const cooking = ComputeNeeds(cook, index, Rates(5));
    Need const* learnCooking = need(cooking, ProfessionStep::Learn, COOKING);
    Check(learnCooking && learnCooking->item == COOKING_SOLD && learnCooking->trainer == TRAINER_COOK &&
              ProfessionErrandDue(*learnCooking),
          "a person's secondary profession is a learn need, bought as the trainer's learn spell");
    Check(!need(cooking, ProfessionStep::Learn, FISHING) && !need(cooking, ProfessionStep::Tool, FISHING),
          "no ladder, no learn need; no tool for a profession not held");
    Snapshot poorCook = cook;
    poorCook.money = 50;
    Check(!need(ComputeNeeds(poorCook, index, Rates(5)), ProfessionStep::Learn, COOKING),
          "short of the price a secondary profession is no need");
    Snapshot fisher = cook;
    hold(fisher, FISHING, 10, 75);
    NeedList const fishing = ComputeNeeds(fisher, index, Rates(5));
    Need const* pole = need(fishing, ProfessionStep::Tool, FISHING);
    Check(pole && pole->item == FISHING_POLE && pole->due && pole->vendor == VENDOR_SUPPLIER && pole->copper == 23,
          "a fisher without a pole has a due tool need at the vendor that sells it");

    // The skill path (WR11): a crafter's rank waits for a stretch past the cap; the path's recipes come last
    Snapshot crafter = bot;
    crafter.level = 12;
    crafter.ladders.push_back({SMITHING, SMITH_JOURNEYMAN, 150, 500, 50, 10});
    hold(crafter, SMITHING, 75, 75);
    hold(crafter, MINING, 20, 75);
    Check(need(ComputeNeeds(crafter, index, Rates(12)), ProfessionStep::Rank, SMITHING),
          "without a known path a crafter at its cap has its rank need, as before WR11");
    NeedRates below = Rates(12);
    below.pathKnown = true;
    NeedList const belowList = ComputeNeeds(crafter, index, below);
    Check(!need(belowList, ProfessionStep::Rank, SMITHING) && need(belowList, ProfessionStep::Tool, MINING) &&
              !need(belowList, ProfessionStep::Recipe, SMITHING),
          "a known path whose stretch stays below the cap: no rank for the crafter, no recipe; the tool stays");
    NeedRates past = below;
    past.pathPurchases = {{SMITH_JOURNEYMAN, SMITHING, 500, true},
                          {SMITH_RECIPE, SMITHING, 300, false},
                          {SMITH_ITEM_RECIPE, SMITHING, 150, false},
                          {SMITH_SOLD_RECIPE, SMITHING, 400, false, SMITH_PLANS}};
    NeedList const pathList = ComputeNeeds(crafter, index, past);
    Need const* pathRank = need(pathList, ProfessionStep::Rank, SMITHING);
    Need const* taught = nullptr;
    Need const* fromItem = nullptr;
    Need const* plans = nullptr;
    for (Need const& candidate : pathList.needs)
        if (candidate.step == ProfessionStep::Recipe && candidate.kind == UpkeepKind::Profession)
            (candidate.item == SMITH_RECIPE ? taught : candidate.item == SMITH_PLANS ? plans : fromItem) = &candidate;
    Check(plans && plans->stock == UpkeepStock::Tool && plans->vendor == VENDOR_SMITH && plans->copper == 400 &&
              plans->due && !plans->trainer && !ProfessionErrandDue(*plans) && plans->useItems.size() == 1 &&
              plans->useItems[0] == SMITH_PLANS,
          "a recipe the path buys from a vendor is a due stock need of its recipe item at that vendor (WR14b)");
    Snapshot holdsPlans = crafter;
    holdsPlans.stock.push_back({SMITH_PLANS, 1, StockBit(UpkeepStock::Tool)});
    bool plansAgain = false;
    for (Need const& candidate : ComputeNeeds(holdsPlans, index, past).needs)
        plansAgain = plansAgain || (candidate.step == ProfessionStep::Recipe && candidate.item == SMITH_PLANS);
    Check(!plansAgain, "a recipe item in the bags is no need: the recipe use learns it (WR14b)");
    Check(pathRank && pathRank->item == SMITH_JOURNEYMAN && pathRank->trainer == TRAINER_SMITH,
          "a path past the cap makes the crafter's rank a need");
    Check(taught && taught->trainer == TRAINER_SMITH && taught->copper == 300 && taught->due &&
              ProfessionErrandDue(*taught) && Describe(*taught).find("recipe of skill 164") != std::string::npos,
          "the path's trainer recipe is a due need at its trainer, the errand's to buy (" +
              (taught ? Describe(*taught) : std::string("none")) + ")");
    Check(fromItem && fromItem->item == SMITH_ITEM_RECIPE && !fromItem->trainer && !fromItem->due &&
              !ProfessionErrandDue(*fromItem) && pathList.reserveProfession >= 500 + 300 + 150,
          "a recipe only an item teaches counts in the reserve and starts no trainer errand");
    std::size_t firstRecipe = pathList.needs.size();
    bool recipesLast = true;
    for (std::size_t at = 0; at < pathList.needs.size(); ++at)
    {
        bool const recipe =
            pathList.needs[at].kind == UpkeepKind::Profession && pathList.needs[at].step == ProfessionStep::Recipe;
        if (recipe && firstRecipe == pathList.needs.size())
            firstRecipe = at;
        if (!recipe && firstRecipe < at)
            recipesLast = false;
    }
    Check(firstRecipe < pathList.needs.size() && recipesLast && Find(pathList, NeedOrder::Profession) &&
              NeedOrder::Profession > NeedOrder::Training && NeedOrder::Profession > NeedOrder::Riding,
          "the path's recipes are the last needs, after every learn, rank and tool (WR4a's order)");
    Snapshot knows = crafter;
    knows.knownRecipes = {SMITH_RECIPE};
    NeedList const knownList = ComputeNeeds(knows, index, past);
    bool knownNeed = false;
    for (Need const& candidate : knownList.needs)
        knownNeed = knownNeed || (candidate.step == ProfessionStep::Recipe && candidate.item == SMITH_RECIPE);
    Check(!knownNeed, "a recipe the bot knows is no need");
    NeedList lockedRecipe;
    lockedRecipe.money = crafter.money;
    AddProfessions(lockedRecipe, index, crafter, past, [](std::uint32_t spell) { return spell != SMITH_RECIPE; });
    bool lockedNeed = false;
    bool itemNeed = false;
    for (Need const& candidate : lockedRecipe.needs)
    {
        lockedNeed = lockedNeed || (candidate.step == ProfessionStep::Recipe && candidate.item == SMITH_RECIPE);
        itemNeed = itemNeed || (candidate.step == ProfessionStep::Recipe && candidate.item == SMITH_ITEM_RECIPE);
    }
    Check(!lockedNeed && itemNeed, "MayLearn false removes the path's recipe need, the others stay (WR.11)");
}

// The pet need (WU21b): a hunter of 10 or above with Tame Beast and no pet wants the nearest tameable spawn of a
// wanted family at or below its level on its map; an exotic one only with Beast Mastery's aura
constexpr std::uint32_t BEAST_WOLF = 299;     // family 1, levels 10-11, 100 yd
constexpr std::uint32_t BEAST_BOAR = 113;     // family 5, levels 8-9, 300 yd
constexpr std::uint32_t BEAST_BEAR = 1188;    // family 4, level 30, 20 yd
constexpr std::uint32_t BEAST_EXOTIC = 6498;  // family 39 (devilsaur), exotic, level 5, 50 yd
constexpr std::uint32_t BEAST_CAT = 2042;     // family 2, on another map, 5 yd

UpkeepIndex PetFixture()
{
    UpkeepGather input;
    input.trainers.push_back({TRAINER_RIDING, TrainerKind::Mount, 0, {RidingSpell(SPELL_APPRENTICE, 4 * GOLD, 20, 0)}});
    input.trainerEntries = {{TRAINER_RIDING, TRAINER_RIDING}};
    input.tameable = {{0, 100.0f, 0.0f, 0.0f, BEAST_WOLF, 1, 11, false},
                      {0, 300.0f, 0.0f, 0.0f, BEAST_BOAR, 5, 9, false},
                      {0, 0.0f, 20.0f, 0.0f, BEAST_BEAR, 4, 30, false},
                      {0, 0.0f, 50.0f, 0.0f, BEAST_EXOTIC, 39, 5, true},
                      {1, 5.0f, 0.0f, 0.0f, BEAST_CAT, 2, 6, false},
                      {0, 300.0f, 0.0f, 0.0f, BEAST_BOAR, 5, 9, false}};  // the same spawn twice: kept once
    UpkeepIndex index = UpkeepIndex::Build(std::move(input));
    index.version = 7;
    return index;
}

void PetNeed()
{
    std::cout << "Scenario: the pet need of a hunter without a pet\n";
    UpkeepIndex const index = PetFixture();
    Check(index.stats.tameableSpawns == 5 && index.stats.tameableEntries == 5 &&
              index.TameableOn(0).second - index.TameableOn(0).first == 4,
          "the index keeps each tameable spawn once, by map");
    Check(index.TameTarget(0, BEAST_WOLF) && index.TameTarget(0, BEAST_EXOTIC) && index.TameTarget(1, BEAST_CAT) &&
              !index.TameTarget(1, BEAST_WOLF) && !index.TameTarget(0, BEAST_CAT) && !index.TameTarget(2, BEAST_WOLF),
          "the tame stop's lookup: an entry is a tame target on the map the index keeps it on, nowhere else");
    auto const petNeed = [](NeedList const& list) { return Find(list, NeedOrder::Pet); };
    auto const hunter = [&index](std::uint8_t level)
    {
        Snapshot bot = Bot(index, PET_CLASS_HUNTER, level, 20 * GOLD, level);
        bot.knowsTameBeast = true;
        return bot;
    };

    Snapshot const lone = hunter(12);
    NeedList const lonely = Show("hunter 12, Tame Beast, no pet", lone, index, Rates(12));
    Need const* need = petNeed(lonely);
    Check(need && need->kind == UpkeepKind::Pet && need->beast == BEAST_WOLF && need->beastFamily == 1 &&
              need->copper == 0 && need->due && PetErrandDue(*need) && AnyErrandDue(lonely),
          "the nearest wanted spawn: the wolf at 100 yd (the bear is above the level, the devilsaur exotic, the cat "
          "on another map), due at once, price 0, a trip");
    Check(need && need->beastAt.map == 0 && need->beastAt.x == 100.0f, "the need names the spawn's place");
    Check(lonely.reserve == ComputeNeeds(
                                [&lone]
                                {
                                    Snapshot s = lone;
                                    s.hasPet = true;
                                    return s;
                                }(),
                                index, Rates(12))
                                .reserve,
          "the pet need adds nothing to the reserve");

    Snapshot pet = hunter(12);
    pet.hasPet = true;
    Check(!petNeed(ComputeNeeds(pet, index, Rates(12))), "a hunter with a pet (summoned or not) has no pet need");
    Snapshot untaught = hunter(12);
    untaught.knowsTameBeast = false;
    Check(!petNeed(ComputeNeeds(untaught, index, Rates(12))), "a hunter without Tame Beast has no pet need");
    Check(!petNeed(ComputeNeeds(hunter(9), index, Rates(9))), "a level 9 hunter (Tame Beast learned) has no pet need");
    Snapshot mage = Bot(index, CLASS_MAGE, 12, 20 * GOLD, 12);
    mage.knowsTameBeast = true;
    Check(!petNeed(ComputeNeeds(mage, index, Rates(12))), "a mage has no pet need");

    NeedList const ten = ComputeNeeds(hunter(10), index, Rates(10));
    Check(petNeed(ten) && petNeed(ten)->beast == BEAST_BOAR,
          "at 10 the wolf (up to 11) is above the level: the boar at 300 yd");
    Snapshot alone = hunter(12);
    alone.position = {2, 0.0f, 0.0f, 0.0f};
    NeedList const nothing = ComputeNeeds(alone, index, Rates(12));
    Check(petNeed(nothing) && !petNeed(nothing)->due && !petNeed(nothing)->beast && !AnyErrandDue(nothing),
          "no tameable spawn on the map: the need stays, no trip");
    Snapshot low = hunter(12);
    low.position = {0, 0.0f, 25.0f, 0.0f};
    Check(petNeed(ComputeNeeds(low, index, Rates(12)))->beast == BEAST_WOLF,
          "beside the level 30 bear only a beast at or below the level is named");

    Snapshot master = hunter(12);
    master.tamesExotic = true;
    Check(petNeed(ComputeNeeds(master, index, Rates(12)))->beast == BEAST_EXOTIC,
          "with Beast Mastery's aura the exotic beast at 50 yd is the nearest");
    std::cout << "  " << Describe(*need) << "\n";
}

// The swap form (WU21c): a Beast Mastery hunter (the exotic aura) with a pet of no wanted family stores it at a stable
// master and tames a wanted (exotic) beast; a slot is bought only from the gold above the reserve
void PetSwap()
{
    std::cout << "Scenario: the stable swap of an unwanted pet\n";
    UpkeepIndex const index = PetFixture();
    auto const petNeed = [](NeedList const& list) { return Find(list, NeedOrder::Pet); };
    auto const master = [&index](std::uint64_t money)
    {
        Snapshot bot = Bot(index, PET_CLASS_HUNTER, 60, money, 60);
        bot.knowsTameBeast = true;
        bot.tamesExotic = true;
        bot.hasPet = true;
        bot.petFamily = 1;  // a wolf
        bot.stableSlotPrice = 500;
        return bot;
    };
    Check(
        !WantedFamily(1, true) && WantedFamily(39, true) && WantedFamily(1, false) && WantedFamily(0, true),
        "Beast Mastery wants an exotic family, not a wolf; another spec wants any; an unknown family counts as wanted");

    NeedList const rich =
        Show("Beast Mastery 60 with a wolf, 20 g, no slot bought", master(20 * GOLD), index, Rates(60));
    Need const* swap = petNeed(rich);
    Check(swap && swap->swap && swap->beast == BEAST_EXOTIC && swap->copper == 500 && swap->due &&
              PetErrandDue(*swap) && AnyErrandDue(rich),
          "a Beast Mastery hunter at 60 swaps its wolf: the exotic beast named, the first slot's 500 c, due");
    Check(rich.reserve == ComputeNeeds(
                              [&]
                              {
                                  Snapshot s = master(20 * GOLD);
                                  s.petFamily = 39;
                                  return s;
                              }(),
                              index, Rates(60))
                              .reserve,
          "the slot adds nothing to the reserve (it is paid from the gold above it)");

    Snapshot wanted = master(20 * GOLD);
    wanted.petFamily = 39;  // a devilsaur
    Check(!petNeed(ComputeNeeds(wanted, index, Rates(60))), "a hunter with a wanted pet does not swap");
    Snapshot other = master(20 * GOLD);
    other.tamesExotic = false;
    Check(!petNeed(ComputeNeeds(other, index, Rates(60))), "a hunter of another spec keeps its wolf");

    std::uint64_t const reserve = rich.reserve;
    NeedList const poor = Show("the same, 499 c above the reserve", master(reserve + 499), index, Rates(60));
    Check(petNeed(poor) && !petNeed(poor)->due && !PetErrandDue(*petNeed(poor)),
          "a slot priced above the free gold (499 c above the reserve) means no swap");
    NeedList const exact = ComputeNeeds(master(reserve + 500), index, Rates(60));
    Check(petNeed(exact) && petNeed(exact)->due, "500 c above the reserve pays the slot");
    Check(!PetSwapPayable(400, 0, 500) && PetSwapPayable(500, 0, 500) && !PetSwapPayable(900, 500, 500) &&
              PetSwapPayable(0, 500, 0),
          "the gold above the reserve pays the slot; an empty bought slot costs nothing");

    Snapshot free = master(0);
    free.stableFree = true;
    NeedList const freeList = ComputeNeeds(free, index, Rates(60));
    Check(petNeed(freeList) && petNeed(freeList)->copper == 0 && petNeed(freeList)->due,
          "an empty bought slot: the swap costs nothing and is due without gold");
    Snapshot full = master(20 * GOLD);
    full.stableSlotPrice = 0;  // every slot bought and full
    Check(!petNeed(ComputeNeeds(full, index, Rates(60))), "every slot full: the hunter keeps its pet");
    Snapshot away = master(20 * GOLD);
    away.position = {1, 0.0f, 0.0f, 0.0f};  // only a cat (not wanted) on map 1
    Check(!petNeed(ComputeNeeds(away, index, Rates(60))),
          "no wanted beast on the map: no swap (the hunter keeps its pet)");
    Snapshot lone = master(20 * GOLD);
    lone.hasPet = false;
    Check(petNeed(ComputeNeeds(lone, index, Rates(60))) && !petNeed(ComputeNeeds(lone, index, Rates(60)))->swap,
          "without a pet the tame need of WU21b, no swap");
    if (swap)
        std::cout << "  " << Describe(*swap) << "\n";
}

void TameSteps()
{
    std::cout << "Scenario: the tame at the beast (break off, cast again in the fight, rest, bounded tries)\n";
    Check(!TameBreaksOff(100.0f) && !TameBreaksOff(TAME_BREAK_OFF_HEALTH_PCT) && TameBreaksOff(34.0f),
          "the channel holds down to the break-off health and breaks off below it");
    Check(TameNextStep(0, 100.0f, 0) == TameNext::Cast, "a healthy hunter casts at once on arrival");
    Check(TameNextStep(1, 40.0f, 1000) == TameNext::Rest && TameNextStep(0, 40.0f, 0) == TameNext::Rest,
          "a hurt hunter rests before a try (after a broken channel or on arrival)");
    Check(TameNextStep(1, 40.0f, TAME_REST_MS) == TameNext::Cast,
          "the rest ends after TAME_REST_MS whatever the health (a bot never stalls)");
    Check(TameNextStep(1, TAME_START_HEALTH_PCT, 0) == TameNext::Cast, "back to the start health: another try");
    Check(TameNextStep(TAME_MAX_TRIES - 1, 100.0f, 0) == TameNext::Cast &&
              TameNextStep(TAME_MAX_TRIES, 100.0f, 0) == TameNext::GiveUp,
          "the stop casts TAME_MAX_TRIES times at most, then gives up");
    Check(TAME_START_HEALTH_PCT > TAME_BREAK_OFF_HEALTH_PCT, "a try starts above the break-off health");
    {
        // The reach bound over a chase (WU21e run 2): a re-pick every 10 s keeps the start, the stop ends in time
        std::uint32_t start = 0;
        std::uint32_t stuckAt = 0;
        for (std::uint32_t now = 5000; now <= 400000 && !stuckAt; now += 1000)
        {
            start = TameReachStartMs(start, now);  // the pick of this tick, a new beast every 10 s
            if (now - start > TAME_REACH_MS)
                stuckAt = now;
        }
        Check(stuckAt == 5000 + TAME_REACH_MS + 1000, "re-picks keep the reach bound's start: a chase ends after it");
        Check(TameReachStartMs(0, 90000) == 90000 && TameReachStartMs(5000, 90000) == 5000,
              "a reset (a channel's end, a fight at the stop, a respawn wait) starts the bound anew at the next pick");
    }
    Check(TameRecastsInFight(1, 90.0f, 0) && TameRecastsInFight(1, TAME_START_HEALTH_PCT, TAME_RECAST_MS),
          "a channel the beast broke (stun, knockback) is cast again at once in its fight");
    Check(!TameRecastsInFight(TAME_MAX_TRIES, 90.0f, 0), "no cast again in the fight past the tries");
    Check(!TameRecastsInFight(1, TAME_START_HEALTH_PCT - 1.0f, 0),
          "a hurt hunter fights the beast instead (the stop's rest and next try follow)");
    Check(!TameRecastsInFight(1, 90.0f, TAME_RECAST_MS + 1), "the wait for a cast again ends (a bot never stalls)");

    // The beasts' spells, as Spell.dbc gives their effects (effect, aura): APPLY_AURA 6, SCHOOL_DAMAGE 2,
    // WEAPON_DAMAGE 58; MOD_ROOT 26, MOD_DECREASE_SPEED 33
    Check(!BreaksTameChannel(58, 0) && BreaksTameChannel(6, AURA_MOD_STUN),
          "Swoop (weapon damage, a stun) breaks the channel: the buzzard is no tame target");
    Check(BreaksTameChannel(EFFECT_KNOCK_BACK, 0), "Wing Buffet's knockback breaks the channel: the moth is none");
    Check(BreaksTameChannel(6, AURA_MOD_FEAR) && BreaksTameChannel(6, AURA_MOD_CONFUSE) &&
              BreaksTameChannel(6, AURA_MOD_SILENCE) && BreaksTameChannel(EFFECT_INTERRUPT_CAST, 0) &&
              BreaksTameChannel(EFFECT_KNOCK_BACK_DEST, 0),
          "a fear, a confuse, a silence or an interrupt breaks it too");
    Check(!BreaksTameChannel(2, 0) && !BreaksTameChannel(6, 26) && !BreaksTameChannel(6, 33),
          "a bite, a root or a slow only hurts: such a beast stays a tame target");
    Check(!TameTargetSpawns(1) && TameTargetSpawns(2) && TameTargetSpawns(40),
          "a single named spawn is no tame target (one tame or kill takes it); a beast with several spawns is");
}

// The threats of each beast of a pack at the hunter's spot 10 yards short of it on the way in from (hx, hy)
std::vector<std::uint32_t> PackThreats(std::vector<TameSiteUnit> const& units, float hx, float hy, float assist)
{
    std::vector<std::uint32_t> counts;
    for (std::size_t i = 0; i < units.size(); ++i)
    {
        float const yards = std::hypot(hx - units[i].x, hy - units[i].y);
        float const share = yards > TAME_CAST_YARDS / 2 ? TAME_CAST_YARDS / 2 / yards : 1.0f;
        float const sx = units[i].x + (hx - units[i].x) * share;
        float const sy = units[i].y + (hy - units[i].y) * share;
        std::uint32_t count = 0;
        for (std::size_t j = 0; j < units.size(); ++j)
            if (j != i && TameThreat(units[j], units[i].x, units[i].y, sx, sy, assist))
                ++count;
        counts.push_back(count);
    }
    return counts;
}

void TameSiteRules()
{
    std::cout << "Scenario: the tame's site (WU.23: threats, the pick, clear or give up, the respawn wait, patrols)\n";
    Check(TameSpawnStands(0) && TameSpawnStands(1) && !TameSpawnStands(MOVEMENT_WAYPOINT),
          "a standing or wandering spawn is a tame target, a patrolling one (a waypoint path) is not");

    float const assist = 10.0f;  // the core's CreatureFamilyAssistanceRadius default
    TameSiteUnit const hostile{30.0f, 0.0f, 20.0f, true, false};
    Check(TameThreat(hostile, 0.0f, 20.0f, 10.0f, 0.0f, assist), "a hostile whose aggro reach covers the spot");
    Check(!TameThreat({60.0f, 0.0f, 20.0f, true, false}, 0.0f, 20.0f, 10.0f, 0.0f, assist),
          "a hostile beyond its aggro reach of the spot is none (the bot is no closer)");
    Check(TameThreat({5.0f, 25.0f, 0.0f, false, true}, 0.0f, 20.0f, 10.0f, 0.0f, assist) &&
              TameThreat({6.0f, 9.0f, 0.0f, false, true}, 0.0f, 20.0f, 0.0f, 0.0f, assist),
          "a pack member near the beast, or near its run to the hunter, answers its call for help");
    Check(!TameThreat({40.0f, 40.0f, 0.0f, false, true}, 0.0f, 20.0f, 0.0f, 0.0f, assist) &&
              !TameThreat({1.0f, 21.0f, 20.0f, false, false}, 0.0f, 20.0f, 0.0f, 0.0f, assist),
          "a far pack member, or a neutral creature of another faction beside the beast, joins nothing");
    Check(TameThreat({80.0f, 80.0f, 0.0f, false, false, true}, 0.0f, 20.0f, 0.0f, 0.0f, assist),
          "a creature that fights the hunter already is a threat wherever it stands (no cast amid a fight)");

    // A pack along x: three wolves within the assistance radius of each other, one 40 yards out at its edge; the
    // hunter comes from the far side of the edge wolf
    std::vector<TameSiteUnit> pack = {{0.0f, 0.0f, 15.0f, false, true},
                                      {6.0f, 0.0f, 15.0f, false, true},
                                      {12.0f, 0.0f, 15.0f, false, true},
                                      {52.0f, 0.0f, 15.0f, false, true}};
    std::vector<std::uint32_t> const counts = PackThreats(pack, 90.0f, 0.0f, assist);
    Check(counts[3] == 0 && counts[0] > 0 && counts[1] > 0 && counts[2] > 0,
          "of a pack the edge member has no threat, the others one or more");
    TamePick const edge{counts[3], false, 38.0f};
    TamePick const inner{counts[2], false, 78.0f};
    Check(TameBetter(edge, inner) && !TameBetter(inner, edge),
          "the pick takes the edge member (a site where none joins)");
    Check(TameBetter({0, true, 80.0f}, {1, false, 20.0f}),
          "a beast of another entry with a clean site beats the planned entry's beast whose pack would join");
    Check(TameBetter({0, false, 80.0f}, {0, true, 20.0f}) && TameBetter({1, false, 20.0f}, {1, false, 30.0f}),
          "at equal threats the planned entry first, then the nearest");

    // Two hunters at one pack: the first tames the edge member (in its fight, so no threat and no pick); the second
    // takes the nearest of the rest and fights its pack-mates first
    pack.pop_back();
    std::vector<std::uint32_t> const rest = PackThreats(pack, 90.0f, 0.0f, assist);
    Check(rest[2] > 0 && TameSiteNext(rest[2], 0) == TameSite::Clear,
          "the second hunter at the pack clears the members that would join, then casts");
    Check(TameSiteNext(0, 0) == TameSite::Cast && TameSiteNext(0, TAME_MAX_CLEARS) == TameSite::Cast,
          "a clean site casts at once, also after clears");
    Check(
        TameSiteNext(2, TAME_MAX_CLEARS - 1) == TameSite::Clear && TameSiteNext(1, TAME_MAX_CLEARS) == TameSite::GiveUp,
        "a site still busy after TAME_MAX_CLEARS fights gives the stop up (a bot never stalls)");

    // The respawn wait, from the spawn's respawn time: wanted beasts get the long budget, any pet the short one
    Check(TameWaitMs(60 * 1000, 0, false) == 60 * 1000 + TAME_RESPAWN_GRACE_MS &&
              TameWaitMs(300 * 1000, 0, false) == 300 * 1000 + TAME_RESPAWN_GRACE_MS,
          "any pet: a respawn within the short budget (the common 5 min) is waited for, the grace on top");
    Check(
        TameWaitMs(400 * 1000, 0, false) == 0 && TameWaitMs(400 * 1000, 0, true) == 400 * 1000 + TAME_RESPAWN_GRACE_MS,
        "a respawn beyond 5 min: any pet takes another target, the wanted beast is waited for");
    Check(TameWaitMs(600 * 1000, 400 * 1000, true) == 0 && TameWaitMs(100 * 1000, 400 * 1000, true) > 0,
          "the budget counts from the first wait: a second wait only while it fits");
    Check(TameWaitMs(TAME_WAIT_WANTED_MS + 1, 0, true) == 0 && TameWaitMs(0, TAME_WAIT_WANTED_MS + 1, true) == 0,
          "every wait ends: a respawn beyond the budget, or the budget spent, means no wait");
    Check(TameWaitMs(0, 0, false) == TAME_RESPAWN_GRACE_MS, "a respawn due now waits the grid's update only");

    // When the next beast is free (WU21f, lab7 at 2000 bots: the Mulgore plainstriders killed by others, their
    // creatures removed, only the map's respawn records left): the soonest of what the wait reads
    std::int64_t const now = 1'000'000;
    Check(TameFreeInS(TAME_FREE_NONE, false, now + 90, now) == 90 &&
              TameWaitMs(TameFreeInS(TAME_FREE_NONE, false, now + 90, now) * 1000, 0, false) > 0,
          "beasts all killed by others: the removed spawn's respawn record arms the wait");
    Check(TameFreeInS(TameFreeInS(TAME_FREE_NONE, false, now + 200, now), false, now + 40, now) == 40,
          "several dead spawns: the soonest respawn");
    Check(TameFreeInS(TameFreeInS(TAME_FREE_NONE, false, now + 200, now), true, 0, now) == 0,
          "a beast in another's fight is free at once: the wait reads again after the grace");
    Check(TameFreeInS(TAME_FREE_NONE, false, now - 3, now) == 0, "a respawn overdue (the 5 s respawn pass) is due now");
    Check(TameFreeInS(TAME_FREE_NONE, false, now + 400, now) == 400 &&
              TameWaitMs(TameFreeInS(TAME_FREE_NONE, false, now + 400, now) * 1000, 0, false) == 0,
          "a respawn beyond the any-pet budget arms no wait: the stop is given up, the need's next trip tries again");
}

// The stock rule invariant (WU28): what the shopping list buys, the sell rule keeps, up to the stock target; above
// it the surplus is sellable. A sell rule answers how many of a bag item the bot keeps (the rest it sells)
using KeptRule = std::function<std::uint32_t(Snapshot const&, NeedRates const&, StockItem const&)>;

constexpr std::array<std::uint32_t, 6> STOCK_VENDORS = {VENDOR_INN,     VENDOR_ARMS,   VENDOR_GENERAL,
                                                        VENDOR_REAGENT, VENDOR_POISON, VENDOR_STABLE};
constexpr std::array<std::uint32_t, 12> STOCK_ITEMS = {
    ITEM_BREAD,  ITEM_GOOD_BREAD,  ITEM_WATER,     ITEM_MEAT,      ITEM_ARROW,    ITEM_RAZOR_ARROW,
    ITEM_BULLET, ITEM_TERRORSHAFT, ITEM_INSTANT_1, ITEM_INSTANT_2, ITEM_DEADLY_1, ITEM_POWDER};
constexpr std::uint32_t STOCK_SNAPSHOTS = 4000;

// A bag item as the snapshot fill counts it, from the fixture's offers of each kind
StockItem StockOf(UpkeepIndex const& index, std::uint8_t cls, std::uint32_t item, std::uint32_t count)
{
    StockItem entry;
    entry.item = item;
    entry.count = count;
    for (std::size_t slot = 0; slot < UPKEEP_STOCK_COUNT; ++slot)
    {
        UpkeepStock const kind = static_cast<UpkeepStock>(slot);
        for (VendorOffer const& offer : index.Offers(kind))
            if (offer.item == item && (kind != UpkeepStock::Reagent || (offer.classMask & UpkeepIndex::ClassBit(cls))))
            {
                entry.kinds |= StockBit(kind);
                entry.diet |= offer.diet;
                entry.subClass = offer.subClass;
                entry.damage = offer.damage;
                entry.lot = std::max<std::uint32_t>(offer.buyCount, 1);
            }
    }
    return entry;
}

std::uint32_t LotOf(UpkeepIndex const& index, std::uint32_t item)
{
    for (std::size_t slot = 0; slot < UPKEEP_STOCK_COUNT; ++slot)
        for (VendorOffer const& offer : index.Offers(static_cast<UpkeepStock>(slot)))
            if (offer.item == item)
                return std::max<std::uint32_t>(offer.buyCount, 1);
    return 1;
}

std::string Describe(Snapshot const& bot, NeedRates const& rates)
{
    std::string text = "class " + std::to_string(bot.cls) + " level " + std::to_string(bot.level) + " thrift " +
                       std::to_string(TraitUnit(bot, TraitRules::Trait::Thrift)) + " ammo " +
                       std::to_string(bot.ammoSubClass) + "/" + std::to_string(bot.ammoDamage) + " diet " +
                       std::to_string(bot.petDiet) + ", use";
    for (float const use : rates.usePerHour)
        text += " " + std::to_string(use);
    text += ", stock";
    for (StockItem const& entry : bot.stock)
        text += " " + std::to_string(entry.item) + "x" + std::to_string(entry.count);
    return text;
}

// One snapshot: every bought lot is kept whole after the buy; a kind's single-kind items keep no more than its target
// and one lot (the surplus is sellable). Returns the broken half, empty when both hold. Over the stock rule's kinds:
// demand keeps food, water and poison (WU37b), item-demand's StockWantInvariant holds the invariant for them
std::string Breaks(UpkeepIndex const& index, Snapshot const& bot, NeedRates const& rates, KeptRule const& rule)
{
    NeedList const list = ComputeNeeds(bot, index, rates);
    for (std::uint32_t const vendor : STOCK_VENDORS)
        for (ShopLine const& line : PlanShopping(list, index, bot, vendor))
        {
            if (line.outcome != ShopOutcome::Buy || (StockBit(line.kind) & DEMAND_KINDS))
                continue;
            Snapshot after = bot;
            auto it = std::find_if(after.stock.begin(), after.stock.end(),
                                   [&line](StockItem const& entry) { return entry.item == line.item; });
            if (it == after.stock.end())
                it = after.stock.insert(after.stock.end(), StockOf(index, bot.cls, line.item, 0));
            it->count += line.lots * line.lotSize;
            std::uint32_t const kept = rule(after, rates, *it);
            if (kept < it->count)
                return "vendor " + std::to_string(vendor) + " buys " + std::to_string(line.lots * line.lotSize) +
                       " of " + std::to_string(line.item) + " (" + ShopWord(line.kind) + ", target " +
                       std::to_string(line.target) + "), the sell rule keeps " + std::to_string(kept) + " of " +
                       std::to_string(it->count);
        }
    for (std::size_t slot = 0; slot < UPKEEP_STOCK_COUNT; ++slot)
    {
        UpkeepStock const kind = static_cast<UpkeepStock>(slot);
        std::uint32_t const target = kind == UpkeepStock::Mount ? 0 : StockTarget(bot, rates, kind);
        if (!target || (StockBit(kind) & DEMAND_KINDS))
            continue;
        std::uint32_t kept = 0;
        std::uint32_t lot = 1;
        for (StockItem const& entry : bot.stock)
            if (entry.kinds == StockBit(kind))
            {
                kept += rule(bot, rates, entry);
                lot = std::max(lot, LotOf(index, entry.item));
            }
        if (kept > target + lot - 1)
            return std::string(ShopWord(kind)) + ": keeps " + std::to_string(kept) + " above the target " +
                   std::to_string(target) + " and a lot of " + std::to_string(lot);
    }
    return {};
}

// A random persistent bot: class, level, gold, thrift, measured use 0.5..40 per hour or the default, a ranged weapon
// or none, equipped ammo or none, a pet diet or none, and a few stock items in random counts
Snapshot RandomBot(UpkeepIndex const& index, std::mt19937_64& rng, NeedRates& rates)
{
    constexpr std::array<std::uint8_t, 6> CLASSES = {CLASS_WARRIOR, CLASS_ROGUE,  CLASS_HUNTER,
                                                     CLASS_MAGE,    CLASS_PRIEST, CLASS_PALADIN};
    auto const pick = [&rng](std::uint32_t count)
    { return std::uniform_int_distribution<std::uint32_t>(0, count - 1)(rng); };
    std::uint8_t const cls = CLASSES[pick(CLASSES.size())];
    std::uint8_t const level = static_cast<std::uint8_t>(1 + pick(LEVEL_CAP));
    Snapshot bot = Bot(index, cls, level, pick(100) * GOLD + pick(GOLD), level);
    bot.stock.clear();
    bot.traits = Traits(static_cast<int>(pick(201)) - 100, 0, false);
    bool const shoots = cls == CLASS_HUNTER || cls == CLASS_WARRIOR || cls == CLASS_ROGUE;
    bot.ammoSubClass = shoots && pick(3) ? (pick(2) ? ARROWS : static_cast<std::uint8_t>(UpkeepItem::SUBCLASS_BULLET))
                                         : std::uint8_t(0);
    bot.ammoDamage = 0;
    bot.petDiet = cls == CLASS_HUNTER && pick(2) ? MEAT_DIET : 0;
    for (std::uint32_t n = pick(6); n; --n)
    {
        StockItem entry = StockOf(index, cls, STOCK_ITEMS[pick(STOCK_ITEMS.size())], 0);
        entry.count = (pick(4) ? 1 + pick(60) : 1 + pick(400)) * (entry.kinds & StockBit(UpkeepStock::Ammo) ? 20 : 1);
        auto const it = std::find_if(bot.stock.begin(), bot.stock.end(),
                                     [&entry](StockItem const& other) { return other.item == entry.item; });
        if (it != bot.stock.end())
            it->count += entry.count;
        else
            bot.stock.push_back(entry);
    }
    if (bot.ammoSubClass && pick(2))
        for (StockItem const& entry : bot.stock)
            if (entry.subClass == bot.ammoSubClass && (entry.kinds & StockBit(UpkeepStock::Ammo)))
                bot.ammoDamage = entry.damage;
    // The slot measure samples only the kinds the class uses (UseMeasured)
    rates = Rates(level);
    for (std::size_t slot = 0; slot < UPKEEP_STOCK_COUNT; ++slot)
    {
        float const use = pick(3) ? std::uniform_real_distribution<float>(0.5f, 40.0f)(rng) : 0.0f;
        rates.usePerHour[slot] = UseMeasured(cls, level, static_cast<UpkeepStock>(slot)) ? use : 0.0f;
    }
    return bot;
}

void StockRuleInvariant(UpkeepIndex const& index, std::uint64_t seed, KeptRule const& rule, char const* name,
                        bool holds)
{
    std::cout << "Scenario: the stock rule invariant against " << name << ", seed " << seed << "\n";
    std::mt19937_64 rng(seed);
    std::string broken;
    std::uint32_t tried = 0;
    for (; tried < STOCK_SNAPSHOTS && broken.empty(); ++tried)
    {
        NeedRates rates;
        Snapshot const bot = RandomBot(index, rng, rates);
        broken = Breaks(index, bot, rates, rule);
        if (!broken.empty())
            std::cout << "   counterexample at snapshot " << tried << ": " << broken << "\n     "
                      << Describe(bot, rates) << "\n";
    }
    Check(broken.empty() == holds, std::string(name) + (holds ? ": no counterexample in " : ": a counterexample in ") +
                                       std::to_string(tried) + " snapshots");
}

// One keeper per kind (WU37b, WU.27): the stock rule decides no food, water or poison (demand's verdict does); meat
// that is food and pet food keeps its pet food verdict for a hunter whose pet eats it, none for a bot without a pet
void DemandKinds(UpkeepIndex const& index)
{
    std::cout << "Scenario: the stock rule leaves food, water and poison to demand (WU37b)\n";
    NeedRates const rates = Rates(30);
    Snapshot priest = Bot(index, CLASS_PRIEST, 30, 50 * GOLD, 30);
    priest.stock = {StockOf(index, CLASS_PRIEST, ITEM_GOOD_BREAD, 40), StockOf(index, CLASS_PRIEST, ITEM_WATER, 1)};
    Snapshot rogue = Bot(index, CLASS_ROGUE, 30, 50 * GOLD, 30);
    rogue.stock = {StockOf(index, CLASS_ROGUE, ITEM_INSTANT_2, 1)};
    Check(KeepOf(&index, priest, rates, priest.stock[0]).verdict == StockVerdict::None &&
              KeepOf(&index, priest, rates, priest.stock[1]).verdict == StockVerdict::None &&
              KeepOf(&index, rogue, rates, rogue.stock[0]).verdict == StockVerdict::None,
          "food, water and poison: no stock verdict, above or below the target");
    Snapshot hunter = Bot(index, CLASS_HUNTER, 30, 50 * GOLD, 30);
    hunter.petDiet = MEAT_DIET;
    StockItem meat = StockOf(index, CLASS_HUNTER, ITEM_MEAT, 1);
    meat.kinds |= StockBit(UpkeepStock::Food);  // the fixture's meat is pet food only; Tough Jerky is both
    hunter.stock = {meat};
    Snapshot warrior = Bot(index, CLASS_WARRIOR, 30, 50 * GOLD, 30);
    warrior.stock = {meat};
    Check((meat.kinds & StockBit(UpkeepStock::PetFood)) &&
              KeepOf(&index, hunter, rates, hunter.stock[0]).verdict == StockVerdict::Buy &&
              KeepOf(&index, warrior, rates, warrior.stock[0]).verdict == StockVerdict::None,
          "meat (food and pet food): the pet food verdict for the hunter's pet, none without a pet");
}

// Demand's buys (item use WI9b): potions at an apothecary beside the inn's bread, the weaker potion alone at an
// alchemist, a mana potion no want names at a third vendor; the riding trainer Bot() needs
constexpr std::uint32_t VENDOR_APOTHECARY = 5010;
constexpr std::uint32_t VENDOR_ALCHEMIST = 5008;
constexpr std::uint32_t VENDOR_MANA = 5009;
constexpr std::uint32_t ITEM_SUPERIOR_HEALING = 3928;
constexpr std::uint32_t ITEM_SUPER_HEALING = 22829;
constexpr std::uint32_t ITEM_SUPERIOR_MANA = 6149;
constexpr std::uint32_t ITEM_UNSOLD_POTION = 13446;
UpkeepIndex UseFixture()
{
    using namespace UpkeepItem;
    UpkeepGather input;
    input.trainers.push_back({TRAINER_RIDING, TrainerKind::Mount, 0, {RidingSpell(SPELL_APPRENTICE, 4 * GOLD, 20, 0)}});
    input.trainerEntries = {{TRAINER_RIDING, TRAINER_RIDING}};
    UpkeepItemFacts bread = Item(ITEM_BREAD, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 1);
    bread.spellCategory = CATEGORY_FOOD;
    input.items = {bread, Item(ITEM_SUPERIOR_HEALING, CLASS_CONSUMABLE, SUBCLASS_POTION, 1000, 1, 35),
                   Item(ITEM_SUPER_HEALING, CLASS_CONSUMABLE, SUBCLASS_POTION, 4000, 1, 55),
                   Item(ITEM_SUPERIOR_MANA, CLASS_CONSUMABLE, SUBCLASS_POTION, 1200, 1, 31)};
    input.vendorItems = {{VENDOR_INN, ITEM_BREAD, 0, 0, 0, true},
                         {VENDOR_APOTHECARY, ITEM_SUPERIOR_HEALING, 0, 0, 0, true},
                         {VENDOR_APOTHECARY, ITEM_SUPER_HEALING, 0, 0, 0, true},
                         {VENDOR_ALCHEMIST, ITEM_SUPERIOR_HEALING, 0, 0, 0, true},
                         {VENDOR_MANA, ITEM_SUPERIOR_MANA, 0, 0, 0, true}};
    UpkeepIndex index = UpkeepIndex::Build(std::move(input));
    index.version = 7;
    return index;
}

std::size_t UseNeedCount(NeedList const& list)
{
    std::size_t count = 0;
    for (Need const& need : list.needs)
        count += need.order == NeedOrder::Use;
    return count;
}

// Upkeep WU37a (WU.27): demand's food, water and poison wants replace the best offer by level; the order, due, reserve
// and funding stay the stock need's; a rogue's hands are one poison need whose items carry their hand's units
void DemandStockNeeds()
{
    std::cout << "Scenario: demand names the food and poison bought (WU37a), the need keeps due, reserve, funding\n";
    UpkeepIndex const index = Fixture();
    Snapshot bot = Bot(index, CLASS_ROGUE, 40, 100 * GOLD, 40);
    SetStock(bot, UpkeepStock::Food, 0);
    NeedRates const plain = Rates(40);
    NeedRates wants = plain;
    StockWant& poison = wants.stockWants[static_cast<std::size_t>(UpkeepStock::Poison)];
    poison = {true, 20, 0, 20, {ITEM_INSTANT_2, ITEM_DEADLY_1}, {10, 10}, 18};
    StockWant& food = wants.stockWants[static_cast<std::size_t>(UpkeepStock::Food)];
    food = {true, 20, 0, 20, {ITEM_GOOD_BREAD, ITEM_BREAD}, {20, 20}, 15};
    StockWant& water = wants.stockWants[static_cast<std::size_t>(UpkeepStock::Water)];
    water = {true, 20, 0, 20, {ITEM_ARROW}, {20}, 16};  // no water offer of the item: no need
    NeedList const old = ComputeNeeds(bot, index, plain);
    NeedList const with = Show("rogue 40, no food or poison, demand names its items", bot, index, wants);
    std::size_t poisons = 0;
    Need const* poisonNeed = nullptr;
    Need const* foodNeed = nullptr;
    bool water20 = false;
    for (Need const& need : with.needs)
    {
        poisons += need.stock == UpkeepStock::Poison ? 1 : 0;
        poisonNeed = need.stock == UpkeepStock::Poison ? &need : poisonNeed;
        foodNeed = need.stock == UpkeepStock::Food ? &need : foodNeed;
        water20 = water20 || need.stock == UpkeepStock::Water;
    }
    Check(poisons == 1 && poisonNeed && poisonNeed->units == 20 && poisonNeed->due &&
              poisonNeed->order == NeedOrder::ReagentPoison && poisonNeed->useItems.size() == 2 &&
              poisonNeed->item == ITEM_INSTANT_2 && poisonNeed->target == 20 && poisonNeed->keptGrade == 0,
          "one poison need for both hands with both hands' items, due, in the reagent and poison order");
    Check(foodNeed && foodNeed->due && foodNeed->order == NeedOrder::FoodWater && !water20 &&
              foodNeed->item == NamedOfferOf(index, bot, UpkeepStock::Food, food.items)->item,
          "food: the first of demand's items an offer fits, due; a want no water offer fits adds no need");
    Check(with.reserveStock > 0 && with.shortfall == 0 && foodNeed && foodNeed->funded == foodNeed->copper,
          "the stock needs are in the reserve and funded in the fixed order (a bot must eat)");
    std::vector<ShopLine> const at = PlanShopping(with, index, bot, VENDOR_POISON);
    ShopLine const* line = Line(at, UpkeepStock::Poison);
    Check(line && line->outcome == ShopOutcome::Buy && line->item == ITEM_INSTANT_2 && line->units == 10 &&
              CopperWithinFunded(at),
          "at the poison vendor: the main hand's item, its hand's 10 units, within the share");
    NeedRates unanswered = plain;
    unanswered.stockWants[static_cast<std::size_t>(UpkeepStock::Food)].items = {ITEM_GOOD_BREAD};
    NeedList const same = ComputeNeeds(bot, index, unanswered);
    bool equal = same.needs.size() == old.needs.size() && same.reserve == old.reserve;
    for (std::size_t i = 0; equal && i < old.needs.size(); ++i)
        equal = same.needs[i].item == old.needs[i].item && same.needs[i].units == old.needs[i].units;
    Check(equal, "a kind demand does not answer keeps the best offer by level (AddStockNeed)");
    StockLevels levels = LevelsOf(&index, bot);
    DemandLevels(levels, wants);
    Check(levels.basis[static_cast<std::size_t>(UpkeepStock::Food)] == DEMAND_STOCK_BASIS &&
              levels.count[static_cast<std::size_t>(UpkeepStock::Poison)] == 0 &&
              levels.basis[static_cast<std::size_t>(UpkeepStock::Ammo)] == LevelsOf(&index, bot).basis[2],
          "the use measure counts demand's held for the answered kinds, on demand's basis; the others unchanged");
}

void UseNeeds()
{
    std::cout << "Scenario: demand's buys (item use WI9b): last, never due, only from the gold above the reserve\n";
    UpkeepIndex const index = UseFixture();
    Snapshot bot = Bot(index, CLASS_WARRIOR, 60, 100 * GOLD, 60);
    SetStock(bot, UpkeepStock::Food, 0);
    NeedRates const plain = Rates(60);
    NeedRates wants = plain;
    wants.useWants.push_back({{ITEM_SUPER_HEALING, ITEM_SUPERIOR_HEALING}, 6, 6, 0});
    wants.useWants.push_back({{ITEM_UNSOLD_POTION}, 6, 6, 1});  // no vendor sells it: no need
    NeedList const without = ComputeNeeds(bot, index, plain);
    NeedList const with = Show("warrior 60, no food, wants 6 healing potions and an unsold one", bot, index, wants);
    Need const* use = Find(with, NeedOrder::Use);
    bool sameOthers = without.needs.size() + 1 == with.needs.size();
    for (std::size_t i = 0; sameOthers && i < without.needs.size(); ++i)
        sameOthers = with.needs[i].order == without.needs[i].order && with.needs[i].due == without.needs[i].due &&
                     with.needs[i].funded == without.needs[i].funded && with.needs[i].copper == without.needs[i].copper;
    Check(use && UseNeedCount(with) == 1 && &with.needs.back() == use && !use->due && use->item == ITEM_SUPER_HEALING &&
              use->units == 6 && use->vendor == VENDOR_APOTHECARY && use->stock == UpkeepStock::Use &&
              use->kind == UpkeepKind::Consumable,
          "a want with an offer is one Use need, last in the order, never due; a want no vendor sells adds none");
    Check(with.reserve == without.reserve && with.shortfall == without.shortfall && sameOthers &&
              !(PayableKinds(with, true) & StockBit(UpkeepStock::Use)),
          "the wants change no reserve, shortfall, earlier need or due kind");
    std::uint64_t const above = with.money > with.reserve ? with.money - with.reserve : 0;
    Check(use && use->funded == std::min<std::uint64_t>(use->copper, above),
          "the use need is funded from the gold above the reserve");

    // NR4 (item 17): the use need is no part of the reserve, so it never names the shortfall
    Snapshot broke = bot;
    broke.money = 0;
    NeedRates large = plain;
    large.useWants.push_back({{ITEM_SUPER_HEALING}, 1000, 1000, 0});
    NeedList const wanting = ComputeNeeds(broke, index, large);
    Need const* wanted = Find(wanting, NeedOrder::Use);
    std::uint64_t dearest = 0;
    for (Planner::NeedBridge::ReservePart const& part : Planner::NeedBridge::PartsOf(wanting))
        dearest = std::max<std::uint64_t>(dearest, Scaled(wanting, part.copper));
    Planner::NeedBridge::NeedInputs const named = Planner::NeedBridge::InputsOf(wanting);
    std::cout << "   broke, wants 1000 potions: " << named.shortText << "\n";
    Check(wanted && wanted->copper > dearest && named.shortfallCopper > 0 &&
              named.shortText.find("for use") == std::string::npos &&
              named.shortText.find("the reserve") == std::string::npos,
          "a broke bot with a large use want names its shortfall by a reserve part, not use");

    std::cout << "   at the apothecary, the alchemist (the weaker potion only), the mana vendor and the inn:\n";
    std::vector<ShopLine> const apothecary = PlanShopping(with, index, bot, VENDOR_APOTHECARY);
    std::vector<ShopLine> const alchemist = PlanShopping(with, index, bot, VENDOR_ALCHEMIST);
    std::vector<ShopLine> const mana = PlanShopping(with, index, bot, VENDOR_MANA);
    std::vector<ShopLine> const inn = PlanShopping(with, index, bot, VENDOR_INN);
    bool const within = CopperWithinFunded(apothecary) && CopperWithinFunded(alchemist) && CopperWithinFunded(mana) &&
                        CopperWithinFunded(inn);
    ShopLine const* strong = Line(apothecary, UpkeepStock::Use);
    ShopLine const* weak = Line(alchemist, UpkeepStock::Use);
    Check(strong && strong->outcome == ShopOutcome::Buy && strong->item == ITEM_SUPER_HEALING && strong->lots == 6 &&
              weak && weak->outcome == ShopOutcome::Buy && weak->item == ITEM_SUPERIOR_HEALING && weak->lots == 6 &&
              Line(mana, UpkeepStock::Use) && Line(mana, UpkeepStock::Use)->outcome == ShopOutcome::NotSold &&
              Line(inn, UpkeepStock::Use) && Line(inn, UpkeepStock::Use)->outcome == ShopOutcome::NotSold && within,
          "shopping buys the first of the want's items the vendor sells, never another use item, within the share");
    std::unordered_map<std::uint32_t, StockMask> const cover =
        VendorCover(index, bot, with, StockBit(UpkeepStock::Use));
    auto const covers = [&](std::uint32_t vendor)
    {
        auto const found = cover.find(vendor);
        return found != cover.end() && (found->second & StockBit(UpkeepStock::Use));
    };
    Check(covers(VENDOR_APOTHECARY) && covers(VENDOR_ALCHEMIST) && !covers(VENDOR_MANA) && !covers(VENDOR_INN),
          "the vendors that sell one of the want's items cover the use kind");

    Snapshot tight = bot;
    tight.money = static_cast<std::uint32_t>(with.reserve + 2000);  // half a Super Healing lot above the reserve
    NeedList const half = Show("the same warrior, half a potion's price above the reserve", tight, index, wants);
    Need const* halfUse = Find(half, NeedOrder::Use);
    ShopLine const* budget = Line(PlanShopping(half, index, tight, VENDOR_APOTHECARY), UpkeepStock::Use);
    bool othersFunded = true;
    for (Need const& need : half.needs)
        othersFunded = othersFunded && (need.order == NeedOrder::Use || need.funded == need.copper);
    Check(half.reserve == with.reserve && halfUse && halfUse->funded == 2000 && budget &&
              budget->outcome == ShopOutcome::Budget && othersFunded,
          "half a lot above the reserve: the earlier needs are paid, the potion skipped for the budget");
    Snapshot poor = bot;
    poor.money = static_cast<std::uint32_t>(with.reserve > 1 ? with.reserve - 1 : 0);
    NeedList const below = ComputeNeeds(poor, index, wants);
    Check(Find(below, NeedOrder::Use) && Find(below, NeedOrder::Use)->funded == 0,
          "below the reserve the use need gets nothing");
}

constexpr std::uint32_t ITEM_MARKET_CLOTH = 4306;
constexpr std::uint32_t ITEM_MARKET_BAG = 4500;

// Fund with a fault, the controls of MarketNeeds: the market's lines before the use items, or paid from all the gold
enum class FundFault
{
    MarketFirst,
    MarketFromAll
};

void FundVariant(NeedList& list, FundFault fault)
{
    auto const rank = [fault](NeedOrder order)
    { return fault == FundFault::MarketFirst && order == NeedOrder::Market ? -1 : static_cast<int>(order); };
    std::stable_sort(list.needs.begin(), list.needs.end(),
                     [&rank](Need const& a, Need const& b) { return rank(a.order) < rank(b.order); });
    std::uint64_t left = list.money;
    std::uint64_t above = list.money > list.reserve ? list.money - list.reserve : 0;
    for (Need& need : list.needs)
    {
        bool const aboveOnly =
            need.order == NeedOrder::Use || (need.order == NeedOrder::Market && fault != FundFault::MarketFromAll);
        need.funded = std::min(need.copper, aboveOnly ? std::min(left, above) : left);
        left -= need.funded;
        if (aboveOnly)
            above -= need.funded;
    }
}

std::uint64_t FundedOf(NeedList const& list, NeedOrder order)
{
    std::uint64_t funded = 0;
    for (Need const& need : list.needs)
        funded += need.order == order ? need.funded : 0;
    return funded;
}

// The reserve's parts as PartsOf counted them before the market (its control): every need with copper but repair,
// training, riding, pet, use and professions, and the four reserve sums
std::size_t PartsBeforeMarket(NeedList const& list)
{
    std::size_t parts = (list.reserveRepair ? 1 : 0) + (list.reserveTraining ? 1 : 0) + (list.reserveRiding ? 1 : 0) +
                        (list.reserveProfession ? 1 : 0);
    for (Need const& need : list.needs)
        parts += need.copper && need.order != NeedOrder::Repair && need.order != NeedOrder::Training &&
                         need.order != NeedOrder::Riding && need.order != NeedOrder::Pet &&
                         need.order != NeedOrder::Use && need.order != NeedOrder::Profession
                     ? 1
                     : 0;
    return parts;
}

void MarketNeeds()
{
    std::cout << "Scenario: the market's lines (auction house WA4a): after the professions, never due, only from the "
                 "gold above the reserve the use items leave\n";
    UpkeepIndex const index = UseFixture();
    NeedRates uses = Rates(60);
    uses.useWants.push_back({{ITEM_SUPER_HEALING, ITEM_SUPERIOR_HEALING}, 6, 6, 0});
    NeedRates markets = uses;
    markets.marketWants.push_back({ITEM_MARKET_CLOTH, 20, 3000});
    markets.marketWants.push_back({ITEM_MARKET_BAG, 1, 40000});
    markets.marketWants.push_back({ITEM_MARKET_CLOTH, 0, 100});  // no units: no need
    Snapshot bot = Bot(index, CLASS_WARRIOR, 60, 100 * GOLD, 60);
    SetStock(bot, UpkeepStock::Food, 0);
    NeedList const shown =
        Show("warrior 60, no food, wants potions, 20 cloth and a bag at the market", bot, index, markets);
    std::size_t const n = shown.needs.size();
    std::size_t marketCount = 0;
    for (Need const& need : shown.needs)
        marketCount += need.order == NeedOrder::Market ? 1 : 0;
    bool last = marketCount == 2 && n >= 3 && shown.needs[n - 3].order != NeedOrder::Market;
    for (std::size_t i = n >= 2 ? n - 2 : 0; last && i < n; ++i)
        last = shown.needs[i].order == NeedOrder::Market && !shown.needs[i].due &&
               shown.needs[i].kind == UpkeepKind::Gold && std::string(OrderWord(shown.needs[i].order)) == "market";
    Check(last, "two market needs (a want without units adds none), last in the order, never due");

    // Money from nothing to well above the reserve, the same bot with and without the market's lines
    NeedList const base = ComputeNeeds(bot, index, uses);
    std::uint64_t const top = base.reserve + 120000;
    int steps = 0;
    int partly = 0;
    int useLoses = 0;
    int overAbove = 0;
    int reserveMoves = 0;
    int partsMove = 0;
    int controlUse = 0;
    int controlAbove = 0;
    int controlParts = 0;
    for (std::uint64_t money = 0; money <= top; money += 997)
    {
        Snapshot rich = bot;
        rich.money = static_cast<std::uint32_t>(money);
        NeedList const without = ComputeNeeds(rich, index, uses);
        NeedList const with = ComputeNeeds(rich, index, markets);
        std::uint64_t const above = with.money > with.reserve ? with.money - with.reserve : 0;
        ++steps;
        std::uint64_t marketCopper = 0;
        for (Need const& need : with.needs)
            marketCopper += need.order == NeedOrder::Market ? need.copper : 0;
        std::uint64_t const marketFunded = FundedOf(with, NeedOrder::Market);
        partly += marketFunded && marketFunded < marketCopper ? 1 : 0;
        useLoses += FundedOf(with, NeedOrder::Use) != FundedOf(without, NeedOrder::Use) ? 1 : 0;
        overAbove += FundedOf(with, NeedOrder::Use) + marketFunded > above ? 1 : 0;
        bool sameOthers = without.needs.size() + 2 == with.needs.size();
        for (std::size_t i = 0; sameOthers && i < without.needs.size(); ++i)
            sameOthers = with.needs[i].order == without.needs[i].order &&
                         with.needs[i].funded == without.needs[i].funded &&
                         with.needs[i].copper == without.needs[i].copper && with.needs[i].due == without.needs[i].due;
        reserveMoves += with.reserve != without.reserve || with.shortfall != without.shortfall || !sameOthers ? 1 : 0;
        Planner::NeedBridge::NeedInputs const in = Planner::NeedBridge::InputsOf(with);
        Planner::NeedBridge::NeedInputs const out = Planner::NeedBridge::InputsOf(without);
        std::size_t const partsWithout = Planner::NeedBridge::PartsOf(without).size();
        partsMove += Planner::NeedBridge::PartsOf(with).size() != partsWithout ||
                             in.reserveCopper != out.reserveCopper || in.shortfallCopper != out.shortfallCopper ||
                             in.shortKind != out.shortKind || in.shortText != out.shortText
                         ? 1
                         : 0;

        NeedList first = with;
        FundVariant(first, FundFault::MarketFirst);
        controlUse += FundedOf(first, NeedOrder::Use) != FundedOf(without, NeedOrder::Use) ? 1 : 0;
        NeedList all = with;
        FundVariant(all, FundFault::MarketFromAll);
        controlAbove += FundedOf(all, NeedOrder::Use) + FundedOf(all, NeedOrder::Market) > above ? 1 : 0;
        controlParts += PartsBeforeMarket(with) != partsWithout ? 1 : 0;
    }
    std::cout << "   " << steps << " purses from 0 to the reserve + 12g (reserve " << Money(base.reserve) << "), "
              << partly << " with the market partly funded; counterexamples (rule / control): use " << useLoses << "/"
              << controlUse << ", above the reserve " << overAbove << "/" << controlAbove << ", reserve parts "
              << partsMove << "/" << controlParts << ", reserve and earlier needs " << reserveMoves << "\n";
    Check(partly > 0, "the sweep passes purses that fund the market's lines only in part");
    Check(useLoses == 0, "a use item's need never loses funding to the market's later lines");
    Check(controlUse > 0, "control: the market funded before the use items takes from them (caught)");
    Check(overAbove == 0, "the use items and the market's lines together stay within the gold above the reserve");
    Check(controlAbove > 0, "control: the market paid from all the gold reaches into the reserve (caught)");
    Check(reserveMoves == 0, "the market's lines change no reserve, shortfall or earlier need");
    Check(partsMove == 0, "the market's lines are no reserve part: the planner's reserve and short text unchanged");
    Check(controlParts > 0, "control: PartsOf without the market's exclusion adds them as parts (caught)");
}

// The market's kept copper (UpkeepNeeds::MarketKept, WA3-4 review N1): what the purse keeps from the auction visit and
// the stop's worth. Over a sweep of purses, the spendable gold (the purse above it) never reaches into what Fund paid a
// need before the market, and buys exactly the market's share. Its control, the reserve and the funded use items alone
// (WA4b), lets a spendthrift's small reserve and a riding purchase beyond the reserve be spent at the market
std::uint64_t MarketKeptOfWA4b(NeedList const& list) { return list.reserve + FundedOf(list, NeedOrder::Use); }

void MarketKeptNeeds(UpkeepIndex const& index)
{
    std::cout << "Scenario: the market keeps what Fund pays before it (WA3-4 review N1)\n";
    auto const withMarket = [](NeedRates rates)
    {
        rates.marketWants.push_back({ITEM_MARKET_CLOTH, 20, 3000});
        rates.marketWants.push_back({ITEM_MARKET_BAG, 1, 400000});
        return rates;
    };
    Snapshot spendthrift = Bot(index, CLASS_WARRIOR, 20, 0, 14);
    spendthrift.repairCost = 450;
    spendthrift.traits = Traits(TraitRules::MIN_VALUE, 0, false);
    // Riding bought for the want, its worth alone too small for the reserve: the purchase lies beyond the reserve
    Snapshot rider = Bot(index, CLASS_WARRIOR, 20, 0, 20);
    rider.traits = Traits(0, 0, true);
    struct Case
    {
        char const* name;
        Snapshot bot;
        NeedRates rates;
        int rule = 0;
        int control = 0;
        int funded = 0;
    };
    std::vector<Case> cases = {
        {"spendthrift warrior at 20, six levels untrained, repair due", spendthrift, withMarket(Rates(20))},
        {"collector warrior at 20 walking 3 min/h, riding bought beyond the reserve", rider,
         withMarket(Rates(20, 3.0f))}};
    for (Case& c : cases)
    {
        for (std::uint64_t money = 0; money <= 300 * GOLD; money += 4999)
        {
            Snapshot bot = c.bot;
            bot.money = static_cast<std::uint32_t>(money);
            NeedList const list = ComputeNeeds(bot, index, c.rates);
            std::uint64_t before = 0;
            std::uint64_t marketCopper = 0;
            for (Need const& need : list.needs)
            {
                before += need.order != NeedOrder::Market ? need.funded : 0;
                marketCopper += need.order == NeedOrder::Market ? need.copper : 0;
            }
            std::uint64_t const marketFunded = FundedOf(list, NeedOrder::Market);
            auto const wrong = [&](std::uint64_t kept)
            {
                std::uint64_t const spendable = money > kept ? money - kept : 0;
                return spendable + before > money || std::min(spendable, marketCopper) != marketFunded;
            };
            c.rule += wrong(MarketKept(list)) ? 1 : 0;
            c.control += wrong(MarketKeptOfWA4b(list)) ? 1 : 0;
            c.funded += before > list.reserve + FundedOf(list, NeedOrder::Use) ? 1 : 0;
        }
        std::cout << "   " << c.name << ": " << c.funded << " purses where earlier needs take more than the reserve "
                  << "and the use items; counterexamples (rule / control) " << c.rule << "/" << c.control << "\n";
        Check(c.rule == 0, std::string(c.name) + ": the market spends only its share, never an earlier need's");
        Check(c.control > 0, std::string(c.name) + ": control: the reserve and the use items alone (caught)");
    }
}

// NR3 (NR.4): a sick hunter's trip takes no errand that fights (the tame, the stable's swap) and no stop or placed
// errand outside its safe area; the same hunter healthy keeps the tame and the sale (its trip never calls the filter)
void SickTrip()
{
    using WorldTravel::ErrandKind;
    std::vector<WorldTravel::ErrandOffer> offers(3);
    offers[0].Kind = ErrandKind::Sell;
    offers[0].Service = WorldTravel::SERVICE_VENDOR;
    offers[1].Kind = ErrandKind::Tame;
    offers[1].Placed = true;
    offers[1].Place = {0, 10.0f, 10.0f, 0.0f};  // inside the safe area: the fight alone leaves it out
    offers[1].Due = true;
    offers[2].Kind = ErrandKind::TurnIn;
    offers[2].Placed = true;
    offers[2].Place = {0, 900.0f, 900.0f, 0.0f};
    std::vector<WorldTravel::ServiceStop> stops(2);
    stops[0].Id = 1;
    stops[0].Place = {0, 5.0f, 5.0f, 0.0f};
    stops[0].Services = WorldTravel::SERVICE_VENDOR;
    stops[1].Id = 2;
    stops[1].Place = {0, 800.0f, 800.0f, 0.0f};
    stops[1].Services = WorldTravel::SERVICE_VENDOR;
    auto const safeAt = [](WorldTravel::ErrandPlace const& place) { return place.X < 100.0f && place.Y < 100.0f; };
    auto const has = [](std::vector<WorldTravel::ErrandOffer> const& list, ErrandKind kind)
    {
        return std::any_of(list.begin(), list.end(),
                           [kind](WorldTravel::ErrandOffer const& offer) { return offer.Kind == kind; });
    };
    std::vector<WorldTravel::ErrandOffer> sick = offers;
    std::vector<WorldTravel::ServiceStop> sickStops = stops;
    WorldTravel::KeepSickErrands(sickStops, sick, safeAt);
    Check(has(offers, ErrandKind::Tame) && has(offers, ErrandKind::Sell) && stops.size() == 2,
          "a healthy hunter with a tame due keeps the tame stop and the sale");
    Check(!has(sick, ErrandKind::Tame) && has(sick, ErrandKind::Sell) && !has(sick, ErrandKind::TurnIn) &&
              sickStops.size() == 1 && sickStops.front().Id == 1,
          "a sick hunter's trip has no tame stop and no stop outside its safe area (mutant: the tame stays)");
    Check(WorldTravel::ErrandStartsFight(ErrandKind::Tame) && WorldTravel::ErrandStartsFight(ErrandKind::Stable) &&
              !WorldTravel::ErrandStartsFight(ErrandKind::Sell) && !WorldTravel::ErrandStartsFight(ErrandKind::Repair),
          "the tame and the stable's swap start a fight, a sale and a repair do not");
}

int main(int argc, char** argv)
{
    std::uint64_t const seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2811;
    std::cout << "upkeep needs selftest, seed " << seed << "\n";
    std::cout << "Tuning [E]:\n";
    for (std::string const& line : TuningLines())
        std::cout << "  " << line << "\n";
    std::cout << "  riding horizon: " << RidingHorizonHours(20) << " h at 20, " << RidingHorizonHours(40)
              << " h at 40, " << RidingHorizonHours(70) << " h at 70, " << RidingHorizonHours(80) << " h at 80\n";
    UpkeepIndex const index = Fixture();
    RepairBeforeFood(index);
    CookedFood(index);
    RepairDueRule();
    PartialRepair(index);
    AmmoAndWater(index);
    PetDiet(index);
    Training(index);
    TrainingGold(index);
    TrainingBuying(index);
    GroundRiding(index);
    RidingPlaces(index);
    TrainingPlaces(index);
    WeaponMasters();
    ProfessionNeeds();
    PetNeed();
    PetSwap();
    TameSteps();
    TameSiteRules();
    SickTrip();
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
    KeptRule const shared = [&index](Snapshot const& bot, NeedRates const& rates, StockItem const& entry)
    { return KeepOf(&index, bot, rates, entry).keep; };
    StockRuleInvariant(index, seed, shared, "the stock rule (KeepOf)", true);
    DemandKinds(index);
    UseNeeds();
    MarketNeeds();
    MarketKeptNeeds(index);
    DemandStockNeeds();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
