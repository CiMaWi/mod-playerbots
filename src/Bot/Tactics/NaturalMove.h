/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALMOVE_H
#define _PLAYERBOT_NATURALMOVE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

#include "BotAttributes.h"
#include "Common.h"
#include "NaturalMoveConfig.h"
#include "NaturalReaction.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// Natural chasing and tracking (G2b), the pure parts. Where ReachCombatTo ran to the target's exact position, with
// Playerbots.Natural.Track.Enable, the Natural tactic and a PvP context the chaser runs to a lagged, predicted point:
// every call, and every check of the bot in a fight (TrackFeed), puts the target's true position into a ring (one
// sample per gap, RingSamples of them), so a chase after a pause has its history; the chaser sees
// only the target as it was lag ms ago (log-normal around a median that falls with Speed) and predicts where it is
// now by Foresight: below VelocityFromSkill it runs to where it last saw the target; from there it extrapolates the
// velocity with a gain (a share of the true velocity, drawn per call); from TurnRateFromSkill it bends the
// extrapolation by the turn rate it read; from PatternFromSkill it reads jukes over HistoryMs and, when they repeat,
// expects the next one on time and runs the leg before as the next. A heading change of JukeDeg is a juke no level
// predicts. The predicted point is at most MaxLeadYards from the seen one; a target that moved less than MinMoveYards
// in the half second before the seen instant is standing; a chaser out of its reach that neither closed on the target
// nor ran for StuckMs aims at the true position for a while. Every draw comes from a hash of the bot, the instant and
// its call number (no RNG state). The game side is in MovementActions.cpp (ReachCombatTo) and PlayerbotAI.cpp
// (NaturalTrackHere, the params by SetPvpSkill); the numbers are TrackConfig's.

constexpr float TRACK_PI = 3.14159265358979323846f;
constexpr float TRACK_TWO_PI = 2.0f * TRACK_PI;
constexpr float TRACK_RAD_PER_DEG = TRACK_PI / 180.0f;
constexpr uint32 TRACK_PERCENT = 100;
constexpr uint32 TRACK_MAX_LAG_MS = 3000;             // no lag draw beyond this
constexpr float TRACK_MAX_GAIN = 1.25f;               // the gain draw's cap (a share of the true velocity)
constexpr uint32 TRACK_VELOCITY_WINDOW_MS = 250;      // the velocity is read over this much of what the chaser saw
constexpr uint32 TRACK_MIN_LEG_MS = 60;               // a pattern's current leg shorter than this: the window instead
constexpr uint32 TRACK_STAND_WINDOW_MS = 500;         // moved less than MinMoveYards in this before the seen instant
constexpr uint32 TRACK_STALE_MS = 1000;               // a ring whose newest sample is older than this starts again
constexpr float TRACK_LAG_SPAN_FACTOR = 1.5f;         // the ring covers this many median lags (the draw's tail)
constexpr float TRACK_MAX_TURN_RAD_PER_MS = 0.0035f;  // about 200 degrees per second: a faster turn rate is capped
constexpr uint32 TRACK_MIN_JUKES = 2;                 // jukes in the history that make a pattern
constexpr float TRACK_MIN_SEGMENT_YARDS = 0.05f;      // a segment between two samples shorter than this has no heading
constexpr float TRACK_CLOSE_YARDS = 1.0f;             // closing this much on the target is progress (StuckMs)
constexpr float TRACK_ESCAPE_YARDS = 8.0f;            // the target this much farther than the best: a new chase
constexpr float TRACK_MOVED_YARDS = 2.0f;             // the chaser running this far itself is progress (StuckMs)
constexpr uint32 TRACK_TRUE_AIM_MS = 1500;            // a stuck chaser aims at the true position this long
constexpr float TRACK_SAME_YARDS = 0.5f;              // a predicted point this close to the seen one needs no checks
constexpr uint32 TRACK_SELFTEST_BOTS = 24;            // simulated chasers per skill and run in RunTrackChecks
constexpr uint32 TRACK_SELFTEST_DRAWS = 4000;         // lag and gain draws per skill in RunTrackChecks
constexpr float TRACK_SELFTEST_RUN = 0.007f;          // the simulated target's run speed (yards per ms)

constexpr uint64 TRACK_LAG_SALT = 0x6A09E667F3BCC908ULL;
constexpr uint64 TRACK_GAIN_SALT = 0xBB67AE8584CAA73BULL;

// The instant has come (getMSTime wraps)
inline bool TrackReached(uint32 atMs, uint32 nowMs) { return static_cast<int32>(nowMs - atMs) >= 0; }

// The signed shortest turn from one heading to another, in (-pi, pi]
inline float TrackTurn(float from, float to)
{
    float delta = std::fmod(to - from, TRACK_TWO_PI);
    if (delta <= -TRACK_PI)
        delta += TRACK_TWO_PI;
    else if (delta > TRACK_PI)
        delta -= TRACK_TWO_PI;

    return delta;
}

// What the chaser reads, by Foresight (each level adds to the one below)
enum class TrackLevel : uint8
{
    LastSeen,  // runs to where it last saw the target
    Velocity,  // extrapolates the velocity
    TurnRate,  // bends the extrapolation by the turn rate
    Pattern,   // reads repeated jukes over its history
};

inline char const* TrackLevelName(TrackLevel level)
{
    switch (level)
    {
        case TrackLevel::LastSeen:
            return "last seen";
        case TrackLevel::Velocity:
            return "velocity";
        case TrackLevel::TurnRate:
            return "turn rate";
        case TrackLevel::Pattern:
            return "pattern";
    }

    return "?";
}

// The natural tracking's numbers for one bot (ComputeTrackParams, cached by PlayerbotAI::SetPvpSkill)
struct TrackParams
{
    uint32 LagMs = 110;  // the median lag (Speed)
    float LagSigma = 0.25f;
    bool Velocity = true;  // Foresight >= VelocityFromSkill
    bool TurnRate = true;  // and >= TurnRateFromSkill
    bool Pattern = true;   // and >= PatternFromSkill
    float Gain = 0.9f;     // the median share of the true velocity (Foresight)
    float GainSigma = 0.2f;
    uint32 HistoryMs = 1500;  // the history a pattern is read from (Foresight)

    TrackLevel Level() const
    {
        if (!Velocity)
            return TrackLevel::LastSeen;

        if (Pattern)
            return TrackLevel::Pattern;

        return TurnRate ? TrackLevel::TurnRate : TrackLevel::Velocity;
    }
};

