/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalTrace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Battleground.h"
#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "MatchLog.h"
#include "MatchTrace.h"
#include "MoveSpline.h"
#include "MovementHandlerScript.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestRecord.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"

namespace
{
// Lock-free marks by guid counter: a hook takes the registry lock only for a unit whose bucket is marked (at most
// MaxUnits units in 4096 buckets, so an untraced unit almost never gets past this test)
constexpr uint32 MarkBuckets = 4096;
// A unit's rows go to the writer in batches of this many (2 s of pos rows), and at the end of its trace
constexpr size_t FlushRows = 20;
constexpr size_t DateLength = 10;
constexpr float PercentRound = 0.5f;

// Ms since a start; modular, so exact across the wrap of getMSTime (getMSTimeDiff is one short there)
uint32 Elapsed(uint32 start, uint32 now) { return now - start; }

enum class RowKind : uint8
{
    Start,
    Pos,
    Target,
    CastStart,
    CastFinish,
    Cancel,
    Interrupt,
    Move,
    Truncated,
    End
};

char const* KindName(RowKind kind)
{
    switch (kind)
    {
        case RowKind::Start:
            return "start";
        case RowKind::Pos:
            return "pos";
        case RowKind::Target:
            return "target";
        case RowKind::CastStart:
            return "cast_start";
        case RowKind::CastFinish:
            return "cast_finish";
        case RowKind::Cancel:
            return "cancel";
        case RowKind::Interrupt:
            return "interrupt";
        case RowKind::Move:
            return "mv";
        case RowKind::Truncated:
            return "truncated";
        case RowKind::End:
            return "end";
    }

    return "unknown";
}

// One row: plain values only, formatted on the writer thread. The fields by kind:
//   start: Source, Side, Spell = class id, Flag = a bot, Extra = the interval in ms
//   pos: X Y Z O, MoveFlags, Moving, Spline, Speed (run), Target, Victim, Health, Spell = the current cast (channel
//        first, 0 = none), CastState, Extra = elapsed ms of a cast (-1 = none, a channel), Extra2 = remaining ms
//   target: Target (0 = none); cast_start: Spell, Target, Extra = cast time ms; cast_finish: Spell, Target
//   cancel: Spell, Flag = by the caster; interrupt: Spell; mv: Extra = opcode, MoveFlags, X Y Z O
//   truncated: no fields; end: Extra = rows kept, Extra2 = rows dropped
struct Row
{
    uint32 Ms = 0;
    RowKind Kind = RowKind::Pos;
    uint8 Health = 0;
    uint8 CastState = 0;
    char Side = 0;
    bool Moving = false;
    bool Spline = false;
    bool Flag = false;
    uint32 MoveFlags = 0;
    uint32 Spell = 0;
    int32 Extra = 0;
    int32 Extra2 = 0;
    uint64 Guid = 0;
    uint64 Target = 0;
    uint64 Victim = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float O = 0.0f;
    float Speed = 0.0f;
    char const* Source = "";
};

// Where the rows of a trace go, made at its start on the map thread and shared by its batches. JSONL (G0e): the file
// <Directory>trace-<Date>.jsonl. Binary (G0i): the file <Directory><File>, Directory = <root>/<instance>/<YYYY-MM-DD>/;
// Instance and StartUnixMs go into the header of a new file. The units of one binary match share the place of the
// match's first start (Tracer::Insert), so its day folder and header fields are fixed per match.
struct Place
{
    std::string Directory;
    std::string Date;
    bool Binary = false;
    std::string File;
    std::string Instance;
    uint64 StartUnixMs = 0;
};

using PlacePtr = std::shared_ptr<Place const>;

// The rows of one unit on their way to the file, with the unit's start source and interval (the header fields of a
// binary file this batch creates)
struct Batch
{
    std::string MatchId;
    PlacePtr Where;
    char const* Source = "";
    uint32 Interval = 0;
    std::vector<Row> Rows;
};

struct UnitState
{
    std::string MatchId;
    PlacePtr Where;
    char const* Source = "";
    uint32 StartMs = 0;
    uint32 Interval = 100;
    uint32 MaxRows = 0;
    // Ms since the start at which the next pos slot opens (a missed slot is not back-filled)
    uint32 NextDue = 0;
    // The last row's ms: rows of a unit never go back in time
    uint32 LastMs = 0;
    uint64 LastTarget = 0;
    uint32 Rows = 0;
    uint32 Dropped = 0;
    bool Truncated = false;
    std::vector<Row> Buffer;
};

enum class AddResult : uint8
{
    Added,
    Exists,
    Full
};

// The registry of traced units and their buffers. One instance is live (the hooks'), the selftest makes its own with
// a fake clock and a sink that keeps the batches.
class Tracer
{
public:
    using Sink = std::function<void(Batch&&)>;
    using ClockFn = std::function<uint32()>;

    Tracer(std::atomic<uint32>& live, Sink sink, ClockFn clock)
        : _live(live), _sink(std::move(sink)), _clock(std::move(clock))
    {
    }

    // Lock-free: false for a unit that is certainly not traced
    bool Marked(uint64 guid) const { return _marks[guid % MarkBuckets].load(std::memory_order_relaxed) != 0; }

    AddResult Add(uint64 guid, std::string matchId, PlacePtr where, Row start, uint32 maxUnits, uint32 interval,
                  uint32 maxRows)
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_units.count(guid))
            return AddResult::Exists;

        if (_units.size() >= maxUnits)
        {
            ++Refused;
            return AddResult::Full;
        }

        Insert(guid, std::move(matchId), std::move(where), start, interval, maxRows);
        return AddResult::Added;
    }

    // All of a group (the players of an organic match, each with its start row) or none, under one lock: Exists
    // when one of them is traced already, Full when they do not all fit under maxUnits (all counted as refused)
    AddResult AddGroup(std::vector<std::pair<uint64, Row>> members, std::string const& matchId, PlacePtr const& where,
                       uint32 maxUnits, uint32 interval, uint32 maxRows)
    {
        std::lock_guard<std::mutex> guard(_lock);
        for (auto const& member : members)
        {
            if (_units.count(member.first))
            {
                Refused += members.size();
                return AddResult::Exists;
            }
        }

        if (members.empty() || _units.size() + members.size() > maxUnits)
        {
            Refused += members.size();
            return AddResult::Full;
        }

        for (auto& member : members)
            Insert(member.first, matchId, where, member.second, interval, maxRows);

        return AddResult::Added;
    }

    // The sample of a traced unit: a target row when the target changed, a pos row (read by read) when the unit's
    // next slot is due. False when the unit is not traced.
    template <typename Read>
    bool Offer(uint64 guid, uint64 target, Read&& read)
    {
        Batch batch;
        {
            std::lock_guard<std::mutex> guard(_lock);
            auto const found = _units.find(guid);
            if (found == _units.end())
                return false;

            UnitState& state = found->second;
            uint32 const elapsed = Elapsed(state.StartMs, _clock());
            if (target != state.LastTarget)
            {
                state.LastTarget = target;
                Row row;
                row.Kind = RowKind::Target;
                row.Target = target;
                if (Push(guid, state, row, elapsed))
                    ++EventRows;
            }

            if (elapsed >= state.NextDue)
            {
                state.NextDue = (elapsed / state.Interval + 1) * state.Interval;
                Row row;
                row.Kind = RowKind::Pos;
                read(row);
                row.Target = target;
                if (Push(guid, state, row, elapsed))
                    ++PosRows;
            }

            TakeIfFull(state, batch);
        }

        if (!batch.Rows.empty())
            _sink(std::move(batch));

        return true;
    }

    // An event row of a traced unit (any thread: the time is read under the lock)
    void Event(uint64 guid, Row row)
    {
        Batch batch;
        {
            std::lock_guard<std::mutex> guard(_lock);
            auto const found = _units.find(guid);
            if (found == _units.end())
                return;

            UnitState& state = found->second;
            bool const move = row.Kind == RowKind::Move;
            if (Push(guid, state, row, Elapsed(state.StartMs, _clock())))
                ++(move ? MoveRows : EventRows);

            TakeIfFull(state, batch);
        }

        if (!batch.Rows.empty())
            _sink(std::move(batch));
    }

    bool Remove(uint64 guid)
    {
        std::vector<Batch> batches;
        {
            std::lock_guard<std::mutex> guard(_lock);
            auto const found = _units.find(guid);
            if (found == _units.end())
                return false;

            Close(found, batches);
        }

        Send(batches);
        return true;
    }

    // The traces of a match end (an empty id: none); the number ended
    uint32 EndMatch(std::string const& matchId)
    {
        if (matchId.empty())
            return 0;

        std::vector<Batch> batches;
        {
            std::lock_guard<std::mutex> guard(_lock);
            for (auto itr = _units.begin(); itr != _units.end();)
            {
                if (itr->second.MatchId == matchId)
                    itr = Close(itr, batches);
                else
                    ++itr;
            }
        }

        Send(batches);
        return static_cast<uint32>(batches.size());
    }

    uint32 Clear()
    {
        std::vector<Batch> batches;
        {
            std::lock_guard<std::mutex> guard(_lock);
            for (auto itr = _units.begin(); itr != _units.end();)
                itr = Close(itr, batches);
        }

        Send(batches);
        return static_cast<uint32>(batches.size());
    }

    size_t Count()
    {
        std::lock_guard<std::mutex> guard(_lock);
        return _units.size();
    }

    std::atomic<uint64> Started{0};
    std::atomic<uint64> Ended{0};
    std::atomic<uint64> PosRows{0};
    std::atomic<uint64> EventRows{0};
    std::atomic<uint64> MoveRows{0};
    std::atomic<uint64> Refused{0};
    std::atomic<uint64> TruncatedUnits{0};
    std::atomic<uint64> DroppedRows{0};

    // The binary matches with a traced unit (their shared places)
    size_t Matches()
    {
        std::lock_guard<std::mutex> guard(_lock);
        return _matches.size();
    }

