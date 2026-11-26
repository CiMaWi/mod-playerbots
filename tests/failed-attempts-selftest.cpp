/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of travel's record of failed attempts (WT64c; PLAN 3.20 (6)(7), INTERFACE 5): the expiry (10, 20, 40,
// 80 min and its cap), a repeat only within as long again after the expiry, a live entry left as it was, wrap-safe
// times, a spot's entry refusing a start there and a goal near it, 16 entries with the oldest first out, the four
// kinds' matching, a row for every word of FailWord, StuckCauseWord, the trip's own ends and GoTo::CAUSES, the entry
// each cause writes, which causes reach the stuck book, the reset and the plain copy, and the pickers' reading of it
// (PA32: a refused goal or approach place is not offered again, a refused spot offers none; PA56: a walk refused where
// it began at the bot's spot refuses every start there, as a spot does, until it expires). The numbers follow
// INTERFACE 5, not the memos the record replaces: a place matches within 10 yd (the stuck avoid widened from 20 yd with
// repeats, the inn memo used 15 yd and 60 min). Each mutant (one rule broken on purpose) must fail the suite. Build
// (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel
//   tests/failed-attempts-selftest.cpp

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include "FailedAttemptRules.h"
#include "GoToRules.h"
#include "TravelLegRules.h"
#include "WorldTravelRules.h"

namespace
{
using FailedAttempts::CauseRow;
using FailedAttempts::Entry;
using FailedAttempts::Failure;
using FailedAttempts::Key;
using FailedAttempts::Kind;
using FailedAttempts::Leg;
using FailedAttempts::Noted;
using FailedAttempts::Phase;
using FailedAttempts::Record;
using FailedAttempts::Rules;
using FailedAttempts::Target;
using FailedAttempts::Way;
using WorldTravel::SightPoint;

constexpr std::uint32_t MINUTE = 60 * 1000;
constexpr std::uint32_t BASE = 10 * MINUTE;  // StuckAvoidSeconds 600

struct Counts
{
    int checks{0};
    int failures{0};
    bool loud{true};

