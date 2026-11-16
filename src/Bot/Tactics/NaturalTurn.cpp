/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalTurn.h"

#include <algorithm>
#include <cmath>

#include "BotTactics.h"
#include "BotTacticsInternal.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PvpTestCounters.h"
#include "ServerFacade.h"
#include "Unit.h"

// G2a: whether the bot turns naturally now (NaturalTurn.h)
bool NaturalFacingHere(PlayerbotAI* botAI, uint32 nowMs)
{
    TurnConfig const& config = sPlayerbotAIConfig.NaturalFacing;
    if (!config.Enable || !botAI->TacticEnabledHere(TacticId::Natural))
        return false;

    TacticContext const context = botAI->GetTacticContext();
    if (config.Contexts & TacticContextBit(context))
        return true;

    // Out of PvP only in a real player's sight, and never in a dungeon or raid (their scripted facing stays exact)
    Player* const bot = botAI->GetBot();
    Map* const map = bot ? bot->GetMap() : nullptr;
    if (!config.OutsidePvpInSight || context != TacticContext::Pve || !map || map->Instanceable())
        return false;

    TurnBotState& state = botAI->GetTactics().GetTurnState();
    if (state.InSightCheckedMs == 0 || nowMs - state.InSightCheckedMs >= config.InSightCacheMs)
    {
        state.InSight = botAI->HasPlayerNearby(static_cast<float>(config.InSightYards));
        state.InSightCheckedMs = std::max(nowMs, 1u);
    }

    return state.InSight;
}

// G2a: one step of the bot's turn at the pre-gate point. A moving, controlled or dead bot ends it (a moving bot faces
// its path); the correction looks its target up again (same map). True while the turn has work left.
bool StepNaturalTurn(Player* bot, TurnBotState& state, uint32 nowMs)
{
    if (bot->isMoving() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || !bot->IsAlive())
    {
        state.Stop();
        return false;
    }

    bool haveTarget = false;
    float targetRad = 0.0f;
    if (TurnNeedsTarget(state, nowMs))
    {
        WorldObject* const target = ObjectAccessor::GetWorldObject(*bot, ObjectGuid(state.TargetGuid));
        if (target && target != bot && target->IsInWorld() && target->GetMapId() == bot->GetMapId())
        {
            haveTarget = true;
            targetRad = bot->GetAngle(target);
        }
    }

    TurnStepOut const out = StepTurn(state, nowMs, bot->GetOrientation(), haveTarget, targetRad);
    if (out.Write)
        bot->SetOrientation(out.Orientation);

    // As the snap: no packet while rooted
    if (!bot->IsRooted())
    {
        if (out.Spline)
            bot->SetFacingTo(out.SplineRad);

        if (out.Packet)
        {
            PerfMonitorScope scope(
                PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalTurn::Packet") : nullptr);
            bot->SendMovementFlagUpdate(true);
        }
    }

    return state.Active();
}

// G2a: face the target the natural way where NaturalFacingHere says so, else the instant snap (as before; also for a
// moving bot, whose path sets its facing; there the "set facing" action waits for the bot to stand, 13e-polish, so only
// a cast or a swing on the move snaps). A turn under way to the same object goes on; anything else starts a new
// one from the bot's orientation now. A bot without control (stunned, feared in place) gets the snap too: the stepper
// stops a turn there, and a refused cast would only be retried every CastRetryMs. True when the target is inside the
// core's 180-degree spell arc (Spell.cpp CheckCast), so a cast may go ahead while the turn finishes.
bool PlayerbotAI::FaceTarget(WorldObject* target, TurnSnapSite site)
{
    if (!target)
        return false;

    uint32 const nowMs = getMSTime();
    if (bot->isMoving() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || !NaturalFacingHere(this, nowMs))
    {
        if (tactics->turnState)
        {
            tactics->turnState->Stop();
            ++tactics->turnState->Snaps;
        }

        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalTurn::Snap")
                                                        : nullptr);
        // G2a measurement (13e-decide): why and from where the bot snapped, so a run can attribute instant 180s. The
        // why is the branch's cause: natural facing off here snaps whatever else holds, else moving, else no control
        // (read only for the trace line: LOG_TRACE's arguments are lazy)
        auto const why = [this, nowMs]()
        {
            return !NaturalFacingHere(this, nowMs) ? TurnSnapWhy::NotHere
                   : bot->isMoving()               ? TurnSnapWhy::Moving
                                                   : TurnSnapWhy::NoControl;
        };
        LOG_TRACE("playerbots", "Turn: {} snaps {} deg to {} ({}, {}), at {} ms", bot->GetName(),
                  static_cast<int32>(
                      std::fabs(TurnDelta(bot->GetOrientation(), bot->GetAngle(target))) / TURN_RAD_PER_DEG + 0.5f),
                  target->GetName(), TurnSnapWhyName(why()), TurnSnapSiteName(site), nowMs);
        ServerFacade::instance().SetFacingTo(bot, target);
        return true;
    }

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalTurn::Face")
                                                    : nullptr);
    TurnBotState& state = tactics->GetTurnState();
    uint64 const targetGuid = target->GetGUID().GetRawValue();
    // A direction turn that holds (blink back's 180 before its cast, TurnDirectionHolds) is not turned back: the
    // orientation stays, the answer is the arc's
    if ((!state.Turning() || state.TargetGuid != targetGuid) && !TurnDirectionHolds(state, nowMs))
    {
        TurnRefreshRules(state.Params, sPlayerbotAIConfig.NaturalFacing);
        float const targetRad = bot->GetAngle(target);
        TurnPlan const plan = PlanTurn(state.Params, bot->GetOrientation(), targetRad, nowMs,
                                       TurnHash(bot->GetGUID().GetCounter(), nowMs, state.Turns));
        state.Begin(plan, targetGuid, nowMs);
        if (plan.Leg.Spline && !bot->IsRooted())
            bot->SetFacingTo(plan.Leg.Goal());

        tactics->SetNaturalPreGate(true);
        CountTactic(TacticCounter::TurnFlicks);
        LOG_TRACE("playerbots",
                  "Turn: {} turns {} deg to {} in {} ms ({}), error {:.1f} deg, correction in {} ms (0: none), "
                  "skill {}, at {} ms",
                  bot->GetName(), state.LastTurnDeg, target->GetName(), plan.Leg.DurMs,
                  plan.Leg.Keyboard ? "keyboard" : (plan.Leg.Spline ? "spline" : "flick"), state.LastErrorDeg,
                  plan.Correct ? plan.CorrectDelayMs : 0u, GetPvpSkill(), nowMs);
    }

    return bot->HasInArc(static_cast<float>(M_PI), target);
}

