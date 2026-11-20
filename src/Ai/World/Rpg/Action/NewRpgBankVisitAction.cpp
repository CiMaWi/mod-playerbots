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
    // The banker's object is walked up to with a random step when no path leads on (WT64g's)
    constexpr float UNSTUCK_STEP_YARDS = 10.0f;
    // The visit's place is reached where its step stops asking for the walk (BANKER_SEARCH_YARDS, BankVisit.cpp)
    constexpr float VISIT_REACH_YARDS = 20.0f;

    BankMail::BankVisitStep const step = BankMail::StepBankVisit(botAI);
    if (step.result != BankMail::BankVisitResult::Walk)
        return step.result != BankMail::BankVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the banker
    if (step.banker.IsEmpty())
        return Away(WorldPosition(step.mapId, step.x, step.y, step.z), VISIT_REACH_YARDS).value_or(true);
    if (!MoveWorldObjectTo(step.banker))
        MoveRandomNear(UNSTUCK_STEP_YARDS);
    return true;
}
