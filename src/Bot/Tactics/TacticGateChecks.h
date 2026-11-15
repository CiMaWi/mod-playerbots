/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TACTICGATECHECKS_H
#define PLAYERBOTS_TACTICGATECHECKS_H

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "BotAttributes.h"
#include "TacticGate.h"

// Pearson correlation of two equally long series (0 when one of them is constant)
inline double Correlation(std::vector<double> const& x, std::vector<double> const& y)
{
    double const n = static_cast<double>(x.size());
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double syy = 0.0;
    double sxy = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        sx += x[i];
        sy += y[i];
        sxx += x[i] * x[i];
        syy += y[i] * y[i];
        sxy += x[i] * y[i];
    }

    double const vx = sxx - sx * sx / n;
    double const vy = syy - sy * sy / n;
    if (vx <= 0.0 || vy <= 0.0)
        return 0.0;

    return (sxy - sx * sy / n) / std::sqrt(vx * vy);
}

// Pure checks of the skill gate (TacticGate.h) on synthetic guids, skills and times; no bots, no config
template <typename Check, typename Say>
void RunGateChecks(Check&& check, Say&& say)
{
    constexpr uint64 FirstGuid = 1000;
    constexpr uint32 Guids = 1000;
    constexpr uint32 WindowMs = 1500;

    // Curve: at e = 1 every lerp is the one before G0g, bit for bit (D3's float factor, D4's rounded numbers)
    uint32 lerpMismatches = 0;
    for (uint32 skill = 1; skill <= 99; ++skill)
    {
        float const position = (static_cast<float>(skill) - 1.0f) / 98.0f;
        auto const oldLerp = [position](uint32 low, uint32 high)
        { return static_cast<uint32>(low + (static_cast<float>(high) - low) * position + 0.5f); };
        if (SkillPosition(static_cast<uint8>(skill)) != position ||
            SkillLerp(3.0f, 0.5f, position, 1.0f) != 3.0f + (0.5f - 3.0f) * position ||
            SkillLerpRounded(60, 0, position, 1.0f) != oldLerp(60, 0) ||
            SkillLerpRounded(3000, 200, position, 1.0f) != oldLerp(3000, 200))
            ++lerpMismatches;
    }

    check(lerpMismatches == 0, std::format("e = 1 differs from the old lerp at {} skills", lerpMismatches));
    // The plan's "about 76 %": 1 - (1 - 0.5)^2 = 0.75 exactly
    float const half = SkillCurve(SkillPosition(50), 2.0f);
    say(std::format("e = 2, skill 50 sits {:.4f} of the way", half));
    check(std::fabs(half - 0.75f) < 0.02f, std::format("e = 2 at skill 50: {:.4f}, not 0.75", half));
    check(SkillCurve(0.0f, 2.0f) == 0.0f && SkillCurve(1.0f, 2.0f) == 1.0f && SkillCurve(0.0f, 0.5f) == 0.0f &&
              SkillCurve(1.0f, 0.5f) == 1.0f,
          "curve ends are not 0 and 1");
    bool monotonic = true;
    for (uint32 skill = 2; skill <= 99; ++skill)
        for (float const exponent : {0.5f, 2.0f, 3.0f})
            if (SkillCurve(SkillPosition(static_cast<uint8>(skill)), exponent) <
                SkillCurve(SkillPosition(static_cast<uint8>(skill - 1)), exponent))
                monotonic = false;

    check(monotonic, "curve not monotonic");
    check(SkillCurve(0.5f, 0.5f) < 0.5f, "e = 0.5 does not bend the other way");

    // Knowledge: 1000 synthetic guids, jitter -15..+15 around the threshold, mean near 0, patchy per tactic
    int32 minJitter = 1000;
    int32 maxJitter = -1000;
    int64 jitterSum = 0;
    uint32 patchy = 0;
    uint32 spreadZero = 0;
    uint32 baseZero = 0;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        int32 const jitter = static_cast<int32>(TacticKnownFrom(50, 15, guid, TacticId::Hold)) - 50;
        minJitter = std::min(minJitter, jitter);
        maxJitter = std::max(maxJitter, jitter);
        jitterSum += jitter;
        if (TacticKnownFrom(50, 15, guid, TacticId::Kite) != TacticKnownFrom(50, 15, guid, TacticId::Hold))
            ++patchy;

        if (TacticKnownFrom(50, 0, guid, TacticId::Hold) != 50)
            ++spreadZero;

        if (TacticKnownFrom(0, 15, guid, TacticId::Hold) != 0)
            ++baseZero;
    }

    double const jitterMean = static_cast<double>(jitterSum) / Guids;
    say(std::format("knowledge jitter over {} guids: {} to {}, mean {:.2f}", Guids, minJitter, maxJitter, jitterMean));
    check(minJitter == -15 && maxJitter == 15,
          std::format("knowledge jitter {} to {}, not -15 to 15", minJitter, maxJitter));
    check(std::fabs(jitterMean) < 1.5, std::format("knowledge jitter mean {:.2f} (tolerance 1.5)", jitterMean));
    check(patchy >= 900, std::format("knowledge not patchy: {} of {} differ between two tactics", patchy, Guids));
    check(spreadZero == 0, "spread 0 still jitters");
    check(baseZero == 0, "threshold 0 is not known by every bot");

    // Use chance per window: stable inside a window, the configured rate over 10 000 windows
    TacticSettings settings;
    settings.UseChanceAtSkill1 = 20;
    settings.UseChanceAtSkill99 = 90;
    check(TacticUseChance(settings, 1, 1.0f) == 20 && TacticUseChance(settings, 99, 1.0f) == 90 &&
              TacticUseChance(settings, 50, 1.0f) == 55 && TacticUseChance(settings, 0, 1.0f) == 100,
          "use chance lerp (20 at 1, 55 at 50, 90 at 99, 100 at skill 0)");
    check(TacticUseChance(settings, 50, 2.0f) == 73, "use chance at e = 2, skill 50 is not 73");
    for (uint32 const chance : {5u, 20u, 50u, 90u})
    {
        uint64 const guid = FirstGuid + chance;
        uint32 used = 0;
        for (uint32 window = 0; window < 10000; ++window)
            if (TacticUsedInWindow(guid, TacticId::Hold, chance, WindowMs, 12345 + window * WindowMs))
                ++used;

        double const rate = used / 100.0;
        say(std::format("chance {} %: used in {:.2f} % of 10000 windows", chance, rate));
        check(std::fabs(rate - chance) <= 3.0,
              std::format("chance {} %: rate {:.2f} % over 10000 windows (tolerance 3 points)", chance, rate));
    }

    uint32 unstable = 0;
    uint32 windows = 0;
    uint32 usedWindows = 0;
    {
        uint64 const guid = FirstGuid + 7;
        uint64 lastWindow = TacticUseWindow(guid, WindowMs, 0);
        bool lastAnswer = TacticUsedInWindow(guid, TacticId::Kite, 50, WindowMs, 0);
        for (uint32 now = 37; now < 600000; now += 37)
        {
            uint64 const window = TacticUseWindow(guid, WindowMs, now);
            bool const answer = TacticUsedInWindow(guid, TacticId::Kite, 50, WindowMs, now);
            if (window == lastWindow && answer != lastAnswer)
                ++unstable;

            if (window != lastWindow)
            {
                ++windows;
                usedWindows += answer ? 1 : 0;
            }

            lastWindow = window;
            lastAnswer = answer;
        }
    }

    check(unstable == 0, std::format("{} answers changed inside a window", unstable));
    check(windows >= 390 && usedWindows > 0 && usedWindows < windows,
          std::format("{} windows, {} used: the answer does not change between windows", windows, usedWindows));
    check(!TacticUsedInWindow(FirstGuid, TacticId::Hold, 0, WindowMs, 0) &&
              TacticUsedInWindow(FirstGuid, TacticId::Hold, 100, WindowMs, 0),
          "chance 0 used or chance 100 not used");

    // Effort: defaults are full effort at every skill; a range lerps and jitters within its spread, clamped
    TacticGateParams params;
    uint32 notFull = 0;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
        if (SkillEffort(params, static_cast<uint8>(1 + guid % 99), guid) != SKILL_GATE_FULL)
            ++notFull;

    check(notFull == 0, std::format("default effort below 100 for {} bots", notFull));
    params.EffortAtSkill1 = 20;
    params.EffortAtSkill99 = 90;
    check(SkillEffort(params, 1, FirstGuid) == 20 && SkillEffort(params, 99, FirstGuid) == 90 &&
              SkillEffort(params, 0, FirstGuid) == SKILL_GATE_FULL,
          "effort lerp (20 at 1, 90 at 99, 100 at skill 0)");
    params.EffortSpread = 10;
    int32 minEffort = 1000;
    int32 maxEffort = -1000;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        int32 const effort = SkillEffort(params, 50, guid);
        minEffort = std::min(minEffort, effort);
        maxEffort = std::max(maxEffort, effort);
    }

    check(minEffort == 45 && maxEffort == 65,
          std::format("effort at skill 50, spread 10: {} to {}, not 45 to 65", minEffort, maxEffort));
    params.EffortAtSkill1 = 0;
    params.EffortSpread = 50;
    bool clamped = true;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
        if (SkillEffort(params, 1, guid) > 50)
            clamped = false;

    check(clamped, "effort below 0 wraps instead of clamping");

    // Style: a function of the guid only. Over 1000 synthetic guids, traits against skill (BotProfileMgr's
    // guid-seeded percentiles through the uniform distribution, and plain skills 1..99) and against each other:
    // |r| < 0.1 (about three standard errors at n = 1000)
    params = TacticGateParams();
    params.StyleSpread = 30;
    std::vector<double> seeded;
    std::vector<double> plain;
    std::array<std::vector<double>, SKILL_STYLE_TRAIT_COUNT> traits;
    int32 minTrait = 1000;
    int32 maxTrait = -1000;
    uint32 skillDependent = 0;
    std::array<TacticSettings, TACTIC_COUNT> const tactics{};
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        seeded.push_back(BotProfileMgr::SkillFromPercentile(BotProfileMgr::LegacyPercentile(static_cast<uint32>(guid)),
                                                            true, 50.0, 15.0, 1.0, 99.0));
        plain.push_back(static_cast<double>(1 + guid % 99));
        for (uint32 trait = 0; trait < SKILL_STYLE_TRAIT_COUNT; ++trait)
        {
            int32 const value = SkillStyle(params.StyleSpread, guid, static_cast<SkillStyleTrait>(trait));
            traits[trait].push_back(value);
            minTrait = std::min(minTrait, value);
            maxTrait = std::max(maxTrait, value);
        }

        if (ComputeTacticGate(params, tactics, 1, guid).Style != ComputeTacticGate(params, tactics, 99, guid).Style)
            ++skillDependent;
    }

    check(minTrait == 20 && maxTrait == 80,
          std::format("style traits {} to {}, not 20 to 80 at spread 30", minTrait, maxTrait));
    check(skillDependent == 0, std::format("style changes with skill for {} guids", skillDependent));
    double worst = 0.0;
    for (uint32 trait = 0; trait < SKILL_STYLE_TRAIT_COUNT; ++trait)
    {
        worst = std::max(worst, std::fabs(Correlation(traits[trait], seeded)));
        worst = std::max(worst, std::fabs(Correlation(traits[trait], plain)));
        worst = std::max(worst, std::fabs(Correlation(traits[trait], traits[(trait + 1) % SKILL_STYLE_TRAIT_COUNT])));
    }

    say(std::format("style over {} guids: traits {} to {}, largest |r| {:.3f}", Guids, minTrait, maxTrait, worst));
    check(worst < 0.1, std::format("style correlates with skill or another trait: |r| {:.3f}", worst));
    check(SkillStyle(0, FirstGuid, SkillStyleTrait::Caution) == SKILL_STYLE_NEUTRAL, "style spread 0 is not 50");

    // The whole gate: defaults change nothing (known, always used), skill 0 is full ability, thresholds bite
    params = TacticGateParams();
    TacticGateState const defaults = ComputeTacticGate(params, tactics, 1, FirstGuid);
    check(defaults.KnownMask == TACTIC_ALL_MASK && defaults.Effort == SKILL_GATE_FULL &&
              TacticGatePasses(defaults, TacticId::Hold, false, FirstGuid, WindowMs, 4242),
          "the default gate at skill 1 does not let every tactic act");
    std::array<TacticSettings, TACTIC_COUNT> holdTactics{};
    TacticSettings& hold = holdTactics[static_cast<uint32>(TacticId::Hold)];
    hold.KnownFromSkill = 40;
    hold.UseChanceAtSkill1 = 0;
    hold.UseChanceAtSkill99 = 0;
    TacticGateState const off = ComputeTacticGate(params, holdTactics, 0, FirstGuid);
    check(off.KnownMask == TACTIC_ALL_MASK && off.UseChance[0] == SKILL_GATE_FULL && off.Effort == SKILL_GATE_FULL &&
              off.Style[0] == SKILL_STYLE_NEUTRAL,
          "skill 0 is not full ability");
    uint32 lowKnows = 0;
    uint32 highMisses = 0;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        if (ComputeTacticGate(params, holdTactics, 20, guid).KnownMask & TacticBit(TacticId::Hold))
            ++lowKnows;

        if (!(ComputeTacticGate(params, holdTactics, 60, guid).KnownMask & TacticBit(TacticId::Hold)))
            ++highMisses;
    }

    check(lowKnows == 0 && highMisses == 0,
          std::format("threshold 40 +- 15: {} bots at skill 20 know it, {} at 60 do not", lowKnows, highMisses));
    TacticGateState const known = ComputeTacticGate(params, holdTactics, 99, FirstGuid);
    check(!TacticGatePasses(known, TacticId::Hold, false, FirstGuid, WindowMs, 4242) &&
              TacticGatePasses(known, TacticId::Hold, true, FirstGuid, WindowMs, 4242),
          "use chance 0 passes, or full ability does not");
    check(SkillExemptFor(true, true) && !SkillExemptFor(false, true) && !SkillExemptFor(true, false) &&
              !SkillExemptFor(false, false),
          "the real-master exemption is not option and master");
}

#endif
