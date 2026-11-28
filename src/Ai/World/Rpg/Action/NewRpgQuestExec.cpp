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
#include "NewRpgSoughtTarget.h"
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
#include "QuestFieldExec.h"
#include "QuestGiveUp.h"
#include "QuestLogRules.h"
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
// The time a bot takes to choose its next target (questing WQ55)
constexpr uint32 TARGET_THINK_MIN_MS = 400;
constexpr uint32 TARGET_THINK_MAX_MS = 1000;
constexpr uint8 TRACED_ATTACK = 1;
constexpr uint8 TRACED_WALK = 2;

// A quest_exec decision row (world-log.md): ref is the quest
void Trace(Player* bot, std::string_view choice, std::string_view reason, float value, uint32 questId,
           WorldLogFormat::Alternatives const& alternatives = {})
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, choice, reason, value, alternatives, questId);
    // A transition of the executor: its next step in the same check when this one took no time (questing WQ55)
    if (PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot))
        botAI->ChainOn();
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
                 Planner::QuestExec::ObjectiveAt(*record, step.objective).poiIndex,
                 Planner::QuestPlaces::Roam(*step.place)};
    step.place = nullptr;  // it points into this index snapshot
    return true;
}

// WT35g: the chain after the step's use (QuestExecRules.h): the creatures its kill step holds (KillsAfterUse) and
// the creature its talk step talks to (TalksAfterUse, 0: none)
struct UseChain
{
    std::vector<uint32> foes;
    uint32 talker{0};
};

UseChain ChainOf(NewRpgInfo::DoQuest const& data)
{
    UseChain chain;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    if (!record || objective >= Planner::QuestExec::ObjectiveCount(*record))
        return chain;
    Planner::QuestObjective const& work = Planner::QuestExec::ObjectiveAt(*record, objective);
    if (Planner::QuestExec::KillsAfterUse(work))
        chain.foes = Planner::QuestExec::KillEntries(work);
    if (Planner::QuestExec::TalksAfterUse(work))
        chain.talker = work.talk.creature;
    return chain;
}

// WT35h: a stalled walk to the turn-in's ender tries it only while another usable ender is left (QuestExecRules.h
// StallTries); without the index the ender is tried, as before
bool EnderStallTries(Player* bot, NewRpgInfo::DoQuest const& data)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    return !record || Planner::QuestExec::StallTries(*record, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                                     static_cast<uint8>(bot->GetTeamId()), data.tried, data.place);
}

