/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STATUSENDTYPES_H
#define PLAYERBOTS_STATUSENDTYPES_H

// The outcome of an RPG status end, game-free: the type the status state holds (rules: StatusEndRules.h).

#include <cstdint>

namespace StatusEnd
{
// What happened, not a judgement. Interrupted: another status, the planner or a reset took the bot over. Ordered: a
// GM, test or bridge order ended it. Sick: resurrection sickness ended it and the bot rests. Danger: a risen bot gives
// up its goal in ground it cannot survive or at its deaths in a row, and leaves (awareness AW38b, INTERFACE.md §3.6).
enum class Outcome : std::uint8_t
{
    Arrived,
    Finished,
    TimedOut,
    Stuck,
    Abandoned,
    Interrupted,
    Ordered,
    Sick,
    Danger
};
constexpr std::uint32_t OUTCOME_COUNT = 9;
}  // namespace StatusEnd

#endif
