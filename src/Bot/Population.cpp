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
#include "LFGMgr.h"
#include "LadderMgr.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PopulationPolicy.h"
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
             "XP multiplier {}-{} %, deadband {} %, gain {} %, response {} h, logout preference {} %",
             reload ? "reload: " : "", BalanceEnableKey, balance.ranges.size(),
             PopulationPolicy::BalanceRangeSize(settings), settings.maxLevel, settings.maxLevelWeightPct,
             balance.intervalSeconds / 60, settings.minMultiplierPct, settings.maxMultiplierPct, settings.deadbandPct,
             settings.gainPct, settings.responseSeconds / 3600, settings.preferencePct);
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
