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
class QuestIndex;
struct QuestOrderRun;
namespace QuestLog
{
struct Record;
}
}  // namespace Planner

// Quest checks shared by the new RPG actions and values. They encode new RPG questing policy
// (solo, non-repeatable, level window), not general quest rules.
namespace NewRpgHelpers
{
// Radius within which a quest giver with something to accept or reward pulls the bot over.
constexpr float QUEST_GIVER_SEARCH_DISTANCE = 80.0f;

// Questing WQ31: the bot's quest executor runs an order (NewRpgInfo::DoQuest::order). O(1)
bool OnOrder(PlayerbotAI* botAI);

// Questing WQ18b: the failure record of a bot on an order as the accept and the log upkeep read it (QuestLogRules'
// Read), kept by the order's run and read again only after a write to the record (QuestFail::Reread): the executor's
// tick copies no rows and takes no lock. The second form finds the bot's running order; a bot on none reads anew.
// Map thread
std::shared_ptr<Planner::QuestLog::Record const> OrderRecord(Player* bot, Planner::QuestOrderRun& run);
std::shared_ptr<Planner::QuestLog::Record const> OrderRecord(PlayerbotAI* botAI, Player* bot);

// Questing WQ31 (WQ.40): whether a bot on an order takes a quest a giver offers, QuestLogRules' MayAccept (the one
// take test). The facts (the log, the rewarded quests, the failure record) are gathered at the first
// quest that asks and kept for one search; an accept changes the log, so it calls Reset
class OrderTake
{
public:
    explicit OrderTake(Player* bot) : _bot(bot) {}
    OrderTake(OrderTake const&) = delete;
    OrderTake& operator=(OrderTake const&) = delete;

    bool MayAccept(Quest const* quest);
    void Reset() { _built = false; }

private:
    void Build();

    Player* _bot;
    bool _built{false};
    std::shared_ptr<Planner::QuestIndex const> _quests;
    std::vector<std::uint32_t> _log;
    std::vector<std::uint32_t> _rewarded;
    std::vector<std::uint32_t> _givenUp;
    std::vector<std::int32_t> _takenGroups;
};

// Whether a bot not on an order takes a quest a giver offers: upstream's filter (the game's checks, worth and capable
// doing) and the planner's capability refusal for a bot it owns
bool FreeBotTakes(PlayerbotAI* botAI, Player* bot, Quest const* quest);

// Player::CanInteractWithQuestGiver without the distance check.
bool CanInteractWithQuestGiver(Player* bot, Object* questGiver);
// constraints: the bot's test constraints (TestConstraints::GiverSought); 0 outside a test. A bot on an order asks
// OrderTake (take: the search's, else one of its own), without the game's take check: a planned take the game refuses
// stays open, and its order's end at the giver gives it up (QuestFail cannot_take, WQ18c)
bool HasQuestToAcceptOrReward(Player* bot, WorldObject* object, uint32 constraints = 0, OrderTake* take = nullptr);
bool IsQuestWorthDoing(Player* bot, Quest const* quest);
bool IsQuestCapableDoing(Player* bot, Quest const* quest);
// Questing WT36a: a bot the planner owns takes no quest the executor refuses for its side (the quest index's capability
// refusal, QuestExecRules.h), at a giver as in its plan; any other bot (it keeps no possessions, or a test holds its
// planner) keeps upstream's accept
bool IsQuestPlannerOffered(PlayerbotAI* botAI, Player* bot, Quest const* quest);

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
