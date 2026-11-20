/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of GoTo's rules (WT64b; PLAN 3.20 (1)-(5), INTERFACE 1): the five steps in their order, an end read
// once, a holder's end ends its trip, a place on another map or beyond a local piece asks for a trip, a near place
// whose path is not carried asks for a trip and orders no move, the path query only on a change, the judge with its
// reach, and the cause table over every word of a trip's abort sources and of the local Stuck words. WT64g: an
// approach (a point not carried takes the path's end within the reach, else no_path, never a trip), a roam sample
// that is not carried is dropped, and the follower's leg order (LegCarried). WT64i: an approach's stand point is
// picked again when the reach changes (the errand's sight step), and a quiet order's failure writes no Stuck row and
// refuses no place. Each mutant (one rule broken on purpose) must fail the suite.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel
//   tests/go-to-selftest.cpp

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string_view>

#include "GoToRules.h"
#include "TravelLegRules.h"
#include "WorldTravelRules.h"

namespace
{
using GoTo::Answer;
using GoTo::Decision;
using GoTo::Facts;
using GoTo::Step;
using StatusEnd::Outcome;

constexpr std::uint32_t HOLDER = StatusEnd::STATUS_DO_QUEST;
constexpr std::uint32_t OTHER = StatusEnd::STATUS_GO_GRIND;
constexpr GoTo::Place PLACE{0, 100.0f, 200.0f, 30.0f};
constexpr GoTo::Place SECOND{0, 300.0f, 200.0f, 30.0f};
constexpr float REACH = 5.0f;

static_assert(GoTo::OutcomeOf("abort:no_route") == Outcome::Abandoned, "the table is constexpr");
static_assert(static_cast<int>(Step::There) < static_cast<int>(Step::Refused) &&
                  static_cast<int>(Step::Refused) < static_cast<int>(Step::EndRead) &&
                  static_cast<int>(Step::EndRead) < static_cast<int>(Step::Local) &&
                  static_cast<int>(Step::Local) < static_cast<int>(Step::Trip),
              "the five steps in their order");

struct StubTrip
{
    int id{0};
};

// One bot: its held trip, its GoTo memory, the judge's answer and the calls each rule made
struct Bot
{
    StatusEnd::TripHold<StubTrip> hold;
    GoTo::State state;
    bool carried{true};
    int judgeCalls{0};
    int takeCalls{0};
    bool lastRead{false};  // the mutant "the holder's end is left to read"
};

using DecideFn = Decision (*)(Facts const&, Bot&);
using CarriedFn = bool (*)(std::uint32_t, float, float, float);
using RoamFn = bool (*)(std::uint32_t, float, float);
using LegFn = bool (*)(std::uint32_t, float, bool, float, float, float);

constexpr std::uint32_t NORMAL = TravelWalk::PATH_NORMAL;
constexpr std::uint32_t INCOMPLETE = TravelWalk::PATH_INCOMPLETE;
constexpr std::uint32_t FAR_END = TravelWalk::PATH_FARFROMPOLY_END;

Decision Real(Facts const& facts, Bot& bot)
{
    return GoTo::Decide(
        facts, bot.state,
        [&]
        {
            ++bot.takeCalls;
            return bot.hold.TakeEnd(HOLDER);
        },
        [&]
        {
            ++bot.judgeCalls;
            return bot.carried;
        });
}

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

Facts Near(float yards, GoTo::Place place = PLACE)
{
    Facts facts;
    facts.place = place;
    facts.sameMap = true;
    facts.yards = yards;
    facts.reach = REACH;
    return facts;
}

Facts OtherMap(float yards)
{
    Facts facts = Near(yards);
    facts.sameMap = false;
    facts.place.map = 1;
    return facts;
}

// A trip to PLACE asked and finished by itself with this end, unread
void FinishedTrip(Bot& bot, Outcome outcome, char const* word, std::uint32_t holder = HOLDER)
{
    bot.state.tripAsked = true;
    bot.state.tripPlace = PLACE;
    bot.hold.Begin(holder, StubTrip{1});
    bot.hold.Finish(outcome, word);
}

bool Word(Decision const& decision, char const* word)
{
    return decision.word && std::string_view(decision.word) == word;
}

bool NoMove(Decision const& decision) { return !decision.orderLocal && !decision.askTrip; }

// An approach (WT64g): an object's stand point judged at any distance, no trip. The judge asks the path to the
// stand point and measures its end against the object (the place), so the carried path's end within the reach is
// ordered and one beyond it is no_path
void Approaches(DecideFn decide, CarriedFn carried, Counts& c)
{
    auto const approach = [](float yards)
    {
        Facts facts = Near(yards);
        facts.tripAllowed = false;
        facts.localRange = GoTo::APPROACH_RANGE_YARDS;
        return facts;
    };
    {
        Bot bot;
        bot.carried = carried(NORMAL, 3.0f, 0.5f, REACH);
        Decision const d = decide(approach(30.0f), bot);
        c.Check(d.step == Step::Local && d.answer == Answer::Going && d.orderLocal && !d.askTrip,
                "approach: a carried stand point is ordered");
    }
    {
        Bot bot;
        bot.carried = carried(INCOMPLETE, 3.0f, REACH - 1.0f, REACH);
        Decision const d = decide(approach(30.0f), bot);
        c.Check(d.step == Step::Local && d.orderLocal && !d.askTrip,
                "approach: a stand point not carried takes the path's end within the reach of the object");
    }
    {
        Bot bot;
        bot.carried = carried(INCOMPLETE, 3.0f, REACH + 1.0f, REACH);
        Decision const d = decide(approach(30.0f), bot);
        c.Check(d.answer == Answer::Failed && Word(d, "no_path") && d.outcome == Outcome::Abandoned && NoMove(d) &&
                    !bot.state.tripAsked,
                "approach: a path's end beyond the reach fails no_path, no trip, no move");
    }
    {
        Bot bot;
        bot.carried = carried(NORMAL | FAR_END, 3.0f, 1.0f, REACH);
        Decision const d = decide(approach(30.0f), bot);
        c.Check(d.answer == Answer::Failed && Word(d, "no_path") && NoMove(d),
                "approach: an end far from a polygon is not carried, within the reach too");
    }
    {
        Bot bot;
        bot.carried = carried(NORMAL, 3.0f, 0.5f, REACH);
        Decision const d = decide(approach(GoTo::LOCAL_RANGE_YARDS + 200.0f), bot);
        c.Check(d.step == Step::Local && d.pathAsked && d.orderLocal && !d.askTrip,
                "approach: beyond the local range one judged order still, not a trip");
        Decision const again = decide(approach(GoTo::LOCAL_RANGE_YARDS + 199.0f), bot);
        c.Check(!again.pathAsked && !again.orderLocal && again.answer == Answer::Going && bot.judgeCalls == 1,
                "approach: judged once per stand point and move");
    }
    {
        Bot bot;
        Facts refused = approach(30.0f);
        refused.refused = "no_path";
        Decision const d = decide(refused, bot);
        c.Check(d.step == Step::Refused && d.answer == Answer::Failed && NoMove(d) && bot.judgeCalls == 0,
                "approach: a refused object fails at once, no path query");
    }
}

// A roam sample (WT64g): carried and ending at the sample, else dropped and the next drawn (ROAM_SAMPLES as before)
void Roams(RoamFn roam, Counts& c)
{
    c.Check(roam(NORMAL, 3.0f, 0.5f), "roam: a carried sample is ordered");
    c.Check(!roam(INCOMPLETE, 3.0f, GoTo::ROAM_REACH_YARDS + 7.0f),
            "roam: an incomplete path that ends short drops it");
    c.Check(!roam(NORMAL | FAR_END, 3.0f, 0.5f), "roam: an end far from a polygon drops it");
    c.Check(!roam(TravelWalk::PATH_NOPATH, 0.0f, 0.0f), "roam: no path drops it");
    c.Check(!roam(INCOMPLETE, TravelWalk::HOP_STEP_YARDS + 4.0f, 0.5f), "roam: a hop drops it");
    c.Check(roam(NORMAL, TravelWalk::HOP_STEP_YARDS + 0.9f, 0.5f), "roam: a NORMAL path's slope step is no hop");
    // The draw: the first carried sample of eight is ordered; none carried, no order
    struct Sample
    {
        std::uint32_t type;
        float endYards;
    };
    Sample const draws[] = {{NORMAL | FAR_END, 0.5f}, {INCOMPLETE, 20.0f}, {NORMAL, 1.0f}, {NORMAL, 0.2f}};
    int ordered = -1;
    for (int i = 0; i < static_cast<int>(std::size(draws)) && ordered < 0; ++i)
        if (roam(draws[i].type, 3.0f, draws[i].endYards))
            ordered = i;
    c.Check(ordered == 2, "roam: the samples not carried are dropped, the next carried one is ordered");
}

// The follower's order on its leg (WT64g, site 11): within the local range the end within reach of the place, beyond
// it a carried end that gains on it; a hop only where the link counts it
void Legs(LegFn leg, Counts& c)
{
    constexpr float REACH_LEG = TravelWalk::REACH_YARDS;
    c.Check(leg(NORMAL, 3.0f, false, 2.0f, 60.0f, REACH_LEG), "leg: a carried local order is taken");
    c.Check(!leg(INCOMPLETE, 3.0f, false, 25.0f, 60.0f, REACH_LEG),
            "leg: a local order whose path ends short of the reach is refused");
    c.Check(leg(INCOMPLETE, 3.0f, false, 150.0f, 200.0f, REACH_LEG),
            "leg: a far order whose carried end gains is taken");
    c.Check(!leg(INCOMPLETE, 3.0f, false, 197.0f, 200.0f, REACH_LEG),
            "leg: a far order that gains too little is refused");
    c.Check(!leg(NORMAL | FAR_END, 3.0f, false, 2.0f, 60.0f, REACH_LEG), "leg: an end far from a polygon is refused");
    c.Check(!leg(NORMAL | MeshMove::PATH_SHORTCUT, 3.0f, false, 2.0f, 60.0f, REACH_LEG), "leg: a shortcut is refused");
    c.Check(
        !leg(INCOMPLETE, 12.0f, false, 2.0f, 60.0f, REACH_LEG) && leg(INCOMPLETE, 12.0f, true, 2.0f, 60.0f, REACH_LEG),
        "leg: a hop is taken only where the walk link counts it");
    c.Check(leg(NORMAL, TravelWalk::HOP_STEP_YARDS + 0.9f, false, 0.3f, 60.0f, REACH_LEG),
            "leg: a NORMAL path's slope step is no hop (WT64g-2)");
}

void Suite(DecideFn decide, CarriedFn carried, Counts& c)
{
    // Step 1 before everything: within reach is There, whatever else waits
    {
        Bot bot;
        FinishedTrip(bot, Outcome::Abandoned, "abort:no_route");
        Facts facts = Near(3.0f);
        facts.refused = "no_path";
        Decision const d = decide(facts, bot);
        c.Check(d.step == Step::There && d.answer == Answer::There && NoMove(d) && bot.judgeCalls == 0 &&
                    bot.takeCalls == 0,
                "1: within reach is There, before a refusal and an unread end (no query, no move)");
        c.Check(decide(OtherMap(3.0f), bot).answer != Answer::There, "1: a place on another map is never within reach");
    }

    // Step 2 before 3 and 4: a refusal is Failed with its cause, no path query, no request, no move
    {
        Bot bot;
        FinishedTrip(bot, Outcome::Abandoned, "abort:no_route");
        Facts facts = Near(20.0f);
        facts.refused = "no_path";
        Decision const d = decide(facts, bot);
        c.Check(d.step == Step::Refused && d.answer == Answer::Failed && Word(d, "no_path") &&
                    d.outcome == Outcome::Abandoned,
                "2: a live refusal is Failed with its cause word and the table's outcome, before the unread end");
        c.Check(NoMove(d) && !d.pathAsked && bot.judgeCalls == 0 && bot.takeCalls == 0,
                "2: a refusal asks no path, no trip and orders no move");

        // Step 3: the end the refusal left waits; it is read once
        Decision const read = decide(Near(20.0f), bot);
        c.Check(read.step == Step::EndRead && read.answer == Answer::Failed && Word(read, "abort:no_route") &&
                    read.outcome == Outcome::Abandoned && NoMove(read) && bot.judgeCalls == 0,
                "3: an unread failed end is Failed with its word, before the local piece (no query)");
        Decision const again = decide(Near(20.0f), bot);
        c.Check(again.step == Step::Local && again.orderLocal, "3: an end is read once: the next tick walks on");
    }

    // Step 3: an arrived end is There, once; an end another status held is not this holder's
    {
        Bot bot;
        FinishedTrip(bot, Outcome::Arrived, "arrived");
        Decision const read = decide(Near(10.0f), bot);
        c.Check(read.step == Step::EndRead && read.answer == Answer::There && NoMove(read),
                "3: an arrived end outside the holder's reach is There");
        c.Check(decide(Near(10.0f), bot).step != Step::EndRead, "3: the arrived end is read once");

        Bot other;
        FinishedTrip(other, Outcome::Arrived, "arrived", OTHER);
        c.Check(decide(Near(10.0f), other).step != Step::EndRead, "3: an end another status held is not read");

        Bot moved;
        FinishedTrip(moved, Outcome::Arrived, "arrived");
        Decision const elsewhere = decide(Near(10.0f, SECOND), moved);
        Decision const back = decide(Near(10.0f), moved);
        c.Check(elsewhere.step == Step::Local && back.step != Step::EndRead,
                "3: an end is this place's: another place forgets it");
    }

    // A holder's end ends its trip and leaves nothing to read
    {
        Bot bot;
        bot.state.tripAsked = true;
        bot.state.tripPlace = PLACE;
        bot.hold.Begin(HOLDER, StubTrip{2});
        c.Check(bot.hold.End(Outcome::Interrupted, "interrupted") && !bot.hold.Held() &&
                    bot.hold.Holder() == StatusEnd::STATUS_IDLE,
                "end: the holder's end ends its trip");
        c.Check(decide(Near(20.0f), bot).step != Step::EndRead, "end: the holder's own end is not read as its trip's");
        Bot reset;
        reset.hold.Begin(HOLDER, StubTrip{3});
        c.Check(reset.hold.Reset(false) == StatusEnd::ResetEffect::Fresh && !reset.hold.Held(),
                "end: a reset ends the trip with its holder");
    }

    // Step 4: the local piece is carried: ordered on a fresh judgement, the path asked only on a change
    {
        Bot bot;
        Facts facts = Near(20.0f);
        facts.moveStamp = 1;
        Decision const first = decide(facts, bot);
        c.Check(first.step == Step::Local && first.answer == Answer::Going && first.orderLocal && first.pathAsked &&
                    !first.askTrip && bot.judgeCalls == 1,
                "4: a carried local piece is ordered, Going, no trip");
        Decision const second = decide(facts, bot);
        c.Check(second.step == Step::Local && !second.orderLocal && !second.pathAsked && bot.judgeCalls == 1,
                "4: the same place and move: no new path query, the order goes on");
        facts.moveStamp = 2;
        Decision const moved = decide(facts, bot);
        c.Check(moved.orderLocal && bot.judgeCalls == 2, "4: the bot's last move changed: one new query and order");
        decide(Near(20.0f, SECOND), bot);
        c.Check(bot.judgeCalls == 3, "4: another place: one new query");
        Bot edge;
        decide(Near(GoTo::LOCAL_RANGE_YARDS), edge);
        c.Check(edge.judgeCalls == 1, "4: the local range's edge is a local piece");
    }

    // Step 5: a near place whose path is not carried asks for a trip and orders no move; a held trip is the follower's
    {
        Bot bot;
        bot.carried = false;
        Decision const d = decide(Near(20.0f), bot);
        c.Check(d.step == Step::Trip && d.answer == Answer::Going && d.askTrip && !d.orderLocal && bot.judgeCalls == 1,
                "5: a near place not carried asks for a trip and orders no move");
        Facts held = Near(20.0f);
        held.tripHeld = true;
        Decision const going = decide(held, bot);
        c.Check(going.step == Step::Trip && going.answer == Answer::Going && NoMove(going) && bot.judgeCalls == 1,
                "5: a held trip goes on: no new request, no query, no move (one stepper)");
        Bot near;
        Facts nearHeld = Near(20.0f);
        nearHeld.tripHeld = true;
        Decision const ahead = decide(nearHeld, near);
        c.Check(ahead.step == Step::Trip && NoMove(ahead) && near.judgeCalls == 0,
                "5: a held trip near a carried place stays the follower's");
    }

    // Step 5: another map or beyond a local piece asks for a trip without a path query
    {
        Bot bot;
        Decision const other = decide(OtherMap(20.0f), bot);
        c.Check(other.step == Step::Trip && other.askTrip && !other.orderLocal && bot.judgeCalls == 0,
                "5: a place on another map asks for a trip (no query, no move)");
        Decision const far = decide(Near(GoTo::LOCAL_RANGE_YARDS + 1.0f), bot);
        c.Check(far.step == Step::Trip && far.askTrip && !far.orderLocal && bot.judgeCalls == 0,
                "5: a place beyond a local piece asks for a trip (no query, no move)");
    }

    // A whole walk: a trip, its arrival short of the reach, the last yards as a local piece, There
    {
        Bot bot;
        Decision const ask = decide(Near(500.0f), bot);
        bot.hold.Begin(HOLDER, StubTrip{4});
        Facts held = Near(200.0f);
        held.tripHeld = true;
        Decision const going = decide(held, bot);
        bot.hold.Finish(Outcome::Arrived, "arrived");
        Decision const arrived = decide(Near(9.0f), bot);
        Facts last = Near(9.0f);
        last.moveStamp = 1;
        Decision const piece = decide(last, bot);
        Decision const there = decide(Near(4.0f), bot);
        c.Check(ask.askTrip && going.answer == Answer::Going && !going.askTrip && arrived.step == Step::EndRead &&
                    arrived.answer == Answer::There && piece.step == Step::Local && piece.orderLocal &&
                    there.step == Step::There,
                "walk: trip, arrival, the last yards, There");
    }

    // The judge: a carried class and an end within the reach
    {
        using namespace TravelWalk;
        c.Check(carried(PATH_NORMAL, 2.0f, 1.0f, 3.0f), "judge: a normal path ending within reach is carried");
        c.Check(carried(PATH_INCOMPLETE, 2.0f, 2.0f, 3.0f), "judge: an incomplete path ending within reach is carried");
        c.Check(!carried(PATH_INCOMPLETE, 2.0f, 8.0f, 3.0f), "judge: an incomplete path ending short is not carried");
        c.Check(!carried(PATH_NORMAL, 2.0f, 8.0f, 3.0f), "judge: a path ending away from the place is not carried");
        c.Check(!carried(PATH_NOPATH, 0.0f, 0.0f, 3.0f), "judge: no path is not carried");
        c.Check(!carried(PATH_NORMAL | PATH_NOT_USING_PATH, 2.0f, 0.0f, 3.0f),
                "judge: the engine's line is not carried");
        c.Check(!carried(PATH_NORMAL | PATH_SHORT, 2.0f, 0.0f, 3.0f), "judge: a cut path is not carried");
        c.Check(!carried(PATH_NORMAL | MeshMove::PATH_SHORTCUT, 2.0f, 0.0f, 3.0f), "judge: a shortcut is not carried");
        c.Check(!carried(PATH_NORMAL | PATH_FARFROMPOLY_END, 2.0f, 0.0f, 3.0f),
                "judge: an end far from a polygon is not carried");
        c.Check(!carried(PATH_INCOMPLETE, HOP_STEP_YARDS + 1.0f, 0.0f, 3.0f), "judge: a hop is not carried");
        c.Check(carried(PATH_NORMAL, HOP_STEP_YARDS + 0.9f, 0.0f, 3.0f),
                "judge: a NORMAL path's slope step is carried");
    }
}

// WT64i: the errand's sight step after its approach. The approach to a vendor at 5.5 yd drew its stand point for that
// reach; the sight step asks 1 yd of the same vendor from where the bot stands, 5 yd off: the point is drawn again
// (else its path ends 5 yd from the vendor and the step fails no_path), and the step's failure, quiet, writes no
// Stuck row and refuses no place (else the vendor's place refuses the stop's quest ender or trainer)
using StandFn = bool (*)(bool sameObject, float pickedReach, float reach, float objectMovedYards, float botToPickYards);
using MarksFn = bool (*)(bool quiet);

void SightStep(StandFn stand, MarksFn marks, Counts& c)
{
    c.Check(stand(true, 5.5f, 1.0f, 0.0f, 5.0f), "stand: a changed reach draws the stand point again");
    c.Check(!stand(true, 1.0f, 1.0f, 0.0f, 5.0f), "stand: the same object and reach keep the point");
    c.Check(stand(false, 1.0f, 1.0f, 0.0f, 5.0f), "stand: another object draws a point");
    c.Check(stand(true, 1.0f, 1.0f, 2.0f, 5.0f), "stand: an object moved beyond the reach draws a point");
    c.Check(stand(true, 5.5f, 5.5f, 0.0f, 5.0f), "stand: a bot within the reach of the place draws a point");
    struct
    {
        int stuckRows{0};
        bool avoided{false};
    } sight, approach;
    // The failed end's marks as NewRpgBaseAction::JudgedWalk writes them
    auto const fail = [&](bool quiet, auto& written)
    {
        if (marks(quiet))
        {
            ++written.stuckRows;
            written.avoided = true;
        }
    };
    fail(true, sight);
    fail(false, approach);
    c.Check(sight.stuckRows == 0 && !sight.avoided, "sight step: a quiet failure writes no Stuck row and no avoid");
    c.Check(approach.stuckRows == 1 && approach.avoided, "approach: a failure writes its Stuck row and the avoid");
}

bool StandWithoutReach(bool sameObject, float, float reach, float objectMovedYards, float botToPickYards)
{
    return !sameObject || objectMovedYards > reach || botToPickYards <= reach;
}

bool MarksAlways(bool) { return true; }

// The mutants: each breaks one rule
Decision EndBeforeRefusal(Facts const& facts, Bot& bot)
{
    if (!(facts.sameMap && facts.yards <= facts.reach) && bot.state.tripAsked && facts.place == bot.state.tripPlace)
        if (std::optional<StatusEnd::TripEnd> const end = bot.hold.TakeEnd(HOLDER))
        {
            bot.state.tripAsked = false;
            Decision decision;
            decision.step = Step::EndRead;
            decision.word = end->word;
            decision.outcome = end->outcome;
            decision.answer = end->outcome == Outcome::Arrived ? Answer::There : Answer::Failed;
            return decision;
        }
    return Real(facts, bot);
}

Decision EndReadTwice(Facts const& facts, Bot& bot)
{
    bool const asked = bot.state.tripAsked;
    Decision const decision = GoTo::Decide(
        facts, bot.state,
        [&]() -> std::optional<StatusEnd::TripEnd>
        {
            if (!bot.hold.Held() && bot.hold.LastEnd().word)
                return bot.hold.LastEnd();
            return std::nullopt;
        },
        [&]
        {
            ++bot.judgeCalls;
            return bot.carried;
        });
    if (decision.step == Step::EndRead)
        bot.state.tripAsked = asked;
    return decision;
}

Decision HolderEndLeftToRead(Facts const& facts, Bot& bot)
{
    return GoTo::Decide(
        facts, bot.state,
        [&]() -> std::optional<StatusEnd::TripEnd>
        {
            if (bot.hold.Held() || !bot.hold.LastEnd().word || bot.lastRead)
                return std::nullopt;
            bot.lastRead = true;
            return bot.hold.LastEnd();
        },
        [&]
        {
            ++bot.judgeCalls;
            return bot.carried;
        });
}

Decision OtherMapLocal(Facts const& facts, Bot& bot)
{
    Facts local = facts;
    if (!local.sameMap && local.yards <= local.localRange)
        local.sameMap = true;
    return Real(local, bot);
}

Decision RefusalMoves(Facts const& facts, Bot& bot)
{
    Decision decision = Real(facts, bot);
    if (decision.step == Step::Refused)
        decision.orderLocal = true;
    return decision;
}

Decision NotCarriedMoves(Facts const& facts, Bot& bot)
{
    Decision decision = Real(facts, bot);
    if (decision.step == Step::Trip && decision.askTrip && facts.sameMap && facts.yards <= facts.localRange)
        decision.orderLocal = true;
    return decision;
}

Decision QueryEveryTick(Facts const& facts, Bot& bot)
{
    bot.state.judged = false;
    return Real(facts, bot);
}

// An approach that asks a trip where its stand point is not carried
Decision ApproachAsksTrip(Facts const& facts, Bot& bot)
{
    Facts trip = facts;
    trip.tripAllowed = true;
    return Real(trip, bot);
}

// The old roam mask: NORMAL, INCOMPLETE or FARFROMPOLY, wherever the path ends
bool OldRoamMask(std::uint32_t pathType, float, float)
{
    return (pathType & (NORMAL | INCOMPLETE)) && !(pathType & ~(NORMAL | INCOMPLETE | FAR_END | 0x40u));
}

bool RoamWithoutReach(std::uint32_t pathType, float lastStep, float)
{
    return MeshMove::Classify(pathType, lastStep) == MeshMove::Class::Carried;
}

// The old far walk's mask on the follower's order: FARFROMPOLY accepted, a short local end taken
bool OldLegMask(std::uint32_t pathType, float, bool, float endToPlace, float yards, float)
{
    return (pathType & (NORMAL | INCOMPLETE)) && !(pathType & ~(NORMAL | INCOMPLETE | FAR_END | 0x40u)) &&
           endToPlace + GoTo::LEG_GAIN_YARDS < yards;
}

bool LegHopAlways(std::uint32_t pathType, float lastStep, bool, float endToPlace, float yards, float reach)
{
    return GoTo::LegCarried(pathType, lastStep, true, endToPlace, yards, reach);
}

bool CarriedWithoutReach(std::uint32_t pathType, float lastStep, float, float)
{
    return MeshMove::Classify(pathType, lastStep) == MeshMove::Class::Carried;
}

// The cause table: every word a trip's abort source or a local Stuck row writes has its outcome
void Causes(Counts& c)
{
    using TravelLegs::FailReason;
    for (int r = static_cast<int>(FailReason::Timeout); r <= static_cast<int>(FailReason::CrossingFailed); ++r)
    {
        FailReason const reason = static_cast<FailReason>(r);
        char const* const word = TravelLegs::AbortSource(reason);
        std::optional<Outcome> const known = GoTo::KnownOutcome(word);
        if (!known)
            std::printf("unlisted abort word: %s\n", word);
        c.Check(known && *known == TravelLegs::FailOutcome(reason),
                "causes: every trip abort word has the failed leg's outcome");
    }
    struct Stuck
    {
        WorldTravel::StuckCause cause;
        Outcome outcome;
    };
    constexpr Stuck STUCK[] = {
        {WorldTravel::StuckCause::OnTransport, Outcome::Stuck},
        {WorldTravel::StuckCause::FellOrSwimming, Outcome::Stuck},
        {WorldTravel::StuckCause::DoorOrElevator, Outcome::Stuck},
        {WorldTravel::StuckCause::UnreachablePoi, Outcome::Abandoned},
        {WorldTravel::StuckCause::NoPath, Outcome::Abandoned},
        {WorldTravel::StuckCause::NoProgress, Outcome::Stuck},
        {WorldTravel::StuckCause::OffMesh, Outcome::Stuck},
        {WorldTravel::StuckCause::Other, Outcome::Stuck},
    };
    for (Stuck const& row : STUCK)
    {
        char const* const word = WorldTravel::StuckCauseWord(row.cause);
        std::optional<Outcome> const known = GoTo::KnownOutcome(word);
        if (!known)
            std::printf("unlisted Stuck word: %s\n", word);
        c.Check(known && *known == row.outcome, "causes: every local Stuck word has its outcome");
    }
    for (std::uint32_t o = 0; o < StatusEnd::OUTCOME_COUNT; ++o)
    {
        Outcome const outcome = static_cast<Outcome>(o);
        c.Check(GoTo::OutcomeOf(StatusEnd::TripAbortSource(outcome)) == outcome &&
                    GoTo::OutcomeOf(StatusEnd::Word(outcome)) == outcome,
                "causes: an outcome's own word and abort source give the outcome back");
    }
    // INTERFACE 1, row by row
    struct Row
    {
        char const* word;
        Outcome outcome;
    };
    constexpr Row INTERFACE[] = {
        {"deadline", Outcome::TimedOut},
        {"timeout", Outcome::TimedOut},
        {"stalled", Outcome::Stuck},
        {"stuck", Outcome::Stuck},
        {"no_progress", Outcome::Stuck},
        {"door_or_elevator", Outcome::Stuck},
        {"off_mesh", Outcome::Stuck},
        {"stuck_aboard", Outcome::Stuck},
        {"on_transport", Outcome::Stuck},
        {"fell_or_swimming", Outcome::Stuck},
        {"no_path", Outcome::Abandoned},
        {"no_walk", Outcome::Abandoned},
        {"no_route", Outcome::Abandoned},
        {"unreachable_poi", Outcome::Abandoned},
        {"no_answer", Outcome::Abandoned},
        {"no_service", Outcome::Abandoned},
        {"death", Outcome::Abandoned},
        {"portal", Outcome::Abandoned},
        {"taxi", Outcome::Abandoned},
        {"fare", Outcome::Abandoned},
        {"landing", Outcome::Abandoned},
        {"hearth", Outcome::Abandoned},
        {"takeoff", Outcome::Abandoned},
        {"transport_lost", Outcome::Abandoned},
        {"no_exit", Outcome::Abandoned},
        {"no_crossing", Outcome::Abandoned},
        {"crossing_failed", Outcome::Abandoned},
        {"wrong_map", Outcome::Abandoned},
        {"not_bound", Outcome::Abandoned},
        {"held", Outcome::Interrupted},
        {"stopped", Outcome::Ordered},
        {"sick", Outcome::Sick},
    };
    for (Row const& row : INTERFACE)
    {
        std::optional<Outcome> const known = GoTo::KnownOutcome(row.word);
        if (!known || *known != row.outcome)
            std::printf("INTERFACE word off the table: %s\n", row.word);
        c.Check(known && *known == row.outcome, "causes: INTERFACE 1's words, row by row");
    }
    bool unique = true;
    for (std::size_t i = 0; i < std::size(GoTo::CAUSES); ++i)
        for (std::size_t j = i + 1; j < std::size(GoTo::CAUSES); ++j)
            unique = unique && GoTo::CAUSES[i].word != GoTo::CAUSES[j].word;
    c.Check(unique, "causes: each word once");
    c.Check(!GoTo::KnownOutcome("bogus") && GoTo::OutcomeOf("bogus") == Outcome::Abandoned &&
                GoTo::OutcomeOf(nullptr) == Outcome::Abandoned,
            "causes: an unknown word is a failure (Abandoned) and listed as unknown");
    c.Check(GoTo::OutcomeOf("abort:no_route") == GoTo::OutcomeOf("no_route"), "causes: the abort prefix is dropped");
}

// A trip asked for and not begun (another holder's trip is held) is an end with its word, at once: no wait without a
// bound, no move, and no end left to read; a begun trip and a tick that asked none are left as they are
using AfterBeginFn = GoTo::Decision (*)(GoTo::Decision, GoTo::State&, bool);

GoTo::Decision RefusedBeginGoesOn(GoTo::Decision decision, GoTo::State&, bool) { return decision; }

void BeginRefused(AfterBeginFn afterBegin, Counts& c)
{
    auto const noEnd = [] { return std::optional<StatusEnd::TripEnd>(); };
    auto const notCarried = [] { return false; };
    GoTo::Facts facts;
    facts.place = PLACE;
    facts.sameMap = false;
    facts.reach = REACH;
    GoTo::State state;
    GoTo::Decision const asked = GoTo::Decide(facts, state, noEnd, notCarried);
    c.Check(asked.step == Step::Trip && asked.askTrip && state.tripAsked, "begin: a place on another map asks a trip");
    GoTo::State begunState = state;
    GoTo::Decision const begun = afterBegin(asked, begunState, true);
    c.Check(begun.answer == GoTo::Answer::Going && begun.askTrip && begunState.tripAsked && !begun.word,
            "begin: a begun trip is going and its end is still to be read");
    GoTo::Decision const refused = afterBegin(asked, state, false);
    c.Check(refused.answer == GoTo::Answer::Failed && Word(refused, GoTo::BEGIN_REFUSED) &&
                refused.outcome == Outcome::Interrupted && NoMove(refused),
            "begin: a refused begin is an end with the word held (Interrupted); no trip asked, no move ordered");
    c.Check(!state.tripAsked, "begin: a refused begin leaves no end to read");
    int takes = 0;
    GoTo::Decision const again = GoTo::Decide(
        facts, state,
        [&]
        {
            ++takes;
            return std::optional<StatusEnd::TripEnd>();
        },
        notCarried);
    c.Check(takes == 0 && again.askTrip, "begin: the tick after a refused begin reads no end and asks again");
    GoTo::Decision local;
    local.step = Step::Local;
    local.answer = GoTo::Answer::Going;
    GoTo::State other;
    c.Check(afterBegin(local, other, false).answer == GoTo::Answer::Going, "begin: only an asked trip can be refused");
    c.Check(GoTo::KnownOutcome(GoTo::BEGIN_REFUSED).has_value(), "begin: the refusal's word is in the cause table");
}
}  // namespace

