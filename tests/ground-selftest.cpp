/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of GroundRules.h (pathing PA31: a bot gets to ground before it plans): a trip that ends with its bot
// in the air lands it (the cause of PA30's group A), and before a status pick or a route request a bot in the air
// comes down, one above its floor without a flying mount falls (group B), a bot on its floor without a polygon makes
// no move and writes one `none` row per spot (groups C, D). One row per use, none while a descent is under way.
// PA34: a fall or a landing that left the bot up is judged afresh, with a row (G225); a bot standing on a polygon's
// surface without a flying mount or flags does not fall (G229: a floor far below a ledge's edge dropped it through a
// city), one 10 yd over a slope whose polygon only touches the walk's start box does (G233); the bound waits while the
// fall's spline runs, so no route request cuts it (G236).
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

// The glue's order: the gate, then the floor; counts the mesh reads. The mesh: onMesh false = no polygon in the box,
// else the nearest polygon's surface meshDepth yd under the bot's feet (0: it stands on it); meshKnown false = no tile
struct Run
{
    Ground::Memo memo;
    int meshReads = 0;
    bool onMesh = true;
    float meshDepth = 0.0f;
    bool meshKnown = true;

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
                              [this, &state]
                              {
                                  ++meshReads;
                                  Ground::MeshRead mesh;
                                  if (meshKnown)
                                  {
                                      mesh.kind = onMesh ? Ground::MeshRead::Kind::Found : Ground::MeshRead::Kind::None;
                                      mesh.z = state.bot.z - meshDepth;
                                  }
                                  return mesh;
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
    nine.onMesh = false;  // 9 yd over the polygon below: outside the walk's start box
    Ground::Step const fall = nine.Ask(At(9.0f, false), Origin::Route, 1000);
    Check(fall.act == Act::Fall && fall.row && fall.target.z == 90.0f && nine.meshReads == 1 && nine.memo.fell,
          "B: 9 yd up on foot, off the mesh: it falls, one row, the bound waits for the fall's spline");
    Run mounted;
    mounted.onMesh = false;
    Check(mounted.Ask(At(4.0f, false), Origin::Route, 1000).act == Act::Fall,
          "B: 4 yd up off the mesh (a ground mount or none): it falls, as a player's client would");
    // G229: Elali in Orgrimmar stood 2 yd beside a polygon at its height, the reading was the ground under the city
    Run ledge;
    Ground::Step const stay = ledge.Ask(At(19.0f, false), Origin::Route, 1000);
    Check(stay.act == Act::None && !stay.row && !Ground::Holds(stay.act) && ledge.meshReads == 1 &&
              !ledge.memo.downSinceMs,
          "G229: 19 yd over the reading, no flyer, on the mesh: no fall, no row, it plans on");
    Run ledgeEnd;
    Check(ledgeEnd.Ask(At(19.0f, false), Origin::TripEnd, 1000).act == Act::None,
          "G229: the same at a trip's end: no fall");
    Run flags;
    Check(flags.Ask(At(19.0f, false, true), Origin::Route, 1000).act == Act::Fall && flags.meshReads == 0,
          "G229: flying flags left by a dismount in the air: it falls, no mesh read");
    // G233: Everlook (1, 6677, -4741, 713), 10 yd over the terrain: the walk's start box touches a slope's polygon, its
    // nearest point lies 9 yd below the bot's feet; it does not stand there and falls
    Run slope;
    slope.meshDepth = 9.0f;
    Ground::Step const drop = slope.Ask(At(10.0f, false), Origin::Pick, 1000);
    Check(drop.act == Act::Fall && drop.row && slope.meshReads == 1,
          "G233: 10 yd up, the box's polygon 9 yd under its feet: it falls, one row");
    Run under;
    under.meshDepth = 4.5f;
    Check(under.Ask(At(10.0f, false), Origin::Route, 1000).act == Act::Fall,
          "G233: a polygon 4.5 yd under its feet (inside the start box, beyond STEP_YARDS): it falls");
    Run feet;
    feet.meshDepth = 0.1f;
    Check(feet.Ask(At(19.0f, false), Origin::Route, 1000).act == Act::None,
          "G233: Elali's polygon 0.1 yd under its feet (2.4 yd beside it): it stands, no fall");
    Run sunk;
    sunk.meshDepth = -1.5f;
    Check(sunk.Ask(At(19.0f, false), Origin::Route, 1000).act == Act::None,
          "G233: a polygon 1.5 yd over its feet (sunk into the floor): it stands, no fall");
    Run noTile;
    noTile.meshKnown = false;
    Check(noTile.Ask(At(10.0f, false), Origin::Route, 1000).act == Act::Fall,
          "G233: no tile to judge: the floor reading decides, it falls");
    Run noTileFloor;
    noTileFloor.meshKnown = false;
    Check(noTileFloor.Ask(At(0.0f, false), Origin::Pick, 1000).act == Act::None,
          "no tile to judge on its floor: no none row (a walk may start)");
    Run boxFloor;
    boxFloor.meshDepth = 4.5f;
    Check(boxFloor.Ask(At(0.0f, false), Origin::Pick, 1000).act == Act::None,
          "on its floor with a polygon in the start box (4.5 yd off): a walk starts there, no none row");
    Run hover;
    Ground::Step const a2 = hover.Ask(At(3.0f, true, true), Origin::Pick, 1000);
    Check(a2.act == Act::Land && a2.row,
          "A2: on the flyer 3 yd over the floor (hearth refused: flying): it lands, one row");
    Run step;
    Check(step.Ask(At(Ground::STEP_YARDS, false), Origin::Pick, 1000).act == Act::None,
          "on the floor: within STEP_YARDS a bot stands on it");
    Run fell;
    fell.onMesh = false;
    fell.Ask(At(9.0f, false), Origin::Pick, 1000);
    Ground::Step const after = fell.Ask(At(0.0f, false), Origin::Pick, 3000);
    Check(after.act == Act::Stay && after.row && !Ground::Holds(after.act),
          "B: fallen onto a floor without a polygon: row none (PA42's place), no move, it plans on");
}

// G225: a landing is instant. A flyer still hovering 3 yd up at its next read (the dismount left it there, or it
// mounted again) is judged afresh with a row: no landing window holds its pick or route request back
void CheckLandingIsInstant()
{
    Run hover;
    Ground::Step const first = hover.Ask(At(3.0f, true, true), Origin::Route, 1000);
    Check(first.act == Act::Land && first.row && hover.memo.fell,
          "G225: 3 yd up on the flyer: lands, one row, the bound waits only for the fall's spline");
    Gate gate = Gate::Read;
    Ground::Step const next = hover.Ask(At(3.0f, true, true), Origin::Route, 1000 + Ground::CHECK_MS, &gate);
    Check(gate == Gate::Read && next.act == Act::Land && next.row,
          "G225: still 3 yd up at the next read, not moving, on the flyer again: Land with a new row");
    hover.Ask(At(3.0f, true, true), Origin::Route, 1500 + Ground::CHECK_MS, &gate);
    Check(gate == Gate::Skip, "G225: between reads the gate skips (the caller plans on), no wait for LANDING_MS");
    Run fall;
    fall.onMesh = false;
    fall.Ask(At(9.0f, false), Origin::Pick, 1000);
    Ground::Step const again = fall.Ask(At(9.0f, false), Origin::Pick, 1000 + Ground::CHECK_MS);
    Check(again.act == Act::Fall && again.row, "G225: a fall that left the bot up is judged afresh, with a row");
}

// G236: the engine's fall is a spline (MoveFall); Tallien fell 192 yd in 14 reads of 13 yd each, a route request let
// through between reads cut the spline every time. The bound waits while it runs
void CheckFallWaits()
{
    Run run;
    run.onMesh = false;
    Ground::Step const drop = run.Ask(At(192.0f, false), Origin::Route, 1000);
    Check(drop.act == Act::Fall && drop.row, "G236: 192 yd up off the mesh: it falls, one row");
    Ground::State falling = At(150.0f, false);
    falling.moving = true;
    Gate gate = Gate::Read;
    Ground::Step const mid = run.Ask(falling, Origin::Route, 1000 + Ground::CHECK_MS + 200, &gate);
    Check(gate == Gate::Wait && mid.act == Act::Wait && Ground::Holds(mid.act) && !mid.row,
          "G236: the next route request while the fall's spline runs waits (no request cuts it), no row");
    int const reads = run.meshReads;
    run.onMesh = true;
    Ground::Step const down = run.Ask(At(0.0f, false), Origin::Route, 6000, &gate);
    Check(gate == Gate::Read && down.act == Act::None && !down.row && !run.memo.downSinceMs && !run.memo.fell &&
              run.meshReads == reads + 1,
          "G236: the spline ended on the floor: judged afresh, on the mesh, it plans on");
    Run cut;
    cut.onMesh = false;
    cut.Ask(At(192.0f, false), Origin::Route, 1000);
    Ground::Step const again = cut.Ask(At(150.0f, false), Origin::Route, 3000, &gate);
    Check(gate == Gate::Read && again.act == Act::Fall && again.row,
          "G236: a spline that stopped above the floor (cut by another move): judged afresh, a new fall with a row");
    Ground::State stuck = At(150.0f, false);
    stuck.moving = true;
    cut.Ask(stuck, Origin::Route, 3000 + Ground::LANDING_MS + 1, &gate);
    Check(gate == Gate::Read, "G236: every wait ends: a spline still running past LANDING_MS is read again");
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
    CheckLandingIsInstant();
    CheckFallWaits();
    CheckOffMesh();
    CheckBounds();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