    void Check(bool pass, char const* what)
    {
        ++checks;
        if (pass)
            return;
        ++failures;
        if (loud)
            std::printf("FAIL: %s\n", what);
    }
};

SightPoint At(float x, float y, std::uint32_t mapId = 0) { return SightPoint{mapId, x, y, 0.0f}; }

Key PlaceKey(SightPoint const& point)
{
    Key key;
    key.from = point;
    return key;
}

Key LinkKey(std::uint32_t from, std::uint32_t to)
{
    Key key;
    key.kind = Kind::Link;
    key.fromNode = from;
    key.toNode = to;
    return key;
}

Key WalkKey(SightPoint const& from, SightPoint const& to)
{
    Key key;
    key.kind = Kind::Walk;
    key.from = from;
    key.to = to;
    return key;
}

Key WayKey(Way way)
{
    Key key;
    key.kind = Kind::Way;
    key.way = way;
    return key;
}

bool Same(SightPoint const& a, SightPoint const& b) { return a.MapId == b.MapId && a.X == b.X && a.Y == b.Y; }

// The failure every cause row is resolved against: distinct goal, spot, walk ends and link nodes
SightPoint const GOAL = At(1000.0f, 0.0f);
SightPoint const SPOT = At(0.0f, 1000.0f);
SightPoint const WALK_FROM = At(500.0f, 500.0f);
SightPoint const WALK_TO = At(600.0f, 500.0f);
constexpr std::uint32_t NODE_A = 3;
constexpr std::uint32_t NODE_B = 4;

Failure On(Leg leg, bool airLink = false)
{
    Failure failure;
    failure.leg = leg;
    failure.fromNode = NODE_A;
    failure.toNode = NODE_B;
    failure.walkFrom = WALK_FROM;
    failure.walkTo = WALK_TO;
    failure.goal = GOAL;
    failure.spot = SPOT;
    failure.airLink = airLink;
    return failure;
}

enum class Expect
{
    None,
    Goal,  // a place at the goal
    Spot,  // a place at the bot's spot, refusing a start
    Link,
    Walk,
    Fly,
    Hearth,
    Wrong  // an entry of the right kind with the wrong key
};

Expect Classify(std::optional<Key> const& key)
{
    if (!key)
        return Expect::None;
    switch (key->kind)
    {
        case Kind::Place:
            if (key->spot)
                return Same(key->from, SPOT) ? Expect::Spot : Expect::Wrong;
            return Same(key->from, GOAL) ? Expect::Goal : Expect::Wrong;
        case Kind::Link:
            return key->fromNode == NODE_A && key->toNode == NODE_B ? Expect::Link : Expect::Wrong;
        case Kind::Walk:
            return Same(key->from, WALK_FROM) && Same(key->to, WALK_TO) ? Expect::Walk : Expect::Wrong;
        case Kind::Way:
            return key->way == Way::Flying ? Expect::Fly : Expect::Hearth;
    }
    return Expect::Wrong;
}

struct Case
{
    std::string_view word;
    Leg leg;
    bool airLink;
    Expect expect;
};

// INTERFACE 5's table, per word and the leg it ends
constexpr Case CASES[] = {
    {"no_path", Leg::GraphWalk, false, Expect::Link},
    {"no_path", Leg::LocalPiece, false, Expect::Goal},
    {"no_path", Leg::OffGraphWalk, false, Expect::Walk},
    {"no_walk", Leg::OffGraphWalk, false, Expect::Walk},
    {"no_progress", Leg::GraphWalk, false, Expect::Link},
    {"no_progress", Leg::OffGraphWalk, false, Expect::Walk},
    {"no_progress", Leg::LocalPiece, false, Expect::Goal},
    {"stuck", Leg::GraphWalk, false, Expect::Link},
    {"stuck", Leg::Fly, false, Expect::Fly},
    {"abort:stalled", Leg::OffGraphWalk, false, Expect::Walk},
    {"door_or_elevator", Leg::GraphWalk, false, Expect::Link},
    {"door_or_elevator", Leg::LocalPiece, false, Expect::Goal},
    {"unreachable_poi", Leg::LocalPiece, false, Expect::Goal},
    {"no_route", Leg::None, false, Expect::Goal},
    {"abort:no_route", Leg::Fly, false, Expect::Goal},
    {"off_mesh", Leg::LocalPiece, false, Expect::Spot},
    {"off_mesh", Leg::Fly, false, Expect::Spot},
    {"death", Leg::GraphWalk, false, Expect::Link},
    {"death", Leg::OffGraphWalk, false, Expect::Walk},
    {"death", Leg::Ride, false, Expect::None},
    {"death", Leg::LocalPiece, false, Expect::None},
    {"death", Leg::Fly, false, Expect::Fly},
    {"trip_deaths", Leg::GraphWalk, false, Expect::Goal},
    {"trip_deaths", Leg::OffGraphWalk, false, Expect::Goal},
    {"trip_deaths", Leg::Ride, false, Expect::Goal},
    {"trip_deaths", Leg::LocalPiece, false, Expect::Goal},
    {"trip_deaths", Leg::Fly, false, Expect::Goal},
    {"trip_deaths", Leg::None, false, Expect::Goal},
    {"timeout", Leg::Ride, false, Expect::Link},
    {"timeout", Leg::GraphWalk, false, Expect::None},
    {"timeout", Leg::Taxi, false, Expect::None},
    {"timeout", Leg::Hearth, false, Expect::None},
    {"timeout", Leg::Fly, false, Expect::Fly},
    {"transport_lost", Leg::Ride, false, Expect::Link},
    {"no_exit", Leg::Ride, false, Expect::Link},
    {"no_crossing", Leg::Ride, false, Expect::Link},
    {"crossing_failed", Leg::Ride, false, Expect::Link},
    {"portal", Leg::Portal, false, Expect::Link},
    {"taxi", Leg::Taxi, false, Expect::Link},
    {"fare", Leg::Taxi, false, Expect::Link},
    {"landing", Leg::Taxi, false, Expect::Link},
    {"landing", Leg::Fly, false, Expect::Fly},
    {"takeoff", Leg::Fly, false, Expect::Spot},
    {"takeoff", Leg::Fly, true, Expect::Link},
    {"hearth", Leg::Hearth, false, Expect::Hearth},
    {"abort:not_bound", Leg::None, false, Expect::Goal},
    {"inn_failed", Leg::None, false, Expect::Goal},
    {"inn_no_walk", Leg::OffGraphWalk, false, Expect::Goal},  // an inn trip's no_walk end: the inn's place (NE3)
    {"inn_no_walk", Leg::GraphWalk, false, Expect::Goal},
    {"gather_no_walk", Leg::LocalPiece, false, Expect::Goal},  // a gather walk's no_walk end: the stop (WPC9)
};

// The trip's own ends (TravelLegs' "abort:" sources that are no failed leg) and the inn trip's event
constexpr std::string_view TRIP_ENDS[] = {"abort:deadline", "abort:stalled",   "abort:no_service",
                                          "abort:stopped",  "abort:sick",      "abort:held",
                                          "abort:order",    "abort:not_bound", "inn_failed"};
constexpr std::string_view STUCK_BOOK_WORDS[] = {"no_path", "no_progress", "stuck", "stalled", "door_or_elevator"};

bool InGoToCauses(std::string_view word)
{
    for (GoTo::CauseRow const& row : GoTo::CAUSES)
        if (row.word == word)
            return true;
    return false;
}

template <typename R>
void Expiry(Counts& counts)
{
    counts.Check(R::LiveMs(BASE, 0) == 10 * MINUTE && R::LiveMs(BASE, 1) == 20 * MINUTE &&
                     R::LiveMs(BASE, 2) == 40 * MINUTE && R::LiveMs(BASE, 3) == 80 * MINUTE,
                 "expiry: 10, 20, 40, 80 min");
    counts.Check(R::LiveMs(BASE, 4) == 80 * MINUTE && R::LiveMs(BASE, 31) == 80 * MINUTE, "expiry: capped at 80 min");

    // Each failure within its repeat window: the repeats rise to 3 and stay there
    Record record;
    SightPoint const place = At(10.0f, 10.0f);
    std::uint32_t now = 5000;
    bool chain = FailedAttempts::Note<R>(record, PlaceKey(place), "no_route", now, BASE) == Noted::Added;
    std::uint32_t const gaps[] = {15 * MINUTE, 30 * MINUTE, 60 * MINUTE, 120 * MINUTE};
    for (std::uint32_t gap : gaps)
    {
        now += gap;
        chain = chain && FailedAttempts::Note<R>(record, PlaceKey(place), "no_route", now, BASE) == Noted::Repeated;
    }
    counts.Check(chain, "expiry: a failure within as long again after the expiry is a repeat");
    counts.Check(record.count == 1 && record.entries[0].repeats == 3 && record.entries[0].liveMs == 80 * MINUTE,
                 "expiry: at most 3 repeats, 80 min");
    counts.Check(!FailedAttempts::IsPlaceRefused<R>(record, place, now + 80 * MINUTE - 1).empty() &&
                     FailedAttempts::IsPlaceRefused<R>(record, place, now + 80 * MINUTE).empty(),
                 "expiry: refused until 80 min after the last failure, not after");

    // The second step: 20 min after a repeat
    Record twice;
    FailedAttempts::Note<R>(twice, PlaceKey(place), "no_route", 0, BASE);
    FailedAttempts::Note<R>(twice, PlaceKey(place), "no_route", 12 * MINUTE, BASE);
    counts.Check(!FailedAttempts::IsPlaceRefused<R>(twice, place, 31 * MINUTE).empty() &&
                     FailedAttempts::IsPlaceRefused<R>(twice, place, 32 * MINUTE).empty(),
                 "expiry: 20 min after the first repeat");
}

template <typename R>
void Repeats(Counts& counts)
{
    SightPoint const place = At(10.0f, 10.0f);
    Record record;
    FailedAttempts::Note<R>(record, PlaceKey(place), "no_route", 0, BASE);
    counts.Check(!FailedAttempts::IsPlaceRefused<R>(record, place, 10 * MINUTE - 1).empty() &&
                     FailedAttempts::IsPlaceRefused<R>(record, place, 10 * MINUTE).empty(),
                 "repeat: a first failure is refused for 10 min");

    // A live entry is never tried: a second word for it leaves it as it was
    counts.Check(FailedAttempts::Note<R>(record, PlaceKey(place), "no_route", 5 * MINUTE, BASE) == Noted::AlreadyLive &&
                     record.entries[0].lastMs == 0 && record.entries[0].repeats == 0,
                 "repeat: a live entry stays as it was");
    counts.Check(FailedAttempts::IsPlaceRefused<R>(record, place, 10 * MINUTE).empty(),
                 "repeat: a live entry's failure does not extend it");

    Record late = record;
    counts.Check(
        FailedAttempts::Note<R>(record, PlaceKey(place), "no_route", 20 * MINUTE - 1, BASE) == Noted::Repeated &&
            record.entries[0].repeats == 1,
        "repeat: within as long again after the expiry");
    counts.Check(FailedAttempts::Note<R>(late, PlaceKey(place), "no_route", 20 * MINUTE, BASE) == Noted::Added &&
                     late.count == 1 && late.entries[0].repeats == 0 && late.entries[0].liveMs == BASE,
                 "repeat: later, the entry starts afresh");

    // Wrap-safe times: getMSTime wraps after 49 days
    Record wrap;
    std::uint32_t const last = UINT32_MAX - 1000;
    FailedAttempts::Note<R>(wrap, PlaceKey(place), "no_route", last, BASE);
    counts.Check(!FailedAttempts::IsPlaceRefused<R>(wrap, place, last + 500).empty() &&
                     !FailedAttempts::IsPlaceRefused<R>(wrap, place, last + 5 * MINUTE).empty() &&
                     FailedAttempts::IsPlaceRefused<R>(wrap, place, last + 10 * MINUTE).empty(),
                 "repeat: times are wrap-safe");
    counts.Check(
        FailedAttempts::Note<R>(wrap, PlaceKey(place), "no_route", last + 15 * MINUTE, BASE) == Noted::Repeated,
        "repeat: a repeat across the wrap");
}

template <typename R>
void Spots(Counts& counts)
{
    Record record;
    std::optional<Noted> const off = FailedAttempts::NoteFailure<R>(record, "off_mesh", On(Leg::LocalPiece), 0, BASE);
    counts.Check(off == Noted::Added, "spot: off_mesh writes an entry");
    counts.Check(FailedAttempts::RefusesStartAt<R>(record, SPOT, MINUTE) &&
                     FailedAttempts::RefusesStartAt<R>(record, At(SPOT.X + 9.0f, SPOT.Y), MINUTE) &&
                     !FailedAttempts::RefusesStartAt<R>(record, At(SPOT.X + 11.0f, SPOT.Y), MINUTE),
                 "spot: no start within 10 yd of an off_mesh spot");
    counts.Check(FailedAttempts::IsPlaceRefused<R>(record, At(SPOT.X, SPOT.Y + 5.0f), MINUTE) == "off_mesh",
                 "spot: no goal near an off_mesh spot, with its cause");
    counts.Check(!FailedAttempts::RefusesStartAt<R>(record, SPOT, 10 * MINUTE), "spot: the start refusal expires");

    Record takeoff;
    FailedAttempts::NoteFailure<R>(takeoff, "takeoff", On(Leg::Fly), 0, BASE);
    counts.Check(FailedAttempts::RefusesStartAt<R>(takeoff, SPOT, MINUTE) &&
                     FailedAttempts::IsPlaceRefused<R>(takeoff, SPOT, MINUTE) == "takeoff",
                 "spot: a refused takeoff's spot refuses a start and a goal");

    Record goal;
    FailedAttempts::NoteFailure<R>(goal, "abort:no_route", On(Leg::None), 0, BASE);
    counts.Check(FailedAttempts::IsPlaceRefused<R>(goal, GOAL, MINUTE) == "no_route" &&
                     !FailedAttempts::RefusesStartAt<R>(goal, GOAL, MINUTE),
                 "spot: a goal's entry refuses the goal, not a start there");
}

template <typename R>
void Capacity(Counts& counts)
{
    Record record;
    for (std::uint32_t i = 0; i < 16; ++i)
        FailedAttempts::Note<R>(record, PlaceKey(At(100.0f * i, 0.0f)), "no_route", i * 1000, BASE);
    counts.Check(record.count == 16, "capacity: 16 entries");
    FailedAttempts::Note<R>(record, PlaceKey(At(5000.0f, 0.0f)), "no_route", 20000, BASE);
    bool rest = true;
    for (std::uint32_t i = 1; i < 16; ++i)
        rest = rest && !FailedAttempts::IsPlaceRefused<R>(record, At(100.0f * i, 0.0f), 20000).empty();
    counts.Check(record.count == 16 && FailedAttempts::IsPlaceRefused<R>(record, At(0.0f, 0.0f), 20000).empty() &&
                     !FailedAttempts::IsPlaceRefused<R>(record, At(5000.0f, 0.0f), 20000).empty() && rest,
                 "capacity: the oldest goes first");

    // The oldest by its last failure, wherever it stands
    std::uint32_t const now = 100 * MINUTE;
    Record shuffled;
    shuffled.count = 16;
    for (std::uint32_t i = 0; i < 16; ++i)
        shuffled.entries[i] = Entry{PlaceKey(At(100.0f * i, 0.0f)), "no_route", now - i % 4 * MINUTE, BASE, 0};
    shuffled.entries[5].lastMs = now - 6 * MINUTE;
    FailedAttempts::Note<R>(shuffled, PlaceKey(At(5000.0f, 0.0f)), "no_route", now, BASE);
    counts.Check(Same(shuffled.entries[5].key.from, At(5000.0f, 0.0f)), "capacity: the oldest last failure goes");

    // An entry past its repeat window goes before a live one that is older
    Record gone;
    gone.count = 16;
    for (std::uint32_t i = 0; i < 16; ++i)
        gone.entries[i] = Entry{PlaceKey(At(100.0f * i, 0.0f)), "no_route", now - MINUTE, BASE, 0};
    gone.entries[3] = Entry{PlaceKey(At(300.0f, 0.0f)), "no_route", now - 50 * MINUTE, 80 * MINUTE, 3};
    gone.entries[7] = Entry{PlaceKey(At(700.0f, 0.0f)), "no_route", now - 25 * MINUTE, BASE, 0};
    FailedAttempts::Note<R>(gone, PlaceKey(At(5000.0f, 0.0f)), "no_route", now, BASE);
    counts.Check(Same(gone.entries[7].key.from, At(5000.0f, 0.0f)) && Same(gone.entries[3].key.from, At(300.0f, 0.0f)),
                 "capacity: a gone entry's slot first");
}

template <typename R>
void Matching(Counts& counts)
{
    Key const place = PlaceKey(At(0.0f, 0.0f));
    counts.Check(R::SameKey(place, PlaceKey(At(9.0f, 0.0f))) && R::SameKey(place, PlaceKey(At(0.0f, 0.0f))),
                 "match: a place within 10 yd");
    counts.Check(!R::SameKey(place, PlaceKey(At(11.0f, 0.0f))) && !R::SameKey(place, PlaceKey(At(15.0f, 0.0f))),
                 "match: a place beyond 10 yd is another");
    counts.Check(!R::SameKey(place, PlaceKey(At(0.0f, 0.0f, 1))), "match: a place on another map is another");

    counts.Check(R::SameKey(LinkKey(3, 4), LinkKey(3, 4)) && R::SameKey(LinkKey(3, 4), LinkKey(4, 3)),
                 "match: a link either way");
    counts.Check(!R::SameKey(LinkKey(3, 4), LinkKey(3, 5)) && !R::SameKey(LinkKey(3, 4), LinkKey(5, 4)),
                 "match: another link");

    Key const walk = WalkKey(At(0.0f, 0.0f), At(200.0f, 0.0f));
    counts.Check(R::SameKey(walk, WalkKey(At(5.0f, 0.0f), At(205.0f, 0.0f))), "match: a walk with both ends near");
    counts.Check(!R::SameKey(walk, WalkKey(At(0.0f, 0.0f), At(300.0f, 0.0f))) &&
                     !R::SameKey(walk, WalkKey(At(100.0f, 0.0f), At(200.0f, 0.0f))),
                 "match: a walk with one end away is another");

    counts.Check(R::SameKey(WayKey(Way::Flying), WayKey(Way::Flying)) &&
                     !R::SameKey(WayKey(Way::Flying), WayKey(Way::Hearthstone)),
                 "match: a way by itself");
    counts.Check(
        !R::SameKey(place, WalkKey(At(0.0f, 0.0f), At(0.0f, 0.0f))) && !R::SameKey(LinkKey(0, 0), WayKey(Way::Flying)),
        "match: another kind is another key");

    Record record;
    FailedAttempts::Note<R>(record, LinkKey(3, 4), "no_path", 0, BASE);
    FailedAttempts::Note<R>(record, walk, "no_walk", 0, BASE);
    FailedAttempts::Note<R>(record, WayKey(Way::Hearthstone), "hearth", 0, BASE);
    counts.Check(FailedAttempts::IsRefused<R>(record, LinkKey(4, 3), MINUTE) &&
                     FailedAttempts::IsRefused<R>(record, WalkKey(At(5.0f, 0.0f), At(205.0f, 0.0f)), MINUTE) &&
                     FailedAttempts::IsRefused<R>(record, WayKey(Way::Hearthstone), MINUTE) &&
                     !FailedAttempts::IsRefused<R>(record, WayKey(Way::Flying), MINUTE) &&
                     FailedAttempts::IsPlaceRefused<R>(record, At(0.0f, 0.0f), MINUTE).empty(),
                 "match: the reader asks per kind");
}

template <typename R>
void Table(Counts& counts)
{
    bool unique = true;
    for (std::size_t i = 0; i < std::size(FailedAttempts::CAUSES); ++i)
        for (std::size_t j = i + 1; j < std::size(FailedAttempts::CAUSES); ++j)
            unique = unique && FailedAttempts::CAUSES[i].word != FailedAttempts::CAUSES[j].word;
    counts.Check(unique, "table: one row per word");

    bool legWords = true;
    for (int reason = 0; reason <= static_cast<int>(TravelLegs::FailReason::CrossingFailed); ++reason)
    {
        auto const fail = static_cast<TravelLegs::FailReason>(reason);
        legWords = legWords && FailedAttempts::FindCause(TravelLegs::FailWord(fail)) &&
                   FailedAttempts::FindCause(TravelLegs::AbortSource(fail));
        if (!FailedAttempts::FindCause(TravelLegs::FailWord(fail)) && counts.loud)
            std::printf("  no row: %s\n", TravelLegs::FailWord(fail));
    }
    counts.Check(legWords, "table: a row for every FailWord and its abort source");

    bool stuckWords = true;
    for (int cause = 0; cause <= static_cast<int>(WorldTravel::StuckCause::Other); ++cause)
        stuckWords = stuckWords && FailedAttempts::FindCause(
                                       WorldTravel::StuckCauseWord(static_cast<WorldTravel::StuckCause>(cause)));
    counts.Check(stuckWords, "table: a row for every StuckCauseWord");

    bool tripWords = true;
    for (std::string_view word : TRIP_ENDS)
        tripWords = tripWords && FailedAttempts::FindCause(word);
    counts.Check(tripWords, "table: a row for every trip end");

    // In step with GoTo's table: every GoTo word has a row, every row is a GoTo word but the three GoTo never sees
    bool goTo = true;
    for (GoTo::CauseRow const& row : GoTo::CAUSES)
        goTo = goTo && FailedAttempts::FindCause(row.word);
    for (CauseRow const& row : FailedAttempts::CAUSES)
        goTo = goTo &&
               (InGoToCauses(row.word) || row.word == "none" || row.word == "inn_failed" || row.word == "inn_no_walk");
    counts.Check(goTo, "table: in step with GoTo::CAUSES");

    // An end from outside, a service or the bot's own mishap writes nothing
    bool outside = true;
    for (CauseRow const& row : FailedAttempts::CAUSES)
    {
        StatusEnd::Outcome const outcome = GoTo::OutcomeOf(row.word.data());
        bool const blames = outcome == StatusEnd::Outcome::Abandoned || outcome == StatusEnd::Outcome::Stuck ||
                            row.word == "timeout" || row.word == "inn_failed";
        outside = outside && (blames || row.target == Target::None);
    }
    counts.Check(outside, "table: an interrupted, ordered, sick or finished end writes nothing");
    counts.Check(!FailedAttempts::FindCause("bogus") && !FailedAttempts::FindCause("abort:bogus"),
                 "table: an unknown word has no row");

    bool cases = true;
    for (Case const& c : CASES)
    {
        CauseRow const* row = FailedAttempts::FindCause(c.word);
        bool const pass = row && Classify(R::KeyFor(row->target, On(c.leg, c.airLink))) == c.expect;
        if (!pass && counts.loud)
            std::printf("  entry: %.*s on leg %d\n", static_cast<int>(c.word.size()), c.word.data(),
                        static_cast<int>(c.leg));
        cases = cases && pass;
    }
    counts.Check(cases, "table: each cause writes INTERFACE 5's entry");

    // Every row that writes an entry is covered above; a none row writes nothing on any leg
    bool covered = true;
    Leg const legs[] = {Leg::None,   Leg::GraphWalk, Leg::OffGraphWalk, Leg::LocalPiece, Leg::Ride,
                        Leg::Portal, Leg::Taxi,      Leg::Fly,          Leg::Hearth};
    for (CauseRow const& row : FailedAttempts::CAUSES)
    {
        if (row.target == Target::None)
        {
            for (Leg leg : legs)
                covered = covered && !R::KeyFor(row.target, On(leg, true));
            continue;
        }
        bool found = false;
        for (Case const& c : CASES)
            found = found || FailedAttempts::FindCause(c.word) == &row;
        covered = covered && found;
    }
    counts.Check(covered, "table: every writing row has a case, a none row writes nothing");

    Record record;
    counts.Check(!FailedAttempts::NoteFailure<R>(record, "bogus", On(Leg::GraphWalk), 0, BASE) &&
                     !FailedAttempts::NoteFailure<R>(record, "held", On(Leg::GraphWalk), 0, BASE) && record.count == 0,
                 "table: an unknown or outside word writes nothing");
    counts.Check(FailedAttempts::NoteFailure<R>(record, "abort:stalled", On(Leg::GraphWalk), 0, BASE) == Noted::Added &&
                     record.entries[0].cause == "stalled",
                 "table: the entry keeps its bare cause word");
}

template <typename R>
void StuckBook(Counts& counts)
{
    bool rows = true;
    for (CauseRow const& row : FailedAttempts::CAUSES)
    {
        bool listed = false;
        for (std::string_view word : STUCK_BOOK_WORDS)
            listed = listed || word == row.word;
        rows = rows && row.stuckBook == listed;
    }
    counts.Check(rows, "stuck book: only no_path, no_progress (stuck, stalled) and door_or_elevator");

    bool graph = true;
    for (std::string_view word : STUCK_BOOK_WORDS)
        graph = graph && FailedAttempts::ReachesStuckBook<R>(word, On(Leg::GraphWalk));
    counts.Check(graph && FailedAttempts::ReachesStuckBook<R>("abort:stalled", On(Leg::GraphWalk)),
                 "stuck book: a graph link that did not carry a bot");

    Failure offMesh = On(Leg::GraphWalk);
    offMesh.onMesh = false;
    counts.Check(!FailedAttempts::ReachesStuckBook<R>("no_path", offMesh), "stuck book: not of a bot off the mesh");
    counts.Check(!FailedAttempts::ReachesStuckBook<R>("no_path", On(Leg::LocalPiece)) &&
                     !FailedAttempts::ReachesStuckBook<R>("no_progress", On(Leg::OffGraphWalk)) &&
                     !FailedAttempts::ReachesStuckBook<R>("stuck", On(Leg::Ride)) &&
                     !FailedAttempts::ReachesStuckBook<R>("stuck", On(Leg::Fly)),
                 "stuck book: only on a graph link");
    counts.Check(!FailedAttempts::ReachesStuckBook<R>("no_walk", On(Leg::GraphWalk)) &&
                     !FailedAttempts::ReachesStuckBook<R>("death", On(Leg::GraphWalk)) &&
                     !FailedAttempts::ReachesStuckBook<R>("bogus", On(Leg::GraphWalk)),
                 "stuck book: no other cause");
}

template <typename R>
void ResetAndCopy(Counts& counts)
{
    Record record;
    FailedAttempts::NoteFailure<R>(record, "off_mesh", On(Leg::LocalPiece), 0, BASE);
    FailedAttempts::NoteFailure<R>(record, "no_path", On(Leg::GraphWalk), 15 * MINUTE, BASE);
    FailedAttempts::NoteFailure<R>(record, "hearth", On(Leg::Hearth), 15 * MINUTE, BASE);

    std::vector<Entry> const live = FailedAttempts::LiveEntries<R>(record, 16 * MINUTE);
    counts.Check(live.size() == 2 && live[0].cause == "no_path" && live[1].cause == "hearth",
                 "copy: the live entries only");

    FailedAttempts::Reset<R>(record);
    counts.Check(record.count == 0 && !FailedAttempts::RefusesStartAt<R>(record, SPOT, MINUTE) &&
                     !FailedAttempts::IsRefused<R>(record, WayKey(Way::Hearthstone), 16 * MINUTE),
                 "reset: nothing is refused");
    counts.Check(live.size() == 2, "copy: a plain copy outlives the reset");
    counts.Check(FailedAttempts::NoteFailure<R>(record, "hearth", On(Leg::Hearth), 17 * MINUTE, BASE) == Noted::Added &&
                     record.entries[0].repeats == 0,
                 "reset: a failure after it starts afresh");
}

// The pickers' reading (PA32): a bot told "no route" or "no walk" is not offered the same place at its next pick. The
// pick reads the one record and nothing else (Offered and KeepOffered take the Record, the place and the bot's spot:
// no second memory, no other state argument). Their candidates are places; KeepOffered keeps their order
template <typename R>
std::vector<SightPoint> Offer(Record const& record, std::vector<SightPoint> candidates, SightPoint const& here,
                              std::uint32_t nowMs)
{
    FailedAttempts::KeepOffered<R>(record, candidates, here, nowMs,
                                   [](SightPoint const& place) { return std::optional<SightPoint>(place); });
    return candidates;
}

template <typename R>
void Picks(Counts& counts)
{
    auto const offer = Offer<R>;
    SightPoint const other = At(-1000.0f, 0.0f);  // a place nothing refuses
    SightPoint const away = At(0.0f, -1000.0f);   // where the bot stands, far from every entry

    // 1. A route none at the first ask: no leg yet, so FailureOf gives leg None with the trip's goal; LegFailed's
    // word writes a live place entry of the goal, EndTrip's abort source finds it live. Every pick refuses the goal
    // and a place within 10 yd of it, not one 11 yd away
    TravelLegs::Trip const asked;
    Failure const none = TravelLegs::FailureOf(asked, TravelLegs::Point{GOAL.MapId, GOAL.X, GOAL.Y, 0.0f},
                                               TravelLegs::Point{SPOT.MapId, SPOT.X, SPOT.Y, 0.0f});
    Record route;
    counts.Check(
        none.leg == Leg::None &&
            FailedAttempts::NoteFailure<R>(route, TravelLegs::FailWord(TravelLegs::FailReason::NoRoute), none, 0,
                                           BASE) == Noted::Added &&
            FailedAttempts::NoteFailure<R>(route, "abort:no_route", none, MINUTE / 60, BASE) == Noted::AlreadyLive &&
            route.count == 1 && route.entries[0].key.kind == Kind::Place && !route.entries[0].key.spot &&
            Same(route.entries[0].key.from, GOAL) && route.entries[0].cause == "no_route",
        "pick: a route none at the first ask writes the goal's place entry, once");
    counts.Check(!FailedAttempts::Offered<R>(route, GOAL, away, 3000) &&
                     !FailedAttempts::Offered<R>(route, At(GOAL.X + 9.0f, GOAL.Y), away, 3000),
                 "pick: a goal whose route was none is not offered, nor a place within 10 yd of it");
    counts.Check(FailedAttempts::Offered<R>(route, At(GOAL.X + 11.0f, GOAL.Y), away, 3000) &&
                     FailedAttempts::Offered<R>(route, other, away, 3000),
                 "pick: a place 11 yd from it is offered");

    // 2. A refused approach (GoTo's local piece, no_path) writes the same place entry, and the same pick refuses it
    Record approach;
    FailedAttempts::NoteFailure<R>(approach, "no_path", On(Leg::LocalPiece), 0, BASE);
    counts.Check(approach.count == 1 && approach.entries[0].key.kind == Kind::Place &&
                     Same(approach.entries[0].key.from, GOAL) &&
                     !FailedAttempts::Offered<R>(approach, GOAL, away, 3000),
                 "pick: a refused approach's place is not offered");

    // 3. A bot whose only goal is refused picks another; with nothing else the pick has none, never the refused goal
    std::vector<SightPoint> const two = offer(route, {GOAL, other}, away, 3000);
    counts.Check(two.size() == 1 && Same(two[0], other), "pick: of a refused goal and another, the other");
    counts.Check(offer(route, {GOAL}, away, 3000).empty(), "pick: a refused goal alone leaves none");
    std::vector<SightPoint> const order = offer(route, {other, GOAL, At(2000.0f, 0.0f)}, away, 3000);
    counts.Check(order.size() == 2 && Same(order[0], other) && Same(order[1], At(2000.0f, 0.0f)),
                 "pick: the offered keep their order");
    counts.Check(offer(Record{}, {GOAL, other}, away, 3000).size() == 2, "pick: an empty record offers all");

    // 4. Expiry: offered again after it (10 min; 20 s on shortavoid's base), refused again as a repeat, twice as long
    counts.Check(!FailedAttempts::Offered<R>(route, GOAL, away, 10 * MINUTE - 1) &&
                     FailedAttempts::Offered<R>(route, GOAL, away, 10 * MINUTE),
                 "pick: offered again at the expiry");
    Record repeat = route;
    counts.Check(FailedAttempts::NoteFailure<R>(repeat, "no_route", none, 11 * MINUTE, BASE) == Noted::Repeated &&
                     !FailedAttempts::Offered<R>(repeat, GOAL, away, 30 * MINUTE),
                 "pick: a repeat refuses for twice as long");
    counts.Check(FailedAttempts::Offered<R>(repeat, GOAL, away, 31 * MINUTE),
                 "pick: offered after the repeat's expiry");
    std::uint32_t const shortBase = 20 * 1000;
    Record shortAvoid;
    FailedAttempts::NoteFailure<R>(shortAvoid, "no_route", none, 0, shortBase);
    counts.Check(!FailedAttempts::Offered<R>(shortAvoid, GOAL, away, shortBase - 1) &&
                     FailedAttempts::Offered<R>(shortAvoid, GOAL, away, shortBase),
                 "pick: shortavoid's base, offered again after 20 s");

    // 5. Eviction (the ring as it is, evidence for its size): a bot refused a new goal every 3 s holds 16 entries;
    // the 17th and 18th push out the two oldest, so after 51 s the first goals are offered again, long before their
    // 10 min. The record does not hold more than 16 refused goals at once
    Record ring;
    for (std::uint32_t i = 0; i < 18; ++i)
        FailedAttempts::Note<R>(ring, PlaceKey(At(100.0f * i, 50.0f)), "no_route", i * 3000, BASE);
    bool held = true;
    for (std::uint32_t i = 2; i < 18; ++i)
        held = held && !FailedAttempts::Offered<R>(ring, At(100.0f * i, 50.0f), away, 54000);
    counts.Check(held && FailedAttempts::Offered<R>(ring, At(0.0f, 50.0f), away, 54000) &&
                     FailedAttempts::Offered<R>(ring, At(100.0f, 50.0f), away, 54000),
                 "pick: past 16 refused goals the oldest are offered again");

    // 6. A live spot entry where the bot stands (off_mesh, takeoff) offers no candidate, whatever the goal; 11 yd from
    // it the pick is as usual; a goal's entry where the bot stands refuses no start
    Record spot;
    FailedAttempts::NoteFailure<R>(spot, "off_mesh", On(Leg::LocalPiece), 0, BASE);
    counts.Check(offer(spot, {GOAL, other}, SPOT, MINUTE).empty() &&
                     offer(spot, {GOAL, other}, At(SPOT.X + 9.0f, SPOT.Y), MINUTE).empty(),
                 "pick: none at a refused spot");
    counts.Check(offer(spot, {GOAL, other}, At(SPOT.X + 11.0f, SPOT.Y), MINUTE).size() == 2 &&
                     FailedAttempts::Offered<R>(spot, GOAL, At(SPOT.X + 11.0f, SPOT.Y), MINUTE),
                 "pick: a refused spot 11 yd away refuses nothing");
    counts.Check(!FailedAttempts::Offered<R>(spot, GOAL, SPOT, MINUTE),
                 "pick: one place at a refused spot is not offered");
    counts.Check(offer(route, {other}, GOAL, 3000).size() == 1 && FailedAttempts::Offered<R>(route, other, GOAL, 3000),
                 "pick: a goal's entry where the bot stands refuses no start");
    std::vector<int> unknown = {1, 2};
    FailedAttempts::KeepOffered<R>(route, unknown, away, 3000, [](int) { return std::optional<SightPoint>(); });
    std::vector<int> unknownAtSpot = {1, 2};
    FailedAttempts::KeepOffered<R>(spot, unknownAtSpot, SPOT, MINUTE, [](int) { return std::optional<SightPoint>(); });
    counts.Check(unknown.size() == 2 && unknownAtSpot.empty(),
                 "pick: a candidate without a known place is kept, unless no start is offered");

    // 7. A quiet approach marks nothing (GoTo::MarksFailure, proven by the go-to selftest): its place stays offered
    Record quiet;
    if (GoTo::MarksFailure(true))
        FailedAttempts::NoteFailure<R>(quiet, "no_path", On(Leg::LocalPiece), 0, BASE);
    counts.Check(quiet.count == 0 && FailedAttempts::Offered<R>(quiet, GOAL, away, MINUTE),
                 "pick: a quiet approach's place stays offered");

    // 8. A cause without a target (interrupted, sick, held, ...) refuses no pick
    bool noneOffered = true;
    for (CauseRow const& row : FailedAttempts::CAUSES)
    {
        if (row.target != Target::None)
            continue;
        Record ended;
        FailedAttempts::NoteFailure<R>(ended, row.word, On(Leg::None), 0, BASE);
        FailedAttempts::NoteFailure<R>(ended, row.word, On(Leg::LocalPiece), 0, BASE);
        noneOffered = noneOffered && FailedAttempts::Offered<R>(ended, GOAL, SPOT, MINUTE) &&
                      offer(ended, {GOAL, SPOT}, SPOT, MINUTE).size() == 2;
    }
    counts.Check(noneOffered, "pick: a cause without a target refuses no pick");

    // 9. PA56: a walk off the graph refused (no_walk) where it began at the bot's own spot (its first piece) refuses
    // every start within 10 yd of that spot until it expires, as a spot entry does, and GoTo's step 2 reads its cause;
    // the walk itself stays a walk entry the route request leaves out. A refused walk that began elsewhere (its last
    // piece, from a node), a death on the first piece and a stuck walk refuse no start
    Failure start = On(Leg::OffGraphWalk);
    start.walkFrom = SPOT;
    start.startSide = true;
    Record walkStart;
    FailedAttempts::NoteFailure<R>(walkStart, "no_walk", start, 0, BASE);
    counts.Check(walkStart.count == 1 && walkStart.entries[0].key.kind == Kind::Walk &&
                     Same(walkStart.entries[0].key.from, SPOT) && Same(walkStart.entries[0].key.to, WALK_TO),
                 "pick: a walk refused at the start is one walk entry");
    counts.Check(offer(walkStart, {GOAL, other}, SPOT, MINUTE).empty() &&
                     offer(walkStart, {GOAL, other}, At(SPOT.X + 9.0f, SPOT.Y), MINUTE).empty() &&
                     FailedAttempts::StartRefusedBy<R>(walkStart, At(SPOT.X, SPOT.Y + 9.0f), MINUTE) == "no_walk",
                 "pick: none where a walk was refused at its start, and its cause refuses GoTo's start");
    counts.Check(offer(walkStart, {GOAL, other}, At(SPOT.X + 11.0f, SPOT.Y), MINUTE).size() == 2 &&
                     FailedAttempts::Offered<R>(walkStart, SPOT, away, MINUTE),
                 "pick: 11 yd from it all are offered, and the spot itself is no refused goal");
    counts.Check(offer(walkStart, {GOAL, other}, SPOT, 10 * MINUTE).size() == 2,
                 "pick: the walk's start refusal expires with its entry");
    Failure later = start;
    later.startSide = false;
    Record lastPiece;
    FailedAttempts::NoteFailure<R>(lastPiece, "no_walk", later, 0, BASE);
    Record died;
    FailedAttempts::NoteFailure<R>(died, "death", start, 0, BASE);
    Record stuck;
    FailedAttempts::NoteFailure<R>(stuck, "stuck", start, 0, BASE);
    counts.Check(lastPiece.count == 1 && died.count == 1 && stuck.count == 1 &&
                     offer(lastPiece, {GOAL, other}, SPOT, MINUTE).size() == 2 &&
                     offer(died, {GOAL, other}, SPOT, MINUTE).size() == 2 &&
                     offer(stuck, {GOAL, other}, SPOT, MINUTE).size() == 2,
                 "pick: a walk refused elsewhere, a death or a stuck walk at the start refuse no start");
    counts.Check(FailedAttempts::StartRefusedBy<R>(spot, SPOT, MINUTE) == "off_mesh" &&
                     FailedAttempts::StartRefusedBy<R>(route, GOAL, 3000).empty(),
                 "pick: a spot's start refusal names its cause; a goal's entry names none");
}

template <typename R>
void Suite(Counts& counts)
{
    Expiry<R>(counts);
    Repeats<R>(counts);
    Spots<R>(counts);
    Capacity<R>(counts);
    Matching<R>(counts);
    Table<R>(counts);
    StuckBook<R>(counts);
    ResetAndCopy<R>(counts);
    Picks<R>(counts);
}

// The mutants: each breaks one rule
struct UncappedExpiry : Rules
{
    static std::uint32_t LiveMs(std::uint32_t baseMs, std::uint32_t repeats)
    {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(baseMs) << std::min(repeats, 8u));
    }
};

