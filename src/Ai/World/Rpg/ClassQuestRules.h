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
// The table comes from the hub's config/wu-class-quests.json (census 2026-11-11); the chains are the quest index's: a
// step's prerequisites are its PrevQuestID and every quest whose NextQuestID names it (the core's Quest::prevQuests;
// any one rewarded unlocks it). The trainer quests (their spell is on the class trainer: 31, 5061, 4490, 7463, 7631,
// 7647, 9737, 11001, 12172, 12173) are not here: they are ordinary optional quests.
namespace Planner::ClassQuests
{
// Why a quest is or is not doable by a bot today (the census's cause words)
enum class Cause : std::uint8_t
{
    Value,        // ordinary objectives: talk, deliver, kill spawned creatures; only its value kept bots away
    Objective,    // an item used on a creature, at an object or a place, a spell on an NPC: the executor, WT35b
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

// [E] The unlock bonus in XP: this share of the XP the bot needs for its next level, added to the quest's value. The
// goal model floors a class option at the best other rate plus (reward + bonus) / minutes (AddClassQuests), so the
// quest wins the next decision at any distance (J2); the bonus's size decides whether a circuit that takes it beats a
// lone trip to it. A tuning value read from the nights: time from unlock to done (NOTES watchlist)
constexpr float UNLOCK_BONUS_LEVEL_SHARE = 0.25f;
constexpr char const* REASON_WORD =
    "class_quest";                          // the goal and circuit rows' reason (UpkeepRules REASON_CLASS_QUEST)
constexpr std::uint32_t GIVE_UP_LIMIT = 2;  // a quest given up this often gets no bonus (its grant is WU21a's)
constexpr int MAX_CHAIN = 16;               // chain steps walked back from a class quest

// What the table itself knows. The limits (class, race, level) and the chain (the predecessors) are the quest index's
// (QuestRecord), joined into a Row; the table has no column the record holds
struct Entry
{
    std::uint32_t quest;
    Cause cause;
    std::uint32_t rewardSpell;  // the spell the quest casts at its reward; 0 for a chain step
    std::uint32_t learns;       // the spell the bot learns from it
};

// Sorted by quest id. A row with a reward spell is a class quest, the others are steps of a chain before one
inline constexpr Entry TABLE[] = {
    {63, Cause::Objective, 0, 0},               // Call of Water
    {96, Cause::Placeholder, 5396, 5394},       // Call of Water
    {100, Cause::Value, 0, 0},                  // Call of Water
    {220, Cause::Objective, 0, 0},              // Call of Water
    {1471, Cause::Objective, 11520, 697},       // The Binding
    {1472, Cause::Value, 0, 0},                 // Devourer of Souls
    {1473, Cause::Objective, 0, 0},             // Creature of the Void
    {1474, Cause::Objective, 11519, 712},       // The Binding
    {1476, Cause::Objective, 0, 0},             // Hearts of the Pure
    {1498, Cause::Value, 8121, 71},             // Path of Defense
    {1501, Cause::Objective, 0, 0},             // Creature of the Void
    {1504, Cause::Objective, 11520, 697},       // The Binding
    {1505, Cause::Value, 0, 0},                 // Veteran Uzzek
    {1507, Cause::Value, 0, 0},                 // Devourer of Souls
    {1508, Cause::Value, 0, 0},                 // Blind Cazul
    {1509, Cause::Value, 0, 0},                 // News of Dogran
    {1510, Cause::Value, 0, 0},                 // News of Dogran
    {1511, Cause::Objective, 0, 0},             // Ken'zigla's Draught
    {1512, Cause::Objective, 0, 0},             // Love's Gift
    {1513, Cause::Objective, 11519, 712},       // The Binding
    {1515, Cause::Objective, 0, 0},             // Dogran's Captivity
    {1524, Cause::Objective, 0, 0},             // Call of Fire
    {1525, Cause::Objective, 0, 0},             // Call of Fire
    {1526, Cause::Objective, 0, 0},             // Call of Fire
    {1527, Cause::Objective, 2075, 3599},       // Call of Fire
    {1530, Cause::Value, 0, 0},                 // Call of Water
    {1534, Cause::Objective, 0, 0},             // Call of Water
    {1535, Cause::Objective, 0, 0},             // Call of Water
    {1536, Cause::Objective, 0, 0},             // Call of Water
    {1638, Cause::Value, 0, 0},                 // A Warrior's Training
    {1639, Cause::Value, 0, 0},                 // Bartleby the Drunk
    {1640, Cause::Value, 0, 0},                 // Beat Bartleby
    {1642, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1643, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1644, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1646, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1647, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1648, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1649, Cause::Value, 0, 0},                 // The Tome of Valor
    {1650, Cause::Value, 0, 0},                 // The Tome of Valor
    {1651, Cause::Value, 0, 0},                 // The Tome of Valor
    {1652, Cause::Value, 5503, 5502},           // The Tome of Valor
    {1661, Cause::Placeholder, 13820, 13819},   // The Tome of Nobility
    {1665, Cause::Value, 8121, 71},             // Bartleby's Mug
    {1678, Cause::Value, 8121, 71},             // Vejrek
    {1679, Cause::Value, 0, 0},                 // Muren Stormpike
    {1683, Cause::Value, 8121, 71},             // Vorlus Vilehoof
    {1684, Cause::Value, 0, 0},                 // Elanaria
    {1688, Cause::Objective, 0, 0},             // Surena Caledon
    {1689, Cause::Objective, 11520, 697},       // The Binding
    {1716, Cause::Value, 0, 0},                 // Devourer of Souls
    {1718, Cause::Value, 0, 0},                 // The Islander
    {1719, Cause::Scripted, 8616, 2458},        // The Affray
    {1738, Cause::Objective, 0, 0},             // Heartswood
    {1739, Cause::Objective, 11519, 712},       // The Binding
    {1758, Cause::Value, 0, 0},                 // Tome of the Cabal
    {1778, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1779, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1780, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1781, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1783, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1784, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1785, Cause::Value, 7329, 7328},           // The Tome of Divinity
    {1786, Cause::Value, 0, 0},                 // The Tome of Divinity
    {1787, Cause::Objective, 0, 0},             // The Tome of Divinity
    {1788, Cause::Value, 7329, 7328},           // The Tome of Divinity
    {1795, Cause::Objective, 1373, 691},        // The Binding
    {1798, Cause::Value, 0, 0},                 // Seeking Strahad
    {1801, Cause::Value, 0, 0},                 // Tome of the Cabal
    {1802, Cause::Objective, 0, 0},             // Tome of the Cabal
    {1803, Cause::Objective, 0, 0},             // Tome of the Cabal
    {1804, Cause::Objective, 0, 0},             // Tome of the Cabal
    {1805, Cause::Objective, 0, 0},             // Tome of the Cabal
    {1818, Cause::Value, 0, 0},                 // Speak with Dillinger
    {1819, Cause::Objective, 8121, 71},         // Ulag the Cleaver
    {2997, Cause::Value, 0, 0},                 // Tome of Divinity
    {2998, Cause::Value, 0, 0},                 // Tome of Divinity
    {2999, Cause::Value, 0, 0},                 // Tome of Divinity
    {3000, Cause::Value, 0, 0},                 // Tome of Divinity
    {3681, Cause::Value, 0, 0},                 // Tome of Divinity
    {5921, Cause::Value, 0, 0},                 // Moonglade
    {5922, Cause::Value, 0, 0},                 // Moonglade
    {5929, Cause::Value, 0, 0},                 // Great Bear Spirit
    {5930, Cause::Value, 0, 0},                 // Great Bear Spirit
    {5931, Cause::Value, 0, 0},                 // Back to Darnassus
    {5932, Cause::Value, 0, 0},                 // Back to Thunder Bluff
    {6001, Cause::Objective, 19179, 5487},      // Body and Heart
    {6002, Cause::Objective, 19179, 5487},      // Body and Heart
    {6061, Cause::Objective, 0, 0},             // Taming the Beast
    {6062, Cause::Objective, 0, 0},             // Taming the Beast
    {6063, Cause::Objective, 0, 0},             // Taming the Beast
    {6064, Cause::Objective, 0, 0},             // Taming the Beast
    {6081, Cause::Value, 5300, 6991},           // Training the Beast
    {6082, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6083, Cause::Objective, 0, 0},             // Taming the Beast
    {6084, Cause::Objective, 0, 0},             // Taming the Beast
    {6085, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6086, Cause::Value, 5300, 6991},           // Training the Beast
    {6087, Cause::Objective, 0, 0},             // Taming the Beast
    {6088, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6089, Cause::Value, 5300, 6991},           // Training the Beast
    {6101, Cause::Objective, 0, 0},             // Taming the Beast
    {6102, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6103, Cause::Value, 5300, 6991},           // Training the Beast
    {6121, Cause::Value, 0, 0},                 // Lessons Anew
    {6122, Cause::Objective, 0, 0},             // The Principal Source
    {6123, Cause::Objective, 0, 0},             // Gathering the Cure
    {6124, Cause::Objective, 0, 0},             // Curing the Sick
    {6125, Cause::Value, 8947, 8946},           // Power over Poison
    {6126, Cause::Value, 0, 0},                 // Lessons Anew
    {6127, Cause::Objective, 0, 0},             // The Principal Source
    {6128, Cause::Objective, 0, 0},             // Gathering the Cure
    {6129, Cause::Objective, 0, 0},             // Curing the Sick
    {6130, Cause::Value, 8947, 8946},           // Power over Poison
    {7581, Cause::Objective, 0, 0},             // The Prison's Bindings
    {7582, Cause::Objective, 0, 0},             // The Prison's Casing
    {7583, Cause::Instance, 20700, 18540},      // Suppression
    {7601, Cause::Value, 0, 0},                 // What Niby Commands
    {7602, Cause::Objective, 0, 0},             // Flawless Fel Essence
    {7603, Cause::Objective, 1413, 1122},       // Kroshius' Infernal Core
    {9362, Cause::Objective, 0, 0},             // Warlord Krellian
    {9364, Cause::Scripted, 28285, 28272},      // Fragmented Magic
    {9461, Cause::Value, 0, 0},                 // Call of Fire
    {9464, Cause::Value, 0, 0},                 // Call of Fire
    {9465, Cause::Objective, 0, 0},             // Call of Fire
    {9467, Cause::Objective, 0, 0},             // Call of Fire
    {9468, Cause::Objective, 0, 0},             // Call of Fire
    {9484, Cause::Objective, 0, 0},             // Taming the Beast
    {9485, Cause::Objective, 1579, 1515},       // Taming the Beast
    {9486, Cause::Objective, 0, 0},             // Taming the Beast
    {9501, Cause::Objective, 0, 0},             // Call of Water
    {9503, Cause::Objective, 0, 0},             // Call of Water
    {9504, Cause::Objective, 0, 0},             // Call of Water
    {9508, Cause::Objective, 0, 0},             // Call of Water
    {9509, Cause::Objective, 5396, 5394},       // Call of Water
    {9529, Cause::Value, 0, 0},                 // The Stone
    {9555, Cause::Value, 2075, 3599},           // Call of Fire
    {9582, Cause::Objective, 8121, 71},         // Strength of One
    {9591, Cause::Objective, 0, 0},             // Taming the Beast
    {9592, Cause::Objective, 0, 0},             // Taming the Beast
    {9593, Cause::Objective, 1579, 1515},       // Taming the Beast
    {9598, Cause::Objective, 0, 0},             // Redemption
    {9600, Cause::Objective, 7329, 7328},       // Redemption
    {9619, Cause::Objective, 11520, 697},       // The Rune of Summoning
    {9673, Cause::Value, 5300, 6991},           // Beast Training
    {9675, Cause::Value, 5300, 6991},           // Beast Training
    {9678, Cause::Objective, 0, 0},             // The First Trial
    {9681, Cause::Value, 0, 0},                 // A Study in Power
    {9684, Cause::Objective, 0, 0},             // Claiming the Light
    {9685, Cause::Objective, 7329, 7328},       // Redeeming the Dead
    {9686, Cause::Value, 0, 0},                 // The Second Trial
    {9690, Cause::Value, 0, 0},                 // The Second Trial
    {9691, Cause::Value, 5503, 5502},           // Return to Silvermoon
    {9712, Cause::Placeholder, 34768, 34769},   // The Thalassian Warhorse
    {10350, Cause::Value, 8121, 71},            // Behomat
    {12228, Cause::Placeholder, 28285, 28272},  // Reacquiring the Magic [PH]
    {12636, Cause::Value, 0, 0},                // The Eye Of Acherus
    {12641, Cause::Objective, 0, 0},            // Death Comes From On High
    {12657, Cause::Value, 0, 0},                // The Might Of The Scourge
    {12670, Cause::Value, 0, 0},                // The Scarlet Harvest
    {12680, Cause::Objective, 0, 0},            // Grand Theft Palomino
    {12687, Cause::Scripted, 52382, 48778},     // Into the Realm of Shadows
    {12751, Cause::Value, 0, 0},                // A Sort Of Homecoming (after
                                                // 12739,12742,12743,12744,12745,12746,12747,12748,12749,12750)
    {12754, Cause::Objective, 0, 0},            // Ambush At The Overlook
    {12755, Cause::Objective, 0, 0},            // A Meeting With Fate
    {12756, Cause::Objective, 0, 0},            // The Scarlet Onslaught Emerges
    {12757, Cause::Objective, 0, 0},            // Scarlet Armies Approach...
    {12778, Cause::Value, 0, 0},                // The Scarlet Apocalypse
    {12779, Cause::Objective, 0, 0},            // An End To All Things...
    {12800, Cause::Value, 0, 0},                // The Lich King's Command
    {12801, Cause::Scripted, 53821, 50977},     // The Light of Dawn
    {12842, Cause::Value, 0, 0},                // Runeforging: Preparation For Battle (after 12619)
    {12848, Cause::Objective, 0, 0},            // The Endless Hunger
    {12850, Cause::Value, 0, 0},                // Report To Scourge Commander Thalanor
};

inline bool IsClassQuest(Entry const& entry) { return entry.rewardSpell != 0; }

// A table row joined with its quest's record in the quest index (ClassQuestJoin.h builds it once per index on a
// worker, after the refusals are marked): the limits and chain come from the index, the rest from the table. Plain
// data, so these rules stay std only
struct Row
{
    Entry const* own{nullptr};
    bool inIndex{false};  // the quest has a record; without one the limits below are empty (the command lists it)
    std::uint32_t classMask{0};  // 1 << (class - 1); 0: any
    std::uint32_t raceMask{0};   // 1 << (race - 1); 0: any
    std::uint32_t minLevel{0};
    std::vector<std::uint32_t> prev;    // sorted; one of them rewarded unlocks it; empty: a chain's first step
    std::vector<std::uint32_t> active;  // sorted; quests that must be active (the core's negative prevs): no rule
    std::array<bool, 2> refused{};      // per side (0 Alliance, 1 Horde): the executor refuses the quest
    bool giverIndexed{false};

