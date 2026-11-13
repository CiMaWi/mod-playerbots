/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeAbilities.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <list>
#include <string>
#include <vector>

#include "AttackAction.h"
#include "BridgeGoals.h"
#include "BridgeNames.h"
#include "CensusMgr.h"
#include "FollowActions.h"
#include "Group.h"
#include "MovementActions.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionAction.h"
#include "PositionValue.h"
#include "QuestDef.h"
#include "StayActions.h"
#include "TestPlayer.h"
#include "TravelPlaces.h"
#include "WorldSession.h"

namespace PlayerbotBridge
{
// Read once the order is admitted; Target is set only for a row that takes a unit, once the bot sees it. Place and
// QuestId are what the order's place and quest mean (a name looked up), set only for a row that takes them.
struct ActFacts
{
    PostMapJoin const& Phase;
    Order const& Request;
    PlayerbotAI* Ai;
    Player* Bot;
    Player* Partner;
    Group* BotGroup;
    Unit* Target;
    std::string Place;
    uint32_t QuestId;
};

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

using Planner::GoalApply::BridgeAnswer;
using Planner::GoalApply::BridgeOrder;

// The goal verbs in the planner's order (BridgeGoals.h), the same sequence
constexpr BridgeOrder GoalOrderOf(Intent operation)
{
    return static_cast<BridgeOrder>(static_cast<uint8_t>(operation) - static_cast<uint8_t>(Intent::GoTo));
}
static_assert(GoalOrderOf(Intent::GoTo) == BridgeOrder::GoTo && GoalOrderOf(Intent::Quest) == BridgeOrder::Quest &&
              GoalOrderOf(Intent::Errands) == BridgeOrder::Errands &&
              GoalOrderOf(Intent::SetHearth) == BridgeOrder::SetHearth &&
              GoalOrderOf(Intent::GrindHere) == BridgeOrder::GrindHere &&
              GoalOrderOf(Intent::GatherHere) == BridgeOrder::GatherHere &&
              GoalOrderOf(Intent::MeetAt) == BridgeOrder::MeetAt &&
              GoalOrderOf(Intent::PausePlan) == BridgeOrder::Pause &&
              GoalOrderOf(Intent::ResumePlan) == BridgeOrder::Resume);

// The planner checks the bot's scope, the slot's holder and the order's own rule before it changes anything
Result GoalOnce(ActFacts const& act, Refusal& why)
{
    switch (Planner::BridgeGoal(act.Ai, act.Bot, GoalOrderOf(act.Request.Operation), act.Place, act.QuestId))
    {
        case BridgeAnswer::Done:
            return Result::Applied;
        case BridgeAnswer::NotPlanner:
            return Refuse(why, Refusal::NotPlanner);
        case BridgeAnswer::GoalHeld:
            return Refuse(why, Refusal::GoalHeld);
        case BridgeAnswer::UnknownPlace:
            return Refuse(why, Refusal::UnknownPlace);
        case BridgeAnswer::NoRoute:
            return Refuse(why, Refusal::NoRoute);
        case BridgeAnswer::NoSuchQuest:
            return Refuse(why, Refusal::NoSuchQuest);
        case BridgeAnswer::NothingHere:
            return Refuse(why, Refusal::NothingHere);
        case BridgeAnswer::AlreadyThere:
            return Refuse(why, Refusal::AlreadyThere);
        case BridgeAnswer::CannotNow:
            return Refuse(why, Refusal::CannotNow);
        default:
            return Result::Failed;
    }
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

    // The strategies are the follow, as playerbots' follow command answers "Following" without a move. One move now
    // toward the formation location is a nudge; a bot already there needs none. MoveTo refuses while the bot's last
    // move still runs (MovementAction::IsWaitingForLastMove), as after an invite, and the follow is in effect anyway.
    FollowAction follow(ai);
    if (follow.isUseful())
        follow.Execute(Event());

    return Result::Applied;
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

// Following needs the partner near: on the bot's map and in its instance.
Refusal FollowCheck(ActFacts const& act) { return act.Bot->IsInMap(act.Partner) ? Refusal::None : Refusal::TooFar; }

Refusal AttackCheck(ActFacts const& act)
{
    // The bot's own target: its selection, or the unit it fights while the selection is elsewhere.
    if (act.Bot->GetTarget() != act.Target->GetGUID() && act.Bot->GetVictim() != act.Target)
        return Refusal::NoTarget;

    if (act.Bot->IsMounted())
        return Refusal::CannotNow;

    return Refusal::None;
}

Result AttackOnce(ActFacts const& act, Refusal&)
{
    // The bot already fights the unit: the order is in effect (AttackAction would refuse an attack it already runs)
    if (act.Bot->GetVictim() == act.Target)
        return Result::Applied;

    // A bridge attack cannot defer the attack or update the wait strategy's combat-start timer.
    if (act.Ai->HasStrategy("wait for attack", BOT_STATE_COMBAT))
        return Result::Failed;

    AttackAction attack(act.Ai, "attack");
    // Attack can fail after changing selection/AI/movement. Failed is an attempted result, never a retry grant.
    return attack.ExecuteExactTarget(act.Target) ? Result::Applied : Result::Failed;
}

// The bot marks as itself (as RtiAction does): in a raid only as its leader or an assistant, and never a player who
// is not connected or is hostile to the bot.
Refusal MarkCheck(ActFacts const& act)
{
    Group* group = act.BotGroup;
    Player* bot = act.Bot;
    Unit* target = act.Target;
    if ((group->isRaidGroup() && !group->IsLeader(bot->GetGUID()) && !group->IsAssistant(bot->GetGUID())) ||
        (target->IsPlayer() &&
         (ObjectAccessor::FindConnectedPlayer(target->GetGUID()) != target || target->IsHostileTo(bot))))
        return Refusal::CannotNow;

    return Refusal::None;
}

Result MarkOnce(ActFacts const& act, Refusal&)
{
    uint8_t const icon = act.Request.Icon;
    act.BotGroup->SetTargetIcon(icon, act.Bot->GetGUID(), act.Target->GetGUID());
    return act.BotGroup->GetTargetIcon(icon) == act.Target->GetGUID() ? Result::Applied : Result::Failed;
}

bool InGroup(GroupRule rule, ActFacts const& act)
{
    Group* group = act.BotGroup;
    Player* partner = act.Partner;
    switch (rule)
    {
        case GroupRule::BotGroup:
            return group != nullptr;
        case GroupRule::PartnerInGroup:
            return group && partner->GetGroup() == group && group->IsMember(partner->GetGUID());
        default:
            return true;
    }
}

// Choice parameters: the raid icons by index, the formation shapes by FormationShape value minus one.
constexpr char const* const RaidIcons[] = {"star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull"};
constexpr char const* const FormationShapes[] = {"close", "spread", "line"};

constexpr ParamRow AttackParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, "The enemy you have targeted, by name.", {}}};
constexpr ParamRow MarkParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, "The unit to mark, by name.", {}},
    {"icon", ParamKind::Choice, OrderField::Icon, "The raid icon to put on it.", RaidIcons}};
