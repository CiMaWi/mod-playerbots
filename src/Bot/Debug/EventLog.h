/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_EVENTLOG_H
#define _PLAYERBOT_EVENTLOG_H

#include <cstdint>

#include "EventTypes.h"
#include "WorldLogTypes.h"

class Action;
class Player;

// The event log's API for the emit sites: the typed emits and the thread-local source scope. Game-free (no writer, no
// formatter), so tests/event-log-selftest.cpp checks the scope the server runs.
namespace EventLog
{
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

// Name of the innermost scope on this thread, or "other"; an action's name stays valid until the next call on the
// same thread (defined in WorldLogWriter.cpp, which knows Action)
char const* CurrentSource();

// Typed events into the world log's event stream (world log on, any player); thread-safe, no I/O.
// source/kind must outlive the call (string literals); the row copies them at once.
// One event (type, source, id, amount and the player's place) to the readers of its type; id: item/quest/spell/skill
// id or 0
void Emit(Player* bot, EventType type, char const* source, uint32_t id = 0, int64_t amount = 0);
// The same by the type's word (the old form, kept for unlanded callers): the word is looked up in EventTypes.h
void Emit(Player* bot, char const* type, char const* source, uint32_t id = 0, int64_t amount = 0);
// The row for a player that need not be on the map (its guid counter and a place it was seen at); no reader but the
// row, not in the bot's recorder ring
void EmitAt(uint32_t bot, WorldLogFormat::Place const& place, EventType type, char const* source, uint32_t id = 0,
            int64_t amount = 0);
void EmitAt(uint32_t bot, WorldLogFormat::Place const& place, char const* type, char const* source, uint32_t id = 0,
            int64_t amount = 0);
// Honesty tag for a shortcut site: the kind's row (type "Honesty" + the kind's word); source names the site. Same row
// format.
void Honesty(Player* bot, HonestyKind kind, char const* source, uint32_t id = 0, int64_t amount = 0);
// The same by the kind's word (the old form, kept for unlanded callers); a word the table lacks is a row only
void Honesty(Player* bot, char const* kind, char const* source, uint32_t id = 0, int64_t amount = 0);
}  // namespace EventLog

#endif
