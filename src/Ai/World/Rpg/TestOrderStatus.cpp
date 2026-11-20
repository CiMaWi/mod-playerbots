/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestOrderStatus.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "BotTravel.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TestOrders.h"
#include "TravelMgr.h"

namespace
{
// `route start` (tests, WT64e): a status started at the order's place with what its own pick would give it. grind,
// camp: the spot; quest: the quest in the bot's log, its first open objective at the place; gather, pvp: the node's or
// capture point's spawn; flight: the bot's nearest flight master, which must be the order's (the case starts the bot
// at that master's town), and one of its flights; vendor, trainer, banker, mail: an errand trip of one stop;
// wander_npc, roam: no place (the status picks around the bot's start)
constexpr char const* STATUS_WORDS[] = {"grind",  "camp",    "quest",  "gather", "pvp",        "flight",
                                        "vendor", "trainer", "banker", "mail",   "wander_npc", "roam"};

int32 FirstOpenObjective(Player* bot, Quest const* quest)
{
    QuestStatusData const& status = bot->getQuestStatusMap().at(quest->GetQuestId());
    for (int32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            return i;
    for (int32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (status.ItemCount[i] < quest->RequiredItemCount[i])
            return QUEST_OBJECTIVES_COUNT + i;
    return 0;
}

std::optional<NewRpgInfo::RpgData> OrderedStatus(Player* bot, TravelOrder const& order, char const*& refusal)
{
    std::string const& word = order.status;
    if (word == "grind")
        return NewRpgInfo::GoGrind{order.dest};
    if (word == "camp")
        return NewRpgInfo::GoCamp{order.dest};
    if (word == "roam")
        return NewRpgInfo::WanderRandom{};
    if (word == "wander_npc")
        return NewRpgInfo::WanderNpc{};
    if (word == "gather")
        return NewRpgInfo::DoGather{.nodeSpawnId = order.entry, .nodePos = order.dest};
    if (word == "pvp")
        return NewRpgInfo::OutdoorPvP{order.entry};
    if (word == "quest")
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(order.entry);
        QuestStatus const questStatus = bot->GetQuestStatus(order.entry);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            refusal = "the quest is not in the log";
            return std::nullopt;
        }
        return NewRpgInfo::DoQuest{
            .quest = quest, .questId = order.entry, .objectiveIdx = FirstOpenObjective(bot, quest), .pos = order.dest};
    }
    if (word == "flight")
    {
        auto const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
        std::vector<std::vector<uint32>> paths = sTravelMgr.GetOptimalFlightDestinations(bot);
        if (!info || info->templateEntry != order.entry || paths.empty())
        {
            refusal = !info || info->templateEntry != order.entry ? "the nearest flight master is another"
                                                                  : "no flight from the nearest flight master";
            return std::nullopt;
        }
        std::vector<uint32> path = std::move(paths[urand(0, paths.size() - 1)]);
        return NewRpgInfo::TravelFlight{info->templateEntry, info->pos, std::move(path)};
    }
    NewRpgInfo::ErrandStop stop;
    stop.pos = order.dest;
    if (word == "vendor")
        stop.vendorEntry = order.entry;
    else if (word == "trainer")
        stop.trainerEntry = order.entry;
    else if (word == "banker")
        stop.bankAfter = true;  // a deposit only: no vendor part
    else if (word == "mail")
        stop.mail = true;
    else
    {
        refusal = "unknown status";
        return std::nullopt;
    }
    std::vector<NewRpgInfo::ErrandStop> stops;
    stops.push_back(std::move(stop));
    return RpgStatus::ErrandTrip(std::move(stops), bot->GetMoney());
}
}  // namespace

bool TravelOrders::StartStatus(PlayerbotAI* botAI, Player* bot, TravelOrder const& order)
{
    char const* refusal = "";
    std::optional<NewRpgInfo::RpgData> next = OrderedStatus(bot, order, refusal);
    if (!next)
    {
        LOG_INFO("playerbots", "[Route] {} status {} at {} (entry {}) refused: {}", bot->GetName(), order.status,
                 order.place, order.entry, refusal);
        return false;
    }
    LOG_INFO("playerbots", "[Route] {} status {} at {} ({}, {:.0f}, {:.0f}, {:.0f}) entry {} ordered", bot->GetName(),
             order.status, order.place, order.dest.GetMapId(), order.dest.GetPositionX(), order.dest.GetPositionY(),
             order.dest.GetPositionZ(), order.entry);
    botAI->GetTravel().CancelLootInteraction("order");
    RpgStatus::Start(botAI, std::move(*next), "order", StatusEnd::Outcome::Ordered);
    return true;
}

bool TravelOrders::KnownStatus(std::string const& word)
{
    return std::find(std::begin(STATUS_WORDS), std::end(STATUS_WORDS), word) != std::end(STATUS_WORDS);
}

std::string TravelOrders::StatusWords()
{
    std::string words;
    for (char const* word : STATUS_WORDS)
        words += (words.empty() ? "" : "|") + std::string(word);
    return words;
}
