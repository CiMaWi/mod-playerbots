/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PILLARTACTIC_H
#define _PLAYERBOT_PILLARTACTIC_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "ArenaCover.h"
#include "ArenaCoverConfig.h"
#include "Common.h"
#include "LosTactic.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// The Pillar tactic (G2f, PLAN 3.15, DT.9: arena only), the pure parts. "cast at me" -> "break los": an enemy's
// cast bar on the bot (a cast of HardCastMs or more, or crowd control; never a channel, which is not re-checked, nor a
// spell that ignores LOS) that no interrupt of the bot stopped (the interrupts rank above) makes the bot step out of
// the caster's sight before the bar ends: a pillar ring point hidden from the caster within StepYards
// (FindCoverPoint), else (Lordaeron has no table rows) a point round the bot that a live ray shows hidden. The core
// re-checks the target's LOS when the bar completes (Spell::_cast -> CheckCast), so the cast fails there
// (castsLosBroken, judged by one ray when the bar should have ended). "pillar step": a ranged bot chased by an enemy
// melee next to a pillar steps round it (NextCirclePoint, one hop of at most StepYards; on Ring of Valor only to the
// far side from the chaser). Skill (PillarConfig): known from KnownFromSkill (knowledge): the step starts when the
// bar has the walk plus a margin left (cautious bots earlier by up to CautiousEarlyMs, bold ones later), and melee
// is looped; below it a hard cast is hidden from at once by chance (RandomHidePctAtSkill1..59, a hash draw per cast;
// not counted as losBreaks, so a gate cell stays 0) or never. Guards: the relocation ring of the Los tactic (hold
// HoldMs, at most MaxRelocs per RelocWindowMs), the cover search interval, no move before the gates or in Ring of
// Valor's first RvIdleMs (LosArenaIdle), never across Dalaran's waterfall. DT.12: the visible cast bar, positions and
// rays only.

constexpr float PILLAR_RING_GAP = 1.5f;           // a ring point stands this far outside the pillar (FindCoverPoint)
constexpr float PILLAR_NEAR_YARDS = 3.0f;         // a pillar step only from at most this off a pillar's ring
constexpr float PILLAR_MIN_STEP_YARDS = 1.0f;     // a shorter step is no step (the pillar is between already)
constexpr float PILLAR_CHASER_YARDS = 8.0f;       // an enemy melee this near that targets the bot chases it
constexpr float PILLAR_RV_FAR_DEG = 90.0f;        // Ring of Valor: a step ends at least this far round from the chaser
constexpr uint32 PILLAR_MARGIN_MS = 400;          // a known break starts when the bar has the walk plus this left
constexpr uint32 PILLAR_ARRIVE_SLACK_MS = 150;    // the point must be reached this long before the bar ends
constexpr uint32 PILLAR_SAME_CAST_GAP_MS = 1000;  // a bar not seen for this long is a new cast
constexpr uint32 PILLAR_PUSHBACK_MS = 1000;       // pushback may set a bar back by this between two looks
constexpr uint32 PILLAR_JUDGE_MS = 250;           // a watched cast is judged this long after its bar should end
constexpr uint32 PILLAR_WATCH_MAX_MS = 4000;      // pushback extends a watch at most this past its first end
constexpr uint32 PILLAR_PARAMS_MS = 1000;         // the numbers are read again at most this often
constexpr float PILLAR_FALLBACK_SHORT = 0.6f;     // the fallback's inner ring, times StepYards
constexpr uint64 PILLAR_SALT_HIDE = 0xA54FF53A5F1D36F1ULL;

// Dalaran Sewers' waterfall (G2d-b's scan disc (1291.58, 790.67) r 7.21, left out of the table, plus a yard): no
// break or step walks across it, on or off (it blocks movement and LOS 30 s at a time)
constexpr float PILLAR_DS_WATERFALL_X = 1291.58f;
constexpr float PILLAR_DS_WATERFALL_Y = 790.67f;
constexpr float PILLAR_DS_WATERFALL_R = 8.21f;

