/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKRULESSELFTEST_H
#define _PLAYERBOT_BANKRULESSELFTEST_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "AuctionPricing.h"
#include "BankRules.h"
#include "ItemWorthRules.h"

// The scenarios of the bank rules (BankRules.h), run by tests/bank-rules-selftest.cpp.
// Each scenario adds its decisions to lines; a failed check adds a "FAIL:" line.
namespace BankMail
{
namespace BankSelftestDetail
{
// The core's bag id of the backpack and the bank's item slots, and the first slot of each
constexpr uint8_t MAIN_BAG = 255;
constexpr uint8_t BACKPACK_SLOT = 23;
constexpr uint8_t BANK_SLOT = 39;
constexpr uint32_t SELL = 100;
constexpr uint32_t USE = 400;
constexpr float BETTER = 50.0f;
constexpr float WORN = 30.0f;
constexpr uint32_t STACK_20 = 20;
constexpr uint32_t CARRIED_SEARCH_SEED = 20261113;

struct Result
{
    uint32_t checks = 0;
    uint32_t failed = 0;
};

class Checker
{
public:
    Checker(std::vector<std::string>& lines, Result& result) : _lines(lines), _result(result) {}

    void Check(bool ok, std::string const& what)
    {
        ++_result.checks;
        if (ok)
            return;
        ++_result.failed;
        _lines.push_back("FAIL: " + what);
    }

    void Show(std::string const& scenario, BankPlan const& plan)
    {
        _lines.push_back(scenario + ": " + DescribePlan(plan));
        for (BankVerdict const& verdict : plan.verdicts)
            _lines.push_back("  " + DescribeVerdict(verdict));
    }

    void Show(std::string const& what, KeepAnswer const& answer)
    {
        _lines.push_back(what + ": " +
                         (answer.keeps ? "kept, later " + std::to_string(answer.laterWorth) + " at level " +
                                             std::to_string(answer.dueLevel) + " (" + answer.provider + ")"
                                       : std::string("not kept")));
    }

