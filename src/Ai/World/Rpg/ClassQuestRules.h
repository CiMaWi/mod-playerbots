/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CLASSQUESTRULES_H
#define _PLAYERBOT_CLASSQUESTRULES_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

// Upkeep 3.11 (WU20a): the class quests a bot learns a spell from, the steps of their chains, and the unlock bonus a
// governed bot (GoalInterface.h Governed) gets on the next one it can do. Std only, so tests/class-quest-selftest.cpp
// checks it; the planner job (PlannerQuests.cpp GoalsCompute) applies it on the worker from the snapshot's facts.
// The table comes from the hub's config/wu-class-quests.json (census 2026-11-11) and acore_world's chains: a step's
// prerequisites are its PrevQuestID and every quest whose NextQuestID names it (the core's Quest::prevQuests; any one
// rewarded unlocks it). The trainer quests (their spell is on the class trainer: 31, 5061, 4490, 7463, 7631, 7647,
// 9737, 11001, 12172, 12173) are not here: they are ordinary optional quests.
namespace Planner::ClassQuests
{
// Why a quest is or is not doable by a bot today (the census's cause words)
enum class Cause : std::uint8_t
{
    Value,        // ordinary objectives: talk, deliver, kill spawned creatures; only its value kept bots away
    Objective,    // an item used on a creature, at an object or a place, a spell on an NPC: automatic until WU20c
    Instance,     // in a dungeon or for a group
    Scripted,     // an event or summoned creatures (9364's Polymorph Clones have no spawn)
    Placeholder,  // no giver
    Trainer,      // its spell is on the bot's class trainer: the quest left the table
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Cause::Count)> CAUSE_WORDS = {
    "value", "objective", "instance", "scripted", "placeholder", "trainer"};
inline char const* CauseWord(Cause cause) { return CAUSE_WORDS[static_cast<std::size_t>(cause)]; }

enum class State : std::uint8_t
{
    Doable,     // the bot can do it now: it gets the bonus
    Automatic,  // the level-up grant teaches its spell (WU.7)
    Ordinary    // not a class quest for this bot: the trainer teaches its spell
};

// [E] The unlock bonus in XP: this share of the XP the bot needs for its next level, added to the quest's value. Large
// (a class quest beats most camps and pays a detour of a few quests' worth) but finite, so a far trip still loses to
// levelling where the bot is (WU.16). A tuning value read from the nights: time from unlock to done (NOTES watchlist)
constexpr float UNLOCK_BONUS_LEVEL_SHARE = 0.25f;
constexpr char const* REASON_WORD =
    "class_quest";                          // the goal and circuit rows' reason (UpkeepRules REASON_CLASS_QUEST)
constexpr std::uint32_t GIVE_UP_LIMIT = 2;  // a quest given up this often gets no bonus (its grant is WU21a's)
constexpr int MAX_CHAIN = 16;               // chain steps walked back from a class quest

struct Entry
{
    std::uint32_t quest;
    std::uint32_t classMask;  // 1 << (class - 1); 0: any
    std::uint32_t raceMask;   // 1 << (race - 1); 0: any
    std::uint32_t minLevel;
    std::array<std::uint32_t, 2> prev;  // one of them rewarded unlocks it; 0: none (a chain's first step)
    Cause cause;
    std::uint32_t rewardSpell;  // the spell the quest casts at its reward; 0 for a chain step
    std::uint32_t learns;       // the spell the bot learns from it
};

// Sorted by quest id. A row with a reward spell is a class quest, the others are steps of a chain before one
inline constexpr Entry TABLE[] = {
    {63, 64, 690, 20, {220, 0}, Cause::Objective, 0, 0},             // Call of Water
    {96, 64, 690, 20, {100, 0}, Cause::Placeholder, 5396, 5394},     // Call of Water
    {100, 64, 690, 20, {63, 0}, Cause::Value, 0, 0},                 // Call of Water
    {220, 64, 690, 20, {1534, 0}, Cause::Objective, 0, 0},           // Call of Water
    {1471, 256, 16, 10, {1473, 0}, Cause::Objective, 11520, 697},    // The Binding
    {1472, 256, 690, 20, {0, 0}, Cause::Value, 0, 0},                // Devourer of Souls
    {1473, 256, 16, 10, {0, 0}, Cause::Objective, 0, 0},             // Creature of the Void
    {1474, 256, 690, 20, {1476, 0}, Cause::Objective, 11519, 712},   // The Binding
    {1476, 256, 690, 20, {1472, 0}, Cause::Objective, 0, 0},         // Hearts of the Pure
    {1498, 1, 690, 10, {1505, 0}, Cause::Value, 8121, 71},           // Path of Defense
    {1501, 256, 2, 10, {0, 0}, Cause::Objective, 0, 0},              // Creature of the Void
    {1504, 256, 2, 10, {1501, 0}, Cause::Objective, 11520, 697},     // The Binding
    {1505, 1, 690, 10, {0, 0}, Cause::Value, 0, 0},                  // Veteran Uzzek
    {1507, 256, 690, 20, {0, 0}, Cause::Value, 0, 0},                // Devourer of Souls
    {1508, 256, 690, 20, {1507, 0}, Cause::Value, 0, 0},             // Blind Cazul
    {1509, 256, 690, 20, {1508, 0}, Cause::Value, 0, 0},             // News of Dogran
    {1510, 256, 690, 20, {1509, 0}, Cause::Value, 0, 0},             // News of Dogran
    {1511, 256, 690, 20, {1510, 0}, Cause::Objective, 0, 0},         // Ken'zigla's Draught
    {1512, 256, 690, 20, {1515, 0}, Cause::Objective, 0, 0},         // Love's Gift
    {1513, 256, 690, 20, {1512, 0}, Cause::Objective, 11519, 712},   // The Binding
    {1515, 256, 690, 20, {1511, 0}, Cause::Objective, 0, 0},         // Dogran's Captivity
    {1524, 64, 690, 10, {0, 0}, Cause::Objective, 0, 0},             // Call of Fire
    {1525, 64, 690, 10, {1524, 0}, Cause::Objective, 0, 0},          // Call of Fire
    {1526, 64, 690, 10, {1525, 0}, Cause::Objective, 0, 0},          // Call of Fire
    {1527, 64, 690, 10, {1526, 0}, Cause::Objective, 2075, 3599},    // Call of Fire
    {1530, 64, 690, 20, {0, 0}, Cause::Value, 0, 0},                 // Call of Water
    {1534, 64, 690, 20, {1536, 0}, Cause::Objective, 0, 0},          // Call of Water
    {1535, 64, 690, 20, {1530, 0}, Cause::Objective, 0, 0},          // Call of Water
    {1536, 64, 690, 20, {1535, 0}, Cause::Objective, 0, 0},          // Call of Water
    {1638, 1, 1101, 10, {0, 0}, Cause::Value, 0, 0},                 // A Warrior's Training
    {1639, 1, 1101, 10, {1638, 0}, Cause::Value, 0, 0},              // Bartleby the Drunk
    {1640, 1, 1101, 10, {1639, 0}, Cause::Value, 0, 0},              // Beat Bartleby
    {1642, 2, 1, 12, {0, 0}, Cause::Value, 0, 0},                    // The Tome of Divinity
    {1643, 2, 1, 12, {1642, 2998}, Cause::Value, 0, 0},              // The Tome of Divinity
    {1644, 2, 1, 12, {1643, 0}, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1646, 2, 4, 12, {0, 0}, Cause::Value, 0, 0},                    // The Tome of Divinity
    {1647, 2, 4, 12, {1646, 2997}, Cause::Value, 0, 0},              // The Tome of Divinity
    {1648, 2, 4, 12, {1647, 0}, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1649, 2, 1101, 20, {0, 0}, Cause::Value, 0, 0},                 // The Tome of Valor
    {1650, 2, 1101, 20, {1649, 0}, Cause::Value, 0, 0},              // The Tome of Valor
    {1651, 2, 1101, 20, {1650, 0}, Cause::Value, 0, 0},              // The Tome of Valor
    {1652, 2, 1101, 20, {1651, 0}, Cause::Value, 5503, 5502},        // The Tome of Valor
    {1661, 2, 1101, 40, {0, 0}, Cause::Placeholder, 13820, 13819},   // The Tome of Nobility
    {1665, 1, 1101, 10, {1640, 0}, Cause::Value, 8121, 71},          // Bartleby's Mug
    {1678, 1, 68, 10, {1679, 0}, Cause::Value, 8121, 71},            // Vejrek
    {1679, 1, 68, 10, {0, 0}, Cause::Value, 0, 0},                   // Muren Stormpike
    {1683, 1, 1101, 10, {1684, 0}, Cause::Value, 8121, 71},          // Vorlus Vilehoof
    {1684, 1, 1101, 10, {0, 0}, Cause::Value, 0, 0},                 // Elanaria
    {1688, 256, 1101, 10, {0, 0}, Cause::Objective, 0, 0},           // Surena Caledon
    {1689, 256, 1101, 10, {1688, 0}, Cause::Objective, 11520, 697},  // The Binding
    {1716, 256, 1101, 20, {0, 0}, Cause::Value, 0, 0},               // Devourer of Souls
    {1718, 1, 0, 30, {0, 0}, Cause::Value, 0, 0},                    // The Islander
    {1719, 1, 0, 30, {1718, 0}, Cause::Scripted, 8616, 2458},        // The Affray
    {1738, 256, 1101, 20, {1716, 0}, Cause::Objective, 0, 0},        // Heartswood
    {1739, 256, 1101, 20, {1738, 0}, Cause::Objective, 11519, 712},  // The Binding
    {1758, 256, 1101, 30, {1798, 0}, Cause::Value, 0, 0},            // Tome of the Cabal
    {1778, 2, 4, 12, {1648, 0}, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1779, 2, 4, 12, {1778, 0}, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1780, 2, 1, 12, {1644, 0}, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1781, 2, 1, 12, {1780, 0}, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1783, 2, 4, 12, {1779, 0}, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1784, 2, 4, 12, {1783, 0}, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1785, 2, 4, 12, {1784, 0}, Cause::Value, 7329, 7328},           // The Tome of Divinity
    {1786, 2, 1, 12, {1781, 0}, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1787, 2, 1, 12, {1786, 0}, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1788, 2, 1, 12, {1787, 0}, Cause::Value, 7329, 7328},           // The Tome of Divinity
    {1795, 256, 0, 30, {1804, 1805}, Cause::Objective, 1373, 691},   // The Binding
    {1798, 256, 1101, 30, {0, 0}, Cause::Value, 0, 0},               // Seeking Strahad
    {1801, 256, 690, 30, {0, 0}, Cause::Value, 0, 0},                // Tome of the Cabal
    {1802, 256, 1101, 30, {1758, 0}, Cause::Objective, 0, 0},        // Tome of the Cabal
    {1803, 256, 690, 30, {1801, 0}, Cause::Objective, 0, 0},         // Tome of the Cabal
    {1804, 256, 1101, 30, {1802, 0}, Cause::Objective, 0, 0},        // Tome of the Cabal
    {1805, 256, 690, 30, {1803, 0}, Cause::Objective, 0, 0},         // Tome of the Cabal
    {1818, 1, 690, 10, {0, 0}, Cause::Value, 0, 0},                  // Speak with Dillinger
    {1819, 1, 690, 10, {1818, 0}, Cause::Objective, 8121, 71},       // Ulag the Cleaver
    {2997, 2, 4, 12, {0, 0}, Cause::Value, 0, 0},                    // Tome of Divinity
    {2998, 2, 1, 12, {0, 0}, Cause::Value, 0, 0},                    // Tome of Divinity
    {2999, 2, 4, 12, {0, 0}, Cause::Value, 0, 0},                    // Tome of Divinity
    {3000, 2, 4, 12, {0, 0}, Cause::Value, 0, 0},                    // Tome of Divinity
    {3681, 2, 1, 12, {0, 0}, Cause::Value, 0, 0},                    // Tome of Divinity
    {5921, 1024, 8, 10, {0, 0}, Cause::Value, 0, 0},                 // Moonglade
    {5922, 1024, 32, 10, {0, 0}, Cause::Value, 0, 0},                // Moonglade
    {5929, 1024, 8, 10, {5921, 0}, Cause::Value, 0, 0},              // Great Bear Spirit
    {5930, 1024, 32, 10, {5922, 0}, Cause::Value, 0, 0},             // Great Bear Spirit
    {5931, 1024, 8, 10, {5929, 0}, Cause::Value, 0, 0},              // Back to Darnassus
    {5932, 1024, 32, 10, {5930, 0}, Cause::Value, 0, 0},             // Back to Thunder Bluff
    {6001, 1024, 8, 10, {5931, 0}, Cause::Objective, 19179, 5487},   // Body and Heart
    {6002, 1024, 32, 10, {5932, 0}, Cause::Objective, 19179, 5487},  // Body and Heart
    {6061, 4, 32, 10, {0, 0}, Cause::Objective, 0, 0},               // Taming the Beast
    {6062, 4, 130, 10, {0, 0}, Cause::Objective, 0, 0},              // Taming the Beast
    {6063, 4, 8, 10, {0, 0}, Cause::Objective, 0, 0},                // Taming the Beast
    {6064, 4, 4, 10, {0, 0}, Cause::Objective, 0, 0},                // Taming the Beast
    {6081, 4, 130, 10, {6082, 0}, Cause::Value, 5300, 6991},         // Training the Beast
    {6082, 4, 130, 10, {6083, 0}, Cause::Objective, 1579, 1515},     // Taming the Beast
    {6083, 4, 130, 10, {6062, 0}, Cause::Objective, 0, 0},           // Taming the Beast
    {6084, 4, 4, 10, {6064, 0}, Cause::Objective, 0, 0},             // Taming the Beast
    {6085, 4, 4, 10, {6084, 0}, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6086, 4, 4, 10, {6085, 0}, Cause::Value, 5300, 6991},           // Training the Beast
    {6087, 4, 32, 10, {6061, 0}, Cause::Objective, 0, 0},            // Taming the Beast
    {6088, 4, 32, 10, {6087, 0}, Cause::Objective, 1579, 1515},      // Taming the Beast
    {6089, 4, 32, 10, {6088, 0}, Cause::Value, 5300, 6991},          // Training the Beast
    {6101, 4, 8, 10, {6063, 0}, Cause::Objective, 0, 0},             // Taming the Beast
    {6102, 4, 8, 10, {6101, 0}, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6103, 4, 8, 10, {6102, 0}, Cause::Value, 5300, 6991},           // Training the Beast
    {6121, 1024, 1101, 14, {0, 0}, Cause::Value, 0, 0},              // Lessons Anew
    {6122, 1024, 1101, 14, {6121, 0}, Cause::Objective, 0, 0},       // The Principal Source
    {6123, 1024, 1101, 14, {6122, 0}, Cause::Objective, 0, 0},       // Gathering the Cure
    {6124, 1024, 1101, 14, {6123, 0}, Cause::Objective, 0, 0},       // Curing the Sick
    {6125, 1024, 1101, 14, {6124, 0}, Cause::Value, 8947, 8946},     // Power over Poison
    {6126, 1024, 690, 14, {0, 0}, Cause::Value, 0, 0},               // Lessons Anew
    {6127, 1024, 690, 14, {6126, 0}, Cause::Objective, 0, 0},        // The Principal Source
    {6128, 1024, 690, 14, {6127, 0}, Cause::Objective, 0, 0},        // Gathering the Cure
    {6129, 1024, 690, 14, {6128, 0}, Cause::Objective, 0, 0},        // Curing the Sick
    {6130, 1024, 690, 14, {6129, 0}, Cause::Value, 8947, 8946},      // Power over Poison
    {7581, 256, 0, 60, {0, 0}, Cause::Objective, 0, 0},              // The Prison's Bindings
    {7582, 256, 0, 60, {7581, 0}, Cause::Objective, 0, 0},           // The Prison's Casing
    {7583, 256, 0, 60, {7582, 0}, Cause::Instance, 20700, 18540},    // Suppression
    {7601, 256, 0, 50, {0, 0}, Cause::Value, 0, 0},                  // What Niby Commands
    {7602, 256, 0, 50, {7601, 0}, Cause::Objective, 0, 0},           // Flawless Fel Essence
    {7603, 256, 0, 50, {7602, 0}, Cause::Objective, 1413, 1122},     // Kroshius' Infernal Core
    {9362, 128, 0, 60, {0, 0}, Cause::Objective, 0, 0},              // Warlord Krellian
    {9364, 128, 0, 60, {9362, 0}, Cause::Scripted, 28285, 28272},    // Fragmented Magic
    {9461, 64, 0, 10, {9468, 0}, Cause::Value, 0, 0},                // Call of Fire
    {9464, 64, 1101, 10, {0, 0}, Cause::Value, 0, 0},                // Call of Fire
    {9465, 64, 1101, 10, {9464, 0}, Cause::Objective, 0, 0},         // Call of Fire
    {9467, 64, 1101, 10, {9465, 0}, Cause::Objective, 0, 0},         // Call of Fire
    {9468, 64, 1101, 10, {9467, 0}, Cause::Objective, 0, 0},         // Call of Fire
    {9484, 4, 512, 10, {0, 0}, Cause::Objective, 0, 0},              // Taming the Beast
    {9485, 4, 512, 10, {9486, 0}, Cause::Objective, 1579, 1515},     // Taming the Beast
    {9486, 4, 512, 10, {9484, 0}, Cause::Objective, 0, 0},           // Taming the Beast
    {9501, 64, 1024, 20, {0, 0}, Cause::Objective, 0, 0},            // Call of Water
    {9503, 64, 1024, 20, {9501, 0}, Cause::Objective, 0, 0},         // Call of Water
    {9504, 64, 1024, 20, {9503, 0}, Cause::Objective, 0, 0},         // Call of Water
    {9508, 64, 1024, 20, {9504, 0}, Cause::Objective, 0, 0},         // Call of Water
    {9509, 64, 1024, 20, {9508, 0}, Cause::Objective, 5396, 5394},   // Call of Water
    {9529, 256, 512, 10, {0, 0}, Cause::Value, 0, 0},                // The Stone
    {9555, 64, 1101, 10, {9461, 0}, Cause::Value, 2075, 3599},       // Call of Fire
    {9582, 1, 1101, 10, {0, 0}, Cause::Objective, 8121, 71},         // Strength of One
    {9591, 4, 1024, 10, {0, 0}, Cause::Objective, 0, 0},             // Taming the Beast
    {9592, 4, 1024, 10, {9591, 0}, Cause::Objective, 0, 0},          // Taming the Beast
    {9593, 4, 1024, 10, {9592, 0}, Cause::Objective, 1579, 1515},    // Taming the Beast
    {9598, 2, 1024, 12, {0, 0}, Cause::Objective, 0, 0},             // Redemption
    {9600, 2, 1024, 12, {9598, 0}, Cause::Objective, 7329, 7328},    // Redemption
    {9619, 256, 512, 10, {9529, 0}, Cause::Objective, 11520, 697},   // The Rune of Summoning
    {9673, 4, 512, 10, {9485, 0}, Cause::Value, 5300, 6991},         // Beast Training
    {9675, 4, 1024, 10, {9593, 0}, Cause::Value, 5300, 6991},        // Beast Training
    {9678, 2, 512, 12, {0, 0}, Cause::Objective, 0, 0},              // The First Trial
    {9681, 2, 512, 12, {9678, 0}, Cause::Value, 0, 0},               // A Study in Power
    {9684, 2, 512, 12, {9681, 0}, Cause::Objective, 0, 0},           // Claiming the Light
    {9685, 2, 512, 12, {9684, 0}, Cause::Objective, 7329, 7328},     // Redeeming the Dead
    {9686, 2, 512, 20, {9690, 0}, Cause::Value, 0, 0},               // The Second Trial
    {9690, 2, 512, 20, {0, 0}, Cause::Value, 0, 0},                  // The Second Trial
    {9691, 2, 512, 20, {9686, 0}, Cause::Value, 5503, 5502},         // Return to Silvermoon
    {9712, 2, 512, 40, {0, 0}, Cause::Placeholder, 34768, 34769},    // The Thalassian Warhorse
    {10350, 1, 1101, 10, {9582, 0}, Cause::Value, 8121, 71},         // Behomat
    {12228, 128, 0, 60, {0, 0}, Cause::Placeholder, 28285, 28272},   // Reacquiring the Magic [PH]
    {12636, 32, 0, 55, {12848, 0}, Cause::Value, 0, 0},              // The Eye Of Acherus
    {12641, 32, 0, 55, {12636, 0}, Cause::Objective, 0, 0},          // Death Comes From On High
    {12657, 32, 0, 55, {12641, 0}, Cause::Value, 0, 0},              // The Might Of The Scourge
    {12670, 32, 0, 55, {12850, 0}, Cause::Value, 0, 0},              // The Scarlet Harvest
    {12680, 32, 0, 55, {12670, 0}, Cause::Objective, 0, 0},          // Grand Theft Palomino
    {12687, 32, 0, 55, {12680, 0}, Cause::Scripted, 52382, 48778},   // Into the Realm of Shadows
    {12751, 32, 0, 55, {0, 0}, Cause::Value, 0, 0},                  // A Sort Of Homecoming (after
                                                     // 12739,12742,12743,12744,12745,12746,12747,12748,12749,12750)
    {12754, 32, 0, 55, {12751, 0}, Cause::Objective, 0, 0},         // Ambush At The Overlook
    {12755, 32, 0, 55, {12754, 0}, Cause::Objective, 0, 0},         // A Meeting With Fate
    {12756, 32, 0, 55, {12755, 0}, Cause::Objective, 0, 0},         // The Scarlet Onslaught Emerges
    {12757, 32, 0, 55, {12756, 0}, Cause::Objective, 0, 0},         // Scarlet Armies Approach...
    {12778, 32, 0, 55, {12757, 0}, Cause::Value, 0, 0},             // The Scarlet Apocalypse
    {12779, 32, 0, 55, {12778, 0}, Cause::Objective, 0, 0},         // An End To All Things...
    {12800, 32, 0, 55, {12779, 0}, Cause::Value, 0, 0},             // The Lich King's Command
    {12801, 32, 0, 55, {12800, 0}, Cause::Scripted, 53821, 50977},  // The Light of Dawn
    {12842, 32, 0, 55, {0, 0}, Cause::Value, 0, 0},                 // Runeforging: Preparation For Battle (after 12619)
    {12848, 32, 0, 55, {12842, 0}, Cause::Objective, 0, 0},         // The Endless Hunger
    {12850, 32, 0, 55, {12657, 0}, Cause::Value, 0, 0},             // Report To Scourge Commander Thalanor
};

inline Entry const* Find(std::uint32_t quest)
{
    Entry const* end = std::end(TABLE);
    Entry const* it =
        std::lower_bound(std::begin(TABLE), end, quest, [](Entry const& e, std::uint32_t id) { return e.quest < id; });
    return it != end && it->quest == quest ? it : nullptr;
}

inline bool IsClassQuest(Entry const& entry) { return entry.rewardSpell != 0; }

// trainerTeaches(spell): the bot's class trainer teaches the spell (UpkeepIndex TrainerList taughtBy, WU2-fix)
template <typename TrainerTeaches>
State StateOf(Entry const& entry, TrainerTeaches const& trainerTeaches)
{
    if (IsClassQuest(entry) && entry.learns && trainerTeaches(entry.learns))
        return State::Ordinary;
    return entry.cause == Cause::Value ? State::Doable : State::Automatic;
}

inline bool Has(std::vector<std::uint32_t> const& sorted, std::uint32_t id)
{
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

// What the rule needs to know about a bot; every list sorted
struct BotFacts
{
    std::uint32_t cls{0};   // class id
    std::uint32_t race{0};  // race id
    std::uint32_t level{1};
    bool governed{false};
    std::uint32_t levelXp{0};  // XP from this level to the next
    std::vector<std::uint32_t> rewarded;
    std::vector<std::uint32_t> givenUp;      // quests given up GIVE_UP_LIMIT times or more
    std::vector<std::uint32_t> knownSpells;  // the class-quest spells the bot knows (learns and reward spells)
};

inline bool Fits(Entry const& entry, BotFacts const& bot)
{
    bool const cls = !entry.classMask || (bot.cls && ((entry.classMask >> (bot.cls - 1)) & 1u));
    bool const race = !entry.raceMask || (bot.race && ((entry.raceMask >> (bot.race - 1)) & 1u));
    return cls && race;
}

inline bool Unlocked(Entry const& entry, BotFacts const& bot)
{
    if (!entry.prev[0] && !entry.prev[1])
        return true;
    return (entry.prev[0] && Has(bot.rewarded, entry.prev[0])) || (entry.prev[1] && Has(bot.rewarded, entry.prev[1]));
}

// The chain's next step towards a class quest: the earliest not rewarded step the bot can reach, walking back from
// the class quest through the steps that fit its class and race. nullptr when the class quest is rewarded
inline Entry const* NextStep(Entry const& classQuest, BotFacts const& bot)
{
    Entry const* step = &classQuest;
    for (int depth = 0; depth < MAX_CHAIN; ++depth)
    {
        if (Has(bot.rewarded, step->quest))
            return nullptr;
        if (Unlocked(*step, bot))
            return step;
        Entry const* back = nullptr;
        for (std::uint32_t prev : step->prev)
        {
            Entry const* entry = prev ? Find(prev) : nullptr;
            if (entry && !back && Fits(*entry, bot))
                back = entry;
        }
        if (!back)
            return step;  // the chain before it is outside the table: the step waits for that
        step = back;
    }
    return step;
}

// Why a class quest gives a bot the bonus or not; the harness prints the word
enum class Verdict : std::uint8_t
{
    Bonus,
    NotGoverned,
    NotForBot,   // another class or race
    NotDoable,   // automatic, or the trainer teaches its spell
    SpellKnown,  // the bot knows its spell (a backstory, WU.8, or a grant)
    Rewarded,    // done
    GivenUp,     // the class quest or its step, GIVE_UP_LIMIT times
    StepWaits,   // the chain's next step is not doable yet, or its chain starts outside the table
    LowLevel,    // the next step's level
    NoRoute,     // no known way to the step's giver
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Verdict::Count)> VERDICT_WORDS = {
    "bonus",    "not_governed", "not_for_bot", "not_doable", "spell_known",
    "rewarded", "given_up",     "step_waits",  "low_level",  "no_route"};
inline char const* VerdictWord(Verdict verdict) { return VERDICT_WORDS[static_cast<std::size_t>(verdict)]; }

struct Judgement
{
    Verdict verdict{Verdict::NotForBot};
    Entry const* step{nullptr};  // the quest that gets the bonus (the class quest or its chain's next step)
};

// hasRoute(quest): the planner knows a way to the quest's giver (an indexed giver on a map it can travel to)
template <typename TrainerTeaches, typename HasRoute>
Judgement Judge(Entry const& classQuest, BotFacts const& bot, TrainerTeaches const& trainerTeaches,
                HasRoute const& hasRoute)
{
    if (!bot.governed)
        return {Verdict::NotGoverned, nullptr};
    if (!IsClassQuest(classQuest) || !Fits(classQuest, bot))
        return {Verdict::NotForBot, nullptr};
    if (StateOf(classQuest, trainerTeaches) != State::Doable)
        return {Verdict::NotDoable, nullptr};
    if (Has(bot.knownSpells, classQuest.learns) || Has(bot.knownSpells, classQuest.rewardSpell))
        return {Verdict::SpellKnown, nullptr};
    Entry const* step = NextStep(classQuest, bot);
    if (!step)
        return {Verdict::Rewarded, nullptr};
    if (Has(bot.givenUp, classQuest.quest) || Has(bot.givenUp, step->quest))
        return {Verdict::GivenUp, step};
    if (StateOf(*step, trainerTeaches) != State::Doable || !Unlocked(*step, bot))
        return {Verdict::StepWaits, step};
    if (bot.level < step->minLevel)
        return {Verdict::LowLevel, step};
    if (!hasRoute(step->quest))
        return {Verdict::NoRoute, step};
    return {Verdict::Bonus, step};
}

inline float BonusXp(BotFacts const& bot) { return UNLOCK_BONUS_LEVEL_SHARE * static_cast<float>(bot.levelXp); }

// Every quest that gets the bonus for this bot (quest id, bonus XP), sorted by quest; empty for an ungoverned bot
template <typename TrainerTeaches, typename HasRoute>
std::vector<std::pair<std::uint32_t, float>> Bonuses(BotFacts const& bot, TrainerTeaches const& trainerTeaches,
                                                     HasRoute const& hasRoute)
{
    std::vector<std::pair<std::uint32_t, float>> out;
    if (!bot.governed)
        return out;
    for (Entry const& entry : TABLE)
    {
        if (!IsClassQuest(entry))
            continue;
        Judgement const judgement = Judge(entry, bot, trainerTeaches, hasRoute);
        if (judgement.verdict == Verdict::Bonus)
            out.emplace_back(judgement.step->quest, BonusXp(bot));
    }
    std::sort(out.begin(), out.end());
    auto const same = [](std::pair<std::uint32_t, float> const& a, std::pair<std::uint32_t, float> const& b)
    { return a.first == b.first; };
    out.erase(std::unique(out.begin(), out.end(), same), out.end());
    return out;
}

// The class quests and their doable steps in the bot's log: protected from the circuit's abandon once taken
inline std::vector<std::uint32_t> Protected(std::vector<std::uint32_t> const& inLog, BotFacts const& bot)
{
    std::vector<std::uint32_t> out;
    if (!bot.governed)
        return out;
    for (std::uint32_t quest : inLog)
    {
        Entry const* entry = Find(quest);
        if (entry && Fits(*entry, bot) && entry->cause == Cause::Value && !Has(bot.knownSpells, entry->learns))
            out.push_back(quest);
    }
    return out;
}
}  // namespace Planner::ClassQuests

#endif
