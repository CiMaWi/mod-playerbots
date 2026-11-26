/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTFAILROWS_H
#define _PLAYERBOT_QUESTFAILROWS_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

// Questing WQ38 (§3.11 part 7): the two rows that say a quest failed by the user's definition. A quest worked on
// orders for 10 minutes without a credit writes QuestStall (the stall sum, kept per quest of the log with the bot's
// goals, so it outlives an order); a dropped quest's QuestAbandon row carries the share of its credits that were done.
// A quest that is not finished yet has not failed: the sum is a reading and ends nothing. Pure, for the selftest
// (tests/quest-field-exec-selftest.cpp)
namespace Planner
{
namespace QuestFailRows
{
constexpr std::uint32_t MINUTE_MS = 60 * 1000;
constexpr std::uint32_t LIMIT_MS = 10 * MINUTE_MS;       // ordered work without a credit before the first row
constexpr std::uint32_t SHORT_LIMIT_MS = 2 * MINUTE_MS;  // under the test constraint shortgiveup
// A pause between two ticks of the quest counts this much at most (as Pursuit::CLOCK_GAP_MS): a fight, a rest, a trip
// or time off the orders adds at most this, never its length
constexpr std::uint32_t GAP_MS = 5000;
constexpr std::int64_t SHARE_STEP = 1000;  // the QuestAbandon amount: (share + 1) x 1000 + the quest's level
constexpr std::uint32_t MAX_SHARE = 100;
constexpr std::uint32_t MAX_LEVEL_IN_AMOUNT = 999;

inline std::uint32_t LimitMs(bool shortened) { return shortened ? SHORT_LIMIT_MS : LIMIT_MS; }

// One quest's sum: its progress when last ticked (the sum of its counts: a rise is a credit), the ordered time since
// its last credit and when its next row is due
struct Sum
{
    std::uint32_t quest{0};
    std::uint32_t progress{0};
    std::uint32_t workedMs{0};
    std::uint32_t nextRowMs{0};  // 0 = the limit
    std::uint32_t lastTick{0};   // the game's ms stamp of the last tick; 0 = none yet
};

// The bot's sums, one per quest it worked on an order (GoalApply::BotGoals::stalls; a reset of the AI starts anew)
struct Sums
{
    std::vector<Sum> quests;
};

struct Row
{
    std::uint32_t quest{0};
    std::uint32_t minutes{0};
};

// The quests no longer in the log are forgotten: a quest taken again starts at zero
template <typename InLog>
void KeepLogged(Sums& sums, InLog inLog)
{
    std::erase_if(sums.quests, [&inLog](Sum const& sum) { return !inLog(sum.quest); });
}

// One tick of an order that expects an open objective of the quest (once per quest and tick): the time since the
// quest's last tick counts, at most GAP_MS; a rise of its progress since then is a credit and zeroes the sum. The row
// when the sum reaches the limit, then twice the limit, four times ... after the last credit
inline std::optional<Row> Tick(Sums& sums, std::uint32_t quest, std::uint32_t progress, std::uint32_t now,
                               std::uint32_t limitMs)
{
    auto it = std::find_if(sums.quests.begin(), sums.quests.end(), [quest](Sum const& s) { return s.quest == quest; });
    if (it == sums.quests.end())
        it = sums.quests.insert(sums.quests.end(), Sum{quest});
    Sum& sum = *it;
    std::uint32_t const since = sum.lastTick ? now - sum.lastTick : 0;  // game stamps wrap: a difference
    sum.lastTick = now ? now : 1;
    sum.workedMs += std::min(since, GAP_MS);
    bool const credit = progress > sum.progress;
    sum.progress = progress;  // a fall (an item sold) only lowers the mark
    if (credit)
    {
        sum.workedMs = 0;
        sum.nextRowMs = 0;
        return std::nullopt;
    }
    std::uint32_t const due = sum.nextRowMs ? sum.nextRowMs : limitMs;
    if (sum.workedMs < due)
        return std::nullopt;
    sum.nextRowMs =
        due > std::numeric_limits<std::uint32_t>::max() / 2 ? std::numeric_limits<std::uint32_t>::max() : due * 2;
    return Row{quest, sum.workedMs / MINUTE_MS};
}

// One credit of a quest as the abandon counts it: what the log holds and what the quest requires (a kill or object,
// an item, the exploration as 0 or 1 of 1)
struct Credit
{
    std::uint32_t have{0};
    std::uint32_t required{0};
};

// The share of the quest's credits that were done, 0 to 100 (rounded down): the credits done, each capped at its
// requirement, over the credits required; a complete quest that has credits 100, a quest without any 0
inline std::uint32_t DoneShare(std::span<Credit const> credits, bool complete)
{
    std::uint64_t done = 0;
    std::uint64_t required = 0;
    for (Credit const& credit : credits)
    {
        done += std::min(credit.have, credit.required);
        required += credit.required;
    }
    if (!required)
        return 0;
    if (complete)
        return MAX_SHARE;
    return static_cast<std::uint32_t>(done * MAX_SHARE / required);
}

// The QuestAbandon row's amount: the quest's level, and with a known share (share + 1) x 1000 above it, so a row of an
// abandon whose share is unknown (the client's packet, rows before WQ38) stays under 1000 and reads as the level only
inline std::int64_t AbandonAmount(std::uint32_t level, std::optional<std::uint32_t> share)
{
    std::int64_t const at = std::min(level, MAX_LEVEL_IN_AMOUNT);
    if (!share)
        return at;
    return (static_cast<std::int64_t>(std::min(*share, MAX_SHARE)) + 1) * SHARE_STEP + at;
}
}  // namespace QuestFailRows
}  // namespace Planner

#endif
