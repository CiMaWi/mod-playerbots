/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the upkeep index (src/Ai/World/Rpg/UpkeepIndex.h, upkeep plan WU2) over a small synthetic gather:
// the item rules, trainer dedupe and lists, dense positions and bit sets, vendor offers per kind with prices,
// determinism, publication under reader threads, the memory estimate and learn spells (known by what they teach).
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/upkeep-index-selftest.cpp

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "PlannerIndexes.h"
#include "UpkeepIndex.h"

using namespace Planner;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& what)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << what << "\n";
}

constexpr std::uint8_t CLASS_WARRIOR = 1;
constexpr std::uint8_t CLASS_PALADIN = 2;
constexpr std::uint8_t CLASS_ROGUE = 4;
constexpr std::uint8_t CLASS_MAGE = 8;
constexpr std::uint8_t RACE_HUMAN = 1;

constexpr std::uint32_t TRAINER_MAGE = 100;       // keys: the lowest creature entry of each trainer
constexpr std::uint32_t TRAINER_MAGE_CITY = 110;  // a second mage trainer, more spells
constexpr std::uint32_t TRAINER_WARRIOR = 200;
constexpr std::uint32_t TRAINER_RIDING = 300;
constexpr std::uint32_t TRAINER_PET = 400;
constexpr std::uint32_t TRAINER_PALADIN = 500;

constexpr std::uint32_t VENDOR_INN = 5000;      // food and water
constexpr std::uint32_t VENDOR_ARMS = 5001;     // ammo, a poison, a currency-only item
constexpr std::uint32_t VENDOR_REAGENT = 5002;  // mage reagents and a mount
constexpr std::uint32_t VENDOR_NONE = 5003;     // sells only an item no rule takes

constexpr std::uint32_t ITEM_BREAD = 4540;
constexpr std::uint32_t ITEM_WATER = 159;
constexpr std::uint32_t ITEM_ARROW = 2512;
constexpr std::uint32_t ITEM_POISON = 6947;
constexpr std::uint32_t ITEM_POWDER = 17020;
constexpr std::uint32_t ITEM_RUNE = 17031;
constexpr std::uint32_t ITEM_HORSE = 2414;
constexpr std::uint32_t ITEM_BADGE_ARROW = 30319;  // sold for currency only
constexpr std::uint32_t ITEM_SWORD = 2488;
constexpr std::uint32_t ITEM_WITHOUT_FACTS = 99999;

constexpr std::uint32_t SPELL_FROSTBOLT = 116;
constexpr std::uint32_t SPELL_ARCANE_INTELLECT = 1459;
constexpr std::uint32_t SPELL_TELEPORT = 3561;
constexpr std::uint32_t SPELL_BLINK = 1953;
constexpr std::uint32_t SPELL_HEROIC_STRIKE = 78;
constexpr std::uint32_t SPELL_APPRENTICE_RIDING = 33388;
constexpr std::uint32_t SPELL_COLD_WEATHER_FLYING = 54197;
constexpr std::uint32_t SPELL_JUDGEMENT = 10321;  // a learn spell: the bot only ever has what it teaches
constexpr std::uint32_t SPELL_JUDGEMENT_TAUGHT = 20271;
constexpr std::uint32_t SPELL_TWO_FOLD = 70001;  // a synthetic learn spell teaching two spells
constexpr std::uint32_t SPELL_TWO_FOLD_A = 70002;
constexpr std::uint32_t SPELL_TWO_FOLD_B = 70003;
constexpr std::uint32_t SPELL_SEAL = 21084;  // teaches nothing

TrainerSpell Spell(std::uint32_t id, std::uint32_t cost, std::uint8_t level) { return {id, cost, 0, 0, {}, level, {}}; }

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

