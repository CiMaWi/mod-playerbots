/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Population.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AcquaintanceMgr.h"
#include "AsyncCallbackProcessor.h"
#include "BotProfileMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "LFGMgr.h"
#include "LadderMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotsDatabase.h"
#include "PopulationPolicy.h"
#include "PvpGearMgr.h"
#include "QueryCallback.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "World.h"

namespace
{
std::atomic<bool> s_persistent{false};
std::atomic<bool> s_rotation{false};

constexpr char const* RotationEnableKey = "Playerbots.Population.Rotation.Enable";
constexpr char const* RosterSizeKey = "Playerbots.Population.Rotation.RosterSize";
constexpr uint32 DefaultRosterSize = 2500;

struct RosterBot
{
    uint32 guid;
    uint8 team;
    uint8 classId;
};

// World thread only
struct Rotation
{
    PopulationPolicy::RotationSettings settings;
    uint32 rosterSize = DefaultRosterSize;
    uint32 acquaintedMinScore = 1;
    PopulationPolicy::RotationSchedule schedule;
    std::vector<RosterBot> roster;
    std::unordered_set<uint32> rosterGuids;
    bool rosterDirty = true;
    size_t rosterAccounts = 0;
    uint32 nextTick = 0;
    uint32 lastTick = 0;
    std::unordered_set<uint32> acquainted;
    std::vector<PopulationPolicy::RotationEntry> entries;
    PopulationPolicy::RotationTickResult last;
    uint32 lastExtra = 0;
    uint64 ticks = 0;
    uint64 swaps = 0;
    uint64 acquaintedLogins = 0;
    uint64 extraLogouts = 0;
};

Rotation& GetRotation()
{
    static Rotation rotation;
    return rotation;
}

uint32 Now() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

// Online, but a logout now would cut something short
bool IsPinned(Player* bot)
{
    if (!bot)
        return false;  // still logging in: its minimum session protects it

    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInFlight() || bot->IsInCombat())
        return true;

    if (bot->GetGroup() || bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
        return true;

    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return true;

    // Logged in over the cap by a ladder move (PvP gear comes off before a depart), or its team arrives or departs
    for (uint8 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
    {
        uint32 const teamId = bot->GetArenaTeamId(slot);
        if (teamId && LadderMgr::Instance().IsMoving(teamId))
            return true;
    }

    Map* map = bot->GetMap();
    return map && map->Instanceable();
}

// A member of an arena team, the ladder's teams included: outside the roster it is left to the ladder (members it
// released, resident teams), not logged out by the extra-logout pass
bool InArenaTeam(Player* bot)
{
    if (!bot)
        return false;

    for (uint8 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
        if (bot->GetArenaTeamId(slot))
            return true;

    return false;
}

std::string BotName(uint32 guid)
{
    std::string name;
    if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(guid), name))
        name = "?";

    return name;
}

// One synchronous query per account, as AddRandomBots does: at the first tick, after ".reload config" and when H4
// invalidates the roster
void LoadRoster(Rotation& rotation, std::vector<uint32> const& accounts)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Population::LoadRoster");
    rotation.roster.clear();
    for (uint32 const accountId : accounts)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_INFO_CHARS);
        stmt->SetData(0, accountId);
        PreparedQueryResult result = CharacterDatabase.Query(stmt);
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            uint8 const race = fields[3].Get<uint8>();
            RosterBot bot;
            bot.guid = fields[0].Get<uint32>();
            bot.team = Player::TeamIdForRace(race) == TEAM_ALLIANCE ? 0 : 1;
            bot.classId = fields[4].Get<uint8>();
            rotation.roster.push_back(bot);
        } while (result->NextRow());
    }

    // The oldest characters form the roster when the accounts hold more
    std::sort(rotation.roster.begin(), rotation.roster.end(),
              [](RosterBot const& a, RosterBot const& b) { return a.guid < b.guid; });
    size_t const found = rotation.roster.size();
    if (rotation.rosterSize && found > rotation.rosterSize)
        rotation.roster.resize(rotation.rosterSize);

    rotation.rosterGuids.clear();
    for (RosterBot const& bot : rotation.roster)
        rotation.rosterGuids.insert(bot.guid);

    rotation.rosterDirty = false;
    rotation.rosterAccounts = accounts.size();
    LOG_INFO("playerbots", "[Population] rotation roster: {} characters on {} accounts (found {}, RosterSize {})",
             rotation.roster.size(), accounts.size(), found, rotation.rosterSize);
    if (rotation.rosterSize && found < rotation.rosterSize)
        LOG_WARN("playerbots",
                 "[Population] rotation roster: {} characters, fewer than RosterSize {}; accounts and characters are "
                 "made at startup (RosterSize sizes them), restart to create the rest",
                 found, rotation.rosterSize);
}

// H3: level-range balance (Playerbots.Population.Balance.*)
constexpr char const* BalanceEnableKey = "Playerbots.Population.Balance.Enable";
constexpr size_t LevelSlots = 256;

std::atomic<bool> s_balance{false};
// XP multiplier per level for the XP hook (any thread, relaxed; 0 = never written, read as 1). Written on the world
// thread at each balance update and config load.
std::atomic<float> s_levelXP[LevelSlots];

// World thread only
struct Balance
{
    PopulationPolicy::BalanceSettings settings;
    uint32 intervalSeconds = 600;
    std::vector<PopulationPolicy::BalanceRange> ranges;
    std::vector<uint32> counts;
    std::vector<double> levelWeight = std::vector<double>(LevelSlots, 1.0);  // rotation weight per level
    uint32 population = 0;
    bool fromRoster = false;
    uint32 nextUpdate = 0;
    uint32 lastUpdate = 0;
    uint64 updates = 0;
};

Balance& GetBalance()
{
    static Balance balance;
    return balance;
}

uint32 BalanceIndex(PopulationPolicy::BalanceSettings const& settings, size_t ranges, uint32 level)
{
    uint32 const index = PopulationPolicy::BalanceRangeOf(level, settings);
    return ranges ? std::min<uint32>(index, static_cast<uint32>(ranges - 1)) : 0;
}

// The level of any character, online or not, from the character cache (kept current on level-up); 0 = unknown
uint8 CachedLevel(uint32 guid)
{
    return sCharacterCache->GetCharacterLevelByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
}

// The ranges' XP multipliers (XP hook) and online weights (rotation) per level; all 1 while off
void PublishBalance(Balance& balance, bool on)
{
    for (size_t level = 0; level < LevelSlots; ++level)
    {
        PopulationPolicy::BalanceRange const* range =
            on && !balance.ranges.empty()
                ? &balance.ranges[BalanceIndex(balance.settings, balance.ranges.size(), static_cast<uint32>(level))]
                : nullptr;
        s_levelXP[level].store(range ? static_cast<float>(range->multiplier) : 1.0f, std::memory_order_relaxed);
        balance.levelWeight[level] = range ? range->weight : 1.0;
    }
}

// The rotation's share weight of a roster bot (its level range's); 1 while the balance is off
double BalanceWeight(uint32 guid)
{
    if (!s_balance.load(std::memory_order_relaxed))
        return 1.0;

    return GetBalance().levelWeight[CachedLevel(guid)];
}

