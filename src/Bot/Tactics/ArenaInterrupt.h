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
// query, which is the commit point, and the kick lands at commit plus reaction. With HoldPolicy 1 (G5c-c) the hold on
// damage is DecideHold's: it holds only while a cast heal is likely (an enemy low; instant heals the bot recognises
// lower the health at which it expects one, a cast heal seen lately raises it) and its window reachable (reaction,
// anticipation), diluted by Knowledge, for as long as Patience allows. Weak bots notice casts late after their kick
// is ready (the attention gap, AttentionMsAtSkill*) and let some go (the use draw, UsePctAtSkill*).

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

// G5c-c's hold policy at one bot's Knowledge and Patience (HoldPolicy 1; InterruptConfig's Hold* keys)
struct InterruptHoldParams
{
    bool Enable = false;
    float KnowShare = 1.0f;       // the chance the bot knows an enemy spell (cast time, heal, instant)
    uint32 AssumedCastMs = 2000;  // the cast time it assumes for a heal it does not know
    float HpHigh = 0.80f;         // the need for a cast heal: none at or above HpHigh, full at or below HpLow
    float HpLow = 0.40f;
    float RecentShift = 0.10f;  // a healer seen starting a cast heal lately: HpHigh this much higher, no cover
    float Cover = 0.80f;        // recognised instant heals: HpHigh this share of the way down to HpLow
    uint32 WindowMarginMs = 300;
    float KnowDilution = 1.0f;  // Knowledge's factor on the motivation
    uint32 MaxHoldMs = 3500;    // Patience: the longest episode
    uint32 ResetMs = 6000;
    float TrendMs = 0.0f;  // the health projection's horizon (HoldTrendHorizonMs times Knowledge's share; 0: none)
};

// The attention gap's shape: SkillLerp's curve exponent (times Skill.CurveExponent), so the gap shrinks fast with skill
constexpr float ATTENTION_CURVE = 2.0f;
// The longest attention gap, in means (the exponential draw's tail cut)
constexpr double ATTENTION_MAX_MEANS = 3.0;
// Input::ReadyMs when the caller does not know how long the kick has been ready: the cast is noticed
constexpr uint32 ATTENTION_UNKNOWN_MS = 0xFFFFFFFFu;

// One bot's numbers at its skill, Knowledge and Patience (cheap: computed at each decision)
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
    float UseShare = 1.0f;     // the chance the bot notices an interruptible cast at all
    float AttentionMs = 0.0f;  // the mean gap after its kick is ready before it notices casts (0: none)
    InterruptHoldParams Hold;
};

// Skill 0 (the skill system off) counts as skill 99, for Knowledge too (Knowledge 0: no attributes, as 99)
inline Params ComputeParams(uint8 skill, uint8 patience, uint8 knowledge, float curveExponent,
                            InterruptConfig const& config)
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
    params.UseShare = pct(SkillLerpRounded(config.UsePctAtSkill1, config.UsePctAtSkill99, position, curveExponent));
    params.AttentionMs = std::max(
        0.0f, SkillLerp(static_cast<float>(config.AttentionMsAtSkill1), static_cast<float>(config.AttentionMsAtSkill99),
                        position, ATTENTION_CURVE * curveExponent));

    float const knowPosition = SkillPosition(skill == 0 || knowledge == 0 ? 99 : knowledge);
    InterruptHoldParams& hold = params.Hold;
    hold.Enable = config.HoldPolicy != 0;
    hold.KnowShare = pct(
        SkillLerpRounded(config.HoldKnowPctAtKnowledge1, config.HoldKnowPctAtKnowledge99, knowPosition, curveExponent));
    hold.AssumedCastMs = config.HoldAssumedCastMs;
    hold.HpHigh = pct(config.HoldHpHighPct);
    hold.HpLow = std::min(pct(config.HoldHpLowPct), hold.HpHigh);
    hold.RecentShift = pct(config.HoldRecentHpShiftPct);
    hold.Cover = pct(config.HoldInstantCoverPct);
    hold.WindowMarginMs = config.HoldWindowMarginMs;
    hold.KnowDilution = pct(SkillLerpRounded(config.HoldMotivationPctAtKnowledge1,
                                             config.HoldMotivationPctAtKnowledge99, knowPosition, curveExponent));
    float const atPatience0 = static_cast<float>(config.HoldMsAtPatience0);
    float const atPatience100 = static_cast<float>(config.HoldMsAtPatience100);
    float const patienceShare = static_cast<float>(std::min<uint8>(patience, 100)) / 100.0f;
    hold.MaxHoldMs = static_cast<uint32>(atPatience0 + (atPatience100 - atPatience0) * patienceShare + 0.5f);
    hold.ResetMs = config.HoldResetMs;
    hold.TrendMs = static_cast<float>(config.HoldTrendHorizonMs) *
                   pct(SkillLerpRounded(config.HoldTrendPctAtKnowledge1, config.HoldTrendPctAtKnowledge99, knowPosition,
                                        curveExponent));
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

