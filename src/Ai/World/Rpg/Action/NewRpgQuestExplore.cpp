/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <memory>
#include <string_view>

#include "Log.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestExecRules.h"
#include "QuestIndex.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

// WT35f: an exploring objective. The bot walks to its area trigger's centre, and once inside the trigger the client's
// message goes once through the session's handler (as a portal leg's does, NewRpgRoute.cpp), which credits an
// incomplete quest the trigger belongs to. The log's exploration flag says whether it counted. Ported/adapted from
// mod-playerbots PR #2632 (head d4e5d110): its explore branch, without its direct credit call and its timers
namespace
{
constexpr uint32 QUEST_TRIGGER_HOLD_MS = 2 * IN_MILLISECONDS;  // after the message: the handler's credit
// The walk's reach at the trigger's place: its centre, inside the smallest sphere trigger
constexpr float QUEST_TRIGGER_REACH_YARDS = 2.0f;

// A quest_exec decision row (world-log.md): ref is the quest
void Trace(Player* bot, std::string_view choice, std::string_view reason, float value, uint32 questId,
           WorldLogFormat::Alternatives const& alternatives = {})
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, choice, reason, value, alternatives, questId);
}
}  // namespace

bool NewRpgDoQuestAction::EngageTrigger(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::ExploreAct;
    // A fight first; the combat actions fight
    if (bot->IsInCombat())
        return false;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    uint32 const triggerId = record && objective < record->objectives.size() ? record->objectives[objective].target : 0;
    AreaTrigger const* trigger = triggerId ? sObjectMgr->GetAreaTrigger(triggerId) : nullptr;

    float const yards = bot->GetDistance(data.pos);
    ExploreAct const act = trigger ? Planner::QuestExec::ExploreVerdict(
                                         bot->IsInAreaTriggerRadius(trigger), data.actedAt != 0,
                                         data.actedAt && GetMSTimeDiffToNow(data.actedAt) >= QUEST_TRIGGER_HOLD_MS)
                                   : ExploreAct::Tried;
    switch (act)
    {
        case ExploreAct::Walk:
            return WalkOn(data, data.pos, yards, false, data.place, QUEST_TRIGGER_REACH_YARDS);
        case ExploreAct::Hold:
            return true;
        case ExploreAct::Tried:
            // Inside and sent, the flag still unset: a script took the message, or the trigger was out of reach
            Trace(bot, "place_tried", "trigger", yards, data.questId,
                  {{"place", static_cast<float>(data.place)}, {"trigger", static_cast<float>(triggerId)}});
            data.tried.push_back(data.place);
            LeavePlace(data);
            return true;
        default:
            break;
    }

    ResetWalk(data);
    WorldPacket packet(CMSG_AREATRIGGER, 4);
    packet << triggerId;
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    data.actedAt = getMSTime();
    LOG_DEBUG("playerbots", "[New RPG] {} enters area trigger {} for quest {}", bot->GetName(), triggerId,
              data.questId);
    Trace(bot, "explore_used", "trigger", yards, data.questId, {{"trigger", static_cast<float>(triggerId)}});
    return true;
}
