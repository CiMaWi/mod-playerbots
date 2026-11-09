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
}  // namespace EventLog

#endif
