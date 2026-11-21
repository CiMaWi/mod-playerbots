/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeAbilities.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <list>
#include <string>
#include <vector>

#include "AcceptInvitationAction.h"
#include "AcceptQuestAction.h"
#include "AttackAction.h"
#include "BridgeGoals.h"
#include "BridgeItemRule.h"
#include "BridgeNames.h"
#include "BridgePerception.h"
#include "BridgeQuiet.h"
#include "BridgeSpellRule.h"
#include "BridgeTexts.h"
#include "CensusMgr.h"
#include "ChatShortcutActions.h"
#include "EmoteAction.h"
#include "Event.h"
#include "Formations.h"
#include "Group.h"
#include "InviteToGroupAction.h"
#include "ItemUse.h"
#include "Log.h"
#include "MovementActions.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionAction.h"
#include "PositionValue.h"
#include "PullActions.h"
#include "PullStrategy.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "RandomPlayerbotMgr.h"
#include "ReadyCheckAction.h"
#include "SaveManaAction.h"
#include "SetFocusHealTargetsAction.h"
#include "ShareQuestAction.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TestConstraints.h"
#include "TestPlayer.h"
#include "TravelPlaces.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace PlayerbotBridge
{
// Read once the order is admitted; Target is set only for a row that takes a unit, once the bot sees it. Place,
// QuestId, ItemId and SpellId are what the order's place, quest, item and spell mean (a name looked up), set only for
// a row that takes them. A row reads a choice and a count from Request.
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
    uint32_t ItemId = 0;
    uint32_t SpellId = 0;
};

