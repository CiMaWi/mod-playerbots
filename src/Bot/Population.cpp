/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Population.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <string>
#include <unordered_set>
#include <vector>

#include "AcquaintanceMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "LadderMgr.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PopulationPolicy.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "ThreadCheck.h"

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

    Map* map = bot->GetMap();
    return map && map->Instanceable();
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

    Rotation& rotation = GetRotation();
    PopulationPolicy::RotationSettings& settings = rotation.settings;
    bool const rotationOn = sConfigMgr->GetOption<bool>(RotationEnableKey, false);
    settings.intervalSeconds =
        std::max<uint32>(10, sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.IntervalSeconds", 300));
    settings.meanSessionSeconds =
        60 * std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Playerbots.Population.Rotation.MeanSessionMinutes", 120));
    settings.minSessionSeconds = std::min<uint32>(
        settings.meanSessionSeconds,
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

            if (rotation.rosterGuids.contains(guid) || IsPinned(sRandomPlayerbotMgr.GetPlayerBot(guid)))
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
    uint32 const checks = PopulationPolicy::RotationSelftest(failures, false);
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
