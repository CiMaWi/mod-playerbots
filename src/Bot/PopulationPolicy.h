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
inline RandomizeDecision DecideRandomize(bool persistent, uint32_t level, bool deathKnight, uint32_t maxLevel,
                                         bool downgradeMaxLevelBot)
{
    bool const reroll = level < 3 || (level < 56 && deathKnight);
    bool const downgrade = !reroll && level >= maxLevel && downgradeMaxLevelBot;

    if (!persistent)
        return {reroll || downgrade, KeptReason::None};

    return {false, reroll ? KeptReason::Reroll : (downgrade ? KeptReason::Downgrade : KeptReason::None)};
}

// RandomizeFirst's level: the rolled one, or the bot's current level when persistent (a re-initialisation at the
// same level; a new character so stays at its creation level).
inline uint32_t FirstLevel(bool persistent, uint32_t currentLevel, uint32_t rolledLevel)
{
    return persistent ? currentLevel : rolledLevel;
}

// The level a factory or reset path may set: never below the current one when persistent.
inline uint32_t KeepLevel(bool persistent, uint32_t currentLevel, uint32_t requestedLevel)
{
    return (persistent && requestedLevel < currentLevel) ? currentLevel : requestedLevel;
}

// RandomBotLevelMgr (level brackets and level reset) runs only without persistence.
inline bool LevelMgrAllowed(bool persistent) { return !persistent; }

// The level-lowering options persistence refuses, for the startup and reload warning.
inline std::vector<char const*> RefusedOptions(bool persistent, bool downgradeMaxLevelBot, bool resetBotLevel,
                                               bool levelBrackets)
{
    std::vector<char const*> refused;
    if (!persistent)
        return refused;

    if (downgradeMaxLevelBot)
        refused.push_back("Playerbots.DowngradeMaxLevelBot");
    if (resetBotLevel)
        refused.push_back("Playerbots.ResetBotLevel.Enabled");
    if (levelBrackets)
        refused.push_back("Playerbots.LevelBrackets.Enabled");

    return refused;
}

inline char const* KeptReasonName(KeptReason reason)
{
    switch (reason)
    {
        case KeptReason::Reroll:
            return "reroll";
        case KeptReason::Downgrade:
            return "downgrade";
        default:
            return "none";
    }
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
    double weight = 1.0;      // H3: weight of an unacquainted bot's share (its level range's BalanceRange::weight)
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

        // H3's logout preference: the unacquainted bots' shares weighted by level range, normalised so that together
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

// A synthetic roster over simulated days: the selftest of the schedule (harness and ".playerbots population
// selftest"). Guids 1..roster, 60 % team 0; acquainted bots spread over the roster, blocked ones too (the first
// acquainted bot is also blocked); random pins and logouts by other paths (refilled in login order).
struct RotationSimParams
{
    uint32_t roster = 300;
    uint32_t acquainted = 10;
    uint32_t blocked = 5;
    uint32_t days = 3;
    uint32_t pinPermille = 20;  // per tick, an online bot gets pinned for pinTicks
    uint32_t pinTicks = 12;
    uint32_t dropPermille = 5;  // per tick, an online bot that is not pinned logs out by another path
    uint32_t seed = 1;
    RotationSettings settings;
    // Checks: an acquainted bot's daily share at most this far below its owed share; the others' mean and minimum
    double acquaintedTolerance = 0.05;
    double otherMeanTolerance = 0.03;
    double otherMinTolerance = 0.2;
};

struct RotationSimStats
{
    RotationShares shares;
    uint32_t eligible = 0;
    uint32_t acquainted = 0;
    uint32_t ticks = 0;
    uint32_t capMisses = 0;      // ticks whose online count was not min(cap, eligible)
    uint32_t blockedOnline = 0;  // blocked bots seen online, summed over ticks
    uint32_t badSwaps = 0;       // swaps across teams, of a pinned or blocked bot, or inside the minimum session
    uint64_t swaps = 0;
    uint64_t logins = 0;
    double minAcquaintedShare = 1.0;
    double meanAcquaintedShare = 0.0;
    double minOtherShare = 1.0;
    double meanOtherShare = 0.0;
    double maxOtherShare = 0.0;
    double meanSessionSeconds = 0.0;
};

inline RotationSimStats SimulateRotation(RotationSimParams const& params)
{
    RotationSimStats stats;
    RotationSettings const& settings = params.settings;
    uint32_t const n = params.roster;
    uint32_t const day = 86400;
    uint32_t const start = 1000000;

    std::vector<RotationEntry> roster(n);
    uint32_t const acquaintedStep = params.acquainted ? std::max(1u, n / params.acquainted) : 0;
    uint32_t const blockedStep = params.blocked ? std::max(2u, n / params.blocked) : 0;
    uint32_t acquaintedCount = 0;
    uint32_t blockedCount = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        RotationEntry& entry = roster[i];
        entry.guid = i + 1;
        entry.team = (i % 5) < 3 ? 0 : 1;
        if (acquaintedStep && i % acquaintedStep == 0 && acquaintedCount < params.acquainted)
        {
            entry.acquainted = true;
            ++acquaintedCount;
        }
        if (blockedStep && blockedCount < params.blocked &&
            ((i == 0 && entry.acquainted) || (!entry.acquainted && i % blockedStep == blockedStep / 2)))
        {
            entry.blocked = true;
            ++blockedCount;
        }
    }

    uint32_t eligible = 0;
    for (RotationEntry const& entry : roster)
        if (!entry.blocked)
        {
            ++eligible;
            if (entry.acquainted)
                ++stats.acquainted;
        }

    stats.eligible = eligible;
    uint32_t const target = std::min(settings.cap, eligible);

    uint32_t rng = params.seed * 2654435761U + 1;
    auto const roll = [&rng]()
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng % 1000;
    };

    RotationSchedule schedule(params.seed);
    std::vector<uint32_t> lastChange(n, 0);
    std::vector<uint32_t> pinLeft(n, 0);
    std::vector<uint32_t> onlineSeconds(static_cast<size_t>(params.days) * n, 0);
    uint64_t totalOnline = 0;
    uint32_t now = start;

    auto const fill = [&]()
    {
        uint32_t online = 0;
        for (RotationEntry const& entry : roster)
            if (entry.online)
                ++online;

        if (online >= target)
            return;

        std::vector<uint32_t> const order = schedule.LoginOrder(now, roster, settings);
        for (size_t k = 0; k < order.size() && online < target; ++k, ++online)
        {
            roster[order[k] - 1].online = true;
            lastChange[order[k] - 1] = now;
            schedule.Note(order[k], true, now);
            ++stats.logins;
        }
    };

    stats.shares = schedule.Tick(now, roster, settings).shares;
    fill();

    uint32_t const ticks = params.days * day / settings.intervalSeconds;
    for (uint32_t t = 0; t < ticks; ++t)
    {
        uint32_t const d = (now - start) / day;
        for (uint32_t i = 0; i < n; ++i)
            if (roster[i].online && d < params.days)
            {
                onlineSeconds[static_cast<size_t>(d) * n + i] += settings.intervalSeconds;
                totalOnline += settings.intervalSeconds;
            }

        now += settings.intervalSeconds;
        for (uint32_t i = 0; i < n; ++i)
        {
            RotationEntry& entry = roster[i];
            if (pinLeft[i])
                --pinLeft[i];
            if (entry.online && !pinLeft[i] && roll() < params.pinPermille)
                pinLeft[i] = params.pinTicks;
            entry.pinned = entry.online && pinLeft[i] > 0;
            if (entry.online && !entry.pinned && roll() < params.dropPermille)
            {
                entry.online = false;
                lastChange[i] = now;
                schedule.Note(entry.guid, false, now);
            }
        }

        RotationTickResult const result = schedule.Tick(now, roster, settings);
        stats.shares = result.shares;
        for (RotationSwap const& swap : result.swaps)
        {
            RotationEntry& out = roster[swap.out - 1];
            RotationEntry& in = roster[swap.in - 1];
            bool const outReady =
                !lastChange[swap.out - 1] || now - lastChange[swap.out - 1] >= settings.minSessionSeconds;
            bool const inReady =
                !lastChange[swap.in - 1] || now - lastChange[swap.in - 1] >= settings.minSessionSeconds;
            if (!out.online || out.pinned || out.blocked || in.online || in.blocked || out.team != in.team ||
                !outReady || !inReady)
                ++stats.badSwaps;

            out.online = false;
            in.online = true;
            lastChange[swap.out - 1] = now;
            lastChange[swap.in - 1] = now;
            ++stats.swaps;
            ++stats.logins;
        }

        fill();
        ++stats.ticks;
        uint32_t online = 0;
        for (RotationEntry const& entry : roster)
        {
            if (!entry.online)
                continue;

            ++online;
            if (entry.blocked)
                ++stats.blockedOnline;
        }

        if (online != target)
            ++stats.capMisses;
    }

    // Daily shares, the first day left out as warm-up when there are more
    uint32_t const firstDay = params.days > 1 ? 1 : 0;
    double acquaintedSum = 0.0;
    double otherSum = 0.0;
    uint32_t acquaintedSamples = 0;
    uint32_t otherSamples = 0;
    for (uint32_t d = firstDay; d < params.days; ++d)
        for (uint32_t i = 0; i < n; ++i)
        {
            if (roster[i].blocked)
                continue;

            double const share = onlineSeconds[static_cast<size_t>(d) * n + i] / static_cast<double>(day);
            if (roster[i].acquainted)
            {
                stats.minAcquaintedShare = std::min(stats.minAcquaintedShare, share);
                acquaintedSum += share;
                ++acquaintedSamples;
            }
            else
            {
                stats.minOtherShare = std::min(stats.minOtherShare, share);
                stats.maxOtherShare = std::max(stats.maxOtherShare, share);
                otherSum += share;
                ++otherSamples;
            }
        }

    stats.meanAcquaintedShare = acquaintedSamples ? acquaintedSum / acquaintedSamples : 0.0;
    stats.meanOtherShare = otherSamples ? otherSum / otherSamples : 0.0;
    if (!otherSamples)
        stats.minOtherShare = 0.0;
    if (!acquaintedSamples)
        stats.minAcquaintedShare = 0.0;
    stats.meanSessionSeconds = stats.logins ? static_cast<double>(totalOnline) / stats.logins : 0.0;
    return stats;
}

