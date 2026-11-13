/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MAILRULES_H
#define _PLAYERBOT_MAILRULES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// The Player-free rules of a persistent bot's mailbox visit (bank-mail plan, WB.4 and WB.5): what it takes from its
// mail and in which order, and what a mail errand is worth. Header-only so tests/mail-rules-selftest.cpp checks the
// same code the server runs. All worth is in copper.
//
// The visit uses (signatures kept stable):
//   VisitPlan PlanMailVisit(std::vector<MailIn> const& mails, BagSpace const& space, MailOptions const& options);
//   uint32_t MailErrandWorth(uint32_t waiting, uint32_t defaultWorth, uint32_t seen, uint32_t expected,
//                            float orderlinessWeight);
// MailRulesInput.h turns the live mail list into these inputs (BuildMailIn, BuildBagSpace, BuildMailOptions). The
// plan is recomputed from the live list after each answer of the server, so a changed list never leaves a stale plan;
// a take the server refuses goes into MailOptions so the next plan leaves that item or money in the mail.
namespace BankMail
{
constexpr uint32_t MAIL_WORTH_CAP = std::numeric_limits<uint32_t>::max();
// The orderliness weight until the traits supply one (bank-mail WB8)
constexpr float DEFAULT_ORDERLINESS_WEIGHT = 1.0f;
// The client's inbox shows this many mails
constexpr uint32_t DEFAULT_MAILS_PER_VISIT = 50;

struct MailItemIn
{
    uint32_t itemGuid = 0;  // the item's low guid, the key of a take
    uint32_t entry = 0;
    uint32_t count = 1;
    uint32_t stackMax = 1;
    uint32_t worth = 0;     // copper, finite
    bool priority = false;  // an upgrade, quest or kept item: taken before every other item
};

struct MailIn
{
    uint32_t id = 0;
    bool delivered = true;  // its delivery time has come
    bool cod = false;       // cash on delivery: never paid
    bool deleted = false;
    bool unread = true;
    bool hasText = false;
    uint32_t money = 0;
    std::vector<MailItemIn> items;
};

// How many more of an entry fit on the partial stacks in the bot's bags
struct StackRoom
{
    uint32_t entry = 0;
    uint32_t room = 0;
};

struct BagSpace
{
    uint32_t freeSlots = 0;  // empty slots of the backpack and the plain bags
    std::vector<StackRoom> stackRoom;
};

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
    Later,  // beyond the mails one visit opens
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

struct VisitPlan
{
    std::vector<Step> steps;            // in order: mark read, money, items by worth, deletes
    std::vector<MailVerdict> verdicts;  // one per mail, in list order
    uint32_t mailsOpened = 0;
    uint32_t money = 0;
    uint32_t itemsTaken = 0;
    uint32_t itemsLeft = 0;
    uint32_t deletes = 0;
    uint32_t seenAfter = 0;  // worth left in the opened mails: the bot's seen worth after the visit
};

inline uint32_t AddSaturating(uint32_t sum, uint32_t value)
{
    return value > MAIL_WORTH_CAP - sum ? MAIL_WORTH_CAP : sum + value;
}

inline char const* ChoiceWord(MailChoice choice)
{
    switch (choice)
    {
        case MailChoice::Take:
            return "take";
        case MailChoice::Leave:
            return "leave";
        default:
            return "skip";
    }
}

inline char const* ReasonWord(MailReason reason)
{
    switch (reason)
    {
        case MailReason::Emptied:
            return "emptied";
        case MailReason::NoSpace:
            return "no_space";
        case MailReason::Refused:
            return "refused";
        case MailReason::Cod:
            return "cod";
        case MailReason::NotDelivered:
            return "not_delivered";
        case MailReason::Later:
            return "later";
        default:
            return "deleted";
    }
}

namespace MailRulesDetail
{
inline bool Contains(std::vector<uint32_t> const& values, uint32_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Partial stacks first, then one free slot per item; the rest of a new stack takes later items of its entry. The
// server's CanStoreItem has the last word (a refused take comes back through MailOptions::refusedItems).
inline bool Fits(BagSpace& space, MailItemIn const& item)
{
    auto stack = std::find_if(space.stackRoom.begin(), space.stackRoom.end(),
                              [&item](StackRoom const& room) { return room.entry == item.entry; });
    uint32_t const room = stack == space.stackRoom.end() ? 0 : stack->room;
    if (room >= item.count)
    {
        stack->room -= item.count;
        return true;
    }

    if (!space.freeSlots)
        return false;

    --space.freeSlots;
    uint32_t const newRoom = room + (item.stackMax > item.count ? item.stackMax - item.count : 0);
    if (stack != space.stackRoom.end())
        stack->room = newRoom;
    else if (newRoom)
        space.stackRoom.push_back({item.entry, newRoom});
    return true;
}

struct Candidate
{
    size_t mail = 0;
    MailItemIn const* item = nullptr;
};
}  // namespace MailRulesDetail

// WB.5: mail not yet delivered and cash-on-delivery mail are left alone; money first (it needs no space); then items
// by worth, highest first, while they fit; an item that does not fit stays and its worth is seen; a mail with nothing
// left in it (text only included) is marked read and deleted; no bag item is ever destroyed for mail.
inline VisitPlan PlanMailVisit(std::vector<MailIn> const& mails, BagSpace const& space, MailOptions const& options)
{
    using namespace MailRulesDetail;

    VisitPlan plan;
    plan.verdicts.resize(mails.size());
    std::vector<bool> opened(mails.size(), false);
    for (size_t i = 0; i < mails.size(); ++i)
    {
        MailIn const& mail = mails[i];
        MailVerdict& verdict = plan.verdicts[i];
        verdict.mailId = mail.id;
        if (mail.deleted)
            verdict.reason = MailReason::Deleted;
        else if (!mail.delivered)
            verdict.reason = MailReason::NotDelivered;
        else if (mail.cod)
            verdict.reason = MailReason::Cod;
        else if (plan.mailsOpened < options.mailsPerVisit)
        {
            opened[i] = true;
            ++plan.mailsOpened;
            verdict.choice = MailChoice::Take;
            verdict.reason = MailReason::Emptied;
            if (mail.unread)
                plan.steps.push_back({StepKind::MarkRead, mail.id, 0});
        }
    }

    for (size_t i = 0; i < mails.size(); ++i)
    {
        if (!opened[i] || !mails[i].money)
            continue;
        MailVerdict& verdict = plan.verdicts[i];
        if (Contains(options.refusedMoney, mails[i].id))
        {
            verdict.choice = MailChoice::Leave;
            verdict.reason = MailReason::Refused;
            verdict.worthLeft = AddSaturating(verdict.worthLeft, mails[i].money);
            plan.seenAfter = AddSaturating(plan.seenAfter, mails[i].money);
            continue;
        }
        plan.steps.push_back({StepKind::TakeMoney, mails[i].id, 0});
        verdict.moneyTaken = mails[i].money;
        plan.money = AddSaturating(plan.money, mails[i].money);
    }

    std::vector<Candidate> candidates;
    for (size_t i = 0; i < mails.size(); ++i)
        if (opened[i])
            for (MailItemIn const& item : mails[i].items)
                candidates.push_back({i, &item});
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](Candidate const& left, Candidate const& right)
                     {
                         if (left.item->priority != right.item->priority)
                             return left.item->priority;
                         return left.item->worth > right.item->worth;
                     });

