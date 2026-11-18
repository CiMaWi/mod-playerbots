/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Every text the game writes for a language model: the words of a bot's facts, nature, deeds, state and surroundings,
// the catalogue's sentences, the words of topic answers, refusals and wakes. The bridge's own texts are in the hub
// (bridge/claude/prompt-texts.mjs). No logic here: a new model-facing string goes here, never into a logic file.
// Standard library only, so tests and tools can include it. A wording edit rebuilds every file that includes this.

#ifndef PLAYERBOTS_BRIDGETEXTS_H
#define PLAYERBOTS_BRIDGETEXTS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace BridgeTexts
{
// ---------------------------------------------------------------------------------------------
// 1. Facts: the words of a bot's lasting facts (people, calling, role, rank, makes, servants, standing)
// ---------------------------------------------------------------------------------------------
namespace Facts
{
// What a class does in a talent tree, as the facts phrase it: a row per class id, a phrase per tree; the fallback
// stands for a class or tree without a phrase.
struct TreePhrases
{
    std::array<std::string_view, 3> Phrase;
};

inline constexpr std::array<TreePhrases, 12> PHRASES = {{
    {{"", "", ""}},  // 0: no class
    {{"fights with great blows of a two-handed weapon", "fights in a furious whirl with a weapon in each hand",
      "stands in front behind a shield and guards the others"}},  // 1 warrior
    {{"heals and shields the others with the Light", "guards the others with shield and holy power",
      "smites enemies with holy fire and a heavy weapon"}},  // 2 paladin
    {{"fights beside a tamed beast, bow in hand", "strikes from afar with deadly aim",
      "fights with traps and tricks, hardy in the wild"}},  // 3 hunter
    {{"kills with poison and the dagger", "fights toe to toe with quick blades",
      "stalks from the shadows and strikes unseen"}},  // 4 rogue
    {{"shields and mends the others with steady discipline", "heals the others with holy light",
      "wields the dark and plagues enemies with shadow"}},  // 5 priest
    {{"drains enemies with cold blood and runic blades", "strikes with frost and icy runes",
      "fights with plague, rot and the dead"}},  // 6 death knight
    {{"calls down lightning and fire", "fights with weapons wrapped in the elements",
      "heals the others with the spirits and the water"}},                                  // 7 shaman
    {{"bends raw arcane power", "burns enemies with fire", "chills and freezes enemies"}},  // 8 mage
    {{"plagues enemies with curses and slow decay", "commands demons to fight for it",
      "rains fire and ruin on enemies"}},  // 9 warlock
    {{"", "", ""}},                        // 10: none
    {{"calls the fury of moon and sun", "fights as a beast with claw and fang",
      "heals the others with growing things"}},  // 11 druid
}};

inline constexpr std::string_view FALLBACK_PHRASE = "fights as best it can";

// The rank words of a craft, from a skill value of 0 up in bands (apprentice first); the facts say a craft's rank with
// one.
inline constexpr std::array<std::string_view, 6> RANK_WORDS = {"apprentice", "journeyman", "expert",
                                                               "artisan",    "master",     "grand master"};

// The professions a bot may have, as the facts name them: the game's skill line id, the name and what it is good for on
// the road.
struct Craft
{
    uint32_t Skill;
    std::string_view Name;
    std::string_view Useful;
};

inline constexpr std::array<Craft, 14> CRAFTS = {{
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

// The servants a class calls by a spell it knows, as the facts name them: the class id, the spell id and the creature's
// name.
struct Servant
{
    uint8_t Class;
    uint32_t Spell;
    std::string_view Name;
};

inline constexpr std::array<Servant, 7> SERVANTS = {{
    {9, 688, "Imp"},
    {9, 697, "Voidwalker"},
    {9, 712, "Succubus"},
    {9, 691, "Felhunter"},
    {9, 30146, "Felguard"},
    {8, 31687, "Water Elemental"},
    {6, 46584, "Ghoul"},
}};

// The standing with a faction, hated to exalted (the game's ReputationRank order), as the facts say it.
inline constexpr std::array<std::string_view, 8> STANDING_WORDS = {"hated",    "hostile", "unfriendly", "neutral",
                                                                   "friendly", "honored", "revered",    "exalted"};

// The people words of a character, indexed by the game's race id (empty for an id without one): "a human warrior".
inline constexpr char const* RACE_WORDS[] = {"",       "human", "orc",   "dwarf", "night elf", "undead",
                                             "tauren", "gnome", "troll", "",      "blood elf", "draenei"};
// The calling words of a character, indexed by the game's class id (empty for an id without one): "a human warrior".
inline constexpr char const* CLASS_WORDS[] = {
    "", "warrior", "paladin", "hunter", "rogue", "priest", "death knight", "shaman", "mage", "warlock", "", "druid"};
}  // namespace Facts

// ---------------------------------------------------------------------------------------------
// 2. Nature: trait words and interests
// ---------------------------------------------------------------------------------------------
namespace Nature
{
// The words of each trait, in the Trait enum's order: Low and High are the two poles. Each pole has three words, one
// for each strength of the trait: [0] slight, [1] noticeable, [2] strong. A bot's nature tells the model the word of
// the strength its value has (TraitRules.h Word); every word is its own, none is another word with a qualifier in
// front. In-world words only: no numbers, nothing about mechanics, no comma (the words are joined into a comma list),
// no "your".
struct TraitWords
{
    std::string_view Name;  // the GM command's name for the trait
    std::array<std::string_view, 3> Low;
    std::array<std::string_view, 3> High;
};

// Loyalty is about the bonds a character chose (friends, a guild, a group); zeal about those it was born into (its
// people, its faction). Generosity runs from greedy to generous; thrift is how freely a character spends on itself.
inline constexpr std::array<TraitWords, 21> WORDS = {{
    {"drive", {"unhurried", "relaxed", "laid-back"}, {"purposeful", "driven", "ambitious"}},
    {"curiosity", {"down-to-earth", "hard to impress", "incurious"}, {"interested", "curious", "inquisitive"}},
    {"caution", {"bold", "daring", "reckless"}, {"prudent", "careful", "overcautious"}},
    {"aggression", {"peaceable", "conciliatory", "a pacifist"}, {"combative", "belligerent", "bloodthirsty"}},
    {"sociability", {"private", "solitary", "a loner"}, {"outgoing", "sociable", "gregarious"}},
    {"loyalty",
     {"independent", "self-reliant", "bound to no one"},
     {"a loyal friend", "a devoted friend", "fiercely loyal to friends"}},
    {"generosity", {"acquisitive", "greedy", "insatiable"}, {"giving", "generous", "open-handed"}},
    {"thrift", {"free-spending", "extravagant", "a spendthrift"}, {"thrifty", "frugal", "austere"}},
    {"orderliness", {"untidy", "scatterbrained", "chaotic"}, {"tidy", "methodical", "meticulous"}},
    {"patience", {"impatient", "short-tempered", "hot-tempered"}, {"even-tempered", "patient", "unflappable"}},
    {"courtesy", {"awkward", "tactless", "oblivious"}, {"considerate", "courteous", "chivalrous"}},
    {"talkativeness", {"quiet", "taciturn", "tight-lipped"}, {"talkative", "garrulous", "a chatterbox"}},
    {"warmth", {"cool", "aloof", "cold"}, {"friendly", "warm", "affectionate"}},
    {"humour", {"serious", "earnest", "humourless"}, {"playful", "a joker", "a clown"}},
    {"register", {"casual", "plain-spoken", "blunt"}, {"well-spoken", "formal", "ceremonious"}},
    {"confidence", {"modest", "humble", "meek"}, {"assertive", "proud", "commanding"}},
    {"outlook", {"downcast", "gloomy", "bleak"}, {"hopeful", "cheerful", "irrepressible"}},
    {"piety", {"lax in faith", "worldly", "irreverent"}, {"reverent", "devout", "deeply devout"}},
    {"zeal",
     {"even-handed", "unmoved by banners", "bound to no banner"},
     {"proud of homeland", "patriotic", "fiercely patriotic"}},
    {"suspicion", {"unsuspecting", "trusting", "gullible"}, {"watchful", "suspicious", "distrustful"}},
    {"theatricality",
     {"not one for fancy words", "matter-of-fact", "deadpan"},
     {"expressive", "dramatic", "flamboyant"}},
}};

// Pairs of trait words that contradict each other. A nature never tells both: the word of the lower-ranking trait is
// left out (TraitRules.h ToldOrder). The ranks: a trait that also steers what the bot does before one that only
// colours its talk, then the stronger trait, then the trait in fewer pairs here, then a coin the bot's own values
// flip. Each word must be a word of WORDS, the two of different traits; the build fails otherwise.
struct WordConflict
{
    std::string_view First;
    std::string_view Second;
};

inline constexpr WordConflict CONFLICTS[] = {
    // Easy-going against a temper and against a taste for fights
    {"unhurried", "impatient"},
    {"relaxed", "impatient"},
    {"relaxed", "short-tempered"},
    {"relaxed", "hot-tempered"},
    {"relaxed", "belligerent"},
    {"laid-back", "impatient"},
    {"laid-back", "short-tempered"},
    {"laid-back", "hot-tempered"},
    {"laid-back", "belligerent"},
    // Peaceful against a temper, hostile against calm and kind
    {"peaceable", "short-tempered"},
    {"peaceable", "hot-tempered"},
    {"conciliatory", "short-tempered"},
    {"conciliatory", "hot-tempered"},
    {"belligerent", "even-tempered"},
    {"belligerent", "friendly"},
    {"belligerent", "warm"},
    {"belligerent", "considerate"},
    {"belligerent", "courteous"},
    // Careful against careless
    {"reckless", "methodical"},
    {"reckless", "meticulous"},
    {"careful", "scatterbrained"},
    {"overcautious", "belligerent"},
    {"overcautious", "bloodthirsty"},
    {"prudent", "extravagant"},
    {"prudent", "a spendthrift"},
    // Meek against bold, fierce, hot and loud
    {"meek", "bold"},
    {"meek", "daring"},
    {"meek", "reckless"},
    {"meek", "combative"},
    {"meek", "belligerent"},
    {"meek", "bloodthirsty"},
    {"meek", "short-tempered"},
    {"meek", "hot-tempered"},
    {"meek", "blunt"},
    {"meek", "flamboyant"},
    {"modest", "flamboyant"},
    // Distant against fond of company
    {"aloof", "outgoing"},
    {"aloof", "sociable"},
    {"aloof", "gregarious"},
    {"aloof", "garrulous"},
    {"aloof", "a chatterbox"},
    {"cold", "sociable"},
    {"cold", "gregarious"},
    {"cold", "considerate"},
    // Plain against showy
    {"austere", "flamboyant"},
    {"down-to-earth", "flamboyant"},
    {"down-to-earth", "extravagant"},
    {"worldly", "austere"},
    {"not one for fancy words", "ceremonious"},
    {"matter-of-fact", "ceremonious"},
    {"blunt", "courteous"},
    // Calm against stirred, still against lively
    {"unflappable", "dramatic"},
    {"deadpan", "hot-tempered"},
    {"deadpan", "irrepressible"},
    {"expressive", "taciturn"},
    {"expressive", "tight-lipped"},
    // Aware against unaware
    {"oblivious", "watchful"},
    {"hard to impress", "gullible"},
    {"awkward", "commanding"},
};

// The interests, in the Interest enum's order, as the model reads them in a bot's nature; no commas, the names are
// joined into comma lists.
inline constexpr std::array<std::string_view, 10> INTEREST_NAMES = {
    "fishing",      "cooking", "collecting pets and mounts",
    "crafting",     "trading", "duelling",
    "exploring",    "lore",    "inns and ale",
    "fine clothes",
};

// An interest and a trait word that contradict each other: a bot whose nature holds the word is never given the
// interest (TraitRules.h Roll draws another). First is a name of INTEREST_NAMES, Second a word of WORDS.
inline constexpr WordConflict INTEREST_CONFLICTS[] = {
    {"duelling", "peaceable"}, {"duelling", "conciliatory"}, {"duelling", "a pacifist"}, {"exploring", "incurious"},
    {"lore", "incurious"},     {"fine clothes", "thrifty"},  {"fine clothes", "frugal"}, {"fine clothes", "austere"},
};
}  // namespace Nature

// ---------------------------------------------------------------------------------------------
// 3. Deeds: the wording of a bot's record, its history and the others it knows
// ---------------------------------------------------------------------------------------------
namespace Deeds
{
// The rank word of a craft in a deed, indexed from the first rank a skill value of CRAFT_RANK_STEP times n completes (n
// from 1): "You mastered journeyman Mining".
inline constexpr char const* SKILL_RANK_WORDS[6] = {"apprentice", "journeyman", "expert",
                                                    "artisan",    "master",     "grand master"};

// The place of a deed when its place has no name: "in the wilds".
inline constexpr char const* WILDS = "the wilds";

// The creature of a kill when it has no name.
inline constexpr char const* A_FOE = "a creature";

// Follows the creature's name in a kill of a world boss.
inline constexpr char const* FEARSOME_FOE = ", a world boss,";

// Follows the creature's name in a kill of an elite.
inline constexpr char const* ELITE_FOE = ", an elite creature,";

// Follows the creature's name in a kill of a rare elite.
inline constexpr char const* RARE_FOE = ", a rare creature,";

// Follows the creature's name in a kill of a creature well above the bot's level.
inline constexpr char const* STRONGER_FOE = ", a creature stronger than you,";

// Opens the deed of a kill; the creature, its kind and the place follow.
inline constexpr char const* SLEW = "You slew ";

// Joins a deed to its place: "You slew a creature in Elwynn Forest."
inline constexpr char const* PLACE_IN = " in ";

// The deed of a quest without a known title; the place follows.
inline constexpr char const* FINISHED_QUEST = "You finished a quest";

// Opens the deed of a quest; the quest's title and the place follow.
inline constexpr char const* FINISHED_THE_QUEST = "You finished the quest ";

// Opens the deed of an item put on; the item's name and the place follow.
inline constexpr char const* PUT_ON = "You put on ";

// The item of an equip deed when the item has no name.
inline constexpr char const* NEW_GEAR = "new gear";

// The deed of a level that is a milestone; the place follows.
inline constexpr char const* MILESTONE = "You reached a milestone of experience in ";

// The deed of any other level; the place follows.
inline constexpr char const* GREW_IN_EXPERIENCE = "You grew in experience in ";

// The profession of a skill deed when the skill has no name.
inline constexpr char const* YOUR_CRAFT = "your profession";

// Opens the deed of a craft rank; the rank word, the craft and the place follow.
inline constexpr char const* MASTERED = "You mastered ";

// Opens the deed of a craft that grew without a new rank; the craft and the place follow.
inline constexpr char const* GREW_BETTER_AT = "You grew better at ";

// Opens the deed of a spell learned; the spell and the place follow.
inline constexpr char const* LEARNED = "You learned ";

// The spell of a learn deed when the spell has no name.
inline constexpr char const* A_NEW_SKILL = "a new skill";

// Opens the deed of a flight point; the flight point's name follows.
inline constexpr char const* FOUND_FLIGHT_MASTER = "You found the flight master at ";

// Opens the deed of a death by a creature; its name and the place follow.
inline constexpr char const* DIED_TO = "You died to ";

// Opens the deed of a death by a player; its name and the place follow.
inline constexpr char const* SLAIN_BY = "You were slain by ";

// Opens the deed of a death by an unknown hand; the place follows.
inline constexpr char const* DIED_IN = "You died in ";

// Opens the deed of a resurrection; the place follows.
inline constexpr char const* CAME_BACK = "You came back to life in ";

// Opens the deed of a fight with a companion; the companion's name and the place follow.
inline constexpr char const* FOUGHT_BESIDE = "You fought beside ";

// Opens the deed of a fight with a named creature; its name and the place follow.
inline constexpr char const* FOUGHT = "You fought ";

// Opens the deed of a fight without a named creature; the place follows.
inline constexpr char const* FOUGHT_A_BATTLE = "You fought a battle in ";

// Opens the deed of a dungeon or raid entered; its name follows.
inline constexpr char const* ENTERED = "You entered ";

// The dungeon of a deed when its map has no name.
inline constexpr char const* A_DUNGEON = "a dungeon";

// The deed of a stay with a group; the place follows.
inline constexpr char const* HELD_TOGETHER = "You held together with a group in ";

// Opens the deed of a long stay; the zone follows.
inline constexpr char const* SPENT_A_WHILE = "You spent a long while in ";

// Opens the deed of a cleared dungeon; its name follows.
inline constexpr char const* CLEARED = "You cleared ";

// The deed of joining a guild; the place follows.
inline constexpr char const* JOINED_GUILD = "You joined a guild in ";

// Opens the deed of a duel with a named rival; the rival's name and the place follow.
inline constexpr char const* DUEL_WITH = "You fought a duel with ";

// Opens the deed of a duel without a named rival; the place follows.
inline constexpr char const* DUEL_IN = "You fought a duel in ";

// The history sentence of a zone the bot knows; the zone follows.
inline constexpr char const* SPENT_TIME_IN = "You have spent time in ";

// The history sentence of a dungeon the bot knows; its name follows.
inline constexpr char const* BEEN_INSIDE = "You have been inside ";

// The history sentence of a boss the bot knows; its name follows.
inline constexpr char const* HELPED_BRING_DOWN = "You helped bring down ";

// What a known bot shared with the bot: kills together.
inline constexpr char const* SLEW_FOES_WITH_YOU = "slew enemies with you";

// What a known bot shared with the bot: a fight side by side.
inline constexpr char const* FOUGHT_BESIDE_YOU = "fought beside you";

// What a known bot shared with the bot: a kill of the bot.
inline constexpr char const* ONCE_KILLED_YOU = "once killed you";

// What a known bot shared with the bot: a group held together.
inline constexpr char const* HELD_GROUP_WITH_YOU = "held a group together with you";

// What a known bot shared with the bot: a cleared dungeon.
inline constexpr char const* CLEARED_WITH_YOU = "cleared a dungeon with you";

// What a known bot shared with the bot: a duel.
inline constexpr char const* DUEL_WITH_YOU = "fought a duel with you";

// What a known bot shared with the bot when nothing more specific is known.
inline constexpr char const* CROSSED_PATHS = "crossed paths with you";

// Joins two different things a known bot shared.
inline constexpr char const* AND = " and ";

// The article before a people word that starts with a vowel: "an orc".
inline constexpr char const* ARTICLE_AN = "an ";

// The article before any other people word: "a human warrior".
inline constexpr char const* ARTICLE_A = "a ";
}  // namespace Deeds

// ---------------------------------------------------------------------------------------------
// 4. Acts: for each act its sentence, its state phrase and the sentences of its own parameters
// ---------------------------------------------------------------------------------------------
namespace Acts
{
// Parameter sentences shared by several acts (and a topic).
// The catalogue's sentence of the quest parameter of quest and share quest.
inline constexpr char const* QUEST_QUEST = "A quest in your log, by its title.";

// The catalogue's sentence of the place parameter of go_to and meet_at, and of the topic route.
inline constexpr char const* TRIP_PLACE = "A known place's name, or x,y,z,map.";

// The catalogue's sentence of the act follow.
inline constexpr char const* FOLLOW_SENTENCE =
    "Follow the person you are talking to; they must be in your group and near you.";

// How the act follow reads in the bot's state.
inline constexpr char const* FOLLOW_STATE = "following you";

// The catalogue's sentence of the act stay.
inline constexpr char const* STAY_SENTENCE = "Stop and hold your current spot until told otherwise.";

// How the act stay reads in the bot's state.
inline constexpr char const* STAY_STATE = "holding this spot";

// The catalogue's sentence of the act flee.
inline constexpr char const* FLEE_SENTENCE = "Back away once from the enemy you are fighting.";

// How the act flee reads in the bot's state.
inline constexpr char const* FLEE_STATE = "backing away from my enemy";

// The catalogue's sentence of the act attack.
inline constexpr char const* ATTACK_SENTENCE =
    "Attack a hostile or neutral creature you can see, by its name; never a player or a pet.";

// How the act attack reads in the bot's state. {target} stands for the parameter of that name.
inline constexpr char const* ATTACK_STATE = "attacking {target}";

// The catalogue's sentence of the target parameter of attack by name.
inline constexpr char const* ATTACK_BY_NAME_TARGET = "A hostile or neutral creature you can see, by name.";

// The catalogue's sentence of the act mark.
inline constexpr char const* MARK_SENTENCE =
    "Put a raid icon on a unit you can see; in a raid only as its leader or an assistant.";

// How the act mark reads in the bot's state. {target} stands for the parameter of that name.
inline constexpr char const* MARK_STATE = "marking {target}";

// The catalogue's sentence of the target parameter of mark.
inline constexpr char const* MARK_TARGET = "The unit to mark, by name.";

// The catalogue's sentence of the icon parameter of mark.
inline constexpr char const* MARK_ICON = "The raid icon to put on it.";

// The raid icons a mark may use, by the icon's index; the model picks one by word.
inline constexpr char const* const RAID_ICONS[] = {"star", "circle", "diamond", "triangle",
                                                   "moon", "square", "cross",   "skull"};

// The catalogue's sentence of the act focus_healing.
inline constexpr char const* FOCUS_HEALING_SENTENCE = "As a healer, heal one member of your group before the others.";

// How the act focus_healing reads in the bot's state. {target} stands for the parameter of that name.
inline constexpr char const* FOCUS_HEALING_STATE = "healing {target} first";

// The catalogue's sentence of the target parameter of focus heal.
inline constexpr char const* FOCUS_HEAL_TARGET = "The group member to heal first, by name.";

// The catalogue's sentence of the act save_mana.
inline constexpr char const* SAVE_MANA_SENTENCE =
    "Save mana in fights, or spend it freely, in the group you share with the speaker.";

// How the act save_mana reads in the bot's state.
inline constexpr char const* SAVE_MANA_STATE = "minding my mana";

// The catalogue's sentence of the enabled parameter of save mana.
inline constexpr char const* SAVE_MANA_ENABLED = "Yes to save mana, no to spend it freely.";

// The catalogue's sentence of the act formation.
inline constexpr char const* FORMATION_SENTENCE = "Change how you keep to the group you share with the speaker.";

// How the act formation reads in the bot's state. {shape} stands for the parameter of that name.
inline constexpr char const* FORMATION_STATE = "keeping a {shape} formation";

// The catalogue's sentence of the shape parameter of formation.
inline constexpr char const* FORMATION_SHAPE = "How the group keeps together.";

// The formations a group may keep, by FormationShape value minus one; the model picks one by word.
inline constexpr char const* const FORMATION_SHAPES[] = {"close", "spread", "line"};

// The catalogue's sentence of the act position.
inline constexpr char const* POSITION_SENTENCE = "Walk to a spot near you that you can reach on foot.";

// How the act position reads in the bot's state.
inline constexpr char const* POSITION_STATE = "walking to a spot nearby";

// The catalogue's sentence of the place parameter of position.
inline constexpr char const* POSITION_PLACE = "A spot near you, as x,y,z,map.";

// The catalogue's sentence of the act go_to.
inline constexpr char const* GO_TO_SENTENCE = "Travel to a place you know, by your own route.";

// How the act go_to reads in the bot's state. {place} stands for the parameter of that name.
inline constexpr char const* GO_TO_STATE = "travelling to {place}";

// The catalogue's sentence of the act quest.
inline constexpr char const* QUEST_SENTENCE = "Work on or turn in a quest from your log.";

// How the act quest reads in the bot's state. {quest} stands for the parameter of that name.
inline constexpr char const* QUEST_STATE = "working on {quest}";

// The catalogue's sentence of the act errands.
inline constexpr char const* ERRANDS_SENTENCE = "Run your errands in a town near you.";

// How the act errands reads in the bot's state.
inline constexpr char const* ERRANDS_STATE = "running my errands";

// The catalogue's sentence of the act set_hearth.
inline constexpr char const* SET_HEARTH_SENTENCE = "Go to the nearest fitting inn and make it your home.";

// How the act set_hearth reads in the bot's state.
inline constexpr char const* SET_HEARTH_STATE = "going to an inn to set my hearthstone";

// The catalogue's sentence of the act grind_here.
inline constexpr char const* GRIND_HERE_SENTENCE = "Hunt monsters around where you are.";

// How the act grind_here reads in the bot's state.
inline constexpr char const* GRIND_HERE_STATE = "hunting around here";

// The catalogue's sentence of the act gather_here.
inline constexpr char const* GATHER_HERE_SENTENCE =
    "Mine ore or pick herbs around where you are, as your professions allow.";

// How the act gather_here reads in the bot's state.
inline constexpr char const* GATHER_HERE_STATE = "gathering around here";

// The catalogue's sentence of the act meet_at.
inline constexpr char const* MEET_AT_SENTENCE = "Travel to a place to meet the person you are talking to there.";

// How the act meet_at reads in the bot's state. {place} stands for the parameter of that name.
inline constexpr char const* MEET_AT_STATE = "going to meet you at {place}";

// The catalogue's sentence of the act pause_plan.
inline constexpr char const* PAUSE_PLAN_SENTENCE = "Set your own plans aside and wait here, for ten minutes at most.";

// How the act pause_plan reads in the bot's state.
inline constexpr char const* PAUSE_PLAN_STATE = "waiting here for now";

// The catalogue's sentence of the act resume_plan.
inline constexpr char const* RESUME_PLAN_SENTENCE = "Go back to your own plans.";

// How the act resume_plan reads in the bot's state.
inline constexpr char const* RESUME_PLAN_STATE = "back to my own plans";

// The catalogue's sentence of the act emote.
inline constexpr char const* EMOTE_SENTENCE = "Make a gesture, at the person you are talking to when you see them.";

// How the act emote reads in the bot's state. {emote} stands for the parameter of that name.
inline constexpr char const* EMOTE_STATE = "making a {emote} gesture";

// The catalogue's sentence of the emote parameter of emote.
inline constexpr char const* EMOTE_EMOTE = "The gesture.";

// The gestures the model may make, by word; BridgeAbilities.cpp pairs them by position with the text emotes.
inline constexpr char const* const EMOTE_WORDS[] = {
    "wave",  "bow",   "cheer", "laugh", "dance", "salute", "kneel", "point",   "cry",     "shy",
    "thank", "hello", "bye",   "nod",   "no",    "yawn",   "sigh",  "whistle", "applaud", "congratulate"};

// The catalogue's sentence of the act roll.
inline constexpr char const* ROLL_SENTENCE = "Roll a random number from 1 to 100; your group sees it.";

// How the act roll reads in the bot's state.
inline constexpr char const* ROLL_STATE = "rolling a die";

// The catalogue's sentence of the act ready.
inline constexpr char const* READY_SENTENCE = "Answer your group's open ready check: you are ready.";

// How the act ready reads in the bot's state.
inline constexpr char const* READY_STATE = "ready";

// The catalogue's sentence of the act leave_group.
inline constexpr char const* LEAVE_GROUP_SENTENCE = "Leave your group.";

// How the act leave_group reads in the bot's state.
inline constexpr char const* LEAVE_GROUP_STATE = "leaving the group";

// The catalogue's sentence of the act join_group.
inline constexpr char const* JOIN_GROUP_SENTENCE =
    "Join the group of the person you are talking to, or invite them to yours.";

// How the act join_group reads in the bot's state.
inline constexpr char const* JOIN_GROUP_STATE = "joining your group";

// The catalogue's sentence of the act share_quest.
inline constexpr char const* SHARE_QUEST_SENTENCE = "Share a quest of your log with your group.";

// How the act share_quest reads in the bot's state. {quest} stands for the parameter of that name.
inline constexpr char const* SHARE_QUEST_STATE = "sharing {quest}";

// The catalogue's sentence of the act accept_quest.
inline constexpr char const* ACCEPT_QUEST_SENTENCE = "Take the quest a group member has just shared with you.";

// How the act accept_quest reads in the bot's state. {quest} stands for the parameter of that name.
inline constexpr char const* ACCEPT_QUEST_STATE = "taking up {quest}";

// The catalogue's sentence of the quest parameter of accept quest.
inline constexpr char const* ACCEPT_QUEST_QUEST = "The quest a group member shared with you, by its title.";

// The catalogue's sentence of the act pull.
inline constexpr char const* PULL_SENTENCE =
    "As your group's tank or with a ranged attack, draw one creature near your group into a fight.";

// How the act pull reads in the bot's state. {target} stands for the parameter of that name.
inline constexpr char const* PULL_STATE = "pulling {target}";

// The catalogue's sentence of the target parameter of pull.
inline constexpr char const* PULL_TARGET = "One creature near your group, by name.";

// The catalogue's sentence of the act cast.
inline constexpr char const* CAST_SENTENCE =
    "Cast a helpful spell of yours (a buff, a heal, a cure, a resurrection, conjured food or water, a portal) on a "
    "friend or yourself.";

// How the act cast reads in the bot's state. {spell}, {target} stand for the parameter of that name.
inline constexpr char const* CAST_STATE = "casting {spell} on {target}";

// The catalogue's sentence of the spell parameter of cast.
inline constexpr char const* CAST_SPELL = "A helpful spell of yours, by name.";

// The catalogue's sentence of the target parameter of cast.
inline constexpr char const* CAST_TARGET = "Whom it is for, by name; your own name for yourself.";

// The catalogue's sentence of the act attack_own_target.
inline constexpr char const* ATTACK_OWN_TARGET_SENTENCE = "Attack the enemy you have targeted.";

// The catalogue's sentence of the target parameter of attack.
inline constexpr char const* ATTACK_TARGET = "The enemy you have targeted, by name.";
}  // namespace Acts

// ---------------------------------------------------------------------------------------------
// 5. Lookups: the catalogue's sentence of each topic, then the words of their answers
// ---------------------------------------------------------------------------------------------
namespace Lookups
{
// The catalogue's sentence of the topic inventory.
inline constexpr char const* INVENTORY_SENTENCE = "The items you wear and carry, equipped ones first.";

// The catalogue's sentence of the topic quests.
inline constexpr char const* QUESTS_SENTENCE = "The quests in your log and how far along each one is.";

// The catalogue's sentence of the topic cooldowns.
inline constexpr char const* COOLDOWNS_SENTENCE = "Your spells still cooling down, longest first.";

// The catalogue's sentence of the topic strategies.
inline constexpr char const* STRATEGIES_SENTENCE = "The habits you follow in and out of a fight.";

// The catalogue's sentence of the topic spells.
inline constexpr char const* SPELLS_SENTENCE = "The class spells you know, each at its highest rank.";

// The catalogue's sentence of the topic plan.
inline constexpr char const* PLAN_SENTENCE = "What you are doing now, why, and your next stops.";

// The catalogue's sentence of the topic options.
inline constexpr char const* OPTIONS_SENTENCE = "The choices you weighed for what to do next.";

// The catalogue's sentence of the topic route.
inline constexpr char const* ROUTE_SENTENCE = "How you would travel to a place: the whole trip, then each leg.";

// The catalogue's sentence of the topic quest_plan.
inline constexpr char const* QUEST_PLAN_SENTENCE = "The quests you plan to take, do and turn in, in walking order.";

// The catalogue's sentence of the topic bags.
inline constexpr char const* BAGS_SENTENCE = "Your bags, what you keep in the bank and the mail that waits for you.";

// The catalogue's sentence of the topic places.
inline constexpr char const* PLACES_SENTENCE = "Where your hearthstone is bound and the flight points you know.";

// The catalogue's sentence of the topic history.
inline constexpr char const* HISTORY_SENTENCE =
    "What you did that mattered, in order, and the lands, dungeons and creatures you know.";

// The catalogue's sentence of the topic lately.
inline constexpr char const* LATELY_SENTENCE = "What you did of note lately, newest first.";

// The catalogue's sentence of the topic known.
inline constexpr char const* KNOWN_SENTENCE = "The others you have shared deeds with, and what you shared.";

// The catalogue's sentence of the topic needs.
inline constexpr char const* NEEDS_SENTENCE =
    "What you need: what you must pay for soon, what you are short of and what you are saving for. "
    "It changes, so look here before you answer what you need, also before you say you need nothing.";

// The needs topic's answer for a bot the upkeep rules do not govern.
inline constexpr char const* NEEDS_PROVIDED =
    "My gear and gold are provided for; I have nothing to pay for and nothing to save up for myself.";

// The needs topic's answer while the upkeep index is not ready.
inline constexpr char const* NEEDS_PENDING = "I have not worked out what I need to pay for yet.";

// The needs topic's answer with nothing to pay for.
inline constexpr char const* NEEDS_NOTHING = "I have nothing to pay for right now.";

// What a repair need buys.
inline constexpr char const* NEEDS_REPAIRS = "repairs";

// Follows the count of spells a training need buys, one or more: "3 spells at the warrior trainer".
inline constexpr char const* NEEDS_ONE_SPELL = " spell";
inline constexpr char const* NEEDS_SPELLS = " spells";

// Joins the spells to the trainer; the class word (Facts::CLASS_WORDS) and NEEDS_TRAINER follow.
inline constexpr char const* NEEDS_AT_THE = " at the ";
inline constexpr char const* NEEDS_TRAINER = " trainer";

// The trainer's class word for a class without one.
inline constexpr char const* NEEDS_ANY_CLASS = "class";

// What a pet need buys: a stable slot that keeps the old pet, or a new pet.
inline constexpr char const* NEEDS_OLD_PET_SLOT = "a stable slot for the old pet";
inline constexpr char const* NEEDS_NEW_PET = "a new pet";

// Opens a need without a price: due now, or paid when a trip passes by; what it buys follows.
inline constexpr char const* NEEDS_MUST_GET = "I must get ";
inline constexpr char const* NEEDS_WILL_GET = "I will get ";

// Opens a need with a price: due now, or paid when a trip passes by; what it buys follows.
inline constexpr char const* NEEDS_MUST_PAY = "I must pay for ";
inline constexpr char const* NEEDS_WILL_PAY = "I will pay for ";

// Follows what a need buys when the bot pays for it as a trip passes by.
inline constexpr char const* NEEDS_WHEN_PASSING = " when I pass by";

// Follows what a need without a price buys; what going without costs follows.
inline constexpr char const* NEEDS_NO_COST = ", at no cost; going without means ";

// How much of a need's price the bot's gold covers: all, a part (the amount between the two), none.
inline constexpr char const* NEEDS_ALL_SET_ASIDE = "the gold is set aside";
inline constexpr char const* NEEDS_PART_OPENS = "only ";
inline constexpr char const* NEEDS_PART_SET_ASIDE = " is set aside";
inline constexpr char const* NEEDS_NONE_SET_ASIDE = "none of it is set aside";

// Follows the gold set aside for a need with a price; what going without costs follows.
inline constexpr char const* NEEDS_GOING_WITHOUT = "; going without means ";

// What the bot saves for when no need and no riding rank names it.
inline constexpr char const* NEEDS_WHAT_IS_COMING = "what is coming";

// The reserve: NEEDS_RESERVE_KEEP, the gold kept back, NEEDS_RESERVE_HOLD, the gold the bot holds.
inline constexpr char const* NEEDS_RESERVE_KEEP = "I keep ";
inline constexpr char const* NEEDS_RESERVE_HOLD = " back for what is coming and hold ";

// The saving entry: NEEDS_SAVING, the shortfall, NEEDS_SHORT_FOR, what the bot saves for.
inline constexpr char const* NEEDS_SAVING = "I am saving: ";
inline constexpr char const* NEEDS_SHORT_FOR = " short for ";
}  // namespace Lookups

// ---------------------------------------------------------------------------------------------
// 6. Refusals: what the model is told for each reason
// ---------------------------------------------------------------------------------------------
namespace Refusals
{
// These are facts for the model to put into its own words; they are never said in chat as written.

// What the model is told when the reason is dead.
inline constexpr char const* DEAD = "You are dead.";

// What the model is told when the reason is battleground.
inline constexpr char const* BATTLEGROUND = "You are in a battleground, where this cannot be done.";

// What the model is told when the reason is travelling.
inline constexpr char const* TRAVELLING = "You are in flight; this cannot be done until you land.";

// What the model is told when the reason is not_with_you.
inline constexpr char const* NOT_WITH_YOU = "The one who asked is not the one you follow.";

// What the model is told when the reason is too_far.
inline constexpr char const* TOO_FAR = "The one who asked is too far away.";

// What the model is told when the reason is not_in_group.
inline constexpr char const* NOT_IN_GROUP = "The one who asked is not in your group.";

// What the model is told when the reason is unreachable.
inline constexpr char const* UNREACHABLE = "You cannot reach that spot from here.";

// What the model is told when the reason is target_not_visible.
inline constexpr char const* TARGET_NOT_VISIBLE = "You cannot see that target from here.";

// What the model is told when the reason is no_target.
inline constexpr char const* NO_TARGET = "You have no target to attack.";

// What the model is told when the reason is cannot_now.
inline constexpr char const* CANNOT_NOW = "This cannot be done right now.";

// What the model is told when the reason is not_planner.
inline constexpr char const* NOT_PLANNER = "Your plans are not yours to change right now.";

// What the model is told when the reason is goal_held.
inline constexpr char const* GOAL_HELD = "Your group holds to a goal, and you with it.";

// What the model is told when the reason is unknown_place.
inline constexpr char const* UNKNOWN_PLACE = "You do not know that place.";

// What the model is told when the reason is no_route.
inline constexpr char const* NO_ROUTE = "You know no way there.";

// What the model is told when the reason is no_such_quest.
inline constexpr char const* NO_SUCH_QUEST = "That quest is not in your quest log.";

// What the model is told when the reason is nothing_here.
inline constexpr char const* NOTHING_HERE = "There is nothing for you to do here.";

// What the model is told when the reason is already_there.
inline constexpr char const* ALREADY_THERE = "You are already there.";

// What the model is told when the reason is unknown_name.
inline constexpr char const* UNKNOWN_NAME = "You know nothing by that name.";

// ---------------------------------------------------------------------------------------------
// Told by the chat module (mod-ollama-chat), not by the registry
// ---------------------------------------------------------------------------------------------
// What the model is told when the reason is not_supported.
inline constexpr char const* NOT_SUPPORTED = "This cannot be done on request.";

// What the model is told when the reason is standing_not_supported.
inline constexpr char const* STANDING_NOT_SUPPORTED =
    "You can follow or stay until told otherwise, but not as a standing order.";

// What the model is told when the reason is too_soon.
inline constexpr char const* TOO_SOON = "You took an order a moment ago; it is too soon for another.";

// What the model is told when the reason is unavailable.
inline constexpr char const* UNAVAILABLE = "You cannot take orders right now.";

// What the model is told when the reason is busy.
inline constexpr char const* BUSY = "You are still finishing an earlier order.";

// What the model is told when the reason is not_fighting.
inline constexpr char const* NOT_FIGHTING = "You are not fighting anything.";

// What the model is told when the reason is not_busy.
inline constexpr char const* NOT_BUSY = "You are not busy with anything.";

// What the model is told when the reason is wait_too_short.
inline constexpr char const* WAIT_TOO_SHORT = "That wait is too short to be worth it.";

// What the model is told when the reason is wait_too_long.
inline constexpr char const* WAIT_TOO_LONG = "That wait is too long.";

// What the model is told when the reason is put_off.
inline constexpr char const* PUT_OFF = "You have already put this off as long as you can.";

// What the model is told when the reason is failed.
inline constexpr char const* FAILED = "You tried, but it did not work.";
}  // namespace Refusals

// ---------------------------------------------------------------------------------------------
// 7. Wakes: the sentence a wake turn opens with
// ---------------------------------------------------------------------------------------------
namespace Wakes
{
// Opens a wake turn when the fight the bot waited out is over.
inline constexpr char const* COMBAT_END = "Your fight is over.";

// Opens a wake turn when the activity the bot waited for is done.
inline constexpr char const* ACTIVITY_DONE = "You have finished what you were doing.";

// Opens a wake turn when the time the bot meant to wait has passed.
inline constexpr char const* TIME_PASSED = "The time you meant to wait has passed.";
}  // namespace Wakes

// ---------------------------------------------------------------------------------------------
// 8. Codes on the wire
// ---------------------------------------------------------------------------------------------
// Codes on the wire. A model does not read these as written: the bridge turns each into words (hub,
// bridge/claude/prompt-texts.mjs). They are listed here so both sides can be compared; they are not part of a
// wording review.
namespace Around
{
// The kind word of a hostile unit in the "around you" list.
inline constexpr char const* HOSTILE = "hostile";

// The kind word of a friendly unit that offers a service in the "around you" list.
inline constexpr char const* SERVICE = "service";

// The kind word of a quest giver or a quest object in the "around you" list.
inline constexpr char const* TASK = "task";

// The kind word of another player or bot in the "around you" list.
inline constexpr char const* PERSON = "person";

// The kind word of a game object in the "around you" list.
inline constexpr char const* OBJECT = "object";

// The danger word of a world boss.
inline constexpr char const* BOSS = "boss";

// The danger word of a hostile at or below the bot's gray level.
inline constexpr char const* TRIVIAL = "trivial";

// The danger word of an elite that is not trivial.
inline constexpr char const* ELITE = "elite";

// The danger word of a hostile far above the bot's level.
inline constexpr char const* DEADLY = "deadly";

// The danger word of a hostile above the bot's level.
inline constexpr char const* HARD = "hard";

// The danger word of a hostile below the bot's level.
inline constexpr char const* EASY = "easy";

// The danger word of a hostile of about the bot's level.
inline constexpr char const* EVEN = "even";

// The detail word of a friendly innkeeper in the "around you" list.
inline constexpr char const* INNKEEPER = "innkeeper";

// The detail word of a friendly flight master.
inline constexpr char const* FLIGHTMASTER = "flightmaster";

// The detail word of a friendly banker.
inline constexpr char const* BANKER = "banker";

// The detail word of a friendly auctioneer.
inline constexpr char const* AUCTIONEER = "auctioneer";

// The detail word of a friendly trainer.
inline constexpr char const* TRAINER = "trainer";

// The detail word of a friendly repairer.
inline constexpr char const* REPAIR = "repair";

// The detail word of a friendly vendor.
inline constexpr char const* VENDOR = "vendor";

// The detail word of a quest giver the bot can hand a quest in to.
inline constexpr char const* TASK_REWARD = "reward";

// The detail word of a quest giver the bot can take a quest from.
inline constexpr char const* TASK_QUEST = "quest";

// The detail word of a member of the bot's group.
inline constexpr char const* PERSON_GROUP = "group";

// The detail word of another bot that is not in the bot's group.
inline constexpr char const* PERSON_BOT = "bot";

// The detail word of a real player that is not in the bot's group.
inline constexpr char const* PERSON_PLAYER = "player";

// The detail word of a mailbox.
inline constexpr char const* OBJECT_MAILBOX = "mailbox";

// The detail word of an anvil.
inline constexpr char const* OBJECT_ANVIL = "anvil";

// The detail word of a forge.
inline constexpr char const* OBJECT_FORGE = "forge";

// The detail word of a cooking fire.
inline constexpr char const* OBJECT_FIRE = "fire";
}  // namespace Around

namespace Lookups
{
// The place word of the route topic when the place is a literal x,y,z,map position.
inline constexpr char const* ROUTE_LITERAL_PLACE = "position";

// The plan topic's goal row word when the bot has no goal.
inline constexpr char const* PLAN_NONE = "none";

// The plan topic's reason row word when the bot's plans are not its own.
inline constexpr char const* PLAN_NOT_PLANNER = "not_planner";

// The plan topic's reason row word when the bot's plan is paused.
inline constexpr char const* PLAN_PAUSED = "paused";

// The plan topic's reason row word when a plan is being worked out.
inline constexpr char const* PLAN_PLANNING = "planning";

// The plan topic's reason row word when nothing else says why there is no goal.
inline constexpr char const* PLAN_IDLE = "idle";

// The bags topic's row word for the free bag slots.
inline constexpr char const* BAGS_FREE_SLOTS = "free_slots";

// The bags topic's row word for the mail waiting at the mailbox.
inline constexpr char const* BAGS_MAIL_WAITING = "mail_waiting";

// The bags topic's row word for the gold in the mail the bot has seen.
inline constexpr char const* BAGS_MAIL_SEEN = "mail_seen";

// The bags topic's row word for the gold the bot expects in the mail.
inline constexpr char const* BAGS_MAIL_EXPECTED = "mail_expected";

// The bags topic's row word for the free bank slots.
inline constexpr char const* BAGS_BANK_FREE = "bank_free";

// The route topic's failure word for a place the bot does not know.
inline constexpr char const* ROUTE_UNKNOWN_PLACE = "unknown_place";

// The needs topic's row word for a bot the upkeep rules do not govern.
inline constexpr char const* NEEDS_ROW_PROVIDED = "provided";

// The needs topic's row word while the upkeep index is not ready.
inline constexpr char const* NEEDS_ROW_PENDING = "pending";

// The needs topic's row word with nothing to pay for.
inline constexpr char const* NEEDS_ROW_NOTHING = "nothing";

// The needs topic's row word for the gold kept back.
inline constexpr char const* NEEDS_ROW_RESERVE = "reserve";

// The needs topic's row word for the shortfall the bot saves up for.
inline constexpr char const* NEEDS_ROW_SAVING = "saving";

// The row words (BotStateItem::word) of the state topics, sent beside each row's code: one word of [a-z_] per kind of
// row, so a reader of the rows needs no table of codes. Not the `name` values above.
inline constexpr char const* ROW_GOAL = "goal";
inline constexpr char const* ROW_SOURCE = "source";
inline constexpr char const* ROW_REASON = "reason";
inline constexpr char const* ROW_STOP = "stop";
inline constexpr char const* ROW_OPTION = "option";
inline constexpr char const* ROW_CURRENT = "current";
inline constexpr char const* ROW_TOTAL = "total";
inline constexpr char const* ROW_LEG = "leg";
inline constexpr char const* ROW_FAILED = "failed";
inline constexpr char const* ROW_TAKE = "take";
inline constexpr char const* ROW_TASK = "task";
inline constexpr char const* ROW_TURN_IN = "turn_in";
inline constexpr char const* ROW_CARRIED = "carried";
inline constexpr char const* ROW_DEFERRED = "deferred";
inline constexpr char const* ROW_FREE = "free";
inline constexpr char const* ROW_SELL = "sell";
inline constexpr char const* ROW_BAG = "bag";
inline constexpr char const* ROW_MAIL = "mail";
inline constexpr char const* ROW_MAIL_SEEN = "mail_seen";
inline constexpr char const* ROW_MAIL_EXPECTED = "mail_expected";
inline constexpr char const* ROW_KEPT = "kept";
inline constexpr char const* ROW_BANK_FREE = "bank_free";
inline constexpr char const* ROW_HEARTH = "hearth";
inline constexpr char const* ROW_FLIGHT_HERE = "flight_here";
inline constexpr char const* ROW_FLIGHT_ELSEWHERE = "flight_elsewhere";
inline constexpr char const* ROW_EVENT = "event";
inline constexpr char const* ROW_ZONE = "zone";
inline constexpr char const* ROW_DUNGEON = "dungeon";
inline constexpr char const* ROW_BOSS = "boss";
inline constexpr char const* ROW_DEED = "deed";
inline constexpr char const* ROW_BOT = "bot";
inline constexpr char const* ROW_NEED_PROVIDED = "provided";
inline constexpr char const* ROW_NEED_NOTHING = "nothing";
inline constexpr char const* ROW_NEED_PENDING = "pending";
inline constexpr char const* ROW_NEED_DUE = "due";
inline constexpr char const* ROW_NEED_PASSING = "passing";
inline constexpr char const* ROW_NEED_RESERVE = "reserve";
inline constexpr char const* ROW_NEED_SAVING = "saving";

// The bags topic's sentences of the rows the bridge has no phrase for (the numbers stay in value and extra).
inline constexpr char const* BAGS_TEXT_MAIL_WAITING = "Mail is waiting for the bot at the mailbox.";
inline constexpr char const* BAGS_TEXT_MAIL_SEEN = "The bot saw gold in its mail and left it there.";
inline constexpr char const* BAGS_TEXT_MAIL_EXPECTED = "The bot expects gold to arrive by mail.";
inline constexpr char const* BAGS_TEXT_KEPT_BANK = " waits in the bank for a later level.";
inline constexpr char const* BAGS_TEXT_KEPT_BAGS = " is carried for a later level.";
inline constexpr char const* BAGS_TEXT_BANK_FREE = "The bot has room left in its bank.";

// The where topic (LC18a, BridgeKnowledgeRules.h): where a service, a person or a place is, if the bot can know it.
// The catalogue's sentence of the topic where.
inline constexpr char const* WHERE_SENTENCE = "Where a service, a person or a place is, if you know it, nearest first.";

// The where topic's one parameter: what to look for.
inline constexpr char const* WHERE_NAME_SENTENCE =
    "A service in plain words (vendor, repairs, trainer, inn, flight master, bank, mailbox, auction house, quest "
    "giver) or the name of a person or a place.";

// The where topic's row words: a thing the bot knows, nothing known, the places not gathered yet.
inline constexpr char const* ROW_WHERE_FOUND = "found";
inline constexpr char const* ROW_WHERE_UNKNOWN = "unknown";
inline constexpr char const* ROW_WHERE_PENDING = "pending";

// The where topic's one line when the bot knows nothing of the name, and while the places are not gathered yet. Neither
// repeats the name asked.
inline constexpr char const* WHERE_UNKNOWN = "You do not know where that is.";
inline constexpr char const* WHERE_PENDING = "The list of places is not ready yet.";

// A found line: "<name>, <what> in <town or zone>; <how far>, to the <direction>." The direction is left out for a
// place on another continent.
inline constexpr char const* WHERE_PART = ", ";
inline constexpr char const* WHERE_IN = " in ";
inline constexpr char const* WHERE_FAR_PART = "; ";
inline constexpr char const* WHERE_TOWARDS = ", to the ";
inline constexpr char const* WHERE_END = ".";

// What a town is, when a place is asked by its name.
inline constexpr char const* WHERE_TOWN = "town";

// How far a place is, nearest band first (the yards are in BridgeKnowledgeRules.h), then a place on another map.
inline constexpr std::array<char const*, 4> WHERE_DISTANCES = {"close by", "near", "far", "very far"};
inline constexpr char const* WHERE_OTHER_CONTINENT = "on another continent";

// The eight compass words, clockwise from north.
inline constexpr std::array<char const*, 8> WHERE_DIRECTIONS = {"north", "north-east", "east", "south-east",
                                                                "south", "south-west", "west", "north-west"};

// A service in plain words, in the order of Planner::ServiceKind: what the line calls it, and the words a question may
// use for it (compared without case, spaces or punctuation; empty = unused).
struct WhereService
{
    char const* What;
    std::array<char const*, 5> Asked;
};
inline constexpr std::array<WhereService, 10> WHERE_SERVICES = {{
    {"vendor", {"vendor", "vendors", "merchant", "shop", "trader"}},
    {"bag vendor", {"bag vendor", "bags", "bag", "", ""}},
    {"repairs", {"repairs", "repair", "armorer", "", ""}},
    {"trainer", {"trainer", "trainers", "training", "", ""}},
    {"innkeeper", {"inn", "innkeeper", "tavern", "", ""}},
    {"flight master", {"flight master", "flight point", "flights", "flight", ""}},
    {"banker", {"bank", "banker", "", "", ""}},
    {"mailbox", {"mailbox", "mail", "", "", ""}},
    {"auctioneer", {"auction house", "auctioneer", "auction", "", ""}},
    {"quest giver", {"quest giver", "quest givers", "quests", "", ""}},
}};
}  // namespace Lookups
}  // namespace BridgeTexts

#endif