std::string RangeName(PopulationPolicy::BalanceRange const& range)
{
    return range.firstLevel == range.lastLevel ? std::to_string(range.firstLevel)
                                               : Acore::StringFormat("{}-{}", range.firstLevel, range.lastLevel);
}

void LoadBalanceConfig(bool reload)
{
    Balance& balance = GetBalance();
    PopulationPolicy::BalanceSettings settings;
    auto const get = [](char const* key, uint32 fallback)
    { return sConfigMgr->GetOption<uint32>(std::string("Playerbots.Population.Balance.") + key, fallback); };
    settings.rangeSize = std::max<uint32>(1, get("RangeSize", 10));
    settings.maxLevel = std::max<uint32>(2, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
    settings.maxLevelWeightPct = get("MaxLevelWeightPct", 200);
    settings.minMultiplierPct = std::clamp<uint32>(get("MinMultiplierPct", 50), 1, 100);
    settings.maxMultiplierPct = std::max<uint32>(100, get("MaxMultiplierPct", 300));
    settings.deadbandPct = get("DeadbandPct", 10);
    settings.gainPct = get("GainPct", 50);
    settings.responseSeconds = 3600 * std::max<uint32>(1, get("ResponseHours", 384));
    float const smoothingHours = sConfigMgr->GetOption<float>("Playerbots.Population.Balance.SmoothingHours", 6.0f);
    settings.smoothingSeconds = static_cast<uint32>(std::clamp(smoothingHours * 3600.0f, 1.0f, 8640000.0f));
    settings.preferencePct = std::min<uint32>(90, get("LogoutPreferencePct", 25));
    balance.intervalSeconds = 60 * std::max<uint32>(1, get("IntervalMinutes", 10));

    std::vector<PopulationPolicy::BalanceRange> layout = PopulationPolicy::BalanceLayout(settings);
    if (!PopulationPolicy::BalanceSameLayout(layout, balance.ranges))
    {
        balance.ranges = std::move(layout);
        balance.updates = 0;
        balance.lastUpdate = 0;
    }

    balance.settings = settings;
    balance.nextUpdate = 0;
    bool const on = sConfigMgr->GetOption<bool>(BalanceEnableKey, false);
    if (!on)
        balance.lastUpdate = 0;  // turned on again later: no integral step over the time it was off

    PublishBalance(balance, on);
    s_balance.store(on, std::memory_order_release);
    if (!on)
    {
        LOG_INFO("playerbots", "[Population] {}{} = 0", reload ? "reload: " : "", BalanceEnableKey);
        return;
    }

    LOG_INFO("playerbots",
             "[Population] {}{} = 1: {} ranges of {} levels up to {} (max level at {} % of a range), every {} min, "
             "XP multiplier {}-{} %, deadband {} %, gain {} %, response {} h, smoothing {} s, logout preference {} %",
             reload ? "reload: " : "", BalanceEnableKey, balance.ranges.size(),
             PopulationPolicy::BalanceRangeSize(settings), settings.maxLevel, settings.maxLevelWeightPct,
             balance.intervalSeconds / 60, settings.minMultiplierPct, settings.maxMultiplierPct, settings.deadbandPct,
             settings.gainPct, settings.responseSeconds / 3600, settings.smoothingSeconds, settings.preferencePct);
    if (!sConfigMgr->GetOption<bool>("Playerbots.Population.Persistent", false) &&
        sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.Enabled", false))
        LOG_WARN("playerbots",
                 "[Population] Playerbots.LevelBrackets.Enabled = 1 also moves bots between level ranges (by setting "
                 "their levels); the two work against each other. Set one of them to 0.");
    if (!sConfigMgr->GetOption<bool>("Playerbots.Population.Rotation.Enable", false))
        LOG_INFO("playerbots",
                 "[Population] balance: the rotation is off, so ranges count the online random bots and "
                 "the logout preference does nothing");
}

void PrintBalance(ChatHandler* handler, uint32 now)
{
    Balance const& balance = GetBalance();
    PopulationPolicy::BalanceSettings const& settings = balance.settings;
    handler->PSendSysMessage(
        "Balance: {}; every {} min, {} ranges of {} levels, max level {} at {} % of a range, XP multiplier {}-{} %, "
        "deadband {} %, gain {} %, response {} h, logout preference {} %",
        Population::IsBalance() ? "on" : "off", balance.intervalSeconds / 60, balance.ranges.size(),
        PopulationPolicy::BalanceRangeSize(settings), settings.maxLevel, settings.maxLevelWeightPct,
        settings.minMultiplierPct, settings.maxMultiplierPct, settings.deadbandPct, settings.gainPct,
        settings.responseSeconds / 3600, settings.preferencePct);
    if (!Population::IsBalance())
        return;

    if (!balance.updates)
    {
        handler->PSendSysMessage("No balance update yet");
        return;
    }

    handler->PSendSysMessage("Last balance update {} s ago ({} in all): {} bots counted ({})", now - balance.lastUpdate,
                             balance.updates, balance.population,
                             balance.fromRoster ? "the rotation roster" : "the online random bots");
    for (PopulationPolicy::BalanceRange const& range : balance.ranges)
        handler->PSendSysMessage(
            "  levels {}: {} / target {:.0f} ({:+.0f} %), XP x{:.2f}, online weight {:.2f}{}", RangeName(range),
            range.count, range.target, range.target > 0.0 ? 100.0 * (range.count - range.target) / range.target : 0.0,
            range.multiplier, range.weight,
            range.overSince ? Acore::StringFormat(", over target for {} s", now - range.overSince) : std::string());
}

// H4a: retiring never-met random bots (Playerbots.Population.Retire.*)
std::atomic<bool> s_retire{false};

constexpr char const* RetireEnableKey = "Playerbots.Population.Retire.Enable";

// The rows of a retired bot that Player::DeleteFromDB leaves behind: one DELETE each, in one transaction per DB queued
// before the core's delete (one async worker per DB, so FIFO: item_soulbound_trade_data still finds the bot's items).
// A table or column the DB lacks (mod-ollama-chat not installed, an older schema) is left out. The hub's
// tools/population-inventory.js reads both lists (one entry per line) and checks them against the real schemas.
struct RetireRow
{
    char const* table;
    char const* column;
    bool byItem;  // the column holds item guids: those of the bot's items (item_instance.owner_guid)
};

// clang-format off
constexpr RetireRow RetireCharacterRows[] = {
    {"character_stats", "guid", false},
    {"character_arena_stats", "guid", false},
    {"character_banned", "guid", false},
    {"character_battleground_random", "guid", false},
    {"character_brew_of_the_month", "guid", false},
    {"character_achievement_offline_updates", "guid", false},
    {"battleground_deserters", "guid", false},
    {"lfg_data", "guid", false},
    {"mail_server_character", "guid", false},
    {"guild_member_withdraw", "guid", false},
    {"channels_bans", "playerGUID", false},
    {"recovery_item", "Guid", false},
    {"pvpstats_players", "character_guid", false},
    {"quest_tracker", "character_guid", false},
    {"item_refund_instance", "player_guid", false},
    {"item_soulbound_trade_data", "itemGuid", true},
    {"mod_ollama_chat_personality", "guid", false},
    {"mod_ollama_chat_bot_player_sentiments", "bot_guid", false},
    {"mod_ollama_chat_bot_player_sentiments", "player_guid", false},
    {"mod_ollama_chat_history", "bot_guid", false},
    {"mod_ollama_chat_history", "player_guid", false},
    {"mod_ollama_chat_memories", "bot_guid", false},
    {"mod_ollama_chat_relationships", "bot_guid", false},
    {"mod_ollama_chat_relationships", "other_guid", false},
};

constexpr RetireRow RetirePlayerbotsRows[] = {
    {"playerbots_random_bots", "bot", false},
    {"playerbots_db_store", "guid", false},
    {"playerbots_bot_profile", "guid", false},
    {"playerbots_acquaintance", "bot_guid", false},
    {"playerbots_pvp_loadout", "guid", false},
    {"playerbots_pvp_worn", "guid", false},
    {"playerbots_ladder_roster", "guid", false},
    {"playerbots_preferred_mounts", "guid", false},
    {"playerbots_guild_tasks", "owner", false},
    {"playerbots_custom_strategy", "owner", false},
};
// clang-format on

struct RetireRecord
{
    std::string name;
    uint32 guid;
    uint32 level;
    uint32 at;
    bool dryRun;
};

// One pass's candidate batch and the protections its queries found
struct RetirePass
{
    std::vector<uint32> batch;
    std::unordered_map<uint32, uint32> protect;
    uint32 seed = 0;
    bool failed = false;
};

// World thread only
struct Retire
{
    bool enable = false;
    bool dryRun = true;
    uint32 intervalSeconds = 600;
    uint32 maxPerPass = 1;
    uint32 maxPerHour = 10;
    uint32 maxScore = 0;
    uint32 persistSeconds = 3600;
    uint32 candidates = 50;
    bool testRange = false;
    uint32 testFirst = 0;
    uint32 testLast = 0;
    uint32 testTarget = 0;
    std::string refusal;  // why the switched-on retire does not run; empty: it runs
    uint32 nextPass = 0;
    bool busy = false;                                 // the preparation's or a pass's queries are in flight
    bool prepared = false;                             // the schemas' columns and the clone accounts are loaded
    std::unordered_set<std::string> characterColumns;  // "table.column"
    std::unordered_set<std::string> playerbotsColumns;
    std::unordered_set<uint32> cloneAccounts;  // lab-bots clone accounts (email LAB-BOTS ...): never touched
    std::unordered_set<uint32> refusedLogged;  // known bots whose refusal was logged (once per bot)
    std::vector<uint32> recent;                // game times of this hour's retires (dry-run picks too)
    std::deque<RetireRecord> last;
    std::unordered_set<uint32> const* online = nullptr;  // the manager's online set while the callbacks run
    uint32 seed = 0;
    uint32 lastPass = 0;
    uint32 lastInRange = 0;
    uint32 characterRows = 0;
    uint32 playerbotsRows = 0;
    uint64 passes = 0;
    uint64 retired = 0;
    uint64 wouldRetire = 0;
    uint64 refused = 0;
    QueryCallbackProcessor queries;
};

Retire& GetRetire()
{
    static Retire retire;
    return retire;
}

std::string JoinIds(std::vector<uint32> const& ids)
{
    std::string text;
    for (uint32 const id : ids)
        text += (text.empty() ? "" : ",") + std::to_string(id);

    return text;
}

std::string ColumnKey(char const* table, char const* column) { return std::string(table) + "." + column; }

// The protections the world thread knows without a query: online, away, a clone account, and the character cache's
// arena teams and group (the pass's query checks those again in the DB)
uint32 MemoryProtect(Retire const& retire, uint32 guid)
{
    ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(guid);
    uint32 protect = 0;
    if ((retire.online && retire.online->contains(guid)) || ObjectAccessor::FindConnectedPlayer(playerGuid) ||
        sRandomPlayerbotMgr.GetPlayerBot(playerGuid))
        protect |= PopulationPolicy::RetireOnline;

    if (LadderMgr::Instance().IsAway(guid))
        protect |= PopulationPolicy::RetireAway;

    CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(playerGuid);
    if (!entry || !entry->AccountId || retire.cloneAccounts.contains(entry->AccountId))
        return protect | PopulationPolicy::RetireClone;

    for (uint32 const teamId : entry->ArenaTeamId)
        if (teamId)
            protect |= PopulationPolicy::RetireArenaTeam;

    if (!entry->GroupGuid.IsEmpty())
        protect |= PopulationPolicy::RetireGrouped;

    return protect;
}

PopulationPolicy::RetireCandidate RetireCandidateOf(Retire const& retire, uint32 guid)
{
    PopulationPolicy::RetireCandidate candidate;
    candidate.guid = guid;
    candidate.level = CachedLevel(guid);
    candidate.score = AcquaintanceMgr::Instance().MaxScoreOfBot(guid);
    candidate.protect = MemoryProtect(retire, guid);
    return candidate;
}

int32 RetireRangeIndex(std::vector<PopulationPolicy::RetireRange> const& ranges, uint32 level)
{
    for (size_t i = 0; i < ranges.size(); ++i)
        if (level >= ranges[i].firstLevel && level <= ranges[i].lastLevel)
            return static_cast<int32>(i);

    return -1;
}

// The ranges a pass may retire from with their roster counts now: Retire.TestRange with Retire.TestTarget, else H3's
// ranges over target for Retire.PersistMinutes (never the level-1 range new bots start in); only those with a surplus
std::vector<PopulationPolicy::RetireRange> RetireRanges(Retire const& retire, uint32 now)
{
    std::vector<PopulationPolicy::RetireRange> ranges;
    if (retire.testRange)
        ranges.push_back({retire.testFirst, retire.testLast, 0, static_cast<double>(retire.testTarget)});
    else
        for (PopulationPolicy::BalanceRange const& range : Population::BalanceRanges())
            if (PopulationPolicy::RetireRangeDue(range.overSince, now, retire.persistSeconds) &&
                PopulationPolicy::RetireRangeAllowed(range.firstLevel, range.lastLevel))
                ranges.push_back({range.firstLevel, range.lastLevel, 0, range.target});

    if (ranges.empty())
        return ranges;

    for (RosterBot const& bot : GetRotation().roster)
    {
        int32 const index = RetireRangeIndex(ranges, CachedLevel(bot.guid));
        if (index >= 0)
            ++ranges[index].count;
    }

    ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [](PopulationPolicy::RetireRange const& range)
                                { return !PopulationPolicy::RetireSurplus(range.count, range.target); }),
                 ranges.end());
    return ranges;
}

