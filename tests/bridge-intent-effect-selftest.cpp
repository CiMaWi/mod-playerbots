/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The isolated runner supplies game doubles and includes the unchanged production effect implementation and the
// AttackAction and StayAction method bodies; FollowAction is a counting double. It does not replace a decision path
// with a parallel guard implementation.
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>

// clang-format off
#include "J6b1-game-fixture.h"
#include "BridgeIntentEffect.cpp"
#include "AttackAction-body.inc"
#include "StayAction-body.inc"
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
};

namespace
{
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
        mutations = attacks = marks = follows = usefulFollow = stays = stops = 0;
        security = melee = followUseful = followResult = true;
        waitForAttack = prohibited = unavailable = moveFromGroup = false;
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

    // A target-less verb with its wire shape (no target) and a bot that is moving.
    void OneShot(Intent operation)
    {
        Command.Operation = operation;
        Command.Target = 0;
        Bot.Moving = true;
        AI.Values.Moving.Data = true;
        Bot.State = UNIT_STATE_CHASE | UNIT_STATE_FOLLOW;
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
        Check(mutations == 0 && attacks == 0 && marks == 0 && follows == 0 && stays == 0 && stops == 0,
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
    for (int operation = 0; operation <= static_cast<int>(Intent::DungeonClear); ++operation)
    {
        Fixture f;
        f.Command.Operation = static_cast<Intent>(operation);
        if (f.Command.Operation != Intent::AttackOwnTarget && f.Command.Operation != Intent::Mark &&
            f.Command.Operation != Intent::Follow && f.Command.Operation != Intent::Stay)
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
        Check(f.Run() == Result::Applied, "follow applies");
        Check(follows == 1 && usefulFollow == 1 && stays == 0 && stops == 0 && attacks == 0 && marks == 0,
              "one follow move and nothing else");
        Check(f.Authorizations == 1 && f.Party.Marked == ObjectGuid() && f.Bot.Target == f.Target.Guid,
              "one follow authorization without target or selection writes");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        Check(f.Run() == Result::Applied, "stay applies");
        Check(stays == 1 && stops == 1 && follows == 0 && usefulFollow == 0 && attacks == 0 && marks == 0,
              "one stop and nothing else");
        Check(!f.Bot.Moving && !f.Bot.HasUnitState(UNIT_STATE_CHASE | UNIT_STATE_FOLLOW) &&
                  f.AI.Values.StayTime.Data != 0 && f.AI.Values.MovingParam == "moving/self target",
              "stay stops chase and follow once and records its stay time");
        Check(f.Authorizations == 1, "one stay authorization");
    }
    for (Intent operation : {Intent::Follow, Intent::Stay})
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
        if (operation == Intent::Follow)
            oneShotDenials.push_back([](Fixture&) { followUseful = false; });
        else
        {
            oneShotDenials.push_back([](Fixture& f) { f.AI.Values.Moving.Data = false; });
            oneShotDenials.push_back([](Fixture&) { moveFromGroup = true; });
            oneShotDenials.push_back([](Fixture& f) { f.AI.Values.Positions.Data["stay"].Set = true; });
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
            Check(mutations == 0 && follows == 0 && stays == 0 && usefulFollow == 0, "one-shot refusals no effect");
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
        f.Denied();
        Check(f.Authorizations == 1 && usefulFollow == 0, "a follow in a leader-run group has no master to follow");
    }
    {
        Fixture f;
        f.OneShot(Intent::Stay);
        f.AI.Master = nullptr;
        Check(f.Run() == Result::Applied && stays == 1, "the group leader may stop a masterless bot once");
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
        Check(f.Run() == Result::Failed && stays == 1 && stops == 0 && f.Authorizations == 1,
              "stay failure one attempt");
    }
    std::cout << "PASS " << checks << " checks\n";
}