namespace
{
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

// Runs playerbots' own command action as the matching chat command does (LC.35), with the partner as the requester;
// its reply lines stay unsaid under RunRow's QuietScope, as the bridge reply speaks.
Result RunCommand(Action& action, char const* command, std::string const& param, Player* partner)
{
    return action.Execute(Event(command, param, partner)) ? Result::Applied : Result::Failed;
}

// The "follow" command (FollowChatShortcutAction). Execute admitted only a partner of the bot's group outside
// battlegrounds, so a masterless bot here is grouped and takes the partner as master, as PlayerbotAI::FindNewMaster
// picks a player of its group. A bot with a master keeps it and follows that master, so Applied means "follows the
// partner" only because the user is the only player (DS.12) and, outside battlegrounds, playerbots gives a bot a real
// player as master; with several players this would need the partner as master. An ungrouped bot never reaches this.
Result FollowOnce(PlayerbotAI* ai, Player* partner)
{
    if (!ai->GetMaster())
    {
        ai->SetMaster(partner);
        ai->ResetStrategies();
    }

    FollowChatShortcutAction follow(ai);
    return RunCommand(follow, "follow", "", partner);
}

// The "stay" command (StayChatShortcutAction); a masterless bot stays for its requester.
Result StayOnce(PlayerbotAI* ai, Player* partner)
{
    StayChatShortcutAction stay(ai);
    return RunCommand(stay, "stay", "", partner);
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

// The "formation <name>" command (SetFormationAction).
Result FormationOnce(PlayerbotAI* ai, Player* partner, Order const& order)
{
    char const* name = FormationName(order.Shape);
    if (!name)
        return Result::Rejected;

    SetFormationAction formation(ai);
    return RunCommand(formation, "formation", name, partner);
}

// The "save mana *" command (enabled: the class's level) or "save mana" (disabled) (SaveManaAction).
Result SaveManaOnce(PlayerbotAI* ai, Player* partner, bool enabled)
{
    SaveManaAction save(ai);
    return RunCommand(save, "save mana", enabled ? "*" : "", partner);
}

// The "focus heal +<name>" command (SetFocusHealTargetsAction) for a player of the bot's group, by a healer or
// off-healer; another target is a refusal here, not the command's reply line.
Result FocusHealOnce(ActFacts const& act, Refusal& why)
{
    Unit* target = act.Target;
    if (!target->IsPlayer() || !act.BotGroup->IsMember(target->GetGUID()) ||
        (!PlayerbotAI::IsHeal(act.Bot) && !act.Ai->HasStrategy("offheal", BOT_STATE_COMBAT)))
        return Refuse(why, Refusal::CannotNow);

    SetFocusHealTargetsAction focus(act.Ai);
    return RunCommand(focus, "focus heal", "+" + target->GetName(), act.Partner);
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

// LC.29: a unit the bot may fight on an order is a creature, never a player or a player's pet (also for an order
// that gives the unit by id, past the name filter), and one the core lets it attack.
bool MayAttack(ActFacts const& act)
{
    return !act.Target->GetCharmerOrOwnerPlayerOrPlayerItself() && act.Bot->IsValidAttackTarget(act.Target);
}

// An attack by name: any enemy creature the bot sees, not only its own target.
Refusal AttackByNameCheck(ActFacts const& act)
{
    return act.Bot->IsMounted() || !MayAttack(act) ? Refusal::CannotNow : Refusal::None;
}

// LC17a: a pull stays with the group: one creature within this many yards of the bot (a group member), in its sight.
constexpr float PullBound = 20.0f;

// A tank pulls with its pull strategy (playerbots' pull request: it draws the creature and comes back to where it
// stood); any other bot pulls with a ranged attack, its first strike.
PullStrategy* TankPull(ActFacts const& act)
{
    return PlayerbotAI::IsTank(act.Bot) ? PullStrategy::Get(act.Ai) : nullptr;
}

Refusal PullCheck(ActFacts const& act)
{
    if (act.Bot->IsMounted())
        return Refusal::CannotNow;

    if (!act.Bot->IsWithinDistInMap(act.Target, PullBound) || !act.Bot->IsWithinLOSInMap(act.Target))
        return Refusal::TooFar;

    PullStrategy* strategy = TankPull(act);
    bool const able = strategy ? strategy->CanDoPullAction(act.Target) : PlayerbotAI::IsRanged(act.Bot);
    return able && MayAttack(act) ? Refusal::None : Refusal::CannotNow;
}

Result PullOnce(ActFacts const& act, Refusal& why)
{
    if (PullStrategy* strategy = TankPull(act))
    {
        PullRequestAction::Request(act.Ai, strategy, act.Target);
        return Result::Applied;
    }

    return AttackOnce(act, why);
}

// LC17b: the spell rule reads the core's effect values by number (BridgeSpellRule.h).
static_assert(SpellEffectId::Instakill == SPELL_EFFECT_INSTAKILL &&
                  SpellEffectId::SchoolDamage == SPELL_EFFECT_SCHOOL_DAMAGE &&
                  SpellEffectId::PortalTeleport == SPELL_EFFECT_PORTAL_TELEPORT &&
                  SpellEffectId::TeleportUnits == SPELL_EFFECT_TELEPORT_UNITS &&
                  SpellEffectId::EnvironmentalDamage == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE &&
                  SpellEffectId::PowerDrain == SPELL_EFFECT_POWER_DRAIN &&
                  SpellEffectId::HealthLeech == SPELL_EFFECT_HEALTH_LEECH &&
                  SpellEffectId::WeaponDamageNoSchool == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL &&
                  SpellEffectId::Leap == SPELL_EFFECT_LEAP &&
                  SpellEffectId::WeaponPercentDamage == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE &&
                  SpellEffectId::Jump == SPELL_EFFECT_JUMP && SpellEffectId::JumpDest == SPELL_EFFECT_JUMP_DEST &&
                  SpellEffectId::TeleportUnitsFaceCaster == SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER &&
                  SpellEffectId::TradeSkill == SPELL_EFFECT_TRADE_SKILL &&
                  SpellEffectId::WeaponDamage == SPELL_EFFECT_WEAPON_DAMAGE &&
                  SpellEffectId::PowerBurn == SPELL_EFFECT_POWER_BURN &&
                  SpellEffectId::InterruptCast == SPELL_EFFECT_INTERRUPT_CAST &&
                  SpellEffectId::Stuck == SPELL_EFFECT_STUCK && SpellEffectId::Charge == SPELL_EFFECT_CHARGE &&
                  SpellEffectId::KnockBack == SPELL_EFFECT_KNOCK_BACK &&
                  SpellEffectId::AttackMe == SPELL_EFFECT_ATTACK_ME &&
                  SpellEffectId::TeleportGraveyard == SPELL_EFFECT_TELEPORT_GRAVEYARD &&
                  SpellEffectId::NormalizedWeaponDamage == SPELL_EFFECT_NORMALIZED_WEAPON_DMG &&
                  SpellEffectId::PullTowards == SPELL_EFFECT_PULL_TOWARDS &&
                  SpellEffectId::LeapBack == SPELL_EFFECT_LEAP_BACK &&
                  SpellEffectId::KnockBackDest == SPELL_EFFECT_KNOCK_BACK_DEST &&
                  SpellEffectId::PullTowardsDest == SPELL_EFFECT_PULL_TOWARDS_DEST &&
                  SpellEffectId::ChargeDest == SPELL_EFFECT_CHARGE_DEST &&
                  TradeskillAttribute == SPELL_ATTR0_IS_TRADESKILL && MAX_SPELL_EFFECTS == 3,
              "the spell rule's effect values are SharedDefines.h's");

// A spell's facts for the spell rule: its SpellInfo, and whether a profession's skill line teaches it (the spell
// manager's lists, no database query).
SpellFacts FactsOf(SpellInfo const* spell)
{
    SpellFacts facts{spell->IsPassive(), spell->IsPositive(), false, spell->Attributes, {}};
    for (uint8 index = 0; index < MAX_SPELL_EFFECTS; ++index)
        facts.Effects[index] = static_cast<uint32_t>(spell->Effects[index].Effect);

    SkillLineAbilityMapBounds const skills = sSpellMgr->GetSkillLineAbilityMapBounds(spell->Id);
    for (auto skill = skills.first; skill != skills.second; ++skill)
        facts.Profession = facts.Profession || IsProfessionSkill(skill->second->SkillLine);

    return facts;
}

// Whom a spell lands on: the named friend for a spell cast on a unit (or a friend's body), else the bot (a conjure,
// a portal, an aura of its own): the effect falls where the bot stands whoever was named.
Unit* CastTarget(ActFacts const& act, SpellInfo const* spell)
{
    return spell->GetExplicitTargetMask() & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_CORPSE_MASK) ? act.Target : act.Bot;
}

// The power and reagents the cast costs: CanCastSpell checks the cast without them.
bool CanPay(Player* bot, SpellInfo const* spell)
{
    if (spell->PowerType < MAX_POWERS && static_cast<int64>(bot->GetPower(static_cast<Powers>(spell->PowerType))) <
                                             spell->CalcPowerCost(bot, spell->GetSchoolMask()))
        return false;

    if (bot->CanNoReagentCast(spell))
        return true;

    for (uint8 index = 0; index < MAX_SPELL_REAGENTS; ++index)
        if (spell->Reagent[index] > 0 && !bot->HasItemCount(spell->Reagent[index], spell->ReagentCount[index]))
            return false;

    return true;
}

// A helpful spell (BridgeSpellRule.h) on a friend or the bot itself (also for a unit given by id), within the spell's
// range, off cooldown, paid for.
Refusal CastCheck(ActFacts const& act)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(act.SpellId);
    if (!spell || !HelpfulSpell(FactsOf(spell)) || (act.Target != act.Bot && !act.Bot->IsFriendlyTo(act.Target)))
        return Refusal::CannotNow;

    Unit* target = CastTarget(act, spell);
    if (target != act.Bot && !act.Bot->IsWithinDistInMap(target, spell->GetMaxRange(true, act.Bot)))
        return Refusal::TooFar;

    return act.Bot->HasSpellCooldown(spell->Id) || !CanPay(act.Bot, spell) ? Refusal::CannotNow : Refusal::None;
}

// One cast through playerbots' own path: dismounted first, standing up and still (as CastCustomSpellAction; a seated
// bot's CastSpell would only stand it up), then its CanCastSpell check (the core's cast check: a dead friend for a
// resurrection, a target it may cast on, the global cooldown: an order that meets the bot's own cast is refused, the
// speaker may ask again) and CastSpell. A refusal or a failure there writes one debug line of why.
Result CastOnce(ActFacts const& act, Refusal& why)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(act.SpellId);
    Unit* target = CastTarget(act, spell);
    Player* bot = act.Bot;
    if (bot->IsMounted() && !bot->IsFlying() && !spell->HasAttribute(SPELL_ATTR0_ALLOW_WHILE_MOUNTED))
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);

    bool const seated = !bot->IsStandState();
    if (seated)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    if (bot->isMoving())
        bot->StopMoving();

    bool const checked = act.Ai->CanCastSpell(spell->Id, target, true);
    // The ordered cast is no cast of the bot's own AI: a test's nocast bit is off for this call
    uint32 const heldConstraints = act.Ai->testConstraints;
    act.Ai->testConstraints = heldConstraints & ~TestConstraints::NOCAST;
    bool const cast = checked && act.Ai->CastSpell(spell->Id, target);
    act.Ai->testConstraints = heldConstraints;
    if (cast)
        return Result::Applied;

    LOG_DEBUG("playerbots", "Bridge cast {} by {} on {}: {} (prepare {}, seated {}, casting {}, gcd {})", spell->Id,
              bot->GetName(), target->GetName(), checked ? "failed" : "refused", act.Ai->LastPrepareResult(), seated,
              bot->IsNonMeleeSpellCast(false), bot->GetGlobalCooldownMgr().HasGlobalCooldown(spell));
    return checked ? Result::Failed : Refuse(why, Refusal::CannotNow);
}

