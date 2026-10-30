/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACCSPELLS_H
#define _PLAYERBOT_ARENACCSPELLS_H

#include <algorithm>
#include <array>
#include <utility>

#include "ArenaCc.h"
#include "GameObject.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PvpTestCounters.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

// The core's numbers of the CC option rows (ArenaCc.h), read from the spell store once: GetCcSpellTable builds the
// table on its first call (a function-local static, thread-safe, after the spell store has loaded: every caller is a
// bot update or a GM command). Only the spells' static data; never a unit's DR counters (DT.12).

// G4d: a CC whose aura a world database script casts (spell_scripts), not an effect of the spell: the cast, its aura
constexpr std::array<std::pair<uint32, uint32>, 1> CC_SCRIPTED_AURAS = {{
    {49203, 51209},  // hungering cold: a dummy at the death knight, the script casts the freeze (10 s, 10 yd)
}};

// The DR group a cast counts under (as PlayerbotAI.cpp's GetPvpTestCastGroup). A trap is cast as a summon spell
// without a group of its own; its CC is the spell the trap's game object casts when it springs.
inline SpellInfo const* GetCcAuraSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return spellInfo;

    for (auto const& [castId, auraId] : CC_SCRIPTED_AURAS)
    {
        if (spellInfo->Id != castId)
            continue;

        if (SpellInfo const* const aura = sSpellMgr->GetSpellInfo(auraId))
            return aura;
    }

    if (GetDiminishingReturnsGroupForSpell(spellInfo, false) != DIMINISHING_NONE)
        return spellInfo;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        if (effect.Effect != SPELL_EFFECT_SUMMON_OBJECT_WILD && effect.Effect != SPELL_EFFECT_SUMMON_OBJECT_SLOT1)
            continue;

        GameObjectTemplate const* trap = sObjectMgr->GetGameObjectTemplate(effect.MiscValue);
        if (!trap || trap->type != GAMEOBJECT_TYPE_TRAP)
            continue;

        if (SpellInfo const* trapSpell = sSpellMgr->GetSpellInfo(trap->trap.spellId))
            return trapSpell;
    }

    return spellInfo;
}

inline CcSpellData ResolveCcSpell(uint32 spellId)
{
    CcSpellData data;
    SpellInfo const* cast = sSpellMgr->GetSpellInfo(spellId);
    if (!cast)
        return data;

    SpellInfo const* aura = GetCcAuraSpell(cast);
    DiminishingGroup const group = GetDiminishingReturnsGroupForSpell(aura, false);
    data.AuraSpellId = aura->Id;
    data.Group = static_cast<uint8>(group);
    data.DrType = static_cast<uint8>(GetDiminishingReturnsGroupType(group));
    data.BaseMs = static_cast<uint32>(std::max<int32>(aura->GetMaxDuration(), 0));
    data.PvpCapMs = static_cast<uint32>(std::max<int32>(GetDiminishingReturnsLimitDuration(group, aura), 0));
    data.CastMs = cast->CastTimeEntry ? static_cast<uint32>(std::max<int32>(cast->CastTimeEntry->CastTime, 0)) : 0;
    data.RangeYards = cast->GetMaxRange(false);
    data.DamageInterrupts = (aura->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE) != 0;
    data.Valid = true;
    return data;
}

// The caster is the bot, its pet or (a pet of) a member of its group
inline bool CcTeamCaster(Unit const* caster, Player const* bot)
{
    Player const* const owner = caster ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    Group const* const group = bot->GetGroup();
    return owner && (owner == bot || (group && owner->GetGroup() == group));
}

constexpr uint8 CC_PENDING_AURAS = 4;  // team breakable auras one damage event remembers (G4b-b review NOTE 6)

