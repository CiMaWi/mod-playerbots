/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_POPULATIONPOLICY_H
#define _PLAYERBOT_POPULATIONPOLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Pure rules of the persistent population (Playerbots.Population.*), no game types, so a standalone
// harness can check them. Population.cpp and the call sites apply them to live bots.
namespace PopulationPolicy
{
// Why persistence turned a randomize into an incremental one at the bot's current level.
enum class KeptReason : uint8_t
{
    None,      // nothing kept: the upstream rule would not have changed the level either
    Reroll,    // upstream rerolls bots below level 3 (Death Knights below 56) to a random level
    Downgrade  // upstream sends a max-level bot to the min level (Playerbots.DowngradeMaxLevelBot)
};

struct RandomizeDecision
{
    bool first;       // true: RandomizeFirst (full reroll at a new level); false: incremental at the current level
    KeptReason kept;  // only set when persistent
};

// RandomPlayerbotMgr::Randomize's choice. Off: upstream's rule unchanged. On: always incremental at the current
// level, so no bot is rerolled (also not below level 3) and no max-level bot is downgraded.
inline RandomizeDecision DecideRandomize(bool livingWorld, uint32_t level, bool deathKnight, uint32_t maxLevel,
                                         bool downgradeMaxLevelBot)
{
    bool const reroll = level < 3 || (level < 56 && deathKnight);
    bool const downgrade = !reroll && level >= maxLevel && downgradeMaxLevelBot;

    if (!livingWorld)
        return {reroll || downgrade, KeptReason::None};

    return {false, reroll ? KeptReason::Reroll : (downgrade ? KeptReason::Downgrade : KeptReason::None)};
}

// RandomizeFirst's level: the rolled one, or the bot's current level in a living world (a re-initialisation at the
// same level; a new character so stays at its creation level).
inline uint32_t FirstLevel(bool livingWorld, uint32_t currentLevel, uint32_t rolledLevel)
{
    return livingWorld ? currentLevel : rolledLevel;
}

// The level a factory or reset path may set: never below the current one.
inline uint32_t KeepLevel(uint32_t currentLevel, uint32_t requestedLevel)
{
    return requestedLevel < currentLevel ? currentLevel : requestedLevel;
}

// The upstream level-lowering options our fork ignores, those of them that are set, for the startup and reload warning.
inline std::vector<char const*> RefusedOptions(bool downgradeMaxLevelBot, bool resetBotLevel, bool levelBrackets)
{
    std::vector<char const*> refused;
    if (downgradeMaxLevelBot)
        refused.push_back("Playerbots.DowngradeMaxLevelBot");
    if (resetBotLevel)
        refused.push_back("Playerbots.ResetBotLevel.Enabled");
    if (levelBrackets)
        refused.push_back("Playerbots.LevelBrackets.Enabled");

    return refused;
}

// H2: a roster above the online cap, rotated by schedule (Playerbots.Population.Rotation.*). Every tick the
// schedule accrues each roster bot's owed online time (its share of the day, minus the time it was online) and swaps
// the most over-served online bots for the most owed offline ones of the same team (only a bot owed time comes in;
// logins and logouts between ticks are noted). Bots a real player knows are owed
// a larger share; blocked bots (away on an arena server) are never chosen and owe nothing.

struct RotationSettings
{
    uint32_t cap = 0;                    // online cap: the random bot manager's bot count
    uint32_t intervalSeconds = 300;      // between two rotation ticks
    uint32_t meanSessionSeconds = 7200;  // swap budget per tick: online x interval / mean session
    uint32_t minSessionSeconds = 1800;   // a rotation swap needs both bots this long in their current state
    uint32_t acquaintedSharePct = 90;    // online share owed to a bot a real player knows, as far as the cap allows
};

struct RotationShares
{
    double acquainted = 0.0;
    double other = 0.0;
};

// The online share each eligible bot is owed. An acquainted bot gets at least the fair share cap / eligible and at
// most cap / acquainted; the others share what is left. Together they fill the cap.
inline RotationShares ComputeShares(uint32_t cap, uint32_t eligible, uint32_t acquainted, uint32_t acquaintedSharePct)
{
    RotationShares shares;
    if (!cap || !eligible)
        return shares;

    if (cap >= eligible)
        return {1.0, 1.0};

    acquainted = std::min(acquainted, eligible);
    double const fair = static_cast<double>(cap) / eligible;
    double acquaintedShare = std::max(fair, std::min(1.0, acquaintedSharePct / 100.0));
    if (acquainted && acquaintedShare * acquainted > cap)
        acquaintedShare = static_cast<double>(cap) / acquainted;

    uint32_t const others = eligible - acquainted;
    double const otherShare = others ? (cap - acquaintedShare * acquainted) / others : 0.0;
    shares.acquainted = acquaintedShare;
    shares.other = std::clamp(otherShare, 0.0, 1.0);
    return shares;
}

// Swaps per tick: about online x interval / mean session, so a session lasts the mean session on average
inline uint32_t RotationBudget(uint32_t online, RotationSettings const& settings)
{
    if (!online || !settings.meanSessionSeconds)
        return 0;

    uint64_t const scaled = static_cast<uint64_t>(online) * settings.intervalSeconds;
    return static_cast<uint32_t>((scaled + settings.meanSessionSeconds - 1) / settings.meanSessionSeconds);
}

// One roster bot as the world thread sees it at a tick
struct RotationEntry
{
    uint32_t guid = 0;
    uint8_t team = 0;         // 0 Alliance, 1 Horde: a swap keeps the team, so the faction ratio stays
    bool acquainted = false;  // a real player knows the bot
    bool blocked = false;     // never logs in (away on an arena server, a class kept offline): no share, never chosen
    bool online = false;      // in the random bot manager's online set
    bool pinned = false;      // online, but not to be logged out now (group, battleground, instance, combat, flight)
    double weight = 1.0;      // weight of an unacquainted bot's share (1.0: all equal)
};

struct RotationSwap
{
    uint32_t out = 0;
    uint32_t in = 0;  // 0: only a logout (the login fill takes the next bot)
};

struct RotationTickResult
{
    std::vector<RotationSwap> swaps;
    RotationShares shares;
    uint32_t eligible = 0;
    uint32_t blocked = 0;
    uint32_t acquainted = 0;  // eligible ones
    uint32_t online = 0;      // eligible ones
    uint32_t budget = 0;
};

class RotationSchedule
{
public:
    // Login order: bots past their minimum offline time first, then the most owed, then a seeded hash of the guid (so a
    // restart does not always start with the same bots)
    struct LoginKey
    {
        bool ready = true;
        double credit = 0.0;
        uint32_t tie = 0;
    };

