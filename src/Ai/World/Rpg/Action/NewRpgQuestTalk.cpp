/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <list>
#include <memory>
#include <string_view>
#include <vector>

#include "Creature.h"
#include "GossipDef.h"
#include "Log.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestIndex.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

// WT35f: the creatures a quest's objective asks the bot to talk to. The gossip is opened through the session's
// handler (CMSG_GOSSIP_HELLO), and where the credit sits on an option, the option the gather tied to the quest is
// picked through its handler (CMSG_GOSSIP_SELECT_OPTION). The log says whether it counted. Ported/adapted from
// mod-playerbots PR #2632 (head d4e5d110): its talk branch, without its direct credit call and its timers (the
// passed ring and the hold do that)
namespace
{
constexpr float QUEST_TALK_REACH_YARDS = INTERACTION_DISTANCE - 2.0f;  // within this and in sight: the hello
constexpr uint32 QUEST_TALK_HOLD_MS = 2 * IN_MILLISECONDS;             // after the hello or the select: the credit

// A quest_exec decision row (world-log.md): ref is the quest
void Trace(Player* bot, std::string_view choice, std::string_view reason, float value, uint32 questId,
           WorldLogFormat::Alternatives const& alternatives = {})
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, choice, reason, value, alternatives, questId);
}

// The talk credit the index holds for the objective being worked (TalkHow::None when it holds none)
Planner::TalkCredit TalkOfPlace(NewRpgInfo::DoQuest const& data)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    return record && objective < record->objectives.size() ? record->objectives[objective].talk : Planner::TalkCredit{};
}

Planner::QuestExec::TalkSeen See(Player* bot, NewRpgInfo::DoQuest const& data, Creature* creature,
                                 Planner::TalkCredit const& talk)
{
    Planner::QuestExec::TalkSeen seen;
    seen.select = talk.how == Planner::TalkHow::Select;
    seen.helloSent = data.actedAt != 0;
    seen.selected = data.selected;
    seen.holdOver = data.actedAt && GetMSTimeDiffToNow(data.actedAt) >= QUEST_TALK_HOLD_MS;
    if (seen.helloSent && seen.select)
    {
        // The menu the hello opened: a condition (the quest taken, an item) may hide the option
        GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
        seen.optionShown =
            menu.GetSenderGUID() == data.targetGuid && menu.GetMenuId() == talk.menu && menu.GetItem(talk.option);
    }
    if (!creature || !creature->IsInWorld() || !creature->IsAlive())
        return seen;
    seen.present = true;
    seen.passed = Planner::QuestExec::Remembered(data.passed, creature->GetGUID());
    seen.usable =
        creature->GetNpcFlags() != UNIT_NPC_FLAG_NONE && !bot->IsHostileTo(creature) && !creature->IsInCombat();
    seen.inReach = bot->GetDistance(creature) <= QUEST_TALK_REACH_YARDS && bot->IsWithinLOSInMap(creature);
    return seen;
}
}  // namespace