// questing WQ19a: whether the need an order picked last is still open in the log (its quest incomplete, its objective's
// count not met)
bool PickOpen(Player* bot, NewRpgInfo::DoQuest const& data)
{
    auto const it = bot->getQuestStatusMap().find(data.questId);
    return data.quest && it != bot->getQuestStatusMap().end() &&
           bot->GetQuestStatus(data.questId) == QUEST_STATUS_INCOMPLETE &&
           SlotOpen(data.quest, it->second, data.objectiveIdx);
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
        {
            // WQ6d trace: a talk hold whose option was picked ends here when the log closed the objective
            if (data.selected)
            {
                bool const inLog = it != bot->getQuestStatusMap().end();
                Trace(bot, "talk_end", "closed", GetMSTimeDiffToNow(data.actedAt) / 1000.0f, questId,
                      {{"explored", inLog ? (it->second.Explored ? 1.0f : 0.0f) : -1.0f},
                       {"status", static_cast<float>(bot->GetQuestStatus(questId))}});
            }
            LeavePlace(data);
        }
    }
    if (data.pos == WorldPosition())
    {
        Planner::QuestExec::Step step;
        QuestDestination place{WorldPosition(), 0, 0.0f};
        if (!NextQuestStep(botAI, bot, data, step, place))
            return false;
        // Refused (the start already asked: PLAN 3.2), no open objective with a place on this map (PLAN 3.1 rule 6),
        // or every place tried without a target (locked when the run passed over a container it could not open)
        if (char const* const word = Planner::QuestExec::EndWord(step, data.lockedSeen))
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

    // An area trigger is walked into, no target sought (WT35f)
    if (data.work == static_cast<uint8>(Work::Explore))
        return EngageTrigger(data);

    // Kill and loot: the bot seeks the objective's creature (a loot objective's: its item's sources) within sight,
    // on the way and at the place, and holds the nearest; object and chest the same for gameobjects (WT35e), talk
    // for the creature it talks to (WT35f)
    bool const objects = data.work == static_cast<uint8>(Work::Object) || data.work == static_cast<uint8>(Work::Chest);
    bool const talk = data.work == static_cast<uint8>(Work::Talk);
    // WT35b: a quest item at a use rule's target, or a class spell cast on it
    bool const uses = data.work == static_cast<uint8>(Work::Use) || data.work == static_cast<uint8>(Work::Cast);
    bool const seeks = objects || talk || uses || data.work == static_cast<uint8>(Work::Kill) ||
                       data.work == static_cast<uint8>(Work::Loot);
    if (seeks)
    {
        if (!data.targetGuid.IsEmpty())
            return EngageHeld(data);
        if (!data.lastScan || GetMSTimeDiffToNow(data.lastScan) >= QUEST_SCAN_MS)
        {
            data.lastScan = getMSTime();
            if (SeekHeld(data))
                return true;
        }
    }

    if (!data.lastReachPOI)
    {
        float const yards = bot->GetDistance(data.pos);
        if (yards > QUEST_ARRIVE_YARDS)
            return WalkOn(data, data.pos, yards, false, data.place, QUEST_ARRIVE_YARDS);
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

bool NewRpgDoQuestAction::EngageHeld(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::Work;
    bool const objects = data.work == static_cast<uint8>(Work::Object) || data.work == static_cast<uint8>(Work::Chest);
    bool const talk = data.work == static_cast<uint8>(Work::Talk);
    bool const uses = data.work == static_cast<uint8>(Work::Use) || data.work == static_cast<uint8>(Work::Cast);
    // WT35g: use, then kill what the use brought forth, then talk to what the kill brought forth: a held creature
    // of the chain is its step's (the current entry: a transformed target keeps its guid)
    UseChain const chain = uses ? ChainOf(data) : UseChain{};
    if ((chain.talker || !chain.foes.empty()) && data.targetGuid.IsCreatureOrVehicle())
    {
        Unit const* held = botAI->GetUnit(data.targetGuid);
        uint32 const entry = held ? held->GetEntry() : data.targetGuid.GetEntry();
        if (chain.talker && entry == chain.talker)
            return EngageTalk(data);
        if (std::find(chain.foes.begin(), chain.foes.end(), entry) != chain.foes.end())
            return EngageTarget(data);
    }
    return objects ? EngageObject(data) : (talk ? EngageTalk(data) : (uses ? EngageUse(data) : EngageTarget(data)));
}

bool NewRpgDoQuestAction::SeekHeld(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::Work;
    bool const objects = data.work == static_cast<uint8>(Work::Object) || data.work == static_cast<uint8>(Work::Chest);
    bool const talk = data.work == static_cast<uint8>(Work::Talk);
    bool const uses = data.work == static_cast<uint8>(Work::Use) || data.work == static_cast<uint8>(Work::Cast);
    if (objects)
        return SeekObject(data, QUEST_SCAN_YARDS);
    if (talk)
        return SeekTalk(data, QUEST_SCAN_YARDS);
    if (uses)
    {
        UseChain const chain = ChainOf(data);
        return (chain.talker && SeekTalk(data, QUEST_SCAN_YARDS)) ||
               (!chain.foes.empty() && SeekTarget(data, QUEST_SCAN_YARDS)) || SeekUse(data, QUEST_SCAN_YARDS);
    }
    if (data.work == static_cast<uint8>(Work::Kill) || data.work == static_cast<uint8>(Work::Loot))
    {
        if (!SeekTarget(data, QUEST_SCAN_YARDS))
            return false;
        // Choosing the next target takes a moment (questing WQ55): the bot goes for it at its next check, exactly then
        botAI->SetThinkDelay(urand(TARGET_THINK_MIN_MS, TARGET_THINK_MAX_MS));
        return true;
    }
    return false;
}

namespace
{
// Whether a ready quest's hand-in fails for the bags alone (questing WQ18b): the game would reward it but for the room
// its reward needs (CanRewardQuest with a reward). Since WQ43 the turn-in makes room first (MakeRoomForHandIn), so at
// the order's end this holds only when the stacks the bot may destroy could not cover the reward
bool BagsRefuseHandIn(Player* bot, uint32 questId)
{
    Quest const* const quest = sObjectMgr->GetQuestTemplate(questId);
    return quest && bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, false) &&
           !bot->CanRewardQuest(quest, 0, false);
}

// questing WQ20: the order's ask of the bot: where it stands, its level, and whether it walks to the place (before the
// arrival the way's allowance applies, QuestFieldExec::OnOrder)
Planner::QuestFieldExec::PickAsk OrderAsk(Player const* bot, NewRpgInfo::DoQuest const& data)
{
    Planner::QuestFieldExec::PickAsk ask{bot->GetPositionX(), bot->GetPositionY(), static_cast<uint8>(bot->GetLevel()),
                                         Planner::QuestFieldExec::NEUTRAL_CAUTION};
    ask.onWay = data.order && !data.order->arrivedAt;
    return ask;
}
}  // namespace

bool NewRpgDoQuestAction::OnOrder(NewRpgInfo::DoQuest const& data, WorldObject const* target) const
{
    return !data.order || Planner::QuestFieldExec::OnOrder(data.order->order, OrderAsk(bot, data), target->GetMapId(),
                                                           target->GetPositionX(), target->GetPositionY());
}

bool NewRpgDoQuestAction::DoOrder(NewRpgInfo::DoQuest& data)
{
    Planner::QuestOrderRun& run = *data.order;
    uint32 const now = getMSTime();
    // The held target's objective closed (its count met, its quest complete or gone): it is let go
    if (!data.targetGuid.IsEmpty() && !PickOpen(bot, data))
        ClearTarget(data);
    // A held target is work found: the dry clock waits while the bot works one
    if (!data.targetGuid.IsEmpty())
        run.lastFound = Planner::QuestFieldExec::Stamp(now);
    if (!run.arrivedAt && bot->GetDistance(data.pos) <= QUEST_ARRIVE_YARDS)
    {
        ResetWalk(data);
        run.arrivedAt = Planner::QuestFieldExec::Stamp(now);
        Trace(bot, "place_reached", "order", bot->GetDistance(data.pos), data.questId,
              {{"open", static_cast<float>(run.open.size())}, {"credits", static_cast<float>(run.wayCredits)}});
    }
    // Questing WQ43 (L4): a bot still closing on its aim, between its fights, restarts the walk's bound
    Planner::QuestFieldExec::NoteCloser(run, bot->GetDistance(data.pos), now);
    // The order's own end (QuestFieldExec::End); of its ends only no_ender writes failure rows, and a hand-in the
    // bags refuse is none: the end names the bags (NameEnd, questing WQ18b)
    if (std::optional<Planner::QuestOrderEnd> const found =
            Planner::QuestFieldExec::End(run, now, !data.targetGuid.IsEmpty()))
    {
        // nor is one whose ender stands elsewhere in the field (part 4): no_ender is said at the ender
        std::function<bool(uint32)> elsewhere = [](uint32) { return false; };
        if (*found == Planner::QuestOrderEnd::NoEnder)
            elsewhere = NewRpgInternal::OrderEnderElsewhere(bot, run.order);
        Planner::QuestOrderEnd const end = Planner::QuestFieldExec::NameEnd(
            run, *found, [this](uint32 questId) { return BagsRefuseHandIn(bot, questId); }, elsewhere);
        ClearTarget(data);
        RpgStatus::End(botAI, Planner::QuestFieldExec::OutcomeOf(end), {.word = Planner::QuestOrderEndName(end)});
        return true;
    }
    if (!data.targetGuid.IsEmpty())
        return EngageHeld(data);
    if (!data.lastScan || GetMSTimeDiffToNow(data.lastScan) >= QUEST_SCAN_MS)
    {
        data.lastScan = getMSTime();
        if (LookOrder(data))
        {
            // The picked need's seek can end the status (a use that really fails): the order is gone then
            if (auto* const doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data); doing && doing->order)
                doing->order->lastFound = Planner::QuestFieldExec::Stamp(getMSTime());
            return true;
        }
    }
    // To the place (travel's walk), then round it: the field's targets come into sight
    if (!run.arrivedAt)
        return WalkOn(data, data.pos, bot->GetDistance(data.pos), false, 0, QUEST_ARRIVE_YARDS);
    return RoamInPlace(data.pos, data.roam);
}