int main()
{
    Counts counts;
    Suite(Real, GoTo::Carried, counts);
    Approaches(Real, GoTo::Carried, counts);
    Roams(GoTo::RoamCarried, counts);
    Legs(GoTo::LegCarried, counts);
    Causes(counts);
    BeginRefused(GoTo::AfterBegin, counts);
    {
        Counts silent;
        silent.loud = false;
        BeginRefused(RefusedBeginGoesOn, silent);
        counts.Check(silent.failures > 0, "mutant killed: a refused begin still answers going");
    }

    struct Mutant
    {
        char const* name;
        DecideFn decide;
        CarriedFn carried;
    };
    Mutant const mutants[] = {
        {"mutant killed: steps 2 and 3 swapped (an end read before a refusal)", EndBeforeRefusal, GoTo::Carried},
        {"mutant killed: an end read twice", EndReadTwice, GoTo::Carried},
        {"mutant killed: the holder's own end left to read", HolderEndLeftToRead, GoTo::Carried},
        {"mutant killed: carried judged without the reach", Real, CarriedWithoutReach},
        {"mutant killed: a place on another map taken as a local piece", OtherMapLocal, GoTo::Carried},
        {"mutant killed: a refusal that still orders a move", RefusalMoves, GoTo::Carried},
        {"mutant killed: a near place not carried still orders a move", NotCarriedMoves, GoTo::Carried},
        {"mutant killed: the path query asked on every tick", QueryEveryTick, GoTo::Carried},
        {"mutant killed: an approach point not carried still ordered (a trip asked)", ApproachAsksTrip, GoTo::Carried},
    };
    for (Mutant const& mutant : mutants)
    {
        Counts silent;
        silent.loud = false;
        Suite(mutant.decide, mutant.carried, silent);
        Approaches(mutant.decide, mutant.carried, silent);
        counts.Check(silent.failures > 0, mutant.name);
    }
    {
        Counts silent;
        silent.loud = false;
        Approaches(Real, CarriedWithoutReach, silent);
        counts.Check(silent.failures > 0, "mutant killed: an approach takes the path's end beyond the reach");
    }
    struct RoamMutant
    {
        char const* name;
        RoamFn roam;
    };
    RoamMutant const roamMutants[] = {
        {"mutant killed: FARFROMPOLY accepted in the roam mask (a dropped sample ordered anyway)", OldRoamMask},
        {"mutant killed: a roam sample whose path ends short ordered anyway", RoamWithoutReach},
    };
    for (RoamMutant const& mutant : roamMutants)
    {
        Counts silent;
        silent.loud = false;
        Roams(mutant.roam, silent);
        counts.Check(silent.failures > 0, mutant.name);
    }
    struct LegMutant
    {
        char const* name;
        LegFn leg;
    };
    LegMutant const legMutants[] = {
        {"mutant killed: FARFROMPOLY accepted in the leg's order (the old far walk's mask)", OldLegMask},
        {"mutant killed: a hop taken where the link does not count it", LegHopAlways},
    };
    for (LegMutant const& mutant : legMutants)
    {
        Counts silent;
        silent.loud = false;
        Legs(mutant.leg, silent);
        counts.Check(silent.failures > 0, mutant.name);
    }

    SightStep(GoTo::RepickStand, GoTo::MarksFailure, counts);
    {
        Counts silent;
        silent.loud = false;
        SightStep(StandWithoutReach, GoTo::MarksFailure, silent);
        counts.Check(silent.failures > 0, "mutant killed: the stand point kept when the reach changes");
    }
    {
        Counts silent;
        silent.loud = false;
        SightStep(GoTo::RepickStand, MarksAlways, silent);
        counts.Check(silent.failures > 0, "mutant killed: the sight step's quiet flag off (a Stuck row and an avoid)");
    }

    std::printf("go-to-selftest: %d/%d passed\n", counts.checks - counts.failures, counts.checks);
    return counts.failures == 0 ? 0 : 1;
}
