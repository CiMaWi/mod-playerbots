/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include <functional>
#include <memory>
#include <optional>

#include "GoToRules.h"
#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"
#include "TravelProgressState.h"

namespace WorldTravel
{
enum class StuckCause : uint8_t;  // WorldTravelRules.h
}

// WT33b: a place the quest executor walks to (QuestPlaceRules.h): the objective's quest-log slot, -1 the turn-in
struct QuestDestination
{
    WorldPosition pos;
    int32 objectiveIdx;
    float roam;  // how far the bot roams around pos once there (QuestPlaces::Roam)
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    // A status's walk to its place (WT64b; GoToRules.h, NewRpgGoTo.cpp): There (within reach: do the work; also the
    // tick its trip arrived), Going (a local piece the mesh carries is ordered, or the status's trip is asked for or
    // under way) or Failed (a typed end, read here: its Stuck row is written and the status has ended with the cause
    // word's outcome). O(1) but one path query when the place or the bot's move changed. The route follower's legs
    // have their own judged walk (NewRpgTravelAction::MoveFarTo), which no status can call
    ::GoTo::Answer GoTo(WorldPosition const& place, float reach);
    // GoTo for an executor, one line per site: nothing when the bot is there, else what Execute returns this tick
    // (false: the held trip's follower did nothing this tick, so the lower actions run as under travel's own status)
    std::optional<bool> Away(WorldPosition const& place, float reach);
    // go_grind's and go_camp's reach: inside the 10 yd at which the status update takes them as arrived
    static constexpr float PLACE_REACH_YARDS = 8.0f;
    // The mover gives dest up: the Stuck row (RecordStuck), then the avoid or, off the mesh, the recovery. The cause
    WorldTravel::StuckCause GiveUpMove(WorldPosition const& dest);
    // The place is not walked to again for a while (GoTo's step 2 until WT64c); word: the cause it was given up for
    void AvoidPlace(WorldPosition const& place, char const* word);
    // Records the Stuck event (cause, status, bot and destination) when the mover gives a destination up
    WorldTravel::StuckCause RecordStuck(WorldPosition const& dest);
    // The bot on the ground stands where no walkable poly is near (NavMeshSnap); false when the mesh cannot tell
    bool BotOffMesh();
    // An off-mesh bot's way back onto the mesh (WorldTravel::OffMeshNext): hearth, else an unseen short move
    void RecoverOffMesh();
    // Casts the hearthstone as a player would (off the mount first, on the ground). On a refusal `refusal` holds the
    // reason word (WorldTravel::HearthRefusalWord) and `castResult` the core's SpellCastResult for cast_failed
    bool CastHearth(char const*& refusal, float& castResult);
    // The last yards to an object (WT64g; PLAN 3.20 (8), sites 12-14): a vendor, a quest giver or ender, a trainer, a
    // banker. A stand point within `distance` of the object, toward the bot, is picked once per object (again when the
    // object moved beyond `distance` of where it was picked) and judged as GoTo judges a local piece, at any distance:
    // the carried path's end within `distance` of the object is ordered and watched by the one progress observer.
    // Else the approach ends no_path (GoToRules.h NOT_CARRIED: its Stuck row, the object's place refused for a while)
    // and asks no trip. There: within `distance`; Going: under way; Failed: a typed end, or the object is gone (no
    // row). endsStatus: a Failed also ends the status with the word's outcome (the object is the status's goal); else
    // the caller skips the object. quiet: a Failed writes no Stuck row and refuses no place (GoTo::MarksFailure; an
    // errand's sight step, whose failure its caller passes over)
    ::GoTo::Answer Approach(ObjectGuid guid, float distance = INTERACTION_DISTANCE, bool endsStatus = false,
                            bool quiet = false);
    // The roams (WT64g, site 15): up to ROAM_SAMPLES points drawn, each judged (GoTo::RoamCarried); one the mesh does
    // not carry, in water or behind a wall is dropped and the next drawn. Counted as kind other: a roam adds no miss
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL,
                        WorldObject* center = nullptr);
    // WT33b: a walk to a random point within `roam` yards of `centre` (a quest place's area), so a bot at its place
    // meets the area's targets without drifting off
    bool RoamInPlace(WorldPosition const& centre, float roam);
    static constexpr int ROAM_SAMPLES = 8;
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* QUEST RELATED CHECK */
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();

