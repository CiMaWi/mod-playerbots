/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SKILLCONFIG_H
#define _PLAYERBOT_SKILLCONFIG_H

#include <string>

#include "Define.h"

// Playerbots.Skill.*: the skill's reaction stretch, its notice model, the distribution a bot's percentile maps through
// and the real-master exemption (PlayerbotAIConfig::Skill). The gate, growth and attribute numbers have their own
// structs (TacticGateParams, SkillGrowthParams, BotAttributeParams).
struct SkillConfig
{
    // ReactFactorAtSkill1/99: the reaction stretch at skill 1 and 99 (1 = off)
    float ReactFactorLow = 1.0f;
    float ReactFactorHigh = 1.0f;
    // FullAbilityInPvE: bots play at full ability outside a PvP fight
    bool FullAbilityInPvE = false;
    bool ReactActive = false;  // derived: a factor differs from 1
    // NoticeMissPercentAtSkill1/99, NoticeDelayMsAtSkill1/99, NoticeWindowMs: the notice model
    uint32 NoticeMissPercentLow = 0;
    uint32 NoticeMissPercentHigh = 0;
    uint32 NoticeDelayMsLow = 0;
    uint32 NoticeDelayMsHigh = 0;
    uint32 NoticeWindowMs = 1500;
    bool NoticeActive = false;  // derived: a miss share or a delay is set
    // Distribution (normal or uniform) and its parameters: a bot's fixed percentile maps to its skill through it.
    // Mean (1-99) and Sd (0.1-100) shape the normal one and are not in conf.dist; Min and Max (1-99, Max at least
    // Min) bound the uniform one.
    std::string Distribution = "normal";
    bool DistributionUniform = false;  // derived from Distribution
    float Mean = 50.0f;
    float Sd = 15.0f;
    float Min = 1.0f;
    float Max = 99.0f;
    // ExemptRealMasterBots: bots with a game-client master play at full ability (1, phase D) or keep their skill
    // (0, DT.2)
    bool ExemptRealMasterBots = true;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (SkillConfig.cpp); its Finish sets the derived values and keeps Max at least Min
extern ConfigFamily<SkillConfig> const SkillFamily;

// The one instance (SkillConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.Skill.
SkillConfig& SkillConfigStorage();

#endif