    void Note(std::string const& line) { _lines.push_back(line); }

private:
    std::vector<std::string>& _lines;
    Result& _result;
};

inline KeepItemIn Gear(uint32_t entry, uint8_t requiredLevel, float score = BETTER, float wornScore = WORN,
                       uint32_t useWorth = USE, uint32_t sellNow = SELL)
{
    KeepItemIn item;
    item.itemGuid = entry;
    item.entry = entry;
    item.requiredLevel = requiredLevel;
    item.sellNow = sellNow;
    item.useWorth = useWorth;
    item.wearable = true;
    item.score = score;
    item.wornScore = wornScore;
    return item;
}

inline KeepBotIn Bot(uint8_t level)
{
    KeepBotIn bot;
    bot.level = level;
    return bot;
}

inline BagSpace Space(uint32_t freeSlots) { return BagSpace{freeSlots, {}}; }

// Items in the backpack and in the bank, in consecutive slots from the first
inline BankIn MakeIn(uint8_t level, std::vector<KeepItemIn> const& bags, std::vector<KeepItemIn> const& bank,
                     uint32_t bagFree, uint32_t bankFree)
{
    BankIn in;
    in.bot = Bot(level);
    for (size_t i = 0; i < bags.size(); ++i)
        in.bags.push_back({bags[i], MAIN_BAG, static_cast<uint8_t>(BACKPACK_SLOT + i)});
    for (size_t i = 0; i < bank.size(); ++i)
        in.bank.push_back({bank[i], MAIN_BAG, static_cast<uint8_t>(BANK_SLOT + i)});
    in.bagSpace = Space(bagFree);
    in.bankSpace = Space(bankFree);
    return in;
}

inline bool Moves(BankPlan const& plan, std::vector<std::pair<BankChoice, uint32_t>> const& expected)
{
    if (plan.steps.size() != expected.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i)
        if (plan.steps[i].kind != expected[i].first || plan.steps[i].entry != expected[i].second)
            return false;
    return true;
}

inline BankVerdict const* VerdictFor(BankPlan const& plan, uint32_t entry)
{
    auto verdict = std::find_if(plan.verdicts.begin(), plan.verdicts.end(),
                                [entry](BankVerdict const& candidate) { return candidate.entry == entry; });
    return verdict == plan.verdicts.end() ? nullptr : &*verdict;
}

inline bool Is(BankPlan const& plan, uint32_t entry, BankChoice choice, BankReason reason)
{
    BankVerdict const* verdict = VerdictFor(plan, entry);
    return verdict && verdict->choice == choice && verdict->reason == reason;
}

inline void CheckHorizon(Checker& checker)
{
    KeepBotIn const bot = Bot(10);
    KeepAnswer const near = DecideKeep(Gear(101, 12), bot);
    KeepAnswer const far = DecideKeep(Gear(102, 30), bot);
    KeepAnswer const edge = DecideKeep(Gear(103, 15), bot);
    KeepAnswer const beyond = DecideKeep(Gear(104, 16), bot);
    checker.Show("level 10, an upgrade for level 12", near);
    checker.Show("level 10, an upgrade for level 30", far);
    checker.Show("level 10, an upgrade for level 15 (5 levels in)", edge);
    checker.Show("level 10, an upgrade for level 16 (6 levels in)", beyond);
    checker.Check(near.keeps && near.dueLevel == 12 && near.laterWorth == USE, "two levels away is kept, due at 12");
    checker.Check(!far.keeps, "twenty levels away is not kept");
    checker.Check(edge.keeps && !beyond.keeps, "the horizon keeps 5 levels ahead, not 6");

    std::vector<KeepItemIn> bags = {Gear(103, 15), Gear(102, 30), Gear(101, 12), Gear(104, 16)};
    BankPlan const plan = PlanBankVisit(MakeIn(10, bags, {}, 0, BANK_ITEM_SLOTS));
    checker.Show("level 10 at a banker with those four", plan);
    checker.Check(Moves(plan, {{BankChoice::Deposit, 101}, {BankChoice::Deposit, 103}}) && plan.verdicts.size() == 2,
                  "the two kept are deposited, equal worth by entry; the others are no business of the bank");
    std::reverse(bags.begin(), bags.end());
    BankPlan const reversed = PlanBankVisit(MakeIn(10, bags, {}, 0, BANK_ITEM_SLOTS));
    checker.Check(Moves(reversed, {{BankChoice::Deposit, 101}, {BankChoice::Deposit, 103}}),
                  "the order does not depend on the bags' order");
    checker.Check(plan.bankFreeAfter == BANK_ITEM_SLOTS - 2, "two bank slots used");
}

inline void CheckUpgrade(Checker& checker)
{
    KeepBotIn const bot = Bot(10);
    KeepAnswer const slight = DecideKeep(Gear(111, 12, 32.0f), bot);
    KeepAnswer const clear = DecideKeep(Gear(112, 12, 34.0f), bot);
    KeepItemIn empty = Gear(113, 12, 5.0f, 0.0f);
    empty.slotEmpty = true;
    KeepItemIn nothing = Gear(114, 12, 0.0f, 0.0f);
    nothing.slotEmpty = true;
    KeepItemIn cannot = Gear(115, 12);
    cannot.wearable = false;
    checker.Show("score 32 over worn 30 (threshold 1.1)", slight);
    checker.Show("score 34 over worn 30", clear);
    checker.Check(!slight.keeps && clear.keeps, "an upgrade must beat the worn score times the threshold");
    checker.Check(DecideKeep(empty, bot).keeps, "an empty slot: any score above 0 is an upgrade");
    checker.Check(!DecideKeep(nothing, bot).keeps, "an item that scores 0 is never kept");
    checker.Check(!DecideKeep(cannot, bot).keeps, "an item the class does not wear is not kept");
}

inline void CheckRule(Checker& checker)
{
    KeepAnswer const answer{true, SELL, 12, "gear_for_level"};
    checker.Check(!Keeps(answer, SELL), "later worth equal to the sell worth: not kept");
    checker.Check(Keeps({true, SELL + 1, 12, "gear_for_level"}, SELL), "one copper more: kept");
    checker.Check(Keeps({true, 1, 12, "gear_for_level"}, 0), "an unsellable item with any later worth: kept");
    checker.Check(Keeps({true, 1, 12, "stock", true}, SELL), "an unsold item (demand keeps it): kept below its sale");
    checker.Check(!Keeps({false, USE, 12, ""}, 0), "a provider that does not keep is never kept");
    KeepAnswer const cheap = DecideKeep(Gear(121, 12, BETTER, WORN, SELL, SELL), Bot(10));
    checker.Show("an upgrade whose use worth equals its sell worth", cheap);
    checker.Check(!cheap.keeps, "not worth more later: sold like today");
}

inline KeepAnswer CheapProvider(KeepItemIn const&, KeepBotIn const&) { return {true, 300, 14, "cheap"}; }
inline KeepAnswer DearProvider(KeepItemIn const&, KeepBotIn const&) { return {true, 500, 18, "dear"}; }
inline KeepAnswer EqualProvider(KeepItemIn const&, KeepBotIn const&) { return {true, 500, 20, "equal"}; }
inline KeepAnswer NeverProvider(KeepItemIn const&, KeepBotIn const&) { return {}; }

inline void CheckProviders(Checker& checker)
{
    KeepProvider const providers[] = {CheapProvider, NeverProvider, DearProvider, EqualProvider};
    KeepAnswer const best = DecideKeep(Gear(131, 12), Bot(10), providers);
    checker.Show("four providers (300 at 14, none, 500 at 18, 500 at 20)", best);
    checker.Check(best.keeps && best.laterWorth == 500 && best.dueLevel == 18 && std::string(best.provider) == "dear",
                  "the best later worth wins with its due level; the first provider on a tie");
    KeepProvider const onlyCheap[] = {CheapProvider};
    checker.Check(!DecideKeep(Gear(132, 12, BETTER, WORN, USE, 400), Bot(10), onlyCheap).keeps,
                  "an answer below the sell worth fails the rule");
    checker.Check(std::size(KEEP_PROVIDERS) == 4 && KEEP_PROVIDERS[0] == GearForLaterLevel &&
                      KEEP_PROVIDERS[1] == StockForLater && KEEP_PROVIDERS[2] == ProfessionMaterials &&
                      KEEP_PROVIDERS[3] == AuctionStock,
                  "the server asks four providers: gear for a later level, use stock, profession materials, then "
                  "auction stock");
}

inline void CheckDue(Checker& checker)
{
    BankPlan const due = PlanBankVisit(MakeIn(12, {Gear(202, 12)}, {Gear(201, 12)}, 4, 27));
    checker.Show("level 12, kept for 12 in the bank and in the bags", due);
    checker.Check(Moves(due, {{BankChoice::Withdraw, 201}}), "a kept item is withdrawn at its level");
    checker.Check(
        Is(due, 201, BankChoice::Withdraw, BankReason::Due) && Is(due, 202, BankChoice::Keep, BankReason::Due),
        "due in the bank: withdraw due; due in the bags: keep due (the equip action wears it)");
    BankPlan const early = PlanBankVisit(MakeIn(11, {}, {Gear(201, 12)}, 4, 27));
    checker.Show("level 11, kept for 12 in the bank", early);
    checker.Check(early.steps.empty() && Is(early, 201, BankChoice::Leave, BankReason::KeptForLevel),
                  "one level early it stays");
    std::vector<KeptEntry> const kept = KeptInBank(MakeIn(12, {}, {Gear(201, 12)}, 4, 27));
    checker.Check(kept.size() == 1 && kept[0].dueLevel == 12 && kept[0].laterWorth == USE,
                  "the kept list holds the due item until it leaves the bank");
}

// CS29b: gear of the bot's level or below as BankRulesInput.cpp reads it, with the item usage's answer; the
// prediction's fields are set to values that would refuse it, so only the usage can keep it
inline KeepItemIn Due(uint32_t entry, uint8_t requiredLevel, uint8_t usage, uint32_t useWorth = USE)
{
    KeepItemIn item = Gear(entry, requiredLevel, 0.0f, WORN, useWorth);
    item.wearable = false;
    item.wearAnswer = WearAnswer::ItemUsage;
    item.usage = usage;
    return item;
}

inline void CheckDueUsage(Checker& checker)
{
    constexpr uint8_t USAGE_NONE = 0;
    constexpr uint8_t USAGE_BROKEN_EQUIP = 4;
    KeepBotIn const bot = Bot(12);
    KeepAnswer const equip = DecideKeep(Due(1001, 12, USAGE_EQUIP), bot);
    KeepAnswer const replace = DecideKeep(Due(1002, 10, USAGE_REPLACE), bot);
    KeepAnswer const bad = DecideKeep(Due(1003, 12, USAGE_BAD_EQUIP), bot);
    checker.Show("level 12, the usage answers equip for a level 12 item", equip);
    checker.Show("the usage answers bad equip", bad);
    checker.Check(equip.keeps && equip.dueLevel == 12 && replace.keeps && replace.dueLevel == 10 && bad.keeps &&
                      std::string(bad.provider) == "gear_for_level",
                  "the usage wears it (equip, replace, bad equip): kept, due now at its level");
    checker.Check(
        !DecideKeep(Due(1004, 12, USAGE_NONE), bot).keeps && !DecideKeep(Due(1005, 12, USAGE_BROKEN_EQUIP), bot).keeps,
        "the usage would not wear it (none, broken): not kept");
    KeepItemIn marginal = Due(1006, 12, USAGE_REPLACE);
    marginal.score = 31.0f;  // the prediction's threshold would refuse 31 over a worn 30
    KeepItemIn plain = Due(1007, 12, USAGE_BAD_EQUIP);
    plain.slotEmpty = true;  // score 0 in an empty slot: the prediction never keeps it
    checker.Check(DecideKeep(marginal, bot).keeps && DecideKeep(plain, bot).keeps,
                  "no threshold and no score test on a usage answer");
    KeepItemIn worn = Due(1008, 12, USAGE_NONE);
    worn.wearable = true;
    worn.score = BETTER;
    checker.Check(!DecideKeep(worn, bot).keeps, "a usage answer is not overruled by the prediction's fields");
    KeepAnswer const later = DecideKeep(Gear(1009, 14), bot);
    checker.Check(later.keeps && later.dueLevel == 14, "a later item is still predicted: kept, due at its level");

    std::vector<KeepItemIn> const bank = {Due(1001, 12, USAGE_EQUIP, 300), Due(1004, 12, USAGE_NONE), Gear(1009, 14),
                                          Due(1003, 11, USAGE_BAD_EQUIP, 900)};
    BankPlan const plan = PlanBankVisit(MakeIn(12, {Due(1010, 12, USAGE_NONE)}, bank, 4, 24));
    checker.Show("level 12: two due items the usage wears, one it does not, one for 14; a bag item it does not wear",
                 plan);
    checker.Check(
        Moves(plan, {{BankChoice::Withdraw, 1003}, {BankChoice::Withdraw, 1001}, {BankChoice::Withdraw, 1004}}),
        "due by later worth, then released; nothing deposited");
    checker.Check(Is(plan, 1003, BankChoice::Withdraw, BankReason::Due) &&
                      Is(plan, 1001, BankChoice::Withdraw, BankReason::Due) &&
                      Is(plan, 1004, BankChoice::Withdraw, BankReason::Released) &&
                      Is(plan, 1009, BankChoice::Leave, BankReason::KeptForLevel),
                  "a due item the usage would not wear is withdrawn as released, not as due gear");
    checker.Check(VerdictFor(plan, 1004) && VerdictFor(plan, 1004)->laterWorth == 0 &&
                      KeptInBank(MakeIn(12, {}, bank, 4, 24)).size() == 3,
                  "it has no later worth and is not in the kept list");
}

inline void CheckReleased(Checker& checker)
{
    KeepItemIn outlevelled = Gear(302, 12);
    outlevelled.wearable = false;
    BankPlan const plan =
        PlanBankVisit(MakeIn(11, {}, {Gear(301, 12, BETTER, 60.0f), outlevelled, Gear(303, 14)}, 4, 25));
    checker.Show("level 11 wears better than a kept item; another no longer fits its armor kind", plan);
    checker.Check(Is(plan, 301, BankChoice::Withdraw, BankReason::Released) &&
                      Is(plan, 302, BankChoice::Withdraw, BankReason::Released) &&
                      Is(plan, 303, BankChoice::Leave, BankReason::KeptForLevel),
                  "no longer worth more later: withdrawn as released; the one still kept stays");
    checker.Check(VerdictFor(plan, 301) && VerdictFor(plan, 301)->laterWorth == 0,
                  "a released item has no later worth");
    checker.Check(KeptInBank(MakeIn(11, {}, {Gear(301, 12, BETTER, 60.0f), Gear(303, 14)}, 4, 26)).size() == 1,
                  "a released item is not in the kept list");
    checker.Note("released items are only withdrawn: the existing sell action sells them as ordinary items");
}

inline void CheckBankFull(Checker& checker)
{
    BankPlan const plan = PlanBankVisit(MakeIn(10, {Gear(403, 13, BETTER, WORN, 300), Gear(402, 13, BETTER, WORN, 500)},
                                               {Gear(401, 14, BETTER, WORN, 200)}, 4, 1));
    checker.Show("one bank slot free, two kept items in the bags", plan);
    checker.Check(Moves(plan, {{BankChoice::Deposit, 402}}) && Is(plan, 403, BankChoice::Keep, BankReason::BankFull),
                  "the dearer goes in; the lowest-worth kept item stays in the bags");
    checker.Check(Is(plan, 401, BankChoice::Leave, BankReason::KeptForLevel),
                  "nothing banked is taken out to make room, even a cheaper one");
    checker.Check(plan.bankFreeAfter == 0 && plan.keptInBags == 1, "the bank is full after");
}

inline void CheckNothing(Checker& checker)
{
    KeepItemIn cloth = Gear(501, 0, 0.0f, 0.0f);
    cloth.wearable = false;
    BankPlan const plan = PlanBankVisit(MakeIn(10, {cloth, Gear(502, 8, 31.0f), Gear(503, 40)}, {}, 2, 28));
    checker.Show("nothing kept: cloth, a weak item, far gear", plan);
    checker.Check(plan.steps.empty() && plan.verdicts.empty() && plan.bankFreeAfter == BANK_ITEM_SLOTS,
                  "nothing kept means nothing to do");
    BankPlan const none = PlanBankVisit(BankIn{});
    checker.Show("empty input", none);
    checker.Check(none.steps.empty() && none.verdicts.empty() && none.bankFreeAfter == 0, "empty input plans nothing");
}

inline void CheckOrder(Checker& checker)
{
    std::vector<KeepItemIn> const bank = {Gear(601, 12, BETTER, WORN, 300), Gear(602, 12, BETTER, WORN, 900),
                                          Gear(603, 11, BETTER, WORN, 500), Gear(604, 12, BETTER, 60.0f, USE, 50)};
    BankPlan const room = PlanBankVisit(MakeIn(12, {}, bank, 4, 24));
    checker.Show("three due and one released, bag room for all", room);
    checker.Check(Moves(room, {{BankChoice::Withdraw, 602},
                               {BankChoice::Withdraw, 603},
                               {BankChoice::Withdraw, 601},
                               {BankChoice::Withdraw, 604}}),
                  "due by later worth, then released");
    BankPlan const tight = PlanBankVisit(MakeIn(12, {}, bank, 2, 24));
    checker.Show("the same with two free bag slots", tight);
    checker.Check(Moves(tight, {{BankChoice::Withdraw, 602}, {BankChoice::Withdraw, 603}}) &&
                      Is(tight, 601, BankChoice::Leave, BankReason::NoSpace) &&
                      Is(tight, 604, BankChoice::Leave, BankReason::NoSpace),
                  "what does not fit the bags stays in the bank: no_space");
    checker.Check(tight.leftInBank == 2 && tight.bankFreeAfter == 26, "two withdrawn free two bank slots");
}

inline void CheckSwap(Checker& checker)
{
    std::vector<KeepItemIn> const bank = {Gear(701, 9, BETTER, 60.0f)};
    BankPlan const plan = PlanBankVisit(MakeIn(10, {Gear(702, 13)}, bank, 1, 0));
    checker.Show("a full bank with a released item, a kept item in the bags", plan);
    checker.Check(Moves(plan, {{BankChoice::Withdraw, 701}, {BankChoice::Deposit, 702}}),
                  "the withdrawal frees the slot the deposit takes, withdrawal first");
    BankPlan const stuck = PlanBankVisit(MakeIn(10, {Gear(702, 13)}, bank, 0, 0));
    checker.Show("the same with full bags", stuck);
    checker.Check(stuck.steps.empty() && Is(stuck, 701, BankChoice::Leave, BankReason::NoSpace) &&
                      Is(stuck, 702, BankChoice::Keep, BankReason::BankFull),
                  "no bag room: nothing is withdrawn and the deposit finds the bank full");
}

inline void CheckStacks(Checker& checker)
{
    KeepItemIn three = Gear(801, 13);
    three.count = 3;
    three.stackMax = STACK_20;
    BankIn in = MakeIn(10, {three}, {}, 0, 0);
    in.bankSpace.stackRoom.push_back({801, 5});
    BankPlan const merge = PlanBankVisit(in);
    checker.Show("a stack of 3 onto a bank stack with room 5, no free bank slot", merge);
    checker.Check(Moves(merge, {{BankChoice::Deposit, 801}}) && merge.bankFreeAfter == 0,
                  "a merge onto a partial stack costs no slot");
    in.bags[0].item.count = 6;
    checker.Check(Is(PlanBankVisit(in), 801, BankChoice::Keep, BankReason::BankFull),
                  "a stack larger than the room needs a free slot");

    KeepItemIn four = Gear(802, 10);
    four.count = 4;
    four.stackMax = STACK_20;
    BankIn back = MakeIn(10, {}, {four}, 0, 27);
    back.bagSpace.stackRoom.push_back({802, 10});
    checker.Check(Moves(PlanBankVisit(back), {{BankChoice::Withdraw, 802}}),
                  "a due stack is withdrawn onto a partial bag stack");
}

// A banker visit's move as the core makes it, for single items: the item leaves its slot for a free one on the other
// side (BankVisit.cpp plans again before every move)
inline void Apply(BankIn& in, BankStep const& step)
{
    bool const withdraw = step.kind == BankChoice::Withdraw;
    std::vector<BankItemIn>& from = withdraw ? in.bank : in.bags;
    std::vector<BankItemIn>& to = withdraw ? in.bags : in.bank;
    auto moved = std::find_if(from.begin(), from.end(),
                              [&step](BankItemIn const& item) { return item.item.itemGuid == step.itemGuid; });
    if (moved == from.end())
        return;
    BankItemIn item = *moved;
    from.erase(moved);
    item.slot = static_cast<uint8_t>((withdraw ? BACKPACK_SLOT : BANK_SLOT) + to.size());
    to.push_back(item);
    BagSpace& freed = withdraw ? in.bankSpace : in.bagSpace;
    BagSpace& taken = withdraw ? in.bagSpace : in.bankSpace;
    ++freed.freeSlots;
    --taken.freeSlots;
}

// The night's full banks (NR7): the visit plans again before each move. An item moved earlier on the visit that the
// rebuilt plan would move back (its keep changed with its place) stays where it lies: it frees no slot for the next
// deposit and takes none from it
inline void CheckSettled(Checker& checker)
{
    BankIn const deposited = MakeIn(10, {Gear(905, 13)}, {Gear(904, 10)}, 1, 0);
    BankPlan const full = PlanNextMove(deposited, {904}, {});
    checker.Show("a full bank after depositing 904, which the rebuilt plan would withdraw", full);
    checker.Check(full.steps.empty() && Is(full, 904, BankChoice::Leave, BankReason::Settled) &&
                      Is(full, 905, BankChoice::Keep, BankReason::BankFull) && full.bankFreeAfter == 0,
                  "a deposited item the rebuilt plan would take out frees no bank slot for the next deposit");

    BankIn const withdrawn = MakeIn(10, {Gear(906, 13), Gear(907, 14)}, {}, 0, 1);
    BankPlan const room = PlanNextMove(withdrawn, {906}, {});
    checker.Show("one bank slot after withdrawing 906, which the rebuilt plan would deposit", room);
    checker.Check(Moves(room, {{BankChoice::Deposit, 907}}) && Is(room, 906, BankChoice::Keep, BankReason::Settled) &&
                      room.bankFreeAfter == 0,
                  "a withdrawn item the rebuilt plan would deposit takes no bank slot from the next deposit");
}

// The visit's refused moves: the core said no to a move the rule allowed; the item stays and takes no room
inline void CheckRefused(Checker& checker)
{
    BankIn const in = MakeIn(10, {}, {Gear(901, 10), Gear(902, 10)}, 1, 26);
    BankPlan const first = PlanBankVisit(in);
    BankPlan const refused = PlanBankVisit(in, {901});
    checker.Show("two due items, one bag slot", first);
    checker.Show("the same after the core refused the first", refused);
    checker.Check(Moves(first, {{BankChoice::Withdraw, 901}}) && Is(first, 902, BankChoice::Leave, BankReason::NoSpace),
                  "one bag slot: the first by entry is withdrawn, the other has no room");
    checker.Check(
        Moves(refused, {{BankChoice::Withdraw, 902}}) && Is(refused, 901, BankChoice::Leave, BankReason::Settled),
        "a refused withdrawal stays in the bank and leaves its bag slot to the next");

    // R4/R5: a word per refusal code, each read by the night check's 'refused%'; the designed skips (settled, the
    // core's pre-test) never start with refused
    std::vector<std::string_view> words;
    for (BankReason reason :
         {BankReason::RefusedLooted, BankReason::RefusedNotOwned, BankReason::RefusedLimit, BankReason::RefusedFull,
          BankReason::RefusedBag, BankReason::RefusedInPlace, BankReason::RefusedOther})
        words.push_back(ReasonWord(reason));
    bool const named =
        std::all_of(words.begin(), words.end(), [](std::string_view word) { return word.starts_with("refused"); });
    size_t const refusals = words.size();
    for (BankReason reason : {BankReason::Settled, BankReason::BlockedLooted, BankReason::BlockedNotOwned,
                              BankReason::BlockedLimit, BankReason::BlockedOther})
        words.push_back(ReasonWord(reason));
    bool const skips = std::none_of(words.begin() + refusals, words.end(),
                                    [](std::string_view word) { return word.starts_with("refused"); });
    std::sort(words.begin(), words.end());
    checker.Check(named && skips && std::adjacent_find(words.begin(), words.end()) == words.end(),
                  "each refusal code has its own word starting with refused; no designed skip's word does");

    BankIn const deposit = MakeIn(10, {Gear(903, 13)}, {}, 0, BANK_ITEM_SLOTS);
    BankPlan const kept = PlanBankVisit(deposit, {903});
    checker.Show("a kept item whose deposit the core refused", kept);
    checker.Check(kept.steps.empty() && Is(kept, 903, BankChoice::Keep, BankReason::Settled) &&
                      kept.bankFreeAfter == BANK_ITEM_SLOTS,
                  "a refused deposit stays in the bags and takes no bank slot");

    // R6: the input asks the core's own deposit test; an item it refuses by its state (an opened box) gets no step
    KeepItemIn opened = Gear(908, 14);
    opened.depositBlock = BankReason::BlockedLooted;
    BankIn const blocked = MakeIn(10, {opened, Gear(909, 13)}, {}, 0, 1);
    BankPlan const left = PlanBankVisit(blocked);
    checker.Show("an item the core would refuse to bank and one it takes, one bank slot", left);
    checker.Check(Moves(left, {{BankChoice::Deposit, 909}}) &&
                      Is(left, 908, BankChoice::Keep, BankReason::BlockedLooted) && left.keptInBags == 1 &&
                      left.bankFreeAfter == 0,
                  "an item the core would refuse is kept with its word and leaves the bank slot to the next");
    CarriedKeeps const carried = KeptInBags(blocked);
    bool const marked = std::any_of(carried.items.begin(), carried.items.end(),
                                    [](KeptEntry const& item) { return item.entry == 908 && item.blocked; });
    checker.Check(marked, "it stays kept in the bags, marked so the trip counts no deposit for it");
}

// The visit plans again after every move (WB5 review note 2): the bag slot a deposit frees lets a due item out
inline void CheckReplan(Checker& checker)
{
    BankIn in = MakeIn(10, {Gear(905, 13)}, {Gear(904, 10)}, 0, 1);
    BankPlan const first = PlanBankVisit(in);
    checker.Show("full bags, one bank slot, a due item in the bank, a kept item in the bags", first);
    checker.Check(Moves(first, {{BankChoice::Deposit, 905}}) && Is(first, 904, BankChoice::Leave, BankReason::NoSpace),
                  "the first plan deposits; the due item has no bag room yet");
    Apply(in, first.steps.front());
    BankPlan const second = PlanBankVisit(in);
    checker.Show("planned again after the deposit", second);
    checker.Check(
        Moves(second, {{BankChoice::Withdraw, 904}}) && Is(second, 905, BankChoice::Leave, BankReason::KeptForLevel),
        "the freed bag slot takes the due item; the deposited one stays");
    Apply(in, second.steps.front());
    BankPlan const third = PlanBankVisit(in);
    checker.Check(third.steps.empty() && Is(third, 904, BankChoice::Keep, BankReason::Due),
                  "after both moves nothing is left to move: the visit ends");
}

// The due list after a level change (WB7b: `bank status` showed `known ... 0 due` while the live rule saw 1 due). The
// knowledge was built at the old level and its due count stored then; read at the new level it shows the item due
inline void CheckDueAfterLevelChange(Checker& checker)
{
    BankIn in = MakeIn(10, {}, {Gear(906, 11)}, 4, 27);
    std::vector<KeptEntry> const known = KeptInBank(in);
    DueTotal const storedAtBuild = DueAt(known, in.bot.level);
    in.bot.level = 11;
    DueTotal const live = DueAt(KeptInBank(in), in.bot.level);
    DueTotal const read = DueAt(known, in.bot.level);
    checker.Note("level 10 -> 11, a bank item due at 11: stored at the build " + std::to_string(storedAtBuild.count) +
                 " due, live " + std::to_string(live.count) + " due, the knowledge read at 11 " +
                 std::to_string(read.count) + " due");
    checker.Check(known.size() == 1 && storedAtBuild.count == 0, "kept at level 10 for level 11, not due yet");
    checker.Check(live.count == 1 && read.count == 1 && read.worth == live.worth,
                  "after the level change the knowledge read at the new level shows the due item");
}

// The item usage's view of the bags (WB9): the bags' items, the memo the bot keeps (KeptInBags) and what follows from
// it. Every test item here would sell today (vendor or auction usage); a carried one is ITEM_USAGE_BANK instead,
// which the sell action skips, and in the space rule it counts at its later worth
struct CarriedView
{
    BankIn in;
    CarriedKeeps carried;
};

inline CarriedView ViewOf(uint8_t level, std::vector<KeepItemIn> const& bags)
{
    CarriedView view;
    view.in = MakeIn(level, bags, {}, 0, BANK_ITEM_SLOTS);
    view.carried = KeptInBags(view.in);
    return view;
}

// What a vendor stop sells: the items the memo does not keep (their ordinary usage sells)
inline std::vector<uint32_t> SoldAtStop(CarriedView const& view)
{
    std::vector<uint32_t> sold;
    for (BankItemIn const& item : view.in.bags)
        if (!KeptWorthEach(view.carried, item.item.entry, view.in.bot.level))
            sold.push_back(item.item.entry);
    return sold;
}

inline bool Holds(std::vector<uint32_t> const& entries, uint32_t entry)
{
    return std::find(entries.begin(), entries.end(), entry) != entries.end();
}

// The space rule's worth of a bag item: its later worth when the memo keeps it, else what it sells for
inline uint32_t SpaceWorth(CarriedView const& view, KeepItemIn const& item)
{
    WorldTravel::ItemFacts facts;
    facts.LaterWorthEach = KeptWorthEach(view.carried, item.entry, view.in.bot.level);
    return WorldTravel::ItemWorth(WorldTravel::ClassifyForSpace(facts), item.sellNow, 0, item.count, 1.0f,
                                  facts.LaterWorthEach);
}

inline std::string DescribeStop(CarriedView const& view)
{
    std::string line = "carried at level " + std::to_string(view.carried.level) + ":";
    for (KeptEntry const& item : view.carried.items)
        line += " " + std::to_string(item.entry) + " (later " + std::to_string(item.laterWorth) + ")";
    line += "; sold at a vendor stop:";
    for (uint32_t entry : SoldAtStop(view))
        line += " " + std::to_string(entry);
    return line;
}

inline void CheckCarried(Checker& checker)
{
    KeepItemIn junk = Gear(1005, 0, 0.0f, 0.0f, 0, 50);
    junk.wearable = false;
    CarriedView const view = ViewOf(10, {Gear(1001, 13), Gear(1002, 10), Gear(1003, 13, 20.0f), Gear(1004, 16), junk});
    checker.Note(
        "level 10 bags: 1001 upgrade for 13, 1002 upgrade for 10, 1003 worse than worn for 13, 1004 for 16, "
        "1005 junk");
    checker.Note("  " + DescribeStop(view));
    std::vector<uint32_t> const sold = SoldAtStop(view);
    checker.Check(view.carried.items.size() == 1 && view.carried.items[0].entry == 1001 &&
                      KeptWorthEach(view.carried, 1001, 10) == USE,
                  "the memo holds the upgrade for a later level at its later worth, nothing else");
    checker.Check(!Holds(sold, 1001), "a kept item is not sold");
    checker.Check(Holds(sold, 1003) && Holds(sold, 1004) && Holds(sold, 1005),
                  "items the rule does not keep sell as today");
    checker.Check(Holds(sold, 1002),
                  "a due upgrade is not carried: the item usage labels it an upgrade first (and equips it)");
    checker.Check(!KeptWorthEach(view.carried, 1001, 11) && !KeptWorthEach(CarriedKeeps(), 1001, 0),
                  "read at another level, or never built, the memo keeps nothing");
}

inline void CheckCarriedReleased(Checker& checker)
{
    CarriedView const before = ViewOf(10, {Gear(1101, 13), Gear(1102, 14)});
    // The bot has since put on better gear for 1101's slot, and 1102 no longer fits its armor kind
    KeepItemIn outgrown = Gear(1102, 14);
    outgrown.wearable = false;
    CarriedView const after = ViewOf(10, {Gear(1101, 13, BETTER, 60.0f), outgrown});
    checker.Note("released: " + DescribeStop(before) + " -> " + DescribeStop(after));
    checker.Check(before.carried.items.size() == 2 && !Holds(SoldAtStop(before), 1101), "kept while worth more later");
    checker.Check(after.carried.items.empty() && Holds(SoldAtStop(after), 1101) && Holds(SoldAtStop(after), 1102),
                  "a released item falls back to its ordinary usage and is sold");
    KeepItemIn cheap = Gear(1103, 13, BETTER, WORN, SELL, SELL);
    checker.Check(SoldAtStop(ViewOf(10, {cheap})).size() == 1,
                  "later worth not above what it fetches now: never kept, sold");
}

inline void CheckCarriedSpace(Checker& checker)
{
    KeepItemIn junk = Gear(1201, 0, 0.0f, 0.0f, 0, 50);
    junk.wearable = false;
    KeepItemIn cheapUse = Gear(1202, 0, 0.0f, 0.0f, 0, 300);
    cheapUse.wearable = false;
    KeepItemIn dear = Gear(1203, 0, 0.0f, 0.0f, 0, 900);
    dear.wearable = false;
    KeepItemIn const kept = Gear(1204, 13);  // later worth 400, sells for 100

    auto victimOf = [](std::vector<KeepItemIn> const& bags) -> uint32_t
    {
        CarriedView const view = ViewOf(10, bags);
        std::vector<WorldTravel::SpaceCandidate> candidates;
        for (BankItemIn const& item : view.in.bags)
            candidates.push_back({SpaceWorth(view, item.item), false});
        int32_t const victim = WorldTravel::LowestWorthVictim(candidates);
        return victim == WorldTravel::NO_VICTIM ? 0 : view.in.bags[victim].item.entry;
    };
    uint32_t const withJunk = victimOf({kept, dear, junk});
    uint32_t const keptLast = victimOf({kept, dear});
    uint32_t const cheaperThere = victimOf({kept, dear, cheapUse});
    checker.Note("full bags, victims: kept + dear + junk -> " + std::to_string(withJunk) + "; kept + dear -> " +
                 std::to_string(keptLast) + "; kept + dear + one worth 300 -> " + std::to_string(cheaperThere));
    checker.Check(withJunk == 1201, "with full bags the junk goes before the kept item");
    checker.Check(cheaperThere == 1202, "anything worth less than the kept item's later worth goes first");
    checker.Check(keptLast == 1204, "the kept item goes only when everything else is worth more");
    CarriedView const view = ViewOf(10, {kept});
    checker.Check(SpaceWorth(view, view.in.bags[0].item) == USE,
                  "in the space rule a kept item counts at its later worth, not as priceless");
}

// Seeded search over random bags (the seed is printed): the memo keeps exactly the items the rule keeps and that are
// not due, at their later worth; read at another level it keeps nothing; a kept item's space worth is its later worth,
// above what it fetches now, so the space rule takes it only when no destroyable item is worth less
inline void CheckCarriedSearch(Checker& checker, uint32_t seed)
{
    constexpr uint32_t ROUNDS = 2000;
    constexpr uint32_t MAX_ITEMS = 12;
    uint32_t state = seed ? seed : 1;
    auto next = [&state](uint32_t range) -> uint32_t
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return range ? state % range : 0;
    };