struct UncappedRepeats : Rules
{
    static std::uint32_t NextRepeats(Phase phase, std::uint32_t repeats)
    {
        return phase == Phase::Repeat ? repeats + 1 : 0;
    }
};

struct NoRepeatWindow : Rules
{
    static Phase PhaseOf(Entry const& entry, std::uint32_t nowMs)
    {
        return nowMs - entry.lastMs < entry.liveMs ? Phase::Live : Phase::Gone;
    }
};

struct EndlessRepeatWindow : Rules
{
    static Phase PhaseOf(Entry const& entry, std::uint32_t nowMs)
    {
        return nowMs - entry.lastMs < entry.liveMs ? Phase::Live : Phase::Repeat;
    }
};

struct WrappingTimes : Rules
{
    static Phase PhaseOf(Entry const& entry, std::uint32_t nowMs)
    {
        if (nowMs < entry.lastMs + entry.liveMs)
            return Phase::Live;
        return nowMs < entry.lastMs + 2 * entry.liveMs ? Phase::Repeat : Phase::Gone;
    }
};

struct LiveRewritten : Rules
{
    static bool TakesFailure(Phase) { return true; }
};

struct WidePlace : Rules
{
    static bool Near(SightPoint const& a, SightPoint const& b) { return WorldTravel::WithinSight(a, b, 20.0f); }
    static bool SameKey(Key const& stored, Key const& asked)
    {
        if (stored.kind == Kind::Place && asked.kind == Kind::Place)
            return Near(stored.from, asked.from);
        return Rules::SameKey(stored, asked);
    }
};

