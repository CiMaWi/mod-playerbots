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
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "BankMailTypes.h"
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
//   void AssignStock(entries, laterBySlot, bags, bank);                   // demand's use stock per stack (WI9b)
//   CarriedKeeps KeptInBags(BankIn const& in);                            // the bag items kept for a later level
//   uint32_t KeptWorthEach(CarriedKeeps const&, uint32_t entry, uint8_t level);  // the item usage's O(k) lookup
//   DueTotal DueAt(std::vector<Kept> const& kept, uint8_t level);         // the kept items due at the level
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
// Item use WI9b: the bank keeps up to this many more wanted stocks of a demand slot as stock for later [E]
constexpr float BANK_STOCK_SHARE = 1.0f;
// The due level of stock for later: no level makes it due, so it makes no bank trip (ErrandWaits, DueAt)
constexpr uint8_t STOCK_LATER_LEVEL = 255;

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
    // Item use WI9b, demand's use stock (AssignStock): one unit's worth to the bot (0: not demand's or unusable),
    // whether this stack is part of what the bags carry, or stock for later, and whether demand keeps its entry (a kept
    // entry is never sold, whatever its worth)
    uint32_t stockWorthEach = 0;
    bool stockCarried = false;
    bool stockLater = false;
    bool stockKept = false;
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
    // Not for sale: demand keeps the item's entry (item use WI9b), so the bank weighs it against the bags, not a sale
    bool unsold = false;
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

// Use stock (item use WI9b): a stack the bags carry is due now (it stays in the bags, a bank stack is withdrawn); a
// stack for later is due at no level, so it is banked and never fetched by a trip of its own. A stack of an entry
// demand keeps is unsold (demand never sells it); a surplus entry's stock for later must beat its sale (WB.2)
inline KeepAnswer StockForLater(KeepItemIn const& item, KeepBotIn const& bot)
{
    if (!item.stockWorthEach || (!item.stockCarried && !item.stockLater))
        return {};

    uint64_t const worth = static_cast<uint64_t>(item.stockWorthEach) * std::max<uint32_t>(item.count, 1);
    uint32_t const laterWorth = static_cast<uint32_t>(std::min<uint64_t>(worth, std::numeric_limits<uint32_t>::max()));
    return {true, laterWorth, item.stockCarried ? std::max(bot.level, uint8_t{1}) : STOCK_LATER_LEVEL, "stock",
            item.stockKept};
}

// Every provider, asked in this order
inline constexpr KeepProvider KEEP_PROVIDERS[] = {GearForLaterLevel, StockForLater};

// The rule (WB.2): kept when its later worth exceeds what it fetches now; an equal worth is not kept
inline bool Keeps(KeepAnswer const& answer, uint32_t sellNow)
{
    return answer.keeps && (answer.unsold || answer.laterWorth > sellNow);
}

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
        case BankReason::KeptStock:
            return "kept_stock";
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

struct DueTotal
{
    uint32_t count = 0;
    uint32_t worth = 0;
};

// The kept items due at the level (KeptEntry, BankKnowledge's KeptItem). Read at the reader's level, never stored: a
// level change makes an item due without any bank move
template <typename Kept>
inline DueTotal DueAt(std::vector<Kept> const& kept, uint8_t level)
{
    DueTotal due;
    for (Kept const& item : kept)
    {
        if (item.dueLevel > level)
            continue;
        due.count += item.count;
        due.worth += item.laterWorth;
    }
    return due;
}

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

// Why a kept item that is not due stays in the bank or goes there: stock for later, else a later level
inline BankReason WaitReason(KeepAnswer const& answer)
{
    return answer.dueLevel >= STOCK_LATER_LEVEL ? BankReason::KeptStock : BankReason::KeptForLevel;
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
            MakeVerdict(*decided.item, decided.answer, true, BankChoice::Leave, WaitReason(decided.answer)));
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
                MakeVerdict(*decided.item, decided.answer, false, BankChoice::Deposit, WaitReason(decided.answer)));
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

// Item use WI9b: one entry of demand's use stock. worthEach: one unit's worth to the bot; carryUnits: the units the
// bags carry (demand's kept units over bags and bank); slot: the demand slot whose share bounds its stock for later
struct StockEntry
{
    uint32_t entry = 0;
    uint32_t worthEach = 0;
    uint32_t carryUnits = 0;
    uint8_t slot = 0;
};