// The window over which InterruptHoldInput::FriendLossPctPerS is measured (the health trend)
constexpr uint32 HOLD_TREND_WINDOW_MS = 2000;

// What a judging bot sees for the hold on a damage cast (G5c-c; health bars, cast bars, auras, DT.12), from one pass
// over the fight's enemies
struct InterruptHoldInput
{
    bool HealerSeen = false;  // an enemy healer other than the caster, in reach and sight
    uint8 FriendHpPct = 100;  // the lowest health of the caster's team in reach and sight
    // that friend's net health loss over the last HOLD_TREND_WINDOW_MS, in points per second (0: none or unknown)
    float FriendLossPctPerS = 0.0f;
    bool RecentCastHeal = false;  // the healer started a cast heal it knows (KnowsSpell) within HoldRecentCastHealMs
    uint32 HealCastMs = 0;      // the cast time the bot believes the healer's next cast heal takes (BelievedHealCastMs)
    bool InstantCover = false;  // instant heals or absorbs of the healer the bot recognises, seen lately
    uint32 ReactionMs = 0;      // the bot's expected time to a kick it waits for (HoldReactionMs)
    uint32 HeldMs = 0;          // the open episode's length (0: none)
    bool Expired = false;       // the open episode ran out (no hold again until it resets)
};

struct Input
{
    bool Deep = false;  // the bot judges the cast (else the shortcut: no Classify)
    Kind CastKind = Kind::Other;
    uint32 CastMs = 0;           // the bar's length
    uint8 TargetHpPct = 100;     // the cast's target (a heal's friend)
    bool HealerInRange = false;  // another enemy, a healer in reach, worth holding the kick for (HoldPolicy 0)
    float FakeRate = 0.0f;       // cancels seen per cast of this caster (0 below CancelMinCasts)
    uint32 ReadyMs = ATTENTION_UNKNOWN_MS;  // how long the bot's kick has been ready (InterruptBotState::KickReadyMs)
    uint64 ReadyHash = 0;                   // stable per ready window (AttentionHash)
    InterruptHoldInput Hold;                // HoldPolicy 1
};

enum class Reason : uint8
{
    Shortcut,
    Heal,
    UrgentHeal,
    Cc,
    Damage,
    HoldForHealer,
    Held,         // HoldPolicy 1: the damage cast is let go for an expected cast heal
    HoldExpired,  // HoldPolicy 1: a reason to hold, but the episode ran out: the damage cast is kicked
    Unnoticed,    // the attention gap or the use draw (AttentionMsAtSkill*, UsePctAtSkill*): the bot lets it go
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
        case Reason::Held:
            return "held";
        case Reason::HoldExpired:
            return "hold-expired";
        case Reason::Unnoticed:
            return "unnoticed";
        default:
            return "not-worth";
    }
}

// DecideHold's answer and its parts (trace and tactics show)
struct InterruptHoldDecision
{
    bool Hold = false;     // the damage cast is let go
    bool Expired = false;  // a reason to hold, but the episode ran out: the damage cast is kicked
    bool Reachable = false;
    uint8 ProjectedHpPct = 100;  // FriendHpPct projected over the trend horizon (HoldProjectedHpPct)
    float HpHigh = 0.0f;         // the health below which the bot expects a cast heal (HoldHpHigh)
    float Need = 0.0f;           // the chance of a cast heal as the bot judges it
    float Motivation = 0.0f;
};

