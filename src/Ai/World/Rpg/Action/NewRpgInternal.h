/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINTERNAL_H
#define PLAYERBOTS_NEWRPGINTERNAL_H

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "Define.h"
#include "HeldTrip.h"
#include "NewRpgInfo.h"
#include "StandRules.h"
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

// Transport legs, called by the test orders (`route aboard`; at: the named offset on the deck, null: by the first row)
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry, TravelCrossing::Spot const* at = nullptr);

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
// PA36 trace (debug log only): a transport leg's leave start and ride on past its stop, and an approach that has not
// reached its dock after 30 s. sightBefore: the trip's last scan before this tick's step
void TraceTransportStep(Player* bot, TravelLegs::Trip const& trip, TravelLegs::Observation const& seen,
                        TravelLegs::Act act, uint32 sightBefore);
void LogMisses(Player* bot, TravelLegs::Trip const& trip);
void SeeTransport(Player* bot, Transport* transport, TravelLegs::TransportSight& sight);
void SendTransportNotes(Player* bot, TravelLegs::Trip& trip);
bool OffFirstBegins(Player* bot, HeldTrip& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr,
                    StatusEnd::Outcome outcome = StatusEnd::Outcome::Abandoned);
TravelLegs::OffStep GetOffFirst(PlayerbotAI* botAI, Player* bot, HeldTrip& data);
// The get-off of a bot aboard a vessel with no trip (PA39b, TravelLegs::DecideVesselLeave): true while it waits or
// moves, the status waits
bool VesselLeaveStep(PlayerbotAI* botAI, Player* bot);
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
// The executor on a field order (questing WQ19a, `route field`): DoQuest in order mode at x, y of the bot's map for
// `minutes`, expecting every open need of the log with a field-map row there; false while the indexes build
bool StartFieldOrder(PlayerbotAI* botAI, Player* bot, float x, float y, float minutes);
// The executor on a way (questing WQ20, `route way`): DoQuest in order mode to x, y of the bot's map, ended by the
// arrival, taking what the log needs within `allowance` yards of the way (QuestFieldExec::OnOrder)
bool StartWayOrder(PlayerbotAI* botAI, Player* bot, float x, float y, float allowance);
// The quest route's orders (questing WQ18b, PlannerService's StartRoute). OrderNeedOpen: the test of whether a need
// an order expects is open for the bot now (GoalApply::AdvanceRoute's `open`), over one count of the bot's log.
// StartRouteOrder: DoQuest in order mode on the route's order, its Status row's word `word`; false while the quest
// index builds or for an order of another map. MarkOrderReplaced: the executor's running order, when it is this one,
// ends as replaced at its next tick (true) or goes on (false)
std::function<bool(Planner::QuestOrderNeed const&)> OrderNeedOpen(PlayerbotAI* botAI, Player* bot);
bool StartRouteOrder(PlayerbotAI* botAI, Player* bot, Planner::QuestOrder const& order, char const* word);
void MarkOrderReplaced(PlayerbotAI* botAI, Planner::QuestOrder const& order, bool replaced);
// questing WQ18b part 4: whether a ready quest's ender stands elsewhere in the order's field than where the bot stands
// (QuestFieldExec::EnderElsewhere over the quest index's enders): the test a no_ender end is named by
std::function<bool(uint32)> OrderEnderElsewhere(Player* bot, Planner::QuestOrder const& order);
// Right after a DoQuest start (an order, a pick): ends it at once when the step function refuses the quest or finds
// no place (QuestExec::EndWord), before any walk or fight; true when it ended
bool EndQuestAtStart(PlayerbotAI* botAI, Player* bot);
// The quests the bot gave up ClassQuests::GIVE_UP_LIMIT times (QuestFailRecord): the picker and the planner skip them
std::vector<uint32> GivenUpQuests(Player* bot);

// The route follower, called by the transport legs (a trip's onward waypoints)
TravelRoute::Point GraphNodeAt(std::uint32_t index);
// A walk off the graph as the bot starts it (NewRpgTravelWalk.cpp; WT56, WT64i): TravelWalk::StartPiece on the judge's
// reader, GoTo::ProbeCarried; per probe step a straight path that aims and the order's smooth path that is judged
// (TravelWalk::OrderStep), done within arriveYards (the waypoint's reach), on the bot's map thread
TravelWalk::PieceWalk ProbeWalkPiece(Player* bot, TravelRoute::Point const& from, TravelRoute::Point const& to,
                                     float arriveYards, bool fragment, bool tilesLoaded);
// A refused walk off the graph's trace words (NewRpgTravelWalk.cpp; pathing PA55): whether the mesh joins the bot's
// spot and the walk's end, and the spot's nearest polygon; one wide search, so the info log's refusal line only
std::string RefusedWalkMeshWords(Player* bot, TravelRoute::Point const& to);
// The stand points a target's rule must leave free (NewRpgGoTo.cpp; pathing PA47): the other players and the points
// claimed in the last minute within StandRules::OccupantYards of its place, whatever they were asked for. Every caller
// of NavMeshSnap::StandPoint for a bot asks these and claims the point it walks to (ClaimStand); bot's map thread
std::vector<std::array<float, 3>> StandOccupants(Player* bot, StandRules::Target const& target, uint32 now);
void ClaimStand(Player* bot, float x, float y, float z, uint32 now);
// One engine path call as the probe's steps and an approach's judge read it (NewRpgTravelWalk.cpp; pathing PA4, PA5):
// the result, the path's type, its points (forceDest off) and why it fell short; wide: the map's wide query
TravelWalk::StepAnswer AskTravelPath(Player* bot, TravelWalk::ProbePoint const& start,
                                     TravelWalk::ProbePoint const& end, bool straight, bool wide);
}  // namespace NewRpgInternal

#endif
