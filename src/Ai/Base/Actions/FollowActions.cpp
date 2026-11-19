/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FollowActions.h"
#include "BotTravel.h"
#include "Event.h"
#include "Formations.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "MotionMaster.h"
#include "NewRpgInternal.h"
#include "MoveSpline.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Transport.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    Transport* GetTransportForPosTolerant(Map* map, WorldObject* ref, uint32 phaseMask, float x, float y, float z)
    {
        if (!map || !ref)
            return nullptr;

        std::array<float, 4> const probes = { z, z + 0.5f, z + 1.5f, z - 0.5f };
        for (float const pz : probes)
        {
            if (Transport* t = map->GetTransportForPos(phaseMask, x, y, pz, ref))
                return t;
        }

        return nullptr;
    }

    // Attempts to find a point on the leader's transport that is closer to the bot,
    // by probing along the segment from master -> bot and returning the last point
    // that is still detected as being on the expected transport.
    bool FindBoardingPointOnTransport(Map* map, Transport* expectedTransport, WorldObject* ref,
        float masterX, float masterY, float masterZ,
        float botX, float botY, float botZ,
        float& outX, float& outY, float& outZ)
    {
        if (!map || !expectedTransport || !ref)
            return false;

        uint32 const phaseMask = ref->GetPhaseMask();

        // Ensure master is actually detected on that transport (tolerant).
        if (GetTransportForPosTolerant(map, ref, phaseMask, masterX, masterY, masterZ) != expectedTransport)
            return false;

        // The raycast in GetTransportForPos starts at (z + 2). Probe with a safe Z.
        float const probeZ = std::max(masterZ, botZ);

        // Adaptive step count: small platforms need tighter sampling.
        float const dx2 = botX - masterX;
        float const dy2 = botY - masterY;
        float const dist2d = std::sqrt(dx2 * dx2 + dy2 * dy2);
        int32 const steps = std::clamp(static_cast<int32>(dist2d / 0.75f), 10, 28);

        float const dx = (botX - masterX) / static_cast<float>(steps);
        float const dy = (botY - masterY) / static_cast<float>(steps);

        // Master must actually be on the expected transport for this to work.
        if (map->GetTransportForPos(ref->GetPhaseMask(), masterX, masterY, probeZ, ref) != expectedTransport)
            return false;

        float lastX = masterX;
        float lastY = masterY;
        bool found = false;

        for (int32 i = 1; i <= steps; ++i)
        {
            float const px = masterX + dx * i;
            float const py = masterY + dy * i;

            Transport* const t = GetTransportForPosTolerant(map, ref, phaseMask, px, py, probeZ);
            if (t != expectedTransport)
                break;

            lastX = px;
            lastY = py;
            found = true;
        }

        if (!found)
            return false;

        outX = lastX;
        outY = lastY;
        outZ = masterZ; // keep deck-level Z to encourage stepping onto the platform/boat
        return true;
    }
}