// The CC auras on the unit that damage breaks (Unit::HasBreakableByDamageCrowdControlAura's aura types and test) cast
// by the bot, its pet or a member of its group: up to most of them into auras, in type order; the count (G4b-b: OwnCc
// and cc.ownBreaks). The unit's own map thread.
inline uint8 CcTeamBreakableAuras(Unit const* unit, Player const* bot, std::array<Aura const*, CC_PENDING_AURAS>& auras,
                                  uint8 most = CC_PENDING_AURAS)
{
    static constexpr std::array<AuraType, 5> Types = {SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_STUN,
                                                      SPELL_AURA_MOD_ROOT, SPELL_AURA_TRANSFORM};
    uint8 const limit = std::min(most, CC_PENDING_AURAS);
    uint8 count = 0;
    for (AuraType const type : Types)
    {
        for (AuraEffect const* effect : unit->GetAuraEffectsByType(type))
        {
            if (!(effect->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE) ||
                !CcTeamCaster(effect->GetCaster(), bot))
                continue;

            Aura const* const aura = effect->GetBase();
            if (std::find(auras.begin(), auras.begin() + count, aura) != auras.begin() + count)
                continue;

            auras[count++] = aura;
            if (count >= limit)
                return count;
        }
    }

    return count;
}

// The first of them, else nullptr
inline Aura const* CcTeamBreakableAura(Unit const* unit, Player const* bot)
{
    std::array<Aura const*, CC_PENDING_AURAS> auras{};
    return CcTeamBreakableAuras(unit, bot, auras, 1) ? auras[0] : nullptr;
}

// G4c-b DotGuard: the team's periodic damage (a DoT, a ground effect's ticks) is on the unit, as a player sees its
// debuffs; once per cc ready refresh, only on the target
inline bool CcTeamDotOn(Unit const* unit, Player const* bot)
{
    static constexpr std::array<AuraType, 3> Types = {SPELL_AURA_PERIODIC_DAMAGE, SPELL_AURA_PERIODIC_DAMAGE_PERCENT,
                                                      SPELL_AURA_PERIODIC_LEECH};
    for (AuraType const type : Types)
    {
        for (AuraEffect const* effect : unit->GetAuraEffectsByType(type))
        {
            if (CcTeamCaster(effect->GetCaster(), bot))
                return true;
        }
    }

    return false;
}

// G4c-b opponent memory: a mechanic immunity the unit cast on itself just now (the PvP trinket, every man for himself,
// will of the forsaken: the core removes the CC while it applies, AURA_REMOVE_BY_DEFAULT; the effect is registered
// before its handler runs). Its full duration tells a fresh one from an older self immunity (berserker rage).
inline bool CcSelfTrinketOn(Unit const* unit)
{
    for (AuraEffect const* effect : unit->GetAuraEffectsByType(SPELL_AURA_MECHANIC_IMMUNITY))
    {
        Aura const* const aura = effect->GetBase();
        if (effect->GetCasterGUID() == unit->GetGUID() && aura->GetDuration() == aura->GetMaxDuration())
            return true;
    }

    return false;
}

namespace CcAreaDetail
{
// Damage on the spell's targets: a direct effect, or an aura that ticks damage
inline bool EffectDamages(SpellEffectInfo const& effect)
{
    switch (effect.Effect)
    {
        case SPELL_EFFECT_SCHOOL_DAMAGE:
        case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
        case SPELL_EFFECT_HEALTH_LEECH:
        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
        case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
            return true;
        default:
            break;
    }

    return effect.IsAura() && (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                               effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE_PERCENT ||
                               effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH);
}

// The effect's area on enemies: a cone, an area around the source (the caster) or the destination (the target, or the
// caster's feet for TARGET_DEST_CASTER), a persistent area at the destination; else none
inline CcAreaCentre EffectCentre(SpellEffectInfo const& effect)
{
    bool const atCaster = effect.TargetA.GetTarget() == TARGET_DEST_CASTER;
    if (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA)
        return atCaster ? CcAreaCentre::Caster : CcAreaCentre::Target;

    for (SpellImplicitTargetInfo const& target : {effect.TargetA, effect.TargetB})
    {
        if (target.GetCheckType() != TARGET_CHECK_ENEMY)
            continue;

        if (target.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE)
            return CcAreaCentre::Cone;

        if (target.GetSelectionCategory() != TARGET_SELECT_CATEGORY_AREA)
            continue;

        SpellTargetReferenceTypes const reference = target.GetReferenceType();
        if (reference == TARGET_REFERENCE_TYPE_SRC || reference == TARGET_REFERENCE_TYPE_CASTER || atCaster)
            return CcAreaCentre::Caster;

        return CcAreaCentre::Target;
    }

    return CcAreaCentre::None;
}

// Spell::SearchChainTargets' jump radius
inline float ChainJumpYards(SpellInfo const* info)
{
    constexpr float RangedJumpYards = 7.5f;
    constexpr float JumpYards = 10.0f;
    if (info->JumpDistance > 0.0f)
        return info->JumpDistance;

    return info->DmgClass == SPELL_DAMAGE_CLASS_RANGED ? RangedJumpYards : JumpYards;
}
}  // namespace CcAreaDetail

