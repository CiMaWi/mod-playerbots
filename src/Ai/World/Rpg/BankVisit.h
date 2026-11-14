/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKVISIT_H
#define _PLAYERBOT_BANKVISIT_H

#include <optional>
#include <string>

#include "Define.h"
#include "ObjectGuid.h"
#include "VisitLifecycle.h"

class Player;
class PlayerbotAI;

// A persistent bot's banker visit (bank-mail plan, PLAN §3.5): walk into reach of a banker, withdraw what is due or
// no longer kept, deposit what is kept (BankRules.h), equip, finish. Items move in place on the bot's map thread
// through the core's own checks (CanStoreItem, CanBankItem), as the core's bank handlers do; no packet is queued, so
// the plan is recomputed from the live bags and bank before every move. The state is NewRpgInfo::bankMail.bankVisit
// (BankMailState.h); every end writes its row (VisitLifecycle.h).
//
// Used by the GM command and, later, the trips (signatures kept stable):
//   std::optional<BankerSpot> FindBankerSpot(Player* bot, char const*& refusal);
//   bool BeginBankVisit(PlayerbotAI* botAI, BankerSpot const& spot);
//   BankVisitStep StepBankVisit(PlayerbotAI* botAI);
namespace BankMail
{
// A banker's spawn: the planner's service index entry nearest the bot on its map that serves the bot's faction
struct BankerSpot
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 entry = 0;       // the banker's creature template
    float distance = 0.0f;  // yards from the bot when it was found
};

enum class BankVisitResult : uint8
{
    Idle,     // no visit
    Walk,     // walk to the spawn (banker not found yet) or into reach of the banker
    Pending,  // at the banker: an item moved this tick, the visit goes on
    Ended     // the visit ended this tick, its rows written
};

struct BankVisitStep
{
    BankVisitResult result = BankVisitResult::Idle;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ObjectGuid banker;  // set when the banker is in sight but out of reach
};

// World thread or the bot's map thread: the nearest banker spawn on the bot's map whose faction serves the bot;
// refusal names why there is none
std::optional<BankerSpot> FindBankerSpot(Player* bot, char const*& refusal);
// World thread or the bot's map thread: starts a visit to the spot; false while a visit of either kind runs
bool BeginBankVisit(PlayerbotAI* botAI, BankerSpot const& spot);
// The bot's map thread, inside its AI update (reads and moves its items): one step of the visit
BankVisitStep StepBankVisit(PlayerbotAI* botAI);
// World thread or the bot's map thread (reads its bank): ends a running visit with its row; nothing without one
void EndBankVisit(PlayerbotAI* botAI, VisitEnd end);

// Visits started and ended since the server start, by how they ended (any thread)
std::string BankVisitCounters();
}  // namespace BankMail

#endif
