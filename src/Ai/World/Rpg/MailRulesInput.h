/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MAILRULESINPUT_H
#define _PLAYERBOT_MAILRULESINPUT_H

#include <vector>

#include "MailRules.h"

class Player;
class PlayerbotAI;

// The live mail list and bags as the plain inputs of MailRules.h. Mail list reads (Player::GetMails) happen only on
// the bot's own map thread inside its AI update or on the world thread (BankMailState.h); nothing here keeps a Mail*
// or Item* past the call.
namespace BankMail
{
// The bot's mails as it sees them now: deleted and expired mail left out; item worth from its item usage
// (ItemUsageValue::ItemWorth), an item that is never destroyed for space marked priority with its use worth
std::vector<MailIn> BuildMailIn(PlayerbotAI* botAI);
// Free slots of the backpack and the plain bags, and the room on partial stacks there
BagSpace BuildBagSpace(Player* bot);
// The configured mails per visit; no refusals
MailOptions BuildMailOptions();
}  // namespace BankMail

#endif
