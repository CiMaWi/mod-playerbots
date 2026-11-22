/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI9a (INTERFACE 1.13): demand, ItemDemandRules.h, on records of the committed dump. It prints the
// tuning tables, then hand cases per slot with their lines and verdicts (heal below, at and above the stock, mana,
// flasks per role, elixirs, scrolls, food and drink, well-fed, coatings per class, an explosive with and without the
// skill, level and class masks, measured use, upkeep's stock targets, worth, thrift, not persistent, control), then
// a seeded random search over the dump's consumed records that counts the invariants D1..D4 (and the worth bounds),
// each of which must stay 0, with a check that the search was not empty. WI9b adds the buy pick: hand cases over
// vendor offers, and D5..D8 in the search (offered entries mixed in).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ItemDemandRules.h"
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

std::map<std::uint32_t, Record> byEntry;  // the first consumed record of each item
std::vector<Record> pool;                 // every consumed item record of the dump
std::map<std::uint32_t, std::string> names;
std::vector<std::uint16_t> skillsSeen;  // the skills the pool's records need

std::string NameOf(std::uint32_t entry)
{
    auto const it = names.find(entry);
    return it == names.end() ? std::string("?") : it->second;
}

bool Load(std::string const& root)
{
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    if (!in)
        return false;
    std::set<std::uint16_t> skills;
    std::string line;
    while (std::getline(in, line))
        if (std::optional<DumpRow> const row = ParseDumpLine(line))
            if (row->record.source == Source::ItemSpell && row->record.consumed && row->record.itemEntry)
            {
                pool.push_back(row->record);
                byEntry.emplace(row->record.itemEntry, row->record);
                names.emplace(row->record.itemEntry, row->name);
                if (row->record.needs.skill)
                    skills.insert(row->record.needs.skill);
            }
    skillsSeen.assign(skills.begin(), skills.end());
    return !pool.empty();
}

// The item ids of the hand cases (the dump names them)
constexpr std::uint32_t RUNIC_HEALING = 33447;
constexpr std::uint32_t MINOR_HEALING = 118;
constexpr std::uint32_t RUNIC_MANA = 33448;
constexpr std::uint32_t SUPERIOR_HEALING = 3928;   // level 35, vendors sell it (WI9b)
constexpr std::uint32_t SUPER_HEALING = 22829;     // level 55
constexpr std::uint32_t SUPERIOR_MANA = 6149;      // level 31
constexpr std::uint32_t FLASK_RAGE = 46377;        // attack power
constexpr std::uint32_t FLASK_WYRM = 46376;        // spell power
constexpr std::uint32_t FLASK_STONEBLOOD = 46379;  // health
constexpr std::uint32_t ELIXIR_STRENGTH = 40073;   // battle
constexpr std::uint32_t ELIXIR_THOUGHTS = 44332;   // guardian, intellect
constexpr std::uint32_t SCROLL_STRENGTH = 43466;
constexpr std::uint32_t FLATBREAD = 33449;
constexpr std::uint32_t TEA = 33445;
constexpr std::uint32_t MAMMOTH_MEAL = 34754;
constexpr std::uint32_t INSTANT_POISON = 43231;
constexpr std::uint32_t DEADLY_POISON = 43233;
constexpr std::uint32_t SHARPENING_STONE = 23529;
constexpr std::uint32_t WIZARD_OIL = 36900;
constexpr std::uint32_t SARONITE_BOMB = 41119;  // engineering 350
constexpr std::uint16_t ENGINEERING = 202;

constexpr std::uint32_t SUB_DAGGER = 15;
constexpr std::uint32_t SUB_SWORD = 7;
constexpr std::uint32_t SUB_STAFF = 10;

Record Rec(std::uint32_t entry)
{
    auto const it = byEntry.find(entry);
    if (it == byEntry.end())
    {
        Check(false, "the dump has a consumed record of item " + std::to_string(entry));
        Record none;
        none.itemEntry = entry;
        return none;
    }
    return it->second;
}

UsableEntry EntryOf(Record const& record, std::uint32_t id, std::uint32_t count)
{
    Classified const classified = Classify(record);
    UsableEntry entry;
    entry.record = id;
    entry.item.entry = record.itemEntry;
    entry.kind = classified.kind;
    entry.sub = classified.sub;
    entry.rank = classified.rank;
    entry.count = count;
    entry.consumed = record.consumed;
    entry.potion = record.potion;
    entry.replaceCopper = record.replaceCopper;
    entry.ready = true;
    entry.shape = record.shape;
    entry.radiusYards = record.radiusYards;
    entry.rangeYards = record.rangeYards;
    entry.minRangeYards = record.minRangeYards;
    entry.rangeFriendYards = record.rangeFriendYards;
    return entry;
}

// A coating enchantment's stats, by hand (the live builder reads them from the game's enchantment store)
BuffWeights StatsOf(std::uint32_t entry)
{
    BuffWeights stats{};
    auto const set = [&](BuffStat stat, float amount) { stats[static_cast<std::size_t>(stat)] = amount; };
    if (entry == SHARPENING_STONE || entry == 28421)
    {
        set(BuffStat::AttackPower, 12.0f);
        set(BuffStat::Crit, 14.0f);
    }
    else if (entry == WIZARD_OIL)
        set(BuffStat::SpellPower, 36.0f);
    else if (entry == 36899)
    {
        set(BuffStat::ManaRegen, 12.0f);
        set(BuffStat::HealPower, 25.0f);
    }
    return stats;
}

PoisonKind PoisonOf(std::uint32_t entry)
{
    std::string const name = NameOf(entry);
    if (name.find("Instant Poison") != std::string::npos)
        return PoisonKind::Instant;
    if (name.find("Deadly Poison") != std::string::npos)
        return PoisonKind::Deadly;
    return PoisonKind::None;
}

struct Holding
{
    std::vector<UsableEntry> list;
    std::vector<Record> records;
    std::vector<std::uint8_t> canUse;
    std::vector<BuffWeights> stats;
    std::vector<PoisonKind> kinds;
    std::vector<DemandSkill> skills;
    std::vector<std::uint8_t> offered;

    void Add(Record const& record, std::uint32_t count, bool usable = true)
    {
        offered.push_back(0);
        records.push_back(record);
        list.push_back(EntryOf(record, static_cast<std::uint32_t>(list.size()), count));
        canUse.push_back(usable ? 1 : 0);
        stats.push_back(StatsOf(record.itemEntry));
        kinds.push_back(record.exclude == Exclude::Coating && record.needs.classMask ? PoisonOf(record.itemEntry)
                                                                                     : PoisonKind::None);
    }
    void Add(std::uint32_t entry, std::uint32_t count, bool usable = true) { Add(Rec(entry), count, usable); }
    // A vendor sells it (WI9b): held, the last entry flagged; else a count of 0
    void Offer(std::uint32_t entry, std::uint32_t count = 0)
    {
        Add(entry, count);
        offered.back() = 1;
    }
};

TargetView Weapon(std::uint32_t subClass, std::uint32_t invType)
{
    TargetView view;
    view.kind = TargetKind::OwnItem;
    view.found = true;
    view.itemClass = 2;
    view.itemSubClass = subClass;
    view.itemInvType = invType;
    view.itemLevel = 150;
    view.requiredLevel = 80;
    return view;
}

bool UsesMana(std::uint8_t cls) { return cls != 1 && cls != 4 && cls != 6; }

DemandInput Bot(std::uint8_t cls, std::uint8_t level, Role role, Holding const& holding, float thrift = 0.0f)
{
    DemandInput input;
    input.cls = cls;
    input.race = 1;
    input.level = level;
    input.role = role;
    input.classBit = 1u << (cls - 1);
    input.raceBit = 1u;
    input.usesMana = UsesMana(cls);
    input.skills = holding.skills;
    input.thrift = thrift;
    input.earnCopperPerMinute = 1000;
    input.deathCopper = DeathCopper(1000, 20000);
    input.persistent = true;
    input.list = holding.list;
    input.records = holding.records;
    input.canUse = holding.canUse;
    input.offered = holding.offered;
    input.coatStats = holding.stats;
    input.poisonKinds = holding.kinds;
    return input;
}

