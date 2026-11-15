/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SKILLGROWTHCHECKS_H
#define PLAYERBOTS_SKILLGROWTHCHECKS_H

#include <algorithm>
#include <cmath>
#include <string>

#include "SkillGrowth.h"

// The exit checks of G6b (tactics selftest growth and the scratchpad harness): check(bool ok, std::string what)
template <typename Check>
void RunSkillGrowthChecks(Check&& check)
{
    SkillGrowthParams const params;
    constexpr uint64 FirstGuid = 5000;
    constexpr uint32 Bots = 2000;

    // 1. The curve: the mean skill after n matches is first roll + gap x (1 - (1 - gain / gap)^n), per gap and for
    //    the real-player gain, within 3 % of the gap; the population reaches about 60 % of its gaps in 30 matches
    for (uint32 gap : {10u, 25u, 40u})
    {
        for (bool realPlayer : {false, true})
        {
            double const gain = SkillGrowthMatchGain(params, realPlayer);
            for (uint32 matches : {10u, 30u, 100u})
            {
                double sum = 0.0;
                for (uint32 bot = 0; bot < Bots; ++bot)
                {
                    uint8 const first = 40;
                    uint8 const potential = static_cast<uint8>(first + gap);
                    uint8 skill = first;
                    for (uint32 match = 0; match < matches; ++match)
                        skill = GrowSkill(skill, first, potential, gain, SkillGrowthHash(match * 7919ULL + 1, bot));

                    sum += skill - first;
                }

                double const mean = sum / Bots;
                double const expected = gap * (1.0 - std::pow(1.0 - gain / gap, static_cast<double>(matches)));
                check(std::abs(mean - expected) <= 0.03 * gap,
                      "curve gap " + std::to_string(gap) + (realPlayer ? " with a real player" : "") + " after " +
                          std::to_string(matches) + " matches: mean +" + std::to_string(mean) + ", expected +" +
                          std::to_string(expected));
            }
        }
    }

    double progress30 = 0.0;
    double progress10Real = 0.0;
    double progress10Solo = 0.0;
    for (uint32 bot = 0; bot < Bots; ++bot)
    {
        uint64 const guid = FirstGuid + bot;
        uint8 const first = 30;
        uint8 const potential = SkillGrowthPotential(first, guid, params);
        uint8 solo = first;
        uint8 real = first;
        for (uint32 match = 0; match < 30; ++match)
        {
            solo = GrowSkill(solo, first, potential, SkillGrowthMatchGain(params, false), SkillGrowthHash(match, guid));
            if (match < 10)
                real = GrowSkill(real, first, potential, SkillGrowthMatchGain(params, true),
                                 SkillGrowthHash(match + 1000, guid));

            if (match == 9)
                progress10Solo += static_cast<double>(solo - first) / (potential - first);
        }

        progress30 += static_cast<double>(solo - first) / (potential - first);
        progress10Real += static_cast<double>(real - first) / (potential - first);
    }

    progress30 /= Bots;
    progress10Real /= Bots;
    progress10Solo /= Bots;
    check(progress30 >= 0.45 && progress30 <= 0.75,
          "30 matches reach " + std::to_string(progress30) + " of the gap (0.45-0.75)");
    check(progress10Real >= 0.45 && progress10Real > 2.0 * progress10Solo,
          "10 matches beside a real player reach " + std::to_string(progress10Real) + " of the gap, alone " +
              std::to_string(progress10Solo));

    // 2. The potential: the gap lies in PotentialMin..PotentialMax and takes both ends; after many matches every bot
    //    sits exactly at its potential, never above it, and never above 99
    uint32 below = 0;
    uint32 above = 0;
    bool lowSeen = false;
    bool highSeen = false;
    for (uint32 bot = 0; bot < 10000; ++bot)
    {
        uint32 const gap = SkillGrowthGap(FirstGuid + bot, params);
        below += gap < params.PotentialMin ? 1 : 0;
        above += gap > params.PotentialMax ? 1 : 0;
        lowSeen = lowSeen || gap == params.PotentialMin;
        highSeen = highSeen || gap == params.PotentialMax;
    }

    check(!below && !above && lowSeen && highSeen, "gaps in 10-40 (" + std::to_string(below) + " below, " +
                                                       std::to_string(above) + " above, ends seen " +
                                                       std::to_string(lowSeen) + std::to_string(highSeen) + ")");

    uint32 notAtPotential = 0;
    uint32 overPotential = 0;
    uint32 over99 = 0;
    uint32 fell = 0;
    for (uint32 bot = 0; bot < Bots; ++bot)
    {
        uint64 const guid = FirstGuid + bot;
        uint8 const first = static_cast<uint8>(1 + bot % 99);
        uint8 const potential = SkillGrowthPotential(first, guid, params);
        uint8 skill = first;
        for (uint32 match = 0; match < 3000; ++match)
        {
            uint8 const next = GrowSkill(skill, first, potential, SkillGrowthMatchGain(params, match % 3 == 0),
                                         SkillGrowthHash(match, guid));
            fell += next < skill ? 1 : 0;
            overPotential += next > potential ? 1 : 0;
            over99 += next > SKILL_GROWTH_MAX_SKILL ? 1 : 0;
            skill = next;
        }

        notAtPotential += skill != potential ? 1 : 0;
    }

    check(!notAtPotential && !overPotential && !over99 && !fell,
          "3000 matches: " + std::to_string(notAtPotential) + " bots off their potential, " +
              std::to_string(overPotential) + " steps over it, " + std::to_string(over99) + " over 99, " +
              std::to_string(fell) + " falls");
    check(SkillGrowthPotential(90, FirstGuid, params) == 99 && SkillGrowthPotential(99, FirstGuid, params) == 99 &&
              GrowSkill(99, 99, 99, 2.4, 1) == 99,
          "the potential and the skill stop at 99");
    check(GrowSkill(50, 50, 70, 0.0, 1) == 50 && GrowSkill(70, 50, 70, 2.4, 1) == 70 &&
              GrowSkill(60, 50, 50, 2.4, 1) == 60 && SkillGrowthExpectedGain(45, 50, 70, 0.8) == 0.8,
          "no growth without a gain, at the potential or without a gap; below the first roll the full gain");

    // 3. The rounding: the mean of 10 000 draws within 2 % of the value; for small gains (the last steps toward the
    //    potential) 2 % is below the draws' own noise, so within 4 standard errors there
    for (double value : {0.05, 0.37, 0.8, 1.6, 2.4})
    {
        double sum = 0.0;
        for (uint32 draw = 0; draw < 10000; ++draw)
            sum += SkillGrowthRound(value, SkillGrowthHash(draw * 2654435761ULL, 77));

        double const mean = sum / 10000.0;
        double const fraction = value - std::floor(value);
        double const noise = 4.0 * std::sqrt(fraction * (1.0 - fraction) / 10000.0);
        check(std::abs(mean - value) <= std::max(0.02 * value, noise),
              "rounding " + std::to_string(value) + ": mean " + std::to_string(mean) + " over 10000 draws");
    }

    // 4. The attributes (DT.14): after growth the learnable ones rise (Full at full rate, Dexterity at half), the
    //    fixed ones (Speed, Focus, Discipline, Grit and the traits) keep the first roll's value; at the defaults and
    //    with spreads and loadings below 1
    BotAttributeParams model;
    model.Loading = {0.5f, 0.8f, 0.6f, 0.5f, 0.8f, 0.8f, 0.6f, 0.6f, 0.0f, 0.0f, 0.0f, 0.7f};
    model.Spread.fill(1.0f);
    for (bool withModel : {false, true})
    {
        BotAttributeParams const used = withModel ? model : BotAttributeParams();
        SkillDistributionParams const distribution;
        uint32 fixedChanged = 0;
        uint32 learnableFell = 0;
        std::array<double, BOT_ATTRIBUTE_COUNT> rise{};
        for (uint32 bot = 0; bot < Bots; ++bot)
        {
            uint64 const guid = FirstGuid + bot;
            uint8 const first = static_cast<uint8>(20 + bot % 50);
            uint8 const grown = SkillGrowthPotential(first, guid, params);
            BotAttributeValues const before = ComputeBotAttributes(used, distribution, first, first, guid);
            BotAttributeValues const after = ComputeBotAttributes(used, distribution, grown, first, guid);
            for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            {
                int32 const delta = static_cast<int32>(after.Value[index]) - before.Value[index];
                rise[index] += delta;
                if (BOT_ATTRIBUTE_DEFINITIONS[index].Learnable == AttributeLearnable::Fixed)
                    fixedChanged += delta ? 1 : 0;
                else
                    learnableFell += delta < 0 ? 1 : 0;
            }
        }

        std::string const name = withModel ? "model" : "defaults";
        check(!fixedChanged && !learnableFell, name + ": " + std::to_string(fixedChanged) +
                                                   " fixed attributes changed, " + std::to_string(learnableFell) +
                                                   " learnable ones fell");
        uint32 const speed = static_cast<uint32>(BotAttribute::Speed);
        uint32 const dexterity = static_cast<uint32>(BotAttribute::Dexterity);
        uint32 const knowledge = static_cast<uint32>(BotAttribute::Knowledge);
        uint32 const wisdom = static_cast<uint32>(BotAttribute::Wisdom);
        check(rise[speed] == 0.0 && rise[knowledge] > 0.0 && rise[wisdom] > 0.0 && rise[dexterity] > 0.0 &&
                  rise[dexterity] < rise[knowledge],
              name + ": mean rises speed " + std::to_string(rise[speed] / Bots) + ", knowledge " +
                  std::to_string(rise[knowledge] / Bots) + ", wisdom " + std::to_string(rise[wisdom] / Bots) +
                  ", dexterity " + std::to_string(rise[dexterity] / Bots));
    }
}

#endif
