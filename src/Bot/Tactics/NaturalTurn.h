/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALTURN_H
#define _PLAYERBOT_NATURALTURN_H

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "BotAttributes.h"
#include "Common.h"
#include "NaturalReaction.h"
#include "NaturalTurnConfig.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// Natural facing and turning (G2a), the pure parts. Where a bot snapped to face its target (four call sites), with
// Playerbots.Natural.Facing.Enable, the Natural tactic and a PvP context it turns: a minimum-jerk flick
// (angle(t) = from + d * (10u^3 - 15u^4 + 6u^5), u = t / duration) whose duration grows with the angle and falls with
// Dexterity, ending off the target by an aim error drawn once per turn (log-normal, smaller for a narrow turn), then
// with a chance a correction sub-flick 120-250 ms later that leaves a quarter of the error; low Dexterity sometimes
// turns like a keyboard (a constant rate). The pre-gate stepper writes the server orientation every step and sends
// a heartbeat only on a change of more than PacketMinDeg at most every PacketMinMs (Mode 1), or one facing spline per
// leg of at least SplineMinDeg (Mode 2). Every draw comes from a hash of the bot, the turn's start and its number (no
// RNG state). The game side is in PlayerbotAI.cpp (FaceTarget, StepNaturalPreGate); the numbers are TurnConfig's.

constexpr float TURN_PI = 3.14159265358979323846f;
constexpr float TURN_TWO_PI = 2.0f * TURN_PI;
constexpr float TURN_RAD_PER_DEG = TURN_PI / 180.0f;
constexpr float TURN_DURATION_SIGMA = 0.15f;   // a flick's own log-normal spread around its duration by skill
constexpr uint32 TURN_MAX_LEG_MS = 3000;       // no leg lasts longer (a keyboard turn at a tiny configured rate)
constexpr float TURN_ERROR_MAX_DEG = 45.0f;    // the drawn aim error's cap
constexpr float TURN_ERROR_FULL_DEG = 90.0f;   // a turn this wide or wider carries the whole error, a narrower one less
constexpr float TURN_ERROR_MIN_SHARE = 0.25f;  // the share of the error a tiny turn still carries
constexpr float TURN_SAME_RAD = 0.01f;         // orientations closer than this (about 0.6 degrees) are the same
constexpr uint32 TURN_WAIT_MAX_MS = 2000;      // a cast wait longer than this is not counted (the bot went on)
constexpr uint32 TURN_MODE_SPLINE = 2;
constexpr uint32 TURN_PERCENT = 100;
constexpr uint32 TURN_SELFTEST_DRAWS = 4000;  // turns drawn per skill in RunTurnChecks

constexpr uint64 TURN_ERROR_SALT = 0x452821E638D01377ULL;
constexpr uint64 TURN_SIGN_SALT = 0xBE5466CF34E90C6CULL;
constexpr uint64 TURN_DURATION_SALT = 0xC0AC29B7C97C50DDULL;
constexpr uint64 TURN_KEYBOARD_SALT = 0x3F84D5B5B5470917ULL;
constexpr uint64 TURN_CORRECT_SALT = 0x9216D5D98979FB1BULL;
constexpr uint64 TURN_DELAY_SALT = 0xD1310BA698DFB5ACULL;
constexpr uint64 TURN_CORRECTION_SALT = 0x2FFD72DBD01ADFB7ULL;

// An orientation in [0, 2 pi)
inline float TurnWrap(float angle)
{
    float wrapped = std::fmod(angle, TURN_TWO_PI);
    if (wrapped < 0.0f)
        wrapped += TURN_TWO_PI;

    return wrapped >= TURN_TWO_PI ? 0.0f : wrapped;
}

// The signed shortest turn from one orientation to another, in (-pi, pi]
inline float TurnDelta(float from, float to)
{
    float const delta = TurnWrap(to - from);
    return delta > TURN_PI ? delta - TURN_TWO_PI : delta;
}

// The minimum-jerk position at u in [0, 1]: 0 to 1 with zero speed and acceleration at both ends
inline float MinimumJerk(float u)
{
    float const t = std::clamp(u, 0.0f, 1.0f);
    return t * t * t * (10.0f + t * (6.0f * t - 15.0f));
}

// The instant has come (getMSTime wraps)
inline bool TurnReached(uint32 atMs, uint32 nowMs) { return static_cast<int32>(nowMs - atMs) >= 0; }

