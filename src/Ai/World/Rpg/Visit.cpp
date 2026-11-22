/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Visit.h"

#include <array>
#include <atomic>
#include <optional>

#include "BankMailService.h"
#include "BankMailState.h"
#include "EventLog.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace BankMail
{
namespace
{
constexpr size_t VISIT_KIND_COUNT = static_cast<size_t>(VisitKind::Craft) + 1;

std::array<std::atomic<uint32>, VISIT_KIND_COUNT> visitsStarted{};
std::array<VisitEndCounters, VISIT_KIND_COUNT> visitsEnded{};

VisitKindRow const* RowOf(VisitKind kind)
{
    switch (kind)
    {
        case VisitKind::Bank:
            return &BankVisitRow();
        case VisitKind::Mail:
            return &MailVisitRow();
        case VisitKind::Craft:
            return &CraftVisitRow();
        default:
            return nullptr;
    }
}
}  // namespace

bool BeginVisit(PlayerbotAI* botAI, VisitKind kind, VisitSpot const* spot)
{
    VisitKindRow const* row = RowOf(kind);
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !row || VisitStartRefusal(botAI->bankMail, kind))
        return false;

    VisitState& visit = botAI->bankMail.visit;
    visit = VisitState{};
    visit.kind = kind;
    visit.phase = spot ? VisitPhase::Walk : VisitPhase::Act;
    botAI->AddWork(BotWork::Visit);
    visit.mapId = spot ? spot->mapId : bot->GetMapId();
    visit.x = spot ? spot->x : bot->GetPositionX();
    visit.y = spot ? spot->y : bot->GetPositionY();
    visit.z = spot ? spot->z : bot->GetPositionZ();
    visit.entry = spot ? spot->entry : 0;
    visit.inPlace = !spot;
    visit.startedMs = getMSTime();
    visit.limitMs = row->limitSeconds() * IN_MILLISECONDS;
    ++visitsStarted[static_cast<size_t>(kind)];
    EventLog::Emit(bot, EventLog::EventType::VisitStart, row->startWord, visit.mapId,
                   static_cast<int64>(spot ? spot->distance : 0.0f));
    return true;
}

VisitStep StepVisit(PlayerbotAI* botAI)
{
    VisitStep step;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || botAI->bankMail.visit.phase == VisitPhase::None)
        return step;

    VisitState& visit = botAI->bankMail.visit;
    VisitKindRow const& row = *RowOf(visit.kind);
    step.result = VisitResult::Ended;
    // The deadline, death, combat and a map change end the visit in the update's watch (BankMail::WatchVisits)
    if (visit.phase == VisitPhase::Await && row.awaited)
    {
        VisitTurn const turn = row.awaited(botAI, bot);
        if (turn.next == VisitNext::End)
        {
            EndVisit(botAI, turn.end);
            return step;
        }
        if (turn.next != VisitNext::Continue)
        {
            step.result = VisitResult::Pending;
            return step;
        }
        visit.phase = VisitPhase::Act;
    }

    // In place: no object to find, resolve or reach
    if (!visit.inPlace)
    {
        if (visit.object.IsEmpty())
        {
            if (bot->GetExactDist(visit.x, visit.y, visit.z) > row.searchYards)
            {
                visit.phase = VisitPhase::Walk;
                step.result = VisitResult::Walk;
                step.mapId = visit.mapId;
                step.x = visit.x;
                step.y = visit.y;
                step.z = visit.z;
                step.reach = row.searchYards;
                return step;
            }
            // Once per visit: a grid search near the spawn
            visit.object = row.find(bot, visit);
            if (visit.object.IsEmpty())
            {
                EndVisit(botAI, row.missing);
                return step;
            }
        }

        // Gone, dead or no longer serving the bot: no walking brings it into reach
        WorldObject const* object = row.resolve(bot, visit.object);
        if (!object)
        {
            EndVisit(botAI, row.missing);
            return step;
        }
        float range = 0.0f;
        if (!row.reach(bot, *object, range))
        {
            visit.phase = VisitPhase::Walk;
            step.result = VisitResult::Walk;
            step.object = visit.object;
            step.reach = row.approach ? row.approach(range) : INTERACTION_DISTANCE;
            step.unreached = row.missing;
            return step;
        }
    }

    visit.phase = VisitPhase::Act;
    VisitTurn const turn = row.act(botAI, bot);
    if (turn.next == VisitNext::End)
    {
        EndVisit(botAI, turn.end);
        return step;
    }
    if (turn.next == VisitNext::Await)
        visit.phase = VisitPhase::Await;
    step.result = VisitResult::Pending;
    return step;
}

void EndVisit(PlayerbotAI* botAI, VisitEnd end)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || botAI->bankMail.visit.phase == VisitPhase::None)
        return;

    VisitState const visit = botAI->bankMail.visit;
    VisitKindRow const& row = *RowOf(visit.kind);
    // A visit's gain is put on now, in its own AI update out of combat
    bool const equip = row.end(botAI, bot, visit, end) && EquipsAfter(end);
    botAI->bankMail.visit = VisitState{};
    ++visitsEnded[static_cast<size_t>(visit.kind)][static_cast<size_t>(end)];
    if (equip)
        botAI->DoSpecificAction("equip upgrades packet action");
}

std::string VisitCounters(VisitKind kind)
{
    size_t const index = static_cast<size_t>(kind);
    return DescribeVisitCounters(kind, visitsStarted[index].load(), visitsEnded[index]);
}
}  // namespace BankMail