struct OneWayLink : Rules
{
    static bool SameKey(Key const& stored, Key const& asked)
    {
        if (stored.kind == Kind::Link && asked.kind == Kind::Link)
            return stored.fromNode == asked.fromNode && stored.toNode == asked.toNode;
        return Rules::SameKey(stored, asked);
    }
};

struct OneEndWalk : Rules
{
    static bool SameKey(Key const& stored, Key const& asked)
    {
        if (stored.kind == Kind::Walk && asked.kind == Kind::Walk)
            return Near(stored.from, asked.from);
        return Rules::SameKey(stored, asked);
    }
};

struct AnyWay : Rules
{
    static bool SameKey(Key const& stored, Key const& asked)
    {
        if (stored.kind == Kind::Way && asked.kind == Kind::Way)
            return true;
        return Rules::SameKey(stored, asked);
    }
};

struct FirstSlotOut : Rules
{
    static std::size_t SlotFor(Record const& record, std::uint32_t)
    {
        return record.count < FailedAttempts::MAX_ENTRIES ? record.count : 0;
    }
};

struct OldestOnly : Rules
{
    static std::size_t SlotFor(Record const& record, std::uint32_t nowMs)
    {
        if (record.count < FailedAttempts::MAX_ENTRIES)
            return record.count;
        std::size_t oldest = 0;
        for (std::size_t i = 0; i < record.count; ++i)
            if (nowMs - record.entries[i].lastMs > nowMs - record.entries[oldest].lastMs)
                oldest = i;
        return oldest;
    }
};

