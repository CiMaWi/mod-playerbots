/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the class-quest rules (upkeep WU20a, WU39, 3.11): the table's states, one state per class quest and
// bot, the chain's next step, the unlock bonus and who is given what (DueQuests), with WU39's invariant (no bot stalls
// on a class-quest spell) over generated bots on a hand-made fixture and on lab7's joined rows (seed printed; argv[1]
// overrides it), and the named cases with their judgement and due list.
// Build (module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/class-quest-selftest.cpp

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ClassQuestJoin.h"
#include "ClassQuestRules.h"
#include "GoalInterface.h"
#include "GoalModelHarness.h"
#include "QuestCircuitLog.h"
#include "QuestFailBook.h"

using namespace Planner::ClassQuests;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? passed : failed)++;
}

constexpr std::uint32_t WARRIOR = 1, PALADIN = 2, HUNTER = 3, WARLOCK = 9;
constexpr std::uint32_t HUMAN = 1, ORC = 2, UNDEAD = 5;

auto const noTrainer = [](std::uint32_t) { return false; };
auto const anyRoute = [](std::uint32_t) { return Route::Yes; };

BotFacts Bot(std::uint32_t cls, std::uint32_t race, std::uint32_t level, std::vector<std::uint32_t> rewarded = {})
{
    BotFacts bot;
    bot.cls = cls;
    bot.race = race;
    bot.level = level;
    bot.governed = true;
    bot.levelXp = 8000;
    std::sort(rewarded.begin(), rewarded.end());
    bot.rewarded = std::move(rewarded);
    return bot;
}

// WU38: the table's old columns (tests/class-quest-old-table.tsv, dumped from the parent commit) and the quest index's
// own for the same quests (tests/class-quest-index-dump.tsv, written by `playerbots planner classquests` on lab7)
struct OldRow
{
    std::uint32_t quest{0}, classMask{0}, raceMask{0}, minLevel{0}, prev0{0}, prev1{0}, rewardSpell{0}, learns{0};
    std::string cause;
};

std::vector<OldRow> LoadOld(std::string const& path)
{
    std::vector<OldRow> out;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        OldRow row;
        in >> row.quest >> row.classMask >> row.raceMask >> row.minLevel >> row.prev0 >> row.prev1 >> row.cause >>
            row.rewardSpell >> row.learns;
        out.push_back(row);
    }
    return out;
}

// A fixture index with a record per old row (its limits, and its predecessors as prevAny, as the core's would be)
Planner::QuestIndex IndexOf(std::vector<OldRow> const& old)
{
    Planner::QuestGather gather;
    for (OldRow const& row : old)
    {
        Planner::QuestRecord record;
        record.id = row.quest;
        record.classMask = row.classMask;
        record.raceMask = row.raceMask;
        record.minLevel = row.minLevel;
        for (std::uint32_t const prev : {row.prev0, row.prev1})
            if (prev)
                record.prevAny.push_back(static_cast<std::int32_t>(prev));
        gather.quests.push_back(std::move(record));
    }
    return Planner::ClassQuests::Joined(Planner::QuestIndex::Build(std::move(gather)));
}

// The joined rows the rules run on in the cases below
Rows const& R()
{
    static Planner::QuestIndex const index = IndexOf(LoadOld("tests/class-quest-old-table.tsv"));
    return index.classRows;
}

Judgement JudgeId(std::uint32_t quest, BotFacts const& bot)
{
    Row const* row = Find(R(), quest);
    return row ? Judge(R(), *row, bot, noTrainer, anyRoute) : Judgement{};
}

std::string Show(Judgement const& judgement)
{
    return std::string(VerdictWord(judgement.verdict)) +
           (judgement.step ? " step " + std::to_string(judgement.step->quest()) : std::string());
}

void Table()
{
    bool sorted = true;
    int classQuests = 0;
    for (std::size_t i = 0; i < std::size(TABLE); ++i)
    {
        if (i && TABLE[i - 1].quest >= TABLE[i].quest)
            sorted = false;
        classQuests += IsClassQuest(TABLE[i]) ? 1 : 0;
    }
    Check(sorted, "the table is sorted by quest id, each quest once");
    // config/wu-class-quests.json has 60 quests; the 10 trainer quests left the table, the 124 step rows too (WU39)
    Check(classQuests == 50 && std::size(TABLE) == 50,
          "50 class quests and no step row (60 of the census less the 10 trainer quests): " +
              std::to_string(classQuests) + " of " + std::to_string(std::size(TABLE)));
    bool doable = true;
    for (std::uint32_t quest : {1498u, 1665u, 1678u, 1683u, 1785u, 1788u, 9691u, 6081u, 9673u, 6086u, 6089u, 6103u,
                                9675u, 9555u, 6125u, 6130u, 1652u, 10350u})
        doable = doable && Find(R(), quest) && StateOf(*Find(R(), quest), noTrainer) == State::Doable;
    Check(doable, "the census's value class quests are doable (1652 and 10350 included)");
    std::string objective;
    bool objectiveDoable = true;
    for (Entry const& entry : TABLE)
        if (entry.cause == Cause::Objective)
        {
            objective += " " + std::to_string(entry.quest);
            objectiveDoable = objectiveDoable && StateOf(*Find(R(), entry.quest), noTrainer) == State::Doable;
        }
    std::printf("info: objective class quests, doable from WU39 (automatic before):%s\n", objective.c_str());
    Check(objectiveDoable, "WU39: an objective class quest is doable by its own columns (the executor does them)");
    bool automatic = true;
    for (std::uint32_t quest : {1719u, 12687u, 12801u, 7583u, 96u, 1661u, 9712u, 12228u, 9364u})
        automatic = automatic && Find(R(), quest) && StateOf(*Find(R(), quest), noTrainer) == State::Automatic;
    Check(automatic, "the automatic ones (scripted, instance, placeholder; 9364 too) are not doable");
    bool trainer = true;
    for (std::uint32_t quest : {31u, 5061u, 4490u, 7463u, 7631u, 7647u, 9737u, 11001u, 12172u, 12173u})
        trainer = trainer && !Find(R(), quest);
    Check(trainer, "the trainer quests are not in the table");
    auto const teaches5502 = [](std::uint32_t spell) { return spell == 5502; };
    auto const teaches13819 = [](std::uint32_t spell) { return spell == 13819; };
    Check(StateOf(*Find(R(), 1652), teaches5502) == State::Ordinary &&
              StateOf(*Find(R(), 9691), teaches5502) == State::Ordinary,
          "1652 and 9691 leave the table when the trainer teaches 5502");
    Check(StateOf(*Find(R(), 1661), teaches13819) == State::Ordinary &&
              StateOf(*Find(R(), 1661), noTrainer) == State::Automatic,
          "1661 leaves the table when the trainer teaches Summon Warhorse 13819, else stays automatic");
    Row const* binding = Find(R(), 1795);
    Check(binding && binding->prev == std::vector<std::uint32_t>{1804, 1805},
          "1795 needs 1804 or 1805 (their NextQuestID), not only its own PrevQuestID 0");
    Row const* step = Find(R(), 1505);
    Check(step && !step->own && step->inIndex && step->minLevel == 10,
          "the step 1505 is a row from the index (no table entry), its limits the record's");
}

// WU38: the old table's columns and the quest index's own, one line per table row (lab7's index, as dumped by
// `playerbots planner classquests`): the join gives the old limits, and every old predecessor is among the index's
struct DumpRow
{
    std::uint32_t quest{0}, inIndex{0}, classMask{0}, raceMask{0}, minLevel{0};
    std::int32_t prev{0};
    std::vector<std::int32_t> prevAny;
    std::uint32_t refused[2]{0, 0}, giver{0};
};

std::vector<DumpRow> LoadDump(std::string const& path)
{
    std::vector<DumpRow> out;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        DumpRow row;
        std::string any;
        in >> row.quest >> row.inIndex >> row.classMask >> row.raceMask >> row.minLevel >> row.prev >> any >>
            row.refused[0] >> row.refused[1] >> row.giver;
        for (std::size_t at = 0; any != "-" && at < any.size();)
        {
            std::size_t const comma = any.find(',', at);
            row.prevAny.push_back(std::atoi(any.substr(at, comma - at).c_str()));
            at = comma == std::string::npos ? any.size() : comma + 1;
        }
        out.push_back(row);
    }
    return out;
}

// A QuestIndex from the dump (its limits, prev lists and refusals), joined: lab7's rows
Planner::QuestIndex DumpIndex(std::vector<DumpRow> const& dump)
{
    Planner::QuestGather gather;
    for (DumpRow const& row : dump)
        if (row.inIndex)
        {
            Planner::QuestRecord record;
            record.id = row.quest;
            record.classMask = row.classMask;
            record.raceMask = row.raceMask;
            record.minLevel = row.minLevel;
            record.prev = row.prev;
            record.prevAny = row.prevAny;
            gather.quests.push_back(std::move(record));
        }
    Planner::QuestIndex index = Planner::QuestIndex::Build(std::move(gather));
    for (Planner::QuestRecord& record : index.quests)
        for (DumpRow const& row : dump)
            if (row.quest == record.id)
                record.refused = {static_cast<std::uint8_t>(row.refused[0]), static_cast<std::uint8_t>(row.refused[1])};
    Planner::ClassQuests::JoinInto(index);
    return index;
}

