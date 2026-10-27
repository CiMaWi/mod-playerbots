/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RANDOMPLAYERBOTMGR_H
#define PLAYERBOTS_RANDOMPLAYERBOTMGR_H

#include "GameTime.h"
#include "NewRpgInfo.h"
#include "ObjectGuid.h"
#include "PlayerbotCommandServer.h"
#include "PlayerbotMgr.h"
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_set>

struct BattlegroundInfo
{
    std::vector<uint32> bgInstances;
    std::vector<uint32> ratedArenaInstances;
    std::vector<uint32> skirmishArenaInstances;
    uint32 bgInstanceCount = 0;
    uint32 ratedArenaInstanceCount = 0;
    uint32 skirmishArenaInstanceCount = 0;
    uint32 minLevel = 0;
    uint32 maxLevel = 0;
    uint32 activeRatedArenaQueue = 0;     // 0 = Inactive, 1 = Active
    uint32 activeSkirmishArenaQueue = 0;  // 0 = Inactive, 1 = Active
    uint32 activeBgQueue = 0;             // 0 = Inactive, 1 = Active

    // Bots (Arena)
    uint32 ratedArenaBotCount = 0;
    uint32 skirmishArenaBotCount = 0;

    // Bots (Battleground)
    uint32 bgHordeBotCount = 0;
    uint32 bgAllianceBotCount = 0;

    // Players (Arena)
    uint32 ratedArenaPlayerCount = 0;
    uint32 skirmishArenaPlayerCount = 0;

    // Players (Battleground)
    uint32 bgHordePlayerCount = 0;
    uint32 bgAlliancePlayerCount = 0;
};

class ChatHandler;
class PerfMonitorOperation;
class WorldLocation;

struct CachedEvent
{
    uint32 value = 0;
    uint32 lastChangeTime = 0;
    uint32 validIn = 0;
    std::string data;

    bool IsEmpty() const { return !lastChangeTime; }
};

struct BotEventCache
{
    bool loaded = false;
    std::unordered_map<std::string, CachedEvent> events;
};

// https://gist.github.com/bradley219/5373998

class botPIDImpl;
class botPID
{
public:
    // Kp -  proportional gain
    // Ki -  Integral gain
    // Kd -  derivative gain
    // dt -  loop interval time
    // max - maximum value of manipulated variable
    // min - minimum value of manipulated variable
    botPID(double dt, double max, double min, double Kp, double Ki, double Kd);
    void adjust(double Kp, double Ki, double Kd);
    void reset();

    double calculate(double setpoint, double pv);
    ~botPID();

private:
    botPIDImpl* pimpl;
};

class ChatHandler;

class RandomPlayerbotMgr : public PlayerbotHolder
{
public:
    static RandomPlayerbotMgr& instance()
    {
        static RandomPlayerbotMgr instance;

        return instance;
    }

    void LogPlayerLocation();
    void UpdateAIInternal(uint32 elapsed, bool minimal = false) override;

