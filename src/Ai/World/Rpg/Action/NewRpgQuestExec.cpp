/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "BotTravel.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "ClassQuestRules.h"
#include "Creature.h"
#include "G3D/Vector2.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "NewRpgAction.h"
#include "NewRpgBaseAction.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PursuitRules.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestFailRecord.h"
#include "QuestGiveUp.h"
#include "QuestPackets.h"
#include "QuestPlaceRules.h"
#include "Random.h"
#include "StatsWeightCalculator.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldLogStatus.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
// WT35a: the executor's seek and engagement (adapted from mod-playerbots PR #2632, head d4e5d110; tuning values)
constexpr uint32 QUEST_SCAN_MS = 2 * IN_MILLISECONDS;         // a seek at most this often
constexpr float QUEST_SCAN_YARDS = 150.0f;                    // and at most this far (and within sight)
constexpr float QUEST_ENGAGE_YARDS = 30.0f;                   // within this and in sight: the attack
constexpr float QUEST_TARGET_MOVED_YARDS = 10.0f;             // a held target that moved this far is walked to anew
constexpr float QUEST_ARRIVE_YARDS = 10.0f;                   // at the place
constexpr uint32 QUEST_PLACE_LOOK_MS = 60 * IN_MILLISECONDS;  // at a place this long without a target: it is tried
constexpr uint32 QUEST_PLACE_STAY_MS = 5 * MINUTE * IN_MILLISECONDS;  // at a place this long: the next area
// The pursuit and walk clocks are QuestExecRules.h's. Trace rows written once per hold or walk (DoQuest::traced)
constexpr uint8 TRACED_ATTACK = 1;
constexpr uint8 TRACED_WALK = 2;

// A quest_exec decision row (world-log.md): ref is the quest
void Trace(Player* bot, std::string_view choice, std::string_view reason, float value, uint32 questId,
           WorldLogFormat::Alternatives const& alternatives = {})
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, choice, reason, value, alternatives, questId);
}

// Whether the objective in this quest-log slot (0..3 creature or object, QUEST_OBJECTIVES_COUNT + i item i) still
// needs progress; POI_NONE: whether the quest's exploration or event has not happened yet
bool SlotOpen(Quest const* quest, QuestStatusData const& progress, int32 slot)
{
    if (slot == Planner::POI_NONE)
        return quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) && !progress.Explored;
    if (slot >= 0 && slot < QUEST_OBJECTIVES_COUNT)
        return quest->RequiredNpcOrGo[slot] != 0 &&
               progress.CreatureOrGOCount[slot] < quest->RequiredNpcOrGoCount[slot];
    int32 const item = slot - QUEST_OBJECTIVES_COUNT;
    return item >= 0 && item < QUEST_ITEM_OBJECTIVES_COUNT && quest->RequiredItemId[item] != 0 &&
           progress.ItemCount[item] < quest->RequiredItemCount[item];
}