constexpr ParamRow FocusHealParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, "The group member to heal first, by name.", {}}};
constexpr ParamRow SaveManaParams[] = {
    {"enabled", ParamKind::Flag, OrderField::Enabled, "Yes to save mana, no to spend it freely.", {}}};
constexpr ParamRow FormationParams[] = {
    {"shape", ParamKind::Choice, OrderField::Shape, "How the group keeps together.", FormationShapes}};
constexpr ParamRow PositionParams[] = {
    {"place", ParamKind::Place, OrderField::Position, "A spot near you, as x,y,z,map.", {}}};
constexpr ParamRow TripParams[] = {
    {"place", ParamKind::Place, OrderField::PlaceName, "A known place's name, or x,y,z,map.", {}}};
constexpr ParamRow QuestParams[] = {
    {"quest", ParamKind::Quest, OrderField::QuestId, "A quest in your log, by its title.", {}}};

// Each row's refusals besides the common ones, in check order.
constexpr Refusal FollowRefusals[] = {Refusal::NotWithYou, Refusal::TooFar};
constexpr Refusal FleeRefusals[] = {Refusal::CannotNow};
constexpr Refusal AttackRefusals[] = {Refusal::UnknownName, Refusal::TargetNotVisible, Refusal::NoTarget,
                                      Refusal::CannotNow};
constexpr Refusal GroupTargetRefusals[] = {Refusal::NotInGroup, Refusal::UnknownName, Refusal::TargetNotVisible,
                                           Refusal::CannotNow};
constexpr Refusal GroupSettingRefusals[] = {Refusal::NotInGroup};
constexpr Refusal PositionRefusals[] = {Refusal::Unreachable};
constexpr Refusal TripRefusals[] = {Refusal::NotPlanner,   Refusal::GoalHeld,  Refusal::UnknownPlace,
                                    Refusal::AlreadyThere, Refusal::CannotNow, Refusal::NoRoute};
