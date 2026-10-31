/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAINTERRUPTSIM_H
#define _PLAYERBOT_ARENAINTERRUPTSIM_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "ArenaInterrupt.h"
#include "StringFormat.h"

// G5c-c's fight model for `tactics selftest interrupt` (pure, deterministic, well under a second): one kicker against
// an enemy healer and an enemy damage caster, in 100 ms steps. The healer's friend loses health on a curve with
// spikes (or is held at one health, SteadyHpPct; the kicker reads its loss over the last 2 s, the trend); the healer
// answers by its kit; the caster casts a 2.5-3.5 s damage
// bar every few seconds, and each one it completes hits the kicker's friend, whom the kicker's healer heals at a
// steady rate. The kicker decides once per bar with ArenaInterrupt::Decide (at the first moment its kick is ready
// during the bar) and its kick lands at the later of the commit point and its reaction draw; a kick due after the
// bar's end misses (the kick stays ready). An interrupted cast locks its caster out. A heal kick is worth more the
// lower its friend (saving kicks: below SIM_SAVING_HP_PCT), a damage kick is worth the hit it prevents; enemy deaths
// and own deaths per fight weigh the two. Numbers per fight are the model's, not the game's: the model compares
// policies and skill bands, it does not predict match results.
namespace ArenaInterruptSim
{
enum class HealerKit : uint8
{
    Cast,     // cast heals only: a 2.0 s heal, a 3.0 s heal below 45 % health
    Mixed,    // instant heals above 35 % health, a 1.5 s heal below
    Instant,  // instant heals only
    Count
};

constexpr uint32 KIT_COUNT = static_cast<uint32>(HealerKit::Count);

inline char const* KitName(HealerKit kit)
{
    switch (kit)
    {
        case HealerKit::Cast:
            return "cast healer";
        case HealerKit::Mixed:
            return "mixed healer";
        default:
            return "instant healer";
    }
}

enum class Policy : uint8
{
    NoHold,  // HoldPolicy 0, HoldForHealer 0: every damage cast worth a kick is kicked
    Rule,    // HoldPolicy 0, HoldForHealer 1 (G5b): damage casts held once the healer has healed
    Hold,    // HoldPolicy 1 (G5c-c)
    Count
};

inline char const* PolicyName(Policy mode)
{
    switch (mode)
    {
        case Policy::NoHold:
            return "no hold";
        case Policy::Rule:
            return "G5b rule";
        default:
            return "hold policy";
    }
}

// Spells as the bot's knowledge draws see them (ids of real spells; the model keeps its own cast times)
constexpr uint32 SIM_LONG_HEAL = 5185;   // Healing Touch
constexpr uint32 SIM_HEAL = 8936;        // Regrowth
constexpr uint32 SIM_FLASH_HEAL = 2061;  // Flash Heal
constexpr uint32 SIM_INSTANT = 139;      // Renew
constexpr uint32 SIM_DRUID_PRIOR_MS = 2000;
constexpr uint32 SIM_PRIEST_PRIOR_MS = 1500;
constexpr uint32 SIM_STEP_MS = 100;
constexpr uint32 SIM_BURST_MS = 5000;
constexpr uint32 SIM_SAVING_HP_PCT = 50;
// The kicker's healer: 4 % health per second, about even with the caster's uncontested 5.5 % (a kick on a damage cast
// and one on a heal are then worth about the same on average; the selftest also weighs 3 and 4.5 %)
constexpr float SIM_OWN_HEAL_PER_STEP = 0.4f;
constexpr float SIM_HIT_MIN = 20.0f;  // a completed damage cast: 20-30 % of the kicker's friend
constexpr float SIM_HIT_SPREAD = 10.0f;

struct Setup
{
    HealerKit Kit = HealerKit::Cast;
    Policy Mode = Policy::Hold;
    uint8 Skill = 80;  // the kicker's skill, Knowledge and every ability
    uint8 Patience = 50;
    uint8 KnownFromSkill = 50;  // the tactic's known skill (below it the shortcut)
    uint8 SteadyHpPct = 0;      // 0: the pressure curve; else the healer's friend stays at this health (no deaths)
    InterruptConfig Config;     // HoldPolicy and HoldForHealer are set by Mode
    ReactionParams Reaction;
    uint32 Fights = 500;
    uint32 FightMs = 60000;
    uint32 KickCooldownMs = 10000;
    uint32 LockoutMs = 4000;
    float OwnHealPerStep = SIM_OWN_HEAL_PER_STEP;
    uint64 Seed = 1;
};

struct Result
{
    uint32 Fights = 0;
    uint32 HealBars = 0;
    uint32 DamageBars = 0;
    uint32 Kicks = 0;
    uint32 HealKicks = 0;
    uint32 SavingKicks = 0;  // heal kicks on a friend below SIM_SAVING_HP_PCT
    uint32 DamageKicks = 0;
    uint32 TooLate = 0;  // kicks due after the bar's end
    uint32 Held = 0;     // damage casts let go for an expected heal (Held and HoldForHealer)
    uint32 Unnoticed = 0;
    uint32 Episodes = 0;
    uint32 Paid = 0;
    uint32 Vain = 0;
    uint32 Missed = 0;
    uint32 Expired = 0;
    uint32 Deaths = 0;     // the healer's friend at 0 health (it comes back full)
    uint32 OwnDeaths = 0;  // the kicker's friend at 0 health (it comes back full)