    std::uint32_t quest() const { return own->quest; }
};
using Rows = std::vector<Row>;

inline Row const* Find(Rows const& rows, std::uint32_t quest)
{
    auto const it = std::lower_bound(rows.begin(), rows.end(), quest,
                                     [](Row const& row, std::uint32_t id) { return row.quest() < id; });
    return it != rows.end() && it->quest() == quest ? &*it : nullptr;
}

// trainerTeaches(spell): the bot's class trainer teaches the spell (UpkeepIndex TrainerList taughtBy, WU2-fix)
template <typename TrainerTeaches>
State StateOf(Row const& row, TrainerTeaches const& trainerTeaches)
{
    Entry const& entry = *row.own;
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

inline bool Fits(Row const& row, BotFacts const& bot)
{
    bool const cls = !row.classMask || (bot.cls && ((row.classMask >> (bot.cls - 1)) & 1u));
    bool const race = !row.raceMask || (bot.race && ((row.raceMask >> (bot.race - 1)) & 1u));
    return cls && race;
}

inline bool Unlocked(Row const& row, BotFacts const& bot)
{
    if (row.prev.empty())
        return true;
    for (std::uint32_t const prev : row.prev)
        if (Has(bot.rewarded, prev))
            return true;
    return false;
}

// The chain's next step towards a class quest: the earliest not rewarded step the bot can reach, walking back from
// the class quest through the steps that fit its class and race. nullptr when the class quest is rewarded
inline Row const* NextStep(Rows const& rows, Row const& classQuest, BotFacts const& bot)
{
    Row const* step = &classQuest;
    for (int depth = 0; depth < MAX_CHAIN; ++depth)
    {
        if (Has(bot.rewarded, step->quest()))
            return nullptr;
        if (Unlocked(*step, bot))
            return step;
        Row const* back = nullptr;
        for (std::uint32_t prev : step->prev)
        {
            Row const* row = Find(rows, prev);
            if (row && !back && Fits(*row, bot))
                back = row;
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
    Row const* step{nullptr};  // the quest that gets the bonus (the class quest or its chain's next step)
};

// hasRoute(quest): the planner knows a way to the quest's giver (an indexed giver on a map it can travel to)
template <typename TrainerTeaches, typename HasRoute>
Judgement Judge(Rows const& rows, Row const& classQuest, BotFacts const& bot, TrainerTeaches const& trainerTeaches,
                HasRoute const& hasRoute)
{
    Entry const& own = *classQuest.own;
    if (!bot.governed)
        return {Verdict::NotGoverned, nullptr};
    if (!IsClassQuest(own) || !Fits(classQuest, bot))
        return {Verdict::NotForBot, nullptr};
    if (StateOf(classQuest, trainerTeaches) != State::Doable)
        return {Verdict::NotDoable, nullptr};
    if (Has(bot.knownSpells, own.learns) || Has(bot.knownSpells, own.rewardSpell))
        return {Verdict::SpellKnown, nullptr};
    Row const* step = NextStep(rows, classQuest, bot);
    if (!step)
        return {Verdict::Rewarded, nullptr};
    if (Has(bot.givenUp, own.quest) || Has(bot.givenUp, step->quest()))
        return {Verdict::GivenUp, step};
    if (StateOf(*step, trainerTeaches) != State::Doable || !Unlocked(*step, bot))
        return {Verdict::StepWaits, step};
    if (bot.level < step->minLevel)
        return {Verdict::LowLevel, step};
    if (!hasRoute(step->quest()))
        return {Verdict::NoRoute, step};
    return {Verdict::Bonus, step};
}

inline float BonusXp(BotFacts const& bot) { return UNLOCK_BONUS_LEVEL_SHARE * static_cast<float>(bot.levelXp); }

// Every quest that gets the bonus for this bot (quest id, bonus XP), sorted by quest; empty for an ungoverned bot and
// at the top level (no XP to the next level: no bonus, so no second planning pass in PlanGoals either)
template <typename TrainerTeaches, typename HasRoute>
std::vector<std::pair<std::uint32_t, float>> Bonuses(Rows const& rows, BotFacts const& bot,
                                                     TrainerTeaches const& trainerTeaches, HasRoute const& hasRoute)
{
    std::vector<std::pair<std::uint32_t, float>> out;
    if (!bot.governed || BonusXp(bot) <= 0.0f)
        return out;
    for (Row const& row : rows)
    {
        if (!IsClassQuest(*row.own))
            continue;
        Judgement const judgement = Judge(rows, row, bot, trainerTeaches, hasRoute);
        if (judgement.verdict == Verdict::Bonus)
            out.emplace_back(judgement.step->quest(), BonusXp(bot));
    }
    std::sort(out.begin(), out.end());
    auto const same = [](std::pair<std::uint32_t, float> const& a, std::pair<std::uint32_t, float> const& b)
    { return a.first == b.first; };
    out.erase(std::unique(out.begin(), out.end(), same), out.end());
    return out;
}

// The class quests and their doable steps in the bot's log: protected from the circuit's abandon once taken
inline std::vector<std::uint32_t> Protected(Rows const& rows, std::vector<std::uint32_t> const& inLog,
                                            BotFacts const& bot)
{
    std::vector<std::uint32_t> out;
    if (!bot.governed)
        return out;
    for (std::uint32_t quest : inLog)
    {
        Row const* row = Find(rows, quest);
        if (row && Fits(*row, bot) && row->own->cause == Cause::Value && !Has(bot.knownSpells, row->own->learns))
            out.push_back(quest);
    }
    return out;
}
}  // namespace Planner::ClassQuests

#endif
