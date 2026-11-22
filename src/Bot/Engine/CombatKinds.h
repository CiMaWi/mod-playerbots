/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATKINDS_H
#define _PLAYERBOT_COMBATKINDS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// What a spell can answer, read from the game's spell data (combat-foundation CFM3b, MEASURES §6.2: combat_known's
// kinds), and the small rules of the sample rows (MEASURES §1), free of core includes so
// tests/combat-kinds-selftest.cpp proves them offline. CombatRecord.cpp fills SpellFacts from SpellInfo; the numbers
// in Data are the core's enum values, checked there by static_assert.
namespace CombatKinds
{
// The bits of combat_known's kinds (world-log.md lists them; the report reads them from there)
enum Kind : std::uint32_t
{
    INTERRUPT = 1u << 0,
    DISPEL_MAGIC = 1u << 1,
    DISPEL_CURSE = 1u << 2,
    DISPEL_DISEASE = 1u << 3,
    DISPEL_POISON = 1u << 4,
    PURGE = 1u << 5,
    STEAL = 1u << 6,
    CONTROL = 1u << 7,
    TAUNT = 1u << 8,
    THREAT_DROP = 1u << 9,
    SELF_HEAL = 1u << 10,
    HEAL = 1u << 11,
    DEFENSIVE = 1u << 12,
    FINISHER = 1u << 13,
    CLOSE = 1u << 14,
    ESCAPE = 1u << 15,
    AREA = 1u << 16,
    RECOVER = 1u << 17,
    COOLDOWN = 1u << 18
};

namespace Data
{
// SpellEffects
constexpr std::uint32_t EFFECT_HEAL = 10;
constexpr std::uint32_t EFFECT_PERSISTENT_AREA_AURA = 27;
constexpr std::uint32_t EFFECT_LEAP = 29;
constexpr std::uint32_t EFFECT_ENERGIZE = 30;
constexpr std::uint32_t EFFECT_DISPEL = 38;
constexpr std::uint32_t EFFECT_JUMP = 41;
constexpr std::uint32_t EFFECT_JUMP_DEST = 42;
constexpr std::uint32_t EFFECT_THREAT = 63;
constexpr std::uint32_t EFFECT_HEAL_MAX_HEALTH = 67;
constexpr std::uint32_t EFFECT_INTERRUPT_CAST = 68;
constexpr std::uint32_t EFFECT_CHARGE = 96;
constexpr std::uint32_t EFFECT_DISPEL_MECHANIC = 108;
constexpr std::uint32_t EFFECT_ATTACK_ME = 114;
constexpr std::uint32_t EFFECT_PULL_TOWARDS = 124;
constexpr std::uint32_t EFFECT_MODIFY_THREAT_PERCENT = 125;
constexpr std::uint32_t EFFECT_STEAL_BENEFICIAL_BUFF = 126;
constexpr std::uint32_t EFFECT_HEAL_PCT = 136;
constexpr std::uint32_t EFFECT_ENERGIZE_PCT = 137;
constexpr std::uint32_t EFFECT_LEAP_BACK = 138;
constexpr std::uint32_t EFFECT_PULL_TOWARDS_DEST = 145;
constexpr std::uint32_t EFFECT_CHARGE_DEST = 149;
// AuraType
constexpr std::uint32_t AURA_MOD_POSSESS = 2;
constexpr std::uint32_t AURA_MOD_CONFUSE = 5;
constexpr std::uint32_t AURA_MOD_CHARM = 6;
constexpr std::uint32_t AURA_MOD_FEAR = 7;
constexpr std::uint32_t AURA_PERIODIC_HEAL = 8;
constexpr std::uint32_t AURA_MOD_THREAT = 10;
constexpr std::uint32_t AURA_MOD_TAUNT = 11;
constexpr std::uint32_t AURA_MOD_STUN = 12;
constexpr std::uint32_t AURA_OBS_MOD_HEALTH = 20;
constexpr std::uint32_t AURA_OBS_MOD_POWER = 21;
constexpr std::uint32_t AURA_PERIODIC_ENERGIZE = 24;
constexpr std::uint32_t AURA_MOD_ROOT = 26;
constexpr std::uint32_t AURA_MOD_SILENCE = 27;
constexpr std::uint32_t AURA_MOD_INCREASE_SPEED = 31;
constexpr std::uint32_t AURA_SCHOOL_IMMUNITY = 39;
constexpr std::uint32_t AURA_DAMAGE_IMMUNITY = 40;
constexpr std::uint32_t AURA_MOD_PARRY_PERCENT = 47;
constexpr std::uint32_t AURA_MOD_DODGE_PERCENT = 49;
constexpr std::uint32_t AURA_MOD_PACIFY_SILENCE = 60;
constexpr std::uint32_t AURA_FEIGN_DEATH = 66;
constexpr std::uint32_t AURA_SCHOOL_ABSORB = 69;
constexpr std::uint32_t AURA_MECHANIC_IMMUNITY = 77;
constexpr std::uint32_t AURA_MOD_DAMAGE_PERCENT_TAKEN = 87;
constexpr std::uint32_t AURA_MOD_TOTAL_THREAT = 103;
constexpr std::uint32_t AURA_MOD_SPEED_ALWAYS = 129;
constexpr std::uint32_t AURA_MOD_SPEED_NOT_STACK = 171;
// Mechanics that take a unit's control (snare, daze and disarm do not)
constexpr std::uint32_t MECHANIC_CHARM = 1;
constexpr std::uint32_t MECHANIC_DISORIENTED = 2;
constexpr std::uint32_t MECHANIC_FEAR = 5;
constexpr std::uint32_t MECHANIC_ROOT = 7;
constexpr std::uint32_t MECHANIC_SILENCE = 9;
constexpr std::uint32_t MECHANIC_SLEEP = 10;
constexpr std::uint32_t MECHANIC_STUN = 12;
constexpr std::uint32_t MECHANIC_FREEZE = 13;
constexpr std::uint32_t MECHANIC_KNOCKOUT = 14;
constexpr std::uint32_t MECHANIC_POLYMORPH = 17;
constexpr std::uint32_t MECHANIC_BANISH = 18;
constexpr std::uint32_t MECHANIC_SHACKLE = 20;
constexpr std::uint32_t MECHANIC_TURN = 23;
constexpr std::uint32_t MECHANIC_HORROR = 24;
constexpr std::uint32_t MECHANIC_SAPPED = 30;
// a slow: no control, but an immunity to it frees the caster
constexpr std::uint32_t MECHANIC_SNARE = 11;
// DispelType
constexpr std::int32_t DISPEL_TYPE_MAGIC = 1;
constexpr std::int32_t DISPEL_TYPE_CURSE = 2;
constexpr std::int32_t DISPEL_TYPE_DISEASE = 3;
constexpr std::int32_t DISPEL_TYPE_POISON = 4;
// AuraStateType: the target's health below 20 % or 35 %
constexpr std::uint32_t STATE_HEALTHLESS_20 = 2;
constexpr std::uint32_t STATE_HEALTHLESS_35 = 13;
}  // namespace Data

constexpr std::uint32_t CONTROL_MIN_MS = 2000;    // a control effect lasts 2 s or longer
constexpr std::uint32_t COOLDOWN_MIN_MS = 30000;  // a cooldown (and a defensive's) is 30 s or longer
constexpr std::size_t EFFECTS = 3;

// Whom an effect lands on, from its first implicit target: the caster, an ally, an enemy, either, or anything else
// (a place, an item)
enum class Aim : std::uint8_t
{
    Other,
    Self,
    Ally,
    Enemy,
    Any
};

struct Effect
{
    std::uint32_t effect = 0;  // 0: the slot holds no effect
    std::uint32_t aura = 0;
    std::uint32_t mechanic = 0;
    std::int32_t misc = 0;   // a dispel's type
    std::int32_t value = 0;  // the amount, for its sign
    Aim aim = Aim::Other;
    bool area = false;  // an area target
};

struct SpellFacts
{
    std::array<Effect, EFFECTS> effects{};
    std::uint32_t mechanic = 0;
    std::int32_t durationMs = 0;   // -1 without an end
    std::uint32_t cooldownMs = 0;  // the larger of the spell's and its category's
    std::uint32_t targetAuraState = 0;
};

constexpr bool ControlMechanic(std::uint32_t mechanic)
{
    using namespace Data;
    switch (mechanic)
    {
        case MECHANIC_CHARM:
        case MECHANIC_DISORIENTED:
        case MECHANIC_FEAR:
        case MECHANIC_ROOT:
        case MECHANIC_SILENCE:
        case MECHANIC_SLEEP:
        case MECHANIC_STUN:
        case MECHANIC_FREEZE:
        case MECHANIC_KNOCKOUT:
        case MECHANIC_POLYMORPH:
        case MECHANIC_BANISH:
        case MECHANIC_SHACKLE:
        case MECHANIC_TURN:
        case MECHANIC_HORROR:
        case MECHANIC_SAPPED:
            return true;
        default:
            return false;
    }
}

constexpr bool ControlAura(std::uint32_t aura)
{
    using namespace Data;
    return aura == AURA_MOD_POSSESS || aura == AURA_MOD_CONFUSE || aura == AURA_MOD_CHARM || aura == AURA_MOD_FEAR ||
           aura == AURA_MOD_STUN || aura == AURA_MOD_ROOT || aura == AURA_MOD_SILENCE ||
           aura == AURA_MOD_PACIFY_SILENCE;
}

constexpr std::uint32_t DispelBit(std::int32_t type)
{
    switch (type)
    {
        case Data::DISPEL_TYPE_MAGIC:
            return DISPEL_MAGIC;
        case Data::DISPEL_TYPE_CURSE:
            return DISPEL_CURSE;
        case Data::DISPEL_TYPE_DISEASE:
            return DISPEL_DISEASE;
        case Data::DISPEL_TYPE_POISON:
            return DISPEL_POISON;
        default:
            return 0;
    }
}

// One effect's kinds; the spell's own facts decide control's length, a defensive's cooldown
constexpr std::uint32_t EffectKinds(Effect const& e, SpellFacts const& spell)
{
    using namespace Data;
    std::uint32_t kinds = 0;
    bool const self = e.aim == Aim::Self;
    bool const longCooldown = spell.cooldownMs >= COOLDOWN_MIN_MS;

    switch (e.effect)
    {
        case EFFECT_INTERRUPT_CAST:
            kinds |= INTERRUPT;
            break;
        case EFFECT_DISPEL:
            // on a friend its type; on an enemy a purge; a spell for either is both
            if (e.aim != Aim::Enemy)
                kinds |= DispelBit(e.misc);
            if (e.aim == Aim::Enemy || e.aim == Aim::Any)
                kinds |= PURGE;
            break;
        case EFFECT_STEAL_BENEFICIAL_BUFF:
            kinds |= STEAL;
            break;
        case EFFECT_ATTACK_ME:
            kinds |= TAUNT;
            break;
        case EFFECT_THREAT:
        case EFFECT_MODIFY_THREAT_PERCENT:
            if (e.value < 0)
                kinds |= THREAT_DROP;
            break;
        case EFFECT_HEAL:
        case EFFECT_HEAL_PCT:
        case EFFECT_HEAL_MAX_HEALTH:
            kinds |= self ? SELF_HEAL : HEAL;
            break;
        case EFFECT_CHARGE:
        case EFFECT_CHARGE_DEST:
        case EFFECT_JUMP:
        case EFFECT_JUMP_DEST:
        case EFFECT_PULL_TOWARDS:
        case EFFECT_PULL_TOWARDS_DEST:
            kinds |= CLOSE;
            break;
        case EFFECT_LEAP:
        case EFFECT_LEAP_BACK:
            kinds |= ESCAPE;
            break;
        case EFFECT_DISPEL_MECHANIC:
            if (self)
                kinds |= ESCAPE;
            break;
        case EFFECT_ENERGIZE:
        case EFFECT_ENERGIZE_PCT:
            if (self)
                kinds |= RECOVER;
            break;
        default:
            break;
    }

    if (e.aura)
    {
        bool const lasts = spell.durationMs < 0 || static_cast<std::uint32_t>(spell.durationMs) >= CONTROL_MIN_MS;
        bool const controls = ControlAura(e.aura) || ControlMechanic(e.mechanic) || ControlMechanic(spell.mechanic);
        if (controls && lasts && !self && e.aim != Aim::Ally)
            kinds |= CONTROL;

        switch (e.aura)
        {
            case AURA_MOD_TAUNT:
                kinds |= TAUNT;
                break;
            case AURA_MOD_TOTAL_THREAT:
            case AURA_MOD_THREAT:
                if (e.value < 0)
                    kinds |= THREAT_DROP;
                break;
            case AURA_FEIGN_DEATH:
                kinds |= THREAT_DROP;
                break;
            case AURA_PERIODIC_HEAL:
            case AURA_OBS_MOD_HEALTH:
                kinds |= self ? SELF_HEAL : HEAL;
                break;
            case AURA_SCHOOL_ABSORB:
            case AURA_SCHOOL_IMMUNITY:
            case AURA_DAMAGE_IMMUNITY:
                if (self && longCooldown)
                    kinds |= DEFENSIVE;
                break;
            case AURA_MOD_DAMAGE_PERCENT_TAKEN:
                if (self && longCooldown && e.value < 0)
                    kinds |= DEFENSIVE;
                break;
            case AURA_MOD_DODGE_PERCENT:
            case AURA_MOD_PARRY_PERCENT:
                if (self && longCooldown && e.value > 0)
                    kinds |= DEFENSIVE;
                break;
            case AURA_MOD_INCREASE_SPEED:
            case AURA_MOD_SPEED_ALWAYS:
            case AURA_MOD_SPEED_NOT_STACK:
                if (self && e.value > 0)
                    kinds |= ESCAPE;
                break;
            case AURA_MECHANIC_IMMUNITY:
                // the caster immune to a control or a slow (an immunity to anything else, a bandage say, frees nothing)
                if (self && e.misc >= 0 &&
                    (ControlMechanic(static_cast<std::uint32_t>(e.misc)) || e.misc == static_cast<int>(MECHANIC_SNARE)))
                    kinds |= ESCAPE;
                break;
            case AURA_PERIODIC_ENERGIZE:
            case AURA_OBS_MOD_POWER:
                if (self)
                    kinds |= RECOVER;
                break;
            default:
                break;
        }
    }

    // an area target, or a persistent area effect on the ground (its first target is often only the place)
    if (e.area || e.effect == EFFECT_PERSISTENT_AREA_AURA)
        kinds |= AREA;
    return kinds;
}

// combat_known's kinds of a spell: total, 0 for a spell that answers none
constexpr std::uint32_t Classify(SpellFacts const& spell)
{
    std::uint32_t kinds = 0;
    for (Effect const& e : spell.effects)
        if (e.effect)
            kinds |= EffectKinds(e, spell);
    if (spell.targetAuraState == Data::STATE_HEALTHLESS_20 || spell.targetAuraState == Data::STATE_HEALTHLESS_35)
        kinds |= FINISHER;
    if (spell.cooldownMs >= COOLDOWN_MIN_MS)
        kinds |= COOLDOWN;
    return kinds;
}

// The sample rows (MEASURES §1)

// combat_sample self's reason: CombatTally::Share in its order
constexpr std::array<std::string_view, 5> SHARE_WORDS = {"idle", "waiting", "reach", "controlled", "acting"};

// The power kind's word (Powers: mana, rage, focus, energy, happiness, rune, runic power); "power" for the others
constexpr std::string_view PowerWord(std::uint32_t power)
{
    switch (power)
    {
        case 0:
            return "mana";
        case 1:
            return "rage";
        case 2:
            return "focus";
        case 3:
            return "energy";
        case 6:
            return "runic";
        default:
            return "power";
    }
}

// Power as a player reads it: the core keeps rage and runic power in tenths
constexpr std::uint32_t Points(std::uint32_t power, std::int64_t value)
{
    if (value < 0)
        return 0;
    std::uint32_t const points = static_cast<std::uint32_t>(value);
    return power == 1 || power == 6 ? points / 10 : points;
}

// A death knight's ready runes (no cooldown left) as a 6-bit mask
constexpr std::uint32_t RuneMask(std::array<std::uint32_t, 6> const& cooldowns)
{
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < cooldowns.size(); ++i)
        if (!cooldowns[i])
            mask |= 1u << i;
    return mask;
}

