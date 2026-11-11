/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MailVisit.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "BankMailState.h"
#include "CensusMgr.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Mail.h"
#include "MailRulesInput.h"
#include "Map.h"
#include "Opcodes.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace BankMail
{
namespace
{
// The bot looks for the mailbox once it is this close to the spawn (the spawn is the mailbox's own position)
constexpr float MAILBOX_SEARCH_YARDS = 20.0f;
// A mailbox packet: the mailbox guid and the mail id, some with one more uint32
constexpr size_t MAIL_PACKET_BYTES = sizeof(uint64) + sizeof(uint32);

std::atomic<uint32> visitsStarted{0};
std::atomic<uint32> visitsDone{0};
std::atomic<uint32> visitsTimedOut{0};
std::atomic<uint32> visitsWithoutMailbox{0};

bool Queue(Player* bot, OpcodeClient opcode, ObjectGuid const& mailbox, uint32 mailId, std::optional<uint32> last)
{
    WorldSession* session = bot ? bot->GetSession() : nullptr;
    if (!session)
        return false;

    // QueuePacket takes ownership; the world thread runs the handler when it drains the bot's session
    WorldPacket* packet = new WorldPacket(opcode, MAIL_PACKET_BYTES + (last ? sizeof(uint32) : 0));
    *packet << mailbox << mailId;
    if (last)
        *packet << *last;
    session->QueuePacket(packet);
    return true;
}

bool SameStep(Step const& left, Step const& right)
{
    return left.kind == right.kind && left.mailId == right.mailId && left.itemGuid == right.itemGuid;
}

// Mail list read: the bot's own map thread inside its AI update only (StepMailVisit; BankMailState.h)
bool StepShows(Player* bot, Step const& step)
{
    Mail const* mail = bot->GetMail(step.mailId);
    if (!mail || mail->state == MAIL_STATE_DELETED)
        return true;

    switch (step.kind)
    {
        case StepKind::MarkRead:
            return (mail->checked & MAIL_CHECK_MASK_READ) != 0;
        case StepKind::TakeMoney:
            return !mail->money;
        case StepKind::TakeItem:
            return std::none_of(mail->items.begin(), mail->items.end(),
                                [&step](MailItemInfo const& info) { return info.item_guid == step.itemGuid; });
        default:
            return false;
    }
}

// The awaited step's answer: a take the list does not show was refused and is left for the rest of the visit
void TakeAnswer(Player* bot, MailVisitState& visit)
{
    Step const& step = visit.awaiting;
    bool const shows = StepShows(bot, step);
    auto verdict = std::find_if(visit.verdicts.begin(), visit.verdicts.end(),
                                [&step](MailVerdict const& known) { return known.mailId == step.mailId; });
    if (step.kind == StepKind::TakeMoney)
    {
        uint32 const gained = bot->GetMoney() > visit.moneyBefore ? bot->GetMoney() - visit.moneyBefore : 0;
        if (!shows)
            visit.options.refusedMoney.push_back(step.mailId);
        visit.moneyTaken = AddSaturating(visit.moneyTaken, gained);
        if (verdict != visit.verdicts.end())
            verdict->moneyTaken = AddSaturating(verdict->moneyTaken, gained);
    }
    else if (step.kind == StepKind::TakeItem)
    {
        if (!shows)
            visit.options.refusedItems.push_back(step.itemGuid);
        else
        {
            ++visit.itemsTaken;
            if (verdict != visit.verdicts.end())
                ++verdict->itemsTaken;
        }
    }
    visit.answered.push_back(step);
}

// One mail decision row per mail of the first plan: its outcome from the last plan, emptied when it is gone
void WriteMailRows(Player* bot, MailVisitState const& visit, VisitPlan const& last)
{
    for (MailVerdict const& first : visit.verdicts)
    {
        MailVerdict row = first;
        auto now = std::find_if(last.verdicts.begin(), last.verdicts.end(),
                                [&first](MailVerdict const& known) { return known.mailId == first.mailId; });
        row.choice = now != last.verdicts.end() ? now->choice : MailChoice::Take;
        row.reason = now != last.verdicts.end() ? now->reason : MailReason::Emptied;
        row.itemsLeft = now != last.verdicts.end() ? now->itemsLeft : 0;
        row.worthLeft = now != last.verdicts.end() ? now->worthLeft : 0;
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Mail, ChoiceWord(row.choice), ReasonWord(row.reason),
                           static_cast<float>(row.worthLeft),
                           {{"money", static_cast<float>(row.moneyTaken)},
                            {"taken", static_cast<float>(row.itemsTaken)},
                            {"left", static_cast<float>(row.itemsLeft)}},
                           row.mailId);
    }
}

void EndVisit(PlayerbotAI* botAI, Player* bot, char const* how, std::atomic<uint32>& counter, VisitPlan const* last)
{
    MailVisitState& visit = botAI->rpgInfo.bankMail.mailVisit;
    VisitPlan computed;
    if (!last && visit.planned)
    {
        // Mail list read: the bot's own map thread inside its AI update (BankMailState.h)
        computed = PlanMailVisit(BuildMailIn(botAI), BuildBagSpace(bot), visit.options);
        last = &computed;
    }

    uint32 const itemsLeft = last ? last->itemsLeft : 0;
    if (last && WorldLog::Active())
        WriteMailRows(bot, visit, *last);
    EventLog::Emit(bot, WorldLogFormat::EVENT_MAIL_VISIT, how,
                   WorldLogFormat::PackVisitCounts(visit.mailsOpened, visit.itemsTaken, itemsLeft), visit.moneyTaken);
    LOG_DEBUG("playerbots",
              "[Bank mail] {} mailbox visit {}: {} mails opened, {} copper and {} items taken, {} left, {} s",
              bot->GetName(), how, visit.mailsOpened, visit.moneyTaken, visit.itemsTaken, itemsLeft,
              getMSTimeDiff(visit.startedMs, getMSTime()) / IN_MILLISECONDS);

    MailKnowledge& known = botAI->rpgInfo.bankMail.mail;
    if (last)
        known.seenCopper = last->seenAfter;
    RefreshMailKnowledge(bot, known);
    bool const equip = visit.itemsTaken > 0;
    visit = MailVisitState{};
    ++counter;
    if (equip)
        botAI->DoSpecificAction("equip upgrades packet action");
}
}  // namespace

