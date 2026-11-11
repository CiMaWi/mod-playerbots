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
#include "Timer.h"
#include "TravelMgr.h"
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
uint32 constexpr GRIND_PICK_TRIES = 3;
uint32 constexpr HEARTHSTONE_CAST_SPELL = 8690;  // the stone's spell (NewRpgAction.cpp's HEARTHSTONE_SPELL)

// A grind spot is a 50 yd cell centre (TravelMgr rounds x, y and z), so it can sit off the ground or inside a rock.
// It is kept when the nav mesh has a poly a bot may walk (the bots' path filter) within 3 yd across and 50 yd up or
// down, and moves onto that poly; on a tile not loaded yet nothing can be judged and the spot stays as it is.
bool SnapToNavMesh(Player* bot, WorldPosition& spot)
{
    float x = spot.GetPositionX();
    float y = spot.GetPositionY();
    float z = spot.GetPositionZ();
    NavMeshSnap::Result const result = NavMeshSnap::Nearest(bot->GetMap(), x, y, z, 3.0f, 50.0f);
    if (result == NavMeshSnap::Result::Found)
        spot.Relocate(x, y, z);
    return result != NavMeshSnap::Result::None;
}
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
    bool const hearthReady = bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1, false) &&
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

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // Don't start a ground move while the bot is on a taxi: the MovePoint issued
    // below would fight the active flight spline. Returning true consumes the tick
    // instead of returning false, because the callers of MoveFarTo fall through to
    // MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight())
        return true;

    bool newDest = dest != botAI->rpgInfo.moveFarPos;
    // A route follower's waypoints are graph nodes: the route search avoids stuck edges itself (StuckBook), so the
    // avoided-spot memo below would only end the trip early
    bool const travelling = std::holds_alternative<NewRpgInfo::Travel>(botAI->rpgInfo.data);
    if (newDest)
    {
        // A destination given up after a Stuck event is not walked to again for a while
        NewRpgInfo const& info = botAI->rpgInfo;
        WorldTravel::SightPoint const given{info.stuckAvoidPos.GetMapId(), info.stuckAvoidPos.GetPositionX(),
                                            info.stuckAvoidPos.GetPositionY(), info.stuckAvoidPos.GetPositionZ()};
        WorldTravel::SightPoint const wanted{dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(),
                                             dest.GetPositionZ()};
        if (!travelling &&
            WorldTravel::IsAvoidedSpot(info.stuckAvoidTs != 0, GetMSTimeDiffToNow(info.stuckAvoidTs),
                                       sPlayerbotAIConfig.WorldTravel.StuckAvoidSeconds * IN_MILLISECONDS, given,
                                       wanted, info.stuckRepeats))
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
    // finish. The stuck counter below continues to track real
    // progress toward dest and gives the destination up if the
    // committed paths genuinely aren't closing the gap.
    //
    // A changed destination (e.g. the gather state switching to a
    // live node it passes, or a quest POI change) skips this gate:
    // letting the old spline finish would keep walking the bot
    // toward a target it no longer has.
    if (!newDest)
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // stuck check
    float disToDest = bot->GetDistance(dest);
    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never give up on the destination below.
    if (disToDest + 5.0f < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
    }
    else if ((++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime) ||
             (botAI->rpgInfo.stuckAttempts == WorldTravel::OFF_MESH_CHECK_ATTEMPT && BotOffMesh()))
    {
        // No meaningful progress toward dest for `stuckTime`, or none at all off the mesh: give the destination up,
        // record why and go IDLE. Still at the spot where a recovery left the bot off the mesh: give it up quietly
        // (no Stuck event, no recovery) until the back-off is over
        NewRpgInfo& info = botAI->rpgInfo;
        WorldTravel::SightPoint const here{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                           bot->GetPositionZ()};
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
        botAI->rpgInfo.SetMoveFarTo(WorldPosition());
        // A route follower stays in RPG_TRAVEL: its next tick fails the leg and asks for a new route (off the mesh:
        // the trip ends)
        if (auto* travel = std::get_if<NewRpgInfo::Travel>(&info.data))
        {
            travel->stuck = true;
            travel->offMesh = offMesh;
            return true;
        }
        WorldLog::StatusReason const reasonScope("stuck");
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
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
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
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

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            NewRpgHelpers::IsQuestWorthDoing(bot, quest) && NewRpgHelpers::IsQuestCapableDoing(bot, quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->RecordQuestAccepted(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->RecordQuestRewarded(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                uint32 const itemId = quest->RewardChoiceItemId[i];
                ItemTemplate const* rewardProto = sObjectMgr->GetItemTemplate(itemId);
                uint8 rewardSlot = NULL_SLOT;
                if (rewardProto)
                    rewardSlot = botAI->FindEquipSlot(rewardProto, NULL_SLOT, true);
                float score = calc.CalculateItem(itemId, 0, rewardSlot);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        bool const worthDoing = NewRpgHelpers::IsQuestWorthDoing(bot, quest);
        bool const capableDoing = NewRpgHelpers::IsQuestCapableDoing(bot, quest);
        bool const failed = bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED;
        if (!worthDoing || !capableDoing || failed)
        {
            QuestDropReason const reason = !worthDoing    ? QuestDropReason::NotWorth
                                           : !capableDoing ? QuestDropReason::NotCapable
                                                           : QuestDropReason::Failed;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        int32 const zoneOrSort = quest->GetZoneOrSort();
        if (zoneOrSort < 0 || (zoneOrSort > 0 && zoneOrSort != botZoneId))
        {
            QuestDropReason const reason = zoneOrSort < 0 ? QuestDropReason::ClassOrHoliday
                                                          : QuestDropReason::OtherZone;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->RecordQuestDropped(questId, QuestDropReason::LogFull);
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    ObjectGuid npcOrGo = AI_VALUE(ObjectGuid, "new rpg quest giver");
    if (!npcOrGo)
        return false;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
    if (!object)
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    if (info.questGiver != npcOrGo || GetMSTimeDiffToNow(info.questGiverLastTry) > questGiverRetryGap)
    {
        info.questGiver = npcOrGo;
        info.questGiverSince = getMSTime();
    }
    info.questGiverLastTry = getMSTime();

    // Unreachable, or an offer the interaction never clears: let the status action and grinding resume.
    if (GetMSTimeDiffToNow(info.questGiverSince) > questGiverGiveUpTime)
    {
        LOG_DEBUG("playerbots", "[New RPG] {} gives up on quest giver {}", bot->GetName(), npcOrGo.ToString());
        info.ignoredQuestGivers.insert(npcOrGo);
        info.questGiver.Clear();
        return false;
    }

    if (bot->CanInteractWithQuestGiver(object))
    {
        InteractWithNpcOrGameObjectForQuest(npcOrGo);
        ForceToWait(5000);
        return true;
    }
    return MoveWorldObjectTo(npcOrGo);
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    QuestPOIVector const* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
    {
        return false;
    }

    QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                QuestPOIPoint const& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        }

        if (poiInfo.empty())
            return false;

        return true;
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Get POIs to go
    for (QuestPOI const& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.size() == 0)
            continue;
        float dx = 0, dy = 0;
        std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
        for (size_t i = 0; i < qPoi.points.size(); i++)
        {
            QuestPOIPoint const& point = qPoi.points[i];
            dx += point.x * weights[i];
            dy += point.y * weights[i];
        }

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
    }

    if (poiInfo.size() == 0)
    {
        // LOG_DEBUG("playerbots", "[New rpg] {}: No available poi can be found for quest {}", bot->GetName(), questId);
        return false;
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    // At most GRIND_PICK_TRIES draws: a spot off the nav mesh is dropped and another drawn, else no spot
    WorldPosition dest{};
    uint32 rejected = 0;
    for (uint32 attempt = 0; attempt < GRIND_PICK_TRIES; ++attempt)
    {
        std::vector<WorldLocation>* pool = nullptr;
        if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
            pool = &hi_prepared_locs;
        else if (!lo_prepared_locs.empty())
            pool = &lo_prepared_locs;
        if (!pool)
            break;

        uint32 const idx = urand(0, pool->size() - 1);
        WorldPosition candidate = (*pool)[idx];
        if (SnapToNavMesh(bot, candidate))
        {
            dest = candidate;
            break;
        }

        ++rejected;
        (*pool)[idx] = pool->back();
        pool->pop_back();
    }
    if (rejected)
        LOG_DEBUG("playerbots", "[New RPG] Bot {} grind pos: {} spots off the nav mesh dropped", bot->GetName(),
                  rejected);
    LOG_DEBUG("playerbots",
              "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({} near, {} in range of {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

static_assert(RPG_STATUS_END == WorldLog::RPG_STATUS_WORD_COUNT && RPG_ERRANDS == 11 && RPG_TRAVEL == 10 &&
                  RPG_DO_GATHER == 9 && RPG_REST == 7,
              "WorldLog::RPG_STATUS_WORDS follows NewRpgStatus");
static_assert(uint32(RPG_GO_GRIND) == Upkeep::RPG_STATUS_GO_GRIND &&
                  uint32(RPG_WANDER_RANDOM) == Upkeep::RPG_STATUS_WANDER_RANDOM &&
                  uint32(RPG_DO_QUEST) == Upkeep::RPG_STATUS_DO_QUEST &&
                  uint32(RPG_OUTDOOR_PVP) == Upkeep::RPG_STATUS_OUTDOOR_PVP &&
                  uint32(RPG_DO_GATHER) == Upkeep::RPG_STATUS_DO_GATHER && RPG_STATUS_END <= 32,
              "UpkeepRules.h's fighting statuses follow NewRpgStatus");

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    WorldLog::StatusReason const reasonScope("pick");
    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    // A bot with resurrection sickness picks no status that fights (upkeep §3.5); with none left it rests
    bool const sick = bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0 || !Upkeep::StatusAllowed(status, sick))
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::RpgStatus, WorldLog::NewRpgStatusWord(RPG_REST),
                           "none_available", 0.0f);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    uint32 chosenWeight = 0;
    for (NewRpgStatus status : availableStatus)
    {
        accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (accumulate >= rand)
        {
            chosenStatus = status;
            chosenWeight = sPlayerbotAIConfig.RpgStatusProbWeight[status];
            break;
        }
    }
    // The world log's decision row weighs the chosen status against the three heaviest other available ones
    WorldLogFormat::Alternatives alternatives;
    if (WorldLog::Active())
        for (NewRpgStatus status : availableStatus)
            if (status != chosenStatus)
                alternatives.Offer({WorldLog::NewRpgStatusWord(status),
                                    static_cast<float>(sPlayerbotAIConfig.RpgStatusProbWeight[status])},
                                   false);

    auto const startStatus = [this](NewRpgStatus status) -> bool
    {
        switch (status)
        {
            case RPG_WANDER_RANDOM:
            {
                botAI->rpgInfo.ChangeToWanderRandom();
                return true;
            }
            case RPG_WANDER_NPC:
            {
                botAI->rpgInfo.ChangeToWanderNpc();
                return true;
            }
            case RPG_GO_GRIND:
            {
                WorldPosition pos = SelectRandomGrindPos(bot);
                if (pos != WorldPosition())
                {
                    botAI->rpgInfo.ChangeToGoGrind(pos);
                    return true;
                }
                return false;
            }
            case RPG_GO_CAMP:
            {
                WorldPosition pos = SelectRandomCampPos(bot);
                if (pos != WorldPosition())
                {
                    botAI->rpgInfo.ChangeToGoCamp(pos);
                    return true;
                }
                return false;
            }
            case RPG_DO_QUEST:
            {
                std::vector<uint32> availableQuests;
                for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                {
                    uint32 questId = bot->GetQuestSlotQuestId(slot);
                    if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                        continue;

                    std::vector<POIInfo> poiInfo;
                    if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                    {
                        availableQuests.push_back(questId);
                    }
                }
                if (availableQuests.size())
                {
                    uint32 questId = availableQuests[urand(0, availableQuests.size() - 1)];
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (quest)
                    {
                        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
                        return true;
                    }
                }
                return false;
            }
            case RPG_TRAVEL_FLIGHT:
            {
                uint32 flightMasterEntry = 0;
                WorldPosition flightMasterPos;
                std::vector<uint32> path;
                if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
                {
                    botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                    return true;
                }
                return false;
            }
            case RPG_DO_GATHER:
            {
                botAI->rpgInfo.ChangeToDoGather();
                return true;
            }
            case RPG_IDLE:
            {
                botAI->rpgInfo.ChangeToIdle();
                return true;
            }
            case RPG_REST:
            {
                botAI->rpgInfo.ChangeToRest();
                bot->SetStandState(UNIT_STAND_STATE_SIT);
                return true;
            }
            case RPG_OUTDOOR_PVP:
            {
                botAI->rpgInfo.ChangeToOutdoorPvp();
                return true;
            }
            default:
            {
                botAI->rpgInfo.ChangeToRest();
                bot->SetStandState(UNIT_STAND_STATE_SIT);
                return true;
            }
        }
        return false;
    };
    bool const started = startStatus(chosenStatus);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::RpgStatus, WorldLog::NewRpgStatusWord(chosenStatus),
                       started ? "weights" : "no_target", static_cast<float>(chosenWeight), alternatives);
    return started;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
                return false;

            // No point farming with (nearly) full bags - the loot pipeline
            // can't store the harvest (see StoreLootAction).
            if (AI_VALUE(uint8, "bag space") > 80)
                return false;

            return sGatherNodeMgr.HasUsableNodes(bot);
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}
