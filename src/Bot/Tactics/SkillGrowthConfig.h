/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SKILLGROWTHCONFIG_H
#define PLAYERBOTS_SKILLGROWTHCONFIG_H

#include "Define.h"

// The numbers of skill growth (Playerbots.Skill.Growth.*, PlayerbotAIConfig::SkillGrowth); the functions are in
// SkillGrowth.h.

// Config table rows (SkillConfig.cpp); only Enable is in conf.dist, the numbers are read as floats and clamped with a
// warning: GainPerMatch 0-10, RealPlayerFactor 1-10, PotentialMin and PotentialMax 0-98 (Max at least Min).
struct SkillGrowthParams
{
    // The not-yet switch; PlayerbotAIConfig::SkillGrowthEnable is it on a server that is not a lab
    bool Enable = false;
    double GainPerMatch = 0.8;      // skill points per match at the first roll
    double RealPlayerFactor = 3.0;  // times the gain with a real player (or the headless test player) on the team
    uint32 PotentialMin = 10;       // the potential's gap above the first roll, drawn per bot
    uint32 PotentialMax = 40;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (SkillConfig.cpp); its Finish keeps PotentialMax at least PotentialMin
extern ConfigFamily<SkillGrowthParams> const SkillGrowthFamily;

#endif