bool QueueMarkRead(Player* bot, ObjectGuid const& mailbox, uint32 mailId)
{
    return Queue(bot, CMSG_MAIL_MARK_AS_READ, mailbox, mailId, std::nullopt);
}

bool QueueTakeMoney(Player* bot, ObjectGuid const& mailbox, uint32 mailId)
{
    return Queue(bot, CMSG_MAIL_TAKE_MONEY, mailbox, mailId, std::nullopt);
}

bool QueueTakeItem(Player* bot, ObjectGuid const& mailbox, uint32 mailId, uint32 itemLowGuid)
{
    return Queue(bot, CMSG_MAIL_TAKE_ITEM, mailbox, mailId, itemLowGuid);
}

bool QueueDelete(Player* bot, ObjectGuid const& mailbox, uint32 mailId)
{
    constexpr uint32 NO_MAIL_TEMPLATE = 0;
    return Queue(bot, CMSG_MAIL_DELETE, mailbox, mailId, NO_MAIL_TEMPLATE);
}

bool QueueMailStep(Player* bot, ObjectGuid const& mailbox, Step const& step)
{
    switch (step.kind)
    {
        case StepKind::MarkRead:
            return QueueMarkRead(bot, mailbox, step.mailId);
        case StepKind::TakeMoney:
            return QueueTakeMoney(bot, mailbox, step.mailId);
        case StepKind::TakeItem:
            return QueueTakeItem(bot, mailbox, step.mailId, step.itemGuid);
        default:
            return QueueDelete(bot, mailbox, step.mailId);
    }
}

std::optional<MailboxSpot> FindMailboxSpot(Player* bot, char const*& refusal)
{
    refusal = nullptr;
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
    {
        refusal = "the planner's indexes are not ready";
        return std::nullopt;
    }

    std::vector<Planner::Nearby> const nearest =
        indexes->Nearest(Planner::ServiceKind::Mailbox, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                         bot->GetPositionZ(), 1);
    if (nearest.empty())
    {
        refusal = "no mailbox on the map";
        return std::nullopt;
    }

    Planner::Service const& service = indexes->services[nearest.front().index];
    return MailboxSpot{service.mapId, service.x, service.y, service.z, nearest.front().distance};
}

bool BeginMailVisit(PlayerbotAI* botAI, MailboxSpot const& spot)
{
    if (!botAI || botAI->rpgInfo.bankMail.mailVisit.phase != MailVisitPhase::None)
        return false;

    MailVisitState& visit = botAI->rpgInfo.bankMail.mailVisit;
    visit = MailVisitState{};
    visit.phase = MailVisitPhase::Walk;
    visit.mapId = spot.mapId;
    visit.x = spot.x;
    visit.y = spot.y;
    visit.z = spot.z;
    visit.startedMs = getMSTime();
    visit.limitMs = sPlayerbotAIConfig.BankMail.MailVisitDeadlineSeconds * IN_MILLISECONDS;
    visit.options = BuildMailOptions();
    ++visitsStarted;
    return true;
}

