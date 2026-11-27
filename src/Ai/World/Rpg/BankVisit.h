/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKVISIT_H
#define _PLAYERBOT_BANKVISIT_H

#include <optional>
#include <string>

#include "Define.h"
#include "ObjectGuid.h"
#include "Visit.h"

class Player;
class PlayerbotAI;

// A persistent bot's banker visit (bank-mail plan, PLAN §3.5): walk into reach of a banker, withdraw what is due or
// no longer kept, deposit what is kept (BankRules.h), equip, finish. Items move in place on the bot's map thread
// through the core's own checks (CanStoreItem, CanBankItem), as the core's bank handlers do; no packet is queued, so
// the plan is recomputed from the live bags and bank before every move. A kind of the visit driver (Visit.h): the
// lifecycle is PlayerbotAI::bankMail.visit, the bank's own data PlayerbotAI::bankMail.bankData (BankMailState.h).
//
// Used by the GM command, the errand stops and (Withdraw, Deposit) the chat command:
//   std::optional<BankerSpot> FindBankerSpot(Player* bot, char const*& refusal);
//   bool BeginBankVisit(PlayerbotAI* botAI, BankerSpot const& spot, bool auctionNext = false);
// The visit is stepped by the visit driver (Visit.h StepVisit) through `new rpg visit`.
class Item;
class Player;
enum InventoryResult : uint8;

namespace BankMail
{
// A banker's spawn: the planner's service index entry nearest the bot on its map that serves the bot's faction
using BankerSpot = VisitSpot;

// World thread or the bot's map thread: the nearest banker spawn on the bot's map whose faction serves the bot;
// refusal names why there is none
std::optional<BankerSpot> FindBankerSpot(Player* bot, char const*& refusal);
// The core's interact test for a banker's faction (Player::GetNPCIfCanInteractWith): the planner's stop pick and the
// finds use it
bool BankerServes(Player const* bot, uint32 factionTemplate);
// The nearest banker serving the bot within yards of a place (an errand stop's own banker); distance is from the bot
std::optional<BankerSpot> BankerSpotNear(Player* bot, uint32 mapId, float x, float y, float z, float yards);
// One item from the bank into the bags, and one from the bags into the bank: the bodies of the core's bank handlers, on
// the bot's map thread (the visit's moves and the chat bank command). EQUIP_ERR_NONE (deposit): it already lies there
InventoryResult Withdraw(Player* bot, Item* item);
InventoryResult Deposit(Player* bot, Item* item);
// World thread or the bot's map thread: starts a visit to the spot; false while a visit runs. auctionNext: an auction
// errand of the same trip follows, so the sale stock is due for it (NR12b; the GM command passes none)
bool BeginBankVisit(PlayerbotAI* botAI, BankerSpot const& spot, bool auctionNext = false);
// Visits started and ended since the server start, by how they ended (any thread)
std::string BankVisitCounters();
}  // namespace BankMail

#endif