bool NewRpgDoQuestAction::LookOrder(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestFieldExec::Candidate;
    Planner::QuestOrderRun const& run = *data.order;
    if (run.seeks.empty())
        return false;
    // One look per kind over the entries of every open need
    std::vector<uint32> creatureEntries;
    std::vector<uint32> objectEntries;
    for (Planner::QuestOrderSeek const& seek : run.seeks)
        (seek.object ? objectEntries : creatureEntries).push_back(seek.entry);
    for (std::vector<uint32>* entries : {&creatureEntries, &objectEntries})
    {
        std::sort(entries->begin(), entries->end());
        entries->erase(std::unique(entries->begin(), entries->end()), entries->end());
    }
    float const range = std::min(QUEST_SCAN_YARDS, sPlayerbotAIConfig.SightDistance);
    uint32 const mapId = bot->GetMapId();
    std::vector<Candidate> candidates;
    auto const add = [&run, &candidates, mapId](uint32 entry, bool object, uint8 level, float x, float y)
    {
        for (Planner::QuestOrderSeek const& seek : run.seeks)
            if (seek.entry == entry && seek.object == object)
                candidates.push_back({seek.need, object ? Planner::FieldKind::Object : Planner::FieldKind::Creature,
                                      level, mapId, x, y});
    };
    if (!creatureEntries.empty())
    {
        std::list<Creature*> creatures;
        bot->GetCreatureListWithEntryInGrid(creatures, creatureEntries, range);
        for (Creature* creature : creatures)
            if (creature->IsInWorld() && creature->IsAlive() &&
                !Planner::QuestExec::Remembered(data.passed, creature->GetGUID()) &&
                !(creature->hasLootRecipient() && !creature->isTappedBy(bot)))
                add(creature->GetEntry(), false, creature->GetLevel(), creature->GetPositionX(),
                    creature->GetPositionY());
    }
    if (!objectEntries.empty())
    {
        std::list<GameObject*> objects;
        bot->GetGameObjectListWithEntryInGrid(objects, objectEntries, range);
        for (GameObject* go : objects)
            if (go->isSpawned() && !Planner::QuestExec::Remembered(data.passed, go->GetGUID()))
                add(go->GetEntry(), true, 0, go->GetPositionX(), go->GetPositionY());
    }
    Planner::QuestFieldExec::PickAsk const ask = OrderAsk(bot, data);
    // The nearest candidate's need becomes the executor's quest and objective, and that objective's own seek holds the
    // target (its checks are the one-quest executor's); a need whose targets that seek refuses is left for this look
    while (std::optional<std::size_t> const at = Planner::QuestFieldExec::Pick(candidates, run.open, run.order, ask))
    {
        uint32 const need = candidates[*at].need;
        if (PickNeed(data, need) && SeekHeld(data))
            return true;
        std::erase_if(candidates, [need](Candidate const& candidate) { return candidate.need == need; });
    }
    return false;
}

