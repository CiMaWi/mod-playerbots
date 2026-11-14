/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELPROGRESS_H
#define PLAYERBOTS_TRAVELPROGRESS_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "TravelLegRules.h"

namespace TravelProgress
{
constexpr std::uint32_t WALK_BUDGET_MS = 90'000;
constexpr std::uint32_t SAMPLE_MS = 1'000;
constexpr std::uint32_t OFF_MESH_PROBE_MS = 2 * SAMPLE_MS;
constexpr float ADVANCE_YARDS = 5.0f;
constexpr float SPLINE_POSITION_TOLERANCE_YARDS = 1.0f;

enum class Owner : std::uint8_t
{
    Walk,
    Leg,
    Loot,
    Combat,
    Control,
    Exempt
};

enum class Overdue : std::uint8_t
{
    None,
    Walk,
    OffMesh,
    RouteAnswer,
    Transport,
    LegDeadline,
    RouteDeadline
};

enum class Work : std::uint8_t
{
    Continue,
    Hold,
    Recover,
    DeferLoot
};

inline Work ScheduledWork(bool canRecover, Overdue reason, bool lootOwns, bool lootExpired)
{
    if (canRecover && reason != Overdue::None)
        return Work::Recover;
    if (lootOwns)
        return canRecover && lootExpired ? Work::DeferLoot : Work::Hold;
    return Work::Continue;
}

inline char const* ResumeWord(Owner owner)
{
    switch (owner)
    {
        case Owner::Loot:
            return "loot";
        case Owner::Combat:
            return "combat";
        case Owner::Control:
            return "control";
        default:
            return nullptr;
    }
}

struct Motion
{
    std::uint32_t Id{0};
    bool Active{false};
    float Yards{0.0f};  // position on the current core spline
    bool PositionMatches{true};
    // Value identity only: resolved on the map thread, never dereferenced or persisted.
    std::uintptr_t Generator{0};
    TravelRoute::Point Destination{};
};

struct Sample
{
    bool Selected{false};
    bool CombatChanged{false};
    bool LootCompleted{false};
    float Yards{0.0f};
};

class State
{
public:
    Sample Observe(std::uint32_t now, Owner owner, bool combat, std::uint32_t lootCompleted)
    {
        Sample sample{!_observed, combat != _combat, _observed && lootCompleted != _lootCompleted};
        if (_observed && _timeValid && owner == _owner)
        {
            std::uint32_t const elapsed = now - _lastMs;
            if (owner == Owner::Walk)
            {
                AddTime(_walkMs, elapsed);
                AddTime(_routeMs, elapsed);
            }
            if (owner == Owner::Walk || owner == Owner::Leg)
                AddTime(_legMs, elapsed);
        }
        if (owner == Owner::Combat || (ResumeWord(owner) && _resume != Owner::Combat))
            _resume = owner;
        if (owner != Owner::Walk || !_timeValid)
            _rebase = true;
        if (owner == Owner::Combat || owner == Owner::Control || owner == Owner::Loot)
            _interruptedWalk = true;
        _observed = true;
        _timeValid = true;
        _owner = owner;
        _combat = combat;
        _lastMs = now;
        _lootCompleted = lootCompleted;

        if (sample.LootCompleted)
            _walkMs = 0;
        return sample;
    }

    void Suspend()
    {
        _ownsMotion = false;
        _generator = 0;
        _timeValid = false;
        _rebase = true;
        _interruptedWalk = _observed;
    }

    bool SampleDue(std::uint32_t now) const { return !_sampled || now - _sampleMs >= SAMPLE_MS; }
    void NoteSample(std::uint32_t now)
    {
        _sampled = true;
        _sampleMs = now;
    }

    void CheckWalkBudget()
    {
        if (_owner == Owner::Walk && _walkMs >= WALK_BUDGET_MS)
            Latch(Overdue::Walk);
    }

    void AttemptWalk() { _attemptedWalk = true; }
    bool AttemptedWalk() const { return _attemptedWalk; }
    bool ShouldProbeOffMesh()
    {
        if (_probed || _walkMs < OFF_MESH_PROBE_MS || _overdue != Overdue::None)
            return false;
        _probed = true;
        return true;
    }

    bool HasMotion() const { return _ownsMotion; }
    bool Owns(std::uint32_t id) const { return _ownsMotion && _motion == id; }
    bool SameGenerator(Motion const& motion) const
    {
        return _ownsMotion && motion.Generator && _generator == motion.Generator &&
               _destination.mapId == motion.Destination.mapId && _destination.x == motion.Destination.x &&
               _destination.y == motion.Destination.y && _destination.z == motion.Destination.z;
    }

    void CommitMotion(Motion const& motion)
    {
        // A new spline establishes a baseline; issuing it is not movement.
        _creditedYards = motion.Yards;
        _ownsMotion = motion.Generator != 0;
        _generator = motion.Generator;
        _destination = motion.Destination;
        _motion = motion.Id;
        _lastYards = motion.Yards;
        _rebase = false;
    }

