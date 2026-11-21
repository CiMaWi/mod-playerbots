/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STARTERRULES_H
#define PLAYERBOTS_STARTERRULES_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <unordered_map>

#include "EventTypes.h"
#include "RecordRules.h"

// Conversation starters (living-conversation LC28a): an event of a bot's play that a real player near it could hear
// it talk about. Pure rules without game types, so tests/starter-selftest.cpp proves them offline: one table row per
// kind (its event type, its salience, its mark and its cooldowns), the decision for one event, the cooldown book and
// the bounded queue of one listener. Salience is decided here, before any model call; numbers are tuning values.
namespace StarterRules
{
enum class Kind : uint8_t
{
    Loot,
    Kill,
    Quest,
    Level,
    Visit,
    Need,
    Praise
};

constexpr std::size_t KIND_COUNT = 7;
// Stable words (world-log.md, Starter); the index is the kind
inline constexpr char const* KIND_WORDS[KIND_COUNT] = {"loot", "kill", "quest", "level", "visit", "need", "praise"};

enum class Reason : uint8_t
{
    Cooldown,
    Unperceived,
    BelowMark
};

inline constexpr char const* REASON_WORDS[] = {"cooldown", "unperceived", "below_mark"};

// Where a started line would be said: in the party the bot shares with the listener, else aloud beside it
enum class Scope : uint8_t
{
    Say,
    Party
};

inline constexpr char const* SCOPE_WORDS[] = {"say", "party"};

inline char const* Word(Kind kind)
{
    std::size_t const index = static_cast<std::size_t>(kind);
    return index < KIND_COUNT ? KIND_WORDS[index] : "other";
}

inline char const* Word(Reason reason) { return REASON_WORDS[static_cast<std::size_t>(reason)]; }

inline char const* Word(Scope scope) { return SCOPE_WORDS[static_cast<std::size_t>(scope)]; }

// Real time (LC.46): a bot starts a talk about one kind of thing at most once in BOT_COOLDOWN_MS; one party hears one
// kind at most once in SCOPE_COOLDOWN_MS
constexpr uint64_t BOT_COOLDOWN_MS = 10 * 60 * 1000;
constexpr uint64_t SCOPE_COOLDOWN_MS = 60 * 1000;
constexpr uint8_t NEED_SALIENCE = 40;
constexpr uint8_t PRAISE_SALIENCE = 40;
// Offered starters a listener keeps for the chat module to read (LC28b); the oldest goes first
constexpr std::size_t QUEUE_CAP = 8;
// The book forgets entries older than every cooldown once it holds this many
constexpr std::size_t BOOK_PRUNE_AT = 4096;

// The checkable facts of one event; the manager fills those of its kind
struct Facts
{
    RecordRules::Facts Record;  // kill: rank, level difference; loot: quality, item level gain; level: the new level
    bool Worn = false;          // loot: the looter wears the item
    bool HandedOut = false;     // loot, level: the server handed it out, the bot did not earn it
    bool LevelUp = false;       // level: the new level is above the old one
    bool First = false;         // visit: the first entry of the place
    bool Begun = false;         // need: the shortfall begins (not ends)
    bool Usable = false;        // praise: the player can use the item
    bool Better = false;        // praise: it beats what the player wore
};

// 0 when the event is nothing to talk about
inline uint8_t LootSalience(Facts const& facts)
{
    if (!facts.Worn || facts.HandedOut || facts.Record.ItemLevelGain <= 0)
        return 0;

    return RecordRules::Score(RecordRules::Kind::Equip, facts.Record);
}

// A boss or a rare creature; an ordinary or merely elite kill is none
inline uint8_t KillSalience(Facts const& facts)
{
    uint8_t const rank = facts.Record.Rank;
    if (rank != RecordRules::RANK_RARE && rank != RecordRules::RANK_RARE_ELITE && rank != RecordRules::RANK_WORLD_BOSS)
        return 0;

    return RecordRules::Score(RecordRules::Kind::Kill, facts.Record);
}

inline uint8_t QuestSalience(Facts const& facts) { return RecordRules::Score(RecordRules::Kind::Quest, facts.Record); }

inline uint8_t LevelSalience(Facts const& facts)
{
    if (!facts.LevelUp || facts.HandedOut)
        return 0;

    return RecordRules::Score(RecordRules::Kind::Level, facts.Record);
}

inline uint8_t VisitSalience(Facts const& facts)
{
    return facts.First ? RecordRules::Score(RecordRules::Kind::Visit, facts.Record) : 0;
}

inline uint8_t NeedSalience(Facts const& facts) { return facts.Begun ? NEED_SALIENCE : 0; }

inline uint8_t PraiseSalience(Facts const& facts) { return facts.Usable && facts.Better ? PRAISE_SALIENCE : 0; }

struct Row
{
    Kind StarterKind;
    EventLog::EventType Event;  // the event that brings it; Unknown: no event yet, the row is off
    uint8_t (*Salience)(Facts const&);
    uint8_t Mark;
    uint64_t BotCooldownMs;
    uint64_t ScopeCooldownMs;
};

// Visit is not an event row: BotRecordMgr hands a first entry to the starters under it. Praise waits for an event of
// the player's own new item; Need is off (no event) until a shortfall fixture gives it a live case (NeedShort).
using EventLog::EventType;
inline constexpr Row TABLE[KIND_COUNT] = {
    {Kind::Loot, EventType::Equip, LootSalience, RecordRules::LOW_MARK, BOT_COOLDOWN_MS, SCOPE_COOLDOWN_MS},
    {Kind::Kill, EventType::Kill, KillSalience, RecordRules::LOW_MARK, BOT_COOLDOWN_MS, SCOPE_COOLDOWN_MS},
    {Kind::Quest, EventType::QuestDone, QuestSalience, RecordRules::LOW_MARK, BOT_COOLDOWN_MS, SCOPE_COOLDOWN_MS},
    {Kind::Level, EventType::Level, LevelSalience, RecordRules::LOW_MARK, BOT_COOLDOWN_MS, SCOPE_COOLDOWN_MS},
    {Kind::Visit, EventType::Visit, VisitSalience, RecordRules::LOW_MARK, BOT_COOLDOWN_MS, SCOPE_COOLDOWN_MS},
    {Kind::Need, EventType::Unknown, NeedSalience, RecordRules::LOW_MARK, BOT_COOLDOWN_MS, SCOPE_COOLDOWN_MS},
    {Kind::Praise, EventType::Unknown, PraiseSalience, RecordRules::LOW_MARK, BOT_COOLDOWN_MS, SCOPE_COOLDOWN_MS}};

// The row an event type brings, or none (every other type, and the rows without an event)
inline Row const* RowFor(EventType type)
{
    if (type == EventType::Unknown)
        return nullptr;

    for (Row const& row : TABLE)
        if (row.Event == type)
            return &row;

    return nullptr;
}

// Who could hear the bot: whether a real player perceives it or shares its group, and the scope it would speak in
struct Seen
{
    bool Perceived = false;
    Scope Where = Scope::Say;
    uint64_t ScopeKey = 0;  // party: the group; say: the bot
};

// The last offer per bot and kind and per scope and kind. Not thread-safe: the manager holds its lock.
class Book
{
public:
    uint64_t LastByBot(uint32_t bot, Kind kind) const { return Find(_byBot, BotKey(bot, kind)); }

