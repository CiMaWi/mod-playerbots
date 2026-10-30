/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAFOCUS_H
#define _PLAYERBOT_ARENAFOCUS_H

#include <algorithm>
#include <array>

#include "ArenaFocusConfig.h"
#include "BotAttributes.h"
#include "Common.h"
#include "TacticGate.h"
#include "Tactics.h"

// Target hold and team plan (PLAN 3.15): how long one bot keeps its target (HoldDecision, every PvP context), which
// enemy an arena team kills and which it controls (Decide), who sets the marks (ElectPlanner), when a mark may change
// (RemarkDecision) and when a bot without marks takes its partner's target (FollowPartnerDecision). Pure functions of
// plain values (ms timestamps, health percents, flags) that a human in the seat could see: no bots, no config
// singleton, no allocation, at most FOCUS_MAX_ENEMIES enemies. Their rolls hash the caller's seed (a guid) per window,
// so the same input gives the same answer. .playerbots tactics selftest focus drives them with synthetic cases.

constexpr uint8 FOCUS_MAX_ENEMIES = 3;
constexpr uint8 FOCUS_MAX_TEAM = 5;
constexpr int8 FOCUS_NONE = -1;
constexpr int32 FOCUS_FULL_HEALTH = 100;
constexpr uint8 FOCUS_FULL_ABILITY_SKILL = 100;  // skill 0 (system off) ranks above every rolled skill
// A switch's reaction cost raises the hysteresis by 1 per 100 ms
constexpr uint32 FOCUS_SWITCH_COST_MS_PER_POINT = 100;
constexpr uint32 FOCUS_EXECUTE_WINDOW_MS = 3000;  // one execute answer per bot and window
constexpr uint32 FOCUS_FOLLOW_WINDOW_MS = 10000;  // one follow answer per bot and window

// Score of a kill candidate: (100 - health) plus these (research design, drafts/phase-g/target-interrupt.md 2.3;
// tunables)
namespace FocusWeight
{
constexpr int32 Healer = 25;
constexpr int32 PerInRange = 10;  // per own bot in range
constexpr int32 PerLos = 10;      // per own bot with line of sight
constexpr int32 TrinketUsed = 15;
constexpr int32 DefensiveUp = -40;
constexpr int32 DefensiveUsed = 10;    // spent: on cooldown
constexpr int32 ImmunePhysical = -40;  // only for a mostly physical team
constexpr int32 StunnedOrFeared = -15;
}  // namespace FocusWeight

// How the traits move the parameters, by the trait's shift (trait - 50) / 50, -1 to 1 (tunables)
namespace FocusStyle
{
constexpr float PatienceHold = 0.25f;        // hold x (1 + 0.25 shift)
constexpr float TeamworkFollow = 0.5f;       // follow chance x (1 + 0.5 shift)
constexpr float TeamworkFollowDelay = 0.3f;  // follow delay x (1 - 0.3 shift)
constexpr float TeamworkDefer = 0.5f;        // a non-planner's re-mark margin x (1 + 0.5 shift)
constexpr float AggressionExecute = 10.0f;   // execute thresholds + 10 shift points
constexpr float CautionDefensive = 15.0f;    // the swap off a defensive + 15 shift points of health
}  // namespace FocusStyle

namespace FocusSalt
{
constexpr uint64 HoldExecute = 0x3C6EF372FE94F82BULL;
constexpr uint64 PlanExecute = 0xA54FF53A5F1D36F1ULL;
constexpr uint64 Follow = 0x5BE0CD19137E2179ULL;
}  // namespace FocusSalt

// One bot's target hold (HoldDecision, FollowPartnerDecision)
struct HoldParams
{
    uint32 HoldMs = 0;
    uint8 HysteresisPct = 0;  // score points; a point is one percent of health
    uint8 ExecPct = 0;
    uint8 ExecChance = 0;
    uint8 FollowChance = 0;
    uint32 FollowDelayMs = 0;
    uint32 LosLostMs = 0;

