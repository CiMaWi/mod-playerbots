/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMFIGHTCOMMANDS_H
#define _PLAYERBOT_ITEMFIGHTCOMMANDS_H

class ChatHandler;

// Console commands of the item use plan's lane 2 (playerbots item ...), on the world thread
namespace ItemUse
{
// playerbots item usable <bot>: the bot prints its usable list, its situation and the offer at its next AI update
// (its map thread) to the server log, lines `ITEM usable:`; the reply is "ordered"
bool HandleUsableCommand(ChatHandler* handler, char const* args);
// playerbots item spend <bot> (WI16): the same print, whose `ITEM spend:` lines hold each weighed consumed candidate's
// spend case (danger, death cost, price, traits, control) and verdict, and the offer's pick; the reply is "ordered"
bool HandleSpendCommand(ChatHandler* handler, char const* args);
}  // namespace ItemUse

#endif