// Marks demand's use stock on the stacks of the bags and the bank. Per entry (entries with carried units first, then
// the higher worth, then the entry) its stacks in the bags first, the larger first, then the bank's: a stack is
// carried while the carried units are below carryUnits, else stock for later while its slot's later units are below
// laterBySlot (BANK_STOCK_SHARE wanted stocks), else neither (sold as surplus, or kept in the bags by demand). The
// stacks' order in the input never changes the marks
inline void AssignStock(std::span<StockEntry const> entries, std::span<uint32_t const> laterBySlot,
                        std::vector<BankItemIn>& bags, std::vector<BankItemIn>& bank)
{
    std::vector<StockEntry> order(entries.begin(), entries.end());
    std::sort(order.begin(), order.end(),
              [](StockEntry const& a, StockEntry const& b)
              {
                  if ((a.carryUnits > 0) != (b.carryUnits > 0))
                      return a.carryUnits > 0;
                  if (a.worthEach != b.worthEach)
                      return a.worthEach > b.worthEach;
                  return a.entry < b.entry;
              });
    std::vector<uint32_t> laterLeft(laterBySlot.begin(), laterBySlot.end());

    struct Stack
    {
        BankItemIn* item = nullptr;
        bool inBank = false;
    };
    for (StockEntry const& entry : order)
    {
        std::vector<Stack> stacks;
        for (BankItemIn& item : bags)
            if (item.item.entry == entry.entry)
                stacks.push_back({&item, false});
        for (BankItemIn& item : bank)
            if (item.item.entry == entry.entry)
                stacks.push_back({&item, true});
        std::sort(stacks.begin(), stacks.end(),
                  [](Stack const& a, Stack const& b)
                  {
                      if (a.inBank != b.inBank)
                          return !a.inBank;
                      if (a.item->item.count != b.item->item.count)
                          return a.item->item.count > b.item->item.count;
                      if (a.item->bag != b.item->bag)
                          return a.item->bag < b.item->bag;
                      if (a.item->slot != b.item->slot)
                          return a.item->slot < b.item->slot;
                      return a.item->item.itemGuid < b.item->item.itemGuid;
                  });

        uint32_t carried = 0;
        uint32_t* later = entry.slot < laterLeft.size() ? &laterLeft[entry.slot] : nullptr;
        for (Stack const& stack : stacks)
        {
            KeepItemIn& item = stack.item->item;
            item.stockWorthEach = entry.worthEach;
            item.stockKept = entry.carryUnits > 0;
            item.stockCarried = false;
            item.stockLater = false;
            if (carried < entry.carryUnits)
            {
                item.stockCarried = true;
                carried += item.count;
            }
            else if (later && *later > 0)
            {
                item.stockLater = true;
                *later -= std::min(*later, item.count);
            }
        }
    }
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

// Kept and not due yet: a due item is gear the bot may wear now, which the item usage labels as an upgrade first
inline CarriedKeeps KeptInBags(BankIn const& in)
{
    using namespace BankRulesDetail;
    std::vector<Decided> kept;
    for (BankItemIn const& item : in.bags)
    {
        KeepAnswer const answer = DecideKeep(item.item, in.bot);
        if (answer.keeps && !IsDue(answer, in.bot))
            kept.push_back({&item, answer});
    }
    SortByLaterWorth(kept);

    CarriedKeeps carried;
    carried.level = in.bot.level;
    for (Decided const& decided : kept)
        carried.items.push_back({decided.item->item.entry, decided.item->item.count, decided.answer.laterWorth,
                                 decided.answer.dueLevel, decided.answer.provider});
    return carried;
}

// The later worth of one carried item of the entry (at least 1), read at the bot's level now; 0 when no item of the
// entry is kept or the level is not the one they were decided at. A loop over the few kept items, no keep decision
inline uint32_t KeptWorthEach(CarriedKeeps const& carried, uint32_t entry, uint8_t level)
{
    if (!carried.level || carried.level != level)
        return 0;
    for (KeptEntry const& item : carried.items)
        if (item.entry == entry)
            return std::max<uint32_t>(item.laterWorth / std::max<uint32_t>(item.count, 1), 1);
    return 0;
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
