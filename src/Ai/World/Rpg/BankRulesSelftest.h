/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKRULESSELFTEST_H
#define _PLAYERBOT_BANKRULESSELFTEST_H

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "BankRules.h"

// The scenarios of the bank rules (BankRules.h), shared by tests/bank-rules-selftest.cpp and the console's
// "playerbots bank selftest". Each scenario adds its decisions to lines; a failed check adds a "FAIL:" line.
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
    checker.Check(std::size(KEEP_PROVIDERS) == 1 && KEEP_PROVIDERS[0] == GearForLaterLevel,
                  "the server asks one provider: gear for a later level");
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
        Moves(refused, {{BankChoice::Withdraw, 902}}) && Is(refused, 901, BankChoice::Leave, BankReason::Refused),
        "a refused withdrawal stays in the bank and leaves its bag slot to the next");

    BankIn const deposit = MakeIn(10, {Gear(903, 13)}, {}, 0, BANK_ITEM_SLOTS);
    BankPlan const kept = PlanBankVisit(deposit, {903});
    checker.Show("a kept item whose deposit the core refused", kept);
    checker.Check(kept.steps.empty() && Is(kept, 903, BankChoice::Keep, BankReason::Refused) &&
                      kept.bankFreeAfter == BANK_ITEM_SLOTS,
                  "a refused deposit stays in the bags and takes no bank slot");
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
    CheckReleased(checker);
    CheckBankFull(checker);
    CheckNothing(checker);
    CheckOrder(checker);
    CheckSwap(checker);
    CheckStacks(checker);
    CheckRefused(checker);
    CheckReplan(checker);
    CheckDueAfterLevelChange(checker);
    lines.push_back("BANK selftest: " + std::to_string(result.checks) + " checks, " + std::to_string(result.failed) +
                    " failed" + (result.failed ? "" : " (PASS)"));
    return result;
}
}  // namespace BankMail

#endif