void LogRetireRefusal(Retire& retire, PopulationPolicy::RetireCandidate const& candidate)
{
    if (!retire.refusedLogged.insert(candidate.guid).second)
        return;

    ++retire.refused;
    LOG_INFO("playerbots", "[Population] retire refused: {} (#{}, level {}): {}", BotName(candidate.guid),
             candidate.guid, candidate.level,
             PopulationPolicy::RetireReasons(candidate.protect, candidate.score, retire.maxScore));
}

// Deletes one bot (or, with DryRun, only logs it). The caller checked it a moment ago on this thread: offline, in an
// over-target range, never met, unprotected.
void RetireBot(Retire& retire, uint32 guid, uint32 score, PopulationPolicy::RetireRange const& range, uint32 now)
{
    ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(guid);
    CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(playerGuid);
    if (!entry)
        return;

    std::string const name = entry->Name;
    uint32 const account = entry->AccountId;
    uint32 const level = entry->Level;
    uint32 const classId = entry->Class;
    retire.recent.push_back(now);
    retire.last.push_front({name, guid, level, now, retire.dryRun});
    if (retire.last.size() > 5)
        retire.last.pop_back();

    if (retire.dryRun)
    {
        ++retire.wouldRetire;
        LOG_INFO("playerbots",
                 "[Population] would retire: {} (#{}, level {}, class {}, account {}, score {}) range {}-{} count {} "
                 "target {:.0f} (Retire.DryRun = 1: nothing deleted)",
                 name, guid, level, classId, account, score, range.firstLevel, range.lastLevel, range.count,
                 range.target);
        return;
    }

    // Memory first: nothing may write the bot's rows back after the delete
    sRandomPlayerbotMgr.ForgetBot(guid);
    BotProfileMgr::instance().Forget(guid);
    AcquaintanceMgr::Instance().ForgetBot(guid);
    PvpGearMgr::Instance().ForgetBot(guid);
    Population::InvalidateRoster(guid);

    CharacterDatabaseTransaction characters = CharacterDatabase.BeginTransaction();
    bool anyCharacterRow = false;
    for (RetireRow const& row : RetireCharacterRows)
    {
        if (!retire.characterColumns.contains(ColumnKey(row.table, row.column)))
            continue;

        characters->Append(row.byItem
                               ? Acore::StringFormat("DELETE FROM {} WHERE {} IN (SELECT guid FROM "
                                                     "item_instance WHERE owner_guid = {})",
                                                     row.table, row.column, guid)
                               : Acore::StringFormat("DELETE FROM {} WHERE {} = {}", row.table, row.column, guid));
        anyCharacterRow = true;
    }

    if (anyCharacterRow)
        CharacterDatabase.CommitTransaction(characters);

    PlayerbotsDatabaseTransaction playerbots = PlayerbotsDatabase.BeginTransaction();
    bool anyPlayerbotsRow = false;
    for (RetireRow const& row : RetirePlayerbotsRows)
    {
        if (!retire.playerbotsColumns.contains(ColumnKey(row.table, row.column)))
            continue;

        playerbots->Append(Acore::StringFormat("DELETE FROM {} WHERE {} = {}", row.table, row.column, guid));
        anyPlayerbotsRow = true;
    }

    if (anyPlayerbotsRow)
        PlayerbotsDatabase.CommitTransaction(playerbots);

    // The core's delete (CHAR_DELETE_REMOVE): the character, its items, pets, mail, social lists, achievements, quests,
    // guild membership and the character cache entry; the account stays (H4b reuses its free slot). It runs a few
    // synchronous mail queries on this thread, hence the per-pass and per-hour caps.
    Player::DeleteFromDB(guid, account, true, true);
    ++retire.retired;
    LOG_INFO("playerbots",
             "[Population] retire: {} (#{}, level {}, class {}, account {}, score {}) range {}-{} count {} target "
             "{:.0f}",
             name, guid, level, classId, account, score, range.firstLevel, range.lastLevel, range.count, range.target);
}

