/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MAILRULESSELFTEST_H
#define _PLAYERBOT_MAILRULESSELFTEST_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "MailRules.h"

// The scenarios of the mail rules (MailRules.h), shared by tests/mail-rules-selftest.cpp and the console's
// "playerbots mail selftest". Each scenario adds its decisions to lines; a failed check adds a "FAIL:" line.
namespace BankMail
{
namespace MailSelftestDetail
{
constexpr uint32_t GOLD = 10000;
constexpr uint32_t DEFAULT_WORTH = 100;
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

    void Show(std::string const& scenario, VisitPlan const& plan)
    {
        _lines.push_back(scenario + ": " + DescribePlan(plan));
        for (MailVerdict const& verdict : plan.verdicts)
            _lines.push_back("  " + DescribeVerdict(verdict));
    }

private:
    std::vector<std::string>& _lines;
    Result& _result;
};

inline MailItemIn MakeItem(uint32_t guid, uint32_t worth, uint32_t entry = 0, uint32_t count = 1, uint32_t stackMax = 1)
{
    MailItemIn item;
    item.itemGuid = guid;
    item.entry = entry ? entry : guid;
    item.count = count;
    item.stackMax = stackMax;
    item.worth = worth;
    return item;
}

inline MailIn MakeMail(uint32_t id, uint32_t money, std::vector<MailItemIn> items = {})
{
    MailIn mail;
    mail.id = id;
    mail.money = money;
    mail.items = std::move(items);
    return mail;
}

inline uint32_t CountSteps(VisitPlan const& plan, StepKind kind)
{
    uint32_t count = 0;
    for (Step const& step : plan.steps)
        if (step.kind == kind)
            ++count;
    return count;
}

inline bool HasStep(VisitPlan const& plan, StepKind kind, uint32_t mailId, uint32_t itemGuid = 0)
{
    for (Step const& step : plan.steps)
        if (step.kind == kind && step.mailId == mailId && step.itemGuid == itemGuid)
            return true;
    return false;
}

// The index of the first step of a kind, or the step count
inline size_t FirstStep(VisitPlan const& plan, StepKind kind)
{
    for (size_t i = 0; i < plan.steps.size(); ++i)
        if (plan.steps[i].kind == kind)
            return i;
    return plan.steps.size();
}

inline size_t LastStep(VisitPlan const& plan, StepKind kind)
{
    size_t last = 0;
    for (size_t i = 0; i < plan.steps.size(); ++i)
        if (plan.steps[i].kind == kind)
            last = i;
    return last;
}

inline BagSpace Space(uint32_t freeSlots) { return BagSpace{freeSlots, {}}; }

inline void CheckFits(Checker& checker)
{
    std::vector<MailIn> const mails = {MakeMail(1, 5 * GOLD, {MakeItem(11, 300), MakeItem(12, 200)}),
                                       MakeMail(2, GOLD, {MakeItem(21, 50)})};
    VisitPlan const plan = PlanMailVisit(mails, Space(10), MailOptions{});
    checker.Show("money and items that fit", plan);
    checker.Check(plan.mailsOpened == 2 && plan.money == 6 * GOLD && plan.itemsTaken == 3 && plan.itemsLeft == 0,
                  "every mail opened, all money and items taken");
    checker.Check(CountSteps(plan, StepKind::MarkRead) == 2, "each unread mail is marked read");
    checker.Check(LastStep(plan, StepKind::TakeMoney) < FirstStep(plan, StepKind::TakeItem), "money before items");
    checker.Check(plan.deletes == 2 && FirstStep(plan, StepKind::Delete) > LastStep(plan, StepKind::TakeItem),
                  "both emptied mails deleted after the takes");
    checker.Check(plan.seenAfter == 0, "nothing left: nothing seen");
    checker.Check(plan.verdicts[0].choice == MailChoice::Take && plan.verdicts[0].reason == MailReason::Emptied,
                  "an emptied mail's verdict is take/emptied");
}

inline void CheckShortSpace(Checker& checker)
{
    std::vector<MailIn> const mails = {MakeMail(1, 0, {MakeItem(11, 40), MakeItem(12, 900)}),
                                       MakeMail(2, 0, {MakeItem(21, 500), MakeItem(22, 70)})};
    VisitPlan const plan = PlanMailVisit(mails, Space(2), MailOptions{});
    checker.Show("more items than space", plan);
    checker.Check(
        HasStep(plan, StepKind::TakeItem, 1, 12) && HasStep(plan, StepKind::TakeItem, 2, 21) && plan.itemsTaken == 2,
        "the two highest-worth items are taken, across mails");
    checker.Check(plan.steps.size() > 3 && plan.steps[2].itemGuid == 12 && plan.steps[3].itemGuid == 21,
                  "items are taken highest worth first");
    checker.Check(plan.itemsLeft == 2 && plan.seenAfter == 110, "the two left are seen: 40 + 70");
    checker.Check(plan.deletes == 0, "a mail that still holds an item is not deleted");
    checker.Check(plan.verdicts[0].choice == MailChoice::Leave && plan.verdicts[0].reason == MailReason::NoSpace,
                  "a mail with an item left is leave/no_space");

    std::vector<MailIn> priority = {MakeMail(3, 0, {MakeItem(31, 5000), MakeItem(32, 10)})};
    priority[0].items[1].priority = true;
    VisitPlan const upgrade = PlanMailVisit(priority, Space(1), MailOptions{});
    checker.Show("an upgrade before a dearer vendor item", upgrade);
    checker.Check(HasStep(upgrade, StepKind::TakeItem, 3, 32) && upgrade.seenAfter == 5000,
                  "a priority item is taken first, the vendor item seen");
}

inline void CheckLeftAlone(Checker& checker)
{
    MailIn cod = MakeMail(1, 0, {MakeItem(11, 800)});
    cod.cod = true;
    MailIn later = MakeMail(2, 3 * GOLD, {MakeItem(21, 800)});
    later.delivered = false;
    MailIn gone = MakeMail(3, GOLD);
    gone.deleted = true;
    VisitPlan const plan =
        PlanMailVisit({cod, later, gone, MakeMail(4, 0, {MakeItem(41, 1)})}, Space(8), MailOptions{});
    checker.Show("cash on delivery, not delivered, deleted", plan);
    checker.Check(plan.verdicts[0].choice == MailChoice::Skip && plan.verdicts[0].reason == MailReason::Cod,
                  "cash-on-delivery mail is skipped");
    checker.Check(plan.verdicts[1].choice == MailChoice::Skip && plan.verdicts[1].reason == MailReason::NotDelivered,
                  "mail not yet delivered is skipped");
    checker.Check(plan.verdicts[2].reason == MailReason::Deleted, "a deleted mail is skipped");
    bool touched = false;
    for (Step const& step : plan.steps)
        touched = touched || step.mailId != 4;
    checker.Check(!touched, "no step touches a skipped mail (no payment, no delete, no mark read)");
    checker.Check(plan.mailsOpened == 1 && plan.money == 0 && plan.seenAfter == 0,
                  "skipped mail is neither opened nor seen");
}

inline void CheckTextAndEmpty(Checker& checker)
{
    MailIn text = MakeMail(1, 0);
    text.hasText = true;
    MailIn readText = MakeMail(2, 0);
    readText.hasText = true;
    readText.unread = false;
    VisitPlan const plan = PlanMailVisit({text, readText}, Space(0), MailOptions{});
    checker.Show("text-only mail", plan);
    checker.Check(HasStep(plan, StepKind::MarkRead, 1) && HasStep(plan, StepKind::Delete, 1),
                  "unread text-only mail is marked read and deleted");
    checker.Check(!HasStep(plan, StepKind::MarkRead, 2) && HasStep(plan, StepKind::Delete, 2),
                  "read text-only mail is deleted");
    checker.Check(FirstStep(plan, StepKind::MarkRead) < FirstStep(plan, StepKind::Delete), "read before delete");

    VisitPlan const none = PlanMailVisit({}, Space(4), MailOptions{});
    checker.Show("an empty mail list", none);
    checker.Check(none.steps.empty() && none.verdicts.empty() && none.mailsOpened == 0 && none.seenAfter == 0,
                  "an empty list plans nothing");
}

inline void CheckMoneyAndBigItem(Checker& checker)
{
    MailIn mail = MakeMail(1, 2 * GOLD, {MakeItem(11, 700)});
    mail.unread = false;
    VisitPlan const plan = PlanMailVisit({mail}, Space(0), MailOptions{});
    checker.Show("money and an item that does not fit", plan);
    checker.Check(HasStep(plan, StepKind::TakeMoney, 1) && plan.money == 2 * GOLD, "the money is taken");
    checker.Check(plan.itemsTaken == 0 && plan.itemsLeft == 1 && plan.seenAfter == 700, "the item stays and is seen");
    checker.Check(plan.deletes == 0 && !HasStep(plan, StepKind::Delete, 1), "the mail is not deleted");

    VisitPlan const emptied = PlanMailVisit({MakeMail(2, GOLD, {MakeItem(21, 5)})}, Space(1), MailOptions{});
    checker.Show("mail emptied by the take", emptied);
    checker.Check(HasStep(emptied, StepKind::Delete, 2) && emptied.steps.back().kind == StepKind::Delete,
                  "mail emptied by the take is deleted last");
}

inline void CheckStacksAndRefusals(Checker& checker)
{
    constexpr uint32_t CLOTH = 2589;
    BagSpace space = Space(0);
    space.stackRoom.push_back({CLOTH, 15});
    std::vector<MailIn> const mails = {
        MakeMail(1, 0, {MakeItem(11, 60, CLOTH, 10, STACK_20), MakeItem(12, 60, CLOTH, 10, STACK_20)})};
    VisitPlan const plan = PlanMailVisit(mails, space, MailOptions{});
    checker.Show("stacks onto a partial stack, no free slot", plan);
    checker.Check(plan.itemsTaken == 1 && plan.itemsLeft == 1, "15 room takes one stack of 10, not two");

    space.freeSlots = 1;
    VisitPlan const slot = PlanMailVisit(mails, space, MailOptions{});
    checker.Check(slot.itemsTaken == 2, "one free slot and the stack room take both stacks");

    MailOptions refused;
    refused.refusedItems.push_back(11);
    refused.refusedMoney.push_back(2);
    VisitPlan const again =
        PlanMailVisit({MakeMail(1, 0, {MakeItem(11, 300), MakeItem(12, 30)}), MakeMail(2, GOLD)}, Space(4), refused);
    checker.Show("the server refused an item and money", again);
    checker.Check(!HasStep(again, StepKind::TakeItem, 1, 11) && HasStep(again, StepKind::TakeItem, 1, 12),
                  "a refused item is not tried again, the other is taken");
    checker.Check(!HasStep(again, StepKind::TakeMoney, 2) && !HasStep(again, StepKind::Delete, 2) &&
                      again.verdicts[1].reason == MailReason::Refused,
                  "refused money stays, its mail is kept");
    checker.Check(again.seenAfter == 300 + GOLD, "what stays after a refusal is seen");

    MailOptions few;
    few.mailsPerVisit = 1;
    VisitPlan const limited = PlanMailVisit({MakeMail(1, GOLD), MakeMail(2, GOLD)}, Space(4), few);
    checker.Check(limited.mailsOpened == 1 && limited.verdicts[1].reason == MailReason::Later &&
                      !HasStep(limited, StepKind::MarkRead, 2),
                  "mail beyond the visit's limit waits, unopened");
}

inline void CheckErrandWorth(Checker& checker, std::vector<std::string>& lines)
{
    uint32_t const none = MailErrandWorth(0, DEFAULT_WORTH, 0, 0, DEFAULT_ORDERLINESS_WEIGHT);
    uint32_t const one = MailErrandWorth(1, DEFAULT_WORTH, 0, 0, DEFAULT_ORDERLINESS_WEIGHT);
    uint32_t const many = MailErrandWorth(12, DEFAULT_WORTH, 0, 0, DEFAULT_ORDERLINESS_WEIGHT);
    uint32_t const seen = MailErrandWorth(2, DEFAULT_WORTH, 700, 0, DEFAULT_ORDERLINESS_WEIGHT);
    uint32_t const expected = MailErrandWorth(0, DEFAULT_WORTH, 0, 4 * GOLD, DEFAULT_ORDERLINESS_WEIGHT);
    uint32_t const tidy = MailErrandWorth(2, DEFAULT_WORTH, 700, 0, 1.5f);
    uint32_t const untidy = MailErrandWorth(2, DEFAULT_WORTH, 700, 0, 0.5f);
    uint32_t const huge = MailErrandWorth(MAIL_WORTH_CAP, MAIL_WORTH_CAP, MAIL_WORTH_CAP, MAIL_WORTH_CAP, 2.0f);
    uint32_t const negative = MailErrandWorth(5, DEFAULT_WORTH, 700, 0, -1.0f);
    lines.push_back("errand worth: 0 waiting " + std::to_string(none) + ", 1 waiting " + std::to_string(one) +
                    ", 12 waiting " + std::to_string(many) + ", 2 waiting + 700 seen " + std::to_string(seen) +
                    ", 4 gold expected " + std::to_string(expected) + ", tidy 1.5 " + std::to_string(tidy) +
                    ", untidy 0.5 " + std::to_string(untidy) + ", saturated " + std::to_string(huge));
    checker.Check(none == 0 && one == DEFAULT_WORTH && many == 12 * DEFAULT_WORTH, "waiting mail x the default");
    checker.Check(seen == 900 && expected == 4 * GOLD, "seen and expected add");
    checker.Check(tidy == 1350 && untidy == 450, "orderliness scales the worth");
    checker.Check(huge == MAIL_WORTH_CAP && negative == 0, "saturates; a weight at or below 0 gives 0");

    VisitPlan const plan = PlanMailVisit({MakeMail(1, 0, {MakeItem(11, MAIL_WORTH_CAP), MakeItem(12, MAIL_WORTH_CAP)})},
                                         Space(0), MailOptions{});
    checker.Check(plan.seenAfter == MAIL_WORTH_CAP && plan.verdicts[0].worthLeft == MAIL_WORTH_CAP,
                  "seen worth saturates");
}
}  // namespace MailSelftestDetail

// Runs every scenario; returns the checks run and failed
inline MailSelftestDetail::Result RunMailRulesSelftest(std::vector<std::string>& lines)
{
    using namespace MailSelftestDetail;
    Result result;
    Checker checker(lines, result);
    CheckFits(checker);
    CheckShortSpace(checker);
    CheckLeftAlone(checker);
    CheckTextAndEmpty(checker);
    CheckMoneyAndBigItem(checker);
    CheckStacksAndRefusals(checker);
    CheckErrandWorth(checker, lines);
    lines.push_back("MAIL selftest: " + std::to_string(result.checks) + " checks, " + std::to_string(result.failed) +
                    " failed" + (result.failed ? "" : " (PASS)"));
    return result;
}
}  // namespace BankMail

#endif
