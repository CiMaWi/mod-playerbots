/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MATCHTRACE_H
#define _PLAYERBOT_MATCHTRACE_H

#include <string>
#include <vector>

#include "Common.h"
#include "ObjectGuid.h"

class ChatHandler;
class Player;
struct PvpTestMatchRecord;

// The kinds of a trace row. The names follow the natural tracer of G0e (cast start, cast finish, cancel, ...), so its
// 100 ms sampler can write "pos" rows of the same form into the same kind of line.
enum class TraceKind : uint8
{
    Pos,
    CastStart,
    CastFinish,
    Cancel,
    CcOn,
    CcOff,
    Action,
    Death
};

// One event of a trace: plain values only (no strings, no game objects), formatted on the writer thread. Guid and
// Other are player guid counters (0 = none); Other is the target, the caster of a CC or the killer (the owner of a pet
// or totem).
struct TraceRow
{
    // Milliseconds since the gates opened (the natural tracer: since its sampler started)
    uint32 Ms = 0;
    TraceKind Kind = TraceKind::Pos;
    // Pos: health %; Cancel: 1 = cancelled by the caster; CcOn/CcOff: the mechanic; Action: the skill category
    uint8 Extra = 0;
    // CcOff: the AuraRemoveMode
    uint8 Extra2 = 0;
    uint32 Spell = 0;
    uint64 Guid = 0;
    uint64 Other = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float O = 0.0f;
};

// The JSON object of one row, e.g. {"t":1000,"k":"pos","u":12,"x":1.5,...}
std::string TraceRowJson(TraceRow const& row);

struct TraceUnit
{
    uint64 Guid = 0;
    std::string Name;
    char Side = 'A';
    uint8 ClassId = 0;
};

// Playerbots.MatchLog.TraceEvery (with Playerbots.MatchLog.Enable): one arena match in N, chosen by the FNV-1a hash of
// its match id, keeps a trace: the casts (start, finish, cancel) of the participants, CC applied and removed, deaths,
// the executed actions of the skill categories and the positions every second. At the match end the trace is one JSON
// line in <LogsDir>/pvptest/traces-<date>.jsonl, keyed by the match id of its record in matches-<date>.jsonl.
// The buffer of a match is pushed to on its arena map's thread; the registry of the running traces (instance id ->
// trace) is shared and guarded by one mutex, which the hooks take only while a trace runs. The line is formatted on
// the writer thread. Caps per match: Playerbots.MatchLog.TraceMaxEvents and TraceMaxBytes; a cut trace keeps its
// deaths (a reserve) and ends with a "truncated" row.
class MatchTrace
{
public:
    MatchTrace(std::string matchId, uint32 every, uint32 maxEvents, uint32 maxBytes);

    // Appends a row; false when the caps dropped it. Deaths may use a reserve the other rows cannot take; once the
    // trace is cut, only deaths are kept
    bool Push(TraceRow const& row);
    // The match's facts from its record (roster, map, winner) and the time of the end since the gates opened
    void Finish(PvpTestMatchRecord const& record, uint32 endMs);
    // The JSON line of the match, without the newline
    std::string ToJson() const;

    std::string const& MatchId() const { return _matchId; }
    std::string const& StartTime() const { return _time; }
    std::vector<TraceRow> const& Rows() const { return _rows; }
    uint32 Dropped() const { return _dropped; }
    bool Truncated() const { return _truncated; }
    size_t EstimatedBytes() const { return _bytes; }

    // FNV-1a, 32 bit: stable across builds and platforms (std::hash is not)
    static uint32 Hash(std::string const& text);
    // every > 0 and Hash(matchId) % every == 0
    static bool Selected(std::string const& matchId, uint32 every);

    // Map thread, from MatchLog::OnArenaStart: a trace for this arena when Playerbots.MatchLog.TraceEvery selects it
    static void Begin(uint32 instanceId, std::string const& matchId);
    // From MatchLog::OnEnd: closes the trace with the record's facts. The next update of the arena's map queues it to
    // the writer: the core ends an arena inside the killing blow, before the death hook of that blow has run.
    static void End(uint32 instanceId, PvpTestMatchRecord const& record);
    // Map thread, from PlayerbotAI::EndExecutedCount: an executed action of a skill category (one test when no trace
    // runs)
    static void OnAction(Player* bot, uint8 category, ObjectGuid target);

    // The traces line of .playerbots matchlog status
    static std::string StatusText();
    static bool HandleSelftestCommand(ChatHandler* handler, char const* args);

private:
    std::string _matchId;
    std::string _server;
    std::string _time;
    std::string _map;
    std::string _winnerSide;
    uint8 _arenaType = 0;
    bool _rated = false;
    uint32 _every = 0;
    uint32 _maxEvents = 0;
    uint32 _maxBytes = 0;
    uint32 _reserve = 0;
    uint32 _endMs = 0;
    std::vector<TraceUnit> _units;
    std::vector<TraceRow> _rows;
    size_t _bytes = 0;
    uint32 _dropped = 0;
    bool _truncated = false;
    char const* _reason = "";
};

#endif
