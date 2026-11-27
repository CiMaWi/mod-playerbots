/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GEARWORTHINPUT_H
#define _PLAYERBOT_GEARWORTHINPUT_H

#include <memory>
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

// The bot's thread. Reads many items of one bot under one score setup (R2): the calculator, the worn items' scores
// and the earn rates are set up once, at construction (R7: an auction visit's gear lines read every listed item with
// one reader). Valid while the bot's gear and level stay as they were then
class InputReader
{
public:
    explicit InputReader(PlayerbotAI* botAI);
    ~InputReader();
    InputReader(InputReader const&) = delete;
    InputReader& operator=(InputReader const&) = delete;

    // Scores the item (with its suffix) against what it would replace and everything then worn, following the slot
    // choice of the upgrade rule and the equip pass; empty for an item without an equipment slot. It does not ask
    // whether the bot would wear the item: that is "item upgrade" (R1)
    std::optional<LiveInput> Read(ItemTemplate const* proto, int32 randomPropertyId) const;

private:
    struct State;
    PlayerbotAI* botAI;
    std::unique_ptr<State> state;
};

// One item: InputReader's read with a reader of its own (the vendor's purchases, which change the worn gear)
std::optional<LiveInput> ReadInput(PlayerbotAI* botAI, ItemTemplate const* proto, int32 randomPropertyId);
}  // namespace GearWorth

#endif
