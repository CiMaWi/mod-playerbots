/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_VISIT_H
#define _PLAYERBOT_VISIT_H

#include <string>

#include "Define.h"
#include "ObjectGuid.h"
#include "VisitLifecycle.h"
#include "VisitState.h"

class Player;
class PlayerbotAI;
class WorldObject;

// The visit driver (consolidation CS28): one lifecycle for every kind of visit, on the one VisitState in
// PlayerbotAI::bankMail.visit. Begin checks the one-visit refusal and writes the start row; each step watches the
// deadline, walks to the spawn, finds the object once near it, resolves it, walks into its reach, then lets the kind
// act; the end has the kind write its rows, puts on what the visit brought, clears the state and counts the end
// (VisitLifecycle.h). A kind supplies only what differs, as one row of functions (VisitKindRow).
namespace BankMail
{
// A visit's object spawn: the planner's service index entry the visit walks to
struct VisitSpot
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 entry = 0;       // the object's template
    float distance = 0.0f;  // yards from the bot when it was found
};

enum class VisitResult : uint8
{
    Idle,     // no visit
    Walk,     // walk to the spawn (object not found yet) or into reach of the object
    Pending,  // the kind acted or waits, the visit goes on
    Ended     // the visit ended this step, its rows written
};

struct VisitStep
{
    VisitResult result = VisitResult::Idle;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ObjectGuid object;  // set when the object is in sight but out of reach
    // The walk's reach: the kind's search yards to the spawn, its approach distance to the object
    float reach = 0.0f;
    // The end when the object's stand point cannot be reached: the kind's `missing`
    VisitEnd unreached = VisitEnd::NoSpot;
};

// What a kind's act or awaited answer tells the driver
enum class VisitNext : uint8
{
    Continue,  // the awaited answer came: the visit goes on in this step
    Pending,   // acted, or still waiting: the visit goes on in a later step
    Await,     // acted: the answer comes in a later step
    End        // the visit ends with `end`
};

struct VisitTurn
{
    VisitNext next = VisitNext::Pending;
    VisitEnd end = VisitEnd::Done;
};

// One kind of visit. Game reads happen on the bot's own map thread inside its AI update, except `end`, which may run on
// the world thread (EndVisits)
struct VisitKindRow
{
    char const* startWord;     // the VisitStart row's source
    float searchYards;         // the object is looked for once the bot is this close to the spawn
    VisitEnd missing;          // the end when the object is not found or gone
    uint32 (*limitSeconds)();  // the visit's deadline (its config row)
    // Once per visit, near the spawn: the object's guid, or empty
    ObjectGuid (*find)(Player* bot, VisitState const& visit);
    // At each step: the object, or nullptr when it is gone or no longer serves the bot
    WorldObject const* (*resolve)(Player* bot, ObjectGuid object);
    // In reach of the resolved object; `range` names its own range when the kind tests one
    bool (*reach)(Player* bot, WorldObject const& object, float& range);
    // The distance to approach the object to, from the range `reach` named; nullptr: the interaction distance
    float (*approach)(float range);
    // The awaited answer of the last act (VisitPhase::Await); nullptr for a kind that awaits none
    VisitTurn (*awaited)(PlayerbotAI* botAI, Player* bot);
    // In reach: one act
    VisitTurn (*act)(PlayerbotAI* botAI, Player* bot);
    // The kind's rows, event and debug line, and its own data cleared; true when the visit brought something to put on
    bool (*end)(PlayerbotAI* botAI, Player* bot, VisitState const& visit, VisitEnd end);
};

// The kinds the driver runs (each in its kind's .cpp)
VisitKindRow const& BankVisitRow();
VisitKindRow const& MailVisitRow();
VisitKindRow const& CraftVisitRow();

// World thread or the bot's map thread: starts a visit of the kind to the spot (null: in place, where the bot stands,
// with no object to find or reach); false while a visit of any kind runs
bool BeginVisit(PlayerbotAI* botAI, VisitKind kind, VisitSpot const* spot);
// The bot's map thread, inside its AI update: one step of the running visit
VisitStep StepVisit(PlayerbotAI* botAI);
// World thread or the bot's map thread: ends the running visit with its row; nothing without one
void EndVisit(PlayerbotAI* botAI, VisitEnd end);

// The kind's visits started and ended since the server start, by how they ended (any thread)
std::string VisitCounters(VisitKind kind);
}  // namespace BankMail

#endif
