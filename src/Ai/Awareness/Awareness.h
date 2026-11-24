/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AWARENESS_H
#define _PLAYERBOT_AWARENESS_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "AwarenessRules.h"

class ChatHandler;
class PlayerbotAI;

// The bot's picture of the units around it, filled from one grid visit (AwarenessRules.h holds the types and rules).
// Every call runs on the bot's own map thread, or on the world thread between map updates (console commands)
namespace Awareness
{
// Corpses whose entry a test's check can count (Store::deadEntries); more are only counted
constexpr std::size_t DEAD_KEPT = 32;

// The bot's picture and its age; PlayerbotAI::awareness, created on the first Look
struct Store
{
    AwarenessRules::Picture picture;
    bool built = false;
    std::uint32_t fillUs = 0;       // the last fill's duration
    std::uint16_t deadSkipped = 0;  // dead units within the range at the last fill, not in the picture
    std::array<std::uint32_t, DEAD_KEPT> deadEntries{};
    // A test's `aw check reach=<entry>`: the reach the picture named for that creature, read by a later `reached=`
    float namedReach = -1.0f;
    std::uint32_t namedReachEntry = 0;
};

// What is around the bot now, both sides; rebuilt first when it is older than its age (AwarenessRules::IsFresh)
AwarenessRules::Picture const& Look(PlayerbotAI* botAI);

// Who would join if the bot attacked this creature of its picture: the neighbours the engine would call when the
// target enters the fight (AwarenessRules::WouldAssistByPlace, then the engine's CanAssistTo and the target's line
// of sight to each). Slots of the picture; empty when the target is not a creature in it. No grid visit
AwarenessRules::UnitList JoinersOf(PlayerbotAI* botAI, std::uint64_t targetGuid);

// `playerbots aw look <bot> [target=<entry|guid:<counter>|name>]`: the picture, one unit per line; with a target
// also the line of who would join it
bool HandleLookCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw check <bot> [enemies=<n>] [entries=<e1,e2..> spawns=<n>] [target=.. joins=<n>] ...`: a test's
// verdict line (the words: Awareness.cpp, CheckUsage)
bool HandleCheckCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw stats`: the counters since the server started
bool HandleStatsCommand(ChatHandler* handler);
// The meter line of `playerbots tickperf`: reads and rebuilds since the last reset; `reset` starts a new window
std::string MeterLine(bool reset);
}  // namespace Awareness

#endif