// One bot's numbers (from PillarConfig and its attributes)
struct PillarParams
{
    bool Known = true;         // times its breaks and loops melee
    uint32 RandomHidePct = 0;  // below Known: the chance per hard cast that it hides at once
    int32 EarlyMs = 0;         // the style: cautious > 0 starts earlier, bold < 0 later
    uint32 HardCastMs = 1500;
    float StepYards = 6.0f;
};

struct PillarGateInputs
{
    uint8 Skill = 0;
    uint8 Knowledge = 0;
    uint8 Caution = SKILL_STYLE_NEUTRAL;
    bool FullAbility = true;
};

inline PillarParams ComputePillarParams(PillarConfig const& config, PillarGateInputs const& in)
{
    bool const full = in.FullAbility || in.Skill == 0;
    PillarParams params;
    params.Known = full || in.Knowledge >= config.KnownFromSkill;
    if (!params.Known)
    {
        // skill 1 .. 59 (the last skill below the default KnownFromSkill), linear
        float const position = std::clamp((static_cast<float>(in.Skill) - 1.0f) / 58.0f, 0.0f, 1.0f);
        float const pct = static_cast<float>(config.RandomHidePctAtSkill1) +
                          (static_cast<float>(config.RandomHidePctAtSkill59) - config.RandomHidePctAtSkill1) * position;
        params.RandomHidePct = std::min<uint32>(static_cast<uint32>(pct + 0.5f), 100);
    }

    int32 const caution = full ? SKILL_STYLE_NEUTRAL : in.Caution;
    params.EarlyMs = static_cast<int32>(config.CautiousEarlyMs) * (caution - SKILL_STYLE_NEUTRAL) / SKILL_STYLE_NEUTRAL;
    params.HardCastMs = config.HardCastMs;
    params.StepYards = static_cast<float>(config.StepYards);
    return params;
}

// The Los tactic's guards from its config (the ring is shared: a regain and a break count alike)
inline LosParams PillarGuardParams(LosConfig const& config)
{
    LosParams params;
    params.HoldMs = config.HoldMs;
    params.MaxRelocs = std::clamp<uint32>(config.MaxRelocs, 1, LOS_RING);
    params.RelocWindowMs = config.RelocWindowMs;
    params.SearchIntervalMs = config.SearchIntervalMs;
    params.MaxRays = std::max<uint32>(config.MaxRaysPerSearch, 1);
    return params;
}

// A bar worth hiding from: a cast time (no channel) of HardCastMs or more, or crowd control; LOS must matter to it
inline bool PillarHardCast(uint32 castMs, bool crowdControl, bool ignoresLos, uint32 hardCastMs)
{
    return !ignoresLos && castMs > 0 && (castMs >= hardCastMs || crowdControl);
}

enum class PillarWhen : uint8
{
    Wait,     // more bar left than the walk needs
    Now,      // step now
    TooLate,  // not even a short step makes it
};

// When a known bot steps: when the bar has the walk (travelMs) plus PILLAR_MARGIN_MS plus its style's EarlyMs left
// (never less than the walk plus PILLAR_ARRIVE_SLACK_MS); a random hide steps at once
inline PillarWhen PillarBreakWhen(PillarParams const& params, uint32 remainingMs, uint32 travelMs, bool known)
{
    if (remainingMs < PILLAR_ARRIVE_SLACK_MS)
        return PillarWhen::TooLate;

    if (!known)
        return PillarWhen::Now;

    int64 const start = std::max<int64>(static_cast<int64>(travelMs) + PILLAR_MARGIN_MS + params.EarlyMs,
                                        static_cast<int64>(travelMs) + PILLAR_ARRIVE_SLACK_MS);
    return static_cast<int64>(remainingMs) <= start ? PillarWhen::Now : PillarWhen::Wait;
}

// Does the walk of travelMs end before the bar (remainingMs)?
inline bool PillarArrivesInTime(uint32 remainingMs, uint32 travelMs)
{
    return static_cast<uint64>(travelMs) + PILLAR_ARRIVE_SLACK_MS <= remainingMs;
}

