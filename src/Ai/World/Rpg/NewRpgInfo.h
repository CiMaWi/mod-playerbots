/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFO_H
#define PLAYERBOTS_NEWRPGINFO_H

#include <memory>
#include <unordered_set>

#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "WorldTravelRules.h"

#include <array>
#include <string>
#include <unordered_map>

using NewRpgStatusTransitionProb = std::vector<std::vector<int>>;

class Player;

namespace Planner::GoalApply
{
struct BotGoals;  // GoalInterface.h
}

struct NewRpgInfo
{
    NewRpgInfo() : data(Idle{}) {}
    ~NewRpgInfo() = default;

    // The bot it belongs to (PlayerbotAI sets it): the world log's Status rows; null in a copy of no bot
    Player* bot{nullptr};

    // RPG_GO_GRIND
    struct GoGrind
    {
        WorldPosition pos{};
    };
    // RPG_GO_CAMP
    struct GoCamp
    {
        WorldPosition pos{};
    };
    // RPG_WANDER_NPC
    struct WanderNpc
    {
        ObjectGuid npcOrGo{};
        uint32 lastReach{0};
    };
    // RPG_WANDER_RANDOM
    struct WanderRandom
    {
        WanderRandom() = default;
    };
    // RPG_DO_QUEST
    struct DoQuest
    {
        Quest const* quest{nullptr};
        uint32 questId{0};
        int32 objectiveIdx{0};
        WorldPosition pos{};
        uint32 lastReachPOI{0};
    };
    // RPG_TRAVEL_FLIGHT
    struct TravelFlight
    {
        uint32 flightMasterEntry{0};
        WorldPosition flightMasterPos{};
        std::vector<uint32> path;
        bool inFlight{false};
    };
    // RPG_REST
    struct Rest
    {
        Rest() = default;
    };
    // RPG_OUTDOOR_PVP
    struct OutdoorPvP
    {
        ObjectGuid::LowType capturePointSpawnId{0};
    };
    // RPG_DO_GATHER
    struct DoGather
    {
        ObjectGuid::LowType nodeSpawnId{0};
        WorldPosition nodePos{};
        std::unordered_set<ObjectGuid::LowType> visited;
        uint32 lastReach{0};
        uint32 lastPassiveCheck{0};
        ObjectGuid::LowType lastSwitchedFrom{0};
    };
    // RPG_TRAVEL: follows a planned route leg by leg (NewRpgTravelAction, TravelLegRules.h)
    struct Travel
    {
        WorldPosition dest{};
        TravelLegs::Trip trip;        // the route's legs and the place on them; no legs: a route is due
        TravelLegs::RouteWait route;  // the route request waited for
        bool stuck{false};            // MoveFarTo gave the current waypoint up (instead of going IDLE)
        bool offMesh{false};          // that give-up found the bot off the mesh: the leg fails, the trip ends
        uint32 failedMs{0};           // the last failed leg: the new route is asked for REPLAN_DELAY_MS later
        float estimateSeconds{0.0f};  // the first route's
        uint32 tripStartMs{0};
        uint32 routeStartMs{0};  // the current route's start and estimate: the trip's deadline
        float routeSeconds{0.0f};
        std::string place;  // a GM order's place (`playerbots route go`): its first route is logged
        bool bindInn{false};   // an inn errand: at the end the bot binds at the innkeeper in front of it
        bool noHearth{false};  // a hearth leg failed: later routes of this trip leave the hearthstone out
        uint8 bindTries{0};    // an inn errand: binds tried in front of the innkeeper
        uint32 tripId{0};      // the world log's trip id (WL7)
        // A transport leg's last scan (TravelLegs::TRANSPORT_SCAN_MS)
        uint32 transportScanMs{0};
    };
    // RPG_ERRANDS: a trip through stops near the bot (WorldTravel::PlanTrip, ErrandRules.h); at each stop the bot does
    // every errand assigned there: sell, buy (a bag by worth, the shopping list) and repair at its vendor, turn in
    // completed quests, visit a trainer
    struct ErrandTurnIn
    {
        uint32 questId{0};
        uint32 ender{0};  // the creature or object entry that takes it
        bool enderIsObject{false};
    };
    struct ErrandStop
    {
        WorldPosition pos{};
        uint32 vendorEntry{0};   // the vendor of this stop; 0 = none
        uint32 trainerEntry{0};  // the trainer of this stop; 0 = none
        std::vector<ErrandTurnIn> turnIns;
    };
    struct Errands
    {
        std::vector<ErrandStop> stops;
        uint8 stop{0};           // the stop the bot walks to or works at
        bool arrived{false};     // the bot reached the current stop
        bool vendorDone{false};  // the current stop's vendor part is done (or its vendor is gone)
        ObjectGuid vendor;       // the current stop's vendor, found near its spawn on arrival
        bool visited{false};     // an errand was done at some stop: the trip's minutes are measured
        uint32 vendorEntry{0};   // the trip's first vendor (the census, the Inventory rows)
        uint32 moneyAtStart{0};
    };
    struct Idle
    {
    };

    uint32 startT{0};     // start timestamp of the current status
    uint32 lastInnMs{0};  // the last inn errand check (WorldTravel::InnCheckDue); 0 = never
    std::vector<WorldTravel::AvoidedSpot> innAvoid;  // inn spots whose bind found no usable innkeeper (InnAvoided)
    uint32 routeRequests{0};  // the bot's route request ids (TravelRoute::Snapshot::requestId), across trips
    // The planner's view of the bot (Planner::PlannerHook, PlannerService.cpp); a reset only costs a new snapshot
    struct PlannerState
    {
        uint32 stateVersion{0};    // counts the bot's situation changes; a result of an older version is not used
        uint32 stateKey{0};        // level, zone, life and group as last seen
        uint32 lastSnapshotMs{0};  // 0 = none yet
        bool snapshotDue{true};    // the situation changed or a goal ended since the last snapshot
        bool wasIdle{false};
        bool pending{false};  // a job is out; its result is taken before the next one is submitted
        std::shared_ptr<Planner::GoalApply::BotGoals> goals;  // the goal and plan of a planner bot (WT16)
    };
    PlannerState planner;

