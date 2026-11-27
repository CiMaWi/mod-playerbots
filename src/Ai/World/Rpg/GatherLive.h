/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERLIVE_H
#define _PLAYERBOT_GATHERLIVE_H

#include <cstdint>
#include <string>
#include <vector>

#include "GatherRateRules.h"

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

// The tool category a gather skill's node cast asks for (WPC8): the base-rank Mining spell's (a pick); 0 for a skill
// whose node cast asks for none
std::uint32_t GatherToolCategory(std::uint16_t skill);

// Whether one of the held tools (item ids, Snapshot::toolsHeld) covers the skill's tool category; true when the skill
// asks for none
bool HoldsGatherTool(std::vector<std::uint32_t> const& toolsHeld, std::uint16_t skill);

// The bot's map thread (FillSnapshot, WR13d): a herbalist's or miner's view of its zone's spawn points, the snapshot's
// gatherUp and gatherDown: those within its sight (the map's visibility range, a loaded grid) up or empty, and the
// points its respawn memory holds. Once per snapshot, over the zone's spawns
void FillGatherSight(Player* bot, Snapshot& snapshot);

// World thread: `playerbots professions nodes <bot>`, the gather rules on the bot's zone now: its gather skills, the
// zone's spawn points nearest first (respawn, pool share, worth, net at the detour), the node it would pass by for,
// and the circuit the rules build from where it stands
bool HandleNodesCommand(ChatHandler* handler, std::string name);

// The observed minutes per take (professions WPC13b, DECISIONS WPC.13): the map thread whose planned circuit ended
// (a status change, or a reset that drops it) adds its span, started at startMs (0: no planned circuit), to the tally
void NoteGatherSpan(GatherRate::Span span, std::uint32_t startMs);

// World thread (WorldHook GatherRates): a timer add; every RATE_PUBLISH_MS the tally fades by the time passed and is
// published for the planner
void UpdateGatherRates(std::uint32_t diff);

// Any thread: the newest published tally; empty before the first publication, the static rule
GatherRate::View GatherRatesNow();

// World thread: `playerbots professions rates [zone]`, the tally per zone and way; `rates set <zone> <way> <minutes>
// <takes>` (game master or console, a test's fixture) sets one tally and publishes at once
bool HandleRatesCommand(ChatHandler* handler, std::vector<std::string> const& args);
}  // namespace Planner

#endif
