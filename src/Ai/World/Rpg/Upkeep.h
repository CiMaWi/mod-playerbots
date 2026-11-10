/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEP_H
#define _PLAYERBOT_UPKEEP_H

#include "Define.h"
#include "UpkeepRules.h"

class Player;

namespace Upkeep
{
// Whether the bot gets the free handout of this kind (UpkeepRules.h: it does not keep its possessions, the kind is not
// honest yet, or it is in the handout share)
bool Granted(Player* bot, UpkeepKind kind);
// The same with PlayerbotFactory::KeepsPossessions(bot) already known, for a site that hands out several kinds
bool Granted(Player* bot, UpkeepKind kind, bool keepsPossessions);
// The kind's honesty row (Honesty<word>) for a handout that ran
void Note(Player* bot, UpkeepKind kind, char const* source, uint32 id = 0, int64 amount = 0);

// Runs the handouts of one kind when the bot is granted it and writes the kind's row once
template <typename... Handouts>
bool HandOut(Player* bot, UpkeepKind kind, bool keepsPossessions, char const* source, Handouts&&... handouts)
{
    if (!Granted(bot, kind, keepsPossessions))
        return false;

    (handouts(), ...);
    Note(bot, kind, source);
    return true;
}
}  // namespace Upkeep

#endif
