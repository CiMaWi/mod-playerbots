/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CensusMgr.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <variant>

#include "Action.h"
#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldLog.h"

namespace
{
constexpr uint32 MS_PER_SECOND = 1000;
constexpr uint32 MS_PER_HOUR = 3600 * MS_PER_SECOND;
constexpr float PERCENT = 100.0f;
constexpr size_t BYTES_PER_ROW = 160;
constexpr char const* LOCATIONS_FILE = "bot_locations.csv";
constexpr char const* EVENTS_FILE = "bot_events.csv";
constexpr char const* HONESTY_PREFIX = "Honesty";
constexpr char const* LEVEL_UP_TYPE = "LevelUp";
constexpr int64 ONE_EVENT = 1;

using WorldLogFormat::Stream;

// The event log's gate: census on and a bot (real players are not logged)
bool IsLoggedBot(Player* player) { return sPlayerbotAIConfig.CensusEnable && player && GET_PLAYERBOT_AI(player); }

// The census's gate, or the world log's: on and any player (real players too)
bool IsLogged(Player* player) { return player && (WorldLog::Active() || IsLoggedBot(player)); }

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

// One world-log event row of any player, from its own state, on the thread that updates its map (or the world thread)
void EmitWorldLogEvent(Player* player, std::string_view type, std::string_view source, uint32 id, int64 amount)
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog event");

    WorldLogFormat::EventRow row;
    row.t = WorldLog::NowMs();
    row.bot = player->GetGUID().GetCounter();
    row.type = type;
    row.source = source;
    row.id = id;
    row.amount = amount;
    row.place = PlaceOf(player);
    CensusMgr::Instance().EnqueueWorldLog(Stream::Event, WorldLogFormat::FormatEventRow(row), 1, row.t);

    if (pmo)
        pmo->finish();
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

// The census files, truncated when their first line comes
void OpenCensusFiles(std::string const& dir, std::ofstream& locations, std::ofstream& events)
{
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    locations.open(dir + LOCATIONS_FILE, std::ios::out | std::ios::trunc);
    events.open(dir + EVENTS_FILE, std::ios::out | std::ios::trunc);
    if (!locations || !events)
        LOG_ERROR("playerbots", "Census: cannot open the files in {}; the census is discarded", dir);
}

// Where a PvP kill or death happened, from the player's own state
char const* PvpPlace(Player* player)
{
    if (player->InArena())
        return "arena";

    return player->InBattleground() ? "battleground" : "world";
}

// An economy or life event whose source is the cause on this thread (the executing action, mapped to a source word)
void EmitFromScope(Player* player, char const* type, uint32 id, int64 amount)
{
    if (!IsLogged(player))
        return;

    CensusMgr::Instance().OnEvent(player, type, EventLog::SourceWordOf(EventLog::CurrentSource()), id, amount);
}

// ItemIn for the item StoreNewItem stored just before this hook: reads the note, never the item (TakeStoredEntry)
void EmitStoredItemIn(Player* player, Item* item, uint32 count, char const* source)
{
    if (uint32 const entry = EventLog::TakeStoredEntry(item))
        EventLog::Emit(player, "ItemIn", source, entry, count);
}

// The cheap, state-free columns of one sample row (the part after the time and the type). Columns, in order: name,
// "POINT(y x)", orientation, race, class, map, level, health, power%, money, grouper, guilder, allowActivity,
// in-world, combat, safe (not evaluated: n/a), alive, then the appended zoneId, areaId, inCombat.
void AppendRow(std::string& out, std::string const& time, char const* type, Player* player, PlayerbotAI* ai)
{
    Powers const powerType = player->getPowerType();
    uint32 const maxPower = std::max<uint32>(1, player->GetMaxPower(powerType));
    float const powerPercent = PERCENT * static_cast<float>(player->GetPower(powerType)) / static_cast<float>(maxPower);
    uint32 const grouper = ai ? static_cast<uint32>(ai->GetGrouperType()) : 0;
    uint32 const guilder = ai ? static_cast<uint32>(ai->GetGuilderType()) : 0;
    bool const allow = ai ? ai->IsActivityAllowedCached() : true;
    bool const combat = player->IsInCombat();

    out += Acore::StringFormat(
        "{},{},{},\"POINT({:.2f} {:.2f})\",{:.2f},{},{},{},{},{},{:.2f},{},{},{},{},{},{},n/a,{},"
        "{},{},{}\n",
        time, type, player->GetName(), player->GetPositionY(), player->GetPositionX(), player->GetOrientation(),
        static_cast<uint32>(player->getRace()), static_cast<uint32>(player->getClass()), player->GetMapId(),
        static_cast<uint32>(player->GetLevel()), player->GetHealth(), powerPercent, player->GetMoney(), grouper,
        guilder, allow ? "active" : "inactive", player->IsInWorld() ? "active" : "delay",
        combat ? "combat" : "non-combat", player->IsAlive() ? "alive" : "dead", player->GetZoneId(),
        player->GetAreaId(), combat ? 1 : 0);
}

// Level-up and event log hooks; registered once, do nothing while the census is off. They run on the thread that
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
              {PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_MONEY_CHANGED, PLAYERHOOK_ON_STORE_NEW_ITEM,
               PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM, PLAYERHOOK_ON_QUEST_REWARD_ITEM,
               PLAYERHOOK_ON_CREATE_ITEM, PLAYERHOOK_ON_AFTER_STORE_OR_EQUIP_NEW_ITEM,
               PLAYERHOOK_ON_AFTER_TAKE_ITEM_FROM_MAIL, PLAYERHOOK_CAN_SELL_ITEM, PLAYERHOOK_ON_PLAYER_RESURRECT,
               PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE})
    {
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        CensusMgr::Instance().OnLevelChanged(player, oldLevel);
    }

    // Called before the change; a loss is capped at the money the player has, as ModifyMoney does
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (amount > 0)
            EmitFromScope(player, "GoldIn", 0, amount);
        else if (amount < 0 && player && player->GetMoney())
            EmitFromScope(player, "GoldOut", 0, std::min<int64>(-static_cast<int64>(amount), player->GetMoney()));
    }

    // Inside StoreNewItem, before the scripts' item hooks: the item is alive here (the core's callers use it after
    // this hook), so its entry is read now for the hooks below, which must not read the item
    void OnPlayerStoreNewItem(Player* /*player*/, Item* item, uint32 /*count*/) override
    {
        if ((sPlayerbotAIConfig.CensusEnable || WorldLog::Active()) && item)
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
}  // namespace

