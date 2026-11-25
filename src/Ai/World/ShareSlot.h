/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SHARESLOT_H
#define _PLAYERBOT_SHARESLOT_H

#include <cstdint>

// A share of the persistent bots chosen by guid, for a comparison by cohort (world PvP's
// Playerbots.WorldPvp.Enthusiasm.StartCostShare): a bot's slot 0..99 is a fixed mix of its guid counter,
// so it stays in one cohort across builds and restarts and SQL names the same bots:
// ((guid * 2654435761) & 0xFFFFFFFF) >> 16) % 100 < share
// Every share here uses this one mix, so a smaller share lies wholly inside a larger one. Player-free and header-only
// for the selftests.
namespace BotShare
{
constexpr uint32_t SHARE_MIX = 2654435761u;
constexpr uint32_t SHARE_SHIFT = 16;
constexpr uint32_t SHARE_PERCENT_MAX = 100;

constexpr uint32_t ShareSlot(uint32_t guidCounter)
{
    return ((guidCounter * SHARE_MIX) >> SHARE_SHIFT) % SHARE_PERCENT_MAX;
}

constexpr bool InShare(uint32_t guidCounter, uint32_t sharePercent) { return ShareSlot(guidCounter) < sharePercent; }
}  // namespace BotShare

#endif