    bool operator==(HoldParams const&) const = default;
};

// The team plan's numbers, the planner's (Decide, RemarkDecision); CcCapable and PhysicalTeam come from the caller
struct TeamParams
{
    uint32 PlanIntervalMs = 0;
    uint32 HoldMs = 0;
    uint8 HysteresisPct = 0;
    uint8 ExecPct = 0;
    uint8 ExecChance = 0;
    uint8 DefensiveSwapPct = 0;
    uint8 StrongMargin = 0;
    uint32 RemarkDelayMs = 0;
    uint32 MinRemarkMs = 0;
    uint32 HumanMarkRespectMs = 0;
    uint32 LosLostMs = 0;
    bool CcCapable = false;
    bool PhysicalTeam = false;

    bool operator==(TeamParams const&) const = default;
};

struct FocusParams
{
    HoldParams Hold;
    TeamParams Team;
};

// An ability's lerp position (0, at skill 0, is full ability) and a trait's shift (0 at the neutral 50)
inline float FocusAbilityPosition(uint8 ability) { return ability == 0 ? 1.0f : SkillPosition(ability); }

inline float FocusTraitShift(uint8 trait) { return (static_cast<float>(std::min<uint8>(trait, 100)) - 50.0f) / 50.0f; }

inline uint8 FocusPercent(float value) { return static_cast<uint8>(std::clamp(value, 0.0f, 100.0f) + 0.5f); }

inline uint32 FocusScaled(uint32 value, float factor)
{
    return static_cast<uint32>(std::max(0.0f, static_cast<float>(value) * factor) + 0.5f);
}

// The parameters of one bot, cached once per skill load (G1b: SetPvpSkill). Wisdom is the lerp position (the re-mark
// delay: Speed, a reaction); the traits move them by FocusStyle. With the attribute defaults (abilities = skill,
// traits 50) the result is the plain skill lerp; skill 0 gives the skill-99 values.
inline FocusParams ComputeFocusParams(uint8 skill, BotAttributeValues const& attributes, float exponent,
                                      FocusConfig const& config)
{
    bool const full = skill == 0;
    float const wisdom = full ? 1.0f : FocusAbilityPosition(attributes.Get(BotAttribute::Wisdom));
    float const speed = full ? 1.0f : FocusAbilityPosition(attributes.Get(BotAttribute::Speed));
    auto const shift = [&](BotAttribute attribute) { return full ? 0.0f : FocusTraitShift(attributes.Get(attribute)); };
    float const aggression = shift(BotAttribute::Aggression);
    float const caution = shift(BotAttribute::Caution);
    float const patience = shift(BotAttribute::Patience);
    float const teamwork = shift(BotAttribute::Teamwork);
    auto const lerp = [&](uint32 atSkill1, uint32 atSkill99, float position)
    { return SkillLerpRounded(atSkill1, atSkill99, position, exponent); };
    auto const percent = [&](uint32 atSkill1, uint32 atSkill99, float add)
    { return FocusPercent(static_cast<float>(lerp(atSkill1, atSkill99, wisdom)) + add); };

    FocusParams params;
    HoldParams& hold = params.Hold;
    hold.HoldMs = FocusScaled(lerp(config.HoldMsAtSkill1, config.HoldMsAtSkill99, wisdom),
                              1.0f + FocusStyle::PatienceHold * patience);
    hold.HysteresisPct = percent(config.HoldHysteresisPctAtSkill1, config.HoldHysteresisPctAtSkill99, 0.0f);
    hold.ExecPct =
        percent(config.HoldExecPctAtSkill1, config.HoldExecPctAtSkill99, FocusStyle::AggressionExecute * aggression);
    hold.ExecChance = percent(config.HoldExecChanceAtSkill1, config.HoldExecChanceAtSkill99, 0.0f);
    hold.FollowChance = FocusPercent(
        static_cast<float>(lerp(config.HoldFollowChanceAtSkill1, config.HoldFollowChanceAtSkill99, wisdom)) *
        (1.0f + FocusStyle::TeamworkFollow * teamwork));
    hold.FollowDelayMs = FocusScaled(lerp(config.HoldFollowDelayMsAtSkill1, config.HoldFollowDelayMsAtSkill99, wisdom),
                                     1.0f - FocusStyle::TeamworkFollowDelay * teamwork);
    hold.LosLostMs = config.HoldLosLostMs;

    TeamParams& team = params.Team;
    team.PlanIntervalMs = lerp(config.FocusPlanIntervalMsAtSkill1, config.FocusPlanIntervalMsAtSkill99, wisdom);
    team.HoldMs = FocusScaled(lerp(config.FocusHoldMsAtSkill1, config.FocusHoldMsAtSkill99, wisdom),
                              1.0f + FocusStyle::PatienceHold * patience);
    team.HysteresisPct = percent(config.FocusHysteresisPctAtSkill1, config.FocusHysteresisPctAtSkill99, 0.0f);
    team.ExecPct =
        percent(config.FocusExecPctAtSkill1, config.FocusExecPctAtSkill99, FocusStyle::AggressionExecute * aggression);
    team.ExecChance = percent(config.FocusExecChanceAtSkill1, config.FocusExecChanceAtSkill99, 0.0f);
    team.DefensiveSwapPct =
        FocusPercent(static_cast<float>(config.FocusDefensiveSwapPct) + FocusStyle::CautionDefensive * caution);
    team.StrongMargin = static_cast<uint8>(
        std::min<uint32>(FocusScaled(lerp(config.FocusStrongMarginAtSkill1, config.FocusStrongMarginAtSkill99, wisdom),
                                     1.0f + FocusStyle::TeamworkDefer * teamwork),
                         UINT8_MAX));
    team.RemarkDelayMs = lerp(config.FocusRemarkDelayMsAtSkill1, config.FocusRemarkDelayMsAtSkill99, speed);
    team.MinRemarkMs = config.FocusMinRemarkSeconds * IN_MILLISECONDS;
    team.HumanMarkRespectMs = config.FocusHumanMarkRespectSeconds * IN_MILLISECONDS;
    team.LosLostMs = config.FocusLosLostMs;
    return params;
}