bool FollowAction::Execute(Event /*event*/)
{
    Formation* formation = AI_VALUE(Formation*, "formation");
    std::string const target = formation->GetTargetName();

    // Transport handling for moving transports only (boats/zeppelins).
    Player* master = botAI->GetMaster();
    if (master && master->IsInWorld() && bot->IsInWorld() && bot->GetMapId() == master->GetMapId())
    {
        Map* map = master->GetMap();
        uint32 const mapId = bot->GetMapId();
        Transport* transport = nullptr;
        bool masterOnTransport = false;

        if (master->GetTransport())
        {
            transport = master->GetTransport();
            masterOnTransport = true;
        }
        else if (map)
        {
            transport = GetTransportForPosTolerant(map, master, master->GetPhaseMask(),
                master->GetPositionX(), master->GetPositionY(), master->GetPositionZ());
            masterOnTransport = (transport != nullptr);
        }

        // A vessel the crossing table knows (a lift's or the tram's car, a ship, a zeppelin): the bot boards and leaves
        // by the stored rows of the stop it stands at ("follow aboard", WT70d); the assist below is only for a vessel
        // the table lacks. Ignore static transports (elevators/trams) and known vessels: only keep unknown
        // boats/zeppelins here.
        if (transport && (transport->IsStaticTransport() || BotTravel::HasStopRecords(*transport)))
            transport = nullptr;

        if (transport && map && bot->GetTransport() != transport)
        {
            float const boardingAssistDistance = 60.0f;
            float const dist2d = ServerFacade::instance().GetDistance2d(bot, master);
            bool const inAssist = ServerFacade::instance().IsDistanceLessOrEqualThan(dist2d, boardingAssistDistance);

            if (inAssist)
            {
                float destX = masterOnTransport ? master->GetPositionX() : transport->GetPositionX();
                float destY = masterOnTransport ? master->GetPositionY() : transport->GetPositionY();
                float destZ = masterOnTransport ? master->GetPositionZ() : transport->GetPositionZ();
                float edgeX = 0.0f;
                float edgeY = 0.0f;
                float edgeZ = 0.0f;

                if (masterOnTransport &&
                    FindBoardingPointOnTransport(map, transport, master,
                        master->GetPositionX(), master->GetPositionY(), master->GetPositionZ(),
                        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                        edgeX, edgeY, edgeZ))
                {
                    destX = edgeX;
                    destY = edgeY;
                    destZ = edgeZ;
                }

                MovementPriority const priority = botAI->GetState() == BOT_STATE_COMBAT
                    ? MovementPriority::MOVEMENT_COMBAT
                    : MovementPriority::MOVEMENT_NORMAL;

                bool const movingAllowed = IsMovingAllowed();
                bool const dupMove = IsDuplicateMove(destX, destY, destZ);
                bool const waiting = IsWaitingForLastMove(priority);

                if (movingAllowed && !dupMove && !waiting)
                {
                    if (bot->IsSitState())
                        bot->SetStandState(UNIT_STAND_STATE_STAND);

                    bot->CastStop();

                    if (MotionMaster* mm = bot->GetMotionMaster())
                    {
                        mm->MovePoint(
                            /*id*/ 0,
                            /*coords*/ destX, destY, destZ,
                            /*forcedMovement*/ FORCED_MOVEMENT_NONE,
                            /*speed*/ 0.0f,
                            /*orientation*/ 0.0f,
                            /*generatePath*/ false,
                            /*forceDestination*/ false);
                    }
                    else
                        return false;

                    float delay = IN_MILLISECONDS * MoveDelay(bot->GetExactDist(destX, destY, destZ));
                    delay = std::clamp(delay, 0.0f, static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));

                    AI_VALUE(LastMovement&, "last movement")
                        .Set(mapId, destX, destY, destZ, bot->GetOrientation(), delay, priority);
                    ClearIdleState();
                    return true;
                }
            }
        }
    }
    // end unified transport handling

    bool moved = false;
    if (!target.empty())
    {
        moved = Follow(AI_VALUE(Unit*, target));
    }
    else
    {
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || loc.GetMapId() == MAPID_INVALID)
            return false;

        MovementPriority priority = botAI->GetState() == BOT_STATE_COMBAT ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_NORMAL;
        moved = MoveTo(loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ(), false, false, false,
                       true, priority, true);
    }

    // This section has been commented out because it was forcing the pet to
    // follow the bot on every "follow" action tick, overriding any attack or
    // stay commands that might have been issued by the player.
    // if (Pet* pet = bot->GetPet())
    // {
    //     botAI->PetFollow();
    // }
    // if (moved)
    // botAI->SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);

    return moved;
}

