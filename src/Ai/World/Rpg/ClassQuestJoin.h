/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CLASSQUESTJOIN_H
#define _PLAYERBOT_CLASSQUESTJOIN_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ClassQuestRules.h"
#include "QuestIndex.h"

// Upkeep 3.11 (WU38): the class-quest table on the quest index. The table keeps its own columns; the limits, the
// predecessors and the executor's refusal per side are joined once per index on its worker, after MarkRefusals, into
// QuestIndex::classRows, which the planner job reads (WU39: the chains' steps are the index's too). No DB read, nothing
// per tick.
namespace Planner::ClassQuests
{
// The class quests' rows and their chains' steps, sorted by quest (WU39: the steps are the index's, walked back from
// each class quest through its predecessors to MAX_CHAIN deep). The predecessors are the record's prevAny (the core's
// prevQuests: its own PrevQuestId and every NextQuestId naming it), the record's prev where prevAny is empty
// (synthetic records); a positive one must be rewarded first, a negative one active. A predecessor without a record
// gets a row without one (it fits no bot: the chain is broken for the rules, the command lists it)
inline Row RowOf(QuestIndex const& index, std::uint32_t quest)
{
    Row row;
    row.id = quest;
    row.own = FindEntry(quest);
    if (QuestRecord const* record = index.Find(quest))
    {
        row.inIndex = true;
        row.classMask = record->classMask;
        row.raceMask = record->raceMask;
        row.minLevel = record->minLevel;
        std::vector<std::int32_t> prevs = record->prevAny;
        if (prevs.empty() && record->prev)
            prevs.push_back(record->prev);
        for (std::int32_t const prev : prevs)
            (prev > 0 ? row.prev : row.active).push_back(static_cast<std::uint32_t>(prev > 0 ? prev : -prev));
        std::sort(row.prev.begin(), row.prev.end());
        row.prev.erase(std::unique(row.prev.begin(), row.prev.end()), row.prev.end());
        std::sort(row.active.begin(), row.active.end());
        row.active.erase(std::unique(row.active.begin(), row.active.end()), row.active.end());
        for (std::size_t side = 0; side < SIDES; ++side)
            row.refused[side] = record->refused[side] != 0;
        row.giverIndexed = !record->givers.empty();
    }
    return row;
}

inline Rows Join(QuestIndex const& index)
{
    static_assert(SIDES == 2 && SIDE_RACES[0] == SIDE_RACE_MASKS[0] && SIDE_RACES[1] == SIDE_RACE_MASKS[1],
                  "ClassQuestRules.h SIDE_RACES are the quest index's sides");
    Rows rows;
    std::set<std::uint32_t> seen;
    std::deque<std::pair<std::uint32_t, int>> open;  // quest, depth below its class quest
    for (Entry const& entry : TABLE)
    {
        seen.insert(entry.quest);
        rows.push_back(RowOf(index, entry.quest));
        for (std::uint32_t const prev : rows.back().prev)
            open.emplace_back(prev, 1);
    }
    while (!open.empty())
    {
        auto const [quest, depth] = open.front();
        open.pop_front();
        if (!seen.insert(quest).second)
            continue;
        rows.push_back(RowOf(index, quest));
        if (depth < MAX_CHAIN)
            for (std::uint32_t const prev : rows.back().prev)
                open.emplace_back(prev, depth + 1);
    }
    std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.quest() < b.quest(); });
    return rows;
}

// Join and keep the rows with the index; before the index is published (its worker), after MarkRefusals
inline void JoinInto(QuestIndex& index) { index.classRows = Join(index); }

// A build for a harness or test: the index joined at once
inline QuestIndex Joined(QuestIndex index)
{
    JoinInto(index);
    return index;
}

struct JoinCounts
{
    std::uint32_t classQuests{0};
    std::uint32_t steps{0};
    std::uint32_t refused{0};  // rows (class quests and steps) the executor refuses on some side
    std::uint32_t missing{0};  // rows whose quest is not in the index
};

inline JoinCounts CountsOf(Rows const& rows)
{
    JoinCounts counts;
    for (Row const& row : rows)
    {
        (IsClassQuest(row) ? counts.classQuests : counts.steps)++;
        counts.refused += (row.refused[0] || row.refused[1]) ? 1 : 0;
        counts.missing += row.inIndex ? 0 : 1;
    }
    return counts;
}

// The chain of a class quest for a bot of this race (and the class its mask names), from the first step to the quest:
// the walk WalkChain takes with nothing rewarded. A predecessor past the join's walk ends it
inline std::vector<Row const*> ChainOf(Rows const& rows, Row const& classQuest, std::uint32_t race)
{
    BotFacts bot;
    bot.race = race;
    for (std::uint32_t cls = 1; cls <= 32 && classQuest.classMask; ++cls)
        if ((classQuest.classMask >> (cls - 1)) & 1u)
        {
            bot.cls = cls;
            break;
        }
    std::vector<Row const*> chain{&classQuest};
    for (int depth = 0; depth < MAX_CHAIN; ++depth)
    {
        Row const* back = nullptr;
        for (std::uint32_t const prev : chain.back()->prev)
        {
            Row const* row = Find(rows, prev);
            if (row && !back && Fits(*row, bot))
                back = row;
        }
        if (!back)
            break;
        chain.push_back(back);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}
}  // namespace Planner::ClassQuests

#endif
