/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SkillConfig.h"

#include <array>
#include <limits>
#include <utility>

#include "BotAttributeTypes.h"
#include "ConfigTable.h"
#include "SkillGrowthConfig.h"
#include "TacticGateConfig.h"
#include "TacticScope.h"

namespace
{
using SkillRow = ConfigRow<SkillConfig>;
using GateRow = ConfigRow<TacticGateParams>;
using AttributeRow = ConfigRow<BotAttributeParams>;
using GrowthRow = ConfigRow<SkillGrowthParams>;

constexpr uint32 MaxNoticeMissPercent = 95;
constexpr uint32 MaxNoticeDelayMs = 10000;
constexpr uint32 MaxNoticeWindowMs = 60000;
constexpr uint32 MaxSpread = 50;
constexpr uint32 MaxPercent = 100;
constexpr uint32 MaxPotential = 98;

// normal or uniform, any case; another name keeps normal with a warning
bool ParseDistribution(std::string const& text, SkillConfig&)
{
    std::string const name = TacticText::Lower(text);
    return name == "normal" || name == "uniform";
}

// The derived switches, the distribution's name in lower case and Max at least Min (the old read clamped it to Min-99)
void FinishSkill(SkillConfig& out)
{
    out.Distribution = TacticText::Lower(out.Distribution);
    out.DistributionUniform = out.Distribution == "uniform";
    out.ReactActive = out.ReactFactorLow != 1.0f || out.ReactFactorHigh != 1.0f;
    out.NoticeActive = out.NoticeMissPercentLow != 0 || out.NoticeMissPercentHigh != 0 || out.NoticeDelayMsLow != 0 ||
                       out.NoticeDelayMsHigh != 0;
    if (out.Max < out.Min)
        out.Max = out.Min;
}

constexpr std::array<SkillRow, 14> SkillRows = {{
    {"ReactFactorAtSkill1", &SkillConfig::ReactFactorLow, 0.1f, 10.0f, ConfigKind::TuningListed},
    {"ReactFactorAtSkill99", &SkillConfig::ReactFactorHigh, 0.1f, 10.0f, ConfigKind::TuningListed},
    {"FullAbilityInPvE", &SkillConfig::FullAbilityInPvE, 0, 0, ConfigKind::TuningListed},
    {"NoticeMissPercentAtSkill1", &SkillConfig::NoticeMissPercentLow, 0, MaxNoticeMissPercent,
     ConfigKind::TuningListed},
    {"NoticeMissPercentAtSkill99", &SkillConfig::NoticeMissPercentHigh, 0, MaxNoticeMissPercent,
     ConfigKind::TuningListed},
    {"NoticeDelayMsAtSkill1", &SkillConfig::NoticeDelayMsLow, 0, MaxNoticeDelayMs, ConfigKind::TuningListed},
    {"NoticeDelayMsAtSkill99", &SkillConfig::NoticeDelayMsHigh, 0, MaxNoticeDelayMs, ConfigKind::TuningListed},
    {"NoticeWindowMs", &SkillConfig::NoticeWindowMs, 100, MaxNoticeWindowMs, ConfigKind::TuningListed},
    {"Distribution", &SkillConfig::Distribution, 0, 0, ConfigKind::TuningListed, ConfigOutOfRange::Clamp,
     &ParseDistribution},
    {"Mean", &SkillConfig::Mean, 1.0f, 99.0f},
    {"Sd", &SkillConfig::Sd, 0.1f, 100.0f},
    {"Min", &SkillConfig::Min, 1.0f, 99.0f, ConfigKind::TuningListed},
    // Max's lower bound is Min (the Finish raises it, silently as the old read did)
    {"Max", &SkillConfig::Max, -std::numeric_limits<double>::infinity(), 99.0f, ConfigKind::TuningListed},
    {"ExemptRealMasterBots", &SkillConfig::ExemptRealMasterBots, 0, 0, ConfigKind::TuningListed},
}};

constexpr std::array<GateRow, 6> SkillGateRows = {{
    {"CurveExponent", &TacticGateParams::CurveExponent, 0.1f, 10.0f, ConfigKind::TuningListed},
    {"KnowledgeSpread", &TacticGateParams::KnowledgeSpread, 0, MaxSpread, ConfigKind::TuningListed},
    {"EffortAtSkill1", &TacticGateParams::EffortAtSkill1, 0, MaxPercent, ConfigKind::TuningListed},
    {"EffortAtSkill99", &TacticGateParams::EffortAtSkill99, 0, MaxPercent, ConfigKind::TuningListed},
    {"EffortSpread", &TacticGateParams::EffortSpread, 0, MaxSpread, ConfigKind::TuningListed},
    {"StyleSpread", &TacticGateParams::StyleSpread, 0, MaxSpread, ConfigKind::TuningListed},
}};

float& LoadingAt(BotAttributeParams& out, uint32_t index) { return out.Loading[index]; }
float& SpreadAt(BotAttributeParams& out, uint32_t index) { return out.Spread[index]; }

// One row per attribute, keyed by its name, 0 to upper; the defaults are BotAttributeParams' initializers (Loading 1
// for the abilities and 0 for the traits, Spread 0)
template <std::size_t... Index>
constexpr std::array<AttributeRow, BOT_ATTRIBUTE_COUNT> AttributeRows(float& (*at)(BotAttributeParams&, uint32_t),
                                                                      double upper, std::index_sequence<Index...>)
{
    return {{AttributeRow{BOT_ATTRIBUTE_DEFINITIONS[Index].Name,
                          ConfigElement<BotAttributeParams, float>{at, static_cast<uint32_t>(Index)}, 0.0, upper}...}};
}

constexpr std::array<AttributeRow, BOT_ATTRIBUTE_COUNT> LoadingRows =
    AttributeRows(&LoadingAt, 1.0, std::make_index_sequence<BOT_ATTRIBUTE_COUNT>{});
constexpr std::array<AttributeRow, BOT_ATTRIBUTE_COUNT> SpreadRows =
    AttributeRows(&SpreadAt, 3.0, std::make_index_sequence<BOT_ATTRIBUTE_COUNT>{});

// PotentialMax at least PotentialMin (the old read clamped it to Min-98)
void FinishGrowth(SkillGrowthParams& out)
{
    if (out.PotentialMax < out.PotentialMin)
        out.PotentialMax = out.PotentialMin;
}

constexpr std::array<GrowthRow, 5> SkillGrowthRows = {{
    {"Enable", &SkillGrowthParams::Enable, 0, 0, ConfigKind::NotYet},
    {"GainPerMatch", &SkillGrowthParams::GainPerMatch, 0.0f, 10.0f},
    {"RealPlayerFactor", &SkillGrowthParams::RealPlayerFactor, 1.0f, 10.0f},
    {"PotentialMin", &SkillGrowthParams::PotentialMin, 0, MaxPotential},
    {"PotentialMax", &SkillGrowthParams::PotentialMax, 0, MaxPotential},
}};
}  // namespace

ConfigFamily<SkillGrowthParams> const SkillGrowthFamily{"Playerbots.Skill.Growth.", SkillGrowthRows, &FinishGrowth};
ConfigFamily<BotAttributeParams> const SkillAttributeLoadingFamily{"Playerbots.Skill.Attributes.Loading.", LoadingRows};
ConfigFamily<BotAttributeParams> const SkillAttributeSpreadFamily{"Playerbots.Skill.Attributes.Spread.", SpreadRows};
ConfigFamily<SkillConfig> const SkillFamily{"Playerbots.Skill.", SkillRows, &FinishSkill};
ConfigFamily<TacticGateParams> const SkillGateFamily{"Playerbots.Skill.", SkillGateRows};

SkillConfig& SkillConfigStorage()
{
    static SkillConfig values;
    return values;
}

TacticGateParams& TacticGateParamsStorage()
{
    static TacticGateParams values;
    return values;
}

SkillGrowthParams& SkillGrowthParamsStorage()
{
    static SkillGrowthParams values;
    return values;
}

BotAttributeParams& BotAttributeParamsStorage()
{
    static BotAttributeParams values;
    return values;
}
