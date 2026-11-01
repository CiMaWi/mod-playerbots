/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENADEFENSIVE_H
#define _PLAYERBOT_ARENADEFENSIVE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "ArenaDefensiveConfig.h"
#include "BotAttributes.h"
#include "Common.h"
#include "NaturalReaction.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// Defensives on burst and the partner protected (G5d), the pure parts. A bot samples its own health every SampleMs
// into a small ring; "burst" is the health lost within BurstWindowMs at or above a skill-lerped share, "focused" is
// enough enemy players with the bot as their victim or cast target (what the combat log and the cast bars show,
// DT.12). The self-defensive trigger fires at or below a skill-lerped health line AND (burst OR focused), or at the
// critical line, or (low skill) at the opener; low effort presses late. The partner is protected only in PvP (the
// Protect tactic's scope, DT.3) below a skill-lerped health line. The opponent memory keeps the health at which each
// enemy was first seen with a defensive up; a bot with enough Foresight holds its burst cooldowns while the enemy's
// defensive is up and, when that defensive may be ready again, until the enemy nears the health it used it at. The
// game side is DefensiveTacticStrategy.h (CcTacticStrategy.cpp); the numbers are DefensiveConfig's.

// Defensive auras a player sees on an enemy's buff bar (FocusStrategy's list) and the immunities
constexpr std::array<uint32, 16> DEFENSIVE_AURAS = {
    33206,  // Pain Suppression
    47788,  // Guardian Spirit
    22812,  // Barkskin
    61336,  // Survival Instincts
    871,    // Shield Wall
    12975,  // Last Stand
    498,    // Divine Protection
    6940,   // Hand of Sacrifice
    5277,   // Evasion
    26669,  // Evasion
    31224,  // Cloak of Shadows
    48792,  // Icebound Fortitude
    48707,  // Anti-Magic Shell
    47585,  // Dispersion
    19263,  // Deterrence
    30823,  // Shamanistic Rage
};

constexpr std::array<uint32, 5> DEFENSIVE_IMMUNITY_AURAS = {
    45438,  // Ice Block
    642,    // Divine Shield
    1022,   // Hand of Protection (ranks 1-3)
    5599,  10278,
};

// A cooldown spent on oneself or a partner (first ranks): tactics.defSelf / tactics.defPartner
enum class DefensiveCastKind : uint8
{
    None = 0,
    Self,
    Partner
};

constexpr std::array<uint32, 12> SELF_DEFENSIVE_SPELLS = {
    45438,  // Ice Block
    642,    // Divine Shield
    31224,  // Cloak of Shadows
    5277,   // Evasion
    26669,  // Evasion (rank 2)
    48792,  // Icebound Fortitude
    19263,  // Deterrence
    22812,  // Barkskin
    61336,  // Survival Instincts
    33206,  // Pain Suppression (on itself)
    47585,  // Dispersion
    871,    // Shield Wall
};

constexpr std::array<uint32, 5> PARTNER_DEFENSIVE_SPELLS = {
    1022,   // Hand of Protection (blessing of protection)
    33206,  // Pain Suppression (on a partner)
    3411,   // Intervene
    47788,  // Guardian Spirit
    6940,   // Hand of Sacrifice
};

inline DefensiveCastKind ClassifyDefensiveCast(uint32 firstRankId, bool onSelf)
{
    auto const in = [firstRankId](auto const& list)
    { return std::find(list.begin(), list.end(), firstRankId) != list.end(); };
    if (!onSelf && in(PARTNER_DEFENSIVE_SPELLS))
        return DefensiveCastKind::Partner;

    return onSelf && in(SELF_DEFENSIVE_SPELLS) ? DefensiveCastKind::Self : DefensiveCastKind::None;
}

// The bot's own health, sampled every SampleMs (oldest overwritten)
constexpr uint32 DEFENSIVE_RING_SAMPLES = 16;

struct HealthRing
{
    std::array<uint32, DEFENSIVE_RING_SAMPLES> Ms{};
    std::array<uint8, DEFENSIVE_RING_SAMPLES> Hp{};
    uint32 Count = 0;
    uint32 Next = 0;

