/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CRAFTVISIT_H
#define _PLAYERBOT_CRAFTVISIT_H

#include <optional>
#include <string>
#include <string_view>

#include "Define.h"
#include "ObjectGuid.h"
#include "VisitLifecycle.h"

class Player;
class PlayerbotAI;

// A persistent bot's craft visit (professions plan WR7, PLAN 3.10): walk within the station's own range (none for a
// recipe without a spell focus or a processing spell), then cast the recipe or the processing spell one cast after
// another, each checked first with the craft rules (CraftRules.h) and each waited for until the bags show its
// result. The state is PlayerbotAI::bankMail.craftVisit (BankMailState.h); every end writes its row and counter
// (VisitLifecycle.h). Built on the mailbox and bank visits' code as it is (WR.18); consolidation's CS28 takes it in
// as one more caller of the one visit driver.
//
// Used by the GM commands and the craft errand (WR14, QueuedCraft and BeginQueuedCraft below):
//   std::optional<StationSpot> FindStationSpot(Player* bot, uint32 focus, char const*& refusal);
//   bool BeginCraftVisit(PlayerbotAI* botAI, CraftOrder const& order, StationSpot const* spot);
//   CraftVisitStep StepCraftVisit(PlayerbotAI* botAI);
namespace BankMail
{
// The nearest spawned station of a spell focus on the bot's map, from the planner's station index
struct StationSpot
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 entry = 0;       // the station's gameobject template
    float distance = 0.0f;  // yards from the bot when it was found
};

// What the visit casts: a recipe spell, or a processing spell on an item entry (source)
struct CraftOrder
{
    uint32 spell = 0;
    uint32 source = 0;  // the processed item's entry; 0 = a recipe
    uint32 focus = 0;   // the recipe's spell focus; 0 = none
    uint32 casts = 0;   // at least one
};

enum class CraftVisitResult : uint8
{
    Idle,     // no visit
    Walk,     // walk to the spawn (station not found yet) or into the station's range
    Pending,  // in reach: a cast started or is awaited, the visit goes on
    Ended     // the visit ended this tick, its rows written
};

struct CraftVisitStep
{
    CraftVisitResult result = CraftVisitResult::Idle;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ObjectGuid station;  // set when the station is in sight but out of its range
    float range = 0.0f;  // the station's own range, yards
};

// World thread or the bot's map thread, never per tick: the nearest station of the focus on the bot's map; refusal
// names why there is none
std::optional<StationSpot> FindStationSpot(Player* bot, uint32 focus, char const*& refusal);
// World thread or the bot's map thread: starts a visit (spot null: where the bot stands); false while a visit runs
bool BeginCraftVisit(PlayerbotAI* botAI, CraftOrder const& order, StationSpot const* spot);
// The bot's map thread, inside its AI update (reads its bags, casts): one step of the visit
CraftVisitStep StepCraftVisit(PlayerbotAI* botAI);
// World thread or the bot's map thread: ends a running visit with its row; nothing without one
void EndCraftVisit(PlayerbotAI* botAI, VisitEnd end);

// Visits started and ended since the server start, by how they ended (any thread)
std::string CraftVisitCounters();

// World thread or the bot's map thread (reads its bags): the order for a recipe spell the bot knows; casts 0 = as
// many as its bags allow now, at least one, so that the visit itself ends with the cause. refusal names why there is
// no order
std::optional<CraftOrder> RecipeOrder(Player* bot, uint32 spell, uint32 casts, char const*& refusal);
// The craft errand (WR14), the bot's map thread (reads its bags): the skill path's first queued craft from position
// from on that the bot knows, that is no enchant (it needs a target item) and whose reagents the bags hold; at = its
// position in the queue. queueWorthCopper = the queue's net worth in copper (SkillPath::QueueWorth). Null: none
std::optional<CraftOrder> QueuedCraft(PlayerbotAI* botAI, std::size_t from, std::size_t& at, float& queueWorthCopper);
// The first of those queued crafts that needs a station (WR14b): what a trip to a station is for; a craft without one
// does at any stop. Null: none
std::optional<CraftOrder> QueuedStationCraft(PlayerbotAI* botAI, float& queueWorthCopper);
// The craft errand's stop: starts a craft visit for the next queued craft from position tried on whose station (if
// it needs one) stands within reach yards; tried moves past the crafts looked at. False: none started
bool BeginQueuedCraft(PlayerbotAI* botAI, uint8& tried, float reach);
// The craft stop's disenchant (WR14c, PLAN 3.10), the bot's map thread (reads its bags): the first bag item of the
// disenchant usage (an enchanter's item whose outlet the item rules weigh above its sale, ItemUsageValue) that its
// skill takes, as a processing order where the bot stands, its casts bounded. Null: none
std::optional<CraftOrder> DisenchantOrder(PlayerbotAI* botAI);
// A persistent bot's `profession disenchant` row (WR12, WR19b): the item, its outlet's worth each in copper, what a
// vendor pays for it, the count (both disenchant callers write it)
void NoteDisenchant(PlayerbotAI* botAI, Player* bot, uint32 entry, uint32 count);
// The processing spells by their command word (disenchant, prospect, mill); 0 = none
uint32 ProcessSpellOf(std::string_view word);
// World thread or the bot's map thread (reads its bags): the first bag item the processing spell accepts from the
// bot (entry 0: any item), and the casts its stacks allow; 0 = none
uint32 ProcessableItem(Player* bot, uint32 spell, uint32 entry, uint32& casts);
}  // namespace BankMail

#endif
