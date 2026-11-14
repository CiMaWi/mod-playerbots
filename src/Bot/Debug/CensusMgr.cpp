/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CensusMgr.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <utility>
#include <variant>

#include "Action.h"
#include "BotRecordMgr.h"
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
#include "StringFormat.h"
#include "TravelLegRules.h"
#include "UpkeepRules.h"
#include "WorldLog.h"
#include "WorldLogBotState.h"

namespace
{
constexpr uint32 MS_PER_SECOND = 1000;
constexpr uint32 MS_PER_HOUR = 3600 * MS_PER_SECOND;
constexpr char const* HONESTY_PREFIX = "Honesty";
constexpr int64 ONE_EVENT = 1;

using WorldLogFormat::Stream;

// The world log's gate: on and any player (real players too)
bool IsLogged(Player* player) { return player && WorldLog::Active(); }

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
    CensusMgr::Instance().EnqueueWorldLog(Stream::Event, WorldLogFormat::FormatEventRow(row), 1, row.t);

    if (pmo)
        pmo->finish();
}

// One world-log event row of any player, from its own state, on the thread that updates its map (or the world thread)
void EmitWorldLogEvent(Player* player, std::string_view type, std::string_view source, uint32 id, int64 amount)
{
    EnqueueEventRow(player->GetGUID().GetCounter(), PlaceOf(player), type, source, id, amount);

    // A bot's event also goes into its recorder ring (WL8; its own PerfMonitor row)
    WorldLog::NoteEvent(player, type, source, id, amount);
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

// Where a PvP kill or death happened, from the player's own state
char const* PvpPlace(Player* player)
{
    if (player->InArena())
        return "arena";

    return player->InBattleground() ? "battleground" : "world";
}

// A travelling bot's death counts on its trip and leg at the death itself (WT10e-2c round 3): counted at the engine
// change, a bot revived and killed again before its next AI tick never left the dead engine, and that death was lost
void NoteTravelDeath(Player* killed)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(killed);
    if (!botAI)
        return;
    auto* travel = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!travel)
        return;
    TravelLegs::NoteLife(travel->trip, TravelLegs::Life::Dead, getMSTime());
    // A death on a fly leg after its takeoff (upkeep WU10): FlightFall death, amount the leg's number. A bot killed on
    // the ground before it took off (WU11b fix 7: Glettla, killed at Valgarde by the mob that had killed it there
    // before) died a ground death, counted by its Death row only
    TravelLegs::Trip const& trip = travel->trip;
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly && trip.seenInFlight)
        EventLog::Emit(killed, Upkeep::EVENT_FLIGHT_FALL, "death", travel->tripId, trip.legIdx + 1);
}

// An economy or life event whose source is the cause on this thread (the executing action, mapped to a source word)
void EmitFromScope(Player* player, char const* type, uint32 id, int64 amount)
{
    if (!IsLogged(player))
        return;

    CensusMgr::Instance().OnEvent(player, type, EventLog::SourceWordOf(EventLog::CurrentSource()), id, amount);
}

// Set from OnPlayerBeforeLootMoney to the loot script's OnLootMoney on the thread that handles the loot window: the
// money changes between them are the loot's gold (the looter's or the group's shares)
thread_local bool lootMoneyOpen = false;

// ItemIn for the item StoreNewItem stored just before this hook: reads the note, never the item (TakeStoredEntry)
void EmitStoredItemIn(Player* player, Item* item, uint32 count, char const* source)
{
    if (uint32 const entry = EventLog::TakeStoredEntry(item))
        EventLog::Emit(player, "ItemIn", source, entry, count);
}

// Level-up and event log hooks; registered once, do nothing while the world log is off. They run on the thread that
// updates the player's map (or the world thread): they read only the hooked players and queue one line each.
// StoreNewItem is not counted (loot, quest, vendor and craft items pass through it before their own hook below); it
// only notes the stored item's entry for those hooks. Master-loot items and disenchant-roll results have no hook of
// their own and are not counted as ItemIn.
class CensusPlayerScript : public PlayerScript
{
public:
    CensusPlayerScript()
        : PlayerScript(
              "CensusPlayerScript",
              {PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_MONEY_CHANGED, PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
               PLAYERHOOK_ON_STORE_NEW_ITEM, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM,
               PLAYERHOOK_ON_QUEST_REWARD_ITEM, PLAYERHOOK_ON_CREATE_ITEM, PLAYERHOOK_ON_AFTER_STORE_OR_EQUIP_NEW_ITEM,
               PLAYERHOOK_ON_AFTER_TAKE_ITEM_FROM_MAIL, PLAYERHOOK_CAN_SELL_ITEM, PLAYERHOOK_ON_PLAYER_RESURRECT,
               PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE})
    {
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        CensusMgr::Instance().OnLevelChanged(player, oldLevel);
    }