// Speed carries the lag, Foresight the predictors, their gain and the history; skill 0 is full ability (the skill-99
// values). With the attribute defaults both are the skill.
inline TrackParams ComputeTrackParams(uint8 skill, BotAttributeValues const& attributes, float exponent,
                                      TrackConfig const& config)
{
    uint8 const speed = attributes.Get(BotAttribute::Speed);
    uint8 const foresight = attributes.Get(BotAttribute::Foresight);
    bool const full = skill == 0;
    float const speedPosition = full || speed == 0 ? 1.0f : SkillPosition(speed);
    float const foresightPosition = full || foresight == 0 ? 1.0f : SkillPosition(foresight);
    uint32 const ability = full || foresight == 0 ? 99u : foresight;
    auto const lerp = [exponent](uint32 atSkill1, uint32 atSkill99, float position)
    { return SkillLerp(static_cast<float>(atSkill1), static_cast<float>(atSkill99), position, exponent); };

    TrackParams params;
    params.LagMs = std::min(
        static_cast<uint32>(std::max(lerp(config.LagMsAtSkill1, config.LagMsAtSkill99, speedPosition), 0.0f) + 0.5f),
        TRACK_MAX_LAG_MS);
    params.LagSigma = static_cast<float>(config.LagSigmaPct) / static_cast<float>(TRACK_PERCENT);
    params.Velocity = ability >= config.VelocityFromSkill;
    params.TurnRate = params.Velocity && ability >= config.TurnRateFromSkill;
    params.Pattern = params.Velocity && ability >= config.PatternFromSkill;
    params.Gain = std::clamp(lerp(config.GainPctAtSkill1, config.GainPctAtSkill99, foresightPosition), 0.0f,
                             static_cast<float>(TRACK_PERCENT) * TRACK_MAX_GAIN) /
                  static_cast<float>(TRACK_PERCENT);
    params.GainSigma = static_cast<float>(config.GainSigmaPct) / static_cast<float>(TRACK_PERCENT);
    params.HistoryMs = static_cast<uint32>(
        std::max(lerp(config.HistoryMsAtSkill1, config.HistoryMsAtSkill99, foresightPosition), 0.0f) + 0.5f);
    return params;
}

