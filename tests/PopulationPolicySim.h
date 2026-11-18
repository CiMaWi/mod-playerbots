/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_POPULATIONPOLICYSIM_H
#define _PLAYERBOT_POPULATIONPOLICYSIM_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "PopulationPolicy.h"

// Simulators and selftests of the population rules (src/Bot/PopulationPolicy.h): proof code for the offline harness
// tests/population-selftest.cpp, kept out of the rule header the server includes.
namespace PopulationPolicy
{
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

// The rotation checks of the offline harness (tests/population-selftest.cpp): share arithmetic, swaps on a
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

// The range a new bot starts in (level 1) is never retired from: a new level-1 bot would replace the retired one in the
// same range
inline bool RetireRangeAllowed(uint32_t firstLevel, uint32_t lastLevel, uint32_t createLevel = 1)
{
    return createLevel < firstLevel || createLevel > lastLevel;
}

struct RetireSimParams
{
    uint32_t bots = 2500;
    uint32_t hours = 48;
    uint32_t passesPerHour = 6;
    uint32_t maxPerPass = 1;
    uint32_t maxPerHour = 10;
    uint32_t maxScore = 0;
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

// The retire selection rule (the offline harness, tests/population-selftest.cpp; full adds the long simulation)
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
    check(!RetireRangeAllowed(1, 9) && RetireRangeAllowed(10, 19) && RetireRangeAllowed(80, 80),
          "the level-1 range is not retired from");
    std::vector<uint32_t> times = {100, 200, 3700, 3750};
    check(RetireHourLeft(times, 3800, 3) == 1 && times.size() == 2, "the sliding hour drops old retires");
    check(RetireHourLeft(times, 3800, 2) == 0, "the hour cap reached leaves none");
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

// The resize checks of the offline harness (tests/population-selftest.cpp): layout, shares, which ranges lose
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

// The refill checks of the offline harness (tests/population-selftest.cpp): the spare share, the start's target and
// sizing, and the refill count with the cap on and off
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

    // CS23d: the retire's top-level range (Retire.TestTarget); 0 retires nothing
    check(TopLevelRetireRanges(80, 0).empty() && TopLevelRetireRanges(70, 0).empty(),
          "a top-level target of 0 names no range: nothing retires");
    std::vector<RetireRange> const top = TopLevelRetireRanges(80, 250);
    check(top.size() == 1 && top[0].firstLevel == 80 && top[0].lastLevel == 80 && top[0].count == 0 &&
              top[0].target == 250.0,
          "a top-level target names the one range max-max with that target");
    check(RetireSurplus(300, top[0].target) == 50 && RetireSurplus(250, top[0].target) == 0,
          "the top level is retired from down to the target, never below");
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

    // CS26d: a start creates the roster's count only while the roster accounts hold no character; main's ladder pool
    // keeps its place in the target
    check(RefillTarget(2500, 0, 0) == 2500, "an empty roster: the start's target is the roster's count");
    check(RefillTarget(2500, 1, 0) == 0 && RefillTarget(2500, 2400, 0) == 0 && RefillTarget(2500, 2600, 0) == 0,
          "a roster holding any character: the start's target is none");
    check(RefillTarget(2500, 0, 40) == 2540 && RefillTarget(2500, 2400, 40) == 40,
          "main's ladder pool is added to the target either way");
    check(RosterAccountBots(2500, 0) == 2750 && RosterAccountBots(2500, 2400) == 2750,
          "the accounts are sized for the roster's count and its tenth while fewer characters are found");
    check(RosterAccountBots(2500, 2563) == 2563 + 257 && (RosterAccountBots(2500, 2563) + 9) / 10 == 282,
          "a roster grown past its count: the accounts are sized for the characters found and their tenth");

    // A resize deleted 100 (spread over the accounts); RosterSize stays at 2500
    std::vector<uint32_t> held(275, 0);
    for (size_t i = 0; i < 250; ++i)
        held[i] = i < 100 ? 9 : 10;

    std::vector<RefillAccount> const shrunk = accounts(held, RefillRole::Roster);
    check(RefillPlanned(shrunk, RefillPlan(shrunk, true, 2400, 10), 10) == 0,
          "a target of the characters held: nothing comes back");
    check(RefillPlanned(shrunk, RefillPlan(shrunk, true, RefillTarget(2500, 2400, 0), 10), 10) == 0,
          "after a resize to 2400 with RosterSize left at 2500: the factory creates none");
    check(RefillPlanned(shrunk, RefillPlan(shrunk, false, 2400, 10), 10) == 350,
          "rotation off: the deleted ones and the spare accounts fill up");

    // A resize added 50 on spare accounts: the next start makes nothing (RosterSize stays at 2500)
    held.assign(275, 0);
    for (size_t i = 0; i < 255; ++i)
        held[i] = 10;

    std::vector<RefillAccount> const grown = accounts(held, RefillRole::Roster);
    check(RefillPlanned(grown, RefillPlan(grown, true, RefillTarget(2500, 2550, 0), 10), 10) == 0,
          "after a resize to 2550: nothing more");

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

    return checks;
}
}  // namespace PopulationPolicy

#endif
