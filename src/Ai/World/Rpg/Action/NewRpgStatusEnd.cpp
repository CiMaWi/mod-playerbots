/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgStatusEnd.h"

#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "BotTravel.h"
#include "ErrandRules.h"
#include "EventLog.h"
#include "GatherLive.h"
#include "GatherPassBy.h"
#include "GoToRules.h"
#include "GroundRules.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "NewRpgAction.h"
#include "NewRpgDoFish.h"
#include "NewRpgInfo.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PursuitRules.h"
#include "QuestGiveUp.h"
#include "TestConstraintStates.h"
#include "Timer.h"
#include "TravelLegRules.h"
#include "TravelTransportGeometry.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldLogStatus.h"

static_assert(uint32(RPG_IDLE) == StatusEnd::STATUS_IDLE && uint32(RPG_GO_GRIND) == StatusEnd::STATUS_GO_GRIND &&
                  uint32(RPG_GO_CAMP) == StatusEnd::STATUS_GO_CAMP &&
                  uint32(RPG_WANDER_RANDOM) == StatusEnd::STATUS_WANDER_RANDOM &&
                  uint32(RPG_WANDER_NPC) == StatusEnd::STATUS_WANDER_NPC &&
                  uint32(RPG_DO_QUEST) == StatusEnd::STATUS_DO_QUEST &&
                  uint32(RPG_TRAVEL_FLIGHT) == StatusEnd::STATUS_TRAVEL_FLIGHT &&
                  uint32(RPG_REST) == StatusEnd::STATUS_REST &&
                  uint32(RPG_OUTDOOR_PVP) == StatusEnd::STATUS_OUTDOOR_PVP &&
                  uint32(RPG_DO_GATHER) == StatusEnd::STATUS_DO_GATHER &&
                  uint32(RPG_TRAVEL) == StatusEnd::STATUS_TRAVEL && uint32(RPG_ERRANDS) == StatusEnd::STATUS_ERRANDS &&
                  uint32(RPG_DO_FISH) == StatusEnd::STATUS_DO_FISH && uint32(RPG_STATUS_END) == StatusEnd::STATUS_COUNT,
              "StatusEndRules.h's status numbers follow NewRpgStatus");

// The status core's way into NewRpgInfo and BotTravel (their friend): only the core replaces a status and begins or
// ends the held trip
struct RpgStatusCore
{
    static StatusEnd::TripHold<HeldTrip>& Hold(BotTravel& travel) { return travel.hold; }

    // The replacing step of a change: the held trip ends with the status's outcome (its passenger motion discarded),
    // the status data is replaced and its start time set; a travel status begins its trip with that start and a world
    // log id (WT64f: status and trip start in this one call), idle clears the quest giver and the ignored ones
    static void Replace(PlayerbotAI* botAI, NewRpgInfo& info, NewRpgInfo::RpgData next, StatusEnd::TripEnd end)
    {
        StatusEnd::TripHold<HeldTrip>& hold = Hold(botAI->GetTravel());
        if (HeldTrip* const trip = hold.Held(); trip && botAI->GetBot())
            TravelTransportGeometry::DiscardMotion(*botAI->GetBot(), trip->passengerMotion);
        hold.End(end.outcome, end.word);
        info.ReplaceStatus(std::move(next));
        info.startT = getMSTime();
        if (auto const* travel = std::get_if<NewRpgInfo::Travel>(&info.data))
        {
            HeldTrip trip;
            trip.dest = travel->dest;
            trip.place = travel->place;
            trip.tripStartMs = info.startT;
            trip.tripId = WorldLog::NextTripId();
            hold.Begin(RPG_TRAVEL, std::move(trip));
        }
        else if (std::holds_alternative<NewRpgInfo::Idle>(info.data))
        {
            botAI->rpgMemos.questGiver.Clear();
            botAI->rpgMemos.ignoredQuestGivers.clear();
        }
    }
};

