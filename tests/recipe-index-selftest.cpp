/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the recipe index (src/Ai/World/Rpg/RecipeIndex.h, professions plan WR3a) on a small hand-made
// gather: skill thresholds, reagents, tools and station, the three ways a recipe is learned (trainer, an item that is
// sold or found, the skill itself), a rank ladder, the loot stores' expected yields, "what an item is made from", and
// the dump read back unchanged. WR3b adds checks on the dumped fixture (tests/fixtures/recipe-index-dump.txt, written
// by `playerbots professions index dump` on a lab): every profession has its old-world ladder to 300, every reagent is
// a known item, no recipe is its own input, and WR2's table of crafts by class holds.
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ProfessionRules.h"
#include "RecipeIndex.h"

using namespace Recipes;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << '\n';
    }
}

constexpr std::uint16_t ALCHEMY = 171;
constexpr std::uint16_t BLACKSMITHING = 164;

SpellRow Spell(std::uint32_t id, std::string name, Effect effect = Effect::Make, std::uint32_t product = 0)
{
    SpellRow row;
    row.spell = id;
    row.name = std::move(name);
    row.effect = effect;
    row.product = product;
    row.productCount = product ? 1 : 0;
    row.castMs = 3000;
    return row;
}

AbilityRow Ability(std::uint16_t skill, std::uint32_t spell, std::uint32_t orange, std::uint32_t yellow,
                   std::uint32_t grey, std::uint32_t acquire)
{
    AbilityRow row;
    row.skill = skill;
    row.spell = spell;
    row.minRank = orange;
    row.yellow = yellow;
    row.grey = grey;
    row.acquire = acquire;
    return row;
}

ItemFact Item(std::uint32_t id, std::string name, bool known = true)
{
    ItemFact fact;
    fact.id = id;
    fact.known = known;
    fact.name = std::move(name);
    return fact;
}

Gather HandGather()
{
    Gather g;
    // 1000: an elixir taught by a trainer through a wrapper spell (1001), two reagents
    g.abilities.push_back(Ability(ALCHEMY, 1000, 90, 110, 130, ACQUIRE_ON_SKILL_VALUE));
    g.spells.push_back(Spell(1000, "Elixir of Testing", Effect::Make, 9001));
    g.spells.back().reagents = {{2447, 2}, {3371, 1}};
    g.trainers.push_back({100, 1001, 1000, 500, ALCHEMY, 90, 10});
    g.trainers.push_back({101, 1001, 1000, 400, ALCHEMY, 90, 10});  // a second, cheaper trainer
    // 3000: learned from an item that two vendors sell
    g.abilities.push_back(Ability(ALCHEMY, 3000, 150, 170, 190, 0));
    g.spells.push_back(Spell(3000, "Potion of Recipes", Effect::Make, 9001));
    g.spells.back().reagents = {{2447, 3}};
    g.recipeItems.push_back({6000, 3000, 2, 2000});
    // 4000: learned from an item found in the world (no vendor)
    g.abilities.push_back(Ability(ALCHEMY, 4000, 200, 215, 230, 0));
    g.spells.push_back(Spell(4000, "Flask of Finding", Effect::Make, 9002));
    g.spells.back().reagents = {{2447, 1}, {9001, 1}};
    g.recipeItems.push_back({6001, 4000, 0, 0});
    // 5000: no source at all; 5100: learned with the skill
    g.abilities.push_back(Ability(ALCHEMY, 5000, 250, 260, 270, 0));
    g.spells.push_back(Spell(5000, "Orphan Brew", Effect::Make, 9003));
    g.abilities.push_back(Ability(ALCHEMY, 5100, 1, 1, 1, ACQUIRE_ON_SKILL_LEARN));
    g.spells.push_back(Spell(5100, "Basic Know-how", Effect::Other));
    // 2000: blacksmithing, with a tool and a station
    g.abilities.push_back(Ability(BLACKSMITHING, 2000, 1, 20, 40, ACQUIRE_ON_SKILL_VALUE));
    g.spells.push_back(Spell(2000, "Copper Rod", Effect::Make, 9004));
    g.spells.back().reagents = {{2840, 1}};
    g.spells.back().tools = {5956};
    g.spells.back().focus = 3;
    g.spells.back().toolCategories = {14};
    g.trainers.push_back({102, 2000, 2000, 10, BLACKSMITHING, 1, 1});
    // The alchemy rank ladder, two rungs through a wrapper
    std::uint32_t const rungs[][3] = {{2259, 75, 1}, {3101, 150, 2}, {3464, 225, 3}, {11611, 300, 4}, {28596, 375, 5}};
    for (auto const& rung : rungs)
    {
        SpellRow row = Spell(rung[0], "Rank " + std::to_string(rung[2]), Effect::Other);
        row.rankSkill = ALCHEMY;
        row.rankCap = rung[1];
        row.rankStep = rung[2];
        g.spells.push_back(row);
        g.trainers.push_back({103, rung[0], rung[0], 100 * rung[2], ALCHEMY, rung[1] - 75, 5 * rung[2]});
    }
    g.processes.push_back({ProcessKind::Prospect, 2770, 755, 1, 20, 20, 2, PROSPECT_MILL_COUNT, {{774, 0.5}}});
    for (std::uint32_t const id : {2447u, 3371u, 2840u, 5956u, 9001u, 9002u, 9003u, 9004u, 6000u, 6001u})
        g.items.push_back(Item(id, "item " + std::to_string(id)));
    return g;
}