    float PerFight(uint32 value) const { return Fights ? static_cast<float>(value) / Fights : 0.0f; }
    float KicksPerCast() const
    {
        uint32 const casts = HealBars + DamageBars;
        return casts ? static_cast<float>(Kicks) / casts : 0.0f;
    }
    float HealShare() const { return Kicks ? static_cast<float>(HealKicks) / Kicks : 0.0f; }
    float HeldShare() const { return DamageBars ? static_cast<float>(Held) / DamageBars : 0.0f; }
    float VainShare() const { return Episodes ? static_cast<float>(Vain) / Episodes : 0.0f; }
    float NetDeaths() const { return PerFight(Deaths) - PerFight(OwnDeaths); }
};

namespace Detail
{
struct Rng
{
    uint64 State;
    uint64 Next() { return SplitMix64(State++); }
    float Unit() { return static_cast<float>(UnitFromHash(Next())); }
};

struct Bar
{
    bool Active = false;
    bool Decided = false;
    bool Kicks = false;
    uint32 StartMs = 0;
    uint32 EndMs = 0;
    uint32 LandMs = 0;
    uint32 SpellId = 0;
    float Heals = 0.0f;
};
}  // namespace Detail

inline Result Simulate(Setup const& setup)
{
    using namespace ArenaInterrupt;
    InterruptConfig config = setup.Config;
    config.HoldPolicy = setup.Mode == Policy::Hold ? 1 : 0;
    config.HoldForHealer = setup.Mode == Policy::Rule ? 1 : 0;
    constexpr float Exponent = 1.0f;
    Params const params = ComputeParams(setup.Skill, setup.Patience, setup.Skill, Exponent, config);
    ReactionState const reaction = ComputeReactionState(setup.Reaction, setup.Skill, Exponent);
    ReactionTypeInputs abilities;
    abilities.Speed = abilities.Wisdom = abilities.Focus = abilities.Dexterity = abilities.Foresight =
        std::clamp<uint8>(setup.Skill, 1, 99);
    abilities.Patience = setup.Patience;
    ReactionTypeState const types = ComputeReactionTypeState(setup.Reaction, abilities, Exponent);
    uint32 const extraMs = setup.Reaction.PingMs + setup.Reaction.DisplayMs;
    uint32 const holdReactionMs = HoldReactionMs(reaction, types, ReactionEase::Common, extraMs, setup.Reaction.MinMs);
    bool const known = setup.Skill >= setup.KnownFromSkill;
    uint32 const priorMs = setup.Kit == HealerKit::Cast ? SIM_DRUID_PRIOR_MS : SIM_PRIEST_PRIOR_MS;

    Result result;
    result.Fights = setup.Fights;
    for (uint32 fight = 0; fight < setup.Fights; ++fight)
    {
        uint64 const botGuid = SplitMix64(setup.Seed * 1000003ULL + fight) & 0xFFFFFFULL;
        Detail::Rng rng{SplitMix64(setup.Seed ^ (static_cast<uint64>(fight) << 24) ^ 0xF16A7ULL)};
        InterruptBotState state;
        float hp = setup.SteadyHpPct ? static_cast<float>(setup.SteadyHpPct) : 100.0f;
        float ownHp = 100.0f;
        Detail::Bar heal;
        Detail::Bar damage;
        uint32 healerFreeMs = 0;
        uint32 casterFreeMs = static_cast<uint32>(rng.Unit() * 2000.0f);
        bool healed = false;  // the healer has healed (G5b's rule: a healer seen healing)
        uint32 lastHealSpell = 0;
        uint32 lastHealCastMs = 0;
        bool lastHealBarValid = false;
        uint32 lastHealBarMs = 0;
        bool instantSeen = false;
        uint32 lastInstantMs = 0;
        uint32 burstUntilMs = 0;
        uint32 nextBurstMs = 4000 + static_cast<uint32>(rng.Unit() * 8000.0f);
        constexpr uint32 TrendSteps = HOLD_TREND_WINDOW_MS / SIM_STEP_MS;
        std::array<float, TrendSteps> trend;  // the healer's friend's health over the trend window (a ring)
        trend.fill(hp);
        uint32 trendAt = 0;

        auto const hpPct = [&hp]() { return static_cast<uint8>(std::clamp(std::lround(hp), 0L, 100L)); };
        auto const decide = [&](Detail::Bar& bar, bool isHeal, uint32 nowMs)
        {
            bar.Decided = true;
            uint64 const hash = SplitMix64((botGuid << 32) ^ bar.StartMs ^ (isHeal ? 0x11ULL : 0x22ULL));
            Input input;
            input.Deep = UsesDeep(known, false, true, types, hash);
            input.CastKind = isHeal ? Kind::Heal : Kind::Damage;
            input.CastMs = bar.EndMs - bar.StartMs;
            input.TargetHpPct = hpPct();
            input.HealerInRange = healed;
            input.ReadyMs = state.KickReadyMs(nowMs);
            input.ReadyHash = AttentionHash(botGuid, state.KickReadyAtMs);
            state.ResetHold(nowMs, params.Hold.ResetMs);
            InterruptHoldInput& hold = input.Hold;
            hold.HealerSeen = true;
            hold.FriendHpPct = hpPct();
            hold.FriendLossPctPerS = std::max(0.0f, trend[trendAt] - hp) * 1000.0f / HOLD_TREND_WINDOW_MS;
            hold.RecentCastHeal = lastHealBarValid && nowMs - lastHealBarMs <= config.HoldRecentCastHealMs &&
                                  KnowsSpell(params.Hold, botGuid, lastHealSpell);
            hold.HealCastMs = BelievedHealCastMs(params.Hold, KnowsSpell(params.Hold, botGuid, lastHealSpell),
                                                 lastHealCastMs, priorMs);
            hold.InstantCover = instantSeen && nowMs - lastInstantMs <= config.HoldInstantSeenMs &&
                                KnowsSpell(params.Hold, botGuid, SIM_INSTANT);
            hold.ReactionMs = holdReactionMs;
            hold.HeldMs = state.Hold.HeldMs(nowMs);
            hold.Expired = state.Hold.Expired;
            Decision const decision = Decide(input, params, hash);
            if (decision.Why == Reason::Held || decision.Why == Reason::HoldForHealer)
                ++result.Held;

            if (decision.Why == Reason::Held)
                state.NoteHeld(nowMs);
            else if (decision.Why == Reason::HoldExpired)
                state.NoteHoldExpired(nowMs);
            else if (decision.Why == Reason::Unnoticed)
                ++result.Unnoticed;

            if (!decision.Kick)
                return;

            uint64 const drawHash = SplitMix64(hash ^ 0x5EAC7ULL);
            uint32 reactionMs =
                ReactionMs(reaction, setup.Reaction.MinMs, drawHash) + extraMs +
                EaseCostMs(types, ReactionEase::Common) +
                ThinkCostMs(types, input.Deep ? ReactionThinkDepth::Deep : ReactionThinkDepth::Shortcut);
            if (isHeal && state.Hold.Active)
                reactionMs = AnticipatedMs(reactionMs, types.AnticipateShare, setup.Reaction.MinMs);

            bar.LandMs = std::max(bar.StartMs + decision.CommitMs, nowMs + reactionMs);
            bar.Kicks = bar.LandMs < bar.EndMs;
            if (!bar.Kicks)
                ++result.TooLate;
        };
        auto const land = [&](Detail::Bar& bar, bool isHeal, uint32 nowMs)
        {
            if (!bar.Active || !bar.Kicks || bar.LandMs > nowMs || state.KickReadyAtMs > bar.LandMs)
                return;

            state.NoteKick(bar.LandMs, setup.KickCooldownMs);
            bar.Active = false;
            ++result.Kicks;
            if (isHeal)
            {
                ++result.HealKicks;
                if (hpPct() < SIM_SAVING_HP_PCT)
                    ++result.SavingKicks;

                healerFreeMs = bar.LandMs + setup.LockoutMs;
                state.NoteHoldHealKick();
            }
            else
            {
                ++result.DamageKicks;
                casterFreeMs = bar.LandMs + setup.LockoutMs;
            }
        };
        auto const start = [](Detail::Bar& bar, uint32 nowMs, uint32 castMs, uint32 spellId, float heals)
        {
            bar = Detail::Bar();
            bar.Active = true;
            bar.StartMs = nowMs;
            bar.EndMs = nowMs + castMs;
            bar.SpellId = spellId;
            bar.Heals = heals;
        };

        for (uint32 nowMs = 0; nowMs < setup.FightMs; nowMs += SIM_STEP_MS)
        {
            // Kicks first: one due before a bar's end lands before the bar completes in the same step
            land(heal, true, nowMs);
            land(damage, false, nowMs);

            // Pressure: a steady part with spikes, and a burst of SIM_BURST_MS every 8-16 s at three times the rate
            if (nowMs >= nextBurstMs)
            {
                burstUntilMs = nowMs + SIM_BURST_MS;
                nextBurstMs = burstUntilMs + 8000 + static_cast<uint32>(rng.Unit() * 8000.0f);
            }

            float const pressure = nowMs < burstUntilMs ? 3.0f : 1.0f;
            hp -= 0.25f * pressure;
            if (rng.Unit() < 0.02f * pressure)
                hp -= 8.0f + 7.0f * rng.Unit();

            if (hp <= 0.0f)
            {
                ++result.Deaths;
                hp = 100.0f;
            }

            ownHp = std::min(100.0f, ownHp + setup.OwnHealPerStep);
            if (heal.Active && nowMs >= heal.EndMs)
            {
                heal.Active = false;
                hp = std::min(100.0f, hp + heal.Heals);
                healerFreeMs = nowMs;
            }

            if (damage.Active && nowMs >= damage.EndMs)
            {
                damage.Active = false;
                ownHp -= SIM_HIT_MIN + SIM_HIT_SPREAD * rng.Unit();
                if (ownHp <= 0.0f)
                {
                    ++result.OwnDeaths;
                    ownHp = 100.0f;
                }

                casterFreeMs = nowMs + 500 + static_cast<uint32>(rng.Unit() * 2000.0f);
            }

            if (!heal.Active && nowMs >= healerFreeMs)
            {
                bool castHeal = false;
                uint32 castMs = 0;
                uint32 spellId = 0;
                float amount = 0.0f;
                switch (setup.Kit)
                {
                    case HealerKit::Cast:
                        if (hp < 85.0f)
                        {
                            castHeal = true;
                            castMs = hp < 45.0f ? 3000 : 2000;
                            spellId = hp < 45.0f ? SIM_LONG_HEAL : SIM_HEAL;
                            amount = hp < 45.0f ? 45.0f : 25.0f;
                        }
                        break;
                    case HealerKit::Mixed:
                        if (hp < 35.0f)
                        {
                            castHeal = true;
                            castMs = 1500;
                            spellId = SIM_FLASH_HEAL;
                            amount = 25.0f;
                        }
                        else if (hp < 85.0f)
                            amount = 9.0f;
                        break;
                    default:
                        if (hp < 90.0f)
                            amount = 12.0f;
                        break;
                }

                if (castHeal)
                {
                    start(heal, nowMs, castMs, spellId, amount);
                    healed = true;
                    lastHealSpell = spellId;
                    lastHealCastMs = castMs;
                    lastHealBarValid = true;
                    lastHealBarMs = nowMs;
                    state.NoteHoldHealBar();
                    ++result.HealBars;
                }
                else if (amount > 0.0f)
                {
                    hp = std::min(100.0f, hp + amount);
                    healed = true;
                    instantSeen = true;
                    lastInstantMs = nowMs;
                    healerFreeMs = nowMs + 1500;
                }
            }

            if (setup.SteadyHpPct)
                hp = static_cast<float>(setup.SteadyHpPct);

            if (!damage.Active && nowMs >= casterFreeMs)
            {
                start(damage, nowMs, 2500 + static_cast<uint32>(rng.Unit() * 1000.0f), 133, 0.0f);
                ++result.DamageBars;
            }

            if (state.KickReadyAtMs <= nowMs)
            {
                if (heal.Active && !heal.Decided)
                    decide(heal, true, nowMs);

                if (damage.Active && !damage.Decided)
                    decide(damage, false, nowMs);
            }

            trend[trendAt] = hp;
            trendAt = (trendAt + 1) % TrendSteps;
        }

        state.EndHold();
        result.Episodes += state.HoldEpisodes;
        result.Paid += state.HoldsPaid;
        result.Vain += state.HoldsVain;
        result.Missed += state.HoldsMissed;
        result.Expired += state.HoldsExpired;
    }

    return result;
}

// One line of the model's table
inline std::string ResultLine(char const* name, Setup const& setup, Result const& result, Result const& noHold)
{
    float const kept = noHold.DamageKicks ? 100.0f * result.DamageKicks / noHold.DamageKicks : 0.0f;
    return Acore::StringFormat(
        "{} s{} p{}{} {}: heal share {:.1f} %, kicks {:.2f}/fight ({:.3f}/cast), heal kicks {:.2f} (saving {:.2f}), "
        "damage kicks {:.2f} ({:.0f} % of no hold), enemy deaths {:.2f}, own deaths {:.2f}, net {:+.2f}; held {:.1f} % "
        "of damage casts, episodes {:.2f}/fight (paid {}, vain {} = {:.0f} %, missed {}, expired {}), unnoticed {}",
        name, static_cast<uint32>(setup.Skill), static_cast<uint32>(setup.Patience),
        setup.SteadyHpPct ? Acore::StringFormat(" steady {} %", static_cast<uint32>(setup.SteadyHpPct)) : "",
        PolicyName(setup.Mode), 100.0f * result.HealShare(), result.PerFight(result.Kicks), result.KicksPerCast(),
        result.PerFight(result.HealKicks), result.PerFight(result.SavingKicks), result.PerFight(result.DamageKicks),
        kept, result.PerFight(result.Deaths), result.PerFight(result.OwnDeaths), result.NetDeaths(),
        100.0f * result.HeldShare(), result.PerFight(result.Episodes), result.Paid, result.Vain,
        100.0f * result.VainShare(), result.Missed, result.Expired, result.Unnoticed);
}

// The lab overlay's numbers (config/scenarios: HoldPolicy 1, the use draw, the attention gap; the rest, the trend too,
// at the dist proposals)
inline InterruptConfig LabConfig()
{
    InterruptConfig lab;
    lab.HoldPolicy = 1;
    lab.UsePctAtSkill1 = 20;
    lab.AttentionMsAtSkill1 = 30000;
    lab.AttentionMsAtSkill99 = 0;
    return lab;
}

// G5c-c's checks of `tactics selftest interrupt` (pure): HoldPolicy 0 parity, the need, the cover, the window,
// Patience, Knowledge, the trend, the use draw and the attention gap, the episode state and the fight model's bars
// (seed: the model's fights; the offline harness checks seeds 1-8). check(ok, what) counts, say(line) reports.
template <typename Check, typename Say>
inline void RunHoldSelftest(Check&& check, Say&& say, uint64 modelSeed = 1)
{
    using namespace ArenaInterrupt;
    using Acore::StringFormat;
    constexpr float Exponent = 1.0f;
    constexpr uint32 Draws = 400;
    InterruptConfig const dist{};

    // 1. Parity: HoldPolicy 0 decides as G5b's rule, kept verbatim here
    auto const oldDecide = [](Input const& input, Params const& params, uint64 hash)
    {
        Decision decision;
        if (!input.Deep)
        {
            decision.Kick = true;
            decision.Why = Reason::Shortcut;
            return decision;
        }

        float base = 0.0f;
        bool waits = true;
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

        float share = base + params.CancelShare * std::clamp(input.FakeRate, 0.0f, 1.0f);
        if (waits)
            share += params.PatienceShare;

        if (share > 0.0f && params.Sigma > 0.0f)
            share *= static_cast<float>(std::exp(params.Sigma * Probit(UnitFromHash(SplitMix64(hash ^ COMMIT_SALT)))));

        decision.Kick = true;
        decision.Share = std::clamp(share, 0.0f, params.MaxShare);
        decision.CommitMs = static_cast<uint32>(decision.Share * static_cast<float>(input.CastMs) + 0.5f);
        return decision;
    };
    uint32 differ = 0;
    for (uint32 draw = 0; draw < 1000; ++draw)
    {
        uint64 const seed = SplitMix64(0xA11CEULL + draw);
        auto const pick = [seed](uint32 salt, uint32 range)
        { return static_cast<uint32>(SplitMix64(seed ^ (static_cast<uint64>(salt) << 40)) % range); };
        InterruptConfig config = dist;
        config.HoldForHealer = pick(1, 2);
        Params const params = ComputeParams(static_cast<uint8>(pick(2, 100)), static_cast<uint8>(pick(3, 101)),
                                            static_cast<uint8>(pick(4, 100)), Exponent, config);
        Input input;
        input.Deep = pick(5, 4) != 0;
        input.CastKind = static_cast<Kind>(pick(6, KIND_COUNT));
        input.CastMs = pick(7, 4000);
        input.TargetHpPct = static_cast<uint8>(pick(8, 101));
        input.HealerInRange = pick(9, 2) != 0;
        input.FakeRate = static_cast<float>(pick(10, 121)) / 100.0f;
        input.Hold.HealerSeen = pick(11, 2) != 0;
        input.Hold.FriendHpPct = static_cast<uint8>(pick(12, 101));
        input.Hold.HealCastMs = pick(13, 3500);
        input.Hold.RecentCastHeal = pick(14, 2) != 0;
        input.Hold.HeldMs = pick(15, 8000);
        input.ReadyMs = pick(16, 30000);
        input.ReadyHash = SplitMix64(seed ^ 0x2EADULL);
        input.Hold.FriendLossPctPerS = static_cast<float>(pick(17, 2001)) / 100.0f;
        Decision const now = Decide(input, params, seed);
        Decision const before = oldDecide(input, params, seed);
        if (now.Kick != before.Kick || now.CommitMs != before.CommitMs || now.Share != before.Share ||
            now.Why != before.Why || now.UseDraw != 0.0f || now.AttentionGapMs != 0 || now.Hold.Motivation != 0.0f)
            ++differ;
    }

    check(differ == 0, StringFormat("HoldPolicy 0 decides as G5b's rule ({} of 1000 differ)", differ));

    InterruptConfig const lab = LabConfig();
    ReactionParams const reactionParams{};
    auto const reactionAt = [&reactionParams](uint8 ability, uint8 foresight)
    {
        ReactionState const state = ComputeReactionState(reactionParams, ability, Exponent);
        ReactionTypeInputs abilities;
        abilities.Speed = abilities.Wisdom = abilities.Focus = abilities.Dexterity = ability;
        abilities.Foresight = foresight;
        ReactionTypeState const types = ComputeReactionTypeState(reactionParams, abilities, Exponent);
        return HoldReactionMs(state, types, ReactionEase::Common, reactionParams.PingMs + reactionParams.DisplayMs,
                              reactionParams.MinMs);
    };
    uint32 const reaction99 = reactionAt(99, 99);
    uint32 const reaction1 = reactionAt(1, 1);
    Params const p99 = ComputeParams(99, 50, 99, Exponent, lab);
    Input fireball;
    fireball.Deep = true;
    fireball.CastKind = Kind::Damage;
    fireball.CastMs = 3000;
    fireball.Hold.HealerSeen = true;
    fireball.Hold.HealCastMs = 1500;
    fireball.Hold.ReactionMs = reaction99;
    auto const at = [&fireball](uint8 hpPct)
    {
        Input input = fireball;
        input.Hold.FriendHpPct = hpPct;
        return input;
    };
    auto const needAt = [&at](uint8 hpPct, bool recent, bool cover, Params const& params)
    {
        Input input = at(hpPct);
        input.Hold.RecentCastHeal = recent;
        input.Hold.InstantCover = cover;
        return DecideHold(input.Hold, params, 1).Need;
    };

    // 2. The need: pressure decides; a cast heal seen lately raises the health at which one is expected, no more
    Decision const low = Decide(at(25), p99, 1);
    check(low.Why == Reason::Held && !low.Kick && low.Hold.Motivation >= 0.9f,
          StringFormat("a fireball is held while an enemy is at 25 % (motivation {:.2f})", low.Hold.Motivation));
    Decision const high = Decide(at(90), p99, 1);
    check(high.Kick && high.Why == Reason::Damage && high.Hold.Motivation == 0.0f,
          "a fireball is kicked while every enemy is at 90 %");
    check(needAt(static_cast<uint8>(lab.HoldHpHighPct), false, false, p99) == 0.0f &&
              needAt(static_cast<uint8>(lab.HoldHpLowPct), false, false, p99) == 1.0f,
          "the need is 0 at HoldHpHighPct and full at HoldHpLowPct");
    float const recent80 = needAt(80, true, false, p99);
    check(needAt(80, false, false, p99) == 0.0f && recent80 > 0.0f && recent80 < 0.5f,
          StringFormat("a cast heal seen lately makes 80 % health a small need ({:.2f}), not a full one", recent80));
    check(needAt(95, true, false, p99) == 0.0f && Decide(at(95), p99, 1).Kick,
          "a cast heal seen lately gives no need while every enemy is at 95 % (pressure decides)");

    // 3. The cover: recognised instant heals lower the health at which a cast heal is expected, not below HpLow
    float const bare55 = needAt(55, false, false, p99);
    float const cover55 = needAt(55, false, true, p99);
    float const bare45 = needAt(45, false, false, p99);
    float const cover45 = needAt(45, false, true, p99);
    check(bare55 > 0.0f && cover55 == 0.0f && cover45 > 0.0f && cover45 < bare45,
          StringFormat("instant cover: 55 % from {:.2f} to {:.2f}, 45 % from {:.2f} to {:.2f}", bare55, cover55, bare45,
                       cover45));
    check(needAt(30, false, true, p99) == 1.0f, "an instant heal cannot save a friend at 30 %: the need stays full");
    check(needAt(55, true, true, p99) >= bare55, "a cast heal seen lately cancels the cover");
    Params const p1 = ComputeParams(1, 50, 1, Exponent, lab);
    uint64 unknownBot = 0;
    for (uint64 bot = 1; bot < 1000 && unknownBot == 0; ++bot)
        if (!KnowsSpell(p1.Hold, bot, SIM_INSTANT))
            unknownBot = bot;

    check(unknownBot != 0 && needAt(55, false, KnowsSpell(p1.Hold, unknownBot, SIM_INSTANT), p1) == bare55,
          "an instant heal the bot does not recognise leaves the need as it is");

    // 3b. The trend: the need at the health the bot expects when its kick is back; without a loss as before
    Input losing = at(85);
    losing.Hold.FriendLossPctPerS = 6.0f;
    InterruptHoldDecision const steady85 = DecideHold(at(85).Hold, p99, 1);
    InterruptHoldDecision const losing85 = DecideHold(losing.Hold, p99, 1);
    check(steady85.Need == 0.0f && losing85.ProjectedHpPct == 25 && losing85.Need == 1.0f &&
              Decide(losing, p99, 1).Why == Reason::Held,
          StringFormat("a friend at 85 % losing 6 points per second is read at {} %: full need, the fireball held",
                       static_cast<uint32>(losing85.ProjectedHpPct)));
    uint8 const losing85K1 = DecideHold(losing.Hold, p1, 1).ProjectedHpPct;
    check(p1.Hold.TrendMs < p99.Hold.TrendMs && losing85K1 > losing85.ProjectedHpPct,
          StringFormat("Knowledge 1 reads the trend less far ahead ({} % against {} %)",
                       static_cast<uint32>(losing85K1), static_cast<uint32>(losing85.ProjectedHpPct)));
    uint32 trendDiffer = 0;
    for (uint32 draw = 0; draw < 1000; ++draw)
    {
        uint64 const seed = SplitMix64(0x7AE4DULL + draw);
        auto const pick = [seed](uint32 salt, uint32 range)
        { return static_cast<uint32>(SplitMix64(seed ^ (static_cast<uint64>(salt) << 40)) % range); };
        Params const params = ComputeParams(static_cast<uint8>(pick(1, 100)), static_cast<uint8>(pick(2, 101)),
                                            static_cast<uint8>(pick(3, 100)), Exponent, lab);
        Params noTrend = params;
        noTrend.Hold.TrendMs = 0.0f;
        Input unfilled = fireball;
        unfilled.CastMs = 1000 + pick(4, 3000);
        unfilled.Hold.FriendHpPct = static_cast<uint8>(pick(5, 101));
        unfilled.Hold.RecentCastHeal = pick(6, 2) != 0;
        unfilled.Hold.InstantCover = pick(7, 2) != 0;
        unfilled.Hold.HealCastMs = pick(8, 3500);
        unfilled.Hold.HeldMs = pick(9, 8000);
        Input loss = unfilled;
        loss.Hold.FriendLossPctPerS = static_cast<float>(pick(10, 2001)) / 100.0f;
        Decision const a = Decide(unfilled, params, seed);
        Decision const b = Decide(loss, noTrend, seed);
        Decision const c = Decide(unfilled, noTrend, seed);
        if (a.Kick != c.Kick || a.CommitMs != c.CommitMs || a.Why != c.Why || a.Hold.Need != c.Hold.Need ||
            b.Kick != c.Kick || b.CommitMs != c.CommitMs || b.Why != c.Why || b.Hold.Need != c.Hold.Need)
            ++trendDiffer;
    }

    check(trendDiffer == 0,
          StringFormat("without a loss or a horizon the policy reads the current health ({} of 1000 differ)",
                       trendDiffer));

    // 4. The window: the anticipated reaction against the believed cast time, monotone in skill, not Patience
    uint32 previous = reaction1;
    bool falling = true;
    uint32 reachFrom = 0;
    bool closed = true;
    bool patienceFree = true;
    for (uint8 skill = 1; skill <= 99; ++skill)
    {
        uint32 const reactionMs = reactionAt(skill, skill);
        falling = falling && reactionMs <= previous;
        previous = reactionMs;
        Input shortHeal = at(50);
        shortHeal.Hold.HealCastMs = 1000;
        shortHeal.Hold.ReactionMs = reactionMs;
        bool const reach = DecideHold(shortHeal.Hold, ComputeParams(skill, 100, skill, Exponent, lab), 1).Reachable;
        patienceFree = patienceFree &&
                       reach == DecideHold(shortHeal.Hold, ComputeParams(skill, 0, skill, Exponent, lab), 1).Reachable;
        if (reach && reachFrom == 0)
            reachFrom = skill;
        else if (!reach && reachFrom != 0)
            closed = false;
    }

    check(falling, StringFormat("the expected reaction falls with skill ({} to {} ms)", reaction1, reaction99));
    check(reachFrom > 1 && reachFrom < 99 && closed && patienceFree,
          StringFormat("a 1000 ms heal at 50 % health is in reach from skill {} up, at Patience 0 and 100", reachFrom));
    Input urgent = at(25);
    urgent.Hold.HealCastMs = 700;
    urgent.Hold.ReactionMs = reactionAt(99, 99);
    bool const foresightHigh = DecideHold(urgent.Hold, p99, 1).Reachable;
    urgent.Hold.ReactionMs = reactionAt(99, 1);
    bool const foresightLow = DecideHold(urgent.Hold, p99, 1).Reachable;
    check(foresightHigh && !foresightLow, "Foresight 99 reaches a 700 ms heal that Foresight 1 cannot");
    urgent.Hold.HealCastMs = 0;
    urgent.Hold.ReactionMs = 0;
    check(!DecideHold(urgent.Hold, p99, 1).Reachable && Decide(urgent, p99, 1).Kick, "an instant heal never holds");

    // 5. Patience: the longest episode; an expired episode re-arms HoldResetMs after it ran out
    uint32 lastMs = 0;
    bool rising = true;
    for (uint8 patience = 0; patience <= 100; ++patience)
    {
        uint32 const maxMs = ComputeParams(99, patience, 99, Exponent, lab).Hold.MaxHoldMs;
        rising = rising && maxMs >= lastMs;
        lastMs = maxMs;
    }

    Params const patient = ComputeParams(99, 100, 99, Exponent, lab);
    Params const impatient = ComputeParams(99, 0, 99, Exponent, lab);
    check(rising && impatient.Hold.MaxHoldMs == lab.HoldMsAtPatience0 &&
              patient.Hold.MaxHoldMs == lab.HoldMsAtPatience100,
          StringFormat("the longest hold rises with Patience ({} to {} ms)", impatient.Hold.MaxHoldMs,
                       patient.Hold.MaxHoldMs));
    Input held3s = at(25);
    held3s.Hold.HeldMs = 3000;
    check(Decide(held3s, patient, 1).Why == Reason::Held, "Patience 100 still holds after 3 s");
    Decision const expired = Decide(held3s, impatient, 1);
    check(expired.Why == Reason::HoldExpired && expired.Kick, "Patience 0 kicks the damage cast after 3 s");
    InterruptBotState episode;
    episode.NoteHeld(1000);
    bool const firstExpiry = episode.NoteHoldExpired(4000);
    bool const secondExpiry = episode.NoteHoldExpired(5000);
    Input again = at(25);
    again.Hold.HeldMs = 0;
    again.Hold.Expired = episode.Hold.Expired;
    check(firstExpiry && !secondExpiry && Decide(again, impatient, 1).Why == Reason::HoldExpired,
          "an expired episode does not hold again");
    check(episode.ResetHold(4000 + lab.HoldResetMs - 1, lab.HoldResetMs) == InterruptHoldEnd::None &&
              episode.ResetHold(4000 + lab.HoldResetMs, lab.HoldResetMs) == InterruptHoldEnd::Vain,
          "the expired episode ends HoldResetMs after it ran out, later kicks do not extend it");
    again.Hold.Expired = episode.Hold.Expired;
    check(Decide(again, impatient, 1).Why == Reason::Held, "after the reset the bot holds again");

    // 6. Knowledge: the spells it knows, the dilution
    for (uint8 knowledge : {1, 50, 99})
    {
        Params const params = ComputeParams(80, 50, knowledge, Exponent, lab);
        uint32 knows = 0;
        for (uint32 spell = 0; spell < Draws; ++spell)
            knows += KnowsSpell(params.Hold, 4242, 1000 + spell * 7) ? 1 : 0;

        float const share = static_cast<float>(knows) / Draws;
        check(std::fabs(share - params.Hold.KnowShare) <= 0.05f,
              StringFormat("Knowledge {} knows {:.2f} of the spells (configured {:.2f})",
                           static_cast<uint32>(knowledge), share, params.Hold.KnowShare));
    }

    auto const heldShare = [&](uint8 knowledge, uint8 hpPct)
    {
        Params const params = ComputeParams(80, 50, knowledge, Exponent, lab);
        uint32 held = 0;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            Input input = at(hpPct);
            input.Hold.HealCastMs = BelievedHealCastMs(params.Hold, KnowsSpell(params.Hold, draw, SIM_HEAL), 2000, 0);
            input.Hold.RecentCastHeal = KnowsSpell(params.Hold, draw, SIM_HEAL);
            held += Decide(input, params, SplitMix64(draw)).Why == Reason::Held ? 1 : 0;
        }

        return static_cast<float>(held) / Draws;
    };
    float const held1 = heldShare(1, 50);
    float const held99 = heldShare(99, 50);
    check(held1 <= 0.5f * held99,
          StringFormat("Knowledge 1 holds at most half as often as 99 ({:.2f}, {:.2f})", held1, held99));
    float const held20 = heldShare(20, 60);
    float const held80 = heldShare(80, 60);
    check(held80 >= 2.0f * held20,
          StringFormat("Knowledge 80 holds at least twice as often as 20 in the same state ({:.2f}, {:.2f})", held80,
                       held20));
    check(BelievedHealCastMs(p1.Hold, false, 3000, 1500) == lab.HoldAssumedCastMs &&
              BelievedHealCastMs(p1.Hold, true, 3000, 1500) == 3000 &&
              BelievedHealCastMs(p1.Hold, false, 0, 1500) == 1500,
          "an unknown heal has the assumed cast time, a known one its own, none the class's");

