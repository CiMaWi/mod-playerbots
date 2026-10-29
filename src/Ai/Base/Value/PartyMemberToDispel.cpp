/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyMemberToDispel.h"

#include "NaturalReaction.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"

class PartyMemberToDispelPredicate : public FindPlayerPredicate, public PlayerbotAIAware
{
public:
    PartyMemberToDispelPredicate(PlayerbotAI* botAI, uint32 dispelType)
        : FindPlayerPredicate(), PlayerbotAIAware(botAI), dispelType(dispelType)
    {
    }

    bool Check(Unit* unit) override { return unit->IsAlive() && botAI->HasAuraToDispel(unit, dispelType); }

private:
    uint32 dispelType;
};

Unit* PartyMemberToDispel::Get()
{
    // A cached value older than the cadence's interval is computed again; without the cadence nothing changes
    if (lastCheckTime && botAI->IsNaturalCadenceActive() &&
        getMSTime() - lastCheckTime >=
            ReactionCheckIntervalMs(checkInterval, true, sPlayerbotAIConfig.NaturalCadenceCheckMs))
        lastCheckTime = 0;

    return PartyMemberValue::Get();
}

Unit* PartyMemberToDispel::Calculate()
{
    uint32 dispelType = atoi(qualifier.c_str());

    PartyMemberToDispelPredicate predicate(botAI, dispelType);
    return FindPartyMember(predicate);
}