    void Clear() { *this = HealthRing(); }

    void Push(uint32 nowMs, uint32 hpPct)
    {
        Ms[Next] = nowMs;
        Hp[Next] = static_cast<uint8>(std::min(hpPct, 100u));
        Next = (Next + 1) % DEFENSIVE_RING_SAMPLES;
        Count = std::min(Count + 1, DEFENSIVE_RING_SAMPLES);
    }

    uint32 LastMs() const { return Count ? Ms[(Next + DEFENSIVE_RING_SAMPLES - 1) % DEFENSIVE_RING_SAMPLES] : 0; }

    // The health lost within windowMs: the highest sample in the window minus the health now (0 when it rose)
    uint32 LostPct(uint32 nowMs, uint32 hpPct, uint32 windowMs) const
    {
        uint32 peak = std::min(hpPct, 100u);
        for (uint32 back = 0; back < Count; ++back)
        {
            uint32 const index = (Next + 2 * DEFENSIVE_RING_SAMPLES - 1 - back) % DEFENSIVE_RING_SAMPLES;
            if (nowMs - Ms[index] > windowMs)
                break;

            peak = std::max<uint32>(peak, Hp[index]);
        }

        return peak - std::min(hpPct, 100u);
    }
};

// The numbers of one bot, cached once per skill load (SetPvpSkill); skill 0 gives the skill-99 values
struct DefensiveParams
{
    uint32 WindowMs = 2000;
    uint32 SampleMs = 250;
    uint32 BurstLossPct = 30;
    uint32 FocusedMinEnemies = 2;
    uint32 HpPct = 60;  // the line: burst or focused at or below it
    uint32 FocusedHpBonusPct = 15;
    uint32 CriticalHpPct = 20;
    uint32 OpenerPct = 0;  // the chance per match to spend the cooldowns at the first contact
    uint32 LateMs = 0;     // the trigger must hold this long before the bot presses (low effort)
    uint32 MinGapMs = 1500;
    bool PotionEnable = true;
    uint32 PotionHpPct = 35;
    uint32 ProtectHpPct = 40;
    uint32 ProtectDistanceYards = 30;
    bool OpponentKnows = true;  // Foresight reaches Opponent.FromSkill and Opponent.Enable
    uint32 OpponentLeadPct = 20;
    uint32 OpponentExpireMs = 120000;
};

constexpr uint32 DEFENSIVE_PROTECT_TANK_HP_PCT = 10;  // a tank is protected only this low (the old picker's line)
constexpr uint32 DEFENSIVE_OPENER_WINDOW_MS = 8000;   // "at the start": this long after the first combat of a match
constexpr uint32 DEFENSIVE_OBSERVE_MS = 500;          // the enemy defensive scan at most this often
constexpr uint32 DEFENSIVE_HOLD_EPISODE_MS = 1000;    // burst holds closer than this are one episode
constexpr uint64 DEFENSIVE_OPENER_SALT = 0x3C6EF372FE94F82BULL;

inline float DefensiveAbilityPosition(uint8 ability) { return ability == 0 ? 1.0f : SkillPosition(ability); }

inline float DefensiveTraitShift(uint8 trait)
{
    return (static_cast<float>(std::min<uint8>(trait, 100)) - 50.0f) / 50.0f;
}

inline uint32 DefensivePct(float value) { return static_cast<uint32>(std::clamp(value, 0.0f, 100.0f) + 0.5f); }

