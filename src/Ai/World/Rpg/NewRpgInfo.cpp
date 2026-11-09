/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgInfo.h"
#include "Timer.h"
#include "WorldLogStatus.h"
#include <cmath>

void NewRpgInfo::ChangeToGoGrind(WorldPosition pos)
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    data = GoGrind{pos};
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToGoCamp(WorldPosition pos)
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    data = GoCamp{pos};
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToWanderNpc()
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    data = WanderNpc{};
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToWanderRandom()
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    data = WanderRandom{};
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToDoQuest(uint32 questId, Quest const* quest)
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    DoQuest do_quest;
    do_quest.questId = questId;
    do_quest.quest = quest;
    data = do_quest;
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path)
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    TravelFlight flight;
    flight.flightMasterEntry = flightMasterEntry;
    flight.flightMasterPos = flightMasterPos;
    flight.path = std::move(path);
    flight.inFlight = false;
    data = flight;
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId)
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    OutdoorPvP pvp;
    pvp.capturePointSpawnId = capturePointSpawnId;
    data = pvp;
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToDoGather()
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    data = DoGather{};
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToRest()
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    data = Rest{};
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToTravel(WorldPosition dest, std::string place)
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    Travel travel;
    travel.dest = dest;
    travel.tripStartMs = startT;
    travel.routeStartMs = startT;
    travel.place = std::move(place);
    travel.tripId = WorldLog::NextTripId();
    data = std::move(travel);
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToErrands(uint32 vendorEntry, WorldPosition vendorPos, uint32 money)
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    Errands errands;
    errands.vendorEntry = vendorEntry;
    errands.vendorPos = vendorPos;
    errands.moneyAtStart = money;
    data = errands;
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::ChangeToIdle()
{
    NewRpgStatus const oldStatus = GetStatus();
    startT = getMSTime();
    data = Idle{};
    questGiver.Clear();
    ignoredQuestGivers.clear();
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

bool NewRpgInfo::CanChangeTo(NewRpgStatus)
{
    return true;
}

void NewRpgInfo::Reset()
{
    NewRpgStatus const oldStatus = GetStatus();
    data = Idle{};
    startT = getMSTime();
    questGiver.Clear();
    ignoredQuestGivers.clear();
    WorldLog::NoteStatus(bot, oldStatus, GetStatus());
}

void NewRpgInfo::SetMoveFarTo(WorldPosition pos)
{
    nearestMoveFarDis = FLT_MAX;
    stuckTs = 0;
    stuckAttempts = 0;
    moveFarPos = pos;
}

NewRpgStatus NewRpgInfo::StatusFromString(std::string const& name)
{
    if (name == "idle")           return RPG_IDLE;
    if (name == "rest")           return RPG_REST;
    if (name == "wander random")  return RPG_WANDER_RANDOM;
    if (name == "wander npc")     return RPG_WANDER_NPC;
    if (name == "go grind")       return RPG_GO_GRIND;
    if (name == "go camp")        return RPG_GO_CAMP;
    if (name == "do quest")       return RPG_DO_QUEST;
    if (name == "travel flight")  return RPG_TRAVEL_FLIGHT;
    if (name == "outdoor pvp")    return RPG_OUTDOOR_PVP;
    if (name == "do gather")      return RPG_DO_GATHER;
    if (name == "travel")         return RPG_TRAVEL;
    if (name == "errands")        return RPG_ERRANDS;
    return RPG_STATUS_END;
}

NewRpgStatus NewRpgInfo::GetStatus()
{
    return std::visit([](auto&& arg) -> NewRpgStatus {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, Idle>) return RPG_IDLE;
        if constexpr (std::is_same_v<T, GoGrind>) return RPG_GO_GRIND;
        if constexpr (std::is_same_v<T, GoCamp>) return RPG_GO_CAMP;
        if constexpr (std::is_same_v<T, WanderNpc>) return RPG_WANDER_NPC;
        if constexpr (std::is_same_v<T, WanderRandom>) return RPG_WANDER_RANDOM;
        if constexpr (std::is_same_v<T, Rest>) return RPG_REST;
        if constexpr (std::is_same_v<T, DoQuest>) return RPG_DO_QUEST;
        if constexpr (std::is_same_v<T, TravelFlight>) return RPG_TRAVEL_FLIGHT;
        if constexpr (std::is_same_v<T, OutdoorPvP>) return RPG_OUTDOOR_PVP;
        if constexpr (std::is_same_v<T, DoGather>) return RPG_DO_GATHER;
        if constexpr (std::is_same_v<T, Travel>) return RPG_TRAVEL;
        if constexpr (std::is_same_v<T, Errands>) return RPG_ERRANDS;
        return RPG_IDLE;
    }, data);
}