UpkeepGather Synthetic()
{
    using namespace UpkeepItem;
    UpkeepGather input;
    // The city trainer first: the build orders by key, not by gather order
    input.trainers.push_back({TRAINER_MAGE_CITY,
                              TrainerKind::Class,
                              CLASS_MAGE,
                              {Spell(SPELL_TELEPORT, 2000, 20), Spell(SPELL_BLINK, 1500, 20),
                               Spell(SPELL_FROSTBOLT, 999, 4)}});  // Frostbolt at other terms: a conflict
    input.trainers.push_back({TRAINER_MAGE,
                              TrainerKind::Class,
                              CLASS_MAGE,
                              {Spell(SPELL_FROSTBOLT, 100, 4), Spell(SPELL_ARCANE_INTELLECT, 10, 1)}});
    input.trainers.push_back({TRAINER_MAGE, TrainerKind::Class, CLASS_MAGE, {}});  // the same trainer again
    input.trainers.push_back({TRAINER_WARRIOR, TrainerKind::Class, CLASS_WARRIOR, {Spell(SPELL_HEROIC_STRIKE, 10, 1)}});
    input.trainers.push_back(
        {TRAINER_RIDING,
         TrainerKind::Mount,
         0,
         {Spell(SPELL_COLD_WEATHER_FLYING, 10000000, 77), Spell(SPELL_APPRENTICE_RIDING, 40000, 20)}});
    input.trainers.push_back({TRAINER_PET, TrainerKind::Pet, 0, {Spell(7, 0, 10)}});
    input.trainerEntries = {{TRAINER_MAGE, TRAINER_MAGE},           {TRAINER_MAGE + 1, TRAINER_MAGE},
                            {TRAINER_MAGE_CITY, TRAINER_MAGE_CITY}, {TRAINER_WARRIOR, TRAINER_WARRIOR},
                            {TRAINER_RIDING, TRAINER_RIDING},       {TRAINER_PET, TRAINER_PET},
                            {TRAINER_MAGE + 1, TRAINER_MAGE},       {9000, 12345}};  // a repeat and an orphan

    UpkeepItemFacts bread = Item(ITEM_BREAD, CLASS_CONSUMABLE, SUBCLASS_FOOD, 25, 1, 5);
    bread.spellCategory = CATEGORY_FOOD;
    bread.foodType = 4;  // bread
    UpkeepItemFacts water = Item(ITEM_WATER, CLASS_CONSUMABLE, SUBCLASS_FOOD, 5, 1, 1);
    water.spellCategory = CATEGORY_DRINK;
    UpkeepItemFacts arrow = Item(ITEM_ARROW, CLASS_PROJECTILE, SUBCLASS_ARROW, 10, 200, 1);
    UpkeepItemFacts poison = Item(ITEM_POISON, CLASS_CONSUMABLE, SUBCLASS_CONSUMABLE_OTHER, 25, 1, 20);
    poison.allowableClass = CLASS_MASK_ROGUE;
    UpkeepItemFacts powder = Item(ITEM_POWDER, CLASS_MISC, 1, 1000, 1, 0);
    UpkeepItemFacts rune = Item(ITEM_RUNE, CLASS_MISC, 1, 1000, 1, 0);
    UpkeepItemFacts horse = Item(ITEM_HORSE, CLASS_MISC, SUBCLASS_MOUNT, 10000, 1, 20);
    horse.allowableRace = 1u << (RACE_HUMAN - 1);
    horse.requiredSkillRank = 75;
    UpkeepItemFacts badgeArrow = Item(ITEM_BADGE_ARROW, CLASS_PROJECTILE, SUBCLASS_ARROW, 0, 1000, 60);
    UpkeepItemFacts sword = Item(ITEM_SWORD, 2, 7, 5000, 1, 10);
    input.items = {bread, water, arrow, poison, powder, rune, horse, badgeArrow, sword};
    input.vendorItems = {
        {VENDOR_INN, ITEM_BREAD, 0, 0, 0, true},
        {VENDOR_INN, ITEM_WATER, 0, 0, 0, true},
        {VENDOR_INN, ITEM_WATER, 0, 0, 0, true},  // listed twice: one offer
        {VENDOR_ARMS, ITEM_ARROW, 0, 0, 0, true},
        {VENDOR_ARMS, ITEM_POISON, 5, 3600, 0, true},
        {VENDOR_ARMS, ITEM_BADGE_ARROW, 0, 0, 2400, false},
        {VENDOR_ARMS, ITEM_WITHOUT_FACTS, 0, 0, 0, true},
        {VENDOR_REAGENT, ITEM_POWDER, 0, 0, 0, true},
        {VENDOR_REAGENT, ITEM_RUNE, 0, 0, 0, true},
        {VENDOR_REAGENT, ITEM_HORSE, 0, 0, 0, true},
        {VENDOR_NONE, ITEM_SWORD, 0, 0, 0, true},
    };
    input.reagents = {{ITEM_POWDER, CLASS_MAGE}, {ITEM_RUNE, CLASS_MAGE}, {ITEM_POWDER, CLASS_MAGE}};
    input.emptyVendors = 1;
    input.missingItems = 2;
    return input;
}