Recipe const* Find(RecipeIndex const& index, std::uint32_t spell)
{
    for (Recipe const& recipe : index.Recipes())
        if (recipe.spell == spell)
            return &recipe;
    return nullptr;
}

void CheckThresholds(RecipeIndex const& index)
{
    Recipe const* elixir = Find(index, 1000);
    Check(elixir != nullptr, "the elixir is in the index");
    if (!elixir)
        return;
    Check(elixir->minRank == 90 && elixir->yellow == 110 && elixir->grey == 130, "orange, yellow, grey kept");
    Check(elixir->Green() == 120, "green is the middle of yellow and grey");
    Check(elixir->reagents.size() == 2 && elixir->reagents[0].item == 2447 && elixir->reagents[0].count == 2,
          "reagents and counts");
    Check(elixir->product == 9001 && elixir->productCount == 1 && elixir->castMs == 3000, "product and cast time");

    Recipe const* rod = Find(index, 2000);
    Check(rod && rod->tools == std::vector<std::uint32_t>{5956} && rod->focus == 3 &&
              rod->toolCategories == std::vector<std::uint32_t>{14},
          "tool, tool category and station");
    // sorted by skill, then threshold
    Check(index.Recipes().front().skill == BLACKSMITHING, "recipes sorted by skill line");
    Check(index.Recipes()[1].spell == 5100 || index.Recipes()[1].minRank <= index.Recipes()[2].minRank,
          "recipes sorted by threshold within a skill");
}

void CheckLearned(RecipeIndex const& index)
{
    Recipe const* elixir = Find(index, 1000);
    Check(elixir && HowLearned(*elixir) == "trainer", "a trainer's recipe says trainer");
    Check(elixir && elixir->trainers == 2 && elixir->trainerCopper == 400 && elixir->trainerRank == 90 &&
              elixir->trainerLevel == 10,
          "the cheapest of two trainers, counted");
    Recipe const* potion = Find(index, 3000);
    Check(potion && HowLearned(*potion) == "item-vendor" && potion->items.size() == 1 &&
              potion->items[0].item == 6000 && potion->items[0].vendors == 2 && potion->items[0].buyCopper == 2000,
          "a recipe item that vendors sell");
    Recipe const* flask = Find(index, 4000);
    Check(flask && HowLearned(*flask) == "item-found", "a recipe item nobody sells is found");
    Recipe const* orphan = Find(index, 5000);
    Check(orphan && HowLearned(*orphan) == "none", "a recipe without a source says none");
    Recipe const* skill = Find(index, 5100);
    Check(skill && HowLearned(*skill) == "skill", "a recipe learned with the skill");
    Recipe const* rod = Find(index, 2000);
    Check(rod && HowLearned(*rod) == "trainer", "a trainer wins over the rank rule");
}

void CheckLadder(RecipeIndex const& index)
{
    std::vector<Rung> const ladder = index.Ladder(ALCHEMY);
    Check(ladder.size() == 5, "five rungs");
    bool ordered = true;
    for (std::size_t i = 1; i < ladder.size(); ++i)
        ordered = ordered && ladder[i - 1].cap < ladder[i].cap;
    Check(ordered, "rungs by cap");
    Check(ladder.size() == 5 && ladder[3].cap == 300 && ladder[3].copper == 400 && ladder[3].reqRank == 225 &&
              ladder[3].reqLevel == 20,
          "the 300 rung: price, skill needed, level");
    Check(index.Ladder(BLACKSMITHING).empty(), "no rungs gathered for another skill");
}