constexpr Refusal QuestRefusals[] = {Refusal::UnknownName, Refusal::NotPlanner, Refusal::GoalHeld,
                                     Refusal::NoSuchQuest};
constexpr Refusal ErrandRefusals[] = {Refusal::NotPlanner, Refusal::GoalHeld, Refusal::NothingHere};
constexpr Refusal HereRefusals[] = {Refusal::NotPlanner, Refusal::GoalHeld, Refusal::CannotNow, Refusal::NothingHere};
constexpr Refusal PlanRefusals[] = {Refusal::NotPlanner, Refusal::GoalHeld};

constexpr Refusal Common[] = {Refusal::Battleground, Refusal::Travelling, Refusal::Dead};

// The verbs without a row, with their words on the wire.
struct NoEffectVerb
{
    Intent Operation;
    char const* Word;
};
constexpr NoEffectVerb NoEffect[] = {{Intent::Regroup, "regroup"},
                                     {Intent::Pull, "pull"},
                                     {Intent::AssignRole, "assign_role"},
                                     {Intent::DungeonClear, "dungeon_clear"}};

// Indexed by Refusal; the words and sentences the bridge wire already uses.
constexpr RefusalText RefusalTexts[] = {
    {Refusal::None, "none", ""},
    {Refusal::Dead, "dead", "I am in no state to do anything right now."},
    {Refusal::Battleground, "battleground", "Not here; I have my own fight to see to."},
    {Refusal::Travelling, "travelling", "I cannot do that while I am in flight."},
    {Refusal::NotWithYou, "not_with_you", "We are not travelling together."},
    {Refusal::TooFar, "too_far", "You are too far away for me to reach."},
    {Refusal::NotInGroup, "not_in_group", "We are not in the same group."},
    {Refusal::Unreachable, "unreachable", "I cannot get to that spot from here."},
    {Refusal::TargetNotVisible, "target_not_visible", "I cannot see that target from here."},
    {Refusal::NoTarget, "no_target", "I have no target to attack."},
    {Refusal::CannotNow, "cannot_now", "I cannot do that right now."},
    {Refusal::NotPlanner, "not_planner", "My own plans are not mine to change right now."},
    {Refusal::GoalHeld, "goal_held", "My group has a goal I am holding to."},
    {Refusal::UnknownPlace, "unknown_place", "I do not know that place."},
    {Refusal::NoRoute, "no_route", "I cannot find a way there."},
    {Refusal::NoSuchQuest, "no_such_quest", "That quest is not in my log."},
    {Refusal::NothingHere, "nothing_here", "There is nothing for me to do here."},
    {Refusal::AlreadyThere, "already_there", "I am already there."},
    {Refusal::UnknownName, "unknown_name", "I do not know what you mean by that."}};

constexpr bool RefusalTextsInOrder()
{
    for (std::size_t index = 0; index < std::size(RefusalTexts); ++index)
        if (static_cast<std::size_t>(RefusalTexts[index].Code) != index)
            return false;

    return static_cast<std::size_t>(Refusal::UnknownName) + 1 == std::size(RefusalTexts);
}
static_assert(RefusalTextsInOrder(), "one text per Refusal, in the enum's order");

