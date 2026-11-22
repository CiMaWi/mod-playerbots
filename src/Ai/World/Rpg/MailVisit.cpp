/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MailVisit.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include "BankMailConfig.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "EventLog.h"
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

// Mail list read: the bot's own map thread inside its AI update only (StepVisit; BankMailState.h)
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
void TakeAnswer(Player* bot, MailVisitData& visit)
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
void WriteMailRows(Player* bot, MailVisitData const& visit, VisitPlan const& last)
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

uint32 LimitSeconds() { return sPlayerbotAIConfig.BankMail.MailVisitDeadlineSeconds; }

// By its type: the service index's mailbox spawns name no template the search needs
ObjectGuid FindMailbox(Player* bot, VisitState const& /*visit*/)
{
    GameObject const* found = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, MAILBOX_SEARCH_YARDS);
    return found ? found->GetGUID() : ObjectGuid::Empty;
}

WorldObject const* ResolveMailbox(Player* bot, ObjectGuid mailbox) { return bot->GetMap()->GetGameObject(mailbox); }

// The handlers' own check (WorldSession::CanOpenMailBox): out of reach they ignore the packet
bool MailboxInReach(Player* bot, WorldObject const& mailbox, float& /*range*/)
{
    return bot->GetGameObjectIfCanInteractWith(mailbox.GetGUID(), GAMEOBJECT_TYPE_MAILBOX);
}

// The handler runs in the world thread's drain after this tick's map update: the answer shows from the next world
// tick on (a teleport skips the drain)
VisitTurn AwaitAnswer(PlayerbotAI* botAI, Player* bot)
{
    MailVisitData& visit = botAI->bankMail.mailData;
    if (static_cast<uint32>(GameTime::GetGameTimeMS().count()) == visit.stepTick || bot->IsBeingTeleported())
        return VisitTurn{VisitNext::Pending};
    TakeAnswer(bot, visit);
    return VisitTurn{VisitNext::Continue};
}

// Mail list read: the bot's own map thread inside its AI update (BankMailState.h). Planned again from the live list
// at every step; the first planned step not answered yet is queued
VisitTurn QueueNext(PlayerbotAI* botAI, Player* bot)
{
    MailVisitData& visit = botAI->bankMail.mailData;
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
        return VisitTurn{VisitNext::End, VisitEnd::Done};

    visit.moneyBefore = bot->GetMoney();
    if (!QueueMailStep(bot, botAI->bankMail.visit.object, *next))
    {
        visit.answered.push_back(*next);  // no session: never sent, not tried again
        return VisitTurn{VisitNext::Pending};
    }
    visit.awaiting = *next;
    visit.stepTick = static_cast<uint32>(GameTime::GetGameTimeMS().count());
    return VisitTurn{VisitNext::Await};
}

bool WriteMailEnd(PlayerbotAI* botAI, Player* bot, VisitState const& state, VisitEnd end)
{
    MailVisitData& visit = botAI->bankMail.mailData;
    char const* how = VisitEndWord(VisitKind::Mail, end);
    // Once the visit planned at the mailbox, the rows come from the live list's plan (the step that found nothing
    // left to queue planned the same list). Mail list read: the bot's own map thread inside its AI update, or the
    // world thread (BankMailState.h)
    std::optional<VisitPlan> last;
    if (visit.planned)
        last = PlanMailVisit(BuildMailIn(botAI), BuildBagSpace(bot), visit.options);

    uint32 const itemsLeft = last ? last->itemsLeft : 0;
    if (last && WorldLog::Active())
        WriteMailRows(bot, visit, *last);
    EventLog::Emit(bot, EventLog::EventType::MailVisit, how,
                   WorldLogFormat::PackVisitCounts(visit.mailsOpened, visit.itemsTaken, itemsLeft), visit.moneyTaken);
    LOG_DEBUG("playerbots",
              "[Bank mail] {} mailbox visit {}: {} mails opened, {} copper and {} items taken, {} left, {} s",
              bot->GetName(), how, visit.mailsOpened, visit.moneyTaken, visit.itemsTaken, itemsLeft,
              getMSTimeDiff(state.startedMs, getMSTime()) / IN_MILLISECONDS);

    MailKnowledge& known = botAI->bankMail.mail;
    if (last)
        known.seenCopper = last->seenAfter;
    RefreshMailKnowledge(bot, known);
    bool const took = visit.itemsTaken > 0;
    visit = MailVisitData{};
    return took;
}

constexpr VisitKindRow MAIL_VISIT{.startWord = "mail",
                                  .searchYards = MAILBOX_SEARCH_YARDS,
                                  .missing = VisitEnd::NoSpot,
                                  .limitSeconds = &LimitSeconds,
                                  .find = &FindMailbox,
                                  .resolve = &ResolveMailbox,
                                  .reach = &MailboxInReach,
                                  .approach = nullptr,
                                  .awaited = &AwaitAnswer,
                                  .act = &QueueNext,
                                  .end = &WriteMailEnd};
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
    return MailboxSpot{
        .mapId = service.mapId, .x = service.x, .y = service.y, .z = service.z, .distance = nearest.front().distance};
}

std::optional<MailboxSpot> MailboxSpotNear(Player* bot, uint32 mapId, float x, float y, float z, float yards)
{
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
        return std::nullopt;

    std::vector<Planner::Nearby> const nearest =
        indexes->Nearest(Planner::ServiceKind::Mailbox, mapId, x, y, z, 1, yards);
    if (nearest.empty())
        return std::nullopt;
    Planner::Service const& service = indexes->services[nearest.front().index];
    return MailboxSpot{.mapId = service.mapId,
                       .x = service.x,
                       .y = service.y,
                       .z = service.z,
                       .distance = bot->GetExactDist(service.x, service.y, service.z)};
}

VisitKindRow const& MailVisitRow() { return MAIL_VISIT; }

bool BeginMailVisit(PlayerbotAI* botAI, MailboxSpot const& spot)
{
    if (!BeginVisit(botAI, VisitKind::Mail, &spot))
        return false;
    botAI->bankMail.mailData = MailVisitData{};
    botAI->bankMail.mailData.options = BuildMailOptions();
    return true;
}

std::string MailVisitCounters() { return VisitCounters(VisitKind::Mail); }
}  // namespace BankMail
