/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MOVETOLOOTRULES_H
#define _PLAYERBOT_MOVETOLOOTRULES_H

#include <cstdint>

// The progress watch of MoveToLootAction: when a loot target the bot gets no nearer to is given up.
// Player-free for the selftest (tests/move-to-loot-rules-selftest.cpp)
namespace MoveToLootRules
{
constexpr std::uint32_t NO_PROGRESS_MS = 5000;  // give up on a target after 5 s without progress
constexpr float PROGRESS_YARDS = 0.5f;          // a smaller gain is distance jitter
constexpr float REACHED_MARGIN_YARDS = 2.0f;    // this near the contact distance the target is reached

// The target watched (its raw guid, 0: none), the nearest distance reached and when it was reached
struct Watch
{
    std::uint64_t target = 0;
    float distance = 0.0f;
    std::uint32_t sinceMs = 0;
};

// One call of the move to `target`, `distance` yards away: true (and the watch cleared) when the bot has got no
// nearer for NO_PROGRESS_MS since the target's first call. The clock runs between calls however far apart they come,
// so a target whose calls are seconds apart (another action claiming the ticks between) is given up too.
inline bool GiveUp(Watch& watch, std::uint64_t target, float distance, std::uint32_t nowMs, float contactYards)
{
    if (target != watch.target)
        watch = Watch{target, distance, nowMs};
    else if (distance < watch.distance - PROGRESS_YARDS)
    {
        watch.distance = distance;
        watch.sinceMs = nowMs;
    }

    if (distance <= contactYards + REACHED_MARGIN_YARDS || nowMs - watch.sinceMs < NO_PROGRESS_MS)
        return false;
    watch = Watch{};
    return true;
}
}  // namespace MoveToLootRules

#endif
