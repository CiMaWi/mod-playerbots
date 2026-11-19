/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTSTORES_H
#define _PLAYERBOT_BOTSTORES_H

#include <cstdint>
#include <string>

// The rows of a deleted bot that Player::DeleteFromDB leaves behind: one DELETE each, in one transaction per DB queued
// before the core's delete (one async worker per DB, so FIFO: item_soulbound_trade_data still finds the bot's items).
// Game-free, one row per line: the hub's tools/lib/bot-tables.js parses this list.

enum class BotRowDb
{
    Characters,
    Playerbots
};

enum class BotRowOwner
{
    Core,       // a table of the core that Player::DeleteFromDB leaves
    Module,     // mod-ollama-chat's table
    Playerbots  // mod-playerbots' own table
};

struct BotRow
{
    BotRowDb db;
    char const* table;
    char const* column;
    bool byItem;  // the column holds item guids: those of the bot's items (item_instance.owner_guid)
    BotRowOwner owner;
};

// clang-format off
inline constexpr BotRow BotRows[] = {
    {BotRowDb::Characters, "character_stats", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "character_arena_stats", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "character_banned", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "character_battleground_random", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "character_brew_of_the_month", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "character_achievement_offline_updates", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "battleground_deserters", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "lfg_data", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "mail_server_character", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "guild_member_withdraw", "guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "channels_bans", "playerGUID", false, BotRowOwner::Core},
    {BotRowDb::Characters, "recovery_item", "Guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "pvpstats_players", "character_guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "quest_tracker", "character_guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "item_refund_instance", "player_guid", false, BotRowOwner::Core},
    {BotRowDb::Characters, "item_soulbound_trade_data", "itemGuid", true, BotRowOwner::Core},
    {BotRowDb::Characters, "mod_ollama_chat_personality", "guid", false, BotRowOwner::Module},
    {BotRowDb::Characters, "mod_ollama_chat_bot_player_sentiments", "bot_guid", false, BotRowOwner::Module},
    {BotRowDb::Characters, "mod_ollama_chat_bot_player_sentiments", "player_guid", false, BotRowOwner::Module},
    {BotRowDb::Characters, "mod_ollama_chat_history", "bot_guid", false, BotRowOwner::Module},
    {BotRowDb::Characters, "mod_ollama_chat_history", "player_guid", false, BotRowOwner::Module},
    {BotRowDb::Characters, "mod_ollama_chat_memories", "bot_guid", false, BotRowOwner::Module},
    {BotRowDb::Characters, "mod_ollama_chat_relationships", "bot_guid", false, BotRowOwner::Module},
    {BotRowDb::Characters, "mod_ollama_chat_relationships", "other_guid", false, BotRowOwner::Module},
    {BotRowDb::Playerbots, "playerbots_random_bots", "bot", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_db_store", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_bot_profile", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_bot_traits", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_bot_record", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_acquaintance", "bot_guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_pvp_loadout", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_pvp_worn", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_ladder_roster", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_preferred_mounts", "guid", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_guild_tasks", "owner", false, BotRowOwner::Playerbots},
    {BotRowDb::Playerbots, "playerbots_custom_strategy", "owner", false, BotRowOwner::Playerbots},
};
// clang-format on

// A bot's delete and the stores' memory of it (BotStores.cpp). World thread
namespace BotStores
{
// A store's memory of one bot, dropped before its rows are deleted so nothing writes them back; at the store's load,
// in forget order. A second registration under the same name replaces the first
void RegisterForget(char const* name, void (*forget)(std::uint32_t guid));

// Startup: which of the module's rows the characters DB has (mod-ollama-chat's tables need not exist)
void LoadModuleRows();

// The rows Delete clears in one DB per bot
std::uint32_t RowCount(BotRowDb db);

// A bot's complete delete (retire, resize): every registered forget, the list's rows of both DBs, then the core's
// delete (Player::DeleteFromDB), in this order
void Delete(std::uint32_t guid, std::uint32_t account);

// Startup, after the random bot accounts' characters are deleted: the list's playerbots rows of guids no character
// outside those accounts has (guid 0 is a shared row, e.g. a server-wide custom strategy, and stays)
void SweepPlayerbotsRows(std::string const& characterDb, std::string const& botAccountIds);
}  // namespace BotStores

#endif
