/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACCSPELLS_H
#define _PLAYERBOT_ARENACCSPELLS_H

#include <algorithm>

#include "ArenaCc.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

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

#endif