    // Called before the change; a loss is capped at the money the player has, as ModifyMoney does. Looted gold is
    // paid while the loot window's money is handled (the bots' loot action only queues the packet), so its source
    // comes from that window, not from the action scope.
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (amount > 0 && lootMoneyOpen)
            EventLog::Emit(player, "GoldIn", "loot", 0, amount);
        else if (amount > 0)
            EmitFromScope(player, "GoldIn", 0, amount);
        else if (amount < 0 && player && player->GetMoney())
            EmitFromScope(player, "GoldOut", 0, std::min<int64>(-static_cast<int64>(amount), player->GetMoney()));
    }

    void OnPlayerBeforeLootMoney(Player* /*player*/, Loot* /*loot*/) override { lootMoneyOpen = true; }

    // Inside StoreNewItem, before the scripts' item hooks: the item is alive here (the core's callers use it after
    // this hook), so its entry is read now for the hooks below, which must not read the item
    void OnPlayerStoreNewItem(Player* /*player*/, Item* item, uint32 /*count*/) override
    {
        if (WorldLog::Active() && item)
            EventLog::NoteStoredItem(item, item->GetEntry());
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        EmitStoredItemIn(player, item, count, "loot");
    }

    // A won need or greed roll: Group stores the item itself and fires only this hook, not OnPlayerLootItem
    void OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 count, RollVote /*voteType*/,
                                     Roll* /*roll*/) override
    {
        EmitStoredItemIn(player, item, count, "loot");
    }

    void OnPlayerQuestRewardItem(Player* player, Item* item, uint32 count) override
    {
        EmitStoredItemIn(player, item, count, "quest");
    }

    void OnPlayerCreateItem(Player* player, Item* item, uint32 count) override
    {
        EmitStoredItemIn(player, item, count, "craft");
    }

    // count is the number of vendor stacks bought; the item is null when the store failed and is never read (the
    // template is static data)
    void OnPlayerAfterStoreOrEquipNewItem(Player* player, uint32 /*vendorslot*/, Item* item, uint8 count, uint8 /*bag*/,
                                          uint8 /*slot*/, ItemTemplate const* proto, Creature* /*vendor*/,
                                          VendorItem const* /*crItem*/, bool /*bStore*/) override
    {
        if (item && proto)
            EventLog::Emit(player, "ItemIn", "vendor", proto->ItemId, static_cast<int64>(proto->BuyCount) * count);
    }

    // A mail item is moved into the bags, not stored new, so there is no note and the item is read here: safe while
    // no script registered before this one destroys the taken item in this hook (none does)
    void OnPlayerAfterTakeItemFromMail(Player* player, Item* item, uint32 count) override
    {
        if (item && IsLogged(player))
            EventLog::Emit(player, "ItemIn", "mail", item->GetEntry(), count);
    }

    // "sold" ItemOut is an upper bound: a later check of the sell handler can still refuse the sale, and the whole
    // stack is counted where SellAction sells only the part that quest items do not need. The item is alive here:
    // the sell handler reads it after this hook.
    [[nodiscard]] bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
    {
        if (item)
            EventLog::Emit(player, "ItemOut", "sold", item->GetEntry(), item->GetCount());

        return true;
    }

    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
    {
        EmitFromScope(player, "DeathReturn", 0, ONE_EVENT);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (quest)
            EventLog::Emit(player, "QuestDone", "quest", quest->GetQuestId(), ONE_EVENT);
    }

    // Environmental damage (falling, lava, drowning) is dealt by the player to itself
    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        Planner::QuestStatDeath(killed);
        NoteTravelDeath(killed);
        if (killer == killed)
        {
            EventLog::Emit(killed, "Death", "environment", 0, ONE_EVENT);
            return;
        }

        uint32 const killerId = killer->GetGUID().GetCounter();
        EventLog::Emit(killed, "Death", "player", killerId, ONE_EVENT);
        EventLog::Emit(killed, "PvpDeath", PvpPlace(killed), killerId, ONE_EVENT);
        EventLog::Emit(killer, "PvpKill", PvpPlace(killer), killed->GetGUID().GetCounter(), ONE_EVENT);
    }

    // A player's pet, totem or guardian is a creature: its kill is a PvP death with the owner as the killer
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        Planner::QuestStatDeath(killed);
        NoteTravelDeath(killed);
        ObjectGuid const owner = killer->GetOwnerGUID();
        if (!owner.IsPlayer())
        {
            EventLog::Emit(killed, "Death", "creature", killer->GetEntry(), ONE_EVENT);
            return;
        }

        EventLog::Emit(killed, "Death", "player_pet", owner.GetCounter(), ONE_EVENT);
        EventLog::Emit(killed, "PvpDeath", PvpPlace(killed), owner.GetCounter(), ONE_EVENT);
    }
};

// Ends the loot money window OnPlayerBeforeLootMoney opened: the core calls it after paying the gold
class CensusLootScript : public LootScript
{
public:
    CensusLootScript() : LootScript("CensusLootScript", {LOOTHOOK_ON_LOOT_MONEY}) {}

    void OnLootMoney(Player* /*player*/, uint32 /*gold*/) override { lootMoneyOpen = false; }
};
}  // namespace

CensusMgr::~CensusMgr() { Shutdown(); }

