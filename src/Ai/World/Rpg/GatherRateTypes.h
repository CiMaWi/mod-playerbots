/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERRATETYPES_H
#define _PLAYERBOT_GATHERRATETYPES_H

#include <cstddef>
#include <cstdint>

// The data of the observed gather rates (GatherRateRules.h) that a bot's state holds: a circuit's way and one span as
// it ended. Types only, so NewRpgInfo.h keeps to its closure (consolidation CS15).
namespace GatherRate
{
enum class Way : std::uint8_t
{
    Herbalism,
    Mining,
    Creatures,
    None  // mixed ways, no circuit or another skill: never observed, always the prior
};

constexpr std::size_t WAYS = 3;

// One circuit span as it ended: its zone at the start, its way, how long it ran, its takes and the stops it resolved
// (a take or a no_source)
struct Span
{
    std::uint32_t zone{0};
    Way way{Way::None};
    std::uint32_t milliseconds{0};
    std::uint32_t takes{0};
    std::uint32_t resolved{0};
};
}  // namespace GatherRate

#endif