// WI10a: the item the player named, used on the bot itself or on the named unit through the item-use framework
// (ItemUse::Use, caller ability; C10: the game's own handler, never a hand-made effect). The bridge runs on the world
// thread after the map threads joined, so the call is synchronous and its cause is the refusal, as CastOnce asks the
// core's cast check directly. There is no separate Check: Use checks first and writes the ItemUse row with the exact
// cause (ability:refused:<cause>), which a read-only check would not. The wire's Refusal set is closed, so the cause
// collapses onto it (BridgeItemRule.h).
Result UseItemOnce(ActFacts const& act, Refusal& why)
{
    Player* bot = act.Bot;
    Item* item = bot->GetItemByEntry(act.ItemId);
    if (!item)
        return Refuse(why, Refusal::UnknownName);

    ItemUse::Request const request = act.Target == bot ? ItemUse::OnSelf(item, ItemUse::Caller::Ability)
                                                       : ItemUse::OnUnit(item, act.Target, ItemUse::Caller::Ability);
    ItemUse::UseResult const result = ItemUse::Use(bot, request);
    if (result.outcome != ItemUse::Outcome::Refused)
        return Result::Applied;

    switch (RefusalOfCause(ItemUse::CauseWord(result.cause)))
    {
        case ItemRefusal::UnknownName:
            return Refuse(why, Refusal::UnknownName);
        case ItemRefusal::TooFar:
            return Refuse(why, Refusal::TooFar);
        case ItemRefusal::TargetNotVisible:
            return Refuse(why, Refusal::TargetNotVisible);
        case ItemRefusal::Dead:
            return Refuse(why, Refusal::Dead);
        default:
            return Refuse(why, Refusal::CannotNow);
    }
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

// The gestures: the word on the wire (BridgeTexts::Acts::EMOTE_WORDS) and its text emote, in the same order.
constexpr TextEmotes EmoteIds[] = {TEXT_EMOTE_WAVE,  TEXT_EMOTE_BOW,     TEXT_EMOTE_CHEER,   TEXT_EMOTE_LAUGH,
                                   TEXT_EMOTE_DANCE, TEXT_EMOTE_SALUTE,  TEXT_EMOTE_KNEEL,   TEXT_EMOTE_POINT,
                                   TEXT_EMOTE_CRY,   TEXT_EMOTE_SHY,     TEXT_EMOTE_THANK,   TEXT_EMOTE_HELLO,
                                   TEXT_EMOTE_BYE,   TEXT_EMOTE_NOD,     TEXT_EMOTE_NO,      TEXT_EMOTE_YAWN,
                                   TEXT_EMOTE_SIGH,  TEXT_EMOTE_WHISTLE, TEXT_EMOTE_APPLAUD, TEXT_EMOTE_CONGRATULATE};
static_assert(std::size(BridgeTexts::Acts::EMOTE_WORDS) == std::size(EmoteIds), "one text emote per gesture word");

// The "emote" action's own text emote (EmoteActionBase::Emote): it faces its target and names it.
class Gesture : public EmoteActionBase
{
public:
    explicit Gesture(PlayerbotAI* ai) : EmoteActionBase(ai, "emote") {}

    bool At(Unit* target, TextEmotes emote) { return Emote(target, emote, true); }
};

// The gesture at the partner when the bot sees it; out of sight it names nobody.
Result EmoteOnce(ActFacts const& act)
{
    uint8_t const choice = act.Request.Choice;
    if (choice >= std::size(EmoteIds))
        return Result::Failed;

    Gesture gesture(act.Ai);
    Unit* target = Perceives(act.Bot, act.Partner) ? act.Partner : nullptr;
    return gesture.At(target, EmoteIds[choice]) ? Result::Applied : Result::Failed;
}

constexpr uint32 RollMinimum = 1;
constexpr uint32 RollMaximum = 100;

// The "roll" command without an item (RollAction) is this roll, but answers false. The group sees it, else the bot.
Result RollOnce(ActFacts const& act)
{
    act.Bot->DoRandomRoll(RollMinimum, RollMaximum);
    return Result::Applied;
}

// A ready check is a group's: an ungrouped bot has none to answer. The core keeps no open check, so a grouped bot
// answers as the "ready" command does, open check or not.
Refusal ReadyRowCheck(ActFacts const& act) { return act.BotGroup ? Refusal::None : Refusal::CannotNow; }

// The "ready" command (ReadyCheckAction without a request packet): the answer "ready" at once, the one path that also
// answers a master's check.
Result ReadyOnce(ActFacts const& act)
{
    ReadyCheckAction ready(act.Ai);
    return RunCommand(ready, "ready", "", act.Partner);
}

// As LeaveGroupAction::Leave without its "Goodbye!" line. The bot's session queues the leave (CMSG_GROUP_DISBAND, a
// client's leave: a leader's group passes on), so the bot is out of the group at its next session update.
Result LeaveOnce(ActFacts const& act)
{
    act.Ai->LeaveOrDisbandGroup();
    return Result::Applied;
}

// The group a player may invite into: its leader or a raid assistant does.
bool MayInvite(Group* group, Player* inviter)
{
    return group->IsLeader(inviter->GetGUID()) || group->IsAssistant(inviter->GetGUID());
}

// The partner's group, or the one it is forming: an invitation it sent before it had a group leaves it the leader of
// a group not yet created (WorldSession::HandleGroupInviteOpcode).
Group* PartnerGroup(Player* partner)
{
    if (Group* group = partner->GetGroup())
        return group;

    Group* forming = partner->GetGroupInvite();
    return forming && forming->GetLeaderGUID() == partner->GetGUID() ? forming : nullptr;
}

// The bot's own security admits the group's leader, as AcceptInvitationAction checks the inviter it accepts; silent
// here, where the action would decline aloud.
bool AcceptsLeaderOf(ActFacts const& act, Group* group)
{
    Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
    return leader && act.Ai->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, true, leader);
}

// join_group: in the partner's group already; the partner's invitation it holds; the partner's group, when the
// partner may invite and it has room, and the bot has none; else the bot invites the partner, from no group or one it
// may invite into with room. Another group or invitation, a battleground queue, a full group or a leader the bot's
// security does not admit: cannot_now.
Refusal JoinCheck(ActFacts const& act)
{
    Player* bot = act.Bot;
    Player* partner = act.Partner;
    Group* own = act.BotGroup;
    Group* theirs = PartnerGroup(partner);
    if (own && own == theirs)
        return Refusal::None;

    if (bot->InBattlegroundQueue())
        return Refusal::CannotNow;

    if (Group* invite = bot->GetGroupInvite())
        return invite == theirs && AcceptsLeaderOf(act, invite) ? Refusal::None : Refusal::CannotNow;

    if (theirs)
        return !own && MayInvite(theirs, partner) && !theirs->IsFull() && AcceptsLeaderOf(act, theirs)
                   ? Refusal::None
                   : Refusal::CannotNow;

    if (partner->GetGroupInvite())
        return Refusal::CannotNow;

    return !own || (MayInvite(own, bot) && !own->IsFull()) ? Refusal::None : Refusal::CannotNow;
}

