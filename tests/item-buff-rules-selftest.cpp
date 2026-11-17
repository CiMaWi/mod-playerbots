/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI8a (INTERFACE 1.12): the buff picker of ItemBuffRules.h on records of the committed dump. Hand
// cases print the picks per role for the holdings of scenario wi-buff block A (the plan's readable proof: the
// scenario expects what this prints), what excludes what, the held auras' slots, the worth gate; then a seeded
// random search over the dump's buff records counts the invariants B1..B5, each of which must stay 0.

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ItemBuffRules.h"
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

std::map<std::uint32_t, Record> byEntry;  // the first buff record of each item
std::vector<Record> pool;                 // every buff record of the dump
std::map<std::uint32_t, std::string> names;

std::string byEntryName(std::uint32_t entry)
{
    auto const it = names.find(entry);
    return it == names.end() ? std::string("?") : it->second;
}

constexpr std::uint32_t FOOD = 34754;  // Mega Mammoth Meal

Record Rec(std::uint32_t entry)
{
    auto const it = byEntry.find(entry);
    if (it == byEntry.end())
    {
        Check(false, "the dump has a buff record of item " + std::to_string(entry));
        return Record{};
    }
    return it->second;
}

UsableEntry Entry(Record const& record, std::uint32_t id)
{
    UsableEntry entry;
    entry.record = id;
    entry.item.entry = record.itemEntry;
    entry.kind = Kind::LongBuff;
    entry.count = 1;
    entry.consumed = record.consumed;
    entry.replaceCopper = record.replaceCopper;
    entry.ready = true;
    return entry;
}

struct Holding
{
    std::vector<UsableEntry> list;
    std::vector<Record> records;
    std::vector<std::uint32_t> groups;  // the held auras
    std::vector<std::int32_t> amounts;
};

Holding Hold(std::vector<std::uint32_t> const& entries)
{
    Holding holding;
    for (std::uint32_t const entry : entries)
    {
        holding.records.push_back(Rec(entry));
        holding.list.push_back(Entry(holding.records.back(), static_cast<std::uint32_t>(holding.list.size())));
    }
    return holding;
}

void HoldAura(Holding& holding, Record const& record, std::int32_t amount)
{
    for (std::size_t i = 0; i < record.groupCount && i < MAX_GROUPS; ++i)
    {
        holding.groups.push_back(record.groups[i]);
        holding.amounts.push_back(amount);
    }
}

BuffBot Bot(Role role, Ahead ahead, Holding const& holding, std::uint32_t earn = 1000)
{
    BuffBot bot;
    bot.role = role;
    bot.level = 80;
    bot.ahead = ahead;
    bot.auraGroups = holding.groups;
    bot.auraAmounts = holding.amounts;
    bot.earnCopperPerMinute = earn;
    bot.deathCopper = DeathCopper(earn, 0);
    return bot;
}

std::vector<BuffPick> Pick(BuffBot const& bot, Holding const& holding)
{
    return PickBuffs(bot, holding.list, holding.records);
}

std::string Show(std::vector<BuffPick> const& picks, Holding const& holding)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    for (BuffPick const& pick : picks)
        out << "\n    " << BuffChoice(pick) << " " << holding.list[pick.usable].item.entry << " ("
            << byEntryName(holding.list[pick.usable].item.entry) << ") " << BuffReasonWord(pick.reason)
            << " score=" << pick.score << " saves=" << pick.verdict.savesCopper << " cost=" << pick.verdict.costCopper;
    return out.str();
}

// The entries a pick list uses, in slot order
std::vector<std::uint32_t> Used(std::vector<BuffPick> const& picks, Holding const& holding)
{
    std::vector<std::uint32_t> used;
    for (BuffPick const& pick : picks)
        if (pick.use)
            used.push_back(holding.list[pick.usable].item.entry);
    return used;
}

