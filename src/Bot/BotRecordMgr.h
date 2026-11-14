/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTRECORDMGR_H
#define PLAYERBOTS_BOTRECORDMGR_H

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Define.h"
#include "RecordRules.h"

class ChatHandler;
class Player;

// Every bot's record (living-conversation LC6, RecordRules.h): the major events of its real play, taken from the world
// log's events whether or not the world log runs. All rows are loaded with one query at startup (world thread); an
// event scores, and a row above the low mark goes into memory under one short lock and to playerbots_bot_record with an
// asynchronous write. No call queries the database after startup; readers get copies.
class BotRecordMgr
{
public:
    static BotRecordMgr& Instance()
    {
        static BotRecordMgr instance;
        return instance;
    }

    // Startup, world thread: loads every living row and the last event id (memory only, with one WARN, when the table
    // is missing); the record takes events from then on
    void Load();
    // Any thread: true once Load ran
    bool Active() const { return _active.load(std::memory_order_acquire); }
    // Any thread (the bot's map thread or the world thread): one world-log event; a bot's major event becomes a row
    // (a kill also for its group's bots at the kill)
    void OnEvent(Player* player, std::string_view type, std::string_view source, uint32 id, int64 amount);
    // The fight's map thread: the row of one participant of an ended fight; all participants share its event id
    void OnFightEnd(Player* player, uint32 fightId, uint32 seconds, std::string_view cause, std::string others);
    // Any thread: a copy of the bot's rows
    std::vector<RecordRules::Row> Rows(uint32 guid);
    // World thread. A retired bot: drops its rows from memory (the retire deletes them)
    void Forget(uint32 guid);

    // .playerbots record [<bot name|guid>]
    static bool HandleRecordCommand(ChatHandler* handler, char const* args);

private:
    // A bot's row for one event, before its id and place
    struct Event
    {
        RecordRules::Kind EventKind = RecordRules::Kind::Kill;
        uint8 Score = 0;
        std::string Facts;
        std::string Others;
        uint64 ShareKey = 0;  // 0: the event is the bot's alone
    };

    BotRecordMgr() = default;

    // participants: the bots whose rows the event writes, all on the calling thread's map
    void Record(std::vector<Player*> const& participants, Event const& event);
    // The caller holds _lock; returns the statements to run after it is released
    void InsertLocked(uint32 guid, RecordRules::Row row, uint32 now, std::vector<std::string>& statements);

    std::mutex _lock;
    std::unordered_map<uint32, std::vector<RecordRules::Row>> _rows;
    RecordRules::ShareBook _share;
    uint32 _lastEventId = 0;
    std::atomic<bool> _active = false;
    // playerbots_bot_record exists (update 2026_11_14_00); without it the record lives in memory only
    bool _table = false;
};

#endif