void CheckItemRules()
{
    using namespace UpkeepItem;
    UpkeepItemFacts food = Item(1, CLASS_CONSUMABLE, SUBCLASS_FOOD, 1, 1, 1);
    food.spellCategory = CATEGORY_FOOD;
    Check(StockKinds(food) == StockBit(UpkeepStock::Food), "food: consumable food with the food category");
    food.foodType = 1;
    Check(StockKinds(food) == (StockBit(UpkeepStock::Food) | StockBit(UpkeepStock::PetFood)),
          "food with a food type is also pet food");
    Check(DietBit(1) == 1 && DietBit(8) == 0x80 && DietBit(0) == 0 && DietBit(9) == 0, "diet bits are FoodType - 1");
    UpkeepItemFacts drink = Item(2, CLASS_CONSUMABLE, SUBCLASS_CONSUMABLE, 1, 1, 1);
    drink.spellCategory = CATEGORY_DRINK;
    Check(StockKinds(drink) == StockBit(UpkeepStock::Water), "water: plain consumables with the drink category too");
    UpkeepItemFacts potion = Item(3, CLASS_CONSUMABLE, 1, 1, 1, 1);
    potion.spellCategory = CATEGORY_FOOD;
    Check(StockKinds(potion) == 0, "a potion is no stock kind");
    Check(StockKinds(Item(4, CLASS_PROJECTILE, SUBCLASS_BULLET, 1, 1, 1)) == StockBit(UpkeepStock::Ammo), "bullets");
    UpkeepItemFacts oil = Item(5, CLASS_CONSUMABLE, SUBCLASS_CONSUMABLE_OTHER, 1, 1, 1);
    Check(StockKinds(oil) == 0, "an item for every class in the poison subclass is not a poison");
    oil.allowableClass = CLASS_MASK_ROGUE | 0xFFFFF800;  // bits beyond the playable classes are ignored
    Check(StockKinds(oil) == StockBit(UpkeepStock::Poison), "rogue-only items of the subclass are poisons");
    Check(StockKinds(Item(6, CLASS_MISC, SUBCLASS_MOUNT, 1, 1, 1)) == StockBit(UpkeepStock::Mount), "mounts");
    Check(StockKinds(Item(7, CLASS_MISC, 1, 1, 1, 1)) == 0, "a reagent needs the class reagent list");

    VendorOffer arrows;
    arrows.buyPrice = 10;
    arrows.buyCount = 200;
    Check(
        arrows.PriceFor(1) == 10 && arrows.PriceFor(200) == 10 && arrows.PriceFor(201) == 20 && arrows.PriceFor(0) == 0,
        "prices are whole lots of buyCount at the game's price");
}

