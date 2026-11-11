/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The isolated runner supplies game doubles and includes the unchanged production effect implementation and the
// AttackAction, StayAction, FleeAction and MoveToPositionAction::ExecuteExactPosition method bodies; FollowAction,
// MovementAction::MoveAway/MoveTo and PathGenerator are counting doubles; the formation, mana save level and focus heal
// values are plain doubles. It does not replace a decision path with a parallel guard implementation.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <type_traits>

// clang-format off
#include "J6b1-game-fixture.h"
#include "BridgeIntentEffect.cpp"
#include "BridgeAbilities.cpp"
#include "AttackAction-body.inc"
#include "StayAction-body.inc"
#include "FleeAction-body.inc"
#include "PositionAction-body.inc"
// clang-format on

using PlayerbotBridge::AbilityData;
using PlayerbotBridge::AbilityKind;
using PlayerbotBridge::AbilityRow;
using PlayerbotBridge::Baseline;
using PlayerbotBridge::BotStateTopic;
using PlayerbotBridge::Completion;
using PlayerbotBridge::FormationShape;
using PlayerbotBridge::Intent;
using PlayerbotBridge::Order;
using PlayerbotBridge::OrderField;
using PlayerbotBridge::ParamKind;
using PlayerbotBridge::ParamRow;
using PlayerbotBridge::Part;
using PlayerbotBridge::Refusal;
using PlayerbotBridge::RestoreResult;
using PlayerbotBridge::Result;
using PlayerbotBridge::TopicRow;

static_assert(!std::is_default_constructible_v<PlayerbotBridge::PostMapJoin>);
static_assert(!std::is_copy_constructible_v<PlayerbotBridge::PostMapJoin>);
static_assert(!std::is_move_constructible_v<PlayerbotBridge::PostMapJoin>);

class OllamaBotRandomChatter
{
public:
    static Result Run(Order const& order, std::function<bool()> const& authorize, bool worker = false,
                      Refusal* why = nullptr)
    {
        PlayerbotBridge::PostMapJoin phase;
        if (!worker)
            return why ? PlayerbotBridge::Execute(phase, order, authorize, why)
                       : PlayerbotBridge::Execute(phase, order, authorize);

        Result result = Result::Applied;
        std::thread thread([&] { result = PlayerbotBridge::Execute(phase, order, authorize, why); });
        thread.join();
        return result;
    }

    static Result RunSteps(std::vector<Order> const& steps, std::function<bool()> const& authorize,
                           std::vector<PlayerbotBridge::StepOutcome>* outcomes)
    {
        PlayerbotBridge::PostMapJoin phase;
        return PlayerbotBridge::Execute(phase, std::span<Order const>(steps), authorize, outcomes);
    }
    static bool Reachable(Order const& order, bool worker = false)
    {
        PlayerbotBridge::PostMapJoin phase;
        if (!worker)
            return PlayerbotBridge::PositionReachable(phase, order);

        bool result = true;
        std::thread thread([&] { result = PlayerbotBridge::PositionReachable(phase, order); });
        thread.join();
        return result;
    }

    static bool Capture(uint64_t bot, Part part, Baseline& out, bool worker = false)
    {
        PlayerbotBridge::PostMapJoin phase;
        if (!worker)
            return PlayerbotBridge::Capture(phase, bot, part, out);

        bool result = true;
        std::thread thread([&] { result = PlayerbotBridge::Capture(phase, bot, part, out); });
        thread.join();
        return result;
    }

    static RestoreResult Restore(uint64_t bot, Baseline const& before, Baseline const& applied, bool worker = false)
    {
        PlayerbotBridge::PostMapJoin phase;
        if (!worker)
            return PlayerbotBridge::Restore(phase, bot, before, applied);

        RestoreResult result = RestoreResult::Restored;
        std::thread thread([&] { result = PlayerbotBridge::Restore(phase, bot, before, applied); });
        thread.join();
        return result;
    }
};

namespace
{
Spell channeling;
using StrategyChanges = std::vector<std::pair<std::string, int>>;
// The strategy changes of playerbots' own "follow" and "stay" commands (ChatShortcutActions.cpp).
StrategyChanges const FollowStrategies{{"+follow,-passive,-grind,-move from group", BOT_STATE_NON_COMBAT},
                                       {"-stay,-follow,-passive,-grind,-move from group", BOT_STATE_COMBAT}};
StrategyChanges const StayStrategies{{"+stay,-passive,-move from group", BOT_STATE_NON_COMBAT},
                                     {"+stay,-follow,-passive,-move from group", BOT_STATE_COMBAT}};

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
    {
        std::cerr << "FAIL " << label << '\n';
        std::exit(1);
    }
}

// LC1a: what Fixture::Run saw per verb, for the ability table's checks at the end.
std::map<Intent, std::set<Refusal>> seenRefusals;
std::set<Intent> seenApplied;

struct Fixture
{
    Player Bot;
    Player Human;
    Player Other;
    Unit Target;
    Group Party;
    Group OtherParty;
    PlayerbotAI AI;
    PlayerbotAI HumanAI;
    WorldSession HumanSession;
    Order Command{Intent::Mark, 1, 2, 3, 4, 6, 30.0f};
    int Authorizations = 0;
    Refusal Why = Refusal::None;
    // LC2: a stepped order's outcomes and its BridgeAct events.
    std::vector<PlayerbotBridge::StepOutcome> Outcomes;
    int Events = 0;

    Fixture()
    {
        mutations = attacks = marks = follows = usefulFollow = stays = stops = flees = fleeRuns = moves = paths = 0;
        security = melee = followUseful = followResult = fleeResult = moveResult = healer = formationLoad = true;
        waitForAttack = prohibited = unavailable = moveFromGroup = fleeBackwards = false;
        fleeFrom = nullptr;
        fleeDistance = 0.0f;
        pathType = PATHFIND_NORMAL;
        pathLength = 12.0f;
        pathOwner = nullptr;
        Human.X = 20.0f;
        Bot.Guid = ObjectGuid(1);
        Human.Guid = ObjectGuid(2);
        Human.Session = &HumanSession;
        Target.Guid = ObjectGuid(4);
        Other.Guid = ObjectGuid(5);
        Bot.Party = Human.Party = &Party;
        Bot.Target = Target.Guid;
        Bot.AI = &AI;
        AI.Bot = &Bot;
        AI.Master = &Human;
        ObjectAccessor::Units = {{1, &Bot}, {2, &Human}, {4, &Target}, {5, &Other}};
        PlayerbotThreadCheck::worldThread = std::this_thread::get_id();
    }

    // A target-less verb with its wire shape (no target) and a bot that is moving; a flee has an opponent in melee; a
    // position is on the bot's map, about 11 yards from the bot at (0, 0, 0) and from the controller at (20, 0, 0).
    void OneShot(Intent operation)
    {
        Command.Operation = operation;
        Command.Target = 0;
        Bot.Moving = true;
        AI.Values.Moving.Data = true;
        Bot.State = UNIT_STATE_CHASE | UNIT_STATE_FOLLOW;
        if (operation == Intent::Flee)
            AI.Values.Current.Data = &Target;
        if (operation == Intent::Position)
        {
            Command.MapId = 1;
            Command.X = 10.0;
            Command.Y = -4.0;
            Command.Z = 2.0;
        }
        // The lasting verbs: a close formation, mana saving on, and focus healing on the third group member.
        if (operation == Intent::Formation)
            Command.Shape = FormationShape::Close;
        if (operation == Intent::SaveMana)
            Command.Enabled = true;
        if (operation == Intent::FocusHealing)
            Command.Target = Other.Guid.Value;
    }

    Baseline Captured(Part part)
    {
        Baseline baseline;
        Check(OllamaBotRandomChatter::Capture(Bot.Guid.Value, part, baseline) && baseline.Held == part,
              "capture of a live bot's part");
        return baseline;
    }

    Result Run()
    {
        // A value outside the enum: a result that leaves the reason unwritten fails the check below.
        Why = static_cast<Refusal>(99);
        int const events = bridgeActs;
        Result const result = OllamaBotRandomChatter::Run(
            Command,
            [&]
            {
                ++Authorizations;
                return true;
            },
            false, &Why);
        Check(result == Result::Rejected || Why == Refusal::None, "a result other than a refusal carries no reason");
        // LC1a: one BridgeAct event for each order the facts admit (applied, failed, or refused with a reason) and
        // none for an order refused before them.
        bool const decided = result == Result::Applied || result == Result::Failed ||
                             (result == Result::Rejected && Why != Refusal::None);
        Check(bridgeActs - events == (decided ? 1 : 0), "one BridgeAct event per decided order, none before the facts");
        if (decided)
        {
            AbilityRow const* row = PlayerbotBridge::FindRow(Command.Operation);
            Check(row && bridgeActType == "BridgeAct" && bridgeActSource == row->Name && bridgeActBot == &Bot &&
                      bridgeActId == static_cast<uint32>(Why) && bridgeActAmount == static_cast<int64>(result),
                  "the BridgeAct event names the bot, the verb, the refusal and the result");
        }
        if (result == Result::Applied)
            seenApplied.insert(Command.Operation);

        if (result == Result::Rejected && Why != Refusal::None)
            seenRefusals[Command.Operation].insert(Why);

        return result;
    }

    Result RunSteps(std::vector<Order> const& steps)
    {
        int const events = bridgeActs;
        Outcomes.assign(1, {Result::Applied, static_cast<Refusal>(99)});
        Result const result = OllamaBotRandomChatter::RunSteps(
            steps,
            [&]
            {
                ++Authorizations;
                return true;
            },
            &Outcomes);
        Events = bridgeActs - events;
        return result;
    }

    void Denied(Result expected = Result::Rejected, Refusal reason = Refusal::None)
    {
        Check(Run() == expected, "denial result");
        Check(Why == reason, "denial reason");
        Check(mutations == 0 && attacks == 0 && marks == 0 && follows == 0 && stays == 0 && stops == 0 && flees == 0 &&
                  fleeRuns == 0 && moves == 0,
              "denied no mutation");
    }

    // DS.14: a plausibility or safety refusal after the one admission, with the reason the caller words for the
    // partner, and no effect.
    void Refused(Refusal reason)
    {
        Denied(Result::Rejected, reason);
        Check(Authorizations == 1, "a plausibility refusal follows the one admission");
    }
};

// An ungrouped bot and partner: the order carries no group.
void Ungrouped(Fixture& f)
{
    f.Bot.Party = f.Human.Party = nullptr;
    f.Command.Group = 0;
}

// DS.14: who leads the bot, its security level and what the partner sees or where the partner stands never decide an
// order; only following needs the partner near (same map and instance).
std::vector<std::function<void(Fixture&)>> const NoLeaderGate = {[](Fixture& f) { f.AI.Master = &f.Other; },
                                                                 [](Fixture& f)
                                                                 {
                                                                     f.AI.Master = nullptr;
                                                                     f.Party.Leader = f.Other.Guid;
                                                                 },
                                                                 [](Fixture&) { security = false; },
                                                                 [](Fixture& f) { f.Human.Seen = false; },
                                                                 [](Fixture& f) { f.Bot.Seen = false; },
                                                                 [](Fixture& f) { f.Human.Phase = 2; },
                                                                 [](Fixture& f) { f.Human.Near = false; },
                                                                 [](Fixture& f) { f.Human.Hidden.insert(4); }};
}  // namespace

