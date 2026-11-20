/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgCraftVisitAction.h"

#include <algorithm>

#include "CraftVisit.h"
#include "PlayerbotAI.h"

bool NewRpgCraftVisitAction::isUseful()
{
    return BankMail::VisitRunsOf(botAI->bankMail.visit, BankMail::VisitKind::Craft);
}

bool NewRpgCraftVisitAction::Execute(Event /*event*/)
{
    // A random step when no path leads on, as the errands do
    constexpr float UNSTUCK_STEP_YARDS = 10.0f;
    // The walk stops this far inside the station's own range, so the cast's range test (IsWithinDistInMap) holds
    constexpr float RANGE_MARGIN_YARDS = 1.0f;
    constexpr float MIN_APPROACH_YARDS = 1.0f;

    BankMail::CraftVisitStep const step = BankMail::StepCraftVisit(botAI);
    if (step.result != BankMail::CraftVisitResult::Walk)
        return step.result != BankMail::CraftVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the station
    bool const moving =
        step.station.IsEmpty()
            ? MoveFarTo(WorldPosition(step.mapId, step.x, step.y, step.z))
            : MoveWorldObjectTo(step.station, std::max(step.range - RANGE_MARGIN_YARDS, MIN_APPROACH_YARDS));
    if (!moving)
        MoveRandomNear(UNSTUCK_STEP_YARDS);
    return true;
}
