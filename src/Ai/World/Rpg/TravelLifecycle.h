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

namespace TravelLifecycle
{
constexpr std::uint32_t PASSENGER_INTERVAL_MS = 1000;

struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

constexpr float CROSSING_YARDS = 25.0f;
constexpr float EXIT_SEARCH_VERTICAL_YARDS = 10.0f;
constexpr float DOCK_FLOOR_YARDS = 2.0f;

inline float DistanceSquared(Point const& a, Point const& b)
{
    float const dx = a.X - b.X;
    float const dy = a.Y - b.Y;
    float const dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

struct DockPose
{
    Point Position;
    float Orientation = 0.0f;
};

inline bool SameDock(DockPose const& issued, DockPose const& current)
{
    return DistanceSquared(issued.Position, current.Position) <= 0.2f * 0.2f &&
           std::cos(issued.Orientation - current.Orientation) >= 0.99995f;
}

struct Footprint
{
    Point Low;
    Point High;
    float Clearance = 0.0f;

    bool Outside(Point const& point) const
    {
        return point.X < Low.X - Clearance || point.X > High.X + Clearance || point.Y < Low.Y - Clearance ||
               point.Y > High.Y + Clearance;
    }
};

template <class Geometry>
bool AcceptBoard(Geometry const& geometry, Point const& shore, Point const& deck)
{
    return std::fabs(shore.Z - deck.Z) <= DOCK_FLOOR_YARDS &&
           DistanceSquared(shore, deck) <= CROSSING_YARDS * CROSSING_YARDS && geometry.OnGround(shore) &&
           geometry.CrossingClear(shore, deck);
}

template <class Geometry>
std::optional<Point> FindQuay(Geometry const& geometry, Point const& approach, Point const& origin)
{
    auto const tryShore = [&](Point const& seed) -> std::optional<Point>
    {
        auto const shore = geometry.ShoreAt(seed);
        if (!shore)
            return std::nullopt;
        auto const deck = geometry.DeckFrom(*shore);
        if (deck && AcceptBoard(geometry, *shore, *deck) && geometry.ConnectedGround(approach, *shore))
            return shore;
        return std::nullopt;
    };
    if (auto const shore = tryShore(approach))
        return shore;

    // Sample around the model horizontally, using the normal approach's floor as the height reference.
    constexpr unsigned BEARINGS = 24;
    constexpr float BEARING_STEP = 0.26179939f;
    for (unsigned bearing = 0; bearing < BEARINGS; ++bearing)
    {
        float const angle = static_cast<float>(bearing) * BEARING_STEP;
        float const dx = std::cos(angle);
        float const dy = std::sin(angle);
        for (float distance = 1.0f; distance <= CROSSING_YARDS; distance += 1.0f)
            if (auto const shore = tryShore({origin.X + dx * distance, origin.Y + dy * distance, approach.Z}))
                return shore;
    }
    return std::nullopt;
}

// The mesh adapter supplies actual ground and a complete ground corridor. The deck crossing is checked separately.
template <class Geometry>
bool AcceptExit(Geometry const& geometry, Footprint const& hull, Point const& from, Point const& ground,
                Point const& onward)
{
    return hull.Outside(ground) && hull.Outside(onward) && std::fabs(ground.Z - from.Z) <= DOCK_FLOOR_YARDS &&
           DistanceSquared(from, ground) <= CROSSING_YARDS * CROSSING_YARDS && geometry.OnGround(ground) &&
           DistanceSquared(ground, onward) >= 2.0f * 2.0f && geometry.CrossingClear(from, ground) &&
           geometry.ConnectedGround(ground, onward);
}

enum class MotionKind : std::uint8_t
{
    Board,
    Exit
};
enum class MotionState : std::uint8_t
{
    None,
    Moving,
    Reached,
    Cancelled
};
enum class Outcome : std::uint8_t
{
    Pending,
    Boarded,
    Exited,
    Cancelled,
    Lost
};
enum class Contact : std::uint8_t
{
    Unavailable,
    OffSurface,
    AboveDeck,
    Feet
};
enum class DockState : std::uint8_t
{
    Unavailable,
    Still,
    Departed
};

struct PassengerMotion
{
    MotionKind Kind = MotionKind::Board;
    std::uint64_t Transport = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Map = 0;
    std::uint32_t Spline = 0;
    Point Endpoint;
    Outcome Result = Outcome::Pending;
    bool CompletionSampled = false;
    bool ContactOnly = false;  // the existing unseen hop has no issued spline
    DockPose Dock;
    Point ExitFrom;
    Point Onward;
};

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
    if (!spline.Eligible || spline.Map != motion.Map || (!motion.ContactOnly && !MatchesSpline(motion, spline)))
        return MotionState::Cancelled;
    if (!spline.Finalized)
        return motion.ContactOnly ? MotionState::Cancelled : MotionState::Moving;

    constexpr float ENDPOINT_TOLERANCE_YARDS = 0.8f;
    return DistanceSquared(spline.Position, motion.Endpoint) <= ENDPOINT_TOLERANCE_YARDS * ENDPOINT_TOLERANCE_YARDS
               ? MotionState::Reached
               : MotionState::Cancelled;
}

template <class Adapter>
void SettleMotion(PassengerMotion const& motion, Adapter& adapter)
{
    if (!motion.ContactOnly)
    {
        SplineObservation const spline = adapter.ReadSpline();
        if (!spline.Finalized && MatchesSpline(motion, spline))
            adapter.SettleOldFrame();
    }
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

struct Cadence
{
    std::uint32_t LastProbeMs = 0;
    std::uint32_t Map = 0;
    bool Sampled = false;
    bool Revalidate = false;
};

struct Membership
{
    std::uint64_t Guid = 0;
    std::uint32_t Entry = 0;
    bool Available = false;
};

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
};

struct Sample
{
    Membership Before;
    Membership After;
    Surface Observed;
    MotionState Motion = MotionState::None;
    DockState Dock = DockState::Unavailable;
};

// The adapter resolves transport identities and applies the frame change on the bot's map thread.
template <class Adapter>
Sample Reconcile(Adapter& adapter, PassengerMotion* motion, MotionState state, std::uint64_t preferred = 0)
{
    Sample sample;
    sample.Before = sample.After = adapter.ReadMembership();
    sample.Motion = state;
    if (!sample.Before.Available)
        return sample;

    if (motion && state == MotionState::Cancelled)
        SettleMotion(*motion, adapter);

    if (motion && motion->Kind == MotionKind::Exit && sample.Before.Guid != motion->Transport)
    {
        SettleMotion(*motion, adapter);
        motion->Result = Outcome::Lost;
        return sample;
    }

    if (motion && (state == MotionState::Moving || state == MotionState::Reached))
    {
        sample.Dock = adapter.ObserveDock(*motion);
        if (sample.Dock == DockState::Unavailable)
            return sample;
        if (sample.Dock == DockState::Departed)
        {
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

    std::uint64_t const expected = motion ? motion->Transport : preferred;
    sample.Observed = adapter.Probe(sample.Before.Guid ? sample.Before.Guid : expected, sample.Before.Guid != 0);

    if (sample.Before.Guid && sample.Observed.Guid == sample.Before.Guid &&
        sample.Observed.Where == Contact::OffSurface && !preferred)
    {
        bool const safeExit = motion && motion->Kind == MotionKind::Exit && state == MotionState::Reached &&
                              motion->Transport == sample.Before.Guid && adapter.ConfirmExit(*motion);
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
    else if (!sample.Before.Guid && sample.Observed.Guid && (!expected || sample.Observed.Guid == expected) &&
             (!motion || state == MotionState::Reached) && sample.Observed.Where == Contact::Feet)
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
void Maintain(Cadence& cadence, std::uint32_t nowMs, std::uint32_t map, PassengerMotion* motion, Adapter& adapter)
{
    if (motion && motion->Result != Outcome::Pending)
        motion = nullptr;

    MotionState const state = motion ? ClassifyMotion(*motion, adapter.ReadSpline()) : MotionState::None;
    bool const completed =
        motion && !motion->CompletionSampled && (state == MotionState::Reached || state == MotionState::Cancelled);
    std::uint32_t const gapMs = cadence.Sampled ? nowMs - cadence.LastProbeMs : 0;
    if (cadence.Sampled && gapMs < PASSENGER_INTERVAL_MS && !completed && !cadence.Revalidate && cadence.Map == map)
        return;

    cadence = {nowMs, map, true, false};
    if (completed)
        motion->CompletionSampled = true;

    adapter.Observe(nowMs, gapMs, motion, [&] { return Reconcile(adapter, motion, state); });
}
}  // namespace TravelLifecycle

#endif