std::string NewRpgInfo::ToString()
{
    std::stringstream out;
    out << "Status: ";
    std::visit([&out, this](auto&& arg)
    {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, GoGrind>)
        {
            out << "GO_GRIND";
            out << "\nGrindPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastGoGrind: " << startT;
        }
        else if constexpr (std::is_same_v<T, GoCamp>)
        {
            out << "GO_CAMP";
            out << "\nCampPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastGoCamp: " << startT;
        }
        else if constexpr (std::is_same_v<T, WanderNpc>)
        {
            out << "WANDER_NPC";
            out << "\nnpcOrGo: entry " << arg.npcOrGo.GetEntry() << " (guid " << arg.npcOrGo.GetCounter()
                << ")";
            out << "\nlastWanderNpc: " << startT;
            out << "\nlastReachNpcOrGo: " << arg.lastReach;
        }
        else if constexpr (std::is_same_v<T, WanderRandom>)
        {
            out << "WANDER_RANDOM";
            out << "\nlastWanderRandom: " << startT;
        }
        else if constexpr (std::is_same_v<T, Idle>)
        {
            out << "IDLE";
        }
        else if constexpr (std::is_same_v<T, Rest>)
        {
            out << "REST";
            out << "\nlastRest: " << startT;
        }
        else if constexpr (std::is_same_v<T, DoQuest>)
        {
            out << "DO_QUEST";
            out << "\nquestId: " << arg.questId;
            out << "\nobjectiveIdx: " << arg.objectiveIdx;
            out << "\npoiPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastReachPOI: " << (arg.lastReachPOI ? GetMSTimeDiffToNow(arg.lastReachPOI) : 0);
        }
        else if constexpr (std::is_same_v<T, TravelFlight>)
        {
            out << "TRAVEL_FLIGHT";
            out << "\nflightMasterEntry: " << arg.flightMasterEntry;
            out << "\nfromNode: " << arg.path[0];
            out << "\ntoNode: " << arg.path[arg.path.size() - 1];
            out << "\ninFlight: " << arg.inFlight;
        }
        else if constexpr (std::is_same_v<T, OutdoorPvP>)
        {
            out << "OUTDOOR_PVP";
            if (!arg.capturePointSpawnId)
                out << "\nNo capture point assigned.";
            else
                out << "\ncapturePointSpawnId: " << arg.capturePointSpawnId;
        }
        else if constexpr (std::is_same_v<T, DoGather>)
        {
            out << "DO_GATHER";
            out << "\nnodeSpawnId: " << arg.nodeSpawnId;
            out << "\nnodePos: " << arg.nodePos.GetMapId() << " " << arg.nodePos.GetPositionX() << " "
                << arg.nodePos.GetPositionY() << " " << arg.nodePos.GetPositionZ();
            out << "\nvisited: " << arg.visited.size();
            out << "\nlastReach: " << (arg.lastReach ? GetMSTimeDiffToNow(arg.lastReach) : 0);
        }
        else if constexpr (std::is_same_v<T, Travel>)
        {
            out << "TRAVEL";
            out << "\ndest: " << arg.dest.GetMapId() << " " << arg.dest.GetPositionX() << " "
                << arg.dest.GetPositionY() << " " << arg.dest.GetPositionZ();
            out << "\nleg: " << arg.trip.legIdx << "/" << arg.trip.legs.size();
            out << "\nwaitingForRoute: " << arg.route.waiting;
            out << "\nreplans: " << static_cast<uint32>(arg.trip.replans);
            out << "\nestimate: " << arg.estimateSeconds;
            out << "\nelapsed: " << GetMSTimeDiffToNow(arg.tripStartMs) / IN_MILLISECONDS;
        }
        else if constexpr (std::is_same_v<T, Errands>)
        {
            out << "ERRANDS";
            out << "\nvendorEntry: " << arg.vendorEntry;
            out << "\nvendorPos: " << arg.vendorPos.GetMapId() << " " << arg.vendorPos.GetPositionX() << " "
                << arg.vendorPos.GetPositionY() << " " << arg.vendorPos.GetPositionZ();
            out << "\nfound: " << (arg.vendor.IsEmpty() ? 0 : 1);
            out << "\nlastErrands: " << startT;
        }
        else
            out << "UNKNOWN";
    }, data);
    return out.str();
}
