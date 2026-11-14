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
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "PathGenerator.h"
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
// A direction whose way ends this close is blocked, as is one whose path is this many times as long as it reaches
constexpr float STEP_BLOCKED_YARDS = 3.0f;
constexpr float STEP_DETOUR = 2.0f;
constexpr float FAN_STEP = static_cast<float>(M_PI) / 8.0f;
constexpr float FAN_LIMIT = static_cast<float>(M_PI) / 2.0f;
// The debug line's letter for each Step: taken, wall, edge, refused
constexpr char STEP_CODES[] = "TWER";
// The test flight (WP20): its time bound; the action not run this long ends the flight (the trigger was off: out of
// PvP combat); a hop's refusals are logged at most once a second
constexpr uint32 FLIGHT_MS = 30 * IN_MILLISECONDS;
constexpr uint32 FLIGHT_GAP_MS = 3 * IN_MILLISECONDS;
constexpr uint32 NOTE_MS = IN_MILLISECONDS;

// DIAG (WP21): the bot's foes by both readings: getAttackers() (units whose victim it is) and its PvP combat
// references
struct FoeView
{
    uint32 attackers = 0;
    uint32 attackerPlayers = 0;
    uint32 refs = 0;
    float nearestYards = -1.0f;
    std::string names;
};

FoeView ViewFoes(Player* bot)
{
    FoeView view;
    auto const note = [&](Unit* unit, char const* how)
    {
        float const yards = bot->GetExactDist2d(unit);
        if (view.nearestYards < 0.0f || yards < view.nearestYards)
            view.nearestYards = yards;
        view.names +=
            Acore::StringFormat(" {}:{}{}@{:.0f}", how, unit->GetName(), unit->IsPlayer() ? "" : "(npc)", yards);
    };
    for (Unit* const attacker : bot->getAttackers())
    {
        if (!attacker)
            continue;
        ++view.attackers;
        view.attackerPlayers += attacker->IsPlayer() ? 1 : 0;
        note(attacker, "a");
    }
    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* const other = ref ? ref->GetOther(bot) : nullptr;
        if (!other || !other->IsInWorld() || other->GetMapId() != bot->GetMapId())
            continue;
        ++view.refs;
        note(other, "r");
    }
    return view;
}

bool Logged() { return sLog->ShouldLog("playerbots", LogLevel::LOG_LEVEL_DEBUG); }
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
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();
    float x = botX + std::cos(angle) * STEP_YARDS;
    float y = botY + std::sin(angle) * STEP_YARDS;
    float z = botZ;
    // A step goes as far as the navmesh path to its point leads. (The navmesh ray of CheckCollisionAndGetValidCoords
    // ends at the bot itself on plain walkable ground, and MoveTo refuses a point its path does not reach.) A path
    // that ends right in front (a ledge, the mesh's end) or winds to its end is an edge
    PathGenerator path(bot);
    path.CalculatePath(botX, botY, botZ, x, y, z, false);
    Movement::PointsArray const& points = path.GetPath();
    endYards = 0.0f;
    if (path.GetPathType() & PATHFIND_NOT_USING_PATH)
    {
        // Swimming, flying or no navmesh under the bot: the straight line, as far as nothing is hit
        bool const clear = bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, botZ, x, y, z);
        endYards = bot->GetExactDist2d(x, y);
        if (!clear || endYards < STEP_BLOCKED_YARDS)
            return Step::Wall;
    }
    else
    {
        if ((path.GetPathType() & PATHFIND_NOPATH) || points.size() < 2)
            return Step::Wall;
        float length = 0.0f;
        for (std::size_t i = 1; i < points.size(); ++i)
            length += (points[i] - points[i - 1]).length();
        x = points.back().x;
        y = points.back().y;
        z = points.back().z;
        endYards = bot->GetExactDist2d(x, y);
        if (endYards < STEP_BLOCKED_YARDS || length > endYards * STEP_DETOUR)
            return Step::Edge;
    }
    if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, true, MovementPriority::MOVEMENT_COMBAT))
    {
        if (Logged())
            LOG_DEBUG("playerbots",
                      "[WorldPvp] DIAG step of {} at {:.0f} deg to ({:.1f}, {:.1f}, {:.1f}) is refused: {}",
                      bot->GetName(), angle * 180.0f / static_cast<float>(M_PI), x, y, z, RefusalCause(x, y, z, true));
        return Step::Refused;
    }
    _stepMs = nowMs;
    _stepX = x;
    _stepY = y;
    return Step::Taken;
}

// DIAG (WP21): why MoveTo refused the point, by its own checks in their order
std::string WorldPvpRetreatAction::RefusalCause(float x, float y, float z, bool exact)
{
    if (!IsMovingAllowed())
        return "cannot move";
    LastMovement const& last = *context->GetValue<LastMovement&>("last movement");
    uint32 const agoMs = getMSTime() - last.msTime;
    if (IsDuplicateMove(x, y, z))
        return Acore::StringFormat("the same point as its last move {} ms ago", agoMs);
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
        return Acore::StringFormat("waits for its last move (priority {}, {} ms ago, delay {:.0f} ms)",
                                   static_cast<uint32>(last.priority), agoMs, last.lastdelayTime);
    if (bot->GetVehicle())
        return "on a vehicle";
    if (exact || bot->IsFlying() || bot->isSwimming())
        return Acore::StringFormat("the point is its own place ({:.2f} yd), flying {}, swimming {}",
                                   bot->GetExactDist(x, y, z), bot->IsFlying(), bot->isSwimming());
    PathGenerator path(bot);
    bool const ok = path.CalculatePath(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), x, y, z, false);
    float endYards = 0.0f;
    if (!path.GetPath().empty())
        endYards = bot->GetExactDist2d(path.GetPath().back().x, path.GetPath().back().y);
    return Acore::StringFormat(
        "no path height or the path ends at the bot: path ok {} type {:#x}, {} points, ends "
        "{:.1f} yd off",
        ok, static_cast<uint32>(path.GetPathType()), path.GetPath().size(), endYards);
}

