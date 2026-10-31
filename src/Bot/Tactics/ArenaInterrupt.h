/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAINTERRUPT_H
#define _PLAYERBOT_ARENAINTERRUPT_H

#include <algorithm>
#include <array>
#include <cmath>

#include "ArenaInterruptConfig.h"
#include "Common.h"
#include "NaturalReaction.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// The Interrupt tactic (G5b, PLAN 3.15): which enemy cast gets the bot's kick and when. Pure: the callers read the
// cast bar, the spell's effects and the opponent memory (what a player in the bot's seat sees, DT.12) and pass plain
// numbers. A bot below the tactic's known skill, or one that takes the shortcut (Wisdom, G0h3's think depth), kicks
// the first interruptible cast it notices as soon as its reaction allows (today's behaviour); a judging bot classifies
// the cast, holds its kick on damage for a healer in reach and commits late on heals, with log-normal noise. The kick
// lands at the later of the commit point and the reaction model's due time when the model counts from the bar's start
// (Natural.Reaction.EndToEnd, a bar younger than REACTION_MAX_STAMP_AGE_MS); else the model counts from the first
// query, which is the commit point, and the kick lands at commit plus reaction.

namespace ArenaInterrupt
{
enum class Kind : uint8
{
    Heal,
    Cc,
    Damage,
    Other,
    Count
};

constexpr uint32 KIND_COUNT = static_cast<uint32>(Kind::Count);

inline char const* KindName(Kind kind)
{
    switch (kind)
    {
        case Kind::Heal:
            return "heal";
        case Kind::Cc:
            return "cc";
        case Kind::Damage:
            return "damage";
        default:
            return "other";
    }
}

// What a player sees of the cast's spell (ArenaInterruptSpells.h reads it from the spell's effects)
struct SpellFacts
{
    bool Heals = false;     // a heal, a heal over time or an absorb
    bool Controls = false;  // a loss-of-control aura (a CC DR group: polymorph, fear, cyclone, root, ...)
    bool Damages = false;   // direct or periodic damage
};

// A CC beats a heal (a damaging CC is a CC); everything else is not worth judging
inline Kind Classify(SpellFacts const& facts)
{
    if (facts.Controls)
        return Kind::Cc;

    if (facts.Heals)
        return Kind::Heal;

    return facts.Damages ? Kind::Damage : Kind::Other;
}

// The kick's worth on a cast: a heal on a low friend, a CC on a team member, a heal, a damage cast; 0 = none
inline uint32 Score(Kind kind, uint8 targetHpPct, uint8 urgentHpPct)
{
    switch (kind)
    {
        case Kind::Heal:
            return targetHpPct <= urgentHpPct ? 400 : 200;
        case Kind::Cc:
            return 300;
        case Kind::Damage:
            return 100;
        default:
            return 0;
    }
}

// One bot's numbers at its skill and Patience (cheap: computed at each decision)
struct Params
{
    float HealShare = 0.70f;
    float DamageShare = 0.50f;
    float CcShare = 0.0f;
    float MaxShare = 0.90f;
    float Sigma = 0.15f;
    float CancelShare = 0.15f;
    float PatienceShare = 0.0f;  // signed: the Patience trait's part
    uint8 UrgentHpPct = 30;
    bool HoldForHealer = true;
};

// Skill 0 (the skill system off) counts as skill 99
inline Params ComputeParams(uint8 skill, uint8 patience, float curveExponent, InterruptConfig const& config)
{
    float const position = SkillPosition(skill == 0 ? 99 : skill);
    auto const pct = [](uint32 value) { return static_cast<float>(std::min<uint32>(value, 100)) / 100.0f; };
    Params params;
    params.HealShare =
        pct(static_cast<uint32>(SkillLerp(static_cast<float>(config.HealSharePctAtSkill1),
                                          static_cast<float>(config.HealSharePctAtSkill99), position, curveExponent) +
                                0.5f));
    params.DamageShare = pct(config.DamageSharePct);
    params.CcShare = pct(config.CcSharePct);
    params.MaxShare = pct(config.MaxSharePct);
    params.Sigma = std::max(0.0f, SkillLerp(static_cast<float>(config.SigmaPctAtSkill1),
                                            static_cast<float>(config.SigmaPctAtSkill99), position, curveExponent) /
                                      100.0f);
    params.CancelShare = pct(config.CancelSharePct);
    float const trait = (static_cast<float>(std::min<uint8>(patience, 100)) - 50.0f) / 50.0f;
    params.PatienceShare = pct(config.PatienceSharePct) * trait;
    params.UrgentHpPct = static_cast<uint8>(std::min<uint32>(config.UrgentHpPct, 100));
    params.HoldForHealer = config.HoldForHealer != 0;
    return params;
}

// The deep path: the bot knows the tactic (TacticActive) and, with G0h3's think part on, takes the deep check by its
// Wisdom (ThinkDeepPercent); full ability (skill 0, PvE with Skill.FullAbilityInPvE, exempt) always judges
inline bool UsesDeep(bool known, bool fullAbility, bool thinkEnable, ReactionTypeState const& types, uint64 hash)
{
    if (!known)
        return false;

    if (fullAbility || !thinkEnable)
        return true;

    return ChooseThinkDepth(types, hash) == ReactionThinkDepth::Deep;
}

struct Input
{
    bool Deep = false;  // the bot judges the cast (else the shortcut: no Classify)
    Kind CastKind = Kind::Other;
    uint32 CastMs = 0;           // the bar's length
    uint8 TargetHpPct = 100;     // the cast's target (a heal's friend)
    bool HealerInRange = false;  // another enemy, a healer in reach, worth holding the kick for
    float FakeRate = 0.0f;       // cancels seen per cast of this caster (0 below CancelMinCasts)
};

enum class Reason : uint8
{
    Shortcut,
    Heal,
    UrgentHeal,
    Cc,
    Damage,
    HoldForHealer,
    NotWorth,
    Count
};

inline char const* ReasonName(Reason reason)
{
    switch (reason)
    {
        case Reason::Shortcut:
            return "shortcut";
        case Reason::Heal:
            return "heal";
        case Reason::UrgentHeal:
            return "urgent-heal";
        case Reason::Cc:
            return "cc";
        case Reason::Damage:
            return "damage";
        case Reason::HoldForHealer:
            return "hold-for-healer";
        default:
            return "not-worth";
    }
}

struct Decision
{
    bool Kick = false;  // the cast gets the kick once CommitMs of its bar have passed
    uint32 CommitMs = 0;
    float Share = 0.0f;  // CommitMs / CastMs
    Reason Why = Reason::NotWorth;
};

constexpr uint64 COMMIT_SALT = 0x1F83D9ABFB41BD6BULL;

// One decision per enemy cast; hash = the event's (G0h's ReactionEventHash), so the draw is stable across checks
inline Decision Decide(Input const& input, Params const& params, uint64 hash)
{
    Decision decision;
    if (!input.Deep)
    {
        decision.Kick = true;
        decision.Why = Reason::Shortcut;
        return decision;
    }

    float base = 0.0f;
    bool waits = true;  // a heal and a damage cast wait by Patience; a CC (and an urgent heal) does not
    switch (input.CastKind)
    {
        case Kind::Cc:
            base = params.CcShare;
            waits = false;
            decision.Why = Reason::Cc;
            break;
        case Kind::Heal:
            waits = input.TargetHpPct > params.UrgentHpPct;
            base = waits ? params.HealShare : params.CcShare;
            decision.Why = waits ? Reason::Heal : Reason::UrgentHeal;
            break;
        case Kind::Damage:
            if (params.HoldForHealer && input.HealerInRange)
            {
                decision.Why = Reason::HoldForHealer;
                return decision;
            }

            base = params.DamageShare;
            decision.Why = Reason::Damage;
            break;
        default:
            decision.Why = Reason::NotWorth;
            return decision;
    }

    // A caster seen cancelling makes a skilled bot wait longer on it (fakes)
    float share = base + params.CancelShare * std::clamp(input.FakeRate, 0.0f, 1.0f);
    if (waits)
        share += params.PatienceShare;

    if (share > 0.0f && params.Sigma > 0.0f)
        share *= static_cast<float>(std::exp(params.Sigma * Probit(UnitFromHash(SplitMix64(hash ^ COMMIT_SALT)))));

    decision.Kick = true;
    decision.Share = std::clamp(share, 0.0f, params.MaxShare);
    decision.CommitMs = static_cast<uint32>(decision.Share * static_cast<float>(input.CastMs) + 0.5f);
    return decision;
}

// A heal kick at or after this share of the bar counts as late (tactics.intLate)
constexpr uint32 LATE_SHARE_PCT = 50;
// A kick within this long of the commit point counts as bound by the commit, later ones by the reaction (trace only)
constexpr uint32 BOUND_TOLERANCE_MS = 150;
}  // namespace ArenaInterrupt

