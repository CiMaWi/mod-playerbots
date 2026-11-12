/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_HOLDRULES_H
#define _PLAYERBOT_HOLDRULES_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// A test's hold on a bot (`playerbots route hold|release`, functional tests): the choices a held bot does not make on
// its own. Kept in NewRpgInfo::heldChoices (not saved), read only where the bot already decides: the idle status
// update, the planner hook, the errand triggers' gate, the hearth snapshots and the grind multiplier. Orders are taken
// before the idle gate, so a held bot runs an order and holds again when it ends (arrived, abort, stop). Player-free
// for the selftest
namespace HoldRules
{
constexpr std::uint8_t PLANNER = 1;  // planner goals and jobs
constexpr std::uint8_t STATUS = 2;   // the weighted random status pick
constexpr std::uint8_t ERRAND = 4;   // vendor errands and inn binds
constexpr std::uint8_t HEARTH = 8;   // hearth legs, the off-mesh hearth and the planner's inn goal
constexpr std::uint8_t ALL = PLANNER | STATUS | ERRAND | HEARTH;
constexpr std::int16_t NO_CHANGE = -1;  // an order without a hold change

constexpr char const* CHOICE_NAMES = "planner, status, errand, hearth";

// The bit a release word names; 0 for an unknown word
inline std::uint8_t ParseChoice(std::string_view word)
{
    if (word == "planner")
        return PLANNER;
    if (word == "status")
        return STATUS;
    if (word == "errand")
        return ERRAND;
    if (word == "hearth")
        return HEARTH;
    return 0;
}

inline std::string Format(std::uint8_t mask)
{
    static constexpr std::pair<std::uint8_t, char const*> names[] = {
        {PLANNER, "planner"}, {STATUS, "status"}, {ERRAND, "errand"}, {HEARTH, "hearth"}};
    std::string out;
    for (auto const& [bit, name] : names)
    {
        if (!(mask & bit))
            continue;
        if (!out.empty())
            out += ',';
        out += name;
    }
    return out.empty() ? "none" : out;
}

// What an idle bot's status update may do on its own
struct IdleGate
{
    bool planner{true};  // planner job submits and goals (PlannerHook, OnIdle)
    bool status{true};   // the weighted status pick
};

inline IdleGate GateOf(std::uint8_t mask) { return {!(mask & PLANNER), !(mask & STATUS)}; }
inline bool ErrandAllowed(std::uint8_t mask) { return !(mask & ERRAND); }
inline bool HearthAllowed(std::uint8_t mask) { return !(mask & HEARTH); }
// A held status also stops grinding ("attack anything" picks no new mob); combat with what attacks the bot, loot,
// food and drink stay
inline bool GrindAllowed(std::uint8_t mask) { return !(mask & STATUS); }

// A pending order (TravelOrder) gains a hold change and/or a trip; a second post merges with the first, so `route hold`
// then `route go` before the bot's next update keeps both. The later trip or stop replaces an earlier one
template <typename Order>
bool HasTrip(Order const& order)
{
    return order.stop || !order.place.empty();
}

// The bot's AI update takes its order at once (TravelOrders::TakeAndApply); only a trip or stop waits until the bot is
// out of combat, a hold or release never waits
template <typename Order>
bool TakeNow(Order const& order, bool inCombat)
{
    return !inCombat || !HasTrip(order);
}

template <typename Order>
void Merge(Order& pending, Order&& incoming)
{
    if (incoming.hold != NO_CHANGE)
        pending.hold = incoming.hold;
    if (HasTrip(incoming))
    {
        pending.dest = incoming.dest;
        pending.place = std::move(incoming.place);
        pending.stop = incoming.stop;
    }
    pending.postedMs = incoming.postedMs;
}
}  // namespace HoldRules

#endif
