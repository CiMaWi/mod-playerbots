/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The isolated runner supplies game doubles and includes the unchanged production effect implementation and the
// AttackAction, StayAction, FleeAction and MoveToPositionAction::ExecuteExactPosition method bodies; FollowAction,
// MovementAction::MoveAway/MoveTo and PathGenerator are counting doubles. It does not replace a decision path with a
// parallel guard implementation.
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>

// clang-format off
#include "J6b1-game-fixture.h"
#include "BridgeIntentEffect.cpp"
#include "AttackAction-body.inc"
#include "StayAction-body.inc"
#include "FleeAction-body.inc"
#include "PositionAction-body.inc"
// clang-format on

using PlayerbotBridge::Intent;
using PlayerbotBridge::Order;
using PlayerbotBridge::Result;

static_assert(!std::is_default_constructible_v<PlayerbotBridge::PostMapJoin>);
static_assert(!std::is_copy_constructible_v<PlayerbotBridge::PostMapJoin>);
static_assert(!std::is_move_constructible_v<PlayerbotBridge::PostMapJoin>);

class OllamaBotRandomChatter
{
public:
    static Result Run(Order const& order, std::function<bool()> const& authorize, bool worker = false)
    {
        PlayerbotBridge::PostMapJoin phase;
        if (!worker)
            return PlayerbotBridge::Execute(phase, order, authorize);

        Result result = Result::Applied;
        std::thread thread([&] { result = PlayerbotBridge::Execute(phase, order, authorize); });
        thread.join();
        return result;
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

    Fixture()
    {
        mutations = attacks = marks = follows = usefulFollow = stays = stops = flees = fleeRuns = moves = paths = 0;
        security = melee = followUseful = followResult = fleeResult = moveResult = true;
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
    }

    Result Run()
    {
        return OllamaBotRandomChatter::Run(Command,
                                           [&]
                                           {
                                               ++Authorizations;
                                               return true;
                                           });
    }

    void Denied(Result expected = Result::Rejected)
    {
        Check(Run() == expected, "denial result");
        Check(mutations == 0 && attacks == 0 && marks == 0 && follows == 0 && stays == 0 && stops == 0 && flees == 0 &&
                  fleeRuns == 0 && moves == 0,
              "denied no mutation");
    }
};
}  // namespace

