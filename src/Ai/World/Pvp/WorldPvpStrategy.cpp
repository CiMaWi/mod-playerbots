/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldPvpStrategy.h"

#include <cmath>
#include <string>

#include "AiObjectContext.h"
#include "Event.h"
#include "FightBook.h"
#include "FightWiring.h"
#include "FleeManager.h"
#include "Log.h"
#include "Map.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "Timer.h"

namespace
{
// One step of the retreat, and how long a step under way holds the turn (a 20 yd step takes about 3 s at run speed)
constexpr float STEP_YARDS = 20.0f;
constexpr uint32 STEP_HOLD_MS = 4 * IN_MILLISECONDS;
// A step this close to its end is done
constexpr float STEP_DONE_YARDS = 2.0f;
// A direction whose first collision lies this close is blocked
constexpr float STEP_BLOCKED_YARDS = 3.0f;
constexpr float FAN_STEP = static_cast<float>(M_PI) / 8.0f;
constexpr float FAN_LIMIT = static_cast<float>(M_PI) / 2.0f;
// The debug line's letter for each Step: taken, wall, edge, refused
constexpr char STEP_CODES[] = "TWER";
// The test flight (WP20): its time bound; the action not run this long ends the flight (the trigger was off: out of
// PvP combat); a hop's refusals are logged at most once a second
constexpr uint32 FLIGHT_MS = 30 * IN_MILLISECONDS;
constexpr uint32 FLIGHT_GAP_MS = 3 * IN_MILLISECONDS;
constexpr uint32 NOTE_MS = IN_MILLISECONDS;
}  // namespace

void WorldPvpStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("world pvp retreat", {NextAction("world pvp retreat", ACTION_EMERGENCY + 1.0f)}));
    triggers.push_back(new TriggerNode("world pvp flee", {NextAction("world pvp flee", ACTION_EMERGENCY + 2.0f)}));
}

bool WorldPvpRetreatTrigger::IsActive()
{
    if (botAI->rpgInfo.constraints & TestConstraints::FLEES)
        return false;
    float x = 0.0f;
    float y = 0.0f;
    return WorldPvp::RetreatFrom(botAI, x, y);
}

bool WorldPvpRetreatAction::isUseful()
{
    float x = 0.0f;
    float y = 0.0f;
    return bot->IsAlive() && WorldPvp::RetreatFrom(botAI, x, y);
}

bool WorldPvpRetreatAction::Execute(Event /*event*/)
{
    float fromX = 0.0f;
    float fromY = 0.0f;
    if (!WorldPvp::RetreatFrom(botAI, fromX, fromY))
        return false;
    bool gaveUp = false;
    bool const held = StepAway(fromX, fromY, "retreat", gaveUp);
    if (gaveUp)
        WorldPvp::GiveUpRetreat(botAI);
    return held;
}

