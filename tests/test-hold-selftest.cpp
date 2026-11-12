/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of a test's hold on a bot (travel plan WT39, HoldRules.h): a held bot's idle update picks no goal,
// status or errand; an order is taken while held; the bot holds again after the trip ends (arrived, abort, stop); a
// partial release keeps the named choices off; a hold and a trip posted before the next update both survive.
// WT39b: a held bot seeks no grind target; a hold is taken at the next AI update even in combat, a trip once out of it.
// The model bot below follows PlayerbotAI::UpdateAIInternal's order intake (TravelOrders::TakeAndApply), then
// NewRpgStatusUpdateAction::Execute's idle gate.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/test-hold-selftest.cpp

#include <cstdio>
#include <optional>
#include <string>

#include "HoldRules.h"

namespace
{
using namespace HoldRules;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

struct Order
{
    int dest{0};
    std::string place;
    bool stop{false};
    std::uint32_t postedMs{0};
    std::int16_t hold{NO_CHANGE};
};

enum class Status
{
    Idle,
    Travel,
    Wander
};

enum class Step
{
    Nothing,  // held: stands still
    Goal,     // a planner goal
    Pick,     // the weighted status pick
    Order,    // took an order
    Busy      // not idle: the current status runs
};

struct ModelBot
{
    std::uint8_t mask{0};
    Status status{Status::Idle};
    std::optional<Order> pending;
    bool inCombat{false};

    void Post(Order order)
    {
        if (pending)
            Merge(*pending, std::move(order));
        else
            pending = std::move(order);
    }

