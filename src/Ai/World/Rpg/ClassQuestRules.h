/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CLASSQUESTRULES_H
#define _PLAYERBOT_CLASSQUESTRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

// Upkeep 3.11 (WU20a, WU39): the class quests a bot learns a spell from, one state per class quest and bot, the unlock
// bonus a governed bot (GoalInterface.h Governed) gets on the next step it can do, and who is given which spell when
// (DueQuests; the completion is WU21a's). Std only, so tests/class-quest-selftest.cpp checks it; the planner job
// (PlannerQuests.cpp GoalsCompute) applies it on the worker from the snapshot's facts.
// The table comes from the hub's config/wu-class-quests.json (census 2026-11-11) and holds the class quests only; the
// chains are the quest index's (ClassQuestJoin.h): a step's prerequisites are its PrevQuestID and every quest whose
// NextQuestID names it (the core's Quest::prevQuests; any one rewarded unlocks it). A step has no state of its own. The
// trainer quests (their spell is on the class trainer: 31, 5061, 4490, 7463, 7631, 7647, 9737, 11001, 12172, 12173)
// are not here: they are ordinary optional quests.
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
    Doable,     // the bot can do it: the bonus drives it; its spell is granted only after the bot failed it (WU.19)
    Automatic,  // no bot can do it, or the executor refuses it or a step of the bot's chain: granted when it unlocks
    Ordinary    // not a class quest for this bot: the trainer teaches its spell
};

// [E] The unlock bonus in XP: this share of the XP the bot needs for its next level, added to the quest's value. The
// goal model floors a class option at the best other rate plus (reward + bonus) / minutes (AddClassQuests), so the
// quest wins the next decision at any distance (J2); the bonus's size decides whether a circuit that takes it beats a
// lone trip to it. A tuning value read from the nights: time from unlock to done (NOTES watchlist)
constexpr float UNLOCK_BONUS_LEVEL_SHARE = 0.25f;
constexpr char const* REASON_WORD =
    "class_quest";                          // the goal and circuit rows' reason (UpkeepRules REASON_CLASS_QUEST)
constexpr std::uint32_t GIVE_UP_LIMIT = 2;  // a quest given up this often (any cause) is failed
constexpr int MAX_CHAIN = 16;               // chain steps walked back from a class quest (the join and the walk)
// The races of each side (QuestIndex.h SIDE_RACE_MASKS; ClassQuestJoin.h asserts they agree): the executor's refusal
// is per side, and a race of neither side has nothing refused
constexpr std::array<std::uint32_t, 2> SIDE_RACES = {0x44D, 0x2B2};

// What the table itself knows. The limits (class, race, level) and the chain (the predecessors) are the quest index's
// (QuestRecord), joined into a Row; the table has no column the record holds
struct Entry
{
    std::uint32_t quest;
    Cause cause;
    std::uint32_t rewardSpell;  // the spell the quest casts at its reward (non-zero: a class quest)
    std::uint32_t learns;       // the spell the bot learns from it
};