// The natural facing's numbers for one bot (ComputeTurnParams, cached by PlayerbotAI::SetPvpSkill)
struct TurnParams
{
    uint32 MsFor180 = 90;   // a 180-degree flick (Dexterity)
    uint32 MinMs = 40;      // the shortest leg
    float ErrorDeg = 2.0f;  // the median aim error (Dexterity)
    float ErrorSigma = 0.4f;
    uint32 CorrectionPct = 35;  // the chance of a correction sub-flick (Dexterity)
    uint32 CorrectionDelayMinMs = 120;
    uint32 CorrectionDelayMaxMs = 250;
    uint32 CorrectionResidualPct = 25;
    uint32 KeyboardPct = 0;  // the chance of a keyboard turn (Dexterity)
    uint32 KeyboardDegPerS = 180;
    // Not by skill: TurnRefreshRules copies them at every turn, so a .reload config reaches logged-in bots
    uint32 Mode = 1;
    uint32 PacketMinDeg = 8;
    uint32 PacketMinMs = 80;
    uint32 SplineMinDeg = 25;
};

inline void TurnRefreshRules(TurnParams& params, TurnConfig const& config)
{
    params.Mode = config.Mode;
    params.PacketMinDeg = config.PacketMinDeg;
    params.PacketMinMs = config.PacketMinMs;
    params.SplineMinDeg = config.SplineMinDeg;
}

// Dexterity (mechanics) carries the flick's speed, the aim error, the correction's chance and the keyboard share;
// skill 0 is full ability (the skill-99 values). With the attribute defaults Dexterity is the skill.
inline TurnParams ComputeTurnParams(uint8 skill, BotAttributeValues const& attributes, float exponent,
                                    TurnConfig const& config)
{
    uint8 const dexterity = attributes.Get(BotAttribute::Dexterity);
    float const position = skill == 0 || dexterity == 0 ? 1.0f : SkillPosition(dexterity);
    auto const lerp = [position, exponent](uint32 atSkill1, uint32 atSkill99)
    { return SkillLerp(static_cast<float>(atSkill1), static_cast<float>(atSkill99), position, exponent); };
    auto const percent = [](float value)
    { return std::min(static_cast<uint32>(std::max(value, 0.0f) + 0.5f), TURN_PERCENT); };

    TurnParams params;
    params.MinMs = std::max(config.MinMs, 1u);
    params.MsFor180 =
        std::max(static_cast<uint32>(std::max(lerp(config.MsFor180AtSkill1, config.MsFor180AtSkill99), 0.0f) + 0.5f),
                 params.MinMs);
    params.ErrorDeg = std::clamp(lerp(config.ErrorDegAtSkill1, config.ErrorDegAtSkill99), 0.0f,
                                 static_cast<float>(TURN_ERROR_MAX_DEG));
    params.ErrorSigma = static_cast<float>(config.ErrorSigmaPct) / static_cast<float>(TURN_PERCENT);
    params.CorrectionPct = percent(lerp(config.CorrectionPctAtSkill1, config.CorrectionPctAtSkill99));
    params.CorrectionDelayMinMs = config.CorrectionDelayMinMs;
    params.CorrectionDelayMaxMs = std::max(config.CorrectionDelayMaxMs, config.CorrectionDelayMinMs);
    params.CorrectionResidualPct = std::min(config.CorrectionResidualPct, TURN_PERCENT);
    params.KeyboardPct = percent(lerp(config.KeyboardPctAtSkill1, config.KeyboardPctAtSkill99));
    params.KeyboardDegPerS = std::max(config.KeyboardDegPerS, 1u);
    TurnRefreshRules(params, config);
    return params;
}

// One leg of a turn (the flick or its correction): from an orientation by a signed delta over DurMs
struct TurnLeg
{
    float From = 0.0f;
    float Delta = 0.0f;
    uint32 StartMs = 0;
    uint32 DurMs = 0;
    bool Keyboard = false;  // a constant rate instead of the minimum-jerk curve
    bool Spline = false;    // Mode 2: sent as one facing spline, no heartbeats

    float Goal() const { return TurnWrap(From + Delta); }
    bool Done(uint32 nowMs) const { return nowMs - StartMs >= DurMs; }

    float At(uint32 nowMs) const
    {
        uint32 const elapsed = nowMs - StartMs;
        if (elapsed >= DurMs)
            return Goal();

        float const u = static_cast<float>(elapsed) / static_cast<float>(DurMs);
        return TurnWrap(From + Delta * (Keyboard ? u : MinimumJerk(u)));
    }
};

