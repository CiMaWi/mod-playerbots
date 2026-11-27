/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTORDERS_H
#define PLAYERBOTS_TESTORDERS_H

#include <optional>
#include <string>

#include "Define.h"
#include "TravelCrossingTypes.h"
#include "TravelMgr.h"

class Player;
class PlayerbotAI;

// GM orders for tests (`playerbots route go|stop|transport|constraint|hold|release|aboard|quest|start|flush`): the
// command posts on the world thread, the bot's map thread takes its order at its next AI update, ahead of the AI's
// delays (TakeAndApply from PlayerbotAI::UpdateAI: one atomic load per update while no order waits for the bot; an
// order that moves the bot waits until it is out of combat, a constraint change never). A bot outside the new rpg
// strategy takes only the constraint change of an order (TestConstraints::NeedsNewRpg); an order not taken in a
// minute is dropped
struct TravelOrder
{
    WorldPosition dest{};
    std::string place;  // the name in the log lines
    bool stop{false};   // end the bot's trip instead
    uint32 postedMs{0};
    uint32 on{0}, off{0};  // TestConstraints bits set, then those cleared (`off`), before anything else of the order
    uint32 aboard{0};      // `route aboard`: the transport entry to stand the bot on; 0 = none
    uint32 quest{0};       // `route quest`: the quest in the bot's log to start DoQuest for; 0 = none
    bool field{false};     // `route field` (questing WQ19a): a field order at dest's x, y on the bot's map
    float minutes{0.0f};   // and its stay
    std::string status;    // `route start` (WT64e, TestOrderStatus.h): the status word to start at dest; empty = none
    uint32 entry{0};       // what it needs besides the place (quest, vendor, trainer, flight master, node, point)
    bool flush{false};  // `route flush` (WT64e): the bot's MeshMiss rows written now, not at the travel block's flush
    // `route aboard <bot> <entry> x y z` (PA76): the offset on the deck to stand the bot at; none: by the first row
    std::optional<TravelCrossing::Spot> aboardAt;
};

namespace TravelOrders
{
void Post(PlayerbotAI* botAI, TravelOrder order);
bool TakeAndApply(PlayerbotAI* botAI, Player* bot);  // on the bot's map thread: the order taken and applied
}  // namespace TravelOrders

#endif
