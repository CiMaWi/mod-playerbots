/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEBOTFACTS_H
#define PLAYERBOTS_BRIDGEBOTFACTS_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class ObjectGuid;
class Player;

// LC9: the lasting facts of a bot (P §3.3) as words, read from the bot's own data on the world thread while maps are
// idle (the same rule as ReadBotState; in memory only, no database). The wire carries them from LC10. No number, id or
// hidden skill is in any word.
namespace PlayerbotBridge
{
// LC18e: a place the bot has been to, from its record's Visit rows
struct BotVisit
{
    std::string name;       // The game's name of the zone, dungeon or continent
    std::string kind;       // zone, dungeon or continent
    std::string land;       // The continent of a zone or dungeon; empty for a continent or when none
    std::uint32_t ago = 0;  // Seconds since the latest entry
};

// LC18f: a recipe the bot knows (a profession spell that creates an item), as the reader found it: the craft's name,
// the item made (its name, class and subclass) and the skill value the recipe asks for
struct BotRecipe
{
    std::string craft;
    std::string itemName;
    std::uint32_t itemClass = 0;
    std::uint32_t itemSubclass = 0;
    std::uint32_t requiredSkill = 0;
};

// LC18f: what a craft makes that is useful on the road: the craft's name and the item names, best first
struct BotMake
{
    std::string craft;
    std::vector<std::string> items;
};

struct BotFacts
{
    std::string phrase;  // How it fights, one fixed phrase for its class and talent tree
    std::vector<std::pair<std::string, std::string>> crafts;  // Craft name, rank word
    std::vector<BotMake> makes;         // What its recipes make that is useful on the road (LC18f)
    std::vector<std::string> servants;  // Servants and pets it can call, by name
    std::vector<std::pair<std::string, std::string>> standing;  // Faction name, standing word (not neutral)
    std::string guild;                                          // Empty: none
    std::vector<std::string> relatives;                         // None before the family plan (LC20)
    std::vector<std::string> before;  // LC20a: the background statements, in ordinal order (BotPastMgr)
    std::vector<BotVisit> visits;                               // Where it has been, newest first (LC18e)
};

// False (facts reset) off the world thread, or for a guid that is not an online bot in the world.
bool ReadBotFacts(ObjectGuid const& botGuid, BotFacts& facts);

// LC18f: the recipes the bot knows, from its spell map and the loaded stores (no query); the caller is on the world
// thread
void ReadBotRecipes(Player* bot, std::vector<BotRecipe>& recipes);
}  // namespace PlayerbotBridge

#endif
