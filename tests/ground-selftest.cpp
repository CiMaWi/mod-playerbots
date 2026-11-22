/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of GroundRules.h (pathing PA31: a bot gets to ground before it plans): a trip that ends with its bot
// in the air lands it (the cause of PA30's group A), and before a status pick or a route request a bot in the air
// comes down, one above its floor without a flying mount falls (group B), a bot on its floor without a polygon makes
// no move and writes one `none` row per spot (groups C, D). One row per use, none while a landing is under way.
// Run by tools/selftests.js (tests/selftests.json).

#include <cstdint>
#include <iostream>
#include <string>

#include "GroundRules.h"

using Ground::Act;
using Ground::Gate;
using Ground::Origin;
using TravelRoute::Point;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }
    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

float const NONE = TravelLegs::FLY_NO_GROUND;

// A bot at height yd over the ground at 90 (map 571), no water
Ground::State At(float height, bool flyingMount, bool flyingFlag = false)
{
    Ground::State state;
    state.flyingMount = flyingMount;
    state.flyingFlag = flyingFlag;
    state.bot = {571, 300.0f, 0.0f, 90.0f + height};
    state.groundZ = 90.0f;
    return state;
}

// The glue's order: the gate, then the floor; counts the mesh reads
struct Run
{
    Ground::Memo memo;
    int meshReads = 0;
    bool onMesh = true;

    Ground::Step Ask(Ground::State const& state, Origin origin, std::uint32_t nowMs, Gate* gate = nullptr)
    {
        Gate const g = Ground::GateOf(state, memo, origin, nowMs);
        if (gate)
            *gate = g;
        if (g == Gate::Skip)
            return {};
        if (g == Gate::Wait)
            return {Act::Wait, state.bot, 0.0f, false};
        return Ground::Decide(state, memo, origin, nowMs,
                              [this]
                              {
                                  ++meshReads;
                                  return onMesh;
                              });
    }
};

// The cause (group A's main origin, 48 %): a trip given up from outside (a status cut, a hold, a switch) while its
// bot flies. The trip ends as before; its end lands the bot
void CheckTripEnd()
{
    Run run;
    Ground::Step const down = run.Ask(At(113.0f, true, true), Origin::TripEnd, 5000);
    Check(down.act == Act::Descend && down.target.z == 90.0f && down.row && run.memo.downSinceMs == 5000,
          "trip end: 113 yd up on the flying mount, straight down to the floor, one row");
    Check(run.meshReads == 0, "trip end: the air needs no mesh read");

    Run fall;
    Ground::Step const drop = fall.Ask(At(40.0f, false, true), Origin::TripEnd, 5000);
    Check(drop.act == Act::Fall && drop.row, "trip end: 40 yd up with flying flags and no flying mount: it falls");

    Ground::State taxi = At(113.0f, true, true);
    taxi.skip = true;  // a taxi flight, a transport, in water, dead, in combat, falling: the glue sets skip
    Run skipped;
    Gate gate = Gate::Read;
    Check(skipped.Ask(taxi, Origin::TripEnd, 5000, &gate).act == Act::None && gate == Gate::Skip &&
              !skipped.memo.downSinceMs,
          "trip end: a taxi flight (or water, a transport) is not this rule's");

    Run floor;
    Ground::Step const stand = floor.Ask(At(0.5f, false), Origin::TripEnd, 5000);
    Check(stand.act == Act::None && !stand.row && floor.meshReads == 0,
          "trip end: on the floor nothing, and the mesh is not judged at a trip's end");
    floor.onMesh = false;
    Check(floor.Ask(At(0.5f, false), Origin::TripEnd, 5100).act == Act::None && floor.meshReads == 0,
          "trip end: two trip ends within CHECK_MS both read (no throttle), off the mesh still nothing");

    Run sitting;
    Check(sitting.Ask(At(1.0f, true, true), Origin::TripEnd, 5000).act == Act::None,
          "trip end: sitting on the flyer on the ground stays mounted (no dismount loop)");

    Run noFloor;
    Ground::State voidState = At(50.0f, true, true);
    voidState.groundZ = NONE;
    Check(noFloor.Ask(voidState, Origin::TripEnd, 5000).act == Act::None, "trip end: no floor found, nothing to do");
}

// The bound at the pick: a flyer in the air comes down once, one row, the landing's ticks wait, the end lands
void CheckAirAtPick()
{
    Run run;
    Ground::Step const down = run.Ask(At(113.0f, true, true), Origin::Pick, 10000);
    Check(down.act == Act::Descend && down.row && Ground::Holds(down.act) && down.height == 113.0f,
          "pick: in the air on the flying mount: flies down, one row, no pick this tick");
    Ground::State moving = At(60.0f, true, true);
    moving.moving = true;
    Gate gate = Gate::Read;
    Ground::Step const on = run.Ask(moving, Origin::Pick, 10200, &gate);
    Check(on.act == Act::Wait && !on.row && gate == Gate::Wait && Ground::Holds(on.act),
          "pick: a second pick while the descent runs: waits, no second row, no read");
    Ground::Step const again = run.Ask(At(60.0f, true, true), Origin::Pick, 12000);
    Check(again.act == Act::Descend && !again.row,
          "pick: the descent stopped short (another move took over): down again, no second row");
    int const reads = run.meshReads;
    Ground::Step const land = run.Ask(At(0.3f, true, true), Origin::Pick, 15000);
    Check(land.act == Act::Land && !land.row && !run.memo.downSinceMs && run.meshReads == reads,
          "pick: down on the floor after its descent: off the flyer, no row");
    // The Goal's "twice": the next pick sees a bot on its floor on the mesh and plans from there
    Ground::Step const plan = run.Ask(At(0.0f, false), Origin::Pick, 17000);
    Check(plan.act == Act::None && !plan.row && !Ground::Holds(plan.act) && run.meshReads == reads + 1,
          "pick: the next pick finds the bot on the mesh: it plans, no row");
}