private:
    using Units = std::unordered_map<uint64, UnitState>;

    // A binary match's place and its traced units
    struct MatchPlace
    {
        PlacePtr Where;
        uint32 Units = 0;
    };

    // The lock is held and the unit is not traced: its state, start row, mark and count
    void Insert(uint64 guid, std::string matchId, PlacePtr where, Row& start, uint32 interval, uint32 maxRows)
    {
        UnitState& state = _units[guid];
        // One binary file per match: a unit of a match with a traced unit takes the match's place (day, header)
        auto const match = matchId.empty() ? _matches.end() : _matches.find(matchId);
        if (match != _matches.end())
        {
            ++match->second.Units;
            state.Where = match->second.Where;
        }
        else
        {
            if (where->Binary && !matchId.empty())
                _matches[matchId] = MatchPlace{where, 1};

            state.Where = std::move(where);
        }

        state.MatchId = std::move(matchId);
        state.Source = start.Source;
        state.StartMs = _clock();
        state.Interval = std::max<uint32>(interval, 1);
        state.MaxRows = maxRows;
        state.Buffer.reserve(FlushRows + 2);
        start.Kind = RowKind::Start;
        start.Extra = static_cast<int32>(state.Interval);
        Push(guid, state, start, 0);
        _marks[guid % MarkBuckets].fetch_add(1, std::memory_order_relaxed);
        _live.store(static_cast<uint32>(_units.size()), std::memory_order_relaxed);
        ++Started;
    }

    // The lock is held: stamps and appends a row; false when the unit's row cap dropped it (the first dropped row
    // leaves a "truncated" row instead)
    bool Push(uint64 guid, UnitState& state, Row& row, uint32 elapsed)
    {
        row.Guid = guid;
        row.Ms = std::max(elapsed, state.LastMs);
        state.LastMs = row.Ms;
        if (state.Truncated || state.Rows >= state.MaxRows)
        {
            ++state.Dropped;
            ++DroppedRows;
            if (!state.Truncated)
            {
                state.Truncated = true;
                ++TruncatedUnits;
                row.Kind = RowKind::Truncated;
                state.Buffer.push_back(row);
            }

            return false;
        }

        state.Buffer.push_back(row);
        ++state.Rows;
        return true;
    }

    // The lock is held: a full buffer leaves for the writer once the lock is released
    static void TakeIfFull(UnitState& state, Batch& batch)
    {
        if (state.Buffer.size() < FlushRows)
            return;

        batch.MatchId = state.MatchId;
        batch.Where = state.Where;
        batch.Source = state.Source;
        batch.Interval = state.Interval;
        batch.Rows.swap(state.Buffer);
        state.Buffer.reserve(FlushRows + 2);
    }

    // The lock is held: the end row (never dropped), the last batch, the mark; returns the next entry
    Units::iterator Close(Units::iterator itr, std::vector<Batch>& batches)
    {
        UnitState& state = itr->second;
        Row end;
        end.Kind = RowKind::End;
        end.Guid = itr->first;
        end.Ms = std::max(Elapsed(state.StartMs, _clock()), state.LastMs);
        end.Extra = static_cast<int32>(state.Rows);
        end.Extra2 = static_cast<int32>(state.Dropped);
        state.Buffer.push_back(end);
        if (!state.MatchId.empty())
        {
            auto const match = _matches.find(state.MatchId);
            if (match != _matches.end() && match->second.Where == state.Where && !--match->second.Units)
                _matches.erase(match);
        }

        Batch batch;
        batch.MatchId = std::move(state.MatchId);
        batch.Where = std::move(state.Where);
        batch.Source = state.Source;
        batch.Interval = state.Interval;
        batch.Rows = std::move(state.Buffer);
        batches.push_back(std::move(batch));
        _marks[itr->first % MarkBuckets].fetch_sub(1, std::memory_order_relaxed);
        itr = _units.erase(itr);
        _live.store(static_cast<uint32>(_units.size()), std::memory_order_relaxed);
        ++Ended;
        return itr;
    }

    void Send(std::vector<Batch>& batches)
    {
        for (Batch& batch : batches)
            _sink(std::move(batch));
    }

    std::atomic<uint32>& _live;
    Sink _sink;
    ClockFn _clock;
    std::mutex _lock;
    Units _units;
    std::unordered_map<std::string, MatchPlace> _matches;
    std::array<std::atomic<uint16>, MarkBuckets> _marks{};
};

std::string Number(int32 value) { return value < 0 ? std::string("null") : std::to_string(value); }

std::string Bool(bool value) { return value ? "true" : "false"; }

// The JSON object of a row; matchJson is the quoted match id
std::string RowJson(Row const& row, std::string const& matchJson)
{
    std::string json = Acore::StringFormat("{{\"t\":{},\"k\":\"{}\",\"u\":{},\"m\":{}", row.Ms, KindName(row.Kind),
                                           row.Guid, matchJson);
    switch (row.Kind)
    {
        case RowKind::Start:
            json += Acore::StringFormat(",\"src\":\"{}\",\"side\":{},\"cls\":{},\"bot\":{},\"iv\":{}", row.Source,
                                        row.Side ? "\"" + std::string(1, row.Side) + "\"" : std::string("null"),
                                        row.Spell, Bool(row.Flag), row.Extra);
            break;
        case RowKind::Pos:
            json += Acore::StringFormat(
                ",\"x\":{:.2f},\"y\":{:.2f},\"z\":{:.2f},\"o\":{:.3f},\"mf\":{},\"mov\":{},\"spl\":{},\"run\":{:.2f},"
                "\"tg\":{},\"vi\":{},\"hp\":{},\"cs\":{},\"st\":{},\"ce\":{},\"cr\":{}",
                row.X, row.Y, row.Z, row.O, row.MoveFlags, Bool(row.Moving), Bool(row.Spline), row.Speed, row.Target,
                row.Victim, static_cast<uint32>(row.Health), row.Spell, static_cast<uint32>(row.CastState),
                Number(row.Extra), Number(row.Extra2));
            break;
        case RowKind::Target:
            json += Acore::StringFormat(",\"tg\":{}", row.Target);
            break;
        case RowKind::CastStart:
            json += Acore::StringFormat(",\"s\":{},\"tg\":{},\"ct\":{}", row.Spell, row.Target, row.Extra);
            break;
        case RowKind::CastFinish:
            json += Acore::StringFormat(",\"s\":{},\"tg\":{}", row.Spell, row.Target);
            break;
        case RowKind::Cancel:
            json += Acore::StringFormat(",\"s\":{},\"self\":{}", row.Spell, Bool(row.Flag));
            break;
        case RowKind::Interrupt:
            json += Acore::StringFormat(",\"s\":{}", row.Spell);
            break;
        case RowKind::Move:
            json += Acore::StringFormat(",\"op\":{},\"mf\":{},\"x\":{:.2f},\"y\":{:.2f},\"z\":{:.2f},\"o\":{:.3f}",
                                        row.Extra, row.MoveFlags, row.X, row.Y, row.Z, row.O);
            break;
        case RowKind::Truncated:
            break;
        case RowKind::End:
            json += Acore::StringFormat(",\"rows\":{},\"dropped\":{}", row.Extra, row.Extra2);
            break;
    }

    json += '}';
    return json;
}

