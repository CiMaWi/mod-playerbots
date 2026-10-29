/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICGATE_H
#define _PLAYERBOT_TACTICGATE_H

#include <algorithm>
#include <array>
#include <cmath>

#include "Common.h"
#include "SplitMix64.h"
#include "Tactics.h"

// The skill gate of the tactics: a bot uses a tactic when it knows it (a threshold with per-bot jitter) and uses it in
// the current window (a chance per window); the tactic's numbers are lerped over skill; effort and three style traits
// shape the rest. Pure functions: no bots, no config singleton, so .playerbots tactics selftest gate drives them with
// synthetic guids, skills and times. PlayerbotAI::SetPvpSkill caches a TacticGateState per bot; the tactics ask
// PlayerbotAI::TacticActive. Skill 0 (the skill system off) is full ability: every tactic known and used, effort 100,
// style neutral. Hidden like the skill: only GM commands and measurement records show these values.

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

// One bot's gate, cached by PlayerbotAI::SetPvpSkill
struct TacticGateState
{
    std::array<uint8, TACTIC_COUNT> KnownFrom{};  // jittered threshold: 0 = every skill knows it, 100 = none
    std::array<uint8, TACTIC_COUNT> UseChance{};  // percent per window
    uint32 KnownMask = 0;                         // TacticBit of every tactic the bot knows at its skill
    uint8 Effort = SKILL_GATE_FULL;
    std::array<uint8, SKILL_STYLE_TRAIT_COUNT> Style{};
};

// What the gate reads of a bot (G0j, BotAttributes.h's AttributeGateInputs): the skill (0 = full ability), the
// knowledge, discipline and grit attributes, whether EffortSpread's jitter applies, and the style traits whose bit is
// set in StyleMask (the others come from G0g's StyleSpread draw). With the attribute defaults all three are the skill.
struct TacticGateInputs
{
    uint8 Skill = 0;
    uint8 Knowledge = 0;
    uint8 Discipline = 0;
    uint8 Grit = 0;
    bool EffortJitter = true;
    uint8 StyleMask = 0;
    std::array<uint8, SKILL_STYLE_TRAIT_COUNT> Style{};
};

// 0 at skill 1, 1 at skill 99 (callers treat skill 0 as full ability before they ask)
inline float SkillPosition(uint8 skill) { return (static_cast<float>(std::clamp<uint8>(skill, 1, 99)) - 1.0f) / 98.0f; }

// Playerbots.Skill.CurveExponent: position' = 1 - (1 - position)^e. At e = 1 the straight line, exactly; above 1 a bot
// gains most of its ability early (e = 2: skill 50 sits 75 % of the way), below 1 late.
inline float SkillCurve(float position, float exponent)
{
    if (exponent == 1.0f)
        return position;

    return 1.0f - std::pow(1.0f - position, exponent);
}

// From the value at skill 1 to the value at skill 99 along the curve; at e = 1 the same expression as before G0g
inline float SkillLerp(float atSkill1, float atSkill99, float position, float exponent)
{
    return atSkill1 + (atSkill99 - atSkill1) * SkillCurve(position, exponent);
}

// The same, rounded, for whole numbers (percent, ms)
inline uint32 SkillLerpRounded(uint32 atSkill1, uint32 atSkill99, float position, float exponent)
{
    return static_cast<uint32>(atSkill1 + (static_cast<float>(atSkill99) - atSkill1) * SkillCurve(position, exponent) +
                               0.5f);
}

// A whole number from -spread to +spread, evenly, from a hash
inline int32 SkillGateJitter(uint64 hash, uint32 spread)
{
    if (spread == 0)
        return 0;

    return static_cast<int32>(hash % (2 * static_cast<uint64>(spread) + 1)) - static_cast<int32>(spread);
}

// The skill from which a bot knows a tactic: the configured threshold moved per bot and tactic, so knowledge is patchy.
// A threshold of 0 means every bot knows it (no jitter); the result stays in 1-100 otherwise (100 = no bot).
inline uint8 TacticKnownFrom(uint8 knownFromSkill, uint32 spread, uint64 guid, TacticId id)
{
    if (knownFromSkill == 0)
        return 0;

    uint64 const hash = SplitMix64(((guid << 8) | static_cast<uint64>(id)) ^ TacticGateSalt::Knowledge);
    return static_cast<uint8>(std::clamp<int32>(knownFromSkill + SkillGateJitter(hash, spread), 1, 100));
}

// The chance (percent) that a bot that knows the tactic uses it in a window, lerped over skill
inline uint8 TacticUseChance(TacticSettings const& settings, uint8 skill, float exponent)
{
    if (skill == 0)
        return SKILL_GATE_FULL;

    return static_cast<uint8>(std::min<uint32>(
        SKILL_GATE_FULL,
        SkillLerpRounded(settings.UseChanceAtSkill1, settings.UseChanceAtSkill99, SkillPosition(skill), exponent)));
}

// D4's windows: windowMs long, each bot's windows start at its own phase so that bots do not change in step
inline uint64 TacticUseWindow(uint64 guid, uint32 windowMs, uint32 nowMs)
{
    uint32 const length = std::max<uint32>(windowMs, 1);
    return (static_cast<uint64>(nowMs) + SplitMix64(guid ^ TacticGateSalt::UsePhase) % length) / length;
}