bool NewRpgDoQuestAction::SeekTalk(NewRpgInfo::DoQuest& data, float maxYards)
{
    Quest const* quest = data.quest;
    int32 const slot = data.objectiveIdx;
    if (!quest || slot < 0 || slot >= QUEST_OBJECTIVES_COUNT || quest->RequiredNpcOrGo[slot] <= 0)
        return false;
    Planner::TalkCredit const talk = TalkOfPlace(data);

    float const range = std::min(maxYards, sPlayerbotAIConfig.SightDistance);
    std::list<Creature*> creatures;
    bot->GetCreatureListWithEntryInGrid(creatures, static_cast<uint32>(quest->RequiredNpcOrGo[slot]), range);
    Creature* nearest = nullptr;
    float nearestYards = range;
    for (Creature* creature : creatures)
    {
        Planner::QuestExec::TalkAct const act = Planner::QuestExec::TalkVerdict(See(bot, data, creature, talk));
        // One busy (a script's phase, a fight) is waited for: the look round of the place is held while it is in sight
        if (act == Planner::QuestExec::TalkAct::NoInteract)
            data.lastEngage = getMSTime();
        if (act != Planner::QuestExec::TalkAct::Walk && act != Planner::QuestExec::TalkAct::Hello)
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
    data.actedAt = 0;
    data.selected = false;
    LOG_DEBUG("playerbots", "[New RPG] {} holds talk creature {} ({} yd) for quest {} objective {}", bot->GetName(),
              data.targetGuid.ToString(), nearestYards, data.questId, data.objectiveIdx);
    Trace(bot, "hold", "talk", nearestYards, data.questId,
          {{"entry", static_cast<float>(nearest->GetEntry())}, {"candidates", static_cast<float>(creatures.size())}});
    return true;
}

bool NewRpgDoQuestAction::EngageTalk(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::TalkAct;
    // A fight holds the engagement; the combat actions fight. The pursuit clock bounds the walk to the creature
    if (bot->IsInCombat())
        return false;
    uint32 const sinceLast = data.lastEngage ? GetMSTimeDiffToNow(data.lastEngage) : 0;
    Planner::QuestExec::Pursue(data.engagedMs, data.ticking, false, sinceLast);
    data.lastEngage = getMSTime();

    Unit* unit = botAI->GetUnit(data.targetGuid);
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    Planner::TalkCredit const talk = TalkOfPlace(data);
    TalkAct const act = Planner::QuestExec::TalkVerdict(See(bot, data, creature, talk));
    float const entry = static_cast<float>(data.targetGuid.GetEntry());
    switch (act)
    {
        case TalkAct::Hold:
            return true;
        case TalkAct::Done:
            Trace(bot, "talk_done", data.selected ? "select" : "hello", GetMSTimeDiffToNow(data.actedAt) / 1000.0f,
                  data.questId, {{"entry", entry}});
            bot->PlayerTalkClass->SendCloseGossip();
            Planner::QuestExec::Remember(data.passed, data.passedNext, data.targetGuid);
            ClearTarget(data);
            return true;
        case TalkAct::NoOption:
            bot->PlayerTalkClass->SendCloseGossip();
            PassOver(data, "no_option");
            return true;
        case TalkAct::Select:
        {
            // The client's select: the open menu, the option's list id; no code (a coded option is no credit row)
            WorldPacket packet(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4);
            packet << data.targetGuid << talk.menu << talk.option;
            bot->GetSession()->HandleGossipSelectOptionOpcode(packet);
            data.selected = true;
            data.actedAt = getMSTime();
            Trace(bot, "talk_used", "select", creature ? bot->GetDistance(creature) : 0.0f, data.questId,
                  {{"entry", entry},
                   {"menu", static_cast<float>(talk.menu)},
                   {"option", static_cast<float>(talk.option)}});
            return true;
        }
        case TalkAct::Skip:
        case TalkAct::NoInteract:
            // Gone or busy: not remembered, so the seek takes it again once it is back or free
            Trace(bot, "pass_over", act == TalkAct::Skip ? "gone" : "no_interact", data.engagedMs / 1000.0f,
                  data.questId, {{"entry", entry}});
            ClearTarget(data);
            return true;
        default:
            break;
    }
    if (Planner::QuestExec::PursuitOver(data.engagedMs))
    {
        PassOver(data, "clock");
        return true;
    }
    float const yards = bot->GetDistance(creature);
    if (act == TalkAct::Walk)
    {
        data.targetPos = WorldPosition(bot->GetMapId(), creature->GetPositionX(), creature->GetPositionY(),
                                       creature->GetPositionZ());
        return WalkOn(data, data.targetPos, yards, true, creature->GetEntry(), QUEST_TALK_REACH_YARDS);
    }

    // The hello as a client sends it on a click: the handler opens the gossip and runs the creature's hello script
    ResetWalk(data);
    if (bot->isMoving())
        bot->StopMoving();
    bot->SetFacingToObject(creature);
    WorldPacket packet(CMSG_GOSSIP_HELLO, 8);
    packet << data.targetGuid;
    bot->GetSession()->HandleGossipHelloOpcode(packet);
    data.actedAt = getMSTime();
    LOG_DEBUG("playerbots", "[New RPG] {} talks to {} for quest {}", bot->GetName(), creature->GetEntry(),
              data.questId);
    Trace(bot, "talk_used", "hello", yards, data.questId, {{"entry", entry}});
    return true;
}
