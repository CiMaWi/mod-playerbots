/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgMailVisitAction.h"

#include "MailVisit.h"
#include "PlayerbotAI.h"

bool NewRpgMailVisitAction::isUseful() { return botAI->bankMail.mailVisit.phase != BankMail::MailVisitPhase::None; }

bool NewRpgMailVisitAction::Execute(Event /*event*/)
{
    // The mailbox's object is walked up to with a random step when no path leads on (WT64g's)
    constexpr float UNSTUCK_STEP_YARDS = 10.0f;
    // The visit's place is reached where its step stops asking for the walk (MAILBOX_SEARCH_YARDS, MailVisit.cpp)
    constexpr float VISIT_REACH_YARDS = 20.0f;

    BankMail::MailVisitStep const step = BankMail::StepMailVisit(botAI);
    if (step.result != BankMail::MailVisitResult::Walk)
        return step.result != BankMail::MailVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the mailbox
    if (step.mailbox.IsEmpty())
        return Away(WorldPosition(step.mapId, step.x, step.y, step.z), VISIT_REACH_YARDS).value_or(true);
    if (!MoveWorldObjectTo(step.mailbox))
        MoveRandomNear(UNSTUCK_STEP_YARDS);
    return true;
}