std::string BatchText(Batch const& batch)
{
    std::string const matchJson = PvpTestRecordWriter::JsonQuote(batch.MatchId);
    std::string text;
    text.reserve(batch.Rows.size() * 240);
    for (Row const& row : batch.Rows)
    {
        text += RowJson(row, matchJson);
        text += '\n';
    }

    return text;
}

// G0i: the binary format v1 (the hub's .claude/plans/living-azeroth/drafts/phase-g/natural-format.md; decoded by
// tools/lib/natural-trace.js). Little endian, written byte by byte, so the layout does not depend on the host.
constexpr uint16 TraceFormatVersion = 1;
constexpr uint16 TraceFormatLittleEndian = 1;
constexpr uint8 BlockRows = 1;
constexpr size_t BlockHeadBytes = 9;
constexpr size_t BlockLengthAt = 5;
constexpr size_t HeaderSizeAt = 8;
constexpr uint8 KindCount = static_cast<uint8>(RowKind::End) + 1;
constexpr size_t SafeNameLength = 120;

enum FieldType : uint8
{
    FieldU8 = 1,
    FieldU16 = 2,
    FieldU32 = 3,
    FieldI16 = 4,
    FieldI32 = 5,
    FieldBool = 6,
    FieldChar = 7,
    FieldEnum = 8,
    FieldI32OrNull = 9
};

struct FieldDef
{
    char const* Name;
    FieldType Type;
    uint16 Scale;
};

// The values of the one enum field (start: src), in table order
std::array<char const*, 3> const SourceNames = {"organic", "harness", "gm"};

// The v1 field table by RowKind value (= kind id); PutRow writes the same fields in the same order
std::vector<FieldDef> const& KindFields(RowKind kind)
{
    static std::array<std::vector<FieldDef>, KindCount> const table = {{
        {{"src", FieldEnum, 1},
         {"side", FieldChar, 1},
         {"cls", FieldU8, 1},
         {"bot", FieldBool, 1},
         {"iv", FieldU32, 1}},
        {{"x", FieldI32, 100},
         {"y", FieldI32, 100},
         {"z", FieldI32, 100},
         {"o", FieldI16, 1000},
         {"mf", FieldU32, 1},
         {"mov", FieldBool, 1},
         {"spl", FieldBool, 1},
         {"run", FieldU16, 100},
         {"tg", FieldU32, 1},
         {"vi", FieldU32, 1},
         {"hp", FieldU8, 1},
         {"cs", FieldU32, 1},
         {"st", FieldU8, 1},
         {"ce", FieldI32OrNull, 1},
         {"cr", FieldI32OrNull, 1}},
        {{"tg", FieldU32, 1}},
        {{"s", FieldU32, 1}, {"tg", FieldU32, 1}, {"ct", FieldI32, 1}},
        {{"s", FieldU32, 1}, {"tg", FieldU32, 1}},
        {{"s", FieldU32, 1}, {"self", FieldBool, 1}},
        {{"s", FieldU32, 1}},
        {{"op", FieldU32, 1},
         {"mf", FieldU32, 1},
         {"x", FieldI32, 100},
         {"y", FieldI32, 100},
         {"z", FieldI32, 100},
         {"o", FieldI16, 1000}},
        {},
        {{"rows", FieldU32, 1}, {"dropped", FieldU32, 1}},
    }};
    return table[static_cast<size_t>(kind)];
}

uint32 TypeBytes(FieldType type)
{
    switch (type)
    {
        case FieldU16:
        case FieldI16:
            return 2;
        case FieldU32:
        case FieldI32:
        case FieldI32OrNull:
            return 4;
        default:
            return 1;
    }
}

void PutU8(std::string& out, uint32 value) { out.push_back(static_cast<char>(value & 0xFF)); }

void PutU16(std::string& out, uint32 value)
{
    PutU8(out, value);
    PutU8(out, value >> 8);
}

void PutU32(std::string& out, uint32 value)
{
    PutU16(out, value & 0xFFFF);
    PutU16(out, value >> 16);
}

void PutU64(std::string& out, uint64 value)
{
    PutU32(out, static_cast<uint32>(value & 0xFFFFFFFFu));
    PutU32(out, static_cast<uint32>(value >> 32));
}

void SetU32(std::string& out, size_t at, uint32 value)
{
    for (size_t index = 0; index < 4; ++index)
        out[at + index] = static_cast<char>((value >> (8 * index)) & 0xFF);
}

// A length-prefixed UTF-8 string: u8 or (wide) u16 length; a longer text is cut
void PutString(std::string& out, std::string const& text, bool wide)
{
    size_t const length = std::min<size_t>(text.size(), wide ? 0xFFFF : 0xFF);
    if (wide)
        PutU16(out, static_cast<uint32>(length));
    else
        PutU8(out, static_cast<uint32>(length));

    out.append(text, 0, length);
}

// round(value * scale), half up as the hub's encoder, clamped to the field's range; NaN writes 0
int64 Scaled(float value, double scale, int64 low, int64 high)
{
    double const scaled = std::floor(static_cast<double>(value) * scale + 0.5);
    if (std::isnan(scaled))
        return 0;

    return static_cast<int64>(std::clamp(scaled, static_cast<double>(low), static_cast<double>(high)));
}

void PutI32(std::string& out, int32 value) { PutU32(out, static_cast<uint32>(value)); }

void PutCoordinate(std::string& out, float value)
{
    PutI32(out, static_cast<int32>(
                    Scaled(value, 100.0, std::numeric_limits<int32>::min(), std::numeric_limits<int32>::max())));
}

void PutFacing(std::string& out, float value)
{
    PutU16(out, static_cast<uint16>(static_cast<int16>(
                    Scaled(value, 1000.0, std::numeric_limits<int16>::min(), std::numeric_limits<int16>::max()))));
}

uint32 Counter(uint64 guid) { return static_cast<uint32>(std::min<uint64>(guid, 0xFFFFFFFFu)); }

// i32n: negative = null, written as -1
void PutOrNull(std::string& out, int32 value) { PutI32(out, value < 0 ? -1 : value); }

uint32 SourceIndex(char const* source)
{
    for (size_t index = 0; source && index < SourceNames.size(); ++index)
    {
        if (!std::strcmp(source, SourceNames[index]))
            return static_cast<uint32>(index);
    }

    return 0xFF;
}