    // MOVE_FAR
    float nearestMoveFarDis{FLT_MAX};
    uint32 stuckTs{0};
    uint32 stuckAttempts{0};
    WorldPosition moveFarPos;
    // The destination given up after the last Stuck event, avoided for WorldTravel.StuckAvoidSeconds
    WorldPosition stuckAvoidPos;
    uint32 stuckAvoidTs{0};
    WorldPosition stuckBotPos;  // where the bot stood at that Stuck event
    uint32 stuckRepeats{0};     // give-ups in a row at the same obstacle (WorldTravel::NextStuckRepeats)
    WorldPosition offMeshPos;   // where the last off-mesh recovery left the bot (seen, no_poly): it backs off there
    uint32 offMeshTs{0};        // its time; 0 = none (WorldTravel::OffMeshBackoff)
    // END MOVE_FAR

    // GHOST (SpiritHealerAction): the last spirit healer search and the last fallback teleport
    uint32 ghostHealerTs{0};
    uint32 ghostTeleportTs{0};

    // QUEST GIVER
    ObjectGuid questGiver;  // giver the bot is walking to or talking with
    uint32 questGiverSince{0};
    uint32 questGiverLastTry{0};
    GuidSet ignoredQuestGivers;  // skipped until the next IDLE
    // END QUEST GIVER

    using RpgData = std::variant<
        Idle,
        GoGrind,
        GoCamp,
        WanderNpc,
        WanderRandom,
        DoQuest,
        Rest,
        TravelFlight,
        OutdoorPvP,
        DoGather,
        Travel,
        Errands
    >;
    RpgData data;

    NewRpgStatus GetStatus();
    static NewRpgStatus StatusFromString(std::string const& name);
    bool HasStatusPersisted(uint32 maxDuration) { return GetMSTimeDiffToNow(startT) > maxDuration; }
    void ChangeToGoGrind(WorldPosition pos);
    void ChangeToGoCamp(WorldPosition pos);
    void ChangeToWanderNpc();
    void ChangeToWanderRandom();
    void ChangeToDoQuest(uint32 questId, Quest const* quest);
    void ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path);
    void ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId = 0);
    void ChangeToDoGather();
    void ChangeToRest();
    void ChangeToTravel(WorldPosition dest, std::string place = {});
    void ChangeToErrands(std::vector<ErrandStop> stops, uint32 money);
    void ChangeToIdle();
    bool CanChangeTo(NewRpgStatus status);
    void Reset();
    void SetMoveFarTo(WorldPosition pos);
    std::string ToString();
};

enum class QuestDropReason : uint8
{
    Trivial,
    NotWorth,
    NotCapable,
    Failed,
    OtherZone,
    ClassOrHoliday,
    LogFull,
    Count
};

enum class QuestAbandonReason : uint8
{
    NoProgress,
    CannotTurnIn,
    Count
};

inline char const* QuestDropReasonName(QuestDropReason reason)
{
    switch (reason)
    {
        case QuestDropReason::Trivial:
            return "trivial";
        case QuestDropReason::NotWorth:
            return "not_worth";
        case QuestDropReason::NotCapable:
            return "not_capable";
        case QuestDropReason::Failed:
            return "failed";
        case QuestDropReason::OtherZone:
            return "other_zone";
        case QuestDropReason::ClassOrHoliday:
            return "class_or_holiday";
        case QuestDropReason::LogFull:
            return "log_full";
        default:
            return "unknown";
    }
}

inline char const* QuestAbandonReasonName(QuestAbandonReason reason)
{
    switch (reason)
    {
        case QuestAbandonReason::NoProgress:
            return "no_progress";
        case QuestAbandonReason::CannotTurnIn:
            return "cannot_turn_in";
        default:
            return "unknown";
    }
}

struct QuestCounters
{
    uint32 accepted{0};
    uint32 completed{0};
    uint32 rewarded{0};
    std::array<uint32, static_cast<size_t>(QuestDropReason::Count)> dropped{};
    std::array<uint32, static_cast<size_t>(QuestAbandonReason::Count)> abandoned{};
};

struct NewRpgStatistic
{
    uint32 questAccepted{0};
    uint32 questCompleted{0};
    uint32 questAbandoned{0};
    uint32 questRewarded{0};
    uint32 questDropped{0};
    // Filled only while CollectDetailedQuestStats is enabled.
    std::unordered_map<uint32, QuestCounters> byQuest;

    NewRpgStatistic& operator+=(NewRpgStatistic const& other)
    {
        questAccepted += other.questAccepted;
        questCompleted += other.questCompleted;
        questAbandoned += other.questAbandoned;
        questRewarded += other.questRewarded;
        questDropped += other.questDropped;

        for (auto const& [questId, counters] : other.byQuest)
        {
            QuestCounters& target = byQuest[questId];
            target.accepted += counters.accepted;
            target.completed += counters.completed;
            target.rewarded += counters.rewarded;
            for (size_t i = 0; i < target.dropped.size(); ++i)
                target.dropped[i] += counters.dropped[i];
            for (size_t i = 0; i < target.abandoned.size(); ++i)
                target.abandoned[i] += counters.abandoned[i];
        }
        return *this;
    }
};

#endif