int main()
{
    {
        Fixture f;
        Check(f.Run() == Result::Applied, "mark applies");
        Check(marks == 1 && attacks == 0, "one mark attempt");
        Check(f.Party.Actor == f.Bot.Guid && f.Party.Marked == f.Target.Guid, "the bot marks as itself, exact target");
        Check(f.Party.Icons[6] == f.Target.Guid && f.Authorizations == 1, "exact icon and authorization");
    }
    {
        Fixture f;
        f.Command.Operation = Intent::AttackOwnTarget;
        f.Human.Target = f.Other.Guid;
        Check(f.Run() == Result::Applied, "attack applies");
        Check(attacks == 1 && marks == 0 && f.Bot.LastAttacked == &f.Target, "own target not human selection");
        Check(f.AI.Values.Priority.Get().empty() && !f.AI.Values.Pull.Get(), "no master priority or pull state");
        Check(f.Authorizations == 1, "one attack authorization");
    }
    for (Intent operation : {Intent::AttackOwnTarget, Intent::Mark})
    {
        Fixture headless;
        headless.Command.Operation = operation;
        headless.HumanSession.Headless = true;
        Check(!headless.Human.AI, "headless controller has no attached AI");
        headless.Denied();

        Fixture missingSession;
        missingSession.Command.Operation = operation;
        missingSession.Human.Session = nullptr;
        missingSession.Denied();
    }
    {
        // The headless test player's order applies like a real player's; any other headless session stays denied
        Fixture f;
        f.OneShot(Intent::Follow);
        f.HumanSession.Headless = true;
        TestPlayerMgr::Instance().Session = &f.HumanSession;
        Result const result = f.Run();
        TestPlayerMgr::Instance().Session = nullptr;
        Check(result == Result::Applied && follows == 1 && f.Authorizations == 1, "the test player's follow applies");

        Fixture other;
        other.OneShot(Intent::Follow);
        other.HumanSession.Headless = true;
        WorldSession testSession;
        testSession.Headless = true;
        TestPlayerMgr::Instance().Session = &testSession;
        other.Denied();
        TestPlayerMgr::Instance().Session = nullptr;
    }
    int unavailableVerbs = 0;
    for (int operation = 0; operation <= static_cast<int>(Intent::DungeonClear); ++operation)
    {
        Fixture f;
        f.Command.Operation = static_cast<Intent>(operation);
        if (f.Command.Operation != Intent::AttackOwnTarget && f.Command.Operation != Intent::Mark &&
            f.Command.Operation != Intent::Follow && f.Command.Operation != Intent::Stay &&
            f.Command.Operation != Intent::Flee && f.Command.Operation != Intent::Position &&
            f.Command.Operation != Intent::Formation && f.Command.Operation != Intent::SaveMana &&
            f.Command.Operation != Intent::FocusHealing)
        {
            f.Denied(Result::Unavailable);
            Check(f.Authorizations == 0, "unavailable before admission callback");
            ++unavailableVerbs;
        }
    }
    {
        // Regroup, pull, assign role and dungeon clear have no safe playerbots effect (DS.9) and stay refused.
        Check(unavailableVerbs == 4, "four verbs stay unavailable");
        for (Intent operation : {Intent::Regroup, Intent::Pull, Intent::AssignRole, Intent::DungeonClear})
        {
            Fixture f;
            f.OneShot(operation);
            f.Denied(Result::Unavailable);
            Check(f.Authorizations == 0 && PlayerbotBridge::LastingPart(operation) == Part::None,
                  "an unavailable verb is refused before admission and has no lasting part");
        }
    }
    std::vector<std::function<void(Fixture&)>> denials = {
        [](Fixture& f) { f.Command.Operation = static_cast<Intent>(255); },
        [](Fixture& f) { f.Command.Bot = 0; },
        [](Fixture& f) { f.Command.Controller = 0; },
        [](Fixture& f) { f.Command.Controller = 1; },
        [](Fixture& f) { f.Command.Group = 0; },
        [](Fixture& f) { f.Command.Target = 0; },
        [](Fixture& f) { f.Command.Icon = 8; },
        [](Fixture& f) { f.Command.VisibilityDistance = 0.0f; },
        [](Fixture& f) { f.Command.VisibilityDistance = std::numeric_limits<float>::infinity(); },
        [](Fixture& f) { f.Command.VisibilityDistance = std::numeric_limits<float>::quiet_NaN(); },
        [](Fixture&) { ObjectAccessor::Units.erase(1); },
        [](Fixture&) { ObjectAccessor::Units.erase(2); },
        [](Fixture& f) { f.Bot.World = false; },
        [](Fixture& f) { f.Bot.AI = nullptr; },
        [](Fixture& f) { f.AI.BotAI = false; },
        [](Fixture& f) { f.Human.AI = &f.HumanAI; },
        [](Fixture& f) { f.Human.Team = 1; },
        [](Fixture& f) { f.Bot.Party = nullptr; },
        [](Fixture& f) { f.Party.Guid = ObjectGuid(77); }};
    for (auto const& deny : denials)
    {
        Fixture f;
        deny(f);
        f.Denied();
    }
    std::vector<std::pair<std::function<void(Fixture&)>, Refusal>> const markRefusals = {
        {[](Fixture& f) { f.Bot.Alive = false; }, Refusal::Dead},
        {[](Fixture& f) { f.Bot.Battleground = true; }, Refusal::Battleground},
        {[](Fixture& f) { f.Bot.State = UNIT_STATE_IN_FLIGHT; }, Refusal::Travelling},
        {Ungrouped, Refusal::NotInGroup},
        {[](Fixture&) { ObjectAccessor::Units.erase(4); }, Refusal::TargetNotVisible},
        {[](Fixture& f) { f.Target.World = false; }, Refusal::TargetNotVisible},
        {[](Fixture& f) { f.Target.Map = 2; }, Refusal::TargetNotVisible},
        {[](Fixture& f) { f.Target.Phase = 2; }, Refusal::TargetNotVisible},
        {[](Fixture& f) { f.Target.Seen = false; }, Refusal::TargetNotVisible},
        {[](Fixture& f) { f.Target.Near = false; }, Refusal::TargetNotVisible},
        {[](Fixture& f)
         {
             f.Party.Raid = true;
             f.Party.Leader = f.Other.Guid;
         },
         Refusal::CannotNow},
        {[](Fixture& f)
         {
             f.Target.PlayerType = true;
             f.Target.Hostile = true;
         },
         Refusal::CannotNow}};
    for (auto const& [deny, reason] : markRefusals)
    {
        Fixture f;
        deny(f);
        f.Refused(reason);
    }
    // An attack or a mark is the bot's own act on what it sees; the partner may be elsewhere or in another group.
    for (Intent operation : {Intent::Mark, Intent::AttackOwnTarget})
    {
        std::vector<std::function<void(Fixture&)>> loosened = NoLeaderGate;
        loosened.push_back([](Fixture& f) { f.Human.World = false; });
        loosened.push_back([](Fixture& f) { f.Human.Map = 2; });
        loosened.push_back([](Fixture& f) { f.Human.Party = &f.OtherParty; });
        for (auto const& loosen : loosened)
        {
            Fixture f;
            f.Command.Operation = operation;
            loosen(f);
            Check(f.Run() == Result::Applied && f.Authorizations == 1, "an attack or mark without a leader's say");
            Check(operation == Intent::Mark ? marks == 1 && f.Party.Actor == f.Bot.Guid : attacks == 1 && marks == 0,
                  "the bot's own attack or mark");
        }
    }
    {
        Fixture f;
        f.Command.Operation = Intent::AttackOwnTarget;
        Ungrouped(f);
        Check(f.Run() == Result::Applied && attacks == 1, "an ungrouped bot attacks its own target");
    }
    {
        Fixture f;
        f.Human.Hidden.insert(f.Target.Guid.Value);
        f.Command.Operation = Intent::AttackOwnTarget;
        Check(f.Run() == Result::Applied && attacks == 1, "a target the bot sees and the partner does not");
    }
    {
        Fixture f;
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied, "a masterless bot marks");
    }
    {
        Fixture f;
        f.Command.Target = f.Other.Guid.Value;
        Check(f.Run() == Result::Applied && f.Party.Marked == f.Other.Guid, "connected player mark");
    }
    {
        Fixture f;
        f.Command.Target = f.Other.Guid.Value;
        f.Other.Hostile = true;
        f.Refused(Refusal::CannotNow);
    }
    {
        Fixture f;
        f.Party.Raid = true;
        f.Party.Leader = f.Other.Guid;
        f.Party.Assistant = f.Bot.Guid;
        Check(f.Run() == Result::Applied && f.Party.Actor == f.Bot.Guid, "a raid assistant bot marks as itself");
    }
    {
        Fixture f;
        f.Party.Raid = true;
        f.Party.Leader = f.Bot.Guid;
        Check(f.Run() == Result::Applied && f.Party.Actor == f.Bot.Guid, "a raid leader bot marks as itself");
    }
    {
        // The partner's own standing in the raid gives the bot none.
        Fixture f;
        f.Party.Raid = true;
        f.Refused(Refusal::CannotNow);
        Check(f.Party.Icons[6] == ObjectGuid(), "a raid led by the partner: the bot does not mark");
    }
    {
        Fixture f;
        f.Party.Raid = true;
        f.Party.Leader = f.Other.Guid;
        f.Party.Assistant = f.Human.Guid;
        f.Refused(Refusal::CannotNow);
    }
    {
        Fixture f;
        f.Party.MarkResult = false;
        Check(f.Run() == Result::Failed && marks == 1, "mark failure one attempt");
    }
    {
        Fixture f;
        PlayerbotThreadCheck::worldThread = std::thread::id();
        f.Denied();
        Check(f.Authorizations == 0, "startup no authority");
    }
    {
        Fixture f;
        Check(OllamaBotRandomChatter::Run(
                  f.Command, [] { return true; }, true) == Result::Rejected,
              "scope cannot cross threads");
        Check(mutations == 0, "wrong thread no effect");
    }
    {
        Fixture f;
        Check(OllamaBotRandomChatter::Run(f.Command, {}) == Result::Rejected, "missing admission callback");
        Check(OllamaBotRandomChatter::Run(f.Command, [] { return false; }) == Result::Rejected, "admission refusal");
        Check(mutations == 0, "admission no effect");
        Check(OllamaBotRandomChatter::Run(f.Command,
                                          [&]
                                          {
                                              f.Bot.Alive = false;
                                              return true;
                                          }) == Result::Rejected,
              "facts resolved after admission callback");
        Check(mutations == 0, "changed authorization no effect");
    }
    std::vector<std::pair<std::function<void(Fixture&)>, Refusal>> const attackDenials = {
        {[](Fixture& f) { f.Bot.Target = f.Other.Guid; }, Refusal::NoTarget},
        {[](Fixture& f) { f.Bot.Alive = false; }, Refusal::Dead},
        {[](Fixture& f) { f.Bot.Mounted = true; }, Refusal::CannotNow},
        {[](Fixture& f) { f.Bot.State = UNIT_STATE_IN_FLIGHT; }, Refusal::Travelling},
        {[](Fixture& f) { f.Bot.Battleground = true; }, Refusal::Battleground},
        {[](Fixture& f) { f.Target.Seen = false; }, Refusal::TargetNotVisible}};
    for (auto const& [deny, reason] : attackDenials)
    {
        Fixture f;
        f.Command.Operation = Intent::AttackOwnTarget;
        deny(f);
        f.Refused(reason);
    }
    std::vector<std::function<void(Fixture&)>> attackFailures = {[](Fixture& f)
                                                                 { f.Bot.MotionState.Type = FLIGHT_MOTION_TYPE; },
                                                                 [](Fixture& f)
                                                                 {
                                                                     f.Target.PlayerType = true;
                                                                     prohibited = true;
                                                                 },
                                                                 [](Fixture& f) { f.Target.Friendly = true; },
                                                                 [](Fixture& f) { f.Target.Alive = false; },
                                                                 [](Fixture&) { unavailable = true; },
                                                                 [](Fixture& f) { f.Target.Los = false; },
                                                                 [](Fixture& f)
                                                                 {
                                                                     f.AI.Vehicle = true;
                                                                     f.AI.Passenger = false;
                                                                 },
                                                                 [](Fixture& f) { f.Bot.Valid = false; },
                                                                 [](Fixture&) { waitForAttack = true; },
                                                                 [](Fixture& f)
                                                                 {
                                                                     f.AI.Values.Current.Data = &f.Target;
                                                                     f.Bot.Victim = &f.Target;
                                                                     f.Bot.State = UNIT_STATE_MELEE_ATTACKING;
                                                                     f.AI.State = BOT_STATE_COMBAT;
                                                                 }};
    for (auto const& fail : attackFailures)
    {
        Fixture f;
        f.Command.Operation = Intent::AttackOwnTarget;
        fail(f);
        f.Denied(Result::Failed);
    }
    {
        Fixture f;
        f.Command.Operation = Intent::AttackOwnTarget;
        f.Bot.AttackResult = false;
        Check(f.Run() == Result::Failed, "core failure is not reported applied");
        Check(attacks == 1 && mutations > 1, "partial failed attempt remains one attempt");
    }
    {
        Fixture f;
        waitForAttack = true;
        AttackAction action(&f.AI, "attack");
        f.AI.Values.Current.Data = &f.Target;
        Check(action.Execute(Event{}), "legacy wait semantics preserved");
        Check(attacks == 0 && mutations > 0, "legacy target selection retained");
    }
    {
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.Values.Positions.Data["stay"].valueSet = f.AI.Values.Positions.Data["return"].valueSet = true;
        f.AI.Values.Priority.Data = {f.Target.Guid};
        Check(f.Run() == Result::Applied, "follow applies");
        Check(follows == 1 && usefulFollow == 1 && stays == 0 && stops == 0 && attacks == 0 && marks == 0,
              "one follow move and nothing else");
        Check(f.Authorizations == 1 && f.Party.Marked == ObjectGuid() && f.Bot.Target == f.Target.Guid,
              "one follow authorization without target or selection writes");
        Check(f.AI.Strategies == FollowStrategies && f.AI.StrategyResets == 0 && f.AI.Resets == 0 &&
                  f.AI.Master == &f.Human,
              "the follow command's strategies for the master");
        Check(!f.AI.Values.Positions.Data["stay"].valueSet && !f.AI.Values.Positions.Data["return"].valueSet &&
                  f.AI.Values.Priority.Data.empty(),
              "follow clears the stay and return positions and the prioritized targets");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        f.Bot.X = 3.0f;
        f.Bot.Y = -2.0f;
        f.Bot.Z = 1.0f;
        Check(f.Run() == Result::Applied, "stay applies");
        Check(stays == 1 && stops == 1 && follows == 0 && usefulFollow == 0 && attacks == 0 && marks == 0,
              "one stop and nothing else");
        Check(!f.Bot.Moving && !f.Bot.HasUnitState(UNIT_STATE_CHASE | UNIT_STATE_FOLLOW) &&
                  f.AI.Values.StayTime.Data != 0 && f.AI.Values.MovingParam.empty(),
              "stay stops chase and follow once and records its stay time");
        Check(f.AI.Resets == 1 && f.AI.Strategies == StayStrategies && f.AI.StrategyResets == 0 &&
                  f.AI.Master == &f.Human,
              "the stay command's reset and strategies");
        for (char const* name : {"stay", "return"})
        {
            PositionInfo const& position = f.AI.Values.Positions.Data[name];
            Check(position.valueSet && position.x == 3.0f && position.y == -2.0f && position.z == 1.0f &&
                      position.mapId == 1,
                  "stay and return positions where the bot stands");
        }
        Check(f.Authorizations == 1, "one stay authorization");
    }
    {
        Fixture f;
        f.OneShot(Intent::Flee);
        Check(f.Run() == Result::Applied, "flee applies");
        Check(flees == 1 && fleeRuns == 1 && fleeFrom == &f.Target && fleeBackwards &&
                  fleeDistance == sPlayerbotAIConfig.FleeDistance && mutations == 1,
              "one backwards move away from the opponent at the configured flee distance");
        Check(follows == 0 && usefulFollow == 0 && stays == 0 && stops == 0 && attacks == 0 && marks == 0 &&
                  f.Authorizations == 1 && f.Bot.Target == f.Target.Guid && f.AI.Values.Current.Data == &f.Target,
              "one flee authorization without target or selection writes");
    }
    {
        Fixture f;
        f.OneShot(Intent::Flee);
        f.AI.Values.Current.Data = &f.Other;
        Check(f.Run() == Result::Applied && flees == 1 && fleeFrom == &f.Other,
              "flee uses the bot's current target value, not its selection");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        Check(f.Run() == Result::Applied, "position applies");
        Check(moves == 1 && moveMap == 1 && moveX == 10.0f && moveY == -4.0f && moveZ == 2.0f && mutations == 1,
              "one move to the exact position");
        Check(!moveIdle && !moveReact && moveNormalOnly && moveExact, "an exact waypoint over normal paths, not idle");
        Check(paths == 1 && pathOwner == static_cast<Unit const*>(&f.Bot) && pathX == 10.0f && pathY == -4.0f &&
                  pathZ == 2.0f && !pathForce,
              "one complete-path query from the bot to that position");
        Check(follows == 0 && stays == 0 && stops == 0 && flees == 0 && attacks == 0 && marks == 0 &&
                  f.Authorizations == 1 && f.AI.Values.Positions.Data.empty() && f.Bot.Target == f.Target.Guid,
              "one position authorization without named position, target or selection writes");
    }
    for (Intent operation : {Intent::Follow, Intent::Stay, Intent::Flee, Intent::Position, Intent::Formation,
                             Intent::SaveMana, Intent::FocusHealing})
    {
        // No reason for the partner: a malformed or stale order, a partner who is no player, or the other side.
        std::vector<std::function<void(Fixture&)>> oneShotDenials = {
            [](Fixture& f) { f.Command.Target = f.Target.Guid.Value; },
            [](Fixture& f) { f.HumanSession.Headless = true; },
            [](Fixture& f) { f.Human.Session = nullptr; },
            [](Fixture& f) { f.Command.Bot = 0; },
            [](Fixture& f) { f.Command.Controller = 0; },
            [](Fixture& f) { f.Command.Controller = 1; },
            [](Fixture& f) { f.Command.Group = 0; },
            [](Fixture& f) { f.Command.VisibilityDistance = std::numeric_limits<float>::quiet_NaN(); },
            [](Fixture&) { ObjectAccessor::Units.erase(1); },
            [](Fixture&) { ObjectAccessor::Units.erase(2); },
            [](Fixture& f) { f.Bot.World = false; },
            [](Fixture& f) { f.Bot.AI = nullptr; },
            [](Fixture& f) { f.AI.BotAI = false; },
            [](Fixture& f) { f.Human.AI = &f.HumanAI; },
            [](Fixture& f) { f.Human.Team = 1; },
            [](Fixture& f) { f.Bot.Party = nullptr; },
            [](Fixture& f) { f.Party.Guid = ObjectGuid(77); }};
        // The common plausibility and safety rules, then each verb's own.
        std::vector<std::pair<std::function<void(Fixture&)>, Refusal>> refusals = {
            {[](Fixture& f) { f.Bot.Alive = false; }, Refusal::Dead},
            {[](Fixture& f) { f.Bot.Battleground = true; }, Refusal::Battleground},
            {[](Fixture& f) { f.Bot.State |= UNIT_STATE_IN_FLIGHT; }, Refusal::Travelling}};
        std::vector<std::function<void(Fixture&)>> freed = NoLeaderGate;
        if (operation == Intent::Follow)
        {
            refusals.push_back({[](Fixture& f) { f.Human.Party = &f.OtherParty; }, Refusal::NotWithYou});
            refusals.push_back({[](Fixture& f) { f.Party.Member = false; }, Refusal::NotWithYou});
            refusals.push_back({Ungrouped, Refusal::NotWithYou});
            refusals.push_back({[](Fixture& f) { f.Human.Map = 2; }, Refusal::TooFar});
            refusals.push_back({[](Fixture& f) { f.Human.World = false; }, Refusal::TooFar});
        }
        else
        {
            freed.push_back([](Fixture& f) { f.Human.Map = 2; });
            freed.push_back([](Fixture& f) { f.Human.World = false; });
        }
        if (operation == Intent::Formation || operation == Intent::SaveMana)
        {
            refusals.push_back({[](Fixture& f) { f.Human.Party = &f.OtherParty; }, Refusal::NotInGroup});
            refusals.push_back({[](Fixture& f) { f.Party.Member = false; }, Refusal::NotInGroup});
            refusals.push_back({Ungrouped, Refusal::NotInGroup});
        }
        else if (operation != Intent::Follow)
            freed.push_back([](Fixture& f) { f.Human.Party = &f.OtherParty; });
        if (operation == Intent::Stay || operation == Intent::Flee || operation == Intent::Position)
            freed.push_back(Ungrouped);
        if (operation == Intent::FocusHealing)
        {
            oneShotDenials.erase(oneShotDenials.begin());
            oneShotDenials.push_back([](Fixture& f) { f.Command.Target = 0; });
            refusals.push_back({Ungrouped, Refusal::NotInGroup});
            refusals.push_back({[](Fixture&) { ObjectAccessor::Units.erase(5); }, Refusal::TargetNotVisible});
            refusals.push_back({[](Fixture& f) { f.Other.World = false; }, Refusal::TargetNotVisible});
            refusals.push_back({[](Fixture& f) { f.Other.Map = 2; }, Refusal::TargetNotVisible});
            refusals.push_back({[](Fixture& f) { f.Other.Phase = 2; }, Refusal::TargetNotVisible});
            refusals.push_back({[](Fixture& f) { f.Other.Seen = false; }, Refusal::TargetNotVisible});
            refusals.push_back({[](Fixture& f) { f.Other.Near = false; }, Refusal::TargetNotVisible});
            refusals.push_back({[](Fixture& f) { f.Party.Outsider = f.Other.Guid; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture& f) { f.Command.Target = f.Target.Guid.Value; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture&) { healer = false; }, Refusal::CannotNow});
        }
        else if (operation == Intent::Flee)
        {
            refusals.push_back({[](Fixture& f) { f.AI.Values.Current.Data = nullptr; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture& f) { f.Target.Alive = false; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture& f) { f.Target.World = false; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture& f) { f.Target.Map = 2; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture&) { melee = false; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture& f) { f.Bot.Channeled = &channeling; }, Refusal::CannotNow});
            refusals.push_back({[](Fixture& f) { f.Bot.Mounted = true; }, Refusal::CannotNow});
        }
        else if (operation == Intent::Position)
        {
            refusals.push_back({[](Fixture& f) { f.Command.MapId = 2; }, Refusal::Unreachable});
            refusals.push_back({[](Fixture& f) { f.Command.X = 45.0; }, Refusal::Unreachable});
            refusals.push_back({[](Fixture&) { pathType = PATHFIND_NOPATH; }, Refusal::Unreachable});
            refusals.push_back({[](Fixture&) { pathLength = 60.5f; }, Refusal::Unreachable});
        }
        for (auto const& [deny, reason] : refusals)
        {
            Fixture f;
            f.OneShot(operation);
            deny(f);
            f.Refused(reason);
        }
        for (auto const& loosen : freed)
        {
            Fixture f;
            f.OneShot(operation);
            loosen(f);
            Check(f.Run() == Result::Applied && f.Authorizations == 1, "an order without a leader's say applies");
        }
        for (auto const& deny : oneShotDenials)
        {
            Fixture f;
            f.OneShot(operation);
            deny(f);
            f.Denied();
        }
        {
            Fixture f;
            f.OneShot(operation);
            Check(OllamaBotRandomChatter::Run(f.Command, {}) == Result::Rejected, "one-shot missing callback");
            Check(OllamaBotRandomChatter::Run(f.Command, [] { return false; }) == Result::Rejected,
                  "one-shot admission refusal");
            Check(OllamaBotRandomChatter::Run(f.Command,
                                              [&]
                                              {
                                                  f.Bot.Alive = false;
                                                  return true;
                                              }) == Result::Rejected,
                  "one-shot facts resolved after admission callback");
            Check(OllamaBotRandomChatter::Run(
                      f.Command, [] { return true; }, true) == Result::Rejected,
                  "one-shot scope cannot cross threads");
            Check(mutations == 0 && follows == 0 && stays == 0 && usefulFollow == 0 && flees == 0 && fleeRuns == 0 &&
                      moves == 0,
                  "one-shot refusals no effect");
        }
        {
            Fixture f;
            f.OneShot(operation);
            PlayerbotThreadCheck::worldThread = std::thread::id();
            f.Denied();
            Check(f.Authorizations == 0, "one-shot startup no authority");
        }
    }
    {
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied && follows == 1 && f.Authorizations == 1,
              "the group leader may make a masterless bot follow");
        Check(f.AI.Master == &f.Human && f.AI.StrategyResets == 1 && f.AI.Strategies == FollowStrategies,
              "a masterless bot takes the leader as master, then the follow strategies");
    }
    {
        // DS.14: a partner who does not lead the group. A masterless grouped bot takes the partner as master.
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.Master = nullptr;
        f.Party.Leader = f.Other.Guid;
        Check(f.Run() == Result::Applied && follows == 1 && f.AI.Master == &f.Human && f.AI.StrategyResets == 1 &&
                  f.AI.Strategies == FollowStrategies,
              "a masterless grouped bot follows a partner who does not lead");
    }
    {
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.Master = &f.Other;
        f.Party.Leader = f.Other.Guid;
        Check(f.Run() == Result::Applied && follows == 1 && f.AI.Master == &f.Other && f.AI.StrategyResets == 0 &&
                  f.AI.Strategies == FollowStrategies,
              "a bot with a master keeps it when a partner who does not lead asks it to follow");
    }
    {
        // An ungrouped bot never follows and never takes a master; it may stay for a partner far away.
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.Master = nullptr;
        Ungrouped(f);
        f.Refused(Refusal::NotWithYou);
        Check(!f.AI.Master && f.AI.StrategyResets == 0 && f.AI.Strategies.empty(), "an ungrouped bot takes no master");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        f.AI.Master = nullptr;
        Ungrouped(f);
        f.Human.Map = 2;
        f.Human.Near = f.Human.Seen = false;
        Check(f.Run() == Result::Applied && stays == 1 && !f.AI.Master && f.AI.Strategies == StayStrategies,
              "an ungrouped bot stays when a partner far away asks");
    }
    {
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.Master = nullptr;
        f.Bot.Battleground = true;
        f.Refused(Refusal::Battleground);
        Check(f.Authorizations == 1 && !f.AI.Master && f.AI.Strategies.empty() && f.AI.StrategyResets == 0,
              "a masterless battleground bot keeps playerbots' own master choice");
    }
    {
        Fixture f;
        f.OneShot(Intent::Follow);
        followUseful = false;
        Check(f.Run() == Result::Applied && follows == 0 && usefulFollow == 1 && f.AI.Strategies == FollowStrategies,
              "a bot already at its place follows from here on without a move");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied && stays == 1 && !f.AI.Master && f.AI.Strategies == StayStrategies,
              "the group leader may make a masterless bot stay");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        f.Bot.Moving = false;
        f.AI.Values.Moving.Data = false;
        f.Bot.State = 0;
        Check(f.Run() == Result::Applied && stays == 1 && stops == 0 && f.AI.Resets == 1 &&
                  f.AI.Strategies == StayStrategies,
              "a standing bot told to stay stays");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        moveFromGroup = true;
        f.AI.Values.Positions.Data["stay"].valueSet = true;
        Check(f.Run() == Result::Applied && stops == 1 && f.AI.Strategies == StayStrategies,
              "stay replaces move from group and an old stay position");
    }
    {
        Fixture f;
        f.OneShot(Intent::Flee);
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied && flees == 1, "the group leader may make a masterless bot flee once");
    }
    {
        Fixture f;
        f.OneShot(Intent::Flee);
        f.AI.Values.Current.Data = nullptr;
        f.Refused(Refusal::CannotNow);
        Check(f.Authorizations == 1, "a flee without an opponent is refused after admission, never a failed attempt");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        Check(OllamaBotRandomChatter::Reachable(f.Command) && paths == 1 && mutations == 0 && f.Authorizations == 0,
              "the reachability fact is one read-only path query");
        f.Command.X = 30.0;
        f.Command.Y = f.Command.Z = 0.0;
        pathLength = 60.0f;
        Check(OllamaBotRandomChatter::Reachable(f.Command), "exactly at the radius with a path of twice the radius");
        Check(f.Run() == Result::Applied && moves == 1 && moveX == 30.0f, "a boundary position applies");
    }
    std::vector<std::pair<std::function<void(Fixture&)>, Refusal>> const remote = {
        {[](Fixture& f) { f.Command.MapId = 2; }, Refusal::Unreachable},
        {[](Fixture& f) { f.Command.X = std::numeric_limits<double>::quiet_NaN(); }, Refusal::None},
        {[](Fixture& f) { f.Command.Y = std::numeric_limits<double>::infinity(); }, Refusal::None},
        {[](Fixture& f) { f.Command.Z = -std::numeric_limits<double>::infinity(); }, Refusal::None},
        {[](Fixture& f) { f.Command.X = 1e300; }, Refusal::Unreachable},
        {[](Fixture& f) { f.Command.Y = -std::numeric_limits<double>::max(); }, Refusal::Unreachable},
        {[](Fixture& f) { f.Command.X = 45.0; }, Refusal::Unreachable},
        {[](Fixture& f) { f.Command.Z = 31.0; }, Refusal::Unreachable},
        {[](Fixture& f) { f.Command.VisibilityDistance = 0.0f; }, Refusal::None},
        {[](Fixture& f) { f.Command.VisibilityDistance = std::numeric_limits<float>::quiet_NaN(); }, Refusal::None},
        {[](Fixture&) { ObjectAccessor::Units.erase(1); }, Refusal::None},
        {[](Fixture& f) { f.Bot.World = false; }, Refusal::None}};
    for (auto const& [deny, reason] : remote)
    {
        Fixture f;
        f.OneShot(Intent::Position);
        deny(f);
        Check(!OllamaBotRandomChatter::Reachable(f.Command) && paths == 0 && mutations == 0,
              "an unchecked position is refused before any path query");
        f.Denied(Result::Rejected, reason);
        Check(paths == 0, "no path query for an order refused on its facts");
    }
    // DS.14: the partner plays no part in a position: far away, out of the world or offline, or the spot 35 yards
    // from the partner and 15 from the bot.
    for (auto const& loosen :
         std::vector<std::function<void(Fixture&)>>{
             [](Fixture& f) { f.Human.Map = 2; }, [](Fixture& f) { f.Human.World = false; },
             [](Fixture&) { ObjectAccessor::Units.erase(2); }, [](Fixture& f) { f.Command.X = -15.0; }})
    {
        Fixture f;
        f.OneShot(Intent::Position);
        loosen(f);
        Check(OllamaBotRandomChatter::Reachable(f.Command) && paths == 1, "the reachability fact ignores the partner");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        f.Command.X = -15.0;
        Check(f.Run() == Result::Applied && moves == 1 && moveX == -15.0f, "a spot near the bot, far from the partner");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        ObjectAccessor::Units.erase(2);
        f.Denied();
        Check(paths == 0, "an offline partner's position is refused before any path query");
    }
    std::vector<PathType> const incomplete = {PATHFIND_BLANK,
                                              PATHFIND_SHORTCUT,
                                              PATHFIND_INCOMPLETE,
                                              PATHFIND_NOPATH,
                                              PATHFIND_NOT_USING_PATH,
                                              static_cast<PathType>(PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH),
                                              static_cast<PathType>(PATHFIND_NORMAL | PATHFIND_SHORT),
                                              static_cast<PathType>(PATHFIND_NORMAL | PATHFIND_FARFROMPOLY_START),
                                              static_cast<PathType>(PATHFIND_NORMAL | PATHFIND_FARFROMPOLY_END)};
    for (PathType type : incomplete)
    {
        Fixture f;
        f.OneShot(Intent::Position);
        pathType = type;
        Check(!OllamaBotRandomChatter::Reachable(f.Command) && paths == 1 && mutations == 0,
              "only a complete normal path is reachable");
        f.Refused(Refusal::Unreachable);
        Check(paths == 2 && f.Authorizations == 1, "an incomplete path is refused after one admission");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        pathLength = 60.5f;
        Check(!OllamaBotRandomChatter::Reachable(f.Command) && paths == 1, "a detour longer than twice the radius");
        Check(!OllamaBotRandomChatter::Reachable(f.Command, true) && paths == 1, "no path query off the world thread");
        PlayerbotThreadCheck::worldThread = std::thread::id();
        pathLength = 12.0f;
        Check(!OllamaBotRandomChatter::Reachable(f.Command) && paths == 1, "no path query during startup");
    }
    for (double Order::* coordinate : {&Order::X, &Order::Y, &Order::Z})
    {
        Fixture f;
        f.OneShot(Intent::Position);
        f.Command.*coordinate = std::numeric_limits<double>::quiet_NaN();
        f.Denied();
        Check(f.Authorizations == 0 && paths == 0, "a non-finite position is refused before admission");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        Check(OllamaBotRandomChatter::Reachable(f.Command), "the fact held when the adapter checked it");
        pathType = PATHFIND_NOPATH;
        f.Refused(Refusal::Unreachable);
        Check(f.Authorizations == 1 && paths == 2, "a stale reachability fact is checked again before the move");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        Check(OllamaBotRandomChatter::Run(f.Command,
                                          [&]
                                          {
                                              pathType = PATHFIND_INCOMPLETE;
                                              return true;
                                          }) == Result::Rejected &&
                  paths == 1 && moves == 0 && mutations == 0,
              "pathing is read after the admission callback");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied && moves == 1, "the group leader may move a masterless bot once");
    }
    {
        Fixture f;
        f.OneShot(Intent::Position);
        moveResult = false;
        Check(f.Run() == Result::Failed && moves == 1 && paths == 1 && f.Authorizations == 1,
              "position failure one attempt");
    }
    {
        Fixture f;
        f.OneShot(Intent::Follow);
        followResult = false;
        Check(f.Run() == Result::Failed && follows == 1 && f.Authorizations == 1, "follow failure one attempt");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        f.Bot.MotionState.Type = FLIGHT_MOTION_TYPE;
        Check(f.Run() == Result::Failed && stays == 1 && stops == 0 && f.Authorizations == 1 && f.AI.Resets == 0 &&
                  f.AI.Strategies.empty() && f.AI.Values.Positions.Data.empty(),
              "stay failure one attempt");
    }
    {
        Fixture f;
        f.OneShot(Intent::Flee);
        fleeResult = false;
        Check(f.Run() == Result::Failed && flees == 1 && fleeRuns == 0 && f.Authorizations == 1,
              "flee failure one attempt");
    }
    // J12: the lasting verbs and their baselines.
    {
        Check(PlayerbotBridge::LastingPart(Intent::Follow) == Part::Movement &&
                  PlayerbotBridge::LastingPart(Intent::Stay) == Part::Movement &&
                  PlayerbotBridge::LastingPart(Intent::Formation) == Part::Formation &&
                  PlayerbotBridge::LastingPart(Intent::SaveMana) == Part::ManaSave &&
                  PlayerbotBridge::LastingPart(Intent::FocusHealing) == Part::FocusHeal,
              "follow and stay share the movement part; each setting verb has its own");
        for (Intent operation : {Intent::Flee, Intent::Position, Intent::AttackOwnTarget, Intent::Mark})
            Check(PlayerbotBridge::LastingPart(operation) == Part::None, "a one-shot verb has no lasting part");
    }
    for (auto const& [shape, name] : std::vector<std::pair<FormationShape, std::string>>{
             {FormationShape::Close, "near"}, {FormationShape::Spread, "far"}, {FormationShape::Line, "line"}})
    {
        Fixture f;
        f.OneShot(Intent::Formation);
        f.Command.Shape = shape;
        Check(f.Run() == Result::Applied && f.AI.Values.Formation.Name == name && f.AI.Values.Formation.Loads == 1,
              "the formation shape sets its playerbots formation");
        Check(mutations == 1 && f.Authorizations == 1 && f.AI.Strategies.empty() && follows == 0 && stays == 0 &&
                  moves == 0 && attacks == 0 && marks == 0,
              "a formation changes only the formation value");
    }
    {
        Fixture f;
        f.OneShot(Intent::Formation);
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied && f.AI.Values.Formation.Name == "near", "a masterless bot's formation");
    }
    {
        Fixture f;
        f.OneShot(Intent::Formation);
        formationLoad = false;
        Check(f.Run() == Result::Failed && f.AI.Values.Formation.Name == "chaos" && f.Authorizations == 1,
              "a refused formation is a failed attempt");
    }
    for (auto const& [cls, level] : std::vector<std::pair<uint8_t, double>>{{CLASS_HUNTER, 5.0},
                                                                            {CLASS_SHAMAN, 5.0},
                                                                            {CLASS_DRUID, 5.0},
                                                                            {CLASS_MAGE, 2.0},
                                                                            {CLASS_PRIEST, 2.0},
                                                                            {CLASS_WARLOCK, 2.0},
                                                                            {CLASS_WARRIOR, 3.0}})
    {
        Fixture f;
        f.OneShot(Intent::SaveMana);
        f.Bot.Class = cls;
        Check(f.Run() == Result::Applied && f.AI.Values.ManaSave.Data == level && mutations == 1 &&
                  f.Authorizations == 1 && f.AI.Strategies.empty(),
              "save mana on sets the class level of the save mana command");
    }
    {
        Fixture f;
        f.OneShot(Intent::SaveMana);
        f.Command.Enabled = false;
        f.AI.Values.ManaSave.Data = 5.0;
        Check(f.Run() == Result::Applied && f.AI.Values.ManaSave.Data == 1.0 && mutations == 1,
              "save mana off spends freely");
    }
    {
        Fixture f;
        f.OneShot(Intent::FocusHealing);
        f.AI.Values.FocusHeal.Data = {f.Human.Guid, f.Target.Guid};
        Check(f.Run() == Result::Applied && f.AI.Values.FocusHeal.Data == std::list<ObjectGuid>{f.Other.Guid},
              "focus healing makes the target the one focus heal target");
        Check(f.AI.Strategies == StrategyChanges{{"+focus heal targets", BOT_STATE_COMBAT}} && f.Authorizations == 1 &&
                  attacks == 0 && marks == 0 && f.Bot.Target == f.Target.Guid,
              "with the combat focus heal strategy and no target or selection writes");
        Check(f.Run() == Result::Applied && f.AI.Values.FocusHeal.Data.size() == 1,
              "a repeated focus heal replaces, never stacks");
    }
    {
        Fixture f;
        f.OneShot(Intent::FocusHealing);
        healer = false;
        f.AI.Active.emplace("offheal", BOT_STATE_COMBAT);
        Check(f.Run() == Result::Applied, "an off-healer takes a focus heal target");
        Fixture self;
        self.OneShot(Intent::FocusHealing);
        self.Command.Target = self.Human.Guid.Value;
        self.AI.Master = nullptr;
        Check(self.Run() == Result::Applied && self.AI.Values.FocusHeal.Data == std::list<ObjectGuid>{self.Human.Guid},
              "the leader may name itself for a masterless healer");
    }
    {
        Fixture f;
        f.OneShot(Intent::FocusHealing);
        f.AI.Values.Missing = "focus heal targets";
        Check(f.Run() == Result::Failed && f.AI.Strategies.empty(), "a missing value is a failed attempt");
    }
    for (Intent operation : {Intent::Follow, Intent::Stay, Intent::Flee, Intent::Position, Intent::Mark,
                             Intent::AttackOwnTarget, Intent::FocusHealing, Intent::SaveMana, Intent::Formation})
    {
        for (bool shape : {true, false})
        {
            if ((shape && operation == Intent::Formation) || (!shape && operation == Intent::SaveMana))
                continue;

            Fixture f;
            f.OneShot(operation);
            if (operation == Intent::Mark || operation == Intent::AttackOwnTarget)
                f.Command.Target = f.Target.Guid.Value;
            if (shape)
                f.Command.Shape = FormationShape::Line;
            else
                f.Command.Enabled = true;
            f.Denied();
            Check(f.Authorizations == 0, "a stray shape or enable flag is refused before admission");
        }
    }
    for (FormationShape shape :
         {FormationShape::None, static_cast<FormationShape>(4), static_cast<FormationShape>(255)})
    {
        Fixture f;
        f.OneShot(Intent::Formation);
        f.Command.Shape = shape;
        f.Denied();
        Check(f.Authorizations == 0, "a formation without a known shape is refused before admission");
    }
    {
        Fixture f;
        f.OneShot(Intent::SaveMana);
        f.Command.Target = f.Other.Guid.Value;
        f.Denied();
        Check(f.Authorizations == 0, "a stray target on a setting verb is refused before admission");
    }
    {
        // Formation: baseline, apply, repeat, restore, second restore.
        Fixture f;
        f.OneShot(Intent::Formation);
        Baseline const before = f.Captured(Part::Formation);
        Check(before.Formation == "chaos" && mutations == 0 && f.AI.Values.Formation.Loads == 0,
              "capture reads the formation without a write");
        Check(f.Run() == Result::Applied, "the first formation applies");
        f.Command.Shape = FormationShape::Line;
        Check(f.Run() == Result::Applied && f.AI.Values.Formation.Name == "line", "a repeat replaces the formation");
        Baseline const applied = f.Captured(Part::Formation);
        Check(applied.Formation == "line" && before.Formation == "chaos", "the baseline is not recaptured");
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Values.Formation.Name == "chaos",
              "restore puts the formation back");
        int const writes = mutations;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  mutations == writes && f.AI.Values.Formation.Name == "chaos",
              "a second restore writes nothing");
    }
    {
        Fixture f;
        f.OneShot(Intent::Formation);
        Baseline const before = f.Captured(Part::Formation);
        Check(f.Run() == Result::Applied, "formation applies before a player's change");
        Baseline const applied = f.Captured(Part::Formation);
        f.AI.Values.Formation.Name = "queue";
        int const writes = mutations;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Superseded &&
                  f.AI.Values.Formation.Name == "queue" && mutations == writes,
              "a formation changed after the bridge stays as the later command left it");
        f.AI.Values.Formation.Name = "near";
        formationLoad = false;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Failed,
              "a refused formation restore is reported failed");
        f.AI.Values.Missing = "formation";
        Baseline missing;
        missing.Held = Part::ManaSave;
        Check(!OllamaBotRandomChatter::Capture(f.Bot.Guid.Value, Part::Formation, missing) && missing == Baseline() &&
                  OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Failed,
              "a missing value fails capture and restore");
    }
    {
        // Mana: baseline, apply, restore; a later change wins.
        Fixture f;
        f.OneShot(Intent::SaveMana);
        f.AI.Values.ManaSave.Data = 1.5;
        Baseline const before = f.Captured(Part::ManaSave);
        Check(f.Run() == Result::Applied && f.AI.Values.ManaSave.Data == 5.0, "save mana applies");
        Baseline const applied = f.Captured(Part::ManaSave);
        Check(before.ManaSaveLevel == 1.5 && applied.ManaSaveLevel == 5.0, "mana levels captured");
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Values.ManaSave.Data == 1.5,
              "restore puts the mana save level back");
        Check(f.Run() == Result::Applied, "save mana applies again");
        f.AI.Values.ManaSave.Data = 4.0;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Superseded &&
                  f.AI.Values.ManaSave.Data == 4.0,
              "a mana level changed after the bridge stays");
    }
    {
        // Focus heal from none: the list empties and the strategy goes off again.
        Fixture f;
        f.OneShot(Intent::FocusHealing);
        Baseline const before = f.Captured(Part::FocusHeal);
        Check(f.Run() == Result::Applied, "focus heal applies from none");
        Baseline const applied = f.Captured(Part::FocusHeal);
        Check(before.FocusHealTargets.empty() && !before.FocusHealStrategy &&
                  applied.FocusHealTargets == std::vector<uint64_t>{f.Other.Guid.Value} && applied.FocusHealStrategy,
              "focus heal lists and strategy captured");
        f.AI.Strategies.clear();
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Values.FocusHeal.Data.empty() && !f.AI.HasStrategy("focus heal targets", BOT_STATE_COMBAT) &&
                  f.AI.Strategies == StrategyChanges{{"-focus heal targets", BOT_STATE_COMBAT}},
              "restore empties the list and turns the strategy off");
        f.AI.Values.FocusHeal.Data = {f.Human.Guid};
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Superseded &&
                  f.AI.Values.FocusHeal.Data == std::list<ObjectGuid>{f.Human.Guid},
              "a focus heal list changed after the bridge stays");
    }
    {
        // Focus heal over an existing list: the old list comes back, the strategy stays on without a change.
        Fixture f;
        f.OneShot(Intent::FocusHealing);
        f.AI.Values.FocusHeal.Data = {f.Human.Guid};
        f.AI.ChangeStrategy("+focus heal targets", BOT_STATE_COMBAT);
        Baseline const before = f.Captured(Part::FocusHeal);
        Check(f.Run() == Result::Applied, "focus heal applies over a list");
        Baseline const applied = f.Captured(Part::FocusHeal);
        f.AI.Strategies.clear();
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Values.FocusHeal.Data == std::list<ObjectGuid>{f.Human.Guid} && f.AI.Strategies.empty() &&
                  f.AI.HasStrategy("focus heal targets", BOT_STATE_COMBAT),
              "restore brings the old list back and leaves the strategy");
    }
    {
        // Movement: a following bot told to stay goes back to following, without the stay spots.
        Fixture f;
        f.OneShot(Intent::Stay);
        f.AI.ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
        Baseline const before = f.Captured(Part::Movement);
        Check(f.Run() == Result::Applied, "stay applies over follow");
        Baseline const applied = f.Captured(Part::Movement);
        Check(applied.Stay.Set && applied.Return.Set && !before.Stay.Set && before.Strategies != applied.Strategies,
              "the stay strategies and spots captured");
        f.AI.Values.Positions.Data["return"].Set(7.0f, 7.0f, 7.0f, 1);
        f.AI.Values.Positions.Data["stay"].Set(8.0f, 8.0f, 8.0f, 1);
        Check(f.Captured(Part::Movement) == applied, "spots playerbots moved keep the stay the bridge's (ledger rule)");
        f.AI.Strategies.clear();
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored,
              "restore after stay, even when the stay and return spots moved");
        Check(
            f.AI.Strategies == StrategyChanges{{"+follow,-stay", BOT_STATE_NON_COMBAT}, {"-stay", BOT_STATE_COMBAT}} &&
                f.AI.Active == std::set<std::pair<std::string, int>>{{"follow", BOT_STATE_NON_COMBAT}},
            "follow comes back (stay had dropped its sibling) and the stay strategies come off");
        Check(!f.AI.Values.Positions.Data["stay"].valueSet && !f.AI.Values.Positions.Data["return"].valueSet,
              "and the stay and return spots are cleared");
        int const writes = mutations;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  mutations == writes,
              "a second movement restore writes nothing");
    }
    {
        // Movement: a passive bot staying at a spot, told to follow, gets its stay, spot and passive back.
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.ChangeStrategy("+stay,+passive", BOT_STATE_NON_COMBAT);
        f.AI.ChangeStrategy("+stay,+passive", BOT_STATE_COMBAT);
        f.AI.Values.Positions.Data["stay"].Set(4.0f, 5.0f, 6.0f, 1);
        Baseline const before = f.Captured(Part::Movement);
        Check(f.Run() == Result::Applied, "follow applies over stay");
        Baseline const applied = f.Captured(Part::Movement);
        f.AI.Strategies.clear();
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Strategies == StrategyChanges{{"-follow,+stay,+passive", BOT_STATE_NON_COMBAT},
                                                     {"+stay,+passive", BOT_STATE_COMBAT}},
              "restore turns follow off and stay and passive on in both states (follow had dropped its sibling stay)");
        PositionInfo const& stay = f.AI.Values.Positions.Data["stay"];
        Check(stay.valueSet && stay.x == 4.0f && stay.y == 5.0f && stay.z == 6.0f && stay.mapId == 1 &&
                  !f.AI.Values.Positions.Data["return"].valueSet,
              "and the old stay spot comes back");
    }
    {
        // Movement: a bot a person set to run away (the "runaway" command), told to stay, runs away again.
        Fixture f;
        f.OneShot(Intent::Stay);
        f.AI.ChangeStrategy("+runaway,-stay", BOT_STATE_NON_COMBAT);
        f.AI.ChangeStrategy("+runaway,-stay", BOT_STATE_COMBAT);
        Baseline const before = f.Captured(Part::Movement);
        Check(f.Run() == Result::Applied && !f.AI.HasStrategy("runaway", BOT_STATE_NON_COMBAT) &&
                  !f.AI.HasStrategy("runaway", BOT_STATE_COMBAT),
              "stay drops its sibling runaway in both states");
        Baseline const applied = f.Captured(Part::Movement);
        f.AI.Strategies.clear();
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Strategies ==
                      StrategyChanges{{"-stay,+runaway", BOT_STATE_NON_COMBAT}, {"-stay,+runaway", BOT_STATE_COMBAT}} &&
                  f.AI.Active == std::set<std::pair<std::string, int>>{{"runaway", BOT_STATE_NON_COMBAT},
                                                                       {"runaway", BOT_STATE_COMBAT}},
              "restore brings runaway back in both states and takes stay off");
    }
    {
        // Movement: a guarding bot told to follow guards again (follow drops guard out of combat only).
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.ChangeStrategy("+guard", BOT_STATE_NON_COMBAT);
        f.AI.ChangeStrategy("+guard", BOT_STATE_COMBAT);
        Baseline const before = f.Captured(Part::Movement);
        Check(f.Run() == Result::Applied, "follow applies over guard");
        Baseline const applied = f.Captured(Part::Movement);
        f.AI.Strategies.clear();
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Strategies == StrategyChanges{{"-follow,+guard", BOT_STATE_NON_COMBAT}} &&
                  f.AI.Active == std::set<std::pair<std::string, int>>{{"guard", BOT_STATE_NON_COMBAT},
                                                                       {"guard", BOT_STATE_COMBAT}},
              "restore brings guard back out of combat and takes follow off");
    }
    {
        // Focus heal: a person turning only the strategy off after the bridge wins.
        Fixture f;
        f.OneShot(Intent::FocusHealing);
        Baseline const before = f.Captured(Part::FocusHeal);
        Check(f.Run() == Result::Applied, "focus heal applies before a strategy change");
        Baseline const applied = f.Captured(Part::FocusHeal);
        f.AI.ChangeStrategy("-focus heal targets", BOT_STATE_COMBAT);
        int const writes = mutations;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Superseded &&
                  mutations == writes && f.AI.Values.FocusHeal.Data == std::list<ObjectGuid>{f.Other.Guid},
              "a focus heal strategy changed after the bridge stays");
    }
    {
        // One equality rule for Restore and a plan ledger: the bot and the part always, Movement by its strategies.
        Baseline a;
        a.Bot = 1;
        a.Held = Part::Movement;
        a.Strategies = 3;
        Baseline b = a;
        b.Stay = {true, 1.0f, 2.0f, 3.0f, 1};
        b.Return.Set = true;
        Check(a == b, "movement baselines with other spots are equal");
        b.Strategies = 1;
        Check(a != b, "movement baselines with other strategies differ");
        b = a;
        b.Bot = 5;
        Check(a != b, "baselines of two bots differ");
        b = a;
        b.Held = Part::Formation;
        Check(a != b, "baselines of two parts differ");
    }
    {
        // Movement: a playerbots follow command after the bridge stay wins.
        Fixture f;
        f.OneShot(Intent::Stay);
        Baseline const before = f.Captured(Part::Movement);
        Check(f.Run() == Result::Applied, "stay applies before a player's follow");
        Baseline const applied = f.Captured(Part::Movement);
        for (auto const& [names, state] : FollowStrategies)
            f.AI.ChangeStrategy(names, state);
        auto const active = f.AI.Active;
        int const writes = mutations;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Superseded &&
                  f.AI.Active == active && mutations == writes,
              "a later follow command is never undone");
    }
    {
        // Restore needs no group or controller, but a live bot.
        Fixture f;
        f.OneShot(Intent::Formation);
        Baseline const before = f.Captured(Part::Formation);
        Check(f.Run() == Result::Applied, "formation applies before ungroup");
        Baseline const applied = f.Captured(Part::Formation);
        f.Bot.Party = nullptr;
        f.AI.Master = nullptr;
        ObjectAccessor::Units.erase(2);
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Values.Formation.Name == "chaos",
              "an ungrouped bot without its controller is restored");
    }
    std::vector<std::function<void(Fixture&)>> gone = {[](Fixture&) { ObjectAccessor::Units.erase(1); }, [](Fixture& f)
                                                       { f.Bot.AI = nullptr; }, [](Fixture& f) { f.AI.BotAI = false; }};
    for (auto const& leave : gone)
    {
        Fixture f;
        f.OneShot(Intent::Formation);
        Baseline const before = f.Captured(Part::Formation);
        Check(f.Run() == Result::Applied, "formation applies before logout");
        Baseline const applied = f.Captured(Part::Formation);
        leave(f);
        int const writes = mutations;
        Baseline out = applied;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Gone &&
                  mutations == writes && f.AI.Values.Formation.Name == "near",
              "a logged-out bot or one no longer a bot is gone, nothing applied");
        Check(!OllamaBotRandomChatter::Capture(f.Bot.Guid.Value, Part::Formation, out) && out == Baseline(),
              "and cannot be captured");
    }
    {
        // A bot out of the world for a teleport: Failed for a retry, nothing written; back in the world it restores.
        Fixture f;
        f.OneShot(Intent::Formation);
        Baseline const before = f.Captured(Part::Formation);
        Check(f.Run() == Result::Applied, "formation applies before a teleport");
        Baseline const applied = f.Captured(Part::Formation);
        f.Bot.World = false;
        int const writes = mutations;
        Baseline out = applied;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Failed &&
                  mutations == writes && f.AI.Values.Formation.Name == "near",
              "a teleporting bot's restore fails for a retry, nothing applied");
        Check(!OllamaBotRandomChatter::Capture(f.Bot.Guid.Value, Part::Formation, out) && out == Baseline(),
              "and it is not captured");
        f.Bot.World = true;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Restored &&
                  f.AI.Values.Formation.Name == "chaos",
              "back in the world, the retry restores");
    }
    {
        Fixture f;
        f.OneShot(Intent::Formation);
        Baseline const before = f.Captured(Part::Formation);
        Check(f.Run() == Result::Applied, "formation applies before the phase checks");
        Baseline const applied = f.Captured(Part::Formation);
        Baseline other = applied;
        other.Held = Part::ManaSave;
        Baseline none;
        none.Bot = f.Bot.Guid.Value;
        int const writes = mutations;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, other) == RestoreResult::Rejected &&
                  OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, none, none) == RestoreResult::Rejected &&
                  OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied, true) == RestoreResult::Rejected,
              "mismatched or empty baselines and other threads are refused");
        Check(before.Bot == f.Bot.Guid.Value && applied.Bot == f.Bot.Guid.Value, "baselines carry the bot");
        Baseline foreign = before;
        foreign.Bot = f.Other.Guid.Value;
        Baseline foreignApplied = applied;
        foreignApplied.Bot = f.Other.Guid.Value;
        Check(
            OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, foreign, applied) == RestoreResult::Rejected &&
                OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, foreignApplied) == RestoreResult::Rejected &&
                OllamaBotRandomChatter::Restore(f.Other.Guid.Value, before, applied) == RestoreResult::Rejected &&
                mutations == writes && f.AI.Values.Formation.Name == "near",
            "another bot's baselines are refused, nothing applied");
        Baseline out = applied;
        Check(!OllamaBotRandomChatter::Capture(f.Bot.Guid.Value, Part::None, out) && out == Baseline(),
              "no capture of no part");
        out = applied;
        Check(!OllamaBotRandomChatter::Capture(f.Bot.Guid.Value, Part::Formation, out, true) && out == Baseline(),
              "no capture off the world thread");
        PlayerbotThreadCheck::worldThread = std::thread::id();
        out = applied;
        Check(OllamaBotRandomChatter::Restore(f.Bot.Guid.Value, before, applied) == RestoreResult::Rejected &&
                  !OllamaBotRandomChatter::Capture(f.Bot.Guid.Value, Part::Formation, out) && out == Baseline() &&
                  mutations == writes && f.AI.Values.Formation.Name == "near",
              "no restore or capture during startup");
    }
    // WT18a: each goal verb hands the planner one order after the admission and the prologue, with its place or
    // quest; the planner's answer is the result and the reason. No group is needed and nothing else changes.
    {
        using Planner::GoalApply::BridgeAnswer;
        using Planner::GoalApply::BridgeOrder;
        struct GoalVerb
        {
            Intent Operation;
            BridgeOrder Order;
            char const* Place;
            uint32_t Quest;
        };
        std::vector<GoalVerb> const verbs = {
            {Intent::GoTo, BridgeOrder::GoTo, "goldshire", 0},     {Intent::Quest, BridgeOrder::Quest, "", 33},
            {Intent::Errands, BridgeOrder::Errands, "", 0},        {Intent::SetHearth, BridgeOrder::SetHearth, "", 0},
            {Intent::GrindHere, BridgeOrder::GrindHere, "", 0},    {Intent::GatherHere, BridgeOrder::GatherHere, "", 0},
            {Intent::MeetAt, BridgeOrder::MeetAt, "1.5,2,3,0", 0}, {Intent::PausePlan, BridgeOrder::Pause, "", 0},
            {Intent::ResumePlan, BridgeOrder::Resume, "", 0}};
        auto const goal = [](Fixture& f, GoalVerb const& verb)
        {
            f.Command.Operation = verb.Operation;
            f.Command.Target = 0;
            f.Command.PlaceName = verb.Place;
            f.Command.QuestId = verb.Quest;
            bridgeCalls = 0;
            bridgeAnswer = BridgeAnswer::Done;
            bridgeBot = nullptr;
            bridgeAI = nullptr;
            bridgePlace = "unset";
            bridgeQuest = 99;
        };
        auto const noOtherEffect = []
        {
            return mutations == 0 && attacks == 0 && marks == 0 && follows == 0 && stays == 0 && stops == 0 &&
                   flees == 0 && moves == 0 && paths == 0;
        };
        for (GoalVerb const& verb : verbs)
        {
            {
                Fixture f;
                goal(f, verb);
                Check(f.Run() == Result::Applied && f.Authorizations == 1 && bridgeCalls == 1,
                      "a goal verb is delivered: one order to the planner after one admission");
                Check(bridgeOrder == verb.Order && bridgePlace == verb.Place && bridgeQuest == verb.Quest &&
                          bridgeBot == &f.Bot && bridgeAI == &f.AI,
                      "the planner gets the verb's order, place and quest for this bot");
                Check(noOtherEffect() && PlayerbotBridge::LastingPart(verb.Operation) == Part::None,
                      "a goal verb changes nothing else and has no lasting part");
            }
            {
                Fixture f;
                Ungrouped(f);
                goal(f, verb);
                Check(f.Run() == Result::Applied && bridgeCalls == 1, "a goal verb needs no group");
            }
            std::vector<std::pair<BridgeAnswer, Refusal>> const answers = {
                {BridgeAnswer::NotPlanner, Refusal::NotPlanner},     {BridgeAnswer::GoalHeld, Refusal::GoalHeld},
                {BridgeAnswer::UnknownPlace, Refusal::UnknownPlace}, {BridgeAnswer::NoRoute, Refusal::NoRoute},
                {BridgeAnswer::NoSuchQuest, Refusal::NoSuchQuest},   {BridgeAnswer::NothingHere, Refusal::NothingHere},
                {BridgeAnswer::AlreadyThere, Refusal::AlreadyThere}, {BridgeAnswer::CannotNow, Refusal::CannotNow}};
            for (auto const& [answer, reason] : answers)
            {
                Fixture f;
                goal(f, verb);
                bridgeAnswer = answer;
                Check(f.Run() == Result::Rejected && f.Why == reason && bridgeCalls == 1 && f.Authorizations == 1,
                      "the planner's refusal is the order's reason (mutant: a refusal dropped)");
            }
            {
                Fixture f;
                goal(f, verb);
                bridgeAnswer = BridgeAnswer::Failed;
                Check(f.Run() == Result::Failed && bridgeCalls == 1, "an executor that did not start is a failure");
            }
            std::vector<std::pair<std::function<void(Fixture&)>, Refusal>> const prologue = {
                {[](Fixture& f) { f.Bot.Alive = false; }, Refusal::Dead},
                {[](Fixture& f) { f.Bot.Battleground = true; }, Refusal::Battleground},
                {[](Fixture& f) { f.Bot.State |= UNIT_STATE_IN_FLIGHT; }, Refusal::Travelling}};
            for (auto const& [change, reason] : prologue)
            {
                Fixture f;
                goal(f, verb);
                change(f);
                f.Refused(reason);
                Check(bridgeCalls == 0, "the prologue refuses before the planner (mutant: no prologue for goals)");
            }
            std::vector<std::function<void(Fixture&)>> const stale = {
                [](Fixture& f) { f.Command.Group = 0; }, [](Fixture& f) { f.Human.Team = 1; },
                [](Fixture& f) { f.Bot.World = false; }, [](Fixture& f) { f.AI.BotAI = false; }};
            for (auto const& change : stale)
            {
                Fixture f;
                goal(f, verb);
                change(f);
                f.Denied();
                Check(bridgeCalls == 0, "a stale or foreign order reaches no planner");
            }
        }
        // Stray or missing fields: a mapping error, refused before the admission callback and the planner.
        std::vector<std::pair<GoalVerb, std::function<void(Fixture&)>>> const malformed = {
            {verbs[0], [](Fixture& f) { f.Command.PlaceName.clear(); }},
            {verbs[6], [](Fixture& f) { f.Command.PlaceName.clear(); }},
            {verbs[0], [](Fixture& f) { f.Command.PlaceName = std::string(PlayerbotBridge::MaxPlaceName + 1, 'a'); }},
            {verbs[1], [](Fixture& f) { f.Command.PlaceName = "goldshire"; }},
            {verbs[4], [](Fixture& f) { f.Command.PlaceName = "goldshire"; }},
            {verbs[1], [](Fixture& f) { f.Command.QuestId = 0; }},
            {verbs[0], [](Fixture& f) { f.Command.QuestId = 33; }},
            {verbs[7], [](Fixture& f) { f.Command.QuestId = 33; }},
            {verbs[2], [](Fixture& f) { f.Command.Target = f.Target.Guid.Value; }},
            {verbs[5], [](Fixture& f) { f.Command.Shape = FormationShape::Line; }},
            {verbs[8], [](Fixture& f) { f.Command.Enabled = true; }},
            {verbs[3], [](Fixture& f) { f.Command.VisibilityDistance = 0.0f; }}};
        for (auto const& [verb, change] : malformed)
        {
            Fixture f;
            goal(f, verb);
            change(f);
            f.Denied();
            Check(f.Authorizations == 0 && bridgeCalls == 0,
                  "a malformed goal order reaches neither admission nor planner");
        }
        {
            Fixture f;
            goal(f, verbs[0]);
            f.Command.PlaceName = std::string(PlayerbotBridge::MaxPlaceName, 'a');
            Check(f.Run() == Result::Applied && bridgePlace.size() == PlayerbotBridge::MaxPlaceName,
                  "a place of the longest length passes");
        }
        for (Intent operation : {Intent::Follow, Intent::Stay, Intent::Position, Intent::SaveMana})
        {
            Fixture f;
            f.OneShot(operation);
            f.Command.PlaceName = "goldshire";
            f.Denied();
            Fixture g;
            g.OneShot(operation);
            g.Command.QuestId = 33;
            g.Denied();
            Check(f.Authorizations == 0 && g.Authorizations == 0, "no place or quest on the other verbs");
        }
        {
            Fixture f;
            goal(f, verbs[0]);
            Check(OllamaBotRandomChatter::Run(
                      f.Command, [] { return true; }, true) == Result::Rejected &&
                      bridgeCalls == 0,
                  "a goal order off the world thread reaches no planner");
            Check(OllamaBotRandomChatter::Run(f.Command, [] { return false; }) == Result::Rejected && bridgeCalls == 0,
                  "a refused admission reaches no planner");
        }
    }
    // LC2: names for targets. A unit, quest or place given by name is looked up among what the bot perceives or
    // knows, then passes the same gate as an id (mutants: a name taken without the gate, an unknown name applied,
    // names looked up before the common rule).
    {
        auto const named = [](Fixture& f)
        {
            f.Target.Name = "Defias Thug";
            f.Target.X = 30.0f;
            f.Other.Name = "Brannoc";
            f.AI.Values.Npcs.Data = {f.Target.Guid};
            f.AI.Values.Friends.Data = {f.Other.Guid, f.Human.Guid};
            f.Party.Slots = {{f.Bot.Guid, "Fixturebot"}, {f.Human.Guid, "Partner"}, {f.Other.Guid, "Brannoc"}};
            f.Command.Target = 0;
            f.Command.TargetName = "defias thug";
        };
        {
            Fixture f;
            named(f);
            Check(f.Run() == Result::Applied && f.Party.Marked == f.Target.Guid && f.Party.Icons[6] == f.Target.Guid &&
                      f.Authorizations == 1,
                  "a mark by name marks the unit of that name");
        }
        {
            Fixture f;
            named(f);
            f.Command.Operation = Intent::AttackOwnTarget;
            f.Command.TargetName = "Defias Thug";
            Check(f.Run() == Result::Applied && attacks == 1 && f.Bot.LastAttacked == &f.Target,
                  "an attack by name attacks the unit of that name");
        }
        {
            Fixture f;
            named(f);
            f.OneShot(Intent::FocusHealing);
            f.Command.Target = 0;
            f.Command.TargetName = "BRANNOC";
            // A group member is found by name without being among the units the bot keeps as values.
            f.AI.Values.Friends.Data.clear();
            Check(f.Run() == Result::Applied && f.AI.Values.FocusHeal.Get() == std::list<ObjectGuid>{f.Other.Guid},
                  "focus healing by name heals the group member of that name first");
        }
        for (Intent operation : {Intent::AttackOwnTarget, Intent::Mark, Intent::FocusHealing})
        {
            Fixture f;
            named(f);
            f.OneShot(operation);
            f.Command.Target = 0;
            f.Command.TargetName = "Hogger";
            f.Refused(Refusal::UnknownName);
        }
        {
            Fixture f;
            named(f);
            f.Target.Seen = false;
            f.Refused(Refusal::TargetNotVisible);
        }
        {
            Fixture f;
            named(f);
            f.Command.TargetName = "de";
            f.Refused(Refusal::UnknownName);
        }
        {
            Fixture f;
            named(f);
            f.Command.TargetName = std::string(PlayerbotBridge::MaxName, 'a');
            f.Refused(Refusal::UnknownName);
        }
        {
            Fixture f;
            named(f);
            f.Command.TargetName = "Hogger";
            f.Bot.Alive = false;
            f.Refused(Refusal::Dead);
        }
        {
            Fixture f;
            named(f);
            Ungrouped(f);
            f.Command.TargetName = "Hogger";
            f.Refused(Refusal::NotInGroup);
        }
        // Two of one name and a nearer part of the name: the nearer of the exact ones; a part when none is exact.
        for (bool exact : {true, false})
        {
            Fixture f;
            named(f);
            Unit twin;
            twin.Guid = ObjectGuid(6);
            twin.Name = "Defias Thug";
            twin.X = 10.0f;
            Unit leader;
            leader.Guid = ObjectGuid(7);
            leader.Name = "Defias Thug Leader";
            leader.X = 1.0f;
            ObjectAccessor::Units[6] = &twin;
            ObjectAccessor::Units[7] = &leader;
            f.AI.Values.Possible.Data = {f.Target.Guid, leader.Guid, twin.Guid};
            f.Command.TargetName = exact ? "Defias Thug" : "thug";
            Check(f.Run() == Result::Applied && f.Party.Marked == (exact ? twin.Guid : leader.Guid),
                  "an exact name beats a nearer part of one, two of one name go to the nearer, else the nearest part");
        }
        // A quest by its title, among the bot's log.
        objectMgr.Quests = {{62, {"The Fargodeep Mine"}}, {40, {"A Fishy Peril"}}};
        std::vector<std::pair<char const*, uint32_t>> const quests = {
            {"the fargodeep mine", 62}, {"Fishy", 40}, {"Westfall Stew", 0}, {"The Jasperlode Mine", 0}};
        for (auto const& [title, quest] : quests)
        {
            Fixture f;
            f.Command.Operation = Intent::Quest;
            f.Command.Target = 0;
            f.Command.QuestName = title;
            f.Bot.QuestLog = {40, 0, 999, 62};
            bridgeCalls = 0;
            bridgeAnswer = Planner::GoalApply::BridgeAnswer::Done;
            bridgeQuest = 99;
            if (!quest)
            {
                f.Refused(Refusal::UnknownName);
                Check(bridgeCalls == 0, "an unknown quest title reaches no planner");
                continue;
            }
            Check(f.Run() == Result::Applied && bridgeCalls == 1 && bridgeQuest == quest,
                  "a quest by its title hands the planner the log quest's id");
        }
        // A place by name: the known place, on the bot's map before another's, under the planner's name for it; a
        // literal or an unknown name as given, for the planner's own rule.
        std::vector<std::pair<char const*, char const*>> const places = {{"Thunder Bluff", "thunderbluff"},
                                                                         {"ORGRIMMAR", "orgrimmar"},
                                                                         {"hill", "razorhill"},
                                                                         {"1.5,2,3,0", "1.5,2,3,0"},
                                                                         {"Atlantis", "Atlantis"}};
        for (Intent operation : {Intent::GoTo, Intent::MeetAt})
            for (auto const& [text, place] : places)
            {
                Fixture f;
                f.Command.Operation = operation;
                f.Command.Target = 0;
                f.Command.PlaceName = text;
                // Beside Sentinel Hill's spot, but on Kalimdor: Razor Hill is the nearer hill.
                f.Bot.X = -10600.0f;
                f.Bot.Y = 1100.0f;
                bridgeCalls = 0;
                bridgeAnswer = Planner::GoalApply::BridgeAnswer::Done;
                bridgePlace = "unset";
                Check(f.Run() == Result::Applied && bridgeCalls == 1 && bridgePlace == place,
                      "a place by name reaches the planner under its own name, a literal or unknown one as given");
            }
        // A unit or quest comes as an id or a name, never both and never neither; no name on a verb without one.
        std::vector<std::function<void(Fixture&)>> const malformed = {
            [](Fixture& f) { f.Command.Target = f.Target.Guid.Value; },
            [](Fixture& f) { f.Command.TargetName.clear(); },
            [](Fixture& f) { f.Command.TargetName = std::string(PlayerbotBridge::MaxName + 1, 'a'); },
            [](Fixture& f) { f.Command.QuestName = "A Fishy Peril"; },
            [](Fixture& f)
            {
                f.Command.Operation = Intent::Stay;
                f.Command.TargetName = "Defias Thug";
            },
            [](Fixture& f)
            {
                f.Command.Operation = Intent::Quest;
                f.Command.TargetName.clear();
                f.Command.QuestId = 40;
                f.Command.QuestName = "A Fishy Peril";
            },
            [](Fixture& f)
            {
                f.Command.Operation = Intent::Quest;
                f.Command.TargetName.clear();
                f.Command.QuestName = std::string(PlayerbotBridge::MaxName + 1, 'a');
            },
            [](Fixture& f)
            {
                f.Command.Operation = Intent::GoTo;
                f.Command.TargetName.clear();
                f.Command.PlaceName = "goldshire";
                f.Command.QuestName = "A Fishy Peril";
            }};
        for (auto const& change : malformed)
        {
            Fixture f;
            named(f);
            change(f);
            f.Denied();
            Check(f.Authorizations == 0, "a malformed name is refused before the admission");
        }
    }
    // LC2: an order of up to three steps, in order as one command: one admission, then per step its facts, its gate
    // and its event; a step that is not applied ends the order (mutants: authorize per step, a refused step not
    // ending the order, the gate skipped after the first step, four steps).
    {
        using Steps = std::vector<Order>;
        auto const stayOf = [](Fixture& f)
        {
            Order stay = f.Command;
            stay.Operation = Intent::Stay;
            stay.Target = 0;
            return stay;
        };
        auto const outcomesAre = [](Fixture const& f, std::vector<std::pair<Result, Refusal>> const& expected)
        {
            if (f.Outcomes.size() != expected.size())
                return false;
            for (std::size_t index = 0; index < expected.size(); ++index)
                if (f.Outcomes[index].Outcome != expected[index].first ||
                    f.Outcomes[index].Why != expected[index].second)
                    return false;
            return true;
        };
        {
            Fixture f;
            Check(f.RunSteps({stayOf(f), f.Command}) == Result::Applied &&
                      outcomesAre(f, {{Result::Applied, Refusal::None}, {Result::Applied, Refusal::None}}) &&
                      f.Events == 2 && f.Authorizations == 1 && f.AI.Resets == 1 && marks == 1 &&
                      f.Party.Marked == f.Target.Guid,
                  "two steps applied: one admission, an event and an effect each");
        }
        {
            Fixture f;
            Check(f.RunSteps({stayOf(f), f.Command, f.Command}) == Result::Applied && f.Outcomes.size() == 3 &&
                      f.Events == 3 && f.Authorizations == 1 && marks == 2,
                  "three steps applied");
        }
        {
            Fixture f;
            f.Target.Seen = false;
            Check(
                f.RunSteps({stayOf(f), f.Command}) == Result::Rejected &&
                    outcomesAre(f, {{Result::Applied, Refusal::None}, {Result::Rejected, Refusal::TargetNotVisible}}) &&
                    f.Events == 2 && f.AI.Resets == 1 && marks == 0,
                "a refused second step: the first stays applied, the order is refused with the second's reason");
        }
        {
            Fixture f;
            f.Target.Seen = false;
            Check(f.RunSteps({f.Command, stayOf(f)}) == Result::Rejected &&
                      outcomesAre(f, {{Result::Rejected, Refusal::TargetNotVisible}}) && f.Events == 1 &&
                      mutations == 0 && f.AI.Resets == 0,
                  "a refused first step ends the order: the second never runs");
        }
        {
            Fixture f;
            f.Party.MarkResult = false;
            Check(f.RunSteps({f.Command, stayOf(f)}) == Result::Failed &&
                      outcomesAre(f, {{Result::Failed, Refusal::None}}) && f.Events == 1 && f.AI.Resets == 0,
                  "a failed step ends the order");
        }
        {
            Fixture f;
            f.Party.Outsider = f.Human.Guid;
            Order formation = stayOf(f);
            formation.Operation = Intent::Formation;
            formation.Shape = FormationShape::Close;
            Check(f.RunSteps({stayOf(f), formation}) == Result::Rejected &&
                      outcomesAre(f, {{Result::Applied, Refusal::None}, {Result::Rejected, Refusal::NotInGroup}}) &&
                      f.AI.Values.Formation.Loads == 0,
                  "each step passes its own gate: the second's group rule refuses it");
        }
        // Refused before anything runs: no step, four, a malformed or unavailable later step, another scope.
        std::vector<std::pair<Result, std::function<Steps(Fixture&)>>> const before = {
            {Result::Rejected, [](Fixture&) { return Steps{}; }},
            {Result::Rejected, [&](Fixture& f) { return Steps{stayOf(f), f.Command, f.Command, f.Command}; }},
            {Result::Rejected,
             [&](Fixture& f)
             {
                 Order bad = f.Command;
                 bad.Icon = 99;
                 return Steps{stayOf(f), bad};
             }},
            {Result::Rejected,
             [&](Fixture& f)
             {
                 Order bad = f.Command;
                 bad.Target = 0;
                 return Steps{stayOf(f), bad};
             }},
            {Result::Unavailable,
             [&](Fixture& f)
             {
                 Order regroup = stayOf(f);
                 regroup.Operation = Intent::Regroup;
                 return Steps{stayOf(f), regroup};
             }},
            {Result::Rejected,
             [&](Fixture& f)
             {
                 Order other = f.Command;
                 other.Bot = f.Other.Guid.Value;
                 return Steps{stayOf(f), other};
             }},
            {Result::Rejected,
             [&](Fixture& f)
             {
                 Order other = f.Command;
                 other.Controller = f.Other.Guid.Value;
                 return Steps{stayOf(f), other};
             }},
            {Result::Rejected,
             [&](Fixture& f)
             {
                 Order other = f.Command;
                 other.Group = 0;
                 return Steps{stayOf(f), other};
             }},
            {Result::Rejected, [&](Fixture& f)
             {
                 Order other = f.Command;
                 other.VisibilityDistance = 40.0f;
                 return Steps{stayOf(f), other};
             }}};
        for (auto const& [expected, make] : before)
        {
            Fixture f;
            Check(f.RunSteps(make(f)) == expected && f.Outcomes.empty() && f.Events == 0 && f.Authorizations == 0 &&
                      mutations == 0 && marks == 0,
                  "an order with no step, four, a bad step or two scopes is refused before anything runs");
        }
        // One step is the old order: the same result and reason.
        std::vector<std::function<void(Fixture&)>> const variants = {[](Fixture&) {},
                                                                     [](Fixture& f) { f.Target.Seen = false; },
                                                                     [](Fixture& f) { f.Bot.Alive = false; },
                                                                     [](Fixture& f) { f.Party.MarkResult = false; },
                                                                     [](Fixture& f) { f.Command.Icon = 99; },
                                                                     [](Fixture& f)
                                                                     {
                                                                         f.Command.Operation = Intent::Regroup;
                                                                         f.Command.Target = 0;
                                                                     }};
        for (auto const& change : variants)
        {
            Result old = Result::Applied;
            Refusal oldWhy = Refusal::None;
            {
                Fixture f;
                change(f);
                old = f.Run();
                oldWhy = f.Why;
            }
            Fixture f;
            change(f);
            Result const result = f.RunSteps({f.Command});
            Refusal const why =
                result == Result::Rejected && !f.Outcomes.empty() ? f.Outcomes.back().Why : Refusal::None;
            Check(result == old && why == oldWhy, "a one-step order is the old order");
        }
    }
    // LC1a: the ability table. Every row has every part; the eighteen verbs with an effect have a row and the four
    // without one have none; each row applied above and gave each refusal it lists, and no other.
    {
        std::span<AbilityRow const> const rows = PlayerbotBridge::AbilityRows();
        Check(rows.size() == 18, "eighteen rows");
        for (int value = 0; value <= static_cast<int>(Intent::ResumePlan); ++value)
        {
            Intent const operation = static_cast<Intent>(value);
            Check((PlayerbotBridge::FindRow(operation) != nullptr) != PlayerbotBridge::IsUnavailable(operation),
                  "every verb has a row or is one of the four without an effect");
        }
        Check(!PlayerbotBridge::FindRow(static_cast<Intent>(255)) &&
                  !PlayerbotBridge::IsUnavailable(static_cast<Intent>(255)) && !PlayerbotBridge::FindRow("regroup") &&
                  !PlayerbotBridge::FindRow("") && !PlayerbotBridge::FindRow("Follow"),
              "no row for a value outside the enum, a verb without an effect or an unknown name");
        std::span<Refusal const> const common = PlayerbotBridge::CommonRefusals();
        Check(std::vector<Refusal>(common.begin(), common.end()) ==
                  std::vector<Refusal>{Refusal::Battleground, Refusal::Travelling, Refusal::Dead},
              "the common refusals in check order");
        auto const filled = [](char const* text) { return text && *text; };
        auto const listed = [](std::span<Refusal const> list, Refusal reason)
        { return std::find(list.begin(), list.end(), reason) != list.end(); };
        std::set<std::string> names;
        for (AbilityRow const& row : rows)
        {
            Check(PlayerbotBridge::FindRow(row.Operation) == &row && PlayerbotBridge::FindRow(row.Name) == &row &&
                      names.insert(row.Name).second,
                  "a row is found by its verb and by its own name");
            Check(filled(row.Name) && filled(row.Sentence) && filled(row.StatePhrase) && row.Effect,
                  "a row has its name, sentence, state phrase and effect");
            Check((row.Group == PlayerbotBridge::GroupRule::None) == (row.GroupRefusal == Refusal::None) &&
                      (row.Group == PlayerbotBridge::GroupRule::None || listed(row.Refusals, row.GroupRefusal)),
                  "a group rule has its refusal, and the row lists it");
            std::set<std::string> params;
            for (ParamRow const& param : row.Params)
            {
                Check(filled(param.Name) && filled(param.Sentence) && params.insert(param.Name).second,
                      "a parameter has its own name and a sentence");
                Check((param.Kind == ParamKind::Choice) == !param.Choices.empty() &&
                          std::all_of(param.Choices.begin(), param.Choices.end(), filled),
                      "a choice and only a choice has its words");
            }
            std::string_view const phrase = row.StatePhrase;
            for (std::size_t open = phrase.find('{'); open != std::string_view::npos; open = phrase.find('{', open + 1))
            {
                std::size_t const close = phrase.find('}', open);
                Check(close != std::string_view::npos &&
                          params.count(std::string(phrase.substr(open + 1, close - open - 1))) == 1,
                      "a state phrase names only the row's parameters");
            }
            for (Refusal reason : row.Refusals)
                Check(reason != Refusal::None && !listed(common, reason) &&
                          PlayerbotBridge::RefusalWords(reason).Code == reason,
                      "a row lists only its own refusals");
            Check(PlayerbotBridge::LastingPart(row.Operation) == row.Lasting, "the lasting part is the row's");
            Check(seenApplied.count(row.Operation) == 1, "every row applies in this selftest");
            std::set<Refusal> const& seen = seenRefusals[row.Operation];
            for (Refusal reason : row.Refusals)
                Check(seen.count(reason) == 1, "every refusal a row lists is given in this selftest");
            // The planner double gives any goal verb any answer; the planner's own rule is BridgeCheck's.
            bool const goal = row.Operation >= Intent::GoTo;
            for (Refusal reason : seen)
                Check(goal || listed(row.Refusals, reason) || listed(common, reason),
                      "a row gives no refusal it does not list");
        }
        std::set<std::string> words;
        for (int value = 0; value <= static_cast<int>(Refusal::UnknownName); ++value)
        {
            PlayerbotBridge::RefusalText const& text = PlayerbotBridge::RefusalWords(static_cast<Refusal>(value));
            Check(static_cast<int>(text.Code) == value && filled(text.Word) && words.insert(text.Word).second &&
                      std::all_of(text.Word, text.Word + std::strlen(text.Word),
                                  [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; }) &&
                      filled(text.Sentence) == (value != 0),
                  "every refusal has its own snake_case word and, but for none, a sentence");
        }
        Check(PlayerbotBridge::RefusalWords(static_cast<Refusal>(99)).Code == Refusal::None,
              "an unknown refusal reads as none");
        // LC3: every operation has its own wire word, its row's name or one of the four without a row, and the word
        // finds it again; the module parses the wire's verbs with these.
        std::set<std::string> verbs;
        bool verbsRoundTrip = true;
        for (int value = 0; value <= static_cast<int>(Intent::ResumePlan); ++value)
        {
            Intent const operation = static_cast<Intent>(value);
            std::string const word = PlayerbotBridge::IntentWord(operation);
            AbilityRow const* row = PlayerbotBridge::FindRow(operation);
            verbsRoundTrip = verbsRoundTrip && !word.empty() && verbs.insert(word).second &&
                             PlayerbotBridge::FindIntent(word) == operation &&
                             (row ? word == row->Name : PlayerbotBridge::IsUnavailable(operation));
        }
        Check(verbsRoundTrip && verbs.size() == 22, "every operation has its own wire word, and the word finds it");
        // LC.23: the old wire's verbs in Intent order; a bridge that knows no catalogue still sends exactly these.
        char const* const wireVerbs[] = {
            "follow",        "stay",          "flee",        "regroup",    "attack_own_target", "pull",
            "mark",          "focus_healing", "assign_role", "save_mana",  "formation",         "position",
            "dungeon_clear", "go_to",         "quest",       "errands",    "set_hearth",        "grind_here",
            "gather_here",   "meet_at",       "pause_plan",  "resume_plan"};
        bool oldWords = std::size(wireVerbs) == verbs.size();
        for (std::size_t value = 0; oldWords && value < std::size(wireVerbs); ++value)
            oldWords = std::string_view(PlayerbotBridge::IntentWord(static_cast<Intent>(value))) == wireVerbs[value];
        Check(oldWords && !*PlayerbotBridge::IntentWord(static_cast<Intent>(22)) && !PlayerbotBridge::FindIntent("") &&
                  !PlayerbotBridge::FindIntent("Follow") && !PlayerbotBridge::FindIntent("follow ") &&
                  !PlayerbotBridge::FindIntent("inventory") && !PlayerbotBridge::FindIntent("regroups"),
              "every operation keeps its old wire word; no other word, and no topic, is a verb");
        Check(PlayerbotBridge::Takes(*PlayerbotBridge::FindRow(Intent::Mark), OrderField::Icon) &&
                  PlayerbotBridge::Takes(*PlayerbotBridge::FindRow(Intent::Position), OrderField::Position) &&
                  !PlayerbotBridge::Takes(*PlayerbotBridge::FindRow(Intent::Stay), OrderField::Target),
              "a row takes the fields of its parameters only");

        // LC1b: the topic rows. Every topic has a row, in the wire's order and under the wire's word
        // (mod-ollama-chat's StateTopicName); only route takes an argument, a place.
        char const* const wireTopics[] = {"inventory", "quests", "cooldowns",  "strategies", "spells", "plan",
                                          "options",   "route",  "quest_plan", "bags",       "places"};
        std::span<TopicRow const> const topics = PlayerbotBridge::TopicRows();
        Check(topics.size() == std::size(wireTopics), "eleven topic rows");
        for (std::size_t index = 0; index < topics.size(); ++index)
        {
            TopicRow const& row = topics[index];
            BotStateTopic const topic = static_cast<BotStateTopic>(index);
            Check(row.Topic == topic && PlayerbotBridge::FindTopic(topic) == &row &&
                      PlayerbotBridge::FindTopic(row.Name) == &row && std::string_view(row.Name) == wireTopics[index],
                  "a topic row is found by its topic and by the wire's word for it");
            Check(filled(row.Sentence) && row.Limit > 0 && row.Limit <= PlayerbotBridge::BotStateTopicMaxItems,
                  "a topic row has a sentence and a limit within the reader's cap");
            bool const route = topic == BotStateTopic::Route;
            Check(row.Params.size() == (route ? 1u : 0u) &&
                      (!route || (row.Params[0].Kind == ParamKind::Place && filled(row.Params[0].Name) &&
                                  filled(row.Params[0].Sentence) && row.Params[0].Choices.empty())),
                  "only route takes an argument, a place");
            Check(!PlayerbotBridge::FindRow(row.Name) && names.insert(row.Name).second,
                  "no two rows, act or topic, share a name");
        }
        Check(!PlayerbotBridge::FindTopic(static_cast<BotStateTopic>(topics.size())) &&
                  !PlayerbotBridge::FindTopic("Places") && !PlayerbotBridge::FindTopic(""),
              "no topic row for a value outside the enum or an unknown name");

        // LC1b: the catalogue is the acts, then the topics, each entry its row's parts as plain values.
        std::vector<AbilityData> const catalogue = PlayerbotBridge::Catalogue();
        Check(catalogue.size() == rows.size() + topics.size(), "the catalogue holds every act and topic row");
        auto const sameParams =
            [](std::vector<PlayerbotBridge::ParamData> const& data, std::span<ParamRow const> params)
        {
            if (data.size() != params.size())
                return false;
            for (std::size_t index = 0; index < data.size(); ++index)
                if (data[index].Name != params[index].Name || data[index].Kind != params[index].Kind ||
                    data[index].Sentence != params[index].Sentence ||
                    data[index].Choices !=
                        std::vector<std::string>(params[index].Choices.begin(), params[index].Choices.end()))
                    return false;
            return true;
        };
        for (std::size_t index = 0; index < rows.size() && index < catalogue.size(); ++index)
        {
            AbilityRow const& row = rows[index];
            AbilityData const& entry = catalogue[index];
            std::vector<Refusal> expected(common.begin(), common.end());
            expected.insert(expected.end(), row.Refusals.begin(), row.Refusals.end());
            std::vector<Refusal> codes;
            for (PlayerbotBridge::RefusalData const& refusal : entry.Refusals)
            {
                codes.push_back(refusal.Code);
                Check(refusal.Word == PlayerbotBridge::RefusalWords(refusal.Code).Word &&
                          refusal.Sentence == PlayerbotBridge::RefusalWords(refusal.Code).Sentence,
                      "a catalogue refusal carries its word and sentence");
            }
            Check(entry.Kind == AbilityKind::Act && entry.Name == row.Name && entry.Sentence == row.Sentence &&
                      sameParams(entry.Params, row.Params),
                  "an act's entry has its row's name, sentence and parameters, in table order");
            Check(codes == expected, "an act's entry has the common refusals, then its row's, in check order");
            Check(entry.Lasting == row.Lasting && entry.Done == row.Done && entry.StatePhrase == row.StatePhrase &&
                      entry.Event == "BridgeAct" && entry.Event == PlayerbotBridge::ActEventType && entry.Limit == 0,
                  "an act's entry has its row's lasting part, completion, state phrase and event");
        }
        for (std::size_t index = 0; index < topics.size() && rows.size() + index < catalogue.size(); ++index)
        {
            TopicRow const& row = topics[index];
            AbilityData const& entry = catalogue[rows.size() + index];
            Check(entry.Kind == AbilityKind::Topic && entry.Name == row.Name && entry.Sentence == row.Sentence &&
                      sameParams(entry.Params, row.Params) && entry.Limit == row.Limit,
                  "a topic's entry has its row's name, sentence, argument and limit, in table order");
            Check(entry.Refusals.empty() && entry.Lasting == Part::None && entry.Done == Completion::AtOnce &&
                      entry.StatePhrase.empty() && entry.Event.empty(),
                  "a topic's entry has no refusal, lasting part, state phrase or event");
        }

        // LC1b: the canonical text and its hash.
        std::string const text = PlayerbotBridge::CatalogueText(catalogue);
        // LC3: mod-ollama-chat sends the catalogue whole, at most 24,576 encoded bytes, about 1.42 times this text (an
        // answer over its budget drops the catalogue query). Past this bound a new row needs the query paged first.
        Check(text.size() <= 16384, "the catalogue text stays within the module's answer budget");
        auto const plain = [](std::string const& field, char const* forbidden)
        { return field.find_first_of(forbidden) == std::string::npos; };
        char const* const breaks = "\t\n";
        bool fieldsPlain = true;
        for (AbilityData const& entry : catalogue)
        {
            fieldsPlain = fieldsPlain && plain(entry.Name, breaks) && plain(entry.Sentence, breaks) &&
                          plain(entry.StatePhrase, breaks);
            for (PlayerbotBridge::ParamData const& param : entry.Params)
            {
                fieldsPlain = fieldsPlain && plain(param.Name, breaks) && plain(param.Sentence, breaks);
                for (std::string const& choice : param.Choices)
                    fieldsPlain = fieldsPlain && !choice.empty() && plain(choice, "\t\n,");
            }
            for (PlayerbotBridge::RefusalData const& refusal : entry.Refusals)
                fieldsPlain = fieldsPlain && plain(refusal.Word, breaks) && plain(refusal.Sentence, breaks);
        }
        Check(fieldsPlain, "no catalogue field holds a tab or line break, nor a choice a comma");
        std::size_t const lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
        std::size_t expectedLines = 2;
        for (AbilityData const& entry : catalogue)
            expectedLines += 6 + entry.Params.size() + entry.Refusals.size();
        auto const holds = [&text](char const* part) { return text.find(part) != std::string::npos; };
        // LC3: the steps line follows the format line, so the hash covers the most steps an order takes.
        Check(text.rfind("catalogue\t1\nsteps\t3\nact\tfollow\t", 0) == 0 && PlayerbotBridge::CatalogueVersion == 1 &&
                  PlayerbotBridge::MaxSteps == 3 && holds("\ntopic\tplaces\t") &&
                  holds("\nparam\ticon\tchoice\tThe raid icon to put on it.\tstar,circle,") &&
                  holds("\nrefusal\tnot_with_you\tWe are not travelling together.\n") &&
                  holds("\neffect\tmana_save\n") && holds("\ndone\tgoal_ends\n") &&
                  holds("\nstate\tattacking {target}\n") && holds("\nevent\tBridgeAct\n") && holds("\nlimit\t24\n") &&
                  lines == expectedLines,
              "the catalogue text is its format line, then one line per part, enums as words");
        Check(PlayerbotBridge::TextHash("") == 0xcbf29ce484222325ull &&
                  PlayerbotBridge::TextHash("a") == 0xaf63dc4c8601ec8cull &&
                  PlayerbotBridge::TextHash("foobar") == 0x85944171f73967e8ull,
              "the text hash is FNV-1a 64");
        // The words, one past the last value included (empty): the wire reads them.
        auto const wordsOf = [](auto word, int last)
        {
            std::string joined;
            for (int value = 0; value <= last + 1; ++value)
                joined.append(word(value)).append(" ");
            return joined;
        };
        Check(wordsOf([](int value) { return PlayerbotBridge::KindWord(static_cast<ParamKind>(value)); },
                      static_cast<int>(ParamKind::Count)) == "unit place item spell quest choice flag count  " &&
                  wordsOf([](int value) { return PlayerbotBridge::KindWord(static_cast<AbilityKind>(value)); },
                          static_cast<int>(AbilityKind::Topic)) == "act topic  " &&
                  wordsOf([](int value) { return PlayerbotBridge::PartWord(static_cast<Part>(value)); },
                          static_cast<int>(Part::FocusHeal)) == "none movement formation mana_save focus_heal  " &&
                  wordsOf([](int value) { return PlayerbotBridge::CompletionWord(static_cast<Completion>(value)); },
                          static_cast<int>(Completion::GoalEnds)) == "at_once holds arrived target_gone goal_ends  ",
              "the catalogue's enum words, and none for a value outside an enum");
        std::uint64_t const hash = PlayerbotBridge::CatalogueHash(catalogue);
        Check(hash == PlayerbotBridge::TextHash(text) &&
                  PlayerbotBridge::CatalogueHash(PlayerbotBridge::Catalogue()) == hash,
              "the catalogue hash is its text's, the same on every call");
        Check(catalogue.size() > 4 && catalogue[4].Name == "mark" && catalogue[4].Params.size() == 2 &&
                  catalogue.back().Kind == AbilityKind::Topic,
              "the hash changes below are made on mark and the last topic");
        using Change = void (*)(std::vector<AbilityData>&);
        Change const changes[] = {
            [](std::vector<AbilityData>& c) { c[4].Kind = AbilityKind::Topic; },
            [](std::vector<AbilityData>& c) { c[4].Name += "s"; },
            [](std::vector<AbilityData>& c) { c[4].Sentence += "."; },
            [](std::vector<AbilityData>& c)
            {
                c[4].Name += c[4].Sentence.substr(0, 1);
                c[4].Sentence.erase(0, 1);
            },
            [](std::vector<AbilityData>& c) { c[4].Params[0].Name += "s"; },
            [](std::vector<AbilityData>& c) { c[4].Params[0].Kind = ParamKind::Item; },
            [](std::vector<AbilityData>& c) { c[4].Params[0].Sentence += "."; },
            [](std::vector<AbilityData>& c) { c[4].Params[1].Choices[0] += "s"; },
            [](std::vector<AbilityData>& c) { c[4].Params[1].Choices.pop_back(); },
            [](std::vector<AbilityData>& c) { c[4].Params.pop_back(); },
            [](std::vector<AbilityData>& c) { c[4].Refusals[0].Word += "s"; },
            [](std::vector<AbilityData>& c) { c[4].Refusals[0].Sentence += "."; },
            [](std::vector<AbilityData>& c) { c[4].Refusals.pop_back(); },
            [](std::vector<AbilityData>& c) { c[4].Lasting = Part::Formation; },
            [](std::vector<AbilityData>& c) { c[4].Done = Completion::Holds; },
            [](std::vector<AbilityData>& c) { c[4].StatePhrase += "."; },
            [](std::vector<AbilityData>& c) { c[4].Event.clear(); },
            [](std::vector<AbilityData>& c) { c.back().Limit += 1; },
            [](std::vector<AbilityData>& c) { std::swap(c[3], c[4]); },
            [](std::vector<AbilityData>& c) { c.pop_back(); },
        };
        for (Change change : changes)
        {
            std::vector<AbilityData> changed = catalogue;
            change(changed);
            Check(PlayerbotBridge::CatalogueHash(changed) != hash, "every part of every entry changes the hash");
        }
        std::cout << "catalogue " << catalogue.size() << " rows, " << text.size() << " bytes, hash " << std::hex << hash
                  << std::dec << '\n';
    }
    std::cout << "PASS " << checks << " checks\n";
}
