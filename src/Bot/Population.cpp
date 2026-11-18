/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Population.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AcquaintanceMgr.h"
#include "AsyncCallbackProcessor.h"
#include "BotKind.h"
#include "BotProfileMgr.h"
#include "BotRecordMgr.h"
#include "BotTraitMgr.h"
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
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "PopulationPolicy.h"
#include "PvpGearMgr.h"
#include "QueryCallback.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
// BotKind::FollowsTargets (the role value Playerbots.Population.FollowTargets) as LoadConfig last saw it: the
// rotation and the retirement are on together, and the retirement's first pass waits from when this turns on
std::atomic<bool> s_followsTargets{false};

// Population::ReloadRoster's count: the factory made characters; a roster loaded at a lower count loads again
std::atomic<uint32> s_rosterReloads{0};

constexpr char const* RosterSizeKey = "Playerbots.Population.Rotation.RosterSize";
constexpr uint32 DefaultRosterSize = 2500;

// The characters on the roster accounts (stored account type 1) when a start first sizes the accounts, before the
// factory makes any. Read once and kept (startup, world thread; synchronous, as the factory's own reads)
uint32 RosterCharactersFound()
{
    static bool read = false;
    static uint32 found = 0;
    if (read)
        return found;

    read = true;
    std::string accounts;
    if (PreparedQueryResult result =
            PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_ACCOUNT_TYPE)))
    {
        do
        {
            Field* fields = result->Fetch();
            if (fields[1].Get<uint8>() != 1)
                continue;

            if (!accounts.empty())
                accounts += ',';

            accounts += std::to_string(fields[0].Get<uint32>());
        } while (result->NextRow());
    }

    if (!accounts.empty())
        if (QueryResult result =
                CharacterDatabase.Query("SELECT COUNT(*) FROM characters WHERE account IN ({})", accounts))
            found = static_cast<uint32>(result->Fetch()[0].Get<uint64>());

    LOG_INFO("playerbots", "[Population] startup: {} characters on the roster accounts", found);
    return found;
}

// World thread only
struct Rotation
{
    PopulationPolicy::RotationSettings settings;
    uint32 acquaintedMinScore = 1;
    PopulationPolicy::RotationSchedule schedule;
    PopulationRoster roster;  // loaded once, then changed in place where a roster character is made or deleted
    bool rosterLoaded = false;
    uint32 rosterReloads = 0;      // s_rosterReloads at the last load
    std::vector<uint32> accounts;  // the roster accounts of the last roster load
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

    return !PopulationPolicy::MayLogOut(Population::LogoutFactsOf(bot));
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

// The cost of the enumeration and of LoadRoster (world thread; the status command reads it from the console)
struct EnumerationCost
{
    uint32 calls = 0;
    uint64 micros = 0;
    uint64 maxMicros = 0;
    uint32 characters = 0;
    uint32 loadCalls = 0;
    uint64 loadMicros = 0;
};

EnumerationCost enumerationCost;

std::string EnumerationLine()
{
    return Acore::StringFormat(
        "roster enumeration: ramp of {} AddRandomBots calls, {} us in total ({} us max), {} characters; "
        "LoadRoster {} calls {} us",
        enumerationCost.calls, enumerationCost.micros, enumerationCost.maxMicros, enumerationCost.characters,
        enumerationCost.loadCalls, enumerationCost.loadMicros);
}

std::string JoinIds(std::vector<uint32> const& ids)
{
    std::string text;
    for (uint32 const id : ids)
        text += (text.empty() ? "" : ",") + std::to_string(id);

    return text;
}

// One synchronous query (world thread), at the first use after startup's account types; again only when the roster
// accounts change or the factory made characters (ReloadRoster)
void LoadRoster(Rotation& rotation, std::vector<uint32> const& accounts)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Population::LoadRoster");
    auto const loadStart = std::chrono::steady_clock::now();
    rotation.rosterReloads = s_rosterReloads.load(std::memory_order_relaxed);
    std::vector<PopulationRoster::Entry> entries;
    if (!accounts.empty())
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT guid, account, race, class FROM characters WHERE account IN ({}) AND deleteDate IS NULL",
                JoinIds(accounts)))
            do
            {
                Field* fields = result->Fetch();
                entries.push_back(
                    {fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<uint8>(), fields[3].Get<uint8>()});
            } while (result->NextRow());

    rotation.roster.Load(std::move(entries));
    rotation.rosterLoaded = true;
    rotation.accounts = accounts;
    auto const loadElapsed = std::chrono::steady_clock::now() - loadStart;
    uint64 const loadMicros = std::chrono::duration_cast<std::chrono::microseconds>(loadElapsed).count();
    ++enumerationCost.loadCalls;
    enumerationCost.loadMicros += loadMicros;
    LOG_INFO("playerbots", "[Population] rotation roster: {} characters on {} accounts in {} us",
             rotation.roster.Size(), accounts.size(), loadMicros);
}

void EnsureRoster(Rotation& rotation, std::vector<uint32> const& accounts)
{
    if (!rotation.rosterLoaded || rotation.accounts != accounts ||
        rotation.rosterReloads != s_rosterReloads.load(std::memory_order_relaxed))
        LoadRoster(rotation, accounts);
}

// The level of any character, online or not, from the character cache (kept current on level-up); 0 = unknown
uint8 CachedLevel(uint32 guid)
{
    return sCharacterCache->GetCharacterLevelByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
}

// H4a: retiring never-met random bots (Playerbots.Population.Retire.*)
constexpr uint32 RetireMaxScore = 0;             // the highest acquaintance score of a retired bot: never met
constexpr uint32 RetireCandidates = 50;          // bots a pass checks against the database
constexpr uint32 RetireCreateAttempts = 3;       // tries per replacement before it is given up
constexpr uint32 RetireSummarySeconds = 3600;    // between two summary lines
constexpr size_t RetireRecordsKept = 5;          // the last retires and creations the status lists
constexpr uint32 CreateConfirmRetrySeconds = 5;  // a missing row is looked for again (several async DB workers)

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
    {"playerbots_bot_traits", "guid", false},
    {"playerbots_bot_record", "guid", false},
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
};

// One pass's candidate batch and the protections its queries found
struct RetirePass
{
    std::vector<uint32> batch;
    std::unordered_map<uint32, uint32> protect;
    uint32 seed = 0;
    bool failed = false;
};

// The counters a summary reports as differences
struct RetireTotals
{
    uint64 retired = 0;
    uint64 created = 0;
    uint64 gaveUp = 0;
    uint64 refused = 0;
};

