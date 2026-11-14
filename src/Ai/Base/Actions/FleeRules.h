/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FLEERULES_H
#define _PLAYERBOT_FLEERULES_H

#include <algorithm>
#include <cstdint>

// The flight from attacking enemy players (FleeAttackersAction): its state and the rules that begin and end it.
// Player-free for the selftest (tests/flee-rules-selftest.cpp)
namespace FleeRules
{
enum class Outcome : std::uint8_t
{
    None,
    Escaped,   // out of PvP combat, or the escape distance from every attacker
    Cornered,  // the nearest attacker got no farther for the cornered time
    Bound      // the flight's time bound
};

// The world log's source word of a Flee row (literals: the log keeps the pointer)
constexpr char const* Word(Outcome outcome)
{
    switch (outcome)
    {
        case Outcome::Escaped:
            return "escaped";
        case Outcome::Cornered:
            return "cornered";
        case Outcome::Bound:
            return "bound";
        default:
            return "none";
    }
}

// A gain is the nearest attacker this much farther than ever before in the flight
constexpr float GAIN_YARDS = 1.0f;
// A turn adds at most this much to the time without a gain, so a pause of the bot's AI (a stun, a delay) counts once
constexpr std::uint32_t TURN_MS = 1000;

struct Limits
{
    float escapeYards;
    std::uint32_t corneredMs;
    std::uint32_t boundMs;
};

struct Flight
{
    bool active = false;
    // It ended cornered or by its bound: the bot stands and fights and does not flee again in this PvP combat
    bool stood = false;
    std::uint32_t startMs = 0;
    std::uint32_t lastMs = 0;
    std::uint32_t noGainMs = 0;
    float bestYards = 0.0f;
    std::uint32_t mapId = 0;
    float startX = 0.0f;
    float startY = 0.0f;
    // The step under way, and the last look round that took no direction (FleeAttackersAction)
    std::uint32_t stepMs = 0;
    float stepX = 0.0f;
    float stepY = 0.0f;
    bool lookFailed = false;
    bool lookHolds = false;
    std::uint32_t lookMs = 0;
};

// Whether a flight begins: enemy players are in PvP combat with the bot, the nearest closer than the escape distance,
// and it has not stood in this PvP combat
inline bool Begins(Flight const& flight, std::uint32_t foes, float nearestYards, Limits const& limits)
{
    return !flight.active && !flight.stood && foes != 0 && nearestYards < limits.escapeYards;
}

inline void Begin(Flight& flight, float nearestYards, std::uint32_t nowMs)
{
    flight = Flight();
    flight.active = true;
    flight.startMs = nowMs;
    flight.lastMs = nowMs;
    flight.bestYards = nearestYards;
}

// One turn of a flight under way. `foes`: the enemy players in PvP combat with the bot, `nearestYards` the nearest's
// distance; `canMove`: the bot is not rooted, stunned or the like. The time without a gain counts only turns in which
// the bot can move, and the measure starts again from where the nearest attacker is when it can move again
inline Outcome Judge(Flight& flight, std::uint32_t foes, float nearestYards, bool canMove, std::uint32_t nowMs,
                     Limits const& limits)
{
    std::uint32_t const turnMs = std::min<std::uint32_t>(nowMs - flight.lastMs, TURN_MS);
    flight.lastMs = nowMs;
    if (!foes || nearestYards >= limits.escapeYards)
        return Outcome::Escaped;
    if (nowMs - flight.startMs >= limits.boundMs)
        return Outcome::Bound;
    if (!canMove || nearestYards >= flight.bestYards + GAIN_YARDS)
    {
        flight.bestYards = nearestYards;
        flight.noGainMs = 0;
        return Outcome::None;
    }
    flight.noGainMs += turnMs;
    return flight.noGainMs >= limits.corneredMs ? Outcome::Cornered : Outcome::None;
}

inline void End(Flight& flight, Outcome outcome)
{
    flight = Flight();
    flight.stood = outcome == Outcome::Cornered || outcome == Outcome::Bound;
}

// Whether the trigger of a strategy that wants the bot to flee is on (`wants`: its own reason, now). A flight under
// way runs to its outcome whatever the strategy wants by then; a bot that stood flees again only in a new PvP combat;
// no flight begins out of PvP combat
inline bool Due(Flight& flight, bool wants, bool pvpCombat)
{
    if (flight.active)
        return true;
    if (!pvpCombat)
    {
        flight.stood = false;
        return false;
    }
    return wants && !flight.stood;
}
}  // namespace FleeRules

#endif
