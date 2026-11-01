/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAKITE_H
#define _PLAYERBOT_ARENAKITE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string>

#include "ArenaCover.h"
#include "ArenaKiteConfig.h"
#include "Common.h"
#include "PillarTactic.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// The Kite tactic's pure parts (G3a, PLAN 3.15; numbers KiteConfig, DT.3 flee off in scope, DT.9 scope arena,
// battleground and open-world PvP). A healer or caster with an enemy melee on it kites: the class snare first, then a
// step round a pillar (arena), a tested jump spot (arena, G2g-b's table), a step of 8-10 yd towards a partner that can
// peel, else a step away; each step bent away from the melee so that a kite path is no straight line. Effort and style
// (caution, patience) set how long an episode lasts before the bot gives up; below the knowledge threshold the old
// flee runs. The opponent memory's LOS breaks (where an enemy caster hid lately) make the pillar pick avoid that side
// (Foresight). The game side (KiteTacticStrategy.cpp) reads the situation, this header judges it; .playerbots tactics
// selftest kite and the offline harness run the same checks.

constexpr uint32 KITE_SENSE_MS = 1000;           // the melee, the samples and the memory rays at most this often
constexpr uint32 KITE_PARAMS_MS = 5000;          // the gate numbers are read again at most this often
constexpr uint32 KITE_TRY_MS = 500;              // the trigger offers a step at most this often
constexpr uint32 KITE_EPISODE_GAP_MS = 3000;     // no melee on the bot for this long ends the episode
constexpr float KITE_NO_MELEE = 100000.0f;       // "no melee in range"
constexpr float KITE_SAMPLE_YARDS = 30.0f;       // meleeDistX10: the nearest melee on the bot within this
constexpr uint32 KITE_SAMPLE_GAP_MAX_MS = 2000;  // meleeNearMs: a gap between two samples counts at most this
constexpr uint32 KITE_HEAL_HOLD_PCT = 35;        // a healer casting while a partner is below this keeps casting
constexpr float KITE_BEND_MIN_DEG = 15.0f;       // a step bends this much (to KITE_BEND_MAX_DEG) away from the melee
constexpr float KITE_BEND_MAX_DEG = 35.0f;
constexpr float KITE_THROUGH_DEG = 50.0f;        // no step towards a partner within this of the melee's direction
constexpr float KITE_PARTNER_KEEP_YARDS = 3.0f;  // a step towards a partner stops this short of it
constexpr float KITE_PARTNER_MAX_YARDS = 60.0f;  // a partner farther away is no help
constexpr float KITE_STEP_JITTER_YARDS = 1.0f;   // StepYards plus or minus this (8-10 yd)
constexpr float KITE_STEP_MIN_YARDS = 2.0f;
constexpr float KITE_FLANK_DEG = 35.0f;  // a pillar's far-side points: this either side of the line melee-pillar
constexpr float KITE_AVOID_PENALTY_YARDS = 8.0f;  // memory: a pillar side where an enemy hid lately costs this much
constexpr float KITE_CIRCLE_BONUS_YARDS = 2.0f;   // circling on round the pillar the bot stands at is worth this
constexpr float KITE_HIDE_PILLAR_YARDS = 6.0f;    // a LOS break is at a pillar when the enemy is this near its ring
constexpr float KITE_BREAK_MOVED_YARDS = 1.5f;    // a LOS break is the enemy's when it moved this much since the look
constexpr float KITE_FLOOR_DZ = 3.0f;             // a step's ground within this of the bot's height (pillar points:
                                                  // LOS_FLOOR_DZ of the pillar's floor)
constexpr uint32 KITE_CC_MEMORY_MS = 30000;       // a crowd control seen on the enemies longer ago is "unknown"
constexpr uint32 KITE_AFTER_CC_UNKNOWN = 0xFFFF;  // OpponentMemorySlot::LosBreakAfterCcMs "unknown"
constexpr uint32 KITE_MEMORY_SLOTS = 2;           // enemies watched for LOS breaks (one ray each per sense)
constexpr uint32 KITE_MAX_HIDES = 8;              // the opponent memory's slots
constexpr uint64 KITE_SALT_STEP = 0x9E6C63D0676A9A99ULL;
constexpr uint64 KITE_SALT_BEND = 0xD1B54A32D192ED03ULL;

// Classes (SharedDefines.h Classes) with a snare row
constexpr uint8 KITE_CLASS_PALADIN = 2;
constexpr uint8 KITE_CLASS_HUNTER = 3;
constexpr uint8 KITE_CLASS_PRIEST = 5;
constexpr uint8 KITE_CLASS_SHAMAN = 7;
constexpr uint8 KITE_CLASS_MAGE = 8;
constexpr uint8 KITE_CLASS_WARLOCK = 9;
constexpr uint8 KITE_CLASS_DRUID = 11;

// The class snares (or peels) a kiting bot tries first, in its order: instants only (the bot is moving), the spell
// names the bots' spell lookup knows. OnSelf: cast on the bot itself (an area round it or a buff); Facing: a cone in
// front, the melee must be in front of the bot. Classes without a row (warrior, rogue, death knight) are melee and do
// not kite; a priest without the Mind Flay talent's snare uses Psychic Scream, a paladin Hammer of Justice as a peel.
struct KiteSnareRow
{
    uint8 ClassId;
    char const* Spell;
    bool OnSelf;
    bool Facing;
};

inline constexpr std::array<KiteSnareRow, 12> KITE_SNARES = {{
    {KITE_CLASS_MAGE, "frost nova", true, false},  // roots everything within 10 yd
    {KITE_CLASS_MAGE, "cone of cold", false, true},
    {KITE_CLASS_SHAMAN, "frost shock", false, false},
    {KITE_CLASS_SHAMAN, "earthbind totem", true, false},
    {KITE_CLASS_DRUID, "nature's grasp", true, false},  // roots the next attacker
    {KITE_CLASS_DRUID, "typhoon", false, true},         // Balance: knocks back and dazes
    {KITE_CLASS_WARLOCK, "curse of exhaustion", false, false},
    {KITE_CLASS_WARLOCK, "death coil", false, false},  // a 3 s horror
    {KITE_CLASS_PRIEST, "psychic scream", true, false},
    {KITE_CLASS_PALADIN, "hammer of justice", false, false},
    {KITE_CLASS_HUNTER, "concussive shot", false, false},
    {KITE_CLASS_HUNTER, "wing clip", false, false},
}};

