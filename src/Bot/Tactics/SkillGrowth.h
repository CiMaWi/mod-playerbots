/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SKILLGROWTH_H
#define PLAYERBOTS_SKILLGROWTH_H

#include <algorithm>
#include <cmath>
#include <string>

#include "BotAttributes.h"
#include "Common.h"
#include "SplitMix64.h"

// Skill growth (G6b; Playerbots.Skill.Growth.*; DD.1, DT.7, DT.14): a bot that plays an arena or battleground to its
// end grows its hidden skill a little, more when a real player fought on its team, and slower the closer it is to
// its own potential: its first roll plus a gap of PotentialMin..PotentialMax from the guid hash, never above 99. The
// first roll is the profile's percentile through Playerbots.Skill.Distribution (growth never writes the percentile),
// so neither the potential nor the first roll needs a column. Per match:
//     gain = GainPerMatch x (RealPlayerFactor with a real player on the bot's team) x (1 - progress),
//     progress = (skill - first roll) / (potential - first roll), clamped to 0-1,
// rounded stochastically (floor, plus one with the fraction's chance from a hash of the match and the guid), so the
// expected skill after n matches is first roll + gap x (1 - (1 - gain / gap)^n). Pure functions: no bots, no config
// singleton, no generator state, so .playerbots tactics selftest growth and the harnesses drive them.

struct SkillGrowthParams
{
    double GainPerMatch = 0.8;      // skill points per match at the first roll
    double RealPlayerFactor = 3.0;  // times the gain with a real player (or the headless test player) on the team
    uint32 PotentialMin = 10;       // the potential's gap above the first roll, drawn per bot
    uint32 PotentialMax = 40;
};

constexpr uint64 SKILL_GROWTH_POTENTIAL_SALT = 0x6B0761A5D2C3B4F7ULL;
constexpr uint64 SKILL_GROWTH_ROUND_SALT = 0x3C6EF372FE94F82BULL;
constexpr uint8 SKILL_GROWTH_MAX_SKILL = 99;

// The gap from the first roll to the potential, PotentialMin..PotentialMax from the guid hash
inline uint32 SkillGrowthGap(uint64 guid, SkillGrowthParams const& params)
{
    uint32 const low = std::min(params.PotentialMin, params.PotentialMax);
    uint32 const high = std::max(params.PotentialMin, params.PotentialMax);
    return low + static_cast<uint32>(SplitMix64(guid ^ SKILL_GROWTH_POTENTIAL_SALT) % (high - low + 1));
}

// The bot's potential: its first roll plus its gap, at most 99
inline uint8 SkillGrowthPotential(uint8 firstRoll, uint64 guid, SkillGrowthParams const& params)
{
    return static_cast<uint8>(std::min<uint32>(SKILL_GROWTH_MAX_SKILL, firstRoll + SkillGrowthGap(guid, params)));
}

// The expected gain of one match: the match's gain shrunk by the progress toward the potential
inline double SkillGrowthExpectedGain(uint8 skill, uint8 firstRoll, uint8 potential, double gain)
{
    if (gain <= 0.0 || skill >= potential || potential <= firstRoll)
        return 0.0;

    double const progress =
        std::clamp(static_cast<double>(skill - firstRoll) / static_cast<double>(potential - firstRoll), 0.0, 1.0);
    return gain * (1.0 - progress);
}

// Stochastic rounding: the whole part, plus one with the fraction's chance (a uniform from the hash); the mean is the
// value
inline uint32 SkillGrowthRound(double value, uint64 hash)
{
    if (value <= 0.0)
        return 0;

    double const whole = std::floor(value);
    double const uniform = static_cast<double>(SplitMix64(hash ^ SKILL_GROWTH_ROUND_SALT) >> 11) / 9007199254740992.0;
    return static_cast<uint32>(whole) + (uniform < value - whole ? 1 : 0);
}

// One match: the grown skill, never above the potential or 99, never below the skill
inline uint8 GrowSkill(uint8 skill, uint8 firstRoll, uint8 potential, double gain, uint64 hash)
{
    uint32 const cap = std::min<uint32>(potential, SKILL_GROWTH_MAX_SKILL);
    if (skill >= cap)
        return skill;

    uint32 const step = SkillGrowthRound(SkillGrowthExpectedGain(skill, firstRoll, potential, gain), hash);
    return static_cast<uint8>(std::min<uint32>(cap, skill + step));
}

// The gain of one match for a bot: GainPerMatch, times RealPlayerFactor with a real player on its team
inline double SkillGrowthMatchGain(SkillGrowthParams const& params, bool realPlayerOnTeam)
{
    return params.GainPerMatch * (realPlayerOnTeam ? params.RealPlayerFactor : 1.0);
}

// The hash of one bot's rounding in one match
inline uint64 SkillGrowthHash(uint64 matchSeed, uint64 guid) { return SplitMix64(matchSeed ^ (guid << 17) ^ guid); }

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
