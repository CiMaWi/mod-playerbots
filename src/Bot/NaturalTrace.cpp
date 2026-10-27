/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalTrace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
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

// The rows of one unit on their way to the file
struct Batch
{
    std::string MatchId;
    std::string Directory;
    std::string Date;
    std::vector<Row> Rows;
};

struct UnitState
{
    std::string MatchId;
    std::string Directory;
    std::string Date;
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

    AddResult Add(uint64 guid, std::string matchId, std::string directory, std::string date, Row start, uint32 maxUnits,
                  uint32 interval, uint32 maxRows)
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_units.count(guid))
            return AddResult::Exists;

        if (_units.size() >= maxUnits)
        {
            ++Refused;
            return AddResult::Full;
        }

        Insert(guid, std::move(matchId), std::move(directory), std::move(date), start, interval, maxRows);
        return AddResult::Added;
    }

    // All of a group (the players of an organic match, each with its start row) or none, under one lock: Exists
    // when one of them is traced already, Full when they do not all fit under maxUnits (all counted as refused)
    AddResult AddGroup(std::vector<std::pair<uint64, Row>> members, std::string const& matchId,
                       std::string const& directory, std::string const& date, uint32 maxUnits, uint32 interval,
                       uint32 maxRows)
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
            Insert(member.first, matchId, directory, date, member.second, interval, maxRows);

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

