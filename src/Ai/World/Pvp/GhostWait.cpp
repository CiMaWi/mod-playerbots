/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GhostWait.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <vector>

#include "CellImpl.h"
#include "CensusMgr.h"
#include "Corpse.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Timer.h"

namespace WorldPvp
{
namespace
{
PvpMemory& MemoryOf(PlayerbotAI* botAI)
{
    std::unique_ptr<PvpMemory>& memory = botAI->pvpMemory;
    if (!memory)
        memory = std::make_unique<PvpMemory>();
    return *memory;
}

// The yards from the corpse to the nearest alive enemy player within range, FAR_YARDS when there is none
float NearestEnemyYards(Player* bot, Corpse const* corpse, float range)
{
    std::vector<Player*> players;
    Acore::AnyPlayerInObjectRangeCheck check(corpse, range, true, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(corpse, players, check);
    Cell::VisitObjects(corpse, searcher, range);
    float nearest = FAR_YARDS;
    for (Player* const player : players)
    {
        if (player->GetTeamId() != bot->GetTeamId())
            nearest = std::min(nearest, corpse->GetExactDist(player));
    }
    return nearest;
}

class GhostWaitPlayerScript : public PlayerScript
{
public:
    GhostWaitPlayerScript() : PlayerScript("GhostWaitPlayerScript", {PLAYERHOOK_ON_PLAYER_RESURRECT}) {}

    // Any player's resurrection by any path (its corpse, a spirit healer, a spell, the console): the player's map
    // thread, or the world thread (a console command, an accepted spell), which never overlaps that map's update. A
    // bot's ghost wait ends with it
    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
    {
        PlayerbotAI* const botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        if (botAI && botAI->pvpMemory)
            GhostRevived(*botAI->pvpMemory);
    }
};
}  // namespace

bool IsPersistent(PlayerbotAI* botAI, uint32 nowMs)
{
    if (!botAI)
        return false;
    PvpMemory& memory = MemoryOf(botAI);
    if (PersistenceDue(memory, nowMs))
    {
        memory.persistent = PlayerbotFactory::KeepsPossessions(botAI->GetBot());
        memory.persistentKnown = true;
        memory.persistentCheckMs = nowMs;
    }
    return memory.persistent;
}

GhostPlan PlanGhostFor(PlayerbotAI* botAI, Corpse const* corpse)
{
    Player* const bot = botAI ? botAI->GetBot() : nullptr;
    Map const* const map = bot ? bot->GetMap() : nullptr;
    if (!bot || !corpse || !map || map->Instanceable() || bot->InBattleground() || botAI->HasGameClientMaster() ||
        corpse->GetMapId() != bot->GetMapId())
        return GhostPlan();
    uint32 const nowMs = getMSTime();
    if (!IsPersistent(botAI, nowMs))
        return GhostPlan();
    PvpMemory& memory = *botAI->pvpMemory;
    if (!GhostCheckDue(memory, nowMs))
        return GhostHeld(memory, nowMs);

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp ghost") : nullptr);
    GhostValues const values(sPlayerbotAIConfig.WorldPvp);
    time_t const now = time(nullptr);
    time_t const released = corpse->GetGhostTime();
    uint32 const ghostMs =
        now > released ? static_cast<uint32>(std::min<time_t>(now - released, DAY)) * IN_MILLISECONDS : 0;
    GhostTurn const turn =
        GhostChecked(memory, NearestEnemyYards(bot, corpse, values.enemyYards), ghostMs, nowMs, values);
    if (turn.row)
        EventLog::Emit(bot, EVENT_GHOST_WAIT, turn.row, 0, turn.amount);
    return turn.plan;
}
}  // namespace WorldPvp

void AddGhostWaitScripts() { new WorldPvp::GhostWaitPlayerScript(); }