void CheckTrainers(UpkeepIndex const& index)
{
    UpkeepStats const& s = index.stats;
    Check(s.duplicateTrainers == 1, "the repeated trainer is dropped once");
    Check(index.trainers.size() == 5 && s.trainers[0] == 3 && s.trainers[1] == 1 && s.trainers[3] == 1,
          "trainers per kind: 3 class, 1 riding, 1 pet");
    TrainerList const* mage = index.ClassList(CLASS_MAGE);
    Check(mage && mage->spells.size() == 4 && s.classSpells[CLASS_MAGE] == 4,
          "the mage list is the union of both mage trainers");
    bool sorted = mage != nullptr;
    for (std::size_t i = 1; mage && i < mage->spells.size(); ++i)
        sorted = sorted && mage->spells[i - 1].spellId < mage->spells[i].spellId;
    Check(sorted, "a list is sorted by spell id");
    std::size_t const frostbolt = mage ? mage->Find(SPELL_FROSTBOLT) : TrainerList::NOT_FOUND;
    Check(frostbolt != TrainerList::NOT_FOUND && mage->spells[frostbolt].moneyCost == 100 && s.spellConflicts == 1,
          "a spell at two terms keeps the lower key's and counts a conflict");
    Check(mage && mage->Find(SPELL_HEROIC_STRIKE) == TrainerList::NOT_FOUND, "no warrior spell in the mage list");
    Check(index.ClassList(CLASS_WARRIOR) && s.classSpells[CLASS_WARRIOR] == 1 && !index.ClassList(CLASS_ROGUE),
          "a class without trainers has no list");
    TrainerList const* riding = index.RidingList(RACE_HUMAN);
    Check(riding && riding->kind == TrainerKind::Mount && riding->spells.size() == 2 &&
              riding->spells[riding->Find(SPELL_COLD_WEATHER_FLYING)].moneyCost == 10000000,
          "a race without its own riding trainer gets the any-race list, at the game's price");

    UpkeepTrainer const& first = index.trainers.front();
    Check(first.key == TRAINER_MAGE && first.entries == std::vector<std::uint32_t>{TRAINER_MAGE, TRAINER_MAGE + 1},
          "trainers by key; the creature entries joined, repeats dropped");
    UpkeepTrainer const& city = index.trainers[1];
    bool positions = mage && city.spells.size() == 3 && &index.lists[city.list] == mage;
    for (std::uint16_t const position : city.spells)
        positions = positions && position < mage->spells.size();
    Check(positions && mage->spells[city.spells.back()].spellId == SPELL_TELEPORT,
          "a trainer's spells are positions in its list");
    Check(s.orphanEntries == 1 && s.trainerEntries == 6, "an entry naming no trainer is counted, not joined");

    if (!mage)
        return;
    std::vector<std::uint64_t> bits = SpellBits(mage->spells.size());
    std::vector<std::uint32_t> const known = {SPELL_ARCANE_INTELLECT, SPELL_TELEPORT};
    for (std::uint32_t const spell : known)
        SetSpellBit(bits, mage->Find(spell));
    std::vector<std::uint32_t> back;
    for (std::size_t position = 0; position < mage->spells.size(); ++position)
        if (HasSpellBit(bits, position))
            back.push_back(mage->spells[position].spellId);
    Check(bits.size() == 1 && back == known, "known spells -> bits -> the same spells");
    std::vector<std::uint64_t> wide = SpellBits(130);
    SetSpellBit(wide, 129);
    SetSpellBit(wide, 500);  // beyond the list: ignored
    Check(wide.size() == 3 && HasSpellBit(wide, 129) && !HasSpellBit(wide, 128) && !HasSpellBit(wide, 500),
          "bits beyond one word");
}

