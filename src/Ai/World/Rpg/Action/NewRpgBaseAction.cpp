/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "BroadcastHelper.h"
#include "CensusMgr.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "MapCollisionData.h"
#include "MoveSpline.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "QuestLevelRules.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "PathGenerator.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "RouteRules.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "TravelProgress.h"
#include "TravelWalkRules.h"
#include "Upkeep.h"
#include "UpkeepRules.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"
#include "WorldTravelRules.h"
#include "RpgNavMeshSnap.h"
#include "Spell.h"
#include "StartingHistoryRules.h"
#include "G3D/Vector2.h"

namespace
{
uint32 constexpr HEARTHSTONE_CAST_SPELL = 8690;  // the stone's spell (NewRpgRoute.cpp's HEARTHSTONE_SPELL)
}  // namespace

bool NewRpgBaseAction::BotOffMesh()
{
    if (bot->IsInWater() || bot->IsFlying() || bot->GetTransport() ||
        bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_ONTRANSPORT))
        return false;

    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    float z = bot->GetPositionZ();
    return NavMeshSnap::Nearest(bot->GetMap(), x, y, z, WorldTravel::OFF_MESH_ACROSS_YARDS,
                                WorldTravel::OFF_MESH_VERTICAL_YARDS) == NavMeshSnap::Result::None;
}

bool NewRpgBaseAction::CastHearth(char const*& refusal, float& castResult)
{
    refusal = nullptr;
    castResult = 0.0f;
    // The core refuses a mounted cast (SPELL_FAILED_NOT_MOUNTED) and a flight form one, where a player's client leaves
    // the mount or the form first; in the air that would be a fall, so only on the ground
    bool const flying = bot->IsFlying() || bot->IsInFlight();
    bool const flightForm = bot->GetShapeshiftForm() == FORM_FLIGHT || bot->GetShapeshiftForm() == FORM_FLIGHT_EPIC;
    if (!flying && !bot->IsInCombat())
    {
        if (bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        if (flightForm)
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
    }
    if (botAI->DoSpecificAction("hearthstone", Event(), true))
        return true;

    WorldTravel::HearthState state;
    state.alive = bot->IsAlive();
    state.stone = bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1, false);
    state.inCombat = bot->IsInCombat();
    state.battleground = bot->InBattleground();
    state.flying = flying;
    state.mounted = bot->IsMounted();
    state.flightForm = bot->GetShapeshiftForm() == FORM_FLIGHT || bot->GetShapeshiftForm() == FORM_FLIGHT_EPIC;
    state.moving = bot->isMoving();
    state.cooldown = bot->HasSpellCooldown(HEARTHSTONE_CAST_SPELL);
    refusal = WorldTravel::HearthRefusalWord(state);
    // Only for a refusal nothing above explains: the core's own check names it (as PlayerbotAI::CanCastSpell asks it)
    SpellInfo const* info = sSpellMgr->GetSpellInfo(HEARTHSTONE_CAST_SPELL);
    if (std::string_view(refusal) == "cast_failed" && info)
    {
        Spell* spell = new Spell(bot, info, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
        spell->m_targets.SetUnitTarget(bot);
        spell->m_CastItem = bot->GetItemByEntry(StartingHistory::ITEM_HEARTHSTONE);
        castResult = static_cast<float>(spell->CheckCast(true));
        delete spell;
    }
    return false;
}

void NewRpgBaseAction::RecoverOffMesh()
{
    auto const* travel = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    uint32 const tripId = travel ? travel->tripId : 0;
    // The hearthstone when it is ready: the stone's own spell, no bot teleport and no sight check
    char const* refusal = nullptr;
    float castResult = 0.0f;
    bool const hearthReady = TestConstraints::HearthAllowed(botAI->rpgInfo.constraints) &&
                             bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1, false) &&
                             !bot->HasSpellCooldown(HEARTHSTONE_CAST_SPELL) && !bot->IsInCombat() &&
                             !bot->IsNonMeleeSpellCast(false);
    bool const hearthCast = hearthReady && CastHearth(refusal, castResult);
    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    float z = bot->GetPositionZ();
    bool polyFound = false;
    bool seen = false;
    if (!hearthCast)
    {
        polyFound = NavMeshSnap::Nearest(bot->GetMap(), x, y, z, WorldTravel::OFF_MESH_SNAP_YARDS,
                                         WorldTravel::OFF_MESH_SNAP_YARDS) == NavMeshSnap::Result::Found;
        seen = polyFound && sRandomPlayerbotMgr.IsTeleportSeen(bot, WorldPosition(bot->GetMapId(), x, y, z));
    }
    WorldTravel::OffMeshRecovery const recovery = WorldTravel::OffMeshNext(hearthCast, polyFound, seen);
    char const* const word = WorldTravel::OffMeshRecoveryWord(recovery);
    float const yards = recovery == WorldTravel::OffMeshRecovery::Snap ? bot->GetExactDist(x, y, z) : 0.0f;
    LOG_DEBUG("playerbots", "[New RPG] {} off the mesh at ({}, {:.1f}, {:.1f}, {:.1f}): {} ({:.1f} yd{}{})",
              bot->GetName(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), word,
              yards, refusal ? ", hearth refused: " : "", refusal ? refusal : "");
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "off_mesh", word, yards, {}, tripId);
    if (WorldTravel::OffMeshStays(recovery))
    {
        // Nothing moved the bot: its next give-ups here are quiet for a while (WorldTravel::OffMeshBackoff)
        botAI->rpgInfo.offMeshPos = WorldPosition(bot);
        botAI->rpgInfo.offMeshTs = std::max<uint32>(getMSTime(), 1);
    }
    if (recovery != WorldTravel::OffMeshRecovery::Snap)
        return;

    // A last-resort teleport a few yards onto the mesh, unseen (the sight rule), counted in the ledger
    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->TeleportTo(bot->GetMapId(), x, y, z + 0.5f, bot->GetOrientation());
    EventLog::Honesty(bot, "Teleport", "off_mesh_snap", bot->GetMapId());
}

