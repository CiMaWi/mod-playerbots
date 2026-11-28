/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the take test (questing WQ17a, QuestTakeRules.h; WQ41: the game decides what it allows). A table
// with one case per reason word and the order of the rules, then generated bots, quests and game answers (the seed is
// printed; argv[1] replays it) checked against the invariants: the game's answer decides (Facts::takeable, the core's
// CanTakeQuest on the bot's map thread): a quest May allows is in it, and a quest in it is taken unless one of the
// plan's own rules (rewarded, in the log, given up, listed, repeatable, refused for the side, red, grey, kind,
// elite or group, a breadcrumb to an answered target) leaves it; the plan's own rules hold whatever the answer; the
// same input gives the same answer; the circuit's skip for every reason. A quest that needs a skill the bot lacks is
// not in the answer, and the plan holds no copy of the game's rule: a quest of a higher minimum level, another race or
// class or a missing previous quest is taken when the game answers it. Build (Git Bash, module root):
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
constexpr std::uint8_t MAGE = 8;
constexpr std::uint32_t PRINTED_FAILURES = 20;
constexpr std::uint32_t QUEST = 100;   // the table's quest
constexpr std::uint32_t TARGET = 200;  // a breadcrumb target the game answers

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

// The bot's lists, owned here; Facts views them. The table's game answers its quest and the breadcrumb target
struct Lists
{
    std::vector<std::uint32_t> rewarded;
    std::vector<std::uint32_t> inLog;
    std::vector<std::uint32_t> givenUp;
    std::vector<std::uint32_t> takeable{QUEST, TARGET};  // the game's answer
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

Lists Answered(std::vector<std::uint32_t> ids)
{
    Lists lists;
    lists.takeable = std::move(ids);
    return lists;
}

Take::Facts BotOf(std::uint8_t race, std::uint8_t level)
{
    Take::Facts facts;
    facts.race = race;
    facts.level = level;
    return facts;
}

Take::Facts View(Take::Facts facts, Lists const& lists)
{
    facts.rewarded = lists.rewarded;
    facts.inLog = lists.inLog;
    facts.givenUp = lists.givenUp;
    facts.takeable = lists.takeable;
    return facts;
}

QuestIndex IndexOf(std::vector<QuestRecord> quests)
{
    std::sort(quests.begin(), quests.end(), [](QuestRecord const& a, QuestRecord const& b) { return a.id < b.id; });
    QuestIndex index;
    index.quests = std::move(quests);
    return index;
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
    constexpr std::uint32_t UNANSWERED = 201;  // a breadcrumb target the game leaves out
    constexpr std::uint32_t MISSING = 999;     // not in the index, nor in the answer
    Take::Facts const base = BotOf(HUMAN, 20);
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
        QuestRecord quest = Plain(QUEST);
        change(quest);
        cases.push_back({name, expect, quest, std::move(lists), eliteGroup});
    };
    auto const same = [](QuestRecord&) {};
    add("plain", "yes", same);
    add("player's level", "yes", [](QuestRecord& q) { q.level = -1; });
    add("rewarded", "done", same, Done({QUEST}));
    add("in the log", "in_log", same, InLog({QUEST}));
    add("given up", "given_up", same, GaveUp({QUEST}));
    add("daily", "repeatable", [](QuestRecord& q) { q.flags = QuestFlag::DAILY; });
    add("seasonal", "repeatable", [](QuestRecord& q) { q.flags = QuestFlag::SEASONAL; });
    add("refused for the side", "unsupported", [](QuestRecord& q) { q.refused[0] = 1; });
    add("listed as impossible", "impossible", [](QuestRecord& q) { q.impossible = "far_trip"; });  // WQ38
    add("listed and in the log", "in_log", [](QuestRecord& q) { q.impossible = "data"; }, InLog({QUEST}));
    add("refused for the other side", "yes", [](QuestRecord& q) { q.refused[1] = 1; });
    add("3 levels above", "yes", [](QuestRecord& q) { q.level = 23; });
    add("4 levels above (orange)", "yes", [](QuestRecord& q) { q.level = 24; });
    add("5 levels above (red)", "red", [](QuestRecord& q) { q.level = 25; });
    add("6 levels below", "yes", [](QuestRecord& q) { q.level = 14; });  // the game's grey level at 20 is 13 (WQ34)
    add("7 levels below", "grey", [](QuestRecord& q) { q.level = 13; });
    add("dungeon", "kind", [](QuestRecord& q) { q.flags = QuestFlag::DUNGEON; });
    add("pvp", "kind", [](QuestRecord& q) { q.flags = QuestFlag::PVP; });
    add("elite refused", "elite_group", [](QuestRecord& q) { q.flags = QuestFlag::ELITE; });
    add("group refused", "elite_group", [](QuestRecord& q) { q.flags = QuestFlag::GROUP; });
    add("elite allowed", "yes", [](QuestRecord& q) { q.flags = QuestFlag::ELITE; }, {}, true);
    add("raid while elite allowed", "kind", [](QuestRecord& q) { q.flags = QuestFlag::RAID; }, {}, true);
    // WQ41: the game decides; the plan keeps no copy of its rule
    add("not in the game's answer", "game", same, Answered({TARGET}));
    add("an empty answer", "game", same, Answered({}));
    add("in the answer above its minimum level", "yes", [](QuestRecord& q) { q.minLevel = 21; });
    add("in the answer for another race", "yes", [](QuestRecord& q) { q.raceMask = 1u << (ORC - 1); });
    add("in the answer for another class", "yes", [](QuestRecord& q) { q.classMask = 1u << (MAGE - 1); });
    add("in the answer above its maximum level", "yes", [](QuestRecord& q) { q.maxLevel = 19; });
    add("in the answer with its previous not rewarded", "yes", [](QuestRecord& q) { q.prev = 50; });
    add("in the answer with an exclusive group", "yes", [](QuestRecord& q) { q.exclusiveGroup = 7; });
    add("breadcrumb to an answered target", "breadcrumb", [](QuestRecord& q) { q.breadcrumbFor = TARGET; });
    add("breadcrumb to a target the game leaves out", "yes", [](QuestRecord& q) { q.breadcrumbFor = UNANSWERED; });
    add("breadcrumb to a missing target", "yes", [](QuestRecord& q) { q.breadcrumbFor = MISSING; });
    // The order: the first rule that fails is the answer; the plan's own rules before the game's answer
    add("rewarded and daily", "done", [](QuestRecord& q) { q.flags = QuestFlag::DAILY; }, Done({QUEST}));
    add("in the log and rewarded", "done", same, Lists{{QUEST}, {QUEST}, {}, {QUEST}});
    add("given up and daily", "given_up", [](QuestRecord& q) { q.flags = QuestFlag::DAILY; }, GaveUp({QUEST}));
    add("daily left out of the answer", "repeatable", [](QuestRecord& q) { q.flags = QuestFlag::DAILY; }, Answered({}));
    add("refused and red", "unsupported",
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
    add(
        "elite left out of the answer", "elite_group", [](QuestRecord& q) { q.flags = QuestFlag::ELITE; },
        Answered({}));
    add(
        "breadcrumb left out of the answer", "game", [](QuestRecord& q) { q.breadcrumbFor = TARGET; },
        Answered({TARGET}));

    std::array<bool, static_cast<std::size_t>(Reason::Count)> seen{};
    for (Case const& c : cases)
    {
        Take::Facts facts = View(base, c.lists);
        facts.eliteGroup = c.eliteGroup;
        std::optional<Reason> const why = Take::May(c.quest, facts);
        if (why)
            seen[static_cast<std::size_t>(*why)] = true;
        Check(Answer(why) == c.expect, std::string("table: ") + c.name + ": " + Answer(why) + ", expected " + c.expect);
    }
    for (std::size_t i = 0; i < seen.size(); ++i)
        Check(seen[i], std::string("table: no case for ") + Take::WORDS[i]);
    std::array<std::string, static_cast<std::size_t>(Reason::Count)> const words = {
        "done", "in_log", "given_up", "impossible",  "repeatable", "unsupported",
        "red",  "grey",   "kind",     "elite_group", "game",       "breadcrumb"};
    for (std::size_t i = 0; i < words.size(); ++i)
        Check(words[i] == Take::WORDS[i], std::string("word ") + std::to_string(i) + ": " + Take::WORDS[i]);
}

// WQ41: Take::May allows a quest only when it is in the game's answer. Quest 384 (level 7, minimum 5, needs cooking)
// for a level 6 dwarf without cooking: the game leaves it out of the answer (the live case, plan-take-skill, asks the
// same of quest 4161 and a night elf)
void GameAnswer()
{
    constexpr std::uint32_t SKILL_QUEST = 384;
    constexpr std::uint32_t OTHER = 385;
    constexpr std::uint8_t DWARF = 3;
    QuestRecord quest;
    quest.id = SKILL_QUEST;
    quest.level = 7;
    quest.minLevel = 5;
    quest.raceMask = SIDE_RACE_MASKS[0];
    Take::Facts const bot = BotOf(DWARF, 6);
    std::optional<Reason> const given = Take::May(quest, View(bot, Answered({SKILL_QUEST, OTHER})));
    std::optional<Reason> const left = Take::May(quest, View(bot, Answered({OTHER})));
    std::optional<Reason> const none = Take::May(quest, View(bot, Answered({})));
    Check(!given, "answer: in the game's answer, answer " + Answer(given));
    Check(left == Reason::Game, "answer: the skill quest left out of the game's answer, answer " + Answer(left));
    Check(none == Reason::Game, "answer: an empty answer, answer " + Answer(none));
}

// The circuit's skip for each reason: the words its log rows print
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
        {Reason::GivenUp, "refused"},
        {Reason::Impossible, "refused"},
        {Reason::Repeatable, "repeatable"},
        {Reason::Unsupported, "refused"},
        {Reason::Red, "red"},
        {Reason::Grey, "grey"},
        {Reason::Kind, "instance"},
        {Reason::EliteGroup, "elite/group solo"},
        {Reason::Game, "game"},
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
    if (roll(5))
        quest.impossible = "data";  // WQ38
    return quest;
}

