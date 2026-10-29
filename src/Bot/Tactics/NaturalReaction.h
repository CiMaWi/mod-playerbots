/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALREACTION_H
#define _PLAYERBOT_NATURALREACTION_H

#include <algorithm>
#include <array>
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
// End to end (EndToEnd): the target counts from the event's server time (a cast bar's start, an aura's application)
// and adds the modelled round trip, the display and the server ticks of a human's answer; the bot passes at its first
// check at or after the target minus its own check latency, learnt from how far its passes miss their targets (at a
// steady interval: its mean check overshoot), so its action lands on the target on average. No check is woken for
// it: the check interval (Playerbots.Natural.Cadence.*) is the model's resolution.
// Reaction types (G0h3, each part behind its own Enable): a choice costs a Hick's-law price per doubling of the real
// options (Wisdom), a predicted event (the opponent memory's) a shorter draw (Foresight), other pending events a load
// penalty and a miss share (Focus, which keeps them off the most important one), a hard button its ease price
// (Dexterity) and a decision its thinking time (Wisdom: shortcut or deep check). All of it lies inside the event's
// reaction (ReadyAfterMs), so the end-to-end target keeps its sum. SetPvpSkill caches the prices per bot in a
// ReactionTypeState. Also here for G0h3-b: the queue-like follow-up after the bot's own cast or GCD, the busy bot's
// interrupt rules (DT.11 addendum, 2026-10-30) and the per-match OpponentMemory.

// The structure of a reaction: one known answer, a choice between real options, or an event the bot predicted
enum class ReactionType : uint8
{
    Simple = 0,
    Choice,
    Anticipated
};

// How hard the answer is to give: a common button, a rare one (defensive cooldowns, crowd-control breaks), an action
// on the focus target (G4e) or on a new target (a unit other than the bot's target)
enum class ReactionEase : uint8
{
    Common = 0,
    Rare,
    Focus,
    NewTarget,
    Count
};

constexpr uint32 REACTION_EASE_COUNT = static_cast<uint32>(ReactionEase::Count);

// The weak bot's shortcut (it answers the first event it sees) or the skilled bot's deep check (it judges first)
enum class ReactionThinkDepth : uint8
{
    Shortcut = 0,
    Deep
};

// G0h3's parts, each with its Enable (0: G0h2's reaction) and its prices at attribute 1 and 99 (lerped along
// Playerbots.Skill.CurveExponent)
struct ReactionChoiceParams  // Wisdom
{
    bool Enable = false;
    float MsPerDoublingAt1 = 120.0f;
    float MsPerDoublingAt99 = 50.0f;
    uint32 MaxOptions = 8;
};

struct ReactionLoadParams  // Focus
{
    bool Enable = false;
    uint32 MsPerEventAt1 = 200;
    uint32 MsPerEventAt99 = 80;
    uint32 MissPercentPerEventAt1 = 10;
    uint32 MissPercentPerEventAt99 = 2;
    // The share of the penalty and the miss share kept off an event that outranks every pending one
    float TopProtectAt1 = 0.0f;
    float TopProtectAt99 = 0.8f;
    uint32 MaxEvents = 3;
};

struct ReactionEaseParams  // Dexterity
{
    bool Enable = false;
    // By ReactionEase, strictly rising at both ends (so at every Dexterity)
    std::array<uint32, REACTION_EASE_COUNT> MsAt1 = {0, 250, 350, 450};
    std::array<uint32, REACTION_EASE_COUNT> MsAt99 = {0, 80, 120, 180};
    // Below this Dexterity an action on the focus target costs a new target's price
    uint8 FocusDexterity = 40;
};

struct ReactionThinkParams  // Wisdom (depth and cost), Patience (waiting)
{
    bool Enable = false;
    uint32 ShortcutMsAt1 = 40;
    uint32 ShortcutMsAt99 = 20;
    uint32 DeepMsAt1 = 400;
    uint32 DeepMsAt99 = 150;
    uint32 DeepPercentAt1 = 0;
    uint32 DeepPercentAt99 = 100;
    // How long a bot waits for a better moment at the Patience trait 0 and 100 (straight line)
    uint32 PatienceWaitMsAt0 = 0;
    uint32 PatienceWaitMsAt100 = 1500;
};

struct ReactionAnticipateParams  // Foresight
{
    bool Enable = false;
    // The share of the draw a predicted event saves (never below MinMs)
    float ShareAt1 = 0.1f;
    float ShareAt99 = 0.5f;
};

struct ReactionQueueParams  // Speed (G0h3-b)
{
    bool Enable = false;
    uint32 MinMsAt1 = 60;
    uint32 MaxMsAt1 = 250;
    uint32 MinMsAt99 = 10;
    uint32 MaxMsAt99 = 80;
};

