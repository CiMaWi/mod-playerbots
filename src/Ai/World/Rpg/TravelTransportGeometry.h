/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVEL_TRANSPORT_GEOMETRY_H
#define PLAYERBOTS_TRAVEL_TRANSPORT_GEOMETRY_H

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "GameObjectModel.h"
#include "ModelIgnoreFlags.h"
#include "MoveSpline.h"
#include "Player.h"
#include "RpgNavMeshSnap.h"
#include "Transport.h"
#include "TravelLifecycle.h"

namespace TravelTransportGeometry
{
using TravelLifecycle::DockPose;
using TravelLifecycle::Footprint;
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

inline Footprint Hull(Player const& bot, Transport const& transport)
{
    G3D::AABox const& bounds = transport.m_model->GetBounds();
    return {{bounds.low().x, bounds.low().y, bounds.low().z},
            {bounds.high().x, bounds.high().y, bounds.high().z},
            std::max(0.6f, bot.GetObjectSize())};
}

inline std::optional<float> DeckHeight(Transport const& transport, uint32 phase, Point const& point)
{
    constexpr float RISE_YARDS = 6.0f;
    constexpr float DROP_YARDS = 12.0f;
    if (!transport.m_model)
        return std::nullopt;
    float distance = RISE_YARDS + DROP_YARDS;
    G3D::Ray const ray(G3D::Vector3(point.X, point.Y, point.Z + RISE_YARDS), G3D::Vector3(0.0f, 0.0f, -1.0f));
    if (!transport.m_model->intersectRay(ray, distance, false, phase, VMAP::ModelIgnoreFlags::Nothing))
        return std::nullopt;
    return point.Z + RISE_YARDS - distance;
}

struct GroundGeometry
{
    Player const& Bot;
    Transport const* Surface = nullptr;

    std::optional<Point> SnapGround(Point point, float horizontal, float vertical) const
    {
        if (NavMeshSnap::Nearest(Bot.GetMap(), point.X, point.Y, point.Z, horizontal, vertical, NAV_GROUND) !=
            NavMeshSnap::Result::Found)
            return std::nullopt;
        return point;
    }

    std::optional<Point> ShoreAt(Point const& seed) const
    {
        auto shore = SnapGround(seed, 0.45f, TravelLifecycle::DOCK_FLOOR_YARDS);
        if (!shore)
            return std::nullopt;
        // Use the fixed collision floor at the mesh point. The dynamic tree can include the ship itself.
        float const floor = Bot.GetMap()->GetHeight(shore->X, shore->Y, shore->Z + 0.65f, true, 1.65f);
        if (std::fabs(floor - shore->Z) > 0.65f)
            return std::nullopt;
        shore->Z = floor;
        return shore;
    }

    std::optional<Point> DeckFrom(Point const& shore) const
    {
        constexpr float STEP_YARDS = 2.0f;
        constexpr float INWARD_YARDS = 4.0f;
        constexpr float PROBE_YARDS = 100.0f;
        Point const origin = PositionOf(*Surface);
        float const dx = origin.X - shore.X;
        float const dy = origin.Y - shore.Y;
        float const yards = std::hypot(dx, dy);
        if (yards > PROBE_YARDS)
            return std::nullopt;
        uint32 const phase = Bot.GetPhaseMask();
        for (float along = STEP_YARDS; along < yards; along += STEP_YARDS)
        {
            float const part = along / yards;
            auto const edge = DeckHeight(*Surface, phase, {shore.X + dx * part, shore.Y + dy * part, shore.Z});
            if (!edge)
                continue;
            float const inward = std::min((along + INWARD_YARDS) / yards, 1.0f);
            Point const deck{shore.X + dx * inward, shore.Y + dy * inward, *edge};
            if (auto const height = DeckHeight(*Surface, phase, deck))
                return Point{deck.X, deck.Y, *height};
        }
        if (auto const height = DeckHeight(*Surface, phase, {origin.X, origin.Y, shore.Z}))
            return Point{origin.X, origin.Y, *height};
        return std::nullopt;
    }

    bool OnGround(Point const& point) const
    {
        auto const ground = SnapGround(point, 0.35f, 0.65f);
        return ground && TravelLifecycle::DistanceSquared(*ground, point) <= 0.65f * 0.65f &&
               !Bot.GetMap()->IsInWater(Bot.GetPhaseMask(), point.X, point.Y, point.Z, Bot.GetCollisionHeight());
    }

    // Read the current map's ground mesh directly, with no passenger-dependent shortcut or pathfinding state change.
    bool ConnectedGround(Point const& start, Point const& end) const
    {
        MMapData& mmap = Bot.GetMap()->GetMapCollisionData().GetMMapData();
        dtNavMeshQuery const* const query = mmap.GetNavMeshQuery();
        if (!mmap.GetNavMesh() || !query)
            return false;
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND);
        filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        float const from[3] = {start.Y, start.Z, start.X};
        float const to[3] = {end.Y, end.Z, end.X};
        float const extents[3] = {0.35f, 0.65f, 0.35f};
        float fromNearest[3];
        float toNearest[3];
        dtPolyRef fromPoly = 0;
        dtPolyRef toPoly = 0;
        if (dtStatusFailed(query->findNearestPoly(from, extents, &filter, &fromPoly, fromNearest)) || !fromPoly ||
            dtStatusFailed(query->findNearestPoly(to, extents, &filter, &toPoly, toNearest)) || !toPoly)
            return false;