bool HasPick(std::vector<BuffPick> const& picks, Exclude slot, bool use, BuffReason reason)
{
    for (BuffPick const& pick : picks)
        if (pick.slot == slot && pick.use == use && pick.reason == reason)
            return true;
    return false;
}
bool Load(std::string const& root)
{
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line))
        if (std::optional<DumpRow> const row = ParseDumpLine(line))
            if (IsBuffSlot(row->record.exclude) && row->record.source == Source::ItemSpell)
            {
                pool.push_back(row->record);
                byEntry.emplace(row->record.itemEntry, row->record);
                names.emplace(row->record.itemEntry, row->name);
            }
    return !pool.empty();
}

void PrintTables()
{
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "tables: Ahead (none dungeon elite group bg) danger/share/minutes:";
    for (std::size_t i = 0; i < AHEAD_COUNT; ++i)
        std::cout << " " << AHEAD_WORDS[i] << "=" << AHEAD_DANGER[i] << "/" << AHEAD_SHARE[i] << "/"
                  << AHEAD_MINUTES[i];
    std::cout << "; well-fed minutes " << WELL_FED_MINUTES << "\n";
    char const* roles[] = {"tank", "healer", "melee", "ranged", "caster"};
    for (std::size_t r = 0; r < ROLE_WEIGHTS.size(); ++r)
    {
        std::cout << "weights " << roles[r] << ":";
        for (std::size_t s = 0; s < BUFF_STAT_COUNT; ++s)
            if (ROLE_WEIGHTS[r][s] > 0.0f)
                std::cout << " " << BUFF_STAT_WORDS[s] << "=" << ROLE_WEIGHTS[r][s];
        std::cout << "\n";
    }
}

void ExcludeCases()
{
    Record const flask = Rec(46377);
    Record const battle = Rec(39666);
    Record const battle2 = Rec(40073);
    Record const guardian = Rec(40078);
    Record const strength = Rec(43466);
    Record const intellect = Rec(37092);
    Record const food = Rec(FOOD);
    Record otherFood{};
    for (Record const& record : pool)
        if (record.exclude == Exclude::WellFed && record.itemEntry != FOOD)
        {
            otherFood = record;
            break;
        }
    Check(Excludes(flask, battle) && Excludes(battle, flask), "excludes: a flask and a battle elixir");
    Check(Excludes(flask, guardian), "excludes: a flask and a guardian elixir");
    Check(Excludes(flask, Rec(46379)), "excludes: two flasks");
    Check(!Excludes(battle, guardian), "excludes: a battle and a guardian elixir stack");
    Check(Excludes(battle, battle2), "excludes: two battle elixirs");
    Check(Excludes(food, otherFood), "excludes: two buff foods");
    Check(Excludes(strength, intellect), "excludes: two scrolls of different stats (group 1087, the probe)");
    Check(!Excludes(strength, flask) && !Excludes(food, guardian) && !Excludes(food, strength),
          "excludes: a scroll, a food and a flask or elixir stack");

    BuffWeights const stats = BuffStats(food);
    Check(stats[static_cast<std::size_t>(BuffStat::AttackPower)] == 80.0f &&
              stats[static_cast<std::size_t>(BuffStat::Stamina)] == 40.0f &&
              stats[static_cast<std::size_t>(BuffStat::Health)] == 0.0f,
          "stats: Mega Mammoth Meal is 80 ap, 80 rap, 40 sta from its triggered spell; the eating adds nothing");
    Check(BuffMinutes(food) == WELL_FED_MINUTES && BuffMinutes(flask) == 60.0f && BuffMinutes(strength) == 30.0f,
          "minutes: buff food by WELL_FED_MINUTES, flask 60, scroll 30");
    Ahead ahead = Ahead::None;
    Check(AheadOfWord("bg", ahead) && ahead == Ahead::Battleground && AheadWord(Ahead::Elite) == "elite" &&
              !AheadOfWord("raid", ahead),
          "ahead words");
}

struct RoleCase
{
    char const* name;
    Role role;
    std::uint32_t flask;
    std::uint32_t battle;
    std::uint32_t guardian;
    std::uint32_t scroll;
};

