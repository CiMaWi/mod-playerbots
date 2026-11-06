/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeIntentEffect.h"

#include <cmath>

#include "AttackAction.h"
#include "FollowActions.h"
#include "Group.h"
#include "MovementActions.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "PlayerbotSecurity.h"
#include "Playerbots.h"
#include "PositionAction.h"
#include "StayActions.h"
#include "ThreadCheck.h"
#include "WorldSession.h"

namespace PlayerbotBridge
{
PostMapJoin::PostMapJoin() : _thread(std::this_thread::get_id()) {}

bool PostMapJoin::Current() const
{
    // Unlike the diagnostic checker, an uninitialized world thread never grants authority.
    return _thread == std::this_thread::get_id() &&
           _thread == PlayerbotThreadCheck::worldThread.load(std::memory_order_relaxed);
}

namespace
{
bool Visible(Player* observer, Unit* target, float distance)
{
    return target && target->IsInWorld() && observer->IsInMap(target) && observer->InSamePhase(target) &&
           observer->IsWithinDistInMap(target, distance) && observer->CanSeeOrDetect(target, false, true);
}

// Only an attack or a mark names a unit; the wire carries no target for the other verbs.
bool NeedsTarget(Intent operation) { return operation == Intent::AttackOwnTarget || operation == Intent::Mark; }

// One move toward the formation location around the master; the follow strategy itself is left unchanged.
Result FollowOnce(PlayerbotAI* ai, Player* bot, Player* controller)
{
    if (ai->GetMaster() != controller || !bot->IsAlive())
        return Result::Rejected;

    FollowAction follow(ai);
    if (!follow.isUseful())
        return Result::Rejected;

    return follow.Execute(Event()) ? Result::Applied : Result::Failed;
}

// Stops the current movement once; an active follow strategy resumes at the engine's next movement decision.
Result StayOnce(PlayerbotAI* ai, Player* bot)
{
    if (!bot->IsAlive())
        return Result::Rejected;

    StayAction stay(ai);
    if (!stay.isUseful())
        return Result::Rejected;

    return stay.Execute(Event()) ? Result::Applied : Result::Failed;
}

// Moves away once from the bot's own opponent (its "current target" value, never wire data). Not the group-member
// Flee search, which picks a random spot and speaks.
Result FleeOnce(PlayerbotAI* ai, Player* bot)
{
    if (!bot->IsAlive() || bot->IsMounted())
        return Result::Rejected;

    // Without a live opponent MoveAway changes nothing: a refusal, not a failed attempt. IsInMap needs both in world.
    Unit* opponent = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!opponent || !opponent->IsAlive() || !bot->IsInMap(opponent))
        return Result::Rejected;

    FleeAction flee(ai);
    if (!flee.isUseful())
        return Result::Rejected;

    return flee.Execute(Event()) ? Result::Applied : Result::Failed;
}

bool FinitePosition(Order const& order)
{
    return std::isfinite(order.X) && std::isfinite(order.Y) && std::isfinite(order.Z);
}

// Within the radius in double precision: a huge finite coordinate fails here instead of overflowing a float.
bool Near(Player* player, Order const& order)
{
    double const dx = order.X - player->GetPositionX();
    double const dy = order.Y - player->GetPositionY();
    double const dz = order.Z - player->GetPositionZ();
    double const radius = order.VisibilityDistance;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

// One move to the checked position; no stored named position is read or written. An active follow or guard strategy
// may move the bot on at the engine's next movement decision.
Result PositionOnce(PostMapJoin const& phase, PlayerbotAI* ai, Player* bot, Order const& order)
{
    if (!bot->IsAlive() || !PositionReachable(phase, order))
        return Result::Rejected;

    MoveToPositionAction move(ai, "move to position", "bridge");
    return move.ExecuteExactPosition(order.MapId, static_cast<float>(order.X), static_cast<float>(order.Y),
                                     static_cast<float>(order.Z))
               ? Result::Applied
               : Result::Failed;
}
}  // namespace

bool PositionReachable(PostMapJoin const& phase, Order const& order)
{
    if (!phase.Current() || !FinitePosition(order) || !std::isfinite(order.VisibilityDistance) ||
        order.VisibilityDistance <= 0.0f)
        return false;

    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Bot));
    Player* controller = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Controller));
    if (!bot || !bot->IsInWorld() || !controller || !controller->IsInWorld() || bot->GetMapId() != order.MapId ||
        !bot->IsInMap(controller) || !Near(bot, order) || !Near(controller, order))
        return false;

    // Stricter than MoveTo's own search: a complete navmesh path only, never a partial path or a shortcut.
    PathGenerator path(bot);
    path.CalculatePath(static_cast<float>(order.X), static_cast<float>(order.Y), static_cast<float>(order.Z), false);
    return path.GetPathType() == PATHFIND_NORMAL && path.getPathLength() <= 2.0f * order.VisibilityDistance;
}

