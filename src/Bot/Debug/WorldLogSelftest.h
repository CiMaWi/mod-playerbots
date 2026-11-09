/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGSELFTEST_H
#define _PLAYERBOT_WORLDLOGSELFTEST_H

#include <string>
#include <string_view>
#include <vector>

#include "WorldLogFormat.h"

// Checks of WorldLogFormat.h without files or server objects; shared by "playerbots worldlog selftest" and
// tests/world-log-selftest.cpp. Returns the number of checks and appends one line per failure.
namespace WorldLogFormat
{
namespace SelftestDetail
{
// UTC milliseconds of fixed instants (checked against Python's calendar.timegm)
constexpr uint64_t T_2026_11_09_13_45_12 = 1794231912000;
constexpr uint64_t T_2026_11_09_23_59_59 = 1794268799000;
constexpr uint64_t T_2026_11_10_00_00_00 = 1794268800000;
constexpr uint64_t T_2026_12_31_23_59_59 = 1798761599000;
constexpr uint64_t T_2027_01_01_00_00_00 = 1798761600000;
constexpr uint64_t T_2028_02_29_12_34_56 = 1835440496000;
constexpr uint32_t SHORT_SEGMENT_MINUTES = 5;
constexpr uint64_t QUEUE_CAP = 1000;
constexpr uint64_t QUEUE_EXTRA = 37;

inline size_t FieldCount(std::string_view row)
{
    if (!row.empty() && row.back() == ROW_END)
        row.remove_suffix(1);

    size_t fields = 1;
    for (char const c : row)
        if (c == EventLog::FIELD_SEPARATOR)
            ++fields;

    return fields;
}

inline std::string_view Field(std::string_view row, size_t index)
{
    if (!row.empty() && row.back() == ROW_END)
        row.remove_suffix(1);

    for (size_t i = 0; i < index; ++i)
    {
        size_t const comma = row.find(EventLog::FIELD_SEPARATOR);
        if (comma == std::string_view::npos)
            return {};

        row.remove_prefix(comma + 1);
    }

    return row.substr(0, row.find(EventLog::FIELD_SEPARATOR));
}

class Checker
{
public:
    explicit Checker(std::vector<std::string>& failures) : _failures(failures) {}

    void Check(bool ok, std::string_view what)
    {
        ++_checks;
        if (!ok)
            _failures.emplace_back(what);
    }

    void Equal(std::string_view actual, std::string_view expected, std::string_view what)
    {
        ++_checks;
        if (actual == expected)
            return;

        std::string line(what);
        line += ": got \"";
        line += actual;
        line += "\", want \"";
        line += expected;
        line += "\"";
        _failures.push_back(line);
    }

