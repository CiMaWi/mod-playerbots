/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PARTYMEMBERTODISPEL_H
#define PLAYERBOTS_PARTYMEMBERTODISPEL_H

#include "NamedObjectContext.h"
#include "PartyMemberValue.h"

class PlayerbotAI;
class Unit;

class PartyMemberToDispel : public PartyMemberValue, public Qualified
{
public:
    PartyMemberToDispel(PlayerbotAI* botAI, std::string const name = "party member to dispel")
        : PartyMemberValue(botAI, name, 1000), Qualified()
    {
    }

    // At the natural cadence's interval while its faster check runs (the dispel trigger's target), else once a second
    Unit* Get() override;

protected:
    Unit* Calculate() override;
};

#endif