int main()
{
    {
        Fixture f;
        Check(f.Run() == Result::Applied, "mark applies");
        Check(marks == 1 && attacks == 0, "one mark attempt");
        Check(f.Party.Actor == f.Human.Guid && f.Party.Marked == f.Target.Guid, "exact actor and target");
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
    for (int operation = 0; operation <= static_cast<int>(Intent::DungeonClear); ++operation)
    {
        Fixture f;
        f.Command.Operation = static_cast<Intent>(operation);
        if (f.Command.Operation != Intent::AttackOwnTarget && f.Command.Operation != Intent::Mark &&
            f.Command.Operation != Intent::Follow && f.Command.Operation != Intent::Stay &&
            f.Command.Operation != Intent::Flee && f.Command.Operation != Intent::Position)
        {
            f.Denied(Result::Unavailable);
            Check(f.Authorizations == 0, "unavailable before admission callback");
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
        [](Fixture&) { ObjectAccessor::Units.erase(4); },
        [](Fixture& f) { f.Bot.World = false; },
        [](Fixture& f) { f.Human.World = false; },
        [](Fixture& f) { f.Bot.AI = nullptr; },
        [](Fixture& f) { f.AI.BotAI = false; },
        [](Fixture& f) { f.Human.AI = &f.HumanAI; },
        [](Fixture& f) { f.Human.Seen = false; },
        [](Fixture& f) { f.Bot.Seen = false; },
        [](Fixture& f) { f.Human.Map = 2; },
        [](Fixture& f) { f.Human.Phase = 2; },
        [](Fixture& f) { f.Human.Near = false; },
        [](Fixture& f) { f.Human.Team = 1; },
        [](Fixture& f) { f.Bot.Party = nullptr; },
        [](Fixture& f) { f.Party.Guid = ObjectGuid(77); },
        [](Fixture& f) { f.Human.Party = &f.OtherParty; },
        [](Fixture& f) { f.Party.Member = false; },
        [](Fixture& f) { f.AI.Master = &f.Other; },
        [](Fixture& f)
        {
            f.AI.Master = nullptr;
            f.Party.Leader = f.Other.Guid;
        },
        [](Fixture&) { security = false; },
        [](Fixture& f) { f.Target.World = false; },
        [](Fixture& f) { f.Target.Map = 2; },
        [](Fixture& f) { f.Target.Phase = 2; },
        [](Fixture& f) { f.Target.Seen = false; },
        [](Fixture& f) { f.Target.Near = false; },
        [](Fixture& f)
        {
            f.Party.Raid = true;
            f.Party.Leader = f.Other.Guid;
        },
        [](Fixture& f)
        {
            f.Target.PlayerType = true;
            f.Target.Hostile = true;
        }};
    for (auto const& deny : denials)
    {
        Fixture f;
        deny(f);
        f.Denied();
    }
    {
        Fixture f;
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied, "actual group leader fallback");
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
        f.Denied();
    }
    {
        Fixture f;
        f.Party.Raid = true;
        f.Party.Leader = f.Other.Guid;
        f.Party.Assistant = true;
        Check(f.Run() == Result::Applied, "raid assistant marking");
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
                                              f.AI.Master = &f.Other;
                                              return true;
                                          }) == Result::Rejected,
              "facts resolved after admission callback");
        Check(mutations == 0, "changed authorization no effect");
    }
    std::vector<std::function<void(Fixture&)>> attackDenials = {[](Fixture& f) { f.Bot.Target = f.Other.Guid; },
                                                                [](Fixture& f) { f.Bot.Alive = false; },
                                                                [](Fixture& f) { f.Bot.Mounted = true; }};
    for (auto const& deny : attackDenials)
    {
        Fixture f;
        f.Command.Operation = Intent::AttackOwnTarget;
        deny(f);
        f.Denied();
    }
    std::vector<std::function<void(Fixture&)>> attackFailures = {[](Fixture& f)
                                                                 { f.Bot.MotionState.Type = FLIGHT_MOTION_TYPE; },
                                                                 [](Fixture& f) { f.Bot.State = UNIT_STATE_IN_FLIGHT; },
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
        f.AI.Values.Positions.Data["stay"].Stored = f.AI.Values.Positions.Data["return"].Stored = true;
        f.AI.Values.Priority.Data = {f.Target.Guid};
        Check(f.Run() == Result::Applied, "follow applies");
        Check(follows == 1 && usefulFollow == 1 && stays == 0 && stops == 0 && attacks == 0 && marks == 0,
              "one follow move and nothing else");
        Check(f.Authorizations == 1 && f.Party.Marked == ObjectGuid() && f.Bot.Target == f.Target.Guid,
              "one follow authorization without target or selection writes");
        Check(f.AI.Strategies == FollowStrategies && f.AI.StrategyResets == 0 && f.AI.Resets == 0 &&
                  f.AI.Master == &f.Human,
              "the follow command's strategies for the master");
        Check(!f.AI.Values.Positions.Data["stay"].Stored && !f.AI.Values.Positions.Data["return"].Stored &&
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
            Check(position.Stored && position.X == 3.0f && position.Y == -2.0f && position.Z == 1.0f &&
                      position.MapId == 1,
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
    for (Intent operation : {Intent::Follow, Intent::Stay, Intent::Flee, Intent::Position})
    {
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
            [](Fixture& f) { f.Human.Seen = false; },
            [](Fixture& f) { f.Bot.Seen = false; },
            [](Fixture& f) { f.Human.Map = 2; },
            [](Fixture& f) { f.Human.Phase = 2; },
            [](Fixture& f) { f.Human.Near = false; },
            [](Fixture& f) { f.Human.Team = 1; },
            [](Fixture& f) { f.Bot.Party = nullptr; },
            [](Fixture& f) { f.Party.Guid = ObjectGuid(77); },
            [](Fixture& f) { f.Human.Party = &f.OtherParty; },
            [](Fixture& f) { f.Party.Member = false; },
            [](Fixture& f) { f.AI.Master = &f.Other; },
            [](Fixture& f)
            {
                f.AI.Master = nullptr;
                f.Party.Leader = f.Other.Guid;
            },
            [](Fixture&) { security = false; },
            [](Fixture& f) { f.Bot.Alive = false; }};
        if (operation == Intent::Flee)
        {
            oneShotDenials.push_back([](Fixture& f) { f.AI.Values.Current.Data = nullptr; });
            oneShotDenials.push_back([](Fixture& f) { f.Target.Alive = false; });
            oneShotDenials.push_back([](Fixture& f) { f.Target.World = false; });
            oneShotDenials.push_back([](Fixture& f) { f.Target.Map = 2; });
            oneShotDenials.push_back([](Fixture&) { melee = false; });
            oneShotDenials.push_back([](Fixture& f) { f.Bot.Channeled = &channeling; });
            oneShotDenials.push_back([](Fixture& f) { f.Bot.Mounted = true; });
        }
        else if (operation == Intent::Position)
        {
            oneShotDenials.push_back([](Fixture& f) { f.Command.MapId = 2; });
            oneShotDenials.push_back([](Fixture& f) { f.Command.X = 45.0; });
            oneShotDenials.push_back([](Fixture& f) { f.Command.X = -15.0; });
            oneShotDenials.push_back([](Fixture&) { pathType = PATHFIND_NOPATH; });
            oneShotDenials.push_back([](Fixture&) { pathLength = 60.5f; });
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
                                                  f.AI.Master = &f.Other;
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
        Fixture f;
        f.OneShot(Intent::Follow);
        f.AI.Master = nullptr;
        f.Bot.Battleground = true;
        f.Denied();
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
        f.AI.Values.Positions.Data["stay"].Stored = true;
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
        f.Denied();
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
    std::vector<std::function<void(Fixture&)>> remote = {
        [](Fixture& f) { f.Command.MapId = 2; },
        [](Fixture& f) { f.Command.X = std::numeric_limits<double>::quiet_NaN(); },
        [](Fixture& f) { f.Command.Y = std::numeric_limits<double>::infinity(); },
        [](Fixture& f) { f.Command.Z = -std::numeric_limits<double>::infinity(); },
        [](Fixture& f) { f.Command.X = 1e300; },
        [](Fixture& f) { f.Command.Y = -std::numeric_limits<double>::max(); },
        [](Fixture& f) { f.Command.X = 45.0; },
        [](Fixture& f) { f.Command.X = -15.0; },
        [](Fixture& f) { f.Command.Z = 31.0; },
        [](Fixture& f) { f.Command.VisibilityDistance = 0.0f; },
        [](Fixture& f) { f.Command.VisibilityDistance = std::numeric_limits<float>::quiet_NaN(); },
        [](Fixture&) { ObjectAccessor::Units.erase(1); },
        [](Fixture&) { ObjectAccessor::Units.erase(2); },
        [](Fixture& f) { f.Bot.World = false; },
        [](Fixture& f) { f.Human.World = false; },
        [](Fixture& f) { f.Human.Map = 2; }};
    for (auto const& deny : remote)
    {
        Fixture f;
        f.OneShot(Intent::Position);
        deny(f);
        Check(!OllamaBotRandomChatter::Reachable(f.Command) && paths == 0 && mutations == 0,
              "an unchecked position is refused before any path query");
        f.Denied();
        Check(paths == 0, "no path query for an order refused on its facts");
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
        f.Denied();
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
        f.Denied();
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
    std::cout << "PASS " << checks << " checks\n";
}