// The step function over the bot's log and the index's places (QuestExecRules.h); false while the index is still
// building. For a step that works an objective, `place` is where
bool NextQuestStep(PlayerbotAI* botAI, Player* bot, NewRpgInfo::DoQuest const& data, Planner::QuestExec::Step& step,
                   QuestDestination& place)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return false;
    uint32 const questId = data.questId;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    Planner::QuestRecord const* record = quest ? index->Find(questId) : nullptr;
    if (!record)
    {
        step = {Planner::QuestExec::Act::NoPlace};
        return true;
    }
    QuestStatus const status = bot->GetQuestStatus(questId);
    auto const it = bot->getQuestStatusMap().find(questId);
    QuestStatusData const* progress = it != bot->getQuestStatusMap().end() ? &it->second : nullptr;
    Planner::QuestExec::LogState const log{
        progress && (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE),
        status == QUEST_STATUS_COMPLETE};
    auto const open = [quest, progress](int32 slot) { return progress && SlotOpen(quest, *progress, slot); };
    Player* const player = bot;
    auto const hasSkill = [player](uint16 skill) { return player->HasSkill(skill); };
    Planner::QuestPlaces::Skip skip;
    if (data.left != WorldPosition())
        skip = {true, data.left.GetPositionX(), data.left.GetPositionY()};
    // A planner bot goes to the place nearest its plan's tasks for the quest, others to the nearest
    auto const prefer = [botAI, questId](std::vector<Planner::QuestExec::Candidate> const& candidates)
    {
        std::vector<std::pair<float, float>> points;
        points.reserve(candidates.size());
        for (Planner::QuestExec::Candidate const& candidate : candidates)
            points.emplace_back(candidate.place->x, candidate.place->y);
        return Planner::PlannedPoi(botAI, questId, points);
    };
    uint32 const mapId = bot->GetMapId();
    step = Planner::QuestExec::Next(*record, log, open, mapId, bot->GetPositionX(), bot->GetPositionY(),
                                    static_cast<uint8>(bot->GetTeamId()), hasSkill, data.tried, skip, prefer);
    if (step.act == Planner::QuestExec::Act::Work)
        place = {WorldPosition(mapId, step.place->x, step.place->y, step.place->z),
                 record->objectives[step.objective].poiIndex, Planner::QuestPlaces::Roam(*step.place)};
    step.place = nullptr;  // it points into this index snapshot
    return true;
}
}  // namespace

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::Act;
    using Planner::QuestExec::Work;
    uint32 const questId = data.questId;
    if (data.pos != WorldPosition())
    {
        // The step stands while its objective is open: the log is the state (a tried place was left when tried)
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        auto const it = bot->getQuestStatusMap().find(questId);
        if (!quest || it == bot->getQuestStatusMap().end() || !SlotOpen(quest, it->second, data.objectiveIdx))
            LeavePlace(data);
    }
    if (data.pos == WorldPosition())
    {
        Planner::QuestExec::Step step;
        QuestDestination place{WorldPosition(), 0, 0.0f};
        if (!NextQuestStep(botAI, bot, data, step, place))
            return false;
        // Refused (the start already asked: PLAN 3.2), no open objective with a place on this map (PLAN 3.1 rule 6),
        // or every place tried without a target
        if (char const* const word = Planner::QuestExec::EndWord(step))
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = word});
            return true;
        }
        if (step.act != Act::Work)
            return false;  // the turn-in or done: Execute follows the log's status
        data.lastReachPOI = 0;
        data.pos = place.pos;
        data.objectiveIdx = place.objectiveIdx;
        data.roam = place.roam;
        data.left = WorldPosition();
        data.place = Planner::QuestExec::KeyOf(step);
        data.work = static_cast<uint8>(step.work);
    }

    // Kill and loot: the bot seeks the objective's creature (a loot objective's: its item's sources) within sight, on
    // the way and at the place, and holds the nearest
    bool const seeks = data.work == static_cast<uint8>(Work::Kill) || data.work == static_cast<uint8>(Work::Loot);
    if (seeks)
    {
        if (!data.targetGuid.IsEmpty())
            return EngageTarget(data);
        if (!data.lastScan || GetMSTimeDiffToNow(data.lastScan) >= QUEST_SCAN_MS)
        {
            data.lastScan = getMSTime();
            if (SeekTarget(data, QUEST_SCAN_YARDS))
                return true;
        }
    }

    if (!data.lastReachPOI)
    {
        float const yards = bot->GetDistance(data.pos);
        if (yards > QUEST_ARRIVE_YARDS)
            return WalkOn(data, data.pos, yards, false, data.place);
        // Now we are near the quest objective
        ResetWalk(data);
        data.lastReachPOI = getMSTime();
        Trace(bot, "place_reached", "place", yards, questId, {{"place", static_cast<float>(data.place)}});
        return true;
    }
    // Five minutes at this place: pick again and leave it for the objective's next area, where the targets are not
    // thinned out (WT33b; the nearest-place rule would return here every time). A stretch without progress is the
    // order's give-up clock's (WT35c, Execute)
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= QUEST_PLACE_STAY_MS)
    {
        WorldPosition const left = data.pos;
        LeavePlace(data);
        data.left = left;
        return true;
    }

    // A place without a target for a look round: tried, so the step function takes the objective's next place
    if (seeks && GetMSTimeDiffToNow(data.lastReachPOI) >= QUEST_PLACE_LOOK_MS &&
        (!data.lastEngage || GetMSTimeDiffToNow(data.lastEngage) >= QUEST_PLACE_LOOK_MS))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} quest {}: no target at place {}, next place", bot->GetName(), questId,
                  data.place);
        Trace(bot, "place_tried", "look", bot->GetDistance(data.pos), questId,
              {{"place", static_cast<float>(data.place)}, {"tried", static_cast<float>(data.tried.size() + 1)}});
        data.tried.push_back(data.place);
        LeavePlace(data);
        return true;
    }

    // At the place: roam inside its area (the spawns' spread, 8 to 40 yd), so the bot meets the area's targets and
    // does not drift off to the next camp
    return RoamInPlace(data.pos, data.roam);
}