    uint32 activeBots = 0;
    static bool HandlePlayerbotConsoleCommand(ChatHandler* handler, char const* args);
    bool IsRandomBot(Player* bot);
    bool IsRandomBot(ObjectGuid::LowType bot);
    bool IsAddclassBot(Player* bot);
    bool IsAddclassBot(ObjectGuid::LowType bot);
    void Randomize(Player* bot);
    void Clear(Player* bot);
    void RandomizeFirst(Player* bot);
    void RandomizeMin(Player* bot);
    void IncreaseLevel(Player* bot);
    void ScheduleTeleport(uint32 bot, uint32 time = 0);
    void ScheduleChangeStrategy(uint32 bot, uint32 time = 0);
    void HandleCommand(uint32 type, std::string const text, Player* fromPlayer, std::string channelName = "");
    std::string const HandleRemoteCommand(std::string const request);
    void OnPlayerLogout(Player* player);
    void OnPlayerLogin(Player* player);
    void OnPlayerLoginError(uint32 bot);
    Player* GetRandomPlayer();
    std::vector<Player*> GetPlayers() { return players; };
    PlayerBotMap GetAllBots() { return playerBots; };
    void InitArenaTeams();
    void PrintStats();
    double GetBuyMultiplier(Player* bot);
    double GetSellMultiplier(Player* bot);
    void AddTradeDiscount(Player* bot, Player* master, int32 value);
    void SetTradeDiscount(Player* bot, Player* master, uint32 value);
    uint32 GetTradeDiscount(Player* bot, Player* master);
    void Refresh(Player* bot);
    // Ladder moves: log a random bot out and keep it out of currentBots, or let it log in again (not over the cap;
    // overCap: a short login that is logged out again at once, e.g. a depart's unwear, also at the cap)
    void LadderLogout(uint32 bot);
    void LadderLogin(uint32 bot, bool overCap = false);
    void RandomTeleportForLevel(Player* bot);
    void RandomTeleportGrindForLevel(Player* bot);
    void RandomTeleportForRpg(Player* bot);
    uint32 GetMaxAllowedBotCount();
    bool ProcessBot(Player* player);
    void Revive(Player* player);
    // Playerbots.ArenaOnly.SpreadContinents, world thread: moves a waiting bot (not in a battleground or its queue,
    // alive, out of combat) to its home spot when it stands on another map; true when it teleported
    bool PlaceArenaOnlyHome(Player* bot);
    // The home continent (0 Eastern Kingdoms, 1 Kalimdor, 2 Outland, 3 Northrend) of a key: the bot's first arena
    // team id (both members share it), else its guid; the spot is one of three per continent and faction
    static uint32 ArenaOnlyHomeKey(Player* bot);
    static uint32 ArenaOnlyContinent(uint32 key) { return key % 4; }
    static WorldLocation ArenaOnlyHomeSpot(uint32 key, bool horde);
    // The rated arena queue opening one check sets (Playerbots.RandomBotAutoJoinRatedArenaQueueSlots, ...Uncapped)
    // and whether a bot captain may queue on it (BGJoinAction::shouldJoinBg)
    static uint32 RatedArenaQueueOpening(uint32 slots, uint32 count, uint32 running, bool uncapped);
    static bool RatedArenaCaptainMayQueue(uint32 queuedPlayers, uint32 bracketSize, uint32 opening, uint32 running,
                                          bool uncapped);
    // .playerbots arenaonly status | selftest
    static bool HandleArenaOnlyStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleArenaOnlySelftestCommand(ChatHandler* handler, char const* args);
    // Playerbots.ArenaOnly.Enable, any thread: one idle cycle of a bot (back from a battleground -> next queue, ms)
    // and one rated join attempt of a captain (sent: the queue packet went out; else the team was not ready)
    static void NoteArenaOnlyIdle(uint32 ms);
    static void NoteArenaOnlyJoin(bool sent);
    // Playerbots.ArenaOnly.Enable, any thread: an invite accepted by BGStatusCheckAction without the status round trip
    static void NoteArenaOnlyInviteAccepted();
    void ChangeStrategy(Player* player);
    void ChangeStrategyOnce(Player* player);
    uint32 GetValue(Player* bot, std::string const& type);
    uint32 GetValue(uint32 bot, std::string const& type);
    std::string GetData(uint32 bot, std::string const& type);
    void SetValue(uint32 bot, std::string const& type, uint32 value, std::string const& data = "");
    void SetValue(Player* bot, std::string const& type, uint32 value, std::string const& data = "");
    bool IsSpecPvp(uint32 bot, uint8 cls);
    void Remove(Player* bot);
    // Loads the bot's events into the cache unless loaded; world thread (bot login), so map threads find them cached.
    void PreloadEvents(uint32 bot);
    ObjectGuid GetBattleMasterGUID(Player* bot, BattlegroundTypeId bgTypeId);
    CreatureData const* GetCreatureDataByEntry(uint32 entry);
    void LoadBattleMastersCache();
    std::map<uint32, std::map<uint32, BattlegroundInfo>> BattlegroundData;
    std::map<uint32, std::map<uint32, std::map<TeamId, uint32>>> VisualBots;
    std::map<uint32, std::map<uint32, std::map<uint32, uint32>>> Supporters;
    std::map<TeamId, std::vector<uint32>> LfgDungeons;
    void CheckBgQueue();
    void CheckLfgQueue();
    void CheckPlayers();
    void LogBattlegroundInfo();

    std::map<TeamId, std::map<BattlegroundTypeId, std::vector<uint32>>> getBattleMastersCache()
    {
        return BattleMastersCache;
    }

    float getActivityMod() { return activityMod; }
    float getActivityPercentage() { return activityMod * 100.0f; }
    void setActivityPercentage(float percentage) { activityMod = percentage / 100.0f; }
    static uint8 GetTeamClassIdx(bool isAlliance, uint8 claz) { return isAlliance * 20 + claz; }

    void PrepareAddclassCache();
    void Init();
    std::map<uint8, std::unordered_set<ObjectGuid>> addclassCache;