// The accept of an invitation (AcceptInvitationAction: the leader as a random bot's master, the group strategies),
// without its summon: a conversation's join never teleports (LC.8). Applied when the partner's group took the bot.
Result AcceptInvite(ActFacts const& act)
{
    AcceptInvitationAction accept(act.Ai, false);
    RunCommand(accept, "accept invitation", "", act.Partner);
    Group* group = act.Bot->GetGroup();
    return group && group->IsMember(act.Partner->GetGUID()) ? Result::Applied : Result::Failed;
}

// The partner's group takes the bot at once: the partner's invitation (the "join" command, JoinGroupAction), then the
// bot's accept. The bot's own invitation of the partner (the "invite" command, InviteToGroupAction) is applied when
// sent: the partner answers.
Result JoinOnce(ActFacts const& act)
{
    Player* bot = act.Bot;
    Player* partner = act.Partner;
    Group* theirs = PartnerGroup(partner);
    if (act.BotGroup && act.BotGroup == theirs)
        return Result::Applied;

    if (bot->GetGroupInvite())
        return AcceptInvite(act);

    if (theirs)
    {
        JoinGroupAction join(act.Ai);
        RunCommand(join, "join", "", partner);
        return bot->GetGroupInvite() ? AcceptInvite(act) : Result::Failed;
    }

    InviteToGroupAction invite(act.Ai);
    RunCommand(invite, "invite", "", partner);
    return partner->GetGroupInvite() ? Result::Applied : Result::Failed;
}

// A quest of the bot's log (a title was looked up there; an id may name any quest) that the core lets it share.
Refusal ShareCheck(ActFacts const& act)
{
    if (act.Bot->FindQuestSlot(act.QuestId) >= MAX_QUEST_LOG_SIZE)
        return Refusal::UnknownName;

    return act.Bot->CanShareQuest(act.QuestId) ? Refusal::None : Refusal::CannotNow;
}

// The "share <quest>" command (ShareQuestAction, its quest by id): the core offers the quest to each member on the
// bot's map and tells the bot each answer.
Result ShareOnce(ActFacts const& act)
{
    ShareQuestAction share(act.Ai);
    return RunCommand(share, "share", std::to_string(act.QuestId), act.Partner);
}

// A quest the bot may take now: not had or done, its level and class, and room in its log.
Refusal AcceptCheck(ActFacts const& act)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(act.QuestId);
    return quest && act.Bot->CanTakeQuest(quest, false) && act.Bot->CanAddQuest(quest, false) ? Refusal::None
                                                                                              : Refusal::CannotNow;
}

// The accept of a shared quest (AcceptQuestShareAction, as the bot takes its master's share): the quest offered to the
// bot, which tells its sharer and adds the quest. Applied when the quest is in the bot's log.
Result AcceptOnce(ActFacts const& act)
{
    WorldPacket offer(CMSG_PUSHQUESTTOPARTY);
    offer << act.QuestId;
    AcceptQuestShareAction accept(act.Ai);
    accept.Execute(Event("accept quest share", offer, act.Partner));
    return act.Bot->FindQuestSlot(act.QuestId) < MAX_QUEST_LOG_SIZE ? Result::Applied : Result::Failed;
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

// Choice parameters: the raid icons by index and the formation shapes by FormationShape value minus one (their words
// are in BridgeTexts.h).

// LC.29: an attack by name means an enemy creature, never a player. AttackParams are the old form's own target.
constexpr ParamRow AttackParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, BridgeTexts::Acts::ATTACK_TARGET, {}, UnitFilter::Enemy}};
constexpr ParamRow AttackByNameParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, BridgeTexts::Acts::ATTACK_BY_NAME_TARGET, {}, UnitFilter::Enemy}};
constexpr ParamRow PullParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, BridgeTexts::Acts::PULL_TARGET, {}, UnitFilter::Enemy}};
constexpr ParamRow CastParams[] = {
    {"spell", ParamKind::Spell, OrderField::SpellName, BridgeTexts::Acts::CAST_SPELL, {}},
    {"target", ParamKind::Unit, OrderField::Target, BridgeTexts::Acts::CAST_TARGET, {}, UnitFilter::Friend}};
constexpr ParamRow UseItemParams[] = {
    {"item", ParamKind::Item, OrderField::ItemName, BridgeTexts::Acts::USE_ITEM_ITEM, {}},
    {"target", ParamKind::Unit, OrderField::Target, BridgeTexts::Acts::USE_ITEM_TARGET, {}, UnitFilter::Anyone}};
constexpr ParamRow MarkParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, BridgeTexts::Acts::MARK_TARGET, {}, UnitFilter::Anyone},
    {"icon", ParamKind::Choice, OrderField::Icon, BridgeTexts::Acts::MARK_ICON, BridgeTexts::Acts::RAID_ICONS}};
constexpr ParamRow FocusHealParams[] = {
    {"target", ParamKind::Unit, OrderField::Target, BridgeTexts::Acts::FOCUS_HEAL_TARGET, {}, UnitFilter::GroupMember}};
constexpr ParamRow SaveManaParams[] = {
    {"enabled", ParamKind::Flag, OrderField::Enabled, BridgeTexts::Acts::SAVE_MANA_ENABLED, {}}};
constexpr ParamRow FormationParams[] = {{"shape", ParamKind::Choice, OrderField::Shape,
                                         BridgeTexts::Acts::FORMATION_SHAPE, BridgeTexts::Acts::FORMATION_SHAPES}};
constexpr ParamRow TripParams[] = {
    {"place", ParamKind::Place, OrderField::PlaceName, BridgeTexts::Acts::TRIP_PLACE, {}}};
constexpr ParamRow QuestParams[] = {
    {"quest", ParamKind::Quest, OrderField::QuestId, BridgeTexts::Acts::QUEST_QUEST, {}}};
constexpr ParamRow EmoteParams[] = {
    {"emote", ParamKind::Choice, OrderField::Choice, BridgeTexts::Acts::EMOTE_EMOTE, BridgeTexts::Acts::EMOTE_WORDS}};
constexpr ParamRow ShareQuestParams[] = {
    {"quest", ParamKind::Quest, OrderField::QuestId, BridgeTexts::Acts::QUEST_QUEST, {}}};
constexpr ParamRow AcceptQuestParams[] = {{"quest",
                                           ParamKind::Quest,
                                           OrderField::QuestId,
                                           BridgeTexts::Acts::ACCEPT_QUEST_QUEST,
                                           {},
                                           UnitFilter::Anyone,
                                           QuestFrom::Shared}};

// Each row's refusals besides the common ones, in check order.
constexpr Refusal FollowRefusals[] = {Refusal::NotWithYou, Refusal::TooFar};
constexpr Refusal FleeRefusals[] = {Refusal::CannotNow};
constexpr Refusal AttackRefusals[] = {Refusal::UnknownName, Refusal::TargetNotVisible, Refusal::NoTarget,
                                      Refusal::CannotNow};
constexpr Refusal AttackByNameRefusals[] = {Refusal::UnknownName, Refusal::TargetNotVisible, Refusal::CannotNow};
constexpr Refusal PullRefusals[] = {Refusal::NotInGroup, Refusal::UnknownName, Refusal::TargetNotVisible,
                                    Refusal::TooFar, Refusal::CannotNow};
