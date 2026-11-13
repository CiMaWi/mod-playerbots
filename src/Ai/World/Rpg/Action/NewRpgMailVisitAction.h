/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NEWRPGMAILVISITACTION_H
#define _PLAYERBOT_NEWRPGMAILVISITACTION_H

#include "NewRpgBaseAction.h"

// Steps a persistent bot's mailbox visit (MailVisit.h) from any RPG status: walks to the mailbox, then holds the bot
// there until the visit ends. Only a started visit (BankMail::BeginMailVisit) makes it useful.
class NewRpgMailVisitAction : public NewRpgBaseAction
{
public:
    NewRpgMailVisitAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg mail visit") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
