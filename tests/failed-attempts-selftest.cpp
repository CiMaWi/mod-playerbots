/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of travel's record of failed attempts (WT64c; PLAN 3.20 (6)(7), INTERFACE 5): the expiry (10, 20, 40,
// 80 min and its cap), a repeat only within as long again after the expiry, a live entry left as it was, wrap-safe
// times, a spot's entry refusing a start there and a goal near it, 16 entries with the oldest first out, the four
// kinds' matching, a row for every word of FailWord, StuckCauseWord, the trip's own ends and GoTo::CAUSES, the entry
// each cause writes, which causes reach the stuck book, the reset and the plain copy. The numbers follow INTERFACE 5,
// not the memos the record replaces: a place matches within 10 yd (the stuck avoid widened from 20 yd with repeats,
// the inn memo used 15 yd and 60 min). Each mutant (one rule broken on purpose) must fail the suite.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel
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

    // In step with GoTo's table: every GoTo word has a row, every row is a GoTo word but the two GoTo never sees
    bool goTo = true;
    for (GoTo::CauseRow const& row : GoTo::CAUSES)
        goTo = goTo && FailedAttempts::FindCause(row.word);
    for (CauseRow const& row : FailedAttempts::CAUSES)
        goTo = goTo && (InGoToCauses(row.word) || row.word == "none" || row.word == "inn_failed");
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