    uint32_t memoWrong = 0;
    uint32_t staleKept = 0;
    uint32_t worthWrong = 0;
    uint32_t victimWrong = 0;
    uint32_t keptSeen = 0;
    uint32_t keptVictims = 0;
    for (uint32_t round = 0; round < ROUNDS; ++round)
    {
        uint8_t const level = static_cast<uint8_t>(1 + next(75));
        std::vector<KeepItemIn> bags;
        uint32_t const count = 1 + next(MAX_ITEMS);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t const required =
                static_cast<uint8_t>(std::max<int32_t>(0, level - 3 + static_cast<int32_t>(next(12))));
            KeepItemIn item = Gear(2000 + i, required, static_cast<float>(next(60)), static_cast<float>(next(60)),
                                   next(800), next(400));
            item.wearable = next(4) != 0;
            item.slotEmpty = next(8) == 0;
            bags.push_back(item);
        }
        CarriedView const view = ViewOf(level, bags);
        std::vector<WorldTravel::SpaceCandidate> candidates;
        for (KeepItemIn const& item : bags)
        {
            KeepAnswer const answer = DecideKeep(item, view.in.bot);
            bool const keptNotDue = answer.keeps && !IsDue(answer, view.in.bot);
            uint32_t const each = KeptWorthEach(view.carried, item.entry, level);
            if (keptNotDue != (each > 0) || (keptNotDue && each != answer.laterWorth))
                ++memoWrong;
            if (KeptWorthEach(view.carried, item.entry, static_cast<uint8_t>(level + 1)))
                ++staleKept;
            uint32_t const worth = SpaceWorth(view, item);
            if (keptNotDue && (worth != answer.laterWorth || worth <= item.sellNow))
                ++worthWrong;
            keptSeen += keptNotDue ? 1 : 0;
            candidates.push_back({worth, false});
        }
        int32_t const victim = WorldTravel::LowestWorthVictim(candidates);
        if (victim == WorldTravel::NO_VICTIM)
            continue;
        for (WorldTravel::SpaceCandidate const& candidate : candidates)
            if (candidate.Worth < candidates[victim].Worth)
                ++victimWrong;
        keptVictims += KeptWorthEach(view.carried, bags[victim].entry, level) ? 1 : 0;
    }
    checker.Note("seeded search (seed " + std::to_string(seed) + ", " + std::to_string(ROUNDS) +
                 " bags): " + std::to_string(keptSeen) + " kept items, " + std::to_string(keptVictims) +
                 " kept victims; wrong memo " + std::to_string(memoWrong) + ", kept at another level " +
                 std::to_string(staleKept) + ", wrong worth " + std::to_string(worthWrong) +
                 ", a cheaper item spared " + std::to_string(victimWrong));
    checker.Check(memoWrong == 0, "search: the memo keeps exactly the kept items not due yet, at their later worth");
    checker.Check(staleKept == 0, "search: read at another level the memo keeps nothing");
    checker.Check(worthWrong == 0, "search: a kept item's space worth is its later worth, above its sell worth");
    checker.Check(victimWrong == 0 && keptSeen > 0, "search: no destroyable item worth less than the victim is spared");
}
// Item use WI9b: demand's use stock. A stack of a potion (sells 25, worth 300 a unit to the bot) with its own guid
constexpr uint32_t POTION = 33447;
constexpr uint32_t POTION_WORTH = 300;
constexpr uint32_t POTION_SELL = 25;