    BagSpace left = space;
    for (Candidate const& candidate : candidates)
    {
        MailVerdict& verdict = plan.verdicts[candidate.mail];
        MailItemIn const& item = *candidate.item;
        bool const refused = Contains(options.refusedItems, item.itemGuid);
        if (!refused && Fits(left, item))
        {
            plan.steps.push_back({StepKind::TakeItem, verdict.mailId, item.itemGuid});
            ++verdict.itemsTaken;
            ++plan.itemsTaken;
            continue;
        }

        ++verdict.itemsLeft;
        ++plan.itemsLeft;
        verdict.worthLeft = AddSaturating(verdict.worthLeft, item.worth);
        plan.seenAfter = AddSaturating(plan.seenAfter, item.worth);
        verdict.choice = MailChoice::Leave;
        if (verdict.reason != MailReason::NoSpace)
            verdict.reason = refused ? MailReason::Refused : MailReason::NoSpace;
    }

    for (size_t i = 0; i < mails.size(); ++i)
    {
        if (!opened[i] || plan.verdicts[i].choice != MailChoice::Take)
            continue;
        plan.steps.push_back({StepKind::Delete, mails[i].id, 0});
        ++plan.deletes;
    }

    return plan;
}

// The worth of a mail errand: what unopened mail is expected to hold, plus what the bot saw and left, plus what it
// expects from its own sales, scaled by its orderliness. The default per mail is small, so unopened mail alone joins
// a stop but earns no detour.
inline uint32_t MailErrandWorth(uint32_t waiting, uint32_t defaultWorth, uint32_t seen, uint32_t expected,
                                float orderlinessWeight)
{
    if (!(orderlinessWeight > 0.0f))
        return 0;

    double const base = static_cast<double>(waiting) * defaultWorth + seen + expected;
    double const scaled = base * orderlinessWeight;
    return scaled >= static_cast<double>(MAIL_WORTH_CAP) ? MAIL_WORTH_CAP : static_cast<uint32_t>(scaled);
}

// One line of what a plan does, for the selftest and the command
inline std::string DescribePlan(VisitPlan const& plan)
{
    return "open " + std::to_string(plan.mailsOpened) + ", money " + std::to_string(plan.money) + ", take " +
           std::to_string(plan.itemsTaken) + " items, leave " + std::to_string(plan.itemsLeft) + ", delete " +
           std::to_string(plan.deletes) + ", seen after " + std::to_string(plan.seenAfter) + ", " +
           std::to_string(plan.steps.size()) + " steps";
}

inline std::string DescribeVerdict(MailVerdict const& verdict)
{
    return "mail " + std::to_string(verdict.mailId) + ": " + ChoiceWord(verdict.choice) + " " +
           ReasonWord(verdict.reason) + ", money " + std::to_string(verdict.moneyTaken) + ", items " +
           std::to_string(verdict.itemsTaken) + " taken " + std::to_string(verdict.itemsLeft) + " left (worth " +
           std::to_string(verdict.worthLeft) + ")";
}
}  // namespace BankMail

#endif