    uint64_t LastByScope(Seen const& seen, Kind kind) const { return Find(_byScope, ScopeKey(seen, kind)); }

    void Note(uint32_t bot, Seen const& seen, Kind kind, uint64_t nowMs)
    {
        if (_byBot.size() + _byScope.size() >= BOOK_PRUNE_AT)
            Prune(nowMs);

        _byBot[BotKey(bot, kind)] = nowMs;
        _byScope[ScopeKey(seen, kind)] = nowMs;
    }

    std::size_t Size() const { return _byBot.size() + _byScope.size(); }

private:
    // 0: never offered
    static uint64_t Find(std::unordered_map<uint64_t, uint64_t> const& map, uint64_t key)
    {
        auto const found = map.find(key);
        return found == map.end() ? 0 : found->second;
    }

    static uint64_t BotKey(uint32_t bot, Kind kind) { return (uint64_t(bot) << 8) | uint64_t(kind); }

    static uint64_t ScopeKey(Seen const& seen, Kind kind)
    {
        return (seen.ScopeKey << 9) | (uint64_t(seen.Where) << 8) | uint64_t(kind);
    }

    void Prune(uint64_t nowMs)
    {
        uint64_t const longest = BOT_COOLDOWN_MS > SCOPE_COOLDOWN_MS ? BOT_COOLDOWN_MS : SCOPE_COOLDOWN_MS;
        for (auto* map : {&_byBot, &_byScope})
            for (auto it = map->begin(); it != map->end();)
                it = nowMs >= it->second + longest ? map->erase(it) : std::next(it);
    }

    std::unordered_map<uint64_t, uint64_t> _byBot;
    std::unordered_map<uint64_t, uint64_t> _byScope;
};

enum class Outcome : uint8_t
{
    Offer,
    Drop,
    Silent  // nothing worth saying and nobody to hear it: no row
};

struct Verdict
{
    Outcome Result = Outcome::Silent;
    Reason Why = Reason::BelowMark;
    uint8_t Salience = 0;
};

inline bool Cooling(uint64_t last, uint64_t cooldownMs, uint64_t nowMs)
{
    return last != 0 && nowMs < last + cooldownMs;
}

// One event of one bot: below the mark it is dropped (silently when nobody perceives the bot), then nobody perceiving
// it drops it, then a cooldown of the bot or of its scope. The caller notes an offer in the book.
inline Verdict Decide(Row const& row, Facts const& facts, uint32_t bot, Seen const& seen, uint64_t nowMs,
                      Book const& book)
{
    Verdict verdict;
    verdict.Salience = row.Salience(facts);
    if (verdict.Salience < row.Mark)
    {
        verdict.Result = seen.Perceived ? Outcome::Drop : Outcome::Silent;
        verdict.Why = Reason::BelowMark;
        return verdict;
    }

    verdict.Result = Outcome::Drop;
    if (!seen.Perceived)
    {
        verdict.Why = Reason::Unperceived;
        return verdict;
    }

    if (Cooling(book.LastByBot(bot, row.StarterKind), row.BotCooldownMs, nowMs) ||
        Cooling(book.LastByScope(seen, row.StarterKind), row.ScopeCooldownMs, nowMs))
    {
        verdict.Why = Reason::Cooldown;
        return verdict;
    }

    verdict.Result = Outcome::Offer;
    return verdict;
}

// One offered starter as the chat module reads it
struct Entry
{
    uint32_t Bot = 0;
    Kind StarterKind = Kind::Loot;
    uint8_t Salience = 0;
    Scope Where = Scope::Say;
    uint64_t AtMs = 0;
};

// The starters offered to one listener, at most QUEUE_CAP; a new one past the cap pushes the oldest out
class Queue
{
public:
    void Push(Entry const& entry)
    {
        if (_entries.size() >= QUEUE_CAP)
            _entries.pop_front();

        _entries.push_back(entry);
    }

    std::deque<Entry> const& Entries() const { return _entries; }

private:
    std::deque<Entry> _entries;
};
}  // namespace StarterRules

#endif
