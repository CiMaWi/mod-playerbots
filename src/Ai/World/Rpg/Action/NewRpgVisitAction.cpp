/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgVisitAction.h"

#include "BankMailState.h"
#include "Log.h"
#include "MoveSpline.h"
#include "PlayerbotAI.h"
#include "Transport.h"
#include "Visit.h"
#include "VisitLifecycle.h"

namespace
{
char const* WalkWord(::GoTo::Answer answer)
{
    return answer == ::GoTo::Answer::There ? "there" : answer == ::GoTo::Answer::Going ? "going" : "failed";
}

// A unit under a move (its flags, or a spline not yet finished); an object never moves
bool Moving(WorldObject const* object)
{
    Unit const* unit = object ? object->ToUnit() : nullptr;
    return unit && (unit->isMoving() || !unit->movespline->Finalized());
}
}  // namespace

bool NewRpgVisitAction::isUseful() { return BankMail::VisitRuns(botAI->bankMail); }

bool NewRpgVisitAction::Execute(Event /*event*/)
{
    BankMail::VisitStep const step = BankMail::StepVisit(botAI);
    if (step.result != BankMail::VisitResult::Walk)
        return step.result != BankMail::VisitResult::Idle;

    // True while walking too: the lower RPG actions would lead the bot away from the visit's object
    if (step.object.IsEmpty())
    {
        ::GoTo::Answer answer = ::GoTo::Answer::Going;
        std::optional<bool> const away = Away(WorldPosition(step.mapId, step.x, step.y, step.z), step.reach, &answer);
        if (botAI->testConstraints)
            ProbeWalk("spot", WalkWord(answer));
        // An auction visit ends at a failed walk to the spawn too (auction house AH4); the other kinds wait for their
        // limit
        if (answer == ::GoTo::Answer::Failed &&
            BankMail::VisitRunsOf(botAI->bankMail.visit, BankMail::VisitKind::Auction))
            BankMail::EndVisits(botAI, step.unreached);
        return away.value_or(true);
    }
    // The last yards into the kind's reach (WT64g, WT68): an object whose stand point the mesh does not carry ends the
    // visit (its Stuck row); no other move is tried. The step found the object out of reach: an approach that answers
    // there with both standing still ends it too (NR9: a stand point a floor away), unreached
    ::GoTo::Answer const answer = Approach(step.object, step.reach);
    if (botAI->testConstraints)
        ProbeWalk("reach", WalkWord(answer));
    bool const there = answer == ::GoTo::Answer::There;
    if (BankMail::ApproachEndsVisit(answer == ::GoTo::Answer::Failed, there, there && Moving(bot),
                                    there && Moving(botAI->GetWorldObject(step.object))))
        BankMail::EndVisits(botAI, step.unreached);
    return true;
}

void NewRpgVisitAction::ProbeWalk(char const* leg, char const* answer)
{
    uint32 const started = botAI->bankMail.visit.startedMs;
    if (started != probeVisit)
    {
        probeVisit = started;
        probeAsks = 0;
        probeLast.clear();
    }
    ++probeAsks;
    std::string const now = std::string(leg) + " " + answer;
    if (now == probeLast)
        return;
    probeLast = now;
    LOG_INFO("playerbots", "[Probe] {} visit walk {} at ask {} transport {} at ({:.1f}, {:.1f}, {:.1f})",
             bot->GetName(), now, probeAsks, bot->GetTransport() ? bot->GetTransport()->GetEntry() : 0,
             bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}