WorldTravel::StuckCause NewRpgBaseAction::RecordStuck(WorldPosition const& dest)
{
    WorldTravel::StuckInput input;
    input.OtherMap = dest.GetMapId() != bot->GetMapId();
    input.OnTransport = bot->GetTransport() != nullptr || bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    input.FallingOrSwimming =
        bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR) || bot->IsInWater();
    input.NearDoorOrElevator =
        bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_DOOR, WorldTravel::STUCK_DOOR_YARDS) ||
        bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_TRANSPORT, WorldTravel::STUCK_DOOR_YARDS);
    input.StartOffMesh = !input.OtherMap && !input.OnTransport && !input.FallingOrSwimming && BotOffMesh();
    if (!input.OtherMap && !input.OnTransport && !input.FallingOrSwimming && !input.StartOffMesh &&
        !input.NearDoorOrElevator)
    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType const type = path.GetPathType();
        input.PathComputed = true;
        input.NoPath = (type & PATHFIND_NOPATH) != 0;
        input.DestinationOffMesh = (type & PATHFIND_FARFROMPOLY_END) != 0;
        input.StartOffMesh = (type & PATHFIND_FARFROMPOLY_START) != 0;
    }

    WorldTravel::StuckCause const cause = WorldTravel::ClassifyStuck(input);
    char const* const word = WorldTravel::StuckCauseWord(cause);
    LOG_DEBUG("playerbots", "[New RPG] {} gives up ({},{},{},{}) at ({},{},{},{}), stuck: {}", bot->GetName(),
              dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetPositionX(),
              bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(), word);
    // id the RPG status; the row's x, y are the bot's position (the recorder's case holds the trail)
    EventLog::Emit(bot, "Stuck", word, static_cast<uint32>(botAI->rpgInfo.GetStatus()));
    // Plain data only: the world thread maps it to a travel graph edge the route search avoids. A route follower
    // names the edge it walked; other statuses give positions. A bot off the mesh is at fault itself, not the edge
    if (!input.OtherMap && cause != WorldTravel::StuckCause::OffMesh)
    {
        TravelRoute::StuckBook::Report report;
        report.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        report.destination = {dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()};
        if (auto const* travel = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data))
            std::tie(report.fromNode, report.toNode) = TravelLegs::StuckEdge(travel->trip);
        TravelRoute::StuckBook::Instance().Push(report);
    }
    return cause;
}