void CheckMadeBy(RecipeIndex const& index)
{
    std::vector<std::size_t> const& made = index.MadeBy(9001);
    Check(made.size() == 2, "two recipes make the item");
    Check(index.MadeBy(1).empty(), "an item nothing makes");
    for (std::size_t const at : made)
        Check(index.Recipes()[at].product == 9001, "MadeBy points at recipes that make it");
    // the graph checks WR3b runs on the dump
    for (Recipe const& recipe : index.Recipes())
        for (ItemCount const& reagent : recipe.reagents)
        {
            Check(reagent.item != recipe.product, "no recipe is its own input (" + recipe.name + ")");
            ItemFact const* fact = index.Item(reagent.item);
            Check(fact && fact->known, "every reagent is a known item (" + recipe.name + ")");
        }
}

void CheckLoot()
{
    LootTable table;
    table[1] = {{10, 0, 50.0f, 0, 1, 3},                           // 0.5 x 2 average
                {11, 0, 0.0f, 1, 1, 1},                            // a group of two zero-chance items: half each
                {12, 0, 0.0f, 1, 1, 1},  {13, 0, 30.0f, 2, 2, 2},  // an explicit 30 % and the 70 % rest
                {14, 0, 0.0f, 2, 1, 1},  {0, 2, 25.0f, 0, 1, 3}};  // a quarter chance to roll template 2 three times
    table[2] = {{20, 0, 100.0f, 0, 2, 2}};
    auto const expected = [&](std::uint32_t item)
    {
        for (Yield const& yield : ExpectedYield(table, 1))
            if (yield.item == item)
                return yield.expected;
        return 0.0;
    };
    auto const same = [](double a, double b) { return a > b - 1e-9 && a < b + 1e-9; };
    Check(same(expected(10), 1.0), "ungrouped chance times the average count");
    Check(same(expected(11), 0.5) && same(expected(12), 0.5), "a zero-chance group splits the roll");
    Check(same(expected(13), 0.6) && same(expected(14), 0.7), "explicit chance, then the rest to the unset item");
    Check(same(expected(20), 1.5), "a reference rolls its template max-count times at its chance");
    Check(ExpectedYield(table, 99).empty(), "an unknown template yields nothing");

    // The server's references name templates of the reference table, not of the table the row is in
    LootTable references;
    references[2] = {{30, 0, 100.0f, 0, 1, 1}};
    double fromReference = 0.0;
    bool ownTwenty = false;
    for (Yield const& yield : ExpectedYield(table, 1, &references))
    {
        if (yield.item == 30)
            fromReference = yield.expected;
        ownTwenty = ownTwenty || yield.item == 20;
    }
    Check(same(fromReference, 0.75) && !ownTwenty, "a reference is read in the reference table");
}

void CheckDump(RecipeIndex const& index)
{
    std::string const text = index.Dump("test", "core");
    Check(text.rfind("# recipe-index-dump v1 module test core core records ", 0) == 0, "dump header");
    RecipeIndex back;
    std::string error;
    Check(RecipeIndex::Parse(text, back, error), "the dump parses: " + error);
    Check(back.Dump("test", "core") == text, "the dump reads back unchanged");
    Check(back.Records() == index.Records() && back.MadeBy(9001).size() == 2 && back.Ladder(ALCHEMY).size() == 5,
          "the read-back index answers the same");
    Check(back.Processes().size() == 1 && back.Processes()[0].yields.size() == 1 &&
              back.Processes()[0].consumed == PROSPECT_MILL_COUNT,
          "a processing row with its yields survives the dump");

    std::string broken = text;
    broken.erase(broken.rfind("I\t"));  // lose the last item record: the header's count no longer holds
    Check(!RecipeIndex::Parse(broken, back, error), "a dump with a missing record is refused");
    Check(!RecipeIndex::Parse("R\tx\n", back, error), "a dump without header or fields is refused");
}

// ---- WR3b: the dumped fixture ----