// The busy bot (G0h3-b; the user's DT.11 addendum, 2026-10-30)
struct ReactionReevalParams
{
    // Stop the bot's own cast for a kick of an important enemy cast (at least ImportantCastMs: a stand-in for G5b's
    // judgement) when the own cast has MinOwnRemainingMs left and would end too late for the kick; once per enemy cast
    bool Enable = false;
    uint32 ImportantCastMs = 1500;
    uint32 MinOwnRemainingMs = 300;
    // No kick that a bot busy with its own cast or GCD could land only within BusyMarginMs of the enemy cast's end,
    // when being busy is what makes it late
    bool BusySkip = false;
    uint32 BusyMarginMs = 100;
};

struct ReactionMemoryParams  // G0h3-b
{
    bool Enable = false;
    // The fake-cast rate counts from MinCasts casts seen; a cast gone within EndToleranceMs of its bar's end finished
    uint32 MinCasts = 3;
    uint32 EndToleranceMs = 150;
};

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
    // End to end: PingMs is the modelled round trip (half to see the event, half for the answer to reach the server),
    // DisplayMs the client's frame and display delay, ServerTickMs the server ticks (-1: half the bot's map update)
    bool EndToEnd = false;
    uint32 PingMs = 60;
    uint32 DisplayMs = 30;
    int32 ServerTickMs = -1;
    // G0h3's parts; MovementEnable: avoid aoe as a reaction (G0h3-b)
    ReactionChoiceParams Choice;
    ReactionLoadParams Load;
    ReactionEaseParams Ease;
    ReactionThinkParams Think;
    ReactionAnticipateParams Anticipate;
    ReactionQueueParams Queue;
    ReactionReevalParams Reeval;
    ReactionMemoryParams Memory;
    bool MovementEnable = false;
};

// Per bot, cached by SetPvpSkill: ln(median) and sigma of the reaction time at the bot's skill
struct ReactionState
{
    float Mu = 0.0f;
    float Sigma = 0.0f;
};

// Per bot, cached by SetPvpSkill from its attributes: the prices of G0h3's parts (all 0 at skill 0: full ability).
// The parts' Enable is read at each event, so .reload config and .playerbots rndbot reload switch them for logged-in
// bots; changed prices reach a bot at its next skill load.
struct ReactionTypeState
{
    float HickMsPerDoubling = 0.0f;
    uint32 LoadMsPerEvent = 0;
    uint32 LoadMissPercentPerEvent = 0;
    float LoadTopProtect = 0.0f;
    std::array<uint32, REACTION_EASE_COUNT> EaseMs{};
    uint32 ThinkShortcutMs = 0;
    uint32 ThinkDeepMs = 0;
    uint32 ThinkDeepPercent = 0;
    uint32 PatienceWaitMs = 0;
    float AnticipateShare = 0.0f;
    uint32 QueueMinMs = 0;
    uint32 QueueMaxMs = 0;
};

// The attributes the prices come from (abilities 1-99, the Patience trait 0-100)
struct ReactionTypeInputs
{
    uint8 Speed = 99;
    uint8 Wisdom = 99;
    uint8 Focus = 99;
    uint8 Dexterity = 99;
    uint8 Foresight = 99;
    uint8 Patience = 50;
};

// One event's inputs to the parts, from what the bot's logic has already built (the defaults: G0h2's event)
struct ReactionEventInputs
{
    uint32 Options = 1;    // the real options the answer is chosen from
    uint32 Load = 0;       // other events pending
    bool LoadTop = false;  // this event outranks every pending one
    ReactionEase Ease = ReactionEase::Common;
    bool Thinks = false;  // the answer takes a decision
    ReactionThinkDepth Depth = ReactionThinkDepth::Shortcut;
    bool Predicted = false;  // the opponent memory predicts the event
};

// Where an event's server time came from
enum class ReactionStampSource : uint8
{
    FirstQuery = 0,  // no visible start: the bot's first query
    CastBar,         // an enemy cast: now minus the elapsed part of its cast bar
    Aura,            // an aura: now minus the elapsed part of its duration
    Stale            // a cast bar or aura older than REACTION_MAX_STAMP_AGE_MS: the bot's first query
};

