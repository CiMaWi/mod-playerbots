/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERLIVE_H
#define _PLAYERBOT_GATHERLIVE_H

#include <cstdint>
#include <string>

class ChatHandler;
class Player;

namespace Materials
{
struct Rates;
}
namespace SkillPath
{
struct Path;
}

// The gather rules live (professions plan WR13a; the pure half is GatherRules.h)
namespace Planner
{
struct PathData;
struct Snapshot;
namespace Goals
{
struct Situation;
}

// The planner worker (professions WR13b): the gather circuit from where the bot stands over its zone's spawn points
// (GatherNodeMgr, immutable after load), at the path's prices and the place risk, as the situation's one gather
// option; nothing when no stop pays. competing: the other gatherers in the zone. Its stops are nodes and, for a bot
// whose path wants a dropped material or that is short of gold, creatures (WR20c); the situation also carries the
// creature circuit's refusal (too dangerous) for the map thread's skip row
void FillGatherCircuit(Snapshot const& snapshot, SkillPath::Path const* path, Materials::Rates const& rates,
                       PathData const& data, std::uint32_t competing, bool shortOfGold, Goals::Situation& situation);

// The bot's map thread (FillSnapshot, WR13d): a herbalist's or miner's view of its zone's spawn points, the snapshot's
// gatherUp and gatherDown: those within its sight (the map's visibility range, a loaded grid) up or empty, and the
// points its respawn memory holds. Once per snapshot, over the zone's spawns
void FillGatherSight(Player* bot, Snapshot& snapshot);

// World thread: `playerbots professions nodes <bot>`, the gather rules on the bot's zone now: its gather skills, the
// zone's spawn points nearest first (respawn, pool share, worth, net at the detour), the node it would pass by for,
// and the circuit the rules build from where it stands
bool HandleNodesCommand(ChatHandler* handler, std::string name);
}  // namespace Planner

#endif
