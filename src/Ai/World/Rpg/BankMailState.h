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

#include "BankMailTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "VisitState.h"

class PlayerbotAI;

// What a bot knows about its mail and its bank, and the state of a mailbox or bank visit (bank-mail plan). Held per
// bot in PlayerbotAI::bankMail, outside the RPG status: a visit is stepped from any status and a status reset keeps it.
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
    CarriedKeeps carried;        // the bag items it keeps for a later level, for the item usage (RefreshCarriedKeeps)
    uint16 freeSlots{0};         // free bank slots: the 28 item slots and the plain bank bags' slots (WR17)
    uint16 slots{0};             // the bank's 28 item slots and its plain bags' slots
    bool built{false};           // set at login (BuildBankKnowledge)
};

// The mail's own data of a mailbox visit (MailVisit.h; the lifecycle is BotBankMail::visit). The plan is recomputed
// from the live list at every step; this keeps only what the list cannot tell: the steps already answered and the
// takes the server refused, so nothing is sent twice.
struct MailVisitData
{
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

// The bank's own data of a banker visit (BankVisit.h; the lifecycle is BotBankMail::visit). Items move in place, so
// the plan is recomputed from the live bags and bank before every move; this keeps only what they cannot tell: the
// items already moved or refused, so none is tried twice, and the first plan's items with what was really done with
// each (the `bank` decision rows).
struct BankVisitData
{
    std::vector<uint32> moved;          // item low guids moved on this visit
    std::vector<uint32> refused;        // item low guids the core refused to move
    std::vector<BankVerdict> verdicts;  // the first plan's items, with what was really done with each
    bool planned{false};
    uint32 deposited{0};
    uint32 withdrawn{0};
    std::vector<SpaceAnswer>
        space;              // professions WR17: the bank-space steps taken, then the decision that stopped them
    bool spaceDone{false};  // the bank-space rule has nothing more to do on this visit
};

// The craft's own data of a craft visit (CraftVisit.h, professions WR7; the lifecycle is BotBankMail::visit): one
// recipe or one processing spell, cast up to `wanted` times. The bags are read before and after each cast, so this
// keeps only the counts the next read is compared with.
struct CraftVisitData
{
    uint32 spell{0};
    uint32 source{0};  // a processing cast's item entry; 0 = a recipe
    uint32 wanted{0};  // casts asked for
    uint32 made{0};
    uint32 refused{0};        // casts the core did not start, or that made nothing
    uint32 castMs{0};         // when the awaited cast started
    uint32 castBoundMs{0};    // how long it may stay current: the step bound or its cast time, the larger
    uint32 productBefore{0};  // the product's count in the bags before the cast
    uint32 sourceBefore{0};   // the first reagent's or the processed item's count before the cast
    int32 lastResult{-1};     // the last refused cast's SpellCastResult, -1 = none
    uint32 fire{0};           // the spell that lights the station where the bot stands first (WR15); 0: none or lit
};

struct BotBankMail
{
    MailKnowledge mail;
    BankKnowledge bank;
    VisitState visit;  // the one visit the driver runs (Visit.h), of any kind
    MailVisitData mailData;
    BankVisitData bankData;
    CraftVisitData craftData;
};

// A visit of any kind runs: the one test the AI update makes while none does
inline bool VisitRuns(BotBankMail const& state) { return state.visit.phase != VisitPhase::None; }

// At login, on the world thread: the bank's kept items and free slots and the waiting mail, from what the character
// load brought (no query)
void BuildBankKnowledge(PlayerbotAI* botAI);

// The visits ended since the server start, by kind and end (any thread)
using VisitEndCounters = std::array<std::atomic<uint32>, VISIT_END_COUNT>;
std::string DescribeVisitCounters(VisitKind kind, uint32 started, VisitEndCounters const& ended);

// The bot's map thread, in every AI update while a visit runs, whatever engine runs (the stepping engine does not run
// in combat or dead): ends a visit at its deadline, at death or on another map, with its row
void WatchVisits(PlayerbotAI* botAI);
// The world thread or the bot's map thread, before its state is dropped (a far teleport, Reset(true), a logout) or
// at a near teleport: ends a running visit of any kind with its row
void EndVisits(PlayerbotAI* botAI, VisitEnd end);
}  // namespace BankMail

#endif
