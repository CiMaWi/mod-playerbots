/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HELD_TRIP_H
#define PLAYERBOTS_HELD_TRIP_H

#include <optional>
#include <string>

#include "Define.h"
#include "ObjectGuid.h"
#include "TravelLegTypes.h"
#include "TravelLifecycleTypes.h"
#include "TravelMgr.h"
#include "TravelProgressState.h"

// The trip a status holds (WT64f; PLAN 3.20 (3), INTERFACE 1): the route's legs and the place on them, the route
// request waited for and the trip's progress. One per bot, in its travel block (BotTravel::Trip), not in the status
// data: the status core (NewRpgStatusEnd.cpp) begins it with its holder's status and ends it before that status ends
// or is replaced. The route follower (NewRpgTravelAction) steps it
struct HeldTrip
{
    WorldPosition dest{};
    TravelLegs::Trip trip;        // the route's legs and the place on them; no legs: a route is due
    TravelLegs::RouteWait route;  // the route request waited for
    TravelProgress::State progress;
    uint32 failedMs{0};           // the last failed leg: the new route is asked for REPLAN_DELAY_MS later
    float estimateSeconds{0.0f};  // the first route's
    uint32 tripStartMs{0};
    float routeSeconds{0.0f};
    std::string place;     // a GM order's place (`playerbots route go`): its first route is logged
    bool bindInn{false};   // an inn errand: at the end the bot binds at the innkeeper in front of it
    uint8 bindTries{0};    // an inn errand: binds tried in front of the innkeeper
    ObjectGuid innKeeper;  // an inn errand: the innkeeper a bind out of his sight walks up to (WT63e-b)
    uint32 tripId{0};      // the world log's trip id (WL7)
    // A transport leg's last scan (TravelLegs::TRANSPORT_SCAN_MS)
    uint32 transportScanMs{0};
    // A transport leg's deck move under way (the place aboard, the walk to a leaving row's deck point): its
    // spline's id, 0 none. At its end the corner-cut watch looks once (WatchDeckMove: off_deck)
    uint32 deckSpline{0};
    WorldPosition walkTo;  // the far walk's destination (NewRpgTravelAction::MoveFarTo); empty: none, or given up
    bool stepped{false};   // the follower has stepped the trip: its walk clock runs from then (TravelProgress::OwnerOf)
    // The follower's judged order (NewRpgTravelWalk.cpp, WT64g): where the last accepted order to walkTo was given,
    // the orders in a row that moved nothing (TravelWalk::StillOrder), and a refusal MoveFarTo ends the leg with
    WorldPosition walkOrderAt;
    uint8 walkStill{0};
    TravelLegs::FailReason walkRefused{TravelLegs::FailReason::None};
    std::optional<TravelLifecycle::PassengerMotion> passengerMotion;

    TravelRoute::Leg const* CurrentLeg() const
    {
        return trip.legIdx < trip.legs.size() ? &trip.legs[trip.legIdx] : nullptr;
    }
};

#endif
