/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERLIVE_H
#define _PLAYERBOT_GATHERLIVE_H

#include <string>

class ChatHandler;

// The gather rules live (professions plan WR13a; the pure half is GatherRules.h)
namespace Planner
{
// World thread: `playerbots professions nodes <bot>`, the gather rules on the bot's zone now: its gather skills, the
// zone's spawn points nearest first (respawn, pool share, worth, net at the detour), the node it would pass by for,
// and the circuit the rules build from where it stands
bool HandleNodesCommand(ChatHandler* handler, std::string name);
}  // namespace Planner

#endif