// One bot's decisions on the enemy casts in view (PlayerbotAI::GetInterruptState): one slot per cast, the oldest gives
// its slot up. Guid counters only, never a unit. The bot's own update; tactics show reads it on the world thread
// while the maps wait.
constexpr uint32 INTERRUPT_SLOTS = 4;

struct InterruptCastSlot
{
    uint32 Guid = 0;  // 0: free
    uint32 SpellId = 0;
    uint32 StartMs = 0;     // the bar's start as seen at the decision
    uint32 SeenMs = 0;      // the last check that asked
    uint32 ActiveAtMs = 0;  // the first check that let the kick go (0: not yet)
    uint8 Casting = 1;      // enemies casting at the decision (the choice's options)
    bool Deep = false;
    ArenaInterrupt::Kind CastKind = ArenaInterrupt::Kind::Other;
    ArenaInterrupt::Decision Decision;
};

struct InterruptBotState
{
    std::array<InterruptCastSlot, INTERRUPT_SLOTS> Slots{};
    // tactics show: decisions by kind (deep path), shortcuts, skips, kicks let go
    std::array<uint32, ArenaInterrupt::KIND_COUNT> Decided{};
    uint32 Shortcuts = 0;
    uint32 Skips = 0;
    uint32 Kicks = 0;

