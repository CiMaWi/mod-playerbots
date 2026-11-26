/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NEWRPGVISITACTION_H
#define _PLAYERBOT_NEWRPGVISITACTION_H

#include <string>

#include "NewRpgBaseAction.h"

// Steps a persistent bot's mailbox, bank or craft visit (Visit.h StepVisit) from any RPG status: walks to the spawn,
// then into the kind's reach of its object, then holds the bot there until the visit ends. Only a started visit
// (BankMail::BeginVisit) makes it useful.
class NewRpgVisitAction : public NewRpgBaseAction
{
public:
    NewRpgVisitAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg visit") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    // A test probe (auction house AH3), bots under a test constraint only: a line at each change of the walk's answer
    void ProbeWalk(char const* leg, char const* answer);
    uint32 probeVisit{0};  // the visit's start (ms) the asks count for
    uint32 probeAsks{0};
    std::string probeLast;
};

#endif
