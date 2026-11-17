/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTravel.h"

#include "BattlegroundSA.h"
#include "BotTravelInternal.h"
#include "CensusMgr.h"
#include "CheckMountStateAction.h"
#include "GameObject.h"
#include "Log.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NaturalTrace.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PointMovementGenerator.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelProgress.h"
#include "WorldLog.h"

namespace
{
// A follower's intent (WT62, WT.48): its master's transport, a lift or the tram as well as a ship. Only the master's
// membership is read, and only on the bot's map; no surface is searched for it
uint64 MasterTransportGuid(Player const& bot, Player const* master)
{
    if (!master || master == &bot || !master->IsInWorld() || master->FindMap() != bot.FindMap())
        return 0;
    Transport const* const transport = master->GetTransport();
    return transport ? transport->GetGUID().GetRawValue() : 0;
}

// The one narrow rule for bots without a leg (WT62 Do 3): Strand of the Ancients places attackers above its two boats,
// static transports that a client reports standing on. A bot has no client: it means to board the nearer boat
uint64 BattlegroundBoatGuid(Player const& bot)
{
    BattlegroundMap* const map = bot.FindMap() ? bot.FindMap()->ToBattlegroundMap() : nullptr;
    Battleground* const bg = map ? map->GetBG() : nullptr;
    // The rolled type: a game from the random queue carries BATTLEGROUND_RB as its own
    if (!bg || bg->GetBgTypeID(true) != BATTLEGROUND_SA)
        return 0;
    GameObject const* nearer = bg->GetBGObject(BG_SA_BOAT_ONE);
    GameObject const* const other = bg->GetBGObject(BG_SA_BOAT_TWO);
    if (!nearer || (other && bot.GetExactDist2dSq(other) < bot.GetExactDist2dSq(nearer)))
        nearer = other;
    return nearer ? nearer->GetGUID().GetRawValue() : 0;
}
}  // namespace

std::optional<TravelTraceIdentity> TravelTraceFor(NewRpgInfo::Travel const* travel)
{
    if (!travel || travel->place.empty() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return std::nullopt;
    return TravelTraceIdentity{travel->tripId, travel->trip.legIdx, travel->trip.legStartMs};
}

void LogTravelGate(Player const& player, TravelTraceIdentity const& trace, uint32 nowMs, char const* gate)
{
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] gate bot={} trip={} leg={} generation={} tick_ms={} outcome={} combat={} control={} "
              "taxi={}",
              player.GetGUID().GetCounter(), trace.Trip, trace.Leg, trace.Generation, nowMs, gate, player.IsInCombat(),
              !player.HasUnitState(UNIT_STATE_LOST_CONTROL), player.IsInFlight());
}

char const* ContactName(TravelLifecycle::Contact contact)
{
    using TravelLifecycle::Contact;
    switch (contact)
    {
        case Contact::Unavailable:
            return "unavailable";
        case Contact::OffSurface:
            return "off_surface";
        case Contact::AboveDeck:
            return "above_deck";
        case Contact::Feet:
            return "feet";
    }
    return "unavailable";
}

char const* MotionStateName(TravelLifecycle::MotionState state)
{
    using TravelLifecycle::MotionState;
    switch (state)
    {
        case MotionState::None:
            return "none";
        case MotionState::Moving:
            return "moving";
        case MotionState::Reached:
            return "reached";
        case MotionState::Cancelled:
            return "cancelled";
    }
    return "none";
}

char const* PassengerOutcomeName(TravelLifecycle::Outcome outcome)
{
    using TravelLifecycle::Outcome;
    switch (outcome)
    {
        case Outcome::Pending:
            return "pending";
        case Outcome::Boarded:
            return "boarded";
        case Outcome::Exited:
            return "exited";
        case Outcome::Cancelled:
            return "cancelled";
        case Outcome::Lost:
            return "lost";
    }
    return "pending";
}

BotTravel::BotTravel(PlayerbotAI* botAI, Player* bot) : botAI(botAI), bot(bot) {}

bool BotTravel::ReconcileFollowPassenger(Transport const& expected)
{
    if (expected.IsStaticTransport())
        return false;

    PassengerAdapter adapter(*bot, std::nullopt);
    TravelLifecycle::Sample const sample = TravelLifecycle::Reconcile(
        adapter, nullptr, TravelLifecycle::MotionState::None, expected.GetGUID().GetRawValue());
    return !sample.Before.Guid && sample.After.Available && sample.After.Guid == expected.GetGUID().GetRawValue();
}

