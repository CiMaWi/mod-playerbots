/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MatchTrace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "AllMapScript.h"
#include "AllSpellScript.h"
#include "Battleground.h"
#include "Chat.h"
#include "Log.h"
#include "Map.h"
#include "MatchLog.h"
#include "NaturalTrace.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PvpTestRecord.h"
#include "ScriptMgr.h"
#include "SkillStrategy.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Timer.h"
#include "UnitScript.h"

namespace
{
using Clock = std::chrono::steady_clock;

// The mechanics that make an aura a CC row: stuns, roots, fears, silences, sleeps, polymorphs and disorients, horrors,
// incapacitates (knockout, sap), charms, freezes, banishes, shackles, turns and disarms. Snares and dazes are left
// out (frequent, not a loss of control).
constexpr uint64 CcMechanicMask =
    (1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_DISORIENTED) | (1ULL << MECHANIC_DISARM) | (1ULL << MECHANIC_FEAR) |
    (1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_SILENCE) | (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_STUN) |
    (1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_KNOCKOUT) | (1ULL << MECHANIC_POLYMORPH) | (1ULL << MECHANIC_BANISH) |
    (1ULL << MECHANIC_SHACKLE) | (1ULL << MECHANIC_TURN) | (1ULL << MECHANIC_HORROR) | (1ULL << MECHANIC_SAPPED);

// A running trace older than this belongs to a match that never ended (as MatchLog's captures)
constexpr std::chrono::hours MaxAge{3};
constexpr size_t MaxLiveTraces = 64;
constexpr uint32 PositionEveryMs = 1000;
// Rows only deaths may use once the others have reached the caps
constexpr uint32 MaxDeathReserve = 16;
// Estimated bytes (upper bounds for guids and times of up to 7 digits): the line without rows (header, up to 10
// units, the end and truncated rows), and each row by kind
constexpr size_t BaseBytes = 1536;
constexpr size_t DeathBytes = 56;

size_t RowBytes(TraceKind kind)
{
    switch (kind)
    {
        case TraceKind::Pos:
            return 96;
        case TraceKind::CastStart:
        case TraceKind::CastFinish:
            return 80;
        case TraceKind::Cancel:
            return 72;
        case TraceKind::CcOn:
        case TraceKind::CcOff:
            return 96;
        case TraceKind::Action:
            return 80;
        case TraceKind::Death:
        default:
            return DeathBytes;
    }
}

char const* MechanicName(uint8 mechanic)
{
    switch (mechanic)
    {
        case MECHANIC_CHARM:
            return "charm";
        case MECHANIC_DISORIENTED:
            return "disoriented";
        case MECHANIC_DISARM:
            return "disarm";
        case MECHANIC_FEAR:
            return "fear";
        case MECHANIC_ROOT:
            return "root";
        case MECHANIC_SILENCE:
            return "silence";
        case MECHANIC_SLEEP:
            return "sleep";
        case MECHANIC_STUN:
            return "stun";
        case MECHANIC_FREEZE:
            return "freeze";
        case MECHANIC_KNOCKOUT:
            return "incapacitate";
        case MECHANIC_POLYMORPH:
            return "polymorph";
        case MECHANIC_BANISH:
            return "banish";
        case MECHANIC_SHACKLE:
            return "shackle";
        case MECHANIC_TURN:
            return "turn";
        case MECHANIC_HORROR:
            return "horror";
        case MECHANIC_SAPPED:
            return "sapped";
        default:
            return "other";
    }
}

char const* RemoveModeName(uint8 mode)
{
    switch (mode)
    {
        case AURA_REMOVE_BY_CANCEL:
            return "cancel";
        case AURA_REMOVE_BY_ENEMY_SPELL:
            return "dispel";
        case AURA_REMOVE_BY_EXPIRE:
            return "expire";
        case AURA_REMOVE_BY_DEATH:
            return "death";
        default:
            return "default";  // scripted removes, and CC broken by damage
    }
}

// The first CC mechanic of a spell, 0 when it has none
uint8 CcMechanicOf(SpellInfo const* spellInfo)
{
    uint64 const mask = spellInfo ? spellInfo->GetAllEffectsMechanicMask() & CcMechanicMask : 0;
    if (!mask)
        return 0;

    for (uint8 mechanic = 1; mechanic < 64; ++mechanic)
    {
        if (mask & (1ULL << mechanic))
            return mechanic;
    }

    return 0;
}

// A running trace in the registry
struct LiveTrace
{
    std::shared_ptr<MatchTrace> Trace;
    uint32 StartMs = 0;
    uint32 NextPositionMs = 0;
    // Set by End: from then on only deaths are taken (the last blow's), and the next map update queues the line
    bool Closed = false;
    Clock::time_point Created = Clock::now();
};

using Registry = std::unordered_map<uint32, LiveTrace>;

// Shared between the arena map threads (and the world thread for an arena the battleground update ends)
std::mutex TraceLock;
Registry Traces;
// Traces.size(), read by every hook without the lock: 0 = nothing to do
std::atomic<uint32> LiveCount{0};
// The lock-free filter per arena: Marks[id % MarkBuckets] counts the live traces whose instance ids fall into that
// bucket (written under the lock, read without it). An arena whose bucket is 0 has no trace: its hooks return before
// the PerfMonitor op and the lock. A bucket shared with a traced arena only sends the other arena's events on to the
// lookup, which finds nothing (at most 64 live traces in 4096 buckets).
constexpr uint32 MarkBuckets = 4096;
std::array<std::atomic<uint16>, MarkBuckets> Marks{};

std::atomic<uint64> Started{0};
std::atomic<uint64> Queued{0};
std::atomic<uint64> Written{0};
std::atomic<uint64> WrittenBytes{0};
std::atomic<uint64> MaxWrittenBytes{0};
std::atomic<uint64> WrittenEvents{0};
std::atomic<uint64> TruncatedTraces{0};
std::atomic<uint64> DroppedEvents{0};
std::atomic<uint64> DroppedTraces{0};
std::atomic<uint64> FormatMicroseconds{0};

uint64 PlayerCounter(Unit const* unit) { return unit && unit->IsPlayer() ? unit->GetGUID().GetCounter() : 0; }

// A player, or the player that owns a pet or totem
uint64 OwnerCounter(Unit const* unit)
{
    Player const* player = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    return player ? player->GetGUID().GetCounter() : 0;
}

// The instance id when its bucket has a live trace, else 0 (one relaxed load, no lock)
uint32 TracedKey(uint32 instanceId)
{
    return Marks[instanceId % MarkBuckets].load(std::memory_order_relaxed) ? instanceId : 0;
}

// The filter of the hooks, cheapest test first, no lock: the key of the traced arena a player is in, else 0. Pets and
// totems are not participants. The natural tracer (G0e) replaces this predicate with "a traced unit".
uint32 ArenaKeyOf(Unit const* unit)
{
    if (!LiveCount.load(std::memory_order_relaxed) || !unit || !unit->IsPlayer())
        return 0;

    Map const* map = unit->FindMap();
    return map && map->IsBattleArena() ? TracedKey(map->GetInstanceId()) : 0;
}

// Stamps the row with the trace's clock and appends it, when the arena has a trace
void PushRow(uint32 key, TraceRow row)
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "MatchTraceEvent");
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(TraceLock);
        auto const found = Traces.find(key);
        if (found != Traces.end() && (!found->second.Closed || row.Kind == TraceKind::Death))
        {
            row.Ms = getMSTimeDiff(found->second.StartMs, now);
            found->second.Trace->Push(row);
        }
    }

    if (pmo)
        pmo->finish();
}