struct NoFlyWay : Rules
{
    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        if (failure.leg != Leg::Fly || target == Target::Takeoff)
            return Rules::KeyFor(target, failure);
        Failure walk = failure;
        walk.leg = Leg::GraphWalk;
        return Rules::KeyFor(target, walk);
    }
};

struct TimeoutAnyLeg : Rules
{
    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        return Rules::KeyFor(target == Target::Ride ? Target::Leg : target, failure);
    }
};

struct TakeoffAlwaysSpot : Rules
{
    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        Failure ground = failure;
        ground.airLink = false;
        return Rules::KeyFor(target, target == Target::Takeoff ? ground : failure);
    }
};

struct BookAnyLeg : Rules
{
    static bool ReachesStuckBook(CauseRow const& row, Failure const& failure)
    {
        return row.stuckBook && failure.onMesh;
    }
};

struct BookOffMesh : Rules
{
    static bool ReachesStuckBook(CauseRow const& row, Failure const& failure)
    {
        return row.stuckBook && failure.leg == Leg::GraphWalk;
    }
};

struct AnyPlaceRefusesStart : Rules
{
    static bool RefusesStart(Key const&) { return true; }
};

struct ResetKeeps : Rules
{
    static void Reset(Record&) {}
};

// The pick's mutants (PA32). "The pick returns the first candidate" is the same broken rule as PickIgnoresRecord here:
// the pick keeps the offered candidates and the picker draws from them
struct PickIgnoresRecord : Rules
{
    static bool Offers(bool, bool) { return true; }
};

