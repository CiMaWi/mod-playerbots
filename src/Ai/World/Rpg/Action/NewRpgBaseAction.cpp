/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "BotTravel.h"
#include "BroadcastHelper.h"
#include "EventLog.h"
#include "ChatHelper.h"
#include "CheckMountStateAction.h"
#include "Creature.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GroundRules.h"
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

bool NewRpgBaseAction::GetToGround(Ground::Origin origin, uint32 tripId)
{
    Ground::Memo& memo = botAI->GetTravel().GroundMemo();
    uint32 const now = getMSTime();
    Ground::State state;
    state.skip = !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetTransport() ||
                 bot->GetVehicle() || bot->IsInWater() ||
                 bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_ONTRANSPORT);
    state.moving = !bot->movespline->Finalized();
    Ground::Gate const gate = Ground::GateOf(state, memo, origin, now);
    if (gate != Ground::Gate::Read)
        return gate == Ground::Gate::Wait;
    state.flyingMount = bot->IsMounted() && (bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura());
    state.flyingFlag = bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY);
    state.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    auto const [groundZ, waterZ] = CheckMountStateAction::FloorBelow(bot);
    state.groundZ = groundZ;
    state.waterZ = waterZ;
    Ground::Step const step = Ground::Decide(state, memo, origin, now, [this] { return !BotOffMesh(); });
    switch (step.act)
    {
        case Ground::Act::Descend:
            // Straight down on the flying mount, as a failed fly leg's descent (FlyDown; no path in the air)
            MoveTo(step.target.mapId, step.target.x, step.target.y, step.target.z, false, false, false, true,
                   MovementPriority::MOVEMENT_NORMAL, true);
            break;
        case Ground::Act::Fall:
            // The flight's leftovers go first (a dismount in the air can leave the flags); a ground mount is kept
            if (state.flyingMount || state.flyingFlag)
                CheckMountStateAction::LandAndDismount(bot);
            else
                CheckMountStateAction::FallToFloor(bot);
            break;
        case Ground::Act::Land:
            CheckMountStateAction::LandAndDismount(bot);
            break;
        default:
            break;
    }
    if (step.row)
    {
        std::string const reason =
            std::string(Ground::ActWord(step.act)) + ":" + Ground::OriginWord(origin);
        LOG_INFO("playerbots", "[Route] {} grounded ({}) {:.0f} yd above the floor in area {} at ({:.0f}, {:.0f}, {:.0f})",
                 bot->GetName(), reason, step.height, bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(),
                 bot->GetPositionZ());
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "grounded", reason, step.height, {}, tripId);
    }
    return Ground::Holds(step.act);
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
        // Nothing moved the bot: its spot goes to the record of failed attempts, so its next give-ups here are quiet
        // and no start or goal near it is taken while the entry lives (GiveUpWalk, GoTo's step 2)
        FailedAttempts::Failure failure;
        failure.spot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        failure.goal = failure.spot;
        botAI->GetTravel().NoteFailure("off_mesh", failure, tripId);
    }
    if (recovery != WorldTravel::OffMeshRecovery::Snap)
        return;

    // A last-resort teleport a few yards onto the mesh, unseen (the sight rule), counted in the ledger
    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->TeleportTo(bot->GetMapId(), x, y, z + 0.5f, bot->GetOrientation());
    EventLog::Honesty(bot, EventLog::HonestyKind::Teleport, "off_mesh_snap", bot->GetMapId());
}

WorldTravel::StuckCause NewRpgBaseAction::ClassifyGiveUp(WorldPosition const& dest)
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
    EventLog::Emit(bot, EventLog::EventType::Stuck, word, static_cast<uint32>(botAI->rpgInfo.GetStatus()));
    return cause;
}

WorldTravel::StuckCause NewRpgBaseAction::GiveUpWalk(WorldPosition const& dest)
{
    // At a live off_mesh spot a recovery did not take: one Stuck off_mesh per spot and expiry, so two in a row at one
    // spot mean that a recovery did not take. Off the mesh the destination is not to blame
    WorldTravel::SightPoint const here{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    if (FailedAttempts::IsPlaceRefused(botAI->GetTravel().Failures(), here, getMSTime()) == "off_mesh")
        return WorldTravel::StuckCause::OffMesh;
    WorldTravel::StuckCause const cause = ClassifyGiveUp(dest);
    if (cause == WorldTravel::StuckCause::OffMesh)
        RecoverOffMesh();
    return cause;
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    if (IsWaitingForLastMove(priority))
        return false;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    // A fresh distance each sample, so a bad roll (water, a wall, a point the mesh does not carry) is dropped and does
    // not lock the bot in place
    for (int attempt = 0; attempt < ROAM_SAMPLES; ++attempt)
    {
        float const distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float const angle = static_cast<float>(rand_norm()) * 2.0f * static_cast<float>(M_PI);
        if (OrderRoamSample(x + distance * std::cos(angle), y + distance * std::sin(angle), z, priority))
            return true;
    }
    return false;
}

bool NewRpgBaseAction::OrderRoamSample(float x, float y, float z, MovementPriority priority)
{
    Map* const map = bot->GetMap();
    if (!map->CanReachPositionAndGetValidCoords(bot, x, y, z))
        return false;
    if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
        return false;
    // The judge (WT64g): no end far from a polygon, no incomplete path that ends short of the sample
    PathGenerator path(bot);
    path.CalculatePath(x, y, z);
    G3D::Vector3 const& end = path.GetActualEndPosition();
    uint32 const type = path.GetPathType();
    float const lastStep = MeshMove::LastStep(path.GetPath());
    float const endYards = std::sqrt((end.x - x) * (end.x - x) + (end.y - y) * (end.y - y) + (end.z - z) * (end.z - z));
    if (!::GoTo::RoamCarried(type, lastStep, endYards))
        return false;
    return MoveCounted(bot->GetMapId(), x, y, z, MeshMove::Kind::Other, type, lastStep, false, priority);
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}