bool WorldPvpFleeTrigger::IsActive()
{
    if (!(botAI->rpgInfo.constraints & TestConstraints::FLEES))
        return false;
    Player* const bot = botAI->GetBot();
    bool const active = bot->IsAlive() && bot->GetCombatManager().HasPvPCombat();
    // DIAG (WP21): why the flight's trigger is off, once a second
    uint32 const nowMs = getMSTime();
    if (!active && nowMs - _noteMs >= NOTE_MS && Logged())
    {
        _noteMs = nowMs;
        FoeView const view = ViewFoes(bot);
        Unit* const victim = bot->GetVictim();
        LOG_DEBUG("playerbots",
                  "[WorldPvp] DIAG flee trigger of {} off: alive {}, in combat {}, pvp combat {}, attackers {} ({} "
                  "players), pvp refs {}, victim {}, state {}:{}",
                  bot->GetName(), bot->IsAlive(), bot->IsInCombat(), bot->GetCombatManager().HasPvPCombat(),
                  view.attackers, view.attackerPlayers, view.refs, victim ? victim->GetName() : "none",
                  static_cast<uint32>(botAI->GetState()), view.names);
    }
    return active;
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
    // DIAG (WP21): the flight's state once a second
    if (nowMs - _pulseMs >= NOTE_MS && Logged())
    {
        _pulseMs = nowMs;
        FoeView const view = ViewFoes(bot);
        LastMovement const& last = *context->GetValue<LastMovement&>("last movement");
        LOG_DEBUG("playerbots",
                  "[WorldPvp] DIAG {} of {} flight {:.1f} s at ({:.1f}, {:.1f}, {:.1f}): {:.1f} yd from its start, "
                  "nearest foe {:.1f} yd, attackers {} ({} players), pvp refs {}, moving {}, can move {}, rooted {}, "
                  "run speed {:.1f}, last move priority {} {} ms ago delay {:.0f} ms, state {}:{}",
                  what, bot->GetName(), (nowMs - _flightMs) / 1000.0f, bot->GetPositionX(), bot->GetPositionY(),
                  bot->GetPositionZ(), bot->GetExactDist2d(_startX, _startY), view.nearestYards, view.attackers,
                  view.attackerPlayers, view.refs, bot->isMoving(), IsMovingAllowed(), bot->IsRooted(),
                  bot->GetSpeed(MOVE_RUN), static_cast<uint32>(last.priority), nowMs - last.msTime, last.lastdelayTime,
                  static_cast<uint32>(botAI->GetState()), view.names);
    }
    if (!steps)
        return Hop(nowMs);
    // It flees from where its foes are now
    float x = 0.0f;
    float y = 0.0f;
    if (Foes(x, y))
    {
        _fromX = x;
        _fromY = y;
    }
    bool gaveUp = false;
    bool const held = StepAway(_fromX, _fromY, what, gaveUp);
    if (gaveUp)
    {
        _over = true;
        LOG_DEBUG("playerbots", "[WorldPvp] {} of {} ends: every direction blocked", what, bot->GetName());
    }
    return held;
}

// The enemy players in PvP combat with the bot, their number and centre: whoever attacks it or it attacked, a pet at
// its own place; no totems. (getAttackers() holds only the units whose melee victim the bot is: no caster)
uint32 WorldPvpFleeAction::Foes(float& x, float& y)
{
    x = 0.0f;
    y = 0.0f;
    uint32 foes = 0;
    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* const other = ref ? ref->GetOther(bot) : nullptr;
        if (!other || !other->IsInWorld() || other->GetMapId() != bot->GetMapId() || !other->IsAlive() ||
            other->IsTotem())
            continue;
        x += other->GetPositionX();
        y += other->GetPositionY();
        ++foes;
    }
    if (foes)
    {
        x /= foes;
        y /= foes;
    }
    return foes;
}

// A flight begins when players attack the bot: it flees from their centre then and stops attacking
bool WorldPvpFleeAction::Begins(uint32 nowMs, char const* what)
{
    float x = 0.0f;
    float y = 0.0f;
    uint32 const players = Foes(x, y);
    if (!players)
    {
        _flightMs = 0;
        // DIAG (WP21): why the flight does not begin, once a second
        if ((!_noteMs || nowMs - _noteMs >= NOTE_MS) && Logged())
        {
            _noteMs = nowMs;
            FoeView const view = ViewFoes(bot);
            Unit* const victim = bot->GetVictim();
            LOG_DEBUG("playerbots",
                      "[WorldPvp] DIAG {} of {} does not begin: no enemy player in PvP combat with it; {} attackers, "
                      "pvp refs {}, victim "
                      "{}:{}",
                      what, bot->GetName(), view.attackers, view.refs, victim ? victim->GetName() : "none", view.names);
        }
        return false;
    }
    _flightMs = nowMs;
    _startX = bot->GetPositionX();
    _startY = bot->GetPositionY();
    _hopMs = 0;
    _noteMs = 0;
    _over = false;
    _fromX = x;
    _fromY = y;
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
    // Its default start is no place (MAPID_INVALID at 0, 0, 0, which WorldPosition counts as set): the bot's own
    FleeManager manager(bot, STEP_YARDS, 0.0f, false, WorldPosition(bot));
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
        Note(nowMs, ("is refused the move: " + RefusalCause(x, y, z, false)).c_str());
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
