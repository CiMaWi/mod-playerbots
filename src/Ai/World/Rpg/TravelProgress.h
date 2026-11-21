/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELPROGRESS_H
#define PLAYERBOTS_TRAVELPROGRESS_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "TravelLegRules.h"
#include "TravelProgressState.h"

namespace TravelProgress
{
struct Observation
{
    std::uint32_t NowMs;
    Owner Current;
    bool Combat;
    std::uint32_t LootCompleted;
};

// The walk's part of a pass: the owner's clocks, the sample of the owned move and the walk budget. A walk with no
// trip (GoTo's local piece, WT64b) is observed by this alone; Maintain adds a trip's deadlines. sampled: a sample
// was due
template <class Read>
Sample MaintainWalk(State& progress, Observation const& seen, Read&& read, bool& sampled)
{
    Sample sample = progress.Observe(seen.NowMs, seen.Current, seen.Combat, seen.LootCompleted);
    sampled = progress.SampleDue(seen.NowMs);
    if (!sampled)
        return sample;
    progress.NoteSample(seen.NowMs);

    if (progress.HasMotion())
        sample.Yards = progress.ObserveMotion(read());
    if (seen.Current == Owner::Walk || seen.Current == Owner::Leg)
        progress.CheckWalkBudget();
    return sample;
}

// A walk with no trip after a span nobody served it: GoTo's executor is not ticked during a fight, where a trip's
// observer sees owner Combat. A span over gapMs adds nothing to the clocks and revokes the owned move, so a fight on
// the way is no lack of progress; a shorter one is the executor's own tick spacing and counts as walking
inline void SkipGap(State& progress, std::uint32_t nowMs, std::uint32_t lastMs, std::uint32_t gapMs)
{
    if (nowMs - lastMs > gapMs)
        progress.Suspend();
}

template <class Read>
Sample Maintain(State& progress, Observation const& seen, TravelLegs::Trip const& trip,
                TravelLegs::RouteWait const& route, float routeSeconds, Read&& read)
{
    bool sampled = false;
    Sample const sample = MaintainWalk(progress, seen, read, sampled);
    if (!sampled)
        return sample;

    // The follower decides every expiry; the guard only wakes it when the one deadline it reads too has passed
    bool const acting = seen.Current == Owner::Walk || seen.Current == Owner::Leg;
    if (TravelLegs::NextDeadlineMs(trip, seen.NowMs, progress.LegMs(), &route) < 0)
        progress.Latch(Overdue::Due);
    if (acting && trip.legIdx < trip.legs.size() && TravelLegs::PastDeadline(progress.RouteMs(), routeSeconds))
        progress.Latch(Overdue::RouteDeadline);
    return sample;
}

template <class RouteWait, class Submit>
bool SubmitRoute(RouteWait& wait, RouteWait const& request, Submit&& submit)
{
    if (!submit())
        return false;
    wait = request;
    return true;
}

template <class Read, class Move, class Resumed>
bool ContinueWalk(State& state, Read&& read, Move&& move, Resumed&& resumed)
{
    state.AttemptWalk();
    Motion const before = read();
    bool const used = move();
    Motion const after = read();
    if (used && after.Active && after.Generator && before.Id != after.Id)
        state.CommitMotion(after);
    else if (after.Active && state.SameGenerator(after) && !state.Owns(after.Id))
        state.CommitMotion(after);
    if (after.Active && state.Owns(after.Id) && state.SameGenerator(after))
    {
        if (char const* const reason = state.TakeResume())
            resumed(reason);
    }
    return used;
}
}  // namespace TravelProgress

#endif
