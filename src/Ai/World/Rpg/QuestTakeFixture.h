/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTTAKEFIXTURE_H
#define _PLAYERBOT_QUESTTAKEFIXTURE_H

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "PlannerSnapshot.h"
#include "QuestIndex.h"

// A stand-in for the game's answer about takes (Snapshot::takeable, questing WQ41b) in fixtures: the harnesses and the
// circuit test commands, whose bots are no players the core could ask. The plan never reads it: the planner reads the
// core's CanTakeQuest (PlannerService.cpp FillTakeable). It models what those fixtures need of the core's rules
// (Player::CanTakeQuest): the quest's status, the minimum and maximum level, the race and class masks, the previous
// quests, the exclusive groups and the breadcrumbs; no skill, standing or holiday
namespace Planner
{
namespace TakeFixture
{
inline bool Has(std::span<std::uint32_t const> sorted, std::uint32_t id)
{
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

// The exclusive groups of the log's and the rewarded quests, ascending
inline std::vector<std::int32_t> TakenGroups(QuestIndex const& index, std::span<std::uint32_t const> rewarded,
                                             std::span<std::uint32_t const> inLog)
{
    std::vector<std::int32_t> groups;
    for (std::span<std::uint32_t const> const ids : {rewarded, inLog})
        for (std::uint32_t const id : ids)
            if (QuestRecord const* quest = index.Find(id); quest && quest->exclusiveGroup > 0)
                groups.push_back(quest->exclusiveGroup);
    std::sort(groups.begin(), groups.end());
    return groups;
}

// Whether the modelled game lets a bot of this race, class and level with these sorted lists take the quest
inline bool Allows(QuestRecord const& quest, std::uint8_t race, std::uint8_t cls, std::uint8_t level,
                   std::span<std::uint32_t const> rewarded, std::span<std::uint32_t const> inLog,
                   std::span<std::int32_t const> groups)
{
    auto const allows = [](std::uint32_t mask, std::uint8_t id)
    { return !mask || (id >= 1 && id <= 32 && (mask & (1u << (id - 1)))); };
    if (Has(rewarded, quest.id) || Has(inLog, quest.id) || level < quest.minLevel ||
        (quest.maxLevel && level > quest.maxLevel) || !allows(quest.raceMask, race) || !allows(quest.classMask, cls))
        return false;
    auto const met = [&](std::int32_t prev)
    {
        return prev > 0 ? Has(rewarded, static_cast<std::uint32_t>(prev))
                        : Has(inLog, static_cast<std::uint32_t>(-prev));
    };
    bool const previous = quest.prevAny.empty() ? (!quest.prev || met(quest.prev))
                                                : std::any_of(quest.prevAny.begin(), quest.prevAny.end(), met);
    if (!previous)
        return false;
    if (quest.exclusiveGroup > 0 && std::binary_search(groups.begin(), groups.end(), quest.exclusiveGroup))
        return false;
    return !quest.breadcrumbFor || (!Has(rewarded, quest.breadcrumbFor) && !Has(inLog, quest.breadcrumbFor));
}

// The modelled answer for a bot: every quest of the index it allows, ascending (the index is in id order)
inline std::vector<std::uint32_t> Answer(QuestIndex const& index, std::uint8_t race, std::uint8_t cls,
                                         std::uint8_t level, std::span<std::uint32_t const> rewarded,
                                         std::span<std::uint32_t const> inLog)
{
    std::vector<std::int32_t> const groups = TakenGroups(index, rewarded, inLog);
    std::vector<std::uint32_t> ids;
    for (QuestRecord const& quest : index.quests)
        if (Allows(quest, race, cls, level, rewarded, inLog, groups))
            ids.push_back(quest.id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// The snapshot with its modelled answer (Snapshot::takeable) from its race, class, level, log and rewarded quests
inline void Answer(Snapshot& bot, QuestIndex const& index)
{
    std::vector<std::uint32_t> inLog;
    for (QuestSlot const& slot : bot.quests)
        if (slot.quest)
            inLog.push_back(slot.quest);
    std::sort(inLog.begin(), inLog.end());
    std::vector<std::uint32_t> rewarded = bot.rewardedQuests;
    std::sort(rewarded.begin(), rewarded.end());
    bot.takeable = Answer(index, bot.race, bot.cls, bot.level, rewarded, inLog);
}

// The bot with the answer for the context's quest index (a circuit context; none: as it was)
template <typename Context>
Snapshot Answered(Snapshot bot, Context const& context)
{
    if (context.quests)
        Answer(bot, *context.quests);
    return bot;
}
}  // namespace TakeFixture
}  // namespace Planner

#endif
