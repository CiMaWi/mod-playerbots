/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGHELPERS_H
#define PLAYERBOTS_NEWRPGHELPERS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ObjectGuid.h"

class Object;
class Player;
class PlayerbotAI;
class Quest;
class WorldObject;

namespace Planner
{
struct QuestOrderRun;
namespace QuestLog
{
struct Held;
struct Record;
}  // namespace QuestLog
}  // namespace Planner

// Quest checks shared by the new RPG actions and values. They encode new RPG questing policy
// (solo, non-repeatable, level window), not general quest rules.
namespace NewRpgHelpers
{
// Radius within which a quest giver with something to accept or reward pulls the bot over.
constexpr float QUEST_GIVER_SEARCH_DISTANCE = 80.0f;

// Questing WQ31: the bot's quest executor runs an order (NewRpgInfo::DoQuest::order). O(1)
bool OnOrder(PlayerbotAI* botAI);

// Questing WQ34 (WQ.46): a bot the quest log's one accept and one drop cover (QuestLogRules.h): on an order, or a
// planner bot (Planner::Owns). Any other bot (it keeps no possessions, or a test holds its planner) keeps upstream's
// accept and drop (WQ.39). O(1)
bool KeepsLogRule(PlayerbotAI* botAI);
// A holiday's quest (the game's seasonal relation of a game event) whose event is not running. The active events
// change mostly on the world thread (World::Update), but SmartScript actions and Sun's Reach's states start and stop
// some from map threads, so this read can meet such a change, as every map-thread caller of IsActiveEvent can (the
// game's own quest checks among them); a holiday ends or starts days apart, so a stale answer costs one early or late
// drop at most. O(log events)
bool HolidayEnded(Quest const* quest);
// The quest of the bot's log as the one drop reads it (QuestLogRules' Abandon). Map thread
Planner::QuestLog::Held LogHeld(Player* bot, Quest const* quest);

// Questing WQ18b: the failure record of a bot on an order as the accept and the log upkeep read it (QuestLogRules'
// Read), kept by the order's run and read again only after a write to the record (QuestFail::Reread): the executor's
// tick copies no rows and takes no lock. The second form finds the bot's running order; a bot on none reads anew.
// Map thread
std::shared_ptr<Planner::QuestLog::Record const> OrderRecord(Player* bot, Planner::QuestOrderRun& run);
std::shared_ptr<Planner::QuestLog::Record const> OrderRecord(PlayerbotAI* botAI, Player* bot);

// Questing WQ34 (WQ.46): whether a bot KeepsLogRule covers takes a quest, QuestLogRules' Accepts with the game's
// answer for that quest (CanTakeQuest, WQ41) and its holiday. The facts (the grey level, the failure record) are read
// at the first quest that asks and kept for the one search or menu; an accept changes none of them. Map thread
class LogAccept
{
public:
    explicit LogAccept(Player* bot) : _bot(bot) {}
    LogAccept(LogAccept const&) = delete;
    LogAccept& operator=(LogAccept const&) = delete;

    bool Allows(Quest const* quest);

private:
    Player* _bot;
    bool _built{false};
    std::int32_t _grayLevel{0};
    std::vector<std::uint32_t> _givenUp;
};

// The one accept for a single quest outside a giver's menu (a start item, a shared quest, a party's escort, the
// accept-all action): LogAccept's for a bot KeepsLogRule covers, else nothing more than the caller's own checks
bool LogAccepts(PlayerbotAI* botAI, Player* bot, Quest const* quest);

// Whether a bot KeepsLogRule does not cover takes a quest a giver offers: upstream's filter (the game's checks, worth
// and capable doing)
bool FreeBotTakes(Player* bot, Quest const* quest);

// Player::CanInteractWithQuestGiver without the distance check.
bool CanInteractWithQuestGiver(Player* bot, Object* questGiver);
// constraints: the bot's test constraints (a TestConstraints::Set, GiverSought); 0 outside a test. A bot KeepsLogRule
// covers asks LogAccept (take: the search's, else one of its own), which holds the game's take check (WQ41): a giver
// offering only takes the one accept refuses is none to seek, and a planned take left open there is no give-up
// (QuestFail cannot_take is a giver that does not offer it, WQ18c)
bool HasQuestToAcceptOrReward(Player* bot, WorldObject* object, uint64 constraints = 0, LogAccept* take = nullptr);
// questing WQ43: whether the reward's items (the choice and the fixed ones) fail only for room: the bags are full for
// one and the game takes every other (a unique or limited item is no matter of room)
bool RoomRefusesReward(Player* bot, Quest const* quest, uint32 choice);
// questing WQ43: whether a ready quest's reward can be had: the game takes it now, or a bot of the living world (on an
// order or planner-owned, KeepsLogRule) may make the room for it (HandInRoomRules.h; constraints: nodestroy). No bag
// scan: the room is made at the hand-in itself (NewRpgBaseAction::MakeRoomForHandIn)
bool HandInRewardable(Player* bot, Quest const* quest, uint32 choice, uint64 constraints);
bool IsQuestWorthDoing(Player* bot, Quest const* quest);
bool IsQuestCapableDoing(Player* bot, Quest const* quest);

// Nearest giver in the new RPG target caches with something to accept or reward, skipping givers
// the bot gave up on. A distanceLimit of 0 searches the whole cache. A bot not on an order compares only each cache's
// first match (upstream); a bot on an order the nearest of all.
ObjectGuid FindQuestGiver(PlayerbotAI* botAI, float distanceLimit = 0.0f);

// A shapeshift a spell or an item use is refused in: any form but a stance (SpellInfo::CheckShapeshift; the same
// test as the item-use framework's view, ItemUse.cpp). The quest executor's uses and a craft leave it first (WQL2b
// note 1)
bool RealShapeshift(Player* bot);
}  // namespace NewRpgHelpers

#endif