// The registry lock is held: removes a trace and its mark. A closed trace goes to `closed` (queued once the lock is
// released), an unfinished one counts as dropped. Returns the next entry.
Registry::iterator Retire(Registry::iterator itr, std::vector<std::shared_ptr<MatchTrace>>& closed)
{
    if (itr->second.Closed)
        closed.push_back(std::move(itr->second.Trace));
    else
        ++DroppedTraces;

    Marks[itr->first % MarkBuckets].fetch_sub(1, std::memory_order_relaxed);
    itr = Traces.erase(itr);
    LiveCount.store(static_cast<uint32>(Traces.size()), std::memory_order_relaxed);
    return itr;
}

// The registry lock is held: the traces older than MaxAge (a match that never ended)
void SweepStale(Clock::time_point now, std::vector<std::shared_ptr<MatchTrace>>& closed)
{
    for (auto itr = Traces.begin(); itr != Traces.end();)
    {
        if (now - itr->second.Created > MaxAge)
            itr = Retire(itr, closed);
        else
            ++itr;
    }
}

// The registry lock is held: the trace of an instance, in place of the one a reused instance id left behind, after
// the stale traces and the oldest over the cap
void Add(uint32 key, LiveTrace live, Clock::time_point now, std::vector<std::shared_ptr<MatchTrace>>& closed)
{
    auto const found = Traces.find(key);
    if (found != Traces.end())
        Retire(found, closed);

    SweepStale(now, closed);
    while (Traces.size() >= MaxLiveTraces)
    {
        auto oldest = Traces.begin();
        for (auto itr = Traces.begin(); itr != Traces.end(); ++itr)
        {
            if (itr->second.Created < oldest->second.Created)
                oldest = itr;
        }

        Retire(oldest, closed);
    }

    Traces.emplace(key, std::move(live));
    Marks[key % MarkBuckets].fetch_add(1, std::memory_order_relaxed);
    LiveCount.store(static_cast<uint32>(Traces.size()), std::memory_order_relaxed);
}