CensusMgr::~CensusMgr() { Shutdown(); }

std::string CensusMgr::TimeString()
{
    std::time_t const now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S+00", &utc);
    return buffer;
}

void CensusMgr::Update(uint32 diff)
{
    bool const census = sPlayerbotAIConfig.CensusEnable;
    if (!census && !WorldLog::Active())
        return;

    if (!_started)
        StartWriter(WorldLog::Active());

    if (_worldLog)
        UpdateWorldLog(diff);

    if (census)
        UpdateCensus(diff);
}

void CensusMgr::UpdateCensus(uint32 diff)
{
    _elapsed += diff;
    if (_censusStarted && _elapsed < sPlayerbotAIConfig.CensusIntervalSeconds * MS_PER_SECOND)
        return;

    _elapsed = 0;
    if (!_censusStarted)
    {
        _censusStarted = true;
        LOG_INFO("playerbots", "Census on: every {} s into {}", sPlayerbotAIConfig.CensusIntervalSeconds,
                 LOCATIONS_FILE);
    }

    Sample();

    std::map<std::string, uint64, std::less<>> totals;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        totals = _totals;
    }
    LogTotals(totals);
}

// A state pass every StateSeconds, the drops rows every hour of uptime
void CensusMgr::UpdateWorldLog(uint32 diff)
{
    _stateElapsed += diff;
    if (_stateElapsed >= WorldLog::CurrentSettings().stateSeconds * MS_PER_SECOND)
    {
        _stateElapsed = 0;
        SampleState();
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

void CensusMgr::Sample()
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "Census");

    std::string const time = TimeString();
    std::string text;
    {
        // The players are only read here, on the world thread, while no map update runs
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        auto const& players = ObjectAccessor::GetPlayers();
        text.reserve(players.size() * BYTES_PER_ROW);
        for (auto const& pair : players)
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld())
                continue;

            PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
            AppendRow(text, time, ai ? "RND" : "PLR", player, ai);
        }
    }

    if (!text.empty())
        Enqueue(false, std::move(text));

    if (pmo)
        pmo->finish();
}

// One state row per online player (real players with the human flag), built into one chunk: a full queue drops the
// whole pass and counts its rows
void CensusMgr::SampleState()
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog state");

    uint64 const t = WorldLog::NowMs();
    std::string text;
    uint32 rows = 0;
    {
        // The players are only read here, on the world thread, while no map update runs
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        auto const& players = ObjectAccessor::GetPlayers();
        text.reserve(players.size() * WorldLogFormat::STATE_ROW_RESERVE);
        for (auto const& pair : players)
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld())
                continue;

            WorldLogFormat::AppendStateRow(text, StateRowOf(player, t));
            ++rows;
        }
    }

    if (rows)
        EnqueueWorldLog(Stream::State, std::move(text), rows, t);

    if (pmo)
        pmo->finish();
}