    // 7. The use draw and the attention gap
    for (uint8 skill : {1, 50, 99})
    {
        Params const params = ComputeParams(skill, 50, skill, Exponent, lab);
        for (bool deep : {false, true})
        {
            Input input = at(90);
            input.Deep = deep;
            uint32 noticed = 0;
            for (uint32 draw = 0; draw < Draws; ++draw)
                noticed += Decide(input, params, SplitMix64(7000 + draw)).Why != Reason::Unnoticed ? 1 : 0;

            float const share = static_cast<float>(noticed) / Draws;
            check(std::fabs(share - params.UseShare) <= 0.05f,
                  StringFormat("skill {} ({}) notices {:.2f} of the casts (configured {:.2f})",
                               static_cast<uint32>(skill), deep ? "deep" : "shortcut", share, params.UseShare));
        }
    }

    Params const distParams = ComputeParams(1, 50, 1, Exponent, dist);
    uint32 unnoticed = 0;
    for (uint32 draw = 0; draw < Draws; ++draw)
    {
        Input input = at(90);
        input.ReadyMs = 0;
        unnoticed += Decide(input, distParams, SplitMix64(draw)).Why == Reason::Unnoticed ? 1 : 0;
    }

    check(distParams.UseShare == 1.0f && distParams.AttentionMs == 0.0f && unnoticed == 0,
          "at the dist UsePct 100/100 and AttentionMs 0/0 every cast is noticed, also the moment the kick is ready");
    InterruptConfig attentionOnly = lab;
    attentionOnly.UsePctAtSkill1 = 100;
    float lastMean = 1e9f;
    bool shrinking = true;
    bool centred = true;
    bool gated = true;
    for (uint8 skill : {20, 50, 80})
    {
        Params const params = ComputeParams(skill, 50, skill, Exponent, attentionOnly);
        double sum = 0.0;
        for (uint32 window = 0; window < Draws; ++window)
        {
            uint64 const readyHash = AttentionHash(4242, window * 10000);
            uint32 const gapMs = AttentionGapMs(params, readyHash);
            sum += gapMs;
            Input input = at(90);
            input.ReadyHash = readyHash;
            input.ReadyMs = gapMs;
            bool const seen = Decide(input, params, window).Why != Reason::Unnoticed;
            input.ReadyMs = gapMs > 0 ? gapMs - 1 : 0;
            bool const early = Decide(input, params, window).Why == Reason::Unnoticed;
            input.ReadyMs = ATTENTION_UNKNOWN_MS;
            Decision const unknown = Decide(input, params, window);
            gated = gated && seen && (gapMs == 0 || early) && unknown.Why != Reason::Unnoticed &&
                    unknown.AttentionGapMs == 0;
        }

        // The exponential cut at ATTENTION_MAX_MEANS has the mean 1 - e^-3 of the uncut one
        float const mean = static_cast<float>(sum / Draws);
        float const expectedMean = params.AttentionMs * static_cast<float>(1.0 - std::exp(-ATTENTION_MAX_MEANS));
        centred = centred && std::fabs(mean - expectedMean) <= 0.12f * params.AttentionMs + 1.0f;
        shrinking = shrinking && params.AttentionMs < lastMean;
        lastMean = params.AttentionMs;
        say(StringFormat("attention gap at skill {}: mean {:.0f} ms over {} windows (configured mean {:.0f})",
                         static_cast<uint32>(skill), mean, Draws, params.AttentionMs));
    }