// One row: u8 kind id, u32 t, the kind's payload (KindFields)
void PutRow(std::string& out, Row const& row)
{
    PutU8(out, static_cast<uint32>(row.Kind));
    PutU32(out, row.Ms);
    switch (row.Kind)
    {
        case RowKind::Start:
            PutU8(out, SourceIndex(row.Source));
            PutU8(out, static_cast<uint8>(row.Side));
            PutU8(out, std::min<uint32>(row.Spell, 0xFF));
            PutU8(out, row.Flag ? 1 : 0);
            PutU32(out, static_cast<uint32>(std::max<int32>(row.Extra, 0)));
            break;
        case RowKind::Pos:
            PutCoordinate(out, row.X);
            PutCoordinate(out, row.Y);
            PutCoordinate(out, row.Z);
            PutFacing(out, row.O);
            PutU32(out, row.MoveFlags);
            PutU8(out, row.Moving ? 1 : 0);
            PutU8(out, row.Spline ? 1 : 0);
            PutU16(out, static_cast<uint32>(Scaled(row.Speed, 100.0, 0, 0xFFFF)));
            PutU32(out, Counter(row.Target));
            PutU32(out, Counter(row.Victim));
            PutU8(out, row.Health);
            PutU32(out, row.Spell);
            PutU8(out, row.CastState);
            PutOrNull(out, row.Extra);
            PutOrNull(out, row.Extra2);
            break;
        case RowKind::Target:
            PutU32(out, Counter(row.Target));
            break;
        case RowKind::CastStart:
            PutU32(out, row.Spell);
            PutU32(out, Counter(row.Target));
            PutI32(out, row.Extra);
            break;
        case RowKind::CastFinish:
            PutU32(out, row.Spell);
            PutU32(out, Counter(row.Target));
            break;
        case RowKind::Cancel:
            PutU32(out, row.Spell);
            PutU8(out, row.Flag ? 1 : 0);
            break;
        case RowKind::Interrupt:
            PutU32(out, row.Spell);
            break;
        case RowKind::Move:
            PutU32(out, static_cast<uint32>(row.Extra));
            PutU32(out, row.MoveFlags);
            PutCoordinate(out, row.X);
            PutCoordinate(out, row.Y);
            PutCoordinate(out, row.Z);
            PutFacing(out, row.O);
            break;
        case RowKind::Truncated:
            break;
        case RowKind::End:
            PutU32(out, static_cast<uint32>(std::max<int32>(row.Extra, 0)));
            PutU32(out, static_cast<uint32>(std::max<int32>(row.Extra2, 0)));
            break;
    }
}

// The header of a new binary file: magic, version, flags, its size, the match's wall clock start and interval, match
// id, instance, source (of the unit whose batch creates the file), then the field table
std::string HeaderBytes(std::string const& matchId, std::string const& instance, char const* source, uint32 interval,
                        uint64 startUnixMs)
{
    std::string out;
    out.reserve(512);
    out.append("NTRC", 4);
    PutU16(out, TraceFormatVersion);
    PutU16(out, TraceFormatLittleEndian);
    PutU32(out, 0);
    PutU64(out, startUnixMs);
    PutU32(out, interval);
    PutString(out, matchId, true);
    PutString(out, instance, true);
    PutString(out, source ? source : "", true);
    PutU8(out, KindCount);
    for (uint8 id = 0; id < KindCount; ++id)
    {
        RowKind const kind = static_cast<RowKind>(id);
        std::vector<FieldDef> const& fields = KindFields(kind);
        uint32 payload = 0;
        for (FieldDef const& field : fields)
            payload += TypeBytes(field.Type);

        PutU8(out, id);
        PutString(out, KindName(kind), false);
        PutU16(out, payload);
        PutU8(out, static_cast<uint32>(fields.size()));
        for (FieldDef const& field : fields)
        {
            PutString(out, field.Name, false);
            PutU8(out, field.Type);
            PutU16(out, field.Scale);
            if (field.Type != FieldEnum)
                continue;

            PutU8(out, static_cast<uint32>(SourceNames.size()));
            for (char const* name : SourceNames)
                PutString(out, name, false);
        }
    }

    SetU32(out, HeaderSizeAt, static_cast<uint32>(out.size()));
    return out;
}

// One block of one unit's rows: u8 type, u32 unit guid counter, u32 payload bytes, the rows
std::string BlockBytes(std::vector<Row> const& rows)
{
    std::string out;
    out.reserve(BlockHeadBytes + rows.size() * 50);
    PutU8(out, BlockRows);
    PutU32(out, rows.empty() ? 0 : Counter(rows.front().Guid));
    PutU32(out, 0);
    for (Row const& row : rows)
        PutRow(out, row);

    SetU32(out, BlockLengthAt, static_cast<uint32>(out.size() - BlockHeadBytes));
    return out;
}

// A file name part: every character outside [A-Za-z0-9._-] becomes _, at most 120 characters (the hub's safeName)
std::string SafeName(std::string const& text)
{
    std::string name = text.substr(0, SafeNameLength);
    for (char& c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-')
            c = '_';
    }

    return name.empty() ? std::string("_") : name;
}

std::atomic<uint64> QueuedBatches{0};
std::atomic<uint64> WrittenRows{0};
std::atomic<uint64> WrittenBytes{0};
// Binary files created (a header written)
std::atomic<uint64> WrittenFiles{0};
// The formats whose place was logged (bit 0 JSONL, bit 1 binary)
std::atomic<uint8> LoggedFormats{0};
// .playerbots natural sample: -1 = Playerbots.Natural.Trace.SampleEvery, else the N set by the command (0 = off)
std::atomic<int32> SampleOverride{-1};
// .playerbots natural organic: -1 = Playerbots.Natural.Trace.Organic, else 0 or 1 set by the command
std::atomic<int32> OrganicOverride{-1};
// Set by Shutdown (before the sessions are kicked, while map threads still update): no trace begins after it
std::atomic<bool> Stopping{false};

// Map thread: hands a batch to the writer thread, which formats it (a pointer move here). Binary: the block, and the
// header when the writer finds the file new or empty
void WriteBatch(Batch&& batch)
{
    ++QueuedBatches;
    auto shared = std::make_shared<Batch>(std::move(batch));
    Place const& where = *shared->Where;
    if (where.Binary)
    {
        PvpTestRecordWriter::Instance().WriteLine(
            "trace", std::string(), std::string(),
            [shared]()
            {
                std::string bytes = BlockBytes(shared->Rows);
                WrittenRows += shared->Rows.size();
                WrittenBytes += bytes.size();
                return bytes;
            },
            where.Directory, where.File,
            [shared]()
            {
                std::string header = HeaderBytes(shared->MatchId, shared->Where->Instance, shared->Source,
                                                 shared->Interval, shared->Where->StartUnixMs);
                WrittenBytes += header.size();
                ++WrittenFiles;
                return header;
            });
        return;
    }

    PvpTestRecordWriter::Instance().WriteLine(
        "trace", where.Date, std::string(),
        [shared]()
        {
            std::string text = BatchText(*shared);
            WrittenRows += shared->Rows.size();
            WrittenBytes += text.size();
            return text;
        },
        where.Directory);
}

Tracer& Global()
{
    static Tracer tracer(NaturalTrace::Live, &WriteBatch, []() { return getMSTime(); });
    return tracer;
}

bool Organic()
{
    int32 const forced = OrganicOverride.load(std::memory_order_relaxed);
    return forced >= 0 ? forced != 0 : sPlayerbotAIConfig.NaturalTraceOrganic;
}

uint32 SampleEvery()
{
    int32 const forced = SampleOverride.load(std::memory_order_relaxed);
    return forced >= 0 ? static_cast<uint32>(forced) : sPlayerbotAIConfig.NaturalTraceSampleEvery;
}

std::string TraceDirectory()
{
    std::string dir = sPlayerbotAIConfig.NaturalTraceDir;
    if (dir.empty())
    {
        dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
            dir.push_back('/');

        dir += "natural";
    }

    if (dir.back() != '/' && dir.back() != '\\')
        dir.push_back('/');

    return dir;
}

std::string Today()
{
    std::string date = PvpTestRecordWriter::IsoTimeNow().substr(0, DateLength);
    date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
    return date;
}