// What the simulation got wrong, in words; empty = pass
inline std::vector<std::string> RotationSimFailures(RotationSimParams const& params, RotationSimStats const& stats)
{
    std::vector<std::string> failures;
    auto const fail = [&failures](bool bad, std::string const& what)
    {
        if (bad)
            failures.push_back(what);
    };

    std::string const tag =
        "roster " + std::to_string(params.roster) + " cap " + std::to_string(params.settings.cap) + ": ";
    fail(stats.capMisses != 0, tag + std::to_string(stats.capMisses) + " ticks off the cap");
    fail(stats.blockedOnline != 0, tag + "blocked bots came online");
    fail(stats.badSwaps != 0, tag + std::to_string(stats.badSwaps) + " swaps break a rule");
    if (stats.acquainted)
        fail(stats.minAcquaintedShare + params.acquaintedTolerance < stats.shares.acquainted,
             tag + "an acquainted bot got a daily share of " + std::to_string(stats.minAcquaintedShare) + ", owed " +
                 std::to_string(stats.shares.acquainted));

    if (stats.eligible > stats.acquainted)
    {
        fail(std::abs(stats.meanOtherShare - stats.shares.other) > params.otherMeanTolerance,
             tag + "the other bots' mean daily share is " + std::to_string(stats.meanOtherShare) + ", owed " +
                 std::to_string(stats.shares.other));
        fail(stats.minOtherShare + params.otherMinTolerance < stats.shares.other,
             tag + "another bot got a daily share of " + std::to_string(stats.minOtherShare) + ", owed " +
                 std::to_string(stats.shares.other));
    }

    // Sessions last about the configured mean when the roster is above the cap
    if (stats.eligible > params.settings.cap && params.settings.cap)
        fail(stats.meanSessionSeconds < params.settings.meanSessionSeconds * 0.5 ||
                 stats.meanSessionSeconds > params.settings.meanSessionSeconds * 2.0,
             tag + "mean session " + std::to_string(stats.meanSessionSeconds) + " s");

    // Without a surplus nobody is swapped
    if (stats.eligible <= params.settings.cap)
        fail(stats.swaps != 0, tag + "swaps although the whole roster fits under the cap");

    return failures;
}

// The rotation checks shared by the harness and ".playerbots population selftest": share arithmetic, swaps on a
// tiny roster, and simulated days (full: also roster 2500 / cap 2000, DH.1's defaults). Returns the number of checks.
inline uint32_t RotationSelftest(std::vector<std::string>& failures, bool full)
{
    uint32_t checks = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
    };
    auto const same = [](double a, double b) { return std::abs(a - b) < 1e-9; };

    // Shares
    RotationShares shares = ComputeShares(2000, 2500, 50, 90);
    check(same(shares.acquainted, 0.9) && same(shares.other, (2000 - 45.0) / 2450),
          "shares at 2500/2000 with 50 acquainted");
    check(same(shares.acquainted * 50 + shares.other * 2450, 2000.0), "the shares fill the cap");
    shares = ComputeShares(200, 300, 300, 90);
    check(same(shares.acquainted, 200.0 / 300) && same(shares.other, 0.0), "all acquainted: the cap shared");
    shares = ComputeShares(200, 300, 250, 90);
    check(same(shares.acquainted, 0.8) && same(shares.other, 0.0), "more acquainted than the cap at 90 %");
    shares = ComputeShares(2000, 2500, 10, 50);
    check(same(shares.acquainted, 0.8) && same(shares.other, 0.8), "a share below the fair one is raised to it");
    shares = ComputeShares(200, 150, 10, 90);
    check(same(shares.acquainted, 1.0) && same(shares.other, 1.0), "roster under the cap: everyone online");
    check(same(ComputeShares(0, 100, 0, 90).other, 0.0), "cap 0: nobody");
    RotationSettings budgetSettings;
    budgetSettings.intervalSeconds = 300;
    budgetSettings.meanSessionSeconds = 7200;
    check(RotationBudget(2000, budgetSettings) == 84, "budget 2000 x 300 / 7200 rounds up to 84");
    check(RotationBudget(0, budgetSettings) == 0, "budget 0 without online bots");

    // A tiny roster: team kept, pinned and blocked never chosen, minimum session respected
    RotationSettings settings;
    settings.cap = 2;
    settings.intervalSeconds = 60;
    settings.meanSessionSeconds = 60;
    settings.minSessionSeconds = 120;
    settings.acquaintedSharePct = 90;
    std::vector<RotationEntry> tiny(5);
    for (uint32_t i = 0; i < 5; ++i)
        tiny[i].guid = i + 1;
    tiny[0].online = true;  // team 0, online
    tiny[1].online = true;  // team 1, online, pinned
    tiny[1].team = 1;
    tiny[1].pinned = true;
    tiny[2].team = 1;           // team 1, offline
    tiny[3].blocked = true;     // team 0, blocked
    tiny[4].acquainted = true;  // team 0, offline, acquainted
    RotationSchedule schedule(7);
    uint32_t now = 5000;
    check(schedule.Tick(now, tiny, settings).swaps.empty(), "no swap at the first tick (online bots just seen)");
    now += 180;
    RotationTickResult result = schedule.Tick(now, tiny, settings);
    check(result.eligible == 4 && result.blocked == 1 && result.acquainted == 1 && result.online == 2,
          "tick counts eligible, blocked, acquainted, online");
    check(result.swaps.size() == 1 && result.swaps[0].out == 1 && result.swaps[0].in == 5,
          "the acquainted offline bot replaces the team 0 bot; the pinned team 1 bot stays");
    std::vector<uint32_t> order = schedule.LoginOrder(now, tiny, settings);
    check(std::find(order.begin(), order.end(), 4u) == order.end(), "a blocked bot is never in the login order");
    tiny[0].online = false;
    tiny[4].online = true;
    now += 60;
    result = schedule.Tick(now, tiny, settings);
    check(result.swaps.empty(), "inside the minimum session nobody is swapped again");
    order = schedule.LoginOrder(now, tiny, settings);
    check(!order.empty() && order.front() == 3 && order.back() == 1,
          "login order: the bot just logged out comes last (inside its minimum offline time)");
    schedule.Forget(5);
    check(same(schedule.Credit(5), 0.0) && schedule.Size() == 4, "Forget drops a bot's state");

    // Simulated days
    std::vector<RotationSimParams> runs;
    RotationSimParams base;
    base.roster = 300;
    base.acquainted = 10;
    base.blocked = 5;
    base.days = 3;
    base.settings.cap = 200;
    base.settings.intervalSeconds = 300;
    base.settings.meanSessionSeconds = 7200;
    base.settings.minSessionSeconds = 1800;
    runs.push_back(base);

    RotationSimParams under = base;
    under.roster = 150;
    under.dropPermille = 0;
    runs.push_back(under);

    // More acquainted bots than the cap: they share it, the others stay out (no logouts by other paths here: their
    // refill keeps the cap with whoever may log in, the others too)
    RotationSimParams crowded = base;
    crowded.acquainted = 250;
    crowded.blocked = 0;
    crowded.dropPermille = 0;
    runs.push_back(crowded);

    RotationSimParams fast = base;
    fast.settings.intervalSeconds = 60;
    fast.settings.meanSessionSeconds = 1200;
    fast.settings.minSessionSeconds = 300;
    fast.days = 2;
    runs.push_back(fast);

    if (full)
    {
        RotationSimParams large = base;
        large.roster = 2500;
        large.acquainted = 50;
        large.blocked = 40;
        large.settings.cap = 2000;
        runs.push_back(large);

        RotationSimParams calm = large;
        calm.pinPermille = 0;
        calm.dropPermille = 0;
        calm.seed = 99;
        runs.push_back(calm);
    }

    for (RotationSimParams const& params : runs)
    {
        RotationSimStats const stats = SimulateRotation(params);
        std::vector<std::string> const runFailures = RotationSimFailures(params, stats);
        check(runFailures.empty(), runFailures.empty() ? std::string() : runFailures.front());
        for (size_t k = 1; k < runFailures.size(); ++k)
            failures.push_back(runFailures[k]);
    }

    // Deterministic: the same seed gives the same result
    RotationSimStats const a = SimulateRotation(base);
    RotationSimStats const b = SimulateRotation(base);
    check(a.swaps == b.swaps && a.logins == b.logins && same(a.minOtherShare, b.minOtherShare),
          "the simulation is deterministic");

    return checks;
}

// H3: level-range balance by levelling speed (Playerbots.Population.Balance.*). The levels below the max level form
// ranges of rangeSize levels (1-9, 10-19, ..., 70-79 at 10 and 80), the max level a range of its own. Each range's
// target is an equal share of the population, the max level range maxLevelWeightPct of one share (DH.1: double).
// Every interval a controller compares each range's count with its target and sets the range's XP multiplier: over
// target, its bots level faster (they leave the range sooner), under target slower. It is proportional-integral on
// the log of count / target, with a deadband (no correction near the target, so it does not hunt) and bounds (also on
// the integral, so it does not wind up). The max level range has no multiplier (no XP there). Every range also gets
// a weight for the rotation's online shares (the logout preference: bots of an over-target range are owed less online
// time, those of an under-target range more).

constexpr uint32_t BalanceMaxRanges = 32;

struct BalanceSettings
{
    uint32_t rangeSize = 10;           // levels per range below the max level (raised so there are at most 32 ranges)
    uint32_t maxLevel = 80;            // the max level: a range of its own
    uint32_t maxLevelWeightPct = 200;  // the max level range's target against one other range's
    uint32_t minMultiplierPct = 50;    // XP multiplier bounds
    uint32_t maxMultiplierPct = 300;
    uint32_t deadbandPct = 10;           // no correction within this of the target (or the counting noise, if more)
    uint32_t gainPct = 50;               // proportional part: log multiplier per log of count / target
    uint32_t responseSeconds = 1382400;  // integral part: a log error of 1 held this long adds 1 to the log multiplier
    uint32_t smoothingSeconds = 21600;   // the measured log(count / target) is smoothed over this (counting noise)
    uint32_t preferencePct = 25;         // the rotation weight at a range twice (half) its target: 1 -/+ this
};

