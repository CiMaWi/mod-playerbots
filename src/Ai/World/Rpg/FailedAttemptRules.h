/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FAILEDATTEMPTRULES_H
#define _PLAYERBOT_FAILEDATTEMPTRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "WorldTravelRules.h"

// Travel's record of failed attempts (WT64c; INTERFACE 5), Player-free: one per bot, in memory, fresh at logout and
// where the RPG memos reset. Travel writes it at the typed end of a leg or a trip from the one cause table below;
// GoTo, the route request, the planner (the last two through a plain copy, CONTRACT C3) and the pickers (Offered,
// KeepOffered: the grind, camp, flight and wander npc picks, PA32) read it.
// tests/failed-attempts-selftest.cpp checks the rules. Rules is a set of static functions so the selftest's mutants
// can break one of them; the server uses Rules itself.
namespace FailedAttempts
{
using WorldTravel::SightPoint;

constexpr std::size_t MAX_ENTRIES = 16;
constexpr float PLACE_YARDS = 10.0f;  // a place, and each end of a walk off the graph, matches within this
constexpr std::string_view ABORT_PREFIX = "abort:";

enum class Kind : std::uint8_t
{
    Place,
    Link,  // a link of the travel graph, either way
    Walk,  // a walk off the graph, from its first point to its second
    Way
};

enum class Way : std::uint8_t
{
    Flying,
    Hearthstone
};

struct Key
{
    Kind kind{Kind::Place};
    SightPoint from;  // a place's point; a walk's first point
    SightPoint to;    // a walk's second point
    std::uint32_t fromNode{0};
    std::uint32_t toNode{0};
    Way way{Way::Flying};
    // A place where the bot stood (off_mesh, takeoff), or a walk refused where it began at the bot's own spot (PA56):
    // it refuses a start within PLACE_YARDS of from
    bool spot{false};
};

struct Entry
{
    Key key;
    std::string_view cause;   // the cause table's word (a literal)
    std::uint32_t lastMs{0};  // getMSTime() of the last failure
    std::uint32_t liveMs{0};  // the expiry after lastMs, fixed when written
    std::uint32_t repeats{0};
};

struct Record
{
    std::array<Entry, MAX_ENTRIES> entries{};
    std::size_t count{0};
};

// What failed, as the writer knows it at the end
enum class Leg : std::uint8_t
{
    None,          // a trip's end without a leg (no_route, not_bound)
    GraphWalk,     // a walk along a graph link
    OffGraphWalk,  // a walk off the graph
    LocalPiece,    // GoTo's local move to a place
    Ride,          // a transport leg
    Portal,
    Taxi,
    Fly,
    Hearth
};

struct Failure
{
    Leg leg{Leg::None};
    std::uint32_t fromNode{0};  // the graph link of a graph walk, a ride, a portal, a taxi path or an air link
    std::uint32_t toNode{0};
    SightPoint walkFrom;  // a walk off the graph
    SightPoint walkTo;
    SightPoint goal;      // the trip's goal, a local piece's place, the inn
    SightPoint spot;      // where the bot stood
    bool airLink{false};  // a fly leg along an air link of the graph
    bool onMesh{true};    // the bot stood on the mesh (the stuck book's condition)
    // A walk off the graph that began where the bot stood (its leg's first piece, TravelLegs::FailureOf; an order's
    // walk from the bot, TravelLegs::RefusedOrderWalk)
    bool startSide{false};
};

// Which entry a cause writes. On a fly leg every leg target writes the way flying (a fly leg that fails otherwise)
enum class Target : std::uint8_t
{
    None,
    Leg,      // the failed leg's own key: the link of a graph walk, ride, portal or taxi; the walk off the graph; a
              // local piece's place; the hearthstone on a hearth leg
    Walk,     // as Leg, on a walk only (a death)
    Ride,     // as Leg, on a ride only (a timeout of any other leg is the bot's own)
    Goal,     // the place of the goal
    Spot,     // the place where the bot stood
    Takeoff,  // the air link of a fly leg along one, else the spot
    Refusal   // as Leg; a walk off the graph refused where it began at the bot's spot also refuses starts there (PA56)
};

struct CauseRow
{
    std::string_view word;
    Target target;
    bool stuckBook;  // a report for the global stuck book (on a graph link, of a bot on the mesh)
};

// One row per word of the leg's reasons (TravelLegs::FailWord), the local Stuck causes (WorldTravel::StuckCauseWord),
// a trip's own ends and every word of GoTo::CAUSES. A new cause is a new row
inline constexpr CauseRow CAUSES[] = {
    {"no_path", Target::Leg, true},
    {"no_walk", Target::Refusal, false},
    {"no_progress", Target::Leg, true},
    {"stuck", Target::Leg, true},    // a leg's no_progress
    {"stalled", Target::Leg, true},  // a trip's no_progress
    {"door_or_elevator", Target::Leg, true},
    {"unreachable_poi", Target::Goal, false},
    {"no_route", Target::Goal, false},
    {"off_mesh", Target::Spot, false},
    {"death", Target::Walk, false},
    {"timeout", Target::Ride, false},
    {"transport_lost", Target::Leg, false},
    {"no_exit", Target::Leg, false},
    {"no_crossing", Target::Leg, false},
    {"crossing_failed", Target::Leg, false},
    {"portal", Target::Leg, false},
    {"taxi", Target::Leg, false},
    {"fare", Target::Leg, false},
    {"landing", Target::Leg, false},
    {"takeoff", Target::Takeoff, false},
    {"hearth", Target::Leg, false},
    {"not_bound", Target::Goal, false},
    {"trip_deaths", Target::Goal, false},  // the trip's third death (its leg's own death row is separate)
    {"inn_failed", Target::Goal, false},   // the inn trip's event word for not_bound
    {"on_transport", Target::None, false},
    {"fell_or_swimming", Target::None, false},
    {"stuck_aboard", Target::None, false},
    {"other", Target::None, false},
    {"no_answer", Target::None, false},
    {"no_service", Target::None, false},
    {"wrong_map", Target::None, false},
    {"unsupported", Target::None, false},
    {"deadline", Target::None, false},
    {"held", Target::None, false},
    {"stopped", Target::None, false},
    {"sick", Target::None, false},
    {"order", Target::None, false},
    {"interrupted", Target::None, false},
    {"reset", Target::None, false},
    {"abandoned", Target::None, false},
    {"arrived", Target::None, false},
    {"finished", Target::None, false},
    {"none", Target::None, false},
};

// A word bare or as a trip's abort source ("abort:no_route"); nullptr for a word without a row (it writes nothing)
constexpr CauseRow const* FindCause(std::string_view word)
{
    if (word.substr(0, ABORT_PREFIX.size()) == ABORT_PREFIX)
        word.remove_prefix(ABORT_PREFIX.size());
    for (CauseRow const& row : CAUSES)
        if (row.word == word)
            return &row;
    return nullptr;
}

enum class Phase : std::uint8_t
{
    Live,    // refused
    Repeat,  // expired, but a failure now is a repeat
    Gone     // a failure now starts afresh; the slot is free
};

enum class Noted : std::uint8_t
{
    Added,
    Repeated,
    AlreadyLive
};

inline Key LinkKey(Failure const& failure)
{
    Key key;
    key.kind = Kind::Link;
    key.fromNode = failure.fromNode;
    key.toNode = failure.toNode;
    return key;
}

struct Rules
{
    // StuckAvoidSeconds << repeats, at most STUCK_REPEAT_MAX repeats (10, 20, 40, 80 min at 600 s)
    static std::uint32_t LiveMs(std::uint32_t baseMs, std::uint32_t repeats)
    {
        return WorldTravel::StuckAvoidMs(baseMs, repeats);
    }

