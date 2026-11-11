/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTWIRING_H
#define _PLAYERBOT_FIGHTWIRING_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "FightBookRules.h"
#include "FightRules.h"

// How the glue (FightBook.cpp) feeds the fight rules (FightRules.h) from what it reads off the players, and what a
// decision changes in the bot's fight memory. Player-free: tests/fight-wiring-selftest.cpp proves the mapping.
namespace WorldPvp
{
// The target the rules picked stays the scan's target this long (the bot closes in and attacks within it)
constexpr uint32_t START_TARGET_MS = 20 * MS_PER_SECOND;
// The bot-side gate values are read again after this long
constexpr uint32_t GATE_REFRESH_MS = 5 * MS_PER_SECOND;

// Where a bot is, for the choice between the rules and today's attack on sight
struct BotPlace
{
    bool persistent = false;
    // An open-world map (it has a fight book: not instanceable)
    bool hasBook = false;
    bool battleground = false;
    // A PvpTest fighter (WP.6)
    bool forced = false;
};

// A persistent bot on an open-world map outside battlegrounds and arenas, unless PvpTest forces it, starts fights by
// the rules; every other bot keeps today's attack on sight
constexpr bool StartsByRules(BotPlace const& place)
{
    return place.persistent && place.hasBook && !place.battleground && !place.forced;
}

// The bot-side values the gate caches, as the glue reads them
struct GateFacts
{
    bool prohibited = false;
    bool realMaster = false;
    bool realInGroup = false;
    bool inGroup = false;
    bool leader = false;
    float graveyardYards = FAR_YARDS;
    float aggression = 0.0f;
    float caution = 0.0f;
    float helpfulness = 0.0f;
    float courtesy = 0.0f;
    float patience = 0.0f;
};

inline bool GateDue(FightMemory const& memory, uint32_t nowMs)
{
    return !memory.gateKnown || nowMs - memory.gateMs >= GATE_REFRESH_MS;
}

inline void RefreshGate(FightMemory& memory, GateFacts const& facts, uint32_t nowMs)
{
    memory.gateKnown = true;
    memory.gateMs = nowMs;
    memory.openWorld = !facts.prohibited;
    memory.realPlayer = facts.realMaster || facts.realInGroup;
    memory.groupMember = facts.inGroup && !facts.leader;
    memory.graveyardYards = facts.graveyardYards;
    memory.aggression = facts.aggression;
    memory.caution = facts.caution;
    memory.helpfulness = facts.helpfulness;
    memory.courtesy = facts.courtesy;
    memory.patience = facts.patience;
}

// The bot's side of a meeting: the cached gate values and the memory's clocks
inline StartBot MakeStartBot(FightMemory const& memory, uint32_t nowMs)
{
    StartBot bot;
    bot.openWorld = memory.openWorld;
    bot.realPlayer = memory.realPlayer;
    bot.groupMember = memory.groupMember;
    bot.inFight = memory.fightId != 0;
    bot.atObjective = false;  // WP10: capture points
    bot.inCalmArea = false;   // WP6: the calm-area cache
    bot.sinceFightEndMs = memory.hasFought ? nowMs - memory.lastFightEndMs : NEVER_MS;
    bot.sinceChanceMs = nowMs - (memory.hasChance ? memory.lastChanceMs : memory.bornMs);
    bot.graveyardYards = memory.graveyardYards;
    // A ready bot (the lab's `fight ready`) is never peaceable: its next chance starts when the hard rules pass
    bot.aggression = memory.ready ? std::max(memory.aggression, 0.0f) : memory.aggression;
    bot.caution = memory.caution;
    bot.courtesy = memory.courtesy;
    return bot;
}

// The scan gate's word: Refusal::None when the bot could start a fight now
inline Refusal GateOf(FightMemory const& memory, uint32_t nowMs, FightRuleValues const& values)
{
    return StartGate(MakeStartBot(memory, nowMs), values);
}

// The enemy met, as the glue reads it
struct TargetFacts
{
    // Today's target checks passed: attackable, PvP flagged, visible, outside the exclusions
    bool enemy = true;
    uint32_t botLevel = 1;
    uint32_t targetLevel = 1;
    float health = 1.0f;
    // Its victim is a monster
    bool busy = false;
    float graveyardYards = FAR_YARDS;
    uint32_t sinceResurrectionMs = NEVER_MS;
};

inline StartTarget MakeStartTarget(TargetFacts const& facts, FightMemory const& memory, uint64_t guid, uint32_t nowMs)
{
    StartTarget target;
    target.enemy = facts.enemy;
    target.colour = ColourOf(facts.botLevel, facts.targetLevel);
    target.sinceResurrectionMs = facts.sinceResurrectionMs;
    target.graveyardYards = facts.graveyardYards;
    target.contestsObjective = false;  // WP10: capture points
    target.sinceRolledMs = memory.SinceRolled(guid, nowMs);
    target.busy = facts.busy;
    target.health = facts.health;
    target.grudge = 0.0f;  // WP9: grudges
    return target;
}

// Whether the meeting reaches the roll, so the glue counts the odds (a search of the players around) only then
inline bool NeedsOdds(StartBot const& bot, StartTarget const& target, FightRuleValues const& values)
{
    return DecideStart(bot, target, 1.0f, values).chance;
}

// The meeting's decision and what it changes in the memory: a chance restarts the chance clock and marks the target
// as rolled for, start or not; a ready bot's chance starts (p 1) and clears ready; a start picks the target
inline StartDecision DecideMeeting(FightMemory& memory, StartBot const& bot, StartTarget const& target, uint64_t guid,
                                   float roll, uint32_t nowMs, FightRuleValues const& values)
{
    StartDecision decision = DecideStart(bot, target, roll, values);
    if (decision.chance)
    {
        if (memory.ready)
        {
            decision.start = true;
            decision.p = 1.0f;
            decision.refusal = Refusal::None;
            memory.ready = false;
        }
        memory.hasChance = true;
        memory.lastChanceMs = nowMs;
        memory.MarkRolled(guid, nowMs);
    }
    if (decision.start)
    {
        memory.startGuid = guid;
        memory.startMs = nowMs;
        memory.startReason = decision.reason;
    }
    return decision;
}

// Whether `guid` is the target the rules picked for this bot, still fresh
inline bool IsPicked(FightMemory const& memory, uint64_t guid, uint32_t nowMs)
{
    return guid && memory.startGuid == guid && nowMs - memory.startMs < START_TARGET_MS;
}

struct Point
{
    float x = 0.0f;
    float y = 0.0f;
};

// The yards to the nearest of `points` (FAR_YARDS for none)
inline float NearestYards(std::vector<Point> const& points, float x, float y)
{
    float nearest = FAR_YARDS;
    for (Point const& point : points)
        nearest = std::min(nearest, std::hypot(point.x - x, point.y - y));
    return nearest;
}
}  // namespace WorldPvp

#endif
