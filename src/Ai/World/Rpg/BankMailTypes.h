/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKMAILTYPES_H
#define _PLAYERBOT_BANKMAILTYPES_H

#include <cstddef>
#include <cstdint>
#include <vector>

// The bank and mail plain types a bot's state holds (BankMailState.h): a mail visit's steps and verdicts
// (MailRules.h), a bank visit's verdicts and the kept items (BankRules.h), a visit's kind and end
// (VisitLifecycle.h). The rules stay in those headers.
namespace BankMail
{
// The client's inbox shows this many mails
constexpr uint32_t DEFAULT_MAILS_PER_VISIT = 50;

struct MailOptions
{
    uint32_t mailsPerVisit = DEFAULT_MAILS_PER_VISIT;
    std::vector<uint32_t> refusedItems;  // item guids whose take the server refused on this visit
    std::vector<uint32_t> refusedMoney;  // mail ids whose money the server refused (the gold cap)
};

enum class StepKind : uint8_t
{
    MarkRead,
    TakeMoney,
    TakeItem,
    Delete
};

struct Step
{
    StepKind kind = StepKind::MarkRead;
    uint32_t mailId = 0;
    uint32_t itemGuid = 0;  // TakeItem only
};

// The mail decision row's choice and reason (world log kind `mail`)
enum class MailChoice : uint8_t
{
    Take,   // opened, nothing stays: the mail is deleted
    Leave,  // opened, something stays in it
    Skip    // not opened
};

enum class MailReason : uint8_t
{
    Emptied,
    NoSpace,
    Refused,
    Cod,
    NotDelivered,
    Later,   // beyond the mails one visit opens
    Unique,  // the core's test says the bot cannot take the item (a second copy of a unique item, a limit)
    Deleted
};

struct MailVerdict
{
    uint32_t mailId = 0;
    MailChoice choice = MailChoice::Skip;
    MailReason reason = MailReason::Later;
    uint32_t moneyTaken = 0;
    uint32_t itemsTaken = 0;
    uint32_t itemsLeft = 0;
    uint32_t worthLeft = 0;  // copper left in the mail that the bot has now seen
};

// The bank decision row's choice and reason (world log kind `bank`)
enum class BankChoice : uint8_t
{
    Withdraw,  // taken out of the bank
    Deposit,   // put into the bank
    Keep,      // a kept item that stays in the bags
    Leave      // a bank item that stays in the bank
};

enum class BankReason : uint8_t
{
    Due,           // kept and due at the bot's level
    Released,      // in the bank but no longer worth more later: withdrawn to be sold or used as an ordinary item
    KeptForLevel,  // kept for a level the bot has not reached
    KeptStock,     // use stock beyond what the bags carry (item use WI9b): banked, never due by level
    BankFull,      // kept, but the bank has no room left for it
    NoSpace,       // to be withdrawn, but the bags have no room for it
    Refused,       // the core refused a move the plan allowed (BankVisit.cpp); not tried again on that visit
    KeptForSkill   // a profession material or recipe kept for a skill the bot has not reached (professions WR12)
};

struct BankVerdict
{
    BankChoice choice = BankChoice::Leave;
    BankReason reason = BankReason::KeptForLevel;
    bool inBank = false;  // where the item lies before the visit
    uint8_t bag = 0;
    uint8_t slot = 0;
    uint32_t itemGuid = 0;
    uint32_t entry = 0;
    uint32_t count = 0;
    uint32_t laterWorth = 0;  // 0 for a released item
    uint32_t sellNow = 0;
    uint8_t dueLevel = 0;
    char const* provider = "";
};

// What the bank-space rule does at a banker (BankRules.h, DecideBankSpace)
enum class SpaceChoice : uint8_t
{
    None,      // everything kept fits the bank
    BuySlot,   // the next bank bag slot is bought
    PlaceBag,  // a carried bag goes into a bought slot that holds none
    NoSlot     // something does not fit and nothing is bought
};

enum class SpaceReason : uint8_t
{
    Fits,       // nothing kept is left out
    Worth,      // what does not fit is worth the price
    Dear,       // the price is above what does not fit is worth
    Money,      // the money above the reserve does not allow it (ShouldBuyBag)
    NoBag,      // no plain bag carried: a slot alone holds nothing
    AllBought,  // every bank bag slot is bought and holds a bag
    Refused     // the core refused the purchase or the bag move (BankVisit.cpp); not tried again on that visit
};

struct SpaceAnswer
{
    SpaceChoice choice = SpaceChoice::None;
    SpaceReason reason = SpaceReason::Fits;
    uint32_t overflow = 0;  // kept bag items left out for a full bank
    uint32_t worth = 0;     // the later worth of those the bag would hold
    uint32_t price = 0;     // the slot's price and the bag's sale, copper
    uint32_t bagEntry = 0;  // the bag the slot gets (0: none)
    uint32_t slots = 0;     // bank bag slots bought after the step
};

// A bank item that is kept now, as BankKnowledge stores it
struct KeptEntry
{
    uint32_t entry = 0;
    uint32_t count = 0;
    uint32_t laterWorth = 0;
    uint8_t dueLevel = 0;
    char const* provider = "";
};

// The bag items the rule keeps for a later level, as the bot remembers them for its item usage (ITEM_USAGE_BANK):
// such an item is not sold, and when space runs short it counts at its later worth. Decided at one level and read
// only at that level, so a level change never leaves an item kept by an old answer.
struct CarriedKeeps
{
    std::vector<KeptEntry> items;  // by later worth
    uint8_t level = 0;             // the bot's level when they were decided; 0 = never
    uint32_t auctionGain = 0;      // the auction stock among them: listing's worth above the vendor's payout (WA3b)
};

enum class VisitKind : uint8_t
{
    Mail,
    Bank,
    Craft,   // the professions plan's craft visit (WR7 drives it)
    Auction  // the auction plan's visit to an auctioneer (WA3b drives it)
};

// How a visit ended: the row's source word (VisitEndWord) and the counter it moves
enum class VisitEnd : uint8_t
{
    Done,       // the plan's every step answered or move made
    Timeout,    // the deadline passed out of combat
    NoSpot,     // no mailbox, banker or auctioneer where the index put it, or it went
    Unreached,  // the last walk to the box, banker or station failed: the NPC or box is there
    // The craft visit's own ends (professions plan), reached by the visit itself, so before the interruptions
    NoMaterials,  // the materials were not in the bags
    NoStation,    // no station the craft needs where the index put it
    BagsFull,     // no bag space for what the craft makes
    Combat,       // the deadline passed in combat (the stepping engine runs out of combat only)
    Teleport,     // a teleport, far or near (PlayerbotAI::HandleTeleportAck)
    Reset,        // PlayerbotAI::Reset(true)
    Death,        // the bot died
    MapChange,    // the bot is on another map than the spot's
    Logout,       // the bot logs out, or its AI is removed
    Count
};
constexpr size_t VISIT_END_COUNT = static_cast<size_t>(VisitEnd::Count);
}  // namespace BankMail

#endif