// The plan's own rules as the plan text names them, written here apart from QuestTakeRules.h: whether one of them
// leaves the quest, whatever the game answers
// The game's grey level (Acore::XP::GetGrayLevel), written out again
std::int32_t GameGrey(std::uint8_t level)
{
    std::int32_t const l = level;
    return l <= 5 ? 0 : l <= 39 ? l - 5 - l / 10 : l <= 59 ? l - 1 - l / 5 : l - 9;
}

bool OwnRuleLeaves(QuestRecord const& quest, Take::Facts const& bot, Lists const& lists)
{
    std::int32_t const level = quest.level > 0 ? quest.level : static_cast<std::int32_t>(bot.level);
    std::uint8_t const side = SideOfRace(bot.race);
    return Take::Holds(lists.rewarded, quest.id) || Take::Holds(lists.inLog, quest.id) ||
           Take::Holds(lists.givenUp, quest.id) || !quest.impossible.empty() ||
           (quest.flags & (QuestFlag::DAILY | QuestFlag::WEEKLY | QuestFlag::REPEATABLE | QuestFlag::SEASONAL)) ||
           (side < SIDES && quest.refused[side]) || level >= static_cast<std::int32_t>(bot.level) + 5 ||
           (quest.level > 0 && quest.level <= GameGrey(bot.level)) ||
           (quest.flags & (QuestFlag::DUNGEON | QuestFlag::RAID | QuestFlag::PVP)) ||
           ((quest.flags & (QuestFlag::ELITE | QuestFlag::GROUP)) && !bot.eliteGroup);
}

