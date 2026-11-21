/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldLogWriter.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <utility>
#include <variant>

#include "Action.h"
#include "BotRecordMgr.h"
#include "BotTravel.h"
#include "Creature.h"
#include "EventWanted.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestStatEvents.h"
#include "ScriptMgr.h"
#include "StarterMgr.h"
#include "StringFormat.h"
#include "TravelLegRules.h"
#include "UpkeepRules.h"
#include "WorldLog.h"
#include "WorldLogBotState.h"

namespace
{
constexpr uint32 MS_PER_SECOND = 1000;
constexpr uint32 MS_PER_HOUR = 3600 * MS_PER_SECOND;

using WorldLogFormat::Stream;

WorldLogFormat::Place PlaceOf(Player* player)
{
    WorldLogFormat::Place place;
    place.map = player->GetMapId();
    place.zone = player->GetZoneId();
    place.area = player->GetAreaId();
    place.x = player->GetPositionX();
    place.y = player->GetPositionY();
    return place;
}

// One world-log event row of the player with guid counter `bot` at `place` (any thread)
void EnqueueEventRow(uint32 bot, WorldLogFormat::Place const& place, std::string_view type, std::string_view source,
                     uint32 id, int64 amount)
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog event");

    WorldLogFormat::EventRow row;
    row.t = WorldLog::NowMs();
    row.bot = bot;
    row.type = type;
    row.source = source;
    row.id = id;
    row.amount = amount;
    row.place = place;
    WorldLogWriter::Instance().EnqueueWorldLog(Stream::Event, WorldLogFormat::FormatEventRow(row), 1, row.t);

    if (pmo)
        pmo->finish();
}

// One world-log event row of any player, from its own state, on the thread that updates its map (or the world thread)
void EmitWorldLogEvent(Player* player, EventLog::EventType type, std::string_view word, std::string_view source,
                       uint32 id, int64 amount)
{
    EnqueueEventRow(player->GetGUID().GetCounter(), PlaceOf(player), word, source, id, amount);

    // A bot's event also goes into its recorder ring (WL8; its own PerfMonitor row)
    WorldLog::NoteEvent(player, type, word, source, id, amount);
}

// The one funnel of an event: its row (word: the type's word, or the whole word of an honesty or unknown row) and
// the readers its type has (EventTypes.h)
void Dispatch(Player* bot, EventLog::EventType type, std::string_view word, std::string_view source, uint32 id,
              int64 amount)
{
    if (!bot || word.empty() || !EventLog::Wanted(type))
        return;

    if (EventLog::ReadBy(type, EventLog::READ_ROW) && WorldLog::Active())
        EmitWorldLogEvent(bot, type, word, source, id, amount);

    // The bots' record takes its types whether or not the world log runs (its Record row follows the cause's row)
    if (EventLog::ReadBy(type, EventLog::READ_RECORD))
        BotRecordMgr::Instance().OnEvent(bot, type, source, id, amount);
    // LC28a: a starter where a real player perceives the bot (one atomic load while none is online)
    if (EventLog::ReadBy(type, EventLog::READ_STARTER))
        StarterMgr::Instance().OnEvent(bot, type, source, id, amount);
}

// The quest, vendor or flight master the RPG status works on, else 0
uint32 RpgTarget(NewRpgInfo const& info)
{
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
        return quest->questId;

    if (auto const* errands = std::get_if<NewRpgInfo::Errands>(&info.data))
        return errands->vendorEntry;

    if (auto const* flight = std::get_if<NewRpgInfo::TravelFlight>(&info.data))
        return flight->flightMasterEntry;

    return 0;
}

