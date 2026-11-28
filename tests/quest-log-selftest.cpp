/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the quest log's one accept and one drop (questing WQ31, WQ34, QuestLogRules.h): a table with one
// case per rule word, then generated logs, failure records and offers (the seed is printed; argv[1] replays it)
// checked against the invariants: no quest is accepted and abandoned by the same log state; the rules applied twice
// change nothing more; a ready quest is never abandoned; nothing is refused or dropped for room; the plan is due
// exactly when the round's givers offer nothing more or its time has run out. Questing WQ34: the one accept and the
// one drop against the calls' spec (namespace Wq34: invariants A, I1-I3), and the rules before WQ34 as mutants the
// invariants must catch.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel tests/quest-log-selftest.cpp

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "QuestLogRules.h"
#include "QuestTakeFixture.h"

using namespace Planner;
using QuestLog::Drop;

namespace
{
constexpr std::uint32_t DEFAULT_SEED = 20261125;
constexpr std::size_t LOGS = 600;
constexpr std::size_t ROUNDS = 2000;
constexpr std::uint32_t QUESTS = 120;  // ids 1 to QUESTS; offers may name QUESTS + 1, which is missing
constexpr std::uint8_t HUMAN = 1;      // Alliance
constexpr std::uint8_t WARRIOR = 1;
constexpr std::uint32_t PRINTED_FAILURES = 20;
constexpr std::array<char const*, 4> CAUSES = {"cannot_take", "no_ender", "timeout", "unsupported:item"};

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

using Rows = std::vector<std::pair<std::uint32_t, QuestFail::Row>>;
using Ids = std::vector<std::uint32_t>;

QuestFail::Row RowOf(std::uint32_t giveUps, std::string const& cause)
{
    QuestFail::Row row;
    row.giveUps = giveUps;
    row.lastCause = cause;
    return row;
}

bool Has(Ids const& sorted, std::uint32_t id) { return std::binary_search(sorted.begin(), sorted.end(), id); }

void Insert(Ids& sorted, std::uint32_t id)
{
    if (!Has(sorted, id))
        sorted.insert(std::lower_bound(sorted.begin(), sorted.end(), id), id);
}

void Erase(Ids& sorted, std::uint32_t id)
{
    auto const at = std::lower_bound(sorted.begin(), sorted.end(), id);
    if (at != sorted.end() && *at == id)
        sorted.erase(at);
}

QuestIndex IndexOf(std::vector<QuestRecord> quests)
{
    std::sort(quests.begin(), quests.end(), [](QuestRecord const& a, QuestRecord const& b) { return a.id < b.id; });
    QuestIndex index;
    index.quests = std::move(quests);
    return index;
}

QuestRecord Plain(std::uint32_t id, std::int32_t level = 20)
{
    QuestRecord quest;
    quest.id = id;
    quest.level = level;
    quest.minLevel = static_cast<std::uint32_t>(std::max(1, level - 5));
    return quest;
}

// The game's grey line: a copy of Acore::XP::GetGrayLevel (Formulas.h), which the live callers read
std::int32_t GrayLevel(std::uint8_t level)
{
    if (level <= 5)
        return 0;
    if (level <= 39)
        return level - 5 - level / 10;
    if (level <= 59)
        return level - 1 - level / 5;
    return level - 9;
}

// A bot's log state: what the game's answer, the accept and Abandon read
struct State
{
    std::uint8_t race{HUMAN};
    std::uint8_t cls{WARRIOR};
    std::uint8_t level{20};
    Ids rewarded;  // sorted
    Ids inLog;     // sorted
    Ids ready;     // sorted, a part of inLog
    QuestLog::Record record;
};

// The game's answer for the state (WQ41: a fact of the take test), as the fixtures model it
Ids AnswerOf(State const& state, QuestIndex const& index)
{
    return TakeFixture::Answer(index, state.race, state.cls, state.level, state.rewarded, state.inLog);
}

QuestLog::Facts FactsOf(State const& state)
{
    return {.grayLevel = GrayLevel(state.level), .givenUp = state.record.givenUp};
}

// A client's log: the generated logs reach past it, and the rules know no room (questing WQ40)
constexpr std::size_t CLIENT_SLOTS = 25;

bool MayAcceptNow(QuestRecord const& quest, State const& state, QuestIndex const& index)
{
    Ids const answer = AnswerOf(state, index);
    return QuestLog::Accepts(QuestLog::OfferOf(quest, Has(answer, quest.id), false), FactsOf(state));
}

QuestLog::Held HeldOf(QuestRecord const& quest, bool ready, bool failed = false, bool holidayEnded = false)
{
    return {.id = quest.id, .level = quest.level, .ready = ready, .failed = failed, .holidayEnded = holidayEnded};
}

Drop AbandonNow(QuestRecord const& quest, State const& state, bool ready)
{
    return QuestLog::Abandon(HeldOf(quest, ready), GrayLevel(state.level));
}

struct Outcome
{
    Ids dropped;
    std::vector<Drop> why;
    Ids accepted;
    bool endless{false};