// Whom an enemy attacks, from the sampled bot's view
enum class Victim : std::uint8_t
{
    None,
    Me,
    Pet,
    Mate,
    Other
};

constexpr std::string_view VictimWord(Victim victim)
{
    switch (victim)
    {
        case Victim::Me:
            return "me";
        case Victim::Pet:
            return "pet";
        case Victim::Mate:
            return "mate";
        case Victim::Other:
            return "other";
        default:
            return "none";
    }
}

// The enemy row's reason: the bot's target, an enemy attacking the bot or its pet, or both
constexpr std::string_view EnemyReason(bool target, bool attacker)
{
    return target ? (attacker ? "both" : "target") : "attacker";
}

constexpr std::size_t ENEMY_ROWS = 8;
constexpr std::uint32_t SAMPLE_MS = 1000;
constexpr std::uint32_t SAMPLE_GAP_MS = 3 * SAMPLE_MS;

// The self row's clock: due once a second has passed since the last row. It steps a second at a time, so a late tick
// leaves the rows at one a second (the next one comes sooner); after a gap of three seconds it starts again from now
// instead of catching up
struct SampleClock
{
    std::uint32_t lastMs = 0;
    bool started = false;

    void Start(std::uint32_t nowMs)
    {
        lastMs = nowMs;
        started = true;
    }

    bool Due(std::uint32_t nowMs)
    {
        std::uint32_t const elapsed = nowMs - lastMs;
        if (started && elapsed < SAMPLE_MS)
            return false;
        lastMs = !started || elapsed >= SAMPLE_GAP_MS ? nowMs : lastMs + SAMPLE_MS;
        started = true;
        return true;
    }
};

// taken since the last self row: the damage table's total runs on, and starts at 0 again after a fight's end
constexpr std::uint64_t Since(std::uint64_t total, std::uint64_t before)
{
    return total >= before ? total - before : total;
}
}  // namespace CombatKinds

#endif