// World thread only
struct Retire
{
    uint32 intervalSeconds = 600;
    uint32 maxPerPass = 1;
    uint32 maxPerHour = 10;
    uint32 testTarget = 0;  // the top level is retired from down to this many; 0: not at all
    uint32 nextPass = 0;
    bool busy = false;                                 // the preparation's or a pass's queries are in flight
    bool prepared = false;                             // the schemas' columns and the clone accounts are loaded
    std::unordered_set<std::string> characterColumns;  // "table.column"
    std::unordered_set<std::string> playerbotsColumns;
    std::unordered_set<uint32> cloneAccounts;  // lab-bots clone accounts (email LAB-BOTS ...): never touched
    std::unordered_set<uint32> refusedLogged;  // known bots whose refusal was logged (once per bot)
    std::vector<uint32> recent;                // game times of this hour's retires
    std::deque<RetireRecord> last;
    std::unordered_set<uint32> const* online = nullptr;  // the manager's online set while the callbacks run
    uint32 seed = 0;
    uint32 lastPass = 0;
    uint32 lastInRange = 0;
    uint32 characterRows = 0;
    uint32 playerbotsRows = 0;
    uint64 passes = 0;
    uint64 retired = 0;
    uint64 refused = 0;
    uint32 lastSummary = 0;
    uint32 nextSummary = 0;
    bool counting = false;                       // the summary's character count is in flight
    PopulationPolicy::CreateAccounting creates;  // the replacements the live retires owe (H4b)
    std::deque<RetireRecord> lastCreated;
    uint64 made = 0;  // characters made and saved, confirmed or not
    uint64 createMsTotal = 0;
    uint32 createMsMax = 0;
    RetireTotals summaryBase;
    QueryCallbackProcessor queries;
};

Retire& GetRetire()
{
    static Retire retire;
    return retire;
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

// The ranges a pass may retire from with their roster counts now: the top level with Retire.TestTarget; only those
// with a surplus
std::vector<PopulationPolicy::RetireRange> RetireRanges(Retire const& retire)
{
    std::vector<PopulationPolicy::RetireRange> ranges =
        PopulationPolicy::TopLevelRetireRanges(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), retire.testTarget);

    if (ranges.empty())
        return ranges;

    for (PopulationRoster::Entry const& bot : GetRotation().roster.Entries())
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
             PopulationPolicy::RetireReasons(candidate.protect, candidate.score, RetireMaxScore));
}

// A bot's complete delete (H4 retire, WL19 resize): the managers' memory, the rows the core's delete leaves behind,
// then the core's delete. World thread; the caller checked the bot a moment ago.
void DeleteBot(Retire& retire, uint32 guid, uint32 account)
{
    // Memory first: nothing may write the bot's rows back after the delete
    sRandomPlayerbotMgr.ForgetBot(guid);
    BotProfileMgr::instance().Forget(guid);
    BotTraitMgr::instance().Forget(guid);
    BotRecordMgr::Instance().Forget(guid);
    AcquaintanceMgr::Instance().ForgetBot(guid);
    PvpGearMgr::Instance().ForgetBot(guid);
    Population::RemoveFromRoster(guid);

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
    // guild membership and the character cache entry; the account stays, and its free slot takes the replacement. It
    // runs a few synchronous mail queries on this thread, hence the callers' caps.
    Player::DeleteFromDB(guid, account, true, true);
}

// Deletes one bot. The caller checked it a moment ago on this thread: offline, in an over-target range, never met,
// unprotected.
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
    retire.last.push_front({name, guid, level, now});
    if (retire.last.size() > RetireRecordsKept)
        retire.last.pop_back();

    DeleteBot(retire, guid, account);
    PopulationPolicy::CreateOwe(retire.creates, guid, account, classId, now, name);
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
    if (!s_followsTargets.load(std::memory_order_relaxed))
        return;

    if (pass.failed)
    {
        LOG_ERROR("playerbots", "[Population] retire pass: a protection query failed; nothing retired");
        return;
    }

    uint32 const now = Now();
    std::vector<PopulationPolicy::RetireRange> ranges = RetireRanges(retire);
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

        if (candidate.protect || candidate.score > RetireMaxScore)
            ++guarded;

        candidates.push_back(candidate);
    }

    uint32 const left = PopulationPolicy::RetireHourLeft(retire.recent, now, retire.maxPerHour);
    std::vector<uint32> const chosen = PopulationPolicy::SelectRetirees(candidates, ranges, RetireMaxScore,
                                                                        std::min(left, retire.maxPerPass), pass.seed);
    LOG_DEBUG("playerbots", "[Population] retire pass: {} bots in range, batch {}, {} protected, {} chosen",
              retire.lastInRange, pass.batch.size(), guarded, chosen.size());
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

// The protections the world thread does not hold, one query per DB (guild or friend of a real player, arena team, guild
// leader, group, ladder roster); `finish` gets them on the world thread. Every query returns a sentinel row, so an
// empty result means a failed query.
void QueryProtections(Retire& retire, std::shared_ptr<RetirePass> const& pass, void (*finish)(RetirePass const&))
{
    std::string const ids = JoinIds(pass->batch);
    std::string const bots = JoinIds(sPlayerbotAIConfig.RandomBotAccounts);
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
                                       [pass, finish](QueryResult result)
                                       {
                                           if (!result)
                                               pass->failed = true;
                                           else
                                               do
                                               {
                                                   pass->protect[result->Fetch()[0].Get<uint32>()] |=
                                                       PopulationPolicy::RetireLadderRoster;
                                               } while (result->NextRow());

                                           finish(*pass);
                                       }));
}

// A pass's first half: the bots of the over-target ranges, filtered in memory and ordered by the rule, and one query
// per DB for the protections the world thread does not hold (guild or friend of a real player, arena team, guild
// leader, group, ladder roster). Every query returns a sentinel row, so an empty result means a failed query.
void StartRetirePass(Retire& retire, uint32 now)
{
    // The roster is never short by more than one pass's retires
    if (!PopulationPolicy::RetireGate(retire.creates))
    {
        LOG_DEBUG("playerbots", "[Population] retire pass skipped: {} replacements owed{}", retire.creates.owed.size(),
                  retire.creates.paused ? ", paused after a replacement was given up" : "");
        return;
    }

    Rotation const& rotation = GetRotation();
    std::vector<uint32> const& accounts = sPlayerbotAIConfig.RandomBotAccounts;
    if (rotation.roster.Empty() || accounts.empty() || !AcquaintanceMgr::Instance().IsLoaded())
    {
        LOG_DEBUG("playerbots", "[Population] retire pass skipped: roster {}, random bot accounts {}, acquaintances {}",
                  rotation.roster.Size(), accounts.size(),
                  AcquaintanceMgr::Instance().IsLoaded() ? "loaded" : "not loaded");
        return;
    }

    if (!retire.maxPerPass || !PopulationPolicy::RetireHourLeft(retire.recent, now, retire.maxPerHour))
        return;

    std::vector<PopulationPolicy::RetireRange> const ranges = RetireRanges(retire);
    if (ranges.empty())
        return;

    ++retire.passes;
    retire.lastPass = now;
    uint32 const seed = retire.seed ^ static_cast<uint32>(retire.passes * 2654435761ull);
    std::vector<PopulationPolicy::RetireCandidate> pool;
    for (PopulationRoster::Entry const& bot : rotation.roster.Entries())
    {
        if (RetireRangeIndex(ranges, CachedLevel(bot.guid)) < 0)
            continue;

        PopulationPolicy::RetireCandidate const candidate = RetireCandidateOf(retire, bot.guid);
        // Acquainted bots (DH.2): logged once each
        if (candidate.score > RetireMaxScore && !(candidate.protect & PopulationPolicy::RetireClone))
            LogRetireRefusal(retire, candidate);

        pool.push_back(candidate);
    }

    retire.lastInRange = static_cast<uint32>(pool.size());
    auto pass = std::make_shared<RetirePass>();
    pass->batch = PopulationPolicy::SelectRetirees(pool, ranges, RetireMaxScore, RetireCandidates, seed, false);
    pass->seed = seed;
    if (pass->batch.empty())
    {
        LOG_DEBUG("playerbots", "[Population] retire pass: {} bots in range, none unprotected", pool.size());
        return;
    }

    QueryProtections(retire, pass, FinishRetirePass);
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
                        "the core's delete; {} lab clone accounts never touched; each live retire is replaced by a "
                        "new bot of its class on its account{}{}",
                        retire.characterRows, retire.playerbotsRows, retire.cloneAccounts.size(),
                        missing.empty() ? "" : "; not in the DB:", missing);
                }));
}

