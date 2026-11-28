/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTWATCH_H
#define _PLAYERBOT_BOTWATCH_H

#include "Define.h"

class ChatHandler;

namespace WorldLogFormat
{
struct DecisionRow;
}

// The watch (questing WQ53, decision WQ.54): every real player in the world watches one named bot. Once a second the
// player's camera is put on the bot and its body kept near it, the player's level, money, items and quest log are made
// the bot's, and the bot's decision rows are sent to the player as system lines. A test tool for test characters on
// test servers: it changes the watching character freely and restores nothing.
namespace BotWatch
{
// World thread, once per world tick (no map update runs); returns at once while no bot is watched
void Update(uint32 diff);
// Any thread: a decision row as it is written; kept for the watchers when it is the watched bot's
void NoteDecision(WorldLogFormat::DecisionRow const& row);
// "playerbots watch <bot>|off", without a word the state
bool HandleCommand(ChatHandler* handler, char const* args);
}  // namespace BotWatch

#endif
