/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGSELFTEST_H
#define _PLAYERBOT_WORLDLOGSELFTEST_H

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "WorldLogFormat.h"
#include "WorldLogRecorder.h"
#include "WorldLogStatus.h"

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
    std::string const startText = StartText(1, Settings{}, 9876);
    meta.text = startText;
    std::string const metaRow = FormatMetaRow(meta);
    checker.Check(FieldCount(metaRow) == FieldCount(StreamHeader(Stream::Meta)), "meta row field count");
    checker.Equal(metaRow, "1794231912000,start,1,2000,realm=1 segment=60 state=60 ring_bytes=9876\n",
                  "meta start row");
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
    checker.Check(ClampSettings(60, 7, 1).segmentMinutes == 6 && ClampSettings(60, 45, 1).segmentMinutes == 30 &&
                      ClampSettings(60, 59, 1).segmentMinutes == 30 && ClampSettings(60, 12, 1).segmentMinutes == 12,
                  "segment minutes round down to a divisor of 60");
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

    // After a close, a row of the closed period opens the next name, never the closed one
    SegmentTracker closed;
    closed.Place(hourStart, MINUTES_PER_HOUR);
    closed.Close();
    checker.Check(closed.Place(hourStart + 30 * minute, MINUTES_PER_HOUR) == SegmentAction::Open &&
                      closed.Name(Stream::Event) == "event-2026111000.csv" &&
                      closed.EndMs() == hourStart + 2 * MINUTES_PER_HOUR * minute,
                  "a closed name is never reopened");
    SegmentTracker odd;
    odd.Place(hourStart + 50 * minute, 45);
    checker.Check(odd.StartMs() == hourStart + 30 * minute && odd.EndMs() == hourStart + MINUTES_PER_HOUR * minute,
                  "a 45-minute length gives 30-minute segments inside the hour");
}
// (f) WL6: every fact type formats the event header's field count, the word tables cover their enums, the status
// reason scope nests and restores
inline void CheckEventWords(Checker& checker)
{
    constexpr std::string_view FACT_TYPES[] = {"XpGain",    "Kill",       "QuestAccept", "QuestAbandon", "Equip",
                                               "SkillUp",   "SpellLearn", "TaxiNode",    "Flight",       "Hearth",
                                               "MapChange", "Resurrect",  "Status"};
    bool fields = true;
    for (std::string_view const type : FACT_TYPES)
    {
        EventRow row;
        row.t = T_2026_11_09_13_45_12;
        row.bot = 17;
        row.type = type;
        row.source = EventLog::RankWord(1);
        row.id = 8690;
        row.amount = -3;
        fields = fields && FieldCount(FormatEventRow(row)) == FieldCount(StreamHeader(Stream::Event));
    }
    checker.Check(fields, "every WL6 event type formats 11 fields");

    std::string ranks;
    for (uint32_t rank = 0; rank <= EventLog::RANK_COUNT; ++rank)
        ranks.append(EventLog::RankWord(rank)).push_back(' ');
    checker.Equal(ranks, "normal elite rare_elite world_boss rare other ", "a rank word per creature rank");

    std::string sources;
    for (uint32_t source = 0; source <= EventLog::XP_SOURCE_COUNT; ++source)
        sources.append(EventLog::XpSourceWord(source)).push_back(' ');
    checker.Equal(sources, "kill quest quest explore other other ", "an XP word per XP source");

    std::string kinds;
    for (uint32_t kind = 0; kind <= EventLog::MAP_KIND_COUNT; ++kind)
        kinds.append(EventLog::MapKindWord(static_cast<EventLog::MapKind>(kind))).push_back(' ');
    checker.Equal(kinds, "world dungeon raid battleground arena other ", "a map word per map kind");

    checker.Equal(EventLog::SpellLearnWord("train", false, false), "trainer", "a training action learns at a trainer");
    checker.Equal(EventLog::SpellLearnWord("other", true, true), "trainer", "a paid learn is a trainer's");
    checker.Equal(EventLog::SpellLearnWord("other", false, true), "talent", "a talent spell");
    checker.Equal(EventLog::SpellLearnWord("quest", false, false), "quest", "a quest action's learn");
    checker.Equal(EventLog::SpellLearnWord("equip upgrades", false, false), "other", "any other learn");

    checker.Equal(EventLog::ResurrectWord("corpse", false, 0.5f), "corpse", "a corpse run");
    checker.Equal(EventLog::ResurrectWord("other", true, 0.5f), "spirit_healer", "sickness: a spirit healer");
    checker.Equal(EventLog::ResurrectWord("accept resurrect", false, 0.0f), "spell", "nothing restored: a spell");
    checker.Equal(EventLog::ResurrectWord("other", false, 1.0f), "other", "a full restore: other");

    std::string reasons = WorldLog::CurrentStatusReason();
    {
        WorldLog::StatusReason const outer("pick");
        reasons.append(" ").append(WorldLog::CurrentStatusReason());
        {
            WorldLog::StatusReason const inner("arrived");
            reasons.append(" ").append(WorldLog::CurrentStatusReason());
        }
        reasons.append(" ").append(WorldLog::CurrentStatusReason());
    }
    reasons.append(" ").append(WorldLog::CurrentStatusReason());
    checker.Equal(reasons, "other pick arrived pick other", "the status reason nests and restores");
}