// Queues a closed trace to the writer thread, which formats it (map thread: a pointer move)
void Queue(std::shared_ptr<MatchTrace> trace)
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "MatchTraceFlush");
    std::string date = trace->StartTime().substr(0, 10);
    date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
    ++Queued;
    if (trace->Truncated())
    {
        ++TruncatedTraces;
        DroppedEvents += trace->Dropped();
    }

    LOG_INFO("playerbots", "MATCHLOG: trace {}: {} events, about {} bytes, truncated {} ({} dropped)", trace->MatchId(),
             trace->Rows().size(), trace->EstimatedBytes(), trace->Truncated() ? "yes" : "no", trace->Dropped());
    PvpTestRecordWriter::Instance().WriteLine(
        "traces", std::move(date), std::string(),
        [trace]()
        {
            Clock::time_point const start = Clock::now();
            std::string text = trace->ToJson();
            text += '\n';
            ++Written;
            WrittenBytes += text.size();
            WrittenEvents += trace->Rows().size();
            if (text.size() > MaxWrittenBytes.load())
                MaxWrittenBytes.store(text.size());

            FormatMicroseconds += static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
            return text;
        });
    if (pmo)
        pmo->finish();
}

// Map thread, once per map update of a traced arena: queues a closed trace, and once a second the positions of the
// participants (or drops the trace when its match has run past MaxAge without an end)
void OnArenaMapUpdate(Map* map)
{
    if (!LiveCount.load(std::memory_order_relaxed) || !map->IsBattleArena() || !TracedKey(map->GetInstanceId()))
        return;

    uint32 const key = map->GetInstanceId();
    uint32 const now = getMSTime();
    uint32 elapsed = 0;
    std::vector<std::shared_ptr<MatchTrace>> closed;
    {
        std::lock_guard<std::mutex> guard(TraceLock);
        auto const found = Traces.find(key);
        if (found == Traces.end())
            return;

        if (found->second.Closed)
            Retire(found, closed);
        else
        {
            elapsed = getMSTimeDiff(found->second.StartMs, now);
            if (elapsed < found->second.NextPositionMs)
                return;

            if (Clock::now() - found->second.Created > MaxAge)
            {
                Retire(found, closed);
                return;
            }

            found->second.NextPositionMs = elapsed - elapsed % PositionEveryMs + PositionEveryMs;
        }
    }

    if (!closed.empty())
    {
        Queue(std::move(closed.front()));
        return;
    }

    BattlegroundMap* bgMap = map->ToBattlegroundMap();
    Battleground* bg = bgMap ? bgMap->GetBG() : nullptr;
    if (!bg)
        return;

    std::vector<TraceRow> rows;
    rows.reserve(bg->GetPlayers().size());
    for (auto const& [guid, player] : bg->GetPlayers())
    {
        if (!player || player->FindMap() != map)
            continue;

        TraceRow row;
        row.Ms = elapsed;
        row.Kind = TraceKind::Pos;
        row.Guid = guid.GetCounter();
        row.Extra = static_cast<uint8>(std::min(100L, std::lround(player->GetHealthPct())));
        row.X = player->GetPositionX();
        row.Y = player->GetPositionY();
        row.Z = player->GetPositionZ();
        row.O = player->GetOrientation();
        rows.push_back(row);
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "MatchTraceEvent");
    {
        std::lock_guard<std::mutex> guard(TraceLock);
        auto const found = Traces.find(key);
        if (found != Traces.end() && !found->second.Closed)
        {
            for (TraceRow const& row : rows)
                found->second.Trace->Push(row);
        }
    }

    if (pmo)
        pmo->finish();
}

// From the destructor of an arena map: its trace leaves the registry (a closed one still goes to the writer, an
// unfinished one counts as dropped), so a match that never ended does not keep LiveCount above 0
void OnArenaMapDestroy(Map* map)
{
    if (!LiveCount.load(std::memory_order_relaxed) || !map->IsBattleArena() || !TracedKey(map->GetInstanceId()))
        return;

    std::vector<std::shared_ptr<MatchTrace>> closed;
    {
        std::lock_guard<std::mutex> guard(TraceLock);
        auto const found = Traces.find(map->GetInstanceId());
        if (found != Traces.end())
            Retire(found, closed);
    }

    for (std::shared_ptr<MatchTrace>& trace : closed)
        Queue(std::move(trace));
}
}  // namespace