inline KeepItemIn Potion(uint32_t guid, uint32_t count, uint32_t entry = POTION)
{
    KeepItemIn item;
    item.itemGuid = guid;
    item.entry = entry;
    item.count = count;
    item.stackMax = STACK_20;
    item.sellNow = POTION_SELL * count;
    return item;
}

// The bags and the bank as BuildBankIn reads them, then demand's stock marked on them (one demand slot: 0)
inline BankIn StockIn(uint8_t level, std::vector<KeepItemIn> const& bags, std::vector<KeepItemIn> const& bank,
                      std::vector<StockEntry> const& entries, uint32_t wanted, uint32_t bagFree = 4,
                      uint32_t bankFree = 20)
{
    BankIn in = MakeIn(level, bags, bank, bagFree, bankFree);
    std::vector<uint32_t> const later = {
        static_cast<uint32_t>(std::ceil(wanted * static_cast<double>(BANK_STOCK_SHARE)))};
    AssignStock(entries, later, in.bags, in.bank);
    return in;
}

inline BankVerdict const* VerdictOfGuid(BankPlan const& plan, uint32_t guid)
{
    auto verdict = std::find_if(plan.verdicts.begin(), plan.verdicts.end(),
                                [guid](BankVerdict const& candidate) { return candidate.itemGuid == guid; });
    return verdict == plan.verdicts.end() ? nullptr : &*verdict;
}

inline bool GuidIs(BankPlan const& plan, uint32_t guid, BankChoice choice, BankReason reason)
{
    BankVerdict const* verdict = VerdictOfGuid(plan, guid);
    return verdict && verdict->choice == choice && verdict->reason == reason;
}

// What a plan does, order-free: every step's kind and guid and every verdict's choice, reason and guid, sorted
inline std::vector<std::string> PlanFacts(BankPlan const& plan)
{
    std::vector<std::string> facts;
    for (BankStep const& step : plan.steps)
        facts.push_back(std::string("step ") + ChoiceWord(step.kind) + " " + std::to_string(step.itemGuid));
    for (BankVerdict const& verdict : plan.verdicts)
        facts.push_back(std::string("verdict ") + ChoiceWord(verdict.choice) + " " + ReasonWord(verdict.reason) + " " +
                        std::to_string(verdict.itemGuid));
    std::sort(facts.begin(), facts.end());
    return facts;
}

inline void CheckStock(Checker& checker)
{
    std::vector<StockEntry> const keepSix = {{POTION, POTION_WORTH, 6, 0}};

    // Three stacks of 20 in the bags, the bags carry 6: one stack carried, one banked, the third above the share
    BankIn const sixty = StockIn(80, {Potion(1, 20), Potion(2, 20), Potion(3, 20)}, {}, keepSix, 6);
    BankPlan const banked = PlanBankVisit(sixty);
    checker.Show("stock: 3 stacks of 20 in the bags, the bags carry 6, wanted 6", banked);
    checker.Check(GuidIs(banked, 1, BankChoice::Keep, BankReason::Due), "the first stack is carried: due, it stays");
    checker.Check(
        GuidIs(banked, 2, BankChoice::Deposit, BankReason::KeptStock) && Moves(banked, {{BankChoice::Deposit, POTION}}),
        "the next stack is stock for later: deposited kept_stock");
    checker.Check(!VerdictOfGuid(banked, 3), "above the share (one more wanted stock): not kept, no move");
    BankVerdict const* later = VerdictOfGuid(banked, 2);
    checker.Check(later && later->laterWorth == 20 * POTION_WORTH && later->dueLevel == STOCK_LATER_LEVEL &&
                      std::string(later->provider) == "stock",
                  "stock for later: worth 20 units, due at no level, the stock provider");
    CarriedKeeps const memo = KeptInBags(sixty);
    checker.Check(memo.items.size() == 1 && KeptWorthEach(memo, POTION, 80) == POTION_WORTH,
                  "the bags' memo holds the stack for later (the item usage banks it), not the carried one");

    // The order of the stacks never changes the plan
    // The same stacks in the same places, read in the opposite order
    BankIn reversed = MakeIn(80, {Potion(1, 20), Potion(2, 20), Potion(3, 20)}, {}, 4, 20);
    std::reverse(reversed.bags.begin(), reversed.bags.end());
    std::vector<uint32_t> const share = {6};
    AssignStock(keepSix, share, reversed.bags, reversed.bank);
    checker.Check(PlanFacts(PlanBankVisit(reversed)) == PlanFacts(banked),
                  "the stacks' order does not change the plan");
    BankIn const smallFirst = StockIn(80, {Potion(4, 5), Potion(5, 20)}, {}, keepSix, 6);
    BankPlan const small = PlanBankVisit(smallFirst);
    checker.Check(GuidIs(small, 5, BankChoice::Keep, BankReason::Due) &&
                      GuidIs(small, 4, BankChoice::Deposit, BankReason::KeptStock),
                  "the larger stack is carried first");

    // The bags emptied: a bank stack is carried now, so it is withdrawn; the other stays as stock for later
    BankIn const emptied = StockIn(80, {}, {Potion(11, 20), Potion(12, 20)}, keepSix, 6);
    BankPlan const fetched = PlanBankVisit(emptied);
    checker.Show("stock: bags emptied, 2 bank stacks of 20, the bags carry 6", fetched);
    checker.Check(fetched.withdrawDue == 1 && GuidIs(fetched, 11, BankChoice::Withdraw, BankReason::Due),
                  "one bank stack withdrawn due");
    checker.Check(GuidIs(fetched, 12, BankChoice::Leave, BankReason::KeptStock), "the other stays kept_stock");
    checker.Check(DueAt(KeptInBank(emptied), 80).count == 20, "only the carried bank stack is due");
    BankIn const stocked = StockIn(80, {Potion(13, 20)}, {Potion(12, 20)}, keepSix, 6);
    checker.Check(DueAt(KeptInBank(stocked), 80).count == 0 && DueAt(KeptInBank(stocked), 255).count == 20,
                  "stock for later is due at no level: it makes no bank trip");
    // The visit after the first: the banked stack stays stock for later, the bags' third stack does not take its place
    BankPlan const again = PlanBankVisit(StockIn(80, {Potion(1, 20), Potion(3, 20)}, {Potion(2, 20)}, keepSix, 6));
    checker.Show("stock: the next visit, 2 bag stacks and the banked one", again);
    checker.Check(again.steps.empty() && GuidIs(again, 2, BankChoice::Leave, BankReason::KeptStock) &&
                      GuidIs(again, 1, BankChoice::Keep, BankReason::Due) && !VerdictOfGuid(again, 3),
                  "the next visit moves nothing: the banked stack stays, the bags keep the rest");

    // WB.2 holds for a surplus entry (demand sells it): stock for later worth no more than it sells for is not kept
    std::vector<StockEntry> const cheap = {{POTION, POTION_SELL, 0, 0}};
    BankPlan const cheapPlan = PlanBankVisit(StockIn(80, {Potion(21, 20)}, {Potion(23, 20)}, cheap, 40));
    checker.Check(!VerdictOfGuid(cheapPlan, 21) && GuidIs(cheapPlan, 23, BankChoice::Withdraw, BankReason::Released),
                  "a surplus entry worth its sale: not kept, a bank stack is released");
    // An entry demand keeps is never sold: its stacks are carried or banked whatever their sale fetches
    std::vector<StockEntry> const dear = {{POTION, POTION_SELL / 5, 6, 0}};
    BankPlan const dearPlan = PlanBankVisit(StockIn(80, {Potion(24, 20), Potion(25, 20)}, {}, dear, 6));
    checker.Check(GuidIs(dearPlan, 24, BankChoice::Keep, BankReason::Due) &&
                      GuidIs(dearPlan, 25, BankChoice::Deposit, BankReason::KeptStock),
                  "a kept entry worth less than its sale: carried and banked, not left for a sale");

    // A weaker potion of the slot (no carried unit) shares the slot's share: the stronger one's rest takes it first
    std::vector<StockEntry> const twoPotions = {{POTION, POTION_WORTH, 6, 0}, {POTION + 1, 200, 0, 0}};
    BankPlan const shared =
        PlanBankVisit(StockIn(80, {Potion(31, 20), Potion(32, 20), Potion(33, 10, POTION + 1)}, {}, twoPotions, 6));
    checker.Check(GuidIs(shared, 32, BankChoice::Deposit, BankReason::KeptStock) && !VerdictOfGuid(shared, 33),
                  "the share is the slot's: a surplus potion beyond it is not kept");
    BankPlan const alone = PlanBankVisit(StockIn(80, {Potion(31, 20), Potion(33, 10, POTION + 1)}, {}, twoPotions, 6));
    checker.Check(!VerdictOfGuid(alone, 33),
                  "NE8b: a surplus potion (carry 0) is not stock for later, even within the share");
    // The one later case that stays: a carried entry's units beyond its carried ones, while the slot's budget lasts
    BankPlan const rest = PlanBankVisit(StockIn(80, {Potion(34, 6), Potion(35, 4)}, {}, keepSix, 6));
    checker.Check(GuidIs(rest, 34, BankChoice::Keep, BankReason::Due) &&
                      GuidIs(rest, 35, BankChoice::Deposit, BankReason::KeptStock),
                  "a carried entry beyond its carried units is stock for later while the budget lasts");

    // Not demand's item: the gear provider decides as before, beside the stock
    BankIn mixed = StockIn(10, {Potion(41, 20), Potion(42, 20), Gear(901, 13)}, {}, keepSix, 6);
    BankPlan const both = PlanBankVisit(mixed);
    checker.Check(Is(both, 901, BankChoice::Deposit, BankReason::KeptForLevel) &&
                      std::string(VerdictFor(both, 901)->provider) == "gear_for_level" &&
                      GuidIs(both, 42, BankChoice::Deposit, BankReason::KeptStock),
                  "gear still goes to the gear provider, kept_for_level");
    BankIn none = MakeIn(80, {Potion(51, 20)}, {}, 4, 20);
    checker.Check(PlanBankVisit(none).verdicts.empty(), "a potion demand does not list is no stock");
}