void CensusMgr::Update(uint32 diff)
{
    if (!WorldLog::Active())
        return;

    if (!_started)
        StartWriter();

    UpdateWorldLog(diff);
}

// A state pass every StateSeconds and a focus pass every FOCUS_SECONDS between two, the drops rows every hour of
// uptime
void CensusMgr::UpdateWorldLog(uint32 diff)
{
    _stateElapsed += diff;
    _focusElapsed += diff;
    if (_stateElapsed >= WorldLog::CurrentSettings().stateSeconds * MS_PER_SECOND)
    {
        _stateElapsed = 0;
        _focusElapsed = 0;
        SampleState(false);
    }
    else if (_focusElapsed >= WorldLogFormat::FOCUS_SECONDS * MS_PER_SECOND)
    {
        _focusElapsed = 0;
        SampleState(true);
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
    _wake.notify_one();
}

// One state row per online player (real players with the human flag), built into one chunk: a full queue drops the
// whole pass and counts its rows. A focus pass keeps the real players and the bots within FOCUS_YARDS of one; with no
// real player online it ends after one read of the player list.
void CensusMgr::SampleState(bool focusOnly)
{
    PerfMonitorOperation* pmo =
        sPerfMonitor.start(PERF_MON_TOTAL, focusOnly ? "WorldLog state focus" : "WorldLog state");

    uint64 const t = WorldLog::NowMs();
    std::string text;
    uint32 rows = 0;
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
        }
        else
            text.reserve(players.size() * WorldLogFormat::STATE_ROW_RESERVE);

        if (!focusOnly || !_focusPoints.empty())
        {
            for (auto const& pair : players)
            {
                Player* player = pair.second;
                if (!player || !player->IsInWorld())
                    continue;

                if (focusOnly &&
                    !WorldLogFormat::InFocus(_focusPoints, WorldLog::FocusPointOf(player), WorldLogFormat::FOCUS_YARDS))
                    continue;

                WorldLogFormat::AppendStateRow(text, StateRowOf(player, t));
                ++rows;
            }
        }
    }

    if (rows)
        EnqueueWorldLog(Stream::State, std::move(text), rows, t);

    if (pmo)
        pmo->finish();
}

void CensusMgr::OnLevelChanged(Player* player, uint8 oldLevel)
{
    if (player && EventLog::Wanted())
        OnEvent(player, WorldLogFormat::EVENT_LEVEL, EventLog::SOURCE_OTHER, player->GetLevel(), oldLevel);
}

void CensusMgr::OnEvent(Player* bot, std::string_view type, std::string_view source, uint32 id, int64 amount)
{
    if (type.empty() || !bot)
        return;

    if (WorldLog::Active())
        EmitWorldLogEvent(bot, type, source, id, amount);

    // The bots' record takes its events whether or not the world log runs (its Record row follows the cause's row)
    BotRecordMgr::Instance().OnEvent(bot, type, source, id, amount);
}

bool CensusMgr::EnqueueWorldLog(Stream stream, std::string rows, uint32 rowCount, uint64 tMs)
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
    _wake.notify_one();
    return true;
}

void CensusMgr::QueueDropsRowsLocked(uint64 tMs)
{
    std::string rows;
    _account.AppendDropsRows(rows, tMs);
    _account.Admit(Stream::Meta, WorldLogFormat::STREAM_COUNT, 0);
    _queue.push_back(Chunk{std::move(rows), Stream::Meta, WorldLogFormat::STREAM_COUNT, tMs});
}

std::vector<std::string> CensusMgr::WorldLogStatus()
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

void CensusMgr::StartWriter()
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
    _writer = std::thread(&CensusMgr::WriterLoop, this, WorldLog::Dir());
}

// Writer thread: owns the world log's segments and touches nothing but the strings from the queue (and, under _mutex,
// the queue's counters). The segments are only appended to. The event, state and meta segments are opened at start,
// so each has its header row before its first row. After the last batch the stop row (a = rows written to all streams
// before it, b = rows dropped) closes the world log.
void CensusMgr::WriterLoop(std::string dir)
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
            _wake.wait(guard, [this] { return _stop || !_queue.empty(); });
            batch.swap(_queue);
            for (Chunk const& chunk : batch)
                _account.Taken(chunk.stream, chunk.rows);

            stop = _stop;
        }

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

void CensusMgr::Shutdown()
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

void EventLog::Emit(Player* bot, char const* type, char const* source, uint32 id, int64 amount)
{
    if (type)
        CensusMgr::Instance().OnEvent(bot, type, source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::EmitAt(uint32 bot, WorldLogFormat::Place const& place, char const* type, char const* source, uint32 id,
                      int64 amount)
{
    if (type && WorldLog::Active())
        EnqueueEventRow(bot, place, type, source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::Honesty(Player* bot, char const* kind, char const* source, uint32 id, int64 amount)
{
    if (!kind || !IsLogged(bot))
        return;

    CensusMgr::Instance().OnEvent(bot, std::string(HONESTY_PREFIX) + kind, source ? source : SOURCE_OTHER, id, amount);
}

void AddSC_census()
{
    new CensusPlayerScript();
    new CensusLootScript();
}