std::string TraceRowJson(TraceRow const& row)
{
    switch (row.Kind)
    {
        case TraceKind::Pos:
            return Acore::StringFormat(
                "{{\"t\":{},\"k\":\"pos\",\"u\":{},\"x\":{:.1f},\"y\":{:.1f},\"z\":{:.1f},\"o\":{:.2f},\"hp\":{}}}",
                row.Ms, row.Guid, row.X, row.Y, row.Z, row.O, static_cast<uint32>(row.Extra));
        case TraceKind::CastStart:
        case TraceKind::CastFinish:
            return Acore::StringFormat("{{\"t\":{},\"k\":\"{}\",\"u\":{},\"spell\":{},\"tgt\":{}}}", row.Ms,
                                       row.Kind == TraceKind::CastStart ? "cast_start" : "cast_finish", row.Guid,
                                       row.Spell, row.Other);
        case TraceKind::Cancel:
            return Acore::StringFormat("{{\"t\":{},\"k\":\"cancel\",\"u\":{},\"spell\":{},\"self\":{}}}", row.Ms,
                                       row.Guid, row.Spell, row.Extra ? "true" : "false");
        case TraceKind::CcOn:
            return Acore::StringFormat("{{\"t\":{},\"k\":\"cc_on\",\"u\":{},\"by\":{},\"spell\":{},\"mech\":\"{}\"}}",
                                       row.Ms, row.Guid, row.Other, row.Spell, MechanicName(row.Extra));
        case TraceKind::CcOff:
            return Acore::StringFormat(
                "{{\"t\":{},\"k\":\"cc_off\",\"u\":{},\"spell\":{},\"mech\":\"{}\",\"how\":\"{}\"}}", row.Ms, row.Guid,
                row.Spell, MechanicName(row.Extra), RemoveModeName(row.Extra2));
        case TraceKind::Action:
            return Acore::StringFormat("{{\"t\":{},\"k\":\"action\",\"u\":{},\"cat\":\"{}\",\"tgt\":{}}}", row.Ms,
                                       row.Guid, SkillNoticeCategoryName(static_cast<SkillNoticeCategory>(row.Extra)),
                                       row.Other);
        case TraceKind::Death:
        default:
            return Acore::StringFormat("{{\"t\":{},\"k\":\"death\",\"u\":{},\"by\":{}}}", row.Ms, row.Guid, row.Other);
    }
}

MatchTrace::MatchTrace(std::string matchId, uint32 every, uint32 maxEvents, uint32 maxBytes)
    : _matchId(std::move(matchId)),
      _every(every),
      _maxEvents(std::max<uint32>(maxEvents, 16)),
      _maxBytes(std::max<uint32>(maxBytes, 4096)),
      _bytes(BaseBytes)
{
    _reserve = std::min<uint32>(MaxDeathReserve, _maxEvents / 4);
    _time = PvpTestRecordWriter::IsoTimeNow();
}

bool MatchTrace::Push(TraceRow const& row)
{
    bool const death = row.Kind == TraceKind::Death;
    size_t const bytes = RowBytes(row.Kind);
    size_t const eventLimit = death ? _maxEvents : _maxEvents - _reserve;
    size_t const byteLimit = death ? _maxBytes : _maxBytes - _reserve * DeathBytes;
    char const* reason = nullptr;
    if (_truncated && !death)
        reason = _reason;
    else if (_rows.size() >= eventLimit)
        reason = "events";
    else if (_bytes + bytes > byteLimit)
        reason = "bytes";

    if (reason)
    {
        ++_dropped;
        if (!_truncated)
        {
            _truncated = true;
            _reason = reason;
        }

        return false;
    }

    _rows.push_back(row);
    _bytes += bytes;
    return true;
}

void MatchTrace::Finish(PvpTestMatchRecord const& record, uint32 endMs)
{
    _server = record.Server;
    if (!record.StartTime.empty())
        _time = record.StartTime;  // the record's time, so the trace goes into the file of the record's date

    _map = record.Map;
    _arenaType = record.ArenaType;
    _rated = record.Rated;
    _winnerSide = record.WinnerSide;
    _endMs = endMs;
    _units.clear();
    for (PvpTestSideRecord const& side : record.Sides)
    {
        for (PvpTestBotRecord const& bot : side.Bots)
            _units.push_back(TraceUnit{bot.Guid, bot.Name, side.Side, bot.ClassId});
    }
}

