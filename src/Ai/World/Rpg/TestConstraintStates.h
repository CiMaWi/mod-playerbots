/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTCONSTRAINTSTATES_H
#define PLAYERBOTS_TESTCONSTRAINTSTATES_H

#include "Define.h"

class Player;
class PlayerbotAI;
struct TravelOrder;

// A test's constraints on the bot (TestConstraints.h): the state rows' apply and undo; on the bot's map thread
namespace TravelOrders
{
// An order's constraint change (TestOrders.cpp's ApplyTravelOrder); tripFollows: the order starts a trip as well
void ApplyConstraints(PlayerbotAI* botAI, Player* bot, TravelOrder const& order, bool tripFollows);
// The bot's state constraints applied again (a pet summoned since gets nofight); map thread, at every AI update of a
// bot with a state row set
void KeepConstraints(Player* bot, uint32 constraints);
// walkflush (TestConstraints::FlushesWalks): the bot's MeshMiss rows written now, where a walk is ordered or ends.
// One bit test for a bot without the row; map thread
void FlushWalk(PlayerbotAI* botAI);
}  // namespace TravelOrders

#endif
