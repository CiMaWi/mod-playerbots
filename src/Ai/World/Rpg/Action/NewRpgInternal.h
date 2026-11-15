/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINTERNAL_H
#define PLAYERBOTS_NEWRPGINTERNAL_H

#include "Define.h"
#include "NewRpgInfo.h"

class Player;
class PlayerbotAI;

// The RPG action layer's shared helpers: each is defined in one file of the layer and called from another (design
// rpg-action-layer §5.2). Private to src/Ai/World/Rpg/: a file that calls one has `using namespace NewRpgInternal;`
namespace NewRpgInternal
{
// Transport legs, called by the test orders (`route aboard`)
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry);

// The two finishers, called by the status update's sick branch (CS14 removes both declarations)
void FinishErrands(PlayerbotAI* botAI, bool visited, char const* abortSource = "errand_abort");
void GiveTripUp(PlayerbotAI* botAI, NewRpgInfo::Travel const& data, char const* why, char const* source);
}  // namespace NewRpgInternal

#endif
