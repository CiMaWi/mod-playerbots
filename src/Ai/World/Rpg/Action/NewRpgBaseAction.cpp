/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "BotTravel.h"
#include "BroadcastHelper.h"
#include "CensusMgr.h"
#include "ChatHelper.h"
#include "CheckMountStateAction.h"
#include "Creature.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "MapCollisionData.h"
#include "MountRules.h"
#include "MoveSpline.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
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
            CheckMountStateAction::Dismount(bot, MountRules::How::Hearth);
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
    auto const* travel = botAI->GetTravel().Trip();
    uint32 const tripId = travel ? travel->tripId : 0;
    // The hearthstone when it is ready: the stone's own spell, no bot teleport and no sight check
    char const* refusal = nullptr;
    float castResult = 0.0f;
    bool const hearthReady = TestConstraints::HearthAllowed(botAI->testConstraints) &&
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
        botAI->rpgMemos.offMeshPos = WorldPosition(bot);
        botAI->rpgMemos.offMeshTs = std::max<uint32>(getMSTime(), 1);
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
        if (auto const* travel = botAI->GetTravel().Trip())
            std::tie(report.fromNode, report.toNode) = TravelLegs::StuckEdge(travel->trip);
        TravelRoute::StuckBook::Instance().Push(report);
    }
    return cause;
}

WorldTravel::StuckCause NewRpgBaseAction::GiveUpMove(WorldPosition const& dest)
{
    RpgMemos const& memo = botAI->rpgMemos;
    WorldTravel::SightPoint const here{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    WorldTravel::SightPoint const offMeshAt{memo.offMeshPos.GetMapId(), memo.offMeshPos.GetPositionX(),
                                            memo.offMeshPos.GetPositionY(), memo.offMeshPos.GetPositionZ()};
    bool const backingOff =
        WorldTravel::OffMeshBackoff(memo.offMeshTs != 0, GetMSTimeDiffToNow(memo.offMeshTs), offMeshAt, here);
    WorldTravel::StuckCause const cause = backingOff ? WorldTravel::StuckCause::OffMesh : RecordStuck(dest);
    if (cause != WorldTravel::StuckCause::OffMesh)
        AvoidPlace(dest, WorldTravel::StuckCauseWord(cause));  // off the mesh the destination is not to blame
    else if (!backingOff)
        RecoverOffMesh();
    return cause;
}

void NewRpgBaseAction::AvoidPlace(WorldPosition const& place, char const* word)
{
    // A repeat give-up near the last one (still avoided) widens and lengthens the avoid (WorldTravelRules.h)
    RpgMemos& memo = botAI->rpgMemos;
    WorldTravel::SightPoint const here{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    WorldTravel::SightPoint const lastBot{memo.stuckBotPos.GetMapId(), memo.stuckBotPos.GetPositionX(),
                                          memo.stuckBotPos.GetPositionY(), memo.stuckBotPos.GetPositionZ()};
    memo.stuckRepeats = WorldTravel::NextStuckRepeats(
        memo.stuckAvoidTs != 0, GetMSTimeDiffToNow(memo.stuckAvoidTs),
        sPlayerbotAIConfig.WorldTravel.StuckAvoidSeconds * IN_MILLISECONDS, memo.stuckRepeats, lastBot, here);
    memo.stuckAvoidPos = place;
    memo.stuckBotPos = WorldPosition(bot);
    memo.stuckAvoidTs = getMSTime();
    memo.stuckAvoidWord = word;
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

        if (MoveCounted(bot->GetMapId(), dx, dy, dz, MeshMove::Kind::Other, path, false, priority))
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
