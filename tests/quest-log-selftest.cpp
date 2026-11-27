/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the quest-log rules of a bot on an order (questing WQ31, QuestLogRules.h): a table with one case
// per rule word, then generated logs, failure records and offers (the seed is printed; argv[1] replays it) checked
// against the invariants: no quest is accepted and abandoned by the same log state; the rules applied twice change
// nothing more; a ready quest without a failed hand-in is never abandoned; a full log accepts nothing and nothing is
// dropped for room; the plan is due exactly when the round's givers offer nothing more or its time has run out.
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

// A bot's log state: what Take::Facts views and Abandon reads
struct State
{
    std::uint8_t race{HUMAN};
    std::uint8_t cls{WARRIOR};
    std::uint8_t level{20};
    std::uint32_t greyDiff{Take::GREY_DIFF};
    Ids rewarded;  // sorted
    Ids inLog;     // sorted
    Ids ready;     // sorted, a part of inLog
    QuestLog::Record record;
};

std::vector<std::int32_t> GroupsOf(State const& state, QuestIndex const& index)
{
    std::vector<std::int32_t> groups;
    for (Ids const* ids : {&state.inLog, &state.rewarded})
        for (std::uint32_t const id : *ids)
            if (QuestRecord const* quest = index.Find(id); quest && quest->exclusiveGroup > 0)
                groups.push_back(quest->exclusiveGroup);
    std::sort(groups.begin(), groups.end());
    return groups;
}

Take::Facts FactsOf(State const& state, std::vector<std::int32_t> const& groups)
{
    Take::Facts facts;
    facts.race = state.race;
    facts.cls = state.cls;
    facts.level = state.level;
    facts.greyDiff = state.greyDiff;
    facts.eliteGroup = false;  // WQ.43: as the accept at a giver
    facts.rewarded = state.rewarded;
    facts.inLog = state.inLog;
    facts.givenUp = state.record.givenUp;
    facts.takenGroups = groups;
    return facts;
}

// A client's log: the generated logs reach past it, and the rules know no room (questing WQ40)
constexpr std::size_t CLIENT_SLOTS = 25;

bool MayAcceptNow(QuestRecord const& quest, State const& state, QuestIndex const& index)
{
    std::vector<std::int32_t> const groups = GroupsOf(state, index);
    return QuestLog::MayAccept(quest, FactsOf(state, groups), index);
}

