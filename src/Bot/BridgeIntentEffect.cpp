/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeIntentEffect.h"

#include <cmath>
#include <list>

#include "AttackAction.h"
#include "FollowActions.h"
#include "Group.h"
#include "MovementActions.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionAction.h"
#include "PositionValue.h"
#include "StayActions.h"
#include "TestPlayer.h"
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

Result Refuse(Refusal& why, Refusal reason)
{
    why = reason;
    return Result::Rejected;
}

// The one admission test for a conversation partner: a player's own client, or the headless test player standing in
// for one; never a bot. DS.11's later bot-to-bot conversations widen this one function. Any such player of the bot's
// team may make any bot act: safe only because the user is the only player (DS.12); a server with several players
// needs a per-bot partner rule here.
bool PartnerAllowed(Player* partner)
{
    WorldSession const* session = partner ? partner->GetSession() : nullptr;
    PlayerbotAI* partnerAI = partner ? GET_PLAYERBOT_AI(partner) : nullptr;
    return session && (!session->IsHeadless() || TestPlayerMgr::Instance().IsTestPlayer(partner)) &&
           !(partnerAI && partnerAI->IsBotAI());
}

// Only an attack, a mark or a focus heal names a unit; the wire carries no target for the other verbs.
bool NeedsTarget(Intent operation)
{
    return operation == Intent::AttackOwnTarget || operation == Intent::Mark || operation == Intent::FocusHealing;
}

char const* FormationName(FormationShape shape)
{
    switch (shape)
    {
        case FormationShape::Close:
            return "near";
        case FormationShape::Spread:
            return "far";
        case FormationShape::Line:
            return "line";
        default:
            return nullptr;
    }
}

// The strategies the follow and stay commands change (ChatShortcutActions.cpp), in Baseline::Strategies bit order,
// first for the non-combat state, then for the combat state. Runaway and guard are there because adding follow or stay
// drops them: the four are one sibling group (MovementStrategyContext, Engine::addStrategy).
char const* const MovementStrategies[] = {"follow", "stay", "runaway", "guard", "passive", "grind", "move from group"};
BotState const MovementStates[] = {BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT};