// The rows in the wire's order. A lambda only hands the row's facts to its effect function.
constexpr AbilityRow Rows[] = {
    {.Operation = Intent::Follow,
     .Name = "follow",
     .Sentence = "Follow the person you are talking to; they must be in your group and near you.",
     .Params = {},
     .Group = GroupRule::PartnerInGroup,
     .GroupRefusal = Refusal::NotWithYou,
     .Check = FollowCheck,
     .Refusals = FollowRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return FollowOnce(act.Ai, act.Partner); },
     .Lasting = Part::Movement,
     .Done = Completion::Holds,
     .StatePhrase = "following you"},
    {.Operation = Intent::Stay,
     .Name = "stay",
     .Sentence = "Stop and hold your current spot until told otherwise.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = {},
     .Effect = [](ActFacts const& act, Refusal&) { return StayOnce(act.Ai, act.Bot); },
     .Lasting = Part::Movement,
     .Done = Completion::Holds,
     .StatePhrase = "holding this spot"},
    {.Operation = Intent::Flee,
     .Name = "flee",
     .Sentence = "Back away once from the enemy you are fighting.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = FleeRefusals,
     .Effect = [](ActFacts const& act, Refusal& why) { return FleeOnce(act.Ai, act.Bot, why); },
     .Lasting = Part::None,
     .Done = Completion::Arrived,
     .StatePhrase = "backing away from my enemy"},
    {.Operation = Intent::AttackOwnTarget,
     .Name = "attack_own_target",
     .Sentence = "Attack the enemy you have targeted.",
     .Params = AttackParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = AttackCheck,
     .Refusals = AttackRefusals,
     .Effect = AttackOnce,
     .Lasting = Part::None,
     .Done = Completion::TargetGone,
     .StatePhrase = "attacking {target}"},
    {.Operation = Intent::Mark,
     .Name = "mark",
     .Sentence = "Put a raid icon on a unit you can see; in a raid only as its leader or an assistant.",
     .Params = MarkParams,
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = MarkCheck,
     .Refusals = GroupTargetRefusals,
     .Effect = MarkOnce,
     .Lasting = Part::None,
     .Done = Completion::AtOnce,
     .StatePhrase = "marking {target}"},
    {.Operation = Intent::FocusHealing,
     .Name = "focus_healing",
     .Sentence = "As a healer, heal one member of your group before the others.",
     .Params = FocusHealParams,
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = nullptr,
     .Refusals = GroupTargetRefusals,
     .Effect = [](ActFacts const& act, Refusal& why)
     { return FocusHealOnce(act.Ai, act.Bot, act.BotGroup, act.Target, why); },
     .Lasting = Part::FocusHeal,
     .Done = Completion::Holds,
     .StatePhrase = "healing {target} first"},
    {.Operation = Intent::SaveMana,
     .Name = "save_mana",
     .Sentence = "Save mana in fights, or spend it freely, in the group you share with the speaker.",
     .Params = SaveManaParams,
     .Group = GroupRule::PartnerInGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = nullptr,
     .Refusals = GroupSettingRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return SaveManaOnce(act.Ai, act.Bot, act.Request.Enabled); },
     .Lasting = Part::ManaSave,
     .Done = Completion::Holds,
     .StatePhrase = "minding my mana"},
    {.Operation = Intent::Formation,
     .Name = "formation",
     .Sentence = "Change how you keep to the group you share with the speaker.",
     .Params = FormationParams,
     .Group = GroupRule::PartnerInGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = nullptr,
     .Refusals = GroupSettingRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return FormationOnce(act.Ai, act.Request); },
     .Lasting = Part::Formation,
     .Done = Completion::Holds,
     .StatePhrase = "keeping a {shape} formation"},
    {.Operation = Intent::Position,
     .Name = "position",
     .Sentence = "Walk to a spot near you that you can reach on foot.",
     .Params = PositionParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = PositionRefusals,
     .Effect = [](ActFacts const& act, Refusal& why) { return PositionOnce(act.Phase, act.Ai, act.Request, why); },
     .Lasting = Part::None,
     .Done = Completion::Arrived,
     .StatePhrase = "walking to a spot nearby"},
    {.Operation = Intent::GoTo,
     .Name = "go_to",
     .Sentence = "Travel to a place you know, by your own route.",
     .Params = TripParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = TripRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::GoalEnds,
     .StatePhrase = "travelling to {place}"},
    {.Operation = Intent::Quest,
     .Name = "quest",
     .Sentence = "Work on or turn in a quest from your log.",
     .Params = QuestParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = QuestRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::GoalEnds,
     .StatePhrase = "working on {quest}"},
    {.Operation = Intent::Errands,
     .Name = "errands",
     .Sentence = "Run your errands in a town near you.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = ErrandRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::GoalEnds,
     .StatePhrase = "running my errands"},
    {.Operation = Intent::SetHearth,
     .Name = "set_hearth",
     .Sentence = "Go to the nearest fitting inn and make it your home.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = ErrandRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::GoalEnds,
     .StatePhrase = "going to an inn to set my hearthstone"},
    {.Operation = Intent::GrindHere,
     .Name = "grind_here",
     .Sentence = "Hunt monsters around where you are.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = HereRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::GoalEnds,
     .StatePhrase = "hunting around here"},
    {.Operation = Intent::GatherHere,
     .Name = "gather_here",
     .Sentence = "Gather herbs or ore around where you are.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = HereRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::GoalEnds,
     .StatePhrase = "gathering around here"},
    {.Operation = Intent::MeetAt,
     .Name = "meet_at",
     .Sentence = "Travel to a place to meet the person you are talking to there.",
     .Params = TripParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = TripRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::GoalEnds,
     .StatePhrase = "going to meet you at {place}"},
    {.Operation = Intent::PausePlan,
     .Name = "pause_plan",
     .Sentence = "Set your own plans aside and wait here, for ten minutes at most.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = PlanRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::Holds,
     .StatePhrase = "waiting here for now"},
    {.Operation = Intent::ResumePlan,
     .Name = "resume_plan",
     .Sentence = "Go back to your own plans.",
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = PlanRefusals,
     .Effect = GoalOnce,
     .Lasting = Part::None,
     .Done = Completion::AtOnce,
     .StatePhrase = "back to my own plans"}};

