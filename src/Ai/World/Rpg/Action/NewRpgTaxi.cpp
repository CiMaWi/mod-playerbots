/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cmath>
#include <string>
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
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StartingHistoryRules.h"
#include "TravelLegRules.h"
#include "WaypointMovementGenerator.h"
#include "WorldLogStatus.h"
#include "WorldSession.h"

// PA72b trace (G393): the first of the core's checks in Player::ActivateTaxiPathTo (the flight master's case) that
// refuses the path, read right after the refusal; "core" where none does (the instant taxi's teleport answers false)
static std::string TaxiRefusal(Player* bot, Creature* master, std::vector<uint32> const& nodes)
{
    if (nodes.size() < 2)
        return "short";
    if (bot->GetSession()->IsLoggingOut())
        return "logout";
    if (bot->IsInCombat())
        return "combat";
    if (bot->HasUnitState(UNIT_STATE_STUNNED))
        return "stunned";
    if (bot->HasUnitState(UNIT_STATE_ROOT))
        return "rooted";
    if (bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return "disable_move";
    if (bot->IsMounted())
        return "mounted";
    if (bot->IsInDisallowedMountForm())
        return "form";
    if (bot->IsNonMeleeSpellCast(false))
    {
        uint32 spellId = 0;
        for (CurrentSpellTypes const type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
            if (Spell const* spell = bot->GetCurrentSpell(type); spell && !spellId)
                spellId = spell->m_spellInfo->Id;
        return "casting:" + std::to_string(spellId);
    }
    if (!sTaxiNodesStore.LookupEntry(nodes.front()))
        return "no_node";
    uint32 total = 0;
    for (std::size_t i = 1; i < nodes.size(); ++i)
    {
        uint32 path = 0;
        uint32 cost = 0;
        sObjectMgr->GetTaxiPath(nodes[i - 1], nodes[i], path, cost);
        if (!path)
            return "no_path:" + std::to_string(nodes[i - 1]) + "-" + std::to_string(nodes[i]);
        total += cost;
    }
    if (!sObjectMgr->GetTaxiMountDisplayId(nodes.front(), bot->GetTeamId(true)))
        return "no_mount";
    total = static_cast<uint32>(std::ceil(total * bot->GetReputationPriceDiscount(master)));
    if (bot->GetMoney() < total)
        return "fare:" + std::to_string(total);
    return "core";
}

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
        if (sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
            LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed: {}", bot->GetName(),
                      flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back(),
                      TaxiRefusal(bot, flightMaster, nodes));
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

bool NewRpgTravelAction::TalkFlightMaster(HeldTrip& data)
{
    TravelRoute::Leg const& leg = data.trip.legs[data.trip.legIdx];
    if (leg.taxiNodes.size() < 2)
        return LegFailed(data, TravelLegs::FailReason::Taxi);
    // The master of the leg's first taxi node nearest the leg's start that the bot sees (startup cache, read only;
    // scanned only on the few ticks the bot stands at the master)
    TravelMgr::FlightMasterInfo const* master = nullptr;
    float nearest = TravelLegs::FLIGHT_MASTER_YARDS * 2.0f;
    for (auto const& [dbGuid, info] : sTravelMgr.GetFlightMasters(bot->GetTeamId()))
    {
        if (info.taxiNodeId != leg.taxiNodes.front() || info.pos.GetMapId() != leg.from.mapId ||
            !sTravelMgr.SeesFlightMaster(bot, info))
            continue;
        float const yards = TravelRoute::Distance(
            {info.pos.GetMapId(), info.pos.GetPositionX(), info.pos.GetPositionY(), info.pos.GetPositionZ()}, leg.from);
        if (yards < nearest)
        {
            nearest = yards;
            master = &info;
        }
    }
    if (!master)
        return LegFailed(data, TravelLegs::FailReason::Taxi);

    auto const approach = [this](WorldPosition const& to)
    { return MoveFarTo(to) ? TaxiStart::Moving : TaxiStart::Stalled; };
    switch (StartTaxi(master->templateEntry, master->pos, leg.taxiNodes, approach))
    {
        case TaxiStart::Moving:
            return true;
        case TaxiStart::Stalled:
            return false;
        case TaxiStart::Started:
            data.trip.taxiActivated = true;
            return true;
        case TaxiStart::Refused:
        {
            // The core's fare differs from the snapshot's: a purse short of the chain's price fails as the fare
            uint32 cost = 0;
            for (size_t i = 0; i + 1 < leg.taxiNodes.size(); ++i)
            {
                uint32 path = 0;
                uint32 price = 0;
                sObjectMgr->GetTaxiPath(leg.taxiNodes[i], leg.taxiNodes[i + 1], path, price);
                cost += price;
            }
            return LegFailed(data,
                             bot->GetMoney() < cost ? TravelLegs::FailReason::Fare : TravelLegs::FailReason::Taxi);
        }
        case TaxiStart::NoMaster:
        case TaxiStart::Undiscovered:
            break;
    }
    return LegFailed(data, TravelLegs::FailReason::Taxi);
}
