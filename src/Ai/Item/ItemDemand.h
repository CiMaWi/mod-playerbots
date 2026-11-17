/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMDEMAND_H
#define _PLAYERBOT_ITEMDEMAND_H

#include <cstdint>
#include <vector>

#include "ItemDemandRules.h"

class Player;
class PlayerbotAI;

// Item use WI9a, INTERFACE 2.4: demand for the live bot. The input is read from its bags on its map thread (or on the
// world thread while the map threads wait, as a command does) and given to ItemUse::Demand (ItemDemandRules.h).
// The answer is cached in the bot's BotItems and built again when it is older than DEMAND_REFRESH_MS or when the
// count of the item asked about differs from what the answer saw; every build has its PerfMonitor row "item demand".
// Persistent bots only (WI.9): for every other bot the answer is empty and every item "not mine".
namespace ItemUse
{
constexpr std::uint32_t DEMAND_REFRESH_MS = 10000;  // [E] a sale of many items costs one build

struct DemandHeld
{
    std::uint32_t entry{0};
    std::uint32_t count{0};
};

struct DemandBuilt
{
    DemandAnswer answer;
    std::vector<DemandHeld> held;  // every consumable in the bags when it was built, by entry
    Role role{Role::Melee};
    std::uint32_t earnCopperPerMinute{0};
};

struct DemandCache
{
    DemandBuilt built;
    std::uint32_t builtMs{0};
    bool valid{false};
};

// A fresh answer from the bot's bags (no cache); withBank: the bank's item slots join the list (the bank's use stock,
// item use WI9b)
DemandBuilt BuildDemand(PlayerbotAI* botAI, Player* bot, bool withBank = false);
// The cached answer; entry: the item asked about (its count is checked against the answer's), 0: none
DemandAnswer const& DemandOf(PlayerbotAI* botAI, std::uint32_t entry = 0);
// Demand's verdict on one item; DemandVerdict::None: not demand's item (not a consumable, not persistent, no index)
DemandVerdict DemandVerdictOf(PlayerbotAI* botAI, std::uint32_t entry);
// One unit's worth to the bot (ItemWorth's useWorthEach), 0: not demand's item
std::uint32_t DemandWorthEach(PlayerbotAI* botAI, std::uint32_t entry);
// Whether demand answers for this bot: a persistent bot and a built index
bool DemandAnswers(PlayerbotAI* botAI);
}  // namespace ItemUse

#endif