// (g) WL7: decision rows have the header's 13 fields with 0, 1 and 3 alternatives, clean their words and keep two
// decimals; the alternatives keep the best three; the leg chain caps its length; a word per RPG status; trip ids
// count from 1
inline void CheckDecisions(Checker& checker)
{
    size_t const columns = FieldCount(StreamHeader(Stream::Decision));
    DecisionRow row;
    row.t = T_2026_11_09_13_45_12;
    row.bot = 17;
    row.kind = DecisionKind::Errand;
    row.choice = "skip";
    row.reason = "not_worth";
    row.value = 95.004f;
    std::string const none = FormatDecisionRow(row);
    checker.Equal(none, "1794231912000,17,errand,skip,not_worth,95.00,,,,,,,0\n", "a decision without alternatives");
    checker.Check(FieldCount(none) == columns, "decision row field count, no alternative");

    row.kind = DecisionKind::Route;
    row.choice = "walk>flight,walk";
    row.reason = "";
    row.value = -0.004f;
    row.alternatives = {{"legs", 3.0f}};
    row.ref = 42;
    std::string const one = FormatDecisionRow(row);
    checker.Equal(one, "1794231912000,17,route,walk>flight_walk,other,0.00,legs,3.00,,,,,42\n",
                  "one alternative; a comma cleaned, an empty word is other, no -0.00");
    checker.Check(FieldCount(one) == columns, "decision row field count, one alternative");

    row.kind = DecisionKind::RpgStatus;
    row.choice = WorldLog::NewRpgStatusWord(5);
    row.reason = "weights";
    row.value = 40.0f;
    row.alternatives = {{"rest", 1.5f}, {"go grind", -2.125f}, {"idle", 1234567.891f}, {"dropped", 9.0f}};
    row.ref = 0;
    std::string const three = FormatDecisionRow(row);
    checker.Equal(three,
                  "1794231912000,17,rpg_status,do quest,weights,40.00,rest,1.50,go grind,-2.13,idle,"
                  "1234567.88,0\n",
                  "three alternatives, a fourth ignored, half away from zero");
    checker.Check(FieldCount(three) == columns, "decision row field count, three alternatives");
    checker.Check(three.find(ROW_END) == three.size() - 1, "one decision row is one line");

    std::string kinds;
    for (size_t kind = 0; kind <= DECISION_KIND_COUNT; ++kind)
        kinds.append(DecisionKindWord(static_cast<DecisionKind>(kind))).push_back(' ');
    checker.Equal(kinds, "rpg_status route trip errand inn upkeep death other ", "a word per decision kind");

    std::string value;
    AppendValue(value, std::numeric_limits<float>::infinity());
    value.push_back(' ');
    AppendValue(value, 3.0e30f);
    checker.Equal(value, "0.00 1000000000000.00", "not finite is 0, huge values clamp");

    Alternatives highest;
    for (float const weight : {5.0f, 20.0f, 1.0f, 20.0f, 7.0f})
        highest.Offer({"w", weight}, false);
    Alternatives lowest;
    for (float const seconds : {300.0f, 120.0f, 900.0f, 60.0f})
        lowest.Offer({"s", seconds}, true);
    checker.Check(
        highest.Count() == 3 && highest[0].value == 20.0f && highest[1].value == 20.0f && highest[2].value == 7.0f,
        "Offer keeps the three highest");
    checker.Check(
        lowest.Count() == 3 && lowest[0].value == 60.0f && lowest[1].value == 120.0f && lowest[2].value == 300.0f,
        "Offer keeps the three lowest");

    WordChain chain;
    chain.Add("walk");
    chain.Add("flight");
    chain.Add("walk");
    checker.Equal(chain.View(), "walk>flight>walk", "a leg chain");
    WordChain longChain;
    for (int i = 0; i < 20; ++i)
        longChain.Add("transport");
    checker.Check(longChain.View().size() <= CHAIN_CAPACITY && longChain.View().back() == CHAIN_MORE &&
                      longChain.View().substr(0, 20) == "transport>transport>",
                  "a long chain caps its length and ends in +");
    checker.Equal(WordChain().View(), "", "an empty chain");

    checker.Equal(AfterPrefix("abort:deadline", "abort:"), "deadline", "the abort reason after its prefix");
    checker.Equal(AfterPrefix("stopped", "abort:"), "stopped", "no prefix: unchanged");
    checker.Check(WalkSeconds(70.0f, 7.0f) == 10.0f && WalkSeconds(10.0f, 0.0f) == 10.0f, "walk seconds");

    std::string statuses;
    for (int32_t status = -1; status <= WorldLog::RPG_STATUS_WORD_COUNT; ++status)
        statuses.append(WorldLog::NewRpgStatusWord(status)).push_back('|');
    checker.Equal(statuses,
                  "other|idle|go grind|go camp|wander random|wander npc|do quest|travel flight|rest|outdoor pvp|"
                  "do gather|travel|errands|other|",
                  "a word per RPG status, other outside");

    WorldLog::TripCounter trips;
    uint32_t const first = trips.Next();
    uint32_t const second = trips.Next();
    checker.Check(first == 1 && second == 2, "trip ids count from 1");
}

