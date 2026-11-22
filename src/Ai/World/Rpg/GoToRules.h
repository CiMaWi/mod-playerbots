/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GOTORULES_H
#define PLAYERBOTS_GOTORULES_H

// How a status walks to a place (WT64b; PLAN 3.20 (1)-(5), INTERFACE 1), engine-free. A status calls GoTo(place,
// reach) every tick its executor runs and gets one of three answers: There (within reach: do the work), Going (a local
// piece or a trip is under way) or Failed (an end to read: its outcome and cause word). Five steps, in this order:
//   1. within reach: There;
//   2. a live refusal of this place: Failed with its cause, no path query, no request;
//   3. an end of this place's trip waits unread: There (arrived) or Failed, read once;
//   4. the place is on the bot's map within the local range and the mesh carries the path to it: order it, Going;
//   5. else a trip to the place (farther, another map, or near but not carried): Going, and no move is ordered.
// Each step is O(1) but one path query, asked only when the place or the bot's last move changed (State). A refusal
// is never repaired by another move: it is a typed end. Nothing here names a status kind or a place: the holder passes
// its place and reach, and the caller hands its status id to travel. tests/go-to-selftest.cpp drives every rule.
// The same judge serves the other orders of the statuses and of following (WT64g, PLAN 3.20 (8)): an approach of an
// object (step 4 at any distance, no trip: a stand point not carried is a typed end), each roam sample (RoamCarried)
// and the route follower's order on its leg (LegCarried).

#include <cstdint>
#include <optional>
#include <string_view>

#include "MeshMoveRules.h"
#include "StatusEndRules.h"