    bool operator==(Outcome const& other) const
    {
        return dropped == other.dropped && why == other.why && accepted == other.accepted && endless == other.endless;
    }
};

// One application of the rules: the upkeep's drops by Abandon, then the accepts by Accepts over the offers until
// none passes (the executor's search runs every tick while a giver offers a quest)
Outcome Apply(State& state, Ids const& offers, QuestIndex const& index)
{
    Outcome outcome;
    for (std::uint32_t const id : Ids(state.inLog))
    {
        QuestRecord const* quest = index.Find(id);
        if (!quest)
            continue;
        Drop const drop = AbandonNow(*quest, state, Has(state.ready, id));
        if (drop == Drop::Keep)
            continue;
        outcome.dropped.push_back(id);
        outcome.why.push_back(drop);
        Erase(state.inLog, id);
        Erase(state.ready, id);
    }
    bool more = true;
    for (std::size_t pass = 0; more && pass <= offers.size(); ++pass)
    {
        more = false;
        for (std::uint32_t const id : offers)
        {
            QuestRecord const* quest = index.Find(id);
            if (!quest || !MayAcceptNow(*quest, state, index))
                continue;
            outcome.accepted.push_back(id);
            Insert(state.inLog, id);
            more = true;
        }
    }
    outcome.endless = more;
    return outcome;
}

// One case per rule word, the order of the rules and their edges
void Table()
{
    constexpr std::uint32_t PLAIN = 1;
    constexpr std::uint32_t GREY = 2;     // level 13 for a level-20 bot: the game's grey level is 13
    constexpr std::uint32_t REFUSED = 3;  // the capability table refuses it for the Alliance
    constexpr std::uint32_t ELITE = 4;
    constexpr std::uint32_t TAKE_REFUSED = 5;  // a planned take ended without the accept once (WQ18c)
    constexpr std::uint32_t ONCE = 6;          // given up once (timeout)
    constexpr std::uint32_t TWICE = 7;         // given up twice, the last a refused take overwritten by a timeout
    constexpr std::uint32_t NO_ENDER = 8;      // its hand-in failed at its ender
    constexpr std::uint32_t EDGE = 9;          // level 14: one above the grey level of a level-20 bot
    constexpr std::uint32_t TAKE_TWICE = 10;   // a planned take ended without the accept twice
    constexpr std::uint32_t DUNGEON = 11;
    constexpr std::uint32_t DAILY = 12;
    constexpr std::uint32_t REPEATABLE = 13;
    constexpr std::uint32_t HOLIDAY = 14;  // a holiday's quest (seasonal), its holiday running or ended
    constexpr std::uint32_t HIGH = 15;     // 6 levels above the bot, the game lets it take it

    QuestRecord refused = Plain(REFUSED);
    refused.refused[0] = 1;
    QuestRecord elite = Plain(ELITE);
    elite.flags = QuestFlag::ELITE | QuestFlag::GROUP;
    QuestRecord dungeon = Plain(DUNGEON);
    dungeon.flags = QuestFlag::DUNGEON;
    QuestRecord daily = Plain(DAILY);
    daily.flags = QuestFlag::DAILY;
    QuestRecord repeatable = Plain(REPEATABLE);
    repeatable.flags = QuestFlag::REPEATABLE | QuestFlag::WEEKLY;
    QuestRecord holiday = Plain(HOLIDAY);
    holiday.flags = QuestFlag::SEASONAL;
    QuestRecord high = Plain(HIGH, 26);
    high.minLevel = 18;
    QuestIndex const index =
        IndexOf({Plain(PLAIN), Plain(GREY, 13), refused, elite, Plain(TAKE_REFUSED), Plain(ONCE), Plain(TWICE),
                 Plain(NO_ENDER), Plain(EDGE, 14), Plain(TAKE_TWICE), dungeon, daily, repeatable, holiday, high});

    Rows const rows = {{NO_ENDER, RowOf(1, "no_ender")},
                       {TWICE, RowOf(2, "timeout")},
                       {ONCE, RowOf(1, "timeout")},
                       {TAKE_REFUSED, RowOf(1, "cannot_take")},
                       {TAKE_TWICE, RowOf(2, "cannot_take")}};
    QuestLog::Record const record = QuestLog::Read(rows);
    Check(record.givenUp == Ids({TWICE, NO_ENDER, TAKE_TWICE}),
          "read: given up are twice (a refused take twice too) and no ender; a take refused once is not (WQ.44 item "
          "2; mutant: the refused take given up at once)");
    Check(record.handInFailed == Ids({NO_ENDER}), "read: the failed hand-in is no ender's");
    Check(QuestLog::Read({}).givenUp.empty() && QuestLog::Read({}).handInFailed.empty(), "read: no rows, nothing");

    State bot;
    bot.record = record;
    auto const may = [&](std::uint32_t id) { return MayAcceptNow(*index.Find(id), bot, index); };
    Check(may(PLAIN), "accept: a plain quest");
    // No rule for room (questing WQ40, WQ.46 item 1): a log of 25, 30 or 60 quests accepts as an empty one
    for (std::uint32_t const held : {25u, 30u, 60u})
    {
        State crowded = bot;
        for (std::uint32_t id = 0; id < held; ++id)
            crowded.inLog.push_back(5000 + id);
        Check(MayAcceptNow(*index.Find(PLAIN), crowded, index),
              "accept: a plain quest into a log of " + std::to_string(held) + " quests");
    }
    auto const refusal = [&](std::uint32_t id, bool holidayEnded = false)
    {
        Ids const answer = AnswerOf(bot, index);
        QuestRecord const& quest = *index.Find(id);
        return QuestLog::Word(QuestLog::Refuses(QuestLog::OfferOf(quest, Has(answer, id), holidayEnded), FactsOf(bot)));
    };
    auto const is = [](char const* got, char const* want) { return std::string(got) == want; };
    Check(is(refusal(GREY), "grey"), "accept: grey by the game's line (13 for a level-20 bot)");
    Check(may(EDGE), "accept: one level above the game's grey line is not grey (the 4-level setting is gone)");
    Check(may(REFUSED), "accept: a quest the executor cannot do is taken (WQ.46 item 2)");
    Check(may(ELITE), "accept: an elite group quest is taken");
    Check(may(DUNGEON), "accept: a dungeon quest is taken");
    Check(may(HIGH), "accept: a quest the game lets it take is taken however high its level");
    Check(is(refusal(DAILY), "repeats") && is(refusal(REPEATABLE), "repeats"),
          "accept: no daily, weekly or repeatable quest (WQ.46 item 5)");
    Check(may(HOLIDAY), "accept: a holiday's quest while its holiday runs");
    Check(is(refusal(HOLIDAY, true), "holiday_ended"), "accept: a holiday's quest after its holiday");
    Check(may(TAKE_REFUSED), "accept: a take refused once is taken again, as any cause given up once");
    Check(!may(TAKE_TWICE) && is(refusal(TAKE_TWICE), "given_up"), "accept: a take refused twice is given up");
    Check(may(ONCE), "accept: given up once by a timeout");
    Check(!may(TWICE), "accept: given up twice");
    Check(!may(NO_ENDER), "accept: its hand-in failed");
    bot.inLog = {PLAIN};
    Check(!may(PLAIN) && is(refusal(PLAIN), "game"), "accept: in the log (the game's answer)");
    bot.inLog.clear();
    State low = bot;
    low.level = 5;
    Check(MayAcceptNow(Plain(PLAIN, 1), low, IndexOf({Plain(PLAIN, 1)})), "accept: at level 5 nothing is grey");

    // The drop: grey, holiday ended or failed, never a complete quest; a give-up, a quest the executor cannot do and
    // a failed hand-in are no drop (WQ.46 item 4)
    auto const drop = [](std::int32_t level, bool ready, bool failed, bool holidayEnded, std::uint8_t botLevel = 20)
    {
        QuestLog::Held const held{
            .id = 1, .level = level, .ready = ready, .failed = failed, .holidayEnded = holidayEnded};
        return QuestLog::Word(QuestLog::Abandon(held, GrayLevel(botLevel)));
    };
    Check(is(drop(20, false, false, false), "keep"), "abandon: a plain quest is kept");
    Check(is(drop(13, false, false, false), "grey"), "abandon: grey by the game's line");
    Check(is(drop(14, false, false, false), "keep"), "abandon: one above the game's grey line is kept");
    Check(is(drop(1, false, false, false, 5), "keep"), "abandon: at level 5 nothing is grey");
    Check(is(drop(51, false, false, false, 60), "grey") && is(drop(52, false, false, false, 60), "keep"),
          "abandon: the grey line of level 60 is 51");
    Check(is(drop(0, false, false, false), "keep"), "abandon: the bot's level");
    Check(is(drop(-1, false, false, false), "keep"), "abandon: the bot's level (-1)");
    Check(is(drop(20, false, true, false), "failed"), "abandon: the game marks it failed");
    Check(is(drop(20, false, false, true), "holiday_ended"), "abandon: its holiday has ended");
    Check(is(drop(13, true, false, false), "keep"), "abandon: a ready grey quest is kept, to be handed in");
    Check(is(drop(20, true, false, true), "keep"), "abandon: a ready quest after its holiday is kept");
    Check(is(drop(13, false, true, true), "grey"), "abandon: grey before holiday and failed");
    Check(is(drop(20, false, true, true), "holiday_ended"), "abandon: holiday before failed");
    Check(QuestLog::Abandon(HeldOf(*index.Find(TWICE), false), GrayLevel(20)) == Drop::Keep &&
              QuestLog::Abandon(HeldOf(*index.Find(NO_ENDER), true), GrayLevel(20)) == Drop::Keep &&
              QuestLog::Abandon(HeldOf(refused, false), GrayLevel(20)) == Drop::Keep,
          "abandon: given up, a failed hand-in and cannot do are kept");

    QuestLog::Round round;
    Check(!QuestLog::PlanDue(round, 5000, false), "due: no accept, no plan");
    QuestLog::NoteAccept(round, 1000);
    Check(round.open && round.firstAcceptMs == 1000, "due: the round opens at the first accept");
    Check(!QuestLog::PlanDue(round, 5000, true), "due: a giver still offers, 4 s");
    Check(QuestLog::PlanDue(round, 5000, false), "due: no giver offers more");
    Check(!QuestLog::PlanDue(round, 1000 + QuestLog::PLAN_DUE_MS - 1, true), "due: just before the time");
    Check(QuestLog::PlanDue(round, 1000 + QuestLog::PLAN_DUE_MS, true), "due: the time has run out");
    QuestLog::NoteAccept(round, 20000);
    Check(round.firstAcceptMs == 1000 && QuestLog::PlanDue(round, 31000, true), "due: a later accept keeps the first");
    QuestLog::Round wrap;
    QuestLog::NoteAccept(wrap, 0xFFFFF000u);
    Check(!QuestLog::PlanDue(wrap, 0x00001000u, true), "due: across the clock's wrap, 8 s");
    Check(QuestLog::PlanDue(wrap, 0xFFFFF000u + QuestLog::PLAN_DUE_MS, true), "due: across the clock's wrap, 30 s");
}

Ids SomeQuests(std::mt19937& rng, std::uint32_t percent)
{
    Ids ids;
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
        if (roll(3))
            quest.flags |= flag;
    if (roll(15))
        quest.prev = static_cast<std::int32_t>(pick(1, QUESTS)) * (roll(40) ? -1 : 1);
    if (roll(15))
        quest.exclusiveGroup = static_cast<std::int32_t>(pick(1, 6));
    if (roll(10))
        quest.breadcrumbFor = pick(1, QUESTS + 1);
    quest.refused[0] = static_cast<std::uint8_t>(roll(8));
    quest.refused[1] = static_cast<std::uint8_t>(roll(8));
    return quest;
}

State GeneratedState(std::mt19937& rng)
{
    State state;
    state.race = static_cast<std::uint8_t>(1 + rng() % 11);  // 9: no side
    state.cls = static_cast<std::uint8_t>(1 + rng() % 11);
    state.level = static_cast<std::uint8_t>(1 + rng() % 30);
    state.rewarded = SomeQuests(rng, 10);
    std::uint32_t const shape = rng() % 10;
    std::size_t const size = shape < 3 ? CLIENT_SLOTS + rng() % 36 : shape == 3 ? 23 + rng() % 2 : rng() % 12;
    Ids open;
    for (std::uint32_t id = 1; id <= QUESTS; ++id)
        if (!Has(state.rewarded, id))
            open.push_back(id);
    std::shuffle(open.begin(), open.end(), rng);
    open.resize(std::min(size, open.size()));
    std::sort(open.begin(), open.end());
    state.inLog = open;
    for (std::uint32_t const id : state.inLog)
        if (rng() % 100 < 30)
            state.ready.push_back(id);
    Rows rows;
    for (std::uint32_t id = 1; id <= QUESTS; ++id)
        if (rng() % 100 < 12)
            rows.emplace_back(id, RowOf(1 + rng() % 3, CAUSES[rng() % CAUSES.size()]));
    std::shuffle(rows.begin(), rows.end(), rng);
    state.record = QuestLog::Read(rows);

    std::string const where = "read";
    Check(std::is_sorted(state.record.givenUp.begin(), state.record.givenUp.end()) &&
              std::is_sorted(state.record.handInFailed.begin(), state.record.handInFailed.end()),
          where + ": lists sorted");
    for (auto const& [id, row] : rows)
    {
        bool const noEnder = row.lastCause == "no_ender";
        Check(Has(state.record.givenUp, id) == (row.giveUps >= 2 || noEnder),
              where + ": quest " + std::to_string(id) + " " + row.lastCause + " x" + std::to_string(row.giveUps));
        Check(Has(state.record.handInFailed, id) == noEnder, where + ": failed hand-in of " + std::to_string(id));
    }
    return state;
}

struct Counts
{
    std::uint32_t logs{0};
    std::uint32_t full{0};
    std::uint32_t accepted{0};
    std::uint32_t fullAccepts{0};
    std::array<std::uint32_t, static_cast<std::size_t>(Drop::Count)> drops{};
};

// Whether the drop's reason holds for the quest in the state before the drop
bool Justified(Drop drop, QuestRecord const& quest, State const& state)
{
    bool const ready = Has(state.ready, quest.id);
    switch (drop)
    {
        case Drop::Grey:
            return !ready && QuestLog::IsGrey(quest.level, GrayLevel(state.level));
        default:  // the generated states hold no failed quest and no holiday (Wq34's logs do)
            return false;
    }
}

void OneLog(std::mt19937& rng, QuestIndex const& index, std::string const& who, Counts& counts)
{
    State const before = GeneratedState(rng);
    Ids offers;
    for (std::uint32_t n = rng() % 11; n > 0; --n)
        offers.push_back(1 + rng() % (QUESTS + 1));

    State after = before;
    Outcome const outcome = Apply(after, offers, index);
    State again = before;
    Check(Apply(again, offers, index) == outcome, who + ": the same input, another outcome");
    Check(!outcome.endless, who + ": accepts without end");

    Ids accepted = outcome.accepted;
    std::sort(accepted.begin(), accepted.end());
    Check(std::adjacent_find(accepted.begin(), accepted.end()) == accepted.end(), who + ": a quest accepted twice");
    for (std::uint32_t const id : outcome.dropped)
        Check(!Has(accepted, id), who + ": quest " + std::to_string(id) + " abandoned and accepted");
    for (std::uint32_t const id : outcome.accepted)
        Check(AbandonNow(*index.Find(id), after, false) == Drop::Keep,
              who + ": quest " + std::to_string(id) + " accepted, then abandoned");

    // The rules applied twice change nothing more
    State twice = after;
    Outcome const second = Apply(twice, offers, index);
    Check(second.dropped.empty() && second.accepted.empty(), who + ": a second application dropped " +
                                                                 std::to_string(second.dropped.size()) + ", accepted " +
                                                                 std::to_string(second.accepted.size()));

    // Every drop has its reason; a quest without one is kept, ready ones always
    for (std::size_t i = 0; i < outcome.dropped.size(); ++i)
    {
        Check(Justified(outcome.why[i], *index.Find(outcome.dropped[i]), before),
              who + ": quest " + std::to_string(outcome.dropped[i]) + " dropped as " + QuestLog::Word(outcome.why[i]));
        ++counts.drops[static_cast<std::size_t>(outcome.why[i])];
    }
    for (std::uint32_t const id : before.inLog)
    {
        QuestRecord const& quest = *index.Find(id);
        bool const ready = Has(before.ready, id);
        bool const reason = !ready && QuestLog::IsGrey(quest.level, GrayLevel(before.level));
        bool const dropped = std::find(outcome.dropped.begin(), outcome.dropped.end(), id) != outcome.dropped.end();
        Check(dropped == reason,
              who + ": quest " + std::to_string(id) + (ready ? " (ready)" : "") + (dropped ? " dropped" : " kept"));
        if (!dropped)
            ++counts.drops[static_cast<std::size_t>(Drop::Keep)];
    }

    // No rule for room (questing WQ40): `full` counts the logs of a client's 25 quests or more, which accept as any
    // other (Wq34's invariant A checks the accept itself)
    if (before.inLog.size() >= CLIENT_SLOTS)
    {
        ++counts.full;
        if (!outcome.accepted.empty())
            ++counts.fullAccepts;
    }
    ++counts.logs;
    counts.accepted += static_cast<std::uint32_t>(outcome.accepted.size());
}

// The plan's due over generated rounds: due exactly when an accept opened the round and the givers offer nothing
// more or the time has run out, across the clock's wrap; a later accept keeps the first stamp
std::uint32_t GeneratedRounds(std::mt19937& rng, std::string const& who)
{
    std::uint32_t due = 0;
    for (std::size_t r = 0; r < ROUNDS; ++r)
    {
        std::uint32_t const first = static_cast<std::uint32_t>(rng());
        std::uint32_t const elapsed = rng() % (2 * QuestLog::PLAN_DUE_MS);
        bool const offers = rng() % 2 == 0;
        QuestLog::Round round;
        std::string const what = who + " round " + std::to_string(r);
        Check(!QuestLog::PlanDue(round, first + elapsed, offers), what + ": due without an accept");
        QuestLog::NoteAccept(round, first);
        if (rng() % 2 == 0)
            QuestLog::NoteAccept(round, first + elapsed / 2);
        bool const want = !offers || elapsed >= QuestLog::PLAN_DUE_MS;
        bool const got = QuestLog::PlanDue(round, first + elapsed, offers);
        Check(got == want, what + ": elapsed " + std::to_string(elapsed) + (offers ? ", offers" : ", none"));
        due += got ? 1 : 0;
    }
    return due;
}

void GeneratedCases(std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<QuestRecord> records;
    for (std::uint32_t id = 1; id <= QUESTS; ++id)
        records.push_back(Generated(rng, id));
    QuestIndex const index = IndexOf(records);

    Counts counts;
    for (std::size_t l = 0; l < LOGS; ++l)
        OneLog(rng, index, "seed " + std::to_string(seed) + " log " + std::to_string(l), counts);
    std::uint32_t const due = GeneratedRounds(rng, "seed " + std::to_string(seed));

    std::printf("seed=%u logs=%u full=%u accepted=%u full_accepts=%u", seed, counts.logs, counts.full, counts.accepted,
                counts.fullAccepts);
    for (std::size_t i = 0; i < counts.drops.size(); ++i)
        std::printf(" %s=%u", QuestLog::WORDS[i], counts.drops[i]);
    std::printf(" rounds=%zu due=%u\n", ROUNDS, due);
    Check(counts.full > 0 && counts.accepted > 0 && counts.fullAccepts > 0, "generated: no full log, accept or both");
    for (Drop const drop : {Drop::Keep, Drop::Grey})
        Check(counts.drops[static_cast<std::size_t>(drop)] > 0,
              std::string("generated: no case answered ") + QuestLog::Word(drop));
    Check(due > 0 && due < ROUNDS, "generated: rounds all due or none");
}

// Questing WQ34 (written first and seen red on the rules before it, WQ34a): the one accept and the one drop of the log
// as the coordinator's calls fix them (scratchpad/questing/WQ34-record.md, calls 1-6). The spec below is the oracle;
// the paths are what each way into the log asks (every one QuestLogRules' Accepts since WQ34b) and LogDrop is the drop
// a bot's log gets (Abandon). The rules before WQ34 stay here as mutants (Old): each must break an invariant.
//   A   every path accepts exactly the one accept: the game's answer, not grey by the game's line
//       (Acore::XP::GetGrayLevel), not repeatable, daily or weekly, not of an ended holiday, not given up
//   I1  a quest any path accepted is not dropped in the same state (a start item that is also the required item
//       makes the quest complete at once: 8471, 9594)
//   I2  a quest leaves the log exactly when it is not complete and grey by the game's line, its holiday has ended or
//       the game marks it failed
//   I3  a complete quest is never dropped
namespace Wq34
{
enum class Path : std::uint8_t
{
    OrderGiver,  // a giver met on an order (NewRpgQuest.cpp InteractWithNpcOrGameObjectForQuest)
    FreeGiver,   // a giver met by a planner bot off an order (the same, NewRpgHelpers::LogAccept since WQ34b)
    Item,        // a start item's use (UseItemAction.cpp UseStartQuestItem through QuestAction::CanAcceptQuest)
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Path::Count)> PATHS = {"order_giver", "free_giver", "item"};
constexpr std::array<char const*, 4> RULES = {"A", "I1", "I2", "I3"};
constexpr std::uint16_t NO_REPEAT = QuestFlag::DAILY | QuestFlag::WEEKLY | QuestFlag::REPEATABLE;

// The log as the drop reads it: the generated state, the quests the game marks failed and the quests whose holiday
// ended (any quest: an offered one too)
struct Log
{
    State state;
    Ids failed;        // sorted, open quests of inLog
    Ids holidayEnded;  // sorted
};

bool GameGrey(QuestRecord const& quest, std::uint8_t level)
{
    return quest.level > 0 && quest.level <= GrayLevel(level);
}

bool SpecAccept(QuestRecord const& quest, Log const& log, Ids const& answer)
{
    return Has(answer, quest.id) && !GameGrey(quest, log.state.level) && !(quest.flags & NO_REPEAT) &&
           !Has(log.holidayEnded, quest.id) && !Has(log.state.record.givenUp, quest.id);
}

bool SpecDrop(QuestRecord const& quest, Log const& log, bool ready)
{
    return !ready && (GameGrey(quest, log.state.level) || Has(log.holidayEnded, quest.id) || Has(log.failed, quest.id));
}

// What each path asks: the one accept, whatever the path (WQ34b)
bool PathAccepts(Path /*path*/, QuestRecord const& quest, Log const& log, Ids const& answer)
{
    return QuestLog::Accepts(QuestLog::OfferOf(quest, Has(answer, quest.id), Has(log.holidayEnded, quest.id)),
                             FactsOf(log.state));
}

// The drop of a bot's log: the one drop
bool LogDrops(QuestRecord const& quest, Log const& log, bool ready)
{
    return QuestLog::Abandon(HeldOf(quest, ready, Has(log.failed, quest.id), Has(log.holidayEnded, quest.id)),
                             GrayLevel(log.state.level)) != Drop::Keep;
}

// The rules before WQ34, put back as mutants: the invariants must catch each (the step's exit)
namespace Old
{
constexpr std::int32_t ITEM_BAND = 10;         // QuestAction::GREY_QUEST_LEVEL_BAND
constexpr std::int32_t FREE_LEVELS_ABOVE = 3;  // upstream's QUEST_MAX_LEVELS_ABOVE_BOT (IsQuestCapableDoing)

bool Grey4(QuestRecord const& quest, State const& state)
{
    return static_cast<std::int32_t>(state.level) >
           Take::QuestLevel(quest, state.level) + 4;  // the old Take::GREY_DIFF
}

// OrderTake::MayAccept (the take test), FreeBotTakes (upstream's filter, no failure record) and CanAcceptQuest (the
// game and a 10-level band)
bool PathAccepts(Path path, QuestRecord const& quest, Log const& log, Ids const& answer)
{
    State const& state = log.state;
    bool const game = Has(answer, quest.id);
    std::int32_t const level = Take::QuestLevel(quest, state.level);
    switch (path)
    {
        case Path::OrderGiver:
        {
            Take::Facts facts;
            facts.race = state.race;
            facts.level = state.level;
            facts.rewarded = state.rewarded;
            facts.inLog = state.inLog;
            facts.givenUp = state.record.givenUp;
            facts.takeable = answer;
            return !Take::May(quest, facts).has_value();
        }
        case Path::FreeGiver:
            return game && !Grey4(quest, state) && !(quest.flags & Take::REPEAT_FLAGS) &&
                   !(quest.flags & (Take::GROUP_FLAGS | Take::INSTANCE_FLAGS)) &&
                   level <= static_cast<std::int32_t>(state.level) + FREE_LEVELS_ABOVE &&
                   !RefusedFor(quest, SideOfRace(state.race));
        case Path::Item:
            return game && static_cast<std::int32_t>(state.level) <= level + ITEM_BAND;
        default:
            return false;
    }
}

// OrganizeOrderQuestLog's Abandon: a ready quest only for a failed hand-in; else grey at 4, given up, cannot do (a
// failed quest read as one it cannot do); no holiday
bool LogDrops(QuestRecord const& quest, Log const& log, bool ready)
{
    State const& state = log.state;
    if (ready)
        return Has(state.record.handInFailed, quest.id);
    return Grey4(quest, state) || Has(state.record.givenUp, quest.id) || RefusedFor(quest, SideOfRace(state.race)) ||
           Has(log.failed, quest.id);
}
}  // namespace Old

// One set of rules the invariants judge; `gate` false: a mutant, counted and not reported
struct Rules
{
    char const* name;
    bool (*accepts)(Path, QuestRecord const&, Log const&, Ids const&);
    bool (*drops)(QuestRecord const&, Log const&, bool);
    bool gate;
};
constexpr Rules ONE = {"one", &PathAccepts, &LogDrops, true};
constexpr Rules OLD_ACCEPT = {"old_accept", &Old::PathAccepts, &LogDrops, false};
constexpr Rules OLD_DROP = {"old_drop", &PathAccepts, &Old::LogDrops, false};

struct Counts
{
    std::uint32_t logs{0};
    std::uint32_t accepts{0};
    std::uint32_t drops{0};
    std::array<std::uint32_t, RULES.size()> broken{};
    std::array<std::uint32_t, static_cast<std::size_t>(Drop::Count)> words{};
};

void Rule(Counts& counts, Rules const& rules, std::size_t rule, bool ok, std::string const& what)
{
    if (rules.gate)
        Check(ok, std::string(RULES[rule]) + " " + what);
    if (!ok)
        ++counts.broken[rule];
}

std::string Facts(QuestRecord const& quest, Log const& log, bool ready)
{
    State const& state = log.state;
    return "quest " + std::to_string(quest.id) + " L" + std::to_string(quest.level) + " flags " +
           std::to_string(quest.flags) + " bot L" + std::to_string(state.level) + (ready ? " ready" : "") +
           (GameGrey(quest, state.level) ? " grey" : "") + (Has(log.failed, quest.id) ? " failed" : "") +
           (Has(log.holidayEnded, quest.id) ? " holiday_ended" : "") +
           (Has(state.record.givenUp, quest.id) ? " given_up" : "") +
           (Has(state.record.handInFailed, quest.id) ? " no_ender" : "");
}

// I2 and I3 over the log as it is, then A and I1 for every quest by every path
void OneLog(Log const& log, QuestIndex const& index, std::mt19937& rng, std::string const& who, Rules const& rules,
            Counts& counts)
{
    State const& state = log.state;
    for (std::uint32_t const id : state.inLog)
    {
        QuestRecord const& quest = *index.Find(id);
        bool const ready = Has(state.ready, id);
        bool const dropped = rules.drops(quest, log, ready);
        counts.drops += dropped ? 1 : 0;
        if (rules.gate)
        {
            Drop const drop = QuestLog::Abandon(HeldOf(quest, ready, Has(log.failed, id), Has(log.holidayEnded, id)),
                                                GrayLevel(state.level));
            ++counts.words[static_cast<std::size_t>(drop)];
        }
        std::string const what = who + ": " + Facts(quest, log, ready) + (dropped ? " dropped" : " kept");
        if (ready)
            Rule(counts, rules, 3, !dropped, what);
        else
            Rule(counts, rules, 2, dropped == SpecDrop(quest, log, ready), what);
    }
    Ids const answer = AnswerOf(state, index);
    for (QuestRecord const& quest : index.quests)
        for (std::size_t p = 0; p < PATHS.size(); ++p)
        {
            Path const path = static_cast<Path>(p);
            bool const accepts = rules.accepts(path, quest, log, answer);
            Rule(counts, rules, 0, accepts == SpecAccept(quest, log, answer),
                 who + " " + PATHS[p] + ": " + Facts(quest, log, false) + (accepts ? " accepted" : " refused"));
            if (!accepts)
                continue;
            ++counts.accepts;
            bool const ready = path == Path::Item && rng() % 2 == 0;
            Log after = log;
            Insert(after.state.inLog, quest.id);
            if (ready)
                Insert(after.state.ready, quest.id);
            Rule(counts, rules, 1, !rules.drops(quest, after, ready),
                 who + " " + PATHS[p] + ": " + Facts(quest, after, ready) + " accepted, then dropped");
        }
}

// The night's three loops (lab5-20261125-175728) as cases; WQ34a's traces name the paths and the rules
Counts Loops(Rules const& rules)
{
    QuestIndex const index = IndexOf({Plain(9594, 14), Plain(8471, 56), Plain(8304, 60)});
    Counts counts;
    auto const bot = [](std::uint8_t level, Rows const& rows)
    {
        Log log;
        log.state.level = level;
        log.state.record = QuestLog::Read(rows);
        return log;
    };
    auto const accepted = [&index, &rules](Log log, std::uint32_t id, bool ready)
    {
        Insert(log.state.inLog, id);
        if (ready)
            Insert(log.state.ready, id);
        return !rules.drops(*index.Find(id), log, ready);
    };
    // 9594: a level 20 bot uses the start item (also the required one); grey at 13 by the game's line, so it is kept
    Log const plate = bot(20, {});
    Ids const plateAnswer = AnswerOf(plate.state, index);
    Rule(counts, rules, 0, rules.accepts(Path::Item, *index.Find(9594), plate, plateAnswer),
         "loop 9594: item path refuses a quest the game's line shows not grey");
    Rule(counts, rules, 1, accepted(plate, 9594, false), "loop 9594: accepted by its item, then dropped");
    Rule(counts, rules, 3, accepted(plate, 9594, true), "loop 9594: complete at once, then dropped");
    // 8471: its hand-in failed once (no_ender: given up); the item path takes it again, complete at once
    Log const totem = bot(56, {{8471, RowOf(1, "no_ender")}});
    Ids const totemAnswer = AnswerOf(totem.state, index);
    Rule(counts, rules, 0, !rules.accepts(Path::Item, *index.Find(8471), totem, totemAnswer),
         "loop 8471: item path accepts a given-up quest");
    Rule(counts, rules, 3, accepted(totem, 8471, true), "loop 8471: complete quest dropped after a failed hand-in");
    // 8304: given up twice; a planner bot off an order meets its giver, the next order drops it
    Log const letter = bot(60, {{8304, RowOf(2, "no_target")}});
    Ids const letterAnswer = AnswerOf(letter.state, index);
    Rule(counts, rules, 0, !rules.accepts(Path::FreeGiver, *index.Find(8304), letter, letterAnswer),
         "loop 8304: free giver path accepts a given-up quest");
    Rule(counts, rules, 2, accepted(letter, 8304, false), "loop 8304: an open quest dropped for a give-up");
    std::printf("wq34 loops %s: A=%u I1=%u I2=%u I3=%u broken\n", rules.name, counts.broken[0], counts.broken[1],
                counts.broken[2], counts.broken[3]);
    return counts;
}

Counts GeneratedLogs(std::uint32_t seed, Rules const& rules)
{
    std::mt19937 rng(seed + 34);  // apart from GeneratedCases' draws, so its counts stay as they were
    std::vector<QuestRecord> records;
    for (std::uint32_t id = 1; id <= QUESTS; ++id)
        records.push_back(Generated(rng, id));
    QuestIndex const index = IndexOf(records);
    Counts counts;
    for (std::size_t l = 0; l < LOGS; ++l)
    {
        Log log;
        log.state = GeneratedState(rng);
        for (std::uint32_t const id : log.state.inLog)
            if (!Has(log.state.ready, id) && rng() % 100 < 10)
                log.failed.push_back(id);
        for (std::uint32_t id = 1; id <= QUESTS; ++id)
            if (rng() % 100 < 8)
                log.holidayEnded.push_back(id);
        OneLog(log, index, rng, "wq34 seed " + std::to_string(seed) + " log " + std::to_string(l), rules, counts);
        ++counts.logs;
    }
    std::printf("wq34 %s seed=%u logs=%u accepts=%u drops=%u broken A=%u I1=%u I2=%u I3=%u\n", rules.name, seed,
                counts.logs, counts.accepts, counts.drops, counts.broken[0], counts.broken[1], counts.broken[2],
                counts.broken[3]);
    return counts;
}

// The one rule passes; each old rule put back breaks the invariants it should: the old accept A, the old drop I1-I3
void Run(std::uint32_t seed)
{
    Loops(ONE);
    Counts const one = GeneratedLogs(seed, ONE);
    Check(one.accepts > 0 && one.drops > 0, "wq34 generated: no accept or no drop");
    for (std::size_t i = 0; i < one.words.size(); ++i)
        Check(one.words[i] > 0, std::string("wq34 generated: no case answered ") + QuestLog::WORDS[i]);
    Counts const oldAcceptLoops = Loops(OLD_ACCEPT);
    Counts const oldAccept = GeneratedLogs(seed, OLD_ACCEPT);
    Check(oldAccept.broken[0] > 0 && oldAcceptLoops.broken[0] > 0,
          "wq34 mutant: the old accept put back breaks no A (the invariant cannot tell the rules apart)");
    Counts const oldDropLoops = Loops(OLD_DROP);
    Counts const oldDrop = GeneratedLogs(seed, OLD_DROP);
    Check(oldDrop.broken[1] > 0 && oldDrop.broken[2] > 0 && oldDrop.broken[3] > 0 && oldDropLoops.broken[2] > 0 &&
              oldDropLoops.broken[3] > 0,
          "wq34 mutant: the old drop put back breaks not each of I1, I2 and I3");
}
}  // namespace Wq34
}  // namespace