// The first row of the class the game side can cast now (can(row)); -1: none
template <typename Can>
inline int32 KitePickSnare(uint8 classId, Can&& can)
{
    for (uint32 index = 0; index < KITE_SNARES.size(); ++index)
        if (KITE_SNARES[index].ClassId == classId && can(KITE_SNARES[index]))
            return static_cast<int32>(index);

    return -1;
}

inline bool KiteClassHasSnare(uint8 classId)
{
    return KitePickSnare(classId, [](KiteSnareRow const&) { return true; }) >= 0;
}

struct KiteGateInputs
{
    uint8 Skill = 0;
    uint8 Knowledge = 0;
    uint8 Foresight = 0;
    uint8 Effort = 100;
    uint8 Caution = SKILL_STYLE_NEUTRAL;
    uint8 Patience = SKILL_STYLE_NEUTRAL;
    bool FullAbility = true;
};

// One bot's numbers, read again every KITE_PARAMS_MS
struct KiteParams
{
    bool Known = true;        // kites (below it the old flee runs)
    bool Memory = true;       // reads the opponent memory's LOS breaks (Foresight)
    uint32 GiveUpMs = 12000;  // an episode ends (the bot gives up) after this
};

// How long a bot keeps kiting one melee: below EffortMin GiveUpMsAtEffort0, else lerped over effort; caution makes it
// longer, impatience shorter (each trait's distance from neutral, together at most +-30 %); never past EpisodeMs
inline uint32 KiteGiveUpMs(KiteConfig const& config, uint8 effort, uint8 caution, uint8 patience)
{
    float base = static_cast<float>(config.GiveUpMsAtEffort0);
    if (effort >= config.EffortMin)
        base += (static_cast<float>(config.GiveUpMsAtEffort100) - static_cast<float>(config.GiveUpMsAtEffort0)) *
                std::min<float>(effort, 100.0f) / 100.0f;

    float const style = 1.0f + 0.3f *
                                   (static_cast<float>(caution) - SKILL_STYLE_NEUTRAL + static_cast<float>(patience) -
                                    SKILL_STYLE_NEUTRAL) /
                                   (2.0f * SKILL_STYLE_NEUTRAL);
    uint32 const ms = static_cast<uint32>(std::max(base * std::clamp(style, 0.7f, 1.3f), 0.0f) + 0.5f);
    return std::min(ms, config.EpisodeMs);
}

inline KiteParams ComputeKiteParams(KiteConfig const& config, KiteGateInputs const& in)
{
    bool const full = in.FullAbility || in.Skill == 0;
    KiteParams params;
    params.Known = full || in.Knowledge >= config.KnownFromSkill;
    params.Memory = params.Known && (full || in.Foresight >= config.MemoryFromSkill);
    params.GiveUpMs = full ? KiteGiveUpMs(config, 100, SKILL_STYLE_NEUTRAL, SKILL_STYLE_NEUTRAL)
                           : KiteGiveUpMs(config, in.Effort, in.Caution, in.Patience);
    return params;
}

// An enemy as the sense sees it
struct KiteMeleeView
{
    float Yards = 0.0f;
    bool Alive = true;
    bool Melee = true;        // a melee class or spec
    bool TargetsBot = true;   // its victim or its target is the bot
    bool Controlled = false;  // stunned, feared, rooted, frozen: no threat now
};

// "melee on me": an enemy melee within MeleeYards that targets the bot and is free to hit it
inline bool KiteMeleeOn(KiteConfig const& config, KiteMeleeView const& view)
{
    return view.Alive && view.Melee && view.TargetsBot && !view.Controlled &&
           view.Yards <= static_cast<float>(config.MeleeYards);
}

// meleeDistX10's sample: the nearest enemy melee that targets the bot within KITE_SAMPLE_YARDS (controlled or not)
inline bool KiteMeleeSampled(KiteMeleeView const& view)
{
    return view.Alive && view.Melee && view.TargetsBot && view.Yards <= KITE_SAMPLE_YARDS;
}

struct KiteSample
{
    bool Counted = false;
    uint32 DistX10 = 0;
    uint32 NearMs = 0;  // meleeNearMs: the time since the last sense while a melee is within MeleeYards
};

inline KiteSample KiteTakeSample(KiteConfig const& config, uint32 lastSenseMs, float nearestYards, uint32 nowMs)
{
    KiteSample sample;
    if (nearestYards > KITE_SAMPLE_YARDS)
        return sample;

    sample.Counted = true;
    sample.DistX10 = static_cast<uint32>(std::max(nearestYards, 0.0f) * 10.0f + 0.5f);
    if (nearestYards <= static_cast<float>(config.MeleeYards))
        sample.NearMs = lastSenseMs ? std::min(nowMs - lastSenseMs, KITE_SAMPLE_GAP_MAX_MS) : KITE_SENSE_MS;

    return sample;
}

// What the trigger answered last (tactics show)
enum class KiteWhy : uint8
{
    None,
    Go,
    NotKnown,
    NoMelee,
    Controlled,  // the bot itself is stunned, feared or the like
    GaveUp,
    TooSoon
};

inline char const* KiteWhyName(KiteWhy why)
{
    switch (why)
    {
        case KiteWhy::Go:
            return "go";
        case KiteWhy::NotKnown:
            return "not known";
        case KiteWhy::NoMelee:
            return "no melee";
        case KiteWhy::Controlled:
            return "controlled";
        case KiteWhy::GaveUp:
            return "given up";
        case KiteWhy::TooSoon:
            return "too soon";
        default:
            return "none";
    }
}

// One kite step's kind
enum class KiteMove : uint8
{
    None,
    Snare,
    Pillar,
    Jump,
    Partner,
    Away,
    Stay,  // nothing possible or the heal wins: keeps casting
    Panic  // below the knowledge threshold: the old flee (a straight-line run)
};

inline char const* KiteMoveName(KiteMove move)
{
    switch (move)
    {
        case KiteMove::Snare:
            return "snare";
        case KiteMove::Pillar:
            return "pillar";
        case KiteMove::Jump:
            return "jump";
        case KiteMove::Partner:
            return "partner";
        case KiteMove::Away:
            return "away";
        case KiteMove::Stay:
            return "stay";
        case KiteMove::Panic:
            return "panic";
        default:
            return "none";
    }
}

