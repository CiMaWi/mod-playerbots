/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEITEMRULE_H
#define PLAYERBOTS_BRIDGEITEMRULE_H

#include <cstdint>
#include <string_view>

// WI10a: the use_item row's refusals. The item-use framework (ItemUse::Use) refuses with a cause word (INTERFACE.md
// 1.4, ItemUseTypes.h CAUSE_WORDS); the wire's Refusal set is closed (mod-ollama-chat's OrderRefusal switches over its
// 18 values), so the causes collapse onto those the row lists. The exact cause stays in the ItemUse row
// (caller ability, refused:<cause>). Plain data and no game type, so a selftest can feed every word.
namespace PlayerbotBridge
{
// The row's refusals, the same words as Refusal's (BridgeAbilities.cpp converts one to the other).
enum class ItemRefusal : uint8_t
{
    None,
    UnknownName,
    TooFar,
    TargetNotVisible,
    Dead,
    CannotNow
};

// The cause word of a refused use to the refusal told to the player: no item to be had -> UnknownName; out of reach ->
// TooFar; unseen -> TargetNotVisible; a dead bot -> Dead; every other cause (level, cooldown, fight, target kind,
// the engine's own refusal ...) -> CannotNow. No NoTarget: its wire text is about attacking.
constexpr ItemRefusal RefusalOfCause(std::string_view cause)
{
    if (cause == "none")
        return ItemRefusal::None;
    if (cause == "no_item" || cause == "not_equipped")
        return ItemRefusal::UnknownName;
    if (cause == "range" || cause == "too_close")
        return ItemRefusal::TooFar;
    if (cause == "sight")
        return ItemRefusal::TargetNotVisible;
    if (cause == "dead")
        return ItemRefusal::Dead;

    return ItemRefusal::CannotNow;
}
}  // namespace PlayerbotBridge

#endif
