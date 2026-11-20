/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBankVisitAction.h"

#include "BankVisit.h"
#include "PlayerbotAI.h"

bool NewRpgBankVisitAction::isUseful() { return botAI->bankMail.bankVisit.phase != BankMail::BankVisitPhase::None; }

bool NewRpgBankVisitAction::Execute(Event /*event*/)
{
    // The visit's place is reached where its step stops asking for the walk (BANKER_SEARCH_YARDS, BankVisit.cpp)
    constexpr float VISIT_REACH_YARDS = 20.0f;

    BankMail::BankVisitStep const step = BankMail::StepBankVisit(botAI);
    if (step.result != BankMail::BankVisitResult::Walk)
        return step.result != BankMail::BankVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the banker
    if (step.banker.IsEmpty())
        return Away(WorldPosition(step.mapId, step.x, step.y, step.z), VISIT_REACH_YARDS).value_or(true);
    // The last yards to the banker (WT64g): one whose stand point the mesh does not carry ends the visit (its Stuck
    // row)
    if (Approach(step.banker) == ::GoTo::Answer::Failed)
        BankMail::EndBankVisit(botAI, BankMail::VisitEnd::NoSpot);
    return true;
}
