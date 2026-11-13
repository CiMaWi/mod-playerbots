/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgMailVisitAction.h"

#include "MailVisit.h"
#include "PlayerbotAI.h"

bool NewRpgMailVisitAction::isUseful()
{
    return botAI->rpgInfo.bankMail.mailVisit.phase != BankMail::MailVisitPhase::None;
}

bool NewRpgMailVisitAction::Execute(Event /*event*/)
{
    // A random step when no path leads on, as the errands do
    constexpr float UNSTUCK_STEP_YARDS = 10.0f;

    BankMail::MailVisitStep const step = BankMail::StepMailVisit(botAI);
    if (step.result != BankMail::MailVisitResult::Walk)
        return step.result != BankMail::MailVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the mailbox
    bool const moving = step.mailbox.IsEmpty() ? MoveFarTo(WorldPosition(step.mapId, step.x, step.y, step.z))
                                               : MoveWorldObjectTo(step.mailbox);
    if (!moving)
        MoveRandomNear(UNSTUCK_STEP_YARDS);
    return true;
}