// ms timestamps (getMSTime) wrap: compare by difference
inline bool FocusBefore(uint32 nowMs, uint32 untilMs) { return static_cast<int32>(nowMs - untilMs) < 0; }

inline uint32 FocusSwitchCostPoints(uint32 switchCostMs) { return switchCostMs / FOCUS_SWITCH_COST_MS_PER_POINT; }

// Hold

enum class HoldReason : uint8
{
    NoTarget,    // allow: nothing held
    Dead,        // allow
    OutOfRange,  // allow: out of range
    LosLost,     // allow: out of sight for longer than LosLostMs
    Immune,      // allow
    Execute,     // allow: the candidate is in execute range, the hold overridden
    Free,        // allow: the hold is over and the candidate is better by the hysteresis and the switch cost
    HoldActive,  // refuse
    Hysteresis,  // refuse: not better enough
    Count
};

// What the bot sees of its current target and the candidate the target choice proposes
struct HoldInput
{
    bool HasTarget = false;
    bool TargetAlive = true;
    bool TargetInRange = true;
    bool TargetImmune = false;
    uint32 TargetLosLostMs = 0;  // how long the bot has not seen it (0 = in sight)
    uint8 TargetHpPct = FOCUS_FULL_HEALTH;
    uint32 HeldSinceMs = 0;  // when the bot took the target
    uint8 CandidateHpPct = FOCUS_FULL_HEALTH;
    bool CandidateDefensiveUp = false;
    int32 Advantage = 0;      // the candidate's score advantage in points (a choice by health: target - candidate)
    uint32 SwitchCostMs = 0;  // the reaction cost of a new target (G0h3: ReactionEase::NewTarget)
};

