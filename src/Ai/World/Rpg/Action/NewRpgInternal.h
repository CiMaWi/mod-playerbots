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
#include "TravelCrossingTypes.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelTransportGeometry.h"

class Map;
class Player;
class PlayerbotAI;
class Transport;

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
void IssueTransportMove(Player& bot, NewRpgInfo::Travel& data, PassengerMotion motion, Point const& first,
                        TravelCrossing::Move const& move);
std::optional<Outcome> TakePassengerOutcome(Player& bot, NewRpgInfo::Travel& data);
void FillCrossings(Player const& bot, TravelLegs::Trip const& trip, TravelLegs::Observation& seen);
void KeepOnFootAtTransport(Player* bot, TravelLegs::Trip const& trip);
bool DockPointGivenUp(Player* bot, NewRpgInfo::Travel& data);
void CrossByRow(Player* bot, NewRpgInfo::Travel& data, Transport& transport, TravelLegs::Observation const& seen);
void WalkDeckToRow(PlayerbotAI* botAI, Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation const& seen);
void TakePlace(PlayerbotAI* botAI, Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation const& seen);
void WatchDeckMove(Player* bot, NewRpgInfo::Travel& data);
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, TravelLegs::Observation const& seen);
void LogMisses(Player* bot, TravelLegs::Trip const& trip);
void SeeTransport(Player* bot, Transport* transport, TravelLegs::TransportSight& sight);
void SendTransportNotes(Player* bot, TravelLegs::Trip& trip);
bool OffFirstBegins(Player* bot, NewRpgInfo::Travel& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr,
                    StatusEnd::Outcome outcome = StatusEnd::Outcome::Abandoned);
TravelLegs::OffStep GetOffFirst(PlayerbotAI* botAI, Player* bot, NewRpgInfo::Travel& data);
bool PassengerOwnsTravel(Player const& bot, NewRpgInfo::Travel const& data);

// The route follower's graph node lookup (TravelLegs::Advance's nodeAt)
TravelRoute::Point GraphNodeAt(std::uint32_t index);
}  // namespace NewRpgInternal

#endif