// wi-buff block A's holdings per role (scratchpad/item-use/gen-wi-buff.py R)
constexpr RoleCase ROLE_CASES[] = {
    {"tank", Role::Tank, 46379, 40073, 40078, 37094},       {"healer", Role::Healer, 46376, 40070, 40109, 37092},
    {"melee-agi", Role::Melee, 46377, 39666, 40078, 43464}, {"melee-str", Role::Melee, 46377, 40073, 40078, 43466},
    {"ranged", Role::Ranged, 46377, 39666, 40078, 43464},   {"caster", Role::Caster, 46376, 40070, 44332, 37092},
};

Holding RoleHolding(RoleCase const& c) { return Hold({c.flask, c.battle, c.guardian, c.scroll, FOOD}); }

void RoleCases()
{
    for (RoleCase const& c : ROLE_CASES)
    {
        Holding const holding = RoleHolding(c);
        std::vector<BuffPick> const picks = Pick(Bot(c.role, Ahead::Dungeon, holding), holding);
        std::cout << "pick ahead-" << c.name << " (dungeon, earn 1000):" << Show(picks, holding) << "\n";
        std::vector<std::uint32_t> const want = {c.flask, FOOD, c.scroll};
        Check(Used(picks, holding) == want, std::string("ahead-") + c.name + ": the flask, the food and the scroll");
        Check(HasPick(picks, Exclude::Flask, true, BuffReason::AheadDungeon) && picks.size() == 3,
              std::string("ahead-") + c.name + ": use:flask ahead_dungeon, no elixir row");
    }

    // Flask of Pure Mojo against Spellpower Elixir and Mighty Mageblood: the elixirs score more, so the earning
    // rate decides (at 1000 the 2 g elixirs are not worth it): why block A's healer holds Frost Wyrm instead
    Holding const mojo = Hold({46378, 40070, 40109, 37092, FOOD});
    std::vector<BuffPick> const low = Pick(Bot(Role::Healer, Ahead::Dungeon, mojo), mojo);
    std::vector<BuffPick> const high = Pick(Bot(Role::Healer, Ahead::Dungeon, mojo, 2000), mojo);
    std::cout << "pick healer Pure Mojo, earn 1000:" << Show(low, mojo) << "\n";
    std::cout << "pick healer Pure Mojo, earn 2000:" << Show(high, mojo) << "\n";
    Check(HasPick(low, Exclude::Flask, true, BuffReason::AheadDungeon),
          "mojo: at earn 1000 the flask (elixirs not worth)");
    Check(HasPick(high, Exclude::BattleElixir, true, BuffReason::AheadDungeon) &&
              HasPick(high, Exclude::GuardianElixir, true, BuffReason::AheadDungeon) && high.size() == 4,
          "mojo: at earn 2000 the two elixirs (118 > 90), no flask row");

    for (auto const& [ahead, word] :
         {std::pair{Ahead::Elite, "elite"}, std::pair{Ahead::Group, "group"}, std::pair{Ahead::Battleground, "bg"}})
    {
        Holding const holding = RoleHolding(ROLE_CASES[2]);
        std::vector<BuffPick> const picks = Pick(Bot(Role::Melee, ahead, holding), holding);
        std::cout << "pick ahead-" << word << " (melee-agi):" << Show(picks, holding) << "\n";
        Check(!Used(picks, holding).empty() && HasPick(picks, Exclude::Scroll, true, AheadReason(ahead)),
              std::string("ahead-") + word + ": uses something, the scroll with ahead_" + word);
    }
    {
        Holding const holding = RoleHolding(ROLE_CASES[2]);
        std::vector<BuffPick> const picks = Pick(Bot(Role::Melee, Ahead::Elite, holding), holding);
        Check(HasPick(picks, Exclude::Flask, false, BuffReason::NotWorth),
              "ahead-elite: a flask is not worth 5 minutes");
    }

    for (RoleCase const& c : ROLE_CASES)
    {
        Holding const holding = RoleHolding(c);
        std::vector<BuffPick> const none = Pick(Bot(c.role, Ahead::None, holding), holding);
        BuffBot control = Bot(c.role, Ahead::Dungeon, holding);
        control.control = true;
        std::vector<BuffPick> const kept = Pick(control, holding);
        bool noneOk = !none.empty();
        for (BuffPick const& pick : none)
            noneOk = noneOk && !pick.use && pick.reason == BuffReason::NothingAhead;
        bool controlOk = !kept.empty();
        for (BuffPick const& pick : kept)
            controlOk = controlOk && !pick.use && pick.reason == BuffReason::Control;
        Check(noneOk, std::string("none-") + c.name + ": every row skip nothing_ahead");
        Check(controlOk, std::string("control-") + c.name + ": every row skip control");
    }
}