bool NewRpgDoQuestAction::ValidTarget(NewRpgInfo::DoQuest const& data, Creature* creature) const
{
    if (!creature || !creature->IsInWorld() || !creature->IsAlive() ||
        Planner::QuestExec::Remembered(data.passed, creature->GetGUID()))
        return false;
    // Tapped by another: its kill would bring no credit
    if (creature->hasLootRecipient() && !creature->isTappedBy(bot))
        return false;
    if (bot->IsFriendlyTo(creature))
        return false;
    // A test's nofight bot is immune to NPCs, so it can attack none: it still holds the objective's creature and its
    // attack is refused, so the order runs without progress (the give-up case, WT35c) instead of finding no target
    return !TestConstraints::StartsFights(botAI->testConstraints) || bot->IsValidAttackTarget(creature);
}

bool NewRpgDoQuestAction::SeekTarget(NewRpgInfo::DoQuest& data, float maxYards)
{
    Quest const* quest = data.quest;
    int32 const slot = data.objectiveIdx;
    std::vector<uint32> entries;
    if (quest && slot >= 0 && slot < QUEST_OBJECTIVES_COUNT && quest->RequiredNpcOrGo[slot] > 0)
        entries.push_back(static_cast<uint32>(quest->RequiredNpcOrGo[slot]));
    else if (data.work == static_cast<uint8>(Planner::QuestExec::Work::Loot))
    {
        // A loot objective: the creatures its item drops from (QuestIndex.h SeekEntries)
        std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
        Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
        uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
        if (record && objective < record->objectives.size())
            entries = record->objectives[objective].seek;
    }
    if (entries.empty())
        return false;

    float const range = std::min({maxYards, QUEST_SCAN_YARDS, sPlayerbotAIConfig.SightDistance});
    std::list<Creature*> creatures;
    bot->GetCreatureListWithEntryInGrid(creatures, entries, range);
    Creature* nearest = nullptr;
    float nearestYards = range;
    for (Creature* creature : creatures)
    {
        if (!ValidTarget(data, creature))
            continue;
        float const yards = bot->GetDistance(creature);
        if (yards < nearestYards)
        {
            nearest = creature;
            nearestYards = yards;
        }
    }
    if (!nearest)
        return false;

    data.targetGuid = nearest->GetGUID();
    data.targetPos =
        WorldPosition(bot->GetMapId(), nearest->GetPositionX(), nearest->GetPositionY(), nearest->GetPositionZ());
    data.engagedMs = 0;
    data.ticking = false;
    data.traced = static_cast<uint8>(data.traced & ~TRACED_ATTACK);
    bool const seen = bot->IsWithinLOSInMap(nearest);
    LOG_DEBUG("playerbots", "[New RPG] {} holds quest target {} ({} yd) for quest {} objective {}", bot->GetName(),
              data.targetGuid.ToString(), nearestYards, data.questId, data.objectiveIdx);
    Trace(bot, "hold", seen ? "los" : "no_los", nearestYards, data.questId,
          {{"entry", static_cast<float>(nearest->GetEntry())}, {"candidates", static_cast<float>(creatures.size())}});
    return true;
}

