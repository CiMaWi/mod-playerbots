/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSE_H
#define _PLAYERBOT_ITEMUSE_H

#include <cstdint>
#include <string>

#include "ItemUseRules.h"
#include "ItemUseTypes.h"

class GameObject;
class Item;
class Player;
class PlayerbotAI;
class Spell;
class Unit;
struct Position;

// The use path (item-use plan WI3, INTERFACE.md 2.2): one function checks a use, one place builds the packet, and the
// game's own handler casts it, as a client's packet would be (C10). Every request that Use takes ends in one ItemUse
// row. Check, Use and the request builders run on the bot's map thread only (C3); they decide nothing about when.
namespace ItemUse
{
// The first cause that refuses the request, Cause::None when the bot may use it; ends with the engine's CheckCast
// with the cast item (engine: its SpellCastResult for Cause::Engine). Sends nothing, writes no row
Cause Check(Player* bot, Request const& request, std::uint32_t* engine = nullptr);
// Check, then the packet to the game's handler. An instant use writes its row now (Used or Refused); a use with a
// cast time or a channel returns Pending and writes its row at its end, used or interrupted (Update). A bot under the
// test constraint nouse makes no request but the test's: Refused with Cause::None, no row
UseResult Use(Player* bot, Request const& request);

Request OnSelf(Item* item, Caller caller);
Request OnUnit(Item* item, Unit* target, Caller caller);
Request OnItem(Item* item, Item* target, Caller caller);
Request AtSpot(Item* item, Position const& spot, Caller caller);
Request OnObject(Item* item, GameObject* target, Caller caller);
// The piece worn in the equipment slot (a trinket, boots, gloves with a tinker); the record's shape decides the target
Request Worn(Player* bot, std::uint8_t slot, Caller caller);

// A timed use between its start and its row (at most one per bot; PlayerbotAI::botItems)
struct BotItems
{
    PendingUse pending;
    PendingSigns signs;
    std::uint64_t itemGuid{0};
    std::uint32_t kind{0};
    float startX{0.0f};
    float startY{0.0f};
    bool cancelLooked{false};
    bool active{false};
};

bool Pending(Player* bot);
// The bot's map thread, from its AI update while BotWork::ItemTimed is set: ends a pending use with its row when
// the engine cast it, cancelled it, ended it or the bound passed
void Update(PlayerbotAI* botAI, Player* bot, std::uint32_t nowMs);
// A pending use ends at once with the event's row (the bot logs out)
void EndPending(PlayerbotAI* botAI, Player* bot, UseEvent event);
// The engine's spell hooks (AllSpellScript, the caster's map thread): note a pending use's cast or cancel; free while
// no use is pending anywhere
void OnSpellCast(Spell const* spell, Unit* caster);
void OnSpellCastCancel(Spell const* spell, Unit* caster);

// The console's order (playerbots item use): parsed on the world thread, resolved and used on the bot's map thread
enum class OrderTarget : std::uint8_t
{
    Shape,         // no target word: the record's shape decides
    Self,          // self
    Unit,          // unit <player name>
    Creature,      // creature <entry>: the nearest of that template within 100 yd
    Item,          // item <slot>: one of the bot's worn pieces
    Spot,          // at <x> <y> <z>: a spot on the bot's map
    SpotCreature,  // at creature <entry>: the spot where the nearest of that template stands
    Object         // object <entry>: the nearest game object of that template within 100 yd
};

struct Order
{
    std::uint32_t entry{0};
    std::uint8_t slot{SLOT_BAGS};  // slot <slot>: the worn piece instead of an entry
    OrderTarget target{OrderTarget::Shape};
    std::string unitName;
    std::uint32_t creatureEntry{0};
    std::uint32_t objectEntry{0};
    std::uint8_t itemSlot{SLOT_BAGS};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
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