// Wisdom carries the judgement (burst share, health lines, the opener), Foresight the opponent memory, Caution moves
// the self line, the effort (TacticGate's, 0-100) the late press. With the attribute defaults (abilities = skill,
// traits 50) it is the plain skill lerp.
inline DefensiveParams ComputeDefensiveParams(uint8 skill, BotAttributeValues const& attributes, uint8 effort,
                                              float exponent, DefensiveConfig const& config)
{
    bool const full = skill == 0;
    float const wisdom = full ? 1.0f : DefensiveAbilityPosition(attributes.Get(BotAttribute::Wisdom));
    float const foresight = full ? 1.0f : DefensiveAbilityPosition(attributes.Get(BotAttribute::Foresight));
    float const caution = full ? 0.0f : DefensiveTraitShift(attributes.Get(BotAttribute::Caution));
    auto const lerp = [exponent](uint32 atSkill1, uint32 atSkill99, float position)
    { return SkillLerp(static_cast<float>(atSkill1), static_cast<float>(atSkill99), position, exponent); };

    DefensiveParams params;
    params.WindowMs = std::max(config.BurstWindowMs, 1u);
    params.SampleMs = std::max(config.SampleMs, 1u);
    params.BurstLossPct =
        std::max(DefensivePct(lerp(config.BurstLossPctAtSkill1, config.BurstLossPctAtSkill99, wisdom)), 1u);
    params.FocusedMinEnemies = config.FocusedMinEnemies;
    params.HpPct = DefensivePct(lerp(config.HpPctAtSkill1, config.HpPctAtSkill99, wisdom) +
                                static_cast<float>(config.CautionShiftPct) * caution);
    params.FocusedHpBonusPct = config.FocusedHpBonusPct;
    params.CriticalHpPct = std::min(config.CriticalHpPct, 100u);
    params.OpenerPct = DefensivePct(lerp(config.OpenerPctAtSkill1, config.OpenerPctAtSkill99, wisdom));
    float const effortShare = full ? 1.0f : static_cast<float>(std::min<uint8>(effort, 100)) / 100.0f;
    params.LateMs = static_cast<uint32>(std::max(
        0.0f,
        static_cast<float>(config.LateMsAtEffort0) +
            (static_cast<float>(config.LateMsAtEffort100) - static_cast<float>(config.LateMsAtEffort0)) * effortShare +
            0.5f));
    params.MinGapMs = config.MinGapMs;
    params.PotionEnable = config.PotionEnable != 0;
    params.PotionHpPct = std::min(config.PotionHpPct, 100u);
    params.ProtectHpPct = DefensivePct(lerp(config.ProtectHpPctAtSkill1, config.ProtectHpPctAtSkill99, wisdom));
    params.ProtectDistanceYards = config.ProtectDistanceYards;
    params.OpponentKnows =
        config.OpponentEnable != 0 && (full || attributes.Get(BotAttribute::Foresight) >= config.OpponentFromSkill);
    params.OpponentLeadPct = DefensivePct(lerp(0, config.OpponentLeadPctAtForesight99, foresight));
    params.OpponentExpireMs = config.OpponentExpireMs;
    return params;
}

// Why the bot presses a self-defensive now
enum class DefensiveReason : uint8
{
    None = 0,
    Burst,
    Focused,
    Critical,
    Opener
};

inline char const* DefensiveReasonName(DefensiveReason reason)
{
    switch (reason)
    {
        case DefensiveReason::Burst:
            return "burst";
        case DefensiveReason::Focused:
            return "focused";
        case DefensiveReason::Critical:
            return "critical";
        case DefensiveReason::Opener:
            return "opener";
        default:
            return "none";
    }
}

struct DefensiveInput
{
    uint32 HpPct = 100;  // 0: dead
    uint32 LostPct = 0;  // HealthRing::LostPct
    uint32 FocusedBy = 0;
    bool OpenerDue = false;
};

inline bool DefensiveBurst(uint32 lostPct, DefensiveParams const& params) { return lostPct >= params.BurstLossPct; }

inline bool DefensiveFocused(uint32 focusedBy, DefensiveParams const& params)
{
    return params.FocusedMinEnemies != 0 && focusedBy >= params.FocusedMinEnemies;
}

inline DefensiveReason SelfDefensiveReason(DefensiveInput const& input, DefensiveParams const& params)
{
    if (input.HpPct == 0)
        return DefensiveReason::None;

    if (input.OpenerDue)
        return DefensiveReason::Opener;

    if (input.HpPct <= params.CriticalHpPct)
        return DefensiveReason::Critical;

    bool const focused = DefensiveFocused(input.FocusedBy, params);
    uint32 const line = std::min(params.HpPct + (focused ? params.FocusedHpBonusPct : 0), 100u);
    if (input.HpPct > line)
        return DefensiveReason::None;

    if (DefensiveBurst(input.LostPct, params))
        return DefensiveReason::Burst;

    return focused ? DefensiveReason::Focused : DefensiveReason::None;
}