// Sorted by quest id, one row per class quest (the steps of their chains are the index's, WU39)
inline constexpr Entry TABLE[] = {
    {96, Cause::Placeholder, 5396, 5394},       // Call of Water
    {1471, Cause::Objective, 11520, 697},       // The Binding
    {1474, Cause::Objective, 11519, 712},       // The Binding
    {1498, Cause::Value, 8121, 71},             // Path of Defense
    {1504, Cause::Objective, 11520, 697},       // The Binding
    {1513, Cause::Objective, 11519, 712},       // The Binding
    {1527, Cause::Objective, 2075, 3599},       // Call of Fire
    {1652, Cause::Value, 5503, 5502},           // The Tome of Valor
    {1661, Cause::Placeholder, 13820, 13819},   // The Tome of Nobility
    {1665, Cause::Value, 8121, 71},             // Bartleby's Mug
    {1678, Cause::Value, 8121, 71},             // Vejrek
    {1683, Cause::Value, 8121, 71},             // Vorlus Vilehoof
    {1689, Cause::Objective, 11520, 697},       // The Binding
    {1719, Cause::Scripted, 8616, 2458},        // The Affray
    {1739, Cause::Objective, 11519, 712},       // The Binding
    {1785, Cause::Value, 7329, 7328},           // The Tome of Divinity
    {1788, Cause::Value, 7329, 7328},           // The Tome of Divinity
    {1795, Cause::Objective, 1373, 691},        // The Binding
    {1819, Cause::Objective, 8121, 71},         // Ulag the Cleaver
    {6001, Cause::Objective, 19179, 5487},      // Body and Heart
    {6002, Cause::Objective, 19179, 5487},      // Body and Heart
    {6081, Cause::Value, 5300, 6991},           // Training the Beast
    {6082, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6085, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6086, Cause::Value, 5300, 6991},           // Training the Beast
    {6088, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6089, Cause::Value, 5300, 6991},           // Training the Beast
    {6102, Cause::Objective, 1579, 1515},       // Taming the Beast
    {6103, Cause::Value, 5300, 6991},           // Training the Beast
    {6125, Cause::Value, 8947, 8946},           // Power over Poison
    {6130, Cause::Value, 8947, 8946},           // Power over Poison
    {7583, Cause::Instance, 20700, 18540},      // Suppression
    {7603, Cause::Objective, 1413, 1122},       // Kroshius' Infernal Core
    {9364, Cause::Scripted, 28285, 28272},      // Fragmented Magic
    {9485, Cause::Objective, 1579, 1515},       // Taming the Beast
    {9509, Cause::Objective, 5396, 5394},       // Call of Water
    {9555, Cause::Value, 2075, 3599},           // Call of Fire
    {9582, Cause::Objective, 8121, 71},         // Strength of One
    {9593, Cause::Objective, 1579, 1515},       // Taming the Beast
    {9600, Cause::Objective, 7329, 7328},       // Redemption
    {9619, Cause::Objective, 11520, 697},       // The Rune of Summoning
    {9673, Cause::Value, 5300, 6991},           // Beast Training
    {9675, Cause::Value, 5300, 6991},           // Beast Training
    {9685, Cause::Objective, 7329, 7328},       // Redeeming the Dead
    {9691, Cause::Value, 5503, 5502},           // Return to Silvermoon
    {9712, Cause::Placeholder, 34768, 34769},   // The Thalassian Warhorse
    {10350, Cause::Value, 8121, 71},            // Behomat
    {12228, Cause::Placeholder, 28285, 28272},  // Reacquiring the Magic [PH]
    {12687, Cause::Scripted, 52382, 48778},     // Into the Realm of Shadows
    {12801, Cause::Scripted, 53821, 50977},     // The Light of Dawn
};

inline bool IsClassQuest(Entry const& entry) { return entry.rewardSpell != 0; }

inline Entry const* FindEntry(std::uint32_t quest)
{
    Entry const* const end = std::end(TABLE);
    Entry const* const it = std::lower_bound(std::begin(TABLE), end, quest,
                                             [](Entry const& entry, std::uint32_t id) { return entry.quest < id; });
    return it != end && it->quest == quest ? it : nullptr;
}

// A class quest or a step of its chain, joined with the quest's record in the quest index (ClassQuestJoin.h builds the
// rows once per index on a worker, after the refusals are marked): the limits and chain come from the index, the
// cause and spells from the table. Plain data, so these rules stay std only
struct Row
{
    std::uint32_t id{0};
    Entry const* own{nullptr};   // the table's entry; nullptr for a step
    bool inIndex{false};         // the quest has a record; a row without one fits no bot (the command lists it)
    std::uint32_t classMask{0};  // 1 << (class - 1); 0: any
    std::uint32_t raceMask{0};   // 1 << (race - 1); 0: any
    std::uint32_t minLevel{0};
    std::vector<std::uint32_t> prev;  // sorted; one of them rewarded unlocks it; empty: a chain's first step
    std::vector<std::uint32_t>
        active;                     // sorted; quests that must be active (the core's negative prevs): no bot opens it
    std::array<bool, 2> refused{};  // per side (0 Alliance, 1 Horde): the executor refuses the quest
    bool giverIndexed{false};

