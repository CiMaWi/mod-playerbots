/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BRIDGEGOALS_H
#define _PLAYERBOT_BRIDGEGOALS_H

#include <cstdint>
#include <string>

class Player;
class PlayerbotAI;

// The bridge's goal orders (travel plan WT18a): what BridgeIntentEffect asks the planner (Planner::BridgeGoal) and
// what it answers. Plain values, so the effect's offline selftest includes this header without the planner's own
// headers
namespace Planner::GoalApply
{
enum class BridgeOrder : std::uint8_t
{
    GoTo,        // a trip to a known place's name or an x,y,z,map position
    Quest,       // do or turn in a quest of the bot's log
    Errands,     // an errand trip near the bot: sell, repair, turn in
    SetHearth,   // a trip to the nearest fitting inn on the bot's map and a bind there
    GrindHere,   // grind around the bot's position
    GatherHere,  // gather around the bot's position
    MeetAt,      // a trip to a place to meet the partner there
    Pause,       // the bot's own plan stops: it stands idle until resumed, at most PAUSE_MS
    Resume       // the bot's own plan goes on: a pause and a bridge goal end
};

enum class BridgeAnswer : std::uint8_t
{
    Done,          // the goal is the bot's (source bridge) and its executor runs; a pause or a resume took effect
    NotPlanner,    // the planner does not run the bot: Planner.Goals off, a real master, or not persistent
    GoalHeld,      // a group's goal holds the slot
    UnknownPlace,  // neither a known place's name nor x,y,z,map
    NoRoute,       // no route from the bot to the place, or the travel graph is not ready
    NoSuchQuest,   // the quest is not in the bot's log, or it failed
    NothingHere,   // nothing to grind here (a town or an inn), no gathering skill, no errand stop or inn in reach
    AlreadyThere,  // the bot is at the place already
    CannotNow,     // inside an instance: no trip, grind or gather from there
    Failed         // every check passed, but the executor did not start
};
}  // namespace Planner::GoalApply

namespace Planner
{
// PlannerService.cpp. The world thread after the map join, while the bot's map thread is idle: one order through the
// plausibility gate (GoalApply::BridgeCheck); a refusal changes nothing. place: a trip's place name or x,y,z,map;
// quest: a quest order's quest
GoalApply::BridgeAnswer BridgeGoal(PlayerbotAI* botAI, Player* bot, GoalApply::BridgeOrder order,
                                   std::string const& place, std::uint32_t quest);
}  // namespace Planner

#endif