    explicit RotationSchedule(uint32_t seed = 0) : _seed(seed) {}

    void SetSeed(uint32_t seed) { _seed = seed; }

    // Accrues the owed online time since the last tick, then picks up to the budget of swaps: per team the most
    // over-served online bot (not pinned) against the most owed offline one, while the one coming in is owed more.
    // Both must have spent minSessionSeconds in their state. The caller applies every swap before the next tick.
    RotationTickResult Tick(uint32_t now, std::vector<RotationEntry> const& roster, RotationSettings const& settings)
    {
        RotationTickResult result;
        uint32_t const elapsed = (_lastTick && now > _lastTick) ? now - _lastTick : 0;
        _lastTick = now;

        for (RotationEntry const& entry : roster)
        {
            if (entry.blocked)
            {
                ++result.blocked;
                continue;
            }

            ++result.eligible;
            if (entry.acquainted)
                ++result.acquainted;
            if (entry.online)
                ++result.online;
        }

        result.shares = ComputeShares(settings.cap, result.eligible, result.acquainted, settings.acquaintedSharePct);
        double const limit = static_cast<double>(std::max(settings.meanSessionSeconds, settings.intervalSeconds));

        // The unacquainted bots' shares weighted, normalised so that together
        // they still fill what the acquainted ones leave (all weights 1: the shares above, unchanged)
        double weightSum = 0.0;
        uint32_t weighted = 0;
        for (RotationEntry const& entry : roster)
            if (!entry.blocked && !entry.acquainted)
            {
                weightSum += std::max(0.0, entry.weight);
                ++weighted;
            }
        double const weightNorm = weightSum > 0.0 ? weighted / weightSum : 1.0;

        std::vector<Candidate> outs[2];
        std::vector<Candidate> ins[2];
        for (RotationEntry const& entry : roster)
        {
            double const share = entry.acquainted
                                     ? result.shares.acquainted
                                     : std::min(1.0, result.shares.other * std::max(0.0, entry.weight) * weightNorm);
            auto const [it, added] = _state.try_emplace(entry.guid);
            State& state = it->second;
            if (added)
            {
                // A head start by share, so acquainted bots come first at startup
                state.online = entry.online;
                state.since = entry.online ? now : 0;
                state.credit = entry.blocked ? 0.0 : share * settings.minSessionSeconds;
            }
            else if (!entry.blocked && elapsed)
            {
                state.credit += share * elapsed - (state.online ? static_cast<double>(elapsed) : 0.0);
                state.credit = std::clamp(state.credit, -limit, limit);
            }

            if (state.online != entry.online)
            {
                state.online = entry.online;
                state.since = now;
            }

            if (entry.blocked || !Ready(state, now, settings.minSessionSeconds))
                continue;

            uint8_t const team = entry.team ? 1 : 0;
            if (!entry.online)
            {
                if (state.credit > 0.0)
                    ins[team].push_back({entry.guid, state.credit, Mix(entry.guid)});
            }
            else if (!entry.pinned)
                outs[team].push_back({entry.guid, state.credit, Mix(entry.guid)});
        }

        result.budget = RotationBudget(result.online, settings);
        for (uint8_t team = 0; team < 2; ++team)
        {
            std::sort(outs[team].begin(), outs[team].end(), [](Candidate const& a, Candidate const& b)
                      { return a.credit != b.credit ? a.credit < b.credit : a.tie < b.tie; });
            std::sort(ins[team].begin(), ins[team].end(), [](Candidate const& a, Candidate const& b)
                      { return a.credit != b.credit ? a.credit > b.credit : a.tie < b.tie; });
        }

        size_t next[2] = {0, 0};
        while (result.swaps.size() < result.budget)
        {
            int best = -1;
            double bestGain = 0.0;
            for (uint8_t team = 0; team < 2; ++team)
            {
                size_t const i = next[team];
                if (i >= outs[team].size() || i >= ins[team].size())
                    continue;

                double const gain = ins[team][i].credit - outs[team][i].credit;
                if (gain > bestGain)
                {
                    bestGain = gain;
                    best = team;
                }
            }

            if (best < 0)
                break;

            size_t const i = next[best]++;
            RotationSwap const swap{outs[best][i].guid, ins[best][i].guid};
            State& out = _state[swap.out];
            out.online = false;
            out.since = now;
            State& in = _state[swap.in];
            in.online = true;
            in.since = now;
            result.swaps.push_back(swap);
        }

        return result;
    }