protected:
    enum class PlaceAnswer : uint8
    {
        Found,
        None,
        NotReady  // the quest index is still building (startup): wait, the order stays
    };
    // The places of the quest's open objectives on the bot's map, nearest first, or its turn-in once complete.
    // `left`: the place a stay just left, passed over where its objective has another area (QuestPlaces::Skip)
    PlaceAnswer QuestPlaces(uint32 questId, std::vector<QuestDestination>& places, WorldPosition const* left = nullptr);
    // Whether a pick may choose the quest now: it has a place within QUEST_PICK_YARDS (the old POI filter's bound)
    bool HasQuestPlace(uint32 questId);
    static constexpr float QUEST_PICK_YARDS = 1500.0f;
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos,
                                    std::vector<uint32>& path);
    // reason: the rpg_status row's reason word for a started pick ("weights", or a planner bot's fallback word); ref:
    // the row's ref (a fallback's note, Planner::IdleStep)
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus, char const* reason = "weights", uint32 ref = 0);
    bool CheckRpgStatusAvailable(NewRpgStatus status);

    /* FLIGHTS (the random flight status and RPG_TRAVEL's flight legs) */
    enum class TaxiStart : uint8
    {
        Moving,        // walking to the flight master
        Stalled,       // the walk did not move this tick
        NoMaster,      // no living flight master of that entry near masterPos
        Undiscovered,  // a persistent bot does not know the path's nodes
        Refused,       // Player::ActivateTaxiPathTo refused (fare, node not usable)
        Started        // the taxi is taken and the fare paid
    };
    // Walks to the flight master, learns its node as a player does there, and takes the taxi along nodes. approach:
    // the caller's walk to a spot, Moving or Stalled: the follower's leg walk; none, a status's GoTo (TaxiWalk)
    using TaxiApproach = std::function<TaxiStart(WorldPosition const&)>;
    TaxiStart StartTaxi(uint32 masterEntry, WorldPosition const& masterPos, std::vector<uint32> const& nodes,
                        TaxiApproach const& approach = {});
    // A status's walk to its flight master by GoTo: a failed end has ended the status there, and the tick is spent
    TaxiStart TaxiWalk(WorldPosition const& to);
    // While in flight: a flight whose next node lies on another map continues there
    void ContinueCrossMapTaxi();

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;

private:
    // GoTo's memory of this executor, fresh with each status: the rules' state, the local piece's progress (the one
    // observer's walk rule, TravelProgress::MaintainWalk) and its move stamp
    struct GoToWalk
    {
        ::GoTo::State state;
        TravelProgress::State progress;
        ::GoTo::Place place;
        uint32 status{0};
        uint32 statusStart{0};
        uint32 moveSeq{0};    // the move stamp: counts the ends of the local orders given
        uint32 lastMs{0};     // the last tick the local piece was served
        bool ordered{false};  // a local order is under way
        bool arrived{false};  // a trip to this place arrived
        bool local{false};    // a local piece was ordered and its end is not seen yet (walkflush)
        bool spent{false};    // Away's answer for the last tick
        ObjectGuid object;    // an approach's object, the place it was picked at and its stand point
        WorldPosition anchor;
        WorldPosition point;
        float reach{0.0f};  // the reach the stand point was drawn for (GoTo::RepickStand)
    };
    std::unique_ptr<GoToWalk> goTo;
    std::unique_ptr<GoToWalk> approachWalk;  // apart from goTo: a status walks to its place and approaches in turn
    // GoTo's and Approach's one walk: place, the reach's centre; target, where the path is asked to (a stand point);
    // trips, a GoTo (else an approach); endsStatus, a Failed ends the status; quiet, a Failed writes no marks
    ::GoTo::Answer JudgedWalk(GoToWalk& walk, WorldPosition const& place, WorldPosition const& target, float reach,
                              bool trips, bool endsStatus, bool quiet = false);
    void FreshWalk(GoToWalk& walk);
    ::GoTo::Answer FailGoTo(GoToWalk& walk, bool endsStatus, char const* word, StatusEnd::Outcome fallback);
    // One roam sample: valid coords, no water, a carried path (GoTo::RoamCarried), then the order
    bool OrderRoamSample(float x, float y, float z, MovementPriority priority);

protected:
    /* FOR QUEST GIVER */
    // Time on one giver, walking and interacting, after which it is skipped until the next IDLE.
    const uint32 questGiverGiveUpTime = 60 * IN_MILLISECONDS;
    // A longer pause (combat, eating) starts the timer over.
    const uint32 questGiverRetryGap = 5 * IN_MILLISECONDS;
};

#endif