struct HoldResult
{
    bool Allow = false;
    HoldReason Reason = HoldReason::NoTarget;
};

// Whether the bot may leave its target for the candidate. Low skill: tunnel vision (only dead, out of range or immune,
// through the long hold, the high hysteresis and no execute chance); high skill: short hold, quick switch for a reason.
inline HoldResult HoldDecision(HoldInput const& input, uint32 nowMs, HoldParams const& params, uint64 seed)
{
    if (!input.HasTarget)
        return {true, HoldReason::NoTarget};

    if (!input.TargetAlive || input.TargetHpPct == 0)
        return {true, HoldReason::Dead};

    if (!input.TargetInRange)
        return {true, HoldReason::OutOfRange};

    if (input.TargetLosLostMs > params.LosLostMs)
        return {true, HoldReason::LosLost};

    if (input.TargetImmune)
        return {true, HoldReason::Immune};

    if (input.CandidateHpPct <= params.ExecPct && input.TargetHpPct > params.ExecPct && !input.CandidateDefensiveUp &&
        TacticUsedInWindow(seed ^ FocusSalt::HoldExecute, TacticId::Hold, params.ExecChance, FOCUS_EXECUTE_WINDOW_MS,
                           nowMs))
        return {true, HoldReason::Execute};

    if (nowMs - input.HeldSinceMs < params.HoldMs)
        return {false, HoldReason::HoldActive};

    if (input.Advantage >= static_cast<int32>(params.HysteresisPct + FocusSwitchCostPoints(input.SwitchCostMs)))
        return {true, HoldReason::Free};

    return {false, HoldReason::Hysteresis};
}

// Without marks: whether the bot takes the target its partner has had since partnerTargetSinceMs (late, by
// FollowDelayMs; by chance, one answer per FOCUS_FOLLOW_WINDOW_MS window)
inline bool FollowPartnerDecision(uint32 partnerTargetSinceMs, uint32 nowMs, HoldParams const& params, uint64 seed)
{
    if (nowMs - partnerTargetSinceMs < params.FollowDelayMs)
        return false;

    return TacticUsedInWindow(seed ^ FocusSalt::Follow, TacticId::Hold, params.FollowChance, FOCUS_FOLLOW_WINDOW_MS,
                              nowMs);
}

// Team plan

// What the team sees of one enemy (a snapshot per plan window)
struct EnemyInfo
{
    bool Alive = true;
    uint8 HpPct = FOCUS_FULL_HEALTH;
    bool Healer = false;
    bool ImmuneAll = false;
    bool ImmunePhys = false;
    bool CcBreakable = false;  // held in the team's breakable crowd control
    bool StunnedOrFeared = false;
    bool DefensiveUp = false;  // a defensive aura on it now: plain sight, always fed
    // Addon-level knowledge (DT.12): fed only by a planner at or above Playerbots.Tactics.Focus.AddonKnowledgeFromSkill
    // (FocusAddonKnowledge), from what the team saw in this match; false / 0 below it
    bool DefensiveUsed = false;  // seen up this match and gone now: spent, on cooldown
    bool TrinketUsed = false;    // seen used this match (no visible source yet: stays false until the CC step)
    uint8 LosCount = 0;          // own bots with line of sight
    uint8 InRangeCount = 0;      // own bots in range
    uint8 DrLevel = 0;     // diminishing returns of the team's crowd control on it (addon-level; 0 until the CC step)
    uint32 LosLostMs = 0;  // how long no own bot has seen it (0 = in sight)
};

enum class PlanReason : uint8
{
    None,
    NoTarget,    // no enemy can be the kill target
    Start,       // first kill target
    Dead,        // switched: the kill target died
    Immune,      // switched in the same call: the kill target is immune to everything
    Controlled,  // switched: the kill target is in the team's breakable crowd control
    Lost,        // switched: no own bot has seen it for longer than LosLostMs
    Execute,     // switched: another enemy in execute range, the hold overridden
    Defensive,   // switched: the kill target has a defensive up and another is close in health
    Score,       // switched: the hold is over and another is better by the hysteresis and the switch cost
    Held,        // kept: the hold is on
    Hysteresis,  // kept: none better enough
    Count
};