    check(shrinking && centred && gated,
          "the attention gap shrinks with skill, centres on its mean, and a cast is noticed from the gap's end on "
          "(with the ready time unknown at once, no gap shown)");

    // 8. The episode state
    InterruptBotState state;
    check(state.NoteHeld(100) && !state.NoteHeld(900) && state.HoldEpisodes == 1 && state.Holds == 2 &&
              state.Hold.HeldMs(1100) == 1000,
          "the first held cast opens an episode, the next ones extend it");
    state.NoteHoldHealBar();
    check(state.ResetHold(900 + lab.HoldResetMs, lab.HoldResetMs) == InterruptHoldEnd::Missed && !state.Hold.Active,
          "an episode with a heal bar none kicked ends as missed after HoldResetMs");
    state.NoteHeld(5000);
    check(state.NoteHoldHealKick() == InterruptHoldEnd::Paid && state.HoldsPaid == 1 && !state.Hold.Active,
          "a heal kicked ends the episode as paid");
    state.NoteHeld(9000);
    check(state.EndHold() == InterruptHoldEnd::Vain && state.HoldsVain == 1 && state.HoldEpisodes == 3,
          "the match end closes an episode without heal bars as vain");
    state.NoteKick(20000, 10000);
    check(state.KickReadyMs(25000) == 0 && state.KickReadyMs(30000) == 0 && state.KickReadyMs(31500) == 1500,
          "the kick is ready for 0 ms during its cooldown, then counts up");