bool ReadFile(std::string const& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

// "A ladder to 300 taught in the old world": the rungs of cap 75, 150, 225 and 300 exist and a trainer teaches each
// (the 375 and 450 rungs are Outland's and Northrend's)
void CheckLadders(RecipeIndex const& index)
{
    for (ProfessionRules::ProfessionInfo const& profession : ProfessionRules::PROFESSIONS)
    {
        std::uint16_t const skill = static_cast<std::uint16_t>(profession.Which);
        std::string missing;
        for (std::uint32_t const cap : {75u, 150u, 225u, 300u})
        {
            bool taught = false;
            for (Rung const& rung : index.Ladder(skill))
                taught = taught || (rung.cap == cap && rung.trainers > 0);
            if (!taught)
                missing += " " + std::to_string(cap);
        }
        Check(missing.empty(), std::string(profession.Name) + " has a trained ladder to 300 (missing:" + missing + ")");
    }
}

// Recipes whose product is one of their reagents on purpose: reloading a device gives the device back. The recipe
// graph (WA) reads them as a reload, never as a source of the item
std::set<std::uint32_t> const RELOADS = {13240};  // The Mortar: Reloaded

void CheckReagents(RecipeIndex const& index)
{
    std::size_t reagents = 0;
    std::size_t unknown = 0;
    std::size_t selfInput = 0;
    for (Recipe const& recipe : index.Recipes())
        for (ItemCount const& reagent : recipe.reagents)
        {
            ++reagents;
            ItemFact const* fact = index.Item(reagent.item);
            if (!fact || !fact->known)
            {
                ++unknown;
                std::cout << "unknown reagent " << reagent.item << " of " << recipe.spell << ' ' << recipe.name << '\n';
            }
            if (reagent.item == recipe.product && recipe.product && !RELOADS.count(recipe.spell))
            {
                ++selfInput;
                std::cout << "own input: " << recipe.spell << ' ' << recipe.name << '\n';
            }
        }
    std::cout << "reagents: " << reagents << " in " << index.Recipes().size() << " recipes, " << unknown << " unknown, "
              << selfInput << " own inputs (reloads excepted: " << RELOADS.size() << ")\n";
    Check(reagents > 1000, "the dump has the reagents of the real recipes");
    Check(unknown == 0, "every reagent is a known item");
    Check(selfInput == 0, "no recipe is its own input (reloads excepted)");
}

// ---- WR2's table of crafts by class, against what each craft makes ----
// A product the class can use: gear it wears at the band's last level, a gem or a glyph it may use, ammunition for the
// classes with bows and guns, or an item item use has a record for (it knows how to use it:
// tests/fixtures/item-use-dump.txt; the demand rules need the bot's state, so the record stands for the demand answer)

using ProfessionRules::ClassBit;

constexpr std::uint8_t CLASSES[] = {ProfessionRules::WARRIOR, ProfessionRules::PALADIN, ProfessionRules::HUNTER,
                                    ProfessionRules::ROGUE,   ProfessionRules::PRIEST,  ProfessionRules::DEATH_KNIGHT,
                                    ProfessionRules::SHAMAN,  ProfessionRules::MAGE,    ProfessionRules::WARLOCK,
                                    ProfessionRules::DRUID};

std::string ClassName(std::uint8_t cls)
{
    char const* const names[] = {"",   "warrior", "paladin", "hunter",  "rogue", "priest",
                                 "dk", "shaman",  "mage",    "warlock", "",      "druid"};
    return cls < std::size(names) ? names[cls] : "class " + std::to_string(cls);
}

constexpr std::uint32_t Classes(std::initializer_list<std::uint8_t> list)
{
    std::uint32_t mask = 0;
    for (std::uint8_t const cls : list)
        mask |= ClassBit(cls);
    return mask;
}

// Weapon subclasses (item class 2) to the classes that can learn their skill
std::uint32_t WeaponClasses(std::uint32_t subClass)
{
    using namespace ProfessionRules;
    switch (subClass)
    {
        case 0:  // axe
            return Classes({WARRIOR, PALADIN, HUNTER, ROGUE, SHAMAN, DEATH_KNIGHT});
        case 1:  // two-hand axe
            return Classes({WARRIOR, PALADIN, HUNTER, SHAMAN, DEATH_KNIGHT});
        case 2:   // bow
        case 3:   // gun
        case 16:  // thrown
        case 18:  // crossbow
            return Classes({WARRIOR, HUNTER, ROGUE});
        case 4:  // mace
            return Classes({WARRIOR, PALADIN, ROGUE, PRIEST, SHAMAN, DRUID, DEATH_KNIGHT});
        case 5:  // two-hand mace
            return Classes({WARRIOR, PALADIN, SHAMAN, DRUID, DEATH_KNIGHT});
        case 6:  // polearm
            return Classes({WARRIOR, PALADIN, HUNTER, DRUID, DEATH_KNIGHT});
        case 7:  // sword
            return Classes({WARRIOR, PALADIN, HUNTER, ROGUE, MAGE, WARLOCK, DEATH_KNIGHT});
        case 8:  // two-hand sword
            return Classes({WARRIOR, PALADIN, HUNTER, DEATH_KNIGHT});
        case 10:  // staff
            return Classes({WARRIOR, HUNTER, PRIEST, SHAMAN, MAGE, WARLOCK, DRUID});
        case 13:  // fist
            return Classes({WARRIOR, HUNTER, ROGUE, SHAMAN, DRUID});
        case 15:  // dagger
            return Classes({WARRIOR, HUNTER, ROGUE, PRIEST, SHAMAN, MAGE, WARLOCK, DRUID});
        case 19:  // wand
            return Classes({PRIEST, MAGE, WARLOCK});
        case 20:  // fishing pole
            return ALL_CLASSES;
        default:
            return 0;
    }
}

// Armour subclasses (item class 4); hunters and shamans wear mail, warriors and paladins plate from level 40
bool WearsArmor(std::uint32_t subClass, std::uint8_t cls, std::uint32_t level)
{
    using namespace ProfessionRules;
    std::uint32_t const bit = ClassBit(cls);
    switch (subClass)
    {
        case 0:  // jewellery, trinkets
        case 1:  // cloth
            return true;
        case 2:  // leather
            return (bit & Classes({PRIEST, MAGE, WARLOCK})) == 0;
        case 3:  // mail
            return (bit & Classes({WARRIOR, PALADIN, DEATH_KNIGHT})) != 0 ||
                   ((bit & Classes({HUNTER, SHAMAN})) != 0 && level >= 40);
        case 4:  // plate
            return cls == DEATH_KNIGHT || ((bit & Classes({WARRIOR, PALADIN})) != 0 && level >= 40);
        case 6:  // shield
            return (bit & Classes({WARRIOR, PALADIN, SHAMAN})) != 0;
        case 7:
            return cls == PALADIN;  // libram
        case 8:
            return cls == DRUID;  // idol
        case 9:
            return cls == SHAMAN;  // totem
        case 10:
            return cls == DEATH_KNIGHT;  // sigil
        default:
            return false;
    }
}

bool Usable(RecipeIndex const& index, Recipe const& recipe, std::set<std::uint32_t> const& itemUse, std::uint8_t cls,
            std::uint32_t level)
{
    if (recipe.classMask && !(recipe.classMask & ClassBit(cls)))
        return false;
    if (recipe.effect == Effect::Enchant)  // every class enchants the gear it wears
        return true;
    if (recipe.effect != Effect::Make)
        return false;
    ItemFact const* item = index.Item(recipe.product);
    if (!item || !item->known || item->requiredLevel > level)
        return false;
    if (item->allowableClass && item->allowableClass != 0xFFFFFFFFu && !(item->allowableClass & ClassBit(cls)))
        return false;
    switch (item->itemClass)
    {
        case 2:
            return (WeaponClasses(item->subClass) & ClassBit(cls)) != 0;
        case 4:
            return WearsArmor(item->subClass, cls, level);
        case 3:   // gems
        case 16:  // glyphs (allowableClass above)
            return true;
        case 6:  // ammunition
            return (WeaponClasses(2) & ClassBit(cls)) != 0;
        default:
            return itemUse.count(recipe.product) != 0;
    }
}

// The skill a recipe is learned at: the trainer's rank, else the ability's orange, else its yellow (abilities learned
// from a recipe item mostly keep orange 1 in the data)
std::uint32_t LearnRank(Recipe const& recipe)
{
    std::uint32_t const rank = recipe.trainers ? recipe.trainerRank : recipe.minRank;
    return rank <= 1 && recipe.yellow > 1 ? recipe.yellow : rank;
}

// Level bands of ten up to the cap 80; a bot of the band has skill level x 5 (WR2's grant), so the band's recipes are
// those learned at 5 x its first level up to 5 x its last level + 4 (the last band up to 450)
struct Band
{
    std::uint32_t first;
    std::uint32_t last;
};
constexpr Band BANDS[] = {{1, 9}, {10, 19}, {20, 29}, {30, 39}, {40, 49}, {50, 59}, {60, 69}, {70, 80}};

// Pairs the table allows where a class has nothing to use in some band, listed for the user (WR3b record, "Questions
// for the user") and not changed silently: craft skill and class. Empty on the first dump
std::set<std::pair<std::uint16_t, std::uint8_t>> const OPEN_QUESTIONS = {};

void CheckCraftsByClass(RecipeIndex const& index, std::set<std::uint32_t> const& itemUse)
{
    std::set<std::pair<std::uint16_t, std::uint8_t>> failing;
    std::size_t checked = 0;
    for (std::uint8_t const cls : CLASSES)
    {
        std::set<ProfessionRules::Skill> crafts;
        for (ProfessionRules::Pair const& pair : ProfessionRules::OpenPairs(cls))
            for (ProfessionRules::Skill const skill : {pair.First, pair.Second})
                if (ProfessionRules::Is(skill, ProfessionRules::Kind::Craft))
                    crafts.insert(skill);  // two gatherers make nothing: they earn
        for (ProfessionRules::Skill const craft : crafts)
        {
            std::uint16_t const skill = static_cast<std::uint16_t>(craft);
            std::string empty;
            for (Band const& band : BANDS)
            {
                std::uint32_t const from = band.first == 1 ? 1 : 5 * band.first;
                std::uint32_t const to = band.last >= 80 ? 450 : 5 * band.last + 4;
                bool found = false;
                for (Recipe const& recipe : index.Recipes())
                    if (!found && recipe.skill == skill && LearnRank(recipe) >= from && LearnRank(recipe) <= to)
                        found = Usable(index, recipe, itemUse, cls, band.last);
                if (!found)
                    empty += " " + std::to_string(band.first) + "-" + std::to_string(band.last);
            }
            ++checked;
            if (!empty.empty())
            {
                failing.insert({skill, cls});
                std::cout << "QUESTION: " << ProfessionRules::Name(craft) << " for " << ClassName(cls)
                          << ": nothing the class can use at levels" << empty << '\n';
            }
        }
    }
    std::cout << "crafts by class: " << checked << " craft-class pairs, " << failing.size() << " with an empty band\n";
    Check(checked >= 50, "every class has its crafts checked");
    Check(failing == OPEN_QUESTIONS, "WR2's crafts by class: the failing pairs are exactly the listed questions");
}

void CheckDumpedFixture(std::string const& root)
{
    std::string text;
    std::string error;
    RecipeIndex index;
    Check(ReadFile(root + "/tests/fixtures/recipe-index-dump.txt", text), "the dumped fixture is readable");
    Check(RecipeIndex::Parse(text, index, error), "the dumped fixture parses: " + error);
    std::string const again = index.Dump("x", "y");
    Check(again.substr(again.find('\n')) == text.substr(text.find('\n')), "the dumped fixture reads back unchanged");
    std::cout << "dumped fixture: " << text.substr(0, text.find('\n')) << '\n';
    for (ProfessionRules::ProfessionInfo const& profession : ProfessionRules::PROFESSIONS)
        std::cout << index.Summary(static_cast<std::uint16_t>(profession.Which), std::string(profession.Name)) << '\n';

    std::set<std::uint32_t> itemUse;
    std::string useText;
    Check(ReadFile(root + "/tests/fixtures/item-use-dump.txt", useText), "the item use fixture is readable");
    std::istringstream lines(useText);
    std::string line;
    while (std::getline(lines, line))
        if (line.rfind("0\t", 0) == 0)  // source 0: an item's record; the item entry follows
            itemUse.insert(static_cast<std::uint32_t>(std::stoul(line.substr(2, line.find('\t', 2) - 2))));
    Check(itemUse.size() > 1000, "item use's records are read");

    CheckLadders(index);
    CheckReagents(index);
    CheckCraftsByClass(index, itemUse);
}
}  // namespace

int main(int argc, char** argv)
{
    std::cout << "RECIPES selftest (RecipeIndex.h)\n";
    RecipeIndex const index = RecipeIndex::Build(HandGather());
    std::cout << index.Summary(ALCHEMY, "alchemy") << '\n' << index.Summary(BLACKSMITHING, "blacksmithing") << '\n';
    Check(index.Recipes().size() == 6, "six abilities become six recipes");
    CheckThresholds(index);
    CheckLearned(index);
    CheckLadder(index);
    CheckMadeBy(index);
    CheckLoot();
    CheckDump(index);
    if (argc > 1)
        CheckDumpedFixture(argv[1]);
    else
        Check(false, "the repo root is the first argument (tests/selftests.json passes it)");
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
