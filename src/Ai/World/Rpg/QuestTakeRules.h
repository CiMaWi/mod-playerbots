/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTTAKERULES_H
#define _PLAYERBOT_QUESTTAKERULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "QuestIndex.h"

// Whether a bot may take a quest now (questing WQ17a, decisions WQ.21 and WQ.43): one rule set for the circuit, the
// plan's needs and the accept at a giver, so they cannot disagree. Pure: the quest's record and plain facts of the bot;
// no game object, no config, no state. tests/quest-take-selftest.cpp checks it
namespace Planner
{
namespace Take
{
constexpr std::uint32_t GREY_DIFF = 4;        // a quest this many levels below the bot is grey: the core's
                                              // Quests.LowLevelHideDiff default; the server passes its own value
constexpr std::int32_t MAX_LEVELS_ABOVE = 3;  // QUEST_MAX_LEVELS_ABOVE_BOT (QuestLevelRules.h), as today
constexpr std::uint16_t INSTANCE_FLAGS = QuestFlag::DUNGEON | QuestFlag::RAID | QuestFlag::PVP;
constexpr std::uint16_t GROUP_FLAGS = QuestFlag::ELITE | QuestFlag::GROUP;
constexpr std::uint16_t REPEAT_FLAGS =
    QuestFlag::DAILY | QuestFlag::WEEKLY | QuestFlag::REPEATABLE | QuestFlag::SEASONAL;

// Why the bot may not take the quest, in the order May checks the rules: the first rule that fails is the answer
enum class Reason : std::uint8_t
{
    Done,  // rewarded
    InLog,
    GivenUp,  // on the caller's given-up list (QuestFailBook GivenUp)
    Repeatable,
    Level,        // below the quest's minimum level
    Limits,       // race, class or maximum level
    Unsupported,  // the executor refuses a work of it for the bot's side (QuestRecord::refused)
    TooHigh,      // more than MAX_LEVELS_ABOVE above the bot
    Grey,
    Kind,        // dungeon, raid or PvP
    EliteGroup,  // elite or group quest while the caller allows none
    Prerequisite,
    Exclusive,   // a quest of its exclusive group is in the log or rewarded
    Breadcrumb,  // its target is missing, done, in the log or takeable
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Reason::Count)> WORDS = {
    "done",     "in_log", "given_up", "repeatable",  "level",        "limits",    "unsupported",
    "too_high", "grey",   "kind",     "elite_group", "prerequisite", "exclusive", "breadcrumb"};

inline char const* Word(Reason reason) { return WORDS[static_cast<std::size_t>(reason)]; }

// The bot's facts. Every list is sorted ascending; an empty one holds nothing
struct Facts
{
    std::uint8_t race{0};
    std::uint8_t cls{0};
    std::uint8_t level{0};
    std::uint32_t greyDiff{GREY_DIFF};
    bool eliteGroup{false};  // elite and group quests allowed (the circuit: the bot is in a group)
    std::span<std::uint32_t const> rewarded;
    std::span<std::uint32_t const> inLog;
    std::span<std::uint32_t const> givenUp;
    std::span<std::int32_t const> takenGroups;  // the exclusive groups of the log's and the rewarded quests
};

inline bool Holds(std::span<std::uint32_t const> sorted, std::uint32_t id)
{
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

inline std::int32_t QuestLevel(QuestRecord const& quest, std::uint8_t botLevel)
{
    return quest.level > 0 ? quest.level : static_cast<std::int32_t>(botLevel);
}

inline bool IsGrey(QuestRecord const& quest, std::uint8_t level, std::uint32_t greyDiff)
{
    return static_cast<std::int32_t>(level) > QuestLevel(quest, level) + static_cast<std::int32_t>(greyDiff);
}

// The core's rule: one previous quest rewarded (positive) or active (negative). PrevQuestId alone misses the quests
// naming this one as their NextQuestId (1795 after 1804/1805, upkeep WU20a)
inline bool PrerequisiteMet(QuestRecord const& quest, Facts const& bot)
{
    auto const met = [&bot](std::int32_t prev)
    {
        return prev > 0 ? Holds(bot.rewarded, static_cast<std::uint32_t>(prev))
                        : Holds(bot.inLog, static_cast<std::uint32_t>(-prev));
    };
    if (quest.prevAny.empty())
        return !quest.prev || met(quest.prev);
    return std::any_of(quest.prevAny.begin(), quest.prevAny.end(), met);
}

// Every rule but the breadcrumb's, which asks them of its target
inline std::optional<Reason> MayBeforeBreadcrumb(QuestRecord const& quest, Facts const& bot)
{
    auto const allows = [](std::uint32_t mask, std::uint8_t id)
    { return !mask || (id >= 1 && id <= 32 && (mask & (1u << (id - 1)))); };
    if (Holds(bot.rewarded, quest.id))
        return Reason::Done;
    if (Holds(bot.inLog, quest.id))
        return Reason::InLog;
    if (Holds(bot.givenUp, quest.id))
        return Reason::GivenUp;
    if (quest.flags & REPEAT_FLAGS)
        return Reason::Repeatable;
    if (bot.level < quest.minLevel)
        return Reason::Level;
    if (!allows(quest.raceMask, bot.race) || !allows(quest.classMask, bot.cls) ||
        (quest.maxLevel && bot.level > quest.maxLevel))
        return Reason::Limits;
    if (RefusedFor(quest, SideOfRace(bot.race)))
        return Reason::Unsupported;
    if (QuestLevel(quest, bot.level) > static_cast<std::int32_t>(bot.level) + MAX_LEVELS_ABOVE)
        return Reason::TooHigh;
    if (IsGrey(quest, bot.level, bot.greyDiff))
        return Reason::Grey;
    if (quest.flags & INSTANCE_FLAGS)
        return Reason::Kind;
    if ((quest.flags & GROUP_FLAGS) && !bot.eliteGroup)
        return Reason::EliteGroup;
    if (!PrerequisiteMet(quest, bot))
        return Reason::Prerequisite;
    if (quest.exclusiveGroup > 0 &&
        std::binary_search(bot.takenGroups.begin(), bot.takenGroups.end(), quest.exclusiveGroup))
        return Reason::Exclusive;
    return std::nullopt;
}

// Whether the bot may take the quest now: nothing, or the first rule it fails
inline std::optional<Reason> May(QuestRecord const& quest, Facts const& bot, QuestIndex const& quests)
{
    if (std::optional<Reason> const why = MayBeforeBreadcrumb(quest, bot))
        return why;
    if (quest.breadcrumbFor)
    {
        QuestRecord const* target = quests.Find(quest.breadcrumbFor);
        if (!target || !MayBeforeBreadcrumb(*target, bot).has_value() || Holds(bot.rewarded, target->id) ||
            Holds(bot.inLog, target->id))
            return Reason::Breadcrumb;
    }
    return std::nullopt;
}
}  // namespace Take
}  // namespace Planner

#endif