RetireTotals TotalsOf(Retire const& retire)
{
    RetireTotals totals;
    totals.retired = retire.retired;
    totals.created = retire.creates.created;
    totals.gaveUp = retire.creates.gaveUp;
    totals.refused = retire.refused;
    return totals;
}

// The entry as it was before the failed attempt
void LogCreateFailure(PopulationPolicy::CreateEntry const& entry, PopulationPolicy::CreateDecision decision,
                      uint32 maxAttempts, uint32 retrySeconds, std::string const& why)
{
    if (decision == PopulationPolicy::CreateDecision::GaveUp)
        LOG_ERROR("playerbots",
                  "[Population] create gave up: account {} class {} (replacing {} #{}) after {} attempts, last: {}; "
                  "the roster is short by 1 until the next restart (the factory refills accounts at startup); "
                  "retires pause until .reload config",
                  entry.account, entry.classId, entry.retiredName, entry.retiredGuid, entry.attempts + 1, why);
    else
        LOG_WARN("playerbots",
                 "[Population] create failed: account {} class {} (replacing {} #{}): {}; attempt {} of {}, again in "
                 "{} s",
                 entry.account, entry.classId, entry.retiredName, entry.retiredGuid, why, entry.attempts + 1,
                 maxAttempts, retrySeconds);
}

// A new character's confirm query: its row once the save is committed (queued behind the save's transaction on the
// same async worker)
std::string ConfirmQuery(uint32 guid, uint32 account)
{
    return Acore::StringFormat("SELECT guid, account, race, class FROM characters WHERE guid = {} AND account = {}",
                               guid, account);
}

// The confirm query's row joins the roster (world thread); a roster not loaded yet reads it at its first load
void RosterInsertConfirmed(QueryResult const& result)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Population::RosterInsertConfirmed");
    Rotation& rotation = GetRotation();
    if (!rotation.rosterLoaded)
        return;

    Field* fields = result->Fetch();
    rotation.roster.Insert(
        {fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<uint8>(), fields[3].Get<uint8>()});
}

// The confirm query's answer (world thread): the replacement joins the roster only once its row is there
void ConfirmCreated(uint32 retiredGuid, uint32 guid, QueryResult const& result)
{
    bool const found = static_cast<bool>(result);
    Retire& retire = GetRetire();
    PopulationPolicy::CreateEntry const* owed = PopulationPolicy::CreateFind(retire.creates, retiredGuid);
    if (!owed)
        return;

    PopulationPolicy::CreateEntry const entry = *owed;
    PopulationPolicy::CreateDecision const decision = PopulationPolicy::CreateApply(
        retire.creates, retiredGuid,
        found ? PopulationPolicy::CreateOutcome::Confirmed : PopulationPolicy::CreateOutcome::Missing, Now(),
        CreateConfirmRetrySeconds, RetireCreateAttempts);
    if (decision == PopulationPolicy::CreateDecision::Done)
    {
        RosterInsertConfirmed(result);
        LOG_DEBUG("playerbots", "[Population] create confirmed: #{} replaces #{}", guid, retiredGuid);
        return;
    }

    LogCreateFailure(entry, decision, RetireCreateAttempts, CreateConfirmRetrySeconds,
                     Acore::StringFormat("#{} not in characters after the save", guid));
}

void StartCreateConfirm(Retire& retire, uint32 retiredGuid, uint32 guid, uint32 account, uint32 now)
{
    PopulationPolicy::CreateApply(retire.creates, retiredGuid, PopulationPolicy::CreateOutcome::Created, now, 0,
                                  RetireCreateAttempts, guid);
    retire.queries.AddCallback(
        CharacterDatabase.AsyncQuery(ConfirmQuery(guid, account))
            .WithCallback([retiredGuid, guid](QueryResult result) { ConfirmCreated(retiredGuid, guid, result); }));
}

struct MadeBot
{
    uint32 guid = 0;
    std::string name;
    uint32 level = 0;
    uint32 race = 0;
};

// The startup factory's new character of `classId` on `account`, saved and in the character cache (world thread; the
// factory's name pick runs up to two synchronous queries). A `level` above the class's start level is set before the
// save.
bool MakeBot(uint32 account, uint32 classId, uint32 level, MadeBot& made)
{
    std::unique_ptr<WorldSession> const session(new WorldSession(account, "", 0x0, nullptr, SEC_PLAYER,
                                                                 EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0),
                                                                 LOCALE_enUS, 0, false, false, 0));
    // Empty: the factory picks the one name it needs
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> names;
    RandomPlayerbotFactory factory;
    Player* bot = factory.CreateRandomBot(session.get(), static_cast<uint8>(classId), names);
    if (!bot)
        return false;

    if (level > bot->GetLevel())
        bot->SetLevel(static_cast<uint8>(level), false);

    made.guid = bot->GetGUID().GetCounter();
    made.name = bot->GetName();
    made.level = bot->GetLevel();  // a death knight starts at the heroic start level
    made.race = bot->getRace();
    bot->SaveToDB(true, false);
    sCharacterCache->AddCharacterCacheEntry(bot->GetGUID(), account, made.name, bot->getGender(), bot->getRace(),
                                            bot->getClass(), bot->GetLevel());
    bot->CleanupsBeforeDelete();
    delete bot;
    return true;
}