// Follow as playerbots' "follow" command does (ChatShortcutActions.cpp FollowChatShortcutAction), without its
// "Following" line: the bridge reply speaks. Execute admitted only a partner of the bot's group outside battlegrounds,
// so a masterless bot here is grouped and takes the partner as master, as PlayerbotAI::FindNewMaster picks a player of
// its group. A bot with a master keeps it and follows that master, so Applied means "follows the partner" only
// because the user is the only player (DS.12) and, outside battlegrounds, playerbots gives a bot a real player as
// master; with several players this would need the partner as master. An ungrouped bot never reaches this.
Result FollowOnce(PlayerbotAI* ai, Player* partner)
{
    if (!ai->GetMaster())
    {
        ai->SetMaster(partner);
        ai->ResetStrategies();
    }

    ai->ChangeStrategy("+follow,-passive,-grind,-move from group", BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("-stay,-follow,-passive,-grind,-move from group", BOT_STATE_COMBAT);
    ai->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Reset();
    PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
    positions["return"].Reset();
    positions["stay"].Reset();

    // One move now toward the formation location; a bot already there follows from here on without one.
    FollowAction follow(ai);
    if (!follow.isUseful())
        return Result::Applied;

    return follow.Execute(Event()) ? Result::Applied : Result::Failed;
}

// Stay as playerbots' "stay" command does (ChatShortcutActions.cpp StayChatShortcutAction), without its "Staying"
// line, after one stop of a moving bot. A standing bot stays too; a bot on a flight path cannot, and nothing changes.
Result StayOnce(PlayerbotAI* ai, Player* bot)
{
    StayAction stay(ai);
    if (!stay.Execute(Event()))
        return Result::Failed;

    ai->Reset();
    ai->ChangeStrategy("+stay,-passive,-move from group", BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+stay,-follow,-passive,-move from group", BOT_STATE_COMBAT);
    PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
    positions["return"].Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    positions["stay"].Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    return Result::Applied;
}

// Moves away once from the bot's own opponent (its "current target" value, never wire data). Not the group-member
// Flee search, which picks a random spot and speaks.
Result FleeOnce(PlayerbotAI* ai, Player* bot, Refusal& why)
{
    if (bot->IsMounted())
        return Refuse(why, Refusal::CannotNow);

    // Without a live opponent MoveAway changes nothing: a refusal, not a failed attempt. IsInMap needs both in world.
    Unit* opponent = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (!opponent || !opponent->IsAlive() || !bot->IsInMap(opponent))
        return Refuse(why, Refusal::CannotNow);

    FleeAction flee(ai);
    if (!flee.isUseful())
        return Refuse(why, Refusal::CannotNow);

    return flee.Execute(Event()) ? Result::Applied : Result::Failed;
}

// Sets the formation as the "formation <name>" command does (SetFormationAction), without its reply line.
Result FormationOnce(PlayerbotAI* ai, Order const& order)
{
    char const* name = FormationName(order.Shape);
    if (!name)
        return Result::Rejected;

    UntypedValue* formation = ai->GetAiObjectContext()->GetUntypedValue("formation");
    if (!formation)
        return Result::Failed;

    return formation->Load(name) ? Result::Applied : Result::Failed;
}

// The class levels of the "save mana *" command (SaveManaAction.cpp), which also tells the master.
double ClassManaSaveLevel(Player* bot)
{
    switch (bot->getClass())
    {
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
        case CLASS_DRUID:
            return 5.0;
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
            return 2.0;
        default:
            return 3.0;
    }
}

// Sets the mana save level as "save mana *" (enabled) or "save mana" (disabled, 1.0) does, without its reply line.
Result SaveManaOnce(PlayerbotAI* ai, Player* bot, bool enabled)
{
    Value<double>* level = ai->GetAiObjectContext()->GetValue<double>("mana save level");
    if (!level)
        return Result::Failed;

    level->Set(enabled ? ClassManaSaveLevel(bot) : 1.0);
    return Result::Applied;
}

// Makes the target the one focus heal target as the "focus heal <name>" command does (SetFocusHealTargetsAction):
// a player of the bot's group, for a healer or off-healer, with the combat strategy on. No reply line.
Result FocusHealOnce(PlayerbotAI* ai, Player* bot, Group* group, Unit* target, Refusal& why)
{
    if (!target->IsPlayer() || !group->IsMember(target->GetGUID()) ||
        (!PlayerbotAI::IsHeal(bot) && !ai->HasStrategy("offheal", BOT_STATE_COMBAT)))
        return Refuse(why, Refusal::CannotNow);

    Value<std::list<ObjectGuid>>* targets =
        ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("focus heal targets");
    if (!targets)
        return Result::Failed;

    targets->Set({target->GetGUID()});
    ai->ChangeStrategy("+focus heal targets", BOT_STATE_COMBAT);
    return Result::Applied;
}

NamedPosition ReadPosition(PositionMap& positions, std::string const& name)
{
    NamedPosition result;
    PositionMap::iterator const found = positions.find(name);
    if (found == positions.end() || !found->second.isSet())
        return result;

    result.Set = true;
    result.X = found->second.x;
    result.Y = found->second.y;
    result.Z = found->second.z;
    result.MapId = found->second.mapId;
    return result;
}

void WritePosition(PositionMap& positions, std::string const& name, NamedPosition const& position)
{
    if (position.Set)
        positions[name].Set(position.X, position.Y, position.Z, position.MapId);
    else if (positions.find(name) != positions.end())
        positions[name].Reset();
}

// The connected bot's AI, or null for an offline player or one that is no longer a bot. inWorld is false while the
// bot is out of the world, e.g. during a far teleport: it comes back, so that is not a logout.
PlayerbotAI* ConnectedBot(uint64_t guid, bool& inWorld)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    inWorld = bot && bot->IsInWorld();
    return ai && ai->IsBotAI() ? ai : nullptr;
}

bool ReadPart(PlayerbotAI* ai, uint64_t bot, Part part, Baseline& out)
{
    out = Baseline();
    out.Bot = bot;
    out.Held = part;
    switch (part)
    {
        case Part::Movement:
        {
            Value<PositionMap&>* positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position");
            if (!positions)
                return false;

            uint16_t bit = 1;
            for (BotState state : MovementStates)
            {
                for (char const* name : MovementStrategies)
                {
                    if (ai->HasStrategy(name, state))
                        out.Strategies |= bit;

                    bit <<= 1;
                }
            }

            out.Stay = ReadPosition(positions->Get(), "stay");
            out.Return = ReadPosition(positions->Get(), "return");
            return true;
        }
        case Part::Formation:
        {
            UntypedValue* formation = ai->GetAiObjectContext()->GetUntypedValue("formation");
            if (!formation)
                return false;

            out.Formation = formation->Save();
            return true;
        }
        case Part::ManaSave:
        {
            Value<double>* level = ai->GetAiObjectContext()->GetValue<double>("mana save level");
            if (!level)
                return false;

            out.ManaSaveLevel = level->Get();
            return true;
        }
        case Part::FocusHeal:
        {
            Value<std::list<ObjectGuid>>* targets =
                ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("focus heal targets");
            if (!targets)
                return false;

            for (ObjectGuid const& target : targets->Get())
                out.FocusHealTargets.push_back(target.GetRawValue());

            out.FocusHealStrategy = ai->HasStrategy("focus heal targets", BOT_STATE_COMBAT);
            return true;
        }
        default:
            return false;
    }
}

// Writes target's part over current, which ReadPart just filled: only the strategies and values that differ.
bool WritePart(PlayerbotAI* ai, Baseline const& current, Baseline const& target)
{
    switch (target.Held)
    {
        case Part::Movement:
        {
            Value<PositionMap&>* positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position");
            if (!positions)
                return false;

            uint16_t bit = 1;
            for (BotState state : MovementStates)
            {
                std::string changes;
                for (char const* name : MovementStrategies)
                {
                    if ((current.Strategies & bit) != (target.Strategies & bit))
                    {
                        changes += changes.empty() ? "" : ",";
                        changes += (target.Strategies & bit) ? "+" : "-";
                        changes += name;
                    }

                    bit <<= 1;
                }

                if (!changes.empty())
                    ai->ChangeStrategy(changes, state);
            }

            if (current.Stay != target.Stay)
                WritePosition(positions->Get(), "stay", target.Stay);

            if (current.Return != target.Return)
                WritePosition(positions->Get(), "return", target.Return);

            return true;
        }
        case Part::Formation:
        {
            UntypedValue* formation = ai->GetAiObjectContext()->GetUntypedValue("formation");
            return formation && formation->Load(target.Formation);
        }
        case Part::ManaSave:
        {
            Value<double>* level = ai->GetAiObjectContext()->GetValue<double>("mana save level");
            if (!level)
                return false;

            level->Set(target.ManaSaveLevel);
            return true;
        }
        case Part::FocusHeal:
        {
            Value<std::list<ObjectGuid>>* targets =
                ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("focus heal targets");
            if (!targets)
                return false;

            std::list<ObjectGuid> list;
            for (uint64_t guid : target.FocusHealTargets)
                list.push_back(ObjectGuid(guid));

            targets->Set(list);
            if (current.FocusHealStrategy != target.FocusHealStrategy)
                ai->ChangeStrategy(target.FocusHealStrategy ? "+focus heal targets" : "-focus heal targets",
                                   BOT_STATE_COMBAT);

            return true;
        }
        default:
            return false;
    }
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
Result PositionOnce(PostMapJoin const& phase, PlayerbotAI* ai, Order const& order, Refusal& why)
{
    if (!PositionReachable(phase, order))
        return Refuse(why, Refusal::Unreachable);

    MoveToPositionAction move(ai, "move to position", "bridge");
    return move.ExecuteExactPosition(order.MapId, static_cast<float>(order.X), static_cast<float>(order.Y),
                                     static_cast<float>(order.Z))
               ? Result::Applied
               : Result::Failed;
}

// Execute's body below it; it sets why only for a Rejected result.
Result ExecuteOrder(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize, Refusal& why);
}  // namespace

bool PositionReachable(PostMapJoin const& phase, Order const& order)
{
    if (!phase.Current() || !FinitePosition(order) || !std::isfinite(order.VisibilityDistance) ||
        order.VisibilityDistance <= 0.0f)
        return false;

    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Bot));
    if (!bot || !bot->IsInWorld() || bot->GetMapId() != order.MapId || !Near(bot, order))
        return false;

    // Stricter than MoveTo's own search: a complete navmesh path only, never a partial path or a shortcut.
    PathGenerator path(bot);
    path.CalculatePath(static_cast<float>(order.X), static_cast<float>(order.Y), static_cast<float>(order.Z), false);
    return path.GetPathType() == PATHFIND_NORMAL && path.getPathLength() <= 2.0f * order.VisibilityDistance;
}

