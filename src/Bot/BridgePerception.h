/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEPERCEPTION_H
#define PLAYERBOTS_BRIDGEPERCEPTION_H

namespace PlayerbotBridge
{
// What a bot perceives (LCB1b, LC.5): one rule and one radius for the state's `around` list and for the target of an
// order. A unit in the world, on the observer's map and phase, within the observer's own sight range (the range the
// value lists "nearest npcs" and "possible targets" keep) that the observer can see or detect. Each verb's reach
// (melee, a spell's range, the pull's bound) stays a check of its row, after this one. Templates so a selftest can feed
// plain stand-ins for the game's Player and Unit.
template <class Observer, class Target>
bool Perceives(Observer* observer, Target* target)
{
    return target && target->IsInWorld() && observer->IsInMap(target) && observer->InSamePhase(target) &&
           observer->IsWithinDistInMap(target, observer->GetSightRange()) &&
           observer->CanSeeOrDetect(target, false, true);
}
}  // namespace PlayerbotBridge

#endif