constexpr Refusal CastRefusals[] = {Refusal::UnknownName, Refusal::TargetNotVisible, Refusal::TooFar,
                                    Refusal::CannotNow};
constexpr Refusal UseItemRefusals[] = {Refusal::UnknownName, Refusal::TargetNotVisible, Refusal::TooFar,
                                       Refusal::CannotNow};
constexpr Refusal GroupTargetRefusals[] = {Refusal::NotInGroup, Refusal::UnknownName, Refusal::TargetNotVisible,
                                           Refusal::CannotNow};
constexpr Refusal GroupSettingRefusals[] = {Refusal::NotInGroup};
constexpr Refusal TripRefusals[] = {Refusal::NotPlanner,   Refusal::GoalHeld,  Refusal::UnknownPlace,
                                    Refusal::AlreadyThere, Refusal::CannotNow, Refusal::NoRoute};
constexpr Refusal QuestRefusals[] = {Refusal::UnknownName, Refusal::NotPlanner, Refusal::GoalHeld,
                                     Refusal::NoSuchQuest};
constexpr Refusal ErrandRefusals[] = {Refusal::NotPlanner, Refusal::GoalHeld, Refusal::NothingHere};
constexpr Refusal HereRefusals[] = {Refusal::NotPlanner, Refusal::GoalHeld, Refusal::CannotNow, Refusal::NothingHere};
constexpr Refusal PlanRefusals[] = {Refusal::NotPlanner, Refusal::GoalHeld};
constexpr Refusal ReadyRefusals[] = {Refusal::CannotNow};
constexpr Refusal JoinRefusals[] = {Refusal::CannotNow};
constexpr Refusal QuestShareRefusals[] = {Refusal::NotInGroup, Refusal::UnknownName, Refusal::CannotNow};

constexpr Refusal Common[] = {Refusal::Battleground, Refusal::Travelling, Refusal::Dead};

// The verbs without a row, with their words on the wire.
struct NoEffectVerb
{
    Intent Operation;
    char const* Word;
};
constexpr NoEffectVerb NoEffect[] = {
    {Intent::Regroup, "regroup"}, {Intent::AssignRole, "assign_role"}, {Intent::DungeonClear, "dungeon_clear"}};

