/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTFAILRECORD_H
#define _PLAYERBOT_QUESTFAILRECORD_H

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "QuestFailBook.h"

class Player;

// A persistent bot's record of the quests it gave up (questing WT35c; QuestFailBook.h, table
// playerbots_bot_quest_fail). Map threads note a give-up in memory; the world thread loads the record at startup and
// writes the changed rows asynchronously every few minutes and at shutdown. No database call on a map thread
namespace QuestFailRecord
{
// World thread, at startup before any bot logs in: one query; registers the record's forget (BotStores)
void Load();

// Map thread: a give-up of the bot's quest with its cause and the whole minutes pursued. Only a bot that keeps its
// possessions (BotKind::KeepsPossessions) is recorded; others still give up, unrecorded
void Note(Player* bot, std::uint32_t questId, std::string_view cause, std::uint32_t minutes);

// World thread: a timer add; the changed rows' write when it runs out
void Update(std::uint32_t diff);

// World thread, at shutdown: the last write
void Shutdown();

// Any thread: copies of the record
std::optional<QuestFail::Row> Find(std::uint32_t guid, std::uint32_t questId);
std::vector<std::pair<std::uint32_t, QuestFail::Row>> RowsOf(std::uint32_t guid);
// Any thread: the bot's quests given up `limit` times or more, sorted (QuestFail::GivenUp)
std::vector<std::uint32_t> GivenUpQuests(std::uint32_t guid, std::uint32_t limit);
}  // namespace QuestFailRecord

#endif
