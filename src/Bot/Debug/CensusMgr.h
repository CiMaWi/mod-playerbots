/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CENSUSMGR_H
#define _PLAYERBOT_CENSUSMGR_H

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common.h"

class Player;

// World census: every Playerbots.Census.IntervalSeconds one CSV line per online bot and real player goes to
// <LogsDir>/census/bot_locations.csv (the column layout of spp-map's player_location.csv, three columns appended),
// and one line per bot level-up to <LogsDir>/census/bot_events.csv (spp-map's bot_events.csv). The world thread only
// builds the strings; a writer thread owns the files. Opt-in through Playerbots.Census.Enable.
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
    void Enqueue(bool events, std::string text);
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
    bool _stop = false;
    std::thread _writer;
};

#endif