// Generated bots and game answers against every generated quest. The answers are drawn apart from the quests' limits,
// so they hold quests a copy of the game's rule would refuse and leave out quests it would allow
// questing-2 QS.6: the colour of a quest for a bot of level 20 (the game's grey level there is 13) and its share
void Colours()
{
    struct Row
    {
        std::int32_t level;
        Take::Colour colour;
        float share;
    };
    Row const rows[] = {
        {30, Take::Colour::Red, 0.0f},    {25, Take::Colour::Red, 0.0f},    {24, Take::Colour::Orange, 0.3f},
        {23, Take::Colour::Orange, 0.3f}, {22, Take::Colour::Yellow, 1.0f}, {20, Take::Colour::Yellow, 1.0f},
        {18, Take::Colour::Yellow, 1.0f}, {17, Take::Colour::Green, 0.3f},  {14, Take::Colour::Green, 0.3f},
        {13, Take::Colour::Grey, 0.0f},   {1, Take::Colour::Grey, 0.0f},    {-1, Take::Colour::Yellow, 1.0f}};
    for (Row const& row : rows)
    {
        QuestRecord quest;
        quest.level = row.level;
        Take::Colour const colour = Take::ColourOf(quest, 20);
        Check(colour == row.colour, "colour of a level " + std::to_string(row.level) + " quest at 20");
        Check(Take::ColourShare(colour) == row.share, "share of a level " + std::to_string(row.level) + " quest");
        Check(Take::IsRed(quest, 20) == (row.colour == Take::Colour::Red), "red is the colour's");
    }
    QuestRecord high;
    high.level = 60;
    Check(Take::ColourOf(high, 0) == Take::Colour::Yellow, "no bot level: no colour is asked");
    // A level 3 bot in a start area: no grey line yet, a level 7 quest is orange, a level 8 quest red
    QuestRecord seven;
    seven.level = 7;
    QuestRecord eight;
    eight.level = 8;
    Check(Take::ColourOf(seven, 3) == Take::Colour::Orange && Take::ColourOf(eight, 3) == Take::Colour::Red,
          "level 3: a level 7 quest is orange, a level 8 quest red");
}