    uint32_t Checks() const { return _checks; }

private:
    std::vector<std::string>& _failures;
    uint32_t _checks = 0;
};

// (a) every row has its header's field count, words are cleaned, positions have one decimal
inline void CheckRows(Checker& checker)
{
    checker.Equal(StreamName(Stream::Recorder), "recorder", "stream names in enum order");
    checker.Check(FieldCount(StreamHeader(Stream::Event)) == 11, "event header has 11 columns");
    checker.Check(FieldCount(StreamHeader(Stream::State)) == 19, "state header has 19 columns");
    checker.Check(FieldCount(StreamHeader(Stream::Meta)) == 5, "meta header has 5 columns");
    checker.Check(FieldCount(StreamHeader(Stream::Decision)) == 13, "decision header has 13 columns");
    checker.Check(FieldCount(StreamHeader(Stream::Recorder)) == 10, "recorder header has 10 columns");
    checker.Check(
        StreamHeader(Stream::Event).substr(0, 2) == "t," && StreamHeader(Stream::State).substr(0, 2) == "t," &&
            StreamHeader(Stream::Meta).substr(0, 2) == "t," && StreamHeader(Stream::Decision).substr(0, 2) == "t,",
        "server streams start with t");

    EventRow event;
    event.t = T_2026_11_09_13_45_12;
    event.bot = 4242;
    event.type = "Gold,In\n";
    event.id = 7;
    event.amount = -350;
    event.place = Place{0, 12, 87, -8913.25f, 554.04f};
    std::string const eventRow = FormatEventRow(event);
    checker.Check(FieldCount(eventRow) == FieldCount(StreamHeader(Stream::Event)), "event row field count");
    checker.Check(!eventRow.empty() && eventRow.back() == ROW_END && eventRow.find(ROW_END) == eventRow.size() - 1,
                  "event row is one LF-terminated line");
    checker.Equal(eventRow, "1794231912000,4242,Gold_In_,other,7,-350,0,12,87,-8913.3,554.0\n", "event row");

    StateRow state;
    state.t = T_2026_11_09_13_45_12;
    state.bot = 17;
    state.place = Place{530, 3483, 3536, -0.04f, 1.95f};
    state.level = 61;
    state.xp = 1234;
    state.money = 98765;
    state.hpPct = Percent(50, 200);
    state.powerPct = Percent(1, 0);
    state.rpg = 3;
    state.target = 10129;
    state.flags = STATE_FLAG_COMBAT | STATE_FLAG_GROUPED;
    state.freeSlots = 12;
    state.itemLevel = 98;
    state.durabilityMinPct = Percent(130, 120);
    state.group = 4242;
    std::string stateRows;
    AppendStateRow(stateRows, state);
    checker.Check(FieldCount(stateRows) == FieldCount(StreamHeader(Stream::State)), "state row field count");
    checker.Equal(stateRows, "1794231912000,17,530,3483,3536,0.0,2.0,61,1234,98765,25,0,3,10129,65,12,98,100,4242\n",
                  "state row");
    StateRow human;
    human.flags = STATE_FLAG_HUMAN;
    AppendStateRow(stateRows, human);
    checker.Equal(Field(std::string_view(stateRows).substr(stateRows.find(ROW_END) + 1), 12), "-1",
                  "a human's rpg column is -1 by default");

    MetaRow meta{T_2026_11_09_13_45_12, META_START, CONTRACT_VERSION, 2000, ""};
    std::string const startText = StartText(1, Settings{});
    meta.text = startText;
    std::string const metaRow = FormatMetaRow(meta);
    checker.Check(FieldCount(metaRow) == FieldCount(StreamHeader(Stream::Meta)), "meta row field count");
    checker.Equal(metaRow, "1794231912000,start,1,2000,realm=1 segment=60 state=60\n", "meta start row");
    std::string const reloadText = ReloadText(ClampSettings(1, 0, 0));
    checker.Equal(FormatMetaRow(MetaRow{1, META_RELOAD, 5, 1, "a,b"}), "1,reload,5,1,a_b\n", "meta text cleaned");
    checker.Equal(reloadText, "queue=1", "reload text after clamping");

    std::string position;
    AppendPosition(position, -1.25f);
    position.push_back(' ');
    AppendPosition(position, 1.25f);
    position.push_back(' ');
    AppendPosition(position, -0.04f);
    position.push_back(' ');
    AppendPosition(position, 0.96f);
    checker.Equal(position, "-1.3 1.3 0.0 1.0", "positions round half away from zero, no -0.0");
}

// (b) flag bits and settings bounds
inline void CheckFlagsAndSettings(Checker& checker)
{
    constexpr std::array<uint32_t, 9> FLAGS = {STATE_FLAG_COMBAT,  STATE_FLAG_DEAD,     STATE_FLAG_GHOST,
                                               STATE_FLAG_MOUNTED, STATE_FLAG_TAXI,     STATE_FLAG_RESTING,
                                               STATE_FLAG_GROUPED, STATE_FLAG_INSTANCE, STATE_FLAG_HUMAN};
    uint32_t seen = 0;
    bool distinct = true;
    for (uint32_t const flag : FLAGS)
    {
        distinct = distinct && flag && !(flag & (flag - 1)) && !(seen & flag);
        seen |= flag;
    }
    checker.Check(distinct && seen == 511, "nine distinct flag bits 1..256");
    checker.Check(STATE_FLAG_COMBAT == 1 && STATE_FLAG_INSTANCE == 128 && STATE_FLAG_HUMAN == 256,
                  "combat 1, instance 128, human 256");

    Settings const defaults;
    checker.Check(defaults.stateSeconds == 60 && defaults.segmentMinutes == 60 && defaults.queueRows == 200000,
                  "defaults 60 s, 60 min, 200000 rows");
    Settings const low = ClampSettings(0, 0, 0);
    checker.Check(low.stateSeconds == 5 && low.segmentMinutes == 1 && low.queueRows == 1, "lower bounds");
    Settings const high = ClampSettings(99999, 999, 99999999);
    checker.Check(high.stateSeconds == 3600 && high.segmentMinutes == 60 && high.queueRows == 10000000, "upper bounds");
}

// (c) segment names: hourly and short segments, day, year and leap-day boundaries
inline void CheckNames(Checker& checker)
{
    uint64_t const hour = MINUTES_PER_HOUR * MS_PER_MINUTE;
    uint64_t const fiveMinutes = SHORT_SEGMENT_MINUTES * MS_PER_MINUTE;
    checker.Equal(SegmentName(Stream::Event, T_2026_11_09_13_45_12 / hour * hour, MINUTES_PER_HOUR),
                  "event-2026110913.csv", "hourly name");
    checker.Equal(SegmentName(Stream::State, T_2026_11_09_13_45_12 / fiveMinutes * fiveMinutes, SHORT_SEGMENT_MINUTES),
                  "state-202611091345.csv", "5-minute name");
    checker.Equal(SegmentName(Stream::Meta, T_2026_11_09_23_59_59, MINUTES_PER_HOUR), "meta-2026110923.csv",
                  "last hour of the day");
    checker.Equal(SegmentName(Stream::Meta, T_2026_11_10_00_00_00, MINUTES_PER_HOUR), "meta-2026111000.csv",
                  "first hour of the next day");
    checker.Equal(SegmentName(Stream::Event, T_2026_12_31_23_59_59, SHORT_SEGMENT_MINUTES), "event-202612312359.csv",
                  "last minute of the year");
    checker.Equal(SegmentName(Stream::Event, T_2027_01_01_00_00_00, MINUTES_PER_HOUR), "event-2027010100.csv",
                  "first hour of the year");
    checker.Equal(SegmentName(Stream::Decision, T_2028_02_29_12_34_56, MINUTES_PER_HOUR), "decision-2028022912.csv",
                  "leap day");
    checker.Equal(SegmentName(Stream::Recorder, 0, 1), "recorder-197001010000.csv", "epoch");
}

// (d) the cap drops new rows of that stream only and counts them; meta ignores it; the drops rows
inline void CheckQueue(Checker& checker)
{
    QueueAccount account;
    uint64_t admitted = 0;
    for (uint64_t push = 0; push < QUEUE_CAP + QUEUE_EXTRA; ++push)
        admitted += account.Admit(Stream::Event, 1, QUEUE_CAP) ? 1 : 0;

    checker.Check(admitted == QUEUE_CAP, "cap N admits N rows");
    checker.Check(account.Of(Stream::Event).dropped == QUEUE_EXTRA, "N+K pushes drop K rows");
    checker.Check(account.Admit(Stream::State, QUEUE_CAP, QUEUE_CAP), "another stream has its own cap");
    checker.Check(!account.Admit(Stream::State, 1, QUEUE_CAP), "a full stream drops");
    checker.Check(account.Admit(Stream::Meta, QUEUE_CAP * 2, QUEUE_CAP), "meta ignores the cap");
    checker.Check(account.Of(Stream::State).dropped == 1 && account.Of(Stream::Meta).dropped == 0 &&
                      account.Of(Stream::Decision).dropped == 0,
                  "drops are counted per stream");
    checker.Check(!account.Admit(Stream::Decision, QUEUE_CAP + 1, QUEUE_CAP) &&
                      account.Of(Stream::Decision).dropped == QUEUE_CAP + 1,
                  "a chunk over the cap is dropped whole");

    account.Taken(Stream::Event, QUEUE_CAP / 2);
    checker.Check(account.Of(Stream::Event).queued == QUEUE_CAP / 2, "the writer's take frees room");
    checker.Check(account.Admit(Stream::Event, QUEUE_CAP / 2, QUEUE_CAP), "freed room admits again");
    account.Taken(Stream::Event, QUEUE_CAP * 3);
    checker.Check(account.Of(Stream::Event).queued == 0, "queued never goes below zero");
    checker.Check(account.AdmittedTotal() == QUEUE_CAP + QUEUE_CAP / 2 + QUEUE_CAP + QUEUE_CAP * 2,
                  "admitted total over streams");
    checker.Check(account.DroppedTotal() == QUEUE_EXTRA + 1 + QUEUE_CAP + 1, "dropped total over streams");

    std::string drops;
    account.AppendDropsRows(drops, 5);
    checker.Equal(drops,
                  "5,drops,37,1000,event\n5,drops,1,1000,state\n5,drops,0,2000,meta\n5,drops,1001,0,decision\n"
                  "5,drops,0,0,recorder\n",
                  "one drops row per stream, zeros included");
    drops.clear();
    account.AppendDropsRows(drops, 6);
    checker.Equal(drops,
                  "6,drops,0,0,event\n6,drops,0,1000,state\n6,drops,0,2000,meta\n6,drops,0,0,decision\n"
                  "6,drops,0,0,recorder\n",
                  "the next window starts at zero drops and the rows still waiting");
    checker.Check(account.Of(Stream::Event).dropped == QUEUE_EXTRA, "totals survive the window");
}

// (e) the segment each row goes to, on a virtual clock
inline void CheckSegments(Checker& checker)
{
    uint64_t const minute = MS_PER_MINUTE;
    uint64_t const hourStart = T_2026_11_09_23_59_59 / (MINUTES_PER_HOUR * minute) * (MINUTES_PER_HOUR * minute);
    SegmentTracker tracker;
    checker.Check(!tracker.IsOpen(), "nothing open at first");
    checker.Check(tracker.Place(hourStart + 5 * minute, MINUTES_PER_HOUR) == SegmentAction::Open, "first row opens");
    checker.Equal(tracker.Name(Stream::Event), "event-2026110923.csv", "first segment");
    checker.Check(tracker.Place(hourStart + 59 * minute, MINUTES_PER_HOUR) == SegmentAction::Append,
                  "same hour appends");
    checker.Check(tracker.Place(hourStart + MINUTES_PER_HOUR * minute, MINUTES_PER_HOUR) == SegmentAction::Roll,
                  "the boundary rolls");
    checker.Equal(tracker.Name(Stream::Event), "event-2026111000.csv", "rolled over midnight");
    checker.Check(tracker.Place(hourStart + 30 * minute, MINUTES_PER_HOUR) == SegmentAction::Append &&
                      tracker.Name(Stream::Event) == "event-2026111000.csv",
                  "an older row stays in the open segment");
    checker.Check(tracker.Place(hourStart + 4 * MINUTES_PER_HOUR * minute, MINUTES_PER_HOUR) == SegmentAction::Roll &&
                      tracker.Name(Stream::Event) == "event-2026111003.csv",
                  "a gap skips the empty hours");

    // A reload shortens the segments: the open hour keeps its rows until its end, then 5-minute segments follow
    uint64_t const nextHour = hourStart + 5 * MINUTES_PER_HOUR * minute;
    checker.Check(tracker.Place(nextHour - minute, SHORT_SEGMENT_MINUTES) == SegmentAction::Append,
                  "a shorter length waits for the open segment's end");
    checker.Check(tracker.Place(nextHour + 7 * minute, SHORT_SEGMENT_MINUTES) == SegmentAction::Roll &&
                      tracker.Name(Stream::State) == "state-202611100405.csv" &&
                      tracker.EndMs() == nextHour + 10 * minute,
                  "then 5-minute segments");

    // A reload lengthens them again before the boundary: the next segment starts at the closed one's end
    checker.Check(tracker.Place(nextHour + 10 * minute, 10) == SegmentAction::Roll &&
                      tracker.StartMs() == nextHour + 10 * minute && tracker.EndMs() == nextHour + 20 * minute,
                  "aligned longer segment");
    SegmentTracker reloaded;
    reloaded.Place(nextHour, SHORT_SEGMENT_MINUTES);
    checker.Check(reloaded.Place(nextHour + 6 * minute, 10) == SegmentAction::Roll &&
                      reloaded.StartMs() == nextHour + SHORT_SEGMENT_MINUTES * minute &&
                      reloaded.Name(Stream::Meta) == "meta-202611100405.csv",
                  "a longer length never reopens the closed segment's name");

    tracker.Close();
    checker.Check(!tracker.IsOpen(), "closed at stop");
}
}  // namespace SelftestDetail

inline uint32_t Selftest(std::vector<std::string>& failures)
{
    SelftestDetail::Checker checker(failures);
    SelftestDetail::CheckRows(checker);
    SelftestDetail::CheckFlagsAndSettings(checker);
    SelftestDetail::CheckNames(checker);
    SelftestDetail::CheckQueue(checker);
    SelftestDetail::CheckSegments(checker);
    return checker.Checks();
}
}  // namespace WorldLogFormat

#endif