// A watched enemy (the opponent memory's feed): seen in line of sight at the last look, where
struct KiteWatch
{
    uint64 Guid = 0;  // raw guid, 0: free
    CoverVec At;
    bool Seen = false;
};

// The Kite tactic's per-bot state (PlayerbotAI::GetKiteState); GUIDs, never units
struct KiteBotState
{
    KiteParams Params;
    uint32 ParamsMs = 0;
    bool Ranged = false;  // a healer or caster (PlayerbotAI::IsRanged), read with the numbers
    uint32 Instance = 0;  // the battleground (another one starts a fresh state)
    // the sense (once per KITE_SENSE_MS)
    uint32 SenseMs = 0;
    uint64 ChaserGuid = 0;  // the nearest melee on the bot (0: none)
    float ChaserYards = 0.0f;
    float NearestMeleeYards = KITE_NO_MELEE;
    uint32 EnemyCcMs = 0;  // a crowd control seen on an enemy last (0: none)
    std::array<KiteWatch, KITE_MEMORY_SLOTS> Watch{};
    // the episode
    uint32 EpisodeStartMs = 0;  // the first sense of the running episode (0: none)
    uint32 MeleeSeenMs = 0;
    bool GaveUp = false;
    uint32 LastTryMs = 0;
    uint32 LastStepMs = 0;  // the last move step (snares do not count)
    int32 LastPillar = -1;
    bool Guarding = false;
    // the trigger's answer for the action
    bool Due = false;
    uint32 DueMs = 0;
    KiteWhy LastWhy = KiteWhy::None;
    // what it did (tactics show)
    uint32 Episodes = 0;
    uint32 GiveUps = 0;
    uint32 Steps = 0;
    uint32 Snares = 0;
    uint32 Pillars = 0;
    uint32 Jumps = 0;
    uint32 Partner = 0;
    uint32 Away = 0;
    uint32 Stays = 0;
    uint32 Guarded = 0;
    uint32 Refused = 0;  // a chosen move whose MoveTo or cast failed
    uint32 MemoryAvoided = 0;
    uint32 LosBreaksSeen = 0;
    KiteMove LastMove = KiteMove::None;
    uint32 LastMoveMs = 0;
};

// A new match (or none): no episode, no watch, nothing due
inline void KiteResetMatch(KiteBotState& state)
{
    state.SenseMs = 0;
    state.ChaserGuid = 0;
    state.ChaserYards = 0.0f;
    state.NearestMeleeYards = KITE_NO_MELEE;
    state.EnemyCcMs = 0;
    state.Watch = {};
    state.EpisodeStartMs = 0;
    state.MeleeSeenMs = 0;
    state.GaveUp = false;
    state.LastTryMs = 0;
    state.LastStepMs = 0;
    state.LastPillar = -1;
    state.Guarding = false;
    state.Due = false;
    state.DueMs = 0;
}

inline bool KiteSenseDue(KiteBotState const& state, uint32 nowMs)
{
    return !state.SenseMs || nowMs - state.SenseMs >= KITE_SENSE_MS;
}

enum class KiteEpisodeEvent : uint8
{
    None,
    Started,
    GaveUp,
    Ended
};

// The episode: starts with the first melee on the bot, gives up after GiveUpMs of it, ends when no melee was on the
// bot for KITE_EPISODE_GAP_MS (a given-up bot kites again only after that)
inline KiteEpisodeEvent KiteNoteMelee(KiteBotState& state, KiteParams const& params, bool meleeOn, uint32 nowMs)
{
    if (meleeOn)
    {
        state.MeleeSeenMs = std::max(nowMs, 1u);
        if (!state.EpisodeStartMs)
        {
            state.EpisodeStartMs = std::max(nowMs, 1u);
            state.GaveUp = false;
            ++state.Episodes;
            return KiteEpisodeEvent::Started;
        }

        if (!state.GaveUp && nowMs - state.EpisodeStartMs >= params.GiveUpMs)
        {
            state.GaveUp = true;
            ++state.GiveUps;
            return KiteEpisodeEvent::GaveUp;
        }

        return KiteEpisodeEvent::None;
    }

    if (state.EpisodeStartMs && nowMs - state.MeleeSeenMs >= KITE_EPISODE_GAP_MS)
    {
        state.EpisodeStartMs = 0;
        state.GaveUp = false;
        state.LastPillar = -1;
        return KiteEpisodeEvent::Ended;
    }

    return KiteEpisodeEvent::None;
}

// The trigger: known, a melee on the bot, the bot free to act, the episode not given up, at most every KITE_TRY_MS
inline KiteWhy KiteTriggerVerdict(KiteParams const& params, KiteBotState const& state, bool meleeOn, bool botControlled,
                                  uint32 nowMs)
{
    if (!params.Known)
        return KiteWhy::NotKnown;

    if (!meleeOn)
        return KiteWhy::NoMelee;

    if (botControlled)
        return KiteWhy::Controlled;

    if (state.GaveUp)
        return KiteWhy::GaveUp;

    if (state.LastTryMs && nowMs - state.LastTryMs < KITE_TRY_MS)
        return KiteWhy::TooSoon;

    return KiteWhy::Go;
}

// A move step at most every StepIntervalMs (a snare goes whenever one is ready)
inline bool KiteStepDue(KiteConfig const& config, KiteBotState const& state, uint32 nowMs)
{
    return !state.LastStepMs || nowMs - state.LastStepMs >= config.StepIntervalMs;
}

// A healer casting while a partner is below KITE_HEAL_HOLD_PCT keeps casting: the heal wins over the step
inline bool KiteHealHold(bool healer, bool casting, uint32 lowestPartnerPct)
{
    return healer && casting && lowestPartnerPct < KITE_HEAL_HOLD_PCT;
}

struct KitePickGate
{
    bool Known = true;
    bool HealHold = false;
    bool StepDue = true;   // StepIntervalMs since the last move step
    bool Guarded = false;  // the shared relocation guards refuse a move now
    bool SnareFirst = true;
};