DemandKeep KeepOf(DemandAnswer const& answer, std::uint32_t entry)
{
    for (DemandKeep const& keep : answer.keeps)
        if (keep.entry == entry)
            return keep;
    DemandKeep none;
    none.verdict = DemandVerdict::None;
    return none;
}

DemandLine const& LineOf(DemandAnswer const& answer, DemandSlot slot)
{
    return answer.lines[static_cast<std::size_t>(slot)];
}

void Show(std::string const& title, DemandAnswer const& answer)
{
    std::cout << "case " << title << (answer.control ? " (control)" : "") << "\n";
    for (DemandLine const& line : answer.lines)
        if (line.held || line.wantedStock || line.bestEntry)
            std::cout << "  line " << DemandSlotWord(line.slot) << " kind=" << KindWord(line.kind)
                      << " sub=" << SubWord(line.sub) << " wanted=" << line.wantedStock << " held=" << line.held
                      << " use/h=" << line.usePerHour << (line.measured ? " measured" : "")
                      << " worth=" << line.worthCopperEach << " best=" << line.bestEntry
                      << (line.buyUnits ? " buy=" + std::to_string(line.buyEntry) + " x" + std::to_string(line.buyUnits)
                                        : std::string())
                      << "\n";
    for (DemandBuy const& buy : answer.buys)
        std::cout << "  buys " << DemandSlotWord(buy.slot) << " " << buy.entry << " (" << NameOf(buy.entry) << ")\n";
    for (DemandKeep const& keep : answer.keeps)
        std::cout << "  keep " << keep.entry << " (" << NameOf(keep.entry) << ") " << DemandSlotWord(keep.slot) << " "
                  << DemandVerdictWord(keep.verdict) << " keep=" << keep.keepUnits << "/" << keep.held
                  << " worth=" << keep.worthCopperEach << "\n";
}

void PrintTables()
{
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "tables: DEMAND_USE_PER_HOUR:";
    for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
        std::cout << " " << DEMAND_SLOT_WORDS[s] << "=" << DEMAND_USE_PER_HOUR[s];
    std::cout << "\ntables: food, drink: upkeep's (food " << UpkeepNeeds::FOOD_PER_HOUR << "/h but mages, water "
              << UpkeepNeeds::WATER_PER_HOUR << "/h for the mana classes but mages); rogue poisons "
              << UpkeepNeeds::POISON_PER_HOUR << "/h split over the hands; stock hours "
              << UpkeepNeeds::STOCK_TARGET_HOURS << ", thrift scale " << UpkeepNeeds::THRIFT_STOCK_SCALE
              << "; DEMAND_DANGER " << DEMAND_DANGER << ", DEMAND_REST_MINUTES " << DEMAND_REST_MINUTES
              << ", buffs and coatings read with Ahead dungeon\n";
}

void HealCases()
{
    for (std::uint32_t const count : {2u, 6u, 12u})
    {
        Holding holding;
        holding.Add(RUNIC_HEALING, count);
        DemandAnswer const answer = Demand(Bot(1, 80, Role::Melee, holding));
        Show("warrior 80, " + std::to_string(count) + " runic healing", answer);
        DemandKeep const keep = KeepOf(answer, RUNIC_HEALING);
        DemandVerdict const expected = count < 6 ? DemandVerdict::Use : DemandVerdict::Keep;
        Check(LineOf(answer, DemandSlot::Heal).wantedStock == 6 && keep.verdict == expected &&
                  keep.keepUnits == std::min(count, 6u),
              "heal: " + std::to_string(count) + " held of 6 wanted is " + std::string(DemandVerdictWord(expected)));
    }
    Holding holding;
    holding.Add(RUNIC_HEALING, 6);
    holding.Add(MINOR_HEALING, 5);
    DemandAnswer const answer = Demand(Bot(1, 80, Role::Melee, holding));
    Show("warrior 80, 6 runic and 5 minor healing", answer);
    Check(KeepOf(answer, RUNIC_HEALING).verdict == DemandVerdict::Keep &&
              KeepOf(answer, MINOR_HEALING).verdict == DemandVerdict::Surplus &&
              LineOf(answer, DemandSlot::Heal).bestEntry == RUNIC_HEALING,
          "heal: above the stock the weaker potion is surplus, the stronger kept");

    DemandAnswer const low = Demand(Bot(1, 60, Role::Melee, holding));
    Show("warrior 60, 6 runic and 5 minor healing", low);
    Check(KeepOf(low, RUNIC_HEALING).verdict == DemandVerdict::Unusable &&
              KeepOf(low, MINOR_HEALING).verdict == DemandVerdict::Use,
          "heal: a potion above the bot's level is unusable, the one it can drink is kept");
}

void ManaCases()
{
    Holding holding;
    holding.Add(RUNIC_MANA, 4);
    DemandAnswer const mage = Demand(Bot(8, 80, Role::Caster, holding));
    DemandAnswer const warrior = Demand(Bot(1, 80, Role::Melee, holding));
    Show("mage 80, 4 runic mana", mage);
    Show("warrior 80, 4 runic mana", warrior);
    Check(KeepOf(mage, RUNIC_MANA).verdict == DemandVerdict::Use && LineOf(mage, DemandSlot::Mana).wantedStock == 6,
          "mana: a mage keeps mana potions (4 of 6: use)");
    Check(KeepOf(warrior, RUNIC_MANA).verdict == DemandVerdict::Unusable &&
              LineOf(warrior, DemandSlot::Mana).wantedStock == 0,
          "mana: a warrior's mana potion is unusable");
}

void BuffCases()
{
    Holding holding;
    holding.Add(FLASK_RAGE, 2);
    holding.Add(FLASK_WYRM, 2);
    holding.Add(FLASK_STONEBLOOD, 2);
    holding.Add(ELIXIR_STRENGTH, 2);
    holding.Add(ELIXIR_THOUGHTS, 2);
    holding.Add(SCROLL_STRENGTH, 2);
    holding.Add(MAMMOTH_MEAL, 3);
    struct RoleCase
    {
        std::uint8_t cls;
        Role role;
        std::uint32_t flask;
        char const* name;
    };
    for (RoleCase const& c :
         {RoleCase{1, Role::Melee, FLASK_RAGE, "warrior melee"}, RoleCase{8, Role::Caster, FLASK_WYRM, "mage caster"},
          RoleCase{1, Role::Tank, FLASK_STONEBLOOD, "warrior tank"}})
    {
        DemandAnswer const answer = Demand(Bot(c.cls, 80, c.role, holding));
        Show(std::string(c.name) + " 80, flasks, elixirs, a scroll, a meal", answer);
        DemandLine const& flask = LineOf(answer, DemandSlot::Flask);
        bool othersSold = true;
        for (std::uint32_t const other : {FLASK_RAGE, FLASK_WYRM, FLASK_STONEBLOOD})
            if (other != c.flask)
                othersSold = othersSold && (KeepOf(answer, other).verdict == DemandVerdict::Surplus ||
                                            KeepOf(answer, other).verdict == DemandVerdict::Unusable);
        Check(flask.bestEntry == c.flask && KeepOf(answer, c.flask).verdict == DemandVerdict::Keep && othersSold,
              std::string("flask: ") + c.name + " keeps " + NameOf(c.flask) + ", the other flasks are sold");
    }
    DemandAnswer const warrior = Demand(Bot(1, 80, Role::Melee, holding));
    Check(KeepOf(warrior, ELIXIR_THOUGHTS).verdict == DemandVerdict::Unusable &&
              KeepOf(warrior, ELIXIR_STRENGTH).verdict == DemandVerdict::Keep,
          "elixir: intellect scores 0 for a melee warrior (unusable), strength is kept");
    Check(KeepOf(warrior, SCROLL_STRENGTH).verdict == DemandVerdict::Keep &&
              LineOf(warrior, DemandSlot::Scroll).wantedStock == 2,
          "scroll: a warrior keeps its strength scroll");
    Check(KeepOf(warrior, MAMMOTH_MEAL).slot == DemandSlot::WellFed &&
              KeepOf(warrior, MAMMOTH_MEAL).verdict == DemandVerdict::Keep &&
              KeepOf(warrior, MAMMOTH_MEAL).keepUnits == 2,
          "well-fed: the meal fills the well_fed slot (2 kept of 3)");
}

