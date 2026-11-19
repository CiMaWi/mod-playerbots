/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TRAVELCROSSINGTYPES_H
#define _PLAYERBOT_TRAVELCROSSINGTYPES_H

// The stored crossings of a vessel's stops (playerbots_travelnode_crossing; WT70c, WT.53), engine-free: where a bot
// steps from the dock onto the deck and back, as the navigation plan's graph build measured it. The travel graph's
// loader builds the index once (TravelGraphBuilder::AddCrossing, EndLinks) and publishes it with the graph; nothing
// writes it afterwards, so any thread reads it without locks. A transport leg carries the indices of its two stop
// records (TravelRoute::Leg::crossFrom, crossTo) and reads its rows from them: no search, no ray cast at run time.
// tests/travel-graph-selftest.cpp drives the loader's rules and the jump's numbers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace TravelCrossing
{
constexpr std::uint32_t NO_STOP = std::numeric_limits<std::uint32_t>::max();  // no stop record (an index)
constexpr std::size_t MAX_ROWS = 3;           // rows kept per stop and direction, in seq order
constexpr float STOP_NODE_YARDS = 3.0f;       // a ride link's node lies this near its stop's pose
constexpr float JUMP_SPEED = 7.96f;           // a player's jump: the speed upwards at the takeoff, yd/s
constexpr float JUMP_GRAVITY = 19.29f;        // yd/s^2 (the core's Movement::gravity)
constexpr float JUMP_RUN_YARDS_PER_S = 7.0f;  // the run speed that carries a jump over its gap

enum class Kind : std::uint8_t
{
    Walk = 0,
    Jump = 1
};

enum class Direction : std::uint8_t
{
    Board = 0,
    Leave = 1
};

struct Spot
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// One crossing: between the dock point (a world point on the world's mesh) and the deck point (an offset in the
// vessel's frame, on the vessel's mesh). gap and rise as stored: the yards across, and deck z minus dock z
struct Row
{
    Kind kind{Kind::Walk};
    std::uint8_t seq{0};  // the table's rank: 0 = nearest the wait point
    Spot dock;
    Spot deck;
    float gap{0.0f};
    float rise{0.0f};
};

// One stop of one vessel: a ship or zeppelin (guid 0: one per entry) or one car of a lift or tram (guid = its spawn)
struct Stop
{
    std::uint32_t entry{0};
    std::uint32_t guid{0};
    std::uint32_t stop{0};  // the table's stop number, in path order per vessel
    std::uint32_t mapId{0};
    float x{0.0f};  // the vessel's pose while it stands here
    float y{0.0f};
    float z{0.0f};
    float o{0.0f};
    // Ships and zeppelins: the stop's window in the vessel's period, from the core's stop frame at the pose (the
    // period's ms at which it arrives and leaves; arriveMs may be the larger one where the window wraps the period's
    // end). A looping car: the span of its animation in which it stands here (WT70d); a car with a pause time has none
    bool hasWindow{false};
    std::uint32_t arriveMs{0};
    std::uint32_t departMs{0};
    std::uint32_t periodMs{0};
    std::uint8_t boardCount{0};
    std::uint8_t leaveCount{0};
    Row board[MAX_ROWS];
    Row leave[MAX_ROWS];

    std::span<Row const> Rows(Direction direction) const
    {
        return direction == Direction::Board ? std::span<Row const>(board, boardCount)
                                             : std::span<Row const>(leave, leaveCount);
    }
};

// The amount of a Transport event that names a row (cross, crossed, cross_failed, cross_cut, no_crossing)
constexpr std::uint32_t EventAmount(std::uint32_t stop, Direction direction, std::uint8_t seq)
{
    return stop * 100 + static_cast<std::uint32_t>(direction) * 10 + seq;
}

// A player's jump over a crossing: its time in the air and the amplitude of the spline's parabola
struct Jump
{
    float seconds{0.0f};
    float amplitude{0.0f};
};

// The highest rise a jump reaches (1.64 yd)
constexpr float JumpApex() { return JUMP_SPEED * JUMP_SPEED / (2.0f * JUMP_GRAVITY); }

// The jump that ends rise yards above (below: negative) its start: z = vt - gt^2/2 reaches rise on the way down at
// T = (v + sqrt(v^2 - 2gr)) / g; the spline's parabola over a straight line then needs the amplitude gT^2/8
// (JumpHeight). False: the rise is above the apex
inline bool JumpOf(float rise, Jump& jump)
{
    float const under = JUMP_SPEED * JUMP_SPEED - 2.0f * JUMP_GRAVITY * rise;
    if (under < 0.0f)
        return false;
    jump.seconds = (JUMP_SPEED + std::sqrt(under)) / JUMP_GRAVITY;
    jump.amplitude = JUMP_GRAVITY * jump.seconds * jump.seconds / 8.0f;
    return true;
}

// The height above the start that the core's spline gives a jump t seconds after the parabola began: the straight
// line's share of the rise plus the parabola's elevation (MoveSpline::computeParabolicElevation: (T - t) * a / 2 * t
// with a = amplitude * 8 / T^2)
inline float JumpHeight(Jump const& jump, float rise, float t)
{
    float const acceleration = jump.amplitude * 8.0f / (jump.seconds * jump.seconds);
    return rise * t / jump.seconds + (jump.seconds - t) * 0.5f * acceleration * t;
}

// The rise of a row's jump: boarding goes from the dock up to the deck, leaving the other way
constexpr float RiseOf(Row const& row, Direction direction)
{
    return direction == Direction::Board ? row.rise : -row.rise;
}

// A jump a player makes: the rise is within the apex and the run carries it over the gap in the jump's time (5.8 yd
// level, 2.9 yd at the apex)
inline bool IsPlayerJump(float rise, float gap)
{
    Jump jump;
    return JumpOf(rise, jump) && gap <= JUMP_RUN_YARDS_PER_S * jump.seconds;
}

constexpr float PATH_SKIP_YARDS = 0.2f;  // a crossing's first point this near the bot is left out of its path

// A crossing as the leg issues it, one spline: from the bot over the row's first point (the run-up; left out within
// PATH_SKIP_YARDS) to its second: dock then deck boarding, deck then dock leaving. A walk runs that path at the bot's
// run speed. A jump takes the whole spline at the speed that covers the crossing's straight line in the jump's time,
// and its parabola begins after the run-up's share
struct Move
{
    bool jump{false};
    bool runUp{false};         // the path has its first point
    float speed{0.0f};         // yd/s for the whole spline (SetVelocity); 0: the unit's own run speed
    float amplitude{0.0f};     // the parabola's (SetParabolic)
    float startSeconds{0.0f};  // and its start time
    float seconds{0.0f};       // the whole move's time: the stand test asks for this much, plus its slack
};

// rise: end z minus start z; runUpYards: from the bot to the first point; acrossYards: the straight line between the
// two points. A jump no longer than PATH_SKIP_YARDS, or one above the apex (never loaded), is a step: a walk
inline Move MoveOf(Kind kind, float rise, float runUpYards, float acrossYards)
{
    Move move;
    move.runUp = runUpYards > PATH_SKIP_YARDS;
    float const run = move.runUp ? runUpYards : 0.0f;
    Jump jump;
    if (kind == Kind::Jump && acrossYards > PATH_SKIP_YARDS && JumpOf(rise, jump))
    {
        move.jump = true;
        move.speed = acrossYards / jump.seconds;
        move.amplitude = jump.amplitude;
        move.startSeconds = run / move.speed;
        move.seconds = move.startSeconds + jump.seconds;
        return move;
    }
    move.seconds = (run + acrossYards) / JUMP_RUN_YARDS_PER_S;
    return move;
}

// By a stored row: its gap is the yards across on the flat, its rise the height between its two points
inline Move MoveOf(Row const& row, Direction direction, float runUpYards)
{
    return MoveOf(row.kind, RiseOf(row, direction), runUpYards, std::sqrt(row.gap * row.gap + row.rise * row.rise));
}

// The stop records, sorted by (entry, guid, stop). A record's index is what a ride link and a leg carry
class Index
{
public:
    std::size_t Size() const { return _stops.size(); }
    std::span<Stop const> All() const { return _stops; }

    // The record of an index; null for NO_STOP or an index past the end
    Stop const* At(std::uint32_t index) const { return index < _stops.size() ? &_stops[index] : nullptr; }

    std::uint32_t IndexOf(Stop const& stop) const { return static_cast<std::uint32_t>(&stop - _stops.data()); }

    // Every stop of an entry, its cars in guid order (a ship has one car, guid 0)
    std::span<Stop const> StopsOf(std::uint32_t entry) const
    {
        auto const first = std::lower_bound(_stops.begin(), _stops.end(), entry,
                                            [](Stop const& stop, std::uint32_t value) { return stop.entry < value; });
        auto const last = std::upper_bound(first, _stops.end(), entry,
                                           [](std::uint32_t value, Stop const& stop) { return value < stop.entry; });
        return {_stops.data() + (first - _stops.begin()), static_cast<std::size_t>(last - first)};
    }

    // The stops of one car
    std::span<Stop const> StopsOf(std::uint32_t entry, std::uint32_t guid) const
    {
        auto const less = [](Stop const& stop, std::pair<std::uint32_t, std::uint32_t> const& car)
        { return stop.entry != car.first ? stop.entry < car.first : stop.guid < car.second; };
        auto const more = [](std::pair<std::uint32_t, std::uint32_t> const& car, Stop const& stop)
        { return car.first != stop.entry ? car.first < stop.entry : car.second < stop.guid; };
        std::pair<std::uint32_t, std::uint32_t> const car{entry, guid};
        auto const first = std::lower_bound(_stops.begin(), _stops.end(), car, less);
        auto const last = std::upper_bound(first, _stops.end(), car, more);
        return {_stops.data() + (first - _stops.begin()), static_cast<std::size_t>(last - first)};
    }

    // The stop a car stands at: a binary search to the car, then its few stops by the caller's pose test (the
    // passenger check's TravelLifecycle::SameDock against the vessel's pose now, so the index adds no tolerance of its
    // own). Null when it stands at none
    template <typename AtPose>
    Stop const* StopAt(std::uint32_t entry, std::uint32_t guid, AtPose const& atPose) const
    {
        for (Stop const& stop : StopsOf(entry, guid))
            if (atPose(stop))
                return &stop;
        return nullptr;
    }

    // The loader's only write (TravelGraphBuilder::Finish), before the graph is published
    void Assign(std::vector<Stop>&& stops) { _stops = std::move(stops); }

    std::size_t MemoryBytes() const { return _stops.capacity() * sizeof(Stop); }

private:
    std::vector<Stop> _stops;
};
}  // namespace TravelCrossing

#endif