// The owed replacements (world thread): the startup factory's creation and save on the retired bot's account, then a
// confirm query. The factory's name pick runs up to two synchronous queries, hence at most MaxPerPass per call, timed.
void CreateOwedBots(Retire& retire, uint32 now)
{
    for (uint32 const retiredGuid : PopulationPolicy::CreateDue(retire.creates, now, retire.maxPerPass))
    {
        PopulationPolicy::CreateEntry const* owed = PopulationPolicy::CreateFind(retire.creates, retiredGuid);
        if (!owed)
            continue;

        PopulationPolicy::CreateEntry const entry = *owed;
        if (entry.createdGuid)
        {
            StartCreateConfirm(retire, retiredGuid, entry.createdGuid, entry.account, now);
            continue;
        }

        uint32 const start = getMSTime();
        MadeBot made;
        if (!MakeBot(entry.account, entry.classId, 0, made))
        {
            PopulationPolicy::CreateDecision const decision =
                PopulationPolicy::CreateApply(retire.creates, retiredGuid, PopulationPolicy::CreateOutcome::Failed, now,
                                              retire.intervalSeconds, RetireCreateAttempts);
            LogCreateFailure(entry, decision, RetireCreateAttempts, retire.intervalSeconds,
                             "the factory made no character");
            continue;
        }

        uint32 const ms = GetMSTimeDiffToNow(start);
        ++retire.made;
        retire.createMsTotal += ms;
        retire.createMsMax = std::max(retire.createMsMax, ms);
        retire.lastCreated.push_front({made.name, made.guid, made.level, now});
        if (retire.lastCreated.size() > RetireRecordsKept)
            retire.lastCreated.pop_back();

        LOG_INFO("playerbots",
                 "[Population] create: {} (#{}, level {}, class {}, race {}, account {}) replaces {} (#{}), took {} ms",
                 made.name, made.guid, made.level, entry.classId, made.race, entry.account, entry.retiredName,
                 entry.retiredGuid, ms);
        StartCreateConfirm(retire, retiredGuid, made.guid, entry.account, now);
    }
}

// World-log WL19: ".playerbots population resize <count>" (console) moves the roster toward `count` characters by
// WL.14's shares. Deletes are complete (DeleteBot) and take offline unprotected bots of the ranges most over their
// share; additions are the factory's new characters at a level of the ranges most under it, on free character slots of
// the bot accounts, and their first randomize is a full one at that level (RandomPlayerbotMgr::MarkMadeAtLevel), which
// gives the starting history. Existing bots keep their levels.
constexpr uint32 ResizeDeletesPerTick = 5;         // the core's delete runs a few synchronous mail queries
constexpr uint32 ResizeCreatesPerTick = 2;         // the factory's name pick runs up to two synchronous queries
constexpr uint32 ResizeCharactersPerAccount = 10;  // the startup factory's limit per account

enum class ResizeStage
{
    Idle,
    Plan,      // asked for: planned at the next tick
    Querying,  // the protections or the free slots are being read
    Delete,
    Create,
};

struct ResizeSlot
{
    uint32 account;
    uint32 free;
};

// World thread only
struct Resize
{
    ResizeStage stage = ResizeStage::Idle;
    uint32 target = 0;
    uint32 from = 0;
    uint32 before = 0;  // all characters on the accounts when the resize started (`from` counts the layout's only)
    bool planOnly = false;
    bool preparing = false;  // waits for the retire's preparation (the rows a delete clears)
    std::vector<PopulationPolicy::ResizeRange> ranges;
    std::vector<uint32> deletes;  // the chosen bots, the next at the back
    std::vector<uint32> adds;     // the range index of each bot to make, the next at the back
    std::vector<ResizeSlot> slots;
    std::vector<uint32> classCounts;  // roster bots per class id
    uint32 deleted = 0;
    uint32 created = 0;
    uint32 skipped = 0;     // chosen bots protected by then, failed creations
    uint32 confirming = 0;  // created bots whose confirm query has not answered: not in the roster yet
    uint32 startedAt = 0;
    std::string plan;  // the plan as text
    std::string last;  // the last resize's result
};

Resize& GetResize()
{
    static Resize resize;
    return resize;
}

