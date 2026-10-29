/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTATTRIBUTES_H
#define _PLAYERBOT_BOTATTRIBUTES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "Common.h"
#include "NaturalReaction.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// Attributes (Playerbots.Skill.Attributes.*): the hidden skill is a bot's latent general ability, and each bot draws
// eight abilities and four personality traits from it. In normal scores an attribute is
//     z_a = loading_a * z_skill + spread_a * sqrt(1 - loading_a^2) * e_a,
// where z_skill is the skill's score through Playerbots.Skill.Distribution (its percentile through the probit) and the
// own parts e_a are standard normals from the guid hash, correlated by a small matrix (its Cholesky factor, made at
// config load). An ability goes back onto the 1-99 scale through the same distribution, so every lerp that takes the
// skill takes an ability; a trait is 50 + 50 * clamp(z_a / 3, -1, 1) scaled by its spread (0-100, 50 neutral).
// Defaults reproduce the skill: ability loadings 1 (the ability is the skill, exactly; the matrix has no effect) and
// trait spreads 0 (neutral 50). Pure functions: no bots, no config singleton, so .playerbots tactics selftest
// attributes drives them with synthetic guids and skills. PlayerbotAI::SetPvpSkill caches the values once per skill
// load: no per-tick cost, no generator state. Hidden like the skill: only GM commands and measurement records
// show them.

// The abilities (Speed to Grit, on the skill's 1-99 scale), then the personality traits (0-100, 50 neutral)
enum class BotAttribute : uint8
{
    Speed,       // reaction time: the natural reaction's draw, D3's stretch, D4's notice delay
    Wisdom,      // judgement, how deeply a bot thinks (G0h3, the tactics)
    Focus,       // attention and load: D4's miss share
    Dexterity,   // mechanics: rare buttons, target switches, focus targets
    Foresight,   // anticipation, in-match learning, prediction
    Knowledge,   // which tactics the bot knows (G0g's threshold test)
    Discipline,  // how often it uses what it knows (G0g's use chance)
    Grit,        // how hard it tries (G0g's effort)
    Aggression,
    Caution,
    Patience,
    Teamwork,  // peel, follow the team target, share crowd control vs play its own game
    Count
};

constexpr uint32 BOT_ATTRIBUTE_COUNT = static_cast<uint32>(BotAttribute::Count);
constexpr uint32 BOT_ABILITY_COUNT = static_cast<uint32>(BotAttribute::Aggression);

// DT.14 (the user, 2026-10-30): practice (G6b) grows the stored skill; the learnable attributes read the grown skill
// (Half: halfway between the first roll and it), the fixed ones the first roll, which the guid hash reproduces. G0j has
// no growth: SetPvpSkill passes the stored skill as both, so the flag changes nothing yet.
enum class AttributeLearnable : uint8
{
    Full,
    Half,
    Fixed
};

struct BotAttributeDefinition
{
    char const* Name;   // the config key part (Playerbots.Skill.Attributes.Loading.<Name>)
    char const* Label;  // GM output
    AttributeLearnable Learnable;
};

constexpr std::array<BotAttributeDefinition, BOT_ATTRIBUTE_COUNT> BOT_ATTRIBUTE_DEFINITIONS = {{
    {"Speed", "speed", AttributeLearnable::Fixed},
    {"Wisdom", "wisdom", AttributeLearnable::Full},
    {"Focus", "focus", AttributeLearnable::Fixed},
    {"Dexterity", "dexterity", AttributeLearnable::Half},
    {"Foresight", "foresight", AttributeLearnable::Full},
    {"Knowledge", "knowledge", AttributeLearnable::Full},
    {"Discipline", "discipline", AttributeLearnable::Fixed},
    {"Grit", "grit", AttributeLearnable::Fixed},
    {"Aggression", "aggression", AttributeLearnable::Fixed},
    {"Caution", "caution", AttributeLearnable::Fixed},
    {"Patience", "patience", AttributeLearnable::Fixed},
    {"Teamwork", "teamwork", AttributeLearnable::Full},
}};

constexpr uint64 BOT_ATTRIBUTE_SALT = 0xC2B2AE3D27D4EB4FULL;
// The skill's score is clamped where Playerbots.Skill.Distribution's percentiles are (1e-6 from either end)
constexpr double BOT_ATTRIBUTE_MAX_SCORE = 4.753424308822899;

using AttributeMatrix = std::array<std::array<double, BOT_ATTRIBUTE_COUNT>, BOT_ATTRIBUTE_COUNT>;

inline bool IsBotPersonality(uint32 index) { return index >= BOT_ABILITY_COUNT; }

inline AttributeMatrix IdentityAttributeMatrix()
{
    AttributeMatrix matrix{};
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
        matrix[index][index] = 1.0;

    return matrix;
}

// Playerbots.Skill.Distribution and its parameters (PlayerbotAIConfig::SkillDistributionUniform, SkillMean, ...)
struct SkillDistributionParams
{
    bool Uniform = false;
    double Mean = 50.0;
    double Sd = 15.0;
    double Min = 1.0;
    double Max = 99.0;
};