// Indexed by Refusal; the words and sentences the bridge wire already uses.
constexpr RefusalText RefusalTexts[] = {
    {Refusal::None, "none", ""},
    {Refusal::Dead, "dead", BridgeTexts::Refusals::DEAD},
    {Refusal::Battleground, "battleground", BridgeTexts::Refusals::BATTLEGROUND},
    {Refusal::Travelling, "travelling", BridgeTexts::Refusals::TRAVELLING},
    {Refusal::NotWithYou, "not_with_you", BridgeTexts::Refusals::NOT_WITH_YOU},
    {Refusal::TooFar, "too_far", BridgeTexts::Refusals::TOO_FAR},
    {Refusal::NotInGroup, "not_in_group", BridgeTexts::Refusals::NOT_IN_GROUP},
    {Refusal::Unreachable, "unreachable", BridgeTexts::Refusals::UNREACHABLE},
    {Refusal::TargetNotVisible, "target_not_visible", BridgeTexts::Refusals::TARGET_NOT_VISIBLE},
    {Refusal::NoTarget, "no_target", BridgeTexts::Refusals::NO_TARGET},
    {Refusal::CannotNow, "cannot_now", BridgeTexts::Refusals::CANNOT_NOW},
    {Refusal::NotPlanner, "not_planner", BridgeTexts::Refusals::NOT_PLANNER},
    {Refusal::GoalHeld, "goal_held", BridgeTexts::Refusals::GOAL_HELD},
    {Refusal::UnknownPlace, "unknown_place", BridgeTexts::Refusals::UNKNOWN_PLACE},
    {Refusal::NoRoute, "no_route", BridgeTexts::Refusals::NO_ROUTE},
    {Refusal::NoSuchQuest, "no_such_quest", BridgeTexts::Refusals::NO_SUCH_QUEST},
    {Refusal::NothingHere, "nothing_here", BridgeTexts::Refusals::NOTHING_HERE},
    {Refusal::AlreadyThere, "already_there", BridgeTexts::Refusals::ALREADY_THERE},
    {Refusal::UnknownName, "unknown_name", BridgeTexts::Refusals::UNKNOWN_NAME}};

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
     .Sentence = BridgeTexts::Acts::FOLLOW_SENTENCE,
     .Params = {},
     .Group = GroupRule::PartnerInGroup,
     .GroupRefusal = Refusal::NotWithYou,
     .Check = FollowCheck,
     .Refusals = FollowRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return FollowOnce(act.Ai, act.Partner); },
     .Done = Completion::Holds,
     .StatePhrase = BridgeTexts::Acts::FOLLOW_STATE},
    {.Operation = Intent::Stay,
     .Name = "stay",
     .Sentence = BridgeTexts::Acts::STAY_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = {},
     .Effect = [](ActFacts const& act, Refusal&) { return StayOnce(act.Ai, act.Partner); },
     .Done = Completion::Holds,
     .StatePhrase = BridgeTexts::Acts::STAY_STATE},
    {.Operation = Intent::Flee,
     .Name = "flee",
     .Sentence = BridgeTexts::Acts::FLEE_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = FleeRefusals,
     .Effect = [](ActFacts const& act, Refusal& why) { return FleeOnce(act.Ai, act.Bot, why); },
     .Done = Completion::Arrived,
     .StatePhrase = BridgeTexts::Acts::FLEE_STATE},
    {.Operation = Intent::Attack,
     .Name = "attack",
     .Sentence = BridgeTexts::Acts::ATTACK_SENTENCE,
     .Params = AttackByNameParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = AttackByNameCheck,
     .Refusals = AttackByNameRefusals,
     .Effect = AttackOnce,
     .Done = Completion::TargetGone,
     .StatePhrase = BridgeTexts::Acts::ATTACK_STATE},
    {.Operation = Intent::Mark,
     .Name = "mark",
     .Sentence = BridgeTexts::Acts::MARK_SENTENCE,
     .Params = MarkParams,
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = MarkCheck,
     .Refusals = GroupTargetRefusals,
     .Effect = MarkOnce,
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::MARK_STATE},
    {.Operation = Intent::FocusHealing,
     .Name = "focus_healing",
     .Sentence = BridgeTexts::Acts::FOCUS_HEALING_SENTENCE,
     .Params = FocusHealParams,
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = nullptr,
     .Refusals = GroupTargetRefusals,
     .Effect = FocusHealOnce,
     .Done = Completion::Holds,
     .StatePhrase = BridgeTexts::Acts::FOCUS_HEALING_STATE},
    {.Operation = Intent::SaveMana,
     .Name = "save_mana",
     .Sentence = BridgeTexts::Acts::SAVE_MANA_SENTENCE,
     .Params = SaveManaParams,
     .Group = GroupRule::PartnerInGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = nullptr,
     .Refusals = GroupSettingRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return SaveManaOnce(act.Ai, act.Partner, act.Request.Enabled); },
     .Done = Completion::Holds,
     .StatePhrase = BridgeTexts::Acts::SAVE_MANA_STATE},
    {.Operation = Intent::Formation,
     .Name = "formation",
     .Sentence = BridgeTexts::Acts::FORMATION_SENTENCE,
     .Params = FormationParams,
     .Group = GroupRule::PartnerInGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = nullptr,
     .Refusals = GroupSettingRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return FormationOnce(act.Ai, act.Partner, act.Request); },
     .Done = Completion::Holds,
     .StatePhrase = BridgeTexts::Acts::FORMATION_STATE},
    {.Operation = Intent::GoTo,
     .Name = "go_to",
     .Sentence = BridgeTexts::Acts::GO_TO_SENTENCE,
     .Params = TripParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = TripRefusals,
     .Effect = GoalOnce,
     .Done = Completion::GoalEnds,
     .StatePhrase = BridgeTexts::Acts::GO_TO_STATE},
    {.Operation = Intent::Quest,
     .Name = "quest",
     .Sentence = BridgeTexts::Acts::QUEST_SENTENCE,
     .Params = QuestParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = QuestRefusals,
     .Effect = GoalOnce,
     .Done = Completion::GoalEnds,
     .StatePhrase = BridgeTexts::Acts::QUEST_STATE},
    {.Operation = Intent::Errands,
     .Name = "errands",
     .Sentence = BridgeTexts::Acts::ERRANDS_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = ErrandRefusals,
     .Effect = GoalOnce,
     .Done = Completion::GoalEnds,
     .StatePhrase = BridgeTexts::Acts::ERRANDS_STATE},
    {.Operation = Intent::SetHearth,
     .Name = "set_hearth",
     .Sentence = BridgeTexts::Acts::SET_HEARTH_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = ErrandRefusals,
     .Effect = GoalOnce,
     .Done = Completion::GoalEnds,
     .StatePhrase = BridgeTexts::Acts::SET_HEARTH_STATE},
    {.Operation = Intent::GrindHere,
     .Name = "grind_here",
     .Sentence = BridgeTexts::Acts::GRIND_HERE_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = HereRefusals,
     .Effect = GoalOnce,
     .Done = Completion::GoalEnds,
     .StatePhrase = BridgeTexts::Acts::GRIND_HERE_STATE},
    {.Operation = Intent::GatherHere,
     .Name = "gather_here",
     .Sentence = BridgeTexts::Acts::GATHER_HERE_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = HereRefusals,
     .Effect = GoalOnce,
     .Done = Completion::GoalEnds,
     .StatePhrase = BridgeTexts::Acts::GATHER_HERE_STATE},
    {.Operation = Intent::MeetAt,
     .Name = "meet_at",
     .Sentence = BridgeTexts::Acts::MEET_AT_SENTENCE,
     .Params = TripParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = TripRefusals,
     .Effect = GoalOnce,
     .Done = Completion::GoalEnds,
     .StatePhrase = BridgeTexts::Acts::MEET_AT_STATE},
    {.Operation = Intent::PausePlan,
     .Name = "pause_plan",
     .Sentence = BridgeTexts::Acts::PAUSE_PLAN_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = PlanRefusals,
     .Effect = GoalOnce,
     .Done = Completion::Holds,
     .StatePhrase = BridgeTexts::Acts::PAUSE_PLAN_STATE},
    {.Operation = Intent::ResumePlan,
     .Name = "resume_plan",
     .Sentence = BridgeTexts::Acts::RESUME_PLAN_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = PlanRefusals,
     .Effect = GoalOnce,
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::RESUME_PLAN_STATE},
    {.Operation = Intent::Emote,
     .Name = "emote",
     .Sentence = BridgeTexts::Acts::EMOTE_SENTENCE,
     .Params = EmoteParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = {},
     .Effect = [](ActFacts const& act, Refusal&) { return EmoteOnce(act); },
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::EMOTE_STATE},
    {.Operation = Intent::Roll,
     .Name = "roll",
     .Sentence = BridgeTexts::Acts::ROLL_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = {},
     .Effect = [](ActFacts const& act, Refusal&) { return RollOnce(act); },
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::ROLL_STATE},
    // No group rule: an ungrouped bot has no check to answer, cannot_now (ReadyRowCheck).
    {.Operation = Intent::Ready,
     .Name = "ready",
     .Sentence = BridgeTexts::Acts::READY_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = ReadyRowCheck,
     .Refusals = ReadyRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return ReadyOnce(act); },
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::READY_STATE},
    {.Operation = Intent::LeaveGroup,
     .Name = "leave_group",
     .Sentence = BridgeTexts::Acts::LEAVE_GROUP_SENTENCE,
     .Params = {},
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = nullptr,
     .Refusals = GroupSettingRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return LeaveOnce(act); },
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::LEAVE_GROUP_STATE},
    // No group rule: it makes the group.
    {.Operation = Intent::JoinGroup,
     .Name = "join_group",
     .Sentence = BridgeTexts::Acts::JOIN_GROUP_SENTENCE,
     .Params = {},
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = JoinCheck,
     .Refusals = JoinRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return JoinOnce(act); },
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::JOIN_GROUP_STATE},
    {.Operation = Intent::ShareQuest,
     .Name = "share_quest",
     .Sentence = BridgeTexts::Acts::SHARE_QUEST_SENTENCE,
     .Params = ShareQuestParams,
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = ShareCheck,
     .Refusals = QuestShareRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return ShareOnce(act); },
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::SHARE_QUEST_STATE},
    {.Operation = Intent::AcceptQuest,
     .Name = "accept_quest",
     .Sentence = BridgeTexts::Acts::ACCEPT_QUEST_SENTENCE,
     .Params = AcceptQuestParams,
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = AcceptCheck,
     .Refusals = QuestShareRefusals,
     .Effect = [](ActFacts const& act, Refusal&) { return AcceptOnce(act); },
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::ACCEPT_QUEST_STATE},
    {.Operation = Intent::Pull,
     .Name = "pull",
     .Sentence = BridgeTexts::Acts::PULL_SENTENCE,
     .Params = PullParams,
     .Group = GroupRule::BotGroup,
     .GroupRefusal = Refusal::NotInGroup,
     .Check = PullCheck,
     .Refusals = PullRefusals,
     .Effect = PullOnce,
     .Done = Completion::TargetGone,
     .StatePhrase = BridgeTexts::Acts::PULL_STATE},
    {.Operation = Intent::Cast,
     .Name = "cast",
     .Sentence = BridgeTexts::Acts::CAST_SENTENCE,
     .Params = CastParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = CastCheck,
     .Refusals = CastRefusals,
     .Effect = CastOnce,
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::CAST_STATE},
    {.Operation = Intent::UseItem,
     .Name = "use_item",
     .Sentence = BridgeTexts::Acts::USE_ITEM_SENTENCE,
     .Params = UseItemParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = nullptr,
     .Refusals = UseItemRefusals,
     .Effect = UseItemOnce,
     .Done = Completion::AtOnce,
     .StatePhrase = BridgeTexts::Acts::USE_ITEM_STATE},
    // The old form's own-target attack (mod-ollama-chat's frozen verbs, LC.23); the catalogue offers attack.
    {.Operation = Intent::AttackOwnTarget,
     .Name = "attack_own_target",
     .Sentence = BridgeTexts::Acts::ATTACK_OWN_TARGET_SENTENCE,
     .Params = AttackParams,
     .Group = GroupRule::None,
     .GroupRefusal = Refusal::None,
     .Check = AttackCheck,
     .Refusals = AttackRefusals,
     .Effect = AttackOnce,
     .Done = Completion::TargetGone,
     .StatePhrase = BridgeTexts::Acts::ATTACK_STATE,
     .OldFormOnly = true}};