// The press: the reason must have held LateMs (pendingSinceMs, the bot's own clock; 0 none), and the last
// self-defensive (lastUseMs, 0 none) be MinGapMs ago. The opener does not wait.
inline bool DefensiveDue(DefensiveReason reason, uint32& pendingSinceMs, uint32 lastUseMs, uint32 nowMs,
                         DefensiveParams const& params)
{
    if (reason == DefensiveReason::None)
    {
        pendingSinceMs = 0;
        return false;
    }

    if (pendingSinceMs == 0)
        pendingSinceMs = std::max(nowMs, 1u);

    if (lastUseMs != 0 && nowMs - lastUseMs < params.MinGapMs)
        return false;

    return reason == DefensiveReason::Opener || nowMs - pendingSinceMs >= params.LateMs;
}

// One draw per bot and match: this bot spends its cooldowns at the first contact
inline bool DefensiveOpenerDraw(uint64 guid, uint32 matchId, uint32 openerPct)
{
    if (matchId == 0 || openerPct == 0)
        return false;

    uint64 const hash = SplitMix64((guid << 32) ^ matchId ^ DEFENSIVE_OPENER_SALT);
    return UnitFromHash(hash) * 100.0 < static_cast<double>(openerPct);
}

inline bool DefensiveOpenerDue(bool drawn, uint32 combatSinceMs, uint32 nowMs)
{
    return drawn && combatSinceMs != 0 && nowMs - combatSinceMs <= DEFENSIVE_OPENER_WINDOW_MS;
}

// The partner (attacked now: an enemy's victim or cast target, within ProtectDistanceYards) needs protection
inline bool PartnerNeedsProtect(uint32 hpPct, bool attacked, bool tank, DefensiveParams const& params)
{
    if (!attacked || hpPct == 0)
        return false;

    return hpPct <= (tank ? std::min(DEFENSIVE_PROTECT_TANK_HP_PCT, params.ProtectHpPct) : params.ProtectHpPct);
}

// Burst cooldowns against an enemy player (BoostTrigger): held while its defensive is up, and, when its defensive was
// seen long ago (it may be ready again), until it nears the health it used it at (+ the Foresight lead). A defensive
// seen recently is spent: the burst goes now. Unknown: as before.
enum class BurstHold : uint8
{
    None = 0,
    DefensiveUp,
    Bait
};

inline char const* BurstHoldName(BurstHold hold)
{
    switch (hold)
    {
        case BurstHold::DefensiveUp:
            return "up";
        case BurstHold::Bait:
            return "bait";
        default:
            return "none";
    }
}

struct BurstHoldInput
{
    uint32 EnemyHpPct = 100;
    bool DefensiveUp = false;
    uint32 RememberedHpPct = 0;  // 0: never seen
    uint32 RememberedAgeMs = 0;
};

inline BurstHold DecideBurstHold(BurstHoldInput const& input, DefensiveParams const& params)
{
    if (!params.OpponentKnows)
        return BurstHold::None;

    if (input.DefensiveUp)
        return BurstHold::DefensiveUp;

    if (input.RememberedHpPct == 0 || input.RememberedAgeMs <= params.OpponentExpireMs)
        return BurstHold::None;

    return input.EnemyHpPct > std::min(input.RememberedHpPct + params.OpponentLeadPct, 100u) ? BurstHold::Bait
                                                                                             : BurstHold::None;
}

// tactics.deathsDefReady counts a death once: at the lethal hit (before the kill can end the arena and the harness
// reads the counters) or, for a death no damage hook saw (an instakill), at the bot's next check. Alive again arms it.
struct DefensiveDeathLatch
{
    bool Noted = false;  // this death is counted or checked

    // The lethal hit on a live bot: true the first time for this death
    bool OnLethalHit()
    {
        if (Noted)
            return false;

        Noted = true;
        return true;
    }