bool WorldPvpRetreatAction::StepAway(float fromX, float fromY, char const* what, bool& gaveUp)
{
    gaveUp = false;
    uint32 const nowMs = getMSTime();
    if (bot->isMoving() && nowMs - _stepMs < STEP_HOLD_MS && bot->GetExactDist2d(_stepX, _stepY) > STEP_DONE_YARDS)
        return true;
    // A bot that cannot move now (rooted, stunned, feared) waits and may fight back meanwhile; the window bounds it
    if (!IsMovingAllowed())
    {
        LOG_DEBUG("playerbots", "[WorldPvp] DIAG {} of {} at ({:.1f}, {:.1f}): cannot move", what, bot->GetName(),
                  bot->GetPositionX(), bot->GetPositionY());
        return false;
    }
    // The last combat move (the rotation's approach, a step's end) still runs its delay, so a new move is refused:
    // the turn is held and the next tick steps
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
    {
        LOG_DEBUG("playerbots", "[WorldPvp] DIAG {} of {} at ({:.1f}, {:.1f}): waits for its last move", what,
                  bot->GetName(), bot->GetPositionX(), bot->GetPositionY());
        return true;
    }

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp retreat")
                                                    : nullptr);
    float const dx = bot->GetPositionX() - fromX;
    float const dy = bot->GetPositionY() - fromY;
    float const away = dx * dx + dy * dy > 1.0f ? std::atan2(dy, dx) : bot->GetOrientation() + static_cast<float>(M_PI);
    uint32 tried = 0;
    uint32 blocked = 0;
    bool const logged = sLog->ShouldLog("playerbots", LogLevel::LOG_LEVEL_DEBUG);
    std::string fan;
    auto const step = [&](float delta)
    {
        float endYards = 0.0f;
        Step const result = StepTo(away + delta, nowMs, endYards);
        ++tried;
        blocked += result == Step::Wall || result == Step::Edge ? 1 : 0;
        if (logged)
            fan += Acore::StringFormat(" {:+.0f}:{}{:.1f}", delta * 180.0f / static_cast<float>(M_PI),
                                       STEP_CODES[static_cast<uint8>(result)], endYards);
        return result == Step::Taken;
    };
    // A turn whose straight step was not taken says what each direction came to
    auto const note = [&](char const* outcome)
    {
        if (logged && tried > 1)
            LOG_DEBUG("playerbots", "[WorldPvp] {} of {} at ({:.1f}, {:.1f}, {:.1f}) {}, {} of {} blocked:{}", what,
                      bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), outcome, blocked,
                      tried, fan);
    };
    bool taken = step(0.0f);
    for (float delta = FAN_STEP; !taken && delta <= FAN_LIMIT + 0.01f; delta += FAN_STEP)
        taken = step(delta) || step(-delta);
    if (taken)
    {
        note("steps aside");
        return true;
    }
    // Only walls in every direction give the retreat up (the bot fights where it stands); a refused move holds
    if (!WorldPvp::GivesUpRetreat(blocked, tried))
    {
        note("holds");
        return true;
    }
    note("gives up");
    gaveUp = true;
    return false;
}

WorldPvpRetreatAction::Step WorldPvpRetreatAction::StepTo(float angle, uint32 nowMs, float& endYards)
{
    float x = bot->GetPositionX() + std::cos(angle) * STEP_YARDS;
    float y = bot->GetPositionY() + std::sin(angle) * STEP_YARDS;
    float z = bot->GetPositionZ();
    bool exact = true;
    bool const clear = bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                      bot->GetPositionZ(), x, y, z);
    // The straight line ends at (x, y): at the first collision, or, with no collision, where the navmesh ends (a
    // ledge, a slope too steep, most walls). An end right in front blocks the direction either way: a step there does
    // not move the bot, and taken every tick it kept the other directions untried for the whole retreat
    endYards = bot->GetExactDist2d(x, y);
    if (endYards < STEP_BLOCKED_YARDS)
        return clear ? Step::Edge : Step::Wall;
    if (!clear)
    {
        // A farther collision is walked around on a path to the unclipped point
        x = bot->GetPositionX() + std::cos(angle) * STEP_YARDS;
        y = bot->GetPositionY() + std::sin(angle) * STEP_YARDS;
        z = bot->GetPositionZ();
        exact = false;
    }
    if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, exact, MovementPriority::MOVEMENT_COMBAT))
        return Step::Refused;
    _stepMs = nowMs;
    _stepX = x;
    _stepY = y;
    return Step::Taken;
}

bool WorldPvpFleeTrigger::IsActive()
{
    if (!(botAI->rpgInfo.constraints & TestConstraints::FLEES))
        return false;
    Player* const bot = botAI->GetBot();
    return bot->IsAlive() && bot->GetCombatManager().HasPvPCombat();
}

bool WorldPvpFleeAction::isUseful() { return bot->IsAlive() && (botAI->rpgInfo.constraints & TestConstraints::FLEES); }

