/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cmath>

#include "BotTravel.h"
#include "EventLog.h"
#include "GoToRules.h"
#include "Log.h"
#include "MeshMoveRules.h"
#include "NewRpgAction.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RpgMemos.h"
#include "TestConstraintStates.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "TravelProgress.h"
#include "WorldLog.h"
#include "WorldTravelRules.h"

// A status's walk to its place (WT64b; PLAN 3.20 (1)-(5), the rules in GoToRules.h). Every status that walks far
// calls GoTo(place, reach) each tick its executor runs: a local piece is ordered when the mesh carries it and watched
// by the one progress observer's walk rule, anything else is a trip the route follower steps, and a failed end is a
// typed end: the Stuck row with the cause word, then the status's end with that word's outcome. No refusal is repaired
// by another move, and nothing here names a status kind or a place: the holder passes its place and its reach.
// An approach (WT64g) is the same walk to an object's stand point at any distance, with no trip: a stand point the mesh
// does not carry ends no_path

namespace
{
// The local piece not served for this long (a fight, another action's turn): its clock does not count the gap and its
// move is judged again
constexpr uint32 LOCAL_GAP_MS = 10 * IN_MILLISECONDS;

WorldTravel::SightPoint SightOf(WorldPosition const& pos)
{
    return {pos.GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()};
}
}  // namespace

GoTo::Answer NewRpgBaseAction::FailGoTo(GoToWalk& walk, bool endsStatus, char const* word, StatusEnd::Outcome fallback)
{
    walk = GoToWalk{};
    walk.spent = true;
    // A visit's walk under the idle status has no status to end: the refusal (step 2) answers its next ticks
    if (endsStatus && botAI->rpgInfo.GetStatus() != RPG_IDLE)
        RpgStatus::End(botAI, word ? ::GoTo::OutcomeOf(word) : fallback, {.word = word});
    return ::GoTo::Answer::Failed;
}

// The memory is fresh with each status
void NewRpgBaseAction::FreshWalk(GoToWalk& walk)
{
    NewRpgInfo& info = botAI->rpgInfo;
    uint32 const status = static_cast<uint32>(info.GetStatus());
    if (walk.status == status && walk.statusStart == info.startT)
        return;
    walk = GoToWalk{};
    walk.status = status;
    walk.statusStart = info.startT;
}

GoTo::Answer NewRpgBaseAction::GoTo(WorldPosition const& place, float reach)
{
    if (!goTo)
        goTo = std::make_unique<GoToWalk>();
    return JudgedWalk(*goTo, place, place, reach, true, true);
}

GoTo::Answer NewRpgBaseAction::Approach(ObjectGuid guid, float distance, bool endsStatus, bool quiet)
{
    if (!approachWalk)
        approachWalk = std::make_unique<GoToWalk>();
    GoToWalk& walk = *approachWalk;
    walk.spent = false;
    WorldObject* const object = botAI->GetWorldObject(guid);
    if (!object || !object->IsInWorld() || object->GetMapId() != bot->GetMapId())
        return ::GoTo::Answer::Failed;
    FreshWalk(walk);
    // The stand point, once per object and reach (GoTo::RepickStand): the 45 degrees toward the bot (ahead of a moving
    // unit), up to `distance` out, where the collision check lets it stand
    if (::GoTo::RepickStand(walk.object == guid, walk.reach, distance, object->GetExactDist(walk.anchor),
                            bot->GetExactDist(walk.anchor)))
    {
        float const ox = object->GetPositionX();
        float const oy = object->GetPositionY();
        float const oz = object->GetPositionZ();
        Unit const* const unit = object->ToUnit();
        float const angle = (unit && unit->isMoving() ? object->GetOrientation() : object->GetAngle(bot)) +
                            static_cast<float>(M_PI * irand(-25, 25) / 100.0);
        float const rnd = static_cast<float>(rand_norm());
        float x = ox + std::cos(angle) * distance * rnd;
        float y = oy + std::sin(angle) * distance * rnd;
        float z = oz;
        // A point the collision check moved out of the reach in 3D (drawn within it in 2D, set on ground above or
        // below the object) is no stand point: its own path's end would be judged out of reach (WT64g-2)
        if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, ox, oy, oz, x, y, z) ||
            object->GetExactDist(x, y, z) > distance)
        {
            x = ox;
            y = oy;
            z = oz;
        }
        walk.object = guid;
        walk.anchor = WorldPosition(object);
        walk.point = WorldPosition(object->GetMapId(), x, y, z);
        walk.reach = distance;
    }
    WorldPosition const anchor = walk.anchor;
    WorldPosition const point = walk.point;
    return JudgedWalk(walk, anchor, point, distance, false, endsStatus, quiet);
}

