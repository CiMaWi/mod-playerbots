/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGSTATUSEND_H
#define PLAYERBOTS_NEWRPGSTATUSEND_H

#include <optional>
#include <string>
#include <vector>

#include "NewRpgInfo.h"
#include "StatusEndRules.h"

class PlayerbotAI;

// The status core (consolidation CS14, R6): an RPG status ends and starts through these calls only, on the bot's map
// thread. An end runs the status's own clean-up where StatusEnd::CleansUp says so (errands and trips have one), tells
// the planner, replaces the status and writes the one Status row with the end's word. O(1), no lock, no database
// access, no allocation of its own.
namespace RpgStatus
{
// Ends the bot's status as `outcome`: its clean-up, idle, one Status row (note.word, else the outcome's word)
void End(PlayerbotAI* botAI, StatusEnd::Outcome outcome, StatusEnd::Note note = {});
// Starts `next`; a running status ends as `why` first (its clean-up, the planner's note); one Status row with `word`
void Start(PlayerbotAI* botAI, NewRpgInfo::RpgData next, char const* word,
           StatusEnd::Outcome why = StatusEnd::Outcome::Interrupted, StatusEnd::Note note = {});
// PlayerbotAI::Reset: the RPG state starts over (a far teleport keeps a trip, StatusEnd::ResetOf); its route request
// ids and a test's constraints stay. One Status row, teleport or reset
void Reset(PlayerbotAI* botAI, bool farTeleport);

// A status's trip to dest (GoTo, WT64b): begun in the travel block with the bot's status as its holder and a world log
// id; the route follower steps it. False: a trip is held already
bool BeginTrip(PlayerbotAI* botAI, WorldPosition const& dest);
// The held trip ended by itself (the follower's arrival or failure). Travel's status ends with it (End); any other
// holder's status goes on: the trip's end rows are written and its end waits for the holder (TakeTripEnd)
void TripEnded(PlayerbotAI* botAI, StatusEnd::Outcome outcome, StatusEnd::Note note = {});
// A trip's goal word (its start row's reason): inn, order for travel's own status, else the status that holds it
char const* TripGoal(PlayerbotAI* botAI, bool inn);
// The unread end of the trip the bot's status held, once: its outcome and bare cause word (arrived, no_route, ...)
std::optional<StatusEnd::TripEnd> TakeTripEnd(PlayerbotAI* botAI);

// The data of the two statuses with a rule of their own; the core begins a travel status's held trip to dest
// (BotTravel::Trip) with its start and world log id
NewRpgInfo::Travel TripTo(WorldPosition dest, std::string place = {});
NewRpgInfo::Errands ErrandTrip(std::vector<NewRpgInfo::ErrandStop> stops, uint32 money);
}  // namespace RpgStatus

#endif