// One answer per bot, tactic and window (a hash, not a roll per update)
inline bool TacticUsedInWindow(uint64 guid, TacticId id, uint32 chancePercent, uint32 windowMs, uint32 nowMs)
{
    if (chancePercent >= SKILL_GATE_FULL)
        return true;

    if (chancePercent == 0)
        return false;

    uint64 const window = TacticUseWindow(guid, windowMs, nowMs);
    uint64 const hash = SplitMix64((guid << 32) ^ (static_cast<uint64>(id) << 56) ^ window ^ TacticGateSalt::Use);
    return hash % 100 < chancePercent;
}

// How hard the bot tries, 0-100: lerped over skill (or grit), moved per bot by -EffortSpread..+EffortSpread unless
// the grit attribute carries the spread (jitter false)
inline uint8 SkillEffort(TacticGateParams const& params, uint8 skill, uint64 guid, bool jitter = true)
{
    if (skill == 0)
        return SKILL_GATE_FULL;

    int32 const base = static_cast<int32>(
        SkillLerpRounded(params.EffortAtSkill1, params.EffortAtSkill99, SkillPosition(skill), params.CurveExponent));
    int32 const moved = jitter ? SkillGateJitter(SplitMix64(guid ^ TacticGateSalt::Effort), params.EffortSpread) : 0;
    return static_cast<uint8>(std::clamp<int32>(base + moved, 0, SKILL_GATE_FULL));
}

// A style trait, 50 +- StyleSpread: from the guid only, never from the skill or the percentile
inline uint8 SkillStyle(uint32 spread, uint64 guid, SkillStyleTrait trait)
{
    uint64 const hash = SplitMix64(((guid << 8) | static_cast<uint64>(trait)) ^ TacticGateSalt::Style);
    return static_cast<uint8>(
        std::clamp<int32>(SKILL_STYLE_NEUTRAL + SkillGateJitter(hash, spread), 0, SKILL_GATE_FULL));
}

// The whole gate of one bot from its attributes (SetPvpSkill caches it): knowledge against the jittered threshold,
// discipline for the use chance, grit for the effort
inline TacticGateState ComputeTacticGate(TacticGateParams const& params,
                                         std::array<TacticSettings, TACTIC_COUNT> const& tactics,
                                         TacticGateInputs const& inputs, uint64 guid)
{
    uint8 const skill = inputs.Skill;
    TacticGateState state;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        TacticId const id = static_cast<TacticId>(index);
        state.KnownFrom[index] = TacticKnownFrom(tactics[index].KnownFromSkill, params.KnowledgeSpread, guid, id);
        state.UseChance[index] =
            TacticUseChance(tactics[index], skill == 0 ? 0 : inputs.Discipline, params.CurveExponent);
        if (skill == 0 || inputs.Knowledge >= state.KnownFrom[index])
            state.KnownMask |= TacticBit(id);
    }

    state.Effort = SkillEffort(params, skill == 0 ? 0 : inputs.Grit, guid, inputs.EffortJitter);
    for (uint32 index = 0; index < SKILL_STYLE_TRAIT_COUNT; ++index)
    {
        if (skill == 0)
            state.Style[index] = SKILL_STYLE_NEUTRAL;
        else if (inputs.StyleMask & (1u << index))
            state.Style[index] = inputs.Style[index];
        else
            state.Style[index] = SkillStyle(params.StyleSpread, guid, static_cast<SkillStyleTrait>(index));
    }

    return state;
}

// The same from the skill alone (every attribute the skill: G0g's gate)
inline TacticGateState ComputeTacticGate(TacticGateParams const& params,
                                         std::array<TacticSettings, TACTIC_COUNT> const& tactics, uint8 skill,
                                         uint64 guid)
{
    TacticGateInputs inputs;
    inputs.Skill = skill;
    inputs.Knowledge = skill;
    inputs.Discipline = skill;
    inputs.Grit = skill;
    return ComputeTacticGate(params, tactics, inputs, guid);
}

// TacticActive after the enable and scope test (G0c): full ability (skill 0, an exempt bot, PvE with
// Playerbots.Skill.FullAbilityInPvE) uses every tactic; otherwise the bot knows it and uses it in this window
inline bool TacticGatePasses(TacticGateState const& state, TacticId id, bool fullAbility, uint64 guid, uint32 windowMs,
                             uint32 nowMs)
{
    if (fullAbility)
        return true;

    if (!(state.KnownMask & TacticBit(id)))
        return false;

    return TacticUsedInWindow(guid, id, state.UseChance[static_cast<uint32>(id)], windowMs, nowMs);
}

// Playerbots.Skill.ExemptRealMasterBots: a bot with a game-client master plays at full ability only while the option
// is 1 (the default, phase D's behaviour); with 0 it keeps its skill (DT.2). PlayerbotAI::IsSkillExempt asks the
// master only when the option is on.
inline bool SkillExemptFor(bool exemptOption, bool hasRealMaster) { return exemptOption && hasRealMaster; }

#endif