// The last row of rows (without its end)
inline std::string_view LastRow(std::string_view rows)
{
    if (!rows.empty() && rows.back() == ROW_END)
        rows.remove_suffix(1);

    size_t const start = rows.rfind(ROW_END);
    return start == std::string_view::npos ? rows : rows.substr(start + 1);
}

constexpr uint32_t RECORDER_T0 = 1000000;  // the virtual server clock's start, ms
constexpr uint32_t SECOND_MS = 1000;
constexpr uint32_t MINUTE_MS = 60 * SECOND_MS;
constexpr int32_t RPG_DO_QUEST_STATUS = 5;

// Positions once a second for seconds seconds, x growing by step yards each; the first due push (1-based), else 0
inline uint32_t FirstDue(Recorder& recorder, WordTable& words, uint32_t seconds, float step, int32_t rpg)
{
    for (uint32_t i = 0; i < seconds; ++i)
    {
        RecorderPosition const position{RECORDER_T0 + i * SECOND_MS, 100.0f + step * static_cast<float>(i), 50.0f, 1,
                                        static_cast<int16_t>(rpg)};
        if (recorder.PushPosition(words, position, StillWatched(true, false, false, rpg)))
            return i + 1;
    }

    return 0;
}

// The trigger row's name and detail after one event on a fresh recorder ("-" when no case was due)
inline std::string EventTrigger(std::string_view type, std::string_view source)
{
    WordTable words;
    Recorder recorder;
    recorder.Allocate(RecorderSettings{});
    if (!recorder.PushEvent(words, RECORDER_T0, type, source, 0, 0))
        return "-";

    std::string rows;
    recorder.AppendCase(rows, words, 1, 1, RECORDER_T0, T_2026_11_09_13_45_12);
    std::string_view const last = LastRow(rows);
    std::string out(Field(last, 4));
    out.push_back('/');
    out.append(Field(last, 5));
    return out;
}

