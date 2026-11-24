/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERMEMORYSTATE_H
#define _PLAYERBOT_GATHERMEMORYSTATE_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace Gather
{
// The bot's respawn memory (professions WR13d, PLAN 3.9): the spawn points it took or found empty lately and when, so
// that neither a circuit's next lap nor the next circuit's plan sends it back before the spawn's respawn. It outlives
// the gather status (NewRpgInfo::gatherMemory); a ring, so a full one overwrites its slots in turn. Plain data: the
// gather status writes it, the planner snapshot and the selftest read it. Times are getMSTime() values (they wrap)
struct Memory
{
    static constexpr std::size_t SIZE = 32;  // above a circuit's stops (CIRCUIT_STOPS 24): a whole lap fits

    struct Entry
    {
        std::uint32_t spawn{0};
        std::uint32_t atMs{0};
        std::uint32_t respawnMs{0};
    };

    std::array<Entry, SIZE> ring{};
    std::size_t next{0};

    // The spawn was taken or found empty at nowMs; an older note of the same spawn is replaced
    void Note(std::uint32_t spawn, std::uint32_t nowMs, std::uint32_t respawnMs)
    {
        if (!spawn)
            return;
        for (Entry& entry : ring)
            if (entry.spawn == spawn)
            {
                entry = {spawn, nowMs, respawnMs};
                return;
            }
        ring[next] = {spawn, nowMs, respawnMs};
        next = (next + 1) % SIZE;
    }

    // The spawn was noted within its respawn: it holds no node the bot knows of
    [[nodiscard]] bool Away(std::uint32_t spawn, std::uint32_t nowMs) const
    {
        for (Entry const& entry : ring)
            if (spawn && entry.spawn == spawn)
                return static_cast<std::uint32_t>(nowMs - entry.atMs) < entry.respawnMs;
        return false;
    }
};
}  // namespace Gather

#endif