namespace GoTo
{
// A local piece is at most this far (the far walk's pathFinderDis; WT.40: a tuning value, revisable)
constexpr float LOCAL_RANGE_YARDS = 70.0f;
// An approach (WT64g) is one judged order at any distance on the map: the path's own length decides (a path longer
// than the engine's point limit is not carried)
constexpr float APPROACH_RANGE_YARDS = 1000.0f;
// A roam sample is carried when its path ends this close to it (WT.40: a tuning value, the engine's hop step)
constexpr float ROAM_REACH_YARDS = 5.0f;
// The follower's order beyond the local range: its carried path's end gains this much on the place (the old far walk's
// primary order)
constexpr float LEG_GAIN_YARDS = 5.0f;
// The cause word of an approach whose stand point the mesh does not carry
constexpr char const* NOT_CARRIED = "no_path";

enum class Answer : std::uint8_t
{
    There,
    Going,
    Failed
};

// The step that answered, in the order they are tried
enum class Step : std::uint8_t
{
    There,
    Refused,
    EndRead,
    Local,
    Trip
};

struct Place
{
    std::uint32_t map{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    constexpr bool operator==(Place const& other) const
    {
        return map == other.map && x == other.x && y == other.y && z == other.z;
    }
};

// A move is carried when the mesh answers it with a path of class Carried (NORMAL or INCOMPLETE, no hop) whose last
// point lies within the order's reach of the place asked for. No path, the engine's line, a shortcut, an end far from
// a polygon, a hop and an incomplete path that ends short of the reach are not carried (PLAN 3.20 (1))
inline bool Carried(std::uint32_t pathType, float lastStep, float endToPlaceYards, float reach)
{
    return MeshMove::Classify(pathType, lastStep) == MeshMove::Class::Carried && endToPlaceYards <= reach;
}

// A roam sample (WT64g, site 15): carried as any order, its path ending within ROAM_REACH_YARDS of the sample; one that
// is not is dropped and the next drawn. No end far from a polygon and no incomplete path that ends short
inline bool RoamCarried(std::uint32_t pathType, float lastStep, float endToSampleYards)
{
    return Carried(pathType, lastStep, endToSampleYards, ROAM_REACH_YARDS);
}

// The route follower's order on one path answer (WT64g, site 11): its class is Carried (a walk link's hop that the link
// counts itself is taken: hopTaken) and its end lies within reach of the place or, for a place beyond the local range,
// gains LEG_GAIN_YARDS on it. Anything else is refused: the leg fails, and no other move is tried
inline bool LegCarried(std::uint32_t pathType, float lastStep, bool hopTaken, float endToPlaceYards, float yards,
                       float reach)
{
    MeshMove::Class const cls = MeshMove::Classify(pathType, lastStep);
    if (cls != MeshMove::Class::Carried && !(hopTaken && cls == MeshMove::Class::Hop))
        return false;
    return yards <= LOCAL_RANGE_YARDS ? endToPlaceYards <= reach : endToPlaceYards + LEG_GAIN_YARDS < yards;
}

// A walk off the graph's probe step (TravelWalk::ProbePiece; WT64i, WT.59), the one reader of the engine's answer: the
// follower's order from the step's start to the point the step asked, its aim (a corner on the way or the walk's end;
// LegCarried without a counted hop, TravelWalk::REACH_YARDS as the follower's), on an answer with points. The probe
// asks the order's own smooth path (TravelWalk::OrderStep), so the engine's one line refuses the piece before the bot
// sets out, and so does an end the follower's order from there would refuse (WT64i round 4: a partial path that stops
// more than REACH_YARDS short within the local range)
inline bool ProbeCarried(TravelWalk::ProbeAnswer const& answer, TravelWalk::ProbePoint const& start,
                         TravelWalk::ProbePoint const& aim)
{
    return answer.result && !answer.points.empty() &&
           LegCarried(answer.type, MeshMove::LastStep(answer.points), false,
                      TravelWalk::Distance(answer.points.back(), aim), TravelWalk::Distance(start, aim),
                      TravelWalk::REACH_YARDS);
}

// An approach's stand point (WT64g; WT64i): picked once per object and reach, since a point drawn each tick would make
// each tick a new place. Picked again for another object, another reach (an errand's 1 yd sight step after its 5.5 yd
// approach: the point drawn for 5.5 yd lies out of the new reach), an object that moved beyond the reach of where the
// point was picked, or a bot that stands within the reach of that place
inline bool RepickStand(bool sameObject, float pickedReach, float reach, float objectMovedYards, float botToPickYards)
{
    return !sameObject || pickedReach != reach || objectMovedYards > reach || botToPickYards <= reach;
}

// Whether a failed end writes its marks (WT64i): the Stuck row and the place refused for a while (step 2). A quiet
// order (an errand's sight step: a step toward an object out of sight, whose failure its caller passes over) writes
// neither, so that its failure refuses no other work at that place
inline bool MarksFailure(bool quiet) { return !quiet; }

// A local piece's give-up (90 s without advance; WT64i round 5): giveUp() -> its word is the mover's give-up (its Stuck
// row with the classified cause, the avoid, the off-mesh recovery); a quiet order skips it and ends with no word of its
// own, so its failure marks nothing, as any of its failed ends (MarksFailure)
template <class GiveUp>
char const* GiveUpWord(bool quiet, GiveUp const& giveUp)
{
    return MarksFailure(quiet) ? giveUp() : nullptr;
}

// The outcome of an end's cause word (INTERFACE 1), one table for every holder. A trip's word comes as its abort
// source ("abort:no_route") or bare; a local move's as its Stuck word (WorldTravel::StuckCauseWord); a holder's own end
// as its outcome's word (StatusEnd::Word)
struct CauseRow
{
    std::string_view word;
    StatusEnd::Outcome outcome;
};

inline constexpr CauseRow CAUSES[] = {
    {"arrived", StatusEnd::Outcome::Arrived},
    {"finished", StatusEnd::Outcome::Finished},
    {"deadline", StatusEnd::Outcome::TimedOut},
    {"timeout", StatusEnd::Outcome::TimedOut},
    {"stalled", StatusEnd::Outcome::Stuck},
    {"stuck", StatusEnd::Outcome::Stuck},
    {"no_progress", StatusEnd::Outcome::Stuck},
    {"door_or_elevator", StatusEnd::Outcome::Stuck},
    {"off_mesh", StatusEnd::Outcome::Stuck},
    {"stuck_aboard", StatusEnd::Outcome::Stuck},
    {"on_transport", StatusEnd::Outcome::Stuck},
    {"fell_or_swimming", StatusEnd::Outcome::Stuck},
    {"other", StatusEnd::Outcome::Stuck},  // a local give-up with no named cause: the walk's stuck end, as today
    {"abandoned", StatusEnd::Outcome::Abandoned},
    {"no_path", StatusEnd::Outcome::Abandoned},
    {"no_walk", StatusEnd::Outcome::Abandoned},
    {"no_route", StatusEnd::Outcome::Abandoned},
    {"unreachable_poi", StatusEnd::Outcome::Abandoned},
    {"no_answer", StatusEnd::Outcome::Abandoned},
    {"no_service", StatusEnd::Outcome::Abandoned},
    {"death", StatusEnd::Outcome::Abandoned},
    {"portal", StatusEnd::Outcome::Abandoned},
    {"taxi", StatusEnd::Outcome::Abandoned},
    {"fare", StatusEnd::Outcome::Abandoned},
    {"landing", StatusEnd::Outcome::Abandoned},
    {"hearth", StatusEnd::Outcome::Abandoned},
    {"takeoff", StatusEnd::Outcome::Abandoned},
    {"transport_lost", StatusEnd::Outcome::Abandoned},
    {"no_exit", StatusEnd::Outcome::Abandoned},
    {"no_crossing", StatusEnd::Outcome::Abandoned},
    {"crossing_failed", StatusEnd::Outcome::Abandoned},
    {"wrong_map", StatusEnd::Outcome::Abandoned},
    {"not_bound", StatusEnd::Outcome::Abandoned},
    {"unsupported", StatusEnd::Outcome::Abandoned},  // a leg type the follower does not run (none since WT19)
    {"interrupted", StatusEnd::Outcome::Interrupted},
    {"held", StatusEnd::Outcome::Interrupted},
    {"reset", StatusEnd::Outcome::Interrupted},  // TripHold::Reset
    {"order", StatusEnd::Outcome::Ordered},
    {"stopped", StatusEnd::Outcome::Ordered},
    {"sick", StatusEnd::Outcome::Sick},
};

constexpr std::string_view ABORT_PREFIX = "abort:";

// The word without a trip's "abort:" prefix
constexpr std::string_view CauseOf(char const* word)
{
    std::string_view bare = word ? std::string_view(word) : std::string_view();
    if (bare.substr(0, ABORT_PREFIX.size()) == ABORT_PREFIX)
        bare.remove_prefix(ABORT_PREFIX.size());
    return bare;
}

// The table's outcome of a word; none for a word the table lacks (a bug: the selftest lists every source's words)
constexpr std::optional<StatusEnd::Outcome> KnownOutcome(char const* word)
{
    std::string_view const cause = CauseOf(word);
    for (CauseRow const& row : CAUSES)
        if (row.word == cause)
            return row.outcome;
    return std::nullopt;
}

// An unknown word is a failure, not progress: Abandoned
constexpr StatusEnd::Outcome OutcomeOf(char const* word)
{
    return KnownOutcome(word).value_or(StatusEnd::Outcome::Abandoned);
}

// What the holder knows this tick, all O(1)
struct Facts
{
    Place place;
    bool sameMap{false};  // the place is on the bot's map
    float yards{0.0f};    // the bot to the place (same map only)
    float reach{0.0f};    // the holder's reach: within it the work begins
    float localRange{LOCAL_RANGE_YARDS};
    char const* refused{nullptr};  // a live entry of the record of failed attempts: its cause word (a literal)
    bool tripHeld{false};          // the holder's trip is held or asked for: the follower is its one stepper
    std::uint32_t moveStamp{0};    // the caller's stamp of the bot's last move: it changes when the bot's motion is no
                                   // longer the local order GoTo gave (another action moved it, the motion ended)
    bool tripAllowed{true};        // false: an approach (WT64g), whose place not carried is Failed NOT_CARRIED, no trip
};

// The holder's GoTo memory: in its status data, fresh with each status
struct State
{
    Place tripPlace;        // the place the last trip was asked for
    bool tripAsked{false};  // its end is still to be read
    Place judgedPlace;      // the judge's last answer, for this place and this move stamp
    std::uint32_t judgedMove{0};
    bool judged{false};
    bool carried{false};
};

struct Decision
{
    Step step{Step::There};
    Answer answer{Answer::There};
    StatusEnd::Outcome outcome{StatusEnd::Outcome::Arrived};  // Failed: the holder's end
    char const* word{nullptr};                                // Failed: the cause word (a literal); EndRead: the end's
    bool orderLocal{false};  // order the local piece now (a fresh judgement); else the ordered move goes on
    bool askTrip{false};     // ask travel for a trip to the place, with the holder's status as its goal word
    bool pathAsked{false};   // the judge ran this tick
};

// One tick of GoTo. takeEnd() -> std::optional<StatusEnd::TripEnd>: the holder's unread trip end, once
// (TripHold::TakeEnd); judge() -> bool: the mesh carries the local piece (a path query and Carried). Each is called at
// most once, and only by the step that needs it
template <class TakeEnd, class Judge>
Decision Decide(Facts const& facts, State& state, TakeEnd&& takeEnd, Judge&& judge)
{
    Decision decision;
    if (!(facts.place == state.tripPlace))
        state.tripAsked = false;  // another place: the old trip's end is not this place's
    if (facts.sameMap && facts.yards <= facts.reach)
    {
        state.tripAsked = false;
        return decision;
    }
    if (facts.refused)
    {
        decision.step = Step::Refused;
        decision.answer = Answer::Failed;
        decision.outcome = OutcomeOf(facts.refused);
        decision.word = facts.refused;
        return decision;
    }
    if (state.tripAsked && !facts.tripHeld)
    {
        if (std::optional<StatusEnd::TripEnd> const end = takeEnd())
        {
            state.tripAsked = false;
            decision.step = Step::EndRead;
            decision.word = end->word;
            decision.outcome = end->outcome;
            decision.answer = end->outcome == StatusEnd::Outcome::Arrived ? Answer::There : Answer::Failed;
            return decision;
        }
    }
    decision.answer = Answer::Going;
    if (facts.tripHeld)
    {
        decision.step = Step::Trip;  // one stepper: the follower walks the held trip
        return decision;
    }
    if (facts.sameMap && facts.yards <= facts.localRange)
    {
        bool const fresh = !state.judged || !(state.judgedPlace == facts.place) || state.judgedMove != facts.moveStamp;
        if (fresh)
        {
            state.carried = judge();
            state.judged = true;
            state.judgedPlace = facts.place;
            state.judgedMove = facts.moveStamp;
            decision.pathAsked = true;
        }
        if (state.carried)
        {
            decision.step = Step::Local;
            decision.orderLocal = fresh;
            return decision;
        }
    }
    if (!facts.tripAllowed)
    {
        // An approach asks no trip (PLAN 3.20 (8)): the last yards to an object the mesh does not carry are a typed end
        decision.step = Step::Local;
        decision.answer = Answer::Failed;
        decision.outcome = OutcomeOf(NOT_CARRIED);
        decision.word = NOT_CARRIED;
        return decision;
    }
    decision.step = Step::Trip;
    decision.askTrip = true;
    state.tripAsked = true;
    state.tripPlace = facts.place;
    return decision;
}

// A trip that is asked for and that travel does not begin (another holder's trip is held) is an end with a cause
// word, at once: a wait for it would have no bound. The holder ends with the word's outcome, and that end ends the
// held trip (the status core's replacing step), so the next status begins its own
constexpr char const* BEGIN_REFUSED = "held";

// The decision of a tick that asked a trip, after the begin: begun, it stands (Going, the end still to be read);
// refused, it is Failed with BEGIN_REFUSED, asks no trip, orders no move and leaves no end to read
inline Decision AfterBegin(Decision decision, State& state, bool begun)
{
    if (begun || !decision.askTrip)
        return decision;
    state.tripAsked = false;
    decision.answer = Answer::Failed;
    decision.outcome = OutcomeOf(BEGIN_REFUSED);
    decision.word = BEGIN_REFUSED;
    decision.askTrip = false;
    return decision;
}

// An approach's order on its wide path (pathing PA5) is moved by the engine's point move, which searches its own path
// of 1,024 nodes: the order is carried only when that path is. To the wide path's end it is carried as the judge's
// own path (Carried, within the reach of the place); to a point on the way it is a NORMAL path ending within
// TravelWalk::REACH_YARDS of the point: a partial one that ends short can bring the bot to a spot whose own order falls
// short back to the first (offline, pathing PA5: one place, a ping-pong between two floors)
inline bool OrderCarried(std::uint32_t pathType, float lastStep, bool toEnd, float endToPointYards,
                         float endToPlaceYards, float reach)
{
    if (toEnd)
        return Carried(pathType, lastStep, endToPlaceYards, reach);
    return (pathType & TravelWalk::PATH_NORMAL) &&
           Carried(pathType, lastStep, endToPointYards, TravelWalk::REACH_YARDS);
}
}  // namespace GoTo

#endif
