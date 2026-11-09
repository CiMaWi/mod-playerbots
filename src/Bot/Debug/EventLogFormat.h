/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_EVENTLOGFORMAT_H
#define _PLAYERBOT_EVENTLOGFORMAT_H

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

class Action;

// The Player-free part of the census event log: line format, source words and the thread-local source scope.
// Header-only so tests/event-log-selftest.cpp checks the same code the server runs.
namespace EventLog
{
constexpr char const* SOURCE_OTHER = "other";
constexpr char FIELD_SEPARATOR = ',';
constexpr char FIELD_REPLACEMENT = '_';
constexpr size_t LINE_RESERVE = 96;
// EventLog::Emit's world-log id and amount when they differ from the census line's; unset = the census values
constexpr uint32_t SAME_ID = std::numeric_limits<uint32_t>::max();
constexpr int64_t SAME_AMOUNT = std::numeric_limits<int64_t>::min();

// The world-log row's id and amount: Emit's worldLogId/worldLogAmount when set, else the census line's. WL7's Travel
// rows keep the census meaning (legs, replans, leg number) for tools/lab-ledger.js until WL11 and give the world log
// the trip id and seconds.
inline uint32_t WorldLogId(uint32_t id, uint32_t worldLogId) { return worldLogId == SAME_ID ? id : worldLogId; }
inline int64_t WorldLogAmount(int64_t amount, int64_t worldLogAmount)
{
    return worldLogAmount == SAME_AMOUNT ? amount : worldLogAmount;
}

// Copies a field without the characters that would split the CSV line or the row
inline void AppendField(std::string& out, std::string_view field)
{
    for (char const c : field)
        out.push_back(c == FIELD_SEPARATOR || c == '"' || c == '\n' || c == '\r' ? FIELD_REPLACEMENT : c);
}

// One bot_events.csv line: time,guid,type,source,id,amount,map,zone (tools/lab-ledger.js reads columns 2, 3 and 5)
inline std::string FormatEventLine(std::string_view time, uint64_t guid, std::string_view type, std::string_view source,
                                   uint32_t id, int64_t amount, uint32_t mapId, uint32_t zoneId)
{
    std::string line;
    line.reserve(LINE_RESERVE);
    AppendField(line, time);
    line.push_back(FIELD_SEPARATOR);
    line += std::to_string(guid);
    line.push_back(FIELD_SEPARATOR);
    AppendField(line, type);
    line.push_back(FIELD_SEPARATOR);
    AppendField(line, source.empty() ? std::string_view(SOURCE_OTHER) : source);
    line.push_back(FIELD_SEPARATOR);
    line += std::to_string(id);
    line.push_back(FIELD_SEPARATOR);
    line += std::to_string(amount);
    line.push_back(FIELD_SEPARATOR);
    line += std::to_string(mapId);
    line.push_back(FIELD_SEPARATOR);
    line += std::to_string(zoneId);
    line.push_back('\n');
    return line;
}

struct SourceWord
{
    std::string_view key;
    char const* word;
};

// Executing action name -> the plan's source word; the first key the name contains wins, other names pass through
inline constexpr SourceWord SOURCE_WORDS[] = {
    {"loot", "loot"},     {"quest", "quest"},      {"reward", "quest"},    {"repair", "repair"},
    {"sell", "vendor"},   {"buy", "vendor"},       {"train", "train"},     {"taxi", "flight"},
    {"flight", "flight"}, {"mail", "mail"},        {"auction", "auction"}, {"trade", "trade"},
    {"craft", "craft"},   {"disenchant", "craft"}, {"corpse", "corpse"},   {"spirit healer", "spirit_healer"},
};

inline std::string_view SourceWordOf(std::string_view actionName)
{
    if (actionName.empty())
        return SOURCE_OTHER;

    for (SourceWord const& entry : SOURCE_WORDS)
        if (actionName.find(entry.key) != std::string_view::npos)
            return entry.word;

    return actionName;
}

// Innermost cause on this thread: a literal (module grant sites) or the action Engine::ListenAndExecute runs
struct SourceState
{
    char const* source = nullptr;
    Action* action = nullptr;
};

inline thread_local SourceState currentSourceState;

// RAII cause for the hooks that fire inside the scope; nests and restores the previous one. Two thread-local stores,
// no allocation: an action's name is only read when an event inside the scope asks for it (CurrentSource).
class SourceScope
{
public:
    explicit SourceScope(char const* source) : _previous(currentSourceState) { currentSourceState = {source, nullptr}; }
    explicit SourceScope(Action* action) : _previous(currentSourceState) { currentSourceState = {nullptr, action}; }
    ~SourceScope() { currentSourceState = _previous; }