    // The status update: an order first (its hold, then its trip), then the idle gate
    Step Update(bool plannerHasGoal = true)
    {
        if (pending && TakeNow(*pending, inCombat))
        {
            Order const order = std::move(*pending);
            pending.reset();
            if (order.hold != NO_CHANGE)
            {
                mask = static_cast<std::uint8_t>(order.hold);
                if (mask && !HasTrip(order))
                    status = Status::Idle;  // a hold ends the current status
            }
            if (order.stop)
            {
                if (status == Status::Travel)
                    status = Status::Idle;
            }
            else if (HasTrip(order))
                status = Status::Travel;
            return Step::Order;
        }
        if (status != Status::Idle)
            return Step::Busy;
        IdleGate const gate = GateOf(mask);
        if (gate.planner && plannerHasGoal)
            return Step::Goal;
        if (gate.status)
            return Step::Pick;
        return Step::Nothing;
    }
};

Order Hold(std::uint8_t mask)
{
    Order order;
    order.hold = mask;
    return order;
}

Order Go(char const* place)
{
    Order order;
    order.place = place;
    order.dest = 1;
    return order;
}

Order Stop()
{
    Order order;
    order.stop = true;
    order.place = "stop";
    return order;
}

void TestParseFormat()
{
    Check(ParseChoice("planner") == PLANNER && ParseChoice("status") == STATUS && ParseChoice("errand") == ERRAND &&
              ParseChoice("hearth") == HEARTH,
          "parse: the four choice words");
    Check(ParseChoice("Hearth") == 0 && ParseChoice("") == 0 && ParseChoice("all") == 0, "parse: unknown words are 0");
    Check(Format(ALL) == "planner,status,errand,hearth", "format: all");
    Check(Format(0) == "none" && Format(HEARTH) == "hearth" && Format(PLANNER | ERRAND) == "planner,errand",
          "format: none and partial");
}

void TestHeldIdle()
{
    ModelBot bot;
    Check(bot.Update() == Step::Goal, "free: the idle bot takes a planner goal");
    Check(bot.Update(false) == Step::Pick, "free: no goal, the weighted pick");
    bot.status = Status::Wander;
    bot.Post(Hold(ALL));
    Check(bot.Update() == Step::Order && bot.status == Status::Idle, "hold: ends the current status");
    bool still = true;
    for (int i = 0; i < 100; ++i)
        still = still && bot.Update() == Step::Nothing && bot.Update(false) == Step::Nothing;
    Check(still, "held: idle updates pick no goal and no status");
    Check(!ErrandAllowed(bot.mask) && !HearthAllowed(bot.mask), "held: no errand, no inn, no hearth");
}

void TestOrderWhileHeld()
{
    char const* const ends[] = {"arrived", "abort", "stop"};
    for (char const* end : ends)
    {
        ModelBot bot;
        bot.Post(Hold(ALL));
        bot.Update();
        bot.Post(Go("goldshire"));
        bool const taken = bot.Update() == Step::Order && bot.status == Status::Travel;
        Check(taken, "held: an order is taken at the next update, no release first");
        Check(bot.Update() == Step::Busy, "held: the trip runs");
        if (std::string(end) == "stop")
        {
            bot.Post(Stop());
            bot.Update();
        }
        else
            bot.status = Status::Idle;  // Travel arrived / abort:<reason> end in ChangeToIdle
        std::string const name = std::string("held again after ") + end;
        Check(bot.status == Status::Idle && bot.mask == ALL && bot.Update() == Step::Nothing &&
                  bot.Update(false) == Step::Nothing,
              name.c_str());
    }
}

void TestRelease()
{
    ModelBot bot;
    bot.Post(Hold(ALL));
    bot.Update();
    bot.Post(Hold(HEARTH));  // `route release <bot> hearth`
    bot.Update();
    Check(bot.Update() == Step::Goal && bot.Update(false) == Step::Pick, "partial release: planner and status free");
    Check(ErrandAllowed(bot.mask) && !HearthAllowed(bot.mask), "partial release: hearth stays off");
    bot.Post(Hold(0));
    bot.Update();
    Check(bot.mask == 0 && HearthAllowed(bot.mask), "full release: nothing kept off");
    ModelBot moving;
    moving.status = Status::Travel;
    moving.Post(Hold(0));
    moving.Update();
    Check(moving.status == Status::Travel, "release: does not end the current trip");
}

void TestMerge()
{
    ModelBot bot;
    bot.Post(Hold(ALL));
    bot.Post(Go("goldshire"));
    Check(bot.pending && bot.pending->hold == ALL && bot.pending->place == "goldshire",
          "merge: hold then go keeps both");
    bot.Update();
    Check(bot.mask == ALL && bot.status == Status::Travel, "merge: held and travelling after one update");
    ModelBot other;
    other.Post(Go("goldshire"));
    other.Post(Hold(ALL));
    Check(other.pending->place == "goldshire" && other.pending->hold == ALL, "merge: go then hold keeps both");
    other.Post(Stop());
    Check(other.pending->stop && other.pending->hold == ALL, "merge: a later stop replaces the trip, keeps the hold");
    other.Post(Go("stormwind"));
    Check(!other.pending->stop && other.pending->place == "stormwind", "merge: a later go replaces the stop");
}
void TestGrind()
{
    ModelBot bot;
    Check(GrindAllowed(bot.mask), "free: the bot grinds");
    bot.Post(Hold(ALL));
    bot.Update();
    Check(!GrindAllowed(bot.mask), "held: no new grind target");
    bot.Post(Go("goldshire"));
    bot.Update();
    Check(bot.status == Status::Travel && !GrindAllowed(bot.mask), "held on an order's trip: no new grind target");
    bot.Post(Hold(HEARTH));  // `route release <bot> hearth`
    bot.Update();
    Check(GrindAllowed(bot.mask), "partial release (hearth kept off): the bot grinds again");
}

void TestTakeAtNextUpdate()
{
    ModelBot bot;
    bot.status = Status::Wander;
    bot.inCombat = true;
    bot.Post(Hold(ALL));
    Check(bot.Update() == Step::Order && bot.mask == ALL && bot.status == Status::Idle,
          "hold: applied at the next update, also in combat");
    bot.Post(Go("goldshire"));
    Check(bot.Update() == Step::Nothing && bot.pending, "trip: waits while the bot fights");
    bot.inCombat = false;
    Check(bot.Update() == Step::Order && bot.status == Status::Travel, "trip: taken at the first update out of combat");
    ModelBot merged;
    merged.inCombat = true;
    merged.Post(Hold(ALL));
    merged.Post(Go("goldshire"));
    Check(merged.Update() != Step::Order && merged.pending, "hold with a trip: both wait for the end of combat");
    merged.inCombat = false;
    Check(merged.Update() == Step::Order && merged.mask == ALL && merged.status == Status::Travel,
          "hold with a trip: both at the first update out of combat");
}
}  // namespace

int main()
{
    TestParseFormat();
    TestHeldIdle();
    TestOrderWhileHeld();
    TestRelease();
    TestMerge();
    TestGrind();
    TestTakeAtNextUpdate();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