GoTo::Answer NewRpgBaseAction::JudgedWalk(GoToWalk& walk, WorldPosition const& place, WorldPosition const& target,
                                          float reach, bool trips, bool endsStatus, bool quiet)
{
    using ::GoTo::Answer;
    using ::GoTo::Step;
    walk.spent = false;
    if (place == WorldPosition())
        return Answer::Going;

    BotTravel& travel = botAI->GetTravel();
    uint32 const status = static_cast<uint32>(botAI->rpgInfo.GetStatus());
    uint32 const now = getMSTime();
    ::GoTo::Place const at{place.GetMapId(), place.GetPositionX(), place.GetPositionY(), place.GetPositionZ()};
    // The memory is fresh with each status, the local piece's with each place
    FreshWalk(walk);
    if (!(walk.place == at))
    {
        walk.place = at;
        walk.progress = {};
        walk.ordered = false;
        walk.arrived = false;
    }
    walk.spent = true;

    ::GoTo::Facts facts;
    facts.place = at;
    facts.sameMap = place.GetMapId() == bot->GetMapId();
    facts.yards = facts.sameMap ? bot->GetExactDist(place) : 0.0f;
    facts.reach = reach;
    facts.localRange = trips ? ::GoTo::LOCAL_RANGE_YARDS : ::GoTo::APPROACH_RANGE_YARDS;
    facts.tripAllowed = trips;
    facts.tripHeld = trips && travel.Trip() != nullptr && travel.TripHolder() == status;
    if (!facts.tripHeld)
    {
        // A taxi or a vessel owns the bot's movement: no query, no request and no order until it is off
        if (bot->IsInFlight() || bot->GetTransport() || bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
            return Answer::Going;
        // Step 2, the record of failed attempts: a live place entry refuses the place with its cause, a live spot
        // entry where the bot stands (off_mesh, takeoff) refuses the start; no path query and no request. Not read
        // while the status's trip is under way: the follower's ends are its own, and the trip's end tells
        FailedAttempts::Record const& record = travel.Failures();
        std::string_view refused = FailedAttempts::IsPlaceRefused(record, SightOf(place), now);
        WorldTravel::SightPoint const here = SightOf(WorldPosition(bot));
        if (refused.empty() && FailedAttempts::RefusesStartAt(record, here, now))
            refused = FailedAttempts::IsPlaceRefused(record, here, now);
        if (!refused.empty())
            facts.refused = refused.data();  // the cause table's literal
    }

    // The move stamp changes when the bot's motion is no longer the local order given here (another action moved
    // it, the motion ended, the piece was not served for a while), never by that order itself
    auto const read = [&] { return travel.ReadTravelWalk(true); };
    // Whether or not a local order was under way: a fight between two passes is left out of the walk budget
    TravelProgress::SkipGap(walk.progress, now, walk.lastMs, LOCAL_GAP_MS);
    if (walk.ordered)
    {
        TravelProgress::Motion const motion = read();
        if (!motion.Active || !walk.progress.SameGenerator(motion))
        {
            walk.ordered = false;
            ++walk.moveSeq;
        }
    }
    facts.moveStamp = walk.moveSeq;

    // The judge: one path query, asked by step 4 alone and only for a new place or a changed move
    struct
    {
        float x, y, z;
        uint32 type;
        float lastStep;
    } order{};
    auto const judge = [&]
    {
        PathGenerator path(bot);
        path.CalculatePath(target.GetPositionX(), target.GetPositionY(), target.GetPositionZ());
        G3D::Vector3 const& end = path.GetActualEndPosition();
        order = {end.x, end.y, end.z, static_cast<uint32>(path.GetPathType()), MeshMove::LastStep(path.GetPath())};
        float const endYards = place.GetExactDist(end.x, end.y, end.z);
        bool const carried = ::GoTo::Carried(order.type, order.lastStep, endYards, reach);
        // The judge's refusal with its inputs (debug level), as the follower's "refused by the navmesh" line: the
        // place is near and a trip is asked instead, or the approach ends
        if (!carried)
            LOG_DEBUG("playerbots",
                      "[Route] {} {} to ({:.1f}, {:.1f}, {:.1f}) {:.0f} yd, reach {:.1f}: type 0x{:x}, last step "
                      "{:.1f} yd, end ({:.1f}, {:.1f}, {:.1f}) {:.1f} yd from the place: not carried",
                      bot->GetName(), trips ? "local piece" : "approach", place.GetPositionX(), place.GetPositionY(),
                      place.GetPositionZ(), facts.yards, reach, order.type, order.lastStep, end.x, end.y, end.z,
                      endYards);
        return carried;
    };
    ::GoTo::Decision const decision =
        ::GoTo::Decide(facts, walk.state, [&] { return RpgStatus::TakeTripEnd(botAI); }, judge);

    // The local piece's typed end in the record of failed attempts (the trip's own ends are noted by the follower)
    auto const noteLocal = [&](char const* word)
    {
        FailedAttempts::Failure failure;
        failure.leg = FailedAttempts::Leg::LocalPiece;
        failure.goal = SightOf(place);
        failure.spot = SightOf(WorldPosition(bot));
        travel.NoteFailure(word, failure, 0);
    };
    // A failed end that is read here and not by a give-up of the mover: its Stuck row and, for a local piece, its
    // entry in the record (step 2 refuses the place for a while); a quiet order's failure writes neither
    auto const failed = [&](char const* word, StatusEnd::Outcome fallback, bool local)
    {
        if (::GoTo::MarksFailure(quiet))
        {
            EventLog::Emit(bot, EventLog::EventType::Stuck, word ? word : StatusEnd::Word(fallback), status);
            if (local && word)
                noteLocal(word);
        }
        return FailGoTo(walk, endsStatus, word, fallback);
    };
    // walkflush (a test's constraint): a local piece's MeshMiss rows are written at its end
    auto const localEnded = [&]
    {
        if (walk.local)
            TravelOrders::FlushWalk(botAI);
        walk.local = false;
    };
    switch (decision.step)
    {
        case Step::There:
            walk.arrived = false;
            localEnded();
            return Answer::There;
        case Step::Refused:
            // No row: the refusal's Stuck row was written when the place was given up
            return FailGoTo(walk, endsStatus, decision.word, decision.outcome);
        case Step::EndRead:
            if (decision.answer == Answer::There)
            {
                walk.arrived = true;
                return Answer::There;
            }
            return failed(decision.word, decision.outcome, false);
        case Step::Local:
        {
            if (decision.answer == Answer::Failed)
            {
                // An approach whose stand point the mesh does not carry: a typed end, no trip and no other move
                localEnded();
                return failed(decision.word, decision.outcome, true);
            }
            // The one progress observer's walk rule (TravelProgressState.h): 90 s without 5 yd of advance on the
            // ordered move, sampled each second; the executor's ticks are its passes
            bool sampled = false;
            TravelProgress::Owner const owner =
                botAI->CanMove() ? TravelProgress::Owner::Walk : TravelProgress::Owner::Control;
            TravelProgress::MaintainWalk(walk.progress, {now, owner, false, 0}, read, sampled);
            walk.lastMs = now;
            if (walk.progress.TakeReason() == TravelProgress::Overdue::Walk)
            {
                // The mover's give-up: the Stuck row with its classified cause, the record's entry, the off-mesh
                // recovery (which notes its own spot); a quiet order's writes none
                WorldTravel::StuckCause cause = WorldTravel::StuckCause::Other;
                char const* const word = ::GoTo::GiveUpWord(quiet,
                                                            [&]
                                                            {
                                                                cause = GiveUpWalk(place);
                                                                return WorldTravel::StuckCauseWord(cause);
                                                            });
                if (word && cause != WorldTravel::StuckCause::OffMesh)
                    noteLocal(word);
                localEnded();
                return FailGoTo(walk, endsStatus, word, StatusEnd::Outcome::Stuck);
            }
            if (decision.orderLocal)
            {
                // Counted as kind other (WT71): only a carried path is ordered, so it adds no miss
                bool const moved = TravelProgress::ContinueWalk(
                    walk.progress, read,
                    [&]
                    {
                        return MoveCounted(bot->GetMapId(), order.x, order.y, order.z, MeshMove::Kind::Other,
                                           order.type, order.lastStep);
                    },
                    [](char const*) {});
                walk.ordered = moved && walk.progress.HasMotion();
                walk.local = walk.local || walk.ordered;
                if (!walk.ordered)
                    ++walk.moveSeq;  // not taken: judged again at the next tick
            }
            return Answer::Going;
        }
        case Step::Trip:
            if (facts.tripHeld)
            {
                // One stepper, the follower's action: an executor that runs ahead of it hands it the tick
                walk.spent = NewRpgTravelAction(botAI).Execute(Event());
                return Answer::Going;
            }
            if (decision.askTrip)
            {
                // A trip arrived here and the mesh does not carry the last yards: no second trip to the same place
                if (walk.arrived)
                    return failed("unreachable_poi", StatusEnd::Outcome::Abandoned, true);
                // Another holder's trip is held: an end with its word and no wait (GoToRules.h AfterBegin). No Stuck
                // row and no refusal of the place, which is not at fault
                ::GoTo::Decision const begun =
                    ::GoTo::AfterBegin(decision, walk.state, RpgStatus::BeginTrip(botAI, place));
                if (begun.answer == Answer::Failed)
                    return FailGoTo(walk, endsStatus, begun.word, begun.outcome);
            }
            return Answer::Going;
    }
    return Answer::Going;
}

NewRpgBaseAction::TaxiStart NewRpgBaseAction::TaxiWalk(WorldPosition const& to)
{
    return Away(to, INTERACTION_DISTANCE).value_or(true) ? TaxiStart::Moving : TaxiStart::Stalled;
}

std::optional<bool> NewRpgBaseAction::Away(WorldPosition const& place, float reach)
{
    if (GoTo(place, reach) == ::GoTo::Answer::There)
        return std::nullopt;
    return goTo->spent;
}