Drop AbandonNow(QuestRecord const& quest, State const& state, bool ready)
{
    return QuestLog::Abandon(QuestLog::HeldOf(quest, state.record, SideOfRace(state.race), ready), state.level,
                             state.greyDiff);
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

// One application of the rules: the upkeep's drops by Abandon, then the accepts by MayAccept over the offers until
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
    constexpr std::uint32_t GREY = 2;     // level 15 for a level-20 bot: grey at 4, not at 6
    constexpr std::uint32_t REFUSED = 3;  // the capability table refuses it for the Alliance
    constexpr std::uint32_t ELITE = 4;
    constexpr std::uint32_t TAKE_REFUSED = 5;  // a planned take ended without the accept once (WQ18c)
    constexpr std::uint32_t ONCE = 6;          // given up once (timeout)
    constexpr std::uint32_t TWICE = 7;         // given up twice, the last a refused take overwritten by a timeout
    constexpr std::uint32_t NO_ENDER = 8;      // its hand-in failed at its ender
    constexpr std::uint32_t EDGE = 9;          // level 16: not grey for a level-20 bot at 4
    constexpr std::uint32_t TAKE_TWICE = 10;   // a planned take ended without the accept twice

    QuestRecord refused = Plain(REFUSED);
    refused.refused[0] = 1;
    QuestRecord elite = Plain(ELITE);
    elite.flags = QuestFlag::ELITE;
    QuestIndex const index = IndexOf({Plain(PLAIN), Plain(GREY, 15), refused, elite, Plain(TAKE_REFUSED), Plain(ONCE),
                                      Plain(TWICE), Plain(NO_ENDER), Plain(EDGE, 16), Plain(TAKE_TWICE)});

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
    Check(!may(GREY), "accept: grey");
    Check(may(EDGE), "accept: four levels below is not grey");
    Check(!may(REFUSED), "accept: the executor cannot do it");
    Check(!may(ELITE), "accept: elite");
    Check(may(TAKE_REFUSED), "accept: a take refused once is taken again, as any cause given up once");
    Check(!may(TAKE_TWICE) &&
              Take::May(*index.Find(TAKE_TWICE), FactsOf(bot, GroupsOf(bot, index)), index) == Take::Reason::GivenUp,
          "accept: a take refused twice is given up (the take test answers GivenUp)");
    Check(may(ONCE), "accept: given up once by a timeout");
    Check(!may(TWICE), "accept: given up twice");
    Check(!may(NO_ENDER), "accept: its hand-in failed");
    bot.inLog = {PLAIN};
    Check(!may(PLAIN), "accept: in the log");
    bot.inLog.clear();

    auto const drop = [](QuestLog::Held held, std::uint32_t greyDiff = Take::GREY_DIFF)
    { return QuestLog::Word(QuestLog::Abandon(held, 20, greyDiff)); };
    auto const held = [](std::int32_t level, bool ready, bool handInFailed, bool givenUp, bool cannotDo)
    {
        QuestLog::Held quest;
        quest.id = 1;
        quest.level = level;
        quest.ready = ready;
        quest.handInFailed = handInFailed;
        quest.givenUp = givenUp;
        quest.cannotDo = cannotDo;
        return quest;
    };
    auto const is = [](char const* got, char const* want) { return std::string(got) == want; };
    Check(is(drop(held(20, false, false, false, false)), "keep"), "abandon: a plain quest is kept");
    Check(is(drop(held(15, false, false, false, false)), "outlevelled"), "abandon: grey");
    Check(is(drop(held(16, false, false, false, false)), "keep"), "abandon: four levels below is kept");
    Check(is(drop(held(15, false, false, false, false), 6), "keep"), "abandon: the server's grey line");
    Check(is(drop(held(0, false, false, false, false)), "keep"), "abandon: the bot's level");
    Check(is(drop(held(-1, false, false, false, false)), "keep"), "abandon: the bot's level (-1)");
    Check(is(drop(held(20, false, false, true, false)), "given_up"), "abandon: given up");
    Check(is(drop(held(20, false, false, false, true)), "cannot_do"), "abandon: cannot do");
    Check(is(drop(held(20, true, true, false, false)), "no_ender"), "abandon: ready, its hand-in failed");
    Check(is(drop(held(20, true, true, true, true)), "no_ender"), "abandon: ready, hand-in failed and more");
    Check(is(drop(held(15, true, false, false, false)), "keep"), "abandon: a ready grey quest is kept");
    Check(is(drop(held(20, true, false, true, false)), "keep"), "abandon: a ready given-up quest is kept");
    Check(is(drop(held(20, true, false, false, true)), "keep"), "abandon: a ready quest it cannot do is kept");
    Check(is(drop(held(20, false, true, false, false)), "keep"), "abandon: a failed hand-in alone, not ready");
    Check(is(drop(held(15, false, false, true, true)), "outlevelled"), "abandon: grey before given up");
    Check(is(drop(held(20, false, false, true, true)), "given_up"), "abandon: given up before cannot do");
    Check(QuestLog::HeldOf(refused, record, 0, false).cannotDo && !QuestLog::HeldOf(refused, record, 1, false).cannotDo,
          "held: cannot do for its side only");
    Check(QuestLog::HeldOf(*index.Find(NO_ENDER), record, 0, true).handInFailed &&
              QuestLog::HeldOf(*index.Find(TWICE), record, 0, false).givenUp &&
              !QuestLog::HeldOf(*index.Find(ONCE), record, 0, false).givenUp,
          "held: the record's lists");

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
    if (rng() % 4 == 0)
        state.greyDiff = 2 + rng() % 6;
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
        case Drop::Outlevelled:
            return !ready && Take::IsGrey(quest, state.level, state.greyDiff);
        case Drop::GivenUp:
            return !ready && Has(state.record.givenUp, quest.id);
        case Drop::CannotDo:
            return !ready && RefusedFor(quest, SideOfRace(state.race));
        case Drop::NoEnder:
            return ready && Has(state.record.handInFailed, quest.id);
        default:
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

    // Every drop has its reason; a quest without one is kept, ready ones without a failed hand-in too
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
        bool const reason = ready ? Has(before.record.handInFailed, id)
                                  : Take::IsGrey(quest, before.level, before.greyDiff) ||
                                        Has(before.record.givenUp, id) || RefusedFor(quest, SideOfRace(before.race));
        bool const dropped = std::find(outcome.dropped.begin(), outcome.dropped.end(), id) != outcome.dropped.end();
        Check(dropped == reason,
              who + ": quest " + std::to_string(id) + (ready ? " (ready)" : "") + (dropped ? " dropped" : " kept"));
        if (!dropped)
            ++counts.drops[static_cast<std::size_t>(Drop::Keep)];
    }

    // No rule for room (questing WQ40): the accept is the take test alone, whatever the log holds; `full` counts the
    // logs of a client's 25 quests or more, which accept as any other
    std::vector<std::int32_t> const groupsBefore = GroupsOf(before, index);
    for (std::uint32_t const id : offers)
        if (QuestRecord const* quest = index.Find(id))
            Check(MayAcceptNow(*quest, before, index) ==
                      !Take::May(*quest, FactsOf(before, groupsBefore), index).has_value(),
                  who + ": quest " + std::to_string(id) + " accepted or refused by more than the take test");
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
    for (std::size_t i = 0; i < counts.drops.size(); ++i)
        Check(counts.drops[i] > 0, std::string("generated: no case answered ") + QuestLog::WORDS[i]);
    Check(due > 0 && due < ROUNDS, "generated: rounds all due or none");
}
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
    // The order's `bags` end writes no failure row, so the record has no failed hand-in and the ready quest stays
    QuestLog::Held ready;
    ready.id = 100;
    ready.level = 10;
    ready.ready = true;
    QuestLog::Held handInFailed = ready;
    handInFailed.handInFailed = true;
    Check(QuestLog::Abandon(ready, 10, 5) == Drop::Keep && QuestLog::Abandon(handInFailed, 10, 5) == Drop::NoEnder,
          "abandon: a ready quest stays unless a failed hand-in is on record (full bags put none there)");
}

int main(int argc, char** argv)
{
    std::uint32_t seed = DEFAULT_SEED;
    if (argc > 1)
        seed = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    Table();
    FailBookView();
    GeneratedCases(seed);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