    // 9. The game side's state (G5c-c-b): the match start, the kick spells, the health trend
    state.NoteHeld(40000);
    check(state.StartMatch(7, 50000) == InterruptHoldEnd::Vain && !state.Hold.Active && state.MatchId == 7 &&
              state.KickReady(50000) && !state.HasKickSpell(),
          "a new match ends the open episode and the kick is ready since its start");
    constexpr uint32 MindFreeze = 47528;
    constexpr uint32 Strangulate = 47476;
    state.AddKickSpell(MindFreeze, 50000);
    state.AddKickSpell(Strangulate, 50000);
    state.NoteKickSpell(Strangulate, 51000, 120000);
    bool const backupReady = state.KickReady(52000) && state.KickReadyAtMs == 50000;
    state.NoteKickSpell(MindFreeze, 53000, 10000);
    check(backupReady && !state.KickReady(62999) && state.KickReady(63000) && state.KickReadyMs(64000) == 1000,
          "a ready backup kick keeps the kick ready; with both used it is ready when the first comes back");
    state.NoteHeld(70000);
    check(state.EndMatch() == InterruptHoldEnd::Vain && !state.Hold.Active && !state.MatchStarted && state.MatchId == 0,
          "the harness's counting turning on or off ends the open episode and the match");

    // The spell book's kicks that count: only those the bot casts at an enemy (an uncast one stays a ready backup)
    InterruptKickFacts pummel;
    pummel.Aimed = true;
    pummel.ItemFits = true;
    pummel.Warrior = InterruptWarriorSpec::Arms;
    InterruptKickFacts shieldBash = pummel;
    shieldBash.NeedsShield = true;
    shieldBash.ItemFits = false;
    InterruptKickFacts armsSwap = pummel;
    armsSwap.ArmsSwap = true;
    check(!CastsKickSpell(shieldBash) && !CastsKickSpell(pummel) && CastsKickSpell(armsSwap),
          "an Arms warrior without a shield: Shield Bash never, Pummel only through the stance swap");
    InterruptKickFacts torrent;
    torrent.ItemFits = true;
    InterruptKickFacts kick = torrent;
    kick.Aimed = true;
    check(!CastsKickSpell(torrent) && CastsKickSpell(kick),
          "a blood elf: Arcane Torrent (not aimed) is no kick, Kick is");
    InterruptKickFacts protBash = shieldBash;
    protBash.Warrior = InterruptWarriorSpec::Protection;
    InterruptKickFacts protShield = protBash;
    protShield.ItemFits = true;
    InterruptKickFacts protPummel = pummel;
    protPummel.Warrior = InterruptWarriorSpec::Protection;
    InterruptKickFacts furyPummel = pummel;
    furyPummel.Warrior = InterruptWarriorSpec::Fury;
    check(CastsKickSpell(protShield) && !CastsKickSpell(protBash) && !CastsKickSpell(protPummel) &&
              CastsKickSpell(furyPummel),
          "Protection: Shield Bash with a shield worn, never Pummel; Fury: Pummel");
    InterruptEnemyTrack track;
    for (uint32 step = 0; step <= 4; ++step)
        track.Note(static_cast<uint8>(100 - 5 * step), 1000 + 500 * step);