Result Execute(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize, Refusal* why)
{
    Refusal reason = Refusal::None;
    Result const result = ExecuteOrder(phase, order, authorize, reason);
    if (why)
        *why = result == Result::Rejected ? reason : Refusal::None;

    return result;
}

namespace
{
Result ExecuteOrder(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize, Refusal& why)
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
        case Intent::FocusHealing:
        case Intent::SaveMana:
        case Intent::Formation:
            break;
        // No safe playerbots effect: pull and regroup (DS.9), no role setter, and dungeon clear is another module.
        case Intent::Regroup:
        case Intent::Pull:
        case Intent::AssignRole:
        case Intent::DungeonClear:
            return Result::Unavailable;
        default:
            return Result::Rejected;
    }

    // A stray target, shape or enable flag on another verb is a mapping error, never an ignored field.
    if (!order.Bot || !order.Controller || order.Bot == order.Controller ||
        NeedsTarget(order.Operation) != (order.Target != 0) ||
        (order.Operation == Intent::Formation) != (FormationName(order.Shape) != nullptr) ||
        (order.Enabled && order.Operation != Intent::SaveMana) || !std::isfinite(order.VisibilityDistance) ||
        order.VisibilityDistance <= 0.0f || (order.Operation == Intent::Mark && order.Icon > 7) ||
        (order.Operation == Intent::Position && !FinitePosition(order)) || !authorize || !authorize() ||
        !phase.Current())
        return Result::Rejected;

    // Facts are read only now, after the caller's last check. A changed group means a stale scope: no reason to give.
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Bot));
    Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Controller));
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    Group* group = bot ? bot->GetGroup() : nullptr;
    ObjectGuid const groupGuid = group ? group->GetGUID() : ObjectGuid();
    if (!bot || !bot->IsInWorld() || !ai || !ai->IsBotAI() || !PartnerAllowed(partner) ||
        bot->GetTeamId() != partner->GetTeamId() || groupGuid != ObjectGuid(order.Group))
        return Result::Rejected;

    // Battlegrounds and arenas stay playerbots' own (DP.1); a flight path or death leaves the bot nothing to do.
    if (bot->InBattleground())
        return Refuse(why, Refusal::Battleground);

    if (bot->IsInFlight())
        return Refuse(why, Refusal::Travelling);

    if (!bot->IsAlive())
        return Refuse(why, Refusal::Dead);

    // Following and the group settings are group play: the partner shares the bot's group. Stay, flee, a position
    // and an attack need no group; a mark and focus healing need the bot's own group.
    bool const partnerInGroup = group && partner->GetGroup() == group && group->IsMember(partner->GetGUID());
    switch (order.Operation)
    {
        case Intent::Follow:
            if (!partnerInGroup)
                return Refuse(why, Refusal::NotWithYou);

            if (!bot->IsInMap(partner))
                return Refuse(why, Refusal::TooFar);

            return FollowOnce(ai, partner);
        case Intent::Stay:
            return StayOnce(ai, bot);
        case Intent::Flee:
            return FleeOnce(ai, bot, why);
        case Intent::Position:
            return PositionOnce(phase, ai, order, why);
        case Intent::Formation:
            return partnerInGroup ? FormationOnce(ai, order) : Refuse(why, Refusal::NotInGroup);
        case Intent::SaveMana:
            return partnerInGroup ? SaveManaOnce(ai, bot, order.Enabled) : Refuse(why, Refusal::NotInGroup);
        default:
            break;
    }

    if (order.Operation != Intent::AttackOwnTarget && !group)
        return Refuse(why, Refusal::NotInGroup);

    // The bot acts on what it sees itself; whether the partner sees the target plays no part.
    Unit* target = ObjectAccessor::GetUnit(*bot, ObjectGuid(order.Target));
    if (!Visible(bot, target, order.VisibilityDistance))
        return Refuse(why, Refusal::TargetNotVisible);

    if (order.Operation == Intent::FocusHealing)
        return FocusHealOnce(ai, bot, group, target, why);

    if (order.Operation == Intent::Mark)
    {
        // The bot marks as itself (as RtiAction does): in a raid only as its leader or an assistant, and never a player
        // who is not connected or is hostile to the bot.
        if ((group->isRaidGroup() && !group->IsLeader(bot->GetGUID()) && !group->IsAssistant(bot->GetGUID())) ||
            (target->IsPlayer() &&
             (ObjectAccessor::FindConnectedPlayer(target->GetGUID()) != target || target->IsHostileTo(bot))))
            return Refuse(why, Refusal::CannotNow);

        group->SetTargetIcon(order.Icon, bot->GetGUID(), target->GetGUID());
        return group->GetTargetIcon(order.Icon) == target->GetGUID() ? Result::Applied : Result::Failed;
    }

    if (bot->GetTarget() != target->GetGUID())
        return Refuse(why, Refusal::NoTarget);

    if (bot->IsMounted())
        return Refuse(why, Refusal::CannotNow);

    // A bridge attack cannot defer the attack or update the wait strategy's combat-start timer.
    if (ai->HasStrategy("wait for attack", BOT_STATE_COMBAT))
        return Result::Failed;

    AttackAction attack(ai, "attack");
    // Attack can fail after changing selection/AI/movement. Failed is an attempted result, never a retry grant.
    return attack.ExecuteExactTarget(target) ? Result::Applied : Result::Failed;
}
}  // namespace