// A leg's duration: a flick MinMs + (MsFor180 - MinMs) * degrees / 180 with its own spread (z: a standard normal
// draw), a keyboard turn at KeyboardDegPerS; at least MinMs, at most TURN_MAX_LEG_MS
inline uint32 TurnLegMs(TurnParams const& params, float absDeltaRad, bool keyboard, double z)
{
    float const degrees = absDeltaRad / TURN_RAD_PER_DEG;
    float ms = 0.0f;
    if (keyboard)
        ms = degrees * 1000.0f / static_cast<float>(params.KeyboardDegPerS);
    else
        ms =
            (static_cast<float>(params.MinMs) + static_cast<float>(params.MsFor180 - params.MinMs) * degrees / 180.0f) *
            std::exp(TURN_DURATION_SIGMA * static_cast<float>(z));

    return std::clamp(static_cast<uint32>(std::max(ms, 0.0f) + 0.5f), params.MinMs, TURN_MAX_LEG_MS);
}

inline TurnLeg PlanTurnLeg(TurnParams const& params, float fromRad, float goalRad, uint32 nowMs, bool keyboard,
                           uint64 hash)
{
    TurnLeg leg;
    leg.From = TurnWrap(fromRad);
    leg.Delta = TurnDelta(leg.From, goalRad);
    leg.StartMs = nowMs;
    leg.Keyboard = keyboard;
    leg.DurMs =
        TurnLegMs(params, std::fabs(leg.Delta), keyboard, Probit(UnitFromHash(SplitMix64(hash ^ TURN_DURATION_SALT))));
    leg.Spline = params.Mode == TURN_MODE_SPLINE &&
                 std::fabs(leg.Delta) >= static_cast<float>(params.SplineMinDeg) * TURN_RAD_PER_DEG;
    return leg;
}

// A turn's aim error in signed radians: log-normal around ErrorDeg, capped, a narrower turn carrying less of it
inline float TurnErrorRad(TurnParams const& params, float absTurnRad, uint64 hash)
{
    double const z = Probit(UnitFromHash(SplitMix64(hash ^ TURN_ERROR_SALT)));
    float const drawn =
        std::min(params.ErrorDeg * std::exp(params.ErrorSigma * static_cast<float>(z)), TURN_ERROR_MAX_DEG);
    float const share = std::clamp(absTurnRad / (TURN_ERROR_FULL_DEG * TURN_RAD_PER_DEG), TURN_ERROR_MIN_SHARE, 1.0f);
    float const sign = UnitFromHash(SplitMix64(hash ^ TURN_SIGN_SALT)) < 0.5 ? -1.0f : 1.0f;
    return sign * drawn * share * TURN_RAD_PER_DEG;
}

// A turn as FaceTarget plans it: the flick towards the target plus the error, and whether and when it is corrected
struct TurnPlan
{
    TurnLeg Leg;
    float ErrorRad = 0.0f;
    bool Correct = false;
    uint32 CorrectDelayMs = 0;  // after the flick's end
    uint64 Hash = 0;
};

inline uint64 TurnHash(uint64 botGuid, uint32 nowMs, uint32 turn)
{
    return SplitMix64(SplitMix64((botGuid << 32) ^ nowMs) ^ turn);
}

inline TurnPlan PlanTurn(TurnParams const& params, float fromRad, float targetRad, uint32 nowMs, uint64 hash)
{
    TurnPlan plan;
    plan.Hash = hash;
    plan.ErrorRad = TurnErrorRad(params, std::fabs(TurnDelta(fromRad, targetRad)), hash);
    bool const keyboard =
        UnitFromHash(SplitMix64(hash ^ TURN_KEYBOARD_SALT)) * TURN_PERCENT < static_cast<double>(params.KeyboardPct);
    plan.Leg = PlanTurnLeg(params, fromRad, targetRad + plan.ErrorRad, nowMs, keyboard, hash);
    plan.Correct =
        UnitFromHash(SplitMix64(hash ^ TURN_CORRECT_SALT)) * TURN_PERCENT < static_cast<double>(params.CorrectionPct);
    uint32 const span = params.CorrectionDelayMaxMs - params.CorrectionDelayMinMs;
    plan.CorrectDelayMs =
        params.CorrectionDelayMinMs +
        std::min(static_cast<uint32>(UnitFromHash(SplitMix64(hash ^ TURN_DELAY_SALT)) * (span + 1)), span);
    return plan;
}

// A heartbeat goes out on a change of more than PacketMinDeg since the last one, at most every PacketMinMs
inline bool TurnPacketDue(TurnParams const& params, float sentRad, float nowRad, uint32 sentMs, uint32 nowMs)
{
    return std::fabs(TurnDelta(sentRad, nowRad)) > static_cast<float>(params.PacketMinDeg) * TURN_RAD_PER_DEG &&
           nowMs - sentMs >= params.PacketMinMs;
}

// A cast that waited for the arc goes ahead: the wait to count (0: none, or too old to be this cast's)
inline uint32 TurnCastWaitMs(uint32 sinceMs, uint32 nowMs)
{
    if (sinceMs == 0)
        return 0;

    uint32 const waitMs = nowMs - sinceMs;
    return waitMs <= TURN_WAIT_MAX_MS ? waitMs : 0;
}

