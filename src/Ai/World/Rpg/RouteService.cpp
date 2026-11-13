/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteService.h"

#include "BattlefieldMgr.h"
#include "BotTraitMgr.h"
#include "CheckMountStateAction.h"
#include "DBCStructure.h"
#include "FlightMap.h"
#include "Log.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "StartingHistoryRules.h"
#include "StringFormat.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "World.h"

namespace TravelRoute
{
namespace
{
constexpr uint32 SPELL_HEARTHSTONE = 8690;
constexpr float FLY_BASE_SPEED = 7.0f;  // yd/s: the core's base flight speed, a flying mount's bonus on top

static_assert(TAXI_MASK_SIZE == TaxiMaskSize, "the snapshot's taxi mask has PlayerTaxi's layout");

// On a worker: PerfMonitor rows record from any thread (one shard per thread)
void RecordPerf(std::uint64_t searchUs, std::uint64_t waitUs, std::uint32_t flightSearches, float flightMicros)
{
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "route search"))
        row->Record(searchUs);
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "route queue wait"))
        row->Record(waitUs);
    // One sample per flight search at the route's mean (the route keeps only their sum): its max is a route's mean
    if (flightSearches)
        if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "flight search"))
            for (std::uint32_t i = 0; i < flightSearches; ++i)
                row->Record(static_cast<std::uint64_t>(flightMicros / static_cast<float>(flightSearches)));
}
}  // namespace

RouteService& RouteService::Instance()
{
    static RouteService service(TravelGraph::Instance(), StuckBook::Instance());
    return service;
}

Snapshot FillSnapshot(Player const* bot, std::uint32_t requestId)
{
    Snapshot snapshot;
    snapshot.requestId = requestId;
    snapshot.takenMs = getMSTime();
    snapshot.position = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    snapshot.team = bot->GetTeamId() == TEAM_HORDE ? 1 : 0;
    snapshot.level = bot->GetLevel();
    snapshot.money = bot->GetMoney();
    for (uint32 node = 1; node <= TAXI_MASK_SIZE * 32; ++node)
        if (bot->m_taxi.IsTaximaskNodeKnown(node))
            snapshot.LearnTaxiNode(node);
    snapshot.hasHearthstone = bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1);
    snapshot.hearth = {bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ};
    snapshot.hearthCooldownMs = bot->GetSpellCooldownDelay(SPELL_HEARTHSTONE);
    snapshot.mounted = bot->IsMounted();
    // The bot's speed as it is (WT.23): a mounted bot's run speed has its mount in it; one on foot is planned on foot,
    // because travel walks are not proven to mount (the trip's walk_leg rows log mounted per leg)
    snapshot.runSpeed = bot->GetSpeed(MOVE_RUN);
    snapshot.swimSpeed = bot->GetSpeed(MOVE_SWIM);
    // Own flying (WU10): a flying mount and riding 225, on Northrend with Cold Weather Flying; never in an instance
    // or a battleground, nor in a battlefield's zone while its battle is on (Wintergrasp: not in the flight map)
    int32 const mountSpeed = CheckMountStateAction::FlyingMountSpeed(bot);
    if (mountSpeed > 0 && !bot->InBattleground() && !bot->GetMap()->Instanceable())
    {
        snapshot.flyOutland = CheckMountStateAction::RidingAllowsFlying(bot, FLY_MAP_OUTLAND);
        snapshot.flyNorthrend = CheckMountStateAction::RidingAllowsFlying(bot, FLY_MAP_NORTHREND);
        if (Battlefield const* battlefield = sBattlefieldMgr->GetBattlefieldToZoneId(bot->GetZoneId()))
            if (!battlefield->CanFlyIn())
                snapshot.flyOutland = snapshot.flyNorthrend = false;
        // Indoors, in water or in a WMO no-fly area (Dalaran's lie over flyable terrain): no fly leg from here
        snapshot.takeoffHere = !CheckMountStateAction::NoTakeoffHere(bot);
        bool const onFlyingMount = bot->IsMounted() && bot->HasIncreaseMountedFlightSpeedAura();
        snapshot.flySpeed = onFlyingMount ? bot->GetSpeed(MOVE_FLIGHT)
                                          : FLY_BASE_SPEED * (1.0f + static_cast<float>(mountSpeed) / 100.0f);
        std::shared_ptr<FlightRules::FlightMapSet const> const maps = Planner::FlightMaps();
        uint32 const guid = bot->GetGUID().GetCounter();
        snapshot.flightStyle =
            FlightRules::BotFlightStyle(BotTraitMgr::instance().Trait(guid, TraitRules::Trait::Caution), guid,
                                        maps ? maps->params : FlightRules::FlightParams{});
    }
    return snapshot;
}

Params ConfiguredParams()
{
    Params params;
    params.stuckPenalty = static_cast<float>(sPlayerbotAIConfig.WorldTravel.StuckPenalty);
    params.transportWaitSeconds = static_cast<float>(sPlayerbotAIConfig.WorldTravel.TransportWaitSeconds);
    float const flightSpeed = sWorld->getFloatConfig(CONFIG_TAXI_FLIGHT_SPEED);
    params.flightSpeed = flightSpeed > 0.0f ? flightSpeed : TravelGraph::FLIGHT_SPEED;
    // Null until the flight maps are built: no fly legs meanwhile
    params.flightMaps = Planner::FlightMapsReady() ? Planner::FlightMaps() : nullptr;
    return params;
}

void StartRouteService()
{
    RouteService::Instance().Start(sPlayerbotAIConfig.WorldTravel.RouteThreads, RecordPerf);
    LOG_INFO("server.loading", "Route service started: {} worker thread(s)", RouteService::Instance().Stats().threads);
}

std::string RouteServiceStatus()
{
    ServiceStats const stats = RouteService::Instance().Stats();
    std::size_t const flagged = StuckBook::Instance().Current()->flags.size();
    double const expanded =
        stats.searches ? static_cast<double>(stats.expanded) / static_cast<double>(stats.searches) : 0.0;
    return Acore::StringFormat(
        "ROUTE service: {} worker(s), {} requests ({} replaced, {} dropped, {} superseded), {} searches, {:.0f} "
        "expanded per search, search p50/p95 {}/{} us, queue wait p50/p95 {}/{} us, {} waiting, {} expired; {} stuck "
        "edges flagged",
        stats.threads, stats.requests, stats.replaced, stats.dropped, stats.superseded, stats.searches, expanded,
        stats.searchP50Us, stats.searchP95Us, stats.waitP50Us, stats.waitP95Us, stats.waiting, stats.expired, flagged);
}
}  // namespace TravelRoute