    // A login or logout between ticks (the login fill, another path): starts the bot's minimum time in its state
    void Note(uint32_t guid, bool online, uint32_t now)
    {
        State& state = _state[guid];
        if (state.online == online)
            return;

        state.online = online;
        state.since = now;
    }

    LoginKey Key(uint32_t guid, uint32_t now, uint32_t minSessionSeconds) const
    {
        LoginKey key;
        key.tie = Mix(guid);
        auto const it = _state.find(guid);
        if (it == _state.end())
            return key;

        key.ready = Ready(it->second, now, minSessionSeconds);
        key.credit = it->second.credit;
        return key;
    }

    static bool Before(LoginKey const& a, LoginKey const& b)
    {
        if (a.ready != b.ready)
            return a.ready;
        if (a.credit != b.credit)
            return a.credit > b.credit;
        return a.tie < b.tie;
    }

    // The offline bots that may log in, in login order (for filling free online slots)
    std::vector<uint32_t> LoginOrder(uint32_t now, std::vector<RotationEntry> const& roster,
                                     RotationSettings const& settings) const
    {
        std::vector<std::pair<LoginKey, uint32_t>> keyed;
        for (RotationEntry const& entry : roster)
            if (!entry.blocked && !entry.online)
                keyed.emplace_back(Key(entry.guid, now, settings.minSessionSeconds), entry.guid);

        std::sort(keyed.begin(), keyed.end(),
                  [](std::pair<LoginKey, uint32_t> const& a, std::pair<LoginKey, uint32_t> const& b)
                  { return Before(a.first, b.first); });

        std::vector<uint32_t> order;
        order.reserve(keyed.size());
        for (auto const& [key, guid] : keyed)
            order.push_back(guid);

        return order;
    }

    // Owed online seconds (negative: over-served); 0 for a bot the schedule has not seen
    double Credit(uint32_t guid) const
    {
        auto const it = _state.find(guid);
        return it == _state.end() ? 0.0 : it->second.credit;
    }