// One level range: its levels, the last measurement and the controller's state
struct BalanceRange
{
    uint32_t firstLevel = 0;
    uint32_t lastLevel = 0;
    double target = 0.0;
    uint32_t count = 0;
    double measured = 0.0;    // log((count + 1) / (target + 1)), smoothed over smoothingSeconds
    double error = 0.0;       // measured less the deadband: > 0 over target, < 0 under
    double integral = 0.0;    // the controller's integral part (log multiplier)
    double multiplier = 1.0;  // XP multiplier of the range's bots (1 at the max level)
    double weight = 1.0;      // online share weight in the rotation: < 1 over target, > 1 under
    uint32_t overSince = 0;   // when the range last went over its target beyond the deadband; 0: not over
};

// The range size in effect: at least 1, raised until there are at most BalanceMaxRanges ranges
inline uint32_t BalanceRangeSize(BalanceSettings const& settings)
{
    uint32_t const maxLevel = std::max<uint32_t>(2, settings.maxLevel);
    uint32_t size = std::max<uint32_t>(1, settings.rangeSize);
    while ((maxLevel - 1) / size + 2 > BalanceMaxRanges)
        ++size;

    return size;
}

// The range index of a level: level / size below the max level, the last index at or above it
inline uint32_t BalanceRangeOf(uint32_t level, uint32_t rangeSize, uint32_t maxLevel)
{
    uint32_t const top = (maxLevel - 1) / rangeSize + 1;
    return level >= maxLevel ? top : level / rangeSize;
}

inline uint32_t BalanceRangeOf(uint32_t level, BalanceSettings const& settings)
{
    return BalanceRangeOf(level, BalanceRangeSize(settings), std::max<uint32_t>(2, settings.maxLevel));
}

// The ranges of the settings with their levels; targets, counts and state at their start values
inline std::vector<BalanceRange> BalanceLayout(BalanceSettings const& settings)
{
    uint32_t const maxLevel = std::max<uint32_t>(2, settings.maxLevel);
    uint32_t const size = BalanceRangeSize(settings);
    uint32_t const top = BalanceRangeOf(maxLevel, size, maxLevel);
    std::vector<BalanceRange> ranges(top + 1);
    for (uint32_t i = 0; i < top; ++i)
    {
        ranges[i].firstLevel = std::max<uint32_t>(1, i * size);
        ranges[i].lastLevel = std::min(maxLevel - 1, i * size + size - 1);
    }

    ranges[top].firstLevel = maxLevel;
    ranges[top].lastLevel = maxLevel;
    return ranges;
}

inline bool BalanceSameLayout(std::vector<BalanceRange> const& a, std::vector<BalanceRange> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].firstLevel != b[i].firstLevel || a[i].lastLevel != b[i].lastLevel)
            return false;

    return true;
}

// One controller step: `counts` per range (as many as ranges), `population` their sum, `elapsed` the seconds since the
// last step (0 at the first: no smoothing and no integral step). Sets every range's target, count, measured, error,
// integral, multiplier, weight and overSince.
inline void BalanceUpdate(std::vector<BalanceRange>& ranges, std::vector<uint32_t> const& counts, uint32_t population,
                          uint32_t now, uint32_t elapsed, BalanceSettings const& settings)
{
    if (ranges.empty())
        return;

    size_t const top = ranges.size() - 1;
    double const topWeight = settings.maxLevelWeightPct / 100.0;
    double const weights = static_cast<double>(top) + topWeight;
    double const low = std::log(std::max(1u, std::min(settings.minMultiplierPct, 100u)) / 100.0);
    double const high = std::log(std::max(100u, settings.maxMultiplierPct) / 100.0);
    double const gain = settings.gainPct / 100.0;
    double const step = settings.responseSeconds ? static_cast<double>(elapsed) / settings.responseSeconds : 0.0;
    double const smoothing =
        !elapsed ? 1.0 : std::min(1.0, static_cast<double>(elapsed) / std::max(1u, settings.smoothingSeconds));
    double const preference = std::min(90u, settings.preferencePct) / 100.0;
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        BalanceRange& range = ranges[i];
        range.count = i < counts.size() ? counts[i] : 0;
        range.target = weights > 0.0 ? population * (i == top ? topWeight : 1.0) / weights : 0.0;
        double const raw = std::log((range.count + 1.0) / (range.target + 1.0));
        range.measured += (raw - range.measured) * smoothing;
        double const measured = range.measured;
        // Small ranges count noisily: a deadband of at least one standard deviation of a count (Poisson)
        double const deadband = std::log1p(std::max(settings.deadbandPct / 100.0, 1.0 / std::sqrt(range.target + 1.0)));
        range.error = measured > deadband ? measured - deadband : (measured < -deadband ? measured + deadband : 0.0);
        range.overSince = range.error > 0.0 ? (range.overSince ? range.overSince : now) : 0;
        range.weight = 1.0 - preference * std::clamp(range.error / std::log(2.0), -1.0, 1.0);
        if (i == top)
        {
            range.integral = 0.0;
            range.multiplier = 1.0;
            continue;
        }

        range.integral = std::clamp(range.integral + range.error * step, low, high);
        range.multiplier = std::exp(std::clamp(gain * range.error + range.integral, low, high));
    }
}

// Bots levelling under the controller over simulated days (the selftest of the balance; harness and ".playerbots
// population selftest"). Each bot needs baseMinutes + perLevelMinutes x level of online time per level at multiplier
// 1, times its own speed factor; it is online onlineShare of the time (with the preference: weighted by its range,
// normalised over all bots, as the rotation does). The stand-in for H4: every interval up to retirePerInterval bots
// of the max level range, while it is over target, start again at level 1.
struct BalanceSimParams
{
    BalanceSettings settings;
    uint32_t bots = 300;
    uint32_t days = 60;
    uint32_t stepSeconds = 600;
    uint32_t intervalSeconds = 600;  // the controller's
    double onlineShare = 0.8;
    double baseMinutes = 30.0;
    double perLevelMinutes = 2.0;
    double spread = 0.4;              // bot speed factors uniform in [1 - spread, 1 + spread]
    uint32_t retirePerInterval = 2;   // 0: nobody retires (H3 alone: the bots pile up at the max level)
    uint32_t attritionPermille = 50;  // per day, a bot at any level starts again at level 1 (retire and create)
    uint32_t startMaxLevel = 0;       // > 0: every bot starts at a level up to this (skewed); 0: spread over all
    bool controller = true;           // false: multipliers and weights stay 1 (the baseline)
    bool preference = true;
    uint32_t turnPct = 15;  // a turn: the multiplier moves back by more than this from the extreme it reached
    uint32_t seed = 1;
};

struct BalanceSimStats
{
    uint32_t updates = 0;
    uint64_t retired = 0;
    double meanError = 0.0;   // last third: mean |log((count + 1) / (target + 1))| over the ranges below the max level
    double finalError = 0.0;  // the same at the last step
    double minMultiplier = 1.0;  // over every step and range
    double maxMultiplier = 1.0;
    double minWeight = 1.0;
    double maxWeight = 1.0;
    uint32_t reversals = 0;   // second half, the most of one range: multiplier turns (BalanceSimParams::turnPct)
    uint32_t firstWrong = 0;  // first step: ranges beyond the deadband whose multiplier went the wrong way
    std::vector<BalanceRange> final;
    std::vector<std::vector<BalanceRange>> daily;  // the ranges at the start of every day
};