    float ObserveMotion(Motion const& motion)
    {
        if (!SameGenerator(motion))
        {
            _ownsMotion = false;
            _rebase = true;
            return 0.0f;
        }
        if (_owner != Owner::Walk || !motion.PositionMatches)
        {
            _rebase = true;
            return 0.0f;
        }
        // Speed changes and Resume relaunch the same core point generator outside our action callback.
        if (!Owns(motion.Id))
        {
            CommitMotion(motion);
            return 0.0f;
        }
        if (_rebase)
            _creditedYards = motion.Yards;
        bool const advanced = !_rebase && motion.Yards > _lastYards;
        _rebase = false;
        _lastYards = motion.Yards;
        if (advanced)
            _interruptedWalk = false;

        float const gained = motion.Yards - _creditedYards;
        if (!advanced || gained < ADVANCE_YARDS)
            return 0.0f;
        _creditedYards = motion.Yards;
        _walkMs = 0;
        return gained;
    }

    bool WaypointReached()
    {
        bool const walked = !_interruptedWalk;
        if (walked)
            _walkMs = 0;
        ClearWalk();
        return walked;
    }

    void StartLeg(bool previousCompleted)
    {
        _legMs = 0;
        if (previousCompleted)
        {
            _walkMs = 0;
            _interruptedWalk = false;
        }
        ClearWalk();
    }

    void AcceptRoute()
    {
        _routeMs = 0;
        StartLeg(false);
    }

    void Recover()
    {
        _walkMs = 0;
        ClearWalk();
    }

    char const* TakeResume() { return ResumeWord(std::exchange(_resume, Owner::Exempt)); }

    std::uint32_t RouteMs() const { return _routeMs; }
    std::uint32_t LegMs() const { return _legMs; }
    Overdue Reason() const { return _overdue; }
    void Latch(Overdue reason)
    {
        if (_overdue == Overdue::None)
            _overdue = reason;
    }
    Overdue TakeReason() { return std::exchange(_overdue, Overdue::None); }

private:
    static void AddTime(std::uint32_t& total, std::uint32_t elapsed)
    {
        total += std::min(elapsed, std::numeric_limits<std::uint32_t>::max() - total);
    }

    void ClearWalk()
    {
        _ownsMotion = false;
        _attemptedWalk = false;
        _probed = false;
        _generator = 0;
    }

    Owner _owner{Owner::Exempt};
    Owner _resume{Owner::Exempt};
    Overdue _overdue{Overdue::None};
    bool _observed{false};
    bool _timeValid{false};
    bool _combat{false};
    bool _sampled{false};
    bool _rebase{true};
    bool _interruptedWalk{false};
    bool _ownsMotion{false};
    bool _attemptedWalk{false};
    bool _probed{false};
    std::uint32_t _lastMs{0};
    std::uint32_t _sampleMs{0};
    std::uint32_t _lootCompleted{0};
    std::uint32_t _walkMs{0};
    std::uint32_t _routeMs{0};
    std::uint32_t _legMs{0};
    std::uint32_t _motion{0};
    float _lastYards{0.0f};
    std::uintptr_t _generator{0};
    TravelRoute::Point _destination{};
    float _creditedYards{0.0f};
};

struct Observation
{
    std::uint32_t NowMs;
    Owner Current;
    bool Combat;
    std::uint32_t LootCompleted;
};

template <class Read>
Sample Maintain(State& progress, Observation const& seen, TravelLegs::Trip const& trip,
                TravelLegs::RouteWait const& route, float routeSeconds, Read&& read)
{
    Sample sample = progress.Observe(seen.NowMs, seen.Current, seen.Combat, seen.LootCompleted);
    if (!progress.SampleDue(seen.NowMs))
        return sample;
    progress.NoteSample(seen.NowMs);

    if (progress.HasMotion())
        sample.Yards = progress.ObserveMotion(read());

    auto const* leg = trip.legIdx < trip.legs.size() ? &trip.legs[trip.legIdx] : nullptr;
    if (route.waiting && TravelLegs::WaitExpired(route, seen.NowMs))
        progress.Latch(Overdue::RouteAnswer);
    else if (TravelLegs::OffFirstExpired(trip.offFirst, seen.NowMs) ||
             (leg && leg->type == TravelRoute::LegType::Transport &&
              (trip.passengerGuid ? TravelLegs::TransportRideExpired(trip, *leg, seen.NowMs)
                                  : TravelLegs::TransportWaitExpired(trip, *leg, seen.NowMs))))
        progress.Latch(Overdue::Transport);
    else if (seen.Current == Owner::Walk || seen.Current == Owner::Leg)
    {
        progress.CheckWalkBudget();
        if (leg && progress.LegMs() > TravelLegs::LegTimeoutMs(*leg))
            progress.Latch(Overdue::LegDeadline);
        if (leg && TravelLegs::PastDeadline(progress.RouteMs(), routeSeconds))
            progress.Latch(Overdue::RouteDeadline);
    }
    return sample;
}

template <class RouteWait, class Submit>
bool SubmitRoute(RouteWait& wait, RouteWait const& request, Submit&& submit)
{
    if (!submit())
        return false;
    wait = request;
    return true;
}

template <class Read, class Move, class Resumed>
bool ContinueWalk(State& state, Read&& read, Move&& move, Resumed&& resumed)
{
    state.AttemptWalk();
    Motion const before = read();
    bool const used = move();
    Motion const after = read();
    if (used && after.Active && after.Generator && before.Id != after.Id)
        state.CommitMotion(after);
    else if (after.Active && state.SameGenerator(after) && !state.Owns(after.Id))
        state.CommitMotion(after);
    if (after.Active && state.Owns(after.Id) && state.SameGenerator(after))
    {
        if (char const* const reason = state.TakeResume())
            resumed(reason);
    }
    return used;
}
}  // namespace TravelProgress

#endif
