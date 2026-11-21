/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatRecord.h"

#include <memory>

#include "PlayerbotAI.h"

namespace CombatRecord
{
State& StateOf(PlayerbotAI* botAI)
{
    if (!botAI->combatRecord)
        botAI->combatRecord = std::make_shared<State>();
    return *botAI->combatRecord;
}
}  // namespace CombatRecord
