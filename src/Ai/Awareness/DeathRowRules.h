/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_DEATHROWRULES_H
#define _PLAYERBOT_DEATHROWRULES_H

#include "AwarenessRules.h"
#include "WorldTravelRules.h"

// AW9: the `death` decision row (WU33a's words, upkeep §3.14) of a step of the rule of the dead. Header-only so
// tests/awareness-rules-selftest.cpp checks the same mapping the server writes
namespace AwarenessRules
{
// A cause of the healer's way or the bound in the row's words; `None` for a reason that is no cause
constexpr WorldTravel::GhostReason RowCause(DeadReason cause)
{
    switch (cause)
    {
        case DeadReason::CorpseAway:
            return WorldTravel::GhostReason::CorpseAway;
        case DeadReason::CorpseRunsExhausted:
            return WorldTravel::GhostReason::RunsExhausted;
        case DeadReason::CorpseCamped:
            return WorldTravel::GhostReason::Camped;
        case DeadReason::NoPath:
            return WorldTravel::GhostReason::NoPath;
        case DeadReason::NoClearPoint:
            return WorldTravel::GhostReason::NoClearPoint;
        case DeadReason::WaitEnded:
            return WorldTravel::GhostReason::WaitEnded;
        default:
            return WorldTravel::GhostReason::None;
    }
}

// The row of a step: a walk to the corpse or a point `corpse_walk/moving`; the wait without a clear point
// `wait/no_clear_point`; the walk to the grave `healer_walk/<cause>`; the wait at the grave `healer_wait/no_healer`;
// the wait while a player would see the teleport `blocked/seen`; the teleport `teleport/<cause>` (`wait_ended` at the
// bound). No row for the other waits (an enemy player's hold writes world PvP's, a dead leader none), the wait
// without a grave, the rise (its Rise row) and the revives (the healer's revive writes `spirit_healer/<cause>`)
constexpr WorldTravel::GhostWayResult DeathRowOf(DeadStep const& step, DeadReason healerReason)
{
    using WorldTravel::GhostReason;
    using WorldTravel::GhostWay;
    switch (step.act)
    {
        case DeadAct::WalkCorpse:
        case DeadAct::WalkPoint:
            return {GhostWay::CorpseWalk, GhostReason::Moving};
        case DeadAct::Wait:
            return step.reason == DeadReason::NoClearPoint
                       ? WorldTravel::GhostWayResult{GhostWay::Wait, GhostReason::NoClearPoint}
                       : WorldTravel::GhostWayResult{};
        case DeadAct::WalkGrave:
            return {GhostWay::HealerWalk, RowCause(HealerCause(step, healerReason))};
        case DeadAct::TeleportGrave:
            return {GhostWay::Teleport, RowCause(HealerCause(step, healerReason))};
        case DeadAct::WaitGrave:
            if (step.reason == DeadReason::NoHealer)
                return {GhostWay::HealerWait, GhostReason::NoHealer};
            if (step.reason == DeadReason::Seen)
                return {GhostWay::Blocked, GhostReason::Seen};
            return {};
        default:
            return {};
    }
}
}  // namespace AwarenessRules

#endif
