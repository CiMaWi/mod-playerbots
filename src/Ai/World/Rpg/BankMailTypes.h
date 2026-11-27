/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKMAILTYPES_H
#define _PLAYERBOT_BANKMAILTYPES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    std::vector<uint32_t> uniqueMails;   // mail ids an earlier visit left only for unique copies (needs NR.7)
};

// What waits in the bot's mail of one item, read once into the mail knowledge (needs and errands NR.7)
struct WaitingUnits
{
    uint32_t entry = 0;
    uint32_t units = 0;
};

inline void AddUnits(std::vector<WaitingUnits>& waiting, uint32_t entry, uint32_t units)
{
    for (WaitingUnits& known : waiting)
        if (known.entry == entry)
        {
            known.units += units;
            return;
        }
    waiting.push_back({entry, units});
}

inline uint32_t UnitsOf(std::vector<WaitingUnits> const& waiting, uint32_t entry)
{
    for (WaitingUnits const& known : waiting)
        if (known.entry == entry)
            return known.units;
    return 0;
}

// The units still to buy of a want when some wait in the mail
inline uint32_t UnitsLacking(uint32_t wanted, uint32_t waiting) { return wanted > waiting ? wanted - waiting : 0; }

// One delivered auction house mail not taken yet and the copper it stands for
struct WaitingAuctionMail
{
    uint32_t id = 0;
    uint64_t copper = 0;
};

// The expected copper: what a visit saw and left is in the seen worth already, so it is left out here
inline uint32_t UnseenCopper(std::vector<WaitingAuctionMail> const& mails, std::vector<uint32_t> const& seenMails)
{
    uint64_t copper = 0;
    for (WaitingAuctionMail const& mail : mails)
        if (std::find(seenMails.begin(), seenMails.end(), mail.id) == seenMails.end())
            copper += mail.copper;
    return static_cast<uint32_t>(std::min<uint64_t>(copper, std::numeric_limits<uint32_t>::max()));
}

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
    Settled,       // moved or refused earlier on this visit: stays where it lies and takes no room (later plans only)
    KeptForSkill,  // a profession material or recipe kept for a skill the bot has not reached (professions WR12)
    // A move the core refused (BankVisit.cpp), a word per result code of Player::CanBankItem or CanStoreItem
    RefusedLooted,    // EQUIP_ERR_ALREADY_LOOTED: the item's loot is open or was generated (an opened box)
    RefusedNotOwned,  // EQUIP_ERR_DONT_OWN_THAT_ITEM: bound to another character
    RefusedLimit,     // EQUIP_ERR_CANT_CARRY_MORE_OF_THIS or a limit category: no more of it may be held
    RefusedFull,      // EQUIP_ERR_BANK_FULL (deposit) or EQUIP_ERR_INVENTORY_FULL (withdrawal): no room for it
    RefusedBag,       // EQUIP_ERR_CAN_ONLY_DO_WITH_EMPTY_BAGS: a bag that holds items
    RefusedInPlace,   // the core's place for the item is where it lies (Deposit's EQUIP_ERR_NONE)
    RefusedOther,     // any other code (its number goes to the server log)
    // A bag item the core's own deposit test refuses by its state (R6, BankRulesInput.cpp): kept in the bags, no
    // deposit planned; a designed skip, so no word starts with refused
    BlockedLooted,    // EQUIP_ERR_ALREADY_LOOTED: its loot is open or was generated (an opened box)
    BlockedNotOwned,  // EQUIP_ERR_DONT_OWN_THAT_ITEM
    BlockedLimit,     // EQUIP_ERR_CANT_CARRY_MORE_OF_THIS or a limit category
    BlockedOther,     // any other code but a full bank (room is the model's)
    // Needs and errands NR12a (NR.8's margin): sale stock whose listing pays now but would not after one more failed
    // auction stays in the bags, so the next hold does not release it from the bank
    ThinMargin
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
    // Needs and errands NR12a: a stack the reader judged as sale stock (KeepItemIn::stockCandidate), its listing's
    // worth and what a vendor pays for its stock units, the item usage's number for its entry (0: not asked) and the
    // plan that made its move: the visit's moves before that plan (0: the visit's first plan)
    bool saleStock = false;
    uint32_t worth = 0;
    uint32_t payout = 0;
    uint8_t usage = 0;
    uint32_t plan = 0;
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
    uint32_t itemGuid = 0;  // the bag stack's low guid (CarriedKeeps; the auction visit lists by it, ListsStack)
    // the core refuses its deposit by its state (KeepItemIn::depositBlock), or NR.8's margin keeps it in the bags
    // (KeptByMargin): no deposit to plan
    bool blocked = false;
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
