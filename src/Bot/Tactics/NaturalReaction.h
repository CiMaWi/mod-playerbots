/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALREACTION_H
#define _PLAYERBOT_NATURALREACTION_H

#include <algorithm>
#include <cmath>

#include "Common.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// Natural reaction times and attention (Playerbots.Natural.Reaction.*). A bot reacts to an event (an enemy cast to
// kick, a debuff to dispel, a new target) after a human reaction time: log-normal with a median and a spread lerped
// over skill, at least MinMs, drawn from a hash through the inverse normal CDF (no generator state). An event behind
// the bot (outside its view cone) costs a penalty, and so does a change of focus from another category.
// Pure functions: no bots, no config singleton, so .playerbots tactics selftest reaction drives them with synthetic
// hashes, skills and times. PlayerbotAI::SetPvpSkill caches a ReactionState per bot; SkillMultiplier keeps one
// ReactionEvent per notice category and the bot's ReactionFocus. Hidden like the skill: only GM commands show them.

struct ReactionParams
{
    // Median reaction time (ms) and log-space spread at skill 1 and 99 (lerped along Playerbots.Skill.CurveExponent)
    float MedianMsAtSkill1 = 690.0f;
    float MedianMsAtSkill99 = 220.0f;
    float SigmaAtSkill1 = 0.45f;
    float SigmaAtSkill99 = 0.25f;
    uint32 MinMs = 150;
    // Added once per event: target outside the view cone, and a change of focus within FocusTimeoutMs
    uint32 BehindMinMs = 150;
    uint32 BehindMaxMs = 400;
    uint32 SwitchMinMs = 100;
    uint32 SwitchMaxMs = 300;
    float ConeDegrees = 200.0f;
    uint32 FocusTimeoutMs = 2000;
};

// Per bot, cached by SetPvpSkill: ln(median) and sigma of the reaction time at the bot's skill
struct ReactionState
{
    float Mu = 0.0f;
    float Sigma = 0.0f;
};

// One per bot and notice category: the event the category's action answers. An event begins at the first query
// after a quiet gap; the reaction time counts from there.
struct ReactionEvent
{
    uint32 StartMs = 0;
    uint32 LastQueryMs = 0;
    uint32 ReadyAfterMs = 0;  // reaction time plus penalties, from StartMs
    bool Active = false;
    bool Noticed = false;
    bool Missed = false;
    bool Behind = false;    // the behind penalty applies
    bool Switched = false;  // the switch cost applies
};

// One per bot: the category the bot attends to (0 = none) and when it last acted on it
struct ReactionFocus
{
    uint8 Category = 0;
    uint32 AtMs = 0;
};

// A noticed event ends after this quiet gap (two AI updates of the slowest bot); a pending one only after the longer
// gap, so a trigger checked once a second still reaches its reaction time instead of restarting every time
constexpr uint32 REACTION_NOTICED_GAP_MS = 600;
constexpr uint32 REACTION_PENDING_GAP_MS = 2500;
constexpr uint32 REACTION_MAX_MS = 60000;

namespace ReactionSalt
{
constexpr uint64 Event = 0x6A09E667F3BCC909ULL;
constexpr uint64 Miss = 0xBB67AE8584CAA73BULL;
constexpr uint64 Behind = 0x3C6EF372FE94F82CULL;
constexpr uint64 Switch = 0xA54FF53A5F1D36F2ULL;
}  // namespace ReactionSalt

