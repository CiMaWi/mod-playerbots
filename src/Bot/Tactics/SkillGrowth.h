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

#endif
