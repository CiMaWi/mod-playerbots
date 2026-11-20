/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_STARTINGHISTORYRULES_H
#define _PLAYERBOT_STARTINGHISTORYRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// The Player-free rules of a persistent bot's flights and starting history: flights only between discovered nodes,
// and what a bot created above level 1 plausibly has (flight points, a hearthstone bound at a fitting inn, bags).
// Header-only so tests/starting-history-selftest.cpp checks the same code the server runs.
namespace StartingHistory
{
constexpr uint32_t ITEM_HEARTHSTONE = 6948;
constexpr uint32_t ITEM_FACTORY_BAG = 51809;       // the factory's 24-slot bag, never a natural one
constexpr uint32_t ITEM_SMALL_BROWN_POUCH = 4496;  // 6 slots, sold by 51 vendors
constexpr uint32_t ITEM_RUNECLOTH_BAG = 14046;     // 14 slots, tailoring
constexpr uint32_t ITEM_FROSTWEAVE_BAG = 41599;    // 20 slots, tailoring
constexpr uint32_t FIRST_BAG_LEVEL = 10;
constexpr uint32_t RUNECLOTH_BAG_LEVEL = 60;
constexpr uint32_t FROSTWEAVE_BAG_LEVEL = 75;
// A character this young (played seconds) is new: the factory gives it its history at its first randomize
constexpr uint32_t NEW_CHARACTER_PLAYED_SECONDS = 3600;

struct Bracket
{
    uint32_t low = 0;
    uint32_t high = 0;
};

// A flight node of the bot's team with the level bracket of its zone (zones without a bracket give no history)
struct NodeInZone
{
    uint32_t node = 0;
    bool hasBracket = false;
    Bracket bracket;
};

enum class BagStep
{
    Keep,     // a natural bag, or an empty slot below the first bag level
    Equip,    // an empty slot gets the level's bag
    Replace,  // a factory bag gives way to the level's bag
    Remove    // a factory bag below the first bag level goes
};

// Every node after the first is discovered; the first is where the bot stands and learns from its flight master
template <class Known>
bool PathKnown(std::vector<uint32_t> const& path, Known const& known)
{
    if (path.empty())
        return false;

    return std::all_of(path.begin() + 1, path.end(), [&known](uint32_t node) { return known(node); });
}

template <class Known>
std::vector<uint32_t> KnownNodes(std::vector<uint32_t> const& nodes, Known const& known)
{
    std::vector<uint32_t> result;
    for (uint32_t node : nodes)
        if (known(node))
            result.push_back(node);

    return result;
}

// The flight points a bot of this level plausibly has: its race's start nodes plus every node of its team's zones
// whose bracket ends below its level; sorted and unique. A level-1 bot has nothing beyond its creation.
inline std::vector<uint32_t> HistoryNodes(uint32_t level, std::vector<uint32_t> const& startNodes,
                                          std::vector<NodeInZone> const& teamNodes)
{
    std::vector<uint32_t> result;
    if (level <= 1)
        return result;

    for (uint32_t node : startNodes)
        if (node)
            result.push_back(node);

    for (NodeInZone const& spot : teamNodes)
        if (spot.node && spot.hasBracket && spot.bracket.high < level)
            result.push_back(spot.node);

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

inline bool BracketFits(uint32_t level, Bracket const& bracket)
{
    return level >= bracket.low && level <= bracket.high;
}

// The bind is kept when its zone fits the level (a second pass changes nothing)
inline bool BindFits(uint32_t level, bool hasBracket, Bracket const& bindBracket)
{
    return hasBracket && BracketFits(level, bindBracket);
}

// Indices of the team's inns whose zone fits the level, in the given order
inline std::vector<size_t> FittingInns(uint32_t level, std::vector<Bracket> const& innBrackets)
{
    std::vector<size_t> result;
    for (size_t i = 0; i < innBrackets.size(); ++i)
        if (BracketFits(level, innBrackets[i]))
            result.push_back(i);

    return result;
}

// The bag a player of this level plausibly carries in each slot; 0 below the first bag level
inline uint32_t BagForLevel(uint32_t level)
{
    if (level < FIRST_BAG_LEVEL)
        return 0;

    if (level < RUNECLOTH_BAG_LEVEL)
        return ITEM_SMALL_BROWN_POUCH;

    if (level < FROSTWEAVE_BAG_LEVEL)
        return ITEM_RUNECLOTH_BAG;

    return ITEM_FROSTWEAVE_BAG;
}

// currentEntry is 0 for an empty bag slot
inline BagStep DecideBagSlot(uint32_t currentEntry, uint32_t historyBag)
{
    if (currentEntry == ITEM_FACTORY_BAG)
        return historyBag ? BagStep::Replace : BagStep::Remove;

    if (!currentEntry && historyBag)
        return BagStep::Equip;

    return BagStep::Keep;
}

// The riding ranks a player of the level has (upkeep WU14), by the factory's thresholds (UseGroundMountAtMinLevel,
// UseFastGroundMountAtMinLevel, UseFlyMountAtMinLevel, UseFastFlyMountAtMinLevel): tier n gives the first n ranks
// and the mount types 0 to n - 1 (slow, fast, flying, fast flying), as InitMounts hands them out; a rank needs the
// one below it, so a threshold below an unmet earlier one gives nothing
constexpr std::array<uint32_t, 4> RIDING_HISTORY_SPELLS = {33388, 33391, 34090, 34091};

inline uint32_t RidingTier(uint32_t level, std::array<uint32_t, 4> const& thresholds)
{
    uint32_t tier = 0;
    while (tier < thresholds.size() && level >= thresholds[tier])
        ++tier;
    return tier;
}

inline bool NewCharacter(uint32_t playedSeconds) { return playedSeconds < NEW_CHARACTER_PLAYED_SECONDS; }

// The factory gives the history once, to a new persistent random bot above level 1
inline bool FactoryApplies(bool honestRandomBot, uint32_t level, uint32_t playedSeconds)
{
    return honestRandomBot && level > 1 && NewCharacter(playedSeconds);
}

// The professions of a bot made at a level (professions WR5d, WR.20): the skill a player of the level would usually
// have (PLAN 3.8), held by the ranks the level and that skill allow
constexpr uint32_t PROFESSION_SKILL_PER_LEVEL = 5;

// One rank of a profession as the recipe index lists it: the spell that is the rank, its skill cap, and the skill value
// and the level it asks
struct HistoryRung
{
    uint32_t spell = 0;
    uint32_t cap = 0;
    uint32_t reqSkill = 0;
    uint32_t reqLevel = 0;
};

// ranks = how many rungs (lowest cap first) the bot has, 0 = not the profession; value and cap of its skill
struct ProfessionHistory
{
    std::size_t ranks = 0;
    uint32_t value = 0;
    uint32_t cap = 0;
};

// Nothing at level 1, nor when the first rank asks a higher level; a rank needs the one below it
inline ProfessionHistory ProfessionAt(uint32_t level, std::vector<HistoryRung> const& rungs)
{
    ProfessionHistory history;
    if (level <= 1)
        return history;

    uint32_t const skill = level * PROFESSION_SKILL_PER_LEVEL;
    for (HistoryRung const& rung : rungs)
    {
        if (rung.reqLevel > level || rung.reqSkill > skill)
            break;

        ++history.ranks;
        history.cap = rung.cap;
    }

    if (history.ranks)
        history.value = std::min(skill, history.cap);

    return history;
}

// The tools a profession makes for itself (WR.12), by the recipe that makes them; the Arcanite and Adamantite rods
// come from recipe items only, never from a history
struct MadeToolRow
{
    uint16_t skill = 0;
    uint32_t item = 0;
    uint32_t recipe = 0;
};

constexpr std::array<MadeToolRow, 8> MADE_TOOLS = {{
    {202, 6219, 7430},    // engineering: Arclight Spanner
    {333, 6218, 7421},    // enchanting: Runed Copper Rod
    {333, 6339, 7795},    // Runed Silver Rod
    {333, 11130, 13628},  // Runed Golden Rod
    {333, 11145, 13702},  // Runed Truesilver Rod
    {333, 22461, 32664},  // Runed Fel Iron Rod
    {333, 22463, 32667},  // Runed Eternium Rod
    {333, 44452, 60619},  // Runed Titanium Rod
}};

// The made tool a bot of the skill line carries: the last row whose recipe it knows (a higher rod serves the casts of
// the lower ones), 0 for none
template <class Knows>
uint32_t MadeTool(uint16_t skill, Knows const& knows)
{
    uint32_t item = 0;
    for (MadeToolRow const& row : MADE_TOOLS)
        if (row.skill == skill && knows(row.recipe))
            item = row.item;

    return item;
}

// The level-up cleanup of old consumables deletes reagents. Item class 15 (Misc) subclass 0 is Junk, which holds the
// hearthstone; the Misc reagent subclass is 1. (The core's ITEM_SUBCLASS_REAGENT is 0 for class 5, Reagent.)
constexpr uint32_t ITEM_CLASS_REAGENT_ID = 5;
constexpr uint32_t ITEM_CLASS_MISC_ID = 15;
constexpr uint32_t ITEM_SUBCLASS_MISC_REAGENT_ID = 1;
inline bool CleanupDeletesMisc(uint32_t itemClass, uint32_t subClass, uint32_t itemId)
{
    if (itemId == ITEM_HEARTHSTONE)
        return false;
    return itemClass == ITEM_CLASS_REAGENT_ID ||
           (itemClass == ITEM_CLASS_MISC_ID && subClass == ITEM_SUBCLASS_MISC_REAGENT_ID);
}
}  // namespace StartingHistory

#endif
