/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTTRAVELINTERNAL_H
#define _PLAYERBOT_BOTTRAVELINTERNAL_H

#include <optional>
#include <string_view>

#include "BotTravel.h"
#include "Define.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Transport.h"
#include "TravelLifecycle.h"
#include "TravelTransportGeometry.h"

// The travel block's game side that PlayerbotAI.cpp and src/Ai/World/Rpg/BotTravel.cpp share; only these two files
// include this header.

struct TravelTraceIdentity
{
    uint32 Trip;
    uint32 Leg;
    uint32 Generation;
};

std::optional<TravelTraceIdentity> TravelTraceFor(NewRpgInfo::Travel const* travel);
void LogTravelGate(Player const& player, TravelTraceIdentity const& trace, uint32 nowMs, char const* gate);
char const* ContactName(TravelLifecycle::Contact contact);
char const* MotionStateName(TravelLifecycle::MotionState state);
char const* PassengerOutcomeName(TravelLifecycle::Outcome outcome);
std::string_view MountPathWord(BotTravel::MountPath path);

class PassengerAdapter
{
public:
    PassengerAdapter(Player& player, std::optional<TravelTraceIdentity> trace) : _player(player), _trace(trace) {}

    TravelLifecycle::Membership ReadMembership() const { return TravelTransportGeometry::ReadMembership(_player); }

    TravelLifecycle::SplineObservation ReadSpline() const
    {
        return TravelTransportGeometry::SplineControl{_player}.ReadSpline();
    }

    // The surface under the bot of the one vessel the caller names (the ridden one, the boarding's, the leg's or the
    // master's; WT62): no other transport is looked for
    TravelLifecycle::Surface Probe(uint64 expected, bool riding)
    {
        using TravelLifecycle::Contact;
        Transport* surface = Resolve(expected);
        if (!surface)
            return {expected, 0, Contact::Unavailable};

        uint64 const guid = surface->GetGUID().GetRawValue();
        uint32 const entry = surface->GetEntry();
        if (!surface->m_model || !surface->isSpawned() || !_player.InSamePhase(surface))
            return {guid, entry, Contact::Unavailable};
        if (!riding && _player.IsFlying())
            return {guid, entry, Contact::OffSurface};

        constexpr float RAY_RISE_YARDS = 0.5f;
        constexpr float FEET_DROP_YARDS = 0.8f;
        constexpr float AIRBORNE_DROP_YARDS = 8.0f;
        G3D::Vector3 const origin(_player.GetPositionX(), _player.GetPositionY(),
                                  _player.GetPositionZ() + RAY_RISE_YARDS);
        G3D::Ray const ray(origin, G3D::Vector3(0.0f, 0.0f, -1.0f));
        bool const airborne = riding && (_player.Unit::IsFalling() || !_player.movespline->Finalized());
        float distance = RAY_RISE_YARDS + (airborne ? AIRBORNE_DROP_YARDS : FEET_DROP_YARDS);
        if (!surface->m_model->intersectRay(ray, distance, false, _player.GetPhaseMask(),
                                            VMAP::ModelIgnoreFlags::Nothing))
            return {guid, entry, Contact::OffSurface};
        return {guid, entry, distance <= RAY_RISE_YARDS + FEET_DROP_YARDS ? Contact::Feet : Contact::AboveDeck};
    }

    TravelLifecycle::DockState ObserveDock(TravelLifecycle::PassengerMotion const& motion) const
    {
        using TravelLifecycle::DockState;
        Transport const* const transport = Resolve(motion.Transport);
        if (!transport || !TravelTransportGeometry::Available(_player, *transport))
            return DockState::Unavailable;
        return TravelLifecycle::SameDock(motion.Dock, TravelTransportGeometry::PoseOf(*transport))
                   ? DockState::Still
                   : DockState::Departed;
    }

    // An exit is confirmed where the vessel still stands as it did when the crossing was issued (WT70c: the
    // crossing is a stored row's, so no geometry is checked again)
    bool ConfirmExit(TravelLifecycle::PassengerMotion const& motion) const
    {
        Transport const* const transport = _player.GetTransport();
        return transport && TravelTransportGeometry::AtIssuedDock(_player, *transport, motion);
    }

    // WT70c (PLAN 3.21 (6)): MotionTransport::AddPassenger sets UNIT_STATE_IGNORE_PATHFINDING, and the core's
    // vessel path finder refuses such a unit. A bot aboard a vessel that holds a mesh walks its deck by that path
    // finder, so the state is cleared after the attach and again by the passenger check's sample while the bot
    // rides one (a ship's change of map adds its passengers anew). One flag test a sample; the core is untouched.
    // The debug line counts the clears (hypothesis H4)
    void KeepDeckPaths(char const* when)
    {
        Transport const* const ridden = _player.GetTransport();
        if (!ridden || !ridden->GetVesselNavMesh() || !_player.HasUnitState(UNIT_STATE_IGNORE_PATHFINDING))
            return;
        _player.ClearUnitState(UNIT_STATE_IGNORE_PATHFINDING);
        LOG_DEBUG("playerbots", "[TravelLifecycle] deck_paths bot={} entry={} map={} at={}",
                  _player.GetGUID().GetCounter(), ridden->GetEntry(), _player.GetMapId(), when);
    }

