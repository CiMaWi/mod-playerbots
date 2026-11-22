/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgVisitAction.h"

#include "BankMailState.h"
#include "PlayerbotAI.h"
#include "Visit.h"

bool NewRpgVisitAction::isUseful() { return BankMail::VisitRuns(botAI->bankMail); }

bool NewRpgVisitAction::Execute(Event /*event*/)
{
    BankMail::VisitStep const step = BankMail::StepVisit(botAI);
    if (step.result != BankMail::VisitResult::Walk)
        return step.result != BankMail::VisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the visit's object
    if (step.object.IsEmpty())
        return Away(WorldPosition(step.mapId, step.x, step.y, step.z), step.reach).value_or(true);
    // The last yards into the kind's reach (WT64g, WT68): an object whose stand point the mesh does not carry ends the
    // visit (its Stuck row); no other move is tried
    if (Approach(step.object, step.reach) == ::GoTo::Answer::Failed)
        BankMail::EndVisits(botAI, step.unreached);
    return true;
}