// Questing WQ18b: a reader that keeps what it made of a bot's rows reads the book again only after a write
// (QuestFail::Reread over the book's version): the executor of a bot on an order asks at every tick
void FailBookView()
{
    QuestFail::Book book;
    book.Restore(7, 100, RowOf(1, "no_ender"));
    book.Restore(8, 200, RowOf(2, "timeout"));
    auto const read = [](Rows const& rows) { return QuestLog::Read(rows); };
    std::shared_ptr<QuestLog::Record const> held;
    std::uint64_t heldVersion = 0;
    std::uint64_t copies = book.Copies();
    for (int tick = 0; tick < 1000; ++tick)
        QuestFail::Reread(book, 7, held, heldVersion, read);
    Check(book.Copies() - copies == 1 && held && held->handInFailed == Ids{100} && held->givenUp == Ids{100},
          "fail book: 1000 ticks without a write copy the bot's rows once (mutant: Reread copies at every call)");
    book.Note(7, 101, "cannot_take", 0, 5);
    copies = book.Copies();
    for (int tick = 0; tick < 1000; ++tick)
        QuestFail::Reread(book, 7, held, heldVersion, read);
    bool const once = book.Copies() - copies == 1 && held->givenUp == Ids{100};
    book.Note(7, 101, "cannot_take", 0, 6);
    book.Note(7, 102, "cannot_take", 0, 6);
    book.Note(7, 102, "timeout", 3, 7);
    copies = book.Copies();
    for (int tick = 0; tick < 1000; ++tick)
        QuestFail::Reread(book, 7, held, heldVersion, read);
    Check(once && book.Copies() - copies == 1 && held->givenUp == (Ids{100, 101, 102}),
          "fail book: a write is read once, at the next tick; one refused take (cannot_take) is one give-up and not "
          "given up, the second gives it up, as a refused take and a timeout do (WQ18c)");
    std::uint64_t const version = book.Version();
    book.RowsOf(7);
    book.Find(7, 100);
    book.Size();
    book.TakeDirty();
    book.Forget(9);
    bool const still = book.Version() == version;
    book.Restore(8, 201, RowOf(1, "timeout"));
    std::uint64_t const restored = book.Version();
    book.Forget(7);
    QuestFail::Reread(book, 7, held, heldVersion, read);
    Check(still && restored != version && book.Version() != restored && held->givenUp.empty() &&
              held->handInFailed.empty(),
          "fail book: reads and the forget of a bot without rows move no version; a restore and a forget do, and the "
          "forgotten bot's copy is empty");
    // A ready quest stays whatever the record holds (WQ34: a failed hand-in is no drop), a grey one too
    QuestLog::Held const ready{.id = 100, .level = 4, .ready = true};
    Check(QuestLog::Abandon(ready, GrayLevel(20)) == Drop::Keep, "abandon: a ready quest stays, grey or not");
}

int main(int argc, char** argv)
{
    std::uint32_t seed = DEFAULT_SEED;
    if (argc > 1)
        seed = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    Table();
    FailBookView();
    GeneratedCases(seed);
    Wq34::Run(seed);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