bool NewRpgDoQuestAction::EngageTarget(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::Pause;
    // A fight or a pause holds the pursuit's clock, never restarts it (QuestExecRules.h); the combat actions fight
    bool const inCombat = bot->IsInCombat();
    uint32 const sinceLast = data.lastEngage ? GetMSTimeDiffToNow(data.lastEngage) : 0;
    Pause const pause = Planner::QuestExec::Pursue(data.engagedMs, data.ticking, inCombat, sinceLast);
    if (pause != Pause::None)
        Trace(bot, "pause", pause == Pause::Combat ? "combat" : "gap", data.engagedMs / 1000.0f, data.questId,
              {{"entry", static_cast<float>(data.targetGuid.GetEntry())}, {"gap", sinceLast / 1000.0f}});
    if (inCombat)
        return false;
    data.lastEngage = getMSTime();

    Unit* unit = botAI->GetUnit(data.targetGuid);
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    if (!creature || !creature->IsAlive())
    {
        ClearTarget(data);  // killed (its corpse goes to the loot) or gone: the log says whether it counted
        return true;
    }
    if (!ValidTarget(data, creature))
    {
        PassOver(data, "invalid");
        return true;
    }
    if (Planner::QuestExec::PursuitOver(data.engagedMs))
    {
        PassOver(data, "clock");
        return true;
    }

    if (creature->GetExactDist(data.targetPos) > QUEST_TARGET_MOVED_YARDS)
        data.targetPos = WorldPosition(bot->GetMapId(), creature->GetPositionX(), creature->GetPositionY(),
                                       creature->GetPositionZ());
    float const yards = bot->GetDistance(creature);

    // A switch needs a target twice as close, so equally near creatures do not take turns
    if (yards > QUEST_ENGAGE_YARDS && (!data.lastScan || GetMSTimeDiffToNow(data.lastScan) >= QUEST_SCAN_MS))
    {
        data.lastScan = getMSTime();
        ObjectGuid const held = data.targetGuid;
        if (SeekTarget(data, yards * 0.5f) && data.targetGuid != held)
            return true;
    }

    if (yards > QUEST_ENGAGE_YARDS || !bot->IsWithinLOSInMap(creature))
        return WalkOn(data, data.targetPos, yards, true, creature->GetEntry());
    ResetWalk(data);
    bool const attacked = botAI->DoSpecificAction("new rpg attack quest target", Event(), true);
    if (!attacked && !(data.traced & TRACED_ATTACK))
    {
        data.traced |= TRACED_ATTACK;
        Trace(bot, "attack_refused", bot->IsMounted() ? "mounted" : "attack", yards, data.questId,
              {{"entry", static_cast<float>(creature->GetEntry())}});
    }
    return attacked;
}

bool NewRpgDoQuestAction::WalkOn(NewRpgInfo::DoQuest& data, WorldPosition const dest, float const yards,
                                 bool const toTarget, uint32 const id)
{
    uint32 const questId = data.questId;
    char const* const kind = toTarget ? "target" : "place";
    WorldLogFormat::Alternatives const where{
        {"x", dest.GetPositionX()}, {"y", dest.GetPositionY()}, {toTarget ? "entry" : "place", static_cast<float>(id)}};
    if (dest != data.walkDest)
    {
        ResetWalk(data);
        data.walkDest = dest;
        Trace(bot, "walk", kind, yards, questId, where);
    }
    bool const first = !data.lastWalk;
    uint32 const sinceLast = first ? 0 : GetMSTimeDiffToNow(data.lastWalk);
    data.lastWalk = getMSTime();
    // After a pause travel's watch starts again too, so the executor's shorter one always gives the walk up first and
    // travel's give-up (which ends the order) does not come
    if (!first && sinceLast > Planner::QuestExec::CLOCK_GAP_MS)
        botAI->rpgMemos.SetMoveFarTo(WorldPosition());
    if (Planner::QuestExec::WalkStalled(data.walkNearest, data.walkStalledMs, first, sinceLast, yards))
    {
        Trace(bot, "walk_stall", kind, yards, questId, where);
        if (toTarget)
            PassOver(data, "walk");
        else
        {
            Trace(bot, "place_tried", "walk", yards, questId,
                  {{"place", static_cast<float>(data.place)}, {"tried", static_cast<float>(data.tried.size() + 1)}});
            data.tried.push_back(data.place);
            LeavePlace(data);
        }
        return true;
    }
    if (MoveFarTo(dest))
    {
        // Travel's give-up still ended the order (an avoided spot, off the mesh): data is gone, the locals stay
        if (!std::holds_alternative<NewRpgInfo::DoQuest>(botAI->rpgInfo.data))
            Trace(bot, "stuck", kind, yards, questId, where);
        return true;
    }
    if (!(data.traced & TRACED_WALK))
    {
        data.traced |= TRACED_WALK;
        Trace(bot, "walk_refused", kind, yards, questId, where);
    }
    // Long-range sampler couldn't land a candidate: nudge the bot a short distance so the next tick retries from a
    // different position instead of sitting idle.
    return MoveRandomNear(10.0f);
}