    std::uint32_t quest() const { return id; }
};
using Rows = std::vector<Row>;

inline bool IsClassQuest(Row const& row) { return row.own && IsClassQuest(*row.own); }

inline Row const* Find(Rows const& rows, std::uint32_t quest)
{
    auto const it = std::lower_bound(rows.begin(), rows.end(), quest,
                                     [](Row const& row, std::uint32_t id) { return row.quest() < id; });
    return it != rows.end() && it->quest() == quest ? &*it : nullptr;
}

// A class quest's state from its own columns. trainerTeaches(spell): the bot's class trainer teaches the spell
// (UpkeepIndex TrainerList taughtBy, WU2-fix). StateFor adds the bot's chain
template <typename TrainerTeaches>
State StateOf(Row const& row, TrainerTeaches const& trainerTeaches)
{
    Entry const& entry = *row.own;
    if (IsClassQuest(entry) && entry.learns && trainerTeaches(entry.learns))
        return State::Ordinary;
    return entry.cause == Cause::Value || entry.cause == Cause::Objective ? State::Doable : State::Automatic;
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
    std::vector<std::uint32_t> givenUp;      // quests given up GIVE_UP_LIMIT times or more (QuestFailRecord), any cause
    std::vector<std::uint32_t> knownSpells;  // the class-quest spells the bot knows (learns and reward spells)
};

// 0 Alliance, 1 Horde, 2 neither (nothing refused)
inline std::size_t SideOf(std::uint32_t race)
{
    if (race < 1 || race > 32)
        return 2;
    std::uint32_t const bit = 1u << (race - 1);
    return (SIDE_RACES[0] & bit) ? 0 : ((SIDE_RACES[1] & bit) ? 1 : 2);
}

inline bool RefusedFor(Row const& row, std::size_t side) { return side < 2 && row.refused[side]; }

inline bool Fits(Row const& row, BotFacts const& bot)
{
    bool const cls = !row.classMask || (bot.cls && ((row.classMask >> (bot.cls - 1)) & 1u));
    bool const race = !row.raceMask || (bot.race && ((row.raceMask >> (bot.race - 1)) & 1u));
    return row.inIndex && cls && race;
}

inline bool KnowsSpell(Row const& row, BotFacts const& bot)
{
    return (row.own->learns && Has(bot.knownSpells, row.own->learns)) ||
           (row.own->rewardSpell && Has(bot.knownSpells, row.own->rewardSpell));
}

inline bool Unlocked(Row const& row, BotFacts const& bot)
{
    if (row.prev.empty())
        return row.active.empty();
    for (std::uint32_t const prev : row.prev)
        if (Has(bot.rewarded, prev))
            return true;
    return false;
}

// A class quest's chain for a bot: walked back from the class quest through the predecessors that fit the bot's class
// and race (of alternatives the first one the executor does for its side, else the first that fits) to the earliest
// step not rewarded that is unlocked
struct Walk
{
    Row const* next{nullptr};           // that step; nullptr when the class quest is rewarded
    std::vector<Row const*> remaining;  // the steps not rewarded, from the class quest back to next
    bool broken{false};        // a step without a record, needing another active, or whose predecessors fit no bot
    std::uint32_t outside{0};  // the predecessor next waits on, past the join's walk: ordinary questing does it
    std::uint32_t level{0};    // the highest minLevel on remaining: the class quest's level for this bot
};

