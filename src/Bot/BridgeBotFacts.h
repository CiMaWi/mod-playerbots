/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEBOTFACTS_H
#define PLAYERBOTS_BRIDGEBOTFACTS_H

#include <string>
#include <utility>
#include <vector>

class ObjectGuid;

// LC9: the lasting facts of a bot (P §3.3) as words, read from the bot's own data on the world thread while maps are
// idle (the same rule as ReadBotState; in memory only, no database). The wire carries them from LC10. No number, id or
// hidden skill is in any word.
namespace PlayerbotBridge
{
struct BotFacts
{
    std::string phrase;  // How it fights, one fixed phrase for its class and talent tree
    std::vector<std::pair<std::string, std::string>> crafts;    // Craft name, rank word
    std::vector<std::string> useful;                            // What its crafts make that is useful on the road
    std::vector<std::string> servants;                          // Servants and pets it can call, by name
    std::vector<std::pair<std::string, std::string>> standing;  // Faction name, standing word (not neutral)
    std::string guild;                                          // Empty: none
    std::vector<std::string> relatives;                         // None before the family plan (LC20)
};

// False (facts reset) off the world thread, or for a guid that is not an online bot in the world.
bool ReadBotFacts(ObjectGuid const& botGuid, BotFacts& facts);
}  // namespace PlayerbotBridge

#endif