// Map thread, at a trace start (strings only, no I/O): where the unit's rows go. Binary: <root>/<instance>/<UTC day>/
// <match id>.ntr; a GM trace (no match id) gets one file per session, gm-<guid counter>-<unix seconds>.ntr
PlacePtr MakePlace(std::string const& matchId, uint64 guid)
{
    auto place = std::make_shared<Place>();
    place->Binary = sPlayerbotAIConfig.NaturalTraceBinary;
    place->Directory = TraceDirectory();
    if (!place->Binary)
    {
        place->Date = Today();
        return place;
    }

    uint64 const nowMs = static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    place->Instance = MatchLog::ServerId();
    place->StartUnixMs = nowMs;
    place->Directory += SafeName(place->Instance) + "/" + PvpTestRecordWriter::IsoTimeNow().substr(0, DateLength) + "/";
    place->File = SafeName(matchId.empty() ? Acore::StringFormat("gm-{}-{}", guid, nowMs / 1000) : matchId) + ".ntr";
    return place;
}

// Map thread: what a pos row reads from the unit (its own fields only)
void ReadUnit(Player* player, Row& row)
{
    row.X = player->GetPositionX();
    row.Y = player->GetPositionY();
    row.Z = player->GetPositionZ();
    row.O = player->GetOrientation();
    row.MoveFlags = player->GetUnitMovementFlags();
    row.Moving = player->isMoving();
    row.Spline = player->movespline && !player->movespline->Finalized();
    row.Speed = player->GetSpeed(MOVE_RUN);
    Unit const* victim = player->GetVictim();
    row.Victim = victim ? victim->GetGUID().GetCounter() : 0;
    row.Health = static_cast<uint8>(std::min(100.0f, player->GetHealthPct() + PercentRound));
    row.Extra = -1;
    row.Extra2 = -1;
    Spell* spell = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    bool const channel = spell != nullptr;
    if (!spell)
        spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);

    if (!spell || !spell->GetSpellInfo())
        return;

    row.Spell = spell->GetSpellInfo()->Id;
    row.CastState = static_cast<uint8>(spell->getState());
    int32 const remaining = spell->GetCastTimeRemaining();
    row.Extra2 = std::max(remaining, 0);
    if (!channel)
        row.Extra = std::max(spell->GetCastTime() - remaining, 0);
}

// The guid counter of a player whose mark is set (0 = not traced: pets, totems and creatures never are)
uint64 TracedGuid(Unit const* unit)
{
    if (!unit || !unit->IsPlayer())
        return 0;

    uint64 const guid = unit->GetGUID().GetCounter();
    return Global().Marked(guid) ? guid : 0;
}

void PushEvent(uint64 guid, Row const& row)
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "NaturalTraceEvent");
    Global().Event(guid, row);
    if (pmo)
        pmo->finish();
}

uint64 UnitCounter(Unit const* unit) { return unit ? unit->GetGUID().GetCounter() : 0; }
}  // namespace

void NaturalTrace::Sample(Player* player)
{
    uint64 const guid = player->GetGUID().GetCounter();
    Tracer& tracer = Global();
    if (!tracer.Marked(guid))
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "NaturalTraceSample");
    tracer.Offer(guid, player->GetTarget().GetCounter(), [player](Row& row) { ReadUnit(player, row); });
    if (pmo)
        pmo->finish();
}

void NaturalTrace::OnBotCastStart(Player* bot, Spell* spell, Unit* target)
{
    uint64 const guid = TracedGuid(bot);
    // An instant cast, or a channel, has already been cast inside prepare (its finish row is written): only a cast
    // that is still the bot's current one and still preparing begins here
    if (!guid || !spell || bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) != spell ||
        spell->getState() != SPELL_STATE_PREPARING || !spell->GetSpellInfo())
        return;

    Row row;
    row.Kind = RowKind::CastStart;
    row.Spell = spell->GetSpellInfo()->Id;
    row.Target = UnitCounter(target);
    row.Extra = std::max(spell->GetCastTime(), 0);
    PushEvent(guid, row);
}

void NaturalTrace::OnInterrupted(Player* bot, uint32 spellId)
{
    uint64 const guid = TracedGuid(bot);
    if (!guid)
        return;

    Row row;
    row.Kind = RowKind::Interrupt;
    row.Spell = spellId;
    PushEvent(guid, row);
}

void NaturalTrace::OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo)
{
    // Bots write their cast start from PlayerbotAI::CastSpell; this one is for the other players
    uint64 const guid = TracedGuid(caster);
    if (!guid || spell->IsTriggered() || spell->GetCastTime() <= 0 || GET_PLAYERBOT_AI(caster->ToPlayer()))
        return;

    Row row;
    row.Kind = RowKind::CastStart;
    row.Spell = spellInfo ? spellInfo->Id : 0;
    row.Target = UnitCounter(spell->m_targets.GetUnitTarget());
    row.Extra = spell->GetCastTime();
    PushEvent(guid, row);
}

void NaturalTrace::OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo)
{
    uint64 const guid = TracedGuid(caster);
    if (!guid || spell->IsTriggered() || spell->IsAutoRepeat())
        return;

    Row row;
    row.Kind = RowKind::CastFinish;
    row.Spell = spellInfo ? spellInfo->Id : 0;
    row.Target = UnitCounter(spell->m_targets.GetUnitTarget());
    PushEvent(guid, row);
}

void NaturalTrace::OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool bySelf)
{
    uint64 const guid = TracedGuid(caster);
    if (!guid || spell->IsTriggered())
        return;

    Row row;
    row.Kind = RowKind::Cancel;
    row.Spell = spellInfo ? spellInfo->Id : 0;
    row.Flag = bySelf;
    PushEvent(guid, row);
}

void NaturalTrace::OnLogout(Player* player)
{
    if (uint64 const guid = TracedGuid(player))
        Global().Remove(guid);
}

bool NaturalTrace::Selected(std::string const& matchId)
{
    return sPlayerbotAIConfig.NaturalTraceEnable && MatchTrace::Selected(matchId, SampleEvery());
}

namespace
{
Row StartRow(Player* player, char side, char const* source)
{
    Row start;
    start.Source = source;
    start.Side = side;
    start.Spell = player->getClass();
    start.Flag = GET_PLAYERBOT_AI(player) != nullptr;
    return start;
}

void LogDirectoryOnce(Place const& where)
{
    uint8 const bit = where.Binary ? 2 : 1;
    if (LoggedFormats.fetch_or(bit) & bit)
        return;

    if (where.Binary)
        LOG_INFO("playerbots", "NATURAL: traces go to {}<match id>.ntr (binary, one file per match)", where.Directory);
    else
        LOG_INFO("playerbots", "NATURAL: traces go to {}trace-<YYYYMMDD>.jsonl", where.Directory);
}
}  // namespace

bool NaturalTrace::Begin(Player* player, std::string const& matchId, char side, char const* source)
{
    if (!sPlayerbotAIConfig.NaturalTraceEnable || !player || Stopping.load(std::memory_order_relaxed))
        return false;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "NaturalTraceStart");
    uint64 const guid = player->GetGUID().GetCounter();
    PlacePtr const where = MakePlace(matchId, guid);
    AddResult const result =
        Global().Add(guid, matchId, where, StartRow(player, side, source), sPlayerbotAIConfig.NaturalTraceMaxUnits,
                     sPlayerbotAIConfig.NaturalTraceIntervalMs, sPlayerbotAIConfig.NaturalTraceMaxRowsPerUnit);
    if (result == AddResult::Added)
        LogDirectoryOnce(*where);

    if (pmo)
        pmo->finish();

    return result == AddResult::Added;
}

void NaturalTrace::OnArenaStart(Battleground* bg, std::string const& matchId)
{
    if (!sPlayerbotAIConfig.NaturalTraceEnable || !Organic() || !bg || Stopping.load(std::memory_order_relaxed) ||
        !Selected(matchId))
        return;

    // All of the match or nothing (one check and add under the registry lock, as arena starts run on several map
    // threads): a half-traced match is no use for reaction times
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "NaturalTraceStart");
    std::vector<std::pair<uint64, Row>> members;
    for (auto const& entry : bg->GetPlayers())
    {
        if (Player* player = entry.second)
            members.emplace_back(player->GetGUID().GetCounter(),
                                 StartRow(player, player->GetBgTeamId() == TEAM_ALLIANCE ? 'A' : 'B', "organic"));
    }

    PlacePtr const where = MakePlace(matchId, 0);
    if (Global().AddGroup(std::move(members), matchId, where, sPlayerbotAIConfig.NaturalTraceMaxUnits,
                          sPlayerbotAIConfig.NaturalTraceIntervalMs,
                          sPlayerbotAIConfig.NaturalTraceMaxRowsPerUnit) == AddResult::Added)
        LogDirectoryOnce(*where);

    if (pmo)
        pmo->finish();
}