inline BalanceSimStats SimulateBalance(BalanceSimParams const& params)
{
    BalanceSimStats stats;
    BalanceSettings const& settings = params.settings;
    uint32_t const maxLevel = std::max<uint32_t>(2, settings.maxLevel);
    uint32_t const size = BalanceRangeSize(settings);
    std::vector<BalanceRange> ranges = BalanceLayout(settings);
    size_t const top = ranges.size() - 1;
    uint32_t const n = params.bots;
    uint32_t const start = 1000000;

    uint64_t state = params.seed * 0x9E3779B97F4A7C15ULL + 1;
    auto const next = [&state]()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 33);
    };
    auto const unit = [&next]() { return next() / 2147483648.0; };

    std::vector<uint32_t> level(n);
    std::vector<double> progress(n);
    std::vector<double> speed(n);
    uint32_t const startMax = params.startMaxLevel ? std::min(params.startMaxLevel, maxLevel) : maxLevel;
    for (uint32_t i = 0; i < n; ++i)
    {
        speed[i] = 1.0 - params.spread + 2.0 * params.spread * unit();
        level[i] = 1 + next() % startMax;
        progress[i] = level[i] < maxLevel ? unit() : 0.0;
    }

    std::vector<uint32_t> counts(ranges.size());
    std::vector<double> online(ranges.size(), params.onlineShare);
    std::vector<double> extreme(ranges.size(), 1.0);
    std::vector<int> direction(ranges.size(), 0);
    std::vector<uint32_t> turns(ranges.size(), 0);
    uint32_t const steps = static_cast<uint32_t>(static_cast<uint64_t>(params.days) * 86400 / params.stepSeconds);
    uint32_t const interval = std::max(params.intervalSeconds, params.stepSeconds);
    uint32_t nextUpdate = start;
    uint32_t lastUpdate = 0;
    double errorSum = 0.0;
    uint32_t errorSamples = 0;
    uint32_t retireFrom = 0;
    auto const meanError = [&ranges, top]()
    {
        double sum = 0.0;
        for (size_t r = 0; r < top; ++r)
            sum += std::abs(std::log((ranges[r].count + 1.0) / (ranges[r].target + 1.0)));
        return top ? sum / top : 0.0;
    };

    for (uint32_t s = 0; s < steps; ++s)
    {
        uint32_t const now = start + s * params.stepSeconds;
        if ((now - start) % 86400 < params.stepSeconds)
        {
            std::fill(counts.begin(), counts.end(), 0);
            for (uint32_t i = 0; i < n; ++i)
                ++counts[BalanceRangeOf(level[i], size, maxLevel)];

            std::vector<BalanceRange> day = ranges;
            for (size_t r = 0; r < day.size(); ++r)
                day[r].count = counts[r];
            stats.daily.push_back(day);
        }

        if (now >= nextUpdate)
        {
            std::fill(counts.begin(), counts.end(), 0);
            for (uint32_t i = 0; i < n; ++i)
                ++counts[BalanceRangeOf(level[i], size, maxLevel)];

            std::vector<double> before(ranges.size());
            for (size_t r = 0; r < ranges.size(); ++r)
                before[r] = ranges[r].multiplier;

            BalanceUpdate(ranges, counts, n, now, lastUpdate ? now - lastUpdate : 0, settings);
            if (!params.controller)
                for (BalanceRange& range : ranges)
                {
                    range.integral = 0.0;
                    range.multiplier = 1.0;
                    range.weight = 1.0;
                }

            if (!params.preference)
                for (BalanceRange& range : ranges)
                    range.weight = 1.0;

            if (!lastUpdate && params.controller)
                for (size_t r = 0; r < top; ++r)
                    if ((ranges[r].error > 0.0 && ranges[r].multiplier <= before[r]) ||
                        (ranges[r].error < 0.0 && ranges[r].multiplier >= before[r]))
                        ++stats.firstWrong;

            // Online shares as the rotation owes them: weighted, normalised over all bots
            double weightSum = 0.0;
            for (size_t r = 0; r < ranges.size(); ++r)
                weightSum += counts[r] * ranges[r].weight;
            double const norm = weightSum > 0.0 ? n / weightSum : 1.0;
            for (size_t r = 0; r < ranges.size(); ++r)
                online[r] = std::min(1.0, params.onlineShare * ranges[r].weight * norm);

            for (size_t r = 0; r < ranges.size(); ++r)
            {
                stats.minMultiplier = std::min(stats.minMultiplier, ranges[r].multiplier);
                stats.maxMultiplier = std::max(stats.maxMultiplier, ranges[r].multiplier);
                stats.minWeight = std::min(stats.minWeight, ranges[r].weight);
                stats.maxWeight = std::max(stats.maxWeight, ranges[r].weight);
                if (r == top || s < steps / 2)
                {
                    extreme[r] = ranges[r].multiplier;
                    continue;
                }

                // A turn: the multiplier moves against its direction by more than turnPct of the extreme reached
                // (the first such move from the value at half time sets the direction)
                double const m = ranges[r].multiplier;
                double const up = 1.0 + params.turnPct / 100.0;
                double const down = 1.0 / up;
                if (!direction[r])
                {
                    if (m > extreme[r] * up || m < extreme[r] * down)
                    {
                        direction[r] = m > extreme[r] ? 1 : -1;
                        extreme[r] = m;
                    }
                }
                else if (direction[r] > 0 ? m > extreme[r] : m < extreme[r])
                    extreme[r] = m;
                else if (direction[r] > 0 ? m < extreme[r] * down : m > extreme[r] * up)
                {
                    ++turns[r];
                    direction[r] = -direction[r];
                    extreme[r] = m;
                }
            }

            if (s >= steps - steps / 3)
            {
                errorSum += meanError();
                ++errorSamples;
            }

            // Attrition: bots at any level retired and created anew
            if (params.attritionPermille)
            {
                uint32_t const chance =
                    static_cast<uint32_t>(static_cast<uint64_t>(params.attritionPermille) * interval * 1000 / 86400);
                for (uint32_t i = 0; i < n; ++i)
                    if (next() % 1000000 < chance)
                    {
                        level[i] = 1;
                        progress[i] = 0.0;
                        ++stats.retired;
                    }
            }

            // The H4 stand-in
            uint32_t const excess =
                counts[top] > ranges[top].target ? static_cast<uint32_t>(counts[top] - ranges[top].target) : 0;
            uint32_t retire = std::min(params.retirePerInterval, excess);
            for (uint32_t k = 0; k < n && retire; ++k)
            {
                uint32_t const i = (retireFrom + k) % n;
                if (level[i] < maxLevel)
                    continue;

                level[i] = 1;
                progress[i] = 0.0;
                ++stats.retired;
                --retire;
                retireFrom = i + 1;
            }

            ++stats.updates;
            lastUpdate = now;
            nextUpdate = now + interval;
        }

        double const minutes = params.stepSeconds / 60.0;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (level[i] >= maxLevel)
                continue;

            size_t const r = BalanceRangeOf(level[i], size, maxLevel);
            progress[i] += minutes * online[r] * ranges[r].multiplier * speed[i] /
                           (params.baseMinutes + params.perLevelMinutes * level[i]);
            while (progress[i] >= 1.0 && level[i] < maxLevel)
            {
                progress[i] -= 1.0;
                ++level[i];
            }

            if (level[i] >= maxLevel)
                progress[i] = 0.0;
        }
    }

    stats.meanError = errorSamples ? errorSum / errorSamples : 0.0;
    stats.finalError = meanError();
    for (size_t r = 0; r < top; ++r)
        stats.reversals = std::max(stats.reversals, turns[r]);

    stats.final = ranges;
    return stats;
}

// The balance checks shared by the harness and ".playerbots population selftest": layout, targets, controller steps,
// the rotation's weighted shares, and simulated days (full: also DH.1's population of 2500). Returns the checks made.
inline uint32_t BalanceSelftest(std::vector<std::string>& failures, bool full)
{
    uint32_t checks = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("balance: " + what);
    };
    auto const same = [](double a, double b) { return std::abs(a - b) < 1e-9; };

    // Layout
    BalanceSettings settings;
    std::vector<BalanceRange> ranges = BalanceLayout(settings);
    check(ranges.size() == 9 && ranges[0].firstLevel == 1 && ranges[0].lastLevel == 9 && ranges[1].firstLevel == 10 &&
              ranges[7].firstLevel == 70 && ranges[7].lastLevel == 79 && ranges[8].firstLevel == 80 &&
              ranges[8].lastLevel == 80,
          "ranges at 80 / 10: 1-9, 10-19, ..., 70-79, 80");
    check(BalanceRangeOf(1, settings) == 0 && BalanceRangeOf(9, settings) == 0 && BalanceRangeOf(10, settings) == 1 &&
              BalanceRangeOf(79, settings) == 7 && BalanceRangeOf(80, settings) == 8 &&
              BalanceRangeOf(85, settings) == 8 && BalanceRangeOf(0, settings) == 0,
          "range of a level");
    bool covered = true;
    for (uint32_t level = 1; level <= 80; ++level)
    {
        BalanceRange const& range = ranges[BalanceRangeOf(level, settings)];
        covered = covered && range.firstLevel <= level && level <= range.lastLevel;
    }
    check(covered, "every level lies in its range");
    BalanceSettings fine = settings;
    fine.rangeSize = 1;
    check(BalanceLayout(fine).size() <= BalanceMaxRanges && BalanceRangeSize(fine) == 3, "at most 32 ranges");

    // Targets (DH.1) and the first step's directions
    std::vector<uint32_t> counts = {250, 400, 100, 260, 250, 240, 250, 250, 500};
    BalanceUpdate(ranges, counts, 2500, 1000, 0, settings);
    double targets = 0.0;
    for (BalanceRange const& range : ranges)
        targets += range.target;
    check(same(ranges[0].target, 250.0) && same(ranges[8].target, 500.0) && same(targets, 2500.0),
          "targets at 2500: 250 per range, 500 at the max level");
    check(ranges[1].multiplier > 1.0 && ranges[1].weight < 1.0 && ranges[1].overSince == 1000,
          "over target: faster, logged out first, over since now");
    check(ranges[2].multiplier < 1.0 && ranges[2].weight > 1.0 && ranges[2].overSince == 0,
          "under target: slower, online more");
    check(same(ranges[3].multiplier, 1.0) && same(ranges[5].multiplier, 1.0) && same(ranges[3].weight, 1.0),
          "inside the deadband: no correction");
    check(same(ranges[8].multiplier, 1.0), "the max level has no multiplier");
    BalanceUpdate(ranges, counts, 2500, 1600, 600, settings);
    check(ranges[1].overSince == 1000 && ranges[1].integral > 0.0 && ranges[2].integral < 0.0,
          "over since kept; the integral moves with the error");

    // Smoothing (Playerbots.Population.Balance.SmoothingHours): at 6 h a minute moves the measure by 1/360 of the gap,
    // at 60 s it follows at once
    {
        BalanceSettings quick = settings;
        quick.smoothingSeconds = 60;
        std::vector<BalanceRange> slow = BalanceLayout(settings);
        std::vector<BalanceRange> fast = BalanceLayout(quick);
        std::vector<uint32_t> flat(slow.size(), 250);
        flat.back() = 500;
        std::vector<uint32_t> moved = flat;
        moved[1] = 400;
        BalanceUpdate(slow, flat, 2500, 100, 0, settings);
        BalanceUpdate(fast, flat, 2500, 100, 0, quick);
        BalanceUpdate(slow, moved, 2500, 160, 60, settings);
        BalanceUpdate(fast, moved, 2500, 160, 60, quick);
        double const raw = std::log(401.0 / 251.0);
        check(same(fast[1].measured, raw) && slow[1].measured > 0.0 && slow[1].measured < raw / 300.0,
              "smoothing: 60 s follows a step at once, 6 h by 1/360");
    }

    // Bounds: a range holding everybody, held for weeks
    std::vector<uint32_t> skewed(ranges.size(), 0);
    skewed[0] = 2500;
    for (uint32_t k = 0; k < 2000; ++k)
        BalanceUpdate(ranges, skewed, 2500, 2000 + k * 600, 600, settings);
    check(same(ranges[0].multiplier, 3.0) && same(ranges[1].multiplier, 0.5) && same(ranges[8].multiplier, 1.0),
          "saturated: at the bounds");
    check(same(ranges[0].weight, 0.75) && same(ranges[1].weight, 1.25), "weights bounded by the preference");
    // No windup: the integral stopped at the bound, so with the range at half its target afterwards the multiplier
    // turns below 1 within about (log 3 - gain x error) / (error x step) steps (the smoothing adds a few)
    std::vector<uint32_t> half = {125, 250, 250, 250, 250, 250, 250, 250, 625};
    uint32_t back = 0;
    for (uint32_t k = 0; k < 20000 && ranges[0].multiplier >= 1.0; ++k, ++back)
        BalanceUpdate(ranges, half, 2500, 3000000 + k * 600, 600, settings);
    double const halfError = std::log(126.0 / 251.0) + std::log1p(0.1);
    double const expected = (std::log(3.0) + 0.5 * halfError) / (-halfError * 600.0 / settings.responseSeconds) + 216.0;
    check(back <= expected, "after saturation below x1 at half the target within " +
                                std::to_string(static_cast<uint32_t>(expected)) + " steps (" + std::to_string(back) +
                                ")");

    // The rotation weights shares: two offline bots, weights 0.5 and 1.5, owed shares in that ratio
    RotationSettings rotation;
    rotation.cap = 2;
    rotation.minSessionSeconds = 1000;
    std::vector<RotationEntry> four(4);
    for (uint32_t i = 0; i < 4; ++i)
        four[i].guid = i + 1;
    four[0].weight = 0.5;
    four[1].weight = 1.5;
    RotationSchedule schedule(3);
    schedule.Tick(100, four, rotation);
    check(same(schedule.Credit(1), 0.25 * 1000) && same(schedule.Credit(2), 0.75 * 1000) &&
              same(schedule.Credit(3), 0.5 * 1000),
          "rotation shares weighted and normalised");

    // Simulated days
    BalanceSimParams base;
    BalanceSimStats const on = SimulateBalance(base);
    BalanceSimParams off = base;
    off.controller = false;
    BalanceSimStats const baseline = SimulateBalance(off);
    check(on.meanError < 0.25 && on.meanError < baseline.meanError * 0.6,
          "300 bots, 60 days: last third mean error " + std::to_string(on.meanError) + " (baseline " +
              std::to_string(baseline.meanError) + ")");
    check(on.reversals <= 4, "300 bots: " + std::to_string(on.reversals) + " multiplier turns in the second half");
    check(on.minMultiplier >= 0.5 - 1e-9 && on.maxMultiplier <= 3.0 + 1e-9 && on.minWeight >= 0.75 - 1e-9 &&
              on.maxWeight <= 1.25 + 1e-9,
          "300 bots: multipliers and weights within bounds");
    check(on.firstWrong == 0, "300 bots: first step in the right direction");

    BalanceSimParams skew = base;
    skew.startMaxLevel = 19;
    BalanceSimStats const skewed20 = SimulateBalance(skew);
    check(skewed20.firstWrong == 0 && skewed20.minMultiplier >= 0.5 - 1e-9 && skewed20.maxMultiplier <= 3.0 + 1e-9,
          "skewed start (1-19): right direction, within bounds");
    // The bots start as one wave that passes up through the ranges; the multipliers follow it (up while it is in a
    // range, down after it left), so turns are not counted here, only that the ranges end nearer their targets
    double startError = 0.0;
    if (skewed20.daily.size() > 1)
        for (size_t r = 0; r + 1 < skewed20.daily[1].size(); ++r)
            startError += std::abs(std::log((skewed20.daily[1][r].count + 1.0) / (skewed20.daily[1][r].target + 1.0))) /
                          (skewed20.daily[1].size() - 1);
    check(skewed20.final.size() == 9 && skewed20.meanError < 0.35 && skewed20.meanError < startError * 0.25,
          "skewed start: last third mean error " + std::to_string(skewed20.meanError) + ", day 1 " +
              std::to_string(startError));

    BalanceSimParams alone = base;
    alone.retirePerInterval = 0;
    alone.days = 20;
    BalanceSimStats const pile = SimulateBalance(alone);
    check(pile.minMultiplier >= 0.5 - 1e-9 && pile.maxMultiplier <= 3.0 + 1e-9 && pile.reversals <= 4,
          "H3 alone (nobody retires): within bounds, " + std::to_string(pile.reversals) + " turns");

    if (full)
    {
        BalanceSimParams large = base;
        large.bots = 2500;
        large.days = 60;
        large.seed = 7;
        BalanceSimStats const big = SimulateBalance(large);
        large.controller = false;
        BalanceSimStats const bigBaseline = SimulateBalance(large);
        check(big.meanError < 0.15 && big.meanError < bigBaseline.meanError * 0.5 && big.reversals <= 2,
              "2500 bots, 60 days: mean error " + std::to_string(big.meanError) + " (baseline " +
                  std::to_string(bigBaseline.meanError) + "), " + std::to_string(big.reversals) + " turns");
    }

    BalanceSimStats const again = SimulateBalance(base);
    check(same(again.meanError, on.meanError) && again.retired == on.retired, "the simulation is deterministic");
    return checks;
}

