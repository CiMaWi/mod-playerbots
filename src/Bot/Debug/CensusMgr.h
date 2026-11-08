/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CENSUSMGR_H
#define _PLAYERBOT_CENSUSMGR_H

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

class Player;

// World census: every Playerbots.Census.IntervalSeconds one CSV line per online bot and real player goes to
// <LogsDir>/census/bot_locations.csv (the column layout of spp-map's player_location.csv, three columns appended),
// and one line per bot level-up to <LogsDir>/census/bot_events.csv (spp-map's bot_events.csv). The world thread only
// builds the strings; a writer thread owns the files. Opt-in through Playerbots.Census.Enable. Typed bot events
// (EventLog below) share bot_events.csv; their totals per type go to the log every interval and at shutdown.
class CensusMgr
{
public:
    static CensusMgr& Instance()
    {
        static CensusMgr instance;
        return instance;
    }

    // World thread, once per world tick; one counter add, and a sample once per interval. Returns at once when the
    // census is off.
    void Update(uint32 diff);
    // Level-up of a bot (any thread that raises the level); builds one line and queues it, no I/O
    void OnLevelChanged(Player* player, uint8 oldLevel);
    // Typed event of a bot (any thread): one line built from the bot's own state, queued and counted, no I/O
    void OnEvent(Player* bot, std::string_view type, std::string_view source, uint32 id, int64 amount);
    // Flushes the queue and joins the writer thread (server shutdown)
    void Shutdown();

private:
    struct Chunk
    {
        bool events;
        std::string text;
    };

    CensusMgr() = default;
    ~CensusMgr();

    void Sample();
    // countType: the event type the line adds to the totals (empty for location samples)
    void Enqueue(bool events, std::string text, std::string_view countType = {});
    static void LogTotals(std::map<std::string, uint64, std::less<>> const& totals);
    void StartWriter();
    void WriterLoop(std::string dir);

    static std::string TimeString();

    // World thread only
    uint32 _elapsed = 0;
    bool _started = false;

    // Shared with the writer thread
    std::mutex _mutex;
    std::condition_variable _wake;
    std::vector<Chunk> _queue;
    std::map<std::string, uint64, std::less<>> _totals;
    bool _stop = false;
    std::thread _writer;
};

// Typed bot events in bot_events.csv; thread-safe, no I/O, no-ops unless the census is on and the player is a bot.
// source/kind must outlive the call (string literals); the line copies them at once. The scope classes are in
// EventLogFormat.h (SourceScope, CurrentSource).
namespace EventLog
{
// One line "time,guid,type,source,id,amount,map,zone"; counts the type. id: item/quest/spell/skill id or 0.
void Emit(Player* bot, char const* type, char const* source, uint32 id = 0, int64 amount = 0);
// Honesty tag for a shortcut site: type is "Honesty" + kind; source names the site. Same line format.
void Honesty(Player* bot, char const* kind, char const* source, uint32 id = 0, int64 amount = 0);
}  // namespace EventLog

#endif
