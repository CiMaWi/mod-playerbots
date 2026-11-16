/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFO_H
#define PLAYERBOTS_NEWRPGINFO_H

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

#include "BankMailState.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelLegTypes.h"
#include "TravelLifecycleTypes.h"
#include "TravelMgr.h"
#include "TravelProgressState.h"
#include "WorldTravelTypes.h"

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
        TravelProgress::State progress;
        uint32 failedMs{0};           // the last failed leg: the new route is asked for REPLAN_DELAY_MS later
        float estimateSeconds{0.0f};  // the first route's
        uint32 tripStartMs{0};
        float routeSeconds{0.0f};
        std::string place;  // a GM order's place (`playerbots route go`): its first route is logged
        bool bindInn{false};   // an inn errand: at the end the bot binds at the innkeeper in front of it
        bool noHearth{false};  // a hearth leg failed: later routes of this trip leave the hearthstone out
        uint8 bindTries{0};    // an inn errand: binds tried in front of the innkeeper
        uint32 tripId{0};      // the world log's trip id (WL7)
        // A transport leg's last scan (TravelLegs::TRANSPORT_SCAN_MS)
        uint32 transportScanMs{0};
        std::optional<TravelLifecycle::PassengerMotion> passengerMotion;

        TravelRoute::Leg const* CurrentLeg() const
        {
            return trip.legIdx < trip.legs.size() ? &trip.legs[trip.legIdx] : nullptr;
        }

        bool HasWalkTarget() const
        {
            auto const* leg = CurrentLeg();
            return leg ? leg->type == TravelRoute::LegType::Walk : bindInn;
        }

        bool PassengerOwnsProgress() const
        {
            auto const* leg = CurrentLeg();
            return passengerMotion || trip.offFirst.sinceMs || (leg && leg->type == TravelRoute::LegType::Transport);
        }
    };
    // RPG_ERRANDS: a trip through stops near the bot (WorldTravel::PlanTrip, ErrandRules.h); at each stop the bot does
    // every errand assigned there: sell, buy (a bag by worth, the shopping list) and repair at its vendor, turn in
    // completed quests, visit a trainer, the banker and the mailbox (BankVisit.h, MailVisit.h)
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
        bool trainerRiding{false};  // the trainer teaches riding (WU14), else the class trainer (WU13)
        bool trainerWeapon{false};  // a weapon master (WU15a)
        uint32 tameEntry{0};        // the beast to tame at this stop, its spawn the stop's place (WU21b); 0 = none
        // The tame at the stop (WU21b-2): the beast picked, the casts made, a cast whose channel's end is still to be
        // read, when the bot arrived or its last channel ended (the rest before a new try), the last cast and the
        // last AI update that saw its channel run (the channel's length in the channel_broken row)
        ObjectGuid tameBeast;
        uint8 tameTries{0};
        bool tameCast{false};
        uint32 tameSinceMs{0};
        uint32 tameCastMs{0};
        uint32 tameSeenMs{0};
        uint32 tamePickMs{0};  // the reach bound's start past the rest, kept by a new pick (TameReachStartMs)
        // The site and the respawn (WU.23): the threats fought at the cast (TAME_MAX_CLEARS), a fight to clear one
        // whose end starts the rest and reach bounds again; the first wait for a respawn, the wait's end after it
        uint8 tameClears{0};
        bool tameClearing{false};
        uint32 tameWaitFirstMs{0};
        uint32 tameWaitUntilMs{0};
        // The stable at this stop (WU21c): the stable master's entry, the wanted beast and its spawn for the tame stop
        // the visit puts after this one, the wait for the pet to come back after a dismount; tameStabled on that
        // tame stop: the old pet's entry (its PetTamed src stabled; only wanted families are tamed there)
        uint32 stableEntry{0};
        uint32 stableBeast{0};
        WorldPosition stableBeastAt{};
        uint32 stableSinceMs{0};
        uint32 tameStabled{0};
        std::vector<ErrandTurnIn> turnIns;
        // The bank and mail errands of the stop (WB8b), each cleared when its visit starts: a banker visit before
        // the vendor part when it withdraws (it deposits in the same visit), after it when it only deposits; the
        // mailbox last, once the sale has made room
        bool bankBefore{false};
        bool bankAfter{false};
        bool mail{false};
    };
    struct Errands
    {
        std::vector<ErrandStop> stops;
        uint8 stop{0};           // the stop the bot walks to or works at
        bool arrived{false};     // the bot reached the current stop
        bool vendorDone{false};  // the current stop's vendor part is done (or its vendor is gone)
        ObjectGuid vendor;       // the current stop's vendor, found near its spawn on arrival
        uint8 sightSteps{0};     // the steps toward a vendor in reach but out of sight (a ramp, stairs)
        bool visited{false};     // an errand was done at some stop: the trip's minutes are measured
        uint32 vendorEntry{0};   // the trip's first vendor (the census, the Inventory rows)
        uint32 moneyAtStart{0};
    };
    struct Idle
    {
    };

    uint32 startT{0};     // start timestamp of the current status
    uint32 lastInnMs{0};  // the last inn errand check (WorldTravel::InnCheckDue); 0 = never
    uint16 constraints{0};  // a test's constraints (TestConstraints bits, `playerbots route constraint`); not saved
    std::vector<WorldTravel::AvoidedSpot> innAvoid;  // inn spots whose bind found no usable innkeeper (InnAvoided)
    // The planner's view of the bot (Planner::PlannerHook, PlannerService.cpp); a reset only costs a new snapshot
    struct PlannerState
    {
        uint32 stateVersion{0};    // counts the bot's situation changes; a result of an older version is not used
        uint32 stateKey{0};        // level, zone, life and group as last seen
        uint32 lastSnapshotMs{0};  // 0 = none yet
        bool snapshotDue{true};    // the situation changed or a goal ended since the last snapshot
        bool pending{false};       // a job is out; its result is taken before the next one is submitted
        std::shared_ptr<Planner::GoalApply::BotGoals> goals;  // the goal and plan of a planner bot (WT16)
    };
    PlannerState planner;
    // Mail and bank knowledge and visits (BankMailState.h); a status change keeps it, so a visit survives it; a far
    // teleport or PlayerbotAI::Reset(true) keeps the knowledge and ends the visit (BankMail::AfterReset)
    BankMail::BotBankMail bankMail;

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
    bool CanChangeTo(NewRpgStatus status);
    void SetMoveFarTo(WorldPosition pos);
    std::string ToString();

private:
    friend struct RpgStatusCore;  // the status core (NewRpgStatusEnd.cpp): a status ends and starts only there
    void DiscardPassengerMove();
    void ReplaceStatus(RpgData replacement);
};

#endif