struct Decision
{
    bool Kick = false;  // the cast gets the kick once CommitMs of its bar have passed
    uint32 CommitMs = 0;
    float Share = 0.0f;  // CommitMs / CastMs
    Reason Why = Reason::NotWorth;
    float UseDraw = 0.0f;        // the use draw (0 while UseShare is 1)
    uint32 AttentionGapMs = 0;   // the ready window's attention gap (0: AttentionMs 0 or ReadyMs unknown)
    InterruptHoldDecision Hold;  // a damage cast's, under HoldPolicy 1
};

constexpr uint64 COMMIT_SALT = 0x1F83D9ABFB41BD6BULL;
constexpr uint64 HOLD_SALT = 0x5BE0CD19137E2179ULL;
constexpr uint64 USE_SALT = 0x6A09E667F3BCC908ULL;
constexpr uint64 KNOW_SALT = 0x3C6EF372FE94F82BULL;
constexpr uint64 ATTENTION_SALT = 0xA54FF53A5F1D36F1ULL;

// One ready window of one bot's kick (from readyAtMs, InterruptBotState::KickReadyAtMs): the attention gap's hash
inline uint64 AttentionHash(uint64 botGuid, uint32 readyAtMs) { return SplitMix64((botGuid << 32) ^ readyAtMs); }

// How long after its kick is ready the bot notices interruptible casts: one exponential draw per ready window, mean
// AttentionMs, at most ATTENTION_MAX_MEANS of it (a weak bot's attention is elsewhere; a strong one's barely lapses)
inline uint32 AttentionGapMs(Params const& params, uint64 readyHash)
{
    if (params.AttentionMs <= 0.0f)
        return 0;

    double const draw = UnitFromHash(SplitMix64(readyHash ^ ATTENTION_SALT));
    double const means = std::min(-std::log(1.0 - draw), ATTENTION_MAX_MEANS);
    return static_cast<uint32>(means * params.AttentionMs + 0.5);
}

// The health below which the bot expects the healer to need a cast heal: HpHigh; higher by RecentShift while the
// healer was seen starting a cast heal lately (it uses them, its instants cover nothing then); with recognised instant
// heals Cover of the way down to HpLow. Pressure decides: at or below HpLow the need is full in every case.
inline float HoldHpHigh(InterruptHoldParams const& hold, bool recentCastHeal, bool instantCover)
{
    if (recentCastHeal)
        return std::min(1.0f, hold.HpHigh + hold.RecentShift);

    if (instantCover)
        return hold.HpLow + (hold.HpHigh - hold.HpLow) * (1.0f - hold.Cover);

    return hold.HpHigh;
}

// The need for a cast heal at the lowest enemy's health: full at or below HpLow, none at or above hpHigh
inline float HoldNeed(InterruptHoldParams const& hold, float hpHigh, uint8 hpPct)
{
    float const hp = static_cast<float>(std::min<uint8>(hpPct, 100)) / 100.0f;
    if (hp <= hold.HpLow)
        return 1.0f;

    if (hp >= hpHigh)
        return 0.0f;

    return (hpHigh - hp) / (hpHigh - hold.HpLow);
}

// The health the bot expects the lowest enemy at when its kick is back: now minus its recent net loss over the trend
// horizon (a burst that takes it into cast heal range within the kick's cooldown counts now); without a loss or a
// horizon the current health
inline uint8 HoldProjectedHpPct(InterruptHoldParams const& hold, uint8 hpPct, float lossPctPerS)
{
    if (hold.TrendMs <= 0.0f || !(lossPctPerS > 0.0f))
        return hpPct;

    float const projected = static_cast<float>(std::min<uint8>(hpPct, 100)) - lossPctPerS * hold.TrendMs / 1000.0f;
    return static_cast<uint8>(std::clamp(projected, 0.0f, 100.0f) + 0.5f);
}

// Whether the bot knows an enemy spell (its cast time, that it heals, that it is instant): one stable draw per bot and
// spell
inline bool KnowsSpell(InterruptHoldParams const& hold, uint64 botGuid, uint32 spellId)
{
    return hold.KnowShare >= 1.0f || UnitFromHash(SplitMix64((botGuid << 32) ^ spellId ^ KNOW_SALT)) < hold.KnowShare;
}

