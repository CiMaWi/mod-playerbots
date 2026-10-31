/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ENEMYHEALERTARGETVALUE_H
#define PLAYERBOTS_ENEMYHEALERTARGETVALUE_H

#include <string>

#include "NamedObjectContext.h"
#include "ObjectGuid.h"
#include "Value.h"

class PlayerbotAI;
class Unit;

class EnemyHealerTargetValue : public UnitCalculatedValue, public Qualified
{
public:
    EnemyHealerTargetValue(PlayerbotAI* botAI) : UnitCalculatedValue(botAI, "enemy healer target") {}

protected:
    Unit* Calculate() override;

private:
    // G5b: the best interrupt target of the fight's enemies (ArenaInterrupt::Score), kept Interrupt.HealerCacheMs
    Unit* BestInterruptTarget(std::string const& spell);

    ObjectGuid bestGuid;
    uint32 bestUntilMs = 0;
};

#endif
