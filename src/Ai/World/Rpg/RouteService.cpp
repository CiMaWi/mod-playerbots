/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteService.h"

#include "DBCStructure.h"
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
constexpr uint32 RIDING_APPRENTICE = 75;   // a 60 % ground mount
constexpr uint32 RIDING_JOURNEYMAN = 150;  // a 100 % ground mount
constexpr float APPRENTICE_MOUNT_RATE = 1.6f;
constexpr float JOURNEYMAN_MOUNT_RATE = 2.0f;

static_assert(TAXI_MASK_SIZE == TaxiMaskSize, "the snapshot's taxi mask has PlayerTaxi's layout");

// On a worker: PerfMonitor rows record from any thread (one shard per thread)
void RecordPerf(std::uint64_t searchUs, std::uint64_t waitUs)
{
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "route search"))
        row->Record(searchUs);
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "route queue wait"))
        row->Record(waitUs);
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

    float runSpeed = bot->GetSpeed(MOVE_RUN);
    if (!snapshot.mounted)
    {
        // A bot that can ride mounts for a long way (CheckMountStateAction): plan with its ground mount's speed
        uint32 const riding = bot->GetPureSkillValue(SKILL_RIDING);
        if (riding >= RIDING_JOURNEYMAN)
            runSpeed = std::max(runSpeed, playerBaseMoveSpeed[MOVE_RUN] * JOURNEYMAN_MOUNT_RATE);
        else if (riding >= RIDING_APPRENTICE)
            runSpeed = std::max(runSpeed, playerBaseMoveSpeed[MOVE_RUN] * APPRENTICE_MOUNT_RATE);
    }
    snapshot.runSpeed = runSpeed;
    snapshot.swimSpeed = bot->GetSpeed(MOVE_SWIM);
    return snapshot;
}

Params ConfiguredParams()
{
    Params params;
    params.stuckPenalty = static_cast<float>(sPlayerbotAIConfig.WorldTravel.StuckPenalty);
    params.transportWaitSeconds = static_cast<float>(sPlayerbotAIConfig.WorldTravel.TransportWaitSeconds);
    float const flightSpeed = sWorld->getFloatConfig(CONFIG_TAXI_FLIGHT_SPEED);
    params.flightSpeed = flightSpeed > 0.0f ? flightSpeed : TravelGraph::FLIGHT_SPEED;
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
