/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOUNTRULES_H
#define PLAYERBOTS_MOUNTRULES_H

// The one mount owner (travel WT44, C2-6), Player-free: whether a bot may mount now and on what. Both mount paths
// ask it: CheckMountStateAction::Execute (a ground mount, or the master's kind) and a travel fly leg's MountToFly
// (NewRpgRoute.cpp, a flyer). tests/travel-lifecycle-selftest.cpp drives it. Included only by .cpp files (the
// action's header declares the types forward).

#include <cstdint>

namespace MountRules
{
enum class Kind : std::uint8_t
{
    None,
    Ground,
    Flying
};

// How a bot comes off its mount (CheckMountStateAction::Dismount(bot, how)): the same request every time, the word
// names the path in a traced bot's [TravelInteraction] row
enum class How : std::uint8_t
{
    Plain,      // the mount action: a target in reach, the master on foot
    Transport,  // a boarding or a disembark onto a transport (PrepareTransportMove)
    Loot,       // a loot interaction's motion (LootAction PrepareMotion)
    Taxi,       // at the flight master, before the flight
    Fly         // a fly leg's takeoff: off the ground mount, then the flyer
};

struct Facts
{
    bool lootInteraction = false;  // the bot's loot interaction runs (LootObjectStack, BotTravel::HasLootInteraction)
    bool lootWindow = false;       // a loot window is open (Player::GetLootGUID)
    bool lootNear = false;         // a corpse the bot may loot lies in its loot reach ("has available loot")
    bool noMount = false;          // the test constraint nomount (TestConstraints::MountsOnItsOwn)
    bool onFoot = false;           // the trip keeps the bot on foot (TravelKeepsOnFoot: a transport leg, a tame)
};

struct Verdict
{
    bool allowed;
    bool waits;  // refused for now: the loot ends soon, so a fly leg's try does not count
    Kind kind;
    char const* why;  // allowed, none, loot, nomount, on_foot (no string is built: a literal)
};

// The rules in order: loot first (a running interaction, an open window or a corpse in reach: a mounted bot cannot
// open a corpse and rides out of reach before it looks for one, so it loots before it rides on: WT44), then the test
// constraint, then the trip's on-foot rule. A fly leg asks with Kind::Flying: nomount stops it too (one rule,
// judgment call WT.40; a test that needs a flight sets no nomount)
inline Verdict Policy(Facts const& facts, Kind wanted)
{
    if (wanted == Kind::None)
        return {false, false, Kind::None, "none"};
    if (facts.lootInteraction || facts.lootWindow || facts.lootNear)
        return {false, true, Kind::None, "loot"};
    if (facts.noMount)
        return {false, false, Kind::None, "nomount"};
    if (facts.onFoot)
        return {false, false, Kind::None, "on_foot"};
    return {true, false, wanted, "allowed"};
}

inline char const* HowWord(How how)
{
    switch (how)
    {
        case How::Transport:
            return "transport";
        case How::Loot:
            return "loot";
        case How::Taxi:
            return "taxi";
        case How::Fly:
            return "fly";
        default:
            return "plain";
    }
}
}  // namespace MountRules

#endif
