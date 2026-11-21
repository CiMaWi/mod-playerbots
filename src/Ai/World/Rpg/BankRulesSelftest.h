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

#include "BankRules.h"
#include "ItemWorthRules.h"

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
    checker.Check(std::size(KEEP_PROVIDERS) == 3 && KEEP_PROVIDERS[0] == GearForLaterLevel &&
                      KEEP_PROVIDERS[1] == StockForLater && KEEP_PROVIDERS[2] == ProfessionMaterials,
                  "the server asks three providers: gear for a later level, use stock, then profession materials");
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
    checker.Check(GuidIs(alone, 33, BankChoice::Deposit, BankReason::KeptStock),
                  "a surplus potion within the share is stock for later");

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
    CheckCarried(checker);
    CheckCarriedReleased(checker);
    CheckCarriedSpace(checker);
    CheckCarriedSearch(checker, CARRIED_SEARCH_SEED);
    CheckStock(checker);
    CheckMaterials(checker);
    CheckMaterialSearch(checker, MATERIAL_SEARCH_SEED);
    lines.push_back("BANK selftest: " + std::to_string(result.checks) + " checks, " + std::to_string(result.failed) +
                    " failed" + (result.failed ? "" : " (PASS)"));
    return result;
}
}  // namespace BankMail

#endif