    void SettleOldFrame() { _player.StopMoving(); }

    // Ported from mod-playerbots PR #2747 (MovementAction::ClearTransportState, by Keleborn): a stale transport
    // pointer is cleared by hand before AddPassenger, which deadlocks the map on one (TravelLifecycle::StepAttach)
    void Attach(uint64 guid)
    {
        Transport* const transport = Resolve(guid);
        if (!transport)
            return;
        Transport* const prior = _player.GetTransport();
        TravelLifecycle::AttachStep const step = TravelLifecycle::StepAttach(
            prior != nullptr, prior == transport, prior && prior->GetPassengers().count(&_player) > 0);
        if (step == TravelLifecycle::AttachStep::Keep)
            return;
        if (step == TravelLifecycle::AttachStep::ClearThenAttach)
        {
            prior->RemovePassenger(&_player, true);
            if (_player.GetTransport())
            {
                _player.SetTransport(nullptr);
                _player.m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
                _player.m_movementInfo.transport.guid.Clear();
            }
        }
        transport->AddPassenger(&_player, true);
        KeepDeckPaths("attach");
    }

    void Detach(uint64 guid)
    {
        Transport* transport = _player.GetTransport();
        if (transport && transport->GetGUID().GetRawValue() == guid)
            transport->RemovePassenger(&_player, true);
    }

    void PublishFrame()
    {
        if (!_player.IsRooted())
            _player.SendMovementFlagUpdate();
    }

    template <class Reconcile>
    void Observe(uint32 nowMs, uint32 gapMs, TravelLifecycle::PassengerMotion const* motion, Reconcile reconcile)
    {
        KeepDeckPaths("sample");
        auto const observe = [&]
        {
            PerfMonitorScope scope(
                PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Travel::Maintenance") : nullptr);
            return reconcile();
        };
        if (!_trace)
        {
            observe();
            return;
        }

        auto const spline = ReadSpline();
        Transport const* transport = _player.GetTransport();
        if (!transport && motion && ReadMembership().Available)
            transport = Resolve(motion->Transport);
        uint32 const period = transport ? TravelTransportGeometry::Period(*transport) : 0;
        Position const local = _player.m_movementInfo.transport.pos;
        TravelLifecycle::Sample const sample = observe();
        TravelLifecycle::Point const endpoint = motion ? motion->Endpoint : TravelLifecycle::Point{};
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] sample bot={} trip={} leg={} generation={} tick_ms={} probe_gap_ms={} map={} "
                  "world={:.3f},{:.3f},{:.3f} local={:.3f},{:.3f},{:.3f} expected={} entry={} surface={} contact={} "
                  "motion={} spline={} frame={} endpoint={:.3f},{:.3f},{:.3f} "
                  "spline_now={} spline_finalized={} motion_result={} before={} after={} result={} available={} "
                  "period_ms={} dock={}",
                  _player.GetGUID().GetCounter(), _trace->Trip, _trace->Leg, _trace->Generation, nowMs, gapMs,
                  spline.Map, spline.Position.X, spline.Position.Y, spline.Position.Z, local.GetPositionX(),
                  local.GetPositionY(), local.GetPositionZ(), motion ? motion->Transport : 0,
                  motion ? motion->Entry : sample.Observed.Entry, sample.Observed.Guid,
                  ContactName(sample.Observed.Where),
                  motion ? (motion->Kind == TravelLifecycle::MotionKind::Board ? "board" : "exit") : "none",
                  motion ? motion->Spline : 0, spline.Local ? "local" : "world", endpoint.X, endpoint.Y, endpoint.Z,
                  spline.Id, spline.Finalized, MotionStateName(sample.Motion), sample.Before.Guid, sample.After.Guid,
                  motion ? PassengerOutcomeName(motion->Result) : "none", sample.After.Available, period,
                  sample.Dock == TravelLifecycle::DockState::Still      ? "still"
                  : sample.Dock == TravelLifecycle::DockState::Departed ? "departed"
                                                                        : "unavailable");
    }

private:
    Transport* Resolve(uint64 guid) const
    {
        Transport* transport = _player.GetTransport();
        if (!transport || transport->GetGUID().GetRawValue() != guid)
            transport = _player.GetMap()->GetTransport(ObjectGuid(guid));
        return transport && transport->IsInWorld() && transport->FindMap() == _player.FindMap() ? transport : nullptr;
    }

    Player& _player;
    std::optional<TravelTraceIdentity> _trace;
};

#endif
