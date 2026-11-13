/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GOALMODELLOG_H
#define _PLAYERBOT_GOALMODELLOG_H

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "GoalModel.h"

// WT14-2: the ranked goals as world-log decision rows (kind "goal", family rule 14). A pure mapping from a Ranked to
// rows (no Player, no WorldLog), so the offline selftest checks it and WT16 emits the same rows where it applies a
// plan. One row per ranking: the best option, its next ones as alt1..alt3 (their value per minute):
//   choice  <kind>:<target>, e.g. grind:12, quest:30, money:spot_7, move_zone:camp_21, quest:quest_40, rest:0
//           (kind words of KIND_WORDS; the target type of TARGET_WORDS where the kind does not say what the id is)
//   reason  the option's reason word (gold_short, circuit_dry, delivery, class_quest ...), [a-z0-9_:]
//   value   the option's value per minute (XP-equivalent units), 0 for an unusable number
// A failed ranking gives none:0 with the failure as the reason. Trait numbers never appear (C4).
namespace Planner::GoalLog
{
constexpr std::size_t ALTERNATIVES = 3;

struct Alt
{
    std::string choice;
    float value{0.0f};
};

struct Row
{
    std::string choice;
    std::string reason;
    float value{0.0f};
    std::vector<Alt> alternatives;
};

inline float Safe(float value) { return std::isfinite(value) ? value : 0.0f; }

inline std::vector<Row> Rows(Goals::Ranked const& ranked)
{
    std::vector<Row> rows;
    Row row;
    if (!ranked.ok || ranked.options.empty())
    {
        row.choice = "none:0";
        row.reason = Goals::LogWord(ranked.ok ? "no_options" : (ranked.failure.empty() ? "failed" : ranked.failure));
        rows.push_back(std::move(row));
        return rows;
    }
    Goals::Option const& best = ranked.options.front();
    row.choice = Goals::ChoiceWord(best);
    row.reason = Goals::LogWord(best.reason.empty() ? "none" : best.reason);
    row.value = Safe(best.valuePerMinute);
    for (std::size_t i = 1; i < ranked.options.size() && row.alternatives.size() < ALTERNATIVES; ++i)
        row.alternatives.push_back({Goals::ChoiceWord(ranked.options[i]), Safe(ranked.options[i].valuePerMinute)});
    rows.push_back(std::move(row));
    return rows;
}
}  // namespace Planner::GoalLog

#endif