bool BotTravel::LootInteractionExpired() const
{
    return activeLootInteraction && activeLootInteraction->InteractionExpired();
}

bool BotTravel::ConsumeLootExpiry()
{
    return activeLootInteraction && bot->IsAlive() && !bot->IsInCombat() && botAI->CanMove() &&
           activeLootInteraction->ConsumeExpiry();
}

void BotTravel::CancelLootInteraction(std::string_view reason)
{
    if (activeLootInteraction)
        activeLootInteraction->Cancel(reason);
}

void BotTravel::TraceTravelInteraction(std::string_view event, std::string_view detail, ObjectGuid loot,
                                       uint32 interaction, std::string_view path, float yards,
                                       std::optional<std::pair<uint32, uint32>> leg)
{
    if (!NaturalTrace::IsSelected(bot) || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;

    auto const* travel = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    uint32 const trip = travel ? travel->tripId : 0;
    auto const [index, generation] = leg.value_or(
        travel && travel->CurrentLeg() ? std::pair{travel->trip.legIdx, travel->trip.legStartMs} : std::pair{0u, 0u});
    LOG_DEBUG("playerbots",
              "[TravelInteraction] bot={} tick_ms={} at_ms={} trip={} leg={} event={} loot={} interaction={} path={} "
              "detail={} mounted={} obligation={} generation={} request={} yards={:.2f}",
              bot->GetGUID().GetCounter(), getMSTime(), WorldLog::NowMs(), trip, index, event, loot.GetRawValue(),
              interaction, path, detail, uint32(bot->IsMounted()), trip, generation,
              travel ? travel->route.requestId : 0, yards);
}

std::string_view MountPathWord(BotTravel::MountPath path)
{
    return path == BotTravel::MountPath::Packet ? "packet" : "timer";
}

void BotTravel::TraceTravelMount(MountPath path, char const* veto)
{
    bool const accepted = !veto;
    if (path == MountPath::None || !NaturalTrace::IsSelected(bot))
        return;

    ObjectGuid const loot = activeLootInteraction ? activeLootInteraction->InteractionGuid() : bot->GetLootGUID();
    uint32 const generation = activeLootInteraction ? activeLootInteraction->InteractionGeneration() : 0;
    TraceTravelInteraction(accepted ? "mount_accept" : "mount_veto", accepted ? "issued" : veto, loot, generation,
                           MountPathWord(path));
    if (accepted)
    {
        tracedMount = path;
        tracedMountAt = getMSTime();
        botAI->AddWork(BotWork::MountTrace);
    }
}

TravelProgress::Motion BotTravel::ReadTravelWalk(bool checkPosition) const
{
    auto const& spline = *bot->movespline;
    auto* const master = bot->GetMotionMaster();
    auto const* point =
        master && !master->empty() ? dynamic_cast<PointMovementGenerator<Player>*>(master->top()) : nullptr;
    TravelProgress::Motion motion{spline.GetId(), !spline.Finalized(), 0.0f, false, 0};
    if (!point || !spline.Initialized() || spline.onTransport || spline.isFalling() || spline.isCyclic() ||
        bot->GetTransport() || bot->GetVehicle() || bot->IsInFlight())
        return motion;
    motion.Generator = reinterpret_cast<std::uintptr_t>(point);
    motion.Destination.mapId = bot->GetMapId();
    point->GetDestination(motion.Destination.x, motion.Destination.y, motion.Destination.z);
    motion.PositionMatches = true;
    if (checkPosition)
    {
        auto const position = spline.ComputePosition();
        float const tolerance = TravelProgress::SPLINE_POSITION_TOLERANCE_YARDS;
        motion.PositionMatches = bot->GetExactDistSq(position.x, position.y, position.z) <= tolerance * tolerance;
    }
    motion.Yards = spline.Velocity() * static_cast<float>(spline.timePassed()) / 1000.0f;
    return motion;
}

void BotTravel::ObserveTravelProgress(NewRpgInfo::Travel& travel, uint32 nowMs, bool revalidate, bool canMove)
{
    using TravelProgress::Owner;
    auto& progress = travel.progress;
    if (revalidate)
        progress.Suspend();

    auto const* leg = travel.CurrentLeg();
    bool const combat = bot->IsInCombat();
    // The flight down after a failed fly leg (TravelLegs::StartDescent) counts as riding: a trip never ends in the air
    bool const external =
        NewRpgInternal::PassengerOwnsTravel(*bot, travel) || travel.route.waiting || travel.trip.descending;
    Owner owner = Owner::Exempt;
    if (combat)
        owner = Owner::Combat;
    else if (bot->IsAlive() && !external)
    {
        if (!canMove)
            owner = Owner::Control;
        else if (HasLootInteraction())
            owner = Owner::Loot;
        else
            owner = !leg || leg->type == TravelRoute::LegType::Walk ? Owner::Walk : Owner::Leg;
    }
    auto const sample =
        TravelProgress::Maintain(progress, {nowMs, owner, combat, CompletedLootInteractions()}, travel.trip,
                                 travel.route, travel.routeSeconds, [&] { return ReadTravelWalk(true); });
    if (sample.Selected)
        TraceTravelInteraction("route_selected", travel.bindInn ? "inn" : "order");
    if (sample.CombatChanged)
        TraceTravelInteraction(combat ? "combat_begin" : "combat_end", "combat");
    if (sample.LootCompleted)
        TraceTravelInteraction("route_progress", "loot");
    if (sample.Yards > 0.0f)
        TraceTravelInteraction("route_progress", "distance", ObjectGuid::Empty, 0, "none", sample.Yards);
}

void BotTravel::BeginAboard(uint32 entry, uint64 guid)
{
    aboardEntry = entry;
    aboardGuid = guid;
    aboardSinceMs = getMSTime();
    passengerCadence.Revalidate = true;
    if (entry)
        botAI->AddWork(BotWork::Aboard);
}

void BotTravel::ForgetMountTrace()
{
    if (tracedMount == MountPath::None)
        return;

    tracedMount = MountPath::None;
    botAI->EndWork(BotWork::MountTrace);
}

void BotTravel::LogPreGateMisses(uint32 work) const
{
    if (aboardEntry && !(work & BotWork::Aboard))
        LOG_INFO("playerbots", "PreGate miss: Aboard {}", bot->GetName());

    if (activeLootInteraction && !(work & BotWork::Loot))
        LOG_INFO("playerbots", "PreGate miss: Loot {}", bot->GetName());

    if (tracedMount != MountPath::None && !(work & BotWork::MountTrace))
        LOG_INFO("playerbots", "PreGate miss: MountTrace {}", bot->GetName());
}

// A bot on a ship's, zeppelin's or lift's leg checks as often as its follower scans the transport (WT54-11): a stop
// lasts seconds, and at a rest area's 2-20 s, or a slow skill's stretch, bots waiting on Thunder Bluff's tower slept
// through the zeppelin's stop, riders through their far stop, and a scan pair never saw Kamagua's ship still. A bot's
// own short steps check at the loot's pace, one step a check: a loot interaction's (stop, dismount, request, the
// window, its close) inside LootInteraction::PhaseMs, and a trip's until its route is taken (the request, the
// answer). At a trip's 1-3 s times a slow skill's stretch a step took up to 10 s: the loot budget ran out before the
// window opened, and a walk began 36 s after its loot (WT44)
std::optional<uint32> BotTravel::PaceMs() const
{
    if (TravelKeepsOnFoot(botAI))
        return TravelLegs::TRANSPORT_SCAN_MS;
    auto const* const trip = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    bool const awaitsRoute = trip && (trip->trip.legs.empty() || trip->route.waiting);
    if ((botAI->GetWork() & BotWork::Loot) || awaitsRoute)
        return sPlayerbotAIConfig.LootDelay;
    return std::nullopt;
}

void BotTravel::NoteMeshMove(MeshMove::Kind kind, uint32 pathType, float lastStep, bool counted)
{
    if (!WorldLog::Active())
        return;
    if (MeshMove::Note(meshMoves, kind, MeshMove::Classify(pathType, lastStep), counted))
    {
        MeshMove::Spot& spot = meshMoves.last[static_cast<size_t>(kind)];
        spot = {bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY()};
    }
}

void BotTravel::PreGate(uint32 maintenanceMs, NewRpgInfo::Travel* travel,
                        std::optional<TravelTraceIdentity> const& traceTravel)
{
    // WT71: the moves the mesh did not carry, one MeshMiss row per kind with a miss (id = not carried, amount =
    // ordered)
    if (MeshMove::FlushDue(meshMoves, maintenanceMs))
        MeshMove::Flush(meshMoves, maintenanceMs,
                        [&](MeshMove::Kind kind, uint32 missed, uint32 ordered, MeshMove::Spot const& spot)
                        {
                            WorldLogFormat::Place place;
                            place.map = spot.map;
                            place.zone = spot.zone;
                            place.area = spot.area;
                            place.x = spot.x;
                            place.y = spot.y;
                            EventLog::EmitAt(bot->GetGUID().GetCounter(), place, MeshMove::EVENT_TYPE,
                                             MeshMove::KindWord(kind), missed, ordered);
                        });
    bool const revalidateProgress = passengerCadence.Revalidate;
    PassengerAdapter passenger(*bot, traceTravel);
    // Boarding has one owner (WT62, WT.48): a bot on no transport is probed only for the vessel it means to board
    uint64 const intent = TravelLifecycle::ChooseIntent(
        {(botAI->GetWork() & BotWork::Aboard) ? aboardGuid : 0, travel ? TravelLegs::BoardingIntent(travel->trip) : 0,
         MasterTransportGuid(*bot, botAI->GetMaster()), BattlegroundBoatGuid(*bot)});
    TravelLifecycle::Maintain(passengerCadence, maintenanceMs, bot->GetMapId(),
                              travel && travel->passengerMotion ? &*travel->passengerMotion : nullptr, passenger,
                              intent);
    // `route aboard` (BeginAboard): placed once the passenger check made the bot a passenger of that entry, failed
    // after the deadline; while it is pending the check probes at every pass, not at the passenger cadence. Travel's
    // registered steps read their bits on fresh loads: the passenger check or the loot watch can set a later one
    if (botAI->GetWork() & BotWork::Aboard)
    {
        Transport const* ridden = bot->GetTransport();
        bool const placed = ridden && ridden->GetEntry() == aboardEntry;
        uint32 const sinceMs = GetMSTimeDiffToNow(aboardSinceMs);
        if (placed || sinceMs > ABOARD_DEADLINE_MS)
        {
            LOG_INFO("playerbots", "[Route] {} aboard {}: {} after {} ms", bot->GetName(), aboardEntry,
                     placed ? "placed" : "failed", sinceMs);
            EventLog::Emit(bot, ABOARD_EVENT_TYPE, placed ? "placed" : "failed", aboardEntry, sinceMs);
            aboardEntry = 0;
            aboardGuid = 0;
            botAI->EndWork(BotWork::Aboard);
        }
        else
            passengerCadence.Revalidate = true;
    }
    if (botAI->GetWork() & BotWork::Dismount)
    {
        if (!bot->IsAlive() || bot->IsInFlight() || bot->GetVehicle() || bot->GetTransport())
            botAI->EndWork(BotWork::Dismount);
        else if (botAI->CanMove() && !bot->IsRooted() && (botAI->EndWork(BotWork::Dismount) & BotWork::Dismount))
            CheckMountStateAction::CompleteDismount(bot);
    }
    bool const loot = (botAI->GetWork() & BotWork::Loot) && activeLootInteraction;
    if (loot || travel)
    {
        PerfMonitorScope scope(
            travel && PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Travel::Progress") : nullptr);
        bool const canMove = botAI->CanMove();
        if (loot)
            activeLootInteraction->Observe(maintenanceMs, canMove);
        if (travel)
            ObserveTravelProgress(*travel, maintenanceMs, revalidateProgress, canMove);
    }
    if (botAI->GetWork() & BotWork::MountTrace)
    {
        if (bot->IsMounted())
        {
            TraceTravelInteraction("mount_state", "mounted", ObjectGuid::Empty, 0, MountPathWord(tracedMount));
            ForgetMountTrace();
        }
        else if (maintenanceMs - tracedMountAt >= LootInteraction::PhaseMs)
            ForgetMountTrace();
    }
}