inline Walk WalkChain(Rows const& rows, Row const& classQuest, BotFacts const& bot)
{
    Walk walk;
    if (Has(bot.rewarded, classQuest.quest()))
        return walk;
    std::size_t const side = SideOf(bot.race);
    Row const* step = &classQuest;
    for (int depth = 0; depth <= MAX_CHAIN; ++depth)
    {
        walk.remaining.push_back(step);
        walk.level = std::max(walk.level, step->minLevel);
        walk.next = step;
        if (!step->inIndex || (step->prev.empty() && !step->active.empty()))
        {
            walk.broken = true;
            return walk;
        }
        if (Unlocked(*step, bot))
            return walk;
        Row const* back = nullptr;
        bool joined = false;  // a predecessor the join walked
        for (std::uint32_t const prev : step->prev)
        {
            Row const* row = Find(rows, prev);
            joined = joined || row;
            if (!row || !Fits(*row, bot))
                continue;
            if (!back || (RefusedFor(*back, side) && !RefusedFor(*row, side)))
                back = row;
        }
        if (!back)
        {
            if (joined)
                walk.broken = true;
            else
                walk.outside = step->prev.front();
            return walk;
        }
        if (std::find(walk.remaining.begin(), walk.remaining.end(), back) != walk.remaining.end())
        {
            walk.broken = true;  // a cycle in the quest data: no step of it opens
            return walk;
        }
        step = back;
    }
    walk.outside = walk.next->prev.front();
    return walk;
}

// The state of a class quest for a bot (WU39): doable when its own cause is and the bot's chain can be walked without
// a step the executor refuses for the bot's side; else automatic. The matrix's failures (WU22c) join here
template <typename TrainerTeaches>
State StateFor(Row const& classQuest, Walk const& walk, BotFacts const& bot, TrainerTeaches const& trainerTeaches)
{
    State const own = StateOf(classQuest, trainerTeaches);
    if (own != State::Doable)
        return own;
    if (walk.broken)
        return State::Automatic;
    std::size_t const side = SideOf(bot.race);
    for (Row const* step : walk.remaining)
        if (RefusedFor(*step, side))
            return State::Automatic;
    return State::Doable;
}

// The planner's answer about a way to a quest's giver: Unknown when its search was cut short or the travel graph is not
// ready (no verdict: nothing fails for it)
enum class Route : std::uint8_t
{
    Yes,
    No,
    Unknown
};

// Why a class quest gives a bot the bonus or not; the harness prints the word
enum class Verdict : std::uint8_t
{
    Bonus,
    NotGoverned,
    NotForBot,   // another class or race
    NotDoable,   // automatic (or the trainer teaches its spell): granted when it unlocks
    SpellKnown,  // the bot knows its spell (a backstory, WU.8, or a grant)
    Rewarded,    // done
    Failed,      // the class quest or a step of the bot's chain given up GIVE_UP_LIMIT times, or definitely no route
    StepWaits,   // the next step waits on a predecessor past the join's walk (ordinary questing does it)
    LowLevel,    // the next step's level
    Unknown,     // the way to the next step's giver is not known yet
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Verdict::Count)> VERDICT_WORDS = {
    "bonus",    "not_governed", "not_for_bot", "not_doable", "spell_known",
    "rewarded", "failed",       "step_waits",  "low_level",  "unknown"};
inline char const* VerdictWord(Verdict verdict) { return VERDICT_WORDS[static_cast<std::size_t>(verdict)]; }

struct Judgement
{
    Verdict verdict{Verdict::NotForBot};
    Row const* step{nullptr};  // the quest that gets the bonus (the class quest or its chain's next step), or failed
    std::uint32_t waitsOn{0};  // StepWaits: the predecessor past the walk
};

// A walked class quest of a governed bot that fits it and lacks its spell. routeOf(quest) answers a Route
template <typename TrainerTeaches, typename RouteOf>
Judgement JudgeWalk(Row const& classQuest, Walk const& walk, BotFacts const& bot, TrainerTeaches const& trainerTeaches,
                    RouteOf const& routeOf)
{
    if (!walk.next)
        return {Verdict::Rewarded, nullptr, 0};
    if (StateFor(classQuest, walk, bot, trainerTeaches) != State::Doable)
        return {Verdict::NotDoable, walk.next, 0};
    for (Row const* step : walk.remaining)
        if (Has(bot.givenUp, step->quest()))
            return {Verdict::Failed, step, 0};
    if (walk.outside)
        return {Verdict::StepWaits, walk.next, walk.outside};
    if (bot.level < walk.next->minLevel)
        return {Verdict::LowLevel, walk.next, 0};
    switch (routeOf(walk.next->quest()))
    {
        case Route::No:
            return {Verdict::Failed, walk.next, 0};
        case Route::Unknown:
            return {Verdict::Unknown, walk.next, 0};
        default:
            return {Verdict::Bonus, walk.next, 0};
    }
}