Result Execute(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize)
{
    if (!phase.Current())
        return Result::Rejected;

    switch (order.Operation)
    {
        case Intent::Follow:
        case Intent::Stay:
        case Intent::Flee:
        case Intent::Position:
        case Intent::AttackOwnTarget:
        case Intent::Mark:
            break;
        case Intent::Regroup:
        case Intent::Pull:
        case Intent::FocusHealing:
        case Intent::AssignRole:
        case Intent::SaveMana:
        case Intent::Formation:
        case Intent::DungeonClear:
            return Result::Unavailable;
        default:
            return Result::Rejected;
    }

    // A stray target on a target-less verb is a mapping error, never an ignored field.
    if (!order.Bot || !order.Controller || order.Bot == order.Controller || !order.Group ||
        NeedsTarget(order.Operation) != (order.Target != 0) || !std::isfinite(order.VisibilityDistance) ||
        order.VisibilityDistance <= 0.0f || (order.Operation == Intent::Mark && order.Icon > 7) ||
        (order.Operation == Intent::Position && !FinitePosition(order)) || !authorize || !authorize() ||
        !phase.Current())
        return Result::Rejected;

    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Bot));
    Player* controller = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Controller));
    WorldSession const* controllerSession = controller ? controller->GetSession() : nullptr;
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    PlayerbotAI* controllerAI = controller ? GET_PLAYERBOT_AI(controller) : nullptr;
    if (!bot || !bot->IsInWorld() || !ai || !ai->IsBotAI() || !controllerSession || controllerSession->IsHeadless() ||
        (controllerAI && controllerAI->IsBotAI()) || !Visible(bot, controller, order.VisibilityDistance) ||
        !Visible(controller, bot, order.VisibilityDistance) || bot->GetTeamId() != controller->GetTeamId())
        return Result::Rejected;

    Group* group = bot->GetGroup();
    if (!group || group->GetGUID() != ObjectGuid(order.Group) || controller->GetGroup() != group ||
        !group->IsMember(bot->GetGUID()) || !group->IsMember(controller->GetGUID()) ||
        (ai->GetMaster() ? ai->GetMaster() != controller : !group->IsLeader(controller->GetGUID())) ||
        !ai->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_ALLOW_ALL, true, controller))
        return Result::Rejected;

    switch (order.Operation)
    {
        case Intent::Follow:
            return FollowOnce(ai, bot, controller);
        case Intent::Stay:
            return StayOnce(ai, bot);
        case Intent::Flee:
            return FleeOnce(ai, bot);
        case Intent::Position:
            return PositionOnce(phase, ai, bot, order);
        default:
            break;
    }

    Unit* target = ObjectAccessor::GetUnit(*bot, ObjectGuid(order.Target));
    if (!Visible(bot, target, order.VisibilityDistance) || !Visible(controller, target, order.VisibilityDistance))
        return Result::Rejected;

    if (order.Operation == Intent::Mark)
    {
        if ((group->isRaidGroup() && !group->IsLeader(controller->GetGUID()) &&
             !group->IsAssistant(controller->GetGUID())) ||
            (target->IsPlayer() &&
             (ObjectAccessor::FindConnectedPlayer(target->GetGUID()) != target || target->IsHostileTo(controller))))
            return Result::Rejected;

        group->SetTargetIcon(order.Icon, controller->GetGUID(), target->GetGUID());
        return group->GetTargetIcon(order.Icon) == target->GetGUID() ? Result::Applied : Result::Failed;
    }

    if (bot->GetTarget() != target->GetGUID() || !bot->IsAlive() || bot->IsMounted())
        return Result::Rejected;

    // A bridge attack cannot defer the attack or update the wait strategy's combat-start timer.
    if (ai->HasStrategy("wait for attack", BOT_STATE_COMBAT))
        return Result::Failed;

    AttackAction attack(ai, "attack");
    // Attack can fail after changing selection/AI/movement. Failed is an attempted result, never a retry grant.
    return attack.ExecuteExactTarget(target) ? Result::Applied : Result::Failed;
}
}  // namespace PlayerbotBridge
