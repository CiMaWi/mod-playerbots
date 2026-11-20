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

    // Startup, world thread: loads every living row and the last event id; the record takes events from then on
    void Load();
    // Any thread: true once Load ran
    bool Active() const { return _active.load(std::memory_order_acquire); }
    // Any thread (the bot's map thread or the world thread): one world-log event; a bot's major event becomes a row
    // (a kill also for its group's bots at the kill)
    void OnEvent(Player* player, std::string_view type, std::string_view source, uint32 id, int64 amount);
    // The bot's map thread, at its combat edge (LC6b): a bot in combat with enemy players notes their fight (not in a
    // battleground or arena, not a duel)
    void OnCombatEnter(Player* player);
    // The bot's map thread, at its combat end: the bot's fight row once; at a logout its fight ends without a row
    void OnCombatLeave(Player* player, bool logout);
    // The fight's map thread: the row of one participant of an ended fight; all participants share its event id
    void OnFightEnd(Player* player, uint64 fightId, uint32 seconds, std::string_view cause, std::string others);
    // The bot's map thread, at every zone entry (a zone change, the arrival on a new map, the login; LC18e): its Visit
    // rows, one hash lookup per place and one write each for a new row or a moved time
    void OnZoneEnter(Player* player, uint32 zone, uint32 area);
    // Any thread: a copy of the bot's rows (its deeds; the Visit rows are kept apart)
    std::vector<RecordRules::Row> Rows(uint32 guid);
    // Any thread: a copy of the bot's Visit rows, in no order
    std::vector<RecordRules::Row> Visits(uint32 guid);
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
    // The caller holds _lock; returns the statements to run after it is released, false when the bot holds the row
    // already (a bot back in the same fight within SHARE_WINDOW)
    bool InsertLocked(uint32 guid, RecordRules::Row row, uint32 now, std::vector<std::string>& statements);

    std::mutex _lock;
    std::unordered_map<uint32, std::vector<RecordRules::Row>> _rows;
    std::unordered_map<uint32, RecordRules::VisitBook> _visits;
    RecordRules::ShareBook _share;
    RecordRules::FightNotes _fights;
    // The bots with an open fight: a combat end with none skips the lock
    std::atomic<std::size_t> _openFights = 0;
    uint32 _lastEventId = 0;
    std::atomic<bool> _active = false;
};

#endif