    SourceScope(SourceScope const&) = delete;
    SourceScope& operator=(SourceScope const&) = delete;

private:
    SourceState _previous;
};

// The item StoreNewItem stored last on this thread, with its entry. The item hooks that follow StoreNewItem (loot,
// group roll, quest reward, create) run after the hooks of scripts registered earlier, which may destroy the item
// (mod-junk-to-gold sells looted junk at once; a new item is deleted on the spot). Those hooks only compare the
// pointer with this note and never read the item; the core compares a possibly deleted item's pointer the same way
// (Player::MoveItemToInventory).
struct StoredItemNote
{
    void const* item = nullptr;
    uint32_t entry = 0;
};

inline thread_local StoredItemNote storedItemNote;

inline void NoteStoredItem(void const* item, uint32_t entry) { storedItemNote = {item, entry}; }

// The entry noted for this item by the StoreNewItem just before, or 0; a note answers once
inline uint32_t TakeStoredEntry(void const* item)
{
    StoredItemNote const note = storedItemNote;
    storedItemNote = {};
    return item && item == note.item ? note.entry : 0;
}

// Name of the innermost scope on this thread, or "other"; an action's name stays valid until the next call on the
// same thread (defined in CensusMgr.cpp, which knows Action)
char const* CurrentSource();

// The world log's WL6 words. The rank and XP source tables mirror the core's enums by value (CREATURE_ELITE_*,
// PlayerXPSource), so the selftests need no server headers.
constexpr uint32_t RANK_COUNT = 5;
inline constexpr char const* RANK_WORDS[RANK_COUNT] = {"normal", "elite", "rare_elite", "world_boss", "rare"};

// Kill src: the creature's rank
inline char const* RankWord(uint32_t rank) { return rank < RANK_COUNT ? RANK_WORDS[rank] : SOURCE_OTHER; }

constexpr uint32_t XP_SOURCE_COUNT = 5;
inline constexpr char const* XP_SOURCE_WORDS[XP_SOURCE_COUNT] = {"kill", "quest", "quest", "explore", SOURCE_OTHER};

// XpGain src: kill, quest (also the dungeon finder's), explore; battleground XP is other
inline char const* XpSourceWord(uint32_t source)
{
    return source < XP_SOURCE_COUNT ? XP_SOURCE_WORDS[source] : SOURCE_OTHER;
}

enum class MapKind : uint8_t
{
    World,
    Dungeon,
    Raid,
    Battleground,
    Arena
};

constexpr uint32_t MAP_KIND_COUNT = 5;
inline constexpr char const* MAP_KIND_WORDS[MAP_KIND_COUNT] = {"world", "dungeon", "raid", "battleground", "arena"};

// MapChange src: the new map's kind
inline char const* MapKindWord(MapKind kind)
{
    uint32_t const index = static_cast<uint32_t>(kind);
    return index < MAP_KIND_COUNT ? MAP_KIND_WORDS[index] : SOURCE_OTHER;
}

constexpr char const* FLIGHT_START = "start";
constexpr char const* FLIGHT_END = "end";
constexpr char const* LEARN_TRAINER = "trainer";
constexpr char const* LEARN_TALENT = "talent";
constexpr char const* LEARN_QUEST = "quest";
constexpr char const* RESURRECT_SPIRIT_HEALER = "spirit_healer";
constexpr char const* RESURRECT_CORPSE = "corpse";
constexpr char const* RESURRECT_SPELL = "spell";

// SpellLearn src: trainer (a debit just before paid for it, or a training action), talent, quest (a quest action),
// else other. scopeWord is SourceWordOf(CurrentSource()).
inline char const* SpellLearnWord(std::string_view scopeWord, bool paid, bool talent)
{
    if (paid || scopeWord == "train")
        return LEARN_TRAINER;
    if (talent)
        return LEARN_TALENT;
    if (scopeWord == "quest")
        return LEARN_QUEST;

    return SOURCE_OTHER;
}

// Resurrect src: the action's word (spirit_healer, corpse); else a spirit healer applies sickness, a resurrection
// spell restores nothing in the call (health is set after it), anything else is other
inline char const* ResurrectWord(std::string_view scopeWord, bool applySickness, float restorePercent)
{
    if (scopeWord == RESURRECT_SPIRIT_HEALER || applySickness)
        return RESURRECT_SPIRIT_HEALER;
    if (scopeWord == RESURRECT_CORPSE)
        return RESURRECT_CORPSE;
    if (restorePercent <= 0.0f)
        return RESURRECT_SPELL;

    return SOURCE_OTHER;
}
}  // namespace EventLog

#endif
