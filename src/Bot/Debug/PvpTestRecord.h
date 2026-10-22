/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTESTRECORD_H
#define _PLAYERBOT_PVPTESTRECORD_H

#include <array>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common.h"

// One bot of a recorded match: roster facts taken when the arena starts, scores taken from the battleground
struct PvpTestBotRecord
{
    uint64 Guid = 0;
    std::string Name;
    uint8 ClassId = 0;
    uint8 RaceId = 0;
    uint8 Level = 0;
    std::string Spec;
    uint32 GearScore = 0;
    // False when the bot never entered the arena (no score entry); its scores stay zero
    bool Entered = false;
    uint32 Damage = 0;
    uint32 Healing = 0;
    uint32 KillingBlows = 0;
    uint32 Deaths = 0;
    uint32 HonorableKills = 0;
};

struct PvpTestSideRecord
{
    std::string Label;
    char Side = 'A';
    uint32 TeamId = 0;
    std::string Faction;
    std::vector<PvpTestBotRecord> Bots;
};

// One finished (or failed, or stopped) match; every field is written on every record. No MMR or rating: forced
// matches are unrated and pass an MMR of 0.
struct PvpTestMatchRecord
{
    // arena, duel or brawl
    std::string Kind = "arena";
    std::string Id;
    std::string PlanId;
    uint32 Index = 0;
    std::string StartTime;
    uint32 Seed = 0;
    std::string Map;
    uint8 ArenaType = 2;
    bool Rated = false;
    std::string Outcome;
    std::string Winner;
    std::string WinnerSide;
    std::string Reason;
    std::string FailedStage;
    uint32 FightMs = 0;
    uint32 TotalMs = 0;
    std::array<PvpTestSideRecord, 2> Sides;
};

// Appends one JSON line per match to <LogsDir>/pvptest/matches-<YYYYMMDD>.jsonl. The world thread builds the line;
// a writer thread (started with the first record) owns the file and touches nothing but strings.
class PvpTestRecordWriter
{
public:
    static PvpTestRecordWriter& Instance()
    {
        static PvpTestRecordWriter instance;
        return instance;
    }

    ~PvpTestRecordWriter();

    // World thread: builds the line and queues it, no I/O
    void Write(PvpTestMatchRecord const& record);
    // Flushes the queue and joins the writer thread (server shutdown)
    void Shutdown();

    static std::string ToJson(PvpTestMatchRecord const& record);
    static std::string Directory();
    // The file of today's records
    static std::string TodayFile();
    // The current time as 2026-10-23T12:00:00Z
    static std::string IsoTimeNow();
    static std::string ClassName(uint8 classId);

private:
    PvpTestRecordWriter() = default;

    struct Line
    {
        std::string Date;
        std::string Text;
    };

    void WriterLoop(std::string directory);

    // Shared with the writer thread
    std::mutex _mutex;
    std::condition_variable _wake;
    std::vector<Line> _queue;
    bool _stop = false;
    std::thread _writer;
};

#endif