// The inverse of the standard normal CDF (Acklam's rational approximation, relative error below 1.2e-9)
inline double Probit(double u)
{
    constexpr double A[] = {-3.969683028665376e+01, 2.209460984245205e+02,  -2.759285104469687e+02,
                            1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
    constexpr double B[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                            6.680131188771972e+01, -1.328068155288572e+01};
    constexpr double C[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                            -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
    constexpr double D[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
    constexpr double Low = 0.02425;

    u = std::clamp(u, 1e-12, 1.0 - 1e-12);
    if (u < Low || u > 1.0 - Low)
    {
        double const q = std::sqrt(-2.0 * std::log(u < Low ? u : 1.0 - u));
        double const x = (((((C[0] * q + C[1]) * q + C[2]) * q + C[3]) * q + C[4]) * q + C[5]) /
                         ((((D[0] * q + D[1]) * q + D[2]) * q + D[3]) * q + 1.0);
        return u < Low ? x : -x;
    }

    double const q = u - 0.5;
    double const r = q * q;
    return (((((A[0] * r + A[1]) * r + A[2]) * r + A[3]) * r + A[4]) * r + A[5]) * q /
           (((((B[0] * r + B[1]) * r + B[2]) * r + B[3]) * r + B[4]) * r + 1.0);
}

// A hash as a number strictly between 0 and 1 (52 bits: the largest value, 1 - 2^-53, is still below 1 in a double)
inline double UnitFromHash(uint64 hash) { return (static_cast<double>(hash >> 12) + 0.5) * (1.0 / 4503599627370496.0); }

// A whole number from low to high (both included), evenly, from a hash
inline uint32 UniformFromHash(uint64 hash, uint32 low, uint32 high)
{
    if (high <= low)
        return low;

    return low + static_cast<uint32>(hash % (static_cast<uint64>(high - low) + 1));
}

// Skill 0 (the skill system off) counts as skill 99
inline ReactionState ComputeReactionState(ReactionParams const& params, uint8 skill, float curveExponent)
{
    float const position = SkillPosition(skill == 0 ? 99 : skill);
    ReactionState state;
    float const median = SkillLerp(params.MedianMsAtSkill1, params.MedianMsAtSkill99, position, curveExponent);
    state.Mu = std::log(std::max(median, 1.0f));
    state.Sigma = std::max(0.0f, SkillLerp(params.SigmaAtSkill1, params.SigmaAtSkill99, position, curveExponent));
    return state;
}

// The log-normal draw before the minimum (its median is exp(Mu))
inline double ReactionRawMs(ReactionState const& state, uint64 hash)
{
    return std::exp(state.Mu + state.Sigma * Probit(UnitFromHash(hash)));
}

inline uint32 ReactionMs(ReactionState const& state, uint32 minMs, uint64 hash)
{
    double const raw = std::min(ReactionRawMs(state, hash), static_cast<double>(REACTION_MAX_MS));
    return std::max(minMs, static_cast<uint32>(raw + 0.5));
}

inline uint32 BehindPenaltyMs(ReactionParams const& params, uint64 hash)
{
    return UniformFromHash(SplitMix64(hash ^ ReactionSalt::Behind), params.BehindMinMs, params.BehindMaxMs);
}

inline uint32 SwitchCostMs(ReactionParams const& params, uint64 hash)
{
    return UniformFromHash(SplitMix64(hash ^ ReactionSalt::Switch), params.SwitchMinMs, params.SwitchMaxMs);
}

// The focus is elsewhere: another category acted on within FocusTimeoutMs
inline bool ReactionFocusElsewhere(ReactionFocus const& focus, ReactionParams const& params, uint8 category,
                                   uint32 nowMs)
{
    return focus.Category != 0 && focus.Category != category && nowMs - focus.AtMs < params.FocusTimeoutMs;
}

// The reaction time of one event: the draw plus the behind penalty and the switch cost when they apply
inline uint32 EventReactionMs(ReactionState const& state, ReactionParams const& params, uint64 hash, bool behind,
                              bool switched)
{
    uint32 ready = ReactionMs(state, params.MinMs, hash);
    if (behind)
        ready += BehindPenaltyMs(params, hash);

    if (switched)
        ready += SwitchCostMs(params, hash);

    return ready;
}

// The hash of one event: guid, category and start time
inline uint64 ReactionEventHash(uint64 guid, uint8 category, uint32 startMs)
{
    return SplitMix64((guid << 32) ^ (static_cast<uint64>(category) << 56) ^ startMs ^ ReactionSalt::Event);
}

// True when a query at nowMs begins a new event: none yet, a quiet gap after it, or a missed event's window is over
inline bool ReactionEventStarts(ReactionEvent const& event, uint32 nowMs, uint32 missWindowMs)
{
    if (!event.Active)
        return true;

    if (nowMs - event.LastQueryMs > (event.Noticed ? REACTION_NOTICED_GAP_MS : REACTION_PENDING_GAP_MS))
        return true;

    return event.Missed && nowMs - event.StartMs >= missWindowMs;
}

// Begins an event: one draw, the penalties and the miss decision (missPercent: D4's per-window chance, per event)
inline void StartReactionEvent(ReactionEvent& event, ReactionFocus const& focus, ReactionState const& state,
                               ReactionParams const& params, uint64 guid, uint8 category, uint32 missPercent,
                               bool behind, uint32 nowMs)
{
    uint64 const hash = ReactionEventHash(guid, category, nowMs);
    event.StartMs = nowMs;
    event.LastQueryMs = nowMs;
    event.Active = true;
    event.Noticed = false;
    event.Missed = missPercent != 0 && SplitMix64(hash ^ ReactionSalt::Miss) % 100 < missPercent;
    event.Behind = behind;
    event.Switched = ReactionFocusElsewhere(focus, params, category, nowMs);
    event.ReadyAfterMs = EventReactionMs(state, params, hash, event.Behind, event.Switched);
}

// A query of the event's action: passes once the reaction time is over (never for a missed event). The notice (the
// first pass) moves the focus to the category; later passes do not keep it there, because the engine asks the
// multipliers for every queued action, executed or not, so a category whose trigger stays active would hold the
// focus for good and make every other event pay the switch cost
inline bool ReactionEventPasses(ReactionEvent& event, ReactionFocus& focus, uint8 category, uint32 nowMs)
{
    event.LastQueryMs = nowMs;
    if (event.Missed)
        return false;

    if (!event.Noticed)
    {
        if (nowMs - event.StartMs < event.ReadyAfterMs)
            return false;

        event.Noticed = true;
        focus.Category = category;
        focus.AtMs = nowMs;
    }

    return true;
}

#endif
