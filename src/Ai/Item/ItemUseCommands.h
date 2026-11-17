/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSECOMMANDS_H
#define _PLAYERBOT_ITEMUSECOMMANDS_H

class ChatHandler;

// Console commands of the item use plan (playerbots item ...), on the world thread; only `use` and `ahead` act, by
// an order
namespace ItemUse
{
// playerbots item census [unclassified]: the index's counts by item class, shape and kind; with `unclassified`, every
// record the classifier leaves unclassified, with its reason
bool HandleCensusCommand(ChatHandler* handler, char const* args);
// playerbots item info <entry> [enchant]: the dump line of each use effect of the item (or of the enchantment) and
// its kind
bool HandleInfoCommand(ChatHandler* handler, char const* args);
// playerbots item dump [<file>]: writes the whole index as the dump file (the classifier's fixture)
bool HandleDumpCommand(ChatHandler* handler, char const* args);
// playerbots item use <bot> <entry>|slot <slot> [self | unit <name> | creature <entry> | item <slot>] [dead]: a test's
// request (caller test) the bot takes on its map thread; the reply is "ordered", the outcome its ItemUse row
bool HandleUseCommand(ChatHandler* handler, char const* args);
// playerbots item ahead <bot> dungeon|elite|group|bg|none|auto: a test's value of what is ahead of the bot for the
// buff picker (WI8a), set on its map thread by an order; auto: the live signals again. The reply is "ordered"
bool HandleAheadCommand(ChatHandler* handler, char const* args);
}  // namespace ItemUse

#endif
