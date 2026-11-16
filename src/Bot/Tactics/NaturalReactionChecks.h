/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NATURALREACTIONCHECKS_H
#define PLAYERBOTS_NATURALREACTIONCHECKS_H

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "BotAttributes.h"
#include "NaturalReaction.h"
#include "TacticGate.h"

// Pure checks of the natural reaction (NaturalReaction.h) with the configured numbers on synthetic hashes, skills and
// times; no bots
template <typename Check, typename Say>
void RunReactionChecks(Check&& check, Say&& say, ReactionParams const& params, float exponent)
{
    auto const within = [](double value, double expected, double tolerance)
    { return std::fabs(value - expected) <= tolerance * std::fabs(expected); };

    constexpr uint32 Draws = 10000;
    constexpr uint32 MissWindowMs = 1500;

    // The probit against known quantiles, and increasing
    check(std::fabs(Probit(0.5)) < 1e-9 && std::fabs(Probit(0.975) - 1.959963985) < 1e-6 &&
              std::fabs(Probit(0.025) + 1.959963985) < 1e-6 && std::fabs(Probit(0.001) + 3.090232306) < 1e-6,
          "probit quantiles");
    bool increasing = true;
    for (uint32 step = 2; step < 1000; ++step)
        increasing = increasing && Probit(step / 1000.0) > Probit((step - 1) / 1000.0);

    check(increasing, "probit not increasing");
    check(UnitFromHash(0) > 0.0 && UnitFromHash(~0ULL) < 1.0, "unit from hash leaves (0, 1)");

    // 10 000 events per skill: the median of the draws is the configured median and the spread of their logarithms
    // the configured sigma (+-5 %); the minimum holds; a hash always gives the same time
    for (uint8 skill : {1, 20, 50, 80, 99})
    {
        ReactionState const state = ComputeReactionState(params, skill, exponent);
        float const position = SkillPosition(skill);
        double const median = SkillLerp(params.MedianMsAtSkill1, params.MedianMsAtSkill99, position, exponent);
        double const sigma = SkillLerp(params.SigmaAtSkill1, params.SigmaAtSkill99, position, exponent);
        std::vector<double> raw;
        raw.reserve(Draws);
        double sum = 0.0;
        double sumSquares = 0.0;
        uint32 belowMin = 0;
        uint32 raised = 0;
        bool stable = true;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            uint64 const hash = ReactionEventHash(1000 + draw, static_cast<uint8>(1 + draw % 6), draw * 7919);
            double const value = ReactionRawMs(state, hash);
            raw.push_back(value);
            sum += std::log(value);
            sumSquares += std::log(value) * std::log(value);
            uint32 const clamped = ReactionMs(state, params.MinMs, hash);
            belowMin += clamped < params.MinMs ? 1 : 0;
            raised += value < params.MinMs ? 1 : 0;
            stable = stable && clamped == ReactionMs(state, params.MinMs, hash);
        }

        std::nth_element(raw.begin(), raw.begin() + Draws / 2, raw.end());
        double const measuredMedian = raw[Draws / 2];
        double const mean = sum / Draws;
        double const measuredSigma = std::sqrt(std::max(0.0, sumSquares / Draws - mean * mean));
        check(within(measuredMedian, median, 0.05),
              std::format("skill {}: median {:.1f} ms, configured {:.1f}", skill, measuredMedian, median));
        check(sigma == 0.0 ? measuredSigma < 1e-6 : within(measuredSigma, sigma, 0.05),
              std::format("skill {}: sigma {:.4f}, configured {:.4f}", skill, measuredSigma, sigma));
        check(belowMin == 0 && stable,
              std::format("skill {}: {} draws under the minimum, stable {}", skill, belowMin, stable ? "yes" : "no"));
        say(
            std::format("skill {}: median {:.1f} ms (configured {:.1f}), sigma "
                        "{:.4f} ({:.4f}), raised to the minimum {:.1f} %",
                        skill, measuredMedian, median, measuredSigma, sigma, raised * 100.0 / Draws));
    }

    // Behind adds its penalty and a change of focus its cost, each within its range
    ReactionState const middle = ComputeReactionState(params, 50, exponent);
    bool behindOk = true;
    bool switchOk = true;
    double behindSum = 0.0;
    double switchSum = 0.0;
    for (uint32 draw = 0; draw < Draws; ++draw)
    {
        uint64 const hash = SplitMix64(draw ^ ReactionSalt::Event);
        uint32 const plain = EventReactionMs(middle, params, hash, false, false);
        uint32 const behind = EventReactionMs(middle, params, hash, true, false) - plain;
        uint32 const switched = EventReactionMs(middle, params, hash, false, true) - plain;
        behindOk = behindOk && behind >= params.BehindMinMs && behind <= params.BehindMaxMs;
        switchOk = switchOk && switched >= params.SwitchMinMs && switched <= params.SwitchMaxMs;
        behindSum += behind;
        switchSum += switched;
    }

    double const behindMid = (params.BehindMinMs + params.BehindMaxMs) / 2.0;
    double const switchMid = (params.SwitchMinMs + params.SwitchMaxMs) / 2.0;
    check(behindOk && (behindMid == 0.0 || within(behindSum / Draws, behindMid, 0.05)),
          std::format("behind penalty mean {:.1f} ms, expected {:.1f}", behindSum / Draws, behindMid));
    check(switchOk && (switchMid == 0.0 || within(switchSum / Draws, switchMid, 0.05)),
          std::format("switch cost mean {:.1f} ms, expected {:.1f}", switchSum / Draws, switchMid));

    // The event: blocked until its reaction time, then it passes and takes the focus; a quiet gap after the notice
    // starts the next event
    constexpr uint64 Guid = 4242;
    ReactionFocus focus;
    ReactionEvent event;
    StartReactionEvent(event, focus, middle, params, Guid, 1, 0, false, 10000);
    uint32 const ready = event.ReadyAfterMs;
    check(ready == EventReactionMs(middle, params, ReactionEventHash(Guid, 1, 10000), false, false),
          "the event's reaction time is not the plain draw");
    check(!ReactionEventPasses(event, focus, 1, 10000 + ready - 1) && focus.Category == 0,
          "passes before its reaction time");
    check(ReactionEventPasses(event, focus, 1, 10000 + ready) && focus.Category == 1, "blocked after its time");
    uint32 const passedAt = 10000 + ready;
    check(!ReactionEventStarts(event, passedAt + REACTION_NOTICED_GAP_MS, MissWindowMs) &&
              ReactionEventStarts(event, passedAt + REACTION_NOTICED_GAP_MS + 1, MissWindowMs),
          "a noticed event does not end after its quiet gap");
    check(ReactionEventPasses(event, focus, 1, passedAt + 300) && focus.AtMs == passedAt,
          "a later pass of a noticed event moves the focus stamp");

    // A trigger checked once a second keeps its pending event and passes at the first query after the time
    ReactionEvent sparse;
    ReactionFocus none;
    StartReactionEvent(sparse, none, middle, params, Guid, 2, 0, false, 50000);
    uint32 now = 50000;
    bool restarted = false;
    while (!ReactionEventPasses(sparse, none, 2, now) && now < 80000)
    {
        now += 1000;
        restarted = restarted || ReactionEventStarts(sparse, now, MissWindowMs);
    }

    check(!restarted && sparse.Noticed && now - 50000 < sparse.ReadyAfterMs + 1000,
          std::format("a query a second: passed after {} ms, reaction {} ms, restarted {}", now - 50000,
                      sparse.ReadyAfterMs, restarted ? "yes" : "no"));

    // Focus: another category within FocusTimeoutMs pays the switch cost, later not; behind adds its penalty
    uint32 const soon = passedAt + 100;
    ReactionEvent other;
    StartReactionEvent(other, focus, middle, params, Guid, 3, 0, false, soon);
    check(params.FocusTimeoutMs <= 100 ||
              other.ReadyAfterMs == EventReactionMs(middle, params, ReactionEventHash(Guid, 3, soon), false, true),
          "a change of focus does not pay the switch cost");
    uint32 const late = passedAt + params.FocusTimeoutMs;
    StartReactionEvent(other, focus, middle, params, Guid, 3, 0, false, late);
    uint32 const plainLate = EventReactionMs(middle, params, ReactionEventHash(Guid, 3, late), false, false);
    check(other.ReadyAfterMs == plainLate, "the switch cost outlives FocusTimeoutMs");
    StartReactionEvent(other, focus, middle, params, Guid, 3, 0, true, late);
    check(other.ReadyAfterMs >= plainLate + params.BehindMinMs && other.ReadyAfterMs <= plainLate + params.BehindMaxMs,
          "an event behind the bot does not add its penalty");

    // Misses: D4's chance, per event; a missed event never passes and starts again after the window
    uint32 missed = 0;
    for (uint32 draw = 0; draw < Draws; ++draw)
    {
        ReactionEvent trial;
        StartReactionEvent(trial, none, middle, params, Guid + draw, 4, 30, false, draw * 3);
        missed += trial.Missed ? 1 : 0;
    }

    check(within(missed * 100.0 / Draws, 30.0, 0.05),
          std::format("miss share {:.2f} % at 30 %", missed * 100.0 / Draws));
    ReactionEvent lost;
    StartReactionEvent(lost, none, middle, params, Guid, 4, 95, false, 0);
    for (uint32 start = 1; !lost.Missed && start < 100; ++start)
        StartReactionEvent(lost, none, middle, params, Guid, 4, 95, false, start);

    uint32 const lostAt = lost.StartMs;
    check(lost.Missed && !ReactionEventPasses(lost, none, 4, lostAt + MissWindowMs - 100) &&
              !ReactionEventStarts(lost, lostAt + MissWindowMs - 1, MissWindowMs) &&
              ReactionEventStarts(lost, lostAt + MissWindowMs, MissWindowMs),
          "a missed event passes, or does not start again after its window");

    // End to end: the target sum, never a pass before the event's server time, the latency correction on check streams
    ReactionParams endToEnd = params;
    endToEnd.EndToEnd = true;
    ReactionParams firstQuery = params;
    firstQuery.EndToEnd = false;
    constexpr uint32 TickMs = 25;
    ReactionFocus timing;
    ReactionEvent timed;
    StartReactionEvent(timed, timing, middle, endToEnd, Guid, 1, 0, false, 70000);
    StampReactionEvent(timed, timing, endToEnd, 120, ReactionStampSource::CastBar, TickMs);
    uint32 const sum = timed.ReadyAfterMs + endToEnd.PingMs + endToEnd.DisplayMs + TickMs;
    check(timed.TargetMs == sum && timed.StampAgeMs == 120 && timed.DueAfterMs == (sum > 120 ? sum - 120 : 0),
          std::format("end-to-end target {} ms due after {} ms; expected reaction {} + ping {} + display {} + "
                      "ticks {} = {}, 120 ms of it gone",
                      timed.TargetMs, timed.DueAfterMs, timed.ReadyAfterMs, endToEnd.PingMs, endToEnd.DisplayMs, TickMs,
                      sum));
    check((timed.DueAfterMs == 0 || !ReactionEventPasses(timed, timing, 1, 70000 + timed.DueAfterMs - 1)) &&
              ReactionEventPasses(timed, timing, 1, 70000 + timed.DueAfterMs),
          "an end-to-end event does not pass at its due time");
    ReactionEvent plain;
    StartReactionEvent(plain, timing, middle, firstQuery, Guid, 2, 0, false, 70000);
    StampReactionEvent(plain, timing, firstQuery, 120, ReactionStampSource::CastBar, TickMs);
    check(plain.TargetMs == plain.ReadyAfterMs && plain.DueAfterMs == plain.ReadyAfterMs && plain.StampAgeMs == 0,
          "without EndToEnd the event does not count from its first query");
    ReactionEvent stale;
    StartReactionEvent(stale, timing, middle, endToEnd, Guid, 2, 0, false, 70000);
    StampReactionEvent(stale, timing, endToEnd, REACTION_MAX_STAMP_AGE_MS + 1, ReactionStampSource::Aura, TickMs);
    check(stale.StampAgeMs == 0 && stale.StampSource == static_cast<uint8>(ReactionStampSource::Stale),
          "a server time older than the stamp age limit is kept");

    // No pass before the event's server time: a latency beyond the target passes at the first query and the early
    // pass lowers it; an event first seen after its target passes at once and leaves the latency alone
    ReactionFocus slow;
    slow.LatencyMs = 5000.0f;
    slow.LatencySeeded = true;
    ReactionEvent eager;
    StartReactionEvent(eager, slow, middle, endToEnd, Guid, 3, 0, false, 80000);
    StampReactionEvent(eager, slow, endToEnd, 0, ReactionStampSource::CastBar, TickMs);
    check(eager.DueAfterMs == 0 && eager.Corrected && ReactionEventPasses(eager, slow, 3, 80000) &&
              slow.LatencyMs < 5000.0f,
          "a latency beyond the target moves the pass before the first query, or the early pass keeps it");
    float const kept = slow.LatencyMs;
    ReactionEvent seenLate;
    StartReactionEvent(seenLate, slow, middle, endToEnd, Guid, 4, 0, false, 90000);
    StampReactionEvent(seenLate, slow, endToEnd, REACTION_MAX_STAMP_AGE_MS, ReactionStampSource::CastBar, TickMs);
    check(seenLate.TargetMs > REACTION_MAX_STAMP_AGE_MS ||
              (!seenLate.Corrected && seenLate.DueAfterMs == 0 && ReactionEventPasses(seenLate, slow, 4, 90000) &&
               slow.LatencyMs == kept),
          "an event first seen after its target waits, or its pass trains the latency");

    // Check streams: the first check after a server event within one interval, then one every interval plus a
    // jitter; after a warm-up the pass lands on the target on average (the bot's latency is learnt)
    struct StreamResult
    {
        double MeanErrorMs = 0.0;
        float LatencyMs = 0.0f;
        uint32 Early = 0;
    };
    auto const stream = [&](uint32 intervalMs, uint32 jitterMs, bool pauses, uint64 seed)
    {
        constexpr uint32 Events = 400;
        constexpr uint32 Warmup = 100;
        constexpr uint32 PauseMs = 2000;
        StreamResult result;
        ReactionFocus learner;
        double errorSum = 0.0;
        double latencySum = 0.0;
        uint32 counted = 0;
        uint32 clock = 100000;
        for (uint32 index = 0; index < Events; ++index)
        {
            uint64 const hash = SplitMix64(seed + index);
            uint32 const stamp = clock;
            uint32 now = stamp + static_cast<uint32>(hash % intervalMs);
            ReactionEvent trial;
            StartReactionEvent(trial, learner, middle, endToEnd, seed, 1, 0, false, now);
            StampReactionEvent(trial, learner, endToEnd, now - stamp, ReactionStampSource::CastBar, TickMs);
            // Every tenth event of a pausing stream waits out a cast of the bot's own before its next check
            bool const paused = pauses && index % 10 == 5;
            for (uint32 step = 0;
                 step < 1000 && !ReactionEventPasses(trial, learner, 1, now, 2 * (intervalMs + jitterMs)); ++step)
                now += paused && step == 0 ? PauseMs
                                           : intervalMs + static_cast<uint32>(SplitMix64(hash ^ step) % (jitterMs + 1));

            if (static_cast<int32>(now - stamp) < 0)
                ++result.Early;

            if (index >= Warmup && !paused)
            {
                errorSum += static_cast<double>(now - stamp) - trial.TargetMs;
                latencySum += learner.LatencyMs;
                ++counted;
            }

            clock = now + 3000;
        }

        result.MeanErrorMs = errorSum / counted;
        result.LatencyMs = static_cast<float>(latencySum / counted);
        return result;
    };
    struct StreamCase
    {
        uint32 IntervalMs;
        uint32 JitterMs;
        bool Pauses;
    };
    // The cadence (100 ms, a 20 ms stagger, 25 ms ticks), with the bot's own casts, and today's 250 ms + 0-200 ms
    for (StreamCase const& streamCase :
         {StreamCase{100, 25, false}, StreamCase{120, 25, true}, StreamCase{450, 50, false}})
    {
        StreamResult const result =
            stream(streamCase.IntervalMs, streamCase.JitterMs, streamCase.Pauses, 7000 + streamCase.IntervalMs);
        double const meanGap = streamCase.IntervalMs + streamCase.JitterMs / 2.0;
        check(result.Early == 0 && std::fabs(result.MeanErrorMs) <= 0.1 * meanGap &&
                  result.LatencyMs >= 0.25 * meanGap && result.LatencyMs <= 0.75 * meanGap,
              std::format("check stream {} ms + 0-{} ms{}: mean pass - target {:.1f} ms, mean latency {:.1f} ms, "
                          "{} passes before the server time",
                          streamCase.IntervalMs, streamCase.JitterMs, streamCase.Pauses ? " with casts" : "",
                          result.MeanErrorMs, result.LatencyMs, result.Early));
    }

    // The cadence's stagger: upstream's guid % 201 at 200, at most StaggerMs
    bool staggerOk = true;
    for (uint64 const guid : {1ULL, 200ULL, 201ULL, 4242ULL, 987654321ULL})
        staggerOk = staggerOk && CheckStaggerMs(guid, UPSTREAM_CHECK_STAGGER_MS) == guid % 201 &&
                    CheckStaggerMs(guid, 20) <= 20 && CheckStaggerMs(guid, 0) == 0;

    check(staggerOk, "the check stagger is not upstream's guid % 201 at 200, or exceeds StaggerMs");

    // The cure and dispel triggers and the party dispel value: the cadence's interval while it runs, when faster
    check(ReactionCheckIntervalMs(1000, true, 100) == 100 && ReactionCheckIntervalMs(50, true, 100) == 50 &&
              ReactionCheckIntervalMs(1, true, 100) == 1,
          "a reaction trigger under the cadence does not check at min(its interval, CheckMs)");
    check(ReactionCheckIntervalMs(1000, false, 100) == 1000 && ReactionCheckIntervalMs(1, false, 20) == 1,
          "a reaction trigger without the cadence does not keep its own interval");

    // G0h3's reaction types, on a copy of the configured prices
    {
        ReactionParams on = params;
        // The load and anticipation checks need prices: fixed ones (the dist's), so a configured 0 cannot void them
        on.Load.MsPerEventAt1 = 200;
        on.Load.MsPerEventAt99 = 80;
        on.Load.MissPercentPerEventAt1 = 10;
        on.Load.MissPercentPerEventAt99 = 2;
        on.Load.TopProtectAt1 = 0.0f;
        on.Load.TopProtectAt99 = 0.8f;
        on.Anticipate.ShareAt1 = 0.1f;
        on.Anticipate.ShareAt99 = 0.5f;
        auto const pricesAt = [&](uint8 attribute)
        {
            ReactionTypeInputs inputs;
            inputs.Speed = attribute;
            inputs.Wisdom = attribute;
            inputs.Focus = attribute;
            inputs.Dexterity = attribute;
            inputs.Foresight = attribute;
            return ComputeReactionTypeState(on, inputs, exponent);
        };
        ReactionTypeState const low = pricesAt(1);
        ReactionTypeState const mid = pricesAt(50);
        ReactionTypeState const high = pricesAt(99);
        ReactionFocus const fresh;
        auto const eventHash = [](uint32 draw)
        { return ReactionEventHash(Guid + draw, static_cast<uint8>(1 + draw % 6), 200000 + draw * 13); };
        auto const startAt = [&](ReactionParams const& with, ReactionTypeState const& prices,
                                 ReactionEventInputs const& inputs, uint32 draw, uint32 missPercent)
        {
            ReactionEvent event;
            StartReactionEvent(event, fresh, middle, with, Guid + draw, static_cast<uint8>(1 + draw % 6), missPercent,
                               false, 200000 + draw * 13, prices, inputs);
            return event;
        };
        uint32 const commonMs = EaseCostMs(mid, ReactionEase::Common);

        // The default inputs and prices add nothing, the default inputs at the bot's prices the common button's price
        bool unchanged = true;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            uint64 const hash = eventHash(draw);
            ReactionEvent const before = startAt(on, ReactionTypeState(), ReactionEventInputs(), draw, 20);
            ReactionEvent const plainOn = startAt(on, mid, ReactionEventInputs(), draw, 20);
            unchanged = unchanged && before.ReadyAfterMs == EventReactionMs(middle, on, hash, false, false) &&
                        before.Missed == (SplitMix64(hash ^ ReactionSalt::Miss) % 100 < 20) && before.CostMs == 0 &&
                        plainOn.ReadyAfterMs == before.ReadyAfterMs + commonMs &&
                        plainOn.Type == static_cast<uint8>(ReactionType::Simple);
        }

        check(unchanged, "with the default inputs the event is not G0h2's");

        // Choice: every doubling of the options adds the price (+-5 %, at least 1 ms of rounding), less at high Wisdom
        uint32 const maxOptions = on.Choice.MaxOptions;
        auto const hickOk = [&](ReactionTypeState const& prices)
        {
            bool ok = HickCostMs(prices, 0, maxOptions) == 0 && HickCostMs(prices, 1, maxOptions) == 0 &&
                      HickCostMs(prices, maxOptions * 2, maxOptions) == HickCostMs(prices, maxOptions, maxOptions);
            for (uint32 options = 1; options * 2 <= maxOptions; options *= 2)
            {
                double const added = static_cast<double>(HickCostMs(prices, options * 2, maxOptions)) -
                                     HickCostMs(prices, options, maxOptions);
                ok = ok && std::fabs(added - prices.HickMsPerDoubling) <=
                               std::max(1.0, 0.05 * static_cast<double>(prices.HickMsPerDoubling));
            }

            return ok;
        };
        check(hickOk(low) && hickOk(mid) && hickOk(high) &&
                  (low.HickMsPerDoubling == 0.0f || high.HickMsPerDoubling < low.HickMsPerDoubling),
              std::format("doubling the options does not add {:.1f} / {:.1f} / {:.1f} ms at Wisdom 1 / 50 / 99 "
                          "(+-5 %), or not less at high Wisdom",
                          low.HickMsPerDoubling, mid.HickMsPerDoubling, high.HickMsPerDoubling));
        ReactionEventInputs four;
        four.Options = 4;
        ReactionEvent const single = startAt(on, mid, ReactionEventInputs(), 7, 0);
        ReactionEvent const chosen = startAt(on, mid, four, 7, 0);
        check(chosen.Type == static_cast<uint8>(ReactionType::Choice) && chosen.Options == 4 &&
                  single.Type == static_cast<uint8>(ReactionType::Simple) &&
                  chosen.ReadyAfterMs - single.ReadyAfterMs == HickCostMs(mid, 4, maxOptions),
              "a choice of four options does not add Hick's cost to the reaction");

        // Load: a second pending event is answered slower and missed more (at a 10 % base), less so at high Focus,
        // and the top event keeps the protected share off
        struct LoadResult
        {
            double AddedMs = 0.0;
            double AddedMissPercent = 0.0;
            double TopAddedMs = 0.0;
        };
        auto const loadAt = [&](ReactionTypeState const& prices)
        {
            ReactionEventInputs loaded;
            loaded.Load = 1;
            ReactionEventInputs top = loaded;
            top.LoadTop = true;
            double alone = 0.0;
            double withLoad = 0.0;
            double withTop = 0.0;
            uint32 missedAlone = 0;
            uint32 missedLoaded = 0;
            for (uint32 draw = 0; draw < Draws; ++draw)
            {
                ReactionEvent const first = startAt(on, prices, ReactionEventInputs(), draw, 10);
                ReactionEvent const second = startAt(on, prices, loaded, draw, 10);
                alone += first.ReadyAfterMs;
                withLoad += second.ReadyAfterMs;
                withTop += startAt(on, prices, top, draw, 10).ReadyAfterMs;
                missedAlone += first.Missed ? 1 : 0;
                missedLoaded += second.Missed ? 1 : 0;
            }

            LoadResult result;
            result.AddedMs = (withLoad - alone) / Draws;
            result.AddedMissPercent = (static_cast<double>(missedLoaded) - missedAlone) * 100.0 / Draws;
            result.TopAddedMs = (withTop - alone) / Draws;
            return result;
        };
        LoadResult const loadLow = loadAt(low);
        LoadResult const loadHigh = loadAt(high);
        auto const loadMatches = [](LoadResult const& result, ReactionTypeState const& prices)
        {
            return result.AddedMs > 0.0 && std::fabs(result.AddedMs - prices.LoadMsPerEvent) < 0.5 &&
                   result.AddedMissPercent > 0.0 &&
                   std::fabs(result.AddedMissPercent - prices.LoadMissPercentPerEvent) <= 1.0;
        };
        check(loadMatches(loadLow, low) && loadMatches(loadHigh, high),
              std::format("a second pending event adds {:.1f} ms and {:.2f} % misses at Focus 1 (configured {} "
                          "ms, {} %), {:.1f} ms and {:.2f} % at Focus 99 ({} ms, {} %)",
                          loadLow.AddedMs, loadLow.AddedMissPercent, low.LoadMsPerEvent, low.LoadMissPercentPerEvent,
                          loadHigh.AddedMs, loadHigh.AddedMissPercent, high.LoadMsPerEvent,
                          high.LoadMissPercentPerEvent));
        check(loadHigh.AddedMs < loadLow.AddedMs && loadHigh.AddedMissPercent < loadLow.AddedMissPercent &&
                  (high.LoadTopProtect == 0.0f || loadHigh.TopAddedMs < loadHigh.AddedMs),
              std::format("the load is not lighter at high Focus, or the top event not protected (top +{:.1f} "
                          "ms)",
                          loadHigh.TopAddedMs));
        say(
            std::format("load +{:.0f} ms / +{:.1f} % misses per event at Focus 1, +{:.0f} / +{:.1f} at "
                        "99 (top event +{:.0f} ms)",
                        loadLow.AddedMs, loadLow.AddedMissPercent, loadHigh.AddedMs, loadHigh.AddedMissPercent,
                        loadHigh.TopAddedMs));

        // Ease: common < rare < focus target < new target at every Dexterity (the focus target at a new target's
        // price below FocusDexterity), and the price lands in the reaction
        bool easeOrder = true;
        for (uint8 dexterity : {1, 20, 39, 40, 41, 60, 80, 99})
        {
            ReactionTypeInputs inputs;
            inputs.Dexterity = dexterity;
            ReactionTypeState const prices = ComputeReactionTypeState(on, inputs, exponent);
            uint32 const common = EaseCostMs(prices, ReactionEase::Common);
            uint32 const rare = EaseCostMs(prices, ReactionEase::Rare);
            uint32 const focusTarget = EaseCostMs(prices, ReactionEase::Focus);
            uint32 const newTarget = EaseCostMs(prices, ReactionEase::NewTarget);
            bool const focusKnown = dexterity >= on.Ease.FocusDexterity;
            easeOrder = easeOrder && common < rare && rare < focusTarget &&
                        (focusKnown ? focusTarget < newTarget : focusTarget == newTarget);
        }

        ReactionEventInputs rareButton;
        rareButton.Ease = ReactionEase::Rare;
        check(easeOrder && startAt(on, mid, rareButton, 7, 0).ReadyAfterMs - single.ReadyAfterMs ==
                               EaseCostMs(mid, ReactionEase::Rare) - commonMs,
              "the ease order is not common < rare < focus target < new target at every Dexterity, or a rare "
              "button's price is not in the reaction");

        // Anticipation: a predicted cast saves the configured share of the draw (Foresight 50, +-5 %), never below
        // the minimum, and is no choice
        ReactionEventInputs predicted;
        predicted.Predicted = true;
        predicted.Options = 4;
        bool anticipatedOk = true;
        double drawSum = 0.0;
        double shortenedSum = 0.0;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            uint32 const full = ReactionMs(middle, on.MinMs, eventHash(draw));
            ReactionEvent const plainEvent = startAt(on, mid, ReactionEventInputs(), draw, 0);
            ReactionEvent const ahead = startAt(on, mid, predicted, draw, 0);
            uint32 const shortened = ahead.ReadyAfterMs - (plainEvent.ReadyAfterMs - full);
            anticipatedOk = anticipatedOk && ahead.Type == static_cast<uint8>(ReactionType::Anticipated) &&
                            ahead.ReadyAfterMs <= plainEvent.ReadyAfterMs && shortened >= on.MinMs;
            if (full * (1.0 - mid.AnticipateShare) >= on.MinMs + 1.0)
            {
                drawSum += full;
                shortenedSum += shortened;
            }
        }

        double const saved = drawSum > 0.0 ? 1.0 - shortenedSum / drawSum : 0.0;
        check(anticipatedOk && within(saved, mid.AnticipateShare, 0.05),
              std::format("a predicted cast saves {:.3f} of the draw, configured {:.3f} (Foresight 50)", saved,
                          mid.AnticipateShare));

        // Thinking: the shortcut's cost lands in the reaction, a deep check charged later moves the pass, the deep
        // share follows Wisdom, the wait Patience
        ReactionEventInputs thinking;
        thinking.Thinks = true;
        ReactionEvent const quick = startAt(on, mid, thinking, 11, 0);
        ReactionEvent const unthinking = startAt(on, mid, ReactionEventInputs(), 11, 0);
        ReactionEvent deep = quick;
        AddReactionThinkMs(deep, mid.ThinkDeepMs);
        ReactionEvent noticed = quick;
        noticed.Noticed = true;
        AddReactionThinkMs(noticed, 100);
        check(quick.ThinkMs == mid.ThinkShortcutMs &&
                  quick.ReadyAfterMs - unthinking.ReadyAfterMs == mid.ThinkShortcutMs &&
                  deep.ReadyAfterMs == quick.ReadyAfterMs + mid.ThinkDeepMs &&
                  deep.TargetMs == quick.TargetMs + mid.ThinkDeepMs &&
                  deep.DueAfterMs == quick.DueAfterMs + mid.ThinkDeepMs && noticed.ReadyAfterMs == quick.ReadyAfterMs &&
                  mid.ThinkShortcutMs < mid.ThinkDeepMs,
              "the think cost does not land in the reaction, or a deep check does not move the pass");
        uint32 deepChosen = 0;
        for (uint32 draw = 0; draw < Draws; ++draw)
            deepChosen += ChooseThinkDepth(mid, SplitMix64(draw * 7 + 3)) == ReactionThinkDepth::Deep ? 1 : 0;

        ReactionTypeInputs restless;
        restless.Patience = 0;
        ReactionTypeInputs patient;
        patient.Patience = 100;
        check(std::fabs(deepChosen * 100.0 / Draws - mid.ThinkDeepPercent) <= 1.5 &&
                  low.ThinkDeepPercent <= high.ThinkDeepPercent &&
                  ComputeReactionTypeState(on, restless, exponent).PatienceWaitMs == on.Think.PatienceWaitMsAt0 &&
                  ComputeReactionTypeState(on, patient, exponent).PatienceWaitMs == on.Think.PatienceWaitMsAt100,
              std::format("deep checks {:.1f} % at Wisdom 50 (configured {} %), or the wait not from Patience",
                          deepChosen * 100.0 / Draws, mid.ThinkDeepPercent));

        // The queue-like follow-up: after an own cast or GCD that ends before the next check, the check lands within
        // the draw of the end (exactly at it when that is before the regular check), never later than the regular one
        bool queueOk = true;
        for (ReactionTypeState const* prices : {&low, &high})
        {
            for (uint32 draw = 0; draw < Draws; ++draw)
            {
                uint64 const hash = SplitMix64(draw * 31 + 5);
                uint32 const drawMs = QueueDrawMs(*prices, hash);
                uint32 const remaining = static_cast<uint32>((hash >> 40) % 300);
                uint32 const delay = QueueCheckDelayMs(remaining, 100, drawMs);
                bool const endsFirst = remaining < 100;
                queueOk = queueOk && drawMs >= prices->QueueMinMs && drawMs <= prices->QueueMaxMs && delay <= 100 &&
                          (endsFirst ? delay >= remaining && delay - remaining <= drawMs &&
                                           (remaining + drawMs > 100 || delay == remaining + drawMs)
                                     : delay == 100);
            }
        }

        check(queueOk, std::format("the follow-up check does not land within its draw of the end ({}-{} ms at "
                                   "Speed 1, {}-{} ms at 99) or comes after the regular check",
                                   low.QueueMinMs, low.QueueMaxMs, high.QueueMinMs, high.QueueMaxMs));

        // The opponent memory: the fake rate after the casts and cancels seen (an interrupt is no cancel, the same
        // cast seen twice one cast), a repeated spell predicted, bounded to its slots, cleared for the next match
        uint32 const tolerance = params.Memory.EndToleranceMs;
        uint32 const casts = std::max(10u, params.Memory.MinCasts);
        OpponentMemory memory;
        memory.Clear(77);
        uint32 clock = 300000;
        uint32 cancels = 0;
        bool memoryOk = memory.MatchId == 77 && memory.Find(7) == nullptr;
        for (uint32 cast = 0; cast < casts; ++cast)
        {
            uint32 const end = clock + 2000;
            memoryOk = memoryOk && memory.Predicts(7, 100, end, tolerance) == (cast > 0);
            memory.NoteCast(7, 100, end, clock, tolerance);
            memory.NoteCast(7, 100, end, clock + 100, tolerance);
            memoryOk = memoryOk && memory.Predicts(7, 100, end, tolerance) == (cast > 0) &&
                       (memory.FakeRate(7, params.Memory.MinCasts) == 0.0f) ==
                           (cast + 1 < params.Memory.MinCasts || cancels == 0);
            if (cast % 3 == 0)
            {
                memory.NoteCastGone(7, clock + 500, tolerance, false);
                ++cancels;
            }
            else if (cast % 5 == 4)
                memory.NoteCastGone(7, clock + 600, tolerance, true);
            else
                memory.NoteCastGone(7, end + 50, tolerance, false);

            clock = end + 1000;
        }

        OpponentMemorySlot const* seen = memory.Find(7);
        float const rate = memory.FakeRate(7, params.Memory.MinCasts);
        memoryOk = memoryOk && seen && seen->CastsSeen == casts && seen->CancelsSeen == cancels &&
                   std::fabs(rate - static_cast<float>(cancels) / casts) < 1e-6f;
        // A new cast before the end of the one in view: that one was cancelled, and another spell is not predicted
        memory.NoteCast(9, 200, clock + 2000, clock, tolerance);
        memory.NoteCast(9, 300, clock + 2300, clock + 300, tolerance);
        OpponentMemorySlot const* switched = memory.Find(9);
        memoryOk = memoryOk && switched && switched->CastsSeen == 2 && switched->CancelsSeen == 1 &&
                   !memory.Predicts(9, 300, clock + 2300, tolerance);
        for (uint32 enemy = 1; enemy < OPPONENT_MEMORY_SLOTS; ++enemy)
            memory.NoteCast(1000 + enemy, 100, clock + 5000, clock + 1000 + enemy, tolerance);

        memoryOk = memoryOk && memory.Find(7) == nullptr && memory.Find(9) && memory.Find(1001) &&
                   memory.Find(1000 + OPPONENT_MEMORY_SLOTS - 1);
        memory.Clear(78);
        memoryOk = memoryOk && memory.MatchId == 78 && memory.Find(9) == nullptr && memory.Find(1001) == nullptr;
        check(memoryOk, std::format("the opponent memory: fake rate {:.3f} after {} cancels in {} casts, "
                                    "expected {:.3f}; or a repeat, the bound or the clear failed",
                                    rate, cancels, casts, static_cast<float>(cancels) / casts));

        // The busy bot: its own cast stops for a kick of an important cast only when it would end too late; no kick a
        // busy bot reaches only within the margin of the cast's end; the stop off by its flag
        ReactionReevalParams rules = params.Reeval;
        rules.Enable = true;
        uint32 const important = rules.ImportantCastMs;
        uint32 const margin = rules.BusyMarginMs;
        uint32 const ownLeft = std::max(rules.MinOwnRemainingMs, 1000u);
        ReactionReevalParams rulesOff = rules;
        rulesOff.Enable = false;
        check(StopOwnCastForKick(rules, important, ownLeft, ownLeft) &&
                  !StopOwnCastForKick(rules, important, ownLeft + margin + 1, ownLeft) &&
                  (important == 0 || !StopOwnCastForKick(rules, important - 1, ownLeft, ownLeft)) &&
                  (rules.MinOwnRemainingMs == 0 ||
                   !StopOwnCastForKick(rules, important, 1, rules.MinOwnRemainingMs - 1)) &&
                  !StopOwnCastForKick(rulesOff, important, ownLeft, ownLeft),
              "the own cast is not stopped for an important kick it would make too late, or is stopped otherwise");
        check(InterruptOutOfReach(rules, 2000, 1000, 2000 + margin) &&
                  InterruptOutOfReach(rules, 2000, -500, 2000 + margin) &&
                  !InterruptOutOfReach(rules, 1999, 1000, 2000 + margin) &&
                  !InterruptOutOfReach(rules, 2000, 2000, 2000 + margin),
              "a kick made late by being busy is not dropped, or one in reach or late from its reaction alone is");

        // Without the busy time: stamped, the unclipped due no earlier than the own cast's start; unstamped, the due
        // OwnMs earlier; no own cast, the due itself
        ReactionEvent busyEvent;
        busyEvent.TargetMs = 900;
        busyEvent.StampAgeMs = 1200;
        busyEvent.DueAfterMs = 0;
        busyEvent.OwnMs = 200;
        ReactionEvent freeEvent = busyEvent;
        freeEvent.StampAgeMs = 300;
        freeEvent.DueAfterMs = 550;
        freeEvent.OwnMs = 0;
        ReactionEvent unstampedEvent;
        unstampedEvent.DueAfterMs = 700;
        unstampedEvent.OwnMs = 300;
        check(BusyFreeDueMs(busyEvent, 50, true) == -200 && BusyFreeDueMs(freeEvent, 50, true) == 550 &&
                  BusyFreeDueMs(freeEvent, 50, false) == 550 && BusyFreeDueMs(unstampedEvent, 50, false) == 400,
              "the kick's due without the busy time is not the stamped or the unstamped one");

        // One stop per enemy cast: a bot whose kick never follows and that starts its next own cast at once, checked
        // every 100 ms through a 3 s enemy cast with a pushback, stops once; none in the backoff after that wasted
        // stop, one again after it; a followed stop leaves the next cast its own
        ReactionBusyState watch;
        uint32 stops = 0;
        uint32 const castStart = 10000;
        uint32 const castMs = 3000;
        for (uint32 now = castStart; now < castStart + castMs; now += 100)
        {
            uint32 const end = castStart + castMs + (now >= castStart + 1000 ? 250 : 0);
            if (!watch.Watches(7, 116, end, castMs / 2))
                watch.Watch(7, 116, end, castStart + 600);

            if (watch.StopDue(now))
            {
                watch.NoteStop(now, 7, 2139);
                watch.NoteStopUnfollowed(now);
                ++stops;
            }
        }

        uint32 const inBackoff = castStart + castMs + 500;
        watch.Watch(7, 116, inBackoff + castMs, inBackoff + 600);
        bool const backedOff = !watch.StopDue(inBackoff + 600) && !watch.StopDue(inBackoff + castMs);
        uint32 const after = castStart + 600 + REACTION_STOP_BACKOFF_MS;
        watch.Watch(7, 116, after + castMs, after);
        bool const again = watch.StopDue(after);
        watch.NoteStop(after, 7, 2139);
        bool const spent = !watch.StopDue(after + 100);
        watch.Watch(8, 116, after + 2 * castMs, after + castMs);
        check(stops == 1 && backedOff && again && spent && watch.StopDue(after + castMs) && watch.UnfollowedStops == 1,
              std::format("the busy stop is not once per enemy cast: {} stops in one cast, backoff {}, after it "
                          "{}, spent {}",
                          stops, backedOff ? "held" : "broken", again ? "due" : "not due", spent ? "yes" : "no"));

        // An unanswered event loads the others while its trigger asks, up to the pending gap after its last query;
        // a noticed or missed one does not
        ReactionEvent waiting;
        waiting.Active = true;
        waiting.StartMs = 1000;
        waiting.LastQueryMs = 1000;
        ReactionEvent answered = waiting;
        answered.Noticed = true;
        ReactionEvent lost = waiting;
        lost.Missed = true;
        check(ReactionEventPending(waiting, 1000 + REACTION_PENDING_GAP_MS) &&
                  !ReactionEventPending(waiting, 1001 + REACTION_PENDING_GAP_MS) &&
                  !ReactionEventPending(answered, 1000) && !ReactionEventPending(lost, 1000),
              "an unanswered event does not count as pending for exactly the pending gap");
        say(
            std::format("types at attribute 50: choice {:.0f} ms per doubling, load {} ms / {} %, ease "
                        "{}/{}/{}/{} ms, think {}/{} ms (deep {} %), anticipate {:.2f}, queue {}-{} ms",
                        mid.HickMsPerDoubling, mid.LoadMsPerEvent, mid.LoadMissPercentPerEvent, mid.EaseMs[0],
                        mid.EaseMs[1], mid.EaseMs[2], mid.EaseMs[3], mid.ThinkShortcutMs, mid.ThinkDeepMs,
                        mid.ThinkDeepPercent, mid.AnticipateShare, mid.QueueMinMs, mid.QueueMaxMs));
    }
}

#endif
