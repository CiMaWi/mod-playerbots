/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgStatusEnd.h"

#include <string_view>
#include <utility>
#include <variant>

#include "CensusMgr.h"
#include "ErrandRules.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Timer.h"
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
                  uint32(RPG_STATUS_END) == StatusEnd::STATUS_COUNT,
              "StatusEndRules.h's status numbers follow NewRpgStatus");

// The status core's way into NewRpgInfo (its friend): only the core replaces a status
struct RpgStatusCore
{
    // The replacing step of a change: the status data replaced and its start time set; a trip takes its start and
    // world log id, idle clears the quest giver and the ignored ones
    static void Replace(Player* bot, NewRpgInfo& info, RpgMemos& memo, NewRpgInfo::RpgData next)
    {
        info.ReplaceStatus(bot, std::move(next));
        info.startT = getMSTime();
        if (auto* trip = std::get_if<NewRpgInfo::Travel>(&info.data))
        {
            trip->tripStartMs = info.startT;
            trip->tripId = WorldLog::NextTripId();
        }
        else if (std::holds_alternative<NewRpgInfo::Idle>(info.data))
        {
            memo.questGiver.Clear();
            memo.ignoredQuestGivers.clear();
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
        EventLog::Emit(bot, "Inventory", WorldTravel::ErrandEndWord(row), data.vendorEntry,
                       int64(bot->GetMoney()) - int64(data.moneyAtStart));
    else
        EventLog::Emit(bot, "Inventory", WorldTravel::ErrandEndWord(row), data.vendorEntry, int64(minutes));
}

// The trip's arrive arm: the log line and the arrived Travel event (the inn bind stays the trip's own work)
void ArriveTrip(Player* bot, NewRpgInfo::Travel const& data)
{
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} arrived after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                  seconds, data.estimateSeconds, data.trip.replans);
    else
        LOG_INFO("playerbots", "[Route] {} arrived at {} after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                 data.place, seconds, data.estimateSeconds, data.trip.replans);
    EventLog::Emit(bot, "Travel", "arrived", data.tripId, seconds);
}

// The trip's abort arm: the log line, the Travel event (source "abort:<reason>", a literal) and the trip's abort
// decision; a note without source writes the outcome's (StatusEnd::TripAbortSource), one without text logs the reason
void AbortTrip(Player* bot, NewRpgInfo::Travel const& data, Outcome outcome, StatusEnd::Note const& note)
{
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
    EventLog::Emit(bot, "Travel", source, data.tripId, seconds);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort", reason, static_cast<float>(seconds),
                       {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
}

// Every status change of the core, in this order: the ending status's clean-up (it reads the status data), the
// planner's note (the goal's run learns the outcome), the next status, the one Status row
void Change(PlayerbotAI* botAI, NewRpgInfo::RpgData next, Outcome outcome, StatusEnd::Note const& note,
            char const* word)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus const oldStatus = info.GetStatus();
    bool const toIdle = std::holds_alternative<NewRpgInfo::Idle>(next);
    if (oldStatus != RPG_IDLE && StatusEnd::CleansUp(oldStatus, outcome))
    {
        if (auto const* errands = std::get_if<NewRpgInfo::Errands>(&info.data))
            EndErrands(botAI, *errands, outcome);
        else if (auto const* trip = std::get_if<NewRpgInfo::Travel>(&info.data))
        {
            if (outcome == Outcome::Arrived)
                ArriveTrip(botAI->GetBot(), *trip);
            else
                AbortTrip(botAI->GetBot(), *trip, outcome, note);
        }
    }
    if (oldStatus != RPG_IDLE)
        Planner::OnStatusEnd(botAI, outcome, toIdle);
    RpgStatusCore::Replace(botAI->GetBot(), info, botAI->rpgMemos, std::move(next));
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
    auto* const trip = std::get_if<NewRpgInfo::Travel>(&info.data);
    if (trip && botAI->GetBot())
    {
        TravelTransportGeometry::SplineControl control{*botAI->GetBot()};
        TravelLifecycle::ResetMotion(trip->passengerMotion, farTeleport, control);
        trip->progress.Suspend();
    }
    StatusEnd::ResetList const list = StatusEnd::ResetOf(farTeleport, trip != nullptr);
    StatusEnd::ApplyReset(list.status, info);
    StatusEnd::ApplyReset(list.planner, botAI->planner);
    StatusEnd::ApplyReset(list.rpgMemos, botAI->rpgMemos);
    StatusEnd::ApplyReset(list.testConstraints, botAI->testConstraints);
    WorldLog::NoteStatus(botAI->GetBot(), oldStatus, info.GetStatus(), farTeleport ? "teleport" : "reset");
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