uint32 StateFlags(Player* player, bool human)
{
    uint32 flags = 0;
    if (player->IsInCombat())
        flags |= WorldLogFormat::STATE_FLAG_COMBAT;
    if (!player->IsAlive())
        flags |= WorldLogFormat::STATE_FLAG_DEAD;
    if (player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        flags |= WorldLogFormat::STATE_FLAG_GHOST;
    if (player->IsMounted())
        flags |= WorldLogFormat::STATE_FLAG_MOUNTED;
    if (player->IsInFlight())
        flags |= WorldLogFormat::STATE_FLAG_TAXI;
    if (player->HasPlayerFlag(PLAYER_FLAGS_RESTING))
        flags |= WorldLogFormat::STATE_FLAG_RESTING;
    if (player->GetGroup())
        flags |= WorldLogFormat::STATE_FLAG_GROUPED;
    if (player->GetMap() && player->GetMap()->Instanceable())
        flags |= WorldLogFormat::STATE_FLAG_INSTANCE;
    if (human)
        flags |= WorldLogFormat::STATE_FLAG_HUMAN;

    return flags;
}

// ilvl (integer mean over the equipped items, shirt and tabard left out) and dura_min (lowest durability % of the
// equipped items that have durability; 100 if none): one pass over the 19 equipment slots
void ReadEquipment(Player* player, WorldLogFormat::StateRow& row)
{
    uint32 levels = 0;
    uint32 items = 0;
    uint32 durabilityMin = WorldLogFormat::PERCENT;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        if (uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
            durabilityMin = std::min(
                durabilityMin, WorldLogFormat::Percent(item->GetUInt32Value(ITEM_FIELD_DURABILITY), maxDurability));

        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;

        if (ItemTemplate const* proto = item->GetTemplate())
        {
            levels += proto->ItemLevel;
            ++items;
        }
    }

    row.itemLevel = items ? levels / items : 0;
    row.durabilityMinPct = durabilityMin;
}

// One state row; world thread while no map update runs
WorldLogFormat::StateRow StateRowOf(Player* player, uint64 t)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    Powers const powerType = player->getPowerType();

    WorldLogFormat::StateRow row;
    row.t = t;
    row.bot = player->GetGUID().GetCounter();
    row.place = PlaceOf(player);
    row.level = player->GetLevel();
    row.xp = player->GetUInt32Value(PLAYER_XP);
    row.money = player->GetMoney();
    row.hpPct = WorldLogFormat::Percent(player->GetHealth(), player->GetMaxHealth());
    row.powerPct = WorldLogFormat::Percent(player->GetPower(powerType), player->GetMaxPower(powerType));
    if (ai)
    {
        row.rpg = static_cast<int32>(ai->rpgInfo.GetStatus());
        row.target = RpgTarget(ai->rpgInfo);
    }

    row.flags = StateFlags(player, !ai);
    row.freeSlots = player->GetFreeInventorySpace();
    ReadEquipment(player, row);
    if (Group const* group = player->GetGroup())
        row.group = group->GetLeaderGUID().GetCounter();

    return row;
}

// The world log's open segment and file per stream; writer thread only. A segment whose file fails to open or to
// write stays its stream's open segment (its rows are lost and counted) until the stream rolls on: a closed segment's
// name is never reopened.
class SegmentFiles
{
public:
    explicit SegmentFiles(std::string const& dir) : _dir(dir)
    {
        std::error_code error;
        std::filesystem::create_directories(_dir, error);
    }

    // Appends rows (all with times near tMs) to their stream's segment; false when they are lost. Empty rows only
    // open the segment (with its header row).
    bool Write(Stream stream, uint64 tMs, std::string_view rows)
    {
        Segment& segment = _segments[WorldLogFormat::StreamIndex(stream)];
        if (segment.tracker.Place(tMs, WorldLog::CurrentSettings().segmentMinutes) !=
            WorldLogFormat::SegmentAction::Append)
            Open(stream, segment);

        if (segment.file && !rows.empty())
            segment.file.write(rows.data(), static_cast<std::streamsize>(rows.size()));

        if (segment.file)
            return true;

        ++_errors;
        if (!_errorLogged)
        {
            _errorLogged = true;
            LOG_ERROR("playerbots", "[WorldLog] cannot write {}; its rows are lost and counted as write errors",
                      (_dir / segment.name).string());
        }

        return false;
    }