bool WorldPvpFleeAction::Execute(Event /*event*/)
{
    uint16 const set = botAI->rpgInfo.constraints;
    bool const steps = TestConstraints::FleeStep(set);
    if (!steps && !TestConstraints::FleeMgr(set))
        return false;
    char const* const what = steps ? "flee step" : "flee mgr";
    uint32 const nowMs = getMSTime();
    bool const fresh = !_flightMs || nowMs - _lastMs > FLIGHT_GAP_MS;
    _lastMs = nowMs;
    if (fresh && !Begins(nowMs, what))
        return false;
    if (_over)
        return false;
    if (nowMs - _flightMs > FLIGHT_MS)
    {
        _over = true;
        LOG_DEBUG("playerbots", "[WorldPvp] {} of {} at ({:.1f}, {:.1f}, {:.1f}) ends by its time bound", what,
                  bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        return false;
    }
    if (!steps)
        return Hop(nowMs);
    bool gaveUp = false;
    bool const held = StepAway(_fromX, _fromY, what, gaveUp);
    if (gaveUp)
    {
        _over = true;
        LOG_DEBUG("playerbots", "[WorldPvp] {} of {} ends: every direction blocked", what, bot->GetName());
    }
    return held;
}

// A flight begins when players attack the bot: it flees from their centre then and stops attacking
bool WorldPvpFleeAction::Begins(uint32 nowMs, char const* what)
{
    float x = 0.0f;
    float y = 0.0f;
    uint32 players = 0;
    for (Unit* const attacker : bot->getAttackers())
    {
        if (!attacker || !attacker->IsPlayer())
            continue;
        x += attacker->GetPositionX();
        y += attacker->GetPositionY();
        ++players;
    }
    if (!players)
    {
        _flightMs = 0;
        return false;
    }
    _flightMs = nowMs;
    _hopMs = 0;
    _noteMs = 0;
    _over = false;
    _fromX = x / players;
    _fromY = y / players;
    bot->AttackStop();
    context->GetValue<Unit*>("current target")->Set(nullptr);
    LOG_DEBUG("playerbots",
              "[WorldPvp] {} of {} begins at ({:.1f}, {:.1f}, {:.1f}): {} players attack it, their centre ({:.1f}, "
              "{:.1f}) {:.1f} yd off",
              what, bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), players, _fromX,
              _fromY, bot->GetExactDist2d(_fromX, _fromY));
    return true;
}

// One FleeManager point of a step's length, moved to as a step is; the turn is held while the hop runs. The point's
// candidates need line of sight to the current target, so the target is dropped first
bool WorldPvpFleeAction::Hop(uint32 nowMs)
{
    if (bot->isMoving() && nowMs - _hopMs < STEP_HOLD_MS && bot->GetExactDist2d(_hopX, _hopY) > STEP_DONE_YARDS)
        return true;
    if (!IsMovingAllowed())
    {
        Note(nowMs, "cannot move");
        return false;
    }
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
    {
        Note(nowMs, "waits for its last move");
        return true;
    }
    bot->AttackStop();
    context->GetValue<Unit*>("current target")->Set(nullptr);
    FleeManager manager(bot, STEP_YARDS, 0.0f);
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!manager.CalculateDestination(&x, &y, &z))
    {
        Note(nowMs, "finds no point");
        return false;
    }
    if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, false, MovementPriority::MOVEMENT_COMBAT))
    {
        Note(nowMs, "is refused the move");
        return false;
    }
    _hopMs = nowMs;
    _hopX = x;
    _hopY = y;
    LOG_DEBUG("playerbots",
              "[WorldPvp] flee mgr of {} at ({:.1f}, {:.1f}, {:.1f}) hops to ({:.1f}, {:.1f}, {:.1f}), {:.1f} yd; "
              "{:.1f} yd from the centre",
              bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), x, y, z,
              bot->GetExactDist2d(x, y), bot->GetExactDist2d(_fromX, _fromY));
    return true;
}

void WorldPvpFleeAction::Note(uint32 nowMs, char const* outcome)
{
    if (_noteMs && nowMs - _noteMs < NOTE_MS)
        return;
    _noteMs = nowMs;
    LOG_DEBUG("playerbots", "[WorldPvp] DIAG flee mgr of {} at ({:.1f}, {:.1f}, {:.1f}): {}", bot->GetName(),
              bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), outcome);
}
