/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgStatusEnd.h"

#include <string_view>
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
    // The idle step of an end: the status data replaced, the start time set, the quest giver and the ignored ones
    // cleared
    static void ToIdle(NewRpgInfo& info)
    {
        info.ReplaceStatus(NewRpgInfo::Idle{});
        info.startT = getMSTime();
        info.questGiver.Clear();
        info.ignoredQuestGivers.clear();
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
// planner's note (the goal's run learns the outcome), idle, the one Status row. CS14f lets a start pass its next status
void Change(PlayerbotAI* botAI, Outcome outcome, StatusEnd::Note const& note, char const* word)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus const oldStatus = info.GetStatus();
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
        Planner::OnStatusEnd(botAI, outcome, true);
    RpgStatusCore::ToIdle(info);
    WorldLog::NoteStatus(info.bot, oldStatus, info.GetStatus(), word);
}
}  // namespace

void RpgStatus::End(PlayerbotAI* botAI, StatusEnd::Outcome outcome, StatusEnd::Note note)
{
    Change(botAI, outcome, note, note.word ? note.word : StatusEnd::Word(outcome));
}