// The order of a kite step: the snare (SnareFirst), then pillar, jump, partner while a move is due and not guarded,
// the snare (SnareFirst 0), a step away; else stay. can(move) is asked lazily, in this order, so the game side looks
// for a pillar point only when no snare goes, and so on.
template <typename Can>
inline KiteMove KitePick(KitePickGate const& gate, Can&& can)
{
    if (!gate.Known)
        return KiteMove::Panic;

    if (gate.HealHold)
        return KiteMove::Stay;

    if (gate.SnareFirst && can(KiteMove::Snare))
        return KiteMove::Snare;

    bool const steps = gate.StepDue && !gate.Guarded;
    if (steps)
        for (KiteMove const move : {KiteMove::Pillar, KiteMove::Jump, KiteMove::Partner})
            if (can(move))
                return move;

    if (!gate.SnareFirst && can(KiteMove::Snare))
        return KiteMove::Snare;

    if (steps && can(KiteMove::Away))
        return KiteMove::Away;

    return KiteMove::Stay;
}

// A number in [0, 1) from a hash
inline float KiteUnit(uint64 hash) { return static_cast<float>(SplitMix64(hash) >> 40) / 16777216.0f; }

// One step's length: StepYards plus or minus KITE_STEP_JITTER_YARDS (8-10 yd with the default 9)
inline float KiteStepYards(KiteConfig const& config, uint64 hash)
{
    float const yards =
        static_cast<float>(config.StepYards) + (2.0f * KiteUnit(hash ^ KITE_SALT_STEP) - 1.0f) * KITE_STEP_JITTER_YARDS;
    return std::max(yards, KITE_STEP_MIN_YARDS);
}

// How far a step bends away from the melee (degrees, KITE_BEND_MIN_DEG to KITE_BEND_MAX_DEG)
inline float KiteBendDeg(uint64 hash)
{
    return KITE_BEND_MIN_DEG + (KITE_BEND_MAX_DEG - KITE_BEND_MIN_DEG) * KiteUnit(hash ^ KITE_SALT_BEND);
}

struct KiteStepPoint
{
    bool Found = false;
    CoverVec Point;
    float Yards = 0.0f;
    float BendDeg = 0.0f;
};

// A step towards a partner at least partnerMinYards away (so that it can peel), bent away from the melee; none when the
// partner stands behind the melee (within KITE_THROUGH_DEG of its direction). The step stops KITE_PARTNER_KEEP_YARDS
// short of the partner. Z stays the bot's (the game side finds the ground).
inline KiteStepPoint KitePartnerPoint(CoverVec const& self, CoverVec const& partner, CoverVec const& chaser,
                                      float stepYards, float partnerMinYards, uint64 hash)
{
    KiteStepPoint step;
    float const toPartner = CoverDist2d(self, partner);
    if (toPartner < partnerMinYards || toPartner > KITE_PARTNER_MAX_YARDS)
        return step;

    float const partnerAngle = std::atan2(partner.Y - self.Y, partner.X - self.X);
    float const chaserAngle = std::atan2(chaser.Y - self.Y, chaser.X - self.X);
    float const off = CoverWrapAngle(partnerAngle - chaserAngle);
    if (std::fabs(off) < KITE_THROUGH_DEG * COVER_PI / 180.0f)
        return step;

    float const bend = KiteBendDeg(hash);
    float const angle = partnerAngle + (off >= 0.0f ? 1.0f : -1.0f) * bend * COVER_PI / 180.0f;
    float const yards = std::min(stepYards, toPartner - KITE_PARTNER_KEEP_YARDS);
    if (yards < KITE_STEP_MIN_YARDS)
        return step;

    step.Found = true;
    step.Point = {self.X + yards * std::cos(angle), self.Y + yards * std::sin(angle), self.Z};
    step.Yards = yards;
    step.BendDeg = bend;
    return step;
}

// A step away from the melee, bent to one side (the hash picks it)
inline KiteStepPoint KiteAwayPoint(CoverVec const& self, CoverVec const& chaser, float stepYards, uint64 hash)
{
    float const chaserAngle = std::atan2(chaser.Y - self.Y, chaser.X - self.X);
    float const bend = KiteBendDeg(hash);
    float const sideSign = (SplitMix64(hash) & 1) ? 1.0f : -1.0f;
    float const angle = chaserAngle + COVER_PI + sideSign * bend * COVER_PI / 180.0f;
    KiteStepPoint step;
    step.Found = stepYards >= KITE_STEP_MIN_YARDS;
    step.Point = {self.X + stepYards * std::cos(angle), self.Y + stepYards * std::sin(angle), self.Z};
    step.Yards = stepYards;
    step.BendDeg = bend;
    return step;
}

// The side of a pillar a point lies on, seen from the bot: -1 left, 1 right of the line bot -> pillar (0 on it)
inline int32 KiteSide(CoverVec const& self, CoverPillar const& pillar, CoverVec const& point)
{
    float const cross = (pillar.X - self.X) * (point.Y - pillar.Y) - (pillar.Y - self.Y) * (point.X - pillar.X);
    return cross > 0.01f ? 1 : (cross < -0.01f ? -1 : 0);
}

// The active pillar whose ring is within KITE_HIDE_PILLAR_YARDS of a point, the nearest; -1: none
inline int32 KiteNearestPillar(ArenaCoverTable const& table, uint32 activeGates, CoverVec const& at)
{
    int32 best = -1;
    float bestOff = KITE_HIDE_PILLAR_YARDS;
    for (uint32 index = 0; index < table.Count; ++index)
    {
        CoverPillar const& pillar = table.Pillars[index];
        if (!CoverPillarActive(pillar, activeGates))
            continue;

        float const off = std::max(std::hypot(at.X - pillar.X, at.Y - pillar.Y) - pillar.R, 0.0f);
        if (off <= bestOff)
        {
            bestOff = off;
            best = static_cast<int32>(index);
        }
    }

    return best;
}

// Where an enemy hid lately: the opponent memory's LOS breaks at a pillar within windowMs
struct KiteHide
{
    int32 Pillar = -1;
    int32 Side = 0;
};

template <typename Memory>
inline uint32 KiteHidesFrom(Memory const& memory, uint32 nowMs, uint32 windowMs, KiteHide* out, uint32 capacity)
{
    uint32 count = 0;
    for (auto const& slot : memory.Slots)
    {
        if (count >= capacity)
            break;

        if (slot.Guid != 0 && slot.LosBreakMs != 0 && nowMs - slot.LosBreakMs <= windowMs && slot.LosBreakPillar >= 0)
            out[count++] = {slot.LosBreakPillar, slot.LosBreakSide};
    }

    return count;
}

// The look at a watched enemy: still seen (keep), out of sight after it moved (its LOS break), else drop the watch
enum class KiteLook : uint8
{
    Keep,
    Break,
    Drop
};

