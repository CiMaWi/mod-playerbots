/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBankVisitAction.h"

#include "BankVisit.h"
#include "PlayerbotAI.h"

bool NewRpgBankVisitAction::isUseful()
{
    return botAI->rpgInfo.bankMail.bankVisit.phase != BankMail::BankVisitPhase::None;
}

bool NewRpgBankVisitAction::Execute(Event /*event*/)
{
    // A random step when no path leads on, as the errands do
    constexpr float UNSTUCK_STEP_YARDS = 10.0f;

    BankMail::BankVisitStep const step = BankMail::StepBankVisit(botAI);
    if (step.result != BankMail::BankVisitResult::Walk)
        return step.result != BankMail::BankVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the banker
    bool const moving = step.banker.IsEmpty() ? MoveFarTo(WorldPosition(step.mapId, step.x, step.y, step.z))
                                              : MoveWorldObjectTo(step.banker);
    if (!moving)
        MoveRandomNear(UNSTUCK_STEP_YARDS);
    return true;
}