    // A retired bot (H4): its state goes
    void Forget(uint32_t guid) { _state.erase(guid); }

    void Clear()
    {
        _state.clear();
        _lastTick = 0;
    }

    size_t Size() const { return _state.size(); }

private:
    struct State
    {
        double credit = 0.0;
        uint32_t since = 0;  // last login or logout; 0 = offline since before the schedule saw it
        bool online = false;
    };

    struct Candidate
    {
        uint32_t guid;
        double credit;
        uint32_t tie;
    };

    static bool Ready(State const& state, uint32_t now, uint32_t minSessionSeconds)
    {
        return !state.since || now >= state.since + minSessionSeconds;
    }

    uint32_t Mix(uint32_t guid) const
    {
        uint32_t x = guid ^ _seed;
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    std::unordered_map<uint32_t, State> _state;
    uint32_t _lastTick = 0;
    uint32_t _seed = 0;
};

// H4a: retiring never-met random bots (Playerbots.Population.Retire.*). In a level range persistently over its target
// (H3), offline roster bots no real player knows are deleted: acquaintance score at most maxScore (0: never met;
// DH.2), in no guild with a real player, on no real player's friend list, in no arena team, in no group,
// no guild leader, not in a ladder roster, not away on an arena server and not on a lab clone account. At most a few
// per pass and per hour, never so many that a range falls below its target (DH.3).

enum RetireProtect : uint32_t
{
    RetireOnline = 1u << 0,         // online, logging in, or in the manager's online set
    RetireAway = 1u << 1,           // away on an arena server (LadderMgr::IsAway)
    RetireGuildWithReal = 1u << 2,  // in a guild with a real player's character
    RetireFriendOfReal = 1u << 3,   // on a real player's friend list
    RetireArenaTeam = 1u << 4,      // in any arena team (no team is disbanded)
    RetireGuildLeader = 1u << 5,    // leads a guild
    RetireLadderRoster = 1u << 6,   // named in playerbots_ladder_roster
    RetireGrouped = 1u << 7,        // in a group (saved groups outlive a logout)
    RetireClone = 1u << 8,          // on a lab-bots clone account, or no account known
};

constexpr uint32_t RetireProtectBits = 9;

// One bot the pass looks at: its level, its highest acquaintance score with a real player and its protections
struct RetireCandidate
{
    uint32_t guid = 0;
    uint32_t level = 0;
    uint32_t score = 0;
    uint32_t protect = 0;  // RetireProtect bits
};

// A level range the pass may retire from: its levels, the roster's bots in it now and its target
struct RetireRange
{
    uint32_t firstLevel = 0;
    uint32_t lastLevel = 0;
    uint32_t count = 0;
    double target = 0.0;
};

// How many bots a range may lose without falling below its target
inline uint32_t RetireSurplus(uint32_t count, double target)
{
    double const keep = std::ceil(std::max(0.0, target));
    return count > keep ? count - static_cast<uint32_t>(keep) : 0;
}

// Playerbots.Population.Retire.TestTarget (a test toggle): the one range it names is the top level, retired from down
// to `testTarget` bots. 0 names no range, so it retires nothing.
inline std::vector<RetireRange> TopLevelRetireRanges(uint32_t maxLevel, uint32_t testTarget)
{
    if (!testTarget)
        return {};

    return {{maxLevel, maxLevel, 0, static_cast<double>(testTarget)}};
}

// The retires this sliding hour may still take: drops the times an hour old or more from `times`
inline uint32_t RetireHourLeft(std::vector<uint32_t>& times, uint32_t now, uint32_t maxPerHour)
{
    times.erase(std::remove_if(times.begin(), times.end(), [now](uint32_t t) { return now >= t + 3600; }), times.end());
    return maxPerHour > times.size() ? maxPerHour - static_cast<uint32_t>(times.size()) : 0;
}

// A seeded order among bots of equal score (a 32-bit mix of guid and seed): deterministic per seed
inline uint32_t RetireOrderKey(uint32_t guid, uint32_t seed)
{
    uint32_t x = guid ^ (seed * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// The bots to retire: unprotected, score at most maxScore, level in one of `ranges`; lowest score first, then the
// seeded order; at most maxCount, and with bySurplus no more from a range than its surplus. No automatic fallback to
// higher scores: maxScore is the only knob (DH.2). bySurplus false picks the pass's candidate batch (checked further
// before the final pick).
inline std::vector<uint32_t> SelectRetirees(std::vector<RetireCandidate> const& candidates,
                                            std::vector<RetireRange> const& ranges, uint32_t maxScore,
                                            uint32_t maxCount, uint32_t seed, bool bySurplus = true)
{
    struct Pick
    {
        uint32_t score;
        uint32_t key;
        uint32_t guid;
        size_t range;
    };

    std::vector<Pick> picks;
    for (RetireCandidate const& candidate : candidates)
    {
        if (!candidate.guid || candidate.protect || candidate.score > maxScore)
            continue;

        for (size_t i = 0; i < ranges.size(); ++i)
            if (candidate.level >= ranges[i].firstLevel && candidate.level <= ranges[i].lastLevel)
            {
                picks.push_back({candidate.score, RetireOrderKey(candidate.guid, seed), candidate.guid, i});
                break;
            }
    }

    std::sort(picks.begin(), picks.end(),
              [](Pick const& a, Pick const& b)
              {
                  if (a.score != b.score)
                      return a.score < b.score;

                  return a.key != b.key ? a.key < b.key : a.guid < b.guid;
              });

    std::vector<uint32_t> surplus(ranges.size(), UINT32_MAX);
    if (bySurplus)
        for (size_t i = 0; i < ranges.size(); ++i)
            surplus[i] = RetireSurplus(ranges[i].count, ranges[i].target);

    std::vector<uint32_t> chosen;
    for (Pick const& pick : picks)
    {
        if (chosen.size() >= maxCount)
            break;

        if (!surplus[pick.range] || std::find(chosen.begin(), chosen.end(), pick.guid) != chosen.end())
            continue;

        --surplus[pick.range];
        chosen.push_back(pick.guid);
    }

    return chosen;
}

// The protections as text (log lines): "guild|friend|...", "score" when only the score protects
inline std::string RetireReasons(uint32_t protect, uint32_t score, uint32_t maxScore)
{
    static char const* const names[RetireProtectBits] = {"online", "away",   "guild", "friend", "arena",
                                                         "leader", "ladder", "group", "clone"};
    std::string text;
    for (uint32_t bit = 0; bit < RetireProtectBits; ++bit)
        if (protect & (1u << bit))
            text += std::string(text.empty() ? "" : "|") + names[bit];

    if (score > maxScore)
        text += std::string(text.empty() ? "" : "|") + "score";

    return text;
}

// H4b: a live retire owes a new bot of the retired bot's class on its account (the core's delete keeps the account,
// so it has a free slot). The entry stays until the new character's row is confirmed or the replacement is given up.
struct CreateEntry
{
    uint32_t retiredGuid = 0;
    uint32_t account = 0;
    uint32_t classId = 0;
    uint32_t attempts = 0;     // failed creations and confirms
    uint32_t notBefore = 0;    // game time of the next try
    uint32_t createdGuid = 0;  // the new character while its row is unconfirmed: a retry confirms, never creates again
    bool confirming = false;   // its confirm query is in flight
    std::string retiredName;
};

enum class CreateOutcome
{
    Created,    // the character was made and saved: its row is confirmed next
    Failed,     // the factory made none: create again later
    Confirmed,  // its row is in the DB
    Missing,    // its row is not in the DB: confirm again later
};

enum class CreateDecision
{
    Unknown,  // no such entry
    Confirm,
    Retry,
    GaveUp,
    Done,
};

struct CreateAccounting
{
    std::vector<CreateEntry> owed;  // oldest first
    uint64_t created = 0;
    uint64_t gaveUp = 0;
    uint64_t failures = 0;  // failed attempts, given up or not
    bool paused = false;    // a replacement was given up: no retire until a reload (the caller clears it)
};

// No retire pass while a replacement is owed or after one was given up: the roster is short by at most one pass's
// retires, and a factory that fails costs no more than that
inline bool RetireGate(CreateAccounting const& accounting) { return accounting.owed.empty() && !accounting.paused; }

inline void CreateOwe(CreateAccounting& accounting, uint32_t retiredGuid, uint32_t account, uint32_t classId,
                      uint32_t now, std::string const& retiredName = std::string())
{
    CreateEntry entry;
    entry.retiredGuid = retiredGuid;
    entry.account = account;
    entry.classId = classId;
    entry.notBefore = now;
    entry.retiredName = retiredName;
    accounting.owed.push_back(entry);
}

inline CreateEntry const* CreateFind(CreateAccounting const& accounting, uint32_t retiredGuid)
{
    for (CreateEntry const& entry : accounting.owed)
        if (entry.retiredGuid == retiredGuid)
            return &entry;

    return nullptr;
}

// The entries to work on now, oldest first, at least one and at most maxPerCall: none in flight, none waiting
inline std::vector<uint32_t> CreateDue(CreateAccounting const& accounting, uint32_t now, uint32_t maxPerCall)
{
    std::vector<uint32_t> due;
    for (CreateEntry const& entry : accounting.owed)
    {
        if (due.size() >= std::max<uint32_t>(1, maxPerCall))
            break;

        if (!entry.confirming && now >= entry.notBefore)
            due.push_back(entry.retiredGuid);
    }

    return due;
}

// Records an outcome. A failed creation or confirm waits retrySeconds and counts towards maxAttempts; then the
// replacement is given up (the factory refills the account at the next startup) and the retires pause.
inline CreateDecision CreateApply(CreateAccounting& accounting, uint32_t retiredGuid, CreateOutcome outcome,
                                  uint32_t now, uint32_t retrySeconds, uint32_t maxAttempts, uint32_t createdGuid = 0)
{
    auto const it = std::find_if(accounting.owed.begin(), accounting.owed.end(),
                                 [retiredGuid](CreateEntry const& entry) { return entry.retiredGuid == retiredGuid; });
    if (it == accounting.owed.end())
        return CreateDecision::Unknown;

    if (outcome == CreateOutcome::Created)
    {
        it->createdGuid = createdGuid;
        it->confirming = true;
        return CreateDecision::Confirm;
    }

    if (outcome == CreateOutcome::Confirmed)
    {
        ++accounting.created;
        accounting.owed.erase(it);
        return CreateDecision::Done;
    }

    ++accounting.failures;
    it->confirming = false;
    if (++it->attempts >= std::max<uint32_t>(1, maxAttempts))
    {
        ++accounting.gaveUp;
        accounting.paused = true;
        accounting.owed.erase(it);
        return CreateDecision::GaveUp;
    }

    it->notBefore = now + retrySeconds;
    return CreateDecision::Retry;
}

// World-log WL19 (WL.14): the roster's count change (".playerbots population resize"). Level ranges 1-9, 10-19, ...
// up to the level below the top, and the top level alone; the top holds at most ResizeTopSharePct of the roster, the
// other ranges even shares of the rest. Existing bots keep their levels: deletions come from the ranges most over their
// share, additions go to the ranges most under it.
constexpr uint32_t ResizeTopSharePct = 10;
constexpr uint32_t ResizeRangeLevels = 10;

struct ResizeRange
{
    uint32_t firstLevel = 0;
    uint32_t lastLevel = 0;
    uint32_t count = 0;      // roster bots in the range now
    uint32_t removable = 0;  // of them, the ones a deletion may take (offline, unprotected)
    double target = 0.0;     // the range's share of the new roster
    int32_t delta = 0;       // the plan: bots added (> 0) or deleted (< 0)
};

inline std::vector<ResizeRange> ResizeLayout(uint32_t maxLevel)
{
    std::vector<ResizeRange> ranges;
    for (uint32_t first = 1; first < maxLevel; first = (first / ResizeRangeLevels + 1) * ResizeRangeLevels)
    {
        ResizeRange range;
        range.firstLevel = first;
        range.lastLevel = std::min((first / ResizeRangeLevels + 1) * ResizeRangeLevels - 1, maxLevel - 1);
        ranges.push_back(range);
    }

    ResizeRange top;
    top.firstLevel = std::max<uint32_t>(maxLevel, 1);
    top.lastLevel = top.firstLevel;
    ranges.push_back(top);
    return ranges;
}

// The index of a level's range, -1 outside the layout
inline int32_t ResizeRangeOf(std::vector<ResizeRange> const& ranges, uint32_t level)
{
    for (size_t i = 0; i < ranges.size(); ++i)
        if (level >= ranges[i].firstLevel && level <= ranges[i].lastLevel)
            return static_cast<int32_t>(i);

    return -1;
}

inline void ResizeTargets(std::vector<ResizeRange>& ranges, uint32_t roster)
{
    if (ranges.size() < 2)
    {
        for (ResizeRange& range : ranges)
            range.target = roster;

        return;
    }

    double const top = roster * ResizeTopSharePct / 100.0;
    double const other = (roster - top) / static_cast<double>(ranges.size() - 1);
    for (ResizeRange& range : ranges)
        range.target = other;

    ranges.back().target = top;
}

// The plan toward `roster` bots, one bot at a time: a deletion from the range most over its target that still has a
// removable bot, or an addition to the range most under it (the top level only below its share: bots reach it by
// levelling); ties go to the lower range. Returns the planned change (< 0: deletions), short of the asked one when too
// few bots are removable.
inline int32_t ResizePlan(std::vector<ResizeRange>& ranges, uint32_t roster)
{
    uint32_t current = 0;
    for (ResizeRange& range : ranges)
    {
        current += range.count;
        range.delta = 0;
    }

    ResizeTargets(ranges, roster);
    bool const adding = roster > current;
    uint32_t const steps = adding ? roster - current : current - roster;
    int32_t planned = 0;
    for (uint32_t step = 0; step < steps; ++step)
    {
        int32_t best = -1;
        double bestGap = 0.0;
        for (size_t i = 0; i < ranges.size(); ++i)
        {
            ResizeRange const& range = ranges[i];
            double const now = static_cast<double>(range.count) + range.delta;
            double gap = 0.0;
            if (adding)
            {
                gap = range.target - now;
                if (i + 1 == ranges.size() && ranges.size() > 1 && gap <= 0.0)
                    continue;
            }
            else
            {
                if (static_cast<uint32_t>(-range.delta) >= range.removable)
                    continue;

                gap = now - range.target;
            }

            if (best < 0 || gap > bestGap)
            {
                best = static_cast<int32_t>(i);
                bestGap = gap;
            }
        }

        if (best < 0)
            break;

        ranges[best].delta += adding ? 1 : -1;
        planned += adding ? 1 : -1;
    }

    return planned;
}

// A resize counts the roster when it starts, so it waits while a retire query batch runs and while a retire replacement
// is owed (its bot is deleted already, its character not made yet): counted then, the roster would end up with the
// owed characters on top of the count the resize reports
inline bool ResizeMayStart(bool retireBusy, size_t owedReplacements) { return !retireBusy && !owedReplacements; }

// The roster a finished resize reports: what the accounts held when it started, less the deletes and plus the creations
// it made. A fresh DB read right after the last deletes can still count them (the async commit), which made a run say
// 2441 for an asked 2438.
inline uint32_t ResizeDoneCount(uint32_t before, uint32_t deleted, uint32_t created)
{
    return before + created - std::min(before, deleted);
}

// The class of an added bot: the least represented in `classCounts` (indexed by class id) among the bits of
// `allowedMask` (bit classId - 1), the lowest id on ties; 0 when none is allowed
inline uint32_t ResizeClass(std::vector<uint32_t> const& classCounts, uint32_t allowedMask)
{
    uint32_t best = 0;
    for (uint32_t classId = 1; classId < classCounts.size() && classId <= 32; ++classId)
        if ((allowedMask & (1u << (classId - 1))) && (!best || classCounts[classId] < classCounts[best]))
            best = classId;

    return best;
}

// "1-9: 380 -40, ..., 80: 210" (the count before the plan, then the change where there is one)
inline std::string ResizePlanText(std::vector<ResizeRange> const& ranges)
{
    std::string text;
    for (ResizeRange const& range : ranges)
    {
        text += text.empty() ? "" : ", ";
        text += std::to_string(range.firstLevel);
        if (range.lastLevel != range.firstLevel)
            text += "-" + std::to_string(range.lastLevel);

        text += ": " + std::to_string(range.count);
        if (range.delta)
            text += (range.delta > 0 ? " +" : " ") + std::to_string(range.delta);
    }

    return text;
}

// WL19a: the startup factory's refill (RandomPlayerbotFactory::CreateRandomBots). Upstream fills every bot account to
// a full set of characters at every start. Where the population follows the targets (BotKind::FollowsTargets) the
// roster is filled only while it is empty, up to its count (RefillTarget), so a resize's deletions stay deleted, and
// its accounts keep spare slots for a resize's additions.
inline bool RefillCapped(bool followsTargets) { return followsTargets; }

// The spare share the roster accounts are sized for beyond the roster while the refill is capped: a tenth of the
// roster, rounded up (2500 -> 250 bots, 25 empty accounts at 10 characters each)
inline uint32_t RosterSpareBots(uint32_t roster) { return (roster + 9) / 10; }

// CS26d: the characters the roster accounts are sized for while the refill is capped: the larger of the roster's
// count (RosterSize or the online cap) and the characters `found` on the roster accounts at startup, plus the spare
// share, so a roster a resize grew past RosterSize keeps its spare slots
inline uint32_t RosterAccountBots(uint32_t rosterCount, uint32_t found)
{
    uint32_t const roster = std::max(rosterCount, found);
    return roster + RosterSpareBots(roster);
}

// CS26d: the capped refill's target for the roster accounts: the roster's count only while they hold no character
// (a fresh or wiped database), else none (the roster's later growth is a resize's), plus main's ladder pool either way
inline uint32_t RefillTarget(uint32_t rosterCount, uint32_t held, uint32_t ladderPool)
{
    return (held == 0 ? rosterCount : 0) + ladderPool;
}

// What an account becomes in RandomPlayerbotMgr::AssignAccountTypes: a roster (type 1) or AddClass (type 2) account,
// or an unassigned one the roster does not use
enum class RefillRole : uint8_t
{
    Roster,
    AddClass,
    Spare
};

// The roles of the factory's accounts (in account order) from their stored types (0 = unassigned or no row), as
// AssignAccountTypes will give them: types 1 and 2 stay; the lowest unassigned accounts become roster accounts up to
// `neededRoster`, then the highest remaining ones AddClass accounts up to `neededAddClass`; the rest stay unassigned
inline std::vector<RefillRole> RefillRoles(std::vector<uint8_t> const& types, uint32_t neededRoster,
                                           uint32_t neededAddClass)
{
    std::vector<RefillRole> roles(types.size(), RefillRole::Spare);
    std::vector<bool> assigned(types.size(), false);
    uint32_t roster = 0;
    uint32_t addClass = 0;
    for (size_t i = 0; i < types.size(); ++i)
    {
        if (types[i] == 1)
        {
            roles[i] = RefillRole::Roster;
            assigned[i] = true;
            ++roster;
        }
        else if (types[i] == 2)
        {
            roles[i] = RefillRole::AddClass;
            assigned[i] = true;
            ++addClass;
        }
    }

    for (size_t i = 0; i < types.size() && roster < neededRoster; ++i)
    {
        if (assigned[i])
            continue;

        roles[i] = RefillRole::Roster;
        assigned[i] = true;
        ++roster;
    }

    for (size_t i = types.size(); i-- > 0 && addClass < neededAddClass;)
    {
        if (assigned[i])
            continue;

        roles[i] = RefillRole::AddClass;
        assigned[i] = true;
        ++addClass;
    }

    return roles;
}

struct RefillAccount
{
    uint32_t held = 0;  // characters on the account now
    RefillRole role = RefillRole::Roster;
};

constexpr uint32_t RefillNoLimit = 0xFFFFFFFFu;

// How many characters the factory may make on each account. Uncapped: no limit anywhere (upstream's fill to a full
// account). Capped: AddClass accounts still fill up; the roster accounts together reach `target` characters, filled in
// account order up to `slotsPerAccount` each; unassigned accounts get none.
inline std::vector<uint32_t> RefillPlan(std::vector<RefillAccount> const& accounts, bool capped, uint32_t target,
                                        uint32_t slotsPerAccount)
{
    std::vector<uint32_t> plan(accounts.size(), RefillNoLimit);
    if (!capped)
        return plan;

    uint32_t held = 0;
    for (RefillAccount const& account : accounts)
        if (account.role == RefillRole::Roster)
            held += account.held;

    uint32_t budget = target > held ? target - held : 0;
    for (size_t i = 0; i < accounts.size(); ++i)
    {
        if (accounts[i].role == RefillRole::AddClass)
            continue;

        uint32_t const free = accounts[i].held < slotsPerAccount ? slotsPerAccount - accounts[i].held : 0;
        plan[i] = accounts[i].role == RefillRole::Roster ? std::min(budget, free) : 0;
        budget -= plan[i];
    }

    return plan;
}
}  // namespace PopulationPolicy

#endif