    // Wrap-safe: the age is the difference to now
    static Phase PhaseOf(Entry const& entry, std::uint32_t nowMs)
    {
        std::uint32_t const age = nowMs - entry.lastMs;
        if (age < entry.liveMs)
            return Phase::Live;
        if (static_cast<std::uint64_t>(age) < 2ull * entry.liveMs)
            return Phase::Repeat;
        return Phase::Gone;
    }

    // A live entry is never tried: a failure of it is the same failure told twice, and it stays as it was
    static bool TakesFailure(Phase phase) { return phase != Phase::Live; }

    static std::uint32_t NextRepeats(Phase phase, std::uint32_t repeats)
    {
        return phase == Phase::Repeat ? std::min(repeats + 1, WorldTravel::STUCK_REPEAT_MAX) : 0;
    }

    static bool Near(SightPoint const& a, SightPoint const& b) { return WorldTravel::WithinSight(a, b, PLACE_YARDS); }

    static bool SameKey(Key const& stored, Key const& asked)
    {
        if (stored.kind != asked.kind)
            return false;
        switch (stored.kind)
        {
            case Kind::Place:
                return Near(stored.from, asked.from);
            case Kind::Link:
                return (stored.fromNode == asked.fromNode && stored.toNode == asked.toNode) ||
                       (stored.fromNode == asked.toNode && stored.toNode == asked.fromNode);
            case Kind::Walk:
                return Near(stored.from, asked.from) && Near(stored.to, asked.to);
            case Kind::Way:
                return stored.way == asked.way;
        }
        return false;
    }