void NewRpgDoQuestAction::ResetWalk(NewRpgInfo::DoQuest& data)
{
    data.walkDest = WorldPosition();
    data.walkNearest = -1.0f;
    data.walkStalledMs = 0;
    data.lastWalk = 0;
    data.traced = static_cast<uint8>(data.traced & ~TRACED_WALK);
}

void NewRpgDoQuestAction::ClearTarget(NewRpgInfo::DoQuest& data)
{
    if (!data.targetGuid.IsEmpty())
        if (Unit* unit = botAI->GetUnit(data.targetGuid))
            if (!unit->IsAlive())
                AI_VALUE(LootObjectStack*, "available loot")->Add(data.targetGuid);
    data.targetGuid = ObjectGuid::Empty;
    data.targetPos = WorldPosition();
    data.engagedMs = 0;
    data.ticking = false;
    ResetWalk(data);
}

void NewRpgDoQuestAction::PassOver(NewRpgInfo::DoQuest& data, char const* reason)
{
    LOG_DEBUG("playerbots", "[New RPG] {} passes over quest target {} for quest {} ({})", bot->GetName(),
              data.targetGuid.ToString(), data.questId, reason);
    Trace(bot, "pass_over", reason, data.engagedMs / 1000.0f, data.questId,
          {{"entry", static_cast<float>(data.targetGuid.GetEntry())}});
    Planner::QuestExec::Remember(data.passed, data.passedNext, data.targetGuid);
    ClearTarget(data);
}

void NewRpgDoQuestAction::LeavePlace(NewRpgInfo::DoQuest& data)
{
    ClearTarget(data);
    data.lastReachPOI = 0;
    data.pos = WorldPosition();
    data.objectiveIdx = 0;
}

// Ported/adapted from mod-playerbots PR #2632 (head d4e5d110): the targeted attack
Unit* NewRpgAttackQuestTargetAction::GetTarget()
{
    auto* data = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!data || !data->targetGuid.IsAnyTypeCreature())
        return nullptr;

    return botAI->GetUnit(data->targetGuid);
}

bool NewRpgAttackQuestTargetAction::Execute(Event /*event*/)
{
    verbose = false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return false;

    if (bot->IsFriendlyTo(target) || !bot->IsValidAttackTarget(target))
        return false;

    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("current target")->Set(target);

    // Halt even when the attack is deferred: the in-flight spline ends on the mob itself.
    LastMovement& lastMovement = AI_VALUE(LastMovement&, "last movement");
    bool moveControlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
    if (bot->isMoving() && lastMovement.priority < MovementPriority::MOVEMENT_COMBAT && !moveControlled)
    {
        lastMovement.clear();
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
    }

    if (bot->IsMounted())
    {
        bot->Dismount();
        return true;
    }

    bool result = Attack(target);
    LOG_DEBUG("playerbots", "[New RPG] {} attack quest target {} ({}): {}", bot->GetName(), target->GetName(),
              target->GetGUID().ToString(), result ? "engaged" : "refused");
    return result;
}

bool NewRpgInternal::EndQuestAtStart(PlayerbotAI* botAI, Player* bot)
{
    auto const* data = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!data || bot->GetQuestStatus(data->questId) != QUEST_STATUS_INCOMPLETE)
        return false;
    Planner::QuestExec::Step step;
    QuestDestination place{WorldPosition(), 0, 0.0f};
    if (!NextQuestStep(botAI, bot, *data, step, place))
        return false;  // the index is still building: the first tick decides
    char const* const word = Planner::QuestExec::EndWord(step);
    if (!word)
        return false;
    RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = word});
    return true;
}

std::vector<uint32> NewRpgInternal::GivenUpQuests(Player* bot)
{
    return QuestFailRecord::GivenUpQuests(bot->GetGUID().GetCounter(), Planner::ClassQuests::GIVE_UP_LIMIT);
}