void CensusMgr::OnLevelChanged(Player* player, uint8 oldLevel)
{
    if (!player)
        return;

    if (WorldLog::Active())
        EmitWorldLogEvent(player, WorldLogFormat::EVENT_LEVEL, EventLog::SOURCE_OTHER, player->GetLevel(), oldLevel);

    if (!sPlayerbotAIConfig.CensusEnable || !GET_PLAYERBOT_AI(player))
        return;

    // spp-map bot_events.csv: time, name, event, "POINT(y x)", race, class, level, "info 1", "info 2"
    Enqueue(true,
            Acore::StringFormat("{},{},LevelUp,\"POINT({:.2f} {:.2f})\",{},{},{:.2f},\"{}\",\"{}\"\n", TimeString(),
                                player->GetName(), player->GetPositionY(), player->GetPositionX(),
                                static_cast<uint32>(player->getRace()), static_cast<uint32>(player->getClass()),
                                static_cast<float>(player->GetLevel()), static_cast<uint32>(oldLevel),
                                static_cast<uint32>(player->GetLevel())),
            LEVEL_UP_TYPE);
}

void CensusMgr::OnEvent(Player* bot, std::string_view type, std::string_view source, uint32 id, int64 amount,
                        uint32 worldLogId, int64 worldLogAmount)
{
    if (type.empty() || !bot)
        return;

    if (WorldLog::Active())
        EmitWorldLogEvent(bot, type, source, worldLogId == EventLog::SAME_ID ? id : worldLogId,
                          worldLogAmount == EventLog::SAME_AMOUNT ? amount : worldLogAmount);

    if (!IsLoggedBot(bot))
        return;

    Enqueue(true,
            EventLog::FormatEventLine(TimeString(), bot->GetGUID().GetCounter(), type, source, id, amount,
                                      bot->GetMapId(), bot->GetZoneId()),
            type);
}

void CensusMgr::Enqueue(bool events, std::string text, std::string_view countType)
{
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stop)
            return;

        _queue.push_back(Chunk{events ? Sink::Events : Sink::Locations, std::move(text)});
        if (!countType.empty())
        {
            auto const found = _totals.find(countType);
            if (found != _totals.end())
                ++found->second;
            else
                _totals.emplace(std::string(countType), 1);
        }
    }
    _wake.notify_one();
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

        _queue.push_back(Chunk{Sink::WorldLog, std::move(rows), stream, rowCount, tMs});
    }
    _wake.notify_one();
    return true;
}

void CensusMgr::QueueDropsRowsLocked(uint64 tMs)
{
    std::string rows;
    _account.AppendDropsRows(rows, tMs);
    _account.Admit(Stream::Meta, WorldLogFormat::STREAM_COUNT, 0);
    _queue.push_back(Chunk{Sink::WorldLog, std::move(rows), Stream::Meta, WorldLogFormat::STREAM_COUNT, tMs});
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

// One line for the hub's checks: "EventLog totals: <Type>=<n> ..." (counts since the start, types with events only)
void CensusMgr::LogTotals(std::map<std::string, uint64, std::less<>> const& totals)
{
    std::string line = "EventLog totals:";
    for (auto const& [type, count] : totals)
        line += Acore::StringFormat(" {}={}", type, count);

    if (totals.empty())
        line += " (none)";

    LOG_INFO("playerbots", "{}", line);
}

void CensusMgr::StartWriter(bool worldLog)
{
    _started = true;
    _worldLog = worldLog;

    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir.push_back('/');
    dir += "census/";

    if (worldLog)
    {
        uint64 const t = WorldLog::NowMs();
        std::string const text = WorldLogFormat::StartText(WorldLog::RealmId(), WorldLog::CurrentSettings());
        EnqueueWorldLog(Stream::Meta,
                        WorldLogFormat::FormatMetaRow({t, WorldLogFormat::META_START, WorldLogFormat::CONTRACT_VERSION,
                                                       sPlayerbotAIConfig.MaxRandomBots, text}),
                        1, t);
        LOG_INFO("playerbots", "[WorldLog] writing into {}", WorldLog::Dir());
    }

    // An empty census chunk: the writer opens (truncates) the census files at start, also with nobody online
    if (sPlayerbotAIConfig.CensusEnable)
        Enqueue(false, std::string());

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _writerRunning = true;
    }
    _writer = std::thread(&CensusMgr::WriterLoop, this, std::move(dir), worldLog ? WorldLog::Dir() : std::string());
}