// G4c-b AoE guard: the area of a damage spell (an area, cone or chain effect that damages; a channel or ground effect
// through the spell it triggers, one level): the first such effect's centre with its largest radius. Static data: the
// caller caches it per action; the radius is the spell's base (talents add a little: the guard's margin).
inline CcSpellArea CcResolveSpellArea(SpellInfo const* info, bool followTrigger = true)
{
    CcSpellArea area;
    if (!info)
        return area;

    for (SpellEffectInfo const& effect : info->Effects)
    {
        CcSpellArea found;
        if (CcAreaDetail::EffectDamages(effect))
        {
            found.Centre = CcAreaDetail::EffectCentre(effect);
            found.RadiusYards = effect.CalcRadius();
            if (found.Centre == CcAreaCentre::None && effect.ChainTarget > 1 &&
                effect.TargetA.GetCheckType() == TARGET_CHECK_ENEMY)
            {
                found.Centre = CcAreaCentre::Chain;
                found.RadiusYards = CcAreaDetail::ChainJumpYards(info);
            }
        }
        else if (followTrigger && effect.TriggerSpell &&
                 (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL || effect.Effect == SPELL_EFFECT_TRIGGER_MISSILE ||
                  (effect.IsAura() && effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL)))
            found = CcResolveSpellArea(sSpellMgr->GetSpellInfo(effect.TriggerSpell), false);

        if (found.Centre == CcAreaCentre::None)
            continue;

        if (area.Centre == CcAreaCentre::None)
            area = found;
        else if (found.Centre == area.Centre)
            area.RadiusYards = std::max(area.RadiusYards, found.RadiusYards);
    }

    return area;
}

// The team's loss-of-control CC on the unit as a player sees it (G4c chains: the visible auras, by spell): the one
// that ends last, its remaining ms, how long it is up (G4c-b: the opponent memory's dispel timing) and DR group; false
// when none of the team's (an enemy's or a neutral stun, or no CC). A walk over the unit's auras: cc ready calls it
// once per refresh, only on a controlled CC target.
inline bool CcTeamControlAura(Unit const* unit, Player const* bot, uint32& remainingMs, uint32& elapsedMs, uint8& group)
{
    bool found = false;
    remainingMs = 0;
    elapsedMs = 0;
    group = 0;
    for (auto const& [spellId, application] : unit->GetAppliedAuras())
    {
        Aura const* const aura = application->GetBase();
        uint8 const auraGroup = static_cast<uint8>(GetDiminishingReturnsGroupForSpell(aura->GetSpellInfo(), false));
        int32 const leftMs = aura->GetDuration();
        if (!CcGroupLosesControl(auraGroup) || leftMs <= 0 || (found && static_cast<uint32>(leftMs) <= remainingMs) ||
            !CcTeamCaster(aura->GetCaster(), bot))
            continue;

        found = true;
        remainingMs = static_cast<uint32>(leftMs);
        elapsedMs = static_cast<uint32>(std::max<int32>(aura->GetMaxDuration() - leftMs, 0));
        group = auraGroup;
    }

    return found;
}

