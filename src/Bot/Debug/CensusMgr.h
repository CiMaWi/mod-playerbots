/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CENSUSMGR_H
#define _PLAYERBOT_CENSUSMGR_H

#include <array>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Common.h"
#include "EventLogFormat.h"
#include "WorldLogFormat.h"

class Player;

// World census: every Playerbots.Census.IntervalSeconds one CSV line per online bot and real player goes to
// <LogsDir>/census/bot_locations.csv (the column layout of spp-map's player_location.csv, three columns appended),
// and one line per bot level-up to <LogsDir>/census/bot_events.csv (spp-map's bot_events.csv). The world thread only
// builds the strings; a writer thread owns the files. Opt-in through Playerbots.Census.Enable. Typed bot events
// (EventLog below) share bot_events.csv; their totals per type go to the log every interval and at shutdown.
// The same writer thread owns the world log's segments (WorldLog.h; on when Playerbots.WorldLog.Dir is set): events of
// every player with positions, a state row per online player every StateSeconds and meta rows, beside the census.
class CensusMgr
{
public:
    static CensusMgr& Instance()
    {
        static CensusMgr instance;
        return instance;
    }

    // World thread, once per world tick; counter adds, and a sample once per interval. Returns at once when both the
    // census and the world log are off.
    void Update(uint32 diff);
    // Level-up (any thread that raises the level): the census line of a bot, the world log's Level row of any player;
    // queued, no I/O
    void OnLevelChanged(Player* player, uint8 oldLevel);
    // Typed event (any thread): the census line of a bot (counted) and the world log's event row of any player, built
    // from the player's own state and queued, no I/O. worldLogId/worldLogAmount replace id/amount in the world-log row
    // unless they are EventLog::SAME_ID/SAME_AMOUNT.
    void OnEvent(Player* bot, std::string_view type, std::string_view source, uint32 id, int64 amount,
                 uint32 worldLogId = EventLog::SAME_ID, int64 worldLogAmount = EventLog::SAME_AMOUNT);
    // Any thread: queues one world-log chunk of rowCount rows, all with times near tMs (the segment is chosen by tMs).
    // A chunk that would take its stream over Playerbots.WorldLog.QueueRows is dropped whole and counted; false then.
    bool EnqueueWorldLog(WorldLogFormat::Stream stream, std::string rows, uint32 rowCount, uint64 tMs);
    // Any thread: the world log's lines for "playerbots worldlog status"
    std::vector<std::string> WorldLogStatus();
    // Flushes the queue, writes the world log's drops and stop rows and joins the writer thread (server shutdown)
    void Shutdown();

private:
    enum class Sink : uint8
    {
        Locations,  // census bot_locations.csv
        Events,     // census bot_events.csv
        WorldLog    // the segment of stream at tMs
    };

    struct Chunk
    {
        Sink sink;
        std::string text;
        WorldLogFormat::Stream stream = WorldLogFormat::Stream::Event;
        uint32 rows = 0;
        uint64 tMs = 0;
    };

    CensusMgr() = default;
    ~CensusMgr();

    void UpdateCensus(uint32 diff);
    void UpdateWorldLog(uint32 diff);
    void Sample();
    void SampleState();
    // countType: the event type the line adds to the totals (empty for location samples)
    void Enqueue(bool events, std::string text, std::string_view countType = {});
    // The world log's drops rows (one per stream); the caller holds _mutex
    void QueueDropsRowsLocked(uint64 tMs);
    static void LogTotals(std::map<std::string, uint64, std::less<>> const& totals);
    void StartWriter(bool worldLog);
    void WriterLoop(std::string censusDir, std::string worldLogDir);

    static std::string TimeString();

    // World thread only
    uint32 _elapsed = 0;
    uint32 _stateElapsed = 0;
    uint32 _dropsElapsed = 0;
    bool _started = false;
    bool _censusStarted = false;
    bool _worldLog = false;

    // Shared with the writer thread
    std::mutex _mutex;
    std::condition_variable _wake;
    std::vector<Chunk> _queue;
    std::map<std::string, uint64, std::less<>> _totals;
    WorldLogFormat::QueueAccount _account;
    // Published by the writer after each batch: rows written per stream, failed writes, the open segments' names
    std::array<uint64, WorldLogFormat::STREAM_COUNT> _written{};
    uint64 _writeErrors = 0;
    std::array<std::string, WorldLogFormat::STREAM_COUNT> _openNames;
    bool _writerRunning = false;
    bool _stop = false;
    std::thread _writer;
};

// Typed bot events in bot_events.csv (census on, bots only) and the world log's event stream (world log on, any
// player); thread-safe, no I/O.
// source/kind must outlive the call (string literals); the line copies them at once. The scope classes are in
// EventLogFormat.h (SourceScope, CurrentSource).
namespace EventLog
{
// One line "time,guid,type,source,id,amount,map,zone"; counts the type. id: item/quest/spell/skill id or 0. The world
// log's event row (any player) takes worldLogId/worldLogAmount instead of id/amount when they are set.
void Emit(Player* bot, char const* type, char const* source, uint32 id = 0, int64 amount = 0,
          uint32 worldLogId = SAME_ID, int64 worldLogAmount = SAME_AMOUNT);
// Honesty tag for a shortcut site: type is "Honesty" + kind; source names the site. Same line format.
void Honesty(Player* bot, char const* kind, char const* source, uint32 id = 0, int64 amount = 0);
}  // namespace EventLog

#endif
