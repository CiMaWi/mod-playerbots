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
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "BankMailTypes.h"
#include "ItemWorthRules.h"
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
//   SpaceAnswer DecideBankSpace(BankIn const& in, BankPlan const& plan);   // a bank bag slot and its bag (WR17)
// BankRulesInput.h builds BankIn from the live bags and bank (BuildBankIn) and refreshes the bot's BankKnowledge.
namespace BankMail
{
// The bank's item slots (BANK_SLOT_ITEM_START..END)
constexpr uint32_t BANK_ITEM_SLOTS = 28;
// Professions WR17: the engine's bank bag slots (BANK_SLOT_BAG_START..END); BankBagSlotPrices.dbc has rows beyond
// them (8-12 at 999999999 copper), so the rule stops here, not the price table
constexpr uint32_t BANK_BAG_SLOTS = 7;
// The levels ahead a kept item may be due at, until the config supplies one (BankMail.KeepLevelHorizon)
constexpr uint8_t DEFAULT_KEEP_LEVEL_HORIZON = 5;
// A later item counts as an upgrade when its score beats the worn one's times this (Playerbots.EquipUpgradeThreshold)
constexpr float DEFAULT_UPGRADE_THRESHOLD = 1.1f;
// Item use WI9b: the bank keeps up to this many more wanted stocks of a demand slot as stock for later [E]
constexpr float BANK_STOCK_SHARE = 1.0f;
// The due level of stock for later: no level makes it due, so it makes no bank trip (ErrandWaits, DueAt)
constexpr uint8_t STOCK_LATER_LEVEL = 255;
// Professions WR12: a recipe item is kept while the skill it needs lies at most this far above the bot's reference
// skill (its skill now, at least its level times SKILL_PER_LEVEL); the skill path's nearness falls to zero there
constexpr uint16_t RECIPE_KEEP_REACH = 75;
constexpr uint16_t SKILL_PER_LEVEL = 5;

// One profession skill of the bot and its value now
struct SkillRank
{
    uint16_t skill = 0;
    uint16_t value = 0;
};

// Who answers whether the bot wears a gear item (CS29b): the item usage (ItemUpgradeValue, the equip action's answer)
// for an item of the bot's level or below, the bank's prediction for a later level
enum class WearAnswer : uint8_t
{
    Predicted,
    ItemUsage
};

// The item usage's answers the bank reads (ItemUsage in ItemUsageValue.h; BankRulesInput.cpp asserts the numbers)
constexpr uint8_t USAGE_EQUIP = 1;
constexpr uint8_t USAGE_REPLACE = 2;
constexpr uint8_t USAGE_BAD_EQUIP = 3;

// The equip action wears an item the usage answers EQUIP, REPLACE or BAD_EQUIP for (EquipAction.cpp)
inline bool WornByUsage(uint8_t usage)
{
    return usage == USAGE_EQUIP || usage == USAGE_REPLACE || usage == USAGE_BAD_EQUIP;
}

struct KeepItemIn
{
    uint32_t itemGuid = 0;  // the item's low guid
    uint32_t entry = 0;
    uint32_t count = 1;
    uint32_t stackMax = 1;
    uint8_t requiredLevel = 0;
    uint32_t sellNow = 0;   // what selling it fetches now (WT's worth for a vendor), finite
    uint32_t useWorth = 0;  // what it is worth to a bot that uses it (WT's Use worth), finite
    // The prediction (WearAnswer::Predicted): the bot's class wears its kind at the item's level (weapon, armor type,
    // class, race), its stat score for this bot, the score of the worse item worn in the slots it takes, and whether
    // one of those slots is empty
    bool wearable = false;
    float score = 0.0f;
    float wornScore = 0.0f;
    bool slotEmpty = false;
    // The item usage (WearAnswer::ItemUsage): its answer for the item now (ItemUsage's number), read instead of the
    // prediction's fields
    WearAnswer wearAnswer = WearAnswer::Predicted;
    uint8_t usage = 0;
    // Item use WI9b, demand's use stock (AssignStock): one unit's worth to the bot (0: not demand's or unusable),
    // whether this stack is part of what the bags carry, or stock for later, and whether demand keeps its entry (a kept
    // entry is never sold, whatever its worth)
    uint32_t stockWorthEach = 0;
    bool stockCarried = false;
    bool stockLater = false;
    bool stockKept = false;
    // Professions WR12: a material the bot's skill path uses (SkillPath::Material): its units, one unit's worth, the
    // profession and the skill of the first step that uses it (0: now)
    uint32_t materialUsed = 0;
    uint32_t materialCopperEach = 0;
    uint16_t materialSkill = 0;
    uint16_t materialDueSkill = 0;
    // A recipe item: the profession it teaches (0: none), the skill it needs, and whether the bot knows its recipe
    uint16_t recipeSkill = 0;
    uint16_t recipeRank = 0;
    bool recipeKnown = false;
};

struct KeepBotIn
{
    uint8_t level = 1;
    uint8_t horizon = DEFAULT_KEEP_LEVEL_HORIZON;
    float upgradeThreshold = DEFAULT_UPGRADE_THRESHOLD;
    std::vector<SkillRank> skills;  // the bot's profession skills now (professions WR12)
};

// The bot's value of a skill now; 0 when it does not have it
inline uint16_t SkillOf(KeepBotIn const& bot, uint16_t skill)
{
    for (SkillRank const& rank : bot.skills)
        if (rank.skill == skill)
            return rank.value;
    return 0;
}

// A provider's answer; a provider that does not keep the item returns the default
struct KeepAnswer
{
    bool keeps = false;
    uint32_t laterWorth = 0;  // copper the item is worth to the bot at dueLevel
    uint8_t dueLevel = 0;     // the level it is taken out at; at or below the bot's level it is due now
    char const* provider = "";
    // Not for sale: demand keeps the item's entry (item use WI9b), so the bank weighs it against the bags, not a sale
    bool unsold = false;
    // Professions WR12: due only once the bot's value of skill reaches dueSkill (0: no skill condition)
    uint16_t skill = 0;
    uint16_t dueSkill = 0;
};

using KeepProvider = KeepAnswer (*)(KeepItemIn const& item, KeepBotIn const& bot);

// Gear for a later level (WB): an item the bot's class wears, at most horizon levels above the bot, that beats what
// it wears now. Its later worth is WT's worth of an upgrade (the Use worth); it is due at its required level. An item
// of the bot's level or below takes the item usage's answer (CS29b): kept and due exactly when the equip action would
// wear it, with no score test, so a due item the usage would not wear is released like any kept item whose reason
// ended.
inline KeepAnswer GearForLaterLevel(KeepItemIn const& item, KeepBotIn const& bot)
{
    if (item.wearAnswer == WearAnswer::ItemUsage)
    {
        if (!WornByUsage(item.usage))
            return {};
        return {true, item.useWorth, std::max(item.requiredLevel, uint8_t{1}), "gear_for_level"};
    }
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

// A recipe item the bot cannot learn yet (professions WR12): kept at its use worth while the skill it needs lies within
// RECIPE_KEEP_REACH of the bot's reference skill; due once the skill (and the item's level) is reached. A recipe the
// bot knows, or of a profession it does not have, is not kept
inline KeepAnswer RecipeForLater(KeepItemIn const& item, KeepBotIn const& bot)
{
    uint16_t const now = SkillOf(bot, item.recipeSkill);
    if (item.recipeKnown || !now)
        return {};
    uint32_t const reference = std::max<uint32_t>(now, static_cast<uint32_t>(bot.level) * SKILL_PER_LEVEL);
    if (item.recipeRank > reference + RECIPE_KEEP_REACH)
        return {};

    uint8_t const dueLevel = now >= item.recipeRank ? std::max(item.requiredLevel, uint8_t{1}) : STOCK_LATER_LEVEL;
    return {true, item.useWorth, dueLevel, "materials", false, item.recipeSkill, item.recipeRank};
}

// A material the bot's skill path uses (professions WR12, WR.6): never sold, worth its cost by its cheapest source for
// the units the path uses; due now, or once the skill reaches the step that uses it (banked until then, no trip of its
// own: not due, it has no level)
inline KeepAnswer PathMaterial(KeepItemIn const& item, KeepBotIn const& bot)
{
    if (!item.materialUsed)
        return {};

    uint64_t const worth =
        static_cast<uint64_t>(item.materialCopperEach) * std::min(std::max<uint32_t>(item.count, 1), item.materialUsed);
    uint32_t const laterWorth = static_cast<uint32_t>(std::min<uint64_t>(worth, std::numeric_limits<uint32_t>::max()));
    uint8_t const dueLevel =
        SkillOf(bot, item.materialSkill) >= item.materialDueSkill ? std::max(bot.level, uint8_t{1}) : STOCK_LATER_LEVEL;
    return {true, laterWorth, dueLevel, "materials", true, item.materialSkill, item.materialDueSkill};
}

// Professions WR12: the materials and recipes of the bot's professions
inline KeepAnswer ProfessionMaterials(KeepItemIn const& item, KeepBotIn const& bot)
{
    return item.recipeSkill ? RecipeForLater(item, bot) : PathMaterial(item, bot);
}

// Every provider, asked in this order
inline constexpr KeepProvider KEEP_PROVIDERS[] = {GearForLaterLevel, StockForLater, ProfessionMaterials};

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
    return answer.keeps && answer.dueLevel <= bot.level && SkillOf(bot, answer.skill) >= answer.dueSkill;
}

// An item where it lies now: bag is the core's bag id (INVENTORY_SLOT_BAG_0 for the backpack and the bank's item
// slots, else the bag's equipment slot), slot its slot in that bag
struct BankItemIn
{
    KeepItemIn item;
    uint8_t bag = 0;
    uint8_t slot = 0;
};

// Professions WR17: a plain bag the bot carries (not worn), the bag a bank bag slot gets
struct CarriedBag
{
    uint8_t bag = 0;
    uint8_t slot = 0;
    uint32_t itemGuid = 0;
    uint32_t entry = 0;
    uint32_t slots = 0;    // 0: no bag carried
    uint32_t sellNow = 0;  // what it fetches now: what the bot gives up for it
};

// Professions WR17: the bank bag slots and the money the bank-space rule reads
struct BankSpaceIn
{
    uint32_t bought = 0;     // bank bag slots bought (Player::GetBankBagSlotCount)
    uint32_t filled = 0;     // bought slots that hold a bag
    uint32_t nextPrice = 0;  // the next slot's price (BankBagSlotPrices.dbc); 0: none left
    CarriedBag bag;          // the carried plain bag with the most slots
    uint64_t gold = 0;
    uint64_t reserve = 0;  // ShouldBuyBag's reserve: what the bot soon needs
    uint32_t thriftPercent = 100;
    uint32_t budgetPercent = 100;
};

struct BankIn
{
    KeepBotIn bot;
    std::vector<BankItemIn> bags;  // the backpack and the plain bags
    std::vector<BankItemIn> bank;  // the bank's 28 item slots and its bags' items (bag: the bank bag slot)
    BagSpace bagSpace;             // free bag slots and room on partial stacks there
    BagSpace bankSpace;            // free bank slots (item slots, plain bank bags) and room on partial stacks there
    uint32_t bankSlots = BANK_ITEM_SLOTS;  // the item slots and the plain bank bags' slots
    BankSpaceIn space;
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
        case BankReason::KeptForSkill:
            return "kept_for_skill";
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
    if (answer.dueSkill && answer.dueLevel >= STOCK_LATER_LEVEL)
        return BankReason::KeptForSkill;
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
// the higher worth, then the entry): its stacks in the bags first, the larger first, then the bank's, are carried
// while the carried units are below carryUnits; of the rest, the bank's stacks first, then the bags' (the larger
// first), are stock for later while its slot's later units are below laterBySlot (BANK_STOCK_SHARE wanted stocks),
// so a banked stack stays and the next visit moves nothing; else neither (sold as surplus, or kept in the bags by
// demand). Only an entry demand carries units of (carryUnits above 0, what the bot still uses) has stock for later
// (NE8b): an entry demand keeps none of is surplus, so each slot's later budget (at most wanted x share, and wanted is
// capped by NE8a) has no ranked competition and nothing is banked just to be released. The stacks' order in the input
// never changes the marks
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
        for (Stack const& stack : stacks)
        {
            KeepItemIn& item = stack.item->item;
            item.stockWorthEach = entry.worthEach;
            item.stockKept = entry.carryUnits > 0;
            item.stockCarried = carried < entry.carryUnits;
            item.stockLater = false;
            if (item.stockCarried)
                carried += item.count;
        }
        uint32_t* later = entry.carryUnits > 0 && entry.slot < laterLeft.size() ? &laterLeft[entry.slot] : nullptr;
        for (int pass = 0; pass < 2 && later; ++pass)
            for (Stack const& stack : stacks)
            {
                KeepItemIn& item = stack.item->item;
                if (stack.inBank != (pass == 0) || item.stockCarried || !*later)
                    continue;
                item.stockLater = true;
                *later -= std::min(*later, item.count);
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

inline char const* SpaceChoiceWord(SpaceChoice choice)
{
    switch (choice)
    {
        case SpaceChoice::BuySlot:
            return "buy_slot";
        case SpaceChoice::PlaceBag:
            return "place_bag";
        case SpaceChoice::NoSlot:
            return "no_slot";
        default:
            return "none";
    }
}

inline char const* SpaceReasonWord(SpaceReason reason)
{
    switch (reason)
    {
        case SpaceReason::Worth:
            return "worth";
        case SpaceReason::Dear:
            return "dear";
        case SpaceReason::Money:
            return "money";
        case SpaceReason::NoBag:
            return "no_bag";
        case SpaceReason::AllBought:
            return "all_bought";
        case SpaceReason::Refused:
            return "refused";
        default:
            return "fits";
    }
}

// Professions WR17 (PLAN 3.13): room in the bank for what the bot keeps. What does not fit is the plan's kept bag items
// left in the bags for a full bank (bank_full); the carried bag would hold the most valuable of them, and their later
// worth is what a slot with that bag is worth. A bought slot without a bag gets the bag when that worth reaches the
// bag's sale (the bag is the bot's own: no money is spent). Else the next slot is bought when the worth reaches the
// slot's price and the bag's sale together and the money above the reserve allows it (ShouldBuyBag, the bag-buy rule
// travel uses). Never beyond the engine's BANK_BAG_SLOTS; without a carried bag nothing is bought
inline SpaceAnswer DecideBankSpace(BankIn const& in, BankPlan const& plan)
{
    SpaceAnswer answer;
    std::vector<uint32_t> worths;
    for (BankVerdict const& verdict : plan.verdicts)
        if (!verdict.inBank && verdict.choice == BankChoice::Keep && verdict.reason == BankReason::BankFull)
            worths.push_back(verdict.laterWorth);
    answer.overflow = static_cast<uint32_t>(worths.size());
    if (worths.empty())
        return answer;

    BankSpaceIn const& space = in.space;
    answer.choice = SpaceChoice::NoSlot;
    answer.slots = space.bought;
    if (!space.bag.slots)
    {
        answer.reason = SpaceReason::NoBag;
        return answer;
    }
    answer.bagEntry = space.bag.entry;

    std::sort(worths.begin(), worths.end(), std::greater<uint32_t>());
    size_t const held = std::min<size_t>(worths.size(), space.bag.slots);
    uint64_t worth = 0;
    for (size_t i = 0; i < held; ++i)
        worth += worths[i];
    answer.worth = static_cast<uint32_t>(std::min<uint64_t>(worth, std::numeric_limits<uint32_t>::max()));

    bool const open = space.filled < space.bought;
    if (!open && (space.bought >= BANK_BAG_SLOTS || !space.nextPrice))
    {
        answer.reason = SpaceReason::AllBought;
        return answer;
    }
    uint64_t const price = static_cast<uint64_t>(space.bag.sellNow) + (open ? 0 : space.nextPrice);
    answer.price = static_cast<uint32_t>(std::min<uint64_t>(price, std::numeric_limits<uint32_t>::max()));
    if (!answer.worth || price > answer.worth)
    {
        answer.reason = SpaceReason::Dear;
        return answer;
    }
    if (open)
    {
        answer.choice = SpaceChoice::PlaceBag;
        answer.reason = SpaceReason::Worth;
        return answer;
    }

    WorldTravel::BagBuyInput buy;
    buy.Price = answer.price;
    buy.Worth = answer.worth;
    buy.Gold = space.gold;
    buy.Reserve = space.reserve;
    buy.ThriftPercent = space.thriftPercent;
    buy.BudgetPercent = space.budgetPercent;
    if (!WorldTravel::ShouldBuyBag(buy))
    {
        answer.reason = SpaceReason::Money;
        return answer;
    }
    answer.choice = SpaceChoice::BuySlot;
    answer.reason = SpaceReason::Worth;
    return answer;
}

inline std::string DescribeSpace(SpaceAnswer const& answer)
{
    return std::string(SpaceChoiceWord(answer.choice)) + " " + SpaceReasonWord(answer.reason) + ": " +
           std::to_string(answer.overflow) + " left out, worth " + std::to_string(answer.worth) + ", price " +
           std::to_string(answer.price) + ", bag " + std::to_string(answer.bagEntry) + ", slots bought " +
           std::to_string(answer.slots);
}
}  // namespace BankMail

#endif
