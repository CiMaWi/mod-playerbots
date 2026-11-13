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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

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

// The middle of the transport's hull: the centre of its model's bounds (the world box around the turned model box has
// the box's own centre), at the transport's height. A transport's origin need not lie over its deck: a zeppelin's lies
// on its gondola's rail.
inline Point HullCentre(Transport const& transport)
{
    if (!transport.m_model)
        return PositionOf(transport);
    G3D::Vector3 const centre = transport.m_model->GetBounds().center();
    return {centre.x, centre.y, transport.GetPositionZ()};
}

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

// The transport's surface under a point: the first model hit of a ray from rise above the point down to drop below it
inline std::optional<float> DeckHeight(Transport const& transport, uint32 phase, Point const& point, float rise = 6.0f,
                                       float drop = 12.0f)
{
    if (!transport.m_model)
        return std::nullopt;
    float distance = rise + drop;
    G3D::Ray const ray(G3D::Vector3(point.X, point.Y, point.Z + rise), G3D::Vector3(0.0f, 0.0f, -1.0f));
    if (!transport.m_model->intersectRay(ray, distance, false, phase, VMAP::ModelIgnoreFlags::Nothing))
        return std::nullopt;
    return point.Z + rise - distance;
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

    std::optional<Point> ShoreAt(Point const& seed, float vertical = TravelLifecycle::DOCK_FLOOR_YARDS) const
    {
        auto shore = SnapGround(seed, 0.45f, vertical);
        if (!shore)
            return std::nullopt;
        // Use the fixed collision floor at the mesh point. The dynamic tree can include the ship itself.
        float const floor = Bot.GetMap()->GetHeight(shore->X, shore->Y, shore->Z + 0.65f, true, 1.65f);
        if (std::fabs(floor - shore->Z) > 0.65f)
            return std::nullopt;
        shore->Z = floor;
        return shore;
    }

    // Boarding's deck point lies toward the hull's centre, as the exit's does
    std::optional<Point> DeckFrom(Point const& shore) const
    {
        return DeckFrom(shore, TravelLifecycle::BOARD_DECK_INSET_YARDS, HullCentre(*Surface));
    }

    std::optional<Point> DeckFrom(Point const& shore, float inset, Point const& toward) const
    {
        uint32 const phase = Bot.GetPhaseMask();
        return TravelLifecycle::DeckPointFrom(shore, toward, inset,
                                              [&](Point const& point) { return DeckHeight(*Surface, phase, point); });
    }

    // Off the hull: the transport model has no surface under the point or within the bot's reach around it. The
    // model's bounding box cannot stand in for this: a ship's box spans its masts, rigging and bowsprit.
    bool OffHull(Point const& point) const
    {
        if (!Surface || !Surface->m_model)
            return true;
        uint32 const phase = Bot.GetPhaseMask();
        float const reach = std::max(0.6f, Bot.GetObjectSize());
        constexpr std::array<float, 5> AROUND_X = {0.0f, 1.0f, -1.0f, 0.0f, 0.0f};
        constexpr std::array<float, 5> AROUND_Y = {0.0f, 0.0f, 0.0f, 1.0f, -1.0f};
        for (std::size_t i = 0; i < AROUND_X.size(); ++i)
            if (DeckHeight(*Surface, phase, {point.X + AROUND_X[i] * reach, point.Y + AROUND_Y[i] * reach, point.Z}))
                return false;
        return true;
    }

    bool OnGround(Point const& point) const
    {
        auto const ground = SnapGround(point, 0.35f, 0.65f);
        return ground && TravelLifecycle::DistanceSquared(*ground, point) <= 0.65f * 0.65f &&
               !Bot.GetMap()->IsInWater(Bot.GetPhaseMask(), point.X, point.Y, point.Z, Bot.GetCollisionHeight());
    }

    // How the ground mesh joins two points. Connected: the search reached end. Far: the search ran out of nodes on a
    // large mesh before it reached a distant end. Apart: it exhausted a mesh island without reaching end. Off: a point
    // has no ground polygon, or the walk's first polygons are not ground.
    enum class Reach : uint8
    {
        Connected,
        Far,
        Apart,
        Off
    };

    static char const* ReachName(Reach reach)
    {
        switch (reach)
        {
            case Reach::Connected:
                return "connected";
            case Reach::Far:
                return "far";
            case Reach::Apart:
                return "apart";
            case Reach::Off:
                return "off";
        }
        return "unknown";
    }

    // Read the current map's ground mesh directly, with no passenger-dependent shortcut or pathfinding state change.
    // The stored corridor holds the walk's first polygons only (DT_BUFFER_TOO_SMALL keeps the start part).
    Reach GroundReach(Point const& start, Point const& end) const
    {
        MMapData& mmap = Bot.GetMap()->GetMapCollisionData().GetMMapData();
        dtNavMeshQuery const* const query = mmap.GetNavMeshQuery();
        if (!mmap.GetNavMesh() || !query)
            return Reach::Off;
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
            return Reach::Off;

        std::array<dtPolyRef, 64> corridor;
        int count = 0;
        dtStatus const status = query->findPath(fromPoly, toPoly, fromNearest, toNearest, &filter, corridor.data(),
                                                &count, static_cast<int>(corridor.size()));
        if (dtStatusFailed(status) || count == 0)
            return Reach::Off;
        for (int i = 0; i < count; ++i)
        {
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mmap.GetNavMesh()->getTileAndPolyByRef(corridor[i], &tile, &poly)) ||
                poly->getType() != DT_POLYTYPE_GROUND)
                return Reach::Off;
        }
        if (!dtStatusDetail(status, DT_PARTIAL_RESULT))
            return Reach::Connected;
        return dtStatusDetail(status, DT_OUT_OF_NODES) ? Reach::Far : Reach::Apart;
    }

    bool ConnectedGround(Point const& start, Point const& end) const
    {
        return GroundReach(start, end) == Reach::Connected;
    }

    // The quay leads on to the route: joined to it, or part of a mesh too large to search to a far route point.
    bool GroundLeadsOn(Point const& start, Point const& end) const
    {
        Reach const reach = GroundReach(start, end);
        return reach == Reach::Connected || reach == Reach::Far;
    }

    enum class Block : uint8
    {
        Clear,
        Short,
        Hull,
        Sight,
        Unsupported
    };

    static char const* BlockName(Block block)
    {
        switch (block)
        {
            case Block::Clear:
                return "clear";
            case Block::Short:
                return "short";
            case Block::Hull:
                return "hull";
            case Block::Sight:
                return "sight";
            case Block::Unsupported:
                return "unsupported";
        }
        return "unknown";
    }

    bool CrossingClear(Point const& start, Point const& end) const { return CrossingBlock(start, end) == Block::Clear; }

    // The walk across the deck to its edge, along the deck's surface (TravelLifecycle::DeckPath): on the deck, the
    // transport's own model is the only obstacle.
    std::optional<std::vector<Point>> DeckPath(Point const& start, Point const& end, std::uint32_t cells,
                                               std::uint32_t& expanded) const
    {
        expanded = 0;
        if (!Surface || !Surface->m_model)
            return std::vector<Point>{end};
        uint32 const phase = Bot.GetPhaseMask();
        float const radius = std::max(0.35f, Bot.GetObjectSize() * 0.5f);
        return TravelLifecycle::DeckPath(
            start, end, radius, [&](Point const& top, float drop)
            { return DeckHeight(*Surface, phase, top, 0.0f, drop); }, [&](Point const& a, Point const& b)
            { return ModelBlocks({a.X, a.Y, a.Z}, {b.X, b.Y, b.Z}); }, cells, expanded);
    }

    bool ModelBlocks(G3D::Vector3 const& a, G3D::Vector3 const& b) const
    {
        if (!Surface || !Surface->m_model)
            return false;
        float distance = (b - a).length();
        G3D::Ray const ray(a, (b - a) / distance);
        return Surface->m_model->intersectRay(ray, distance, false, Bot.GetPhaseMask(),
                                              VMAP::ModelIgnoreFlags::Nothing);
    }

    Block CrossingBlock(Point const& start, Point const& end) const
    {
        float const dx = end.X - start.X;
        float const dy = end.Y - start.Y;
        float const length = std::hypot(dx, dy);
        if (length < 0.1f)
            return Block::Short;
        float const radius = std::max(0.35f, Bot.GetObjectSize() * 0.5f);
        for (float const side : {0.0f, -radius, radius})
        {
            float const x = -dy / length * side;
            float const y = dx / length * side;
            float const level = TravelLifecycle::CrossingRayHeight(start, end);
            G3D::Vector3 const a(start.X + x, start.Y + y, level);
            G3D::Vector3 const b(end.X + x, end.Y + y, level);
            if (ModelBlocks(a, b))
                return Block::Hull;
            if (!Bot.GetMap()->isInLineOfSight(a.x, a.y, a.z, b.x, b.y, b.z, Bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                               VMAP::ModelIgnoreFlags::Nothing))
                return Block::Sight;
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
                return Block::Unsupported;
        }
        return Block::Clear;
    }
};

// The exit search runs around the transport's origin from where the bot stands, toward the route's next points; its
// deck edges lie toward the hull's centre (TravelLifecycle::FindExit).
inline std::optional<TravelLifecycle::ExitPath> FindExit(Player const& bot, Transport const& transport,
                                                         std::vector<Point> const& route,
                                                         TravelLifecycle::CrossingTally* tally = nullptr)
{
    if (!Available(bot, transport))
        return std::nullopt;
    return TravelLifecycle::FindExit(GroundGeometry{bot, &transport}, PositionOf(bot), PositionOf(transport),
                                     HullCentre(transport), route, tally);
}

inline bool ConfirmExit(Player const& bot, Transport const& transport, TravelLifecycle::PassengerMotion const& motion)
{
    return AtIssuedDock(bot, transport, motion) &&
           TravelLifecycle::AcceptExit(GroundGeometry{bot, &transport}, motion.ExitFrom, PositionOf(bot),
                                       motion.Onward);
}
}  // namespace TravelTransportGeometry

#endif