static_assert(std::size(Rows) + std::size(NoEffect) == IntentCount, "every Intent has a row or a word without one");

// Execute's body below it: one outcome per step that ran after the admission.
Result ExecuteSteps(PostMapJoin const& phase, std::span<Order const> steps, std::function<bool()> const& authorize,
                    std::vector<StepOutcome>& outcomes);
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
    std::vector<StepOutcome> outcomes;
    Result const result = ExecuteSteps(phase, std::span<Order const>(&order, 1), authorize, outcomes);
    if (why)
        *why = result == Result::Rejected && !outcomes.empty() ? outcomes.back().Why : Refusal::None;

    return result;
}

Result Execute(PostMapJoin const& phase, std::span<Order const> steps, std::function<bool()> const& authorize,
               std::vector<StepOutcome>* outcomes)
{
    std::vector<StepOutcome> ran;
    Result const result = ExecuteSteps(phase, steps, authorize, ran);
    if (outcomes)
        *outcomes = std::move(ran);

    return result;
}

AbilityRow const* FindRow(Intent operation)
{
    for (AbilityRow const& row : Rows)
        if (row.Operation == operation)
            return &row;

    return nullptr;
}

AbilityRow const* FindRow(std::string_view name)
{
    for (AbilityRow const& row : Rows)
        if (name == row.Name)
            return &row;

    return nullptr;
}

std::span<AbilityRow const> AbilityRows() { return Rows; }

bool IsUnavailable(Intent operation)
{
    return std::any_of(std::begin(NoEffect), std::end(NoEffect),
                       [operation](NoEffectVerb const& verb) { return verb.Operation == operation; });
}

char const* IntentWord(Intent operation)
{
    if (AbilityRow const* row = FindRow(operation))
        return row->Name;

    for (NoEffectVerb const& verb : NoEffect)
        if (verb.Operation == operation)
            return verb.Word;

    return "";
}

std::optional<Intent> FindIntent(std::string_view word)
{
    if (AbilityRow const* row = FindRow(word))
        return row->Operation;

    for (NoEffectVerb const& verb : NoEffect)
        if (word == verb.Word)
            return verb.Operation;

    return std::nullopt;
}

std::span<Refusal const> CommonRefusals() { return Common; }

RefusalText const& RefusalWords(Refusal reason)
{
    std::size_t const index = static_cast<std::size_t>(reason);
    return index < std::size(RefusalTexts) ? RefusalTexts[index] : RefusalTexts[0];
}

bool Takes(AbilityRow const& row, OrderField field)
{
    return std::any_of(row.Params.begin(), row.Params.end(),
                       [field](ParamRow const& param) { return param.Field == field; });
}

Part LastingPart(Intent operation)
{
    AbilityRow const* row = FindRow(operation);
    return row ? row->Lasting : Part::None;
}

namespace
{
constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

constexpr char const* const AbilityKindWords[] = {"act", "topic"};
constexpr char const* const ParamKindWords[] = {"unit", "place", "item", "spell", "quest", "choice", "flag", "count"};
constexpr char const* const PartWords[] = {"none", "movement", "formation", "mana_save", "focus_heal"};
constexpr char const* const CompletionWords[] = {"at_once", "holds", "arrived", "target_gone", "goal_ends"};
static_assert(std::size(AbilityKindWords) == static_cast<std::size_t>(AbilityKind::Topic) + 1);
static_assert(std::size(ParamKindWords) == static_cast<std::size_t>(ParamKind::Count) + 1);
static_assert(std::size(PartWords) == static_cast<std::size_t>(Part::FocusHeal) + 1);
static_assert(std::size(CompletionWords) == static_cast<std::size_t>(Completion::GoalEnds) + 1);

template <std::size_t Size, typename Enum>
char const* WordOf(char const* const (&words)[Size], Enum value)
{
    std::size_t const index = static_cast<std::size_t>(value);
    return index < Size ? words[index] : "";
}

std::vector<ParamData> ParamsOf(std::span<ParamRow const> params)
{
    std::vector<ParamData> data;
    for (ParamRow const& param : params)
        data.push_back({param.Name, param.Kind, param.Sentence, {param.Choices.begin(), param.Choices.end()}});

    return data;
}

RefusalData RefusalOf(Refusal reason)
{
    RefusalText const& text = RefusalWords(reason);
    return {text.Code, text.Word, text.Sentence};
}

void Line(std::string& text, std::initializer_list<std::string_view> fields)
{
    char const* separator = "";
    for (std::string_view field : fields)
    {
        text.append(separator).append(field);
        separator = "\t";
    }
    text.push_back('\n');
}
}  // namespace