    // The slot a new key takes: a free one, a gone entry's, else the oldest last failure's
    static std::size_t SlotFor(Record const& record, std::uint32_t nowMs)
    {
        if (record.count < MAX_ENTRIES)
            return record.count;
        std::size_t oldest = 0;
        for (std::size_t i = 0; i < record.count; ++i)
        {
            if (PhaseOf(record.entries[i], nowMs) == Phase::Gone)
                return i;
            if (nowMs - record.entries[i].lastMs > nowMs - record.entries[oldest].lastMs)
                oldest = i;
        }
        return oldest;
    }

    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        Key key;
        switch (target)
        {
            case Target::None:
                return std::nullopt;
            case Target::Goal:
                key.from = failure.goal;
                return key;
            case Target::Spot:
                key.from = failure.spot;
                key.spot = true;
                return key;
            case Target::Takeoff:
                if (failure.leg == Leg::Fly && failure.airLink)
                    return LinkKey(failure);
                key.from = failure.spot;
                key.spot = true;
                return key;
            case Target::Leg:
            case Target::Walk:
            case Target::Ride:
            case Target::Refusal:
                break;
        }
        if (failure.leg == Leg::Fly)
        {
            key.kind = Kind::Way;
            key.way = Way::Flying;
            return key;
        }
        if (target == Target::Walk && failure.leg != Leg::GraphWalk && failure.leg != Leg::OffGraphWalk)
            return std::nullopt;
        if (target == Target::Ride && failure.leg != Leg::Ride)
            return std::nullopt;
        switch (failure.leg)
        {
            case Leg::GraphWalk:
            case Leg::Ride:
            case Leg::Portal:
            case Leg::Taxi:
                return LinkKey(failure);
            case Leg::OffGraphWalk:
                key.kind = Kind::Walk;
                key.from = failure.walkFrom;
                key.to = failure.walkTo;
                key.spot = target == Target::Refusal && failure.startSide;
                return key;
            case Leg::LocalPiece:
                key.from = failure.goal;
                return key;
            case Leg::Hearth:
                key.kind = Kind::Way;
                key.way = Way::Hearthstone;
                return key;
            default:
                return std::nullopt;
        }
    }

    static bool ReachesStuckBook(CauseRow const& row, Failure const& failure)
    {
        return row.stuckBook && failure.leg == Leg::GraphWalk && failure.onMesh;
    }

    // Only a spot the bot stood at refuses a start (a place entry's point, a refused walk's first point); every place
    // entry refuses a goal near it
    static bool RefusesStart(Key const& key) { return key.spot; }

    // A pick (PA32) offers a place unless the record refuses it (a live place entry near it) or refuses every start
    // where the bot stands (a live spot entry: off_mesh, takeoff; a walk refused at its start, PA56), as GoTo's step 2
    // would refuse the walk at once
    static bool Offers(bool placeRefused, bool startRefused) { return !placeRefused && !startRefused; }

    static void Reset(Record& record) { record.count = 0; }
};

// A failure of key at nowMs. The matching entry with the latest failure decides: live, it stays as it was; in its
// repeat window, the repeat count rises; else, and for a new key, the entry starts afresh
template <typename R = Rules>
Noted Note(Record& record, Key const& key, std::string_view cause, std::uint32_t nowMs, std::uint32_t baseMs)
{
    Entry* match = nullptr;
    for (std::size_t i = 0; i < record.count; ++i)
    {
        Entry& entry = record.entries[i];
        if (R::SameKey(entry.key, key) && (!match || nowMs - entry.lastMs < nowMs - match->lastMs))
            match = &entry;
    }
    Phase const phase = match ? R::PhaseOf(*match, nowMs) : Phase::Gone;
    if (!R::TakesFailure(phase))
        return Noted::AlreadyLive;
    if (!match)
    {
        std::size_t const slot = R::SlotFor(record, nowMs);
        if (slot == record.count)
            ++record.count;
        match = &record.entries[slot];
    }
    std::uint32_t const repeats = R::NextRepeats(phase, match->repeats);
    *match = Entry{key, cause, nowMs, R::LiveMs(baseMs, repeats), repeats};
    return phase == Phase::Repeat ? Noted::Repeated : Noted::Added;
}

