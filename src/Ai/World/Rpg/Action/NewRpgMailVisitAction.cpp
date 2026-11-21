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
    return BankMail::VisitRunsOf(botAI->bankMail.visit, BankMail::VisitKind::Mail);
}

bool NewRpgMailVisitAction::Execute(Event /*event*/)
{
    // The visit's place is reached where its step stops asking for the walk (MAILBOX_SEARCH_YARDS, MailVisit.cpp)
    constexpr float VISIT_REACH_YARDS = 20.0f;

    BankMail::MailVisitStep const step = BankMail::StepMailVisit(botAI);
    if (step.result != BankMail::MailVisitResult::Walk)
        return step.result != BankMail::MailVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the mailbox
    if (step.mailbox.IsEmpty())
        return Away(WorldPosition(step.mapId, step.x, step.y, step.z), VISIT_REACH_YARDS).value_or(true);
    // The last yards to the mailbox (WT64g): one whose stand point the mesh does not carry ends the visit (its Stuck
    // row)
    if (Approach(step.mailbox) == ::GoTo::Answer::Failed)
        BankMail::EndMailVisit(botAI, BankMail::VisitEnd::NoSpot);
    return true;
}
