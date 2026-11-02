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

        std::vector<Candidate> outs[2];
        std::vector<Candidate> ins[2];
        for (RotationEntry const& entry : roster)
        {
            double const share = entry.acquainted ? result.shares.acquainted : result.shares.other;
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

        std::sort(keyed.begin(), keyed.end(), [](std::pair<LoginKey, uint32_t> const& a,
                                                 std::pair<LoginKey, uint32_t> const& b)
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
            bool const outReady = !lastChange[swap.out - 1] || now - lastChange[swap.out - 1] >= settings.minSessionSeconds;
            bool const inReady = !lastChange[swap.in - 1] || now - lastChange[swap.in - 1] >= settings.minSessionSeconds;
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

    std::string const tag = "roster " + std::to_string(params.roster) + " cap " + std::to_string(params.settings.cap) +
                            ": ";
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
    tiny[0].online = true;                         // team 0, online
    tiny[1].online = true;                         // team 1, online, pinned
    tiny[1].team = 1;
    tiny[1].pinned = true;
    tiny[2].team = 1;                              // team 1, offline
    tiny[3].blocked = true;                        // team 0, blocked
    tiny[4].acquainted = true;                     // team 0, offline, acquainted
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
}  // namespace PopulationPolicy

#endif