// H4a: retiring never-met random bots (Playerbots.Population.Retire.*). In a level range persistently over its target
// (H3), offline roster bots no real player knows are deleted: acquaintance score at most maxScore (0 by default, a
// manual knob; DH.2), in no guild with a real player, on no real player's friend list, in no arena team, in no group,
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

// "Persistently over": H3's overSince (game time since the range is over its target beyond the deadband; 0 = not over)
// is at least persistSeconds old
inline bool RetireRangeDue(uint32_t overSince, uint32_t now, uint32_t persistSeconds)
{
    return overSince && now >= overSince && now - overSince >= persistSeconds;
}

// The range a new bot starts in (level 1) is never retired from by the balance: a new level-1 bot would replace the
// retired one in the same range (the test range of the overlay may still name it)
inline bool RetireRangeAllowed(uint32_t firstLevel, uint32_t lastLevel, uint32_t createLevel = 1)
{
    return createLevel < firstLevel || createLevel > lastLevel;
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

// Playerbots.Population.Retire.TestRange: "first-last" (1 <= first <= last), else false
inline bool ParseRetireTestRange(std::string const& text, uint32_t& firstLevel, uint32_t& lastLevel)
{
    size_t const dash = text.find('-');
    if (dash == std::string::npos || !dash || dash + 1 >= text.size() || dash > 3 || text.size() - dash - 1 > 3)
        return false;

    for (size_t i = 0; i < text.size(); ++i)
        if (i != dash && (text[i] < '0' || text[i] > '9'))
            return false;

    uint32_t const first = static_cast<uint32_t>(std::stoul(text.substr(0, dash)));
    uint32_t const last = static_cast<uint32_t>(std::stoul(text.substr(dash + 1)));
    if (!first || last < first)
        return false;

    firstLevel = first;
    lastLevel = last;
    return true;
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

struct RetireSimParams
{
    uint32_t bots = 2500;
    uint32_t hours = 48;
    uint32_t passesPerHour = 6;
    uint32_t maxPerPass = 1;
    uint32_t maxPerHour = 10;
    uint32_t maxScore = 0;
    uint32_t persistSeconds = 0;
    uint32_t protectedPct = 15;  // bots with a protection bit
    uint32_t metPct = 5;         // bots with a score of 1-10
    uint32_t levelUpPerMille = 10;
    uint32_t createFailPerMille = 0;  // creations that fail
    uint32_t createFailHours = 0;     // ... in the first hours only; 0 = the whole run
    uint32_t createAttempts = 3;
    uint32_t seed = 1;
};

struct RetireSimStats
{
    uint64_t retired = 0;
    uint64_t created = 0;
    uint64_t protectedRetired = 0;  // a bot with a protection bit or a score above maxScore was retired
    uint64_t belowTarget = 0;       // a retire left a range below its target
    uint64_t overHour = 0;          // more retires in a sliding hour than maxPerHour
    uint64_t overPass = 0;          // more retires in a pass than maxPerPass
    uint64_t createRangeRetired = 0;
    uint64_t createFailures = 0;  // failed creation attempts
    uint64_t gaveUp = 0;          // replacements given up
    uint64_t gatedPasses = 0;     // passes without a retire because a replacement was owed
    uint32_t owedAtEnd = 0;
    uint32_t finalRoster = 0;
    uint32_t minRoster = UINT32_MAX;
    uint32_t maxRoster = 0;
};

// One simulated pass's retires (SimulateRetire): the bots chosen leave the roster and are returned
inline std::vector<uint32_t> SimulateRetirePass(RetireSimParams const& params, std::vector<RetireCandidate>& roster,
                                                std::vector<RetireRange> const& layout, std::vector<uint32_t>& recent,
                                                uint32_t now, uint32_t pass, RetireSimStats& stats)
{
    std::vector<RetireRange> ranges;
    for (RetireRange range : layout)
    {
        range.target = params.bots * (range.firstLevel == 80 ? 2.0 : 1.0) / 10.0;
        for (RetireCandidate const& bot : roster)
            if (bot.level >= range.firstLevel && bot.level <= range.lastLevel)
                ++range.count;

        if (RetireRangeAllowed(range.firstLevel, range.lastLevel) && RetireSurplus(range.count, range.target))
            ranges.push_back(range);
    }

    uint32_t const left = RetireHourLeft(recent, now, params.maxPerHour);
    std::vector<uint32_t> const chosen =
        SelectRetirees(roster, ranges, params.maxScore, std::min(left, params.maxPerPass), params.seed + pass);
    if (chosen.size() > params.maxPerPass)
        ++stats.overPass;

    std::vector<uint32_t> retired;
    for (uint32_t const guid : chosen)
    {
        auto const it =
            std::find_if(roster.begin(), roster.end(), [guid](RetireCandidate const& bot) { return bot.guid == guid; });
        if (it == roster.end())
            continue;

        if (it->protect || it->score > params.maxScore)
            ++stats.protectedRetired;

        if (!RetireRangeAllowed(1, 9) && it->level <= 9)
            ++stats.createRangeRetired;

        for (RetireRange& range : ranges)
            if (it->level >= range.firstLevel && it->level <= range.lastLevel)
            {
                --range.count;
                if (range.count < std::ceil(range.target))
                    ++stats.belowTarget;
            }

        roster.erase(it);
        ++stats.retired;
        retired.push_back(guid);
        recent.push_back(now);
        if (recent.size() > params.maxPerHour)
            ++stats.overHour;
    }

    return retired;
}

// Hours of passes over a roster skewed to 70-79, ranges of 10 levels with an equal share and the max level double
// (DH.1), every retire owing a new level-1 bot (H4b; failed creations retried, given up after createAttempts), bots
// levelling up at random between passes
inline RetireSimStats SimulateRetire(RetireSimParams const& params)
{
    uint64_t state = params.seed * 0x9E3779B97F4A7C15ull + 1;
    auto next = [&state](uint32_t bound)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return bound ? static_cast<uint32_t>(state % bound) : 0;
    };

    std::vector<RetireCandidate> roster;
    uint32_t nextGuid = 1;
    for (uint32_t i = 0; i < params.bots; ++i)
    {
        RetireCandidate bot;
        bot.guid = nextGuid++;
        bot.level = next(100) < 40 ? 70 + next(10) : 1 + next(80);
        if (next(100) < params.protectedPct)
            bot.protect = 1u << next(RetireProtectBits);

        if (next(100) < params.metPct)
            bot.score = 1 + next(10);

        roster.push_back(bot);
    }

    std::vector<RetireRange> layout;
    for (uint32_t first = 0; first < 80; first += 10)
        layout.push_back({std::max<uint32_t>(1, first), first + 9, 0, 0.0});

    layout.push_back({80, 80, 0, 0.0});
    RetireSimStats stats;
    CreateAccounting accounting;
    std::vector<uint32_t> recent;
    uint32_t const step = 3600 / std::max<uint32_t>(1, params.passesPerHour);
    uint32_t const passes = params.hours * std::max<uint32_t>(1, params.passesPerHour);
    uint32_t const failPasses = params.createFailHours * std::max<uint32_t>(1, params.passesPerHour);
    for (uint32_t pass = 0; pass < passes; ++pass)
    {
        uint32_t const now = 1000 + pass * step;
        for (RetireCandidate& bot : roster)
            if (bot.level < 80 && next(1000) < params.levelUpPerMille)
                ++bot.level;

        if (!RetireGate(accounting))
            ++stats.gatedPasses;
        else
            for (uint32_t const guid : SimulateRetirePass(params, roster, layout, recent, now, pass, stats))
                CreateOwe(accounting, guid, 0, 0, now);

        // The replacements due: the pass's own and the retries
        bool const failing = params.createFailPerMille && (!failPasses || pass < failPasses);
        for (uint32_t const retiredGuid : CreateDue(accounting, now, params.maxPerPass))
        {
            if (failing && next(1000) < params.createFailPerMille)
            {
                CreateApply(accounting, retiredGuid, CreateOutcome::Failed, now, step, params.createAttempts);
                continue;
            }

            RetireCandidate created;
            created.guid = nextGuid++;
            created.level = 1;
            CreateApply(accounting, retiredGuid, CreateOutcome::Created, now, step, params.createAttempts,
                        created.guid);
            if (CreateApply(accounting, retiredGuid, CreateOutcome::Confirmed, now, step, params.createAttempts) ==
                CreateDecision::Done)
                roster.push_back(created);
        }

        stats.minRoster = std::min<uint32_t>(stats.minRoster, static_cast<uint32_t>(roster.size()));
        stats.maxRoster = std::max<uint32_t>(stats.maxRoster, static_cast<uint32_t>(roster.size()));
    }

    stats.created = accounting.created;
    stats.createFailures = accounting.failures;
    stats.gaveUp = accounting.gaveUp;
    stats.owedAtEnd = static_cast<uint32_t>(accounting.owed.size());
    stats.finalRoster = static_cast<uint32_t>(roster.size());
    return stats;
}

// The retire selection rule (.playerbots population selftest; full adds the long simulation)
inline uint32_t RetireSelftest(std::vector<std::string>& failures, bool full)
{
    uint32_t checks = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("retire: " + what);
    };

    std::vector<RetireRange> const one = {{10, 19, 10, 5.0}};  // surplus 5
    check(SelectRetirees({{100, 15, 0, 0}}, one, 0, 10, 1) == std::vector<uint32_t>{100},
          "an unprotected never-met bot in the range is chosen");
    for (uint32_t bit = 0; bit < RetireProtectBits; ++bit)
        check(SelectRetirees({{100, 15, 0, 1u << bit}}, one, 0, 10, 1).empty(),
              "protection " + RetireReasons(1u << bit, 0, 0) + " alone excludes the bot");

    // Score: never above maxScore, and the low scores only after every score-0 bot
    std::vector<RetireCandidate> mixed;
    for (uint32_t guid = 1; guid <= 4; ++guid)
        mixed.push_back({guid, 12, 0, 0});

    mixed.push_back({5, 12, 1, 0});
    mixed.push_back({6, 12, 2, 0});
    std::vector<uint32_t> const atZero = SelectRetirees(mixed, one, 0, 10, 3);
    check(atZero.size() == 4 && std::find(atZero.begin(), atZero.end(), 5u) == atZero.end(),
          "a bot with score 1 is never chosen at MaxScore 0");
    std::vector<uint32_t> const atOne = SelectRetirees(mixed, one, 1, 10, 3);
    check(atOne.size() == 5 && atOne.back() == 5 && std::find(atOne.begin(), atOne.end(), 6u) == atOne.end(),
          "at MaxScore 1 the score-1 bot comes after every score-0 bot, score 2 never");
    check(SelectRetirees({{5, 12, 1, 0}, {6, 12, 3, 0}}, one, 0, 10, 3).empty(),
          "no automatic fallback: only met bots left, MaxScore 0 retires none");

    // Caps: maxCount and never below the target
    std::vector<RetireCandidate> ten;
    for (uint32_t guid = 1; guid <= 10; ++guid)
        ten.push_back({guid, 14, 0, 0});

    check(SelectRetirees(ten, {{10, 19, 12, 9.5}}, 0, 5, 1).size() == 2, "surplus 12 over 9.5 is 2");
    check(SelectRetirees(ten, {{10, 19, 12, 9.5}}, 0, 1, 1).size() == 1, "maxCount 1 caps a surplus of 2");
    check(SelectRetirees(ten, {{10, 19, 12, 12.0}}, 0, 5, 1).empty(), "a range at its target loses none");
    check(SelectRetirees(ten, {{10, 19, 12, 11.2}}, 0, 5, 1).empty(), "a fractional target is kept whole (ceil)");
    check(SelectRetirees(ten, {{10, 19, 12, 12.0}}, 0, 5, 1, false).size() == 5,
          "the candidate batch ignores the surplus, not maxCount");
    std::vector<RetireCandidate> two = ten;
    for (uint32_t guid = 11; guid <= 15; ++guid)
        two.push_back({guid, 25, 0, 0});

    std::vector<uint32_t> const split = SelectRetirees(two, {{10, 19, 11, 10.0}, {20, 29, 13, 10.0}}, 0, 10, 1);
    uint32_t const fromFirst =
        static_cast<uint32_t>(std::count_if(split.begin(), split.end(), [](uint32_t guid) { return guid <= 10; }));
    check(split.size() == 4 && fromFirst == 1, "each range gives at most its own surplus");

    // Levels outside every range
    check(SelectRetirees({{1, 9, 0, 0}, {2, 20, 0, 0}, {3, 0, 0, 0}}, one, 0, 10, 1).empty(),
          "levels outside the range are never chosen");

    // Deterministic per seed, another seed another order
    std::vector<RetireCandidate> fifty;
    for (uint32_t guid = 1; guid <= 50; ++guid)
        fifty.push_back({guid, 15, 0, 0});

    std::vector<RetireRange> const wide = {{10, 19, 100, 0.0}};
    check(SelectRetirees(fifty, wide, 0, 5, 42) == SelectRetirees(fifty, wide, 0, 5, 42), "same seed, same pick");
    bool differs = false;
    for (uint32_t seed = 1; seed <= 5 && !differs; ++seed)
        differs = SelectRetirees(fifty, wide, 0, 5, seed) != SelectRetirees(fifty, wide, 0, 5, seed + 100);

    check(differs, "another seed picks other bots");

    // Helpers
    check(RetireSurplus(10, 5.0) == 5 && RetireSurplus(5, 5.0) == 0 && RetireSurplus(3, 5.0) == 0 &&
              RetireSurplus(6, 5.5) == 0 && RetireSurplus(4, -1.0) == 4,
          "RetireSurplus");
    check(!RetireRangeDue(0, 5000, 0) && RetireRangeDue(1000, 1000, 0) && !RetireRangeDue(1000, 4599, 3600) &&
              RetireRangeDue(1000, 4600, 3600) && !RetireRangeDue(5000, 1000, 0),
          "RetireRangeDue: 0 is never over; persistSeconds old");
    check(!RetireRangeAllowed(1, 9) && RetireRangeAllowed(10, 19) && RetireRangeAllowed(80, 80),
          "the level-1 range is not retired from");
    std::vector<uint32_t> times = {100, 200, 3700, 3750};
    check(RetireHourLeft(times, 3800, 3) == 1 && times.size() == 2, "the sliding hour drops old retires");
    check(RetireHourLeft(times, 3800, 2) == 0, "the hour cap reached leaves none");
    uint32_t first = 0;
    uint32_t last = 0;
    check(ParseRetireTestRange("1-9", first, last) && first == 1 && last == 9 &&
              ParseRetireTestRange("70-79", first, last) && first == 70 && last == 79,
          "TestRange parses first-last");
    check(!ParseRetireTestRange("", first, last) && !ParseRetireTestRange("9-1", first, last) &&
              !ParseRetireTestRange("0-9", first, last) && !ParseRetireTestRange("a-9", first, last) &&
              !ParseRetireTestRange("10", first, last) && !ParseRetireTestRange("-9", first, last),
          "TestRange refuses malformed text");
    check(RetireReasons(RetireGuildWithReal | RetireFriendOfReal, 3, 0) == "guild|friend|score", "RetireReasons text");

    // H4b: the replacement of a live retire (create, confirm the row, retry, give up)
    CreateAccounting happy;
    CreateOwe(happy, 7, 70, 4, 100, "Old");
    bool const owedGates = !RetireGate(happy) && CreateDue(happy, 100, 1) == std::vector<uint32_t>{7};
    bool const confirming =
        CreateApply(happy, 7, CreateOutcome::Created, 100, 600, 3, 900) == CreateDecision::Confirm &&
        CreateDue(happy, 100, 1).empty() && !RetireGate(happy);
    check(owedGates && confirming &&
              CreateApply(happy, 7, CreateOutcome::Confirmed, 101, 600, 3) == CreateDecision::Done &&
              happy.created == 1 && happy.owed.empty() && RetireGate(happy) && !happy.failures,
          "a replacement is created, confirmed and closes the gate again; no pass while it is owed");
    CreateAccounting failing;
    CreateOwe(failing, 8, 80, 1, 100);
    bool const firstRetry = CreateApply(failing, 8, CreateOutcome::Failed, 100, 600, 3) == CreateDecision::Retry &&
                            CreateDue(failing, 699, 1).empty() &&
                            CreateDue(failing, 700, 1) == std::vector<uint32_t>{8};
    bool const secondRetry = CreateApply(failing, 8, CreateOutcome::Failed, 700, 600, 3) == CreateDecision::Retry;
    check(firstRetry && secondRetry &&
              CreateApply(failing, 8, CreateOutcome::Failed, 1300, 600, 3) == CreateDecision::GaveUp &&
              failing.gaveUp == 1 && failing.failures == 3 && !failing.created && failing.owed.empty() &&
              !RetireGate(failing),
          "a failed creation waits a retry interval; the third failure gives up and pauses the retires");
    failing.paused = false;
    check(RetireGate(failing), "a reload (paused cleared) opens the gate again");
    CreateAccounting missing;
    CreateOwe(missing, 9, 90, 2, 100);
    CreateApply(missing, 9, CreateOutcome::Created, 100, 5, 3, 901);
    bool const missingRetry = CreateApply(missing, 9, CreateOutcome::Missing, 101, 5, 3) == CreateDecision::Retry &&
                              CreateFind(missing, 9) && CreateFind(missing, 9)->createdGuid == 901 &&
                              CreateDue(missing, 105, 1).empty() && CreateDue(missing, 106, 1).size() == 1;
    check(missingRetry && CreateApply(missing, 9, CreateOutcome::Created, 106, 5, 3, 901) == CreateDecision::Confirm &&
              CreateApply(missing, 9, CreateOutcome::Confirmed, 107, 5, 3) == CreateDecision::Done &&
              missing.created == 1 && missing.failures == 1,
          "a missing row is confirmed again (the character is not made twice)");
    CreateAccounting many;
    for (uint32_t guid = 1; guid <= 5; ++guid)
        CreateOwe(many, guid, guid, 1, 100);

    check(CreateDue(many, 100, 3) == std::vector<uint32_t>({1, 2, 3}) && CreateDue(many, 100, 0).size() == 1 &&
              CreateDue(many, 99, 3).empty(),
          "MaxPerPass bounds the creations per call (at least one), none before notBefore");
    check(CreateApply(many, 42, CreateOutcome::Confirmed, 100, 5, 3) == CreateDecision::Unknown && !many.created &&
              many.owed.size() == 5,
          "an unknown entry changes nothing");

    // Over simulated hours, every retire replaced by a level-1 bot: the roster stays constant, no protected or met
    // bot is retired, no range falls below its target, the caps hold
    RetireSimParams sim;
    sim.bots = 300;
    sim.hours = 12;
    sim.passesPerHour = 6;
    sim.maxPerPass = 1;
    sim.maxPerHour = 4;
    RetireSimStats const slow = SimulateRetire(sim);
    check(slow.retired > 0 && slow.retired == slow.created && slow.minRoster == sim.bots && slow.maxRoster == sim.bots,
          "300 bots, 12 h: " + std::to_string(slow.retired) + " retired, roster constant");
    check(!slow.protectedRetired && !slow.belowTarget && !slow.overHour && !slow.overPass && !slow.createRangeRetired,
          "300 bots, 12 h: no protected bot retired, targets and caps hold");
    sim.passesPerHour = 180;
    sim.maxPerPass = 3;
    sim.maxPerHour = 120;
    sim.hours = 2;
    RetireSimStats const fast = SimulateRetire(sim);
    check(fast.retired > 0 && fast.minRoster == sim.bots && fast.maxRoster == sim.bots && !fast.protectedRetired &&
              !fast.belowTarget && !fast.overHour && !fast.overPass,
          "300 bots, overlay rates (20 s, 3/pass, 120/h): " + std::to_string(fast.retired) + " retired, rules hold");

    // Failed creations: the roster is short by at most one pass's retires plus the replacements given up, no pass
    // retires while one is owed or after one was given up, and it is whole again once retried failures stop
    auto const accounted = [](RetireSimStats const& stats, RetireSimParams const& params)
    {
        return stats.retired == stats.created + stats.gaveUp + stats.owedAtEnd &&
               stats.finalRoster + stats.gaveUp + stats.owedAtEnd == params.bots &&
               stats.minRoster + stats.gaveUp + params.maxPerPass >= params.bots && stats.maxRoster <= params.bots &&
               !stats.protectedRetired && !stats.belowTarget && !stats.overHour && !stats.overPass &&
               !stats.createRangeRetired;
    };
    sim.createFailPerMille = 100;
    sim.createFailHours = 1;
    RetireSimStats const flaky = SimulateRetire(sim);
    check(accounted(flaky, sim) && flaky.createFailures > 0 && flaky.gatedPasses > 0 && !flaky.gaveUp &&
              flaky.retired == flaky.created && flaky.finalRoster == sim.bots,
          "overlay rates, 10 % failed creations in the first hour: " + std::to_string(flaky.createFailures) +
              " failures retried, the roster whole at the end");
    sim.createFailPerMille = 1000;
    RetireSimStats const broken = SimulateRetire(sim);
    check(accounted(broken, sim) && broken.gaveUp > 0 && broken.retired == broken.gaveUp &&
              broken.retired <= sim.maxPerPass,
          "overlay rates, every creation failing: " + std::to_string(broken.gaveUp) +
              " given up and counted, then the retires pause");
    if (full)
    {
        RetireSimParams big;
        big.bots = 2500;
        big.hours = 72;
        RetireSimStats const day = SimulateRetire(big);
        check(day.retired > 0 && day.minRoster == big.bots && day.maxRoster == big.bots && !day.protectedRetired &&
                  !day.belowTarget && !day.overHour && !day.overPass && !day.createRangeRetired,
              "2500 bots, 72 h at the defaults: " + std::to_string(day.retired) + " retired, rules hold");
        big.maxScore = 3;
        RetireSimStats const met = SimulateRetire(big);
        check(!met.protectedRetired && met.minRoster == big.bots, "MaxScore 3: no bot above it retired");
        big.maxScore = 0;
        big.createFailPerMille = 20;
        RetireSimStats const flakyDay = SimulateRetire(big);
        check(accounted(flakyDay, big) && flakyDay.createFailures > 0,
              "2500 bots, 72 h, 2 % failed creations: " + std::to_string(flakyDay.createFailures) +
                  " failures, every retire replaced or counted");
    }

    return checks;
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

// A creation-like roster of 2000 (the randomizer's 10 % at level 1 and 10 % at the top lift 1-9 and 80): the
// selftest's fixture and the harness's printed example
inline std::vector<ResizeRange> ResizeCreationFixture()
{
    std::vector<ResizeRange> ranges = ResizeLayout(80);
    std::vector<uint32_t> const counts = {380, 200, 200, 200, 200, 200, 200, 210, 210};
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        ranges[i].count = counts[i];
        ranges[i].removable = counts[i];
    }

    return ranges;
}