enum class TurnPhase : uint8
{
    Idle,
    Flick,       // the turn's flick
    Hold,        // the flick ended off by its error; a correction is due at CorrectAtMs
    Correction,  // the correction sub-flick
};

// What one pre-gate step does
struct TurnStepOut
{
    bool Write = false;  // write Orientation as the server orientation
    float Orientation = 0.0f;
    bool Packet = false;  // send a heartbeat (the orientation written last)
    bool Spline = false;  // Mode 2: launch a facing spline to SplineRad (a correction leg)
    float SplineRad = 0.0f;
    bool Abandoned = false;  // someone else turned the bot: the turn ended
};

// The per-bot state (PlayerbotAI::GetTurnState; the bot's own update, tactics show on the world thread)
struct TurnBotState
{
    TurnParams Params;
    TurnPhase Phase = TurnPhase::Idle;
    TurnLeg Leg;
    uint64 TargetGuid = 0;  // the object faced (raw guid), looked up again for the correction
    uint64 Hash = 0;
    float ErrorRad = 0.0f;
    bool Correct = false;
    uint32 CorrectAtMs = 0;
    float WrittenRad = 0.0f;  // the orientation the stepper wrote last
    bool Written = false;
    float SentRad = 0.0f;  // the orientation the clients saw last
    uint32 SentMs = 0;
    bool FinalPending = false;    // a leg's end not yet sent (the packet rate held it back)
    uint32 CastWaitSinceMs = 0;   // a cast waits for the arc since (0: none)
    uint32 InSightCheckedMs = 0;  // OutsidePvpInSight's cached answer
    bool InSight = false;
    // What the bot did (tactics show)
    uint32 Turns = 0;
    uint32 KeyboardTurns = 0;
    uint32 Corrections = 0;
    uint32 Packets = 0;
    uint32 Splines = 0;
    uint32 Snaps = 0;
    uint32 Abandons = 0;
    uint32 CastWaits = 0;
    uint32 CastWaitMs = 0;
    uint32 LastTurnMs = 0;
    uint32 LastTurnDeg = 0;
    uint32 LastTurnDurMs = 0;
    float LastErrorDeg = 0.0f;

    bool Active() const { return Phase != TurnPhase::Idle || FinalPending; }
    bool Turning() const { return Phase == TurnPhase::Flick || Phase == TurnPhase::Correction; }

    void Stop()
    {
        Phase = TurnPhase::Idle;
        Correct = false;
        FinalPending = false;
        Written = false;
    }

    // A new turn (FaceTarget). A spline leg (Mode 2) is what the clients see from now on.
    void Begin(TurnPlan const& plan, uint64 targetGuid, uint32 nowMs)
    {
        Phase = TurnPhase::Flick;
        Leg = plan.Leg;
        TargetGuid = targetGuid;
        Hash = plan.Hash;
        ErrorRad = plan.ErrorRad;
        Correct = plan.Correct;
        CorrectAtMs = nowMs + plan.Leg.DurMs + plan.CorrectDelayMs;
        Written = false;
        FinalPending = false;
        SentRad = plan.Leg.From;
        ++Turns;
        KeyboardTurns += plan.Leg.Keyboard ? 1 : 0;
        LastTurnMs = std::max(nowMs, 1u);
        LastTurnDeg = static_cast<uint32>(std::fabs(plan.Leg.Delta) / TURN_RAD_PER_DEG + 0.5f);
        LastTurnDurMs = plan.Leg.DurMs;
        LastErrorDeg = plan.ErrorRad / TURN_RAD_PER_DEG;
        if (plan.Leg.Spline)
            NoteSpline(plan.Leg.Goal(), nowMs);
    }

    void NoteSpline(float goalRad, uint32 nowMs)
    {
        SentRad = goalRad;
        SentMs = nowMs;
        ++Splines;
    }
};

// The correction is due and needs the target's orientation from the bot now
inline bool TurnNeedsTarget(TurnBotState const& state, uint32 nowMs)
{
    return state.Phase == TurnPhase::Hold && state.Correct && TurnReached(state.CorrectAtMs, nowMs);
}

