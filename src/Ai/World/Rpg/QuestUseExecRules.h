/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTUSEEXECRULES_H
#define _PLAYERBOT_QUESTUSEEXECRULES_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "ItemUseTypes.h"
#include "QuestUseRules.h"

// The quest executor's use of a quest item at an objective's rule target (questing WT35b, PLAN 3.2): which rule the
// bot works by, what it does at a target it sees, and what a refusal of the item-use framework means for the order.
// No game header: tests/quest-use-exec-selftest.cpp checks it
namespace Planner
{
namespace QuestExec
{
constexpr char const* NO_ITEM = "no_item";    // no rule's item is in the bags: the order ends (no item is handed out)
constexpr char const* NO_SPELL = "no_spell";  // a class-spell rule the bot knows no rank of
constexpr std::uint8_t USE_RETRIES = 3;       // refusals that waiting may change, per target, before it is passed over
constexpr float USE_REACH_MIN_YARDS = 3.0f;   // the walk in: the rule's range less a margin, within these bounds
constexpr float USE_REACH_MAX_YARDS = 15.0f;
constexpr float USE_REACH_MARGIN_YARDS = 2.0f;

// The rule the bot works an objective by: of the objective's own rules, else of the rules that complete the quest
// (QuestIndex attaches those to the event or to the objectives no spawn places), the first of the objective's kind
// whose item the bot holds or whose spell it knows (a ranked class spell lists a rule per rank). None: the word
struct UsePick
{
    QuestUseRule const* rule{nullptr};
    char const* word{nullptr};
};

template <typename Iterator, typename Holds, typename Knows>
UsePick PickRule(Iterator first, Iterator last, std::uint32_t objective, UseKind kind, Holds holds, Knows knows)
{
    bool items = false;
    for (std::uint32_t const wanted : {objective, USE_COMPLETES_QUEST})
        for (Iterator it = first; it != last; ++it)
        {
            QuestUseRule const& rule = *it;
            if (rule.objective != wanted || rule.kind != kind || rule.spellId == 0)
                continue;
            items = items || rule.itemId != 0;
            if (rule.itemId ? holds(rule.itemId) : knows(rule.spellId))
                return {&rule, nullptr};
        }
    return {nullptr, items ? NO_ITEM : NO_SPELL};
}

// How far from the target the bot stands to use: inside the spell's range, and near enough to see the target
inline float UseReach(float rangeYards)
{
    return std::clamp(rangeYards - USE_REACH_MARGIN_YARDS, USE_REACH_MIN_YARDS, USE_REACH_MAX_YARDS);
}

// What a seek or an engagement sees of a unit target: in the world, dead, whether the rule wants it dead, passed over
// in this run, within reach and in sight, and whether a timed use of the bot is running
struct UseSeen
{
    bool present{false};
    bool dead{false};
    bool wantDead{false};
    bool passed{false};
    bool inReach{false};
    bool pending{false};
};

enum class UseAct : std::uint8_t
{
    Skip,  // gone, passed over, or not in the life state the rule wants: the next target
    Wait,  // a timed use runs: no second request
    Walk,  // walk into reach
    Use    // ask the framework
};

inline UseAct UseVerdict(UseSeen const& seen)
{
    if (seen.pending)
        return UseAct::Wait;
    if (!seen.present || seen.passed || seen.dead != seen.wantDead)
        return UseAct::Skip;
    return seen.inReach ? UseAct::Use : UseAct::Walk;
}

// What a refusal of the framework means here: wait and ask again (bounded, then the target is passed over), pass the
// target over at once, or end the order with the cause's word (a use waiting cannot change: PLAN 3.3)
enum class Refusal : std::uint8_t
{
    Retry,
    PassOver,
    GiveUp
};

// One row per cause, in ItemUse::Cause order (CAUSE_WORDS)
constexpr std::array<Refusal, ItemUse::CAUSE_COUNT> REFUSALS = {{
    Refusal::Retry,     // none: no request (the test's nouse)
    Refusal::GiveUp,    // no_effect
    Refusal::GiveUp,    // no_item
    Refusal::GiveUp,    // not_equipped
    Refusal::GiveUp,    // level
    Refusal::GiveUp,    // skill
    Refusal::GiveUp,    // class
    Refusal::GiveUp,    // cant_use
    Refusal::Retry,     // not_ready: a cooldown
    Refusal::Retry,     // potion
    Refusal::GiveUp,    // no_charges
    Refusal::Retry,     // busy: a cast, the global cooldown
    Refusal::Retry,     // dead: the bot
    Refusal::Retry,     // no_control
    Refusal::Retry,     // in_fight: the fight actions fight first
    Refusal::GiveUp,    // not_in_fight
    Refusal::GiveUp,    // arena
    Refusal::GiveUp,    // place
    Refusal::GiveUp,    // form: a real shapeshift is left before the use
    Refusal::PassOver,  // no_target
    Refusal::PassOver,  // target: not this one (its life state, its side)
    Refusal::PassOver,  // full
    Refusal::Retry,     // range
    Refusal::Retry,     // too_close
    Refusal::Retry,     // sight
    Refusal::Retry,     // focus
    Refusal::Retry,     // engine
    Refusal::Retry,     // handler
}};

inline Refusal RefusalOf(ItemUse::Cause cause)
{
    std::size_t const index = static_cast<std::size_t>(cause);
    return index < REFUSALS.size() ? REFUSALS[index] : Refusal::GiveUp;
}

// After a refusal with `tries` refusals at this target before it: a retriable cause is passed over at the last try
inline Refusal AfterRefusal(ItemUse::Cause cause, std::uint8_t tries)
{
    Refusal const refusal = RefusalOf(cause);
    return refusal == Refusal::Retry && tries + 1 >= USE_RETRIES ? Refusal::PassOver : refusal;
}

// The order's end word of a cause that ends it: no_item for the item's absence (PLAN 4 WT35b), else
// refused:<cause> (the ItemUse row's own word)
constexpr std::array<char const*, ItemUse::CAUSE_COUNT> REFUSED_WORDS = {
    "refused:none",         "refused:no_effect", "no_item",          "refused:not_equipped", "refused:level",
    "refused:skill",        "refused:class",     "refused:cant_use", "refused:not_ready",    "refused:potion",
    "refused:no_charges",   "refused:busy",      "refused:dead",     "refused:no_control",   "refused:in_fight",
    "refused:not_in_fight", "refused:arena",     "refused:place",    "refused:form",         "refused:no_target",
    "refused:target",       "refused:full",      "refused:range",    "refused:too_close",    "refused:sight",
    "refused:focus",        "refused:engine",    "refused:handler"};

inline char const* GiveUpWord(ItemUse::Cause cause)
{
    std::size_t const index = static_cast<std::size_t>(cause);
    return index < REFUSED_WORDS.size() ? REFUSED_WORDS[index] : "refused:none";
}

// The quest_use row's result word (world-log.md): one row per request, written at the request. used, pending (a timed
// use: its end is the framework's ItemUse row, no second quest_use row) or refused:<cause> (the ItemUse cause word)
inline std::string UseRowWord(ItemUse::Outcome outcome, ItemUse::Cause cause)
{
    if (outcome == ItemUse::Outcome::Used)
        return "used";
    if (outcome == ItemUse::Outcome::Pending)
        return "pending";
    return "refused:" + std::string(ItemUse::CauseWord(cause));
}

// How the request aims, by the item record's shape (INTERFACE 6.1: the record decides, not the rule's kind): at the
// unit, at the bot itself (an item used near its target), or at the spot where the target stands
enum class UseAim : std::uint8_t
{
    Unit,
    Self,
    Spot
};

inline UseAim AimOf(ItemUse::Shape shape)
{
    switch (shape)
    {
        case ItemUse::Shape::None:
        case ItemUse::Shape::Self:
            return UseAim::Self;
        case ItemUse::Shape::Ground:
            return UseAim::Spot;
        default:
            return UseAim::Unit;
    }
}
}  // namespace QuestExec
}  // namespace Planner

#endif
