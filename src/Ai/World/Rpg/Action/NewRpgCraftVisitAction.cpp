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
    // The visit's place is reached where its step stops asking for the walk (STATION_SEARCH_YARDS, CraftVisit.cpp)
    constexpr float VISIT_REACH_YARDS = 20.0f;
    // The walk stops this far inside the station's own range, so the cast's range test (IsWithinDistInMap) holds
    constexpr float RANGE_MARGIN_YARDS = 1.0f;
    constexpr float MIN_APPROACH_YARDS = 1.0f;

    BankMail::CraftVisitStep const step = BankMail::StepCraftVisit(botAI);
    if (step.result != BankMail::CraftVisitResult::Walk)
        return step.result != BankMail::CraftVisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the station
    if (step.station.IsEmpty())
        return Away(WorldPosition(step.mapId, step.x, step.y, step.z), VISIT_REACH_YARDS).value_or(true);
    // The last yards into the station's range (WT68, as the bank visit's banker): a station whose stand point the
    // mesh does not carry ends the visit (its Stuck row); no other move is tried
    if (Approach(step.station, std::max(step.range - RANGE_MARGIN_YARDS, MIN_APPROACH_YARDS)) == ::GoTo::Answer::Failed)
        BankMail::EndCraftVisit(botAI, BankMail::VisitEnd::NoStation);
    return true;
}