void FoodCases()
{
    Holding holding;
    holding.Add(FLATBREAD, 5);
    holding.Add(TEA, 20);
    DemandAnswer const mage = Demand(Bot(8, 80, Role::Caster, holding));
    DemandAnswer const priest = Demand(Bot(5, 80, Role::Healer, holding));
    Show("mage 80, 5 flatbread, 20 tea", mage);
    Show("priest 80, 5 flatbread, 20 tea", priest);
    Check(KeepOf(mage, FLATBREAD).verdict == DemandVerdict::Surplus &&
              KeepOf(mage, TEA).verdict == DemandVerdict::Surplus,
          "food and drink: a mage conjures its own (upkeep's default 0): bought ones are surplus");
    Check(KeepOf(priest, FLATBREAD).verdict == DemandVerdict::Use &&
              LineOf(priest, DemandSlot::Food).wantedStock == 12 &&
              KeepOf(priest, TEA).verdict == DemandVerdict::Keep && KeepOf(priest, TEA).keepUnits == 12,
          "food and drink: a priest wants 12 of each (5 food: use; 20 tea: 12 kept)");

    DemandInput measured = Bot(5, 80, Role::Healer, holding);
    measured.upkeepUsePerHour[static_cast<std::size_t>(Planner::UpkeepStock::Food)] = 8.0f;
    DemandAnswer const more = Demand(measured);
    Check(LineOf(more, DemandSlot::Food).wantedStock == 24 && LineOf(more, DemandSlot::Food).measured,
          "measured use above the default raises the stock (8/h: 24)");

    DemandAnswer const frugal = Demand(Bot(5, 80, Role::Healer, holding, 1.0f));
    DemandAnswer const spendthrift = Demand(Bot(5, 80, Role::Healer, holding, -1.0f));
    Check(LineOf(frugal, DemandSlot::Food).wantedStock == 18 && LineOf(spendthrift, DemandSlot::Food).wantedStock == 6,
          "thrift scales the stock (frugal 18, spendthrift 6)");
}

void UpkeepCases()
{
    for (float const thrift : {-1.0f, -0.37f, 0.0f, 0.5f, 1.0f})
        for (float const use : {0.0f, 3.3f, 7.0f})
        {
            Planner::Snapshot snapshot;
            snapshot.level = 80;
            TraitRules::BotTraits traits;
            traits.Values[TraitRules::Index(TraitRules::Trait::Thrift)] =
                static_cast<std::int8_t>(std::lround(thrift * TraitRules::MAX_VALUE));
            snapshot.traits = traits;
            UpkeepNeeds::NeedRates rates;
            for (float& rate : rates.usePerHour)
                rate = use;

            Holding holding;
            holding.Add(FLATBREAD, 1);
            holding.Add(TEA, 1);
            holding.Add(INSTANT_POISON, 1);
            holding.Add(DEADLY_POISON, 1);
            bool same = true;
            for (std::uint8_t const cls : {std::uint8_t(4), std::uint8_t(5), std::uint8_t(8)})
            {
                snapshot.cls = cls;
                DemandInput input = Bot(cls, 80, cls == 4 ? Role::Melee : Role::Caster, holding,
                                        UpkeepNeeds::TraitUnit(snapshot, TraitRules::Trait::Thrift));
                input.upkeepUsePerHour = rates.usePerHour;
                input.mainHand = Weapon(SUB_DAGGER, 13);
                input.offHand = Weapon(SUB_DAGGER, 13);
                DemandAnswer const answer = Demand(input);
                std::uint32_t const food = UpkeepNeeds::StockTarget(snapshot, rates, Planner::UpkeepStock::Food);
                std::uint32_t const water = UpkeepNeeds::StockTarget(snapshot, rates, Planner::UpkeepStock::Water);
                std::uint32_t const poison = UpkeepNeeds::StockTarget(snapshot, rates, Planner::UpkeepStock::Poison);
                std::uint32_t const hands =
                    LineOf(answer, DemandSlot::CoatMain).wantedStock + LineOf(answer, DemandSlot::CoatOff).wantedStock;
                bool const foodSame = LineOf(answer, DemandSlot::Food).wantedStock == food;
                bool const waterSame = LineOf(answer, DemandSlot::Drink).wantedStock == water;
                bool const poisonSame = cls != 4 || hands == poison;
                if (!foodSame || !waterSame || !poisonSame)
                    std::cout << "  differs: class " << int(cls) << " thrift " << thrift << " use " << use << " food "
                              << LineOf(answer, DemandSlot::Food).wantedStock << "/" << food << " water "
                              << LineOf(answer, DemandSlot::Drink).wantedStock << "/" << water << " poison " << hands
                              << "/" << poison << "\n";
                same = same && foodSame && waterSame && poisonSame;
            }
            std::ostringstream what;
            what << "food, drink and poison stocks equal upkeep's StockTarget (thrift " << thrift << ", use " << use
                 << ")";
            Check(same, what.str());
        }
}

void CoatCases()
{
    Holding poisons;
    poisons.Add(INSTANT_POISON, 10);
    poisons.Add(DEADLY_POISON, 10);
    DemandInput rogue = Bot(4, 80, Role::Melee, poisons);
    rogue.mainHand = Weapon(SUB_DAGGER, 13);
    rogue.offHand = Weapon(SUB_DAGGER, 13);
    DemandAnswer const rogueAnswer = Demand(rogue);
    Show("rogue 80, two daggers, instant and deadly poison", rogueAnswer);
    Check(KeepOf(rogueAnswer, INSTANT_POISON).slot == DemandSlot::CoatMain &&
              KeepOf(rogueAnswer, DEADLY_POISON).slot == DemandSlot::CoatOff &&
              KeepOf(rogueAnswer, INSTANT_POISON).verdict == DemandVerdict::Keep &&
              KeepOf(rogueAnswer, DEADLY_POISON).verdict == DemandVerdict::Keep,
          "coating: a rogue keeps instant poison for the main hand, deadly for the off hand");

    Holding stones;
    stones.Add(SHARPENING_STONE, 2);
    stones.Add(WIZARD_OIL, 2);
    stones.Add(INSTANT_POISON, 2);
    DemandInput warrior = Bot(1, 80, Role::Melee, stones);
    warrior.mainHand = Weapon(SUB_SWORD, 13);
    DemandAnswer const warriorAnswer = Demand(warrior);
    Show("warrior 80, a sword, a stone, an oil, a poison", warriorAnswer);
    Check(KeepOf(warriorAnswer, SHARPENING_STONE).verdict == DemandVerdict::Use &&
              KeepOf(warriorAnswer, INSTANT_POISON).verdict == DemandVerdict::Unusable,
          "coating: a warrior keeps the stone for its sword (2 of 3: use); the rogue's poison is unusable");

    DemandInput priest = Bot(5, 80, Role::Caster, stones);
    priest.mainHand = Weapon(SUB_STAFF, 17);
    DemandAnswer const priestAnswer = Demand(priest);
    Show("priest 80, a staff, a stone, an oil, a poison", priestAnswer);
    Check(LineOf(priestAnswer, DemandSlot::CoatMain).bestEntry == WIZARD_OIL &&
              KeepOf(priestAnswer, WIZARD_OIL).verdict == DemandVerdict::Use,
          "coating: a priest keeps the wizard oil for its staff");

    for (std::uint8_t const cls : {std::uint8_t(3), std::uint8_t(7)})
    {
        DemandInput input = Bot(cls, 80, cls == 3 ? Role::Ranged : Role::Melee, stones);
        input.mainHand = Weapon(SUB_SWORD, 13);
        DemandAnswer const answer = Demand(input);
        Check(KeepOf(answer, SHARPENING_STONE).verdict == DemandVerdict::Unusable &&
                  LineOf(answer, DemandSlot::CoatMain).wantedStock == 0,
              std::string(cls == 3 ? "hunter" : "shaman") + ": coats no weapon");
    }
}