struct PickIgnoresSpot : Rules
{
    static bool Offers(bool placeRefused, bool) { return !placeRefused; }
};

struct LocalPieceLink : Rules
{
    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        if (failure.leg == Leg::LocalPiece && target == Target::Leg)
            return FailedAttempts::LinkKey(failure);
        return Rules::KeyFor(target, failure);
    }
};

struct NoneRowWritesPlace : Rules
{
    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        return Rules::KeyFor(target == Target::None ? Target::Goal : target, failure);
    }
};

// PA56's mutants: today's rule (the refused walk at the start keys only the walk), and every refused walk refusing the
// spot it began at
struct WalkStartKeysWalkOnly : Rules
{
    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        std::optional<Key> key = Rules::KeyFor(target, failure);
        if (key && key->kind == Kind::Walk)
            key->spot = false;
        return key;
    }
};

struct AnyRefusedWalkRefusesStart : Rules
{
    static std::optional<Key> KeyFor(Target target, Failure const& failure)
    {
        std::optional<Key> key = Rules::KeyFor(target, failure);
        if (key && key->kind == Kind::Walk)
            key->spot = true;
        return key;
    }
};

struct RefusedForever : Rules
{
    static Phase PhaseOf(Entry const&, std::uint32_t) { return Phase::Live; }
};