// One per bot and notice category: the event the category's action answers. An event begins at the first query
// after a quiet gap; the reaction time counts from there, with EndToEnd from the event's server time.
struct ReactionEvent
{
    uint32 StartMs = 0;
    uint32 LastQueryMs = 0;
    uint32 ReadyAfterMs = 0;  // reaction time plus penalties, from StartMs
    uint32 StampAgeMs = 0;    // the event's server time this long before StartMs (0 without EndToEnd)
    uint32 TargetMs = 0;      // end-to-end target from the server time (ReadyAfterMs without EndToEnd)
    uint32 DueAfterMs = 0;    // the pass from StartMs: target minus stamp age and the bot's latency, at least 0
    uint8 StampSource = 0;    // ReactionStampSource
    bool Active = false;
    bool Noticed = false;
    bool Missed = false;
    bool Behind = false;     // the behind penalty applies
    bool Switched = false;   // the switch cost applies
    bool Corrected = false;  // the target lies after the first query: the pass's miss trains the bot's latency
    uint32 CostMs = 0;       // G0h3's choice, load, ease and think costs, inside ReadyAfterMs
    uint32 ThinkMs = 0;      // the think part of CostMs
    uint8 Type = 0;          // ReactionType
    uint8 Options = 1;
    uint8 Load = 0;
    uint8 Ease = 0;    // ReactionEase
    uint8 Busy = 0;    // ReactionBusyAnswer (G0h3-b)
    uint32 OwnMs = 0;  // the bot's own cast within the event's time before its first query (G0h3-b)
};

// How a busy bot met an event (G0h3-b, the trace's busy field)
enum class ReactionBusyAnswer : uint8
{
    None = 0,
    StoppedOwnCast,  // it stopped its own cast for the kick (user note 1)
    Skipped          // its kick could land only at the cast's end or after it (user note 2)
};

// One per bot: the category the bot attends to (0 = none) and when it last acted on it, and the bot's check latency
// (EndToEnd only)
struct ReactionFocus
{
    uint8 Category = 0;
    uint32 AtMs = 0;
    float LatencyMs = 0.0f;
    bool LatencySeeded = false;
};

// A noticed event ends after this quiet gap (two AI updates of the slowest bot); a pending one only after the longer
// gap, so a trigger checked once a second still reaches its reaction time instead of restarting every time
constexpr uint32 REACTION_NOTICED_GAP_MS = 600;
constexpr uint32 REACTION_PENDING_GAP_MS = 2500;
constexpr uint32 REACTION_MAX_MS = 60000;
// An event's server time older than this at the first query counts from the first query (the bot turns to it now)
constexpr uint32 REACTION_MAX_STAMP_AGE_MS = 3000;
// The weight of one overshoot in the bot's running latency mean
constexpr float REACTION_LATENCY_WEIGHT = 0.125f;
// Upstream's stagger of a bot's next AI check (PR #2250): 0-200 ms from the guid
constexpr uint32 UPSTREAM_CHECK_STAGGER_MS = 200;