void SkillCases()
{
    Holding holding;
    holding.Add(SARONITE_BOMB, 3);
    DemandAnswer const without = Demand(Bot(1, 80, Role::Melee, holding));
    holding.skills.push_back({ENGINEERING, 375});
    DemandAnswer const with = Demand(Bot(1, 80, Role::Melee, holding));
    Show("warrior 80, engineer 375, saronite bombs", with);
    Check(KeepOf(without, SARONITE_BOMB).verdict == DemandVerdict::Unusable &&
              KeepOf(with, SARONITE_BOMB).verdict == DemandVerdict::Keep &&
              KeepOf(with, SARONITE_BOMB).slot == DemandSlot::DamageArea,
          "explosive: unusable without engineering, kept in damage_area with it");

    Holding masked;
    masked.Add(INSTANT_POISON, 5);
    DemandInput input = Bot(1, 80, Role::Melee, masked);
    input.mainHand = Weapon(SUB_DAGGER, 13);
    Check(KeepOf(Demand(input), INSTANT_POISON).verdict == DemandVerdict::Unusable,
          "a class-masked item is unusable for another class");

    Holding refused;
    refused.Add(RUNIC_HEALING, 5, false);
    Check(KeepOf(Demand(Bot(1, 80, Role::Melee, refused)), RUNIC_HEALING).verdict == DemandVerdict::Unusable,
          "an item the engine refuses (canUse false) is unusable");
}

void WorthCases()
{
    Holding holding;
    holding.Add(RUNIC_HEALING, 3);
    holding.Add(FLATBREAD, 3);
    holding.Add(SCROLL_STRENGTH, 1);
    holding.Add(SARONITE_BOMB, 1);
    holding.skills.push_back({ENGINEERING, 375});
    DemandAnswer const answer = Demand(Bot(1, 80, Role::Melee, holding));
    Show("warrior 80, worth", answer);
    bool bounded = true;
    for (DemandKeep const& keep : answer.keeps)
    {
        Record const record = Rec(keep.entry);
        std::uint32_t const cap = record.price != PriceSource::None
                                      ? record.replaceCopper
                                      : record.sellCopper * WorldTravel::BUY_PER_SELL_PRICE;
        bounded = bounded && keep.worthCopperEach <= std::max<std::uint32_t>(cap, 1) &&
                  (keep.keepUnits == 0 || keep.worthCopperEach > 0);
    }
    Check(bounded, "worth: never above the replacement cap, above 0 for a kept item");
    Check(WorthEachOf(answer, RUNIC_HEALING) == KeepOf(answer, RUNIC_HEALING).worthCopperEach &&
              WorthEachOf(answer, 999999) == 0 && VerdictOf(answer, 999999) == DemandVerdict::None,
          "worth and verdict of an entry not listed: 0 and none");
}

void CohortCases()
{
    Holding holding;
    holding.Add(RUNIC_HEALING, 3);
    holding.Add(FLATBREAD, 30);
    DemandInput input = Bot(1, 80, Role::Melee, holding);
    input.persistent = false;
    DemandAnswer const none = Demand(input);
    bool empty = none.keeps.empty();
    for (DemandLine const& line : none.lines)
        empty = empty && line.wantedStock == 0 && line.held == 0;
    Check(empty, "not persistent: an empty answer");

    input.persistent = true;
    DemandAnswer const normal = Demand(input);
    input.control = true;
    DemandAnswer const control = Demand(input);
    Show("warrior 80, control cohort", control);
    bool same = control.control && control.keeps.size() == normal.keeps.size();
    for (std::size_t i = 0; same && i < normal.keeps.size(); ++i)
        same = control.keeps[i].verdict == normal.keeps[i].verdict &&
               control.keeps[i].keepUnits == normal.keeps[i].keepUnits;
    Check(same, "control cohort: the same keep and sell answer, flagged");
}

// xorshift64: portable, the same sequence on every compiler
constexpr std::uint64_t SEED = 0x5EED9A0D3ULL;
std::uint64_t rngState = SEED;
std::vector<std::uint32_t> BuysOf(DemandAnswer const& answer, DemandSlot slot)
{
    std::vector<std::uint32_t> entries;
    for (DemandBuy const& buy : answer.buys)
        if (buy.slot == slot)
            entries.push_back(buy.entry);
    return entries;
}

// The buy pick (WI9b): a slot with a usable offered entry wants its stock; the units are wanted minus held; the
// offered entries strongest first; none for control; an offer is never kept
void BuyCases()
{
    Holding none;
    none.Offer(SUPERIOR_HEALING);
    DemandAnswer const empty = Demand(Bot(1, 60, Role::Melee, none));
    Show("warrior 60, no potion, Superior Healing offered", empty);
    DemandLine const& heal = LineOf(empty, DemandSlot::Heal);
    Check(heal.wantedStock == 6 && heal.buyEntry == SUPERIOR_HEALING && heal.buyUnits == 6 &&
              KeepOf(empty, SUPERIOR_HEALING).verdict == DemandVerdict::None,
          "buy: no potion and an offer: want 6, buy 3928 x6, the offer itself not kept");
    for (std::uint32_t const count : {4u, 6u})
    {
        Holding holding;
        holding.Offer(SUPERIOR_HEALING, count);
        DemandAnswer const answer = Demand(Bot(1, 60, Role::Melee, holding));
        Show("warrior 60, " + std::to_string(count) + " Superior Healing held and offered", answer);
        std::uint32_t const units = 6 - count;
        Check(LineOf(answer, DemandSlot::Heal).buyUnits == units &&
                  LineOf(answer, DemandSlot::Heal).buyEntry == (units ? SUPERIOR_HEALING : 0u) &&
                  KeepOf(answer, SUPERIOR_HEALING).keepUnits == count,
              "buy: holding " + std::to_string(count) + " of 6 buys " + std::to_string(units));
    }
    DemandInput control = Bot(1, 60, Role::Melee, none);
    control.control = true;
    DemandAnswer const controlled = Demand(control);
    Check(controlled.buys.empty() && LineOf(controlled, DemandSlot::Heal).buyUnits == 0 &&
              LineOf(controlled, DemandSlot::Heal).wantedStock == 6,
          "buy: the control cohort buys nothing (WI.10), its wanted stock unchanged");

    Holding ranked;
    ranked.Offer(SUPERIOR_HEALING);
    ranked.Offer(SUPER_HEALING);
    DemandAnswer const both = Demand(Bot(1, 60, Role::Melee, ranked));
    Show("warrior 60, Superior and Super Healing offered", both);
    std::vector<std::uint32_t> const order = BuysOf(both, DemandSlot::Heal);
    Check(order.size() == 2 && order[0] == SUPER_HEALING && order[1] == SUPERIOR_HEALING &&
              LineOf(both, DemandSlot::Heal).buyEntry == SUPER_HEALING,
          "buy: the offered entries strongest first (a vendor buys the first it sells)");

    Holding mixed;
    mixed.Offer(SUPERIOR_HEALING);
    mixed.Offer(SUPERIOR_MANA);
    DemandAnswer const mage = Demand(Bot(8, 60, Role::Caster, mixed));
    DemandAnswer const warrior = Demand(Bot(1, 60, Role::Melee, mixed));
    Show("mage 60, healing and mana offered", mage);
    Check(LineOf(mage, DemandSlot::Mana).buyEntry == SUPERIOR_MANA && LineOf(mage, DemandSlot::Mana).buyUnits == 6 &&
              LineOf(mage, DemandSlot::Heal).buyUnits == 6,
          "buy: a mage buys mana and healing potions");
    Check(LineOf(warrior, DemandSlot::Mana).buyUnits == 0 && LineOf(warrior, DemandSlot::Mana).wantedStock == 0 &&
              BuysOf(warrior, DemandSlot::Mana).empty(),
          "buy: a warrior buys no mana potion");

    DemandAnswer const young = Demand(Bot(1, 20, Role::Melee, none));
    Check(young.buys.empty() && LineOf(young, DemandSlot::Heal).wantedStock == 0,
          "buy: an offer above the bot's level is no pick and wants no stock");
}

std::uint32_t Next(std::uint32_t bound)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return bound == 0 ? 0 : static_cast<std::uint32_t>(rngState % bound);
}
float Unit() { return static_cast<float>(Next(2001)) / 1000.0f - 1.0f; }