Rows const& D()
{
    static Planner::QuestIndex const index = DumpIndex(LoadDump("tests/class-quest-index-dump.tsv"));
    return index.classRows;
}

void JoinEquality()
{
    std::vector<OldRow> const old = LoadOld("tests/class-quest-old-table.tsv");
    std::vector<DumpRow> const dump = LoadDump("tests/class-quest-index-dump.tsv");
    Check(old.size() == 174 && dump.size() == old.size(),
          "the old columns and the index dump have one line per old table row: " + std::to_string(old.size()) + ", " +
              std::to_string(dump.size()));
    bool sameOwn = true;
    int oldClass = 0;
    for (OldRow const& o : old)
    {
        if (!o.rewardSpell)
            continue;
        ++oldClass;
        Entry const* entry = FindEntry(o.quest);
        std::string lower = o.cause;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        sameOwn = sameOwn && entry && lower == CauseWord(entry->cause) && o.rewardSpell == entry->rewardSpell &&
                  o.learns == entry->learns;
    }
    Check(sameOwn && oldClass == 50, "every class quest keeps its cause and spells in the table");

    Rows const& joined = D();
    int missing = 0, limitsDiffer = 0, lost = 0, extra = 0, negative = 0, refusedRows = 0, unreached = 0;
    std::string firstDiff;
    for (OldRow const& o : old)
    {
        Row const* row = Find(joined, o.quest);
        if (!row)
        {
            ++unreached;
            std::printf("info: finding: old step %u is on no class quest's chain in the index\n", o.quest);
            continue;
        }
        if (!row->inIndex)
        {
            ++missing;
            std::printf("info: finding: quest %u is not in the index\n", o.quest);
            continue;
        }
        if (row->classMask != o.classMask || row->raceMask != o.raceMask || row->minLevel != o.minLevel)
        {
            ++limitsDiffer;
            if (firstDiff.empty())
                firstDiff = "quest " + std::to_string(o.quest) + " limits " + std::to_string(row->classMask) + "/" +
                            std::to_string(row->raceMask) + "/" + std::to_string(row->minLevel) + " old " +
                            std::to_string(o.classMask) + "/" + std::to_string(o.raceMask) + "/" +
                            std::to_string(o.minLevel);
        }
        for (std::uint32_t const prev : {o.prev0, o.prev1})
            if (prev && !Has(row->prev, prev))
            {
                ++lost;
                std::printf("info: finding: old predecessor %u of %u is not among the index's\n", prev, o.quest);
            }
        for (std::uint32_t const prev : row->prev)
            if (prev != o.prev0 && prev != o.prev1)
            {
                ++extra;
                Row const* walked = Find(joined, prev);
                std::printf("info: the index names predecessor %u of %u, the old table lacked it: %s\n", prev, o.quest,
                            walked && walked->inIndex ? "a step of the join"
                                                      : "a row without a record in the dump (the live index has it)");
            }
        negative += row->active.empty() ? 0 : 1;
        refusedRows += (row->refused[0] || row->refused[1]) ? 1 : 0;
    }
    JoinCounts const counts = CountsOf(joined);
    std::printf(
        "info: join of the index dump: %u class quests, %u steps (%u without a record), %d old rows unreached, %d not "
        "in the index, %d with a predecessor to be active, %d refused on a side, %d extra predecessors\n",
        counts.classQuests, counts.steps, counts.missing, unreached, missing, negative, refusedRows, extra);
    Check(missing == 0 && limitsDiffer == 0,
          "the joined limits (class, race, level) equal the old table's for every row" +
              (firstDiff.empty() ? std::string() : ": " + firstDiff));
    Check(lost == 0, "every old predecessor is among the index's: " + std::to_string(lost) + " lost");
    Check(unreached == 0 && counts.classQuests == 50 && refusedRows == 9,
          "the chains walked from the index reach every old step row; 50 class quests, 9 refused on a side");
}

std::string DueList(std::vector<Due> const& due)
{
    std::string out;
    for (Due const& d : due)
        out += " " + std::to_string(d.spell) + "<-" + std::to_string(d.quest) + (d.failed ? "(failed)" : "");
    return out.empty() ? " none" : out;
}

std::string BonusList(std::vector<std::pair<std::uint32_t, float>> const& bonus)
{
    std::string out;
    for (auto const& [quest, xp] : bonus)
        out += " " + std::to_string(quest);
    return out.empty() ? " none" : out;
}

bool DueHas(std::vector<Due> const& due, std::uint32_t spell, std::uint32_t quest = 0, bool failed = false)
{
    return std::any_of(due.begin(), due.end(), [&](Due const& d)
                       { return d.spell == spell && (!quest || d.quest == quest) && (!quest || d.failed == failed); });
}

bool BonusHas(std::vector<std::pair<std::uint32_t, float>> const& bonus, std::uint32_t quest)
{
    return std::any_of(bonus.begin(), bonus.end(), [quest](auto const& b) { return b.first == quest; });
}

// A small index from records (quest, class, race, level, predecessors, refused on both sides)
struct Rec
{
    std::uint32_t quest, classMask, raceMask, minLevel;
    std::vector<std::int32_t> prev;
    bool refused;
};

Planner::QuestIndex Mini(std::vector<Rec> const& recs)
{
    Planner::QuestGather gather;
    for (Rec const& rec : recs)
    {
        Planner::QuestRecord record;
        record.id = rec.quest;
        record.classMask = rec.classMask;
        record.raceMask = rec.raceMask;
        record.minLevel = rec.minLevel;
        record.prevAny = rec.prev;
        gather.quests.push_back(std::move(record));
    }
    Planner::QuestIndex index = Planner::QuestIndex::Build(std::move(gather));
    for (Planner::QuestRecord& record : index.quests)
        for (Rec const& rec : recs)
            if (rec.quest == record.id && rec.refused)
                record.refused = {1, 1};
    Planner::ClassQuests::JoinInto(index);
    return index;
}

// Prints a named case: each class quest's judgement, the level-up due list and the bonus quests
template <typename RouteOf>
std::vector<Due> Case(std::string const& name, Rows const& rows, BotFacts const& bot,
                      std::vector<std::uint32_t> const& quests, RouteOf const& route,
                      std::vector<std::pair<std::uint32_t, float>>* bonusOut = nullptr)
{
    std::string judged;
    for (std::uint32_t const quest : quests)
    {
        Row const* row = Find(rows, quest);
        Judgement const judgement = row ? Judge(rows, *row, bot, noTrainer, route) : Judgement{};
        judged += " " + std::to_string(quest) + ":" + VerdictWord(judgement.verdict) +
                  (judgement.step ? "@" + std::to_string(judgement.step->quest()) : std::string()) +
                  (judgement.waitsOn ? " waits on " + std::to_string(judgement.waitsOn) : std::string());
    }
    std::vector<Due> const due = DueQuests(rows, bot, When::LevelUp, noTrainer, route);
    auto const bonus = Bonuses(rows, bot, noTrainer, route);
    std::printf("info: case %s:%s; due at level-up:%s; bonus:%s\n", name.c_str(), judged.c_str(), DueList(due).c_str(),
                BonusList(bonus).c_str());
    if (bonusOut)
        *bonusOut = bonus;
    return due;
}

constexpr std::uint32_t DRUID = 11, SHAMAN = 7;

Judgement JudgeD(std::uint32_t quest, BotFacts const& bot)
{
    Row const* row = Find(D(), quest);
    return row ? Judge(D(), *row, bot, noTrainer, anyRoute) : Judgement{};
}
constexpr std::uint32_t DWARF = 3, TAUREN = 6;

