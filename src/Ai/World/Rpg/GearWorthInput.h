/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GEARWORTHINPUT_H
#define _PLAYERBOT_GEARWORTHINPUT_H

#include <optional>

#include "Define.h"
#include "GearWorthRules.h"

class PlayerbotAI;
struct ItemTemplate;

namespace GearWorth
{
// The worth rule's inputs for one item on a live bot, and the equipment slots wearing it takes
struct LiveInput
{
    WorthInput worth;
    uint8 slot{0};    // the slot it replaces
    uint32 slots{0};  // one bit per equipment slot wearing it changes: the slot; for a weapon maybe both hands
};

// The bot's thread. Scores the item (with its suffix), what it would replace and everything then worn under one
// score setup (R2), following the slot choice of the upgrade rule and the equip pass; empty for an item without an
// equipment slot. It does not ask whether the bot would wear the item: that is "item upgrade" (R1)
std::optional<LiveInput> ReadInput(PlayerbotAI* botAI, ItemTemplate const* proto, int32 randomPropertyId);
}  // namespace GearWorth

#endif