// The usable rule, written again from the facts (D2 checks the header against it)
bool UsableFor(DemandInput const& input, std::size_t i, DemandSlot slot)
{
    Record const& record = input.records[i];
    UsableEntry const& entry = input.list[i];
    Needs const& needs = record.needs;
    if (!input.canUse[i] || input.level < needs.level || !entry.consumed || record.conjured)
        return false;
    if ((needs.classMask && !(needs.classMask & input.classBit)) ||
        (needs.raceMask && !(needs.raceMask & input.raceBit)))
        return false;
    if (needs.skill)
    {
        bool known = false;
        for (DemandSkill const& skill : input.skills)
            known = known || (skill.skill == needs.skill && skill.value >= needs.skillRank);
        if (!known)
            return false;
    }
    if (IsBuffDemandSlot(slot))
        return BuffScore(record, input.statWeights.size() >= BUFF_STAT_COUNT ? input.statWeights
                                                                             : RoleWeights(input.role)) > 0.0f;
    if (IsCoatDemandSlot(slot))
    {
        if (input.cls == 3 || input.cls == 7)
            return false;
        bool const fits = (input.mainHand.found && Detail::ItemFits(record, input.mainHand)) ||
                          (input.offHand.found && Detail::ItemFits(record, input.offHand));
        float score = needs.classMask ? 1.0f : 0.0f;
        std::span<float const> const weights = RoleWeights(input.role);
        for (std::size_t s = 0; s < BUFF_STAT_COUNT && i < input.coatStats.size(); ++s)
            if (weights[s] > 0.0f && input.coatStats[i][s] > 0.0f)
                score += weights[s] * input.coatStats[i][s];
        return fits && score > 0.0f;
    }
    if (slot == DemandSlot::Mana)
        return input.usesMana;
    return true;
}

bool SameAnswer(DemandAnswer const& a, DemandAnswer const& b)
{
    if (a.keeps.size() != b.keeps.size() || a.persistent != b.persistent || a.control != b.control)
        return false;
    for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
    {
        DemandLine const& x = a.lines[s];
        DemandLine const& y = b.lines[s];
        if (x.wantedStock != y.wantedStock || x.held != y.held || x.usePerHour != y.usePerHour ||
            x.measured != y.measured || x.worthCopperEach != y.worthCopperEach || x.bestEntry != y.bestEntry ||
            x.buyEntry != y.buyEntry || x.buyUnits != y.buyUnits)
            return false;
    }
    if (a.buys.size() != b.buys.size())
        return false;
    for (std::size_t i = 0; i < a.buys.size(); ++i)
        if (a.buys[i].slot != b.buys[i].slot || a.buys[i].entry != b.buys[i].entry)
            return false;
    for (std::size_t i = 0; i < a.keeps.size(); ++i)
    {
        DemandKeep const& x = a.keeps[i];
        DemandKeep const& y = b.keeps[i];
        if (x.entry != y.entry || x.slot != y.slot || x.keepUnits != y.keepUnits || x.held != y.held ||
            x.verdict != y.verdict || x.worthCopperEach != y.worthCopperEach)
            return false;
    }
    return true;
}

struct Trial
{
    Holding holding;
    std::vector<std::uint32_t> buy;
    DemandInput input;
};

Role RoleFor(std::uint8_t cls)
{
    std::vector<Role> roles;
    switch (cls)
    {
        case 1:
        case 6:
            roles = {Role::Tank, Role::Melee};
            break;
        case 2:
            roles = {Role::Tank, Role::Healer, Role::Melee};
            break;
        case 3:
            roles = {Role::Ranged};
            break;
        case 4:
            roles = {Role::Melee};
            break;
        case 5:
            roles = {Role::Healer, Role::Caster};
            break;
        case 7:
            roles = {Role::Healer, Role::Melee, Role::Caster};
            break;
        case 11:
            roles = {Role::Tank, Role::Healer, Role::Melee, Role::Caster};
            break;
        default:
            roles = {Role::Caster};
            break;
    }
    return roles[Next(static_cast<std::uint32_t>(roles.size()))];
}

TargetView RandomWeapon(bool offHand)
{
    struct Kind2
    {
        std::uint32_t sub;
        std::uint32_t inv;
    };
    static std::vector<Kind2> const kinds = {{15, 13}, {7, 13}, {4, 13}, {0, 13}, {13, 13}, {8, 17}, {10, 17}, {5, 17}};
    if (Next(100) < (offHand ? 55u : 10u))
        return TargetView{};
    Kind2 const kind = kinds[Next(offHand ? 5 : static_cast<std::uint32_t>(kinds.size()))];
    TargetView view = Weapon(kind.sub, kind.inv);
    view.itemLevel = static_cast<std::uint16_t>(5 + Next(250));
    view.requiredLevel = static_cast<std::uint16_t>(Next(81));
    return view;
}

// The pool's records per slot they can fill (with or without mana; a coating fills either hand): half the search's
// draws pick a slot first, so a slot with few records is drawn as often as one with many
std::array<std::vector<std::size_t>, DEMAND_SLOT_COUNT> slotPools;

void BuildSlotPools()
{
    for (std::size_t i = 0; i < pool.size(); ++i)
    {
        UsableEntry const entry = EntryOf(pool[i], 0, 1);
        std::array<bool, DEMAND_SLOT_COUNT> seen{};
        for (bool const mana : {false, true})
        {
            DemandSlot slot = DemandSlot::Heal;
            if (DemandSlotOf(entry, pool[i], mana, slot))
                seen[static_cast<std::size_t>(slot)] = true;
        }
        seen[static_cast<std::size_t>(DemandSlot::CoatOff)] = seen[static_cast<std::size_t>(DemandSlot::CoatMain)];
        for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
            if (seen[s])
                slotPools[s].push_back(i);
    }
}

void AddSkill(std::vector<DemandSkill>& skills, std::uint16_t skill)
{
    for (DemandSkill const& known : skills)
        if (known.skill == skill)
            return;
    skills.push_back({skill, static_cast<std::uint16_t>(Next(451))});
}

void MakeTrial(Trial& trial)
{
    std::uint8_t cls = static_cast<std::uint8_t>(1 + Next(11));
    if (cls == 10)
        cls = 11;
    std::uint8_t race = static_cast<std::uint8_t>(1 + Next(11));
    if (race == 9)
        race = 10;
    std::uint32_t const entries = 1 + Next(30);
    std::map<std::uint32_t, bool> canUse;  // one answer per item, as the engine gives
    for (std::uint32_t e = 0; e < entries; ++e)
    {
        std::size_t pick = Next(static_cast<std::uint32_t>(pool.size()));
        std::size_t const slot = Next(2) == 0 ? Next(static_cast<std::uint32_t>(DEMAND_SLOT_COUNT)) : DEMAND_SLOT_COUNT;
        if (slot < DEMAND_SLOT_COUNT && !slotPools[slot].empty())
            pick = slotPools[slot][Next(static_cast<std::uint32_t>(slotPools[slot].size()))];
        Record const& record = pool[pick];
        bool const usable = canUse.emplace(record.itemEntry, Next(100) < 90).first->second;
        trial.holding.Add(record, 1 + Next(Next(4) == 0 ? 60 : 12), usable);
        trial.holding.list.back().record = static_cast<std::uint32_t>(&record - pool.data());
        if (record.exclude == Exclude::Coating && !record.needs.classMask)
        {
            BuffWeights& stats = trial.holding.stats.back();
            stats = BuffWeights{};
            std::uint32_t const profile = record.itemEntry % 4;
            if (profile == 1)
                stats[static_cast<std::size_t>(BuffStat::AttackPower)] = 4.0f + record.itemEntry % 20;
            else if (profile == 2)
                stats[static_cast<std::size_t>(BuffStat::SpellPower)] = 4.0f + record.itemEntry % 30;
            else if (profile == 3)
                stats[static_cast<std::size_t>(BuffStat::ManaRegen)] = 2.0f + record.itemEntry % 10;
        }
        trial.buy.push_back(Next(3) == 0 ? 0 : record.sellCopper * (1 + Next(6)));
    }
    // Vendor offers (WI9b) of the live kinds (no coating, food or drink): a held entry flagged, the others a count of 0
    std::uint32_t const offers = Next(2) == 0 ? 1 + Next(8) : 0;
    for (std::uint32_t o = 0; o < offers; ++o)
    {
        std::size_t const slot = Next(static_cast<std::uint32_t>(DemandSlot::Food));
        if (slotPools[slot].empty())
            continue;
        Record const& record = pool[slotPools[slot][Next(static_cast<std::uint32_t>(slotPools[slot].size()))]];
        bool flagged = false;
        for (std::size_t i = 0; i < trial.holding.list.size(); ++i)
            if (trial.holding.list[i].item.entry == record.itemEntry)
            {
                trial.holding.offered[i] = 1;
                flagged = true;
            }
        if (flagged)
            continue;
        bool const usable = canUse.emplace(record.itemEntry, Next(100) < 90).first->second;
        trial.holding.Add(record, 0, usable);
        trial.holding.offered.back() = 1;
        trial.holding.list.back().record = static_cast<std::uint32_t>(&record - pool.data());
        trial.buy.push_back(record.sellCopper * (1 + Next(6)));
    }
    for (Record const& record : trial.holding.records)
        if (record.needs.skill && Next(2) == 0)
            AddSkill(trial.holding.skills, record.needs.skill);
    std::uint32_t const skills = Next(3);
    for (std::uint32_t s = 0; s < skills && !skillsSeen.empty(); ++s)
        AddSkill(trial.holding.skills, skillsSeen[Next(static_cast<std::uint32_t>(skillsSeen.size()))]);

    DemandInput& input = trial.input;
    input = Bot(cls, static_cast<std::uint8_t>(1 + Next(80)), RoleFor(cls), trial.holding, Unit());
    input.race = race;
    input.raceBit = 1u << (race - 1);
    input.daring = Unit();
    input.earnCopperPerMinute = 10 + Next(5000);
    input.deathCopper = DeathCopper(input.earnCopperPerMinute, Next(50000));
    input.control = Next(10) == 0;
    input.buyCopper = trial.buy;
    for (float& rate : input.upkeepUsePerHour)
        rate = Next(2) == 0 ? 0.0f : static_cast<float>(Next(1000)) / 100.0f;
    input.mainHand = RandomWeapon(false);
    input.offHand = input.mainHand.found && input.mainHand.itemInvType == 13 ? RandomWeapon(true) : TargetView{};
}