void AuraCases()
{
    RoleCase const& str = ROLE_CASES[3];
    // scroll-weaker: Scroll of Strength VII (25) under the aura of VIII (spell 58449, 30)
    Holding weaker = Hold({str.flask, str.battle, str.guardian, 43465, FOOD});
    HoldAura(weaker, Rec(43466), 30);
    std::vector<BuffPick> picks = Pick(Bot(Role::Melee, Ahead::Dungeon, weaker), weaker);
    std::cout << "pick scroll-weaker (melee-str):" << Show(picks, weaker) << "\n";
    Check(HasPick(picks, Exclude::Scroll, false, BuffReason::Excluded) &&
              Used(picks, weaker) == std::vector<std::uint32_t>{str.flask, FOOD},
          "scroll-weaker: skip:scroll excluded, the flask and the food still used");

    Holding stronger = Hold({str.flask, 43466, FOOD});
    HoldAura(stronger, Rec(43465), 25);
    picks = Pick(Bot(Role::Melee, Ahead::Dungeon, stronger), stronger);
    Check(HasPick(picks, Exclude::Scroll, true, BuffReason::AheadDungeon), "scroll-stronger: VIII replaces a held VII");

    Holding intellect = Hold({37092});
    HoldAura(intellect, Rec(37092), 48);  // the same scroll is on
    picks = Pick(Bot(Role::Caster, Ahead::Dungeon, intellect), intellect);
    Check(HasPick(picks, Exclude::Scroll, false, BuffReason::Excluded), "scroll-equal: the same scroll is not renewed");

    Holding flaskOn = RoleHolding(str);
    HoldAura(flaskOn, Rec(str.flask), 180);
    picks = Pick(Bot(Role::Melee, Ahead::Dungeon, flaskOn), flaskOn);
    std::cout << "pick flask on (melee-str):" << Show(picks, flaskOn) << "\n";
    Check(HasPick(picks, Exclude::Flask, false, BuffReason::Excluded) &&
              Used(picks, flaskOn) == std::vector<std::uint32_t>{FOOD, str.scroll},
          "flask-on: no flask and no elixir over a held flask; the food and the scroll");

    Holding battleOn = Hold({46379, 40073, 40078, FOOD});
    HoldAura(battleOn, Rec(40073), 50);
    picks = Pick(Bot(Role::Tank, Ahead::Dungeon, battleOn, 3000), battleOn);
    std::cout << "pick battle elixir on (tank, earn 3000):" << Show(picks, battleOn) << "\n";
    Check(HasPick(picks, Exclude::BattleElixir, false, BuffReason::Excluded) &&
              HasPick(picks, Exclude::GuardianElixir, true, BuffReason::AheadDungeon) && picks.size() == 3,
          "battle-on: the flask would remove it, the guardian elixir is added");

    Holding foodOn = Hold({FOOD});
    HoldAura(foodOn, Rec(FOOD), 80);
    picks = Pick(Bot(Role::Melee, Ahead::Dungeon, foodOn), foodOn);
    Check(HasPick(picks, Exclude::WellFed, false, BuffReason::Excluded), "food-on: no second well-fed");
}

