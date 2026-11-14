/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEBOTFACTSRULES_H
#define PLAYERBOTS_BRIDGEBOTFACTSRULES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// LC9: the words of a bot's lasting facts (P §3.3), as plain data without a game type: how it fights as a phrase per
// class and talent tree, the rank word of a craft, what a craft is useful for on the road, the servants a class can
// call, the standing with a faction. Header-only so tests/bridge-bot-facts-selftest.cpp checks what the server runs.
// No word holds a digit, an id or a hidden skill (C4).
namespace PlayerbotBridge::FactsRules
{
// Class ids are the game's (1 warrior .. 11 druid; 6 death knight); tabs are the talent trees in the order of the
// class's talent tab pages (a class without a row, id 0 and 10, falls back)
constexpr std::size_t CLASS_SLOTS = 12;
constexpr std::size_t TREES = 3;

struct TreePhrases
{
    std::array<std::string_view, TREES> Phrase;
};

constexpr std::array<TreePhrases, CLASS_SLOTS> PHRASES = {{
    {{"", "", ""}},  // 0: no class
    {{"fights with great blows of a two-handed weapon", "fights in a furious whirl with a weapon in each hand",
      "stands in front behind a shield and guards the others"}},  // 1 warrior
    {{"heals and shields the others with the Light", "guards the others with shield and holy power",
      "smites foes with holy fire and a heavy weapon"}},  // 2 paladin
    {{"fights beside a tamed beast, bow in hand", "strikes from afar with deadly aim",
      "fights with traps and tricks, hardy in the wild"}},  // 3 hunter
    {{"kills with poison and the dagger", "fights toe to toe with quick blades",
      "stalks from the shadows and strikes unseen"}},  // 4 rogue
    {{"shields and mends the others with steady discipline", "heals the others with holy light",
      "wields the dark and plagues foes with shadow"}},  // 5 priest
    {{"drains foes with cold blood and runic blades", "strikes with frost and icy runes",
      "fights with plague, rot and the dead"}},  // 6 death knight
    {{"calls down lightning and fire", "fights with weapons wrapped in the elements",
      "heals the others with the spirits and the water"}},                            // 7 shaman
    {{"bends raw arcane power", "burns foes with fire", "chills and freezes foes"}},  // 8 mage
    {{"plagues foes with curses and slow decay", "commands demons to fight for it",
      "rains fire and ruin on foes"}},  // 9 warlock
    {{"", "", ""}},                     // 10: none
    {{"calls the fury of moon and sun", "fights as a beast with claw and fang",
      "heals the others with growing things"}},  // 11 druid
}};

constexpr std::string_view FALLBACK_PHRASE = "fights as best it can";

constexpr std::string_view Phrase(uint8_t classId, uint8_t tree)
{
    if (tree >= TREES)
        return FALLBACK_PHRASE;

    if (classId >= CLASS_SLOTS || PHRASES[classId].Phrase[tree].empty())
        return FALLBACK_PHRASE;

    return PHRASES[classId].Phrase[tree];
}

// A craft rank in a word: apprentice below 75, journeyman below 150, expert below 225, artisan below 300, master below
// 375, grand master from there
constexpr uint32_t BAND_WIDTH = 75;
constexpr std::size_t RANK_BANDS = 6;
constexpr std::array<std::string_view, RANK_BANDS> RANK_WORDS = {"apprentice", "journeyman", "expert",
                                                                 "artisan",    "master",     "grand master"};

constexpr std::string_view RankWord(uint32_t skillValue)
{
    std::size_t const band = skillValue / BAND_WIDTH;
    return RANK_WORDS[band < RANK_BANDS ? band : RANK_BANDS - 1];
}

// The professions a bot may have: the game's skill line id, its name, and what it is good for on the road
struct Craft
{
    uint32_t Skill;
    std::string_view Name;
    std::string_view Useful;
};

constexpr std::array<Craft, 14> CRAFTS = {{
    {171, "Alchemy", "potions, elixirs and flasks"},
    {164, "Blacksmithing", "weapons, armour and sharpening stones"},
    {333, "Enchanting", "enchantments for gear"},
    {202, "Engineering", "gadgets, scopes and explosives"},
    {773, "Inscription", "glyphs"},
    {755, "Jewelcrafting", "gems and rings"},
    {165, "Leatherworking", "leather armour and armour kits"},
    {197, "Tailoring", "cloth armour, bags and bandages"},
    {185, "Cooking", "food that restores health"},
    {129, "First Aid", "bandages"},
    {356, "Fishing", "fish for the pot"},
    {182, "Herbalism", "herbs for potions"},
    {186, "Mining", "ore for smiths and engineers"},
    {393, "Skinning", "leather for armour"},
}};

// A servant a class calls by a spell it knows: the spell and the creature's name
struct Servant
{
    uint8_t Class;
    uint32_t Spell;
    std::string_view Name;
};

constexpr std::array<Servant, 7> SERVANTS = {{
    {9, 688, "Imp"},
    {9, 697, "Voidwalker"},
    {9, 712, "Succubus"},
    {9, 691, "Felhunter"},
    {9, 30146, "Felguard"},
    {8, 31687, "Water Elemental"},
    {6, 46584, "Ghoul"},
}};

// The standing with a faction (the game's ReputationRank order, hated to exalted)
constexpr std::size_t STANDINGS = 8;
constexpr std::array<std::string_view, STANDINGS> STANDING_WORDS = {"hated",    "hostile", "unfriendly", "neutral",
                                                                    "friendly", "honored", "revered",    "exalted"};
constexpr std::size_t NEUTRAL_STANDING = 3;
constexpr std::size_t MAX_STANDINGS = 6;  // [E] factions in a read, the most marked first

constexpr std::string_view StandingWord(std::size_t rank)
{
    return STANDING_WORDS[rank < STANDINGS ? rank : NEUTRAL_STANDING];
}
}  // namespace PlayerbotBridge::FactsRules

#endif
