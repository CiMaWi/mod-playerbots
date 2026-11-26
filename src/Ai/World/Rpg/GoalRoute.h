/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GOALROUTE_H
#define _PLAYERBOT_GOALROUTE_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "FieldMap.h"
#include "GoalModel.h"
#include "PlannerSnapshot.h"
#include "QuestCircuit.h"
#include "QuestFields.h"
#include "QuestIndex.h"
#include "QuestRoute.h"
#include "QuestTakeRules.h"

// The quest route for a plan of goals (questing plan WQ18a, WQ.43 point 4): what Goals::PlanGoals calls to build the
// route of a bot that is not held on the circuit. It stands after GoalModel.h and QuestRoute.h, which the goal model
// cannot include (QuestFields.h includes it). From the snapshot's log and the quests the take rules allow the bot on
// its map it asks Route::Build over the job's field map, far stops priced by the job's route search, and hands the
// plan on as the goal model's plain data with the risk of its first stop. Pure rules for a planner worker: no game
// header, no config, no state. tests/quest-route-selftest.cpp proves it on generated logs
namespace Planner
{
namespace GoalRoute
{
// A part of the build as its caller times it (questing WQ18b part 3: the service's PerfMonitor rows, where a goals
// job's route time goes; a harness hands none in and no clock is read)
using PartTimer = std::function<void(char const* part, std::uint64_t micros)>;
constexpr char const* PART_NEEDS = "planner route needs";  // the log's needs and the takes listed
constexpr char const* PART_STOPS = "planner route stops";  // the stops chosen, with their walk searches

// The job's route search as the route's walk function: both ends at the bot's height (an order names the middle of a
// cell, which has none; the search snaps its ends to the graph). None for no way and for a place past the job's
// searches; the latter are counted in `capped`
inline Route::Walk WalkOf(TravelCost const& travel, std::uint32_t mapId, float z, std::uint32_t& capped)
{
    if (!travel)
        return {};
    return [&travel, mapId, z, &capped](float fromX, float fromY, float toX, float toY) -> std::optional<float>
    {
        float const seconds = travel({mapId, fromX, fromY, z}, {mapId, toX, toY, z});
        if (std::isfinite(seconds) && seconds >= 0.0f)
            return seconds / QuestFields::SECONDS_PER_MINUTE;
        capped += seconds == TRAVEL_CAPPED ? 1 : 0;
        return std::nullopt;
    };
}

// The route of the bot on its map. The needs are the log's open objectives and ready quests and, while the log has a
// free slot, the takes of the quests given on the map that the take rules allow the bot (no elite or group quest,
// none it gave up): within the route's near radius, or within its far reach when the job prices far stops. The closed
// cells are the situation's. The walk function's calls are the first of the job's searches after the class quests'.
// WQ18b part 3: of the takes beyond the near radius only those QuestFields::FarTakeCandidates lists
inline Goals::RoutePlan Build(FieldMap const& map, Snapshot const& bot, CircuitContext const& context,
                              Goals::Model const& model, Goals::Situation const& situation, PartTimer const& timer = {})
{
    using Clock = std::chrono::steady_clock;
    auto const micros = [](Clock::duration span)
    {
        return static_cast<std::uint64_t>(
            std::max<long long>(std::chrono::duration_cast<std::chrono::microseconds>(span).count(), 0));
    };
    Goals::RoutePlan result;
    if (!context.quests || !Goals::Finite(bot.position))
        return result;
    Clock::time_point const started = timer ? Clock::now() : Clock::time_point{};
    QuestIndex const& index = *context.quests;
    QuestFields::Pricing const pricing{context.weights, context.unlockBonus};
    std::vector<QuestFields::Need> needs = QuestFields::Needs(index, bot.quests, bot.team, pricing, bot.groupSize);

    std::vector<std::uint32_t> inLog;
    for (QuestSlot const& slot : bot.quests)
        if (slot.quest)
            inLog.push_back(slot.quest);
    std::sort(inLog.begin(), inLog.end());
    std::vector<std::uint64_t> closed = situation.closedCells;
    std::sort(closed.begin(), closed.end());
    std::uint32_t cappedWalks = 0;

    Route::Ask ask;
    ask.mapId = bot.position.map;
    ask.x = bot.position.x;
    ask.y = bot.position.y;
    ask.level = bot.level;
    ask.team = bot.team;
    ask.runSpeed = model.runSpeed;
    ask.caution = model.cautionWeight;
    ask.groupSize = bot.groupSize;
    ask.freeSlots = QUEST_LOG_SLOTS - std::min(inLog.size(), QUEST_LOG_SLOTS);
    ask.closed = closed;
    ask.pricing = pricing;
    ask.walk = WalkOf(context.travel, ask.mapId, bot.position.z, cappedWalks);

    if (ask.freeSlots)
    {
        std::vector<std::int32_t> const groups = QuestFields::TakenGroups(index, inLog, bot.rewardedQuests);
        Take::Facts const facts{.race = bot.race,
                                .cls = bot.cls,
                                .level = bot.level,
                                .greyDiff = context.greyDiff,
                                .eliteGroup = false,
                                .rewarded = bot.rewardedQuests,
                                .inLog = inLog,
                                .givenUp = situation.givenUpQuests,
                                .takenGroups = groups};
        std::vector<std::uint32_t> candidates = QuestFields::TakeCandidates(index, ask.mapId, ask.x, ask.y, ask.radius);
        if (ask.walk)
        {
            std::vector<std::uint32_t> const beyond =
                QuestFields::FarTakeCandidates(index, facts, ask.mapId, ask.x, ask.y, ask.radius, candidates, pricing);
            candidates.insert(candidates.end(), beyond.begin(), beyond.end());
        }
        for (QuestFields::Need& take : QuestFields::TakeNeeds(index, facts, ask.mapId, candidates, pricing))
            needs.push_back(std::move(take));
    }
    Clock::time_point const listed = timer ? Clock::now() : Clock::time_point{};

    Route::Plan const built = Route::Build(map, index, needs, ask);
    result.orders.reserve(built.stops.size());
    for (Route::Stop const& stop : built.stops)
        result.orders.push_back(stop.order);
    result.worth = built.worth;
    result.minutes = built.minutes;
    result.walkCalls = built.walkCalls;
    // A place past the job's searches is no gap of the map: the route counts it with what its own budget left out
    std::uint32_t const gaps = static_cast<std::uint32_t>(built.gaps.size());
    result.gaps = gaps - std::min(gaps, cappedWalks);
    result.capped = built.capped + cappedWalks;
    if (!built.stops.empty())
        result.risk =
            QuestFields::FieldOffer(map, needs, built.stops.front().order.cell, Route::Detail::AskAt(ask, ask.x, ask.y))
                .risk;
    if (timer)
    {
        timer(PART_NEEDS, micros(listed - started));
        timer(PART_STOPS, micros(Clock::now() - listed));
    }
    return result;
}

// Goals::PlanGoals' build function over the job's field map; none while the map is not built
inline Goals::RouteBuild Builder(std::shared_ptr<FieldMap const> map, PartTimer timer = {})
{
    if (!map)
        return {};
    return [map = std::move(map), timer = std::move(timer)](
               Snapshot const& bot, CircuitContext const& context, Goals::Model const& model,
               Goals::Situation const& situation) { return Build(*map, bot, context, model, situation, timer); };
}
}  // namespace GoalRoute
}  // namespace Planner

#endif