void CheckOffers(UpkeepIndex const& index)
{
    UpkeepStats const& s = index.stats;
    auto const kind = [](UpkeepStock stock) { return static_cast<std::size_t>(stock); };
    Check(index.Offers(UpkeepStock::Food).size() == 1 && index.Offers(UpkeepStock::PetFood).size() == 1 &&
              index.Offers(UpkeepStock::PetFood).front().diet == DietBit(4),
          "bread is food and pet food, with its diet bit");
    Check(index.Offers(UpkeepStock::Water).size() == 1, "water listed twice at one vendor is one offer");
    Check(index.Offers(UpkeepStock::Ammo).size() == 1 && s.currencyOnly == 1,
          "currency-only arrows are skipped and counted");
    VendorOffer const& poison = index.Offers(UpkeepStock::Poison).front();
    Check(index.Offers(UpkeepStock::Poison).size() == 1 && poison.limited && poison.buyPrice == 25,
          "the poison at its price, its limited stock flagged");
    std::vector<VendorOffer> const& reagents = index.Offers(UpkeepStock::Reagent);
    Check(
        reagents.size() == 2 && reagents.front().classMask == UpkeepIndex::ClassBit(CLASS_MAGE) && s.reagentItems == 2,
        "mage reagents with the class mask of their users");
    std::vector<VendorOffer> const& mounts = index.Offers(UpkeepStock::Mount);
    Check(mounts.size() == 1 && mounts.front().raceMask == 1u && mounts.front().requiredSkillRank == 75,
          "the mount with its race and riding rank");
    Check(s.vendors[kind(UpkeepStock::Food)] == 1 && s.vendors[kind(UpkeepStock::Reagent)] == 1 &&
              s.items[kind(UpkeepStock::Reagent)] == 2 && s.vendors[kind(UpkeepStock::Ammo)] == 1,
          "vendors and items per kind");
    Check(s.unknownItems == 1 && s.emptyVendors == 1 && s.missingItems == 2,
          "rows without facts and the gather's counts are reported");
    std::size_t total = 0;
    for (std::vector<VendorOffer> const& offers : index.offers)
        for (VendorOffer const& offer : offers)
            total += offer.vendorEntry == VENDOR_NONE ? 1 : 0;
    Check(total == 0, "items no rule takes give no offer");

    UpkeepItemFacts powder = Item(ITEM_POWDER, UpkeepItem::CLASS_MISC, 1, 1000, 1, 0);
    Check(index.KindsOf(powder, CLASS_MAGE) == StockBit(UpkeepStock::Reagent) &&
              index.KindsOf(powder, CLASS_WARRIOR) == 0,
          "a reagent counts as stock only for the classes that use it");
}

bool SameIndex(UpkeepIndex const& a, UpkeepIndex const& b)
{
    if (a.lists.size() != b.lists.size() || a.trainers.size() != b.trainers.size() ||
        a.reagentClasses != b.reagentClasses)
        return false;
    for (std::size_t i = 0; i < a.lists.size(); ++i)
        if (a.lists[i].kind != b.lists[i].kind || a.lists[i].requirement != b.lists[i].requirement ||
            !(a.lists[i].spells == b.lists[i].spells))
            return false;
    for (std::size_t i = 0; i < a.trainers.size(); ++i)
        if (a.trainers[i].key != b.trainers[i].key || a.trainers[i].entries != b.trainers[i].entries ||
            a.trainers[i].spells != b.trainers[i].spells)
            return false;
    for (std::size_t kind = 0; kind < UPKEEP_STOCK_COUNT; ++kind)
    {
        if (a.offers[kind].size() != b.offers[kind].size())
            return false;
        for (std::size_t i = 0; i < a.offers[kind].size(); ++i)
            if (a.offers[kind][i].item != b.offers[kind][i].item ||
                a.offers[kind][i].vendorEntry != b.offers[kind][i].vendorEntry)
                return false;
    }
    return true;
}