// Writer thread: owns the census files and the world log's segments and touches nothing but the strings from the
// queue (and, under _mutex, the queue's counters). The census files are truncated when their first line comes; the
// world log's segments are only appended to. The event, state and meta segments are opened at start, so each has its
// header row before its first row. After the last batch the stop row (a = rows written to all streams before it, b =
// rows dropped) closes the world log.
void CensusMgr::WriterLoop(std::string censusDir, std::string worldLogDir)
{
    std::ofstream locations;
    std::ofstream events;
    bool censusOpened = false;
    std::optional<SegmentFiles> segments;
    std::array<uint64, WorldLogFormat::STREAM_COUNT> written{};
    if (!worldLogDir.empty())
    {
        segments.emplace(worldLogDir);
        uint64 const t = WorldLog::NowMs();
        for (Stream const stream : {Stream::Event, Stream::State, Stream::Meta})
            segments->Write(stream, t, {});
    }

    std::vector<Chunk> batch;
    bool stop = false;
    while (!stop)
    {
        {
            std::unique_lock<std::mutex> guard(_mutex);
            _wake.wait(guard, [this] { return _stop || !_queue.empty(); });
            batch.swap(_queue);
            for (Chunk const& chunk : batch)
                if (chunk.sink == Sink::WorldLog)
                    _account.Taken(chunk.stream, chunk.rows);

            stop = _stop;
        }

        // The whole batch, census lines included
        PerfMonitorOperation* pmo = segments ? sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog writer") : nullptr;
        for (Chunk const& chunk : batch)
        {
            if (chunk.sink == Sink::WorldLog)
            {
                if (segments && segments->Write(chunk.stream, chunk.tMs, chunk.text))
                    written[WorldLogFormat::StreamIndex(chunk.stream)] += chunk.rows;

                continue;
            }

            if (!censusOpened)
            {
                censusOpened = true;
                OpenCensusFiles(censusDir, locations, events);
            }

            std::ofstream& file = chunk.sink == Sink::Events ? events : locations;
            if (file)
                file << chunk.text;
        }

        if (censusOpened)
        {
            locations.flush();
            events.flush();
        }

        if (segments)
            segments->Flush();

        if (pmo)
            pmo->finish();

        batch.clear();
        if (segments)
        {
            std::lock_guard<std::mutex> guard(_mutex);
            _written = written;
            _writeErrors = segments->Errors();
            segments->PublishNames(_openNames);
        }
    }

    if (segments)
    {
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
        if (segments->Write(Stream::Meta, stopRow.t, WorldLogFormat::FormatMetaRow(stopRow)))
            ++written[WorldLogFormat::StreamIndex(Stream::Meta)];

        segments->CloseAll();
    }

    std::lock_guard<std::mutex> guard(_mutex);
    _written = written;
    if (segments)
    {
        _writeErrors = segments->Errors();
        segments->PublishNames(_openNames);
    }

    _writerRunning = false;
}

void CensusMgr::Shutdown()
{
    std::map<std::string, uint64, std::less<>> totals;
    bool first = false;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        first = !_stop;
        // The last drops rows go before the stop row the writer adds
        if (first && _worldLog && _writerRunning)
            QueueDropsRowsLocked(WorldLog::NowMs());

        _stop = true;
        totals = _totals;
    }
    _wake.notify_all();
    if (_writer.joinable())
        _writer.join();

    // The destructor calls Shutdown again after the server's own call: log once, while logging still runs
    if (first && _censusStarted)
        LogTotals(totals);
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

void EventLog::Emit(Player* bot, char const* type, char const* source, uint32 id, int64 amount, uint32 worldLogId,
                    int64 worldLogAmount)
{
    if (type)
        CensusMgr::Instance().OnEvent(bot, type, source ? source : SOURCE_OTHER, id, amount, worldLogId,
                                      worldLogAmount);
}

void EventLog::Honesty(Player* bot, char const* kind, char const* source, uint32 id, int64 amount)
{
    if (!kind || !IsLogged(bot))
        return;

    CensusMgr::Instance().OnEvent(bot, std::string(HONESTY_PREFIX) + kind, source ? source : SOURCE_OTHER, id, amount);
}

void AddSC_census() { new CensusPlayerScript(); }
