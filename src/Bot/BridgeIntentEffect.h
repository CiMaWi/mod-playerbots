/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEINTENTEFFECT_H
#define PLAYERBOTS_BRIDGEINTENTEFFECT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

class OllamaBotRandomChatter;

namespace PlayerbotBridge
{
// The verbs, append only (the wire's order). The goal verbs (WT18a, from GoTo on) hand the planner a goal of source
// bridge (Planner::BridgeGoal): it outranks the bot's own until it ends, a new bridge goal replaces it, and a group's
// goal is never replaced. Each refuses before any change with a reason:
//   GoTo (go_to), PlaceName: a trip to a known place or x,y,z,map (RPG_TRAVEL); UnknownPlace, AlreadyThere,
//     NoRoute (one route search), CannotNow inside an instance.
//   Quest (quest), QuestId: work or turn in a log quest (DO_QUEST); NoSuchQuest when it is not in the log or failed.
//   Errands (errands): an errand trip near the bot (RPG_ERRANDS); SetHearth (set_hearth): a trip to the nearest
//     fitting inn on its map and a bind there; NothingHere when no stop or inn is in reach.
//   GrindHere (grind_here), GatherHere (gather_here): around the bot (RPG_GO_GRIND, RPG_DO_GATHER); NothingHere in a
//     town or an inn, or without herbalism or mining; CannotNow inside an instance.
//   MeetAt (meet_at), PlaceName: GoTo with the meeting as its reason.
//   PausePlan (pause_plan): the bot's plan stops, the bot stands idle until ResumePlan (resume_plan) or ten minutes;
//     ResumePlan also ends a bridge goal.
//   Every goal verb: NotPlanner when the planner does not run the bot (a real master such as a
//   player's group, not persistent), GoalHeld when a group's goal holds the slot.
// The social verbs (LC16d) are done at once: Emote (emote) a gesture, Roll (roll) a roll of 1-100, Ready (ready) the
// answer to the group's open ready check, LeaveGroup (leave_group) the bot leaves its group. With a counterpart
// (LC16e): JoinGroup (join_group) the bot joins the partner's group, ShareQuest (share_quest) shares a quest of its
// log with its group, AcceptQuest (accept_quest) takes the quest a group member shared with it.
// The fight verbs (LC17a): Attack (attack) an enemy creature by name, any the bot sees (AttackOwnTarget is the old
// form's own-target attack, out of the catalogue); Pull (pull) one creature near the bot's group, by its tank or a
// ranged attacker. Cast (cast, LC17b) a helpful spell of the bot's own on a friend by name, or on itself.
// UseItem (use_item, WI10a) an item the bot wears or carries, on itself or on a unit by name, through the item-use
// framework (ItemUse::Use, caller ability); a refusal is the framework's cause, collapsed onto these values.
enum class Intent : uint8_t
{
    Follow,
    Stay,
    Flee,
    Regroup,
    AttackOwnTarget,
    Pull,
    Mark,
    FocusHealing,
    AssignRole,
    SaveMana,
    Formation,
    DungeonClear,
    GoTo,
    Quest,
    Errands,
    SetHearth,
    GrindHere,
    GatherHere,
    MeetAt,
    PausePlan,
    ResumePlan,
    Emote,
    Roll,
    Ready,
    LeaveGroup,
    JoinGroup,
    ShareQuest,
    AcceptQuest,
    Attack,
    Cast,
    UseItem
};

enum class Result : uint8_t
{
    Rejected,
    Unavailable,
    Applied,
    Failed
};

// Why Execute refused an order (Rejected), so the caller can word it for the partner. None for every other result and
// for refusals that are no concern of the partner's: a malformed or stale order, or the caller's own admission.
enum class Refusal : uint8_t
{
    None,
    Dead,              // The bot is dead or a ghost.
    Battleground,      // The bot is in a battleground or an arena.
    Travelling,        // The bot is on a flight path.
    NotWithYou,        // Follow: the partner is not in the bot's group.
    TooFar,            // Follow: the partner is on another map or in another instance.
    NotInGroup,        // Mark, focus healing: the bot has no group; formation, save mana: the partner is not in it.
    Unreachable,       // No complete short path (kept as a refusal word; no act gives it since LC17e).
    TargetNotVisible,  // Attack, mark, focus healing: the bot does not see the target.
    NoTarget,          // Attack: the target is not the bot's own.
    CannotNow,     // The verb's own state rule (mounted, no opponent, raid marking, not a healer, an instance, ...).
    NotPlanner,    // Goal verbs: the planner does not run the bot.
    GoalHeld,      // Goal verbs: a group's goal holds the bot.
    UnknownPlace,  // GoTo, MeetAt: no such place.
    NoRoute,       // GoTo, MeetAt: no route from the bot to the place.
    NoSuchQuest,   // Quest: not in the bot's log, or failed.
    NothingHere,   // Errands, SetHearth, GrindHere, GatherHere: nothing to do here.
    AlreadyThere,  // GoTo, MeetAt: the bot is at the place.
    UnknownName    // Attack, mark, focus healing, quest: nothing of that name the bot sees or has in its log.
};

// Formation only: the wire shapes, mapped to playerbots formations (close -> near, spread -> far, line -> line).
enum class FormationShape : uint8_t
{
    None,
    Close,
    Spread,
    Line
};

struct Order
{
    Intent Operation = Intent::Stay;
    uint64_t Bot = 0;
    // The conversation partner: the speaker of the line the bot reacts to (the field keeps the wire's old name).
    uint64_t Controller = 0;
    // The bot's group when the line was captured, 0 for an ungrouped bot; Execute refuses once it no longer matches.
    uint64_t Group = 0;
    uint64_t Target = 0;
    uint8_t Icon = 0;
    // Formation only: the shape; None on every other verb.
    FormationShape Shape = FormationShape::None;
    // SaveMana only: true saves mana at the class level the "save mana *" command picks, false spends freely (1.0).
    bool Enabled = false;
    // GoTo and MeetAt only: a known place's name or "x,y,z,map", at most MaxPlaceName characters; empty otherwise.
    std::string PlaceName = {};
    // Quest only: the quest id; 0 otherwise.
    uint32_t QuestId = 0;
    // A unit's or quest's name instead of Target or QuestId (exactly one of the two), at most MaxName characters:
    // Execute looks it up among what the bot sees or has in its log; empty otherwise. A place name stays in PlaceName.
    std::string TargetName = {};
    std::string QuestName = {};
    // An item the bot carries and a spell it knows, by name (at most MaxName characters), for a row that takes one;
    // empty otherwise. Execute looks them up among the bot's items and spells.
    std::string ItemName = {};
    std::string SpellName = {};
    // A choice parameter's index into its Choices, for a row that takes one (OrderField::Choice); 0 otherwise.
    uint8_t Choice = 0;
    // A count, for a row that takes one: at least 1; 0 otherwise.
    uint32_t Count = 0;
};

constexpr std::size_t MaxPlaceName = 64;
constexpr std::size_t MaxName = MaxPlaceName;

// LC.7: the most steps one order holds.
constexpr std::size_t MaxSteps = 3;

// What one step of an order came to; Why as Execute's why.
struct StepOutcome
{
    Result Outcome = Result::Rejected;
    Refusal Why = Refusal::None;
};

// Construct only on the stack in OllamaBotRandomChatter::OnUpdate, after MapMgr::Update joins its workers.
// Friendship restricts ingress; that exact construction site remains a source-audited integration requirement.
class PostMapJoin final
{
public:
    PostMapJoin(PostMapJoin const&) = delete;
    PostMapJoin& operator=(PostMapJoin const&) = delete;
    bool Current() const;

private:
    friend class ::OllamaBotRandomChatter;
    PostMapJoin();
    std::thread::id const _thread;
};

// Synchronous only. The caller owns replay/rate/epoch/deadline/audience admission and supplies its last currentness
// check here. No order, game pointer, callback or phase scope is retained. Failed attempts must never be replayed.
// DS.14: the bot acts as itself in a conversation with the partner (Order::Controller). Execute checks plausibility and
// safety, never who leads the bot: the bot in the world, a bot, not in a battleground, not in flight, alive; the
// partner online, allowed (PartnerAllowed) and of the bot's team; the captured group still the bot's; then each verb's
// own rule (the goal verbs': Intent's list). why, when given, receives the reason of a Rejected result and None for
// every other result. A unit, quest or place given by name is looked up only after the common and group rules and
// then passes the same checks as an id. One step of the order below.
Result Execute(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize,
               Refusal* why = nullptr);

// One order of 1 to MaxSteps steps for one bot and partner, run in order as one command: every step's shape and the
// shared bot, partner, group and visibility distance are checked first, then authorize is called once; each step then
// re-reads the facts, passes its own checks and writes its event. A step that is not Applied ends the order. The result
// is Applied when every step applied, else the ending step's. outcomes, when given, receives one entry per step that
// ran after the admission, in order (empty when the order was refused before it).
Result Execute(PostMapJoin const& phase, std::span<Order const> steps, std::function<bool()> const& authorize,
               std::vector<StepOutcome>* outcomes = nullptr);
}  // namespace PlayerbotBridge

#endif
