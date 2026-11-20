/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ROUTETYPES_H
#define _PLAYERBOT_ROUTETYPES_H

#include <cstdint>
#include <vector>

#include "TravelCrossingTypes.h"

// The route's plain types (RouteRules.h): a point, a leg and why a search failed. Apart from the search so
// that the travel state (NewRpgInfo.h) holds a route without the route rules.
namespace TravelRoute
{
struct Point
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

enum class LegType : std::uint8_t
{
    Walk,
    Portal,
    Flight,  // a taxi
    Transport,
    Hearth,
    Fly,          // on the bot's own flying mount
    StaticPortal  // a portal object a player clicks (graph link type 6): the follower uses it at its node
};

enum class Failure : std::uint8_t
{
    None,
    GraphNotReady,  // never waited for: no route now
    NoStartNode,    // no graph node on the bot's map (an instance, a map without travel nodes)
    NoGoalNode,
    NoRoute
};

struct Leg
{
    LegType type{LegType::Walk};
    Point from;
    Point to;
    // Graph node indices passed, in order: walk waypoints; a transport leg's stops, between the wait points where the
    // graph has them; the ends otherwise
    std::vector<std::uint32_t> nodes;
    std::uint32_t object{0};  // area trigger, portal object (static portal), first TaxiPath or transport entry
    std::vector<std::uint32_t> taxiNodes;  // flight legs: the taxi node chain for Player::ActivateTaxiPathTo
    float seconds{0.0f};
    std::uint32_t fareCopper{0};
    // Transport legs: the vessel's period in ms from the graph (0: an elevator, TravelLegs::LiftPeriodMs)
    std::uint32_t periodMs{0};
    // Fly legs: the planned flight from the takeoff on the ground to the landing on the ground
    // (FlightRoute::waypoints), its yards (climbs included), its held heights and the bot's clearance it was planned
    // with
    std::vector<Point> flightPoints;
    float flightYards{0.0f};
    std::uint32_t flightLevels{0};
    float flightClearance{0.0f};
    // Transport legs: the index in nodes of the ride's first stop, the ride's stops, and whether an end is a wait point
    // (waitFrom, waitTo: from or to is then the wait point, else the stop itself)
    std::uint32_t rideAt{0};
    Point stopFrom;
    Point stopTo;
    bool waitFrom{false};
    bool waitTo{false};
    // Transport legs: the ride's two stop records in the graph's crossing index (TravelGraph::Crossings), copied from
    // the ride's link (WT70c); NO_STOP on a leg without a ride
    std::uint32_t crossFrom{TravelCrossing::NO_STOP};
    std::uint32_t crossTo{TravelCrossing::NO_STOP};
    // A graph air link (WT61): the fly leg between its two nodes the search found (the trip leaves it out on failure)
    bool air{false};
};
}  // namespace TravelRoute

#endif
