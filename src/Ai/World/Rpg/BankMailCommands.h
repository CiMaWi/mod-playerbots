/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BANKMAILCOMMANDS_H
#define _PLAYERBOT_BANKMAILCOMMANDS_H

class ChatHandler;

// Console and GM commands of the bank-mail plan (playerbots mail ...); they run on the world thread
namespace BankMail
{
// playerbots mail status <bot name>: what the bot knows of its mail and what the mail rules would do now (read only)
bool HandleMailStatusCommand(ChatHandler* handler, char const* args);
// playerbots mail visit [<bot name>]: sends a persistent bot to the nearest mailbox on its map for one visit
// (MailVisit.h); without a name, the visit counters
bool HandleMailVisitCommand(ChatHandler* handler, char const* args);
// playerbots mail selftest: the mail rules' scenarios (MailRulesSelftest.h)
bool HandleMailSelftestCommand(ChatHandler* handler, char const* args);
}  // namespace BankMail

#endif