bool Capture(PostMapJoin const& phase, uint64_t bot, Part part, Baseline& out)
{
    out = Baseline();
    if (!phase.Current())
        return false;

    bool inWorld = false;
    PlayerbotAI* ai = ConnectedBot(bot, inWorld);
    if (!ai || !inWorld || !ReadPart(ai, bot, part, out))
    {
        out = Baseline();
        return false;
    }

    return true;
}

RestoreResult Restore(PostMapJoin const& phase, uint64_t bot, Baseline const& before, Baseline const& applied)
{
    if (!phase.Current() || before.Held == Part::None || before.Held != applied.Held)
        return RestoreResult::Rejected;

    // Another bot's baseline is a caller's mix-up, never a reason to write.
    if (before.Bot != bot || applied.Bot != bot)
        return RestoreResult::Rejected;

    bool inWorld = false;
    PlayerbotAI* ai = ConnectedBot(bot, inWorld);
    if (!ai)
        return RestoreResult::Gone;

    // A teleporting bot is back in the world soon; the caller retries rather than dropping the bridge's state.
    if (!inWorld)
        return RestoreResult::Failed;

    Baseline current;
    if (!ReadPart(ai, bot, before.Held, current))
        return RestoreResult::Failed;

    if (current == before)
        return RestoreResult::Restored;

    // The same rule as a plan ledger's (Baseline::operator==; Movement by its strategies only).
    if (current != applied)
        return RestoreResult::Superseded;

    return WritePart(ai, current, before) ? RestoreResult::Restored : RestoreResult::Failed;
}
}  // namespace PlayerbotBridge