void GeneratedCases(std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::mt19937 answers(seed ^ 41u);  // the game's answers: their own stream, so the other draws stay as they were
    std::vector<QuestRecord> records;
    for (std::uint32_t id = 1; id <= QUESTS; ++id)
        records.push_back(Generated(rng, id));
    QuestIndex const index = IndexOf(records);

    std::array<std::uint32_t, static_cast<std::size_t>(Reason::Count)> counts{};
    std::uint32_t taken = 0;
    std::uint32_t cases = 0;
    std::uint32_t gameDecided = 0;  // taken although the quest's limits or previous quests would refuse it
    for (std::size_t b = 0; b < BOTS; ++b)
    {
        Lists lists;
        lists.rewarded = SomeQuests(rng, 10);
        lists.inLog = SomeQuests(rng, 8);
        lists.givenUp = SomeQuests(rng, 5);
        lists.takeable = SomeQuests(answers, 70);
        std::uint8_t const groupSize = static_cast<std::uint8_t>(rng() % 4);
        std::uint8_t const race = static_cast<std::uint8_t>(rng() % 13);  // 0, 9 and 12: no side
        std::uint8_t const cls = static_cast<std::uint8_t>(1 + rng() % 11);
        std::uint8_t const level = static_cast<std::uint8_t>(1 + rng() % 30);
        Take::Facts bot = BotOf(race, level);
        bot.eliteGroup = groupSize > 1;
        Take::Facts const facts = View(bot, lists);
        Lists const copy = lists;  // the same input in other storage
        Take::Facts const again = View(bot, copy);
        std::string const who = "seed " + std::to_string(seed) + " bot " + std::to_string(b);
        for (QuestRecord const& quest : index.quests)
        {
            ++cases;
            std::string const what = who + " quest " + std::to_string(quest.id);
            std::optional<Reason> const why = Take::May(quest, facts);
            Check(Take::May(quest, facts) == why && Take::May(quest, again) == why,
                  what + ": the same input, another answer");
            bool const logOrDone = Take::Holds(lists.rewarded, quest.id) || Take::Holds(lists.inLog, quest.id);
            Check(!logOrDone || (why && (*why == Reason::Done || *why == Reason::InLog)),
                  what + ": in the log or rewarded, answer " + Answer(why));
            bool const refused =
                RefusedFor(quest, SideOfRace(bot.race)) || Take::Holds(lists.givenUp, quest.id) || Listed(quest);
            Check(!refused || why.has_value(), what + ": refused, given up or listed, taken");

            // The game's answer decides: nothing out of it is taken, and what it holds is taken unless the plan's own
            // rules leave it (or it is a breadcrumb to an answered target)
            bool const answered = Take::Holds(lists.takeable, quest.id);
            Check(why.has_value() || answered, what + ": taken, not in the game's answer");
            bool const breadcrumb = quest.breadcrumbFor && Take::Holds(lists.takeable, quest.breadcrumbFor);
            bool const left = OwnRuleLeaves(quest, facts, lists);
            Check(!answered || left || breadcrumb || !why.has_value(),
                  what + ": in the game's answer and no own rule leaves it, answer " + Answer(why));
            Check(answered || left || why == Reason::Game,
                  what + ": out of the game's answer, answer " + Answer(why) + ", expected game");

            if (!why && !Eligible(quest, race, cls, level))
                ++gameDecided;
            if (why)
                ++counts[static_cast<std::size_t>(*why)];
            else
                ++taken;
        }
    }
    std::printf("seed=%u cases=%u taken=%u game_decided=%u", seed, cases, taken, gameDecided);
    for (std::size_t i = 0; i < counts.size(); ++i)
        std::printf(" %s=%u", Take::WORDS[i], counts[i]);
    std::printf("\n");
    Check(taken > 0, "generated: nothing taken");
    Check(gameDecided > 0, "generated: no quest the template's limits refuse taken on the game's answer");
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
    GameAnswer();
    Colours();
    GeneratedCases(seed);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