std::string MatchTrace::ToJson() const
{
    std::string json;
    json.reserve(_bytes + 256);
    json += Acore::StringFormat(
        "{{\"schema\":1,\"kind\":\"trace\",\"matchId\":{},\"server\":{},\"time\":{},\"map\":{},\"arenaType\":{},"
        "\"rated\":{},\"everyN\":{},\"durationMs\":{},\"winnerSide\":{},\"maxEvents\":{},\"maxBytes\":{},"
        "\"eventCount\":{},\"truncated\":{},\"droppedEvents\":{},\"units\":[",
        PvpTestRecordWriter::JsonQuote(_matchId), PvpTestRecordWriter::JsonQuote(_server),
        PvpTestRecordWriter::JsonQuote(_time), PvpTestRecordWriter::JsonQuote(_map), static_cast<uint32>(_arenaType),
        _rated ? "true" : "false", _every, _endMs, PvpTestRecordWriter::JsonQuote(_winnerSide), _maxEvents, _maxBytes,
        _rows.size(), _truncated ? "true" : "false", _dropped);
    for (size_t index = 0; index < _units.size(); ++index)
    {
        TraceUnit const& unit = _units[index];
        json += Acore::StringFormat("{}{{\"guid\":{},\"name\":{},\"side\":\"{}\",\"class\":{}}}", index ? "," : "",
                                    unit.Guid, PvpTestRecordWriter::JsonQuote(unit.Name), unit.Side,
                                    PvpTestRecordWriter::JsonQuote(PvpTestRecordWriter::ClassName(unit.ClassId)));
    }

    json += "],\"events\":[";
    for (TraceRow const& row : _rows)
    {
        json += TraceRowJson(row);
        json += ',';
    }

    json += Acore::StringFormat("{{\"t\":{},\"k\":\"end\",\"winnerSide\":{}}}", _endMs,
                                PvpTestRecordWriter::JsonQuote(_winnerSide));
    if (_truncated)
        json += Acore::StringFormat(",{{\"t\":{},\"k\":\"truncated\",\"reason\":\"{}\",\"dropped\":{}}}", _endMs,
                                    _reason, _dropped);

    json += "]}";
    return json;
}

uint32 MatchTrace::Hash(std::string const& text)
{
    uint32 hash = 2166136261u;
    for (char const c : text)
    {
        hash ^= static_cast<uint8>(c);
        hash *= 16777619u;
    }

    return hash;
}

bool MatchTrace::Selected(std::string const& matchId, uint32 every) { return every && Hash(matchId) % every == 0; }

void MatchTrace::Begin(uint32 instanceId, std::string const& matchId)
{
    uint32 const every = sPlayerbotAIConfig.MatchLogTraceEvery;
    if (!every || !Selected(matchId, every))
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "MatchTraceStart");
    LiveTrace live;
    live.Trace = std::make_shared<MatchTrace>(matchId, every, sPlayerbotAIConfig.MatchLogTraceMaxEvents,
                                              sPlayerbotAIConfig.MatchLogTraceMaxBytes);
    live.StartMs = getMSTime();
    std::vector<std::shared_ptr<MatchTrace>> closed;
    {
        std::lock_guard<std::mutex> guard(TraceLock);
        Add(instanceId, std::move(live), Clock::now(), closed);
    }

    for (std::shared_ptr<MatchTrace>& trace : closed)
        Queue(std::move(trace));

    ++Started;
    if (pmo)
        pmo->finish();
}

void MatchTrace::End(uint32 instanceId, PvpTestMatchRecord const& record)
{
    if (!LiveCount.load(std::memory_order_relaxed) || !TracedKey(instanceId))
        return;

    std::lock_guard<std::mutex> guard(TraceLock);
    auto const found = Traces.find(instanceId);
    if (found == Traces.end() || found->second.Closed || found->second.Trace->MatchId() != record.MatchId)
        return;

    found->second.Closed = true;
    found->second.Trace->Finish(record, getMSTimeDiff(found->second.StartMs, getMSTime()));
}

void MatchTrace::OnAction(Player* bot, uint8 category, ObjectGuid target)
{
    uint32 const key = ArenaKeyOf(bot);
    if (!key)
        return;

    TraceRow row;
    row.Kind = TraceKind::Action;
    row.Guid = bot->GetGUID().GetCounter();
    row.Extra = category;
    ObjectGuid const other = target ? target : bot->GetTarget();
    row.Other = other.IsPlayer() ? other.GetCounter() : 0;
    PushRow(key, row);
}

std::string MatchTrace::StatusText()
{
    size_t live = 0;
    {
        std::lock_guard<std::mutex> guard(TraceLock);
        live = Traces.size();
    }

    uint64 const written = Written.load();
    return Acore::StringFormat(
        "MATCHLOG: traces (TraceEvery {}, caps {} events / {} bytes): started {}, running {}, queued {}, written {} "
        "(mean {} bytes, max {}, mean {} events, mean format {} us), truncated {} ({} events dropped), dropped "
        "unfinished {}",
        sPlayerbotAIConfig.MatchLogTraceEvery, sPlayerbotAIConfig.MatchLogTraceMaxEvents,
        sPlayerbotAIConfig.MatchLogTraceMaxBytes, Started.load(), live, Queued.load(), written,
        written ? WrittenBytes.load() / written : 0, MaxWrittenBytes.load(),
        written ? WrittenEvents.load() / written : 0, written ? FormatMicroseconds.load() / written : 0,
        TruncatedTraces.load(), DroppedEvents.load(), DroppedTraces.load());
}