// One pre-gate step: the orientation to write and whether a heartbeat (or a correction's spline) goes out. actualRad
// is the bot's orientation now: one other than the last written means someone else turned it (a snap, a path, a
// script) and the turn ends. targetRad (haveTarget) is read only when TurnNeedsTarget said so; without it the
// correction is dropped.
inline TurnStepOut StepTurn(TurnBotState& state, uint32 nowMs, float actualRad, bool haveTarget, float targetRad)
{
    TurnStepOut out;
    TurnParams const& params = state.Params;
    bool const splineEnd = state.Leg.Spline && std::fabs(TurnDelta(state.Leg.Goal(), actualRad)) <= TURN_SAME_RAD;
    if (state.Written && std::fabs(TurnDelta(state.WrittenRad, actualRad)) > TURN_SAME_RAD && !splineEnd)
    {
        state.Stop();
        ++state.Abandons;
        out.Abandoned = true;
        return out;
    }

    if (TurnNeedsTarget(state, nowMs))
    {
        state.Correct = false;
        state.Phase = TurnPhase::Idle;
        if (haveTarget)
        {
            float const residual =
                state.ErrorRad * static_cast<float>(params.CorrectionResidualPct) / static_cast<float>(TURN_PERCENT);
            state.Leg = PlanTurnLeg(params, state.Written ? state.WrittenRad : actualRad, targetRad + residual, nowMs,
                                    state.Leg.Keyboard, SplitMix64(state.Hash ^ TURN_CORRECTION_SALT));
            state.ErrorRad = residual;
            state.Phase = TurnPhase::Correction;
            ++state.Corrections;
            if (state.Leg.Spline)
            {
                out.Spline = true;
                out.SplineRad = state.Leg.Goal();
                state.NoteSpline(out.SplineRad, nowMs);
            }
        }
    }

    if (state.Turning())
    {
        float const angle = state.Leg.At(nowMs);
        out.Write = true;
        out.Orientation = angle;
        state.WrittenRad = angle;
        state.Written = true;
        bool const done = state.Leg.Done(nowMs);
        if (done)
            state.Phase = state.Correct ? TurnPhase::Hold : TurnPhase::Idle;

        if (state.Leg.Spline)
            return out;

        if (TurnPacketDue(params, state.SentRad, angle, state.SentMs, nowMs))
        {
            out.Packet = true;
            state.SentRad = angle;
            state.SentMs = nowMs;
            ++state.Packets;
            state.FinalPending = false;
        }
        else if (done)
            state.FinalPending = std::fabs(TurnDelta(state.SentRad, angle)) > TURN_SAME_RAD;

        return out;
    }

    // The leg's end goes out as soon as the packet rate allows
    if (state.FinalPending && nowMs - state.SentMs >= params.PacketMinMs)
    {
        out.Packet = true;
        state.SentRad = state.WrittenRad;
        state.SentMs = nowMs;
        ++state.Packets;
        state.FinalPending = false;
    }

    return out;
}

