/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKRULES_H
#define _PLAYERBOT_BANKRULES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "MailRules.h"

// The Player-free rules of a persistent bot's bank (bank-mail plan, WB.2): an item goes to the bank when it is worth
// more to this bot later than it fetches now. Header-only so tests/bank-rules-selftest.cpp checks the same code the
// server runs. All worth is in copper.
//
// A keep provider names, for one item, its later worth and the level it is due at (KeepAnswer). The providers are
// asked in KEEP_PROVIDERS' order; the highest later worth among the answers that keep wins. A new provider (materials
// for professions, auction stock, gear sets) is one function plus one line in KEEP_PROVIDERS, and the facts it reads
// as fields of KeepItemIn / KeepBotIn, filled by BankRulesInput.cpp.
//
// The bank visit and the knowledge use (signatures kept stable):
//   KeepAnswer DecideKeep(KeepItemIn const& item, KeepBotIn const& bot);   // the providers and the rule
//   BankPlan PlanBankVisit(BankIn const& in, std::vector<uint32_t> const& refused = {});
//                                                    // withdraw steps, then deposit steps; refused items stay put
//   std::vector<KeptEntry> KeptInBank(BankIn const& in);                  // the bank items kept now
//   char const* ChoiceWord(BankChoice), ReasonWord(BankReason);           // the `bank` decision row's words
// BankRulesInput.h builds BankIn from the live bags and bank (BuildBankIn) and refreshes the bot's BankKnowledge.
namespace BankMail
{
// The bank's item slots this plan uses (BANK_SLOT_ITEM_START..END); bank bags are WR's (WB.3)
constexpr uint32_t BANK_ITEM_SLOTS = 28;
// The levels ahead a kept item may be due at, until the config supplies one (BankMail.KeepLevelHorizon)
constexpr uint8_t DEFAULT_KEEP_LEVEL_HORIZON = 5;
// A later item counts as an upgrade when its score beats the worn one's times this (Playerbots.EquipUpgradeThreshold)
constexpr float DEFAULT_UPGRADE_THRESHOLD = 1.1f;

struct KeepItemIn
{
    uint32_t itemGuid = 0;  // the item's low guid
    uint32_t entry = 0;
    uint32_t count = 1;
    uint32_t stackMax = 1;
    uint8_t requiredLevel = 0;
    uint32_t sellNow = 0;    // what selling it fetches now (WT's worth for a vendor), finite
    uint32_t useWorth = 0;   // what it is worth to a bot that uses it (WT's Use worth), finite
    bool wearable = false;   // the bot's class wears its kind at the item's level (weapon, armor type, class, race)
    float score = 0.0f;      // its stat score for this bot
    float wornScore = 0.0f;  // the score of the worse item worn in the slots it takes
    bool slotEmpty = false;  // one of those slots is empty
    bool bound = false;      // soulbound (WA: a bound item cannot be auctioned); no WB rule reads it
};

struct KeepBotIn
{
    uint8_t level = 1;
    uint8_t horizon = DEFAULT_KEEP_LEVEL_HORIZON;
    float upgradeThreshold = DEFAULT_UPGRADE_THRESHOLD;
};

// A provider's answer; a provider that does not keep the item returns the default
struct KeepAnswer
{
    bool keeps = false;
    uint32_t laterWorth = 0;  // copper the item is worth to the bot at dueLevel
    uint8_t dueLevel = 0;     // the level it is taken out at; at or below the bot's level it is due now
    char const* provider = "";
};

using KeepProvider = KeepAnswer (*)(KeepItemIn const& item, KeepBotIn const& bot);

// Gear for a later level (WB): an item the bot's class wears, at most horizon levels above the bot, that beats what
// it wears now. Its later worth is WT's worth of an upgrade (the Use worth); it is due at its required level. An item
// the bot may already wear keeps its answer while it is an upgrade, so a banked item is due rather than released.
inline KeepAnswer GearForLaterLevel(KeepItemIn const& item, KeepBotIn const& bot)
{
    if (!item.wearable || item.score <= 0.0f)
        return {};
    if (item.requiredLevel > bot.level && item.requiredLevel - bot.level > bot.horizon)
        return {};
    if (!item.slotEmpty && item.score <= item.wornScore * bot.upgradeThreshold)
        return {};

    return {true, item.useWorth, std::max(item.requiredLevel, uint8_t{1}), "gear_for_level"};
}

// Every provider, asked in this order
inline constexpr KeepProvider KEEP_PROVIDERS[] = {GearForLaterLevel};

// The rule (WB.2): kept when its later worth exceeds what it fetches now; an equal worth is not kept
inline bool Keeps(KeepAnswer const& answer, uint32_t sellNow) { return answer.keeps && answer.laterWorth > sellNow; }

// The best answer that passes the rule: the highest later worth, the first provider on a tie; else the default
inline KeepAnswer DecideKeep(KeepItemIn const& item, KeepBotIn const& bot, std::span<KeepProvider const> providers)
{
    KeepAnswer best;
    for (KeepProvider provider : providers)
    {
        KeepAnswer const answer = provider(item, bot);
        if (Keeps(answer, item.sellNow) && (!best.keeps || answer.laterWorth > best.laterWorth))
            best = answer;
    }
    return best;
}

inline KeepAnswer DecideKeep(KeepItemIn const& item, KeepBotIn const& bot)
{
    return DecideKeep(item, bot, KEEP_PROVIDERS);
}

inline bool IsDue(KeepAnswer const& answer, KeepBotIn const& bot)
{
    return answer.keeps && answer.dueLevel <= bot.level;
}

// An item where it lies now: bag is the core's bag id (INVENTORY_SLOT_BAG_0 for the backpack and the bank's item
// slots, else the bag's equipment slot), slot its slot in that bag
struct BankItemIn
{
    KeepItemIn item;
    uint8_t bag = 0;
    uint8_t slot = 0;
};

struct BankIn
{
    KeepBotIn bot;
    std::vector<BankItemIn> bags;  // the backpack and the plain bags
    std::vector<BankItemIn> bank;  // the bank's 28 item slots
    BagSpace bagSpace;             // free bag slots and room on partial stacks there
    BagSpace bankSpace;            // free bank item slots and room on partial stacks there
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
    BankFull,      // kept, but the bank has no room left for it
    NoSpace,       // to be withdrawn, but the bags have no room for it
    Refused        // the core refused a move the plan allowed (BankVisit.cpp); not tried again on that visit
};

inline char const* ChoiceWord(BankChoice choice)
{
    switch (choice)
    {
        case BankChoice::Withdraw:
            return "withdraw";
        case BankChoice::Deposit:
            return "deposit";
        case BankChoice::Keep:
            return "keep";
        default:
            return "leave";
    }
}

inline char const* ReasonWord(BankReason reason)
{
    switch (reason)
    {
        case BankReason::Due:
            return "due";
        case BankReason::Released:
            return "released";
        case BankReason::KeptForLevel:
            return "kept_for_level";
        case BankReason::BankFull:
            return "bank_full";
        case BankReason::Refused:
            return "refused";
        default:
            return "no_space";
    }
}

// One item move of a visit: where the item lies now (bag and slot) and what it is, so the visit can check the slot
// still holds it before it moves
struct BankStep
{
    BankChoice kind = BankChoice::Withdraw;  // Withdraw or Deposit
    uint8_t bag = 0;
    uint8_t slot = 0;
    uint32_t itemGuid = 0;
    uint32_t entry = 0;
    uint32_t count = 0;
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

struct BankPlan
{
    std::vector<BankStep> steps;        // every withdrawal, then every deposit
    std::vector<BankVerdict> verdicts;  // bank items, then the bags' kept items, in the order they were decided
    uint32_t withdrawDue = 0;           // due items withdrawn
    uint32_t withdrawReleased = 0;      // released items withdrawn
    uint32_t deposits = 0;
    uint32_t keptInBags = 0;     // kept items that stay in the bags (due, or the bank is full)
    uint32_t leftInBank = 0;     // bank items that stay (kept for a level, or no bag room)
    uint32_t bankFreeAfter = 0;  // free bank item slots after the visit
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

namespace BankRulesDetail
{
struct Decided
{
    BankItemIn const* item = nullptr;
    KeepAnswer answer;
};

// The highest worth first; ties by entry, then by place, so the order never depends on the input's order
inline bool ByWorth(uint32_t worthA, uint32_t worthB, BankItemIn const& a, BankItemIn const& b)
{
    if (worthA != worthB)
        return worthA > worthB;
    if (a.item.entry != b.item.entry)
        return a.item.entry < b.item.entry;
    if (a.bag != b.bag)
        return a.bag < b.bag;
    return a.slot < b.slot;
}

inline void SortByLaterWorth(std::vector<Decided>& items)
{
    std::sort(items.begin(), items.end(), [](Decided const& a, Decided const& b)
              { return ByWorth(a.answer.laterWorth, b.answer.laterWorth, *a.item, *b.item); });
}

inline void SortBySellWorth(std::vector<Decided>& items)
{
    std::sort(items.begin(), items.end(), [](Decided const& a, Decided const& b)
              { return ByWorth(a.item->item.sellNow, b.item->item.sellNow, *a.item, *b.item); });
}

inline StackRoom* FindRoom(BagSpace& space, uint32_t entry)
{
    auto room = std::find_if(space.stackRoom.begin(), space.stackRoom.end(),
                             [entry](StackRoom const& stack) { return stack.entry == entry; });
    return room == space.stackRoom.end() ? nullptr : &*room;
}

// Takes room for the item: on partial stacks of its entry when they hold all of it, else one free slot (the
// conservative fit of the mail rules: a stack is never split over two places)
inline bool TakeRoom(BagSpace& space, KeepItemIn const& item)
{
    StackRoom* room = item.stackMax > 1 ? FindRoom(space, item.entry) : nullptr;
    if (room && room->room >= item.count)
    {
        room->room -= item.count;
        return true;
    }
    if (!space.freeSlots)
        return false;

    --space.freeSlots;
    if (item.stackMax > item.count)
    {
        if (StackRoom* stack = FindRoom(space, item.entry))
            stack->room += item.stackMax - item.count;
        else
            space.stackRoom.push_back({item.entry, item.stackMax - item.count});
    }
    return true;
}

// The item's whole stack leaves: its slot is free and the room on its partial stack is gone
inline void GiveRoom(BagSpace& space, KeepItemIn const& item)
{
    ++space.freeSlots;
    if (item.stackMax <= item.count)
        return;
    if (StackRoom* stack = FindRoom(space, item.entry))
        stack->room -= std::min(stack->room, item.stackMax - item.count);
}

inline BankVerdict MakeVerdict(BankItemIn const& in, KeepAnswer const& answer, bool inBank, BankChoice choice,
                               BankReason reason)
{
    BankVerdict verdict;
    verdict.choice = choice;
    verdict.reason = reason;
    verdict.inBank = inBank;
    verdict.bag = in.bag;
    verdict.slot = in.slot;
    verdict.itemGuid = in.item.itemGuid;
    verdict.entry = in.item.entry;
    verdict.count = in.item.count;
    verdict.laterWorth = answer.laterWorth;
    verdict.sellNow = in.item.sellNow;
    verdict.dueLevel = answer.dueLevel;
    verdict.provider = answer.provider;
    return verdict;
}

inline BankStep MakeStep(BankItemIn const& in, BankChoice kind)
{
    return {kind, in.bag, in.slot, in.item.itemGuid, in.item.entry, in.item.count};
}

inline bool IsRefused(std::vector<uint32_t> const& refused, BankItemIn const& in)
{
    return std::find(refused.begin(), refused.end(), in.item.itemGuid) != refused.end();
}
}  // namespace BankRulesDetail

// What a bot does at a banker now. Withdrawals come first: due items by later worth, then released items by what
// they fetch, each while the bags have room. Deposits follow: the bags' kept items that are not due, by later worth,
// while the bank has room, counting the slots the withdrawals freed. When the bank is full the lowest-worth kept items
// stay in the bags; nothing already banked is taken out to make room. An item in refused (low guids the core
// refused to move on this visit) stays where it is and takes no room.
inline BankPlan PlanBankVisit(BankIn const& in, std::vector<uint32_t> const& refused = {})
{
    using namespace BankRulesDetail;
    BankPlan plan;
    BagSpace bagSpace = in.bagSpace;
    BagSpace bankSpace = in.bankSpace;

    std::vector<Decided> due;
    std::vector<Decided> released;
    std::vector<Decided> waiting;
    for (BankItemIn const& item : in.bank)
    {
        KeepAnswer const answer = DecideKeep(item.item, in.bot);
        if (!answer.keeps)
            released.push_back({&item, answer});
        else if (IsDue(answer, in.bot))
            due.push_back({&item, answer});
        else
            waiting.push_back({&item, answer});
    }
    SortByLaterWorth(due);
    SortBySellWorth(released);
    SortByLaterWorth(waiting);

    auto withdraw = [&](Decided const& decided, BankReason reason, uint32_t& counter)
    {
        if (IsRefused(refused, *decided.item))
        {
            plan.verdicts.push_back(
                MakeVerdict(*decided.item, decided.answer, true, BankChoice::Leave, BankReason::Refused));
            ++plan.leftInBank;
            return;
        }
        if (!TakeRoom(bagSpace, decided.item->item))
        {
            plan.verdicts.push_back(
                MakeVerdict(*decided.item, decided.answer, true, BankChoice::Leave, BankReason::NoSpace));
            ++plan.leftInBank;
            return;
        }
        GiveRoom(bankSpace, decided.item->item);
        plan.steps.push_back(MakeStep(*decided.item, BankChoice::Withdraw));
        plan.verdicts.push_back(MakeVerdict(*decided.item, decided.answer, true, BankChoice::Withdraw, reason));
        ++counter;
    };
    for (Decided const& decided : due)
        withdraw(decided, BankReason::Due, plan.withdrawDue);
    for (Decided const& decided : released)
        withdraw(decided, BankReason::Released, plan.withdrawReleased);
    for (Decided const& decided : waiting)
    {
        plan.verdicts.push_back(
            MakeVerdict(*decided.item, decided.answer, true, BankChoice::Leave, BankReason::KeptForLevel));
        ++plan.leftInBank;
    }

    std::vector<Decided> deposit;
    std::vector<Decided> dueInBags;
    for (BankItemIn const& item : in.bags)
    {
        KeepAnswer const answer = DecideKeep(item.item, in.bot);
        if (!answer.keeps)
            continue;
        if (IsDue(answer, in.bot))
            dueInBags.push_back({&item, answer});
        else
            deposit.push_back({&item, answer});
    }
    SortByLaterWorth(deposit);
    SortByLaterWorth(dueInBags);
    for (Decided const& decided : deposit)
    {
        if (IsRefused(refused, *decided.item))
        {
            plan.verdicts.push_back(
                MakeVerdict(*decided.item, decided.answer, false, BankChoice::Keep, BankReason::Refused));
            ++plan.keptInBags;
            continue;
        }
        if (TakeRoom(bankSpace, decided.item->item))
        {
            plan.steps.push_back(MakeStep(*decided.item, BankChoice::Deposit));
            plan.verdicts.push_back(
                MakeVerdict(*decided.item, decided.answer, false, BankChoice::Deposit, BankReason::KeptForLevel));
            ++plan.deposits;
            continue;
        }
        plan.verdicts.push_back(
            MakeVerdict(*decided.item, decided.answer, false, BankChoice::Keep, BankReason::BankFull));
        ++plan.keptInBags;
    }
    for (Decided const& decided : dueInBags)
    {
        plan.verdicts.push_back(MakeVerdict(*decided.item, decided.answer, false, BankChoice::Keep, BankReason::Due));
        ++plan.keptInBags;
    }

    plan.bankFreeAfter = bankSpace.freeSlots;
    return plan;
}

// The bank items the rule keeps now, by later worth (a released item is not kept)
inline std::vector<KeptEntry> KeptInBank(BankIn const& in)
{
    using namespace BankRulesDetail;
    std::vector<Decided> kept;
    for (BankItemIn const& item : in.bank)
    {
        KeepAnswer const answer = DecideKeep(item.item, in.bot);
        if (answer.keeps)
            kept.push_back({&item, answer});
    }
    SortByLaterWorth(kept);

    std::vector<KeptEntry> entries;
    for (Decided const& decided : kept)
        entries.push_back({decided.item->item.entry, decided.item->item.count, decided.answer.laterWorth,
                           decided.answer.dueLevel, decided.answer.provider});
    return entries;
}

// One line of what a plan does, for the selftest and the command
inline std::string DescribePlan(BankPlan const& plan)
{
    return "withdraw " + std::to_string(plan.withdrawDue) + " due + " + std::to_string(plan.withdrawReleased) +
           " released, deposit " + std::to_string(plan.deposits) + ", kept in bags " + std::to_string(plan.keptInBags) +
           ", left in bank " + std::to_string(plan.leftInBank) + ", bank free after " +
           std::to_string(plan.bankFreeAfter) + ", " + std::to_string(plan.steps.size()) + " steps";
}

inline std::string DescribeVerdict(BankVerdict const& verdict)
{
    std::string line = std::string(verdict.inBank ? "bank" : "bags") + " " + std::to_string(verdict.bag) + "/" +
                       std::to_string(verdict.slot) + " item " + std::to_string(verdict.entry) + " x" +
                       std::to_string(verdict.count) + ": " + ChoiceWord(verdict.choice) + " " +
                       ReasonWord(verdict.reason) + ", sells now " + std::to_string(verdict.sellNow);
    if (verdict.laterWorth)
        line += ", later " + std::to_string(verdict.laterWorth) + " at level " + std::to_string(verdict.dueLevel) +
                " (" + verdict.provider + ")";
    return line;
}
}  // namespace BankMail

#endif