// Passed by value in and out (no state of its own); Kill and Cc are enemy indexes, FOCUS_NONE for none
struct PlanState
{
    int8 Kill = FOCUS_NONE;
    int8 Cc = FOCUS_NONE;
    uint32 HeldUntilMs = 0;
    PlanReason Reason = PlanReason::None;

    bool operator==(PlanState const&) const = default;
};

inline bool FocusAlive(EnemyInfo const& enemy) { return enemy.Alive && enemy.HpPct > 0; }

inline bool FocusLost(EnemyInfo const& enemy, uint32 losLostMs) { return enemy.LosLostMs > losLostMs; }

// An enemy held in the team's breakable crowd control is never the kill target, nor one immune to everything
inline bool FocusKillCandidate(EnemyInfo const& enemy)
{
    return FocusAlive(enemy) && !enemy.ImmuneAll && !enemy.CcBreakable;
}

inline int32 FocusKillScore(EnemyInfo const& enemy, bool physicalTeam)
{
    int32 score = FOCUS_FULL_HEALTH - std::min<int32>(enemy.HpPct, FOCUS_FULL_HEALTH);
    score += enemy.Healer ? FocusWeight::Healer : 0;
    score += FocusWeight::PerInRange * enemy.InRangeCount + FocusWeight::PerLos * enemy.LosCount;
    score += enemy.TrinketUsed ? FocusWeight::TrinketUsed : 0;
    score += enemy.DefensiveUp ? FocusWeight::DefensiveUp : 0;
    score += enemy.DefensiveUsed ? FocusWeight::DefensiveUsed : 0;
    score += enemy.ImmunePhys && physicalTeam ? FocusWeight::ImmunePhysical : 0;
    score += enemy.StunnedOrFeared ? FocusWeight::StunnedOrFeared : 0;
    return score;
}

using FocusScores = std::array<int32, FOCUS_MAX_ENEMIES>;

// The best kill candidate other than skip: one in sight before a lost one, then the higher score, then the lower index
inline int8 FocusBestKill(EnemyInfo const* enemies, uint8 count, FocusScores const& scores, uint32 losLostMs, int8 skip)
{
    int8 best = FOCUS_NONE;
    bool bestLost = true;
    for (uint8 index = 0; index < count; ++index)
    {
        int8 const candidate = static_cast<int8>(index);
        if (candidate == skip || !FocusKillCandidate(enemies[index]))
            continue;

        bool const lost = FocusLost(enemies[index], losLostMs);
        if (best == FOCUS_NONE || (bestLost && !lost) || (bestLost == lost && scores[index] > scores[best]))
        {
            best = candidate;
            bestLost = lost;
        }
    }

    return best;
}

// The crowd-control target: an alive, not immune enemy other than the kill target; a healer first, then the lowest
// diminishing returns, then the lowest index; none if the team cannot control
inline int8 FocusChooseCc(EnemyInfo const* enemies, uint8 count, int8 kill, TeamParams const& params)
{
    if (!params.CcCapable)
        return FOCUS_NONE;

    int8 best = FOCUS_NONE;
    for (uint8 index = 0; index < count; ++index)
    {
        EnemyInfo const& enemy = enemies[index];
        if (static_cast<int8>(index) == kill || !FocusAlive(enemy) || enemy.ImmuneAll)
            continue;

        if (best == FOCUS_NONE || (enemy.Healer && !enemies[best].Healer) ||
            (enemy.Healer == enemies[best].Healer && enemy.DrLevel < enemies[best].DrLevel))
            best = static_cast<int8>(index);
    }

    return best;
}

