/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGWRITER_H
#define _PLAYERBOT_WORLDLOGWRITER_H

#include <array>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Common.h"
#include "EventLog.h"
#include "ObjectGuid.h"
#include "WorldLogFormat.h"

class Player;

// The world log's writer (WorldLog.h; on when Playerbots.WorldLog.Dir is set): events of every player with positions,
// a state row per online player every StateSeconds (real players and the bots near them every FOCUS_SECONDS),
// decision, recorder and meta rows. The world thread and the hooks
// only build the strings and queue them; one writer thread owns the segment files and wakes every WRITER_WAKE_MS to
// write what waits (WL23).
class WorldLogWriter
{
public:
    static WorldLogWriter& Instance()
    {
        static WorldLogWriter instance;
        return instance;
    }

    // World thread, once per world tick; counter adds, a state pass once per StateSeconds and a focus pass every
    // FOCUS_SECONDS between, each spread over ticks at STATE_ROWS_PER_TICK rows (WL23). Returns at once when the world
    // log is off.
    void Update(uint32 diff);
    // Level-up (any thread that raises the level): the world log's Level row of any player; queued, no I/O
    void OnLevelChanged(Player* player, uint8 oldLevel);
    // An event by its type (any thread): the world log's event row of any player, built from the player's own
    // state and queued, no I/O, and the readers of its type (EventTypes.h)
    void OnEvent(Player* bot, EventLog::EventType type, std::string_view source, uint32 id, int64 amount);
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

    WorldLogWriter() = default;
    ~WorldLogWriter();

    void UpdateWorldLog(uint32 diff);
    // A pass takes the guids of the players it will write (focusOnly: only the real players and the bots within
    // FOCUS_YARDS of one) and writes its first slice; ContinueStatePass writes the next slice on each later tick
    void StartStatePass(bool focusOnly);
    void ContinueStatePass();
    // The world log's drops rows (one per stream); the caller holds _mutex
    void QueueDropsRowsLocked(uint64 tMs);
    void StartWriter();
    void WriterLoop(std::string dir);

    // World thread only
    uint32 _stateElapsed = 0;
    uint32 _focusElapsed = 0;
    std::vector<WorldLogFormat::FocusPoint> _focusPoints;  // the real players' spots of a focus pass, reused
    std::vector<ObjectGuid> _passGuids;                    // the running pass's players; empty when none runs
    size_t _passNext = 0;                                  // the next of them to write
    bool _passFocus = false;
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

#endif