// The pass's second half (world thread, from the query callbacks): checks the batch again with the queries'
// protections and fresh levels and counts (the world moved on while the queries ran), then retires
void FinishRetirePass(RetirePass const& pass)
{
    Retire& retire = GetRetire();
    retire.busy = false;
    if (!s_retire.load(std::memory_order_relaxed))
        return;

    if (pass.failed)
    {
        LOG_ERROR("playerbots", "[Population] retire pass: a protection query failed; nothing retired");
        return;
    }

    uint32 const now = Now();
    std::vector<PopulationPolicy::RetireRange> ranges = RetireRanges(retire, now);
    std::vector<PopulationPolicy::RetireCandidate> candidates;
    uint32 guarded = 0;
    for (uint32 const guid : pass.batch)
    {
        PopulationPolicy::RetireCandidate candidate = RetireCandidateOf(retire, guid);
        auto const it = pass.protect.find(guid);
        if (it != pass.protect.end())
            candidate.protect |= it->second;

        // Bots the user knows through a guild or a friend list (DH.2): logged once each
        if (candidate.protect & (PopulationPolicy::RetireGuildWithReal | PopulationPolicy::RetireFriendOfReal))
            LogRetireRefusal(retire, candidate);

        if (candidate.protect || candidate.score > retire.maxScore)
            ++guarded;

        candidates.push_back(candidate);
    }

    uint32 const left = PopulationPolicy::RetireHourLeft(retire.recent, now, retire.maxPerHour);
    std::vector<uint32> const chosen = PopulationPolicy::SelectRetirees(candidates, ranges, retire.maxScore,
                                                                        std::min(left, retire.maxPerPass), pass.seed);
    LOG_DEBUG("playerbots", "[Population] retire pass: {} bots in range, batch {}, {} protected, {} chosen{}",
              retire.lastInRange, pass.batch.size(), guarded, chosen.size(), retire.dryRun ? " (dry run)" : "");
    for (uint32 const guid : chosen)
    {
        auto const candidate =
            std::find_if(candidates.begin(), candidates.end(),
                         [guid](PopulationPolicy::RetireCandidate const& entry) { return entry.guid == guid; });
        int32 const index = RetireRangeIndex(ranges, candidate->level);
        if (index < 0)
            continue;

        RetireBot(retire, guid, candidate->score, ranges[index], now);
        --ranges[index].count;
    }
}