// The counts after a plan (every bot removable again)
inline std::vector<ResizeRange> ResizeApply(std::vector<ResizeRange> ranges)
{
    for (ResizeRange& range : ranges)
    {
        range.count = static_cast<uint32_t>(static_cast<int32_t>(range.count) + range.delta);
        range.removable = range.count;
        range.delta = 0;
    }

    return ranges;
}

// The resize checks shared by the harness and ".playerbots population selftest": layout, shares, which ranges lose
// and gain for fixture counts, the removable limit and the class pick
inline uint32_t ResizeSelftest(std::vector<std::string>& failures)
{
    uint32_t checks = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("resize: " + what);
    };
    auto withCounts = [](std::vector<uint32_t> const& counts)
    {
        std::vector<ResizeRange> ranges = ResizeLayout(80);
        for (size_t i = 0; i < ranges.size() && i < counts.size(); ++i)
        {
            ranges[i].count = counts[i];
            ranges[i].removable = counts[i];
        }

        return ranges;
    };
    auto deltas = [](std::vector<ResizeRange> const& ranges)
    {
        std::vector<int32_t> out;
        for (ResizeRange const& range : ranges)
            out.push_back(range.delta);

        return out;
    };

    std::vector<ResizeRange> layout = ResizeLayout(80);
    check(layout.size() == 9 && layout[0].firstLevel == 1 && layout[0].lastLevel == 9 && layout[1].firstLevel == 10 &&
              layout[7].firstLevel == 70 && layout[7].lastLevel == 79 && layout[8].firstLevel == 80 &&
              layout[8].lastLevel == 80,
          "layout 80: 1-9, 10-19, ..., 70-79, 80");
    check(ResizeLayout(1).size() == 1 && ResizeRangeOf(layout, 9) == 0 && ResizeRangeOf(layout, 10) == 1 &&
              ResizeRangeOf(layout, 80) == 8 && ResizeRangeOf(layout, 0) < 0 && ResizeRangeOf(layout, 81) < 0,
          "the range of a level");
    ResizeTargets(layout, 2500);
    check(std::abs(layout[8].target - 250.0) < 1e-9 && std::abs(layout[0].target - 281.25) < 1e-9,
          "shares of 2500: 250 at 80, 281.25 in each other range");

    // 2000 -> 2050 -> 1950: the additions go to the ranges under their share, not to 1-9 or 80; the deletions to 1-9
    std::vector<ResizeRange> grow = ResizeCreationFixture();
    int32_t const added = ResizePlan(grow, 2050);
    int32_t middle = 0;
    for (size_t i = 1; i <= 6; ++i)
        middle += (grow[i].delta >= 8 && grow[i].delta <= 9) ? grow[i].delta : 1000;

    check(added == 50 && !grow[0].delta && !grow[7].delta && !grow[8].delta && middle == 50,
          "2000 -> 2050: +8 or +9 in each of 10-69, none in 1-9, 70-79 or 80: " + ResizePlanText(grow));
    std::vector<ResizeRange> shrink = ResizeApply(grow);
    int32_t const removed = ResizePlan(shrink, 1950);
    check(removed == -100 && shrink[0].delta == -100, "2050 -> 1950: all 100 from 1-9: " + ResizePlanText(shrink));

    std::vector<ResizeRange> deep = ResizeCreationFixture();
    int32_t const deepRemoved = ResizePlan(deep, 1700);
    check(deepRemoved == -300 && deep[0].delta < deep[8].delta && deep[8].delta < 0 && deep[7].delta < 0,
          "2000 -> 1700: most from 1-9, then 80 and 70-79 too: " + ResizePlanText(deep));

    std::vector<ResizeRange> empty = withCounts({225, 225, 0, 225, 225, 225, 225, 225, 200});
    check(ResizePlan(empty, 1875) == 100 && empty[2].delta == 100,
          "an empty range gains first: " + ResizePlanText(empty));

    std::vector<ResizeRange> one = withCounts({0, 0, 0, 0, 1000, 0, 0, 0, 0});
    check(ResizePlan(one, 900) == -100 && one[4].delta == -100, "all in one range: the deletions come from it");
    one = withCounts({0, 0, 0, 0, 1000, 0, 0, 0, 0});
    int32_t const spread = ResizePlan(one, 1080);
    std::vector<int32_t> const spreadDeltas = deltas(one);
    check(spread == 80 && !one[4].delta && *std::max_element(spreadDeltas.begin(), spreadDeltas.end()) <= 12,
          "all in one range: the additions spread over the others: " + ResizePlanText(one));

    std::vector<ResizeRange> topHeavy = withCounts({100, 100, 100, 100, 100, 100, 100, 100, 300});
    check(ResizePlan(topHeavy, 1200) == 100 && !topHeavy[8].delta, "80 over its 10 % gains nothing");
    std::vector<ResizeRange> topLight = withCounts({250, 250, 250, 250, 250, 250, 250, 250, 0});
    check(ResizePlan(topLight, 2100) == 100 && topLight[8].delta == 100, "80 under its 10 % may gain");

    std::vector<ResizeRange> limited = withCounts({500, 300, 200, 200, 200, 200, 200, 200, 150});
    limited[0].removable = 10;
    check(ResizePlan(limited, 2050) == -100 && limited[0].delta == -10 && limited[1].delta == -90,
          "a range loses no more than its removable bots: " + ResizePlanText(limited));
    std::vector<ResizeRange> stuck = withCounts({500, 300, 200, 200, 200, 200, 200, 200, 200});
    for (ResizeRange& range : stuck)
        range.removable = 0;

    check(ResizePlan(stuck, 2000) == 0 && deltas(stuck) == std::vector<int32_t>(9, 0), "nothing removable: no plan");
    std::vector<ResizeRange> same = ResizeCreationFixture();
    check(ResizePlan(same, 2000) == 0 && deltas(same) == std::vector<int32_t>(9, 0), "the same count: no plan");

    std::vector<uint32_t> classes(12, 0);
    classes[1] = 5;
    classes[2] = 3;
    classes[3] = 3;
    check(ResizeClass(classes, (1u << 0) | (1u << 1) | (1u << 2)) == 2, "the least represented class, lowest id");
    check(ResizeClass(classes, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5)) == 6, "an empty class first");
    check(ResizeClass(classes, 0) == 0, "no class allowed: none");

    // The ordering of one RetireTick (ProcessReadyCallbacks, ResizeTick, CreateOwedBots): a retire pass deleted k bots
    // and owes k replacements. Started at once, the resize counts N - k and the replacements land on top of its count;
    // waiting for them, it counts N. The runtime order itself is covered by this model and the owed list draining.
    {
        uint32_t const roster = 2000;
        uint32_t const retired = 3;
        uint32_t characters = roster - retired;  // the pass deleted its bots
        size_t owed = retired;
        bool resizeStarted = false;
        uint32_t counted = 0;
        for (uint32_t tick = 0; tick < 6; ++tick)
        {
            if (!resizeStarted && ResizeMayStart(false, owed))
            {
                resizeStarted = true;
                counted = characters;
            }

            if (owed && tick >= 1)  // the creations land from the second tick on (a retry, the confirm query)
            {
                ++characters;
                --owed;
            }
        }

        check(resizeStarted && counted == roster && characters == roster,
              "a resize waits for the owed retire replacements: it counts the whole roster");
    }
    check(!ResizeMayStart(true, 0) && !ResizeMayStart(false, 1) && ResizeMayStart(false, 0),
          "a resize starts only with no retire batch running and no replacement owed");
    check(ResizeDoneCount(2563, 125, 0) == 2438 && ResizeDoneCount(2000, 0, 50) == 2050 &&
              ResizeDoneCount(2000, 30, 30) == 2000 && ResizeDoneCount(10, 20, 0) == 0,
          "the resize's done count is before - deleted + created, exact while the last deletes are uncommitted");
    return checks;
}