void NaturalTrace::EndMatch(std::string const& matchId) { Global().EndMatch(matchId); }

void NaturalTrace::Shutdown()
{
    Stopping.store(true, std::memory_order_relaxed);
    if (Any())
        Global().Clear();
}

bool NaturalTrace::HandleTraceCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> tokens;
    for (std::string_view const token : Acore::Tokenize(args ? args : "", ' ', false))
        tokens.emplace_back(token);

    if (tokens.size() != 2 || (tokens[1] != "on" && tokens[1] != "off"))
    {
        handler->PSendSysMessage("Usage: .playerbots natural trace <player> on|off");
        return true;
    }

    if (!sPlayerbotAIConfig.NaturalTraceEnable)
    {
        handler->PSendSysMessage("NATURAL trace: refused, Playerbots.Natural.Trace.Enable is off");
        return true;
    }

    Player* player = ObjectAccessor::FindPlayerByName(tokens[0]);
    if (!player)
    {
        handler->PSendSysMessage("NATURAL trace: {} is not online", tokens[0]);
        return true;
    }

    uint64 const guid = player->GetGUID().GetCounter();
    if (tokens[1] == "off")
    {
        handler->PSendSysMessage("NATURAL trace {} off: {}", tokens[0],
                                 Global().Remove(guid) ? "trace ended" : "was not traced");
        return true;
    }

    bool const added = Begin(player, "", 0, "gm");
    handler->PSendSysMessage("NATURAL trace {} on: {}", tokens[0],
                             added ? "traced" : "not added (already traced, or MaxUnits reached)");
    return true;
}

bool NaturalTrace::HandleSampleCommand(ChatHandler* handler, char const* args)
{
    std::string const value = args ? args : "";
    if (value == "off")
    {
        SampleOverride.store(0);
        uint32 const ended = Global().Clear();
        handler->PSendSysMessage("NATURAL sample off: no new traces, {} traces ended", ended);
        return true;
    }

    if (value == "config")
    {
        SampleOverride.store(-1);
        handler->PSendSysMessage("NATURAL sample: from the config (every {})",
                                 sPlayerbotAIConfig.NaturalTraceSampleEvery);
        return true;
    }

    Optional<uint32> const every = Acore::StringTo<uint32>(value);
    if (!every || !*every || *every > 1000000)
    {
        handler->PSendSysMessage("Usage: .playerbots natural sample <N|off|config> (N = trace one match in N)");
        return true;
    }

    SampleOverride.store(static_cast<int32>(*every));
    handler->PSendSysMessage("NATURAL sample: one match in {}", *every);
    return true;
}

bool NaturalTrace::HandleOrganicCommand(ChatHandler* handler, char const* args)
{
    std::string const value = args ? args : "";
    if (value != "on" && value != "off" && value != "config")
    {
        handler->PSendSysMessage("Usage: .playerbots natural organic <on|off|config>");
        return true;
    }

    OrganicOverride.store(value == "config" ? -1 : (value == "on" ? 1 : 0));
    handler->PSendSysMessage("NATURAL organic: {} ({})", Organic() ? "on" : "off",
                             value == "config" ? "config" : "command");
    return true;
}

bool NaturalTrace::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    Tracer& tracer = Global();
    int32 const forced = SampleOverride.load();
    handler->PSendSysMessage(
        "NATURAL trace: enable {}, organic {}, sample every {} ({}), interval {} ms, max units {}, dir {}, format {}",
        sPlayerbotAIConfig.NaturalTraceEnable ? 1 : 0, Organic() ? 1 : 0, SampleEvery(),
        forced >= 0 ? "command" : "config", sPlayerbotAIConfig.NaturalTraceIntervalMs,
        sPlayerbotAIConfig.NaturalTraceMaxUnits, TraceDirectory(),
        sPlayerbotAIConfig.NaturalTraceBinary ? "binary" : "jsonl");
    handler->PSendSysMessage(
        "NATURAL units: live {}, started {}, ended {}, refused {}, truncated {}; rows: pos {}, events {}, mv {}, "
        "dropped {}",
        tracer.Count(), tracer.Started.load(), tracer.Ended.load(), tracer.Refused.load(), tracer.TruncatedUnits.load(),
        tracer.PosRows.load(), tracer.EventRows.load(), tracer.MoveRows.load(), tracer.DroppedRows.load());
    PvpTestRecordWriter& writer = PvpTestRecordWriter::Instance();
    handler->PSendSysMessage(
        "NATURAL writer: batches queued {}, rows written {}, bytes written {}, queue {}, failed {}, binary files {}",
        QueuedBatches.load(), WrittenRows.load(), WrittenBytes.load(), writer.QueueSize(), writer.Failed(),
        WrittenFiles.load());
    return true;
}

