/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTORDERS_H
#define PLAYERBOTS_TESTORDERS_H

#include <string>

#include "Define.h"
#include "TravelMgr.h"

class Player;
class PlayerbotAI;

// GM orders for tests (`playerbots route go|stop|transport|constraint|hold|release|aboard`): the command posts on the
// world thread, the bot's map thread takes its order at its next AI update, ahead of the AI's delays (TakeAndApply
// from PlayerbotAI::UpdateAI: one atomic load per update while no order waits for the bot; an order that moves the
// bot waits until it is out of combat, a constraint change never). A bot outside the new rpg strategy refuses its
// order; an order not taken within a minute (the bot logged out or kept fighting) is dropped
struct TravelOrder
{
    WorldPosition dest{};
    std::string place;  // the name in the log lines
    bool stop{false};   // end the bot's trip instead
    uint32 postedMs{0};
    uint16 on{0};  // TestConstraints bits set, then those cleared (`off`), before anything else of the order
    uint16 off{0};
    uint32 aboard{0};  // `route aboard`: the transport entry to stand the bot on; 0 = none
};

namespace TravelOrders
{
void Post(PlayerbotAI* botAI, TravelOrder order);
bool TakeAndApply(PlayerbotAI* botAI, Player* bot);  // on the bot's map thread: the order taken and applied
// The bot's state constraints applied again (a pet summoned since gets nofight); map thread, at every AI update of a
// bot with a state row set
void KeepConstraints(Player* bot, uint16 constraints);
}  // namespace TravelOrders

#endif