// The defaults: loading 1 for the abilities and 0 for the traits, spreads 0, no correlations
inline std::array<float, BOT_ATTRIBUTE_COUNT> DefaultAttributeLoadings()
{
    std::array<float, BOT_ATTRIBUTE_COUNT> loadings{};
    for (uint32 index = 0; index < BOT_ABILITY_COUNT; ++index)
        loadings[index] = 1.0f;

    return loadings;
}

// Read once at config load (PlayerbotAIConfig::SkillAttributes)
struct BotAttributeParams
{
    std::array<float, BOT_ATTRIBUTE_COUNT> Loading = DefaultAttributeLoadings();  // Loading.<Name>, 0-1
    std::array<float, BOT_ATTRIBUTE_COUNT> Spread{};                              // Spread.<Name>, 0-3
    AttributeMatrix Correlation = IdentityAttributeMatrix();                      // Corr.<A>-<B>
    AttributeMatrix Factor = IdentityAttributeMatrix();                           // its Cholesky factor L
};

// One bot's attributes, cached by PlayerbotAI::SetPvpSkill: abilities 1-99 (0 when the skill is 0), traits 0-100
struct BotAttributeValues
{
    std::array<uint8, BOT_ATTRIBUTE_COUNT> Value{};

    uint8 Get(BotAttribute attribute) const { return Value[static_cast<uint32>(attribute)]; }
};

// An ability is the skill itself at loading 1 (its own part has weight 0, so the matrix has no effect on it); a trait
// is neutral at spread 0
inline bool AttributeAtDefault(BotAttributeParams const& params, uint32 index)
{
    return IsBotPersonality(index) ? params.Spread[index] == 0.0f : params.Loading[index] == 1.0f;
}

inline bool AttributesAtDefaults(BotAttributeParams const& params)
{
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
    {
        if (!AttributeAtDefault(params, index))
            return false;
    }

    return true;
}

// The Cholesky factor L of a correlation matrix (L L^T = matrix). False when the matrix is not symmetric or not
// positive definite; the factor is then the identity (uncorrelated own parts).
inline bool FactorAttributeMatrix(AttributeMatrix const& matrix, AttributeMatrix& factor)
{
    factor = AttributeMatrix{};
    for (uint32 row = 0; row < BOT_ATTRIBUTE_COUNT; ++row)
    {
        for (uint32 column = 0; column <= row; ++column)
        {
            if (std::fabs(matrix[row][column] - matrix[column][row]) > 1e-12)
            {
                factor = IdentityAttributeMatrix();
                return false;
            }

            double sum = matrix[row][column];
            for (uint32 k = 0; k < column; ++k)
                sum -= factor[row][k] * factor[column][k];

            if (row != column)
            {
                factor[row][column] = sum / factor[column][column];
                continue;
            }

            if (sum <= 1e-9)
            {
                factor = IdentityAttributeMatrix();
                return false;
            }

            factor[row][row] = std::sqrt(sum);
        }
    }

    return true;
}

// The own parts e: independent standard normals from the guid hash through the probit, correlated by the factor
inline std::array<double, BOT_ATTRIBUTE_COUNT> AttributeOwnParts(AttributeMatrix const& factor, uint64 guid)
{
    std::array<double, BOT_ATTRIBUTE_COUNT> independent{};
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
        independent[index] = Probit(UnitFromHash(SplitMix64(((guid << 8) | index) ^ BOT_ATTRIBUTE_SALT)));

    std::array<double, BOT_ATTRIBUTE_COUNT> parts{};
    for (uint32 row = 0; row < BOT_ATTRIBUTE_COUNT; ++row)
    {
        for (uint32 k = 0; k <= row; ++k)
            parts[row] += factor[row][k] * independent[k];
    }

    return parts;
}

// The skill score an attribute reads (DT.14): learnable ones the grown skill, fixed ones the first roll
inline double AttributeSkillScore(AttributeLearnable learnable, double zSkill, double zSkillFirstRoll)
{
    switch (learnable)
    {
        case AttributeLearnable::Full:
            return zSkill;
        case AttributeLearnable::Half:
            return zSkillFirstRoll + 0.5 * (zSkill - zSkillFirstRoll);
        default:
            return zSkillFirstRoll;
    }
}

// Every attribute's normal score from the skill's scores and the guid
inline std::array<double, BOT_ATTRIBUTE_COUNT> ComputeAttributeScores(BotAttributeParams const& params, double zSkill,
                                                                      double zSkillFirstRoll, uint64 guid)
{
    std::array<double, BOT_ATTRIBUTE_COUNT> const parts = AttributeOwnParts(params.Factor, guid);
    std::array<double, BOT_ATTRIBUTE_COUNT> scores{};
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
    {
        double const loading = params.Loading[index];
        double const own = std::sqrt(std::max(0.0, 1.0 - loading * loading));
        double const skill = AttributeSkillScore(BOT_ATTRIBUTE_DEFINITIONS[index].Learnable, zSkill, zSkillFirstRoll);
        scores[index] = loading * skill + params.Spread[index] * own * parts[index];
    }

    return scores;
}