bool NewRpgDoQuestAction::PickNeed(NewRpgInfo::DoQuest& data, uint32 const need)
{
    Planner::QuestOrderRun const& run = *data.order;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index || need >= run.open.size())
        return false;
    Planner::QuestOrderOpen const& open = run.open[need];
    Planner::QuestRecord const* record = index->Find(open.quest);
    Quest const* quest = sObjectMgr->GetQuestTemplate(open.quest);
    if (!record || !quest || open.objective >= Planner::QuestExec::ObjectiveCount(*record))
        return false;
    // As the step function maps an objective (NextQuestStep): its log slot, its work, its place key (area 0: the
    // order's place is the field, not one of the objective's areas)
    Planner::QuestObjective const& objective = Planner::QuestExec::ObjectiveAt(*record, open.objective);
    data.quest = quest;
    data.questId = open.quest;
    data.objectiveIdx = objective.poiIndex;
    data.work = static_cast<uint8>(Planner::QuestExec::WorkOf(objective, static_cast<uint8>(bot->GetTeamId())));
    data.place = Planner::QuestExec::Key(open.objective, 0);
    return true;
}

bool NewRpgDoQuestAction::ValidTarget(NewRpgInfo::DoQuest const& data, Creature* creature) const
{
    // Open (alive, not tapped by another: its kill would bring no credit, not friendly) and not passed over
    if (!SoughtTarget::Open(bot, creature) || Planner::QuestExec::Remembered(data.passed, creature->GetGUID()))
        return false;
    // WT35g: what a use brought forth is killed when it is hostile; a neutral one the use made (a cured beast) is left
    if ((data.work == static_cast<uint8>(Planner::QuestExec::Work::Use) ||
         data.work == static_cast<uint8>(Planner::QuestExec::Work::Cast)) &&
        !bot->IsHostileTo(creature))
        return false;
    // questing WQ19a: on an order a creature too far above the bot is left (QuestFieldExec's reach rule)
    if (data.order && !Planner::QuestFieldExec::LevelWithinReach(creature->GetLevel(), bot->GetLevel()))
        return false;
    // questing WQ20: on an order a creature the order does not take (off the field and, on the way, beyond the
    // allowance) is left, though nearer than the one the look picked
    if (!OnOrder(data, creature))
        return false;
    // A test's nofight bot is immune to NPCs, so it can attack none: it still holds the objective's creature and its
    // attack is refused, so the order runs without progress (the give-up case, WT35c) instead of finding no target
    return !TestConstraints::StartsFights(botAI->testConstraints) || bot->IsValidAttackTarget(creature);
}

