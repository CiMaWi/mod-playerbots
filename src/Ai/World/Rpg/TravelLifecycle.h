/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVEL_LIFECYCLE_H
#define PLAYERBOTS_TRAVEL_LIFECYCLE_H

#include <cmath>
#include <cstdint>
#include <optional>

#include "TravelLifecycleTypes.h"

namespace TravelLifecycle
{
constexpr std::uint32_t PASSENGER_INTERVAL_MS = 1000;
// A rider without a move of its own (no boarding or exit spline) leaves its deck on this many samples off it in a row:
// one probe off the surface while it stands still is the hull's model a step away from its passengers (WT54-6: two
// riders of 187568 dropped at sea on the same sample), not an exit
constexpr std::uint8_t OFF_SURFACE_SAMPLES = 2;
// A spot within this of the vessel's mesh stands on its deck (WT70e-a; TravelLegs::OFF_DECK_YARDS)
constexpr float DECK_MESH_YARDS = 0.5f;

// PA39b (G341): a bot on no transport becomes a passenger without a boarding move of its own only where it stands on
// the vessel's deck: the ray under its feet hits the model and its would-be offset lies within DECK_MESH_YARDS of the
// vessel's mesh. A hull swept over a bot that waits at the dock hits the ray inside the hull, yards off the deck, where
// no deck path leads to any leaving row. meshYards < 0: no mesh or no answer, the ray decides. PA69 (G161): a ship's
// or zeppelin's hull still under way (Surface::UnderWay) takes no one: arriving, it sweeps over the bot that waits at
// the dock and holds it on a rail whose mesh lies within the half yard but joins no deck path; docked, its hull lies
// off the dock points and the bot boards by a row
constexpr bool StandsOnDeck(Contact ray, float meshYards, bool underWay = false)
{
    return !underWay && ray == Contact::Feet && (meshYards < 0.0f || meshYards <= DECK_MESH_YARDS);
}

inline float DistanceSquared(Point const& a, Point const& b)
{
    float const dx = a.X - b.X;
    float const dy = a.Y - b.Y;
    float const dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

inline bool SameDock(DockPose const& issued, DockPose const& current)
{
    return DistanceSquared(issued.Position, current.Position) <= 0.2f * 0.2f &&
           std::cos(issued.Orientation - current.Orientation) >= 0.99995f;
}

struct SplineObservation
{
    std::uint32_t Map = 0;
    std::uint32_t Id = 0;
    bool Local = false;
    bool Finalized = false;
    bool Eligible = false;
    Point Position;
};

inline bool MatchesSpline(PassengerMotion const& motion, SplineObservation const& spline)
{
    return spline.Map == motion.Map && spline.Id == motion.Spline && spline.Local == (motion.Kind == MotionKind::Exit);
}

inline MotionState ClassifyMotion(PassengerMotion const& motion, SplineObservation const& spline)
{
    if (!spline.Eligible || spline.Map != motion.Map || !MatchesSpline(motion, spline))
        return MotionState::Cancelled;
    if (!spline.Finalized)
        return MotionState::Moving;

    constexpr float ENDPOINT_TOLERANCE_YARDS = 0.8f;
    return DistanceSquared(spline.Position, motion.Endpoint) <= ENDPOINT_TOLERANCE_YARDS * ENDPOINT_TOLERANCE_YARDS
               ? MotionState::Reached
               : MotionState::Cancelled;
}

template <class Adapter>
void SettleMotion(PassengerMotion const& motion, Adapter& adapter)
{
    SplineObservation const spline = adapter.ReadSpline();
    if (!spline.Finalized && MatchesSpline(motion, spline))
        adapter.SettleOldFrame();
}

template <class Adapter>
void DiscardMotion(std::optional<PassengerMotion>& intent, Adapter& adapter)
{
    if (intent)
        SettleMotion(*intent, adapter);
    intent.reset();
}

template <class Adapter>
void ResetMotion(std::optional<PassengerMotion>& intent, bool continuingTrip, Adapter& adapter)
{
    if (!intent)
        return;
    SettleMotion(*intent, adapter);
    if (!continuingTrip || intent->Kind != MotionKind::Board || intent->Result != Outcome::Boarded)
        intent.reset();
}

struct Membership
{
    std::uint64_t Guid = 0;
    std::uint32_t Entry = 0;
    bool Available = false;
};

// Before a passenger attach (ported from mod-playerbots PR #2747, MovementAction::ClearTransportState, by Keleborn):
// the core's AddPassenger deadlocks the map when the bot still points at a transport whose passenger set lacks it, and
// RemovePassenger clears the pointer only for a listed passenger. A listed passenger of the target stays; any other
// prior transport frame is cleared first.
enum class AttachStep : std::uint8_t
{
    Attach,
    ClearThenAttach,
    Keep
};

inline AttachStep StepAttach(bool hasPrior, bool priorIsTarget, bool listedOnPrior)
{
    if (!hasPrior)
        return AttachStep::Attach;
    return priorIsTarget && listedOnPrior ? AttachStep::Keep : AttachStep::ClearThenAttach;
}

inline std::optional<Outcome> PassengerOutcome(PassengerMotion const& motion, Membership const& membership)
{
    if (motion.Result == Outcome::Pending || !membership.Available)
        return std::nullopt;
    if (motion.Result == Outcome::Boarded && (membership.Guid != motion.Transport || membership.Entry != motion.Entry))
        return Outcome::Lost;
    return motion.Result;
}

struct Surface
{
    std::uint64_t Guid = 0;
    std::uint32_t Entry = 0;
    Contact Where = Contact::Unavailable;
    float MeshYards = -1.0f;  // a bot on no transport with its feet on the model: its would-be offset off the mesh
    // A bot on no transport: the vessel is a ship or zeppelin under way between its stops (PA69, G161): its arriving
    // hull sweeps over the bot at the dock
    bool UnderWay = false;
};

struct Sample
{
    Membership Before;
    Membership After;
    Surface Observed;
    MotionState Motion = MotionState::None;
    DockState Dock = DockState::Unavailable;
};

// The adapter resolves transport identities and applies the frame change on the bot's map thread. `before` is the
// membership the caller has just read (Reconcile reads it itself).
template <class Adapter>
Sample ReconcileFrom(Membership const& before, Adapter& adapter, PassengerMotion* motion, MotionState state,
                     std::uint64_t preferred, std::uint8_t* offSurfaceSamples)
{
    Sample sample;
    sample.Before = sample.After = before;
    sample.Motion = state;
    if (!sample.Before.Available)
        return sample;

    if (motion && state == MotionState::Cancelled)
        SettleMotion(*motion, adapter);

    // An exit whose vessel membership is gone before this sample: it left where its move ran to its issued end, off
    // any vessel, while the vessel still stood at the issued dock, whoever cleared the membership first (G396: the
    // core before the sample, walked or jumped); away from its end or on another vessel it is lost
    if (motion && motion->Kind == MotionKind::Exit && sample.Before.Guid != motion->Transport)
    {
        SettleMotion(*motion, adapter);
        bool const exited = !sample.Before.Guid && state == MotionState::Reached && adapter.ExitAtIssuedDock(*motion);
        motion->Result = exited ? Outcome::Exited : Outcome::Lost;
        return sample;
    }

    if (motion && (state == MotionState::Moving || state == MotionState::Reached))
    {
        sample.Dock = adapter.ObserveDock(*motion);
        if (sample.Dock == DockState::Unavailable)
            return sample;
        if (sample.Dock == DockState::Departed)
        {
            // A jump by a stored row is never stopped in the air (WT70c): it runs out, then it counts as cancelled
            if (motion->Jump && state == MotionState::Moving)
                return sample;
            // Only a still-owned active spline can be stopped here. A replacement belongs to its new owner.
            SettleMotion(*motion, adapter);
            state = sample.Motion = MotionState::Cancelled;
        }
    }
    if (motion && state == MotionState::Cancelled)
        motion->Result = Outcome::Cancelled;

    // The issued spline stays in its frame until it completes or another owner replaces it.
    if (state == MotionState::Moving)
        return sample;

    // Boarding by intent (WT62, WT.48): a bot on no transport is probed only for the vessel it means to board (its
    // move's, its leg's or its master's); a bot with none is never taken aboard
    std::uint64_t const expected = motion ? motion->Transport : preferred;
    std::uint64_t const probed = sample.Before.Guid ? sample.Before.Guid : expected;
    if (!probed)
        return sample;
    sample.Observed = adapter.Probe(probed, sample.Before.Guid != 0);

    // A rider the vessel's mesh holds where the ray finds no deck (OnMesh) stays while it stands; a move of its own
    // that ended there (an exit by a row) is judged by the ray alone (WT70e-a)
    Contact const where = sample.Observed.Where;
    bool const offSurface = sample.Before.Guid && sample.Observed.Guid == sample.Before.Guid &&
                            (where == Contact::OffSurface || (where == Contact::OnMesh && motion)) && !preferred;
    bool const leaves = offSurface && (motion || !offSurfaceSamples || ++*offSurfaceSamples >= OFF_SURFACE_SAMPLES);
    if (offSurfaceSamples && (!offSurface || leaves))
        *offSurfaceSamples = 0;
    if (leaves)
    {
        bool const safeExit = motion && motion->Kind == MotionKind::Exit && state == MotionState::Reached &&
                              motion->Transport == sample.Before.Guid && adapter.ExitAtIssuedDock(*motion);
        adapter.SettleOldFrame();
        adapter.Detach(sample.Before.Guid);
        sample.After = adapter.ReadMembership();
        if (sample.After.Available && !sample.After.Guid)
        {
            adapter.PublishFrame();
            if (motion)
                motion->Result = safeExit ? Outcome::Exited : Outcome::Lost;
        }
    }
    else if (!sample.Before.Guid && sample.Observed.Guid == expected && (!motion || state == MotionState::Reached) &&
             sample.Observed.Where == Contact::Feet &&
             (motion || StandsOnDeck(sample.Observed.Where, sample.Observed.MeshYards, sample.Observed.UnderWay)))
    {
        adapter.SettleOldFrame();
        adapter.Attach(sample.Observed.Guid);
        sample.After = adapter.ReadMembership();
        if (sample.After.Available && sample.After.Guid == sample.Observed.Guid)
            adapter.PublishFrame();
    }

    if (motion && motion->Result == Outcome::Pending && state == MotionState::Reached)
    {
        if (motion->Kind == MotionKind::Board && sample.After.Available && sample.After.Guid == motion->Transport &&
            sample.Observed.Where == Contact::Feet)
            motion->Result = Outcome::Boarded;
        else if (sample.Observed.Where != Contact::Unavailable && sample.After.Available)
            motion->Result = Outcome::Cancelled;
    }
    return sample;
}

template <class Adapter>
Sample Reconcile(Adapter& adapter, PassengerMotion* motion, MotionState state, std::uint64_t preferred = 0,
                 std::uint8_t* offSurfaceSamples = nullptr)
{
    return ReconcileFrom(adapter.ReadMembership(), adapter, motion, state, preferred, offSurfaceSamples);
}

// The vessel a bot on no transport means to board (WT62, WT.48), the first one named: a test's aboard placement, its
// transport leg's sighted vessel, its master's transport (a follower; a lift or the tram as well as a ship), and the
// one narrow rule for a bot with neither leg nor master: Strand of the Ancients' nearer boat. 0: none, no probe
struct IntentSources
{
    std::uint64_t Aboard = 0;
    std::uint64_t Leg = 0;
    std::uint64_t Master = 0;
    std::uint64_t Battleground = 0;
};

constexpr std::uint64_t ChooseIntent(IntentSources const& sources)
{
    if (sources.Aboard)
        return sources.Aboard;
    if (sources.Leg)
        return sources.Leg;
    return sources.Master ? sources.Master : sources.Battleground;
}

// The passenger check, once a PASSENGER_INTERVAL_MS per bot. `intent` is the vessel the bot means to board
// (ChooseIntent; 0 none); it counts only while the bot rides nothing
template <class Adapter>
void Maintain(Cadence& cadence, std::uint32_t nowMs, std::uint32_t map, PassengerMotion* motion, Adapter& adapter,
              std::uint64_t intent = 0)
{
    if (motion && motion->Result != Outcome::Pending)
        motion = nullptr;

    MotionState const state = motion ? ClassifyMotion(*motion, adapter.ReadSpline()) : MotionState::None;
    bool const completed =
        motion && !motion->CompletionSampled && (state == MotionState::Reached || state == MotionState::Cancelled);
    std::uint32_t const gapMs = cadence.Sampled ? nowMs - cadence.LastProbeMs : 0;
    if (cadence.Sampled && gapMs < PASSENGER_INTERVAL_MS && !completed && !cadence.Revalidate && cadence.Map == map)
        return;

    cadence = {nowMs, map, true, false, cadence.OffSurface};
    if (completed)
        motion->CompletionSampled = true;

    // A bot on no transport with no move of its own and no vessel to board has nothing to observe (WT62)
    Membership const before = adapter.ReadMembership();
    if (!before.Guid && !motion && !intent)
    {
        cadence.OffSurface = 0;
        return;
    }
    std::uint64_t const boarding = before.Guid ? 0 : intent;
    adapter.Observe(nowMs, gapMs, motion,
                    [&] { return ReconcileFrom(before, adapter, motion, state, boarding, &cadence.OffSurface); });
}
}  // namespace TravelLifecycle

#endif
