/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the conversation starters (living-conversation LC28a, StarterRules.h): the table's decisions over
// modelled events (an upgrade the looter wears against one it does not, praise only for a usable and better item, an
// ordinary kill is none), the cooldowns, the bounded queue, and a search over random event streams for a counterexample
// to two invariants: no bot is offered two starters of one kind inside the cooldown (nor a party one kind inside its
// own), and none is offered where no real player perceives the bot. Mutants of each guard must be caught.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot tests/starter-selftest.cpp

#include <cstdio>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include "StarterRules.h"

namespace
{
using namespace StarterRules;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

constexpr uint64_t T0 = 1000000;

Row const& RowOf(Kind kind) { return TABLE[static_cast<std::size_t>(kind)]; }

Seen Near(Scope where = Scope::Say, uint64_t key = 1)
{
    Seen seen;
    seen.Perceived = true;
    seen.Where = where;
    seen.ScopeKey = key;
    return seen;
}

Seen Far()
{
    Seen seen;
    seen.Perceived = false;
    return seen;
}

Verdict Once(Row const& row, Facts const& facts, Seen const& seen)
{
    Book book;
    return Decide(row, facts, 1, seen, T0, book);
}

bool Offered(Row const& row, Facts const& facts) { return Once(row, facts, Near()).Result == Outcome::Offer; }

bool DroppedFor(Verdict const& verdict, Reason reason)
{
    return verdict.Result == Outcome::Drop && verdict.Why == reason;
}

Facts Loot(bool worn, int32_t gain, uint8_t quality)
{
    Facts facts;
    facts.Worn = worn;
    facts.Record.ItemLevelGain = gain;
    facts.Record.Quality = quality;
    return facts;
}

Facts Kill(uint8_t rank, int32_t levelDiff)
{
    Facts facts;
    facts.Record.Rank = rank;
    facts.Record.LevelDiff = levelDiff;
    return facts;
}

Facts Praise(bool usable, bool better)
{
    Facts facts;
    facts.Usable = usable;
    facts.Better = better;
    return facts;
}

void TableChecks()
{
    bool rowsInOrder = true;
    for (std::size_t index = 0; index < KIND_COUNT; ++index)
        rowsInOrder = rowsInOrder && static_cast<std::size_t>(TABLE[index].StarterKind) == index &&
                      TABLE[index].Salience && TABLE[index].BotCooldownMs >= TABLE[index].ScopeCooldownMs;
    Check(rowsInOrder, "table: a row per kind in kind order, each with a salience and a bot cooldown >= the scope's");
    Check(RowFor(EventType::Kill) == &RowOf(Kind::Kill) && RowFor(EventType::Equip) == &RowOf(Kind::Loot) &&
              RowFor(EventType::QuestDone) == &RowOf(Kind::Quest) && RowFor(EventType::Level) == &RowOf(Kind::Level) &&
              RowFor(EventType::Visit) == &RowOf(Kind::Visit),
          "table: each event type finds its row");
    Check(!RowFor(EventType::NeedShort) && RowOf(Kind::Need).Event == EventType::Unknown &&
              RowOf(Kind::Praise).Event == EventType::Unknown,
          "table: need and praise are off (no event), NeedShort finds no row");
    bool readersMatch = true;
    for (EventLog::TypeRow const& type : EventLog::TYPES)
        readersMatch =
            readersMatch && EventLog::ReadBy(type.Type, EventLog::READ_STARTER) == (RowFor(type.Type) != nullptr);
    Check(readersMatch, "table: the starter rows are exactly the types EventTypes.h gives the starters");
    Check(!RowFor(EventType::Unknown) && !RowFor(EventType::Starter) && !RowFor(EventType::StarterDrop) &&
              !RowFor(EventType::Record) && !RowFor(EventType::Death),
          "table: praise has no event yet, a starter's own rows and other types find none");
    Check(std::string_view(Word(Kind::Loot)) == "loot" && std::string_view(Word(Kind::Praise)) == "praise" &&
              std::string_view(Word(Reason::BelowMark)) == "below_mark" &&
              std::string_view(Word(Reason::Unperceived)) == "unperceived" &&
              std::string_view(Word(Reason::Cooldown)) == "cooldown" && std::string_view(Word(Scope::Party)) == "party",
          "words: kinds, reasons and scopes");
}

void DecisionChecks()
{
    Row const& loot = RowOf(Kind::Loot);
    Check(Offered(loot, Loot(true, 10, RecordRules::QUALITY_RARE)), "loot: a worn upgrade is offered");
    Check(DroppedFor(Once(loot, Loot(false, 10, RecordRules::QUALITY_RARE), Near()), Reason::BelowMark),
          "loot: the same item not worn is none (below the mark)");
    Check(!Offered(loot, Loot(true, 0, RecordRules::QUALITY_EPIC)), "loot: an item no better than the old one is none");
    Check(!Offered(loot, Loot(true, 2, 2)), "loot: a small common gain is below the mark");
    Facts handed = Loot(true, 10, RecordRules::QUALITY_EPIC);
    handed.HandedOut = true;
    Check(!Offered(loot, handed), "loot: an item the server handed out is none");

    Row const& praise = RowOf(Kind::Praise);
    Check(Offered(praise, Praise(true, true)), "praise: a usable, better item is offered");
    Check(!Offered(praise, Praise(false, true)), "praise: an item the player cannot use is none");
    Check(!Offered(praise, Praise(true, false)), "praise: an item no better than the old one is none");

    Row const& kill = RowOf(Kind::Kill);
    Verdict const ordinary = Once(kill, Kill(RecordRules::RANK_NORMAL, 0), Near());
    Check(DroppedFor(ordinary, Reason::BelowMark) && ordinary.Salience == 0, "kill: an ordinary kill is none");
    Check(!Offered(kill, Kill(RecordRules::RANK_NORMAL, 5)) && !Offered(kill, Kill(RecordRules::RANK_ELITE, 0)),
          "kill: a hard normal or a plain elite kill is none");
    Check(Offered(kill, Kill(RecordRules::RANK_RARE, 0)) && Offered(kill, Kill(RecordRules::RANK_RARE_ELITE, 0)) &&
              Offered(kill, Kill(RecordRules::RANK_WORLD_BOSS, 0)),
          "kill: a rare, a rare elite and a boss are offered");
    Check(Once(kill, Kill(RecordRules::RANK_WORLD_BOSS, 0), Near()).Salience >= RecordRules::HIGH_MARK,
          "kill: a boss's salience is at the high mark or above");

    Facts quest;
    Check(Offered(RowOf(Kind::Quest), quest), "quest: a quest done is offered");

    Facts level;
    level.LevelUp = true;
    level.Record.Level = 12;
    Check(Offered(RowOf(Kind::Level), level), "level: a level gained is offered");
    level.HandedOut = true;
    Check(!Offered(RowOf(Kind::Level), level), "level: a level the server handed out is none");
    level.HandedOut = false;
    level.LevelUp = false;
    Check(!Offered(RowOf(Kind::Level), level), "level: a level lost is none");

    Facts visit;
    visit.First = true;
    Check(Offered(RowOf(Kind::Visit), visit), "visit: a first entry is offered");
    visit.First = false;
    Check(!Offered(RowOf(Kind::Visit), visit), "visit: a later entry is none");

    Facts need;
    need.Begun = true;
    Check(Offered(RowOf(Kind::Need), need), "need: a shortfall begun is offered");
    need.Begun = false;
    Check(!Offered(RowOf(Kind::Need), need), "need: a shortfall ended is none");

    Check(DroppedFor(Once(kill, Kill(RecordRules::RANK_RARE, 0), Far()), Reason::Unperceived),
          "perception: a rare kill nobody perceives is dropped (unperceived)");
    Check(Once(kill, Kill(RecordRules::RANK_NORMAL, 0), Far()).Result == Outcome::Silent,
          "perception: an ordinary kill nobody perceives leaves no row");
}

void CooldownChecks()
{
    Row const& kill = RowOf(Kind::Kill);
    Facts const rare = Kill(RecordRules::RANK_RARE, 0);
    Book book;
    Seen const say = Near(Scope::Say, 1);
    Check(Decide(kill, rare, 1, say, T0, book).Result == Outcome::Offer, "cooldown: the first is offered");
    book.Note(1, say, Kind::Kill, T0);
    Check(DroppedFor(Decide(kill, rare, 1, say, T0 + 60000, book), Reason::Cooldown),
          "cooldown: the same bot and kind a minute later is dropped");
    Check(Decide(kill, rare, 1, say, T0 + BOT_COOLDOWN_MS, book).Result == Outcome::Offer,
          "cooldown: the same bot and kind after the bot cooldown is offered");
    Facts level;
    level.LevelUp = true;
    Check(Decide(RowOf(Kind::Level), level, 1, say, T0 + 1000, book).Result == Outcome::Offer,
          "cooldown: another kind of the same bot is offered");
    Check(Decide(kill, rare, 2, Near(Scope::Say, 2), T0 + 1000, book).Result == Outcome::Offer,
          "cooldown: another bot beside the same player is offered");

    Seen const party = Near(Scope::Party, 77);
    book.Note(3, party, Kind::Kill, T0);
    Check(DroppedFor(Decide(kill, rare, 4, party, T0 + 1000, book), Reason::Cooldown),
          "cooldown: another bot of the same party inside the scope cooldown is dropped");
    Check(Decide(kill, rare, 4, party, T0 + SCOPE_COOLDOWN_MS, book).Result == Outcome::Offer,
          "cooldown: another bot of the same party after the scope cooldown is offered");

    Book big;
    for (uint32_t bot = 1; bot <= 3 * BOOK_PRUNE_AT; ++bot)
        big.Note(bot, Near(Scope::Say, bot), Kind::Quest, T0 + uint64_t(bot) * BOT_COOLDOWN_MS);
    Check(big.Size() <= BOOK_PRUNE_AT + 2, "book: bounded, old entries pruned");

    Queue queue;
    for (uint32_t bot = 1; bot <= 20; ++bot)
        queue.Push({bot, Kind::Quest, 35, Scope::Say, T0 + bot});
    Check(queue.Entries().size() == QUEUE_CAP && queue.Entries().front().Bot == 20 - QUEUE_CAP + 1 &&
              queue.Entries().back().Bot == 20,
          "queue: at most QUEUE_CAP entries, the oldest pushed out");
}

enum class Mutant
{
    None,
    CooldownIgnored,    // the book never notes an offer
    PerceptionIgnored,  // every bot is taken for perceived
};

struct SearchResult
{
    uint64_t events = 0;
    uint64_t offers = 0;
    std::map<Reason, uint64_t> drops;
    uint64_t counterexamples = 0;
};

constexpr uint32_t SEARCH_BOTS = 8;
constexpr uint64_t SEARCH_GROUP = 900;

SearchResult Search(Mutant mutant, uint32_t seed, uint32_t steps)
{
    std::mt19937 random(seed);
    auto const roll = [&random](uint32_t below) { return static_cast<uint32_t>(random() % below); };
    SearchResult result;
    Book book;
    std::map<std::pair<uint32_t, Kind>, uint64_t> lastBot;
    std::map<std::pair<uint64_t, Kind>, uint64_t> lastParty;
    uint64_t now = T0;
    for (uint32_t step = 0; step < steps; ++step)
    {
        now += roll(4) == 0 ? roll(BOT_COOLDOWN_MS) : roll(20000);
        uint32_t const bot = 1 + roll(SEARCH_BOTS);
        Row const& row = TABLE[roll(KIND_COUNT)];
        Facts facts;
        facts.Record.Rank = static_cast<uint8_t>(roll(5));
        facts.Record.LevelDiff = static_cast<int32_t>(roll(12)) - 4;
        facts.Record.Level = 1 + roll(80);
        facts.Record.Quality = static_cast<uint8_t>(roll(5));
        facts.Record.ItemLevelGain = static_cast<int32_t>(roll(20)) - 5;
        facts.Worn = roll(2);
        facts.HandedOut = roll(4) == 0;
        facts.LevelUp = roll(2);
        facts.First = roll(2);
        facts.Begun = roll(2);
        facts.Usable = roll(2);
        facts.Better = roll(2);
        bool const perceived = roll(2);
        bool const grouped = bot <= SEARCH_BOTS / 2 && roll(2);
        Seen seen;
        seen.Perceived = perceived || mutant == Mutant::PerceptionIgnored;
        seen.Where = grouped ? Scope::Party : Scope::Say;
        seen.ScopeKey = grouped ? SEARCH_GROUP : bot;

        ++result.events;
        Verdict const verdict = Decide(row, facts, bot, seen, now, book);
        if (verdict.Result == Outcome::Drop)
            ++result.drops[verdict.Why];
        if (verdict.Result != Outcome::Offer)
            continue;

        ++result.offers;
        if (mutant != Mutant::CooldownIgnored)
            book.Note(bot, seen, row.StarterKind, now);
        bool bad = !perceived || verdict.Salience < row.Mark;
        auto const byBot = lastBot.find({bot, row.StarterKind});
        bad = bad || (byBot != lastBot.end() && now < byBot->second + row.BotCooldownMs);
        lastBot[{bot, row.StarterKind}] = now;
        if (grouped)
        {
            auto const byParty = lastParty.find({SEARCH_GROUP, row.StarterKind});
            bad = bad || (byParty != lastParty.end() && now < byParty->second + row.ScopeCooldownMs);
            lastParty[{SEARCH_GROUP, row.StarterKind}] = now;
        }
        if (bad && ++result.counterexamples <= 3 && mutant == Mutant::None)
            std::printf("COUNTEREXAMPLE bot %u kind %s at %llu perceived %d\n", bot, Word(row.StarterKind),
                        static_cast<unsigned long long>(now), perceived ? 1 : 0);
    }

    return result;
}

void SearchChecks()
{
    constexpr uint32_t STEPS = 200000;
    uint64_t counterexamples = 0;
    uint64_t events = 0;
    uint64_t offers = 0;
    bool everyReason = true;
    for (uint32_t seed = 1; seed <= 5; ++seed)
    {
        SearchResult const result = Search(Mutant::None, seed, STEPS);
        counterexamples += result.counterexamples;
        events += result.events;
        offers += result.offers;
        for (Reason reason : {Reason::Cooldown, Reason::Unperceived, Reason::BelowMark})
            everyReason = everyReason && result.drops.count(reason) && result.drops.at(reason) > 0;
    }
    std::printf("events tried: %llu, offered: %llu, counterexamples: %llu\n", static_cast<unsigned long long>(events),
                static_cast<unsigned long long>(offers), static_cast<unsigned long long>(counterexamples));
    Check(counterexamples == 0, "search: no bot offered one kind twice inside the cooldown, none offered unperceived");
    Check(offers > 0 && everyReason, "search: the streams reach offers and every drop reason (not vacuous)");
}

void MutantChecks()
{
    Check(Search(Mutant::CooldownIgnored, 1, 200000).counterexamples > 0,
          "mutant killed: the cooldown ignored (the search finds a repeat)");
    Check(Search(Mutant::PerceptionIgnored, 1, 200000).counterexamples > 0,
          "mutant killed: perception ignored (the search finds an unperceived offer)");

    Row unworn = RowOf(Kind::Loot);
    unworn.Salience = [](Facts const& facts)
    {
        Facts worn = facts;
        worn.Worn = true;
        return LootSalience(worn);
    };
    Check(Once(unworn, Loot(false, 10, RecordRules::QUALITY_RARE), Near()).Result == Outcome::Offer,
          "mutant killed: loot not worn offered (the decision row catches it)");

    Row unusable = RowOf(Kind::Praise);
    unusable.Salience = [](Facts const& facts) { return facts.Better ? PRAISE_SALIENCE : uint8_t(0); };
    Check(Once(unusable, Praise(false, true), Near()).Result == Outcome::Offer,
          "mutant killed: praise for an unusable item (the decision row catches it)");
}
}  // namespace

int main()
{
    TableChecks();
    DecisionChecks();
    CooldownChecks();
    SearchChecks();
    MutantChecks();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