    // The bot's check: true for a death (alive at the last check, dead now) that no lethal hit noted
    bool OnCheck(bool wasAlive, bool alive)
    {
        if (alive)
        {
            Noted = false;
            return false;
        }

        bool const unseen = wasAlive && !Noted;
        Noted = true;
        return unseen;
    }
};

// The per-bot state (PlayerbotAI::GetDefensiveState; the bot's own update and its own casts only)
struct DefensiveBotState
{
    DefensiveParams Params;
    HealthRing Ring;
    uint8 FocusedBy = 0;  // at the last sample
    uint32 PendingSinceMs = 0;
    uint32 LastUseMs = 0;  // the bot's last self-defensive cast (any path)
    uint32 LastPartnerMs = 0;
    uint32 MatchId = 0;
    uint32 CombatSinceMs = 0;  // the first combat of the match (the opener)
    bool OpenerDrawn = false;
    bool WasAlive = true;
    DefensiveDeathLatch Death;  // tactics.deathsDefReady
    uint32 LastObserveMs = 0;   // the enemy defensive scan
    uint32 LastHoldMs = 0;
    uint32 LastBurstMs = 0;  // the last burst counted (tactics.defBurst: one per window)
    // What the bot did (tactics show)
    uint32 Presses = 0;
    uint32 SelfUses = 0;
    uint32 PartnerUses = 0;
    uint32 Holds = 0;
    DefensiveReason LastReason = DefensiveReason::None;
    uint32 LastPressMs = 0;
};