void CheckPublication()
{
    constexpr int READERS = 4;
    constexpr int PUBLISHES = 500;
    SharedSlot<UpkeepIndex> book;
    auto first = std::make_shared<UpkeepIndex>(UpkeepIndex::Build(Synthetic()));
    first->version = 1;
    book.Publish(first);
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < READERS; ++r)
        readers.emplace_back(
            [&]
            {
                while (!done)
                {
                    std::shared_ptr<UpkeepIndex const> const held = book.Current();
                    TrainerList const* mage = held ? held->ClassList(CLASS_MAGE) : nullptr;
                    if (!mage || mage->spells.size() != 4 || held->version == 0)
                        ++torn;
                    ++reads;
                }
            });
    for (int i = 0; i < PUBLISHES; ++i)
    {
        auto next = std::make_shared<UpkeepIndex>(UpkeepIndex::Build(Synthetic()));
        next->version = static_cast<std::uint32_t>(i + 2);
        book.Publish(std::move(next));
    }
    done = true;
    for (std::thread& t : readers)
        t.join();
    Check(torn == 0 && reads > 0,
          "4 readers x 500 publishes: no torn or null read (" + std::to_string(reads.load()) + " reads)");
    std::shared_ptr<UpkeepIndex const> held = book.Current();
    book.Publish(std::make_shared<UpkeepIndex const>(UpkeepIndex::Build(UpkeepGather{})));
    Check(held->ClassList(CLASS_MAGE) != nullptr && book.Current()->lists.empty(),
          "a reader keeps the old index alive after a new publish");
}
// A mount sold for gold and currency is kept with its extended cost; a gold-only one is GoldOnly
void CheckExtendedCost()
{
    using namespace UpkeepItem;
    constexpr std::uint32_t ITEM_GOLD_MOUNT = 18776;
    constexpr std::uint32_t ITEM_CURRENCY_MOUNT = 44690;
    constexpr std::uint32_t EXTENDED_COST = 2400;
    UpkeepGather input;
    UpkeepItemFacts goldMount = Item(ITEM_GOLD_MOUNT, CLASS_MISC, SUBCLASS_MOUNT, 1000000, 1, 70);
    goldMount.requiredSkillRank = 300;
    UpkeepItemFacts currencyMount = Item(ITEM_CURRENCY_MOUNT, CLASS_MISC, SUBCLASS_MOUNT, 10000, 1, 70);
    currencyMount.requiredSkillRank = 300;
    input.items = {goldMount, currencyMount};
    input.vendorItems = {{VENDOR_REAGENT, ITEM_GOLD_MOUNT, 0, 0, 0, true},
                         {VENDOR_REAGENT, ITEM_CURRENCY_MOUNT, 0, 0, EXTENDED_COST, true}};
    UpkeepIndex const index = UpkeepIndex::Build(std::move(input));
    std::vector<VendorOffer> const& mounts = index.Offers(UpkeepStock::Mount);
    bool goldOnly = false;
    bool withCurrency = false;
    for (VendorOffer const& offer : mounts)
    {
        goldOnly = goldOnly || (offer.item == ITEM_GOLD_MOUNT && offer.GoldOnly() && !offer.extendedCost);
        withCurrency = withCurrency ||
                       (offer.item == ITEM_CURRENCY_MOUNT && !offer.GoldOnly() && offer.extendedCost == EXTENDED_COST);
    }
    Check(mounts.size() == 2 && goldOnly && withCurrency && index.stats.currencyOnly == 0,
          "a gold+currency row is kept with its extended cost, not gold only; a gold row is gold only");
}
// Judgement 10321 teaches 20271 (lab7-20261110-153800: paladins knew 20271 and still listed 10321 as a need): the list
// keeps what a spell teaches and finds its teacher, and the spell is known as Trainer::GetDefaultSpellState says
void CheckTeaches()
{
    UpkeepGather input;
    TrainerSpell judgement = Spell(SPELL_JUDGEMENT, 100, 4);
    judgement.teaches[0] = SPELL_JUDGEMENT_TAUGHT;
    TrainerSpell twoFold = Spell(SPELL_TWO_FOLD, 100, 4);
    twoFold.teaches = {SPELL_TWO_FOLD_A, 0, SPELL_TWO_FOLD_B};
    input.trainers.push_back(
        {TRAINER_PALADIN, TrainerKind::Class, CLASS_PALADIN, {twoFold, judgement, Spell(SPELL_SEAL, 10, 1)}});
    input.trainerEntries = {{TRAINER_PALADIN, TRAINER_PALADIN}};
    UpkeepIndex const index = UpkeepIndex::Build(std::move(input));
    TrainerList const* list = index.ClassList(CLASS_PALADIN);
    if (!list || list->spells.size() != 3)
    {
        Check(false, "the paladin list has its three spells");
        return;
    }
    std::size_t const position = list->Find(SPELL_JUDGEMENT);
    std::size_t const twoFoldAt = list->Find(SPELL_TWO_FOLD);
    Check(position != TrainerList::NOT_FOUND && list->spells[position].teaches[0] == SPELL_JUDGEMENT_TAUGHT &&
              list->Teacher(SPELL_JUDGEMENT_TAUGHT) == position && list->Teacher(SPELL_TWO_FOLD_B) == twoFoldAt &&
              list->Teacher(SPELL_JUDGEMENT) == TrainerList::NOT_FOUND &&
              list->Teacher(SPELL_SEAL) == TrainerList::NOT_FOUND && list->taughtBy.size() == 3,
          "a learn spell keeps what it teaches; the list finds the teacher of a taught spell, of nothing else");
    Check(index.stats.estimatedBytes == index.ApproxBytes(), "the memory estimate counts the taught spells");

    auto const knows = [&list](std::uint32_t spellId, std::vector<std::uint32_t> const& has)
    {
        auto const hasSpell = [&has](std::uint32_t id) { return std::find(has.begin(), has.end(), id) != has.end(); };
        return KnowsTrainerSpell(list->spells[list->Find(spellId)], hasSpell);
    };
    Check(!knows(SPELL_JUDGEMENT, {}) && knows(SPELL_JUDGEMENT, {SPELL_JUDGEMENT_TAUGHT}) &&
              knows(SPELL_JUDGEMENT, {SPELL_JUDGEMENT}),
          "a learn spell is known by the spell it teaches, or by itself");
    Check(!knows(SPELL_TWO_FOLD, {SPELL_TWO_FOLD_A}) && !knows(SPELL_TWO_FOLD, {SPELL_TWO_FOLD_B}) &&
              knows(SPELL_TWO_FOLD, {SPELL_TWO_FOLD_B, SPELL_TWO_FOLD_A}),
          "a spell teaching two is known only when both are");
    Check(!knows(SPELL_SEAL, {SPELL_JUDGEMENT_TAUGHT}) && knows(SPELL_SEAL, {SPELL_SEAL}),
          "a spell teaching nothing is known only by itself");
}
}  // namespace

int main()
{
    CheckExtendedCost();
    CheckTeaches();
    CheckItemRules();
    UpkeepIndex const index = UpkeepIndex::Build(Synthetic());
    CheckTrainers(index);
    CheckOffers(index);
    Check(SameIndex(index, UpkeepIndex::Build(Synthetic())), "two builds of one gather are equal");
    Check(index.stats.estimatedBytes > 0 && index.stats.estimatedBytes == index.ApproxBytes(), "the memory estimate");
    UpkeepIndex const empty = UpkeepIndex::Build(UpkeepGather{});
    Check(empty.lists.empty() && !empty.ClassList(CLASS_MAGE) && !empty.RidingList(RACE_HUMAN) &&
              empty.Offers(UpkeepStock::Food).empty(),
          "an empty gather builds an empty index");
    CheckPublication();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
