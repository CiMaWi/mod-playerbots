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
#include "HeldTrip.h"
#include "NewRpgInfo.h"
#include "TravelCrossingTypes.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelTransportGeometry.h"

class Map;
class Player;
class PlayerbotAI;
class Transport;
struct Position;

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
constexpr char const* ABOARD_EVENT_TYPE = "Aboard";  // its event (refused here, placed or failed by BotTravel)

// Transport legs, called by the route follower (NewRpgTransport.cpp defines them)
bool AtTransportLeg(TravelLegs::Trip const& trip);
Transport* FindLegTransport(Player* bot, TravelRoute::Leg const& leg, uint64 expected);
bool ReadyForTransportMove(Player const& bot);
bool PrepareTransportMove(Player& bot);
PassengerMotion Intent(Player const& bot, Transport const& transport, MotionKind kind, Point const& endpoint);
void IssueTransportMove(Player& bot, std::optional<PassengerMotion>& slot, PassengerMotion motion, Point const& first,
                        TravelCrossing::Move const& move);
// A crossing as issued: the bot's start, the row's two points in the order crossed, the start's yards and the move
struct CrossingPlan
{
    Point from;
    Point first;
    Point end;
    float startYards{0.0f};
    TravelCrossing::Move move;
};
CrossingPlan IssueCrossing(Player& bot, std::optional<PassengerMotion>& slot, Transport const& transport,
                           TravelCrossing::Row const& row, TravelCrossing::Direction direction);
std::optional<Outcome> TakePassengerOutcome(Player& bot, HeldTrip& data);
void FillCrossings(Player const& bot, TravelLegs::Trip const& trip, TravelLegs::Observation& seen);
void KeepOnFootAtTransport(Player* bot, TravelLegs::Trip const& trip);
bool DockPointGivenUp(Player* bot, HeldTrip& data);
void CrossByRow(Player* bot, HeldTrip& data, Transport& transport, TravelLegs::Observation const& seen);
void WalkDeckToRow(PlayerbotAI* botAI, Player* bot, HeldTrip& data, TravelLegs::Observation const& seen);
void TakePlace(PlayerbotAI* botAI, Player* bot, HeldTrip& data, TravelLegs::Observation const& seen);
void WatchDeckMove(Player* bot, HeldTrip& data);
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, TravelLegs::Observation const& seen, TravelLegs::Act act);
void LogMisses(Player* bot, TravelLegs::Trip const& trip);
void SeeTransport(Player* bot, Transport* transport, TravelLegs::TransportSight& sight);
void SendTransportNotes(Player* bot, TravelLegs::Trip& trip);
bool OffFirstBegins(Player* bot, HeldTrip& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr,
                    StatusEnd::Outcome outcome = StatusEnd::Outcome::Abandoned);
TravelLegs::OffStep GetOffFirst(PlayerbotAI* botAI, Player* bot, HeldTrip& data);
bool PassengerOwnsTravel(Player const& bot, HeldTrip const& data);

// A follower at its master's vessel (BotTravel::FollowAboard, WT70d)
TravelLegs::FollowAct FollowAboardStep(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing& follow,
                                       std::optional<PassengerMotion>& motion, Position& walkTo);
bool FollowAboardDue(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow);
std::optional<uint32> FollowAboardPaceMs(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow);
uint32 TransportLegPaceMs(Player const& bot, TravelLegs::Trip const& trip, uint32 staggerMs);
std::optional<uint32> TravelScanPaceMs(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow,
                                       std::optional<TravelLifecycle::PassengerMotion> const& followMotion);
void TracePassengerEnd(PlayerbotAI* botAI, Player* bot, TravelLifecycle::PassengerMotion const& motion,
                       TravelLifecycle::Outcome before);
bool FollowWalkEnded(Player* bot, TravelLegs::FollowCrossing& follow, float yards, bool refused);
// A bot whose transport leg starts wakes its followers once (TravelLegs::WakeFollowers, WT70d-b)
void WakeFollowers(Player* bot, TravelLegs::Trip& trip);
bool HasStopRecords(Transport const& transport);

// The quest executor, called by the test orders (`route quest`): DoQuest for a quest in the bot's log, else false
bool StartOrderedQuest(PlayerbotAI* botAI, Player* bot, uint32 questId);

// The route follower, called by the transport legs (a trip's onward waypoints)
TravelRoute::Point GraphNodeAt(std::uint32_t index);
// A walk off the graph as the bot starts it (NewRpgTravelWalk.cpp; WT56, WT64i): TravelWalk::StartPiece on the judge's
// reader, MeshMove::ProbeAccepts, one straight-path call per probe step on the bot's map thread
TravelWalk::PieceWalk ProbeWalkPiece(Player* bot, TravelRoute::Point const& from, TravelRoute::Point const& to,
                                     bool fragment, bool tilesLoaded);
}  // namespace NewRpgInternal

#endif