void NamedCases()
{
    std::vector<OldRow> const old = LoadOld("tests/class-quest-old-table.tsv");
    auto const recOf = [&old](std::uint32_t quest, bool refused, std::uint32_t raceMask = 0)
    {
        for (OldRow const& o : old)
            if (o.quest == quest)
            {
                Rec rec{quest, o.classMask, raceMask ? raceMask : o.raceMask, o.minLevel, {}, refused};
                for (std::uint32_t const prev : {o.prev0, o.prev1})
                    if (prev)
                        rec.prev.push_back(static_cast<std::int32_t>(prev));
                return rec;
            }
        return Rec{quest, 0, 0, 0, {}, refused};
    };
    std::vector<std::pair<std::uint32_t, float>> bonus;

    // 1: the bear chain's step 5930 is refused for a tauren druid: Bear Form's quest is automatic, due at level-up
    std::vector<Due> due = Case("1 tauren druid 10, step 5930 refused", D(), Bot(DRUID, TAUREN, 10), {6002}, anyRoute);
    Check(DueHas(due, 5487, 6002) && JudgeD(6002, Bot(DRUID, TAUREN, 10)).verdict == Verdict::NotDoable,
          "case 1: a refused step makes the class quest automatic: due at level-up");

    // 2 and 3: an Alliance warrior whose only Defensive Stance quest is 1665 (its step 1640 refused), then with 1678
    Planner::QuestIndex const only1665 =
        Mini({recOf(1638, false), recOf(1639, false), recOf(1640, true), recOf(1665, false)});
    due = Case("2 human warrior 10, 1665 only", only1665.classRows, Bot(WARRIOR, HUMAN, 10), {1665}, anyRoute, &bonus);
    Check(DueHas(due, 71, 1665) && bonus.empty(), "case 2: the only quest of the spell is automatic: due, no bonus");
    Planner::QuestIndex const with1678 = Mini({recOf(1638, false), recOf(1639, false), recOf(1640, true),
                                               recOf(1665, false), recOf(1678, false, 1101), recOf(1679, false, 1101)});
    due = Case("3 human warrior 10, 1665 and 1678", with1678.classRows, Bot(WARRIOR, HUMAN, 10), {1665, 1678}, anyRoute,
               &bonus);
    Check(due.empty() && BonusHas(bonus, 1679),
          "case 3: one doable quest of the spell: the bonus through 1678's chain (1679), nothing due");

    // 4: Horde warrior, 1505 -> 1498
    due = Case("4a orc warrior 10", D(), Bot(WARRIOR, ORC, 10), {1498, 1819}, anyRoute, &bonus);
    bool const firstStep = due.empty() && BonusHas(bonus, 1505) && !BonusHas(bonus, 1498);
    due = Case("4b orc warrior 10, 1505 rewarded", D(), Bot(WARRIOR, ORC, 10, {1505}), {1498, 1819}, anyRoute, &bonus);
    Check(firstStep && due.empty() && BonusHas(bonus, 1498) && !BonusHas(bonus, 1505),
          "case 4: the bonus on 1505, then on 1498");

    // 5: a hunter's chain from its first step
    due = Case("5 orc hunter 10", D(), Bot(HUNTER, ORC, 10), {6081, 6082}, anyRoute, &bonus);
    Check(BonusHas(bonus, 6062) && !DueHas(due, 6991) && !DueHas(due, 1515),
          "case 5: the hunter's chains get the bonus on their first step 6062 (objective steps are doable)");

    // 6: given up once (the record's feed below the limit): still offered; twice: failed, due, not offered
    QuestFail::Book record;
    record.Note(21, 1498, "timeout", 10, 1);
    BotFacts once = Bot(WARRIOR, ORC, 10, {1505});
    once.givenUp = QuestFail::GivenUp(record.RowsOf(21), GIVE_UP_LIMIT);
    due = Case("6a orc warrior 10, 1498 given up once", D(), once, {1498}, anyRoute, &bonus);
    bool const offered = due.empty() && BonusHas(bonus, 1498);
    record.Note(21, 1498, "no_target", 3, 2);
    BotFacts twice = once;
    twice.givenUp = QuestFail::GivenUp(record.RowsOf(21), GIVE_UP_LIMIT);
    due = Case("6b orc warrior 10, 1498 given up twice", D(), twice, {1498, 1819}, anyRoute, &bonus);
    Check(offered && DueHas(due, 71, 1498, true) && bonus.empty() &&
              DueHas(DueQuests(D(), twice, When::Check, noTrainer, anyRoute), 71, 1498, true),
          "case 6: given up once it is offered and not due; twice it is failed: due (also at a check), not offered");

    // 7: a step given up twice fails its class quest
    BotFacts step = Bot(WARRIOR, ORC, 10);
    step.givenUp = {1505};
    due = Case("7 orc warrior 10, step 1505 given up twice", D(), step, {1498}, anyRoute, &bonus);
    Check(DueHas(due, 71, 1498, true) && !BonusHas(bonus, 1505),
          "case 7: a step given up twice: its class quest is due");

    // 8: a definite no route fails it; a search cut short (or no graph) is unknown: nothing due
    auto const noRoute1505 = [](std::uint32_t quest) { return quest == 1505 ? Route::No : Route::Yes; };
    due = Case("8a orc warrior 10, no route to 1505", D(), Bot(WARRIOR, ORC, 10), {1498}, noRoute1505);
    bool const definite = DueHas(due, 71, 1498, true);
    auto const capped = [](std::uint32_t) { return Route::Unknown; };
    due = Case("8b orc warrior 10, every search cut short", D(), Bot(WARRIOR, ORC, 10), {1498, 1819}, capped, &bonus);
    Check(definite && due.empty() && bonus.empty() && JudgeId(1498, Bot(WARRIOR, ORC, 10)).verdict == Verdict::Bonus &&
              Judge(D(), *Find(D(), 1498), Bot(WARRIOR, ORC, 10), noTrainer, capped).verdict == Verdict::Unknown,
          "case 8: no route (-1) is failed and due; a cut-short search is unknown and nothing is due");

    // 9: the spell known: nothing
    BotFacts known = Bot(WARRIOR, ORC, 10);
    known.knownSpells = {71};
    due = Case("9 orc warrior 10 knowing 71", D(), known, {1498, 1819}, anyRoute, &bonus);
    Check(!DueHas(due, 71) && !BonusHas(bonus, 1505) && !BonusHas(bonus, 1818), "case 9: a known spell: nothing");

    // 10: a weighted-group bot (not governed): everything at level-up, no bonus
    BotFacts loose = Bot(WARRIOR, ORC, 10);
    loose.governed = false;
    due = Case("10 ungoverned orc warrior 10", D(), loose, {1498}, anyRoute, &bonus);
    Check(DueHas(due, 71) && bonus.empty() && DueQuests(D(), loose, When::Check, noTrainer, anyRoute).empty(),
          "case 10: an ungoverned bot gets everything at level-up, no bonus, nothing at a check");

    // 11: an objective chain the executor does (shaman 1524-1527): doable now, its first step gets the bonus
    due = Case("11 orc shaman 10", D(), Bot(SHAMAN, ORC, 10), {1527}, anyRoute, &bonus);
    Check(!DueHas(due, 3599) && BonusHas(bonus, 1524),
          "case 11: the objective chain 1524-1527 is doable: bonus on 1524");

    // 12: shared spells by race; a predecessor with two alternatives (1795: 1804 Alliance, 1805 Horde)
    std::vector<std::pair<std::uint32_t, float>> dwarfBonus;
    std::vector<Due> const dwarf = Case("12a dwarf warrior 10", D(), Bot(WARRIOR, DWARF, 10),
                                        {1665, 1678, 1683, 9582, 10350}, anyRoute, &dwarfBonus);
    std::vector<Due> const human =
        Case("12b human warrior 10", D(), Bot(WARRIOR, HUMAN, 10), {1665, 1678, 1683, 9582, 10350}, anyRoute, &bonus);
    BotFacts const humanLock = Bot(WARLOCK, HUMAN, 30);
    BotFacts const undeadLock = Bot(WARLOCK, UNDEAD, 30);
    Case("12c human warlock 30", D(), humanLock, {1795}, anyRoute);
    Case("12d undead warlock 30", D(), undeadLock, {1795}, anyRoute);
    Walk const alliance = WalkChain(D(), *Find(D(), 1795), humanLock);
    Walk const horde = WalkChain(D(), *Find(D(), 1795), undeadLock);
    bool const sides = alliance.remaining.size() > 1 && alliance.remaining[1]->quest() == 1804 &&
                       horde.remaining.size() > 1 && horde.remaining[1]->quest() == 1805;
    Check(!DueHas(dwarf, 71) && BonusHas(dwarfBonus, 1679) && !DueHas(human, 71) && !BonusHas(bonus, 1679) &&
              JudgeD(1665, Bot(WARRIOR, DWARF, 10)).verdict == Verdict::NotDoable && sides,
          "case 12: a dwarf's and a human's Defensive Stance sets differ (1678 dwarf only), 1665 automatic for both; "
          "1795's walk takes 1804 for Alliance, 1805 for Horde");
}

