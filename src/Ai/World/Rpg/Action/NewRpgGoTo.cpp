/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <cmath>
#include <mutex>
#include <vector>

#include "BotTravel.h"
#include "CellImpl.h"
#include "DBCStores.h"
#include "EventLog.h"
#include "GameObject.h"
#include "GoToRules.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MeshMoveRules.h"
#include "NewRpgAction.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RpgMemos.h"
#include "RpgNavMeshSnap.h"
#include "StandRules.h"
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
// does not carry, or no stand point (pathing PA47: the stand rule's, StandRules.h), ends no_path

namespace
{
// The local piece not served for this long (a fight, another action's turn): its clock does not count the gap and its
// move is judged again
constexpr uint32 LOCAL_GAP_MS = 10 * IN_MILLISECONDS;

WorldTravel::SightPoint SightOf(WorldPosition const& pos)
{
    return {pos.GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()};
}

// The stand rule's target from the live creature or object (pathing PA47), as the offline check builds it from the
// stores (RouteCheckTargets.cpp StandTarget): a creature's combat reach, an object's type, scaled model box and world
// rotation, a spell focus's radius centre to centre; the caller's reach as its own check measures it
StandRules::Target StandTargetOf(Player* bot, WorldObject* object, float distance)
{
    float const x = object->GetPositionX();
    float const y = object->GetPositionY();
    float const z = object->GetPositionZ();
    float const facing = object->GetOrientation();
    if (Unit const* const unit = object->ToUnit())
        return StandRules::ForCreature(x, y, z, facing, unit->GetCombatReach(), bot->GetObjectSize(), distance);
    GameObject const* const go = object->ToGameObject();
    GameObjectTemplate const* const info = go ? go->GetGOInfo() : nullptr;
    if (!info || info->type == GAMEOBJECT_TYPE_SPELL_FOCUS)
        return StandRules::ForFocus(x, y, z, facing, distance);
    GameObjectDisplayInfoEntry const* const display = sGameObjectDisplayInfoStore.LookupEntry(info->displayId);
    float const scale = go->GetObjectScale();
    StandRules::Box box;
    if (display)
        box = {display->minX * scale, display->minY * scale, display->minZ * scale,
               display->maxX * scale, display->maxY * scale, display->maxZ * scale};
    G3D::Quat const& turn = go->GetWorldRotation();
    float const rotation[4]{turn.x, turn.y, turn.z, turn.w};
    return StandRules::ForObject(x, y, z, facing, info->type, display ? &box : nullptr, rotation, go->GetObjectSize(),
                                 bot->GetObjectSize(), distance);
}

// The stand points bots took in the last minute (tuning), by place (StandRules::CountsFor): an approach or a trip's end
// counts them taken beside the players standing there, so bots that set out together, or for neighbouring targets, do
// not pick one point. Approaches on several maps' threads share it
constexpr uint32 STAND_CLAIM_MS = 60 * IN_MILLISECONDS;

struct StandClaim
{
    ObjectGuid bot;
    uint32 mapId{0};
    uint32 instanceId{0};
    std::array<float, 3> at{};
    uint32 atMs{0};
};

std::mutex standClaimLock;
std::vector<StandClaim> standClaims;

// The players a target's occupants count: within StandRules::OccupantYards of its place
struct PlayerNearPlace
{
    StandRules::Target const& target;

