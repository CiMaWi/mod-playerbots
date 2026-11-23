/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTATTRIBUTETYPES_H
#define _PLAYERBOT_BOTATTRIBUTETYPES_H

#include <array>

#include "Define.h"

// The attributes, their definitions and the parameter and value structs of the attribute model
// (Playerbots.Skill.Attributes.*, PlayerbotAIConfig::SkillAttributes); the functions are in BotAttributes.h.

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

// Playerbots.Skill.Distribution and its parameters (SkillConfig::DistributionUniform, Mean, ...)
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

// Read once at config load (PlayerbotAIConfig::SkillAttributes). Loading and Spread are config table rows
// (SkillConfig.cpp, Playerbots.Skill.Attributes.Loading.<Name> and .Spread.<Name>, not in conf.dist; a value outside
// the bounds is clamped with a warning); the Corr.<A>-<B> pairs are read by PlayerbotAIConfig and factorised there.
struct BotAttributeParams
{
    std::array<float, BOT_ATTRIBUTE_COUNT> Loading = DefaultAttributeLoadings();  // Loading.<Name>, 0-1
    std::array<float, BOT_ATTRIBUTE_COUNT> Spread{};                              // Spread.<Name>, 0-3
    AttributeMatrix Correlation = IdentityAttributeMatrix();                      // Corr.<A>-<B>
    AttributeMatrix Factor = IdentityAttributeMatrix();                           // its Cholesky factor L
};

template <typename Struct>
struct ConfigFamily;

// The config table families of the loadings and spreads (SkillConfig.cpp), one row per attribute
extern ConfigFamily<BotAttributeParams> const SkillAttributeLoadingFamily;
extern ConfigFamily<BotAttributeParams> const SkillAttributeSpreadFamily;

// The one instance (SkillConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.SkillAttributes.
BotAttributeParams& BotAttributeParamsStorage();

// One bot's attributes, cached by PlayerbotAI::SetPvpSkill: abilities 1-99 (0 when the skill is 0), traits 0-100
struct BotAttributeValues
{
    std::array<uint8, BOT_ATTRIBUTE_COUNT> Value{};

    uint8 Get(BotAttribute attribute) const { return Value[static_cast<uint32>(attribute)]; }
};

#endif
