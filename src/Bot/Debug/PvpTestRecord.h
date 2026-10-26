/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTESTRECORD_H
#define _PLAYERBOT_PVPTESTRECORD_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
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
    // Hidden skill 1-99 (Playerbots.Skill.Enable); -1 = none, written as null
    int32 Skill = -1;
    // False when the bot never entered the arena (no score entry); its scores stay zero
    bool Entered = false;
    uint32 Damage = 0;
    uint32 Healing = 0;
    uint32 KillingBlows = 0;
    uint32 Deaths = 0;
    uint32 HonorableKills = 0;
    // Executed casts by skill category while the match ran: interrupt, defensive, dispel, crowd control, cc break,
    // target switch (the categories of Playerbots.Skill.Notice*)
    std::array<uint32, 6> Casts{};
    // M9 (additive): a real (non-bot) player; honor from the battleground score; the personal arena rating before and
    // after the match (-1 = none, written as null); the matchmaker rating change of the member (only when known)
    bool Real = false;
    uint32 Honor = 0;
    int32 PersonalBefore = -1;
    int32 PersonalAfter = -1;
    bool MmrChangeKnown = false;
    int32 MmrChange = 0;
};

struct PvpTestSideRecord
{
    std::string Label;
    char Side = 'A';
    uint32 TeamId = 0;
    std::string Faction;
    // M9 (additive): the arena team's name and its team rating and matchmaker rating before and after the match
    // (-1 = none, written as null; unrated matches have none)
    std::string TeamName;
    int32 TeamRatingBefore = -1;
    int32 TeamRatingAfter = -1;
    int32 MmrBefore = -1;
    int32 MmrAfter = -1;
    std::vector<PvpTestBotRecord> Bots;
};

// One finished (or failed, or stopped) match; every field is written on every record. Forced matches are unrated unless
// asked otherwise; the ratings are null for them. Since M9 every arena match on every server writes a record.
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
    // M9 (additive): the server (Playerbots.Ladder.ServerId, "main" when empty), the match id <server>-<ts>-<n>, the
    // origin (organic, harness or player), whether a real player took part, whether the ratings are known
    std::string Server;
    std::string MatchId;
    std::string Source = "harness";
    bool Real = false;
    bool RatingsKnown = false;
    std::array<PvpTestSideRecord, 2> Sides;
};

// Appends one JSON line per match to <LogsDir>/pvptest/matches-<YYYYMMDD>.jsonl. The world thread builds the line;
// a writer thread (started with the first record) owns the file and touches nothing but strings. WriteLine is the
// generic part (M10): any line for <LogsDir>/pvptest/<prefix>-<YYYYMMDD>.jsonl, e.g. the match traces.
class PvpTestRecordWriter
{
public:
    static PvpTestRecordWriter& Instance()
    {
        static PvpTestRecordWriter instance;
        return instance;
    }

    ~PvpTestRecordWriter();

    // Any thread (since M9 the arena map threads too): builds the line and queues it, no I/O
    void Write(PvpTestMatchRecord const& record);
    // Any thread: queues one line (text ends with a newline) for <prefix>-<date>.jsonl, no I/O. With make, the text is
    // made by make on the writer thread instead (a big line from plain values; make owns them and touches no game
    // object)
    void WriteLine(std::string prefix, std::string date, std::string text, std::function<std::string()> make = {});
    // Flushes the queue and joins the writer thread (server shutdown)
    void Shutdown();
    // Any thread: records waiting for the writer, the most that ever waited, written to the file, lost
    size_t QueueSize();
    size_t QueueHighWater() const { return _queueHigh.load(); }
    uint64 Written() const { return _written.load(); }
    uint64 Failed() const { return _failed.load(); }

    static std::string ToJson(PvpTestMatchRecord const& record);
    static std::string Directory();
    // The file of today's records
    static std::string TodayFile();
    // The current time as 2026-10-23T12:00:00Z
    static std::string IsoTimeNow();
    static std::string ClassName(uint8 classId);
    // A JSON string literal of text (quotes, backslashes and control characters escaped)
    static std::string JsonQuote(std::string const& text);

private:
    PvpTestRecordWriter() = default;

    struct Line
    {
        std::string Prefix;
        std::string Date;
        std::string Text;
        std::function<std::string()> Make;
    };

    // Appends a line to its file; _fileMutex is held
    bool Append(std::string const& directory, Line& line);

    void WriterLoop(std::string directory);

    // Shared with the writer thread
    std::mutex _mutex;
    // Held while the file is appended to: by the writer thread for a batch, by Write for a record after the stop
    std::mutex _fileMutex;
    std::condition_variable _wake;
    std::vector<Line> _queue;
    bool _stop = false;
    std::thread _writer;
    std::atomic<size_t> _queueHigh{0};
    std::atomic<uint64> _written{0};
    std::atomic<uint64> _failed{0};
};

#endif