// (h) WL8: the recorder's ring, words, triggers, case limit and rows on a virtual clock
inline void CheckRecorder(Checker& checker)
{
    RecorderRing<int> ring;
    ring.Allocate(3);
    for (int i = 1; i <= 5; ++i)
        ring.Push(i);
    checker.Check(ring.Count() == 3 && ring.FromOldest(0) == 3 && ring.FromOldest(1) == 4 && ring.FromOldest(2) == 5,
                  "a ring keeps the newest entries, oldest first");

    RecorderSettings const defaults;
    checker.Check(
        defaults.seconds == 300 && defaults.actions == 128 && defaults.events == 64 && defaults.cooldownSeconds == 600,
        "recorder defaults");
    RecorderSettings const low = ClampRecorderSettings(0, 0, 0, 0);
    RecorderSettings const high = ClampRecorderSettings(99999, 99999, 99999, 999999);
    checker.Check(low.seconds == 0 && low.actions == 1 && low.events == 1 && low.cooldownSeconds == 0,
                  "recorder lower bounds (0 seconds = off)");
    checker.Check(high.seconds == 3600 && high.actions == 4096 && high.events == 4096 && high.cooldownSeconds == 86400,
                  "recorder upper bounds");
    checker.Check(RingArrayBytes(low) == 0 && RingArrayBytes(defaults) == 300 * sizeof(RecorderPosition) +
                                                                              128 * sizeof(RecorderAction) +
                                                                              64 * sizeof(RecorderEvent),
                  "ring bytes: none when off, the three arrays else");
    Recorder off;
    checker.Check(!off.Allocate(low) && !off.On() && !off.Unset() && !off.PositionDue(RECORDER_T0),
                  "seconds 0 leaves the recorder off for good");

    WordTable fewWords(3);
    uint16_t const go = fewWords.Intern("go");
    uint16_t const rpg = fewWords.Intern("rpg");
    checker.Check(go == 1 && rpg == 2 && fewWords.Intern("go") == go && fewWords.Intern("full") == WORD_UNKNOWN &&
                      fewWords.Overflow() == 1 && fewWords.Count() == 3,
                  "words: one id per word, ? when the table is full");
    checker.Equal(std::string(fewWords.Word(go)) + "|" + std::string(fewWords.Word(WORD_UNKNOWN)), "go|?",
                  "word lookup");

    Recorder cached;
    int const actionA = 0;
    int const actionB = 0;
    Recorder::ActionWords hit;
    cached.CacheAction(&actionA, 5, Recorder::ActionWords{go, rpg});
    checker.Check(cached.CachedAction(&actionA, 5, hit) && hit.name == go && hit.trigger == rpg &&
                      !cached.CachedAction(&actionA, 6, hit) && !cached.CachedAction(&actionB, 5, hit),
                  "action cache: a hit needs the same action and trigger");

    checker.Equal(EventTrigger("Stuck", "offmesh"), "stuck/offmesh", "a Stuck event makes a case");
    checker.Equal(EventTrigger("Travel", "abort:stopped"), "abort/stopped", "a trip abort makes a case");
    checker.Equal(EventTrigger("Travel", "leg_failed:timeout"), "leg_failed/timeout", "a leg failure makes a case");
    checker.Equal(
        EventTrigger("Travel", "start") + EventTrigger("Travel", "leg_done:walk") + EventTrigger("Travel", "arrived"),
        "---", "other Travel events make none");
    checker.Equal(
        EventTrigger("HonestyTeleport", "spirit_healer_fallback") + EventTrigger("HonestyTeleport", "ghost_to_corpse") +
            EventTrigger("HonestyTeleport", "repop_graveyard") + EventTrigger("HonestyTeleport", "off_mesh_snap"),
        "teleport/spirit_healer_fallbackteleport/ghost_to_corpseteleport/repop_graveyard"
        "teleport/off_mesh_snap",
        "last-resort teleports make a case");
    checker.Equal(EventTrigger("HonestyTeleport", "mgr_to_player") + EventTrigger("Death", "creature") +
                      EventTrigger("Kill", "normal"),
                  "---", "manager teleports, a first death and other events make none");

    WordTable words;
    Recorder deaths9;
    deaths9.Allocate(defaults);
    deaths9.PushEvent(words, RECORDER_T0, "Death", "creature", 0, 1);
    Recorder deaths11;
    deaths11.Allocate(defaults);
    deaths11.PushEvent(words, RECORDER_T0, "Death", "creature", 0, 1);
    checker.Check(deaths9.PushEvent(words, RECORDER_T0 + 9 * MINUTE_MS, "Death", "player", 0, 1) &&
                      !deaths11.PushEvent(words, RECORDER_T0 + 11 * MINUTE_MS, "Death", "player", 0, 1),
                  "a second death 9 minutes after the first makes a case, 11 minutes after none");

    Recorder still;
    still.Allocate(defaults);
    checker.Check(FirstDue(still, words, 400, 0.005f, RPG_DO_QUEST_STATUS) == STILL_SECONDS + 1,
                  "300 s within 3 yards while busy makes a case at the 300th second");
    std::string stillRows;
    uint32_t const stillCount =
        still.AppendCase(stillRows, words, 3, 42, RECORDER_T0 + STILL_SECONDS * SECOND_MS, T_2026_11_09_13_45_12);
    std::string_view const stillTrigger = LastRow(stillRows);
    checker.Check(stillCount == STILL_SECONDS + 1 && Field(stillTrigger, 3) == "trigger" &&
                      Field(stillTrigger, 4) == "still" && Field(stillTrigger, 6) == "300",
                  "the still case: 300 positions, then its trigger row with the seconds");
    bool fieldsOk = true;
    std::string_view rest = stillRows;
    while (!rest.empty())
    {
        size_t const end = rest.find(ROW_END);
        fieldsOk = fieldsOk && FieldCount(rest.substr(0, end + 1)) == FieldCount(StreamHeader(Stream::Recorder)) &&
                   Field(rest, 0) == "3";
        rest.remove_prefix(end + 1);
    }
    checker.Check(fieldsOk, "every case row has the recorder header's 10 fields and the case id");
    Recorder moving;
    moving.Allocate(defaults);
    Recorder idle;
    idle.Allocate(defaults);
    Recorder resting;
    resting.Allocate(defaults);
    checker.Check(FirstDue(moving, words, 400, 0.05f, RPG_DO_QUEST_STATUS) == 0 &&
                      FirstDue(idle, words, 400, 0.0f, RECORDER_RPG_IDLE) == 0 &&
                      FirstDue(resting, words, 400, 0.0f, RECORDER_RPG_REST) == 0,
                  "moving 3 yards a minute, idle or resting makes no still case");
    Recorder fighting;
    fighting.Allocate(defaults);
    uint32_t fightingDue = 0;
    for (uint32_t i = 0; i < 400 && !fightingDue; ++i)
    {
        uint32_t const ms = RECORDER_T0 + i * SECOND_MS;
        if (i % 60 == 59)
            fighting.PushEvent(words, ms, i % 120 == 59 ? "Kill" : "XpGain", "normal", 0, 0);

        RecorderPosition const position{ms, 100.0f, 50.0f, 1, static_cast<int16_t>(RPG_DO_QUEST_STATUS)};
        if (fighting.PushPosition(words, position, StillWatched(true, false, false, RPG_DO_QUEST_STATUS)))
            fightingDue = i + 1;
    }
    checker.Check(fightingDue == 0, "a kill or XP gain every minute in one spot makes no still case");
    checker.Check(!StillWatched(false, false, false, RPG_DO_QUEST_STATUS) &&
                      !StillWatched(true, true, false, RPG_DO_QUEST_STATUS) &&
                      !StillWatched(true, false, true, RPG_DO_QUEST_STATUS) &&
                      StillWatched(true, false, false, RPG_DO_QUEST_STATUS),
                  "the still watch leaves out the dead, flights and transports");

    Recorder::ActionWords const failing{words.Intern("go"), words.Intern("rpg")};
    Recorder::ActionWords const other{words.Intern("sell"), words.Intern("rpg")};
    Recorder streak;
    streak.Allocate(defaults);
    bool early = false;
    for (uint32_t i = 1; i < FAIL_STREAK; ++i)
        early = streak.PushAction(RECORDER_T0 + i, failing, false) || early;
    bool const tenth = streak.PushAction(RECORDER_T0 + FAIL_STREAK, failing, false);
    checker.Check(!early && tenth, "the same action failing 10 times in a row makes a case at the 10th, not the 9th");
    Recorder reset;
    reset.Allocate(defaults);
    bool resetDue = false;
    for (uint32_t i = 0; i < FAIL_STREAK - 1; ++i)
        resetDue = reset.PushAction(RECORDER_T0 + i, failing, false) || resetDue;
    resetDue = reset.PushAction(RECORDER_T0 + 20, failing, true) || resetDue;
    for (uint32_t i = 0; i < FAIL_STREAK - 1; ++i)
        resetDue = reset.PushAction(RECORDER_T0 + 30 + i, failing, false) || resetDue;
    resetDue = reset.PushAction(RECORDER_T0 + 50, other, false) || resetDue;
    for (uint32_t i = 0; i < FAIL_STREAK - 1; ++i)
        resetDue = reset.PushAction(RECORDER_T0 + 60 + i, failing, false) || resetDue;
    checker.Check(!resetDue, "a success or another action between resets the streak");

    Recorder limit;
    limit.Allocate(defaults);
    std::string limitRows;
    bool const firstCase = limit.PushEvent(words, RECORDER_T0, "Stuck", "offmesh", 0, 0);
    limit.AppendCase(limitRows, words, 1, 1, RECORDER_T0, T_2026_11_09_13_45_12);
    bool const within = limit.PushEvent(words, RECORDER_T0 + 5 * MINUTE_MS, "Stuck", "offmesh", 0, 0);
    bool const after = limit.PushEvent(words, RECORDER_T0 + 10 * MINUTE_MS + SECOND_MS, "Stuck", "offmesh", 0, 0);
    checker.Check(firstCase && !within && limit.Suppressed() == 1 && after,
                  "one case per bot per 10 minutes: none 5 minutes later, one again after 10");
    uint32_t const dueRows =
        limit.AppendCase(limitRows, words, 2, 1, RECORDER_T0 + 10 * MINUTE_MS + SECOND_MS, T_2026_11_09_13_45_12);
    uint32_t const againRows =
        limit.AppendCase(limitRows, words, 3, 1, RECORDER_T0 + 11 * MINUTE_MS, T_2026_11_09_13_45_12);
    checker.Check(dueRows > 0 && againRows == 0 && limit.Cases() == 2, "a dump takes the due case once");

    Recorder marked;
    marked.Allocate(defaults);
    marked.RequestCase("console");
    std::string markRows;
    bool const markDue = marked.PushPosition(words, RecorderPosition{RECORDER_T0, 1.0f, 2.0f, 0, 0}, false);
    marked.AppendCase(markRows, words, 4, 1, RECORDER_T0, T_2026_11_09_13_45_12);
    checker.Check(markDue && Field(LastRow(markRows), 4) == "mark" && Field(LastRow(markRows), 5) == "console",
                  "a requested case comes at the next position push");

    Recorder window;
    window.Allocate(ClampRecorderSettings(10, 8, 8, 600));
    window.PushAction(RECORDER_T0, failing, true);
    window.PushPosition(words, RecorderPosition{RECORDER_T0 + 15 * SECOND_MS, 1.0f, 2.0f, 0, 0}, false);
    window.PushEvent(words, RECORDER_T0 + 20 * SECOND_MS, "Stuck", "offmesh", 0, 0);
    std::string windowRows;
    uint32_t const windowCount =
        window.AppendCase(windowRows, words, 5, 1, RECORDER_T0 + 20 * SECOND_MS, T_2026_11_09_13_45_12);
    checker.Check(windowCount == 3 && windowRows.find(",action,") == std::string::npos,
                  "a case holds only the last RecorderSeconds");

    // A whole case: rows by time, event and action rows at the latest position, then the trigger row
    Recorder whole;
    whole.Allocate(defaults);
    whole.PushPosition(words, RecorderPosition{RECORDER_T0, -8913.24f, 554.06f, 0, RPG_DO_QUEST_STATUS}, true);
    whole.PushAction(RECORDER_T0 + 400, failing, false);
    bool const wholeDue = whole.PushEvent(words, RECORDER_T0 + 900, "Stuck", "offmesh", 5, 77);
    std::string wholeRows;
    uint32_t const wholeCount =
        whole.AppendCase(wholeRows, words, 7, 42, RECORDER_T0 + SECOND_MS, T_2026_11_09_13_45_12);
    checker.Check(wholeDue && wholeCount == 4, "a whole case: three entries and the trigger");
    checker.Equal(wholeRows,
                  "7,1794231911000,42,pos,,,5,-8913.2,554.1,0\n"
                  "7,1794231911400,42,action,go,rpg,0,-8913.2,554.1,0\n"
                  "7,1794231911900,42,event,Stuck,offmesh;77,5,-8913.2,554.1,0\n"
                  "7,1794231912000,42,trigger,stuck,offmesh,0,-8913.2,554.1,0\n",
                  "a whole case's rows");
}