namespace
{
using StatusEnd::Outcome;

// The Travel event sources of an abort ("abort:<reason>"); the trip's abort decision takes the reason
constexpr std::string_view TRAVEL_ABORT_PREFIX = "abort:";

// The errand trip's clean-up: the errand check is stamped; a trip with a visited stop adds its minutes to the slot
// measure and records errand_run, a sick end records errand_sick, any other errand_abort (WorldTravel::ErrandEnd)
void EndErrands(PlayerbotAI* botAI, NewRpgInfo::Errands const& data, Outcome outcome)
{
    Player* bot = botAI->GetBot();
    WorldTravel::SlotMeasure& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    uint32 const now = getMSTime();
    float const minutes = static_cast<float>(getMSTimeDiff(botAI->rpgInfo.startT, now)) / (MINUTE * IN_MILLISECONDS);
    WorldTravel::ErrandEndRow const row =
        WorldTravel::ErrandEnd(measure, now, minutes, data.visited, outcome == Outcome::Sick);
    if (row == WorldTravel::ErrandEndRow::Run)
        EventLog::Emit(bot, EventLog::EventType::Inventory, WorldTravel::ErrandEndWord(row), data.vendorEntry,
                       int64(bot->GetMoney()) - int64(data.moneyAtStart));
    else
        EventLog::Emit(bot, EventLog::EventType::Inventory, WorldTravel::ErrandEndWord(row), data.vendorEntry,
                       int64(minutes));
}

// The trip's arrive arm: the log line and the arrived Travel event (the inn bind stays the trip's own work)
void ArriveTrip(Player* bot, HeldTrip const& data)
{
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} arrived after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                  seconds, data.estimateSeconds, data.trip.replans);
    else
        LOG_INFO("playerbots", "[Route] {} arrived at {} after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                 data.place, seconds, data.estimateSeconds, data.trip.replans);
    EventLog::Emit(bot, EventLog::EventType::Travel, "arrived", data.tripId, seconds);
}

