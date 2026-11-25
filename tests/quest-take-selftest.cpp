/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the take rules (questing WQ17a, QuestTakeRules.h): a table with one case per reason word and the
// order of the rules, then generated bots and quests (the seed is printed; argv[1] replays it) checked against the
// invariants: a quest in the log or rewarded is never taken, nor one the capability table refuses or the bot gave up;
// the same input gives the same answer; the circuit's skip for every answer is the one its rules gave before WQ17a
// (OldTakeable below, a copy of them as they stood); the minimum level, race, class, maximum level and capability
// words agree with the feed's Eligible.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel tests/quest-take-selftest.cpp

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "QuestCircuit.h"
#include "QuestTakeRules.h"

using namespace Planner;
using Take::Reason;

namespace
{
constexpr std::uint32_t DEFAULT_SEED = 20261125;
constexpr std::size_t BOTS = 400;
constexpr std::uint32_t QUESTS = 120;  // ids 1 to QUESTS; breadcrumbs may name QUESTS + 1, which is missing
constexpr std::uint8_t HUMAN = 1;      // Alliance
constexpr std::uint8_t ORC = 2;        // Horde
constexpr std::uint8_t WARRIOR = 1;
constexpr std::uint8_t MAGE = 8;
constexpr std::uint32_t PRINTED_FAILURES = 20;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
    {
        ++passed;
        return;
    }
    if (static_cast<std::uint32_t>(failed) < PRINTED_FAILURES)
        std::printf("FAIL %s\n", what.c_str());
    ++failed;
}

std::string Answer(std::optional<Reason> why) { return why ? Take::Word(*why) : "yes"; }

// The bot's lists, owned here; Facts views them
struct Lists
{
    std::vector<std::uint32_t> rewarded;
    std::vector<std::uint32_t> inLog;
    std::vector<std::uint32_t> givenUp;
    std::vector<std::int32_t> takenGroups;
};

Lists Done(std::vector<std::uint32_t> ids)
{
    Lists lists;
    lists.rewarded = std::move(ids);
    return lists;
}

Lists InLog(std::vector<std::uint32_t> ids)
{
    Lists lists;
    lists.inLog = std::move(ids);
    return lists;
}

Lists GaveUp(std::vector<std::uint32_t> ids)
{
    Lists lists;
    lists.givenUp = std::move(ids);
    return lists;
}

Lists Groups(std::vector<std::int32_t> groups)
{
    Lists lists;
    lists.takenGroups = std::move(groups);
    return lists;
}

Take::Facts BotOf(std::uint8_t race, std::uint8_t cls, std::uint8_t level)
{
    Take::Facts facts;
    facts.race = race;
    facts.cls = cls;
    facts.level = level;
    return facts;
}

Take::Facts View(Take::Facts facts, Lists const& lists)
{
    facts.rewarded = lists.rewarded;
    facts.inLog = lists.inLog;
    facts.givenUp = lists.givenUp;
    facts.takenGroups = lists.takenGroups;
    return facts;
}

QuestIndex IndexOf(std::vector<QuestRecord> quests)
{
    std::sort(quests.begin(), quests.end(), [](QuestRecord const& a, QuestRecord const& b) { return a.id < b.id; });
    QuestIndex index;
    index.quests = std::move(quests);
    return index;
}

// The circuit's take rules as they stood before WQ17a (QuestCircuit.h Takeable and PrerequisiteMet at ec180f16),
// with the bot's group size: the oracle that the circuit's skips do not change
struct OldBot
{
    std::uint8_t race{0};
    std::uint8_t cls{0};
    std::uint8_t level{0};
    std::uint8_t groupSize{0};
    std::uint32_t greyDiff{Circuit::GREY_DIFF};
    std::vector<std::uint32_t> rewarded;
    std::vector<std::uint32_t> logIds;
    std::vector<std::int32_t> takenGroups;
};

