/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <vector>

#include "CheckMountStateAction.h"
#include "Creature.h"
#include "DBCStores.h"
#include "EventLog.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MountRules.h"
#include "MoveSpline.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "ObjectDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"
#include "StartingHistoryRules.h"
#include "WaypointMovementGenerator.h"
#include "WorldLogStatus.h"
#include "WorldSession.h"

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (!bot->IsInFlight() && SearchQuestGiverAndAcceptOrReward())
        return true;

    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    // after landing, stop here and let the status update switch to idle
    if (data.inFlight)
        return false;

    TaxiStart const start = StartTaxi(data.flightMasterEntry, data.flightMasterPos, data.path);
    switch (start)
    {
        case TaxiStart::Moving:
        case TaxiStart::Started:
            return true;
        case TaxiStart::Stalled:
            return false;
        case TaxiStart::NoMaster:
        case TaxiStart::Undiscovered:
        case TaxiStart::Refused:
            break;
    }
    // PA68 trace (debug log only, once per ended flight): the start's word and what it read
    if (sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
    {
        uint32 known = 0;
        for (uint32 const node : data.path)
            known += bot->m_taxi.IsTaximaskNodeKnown(node) ? 1 : 0;
        Creature const* const anyMaster = bot->FindNearestCreature(data.flightMasterEntry, 100.0f, false);
        char const* const word = start == TaxiStart::NoMaster       ? "no_master"
                                 : start == TaxiStart::Undiscovered ? "undiscovered"
                                                                    : "refused";
        LOG_DEBUG("playerbots",
                  "[Route] pa68_taxi bot={} start={} master={} pos={:.0f},{:.0f},{:.0f} yards={:.1f} nodes={}-{} "
                  "count={} known={} purse={} persistent={} near_master={} alive={} near_yards={:.1f}",
                  bot->GetName(), word, data.flightMasterEntry, data.flightMasterPos.GetPositionX(),
                  data.flightMasterPos.GetPositionY(), data.flightMasterPos.GetPositionZ(),
                  bot->GetExactDist(data.flightMasterPos), data.path.empty() ? 0 : data.path.front(),
                  data.path.empty() ? 0 : data.path.back(), data.path.size(), known, bot->GetMoney(),
                  botAI->IsPersistentBot() ? 1 : 0, anyMaster ? 1 : 0, anyMaster && anyMaster->IsAlive() ? 1 : 0,
                  anyMaster ? bot->GetExactDist(anyMaster) : -1.0f);
    }
    RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = "failed"});
    return true;
}

NewRpgBaseAction::TaxiStart NewRpgBaseAction::StartTaxi(uint32 masterEntry, WorldPosition const& masterPos,
                                                        std::vector<uint32> const& nodes, TaxiApproach const& approach)
{
    // The master is looked for within this reach of its spawn; the last yards are the approach to its stand point
    // (pathing PA47), as for every other holder that walks to a creature
    float const searchYards = INTERACTION_DISTANCE * 3;
    if (bot->GetExactDist(masterPos) > searchYards)
        return approach ? approach(masterPos) : TaxiWalk(masterPos, searchYards);

    Creature* flightMaster = bot->FindNearestCreature(masterEntry, searchYards + INTERACTION_DISTANCE);
    if (!flightMaster || !flightMaster->IsAlive())
        return TaxiStart::NoMaster;
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
    {
        if (approach)
            return approach(WorldPosition(flightMaster));
        // A failed approach has ended the status with no_path (its Stuck row); the tick is spent
        return Approach(flightMaster->GetGUID(), INTERACTION_DISTANCE, true) == ::GoTo::Answer::Failed
                   ? TaxiStart::Stalled
                   : TaxiStart::Moving;
    }

    botAI->RemoveShapeshift();
    CheckMountStateAction::Dismount(bot, MountRules::How::Taxi);

    // Learning the master's own node is natural: a player learns it here too
    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    auto const known = [this](uint32 node) { return bot->m_taxi.IsTaximaskNodeKnown(node); };
    if (!StartingHistory::PathKnown(nodes, known))
    {
        // Persistent bots pick only discovered routes; other setups still fly and the flight is counted
        if (botAI->IsPersistentBot())
            return TaxiStart::Undiscovered;

        EventLog::Honesty(bot, EventLog::HonestyKind::Flight, "undiscovered_flight", nodes.empty() ? 0 : nodes.back());
    }

    // Pays the fare; fails when a node is not usable or the purse is short
    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        return TaxiStart::Refused;
    }
    return TaxiStart::Started;
}

void NewRpgBaseAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // A next node (none: the bot is at its destination) on another map
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})", bot->GetName(),
              nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}
