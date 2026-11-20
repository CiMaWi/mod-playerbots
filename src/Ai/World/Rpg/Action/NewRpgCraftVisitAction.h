/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NEWRPGCRAFTVISITACTION_H
#define _PLAYERBOT_NEWRPGCRAFTVISITACTION_H

#include "NewRpgBaseAction.h"

// Steps a persistent bot's craft visit (CraftVisit.h) from any RPG status: walks within the station's own range, then
// holds the bot there while it casts, until the visit ends. Only a started visit (BankMail::BeginCraftVisit) makes it
// useful.
class NewRpgCraftVisitAction : public NewRpgBaseAction
{
public:
    NewRpgCraftVisitAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg craft visit") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