bool NewRpgDoQuestAction::SeekTarget(NewRpgInfo::DoQuest& data, float maxYards)
{
    Quest const* quest = data.quest;
    int32 const slot = data.objectiveIdx;
    // A loot objective: the creatures its item drops from (QuestIndex.h SeekEntries); a kill or a chain's kill step:
    // KillEntries (WT35g: the creatures whose kill credits an unspawned objective, what an event's use summons)
    std::vector<uint32> entries;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    if (record && objective < Planner::QuestExec::ObjectiveCount(*record))
    {
        Planner::QuestObjective const& work = Planner::QuestExec::ObjectiveAt(*record, objective);
        entries = data.work == static_cast<uint8>(Planner::QuestExec::Work::Loot)
                      ? work.seek
                      : Planner::QuestExec::KillEntries(work);
    }
    if (entries.empty() && quest && slot >= 0 && slot < QUEST_OBJECTIVES_COUNT && quest->RequiredNpcOrGo[slot] > 0)
        entries.push_back(static_cast<uint32>(quest->RequiredNpcOrGo[slot]));
    if (entries.empty())
        return false;

    float const range = std::min({maxYards, QUEST_SCAN_YARDS, sPlayerbotAIConfig.SightDistance});
    std::size_t candidates = 0;
    Creature* nearest = SoughtTarget::Nearest(
        bot, entries, range, [this, &data](Creature* creature) { return ValidTarget(data, creature); }, candidates);
    if (!nearest)
        return false;
    float const nearestYards = bot->GetDistance(nearest);

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
          {{"entry", static_cast<float>(nearest->GetEntry())}, {"candidates", static_cast<float>(candidates)}});
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
        return WalkOn(data, data.targetPos, yards, true, creature->GetEntry(), INTERACTION_DISTANCE);
    ResetWalk(data);
    bool const attacked = botAI->DoSpecificAction("new rpg attack quest target", Event(), true);
    if (attacked)
        StandWorked(data);
    if (!attacked && !(data.traced & TRACED_ATTACK))
    {
        data.traced |= TRACED_ATTACK;
        Trace(bot, "attack_refused", bot->IsMounted() ? "mounted" : "attack", yards, data.questId,
              {{"entry", static_cast<float>(creature->GetEntry())}});
    }
    return attacked;
}

