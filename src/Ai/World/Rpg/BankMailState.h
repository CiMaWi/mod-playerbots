/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKMAILSTATE_H
#define _PLAYERBOT_BANKMAILSTATE_H

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "BankRules.h"
#include "Define.h"
#include "MailRules.h"
#include "ObjectGuid.h"
#include "VisitLifecycle.h"

class Player;
class PlayerbotAI;

// What a bot knows about its mail and its bank, and the state of a mailbox or bank visit (bank-mail plan). Held per
// bot in NewRpgInfo::bankMail, outside the RPG status: a visit is stepped from any status and a status reset keeps it.
// A bot knows that mail waits and what it expects from it, not what is inside (WB.4).
namespace BankMail
{
struct MailKnowledge
{
    uint16 waiting{0};         // unread mails delivered to its box, counted from the mail list (CountWaitingMail)
    uint32 seenCopper{0};      // worth the bot saw in mail it read and left in the box; in memory only
    uint32 expectedCopper{0};  // worth of the mail it expects (sent to itself, a sale); 0 until something fills it
    uint32 refreshedMs{0};     // when waiting was last counted; 0 = never
};

// An item the bot keeps in its bank for a later level
struct KeptItem
{
    uint32 entry{0};
    uint32 count{0};
    uint32 laterWorth{0};  // copper it is worth to the bot at dueLevel
    uint8 dueLevel{0};
};

// The items due now are read at the reader's level (DueAt), not stored: a level change makes them due
struct BankKnowledge
{
    std::vector<KeptItem> kept;  // the bank items the keep rule keeps now, by later worth (BankRulesInput.h)
    uint16 freeSlots{0};         // free bank item slots (the 28; bank bags are not counted)
    bool built{false};           // set at login (BuildBankKnowledge)
};

enum class MailVisitPhase : uint8
{
    None,  // no visit
    Walk,  // on the way to the mailbox
    List,  // at the mailbox: the next step is planned from the live list and queued
    Take   // a step queued (mark read, take, delete), its answer awaited
};

// One mailbox visit (MailVisit.h). The plan is recomputed from the live list at every step; this keeps only what
// the list cannot tell: the steps already answered and the takes the server refused, so nothing is sent twice.
struct MailVisitState
{
    MailVisitPhase phase{MailVisitPhase::None};
    uint32 mapId{0};  // the mailbox's spawn, from the planner's service index
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    ObjectGuid mailbox;  // found once the bot stands near the spawn; resolved at each use
    uint32 startedMs{0};
    uint32 limitMs{0};   // the visit's whole time, the walk included (BankMailConfig::MailVisitDeadlineSeconds)
    uint32 stepTick{0};  // the world tick (game time ms) the awaited step was queued in; answered in a later tick
    Step awaiting;
    uint32 moneyBefore{0};              // the bot's copper when the awaited step was queued
    MailOptions options;                // mails per visit and this visit's refused takes
    std::vector<Step> answered;         // never queued again on this visit
    std::vector<MailVerdict> verdicts;  // the first plan's mails, with what was really taken from each
    bool planned{false};
    uint32 mailsOpened{0};
    uint32 moneyTaken{0};  // copper
    uint32 itemsTaken{0};
};

enum class BankVisitPhase : uint8
{
    None,  // no visit
    Walk,  // on the way to the banker
    Move   // at the banker: one item moved per tick, planned from the live bags and bank
};

// One banker visit (BankVisit.h). Items move in place, so the plan is recomputed from the live bags and bank before
// every move; this keeps only what they cannot tell: the items already moved or refused, so none is tried twice, and
// the first plan's items with what was really done with each (the `bank` decision rows).
struct BankVisitState
{
    BankVisitPhase phase{BankVisitPhase::None};
    uint32 mapId{0};  // the banker's spawn, from the planner's service index
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    uint32 entry{0};    // the banker's creature template, looked for near the spawn
    ObjectGuid banker;  // found once the bot stands near the spawn; resolved at each use
    uint32 startedMs{0};
    uint32 limitMs{0};                  // the visit's whole time, the walk included (BankVisitDeadlineSeconds)
    std::vector<uint32> moved;          // item low guids moved on this visit
    std::vector<uint32> refused;        // item low guids the core refused to move
    std::vector<BankVerdict> verdicts;  // the first plan's items, with what was really done with each
    bool planned{false};
    uint32 deposited{0};
    uint32 withdrawn{0};
};

struct BotBankMail
{
    MailKnowledge mail;
    BankKnowledge bank;
    MailVisitState mailVisit;
    BankVisitState bankVisit;
};

// A visit of either kind runs: the one test the AI update makes while none does
inline bool VisitRuns(BotBankMail const& state)
{
    return state.mailVisit.phase != MailVisitPhase::None || state.bankVisit.phase != BankVisitPhase::None;
}

// One visit at a time per bot: why a visit of the kind may not start now, or nullptr
inline char const* VisitStartRefusal(BotBankMail const& state, VisitKind kind)
{
    return VisitStartRefusal(kind, state.mailVisit.phase != MailVisitPhase::None,
                             state.bankVisit.phase != BankVisitPhase::None);
}

// A far teleport or a PlayerbotAI::Reset(true) rebuilds NewRpgInfo (TravelLegs::AfterReset keeps only the route
// requests). The bot's knowledge (waiting mail, seen worth, bank slots, kept items) survives it; a running visit
// was ended with its row just before (EndVisits), so nothing of it is left to keep.
inline BotBankMail AfterReset(BotBankMail const& before)
{
    BotBankMail after;
    after.mail = before.mail;
    after.bank = before.bank;
    return after;
}

// Mail list reads (Player::GetMails) happen only on the bot's own map thread inside its AI update or on the world
// thread; never on a planner worker, never for another bot from a map thread, and no Mail* is kept past the call.
uint16 CountWaitingMail(Player const* bot);
void RefreshMailKnowledge(Player const* bot, MailKnowledge& mail);

// At login, on the world thread: the bank's kept items and free slots and the waiting mail, from what the character
// load brought (no query)
void BuildBankKnowledge(PlayerbotAI* botAI);

// The visits ended since the server start, by kind and end (any thread)
using VisitEndCounters = std::array<std::atomic<uint32>, VISIT_END_COUNT>;
std::string DescribeVisitCounters(VisitKind kind, uint32 started, VisitEndCounters const& ended);

// The watch's facts about a visit to a spot on mapId that started at startedMs
VisitWatchIn WatchInOf(Player const* bot, uint32 mapId, uint32 startedMs, uint32 limitMs);
// The bot's map thread, in every AI update while a visit runs, whatever engine runs (the stepping engine does not run
// in combat or dead): ends a visit at its deadline, at death or on another map, with its row
void WatchVisits(PlayerbotAI* botAI);
// The world thread or the bot's map thread, before its state is dropped (a far teleport, Reset(true), a logout):
// ends a running visit of either kind with its row
void EndVisits(PlayerbotAI* botAI, VisitEnd end);
}  // namespace BankMail

#endif