// The trip's abort arm: the log line, the Travel event (source "abort:<reason>", a literal) and the trip's abort
// decision; a note without source writes the outcome's (StatusEnd::TripAbortSource), one without text logs the reason.
// A sick end after TRIP_DEATHS deaths writes the goal's place to the record, as the third death's end would have
// (pathing PA56, TravelLegs::SickEndFailure): on the bot's own status update, not in the death hook
void AbortTrip(PlayerbotAI* botAI, HeldTrip const& data, Outcome outcome, StatusEnd::Note const& note)
{
    Player* const bot = botAI->GetBot();
    if (outcome == Outcome::Sick)
        if (auto const failure = TravelLegs::SickEndFailure(
                data.trip,
                {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()},
                {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()}))
            botAI->GetTravel().NoteFailure(TravelLegs::TRIP_DEATHS_WORD, *failure, data.tripId);
    char const* const source = note.source ? note.source : StatusEnd::TripAbortSource(outcome);
    std::string_view const reason = WorldLogFormat::AfterPrefix(source, TRAVEL_ABORT_PREFIX);
    std::string_view const why = note.text ? std::string_view(note.text) : reason;
    if (!StatusEnd::TripAbortLogsInfo(!data.place.empty(), outcome))
        LOG_DEBUG("playerbots", "[Route] {} gives the trip up: {}", bot->GetName(), why);
    else if (data.place.empty())
        LOG_INFO("playerbots", "[Route] {} gives the trip up: {}", bot->GetName(), why);
    else
        LOG_INFO("playerbots", "[Route] {} gives the trip to {} up: {}", bot->GetName(), data.place, why);
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    EventLog::Emit(bot, EventLog::EventType::Travel, source, data.tripId, seconds);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort", reason, static_cast<float>(seconds),
                       {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
}

// A trip that ends with its bot in the air lands it, whatever ended it (pathing PA31, the cause of PA30's group A: a
// trip given up during its fly leg by a status cut, a hold or a switch left the bot hovering, and every route it asked
// for from there was refused). The trip ends as before; the bot comes down as a player would (Ground::Origin::TripEnd)
void LandsItsBot(PlayerbotAI* botAI, HeldTrip const& trip)
{
    if (Player* const bot = botAI->GetBot(); bot && bot->IsInWorld())
        NewRpgTravelAction(botAI).GetToGround(Ground::Origin::TripEnd, trip.tripId);
}

// Every status change of the core, in this order: the held trip's end rows where its holder's row cleans up, its
// bot's landing (LandsItsBot), the ending status's clean-up (it reads the status data), the planner's note (the
// goal's run learns the outcome), the held trip's end and the next status (Replace), the one Status row
void Change(PlayerbotAI* botAI, NewRpgInfo::RpgData next, Outcome outcome, StatusEnd::Note const& note,
            char const* word)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus const oldStatus = info.GetStatus();
    bool const toIdle = std::holds_alternative<NewRpgInfo::Idle>(next);
    BotTravel const& travel = botAI->GetTravel();
    // The held trip's end rows. Travel's own trip ends by its status's clean-up (gap T1: none when interrupted);
    // another holder's trip never outlives it (WT64b) and ends with the holder's outcome and note, at every end
    bool const tripRows =
        !StatusEnd::TripEndsHolder(travel.TripHolder()) || StatusEnd::CleansUp(travel.TripHolder(), outcome);
    if (HeldTrip const* const trip = travel.Trip(); trip && tripRows)
    {
        if (outcome == Outcome::Arrived)
            ArriveTrip(botAI->GetBot(), *trip);
        else
            AbortTrip(botAI, *trip, outcome, note);
    }
    if (HeldTrip const* const trip = travel.Trip())
        LandsItsBot(botAI, *trip);
    if (auto const* const errands = std::get_if<NewRpgInfo::Errands>(&info.data);
        errands && StatusEnd::CleansUp(oldStatus, outcome))
        EndErrands(botAI, *errands, outcome);
    if (auto const* const fish = std::get_if<NewRpgInfo::DoFish>(&info.data);
        fish && StatusEnd::CleansUp(oldStatus, outcome))
        NewRpgDoFishAction::EndTrip(botAI, *fish, outcome,
                                    note.source ? note.source : (note.word ? note.word : StatusEnd::Word(outcome)));
    // A quest given up, by its executor or by whatever ended it: its failure row and record (questing WT35c). A free
    // quest status writes it when abandoned or ended by a give-up word (GoTo's walk deadline too); an order for its
    // failed hand-ins (WQ19b, Pursuit::WritesFailure) and, at its own end below, for the planned takes it leaves open
    // at their giver (WQ18c, QuestGiveUp::OrderEnded)
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data);
        quest && Pursuit::WritesFailure(quest->order.has_value(), outcome, word))
        QuestGiveUp::Noted(botAI, *quest, word);
    // An order's one QuestOrder row, whatever ended it (questing WQ19a); its own end goes on to the route it belongs
    // to (WQ18b: the route's next order follows; an end that says the place gave nothing or was not reached closes
    // its cell). Any other end goes there too (part 3): a failed walk closes the cell, a take-over gives the order
    // again
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data); quest && quest->order)
    {
        bool const reached = quest->order->arrivedAt != 0;
        if (std::optional<Planner::QuestOrderEnd> const own =
                QuestGiveUp::OrderEnded(botAI, *quest, outcome, note.word))
            Planner::OnOrderEnd(botAI, quest->order->order, *own, reached);
        else
            Planner::OnOrderFailed(botAI, quest->order->order, outcome, note.source ? note.source : note.word);
    }
    if (oldStatus != RPG_IDLE)
        Planner::OnStatusEnd(botAI, outcome, toIdle, note.source ? note.source : note.word);
    char const* const endWord = note.source ? note.source : (note.word ? note.word : StatusEnd::Word(outcome));
    GatherPassBy::NoteStatusEnd(botAI, oldStatus);  // the ended status's passes by cause (professions WR13c)
    // A gather walk that ended no_walk: the current stop's place enters the record of failed attempts, so no circuit
    // plans it while the entry lives (professions WPC9, DECISIONS WPC.12). A local piece's own no_walk entry there
    // stays as it is
    if (auto const* gather = std::get_if<NewRpgInfo::DoGather>(&info.data);
        gather && gather->nodeSpawnId && ::GoTo::CauseOf(endWord) == "no_walk")
    {
        Player* const bot = botAI->GetBot();
        WorldPosition const& stop = gather->nodePos;
        FailedAttempts::Failure failure;
        failure.goal = {stop.GetMapId(), stop.GetPositionX(), stop.GetPositionY(), stop.GetPositionZ()};
        failure.spot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        botAI->GetTravel().NoteFailure("gather_no_walk", failure, 0);
    }
    // A planned circuit's span enters the observed rate's tally (professions WPC13b)
    if (auto const* gather = std::get_if<NewRpgInfo::DoGather>(&info.data))
        Planner::NoteGatherSpan(gather->span, gather->spanStartMs);
    RpgStatusCore::Replace(botAI, info, std::move(next), {outcome, endWord});
    WorldLog::NoteStatus(botAI->GetBot(), oldStatus, info.GetStatus(), word);
}
}  // namespace

