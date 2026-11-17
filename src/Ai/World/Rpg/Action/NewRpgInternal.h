/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINTERNAL_H
#define PLAYERBOTS_NEWRPGINTERNAL_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "Define.h"
#include "NewRpgInfo.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelMgr.h"
#include "TravelTransportGeometry.h"

class Creature;
class Map;
class Player;
class PlayerbotAI;
class Transport;
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

// Transport legs, called by the route follower (NewRpgTransport.cpp defines them)
bool AtTransportLeg(TravelLegs::Trip const& trip);
Transport* FindLegTransport(Player* bot, TravelRoute::Leg const& leg, uint64 expected);
bool ReadyForTransportMove(Player const& bot);
bool PrepareTransportMove(Player& bot);
PassengerMotion Intent(Player const& bot, Transport const& transport, MotionKind kind, Point const& endpoint);
void IssueTransportMove(Player& bot, NewRpgInfo::Travel& data, PassengerMotion motion,
                        std::vector<Point> const& via = {});
std::optional<Outcome> TakePassengerOutcome(Player& bot, NewRpgInfo::Travel& data);
void WalkOff(Player& bot, NewRpgInfo::Travel& data, Transport const& ridden, std::optional<Point> const& waitPoint,
             std::vector<Point> const& route);
void LogExitDockFoot(Player& bot, NewRpgInfo::Travel const& data, Transport const& ridden);
std::vector<Point> OnwardRoute(TravelLegs::Trip const& trip, Map* map, uint32 mapId, Point const& dest, float deckZ);
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, float moved);
uint32 MotionQuayLeg(TravelLegs::Trip const& trip);
void SeekQuay(Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation& seen);
void LogBoardDockFoot(Player* bot, NewRpgInfo::Travel const& data, Transport& transport);
Point BoardStart(Player const& bot, TravelLegs::Trip const& trip);
char const* BoardStartName(TravelLegs::Trip const& trip);
bool SeekBoardQuay(Player* bot, NewRpgInfo::Travel& data, Transport& transport);
void LogBoardMove(Player* bot, uint32 entry, Point const& deck, char const* start);
void LogBoardGate(Player* bot, NewRpgInfo::Travel const& data, Transport const& transport,
                  std::optional<Point> const& deck, bool ready, bool accept);
void LogMisses(Player* bot, TravelLegs::Trip const& trip);
float SeeTransport(Player* bot, Transport* transport, TravelLegs::Trip const& trip, TravelLegs::TransportSight& sight);
void SendTransportNotes(Player* bot, TravelLegs::Trip& trip);
bool OffFirstBegins(Player* bot, NewRpgInfo::Travel& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr,
                    StatusEnd::Outcome outcome = StatusEnd::Outcome::Abandoned);
TravelLegs::OffStep GetOffFirst(Player* bot, NewRpgInfo::Travel& data);
void HopOnto(Player* bot, NewRpgInfo::Travel& data, Transport const& transport, Point deck, uint32 misses);
bool PassengerOwnsTravel(Player const& bot, NewRpgInfo::Travel const& data);

// The route follower, called by the transport legs (a trip's onward waypoints)
TravelRoute::Point GraphNodeAt(std::uint32_t index);

// Errands and trainers: the errand trip's helpers, called by the errand start, the errand visit and the trainer visits
bool BuysAtVendors(PlayerbotAI* botAI);
// The errand trip's needs (WU27, the visit list): the trip's kept list while the state is the one it was computed on,
// else one computed now (counted, kept with the trip); map thread only
std::shared_ptr<UpkeepNeeds::VisitNeeds const> ErrandNeeds(PlayerbotAI* botAI,
                                                           std::shared_ptr<Planner::UpkeepIndex const> const& index);
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