void Scenarios()
{
    BotFacts const warrior = Bot(WARRIOR, ORC, 10, {1505});
    Judgement const path = JudgeId(1498, warrior);
    std::printf("info: warrior 10 (orc, 1505 rewarded) 1498: %s, bonus %.0f XP\n", Show(path).c_str(),
                BonusXp(warrior));
    Check(path.verdict == Verdict::Bonus && path.step->quest() == 1498, "a warrior at 10 with 1498 takeable: bonus");

    BotFacts const fresh = Bot(WARRIOR, ORC, 10);
    Check(JudgeId(1498, fresh).verdict == Verdict::Bonus && JudgeId(1498, fresh).step->quest() == 1505,
          "without 1505 the chain's next step 1505 gets the bonus: " + Show(JudgeId(1498, fresh)));

    BotFacts const hunter = Bot(HUNTER, ORC, 10);
    Walk const walk = WalkChain(R(), *Find(R(), 6081), hunter);
    Check(walk.next && walk.next->quest() == 6062,
          "a hunter's chain starts at its first step 6062: " + std::to_string(walk.next ? walk.next->quest() : 0));
    Check(JudgeId(6081, hunter).verdict == Verdict::Bonus && JudgeId(6081, hunter).step->quest() == 6062,
          "WU39: the taming steps (objective) are doable: the bonus on 6062, " + Show(JudgeId(6081, hunter)));
    BotFacts const tamed = Bot(HUNTER, ORC, 10, {6062, 6083, 6082});
    Check(JudgeId(6081, tamed).verdict == Verdict::Bonus, "after the taming steps 6081 gets the bonus");

    Check(JudgeId(1719, Bot(WARRIOR, ORC, 30, {1718})).verdict == Verdict::NotDoable, "automatic 1719: no bonus");
    Row const* path1498 = Find(R(), 1498);
    Check(Judge(R(), *path1498, warrior, noTrainer, [](std::uint32_t) { return Route::No; }).verdict == Verdict::Failed,
          "a quest with definitely no route: failed, no bonus");
    BotFacts known = warrior;
    known.knownSpells = {71};
    Check(JudgeId(1498, known).verdict == Verdict::SpellKnown, "a bot that knows Defensive Stance 71: no bonus");
    BotFacts gaveUp = warrior;
    gaveUp.givenUp = {1498};
    Check(JudgeId(1498, gaveUp).verdict == Verdict::Failed, "a quest given up twice: failed, no bonus");
    BotFacts low = Bot(WARRIOR, ORC, 9, {1505});
    Check(JudgeId(1498, low).verdict == Verdict::LowLevel, "below the quest's level: no bonus");
    Check(JudgeId(1498, Bot(WARRIOR, HUMAN, 10, {1505})).verdict == Verdict::NotForBot, "another race: no bonus");
    Check(JudgeId(1498, Bot(PALADIN, ORC, 10, {1505})).verdict == Verdict::NotForBot, "another class: no bonus");

    using Planner::GoalApply::Governed;
    Check(Governed(true, 21), "a persistent planner bot (guid 21) is governed");
    Check(!Governed(true, 20), "a weighted-group bot (guid 20) is not governed");
    Check(!Governed(false, 21), "a bot that keeps no possessions (or has a master at a client) is not governed");
    BotFacts ungoverned = warrior;
    ungoverned.governed = false;
    Check(JudgeId(1498, ungoverned).verdict == Verdict::NotGoverned &&
              Bonuses(R(), ungoverned, noTrainer, anyRoute).empty(),
          "an ungoverned bot gets no bonus");
    BotFacts topLevel = warrior;
    topLevel.levelXp = 0;
    Check(Bonuses(R(), topLevel, noTrainer, anyRoute).empty(),
          "at the top level (no XP to the next level) no bonus entry");

    auto const bonuses = Bonuses(R(), warrior, noTrainer, anyRoute);
    std::printf("info: the orc warrior's bonuses:%s\n", BonusList(bonuses).c_str());
    Check(BonusHas(bonuses, 1498) && BonusHas(bonuses, 1818) && bonuses.size() == 2,
          "the orc warrior's bonuses: 1498 and 1819's step 1818 (objective, doable from WU39)");
    BotFacts const warlock = Bot(WARLOCK, UNDEAD, 30);
    Walk const felhunter = WalkChain(R(), *Find(R(), 1795), warlock);
    Check(felhunter.next && felhunter.next->quest() == 1801,
          "an undead warlock's chain to 1795 starts at the Horde 1801: " +
              std::to_string(felhunter.next ? felhunter.next->quest() : 0));
    Check(Protected(R(), {1498, 1505, 6082, 42}, fresh, noTrainer) == std::vector<std::uint32_t>{1498, 1505},
          "protected: the doable class quest and its step in the log, not another class's quest or another quest");
}

// ---- WU39's invariant: no bot stalls on a class-quest spell ----
using Teaches = std::function<bool(std::uint32_t)>;
using RouteFn = std::function<Route(std::uint32_t)>;
using DueFn = std::vector<Due> (*)(Rows const&, BotFacts const&, When, Teaches const&, RouteFn const&);

std::vector<Due> RulesDue(Rows const& rows, BotFacts const& bot, When when, Teaches const& teaches,
                          RouteFn const& route)
{
    return DueQuests(rows, bot, when, teaches, route);
}

// The parent's rules had no due function: the invariant must find their stalls
std::vector<Due> NoDue(Rows const&, BotFacts const&, When, Teaches const&, RouteFn const&) { return {}; }

// The spells the invariant covers: the bot lacks it, some class quest teaching it fits the bot and is at or below its
// level (the highest step level on its chain), the trainer does not teach it, and no such quest is rewarded
std::set<std::uint32_t> Covered(Rows const& rows, BotFacts const& bot, Teaches const& teaches)
{
    std::set<std::uint32_t> settled, open;
    for (Row const& row : rows)
    {
        if (!IsClassQuest(row) || !row.own->learns || !Fits(row, bot) || teaches(row.own->learns))
            continue;
        if (KnowsSpell(row, bot) || Has(bot.rewarded, row.quest()))
            settled.insert(row.own->learns);
        else if (WalkChain(rows, row, bot).level <= bot.level)
            open.insert(row.own->learns);
    }
    for (std::uint32_t const spell : settled)
        open.erase(spell);
    return open;
}

void Add(std::vector<std::uint32_t>& sorted, std::uint32_t id)
{
    auto const it = std::lower_bound(sorted.begin(), sorted.end(), id);
    if (it == sorted.end() || *it != id)
        sorted.insert(it, id);
}

struct Tally
{
    int bots{0}, spells{0}, due{0}, bonus{0}, unknown{0}, waits{0}, top{0}, moves{0}, maxMoves{0}, walked{0};
    std::string first, firstWaits, firstTop;
};

// One bot's state: exactly one of due, bonus, or (neither) unknown or waiting on a predecessor ordinary questing does
std::string CheckState(Rows const& rows, BotFacts const& bot, Teaches const& teaches, RouteFn const& route, DueFn dueFn,
                       Tally& tally, bool count)
{
    std::vector<Due> const levelUp = dueFn(rows, bot, When::LevelUp, teaches, route);
    std::vector<Due> const check = dueFn(rows, bot, When::Check, teaches, route);
    std::vector<Due> const creation = dueFn(rows, bot, When::Creation, teaches, route);
    auto const bonus = Bonuses(rows, bot, teaches, route);
    std::size_t const side = SideOf(bot.race);
    std::set<std::uint32_t> const covered = Covered(rows, bot, teaches);
    for (std::uint32_t const spell : covered)
    {
        bool const due = DueHas(levelUp, spell);
        bool offered = false, unknown = false, waits = false, top = false, twice = false;
        for (Row const& row : rows)
        {
            if (!IsClassQuest(row) || row.own->learns != spell || !Fits(row, bot))
                continue;
            Judgement const judgement = Judge(rows, row, bot, teaches, route);
            unknown = unknown || judgement.verdict == Verdict::Unknown;
            waits = waits || judgement.verdict == Verdict::StepWaits;
            if (judgement.verdict != Verdict::Bonus)
                continue;
            top = top || BonusXp(bot) <= 0.0f;
            bool const listed = BonusHas(bonus, judgement.step->quest());
            offered = offered || listed;
            bool shared = false;  // the step is another spell's chain step too (the hunter's 6062)
            for (Row const& other : rows)
                if (IsClassQuest(other) && other.own->learns != spell && Fits(other, bot))
                {
                    Walk const walk = WalkChain(rows, other, bot);
                    shared = shared || (walk.next == judgement.step);
                }
            twice = twice || (due && listed && !shared);
        }
        if (count)
        {
            ++tally.spells;
            tally.due += due ? 1 : 0;
            tally.bonus += !due && offered ? 1 : 0;
            tally.unknown += !due && !offered && unknown ? 1 : 0;
            tally.waits += !due && !offered && !unknown && waits ? 1 : 0;
            tally.top += !due && !offered && !unknown && !waits && top ? 1 : 0;
        }
        if (twice)
            return "spell " + std::to_string(spell) + " is due and gets the bonus";
        if (!due && !offered && !unknown && !waits && !top)
            return "spell " + std::to_string(spell) + " stalls: not due, no bonus, no unknown way, no wait";
        if (!due && !offered && !unknown && waits && tally.firstWaits.empty())
            tally.firstWaits = "spell " + std::to_string(spell) + " (class " + std::to_string(bot.cls) + ", race " +
                               std::to_string(bot.race) + ", level " + std::to_string(bot.level) + ")";
        if (!due && !offered && !unknown && !waits && top && tally.firstTop.empty())
            tally.firstTop = "spell " + std::to_string(spell) + " (class " + std::to_string(bot.cls) + ")";
    }
    // The feed can offer every bonus step: fits, not refused for the side, unlocked, at the bot's level, a route
    for (auto const& [quest, xp] : bonus)
    {
        Row const* row = Find(rows, quest);
        if (!bot.governed || !row || !Fits(*row, bot) || RefusedFor(*row, side) || !Unlocked(*row, bot) ||
            bot.level < row->minLevel || route(quest) != Route::Yes || Has(bot.rewarded, quest) ||
            Has(bot.givenUp, quest) || xp <= 0.0f)
            return "the bonus on " + std::to_string(quest) + " is one the feed can not offer";
    }
    // Side rules: a governed bot is given only automatic or failed quests, at a check only failed ones; an ungoverned
    // bot nothing at a check and no bonus; nothing for a known spell or a rewarded quest; creation covers level-up
    for (Due const& d : levelUp)
    {
        Row const* row = Find(rows, d.quest);
        Verdict const verdict = row ? Judge(rows, *row, bot, teaches, route).verdict : Verdict::NotForBot;
        if (bot.governed && verdict != Verdict::NotDoable && verdict != Verdict::Failed)
            return "a governed bot is given " + std::to_string(d.quest) + ", which is " + VerdictWord(verdict);
        if (!row || !covered.count(d.spell) || !DueHas(creation, d.spell))
            return "quest " + std::to_string(d.quest) + " is due outside the covered spells or not at creation";
    }
    for (Due const& d : check)
        if (!bot.governed || !d.failed)
            return "quest " + std::to_string(d.quest) + " is due at a check without being failed";
    if (!bot.governed && !bonus.empty())
        return "an ungoverned bot gets a bonus";
    return {};
}