bool MatchTrace::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    uint32 failures = 0;
    uint32 checks = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("MATCHLOG trace selftest FAIL: {}", what);
    };

    // The hash: the FNV-1a test vectors, so the sampling is the same in every build
    check(Hash("") == 0x811c9dc5u && Hash("a") == 0xe40c292cu && Hash("foobar") == 0xbf9cf968u, "FNV-1a vectors");
    std::string const fixed = "main-20261026000000-1";
    check(Selected(fixed, 1) && !Selected(fixed, 0), "every 1 traces all, 0 none");
    check(Selected(fixed, 3) == (Hash(fixed) % 3 == 0), "every 3 by the hash");
    uint32 selected = 0;
    for (uint32 n = 1; n <= 300; ++n)
        selected += Selected(Acore::StringFormat("main-20261026000000-{}", n), 3) ? 1 : 0;

    check(selected >= 60 && selected <= 140, Acore::StringFormat("every 3 picks about a third ({} of 300)", selected));

    // A small match: rows in order, every kind, the end and no marker
    PvpTestMatchRecord record;
    record.MatchId = "selftest-1";
    record.Server = "main";
    record.Map = "Nagrand Arena";
    record.Rated = true;
    record.WinnerSide = "A";
    record.Sides[0].Side = 'A';
    record.Sides[1].Side = 'B';
    PvpTestBotRecord bot;
    bot.Guid = 11;
    bot.Name = "Alpha";
    bot.ClassId = CLASS_MAGE;
    record.Sides[0].Bots.push_back(bot);
    bot.Guid = 22;
    bot.Name = "Bravo \"B\"";
    bot.ClassId = CLASS_ROGUE;
    record.Sides[1].Bots.push_back(bot);

    MatchTrace trace(record.MatchId, 1, 20000, 4194304);
    TraceKind const kinds[] = {TraceKind::Pos,  TraceKind::CastStart, TraceKind::CastFinish, TraceKind::Cancel,
                               TraceKind::CcOn, TraceKind::CcOff,     TraceKind::Action,     TraceKind::Death};
    uint32 ms = 0;
    for (TraceKind const kind : kinds)
    {
        TraceRow row;
        row.Ms = ms += 100;
        row.Kind = kind;
        row.Guid = kind == TraceKind::Death ? 22 : 11;
        row.Other = 22;
        row.Spell = 118;
        row.Extra = kind == TraceKind::CcOn || kind == TraceKind::CcOff ? MECHANIC_POLYMORPH : 1;
        row.Extra2 = AURA_REMOVE_BY_EXPIRE;
        row.X = -1234.56f;
        check(trace.Push(row), "a row under the caps is kept");
    }

    trace.Finish(record, 900);
    std::string const line = trace.ToJson();
    check(MatchLog::JsonWellFormed(line), "the line is well-formed JSON");
    check(line.find("\"k\":\"cc_on\",\"u\":11,\"by\":22,\"spell\":118,\"mech\":\"polymorph\"") != std::string::npos &&
              line.find("\"how\":\"expire\"") != std::string::npos &&
              line.find("\"cat\":\"interrupt\"") != std::string::npos &&
              line.find("\"x\":-1234.6") != std::string::npos,
          "the rows carry their fields");
    check(line.find("{\"t\":900,\"k\":\"end\",\"winnerSide\":\"A\"}]}") != std::string::npos &&
              line.find("\"truncated\":false") != std::string::npos,
          "the end row closes an uncut trace");
    check(line.find("\"name\":\"Bravo \\\"B\\\"\",\"side\":\"B\",\"class\":\"rogue\"") != std::string::npos,
          "the units carry name, side and class");
    bool ordered = true;
    for (size_t index = 1; index < trace.Rows().size(); ++index)
        ordered = ordered && trace.Rows()[index - 1].Ms <= trace.Rows()[index].Ms;

    check(ordered && trace.Rows().size() == 8, "the rows are kept in order");
    check(line.size() <= trace.EstimatedBytes(), "the estimate is an upper bound");

    // The event cap: a tiny cap cuts the positions, the deaths after the cut are kept, the line ends with the marker
    MatchTrace cut("selftest-2", 1, 50, 4194304);
    for (uint32 n = 0; n < 100; ++n)
    {
        TraceRow row;
        row.Ms = n * 10;
        row.Guid = 11;
        cut.Push(row);
    }

    for (uint32 n = 0; n < 3; ++n)
    {
        TraceRow row;
        row.Ms = 1000 + n;
        row.Kind = TraceKind::Death;
        row.Guid = 22;
        check(cut.Push(row), "a death after the cut is kept");
    }

    cut.Finish(record, 1100);
    std::string const cutLine = cut.ToJson();
    check(cut.Truncated() && cut.Dropped() == 100 - (50 - 12) && cut.Rows().size() == 50 - 12 + 3,
          Acore::StringFormat("the event cap keeps {} rows and drops {}", cut.Rows().size(), cut.Dropped()));
    check(MatchLog::JsonWellFormed(cutLine) &&
              cutLine.find(",{\"t\":1100,\"k\":\"truncated\",\"reason\":\"events\",\"dropped\":62}]}") !=
                  std::string::npos &&
              cutLine.find("\"truncated\":true,\"droppedEvents\":62") != std::string::npos,
          "a cut trace ends with the marker");

    // The byte cap: the line stays within it, the reason is bytes, deaths still fit
    MatchTrace byteCapped("selftest-3", 1, 20000, 4096);
    for (uint32 n = 0; n < 200; ++n)
    {
        TraceRow row;
        row.Ms = n * 1000;
        row.Guid = 1234567;
        row.X = -12345.6f;
        row.Y = -12345.6f;
        row.Z = -1234.5f;
        row.O = 6.28f;
        row.Extra = 100;
        byteCapped.Push(row);
    }

    TraceRow death;
    death.Ms = 200000;
    death.Kind = TraceKind::Death;
    death.Guid = 1234567;
    death.Other = 7654321;
    check(byteCapped.Push(death), "a death fits the byte reserve");
    byteCapped.Finish(record, 200001);
    std::string const smallLine = byteCapped.ToJson();
    check(byteCapped.Truncated() && smallLine.find("\"reason\":\"bytes\"") != std::string::npos &&
              smallLine.size() <= 4096 && MatchLog::JsonWellFormed(smallLine),
          Acore::StringFormat("the byte cap holds ({} bytes)", smallLine.size()));

    // The registry, with instance ids far above the core's (which counts up from 1). The filter checks run while this
    // thread holds the registry lock: a lock inside the filter would hang here instead of passing. The closed trace
    // of the last check is not queued, and the drops of this test are taken back from the status counter.
    auto liveTrace = [](char const* matchId, Clock::time_point created)
    {
        LiveTrace live;
        live.Trace = std::make_shared<MatchTrace>(matchId, 1, 100, 4096);
        live.Created = created;
        return live;
    };
    {
        std::vector<std::shared_ptr<MatchTrace>> closed;
        std::lock_guard<std::mutex> guard(TraceLock);
        uint32 const keyA = 0xFFF00000u;
        uint32 const keyB = keyA + MarkBuckets;  // keyA's bucket
        uint32 keyC = keyA + 1;                  // an untraced arena: the next bucket without a live trace
        while (Marks[keyC % MarkBuckets].load() && keyC < keyA + MarkBuckets - 1)
            ++keyC;

        uint16 const markBefore = Marks[keyA % MarkBuckets].load();
        uint64 const droppedBefore = DroppedTraces.load();
        size_t const liveBefore = Traces.size();
        Clock::time_point const now = Clock::now();
        Add(keyB, liveTrace("selftest-old", now), now, closed);
        Add(keyA, liveTrace("selftest-stale", now - MaxAge - std::chrono::minutes(1)), now, closed);
        bool const staleKept = Traces.count(keyA) == 1;  // Add sweeps before it inserts
        check(TracedKey(keyA) == keyA && TracedKey(keyB) == keyB && !TracedKey(keyC),
              "the marks tell traced arenas from an untraced one without the lock");
        uint32 const before = LiveCount.load();
        SweepStale(now, closed);
        check(staleKept && !Traces.count(keyA) && Traces.count(keyB) == 1 && LiveCount.load() == before - 1 &&
                  DroppedTraces.load() == droppedBefore + 1 && closed.empty(),
              "a stale trace is swept, counted as dropped and leaves LiveCount");
        check(TracedKey(keyA) == keyA && Marks[keyA % MarkBuckets].load() == markBefore + 1,
              "a bucket another live trace shares stays marked");
        Add(keyB, liveTrace("selftest-new", now), now, closed);
        auto const found = Traces.find(keyB);
        check(found != Traces.end() && found->second.Trace->MatchId() == "selftest-new" &&
                  DroppedTraces.load() == droppedBefore + 2 && Marks[keyB % MarkBuckets].load() == markBefore + 1,
              "a reused instance id counts the unfinished trace it replaces as dropped");
        if (found != Traces.end())
        {
            found->second.Closed = true;
            Retire(found, closed);
        }

        check(closed.size() == 1 && DroppedTraces.load() == droppedBefore + 2 &&
                  Marks[keyA % MarkBuckets].load() == markBefore && Traces.size() == liveBefore &&
                  LiveCount.load() == liveBefore,
              "a closed trace leaves for the writer, not as dropped, and clears its mark");
        DroppedTraces -= DroppedTraces.load() - droppedBefore;
    }

    handler->PSendSysMessage("MATCHLOG trace selftest: {} checks, {} failed", checks, failures);
    handler->PSendSysMessage("MATCHLOG sample trace: {}", cutLine);
    return true;
}

