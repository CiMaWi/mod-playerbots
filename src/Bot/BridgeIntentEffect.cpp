/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeIntentEffect.h"

#include <cmath>

#include "AttackAction.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "PlayerbotSecurity.h"
#include "Playerbots.h"
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
}  // namespace

Result Execute(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize)
{
    if (!phase.Current())
        return Result::Rejected;

    switch (order.Operation)
    {
        case Intent::AttackOwnTarget:
        case Intent::Mark:
            break;
        case Intent::Follow:
        case Intent::Stay:
        case Intent::Flee:
        case Intent::Regroup:
        case Intent::Pull:
        case Intent::FocusHealing:
        case Intent::AssignRole:
        case Intent::SaveMana:
        case Intent::Formation:
        case Intent::Position:
        case Intent::DungeonClear:
            return Result::Unavailable;
        default:
            return Result::Rejected;
    }

    if (!order.Bot || !order.Controller || order.Bot == order.Controller || !order.Group || !order.Target ||
        !std::isfinite(order.VisibilityDistance) || order.VisibilityDistance <= 0.0f ||
        (order.Operation == Intent::Mark && order.Icon > 7) || !authorize || !authorize() || !phase.Current())
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