std::vector<AbilityData> Catalogue()
{
    std::vector<AbilityData> catalogue;
    catalogue.reserve(std::size(Rows) + std::size(TopicTable));
    for (AbilityRow const& row : Rows)
    {
        AbilityData& entry = catalogue.emplace_back();
        entry.Kind = AbilityKind::Act;
        entry.Name = row.Name;
        entry.Sentence = row.Sentence;
        entry.Params = ParamsOf(row.Params);
        for (Refusal reason : Common)
            entry.Refusals.push_back(RefusalOf(reason));
        for (Refusal reason : row.Refusals)
            entry.Refusals.push_back(RefusalOf(reason));
        entry.Lasting = row.Lasting;
        entry.Done = row.Done;
        entry.StatePhrase = row.StatePhrase;
        entry.Event = ActEventType;
    }
    for (TopicRow const& row : TopicTable)
    {
        AbilityData& entry = catalogue.emplace_back();
        entry.Kind = AbilityKind::Topic;
        entry.Name = row.Name;
        entry.Sentence = row.Sentence;
        entry.Params = ParamsOf(row.Params);
        entry.Limit = row.Limit;
    }
    return catalogue;
}

std::string CatalogueText(std::vector<AbilityData> const& catalogue)
{
    std::string text;
    Line(text, {"catalogue", std::to_string(CatalogueVersion)});
    Line(text, {"steps", std::to_string(MaxSteps)});
    for (AbilityData const& entry : catalogue)
    {
        Line(text, {KindWord(entry.Kind), entry.Name, entry.Sentence});
        for (ParamData const& param : entry.Params)
        {
            std::string choices;
            for (std::string const& choice : param.Choices)
                choices.append(choices.empty() ? "" : ",").append(choice);
            Line(text, {"param", param.Name, KindWord(param.Kind), param.Sentence, choices});
        }
        for (RefusalData const& refusal : entry.Refusals)
            Line(text, {"refusal", refusal.Word, refusal.Sentence});
        Line(text, {"effect", PartWord(entry.Lasting)});
        Line(text, {"done", CompletionWord(entry.Done)});
        Line(text, {"state", entry.StatePhrase});
        Line(text, {"event", entry.Event});
        Line(text, {"limit", std::to_string(entry.Limit)});
    }
    return text;
}

uint64_t CatalogueHash(std::vector<AbilityData> const& catalogue) { return TextHash(CatalogueText(catalogue)); }

uint64_t TextHash(std::string_view text)
{
    uint64_t hash = FnvOffset;
    for (char c : text)
    {
        hash ^= static_cast<uint8_t>(c);
        hash *= FnvPrime;
    }
    return hash;
}

char const* KindWord(AbilityKind kind) { return WordOf(AbilityKindWords, kind); }
char const* KindWord(ParamKind kind) { return WordOf(ParamKindWords, kind); }
char const* PartWord(Part part) { return WordOf(PartWords, part); }
char const* CompletionWord(Completion done) { return WordOf(CompletionWords, done); }

