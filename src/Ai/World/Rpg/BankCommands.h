/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKCOMMANDS_H
#define _PLAYERBOT_BANKCOMMANDS_H

class ChatHandler;

// Console and GM commands of the bank-mail plan's bank side (playerbots bank ...); they run on the world thread
namespace BankMail
{
// playerbots bank status <bot name>: the bank items kept now, what is due, what the bot knows of its bank, and what
// a banker visit would move now (read only)
bool HandleBankStatusCommand(ChatHandler* handler, char const* args);
// playerbots bank selftest: the bank rules' scenarios (BankRulesSelftest.h)
bool HandleBankSelftestCommand(ChatHandler* handler, char const* args);
}  // namespace BankMail

#endif