// The cast time the bot believes the healer's next cast heal takes: the last one seen when it knows that spell, else
// the class's usual one (0: none), else the assumed one
inline uint32 BelievedHealCastMs(InterruptHoldParams const& hold, bool seenKnown, uint32 seenCastMs,
                                 uint32 classPriorMs)
{
    if (seenCastMs > 0)
        return seenKnown ? seenCastMs : hold.AssumedCastMs;

    return classPriorMs > 0 ? classPriorMs : hold.AssumedCastMs;
}

// The bot's expected reaction to a heal it waits for: the reaction model's median plus extraMs (ping and display),
// the button and the deep check, anticipated (Foresight)
inline uint32 HoldReactionMs(ReactionState const& state, ReactionTypeState const& types, ReactionEase ease,
                             uint32 extraMs, uint32 minMs)
{
    uint32 const median = static_cast<uint32>(std::exp(state.Mu) + 0.5f) + extraMs + EaseCostMs(types, ease) +
                          ThinkCostMs(types, ReactionThinkDepth::Deep);
    return AnticipatedMs(median, types.AnticipateShare, minMs);
}

// G5c-c: hold the kick on a damage cast for an expected cast heal? The motivation is the cast heal's need at the
// projected health (HoldProjectedHpPct), 0 when its window is out of reach, times Knowledge's dilution; one stable draw
// per cast; an episode past MaxHoldMs kicks. The window is reachable when the bot's reaction to the bar it waits for
// (anticipated: HoldReactionMs) lands WindowMarginMs before the heal's end; a holding bot may kick as early as it
// likes, so its commit share does not count
inline InterruptHoldDecision DecideHold(InterruptHoldInput const& input, Params const& params, uint64 hash)
{
    InterruptHoldDecision hold;
    if (!input.HealerSeen)
        return hold;

    InterruptHoldParams const& numbers = params.Hold;
    hold.HpHigh = HoldHpHigh(numbers, input.RecentCastHeal, input.InstantCover);
    hold.ProjectedHpPct = HoldProjectedHpPct(numbers, input.FriendHpPct, input.FriendLossPctPerS);
    hold.Need = HoldNeed(numbers, hold.HpHigh, hold.ProjectedHpPct);
    hold.Reachable = input.HealCastMs > 0 && input.ReactionMs + numbers.WindowMarginMs <= input.HealCastMs;
    if (!hold.Reachable)
        return hold;

    hold.Motivation = hold.Need * numbers.KnowDilution;
    if (hold.Motivation <= 0.0f || UnitFromHash(SplitMix64(hash ^ HOLD_SALT)) >= hold.Motivation)
        return hold;

    if (input.Expired || input.HeldMs >= numbers.MaxHoldMs)
        hold.Expired = true;
    else
        hold.Hold = true;

    return hold;
}