    // Account type management
    void AssignAccountTypes();
    bool IsAccountType(uint32 accountId, uint8 accountType);
    bool IsAddClassAccount(uint32 accountId) const;

protected:
    void OnBotLoginInternal(Player* const bot) override;

private:
    RandomPlayerbotMgr() : PlayerbotHolder()
    {
        this->playersLevel = sPlayerbotAIConfig.RandomBotStartingLevel;

        if (sPlayerbotAIConfig.Enabled || sPlayerbotAIConfig.RandomBotAutologin)
        {
            PlayerbotCommandServer::instance().Start();
        }

        BattlegroundData.clear();  // Clear here and here only.

        // Cleanup on server start: orphaned pet data that's often left behind by bot pets that no longer exist in the DB
        CharacterDatabase.Execute("DELETE FROM pet_aura WHERE guid NOT IN (SELECT id FROM character_pet)");
        CharacterDatabase.Execute("DELETE FROM pet_spell WHERE guid NOT IN (SELECT id FROM character_pet)");
        CharacterDatabase.Execute("DELETE FROM pet_spell_cooldown WHERE guid NOT IN (SELECT id FROM character_pet)");

        for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
        {
            for (int queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
            {
                this->BattlegroundData[queueType][bracket] = BattlegroundInfo();
            }
        }

        this->BgCheckTimer = 0;
        this->LfgCheckTimer = 0;
        this->PlayersCheckTimer = 0;
    }

    ~RandomPlayerbotMgr() = default;

    RandomPlayerbotMgr(RandomPlayerbotMgr const&) = delete;
    RandomPlayerbotMgr& operator=(RandomPlayerbotMgr const&) = delete;

    RandomPlayerbotMgr(RandomPlayerbotMgr&&) = delete;
    RandomPlayerbotMgr& operator=(RandomPlayerbotMgr&&) = delete;

    // pid values are set in constructor
    botPID pid = botPID(1, 50, -50, 0, 0, 0);
    float activityMod = 0.25;
    bool _isBotInitializing = true;
    bool _isBotLogging = true;
    NewRpgStatistic rpgStasticTotal;
    // Event cache: read from map threads (bot AI) and the world thread, guarded by _eventCacheLock. The helpers below
    // run with the lock held and never hand a pointer into the cache to callers; no DB query runs with the lock held.
    static void CopyEventLocked(BotEventCache& cache, std::string const& event, uint32* value, std::string* data);
    void ReadEvent(uint32 bot, std::string const& event, uint32* value, std::string* data);
    void LoadEvents(uint32 bot, bool lazy);
    uint32 GetEventValue(uint32 bot, std::string const& event);
    std::string GetEventData(uint32 bot, std::string const& event);
    uint32 SetEventValue(uint32 bot, std::string const& event, uint32 value, uint32 validIn,
                         std::string const& data = "");
    void GetBots();
    std::vector<uint32> GetBgBots(uint32 bracket);
    time_t BgCheckTimer;
    time_t LfgCheckTimer;
    time_t PlayersCheckTimer;
    time_t RealPlayerLastTimeSeen = 0;
    time_t DelayLoginBotsTimer;
    time_t printStatsTimer;
    uint32 AddRandomBots();
    bool ProcessBot(uint32 bot);
    void ScheduleRandomize(uint32 bot, uint32 time);
    void RandomTeleport(Player* bot);
    void RandomTeleport(Player* bot, std::vector<WorldLocation>& locs, bool hearth = false);
    uint32 GetZoneLevel(uint16 mapId, float teleX, float teleY, float teleZ);
    void UpdatePlayerZones();
    std::vector<WorldLocation> GetPlayerZoneTeleportLocations(std::vector<WorldLocation> const& locs, Player* bot);
    typedef void (RandomPlayerbotMgr::*ConsoleCommandHandler)(Player*);
    std::vector<Player*> players;

    // std::map<uint32, std::vector<WorldLocation>> rpgLocsCache;
    std::map<uint32, std::map<uint32, std::vector<WorldLocation>>> rpgLocsCacheLevel;
    std::map<TeamId, std::map<BattlegroundTypeId, std::vector<uint32>>> BattleMastersCache;
    std::unordered_map<uint32, ObjectGuid::LowType> BattleMasterSpawnIds;
    std::unordered_map<uint32, BotEventCache> eventCache;
    std::mutex _eventCacheLock;
    std::atomic<uint32> _offThreadEventLoads{0};  // lazy event loads that ran off the world thread
    std::unordered_set<uint32> currentBots;
    uint32 playersLevel;
    // Written on the world thread, read from map threads.
    std::set<std::pair<uint32, uint32>> _playerZones;
    std::mutex _playerZonesLock;

    // Account lists
    std::vector<uint32> rndBotTypeAccounts;             // Accounts marked as RNDbot (type 1)
    std::vector<uint32> addClassTypeAccounts;           // Accounts marked as AddClass (type 2)

    //void ScaleBotActivity();      // Deprecated function
    static inline uint32 NowSeconds() { return static_cast<uint32>(GameTime::GetGameTime().count()); }
};

#define sRandomPlayerbotMgr RandomPlayerbotMgr::instance()

#endif