void GateCases()
{
    RoleCase const& agi = ROLE_CASES[2];
    Holding const holding = RoleHolding(agi);
    BuffBot young = Bot(Role::Melee, Ahead::Dungeon, holding, 3000);
    young.level = 72;  // the flask needs 75, the elixirs 70
    std::vector<BuffPick> picks = Pick(young, holding);
    std::cout << "pick level 72 (melee-agi, earn 3000):" << Show(picks, holding) << "\n";
    Check(HasPick(picks, Exclude::BattleElixir, true, BuffReason::AheadDungeon) &&
              HasPick(picks, Exclude::GuardianElixir, true, BuffReason::AheadDungeon) && picks.size() == 3,
          "level: below the flask's level (75) the two elixirs; the level 80 scroll is no candidate");

    BuffBot thrifty = Bot(Role::Melee, Ahead::Dungeon, holding);
    thrifty.thrift = 1.0f;
    picks = Pick(thrifty, holding);
    Check(HasPick(picks, Exclude::Flask, false, BuffReason::NotWorth),
          "thrift: a thrifty bot keeps the flask at 1000/min");

    BuffBot poor = Bot(Role::Melee, Ahead::Dungeon, holding, 0);
    picks = Pick(poor, holding);
    Check(Used(picks, holding).empty(), "earn 0: nothing is worth it");

    Holding unready = holding;
    unready.list[0].ready = false;
    picks = Pick(Bot(Role::Melee, Ahead::Dungeon, unready, 3000), unready);
    Check(HasPick(picks, Exclude::BattleElixir, true, BuffReason::AheadDungeon),
          "ready: a flask not ready leaves the elixirs");

    BuffWeights agiOnly{};
    agiOnly[static_cast<std::size_t>(BuffStat::Agility)] = 1.0f;
    BuffBot custom = Bot(Role::Melee, Ahead::Dungeon, holding, 3000);
    custom.statWeights = agiOnly;
    picks = Pick(custom, holding);
    Check(Used(picks, holding) == std::vector<std::uint32_t>{agi.battle, agi.scroll},
          "weights: a span of only agility uses the agility elixir and scroll, nothing else");

    std::vector<BuffPick> const a = Pick(Bot(Role::Melee, Ahead::Dungeon, holding), holding);
    std::vector<BuffPick> const b = Pick(Bot(Role::Melee, Ahead::Dungeon, holding), holding);
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i)
        same = a[i].usable == b[i].usable && a[i].use == b[i].use && a[i].reason == b[i].reason;
    Check(same, "deterministic: the same input gives the same picks");
}

// xorshift64: portable, the same sequence on every compiler
std::uint64_t rngState = 0x8A2B0FF5EEDULL;
std::uint32_t Next(std::uint32_t bound)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return bound == 0 ? 0 : static_cast<std::uint32_t>(rngState % bound);
}
float Unit() { return static_cast<float>(Next(2001)) / 1000.0f - 1.0f; }