// One decision per enemy cast; hash = the event's (G0h's ReactionEventHash), so the draw is stable across checks
inline Decision Decide(Input const& input, Params const& params, uint64 hash)
{
    Decision decision;
    if (params.AttentionMs > 0.0f && input.ReadyMs != ATTENTION_UNKNOWN_MS)
    {
        decision.AttentionGapMs = AttentionGapMs(params, input.ReadyHash);
        if (input.ReadyMs < decision.AttentionGapMs)
        {
            decision.Why = Reason::Unnoticed;
            return decision;
        }
    }

    if (params.UseShare < 1.0f)
    {
        decision.UseDraw = static_cast<float>(UnitFromHash(SplitMix64(hash ^ USE_SALT)));
        if (decision.UseDraw >= params.UseShare)
        {
            decision.Why = Reason::Unnoticed;
            return decision;
        }
    }

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
            decision.Why = Reason::Damage;
            if (params.Hold.Enable)
            {
                decision.Hold = DecideHold(input.Hold, params, hash);
                if (decision.Hold.Hold)
                {
                    decision.Why = Reason::Held;
                    return decision;
                }

                if (decision.Hold.Expired)
                    decision.Why = Reason::HoldExpired;
            }
            else if (params.HoldForHealer && input.HealerInRange)
            {
                decision.Why = Reason::HoldForHealer;
                return decision;
            }

            base = params.DamageShare;
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

// G5c-c: one hold episode, from the first damage cast held until HoldResetMs pass without a hold reason (a cast held
// or kicked as expired), a heal kicked, or the match's end
struct InterruptHoldEpisode
{
    bool Active = false;
    uint32 SinceMs = 0;
    uint32 ReasonMs = 0;       // the last decision with a hold reason
    bool Expired = false;      // ran past MaxHoldMs: damage casts are kicked until it resets
    bool HealBarSeen = false;  // the healer showed a cast heal's bar during the episode

    uint32 HeldMs(uint32 nowMs) const { return Active ? nowMs - SinceMs : 0; }
};

// How an episode ended: a heal kicked (paid), no heal bar at all (vain: instants only or none), heal bars none kicked
enum class InterruptHoldEnd : uint8
{
    None,
    Paid,
    Vain,
    Missed
};

struct InterruptBotState
{
    std::array<InterruptCastSlot, INTERRUPT_SLOTS> Slots{};
    // tactics show: decisions by kind (deep path), shortcuts, skips, kicks let go
    std::array<uint32, ArenaInterrupt::KIND_COUNT> Decided{};
    uint32 Shortcuts = 0;
    uint32 Skips = 0;
    uint32 Kicks = 0;
    // G5c-c: the open hold episode, and for tactics show: damage casts held, episodes, how they ended
    InterruptHoldEpisode Hold;
    uint32 Holds = 0;
    uint32 HoldEpisodes = 0;
    uint32 HoldsExpired = 0;
    uint32 HoldsPaid = 0;
    uint32 HoldsVain = 0;
    uint32 HoldsMissed = 0;
    uint32 KickReadyAtMs = 0;  // G5c-c: when the bot's kick came (or comes) off cooldown; the fight's start at first

    // A damage cast held: opens an episode at the first (true: a new one)
    bool NoteHeld(uint32 nowMs)
    {
        ++Holds;
        if (!Hold.Active)
        {
            OpenHold(nowMs);
            return true;
        }

        Hold.ReasonMs = nowMs;
        return false;
    }

    // A damage cast kicked as the episode ran out: true the first time in the episode. The episode ends HoldResetMs
    // after it ran out; later kicks do not extend it, so under steady pressure the bot holds again after the reset
    bool NoteHoldExpired(uint32 nowMs)
    {
        if (!Hold.Active)
            OpenHold(nowMs);

        if (Hold.Expired)
            return false;

        Hold.ReasonMs = nowMs;
        Hold.Expired = true;
        ++HoldsExpired;
        return true;
    }

    // The attention gap (G5c-c): the bot's kick was used now and is ready again after cooldownMs
    void NoteKick(uint32 nowMs, uint32 cooldownMs) { KickReadyAtMs = nowMs + cooldownMs; }

    // How long the kick has been ready (0: not yet)
    uint32 KickReadyMs(uint32 nowMs) const
    {
        return static_cast<int32>(nowMs - KickReadyAtMs) > 0 ? nowMs - KickReadyAtMs : 0;
    }

    // The healer started a cast heal's bar
    void NoteHoldHealBar()
    {
        if (Hold.Active)
            Hold.HealBarSeen = true;
    }

    // A heal kicked: the open episode paid off
    InterruptHoldEnd NoteHoldHealKick()
    {
        if (!Hold.Active)
            return InterruptHoldEnd::None;

        Hold = InterruptHoldEpisode();
        ++HoldsPaid;
        return InterruptHoldEnd::Paid;
    }

    // At each decision: the episode ends resetMs after its last hold reason
    InterruptHoldEnd ResetHold(uint32 nowMs, uint32 resetMs)
    {
        if (!Hold.Active || nowMs - Hold.ReasonMs < resetMs)
            return InterruptHoldEnd::None;

        return EndHold();
    }

    // The match is over (and the reset above)
    InterruptHoldEnd EndHold()
    {
        if (!Hold.Active)
            return InterruptHoldEnd::None;

        bool const vain = !Hold.HealBarSeen;
        Hold = InterruptHoldEpisode();
        if (vain)
        {
            ++HoldsVain;
            return InterruptHoldEnd::Vain;
        }

        ++HoldsMissed;
        return InterruptHoldEnd::Missed;
    }

    void OpenHold(uint32 nowMs)
    {
        Hold = InterruptHoldEpisode();
        Hold.Active = true;
        Hold.SinceMs = nowMs;
        Hold.ReasonMs = nowMs;
        ++HoldEpisodes;
    }

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
