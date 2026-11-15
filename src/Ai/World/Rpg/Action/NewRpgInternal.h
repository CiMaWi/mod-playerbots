/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINTERNAL_H
#define PLAYERBOTS_NEWRPGINTERNAL_H

#include <cstdint>
#include <vector>

#include "Define.h"
#include "NewRpgInfo.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelMgr.h"
#include "TravelTransportGeometry.h"

class Creature;
class Player;
class PlayerbotAI;
class WorldLocation;

namespace WorldTravel
{
struct ErrandPlace;
struct ServiceStop;
}  // namespace WorldTravel

namespace Planner
{
class UpkeepIndex;
}  // namespace Planner

// The RPG action layer's shared helpers: each is defined in one file of the layer and called from another (design
// rpg-action-layer §5.2). Private to src/Ai/World/Rpg/: a file that calls one has `using namespace NewRpgInternal;`
namespace NewRpgInternal
{
// Transport legs: the passenger names the transport legs and the route follower share
using TravelLifecycle::MotionKind;
using TravelLifecycle::Outcome;
using TravelLifecycle::PassengerMotion;
using TravelLifecycle::Point;
namespace TransportGeometry = TravelTransportGeometry;

// Transport legs, called by the test orders (`route aboard`)
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry);

// Transport legs, called by the route follower
void IssueTransportMove(Player& bot, NewRpgInfo::Travel& data, PassengerMotion motion,
                        std::vector<Point> const& via = {});
bool OffFirstBegins(Player* bot, NewRpgInfo::Travel& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr);

// The route follower, called by the transport legs (a trip's onward waypoints)
TravelRoute::Point GraphNodeAt(std::uint32_t index);

// The two finishers, called by the status update's sick branch (CS14 removes both declarations)
void FinishErrands(PlayerbotAI* botAI, bool visited, char const* abortSource = "errand_abort");
void GiveTripUp(PlayerbotAI* botAI, NewRpgInfo::Travel const& data, char const* why, char const* source);

// Errands and trainers: the errand trip's helpers, called by the errand start, the errand visit and the trainer visits
bool BuysAtVendors(PlayerbotAI* botAI);
WorldTravel::ErrandPlace ErrandPlaceOf(WorldLocation const& loc);

// Trainers, called by the errand start (the trip's stops) and the errand visit (the trainer visits)
void TrainAtTrainer(PlayerbotAI* botAI, Creature* creature);
void RideAtTrainer(PlayerbotAI* botAI, Creature* creature);
void LearnAtWeaponMaster(PlayerbotAI* botAI, Creature* creature);
std::vector<TravelMgr::TrainerInfo const*> TrainerSpawns(Player* bot, Planner::UpkeepIndex const& index, uint32 key,
                                                         float reachYards);
void AddTrainerStops(std::vector<TravelMgr::TrainerInfo const*> const& spawns, uint32 service,
                     std::vector<WorldTravel::ServiceStop>& stops);

// Stable, called by the errand visit: the tame and the stable parts of an errand stop
enum class TameStep : uint8
{
    Busy,      // a cast or its channel runs: the stop holds
    Rest,      // the bot rests before a new try: other actions (food) may run
    Approach,  // the picked beast is out of reach or sight: walk to it
    Done       // tamed or given up: the stop's tame is over
};

TameStep TameAtBeast(PlayerbotAI* botAI, NewRpgInfo::ErrandStop& stop);

enum class StableStep : uint8
{
    Wait,
    Stored,
    Skipped
};

StableStep StableAtMaster(PlayerbotAI* botAI, Creature* master, NewRpgInfo::ErrandStop& stop, uint32& stored);
}  // namespace NewRpgInternal

#endif
