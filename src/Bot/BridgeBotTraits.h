/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEBOTTRAITS_H
#define PLAYERBOTS_BRIDGEBOTTRAITS_H

#include <string>
#include <vector>

class ObjectGuid;

// Travel plan WT11a: a bot's personality as words for the chat modules (WT11b: `{bot_personality}` and the bridge
// card's traits). Read from the trait cache only: any thread, no Player, no database, never numbers.
namespace PlayerbotBridge
{
struct BotPersonality
{
    std::vector<std::string> words;      // Every noticeable trait, strongest first ("very careful", "rather chatty")
    std::vector<std::string> interests;  // One to three ("fishing", "inns and ale")
};

// False (out cleared) when the guid is not a player's or the bot has no traits (it never logged in; without the
// table, not since startup).
bool ReadBotPersonality(ObjectGuid const& botGuid, BotPersonality& out);
}  // namespace PlayerbotBridge

#endif