namespace ReactionSalt
{
constexpr uint64 Event = 0x6A09E667F3BCC909ULL;
constexpr uint64 Miss = 0xBB67AE8584CAA73BULL;
constexpr uint64 Behind = 0x3C6EF372FE94F82CULL;
constexpr uint64 Switch = 0xA54FF53A5F1D36F2ULL;
constexpr uint64 Think = 0x510E527FADE682D1ULL;
constexpr uint64 Queue = 0x9B05688C2B3E6C1FULL;
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

// G0h3's prices at the bot's attributes (the parts' Enable aside: read at each event)
inline ReactionTypeState ComputeReactionTypeState(ReactionParams const& params, ReactionTypeInputs const& inputs,
                                                  float curveExponent)
{
    auto const at = [curveExponent](float atSkill1, float atSkill99, uint8 attribute)
    { return SkillLerp(atSkill1, atSkill99, SkillPosition(attribute), curveExponent); };
    auto const atRounded = [curveExponent](uint32 atSkill1, uint32 atSkill99, uint8 attribute)
    { return SkillLerpRounded(atSkill1, atSkill99, SkillPosition(attribute), curveExponent); };

    ReactionTypeState state;
    state.HickMsPerDoubling =
        std::max(0.0f, at(params.Choice.MsPerDoublingAt1, params.Choice.MsPerDoublingAt99, inputs.Wisdom));
    state.LoadMsPerEvent = atRounded(params.Load.MsPerEventAt1, params.Load.MsPerEventAt99, inputs.Focus);
    state.LoadMissPercentPerEvent =
        atRounded(params.Load.MissPercentPerEventAt1, params.Load.MissPercentPerEventAt99, inputs.Focus);
    state.LoadTopProtect =
        std::clamp(at(params.Load.TopProtectAt1, params.Load.TopProtectAt99, inputs.Focus), 0.0f, 1.0f);
    for (uint32 ease = 0; ease < REACTION_EASE_COUNT; ++ease)
        state.EaseMs[ease] = atRounded(params.Ease.MsAt1[ease], params.Ease.MsAt99[ease], inputs.Dexterity);

    if (inputs.Dexterity < params.Ease.FocusDexterity)
        state.EaseMs[static_cast<uint32>(ReactionEase::Focus)] =
            state.EaseMs[static_cast<uint32>(ReactionEase::NewTarget)];

    state.ThinkShortcutMs = atRounded(params.Think.ShortcutMsAt1, params.Think.ShortcutMsAt99, inputs.Wisdom);
    state.ThinkDeepMs = atRounded(params.Think.DeepMsAt1, params.Think.DeepMsAt99, inputs.Wisdom);
    state.ThinkDeepPercent =
        std::min(100u, atRounded(params.Think.DeepPercentAt1, params.Think.DeepPercentAt99, inputs.Wisdom));
    float const patience = static_cast<float>(std::min<uint8>(inputs.Patience, 100)) / 100.0f;
    state.PatienceWaitMs = static_cast<uint32>(
        params.Think.PatienceWaitMsAt0 +
        (static_cast<float>(params.Think.PatienceWaitMsAt100) - params.Think.PatienceWaitMsAt0) * patience + 0.5f);
    state.AnticipateShare =
        std::clamp(at(params.Anticipate.ShareAt1, params.Anticipate.ShareAt99, inputs.Foresight), 0.0f, 0.95f);
    state.QueueMinMs = atRounded(params.Queue.MinMsAt1, params.Queue.MinMsAt99, inputs.Speed);
    state.QueueMaxMs =
        std::max(state.QueueMinMs, atRounded(params.Queue.MaxMsAt1, params.Queue.MaxMsAt99, inputs.Speed));
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

// Hick's law: the price per doubling of the real options (one option: none), at most maxOptions counted
inline uint32 HickCostMs(ReactionTypeState const& state, uint32 options, uint32 maxOptions)
{
    uint32 const counted = std::min(options, std::max(maxOptions, 1u));
    if (counted < 2)
        return 0;

    return static_cast<uint32>(state.HickMsPerDoubling * std::log2(static_cast<float>(counted)) + 0.5f);
}

// The load of other pending events: a penalty and a miss share per event; an event that outranks every pending one
// keeps the protected share of both off
inline uint32 LoadPenaltyMs(ReactionTypeState const& state, uint32 load, bool top)
{
    float const kept = top ? 1.0f - state.LoadTopProtect : 1.0f;
    return static_cast<uint32>(static_cast<float>(state.LoadMsPerEvent * load) * kept + 0.5f);
}

inline uint32 LoadMissPercent(ReactionTypeState const& state, uint32 load, bool top)
{
    float const kept = top ? 1.0f - state.LoadTopProtect : 1.0f;
    return static_cast<uint32>(static_cast<float>(state.LoadMissPercentPerEvent * load) * kept + 0.5f);
}

inline uint32 EaseCostMs(ReactionTypeState const& state, ReactionEase ease)
{
    return state.EaseMs[std::min(static_cast<uint32>(ease), REACTION_EASE_COUNT - 1)];
}

// The time one decision takes: the shortcut (the first event seen, no judgement) or the deep check
inline uint32 ThinkCostMs(ReactionTypeState const& state, ReactionThinkDepth depth)
{
    return depth == ReactionThinkDepth::Deep ? state.ThinkDeepMs : state.ThinkShortcutMs;
}

// The depth a bot of this Wisdom takes for one decision (the tactics' deep paths ask it; G0h3 has none and charges
// the interrupt's shortcut)
inline ReactionThinkDepth ChooseThinkDepth(ReactionTypeState const& state, uint64 hash)
{
    return SplitMix64(hash ^ ReactionSalt::Think) % 100 < state.ThinkDeepPercent ? ReactionThinkDepth::Deep
                                                                                 : ReactionThinkDepth::Shortcut;
}

// A predicted event's draw: the Foresight share of it saved, never below minMs nor above the draw
inline uint32 AnticipatedMs(uint32 drawMs, float share, uint32 minMs)
{
    if (share <= 0.0f)
        return drawMs;

    uint32 const shortened = static_cast<uint32>(static_cast<float>(drawMs) * (1.0f - share) + 0.5f);
    return std::min(drawMs, std::max(minMs, shortened));
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

// The event waits for its answer (begun, not noticed, not missed, still asked within the pending gap): it loads the
// bot's other events
inline bool ReactionEventPending(ReactionEvent const& event, uint32 nowMs)
{
    return event.Active && !event.Noticed && !event.Missed && nowMs - event.LastQueryMs <= REACTION_PENDING_GAP_MS;
}

// Begins an event: one draw, the penalties and the miss decision (missPercent: D4's per-window chance, per event).
// With the bot's G0h3 prices (types) the parts that are on add their costs and the load its miss share; a predicted
// event (Anticipate) is no choice. The defaults give G0h2's event.
inline void StartReactionEvent(ReactionEvent& event, ReactionFocus const& focus, ReactionState const& state,
                               ReactionParams const& params, uint64 guid, uint8 category, uint32 missPercent,
                               bool behind, uint32 nowMs, ReactionTypeState const& types = ReactionTypeState(),
                               ReactionEventInputs const& inputs = ReactionEventInputs())
{
    uint64 const hash = ReactionEventHash(guid, category, nowMs);
    bool const predicted = params.Anticipate.Enable && inputs.Predicted && types.AnticipateShare > 0.0f;
    bool const choice = !predicted && params.Choice.Enable && inputs.Options >= 2;
    uint32 const load = params.Load.Enable ? std::min(inputs.Load, params.Load.MaxEvents) : 0;
    ReactionEase const ease = params.Ease.Enable ? inputs.Ease : ReactionEase::Common;
    event.ThinkMs = params.Think.Enable && inputs.Thinks ? ThinkCostMs(types, inputs.Depth) : 0;
    event.CostMs = (choice ? HickCostMs(types, inputs.Options, params.Choice.MaxOptions) : 0) +
                   LoadPenaltyMs(types, load, inputs.LoadTop) + (params.Ease.Enable ? EaseCostMs(types, ease) : 0) +
                   event.ThinkMs;
    event.Type = static_cast<uint8>(predicted ? ReactionType::Anticipated
                                              : (choice ? ReactionType::Choice : ReactionType::Simple));
    event.Options = static_cast<uint8>(std::min<uint32>(inputs.Options, 255));
    event.Load = static_cast<uint8>(load);
    event.Ease = static_cast<uint8>(ease);
    uint32 const missShare = std::min<uint32>(100, missPercent + LoadMissPercent(types, load, inputs.LoadTop));
    event.StartMs = nowMs;
    event.LastQueryMs = nowMs;
    event.Active = true;
    event.Noticed = false;
    event.Missed = missShare != 0 && SplitMix64(hash ^ ReactionSalt::Miss) % 100 < missShare;
    event.Behind = behind;
    event.Switched = ReactionFocusElsewhere(focus, params, category, nowMs);
    uint32 saved = 0;
    if (predicted)
    {
        uint32 const draw = ReactionMs(state, params.MinMs, hash);
        saved = draw - AnticipatedMs(draw, types.AnticipateShare, params.MinMs);
    }

    event.Busy = static_cast<uint8>(ReactionBusyAnswer::None);
    event.OwnMs = 0;
    event.ReadyAfterMs = EventReactionMs(state, params, hash, event.Behind, event.Switched) - saved + event.CostMs;
    event.StampAgeMs = 0;
    event.TargetMs = event.ReadyAfterMs;
    event.DueAfterMs = event.ReadyAfterMs;
    event.StampSource = static_cast<uint8>(ReactionStampSource::FirstQuery);
    event.Corrected = false;
}

// The end-to-end target: the event's reaction plus the modelled round trip, the display and the server ticks
inline uint32 EndToEndTargetMs(ReactionParams const& params, uint32 readyMs, uint32 tickMs)
{
    if (!params.EndToEnd)
        return readyMs;

    return readyMs + params.PingMs + params.DisplayMs + tickMs;
}

// With EndToEnd, right after StartReactionEvent: the event happened stampAgeMs before its first query (too old: at
// the first query, source Stale). The pass is due at the target minus the stamp age and the bot's latency, never
// before the first query; a bot that first looks after that (busy, or a cast seen late) passes at once
inline void StampReactionEvent(ReactionEvent& event, ReactionFocus const& focus, ReactionParams const& params,
                               uint32 stampAgeMs, ReactionStampSource source, uint32 tickMs)
{
    if (!params.EndToEnd)
        return;

    if (stampAgeMs > REACTION_MAX_STAMP_AGE_MS)
    {
        stampAgeMs = 0;
        source = ReactionStampSource::Stale;
    }

    event.StampAgeMs = stampAgeMs;
    event.StampSource = static_cast<uint8>(source);
    event.TargetMs = EndToEndTargetMs(params, event.ReadyAfterMs, tickMs);
    uint32 const spent = stampAgeMs + static_cast<uint32>(focus.LatencyMs + 0.5f);
    event.Corrected = event.TargetMs > stampAgeMs;
    event.DueAfterMs = event.TargetMs > spent ? event.TargetMs - spent : 0;
}

// A pass missed its target by missMs (late > 0): the latency moves by a share of it (the first miss wholly), never
// below 0, so at a steady check interval it settles on the mean overshoot. A miss above capMs (a pause such as the
// bot's own cast, not its check interval) is left out (capMs 0: none).
inline void NoteReactionLatency(ReactionFocus& focus, int32 missMs, uint32 capMs)
{
    if (capMs != 0 && missMs > static_cast<int32>(capMs))
        return;

    float const miss = static_cast<float>(missMs);
    focus.LatencyMs = std::max(0.0f, focus.LatencyMs + (focus.LatencySeeded ? miss * REACTION_LATENCY_WEIGHT : miss));
    focus.LatencySeeded = true;
}

// The check interval of a reaction trigger or value (cure, dispel): at most the natural cadence's while its faster
// check runs, so the first look and the later queries of the answer follow the cadence; else its own (1: every check)
inline uint32 ReactionCheckIntervalMs(uint32 intervalMs, bool cadenceActive, uint32 cadenceCheckMs)
{
    return cadenceActive && cadenceCheckMs < intervalMs ? cadenceCheckMs : intervalMs;
}

// The stagger of a bot's next AI check from its guid (upstream's guid % 201 at UPSTREAM_CHECK_STAGGER_MS)
inline uint32 CheckStaggerMs(uint64 guid, uint32 staggerMs)
{
    return static_cast<uint32>(guid % (static_cast<uint64>(staggerMs) + 1));
}

// A query of the event's action: passes once the reaction time is over (never for a missed event). The notice (the
// first pass) moves the focus to the category; later passes do not keep it there, because the engine asks the
// multipliers for every queued action, executed or not, so a category whose trigger stays active would hold the
// focus for good and make every other event pay the switch cost. A corrected event's notice trains the latency.
inline bool ReactionEventPasses(ReactionEvent& event, ReactionFocus& focus, uint8 category, uint32 nowMs,
                                uint32 latencyCapMs = 0)
{
    event.LastQueryMs = nowMs;
    if (event.Missed)
        return false;

    if (!event.Noticed)
    {
        uint32 const waited = nowMs - event.StartMs;
        if (waited < event.DueAfterMs)
            return false;

        event.Noticed = true;
        focus.Category = category;
        focus.AtMs = nowMs;
        if (event.Corrected)
            NoteReactionLatency(focus, static_cast<int32>(waited + event.StampAgeMs - event.TargetMs), latencyCapMs);
    }

    return true;
}

// A tactic that ran its deep check charges the time to the running event before its notice (G5b on): the reaction,
// the target and the pass move by it (thinking starts when the bot looks)
inline void AddReactionThinkMs(ReactionEvent& event, uint32 thinkMs)
{
    if (!event.Active || event.Noticed || thinkMs == 0)
        return;

    event.ThinkMs += thinkMs;
    event.CostMs += thinkMs;
    event.ReadyAfterMs += thinkMs;
    event.TargetMs += thinkMs;
    event.DueAfterMs += thinkMs;
}

// The queue-like follow-up (G0h3-b): the bot's own cast or GCD ends remainingMs from now; its next check comes drawMs
// after that end when this is before its regular check (checkMs), else at the regular check. Never later, so no check
// within the GCD is lost (the kicks, breaks and defensives off the GCD keep theirs).
inline uint32 QueueCheckDelayMs(uint32 remainingMs, uint32 checkMs, uint32 drawMs)
{
    if (remainingMs >= checkMs)
        return checkMs;

    return std::min(checkMs, remainingMs + drawMs);
}

// The follow-up's draw (Speed): a human mashing the button
inline uint32 QueueDrawMs(ReactionTypeState const& state, uint64 hash)
{
    return UniformFromHash(SplitMix64(hash ^ ReactionSalt::Queue), state.QueueMinMs, state.QueueMaxMs);
}

// User note 1 (G0h3-b): the bot stops its own cast for a kick of an important enemy cast when finishing its own would
// leave the kick too late (the own cast ends within BusyMarginMs of the enemy's end, or after it)
inline bool StopOwnCastForKick(ReactionReevalParams const& params, uint32 enemyCastMs, uint32 enemyRemainingMs,
                               uint32 ownRemainingMs)
{
    return params.Enable && enemyCastMs >= params.ImportantCastMs && ownRemainingMs >= params.MinOwnRemainingMs &&
           ownRemainingMs + params.BusyMarginMs >= enemyRemainingMs;
}

// User note 2 (G0h3-b): a bot busy with its own cast, the kick's cooldown or the GCD does not kick a cast its answer
// reaches only within BusyMarginMs of the cast's end or after it, when being busy is what makes it late: readyInMs is
// the answer's due time from now as the bot meets it, freeInMs the one it would have had without being busy (below 0:
// already passed). A kick late from its reaction alone is not skipped.
inline bool InterruptOutOfReach(ReactionReevalParams const& params, uint32 readyInMs, int32 freeInMs,
                                uint32 enemyRemainingMs)
{
    int64 const reach = static_cast<int64>(enemyRemainingMs) - static_cast<int64>(params.BusyMarginMs);
    return params.BusySkip && static_cast<int64>(readyInMs) >= reach && static_cast<int64>(freeInMs) < reach;
}

// The kick's due time from now without the own cast that kept the bot from looking (event.OwnMs before now) or a
// wait: stamped (end to end), the target stays at the event's server time, so the unclipped due, no earlier than
// the own cast's start; unstamped, the draw counts from the first query, which would have come OwnMs earlier
inline int32 BusyFreeDueMs(ReactionEvent const& event, uint32 latencyMs, bool stamped)
{
    int32 const own = static_cast<int32>(event.OwnMs);
    if (!stamped)
        return static_cast<int32>(event.DueAfterMs) - own;

    int32 const due = static_cast<int32>(event.TargetMs) - static_cast<int32>(event.StampAgeMs + latencyMs);
    return std::max(due, -own);
}

// After a stop for a kick the engine did not cast (it chose another action or target), no stop for this long
constexpr uint32 REACTION_STOP_BACKOFF_MS = 30000;

// Per bot (G0h3-b, its own update only): the bot's own cast and GCD as it last saw them, the enemy cast it watches
// while casting (its due time from the cast's start), a stop for a kick and a skipped kick, and the busy path's counts
struct ReactionBusyState
{
    uint32 CastStartMs = 0;
    uint32 CastEndMs = 0;
    uint32 GcdEndMs = 0;
    uint32 WatchGuid = 0;
    uint32 WatchSpellId = 0;
    uint32 WatchEndMs = 0;
    uint32 WatchDueMs = 0;
    bool WatchSpent = false;   // the watched cast had its stop
    uint32 NoStopUntilMs = 0;  // 0: none
    uint32 KickStopMs = 0;
    uint32 KickTargetGuid = 0;
    uint32 KickSpellId = 0;
    uint32 SkipEndMs = 0;
    uint32 QueuedChecks = 0;
    uint32 StoppedCasts = 0;
    uint32 UnfollowedStops = 0;
    uint32 SkippedBusy = 0;

    // The enemy cast watched: the same while its bar keeps the spell and its end moves within toleranceMs (pushback)
    bool Watches(uint32 guid, uint32 spellId, uint32 endMs, uint32 toleranceMs) const
    {
        uint32 const moved = endMs > WatchEndMs ? endMs - WatchEndMs : WatchEndMs - endMs;
        return WatchGuid == guid && WatchSpellId == spellId && moved <= toleranceMs;
    }

    void Watch(uint32 guid, uint32 spellId, uint32 endMs, uint32 dueMs)
    {
        WatchGuid = guid;
        WatchSpellId = spellId;
        WatchEndMs = endMs;
        WatchDueMs = dueMs;
        WatchSpent = false;
    }

    // One stop per enemy cast, from its due time, none within the backoff after a stop the kick did not follow (a
    // re-armed watch would stop every later own cast during the same enemy cast)
    bool StopDue(uint32 nowMs) const
    {
        return WatchGuid != 0 && !WatchSpent && static_cast<int32>(nowMs - WatchDueMs) >= 0 &&
               (NoStopUntilMs == 0 || static_cast<int32>(nowMs - NoStopUntilMs) >= 0);
    }

    void NoteStop(uint32 nowMs, uint32 targetGuid, uint32 kickSpellId)
    {
        KickStopMs = nowMs;
        KickTargetGuid = targetGuid;
        KickSpellId = kickSpellId;
        WatchSpent = true;
        NoStopUntilMs = 0;
        ++StoppedCasts;
    }

    // The engine's check after the stop did not cast the kick: no stop for the backoff
    void NoteStopUnfollowed(uint32 nowMs)
    {
        KickStopMs = 0;
        NoStopUntilMs = std::max(1u, nowMs + REACTION_STOP_BACKOFF_MS);
        ++UnfollowedStops;
    }
};

// The opponent memory (G0h3-b; DT.12): per bot and match, what the bot saw each enemy cast, bounded to
// OPPONENT_MEMORY_SLOTS enemies (the one seen longest ago gives its slot up). Guid counters only, never a unit.
constexpr uint32 OPPONENT_MEMORY_SLOTS = 8;

struct OpponentMemorySlot
{
    uint32 Guid = 0;         // 0: free
    uint32 SpellId = 0;      // the last cast seen
    uint32 PrevSpellId = 0;  // the cast seen before it
    uint32 ViewEndMs = 0;    // the end of the cast bar in view (0: none)
    uint32 LastSeenMs = 0;
    uint16 CastsSeen = 0;
    uint16 CancelsSeen = 0;
};

struct OpponentMemory
{
    std::array<OpponentMemorySlot, OPPONENT_MEMORY_SLOTS> Slots{};
    uint32 MatchId = 0;  // the match it belongs to (0: none)

    void Clear(uint32 matchId = 0)
    {
        Slots = {};
        MatchId = matchId;
    }

    uint32 IndexOf(uint32 guid) const
    {
        for (uint32 index = 0; guid != 0 && index < OPPONENT_MEMORY_SLOTS; ++index)
            if (Slots[index].Guid == guid)
                return index;

        return OPPONENT_MEMORY_SLOTS;
    }

    OpponentMemorySlot const* Find(uint32 guid) const
    {
        uint32 const index = IndexOf(guid);
        return index < OPPONENT_MEMORY_SLOTS ? &Slots[index] : nullptr;
    }

    // The enemy's slot: its own, a free one, or the one seen longest ago
    OpponentMemorySlot& Touch(uint32 guid, uint32 nowMs)
    {
        uint32 index = IndexOf(guid);
        if (index == OPPONENT_MEMORY_SLOTS)
        {
            index = 0;
            for (uint32 other = 1; other < OPPONENT_MEMORY_SLOTS && Slots[index].Guid != 0; ++other)
                if (Slots[other].Guid == 0 || nowMs - Slots[other].LastSeenMs > nowMs - Slots[index].LastSeenMs)
                    index = other;

            Slots[index] = OpponentMemorySlot();
            Slots[index].Guid = guid;
        }

        Slots[index].LastSeenMs = nowMs;
        return Slots[index];
    }

    static bool SameEnd(uint32 endMs, uint32 otherEndMs, uint32 toleranceMs)
    {
        return (endMs > otherEndMs ? endMs - otherEndMs : otherEndMs - endMs) <= toleranceMs;
    }

    // The cast in view ends: gone more than toleranceMs before its bar's end and not interrupted (a player sees the
    // interrupt) it was cancelled
    static void CloseCast(OpponentMemorySlot& slot, uint32 nowMs, uint32 toleranceMs, bool interrupted)
    {
        if (!interrupted && static_cast<int32>(slot.ViewEndMs - nowMs) > static_cast<int32>(toleranceMs) &&
            slot.CancelsSeen < UINT16_MAX)
            ++slot.CancelsSeen;

        slot.ViewEndMs = 0;
    }

    // A cast in view (its spell and its bar's end): the same cast again changes nothing, a new one first closes the
    // one in view
    void NoteCast(uint32 guid, uint32 spellId, uint32 endMs, uint32 nowMs, uint32 toleranceMs)
    {
        OpponentMemorySlot& slot = Touch(guid, nowMs);
        if (slot.ViewEndMs != 0 && slot.SpellId == spellId && SameEnd(slot.ViewEndMs, endMs, toleranceMs))
            return;

        if (slot.ViewEndMs != 0)
            CloseCast(slot, nowMs, toleranceMs, false);

        slot.PrevSpellId = slot.SpellId;
        slot.SpellId = spellId;
        slot.ViewEndMs = std::max(endMs, 1u);
        if (slot.CastsSeen < UINT16_MAX)
            ++slot.CastsSeen;
    }

    // The enemy shows no cast any more
    void NoteCastGone(uint32 guid, uint32 nowMs, uint32 toleranceMs, bool interrupted)
    {
        uint32 const index = IndexOf(guid);
        if (index == OPPONENT_MEMORY_SLOTS || Slots[index].ViewEndMs == 0)
            return;

        Slots[index].LastSeenMs = nowMs;
        CloseCast(Slots[index], nowMs, toleranceMs, interrupted);
    }

    // The enemy's cast of spellId (its bar ending at endMs) was predicted: it cast the same spell the time before.
    // The cast in view counts as this one, so asking before or after noting it gives the same answer.
    bool Predicts(uint32 guid, uint32 spellId, uint32 endMs, uint32 toleranceMs) const
    {
        OpponentMemorySlot const* slot = Find(guid);
        if (!slot || spellId == 0)
            return false;

        bool const inView =
            slot->ViewEndMs != 0 && slot->SpellId == spellId && SameEnd(slot->ViewEndMs, endMs, toleranceMs);
        return (inView ? slot->PrevSpellId : slot->SpellId) == spellId;
    }

    // Cancels seen per casts seen, from minCasts casts on (0 before)
    float FakeRate(uint32 guid, uint32 minCasts) const
    {
        OpponentMemorySlot const* slot = Find(guid);
        if (!slot || slot->CastsSeen == 0 || slot->CastsSeen < minCasts)
            return 0.0f;

        return static_cast<float>(slot->CancelsSeen) / slot->CastsSeen;
    }
};

#endif