// One observed position of the target
struct TrackSample
{
    uint32 Ms = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct TrackPoint
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

inline float TrackDistance2d(TrackPoint const& a, TrackPoint const& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// The target's past positions (newest first by At); holds the maximum, uses RingSamples of it
struct TrackRing
{
    std::array<TrackSample, MaxTrackRingSamples> Samples{};
    uint32 Count = 0;
    uint32 Newest = 0;

    void Clear()
    {
        Count = 0;
        Newest = 0;
    }

    // back 0: the newest sample, Count - 1: the oldest
    TrackSample const& At(uint32 back) const
    {
        return Samples[(Newest + MaxTrackRingSamples - back % MaxTrackRingSamples) % MaxTrackRingSamples];
    }

    void Push(TrackSample const& sample, uint32 capacity)
    {
        Newest = Count == 0 ? 0 : (Newest + 1) % MaxTrackRingSamples;
        Samples[Newest] = sample;
        Count = std::min(Count + 1, std::clamp(capacity, MinTrackRingSamples, MaxTrackRingSamples));
    }
};

// The target's position ageMs before nowMs, between the two samples around it; clamped to the newest sample (a
// younger age) and to the oldest (an older age: covered false). False on an empty ring.
inline bool TrackPositionAt(TrackRing const& ring, uint32 nowMs, uint32 ageMs, TrackPoint& out, bool* covered = nullptr)
{
    if (covered)
        *covered = true;

    if (ring.Count == 0)
        return false;

    for (uint32 back = 0; back < ring.Count; ++back)
    {
        TrackSample const& sample = ring.At(back);
        uint32 const sampleAge = nowMs - sample.Ms;
        if (sampleAge < ageMs)
            continue;

        if (sampleAge == ageMs || back == 0)
        {
            out = TrackPoint{sample.X, sample.Y, sample.Z};
            return true;
        }

        TrackSample const& newer = ring.At(back - 1);
        uint32 const newerAge = nowMs - newer.Ms;
        float const share = sampleAge > newerAge
                                ? static_cast<float>(sampleAge - ageMs) / static_cast<float>(sampleAge - newerAge)
                                : 1.0f;
        out = TrackPoint{sample.X + (newer.X - sample.X) * share, sample.Y + (newer.Y - sample.Y) * share,
                         sample.Z + (newer.Z - sample.Z) * share};
        return true;
    }

    TrackSample const& oldest = ring.At(ring.Count - 1);
    out = TrackPoint{oldest.X, oldest.Y, oldest.Z};
    if (covered)
        *covered = false;

    return true;
}

// The mean velocity (yards per ms) over windowMs that ends ageMs ago; false where the ring does not reach that far
inline bool TrackVelocityAt(TrackRing const& ring, uint32 nowMs, uint32 ageMs, uint32 windowMs, float& vx, float& vy)
{
    TrackPoint end;
    TrackPoint start;
    bool covered = false;
    if (windowMs == 0 || !TrackPositionAt(ring, nowMs, ageMs, end) ||
        !TrackPositionAt(ring, nowMs, ageMs + windowMs, start, &covered) || !covered)
        return false;

    vx = (end.X - start.X) / static_cast<float>(windowMs);
    vy = (end.Y - start.Y) / static_cast<float>(windowMs);
    return true;
}

// The least gap between two ring samples: SampleMs, or wider so that RingSamples reach back over the lag's tail and
// the history (or the velocity and standing windows) the bot reads
inline uint32 TrackSampleGapMs(TrackParams const& params, TrackConfig const& config)
{
    uint32 const samples = std::clamp(config.RingSamples, MinTrackRingSamples, MaxTrackRingSamples);
    uint32 const reads = !params.Velocity ? 0u
                         : params.Pattern
                             ? std::max({params.HistoryMs, 2 * TRACK_VELOCITY_WINDOW_MS, TRACK_STAND_WINDOW_MS})
                             : std::max(2 * TRACK_VELOCITY_WINDOW_MS, TRACK_STAND_WINDOW_MS);
    uint32 const need = static_cast<uint32>(static_cast<float>(params.LagMs) * TRACK_LAG_SPAN_FACTOR) + reads;
    return std::max(config.SampleMs, (need + samples - 2) / (samples - 1));
}

// How the predicted point came about (tactics show, the trace line)
enum class TrackMode : uint8
{
    None,
    LastSeen,  // LastSeen level, or no velocity readable yet
    Standing,  // the target stood
    Velocity,
    Arc,      // the turn rate bent it
    Juke,     // the turn-rate reader saw a juke and went straight on the new heading
    Pattern,  // a repeated juke read
    Count
};

inline char const* TrackModeName(TrackMode mode)
{
    switch (mode)
    {
        case TrackMode::None:
            return "none";
        case TrackMode::LastSeen:
            return "last seen";
        case TrackMode::Standing:
            return "standing";
        case TrackMode::Velocity:
            return "velocity";
        case TrackMode::Arc:
            return "arc";
        case TrackMode::Juke:
            return "juke";
        case TrackMode::Pattern:
            return "pattern";
        case TrackMode::Count:
            break;
    }

    return "?";
}

struct TrackPrediction
{
    TrackPoint Point;      // where the chaser runs
    TrackPoint Seen;       // the target as the chaser saw it
    uint32 LagMs = 0;      // the lag drawn
    uint32 SeenAgeMs = 0;  // the age of what it saw: the newest sample at least LagMs old (the oldest on a young ring)
    float Gain = 0.0f;
    TrackMode Mode = TrackMode::None;
    bool Capped = false;     // MaxLeadYards cut the lead
    bool JukeAhead = false;  // the pattern expected a juke between the seen instant and now
};

// The jukes the chaser read in its history: a heading change of at least JukeDeg between the segments on either side
// of one segment (a juke between two samples), its corner where the two segments' lines cross
struct TrackPatternRead
{
    uint32 Jukes = 0;
    uint32 NewestAgeMs = 0;  // the newest juke's age and corner, the velocity after it (yards per ms)
    TrackPoint Newest;
    float AfterVx = 0.0f;
    float AfterVy = 0.0f;
    uint32 PrevAgeMs = 0;  // the juke before it
    TrackPoint Prev;
    uint32 OldestAgeMs = 0;
};

inline TrackPatternRead TrackReadJukes(TrackRing const& ring, uint32 nowMs, uint32 first, uint32 historyMs,
                                       float jukeRad)
{
    TrackPatternRead read;
    if (first >= ring.Count)
        return read;

    uint32 const seenAgeMs = nowMs - ring.At(first).Ms;
    // Segment k runs from sample k + 1 to sample k; segment k against segment k + 2 (the juke inside k + 1), while
    // the middle segment is within the history
    uint32 back = first;
    while (back + 3 < ring.Count && nowMs - ring.At(back + 2).Ms <= seenAgeMs + historyMs)
    {
        TrackSample const& p0 = ring.At(back);
        TrackSample const& p1 = ring.At(back + 1);
        TrackSample const& p2 = ring.At(back + 2);
        TrackSample const& p3 = ring.At(back + 3);
        float const ax = p2.X - p3.X;
        float const ay = p2.Y - p3.Y;
        float const bx = p0.X - p1.X;
        float const by = p0.Y - p1.Y;
        float const lengthA = std::hypot(ax, ay);
        if (lengthA < TRACK_MIN_SEGMENT_YARDS || std::hypot(bx, by) < TRACK_MIN_SEGMENT_YARDS ||
            std::fabs(TrackTurn(std::atan2(ay, ax), std::atan2(by, bx))) < jukeRad)
        {
            ++back;
            continue;
        }

        // The corner: p2 + s * a meets p1 - u * b; else the middle segment's middle. Its instant at the speed before.
        float const cross = ax * by - ay * bx;
        float const s = cross != 0.0f ? ((p1.X - p2.X) * by - (p1.Y - p2.Y) * bx) / cross : -1.0f;
        float const middleLength = std::hypot(p1.X - p2.X, p1.Y - p2.Y);
        TrackPoint corner{(p1.X + p2.X) * 0.5f, (p1.Y + p2.Y) * 0.5f, (p1.Z + p2.Z) * 0.5f};
        uint32 cornerMs = p2.Ms + (p1.Ms - p2.Ms) / 2;
        if (s >= 0.0f && s * lengthA <= middleLength)
        {
            float const along = s * lengthA;
            corner = TrackPoint{p2.X + s * ax, p2.Y + s * ay, p2.Z};
            float const speed = lengthA / static_cast<float>(std::max(p2.Ms - p3.Ms, 1u));
            cornerMs = p2.Ms + std::min(static_cast<uint32>(along / std::max(speed, 1e-6f)), p1.Ms - p2.Ms);
        }

        uint32 const age = nowMs - cornerMs;
        if (read.Jukes == 0)
        {
            float const afterMs = static_cast<float>(std::max(p0.Ms - p1.Ms, 1u));
            read.NewestAgeMs = age;
            read.Newest = corner;
            read.AfterVx = bx / afterMs;
            read.AfterVy = by / afterMs;
        }
        else if (read.Jukes == 1)
        {
            read.PrevAgeMs = age;
            read.Prev = corner;
        }

        read.OldestAgeMs = age;
        ++read.Jukes;
        back += 2;
    }

    return read;
}

// Where the chaser runs now: what it saw (the newest sample at least the drawn lag old: no interpolation across
// samples it has not seen yet) plus its prediction over that age, capped at MaxLeadYards
inline TrackPrediction TrackPredict(TrackRing const& ring, TrackParams const& params, TrackConfig const& config,
                                    uint32 nowMs, uint64 hash)
{
    TrackPrediction prediction;
    double const lagZ = Probit(UnitFromHash(SplitMix64(hash ^ TRACK_LAG_SALT)));
    prediction.LagMs =
        std::min(static_cast<uint32>(
                     static_cast<float>(params.LagMs) * std::exp(params.LagSigma * static_cast<float>(lagZ)) + 0.5f),
                 TRACK_MAX_LAG_MS);
    if (ring.Count == 0)
        return prediction;

    uint32 first = 0;
    while (first + 1 < ring.Count && nowMs - ring.At(first).Ms < prediction.LagMs)
        ++first;

    TrackSample const& seen = ring.At(first);
    uint32 const seenAge = nowMs - seen.Ms;
    prediction.SeenAgeMs = seenAge;
    prediction.Seen = TrackPoint{seen.X, seen.Y, seen.Z};
    prediction.Point = prediction.Seen;
    prediction.Mode = TrackMode::LastSeen;
    if (!params.Velocity)
        return prediction;

    // A standing target (also a fresh ring: nothing moved yet)
    TrackPoint before;
    TrackPositionAt(ring, nowMs, seenAge + TRACK_STAND_WINDOW_MS, before);
    if (TrackDistance2d(before, prediction.Seen) < static_cast<float>(config.MinMoveYards))
    {
        prediction.Mode = TrackMode::Standing;
        return prediction;
    }

    float vx = 0.0f;
    float vy = 0.0f;
    if (!TrackVelocityAt(ring, nowMs, seenAge, TRACK_VELOCITY_WINDOW_MS, vx, vy))
        return prediction;

    double const gainZ = Probit(UnitFromHash(SplitMix64(hash ^ TRACK_GAIN_SALT)));
    prediction.Gain = std::min(params.Gain * std::exp(params.GainSigma * static_cast<float>(gainZ)), TRACK_MAX_GAIN);
    float const horizon = static_cast<float>(seenAge);
    float const jukeRad = static_cast<float>(config.JukeDeg) * TRACK_RAD_PER_DEG;
    float dx = vx * horizon;
    float dy = vy * horizon;
    prediction.Mode = TrackMode::Velocity;

    TrackPatternRead const read = params.Pattern && params.HistoryMs > 0
                                      ? TrackReadJukes(ring, nowMs, first, params.HistoryMs, jukeRad)
                                      : TrackPatternRead{};
    if (read.Jukes >= TRACK_MIN_JUKES)
    {
        // A repeated juke: the current leg's velocity (since the newest corner), the next juke a period after it and
        // the leg before the newest corner as the next one (the zigzag's other heading)
        uint32 const periodMs = (read.OldestAgeMs - read.NewestAgeMs) / (read.Jukes - 1);
        uint32 const legMs = read.NewestAgeMs > seenAge ? read.NewestAgeMs - seenAge : 0;
        float cx = read.AfterVx;
        float cy = read.AfterVy;
        if (legMs >= TRACK_MIN_LEG_MS)
        {
            cx = (prediction.Seen.X - read.Newest.X) / static_cast<float>(legMs);
            cy = (prediction.Seen.Y - read.Newest.Y) / static_cast<float>(legMs);
        }

        float const prevMs = static_cast<float>(std::max(read.PrevAgeMs - read.NewestAgeMs, 1u));
        float const px = (read.Newest.X - read.Prev.X) / prevMs;
        float const py = (read.Newest.Y - read.Prev.Y) / prevMs;
        // The next juke's age (negative: after now). Due before the seen instant but not read yet (its corner needs
        // two samples after it): the target is on the other heading already.
        int32 const nextAge = static_cast<int32>(read.NewestAgeMs) - static_cast<int32>(periodMs);
        if (nextAge >= static_cast<int32>(seenAge))
        {
            dx = px * horizon;
            dy = py * horizon;
            prediction.JukeAhead = true;
        }
        else if (nextAge >= 0)
        {
            float const beforeMs = static_cast<float>(seenAge - static_cast<uint32>(nextAge));
            float const afterMs = static_cast<float>(nextAge);
            dx = cx * beforeMs + px * afterMs;
            dy = cy * beforeMs + py * afterMs;
            prediction.JukeAhead = true;
        }
        else
        {
            dx = cx * horizon;
            dy = cy * horizon;
        }

        prediction.Mode = TrackMode::Pattern;
    }
    else if (params.TurnRate)
    {
        float ux = 0.0f;
        float uy = 0.0f;
        if (TrackVelocityAt(ring, nowMs, seenAge + TRACK_VELOCITY_WINDOW_MS, TRACK_VELOCITY_WINDOW_MS, ux, uy) &&
            (ux != 0.0f || uy != 0.0f) && (vx != 0.0f || vy != 0.0f))
        {
            float const from = std::atan2(uy, ux);
            float const to = std::atan2(vy, vx);
            float const turn = TrackTurn(from, to);
            if (std::fabs(turn) >= jukeRad)
                prediction.Mode = TrackMode::Juke;
            else
            {
                // An arc at the turn rate read: the heading turns rate * t over the horizon
                float const rate = std::clamp(turn / static_cast<float>(TRACK_VELOCITY_WINDOW_MS),
                                              -TRACK_MAX_TURN_RAD_PER_MS, TRACK_MAX_TURN_RAD_PER_MS);
                float const sweep = rate * horizon;
                if (std::fabs(sweep) > 1e-4f)
                {
                    float const speed = std::hypot(vx, vy);
                    // The window's mean heading lags its end by half the window's turn
                    float const heading = to + rate * static_cast<float>(TRACK_VELOCITY_WINDOW_MS) * 0.5f;
                    dx = speed / rate * (std::sin(heading + sweep) - std::sin(heading));
                    dy = speed / rate * (std::cos(heading) - std::cos(heading + sweep));
                    prediction.Mode = TrackMode::Arc;
                }
            }
        }
    }

    dx *= prediction.Gain;
    dy *= prediction.Gain;
    float const lead = std::hypot(dx, dy);
    float const maxLead = static_cast<float>(config.MaxLeadYards);
    if (lead > maxLead)
    {
        float const scale = lead > 0.0f ? maxLead / lead : 0.0f;
        dx *= scale;
        dy *= scale;
        prediction.Capped = true;
    }

    prediction.Point = TrackPoint{prediction.Seen.X + dx, prediction.Seen.Y + dy, prediction.Seen.Z};
    return prediction;
}

inline uint64 TrackHash(uint64 botGuid, uint32 nowMs, uint32 call)
{
    return SplitMix64(SplitMix64((botGuid << 32) ^ nowMs ^ 0x5851F42D4C957F2DULL) ^ call);
}

// The per-bot state (PlayerbotAI::GetTrackState; the bot's own update, tactics show on the world thread)
struct TrackBotState
{
    TrackParams Params;
    TrackRing Ring;
    uint64 TargetGuid = 0;  // the ring's target (raw guid)
    uint32 Calls = 0;       // predictions made (the hash's call number)
    // StuckMs: the least distance to the target this chase, where the chaser stood then and since when; the true aim
    // until
    float BestDistance = 0.0f;
    float MarkX = 0.0f;
    float MarkY = 0.0f;
    uint32 BestAtMs = 0;
    uint32 TrueUntilMs = 0;
    uint32 InSightCheckedMs = 0;  // OutsidePvpInSight's cached answer
    bool InSight = false;
    // What the bot did (tactics show)
    uint32 Restarts = 0;  // the ring started again (a new target, or stale)
    uint32 Chases = 0;    // destinations set from a tracked point
    uint32 LagMsSum = 0;
    uint32 Capped = 0;
    uint32 Unsticks = 0;
    std::array<uint32, static_cast<uint32>(TrackMode::Count)> Modes{};
    TrackMode LastMode = TrackMode::None;
    uint32 LastLagMs = 0;
    float LastLeadYards = 0.0f;
    float LastOffYards = 0.0f;  // the tracked point's distance from the true position
    uint32 LastMs = 0;

    void ResetStuck()
    {
        BestAtMs = 0;
        TrueUntilMs = 0;
    }

    // A predicted point used as a destination
    void NoteChase(TrackPrediction const& prediction, float offYards, uint32 nowMs)
    {
        ++Chases;
        LagMsSum += prediction.SeenAgeMs;
        Capped += prediction.Capped ? 1 : 0;
        ++Modes[static_cast<uint32>(prediction.Mode)];
        LastMode = prediction.Mode;
        LastLagMs = prediction.SeenAgeMs;
        LastLeadYards = TrackDistance2d(prediction.Point, prediction.Seen);
        LastOffYards = offYards;
        LastMs = std::max(nowMs, 1u);
    }
};

// The target's true position at a ReachCombatTo call: a new target or a stale ring starts again, a sample goes in
// once per TrackSampleGapMs
inline void TrackObserve(TrackBotState& state, TrackConfig const& config, uint64 targetGuid, uint32 nowMs, float x,
                         float y, float z)
{
    if (state.TargetGuid != targetGuid || (state.Ring.Count > 0 && nowMs - state.Ring.At(0).Ms > TRACK_STALE_MS))
    {
        state.Ring.Clear();
        state.TargetGuid = targetGuid;
        state.ResetStuck();
        ++state.Restarts;
    }

    if (state.Ring.Count > 0 && nowMs - state.Ring.At(0).Ms < TrackSampleGapMs(state.Params, config))
        return;

    state.Ring.Push(TrackSample{nowMs, x, y, z}, config.RingSamples);
}

// The ring's feed at the bot's own checks in a fight (PlayerbotAI.cpp FeedNaturalTrack): the current target's true
// position, so a chase after a pause (an in-range cast, a held move) sees the target as it was lag ms ago, not where
// it is now (a ring fed only by chase calls was stale then and started again on one sample of age 0). It never starts
// the ring again over another target's live ring (a chase to a CC or kick target keeps its history); a stale or empty
// ring takes the current target. False: not fed.
inline bool TrackFeed(TrackBotState& state, TrackConfig const& config, uint64 targetGuid, uint32 nowMs, float x,
                      float y, float z)
{
    if (state.TargetGuid != targetGuid && state.Ring.Count > 0 && nowMs - state.Ring.At(0).Ms <= TRACK_STALE_MS)
        return false;

    TrackObserve(state, config, targetGuid, nowMs, x, y, z);
    return true;
}

// StuckMs: a chaser out of its reach that for stuckMs neither closed TRACK_CLOSE_YARDS on the target nor ran
// TRACK_MOVED_YARDS itself aims at the target's true position for TRACK_TRUE_AIM_MS, then tracks again. A chase at an
// equal distance (the chaser runs) and a bot within reach (a caster holding its range) are no stuck chasers; a target
// that got far away (a blink, a charge) starts a new count. distance and reach as ReachCombatTo's (combat reaches in)
inline bool TrackAimTrue(TrackBotState& state, float distance, float reach, float botX, float botY, uint32 nowMs,
                         uint32 stuckMs)
{
    if (state.TrueUntilMs != 0)
    {
        if (!TrackReached(state.TrueUntilMs, nowMs))
            return true;

        state.TrueUntilMs = 0;
    }

    bool const ran = std::hypot(botX - state.MarkX, botY - state.MarkY) >= TRACK_MOVED_YARDS;
    if (state.BestAtMs == 0 || distance <= reach || ran || distance < state.BestDistance - TRACK_CLOSE_YARDS ||
        distance > state.BestDistance + TRACK_ESCAPE_YARDS)
    {
        state.BestDistance = distance;
        state.MarkX = botX;
        state.MarkY = botY;
        state.BestAtMs = std::max(nowMs, 1u);
        return false;
    }

    if (stuckMs == 0 || nowMs - state.BestAtMs < stuckMs)
        return false;

    ++state.Unsticks;
    state.TrueUntilMs = std::max(nowMs + TRACK_TRUE_AIM_MS, 1u);
    state.BestDistance = distance;
    state.MarkX = botX;
    state.MarkY = botY;
    state.BestAtMs = std::max(nowMs, 1u);
    return true;
}

// tactics selftest track (also the offline harness): the numbers by skill, the draws, the ring, then simulated chases
// (a call every 100 ms, as the AI's checks) on a straight run, a circle and a zigzag: the prediction error falls with
// skill, every level misses a sudden turn (at its first call after it and over the lag and a sample gap after it,
// leading along the old heading), the turn rate helps on the circle and the pattern on the zigzag
template <typename Check>
void RunTrackChecks(Check&& check)
{
    TrackConfig const config;
    auto const attributesAt = [](uint8 ability)
    {
        BotAttributeValues values;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = index >= BOT_ABILITY_COUNT ? 50 : ability;

        return values;
    };
    auto const paramsAt = [&](uint8 skill) { return ComputeTrackParams(skill, attributesAt(skill), 1.0f, config); };
    auto const within = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };

    // The numbers by skill
    TrackParams const p99 = paramsAt(99);
    TrackParams const p1 = paramsAt(1);
    TrackParams const p0 = ComputeTrackParams(0, attributesAt(0), 1.0f, config);
    check(p1.LagMs == config.LagMsAtSkill1 && p99.LagMs == config.LagMsAtSkill99,
          "the lag runs from LagMsAtSkill1 to LagMsAtSkill99");
    check(p0.LagMs == p99.LagMs && p0.Pattern && within(p0.Gain, p99.Gain, 1e-5f), "skill 0 is full ability");
    check(p1.Level() == TrackLevel::LastSeen && paramsAt(40).Level() == TrackLevel::Velocity &&
              paramsAt(55).Level() == TrackLevel::TurnRate && paramsAt(70).Level() == TrackLevel::Pattern,
          "the levels: last seen, velocity from 35, turn rate from 50, pattern from 65");
    check(within(p1.Gain, 0.0f, 1e-5f) && within(p99.Gain, 0.9f, 1e-5f) && p1.HistoryMs == 0 && p99.HistoryMs == 1500,
          "the gain runs from 0 to 0.9 and the history from 0 to 1500 ms");
    bool lagFalls = true;
    bool gainRises = true;
    for (uint8 skill = 2; skill <= 99; ++skill)
    {
        lagFalls = lagFalls && paramsAt(skill).LagMs <= paramsAt(skill - 1).LagMs;
        gainRises = gainRises && paramsAt(skill).Gain >= paramsAt(skill - 1).Gain;
    }

    check(lagFalls && gainRises, "the lag falls and the gain rises with skill");

    // The draws: log-normal around the medians
    {
        TrackRing ring;
        for (uint32 index = 0; index <= 30; ++index)
            ring.Push(TrackSample{index * 100, 0.7f * static_cast<float>(index), 0.0f, 0.0f}, MaxTrackRingSamples);

        std::array<uint32, TRACK_SELFTEST_DRAWS> lags{};
        std::array<float, TRACK_SELFTEST_DRAWS> gains{};
        for (uint32 index = 0; index < TRACK_SELFTEST_DRAWS; ++index)
        {
            TrackPrediction const prediction =
                TrackPredict(ring, paramsAt(50), config, 3000, TrackHash(9, 3000, index));
            lags[index] = prediction.LagMs;
            gains[index] = prediction.Gain;
        }

        std::sort(lags.begin(), lags.end());
        std::sort(gains.begin(), gains.end());
        float const lagMedian = static_cast<float>(paramsAt(50).LagMs);
        float const p90 = std::exp(1.2816f * 0.25f);
        check(within(static_cast<float>(lags[TRACK_SELFTEST_DRAWS / 2]), lagMedian, lagMedian * 0.03f) &&
                  within(static_cast<float>(lags[TRACK_SELFTEST_DRAWS * 9 / 10]), lagMedian * p90, lagMedian * 0.05f) &&
                  within(static_cast<float>(lags[TRACK_SELFTEST_DRAWS / 10]), lagMedian / p90, lagMedian * 0.05f),
              "the lag draw is log-normal around the median (sigma 25 %)");
        check(within(gains[TRACK_SELFTEST_DRAWS / 2], paramsAt(50).Gain, paramsAt(50).Gain * 0.03f) &&
                  gains[TRACK_SELFTEST_DRAWS - 1] <= TRACK_MAX_GAIN,
              "the gain draw is log-normal around the median and capped");
    }

    // The ring
    {
        TrackRing ring;
        TrackPoint point;
        check(!TrackPositionAt(ring, 100, 0, point), "an empty ring has no position");
        for (uint32 index = 0; index < 40; ++index)
            ring.Push(TrackSample{index * 100, static_cast<float>(index), 0.0f, 0.0f}, 16);

        bool covered = true;
        TrackPositionAt(ring, 3900, 250, point, &covered);
        bool const middle = within(point.X, 36.5f, 1e-4f) && covered;
        TrackPositionAt(ring, 3950, 0, point, &covered);
        bool const newest = within(point.X, 39.0f, 1e-4f) && covered;
        TrackPositionAt(ring, 3900, 5000, point, &covered);
        check(ring.Count == 16 && ring.At(0).Ms == 3900 && ring.At(15).Ms == 2400 && middle && newest &&
                  within(point.X, 24.0f, 1e-4f) && !covered,
              "the ring keeps RingSamples, interpolates and clamps to its newest and oldest sample");
        float vx = 0.0f;
        float vy = 0.0f;
        check(TrackVelocityAt(ring, 3900, 100, 250, vx, vy) && within(vx, 0.01f, 1e-6f) &&
                  !TrackVelocityAt(ring, 3900, 1400, 250, vx, vy),
              "the velocity is read where the ring reaches");
        // Clock wrap
        TrackRing wrapped;
        uint32 const base = 0xFFFFFF00u;
        for (uint32 index = 0; index < 6; ++index)
            wrapped.Push(TrackSample{base + index * 100, static_cast<float>(index), 0.0f, 0.0f}, 16);

        TrackPositionAt(wrapped, base + 500, 150, point);
        check(within(point.X, 3.5f, 1e-4f), "the ring works across the clock's wrap");
    }

    // Observation: the gap, a new target, a stale ring
    {
        TrackBotState state;
        state.Params = p99;
        uint32 const gap = TrackSampleGapMs(p99, config);
        check(gap == 111 && TrackSampleGapMs(p1, config) == config.SampleMs,
              "the sample gap widens so that the ring covers the lag and the history");
        TrackObserve(state, config, 1, 1000, 0.0f, 0.0f, 0.0f);
        TrackObserve(state, config, 1, 1000 + gap - 1, 1.0f, 0.0f, 0.0f);
        bool const held = state.Ring.Count == 1;
        TrackObserve(state, config, 1, 1000 + gap, 1.0f, 0.0f, 0.0f);
        bool const pushed = state.Ring.Count == 2;
        TrackObserve(state, config, 2, 1000 + 2 * gap, 1.0f, 0.0f, 0.0f);
        bool const newTarget = state.Ring.Count == 1 && state.TargetGuid == 2;
        TrackObserve(state, config, 2, 1000 + 2 * gap + TRACK_STALE_MS + 1, 1.0f, 0.0f, 0.0f);
        check(held && pushed && newTarget && state.Ring.Count == 1 && state.Restarts == 3,
              "one sample per gap; a new target and a stale ring start again");
    }

    // The feed (TrackFeed): a chase, a pause of 1500 ms without chase calls (an in-range cast), a chase again. Fed
    // only by chase calls the ring is stale then and the chaser runs to the true position; fed at every check (100
    // ms) it sees the target at least its lag ago. The feed leaves another target's live ring alone.
    {
        TrackParams const p80 = paramsAt(80);
        auto const after = [&](bool feed)
        {
            TrackBotState state;
            state.Params = p80;
            for (uint32 ms = 1000; ms <= 4000; ms += 100)
            {
                float const x = TRACK_SELFTEST_RUN * static_cast<float>(ms);
                if (ms <= 1500 || ms == 3000)
                    TrackObserve(state, config, 1, ms, x, 0.0f, 0.0f);
                else if (feed)
                    TrackFeed(state, config, 1, ms, x, 0.0f, 0.0f);

                if (ms == 3000)
                    return std::make_pair(TrackPredict(state.Ring, state.Params, config, ms, TrackHash(5, ms, 0)),
                                          state.Restarts);
            }

            return std::make_pair(TrackPrediction{}, 0u);
        };
        auto const [unfed, unfedRestarts] = after(false);
        auto const [fed, fedRestarts] = after(true);
        check(unfed.SeenAgeMs == 0 && unfedRestarts == 2,
              "fed only by chase calls, a chase after a pause starts the ring again and sees the true position");
        check(fed.SeenAgeMs >= fed.LagMs && fed.SeenAgeMs > 0 && fedRestarts == 1 && fed.Mode != TrackMode::LastSeen,
              "fed at every check, a chase after a pause sees the target its lag ago and predicts from there");

        TrackBotState state;
        state.Params = p80;
        TrackObserve(state, config, 2, 1000, 0.0f, 0.0f, 0.0f);
        bool const kept = !TrackFeed(state, config, 1, 1000 + TRACK_STALE_MS, 5.0f, 0.0f, 0.0f) &&
                          state.TargetGuid == 2 && state.Restarts == 1;
        bool const taken = TrackFeed(state, config, 1, 1000 + TRACK_STALE_MS + 1, 5.0f, 0.0f, 0.0f) &&
                           state.TargetGuid == 1 && state.Restarts == 2;
        check(kept && taken, "the feed keeps another target's live ring and takes a stale one");
    }

    // The simulated chases: the target's true path, a call every 100 ms with each chaser's own phase
    struct Path
    {
        char const* Name;
        uint32 Kind;  // 0 straight, 1 circle, 2 zigzag, 3 a sudden turn at 4000 ms
    };
    auto const positionAt = [](uint32 kind, uint32 ms)
    {
        float const t = static_cast<float>(ms);
        switch (kind)
        {
            case 1:
            {
                float const radius = 10.0f;
                float const angle = TRACK_SELFTEST_RUN * t / radius;
                return TrackPoint{radius * std::cos(angle), radius * std::sin(angle), 0.0f};
            }
            case 2:
            {
                // Legs of 700 ms at +-45 degrees from the drift
                uint32 const legMs = 700;
                float const side = TRACK_SELFTEST_RUN * std::sqrt(0.5f);
                uint32 const legs = ms / legMs;
                float const into = static_cast<float>(ms % legMs);
                float const y = (legs % 2 == 0 ? into : static_cast<float>(legMs) - into) * side;
                return TrackPoint{side * t, y, 0.0f};
            }
            case 3:
            {
                if (ms <= 4000)
                    return TrackPoint{TRACK_SELFTEST_RUN * t, 0.0f, 0.0f};

                return TrackPoint{TRACK_SELFTEST_RUN * 4000.0f, TRACK_SELFTEST_RUN * (t - 4000.0f), 0.0f};
            }
            default:
                return TrackPoint{TRACK_SELFTEST_RUN * t, 0.0f, 0.0f};
        }
    };
    // The mean error (yards) of the predicted point against the true position now, over chasers and calls; with
    // Pattern/TurnRate overrides for the comparisons. The sudden turn is read over turnWindowMs after the turn (100:
    // at its first call after it).
    struct Result
    {
        float Error = 0.0f;
        float AlongNew = 0.0f;      // sudden turn: the prediction's progress along the new heading
        float TrueAlongNew = 0.0f;  // sudden turn: the target's true progress along the new heading
        float LeadOld = 0.0f;       // sudden turn: the lead (predicted minus seen point) along the old heading
        float LeadNew = 0.0f;       // sudden turn: the lead along the new heading
        float PatternShare = 0.0f;
    };
    auto const chase = [&](TrackParams const& params, uint32 kind, uint32 turnWindowMs)
    {
        Result result;
        uint32 count = 0;
        uint32 patterns = 0;
        for (uint32 bot = 0; bot < TRACK_SELFTEST_BOTS; ++bot)
        {
            TrackBotState state;
            state.Params = params;
            uint32 const phase = (bot * 37) % 100;
            for (uint32 ms = phase; ms <= 8000; ms += 100)
            {
                TrackPoint const now = positionAt(kind, ms);
                TrackObserve(state, config, 1, ms, now.X, now.Y, now.Z);
                bool const measured = kind == 3 ? ms > 4000 && ms <= 4000 + turnWindowMs : ms >= 3000;
                if (!measured)
                    continue;

                TrackPrediction const prediction =
                    TrackPredict(state.Ring, state.Params, config, ms, TrackHash(bot + 1, ms, state.Calls++));
                result.Error += TrackDistance2d(prediction.Point, now);
                result.AlongNew += prediction.Point.Y;
                result.TrueAlongNew += now.Y;
                result.LeadOld += prediction.Point.X - prediction.Seen.X;
                result.LeadNew += prediction.Point.Y - prediction.Seen.Y;
                patterns += prediction.Mode == TrackMode::Pattern ? 1 : 0;
                ++count;
            }
        }

        if (count > 0)
        {
            result.Error /= static_cast<float>(count);
            result.AlongNew /= static_cast<float>(count);
            result.TrueAlongNew /= static_cast<float>(count);
            result.LeadOld /= static_cast<float>(count);
            result.LeadNew /= static_cast<float>(count);
            result.PatternShare = static_cast<float>(patterns) / static_cast<float>(count);
        }

        return result;
    };
    auto const simulate = [&](TrackParams const& params, uint32 kind) { return chase(params, kind, 100); };

    std::array<uint8, 6> const skills = {1, 30, 45, 60, 80, 99};
    for (Path const& path : {Path{"straight run", 0}, Path{"circle", 1}, Path{"zigzag", 2}})
    {
        bool falls = true;
        float previous = 1e9f;
        std::string errors;
        for (uint8 skill : skills)
        {
            float const error = simulate(paramsAt(skill), path.Kind).Error;
            falls = falls && error < previous;
            previous = error;
            errors += (errors.empty() ? "" : "/") + std::to_string(static_cast<int>(error * 100.0f + 0.5f));
        }

        check(falls, std::string("the prediction error falls with skill on a ") + path.Name +
                         " (cyd at skill 1/30/45/60/80/99: " + errors + ")");
    }

    // A sudden turn: no level reads it before it sees it (at the first call after the turn the prediction runs on
    // along the old heading: less than half the new heading's progress, an error of at least half of it)
    {
        float const progress = TRACK_SELFTEST_RUN * 50.0f;  // the mean progress on the new heading at that call
        bool missed = true;
        std::string errors;
        for (uint8 skill : skills)
        {
            Result const result = simulate(paramsAt(skill), 3);
            missed = missed && result.AlongNew < progress * 0.5f && result.Error >= progress * 0.5f;
            errors += (errors.empty() ? "" : "/") + std::to_string(static_cast<int>(result.Error * 100.0f + 0.5f));
        }

        check(missed, "every level misses a sudden turn (cyd at skill 1/30/45/60/80/99: " + errors + ")");
    }

    // Over the whole window in which the turn hides (the lag and one sample gap after it) no level anticipates the new
    // heading (less than a quarter of the target's mean progress on it, an error of at least half of it), and the lead
    // keeps pointing along the old heading (no lead below the velocity level)
    {
        bool missed = true;
        bool ledOld = true;
        std::string errors;
        std::string leads;
        auto const cyd = [](float yards) { return std::to_string(static_cast<int>(std::lround(yards * 100.0f))); };
        for (uint8 skill : skills)
        {
            TrackParams const params = paramsAt(skill);
            Result const result = chase(params, 3, params.LagMs + TrackSampleGapMs(params, config));
            missed =
                missed && result.AlongNew < result.TrueAlongNew * 0.25f && result.Error >= result.TrueAlongNew * 0.5f;
            ledOld = ledOld && (params.Level() == TrackLevel::LastSeen
                                    ? std::fabs(result.LeadOld) < 1e-4f && std::fabs(result.LeadNew) < 1e-4f
                                    : result.LeadOld > 0.0f && result.LeadOld > 2.0f * std::fabs(result.LeadNew));
            errors += (errors.empty() ? "" : "/") + cyd(result.AlongNew) + ":" + cyd(result.TrueAlongNew);
            leads += (leads.empty() ? "" : "/") + cyd(result.LeadOld) + ":" + cyd(result.LeadNew);
        }

        check(missed,
              "every level misses a sudden turn over the lag and a sample gap after it (cyd on the new "
              "heading, predicted:true, at skill 1/30/45/60/80/99: " +
                  errors + ")");
        check(ledOld,
              "after a sudden turn the lead keeps pointing along the old heading (cyd old:new at skill "
              "1/30/45/60/80/99: " +
                  leads + ")");
    }

    // The turn rate helps on the circle, the pattern on the zigzag (the same bot without them)
    {
        TrackParams arcOff = paramsAt(60);
        arcOff.TurnRate = false;
        check(simulate(paramsAt(60), 1).Error < simulate(arcOff, 1).Error, "the turn rate tracks a circle better");
        TrackParams patternOff = p99;
        patternOff.Pattern = false;
        Result const withPattern = simulate(p99, 2);
        check(withPattern.PatternShare > 0.5f && withPattern.Error < simulate(patternOff, 2).Error,
              "a skilled bot reads the zigzag's pattern and tracks it better");
        check(simulate(p99, 0).PatternShare == 0.0f, "a straight run shows no pattern");
    }

    // Standing, the lead cap
    {
        TrackRing ring;
        for (uint32 index = 0; index < 16; ++index)
            ring.Push(TrackSample{index * 100, 5.0f + 0.001f * static_cast<float>(index), 3.0f, 1.0f}, 16);

        TrackPrediction const standing = TrackPredict(ring, p99, config, 1500, TrackHash(3, 1500, 0));
        check(standing.Mode == TrackMode::Standing && TrackDistance2d(standing.Point, standing.Seen) == 0.0f,
              "a standing target is where it was seen");
        TrackRing fast;
        for (uint32 index = 0; index < 16; ++index)
            fast.Push(TrackSample{index * 100, 5.0f * static_cast<float>(index), 0.0f, 0.0f}, 16);

        TrackParams slow = p99;
        slow.LagMs = 1000;
        TrackPrediction const capped = TrackPredict(fast, slow, config, 1500, TrackHash(3, 1500, 1));
        check(capped.Capped &&
                  within(TrackDistance2d(capped.Point, capped.Seen), static_cast<float>(config.MaxLeadYards), 1e-3f),
              "the lead is capped at MaxLeadYards");
        TrackRing single;
        single.Push(TrackSample{1000, 1.0f, 2.0f, 3.0f}, 16);
        TrackPrediction const fresh = TrackPredict(single, p99, config, 1000, TrackHash(3, 1000, 2));
        check(fresh.Point.X == 1.0f && fresh.Point.Y == 2.0f && fresh.Point.Z == 3.0f,
              "a fresh ring aims at its one sample");
    }

    // Stuck chasers (reach 5 yd): a standing one, one running at an equal distance, one within reach
    {
        float const reach = 5.0f;
        uint32 const stuckMs = config.StuckMs;
        TrackBotState state;
        auto const aim = [&](float distance, uint32 ms)
        { return TrackAimTrue(state, distance, reach, 0.0f, 0.0f, ms, stuckMs); };
        bool const first = !aim(10.0f, 1000);
        bool const closing = !aim(8.5f, 2500);
        bool const waiting = !aim(8.4f, 2500 + stuckMs - 1);
        bool const stuck = aim(8.4f, 2500 + stuckMs);
        bool const holds = aim(8.4f, 2500 + stuckMs + TRACK_TRUE_AIM_MS - 1);
        bool const again = !aim(8.4f, 2500 + stuckMs + TRACK_TRUE_AIM_MS);
        check(first && closing && waiting && stuck && holds && again && state.Unsticks == 1,
              "a chaser out of reach that neither closed nor ran for StuckMs aims at the true position for a while, "
              "then tracks again");
        bool const escaped = !aim(30.0f, 2500 + 3 * stuckMs);
        check(escaped && state.BestDistance == 30.0f, "a target far away starts a new count");

        TrackBotState running;
        bool runningFree = true;
        for (uint32 ms = 1000; ms <= 1000 + 3 * stuckMs; ms += 100)
        {
            float const x = TRACK_SELFTEST_RUN * static_cast<float>(ms);
            runningFree = runningFree && !TrackAimTrue(running, 10.0f, reach, x, 0.0f, ms, stuckMs);
        }

        check(runningFree && running.Unsticks == 0, "a chase at an equal distance (the chaser runs) is not stuck");
        TrackBotState holding;
        bool holdingFree = true;
        for (uint32 ms = 1000; ms <= 1000 + 3 * stuckMs; ms += 100)
            holdingFree = holdingFree && !TrackAimTrue(holding, 4.5f, reach, 0.0f, 0.0f, ms, stuckMs);

        check(holdingFree && holding.Unsticks == 0, "a bot within its reach (holding its range) is not stuck");
    }
}

// The game side (PlayerbotAI.cpp): whether the bot tracks naturally now (the option, the Natural tactic here, a PvP
// context of Natural.Track.Contexts, or out of PvP and out of instances in a real player's sight with
// OutsidePvpInSight, cached for InSightCacheMs). The bot's own update, or the world thread while the maps wait.
class PlayerbotAI;
bool NaturalTrackHere(PlayerbotAI* botAI, uint32 nowMs);

#endif
