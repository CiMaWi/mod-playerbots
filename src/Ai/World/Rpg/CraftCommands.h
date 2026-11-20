/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CRAFTCOMMANDS_H
#define _PLAYERBOT_CRAFTCOMMANDS_H

class ChatHandler;

// GM commands of the professions plan's craft visit (playerbots craft ...); they run on the world thread
namespace BankMail
{
// playerbots craft visit [<bot name> <spell> [<count>]]: sends a persistent bot within the range of the nearest
// station its recipe needs (none: where it stands) and casts the recipe up to count times (CraftVisit.h); without a
// name, the visit counters
bool HandleCraftVisitCommand(ChatHandler* handler, char const* args);
// playerbots craft process <bot name> disenchant|prospect|mill [<item entry>]: a craft visit where the bot stands
// that casts the processing spell on the item (default: the first bag item the spell accepts)
bool HandleCraftProcessCommand(ChatHandler* handler, char const* args);
}  // namespace BankMail

#endif