using SuiteFn = void (*)(Counts&);

struct Mutant
{
    char const* name;
    SuiteFn suite;
};
}  // namespace

int main()
{
    Counts counts;
    Suite<Rules>(counts);

    Mutant const mutants[] = {
        {"mutant killed: the expiry without its cap", Suite<UncappedExpiry>},
        {"mutant killed: the repeats without their cap", Suite<UncappedRepeats>},
        {"mutant killed: no repeat window", Suite<NoRepeatWindow>},
        {"mutant killed: a repeat window without end", Suite<EndlessRepeatWindow>},
        {"mutant killed: times that wrap", Suite<WrappingTimes>},
        {"mutant killed: a live entry rewritten", Suite<LiveRewritten>},
        {"mutant killed: a place matched within 20 yd", Suite<WidePlace>},
        {"mutant killed: a link one way only", Suite<OneWayLink>},
        {"mutant killed: a walk matched by one end", Suite<OneEndWalk>},
        {"mutant killed: any way matches", Suite<AnyWay>},
        {"mutant killed: the first slot goes, not the oldest", Suite<FirstSlotOut>},
        {"mutant killed: a gone entry kept before an older live one", Suite<OldestOnly>},
        {"mutant killed: a fly leg's failure blames its link, not flying", Suite<NoFlyWay>},
        {"mutant killed: a timeout of any leg blames it", Suite<TimeoutAnyLeg>},
        {"mutant killed: a takeoff on an air link blames the spot", Suite<TakeoffAlwaysSpot>},
        {"mutant killed: the stuck book takes any leg", Suite<BookAnyLeg>},
        {"mutant killed: the stuck book takes a bot off the mesh", Suite<BookOffMesh>},
        {"mutant killed: any place refuses a start", Suite<AnyPlaceRefusesStart>},
        {"mutant killed: the reset keeps the entries", Suite<ResetKeeps>},
        {"mutant killed: the pick ignores the record", Suite<PickIgnoresRecord>},
        {"mutant killed: the pick ignores a refused spot", Suite<PickIgnoresSpot>},
        {"mutant killed: a local piece writes its link", Suite<LocalPieceLink>},
        {"mutant killed: a cause without a target writes a place", Suite<NoneRowWritesPlace>},
        {"mutant killed: a refusal never expires", Suite<RefusedForever>},
        {"mutant killed: a walk refused at the start keys only the walk", Suite<WalkStartKeysWalkOnly>},
        {"mutant killed: every refused walk refuses its first point's starts", Suite<AnyRefusedWalkRefusesStart>},
    };
    for (Mutant const& mutant : mutants)
    {
        Counts silent;
        silent.loud = false;
        mutant.suite(silent);
        counts.Check(silent.failures > 0, mutant.name);
    }

    std::printf("failed-attempts-selftest: %d passed, %d failed\n", counts.checks - counts.failures, counts.failures);
    return counts.failures == 0 ? 0 : 1;
}