bool NewRpgDoQuestAction::WalkOn(NewRpgInfo::DoQuest& data, WorldPosition const dest, float const yards,
                                 bool const toTarget, uint32 const id, float const reach)
{
    uint32 const questId = data.questId;
    bool const ender = !toTarget && bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;  // WT35h: the turn-in's
    char const* const kind = toTarget ? "target" : (ender ? "ender" : "place");
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
    // GoTo's own watch does not count a pause (its local piece's gap) and is longer than the executor's, which gives a
    // stalled walk up first
    if (Planner::QuestExec::WalkStalled(data.walkNearest, data.walkStalledMs, first, sinceLast, yards))
    {
        Trace(bot, "walk_stall", kind, yards, questId, where);
        if (toTarget)
            PassOver(data, "walk");
        else if (data.order || (ender ? !EnderStallTries(bot, data) : !StallTriesPlace(data)))
        {
            // The last usable ender or the objective's only place left: the walk starts again from here (a new walk
            // row and watch; GoTo's walk goes on); the order's give-up clock (TickPursuit) bounds it. An order's place
            // (questing WQ19a) is its only one: its own end (QuestFieldExec::End, `time`) bounds the walk
            ResetWalk(data);
        }
        else
        {
            Trace(bot, "place_tried", "walk", yards, questId,
                  {{"place", static_cast<float>(data.place)}, {"tried", static_cast<float>(data.tried.size() + 1)}});
            data.tried.push_back(data.place);
            LeavePlace(data);
            if (ender)
                data.objectiveIdx = -1;  // the turn-in goes on at the next ender (DoCompletedQuest)
        }
        return true;
    }
    if (toTarget)
    {
        // The held target's approach: a stand point the mesh does not carry (or the target gone) passes the target
        // over, as a stalled walk does; no other move is tried
        if (Approach(data.targetGuid, reach) == ::GoTo::Answer::Failed)
        {
            if (!(data.traced & TRACED_WALK))
            {
                data.traced |= TRACED_WALK;
                Trace(bot, "walk_refused", kind, yards, questId, where);
            }
            // The pass-over's inputs (WT68 fix, talk-q9663's second giver: no approach line before it): the reach, the
            // answer, the giver, and the record of failed attempts, whose refusal writes no line of its own
            LOG_DEBUG("playerbots",
                      "[New RPG] {} approach to {} (entry {}) for quest {}: answer Failed, reach {:.1f} yd, {:.0f} yd "
                      "away, failed-attempt record of {} entries",
                      bot->GetName(), data.targetGuid.ToString(), id, questId, reach, yards,
                      botAI->GetTravel().Failures().count);
            PassOver(data, "walk");
        }
        return true;
    }
    std::optional<bool> const away = Away(dest, reach);
    // GoTo's failed end ended the order (its Stuck row, the place refused, the status's typed end): data is gone, the
    // locals stay
    if (!std::holds_alternative<NewRpgInfo::DoQuest>(botAI->rpgInfo.data))
    {
        Trace(bot, "stuck", kind, yards, questId, where);
        return true;
    }
    return away.value_or(true);
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
    data.actedAt = 0;
    data.selected = false;
    data.useTries = 0;
    data.useAskedAt = 0;
    data.usePending = false;
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
    // questing WQ33: the walk's pass-over is a refused stand point or a stalled walk; a lone quest's third ends its
    // status, and data with it (the callers return at once)
    if (std::string_view(reason) == "walk")
        StandRefusal(data);
}