// G2a: a direction turn before a cast that goes the way the bot faces (NaturalTurn.h TurnDirectionNext). Where
// NaturalFacingHere says so and the bot stands in control, a natural turn to the target's angle plus offsetRad (no
// correction: a direction, not an object) and false until its flick is over, so the caller waits; while it holds,
// FaceTarget and "set facing" turn nothing back (TurnDirectionHolds). An attempt whose turns cannot finish within
// TURN_DIRECTION_GIVE_UP_MS (someone else turns the bot back) takes the instant write. Else the instant orientation
// write as before (no packet) and true.
bool NaturalTurnForCast(PlayerbotAI* botAI, Unit* target, float offsetRad)
{
    Player* const bot = botAI->GetBot();
    uint32 const nowMs = getMSTime();
    if (bot->isMoving() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || !NaturalFacingHere(botAI, nowMs))
    {
        if (sPlayerbotAIConfig.NaturalFacing.Enable)
            TurnDirectionEnd(
                botAI->GetTactics().GetTurnState());  // the cast goes now: an earlier attempt holds nothing

        bot->SetOrientation(bot->GetAngle(target) + offsetRad);
        return true;
    }

    TurnBotState& state = botAI->GetTactics().GetTurnState();
    uint64 const key = target->GetGUID().GetRawValue();
    float const goalRad = TurnWrap(bot->GetAngle(target) + offsetRad);
    TurnDirectionStep const step = TurnDirectionNext(state, key, bot->GetOrientation(), goalRad, nowMs);
    if (step == TurnDirectionStep::Wait)
        return false;

    if (step == TurnDirectionStep::Ready || step == TurnDirectionStep::Snap)
    {
        if (step == TurnDirectionStep::Snap)
        {
            state.Stop();
            ++state.Snaps;
            bot->SetOrientation(goalRad);
            LOG_TRACE("playerbots", "Turn: {} gives up a turn to {} deg off {} after {} ms, instant, at {} ms",
                      bot->GetName(), static_cast<int32>(offsetRad / TURN_RAD_PER_DEG + 0.5f), target->GetName(),
                      nowMs - state.DirectionSinceMs, nowMs);
        }

        TurnDirectionEnd(state);
        return true;
    }

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalTurn::Direction")
                                                    : nullptr);
    TurnRefreshRules(state.Params, sPlayerbotAIConfig.NaturalFacing);
    TurnPlan const plan = PlanTurn(state.Params, bot->GetOrientation(), goalRad, nowMs,
                                   TurnHash(bot->GetGUID().GetCounter(), nowMs, state.Turns));
    TurnDirectionBegin(state, plan, key, nowMs);
    if (plan.Leg.Spline && !bot->IsRooted())
        bot->SetFacingTo(plan.Leg.Goal());

    botAI->GetTactics().SetNaturalPreGate(true);
    botAI->CountTactic(TacticCounter::TurnFlicks);
    LOG_TRACE(
        "playerbots",
        "Turn: {} turns {} deg to {} deg off {} in {} ms ({}), error {:.1f} deg, no correction, skill {}, at {} ms",
        bot->GetName(), state.LastTurnDeg, static_cast<int32>(offsetRad / TURN_RAD_PER_DEG + 0.5f), target->GetName(),
        plan.Leg.DurMs, plan.Leg.Keyboard ? "keyboard" : (plan.Leg.Spline ? "spline" : "flick"), state.LastErrorDeg,
        botAI->GetPvpSkill(), nowMs);
    return false;
}

bool NaturalDirectionTurnHolds(PlayerbotAI* botAI)
{
    return sPlayerbotAIConfig.NaturalFacing.Enable &&
           TurnDirectionHolds(botAI->GetTactics().GetTurnState(), getMSTime());
}