uint32 ResizeMaxLevel()
{
    return std::min<uint32>(sPlayerbotAIConfig.RandomBotMaxLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
}

char const* ResizeStageName(ResizeStage stage)
{
    switch (stage)
    {
        case ResizeStage::Plan:
            return "planning";
        case ResizeStage::Querying:
            return "reading the DB";
        case ResizeStage::Delete:
            return "deleting";
        case ResizeStage::Create:
            return "creating";
        default:
            return "idle";
    }
}

void FinishResize(Resize& resize, std::string const& note)
{
    uint32 count = resize.from;
    // The roster when the resize started, less the deletes and plus the creations it made (not from `from`, which
    // misses bots outside the layout's levels; not the roster now, which takes the creations once they are confirmed)
    if (resize.deleted || resize.created)
        count = PopulationPolicy::ResizeDoneCount(resize.before, resize.deleted, resize.created);

    resize.last =
        Acore::StringFormat("roster {} -> {} (asked {}{}): {} deleted, {} created, {} skipped in {} s{}{}", resize.from,
                            count, resize.target, resize.planOnly ? ", plan only" : "", resize.deleted, resize.created,
                            resize.skipped, Now() - resize.startedAt, note.empty() ? "" : "; ", note);
    LOG_INFO("playerbots", "[Population] resize done: {}", resize.last);
    resize.stage = ResizeStage::Idle;
    resize.deletes.clear();
    resize.adds.clear();
    resize.slots.clear();
}

void LogResizePlan(Resize& resize, int32 planned, std::string const& shortfall)
{
    resize.plan = PopulationPolicy::ResizePlanText(resize.ranges);
    LOG_INFO("playerbots", "[Population] resize plan{}: roster {} -> {} ({:+}): {}{}{}",
             resize.planOnly ? " (plan only)" : "", resize.from, resize.target, planned, resize.plan,
             shortfall.empty() ? "" : "; ", shortfall);
}

// The deletion's plan (world thread, from the protection queries): only bots still offline and unprotected count as
// removable
void FinishResizeProtect(RetirePass const& pass)
{
    Retire& retire = GetRetire();
    retire.busy = false;
    Resize& resize = GetResize();
    if (resize.stage != ResizeStage::Querying)
        return;

    if (pass.failed)
    {
        FinishResize(resize, "a protection query failed, nothing deleted");
        return;
    }

    std::vector<std::vector<PopulationPolicy::RetireCandidate>> byRange(resize.ranges.size());
    for (uint32 const guid : pass.batch)
    {
        PopulationPolicy::RetireCandidate candidate = RetireCandidateOf(retire, guid);
        auto const it = pass.protect.find(guid);
        if (it != pass.protect.end())
            candidate.protect |= it->second;

        int32 const index = PopulationPolicy::ResizeRangeOf(resize.ranges, candidate.level);
        if (candidate.protect || candidate.score > RetireMaxScore || index < 0)
            continue;

        ++resize.ranges[index].removable;
        byRange[index].push_back(candidate);
    }

    int32 const planned = PopulationPolicy::ResizePlan(resize.ranges, resize.target);
    uint32 const seed = rand32();
    for (size_t i = 0; i < resize.ranges.size(); ++i)
    {
        PopulationPolicy::ResizeRange const& range = resize.ranges[i];
        if (range.delta >= 0)
            continue;

        std::vector<uint32> const picks =
            PopulationPolicy::SelectRetirees(byRange[i], {{range.firstLevel, range.lastLevel, 0, 0.0}}, RetireMaxScore,
                                             static_cast<uint32>(-range.delta), seed, false);
        resize.deletes.insert(resize.deletes.end(), picks.begin(), picks.end());
    }

    uint32 const asked = resize.from - resize.target;
    uint32 const removed = static_cast<uint32>(-planned);
    std::string const shortfall =
        removed < asked ? Acore::StringFormat("short by {}: too few offline unprotected bots", asked - removed)
                        : std::string();
    LogResizePlan(resize, planned, shortfall);
    if (resize.planOnly || resize.deletes.empty())
    {
        FinishResize(resize, shortfall);
        return;
    }

    resize.stage = ResizeStage::Delete;
}

// The addition's plan (world thread, from the character count query): the free character slots of the bot accounts
void FinishResizeSlots(QueryResult result)
{
    Resize& resize = GetResize();
    if (resize.stage != ResizeStage::Querying)
        return;

    if (!result)
    {
        FinishResize(resize, "the character count query failed, nothing created");
        return;
    }

    std::unordered_map<uint32, uint32> characters;
    do
    {
        Field* fields = result->Fetch();
        characters[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
    } while (result->NextRow());

    Retire const& retire = GetRetire();
    std::unordered_set<uint32> owedAccounts;
    for (PopulationPolicy::CreateEntry const& entry : retire.creates.owed)
        owedAccounts.insert(entry.account);

    uint32 free = 0;
    for (uint32 const account : GetRotation().accounts)
    {
        auto const it = characters.find(account);
        uint32 const held = it == characters.end() ? 0 : it->second;
        // An account owing a retire's replacement keeps its slot for it
        if (held >= ResizeCharactersPerAccount || owedAccounts.contains(account) ||
            retire.cloneAccounts.contains(account))
            continue;

        resize.slots.push_back({account, ResizeCharactersPerAccount - held});
        free += ResizeCharactersPerAccount - held;
    }

    uint32 const reachable = std::min(resize.target, resize.from + free);
    int32 const planned = PopulationPolicy::ResizePlan(resize.ranges, reachable);
    for (size_t i = 0; i < resize.ranges.size(); ++i)
        for (int32 n = 0; n < resize.ranges[i].delta; ++n)
            resize.adds.push_back(static_cast<uint32>(i));

    std::string const shortfall =
        reachable < resize.target
            ? Acore::StringFormat("short by {}: {} free character slots on the bot accounts (at most {} per account)",
                                  resize.target - reachable, free, ResizeCharactersPerAccount)
            : std::string();
    LogResizePlan(resize, planned, shortfall);
    if (resize.planOnly || resize.adds.empty())
    {
        FinishResize(resize, shortfall);
        return;
    }

    resize.stage = ResizeStage::Create;
}

// The plan's first half: the roster's counts per range and class, then the query the plan needs
void StartResize(Resize& resize, Retire& retire)
{
    // One query batch at a time: the retire's own (a pass or its preparation) ends first. Owed replacements (their bots
    // are deleted already; the retired ones' creations or retries) end first too, else the roster counted now misses
    // them and the done count is short. Every owed entry ends in a creation or a give-up, and no pass starts while a
    // resize is not idle, so nothing new is owed meanwhile.
    if (!PopulationPolicy::ResizeMayStart(retire.busy, retire.creates.owed.size()))
        return;

    // The last resize's creations join the roster first, else the roster counted now misses them
    if (resize.confirming)
        return;

    Rotation& rotation = GetRotation();
    resize.before = static_cast<uint32>(rotation.roster.Size());
    resize.ranges = PopulationPolicy::ResizeLayout(ResizeMaxLevel());
    resize.classCounts.assign(MAX_CLASSES, 0);
    resize.from = 0;
    for (PopulationRoster::Entry const& bot : rotation.roster.Entries())
    {
        int32 const index = PopulationPolicy::ResizeRangeOf(resize.ranges, CachedLevel(bot.guid));
        if (index < 0)
            continue;

        ++resize.ranges[index].count;
        ++resize.from;
        if (bot.classId < MAX_CLASSES)
            ++resize.classCounts[bot.classId];
    }

    if (resize.target == resize.from)
    {
        LogResizePlan(resize, 0, "");
        FinishResize(resize, "");
        return;
    }

    if (resize.target > resize.from)
    {
        resize.stage = ResizeStage::Querying;
        retire.queries.AddCallback(
            CharacterDatabase
                .AsyncQuery(Acore::StringFormat("SELECT account, COUNT(*) FROM characters WHERE account IN ({}) GROUP "
                                                "BY account UNION ALL SELECT 0, 0",
                                                JoinIds(rotation.accounts)))
                .WithCallback(FinishResizeSlots));
        return;
    }

    // A delete clears the rows the retire's preparation found in the DBs
    if (!retire.prepared)
    {
        if (resize.preparing)
        {
            FinishResize(resize, "the schema query failed, nothing deleted");
            return;
        }

        resize.preparing = true;
        StartRetirePrepare(retire);
        return;
    }

    std::vector<uint32> batch;
    for (PopulationRoster::Entry const& bot : rotation.roster.Entries())
    {
        PopulationPolicy::RetireCandidate const candidate = RetireCandidateOf(retire, bot.guid);
        if (!candidate.protect && candidate.score <= RetireMaxScore)
            batch.push_back(bot.guid);
    }

    if (batch.empty())
    {
        LogResizePlan(resize, 0, "no offline unprotected bot");
        FinishResize(resize, "no offline unprotected bot, nothing deleted");
        return;
    }

    auto pass = std::make_shared<RetirePass>();
    pass->batch = std::move(batch);
    resize.stage = ResizeStage::Querying;
    QueryProtections(retire, pass, FinishResizeProtect);
}

void ResizeDeleteTick(Resize& resize, Retire& retire)
{
    for (uint32 n = 0; n < ResizeDeletesPerTick && !resize.deletes.empty(); ++n)
    {
        uint32 const guid = resize.deletes.back();
        resize.deletes.pop_back();
        // Checked again: the bot may have logged in since the plan
        PopulationPolicy::RetireCandidate const candidate = RetireCandidateOf(retire, guid);
        CharacterCacheEntry const* entry =
            sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!entry || candidate.protect)
        {
            ++resize.skipped;
            continue;
        }

        LOG_DEBUG("playerbots", "[Population] resize delete: {} (#{}, level {}, class {}, account {})", entry->Name,
                  guid, entry->Level, entry->Class, entry->AccountId);
        DeleteBot(retire, guid, entry->AccountId);
        ++resize.deleted;
    }

    if (resize.deletes.empty())
        FinishResize(resize, "");
}

// The classes a new bot of `range` may have: the creatable ones, a death knight only from the heroic start level
uint32 ResizeClassMask(PopulationPolicy::ResizeRange const& range)
{
    uint32 mask = CLASSMASK_ALL_PLAYABLE & ~sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK);
    if (sPlayerbotAIConfig.DisableDeathKnightLogin ||
        range.lastLevel < sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
        mask &= ~(1u << (CLASS_DEATH_KNIGHT - 1));

    return mask;
}

// A resize's new bot joins the roster once the confirm query finds its row
void StartResizeConfirm(Resize& resize, uint32 guid, uint32 account)
{
    ++resize.confirming;
    GetRetire().queries.AddCallback(
        CharacterDatabase.AsyncQuery(ConfirmQuery(guid, account))
            .WithCallback(
                [guid](QueryResult result)
                {
                    --GetResize().confirming;
                    if (result)
                        RosterInsertConfirmed(result);
                    else
                        LOG_ERROR("playerbots",
                                  "[Population] resize create: #{} not in characters after the save; not in the "
                                  "roster until the next start",
                                  guid);
                }));
}

void ResizeCreateTick(Resize& resize)
{
    for (uint32 n = 0; n < ResizeCreatesPerTick && !resize.adds.empty(); ++n)
    {
        PopulationPolicy::ResizeRange const range = resize.ranges[resize.adds.back()];
        resize.adds.pop_back();
        while (!resize.slots.empty() && !resize.slots.back().free)
            resize.slots.pop_back();

        uint32 const classId = PopulationPolicy::ResizeClass(resize.classCounts, ResizeClassMask(range));
        if (resize.slots.empty() || !classId)
        {
            ++resize.skipped;
            continue;
        }

        uint32 const firstLevel =
            classId == CLASS_DEATH_KNIGHT
                ? std::max<uint32>(range.firstLevel, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
                : range.firstLevel;
        ResizeSlot& slot = resize.slots.back();
        MadeBot made;
        if (!MakeBot(slot.account, classId, urand(firstLevel, range.lastLevel), made))
        {
            ++resize.skipped;
            LOG_WARN("playerbots",
                     "[Population] resize create failed: account {} class {}: the factory made no character",
                     slot.account, classId);
            continue;
        }

        sRandomPlayerbotMgr.MarkMadeAtLevel(made.guid);
        StartResizeConfirm(resize, made.guid, slot.account);
        --slot.free;
        ++resize.classCounts[classId];
        ++resize.created;
        LOG_DEBUG("playerbots", "[Population] resize create: {} (#{}, level {}, class {}, race {}, account {})",
                  made.name, made.guid, made.level, classId, made.race, slot.account);
    }

    if (resize.adds.empty())
        FinishResize(resize, "");
}

// World thread, from RetireTick (its callbacks ran a moment ago, with the manager's online set)
void ResizeTick(Resize& resize, Retire& retire)
{
    if (resize.stage == ResizeStage::Plan)
        StartResize(resize, retire);
    else if (resize.stage == ResizeStage::Delete)
        ResizeDeleteTick(resize, retire);
    else if (resize.stage == ResizeStage::Create)
        ResizeCreateTick(resize);
}

void PrintResize(ChatHandler* handler, uint32 now)
{
    Resize const& resize = GetResize();
    if (resize.stage == ResizeStage::Idle)
    {
        handler->PSendSysMessage("Resize: idle; last: {}", resize.last.empty() ? "none since the start" : resize.last);
        return;
    }

    handler->PSendSysMessage(
        "Resize: roster {} -> {}{}, {} for {} s: {} deleted, {} created, {} skipped, {} deletes and {} creations to "
        "go; "
        "plan: {}",
        resize.from, resize.target, resize.planOnly ? " (plan only)" : "", ResizeStageName(resize.stage),
        now - resize.startedAt, resize.deleted, resize.created, resize.skipped, resize.deletes.size(),
        resize.adds.size(), resize.plan.empty() ? "not yet" : resize.plan);
}

// The hourly summary: the window's retires and creations and the bot accounts' character count (one async query)
void StartRetireSummary(Retire& retire, uint32 now)
{
    RetireTotals const totals = TotalsOf(retire);
    RetireTotals const& base = retire.summaryBase;
    std::string const text = Acore::StringFormat(
        "retired {}, created {}, create gave up {}, refused {}, owed {}{}, roster {}", totals.retired - base.retired,
        totals.created - base.created, totals.gaveUp - base.gaveUp, totals.refused - base.refused,
        retire.creates.owed.size(), retire.creates.paused ? " (paused)" : "", GetRotation().roster.Size());
    uint32 const minutes = (now - retire.lastSummary) / 60;
    retire.summaryBase = totals;
    retire.lastSummary = now;
    retire.nextSummary = now + RetireSummarySeconds;
    std::vector<uint32> const& accounts = sPlayerbotAIConfig.RandomBotAccounts;
    if (accounts.empty())
    {
        LOG_INFO("playerbots", "[Population] retire summary (last {} min): {}, characters ?", minutes, text);
        return;
    }

    retire.counting = true;
    retire.queries.AddCallback(
        CharacterDatabase
            .AsyncQuery(Acore::StringFormat(
                "SELECT COUNT(*) FROM characters WHERE account IN ({}) AND deleteDate IS NULL", JoinIds(accounts)))
            .WithCallback(
                [minutes, text](QueryResult result)
                {
                    GetRetire().counting = false;
                    LOG_INFO("playerbots", "[Population] retire summary (last {} min): {}, characters {}", minutes,
                             text, result ? std::to_string(result->Fetch()[0].Get<uint64>()) : std::string("?"));
                }));
}

// What a pass retires from, as text
std::string RetireRangesText(Retire const& retire)
{
    return retire.testTarget ? Acore::StringFormat("the top level down to {} bots", retire.testTarget)
                             : std::string("no top-level target, nothing is retired");
}

// `on`: the population follows the targets; `wasOn`: it did before this load
void LoadRetireConfig(bool reload, bool wasOn, bool on)
{
    Retire& retire = GetRetire();
    retire.intervalSeconds =
        std::max<uint32>(10, sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.IntervalSeconds", 600));
    retire.maxPerPass = sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.MaxPerPass", 1);
    retire.maxPerHour = sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.MaxPerHour", 10);
    retire.testTarget = sConfigMgr->GetOption<uint32>("Playerbots.Population.Retire.TestTarget", 0);

    // The live safeguard: the first pass after a start, or after the role value is switched on, waits one interval
    if (on && !wasOn)
        retire.nextPass = Now() + retire.intervalSeconds;

    retire.prepared = false;
    retire.refusedLogged.clear();
    retire.creates.paused = false;  // a reload resumes the retires a given-up replacement paused
    retire.lastSummary = Now();
    retire.nextSummary = retire.lastSummary + RetireSummarySeconds;
    retire.summaryBase = TotalsOf(retire);
    if (!reload)
        retire.seed = static_cast<uint32>(time(nullptr));

    if (on)
        LOG_INFO("playerbots",
                 "[Population] {}retire (deletes and replaces characters): every {} s, at most {} per pass and {} per "
                 "hour; {}",
                 reload ? "reload: " : "", retire.intervalSeconds, retire.maxPerPass, retire.maxPerHour,
                 RetireRangesText(retire));
}

void PrintRetire(ChatHandler* handler, uint32 now)
{
    Retire& retire = GetRetire();
    std::string const owed = retire.creates.owed.empty()
                                 ? std::string()
                                 : Acore::StringFormat("; {} replacements still owed", retire.creates.owed.size());
    if (!s_followsTargets.load(std::memory_order_relaxed))
    {
        handler->PSendSysMessage("Retire: off{}", owed);
        return;
    }

    uint32 const left = PopulationPolicy::RetireHourLeft(retire.recent, now, retire.maxPerHour);
    handler->PSendSysMessage("Retire: on; every {} s, at most {} per pass and {} per hour ({} left); {}",
                             retire.intervalSeconds, retire.maxPerPass, retire.maxPerHour, left,
                             RetireRangesText(retire));
    handler->PSendSysMessage(
        "Retire totals: passes {}, retired {}, known bots refused {}; created {}, create failed {}, owed {}; last "
        "pass {}, {} bots in range; {} + {} rows per bot ({}), {} clone accounts",
        retire.passes, retire.retired, retire.refused, retire.creates.created, retire.creates.gaveUp,
        retire.creates.owed.size(),
        retire.lastPass ? Acore::StringFormat("{} s ago", now - retire.lastPass) : std::string("none"),
        retire.lastInRange, retire.characterRows, retire.playerbotsRows, retire.prepared ? "prepared" : "not prepared",
        retire.cloneAccounts.size());
    handler->PSendSysMessage(
        "Create: {} tries per replacement, {} failed attempts{}; {} ms per creation on average, "
        "{} at most",
        RetireCreateAttempts, retire.creates.failures,
        retire.creates.paused ? ", retires paused until .reload config" : "",
        retire.made ? retire.createMsTotal / retire.made : 0, retire.createMsMax);
    for (RetireRecord const& record : retire.last)
        handler->PSendSysMessage("  retired {} (#{}), level {}, {} s ago", record.name, record.guid, record.level,
                                 now - record.at);

    for (RetireRecord const& record : retire.lastCreated)
        handler->PSendSysMessage("  created {} (#{}), level {}, {} s ago", record.name, record.guid, record.level,
                                 now - record.at);
}
}  // namespace

PopulationPolicy::LogoutFacts Population::LogoutFactsOf(Player* bot)
{
    PopulationPolicy::LogoutFacts facts;
    facts.inWorld = bot->IsInWorld();
    facts.teleporting = bot->IsBeingTeleported();
    facts.inFlight = bot->IsInFlight();
    facts.inCombat = bot->IsInCombat();
    facts.grouped = bot->GetGroup() != nullptr;
    facts.inBgOrArena = bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue();
    facts.inBattlefield = bot->InBattlefield();
    facts.lfgActive = sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE;
    // Logged in over the cap by a ladder move (PvP gear comes off before a depart), or its team arrives or departs
    for (uint8 slot = 0; slot < MAX_ARENA_SLOT && !facts.ladderMoving; ++slot)
    {
        uint32 const teamId = bot->GetArenaTeamId(slot);
        facts.ladderMoving = teamId && LadderMgr::Instance().IsMoving(teamId);
    }

    Map* map = bot->FindMap();
    facts.onInstanceableMap = map && map->Instanceable();
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    facts.gameClientMaster = botAI && botAI->HasGameClientMaster();
    return facts;
}

bool Population::IsRotation() { return s_followsTargets.load(std::memory_order_relaxed); }

void Population::LoadConfig(bool reload)
{
    BotKind::LoadLivingWorld();

    LOG_INFO("playerbots", "[Population] {}living world {}; random bot levels never go down", reload ? "reload: " : "",
             BotKind::LivingWorld() ? "on" : "off");

    // Read from sConfigMgr, not PlayerbotAIConfig: on ".reload config" the level-management values there are
    // reloaded by another script whose order against this one is not fixed.
    std::vector<char const*> const refused =
        PopulationPolicy::RefusedOptions(sConfigMgr->GetOption<bool>("Playerbots.DowngradeMaxLevelBot", false),
                                         sConfigMgr->GetOption<bool>("Playerbots.ResetBotLevel.Enabled", false),
                                         sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.Enabled", false));
    if (!refused.empty())
    {
        std::string names;
        for (char const* option : refused)
            names.append(names.empty() ? "" : ", ").append(option);
        LOG_WARN("playerbots",
                 "[Population] {} = 1 ignored: bot levels never go down, so the level brackets and the level reset "
                 "never run; set it to 0",
                 names);
    }

    Rotation& rotation = GetRotation();
    PopulationPolicy::RotationSettings& settings = rotation.settings;
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
    rotation.acquaintedMinScore =
        std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.AcquaintedMinScore", 1));
    rotation.nextTick = 0;
    if (!reload)
        rotation.schedule.SetSeed(static_cast<uint32>(time(nullptr)));

    bool const wasOn = s_followsTargets.load(std::memory_order_relaxed);
    bool const on = BotKind::FollowsTargets();
    s_followsTargets.store(on, std::memory_order_relaxed);
    if (on)
        LOG_INFO("playerbots",
                 "[Population] {}Playerbots.Population.FollowTargets = 1: interval {} s, mean session {} min, min "
                 "session {} min, acquainted share {} % (score >= {})",
                 reload ? "reload: " : "", settings.intervalSeconds, settings.meanSessionSeconds / 60,
                 settings.minSessionSeconds / 60, settings.acquaintedSharePct, rotation.acquaintedMinScore);
    else
        LOG_INFO("playerbots", "[Population] {}{}: no rotation, no retirement", reload ? "reload: " : "",
                 BotKind::LivingWorld() ? "Playerbots.Population.FollowTargets = 0"
                                        : "Playerbots.Population.FollowTargets is off outside a living world");

    LoadRetireConfig(reload, wasOn, on);
}

uint32 Population::GuardLevel(Player* bot, uint32 requestedLevel, char const* site)
{
    if (!bot)
        return requestedLevel;

    uint32 const currentLevel = bot->GetLevel();
    if (PopulationPolicy::KeepLevel(currentLevel, requestedLevel) == requestedLevel)
        return requestedLevel;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return requestedLevel;

    LOG_WARN("playerbots", "[Population] {}: refused to lower {} from level {} to {}, kept {}", site, bot->GetName(),
             currentLevel, requestedLevel, currentLevel);

    return currentLevel;
}

uint32 Population::RosterBotCount(uint32 upstreamCount)
{
    if (!BotKind::FollowsTargets())
        return upstreamCount;

    return std::max(upstreamCount, sConfigMgr->GetOption<uint32>(RosterSizeKey, DefaultRosterSize));
}

uint32 Population::RosterAccountBotCount(uint32 upstreamCount)
{
    uint32 const roster = RosterBotCount(upstreamCount);
    return BotKind::FollowsTargets() ? PopulationPolicy::RosterAccountBots(roster, RosterCharactersFound()) : roster;
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
    EnsureRoster(rotation, accounts);

    rotation.acquainted.clear();
    AcquaintanceMgr::Instance().CollectBots(rotation.acquaintedMinScore, rotation.acquainted);

    bool const noDeathKnights = sPlayerbotAIConfig.DisableDeathKnightLogin;
    LadderMgr const& ladder = LadderMgr::Instance();
    rotation.entries.clear();
    rotation.entries.reserve(rotation.roster.Size());
    for (PopulationRoster::Entry const& bot : rotation.roster.Entries())
    {
        PopulationPolicy::RotationEntry entry;
        entry.guid = bot.guid;
        entry.team = Player::TeamIdForRace(bot.race) == TEAM_ALLIANCE ? 0 : 1;
        entry.acquainted = rotation.acquainted.contains(bot.guid);
        entry.blocked = ladder.IsAway(bot.guid) || (noDeathKnights && bot.classId == CLASS_DEATH_KNIGHT);
        entry.online = online.contains(bot.guid);
        entry.pinned = entry.online && IsPinned(sRandomPlayerbotMgr.GetPlayerBot(bot.guid));
        rotation.entries.push_back(entry);
    }

    rotation.settings.cap = cap;
    rotation.last = rotation.schedule.Tick(now, rotation.entries, rotation.settings);
    rotation.lastTick = now;
    ++rotation.ticks;
    swaps = rotation.last.swaps;

    // Online bots outside the roster (accounts changed) go within the budget; the login fill
    // takes roster bots in their place
    rotation.lastExtra = 0;
    if (!rotation.roster.Empty())
        for (uint32 const guid : online)
        {
            if (rotation.lastExtra >= std::max<uint32>(1, rotation.last.budget))
                break;

            if (rotation.roster.Contains(guid))
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
             rotation.roster.Size(), rotation.last.eligible, rotation.last.blocked, rotation.last.acquainted,
             rotation.last.shares.acquainted, rotation.last.shares.other);

    return swaps;
}

bool Population::InRoster(uint32 guid)
{
    Rotation const& rotation = GetRotation();
    return rotation.roster.Empty() || rotation.roster.Contains(guid);
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

std::vector<PopulationRoster::Entry> const& Population::Roster(std::vector<uint32> const& accounts)
{
    Rotation& rotation = GetRotation();
    EnsureRoster(rotation, accounts);
    return rotation.roster.Entries();
}

void Population::ReloadRoster() { s_rosterReloads.fetch_add(1, std::memory_order_relaxed); }

void Population::RemoveFromRoster(uint32 guid)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Population::RemoveFromRoster");
    Rotation& rotation = GetRotation();
    rotation.roster.Erase(guid);
    rotation.schedule.Forget(guid);
}

bool Population::IsRetire() { return s_followsTargets.load(std::memory_order_relaxed); }

void Population::RetireTick(std::unordered_set<uint32> const& online)
{
    Retire& retire = GetRetire();
    Resize& resize = GetResize();
    // Owed replacements are made even after Retire is switched off: their bots are deleted already
    if (!retire.busy && !retire.counting && retire.creates.owed.empty() && resize.stage == ResizeStage::Idle &&
        !resize.confirming && !IsRetire())
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("Population::RetireTick");
    retire.online = &online;
    retire.queries.ProcessReadyCallbacks();
    if (resize.stage != ResizeStage::Idle)
        ResizeTick(resize, retire);

    retire.online = nullptr;
    uint32 const now = Now();
    if (!retire.creates.owed.empty())
        CreateOwedBots(retire, now);

    if (!IsRetire())
        return;

    if (!retire.counting && now >= retire.nextSummary)
        StartRetireSummary(retire, now);

    // No pass while a resize runs: both would pick from the same offline bots
    if (retire.busy || now < retire.nextPass || resize.stage != ResizeStage::Idle)
        return;

    retire.nextPass = now + retire.intervalSeconds;
    if (!retire.prepared)
        StartRetirePrepare(retire);
    else
        StartRetirePass(retire, now);
}

void Population::NoteEnumeration(uint64 micros, uint32 characters)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Population::NoteEnumeration");
    ++enumerationCost.calls;
    enumerationCost.micros += micros;
    enumerationCost.maxMicros = std::max(enumerationCost.maxMicros, micros);
    enumerationCost.characters = characters;

    // a ramp of 3000 bots takes dozens of passes: a line every few of them, not only when the ramp ends
    if (enumerationCost.calls >= 10)
        FlushEnumeration();
}

void Population::FlushEnumeration()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Population::FlushEnumeration");
    if (!enumerationCost.calls)
        return;

    LOG_INFO("playerbots", "[Population] {}", EnumerationLine());
    enumerationCost.calls = 0;
    enumerationCost.micros = 0;
    enumerationCost.maxMicros = 0;
    enumerationCost.characters = 0;
}

bool Population::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    Rotation const& rotation = GetRotation();
    PopulationPolicy::RotationSettings const& settings = rotation.settings;
    PopulationPolicy::RotationTickResult const& last = rotation.last;
    uint32 const now = Now();

    handler->PSendSysMessage("Population: living world {}, rotation {}; roster {} characters, cap {}",
                             BotKind::LivingWorld() ? "on" : "off", IsRotation() ? "on" : "off", rotation.roster.Size(),
                             settings.cap);
    handler->PSendSysMessage(
        "Settings: interval {} s, mean session {} min, min session {} min, acquainted share {} % (score >= {})",
        settings.intervalSeconds, settings.meanSessionSeconds / 60, settings.minSessionSeconds / 60,
        settings.acquaintedSharePct, rotation.acquaintedMinScore);
    handler->PSendSysMessage("[Population] {}", EnumerationLine());
    PrintRetire(handler, now);
    PrintResize(handler, now);
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

bool Population::HandleResizeCommand(ChatHandler* handler, char const* args)
{
    std::string const text = args ? args : "";
    if (text.empty())
    {
        PrintResize(handler, Now());
        return true;
    }

    size_t const space = text.find(' ');
    std::string const count = text.substr(0, space);
    bool const planOnly = space != std::string::npos && text.substr(space + 1) == "plan";
    if (count.empty() || count.size() > 6 || count.find_first_not_of("0123456789") != std::string::npos ||
        (space != std::string::npos && !planOnly))
    {
        handler->SendSysMessage(
            "Usage: .playerbots population resize [<count> [plan]]: the roster's new number of "
            "characters; plan only logs the plan");
        return false;
    }

    if (!IsRotation())
    {
        handler->SendSysMessage("Resize: needs Playerbots.Population.FollowTargets = 1");
        return false;
    }

    Resize& resize = GetResize();
    if (resize.stage != ResizeStage::Idle)
    {
        PrintResize(handler, Now());
        handler->SendSysMessage("Resize: one runs already; ask again when it is done");
        return false;
    }

    if (GetRotation().accounts.empty())
    {
        handler->SendSysMessage("Resize: the rotation has not loaded its roster yet; ask again after its first tick");
        return false;
    }

    // The last resize's confirm queries may still be in flight: their callbacks count `confirming` down
    std::string const last = resize.last;
    uint32 const confirming = resize.confirming;
    resize = Resize();
    resize.last = last;
    resize.confirming = confirming;
    resize.stage = ResizeStage::Plan;
    resize.target = static_cast<uint32>(std::stoul(count));
    resize.planOnly = planOnly;
    resize.startedAt = Now();
    handler->PSendSysMessage(
        "Resize: roster toward {} characters{}; the plan and the result go to Playerbots.log ([Population] resize)",
        resize.target, planOnly ? " (plan only)" : "");
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