inline KiteLook KiteJudgeLook(KiteWatch const& watch, CoverVec const& nowAt, bool visible)
{
    if (visible)
        return KiteLook::Keep;

    if (watch.Seen && CoverDist2d(watch.At, nowAt) >= KITE_BREAK_MOVED_YARDS)
        return KiteLook::Break;

    return KiteLook::Drop;
}

// The ms since the last crowd control seen on the enemies, KITE_AFTER_CC_UNKNOWN when none within KITE_CC_MEMORY_MS
inline uint32 KiteAfterCcMs(uint32 enemyCcMs, uint32 nowMs)
{
    if (!enemyCcMs || nowMs - enemyCcMs > KITE_CC_MEMORY_MS)
        return KITE_AFTER_CC_UNKNOWN;

    return nowMs - enemyCcMs;
}

struct KitePillarPick
{
    bool Found = false;
    int32 Pillar = -1;
    CoverVec Point;
    float Yards = 0.0f;
    int32 Side = 0;
    bool Circle = false;   // circling on round the pillar the bot stands at (PickPillarStep)
    bool Avoided = false;  // the memory's penalty bore on the pick (a side where an enemy hid lately)
};

// The kite's pillar point: circling on round the pillar the bot stands at (G2f's PickPillarStep, stepYards), or a
// point on the far side of a pillar in reach (KITE_FLANK_DEG either side of the line melee -> pillar, at most maxYards,
// never closer to the melee than the bot is now). Each is scored by its length, minus KITE_CIRCLE_BONUS_YARDS for
// circling, plus KITE_AVOID_PENALTY_YARDS on a pillar side where an enemy hid lately (hides: the memory's, Foresight);
// the lowest wins. farSideOnly: Ring of Valor's rule of PickPillarStep.
inline KitePillarPick PickKitePillar(ArenaCoverTable const& table, uint32 activeGates, CoverVec const& self,
                                     CoverVec const& chaser, float stepYards, float maxYards, bool farSideOnly,
                                     KiteHide const* hides, uint32 hideCount)
{
    KitePillarPick best;
    float bestScore = 0.0f;
    bool anyAvoided = false;
    auto const consider = [&](int32 index, CoverVec const& point, float yards, bool circle)
    {
        CoverPillar const& pillar = table.Pillars[index];
        int32 const side = KiteSide(self, pillar, point);
        bool avoided = false;
        for (uint32 hide = 0; hide < hideCount; ++hide)
            if (hides[hide].Pillar == index && (hides[hide].Side == 0 || hides[hide].Side == side))
                avoided = true;

        anyAvoided = anyAvoided || avoided;
        float const score =
            yards + (avoided ? KITE_AVOID_PENALTY_YARDS : 0.0f) - (circle ? KITE_CIRCLE_BONUS_YARDS : 0.0f);
        if (best.Found && score >= bestScore)
            return;

        bestScore = score;
        best.Found = true;
        best.Pillar = index;
        best.Point = point;
        best.Yards = yards;
        best.Side = side;
        best.Circle = circle;
    };

    PillarStepPick const circling = PickPillarStep(table, activeGates, self, chaser, stepYards, farSideOnly);
    if (circling.Found)
        consider(circling.Pillar, circling.Point, circling.Yards, true);

    float const chaserYards = CoverDist2d(self, chaser);
    for (uint32 index = 0; index < table.Count; ++index)
    {
        CoverPillar const& pillar = table.Pillars[index];
        if (!CoverPillarActive(pillar, activeGates))
            continue;

        float const ring = pillar.R + PILLAR_RING_GAP;
        if (std::hypot(self.X - pillar.X, self.Y - pillar.Y) - ring > maxYards)
            continue;

        float const away = std::atan2(pillar.Y - chaser.Y, pillar.X - chaser.X);
        for (float const sideSign : {-1.0f, 1.0f})
        {
            float const angle = away + sideSign * KITE_FLANK_DEG * COVER_PI / 180.0f;
            CoverVec const point{pillar.X + ring * std::cos(angle), pillar.Y + ring * std::sin(angle), pillar.Z};
            float const yards = CoverDist2d(self, point);
            if (yards < PILLAR_MIN_STEP_YARDS || yards > maxYards ||
                std::fabs(point.X - table.CentreX) > table.HalfSize ||
                std::fabs(point.Y - table.CentreY) > table.HalfSize || CoverInsidePillar(table, activeGates, point) ||
                PillarCrossesWaterfall(table.MapId, self, point) || CoverDist2d(point, chaser) < chaserYards)
                continue;

            consider(static_cast<int32>(index), point, yards, false);
        }
    }

    best.Avoided = best.Found && anyAvoided;
    return best;
}