// The moon mark (Playerbots.Tactics.Focus.MoonMode, G1b-c). G1b-b's lab run: a moon that nobody controls is an enemy
// every target finder skips and the hold keeps the team off (MARK_HELD), so the healer went untouched.
constexpr uint8 FOCUS_MOON_OFF = 0;     // never a moon
constexpr uint8 FOCUS_MOON_ALWAYS = 1;  // G1b: whenever the team has a class with a CC option
constexpr uint8 FOCUS_MOON_USED = 2;    // only a team that plays CC (the Cc tactic), and only while its CC lands

// Whether the team gets a moon: the classes can control, and in mode 2 the team plays crowd control (the planner's
// own Cc tactic: what a team agrees before the match, DT.1)
inline bool FocusMoonCapable(uint8 mode, bool classCanControl, bool teamPlaysCc)
{
    return mode != FOCUS_MOON_OFF && classCanControl && (mode != FOCUS_MOON_USED || teamPlaysCc);
}

// Mode 2: the moon stays while a CC lands on it or a teammate's cast bar shows one at it (lastLandMs, set to the
// moon's mark time first); graceMs without either releases it
inline bool FocusMoonKept(uint32 lastLandMs, uint32 nowMs, uint32 graceMs) { return nowMs - lastLandMs <= graceMs; }

// The hold refuses a free switch off the skull (MARK_HELD); in mode 2 not one to the moon while it is not controlled
inline bool FocusMarkHolds(uint8 mode, bool candidateIsMoon, bool moonControlled)
{
    return mode != FOCUS_MOON_USED || !candidateIsMoon || moonControlled;
}

// The plan for this window: keep prev.Kill until prev.HeldUntilMs unless it died, turned immune, fell into the team's
// breakable crowd control or was lost from sight (each a switch in the same call), another enemy is in execute range
// (with ExecChance per window) or it has a defensive up while another is within DefensiveSwapPct health; after the hold
// a switch needs the hysteresis plus the switch cost. A switch holds the new target for HoldMs. An immune target that
// comes back returns only through the defensive rule or the score after the hold (no ping-pong).
inline PlanState Decide(EnemyInfo const* enemies, uint8 count, PlanState const& prev, uint32 nowMs,
                        TeamParams const& params, uint64 seed, uint32 switchCostMs = 0)
{
    uint8 const size = std::min(count, FOCUS_MAX_ENEMIES);
    FocusScores scores{};
    for (uint8 index = 0; index < size; ++index)
        scores[index] = FocusKillScore(enemies[index], params.PhysicalTeam);

    PlanState next = prev;
    auto const switchTo = [&](int8 kill, PlanReason reason)
    {
        next.Kill = kill;
        next.HeldUntilMs = nowMs + params.HoldMs;
        next.Reason = kill == FOCUS_NONE ? PlanReason::NoTarget : reason;
    };

    int8 const kill = prev.Kill >= 0 && prev.Kill < static_cast<int8>(size) ? prev.Kill : FOCUS_NONE;
    if (kill == FOCUS_NONE)
        switchTo(FocusBestKill(enemies, size, scores, params.LosLostMs, FOCUS_NONE), PlanReason::Start);
    else
    {
        EnemyInfo const& target = enemies[kill];
        int8 const other = FocusBestKill(enemies, size, scores, params.LosLostMs, kill);
        bool const otherSeen = other != FOCUS_NONE && !FocusLost(enemies[other], params.LosLostMs);
        int8 execute = FOCUS_NONE;
        int8 defensive = FOCUS_NONE;
        for (uint8 index = 0; index < size; ++index)
        {
            EnemyInfo const& enemy = enemies[index];
            int8 const candidate = static_cast<int8>(index);
            if (candidate == kill || !FocusKillCandidate(enemy) || FocusLost(enemy, params.LosLostMs) ||
                enemy.DefensiveUp)
                continue;

            if (enemy.HpPct <= params.ExecPct && (execute == FOCUS_NONE || enemy.HpPct < enemies[execute].HpPct))
                execute = candidate;

            if (enemy.HpPct <= target.HpPct + params.DefensiveSwapPct &&
                (defensive == FOCUS_NONE || scores[index] > scores[defensive]))
                defensive = candidate;
        }

        if (!FocusAlive(target))
            switchTo(other, PlanReason::Dead);
        else if (target.ImmuneAll)
            switchTo(other, PlanReason::Immune);
        else if (target.CcBreakable)
            switchTo(other, PlanReason::Controlled);
        else if (FocusLost(target, params.LosLostMs) && otherSeen)
            switchTo(other, PlanReason::Lost);
        else if (execute != FOCUS_NONE && target.HpPct > params.ExecPct &&
                 TacticUsedInWindow(seed ^ FocusSalt::PlanExecute, TacticId::Plan, params.ExecChance,
                                    FOCUS_EXECUTE_WINDOW_MS, nowMs))
            switchTo(execute, PlanReason::Execute);
        else if (target.DefensiveUp && defensive != FOCUS_NONE)
            switchTo(defensive, PlanReason::Defensive);
        else if (FocusBefore(nowMs, prev.HeldUntilMs))
            next.Reason = PlanReason::Held;
        else if (otherSeen && scores[other] >= scores[kill] + params.HysteresisPct +
                                                   static_cast<int32>(FocusSwitchCostPoints(switchCostMs)))
            switchTo(other, PlanReason::Score);
        else
            next.Reason = PlanReason::Hysteresis;
    }

    next.Cc = FocusChooseCc(enemies, size, next.Kill, params);
    return next;
}