// A pass's first half: the bots of the over-target ranges, filtered in memory and ordered by the rule, and one query
// per DB for the protections the world thread does not hold (guild or friend of a real player, arena team, guild
// leader, group, ladder roster). Every query returns a sentinel row, so an empty result means a failed query.
void StartRetirePass(Retire& retire, uint32 now)
{
    Rotation const& rotation = GetRotation();
    std::vector<uint32> const& accounts = sPlayerbotAIConfig.RandomBotAccounts;
    if (rotation.roster.empty() || accounts.empty() || !AcquaintanceMgr::Instance().IsLoaded())
    {
        LOG_DEBUG("playerbots", "[Population] retire pass skipped: roster {}, random bot accounts {}, acquaintances {}",
                  rotation.roster.size(), accounts.size(),
                  AcquaintanceMgr::Instance().IsLoaded() ? "loaded" : "not loaded");
        return;
    }

    if (!retire.maxPerPass || !PopulationPolicy::RetireHourLeft(retire.recent, now, retire.maxPerHour))
        return;

    std::vector<PopulationPolicy::RetireRange> const ranges = RetireRanges(retire, now);
    if (ranges.empty())
        return;

    ++retire.passes;
    retire.lastPass = now;
    uint32 const seed = retire.seed ^ static_cast<uint32>(retire.passes * 2654435761ull);
    std::vector<PopulationPolicy::RetireCandidate> pool;
    for (RosterBot const& bot : rotation.roster)
    {
        if (RetireRangeIndex(ranges, CachedLevel(bot.guid)) < 0)
            continue;

        PopulationPolicy::RetireCandidate const candidate = RetireCandidateOf(retire, bot.guid);
        // Acquainted bots (DH.2): logged once each
        if (candidate.score > retire.maxScore && !(candidate.protect & PopulationPolicy::RetireClone))
            LogRetireRefusal(retire, candidate);

        pool.push_back(candidate);
    }

    retire.lastInRange = static_cast<uint32>(pool.size());
    auto pass = std::make_shared<RetirePass>();
    pass->batch = PopulationPolicy::SelectRetirees(pool, ranges, retire.maxScore, retire.candidates, seed, false);
    pass->seed = seed;
    if (pass->batch.empty())
    {
        LOG_DEBUG("playerbots", "[Population] retire pass: {} bots in range, none unprotected", pool.size());
        return;
    }

    std::string const ids = JoinIds(pass->batch);
    std::string const bots = JoinIds(accounts);
    // A real player's character: on an account that is no random bot account
    std::string const characterSql = Acore::StringFormat(
        "SELECT gm.guid, {} FROM guild_member gm JOIN guild_member o ON o.guildid = gm.guildid "
        "JOIN characters c ON c.guid = o.guid WHERE gm.guid IN ({}) AND c.account NOT IN ({}) "
        "UNION ALL SELECT s.friend, {} FROM character_social s JOIN characters c ON c.guid = s.guid "
        "WHERE s.friend IN ({}) AND (s.flags & 1) <> 0 AND c.account NOT IN ({}) "
        "UNION ALL SELECT guid, {} FROM arena_team_member WHERE guid IN ({}) "
        "UNION ALL SELECT leaderguid, {} FROM guild WHERE leaderguid IN ({}) "
        "UNION ALL SELECT memberGuid, {} FROM group_member WHERE memberGuid IN ({}) "
        "UNION ALL SELECT 0, 0",
        uint32(PopulationPolicy::RetireGuildWithReal), ids, bots, uint32(PopulationPolicy::RetireFriendOfReal), ids,
        bots, uint32(PopulationPolicy::RetireArenaTeam), ids, uint32(PopulationPolicy::RetireGuildLeader), ids,
        uint32(PopulationPolicy::RetireGrouped), ids);
    std::string const ladderSql =
        retire.playerbotsColumns.contains("playerbots_ladder_roster.guid")
            ? Acore::StringFormat("SELECT guid FROM playerbots_ladder_roster WHERE guid IN ({}) UNION ALL SELECT 0",
                                  ids)
            : std::string("SELECT 0");
    retire.busy = true;
    retire.queries.AddCallback(CharacterDatabase.AsyncQuery(characterSql)
                                   .WithChainingCallback(
                                       [pass, ladderSql](QueryCallback& callback, QueryResult result)
                                       {
                                           if (!result)
                                               pass->failed = true;
                                           else
                                               do
                                               {
                                                   Field* fields = result->Fetch();
                                                   pass->protect[fields[0].Get<uint32>()] |= fields[1].Get<uint32>();
                                               } while (result->NextRow());

                                           callback.SetNextQuery(PlayerbotsDatabase.AsyncQuery(ladderSql));
                                       })
                                   .WithCallback(
                                       [pass](QueryResult result)
                                       {
                                           if (!result)
                                               pass->failed = true;
                                           else
                                               do
                                               {
                                                   pass->protect[result->Fetch()[0].Get<uint32>()] |=
                                                       PopulationPolicy::RetireLadderRoster;
                                               } while (result->NextRow());

                                           FinishRetirePass(*pass);
                                       }));
}

bool ReadColumns(QueryResult const& result, std::unordered_set<std::string>& columns)
{
    if (!result)
        return false;

    do
    {
        Field* fields = result->Fetch();
        columns.insert(fields[0].Get<std::string>() + "." + fields[1].Get<std::string>());
    } while (result->NextRow());

    return true;
}

// Once per switch-on and reload, before the first pass: which of the rows the DBs have, and the lab clone accounts
void StartRetirePrepare(Retire& retire)
{
    struct Prepared
    {
        std::unordered_set<std::string> characters;
        std::unordered_set<std::string> playerbots;
        bool failed = false;
    };

    auto prepared = std::make_shared<Prepared>();
    std::string const columnsSql =
        "SELECT TABLE_NAME, COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()";
    retire.busy = true;
    retire.queries.AddCallback(
        CharacterDatabase.AsyncQuery(columnsSql)
            .WithChainingCallback(
                [prepared, columnsSql](QueryCallback& callback, QueryResult result)
                {
                    prepared->failed |= !ReadColumns(result, prepared->characters);
                    callback.SetNextQuery(PlayerbotsDatabase.AsyncQuery(columnsSql));
                })
            .WithChainingCallback(
                [prepared](QueryCallback& callback, QueryResult result)
                {
                    prepared->failed |= !ReadColumns(result, prepared->playerbots);
                    callback.SetNextQuery(LoginDatabase.AsyncQuery(
                        "SELECT 0 UNION ALL SELECT id FROM account WHERE email LIKE 'LAB-BOTS%'"));
                })
            .WithCallback(
                [prepared](QueryResult result)
                {
                    Retire& retire = GetRetire();
                    retire.busy = false;
                    if (prepared->failed || !result)
                    {
                        LOG_ERROR("playerbots",
                                  "[Population] retire: the schema or clone account query failed; "
                                  "nothing is retired until it succeeds (next interval)");
                        return;
                    }

                    retire.characterColumns = std::move(prepared->characters);
                    retire.playerbotsColumns = std::move(prepared->playerbots);
                    retire.cloneAccounts.clear();
                    do
                    {
                        if (uint32 const id = result->Fetch()[0].Get<uint32>())
                            retire.cloneAccounts.insert(id);
                    } while (result->NextRow());

                    std::string missing;
                    retire.characterRows = 0;
                    retire.playerbotsRows = 0;
                    for (RetireRow const& row : RetireCharacterRows)
                        if (retire.characterColumns.contains(ColumnKey(row.table, row.column)))
                            ++retire.characterRows;
                        else
                            missing += " " + ColumnKey(row.table, row.column);

                    for (RetireRow const& row : RetirePlayerbotsRows)
                        if (retire.playerbotsColumns.contains(ColumnKey(row.table, row.column)))
                            ++retire.playerbotsRows;
                        else
                            missing += " " + ColumnKey(row.table, row.column);

                    retire.prepared = true;
                    LOG_INFO(
                        "playerbots",
                        "[Population] retire: {} characters and {} playerbots rows cleared per retired bot besides "
                        "the core's delete; {} lab clone accounts never touched{}{}",
                        retire.characterRows, retire.playerbotsRows, retire.cloneAccounts.size(),
                        missing.empty() ? "" : "; not in the DB:", missing);
                }));
}