// The rows the catalogue lists come first; the old form's rows close the table (AbilityRows is the first part).
constexpr std::size_t ListedRows()
{
    std::size_t listed = 0;
    while (listed < std::size(Rows) && !Rows[listed].OldFormOnly)
        ++listed;

    return listed;
}

constexpr bool OldFormRowsClose()
{
    for (std::size_t index = ListedRows(); index < std::size(Rows); ++index)
        if (!Rows[index].OldFormOnly)
            return false;

    return true;
}

static_assert(OldFormRowsClose(), "the old form's rows close the table, after every row the catalogue lists");

static_assert(std::size(Rows) + std::size(NoEffect) == IntentCount, "every Intent has a row or a word without one");

// Execute's body below it: one outcome per step that ran after the admission.
Result ExecuteSteps(PostMapJoin const& phase, std::span<Order const> steps, std::function<bool()> const& authorize,
                    std::vector<StepOutcome>& outcomes);
}  // namespace

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

constexpr bool RowsTakeOneParamPerField()
{
    for (AbilityRow const& row : Rows)
        if (!OneParamPerField(row.Params))
            return false;

    return true;
}
static_assert(RowsTakeOneParamPerField(), "a row has at most one parameter per Order field");

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

std::span<AbilityRow const> AbilityRows() { return std::span<AbilityRow const>(Rows).first(ListedRows()); }

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

ParamRow const* ParamOf(AbilityRow const& row, OrderField field)
{
    for (ParamRow const& param : row.Params)
        if (param.Field == field)
            return &param;

    return nullptr;
}

bool Takes(AbilityRow const& row, OrderField field) { return ParamOf(row, field) != nullptr; }

namespace
{
constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

constexpr char const* const AbilityKindWords[] = {"act", "topic"};
constexpr char const* const ParamKindWords[] = {"unit", "place", "item", "spell", "quest", "choice", "flag", "count"};
constexpr char const* const CompletionWords[] = {"at_once", "holds", "arrived", "target_gone", "goal_ends"};
static_assert(std::size(AbilityKindWords) == static_cast<std::size_t>(AbilityKind::Topic) + 1);
static_assert(std::size(ParamKindWords) == static_cast<std::size_t>(ParamKind::Count) + 1);
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
    std::span<AbilityRow const> const rows = AbilityRows();
    catalogue.reserve(rows.size() + std::size(TopicTable));
    for (AbilityRow const& row : rows)
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
        entry.Done = row.Done;
        entry.StatePhrase = row.StatePhrase;
        entry.Event = std::string(EventLog::Word(EventLog::EventType::BridgeAct));
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
        Line(text, {"effect", EffectWord});
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

// A unit the bot perceives that the filter lets a name mean (UnitFilter); group members come from the group's slots.
bool Admits(UnitFilter filter, Player* bot, Unit* unit)
{
    switch (filter)
    {
        case UnitFilter::Enemy:
            return !unit->GetCharmerOrOwnerPlayerOrPlayerItself() && !bot->IsFriendlyTo(unit);
        case UnitFilter::Friend:
            return bot->IsFriendlyTo(unit);
        case UnitFilter::GroupMember:
            return false;
        default:
            return true;
    }
}

// The units the bot perceives (the nearest NPCs, possible targets and nearest friendly players it keeps as values)
// and its group's members, those the filter admits. A unit in two lists is the same id twice. The bot's own living
// target (the unit it fights, then its selection, never the bot) counts as the nearest unit of its name; an exact
// name still beats a part of one. The bot itself is among its friends.
uint64_t UnitNamed(ActFacts const& act, UnitFilter filter, std::string const& name)
{
    std::vector<NameCandidate> candidates;
    // The first of equal distance wins, so the unit fought beats the selection.
    for (Unit* own : {act.Bot->GetVictim(), ObjectAccessor::GetUnit(*act.Bot, act.Bot->GetTarget())})
        if (own && Admits(filter, act.Bot, own) && own != act.Bot && own->IsAlive())
            candidates.push_back({own->GetName(), own->GetGUID().GetRawValue(), 0.0f});

    if (filter == UnitFilter::Friend)
        candidates.push_back({act.Bot->GetName(), act.Bot->GetGUID().GetRawValue(), 0.0f});

    // A friend's name means a dead friend in sight too (a resurrection's target), not only a living one or a group
    // member; an enemy's never means the dead.
    char const* const dead = filter == UnitFilter::Friend ? "nearest corpses" : nullptr;
    for (char const* list : {"nearest npcs", "possible targets", "nearest friendly players", dead})
        if (Value<GuidVector>* units = list ? act.Ai->GetAiObjectContext()->GetValue<GuidVector>(list) : nullptr)
            for (ObjectGuid const& guid : units->Get())
                if (Unit* unit = ObjectAccessor::GetUnit(*act.Bot, guid); unit && Admits(filter, act.Bot, unit))
                    candidates.push_back(
                        {unit->GetName(), guid.GetRawValue(),
                         DistanceTo(act.Bot, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ())});

    if (act.BotGroup && filter != UnitFilter::Enemy)
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

// The id of the candidate a name means; 0 for none.
uint32_t IdNamed(std::vector<NameCandidate> const& candidates, std::string const& name)
{
    NameResult const match = ResolveName(name, candidates);
    return match.Kind == NameMatch::None ? 0 : static_cast<uint32_t>(candidates[match.Index].Id);
}

// The quests in a player's log, by title, as ReadQuests walks them.
std::vector<NameCandidate> LogQuests(Player* player)
{
    std::vector<NameCandidate> candidates;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        if (Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr)
            candidates.push_back({quest->GetTitle(), questId, 0.0f});
    }
    return candidates;
}

// The quests in the bot's log, by title; the planner still refuses a failed one.
uint32_t QuestNamed(Player* bot, std::string const& name) { return IdNamed(LogQuests(bot), name); }

// A quest of the sharer's log, by id or title: the sharer the core noted (the divider), still in the bot's group; 0
// when nothing was shared or the sharer's log has no such quest.
uint32_t SharedQuestNamed(Player* bot, Group* group, uint32_t id, std::string const& name)
{
    ObjectGuid const divider = bot->GetDivider();
    Player* sharer =
        divider && group && group->IsMember(divider) ? ObjectAccessor::FindConnectedPlayer(divider) : nullptr;
    if (!sharer)
        return 0;

    std::vector<NameCandidate> const candidates = LogQuests(sharer);
    if (!id)
        return IdNamed(candidates, name);

    bool const held = std::any_of(candidates.begin(), candidates.end(),
                                  [id](NameCandidate const& candidate) { return candidate.Id == id; });
    return held ? id : 0;
}

// The partner's spot as a literal "x,y,z,map", for the planner's place lookup.
std::string PartnerSpot(Player* partner)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%.1f,%.1f,%.1f,%u", partner->GetPositionX(), partner->GetPositionY(),
                  partner->GetPositionZ(), partner->GetMapId());
    return buffer;
}