private:
    using Units = std::unordered_map<uint64, UnitState>;

    // The lock is held and the unit is not traced: its state, start row, mark and count
    void Insert(uint64 guid, std::string matchId, std::string directory, std::string date, Row& start, uint32 interval,
                uint32 maxRows)
    {
        UnitState& state = _units[guid];
        state.MatchId = std::move(matchId);
        state.Directory = std::move(directory);
        state.Date = std::move(date);
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
        batch.Directory = state.Directory;
        batch.Date = state.Date;
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
        Batch batch;
        batch.MatchId = std::move(state.MatchId);
        batch.Directory = std::move(state.Directory);
        batch.Date = std::move(state.Date);
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

std::atomic<uint64> QueuedBatches{0};
std::atomic<uint64> WrittenRows{0};
std::atomic<uint64> WrittenBytes{0};
std::atomic<bool> DirectoryLogged{false};
// .playerbots natural sample: -1 = Playerbots.Natural.Trace.SampleEvery, else the N set by the command (0 = off)
std::atomic<int32> SampleOverride{-1};
// .playerbots natural organic: -1 = Playerbots.Natural.Trace.Organic, else 0 or 1 set by the command
std::atomic<int32> OrganicOverride{-1};
// Set by Shutdown (before the sessions are kicked, while map threads still update): no trace begins after it
std::atomic<bool> Stopping{false};

// Map thread: hands a batch to the writer thread, which formats it (a pointer move here)
void WriteBatch(Batch&& batch)
{
    ++QueuedBatches;
    std::string directory = batch.Directory;
    std::string date = batch.Date;
    auto shared = std::make_shared<Batch>(std::move(batch));
    PvpTestRecordWriter::Instance().WriteLine(
        "trace", std::move(date), std::string(),
        [shared]()
        {
            std::string text = BatchText(*shared);
            WrittenRows += shared->Rows.size();
            WrittenBytes += text.size();
            return text;
        },
        std::move(directory));
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

void LogDirectoryOnce(std::string const& directory)
{
    if (!DirectoryLogged.exchange(true))
        LOG_INFO("playerbots", "NATURAL: traces go to {}trace-<YYYYMMDD>.jsonl", directory);
}
}  // namespace

bool NaturalTrace::Begin(Player* player, std::string const& matchId, char side, char const* source)
{
    if (!sPlayerbotAIConfig.NaturalTraceEnable || !player || Stopping.load(std::memory_order_relaxed))
        return false;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "NaturalTraceStart");
    std::string const directory = TraceDirectory();
    AddResult const result =
        Global().Add(player->GetGUID().GetCounter(), matchId, directory, Today(), StartRow(player, side, source),
                     sPlayerbotAIConfig.NaturalTraceMaxUnits, sPlayerbotAIConfig.NaturalTraceIntervalMs,
                     sPlayerbotAIConfig.NaturalTraceMaxRowsPerUnit);
    if (result == AddResult::Added)
        LogDirectoryOnce(directory);

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

    std::string const directory = TraceDirectory();
    if (Global().AddGroup(std::move(members), matchId, directory, Today(), sPlayerbotAIConfig.NaturalTraceMaxUnits,
                          sPlayerbotAIConfig.NaturalTraceIntervalMs,
                          sPlayerbotAIConfig.NaturalTraceMaxRowsPerUnit) == AddResult::Added)
        LogDirectoryOnce(directory);

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
        "NATURAL trace: enable {}, organic {}, sample every {} ({}), interval {} ms, max units {}, dir {}",
        sPlayerbotAIConfig.NaturalTraceEnable ? 1 : 0, Organic() ? 1 : 0, SampleEvery(),
        forced >= 0 ? "command" : "config", sPlayerbotAIConfig.NaturalTraceIntervalMs,
        sPlayerbotAIConfig.NaturalTraceMaxUnits, TraceDirectory());
    handler->PSendSysMessage(
        "NATURAL units: live {}, started {}, ended {}, refused {}, truncated {}; rows: pos {}, events {}, mv {}, "
        "dropped {}",
        tracer.Count(), tracer.Started.load(), tracer.Ended.load(), tracer.Refused.load(), tracer.TruncatedUnits.load(),
        tracer.PosRows.load(), tracer.EventRows.load(), tracer.MoveRows.load(), tracer.DroppedRows.load());
    PvpTestRecordWriter& writer = PvpTestRecordWriter::Instance();
    handler->PSendSysMessage(
        "NATURAL writer: batches queued {}, rows written {}, bytes written {}, queue {}, failed {}",
        QueuedBatches.load(), WrittenRows.load(), WrittenBytes.load(), writer.QueueSize(), writer.Failed());
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
    Row startBot;
    startBot.Source = "harness";
    startBot.Side = 'A';
    startBot.Spell = 8;
    startBot.Flag = true;
    Row startPlayer;
    startPlayer.Source = "gm";
    startPlayer.Spell = 1;

    check(!tracer->Marked(botA) && !live.load(), "nothing traced: the hooks stop at the flag");
    check(tracer->Add(botA, matchId, "d/", "20261027", startBot, 2, 100, 100000) == AddResult::Added &&
              tracer->Add(playerB, matchId, "d/", "20261027", startPlayer, 2, 100, 100000) == AddResult::Added &&
              live.load() == 2 && tracer->Marked(botA) && tracer->Marked(playerB),
          "two units traced, marked, counted");
    check(tracer->Add(extra, matchId, "d/", "20261027", startBot, 2, 100, 100000) == AddResult::Full &&
              tracer->Add(botA, matchId, "d/", "20261027", startBot, 2, 100, 100000) == AddResult::Exists &&
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

        group.Add(201, "", "d/", "20261027", startPlayer, 4, 100, 100000);
        check(group.AddGroup(members({301, 302, 303, 304}), matchId, "d/", "20261027", 4, 100, 100000) ==
                      AddResult::Full &&
                  groupLive.load() == 1 && !group.Marked(301) && !group.Marked(304) && group.Refused.load() == 4,
              "a group that does not fit under MaxUnits is refused whole");
        check(group.AddGroup(members({301, 302, 303}), matchId, "d/", "20261027", 4, 100, 100000) == AddResult::Added &&
                  groupLive.load() == 4 && group.Marked(301) && group.Marked(303) && group.Started.load() == 4,
              "a group that fits is traced whole");
        check(group.AddGroup(members({401, 302}), matchId, "d/", "20261027", 10, 100, 100000) == AddResult::Exists &&
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
                if (race.AddGroup(members({first, first + 1, first + 2, first + 3}), matchId, "d/", "20261027", 6, 100,
                                  100000) == AddResult::Added)
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
    tracer->Add(extra, "", "d/", "20261027", startPlayer, 2, 100, 5);
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