    void Flush()
    {
        for (Segment& segment : _segments)
            if (segment.file.is_open())
                segment.file.flush();
    }

    void CloseAll()
    {
        for (Segment& segment : _segments)
        {
            segment.tracker.Close();
            if (segment.file.is_open())
                segment.file.close();

            segment.name.clear();
        }

        _namesChanged = true;
    }

    uint64 Errors() const { return _errors; }

    // Copies the open segments' names when they changed since the last call
    void PublishNames(std::array<std::string, WorldLogFormat::STREAM_COUNT>& names)
    {
        if (!_namesChanged)
            return;

        for (size_t index = 0; index < WorldLogFormat::STREAM_COUNT; ++index)
            names[index] = _segments[index].name;

        _namesChanged = false;
    }

private:
    struct Segment
    {
        WorldLogFormat::SegmentTracker tracker;
        std::ofstream file;
        std::string name;
    };

    // Appends to the segment's file (a new or empty file gets the header row first); never truncates
    void Open(Stream stream, Segment& segment)
    {
        if (segment.file.is_open())
            segment.file.close();

        segment.file.clear();
        segment.name = segment.tracker.Name(stream);
        _namesChanged = true;

        std::filesystem::path const path = _dir / segment.name;
        std::error_code error;
        std::uintmax_t const size = std::filesystem::file_size(path, error);
        segment.file.open(path, std::ios::out | std::ios::app | std::ios::binary);
        if (segment.file && (error || !size))
            segment.file << WorldLogFormat::StreamHeader(stream) << WorldLogFormat::ROW_END;
    }

    std::filesystem::path _dir;
    std::array<Segment, WorldLogFormat::STREAM_COUNT> _segments;
    uint64 _errors = 0;
    bool _errorLogged = false;
    bool _namesChanged = false;
};

}  // namespace

// A travelling bot's death counts on its trip and leg at the death itself (WT10e-2c round 3): counted at the engine
// change, a bot revived and killed again before its next AI tick never left the dead engine, and that death was lost
void NoteTravelDeath(Player* killed)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(killed);
    if (!botAI)
        return;
    auto* travel = botAI->GetTravel().Trip();
    if (!travel)
        return;
    TravelLegs::NoteLife(travel->trip, TravelLegs::Life::Dead, getMSTime());
    // The trip's death count after this death (WT64c; the night check trip_deaths_by_count)
    EventLog::Emit(killed, EventLog::EventType::Travel, "death_count", travel->tripId, travel->trip.deaths);
    // A death on a fly leg after its takeoff (upkeep WU10): FlightFall death, amount the leg's number. A bot killed on
    // the ground before it took off (WU11b fix 7: Glettla, killed at Valgarde by the mob that had killed it there
    // before) died a ground death, counted by its Death row only
    TravelLegs::Trip const& trip = travel->trip;
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly && trip.seenInFlight)
        EventLog::Emit(killed, EventLog::EventType::FlightFall, "death", travel->tripId, trip.legIdx + 1);
}

WorldLogWriter::~WorldLogWriter() { Shutdown(); }

void WorldLogWriter::Update(uint32 diff)
{
    if (!WorldLog::Active())
        return;

    if (!_started)
        StartWriter();

    UpdateWorldLog(diff);
}