template <typename TrainerTeaches, typename RouteOf>
Judgement Judge(Rows const& rows, Row const& classQuest, BotFacts const& bot, TrainerTeaches const& trainerTeaches,
                RouteOf const& routeOf)
{
    if (!bot.governed)
        return {Verdict::NotGoverned, nullptr, 0};
    if (!IsClassQuest(classQuest) || !Fits(classQuest, bot))
        return {Verdict::NotForBot, nullptr, 0};
    if (KnowsSpell(classQuest, bot))
        return {Verdict::SpellKnown, nullptr, 0};
    return JudgeWalk(classQuest, WalkChain(rows, classQuest, bot), bot, trainerTeaches, routeOf);
}

// The unit is the spell: one spell the bot lacks and the class quests at or below its level that teach it. A spell is
// due when one of them is failed, or (no doable one among them) automatically; else the doable ones' steps get the
// bonus. A spell the trainer teaches, that the bot knows, or whose quest it was rewarded is no call
struct SpellCall
{
    std::uint32_t spell{0};
    Row const* first{nullptr};          // the first of them (creation, an ungoverned bot's level-up)
    Row const* automatic{nullptr};      // the first automatic one (a governed bot)
    Row const* failed{nullptr};         // the first failed doable one (a governed bot)
    bool doable{false};                 // one is doable and not failed (a governed bot)
    std::vector<Row const*> bonus;      // the steps that get the bonus (any level), empty when the spell is due
    std::vector<Judgement> judgements;  // per class quest teaching the spell that fits the bot (a governed bot)

    bool Due() const { return failed || (!doable && automatic); }
};

template <typename TrainerTeaches, typename RouteOf>
std::vector<SpellCall> Calls(Rows const& rows, BotFacts const& bot, TrainerTeaches const& trainerTeaches,
                             RouteOf const& routeOf)
{
    std::vector<std::pair<std::uint32_t, Row const*>> bySpell;
    for (Row const& row : rows)
        if (IsClassQuest(row) && row.own->learns && Fits(row, bot) && StateOf(row, trainerTeaches) != State::Ordinary)
            bySpell.emplace_back(row.own->learns, &row);
    std::sort(bySpell.begin(), bySpell.end(), [](auto const& a, auto const& b)
              { return a.first != b.first ? a.first < b.first : a.second->quest() < b.second->quest(); });
    std::vector<SpellCall> calls;
    for (std::size_t at = 0; at < bySpell.size();)
    {
        std::size_t end = at;
        bool settled = false;
        for (; end < bySpell.size() && bySpell[end].first == bySpell[at].first; ++end)
            settled =
                settled || KnowsSpell(*bySpell[end].second, bot) || Has(bot.rewarded, bySpell[end].second->quest());
        if (settled)
        {
            at = end;
            continue;
        }
        SpellCall call;
        call.spell = bySpell[at].first;
        for (; at < end; ++at)
        {
            Row const& row = *bySpell[at].second;
            Walk const walk = WalkChain(rows, row, bot);
            bool const inLevel = walk.level <= bot.level;
            if (inLevel && !call.first)
                call.first = &row;
            if (!bot.governed)
                continue;
            Judgement const judgement = JudgeWalk(row, walk, bot, trainerTeaches, routeOf);
            call.judgements.push_back(judgement);
            if (judgement.verdict == Verdict::Bonus)
                call.bonus.push_back(judgement.step);
            if (!inLevel)
                continue;
            if (judgement.verdict == Verdict::Failed)
                call.failed = call.failed ? call.failed : &row;
            else if (judgement.verdict == Verdict::NotDoable)
                call.automatic = call.automatic ? call.automatic : &row;
            else
                call.doable = true;
        }
        if (call.Due())
            call.bonus.clear();
        if (call.first)
            calls.push_back(std::move(call));
        else if (!call.bonus.empty())  // only quests above the bot's level: their steps still get the bonus
            calls.push_back(std::move(call));
    }
    return calls;
}