// Grants what is due: the quest rewarded, its spells known
void Grant(Rows const& rows, BotFacts& bot, std::vector<Due> const& due)
{
    for (Due const& d : due)
    {
        Add(bot.rewarded, d.quest);
        Row const* row = Find(rows, d.quest);
        Add(bot.knownSpells, row->own->learns);
        Add(bot.knownSpells, row->own->rewardSpell);
    }
}

// The bot at level 1 with nothing done: its chains whole
BotFacts Fresh(BotFacts const& bot)
{
    BotFacts fresh;
    fresh.cls = bot.cls;
    fresh.race = bot.race;
    return fresh;
}

std::uint32_t LevelXp(std::uint32_t level) { return level < 80 ? 400 + 100 * level : 0; }

// Follows the rule's own moves until every covered spell is known at the top level: do the offered step (or give it up;
// twice is failed), resolve an unknown way, let ordinary questing do a waited-on predecessor, level up (the due
// function at each level-up, the check's after each move). A stall or a run past the bound is a counterexample
std::string Moves(Rows const& rows, BotFacts bot, Teaches const& teaches, std::map<std::uint32_t, Route>& routes,
                  std::mt19937& rng, DueFn dueFn, Tally& tally)
{
    RouteFn const route = [&routes](std::uint32_t quest) { return routes.count(quest) ? routes[quest] : Route::Yes; };
    std::map<std::uint32_t, int> giveUps;
    int bound = static_cast<int>(80 - bot.level) + 2;
    for (Row const& row : rows)
        if (IsClassQuest(row) && Fits(row, bot))
            bound += 3 * static_cast<int>(WalkChain(rows, row, Fresh(bot)).remaining.size()) + 2;
    Grant(rows, bot, dueFn(rows, bot, When::LevelUp, teaches, route));
    int moves = 0;
    for (; moves <= bound; ++moves)
    {
        Grant(rows, bot, dueFn(rows, bot, When::Check, teaches, route));
        std::set<std::uint32_t> const covered = Covered(rows, bot, teaches);
        if (covered.empty())
        {
            if (bot.level >= 80)
                break;
            ++bot.level;
            bot.levelXp = LevelXp(bot.level);
            Grant(rows, bot, dueFn(rows, bot, When::LevelUp, teaches, route));
            continue;
        }
        std::uint32_t const spell = *covered.begin();
        Judgement act;
        for (Row const& row : rows)
            if (IsClassQuest(row) && row.own->learns == spell && Fits(row, bot))
            {
                Judgement const judgement = Judge(rows, row, bot, teaches, route);
                if (judgement.verdict == Verdict::Bonus && BonusXp(bot) > 0.0f)
                    act = judgement;
                else if ((judgement.verdict == Verdict::Unknown || judgement.verdict == Verdict::StepWaits) &&
                         act.verdict != Verdict::Bonus)
                    act = judgement;
            }
        if (act.verdict == Verdict::Bonus)
        {
            std::uint32_t const quest = act.step->quest();
            if (rng() % 4 == 0)
            {
                if (++giveUps[quest] >= static_cast<int>(GIVE_UP_LIMIT))
                    Add(bot.givenUp, quest);
            }
            else
            {
                Add(bot.rewarded, quest);
                if (IsClassQuest(*act.step))
                    Grant(rows, bot, {{act.step->own->learns, quest, false}});
            }
        }
        else if (act.verdict == Verdict::Unknown)
            routes[act.step->quest()] = rng() % 4 ? Route::Yes : Route::No;
        else if (act.verdict == Verdict::StepWaits)
            Add(bot.rewarded, act.waitsOn);
        else if (DueHas(dueFn(rows, bot, When::LevelUp, teaches, route), spell) && bot.level < 80)
        {
            ++bot.level;  // an automatic quest whose chain came within the bot's level: the next level-up grants it
            bot.levelXp = LevelXp(bot.level);
            Grant(rows, bot, dueFn(rows, bot, When::LevelUp, teaches, route));
        }
        else if (bot.level >= 80)
            break;  // the top level: no bonus, no level-up (counted by CheckState)
        else
            return "stall on spell " + std::to_string(spell) + " at level " + std::to_string(bot.level) + " after " +
                   std::to_string(moves) + " moves";
    }
    tally.moves += moves;
    tally.maxMoves = std::max(tally.maxMoves, moves);
    ++tally.walked;
    if (moves > bound)
        return "not done after " + std::to_string(bound) + " moves";
    return {};
}

// (a) the hand-made fixture: the old table's chains with refused steps, a predecessor with two alternatives, a step
// above its class quest's level, a predecessor without a record, and a chain longer than the join's walk
Planner::QuestIndex Fixture()
{
    std::vector<OldRow> const old = LoadOld("tests/class-quest-old-table.tsv");
    std::vector<Rec> recs;
    for (OldRow const& o : old)
    {
        Rec rec{o.quest, o.classMask, o.raceMask, o.minLevel, {}, false};
        for (std::uint32_t const prev : {o.prev0, o.prev1})
            if (prev)
                rec.prev.push_back(static_cast<std::int32_t>(prev));
        rec.refused = o.quest == 1640 || o.quest == 5929 || o.quest == 9467 || o.quest == 6083;
        if (o.quest == 1649)
            rec.prev = {91001};  // a human paladin's Tome of Valor (1652) waits past the walk
        if (o.quest == 1498)
            rec.prev.push_back(90001);  // an ordinary orc quest opens it too
        if (o.quest == 1505)
            rec.minLevel = 12;  // above 1498's 10
        if (o.quest == 7601)
            rec.prev = {99999};  // no record: 7603's chain is broken
        if (o.quest == 9582)
            rec.prev = {91001};  // 20 steps before it: past the join's walk
        recs.push_back(rec);
    }
    recs.push_back({90001, 0, 690, 8, {}, false});
    for (std::uint32_t quest = 91001; quest <= 91020; ++quest)
        recs.push_back(
            {quest, 0, 1101, 1,
             quest < 91020 ? std::vector<std::int32_t>{static_cast<int>(quest + 1)} : std::vector<std::int32_t>{},
             false});
    return Mini(recs);
}

