/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICGATECONFIG_H
#define _PLAYERBOT_TACTICGATECONFIG_H

#include "Define.h"

// The style traits and the numbers of the skill gate (PlayerbotAIConfig::SkillGate), kept apart from TacticGate.h so
// that the gate's functions can change without rebuilding every PlayerbotAIConfig.h user.

enum class SkillStyleTrait : uint8
{
    Aggression,
    Caution,
    Patience,
    Count
};

constexpr uint32 SKILL_STYLE_TRAIT_COUNT = static_cast<uint32>(SkillStyleTrait::Count);
constexpr uint8 SKILL_STYLE_NEUTRAL = 50;
constexpr uint8 SKILL_GATE_FULL = 100;

namespace TacticGateSalt
{
constexpr uint64 Knowledge = 0x6A09E667F3BCC908ULL;
constexpr uint64 Effort = 0xBB67AE8584CAA73BULL;
constexpr uint64 Style = 0x510E527FADE682D1ULL;
constexpr uint64 UsePhase = 0x9B05688C2B3E6C1FULL;
constexpr uint64 Use = 0x1F83D9ABFB41BD6BULL;
}  // namespace TacticGateSalt

// Read once at config load (PlayerbotAIConfig::SkillGate)
struct TacticGateParams
{
    float CurveExponent = 1.0f;               // Playerbots.Skill.CurveExponent, 0.1-10
    uint8 KnowledgeSpread = 15;               // Playerbots.Skill.KnowledgeSpread, 0-50
    uint8 EffortAtSkill1 = SKILL_GATE_FULL;   // Playerbots.Skill.EffortAtSkill1, 0-100
    uint8 EffortAtSkill99 = SKILL_GATE_FULL;  // Playerbots.Skill.EffortAtSkill99, 0-100
    uint8 EffortSpread = 0;                   // Playerbots.Skill.EffortSpread, 0-50
    uint8 StyleSpread = 0;                    // Playerbots.Skill.StyleSpread, 0-50
};

template <typename Struct>
struct ConfigFamily;

// The config table family of the gate (SkillConfig.cpp)
extern ConfigFamily<TacticGateParams> const SkillGateFamily;

#endif
