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

// The professions a bot may have, as the facts name them: the game's skill line id and the name.
struct Craft
{
    uint32_t Skill;
    std::string_view Name;
};

inline constexpr std::array<Craft, 14> CRAFTS = {{
    {171, "Alchemy"},
    {164, "Blacksmithing"},
    {333, "Enchanting"},
    {202, "Engineering"},
    {773, "Inscription"},
    {755, "Jewelcrafting"},
    {165, "Leatherworking"},
    {197, "Tailoring"},
    {185, "Cooking"},
    {129, "First Aid"},
    {356, "Fishing"},
    {182, "Herbalism"},
    {186, "Mining"},
    {393, "Skinning"},
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

// Opens and closes the type of a kill's creature that is no humanoid: "You slew Mangy Wolf, a beast, in Elwynn Forest."
inline constexpr char const* TYPE_OPEN = ", ";
inline constexpr char const* TYPE_CLOSE = ",";

// The generic word of the rank phrases above, which a creature's type word replaces.
inline constexpr char const* CREATURE_WORD = "creature";

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

// LC17e: the word a place parameter takes for the one the bot is talking with (where they stand at the act).
inline constexpr char const* PARTNER_PLACE = "you";

// The catalogue's sentence of the place parameter of go_to and meet_at; it names PARTNER_PLACE.
inline constexpr char const* TRIP_PLACE =
    "A known place's name, x,y,z,map, or \"you\" for where the person you are talking to stands now.";

// The catalogue's sentence of the place parameter of the topic route, which resolves no PARTNER_PLACE.
inline constexpr char const* ROUTE_PLACE = "A known place's name, or x,y,z,map.";

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

// The catalogue's sentence of the act use_item.
inline constexpr char const* USE_ITEM_SENTENCE =
    "Use an item you wear or carry (a potion, food, a scroll, a bandage, a trinket, a key) on yourself or on a unit, "
    "such as the player's target, by naming it.";

// How the act use_item reads in the bot's state. {item}, {target} stand for the parameter of that name.
inline constexpr char const* USE_ITEM_STATE = "using {item} on {target}";

// The catalogue's sentence of the item parameter of use_item.
inline constexpr char const* USE_ITEM_ITEM = "An item you wear or carry, by name.";

// The catalogue's sentence of the target parameter of use_item.
inline constexpr char const* USE_ITEM_TARGET =
    "Whom or what it is used on, by name; your own name for yourself or when it needs no target.";

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

// The catalogue's sentence of the topic items.
inline constexpr char const* ITEMS_SENTENCE =
    "Your items that can be used: what you carry by kind, what is not ready again yet, what you used last and what you "
    "lack of it for your role. Look here before you say what you could use or have used.";

// The items topic's lines (BridgeItemsTopic.h). A carried kind: ITEMS_CARRY, the kind, ITEMS_KIND, up to four names,
// ITEMS_AND_MORE when there are more.
inline constexpr char const* ITEMS_CARRY = "I carry ";
inline constexpr char const* ITEMS_KIND = " items: ";
inline constexpr char const* ITEMS_AND_MORE = " and more";

// The kind word of an item without a kind.
inline constexpr char const* ITEMS_OTHER = "other";

// An item on cooldown: ITEMS_MY, the name, ITEMS_NOT_READY.
inline constexpr char const* ITEMS_MY = "My ";
inline constexpr char const* ITEMS_NOT_READY = " is not ready again yet.";

// A last use: ITEMS_TRIED, the name, ITEMS_LATELY, how it went.
inline constexpr char const* ITEMS_TRIED = "I tried my ";
inline constexpr char const* ITEMS_LATELY = " not long ago: ";

// How a use went: worked; interrupted, then the cause's words; refused, then the cause's words.
inline constexpr char const* ITEMS_WORKED = "it worked";
inline constexpr char const* ITEMS_INTERRUPTED = "it was interrupted: ";
inline constexpr char const* ITEMS_COULD_NOT = "I could not: ";

// The items topic's answer for a bot whose supplies are provided.
inline constexpr char const* ITEMS_PROVIDED =
    "My supplies are provided for; I lack nothing for my role and keep no stock of my own.";

// A lack: ITEMS_SHORT_OF, the slot's words, ITEMS_FOR_ROLE.
inline constexpr char const* ITEMS_SHORT_OF = "I am short of ";
inline constexpr char const* ITEMS_FOR_ROLE = " supplies for my role.";

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
// What the model is told when the reason is too_soon.
inline constexpr char const* TOO_SOON = "You took an order a moment ago; it is too soon for another.";

// What the model is told when the reason is unavailable.
inline constexpr char const* UNAVAILABLE = "You cannot take orders right now.";

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
// 7a. Starters: what a starter tells the bot it just did (LC28a; the chat module reads them from LC28b on)
// ---------------------------------------------------------------------------------------------
namespace Starters
{
// One sentence per starter kind, in StarterRules::Kind order: loot, kill, quest, level, visit, need, praise.
inline constexpr char const* SENTENCES[] = {"You just put on an item that is better than the one you wore.",
                                            "You just killed a rare creature.",
                                            "You just completed a quest.",
                                            "You just reached a new level.",
                                            "You just arrived in a place you have never been to before.",
                                            "You just found that you cannot afford something you need.",
                                            "You just saw the player beside you put on a better item they can use."};
}  // namespace Starters

// ---------------------------------------------------------------------------------------------
// 8. Codes on the wire
// ---------------------------------------------------------------------------------------------
// Codes on the wire. A model does not read these as written: the bridge turns each into words (hub,
// bridge/claude/prompt-texts.mjs). They are listed here so both sides can be compared; they are not part of a
// wording review.
namespace Around
{
// The game's own word of a creature's type, indexed by the core's CreatureType number; empty for the types that name
// nothing worth saying (none, not specified, non-combat pet).
inline constexpr char const* TYPE_WORDS[14] = {"",      "beast",  "dragonkin", "demon",    "elemental",
                                               "giant", "undead", "humanoid",  "critter",  "mechanical",
                                               "",      "totem",  "",          "gas cloud"};

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
inline constexpr char const* ROW_ITEMS_CARRIED = "carried";
inline constexpr char const* ROW_ITEMS_COOLING = "cooling";
inline constexpr char const* ROW_ITEMS_USED = "used";
inline constexpr char const* ROW_ITEMS_LACKS = "lacks";
inline constexpr char const* ROW_ITEMS_PROVIDED = "provided";

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

// LC18g (LC.54): the side word a place's row carries (its `side` key; the bridge words the sentence), indexed by
// Knowledge::Side (neither: a neutral place). A place the game gives no side carries none.
inline constexpr std::array<char const*, 3> PLACE_SIDE_WORDS = {"neutral", "alliance", "horde"};

// After the line of a person or a place of the other side: its services are not offered to the bot. A where line of a
// person of the other side leaves its service word out.
inline constexpr char const* NOT_SERVED = " They do not serve you.";

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
inline constexpr std::array<WhereService, 12> WHERE_SERVICES = {{
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
    {"stable master", {"stable master", "stables", "stable", "", ""}},
    {"crafting station", {"", "", "", "", ""}},
}};

// The who, what and zone topics (LC18b, BridgeLookupRules.h): who a person is, what an item or a quest is, what a land
// is like, each if the bot can know it. The catalogue's sentences and the one parameter's sentence of each.
inline constexpr char const* WHO_SENTENCE =
    "Who a person is, if you know them: a person of the world or a bot you met.";
inline constexpr char const* WHO_NAME_SENTENCE = "The name of the person.";
inline constexpr char const* WHAT_SENTENCE = "What an item or a quest is, if you know it.";
inline constexpr char const* WHAT_NAME_SENTENCE = "The name of the item or of the quest.";
inline constexpr char const* ZONE_SENTENCE = "What a land is like, if you know it.";
inline constexpr char const* ZONE_NAME_SENTENCE = "The name of the land.";

// The row words: a person of the world, a bot met, an item, a quest, a land, nothing known, the lists not ready yet.
inline constexpr char const* ROW_LOOKUP_NPC = "npc";
inline constexpr char const* ROW_LOOKUP_BOT = "bot";
inline constexpr char const* ROW_LOOKUP_ITEM = "item";
inline constexpr char const* ROW_LOOKUP_QUEST = "quest";
inline constexpr char const* ROW_LOOKUP_ZONE = "zone";
inline constexpr char const* ROW_LOOKUP_UNKNOWN = "unknown";
inline constexpr char const* ROW_LOOKUP_PENDING = "pending";

// The one line when the bot knows nothing of the name (it never repeats the name), and while the lists are not ready.
inline constexpr char const* WHO_UNKNOWN = "You do not know who that is.";
inline constexpr char const* WHAT_UNKNOWN = "You do not know what that is.";
inline constexpr char const* ZONE_UNKNOWN = "You do not know that land.";
inline constexpr char const* LOOKUP_PENDING = "The lists of the world are not ready yet.";

// Level in words: the first word whose top is not below the level (the tops are in BridgeLookupRules.h).
inline constexpr std::array<char const*, 5> LEVEL_WORDS = {"the low levels", "the middle levels", "the high levels",
                                                           "the levels of Outland", "the levels of Northrend"};

// Whose a person or a land is, indexed by Knowledge::Side (neither, Alliance, Horde): a person's side and a land's.
inline constexpr std::array<char const*, 3> WHO_SIDES = {"of no side", "of the Alliance", "of the Horde"};
inline constexpr std::array<char const*, 3> ZONE_LANDS = {"a land of no side", "a land of the Alliance",
                                                          "a land of the Horde"};

// LC18g: a town in a land's line with its own side, indexed by Knowledge::Side; none for a town of no side.
inline constexpr std::array<char const*, 3> TOWN_SIDES = {" (neutral)", " (Alliance)", " (Horde)"};

// A person's line: "<name>, <what> <side>[ in <place>]."; what is a service word, or this one for none.
inline constexpr char const* WHO_PERSON = "person";

// An item's line: "<name>, <quality> <kind> of <level words>, <use>; you carry it." A quest's line:
// "<title>, quest of <level words>[, given by <name>[ in <place>]]; you have it." A land's line: "<name>, <land>[, for
// <level words>[ to <level words>]][; you know <towns>]."
inline constexpr char const* LOOKUP_SEP = ", ";
inline constexpr char const* LOOKUP_OF = " of ";
inline constexpr char const* LOOKUP_SEMI = "; ";
inline constexpr char const* LOOKUP_END = ".";
inline constexpr char const* LOOKUP_IN = " in ";
inline constexpr char const* LOOKUP_QUEST = "quest";
inline constexpr char const* LOOKUP_GIVEN_BY = ", given by ";
inline constexpr char const* LOOKUP_FOR = ", for ";
inline constexpr char const* LOOKUP_TO = " to ";
inline constexpr char const* LOOKUP_KNOW_TOWNS = "; you know ";
inline constexpr char const* LOOKUP_AND = " and ";
inline constexpr char const* LOOKUP_SPACE = " ";  // between the words of one phrase (word order is the text's)
inline constexpr char const* LOOKUP_CARRY = "you carry it";
inline constexpr char const* LOOKUP_NO_CARRY = "you do not carry it";
inline constexpr char const* LOOKUP_HAVE = "you have it";
inline constexpr char const* LOOKUP_DONE = "you have done it";
inline constexpr char const* LOOKUP_NO_HAVE = "you do not have it";

// An item's quality words (ITEM_QUALITY_POOR..HEIRLOOM), then its kind and what it is for, by item class (the game's
// class ids 0..16; the last row serves a class beyond them).
inline constexpr std::array<char const*, 8> ITEM_QUALITIES = {"poor", "common",    "uncommon", "rare",
                                                              "epic", "legendary", "artifact", "heirloom"};
struct ItemKindWords
{
    char const* Kind;
    char const* Use;
};
inline constexpr std::array<ItemKindWords, 18> ITEM_KINDS = {{
    {"consumable", "to use up"},
    {"bag", "to carry things in"},
    {"weapon", "to fight with"},
    {"gem", "to set into gear"},
    {"armor", "to wear"},
    {"reagent", "for casting"},
    {"ammunition", "to shoot"},
    {"trade good", "for crafting"},
    {"item", "for odd uses"},
    {"recipe", "to learn a craft"},
    {"coin", "to spend"},
    {"quiver", "to hold ammunition"},
    {"quest item", "for a quest"},
    {"key", "to open something"},
    {"item", "for odd uses"},
    {"miscellaneous item", "for odd uses"},
    {"glyph", "to shape a spell"},
    {"item", "for odd uses"},
}};

// The mounts and recipes topics (LC18c, LC18f, BridgeOwnRules.h): what the bot can ride and what it can make, from its
// own spellbook (its mounts, the recipes it has learned) and its crafts.
inline constexpr char const* MOUNTS_SENTENCE = "What you can ride: your riding and the mounts you own.";
inline constexpr char const* RECIPES_SENTENCE =
    "What you can make of a craft or of a thing to make, if you have the craft.";
inline constexpr char const* RECIPES_NAME_SENTENCE = "The name of the craft or of the thing to make.";

// The row words: your riding, a mount you own, a craft you have, a thing you know to make, nothing to tell.
inline constexpr char const* ROW_OWN_RIDING = "riding";
inline constexpr char const* ROW_OWN_MOUNT = "mount";
inline constexpr char const* ROW_OWN_CRAFT = "craft";
inline constexpr char const* ROW_OWN_RECIPE = "recipe";
inline constexpr char const* ROW_OWN_NONE = "none";

inline constexpr char const* OWN_RIDING_NAME = "Riding";
inline constexpr char const* OWN_MOUNT_LINE = ", a mount you own.";
inline constexpr char const* OWN_NO_MOUNT = "You own no mount.";
inline constexpr char const* OWN_NO_RIDING = "You cannot ride and own no mount.";
inline constexpr char const* OWN_MADE_WITH = "made with ";
inline constexpr char const* OWN_NO_RECIPE = "You cannot make anything of that.";

// The professions topic (WR18, BridgeProfessionsTopic.h): the professions the bot has, how far it is, what it works
// towards and what it lacks; what it can make is the recipes topic's.
inline constexpr char const* PROFESSIONS_SENTENCE =
    "Your professions: how far you are, what you work towards and what you lack. What you can make is in recipes.";

inline constexpr char const* ROW_PROF_PROVIDED = "provided";
inline constexpr char const* ROW_PROF_NOTHING = "none";
inline constexpr char const* ROW_PROF_PROFESSION = "profession";
inline constexpr char const* ROW_PROF_LACKS = "lacks";

inline constexpr char const* PROF_PROVIDED = "You follow no craft of your own.";
inline constexpr char const* PROF_NOTHING = "You have no profession.";
inline constexpr char const* PROF_MAKING = ", making things to get better at it, towards ";
inline constexpr char const* PROF_TOWARDS = ", working towards ";
inline constexpr char const* PROF_NO_GOAL = ", with nothing more in view";
inline constexpr char const* PROF_GOAL_RECIPE = "a recipe you cannot make yet";
inline constexpr char const* PROF_GOAL_NODE = "a kind of node you cannot gather yet";
inline constexpr char const* PROF_GOAL_SKINNABLE = "creatures you cannot skin yet";
inline constexpr char const* PROF_GOAL_RANK = "a higher rank from a trainer";
inline constexpr char const* PROF_LACKS_LEAD = "You still lack ";
inline constexpr char const* PROF_LACK_RECIPE = "a recipe";
inline constexpr char const* PROF_LACK_RANK = "a higher rank";
inline constexpr char const* PROF_LACK_TOOL = "a tool";
inline constexpr char const* PROF_LACK_MATERIALS = "materials";

// The auction topic (WA9, BridgeAuctionTopic.h): what the bot has listed and at what price, what it looks for and its
// open bids. Its own prices only; money comes worded by the caller.
inline constexpr char const* AUCTION_SENTENCE =
    "What you have listed at the auction house and at what price, what you are looking for and what you would pay at "
    "most, and your open bids.";

inline constexpr char const* ROW_AUCTION_PROVIDED = "provided";
inline constexpr char const* ROW_AUCTION_NOTHING = "none";
inline constexpr char const* ROW_AUCTION_LISTED = "listed";
inline constexpr char const* ROW_AUCTION_WANTS = "wants";
inline constexpr char const* ROW_AUCTION_BIDS = "bids";

inline constexpr char const* AUCTION_PROVIDED = "You trade nothing of your own at the auction house.";
inline constexpr char const* AUCTION_NOTHING = "You have nothing listed, are looking for nothing and have no bid open.";
inline constexpr char const* AUCTION_LISTED_LEAD = "You have listed ";
inline constexpr char const* AUCTION_STACK_OF = "a stack of ";
inline constexpr char const* AUCTION_AT_BUYOUT = " at a buyout of ";
inline constexpr char const* AUCTION_AT_BID = " from a bid of ";
inline constexpr char const* AUCTION_WANTS_LEAD = "You are looking for ";
inline constexpr char const* AUCTION_WANTS_PAY = " and would pay at most ";
inline constexpr char const* AUCTION_WANTS_EACH = " for each";
inline constexpr char const* AUCTION_BIDS_OPEN = "You have bids open on auctions you may still win.";
inline constexpr char const* AUCTION_BIDS_FULL = " That is as many as you hold at once.";
}  // namespace Lookups
}  // namespace BridgeTexts

#endif