    float const loss = track.LossPctPerS(80, 3000);
    check(loss > 9.99f && loss < 10.01f && track.LossPctPerS(100, 3000) == 0.0f &&
              track.LossPctPerS(80, 1400) == 0.0f && track.LossPctPerS(80, 9000) == 0.0f,
          "the trend: the loss over the window's sample, 0 when health rose, without a sample old enough or too old");
    for (uint32 guid = 1; guid <= HOLD_TREND_UNITS; ++guid)
        state.Track(guid, 1000 * guid).Note(50, 1000 * guid);

    state.Track(3, 9000).Note(40, 9000);
    InterruptEnemyTrack const& newcomer = state.Track(99, 9500);
    check(state.FindTrack(1) == nullptr && state.FindTrack(3) && state.FindTrack(3)->Samples == 2 &&
              newcomer.Guid == 99 && newcomer.Samples == 0,
          "an enemy beyond the tracks takes the one sampled longest ago");

    // The fight model (the lab's numbers): bars per healer kit, skill band and Patience
    auto const run = [&](HealerKit kit, Policy mode, uint8 skill, uint8 patience, uint8 steadyHpPct)
    {
        Setup setup;
        setup.Kit = kit;
        setup.Mode = mode;
        setup.Skill = skill;
        setup.Patience = patience;
        setup.SteadyHpPct = steadyHpPct;
        setup.Config = lab;
        setup.Seed = modelSeed;
        Result const result = Simulate(setup);
        Setup off = setup;
        off.Mode = Policy::NoHold;
        say(ResultLine(KitName(kit), setup, result, mode == Policy::NoHold ? result : Simulate(off)));
        return result;
    };
    Result const castOff = run(HealerKit::Cast, Policy::NoHold, 80, 50, 0);
    Result const castRule = run(HealerKit::Cast, Policy::Rule, 80, 50, 0);
    Result const cast80 = run(HealerKit::Cast, Policy::Hold, 80, 50, 0);
    Result const cast50 = run(HealerKit::Cast, Policy::Hold, 50, 50, 0);
    Result const cast20 = run(HealerKit::Cast, Policy::Hold, 20, 50, 0);
    run(HealerKit::Cast, Policy::Hold, 80, 0, 0);
    run(HealerKit::Cast, Policy::Hold, 80, 100, 0);
    Result const mixedOff = run(HealerKit::Mixed, Policy::NoHold, 80, 50, 0);
    run(HealerKit::Mixed, Policy::Rule, 80, 50, 0);
    Result const mixed80 = run(HealerKit::Mixed, Policy::Hold, 80, 50, 0);
    run(HealerKit::Mixed, Policy::Hold, 50, 50, 0);
    run(HealerKit::Mixed, Policy::Hold, 20, 50, 0);
    Result const instantOff = run(HealerKit::Instant, Policy::NoHold, 80, 50, 0);
    run(HealerKit::Instant, Policy::Rule, 80, 50, 0);
    Result const instant80 = run(HealerKit::Instant, Policy::Hold, 80, 50, 0);
    Result const steady0 = run(HealerKit::Instant, Policy::Hold, 80, 0, 30);
    Result const steady = run(HealerKit::Instant, Policy::Hold, 80, 50, 30);
    Result const steady100 = run(HealerKit::Instant, Policy::Hold, 80, 100, 30);
    check(cast80.HealShare() >= 0.60f,
          "model: against a cast healer skill 80 puts at least 60 % of its kicks on heals");
    check(cast80.SavingKicks > castOff.SavingKicks && cast80.Deaths > castOff.Deaths,
          "model: against a cast healer the hold kicks more heals on a low friend and kills more than no hold");
    check(cast80.OwnDeaths < castRule.OwnDeaths,
          "model: against a cast healer the hold loses fewer friends than G5b's rule");
    check(cast80.VainShare() <= 0.15f, "model: against a cast healer at most 15 % of the episodes are vain");
    check(instant80.HeldShare() <= 0.15f, "model: against an instant healer at most 15 % of the damage casts are held");
    check(instant80.DamageKicks * 5 >= instantOff.DamageKicks * 4,
          "model: against an instant healer the hold keeps at least 80 % of the damage kicks");
    check(
        mixed80.HeldShare() <= 0.15f && mixed80.HealShare() > instant80.HealShare(),
        "model: against a mixed healer at most 15 % of the damage casts are held, the heal share above the instant's");
    check(mixed80.SavingKicks * 10 >= mixedOff.SavingKicks * 12 &&
              mixed80.OwnDeaths <= mixedOff.OwnDeaths + mixed80.Fights / 50,
          StringFormat("model: against a mixed healer the trend keeps the kick for the heal on a low friend ({:.2f} "
                       "saving kicks per fight against no hold's {:.2f}) at no more than 0.02 own deaths per fight",
                       mixed80.PerFight(mixed80.SavingKicks), mixedOff.PerFight(mixedOff.SavingKicks)));
    check(cast80.KicksPerCast() >= cast50.KicksPerCast() && cast50.KicksPerCast() >= cast20.KicksPerCast() &&
              cast20.Kicks * 5 <= cast80.Kicks * 2,
          StringFormat("model: kicks fall with skill, skill 20 makes at most 40 % of skill 80's ({:.0f} %)",
                       cast80.Kicks ? 100.0f * cast20.Kicks / cast80.Kicks : 0.0f));
    check(
        cast80.SavingKicks > cast50.SavingKicks && cast50.SavingKicks > cast20.SavingKicks,
        StringFormat("model: the heal kicks on a low friend fall with skill ({:.2f} / {:.2f} / {:.2f} per fight; heal "
                     "share {:.1f} / {:.1f} / {:.1f} %, no order: holding is driven by pressure)",
                     cast80.PerFight(cast80.SavingKicks), cast50.PerFight(cast50.SavingKicks),
                     cast20.PerFight(cast20.SavingKicks), 100.0f * cast80.HealShare(), 100.0f * cast50.HealShare(),
                     100.0f * cast20.HealShare()));
    check(steady0.Expired > steady100.Expired && steady100.HeldShare() > steady0.HeldShare(),
          "model: under steady pressure Patience 0 runs out of holds more often, Patience 100 holds more damage casts");
    check(steady.Expired >= steady.Fights && steady.Episodes >= 2 * steady.Fights,
          "model: under steady pressure an expired episode re-arms (at least 2 episodes and 1 expiry per fight)");