// NE7 (needs-and-errands), the night lab5-20261122-030123: all 28 deposit_withdrawn_again rows were stacks of an entry
// demand carried no unit of (no due row of the entry at the deposit visit), banked as stock for later and released
// at a later visit once other stacks took the slot's share before them. Replayed with one slot: A, which demand
// carries (6 wanted), and B, a weaker entry demand keeps none of. NE8b flips the B ones: stock for later
// only for an entry demand carries units of
inline void CheckStockChurn(Checker& checker)
{
    constexpr uint32_t WEAK = POTION + 1;
    constexpr uint32_t MIDDLE = POTION + 2;
    std::vector<StockEntry> const carriedAndWeak = {{POTION, POTION_WORTH, 6, 0}, {WEAK, 100, 0, 0}};
    BankPlan const deposit = PlanBankVisit(StockIn(12, {Potion(61, 6), Potion(62, 2, WEAK)}, {}, carriedAndWeak, 6));
    checker.Show("NE8b churn: the deposit, A carried 6, B (demand keeps none) 2 in the bags, wanted 6", deposit);
    checker.Check(
        GuidIs(deposit, 61, BankChoice::Keep, BankReason::Due) && !VerdictOfGuid(deposit, 62) && deposit.steps.empty(),
        "NE8b: B, which demand keeps none of, gets no mark and no deposit step (sold as surplus)");

    // (1) The carried entry's overflow takes the share first: a top-up left 20 of A above its 6 (bot 857's water
    // beside its Enriched Lasher Root)
    BankPlan const overflow =
        PlanBankVisit(StockIn(12, {Potion(61, 6), Potion(63, 20)}, {Potion(62, 2, WEAK)}, carriedAndWeak, 6));
    checker.Show("NE8b churn: A's overflow of 6 beside the banked B", overflow);
    checker.Check(GuidIs(overflow, 61, BankChoice::Deposit, BankReason::KeptStock) &&
                      GuidIs(overflow, 62, BankChoice::Withdraw, BankReason::Released),
                  "NE8b: B's bank stack, never marked, is released once; A's overflow is A's own stock for later");

    // (2) A dearer entry demand keeps none of enters the share (bot 1147, one level: Scroll of Strength IV, worth
    // more, banked beside the Stamina IV it releases)
    std::vector<StockEntry> const dearer = {{POTION, POTION_WORTH, 6, 0}, {MIDDLE, 200, 0, 0}, {WEAK, 100, 0, 0}};
    BankPlan const ranked =
        PlanBankVisit(StockIn(12, {Potion(61, 6), Potion(64, 6, MIDDLE)}, {Potion(62, 2, WEAK)}, dearer, 6));
    checker.Show("NE8b churn: a dearer entry kept by none, 6 in the bags, B banked", ranked);
    checker.Check(
        !VerdictOfGuid(ranked, 64) && GuidIs(ranked, 62, BankChoice::Withdraw, BankReason::Released),
        "NE8b: a dearer entry demand keeps none of is never marked, so it takes no share from B (both released)");
    BankPlan const own = PlanBankVisit(
        StockIn(12, {Potion(61, 6), Potion(64, 6, MIDDLE)}, {Potion(62, 2, WEAK), Potion(65, 20)}, dearer, 6));
    checker.Check(GuidIs(own, 65, BankChoice::Leave, BankReason::KeptStock),
                  "the carried entry's own bank stack is stock for later first, whatever enters (NE8 keeps this)");

    // (3) The share shrinks: wanted falls from 6 to 1 (a lower measured use; bots 707 and 797, no deposit at the
    // withdrawal) and a banked entry ranked before B keeps the one unit
    std::vector<StockEntry> const sixWanted = {{POTION, POTION_WORTH, 6, 0}, {MIDDLE, 150, 0, 0}, {WEAK, 100, 0, 0}};
    std::vector<StockEntry> const oneWanted = {{POTION, POTION_WORTH, 1, 0}, {MIDDLE, 150, 0, 0}, {WEAK, 100, 0, 0}};
    std::vector<KeepItemIn> const banked = {Potion(66, 1, MIDDLE), Potion(62, 2, WEAK)};
    BankPlan const before = PlanBankVisit(StockIn(12, {Potion(61, 6)}, banked, sixWanted, 6));
    BankPlan const after = PlanBankVisit(StockIn(12, {Potion(67, 1)}, banked, oneWanted, 1));
    checker.Show("NE8b churn: wanted 1, B banked behind another entry kept by none", after);
    checker.Check(GuidIs(before, 62, BankChoice::Withdraw, BankReason::Released) &&
                      GuidIs(after, 62, BankChoice::Withdraw, BankReason::Released),
                  "NE8b: B was never marked: a smaller wanted stock moves it neither way (released once, both visits)");
}

// Professions WR12: a material the skill path uses, a recipe item, and the bot's skills. Smithing 164 at 100
constexpr uint16_t SMITHING = 164;
constexpr uint16_t TAILORING = 197;
constexpr uint16_t SMITH_SKILL = 100;
constexpr uint32_t MATERIAL_EACH = 30;
constexpr uint32_t MATERIAL_SEARCH_SEED = 20261120;

inline KeepItemIn UsedMaterial(uint32_t entry, uint32_t count, uint32_t used, uint16_t dueSkill,
                               uint32_t sellNow = SELL)
{
    KeepItemIn item;
    item.itemGuid = entry;
    item.entry = entry;
    item.count = count;
    item.stackMax = STACK_20;
    item.sellNow = sellNow;
    item.materialUsed = used;
    item.materialCopperEach = used ? MATERIAL_EACH : 0;
    item.materialSkill = used ? SMITHING : 0;
    item.materialDueSkill = dueSkill;
    return item;
}

inline KeepItemIn RecipeItem(uint32_t entry, uint16_t skill, uint16_t rank, bool known = false)
{
    KeepItemIn item;
    item.itemGuid = entry;
    item.entry = entry;
    item.sellNow = SELL;
    item.useWorth = USE;
    item.recipeSkill = skill;
    item.recipeRank = rank;
    item.recipeKnown = known;
    return item;
}

inline BankIn SmithIn(uint16_t skill, std::vector<KeepItemIn> const& bags, std::vector<KeepItemIn> const& bank,
                      uint8_t level = 20)
{
    BankIn in = MakeIn(level, bags, bank, 4, 20);
    in.bot.skills = {{SMITHING, skill}};
    return in;
}

inline void CheckMaterials(Checker& checker)
{
    KeepBotIn smith = SmithIn(SMITH_SKILL, {}, {}).bot;
    // A wanted material sells for more than its cost by its source (a rare case) and is still not sold (WR.6)
    KeepAnswer const wanted = DecideKeep(UsedMaterial(3001, 10, 10, 0, 5000), smith);
    checker.Show("wanted material, 10 used, sells for 5000", wanted);
    checker.Check(wanted.keeps && wanted.unsold && IsDue(wanted, smith) && std::string(wanted.provider) == "materials",
                  "a material the path uses now is kept, never sold, due now");
    checker.Check(!DecideKeep(UsedMaterial(3002, 10, 0, 0), smith).keeps,
                  "a material the path does not use is not kept");
    KeepAnswer const part = DecideKeep(UsedMaterial(3003, 20, 5, 0), smith);
    checker.Check(part.laterWorth == 5 * MATERIAL_EACH, "a stack is worth only the units the path uses");

    // A material of a later step is banked and due at its skill
    BankIn later = SmithIn(SMITH_SKILL, {UsedMaterial(3004, 10, 10, 125)}, {});
    BankPlan const banked = PlanBankVisit(later);
    checker.Show("smith at 100, a material for its step at 125", banked);
    checker.Check(Is(banked, 3004, BankChoice::Deposit, BankReason::KeptForSkill),
                  "a later step's material is banked, kept for its skill");
    checker.Check(
        KeptInBank(SmithIn(SMITH_SKILL, {}, {UsedMaterial(3004, 10, 10, 125)})).front().dueLevel == STOCK_LATER_LEVEL,
        "a banked material for a later skill makes no bank trip of its own");
    BankPlan const fetched = PlanBankVisit(SmithIn(125, {}, {UsedMaterial(3004, 10, 10, 125)}));
    checker.Show("smith at 125, the material in the bank", fetched);
    checker.Check(Is(fetched, 3004, BankChoice::Withdraw, BankReason::Due), "at its skill the material is due");
    checker.Check(Is(PlanBankVisit(SmithIn(SMITH_SKILL, {UsedMaterial(3005, 10, 10, 0)}, {})), 3005, BankChoice::Keep,
                     BankReason::Due),
                  "a material used now stays in the bags");
    CarriedKeeps const carried =
        KeptInBags(SmithIn(SMITH_SKILL, {UsedMaterial(3004, 10, 10, 125), UsedMaterial(3005, 10, 10, 0)}, {}));
    checker.Check(KeptWorthEach(carried, 3004, 20) == MATERIAL_EACH && !KeptWorthEach(carried, 3005, 20),
                  "the item usage's memo holds the later material (banked, not sold), not the one used now");

    // Recipe items: within reach of the bot's reference skill kept and due at their skill, far above it not
    KeepAnswer const near = DecideKeep(RecipeItem(3101, SMITHING, 125), smith);
    checker.Show("smithing recipe at 125, smith at 100", near);
    checker.Check(near.keeps && !IsDue(near, smith) && near.dueSkill == 125,
                  "a recipe item above the bot's skill is kept, not due yet");
    KeepBotIn grown = smith;
    grown.skills = {{SMITHING, 125}};
    checker.Check(IsDue(DecideKeep(RecipeItem(3101, SMITHING, 125), grown), grown),
                  "at its skill the recipe item is due");
    checker.Check(!DecideKeep(RecipeItem(3102, SMITHING, 200), smith).keeps,
                  "a recipe item far above the reference skill (100 + 75) is not kept");
    checker.Check(DecideKeep(RecipeItem(3103, SMITHING, 175), smith).keeps &&
                      DecideKeep(RecipeItem(3104, SMITHING, 180), SmithIn(SMITH_SKILL, {}, {}, 21).bot).keeps,
                  "the reach counts from the skill now or the level times five, whichever is higher");
    checker.Check(!DecideKeep(RecipeItem(3105, TAILORING, 50), smith).keeps &&
                      !DecideKeep(RecipeItem(3106, SMITHING, 50, true), smith).keeps,
                  "a recipe of another profession, or one the bot knows, is not kept");
    BankPlan const recipeBanked = PlanBankVisit(SmithIn(SMITH_SKILL, {RecipeItem(3101, SMITHING, 125)}, {}));
    checker.Check(Is(recipeBanked, 3101, BankChoice::Deposit, BankReason::KeptForSkill),
                  "a recipe for a later skill is banked, kept for its skill");
}

// Seeded search over random professions items and skills: the materials provider keeps exactly the materials the path
// uses and the recipes in reach; due exactly when the skill (and level) is reached; the bank never deposits a due one
// and never withdraws one not due. The negative control, a due check blind to skill, must find counterexamples
inline uint32_t MaterialViolations(uint32_t seed, bool skillBlind, uint32_t& kept)
{
    constexpr uint32_t ROUNDS = 2000;
    uint32_t state = seed ? seed : 1;
    auto next = [&state](uint32_t range) -> uint32_t
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return range ? state % range : 0;
    };
    uint32_t violations = 0;
    for (uint32_t round = 0; round < ROUNDS; ++round)
    {
        uint8_t const level = static_cast<uint8_t>(1 + next(80));
        uint16_t const skill = static_cast<uint16_t>(next(4) ? 1 + next(450) : 0);
        std::vector<KeepItemIn> bags;
        std::vector<KeepItemIn> bank;
        uint32_t const count = 1 + next(8);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t const entry = 4000 + round * 16 + i;
            KeepItemIn item = next(3) ? UsedMaterial(entry, 1 + next(20), next(3) ? next(40) : 0,
                                                     static_cast<uint16_t>(next(2) ? 0 : next(500)), next(400))
                                      : RecipeItem(entry, next(4) ? SMITHING : TAILORING,
                                                   static_cast<uint16_t>(next(500)), next(5) == 0);
            item.requiredLevel = static_cast<uint8_t>(next(3) ? 0 : next(80));
            (next(2) ? bags : bank).push_back(item);
        }
        BankIn in = MakeIn(level, bags, bank, 2 + next(6), next(10));
        if (skill)
            in.bot.skills = {{SMITHING, skill}};
        auto check = [&](KeepItemIn const& item)
        {
            KeepAnswer const answer = DecideKeep(item, in.bot);
            uint16_t const now = SkillOf(in.bot, item.recipeSkill ? item.recipeSkill : item.materialSkill);
            uint32_t const reference = std::max<uint32_t>(now, static_cast<uint32_t>(level) * SKILL_PER_LEVEL);
            bool const shouldKeep =
                item.recipeSkill ? (!item.recipeKnown && now && item.recipeRank <= reference + RECIPE_KEEP_REACH &&
                                    item.useWorth > item.sellNow)
                                 : item.materialUsed > 0;
            uint8_t const dueLevel = item.recipeSkill ? std::max(item.requiredLevel, uint8_t{1}) : level;
            uint16_t const dueSkill = item.recipeSkill ? item.recipeRank : item.materialDueSkill;
            bool const shouldDue = shouldKeep && dueLevel <= level && (skillBlind || now >= dueSkill);
            kept += answer.keeps ? 1 : 0;
            return answer.keeps != shouldKeep || IsDue(answer, in.bot) != shouldDue;
        };
        for (BankItemIn const& item : in.bags)
            violations += check(item.item);
        for (BankItemIn const& item : in.bank)
            violations += check(item.item);
        BankPlan const plan = PlanBankVisit(in);
        for (BankStep const& step : plan.steps)
        {
            auto const& from = step.kind == BankChoice::Deposit ? in.bags : in.bank;
            auto const at = std::find_if(from.begin(), from.end(), [&step](BankItemIn const& item)
                                         { return item.item.itemGuid == step.itemGuid; });
            if (at == from.end())
            {
                ++violations;
                continue;
            }
            KeepAnswer const answer = DecideKeep(at->item, in.bot);
            bool const due = IsDue(answer, in.bot);
            violations += step.kind == BankChoice::Deposit ? (!answer.keeps || due) : (answer.keeps && !due);
        }
    }
    return violations;
}