// Group B: a bot above the engine's ground without a flying mount (4 to 15 yd over the nearest polygon)
void CheckAboveTheFloor()
{
    Run nine;
    Ground::Step const fall = nine.Ask(At(9.0f, false), Origin::Route, 1000);
    Check(fall.act == Act::Fall && fall.row && fall.target.z == 90.0f, "B: 9 yd up on foot: it falls, one row");
    Run mounted;
    Check(mounted.Ask(At(4.0f, false), Origin::Route, 1000).act == Act::Fall,
          "B: 4 yd up (a ground mount or none): it falls, as a player's client would");
    Run hover;
    Ground::Step const a2 = hover.Ask(At(3.0f, true, true), Origin::Pick, 1000);
    Check(a2.act == Act::Land && a2.row,
          "A2: on the flyer 3 yd over the floor (hearth refused: flying): it lands, one row");
    Run step;
    Check(step.Ask(At(Ground::STEP_YARDS, false), Origin::Pick, 1000).act == Act::None,
          "on the floor: within STEP_YARDS a bot stands on it");
    Run fell;
    fell.Ask(At(9.0f, false), Origin::Pick, 1000);
    fell.onMesh = false;
    Ground::Step const after = fell.Ask(At(0.0f, false), Origin::Pick, 3000);
    Check(after.act == Act::Stay && after.row && !Ground::Holds(after.act),
          "B: fallen onto a floor without a polygon: row none (PA42's place), no move, it plans on");
}

// Groups C and D: on the floor without a polygon. No move, one row per spot
void CheckOffMesh()
{
    Run run;
    run.onMesh = false;
    Ground::Step const first = run.Ask(At(0.0f, false), Origin::Pick, 1000);
    Check(first.act == Act::Stay && first.row && run.meshReads == 1, "C/D: off the mesh on its floor: row none");
    Gate gate = Gate::Read;
    run.Ask(At(0.0f, false), Origin::Pick, 1500, &gate);
    Check(gate == Gate::Skip && run.meshReads == 1, "C/D: a pick within CHECK_MS reads nothing");
    Ground::Step const same = run.Ask(At(0.0f, false), Origin::Pick, 3000);
    Check(same.act == Act::Stay && !same.row, "C/D: the same spot again: no second row");
    Ground::State away = At(0.0f, false);
    away.bot.x += 15.0f;
    Check(run.Ask(away, Origin::Pick, 5000).row, "C/D: 15 yd away, a new spot: a new row");
    Run onMesh;
    Ground::Step const fine = onMesh.Ask(At(0.0f, false), Origin::Route, 1000);
    Check(fine.act == Act::None && !fine.row && onMesh.meshReads == 1, "on the mesh: no act, no row");
    Run noFloor;
    Ground::State voidState = At(50.0f, false);
    voidState.groundZ = NONE;
    Ground::Step const lost = noFloor.Ask(voidState, Origin::Pick, 1000);
    Check(lost.act == Act::Stay && lost.row, "no floor found below: no move, row none");
}

void CheckBounds()
{
    // Every wait ends: a landing that outlives LANDING_MS is judged afresh, with a new row
    Run run;
    run.Ask(At(113.0f, true, true), Origin::Pick, 1000);
    Ground::State moving = At(100.0f, true, true);
    moving.moving = true;
    Gate gate = Gate::Read;
    run.Ask(moving, Origin::Pick, 1000 + Ground::LANDING_MS, &gate);
    Check(gate == Gate::Wait, "bound: still waiting at LANDING_MS");
    Ground::Step const late = run.Ask(moving, Origin::Pick, 1001 + Ground::LANDING_MS, &gate);
    Check(gate == Gate::Read && late.act == Act::Descend && late.row,
          "bound: past LANDING_MS the landing is judged afresh (a new row)");
    // Over the sea: down to the surface
    Run sea;
    Ground::State over = At(200.0f, true, true);
    over.groundZ = -500.0f;
    over.waterZ = 0.0f;
    over.bot.z = 200.0f;
    Ground::Step const surface = sea.Ask(over, Origin::Pick, 1000);
    Check(surface.act == Act::Descend && surface.target.z == 0.0f, "sea: down to the water's surface, not the seabed");
    Check(std::string(Ground::ActWord(Act::Descend)) == "flew_down" &&
              std::string(Ground::ActWord(Act::Fall)) == "fell" &&
              std::string(Ground::ActWord(Act::Land)) == "landed" && std::string(Ground::ActWord(Act::Stay)) == "none",
          "words: flew_down, fell, landed, none");
    Check(std::string(Ground::OriginWord(Origin::Pick)) == "pick" &&
              std::string(Ground::OriginWord(Origin::Route)) == "route" &&
              std::string(Ground::OriginWord(Origin::TripEnd)) == "trip_end",
          "words: pick, route, trip_end");
}
}  // namespace

int main()
{
    CheckTripEnd();
    CheckAirAtPick();
    CheckAboveTheFloor();
    CheckOffMesh();
    CheckBounds();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
