/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MAILVISIT_H
#define _PLAYERBOT_MAILVISIT_H

#include <optional>
#include <string>

#include "Define.h"
#include "MailRules.h"
#include "ObjectGuid.h"
#include "VisitLifecycle.h"

class Player;
class PlayerbotAI;

// A persistent bot's mailbox visit (bank-mail plan, PLAN §3.3): walk into reach of the mailbox, take what the mail
// rules (MailRules.h) say one step at a time, wait for each answer, finish. Every step is a client packet queued on
// the bot's session; the world thread runs its handler when it drains the session (PlayerbotHolder::HandleBotPackets,
// after the map updates), so the answer shows in the mail list from the bot's next tick on. The state is
// NewRpgInfo::bankMail.mailVisit (BankMailState.h); every end writes its row (VisitLifecycle.h).
//
// Used unchanged by the chat command (MailAction) and the trips (signatures kept stable):
//   bool QueueMarkRead(Player* bot, ObjectGuid const& mailbox, uint32 mailId);
//   bool QueueTakeMoney(Player* bot, ObjectGuid const& mailbox, uint32 mailId);
//   bool QueueTakeItem(Player* bot, ObjectGuid const& mailbox, uint32 mailId, uint32 itemLowGuid);
//   bool QueueDelete(Player* bot, ObjectGuid const& mailbox, uint32 mailId);
//   bool QueueMailStep(Player* bot, ObjectGuid const& mailbox, Step const& step);
//   std::optional<MailboxSpot> FindMailboxSpot(Player* bot, char const*& refusal);
//   bool BeginMailVisit(PlayerbotAI* botAI, MailboxSpot const& spot);
//   MailVisitStep StepMailVisit(PlayerbotAI* botAI);
// Each Queue* returns false without a session; none calls a handler, and none reads the mail list.
namespace BankMail
{
// A mailbox's spawn: the planner's service index entry nearest the bot on its map
struct MailboxSpot
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float distance = 0.0f;  // yards from the bot when it was found
};

enum class MailVisitResult : uint8
{
    Idle,     // no visit
    Walk,     // walk to the spawn (mailbox empty) or into reach of the mailbox
    Pending,  // at the mailbox: a step queued or its answer awaited
    Ended     // the visit ended this tick, its rows written
};

struct MailVisitStep
{
    MailVisitResult result = MailVisitResult::Idle;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ObjectGuid mailbox;  // set when the mailbox is in sight but out of reach
};

// The client packets of a mailbox, queued on the bot's session (it takes ownership)
bool QueueMarkRead(Player* bot, ObjectGuid const& mailbox, uint32 mailId);
bool QueueTakeMoney(Player* bot, ObjectGuid const& mailbox, uint32 mailId);
bool QueueTakeItem(Player* bot, ObjectGuid const& mailbox, uint32 mailId, uint32 itemLowGuid);
bool QueueDelete(Player* bot, ObjectGuid const& mailbox, uint32 mailId);
bool QueueMailStep(Player* bot, ObjectGuid const& mailbox, Step const& step);

// World thread or the bot's map thread: the nearest mailbox spawn on the bot's map; refusal names why there is none
std::optional<MailboxSpot> FindMailboxSpot(Player* bot, char const*& refusal);
// World thread or the bot's map thread: starts a visit to the spot; false while a visit of either kind runs
bool BeginMailVisit(PlayerbotAI* botAI, MailboxSpot const& spot);
// The bot's map thread, inside its AI update (reads its mail list): one step of the visit
MailVisitStep StepMailVisit(PlayerbotAI* botAI);
// World thread or the bot's map thread (reads its mail list): ends a running visit with its row; nothing without one
void EndMailVisit(PlayerbotAI* botAI, VisitEnd end);

// Visits started and ended since the server start, by how they ended (any thread)
std::string MailVisitCounters();
}  // namespace BankMail

#endif
