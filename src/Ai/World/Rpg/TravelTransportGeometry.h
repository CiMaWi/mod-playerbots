/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVEL_TRANSPORT_GEOMETRY_H
#define PLAYERBOTS_TRAVEL_TRANSPORT_GEOMETRY_H

#include <cstdint>
#include <optional>

#include "MoveSpline.h"
#include "Player.h"
#include "Transport.h"
#include "TravelLifecycle.h"

namespace TravelTransportGeometry
{
using TravelLifecycle::DockPose;
using TravelLifecycle::Point;

inline Point PositionOf(WorldObject const& object)
{
    return {object.GetPositionX(), object.GetPositionY(), object.GetPositionZ()};
}

inline DockPose PoseOf(Transport const& transport) { return {PositionOf(transport), transport.GetOrientation()}; }

struct SplineControl
{
    Player& Bot;

    TravelLifecycle::SplineObservation ReadSpline() const
    {
        Movement::MoveSpline const& spline = *Bot.movespline;
        bool const eligible = !Bot.IsInCombat() && Bot.IsAlive() && !Bot.HasUnitState(UNIT_STATE_LOST_CONTROL);
        return {Bot.GetMapId(), spline.GetId(), spline.onTransport, spline.Finalized(), eligible, PositionOf(Bot)};
    }

    void SettleOldFrame() { Bot.StopMoving(); }
};

inline void DiscardMotion(Player& bot, std::optional<TravelLifecycle::PassengerMotion>& intent)
{
    SplineControl control{bot};
    TravelLifecycle::DiscardMotion(intent, control);
}

inline uint32 Period(Transport const& transport)
{
    if (MotionTransport const* const ship = transport.ToMotionTransport())
        return ship->GetPeriod();
    if (StaticTransport const* const platform = transport.ToStaticTransport())
        return platform->GetPeriod();
    return 0;
}

inline TravelLifecycle::Membership ReadMembership(Player const& bot)
{
    if (!bot.FindMap() || !bot.IsInWorld() || bot.IsBeingTeleported() || bot.GetVehicle() || bot.IsInFlight())
        return {};
    Transport const* const current = bot.GetTransport();
    ObjectGuid const guid = bot.m_movementInfo.transport.guid;
    bool const flag = bot.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    if (!current)
        return {0, 0, !guid && !flag};
    return {current->GetGUID().GetRawValue(), current->GetEntry(),
            flag && guid == current->GetGUID() && current->IsInWorld() && current->FindMap() == bot.FindMap()};
}

inline bool Available(Player const& bot, Transport const& transport)
{
    return transport.IsInWorld() && transport.FindMap() == bot.FindMap() && transport.m_model &&
           transport.isSpawned() && bot.InSamePhase(&transport);
}

inline bool AtIssuedDock(Player const& bot, Transport const& transport, TravelLifecycle::PassengerMotion const& motion)
{
    return bot.GetMapId() == motion.Map && transport.GetGUID().GetRawValue() == motion.Transport &&
           Available(bot, transport) && TravelLifecycle::SameDock(motion.Dock, PoseOf(transport));
}
}  // namespace TravelTransportGeometry

#endif