namespace
{
// Where a candidate on another map, or a group member the bot has no unit of, stands for the nearest-name rule.
constexpr float Elsewhere = 1.0e9f;

float DistanceTo(Player* bot, float x, float y, float z)
{
    float const dx = x - bot->GetPositionX();
    float const dy = y - bot->GetPositionY();
    float const dz = z - bot->GetPositionZ();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The units the bot perceives (the nearest NPCs, possible targets and nearest friendly players it keeps as values)
// and its group's members. A unit in two lists is the same id twice. The bot's own living target (the unit it fights,
// then its selection, never the bot) counts as the nearest unit of its name; an exact name still beats a part of one.
uint64_t UnitNamed(ActFacts const& act, std::string const& name)
{
    std::vector<NameCandidate> candidates;
    // The first of equal distance wins, so the unit fought beats the selection.
    for (Unit* own : {act.Bot->GetVictim(), ObjectAccessor::GetUnit(*act.Bot, act.Bot->GetTarget())})
        if (own && own != act.Bot && own->IsAlive())
            candidates.push_back({own->GetName(), own->GetGUID().GetRawValue(), 0.0f});

    for (char const* list : {"nearest npcs", "possible targets", "nearest friendly players"})
        if (Value<GuidVector>* units = act.Ai->GetAiObjectContext()->GetValue<GuidVector>(list))
            for (ObjectGuid const& guid : units->Get())
                if (Unit* unit = ObjectAccessor::GetUnit(*act.Bot, guid))
                    candidates.push_back(
                        {unit->GetName(), guid.GetRawValue(),
                         DistanceTo(act.Bot, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ())});

    if (act.BotGroup)
        for (auto const& slot : act.BotGroup->GetMemberSlots())
        {
            Unit* member = ObjectAccessor::GetUnit(*act.Bot, slot.guid);
            float const distance =
                member ? DistanceTo(act.Bot, member->GetPositionX(), member->GetPositionY(), member->GetPositionZ())
                       : Elsewhere;
            candidates.push_back({slot.name, slot.guid.GetRawValue(), distance});
        }

    NameResult const match = ResolveName(name, candidates);
    return match.Kind == NameMatch::None ? 0 : candidates[match.Index].Id;
}

// The quests in the bot's log, by title, as ReadQuests walks them; the planner still refuses a failed one.
uint32_t QuestNamed(Player* bot, std::string const& name)
{
    std::vector<NameCandidate> candidates;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr)
            candidates.push_back({quest->GetTitle(), questId, 0.0f});
    }

    NameResult const match = ResolveName(name, candidates);
    return match.Kind == NameMatch::None ? 0 : static_cast<uint32_t>(candidates[match.Index].Id);
}

// A literal "x,y,z,map" as given; else the known place the name means (one on another map as elsewhere), under the
// name the planner's place lookup takes. No match: the text as given, so the planner refuses it after its own rules.
std::string PlaceNamed(Player* bot, std::string const& text)
{
    if (TravelPlaces::ParseLiteral(text))
        return text;

    std::vector<NameCandidate> candidates;
    for (TravelPlaces::Place const& place : TravelPlaces::All())
    {
        float const distance =
            place.at.mapId == bot->GetMapId() ? DistanceTo(bot, place.at.x, place.at.y, place.at.z) : Elsewhere;
        candidates.push_back({place.name, candidates.size(), distance});
    }

    NameResult const match = ResolveName(text, candidates);
    return match.Kind == NameMatch::None ? text : candidates[match.Index].Name;
}

// The common rule, then the row's: its group rule, the names it takes looked up, a unit seen by the bot, its check,
// its effect.
Result RunRow(AbilityRow const& row, ActFacts& act, Refusal& why)
{
    Player* bot = act.Bot;
    // Battlegrounds and arenas stay playerbots' own (DP.1); a flight path or death leaves the bot nothing to do.
    if (bot->InBattleground())
        return Refuse(why, Refusal::Battleground);

    if (bot->IsInFlight())
        return Refuse(why, Refusal::Travelling);

    if (!bot->IsAlive())
        return Refuse(why, Refusal::Dead);

    // Following and the group settings are group play: the partner shares the bot's group. Stay, flee, a position,
    // an attack and a goal need no group; a mark and focus healing need the bot's own group.
    if (!InGroup(row.Group, act))
        return Refuse(why, row.GroupRefusal);

    // The bot acts on what it sees itself; whether the partner sees the target plays no part. A unit given by name
    // then passes the same test as one given by id.
    if (Takes(row, OrderField::Target))
    {
        uint64_t const target =
            act.Request.TargetName.empty() ? act.Request.Target : UnitNamed(act, act.Request.TargetName);
        if (!target)
            return Refuse(why, Refusal::UnknownName);

        act.Target = ObjectAccessor::GetUnit(*bot, ObjectGuid(target));
        if (!Visible(bot, act.Target, act.Request.VisibilityDistance))
            return Refuse(why, Refusal::TargetNotVisible);
    }

    if (Takes(row, OrderField::QuestId))
    {
        act.QuestId = act.Request.QuestName.empty() ? act.Request.QuestId : QuestNamed(bot, act.Request.QuestName);
        if (!act.QuestId)
            return Refuse(why, Refusal::UnknownName);
    }

    if (Takes(row, OrderField::PlaceName))
        act.Place = PlaceNamed(bot, act.Request.PlaceName);

    Refusal const reason = row.Check ? row.Check(act) : Refusal::None;
    if (reason != Refusal::None)
        return Refuse(why, reason);

    return row.Effect(act, why);
}

