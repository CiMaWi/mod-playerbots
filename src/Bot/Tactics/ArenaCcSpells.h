/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACCSPELLS_H
#define _PLAYERBOT_ARENACCSPELLS_H

#include <algorithm>
#include <array>

#include "ArenaCc.h"
#include "GameObject.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

// The core's numbers of the CC option rows (ArenaCc.h), read from the spell store once: GetCcSpellTable builds the
// table on its first call (a function-local static, thread-safe, after the spell store has loaded: every caller is a
// bot update or a GM command). Only the spells' static data; never a unit's DR counters (DT.12).

// The DR group a cast counts under (as PlayerbotAI.cpp's GetPvpTestCastGroup). A trap is cast as a summon spell
// without a group of its own; its CC is the spell the trap's game object casts when it springs.
inline SpellInfo const* GetCcAuraSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo || GetDiminishingReturnsGroupForSpell(spellInfo, false) != DIMINISHING_NONE)
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

// A CC aura on the unit that damage breaks (Unit::HasBreakableByDamageCrowdControlAura's aura types and test) cast by
// the bot, its pet or a member of its group, else nullptr (G4b-b: OwnCc and cc.ownBreaks). The unit's own map thread.
inline Aura const* CcTeamBreakableAura(Unit const* unit, Player const* bot)
{
    static constexpr std::array<AuraType, 5> Types = {SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_STUN,
                                                      SPELL_AURA_MOD_ROOT, SPELL_AURA_TRANSFORM};
    Group const* const group = bot->GetGroup();
    for (AuraType const type : Types)
    {
        for (AuraEffect const* effect : unit->GetAuraEffectsByType(type))
        {
            if (!(effect->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE))
                continue;

            Unit const* const caster = effect->GetCaster();
            Player const* const owner = caster ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (owner && (owner == bot || (group && owner->GetGroup() == group)))
                return effect->GetBase();
        }
    }

    return nullptr;
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
inline uint32 BuildCcBotTable(uint8 playerClass, Knows const& knows, std::array<uint32, CC_OPTION_COUNT>& knownSpell,
                              CcSpellTable& table)
{
    uint32 knownMask = 0;
    uint32 const classMask = CcClassMask(playerClass);
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        knownSpell[index] = (classMask & CcOptionBit(index)) ? CcHighestKnownRank(CC_OPTIONS[index].SpellId, knows) : 0;
        table[index] = knownSpell[index] ? ResolveCcSpell(knownSpell[index]) : CcSpellData();
        if (knownSpell[index] && table[index].Valid)
            knownMask |= CcOptionBit(index);
    }

    return knownMask;
}

// The unit is controlled as a player sees it (stunned, incapacitated, asleep, cyclone, banished, feared,
// polymorphed): no CC of the team goes on top of it (G4c chains), and the moon counts as held (G1b-c)
inline bool CcUnitControlled(Unit const* unit) { return unit->HasUnitState(UNIT_STATE_CONTROLLED); }

#endif
