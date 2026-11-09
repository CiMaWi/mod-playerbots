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
#include <shared_mutex>
#include <utility>

#include "Action.h"
#include "Config.h"
#include "Creature.h"
#include "Item.h"
#include "Log.h"
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

namespace
{
constexpr uint32 MS_PER_SECOND = 1000;
constexpr float PERCENT = 100.0f;
constexpr size_t BYTES_PER_ROW = 160;
constexpr char const* LOCATIONS_FILE = "bot_locations.csv";
constexpr char const* EVENTS_FILE = "bot_events.csv";
constexpr char const* HONESTY_PREFIX = "Honesty";
constexpr char const* LEVEL_UP_TYPE = "LevelUp";
constexpr int64 ONE_EVENT = 1;

// The event log's gate: census on and a bot (real players are not logged)
bool IsLoggedBot(Player* player) { return sPlayerbotAIConfig.CensusEnable && player && GET_PLAYERBOT_AI(player); }

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
    if (!IsLoggedBot(player))
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
        if (sPlayerbotAIConfig.CensusEnable && item)
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
        if (item && IsLoggedBot(player))
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
    if (!sPlayerbotAIConfig.CensusEnable)
        return;

    _elapsed += diff;
    if (_started && _elapsed < sPlayerbotAIConfig.CensusIntervalSeconds * MS_PER_SECOND)
        return;

    _elapsed = 0;
    if (!_started)
        StartWriter();

    Sample();

    std::map<std::string, uint64, std::less<>> totals;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        totals = _totals;
    }
    LogTotals(totals);
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

void CensusMgr::OnLevelChanged(Player* player, uint8 oldLevel)
{
    if (!sPlayerbotAIConfig.CensusEnable || !player || !GET_PLAYERBOT_AI(player))
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

void CensusMgr::OnEvent(Player* bot, std::string_view type, std::string_view source, uint32 id, int64 amount)
{
    if (type.empty() || !IsLoggedBot(bot))
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

        _queue.push_back(Chunk{events, std::move(text)});
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

void CensusMgr::StartWriter()
{
    _started = true;

    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir.push_back('/');
    dir += "census/";

    _writer = std::thread(&CensusMgr::WriterLoop, this, std::move(dir));
    LOG_INFO("playerbots", "Census on: every {} s into {}", sPlayerbotAIConfig.CensusIntervalSeconds, LOCATIONS_FILE);
}

// Writer thread: owns the two files and touches nothing but strings from the queue. Files are truncated at start.
void CensusMgr::WriterLoop(std::string dir)
{
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::ofstream locations(dir + LOCATIONS_FILE, std::ios::out | std::ios::trunc);
    std::ofstream events(dir + EVENTS_FILE, std::ios::out | std::ios::trunc);
    if (!locations || !events)
        LOG_ERROR("playerbots", "Census: cannot open the files in {}; the census is discarded", dir);

    std::vector<Chunk> batch;
    bool stop = false;
    while (!stop)
    {
        {
            std::unique_lock<std::mutex> guard(_mutex);
            _wake.wait(guard, [this] { return _stop || !_queue.empty(); });
            batch.swap(_queue);
            stop = _stop;
        }

        for (Chunk const& chunk : batch)
        {
            std::ofstream& file = chunk.events ? events : locations;
            if (file)
                file << chunk.text;
        }

        locations.flush();
        events.flush();
        batch.clear();
    }
}

void CensusMgr::Shutdown()
{
    std::map<std::string, uint64, std::less<>> totals;
    bool first = false;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        first = !_stop;
        _stop = true;
        totals = _totals;
    }
    _wake.notify_all();
    if (_writer.joinable())
        _writer.join();

    // The destructor calls Shutdown again after the server's own call: log once, while logging still runs
    if (first && _started)
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

void EventLog::Emit(Player* bot, char const* type, char const* source, uint32 id, int64 amount)
{
    if (type)
        CensusMgr::Instance().OnEvent(bot, type, source ? source : SOURCE_OTHER, id, amount);
}

void EventLog::Honesty(Player* bot, char const* kind, char const* source, uint32 id, int64 amount)
{
    if (!kind || !IsLoggedBot(bot))
        return;

    CensusMgr::Instance().OnEvent(bot, std::string(HONESTY_PREFIX) + kind, source ? source : SOURCE_OTHER, id, amount);
}

void AddSC_census() { new CensusPlayerScript(); }