// The hooks: each returns after one test while no trace runs (LiveCount 0, always with TraceEvery 0), and in an
// untraced arena after its mark, before the PerfMonitor op and the lock. The spell hooks also feed the natural tracer
// (G0e), after its own one test.
class MatchTraceSpellScript : public AllSpellScript
{
public:
    MatchTraceSpellScript()
        : AllSpellScript("MatchTraceSpellScript",
                         {ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL})
    {
    }

    // A cast with a cast time begins (an instant one is cast before this hook: its finish row is enough)
    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (NaturalTrace::Any())
            NaturalTrace::OnSpellPrepare(spell, caster, spellInfo);

        uint32 const key = ArenaKeyOf(caster);
        if (!key || spell->IsTriggered() || spell->GetCastTime() <= 0)
            return;

        PushCast(key, TraceKind::CastStart, spell, caster, spellInfo);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (NaturalTrace::Any())
            NaturalTrace::OnSpellCast(spell, caster, spellInfo);

        uint32 const key = ArenaKeyOf(caster);
        if (!key || spell->IsTriggered() || spell->IsAutoRepeat())
            return;

        PushCast(key, TraceKind::CastFinish, spell, caster, spellInfo);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool bySelf) override
    {
        if (NaturalTrace::Any())
            NaturalTrace::OnSpellCastCancel(spell, caster, spellInfo, bySelf);

        uint32 const key = ArenaKeyOf(caster);
        if (!key || spell->IsTriggered())
            return;

        TraceRow row;
        row.Kind = TraceKind::Cancel;
        row.Guid = caster->GetGUID().GetCounter();
        row.Spell = spellInfo ? spellInfo->Id : 0;
        row.Extra = bySelf ? 1 : 0;
        PushRow(key, row);
    }