MailVisitStep StepMailVisit(PlayerbotAI* botAI)
{
    MailVisitStep step;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || botAI->rpgInfo.bankMail.mailVisit.phase == MailVisitPhase::None)
        return step;

    MailVisitState& visit = botAI->rpgInfo.bankMail.mailVisit;
    step.result = MailVisitResult::Ended;
    if (getMSTimeDiff(visit.startedMs, getMSTime()) >= visit.limitMs)
    {
        EndVisit(botAI, bot, MAIL_VISIT_TIMEOUT, visitsTimedOut, nullptr);
        return step;
    }
    if (bot->GetMapId() != visit.mapId)
    {
        EndVisit(botAI, bot, MAIL_VISIT_NO_MAILBOX, visitsWithoutMailbox, nullptr);
        return step;
    }

    if (visit.phase == MailVisitPhase::Take)
    {
        // The handler runs in the world thread's drain after this tick's map update: the answer shows from the
        // next world tick on (a teleport skips the drain)
        if (static_cast<uint32>(GameTime::GetGameTimeMS().count()) == visit.stepTick || bot->IsBeingTeleported())
        {
            step.result = MailVisitResult::Pending;
            return step;
        }
        TakeAnswer(bot, visit);
        visit.phase = MailVisitPhase::List;
    }

    if (visit.mailbox.IsEmpty())
    {
        if (bot->GetExactDist(visit.x, visit.y, visit.z) > MAILBOX_SEARCH_YARDS)
        {
            visit.phase = MailVisitPhase::Walk;
            step.result = MailVisitResult::Walk;
            step.mapId = visit.mapId;
            step.x = visit.x;
            step.y = visit.y;
            step.z = visit.z;
            return step;
        }
        GameObject const* found = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, MAILBOX_SEARCH_YARDS);
        if (!found)
        {
            EndVisit(botAI, bot, MAIL_VISIT_NO_MAILBOX, visitsWithoutMailbox, nullptr);
            return step;
        }
        visit.mailbox = found->GetGUID();
    }

    if (!bot->GetMap()->GetGameObject(visit.mailbox))
    {
        EndVisit(botAI, bot, MAIL_VISIT_NO_MAILBOX, visitsWithoutMailbox, nullptr);
        return step;
    }
    // The handlers' own check (WorldSession::CanOpenMailBox): out of reach they ignore the packet
    if (!bot->GetGameObjectIfCanInteractWith(visit.mailbox, GAMEOBJECT_TYPE_MAILBOX))
    {
        visit.phase = MailVisitPhase::Walk;
        step.result = MailVisitResult::Walk;
        step.mailbox = visit.mailbox;
        return step;
    }

    // Mail list read: the bot's own map thread inside its AI update (BankMailState.h)
    VisitPlan const plan = PlanMailVisit(BuildMailIn(botAI), BuildBagSpace(bot), visit.options);
    if (!visit.planned)
    {
        visit.planned = true;
        visit.mailsOpened = plan.mailsOpened;
        visit.verdicts = plan.verdicts;
        for (MailVerdict& verdict : visit.verdicts)
        {
            verdict.moneyTaken = 0;
            verdict.itemsTaken = 0;
        }
    }

    auto next = std::find_if(plan.steps.begin(), plan.steps.end(),
                             [&visit](Step const& planned)
                             {
                                 return std::none_of(visit.answered.begin(), visit.answered.end(),
                                                     [&planned](Step const& done) { return SameStep(planned, done); });
                             });
    if (next == plan.steps.end())
    {
        EndVisit(botAI, bot, MAIL_VISIT_DONE, visitsDone, &plan);
        return step;
    }

    step.result = MailVisitResult::Pending;
    visit.moneyBefore = bot->GetMoney();
    if (!QueueMailStep(bot, visit.mailbox, *next))
    {
        visit.answered.push_back(*next);  // no session: never sent, not tried again
        return step;
    }
    visit.awaiting = *next;
    visit.stepTick = static_cast<uint32>(GameTime::GetGameTimeMS().count());
    visit.phase = MailVisitPhase::Take;
    return step;
}

std::string MailVisitCounters()
{
    return Acore::StringFormat("started {}, done {}, timed out {}, no mailbox {}", visitsStarted.load(),
                               visitsDone.load(), visitsTimedOut.load(), visitsWithoutMailbox.load());
}
}  // namespace BankMail