void Search()
{
    constexpr int TRIALS = 20000;
    std::array<int, 5> violations{};  // B1..B5
    int uses = 0;
    int skips = 0;
    std::array<int, BUFF_SLOTS.size()> slotUses{};
    std::array<int, BUFF_REASON_COUNT> reasons{};
    int flaskVsElixir = 0;  // trials holding both a usable flask and a usable elixir
    for (int trial = 0; trial < TRIALS; ++trial)
    {
        Holding holding;
        std::uint32_t const entries = 1 + Next(10);
        for (std::uint32_t i = 0; i < entries; ++i)
        {
            holding.records.push_back(pool[Next(static_cast<std::uint32_t>(pool.size()))]);
            UsableEntry entry = Entry(holding.records.back(), i);
            entry.ready = Next(100) < 85;
            entry.count = Next(100) < 10 ? 0 : 1 + Next(3);
            holding.list.push_back(entry);
        }
        std::uint32_t const auras = Next(4);
        for (std::uint32_t i = 0; i < auras; ++i)
        {
            Record const& aura = pool[Next(static_cast<std::uint32_t>(pool.size()))];
            HoldAura(holding, aura, BuffAmount(aura) + static_cast<std::int32_t>(Next(21)) - 10);
        }
        BuffBot bot;
        bot.role = static_cast<Role>(Next(5));
        bot.level = static_cast<std::uint8_t>(50 + Next(31));
        bot.ahead = Next(100) < 20 ? Ahead::None : static_cast<Ahead>(1 + Next(4));
        bot.aheadMinutes = static_cast<float>(Next(91));
        bot.auraGroups = holding.groups;
        bot.auraAmounts = holding.amounts;
        BuffWeights random{};
        if (Next(100) < 30)
        {
            for (float& weight : random)
                weight = Next(2) ? 0.0f : Unit() + 1.0f;
            bot.statWeights = random;
        }
        bot.thrift = Unit();
        bot.daring = Unit();
        bot.earnCopperPerMinute = Next(4001);
        bot.deathCopper = DeathCopper(bot.earnCopperPerMinute, Next(20000));
        bot.control = Next(100) < 10;

        std::span<float const> const weights =
            bot.statWeights.size() >= BUFF_STAT_COUNT ? bot.statWeights : RoleWeights(bot.role);
        auto const usable = [&](std::size_t i)
        {
            Record const& record = holding.records[i];
            return holding.list[i].count > 0 && holding.list[i].ready && record.needs.level <= bot.level &&
                   BuffScore(record, weights) > 0.0f;
        };
        bool heldFlask = false;
        bool heldElixir = false;
        for (std::size_t i = 0; i < holding.records.size(); ++i)
            if (usable(i))
            {
                heldFlask = heldFlask || holding.records[i].exclude == Exclude::Flask;
                heldElixir = heldElixir || holding.records[i].exclude == Exclude::BattleElixir ||
                             holding.records[i].exclude == Exclude::GuardianElixir;
            }
        flaskVsElixir += heldFlask && heldElixir;

        std::vector<BuffPick> const picks = PickBuffs(bot, holding.list, holding.records);
        std::vector<std::size_t> used;
        for (BuffPick const& pick : picks)
        {
            ++reasons[static_cast<std::size_t>(pick.reason)];
            if (!pick.use)
            {
                ++skips;
                continue;
            }
            ++uses;
            ++slotUses[static_cast<std::size_t>(BuffSlotIndex(pick.slot))];
            Record const& record = holding.records[pick.usable];
            float const score = BuffScore(record, weights);
            if (SlotTaken(bot, record))
                ++violations[0];  // B1 against a held aura
            for (std::size_t const other : used)
                if (Excludes(record, holding.records[other]))
                    ++violations[0];  // B1 between two picks
            if (!(score > 0.0f))
                ++violations[1];
            for (std::size_t i = 0; i < holding.records.size(); ++i)
                if (i != pick.usable && holding.records[i].exclude == record.exclude && usable(i) &&
                    BuffScore(holding.records[i], weights) > score)
                    ++violations[2];
            if (bot.ahead == Ahead::None)
                ++violations[3];
            if (bot.control)
                ++violations[4];
            used.push_back(pick.usable);
        }
    }
    std::cout << "search: " << TRIALS << " trials over " << pool.size() << " buff records, " << uses << " uses, "
              << skips << " skips, " << flaskVsElixir << " with a flask and an elixir; uses per slot:";
    for (std::size_t i = 0; i < BUFF_SLOTS.size(); ++i)
        std::cout << " " << ExcludeWord(BUFF_SLOTS[i]) << "=" << slotUses[i];
    std::cout << "; rows per reason:";
    for (std::size_t i = 0; i < BUFF_REASON_COUNT; ++i)
        std::cout << " " << BUFF_REASON_WORDS[i] << "=" << reasons[i];
    std::cout << "\n";
    char const* words[] = {"B1 exclusive_pair", "B2 useless_pick", "B3 weaker_pick", "B4 nothing_ahead",
                           "B5 control_pick"};
    for (std::size_t i = 0; i < violations.size(); ++i)
        Check(violations[i] == 0, std::string("search: ") + words[i] + " = " + std::to_string(violations[i]));
    bool everySlot = true;
    for (int const count : slotUses)
        everySlot = everySlot && count > 0;
    Check(uses > 1000 && skips > 1000 && everySlot && flaskVsElixir > 500,
          "search: not empty (uses, skips, every slot used, flask against elixirs)");
}
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    bool const loaded = Load(root);
    Check(loaded, "the dump opens and has buff records (" + root + ")");
    if (loaded)
    {
        PrintTables();
        ExcludeCases();
        RoleCases();
        AuraCases();
        GateCases();
        Search();
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