void RpgStatus::End(PlayerbotAI* botAI, StatusEnd::Outcome outcome, StatusEnd::Note note)
{
    Change(botAI, NewRpgInfo::Idle{}, outcome, note, note.word ? note.word : StatusEnd::Word(outcome));
}

void RpgStatus::Start(PlayerbotAI* botAI, NewRpgInfo::RpgData next, char const* word, StatusEnd::Outcome why,
                      StatusEnd::Note note)
{
    Change(botAI, std::move(next), why, note, word);
}

void RpgStatus::Reset(PlayerbotAI* botAI, bool farTeleport)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus const oldStatus = info.GetStatus();
    StatusEnd::TripHold<HeldTrip>& hold = RpgStatusCore::Hold(botAI->GetTravel());
    HeldTrip* const trip = hold.Held();
    if (trip && botAI->GetBot())
    {
        TravelTransportGeometry::SplineControl control{*botAI->GetBot()};
        TravelLifecycle::ResetMotion(trip->passengerMotion, farTeleport, control);
        trip->progress.Suspend();
        trip->walkTo = WorldPosition();
    }
    StatusEnd::ResetList const list = StatusEnd::ResetOf(farTeleport, trip != nullptr);
    // A fishing trip the reset drops gives its weapon back first (professions WR16b)
    std::optional<NewRpgInfo::DoFish> fishing;
    if (auto const* const fish = std::get_if<NewRpgInfo::DoFish>(&info.data))
        fishing = *fish;
    // A planned circuit the reset drops is a span of the observed rate as at a status change (professions WPC13b)
    std::optional<std::pair<GatherRate::Span, uint32>> gathering;
    if (auto const* const gather = std::get_if<NewRpgInfo::DoGather>(&info.data))
        gathering.emplace(gather->span, gather->spanStartMs);
    // The held trip first: kept with its holder across a far teleport, else ended without a row (as its status)
    hold.Reset(farTeleport);
    StatusEnd::ApplyReset(list.status, info);
    if (fishing && !std::holds_alternative<NewRpgInfo::DoFish>(info.data) && botAI->GetBot())
        NewRpgDoFishAction::EndTrip(botAI, *fishing, StatusEnd::Outcome::Interrupted,
                                    farTeleport ? "teleport" : "reset");
    if (gathering && !std::holds_alternative<NewRpgInfo::DoGather>(info.data))
        Planner::NoteGatherSpan(gathering->first, gathering->second);
    StatusEnd::ApplyReset(list.planner, botAI->planner);
    StatusEnd::ApplyReset(list.rpgMemos, botAI->rpgMemos);
    botAI->GetTravel().ResetFailures(list.rpgMemos);  // the record of failed attempts with the memos
    StatusEnd::ApplyReset(list.testConstraints, botAI->testConstraints);
    WorldLog::NoteStatus(botAI->GetBot(), oldStatus, info.GetStatus(), farTeleport ? "teleport" : "reset");
}