void LoadRetireConfig(bool reload, bool persistent)
{
    Retire& retire = GetRetire();
    bool const wasOn = s_retire.load(std::memory_order_relaxed);
    bool const wasLive = wasOn && !retire.dryRun;
    retire.enable = sConfigMgr->GetOption<bool>(RetireEnableKey, false);
    retire.dryRun = sConfigMgr->GetOption<bool>("Playerbots.Population.Retire.DryRun", true);
    retire.intervalSeconds =
        std::max<uint32>(10, sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.IntervalSeconds", 600));
    retire.maxPerPass = sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.MaxPerPass", 1);
    retire.maxPerHour = sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.MaxPerHour", 10);
    retire.maxScore = sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.MaxScore", 0);
    retire.persistSeconds = 60 * sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.PersistMinutes", 60);
    retire.candidates =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.Candidates", 50), 1, 500);
    std::string const testRange = sConfigMgr->GetOption<std::string>("Playerbots.Population.Retire.TestRange", "");
    retire.testRange =
        !testRange.empty() && PopulationPolicy::ParseRetireTestRange(testRange, retire.testFirst, retire.testLast);
    retire.testTarget = sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.TestTarget", 0);
    if (!testRange.empty() && !retire.testRange)
        LOG_WARN("playerbots",
                 "[Population] Playerbots.Population.Retire.TestRange = \"{}\" is not first-last; ignored", testRange);

    retire.refusal.clear();
    if (retire.enable)
    {
        if (!persistent)
            retire.refusal = "Playerbots.Population.Persistent = 0 (the new bots would be re-levelled)";
        else if (!sConfigMgr->GetOption<bool>("Playerbots.Acquaintance.Enable", false))
            retire.refusal = "Playerbots.Acquaintance.Enable = 0 (every bot would look never met)";
        else if (!sConfigMgr->GetOption<bool>(RotationEnableKey, false))
            retire.refusal = "Playerbots.Population.Rotation.Enable = 0 (no roster to keep)";
        else if (!retire.testRange && !sConfigMgr->GetOption<bool>(BalanceEnableKey, false))
            retire.refusal = "Playerbots.Population.Balance.Enable = 0 (no range targets)";
    }

    bool const on = retire.enable && retire.refusal.empty();
    // The live safeguard: the first pass after switching on, or from the dry run to live, waits one interval
    if (on && (!wasOn || (!wasLive && !retire.dryRun)))
        retire.nextPass = Now() + retire.intervalSeconds;

    retire.prepared = false;
    retire.refusedLogged.clear();
    if (!reload)
        retire.seed = static_cast<uint32>(time(nullptr));

    s_retire.store(on, std::memory_order_relaxed);
    if (!retire.enable)
    {
        LOG_INFO("playerbots", "[Population] {}{} = 0", reload ? "reload: " : "", RetireEnableKey);
        return;
    }

    if (!on)
    {
        LOG_ERROR("playerbots", "[Population] {}{} = 1 refused: {}; nothing is retired", reload ? "reload: " : "",
                  RetireEnableKey, retire.refusal);
        return;
    }

    LOG_INFO("playerbots",
             "[Population] {}{} = 1{}: every {} s, at most {} per pass and {} per hour, max score {}, ranges over "
             "target for {} min, batch {}{}",
             reload ? "reload: " : "", RetireEnableKey,
             retire.dryRun ? " (DryRun = 1: logs only)" : " (LIVE: deletes characters)", retire.intervalSeconds,
             retire.maxPerPass, retire.maxPerHour, retire.maxScore, retire.persistSeconds / 60, retire.candidates,
             retire.testRange ? Acore::StringFormat(", test range {}-{} target {}", retire.testFirst, retire.testLast,
                                                    retire.testTarget)
                              : std::string());
}

void PrintRetire(ChatHandler* handler, uint32 now)
{
    Retire& retire = GetRetire();
    if (!retire.enable)
    {
        handler->PSendSysMessage("Retire: off");
        return;
    }

    if (!retire.refusal.empty())
    {
        handler->PSendSysMessage("Retire: refused ({})", retire.refusal);
        return;
    }

    uint32 const left = PopulationPolicy::RetireHourLeft(retire.recent, now, retire.maxPerHour);
    handler->PSendSysMessage(
        "Retire: on{}; every {} s, at most {} per pass and {} per hour ({} left), max score {}, persist {} min, "
        "batch {}{}",
        retire.dryRun ? " (DRY RUN: nothing deleted)" : "", retire.intervalSeconds, retire.maxPerPass,
        retire.maxPerHour, left, retire.maxScore, retire.persistSeconds / 60, retire.candidates,
        retire.testRange
            ? Acore::StringFormat(", test range {}-{} target {}", retire.testFirst, retire.testLast, retire.testTarget)
            : std::string());
    handler->PSendSysMessage(
        "Retire totals: passes {}, retired {}, would retire {}, known bots refused {}; last pass {}, {} bots in range; "
        "{} + {} rows per bot ({}), {} clone accounts",
        retire.passes, retire.retired, retire.wouldRetire, retire.refused,
        retire.lastPass ? Acore::StringFormat("{} s ago", now - retire.lastPass) : std::string("none"),
        retire.lastInRange, retire.characterRows, retire.playerbotsRows, retire.prepared ? "prepared" : "not prepared",
        retire.cloneAccounts.size());
    for (RetireRecord const& record : retire.last)
        handler->PSendSysMessage("  {} {} (#{}), level {}, {} s ago", record.dryRun ? "would retire" : "retired",
                                 record.name, record.guid, record.level, now - record.at);
}
}  // namespace

bool Population::IsPersistent() { return s_persistent.load(std::memory_order_relaxed); }

bool Population::IsRotation() { return s_rotation.load(std::memory_order_relaxed); }