    bool operator()(Player* player) const
    {
        return player->IsInWorld() &&
               StandRules::CountsFor(target, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    }
};
}  // namespace

namespace NewRpgInternal
{
std::vector<std::array<float, 3>> StandOccupants(Player* bot, StandRules::Target const& target, uint32 now)
{
    std::vector<std::array<float, 3>> occupants;
    std::vector<Player*> players;
    PlayerNearPlace check{target};
    Acore::PlayerListSearcher<PlayerNearPlace> searcher(bot, players, check);
    Cell::VisitObjects(target.x, target.y, bot->GetMap(), searcher, StandRules::OccupantYards(target));
    for (Player const* const player : players)
        if (player != bot)
            occupants.push_back({player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()});
    std::lock_guard<std::mutex> lock(standClaimLock);
    std::erase_if(standClaims, [&](StandClaim const& claim)
                  { return claim.bot == bot->GetGUID() || getMSTimeDiff(claim.atMs, now) > STAND_CLAIM_MS; });
    for (StandClaim const& claim : standClaims)
        if (claim.mapId == bot->GetMapId() && claim.instanceId == bot->GetInstanceId() &&
            StandRules::CountsFor(target, claim.at[0], claim.at[1], claim.at[2]))
            occupants.push_back(claim.at);
    return occupants;
}

void ClaimStand(Player* bot, float x, float y, float z, uint32 now)
{
    std::lock_guard<std::mutex> lock(standClaimLock);
    standClaims.push_back({bot->GetGUID(), bot->GetMapId(), bot->GetInstanceId(), {x, y, z}, now});
}
}  // namespace NewRpgInternal

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
    // The stand point (pathing PA.16, PA47), asked once per object and reach (GoTo::RepickStand): the rule's point
    // between the least and the greatest distance, on joined floor, in sight and in front first, spread by bot, apart
    // from the bots standing or headed there, and reached from the bot's floor (G350). No point: the approach ends with
    // no_path, its row names the rule's word. A point the bot's floor is known to lead to is walked to and arrived at
    // (StandRules::PointYards within ARRIVE_YARDS); where the mesh cannot tell (a taxi or a vessel carries the bot, no
    // flood: NavMeshSnap::ReachedFrom), the approach arrives within the band's reach of the target as before
    if (::GoTo::RepickStand(walk.object == guid, walk.reach, distance, object->GetExactDist(walk.anchor)))
    {
        StandRules::Target const target = StandTargetOf(bot, object, distance);
        uint32 const now = getMSTime();
        std::array<float, 3> const from{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        bool const carried =
            bot->IsInFlight() || bot->GetTransport() || bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        NavMeshSnap::Stand const stand = NavMeshSnap::StandPoint(
            object->GetMap(), target, object->GetPhaseMask(), NewRpgInternal::StandOccupants(bot, target, now),
            bot->GetGUID().GetRawValue(), carried ? nullptr : &from);
        bool const found = stand.result == NavMeshSnap::Result::Found;
        WorldLog::Decision(
            bot, WorldLogFormat::DecisionKind::Stand, StandRules::RowChoice(stand.answer),
            stand.result == NavMeshSnap::Result::Unknown ? "no_tile" : StandRules::RowReason(stand.answer),
            found ? stand.answer.yards : 0.0f,
            {{"least", target.minYards},
             {"reach", StandRules::CentreReach(target)},
             {"bot_yd", bot->GetExactDist(object)}},
            object->GetEntry());
        if (!found)
        {
            if (::GoTo::MarksFailure(quiet))
                EventLog::Emit(bot, EventLog::EventType::Stuck, "no_path",
                               static_cast<uint32>(botAI->rpgInfo.GetStatus()));
            return FailGoTo(walk, endsStatus, "no_path", StatusEnd::Outcome::Abandoned);
        }
        NewRpgInternal::ClaimStand(bot, stand.x, stand.y, stand.z, now);
        walk.object = guid;
        walk.anchor = WorldPosition(object);
        walk.point = WorldPosition(object->GetMapId(), stand.x, stand.y, stand.z);
        walk.reach = distance;
        walk.standReach = StandRules::CentreReach(target);
        walk.atPoint = stand.reachKnown;
    }
    WorldPosition const anchor = walk.anchor;
    WorldPosition const point = walk.point;
    return JudgedWalk(walk, anchor, point, walk.atPoint ? StandRules::ARRIVE_YARDS : walk.standReach, false, endsStatus,
                      quiet);
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
    // An approach that arrives at its stand point (atPoint, PA47 G350) measures the bot and the path's end to the
    // point, across the floor (StandRules::PointYards); else to the place, in 3D
    bool const atPoint = !trips && walk.atPoint;
    auto const yardsTo = [&](float x, float y, float z)
    {
        return atPoint ? StandRules::PointYards(x, y, z, target.GetPositionX(), target.GetPositionY(),
                                                target.GetPositionZ())
                       : place.GetExactDist(x, y, z);
    };
    facts.yards = facts.sameMap ? yardsTo(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) : 0.0f;
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

    // The judge: one path query, asked by step 4 alone and only for a new place or a changed move. An approach asks
    // the walk probe's wide search (pathing PA5: one of 1,024 nodes ran out on last yards the mesh joins) and orders
    // the furthest point of that path within the follower's lookahead (TravelWalk::OrderPoint), judged once more as the
    // point move's own path of 1,024 nodes (GoTo::OrderCarried); a local piece asks and orders as before
    struct
    {
        float x, y, z;
        uint32 type;
        float lastStep;
    } order{};
    auto const judge = [&]
    {
        bool const wide = TravelWalk::ApproachAsksWide(trips, bot->GetMap()->Instanceable());
        TravelWalk::ProbePoint const from{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        TravelWalk::ProbePoint const to{target.GetPositionX(), target.GetPositionY(), target.GetPositionZ()};
        // Count and time of the wide judges (pathing PA5), as Travel::WalkProbe's
        PerfMonitorScope scope(
            wide && PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Travel::ApproachJudge") : nullptr);
        TravelWalk::StepAnswer const answer = NewRpgInternal::AskTravelPath(bot, from, to, false, wide);
        TravelWalk::ProbePoint const end = answer.points.empty() ? from : answer.points.back();
        float const lastStep = MeshMove::LastStep(answer.points);
        float const endYards = yardsTo(end.x, end.y, end.z);
        bool carried = ::GoTo::Carried(answer.type, lastStep, endYards, reach);
        order = {end.x, end.y, end.z, answer.type, lastStep};
        // A local piece's move ends at the holder's reach (pathing PA47, TravelWalk::ReachCut): its approach takes the
        // last yards to its stand point
        if (carried && trips)
        {
            TravelWalk::ProbePoint const cut = TravelWalk::ReachCut(
                answer.points, {place.GetPositionX(), place.GetPositionY(), place.GetPositionZ()}, reach);
            order.x = cut.x;
            order.y = cut.y;
            order.z = cut.z;
        }
        // The point move's own path (wide only: a local piece's move asks the judged call again)
        char const* refusedBy = "judged path";
        bool byMove = false;
        if (carried && wide && answer.points.size() >= 2)
        {
            std::uint32_t const index = TravelWalk::OrderPoint(answer.points, TravelWalk::LOOKAHEAD_YARDS);
            TravelWalk::ProbePoint const point = answer.points[index];
            bool const toEnd = index + 1 == answer.points.size();
            PathGenerator move(bot);
            move.CalculatePath(point.x, point.y, point.z);
            G3D::Vector3 const& moved = move.GetActualEndPosition();
            order = {point.x, point.y, point.z, static_cast<uint32>(move.GetPathType()),
                     MeshMove::LastStep(move.GetPath())};
            carried = ::GoTo::OrderCarried(order.type, order.lastStep, toEnd, TravelWalk::Distance(moved, point),
                                           yardsTo(moved.x, moved.y, moved.z), reach);
            refusedBy = "point move's path";
            byMove = true;
        }
        // The judge's refusal with its inputs (debug level), as the follower's "refused by the navmesh" line: the
        // place is near and a trip is asked instead, or the approach ends; then the search's reason word (PA4's) and
        // the points count, which tell a search that ran out from one over the engine's point limit
        if (!carried)
        {
            // PA29: the refusal's reason in the world log, once per judge (the end in alt1/alt2, no probe steps)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip,
                               TravelWalk::RefusalChoice(trips ? TravelWalk::RefusalCaller::LocalPiece
                                                               : TravelWalk::RefusalCaller::Approach),
                               byMove ? TravelWalk::MovePathRefusalReason()
                                      : TravelWalk::RefusalReason(TravelWalk::Probe::PathType, answer.why),
                               TravelWalk::Distance(from, to), {{"to_x", to.x}, {"to_y", to.y}, {"steps", 0.0f}}, 0);
            LOG_DEBUG("playerbots",
                      "[Route] {} {} to ({:.1f}, {:.1f}, {:.1f}) {:.0f} yd, reach {:.1f}: type 0x{:x}, last step "
                      "{:.1f} yd, end ({:.1f}, {:.1f}, {:.1f}) {:.1f} yd from the place: not carried; {} search {}, {} "
                      "points; refused by the {} (type 0x{:x}, last step {:.1f} yd)",
                      bot->GetName(), trips ? "local piece" : "approach", place.GetPositionX(), place.GetPositionY(),
                      place.GetPositionZ(), facts.yards, reach, answer.type, lastStep, end.x, end.y, end.z, endYards,
                      wide ? "wide" : "plain", TravelWalk::WhyWord(answer.why), answer.points.size(), refusedBy,
                      order.type, order.lastStep);
        }
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

NewRpgBaseAction::TaxiStart NewRpgBaseAction::TaxiWalk(WorldPosition const& to, float reach)
{
    return Away(to, reach).value_or(true) ? TaxiStart::Moving : TaxiStart::Stalled;
}

std::optional<bool> NewRpgBaseAction::Away(WorldPosition const& place, float reach)
{
    if (GoTo(place, reach) == ::GoTo::Answer::There)
        return std::nullopt;
    return goTo->spent;
}