// The bar the bot looks at (the hard cast at it with the least time left)
struct PillarCastSeen
{
    uint64 Caster = 0;
    uint32 Spell = 0;
    uint32 ElapsedMs = 0;
    uint32 SeenMs = 0;
    uint32 Serial = 0;     // casts seen (the hash draw's key)
    bool Hide = false;     // this cast is hidden from (known, or the random draw)
    bool Handled = false;  // a break was tried for it
    bool Guarded = false;  // the guards refused it (counted once)
};

// The same bar as the last look: the same caster and spell, seen within PILLAR_SAME_CAST_GAP_MS, not set back by more
// than pushback can (a new cast of the same spell starts at the bar's beginning)
inline bool PillarSameCast(PillarCastSeen const& last, uint64 caster, uint32 spell, uint32 elapsedMs, uint32 nowMs)
{
    return last.SeenMs && last.Caster == caster && last.Spell == spell &&
           nowMs - last.SeenMs < PILLAR_SAME_CAST_GAP_MS &&
           static_cast<uint64>(elapsedMs) + PILLAR_PUSHBACK_MS >= last.ElapsedMs;
}

// Below Known: does the bot hide from this cast (a hash of the bot and the cast's serial; no RNG state)?
inline bool PillarRandomHide(uint32 pct, uint64 guid, uint32 serial)
{
    return pct > 0 && SplitMix64(guid ^ SplitMix64(PILLAR_SALT_HIDE + serial)) % 100 < pct;
}

// Does the floor segment a-b cross Dalaran's waterfall?
inline bool PillarCrossesWaterfall(uint32 mapId, CoverVec const& a, CoverVec const& b)
{
    return mapId == COVER_MAP_DALARAN &&
           CoverSegmentHitsDisc(a, b, PILLAR_DS_WATERFALL_X, PILLAR_DS_WATERFALL_Y, PILLAR_DS_WATERFALL_R);
}

// A step round a pillar from a melee chaser: the active pillar whose ring is nearest the bot (at most PILLAR_NEAR_YARDS
// off it), its NextCirclePoint; refused when shorter than PILLAR_MIN_STEP_YARDS (the pillar is between them already),
// longer than stepYards, outside the box, in a pillar or across the waterfall; farSideOnly (Ring of Valor): the point
// must lie at least PILLAR_RV_FAR_DEG round the pillar from the chaser
struct PillarStepPick
{
    bool Found = false;
    int32 Pillar = -1;
    CoverVec Point;
    float Yards = 0.0f;
};

inline PillarStepPick PickPillarStep(ArenaCoverTable const& table, uint32 activeGates, CoverVec const& self,
                                     CoverVec const& chaser, float stepYards, bool farSideOnly)
{
    PillarStepPick pick;
    float bestOff = PILLAR_NEAR_YARDS;
    int32 best = -1;
    for (uint32 index = 0; index < table.Count; ++index)
    {
        CoverPillar const& pillar = table.Pillars[index];
        if (!CoverPillarActive(pillar, activeGates))
            continue;

        float const off = std::fabs(std::hypot(self.X - pillar.X, self.Y - pillar.Y) - (pillar.R + PILLAR_RING_GAP));
        if (off <= bestOff)
        {
            bestOff = off;
            best = static_cast<int32>(index);
        }
    }

    if (best < 0)
        return pick;

    CoverPillar const& pillar = table.Pillars[best];
    CoverVec const point = NextCirclePoint(pillar, self, chaser, stepYards, PILLAR_RING_GAP);
    float const yards = CoverDist2d(self, point);
    if (yards < PILLAR_MIN_STEP_YARDS || yards > stepYards + 0.01f ||
        std::fabs(point.X - table.CentreX) > table.HalfSize || std::fabs(point.Y - table.CentreY) > table.HalfSize ||
        CoverInsidePillar(table, activeGates, point) || PillarCrossesWaterfall(table.MapId, self, point))
        return pick;

    if (farSideOnly)
    {
        float const pointAngle = std::atan2(point.Y - pillar.Y, point.X - pillar.X);
        float const chaserAngle = std::atan2(chaser.Y - pillar.Y, chaser.X - pillar.X);
        if (std::fabs(CoverWrapAngle(pointAngle - chaserAngle)) < PILLAR_RV_FAR_DEG * COVER_PI / 180.0f)
            return pick;
    }

    pick.Found = true;
    pick.Pillar = best;
    pick.Point = point;
    pick.Yards = yards;
    return pick;
}