// A state pass every StateSeconds and a focus pass every FOCUS_SECONDS between two, the drops rows every hour of
// uptime. A running pass writes its next slice; a pass that falls due meanwhile starts when it has ended.
void WorldLogWriter::UpdateWorldLog(uint32 diff)
{
    _stateElapsed += diff;
    _focusElapsed += diff;
    if (_passNext < _passGuids.size())
        ContinueStatePass();
    else if (_stateElapsed >= WorldLog::CurrentSettings().stateSeconds * MS_PER_SECOND)
    {
        _stateElapsed = 0;
        _focusElapsed = 0;
        StartStatePass(false);
    }
    else if (_focusElapsed >= WorldLogFormat::FOCUS_SECONDS * MS_PER_SECOND)
    {
        _focusElapsed = 0;
        StartStatePass(true);
    }

    _dropsElapsed += diff;
    if (_dropsElapsed < MS_PER_HOUR)
        return;

    _dropsElapsed -= MS_PER_HOUR;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stop)
            return;

        QueueDropsRowsLocked(WorldLog::NowMs());
    }
}

// One state row per online player (real players with the human flag). The pass takes the players' guids in one
// tick and writes them STATE_ROWS_PER_TICK per tick, one chunk per tick with that tick's time: a full queue drops that
// chunk and counts its rows, a player who left the world since the pass began gets no row. A focus pass keeps the
// real players and the bots within FOCUS_YARDS of one at its start; with no real player online it ends after one
// read of the player list.
void WorldLogWriter::StartStatePass(bool focusOnly)
{
    _passGuids.clear();
    _passNext = 0;
    _passFocus = focusOnly;
    {
        // The players are only read here, on the world thread, while no map update runs
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        auto const& players = ObjectAccessor::GetPlayers();
        _focusPoints.clear();
        if (focusOnly)
        {
            for (auto const& pair : players)
            {
                Player* player = pair.second;
                if (player && player->IsInWorld() && !GET_PLAYERBOT_AI(player))
                    _focusPoints.push_back(WorldLog::FocusPointOf(player));
            }

            if (_focusPoints.empty())
                return;
        }

        for (auto const& pair : players)
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld())
                continue;

            if (focusOnly &&
                !WorldLogFormat::InFocus(_focusPoints, WorldLog::FocusPointOf(player), WorldLogFormat::FOCUS_YARDS))
                continue;

            _passGuids.push_back(pair.first);
        }
    }

    ContinueStatePass();
}

void WorldLogWriter::ContinueStatePass()
{
    if (_passNext >= _passGuids.size())
        return;

    PerfMonitorOperation* pmo =
        sPerfMonitor.start(PERF_MON_TOTAL, _passFocus ? "WorldLog state focus" : "WorldLog state");

    uint64 const t = WorldLog::NowMs();
    size_t const end = std::min(_passGuids.size(), _passNext + WorldLogFormat::STATE_ROWS_PER_TICK);
    std::string text;
    text.reserve((end - _passNext) * WorldLogFormat::STATE_ROW_RESERVE);
    uint32 rows = 0;
    {
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        auto const& players = ObjectAccessor::GetPlayers();
        for (; _passNext < end; ++_passNext)
        {
            auto const found = players.find(_passGuids[_passNext]);
            if (found == players.end() || !found->second || !found->second->IsInWorld())
                continue;

            WorldLogFormat::AppendStateRow(text, StateRowOf(found->second, t));
            ++rows;
        }
    }

    if (_passNext >= _passGuids.size())
    {
        _passGuids.clear();
        _passNext = 0;
    }

    if (rows)
        EnqueueWorldLog(Stream::State, std::move(text), rows, t);

    if (pmo)
        pmo->finish();
}

void WorldLogWriter::OnLevelChanged(Player* player, uint8 oldLevel)
{
    if (player && EventLog::Wanted(EventLog::EventType::Level))
        OnEvent(player, EventLog::EventType::Level, EventLog::SOURCE_OTHER, player->GetLevel(), oldLevel);
}

void WorldLogWriter::OnEvent(Player* bot, EventLog::EventType type, std::string_view source, uint32 id, int64 amount)
{
    Dispatch(bot, type, EventLog::Word(type), source, id, amount);
}

bool WorldLogWriter::EnqueueWorldLog(Stream stream, std::string rows, uint32 rowCount, uint64 tMs)
{
    if (rows.empty() || !rowCount)
        return false;

    uint64 const cap = WorldLog::CurrentSettings().queueRows;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stop || !_account.Admit(stream, rowCount, cap))
            return false;

        _queue.push_back(Chunk{std::move(rows), stream, rowCount, tMs});
    }
    return true;
}