inline void CheckMaterialSearch(Checker& checker, uint32_t seed)
{
    uint32_t kept = 0;
    uint32_t controlKept = 0;
    uint32_t const violations = MaterialViolations(seed, false, kept);
    uint32_t const control = MaterialViolations(seed, true, controlKept);
    checker.Note("materials search (seed " + std::to_string(seed) + ", 2000 bags): " + std::to_string(kept) +
                 " kept, " + std::to_string(violations) +
                 " violations; negative control (due blind to skill): " + std::to_string(control) + " counterexamples");
    checker.Check(violations == 0 && kept > 0,
                  "search: kept exactly the path's materials and the recipes in reach, due exactly at their skill");
    checker.Check(control > 0, "search: the negative control finds counterexamples");
}
// Professions WR17: the bank-space rule. Six cloaks kept for level 57 at 54 (400 later each) and a bank with two free
// slots: four do not fit. A 6-slot bag carried that sells for 200, the first slot at 1000 (BankBagSlotPrices.dbc)
constexpr uint32_t FIRST_SLOT_PRICE = 1000;
constexpr uint32_t LATER_SLOT_PRICE = 250000;
constexpr uint32_t SPARE_BAG = 4238;
constexpr uint32_t SPARE_BAG_SLOTS = 6;
constexpr uint32_t SPARE_BAG_SELL = 200;
constexpr uint64_t RICH = 1000000;

inline BankIn SpaceIn(uint32_t kept, uint32_t bankFree, uint64_t gold = RICH, bool carriesBag = true)
{
    std::vector<KeepItemIn> cloaks;
    for (uint32_t i = 0; i < kept; ++i)
    {
        cloaks.push_back(Gear(18509, 57));
        cloaks.back().itemGuid = 5000 + i;
    }
    BankIn in = MakeIn(54, cloaks, {}, 10, bankFree);
    if (carriesBag)
        in.space.bag = {MAIN_BAG,      static_cast<uint8_t>(BACKPACK_SLOT + kept), 6000, SPARE_BAG, SPARE_BAG_SLOTS,
                        SPARE_BAG_SELL};
    in.space.nextPrice = FIRST_SLOT_PRICE;
    in.space.gold = gold;
    return in;
}

inline SpaceAnswer SpaceOf(BankIn const& in) { return DecideBankSpace(in, PlanBankVisit(in)); }

inline bool Is(SpaceAnswer const& answer, SpaceChoice choice, SpaceReason reason)
{
    return answer.choice == choice && answer.reason == reason;
}

inline void CheckBankSpace(Checker& checker)
{
    SpaceAnswer const buy = SpaceOf(SpaceIn(6, 2));
    checker.Note("bank space, 4 left out, rich: " + DescribeSpace(buy));
    checker.Check(Is(buy, SpaceChoice::BuySlot, SpaceReason::Worth) && buy.overflow == 4 && buy.worth == 4 * USE &&
                      buy.price == FIRST_SLOT_PRICE + SPARE_BAG_SELL && buy.bagEntry == SPARE_BAG,
                  "a slot is bought when what does not fit is worth more than the slot and the bag");

    SpaceAnswer const fits = SpaceOf(SpaceIn(6, 6));
    checker.Check(Is(fits, SpaceChoice::None, SpaceReason::Fits) && !fits.overflow, "nothing is bought when all fits");

    BankIn dearIn = SpaceIn(6, 2);
    dearIn.space.nextPrice = LATER_SLOT_PRICE;
    SpaceAnswer const dear = SpaceOf(dearIn);
    checker.Note("bank space, the fourth slot's price: " + DescribeSpace(dear));
    checker.Check(Is(dear, SpaceChoice::NoSlot, SpaceReason::Dear), "no purchase below the price");

    BankIn edgeIn = SpaceIn(6, 2);
    edgeIn.space.nextPrice = 4 * USE - SPARE_BAG_SELL;
    checker.Check(Is(SpaceOf(edgeIn), SpaceChoice::BuySlot, SpaceReason::Worth), "a price equal to the worth buys");
    edgeIn.space.nextPrice += 1;
    checker.Check(Is(SpaceOf(edgeIn), SpaceChoice::NoSlot, SpaceReason::Dear), "one copper above the worth does not");

    SpaceAnswer const poor = SpaceOf(SpaceIn(6, 2, 500));
    checker.Note("bank space, 5 silver: " + DescribeSpace(poor));
    checker.Check(Is(poor, SpaceChoice::NoSlot, SpaceReason::Money), "a poor bot buys nothing");

    BankIn reserved = SpaceIn(6, 2, 10000);
    reserved.space.reserve = 9500;
    checker.Check(Is(SpaceOf(reserved), SpaceChoice::NoSlot, SpaceReason::Money),
                  "the money a bot soon needs is not spent on a slot");

    checker.Check(Is(SpaceOf(SpaceIn(6, 2, RICH, false)), SpaceChoice::NoSlot, SpaceReason::NoBag),
                  "no slot without a bag to put in it");

    // A bag held only some of what does not fit: its worth counts the most valuable ones
    BankIn mixed = SpaceIn(6, 2);
    mixed.space.bag.slots = 2;
    for (size_t i = 0; i < mixed.bags.size(); ++i)
        mixed.bags[i].item.useWorth = USE + static_cast<uint32_t>(i) * 100;
    SpaceAnswer const two = SpaceOf(mixed);
    checker.Check(two.worth == (USE + 300) + (USE + 200), "the bag's worth is its slots' most valuable items");

    BankIn open = SpaceIn(6, 2, 0);
    open.space.bought = 1;
    SpaceAnswer const place = SpaceOf(open);
    checker.Check(Is(place, SpaceChoice::PlaceBag, SpaceReason::Worth) && place.price == SPARE_BAG_SELL,
                  "a bought slot without a bag gets the carried bag, money or not");

    // Never beyond the engine's slots: 100 kept items, a bag always at hand and money without end
    BankIn many = SpaceIn(100, 0, std::numeric_limits<uint32_t>::max());
    uint32_t bought = 0;
    uint32_t steps = 0;
    SpaceAnswer last;
    for (; steps < 3 * BANK_BAG_SLOTS; ++steps)
    {
        many.space.nextPrice = many.space.bought < BANK_BAG_SLOTS ? FIRST_SLOT_PRICE : 999999999;
        last = SpaceOf(many);
        if (last.choice == SpaceChoice::BuySlot)
        {
            ++many.space.bought;
            ++bought;
        }
        else if (last.choice == SpaceChoice::PlaceBag)
        {
            ++many.space.filled;
            many.bankSpace.freeSlots += SPARE_BAG_SLOTS;
        }
        else
            break;
    }
    checker.Note("bank space, 100 left out: " + std::to_string(bought) + " slots bought in " + std::to_string(steps) +
                 " steps, then " + DescribeSpace(last));
    checker.Check(bought == BANK_BAG_SLOTS && many.space.filled == BANK_BAG_SLOTS &&
                      Is(last, SpaceChoice::NoSlot, SpaceReason::AllBought),
                  "never beyond the engine's bank bag slots");
}

// Auction house WA3a: auction stock (AuctionStock). The case auction_vendor.sale_expiry_threshold (hub
// docs/economy-reference/cases/auction_vendor.json, read 2026-11-25): a vendor pays 260 for 20 linen; a sale nets 296,
// an expiry and then the vendor 160; listing is worth 262 at p = 3/4 and the vendor's 260 at the break-even p = 25/34
constexpr uint32_t LINEN = 2589;
constexpr uint32_t LINEN_LOT = 20;
constexpr uint32_t CASE_VENDOR_EXIT = 260;
constexpr uint32_t CASE_SALE_NET = 296;
constexpr uint32_t CASE_EXPIRY_THEN_VENDOR = 160;
constexpr uint32_t CASE_MEAN_AT_THREE_QUARTERS = 262;
// The case gear_trade.sell_boundary (hub docs/economy-reference/cases/gear_trade.json, read 2026-11-25): the Outfitter
// Belt sells for 6, its multiplier is 1 and its quality's factor 13421773/16777216; worn by 7 a vendor pays 0 for it,
// worn by 8 it pays 1
constexpr uint32_t BELT = 2186;
constexpr uint32_t BELT_SELL = 6;
constexpr uint32_t BELT_MULTIPLIER = 1;
constexpr float BELT_QUALITY_MOD = 13421773.0f / 16777216.0f;
constexpr uint32_t BELT_WEAR_EQUAL = 7;
constexpr uint32_t BELT_WEAR_OVER = 8;
constexpr uint32_t BELT_PAID_EQUAL = 0;
constexpr uint32_t BELT_PAID_OVER = 1;
constexpr uint64_t BELT_REFERENCE = 110;  // no history (p = 1/2): worth listing against 0 or 1 copper, not against 6
constexpr uint32_t STOCK_SEARCH_SEED = 20261125;
constexpr char const* AUCTION_STOCK = "auction_stock";

// A stack the item usage sells at the auction house, its fields filled for its stock units as the reader fills them
inline KeepItemIn Stock(uint32_t entry, uint32_t count, uint32_t vendorPayout, uint32_t auctionWorth)
{
    KeepItemIn item;
    item.itemGuid = entry;
    item.entry = entry;
    item.count = count;
    item.stackMax = STACK_20;
    item.sellNow = vendorPayout;
    item.auctionable = true;
    item.vendorPayout = vendorPayout;
    item.auctionWorth = auctionWorth;
    return item;
}

inline bool IsStock(KeepAnswer const& answer) { return answer.keeps && std::string(answer.provider) == AUCTION_STOCK; }

// The case's worth of listing at p = num / den, rounded down as the reader rounds: p x 296 + (1 - p) x 160
inline uint32_t CaseWorth(uint32_t num, uint32_t den)
{
    return (num * CASE_SALE_NET + (den - num) * CASE_EXPIRY_THEN_VENDOR) / den;
}

// The belt's listing worth against a vendor payout, from its reference value with no history
inline uint32_t BeltWorth(uint32_t payout)
{
    AuctionRules::AskInput in;
    in.count = 1;
    in.sellPrice = BELT_SELL;
    in.vendorPayout = payout;
    in.reference = BELT_REFERENCE;
    return AuctionRules::StockWorth(in, AuctionConfig());
}

// The belt as gear of the bot's level the item usage would not wear
inline KeepItemIn Belt(uint32_t payout, uint32_t entry = BELT)
{
    KeepItemIn belt = Stock(entry, 1, payout, BeltWorth(payout));
    belt.wearAnswer = WearAnswer::ItemUsage;
    return belt;
}

// Negative controls: stock rules with one fault each
inline KeepAnswer StockAtParity(KeepItemIn const& item, KeepBotIn const& bot)
{
    KeepItemIn more = item;
    ++more.auctionWorth;  // an equal worth passes
    return AuctionStock(more, bot);
}

inline KeepAnswer StockIgnoringPath(KeepItemIn const& item, KeepBotIn const& bot)
{
    KeepItemIn whole = item;
    whole.materialUsed = 0;
    return AuctionStock(whole, bot);
}

inline KeepAnswer StockOfFullStack(KeepItemIn const& item, KeepBotIn const& bot)
{
    KeepAnswer answer = AuctionStock(item, bot);
    if (answer.keeps)
        answer.laterWorth =
            item.auctionWorth / std::max<uint32_t>(item.count, 1) * std::max<uint32_t>(item.stackMax, 1);
    return answer;
}

// The WA3-4 rule (review B1), WA7b's A/B reference: an entry the skill path uses is never stock, a stack lists whole
inline KeepAnswer StockOfUnusedEntries(KeepItemIn const& item, KeepBotIn const& bot)
{
    return item.materialUsed ? KeepAnswer{} : AuctionStock(item, bot);
}

