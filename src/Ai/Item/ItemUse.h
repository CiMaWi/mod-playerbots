/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSE_H
#define _PLAYERBOT_ITEMUSE_H

#include <cstdint>
#include <string>

#include "ItemUseTypes.h"

class Item;
class Player;
class PlayerbotAI;
class Unit;

// The use path (item-use plan WI3, INTERFACE.md 2.2): one function checks a use, one place builds the packet, and the
// game's own handler casts it, as a client's packet would be (C10). Every request that Use takes ends in one ItemUse
// row. Check, Use and the request builders run on the bot's map thread only (C3); they decide nothing about when.
namespace ItemUse
{
// The first cause that refuses the request, Cause::None when the bot may use it; ends with the engine's CheckCast
// with the cast item (engine: its SpellCastResult for Cause::Engine). Sends nothing, writes no row
Cause Check(Player* bot, Request const& request, std::uint32_t* engine = nullptr);
// Check, then the packet to the game's handler; writes the row. A use with a cast time or a channel counts as used
// when its cast starts (WI4 follows it to its end). A bot under the test constraint nouse makes no request but the
// test's: Refused with Cause::None, no row
UseResult Use(Player* bot, Request const& request);

Request OnSelf(Item* item, Caller caller);
Request OnUnit(Item* item, Unit* target, Caller caller);
Request OnItem(Item* item, Item* target, Caller caller);

// The console's order (playerbots item use): parsed on the world thread, resolved and used on the bot's map thread
enum class OrderTarget : std::uint8_t
{
    Shape,     // no target word: the record's shape decides
    Self,      // self
    Unit,      // unit <player name>
    Creature,  // creature <entry>: the nearest of that template within 100 yd
    Item       // item <slot>: one of the bot's worn pieces
};

struct Order
{
    std::uint32_t entry{0};
    std::uint8_t slot{SLOT_BAGS};  // slot <slot>: the worn piece instead of an entry
    OrderTarget target{OrderTarget::Shape};
    std::string unitName;
    std::uint32_t creatureEntry{0};
    std::uint8_t itemSlot{SLOT_BAGS};
    bool dead{false};
    std::uint32_t postedMs{0};
};

// Any thread: queues the order and sets the bot's work bit (BotWork::ItemOrder)
void PostOrder(PlayerbotAI* botAI, std::uint64_t botGuid, Order order);
// The bot's map thread, from its AI update while the bit is set: uses the bot's waiting orders in their order; an
// order older than a minute is dropped with a log line
void TakeOrders(PlayerbotAI* botAI, Player* bot);
}  // namespace ItemUse

#endif
