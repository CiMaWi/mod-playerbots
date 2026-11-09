/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGFORMAT_H
#define _PLAYERBOT_WORLDLOGFORMAT_H

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "EventLogFormat.h"

// The Player-free part of the world log (stream contract version 1; every word is in the hub's
// .claude/docs/world-log.md): streams and their header rows, row formats, settings bounds, segment names, the segment
// each row goes to, and the per-stream queue account with its drop counts. Header-only so
// tests/world-log-selftest.cpp checks the same code the server runs.
namespace WorldLogFormat
{
constexpr uint32_t CONTRACT_VERSION = 1;
constexpr uint64_t MS_PER_SECOND = 1000;
constexpr uint64_t MS_PER_MINUTE = 60 * MS_PER_SECOND;
constexpr uint64_t MS_PER_DAY = 24 * 60 * MS_PER_MINUTE;
constexpr uint32_t MINUTES_PER_HOUR = 60;
constexpr uint32_t PERCENT = 100;
constexpr char ROW_END = '\n';
constexpr size_t EVENT_ROW_RESERVE = 96;
constexpr size_t STATE_ROW_RESERVE = 128;
constexpr size_t META_ROW_RESERVE = 64;

enum class Stream : uint8_t
{
    Event,
    State,
    Meta,
    Decision,
    Recorder
};

constexpr size_t STREAM_COUNT = 5;

constexpr std::array<std::string_view, STREAM_COUNT> STREAM_NAMES = {"event", "state", "meta", "decision", "recorder"};

// Columns are only ever added at the end
constexpr std::array<std::string_view, STREAM_COUNT> STREAM_HEADERS = {
    "t,bot,type,src,id,amount,map,zone,area,x,y",
    "t,bot,map,zone,area,x,y,level,xp,money,hp,power,rpg,target,flags,free_slots,ilvl,dura_min,group",
    "t,kind,a,b,text",
    "t,bot,kind,choice,reason,value,alt1,value1,alt2,value2,alt3,value3,ref",
    "case,t,bot,kind,name,detail,a,x,y,map",
};

constexpr size_t StreamIndex(Stream stream) { return static_cast<size_t>(stream); }
constexpr Stream StreamAt(size_t index) { return static_cast<Stream>(index); }
constexpr std::string_view StreamName(Stream stream) { return STREAM_NAMES[StreamIndex(stream)]; }
constexpr std::string_view StreamHeader(Stream stream) { return STREAM_HEADERS[StreamIndex(stream)]; }

// The state row's flags column
enum StateFlag : uint32_t
{
    STATE_FLAG_COMBAT = 1,
    STATE_FLAG_DEAD = 2,
    STATE_FLAG_GHOST = 4,
    STATE_FLAG_MOUNTED = 8,
    STATE_FLAG_TAXI = 16,
    STATE_FLAG_RESTING = 32,
    STATE_FLAG_GROUPED = 64,
    STATE_FLAG_INSTANCE = 128,
    STATE_FLAG_HUMAN = 256,
};

// The state row's rpg column for players without bot AI
constexpr int32_t RPG_STATUS_HUMAN = -1;

constexpr char const* EVENT_LEVEL = "Level";

constexpr char const* META_START = "start";
constexpr char const* META_STOP = "stop";
constexpr char const* META_RELOAD = "reload";
constexpr char const* META_DROPS = "drops";
constexpr char const* META_MARK = "mark";

// Playerbots.WorldLog.* tuning values with their defaults and bounds
struct Settings
{
    uint32_t stateSeconds = 60;
    uint32_t segmentMinutes = 60;
    uint32_t queueRows = 200000;
};

constexpr uint32_t MIN_STATE_SECONDS = 5;
constexpr uint32_t MAX_STATE_SECONDS = 3600;
constexpr uint32_t MIN_SEGMENT_MINUTES = 1;
constexpr uint32_t MAX_SEGMENT_MINUTES = MINUTES_PER_HOUR;
constexpr uint32_t MIN_QUEUE_ROWS = 1;
constexpr uint32_t MAX_QUEUE_ROWS = 10000000;

// Segment length in minutes: clamped to 1..60, then rounded down to a divisor of 60, so every segment ends at or
// before the end of the UTC hour its name gives
inline uint32_t SegmentMinutesOf(uint32_t segmentMinutes)
{
    uint32_t minutes = std::clamp(segmentMinutes, MIN_SEGMENT_MINUTES, MAX_SEGMENT_MINUTES);
    while (MINUTES_PER_HOUR % minutes)
        --minutes;

    return minutes;
}

inline Settings ClampSettings(uint32_t stateSeconds, uint32_t segmentMinutes, uint32_t queueRows)
{
    Settings settings;
    settings.stateSeconds = std::clamp(stateSeconds, MIN_STATE_SECONDS, MAX_STATE_SECONDS);
    settings.segmentMinutes = SegmentMinutesOf(segmentMinutes);
    settings.queueRows = std::clamp(queueRows, MIN_QUEUE_ROWS, MAX_QUEUE_ROWS);
    return settings;
}

// Integer percent of max, 0 when max is 0
inline uint32_t Percent(uint64_t value, uint64_t max)
{
    if (!max)
        return 0;

    return static_cast<uint32_t>(std::min<uint64_t>(PERCENT, value * PERCENT / max));
}

template <typename T>
inline void AppendNumber(std::string& out, T value)
{
    std::array<char, 24> buffer{};
    std::to_chars_result const result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

// Yards with one decimal, rounded half away from zero; never "-0.0"
inline void AppendPosition(std::string& out, float value)
{
    int64_t const tenths = std::isfinite(value) ? std::llround(static_cast<double>(value) * 10.0) : 0;
    uint64_t const magnitude = static_cast<uint64_t>(tenths < 0 ? -tenths : tenths);
    if (tenths < 0)
        out.push_back('-');

    AppendNumber(out, magnitude / 10);
    out.push_back('.');
    out.push_back(static_cast<char>('0' + magnitude % 10));
}

inline void AppendSeparator(std::string& out) { out.push_back(EventLog::FIELD_SEPARATOR); }

struct Place
{
    uint32_t map = 0;
    uint32_t zone = 0;
    uint32_t area = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct EventRow
{
    uint64_t t = 0;
    uint64_t bot = 0;
    std::string_view type;
    std::string_view source;
    uint32_t id = 0;
    int64_t amount = 0;
    Place place;
};

struct StateRow
{
    uint64_t t = 0;
    uint64_t bot = 0;
    Place place;
    uint32_t level = 0;
    uint32_t xp = 0;
    uint64_t money = 0;
    uint32_t hpPct = 0;
    uint32_t powerPct = 0;
    int32_t rpg = RPG_STATUS_HUMAN;
    uint32_t target = 0;
    uint32_t flags = 0;
    uint32_t freeSlots = 0;
    uint32_t itemLevel = 0;
    uint32_t durabilityMinPct = PERCENT;
    uint64_t group = 0;
};

struct MetaRow
{
    uint64_t t = 0;
    std::string_view kind;
    int64_t a = 0;
    int64_t b = 0;
    std::string_view text;
};

// event: t, bot, type, src, id, amount, map, zone, area, x, y (an empty source is "other")
inline void AppendEventRow(std::string& out, EventRow const& row)
{
    AppendNumber(out, row.t);
    AppendSeparator(out);
    AppendNumber(out, row.bot);
    AppendSeparator(out);
    EventLog::AppendField(out, row.type);
    AppendSeparator(out);
    EventLog::AppendField(out, row.source.empty() ? std::string_view(EventLog::SOURCE_OTHER) : row.source);
    AppendSeparator(out);
    AppendNumber(out, row.id);
    AppendSeparator(out);
    AppendNumber(out, row.amount);
    AppendSeparator(out);
    AppendNumber(out, row.place.map);
    AppendSeparator(out);
    AppendNumber(out, row.place.zone);
    AppendSeparator(out);
    AppendNumber(out, row.place.area);
    AppendSeparator(out);
    AppendPosition(out, row.place.x);
    AppendSeparator(out);
    AppendPosition(out, row.place.y);
    out.push_back(ROW_END);
}

inline std::string FormatEventRow(EventRow const& row)
{
    std::string out;
    out.reserve(EVENT_ROW_RESERVE);
    AppendEventRow(out, row);
    return out;
}

// state: t, bot, map, zone, area, x, y, level, xp, money, hp, power, rpg, target, flags, free_slots, ilvl, dura_min,
// group
inline void AppendStateRow(std::string& out, StateRow const& row)
{
    AppendNumber(out, row.t);
    AppendSeparator(out);
    AppendNumber(out, row.bot);
    AppendSeparator(out);
    AppendNumber(out, row.place.map);
    AppendSeparator(out);
    AppendNumber(out, row.place.zone);
    AppendSeparator(out);
    AppendNumber(out, row.place.area);
    AppendSeparator(out);
    AppendPosition(out, row.place.x);
    AppendSeparator(out);
    AppendPosition(out, row.place.y);
    AppendSeparator(out);
    AppendNumber(out, row.level);
    AppendSeparator(out);
    AppendNumber(out, row.xp);
    AppendSeparator(out);
    AppendNumber(out, row.money);
    AppendSeparator(out);
    AppendNumber(out, row.hpPct);
    AppendSeparator(out);
    AppendNumber(out, row.powerPct);
    AppendSeparator(out);
    AppendNumber(out, row.rpg);
    AppendSeparator(out);
    AppendNumber(out, row.target);
    AppendSeparator(out);
    AppendNumber(out, row.flags);
    AppendSeparator(out);
    AppendNumber(out, row.freeSlots);
    AppendSeparator(out);
    AppendNumber(out, row.itemLevel);
    AppendSeparator(out);
    AppendNumber(out, row.durabilityMinPct);
    AppendSeparator(out);
    AppendNumber(out, row.group);
    out.push_back(ROW_END);
}

// meta: t, kind, a, b, text
inline void AppendMetaRow(std::string& out, MetaRow const& row)
{
    AppendNumber(out, row.t);
    AppendSeparator(out);
    EventLog::AppendField(out, row.kind);
    AppendSeparator(out);
    AppendNumber(out, row.a);
    AppendSeparator(out);
    AppendNumber(out, row.b);
    AppendSeparator(out);
    EventLog::AppendField(out, row.text);
    out.push_back(ROW_END);
}

inline std::string FormatMetaRow(MetaRow const& row)
{
    std::string out;
    out.reserve(META_ROW_RESERVE);
    AppendMetaRow(out, row);
    return out;
}

// The start row's text: realm=<id> segment=<minutes> state=<seconds>
inline std::string StartText(uint32_t realmId, Settings const& settings)
{
    std::string text = "realm=";
    AppendNumber(text, realmId);
    text += " segment=";
    AppendNumber(text, settings.segmentMinutes);
    text += " state=";
    AppendNumber(text, settings.stateSeconds);
    return text;
}

// The reload row's text: queue=<rows>
inline std::string ReloadText(Settings const& settings)
{
    std::string text = "queue=";
    AppendNumber(text, settings.queueRows);
    return text;
}

struct CivilTime
{
    int64_t year = 0;
    uint32_t month = 0;
    uint32_t day = 0;
    uint32_t hour = 0;
    uint32_t minute = 0;
};

// UTC calendar time of t (days to civil date after Howard Hinnant's algorithm; no gmtime, any thread)
inline CivilTime CivilFromMs(uint64_t tMs)
{
    constexpr int64_t DAYS_TO_0000_03_01 = 719468;
    constexpr int64_t DAYS_PER_ERA = 146097;
    constexpr int64_t YEARS_PER_ERA = 400;

    int64_t const days = static_cast<int64_t>(tMs / MS_PER_DAY) + DAYS_TO_0000_03_01;
    uint64_t const msOfDay = tMs % MS_PER_DAY;
    int64_t const era = days / DAYS_PER_ERA;
    int64_t const dayOfEra = days - era * DAYS_PER_ERA;
    int64_t const yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    int64_t const dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    int64_t const monthIndex = (5 * dayOfYear + 2) / 153;

    CivilTime civil;
    civil.day = static_cast<uint32_t>(dayOfYear - (153 * monthIndex + 2) / 5 + 1);
    civil.month = static_cast<uint32_t>(monthIndex < 10 ? monthIndex + 3 : monthIndex - 9);
    civil.year = yearOfEra + era * YEARS_PER_ERA + (civil.month <= 2 ? 1 : 0);
    civil.hour = static_cast<uint32_t>(msOfDay / (MINUTES_PER_HOUR * MS_PER_MINUTE));
    civil.minute = static_cast<uint32_t>(msOfDay / MS_PER_MINUTE % MINUTES_PER_HOUR);
    return civil;
}

inline void AppendTwoDigits(std::string& out, uint32_t value)
{
    out.push_back(static_cast<char>('0' + value / 10 % 10));
    out.push_back(static_cast<char>('0' + value % 10));
}

// <stream>-<yyyymmddhh>.csv for hourly segments, <stream>-<yyyymmddhhmm>.csv for shorter (test) segments
inline std::string SegmentName(Stream stream, uint64_t startMs, uint32_t segmentMinutes)
{
    CivilTime const civil = CivilFromMs(startMs);
    std::string name(StreamName(stream));
    name.push_back('-');
    AppendNumber(name, civil.year);
    AppendTwoDigits(name, civil.month);
    AppendTwoDigits(name, civil.day);
    AppendTwoDigits(name, civil.hour);
    if (segmentMinutes < MINUTES_PER_HOUR)
        AppendTwoDigits(name, civil.minute);

    name += ".csv";
    return name;
}

enum class SegmentAction : uint8_t
{
    Append,  // into the open segment
    Open,    // no segment was open: open the one holding the row
    Roll     // close the open segment, then open the one holding the row
};

// Which segment of one stream a row goes to, from the row's own t. A segment covers [start, end). A row before the
// open segment's end stays in it (a late row never reopens a closed segment); a row at or after the end rolls to the
// segment holding it. A new segment never starts before the end of any earlier one, also after Close(), so neither a
// length changed by a reload nor a row after a close can bring back a closed name. Writer thread only.
class SegmentTracker
{
public:
    SegmentAction Place(uint64_t tMs, uint32_t segmentMinutes)
    {
        if (_open && tMs < _endMs)
            return SegmentAction::Append;

        uint32_t const minutes = SegmentMinutesOf(segmentMinutes);
        uint64_t const length = minutes * MS_PER_MINUTE;
        uint64_t const aligned = tMs / length * length;
        bool const wasOpen = _open;
        _startMs = std::max(aligned, _endMs);
        // Aligned, unless the row lies in a period already closed: then the length after the floor
        _endMs = aligned + length > _startMs ? aligned + length : _startMs + length;
        _minutes = minutes;
        _open = true;
        return wasOpen ? SegmentAction::Roll : SegmentAction::Open;
    }

    // At stop; the end stays the floor of the next segment
    void Close() { _open = false; }

    bool IsOpen() const { return _open; }
    uint64_t StartMs() const { return _startMs; }
    uint64_t EndMs() const { return _endMs; }
    std::string Name(Stream stream) const { return SegmentName(stream, _startMs, _minutes); }

private:
    bool _open = false;
    uint64_t _startMs = 0;
    uint64_t _endMs = 0;
    uint32_t _minutes = MINUTES_PER_HOUR;
};

struct StreamCounters
{
    uint64_t queued = 0;       // rows waiting for the writer
    uint64_t admitted = 0;     // rows queued since start
    uint64_t dropped = 0;      // rows dropped since start
    uint64_t peak = 0;         // most rows waiting at once since start
    uint64_t droppedHour = 0;  // rows dropped since the last drops rows
    uint64_t peakHour = 0;     // most rows waiting at once since the last drops rows
};

// Rows of each stream waiting for the writer, with the drop counts. A chunk that would take its stream over the cap
// is dropped whole and its rows counted; meta rows ignore the cap. Not locked: the caller holds the queue's mutex.
class QueueAccount
{
public:
    bool Admit(Stream stream, uint64_t rows, uint64_t cap)
    {
        StreamCounters& counters = _counters[StreamIndex(stream)];
        if (stream != Stream::Meta && counters.queued + rows > cap)
        {
            counters.dropped += rows;
            counters.droppedHour += rows;
            return false;
        }

        counters.queued += rows;
        counters.admitted += rows;
        counters.peak = std::max(counters.peak, counters.queued);
        counters.peakHour = std::max(counters.peakHour, counters.queued);
        return true;
    }

    // The writer took a chunk of the stream out of the queue
    void Taken(Stream stream, uint64_t rows)
    {
        StreamCounters& counters = _counters[StreamIndex(stream)];
        counters.queued -= std::min(rows, counters.queued);
    }

    // One drops meta row per stream, zeros included: a = rows dropped, b = peak rows waiting since the last call,
    // text = the stream. Starts the next window.
    void AppendDropsRows(std::string& out, uint64_t tMs)
    {
        for (size_t index = 0; index < STREAM_COUNT; ++index)
        {
            StreamCounters& counters = _counters[index];
            AppendMetaRow(out, MetaRow{tMs, META_DROPS, static_cast<int64_t>(counters.droppedHour),
                                       static_cast<int64_t>(counters.peakHour), STREAM_NAMES[index]});
            counters.droppedHour = 0;
            counters.peakHour = counters.queued;
        }
    }

    StreamCounters const& Of(Stream stream) const { return _counters[StreamIndex(stream)]; }

    uint64_t AdmittedTotal() const
    {
        uint64_t total = 0;
        for (StreamCounters const& counters : _counters)
            total += counters.admitted;

        return total;
    }

    uint64_t DroppedTotal() const
    {
        uint64_t total = 0;
        for (StreamCounters const& counters : _counters)
            total += counters.dropped;

        return total;
    }

private:
    std::array<StreamCounters, STREAM_COUNT> _counters{};
};
}  // namespace WorldLogFormat

#endif