void WorldLogWriter::QueueDropsRowsLocked(uint64 tMs)
{
    std::string rows;
    _account.AppendDropsRows(rows, tMs);
    _account.Admit(Stream::Meta, WorldLogFormat::STREAM_COUNT, 0);
    _queue.push_back(Chunk{std::move(rows), Stream::Meta, WorldLogFormat::STREAM_COUNT, tMs});
}

std::vector<std::string> WorldLogWriter::WorldLogStatus()
{
    WorldLogFormat::Settings const settings = WorldLog::CurrentSettings();
    std::vector<std::string> lines;
    std::lock_guard<std::mutex> guard(_mutex);
    lines.push_back(Acore::StringFormat(
        "WORLDLOG dir \"{}\", writer {}, state every {} s, segments of {} min, queue cap {} rows per stream, write "
        "errors {}",
        WorldLog::Dir(), _writerRunning ? "running" : "not running", settings.stateSeconds, settings.segmentMinutes,
        settings.queueRows, _writeErrors));
    for (size_t index = 0; index < WorldLogFormat::STREAM_COUNT; ++index)
    {
        WorldLogFormat::StreamCounters const& counters = _account.Of(WorldLogFormat::StreamAt(index));
        lines.push_back(Acore::StringFormat("WORLDLOG {}: written {}, dropped {}, queued {}, peak {}, segment {}",
                                            WorldLogFormat::STREAM_NAMES[index], _written[index], counters.dropped,
                                            counters.queued, counters.peak,
                                            _openNames[index].empty() ? "-" : _openNames[index]));
    }

    return lines;
}

void WorldLogWriter::StartWriter()
{
    _started = true;

    uint64 const t = WorldLog::NowMs();
    std::string const text = WorldLogFormat::StartText(WorldLog::RealmId(), WorldLog::CurrentSettings(),
                                                       WorldLog::BotLogBytes(WorldLog::CurrentRecorderSettings()));
    EnqueueWorldLog(Stream::Meta,
                    WorldLogFormat::FormatMetaRow({t, WorldLogFormat::META_START, WorldLogFormat::CONTRACT_VERSION,
                                                   sPlayerbotAIConfig.MaxRandomBots, text}),
                    1, t);
    LOG_INFO("playerbots", "[WorldLog] writing into {}", WorldLog::Dir());

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _writerRunning = true;
    }
    _writer = std::thread(&WorldLogWriter::WriterLoop, this, WorldLog::Dir());
}