// The selftest (tactics selftest kite; the offline harness runs the same checks)
template <typename Check>
void RunKiteChecks(Check&& check)
{
    KiteConfig const config;
    auto inputs =
        [](uint8 skill, uint8 effort = 100, uint8 caution = SKILL_STYLE_NEUTRAL, uint8 patience = SKILL_STYLE_NEUTRAL)
    {
        KiteGateInputs in;
        in.Skill = skill;
        in.Knowledge = skill;
        in.Foresight = skill;
        in.Effort = effort;
        in.Caution = caution;
        in.Patience = patience;
        in.FullAbility = false;
        return in;
    };

    // Gates by skill (knowledge and foresight)
    KiteGateInputs fullIn = inputs(0, 0, 0, 0);
    fullIn.FullAbility = true;
    KiteParams const full = ComputeKiteParams(config, fullIn);
    check(full.Known && full.Memory && full.GiveUpMs == config.GiveUpMsAtEffort100,
          "full ability kites, reads the memory and keeps on as effort 100");
    check(!ComputeKiteParams(config, inputs(39)).Known && ComputeKiteParams(config, inputs(40)).Known,
          "known from KnownFromSkill (40)");
    check(!ComputeKiteParams(config, inputs(59)).Memory && ComputeKiteParams(config, inputs(60)).Memory,
          "the memory read from MemoryFromSkill (60, Foresight)");
    check(!ComputeKiteParams(config, inputs(30)).Memory, "no memory without the tactic");
    check(config.PanicBelowSkill == config.KnownFromSkill, "panic (the old flee) exactly below the knowledge");

    // Give-up time by effort and style
    check(KiteGiveUpMs(config, 0, 50, 50) == 3000 && KiteGiveUpMs(config, 19, 50, 50) == 3000,
          "below EffortMin (20) the bot gives up after GiveUpMsAtEffort0 (3 s)");
    check(KiteGiveUpMs(config, 100, 50, 50) == 12000, "effort 100 gives up after 12 s");
    check(KiteGiveUpMs(config, 50, 50, 50) == 7500, "effort 50: half way (7.5 s)");
    check(KiteGiveUpMs(config, 100, 100, 100) == 15600 && KiteGiveUpMs(config, 100, 0, 0) == 8400,
          "caution and patience scale it +-30 %");
    check(KiteGiveUpMs(config, 100, 100, 0) == 12000, "a cautious but impatient bot evens out");
    KiteConfig shortEpisode;
    shortEpisode.EpisodeMs = 10000;
    check(KiteGiveUpMs(shortEpisode, 100, 100, 100) == 10000, "never past EpisodeMs");
    check(ComputeKiteParams(config, inputs(80, 10)).GiveUpMs == 3000 &&
              ComputeKiteParams(config, inputs(80, 100, 100, 100)).GiveUpMs == 15600,
          "the params carry the give-up time");

    // The trigger's melee
    KiteMeleeView melee;
    melee.Yards = 5.0f;
    check(KiteMeleeOn(config, melee), "a free melee at 5 yd that targets the bot is on it");
    KiteMeleeView view = melee;
    view.Yards = 8.5f;
    check(!KiteMeleeOn(config, view) && KiteMeleeSampled(view), "beyond MeleeYards (8): not on it, still sampled");
    view = melee;
    view.TargetsBot = false;
    check(!KiteMeleeOn(config, view) && !KiteMeleeSampled(view), "a melee on someone else is neither");
    view = melee;
    view.Controlled = true;
    check(!KiteMeleeOn(config, view) && KiteMeleeSampled(view), "a stunned or rooted melee is no threat, but sampled");
    view = melee;
    view.Melee = false;
    check(!KiteMeleeOn(config, view), "a ranged enemy is no melee");
    view = melee;
    view.Alive = false;
    check(!KiteMeleeOn(config, view) && !KiteMeleeSampled(view), "a dead melee is neither");
    view = melee;
    view.Yards = 31.0f;
    check(!KiteMeleeSampled(view), "beyond 30 yd no sample");

    // Samples
    KiteSample sample = KiteTakeSample(config, 10000, 4.26f, 11000);
    check(sample.Counted && sample.DistX10 == 43 && sample.NearMs == 1000, "a sample: distance x10, 1 s near");
    sample = KiteTakeSample(config, 10000, 12.0f, 11000);
    check(sample.Counted && sample.DistX10 == 120 && sample.NearMs == 0, "a melee at 12 yd: sampled, not near");
    sample = KiteTakeSample(config, 10000, 3.0f, 19000);
    check(sample.NearMs == KITE_SAMPLE_GAP_MAX_MS, "a long gap counts at most 2 s near");
    sample = KiteTakeSample(config, 0, 3.0f, 19000);
    check(sample.NearMs == KITE_SENSE_MS, "the first sample counts one sense");
    check(!KiteTakeSample(config, 10000, KITE_NO_MELEE, 11000).Counted, "no melee: no sample");

    // The episode
    KiteParams params = ComputeKiteParams(config, inputs(80, 50));
    KiteBotState state;
    check(KiteNoteMelee(state, params, true, 1000) == KiteEpisodeEvent::Started && state.Episodes == 1,
          "the first melee starts an episode");
    check(KiteTriggerVerdict(params, state, true, false, 1000) == KiteWhy::Go, "the trigger goes");
    state.LastTryMs = 1000;
    check(KiteTriggerVerdict(params, state, true, false, 1200) == KiteWhy::TooSoon, "at most every 500 ms");
    check(KiteTriggerVerdict(params, state, true, true, 1600) == KiteWhy::Controlled, "not while the bot is stunned");
    check(KiteNoteMelee(state, params, true, 1000 + params.GiveUpMs - 1) == KiteEpisodeEvent::None,
          "no give-up before GiveUpMs");
    check(KiteNoteMelee(state, params, true, 1000 + params.GiveUpMs) == KiteEpisodeEvent::GaveUp && state.GaveUp,
          "effort 50 gives up after 7.5 s");
    check(KiteTriggerVerdict(params, state, true, false, 20000) == KiteWhy::GaveUp, "a given-up bot does not kite");
    uint32 const lastMelee = 1000 + params.GiveUpMs;
    check(KiteNoteMelee(state, params, false, lastMelee + 2999) == KiteEpisodeEvent::None && state.GaveUp,
          "a short gap does not end the episode");
    check(KiteNoteMelee(state, params, false, lastMelee + 3000) == KiteEpisodeEvent::Ended && !state.GaveUp,
          "3 s without a melee end it");
    check(KiteNoteMelee(state, params, true, lastMelee + 5000) == KiteEpisodeEvent::Started && state.Episodes == 2,
          "the next melee starts a new episode");
    check(KiteTriggerVerdict(params, state, false, false, lastMelee + 6000) == KiteWhy::NoMelee, "no melee, no kite");
    KiteParams unknown = ComputeKiteParams(config, inputs(30));
    check(KiteTriggerVerdict(unknown, state, true, false, lastMelee + 6000) == KiteWhy::NotKnown,
          "below the knowledge the trigger stays off (the old flee runs)");
    KiteParams highEffort = ComputeKiteParams(config, inputs(80, 100));
    KiteBotState eager;
    KiteNoteMelee(eager, highEffort, true, 1000);
    check(KiteNoteMelee(eager, highEffort, true, 1000 + 7500) == KiteEpisodeEvent::None &&
              KiteNoteMelee(eager, highEffort, true, 1000 + 12000) == KiteEpisodeEvent::GaveUp,
          "effort 100 tries 12 s");
    KiteBotState reset = state;
    KiteResetMatch(reset);
    check(!reset.EpisodeStartMs && !reset.GaveUp && !reset.ChaserGuid && reset.Episodes == state.Episodes,
          "a new match drops the episode, keeps the totals");
    check(KiteStepDue(config, reset, 5000), "the first step is due at once");
    reset.LastStepMs = 5000;
    check(!KiteStepDue(config, reset, 6499) && KiteStepDue(config, reset, 6500), "a step at most every 1.5 s");

    // The pick order
    auto pickWith = [](KitePickGate const& gate, std::initializer_list<KiteMove> possible)
    {
        return KitePick(
            gate, [&](KiteMove move) { return std::find(possible.begin(), possible.end(), move) != possible.end(); });
    };
    KitePickGate gate;
    std::initializer_list<KiteMove> const all = {KiteMove::Snare, KiteMove::Pillar, KiteMove::Jump, KiteMove::Partner,
                                                 KiteMove::Away};
    check(pickWith(gate, all) == KiteMove::Snare, "the snare first");
    check(pickWith(gate, {KiteMove::Pillar, KiteMove::Jump, KiteMove::Partner, KiteMove::Away}) == KiteMove::Pillar,
          "then the pillar step");
    check(pickWith(gate, {KiteMove::Jump, KiteMove::Partner, KiteMove::Away}) == KiteMove::Jump, "then a jump spot");
    check(pickWith(gate, {KiteMove::Partner, KiteMove::Away}) == KiteMove::Partner, "then the partner");
    check(pickWith(gate, {KiteMove::Away}) == KiteMove::Away, "then a step away");
    check(pickWith(gate, {}) == KiteMove::Stay, "nothing possible: stay");
    KitePickGate guarded = gate;
    guarded.Guarded = true;
    check(pickWith(guarded, {KiteMove::Pillar, KiteMove::Partner, KiteMove::Away}) == KiteMove::Stay &&
              pickWith(guarded, all) == KiteMove::Snare,
          "the relocation guards stop the moves, not the snare");
    KitePickGate notDue = gate;
    notDue.StepDue = false;
    check(pickWith(notDue, {KiteMove::Pillar, KiteMove::Away}) == KiteMove::Stay, "no move before the interval");
    KitePickGate snareLast = gate;
    snareLast.SnareFirst = false;
    check(pickWith(snareLast, all) == KiteMove::Pillar &&
              pickWith(snareLast, {KiteMove::Snare, KiteMove::Away}) == KiteMove::Snare,
          "SnareFirst 0: the moves first, the snare before the step away");
    KitePickGate hold = gate;
    hold.HealHold = true;
    check(pickWith(hold, all) == KiteMove::Stay, "a healer casting on a low partner keeps casting");
    KitePickGate low = gate;
    low.Known = false;
    check(pickWith(low, all) == KiteMove::Panic, "below the knowledge: panic (the old flee)");
    check(KiteHealHold(true, true, 34) && !KiteHealHold(true, true, 35) && !KiteHealHold(true, false, 10) &&
              !KiteHealHold(false, true, 10),
          "the heal hold: a casting healer, a partner below 35 %");
    uint32 asked = 0;
    KitePick(gate,
             [&](KiteMove move)
             {
                 ++asked;
                 return move == KiteMove::Snare;
             });
    check(asked == 1, "a snare that goes asks for no pillar point");

    // The snare rows
    check(!KiteClassHasSnare(1) && !KiteClassHasSnare(4) && !KiteClassHasSnare(6),
          "warrior, rogue and death knight have no snare row");
    check(KiteClassHasSnare(KITE_CLASS_MAGE) && KiteClassHasSnare(KITE_CLASS_PRIEST) &&
              KiteClassHasSnare(KITE_CLASS_DRUID) && KiteClassHasSnare(KITE_CLASS_SHAMAN) &&
              KiteClassHasSnare(KITE_CLASS_WARLOCK) && KiteClassHasSnare(KITE_CLASS_PALADIN) &&
              KiteClassHasSnare(KITE_CLASS_HUNTER),
          "every kiting class has a row");
    int32 const nova = KitePickSnare(KITE_CLASS_MAGE, [](KiteSnareRow const&) { return true; });
    check(nova >= 0 && std::string(KITE_SNARES[nova].Spell) == "frost nova" && KITE_SNARES[nova].OnSelf,
          "a mage tries Frost Nova first");
    int32 const cone =
        KitePickSnare(KITE_CLASS_MAGE, [](KiteSnareRow const& row) { return std::string(row.Spell) != "frost nova"; });
    check(cone >= 0 && std::string(KITE_SNARES[cone].Spell) == "cone of cold" && KITE_SNARES[cone].Facing,
          "Frost Nova on cooldown: Cone of Cold (needs facing)");
    check(KitePickSnare(KITE_CLASS_MAGE, [](KiteSnareRow const&) { return false; }) == -1, "none ready: no snare");

    // Step sizes and bends
    bool sizes = true;
    bool bends = true;
    float smallest = 100.0f;
    float largest = 0.0f;
    for (uint64 hash = 1; hash < 2000; ++hash)
    {
        float const yards = KiteStepYards(config, hash * 7919);
        float const bend = KiteBendDeg(hash * 104729);
        sizes = sizes && yards >= 8.0f && yards <= 10.0f;
        bends = bends && bend >= KITE_BEND_MIN_DEG && bend <= KITE_BEND_MAX_DEG;
        smallest = std::min(smallest, yards);
        largest = std::max(largest, yards);
    }
    check(sizes && smallest < 8.2f && largest > 9.8f, "steps are 8-10 yd and use the range");
    check(bends, "bends are 15-35 degrees");

    // The partner step: bent away from the melee, never through it
    CoverVec const self{0.0f, 0.0f, 10.0f};
    CoverVec const chaser{-4.0f, 0.0f, 10.0f};
    KiteStepPoint step = KitePartnerPoint(self, {20.0f, 0.0f, 10.0f}, chaser, 9.0f, 12.0f, 42);
    float const stepAngle = std::atan2(step.Point.Y, step.Point.X) * 180.0f / COVER_PI;
    check(step.Found && std::fabs(step.Yards - 9.0f) < 0.01f && std::fabs(CoverDist2d(self, step.Point) - 9.0f) < 0.01f,
          "a partner 20 yd away: a 9 yd step");
    check(std::fabs(stepAngle) >= KITE_BEND_MIN_DEG - 0.01f && std::fabs(stepAngle) <= KITE_BEND_MAX_DEG + 0.01f,
          "the step bends 15-35 degrees off the straight line");
    check(CoverDist2d(step.Point, chaser) > CoverDist2d(self, chaser) + 5.0f, "the step gains distance on the melee");
    check(!KitePartnerPoint(self, {10.0f, 0.0f, 10.0f}, chaser, 9.0f, 12.0f, 42).Found,
          "a partner nearer than PartnerMinYards (12): no step");
    check(!KitePartnerPoint(self, {-20.0f, 1.0f, 10.0f}, chaser, 9.0f, 12.0f, 42).Found,
          "a partner behind the melee: no step through it");
    check(!KitePartnerPoint(self, {70.0f, 0.0f, 10.0f}, chaser, 9.0f, 12.0f, 42).Found, "a partner 70 yd away: none");
    step = KitePartnerPoint(self, {0.0f, 13.0f, 10.0f}, chaser, 9.0f, 12.0f, 7);
    check(step.Found && step.Yards <= 10.0f + 0.01f && step.Point.X > 0.0f,
          "a partner to the side: the step stops short of it and bends away from the melee");
    KiteStepPoint const away = KiteAwayPoint(self, chaser, 9.0f, 99);
    check(away.Found && CoverDist2d(away.Point, chaser) > 12.0f && away.Point.X > 0.0f,
          "the step away leaves the melee behind");
    bool sidesBoth = false;
    float const firstY = KiteAwayPoint(self, chaser, 9.0f, 1).Point.Y;
    for (uint64 hash = 2; hash < 40 && !sidesBoth; ++hash)
        sidesBoth = (KiteAwayPoint(self, chaser, 9.0f, hash).Point.Y > 0.0f) != (firstY > 0.0f);
    check(sidesBoth, "steps away bend to both sides");

    // Pillars: far side, circling, the memory's avoided side
    std::array<CoverPillar, 2> const pillars = {{{0.0f, 0.0f, 10.0f, 2.0f, 0}, {20.0f, 0.0f, 10.0f, 2.0f, 0}}};
    ArenaCoverTable const table{1, "kite test", 10.0f, 0.0f, 10.0f, 40.0f, pillars.data(), 2};
    CoverVec const before{-7.0f, 0.0f, 10.0f};
    CoverVec const behind{-11.0f, 0.0f, 10.0f};
    KitePillarPick pick = PickKitePillar(table, 1, before, behind, 9.0f, 12.0f, false, nullptr, 0);
    check(pick.Found && pick.Pillar == 0 && !pick.Circle && pick.Point.X > 0.0f && pick.Yards <= 12.0f,
          "a pillar in reach: its far side");
    check(CoverDist2d(pick.Point, behind) > CoverDist2d(before, behind), "the pillar point leaves the melee behind");
    check(!PickKitePillar(table, 0, before, behind, 9.0f, 12.0f, false, nullptr, 0).Found,
          "an inactive pillar (gate down) is no cover");
    check(!PickKitePillar(table, 1, before, behind, 9.0f, 5.0f, false, nullptr, 0).Found,
          "no pillar within PillarMaxYards: none");
    KiteHide const hideHere{pick.Pillar, pick.Side};
    KitePillarPick const other = PickKitePillar(table, 1, before, behind, 9.0f, 12.0f, false, &hideHere, 1);
    check(other.Found && other.Pillar == 0 && other.Side == -pick.Side && other.Avoided,
          "Foresight: the side where an enemy hid lately is avoided");
    std::array<KiteHide, 2> const both = {{{0, -1}, {0, 1}}};
    check(PickKitePillar(table, 1, before, behind, 9.0f, 12.0f, false, both.data(), 2).Found,
          "both sides avoided: the pillar still serves");
    CoverVec const onRing{-3.5f, 0.0f, 10.0f};
    KitePillarPick const circle =
        PickKitePillar(table, 1, onRing, CoverVec{-9.0f, 1.0f, 10.0f}, 9.0f, 12.0f, false, nullptr, 0);
    check(circle.Found && circle.Circle && circle.Pillar == 0, "at a pillar's ring: circling on wins");
    check(KiteSide(before, pillars[0], {3.0f, 2.0f, 10.0f}) == 1 &&
              KiteSide(before, pillars[0], {3.0f, -2.0f, 10.0f}) == -1,
          "the side of a point behind a pillar");

    // The memory feed
    check(KiteNearestPillar(table, 1, {0.0f, 7.0f, 10.0f}) == 0 &&
              KiteNearestPillar(table, 1, {10.0f, 0.0f, 10.0f}) == -1,
          "a LOS break at a pillar: within 6 yd of its ring");
    KiteWatch watch;
    watch.Guid = 5;
    watch.At = {0.0f, 8.0f, 10.0f};
    watch.Seen = true;
    check(KiteJudgeLook(watch, {1.0f, 8.0f, 10.0f}, true) == KiteLook::Keep, "still seen: keep");
    check(KiteJudgeLook(watch, {0.0f, 5.0f, 10.0f}, false) == KiteLook::Break, "moved out of sight: its LOS break");
    check(KiteJudgeLook(watch, {0.5f, 8.0f, 10.0f}, false) == KiteLook::Drop,
          "out of sight without moving (the bot moved): no break");
    watch.Seen = false;
    check(KiteJudgeLook(watch, {0.0f, 3.0f, 10.0f}, false) == KiteLook::Drop, "never seen: no break");
    check(KiteAfterCcMs(0, 5000) == KITE_AFTER_CC_UNKNOWN && KiteAfterCcMs(4000, 5000) == 1000 &&
              KiteAfterCcMs(4000, 4000 + KITE_CC_MEMORY_MS + 1) == KITE_AFTER_CC_UNKNOWN,
          "the time after a crowd control, unknown when none or old");
    struct FakeSlot
    {
        uint32 Guid = 0;
        uint32 LosBreakMs = 0;
        int8 LosBreakPillar = -1;
        int8 LosBreakSide = 0;
    };
    struct FakeMemory
    {
        std::array<FakeSlot, 4> Slots{};
    } memory;
    memory.Slots[0] = {11, 9000, 1, -1};
    memory.Slots[1] = {12, 1000, 0, 1};   // too old
    memory.Slots[2] = {13, 9500, -1, 0};  // no pillar
    memory.Slots[3] = {14, 9800, 0, 1};
    std::array<KiteHide, KITE_MAX_HIDES> hides{};
    uint32 const hideCount = KiteHidesFrom(memory, 10000, 5000, hides.data(), KITE_MAX_HIDES);
    check(hideCount == 2 && hides[0].Pillar == 1 && hides[0].Side == -1 && hides[1].Pillar == 0,
          "the memory's recent breaks at pillars become hides");
}

#endif
