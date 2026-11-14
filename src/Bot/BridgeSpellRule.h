/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGESPELLRULE_H
#define PLAYERBOTS_BRIDGESPELLRULE_H

#include <array>
#include <cstdint>

// The spell rule of the cast row (LC17b, LC.32): a partner may ask a bot for a spell of its own that helps (a buff, a
// heal, a cure, a resurrection, conjured food or water, a portal), never one that harms, crafts or moves the caster
// (a teleport, the hearthstone, a blink or a charge: the planner owns travel). A spell's facts come from its
// SpellInfo (BridgeAbilities.cpp FactsOf); plain data and no game type, so a selftest can feed a list of spells.
namespace PlayerbotBridge
{
// The SharedDefines.h SpellEffects values the rule reads (BridgeAbilities.cpp asserts that they match).
namespace SpellEffectId
{
constexpr uint32_t Instakill = 1;
constexpr uint32_t SchoolDamage = 2;
constexpr uint32_t PortalTeleport = 4;
constexpr uint32_t TeleportUnits = 5;
constexpr uint32_t EnvironmentalDamage = 7;
constexpr uint32_t PowerDrain = 8;
constexpr uint32_t HealthLeech = 9;
constexpr uint32_t WeaponDamageNoSchool = 17;
constexpr uint32_t Leap = 29;
constexpr uint32_t WeaponPercentDamage = 31;
constexpr uint32_t Jump = 41;
constexpr uint32_t JumpDest = 42;
constexpr uint32_t TeleportUnitsFaceCaster = 43;
constexpr uint32_t TradeSkill = 47;
constexpr uint32_t WeaponDamage = 58;
constexpr uint32_t PowerBurn = 62;
constexpr uint32_t InterruptCast = 68;
constexpr uint32_t Stuck = 84;
constexpr uint32_t Charge = 96;
constexpr uint32_t KnockBack = 98;
constexpr uint32_t AttackMe = 114;
constexpr uint32_t TeleportGraveyard = 120;
constexpr uint32_t NormalizedWeaponDamage = 121;
constexpr uint32_t PullTowards = 124;
constexpr uint32_t LeapBack = 138;
constexpr uint32_t KnockBackDest = 144;
constexpr uint32_t PullTowardsDest = 145;
constexpr uint32_t ChargeDest = 149;
}  // namespace SpellEffectId

// SPELL_ATTR0_IS_TRADESKILL: a recipe of a profession.
constexpr uint32_t TradeskillAttribute = 0x00000020;

struct SpellFacts
{
    bool Passive;                     // SpellInfo::IsPassive
    bool Positive;                    // SpellInfo::IsPositive: every effect helps whom it lands on
    bool Profession;                  // A profession's skill line teaches it (SkillLineAbility)
    uint32_t Attributes;              // SpellInfo::Attributes
    std::array<uint32_t, 3> Effects;  // SpellInfo::Effects[i].Effect
};

// Why a spell is or is not one the bot casts on an order, in the order the rule asks.
enum class SpellVerdict : uint8_t
{
    Helpful,
    Passive,
    Craft,
    MovesCaster,
    Harmful
};

// An effect that moves a unit: a teleport (the hearthstone, a mage's teleport, unstuck), a leap or jump, a charge, a
// knock-back or a pull. A portal for the group is a game object (TRANS_DOOR), not one of these.
constexpr bool MovesUnit(uint32_t effect)
{
    using namespace SpellEffectId;
    switch (effect)
    {
        case PortalTeleport:
        case TeleportUnits:
        case TeleportUnitsFaceCaster:
        case TeleportGraveyard:
        case Stuck:
        case Leap:
        case LeapBack:
        case Jump:
        case JumpDest:
        case Charge:
        case ChargeDest:
        case KnockBack:
        case KnockBackDest:
        case PullTowards:
        case PullTowardsDest:
            return true;
        default:
            return false;
    }
}

// An effect that harms: damage, a drain or leech, a kill, an interrupt or a taunt.
constexpr bool Harms(uint32_t effect)
{
    using namespace SpellEffectId;
    switch (effect)
    {
        case Instakill:
        case SchoolDamage:
        case EnvironmentalDamage:
        case PowerDrain:
        case HealthLeech:
        case WeaponDamageNoSchool:
        case WeaponPercentDamage:
        case WeaponDamage:
        case NormalizedWeaponDamage:
        case PowerBurn:
        case InterruptCast:
        case AttackMe:
            return true;
        default:
            return false;
    }
}

constexpr SpellVerdict JudgeSpell(SpellFacts const& spell)
{
    if (spell.Passive)
        return SpellVerdict::Passive;

    bool craft = spell.Profession || (spell.Attributes & TradeskillAttribute) != 0;
    bool moves = false;
    bool harms = !spell.Positive;
    for (uint32_t effect : spell.Effects)
    {
        craft = craft || effect == SpellEffectId::TradeSkill;
        moves = moves || MovesUnit(effect);
        harms = harms || Harms(effect);
    }

    if (craft)
        return SpellVerdict::Craft;

    if (moves)
        return SpellVerdict::MovesCaster;

    return harms ? SpellVerdict::Harmful : SpellVerdict::Helpful;
}

constexpr bool HelpfulSpell(SpellFacts const& spell) { return JudgeSpell(spell) == SpellVerdict::Helpful; }
}  // namespace PlayerbotBridge

#endif