// The break's fallback where no table pillar hides the bot (Lordaeron, or none in reach): points round the bot at
// stepYards and PILLAR_FALLBACK_SHORT of it, sideways from the caster first, then slanting away or back, then straight
// away (never within 60 degrees of the caster's direction); the caller rays each from the caster and checks ground
// and path
inline std::vector<CoverVec> PillarFallbackPoints(CoverVec const& self, CoverVec const& caster, float stepYards)
{
    static constexpr std::array<float, 9> OFFSETS_DEG = {90.0f,   -90.0f, 60.0f,  -60.0f, 120.0f,
                                                         -120.0f, 30.0f,  -30.0f, 0.0f};
    std::vector<CoverVec> points;
    float const away = std::atan2(self.Y - caster.Y, self.X - caster.X);
    for (float const radius : {stepYards, stepYards * PILLAR_FALLBACK_SHORT})
    {
        for (float const offset : OFFSETS_DEG)
        {
            float const angle = away + offset * COVER_PI / 180.0f;
            points.push_back({self.X + radius * std::cos(angle), self.Y + radius * std::sin(angle), self.Z});
        }
    }

    return points;
}

// A cast the bot broke LOS from: judged when its bar should have ended
struct PillarWatch
{
    bool Active = false;
    bool Known = false;  // a known break (counted), not a random hide
    uint64 Caster = 0;
    uint32 Spell = 0;
    uint32 EndMs = 0;
    uint32 FirstEndMs = 0;
};

enum class PillarCastNow : uint8
{
    Gone,     // the caster died or left
    Casting,  // the same bar still runs (pushback)
    Ended,    // the bar is over (it landed, failed or was cancelled)
};

enum class PillarVerdict : uint8
{
    Wait,
    Broken,  // ended while the caster could not see the bot: the completion's LOS check failed it
    Landed,  // ended in sight
    Lost,    // the caster is gone (or the bar ran on too long): not judged
};

inline bool PillarWatchDue(PillarWatch const& watch, uint32 nowMs)
{
    return watch.Active && static_cast<int32>(nowMs - watch.EndMs) >= static_cast<int32>(PILLAR_JUDGE_MS);
}

// One judgment of a due watch (seesBot: the caster's ray to the bot, asked only for Ended)
inline PillarVerdict PillarJudgeWatch(PillarWatch& watch, PillarCastNow castNow, uint32 remainingMs, bool seesBot,
                                      uint32 nowMs)
{
    if (!PillarWatchDue(watch, nowMs))
        return PillarVerdict::Wait;

    if (castNow == PillarCastNow::Casting && nowMs + remainingMs - watch.FirstEndMs <= PILLAR_WATCH_MAX_MS)
    {
        watch.EndMs = nowMs + remainingMs;
        return PillarVerdict::Wait;
    }

    // the caster gone, or a bar pushed back past PILLAR_WATCH_MAX_MS: not judged
    watch.Active = false;
    if (castNow != PillarCastNow::Ended)
        return PillarVerdict::Lost;

    return seesBot ? PillarVerdict::Landed : PillarVerdict::Broken;
}

enum class PillarDue : uint8
{
    None,
    Break,  // "break los"
    Step,   // "pillar step"
};

// The per-bot state (the value "pillar state"; the bot's own update, tactics show on the world thread)
struct PillarBotState
{
    PillarParams Params;
    uint32 ParamsMs = 0;
    uint32 Instance = 0;  // the battleground of the watch (another one drops it)
    PillarCastSeen Cast;
    PillarWatch Watch;
    // the trigger's answer for the action
    uint8 Due = 0;  // PillarDue
    uint32 DueMs = 0;
    uint64 DueCaster = 0;    // Break: the caster
    uint64 DueChaser = 0;    // Step: the chaser
    PillarStepPick DueStep;  // Step: the point
    bool StepGuarding = false;
    // what it did (tactics show)
    uint32 CastsSeen = 0;
    uint32 Breaks = 0;       // known breaks (losBreaks)
    uint32 RandomHides = 0;  // breaks of a bot below Known
    uint32 TooLate = 0;
    uint32 NoPoint = 0;
    uint32 Guarded = 0;
    uint32 Broken = 0;  // all watched casts (castsLosBroken counts the known ones)
    uint32 Landed = 0;
    uint32 Lost = 0;
    uint32 Steps = 0;
    uint32 StepsRefused = 0;  // a step whose ground or path failed
    uint32 FallbackPoints = 0;
    uint32 LastMoveMs = 0;
    uint32 LastSinceGatesMs = 0;  // the last break's or step's time since the gates (Ring of Valor's 21 s)
};