bool RpgStatus::BeginTrip(PlayerbotAI* botAI, WorldPosition const& dest)
{
    StatusEnd::TripHold<HeldTrip>& hold = RpgStatusCore::Hold(botAI->GetTravel());
    if (hold.Held())
        return false;
    HeldTrip trip;
    trip.dest = dest;
    trip.tripStartMs = getMSTime();
    trip.tripId = WorldLog::NextTripId();
    hold.Begin(static_cast<uint32>(botAI->rpgInfo.GetStatus()), std::move(trip));
    return true;
}

void RpgStatus::TripEnded(PlayerbotAI* botAI, StatusEnd::Outcome outcome, StatusEnd::Note note)
{
    BotTravel& travel = botAI->GetTravel();
    HeldTrip* const trip = travel.Trip();
    if (!trip)
        return;
    // An errand status's deadline leaves out the time its trips ran (StatusEnd::NoTripMs)
    auto* const errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    if (errands && travel.TripHolder() == StatusEnd::STATUS_ERRANDS)
        errands->tripMs += GetMSTimeDiffToNow(trip->tripStartMs);
    // walkflush: a test's bot writes the walk's MeshMiss rows here, after the trip's Travel row
    if (StatusEnd::TripEndsHolder(travel.TripHolder()))
    {
        End(botAI, outcome, note);
        TravelOrders::FlushWalk(botAI);
        return;
    }
    // The trip's rows as travel's own status writes them; the end's word is the bare cause the holder reads
    Player* const bot = botAI->GetBot();
    char const* word = StatusEnd::Word(Outcome::Arrived);
    if (outcome == Outcome::Arrived)
        ArriveTrip(bot, *trip);
    else
    {
        AbortTrip(botAI, *trip, outcome, note);
        word = GoTo::CauseOf(note.source ? note.source : StatusEnd::TripAbortSource(outcome)).data();
    }
    TravelTransportGeometry::DiscardMotion(*bot, trip->passengerMotion);
    LandsItsBot(botAI, *trip);
    RpgStatusCore::Hold(travel).Finish(outcome, word, bot->GetExactDist(trip->dest));
    TravelOrders::FlushWalk(botAI);
}

char const* RpgStatus::TripGoal(PlayerbotAI* botAI, bool inn)
{
    uint32 const holder = botAI->GetTravel().TripHolder();
    if (inn || StatusEnd::TripEndsHolder(holder))
        return inn ? "inn" : "order";
    return WorldLog::NewRpgStatusWord(static_cast<int32>(holder));
}

std::optional<StatusEnd::TripEnd> RpgStatus::TakeTripEnd(PlayerbotAI* botAI)
{
    return RpgStatusCore::Hold(botAI->GetTravel()).TakeEnd(static_cast<uint32>(botAI->rpgInfo.GetStatus()));
}

NewRpgInfo::Travel RpgStatus::TripTo(WorldPosition dest, std::string place)
{
    NewRpgInfo::Travel trip;
    trip.dest = dest;
    trip.place = std::move(place);
    return trip;
}

NewRpgInfo::Errands RpgStatus::ErrandTrip(std::vector<NewRpgInfo::ErrandStop> stops, uint32 money)
{
    NewRpgInfo::Errands errands;
    for (NewRpgInfo::ErrandStop const& stop : stops)
        if (stop.vendorEntry)
        {
            errands.vendorEntry = stop.vendorEntry;
            break;
        }
    errands.stops = std::move(stops);
    errands.moneyAtStart = money;
    return errands;
}