bool NewRpgBaseAction::GiveUpMove(WorldPosition const& dest)
{
    NewRpgInfo& info = botAI->rpgInfo;
    WorldTravel::SightPoint const here{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    WorldTravel::SightPoint const offMeshAt{info.offMeshPos.GetMapId(), info.offMeshPos.GetPositionX(),
                                            info.offMeshPos.GetPositionY(), info.offMeshPos.GetPositionZ()};
    bool const backingOff =
        WorldTravel::OffMeshBackoff(info.offMeshTs != 0, GetMSTimeDiffToNow(info.offMeshTs), offMeshAt, here);
    WorldTravel::StuckCause const cause = backingOff ? WorldTravel::StuckCause::OffMesh : RecordStuck(dest);
    bool const offMesh = cause == WorldTravel::StuckCause::OffMesh;
    if (offMesh)
    {
        if (!backingOff)
            RecoverOffMesh();
    }
    else
    {
        // A repeat give-up near the last one (still avoided) widens and lengthens the avoid (WorldTravelRules.h);
        // off the mesh the destination is not to blame
        WorldTravel::SightPoint const lastBot{info.stuckBotPos.GetMapId(), info.stuckBotPos.GetPositionX(),
                                              info.stuckBotPos.GetPositionY(), info.stuckBotPos.GetPositionZ()};
        info.stuckRepeats = WorldTravel::NextStuckRepeats(
            info.stuckAvoidTs != 0, GetMSTimeDiffToNow(info.stuckAvoidTs),
            sPlayerbotAIConfig.WorldTravel.StuckAvoidSeconds * IN_MILLISECONDS, info.stuckRepeats, lastBot, here);
        info.stuckAvoidPos = dest;
        info.stuckBotPos = WorldPosition(bot);
        info.stuckAvoidTs = getMSTime();
    }
    info.SetMoveFarTo(WorldPosition());
    return offMesh;
}

namespace
{
static_assert(TravelWalk::PATH_NOPATH == PATHFIND_NOPATH &&
                  TravelWalk::PATH_NOT_USING_PATH == PATHFIND_NOT_USING_PATH &&
                  TravelWalk::PATH_FARFROMPOLY_END == PATHFIND_FARFROMPOLY_END,
              "TravelWalk reads the engine's PathType bits");

// An order's engine answer as TravelWalk judges it: the type, the path's last step (a shortcut's whole line) and what
// its end gains on dest
TravelWalk::EngineAnswer ReadWalkAnswer(Player* bot, PathGenerator const& path, bool result, WorldPosition const& dest)
{
    TravelWalk::EngineAnswer answer;
    answer.result = result;
    answer.type = path.GetPathType();
    Movement::PointsArray const& points = path.GetPath();
    if (points.size() >= 2)
        answer.lastStep = (points[points.size() - 1] - points[points.size() - 2]).length();
    G3D::Vector3 const& end = path.GetActualEndPosition();
    answer.gain = bot->GetExactDist(dest) - dest.GetExactDist(end.x, end.y, end.z);
    return answer;
}
}  // namespace

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    auto* const travel = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!travel || !travel->HasWalkTarget())
        return MoveFarToImpl(dest);
    return TravelProgress::ContinueWalk(
        travel->progress, [&] { return botAI->ReadTravelWalk(); }, [&] { return MoveFarToImpl(dest); },
        [&](char const* reason) { botAI->TraceTravelInteraction("route_resume", reason); });
}