// A teammate's cast bar shows a CC (a CC DR group) aimed at the target: pending, so nobody casts over it (DT.1, as
// FocusStrategy's FocusTeamCastingCcAt for the moon). The bot itself is not asked.
inline bool CcTeamCastingCcAt(Player const* bot, ObjectGuid target)
{
    Group const* const group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player const* const member = ref->GetSource();
        if (!member || member == bot || !member->IsInMap(bot))
            continue;

        Spell const* const spell = member->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell || !spell->GetSpellInfo() || spell->m_targets.GetUnitTargetGUID() != target)
            continue;

        SpellInfo const* const aura = GetCcAuraSpell(spell->GetSpellInfo());
        if (IsPvpTestCcGroup(static_cast<uint32>(GetDiminishingReturnsGroupForSpell(aura, false))))
            return true;
    }

    return false;
}

inline CcSpellTable const& GetCcSpellTable()
{
    static CcSpellTable const table = []
    {
        CcSpellTable built{};
        for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
            built[index] = ResolveCcSpell(CC_OPTIONS[index].SpellId);

        return built;
    }();
    return table;
}

// The highest rank of a row's spell chain that knows(id) says the bot has, 0 for none (G4a NOTE 1: the table above
// is rank 1; hammer of justice lasts 3 s there and 6 s at its last rank). Walks down from the chain's last rank.
template <typename Knows>
inline uint32 CcHighestKnownRank(uint32 firstRankId, Knows const& knows)
{
    for (uint32 id = sSpellMgr->GetLastSpellInChain(firstRankId); id; id = sSpellMgr->GetPrevSpellInChain(id))
    {
        if (knows(id))
            return id;
    }

    return 0;
}

// One bot's table (CcState.h): per row of its class the highest rank it knows and that rank's numbers. Only on a bot
// update (the spell store is loaded); the caller caches it and rebuilds when the bot's level or talents change.
template <typename Knows>
inline CcRowMask BuildCcBotTable(uint8 playerClass, Knows const& knows, std::array<uint32, CC_OPTION_COUNT>& knownSpell,
                                 CcSpellTable& table)
{
    CcRowMask knownMask = 0;
    CcRowMask const classMask = CcClassMask(playerClass);
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        knownSpell[index] = (classMask & CcOptionBit(index)) ? CcHighestKnownRank(CC_OPTIONS[index].SpellId, knows) : 0;
        table[index] = knownSpell[index] ? ResolveCcSpell(knownSpell[index]) : CcSpellData();
        if (knownSpell[index] && table[index].Valid)
            knownMask |= CcOptionBit(index);
    }

    return knownMask;
}

// tactics force dr (G4c): the row whose rank-1 aura counts under the DR group and that a triggered cast can land on
// the target (usable on it, no stealth, no combo points), CC_NO_ROW for none
inline int8 CcForceDrRow(uint8 group, bool targetIsPlayer, uint8 creatureType)
{
    CcSpellTable const& table = GetCcSpellTable();
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        CcOption const& option = CC_OPTIONS[index];
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(option.SpellId);
        if (!table[index].Valid || table[index].Group != group || option.NeedsStealth || !info ||
            info->NeedsComboPoints() || !CcOptionUsableOn(option, targetIsPlayer, creatureType))
            continue;

        return static_cast<int8>(index);
    }

    return CC_NO_ROW;
}

// The unit is controlled as a player sees it (stunned, incapacitated, asleep, cyclone, banished, feared,
// polymorphed): no CC of the team goes on top of it unless the team's own CC and the bot chains (G4c), and the moon
// counts as held (G1b-c)
inline bool CcUnitControlled(Unit const* unit) { return unit->HasUnitState(UNIT_STATE_CONTROLLED); }

#endif