// Writer thread: owns the world log's segments and touches nothing but the strings from the queue (and, under _mutex,
// the queue's counters). It wakes every WRITER_WAKE_MS, or at stop, and writes and flushes the whole queue; a wake
// with nothing queued writes nothing. The segments are only appended to. The event, state and meta segments are
// opened at start, so each has its header row before its first row. After the last batch the stop row (a = rows
// written to all streams before it, b = rows dropped) closes the world log.
void WorldLogWriter::WriterLoop(std::string dir)
{
    SegmentFiles segments(dir);
    std::array<uint64, WorldLogFormat::STREAM_COUNT> written{};
    uint64 const startMs = WorldLog::NowMs();
    for (Stream const stream : {Stream::Event, Stream::State, Stream::Meta})
        segments.Write(stream, startMs, {});

    std::vector<Chunk> batch;
    bool stop = false;
    while (!stop)
    {
        {
            std::unique_lock<std::mutex> guard(_mutex);
            _wake.wait_for(guard, std::chrono::milliseconds(WorldLogFormat::WRITER_WAKE_MS), [this] { return _stop; });
            batch.swap(_queue);
            for (Chunk const& chunk : batch)
                _account.Taken(chunk.stream, chunk.rows);

            stop = _stop;
        }

        if (batch.empty())
            continue;

        PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog writer");
        for (Chunk const& chunk : batch)
            if (segments.Write(chunk.stream, chunk.tMs, chunk.text))
                written[WorldLogFormat::StreamIndex(chunk.stream)] += chunk.rows;

        segments.Flush();
        if (pmo)
            pmo->finish();

        batch.clear();
        {
            std::lock_guard<std::mutex> guard(_mutex);
            _written = written;
            _writeErrors = segments.Errors();
            segments.PublishNames(_openNames);
        }
    }

    uint64 total = 0;
    for (uint64 const rows : written)
        total += rows;

    uint64 dropped = 0;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        dropped = _account.DroppedTotal();
    }

    WorldLogFormat::MetaRow stopRow;
    stopRow.t = WorldLog::NowMs();
    stopRow.kind = WorldLogFormat::META_STOP;
    stopRow.a = static_cast<int64>(total);
    stopRow.b = static_cast<int64>(dropped);
    if (segments.Write(Stream::Meta, stopRow.t, WorldLogFormat::FormatMetaRow(stopRow)))
        ++written[WorldLogFormat::StreamIndex(Stream::Meta)];

    segments.CloseAll();

    std::lock_guard<std::mutex> guard(_mutex);
    _written = written;
    _writeErrors = segments.Errors();
    segments.PublishNames(_openNames);
    _writerRunning = false;
}

void WorldLogWriter::Shutdown()
{
    {
        std::lock_guard<std::mutex> guard(_mutex);
        // The last drops rows go before the stop row the writer adds; the destructor calls Shutdown again after the
        // server's own call, which finds _stop set
        if (!_stop && _writerRunning)
            QueueDropsRowsLocked(WorldLog::NowMs());

        _stop = true;
    }
    _wake.notify_all();
    if (_writer.joinable())
        _writer.join();
}

char const* EventLog::CurrentSource()
{
    // An action's name is a copy (getName() returns by value); read only when an event asks for it
    thread_local std::string actionName;
    SourceState const state = currentSourceState;
    if (state.action)
    {
        actionName = state.action->getName();
        return actionName.c_str();
    }

    return state.source ? state.source : SOURCE_OTHER;
}

void EventLog::Emit(Player* bot, EventType type, char const* source, uint32 id, int64 amount)
{
    Dispatch(bot, type, Word(type), source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::Emit(Player* bot, char const* type, char const* source, uint32 id, int64 amount)
{
    if (!type)
        return;

    // The word is looked up once here (the old form only): a word the table lacks is a row only
    EventType const known = FromWord(type);
    Dispatch(bot, known, type, source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::EmitAt(uint32 bot, WorldLogFormat::Place const& place, EventType type, char const* source, uint32 id,
                      int64 amount)
{
    if (ReadBy(type, READ_ROW) && !Word(type).empty() && WorldLog::Active())
        EnqueueEventRow(bot, place, Word(type), source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::EmitAt(uint32 bot, WorldLogFormat::Place const& place, char const* type, char const* source, uint32 id,
                      int64 amount)
{
    if (type && WorldLog::Active())
        EnqueueEventRow(bot, place, type, source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::Honesty(Player* bot, HonestyKind kind, char const* source, uint32 id, int64 amount)
{
    if (bot && Wanted(TypeOf(kind)))
        Dispatch(bot, TypeOf(kind), Word(TypeOf(kind)), source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::Honesty(Player* bot, char const* kind, char const* source, uint32 id, int64 amount)
{
    if (!kind || !bot)
        return;

    HonestyKind known;
    if (HonestyKindFromWord(kind, known))
    {
        Honesty(bot, known, source, id, amount);
        return;
    }

    if (!Wanted(EventType::Unknown))
        return;

    std::string const word = std::string(HONESTY_WORD) + kind;
    Dispatch(bot, EventType::Unknown, word, source ? source : SOURCE_OTHER, id, amount);
}