    // The slot of this cast: same enemy and spell, its start within toleranceMs (a pushback moves the bar)
    InterruptCastSlot* Find(uint32 guid, uint32 spellId, uint32 startMs, uint32 toleranceMs)
    {
        for (InterruptCastSlot& slot : Slots)
        {
            if (slot.Guid == 0 || slot.Guid != guid || slot.SpellId != spellId)
                continue;

            uint32 const gap = startMs > slot.StartMs ? startMs - slot.StartMs : slot.StartMs - startMs;
            if (gap <= toleranceMs)
                return &slot;
        }

        return nullptr;
    }

    // The enemy's newest decision
    InterruptCastSlot const* Latest(uint32 guid) const
    {
        InterruptCastSlot const* latest = nullptr;
        for (InterruptCastSlot const& slot : Slots)
            if (slot.Guid != 0 && slot.Guid == guid &&
                (!latest || static_cast<int32>(slot.StartMs - latest->StartMs) > 0))
                latest = &slot;

        return latest;
    }

    // A free slot or the one asked longest ago, cleared
    InterruptCastSlot& Take(uint32 nowMs)
    {
        uint32 index = 0;
        for (uint32 other = 0; other < INTERRUPT_SLOTS; ++other)
        {
            if (Slots[other].Guid == 0)
            {
                index = other;
                break;
            }

            if (nowMs - Slots[other].SeenMs > nowMs - Slots[index].SeenMs)
                index = other;
        }

        Slots[index] = InterruptCastSlot();
        return Slots[index];
    }
};

#endif