// Whether a planner feeds the addon-level fields of EnemyInfo (DT.12): skill 0 (full ability) or its Knowledge at or
// above fromSkill (Playerbots.Tactics.Focus.AddonKnowledgeFromSkill)
inline bool FocusAddonKnowledge(uint8 skill, uint8 knowledge, uint32 fromSkill)
{
    return skill == 0 || knowledge >= fromSkill;
}

// Who sets the marks

struct PlannerCandidate
{
    uint64 Guid = 0;
    uint8 Skill = 0;  // 0 = full ability
    bool Alive = false;
    bool RealPlayer = false;
};

// The team's alive bot with the highest skill (ties: the lowest guid), at or above knownFromSkill (the Plan tactic's
// KnownFromSkill; 0 = every bot); none with a real player in the team: he sets the marks
inline int8 ElectPlanner(PlannerCandidate const* members, uint8 count, uint8 knownFromSkill)
{
    int8 best = FOCUS_NONE;
    uint8 bestSkill = 0;
    for (uint8 index = 0; index < std::min(count, FOCUS_MAX_TEAM); ++index)
    {
        PlannerCandidate const& member = members[index];
        if (member.RealPlayer)
            return FOCUS_NONE;

        uint8 const skill = member.Skill == 0 ? FOCUS_FULL_ABILITY_SKILL : member.Skill;
        if (!member.Alive || skill < knownFromSkill)
            continue;

        if (best == FOCUS_NONE || skill > bestSkill || (skill == bestSkill && member.Guid < members[best].Guid))
        {
            best = static_cast<int8>(index);
            bestSkill = skill;
        }
    }

    return best;
}

// When a mark may change

enum class RemarkEvent : uint8
{
    None,
    Start,       // the gates are open and the enemies seen: the first marks
    KillDead,    // the kill target died
    KillImmune,  // the kill target turned immune
    Opening,     // a big opening: a defensive spent, the healer controlled
    Count
};

enum class RemarkReason : uint8
{
    Allow,
    NoEvent,    // marks change only on events
    HumanMark,  // within HumanMarkRespectMs of a real player's mark
    TooSoon,    // within MinRemarkMs of the last mark
    Margin,     // a non-planner's disagreement is not strong enough
    Delay,      // the human delay after the event has not passed
    Count
};