void Population::LoadConfig(bool reload)
{
    bool const persistent = sConfigMgr->GetOption<bool>("Playerbots.Population.Persistent", false);
    s_persistent.store(persistent, std::memory_order_relaxed);

    LOG_INFO("playerbots", "[Population] {}Playerbots.Population.Persistent = {}{}", reload ? "reload: " : "",
             persistent ? 1 : 0, persistent ? " (bot levels never go down)" : "");

    // Read from sConfigMgr, not PlayerbotAIConfig: on ".reload config" the level-management values there are
    // reloaded by another script whose order against this one is not fixed.
    std::vector<char const*> const refused = PopulationPolicy::RefusedOptions(
        persistent, sConfigMgr->GetOption<bool>("Playerbots.DowngradeMaxLevelBot", false),
        sConfigMgr->GetOption<bool>("Playerbots.ResetBotLevel.Enabled", false),
        sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.Enabled", false));
    for (char const* option : refused)
        LOG_WARN("playerbots",
                 "[Population] {} = 1 is refused while Playerbots.Population.Persistent = 1: "
                 "it would lower bot levels; set it to 0",
                 option);

    LoadBalanceConfig(reload);
    LoadRetireConfig(reload, persistent);

    Rotation& rotation = GetRotation();
    PopulationPolicy::RotationSettings& settings = rotation.settings;
    bool const rotationOn = sConfigMgr->GetOption<bool>(RotationEnableKey, false);
    settings.intervalSeconds =
        std::max<uint32>(10, sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.IntervalSeconds", 300));
    settings.meanSessionSeconds =
        60 *
        std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.MeanSessionMinutes", 120));
    settings.minSessionSeconds =
        std::min<uint32>(settings.meanSessionSeconds,
                         60 * sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.MinSessionMinutes", 30));
    settings.acquaintedSharePct =
        std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.AcquaintedSharePct", 90));
    rotation.rosterSize = sConfigMgr->GetOption<uint32>(RosterSizeKey, DefaultRosterSize);
    rotation.acquaintedMinScore =
        std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.AcquaintedMinScore", 1));
    rotation.rosterDirty = true;
    rotation.nextTick = 0;
    if (!reload)
        rotation.schedule.SetSeed(static_cast<uint32>(time(nullptr)));

    s_rotation.store(rotationOn, std::memory_order_relaxed);
    if (!rotationOn)
    {
        LOG_INFO("playerbots", "[Population] {}{} = 0", reload ? "reload: " : "", RotationEnableKey);
        return;
    }

    LOG_INFO("playerbots",
             "[Population] {}{} = 1: roster {} (0 = every random bot character), interval {} s, mean session {} min, "
             "min session {} min, acquainted share {} % (score >= {})",
             reload ? "reload: " : "", RotationEnableKey, rotation.rosterSize, settings.intervalSeconds,
             settings.meanSessionSeconds / 60, settings.minSessionSeconds / 60, settings.acquaintedSharePct,
             rotation.acquaintedMinScore);
    if (sConfigMgr->GetOption<bool>("Playerbots.EnablePeriodicOnlineOffline", false))
        LOG_WARN("playerbots",
                 "[Population] Playerbots.EnablePeriodicOnlineOffline is superseded while {} = 1: the rotation picks "
                 "the bots from the whole roster and keeps each online until it swaps the bot out",
                 RotationEnableKey);
}

uint32 Population::GuardLevel(Player* bot, uint32 requestedLevel, char const* site)
{
    if (!bot || !IsPersistent())
        return requestedLevel;

    uint32 const currentLevel = bot->GetLevel();
    if (PopulationPolicy::KeepLevel(true, currentLevel, requestedLevel) == requestedLevel)
        return requestedLevel;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return requestedLevel;

    LOG_WARN("playerbots", "[Population] {}: refused to lower {} from level {} to {}, kept {}", site, bot->GetName(),
             currentLevel, requestedLevel, currentLevel);

    return currentLevel;
}

uint32 Population::RosterBotCount(uint32 upstreamCount)
{
    if (!sConfigMgr->GetOption<bool>(RotationEnableKey, false))
        return upstreamCount;

    return std::max(upstreamCount, sConfigMgr->GetOption<uint32>(RosterSizeKey, DefaultRosterSize));
}

std::vector<PopulationPolicy::RotationSwap> Population::RotationTick(std::vector<uint32> const& accounts,
                                                                     std::unordered_set<uint32> const& online,
                                                                     uint32 cap)
{
    std::vector<PopulationPolicy::RotationSwap> swaps;
    if (!IsRotation())
        return swaps;

    PLAYERBOT_CHECK_WORLD_THREAD("Population::RotationTick");
    Rotation& rotation = GetRotation();
    uint32 const now = Now();
    if (now < rotation.nextTick)
        return swaps;

    rotation.nextTick = now + rotation.settings.intervalSeconds;
    if (rotation.rosterDirty || rotation.rosterAccounts != accounts.size())
        LoadRoster(rotation, accounts);

    rotation.acquainted.clear();
    AcquaintanceMgr::Instance().CollectBots(rotation.acquaintedMinScore, rotation.acquainted);

    bool const noDeathKnights = sPlayerbotAIConfig.DisableDeathKnightLogin;
    LadderMgr const& ladder = LadderMgr::Instance();
    rotation.entries.clear();
    rotation.entries.reserve(rotation.roster.size());
    for (RosterBot const& bot : rotation.roster)
    {
        PopulationPolicy::RotationEntry entry;
        entry.guid = bot.guid;
        entry.team = bot.team;
        entry.acquainted = rotation.acquainted.contains(bot.guid);
        entry.blocked = ladder.IsAway(bot.guid) || (noDeathKnights && bot.classId == CLASS_DEATH_KNIGHT);
        entry.online = online.contains(bot.guid);
        entry.pinned = entry.online && IsPinned(sRandomPlayerbotMgr.GetPlayerBot(bot.guid));
        entry.weight = BalanceWeight(bot.guid);
        rotation.entries.push_back(entry);
    }

    rotation.settings.cap = cap;
    rotation.last = rotation.schedule.Tick(now, rotation.entries, rotation.settings);
    rotation.lastTick = now;
    ++rotation.ticks;
    swaps = rotation.last.swaps;

    // Online bots outside the roster (RosterSize lowered, accounts changed) go within the budget; the login fill
    // takes roster bots in their place
    rotation.lastExtra = 0;
    if (!rotation.roster.empty())
        for (uint32 const guid : online)
        {
            if (rotation.lastExtra >= std::max<uint32>(1, rotation.last.budget))
                break;

            if (rotation.rosterGuids.contains(guid))
                continue;

            Player* extra = sRandomPlayerbotMgr.GetPlayerBot(guid);
            if (IsPinned(extra) || InArenaTeam(extra))
                continue;

            swaps.push_back({guid, 0});
            ++rotation.lastExtra;
        }

    for (PopulationPolicy::RotationSwap const& swap : rotation.last.swaps)
    {
        if (!rotation.acquainted.contains(swap.in))
        {
            LOG_DEBUG("playerbots", "[Population] rotation: {} ({}) out, {} ({}) in", BotName(swap.out), swap.out,
                      BotName(swap.in), swap.in);
            continue;
        }

        ++rotation.acquaintedLogins;
        LOG_INFO("playerbots", "[Population] rotation: acquainted bot {} ({}) logs in for {} ({}), owed {:.0f} s",
                 BotName(swap.in), swap.in, BotName(swap.out), swap.out, rotation.schedule.Credit(swap.in));
    }

    rotation.swaps += rotation.last.swaps.size();
    rotation.extraLogouts += rotation.lastExtra;
    LOG_INFO("playerbots",
             "[Population] rotation: {} swaps (budget {}), {} out of the roster, online {}/{}, roster {} (eligible "
             "{}, blocked {}, acquainted {}), shares acquainted {:.2f} other {:.2f}",
             rotation.last.swaps.size(), rotation.last.budget, rotation.lastExtra, online.size(), cap,
             rotation.roster.size(), rotation.last.eligible, rotation.last.blocked, rotation.last.acquainted,
             rotation.last.shares.acquainted, rotation.last.shares.other);

    return swaps;
}

bool Population::InRoster(uint32 guid)
{
    Rotation const& rotation = GetRotation();
    return rotation.roster.empty() || rotation.rosterGuids.contains(guid);
}

bool Population::LoginBefore(uint32 a, uint32 b)
{
    Rotation const& rotation = GetRotation();
    uint32 const now = Now();
    uint32 const minSession = rotation.settings.minSessionSeconds;
    return PopulationPolicy::RotationSchedule::Before(rotation.schedule.Key(a, now, minSession),
                                                      rotation.schedule.Key(b, now, minSession));
}

void Population::NoteLogin(uint32 guid) { GetRotation().schedule.Note(guid, true, Now()); }

void Population::InvalidateRoster(uint32 retiredGuid)
{
    Rotation& rotation = GetRotation();
    rotation.rosterDirty = true;
    if (retiredGuid)
        rotation.schedule.Forget(retiredGuid);
}

bool Population::IsBalance() { return s_balance.load(std::memory_order_relaxed); }

float Population::XPMultiplier(uint32 level)
{
    if (!s_balance.load(std::memory_order_acquire))
        return 1.0f;

    float const multiplier = s_levelXP[std::min<size_t>(level, LevelSlots - 1)].load(std::memory_order_relaxed);
    return multiplier > 0.0f ? multiplier : 1.0f;
}

void Population::BalanceTick(std::unordered_set<uint32> const& online)
{
    if (!IsBalance())
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("Population::BalanceTick");
    Balance& balance = GetBalance();
    uint32 const now = Now();
    if (now < balance.nextUpdate)
        return;

    balance.nextUpdate = now + balance.intervalSeconds;
    // A stalled or paused update integrates at most two intervals
    uint32 const elapsed = balance.lastUpdate && now > balance.lastUpdate
                               ? std::min(now - balance.lastUpdate, 2 * balance.intervalSeconds)
                               : 0;
    balance.lastUpdate = now;

    balance.counts.assign(balance.ranges.size(), 0);
    balance.population = 0;
    auto const count = [&balance](uint32 guid)
    {
        uint8 const level = CachedLevel(guid);
        if (!level || balance.counts.empty())
            return;

        ++balance.counts[BalanceIndex(balance.settings, balance.counts.size(), level)];
        ++balance.population;
    };

    Rotation const& rotation = GetRotation();
    balance.fromRoster = IsRotation() && !rotation.roster.empty();
    if (balance.fromRoster)
        for (RosterBot const& bot : rotation.roster)
            count(bot.guid);
    else
        for (uint32 const guid : online)
            count(guid);

    PopulationPolicy::BalanceUpdate(balance.ranges, balance.counts, balance.population, now, elapsed, balance.settings);
    PublishBalance(balance, true);
    ++balance.updates;

    std::string text;
    for (PopulationPolicy::BalanceRange const& range : balance.ranges)
        text +=
            Acore::StringFormat(" {} {}/{:.0f} x{:.2f}", RangeName(range), range.count, range.target, range.multiplier);

    LOG_INFO("playerbots", "[Population] balance: {} bots ({}), levels count/target XP:{}", balance.population,
             balance.fromRoster ? "roster" : "online", text);
}

std::vector<PopulationPolicy::BalanceRange> Population::BalanceRanges()
{
    Balance const& balance = GetBalance();
    if (!IsBalance() || !balance.updates)
        return {};

    return balance.ranges;
}

uint32 Population::BalanceRangeOf(uint32 level)
{
    Balance const& balance = GetBalance();
    return BalanceIndex(balance.settings, balance.ranges.size(), level);
}

bool Population::IsRetire() { return s_retire.load(std::memory_order_relaxed); }

void Population::RetireTick(std::unordered_set<uint32> const& online)
{
    Retire& retire = GetRetire();
    if (!retire.busy && !IsRetire())
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("Population::RetireTick");
    retire.online = &online;
    retire.queries.ProcessReadyCallbacks();
    retire.online = nullptr;
    if (retire.busy || !IsRetire())
        return;

    uint32 const now = Now();
    if (now < retire.nextPass)
        return;

    retire.nextPass = now + retire.intervalSeconds;
    if (!retire.prepared)
        StartRetirePrepare(retire);
    else
        StartRetirePass(retire, now);
}

bool Population::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    Rotation const& rotation = GetRotation();
    PopulationPolicy::RotationSettings const& settings = rotation.settings;
    PopulationPolicy::RotationTickResult const& last = rotation.last;
    uint32 const now = Now();

    handler->PSendSysMessage("Population: persistent {}, rotation {}; roster {} characters (RosterSize {}), cap {}",
                             IsPersistent() ? "on" : "off", IsRotation() ? "on" : "off", rotation.roster.size(),
                             rotation.rosterSize, settings.cap);
    handler->PSendSysMessage(
        "Settings: interval {} s, mean session {} min, min session {} min, acquainted share {} % (score >= {})",
        settings.intervalSeconds, settings.meanSessionSeconds / 60, settings.minSessionSeconds / 60,
        settings.acquaintedSharePct, rotation.acquaintedMinScore);
    PrintBalance(handler, now);
    PrintRetire(handler, now);
    if (!rotation.ticks)
    {
        handler->PSendSysMessage("No rotation tick yet");
        return true;
    }

    handler->PSendSysMessage(
        "Last tick {} s ago: eligible {}, blocked {}, acquainted {}, online {}, budget {}, swaps {}, out of the "
        "roster {}; shares acquainted {:.2f}, other {:.2f}",
        now - rotation.lastTick, last.eligible, last.blocked, last.acquainted, last.online, last.budget,
        last.swaps.size(), rotation.lastExtra, last.shares.acquainted, last.shares.other);
    handler->PSendSysMessage("Totals: ticks {}, swaps {}, acquainted logins {}, out-of-roster logouts {}",
                             rotation.ticks, rotation.swaps, rotation.acquaintedLogins, rotation.extraLogouts);

    uint32 listed = 0;
    for (PopulationPolicy::RotationEntry const& entry : rotation.entries)
    {
        if (!entry.acquainted)
            continue;

        if (++listed > 20)
        {
            handler->PSendSysMessage("  ... {} acquainted bots in all", last.acquainted);
            break;
        }

        handler->PSendSysMessage("  acquainted {} ({}): {}{}, owed {:.0f} s", BotName(entry.guid), entry.guid,
                                 entry.blocked ? "blocked" : (entry.online ? "online" : "offline"),
                                 entry.pinned ? " (pinned)" : "", rotation.schedule.Credit(entry.guid));
    }

    return true;
}

bool Population::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    std::vector<std::string> failures;
    uint32 checks = PopulationPolicy::RotationSelftest(failures, false);
    checks += PopulationPolicy::BalanceSelftest(failures, false);
    checks += PopulationPolicy::RetireSelftest(failures, false);
    for (std::string const& failure : failures)
        handler->PSendSysMessage("POPULATION selftest FAIL: {}", failure);

    handler->PSendSysMessage("POPULATION selftest: {} checks, {} failures{}", checks, failures.size(),
                             failures.empty() ? " (PASS)" : "");
    return true;
}

class PopulationWorldScript : public WorldScript
{
public:
    PopulationWorldScript()
        : WorldScript("PopulationWorldScript", {WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD})
    {
    }

    void OnStartup() override { Population::LoadConfig(false); }

    // The initial (reload == false) call comes before the world is set up; OnStartup covers it.
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            Population::LoadConfig(true);
    }
};

void AddSC_population() { new PopulationWorldScript(); }
