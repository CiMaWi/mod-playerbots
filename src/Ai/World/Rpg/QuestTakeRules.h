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
// plan's needs and the accept at a giver, so they cannot disagree. WQ41 (decision WQ.47): the game decides what it
// allows (level, race, class, previous quests, exclusive groups, breadcrumbs, skills, standings, holidays): its answer
// is a fact, the core's CanTakeQuest asked on the bot's map thread (Snapshot::takeable, PlannerService.cpp
// FillTakeable; a live accept asks it for its one quest). The rules here are the plan's own choices. Pure: the quest's
// record and plain facts of the bot; no game object, no config, no state. tests/quest-take-selftest.cpp checks it
namespace Planner
{
namespace Take
{
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
    GivenUp,     // on the caller's given-up list (QuestFailBook GivenUp)
    Impossible,  // WQ38: on the list of quests known to be impossible (QuestRecord::impossible)
    Repeatable,
    Unsupported,  // the executor refuses a work of it for the bot's side (QuestRecord::refused)
    TooHigh,      // more than MAX_LEVELS_ABOVE above the bot
    Grey,
    Kind,        // dungeon, raid or PvP
    EliteGroup,  // elite or group quest while the caller allows none
    Game,        // WQ41: not in the game's answer (Facts::takeable)
    Breadcrumb,  // the game lets the bot take its target now: the plan takes the target itself
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Reason::Count)> WORDS = {
    "done",     "in_log", "given_up", "impossible",  "repeatable", "unsupported",
    "too_high", "grey",   "kind",     "elite_group", "game",       "breadcrumb"};

inline char const* Word(Reason reason) { return WORDS[static_cast<std::size_t>(reason)]; }

// The bot's facts. Every list is sorted ascending; an empty one holds nothing
struct Facts
{
    std::uint8_t race{0};  // its side, for the executor's refusals
    std::uint8_t level{0};
    bool eliteGroup{false};  // elite and group quests allowed (the circuit: the bot is in a group)
    std::span<std::uint32_t const> rewarded;
    std::span<std::uint32_t const> inLog;
    std::span<std::uint32_t const> givenUp;
    // The game's answer (Snapshot::takeable, WQ41): the quests the core's CanTakeQuest lets the bot take now. Empty
    // in a snapshot built without it: the bot may then take nothing
    std::span<std::uint32_t const> takeable;
};

inline bool Holds(std::span<std::uint32_t const> sorted, std::uint32_t id)
{
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

inline std::int32_t QuestLevel(QuestRecord const& quest, std::uint8_t botLevel)
{
    return quest.level > 0 ? quest.level : static_cast<std::int32_t>(botLevel);
}

// The game's grey level of a bot of this level: a copy of Acore::XP::GetGrayLevel (Formulas.h)
constexpr std::int32_t GrayLevel(std::uint8_t level)
{
    std::int32_t const own = level;
    if (own <= 5)
        return 0;
    if (own <= 39)
        return own - 5 - own / 10;
    if (own <= 59)
        return own - 1 - own / 5;
    return own - 9;
}

// Grey by the game's colour line, the one line of the log's accept and drop (WQ34, WQ.46 item 3): at or below the
// bot's grey level. A quest without a level is of the bot's own and never grey
inline bool IsGrey(QuestRecord const& quest, std::uint8_t level)
{
    return quest.level > 0 && quest.level <= GrayLevel(level);
}

// The plan's own rules, without the game's answer: the circuit asks these alone of a quest its own turn-ins unlock,
// which the answer, taken before them, cannot hold (QuestCircuit.h Takeable)
inline std::optional<Reason> Own(QuestRecord const& quest, Facts const& bot)
{
    if (Holds(bot.rewarded, quest.id))
        return Reason::Done;
    if (Holds(bot.inLog, quest.id))
        return Reason::InLog;
    if (Holds(bot.givenUp, quest.id))
        return Reason::GivenUp;
    if (Listed(quest))
        return Reason::Impossible;
    if (quest.flags & REPEAT_FLAGS)
        return Reason::Repeatable;
    if (RefusedFor(quest, SideOfRace(bot.race)))
        return Reason::Unsupported;
    if (QuestLevel(quest, bot.level) > static_cast<std::int32_t>(bot.level) + MAX_LEVELS_ABOVE)
        return Reason::TooHigh;
    if (IsGrey(quest, bot.level))
        return Reason::Grey;
    if (quest.flags & INSTANCE_FLAGS)
        return Reason::Kind;
    if ((quest.flags & GROUP_FLAGS) && !bot.eliteGroup)
        return Reason::EliteGroup;
    return std::nullopt;
}

// Whether the bot may take the quest now: nothing, or the first rule it fails. A breadcrumb whose target the game
// lets the bot take is left: taken, it would keep the game from offering the target until it is handed in
inline std::optional<Reason> May(QuestRecord const& quest, Facts const& bot)
{
    if (std::optional<Reason> const why = Own(quest, bot))
        return why;
    if (!Holds(bot.takeable, quest.id))
        return Reason::Game;
    if (quest.breadcrumbFor && Holds(bot.takeable, quest.breadcrumbFor))
        return Reason::Breadcrumb;
    return std::nullopt;
}
}  // namespace Take
}  // namespace Planner

#endif
