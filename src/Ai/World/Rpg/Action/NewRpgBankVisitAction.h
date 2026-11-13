/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NEWRPGBANKVISITACTION_H
#define _PLAYERBOT_NEWRPGBANKVISITACTION_H

#include "NewRpgBaseAction.h"

// Steps a persistent bot's banker visit (BankVisit.h) from any RPG status: walks to the banker, then holds the bot
// there until the visit ends. Only a started visit (BankMail::BeginBankVisit) makes it useful.
class NewRpgBankVisitAction : public NewRpgBaseAction
{
public:
    NewRpgBankVisitAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg bank visit") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