// The units of an entry the bags keep after the visit lists its listed stacks: StockUnits each, or the whole stack
// (whole: the control that lists a stack as WA3a did)
inline uint32_t LeftAfterListing(BankIn const& in, CarriedKeeps const& carried, uint32_t entry, bool whole = false)
{
    uint32_t left = 0;
    for (BankItemIn const& item : in.bags)
    {
        if (item.item.entry != entry)
            continue;
        uint32_t const count = std::max<uint32_t>(item.item.count, 1);
        uint32_t const listed = !ListsStack(carried, item.item.itemGuid) ? 0 : whole ? count : StockUnits(item.item);
        left += count - std::min(count, listed);
    }
    return left;
}

// The stacks the auction visit lists (AuctionVisit.cpp NextStock): the bag stacks whose own keep is auction stock;
// byEntry: WA3b's pick, any stack of an entry with a stack kept as stock (the control of review B1)
inline std::vector<uint32_t> ListedAtVisit(BankIn const& in, CarriedKeeps const& carried, bool byEntry = false)
{
    std::vector<uint32_t> listed;
    for (BankItemIn const& item : in.bags)
    {
        bool lists = ListsStack(carried, item.item.itemGuid);
        for (KeptEntry const& kept : carried.items)
            lists = lists || (byEntry && kept.entry == item.item.entry && std::string(kept.provider) == AUCTION_STOCK);
        if (lists)
            listed.push_back(item.item.itemGuid);
    }
    return listed;
}

inline KeepAnswer StockIgnoringGear(KeepItemIn const& item, KeepBotIn const&)
{
    if (!item.auctionable || !StockUnits(item) || item.auctionWorth <= item.vendorPayout)
        return {};
    return {true, item.auctionWorth, STOCK_LATER_LEVEL, AUCTION_STOCK, true};
}

inline KeepAnswer NoisyStock(KeepItemIn const& item, KeepBotIn const& bot)
{
    static uint32_t state = 1;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    KeepItemIn noisy = item;
    noisy.auctionWorth += state % 3;
    return AuctionStock(noisy, bot);
}

inline void CheckAuctionStock(Checker& checker)
{
    // 2. A listing worth no more than V is never kept: the case's linen at p = 3/4, 25/34 and 1/2
    checker.Check(CaseWorth(3, 4) == CASE_MEAN_AT_THREE_QUARTERS && CaseWorth(25, 34) == CASE_VENDOR_EXIT,
                  "2. case sale_expiry_threshold: listing is worth 262 at p = 3/4, the vendor's 260 at p = 25/34");
    KeepProvider const parity[] = {GearForLaterLevel, StockForLater, ProfessionMaterials, StockAtParity};
    struct Chance
    {
        uint32_t num;
        uint32_t den;
        bool kept;
    };
    bool parityCaught = false;
    bool certainCaught = false;
    for (Chance const chance : {Chance{3, 4, true}, Chance{25, 34, false}, Chance{1, 2, false}})
    {
        std::string const p = std::to_string(chance.num) + "/" + std::to_string(chance.den);
        uint32_t const worth = CaseWorth(chance.num, chance.den);
        CarriedView const view = ViewOf(20, {Stock(LINEN, LINEN_LOT, CASE_VENDOR_EXIT, worth)});
        KeepAnswer const answer = DecideKeep(view.in.bags[0].item, view.in.bot);
        checker.Show("20 linen at p " + p + ": vendor 260, listing worth " + std::to_string(worth), answer);
        checker.Note("  " + DescribeStop(view));
        bool const sold = Holds(SoldAtStop(view), LINEN);
        checker.Check(IsStock(answer) == chance.kept && answer.keeps == chance.kept && sold == !chance.kept &&
                          (!chance.kept || answer.laterWorth == worth),
                      "2. p " + p + (chance.kept ? ": stock, not sold" : ": not kept, sold at a vendor stop"));
        // Controls: a rule that keeps an equal worth, and a worth that ignores the expiry (the sale taken as certain)
        parityCaught = parityCaught || DecideKeep(view.in.bags[0].item, view.in.bot, parity).keeps != chance.kept;
        certainCaught =
            certainCaught ||
            DecideKeep(Stock(LINEN, LINEN_LOT, CASE_VENDOR_EXIT, CASE_SALE_NET), view.in.bot).keeps != chance.kept;
    }
    checker.Check(parityCaught && certainCaught,
                  "2. controls: keeping an equal worth, or a worth that ignores the expiry, keeps linen it must sell "
                  "(caught)");
    KeepItemIn dear = Stock(LINEN, LINEN_LOT, CASE_VENDOR_EXIT, CaseWorth(3, 4));
    dear.sellNow = CASE_VENDOR_EXIT * 5 / 2;
    KeepAnswer const multiplied = DecideKeep(dear, Bot(20));
    checker.Show("the linen at p 3/4 for a bot whose sell multiplier is 250 % (sell worth 650)", multiplied);
    checker.Check(IsStock(multiplied), "stock is unsold: V decided it, the bots' sell multiplier does not veto it");

    // 3. A worn item is judged by its own payout: the same ask lists against 0 or 1 copper, not against 6
    uint32_t const fresh = AuctionRules::VendorPayout(BELT_SELL, 1, 0, BELT_MULTIPLIER, BELT_QUALITY_MOD);
    uint32_t const equal = AuctionRules::VendorPayout(BELT_SELL, 1, BELT_WEAR_EQUAL, BELT_MULTIPLIER, BELT_QUALITY_MOD);
    uint32_t const over = AuctionRules::VendorPayout(BELT_SELL, 1, BELT_WEAR_OVER, BELT_MULTIPLIER, BELT_QUALITY_MOD);
    checker.Check(fresh == BELT_SELL && equal == BELT_PAID_EQUAL && over == BELT_PAID_OVER,
                  "3. case gear_trade.sell_boundary: a vendor pays 6 fresh, 0 worn by 7, 1 worn by 8");
    struct Wear
    {
        uint32_t lost;
        uint32_t payout;
        bool kept;
    };
    bool beltCaught = false;
    for (Wear const wear :
         {Wear{0, fresh, false}, Wear{BELT_WEAR_EQUAL, equal, true}, Wear{BELT_WEAR_OVER, over, true}})
    {
        KeepAnswer const answer = DecideKeep(Belt(wear.payout), Bot(20));
        checker.Show("Outfitter Belt worn by " + std::to_string(wear.lost) + ": vendor pays " +
                         std::to_string(wear.payout) + ", listing worth " + std::to_string(BeltWorth(wear.payout)),
                     answer);
        checker.Check(IsStock(answer) == wear.kept, "3. the belt worn by " + std::to_string(wear.lost) +
                                                        (wear.kept ? " is stock" : " is sold at a vendor"));
        // Control: judged by its full sell price, the worn belt sells like a fresh one
        beltCaught = beltCaught || (wear.kept && !DecideKeep(Belt(BELT_SELL), Bot(20)).keeps);
    }
    checker.Check(beltCaught,
                  "3. control: judged by the full sell price, a worn belt that should be stock is sold "
                  "(caught)");

    // 5. A bot with a master sells as before: RefreshCarriedKeeps returns before reading for a bot that keeps no
    // possessions (BotKind), so its memo stays empty
    std::vector<KeepItemIn> const bags = {Stock(LINEN, LINEN_LOT, CASE_VENDOR_EXIT, CaseWorth(3, 4)), Belt(equal),
                                          Gear(1101, 23)};
    CarriedView master;
    master.in = MakeIn(20, bags, {}, 0, BANK_ITEM_SLOTS);
    CarriedView const persistent = ViewOf(20, bags);
    checker.Note("a bot with a master, " + DescribeStop(master));
    checker.Note("the same bags of a persistent bot, " + DescribeStop(persistent));
    checker.Check(SoldAtStop(master).size() == bags.size(), "5. a bot with a master sells every bag item as before");
    checker.Check(SoldAtStop(persistent).size() < bags.size(),
                  "5. control: with the persistent bot's memo the list differs (caught)");

    // A sample bag of 8 at level 20 with smithing 100: who keeps what, and the sale list
    KeepItemIn worn = Gear(1202, 20);
    worn.wearAnswer = WearAnswer::ItemUsage;
    worn.usage = USAGE_EQUIP;
    KeepItemIn recipe = RecipeItem(1203, SMITHING, SMITH_SKILL + 10);
    KeepItemIn known = RecipeItem(1204, SMITHING, SMITH_SKILL - 20, true);
    for (KeepItemIn* item : {&worn, &recipe, &known})
    {
        item->auctionable = true;
        item->vendorPayout = SELL;
        item->auctionWorth = SELL * 3;
    }
    std::vector<KeepItemIn> const sample = {Gear(1201, 23),
                                            worn,
                                            recipe,
                                            known,
                                            UsedMaterial(1205, 10, 20, SMITH_SKILL + 50),
                                            Stock(LINEN, LINEN_LOT, CASE_VENDOR_EXIT, CaseWorth(3, 4)),
                                            Stock(1206, LINEN_LOT, CASE_VENDOR_EXIT, CaseWorth(1, 2)),
                                            Belt(equal)};
    CarriedView mixed;
    mixed.in = SmithIn(SMITH_SKILL, sample, {});
    mixed.carried = KeptInBags(mixed.in);
    checker.Note(
        "sample bag at level 20, smithing 100: 1201 upgrade for 23, 1202 worn now, 1203 recipe in reach, "
        "1204 known recipe, 1205 path material, 2589 linen at p 3/4, 1206 at p 1/2, 2186 worn belt");
    std::vector<char const*> providers;
    for (BankItemIn const& item : mixed.in.bags)
    {
        KeepAnswer const answer = DecideKeep(item.item, mixed.in.bot);
        checker.Show("  " + std::to_string(item.item.entry), answer);
        providers.push_back(answer.keeps ? answer.provider : "");
    }
    checker.Note("  " + DescribeStop(mixed));
    std::vector<uint32_t> const sold = SoldAtStop(mixed);
    checker.Check(std::string(providers[0]) == "gear_for_level" && std::string(providers[1]) == "gear_for_level" &&
                      std::string(providers[2]) == "materials" && std::string(providers[3]) == AUCTION_STOCK &&
                      std::string(providers[4]) == "materials" && std::string(providers[5]) == AUCTION_STOCK &&
                      std::string(providers[6]).empty() && std::string(providers[7]) == AUCTION_STOCK,
                  "7. sample: gear and a reachable recipe keep their providers, the known recipe, the linen at 3/4 "
                  "and the worn belt are stock");
    checker.Check(sold == std::vector<uint32_t>{1202, 1206},
                  "sample: sold at a vendor stop only the gear the usage equips first and the linen at p 1/2");

    // "50 linen, the path uses 20" (WA3-4 review B1, auction house WA7b): the visit lists the units beyond the path's
    // use, never the path's. Stacks of 20, 20 and 10 hold no more than the path uses: the path's. The stack of 50
    // lists its 30 units beyond for 700 against their payout and keeps the path's 20 with the path's due
    KeepBotIn const smith = SmithIn(SMITH_SKILL, {}, {}).bot;
    bool pathOnly = true;
    std::vector<KeepItemIn> linen;
    for (uint32_t const count : {20u, 20u, 10u, 50u})
    {
        KeepItemIn stack = UsedMaterial(LINEN, count, 20, SMITH_SKILL + 50);
        stack.itemGuid = 100 + static_cast<uint32_t>(linen.size());
        stack.stackMax = 100;
        stack.auctionable = true;
        stack.vendorPayout = StockUnits(stack) * CASE_VENDOR_EXIT / LINEN_LOT;
        stack.auctionWorth = count == 50 ? 700 : stack.vendorPayout * 2;
        KeepAnswer const answer = DecideKeep(stack, smith);
        KeepAnswer const path = PathMaterial(stack, smith);
        checker.Show("50 linen, the path uses 20: a stack of " + std::to_string(count) + ", listing worth " +
                         std::to_string(stack.auctionWorth),
                     answer);
        pathOnly = pathOnly && answer.keeps && answer.unsold && answer.dueLevel == path.dueLevel &&
                   answer.dueSkill == path.dueSkill &&
                   std::string(answer.provider) == (count > 20 ? AUCTION_STOCK : "materials") &&
                   (count > 20 || !AuctionStock(stack, smith).keeps);
        linen.push_back(stack);
    }
    checker.Check(pathOnly, "B1. a stack within the path's use is the path's; the 50 is stock with the path's due");
    BankIn const linenBags = SmithIn(SMITH_SKILL, linen, {});
    CarriedKeeps const linenKept = KeptInBags(linenBags);
    std::vector<uint32_t> const linenListed = ListedAtVisit(linenBags, linenKept);
    KeepProvider const unusedOnly[] = {GearForLaterLevel, StockForLater, ProfessionMaterials, StockOfUnusedEntries};
    std::vector<uint32_t> const wholeListed = ListedAtVisit(linenBags, KeptInBags(linenBags, unusedOnly));
    checker.Note("  the visit lists " + std::to_string(linenListed.size()) + " of the 4 linen stacks (" +
                 std::to_string(StockUnits(linen[3])) + " units of the 50), keeping " +
                 std::to_string(LeftAfterListing(linenBags, linenKept, LINEN)) + " of 100; the WA3-4 rule lists " +
                 std::to_string(wholeListed.size()));
    checker.Check(linenListed == std::vector<uint32_t>{103} && StockUnits(linen[3]) == 30 &&
                      LeftAfterListing(linenBags, linenKept, LINEN) == 70 && wholeListed.empty(),
                  "B1. the visit lists the 30 units beyond the path's use of the 50, no unit of the others");

    // The single stack of 50 the path uses 20 of, due now: it stays in the bags (due, as the path's) and is listed;
    // listing it whole, as the WA3a visit did, starves the path
    KeepItemIn dueLinen = UsedMaterial(LINEN, 50, 20, 0);
    dueLinen.itemGuid = 110;
    dueLinen.stackMax = 100;
    dueLinen.auctionable = true;
    dueLinen.vendorPayout = 30 * CASE_VENDOR_EXIT / LINEN_LOT;
    dueLinen.auctionWorth = 700;
    BankIn const dueBags = SmithIn(SMITH_SKILL, {dueLinen}, {});
    CarriedKeeps const dueKept = KeptInBags(dueBags);
    KeepAnswer const dueAnswer = DecideKeep(dueLinen, smith);
    checker.Show("  linen 50 due now, the path uses 20", dueAnswer);
    checker.Check(IsDue(dueAnswer, smith) && IsStock(dueAnswer) &&
                      ListedAtVisit(dueBags, dueKept) == std::vector<uint32_t>{110} &&
                      LeftAfterListing(dueBags, dueKept, LINEN) == 20,
                  "a stack beyond a due path's use stays in the bags and lists its 30 units, the path's 20 kept");
    checker.Check(LeftAfterListing(dueBags, dueKept, LINEN, true) < 20,
                  "control: listing the whole stack leaves the path short (caught)");

    // The visit picks a stack by its own keep: of two stacks of one entry, the one kept as use stock for later stays,
    // the one kept as auction stock is listed; WA3b's pick by the entry lists both
    KeepItemIn useStock = Stock(LINEN, LINEN_LOT, CASE_VENDOR_EXIT, CaseWorth(3, 4));
    useStock.itemGuid = 201;
    useStock.stockWorthEach = 100;
    useStock.stockLater = true;
    KeepItemIn listing = Stock(LINEN, LINEN_LOT, CASE_VENDOR_EXIT, CaseWorth(3, 4));
    listing.itemGuid = 202;
    CarriedView const mixedEntry = ViewOf(20, {useStock, listing});
    std::vector<uint32_t> const ownListed = ListedAtVisit(mixedEntry.in, mixedEntry.carried);
    std::vector<uint32_t> const entryListed = ListedAtVisit(mixedEntry.in, mixedEntry.carried, true);
    checker.Show("  linen kept as use stock for later", DecideKeep(useStock, mixedEntry.in.bot));
    checker.Show("  linen of the same entry", DecideKeep(listing, mixedEntry.in.bot));
    checker.Check(ownListed == std::vector<uint32_t>{202},
                  "B1. the visit lists the stack kept as auction stock, not its entry's stack kept as use stock");
    checker.Check(entryListed.size() == 2, "B1. control: picking by the entry lists the use stock too (caught)");
}