constexpr uint64_t SIZED_PLAYERS = 2500;     // the queue cap's sizing: 2000+ bots and a few real players
constexpr uint64_t SIZED_STALL_MINUTES = 5;  // a writer that takes nothing for this long
constexpr uint64_t SIZED_MARK_BOTS = 400;    // bots within a mark's reach in a crowded city
constexpr uint64_t PASSES_PER_MINUTE = 60 / FOCUS_SECONDS;

// (i) WL14: the focus range, mark rows, a mark against the case limit, the queue cap's sizing
inline void CheckFocusAndMarks(Checker& checker)
{
    FocusPoint const player{0, 0, 100.0f, 200.0f};
    std::array<FocusPoint, 2> const players = {player, FocusPoint{1, 0, 0.0f, 0.0f}};
    checker.Check(InFocus(players, player, FOCUS_YARDS), "a real player is in its own focus");
    checker.Check(InFocus(players, FocusPoint{0, 0, 160.0f, 280.0f}, FOCUS_YARDS) &&
                      !InFocus(players, FocusPoint{0, 0, 160.1f, 280.0f}, FOCUS_YARDS),
                  "focus reaches 100 yd and no further");
    checker.Check(!InFocus(players, FocusPoint{2, 0, 100.0f, 200.0f}, FOCUS_YARDS) &&
                      !InFocus(players, FocusPoint{0, 7, 100.0f, 200.0f}, FOCUS_YARDS),
                  "another map or instance is out of focus");
    checker.Check(InFocus(players, FocusPoint{1, 0, -70.0f, 70.0f}, FOCUS_YARDS), "every real player's range counts");
    checker.Check(!InFocus(std::array<FocusPoint, 0>{}, player, FOCUS_YARDS), "no real player, no focus");
    checker.Check(Near(player, FocusPoint{0, 0, 136.0f, 248.0f}, MARK_YARDS) &&
                      !Near(player, FocusPoint{0, 0, 136.1f, 248.0f}, MARK_YARDS),
                  "a mark reaches 60 yd and no further");

    Place const place{571, 3537, 4100, 5804.25f, -624.5f};
    std::string const markText = MarkText(place, "  stuck at the bridge, again  ");
    checker.Equal(markText, "x=5804.3 y=-624.5 map=571 area=4100 stuck at the bridge, again",
                  "mark text: the place first, the user's text last and trimmed");
    checker.Equal(FormatMetaRow(MetaRow{T_2026_11_09_13_45_12, META_MARK, 9, 3, markText}),
                  "1794231912000,mark,9,3,x=5804.3 y=-624.5 map=571 area=4100 stuck at the bridge_ again\n",
                  "a mark row: a = the player, b = the bots asked, the text's commas replaced");
    checker.Check(MarkUserText("   ").empty() && MarkUserText("").empty(), "a blank mark text is empty");
    checker.Check(MarkUserText(std::string(MARK_TEXT_MAX + 50, 'a')).size() == MARK_TEXT_MAX,
                  "a mark keeps 200 bytes of text");
    std::string const accented = std::string(MARK_TEXT_MAX - 1, 'a') +
                                 "\xC3\xA4"
                                 "bb";
    checker.Check(MarkUserText(accented).size() == MARK_TEXT_MAX - 1, "the text is cut at a UTF-8 character boundary");

    WordTable words;
    RecorderSettings const defaults;
    Recorder marked;
    marked.Allocate(defaults);
    std::string rows;
    bool const stuck = marked.PushEvent(words, RECORDER_T0, "Stuck", "offmesh", 0, 0);
    marked.RequestCase("user", static_cast<int64_t>(T_2026_11_09_13_45_12));
    bool const waiting = marked.PushPosition(words, RecorderPosition{RECORDER_T0 + SECOND_MS, 1.0f, 2.0f, 0, 0}, false);
    marked.AppendCase(rows, words, 1, 1, RECORDER_T0 + SECOND_MS, T_2026_11_09_13_45_12);
    checker.Check(stuck && !waiting, "a mark waits while another case is due");
    rows.clear();
    bool const markDue =
        marked.PushPosition(words, RecorderPosition{RECORDER_T0 + 2 * SECOND_MS, 1.0f, 2.0f, 0, 0}, false);
    marked.AppendCase(rows, words, 2, 1, RECORDER_T0 + 2 * SECOND_MS, T_2026_11_09_13_45_12);
    std::string_view const trigger = LastRow(rows);
    checker.Check(
        markDue && Field(trigger, 4) == "mark" && Field(trigger, 5) == "user" && Field(trigger, 6) == "1794231912000",
        "a mark right after a case makes its own; the trigger row's a is the mark's t");
    bool const stuckAfter = marked.PushEvent(words, RECORDER_T0 + 5 * MINUTE_MS, "Stuck", "offmesh", 0, 0);
    marked.RequestCase("user", 1);
    bool const secondMark =
        marked.PushPosition(words, RecorderPosition{RECORDER_T0 + 6 * MINUTE_MS, 1.0f, 2.0f, 0, 0}, false);
    checker.Check(!stuckAfter && secondMark && marked.Suppressed() == 1,
                  "after a mark other triggers keep the case limit, a second mark does not");

    // A stalled writer: every player in focus (one full and eleven focus passes a minute), then a mark's cases
    Settings const settings;
    QueueAccount account;
    bool admitted = true;
    for (uint64_t pass = 0; pass < SIZED_STALL_MINUTES * PASSES_PER_MINUTE; ++pass)
        admitted = account.Admit(Stream::State, SIZED_PLAYERS, settings.queueRows) && admitted;
    uint64_t const caseRows = defaults.seconds + defaults.actions + defaults.events + 1;
    for (uint64_t bot = 0; bot < SIZED_MARK_BOTS; ++bot)
        admitted = account.Admit(Stream::Recorder, caseRows, settings.queueRows) && admitted;
    checker.Check(admitted && account.DroppedTotal() == 0,
                  "the default cap holds 5 minutes of 2500 players in focus and 400 full cases");
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
    SelftestDetail::CheckEventWords(checker);
    SelftestDetail::CheckDecisions(checker);
    SelftestDetail::CheckRecorder(checker);
    SelftestDetail::CheckFocusAndMarks(checker);
    return checker.Checks();
}
}  // namespace WorldLogFormat

#endif
