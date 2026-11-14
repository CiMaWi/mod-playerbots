/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKRULESINPUT_H
#define _PLAYERBOT_BANKRULESINPUT_H

#include "BankRules.h"

class Player;
class PlayerbotAI;

// The live bags and bank as the plain inputs of BankRules.h. Runs on the bot's own map thread inside its AI update
// or on the world thread, only at login, from commands and at a bank visit: never per tick. Nothing here keeps an
// Item* past the call and nothing creates an Item.
namespace BankMail
{
// The bot's level, the horizon and the upgrade threshold
KeepBotIn BuildKeepBot(Player const* bot);
// The bags' and the bank's items with what the providers read (worth now, use worth, wearable, scores against the
// worn items), and the room in both
BankIn BuildBankIn(PlayerbotAI* botAI);
// Fills the bot's BankKnowledge from the bank's items: the kept list (the bank items the rule keeps now, by later
// worth), the free bank item slots and the due summary; and refreshes the carried keeps
void RefreshBankKnowledge(PlayerbotAI* botAI);
// The bag items the rule keeps for a later level (BankKnowledge::carried), read by the item usage. Refreshed at login,
// at a level-up, at a bank visit and before the bot sells, destroys for space or plans an errand: the item usage itself
// never decides a keep. Only gear above the bot's level is read. Empty for a bot that does not keep its possessions
void RefreshCarriedKeeps(PlayerbotAI* botAI);
// The later worth of one carried kept item of the entry, or 0 (KeptWorthEach at the bot's level)
uint32_t CarriedWorthEach(PlayerbotAI* botAI, uint32_t entry);
}  // namespace BankMail

#endif