// A skill's normal score through the distribution (as BotProfileMgr::PercentileFromSkill, then the probit)
inline double SkillScore(SkillDistributionParams const& distribution, uint8 skill)
{
    if (!distribution.Uniform)
        return std::clamp((skill - distribution.Mean) / distribution.Sd, -BOT_ATTRIBUTE_MAX_SCORE,
                          BOT_ATTRIBUTE_MAX_SCORE);

    double percentile = 0.5;
    if (distribution.Max > distribution.Min)
        percentile = (skill - distribution.Min) / (distribution.Max - distribution.Min);

    return Probit(std::clamp(percentile, 1e-6, 1.0 - 1e-6));
}

// A normal score back onto the 1-99 scale through the distribution (as BotProfileMgr::SkillFromPercentile)
inline uint8 SkillFromScore(SkillDistributionParams const& distribution, double score)
{
    double value = distribution.Mean + distribution.Sd * score;
    if (distribution.Uniform)
        value = distribution.Min + 0.5 * std::erfc(-score / std::sqrt(2.0)) * (distribution.Max - distribution.Min);

    return static_cast<uint8>(std::clamp<long>(std::lround(value), 1, 99));
}

// A trait from its score: 50 + 50 * clamp(spread * z / 3, -1, 1), so at spread 1 three standard deviations reach 0 or
// 100; exactly 50 at spread 0
inline uint8 PersonalityFromScore(float spread, double score)
{
    if (spread <= 0.0f)
        return SKILL_STYLE_NEUTRAL;

    double const position = std::clamp(spread * score / 3.0, -1.0, 1.0);
    return static_cast<uint8>(std::lround(SKILL_STYLE_NEUTRAL + SKILL_STYLE_NEUTRAL * position));
}

// One bot's attributes. Skill 0 (the skill system off): abilities 0 and traits neutral, and every consumer takes its
// full-ability branch. An ability at loading 1 is the skill itself (no round trip through the distribution).
inline BotAttributeValues ComputeBotAttributes(BotAttributeParams const& params,
                                               SkillDistributionParams const& distribution, uint8 skill,
                                               uint8 skillFirstRoll, uint64 guid)
{
    BotAttributeValues values;
    if (skill == 0)
    {
        for (uint32 index = BOT_ABILITY_COUNT; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = SKILL_STYLE_NEUTRAL;

        return values;
    }

    uint8 const firstRoll = skillFirstRoll ? skillFirstRoll : skill;
    double const zSkill = SkillScore(distribution, skill);
    double const zFirstRoll = firstRoll == skill ? zSkill : SkillScore(distribution, firstRoll);
    std::array<double, BOT_ATTRIBUTE_COUNT> scores{};
    if (!AttributesAtDefaults(params))
        scores = ComputeAttributeScores(params, zSkill, zFirstRoll, guid);

    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
    {
        AttributeLearnable const learnable = BOT_ATTRIBUTE_DEFINITIONS[index].Learnable;
        if (IsBotPersonality(index))
            values.Value[index] = PersonalityFromScore(params.Spread[index], scores[index]);
        else if (!AttributeAtDefault(params, index))
            values.Value[index] = SkillFromScore(distribution, scores[index]);
        else if (firstRoll == skill || learnable == AttributeLearnable::Full)
            values.Value[index] = skill;
        else if (learnable == AttributeLearnable::Fixed)
            values.Value[index] = firstRoll;
        else
            values.Value[index] = SkillFromScore(distribution, AttributeSkillScore(learnable, zSkill, zFirstRoll));
    }

    return values;
}

// What the tactic gate reads: knowledge, discipline and grit, EffortSpread's jitter only while grit is the skill, and
// each style trait from the personality once its spread is set (else G0g's StyleSpread draw)
inline TacticGateInputs AttributeGateInputs(BotAttributeParams const& params, BotAttributeValues const& values,
                                            uint8 skill)
{
    TacticGateInputs inputs;
    inputs.Skill = skill;
    inputs.Knowledge = values.Get(BotAttribute::Knowledge);
    inputs.Discipline = values.Get(BotAttribute::Discipline);
    inputs.Grit = values.Get(BotAttribute::Grit);
    inputs.EffortJitter = AttributeAtDefault(params, static_cast<uint32>(BotAttribute::Grit));
    for (uint32 trait = 0; trait < SKILL_STYLE_TRAIT_COUNT; ++trait)
    {
        uint32 const index = static_cast<uint32>(BotAttribute::Aggression) + trait;
        inputs.Style[trait] = values.Value[index];
        if (!AttributeAtDefault(params, index))
            inputs.StyleMask |= 1u << trait;
    }

    return inputs;
}

// GM output: "speed 50, wisdom 50, ...; personality aggression 50, caution 50, patience 50, teamwork 50"
inline std::string FormatBotAttributes(BotAttributeValues const& values)
{
    std::string text;
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
    {
        if (index == BOT_ABILITY_COUNT)
            text += "; personality ";
        else if (index)
            text += ", ";

        text += BOT_ATTRIBUTE_DEFINITIONS[index].Label;
        text += ' ';
        text += std::to_string(values.Value[index]);
    }

    return text;
}

#endif