struct RemarkInput
{
    RemarkEvent Event = RemarkEvent::None;
    uint32 EventAtMs = 0;  // when the bot saw the event
    bool Planner = false;
    int32 Margin = 0;     // the bot's own best score minus the marked target's
    bool Marked = false;  // the team has marks (LastMarkMs is set)
    uint32 LastMarkMs = 0;
    bool HumanMarked = false;  // a real player has marked (LastHumanMarkMs is set)
    uint32 LastHumanMarkMs = 0;
};

struct RemarkResult
{
    bool Allow = false;
    RemarkReason Reason = RemarkReason::NoEvent;
};

// Marks change only on events, at most once per MinRemarkMs, RemarkDelayMs after the event, never within
// HumanMarkRespectMs of a real player's mark; any bot other than the planner (every bot in a real player's team) only
// on a strong disagreement (Margin at least StrongMargin), so rarely
inline RemarkResult RemarkDecision(RemarkInput const& input, uint32 nowMs, TeamParams const& params)
{
    if (input.Event == RemarkEvent::None)
        return {false, RemarkReason::NoEvent};

    if (input.HumanMarked && nowMs - input.LastHumanMarkMs < params.HumanMarkRespectMs)
        return {false, RemarkReason::HumanMark};

    if (input.Marked && nowMs - input.LastMarkMs < params.MinRemarkMs)
        return {false, RemarkReason::TooSoon};

    if (!input.Planner && input.Margin < static_cast<int32>(params.StrongMargin))
        return {false, RemarkReason::Margin};

    if (nowMs - input.EventAtMs < params.RemarkDelayMs)
        return {false, RemarkReason::Delay};

    return {true, RemarkReason::Allow};
}

// The event a plan's reason is for RemarkDecision (G1a review NOTE 3). Start is the first marks; a dead or immune kill
// target is its own event; a switch for a control, a lost target, an execute or a defensive is an opening. A switch
// by score alone is no event: it changes the planner's choice, never the marks (marks change only on clear events,
// DT.1), as do the kept reasons. The caller treats a Start after the match's first marks as a re-pick (KillDead).
inline RemarkEvent EventOfPlanReason(PlanReason reason)
{
    switch (reason)
    {
        case PlanReason::Start:
            return RemarkEvent::Start;
        case PlanReason::Dead:
            return RemarkEvent::KillDead;
        case PlanReason::Immune:
            return RemarkEvent::KillImmune;
        case PlanReason::Controlled:
        case PlanReason::Lost:
        case PlanReason::Execute:
        case PlanReason::Defensive:
            return RemarkEvent::Opening;
        default:
            return RemarkEvent::None;
    }
}

// Names for .playerbots tactics show
inline char const* HoldReasonName(HoldReason reason)
{
    static constexpr std::array<char const*, static_cast<size_t>(HoldReason::Count)> names = {
        "no target", "dead", "out of range", "los lost", "immune", "execute", "free", "hold active", "hysteresis"};
    size_t const index = static_cast<size_t>(reason);
    return index < names.size() ? names[index] : "none";
}

inline char const* PlanReasonName(PlanReason reason)
{
    static constexpr std::array<char const*, static_cast<size_t>(PlanReason::Count)> names = {
        "none", "no target", "start",     "dead",  "immune", "controlled",
        "lost", "execute",   "defensive", "score", "held",   "hysteresis"};
    size_t const index = static_cast<size_t>(reason);
    return index < names.size() ? names[index] : "none";
}

inline char const* RemarkEventName(RemarkEvent event)
{
    static constexpr std::array<char const*, static_cast<size_t>(RemarkEvent::Count)> names = {
        "none", "start", "kill dead", "kill immune", "opening"};
    size_t const index = static_cast<size_t>(event);
    return index < names.size() ? names[index] : "none";
}

inline char const* RemarkReasonName(RemarkReason reason)
{
    static constexpr std::array<char const*, static_cast<size_t>(RemarkReason::Count)> names = {
        "allow", "no event", "human mark", "too soon", "margin", "delay"};
    size_t const index = static_cast<size_t>(reason);
    return index < names.size() ? names[index] : "none";
}

#endif