// How many of a parameter's two forms, id and name, an order fills: one for a row that takes it, else none.
int Forms(bool id, std::string const& name) { return (id ? 1 : 0) + (name.empty() ? 0 : 1); }

// A stray target, shape, enable flag, place or quest on another verb is a mapping error, never an ignored field; a
// unit or quest comes as an id or a name, never both.
bool Malformed(AbilityRow const* row, Order const& order)
{
    return !order.Bot || !order.Controller || order.Bot == order.Controller ||
           Forms(order.Target != 0, order.TargetName) != (Takes(*row, OrderField::Target) ? 1 : 0) ||
           order.TargetName.size() > MaxName || Takes(*row, OrderField::PlaceName) != !order.PlaceName.empty() ||
           order.PlaceName.size() > MaxPlaceName ||
           Forms(order.QuestId != 0, order.QuestName) != (Takes(*row, OrderField::QuestId) ? 1 : 0) ||
           order.QuestName.size() > MaxName ||
           Takes(*row, OrderField::Shape) != (FormationName(order.Shape) != nullptr) ||
           (order.Enabled && !Takes(*row, OrderField::Enabled)) || !std::isfinite(order.VisibilityDistance) ||
           order.VisibilityDistance <= 0.0f ||
           (Takes(*row, OrderField::Icon) && static_cast<std::size_t>(order.Icon) >= std::size(RaidIcons)) ||
           (Takes(*row, OrderField::Position) && !FinitePosition(order));
}

// One admitted step: its facts, its row, its event.
Result RunStep(PostMapJoin const& phase, AbilityRow const& row, Order const& order, Refusal& why)
{
    if (!phase.Current())
        return Result::Rejected;

    // Facts are read only now, after the caller's last check, and again for every step. A changed group means a stale
    // scope: no reason to give.
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Bot));
    Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(order.Controller));
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    Group* group = bot ? bot->GetGroup() : nullptr;
    ObjectGuid const groupGuid = group ? group->GetGUID() : ObjectGuid();
    if (!bot || !bot->IsInWorld() || !ai || !ai->IsBotAI() || !PartnerAllowed(partner) ||
        bot->GetTeamId() != partner->GetTeamId() || groupGuid != ObjectGuid(order.Group))
        return Result::Rejected;

    ActFacts act{phase, order, ai, bot, partner, group, nullptr, {}, 0};
    Result const result = RunRow(row, act, why);
    // One event for each step the facts admit: applied, failed or refused (id: the refusal, else 0).
    EventLog::Emit(bot, ActEventType, row.Name, static_cast<uint32>(result == Result::Rejected ? why : Refusal::None),
                   static_cast<int64>(result));
    return result;
}

Result ExecuteSteps(PostMapJoin const& phase, std::span<Order const> steps, std::function<bool()> const& authorize,
                    std::vector<StepOutcome>& outcomes)
{
    outcomes.clear();
    if (!phase.Current() || steps.empty() || steps.size() > MaxSteps)
        return Result::Rejected;

    // Every step's shape before the one admission, so a malformed later step stops the order before the first acts;
    // all steps are one bot's, for one partner, in one scope.
    std::array<AbilityRow const*, MaxSteps> rows{};
    Order const& first = steps.front();
    for (std::size_t index = 0; index < steps.size(); ++index)
    {
        Order const& order = steps[index];
        // No row: no safe playerbots effect (pull and regroup, DS.9; no role setter; dungeon clear is another
        // module), or no verb at all.
        AbilityRow const* row = FindRow(order.Operation);
        if (!row)
            return IsUnavailable(order.Operation) ? Result::Unavailable : Result::Rejected;

        if (order.Bot != first.Bot || order.Controller != first.Controller || order.Group != first.Group ||
            order.VisibilityDistance != first.VisibilityDistance || Malformed(row, order))
            return Result::Rejected;

        rows[index] = row;
    }

    if (!authorize || !authorize() || !phase.Current())
        return Result::Rejected;

    // In order; a step that is not applied ends the order, and the later ones never run.
    for (std::size_t index = 0; index < steps.size(); ++index)
    {
        Refusal why = Refusal::None;
        Result const result = RunStep(phase, *rows[index], steps[index], why);
        outcomes.push_back({result, result == Result::Rejected ? why : Refusal::None});
        if (result != Result::Applied)
            return result;
    }
    return Result::Applied;
}
}  // namespace
}  // namespace PlayerbotBridge