private:
    static void PushCast(uint32 key, TraceKind kind, Spell* spell, Unit* caster, SpellInfo const* spellInfo)
    {
        TraceRow row;
        row.Kind = kind;
        row.Guid = caster->GetGUID().GetCounter();
        row.Spell = spellInfo ? spellInfo->Id : 0;
        row.Other = PlayerCounter(spell->m_targets.GetUnitTarget());
        PushRow(key, row);
    }
};

class MatchTraceUnitScript : public UnitScript
{
public:
    MatchTraceUnitScript()
        : UnitScript("MatchTraceUnitScript", true,
                     {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE, UNITHOOK_ON_UNIT_DEATH})
    {
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        uint32 const key = ArenaKeyOf(unit);
        if (!key || !aura)
            return;

        uint8 const mechanic = CcMechanicOf(aura->GetSpellInfo());
        if (!mechanic)
            return;

        TraceRow row;
        row.Kind = TraceKind::CcOn;
        row.Guid = unit->GetGUID().GetCounter();
        row.Other = OwnerCounter(aura->GetCaster());
        row.Spell = aura->GetId();
        row.Extra = mechanic;
        PushRow(key, row);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        uint32 const key = ArenaKeyOf(unit);
        if (!key || !aurApp || !aurApp->GetBase())
            return;

        uint8 const mechanic = CcMechanicOf(aurApp->GetBase()->GetSpellInfo());
        if (!mechanic)
            return;

        TraceRow row;
        row.Kind = TraceKind::CcOff;
        row.Guid = unit->GetGUID().GetCounter();
        row.Spell = aurApp->GetBase()->GetId();
        row.Extra = mechanic;
        row.Extra2 = static_cast<uint8>(mode);
        PushRow(key, row);
    }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        uint32 const key = ArenaKeyOf(unit);
        if (!key)
            return;

        TraceRow row;
        row.Kind = TraceKind::Death;
        row.Guid = unit->GetGUID().GetCounter();
        row.Other = OwnerCounter(killer);
        PushRow(key, row);
    }
};

class MatchTraceMapScript : public AllMapScript
{
public:
    MatchTraceMapScript() : AllMapScript("MatchTraceMapScript", {ALLMAPHOOK_ON_MAP_UPDATE, ALLMAPHOOK_ON_DESTROY_MAP})
    {
    }

    void OnMapUpdate(Map* map, uint32 /*diff*/) override { OnArenaMapUpdate(map); }
    void OnDestroyMap(Map* map) override { OnArenaMapDestroy(map); }
};

void AddSC_match_trace()
{
    new MatchTraceSpellScript();
    new MatchTraceUnitScript();
    new MatchTraceMapScript();
}