        std::array<dtPolyRef, 64> corridor;
        int count = 0;
        dtStatus const status = query->findPath(fromPoly, toPoly, fromNearest, toNearest, &filter, corridor.data(),
                                                &count, static_cast<int>(corridor.size()));
        if (dtStatusFailed(status) || dtStatusDetail(status, DT_PARTIAL_RESULT | DT_BUFFER_TOO_SMALL) || count == 0 ||
            corridor[count - 1] != toPoly)
            return false;
        for (int i = 0; i < count; ++i)
        {
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mmap.GetNavMesh()->getTileAndPolyByRef(corridor[i], &tile, &poly)) ||
                poly->getType() != DT_POLYTYPE_GROUND)
                return false;
        }
        return true;
    }

    bool CrossingClear(Point const& start, Point const& end) const
    {
        float const dx = end.X - start.X;
        float const dy = end.Y - start.Y;
        float const length = std::hypot(dx, dy);
        if (length < 0.1f)
            return false;
        float const radius = std::max(0.35f, Bot.GetObjectSize() * 0.5f);
        for (float const side : {0.0f, -radius, radius})
        {
            float const x = -dy / length * side;
            float const y = dx / length * side;
            constexpr float BODY_YARDS = 0.65f;
            G3D::Vector3 const a(start.X + x, start.Y + y, start.Z + BODY_YARDS);
            G3D::Vector3 const b(end.X + x, end.Y + y, end.Z + BODY_YARDS);
            if (Surface && Surface->m_model)
            {
                float distance = (b - a).length();
                G3D::Ray const ray(a, (b - a) / distance);
                if (Surface->m_model->intersectRay(ray, distance, false, Bot.GetPhaseMask(),
                                                   VMAP::ModelIgnoreFlags::Nothing))
                    return false;
            }
            if (!Bot.GetMap()->isInLineOfSight(a.x, a.y, a.z, b.x, b.y, b.z, Bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                               VMAP::ModelIgnoreFlags::Nothing))
                return false;
        }
        // A clear ray over water or empty air is only useful at a short boarding gap.
        constexpr float MAX_UNSUPPORTED_YARDS = 3.0f;
        float supportedAt = 0.0f;
        for (float distance = 1.0f; distance < length; distance += 1.0f)
        {
            float const part = distance / length;
            float const z = start.Z + (end.Z - start.Z) * part;
            float const floor = Bot.GetMap()->GetHeight(Bot.GetPhaseMask(), start.X + dx * part, start.Y + dy * part,
                                                        z + 0.65f, true, 1.65f);
            if (std::fabs(floor - z) <= 1.0f)
                supportedAt = distance;
            if (distance - supportedAt >= MAX_UNSUPPORTED_YARDS)
                return false;
        }
        return true;
    }
};

struct Exit
{
    Point Ground;
    Point Onward;
};

struct ExitFloorRejection
{
    Point Ground;
    float DeckZ;
};

// Sample eight bearings starting toward the route. Along each bearing, start just outside the padded model box.
inline std::optional<Exit> FindExit(Player const& bot, Transport const& transport, Point const& toward,
                                    std::optional<ExitFloorRejection>* rejectedFloor = nullptr)
{
    if (!Available(bot, transport))
        return std::nullopt;
    GroundGeometry const geometry{bot, &transport};
    Footprint const hull = Hull(bot, transport);
    Point const from = PositionOf(bot);
    float const angle = std::atan2(toward.Y - from.Y, toward.X - from.X);
    for (int const bearing : {0, 1, -1, 2, -2, 3, -3, 4})
    {
        float const direction = angle + static_cast<float>(bearing) * 0.78539816f;
        float const dx = std::cos(direction);
        float const dy = std::sin(direction);
        for (float distance = 2.0f; distance <= TravelLifecycle::CROSSING_YARDS; distance += 2.0f)
        {
            Point const seed{from.X + dx * distance, from.Y + dy * distance, from.Z};
            if (!hull.Outside(seed))
                continue;
            auto const ground = geometry.SnapGround(seed, 1.0f, TravelLifecycle::EXIT_SEARCH_VERTICAL_YARDS);
            if (!ground)
                continue;
            if (std::fabs(ground->Z - from.Z) > TravelLifecycle::DOCK_FLOOR_YARDS)
            {
                if (rejectedFloor && !*rejectedFloor && ground->Z < from.Z && hull.Outside(*ground))
                    *rejectedFloor = ExitFloorRejection{*ground, from.Z};
                continue;
            }
            // If the next ground waypoint is close, prove that corridor. Otherwise prove access into the platform.
            auto const routeGround =
                TravelLifecycle::DistanceSquared(*ground, toward) <= 25.0f * 25.0f && hull.Outside(toward)
                    ? geometry.SnapGround(toward, 1.0f, TravelLifecycle::DOCK_FLOOR_YARDS)
                    : std::nullopt;
            auto const onward = routeGround
                                    ? routeGround
                                    : geometry.SnapGround({ground->X + dx * 4.0f, ground->Y + dy * 4.0f, ground->Z},
                                                          1.5f, TravelLifecycle::DOCK_FLOOR_YARDS);
            if (onward && TravelLifecycle::AcceptExit(geometry, hull, from, *ground, *onward))
                return Exit{*ground, *onward};
        }
    }
    return std::nullopt;
}

inline bool ConfirmExit(Player const& bot, Transport const& transport, TravelLifecycle::PassengerMotion const& motion)
{
    return AtIssuedDock(bot, transport, motion) &&
           TravelLifecycle::AcceptExit(GroundGeometry{bot, &transport}, Hull(bot, transport), motion.ExitFrom,
                                       PositionOf(bot), motion.Onward);
}
}  // namespace TravelTransportGeometry

#endif
