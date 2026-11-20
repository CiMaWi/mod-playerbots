/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PURSUITTYPES_H
#define _PLAYERBOT_PURSUITTYPES_H

#include <array>
#include <cstdint>

// The state of an order's give-up clock (PursuitRules.h holds the rules). Kept beside the bot, not in the order's
// status data, which an interlude replaces: the clock of the order the bot goes back to resumes
namespace Pursuit
{
// The kinds of order with a give-up clock of their own; a quest is the first
enum class Kind : std::uint8_t
{
    None,
    Quest
};

struct Key
{
    Kind kind{Kind::None};
    std::uint32_t target{0};  // a quest: its id

    bool operator==(Key const& other) const { return kind == other.kind && target == other.target; }
};

struct Clock
{
    Key key;
    std::uint32_t run{0};        // the order run that ticked it last (a new run resumes it without counting the gap)
    std::uint32_t idleMs{0};     // running time since the last progress: the give-up reads it
    std::uint32_t pursuedMs{0};  // running time of the whole pursuit: the failure row's minutes
    std::uint32_t progress{0};   // the order's progress count at the last tick
    std::uint32_t lastUse{0};    // the slot's age stamp: the oldest slot is taken for a new key
    bool started{false};         // the progress mark is set (the first tick sets it, counting no progress)
    bool ticking{false};         // the tick before ran the order (false before the first and after a hold)
    bool givenUp{false};         // this run gave up: a second report of it writes nothing
};

constexpr std::size_t CLOCKS = 4;  // the orders a bot keeps a clock for: the one it does and those it may go back to

struct Clocks
{
    std::array<Clock, CLOCKS> slots{};
    std::uint32_t uses{0};
};
}  // namespace Pursuit

#endif
