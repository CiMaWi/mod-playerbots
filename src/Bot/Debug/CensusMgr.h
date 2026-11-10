/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CENSUSMGR_H
#define _PLAYERBOT_CENSUSMGR_H

#include <array>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Common.h"
#include "EventLogFormat.h"
#include "WorldLogFormat.h"

class Player;

// The world log's writer (WorldLog.h; on when Playerbots.WorldLog.Dir is set): events of every player with positions,
// a state row per online player every StateSeconds (real players and the bots near them every FOCUS_SECONDS),
// decision, recorder and meta rows. The world thread and the hooks
// only build the strings and queue them; one writer thread owns the segment files. (The class keeps its old name.)
class CensusMgr
{
public:
    static CensusMgr& Instance()
    {
        static CensusMgr instance;
        return instance;
    }

    // World thread, once per world tick; counter adds, a state pass once per StateSeconds and a focus pass every
    // FOCUS_SECONDS between. Returns at once when the world log is off.
    void Update(uint32 diff);
    // Level-up (any thread that raises the level): the world log's Level row of any player; queued, no I/O
    void OnLevelChanged(Player* player, uint8 oldLevel);
    // Typed event (any thread): the world log's event row of any player, built from the player's own state and queued,
    // no I/O
    void OnEvent(Player* bot, std::string_view type, std::string_view source, uint32 id, int64 amount);
    // Any thread: queues one world-log chunk of rowCount rows, all with times near tMs (the segment is chosen by tMs).
    // A chunk that would take its stream over Playerbots.WorldLog.QueueRows is dropped whole and counted; false then.
    bool EnqueueWorldLog(WorldLogFormat::Stream stream, std::string rows, uint32 rowCount, uint64 tMs);
    // Any thread: the world log's lines for "playerbots worldlog status"
    std::vector<std::string> WorldLogStatus();
    // Flushes the queue, writes the world log's drops and stop rows and joins the writer thread (server shutdown)
    void Shutdown();

private:
    // Rows for the segment of stream at tMs
    struct Chunk
    {
        std::string text;
        WorldLogFormat::Stream stream = WorldLogFormat::Stream::Event;
        uint32 rows = 0;
        uint64 tMs = 0;
    };

    CensusMgr() = default;
    ~CensusMgr();

    void UpdateWorldLog(uint32 diff);
    // focusOnly: only the real players and the bots within FOCUS_YARDS of one
    void SampleState(bool focusOnly);
    // The world log's drops rows (one per stream); the caller holds _mutex
    void QueueDropsRowsLocked(uint64 tMs);
    void StartWriter();
    void WriterLoop(std::string dir);

    // World thread only
    uint32 _stateElapsed = 0;
    uint32 _focusElapsed = 0;
    std::vector<WorldLogFormat::FocusPoint> _focusPoints;  // the real players' spots of a focus pass, reused
    uint32 _dropsElapsed = 0;
    bool _started = false;

    // Shared with the writer thread
    std::mutex _mutex;
    std::condition_variable _wake;
    std::vector<Chunk> _queue;
    WorldLogFormat::QueueAccount _account;
    // Published by the writer after each batch: rows written per stream, failed writes, the open segments' names
    std::array<uint64, WorldLogFormat::STREAM_COUNT> _written{};
    uint64 _writeErrors = 0;
    std::array<std::string, WorldLogFormat::STREAM_COUNT> _openNames;
    bool _writerRunning = false;
    bool _stop = false;
    std::thread _writer;
};

// Typed events into the world log's event stream (world log on, any player); thread-safe, no I/O.
// source/kind must outlive the call (string literals); the row copies them at once. The scope classes are in
// EventLogFormat.h (SourceScope, CurrentSource).
namespace EventLog
{
// One event row (type, source, id, amount and the player's place); id: item/quest/spell/skill id or 0
void Emit(Player* bot, char const* type, char const* source, uint32 id = 0, int64 amount = 0);
// Honesty tag for a shortcut site: type is "Honesty" + kind; source names the site. Same row format.
void Honesty(Player* bot, char const* kind, char const* source, uint32 id = 0, int64 amount = 0);
}  // namespace EventLog

#endif
