/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_HANDINROOMRULES_H
#define _PLAYERBOT_HANDINROOMRULES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// questing WQ43 (WQ.49): room before a hand-in. When the rewards of a quest to hand in need more free slots than the
// bot has, it destroys stacks until they fit, the lowest vendor value of a stack first, and never a kept one (a quest
// item, a worn item, a bag, the hearthstone, a profession's tool, a key, a conjured thing it uses, a thing without
// vendor value). Pure: the bags as a list of stacks and the slots the rewards take; no game object.
// tests/hand-in-room-selftest.cpp checks it
namespace Planner
{
namespace HandInRoom
{
// One stack in the bot's general slots: its vendor value (sell price times count) and whether it is kept
struct Stack
{
    std::uint64_t value{0};
    bool kept{false};
};

// The new slots one reward item takes: its units less what a stack the bot carries still holds (the carried units
// are taken as packed in full stacks), in stacks of at most maxStack
constexpr std::uint32_t SlotsFor(std::uint32_t count, std::uint32_t maxStack, std::uint32_t carried)
{
    std::uint32_t const stack = maxStack ? maxStack : 1;
    std::uint32_t const room = carried && stack > 1 ? (stack - carried % stack) % stack : 0;
    std::uint32_t const rest = count > room ? count - room : 0;
    return (rest + stack - 1) / stack;
}

// The slots the bot lacks for rewards that take `needed` new slots
constexpr std::uint32_t SlotsShort(std::uint32_t needed, std::uint32_t free)
{
    return needed > free ? needed - free : 0;
}

// The stacks to destroy for `slotsShort` slots, cheapest first (of equal value the earlier): none when nothing is
// short, and none when the stacks that may go cannot cover it (the hand-in could not happen anyway). O(n log n)
inline std::vector<std::size_t> Victims(std::span<Stack const> stacks, std::uint32_t slotsShort)
{
    std::vector<std::size_t> free;
    if (!slotsShort)
        return free;
    for (std::size_t i = 0; i < stacks.size(); ++i)
        if (!stacks[i].kept)
            free.push_back(i);
    if (free.size() < slotsShort)
        return {};
    std::stable_sort(free.begin(), free.end(),
                     [&stacks](std::size_t a, std::size_t b) { return stacks[a].value < stacks[b].value; });
    free.resize(slotsShort);
    return free;
}
}  // namespace HandInRoom
}  // namespace Planner

#endif
