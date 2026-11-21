/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <memory>
#include <string_view>
#include <utility>

#include "BroadcastHelper.h"
#include "CensusMgr.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestExecRules.h"
#include "QuestIndex.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

// WT35h: a complete quest's turn-in. The bot walks to the nearest ender of the quest it has not tried in this run
// (QuestExecRules.h NextEnder) through WalkOn, whose watch tries an ender the walk gains nothing on while another is
// left (StallTries; the last one's walk starts again, the order's give-up clock its bound); near the ender the
// giver search at the top of Execute hands the quest in (TurnInQuest, its packets; the reward choice is its own). An
// ender it is not handed in at within a look (not there, hostile, in another phase) is tried as well, and when none is
// left the order ends no_ender. After mod-playerbots PR #3031 (head 52a77ddc: the turn-in at the ender's spawn) and
// PR #2632 (head d4e5d110: the rotation over enders), without their fallbacks
namespace
{
constexpr float ENDER_ARRIVE_YARDS = 10.0f;             // the walk ends this near the ender's spawn
constexpr uint32 ENDER_LOOK_MS = 60 * IN_MILLISECONDS;  // at the ender this long, the quest not handed in: tried

// A quest_exec decision row (world-log.md): ref is the quest
void Trace(Player* bot, std::string_view choice, std::string_view reason, float value, uint32 questId,
           WorldLogFormat::Alternatives const& alternatives = {})
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, choice, reason, value, alternatives, questId);
}

// The ender behind a turn-in place key (the index's Turnin area): its template and 1 for an object, else 0, 0
std::pair<uint32, bool> EnderAt(uint32 questId, Planner::QuestExec::PlaceKey key)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(key);
    uint32 const area = key & 0xFFu;
    if (!record || objective >= record->objectives.size() || area >= record->objectives[objective].areas.size())
        return {0, false};
    Planner::AreaPoint const& at = record->objectives[objective].areas[area];
    return {at.ender, at.enderObject};
}
}  // namespace

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::Act;
    uint32 const questId = data.questId;
    if (data.objectiveIdx != -1)
    {
        // Complete: QuestReady once, then the turn-in (objectiveIdx -1 marks it for the rest of the run)
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, data.quest);
        botAI->RecordQuestCompleted(questId);
        EventLog::Emit(bot, EventLog::EventType::QuestReady, "executor", questId, 0);
        ClearTarget(data);
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
    {
        std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
        if (!index)
            return false;
        Planner::QuestRecord const* record = index->Find(questId);
        Planner::QuestExec::Step const next =
            record ? Planner::QuestExec::NextEnder(*record, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                                   static_cast<uint8>(bot->GetTeamId()), data.tried)
                   : Planner::QuestExec::Step{Act::NoPlace};
        if (next.act != Act::TurnIn)
        {
            // No ender on this map (no_place), or every one tried (no_ender): the hook writes the QuestFail row
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = Planner::QuestExec::EndWord(next)});
            return true;
        }
        data.place = Planner::QuestExec::KeyOf(next);
        data.pos = WorldPosition(bot->GetMapId(), next.place->x, next.place->y, next.place->z);
    }

    // A fight first (the combat actions fight): no walk and no stall counted. Ticks resume after it more than 5 s
    // apart, so the walk's watch takes the fight as a gap (WalkStalled), not as a walk that gained nothing
    if (bot->IsInCombat())
        return false;
    float const yards = bot->GetDistance(data.pos);
    if (!data.lastReachPOI)
    {
        if (yards > ENDER_ARRIVE_YARDS)
            return WalkOn(data, data.pos, yards, false, data.place);
        ResetWalk(data);
        data.lastReachPOI = getMSTime();
        auto const [ender, object] = EnderAt(questId, data.place);
        Trace(bot, "place_reached", "ender", yards, questId,
              {{"place", static_cast<float>(data.place)},
               {"entry", static_cast<float>(ender)},
               {"object", object ? 1.0f : 0.0f}});
        return true;
    }
    if (GetMSTimeDiffToNow(data.lastReachPOI) < ENDER_LOOK_MS)
        return false;
    Trace(bot, "place_tried", "look", yards, questId,
          {{"place", static_cast<float>(data.place)}, {"tried", static_cast<float>(data.tried.size() + 1)}});
    data.tried.push_back(data.place);
    LeavePlace(data);
    data.objectiveIdx = -1;  // the turn-in goes on at the next ender
    return true;
}