// A new match: no bar, no watch, no pending move
inline void PillarResetMatch(PillarBotState& state)
{
    state.Cast = PillarCastSeen();
    state.Watch = PillarWatch();
    state.Due = 0;
    state.DueMs = 0;
    state.DueCaster = 0;
    state.DueChaser = 0;
    state.StepGuarding = false;
}

// The selftest (tactics selftest pillar; the offline harness runs the same checks)
template <typename Check>
void RunPillarChecks(Check&& check)
{
    PillarConfig const config;
    auto inputs = [](uint8 skill, uint8 caution = SKILL_STYLE_NEUTRAL)
    {
        PillarGateInputs in;
        in.Skill = skill;
        in.Knowledge = skill;
        in.Caution = caution;
        in.FullAbility = skill == 0;
        return in;
    };

    // the numbers by skill and style
    PillarParams const full = ComputePillarParams(config, inputs(0, 100));
    check(full.Known && full.RandomHidePct == 0 && full.EarlyMs == 0, "full ability: known, neutral style");
    PillarParams const high = ComputePillarParams(config, inputs(80));
    check(high.Known && high.RandomHidePct == 0 && high.StepYards == 6.0f && high.HardCastMs == 1500,
          "skill 80: known, steps of 6 yd on casts of 1.5 s");
    check(ComputePillarParams(config, inputs(60)).Known && !ComputePillarParams(config, inputs(59)).Known,
          "known from 60");
    PillarParams const novice = ComputePillarParams(config, inputs(1));
    PillarParams const low = ComputePillarParams(config, inputs(20));
    PillarParams const almost = ComputePillarParams(config, inputs(59));
    check(novice.RandomHidePct == 30 && almost.RandomHidePct == 5 && low.RandomHidePct < 30 && low.RandomHidePct > 5,
          "random hides 30 % at skill 1 to 5 % at 59: " + std::to_string(low.RandomHidePct) + " % at 20");
    check(ComputePillarParams(config, inputs(80, 100)).EarlyMs == 300 &&
              ComputePillarParams(config, inputs(80, 0)).EarlyMs == -300 && high.EarlyMs == 0,
          "cautious hides 300 ms earlier, bold 300 ms later");
    LosConfig wide;
    wide.MaxRelocs = 50;
    LosParams const guards = PillarGuardParams(LosConfig());
    check(guards.HoldMs == 2500 && guards.MaxRelocs == 3 && guards.RelocWindowMs == 10000 &&
              PillarGuardParams(wide).MaxRelocs == LOS_RING,
          "guards from the Los config (relocations clamped to the ring)");

    // the bar
    check(PillarHardCast(1500, false, false, 1500) && PillarHardCast(2500, false, false, 1500) &&
              PillarHardCast(1000, true, false, 1500) && !PillarHardCast(1000, false, false, 1500) &&
              !PillarHardCast(0, true, false, 1500) && !PillarHardCast(3000, false, true, 1500),
          "hard: 1.5 s or more or crowd control; never an instant, nor a spell that ignores LOS");
    check(PillarBreakWhen(high, 2000, 860, true) == PillarWhen::Wait &&
              PillarBreakWhen(high, 1260, 860, true) == PillarWhen::Now &&
              PillarBreakWhen(high, 900, 860, true) == PillarWhen::Now &&
              PillarBreakWhen(high, 100, 860, true) == PillarWhen::TooLate,
          "known: steps when the bar has the walk plus 400 ms left");
    PillarParams const cautious = ComputePillarParams(config, inputs(80, 100));
    PillarParams const bold = ComputePillarParams(config, inputs(80, 0));
    check(PillarBreakWhen(cautious, 1500, 860, true) == PillarWhen::Now &&
              PillarBreakWhen(high, 1500, 860, true) == PillarWhen::Wait &&
              PillarBreakWhen(bold, 1100, 860, true) == PillarWhen::Wait &&
              PillarBreakWhen(bold, 1010, 860, true) == PillarWhen::Now,
          "cautious steps earlier, bold later, never later than the walk plus the slack");
    check(PillarBreakWhen(low, 2400, 860, false) == PillarWhen::Now, "a random hide steps at once");
    check(PillarArrivesInTime(1000, 850) && !PillarArrivesInTime(1000, 851), "arrival: the walk plus 150 ms");

    // the same cast, pushback, a new cast
    PillarCastSeen seen;
    seen.Caster = 5;
    seen.Spell = 116;
    seen.ElapsedMs = 1200;
    seen.SeenMs = 10000;
    check(PillarSameCast(seen, 5, 116, 1300, 10100), "the same bar a look later");
    check(PillarSameCast(seen, 5, 116, 700, 10100), "the same bar set back by pushback");
    check(!PillarSameCast(seen, 5, 116, 100, 10100) && !PillarSameCast(seen, 6, 116, 1300, 10100) &&
              !PillarSameCast(seen, 5, 117, 1300, 10100) && !PillarSameCast(seen, 5, 116, 1300, 11100),
          "a recast, another caster, another spell or a long gap is a new cast");

    // random hides: never at 0, always at 100, about the share, deterministic
    uint32 hides = 0;
    bool never = true;
    bool always = true;
    for (uint32 serial = 1; serial <= 2000; ++serial)
    {
        hides += PillarRandomHide(20, 77, serial) ? 1 : 0;
        never = never && !PillarRandomHide(0, 77, serial);
        always = always && PillarRandomHide(100, 77, serial);
    }
    check(never && always && PillarRandomHide(20, 77, 3) == PillarRandomHide(20, 77, 3),
          "random hide: never at 0 %, always at 100 %, deterministic");
    check(hides > 320 && hides < 480, "random hide at 20 %: " + std::to_string(hides) + " of 2000");

    // the shared guards: a regain 1 s ago holds the point, then 3 relocations per 10 s
    LosRelocRing ring;
    ring.Note(10000);
    check(LosRelocGuard(ring, 11000, guards) == LosGuard::Hold, "a break 1 s after a regain is held");
    ring.Note(12600);
    ring.Note(15200);
    check(LosRelocGuard(ring, 18000, guards) == LosGuard::Window, "a fourth relocation in 10 s is refused");

    // the watch: broken, landed, pushback, gone
    PillarWatch watch;
    watch.Active = true;
    watch.EndMs = 20000;
    watch.FirstEndMs = 20000;
    check(PillarJudgeWatch(watch, PillarCastNow::Ended, 0, false, 20100) == PillarVerdict::Wait && watch.Active,
          "watch: not before the end plus 250 ms");
    check(PillarJudgeWatch(watch, PillarCastNow::Casting, 600, false, 20300) == PillarVerdict::Wait &&
              watch.EndMs == 20900 && watch.Active,
          "watch: pushback moves the end");
    check(PillarJudgeWatch(watch, PillarCastNow::Ended, 0, false, 21200) == PillarVerdict::Broken && !watch.Active,
          "watch: ended out of the caster's sight: broken");
    watch.Active = true;
    watch.EndMs = 30000;
    watch.FirstEndMs = 30000;
    check(PillarJudgeWatch(watch, PillarCastNow::Ended, 0, true, 30300) == PillarVerdict::Landed,
          "watch: ended in sight: landed");
    watch.Active = true;
    watch.EndMs = 40000;
    watch.FirstEndMs = 36000;
    check(PillarJudgeWatch(watch, PillarCastNow::Casting, 500, false, 40300) == PillarVerdict::Lost && !watch.Active,
          "watch: pushback no longer than 4 s past the first end, then not judged");
    watch.Active = true;
    watch.EndMs = 50000;
    watch.FirstEndMs = 50000;
    check(PillarJudgeWatch(watch, PillarCastNow::Gone, 0, false, 50300) == PillarVerdict::Lost,
          "watch: the caster gone");
    PillarBotState state;
    state.Watch.Active = true;
    state.Cast.SeenMs = 5;
    state.Due = static_cast<uint8>(PillarDue::Break);
    PillarResetMatch(state);
    check(!state.Watch.Active && !state.Cast.SeenMs && !state.Due, "a new match drops the bar, the watch and the due");

    // Ring of Valor: all pillars up
    ArenaCoverTable const* rv = FindArenaCoverTable(COVER_MAP_RING_OF_VALOR);
    check(rv != nullptr && rv->Count == 4, "Ring of Valor's table");
    if (!rv)
        return;

    auto los = [rv](CoverVec const& a, CoverVec const& b)
    { return !CoverSegmentBlocked(*rv, COVER_ALL_GATES, a, b, 0.0f); };
    CoverPillar const& north = rv->Pillars[0];
    float const ring0 = north.R + PILLAR_RING_GAP;

    // the break: a ring point hidden from the caster within 6 yd
    CoverVec const self{north.X, north.Y + ring0, north.Z};
    CoverVec const caster{north.X + 25.0f, north.Y - 6.0f, north.Z};
    check(los(CoverEye(self, COVER_EYE_YARDS), CoverEye(caster, COVER_EYE_YARDS)), "the case: the caster sees the bot");
    CoverSearchParams search;
    search.RequireHidden = true;
    search.RingGap = PILLAR_RING_GAP;
    search.MaxRays = guards.MaxRays;
    CoverSearchResult const hide =
        FindCoverPoint(*rv, COVER_ALL_GATES, self, caster, nullptr, high.StepYards, los, search);
    check(hide.Found && hide.Hidden && hide.Moved <= high.StepYards && hide.Rays <= search.MaxRays &&
              !los(CoverEye(hide.Point, COVER_EYE_YARDS), CoverEye(caster, COVER_EYE_YARDS)),
          "break: a ring point within 6 yd that the caster cannot see");
    uint32 const lowered = COVER_ALL_GATES & ~(1u << north.Gate);
    CoverSearchResult const sunk = FindCoverPoint(*rv, lowered, self, caster, nullptr, high.StepYards, los, search);
    check(!sunk.Found || sunk.Pillar != 0, "break: a lowered pillar hides nobody");
    CoverVec const distant{north.X + 30.0f, north.Y + 30.0f, north.Z};
    check(!FindCoverPoint(*rv, COVER_ALL_GATES, distant, caster, nullptr, high.StepYards, los, search).Found,
          "break: no pillar within 6 yd, no point (the fallback's turn)");

    // the fallback round the bot: sideways first, never toward the caster, within the step
    std::vector<CoverVec> const around = PillarFallbackPoints({0.0f, 0.0f, 5.0f}, {-20.0f, 0.0f, 5.0f}, 6.0f);
    bool within = around.size() == 18;
    bool awayOrSide = true;
    for (CoverVec const& point : around)
    {
        within = within && CoverDist2d({0.0f, 0.0f, 0.0f}, point) <= 6.01f && point.Z == 5.0f;
        awayOrSide = awayOrSide && point.X >= -3.01f;
    }
    check(within && awayOrSide && std::fabs(around[0].X) < 0.01f && std::fabs(around[0].Y - 6.0f) < 0.01f,
          "fallback: 18 points within the step, sideways first, none within 60 degrees of the caster's direction");

    // the pillar step round the north pillar from a chaser (the bot on the ring 20 degrees off the chaser's side)
    CoverVec const chaser{north.X + ring0 + 4.0f, north.Y - 2.0f, north.Z};
    float const near20 = 20.0f * COVER_PI / 180.0f;
    CoverVec const runner{north.X + ring0 * std::cos(near20), north.Y + ring0 * std::sin(near20), north.Z};
    PillarStepPick const step = PickPillarStep(*rv, COVER_ALL_GATES, runner, chaser, high.StepYards, false);
    float const selfAngle = std::atan2(runner.Y - north.Y, runner.X - north.X);
    float const chaserAngle = std::atan2(chaser.Y - north.Y, chaser.X - north.X);
    float const stepAngle = std::atan2(step.Point.Y - north.Y, step.Point.X - north.X);
    check(step.Found && step.Pillar == 0 && step.Yards <= high.StepYards && step.Yards >= PILLAR_MIN_STEP_YARDS &&
              std::fabs(CoverWrapAngle(stepAngle - chaserAngle)) > std::fabs(CoverWrapAngle(selfAngle - chaserAngle)),
          "step: one hop of at most 6 yd round the pillar, away from the chaser");
    check(std::fabs(CoverDist2d(step.Point, {north.X, north.Y, 0.0f}) - ring0) < 0.01f, "step: on the ring");
    PillarStepPick const rvStep = PickPillarStep(*rv, COVER_ALL_GATES, runner, chaser, high.StepYards, true);
    PillarStepPick const shortStep = PickPillarStep(*rv, COVER_ALL_GATES, runner, chaser, 2.0f, true);
    check(rvStep.Found && !shortStep.Found && PickPillarStep(*rv, COVER_ALL_GATES, runner, chaser, 2.0f, false).Found,
          "Ring of Valor: the far side only (a short hop that stays on the chaser's side is refused)");
    CoverVec const behind{north.X - ring0, north.Y, north.Z};
    CoverVec const chaserEast{north.X + ring0 + 2.0f, north.Y, north.Z};
    check(!PickPillarStep(*rv, COVER_ALL_GATES, behind, chaserEast, high.StepYards, false).Found,
          "step: none when the pillar is between them already");
    check(!PickPillarStep(*rv, lowered, runner, chaser, high.StepYards, false).Found &&
              !PickPillarStep(*rv, COVER_ALL_GATES, distant, chaser, high.StepYards, false).Found,
          "step: none round a lowered pillar or far from every pillar");

    // Dalaran's waterfall
    check(PillarCrossesWaterfall(COVER_MAP_DALARAN, {1280.0f, 790.0f, 7.0f}, {1300.0f, 791.0f, 7.0f}) &&
              !PillarCrossesWaterfall(COVER_MAP_DALARAN, {1308.0f, 760.0f, 7.0f}, {1312.0f, 765.0f, 7.0f}) &&
              !PillarCrossesWaterfall(COVER_MAP_NAGRAND, {1280.0f, 790.0f, 7.0f}, {1300.0f, 791.0f, 7.0f}),
          "Dalaran: a walk across the waterfall is refused, beside it not, other maps never");
    ArenaCoverTable const* ds = FindArenaCoverTable(COVER_MAP_DALARAN);
    bool clear = ds != nullptr;
    if (ds)
    {
        // every ring point of Dalaran's pillars stepped from round the waterfall side: never across it
        for (uint32 index = 0; index < ds->Count; ++index)
        {
            CoverPillar const& pillar = ds->Pillars[index];
            float const dsRing = pillar.R + PILLAR_RING_GAP;
            for (uint32 k = 0; k < 16; ++k)
            {
                float const angle = COVER_TWO_PI * static_cast<float>(k) / 16.0f;
                CoverVec const from{pillar.X + dsRing * std::cos(angle), pillar.Y + dsRing * std::sin(angle), pillar.Z};
                CoverVec const by{pillar.X + 9.0f * std::cos(angle + 0.5f), pillar.Y + 9.0f * std::sin(angle + 0.5f),
                                  pillar.Z};
                PillarStepPick const pick = PickPillarStep(*ds, COVER_ALL_GATES, from, by, 6.0f, false);
                clear = clear && (!pick.Found || !PillarCrossesWaterfall(COVER_MAP_DALARAN, from, pick.Point));
            }
        }
    }
    check(clear, "Dalaran: no pillar step crosses the waterfall");

    // Ring of Valor's first 21 s (LosArenaIdle, the battleground's clock) hold every break and step
    check(LosArenaIdle(true, true, 20999, config.RvIdleMs) && !LosArenaIdle(true, true, 21000, config.RvIdleMs),
          "Ring of Valor: no move in the first 21 s after the gates");
}

#endif
