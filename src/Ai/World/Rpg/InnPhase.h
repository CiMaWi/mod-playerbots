/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_INNPHASE_H
#define _PLAYERBOT_INNPHASE_H

#include "Define.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldTravelRules.h"

namespace WorldTravel
{
// World thread: a spell_area phase row of the area or zone hides a spawn of that phase mask from a bot without quest
// progress (PhaseRowHidesSpawn): TravelMgr's inn list and the planner index's innkeepers (planning WT63d)
inline bool InnPhasedOut(uint32 areaId, uint32 zoneId, uint32 phaseMask)
{
    for (uint32 const phaseArea : {areaId, zoneId})
    {
        SpellAreaForAreaMapBounds const rows = sSpellMgr->GetSpellAreaForAreaMapBounds(phaseArea);
        for (auto row = rows.first; row != rows.second; ++row)
        {
            SpellArea const* spellArea = row->second;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellArea->spellId);
            if (!spell)
                continue;
            for (SpellEffectInfo const& effect : spell->Effects)
                if (effect.ApplyAuraName == SPELL_AURA_PHASE &&
                    PhaseRowHidesSpawn(spellArea->autocast, spellArea->questStart, spellArea->auraSpell,
                                       static_cast<uint32>(effect.MiscValue), phaseMask))
                    return true;
        }
    }
    return false;
}
}  // namespace WorldTravel

#endif