// WL19a: the startup factory's refill (RandomPlayerbotFactory::CreateRandomBots). Upstream fills every bot account to
// a full set of characters at every start. A persistent roster with rotation (Population.Persistent and
// Rotation.Enable) is filled only up to its count instead, so a resize's deletions stay deleted, and its accounts keep
// spare slots for a resize's additions.
inline bool RefillCapped(bool persistent, bool rotation) { return persistent && rotation; }

// The spare share the roster accounts are sized for beyond the roster while the refill is capped: a tenth of the
// roster, rounded up (2500 -> 250 bots, 25 empty accounts at 10 characters each)
inline uint32_t RosterSpareBots(uint32_t roster) { return (roster + 9) / 10; }

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

// The characters a plan makes (the limits of capped accounts; an uncapped account counts its free slots)
inline uint32_t RefillPlanned(std::vector<RefillAccount> const& accounts, std::vector<uint32_t> const& plan,
                              uint32_t slotsPerAccount)
{
    uint32_t total = 0;
    for (size_t i = 0; i < plan.size() && i < accounts.size(); ++i)
    {
        uint32_t const free = accounts[i].held < slotsPerAccount ? slotsPerAccount - accounts[i].held : 0;
        total += plan[i] == RefillNoLimit ? free : plan[i];
    }

    return total;
}