// The writer's one call at a typed end: the cause's row picks the entry (none for an unknown word)
template <typename R = Rules>
std::optional<Noted> NoteFailure(Record& record, std::string_view word, Failure const& failure, std::uint32_t nowMs,
                                 std::uint32_t baseMs)
{
    CauseRow const* row = FindCause(word);
    if (!row)
        return std::nullopt;
    std::optional<Key> const key = R::KeyFor(row->target, failure);
    if (!key)
        return std::nullopt;
    return Note<R>(record, *key, row->word, nowMs, baseMs);
}

template <typename R = Rules>
bool ReachesStuckBook(std::string_view word, Failure const& failure)
{
    CauseRow const* row = FindCause(word);
    return row && R::ReachesStuckBook(*row, failure);
}

template <typename R = Rules>
bool IsRefused(Record const& record, Key const& key, std::uint32_t nowMs)
{
    for (std::size_t i = 0; i < record.count; ++i)
        if (R::SameKey(record.entries[i].key, key) && R::PhaseOf(record.entries[i], nowMs) == Phase::Live)
            return true;
    return false;
}

// The cause of a live place entry within PLACE_YARDS of a goal; empty when none refuses it
template <typename R = Rules>
std::string_view IsPlaceRefused(Record const& record, SightPoint const& place, std::uint32_t nowMs)
{
    Key asked;
    asked.from = place;
    for (std::size_t i = 0; i < record.count; ++i)
    {
        Entry const& entry = record.entries[i];
        if (R::SameKey(entry.key, asked) && R::PhaseOf(entry, nowMs) == Phase::Live)
            return entry.cause;
    }
    return {};
}

// The cause of a live entry that refuses a start within PLACE_YARDS of spot: a spot the bot stood at (off_mesh,
// takeoff) or a walk refused where it began at the bot's spot (no_walk, PA56); empty when none does. No trip starts
// there, so the bot's next act is one that leaves the spot without a trip, or the entry's expiry
template <typename R = Rules>
std::string_view StartRefusedBy(Record const& record, SightPoint const& spot, std::uint32_t nowMs)
{
    for (std::size_t i = 0; i < record.count; ++i)
    {
        Entry const& entry = record.entries[i];
        if (R::RefusesStart(entry.key) && R::Near(entry.key.from, spot) && R::PhaseOf(entry, nowMs) == Phase::Live)
            return entry.cause;
    }
    return {};
}

template <typename R = Rules>
bool RefusesStartAt(Record const& record, SightPoint const& spot, std::uint32_t nowMs)
{
    return !StartRefusedBy<R>(record, spot, nowMs).empty();
}

// The pickers' one question (PA32): may a pick offer this place, the bot standing at here? The record is all it
// reads, no second memory beside it: a goal whose route was none, a refused approach's place and a refused start are
// all entries of it (one writer, BotTravel::NoteFailure)
template <typename R = Rules>
bool Offered(Record const& record, SightPoint const& place, SightPoint const& here, std::uint32_t nowMs)
{
    return R::Offers(!IsPlaceRefused<R>(record, place, nowMs).empty(), RefusesStartAt<R>(record, here, nowMs));
}

// A picker's candidates without those the record refuses, in their order; O(MAX_ENTRIES) per candidate and nothing to
// do on an empty record. placeOf gives a candidate's place, std::nullopt where it is not known (kept, unless no start
// is offered at all: the caller's own checks pass it over). Empty afterwards is the picker's "none available", never
// a refused place
template <typename R = Rules, typename T, typename PlaceOf>
void KeepOffered(Record const& record, std::vector<T>& candidates, SightPoint const& here, std::uint32_t nowMs,
                 PlaceOf const& placeOf)
{
    if (record.count == 0)
        return;
    bool const startRefused = RefusesStartAt<R>(record, here, nowMs);
    auto const refused = [&](T const& candidate)
    {
        std::optional<SightPoint> const place = placeOf(candidate);
        return !R::Offers(place && !IsPlaceRefused<R>(record, *place, nowMs).empty(), startRefused);
    };
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), refused), candidates.end());
}

// The plain copy a worker thread reads (the route request, the planner's snapshot)
template <typename R = Rules>
std::vector<Entry> LiveEntries(Record const& record, std::uint32_t nowMs)
{
    std::vector<Entry> live;
    for (std::size_t i = 0; i < record.count; ++i)
        if (R::PhaseOf(record.entries[i], nowMs) == Phase::Live)
            live.push_back(record.entries[i]);
    return live;
}

template <typename R = Rules>
void Reset(Record& record)
{
    R::Reset(record);
}
}  // namespace FailedAttempts

#endif