// When the completion (WU21a) asks
enum class When : std::uint8_t
{
    Creation,  // a new bot: every class quest at or below its level
    LevelUp,   // a bot not governed: every one at or below its level; a governed bot: the automatic and failed ones
    Check      // any other time: a governed bot's failed doable ones
};

struct Due
{
    std::uint32_t spell{0};  // the spell the bot lacks (the quest's learns)
    std::uint32_t quest{0};  // the class quest the completion grants
    bool failed{false};      // the bot failed the doable quest (the grant's source class_quest_failed)
};

// Who is given what, and when (WU39): per spell the class quest the completion grants; a quest that is rewarded or
// whose spell the bot knows is never due, and a governed bot levelled past a doable quest is given nothing for it
template <typename TrainerTeaches, typename RouteOf>
std::vector<Due> DueQuests(Rows const& rows, BotFacts const& bot, When when, TrainerTeaches const& trainerTeaches,
                           RouteOf const& routeOf)
{
    std::vector<Due> out;
    for (SpellCall const& call : Calls(rows, bot, trainerTeaches, routeOf))
    {
        if (!call.first)
            continue;
        if (when == When::Creation || (when == When::LevelUp && !bot.governed))
            out.push_back({call.spell, call.first->quest(), false});
        else if (bot.governed && call.failed)
            out.push_back({call.spell, call.failed->quest(), true});
        else if (bot.governed && when == When::LevelUp && call.Due())
            out.push_back({call.spell, call.automatic->quest(), false});
    }
    return out;
}

inline float BonusXp(BotFacts const& bot) { return UNLOCK_BONUS_LEVEL_SHARE * static_cast<float>(bot.levelXp); }

// Every quest that gets the bonus for this bot (quest id, bonus XP), sorted by quest; empty for an ungoverned bot and
// at the top level (no XP to the next level: no bonus, so no second planning pass in PlanGoals either)
template <typename TrainerTeaches, typename RouteOf>
std::vector<std::pair<std::uint32_t, float>> Bonuses(Rows const& rows, BotFacts const& bot,
                                                     TrainerTeaches const& trainerTeaches, RouteOf const& routeOf)
{
    std::vector<std::pair<std::uint32_t, float>> out;
    if (!bot.governed || BonusXp(bot) <= 0.0f)
        return out;
    for (SpellCall const& call : Calls(rows, bot, trainerTeaches, routeOf))
        for (Row const* step : call.bonus)
            out.emplace_back(step->quest(), BonusXp(bot));
    std::sort(out.begin(), out.end());
    auto const same = [](std::pair<std::uint32_t, float> const& a, std::pair<std::uint32_t, float> const& b)
    { return a.first == b.first; };
    out.erase(std::unique(out.begin(), out.end(), same), out.end());
    return out;
}

// The doable class quests and the steps of the bot's chains in its log: protected from the circuit's abandon once
// taken
template <typename TrainerTeaches>
std::vector<std::uint32_t> Protected(Rows const& rows, std::vector<std::uint32_t> const& inLog, BotFacts const& bot,
                                     TrainerTeaches const& trainerTeaches)
{
    std::vector<std::uint32_t> chains;
    if (!bot.governed || inLog.empty())
        return chains;
    for (Row const& row : rows)
    {
        if (!IsClassQuest(row) || !Fits(row, bot) || KnowsSpell(row, bot))
            continue;
        Walk const walk = WalkChain(rows, row, bot);
        if (walk.next && StateFor(row, walk, bot, trainerTeaches) == State::Doable)
            for (Row const* step : walk.remaining)
                chains.push_back(step->quest());
    }
    std::sort(chains.begin(), chains.end());
    std::vector<std::uint32_t> out;
    for (std::uint32_t const quest : inLog)
        if (Has(chains, quest))
            out.push_back(quest);
    return out;
}
}  // namespace Planner::ClassQuests

#endif