void Invariant(char const* name, Rows const& rows, std::uint32_t seed, DueFn dueFn, bool expectHolds)
{
    std::mt19937 rng(seed);
    std::vector<std::uint32_t> classQuests, steps, spells;
    for (Row const& row : rows)
    {
        (IsClassQuest(row) ? classQuests : steps).push_back(row.quest());
        if (IsClassQuest(row))
        {
            spells.push_back(row.own->learns);
            spells.push_back(row.own->rewardSpell);
        }
    }
    std::uint32_t const classes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
    std::uint32_t const races[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    int const BOTS = 3000;
    Tally tally;
    for (int n = 0; n < BOTS && tally.first.empty(); ++n)
    {
        BotFacts bot;
        bot.cls = classes[rng() % 10];
        bot.race = races[rng() % 10];
        bot.level = 1 + rng() % 80;
        bot.governed = rng() % 4 != 0;
        bot.levelXp = LevelXp(bot.level);
        // A random prefix of random chains (walked with nothing rewarded), random class quests, spells, give-ups
        for (Row const& row : rows)
            if (IsClassQuest(row) && Fits(row, bot) && rng() % 3 == 0)
            {
                Walk const walk = WalkChain(rows, row, Fresh(bot));
                std::size_t const done = rng() % (walk.remaining.size() + 1);
                for (std::size_t i = 0; i < done; ++i)
                    Add(bot.rewarded, walk.remaining[walk.remaining.size() - 1 - i]->quest());
            }
        for (std::uint32_t const quest : classQuests)
            if (rng() % 12 == 0)
                Add(bot.rewarded, quest);
        for (std::uint32_t const spell : spells)
            if (spell && rng() % 8 == 0)
                Add(bot.knownSpells, spell);
        std::map<std::uint32_t, int> giveUps;
        for (std::vector<std::uint32_t> const* list : {&classQuests, &steps})
            for (std::uint32_t const quest : *list)
                if (rng() % 15 == 0)
                    giveUps[quest] = 1 + static_cast<int>(rng() % 2);
        for (auto const& [quest, times] : giveUps)
            if (times >= static_cast<int>(GIVE_UP_LIMIT))
                Add(bot.givenUp, quest);
        std::set<std::uint32_t> taught;
        for (std::uint32_t const spell : spells)
            if (spell && rng() % 12 == 0)
                taught.insert(spell);
        std::map<std::uint32_t, Route> routes;
        for (std::vector<std::uint32_t> const* list : {&classQuests, &steps})
            for (std::uint32_t const quest : *list)
            {
                std::uint32_t const roll = rng() % 100;
                routes[quest] = roll < 75 ? Route::Yes : (roll < 83 ? Route::No : Route::Unknown);
            }
        Teaches const teaches = [&taught](std::uint32_t spell) { return taught.count(spell) > 0; };
        RouteFn const route = [&routes](std::uint32_t quest)
        { return routes.count(quest) ? routes[quest] : Route::Yes; };
        std::string const who = "bot " + std::to_string(n) + " (class " + std::to_string(bot.cls) + ", race " +
                                std::to_string(bot.race) + ", level " + std::to_string(bot.level) +
                                (bot.governed ? ", governed" : ", not governed") + ")";
        ++tally.bots;
        std::string why = CheckState(rows, bot, teaches, route, dueFn, tally, true);
        if (why.empty())
            why = Moves(rows, bot, teaches, routes, rng, dueFn, tally);
        if (!why.empty())
            tally.first = who + ": " + why;
    }
    std::printf(
        "info: invariant %s seed %u (%s): %d bots, %d covered spells: %d due, %d bonus, %d unknown, %d wait on a "
        "predecessor ordinary questing does%s, %d at the top level without a bonus%s; moves to done %d on %d "
        "bots, at most %d\n",
        name, seed, expectHolds ? "the rules" : "without a due function", tally.bots, tally.spells, tally.due,
        tally.bonus, tally.unknown, tally.waits,
        tally.firstWaits.empty() ? "" : (" (first " + tally.firstWaits + ")").c_str(), tally.top,
        tally.firstTop.empty() ? "" : (" (first " + tally.firstTop + ")").c_str(), tally.moves, tally.walked,
        tally.maxMoves);
    if (expectHolds)
        Check(tally.first.empty() && tally.bonus > 0 && tally.due > 0 && tally.unknown > 0 &&
                  (tally.waits > 0 || std::string(name) != "fixture"),
              std::string("WU39 invariant on ") + name + ": no stall, never due and bonus, bounded moves" +
                  (tally.first.empty() ? std::string() : ": first counterexample " + tally.first));
    else
        Check(!tally.first.empty(), std::string("WU39 invariant on ") + name +
                                        " finds the stall of rules without a due function: " + tally.first);
}

// ---- The planner wiring (WU20a-2): a synthetic hub planned end to end by Goals::PlanGoals ----
namespace G = Planner::Goals;
namespace GH = Planner::GoalHarness;

constexpr std::uint32_t HUB_GIVER = 93001, FAR_GIVER = 93002, CLASS_QUEST = 1498, CLASS_PREV = 1505;
constexpr std::uint32_t GATE = 93011, GATED = 93010, ORDINARY = 95001, ORDINARY_COUNT = 5;
constexpr float FAR_YARDS = 1600.0f;

// A hub with five kill quests near its giver; Path of Defense (1498) at the hub or 1600 yards away; GATED opened only
// by GATE naming it as its NextQuestId (the core's prevQuests, the 1795 gap)
Planner::QuestIndex World(bool classAtHub, Planner::Snapshot const& bot)
{
    Planner::QuestGather gather;
    Planner::Place const& at = bot.position;
    float const hx = at.x, hy = at.y + 5.0f;
    float const cx = classAtHub ? hx : hx + FAR_YARDS, cy = hy;
    gather.places.push_back({at.map, hx, hy, 0.0f, HUB_GIVER, false, bot.zone, bot.area, Planner::NO_CAMP});
    if (!classAtHub)
        gather.places.push_back({at.map, cx, cy, 0.0f, FAR_GIVER, false, bot.zone, bot.area, Planner::NO_CAMP});
    std::uint32_t target = 94001;
    auto const add = [&](std::uint32_t id, std::uint32_t xp, std::uint32_t giver, float x, float y)
    {
        Planner::QuestRecord quest;
        quest.id = id;
        quest.level = 10;
        quest.minLevel = 8;
        quest.xpBase = xp;
        quest.objectives.push_back({Planner::ObjectiveKind::Kill, target, 6u, 0, {}});
        for (float const offset : {-8.0f, 0.0f, 8.0f})
            gather.targets.push_back({target, false, at.map, x + offset, y + offset, 0.0f});
        gather.relations.push_back({id, giver, false, false});
        gather.relations.push_back({id, giver, false, true});
        gather.quests.push_back(std::move(quest));
        ++target;
    };
    for (std::uint32_t i = 0; i < ORDINARY_COUNT; ++i)
        add(ORDINARY + i, 450 + 50 * i, HUB_GIVER, hx + 60.0f + 25.0f * i, hy - 40.0f + 20.0f * i);
    add(CLASS_QUEST, 550, classAtHub ? HUB_GIVER : FAR_GIVER, cx + 70.0f, cy + 30.0f);
    gather.quests.back().minLevel = 10;
    gather.quests.back().classMask = 1;
    gather.quests.back().raceMask = 690;
    gather.quests.back().prevAny = {static_cast<std::int32_t>(CLASS_PREV)};
    add(GATE, 400, HUB_GIVER, hx - 60.0f, hy + 50.0f);
    add(GATED, 500, HUB_GIVER, hx - 70.0f, hy - 50.0f);
    gather.quests.back().prevAny = {static_cast<std::int32_t>(GATE)};
    return Planner::ClassQuests::Joined(Planner::QuestIndex::Build(std::move(gather)));
}

Planner::Snapshot Orc()
{
    Planner::Snapshot bot = GH::Bot(10, 5000);
    bot.race = 2;
    bot.cls = 1;
    bot.team = 1;
    bot.rewardedQuests = {CLASS_PREV};
    return bot;
}

G::Situation GovernedAt()
{
    G::Situation situation = GH::Base();
    situation.governed = true;
    situation.levelXp = 7600;
    return situation;
}

std::string Ranked(G::GoalPlan const& plan)
{
    std::string takes;
    for (Planner::CircuitTake const& take : plan.circuit.take)
        takes += " " + std::to_string(take.quest) + (take.classQuest ? "(class_quest)" : "");
    return "[" + GH::Summary(plan.ranked) + "] takes:" + (takes.empty() ? " none" : takes);
}

bool TakesClass(G::GoalPlan const& plan, std::uint32_t quest)
{
    return std::any_of(plan.circuit.take.begin(), plan.circuit.take.end(),
                       [quest](Planner::CircuitTake const& take) { return take.quest == quest && take.classQuest; });
}

bool HasClassOption(G::Ranked const& ranked)
{
    return std::any_of(ranked.options.begin(), ranked.options.end(),
                       [](G::Option const& option) { return option.reason == REASON_WORD; });
}

bool TopClassTrip(G::Ranked const& ranked)
{
    G::Option const* top = GH::Top(ranked);
    return top && top->reason == REASON_WORD && top->kind == G::GoalKind::Quest &&
           top->targetType == G::TargetType::Camp;
}

bool TopCircuitTaking(G::GoalPlan const& plan, std::uint32_t quest)
{
    G::Option const* top = GH::Top(plan.ranked);
    return top && top->reason == G::CIRCUIT_REASON && TakesClass(plan, quest);
}

G::ReplanInput Interval(G::CurrentGoal const& current, G::GoalPlan const& plan)
{
    G::ReplanInput in;
    in.current = current;
    in.fresh = &plan.ranked;
    in.nowMs = G::REPLAN_INTERVAL_MS + 10;
    in.lastPlanMs = 1;
    return in;
}

std::string Verdict(G::Replan const& replan)
{
    return std::string(G::ReplanWord(replan.verdict)) + " " + replan.reason +
           (replan.chosen ? " " + G::ChoiceWord(*replan.chosen) + " " + replan.chosen->reason : std::string());
}

void Wiring()
{
    Planner::Snapshot const bot = Orc();
    Planner::QuestIndex const far = World(false, bot);
    Planner::QuestIndex const near = World(true, bot);
    Planner::CircuitContext farContext;
    farContext.quests = &far;
    Planner::CircuitContext nearContext;
    nearContext.quests = &near;

    // A warrior at 10 with 1498 takeable: the lone trip over the ordinary circuit
    G::GoalPlan const lone = G::PlanGoals(bot, farContext, GovernedAt());
    std::printf("info: warrior 10, giver 1600 yd off: %s\n", Ranked(lone).c_str());
    Check(TopClassTrip(lone.ranked) && GH::Find(lone.ranked, G::GoalKind::Quest, GH::HOME_HUB),
          "wiring: a governed warrior 10 picks the lone trip to 1498 over its ordinary circuit");

    // The giver at the hub: the circuit takes 1498 with the quests near it, no lone trip
    G::GoalPlan const circuit = G::PlanGoals(bot, nearContext, GovernedAt());
    std::printf("info: warrior 10, giver at the hub: %s\n", Ranked(circuit).c_str());
    Check(TopCircuitTaking(circuit, CLASS_QUEST) && !HasClassOption(circuit.ranked) &&
              Planner::CircuitLog::Rows(circuit.circuit).size() > 0,
          "wiring: a circuit that takes 1498 with the quests near it stands for the class quest");
    bool classRow = false;
    for (auto const& row : Planner::CircuitLog::Rows(circuit.circuit))
        classRow = classRow || (row.choice == "take:1498" && row.reason == std::string(REASON_WORD));
    Check(classRow, "wiring: the circuit row of 1498 has the reason class_quest");

    // Mid-circuit: the circuit goes on (the stay test without the bonus keeps it), the trip comes when the circuit ends
    Planner::Snapshot running = bot;
    running.currentGoal = {static_cast<std::uint32_t>(G::GoalKind::Quest), GH::HOME_HUB, 1};
    G::GoalPlan const mid = G::PlanGoals(running, farContext, GovernedAt());
    G::CurrentGoal current{G::GoalKind::Quest, GH::HOME_HUB, 0.0f, false, false, G::TargetType::Default};
    G::Replan const stays = G::ReplanDecision(Interval(current, mid));
    G::Replan const bare = G::DecideReplan(Interval(current, mid));
    std::printf("info: mid-circuit: %s; with the bonus-less stay test: %s; without it: %s\n", Ranked(mid).c_str(),
                Verdict(stays).c_str(), Verdict(bare).c_str());
    Check(!mid.classBonus.empty() && stays.verdict == G::ReplanVerdict::Stay,
          "wiring: a bot mid-circuit finishes its circuit first (" + Verdict(stays) + ")");
    current.ended = true;
    G::Replan const after = G::ReplanDecision(Interval(current, mid));
    Check(after.verdict == G::ReplanVerdict::Switch && after.chosen && after.chosen->reason == REASON_WORD,
          "wiring: when the circuit ends the next decision takes the class trip (" + Verdict(after) + ")");

    // A bridge goal outranks the class quest; a sick bot rests first
    G::GoalPlan const bridge = G::PlanGoals(bot, farContext, GovernedAt());
    Planner::GoalApply::Slot bridgeSlot;
    Planner::GoalApply::Active bridgeGoal;
    bridgeGoal.kind = G::GoalKind::TravelElsewhere;
    bridgeGoal.target = 7;
    bridgeGoal.source = Planner::GoalApply::Source::Bridge;
    bridgeSlot.Set(bridgeGoal);
    std::printf("info: bridge goal held in the slot; the planner picks: %s\n", Ranked(bridge).c_str());
    Check(GH::Top(bridge.ranked) &&
              !Planner::GoalApply::MayReplace(bridgeSlot.Query(), Planner::GoalApply::Source::Planner),
          "wiring: a bridge goal outranks the class quest");
    Planner::Snapshot sick = bot;
    sick.sickMs = 60000;
    G::GoalPlan const resting = G::PlanGoals(sick, farContext, GovernedAt());
    std::printf("info: sick: %s\n", Ranked(resting).c_str());
    Check(GH::Top(resting.ranked) && GH::Top(resting.ranked)->kind == G::GoalKind::Rest &&
              !HasClassOption(resting.ranked),
          "wiring: a sick bot rests first");

    // No bonus: not governed, the spell known, given up
    G::GoalPlan const ungoverned = G::PlanGoals(bot, nearContext, GH::Base());
    G::Situation known = GovernedAt();
    known.knownClassQuestSpells = {71};
    G::GoalPlan const knows = G::PlanGoals(bot, farContext, known);
    G::Situation givenUp = GovernedAt();
    givenUp.givenUpQuests = {CLASS_QUEST};
    G::GoalPlan const gaveUp = G::PlanGoals(bot, farContext, givenUp);
    std::printf("info: ungoverned at the hub: %s\n", Ranked(ungoverned).c_str());
    Check(ungoverned.classBonus.empty() && !TakesClass(ungoverned, CLASS_QUEST) && knows.classBonus.empty() &&
              !HasClassOption(knows.ranked) && gaveUp.classBonus.empty() && !HasClassOption(gaveUp.ranked),
          "wiring: an ungoverned bot, a known spell and a given-up quest get no bonus and no trip");
    // WU39: the route test tells a way, definitely no way, and a search cut short or no graph apart
    {
        G::Model const model{};
        Planner::RouteGaps gaps;
        std::optional<G::ClassTrip> best;
        Planner::CircuitContext capped = farContext;
        capped.travel = [](Planner::Place const&, Planner::Place const&) { return Planner::TRAVEL_CAPPED; };
        Planner::CircuitContext none = farContext;
        none.travel = [](Planner::Place const&, Planner::Place const&) { return Planner::TRAVEL_NO_ROUTE; };
        Planner::QuestIndex otherMap = far;
        for (Planner::QuestPlace& place : otherMap.places)
            place.mapId += 1;
        Planner::CircuitContext noGraph;
        noGraph.quests = &otherMap;
        Route const walk = G::ClassQuestRoute(bot, farContext, model, CLASS_QUEST, gaps, best);
        bool const trip = best.has_value();
        Route const cut = G::ClassQuestRoute(bot, capped, model, CLASS_QUEST, gaps, best);
        Route const no = G::ClassQuestRoute(bot, none, model, CLASS_QUEST, gaps, best);
        Route const graph = G::ClassQuestRoute(bot, noGraph, model, CLASS_QUEST, gaps, best);
        Route const giverless = G::ClassQuestRoute(bot, farContext, model, 42, gaps, best);
        std::size_t const at = static_cast<std::size_t>(Planner::RouteCaller::ClassQuest);
        Check(walk == Route::Yes && trip && cut == Route::Unknown && no == Route::No && graph == Route::Unknown &&
                  giverless == Route::No && gaps.capped[at] == 1 && gaps.noRoute[at] == 1,
              "wiring: the route test answers yes, unknown for a search cut short (-2) or another map without a "
              "graph, no for -1 and for a quest without a giver");
        G::GoalPlan const cappedPlan = G::PlanGoals(bot, capped, GovernedAt());
        Check(cappedPlan.classBonus.empty() && !HasClassOption(cappedPlan.ranked),
              "wiring: a search cut short gives no bonus and no trip (unknown: nothing is failed either)");
    }
    // The feed (WT35d, SituationOf): the bot's failure record at GIVE_UP_LIMIT
    std::uint32_t constexpr RECORD_BOT = 21;
    QuestFail::Book record;
    record.Note(RECORD_BOT, CLASS_QUEST, "timeout", 10, 1);
    G::Situation fedOnce = GovernedAt();
    fedOnce.givenUpQuests = QuestFail::GivenUp(record.RowsOf(RECORD_BOT), GIVE_UP_LIMIT);
    G::GoalPlan const once = G::PlanGoals(bot, farContext, fedOnce);
    record.Note(RECORD_BOT, CLASS_QUEST, "no_target", 3, 2);
    G::Situation fedTwice = GovernedAt();
    fedTwice.givenUpQuests = QuestFail::GivenUp(record.RowsOf(RECORD_BOT), GIVE_UP_LIMIT);
    G::GoalPlan const twice = G::PlanGoals(bot, farContext, fedTwice);
    Check(fedOnce.givenUpQuests.empty() && !once.classBonus.empty() && HasClassOption(once.ranked),
          "wiring: a class quest given up once by the record keeps its bonus and trip");
    Check(fedTwice.givenUpQuests == std::vector<std::uint32_t>{CLASS_QUEST} && twice.classBonus.empty() &&
              !HasClassOption(twice.ranked),
          "wiring: given up twice, the record's feed withholds the bonus and the trip");
    Check(!Planner::GoalApply::Governed(true, 20) && !Planner::GoalApply::Governed(false, 21) &&
              Planner::GoalApply::Governed(true, 21),
          "wiring: a weighted-group bot and a bot that keeps no possessions are not governed");

    // The 1795 gap: a quest opened only by another's NextQuestId waits for that one
    Planner::Snapshot opened = bot;
    opened.rewardedQuests = {CLASS_PREV, GATE};
    std::sort(opened.rewardedQuests.begin(), opened.rewardedQuests.end());
    G::GoalPlan const before = G::PlanGoals(bot, nearContext, GH::Base());
    G::GoalPlan const afterGate = G::PlanGoals(opened, nearContext, GH::Base());
    std::vector<std::uint32_t> const& unlocks = near.Unlocks(GATE);
    Check(!GH::Takes(before.circuit, GATED) && GH::Takes(before.circuit, GATE) && GH::Takes(afterGate.circuit, GATED) &&
              std::find(unlocks.begin(), unlocks.end(), GATED) != unlocks.end(),
          "gap: a quest gated by another's NextQuestId is taken only after that one is rewarded");
}

// J1 and J2 over generated quest logs and planner states, each planned by PlanGoals on the two hubs
void Decisions(std::uint32_t seed)
{
    std::mt19937 rng(seed + 1);
    Planner::Snapshot const base = Orc();
    Planner::QuestIndex const worlds[2] = {World(false, base), World(true, base)};
    int const STATES = 3000;
    int j1 = 0, j1Flips = 0, j1Held = 0, j1Leaves = 0, j2 = 0, j2Trips = 0, j2Circuits = 0;
    std::string first1, first2;
    for (int n = 0; n < STATES; ++n)
    {
        bool const atHub = rng() % 2;
        Planner::CircuitContext context;
        context.quests = &worlds[atHub ? 1 : 0];
        Planner::Snapshot bot = base;
        bot.level = static_cast<std::uint8_t>(9 + rng() % 4);
        bot.race = rng() % 4 == 0 ? 1 : 2;
        bot.cls = rng() % 5 == 0 ? 2 : 1;
        bot.money = rng() % 20000;
        bot.rewardedQuests.clear();
        if (rng() % 5 != 0)
            bot.rewardedQuests.push_back(CLASS_PREV);
        bool const classDone = rng() % 10 == 0;
        if (classDone)
            bot.rewardedQuests.push_back(CLASS_QUEST);
        for (std::uint32_t quest = ORDINARY; quest < ORDINARY + ORDINARY_COUNT + 1; ++quest)
        {
            std::uint32_t const id = quest == ORDINARY + ORDINARY_COUNT ? GATE : quest;
            std::uint32_t const roll = rng() % 4;
            if (roll == 0)
                bot.rewardedQuests.push_back(id);
            else if (roll == 1)
                bot.quests.push_back({id, 0, {}});
        }
        bool const classInLog = !classDone && rng() % 10 == 0;
        if (classInLog)
            bot.quests.push_back({CLASS_QUEST, 0, {}});
        std::sort(bot.rewardedQuests.begin(), bot.rewardedQuests.end());
        bot.sickMs = rng() % 8 == 0 ? 60000 : 0;
        G::Situation situation = GH::Base();
        situation.governed = rng() % 5 != 0;
        situation.levelXp = 7000 + rng() % 3000;
        if (rng() % 6 == 0)
            situation.knownClassQuestSpells = {71};
        if (rng() % 10 == 0)
            situation.givenUpQuests = {CLASS_QUEST};
        situation.healthPercent = rng() % 5 == 0 ? static_cast<float>(20 + rng() % 60) : 100.0f;
        if (rng() % 2)
            situation.spots = {GH::Spot(21, static_cast<float>(40 + rng() % 200), 5.0f)};
        if (rng() % 2)
            situation.hubs = {
                GH::Hub(31, bot.zone, static_cast<float>(50 + rng() % 400), static_cast<float>(2 + rng() % 20))};
        int const goal = static_cast<int>(rng() % 4);  // none, the circuit, a move to hub 31, grinding spot 21
        G::CurrentGoal current;
        if (goal == 1)
            current = {G::GoalKind::Quest, GH::HOME_HUB, 0.0f, false, false, G::TargetType::Default};
        else if (goal == 2)
        {
            if (situation.hubs.empty())
                situation.hubs = {GH::Hub(31, bot.zone, 200.0f, 10.0f)};
            current = {G::GoalKind::Quest, 31, 0.0f, false, true, G::TargetType::Default};
        }
        else if (goal == 3)
        {
            if (situation.spots.empty())
                situation.spots = {GH::Spot(21, 120.0f, 5.0f)};
            current = {G::GoalKind::Grind, 21, 0.0f, false, false, G::TargetType::Default};
        }
        current.valuePerMinute = static_cast<float>(rng() % 600);
        if (goal)
            bot.currentGoal = {static_cast<std::uint32_t>(current.kind), current.target, 1};
        G::GoalPlan const plan = G::PlanGoals(bot, context, situation);
        std::string const state = "state " + std::to_string(n) + " (class " + std::to_string(bot.cls) + ", race " +
                                  std::to_string(bot.race) + ", level " + std::to_string(bot.level) +
                                  (atHub ? ", giver at the hub" : ", giver far") + ", goal " + std::to_string(goal) +
                                  ")";

        if (goal)
        {
            // J1: what the plan without the bonus keeps, the bot keeps
            G::Situation plainSituation = situation;
            plainSituation.governed = false;
            G::GoalPlan const without = G::PlanGoals(bot, context, plainSituation);
            G::ReplanInput in = Interval(current, plan);
            if (rng() % 3 == 0)
                in.lastPlanMs = in.nowMs - 1000;  // no interval trigger
            in.delta.situation = rng() % 4 == 0;
            G::Replan const with = G::ReplanDecision(in);
            G::ReplanInput plainIn = in;
            plainIn.fresh = &without.ranked;
            G::Replan const reference = G::ReplanDecision(plainIn);
            G::Replan const unguarded = G::DecideReplan(in);
            ++j1;
            if (reference.verdict == G::ReplanVerdict::Stay && with.verdict != G::ReplanVerdict::Stay)
            {
                ++j1Flips;
                if (first1.empty())
                    first1 = state + ": without the bonus " + Verdict(reference) + ", with it " + Verdict(with) + "; " +
                             Ranked(plan);
            }
            if (reference.verdict == G::ReplanVerdict::Stay && unguarded.verdict != G::ReplanVerdict::Stay)
                ++j1Held;
            if (with.verdict != G::ReplanVerdict::Stay)
                ++j1Leaves;
            continue;
        }

        // J2: a governed bot without the spells, with 1498 takeable and doable, not sick, at full health, no goal
        bool const prev = std::binary_search(bot.rewardedQuests.begin(), bot.rewardedQuests.end(), CLASS_PREV);
        bool const takeable = situation.governed && bot.cls == 1 && bot.race == 2 && bot.level >= 10 && prev &&
                              !classDone && !classInLog && situation.knownClassQuestSpells.empty() &&
                              situation.givenUpQuests.empty();
        if (!takeable || bot.sickMs || situation.healthPercent < 100.0f)
            continue;
        ++j2;
        bool const bonus = !plan.classBonus.empty() && plan.classBonus.front().first == CLASS_QUEST;
        bool const trip = TopClassTrip(plan.ranked);
        bool const taking = TopCircuitTaking(plan, CLASS_QUEST);
        j2Trips += trip ? 1 : 0;
        j2Circuits += taking ? 1 : 0;
        if (!(bonus && (trip || taking)) && first2.empty())
            first2 = state + ": " + Ranked(plan);
    }
    std::printf(
        "info: invariant J1/J2 seed %u, %d states: J1 %d with a running goal (%d leave, %d kept only by the "
        "bonus-less stay test), J2 %d (%d lone trips, %d circuits)\n",
        seed + 1, STATES, j1, j1Leaves, j1Held, j2, j2Trips, j2Circuits);
    // The stay test subtracts the bonus from the one plan; a plan without the bonus routes its circuit differently, so
    // the bonus-less rate is an estimate and a flip can happen (WT65: 2 of 2255): allowed in at most 1% of the states
    Check(j1 > 0 && j1Flips * 100 <= j1,
          "J1: the bonus rarely ends a running trip or circuit the bonus-less plan keeps (" + std::to_string(j1Flips) +
              " of " + std::to_string(j1) + ")" +
              (first1.empty() ? std::string() : ": first counterexample " + first1));
    Check(first2.empty() && j2 > 0 && j2Trips > 0 && j2Circuits > 0,
          "J2: a governed bot with a takeable class quest picks it or a circuit that takes it" +
              (first2.empty() ? std::string() : ": first counterexample " + first2));
}
}  // namespace

int main(int argc, char** argv)
{
    std::uint32_t const seed = argc > 1 ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 20261114u;
    Table();
    JoinEquality();
    Scenarios();
    NamedCases();
    Planner::QuestIndex const fixture = Fixture();
    Invariant("fixture", fixture.classRows, seed, NoDue, false);
    Invariant("fixture", fixture.classRows, seed, RulesDue, true);
    Invariant("lab7 rows", D(), seed + 7, RulesDue, true);
    Wiring();
    Decisions(seed);
    std::printf("class quest selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