void Search()
{
    constexpr int TRIALS = 20000;
    BuildSlotPools();
    std::cout << "search: seed 0x" << std::hex << SEED << std::dec << ", " << TRIALS << " trials over " << pool.size()
              << " consumed records\n";
    std::array<int, 10> violations{};
    int buyLines = 0;
    int buyPicks = 0;
    int rebought = 0;
    std::array<int, 5> verdicts{};
    std::array<int, DEMAND_SLOT_COUNT> wanted{};
    int raised = 0;
    int permuted = 0;
    for (int trialIndex = 0; trialIndex < TRIALS; ++trialIndex)
    {
        Trial trial;
        MakeTrial(trial);
        DemandInput const& input = trial.input;
        DemandAnswer const answer = Demand(input);

        for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
            if (answer.lines[s].wantedStock > 0)
                ++wanted[s];
        for (DemandKeep const& keep : answer.keeps)
        {
            ++verdicts[static_cast<std::size_t>(keep.verdict)];
            DemandLine const& line = answer.lines[static_cast<std::size_t>(keep.slot)];
            if (keep.verdict == DemandVerdict::Surplus && line.held < line.wantedStock)
                ++violations[0];
            if (keep.verdict == DemandVerdict::Use || keep.verdict == DemandVerdict::Keep)
            {
                bool usable = false;
                std::uint32_t cap = 1;
                for (std::size_t i = 0; i < input.list.size(); ++i)
                    if (input.list[i].item.entry == keep.entry)
                    {
                        usable = usable || UsableFor(input, i, keep.slot);
                        Record const& record = input.records[i];
                        cap = std::max(
                            cap, record.price != PriceSource::None
                                     ? record.replaceCopper
                                     : std::max(trial.buy[i], record.sellCopper * WorldTravel::BUY_PER_SELL_PRICE));
                    }
                if (!usable)
                    ++violations[1];
                if (keep.worthCopperEach > cap)
                    ++violations[4];
                if (keep.worthCopperEach == 0)
                    ++violations[5];
            }
        }

        // D5..D7 (WI9b): a buy pick is a usable offered entry, its units wanted minus held, none when stocked or
        // for the control cohort
        for (DemandLine const& line : answer.lines)
        {
            buyLines += line.buyUnits > 0;
            if (line.buyUnits > 0 && line.held + line.buyUnits > line.wantedStock)
                ++violations[7];
            if (line.buyUnits > 0 && (line.held >= line.wantedStock || input.control))
                ++violations[8];
        }
        for (DemandBuy const& buy : answer.buys)
        {
            ++buyPicks;
            bool usable = false;
            for (std::size_t i = 0; i < input.list.size(); ++i)
                usable = usable ||
                         (input.list[i].item.entry == buy.entry && input.offered[i] && UsableFor(input, i, buy.slot));
            if (!usable)
                ++violations[6];
        }
        // D8: one pick bought (its units held), then asked again: the bought entry is kept
        if (!answer.buys.empty())
        {
            DemandBuy const& buy = answer.buys[Next(static_cast<std::uint32_t>(answer.buys.size()))];
            Holding after = trial.holding;
            for (UsableEntry& entry : after.list)
                if (entry.item.entry == buy.entry)
                    entry.count += answer.lines[static_cast<std::size_t>(buy.slot)].buyUnits;
            DemandInput boughtInput = input;
            boughtInput.list = after.list;
            DemandVerdict const verdict = KeepOf(Demand(boughtInput), buy.entry).verdict;
            ++rebought;
            if (verdict == DemandVerdict::Surplus || verdict == DemandVerdict::Unusable ||
                verdict == DemandVerdict::None)
                ++violations[9];
        }

        // D3: one slot's rate raised, then every slot's and every upkeep rate
        DemandInput one = input;
        std::size_t const slot = Next(static_cast<std::uint32_t>(DEMAND_SLOT_COUNT));
        one.slotUsePerHour[slot] = answer.lines[slot].usePerHour * 1.5f + 0.1f;
        DemandInput every = input;
        for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
            every.slotUsePerHour[s] = answer.lines[s].usePerHour * 1.5f + 0.1f;
        for (std::size_t k = 0; k < Planner::UPKEEP_STOCK_COUNT; ++k)
        {
            float const rate =
                input.upkeepUsePerHour[k] > 0.0f
                    ? input.upkeepUsePerHour[k]
                    : UpkeepNeeds::DefaultUsePerHour(input.cls, input.level, static_cast<Planner::UpkeepStock>(k));
            every.upkeepUsePerHour[k] = rate * 1.5f + 0.1f;
        }
        for (DemandInput const* raisedInput : {&one, &every})
        {
            DemandAnswer const higher = Demand(*raisedInput);
            ++raised;
            for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
                if (higher.lines[s].wantedStock < answer.lines[s].wantedStock)
                {
                    ++violations[2];
                    break;
                }
        }

        // D4: the same input again, and the list permuted
        bool same = SameAnswer(answer, Demand(input));
        Trial shuffled;
        shuffled.holding.skills = trial.holding.skills;
        std::vector<std::size_t> order(input.list.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        for (std::size_t i = order.size(); i > 1; --i)
            std::swap(order[i - 1], order[Next(static_cast<std::uint32_t>(i))]);
        for (std::size_t const i : order)
        {
            shuffled.holding.list.push_back(trial.holding.list[i]);
            shuffled.holding.records.push_back(trial.holding.records[i]);
            shuffled.holding.canUse.push_back(trial.holding.canUse[i]);
            shuffled.holding.stats.push_back(trial.holding.stats[i]);
            shuffled.holding.kinds.push_back(trial.holding.kinds[i]);
            shuffled.holding.offered.push_back(trial.holding.offered[i]);
            shuffled.buy.push_back(trial.buy[i]);
        }
        DemandInput permutedInput = input;
        permutedInput.skills = shuffled.holding.skills;
        permutedInput.list = shuffled.holding.list;
        permutedInput.records = shuffled.holding.records;
        permutedInput.canUse = shuffled.holding.canUse;
        permutedInput.coatStats = shuffled.holding.stats;
        permutedInput.poisonKinds = shuffled.holding.kinds;
        permutedInput.offered = shuffled.holding.offered;
        permutedInput.buyCopper = shuffled.buy;
        same = same && SameAnswer(answer, Demand(permutedInput));
        ++permuted;
        if (!same)
            ++violations[3];
    }

    char const* words[] = {"D1 sold_below_stock", "D2 kept_unusable", "D3 stock_falls",     "D4 not_same",
                           "W1 worth_above_cap",  "W2 kept_worth_0",  "D5 bought_unusable", "D6 buy_above_stock",
                           "D7 buy_when_stocked", "D8 buy_then_sell"};
    for (std::size_t i = 0; i < violations.size(); ++i)
        Check(violations[i] == 0, std::string("search: ") + words[i] + " = " + std::to_string(violations[i]));

    std::cout << "search: verdicts";
    for (std::size_t v = 0; v < verdicts.size(); ++v)
        std::cout << " " << DEMAND_VERDICT_WORDS[v] << "=" << verdicts[v];
    std::array<int, 2> kinds{};  // summon, resurrect records of the pool, for the print
    for (Record const& record : pool)
    {
        Kind const kind = Classify(record).kind;
        kinds[0] += kind == Kind::Summon;
        kinds[1] += kind == Kind::Resurrect;
    }
    std::cout << "; trials with a wanted stock per slot:";
    bool everySlot = true;
    std::string unreachable;
    for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
    {
        std::cout << " " << DEMAND_SLOT_WORDS[s] << "=" << wanted[s];
        everySlot = everySlot && (wanted[s] > 0 || slotPools[s].empty());
        if (slotPools[s].empty())
            unreachable += " " + std::string(DEMAND_SLOT_WORDS[s]);
    }
    std::cout << "; D3 comparisons " << raised << ", D4 comparisons " << permuted << "; buy lines " << buyLines
              << ", buy picks " << buyPicks << ", D8 rebuys " << rebought << "\n";
    std::cout << "search: slots no dump record fills:" << (unreachable.empty() ? " none" : unreachable) << " (pool "
              << kinds[0] << " summon and " << kinds[1]
              << " resurrect records; none answers a need: a summoned creature has sub none, WI12)\n";
    constexpr int FLOOR = 1000;
    Check(verdicts[0] > FLOOR && verdicts[1] > FLOOR && verdicts[2] > FLOOR && verdicts[3] > FLOOR && everySlot &&
              raised >= TRIALS && permuted >= TRIALS && buyLines > FLOOR && rebought > FLOOR,
          "search: not empty (use, keep, surplus and unusable above 1000, every slot with a wanted stock, D3 and D4 "
          "compared, buy lines and D8 rebuys above 1000)");
}
// Upkeep WU37a (WU.27): demand names the food, water and poison upkeep buys (StockWantOf). Vendor items by level
constexpr std::array<std::uint32_t, 6> VENDOR_FOODS = {4540, 4541, 4542, 4544, 4601, 8950};
constexpr std::array<std::uint32_t, 6> VENDOR_WATERS = {159, 1179, 1205, 1708, 1645, 8766};
constexpr std::array<std::uint32_t, 7> VENDOR_POISONS = {6947, 3775, 5237, 6949, 2892, 10918, 2893};
constexpr std::uint32_t CONJURED_WATER = 5350;
constexpr std::uint32_t ROGUE_LEVEL = 25;

TargetView Dagger(std::uint16_t itemLevel, std::uint16_t requiredLevel)
{
    TargetView view = Weapon(SUB_DAGGER, 13);
    view.itemLevel = itemLevel;
    view.requiredLevel = requiredLevel;
    return view;
}

void ShowWant(std::string const& title, UpkeepNeeds::StockWant const& want)
{
    std::cout << "  want " << title << ": answered " << want.answered << ", target " << want.target << ", held "
              << want.held << ", units " << want.units << ", items";
    for (std::size_t i = 0; i < want.items.size(); ++i)
        std::cout << " " << want.items[i] << " " << NameOf(want.items[i]) << " x" << want.itemUnits[i];
    std::cout << "\n";
}

void StockWantCases()
{
    using Planner::UpkeepStock;
    Holding empty;
    for (std::uint32_t const food : VENDOR_FOODS)
        if (Rec(food).needs.level <= 30)
            empty.Offer(food);
    DemandAnswer const warrior = Demand(Bot(1, 30, Role::Melee, empty));
    UpkeepNeeds::StockWant const food = StockWantOf(warrior, UpkeepStock::Food);
    ShowWant("warrior 30, no food, the foods up to 30 offered", food);
    Check(food.answered && food.held == 0 && food.target == LineOf(warrior, DemandSlot::Food).wantedStock &&
              food.target > 0 && food.units == food.target && food.items == BuysOf(warrior, DemandSlot::Food),
          "stock want: nothing held and an offer: the whole target, demand's buys strongest first");

    Holding two;
    two.Offer(4540, 3);
    two.Offer(4542, 2);
    two.Offer(4541);
    two.Offer(4544);
    DemandAnswer const twoFoods = Demand(Bot(1, ROGUE_LEVEL, Role::Melee, two));
    UpkeepNeeds::StockWant const twoWant = StockWantOf(twoFoods, UpkeepStock::Food);
    ShowWant("warrior 25, 3 Tough Hunk and 2 Moist Cornbread, the baker's four offered", twoWant);
    Check(twoWant.held == 5 && twoWant.units == twoWant.target - 5 && !twoWant.items.empty() &&
              twoWant.items.front() == 4544 && KeepOf(twoFoods, 4540).verdict == DemandVerdict::Use &&
              KeepOf(twoFoods, 4542).verdict == DemandVerdict::Use,
          "stock want (food-pick-A): below the target both foods are kept, the buy is demand's first (4544)");

    // Upkeep WU37b (one keeper): the scenario two-foods-A's bags, a priest at the target of 12 with the stronger food
    // alone; demand keeps the apples and leaves the bread surplus, the stock rule no longer keeps it
    Holding atTarget;
    atTarget.Add(4539, 12);
    atTarget.Add(4540, 2);
    DemandAnswer const priest = Demand(Bot(5, ROGUE_LEVEL, Role::Healer, atTarget));
    Show("priest 25, 12 Goldenbark Apple and 2 Tough Hunk of Bread", priest);
    Check(LineOf(priest, DemandSlot::Food).wantedStock == 12 && KeepOf(priest, 4539).verdict == DemandVerdict::Keep &&
              KeepOf(priest, 4539).keepUnits == 12 && KeepOf(priest, 4540).verdict == DemandVerdict::Surplus,
          "two-foods-A: at the target the stronger food is kept, the weaker one is surplus (sold by demand alone)");

    // Upkeep WU37c: the scenario drink-grade-H's bags, a level-32 priest with 2 Sweet Nectar and 4 Refreshing Spring
    // Water; a drink ranks by the mana its periodic dummy restores, so the nectar is the best and kept, and of the two
    // waters offered the nectar is bought
    constexpr std::uint32_t DRINK_PRIEST_LEVEL = 32;
    Holding drinks;
    drinks.Add(1708, 2);
    drinks.Add(159, 4);
    drinks.Offer(159);
    drinks.Offer(1708);
    DemandAnswer const thirsty = Demand(Bot(5, DRINK_PRIEST_LEVEL, Role::Healer, drinks));
    Show("priest 32, 2 Sweet Nectar and 4 Refreshing Spring Water, both offered", thirsty);
    UpkeepNeeds::StockWant const drinkWant = StockWantOf(thirsty, UpkeepStock::Water);
    ShowWant("priest 32 drink", drinkWant);
    DemandVerdict const nectar = KeepOf(thirsty, 1708).verdict;
    Check(Classify(Rec(1708)).rank > Classify(Rec(159)).rank && LineOf(thirsty, DemandSlot::Drink).bestEntry == 1708 &&
              (nectar == DemandVerdict::Use || nectar == DemandVerdict::Keep) && drinkWant.units > 0 &&
              !drinkWant.items.empty() && drinkWant.items.front() == 1708,
          "drink-grade-H: the stronger drink ranks higher, is the best, kept and bought first (1708 over 159)");

    Holding rogue;
    for (std::uint32_t const poison : VENDOR_POISONS)
        if (Rec(poison).needs.level <= ROGUE_LEVEL)
            rogue.Offer(poison);
    DemandInput rogueInput = Bot(4, ROGUE_LEVEL, Role::Melee, rogue);
    rogueInput.mainHand = Dagger(25, 20);  // Deadly Bronze Poniard 3490, the scenario's
    rogueInput.offHand = Dagger(28, 23);   // Cross Dagger 2819
    DemandAnswer const bare = Demand(rogueInput);
    Show("rogue 25, two daggers, no poison, the vendor's poisons up to 25 offered", bare);
    UpkeepNeeds::StockWant const poison = StockWantOf(bare, UpkeepStock::Poison);
    ShowWant("rogue 25 bare", poison);
    std::uint32_t const mainBuy = LineOf(bare, DemandSlot::CoatMain).buyEntry;
    std::uint32_t const offBuy = LineOf(bare, DemandSlot::CoatOff).buyEntry;
    Check(poison.answered && mainBuy && !poison.items.empty() && poison.items.front() == mainBuy &&
              poison.units == poison.target && poison.target > 0,
          "stock want (rogue-poison): one poison need, the main hand's pick first, the whole stock over both hands");
    Check(!offBuy || offBuy == mainBuy ||
              std::find(poison.items.begin(), poison.items.end(), offBuy) != poison.items.end(),
          "stock want (rogue-poison): the off hand's pick is among the need's items");
    std::cout << "  rogue-poison scenario item: " << mainBuy << " " << NameOf(mainBuy) << " (off hand " << offBuy
              << ")\n";

    Holding conjured;
    conjured.Add(CONJURED_WATER, 20);
    for (std::uint32_t const water : VENDOR_WATERS)
        if (Rec(water).needs.level <= 30)
            conjured.Offer(water);
    DemandAnswer const mage = Demand(Bot(8, 30, Role::Caster, conjured));
    UpkeepNeeds::StockWant const water = StockWantOf(mage, UpkeepStock::Water);
    ShowWant("mage 30, 20 Conjured Water, the waters up to 30 offered", water);
    Check(KeepOf(mage, CONJURED_WATER).verdict == DemandVerdict::None && water.answered && water.held == 0 &&
              water.target == 0 && water.units == 0 && water.items.empty(),
          "stock want: a mage's conjured water is not demand's (it makes it), and upkeep's rate wants no vendor water "
          "of a mage (0 an hour): no water need, as before");

    DemandInput control = Bot(1, 30, Role::Melee, empty);
    control.control = true;
    Check(
        !StockWantOf(Demand(control), UpkeepStock::Food).answered && !StockWantOf(warrior, UpkeepStock::Ammo).answered,
        "stock want: the control cohort and the other kinds are not answered (the offers by level)");
}

// The WU37a invariant over random bots, bags and vendors, the seed logged: for food, water and poison every item the
// shopping list buys is one demand keeps after the purchase (Use or Keep); the stock never passes the target plus one
// lot; a bot that holds nothing usable of the kind gets a want when a usable offer exists
void StockWantInvariant(std::uint64_t seed)
{
    using Planner::UpkeepStock;
    std::cout << "Scenario: the stock want invariant, seed " << seed << "\n";
    rngState = seed ? seed : SEED;
    std::vector<std::uint32_t> entries(VENDOR_FOODS.begin(), VENDOR_FOODS.end());
    entries.insert(entries.end(), VENDOR_WATERS.begin(), VENDOR_WATERS.end());
    entries.insert(entries.end(), VENDOR_POISONS.begin(), VENDOR_POISONS.end());
    entries.push_back(CONJURED_WATER);
    constexpr std::array<std::uint8_t, 6> CLASSES = {1, 4, 4, 5, 8, 11};
    constexpr std::array<UpkeepStock, 3> KINDS = {UpkeepStock::Food, UpkeepStock::Water, UpkeepStock::Poison};
    constexpr std::uint32_t BOTS = 3000;
    std::string broken;
    std::array<std::uint32_t, 3> bought{};
    std::uint32_t split = 0;
    std::uint32_t empties = 0;
    for (std::uint32_t trial = 0; trial < BOTS && broken.empty(); ++trial)
    {
        std::uint8_t const cls = CLASSES[Next(static_cast<std::uint32_t>(CLASSES.size()))];
        std::uint8_t const level = static_cast<std::uint8_t>(1 + Next(60));
        Role const role = RoleFor(cls);
        float const thrift = Unit();
        TargetView const main = RandomWeapon(false);
        TargetView const off = RandomWeapon(true);
        std::map<std::uint32_t, std::uint32_t> held;
        for (std::uint32_t const entry : entries)
            if (Next(100) < 25)
                held[entry] = 1 + Next(30);
        auto const answerNow = [&]()
        {
            Holding holding;
            for (std::uint32_t const entry : entries)
            {
                bool const offered = entry != CONJURED_WATER && Rec(entry).needs.level <= level;
                std::uint32_t const count = held.count(entry) ? held[entry] : 0;
                if (offered)
                    holding.Offer(entry, count);
                else if (count)
                    holding.Add(entry, count);
            }
            DemandInput input = Bot(cls, level, role, holding, thrift);
            input.mainHand = main;
            input.offHand = off;
            return Demand(input);
        };
        for (std::uint32_t visit = 0; visit < 3 && broken.empty(); ++visit)
        {
            DemandAnswer const before = answerNow();
            for (std::size_t k = 0; k < KINDS.size(); ++k)
            {
                UpkeepStock const kind = KINDS[k];
                UpkeepNeeds::StockWant const want = StockWantOf(before, kind);
                bool const usableOffer =
                    kind == UpkeepStock::Poison
                        ? LineOf(before, DemandSlot::CoatMain).wantedStock ||
                              LineOf(before, DemandSlot::CoatOff).wantedStock
                        : LineOf(before, kind == UpkeepStock::Food ? DemandSlot::Food : DemandSlot::Drink).wantedStock;
                if (visit == 0 && want.held == 0 && usableOffer && (!want.units || want.items.empty()))
                    broken = "nothing held, a usable offer, no want";
                std::set<DemandSlot> slots;
                for (std::uint32_t const item : want.items)
                    for (DemandBuy const& buy : before.buys)
                        if (buy.entry == item && IsCoatDemandSlot(buy.slot))
                            slots.insert(buy.slot);
                split += slots.size() > 1 ? 1 : 0;
                // A vendor sells each of demand's items with 70 %; lots of 1, 5 or 20 by entry
                std::size_t pick = want.items.size();
                for (std::size_t i = 0; i < want.items.size() && pick == want.items.size(); ++i)
                    if (Next(100) < 70)
                        pick = i;
                if (!want.units || pick == want.items.size() || !broken.empty())
                    continue;
                std::uint32_t const item = want.items[pick];
                std::uint32_t const lot = std::array<std::uint32_t, 3>{1, 5, 20}[item % 3];
                std::uint32_t const units = (want.itemUnits[pick] + lot - 1) / lot * lot;
                held[item] += units;
                ++bought[k];
                DemandAnswer const after = answerNow();
                DemandVerdict const verdict = VerdictOf(after, item);
                UpkeepNeeds::StockWant const now = StockWantOf(after, kind);
                if (verdict != DemandVerdict::Use && verdict != DemandVerdict::Keep)
                    broken = "bought " + std::to_string(item) + " x" + std::to_string(units) + " is " +
                             std::string(DemandVerdictWord(verdict)) + " after the purchase";
                else if (now.held > want.target + lot)
                    broken = "held " + std::to_string(now.held) + " past the target " + std::to_string(want.target) +
                             " plus a lot of " + std::to_string(lot);
            }
        }
        if (!broken.empty())
            std::cout << "   counterexample at bot " << trial << " (class " << int(cls) << ", level " << int(level)
                      << "): " << broken << "\n";
        empties += held.empty() ? 1 : 0;
    }
    std::cout << "  bought food " << bought[0] << ", water " << bought[1] << ", poison " << bought[2]
              << "; poison wants over both hands " << split << "; bots with empty bags " << empties << "\n";
    Check(broken.empty() && bought[0] > 300 && bought[1] > 300 && bought[2] > 100 && split > 10,
          "stock want invariant at seed " + std::to_string(seed) +
              ": no counterexample (food, water and poison bought above the floors, two-hand wants seen)");
}
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    std::uint64_t const stockSeed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2811;
    bool const loaded = Load(root);
    Check(loaded, "the dump opens and has consumed records (" + root + ")");
    if (loaded)
    {
        PrintTables();
        HealCases();
        ManaCases();
        BuffCases();
        FoodCases();
        UpkeepCases();
        CoatCases();
        SkillCases();
        WorthCases();
        CohortCases();
        BuyCases();
        Search();
        StockWantCases();
        StockWantInvariant(stockSeed);
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