bool NaturalTrace::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    uint32 failures = 0;
    uint32 checks = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("NATURAL selftest FAIL: {}", what);
    };

    // A fake clock that wraps during the test, and a sink that keeps the batches
    std::atomic<uint32> live{0};
    std::vector<Batch> out;
    uint32 clock = 0xFFFFFF00u;
    auto tracer = std::make_unique<Tracer>(
        live, [&out](Batch&& batch) { out.push_back(std::move(batch)); }, [&clock]() { return clock; });
    uint64 const botA = 101;
    uint64 const playerB = 102;
    uint64 const extra = 103;
    std::string const matchId = "selftest-20261027000000-1";
    PlacePtr const jsonl = std::make_shared<Place const>(Place{"d/", "20261027"});
    Row startBot;
    startBot.Source = "harness";
    startBot.Side = 'A';
    startBot.Spell = 8;
    startBot.Flag = true;
    Row startPlayer;
    startPlayer.Source = "gm";
    startPlayer.Spell = 1;

    check(!tracer->Marked(botA) && !live.load(), "nothing traced: the hooks stop at the flag");
    check(tracer->Add(botA, matchId, jsonl, startBot, 2, 100, 100000) == AddResult::Added &&
              tracer->Add(playerB, matchId, jsonl, startPlayer, 2, 100, 100000) == AddResult::Added &&
              live.load() == 2 && tracer->Marked(botA) && tracer->Marked(playerB),
          "two units traced, marked, counted");
    check(tracer->Add(extra, matchId, jsonl, startBot, 2, 100, 100000) == AddResult::Full &&
              tracer->Add(botA, matchId, jsonl, startBot, 2, 100, 100000) == AddResult::Exists &&
              !tracer->Marked(extra),
          "MaxUnits and a second trace of a unit are refused");

    // An organic match is traced whole or not at all: the group check and add hold one lock
    {
        std::atomic<uint32> groupLive{0};
        Tracer group(groupLive, [](Batch&&) {}, [&clock]() { return clock; });
        auto members = [&startBot](std::vector<uint64> const& guids)
        {
            std::vector<std::pair<uint64, Row>> list;
            for (uint64 const guid : guids)
                list.emplace_back(guid, startBot);

            return list;
        };

        group.Add(201, "", jsonl, startPlayer, 4, 100, 100000);
        check(group.AddGroup(members({301, 302, 303, 304}), matchId, jsonl, 4, 100, 100000) == AddResult::Full &&
                  groupLive.load() == 1 && !group.Marked(301) && !group.Marked(304) && group.Refused.load() == 4,
              "a group that does not fit under MaxUnits is refused whole");
        check(group.AddGroup(members({301, 302, 303}), matchId, jsonl, 4, 100, 100000) == AddResult::Added &&
                  groupLive.load() == 4 && group.Marked(301) && group.Marked(303) && group.Started.load() == 4,
              "a group that fits is traced whole");
        check(group.AddGroup(members({401, 302}), matchId, jsonl, 10, 100, 100000) == AddResult::Exists &&
                  groupLive.load() == 4 && !group.Marked(401),
              "a group with a traced unit is refused whole");

        // Two matches start on two threads at once with room for one: one is traced whole, the other not at all
        uint32 split = 0;
        for (uint32 round = 0; round < 50; ++round)
        {
            std::atomic<uint32> raceLive{0};
            Tracer race(raceLive, [](Batch&&) {}, [&clock]() { return clock; });
            std::atomic<uint32> added{0};
            auto start = [&](uint64 first)
            {
                if (race.AddGroup(members({first, first + 1, first + 2, first + 3}), matchId, jsonl, 6, 100, 100000) ==
                    AddResult::Added)
                    ++added;
            };

            std::thread one(start, 501);
            std::thread two(start, 601);
            one.join();
            two.join();
            split += added.load() == 1 && raceLive.load() == 4 && race.Count() == 4 ? 0 : 1;
        }

        check(!split, Acore::StringFormat("two concurrent starts: one traced whole ({} of 50 rounds split)", split));
    }

    // The bot is updated every 30 ms for 3 s (a target from 1500 ms, a cast from 990 to 1980 ms); the player
    // (the non-bot path: the same sampler) every 250 ms, slower than the slots
    uint32 const base = clock;
    for (uint32 step = 0; step <= 100; ++step)
    {
        clock = base + step * 30;
        uint64 const target = step * 30 >= 1500 ? 7 : 0;
        tracer->Offer(botA, target, [step](Row& row) { row.X = static_cast<float>(step); });
        if (step * 30 == 990)
        {
            Row cast;
            cast.Kind = RowKind::CastStart;
            cast.Spell = 116;
            cast.Extra = 990;
            tracer->Event(botA, cast);
        }

        if (step * 30 == 1980)
        {
            Row finish;
            finish.Kind = RowKind::CastFinish;
            finish.Spell = 116;
            tracer->Event(botA, finish);
        }
    }

    for (uint32 step = 0; step <= 12; ++step)
    {
        clock = base + step * 250;
        tracer->Offer(playerB, 0, [](Row& row) { row.Health = 100; });
        Row move;
        move.Kind = RowKind::Move;
        move.Extra = 0xEE;
        tracer->Event(playerB, move);
    }

    check(!tracer->Offer(extra, 0, [](Row&) {}), "an untraced unit is not sampled");
    check(tracer->EndMatch(matchId) == 2 && !live.load() && !tracer->Marked(botA) && !tracer->Marked(playerB),
          "the match end closes both traces and clears the marks");

    std::unordered_map<uint64, std::vector<Row>> rows;
    for (Batch const& batch : out)
    {
        for (Row const& row : batch.Rows)
            rows[row.Guid].push_back(row);
    }

    auto count = [](std::vector<Row> const& list, RowKind kind)
    { return std::count_if(list.begin(), list.end(), [kind](Row const& row) { return row.Kind == kind; }); };
    std::vector<Row> const& a = rows[botA];
    std::vector<Row> const& b = rows[playerB];
    check(count(a, RowKind::Pos) == 31,
          Acore::StringFormat("bot: 31 pos rows in 3 s at 30 ms updates (got {})", count(a, RowKind::Pos)));
    check(count(b, RowKind::Pos) == 13 && count(b, RowKind::Move) == 13,
          "player: one pos row per 250 ms update (missed slots not back-filled), 13 mv rows");

    bool ordered = true;
    bool oneSlot = true;
    bool onTime = true;
    for (std::vector<Row> const* list : {&a, &b})
    {
        int64 lastSlot = -1;
        for (size_t index = 0; index < list->size(); ++index)
        {
            Row const& row = (*list)[index];
            if (index && row.Ms < (*list)[index - 1].Ms)
                ordered = false;

            if (row.Kind != RowKind::Pos)
                continue;

            int64 const slot = row.Ms / 100;
            if (slot <= lastSlot)
                oneSlot = false;

            if (list == &a && row.Ms - slot * 100 >= 30)
                onTime = false;

            lastSlot = slot;
        }
    }

    check(ordered, "rows of a unit never go back in time (across the clock wrap)");
    check(oneSlot, "at most one pos row per 100 ms slot");
    check(onTime, "a pos row is taken at the first update in its slot");
    check(!a.empty() && a.front().Kind == RowKind::Start && a.back().Kind == RowKind::End && a.back().Extra == 35,
          Acore::StringFormat("the trace opens with start and closes with end (rows {})",
                              a.empty() ? 0 : a.back().Extra));

    auto find = [](std::vector<Row> const& list, RowKind kind)
    {
        auto const itr = std::find_if(list.begin(), list.end(), [kind](Row const& row) { return row.Kind == kind; });
        return itr == list.end() ? -1 : static_cast<int64>(itr - list.begin());
    };
    int64 const castStart = find(a, RowKind::CastStart);
    int64 const castFinish = find(a, RowKind::CastFinish);
    int64 const target = find(a, RowKind::Target);
    check(castStart >= 0 && castFinish > castStart && a[castStart].Ms == 990 && a[castFinish].Ms == 1980,
          "cast start before its finish, at their times");
    check(target >= 0 && a[target].Ms == 1500 && a[target].Target == 7 && count(a, RowKind::Target) == 1 &&
              a[target + 1].Kind == RowKind::Pos && a[target + 1].Ms == 1500,
          "one target change row, before the pos row of its update");

    // The row cap: 5 rows kept, one truncated row, the rest counted
    out.clear();
    clock = base;
    tracer->Add(extra, "", jsonl, startPlayer, 2, 100, 5);
    for (uint32 step = 0; step < 20; ++step)
    {
        clock = base + step * 100;
        tracer->Offer(extra, 0, [](Row&) {});
    }

    tracer->Clear();
    std::vector<Row> capped;
    for (Batch const& batch : out)
        capped.insert(capped.end(), batch.Rows.begin(), batch.Rows.end());

    check(capped.size() == 7 && capped[5].Kind == RowKind::Truncated && capped[6].Kind == RowKind::End &&
              capped[6].Extra == 5 && capped[6].Extra2 == 16,
          "the row cap keeps 5 rows, then a truncated row; the end row counts the dropped ones");

    // The format: every kind is one well-formed JSON object
    std::string sample;
    bool wellFormed = true;
    std::string const matchJson = PvpTestRecordWriter::JsonQuote(matchId);
    std::vector<Row> const& cappedRows = capped;
    for (std::vector<Row> const* list : {&a, &b, &cappedRows})
    {
        for (Row const& row : *list)
        {
            std::string const json = RowJson(row, matchJson);
            if (!MatchLog::JsonWellFormed(json) || json.find(KindName(row.Kind)) == std::string::npos)
                wellFormed = false;

            if (row.Kind == RowKind::Pos && sample.empty())
                sample = json;
        }
    }

    check(wellFormed, "every row is a well-formed JSON object with its kind");

    // Sampling by match id: N = 1 traces every match, N = 4 about a quarter, the same ones in every build
    uint32 everyOne = 0;
    uint32 everyFour = 0;
    for (uint32 index = 0; index < 400; ++index)
    {
        std::string const id = Acore::StringFormat("lab1-20261027000000-{}", index);
        everyOne += MatchTrace::Selected(id, 1) ? 1 : 0;
        everyFour += MatchTrace::Selected(id, 4) ? 1 : 0;
    }

    check(everyOne == 400 && everyFour >= 60 && everyFour <= 140 && !MatchTrace::Selected(matchId, 0),
          Acore::StringFormat("one match in N by match id (N = 4: {} of 400)", everyFour));

    // G0i, the binary format: the spec's fixture (natural-format.md, the hub's FIXTURE_ROWS) byte for byte
    {
        std::string const hex =
            "4e54524301000100a9010000d8ef5e7ca10100006400000016006c6162312d32"
            "303236313032373036353931392d323604006c61623107006f7267616e69630a"
            "000573746172740800050373726308010003076f7267616e6963076861726e65"
            "737302676d047369646507010003636c7301010003626f740601000269760301"
            "000103706f732c000f01780564000179056400017a056400016f04e803026d66"
            "030100036d6f760601000373706c0601000372756e0264000274670301000276"
            "6903010002687001010002637303010002737401010002636509010002637209"
            "01000206746172676574040001027467030100030a636173745f73746172740c"
            "00030173030100027467030100026374050100040b636173745f66696e697368"
            "0800020173030100027467030100050663616e63656c05000201730301000473"
            "656c660601000609696e74657272757074040001017303010007026d76160006"
            "026f70030100026d6603010001780564000179056400017a056400016f04e803"
            "08097472756e63617465640000000903656e6408000204726f77730301000764"
            "726f70706564030100013f080000d60000000000000000004101016400000002"
            "000000002e05000001000000002bf301009aa90200b20c000000000000000000"
            "00bc022e05000000000000640000000000ffffffffffffffff0165000000282f"
            "eaff129a0900a1ebffffbdf30102000801014103411700001f000000395aa700"
            "000155070000e91e0000032c0700005aa700001f00000055070000046c070000"
            "4a7c00000000000005d00700005ab900000006d10700005ab9000007d2070000"
            "b500000001000000960000001fffffff00000000721808d307000009ba140000"
            "3700000000000000";
        std::string expected;
        for (size_t index = 0; index + 1 < hex.size(); index += 2)
            expected.push_back(static_cast<char>(std::stoi(hex.substr(index, 2), nullptr, 16)));

        auto make = [](RowKind kind, uint32 ms)
        {
            Row row;
            row.Kind = kind;
            row.Ms = ms;
            row.Guid = 2111;
            return row;
        };
        std::vector<Row> rows;
        Row row = make(RowKind::Start, 0);
        row.Source = "organic";
        row.Side = 'A';
        row.Spell = 1;
        row.Flag = true;
        row.Extra = 100;
        rows.push_back(row);
        row = make(RowKind::Target, 0);
        row.Target = 1326;
        rows.push_back(row);
        row = make(RowKind::Pos, 0);
        row.X = 1277.87f;
        row.Y = 1744.9f;
        row.Z = 32.5f;
        row.Speed = 7.0f;
        row.Target = 1326;
        row.Health = 100;
        row.Extra = -1;
        row.Extra2 = -1;
        rows.push_back(row);
        row = make(RowKind::Pos, 101);
        row.X = -14297.2f;
        row.Y = 6292.66f;
        row.Z = -52.15f;
        row.O = -3.139f;
        row.MoveFlags = 134218241;
        row.Moving = true;
        row.Spline = true;
        row.Speed = 8.33f;
        row.Target = 5953;
        row.Victim = 31;
        row.Health = 57;
        row.Spell = 42842;
        row.CastState = 1;
        row.Extra = 1877;
        row.Extra2 = 7913;
        rows.push_back(row);
        row = make(RowKind::CastStart, 1836);
        row.Spell = 42842;
        row.Target = 31;
        row.Extra = 1877;
        rows.push_back(row);
        row = make(RowKind::CastFinish, 1900);
        row.Spell = 31818;
        rows.push_back(row);
        row = make(RowKind::Cancel, 2000);
        row.Spell = 47450;
        rows.push_back(row);
        row = make(RowKind::Interrupt, 2001);
        row.Spell = 47450;
        rows.push_back(row);
        row = make(RowKind::Move, 2002);
        row.Extra = 181;
        row.MoveFlags = 1;
        row.X = 1.5f;
        row.Y = -2.25f;
        row.O = 6.258f;
        rows.push_back(row);
        rows.push_back(make(RowKind::Truncated, 2003));
        row = make(RowKind::End, 5306);
        row.Extra = 55;
        rows.push_back(row);

        std::string const header =
            HeaderBytes("lab1-20261027065919-26", "lab1", "organic", 100, UINT64_C(1793087959000));
        std::string const bytes = header + BlockBytes(rows);
        size_t differ = 0;
        while (differ < bytes.size() && differ < expected.size() && bytes[differ] == expected[differ])
            ++differ;

        check(expected.size() == 648 && bytes == expected,
              Acore::StringFormat("the binary encoder writes the spec's 648-byte fixture ({} bytes, first difference "
                                  "at {})",
                                  bytes.size(), differ));
        check(header.size() == 425 && TraceFormatVersion == 1 && header[4] == 1 && header[5] == 0 && header[6] == 1 &&
                  header[7] == 0 && header[8] == static_cast<char>(0xA9) && header[9] == 1,
              "the header says version 1 (never another), little endian, its own size");

        // One file per match: the units of a binary match share the first start's place; a GM trace has its own
        std::atomic<uint32> binaryLive{0};
        std::vector<Batch> binaryOut;
        Tracer binary(
            binaryLive, [&binaryOut](Batch&& batch) { binaryOut.push_back(std::move(batch)); },
            [&clock]() { return clock; });
        PlacePtr const first =
            std::make_shared<Place const>(Place{"r/lab1/2026-10-27/", "", true, "m-1.ntr", "lab1", 1});
        PlacePtr const later =
            std::make_shared<Place const>(Place{"r/lab1/2026-10-28/", "", true, "m-1.ntr", "lab1", 2});
        PlacePtr const gm =
            std::make_shared<Place const>(Place{"r/lab1/2026-10-28/", "", true, "gm-7-2.ntr", "lab1", 2});
        binary.Add(501, "m-1", first, startBot, 8, 100, 100000);
        binary.Add(502, "m-1", later, startPlayer, 8, 100, 100000);
        binary.Add(503, "", gm, startPlayer, 8, 100, 100000);
        bool const shared = binary.Matches() == 1;
        binary.EndMatch("m-1");
        bool const released = binary.Matches() == 0 && binaryLive.load() == 1;
        binary.Clear();
        bool samePlace = binaryOut.size() == 3;
        for (Batch const& batch : binaryOut)
        {
            uint64 const guid = batch.Rows.empty() ? 0 : batch.Rows.front().Guid;
            if (guid == 503)
                samePlace = samePlace && batch.Where == gm && batch.MatchId.empty();
            else
                samePlace = samePlace && batch.Where == first && batch.Interval == 100 &&
                            !std::strcmp(batch.Source, guid == 501 ? "harness" : "gm");
        }

        check(shared && released && samePlace,
              "a binary match's units share its first place (day, header) until its last unit ends; a GM trace has "
              "its own");
        std::string const block = BlockBytes(binaryOut.front().Rows);
        check(block.size() == BlockHeadBytes + 13 + 13 && block[0] == 1 &&
                  static_cast<size_t>(static_cast<uint8>(block[BlockLengthAt])) == block.size() - BlockHeadBytes &&
                  SafeName("a/b c:d") == "a_b_c_d" && SafeName(std::string(200, 'x')).size() == SafeNameLength &&
                  SafeName("") == "_",
              "a block is type, unit, payload length and rows (start and end: 13 + 13 bytes); file names are safe");
    }

    handler->PSendSysMessage("NATURAL selftest: {} checks, {} failed", checks, failures);
    handler->PSendSysMessage("NATURAL sample row: {}", sample);
    return true;
}

// A real player's exact movement packets (facing, jumps, heartbeats): handled on the player's map thread
class NaturalTraceMovementScript : public MovementHandlerScript
{
public:
    NaturalTraceMovementScript() : MovementHandlerScript("NaturalTraceMovementScript", {MOVEMENTHOOK_ON_PLAYER_MOVE}) {}

    void OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode) override
    {
        if (!NaturalTrace::Any())
            return;

        uint64 const guid = TracedGuid(player);
        if (!guid)
            return;

        Row row;
        row.Kind = RowKind::Move;
        row.Extra = static_cast<int32>(opcode);
        row.MoveFlags = movementInfo.GetMovementFlags();
        row.X = movementInfo.pos.GetPositionX();
        row.Y = movementInfo.pos.GetPositionY();
        row.Z = movementInfo.pos.GetPositionZ();
        row.O = movementInfo.pos.GetOrientation();
        PushEvent(guid, row);
    }
};

void AddSC_natural_trace() { new NaturalTraceMovementScript(); }