// The seeded search over random bags (invariants 1, 4, 6 and 7)
struct StockTally
{
    uint32_t kept = 0;
    uint32_t pathViolations = 0;   // 1: stock of an entry the path uses
    uint32_t worthViolations = 0;  // 4: stock worth more than its held units, or more units kept than held
    uint32_t gearViolations = 0;   // 7: stock of gear the gear rule keeps or a recipe the bot will learn; a keep lost
    uint32_t starved = 0;          // 1: an entry the visit leaves with fewer units than the path uses (WA7b)
    std::vector<uint32_t> list;    // 6: every decision, in order
};

inline StockTally StockSearch(uint32_t seed, KeepProvider stock, bool pathBlind)
{
    constexpr uint32_t ROUNDS = 2000;
    uint32_t state = seed ? seed : 1;
    auto next = [&state](uint32_t range) -> uint32_t
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return range ? state % range : 0;
    };
    KeepProvider const providers[] = {GearForLaterLevel, StockForLater, ProfessionMaterials, stock};
    KeepProvider const before[] = {GearForLaterLevel, StockForLater, ProfessionMaterials};
    StockTally tally;
    for (uint32_t round = 0; round < ROUNDS; ++round)
    {
        uint8_t const level = static_cast<uint8_t>(1 + next(80));
        std::vector<KeepItemIn> bags;
        std::vector<uint32_t> heldWorth;  // the worth of each stack's stock units (StockUnits)
        uint32_t const count = 1 + next(8);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t const entry = 6000 + next(6);  // repeated entries: several stacks of one item
            KeepItemIn item;
            switch (next(5))
            {
                case 0:
                    item =
                        UsedMaterial(entry, 1 + next(20), 1 + next(30), static_cast<uint16_t>(next(2) ? 0 : next(300)));
                    break;
                case 1:
                    item = Gear(entry, 0);
                    item.wearAnswer = WearAnswer::ItemUsage;
                    item.usage = static_cast<uint8_t>(next(5));
                    break;
                case 2:
                    item = Gear(entry, static_cast<uint8_t>(level + next(12)), static_cast<float>(next(80)));
                    break;
                case 3:
                    item = RecipeItem(entry, SMITHING, static_cast<uint16_t>(next(400)), next(4) == 0);
                    break;
                default:
                    item.entry = entry;
                    item.count = 1 + next(20);
                    item.stackMax = STACK_20;
                    item.sellNow = SELL;
                    break;
            }
            item.itemGuid = round * 16 + i + 1;
            uint32_t const units = pathBlind ? std::max<uint32_t>(item.count, 1) : StockUnits(item);
            uint32_t const worthEach = next(80);
            item.auctionable = next(6) != 0;
            item.vendorPayout = (1 + next(40)) * units;
            item.auctionWorth = worthEach * units;
            bags.push_back(item);
            heldWorth.push_back(worthEach * StockUnits(item));
        }
        BankIn in = MakeIn(level, bags, {}, 4, BANK_ITEM_SLOTS);
        if (next(2))
            in.bot.skills = {{SMITHING, static_cast<uint16_t>(1 + next(300))}};
        for (size_t i = 0; i < in.bags.size(); ++i)
        {
            KeepItemIn const& item = in.bags[i].item;
            KeepAnswer const answer = stock(item, in.bot);
            KeepAnswer const decided = DecideKeep(item, in.bot, providers);
            KeepAnswer const old = DecideKeep(item, in.bot, before);
            bool const isStock = IsStock(decided);
            bool const keptToUse =
                GearForLaterLevel(item, in.bot).keeps || (item.recipeSkill && RecipeForLater(item, in.bot).keeps);
            tally.kept += isStock ? 1 : 0;
            tally.pathViolations += answer.keeps && !StockUnits(item);
            // A stack beyond its path's use is worth the path's units too (AuctionStock, WA7b)
            uint32_t const pathWorth = item.materialUsed ? PathMaterial(item, in.bot).laterWorth : 0;
            tally.worthViolations += answer.keeps && answer.laterWorth > heldWorth[i] + pathWorth;
            tally.gearViolations += (answer.keeps && keptToUse) || (old.keeps && !decided.keeps) ||
                                    (!isStock && (decided.keeps != old.keeps || decided.laterWorth != old.laterWorth ||
                                                  decided.dueLevel != old.dueLevel));
            tally.list.push_back(decided.keeps ? decided.laterWorth + 1 : 0);
        }
        CarriedKeeps const carried = KeptInBags(in);
        CarriedKeeps const listed = KeptInBags(in, providers);
        // An entry the path uses, every stack of it read with the path's units (as the bags' reader reads them)
        for (BankItemIn const& item : in.bags)
        {
            uint32_t held = 0;
            bool same = true;
            for (BankItemIn const& other : in.bags)
                if (other.item.entry == item.item.entry)
                {
                    held += std::max<uint32_t>(other.item.count, 1);
                    same = same && other.item.materialUsed == item.item.materialUsed;
                }
            tally.starved +=
                same && item.item.materialUsed &&
                LeftAfterListing(in, listed, item.item.entry, pathBlind) < std::min(held, item.item.materialUsed);
        }
        for (KeptEntry const& kept : carried.items)
        {
            uint32_t held = 0;
            uint32_t memo = 0;
            for (BankItemIn const& item : in.bags)
                held += item.item.entry == kept.entry ? item.item.count : 0;
            for (KeptEntry const& other : carried.items)
                memo += other.entry == kept.entry ? other.count : 0;
            tally.worthViolations += memo > held;
        }
    }
    return tally;
}

inline void CheckStockSearch(Checker& checker, uint32_t seed)
{
    StockTally const rule = StockSearch(seed, AuctionStock, false);
    StockTally const again = StockSearch(seed, AuctionStock, false);
    StockTally const pathBlind = StockSearch(seed, StockIgnoringPath, true);
    StockTally const whole = StockSearch(seed, StockOfFullStack, false);
    StockTally const gearBlind = StockSearch(seed, StockIgnoringGear, false);
    StockTally const noisy = StockSearch(seed, NoisyStock, false);
    StockTally const noisyAgain = StockSearch(seed, NoisyStock, false);
    checker.Note("stock search (seed " + std::to_string(seed) + ", 2000 bags): " + std::to_string(rule.kept) +
                 " stacks stock; violations path " + std::to_string(rule.pathViolations) + ", held " +
                 std::to_string(rule.worthViolations) + ", gear and recipes " + std::to_string(rule.gearViolations) +
                 "; controls: path blind " + std::to_string(pathBlind.pathViolations) + ", full stack " +
                 std::to_string(whole.worthViolations) + ", gear blind " + std::to_string(gearBlind.gearViolations));
    checker.Check(rule.kept > 0 && rule.pathViolations == 0, "1. an entry a skill path uses is never stock");
    checker.Check(pathBlind.pathViolations > 0, "1. control: a rule blind to the path's use makes them stock (caught)");
    checker.Check(rule.worthViolations == 0, "4. stock never exceeds what is held, in units or worth");
    checker.Check(whole.worthViolations > 0, "4. control: valuing a stack as a full one (caught)");
    checker.Note("  the visit's listings leave a path short: " + std::to_string(rule.starved) +
                 " bag entries; control listing whole stacks: " + std::to_string(pathBlind.starved));
    checker.Check(rule.starved == 0, "1. the visit never lists a unit the skill path uses (WA7b)");
    checker.Check(pathBlind.starved > 0, "1. control: listing the stacks of the path's entries whole (caught)");
    checker.Check(rule.gearViolations == 0,
                  "7. gear the gear rule keeps and a recipe the bot will learn are never stock; other keeps unchanged");
    checker.Check(gearBlind.gearViolations > 0, "7. control: a rule blind to the gear rule (caught)");
    checker.Check(rule.list == again.list, "6. the same input gives the same list");
    checker.Check(noisy.list != noisyAgain.list, "6. control: a rule reading a random number gives two lists (caught)");
}
}  // namespace BankSelftestDetail

// Runs every scenario; returns the checks run and failed
inline BankSelftestDetail::Result RunBankRulesSelftest(std::vector<std::string>& lines)
{
    using namespace BankSelftestDetail;
    Result result;
    Checker checker(lines, result);
    CheckHorizon(checker);
    CheckUpgrade(checker);
    CheckRule(checker);
    CheckProviders(checker);
    CheckDue(checker);
    CheckDueUsage(checker);
    CheckReleased(checker);
    CheckBankFull(checker);
    CheckNothing(checker);
    CheckOrder(checker);
    CheckSwap(checker);
    CheckStacks(checker);
    CheckRefused(checker);
    CheckSettled(checker);
    CheckReplan(checker);
    CheckDueAfterLevelChange(checker);
    CheckCarried(checker);
    CheckCarriedReleased(checker);
    CheckCarriedSpace(checker);
    CheckCarriedSearch(checker, CARRIED_SEARCH_SEED);
    CheckStock(checker);
    CheckStockChurn(checker);
    CheckMaterials(checker);
    CheckMaterialSearch(checker, MATERIAL_SEARCH_SEED);
    CheckBankSpace(checker);
    CheckAuctionStock(checker);
    CheckStockSearch(checker, STOCK_SEARCH_SEED);
    lines.push_back("BANK selftest: " + std::to_string(result.checks) + " checks, " + std::to_string(result.failed) +
                    " failed" + (result.failed ? "" : " (PASS)"));
    return result;
}
}  // namespace BankMail

#endif