namespace
{
// The order's open need the executor works (PickNeed's quest and objective); open.size() when none is
std::size_t WorkedNeed(NewRpgInfo::DoQuest const& data)
{
    std::vector<Planner::QuestOrderOpen> const& open = data.order->open;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    for (std::size_t at = 0; at < open.size(); ++at)
        if (open[at].quest == data.questId && open[at].objective == objective)
            return at;
    return open.size();
}
}  // namespace

void NewRpgDoQuestAction::StandRefusal(NewRpgInfo::DoQuest& data)
{
    namespace Field = Planner::QuestFieldExec;
    uint32 const questId = data.questId;
    if (!data.order)
    {
        // A lone quest has one need: its third refusal gives the quest up (the status door writes the failure row)
        if (!Field::StandRefused(data.standRefusals))
            return;
        Trace(bot, "need_closed", Field::NO_STAND, static_cast<float>(data.standRefusals), questId);
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = Field::NO_STAND});
        return;
    }
    // On an order the need closes and the order goes on with its other needs (QuestFieldExec::Takes leaves the closed
    // one); its quest's failure row is written once (an order's end writes none for it)
    std::vector<Planner::QuestOrderOpen>& open = data.order->open;
    std::size_t const need = WorkedNeed(data);
    if (need >= open.size() || Field::StandClosed(open[need].refusals))
        return;
    bool const report = Field::RefuseNeed(open, need);
    if (!Field::StandClosed(open[need].refusals))
        return;
    Trace(bot, "need_closed", Field::NO_STAND, static_cast<float>(open[need].refusals), questId,
          {{"objective", static_cast<float>(open[need].objective)}, {"left", static_cast<float>(open[need].left)}});
    if (report)
        QuestGiveUp::Noted(botAI, data, Field::NO_STAND);
}

void NewRpgDoQuestAction::StandWorked(NewRpgInfo::DoQuest& data)
{
    if (!data.order)
    {
        Planner::QuestFieldExec::StandWorked(data.standRefusals);
        return;
    }
    std::size_t const need = WorkedNeed(data);
    if (need < data.order->open.size())
        Planner::QuestFieldExec::StandWorked(data.order->open[need].refusals);
}

void NewRpgDoQuestAction::LeavePlace(NewRpgInfo::DoQuest& data)
{
    ClearTarget(data);
    data.lastReachPOI = 0;
    data.pos = WorldPosition();
    data.objectiveIdx = 0;
}

// Ported/adapted from mod-playerbots PR #2632 (head d4e5d110): the targeted attack, of the quest status's target or
// the gather status's (professions WR20c)
Unit* NewRpgAttackQuestTargetAction::GetTarget()
{
    ObjectGuid const target = SoughtTarget::Held(botAI->rpgInfo);
    if (!target.IsAnyTypeCreature())
        return nullptr;

    return botAI->GetUnit(target);
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
    return Planner::QuestLog::Read(QuestFailRecord::RowsOf(bot->GetGUID().GetCounter())).givenUp;
}
