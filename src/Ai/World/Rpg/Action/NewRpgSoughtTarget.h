/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_NEWRPGSOUGHTTARGET_H
#define PLAYERBOT_NEWRPGSOUGHTTARGET_H

#include <cstddef>
#include <functional>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"

class Creature;
class Player;
struct NewRpgInfo;

// The sought-target attack of questing's executor (WT35a), shared with the gather status's creature stops
// (professions WR20c, family rule 16): the nearest open creature of an entry list, and the target an executor holds,
// which "new rpg attack quest target" attacks
namespace SoughtTarget
{
// A creature open to the bot's attack: alive in the world, not tapped by another (its kill would bring nothing) and
// not friendly
bool Open(Player* bot, Creature* creature);

// The nearest creature of the entries within range of the bot that `valid` accepts; candidates: how many of the
// entries the search found. Null: none
Creature* Nearest(Player* bot, std::vector<uint32> const& entries, float range,
                  std::function<bool(Creature*)> const& valid, std::size_t& candidates);

// The creature the bot's running status holds as its target (the quest status's, the gather status's); empty: none
ObjectGuid Held(NewRpgInfo const& info);
}  // namespace SoughtTarget

#endif
