/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <memory>
#include <vector>

#include "BankVisit.h"
#include "DBCStores.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"

// The banker's spot for a bank visit (BankVisit.h): the planner's service index entries of the bot's map, the
// nearest one whose faction serves the bot. The visit's steps are in BankVisit.cpp.
namespace BankMail
{
namespace
{
// The nearest banker spawns on the map looked through for one whose faction serves the bot
constexpr size_t BANKER_CANDIDATES = 32;
}  // namespace

std::optional<BankerSpot> FindBankerSpot(Player* bot, char const*& refusal)
{
    refusal = nullptr;
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
    {
        refusal = "the planner's indexes are not ready";
        return std::nullopt;
    }

    std::vector<Planner::Nearby> const nearest =
        indexes->Nearest(Planner::ServiceKind::Banker, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                         bot->GetPositionZ(), BANKER_CANDIDATES);
    if (nearest.empty())
    {
        refusal = "no banker on the map";
        return std::nullopt;
    }

    for (Planner::Nearby const& candidate : nearest)
    {
        Planner::Service const& service = indexes->services[candidate.index];
        if (!BankerServes(bot, service.factionTemplate))
            continue;
        return BankerSpot{service.mapId, service.x, service.y, service.z, service.entry, candidate.distance};
    }
    refusal = "no banker near the bot serves its faction";
    return std::nullopt;
}

bool BankerServes(Player const* bot, uint32 factionTemplate)
{
    // The banker's reaction must be above Unfriendly
    return bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(factionTemplate), bot) > REP_UNFRIENDLY;
}

std::optional<BankerSpot> BankerSpotNear(Player* bot, uint32 mapId, float x, float y, float z, float yards)
{
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
        return std::nullopt;

    for (Planner::Nearby const& candidate :
         indexes->Nearest(Planner::ServiceKind::Banker, mapId, x, y, z, BANKER_CANDIDATES, yards))
    {
        Planner::Service const& service = indexes->services[candidate.index];
        if (BankerServes(bot, service.factionTemplate))
            return BankerSpot{service.mapId, service.x,     service.y,
                              service.z,     service.entry, bot->GetExactDist(service.x, service.y, service.z)};
    }
    return std::nullopt;
}
}  // namespace BankMail
