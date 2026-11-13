/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTCIRCUITLOG_H
#define _PLAYERBOT_QUESTCIRCUITLOG_H

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "QuestCircuit.h"

// WT13b r6c: the circuit's choices as world-log decision rows (kind "circuit", family rule 14). A pure mapping from a
// plan to rows (no Player, no WorldLog), so the offline selftest checks it and WT16 emits the same rows where it
// applies plans. choice and reason are words of [a-z0-9_:]; the numbers in a choice are quest ids and camp indexes:
// clang-format off
//   take:<quest>            reason value_per_minute, or delivery_elsewhere for a carried quest; value = per minute
//   defer:<quest>           reason log_full, too_far, risky, no_area, elite_group_solo or later_trip; value 0
//   defer:more              the deferrals beyond the cap, value = their count
//   abandon:<quest>         reason grey, overtime, deaths, log_space or unindexed; value 0
//   move:<chain|delivery|bracket>:camp<N>[:q<quest>]  reason chain_quest_takeable_there, delivery, zone_bracket
//   move:nowhere            reason target_not_actionable, log_full_no_delivery or no_target; value = 0
// The move row's value is the walk seconds.
// clang-format on
namespace Planner::CircuitLog
{
constexpr std::size_t MAX_ROWS = 40;  // one plan cannot flood the stream; surplus deferrals fold into defer:more

struct Row
{
    std::string choice;
    std::string reason;
    float value{0.0f};
};

// A skip reason as a word: lower case, anything else becomes an underscore, no doubled ones
inline std::string Word(char const* text)
{
    std::string word;
    for (char const* c = text; *c; ++c)
    {
        char const out = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ? *c : '_';
        if (out != '_' || (!word.empty() && word.back() != '_'))
            word.push_back(out);
    }
    while (!word.empty() && word.back() == '_')
        word.pop_back();
    return word;
}

inline char const* AbandonWord(AbandonReason reason)
{
    switch (reason)
    {
        case AbandonReason::Grey:
            return "grey";
        case AbandonReason::Overtime:
            return "overtime";
        case AbandonReason::Deaths:
            return "deaths";
        case AbandonReason::Unindexed:
            return "unindexed";
        default:
            return "log_space";
    }
}

inline char const* MoveWord(MoveReason reason)
{
    switch (reason)
    {
        case MoveReason::Chain:
            return "chain";
        case MoveReason::Delivery:
            return "delivery";
        case MoveReason::Bracket:
            return "bracket";
        default:
            return "nowhere";
    }
}

// A skip is a choice when the quest fits the bot but is left alone; the rest (done, in the log, limits, too high, grey,
// instance, prerequisites) is not a decision of the circuit
inline bool IsDeferral(SkipReason reason)
{
    return reason == SkipReason::LogFull || reason == SkipReason::TooFar || reason == SkipReason::Risky ||
           reason == SkipReason::NoArea || reason == SkipReason::Solo;
}

inline std::vector<Row> Rows(CircuitPlan const& plan)
{
    std::vector<Row> rows;
    if (!plan.ok)
        return rows;
    for (CircuitTake const& take : plan.take)
        rows.push_back({"take:" + std::to_string(take.quest), take.carried ? "delivery_elsewhere" : "value_per_minute",
                        take.valuePerMinute});

    std::vector<Row> defers;
    std::set<std::uint32_t> seen;
    for (CircuitSkip const& skip : plan.skipped)
        if (IsDeferral(skip.reason) && seen.insert(skip.quest).second)
            defers.push_back({"defer:" + std::to_string(skip.quest), Word(SkipName(skip.reason)), 0.0f});
    for (std::uint32_t const quest : plan.deferred)
        if (seen.insert(quest).second)
            defers.push_back({"defer:" + std::to_string(quest), "later_trip", 0.0f});

    std::vector<Row> tail;
    for (CircuitAbandon const& abandon : plan.abandon)
        tail.push_back({"abandon:" + std::to_string(abandon.quest), AbandonWord(abandon.reason), 0.0f});
    if (plan.moveOn.reason != MoveReason::None)
    {
        Row move;
        move.value = plan.moveOn.seconds;
        if (plan.moveOn.reason == MoveReason::Nowhere)
        {
            move.choice = "move:nowhere";
            move.reason = plan.moveOn.word.empty() ? "no_target" : plan.moveOn.word;
        }
        else
        {
            move.choice = std::string("move:") + MoveWord(plan.moveOn.reason) + ":camp" +
                          std::to_string(plan.moveOn.camp) +
                          (plan.moveOn.quest ? ":q" + std::to_string(plan.moveOn.quest) : std::string());
            move.reason = plan.moveOn.reason == MoveReason::Chain      ? "chain_quest_takeable_there"
                          : plan.moveOn.reason == MoveReason::Delivery ? "delivery"
                                                                       : "zone_bracket";
        }
        tail.push_back(std::move(move));
    }

    std::size_t const fixed = rows.size() + tail.size();
    std::size_t const room = MAX_ROWS > fixed ? MAX_ROWS - fixed : 0;
    if (defers.size() > room)
    {
        std::size_t const surplus = defers.size() - (room ? room - 1 : defers.size());
        defers.resize(room ? room - 1 : 0);
        if (room)
            defers.push_back({"defer:more", "over_cap", static_cast<float>(surplus)});
    }
    rows.insert(rows.end(), defers.begin(), defers.end());
    rows.insert(rows.end(), tail.begin(), tail.end());
    return rows;
}
}  // namespace Planner::CircuitLog

#endif