// The refill checks shared by the harness and ".playerbots population selftest": the gate, the spare share, the
// account roles and the refill count with the cap on and off
inline uint32_t RefillSelftest(std::vector<std::string>& failures)
{
    uint32_t checks = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("refill: " + what);
    };
    auto accounts = [](std::vector<uint32_t> const& held, RefillRole role)
    {
        std::vector<RefillAccount> list;
        for (uint32_t const count : held)
            list.push_back({count, role});

        return list;
    };

    check(RefillCapped(true, true) && !RefillCapped(true, false) && !RefillCapped(false, true) &&
              !RefillCapped(false, false),
          "capped only with persistence and rotation");
    check(RosterSpareBots(2500) == 250 && RosterSpareBots(2001) == 201 && RosterSpareBots(0) == 0,
          "the spare share is a tenth of the roster, rounded up");
    check((2500 + RosterSpareBots(2500) + 9) / 10 == 275, "a roster of 2500 gets 275 accounts at 10 characters");

    // A fresh roster: 250 roster accounts and 25 spare ones (all type 1), nothing held
    std::vector<RefillAccount> const fresh = accounts(std::vector<uint32_t>(275, 0), RefillRole::Roster);
    std::vector<uint32_t> const off = RefillPlan(fresh, false, 2500, 10);
    check(off == std::vector<uint32_t>(275, RefillNoLimit) && RefillPlanned(fresh, off, 10) == 2750,
          "rotation off: every account fills up, as upstream");
    std::vector<uint32_t> const on = RefillPlan(fresh, true, 2500, 10);
    check(RefillPlanned(fresh, on, 10) == 2500 && on[0] == 10 && on[249] == 10 && on[250] == 0 && on[274] == 0,
          "rotation on: the first 250 accounts fill, the 25 spare ones stay empty");

    // A resize deleted 100 (spread over the accounts) and the next start has RosterSize at the done line's count
    std::vector<uint32_t> held(275, 0);
    for (size_t i = 0; i < 250; ++i)
        held[i] = i < 100 ? 9 : 10;

    std::vector<RefillAccount> const shrunk = accounts(held, RefillRole::Roster);
    check(RefillPlanned(shrunk, RefillPlan(shrunk, true, 2400, 10), 10) == 0,
          "after a resize to 2400 with RosterSize 2400: nothing comes back");
    check(RefillPlanned(shrunk, RefillPlan(shrunk, true, 2500, 10), 10) == 100,
          "RosterSize left at 2500: the factory tops up 100");
    check(RefillPlanned(shrunk, RefillPlan(shrunk, false, 2400, 10), 10) == 350,
          "rotation off: the deleted ones and the spare accounts fill up");

    // A resize added 50 on spare accounts: RosterSize 2550 at the next start makes nothing
    held.assign(275, 0);
    for (size_t i = 0; i < 255; ++i)
        held[i] = 10;

    std::vector<RefillAccount> const grown = accounts(held, RefillRole::Roster);
    check(RefillPlanned(grown, RefillPlan(grown, true, 2550, 10), 10) == 0, "after a resize to 2550: nothing more");

    std::vector<RefillAccount> const partial = accounts({10, 7, 0, 0}, RefillRole::Roster);
    check(RefillPlan(partial, true, 25, 10) == std::vector<uint32_t>({0, 3, 5, 0}),
          "the free slots fill in account order up to the target");
    check(RefillPlanned(partial, RefillPlan(partial, true, 10, 10), 10) == 0, "more held than the target: none");

    std::vector<RefillAccount> mixed = accounts({10, 4, 0}, RefillRole::Roster);
    mixed.push_back({3, RefillRole::AddClass});
    mixed.push_back({10, RefillRole::Spare});
    mixed.push_back({0, RefillRole::Spare});
    std::vector<uint32_t> const mixedPlan = RefillPlan(mixed, true, 20, 10);
    check(mixedPlan == std::vector<uint32_t>({0, 6, 0, RefillNoLimit, 0, 0}),
          "AddClass accounts fill up and do not count; unassigned ones get none");

    std::vector<RefillRole> const roles = RefillRoles({1, 1, 0, 0, 0, 2, 0}, 3, 2);
    check(
        roles == std::vector<RefillRole>({RefillRole::Roster, RefillRole::Roster, RefillRole::Roster, RefillRole::Spare,
                                          RefillRole::Spare, RefillRole::AddClass, RefillRole::AddClass}),
        "roles: the lowest unassigned become roster accounts, the highest AddClass ones");
    std::vector<RefillRole> const fresher = RefillRoles({0, 0, 0, 0}, 2, 1);
    check(fresher == std::vector<RefillRole>(
                         {RefillRole::Roster, RefillRole::Roster, RefillRole::Spare, RefillRole::AddClass}),
          "roles on a fresh database");
    check(RefillRoles({0, 0}, 5, 5) == std::vector<RefillRole>(2, RefillRole::Roster),
          "roster accounts first when there are too few");
    return checks;
}
}  // namespace PopulationPolicy

#endif