    // The policies' worth: per kit the shortfall of net deaths (enemy minus own) from the best of the three policies.
    // How much a damage kick is worth depends on the kicker's healer: at 3 % per second no hold is best, at 4.5 % the
    // rule; the hold policy's worst shortfall over the kits and the three calibrations is the smallest
    std::array<float, 3> worst{};
    for (float ownHeal : {0.3f, SIM_OWN_HEAL_PER_STEP, 0.45f})
    {
        for (uint32 kit = 0; kit < KIT_COUNT; ++kit)
        {
            std::array<float, 3> net{};
            for (uint32 mode = 0; mode < 3; ++mode)
            {
                Setup setup;
                setup.Kit = static_cast<HealerKit>(kit);
                setup.Mode = static_cast<Policy>(mode);
                setup.Config = lab;
                setup.OwnHealPerStep = ownHeal;
                setup.Seed = modelSeed;
                net[mode] = Simulate(setup).NetDeaths();
            }

            float const best = *std::max_element(net.begin(), net.end());
            for (uint32 mode = 0; mode < 3; ++mode)
                worst[mode] = std::max(worst[mode], best - net[mode]);
        }
    }

    say(
        StringFormat("model: worst shortfall of net deaths per fight from the best policy: no hold {:.2f}, G5b rule "
                     "{:.2f}, hold policy {:.2f}",
                     worst[0], worst[1], worst[2]));
    check(worst[2] < worst[0] && worst[2] < worst[1],
          "model: the hold policy's worst shortfall from the best policy is below no hold's and the rule's");
}
}  // namespace ArenaInterruptSim

#endif