// A literal "x,y,z,map" as given; else the known place the name means (one on another map as elsewhere), under the
// name the planner's place lookup takes. No match: the text as given, so the planner refuses it after its own rules.
std::string PlaceNamed(Player* bot, Player* partner, std::string const& text)
{
    // The one the bot is talking with, as a place: resolved now, from the partner's position at this act (never a
    // stored one), the word or the partner's own name. The planner then decides whether a route exists.
    if (IsPartnerPlace(text, partner->GetName()))
        return PartnerSpot(partner);

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
    if (match.Kind != NameMatch::None)
        return candidates[match.Index].Name;
    // LC18a: a town the bot knows by the where topic's rule, as its centre's literal; else the text as given
    std::optional<std::string> const town = KnownTownSpot(bot, text);
    return town ? *town : text;
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

    // Following and the group settings are group play: the partner shares the bot's group. Stay, flee,
    // an attack and a goal need no group; a mark, a pull and focus healing need the bot's own group.
    if (!InGroup(row.Group, act))
        return Refuse(why, row.GroupRefusal);

    // The bot acts on what it sees itself; whether the partner sees the target plays no part. A unit given by name
    // then passes the same test as one given by id.
    if (Takes(row, OrderField::Target))
    {
        uint64_t const target = act.Request.TargetName.empty()
                                    ? act.Request.Target
                                    : UnitNamed(act, ParamOf(row, OrderField::Target)->Filter, act.Request.TargetName);
        if (!target)
            return Refuse(why, Refusal::UnknownName);

        act.Target = ObjectAccessor::GetUnit(*bot, ObjectGuid(target));
        if (!Perceives(bot, act.Target))
            return Refuse(why, Refusal::TargetNotVisible);
    }

    if (ParamRow const* quest = ParamOf(row, OrderField::QuestId))
    {
        if (quest->Quests == QuestFrom::Shared)
            act.QuestId = SharedQuestNamed(bot, act.BotGroup, act.Request.QuestId, act.Request.QuestName);
        else
            act.QuestId = act.Request.QuestName.empty() ? act.Request.QuestId : QuestNamed(bot, act.Request.QuestName);

        if (!act.QuestId)
            return Refuse(why, Refusal::UnknownName);
    }

    if (Takes(row, OrderField::PlaceName))
        act.Place = PlaceNamed(bot, act.Partner, act.Request.PlaceName);

    // An item among what the bot wears and carries, a spell among what it knows: two lists, one per kind.
    if (Takes(row, OrderField::ItemName))
    {
        act.ItemId = IdNamed(CarriedItemNames(bot), act.Request.ItemName);
        if (!act.ItemId)
            return Refuse(why, Refusal::UnknownName);
    }

    if (Takes(row, OrderField::SpellName))
    {
        act.SpellId = IdNamed(KnownSpellNames(bot), act.Request.SpellName);
        if (!act.SpellId)
            return Refuse(why, Refusal::UnknownName);
    }

    Refusal const reason = row.Check ? row.Check(act) : Refusal::None;
    if (reason != Refusal::None)
        return Refuse(why, reason);

    QuietScope const quiet;
    return row.Effect(act, why);
}

// How many of a parameter's two forms, id and name, an order fills: one for a row that takes it, else none.
int Forms(bool id, std::string const& name) { return (id ? 1 : 0) + (name.empty() ? 0 : 1); }

// The item, spell, choice and count fields: each present exactly when the row takes it, a name within MaxName and a
// choice among the parameter's Choices.
bool KindsMalformed(AbilityRow const& row, Order const& order)
{
    ParamRow const* choice = ParamOf(row, OrderField::Choice);
    return Takes(row, OrderField::ItemName) == order.ItemName.empty() || order.ItemName.size() > MaxName ||
           Takes(row, OrderField::SpellName) == order.SpellName.empty() || order.SpellName.size() > MaxName ||
           (choice ? order.Choice >= choice->Choices.size() : order.Choice != 0) ||
           Takes(row, OrderField::Count) == (order.Count == 0);
}

// A stray target, shape, enable flag, place, quest, item, spell, choice or count on another verb is a mapping error,
// never an ignored field; a unit or quest comes as an id or a name, never both.
bool Malformed(AbilityRow const* row, Order const& order)
{
    if (KindsMalformed(*row, order))
        return true;

    return !order.Bot || !order.Controller || order.Bot == order.Controller ||
           Forms(order.Target != 0, order.TargetName) != (Takes(*row, OrderField::Target) ? 1 : 0) ||
           order.TargetName.size() > MaxName || Takes(*row, OrderField::PlaceName) != !order.PlaceName.empty() ||
           order.PlaceName.size() > MaxPlaceName ||
           Forms(order.QuestId != 0, order.QuestName) != (Takes(*row, OrderField::QuestId) ? 1 : 0) ||
           order.QuestName.size() > MaxName ||
           Takes(*row, OrderField::Shape) != (FormationName(order.Shape) != nullptr) ||
           (order.Enabled && !Takes(*row, OrderField::Enabled)) ||
           (Takes(*row, OrderField::Icon) &&
            static_cast<std::size_t>(order.Icon) >= std::size(BridgeTexts::Acts::RAID_ICONS));
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
    ActScope const open;
    Result const result = RunRow(row, act, why);
    // One event for each step the facts admit: applied, failed or refused (id: the refusal, else 0).
    EventLog::Emit(bot, EventLog::EventType::BridgeAct, row.Name,
                   static_cast<uint32>(result == Result::Rejected ? why : Refusal::None), static_cast<int64>(result));
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
            Malformed(row, order))
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

bool HoldsTell(Player* bot)
{
    TellFate const fate = FateOfTell(ActState());
    if (fate == TellFate::SpeakCounted && bot)
        EventLog::Emit(bot, EventLog::EventType::BridgeOwnLine, "tell");

    return fate == TellFate::Hold;
}

void NoteCannedTell(Player* bot)
{
    if (bot && FateOfTell(ActState()) == TellFate::Speak)
        EventLog::Emit(bot, EventLog::EventType::CannedLine, "tell");
}
}  // namespace PlayerbotBridge