// tactics selftest defensive (also the offline harness): the pure policy with synthetic health streams
template <typename Check>
void RunDefensiveChecks(Check&& check)
{
    DefensiveConfig const config;
    BotAttributeValues skill99;
    BotAttributeValues skill1;
    BotAttributeValues skill40;
    BotAttributeValues skill80;
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
    {
        bool const trait = index >= BOT_ABILITY_COUNT;
        skill99.Value[index] = trait ? 50 : 99;
        skill1.Value[index] = trait ? 50 : 1;
        skill40.Value[index] = trait ? 50 : 40;
        skill80.Value[index] = trait ? 50 : 80;
    }

    DefensiveParams const p99 = ComputeDefensiveParams(99, skill99, 100, 1.0f, config);
    DefensiveParams const p0 = ComputeDefensiveParams(0, skill1, 0, 1.0f, config);
    DefensiveParams const p1 = ComputeDefensiveParams(1, skill1, 0, 1.0f, config);
    DefensiveParams const p40 = ComputeDefensiveParams(40, skill40, 50, 1.0f, config);
    DefensiveParams const p80 = ComputeDefensiveParams(80, skill80, 80, 1.0f, config);

    // A health stream: from 100 % down by lossPct over spanMs, sampled every 250 ms; the loss the ring sees at its end
    auto const stream = [](uint32 lossPct, uint32 spanMs, uint32 windowMs)
    {
        HealthRing ring;
        uint32 const startMs = 50000;
        uint32 hp = 100;
        for (uint32 at = 0; at <= spanMs; at += 250)
        {
            hp = 100 - lossPct * at / std::max(spanMs, 1u);
            ring.Push(startMs + at, hp);
        }

        return ring.LostPct(startMs + spanMs, hp, windowMs);
    };

    // The exit's numbers: 35 % lost in 2 s is burst, 10 % is not (skill 99, skill 0 and skill 80)
    check(stream(35, 2000, p99.WindowMs) == 35, "the ring sees 35 % lost over 2 s");
    check(DefensiveBurst(stream(35, 2000, p99.WindowMs), p99), "35 % in 2 s is burst at skill 99");
    check(DefensiveBurst(stream(35, 2000, p0.WindowMs), p0), "35 % in 2 s is burst at skill 0 (full ability)");
    check(DefensiveBurst(stream(35, 2000, p80.WindowMs), p80), "35 % in 2 s is burst at skill 80");
    check(!DefensiveBurst(stream(10, 2000, p99.WindowMs), p99), "10 % in 2 s is no burst at skill 99");
    check(!DefensiveBurst(stream(10, 500, p99.WindowMs), p99), "10 % in 0.5 s is no burst at skill 99");
    // The window: 35 % spread over 4 s is a steady loss, not burst
    check(!DefensiveBurst(stream(35, 4000, p99.WindowMs), p99), "35 % over 4 s is no burst (window 2 s)");
    // Heals: the loss is from the window's peak, a heal back up is no loss
    {
        HealthRing ring;
        ring.Push(1000, 90);
        ring.Push(1250, 50);
        ring.Push(1500, 95);
        check(ring.LostPct(1500, 95, 2000) == 0, "a heal back above the window's peak is no loss");
        check(ring.LostPct(1600, 55, 2000) == 40, "the loss is the window's peak minus the health now");
        HealthRing wrap;
        for (uint32 index = 0; index < 3 * DEFENSIVE_RING_SAMPLES; ++index)
            wrap.Push(index * 250, 100 - index);
        check(wrap.Count == DEFENSIVE_RING_SAMPLES && wrap.LastMs() == (3 * DEFENSIVE_RING_SAMPLES - 1) * 250,
              "the ring keeps the newest samples");
    }

    // Lines by skill: burst share falls, the health line rises with skill
    check(p99.BurstLossPct == 30 && p1.BurstLossPct == 55, "burst share 55 % at skill 1, 30 % at skill 99");
    check(p40.BurstLossPct >= 44 && p40.BurstLossPct <= 46, "burst share about 45 % at skill 40 (the research)");
    check(!DefensiveBurst(35, p1), "35 % is no burst at skill 1");
    check(p99.HpPct == 60 && p1.HpPct == 25, "health line 25 % at skill 1, 60 % at skill 99");
    check(p0.HpPct == p99.HpPct && p0.LateMs == 0 && p0.OpenerPct == 0, "skill 0 is the skill-99 bot");
    {
        BotAttributeValues cautious = skill80;
        BotAttributeValues bold = skill80;
        cautious.Value[static_cast<uint32>(BotAttribute::Caution)] = 100;
        bold.Value[static_cast<uint32>(BotAttribute::Caution)] = 0;
        uint32 const high = ComputeDefensiveParams(80, cautious, 80, 1.0f, config).HpPct;
        uint32 const low = ComputeDefensiveParams(80, bold, 80, 1.0f, config).HpPct;
        check(high == p80.HpPct + 10 && low + 10 == p80.HpPct, "Caution 100 presses 10 points earlier, 0 later");
    }
    bool monotone = true;
    uint32 lastLoss = 101;
    uint32 lastLine = 0;
    for (uint8 skill = 1; skill <= 99; ++skill)
    {
        BotAttributeValues values = skill1;
        for (uint32 index = 0; index < BOT_ABILITY_COUNT; ++index)
            values.Value[index] = skill;
        DefensiveParams const params = ComputeDefensiveParams(skill, values, 50, 1.0f, config);
        monotone = monotone && params.BurstLossPct <= lastLoss && params.HpPct >= lastLine;
        lastLoss = params.BurstLossPct;
        lastLine = params.HpPct;
    }
    check(monotone, "the burst share never rises and the health line never falls with skill");

    // The trigger: line AND (burst OR focused), the critical line, the focused bonus
    auto const reason = [](uint32 hp, uint32 lost, uint32 focused, DefensiveParams const& params)
    {
        DefensiveInput input;
        input.HpPct = hp;
        input.LostPct = lost;
        input.FocusedBy = focused;
        return SelfDefensiveReason(input, params);
    };
    check(reason(50, 35, 0, p99) == DefensiveReason::Burst, "skill 99 at 50 % after a burst presses");
    check(reason(70, 35, 0, p99) == DefensiveReason::None, "skill 99 above its line does not press");
    check(reason(50, 10, 0, p99) == DefensiveReason::None, "skill 99 at 50 % without burst or focus waits");
    check(reason(50, 10, 2, p99) == DefensiveReason::Focused, "two enemies on it at 50 %: focused");
    check(reason(70, 10, 2, p99) == DefensiveReason::Focused, "focused adds 15 points to the line");
    check(reason(70, 10, 1, p99) == DefensiveReason::None, "one enemy on it is not focused");
    check(reason(50, 40, 2, p1) == DefensiveReason::None, "skill 1 at 50 % does not press");
    check(reason(18, 0, 0, p1) == DefensiveReason::Critical, "every skill presses at critical health");
    check(reason(0, 50, 2, p99) == DefensiveReason::None, "a dead bot presses nothing");

    // The press: late by effort, the gap after the last use, the opener at once
    {
        uint32 pending = 0;
        check(!DefensiveDue(DefensiveReason::Burst, pending, 0, 10000, p1) && pending == 10000,
              "effort 0 waits its late ms first");
        check(!DefensiveDue(DefensiveReason::Burst, pending, 0, 10000 + p1.LateMs - 1, p1) &&
                  DefensiveDue(DefensiveReason::Burst, pending, 0, 10000 + p1.LateMs, p1),
              "effort 0 presses after 1200 ms");
        check(!DefensiveDue(DefensiveReason::None, pending, 0, 11500, p1) && pending == 0,
              "the reason gone, the wait restarts");
        uint32 fresh = 0;
        check(DefensiveDue(DefensiveReason::Burst, fresh, 0, 20000, p99), "effort 100 presses at once");
        uint32 gap = 0;
        check(!DefensiveDue(DefensiveReason::Burst, gap, 19000, 20000, p99) &&
                  DefensiveDue(DefensiveReason::Burst, gap, 19000, 20500, p99),
              "no second self-defensive within 1.5 s of the last");
        uint32 opener = 0;
        check(DefensiveDue(DefensiveReason::Opener, opener, 0, 5000, p1), "the opener does not wait");
    }

    // The opener: about 40 % of skill-1 bots per match, none at skill 99; its window
    {
        uint32 drawn = 0;
        uint32 drawn99 = 0;
        for (uint64 guid = 1; guid <= 2000; ++guid)
        {
            drawn += DefensiveOpenerDraw(guid, 77, p1.OpenerPct) ? 1 : 0;
            drawn99 += DefensiveOpenerDraw(guid, 77, p99.OpenerPct) ? 1 : 0;
        }
        check(drawn >= 700 && drawn <= 900 && drawn99 == 0, "opener: about 40 % at skill 1, none at skill 99");
        check(!DefensiveOpenerDraw(5, 0, 100), "no opener outside a match");
        check(DefensiveOpenerDue(true, 1000, 8999) && !DefensiveOpenerDue(true, 1000, 9001) &&
                  !DefensiveOpenerDue(true, 0, 1000),
              "the opener lasts 8 s from the first combat");
    }

    // The partner: protected below the skill's line, only while attacked; a tank only very low
    check(PartnerNeedsProtect(35, true, false, p99) && !PartnerNeedsProtect(35, true, false, p1),
          "a partner at 35 %: skill 99 protects, skill 1 waits");
    check(PartnerNeedsProtect(25, true, false, p80), "a partner at 25 % (the forced check): skill 80 protects");
    check(!PartnerNeedsProtect(15, false, false, p99), "a partner nobody attacks needs no protection");
    check(!PartnerNeedsProtect(30, true, true, p99) && PartnerNeedsProtect(8, true, true, p99),
          "a tank only at 10 % or below");

    // The casts counted
    check(ClassifyDefensiveCast(45438, true) == DefensiveCastKind::Self, "ice block is a self-defensive");
    check(ClassifyDefensiveCast(33206, true) == DefensiveCastKind::Self &&
              ClassifyDefensiveCast(33206, false) == DefensiveCastKind::Partner,
          "pain suppression on itself is self, on the partner partner");
    check(ClassifyDefensiveCast(1022, false) == DefensiveCastKind::Partner, "blessing of protection is a partner save");
    check(ClassifyDefensiveCast(133, false) == DefensiveCastKind::None, "a fireball is none");

    // The opponent memory and the burst hold (Foresight)
    {
        OpponentMemory memory;
        memory.Clear(9);
        memory.NoteDefensive(42, 35, 1000, p99.OpponentExpireMs);
        memory.NoteDefensive(42, 20, 3000, p99.OpponentExpireMs);
        check(memory.DefensiveHpPct(42, 3000, p99.OpponentExpireMs) == 35, "the first sighting's health is kept");
        check(memory.DefensiveHpPct(42, 1000 + p99.OpponentExpireMs + 1, p99.OpponentExpireMs) == 0,
              "the memory expires");

        BurstHoldInput input;
        input.EnemyHpPct = 80;
        input.DefensiveUp = true;
        check(DecideBurstHold(input, p99) == BurstHold::DefensiveUp, "no burst into a defensive that is up");
        input.DefensiveUp = false;
        check(DecideBurstHold(input, p99) == BurstHold::None, "an enemy never seen with a defensive: burst");
        input.RememberedHpPct = 35;
        input.RememberedAgeMs = 5000;
        check(DecideBurstHold(input, p99) == BurstHold::None, "its defensive used 5 s ago is spent: burst");
        input.RememberedAgeMs = p99.OpponentExpireMs + 1;
        check(DecideBurstHold(input, p99) == BurstHold::Bait, "its defensive may be back: hold at 80 %");
        input.EnemyHpPct = 50;
        check(DecideBurstHold(input, p99) == BurstHold::None, "within the lead of its defensive health: burst");
        input.EnemyHpPct = 80;
        check(p1.OpponentKnows == false && DecideBurstHold(input, p1) == BurstHold::None,
              "below Opponent.FromSkill the bot does not use the memory");
        check(p99.OpponentLeadPct == 20 && p40.OpponentLeadPct < p80.OpponentLeadPct, "the lead grows with Foresight");
    }

    // A synthetic fight: a steady 4 %/s pressure never reads as burst; healed to 95 %, a spike of 46 % in 1.75 s does
    // at skill 80 (pressed once the health is under its line)
    {
        HealthRing ring;
        uint32 hp = 100;
        bool steadyBurst = false;
        for (uint32 at = 0; at <= 15000; at += 250)
        {
            hp = 100 - at * 4 / 1000;
            ring.Push(at, hp);
            steadyBurst = steadyBurst || DefensiveBurst(ring.LostPct(at, hp, p80.WindowMs), p80);
        }
        check(!steadyBurst && hp == 40, "a steady 4 %/s loss is never burst");
        DefensiveReason spike = DefensiveReason::None;
        ring.Push(15000, 95);
        for (uint32 at = 15250; at <= 16750; at += 250)
        {
            hp = 95 - (at - 15000) * 40 / 1500;
            ring.Push(at, hp);
            DefensiveInput input;
            input.HpPct = hp;
            input.LostPct = ring.LostPct(at, hp, p80.WindowMs);
            if (spike == DefensiveReason::None)
                spike = SelfDefensiveReason(input, p80);
        }
        check(spike == DefensiveReason::Burst, "a healed bot losing 46 % in 1.75 s presses on burst at skill 80");
    }

    // tactics.deathsDefReady: the arena's last death ends the match inside the kill, so the lethal hit counts it (the
    // harness reads the counters before the dead bot's next check); the check after it does not count it again; a
    // death no hit saw counts once at the next check; alive again arms the latch
    {
        DefensiveDeathLatch latch;
        bool const atHit = latch.OnLethalHit();
        bool const secondHit = latch.OnLethalHit();
        bool const checkAfterHit = latch.OnCheck(true, false);
        check(atHit && !secondHit && !checkAfterHit, "a death is counted once, at the lethal hit");
        bool const aliveAgain = latch.OnCheck(false, true);
        bool const unseen = latch.OnCheck(true, false);
        bool const stillDead = latch.OnCheck(false, false);
        check(!aliveAgain && unseen && !stillDead, "a death no hit saw is counted once, at the next check");
        latch.OnCheck(false, true);
        bool const rearmed = latch.OnLethalHit();
        check(rearmed && !latch.OnCheck(true, false), "alive again: the next lethal hit counts, its check does not");
    }
}

#endif