bool NewRpgBaseAction::MoveFarToImpl(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // The taxi or transport still owns movement even if the RPG goal changes. Consume the tick so a caller
    // cannot fall through to MoveRandomNear while the bot is aboard.
    if (bot->IsInFlight() || bot->GetTransport() || bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
        return true;

    bool newDest = dest != botAI->rpgInfo.moveFarPos;
    // A route follower's waypoints are graph nodes: the route search avoids stuck edges itself (StuckBook), so the
    // avoided-spot memo below would only end the trip early
    auto* const travel = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (newDest)
    {
        // A destination given up after a Stuck event is not walked to again for a while
        NewRpgInfo const& info = botAI->rpgInfo;
        WorldTravel::SightPoint const given{info.stuckAvoidPos.GetMapId(), info.stuckAvoidPos.GetPositionX(),
                                            info.stuckAvoidPos.GetPositionY(), info.stuckAvoidPos.GetPositionZ()};
        WorldTravel::SightPoint const wanted{dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(),
                                             dest.GetPositionZ()};
        if (!travel && WorldTravel::IsAvoidedSpot(info.stuckAvoidTs != 0, GetMSTimeDiffToNow(info.stuckAvoidTs),
                                                  sPlayerbotAIConfig.WorldTravel.StuckAvoidSeconds * IN_MILLISECONDS,
                                                  given, wanted, info.stuckRepeats))
        {
            WorldLog::StatusReason const reasonScope("stuck");
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }

        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
    }

    // performance optimization
    if (!newDest && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The travel observer samples owned splines even while
    // action delay or this gate prevents reissuing a move.
    //
    // A changed destination (e.g. the gather state switching to a
    // live node it passes, or a quest POI change) skips this gate:
    // letting the old spline finish would keep walking the bot
    // toward a target it no longer has.
    //
    // A finalized move spline is no move to wait for, whatever the
    // movement flags say: they can stay "moving" after the spline
    // ended (WU11b fix 13: Throke in the air, moving 71 s after its
    // last move), and the gate held such a bot forever.
    if (!newDest)
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && !bot->movespline->Finalized() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    float const disToDest = bot->GetDistance(dest);
    if (travel)
    {
        if (travel->progress.ShouldProbeOffMesh() && BotOffMesh())
        {
            travel->progress.Latch(TravelProgress::Overdue::OffMesh);
            return true;
        }
    }
    else if (disToDest + TravelProgress::ADVANCE_YARDS < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
    }
    else if ((++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime) ||
             (botAI->rpgInfo.stuckAttempts == WorldTravel::OFF_MESH_CHECK_ATTEMPT && BotOffMesh()))
    {
        GiveUpMove(dest);
        WorldLog::StatusReason const reasonScope("stuck");
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }

    // WT58: along a graph link each order is judged on the engine's answer before the move (TravelWalkRules.h); near
    // dest MovePoint would walk a "no path" answer as a straight line. Not taken: the follower's next tick orders the
    // far node (fallback) or fails the leg (refused). A refusal where the bot stands off the mesh goes to the off-mesh
    // recovery instead, as MoveFarTo's own probe does
    TravelWalk::LinkWalk* const walk = travel && travel->trip.walk.Active() ? &travel->trip.walk : nullptr;
    auto const taken = [&](PathGenerator const& path, bool result, bool direct)
    {
        TravelWalk::Verdict const verdict = TravelWalk::Judge(*walk, ReadWalkAnswer(bot, path, result, dest), direct);
        if (verdict == TravelWalk::Verdict::Refuse && BotOffMesh())
        {
            walk->mode = TravelWalk::Mode::Fallback;
            travel->progress.Latch(TravelProgress::Overdue::OffMesh);
        }
        return verdict == TravelWalk::Verdict::Walk || verdict == TravelWalk::Verdict::Hop;
    };

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        if (walk)
        {
            PathGenerator path(bot);
            bool const result = path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
            if (!taken(path, result, true))
                return true;
        }
        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                      false, true);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        PathGenerator path(bot);
        bool const result = path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (walk && !taken(path, result, false))
            return true;
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                return MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true);
            }
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        if (walk)
            TravelWalk::NoteMove(*walk);
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    return false;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // handful of times with a fresh distance each loop so a bad roll
    // doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}