bool OldHas(std::vector<std::uint32_t> const& sorted, std::uint32_t id)
{
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

std::optional<SkipReason> OldTakeable(QuestRecord const& quest, OldBot const& bot, QuestIndex const& quests,
                                      bool breadcrumbRule = true)
{
    if (OldHas(bot.rewarded, quest.id))
        return SkipReason::Rewarded;
    if (OldHas(bot.logIds, quest.id))
        return SkipReason::InLog;
    if (quest.flags & (QuestFlag::DAILY | QuestFlag::WEEKLY | QuestFlag::REPEATABLE | QuestFlag::SEASONAL))
        return SkipReason::Repeatable;
    if (bot.level < quest.minLevel)
        return SkipReason::TooHigh;
    if (!Eligible(quest, bot.race, bot.cls, bot.level))
        return SkipReason::Limits;
    std::int32_t const level = quest.level > 0 ? quest.level : static_cast<std::int32_t>(bot.level);
    if (level > static_cast<std::int32_t>(bot.level) + 3)
        return SkipReason::TooHigh;
    if (static_cast<std::int32_t>(bot.level) > level + static_cast<std::int32_t>(bot.greyDiff))
        return SkipReason::Grey;
    if (quest.flags & (QuestFlag::DUNGEON | QuestFlag::RAID | QuestFlag::PVP))
        return SkipReason::Instance;
    if ((quest.flags & (QuestFlag::ELITE | QuestFlag::GROUP)) && bot.groupSize <= 1)
        return SkipReason::Solo;
    auto const met = [&bot](std::int32_t prev)
    {
        return prev > 0 ? OldHas(bot.rewarded, static_cast<std::uint32_t>(prev))
                        : OldHas(bot.logIds, static_cast<std::uint32_t>(-prev));
    };
    bool const prerequisite = quest.prevAny.empty() ? (!quest.prev || met(quest.prev))
                                                    : std::any_of(quest.prevAny.begin(), quest.prevAny.end(), met);
    if (!prerequisite)
        return SkipReason::Prerequisite;
    if (quest.exclusiveGroup > 0 &&
        std::binary_search(bot.takenGroups.begin(), bot.takenGroups.end(), quest.exclusiveGroup))
        return SkipReason::Exclusive;
    if (breadcrumbRule && quest.breadcrumbFor)
    {
        QuestRecord const* target = quests.Find(quest.breadcrumbFor);
        if (!target || !OldTakeable(*target, bot, quests, false).has_value() || OldHas(bot.rewarded, target->id) ||
            OldHas(bot.logIds, target->id))
            return SkipReason::Breadcrumb;
    }
    return std::nullopt;
}

QuestRecord Plain(std::uint32_t id)
{
    QuestRecord quest;
    quest.id = id;
    quest.level = 20;
    quest.minLevel = 15;
    return quest;
}

// One case per reason word, the order of the rules and the inputs of WQ.43 (elite and group quests allowed or not)
void Table()
{
    constexpr std::uint32_t TARGET = 200;    // a takeable breadcrumb target
    constexpr std::uint32_t HIGH = 201;      // a breadcrumb target too high for the bot
    constexpr std::uint32_t GIVEN_UP = 202;  // a breadcrumb target the bot gave up
    constexpr std::uint32_t MISSING = 999;   // not in the index
    QuestRecord high = Plain(HIGH);
    high.level = 30;
    high.minLevel = 25;
    QuestIndex const index = IndexOf({Plain(TARGET), high, Plain(GIVEN_UP)});
    Take::Facts const base = BotOf(HUMAN, WARRIOR, 20);

    struct Case
    {
        char const* name;
        char const* expect;
        QuestRecord quest;
        Lists lists;
        bool eliteGroup;
    };
    std::vector<Case> cases;
    auto const add =
        [&cases](char const* name, char const* expect, auto&& change, Lists lists = {}, bool eliteGroup = false)
    {
        QuestRecord quest = Plain(100);
        change(quest);
        cases.push_back({name, expect, quest, std::move(lists), eliteGroup});
    };
    auto const same = [](QuestRecord&) {};
    add("plain", "yes", same);
    add("player's level", "yes", [](QuestRecord& q) { q.level = -1; });
    add("rewarded", "done", same, Done({100}));
    add("in the log", "in_log", same, InLog({100}));
    add("given up", "given_up", same, GaveUp({100}));
    add("daily", "repeatable", [](QuestRecord& q) { q.flags = QuestFlag::DAILY; });
    add("seasonal", "repeatable", [](QuestRecord& q) { q.flags = QuestFlag::SEASONAL; });
    add("minimum level", "level", [](QuestRecord& q) { q.minLevel = 21; });
    add("race", "limits", [](QuestRecord& q) { q.raceMask = 1u << (ORC - 1); });
    add("class", "limits", [](QuestRecord& q) { q.classMask = 1u << (MAGE - 1); });
    add("race and class allowed", "yes",
        [](QuestRecord& q)
        {
            q.raceMask = 1u << (HUMAN - 1);
            q.classMask = 1u << (WARRIOR - 1);
        });
    add("maximum level", "limits", [](QuestRecord& q) { q.maxLevel = 19; });
    add("refused for the side", "unsupported", [](QuestRecord& q) { q.refused[0] = 1; });
    add("refused for the other side", "yes", [](QuestRecord& q) { q.refused[1] = 1; });
    add("3 levels above", "yes", [](QuestRecord& q) { q.level = 23; });
    add("4 levels above", "too_high", [](QuestRecord& q) { q.level = 24; });
    add("4 levels below", "yes", [](QuestRecord& q) { q.level = 16; });
    add("5 levels below", "grey", [](QuestRecord& q) { q.level = 15; });
    add("dungeon", "kind", [](QuestRecord& q) { q.flags = QuestFlag::DUNGEON; });
    add("pvp", "kind", [](QuestRecord& q) { q.flags = QuestFlag::PVP; });
    add("elite refused", "elite_group", [](QuestRecord& q) { q.flags = QuestFlag::ELITE; });
    add("group refused", "elite_group", [](QuestRecord& q) { q.flags = QuestFlag::GROUP; });
    add("elite allowed", "yes", [](QuestRecord& q) { q.flags = QuestFlag::ELITE; }, {}, true);
    add("raid while elite allowed", "kind", [](QuestRecord& q) { q.flags = QuestFlag::RAID; }, {}, true);
    add("previous not rewarded", "prerequisite", [](QuestRecord& q) { q.prev = 50; });
    add("previous rewarded", "yes", [](QuestRecord& q) { q.prev = 50; }, Done({50}));
    add("active previous not in the log", "prerequisite", [](QuestRecord& q) { q.prev = -60; });
    add("active previous in the log", "yes", [](QuestRecord& q) { q.prev = -60; }, InLog({60}));
    add("any previous, none met", "prerequisite", [](QuestRecord& q) { q.prevAny = {50, -60}; });
    add("any previous, one met", "yes", [](QuestRecord& q) { q.prevAny = {50, -60}; }, InLog({60}));
    add("exclusive group taken", "exclusive", [](QuestRecord& q) { q.exclusiveGroup = 7; }, Groups({7}));
    add("exclusive group free", "yes", [](QuestRecord& q) { q.exclusiveGroup = 7; }, Groups({8}));
    add("breadcrumb to a takeable target", "breadcrumb", [](QuestRecord& q) { q.breadcrumbFor = TARGET; });
    add("breadcrumb to a missing target", "breadcrumb", [](QuestRecord& q) { q.breadcrumbFor = MISSING; });
    add("breadcrumb to a done target", "breadcrumb", [](QuestRecord& q) { q.breadcrumbFor = TARGET; }, Done({TARGET}));
    add(
        "breadcrumb to a target in the log", "breadcrumb", [](QuestRecord& q) { q.breadcrumbFor = TARGET; },
        InLog({TARGET}));
    add("breadcrumb to a target too high", "yes", [](QuestRecord& q) { q.breadcrumbFor = HIGH; });
    // A given-up target cannot be taken, so its breadcrumb can: the rule asks only whether the target is open
    add(
        "breadcrumb to a given-up target", "yes", [](QuestRecord& q) { q.breadcrumbFor = GIVEN_UP; },
        GaveUp({GIVEN_UP}));
    // The order: the first rule that fails is the answer
    add("rewarded and daily", "done", [](QuestRecord& q) { q.flags = QuestFlag::DAILY; }, Done({100}));
    add("in the log and rewarded", "done", same, Lists{{100}, {100}, {}, {}});
    add("given up and daily", "given_up", [](QuestRecord& q) { q.flags = QuestFlag::DAILY; }, GaveUp({100}));
    add("daily below the minimum level", "repeatable",
        [](QuestRecord& q)
        {
            q.flags = QuestFlag::DAILY;
            q.minLevel = 30;
        });
    add("minimum level and race", "level",
        [](QuestRecord& q)
        {
            q.minLevel = 21;
            q.raceMask = 1u << (ORC - 1);
        });
    add("race and refused", "limits",
        [](QuestRecord& q)
        {
            q.raceMask = 1u << (ORC - 1);
            q.refused[0] = 1;
        });
    add("refused and too high", "unsupported",
        [](QuestRecord& q)
        {
            q.refused[0] = 1;
            q.level = 30;
        });
    add("grey dungeon", "grey",
        [](QuestRecord& q)
        {
            q.level = 10;
            q.flags = QuestFlag::DUNGEON;
        });
    add("elite dungeon", "kind", [](QuestRecord& q) { q.flags = QuestFlag::DUNGEON | QuestFlag::ELITE; });
    add("elite with a missing previous", "elite_group",
        [](QuestRecord& q)
        {
            q.flags = QuestFlag::ELITE;
            q.prev = 50;
        });
    add(
        "missing previous and exclusive", "prerequisite",
        [](QuestRecord& q)
        {
            q.prev = 50;
            q.exclusiveGroup = 7;
        },
        Groups({7}));
    add(
        "exclusive breadcrumb", "exclusive",
        [](QuestRecord& q)
        {
            q.exclusiveGroup = 7;
            q.breadcrumbFor = TARGET;
        },
        Groups({7}));

    std::array<bool, static_cast<std::size_t>(Reason::Count)> seen{};
    for (Case const& c : cases)
    {
        Take::Facts facts = View(base, c.lists);
        facts.eliteGroup = c.eliteGroup;
        std::optional<Reason> const why = Take::May(c.quest, facts, index);
        if (why)
            seen[static_cast<std::size_t>(*why)] = true;
        Check(Answer(why) == c.expect, std::string("table: ") + c.name + ": " + Answer(why) + ", expected " + c.expect);
    }
    for (std::size_t i = 0; i < seen.size(); ++i)
        Check(seen[i], std::string("table: no case for ") + Take::WORDS[i]);
    std::array<std::string, static_cast<std::size_t>(Reason::Count)> const words = {
        "done",     "in_log", "given_up", "repeatable",  "level",        "limits",    "unsupported",
        "too_high", "grey",   "kind",     "elite_group", "prerequisite", "exclusive", "breadcrumb"};
    for (std::size_t i = 0; i < words.size(); ++i)
        Check(words[i] == Take::WORDS[i], std::string("word ") + std::to_string(i) + ": " + Take::WORDS[i]);
}

// The circuit's skip for each reason: the words its log rows print stay as they were
void Skips()
{
    struct Row
    {
        Reason reason;
        char const* name;
    };
    std::array<Row, static_cast<std::size_t>(Reason::Count)> const rows = {{
        {Reason::Done, "rewarded"},
        {Reason::InLog, "in log"},
        {Reason::GivenUp, "race/class/max level"},
        {Reason::Repeatable, "repeatable"},
        {Reason::Level, "too high"},
        {Reason::Limits, "race/class/max level"},
        {Reason::Unsupported, "race/class/max level"},
        {Reason::TooHigh, "too high"},
        {Reason::Grey, "grey"},
        {Reason::Kind, "instance"},
        {Reason::EliteGroup, "elite/group solo"},
        {Reason::Prerequisite, "prerequisite"},
        {Reason::Exclusive, "exclusive"},
        {Reason::Breadcrumb, "breadcrumb"},
    }};
    for (Row const& row : rows)
        Check(std::string(SkipName(SkipOf(row.reason))) == row.name,
              std::string("skip of ") + Take::Word(row.reason) + ": " + SkipName(SkipOf(row.reason)));
}

std::vector<std::uint32_t> SomeQuests(std::mt19937& rng, std::uint32_t percent)
{
    std::vector<std::uint32_t> ids;
    for (std::uint32_t id = 1; id <= QUESTS; ++id)
        if (rng() % 100 < percent)
            ids.push_back(id);
    return ids;
}

QuestRecord Generated(std::mt19937& rng, std::uint32_t id)
{
    auto const roll = [&rng](std::uint32_t percent) { return rng() % 100 < percent; };
    auto const pick = [&rng](std::uint32_t low, std::uint32_t high) { return low + rng() % (high - low + 1); };
    QuestRecord quest;
    quest.id = id;
    quest.level = roll(10) ? -1 : static_cast<std::int32_t>(pick(1, 30));
    quest.minLevel = roll(15) ? pick(1, 32) : static_cast<std::uint32_t>(std::max(1, quest.level - 3));
    quest.maxLevel = roll(5) ? pick(5, 30) : 0;
    quest.raceMask = roll(10) ? (1u << pick(0, 10)) | (1u << pick(0, 10)) : 0;
    quest.classMask = roll(10) ? (1u << pick(0, 10)) : 0;
    std::array<std::uint16_t, 9> const flags = {QuestFlag::DAILY,    QuestFlag::WEEKLY,  QuestFlag::REPEATABLE,
                                                QuestFlag::SEASONAL, QuestFlag::DUNGEON, QuestFlag::RAID,
                                                QuestFlag::PVP,      QuestFlag::ELITE,   QuestFlag::GROUP};
    for (std::uint16_t const flag : flags)
        if (roll(4))
            quest.flags |= flag;
    if (roll(20))
        quest.prev = static_cast<std::int32_t>(pick(1, QUESTS)) * (roll(30) ? -1 : 1);
    if (roll(5))
        quest.prevAny = {static_cast<std::int32_t>(pick(1, QUESTS)), -static_cast<std::int32_t>(pick(1, QUESTS))};
    if (roll(15))
        quest.exclusiveGroup = static_cast<std::int32_t>(pick(1, 6));
    if (roll(15))
        quest.breadcrumbFor = pick(1, QUESTS + 1);
    quest.refused[0] = static_cast<std::uint8_t>(roll(5));
    quest.refused[1] = static_cast<std::uint8_t>(roll(5));
    return quest;
}

// Generated bots against every generated quest
void GeneratedCases(std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<QuestRecord> records;
    for (std::uint32_t id = 1; id <= QUESTS; ++id)
        records.push_back(Generated(rng, id));
    QuestIndex const index = IndexOf(records);

    std::array<std::uint32_t, static_cast<std::size_t>(Reason::Count)> counts{};
    std::uint32_t taken = 0;
    std::uint32_t cases = 0;
    for (std::size_t b = 0; b < BOTS; ++b)
    {
        Lists lists;
        lists.rewarded = SomeQuests(rng, 10);
        lists.inLog = SomeQuests(rng, 8);
        lists.givenUp = SomeQuests(rng, 5);
        for (std::vector<std::uint32_t> const* list : {&lists.rewarded, &lists.inLog})
            for (std::uint32_t const id : *list)
                if (QuestRecord const* quest = index.Find(id); quest && quest->exclusiveGroup > 0)
                    lists.takenGroups.push_back(quest->exclusiveGroup);
        std::sort(lists.takenGroups.begin(), lists.takenGroups.end());
        std::uint8_t const groupSize = static_cast<std::uint8_t>(rng() % 4);
        std::uint8_t const race = static_cast<std::uint8_t>(rng() % 13);  // 0, 9 and 12: no side
        std::uint8_t const cls = static_cast<std::uint8_t>(1 + rng() % 11);
        std::uint8_t const level = static_cast<std::uint8_t>(1 + rng() % 30);
        Take::Facts bot = BotOf(race, cls, level);
        if (rng() % 4 == 0)
            bot.greyDiff = 2 + rng() % 6;
        bot.eliteGroup = groupSize > 1;
        Lists const noGiveUps{lists.rewarded, lists.inLog, {}, lists.takenGroups};
        Take::Facts const withGiveUps = View(bot, lists);
        Take::Facts const circuit = View(bot, noGiveUps);
        Lists const copy = lists;  // the same input in other storage
        Take::Facts const again = View(bot, copy);
        OldBot const old{bot.race,     bot.cls,        bot.level,   groupSize,
                         bot.greyDiff, lists.rewarded, lists.inLog, lists.takenGroups};
        std::string const who = "seed " + std::to_string(seed) + " bot " + std::to_string(b);
        for (QuestRecord const& quest : index.quests)
        {
            ++cases;
            std::string const what = who + " quest " + std::to_string(quest.id);
            std::optional<Reason> const why = Take::May(quest, withGiveUps, index);
            Check(Take::May(quest, withGiveUps, index) == why && Take::May(quest, again, index) == why,
                  what + ": the same input, another answer");
            bool const logOrDone = Take::Holds(lists.rewarded, quest.id) || Take::Holds(lists.inLog, quest.id);
            Check(!logOrDone || (why && (*why == Reason::Done || *why == Reason::InLog)),
                  what + ": in the log or rewarded, answer " + Answer(why));
            bool const refused = RefusedFor(quest, SideOfRace(bot.race)) || Take::Holds(lists.givenUp, quest.id);
            Check(!refused || why.has_value(), what + ": refused or given up, taken");

            std::optional<Reason> const forCircuit = Take::May(quest, circuit, index);
            std::optional<SkipReason> const before = OldTakeable(quest, old, index);
            std::optional<SkipReason> const after =
                forCircuit ? std::optional<SkipReason>(SkipOf(*forCircuit)) : std::nullopt;
            Check(after == before, what + ": the circuit's skip " + (after ? SkipName(*after) : "none") +
                                       ", before WQ17a " + (before ? SkipName(*before) : "none"));

            bool const eligible = Eligible(quest, bot.race, bot.cls, bot.level);
            bool const eligibility = forCircuit && (*forCircuit == Reason::Level || *forCircuit == Reason::Limits ||
                                                    *forCircuit == Reason::Unsupported);
            bool const decidedBefore = forCircuit && *forCircuit < Reason::Level;
            Check(decidedBefore || eligibility == !eligible,
                  what + ": answer " + Answer(forCircuit) + ", Eligible " + (eligible ? "yes" : "no"));

            if (why)
                ++counts[static_cast<std::size_t>(*why)];
            else
                ++taken;
        }
    }
    std::printf("seed=%u cases=%u taken=%u", seed, cases, taken);
    for (std::size_t i = 0; i < counts.size(); ++i)
        std::printf(" %s=%u", Take::WORDS[i], counts[i]);
    std::printf("\n");
    Check(taken > 0, "generated: nothing taken");
    for (std::size_t i = 0; i < counts.size(); ++i)
        Check(counts[i] > 0, std::string("generated: no case answered ") + Take::WORDS[i]);
}
}  // namespace

int main(int argc, char** argv)
{
    std::uint32_t seed = DEFAULT_SEED;
    if (argc > 1)
        seed = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    Table();
    Skips();
    GeneratedCases(seed);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