// tactics selftest turn (also the offline harness): the curve's shape, durations by skill, the error spread, the
// correction and keyboard shares, the packet rule and whole turns stepped at map-tick rates
template <typename Check>
void RunTurnChecks(Check&& check)
{
    TurnConfig const config;
    auto const attributesAt = [](uint8 ability)
    {
        BotAttributeValues values;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = index >= BOT_ABILITY_COUNT ? 50 : ability;

        return values;
    };
    auto const within = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };
    float const degree = TURN_RAD_PER_DEG;

    TurnParams const p99 = ComputeTurnParams(99, attributesAt(99), 1.0f, config);
    TurnParams const p50 = ComputeTurnParams(50, attributesAt(50), 1.0f, config);
    TurnParams const p1 = ComputeTurnParams(1, attributesAt(1), 1.0f, config);
    TurnParams const p0 = ComputeTurnParams(0, attributesAt(0), 1.0f, config);

    // The curve
    check(MinimumJerk(0.0f) == 0.0f && MinimumJerk(1.0f) == 1.0f && within(MinimumJerk(0.5f), 0.5f, 1e-6f),
          "the minimum-jerk curve runs from 0 to 1 through 0.5 at half time");
    bool monotonic = true;
    for (uint32 step = 1; step <= 100; ++step)
        monotonic = monotonic && MinimumJerk(step / 100.0f) >= MinimumJerk((step - 1) / 100.0f);

    check(monotonic, "the curve never turns back");
    float const peak = (MinimumJerk(0.501f) - MinimumJerk(0.499f)) / 0.002f;
    float const startSpeed = MinimumJerk(0.02f) / 0.02f;
    float const endSpeed = (1.0f - MinimumJerk(0.98f)) / 0.02f;
    check(within(peak, 1.875f, 0.01f), "the peak speed is 1.875 times the mean, at half time (a flick)");
    check(startSpeed < 0.01f && endSpeed < 0.01f, "the turn starts and stops at zero speed");
    check(within(TurnDelta(350.0f * degree, 10.0f * degree), 20.0f * degree, 1e-4f) &&
              within(TurnDelta(10.0f * degree, 350.0f * degree), -20.0f * degree, 1e-4f),
          "a turn takes the shortest way across north");
    check(TurnWrap(-0.5f * degree) < TURN_TWO_PI && TurnWrap(-0.5f * degree) > 359.0f * degree &&
              within(TurnWrap(TURN_TWO_PI + degree), degree, 1e-4f),
          "orientations wrap into [0, 360)");

    // Durations by skill
    check(p99.MsFor180 == config.MsFor180AtSkill99 && p1.MsFor180 == config.MsFor180AtSkill1,
          "a 180-degree flick takes 450 ms at skill 1 and 90 ms at 99");
    check(p0.MsFor180 == p99.MsFor180 && p0.ErrorDeg == p99.ErrorDeg && p0.KeyboardPct == p99.KeyboardPct,
          "skill 0 is full ability");
    check(p50.MsFor180 < p1.MsFor180 && p50.MsFor180 > p99.MsFor180, "the flick quickens with skill");
    check(TurnLegMs(p99, TURN_PI, false, 0.0) == 90 && TurnLegMs(p1, TURN_PI, false, 0.0) == 450 &&
              TurnLegMs(p1, 0.0f, false, 0.0) == config.MinMs &&
              TurnLegMs(p1, TURN_PI / 2.0f, false, 0.0) < TurnLegMs(p1, TURN_PI, false, 0.0),
          "the duration grows with the angle from MinMs");
    check(TurnLegMs(p1, TURN_PI, true, 0.0) == 1000 && TurnLegMs(p1, TURN_PI / 2.0f, true, 2.0) == 500,
          "a keyboard turn runs at 180 degrees per second, without spread");

    // Draws over many turns: the error, the flick's spread, the correction and keyboard shares
    auto const median = [](std::vector<float> values)
    {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    struct Drawn
    {
        std::vector<float> Error;
        std::vector<float> FlickMs;
        uint32 Positive = 0;
        uint32 Corrected = 0;
        uint32 Keyboard = 0;
        uint32 DelayOut = 0;
        float MaxError = 0.0f;
    };
    auto const draw = [&](TurnParams const& params, float turnRad)
    {
        Drawn drawn;
        for (uint32 index = 0; index < TURN_SELFTEST_DRAWS; ++index)
        {
            uint64 const hash = TurnHash(1000 + index % 97, 50000 + index * 37, index);
            TurnPlan const plan = PlanTurn(params, 0.0f, turnRad, 50000, hash);
            float const error = std::fabs(plan.ErrorRad) / TURN_RAD_PER_DEG;
            drawn.Error.push_back(error);
            drawn.MaxError = std::max(drawn.MaxError, error);
            drawn.Positive += plan.ErrorRad > 0.0f ? 1 : 0;
            drawn.Corrected += plan.Correct ? 1 : 0;
            drawn.Keyboard += plan.Leg.Keyboard ? 1 : 0;
            drawn.DelayOut +=
                plan.CorrectDelayMs < params.CorrectionDelayMinMs || plan.CorrectDelayMs > params.CorrectionDelayMaxMs
                    ? 1
                    : 0;
            if (!plan.Leg.Keyboard)
                drawn.FlickMs.push_back(static_cast<float>(plan.Leg.DurMs));
        }

        return drawn;
    };
    auto const share = [](uint32 count) { return count * 100.0f / TURN_SELFTEST_DRAWS; };
    Drawn const d1 = draw(p1, TURN_PI * 0.75f);
    Drawn const d50 = draw(p50, TURN_PI * 0.75f);
    Drawn const d99 = draw(p99, TURN_PI * 0.75f);
    Drawn const narrow = draw(p1, 20.0f * degree);
    check(within(median(d1.Error), config.ErrorDegAtSkill1, 1.0f) &&
              within(median(d99.Error), config.ErrorDegAtSkill99, 0.2f),
          "the median aim error is 14 degrees at skill 1 and 2 at 99");
    check(median(d1.Error) > median(d50.Error) && median(d50.Error) > median(d99.Error), "the error falls with skill");
    std::vector<float> sortedLow = d1.Error;
    std::sort(sortedLow.begin(), sortedLow.end());
    float const lowP10 = sortedLow[TURN_SELFTEST_DRAWS / 10];
    float const lowP90 = sortedLow[TURN_SELFTEST_DRAWS * 9 / 10];
    check(lowP10 > 5.0f && lowP10 < 10.0f && lowP90 > 20.0f && lowP90 < 30.0f,
          "the error spreads log-normally (skill 1: p10 5-10, p90 20-30 degrees)");
    check(d1.MaxError <= TURN_ERROR_MAX_DEG + 0.01f, "the error is capped at 45 degrees");
    check(median(narrow.Error) < median(d1.Error) * 0.5f, "a narrow turn carries less error");
    check(within(share(d1.Positive), 50.0f, 3.0f) && within(share(d99.Positive), 50.0f, 3.0f),
          "the error falls on either side evenly");
    check(within(share(d1.Corrected), static_cast<float>(config.CorrectionPctAtSkill1), 3.0f) &&
              within(share(d99.Corrected), static_cast<float>(config.CorrectionPctAtSkill99), 3.0f),
          "a correction follows 70 % of the turns at skill 1 and 35 % at 99");
    check(d1.DelayOut == 0 && d99.DelayOut == 0, "the correction comes 120-250 ms after the flick");
    check(within(share(d1.Keyboard), static_cast<float>(config.KeyboardPctAtSkill1), 3.0f) && d99.Keyboard == 0,
          "35 % keyboard turns at skill 1, none at 99");
    float const want1 = config.MinMs + (p1.MsFor180 - config.MinMs) * 0.75f;
    float const want99 = config.MinMs + (p99.MsFor180 - config.MinMs) * 0.75f;
    check(within(median(d1.FlickMs), want1, want1 * 0.03f) && within(median(d99.FlickMs), want99, want99 * 0.05f),
          "the flick's median duration is the configured one");
    std::vector<float> sortedMs = d1.FlickMs;
    std::sort(sortedMs.begin(), sortedMs.end());
    float const msP10 = sortedMs[sortedMs.size() / 10];
    float const msP90 = sortedMs[sortedMs.size() * 9 / 10];
    check(msP10 < want1 * 0.9f && msP90 > want1 * 1.1f && msP10 > want1 * 0.7f && msP90 < want1 * 1.4f,
          "the flick's duration spreads (p10 and p90 within 10-30 % of the median)");

    // The packet rule
    check(TurnPacketDue(p99, 0.0f, 9.0f * degree, 1000, 1080) && !TurnPacketDue(p99, 0.0f, 7.0f * degree, 1000, 2000) &&
              !TurnPacketDue(p99, 0.0f, 90.0f * degree, 1000, 1079),
          "a heartbeat needs more than 8 degrees and 80 ms");
    check(TurnCastWaitMs(0, 5000) == 0 && TurnCastWaitMs(4000, 4300) == 300 && TurnCastWaitMs(1000, 4000) == 0,
          "a cast wait counts up to 2 s");

    // Whole turns stepped at a map tick: plan, step, hold, correct, send
    struct Run
    {
        uint32 Steps = 0;
        uint32 Packets = 0;
        uint32 MinGapMs = UINT32_MAX;
        float MaxStepDeg = 0.0f;
        float FirstStepDeg = 0.0f;
        float LastStepDeg = 0.0f;
        float PeakStepDeg = 0.0f;
        float FinalOffDeg = 0.0f;  // the end against the target
        float SentOffDeg = 0.0f;   // the last heartbeat against the end
        uint32 Corrections = 0;
        uint32 Splines = 0;
        bool Ended = false;
    };
    auto const run =
        [&](TurnParams params, float targetRad, uint32 tickMs, uint32 startMs, uint64 hash, bool keyboard, bool correct)
    {
        Run result;
        TurnBotState state;
        state.Params = params;
        TurnPlan plan = PlanTurn(params, 0.0f, targetRad, startMs, hash);
        plan.Leg = PlanTurnLeg(params, 0.0f, targetRad + plan.ErrorRad, startMs, keyboard, hash);
        plan.Correct = correct;
        state.Begin(plan, 1, startMs);
        float orientation = 0.0f;
        uint32 lastPacketMs = 0;
        bool sent = false;
        uint32 nowMs = startMs;
        for (uint32 step = 0; step < 400 && state.Active(); ++step)
        {
            nowMs += tickMs;
            TurnStepOut const out = StepTurn(state, nowMs, orientation, TurnNeedsTarget(state, nowMs), targetRad);
            if (out.Write)
            {
                float const moved = std::fabs(TurnDelta(orientation, out.Orientation)) / TURN_RAD_PER_DEG;
                if (result.Steps == 0)
                    result.FirstStepDeg = moved;

                result.MaxStepDeg = std::max(result.MaxStepDeg, moved);
                if (moved > 0.0f)
                    result.LastStepDeg = moved;

                result.PeakStepDeg = std::max(result.PeakStepDeg, moved);
                orientation = out.Orientation;
                ++result.Steps;
            }

            if (out.Packet)
            {
                if (sent)
                    result.MinGapMs = std::min(result.MinGapMs, nowMs - lastPacketMs);

                lastPacketMs = nowMs;
                sent = true;
                ++result.Packets;
            }
        }

        result.Ended = !state.Active();
        result.FinalOffDeg = std::fabs(TurnDelta(targetRad, orientation)) / TURN_RAD_PER_DEG;
        result.SentOffDeg = std::fabs(TurnDelta(state.SentRad, orientation)) / TURN_RAD_PER_DEG;
        result.Corrections = state.Corrections;
        result.Splines = state.Splines;
        return result;
    };

    Run const slow = run(p1, TURN_PI * 0.999f, 10, 100000, 7, false, false);
    check(slow.Ended && slow.PeakStepDeg > 2.5f * slow.FirstStepDeg && slow.PeakStepDeg > 2.5f * slow.LastStepDeg,
          "stepped at 10 ms a turn is a flick: slow at both ends, fast in the middle");
    check(slow.MaxStepDeg < 45.0f, "skill 1 never turns more than 45 degrees in a 10 ms step");
    check(slow.Packets >= 3 && slow.Packets <= slow.Steps / 8 + 2 && slow.MinGapMs >= p1.PacketMinMs,
          "a 180-degree flick sends a few heartbeats, at least 80 ms apart");
    check(slow.SentOffDeg < 0.1f, "the clients get the turn's end");
    Run const fast = run(p99, TURN_PI * 0.999f, 50, 100000, 7, false, false);
    check(fast.Ended && fast.Steps >= 2 && fast.MaxStepDeg < 175.0f,
          "even skill 99 at a 50 ms tick needs two steps for 180 degrees (no instant 180)");
    Run const corrected = run(p1, TURN_PI / 2.0f, 20, 100000, 11, false, true);
    check(corrected.Ended && corrected.Corrections == 1, "a planned correction runs once and the turn ends");
    TurnPlan const planned = PlanTurn(p1, 0.0f, TURN_PI / 2.0f, 100000, 11);
    check(within(corrected.FinalOffDeg,
                 std::fabs(planned.ErrorRad) / TURN_RAD_PER_DEG * config.CorrectionResidualPct / 100.0f, 0.05f),
          "the correction leaves a quarter of the error");
    Run const keys = run(p1, TURN_PI / 2.0f, 50, 100000, 13, true, false);
    check(keys.Ended && within(keys.MaxStepDeg, 9.0f, 0.05f) && keys.Steps >= 5,
          "a keyboard turn moves at a constant 9 degrees per 50 ms");
    Run const wrapped = run(p50, TURN_PI / 2.0f, 20, UINT32_MAX - 100, 17, false, false);
    check(wrapped.Ended && wrapped.FinalOffDeg <= TURN_ERROR_MAX_DEG && wrapped.Steps >= 3,
          "a turn survives the clock's wrap");

    // Mode 2: one spline per wide leg, no heartbeats for it
    TurnParams spline = p1;
    spline.Mode = TURN_MODE_SPLINE;
    Run const splined = run(spline, TURN_PI / 2.0f, 20, 100000, 19, false, true);
    check(splined.Ended && splined.Splines == 1 && splined.Packets <= 2,
          "Mode 2 sends a spline for the flick and at most two heartbeats for the small correction");

    // Someone else turns the bot: the turn ends
    {
        TurnBotState state;
        state.Params = p1;
        state.Begin(PlanTurn(p1, 0.0f, TURN_PI, 1000, 23), 1, 1000);
        TurnStepOut const first = StepTurn(state, 1020, 0.0f, false, 0.0f);
        TurnStepOut const second = StepTurn(state, 1040, first.Orientation + 0.5f, false, 0.0f);
        check(first.Write && second.Abandoned && !state.Active(), "a foreign turn ends the bot's own");
    }
    // The correction's target gone: it is dropped
    {
        TurnBotState state;
        state.Params = p1;
        TurnPlan plan = PlanTurn(p1, 0.0f, TURN_PI / 2.0f, 1000, 29);
        plan.Correct = true;
        state.Begin(plan, 1, 1000);
        float orientation = 0.0f;
        uint32 nowMs = 1000;
        while (state.Active() && nowMs < 5000)
        {
            nowMs += 20;
            TurnStepOut const out = StepTurn(state, nowMs, orientation, false, 0.0f);
            if (out.Write)
                orientation = out.Orientation;
        }

        check(!state.Active() && state.Corrections == 0, "without its target the correction is dropped");
    }
}

// The game side (PlayerbotAI.cpp): whether the bot turns naturally now (the option, the Natural tactic here, a PvP
// context of Natural.Facing.Contexts, or out of PvP and out of instances in a real player's sight with
// OutsidePvpInSight, cached for InSightCacheMs). The bot's own update, or the world thread while the maps wait.
class PlayerbotAI;
bool NaturalFacingHere(PlayerbotAI* botAI, uint32 nowMs);

#endif
