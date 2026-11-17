/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GHOSTWAIT_H
#define _PLAYERBOT_GHOSTWAIT_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "WorldPvpConfig.h"

class Corpse;
class Player;
class PlayerbotAI;

// The ghost's wait (WP7, no camping, the victim's side): a persistent bot's ghost in the open world stays dead while an
// enemy player stands at its corpse, runs to it once the enemy has gone and takes the spirit healer after its wait,
// as a player would. Also the bot's real master as world PvP's rules read it (RealMasterOf). The rules are
// player-free, so tests/ghost-wait-selftest.cpp checks the same code the server runs; GhostWait.cpp is the glue
namespace WorldPvp
{
// What world PvP keeps of a bot besides its enthusiasm (PlayerbotAI::pvpMemory, lazily; not saved): its ghost's
// wait. ghostSinceMs is when the wait began, 0 once the spirit healer was chosen
struct PvpMemory
{
    bool ghostWaiting = false;
    uint32_t ghostSinceMs = 0;
    uint32_t ghostNextCheckMs = 0;
};

// The wait's numbers (WorldPvpConfig's Ghost* options) in milliseconds; the default is the config's default
struct GhostValues
{
    float enemyYards = 0.0f;
    uint32_t recheckMs = 0;
    uint32_t spiritHealerMs = 0;

    explicit GhostValues(WorldPvpConfig const& config = WorldPvpConfig{})
        : enemyYards(static_cast<float>(config.GhostEnemyYards)),
          recheckMs(std::max<uint32_t>(config.GhostRecheckSeconds, 1) * 1000),
          spiritHealerMs(config.GhostSpiritHealerSeconds * 1000)
    {
    }
};

enum class GhostStep : uint8_t
{
    Wait,
    Corpse,
    SpiritHealer
};

struct GhostPlan
{
    GhostStep step = GhostStep::Corpse;
    // While waiting: when to check again
    uint32_t recheckMs = 0;
};

// A ghost waits while an enemy player is within GhostEnemyYards of its corpse, runs to it once the enemy has gone,
// and takes the spirit healer after GhostSpiritHealerSeconds of waiting, as a player would
inline GhostPlan PlanGhost(float enemyYards, uint32_t ghostMs, GhostValues const& values)
{
    GhostPlan plan;
    if (enemyYards > values.enemyYards)
        return plan;
    if (ghostMs >= values.spiritHealerMs)
    {
        plan.step = GhostStep::SpiritHealer;
        return plan;
    }
    plan.step = GhostStep::Wait;
    plan.recheckMs = std::min(values.recheckMs, values.spiritHealerMs - ghostMs);
    return plan;
}

// The GhostWait rows (world-log.md)
constexpr char const* EVENT_GHOST_WAIT = "GhostWait";
constexpr char const* GHOST_ENEMY = "enemy";
constexpr char const* GHOST_GONE = "gone";
constexpr char const* GHOST_HEALER = "healer";
// A ghost on its way to the spirit healer checks no more (its resurrection clears the wait)
constexpr uint32_t GHOST_HEALER_HOLD_MS = 60 * 60 * 1000;

// A check's outcome: the step, and the GhostWait row to write (source nullptr for none)
struct GhostTurn
{
    GhostPlan plan;
    char const* row = nullptr;
    int64_t amount = 0;
};

// Whether the ghost checks its corpse now; between checks the last answer holds (GhostHeld)
inline bool GhostCheckDue(PvpMemory const& memory, uint32_t nowMs)
{
    return !memory.ghostNextCheckMs || static_cast<int32_t>(nowMs - memory.ghostNextCheckMs) >= 0;
}

// The last check's answer
inline GhostPlan GhostHeld(PvpMemory const& memory, uint32_t nowMs)
{
    GhostPlan plan;
    if (memory.ghostWaiting)
    {
        plan.step = memory.ghostSinceMs ? GhostStep::Wait : GhostStep::SpiritHealer;
        plan.recheckMs = memory.ghostNextCheckMs - nowMs;
    }
    return plan;
}

// A check: enemyYards from the corpse to the nearest enemy player (FAR_YARDS for none), ghostMs since the release.
// One row when a wait starts (enemy, the yards), one when it ends (gone or healer, the seconds waited)
inline GhostTurn GhostChecked(PvpMemory& memory, float enemyYards, uint32_t ghostMs, uint32_t nowMs,
                              GhostValues const& values)
{
    GhostTurn turn;
    turn.plan = PlanGhost(enemyYards, ghostMs, values);
    int64_t const waitedSeconds = memory.ghostWaiting && memory.ghostSinceMs ? (nowMs - memory.ghostSinceMs) / 1000 : 0;
    uint32_t nextMs = values.recheckMs;
    switch (turn.plan.step)
    {
        case GhostStep::Wait:
            if (!memory.ghostWaiting)
            {
                memory.ghostWaiting = true;
                memory.ghostSinceMs = nowMs ? nowMs : 1;
                turn.row = GHOST_ENEMY;
                turn.amount = std::lround(enemyYards);
            }
            nextMs = turn.plan.recheckMs;
            break;
        case GhostStep::SpiritHealer:
            memory.ghostWaiting = true;
            memory.ghostSinceMs = 0;
            turn.row = GHOST_HEALER;
            turn.amount = waitedSeconds;
            nextMs = GHOST_HEALER_HOLD_MS;
            break;
        case GhostStep::Corpse:
            if (memory.ghostWaiting)
            {
                turn.row = GHOST_GONE;
                turn.amount = waitedSeconds;
            }
            memory.ghostWaiting = false;
            break;
    }
    uint32_t const nextCheckMs = nowMs + nextMs;
    memory.ghostNextCheckMs = nextCheckMs ? nextCheckMs : 1;
    return turn;
}

// A resurrection ends the wait; the next death starts with a check
inline void GhostRevived(PvpMemory& memory)
{
    memory.ghostWaiting = false;
    memory.ghostSinceMs = 0;
    memory.ghostNextCheckMs = 0;
}

// A bot's real master (WP.6): `has` for a game client's player or a selfbot as master (HasGameClientMaster), and
// `master` that player. Under the test constraint realmaster any master counts, in a bot group its leader: `has`
// then holds also for a lone bot, whose `master` is null (it may join no one's fight)
struct RealMasterFacts
{
    bool has = false;
    Player* master = nullptr;
};

// The glue (GhostWait.cpp), the bot's map thread. The bot's real master, read uncached (two pointer checks, the
// group leader's lookup only under the constraint); nothing for no AI. Not to be kept past the call
RealMasterFacts RealMasterOf(PlayerbotAI* botAI);
// FindCorpseAction and ReviveFromCorpseAction: a persistent ghost in the open world with an enemy player (alive, of
// the other team, flagged or not) within GhostEnemyYards of its corpse waits as a ghost, runs to its corpse once the
// enemy has gone and takes the spirit healer GhostSpiritHealerSeconds after its release (the corpse's ghost time: a
// recheck never resets it). One corpse search per recheck, the last answer in between; a GhostWait row when a wait
// starts and when it ends. Step Corpse for every other ghost (a real master, RealMasterOf: it follows its master; a
// battleground or instance; not persistent, IsPersistentBot; its corpse on another map)
GhostPlan PlanGhostFor(PlayerbotAI* botAI, Corpse const* corpse);
}  // namespace WorldPvp

// The resurrection hook: any resurrection ends a bot's wait
void AddGhostWaitScripts();

#endif