// The follower's step at its master's vessel (BotTravel::FollowAboard): a walk to the boarding row's dock point is
// this action's move; any other act the step issued itself or waits for. The walk under way keeps the action (the
// follow move waits for it); a move before it that has ended, or a lower one under way, holds it no longer. A walk
// whose move ended short of the dock point is walked again while it brings the bot nearer; one that did not, or one
// refused for lack of a path, gives the row up and the next row's walk follows in the same check (WT70d-b replay 6;
// TravelLegs::FollowWalkEnded). Moving not allowed or a vehicle seat refuse the walk as ever: false, no row spent
bool FollowAboardAction::Execute(Event /*event*/)
{
    BotTravel& travel = botAI->GetTravel();
    TravelLegs::FollowCrossing& state = travel.FollowState();
    // Each pass returns or spends a row, and a stop has a few rows
    for (uint32 pass = 0; pass < 8; ++pass)
    {
        Position dock;
        TravelLegs::FollowAct const act = travel.FollowAboard(dock);
        if (act != TravelLegs::FollowAct::Walk)
            return act != TravelLegs::FollowAct::None;
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        float const yards = bot->GetExactDist(dock);
        bool const moving = !bot->movespline->Finalized();
        bool const ours = state.walkMark && lastMove.msTime == state.walkMark;
        if (moving && ours)
            return true;
        if (moving && lastMove.priority > MovementPriority::MOVEMENT_NORMAL)
            return false;
        if (ours && !NewRpgInternal::FollowWalkEnded(bot, state, yards, false))
            continue;
        if (!IsMovingAllowed() || bot->GetVehicle())
            return false;
        lastMove.msTime = 0;
        lastMove.lastdelayTime = 0;
        if (MoveTo(bot->GetMapId(), dock.GetPositionX(), dock.GetPositionY(), dock.GetPositionZ()))
        {
            TravelLegs::FollowWalkIssued(state, yards, lastMove.msTime);
            return true;
        }
        NewRpgInternal::FollowWalkEnded(bot, state, yards, true);
    }
    return false;
}

bool FollowAboardAction::isUseful() { return botAI->GetTravel().FollowAboardDue(); }

bool FollowAction::isUseful()
{
    // move from group takes priority over follow as it's added and removed automatically
    // (without removing/adding follow)
    if (botAI->HasStrategy("move from group", BOT_STATE_COMBAT) ||
        botAI->HasStrategy("move from group", BOT_STATE_NON_COMBAT))
        return false;

    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
        return false;

    Formation* formation = AI_VALUE(Formation*, "formation");
    if (!formation)
        return false;

    std::string const target = formation->GetTargetName();

    Unit* fTarget = nullptr;
    if (!target.empty())
        fTarget = AI_VALUE(Unit*, target);
    else if (botAI->GetMaster())
        fTarget = botAI->GetMaster();
    else
        fTarget = AI_VALUE(Unit*, "group leader");

    if (fTarget)
    {
        if (fTarget->HasUnitState(UNIT_STATE_IN_FLIGHT))
            return false;

        if (!CanDeadFollow(fTarget))
            return false;

        if (fTarget->GetGUID() == bot->GetGUID())
            return false;
    }

    float distance = 0.f;
    if (!target.empty())
    {
        distance = AI_VALUE2(float, "distance", target);
    }
    else
    {
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || bot->GetMapId() != loc.GetMapId())
            return false;

        distance = bot->GetDistance(loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ());
    }
    if (botAI->HasStrategy("master fishing", BOT_STATE_NON_COMBAT))
        return ServerFacade::instance().IsDistanceGreaterThan(distance, sPlayerbotAIConfig.FishingDistanceFromMaster);

    return ServerFacade::instance().IsDistanceGreaterThan(distance, formation->GetMaxDistance());
}

bool FollowAction::CanDeadFollow(Unit* target)
{
    // In battleground, wait for spirit healer
    if (bot->InBattleground() && !bot->IsAlive())
        return false;

    // Move to corpse when dead and player is alive or not a ghost.
    if (!bot->IsAlive() && (target->IsAlive() || !target->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)))
        return false;

    return true;
}
