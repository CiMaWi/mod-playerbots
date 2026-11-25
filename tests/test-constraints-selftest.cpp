/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of a test's constraints on a bot (travel plan WT39, WT50; TestConstraints.h): every table row has its
// gate (hold rows), or a reader of its bit that the row's line checks plus an apply/undo pair where it has one
// (APPLIED); `hold` is the five hold rows, an unknown name is refused; a held
// bot's idle update picks no goal, status or errand; an order is taken while held and the bot holds again after the
// trip ends; a release clears only hold rows; any other row never ends the bot's status; orders merge later-wins per
// bit; a constraint change is taken also in combat, a trip or an aboard order once out of it.
// The model bot below follows PlayerbotAI::UpdateAI's order intake (TravelOrders::TakeAndApply), then
// NewRpgStatusUpdateAction::Execute's idle gate; its state rows run through TestConstraints::ChangeStates and
// KeepStates as the server's do (the engine's side, aggro and damage refusal, is a live test).
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/test-constraints-selftest.cpp

#include <cstdio>
#include <optional>
#include <string>

#include "TestConstraints.h"

namespace
{
using namespace TestConstraints;

int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name.c_str());
    (pass ? passed : failed)++;
}

struct Order
{
    int dest{0};
    std::string place;
    bool stop{false};
    std::uint32_t postedMs{0};
    std::uint32_t on{0};
    std::uint32_t off{0};
    std::uint32_t aboard{0};
    std::uint32_t quest{0};
    bool field{false};
    float minutes{0.0f};
    std::string status;
    std::uint32_t entry{0};
    bool flush{false};
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

// The units a state row changes: the bot and its pets
struct ModelUnits
{
    bool botImmune{false};
    bool petImmune{false};
    bool hasPet{true};
    bool god{false};
    bool traced{false};  // casttrace: the bot is in the combat record's registry (CombatRecord::Track)
    int applies{0};
};

void ApplyNoFight(ModelUnits& units)
{
    units.botImmune = true;
    if (units.hasPet)
        units.petImmune = true;
    ++units.applies;
}
void UndoNoFight(ModelUnits& units)
{
    units.botImmune = false;
    if (units.hasPet)
        units.petImmune = false;
}
void ApplyNoDamage(ModelUnits& units)
{
    units.god = true;
    ++units.applies;
}
void UndoNoDamage(ModelUnits& units) { units.god = false; }
void ApplyTrace(ModelUnits& units) { units.traced = true; }
void UndoTrace(ModelUnits& units) { units.traced = false; }

constexpr StateOps<ModelUnits> MODEL_OPS[] = {
    {NOFIGHT, ApplyNoFight, UndoNoFight}, {NODAMAGE, ApplyNoDamage, UndoNoDamage}, {CASTTRACE, ApplyTrace, UndoTrace}};
static_assert(Covered(MODEL_OPS) == APPLIED, "the model covers every applied row");

struct ModelBot
{
    std::uint32_t set{0};
    Status status{Status::Idle};
    std::optional<Order> pending;
    bool inCombat{false};
    ModelUnits units;
    std::uint32_t aboard{0};
    int constraintRows{0};

    void Post(Order order)
    {
        if (pending)
            Merge(*pending, std::move(order));
        else
            pending = std::move(order);
    }

    // The update: an order first (its constraints, its aboard, then its trip), then the idle gate
    Step Update(bool plannerHasGoal = true)
    {
        if (pending && TakeNow(*pending, inCombat))
        {
            Order const order = std::move(*pending);
            pending.reset();
            if (order.on || order.off)
            {
                std::uint32_t const before = set;
                set = Next(before, order.on, order.off);
                ChangeStates(MODEL_OPS, units, before, set);
                ++constraintRows;
                if (EndsStatus(before, set) && !HasTrip(order))
                    status = Status::Idle;
            }
            if (order.aboard)
                aboard = order.aboard;
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
        IdleGate const gate = GateOf(set);
        if (gate.planner && plannerHasGoal)
            return Step::Goal;
        if (gate.status)
            return Step::Pick;
        return Step::Nothing;
    }
};

Order On(std::uint32_t bits)
{
    Order order;
    order.on = bits;
    return order;
}

Order Off(std::uint32_t bits)
{
    Order order;
    order.off = bits;
    return order;
}

// `route release <bot> <kept>`
Order Release(std::uint32_t kept)
{
    Order order;
    order.on = kept;
    order.off = static_cast<std::uint32_t>(HOLD & ~kept);
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

Order Aboard(std::uint32_t entry)
{
    Order order;
    order.aboard = entry;
    return order;
}

// One line per table row: its gate reads the bit, or its reader and, where it has one, its apply/undo pair. A row
// without a line here fails, so a new row needs its harness line
bool RowWorks(Row const& row)
{
    std::string const name = row.name;
    if (row.kind == Kind::Hold)
    {
        if (name == "planner")
            return GateOf(0).planner && !GateOf(PLANNER).planner && GateOf(PLANNER).status;
        if (name == "status")
            return GateOf(0).status && !GateOf(STATUS).status && GateOf(STATUS).planner;
        if (name == "errand")
            return ErrandAllowed(0) && !ErrandAllowed(ERRAND) && ErrandAllowed(HOLD & ~ERRAND);
        if (name == "hearth")
            return HearthAllowed(0) && !HearthAllowed(HEARTH) && HearthAllowed(HOLD & ~HEARTH);
        if (name == "noquest")
            return QuestChoicesAllowed(0) && !QuestChoicesAllowed(NOQUEST) && QuestChoicesAllowed(HOLD & ~NOQUEST);
        return false;
    }
    ModelUnits units;
    ChangeStates(MODEL_OPS, units, 0, row.bit);
    ModelUnits const applied = units;
    ChangeStates(MODEL_OPS, units, row.bit, 0);
    if (name == "nofight")
        return applied.botImmune && applied.petImmune && !applied.god && !units.botImmune && !units.petImmune &&
               !StartsFights(NOFIGHT) && StartsFights(HOLD | NODAMAGE);
    if (name == "nodamage")
        return applied.god && !applied.botImmune && !units.god;
    // Fails only in the air and past the leg's middle; no other row fails a fly leg; nothing on the bot changes
    bool const untouched = !applied.god && !applied.botImmune && !applied.petImmune && applied.applies == 0;
    bool const failsRight = FlyLegFails(row.bit, 50.0f, 100.0f, true) && FlyLegFails(row.bit, 90.0f, 100.0f, true) &&
                            !FlyLegFails(row.bit, 49.0f, 100.0f, true) && !FlyLegFails(row.bit, 90.0f, 100.0f, false) &&
                            !FlyLegFails(HOLD | NOFIGHT | NODAMAGE, 90.0f, 100.0f, true) && !EndsStatus(0, row.bit);
    if (name == "flyfail")
        return untouched && failsRight && !FlyFailAborts(FLYFAIL) && !FlyFailAborts(STATES & ~FLYABORT);
    if (name == "flyabort")
        return untouched && failsRight && FlyFailAborts(FLYABORT) && FlyFailAborts(FLYFAIL | FLYABORT);
    // Read by the retreat trigger only: nothing to apply; no other row flees
    if (name == "flee")
        return untouched && Flees(FLEE) && !Flees(HOLD | (STATES & ~FLEE)) && StartsFights(FLEE) &&
               !EndsStatus(0, FLEE);
    // Read by world PvP's real master only: nothing to apply; no other row and not the hold sets it; it ends no status
    if (name == "realmaster")
        return untouched && HasRealMaster(REALMASTER) && !HasRealMaster(HOLD) &&
               !HasRealMaster(HOLD | (STATES & ~REALMASTER)) && StartsFights(REALMASTER) && !Flees(REALMASTER) &&
               (REALMASTER & HOLD) == 0 && !EndsStatus(0, REALMASTER);
    // Read by world PvP's place of a bot only: nothing to apply; no other row and not the hold sets it; it ends no
    // status
    if (name == "onsight")
        return untouched && OnSight(ONSIGHT) && !OnSight(HOLD) && !OnSight(HOLD | (STATES & ~ONSIGHT)) &&
               StartsFights(ONSIGHT) && !Flees(ONSIGHT) && !HasRealMaster(ONSIGHT) && (ONSIGHT & HOLD) == 0 &&
               !EndsStatus(0, ONSIGHT);
    // Only nouse stops the bot's own item use, only nospend puts it in the control cohort; nothing on the bot changes,
    // the hold never sets them, they end no status
    if (name == "nouse")
        return untouched && !UsesItemsOnItsOwn(NOUSE) && !UsesItemsOnItsOwn(HOLD | NOUSE) && UsesItemsOnItsOwn(0) &&
               UsesItemsOnItsOwn(STATES & ~NOUSE) && SpendsOnItsOwn(NOUSE) && (NOUSE & HOLD) == 0 &&
               !EndsStatus(0, NOUSE);
    if (name == "nospend")
        return untouched && !SpendsOnItsOwn(NOSPEND) && !SpendsOnItsOwn(HOLD | NOSPEND) && SpendsOnItsOwn(0) &&
               SpendsOnItsOwn(STATES & ~NOSPEND) && UsesItemsOnItsOwn(NOSPEND) && (NOSPEND & HOLD) == 0 &&
               !EndsStatus(0, NOSPEND);
    // Only nodestroy stops the destroy; nothing on the bot changes, the hold never sets it, it ends no status
    if (name == "nodestroy")
        return !applied.god && !applied.botImmune && !applied.petImmune && applied.applies == 0 &&
               !DestroysForSpace(NODESTROY) && !DestroysForSpace(HOLD | NODESTROY) && DestroysForSpace(0) &&
               DestroysForSpace(HOLD | NOFIGHT | NODAMAGE) && (NODESTROY & HOLD) == 0 && !EndsStatus(0, NODESTROY);
    // Only notaxi takes the flights out of a route request; nothing on the bot changes, the hold never sets it
    if (name == "notaxi")
        return !applied.god && !applied.botImmune && !applied.petImmune && applied.applies == 0 &&
               !TaxiAllowed(NOTAXI) && !TaxiAllowed(HOLD | NOTAXI) && TaxiAllowed(0) && TaxiAllowed(STATES & ~NOTAXI) &&
               TaxiAllowed(HOLD) && (NOTAXI & HOLD) == 0 && !EndsStatus(0, NOTAXI) && OwnFlyingAllowed(NOTAXI);
    // Only nocast stops the bot's own casts; nothing on the bot changes, the hold never sets it, it ends no status
    if (name == "nocast")
        return untouched && !CastsOnItsOwn(NOCAST) && !CastsOnItsOwn(HOLD | NOCAST) && CastsOnItsOwn(0) &&
               CastsOnItsOwn(STATES & ~NOCAST) && CastsOnItsOwn(HOLD) && (NOCAST & HOLD) == 0 && !EndsStatus(0, NOCAST);
    // Only novendor stops the wander status's vendor trades; nothing on the bot changes, the hold never sets it, it
    // ends no status
    if (name == "novendor")
        return untouched && !VendorStops(NOVENDOR) && !VendorStops(HOLD | NOVENDOR) && VendorStops(0) &&
               VendorStops(STATES & ~NOVENDOR) && VendorStops(HOLD) && ErrandAllowed(NOVENDOR) &&
               (NOVENDOR & HOLD) == 0 && !EndsStatus(0, NOVENDOR);
    // Only noplan drops the planner's answers (the bot falls back, planning WT63e); nothing on the bot changes, the
    // hold never sets it, it ends no status, the planner row stays free
    if (name == "noplan")
        return untouched && !PlansTaken(NOPLAN) && !PlansTaken(HOLD | NOPLAN) && PlansTaken(0) &&
               PlansTaken(STATES & ~NOPLAN) && PlansTaken(HOLD) && GateOf(NOPLAN).planner && (NOPLAN & HOLD) == 0 &&
               !EndsStatus(0, NOPLAN);
    // Only shortgiveup shortens the quest give-up limit; nothing on the bot changes, the hold never sets it, it ends no
    // status
    if (name == "shortgiveup")
        return untouched && GiveUpShortened(SHORTGIVEUP) && GiveUpShortened(HOLD | SHORTGIVEUP) &&
               !GiveUpShortened(0) && !GiveUpShortened(STATES & ~SHORTGIVEUP) && !GiveUpShortened(HOLD) &&
               (SHORTGIVEUP & HOLD) == 0 && !EndsStatus(0, SHORTGIVEUP);
    // Only anypath makes the skill path take the fastest climb; nothing on the bot changes, the hold never sets it, it
    // ends no status
    if (name == "anypath")
        return untouched && TakesAnyPath(ANYPATH) && TakesAnyPath(HOLD | ANYPATH) && !TakesAnyPath(0) &&
               !TakesAnyPath(STATES & ~ANYPATH) && !TakesAnyPath(HOLD) && (ANYPATH & HOLD) == 0 &&
               !EndsStatus(0, ANYPATH);
    // Only passive stops the bot's own actions; nothing on the bot changes, the hold never sets it, it ends no status
    if (name == "passive")
        return untouched && !ActsOnItsOwn(PASSIVE) && !ActsOnItsOwn(HOLD | PASSIVE) && ActsOnItsOwn(0) &&
               ActsOnItsOwn(STATES & ~PASSIVE) && ActsOnItsOwn(HOLD) && (PASSIVE & HOLD) == 0 &&
               !EndsStatus(0, PASSIVE);
    // Only nofactory keeps the random-bot manager off the bot; nothing on the bot changes, the hold never sets it, it
    // ends no status
    if (name == "nofactory")
        return untouched && !FactoryMayRun(NOFACTORY) && !FactoryMayRun(HOLD | NOFACTORY) && FactoryMayRun(0) &&
               FactoryMayRun(STATES & ~NOFACTORY) && FactoryMayRun(HOLD) && (NOFACTORY & HOLD) == 0 &&
               !EndsStatus(0, NOFACTORY);
    // Only nofly takes the bot's own flying out of a route request (taxis stay); nothing on the bot changes, the hold
    // never sets it, it ends no status
    if (name == "nofly")
        return untouched && !OwnFlyingAllowed(NOFLY) && !OwnFlyingAllowed(HOLD | NOFLY) && OwnFlyingAllowed(0) &&
               OwnFlyingAllowed(STATES & ~NOFLY) && OwnFlyingAllowed(HOLD) && TaxiAllowed(NOFLY) &&
               (NOFLY & HOLD) == 0 && !EndsStatus(0, NOFLY);
    // Only shortavoid shortens the record of failed attempts' expiry base; nothing on the bot changes, the hold never
    // sets it, it ends no status
    if (name == "shortavoid")
        return untouched && ShortAvoid(SHORTAVOID) && ShortAvoid(HOLD | SHORTAVOID) && !ShortAvoid(0) &&
               !ShortAvoid(STATES & ~SHORTAVOID) && !ShortAvoid(HOLD) && (SHORTAVOID & HOLD) == 0 &&
               !EndsStatus(0, SHORTAVOID) && SHORT_AVOID_MS == 20000;
    // Only walkflush writes a walk's MeshMiss rows where it is ordered and where it ends; nothing on the bot changes,
    // the hold never sets it, it ends no status
    if (name == "walkflush")
        return untouched && FlushesWalks(WALKFLUSH) && FlushesWalks(HOLD | WALKFLUSH) && !FlushesWalks(0) &&
               !FlushesWalks(STATES & ~WALKFLUSH) && !FlushesWalks(HOLD) && (WALKFLUSH & HOLD) == 0 &&
               !EndsStatus(0, WALKFLUSH) && StartsFights(WALKFLUSH) && RidesFree(WALKFLUSH) == false;
    // Only freeride prices the rides of a route request at no time; nothing on the bot changes, the hold never sets
    // it, it ends no status, the other route rows stay as they are
    if (name == "freeride")
        return untouched && RidesFree(FREERIDE) && RidesFree(HOLD | FREERIDE) && !RidesFree(0) &&
               !RidesFree(STATES & ~FREERIDE) && !RidesFree(HOLD) && TaxiAllowed(FREERIDE) &&
               OwnFlyingAllowed(FREERIDE) && (FREERIDE & HOLD) == 0 && !EndsStatus(0, FREERIDE);
    // Only casttrace traces the combat engine; nothing on the bot changes, the hold never sets it, it ends no status
    if (name == "casttrace")
        return !applied.god && !applied.botImmune && !applied.petImmune && applied.traced && !units.traced &&
               CombatTraced(CASTTRACE) && CombatTraced(HOLD | CASTTRACE) && !CombatTraced(0) &&
               !CombatTraced(STATES & ~CASTTRACE) && !CombatTraced(HOLD) && CastsOnItsOwn(CASTTRACE) &&
               (CASTTRACE & HOLD) == 0 && !EndsStatus(0, CASTTRACE);
    // Only agedeath ages a death (by 65 minutes, also a day-old one); nothing on the bot changes, the hold never sets
    // it, it ends no status
    if (name == "agedeath")
        return untouched && DeathAgeSeconds(30, AGEDEATH) == 30 + 65 * 60 &&
               DeathAgeSeconds(86400, HOLD | AGEDEATH) == 86400 + AGED_DEATH_SECONDS && DeathAgeSeconds(30, 0) == 30 &&
               DeathAgeSeconds(30, STATES & ~AGEDEATH) == 30 && DeathAgeSeconds(30, HOLD) == 30 &&
               (AGEDEATH & HOLD) == 0 && !EndsStatus(0, AGEDEATH);
    // Only nomount keeps the bot on foot; nothing on the bot changes, the hold never sets it, it ends no status
    if (name == "nomount")
        return !applied.god && !applied.botImmune && !applied.petImmune && applied.applies == 0 &&
               !MountsOnItsOwn(NOMOUNT) && !MountsOnItsOwn(HOLD | NOMOUNT) && MountsOnItsOwn(0) &&
               MountsOnItsOwn(HOLD | NOFIGHT | NODAMAGE | NODESTROY) && (NOMOUNT & HOLD) == 0 &&
               !EndsStatus(0, NOMOUNT);
    return false;
}

void TestTable()
{
    for (Row const& row : ROWS)
        Check(RowWorks(row), std::string("row ") + row.name + " (" + KindName(row.kind) + "): " +
                                 (row.kind == Kind::Hold ? "its gate reads it" : "its reader and apply then undo"));
    Check(HOLD == (PLANNER | STATUS | ERRAND | HEARTH | NOQUEST) && HOLD == CHOICES, "hold: the five hold rows");
    Check(Parse("planner,status,errand,hearth,noquest").set == HOLD &&
              Format(HOLD) == "planner,status,errand,hearth,noquest",
          "hold: parses and formats as the five hold rows");
    {
        Order questOrder;
        questOrder.quest = 7;
        Check(HasTrip(questOrder) && TakeNow(questOrder, false) && !TakeNow(questOrder, true),
              "quest order: counts as a trip (waits out of combat)");
        Order statusOrder;
        statusOrder.status = "grind";
        statusOrder.entry = 3;
        Order flushOrder;
        flushOrder.flush = true;
        Check(HasTrip(statusOrder) && !TakeNow(statusOrder, true) && !HasTrip(flushOrder) && TakeNow(flushOrder, true),
              "status order: counts as a trip; a flush does not and is taken in combat");
        Order passiveOrder;
        passiveOrder.on = PASSIVE;
        Order aboardOrder;
        aboardOrder.on = PASSIVE;
        aboardOrder.aboard = 9;
        Check(!NeedsNewRpg(passiveOrder) && NeedsNewRpg(questOrder) && NeedsNewRpg(statusOrder) &&
                  NeedsNewRpg(flushOrder) && NeedsNewRpg(aboardOrder),
              "new rpg: only a pure constraint change applies without it (a party member's passive, CFM5)");
        Merge(flushOrder, std::move(statusOrder));
        Check(flushOrder.flush && flushOrder.status == "grind" && flushOrder.entry == 3,
              "status order merged onto a flush: both stay");
    }
    Parsed const unknown = Parse("nofight,ghost");
    Check(unknown.set == 0 && unknown.bad == "ghost", "parse: an unknown name is refused with the word");
    Check(Parse("Hearth").set == 0 && Parse("Hearth").bad == "Hearth" && Parse("all").set == 0,
          "parse: names are exact (no case folding, no 'all')");
    Check(Parse("nofight nodamage").set == (NOFIGHT | NODAMAGE) &&
              Parse(" nofight, nodamage ").set == (NOFIGHT | NODAMAGE),
          "parse: commas or spaces between names");
    Check(Parse("").set == 0 && Parse("").bad.empty(), "parse: an empty list is no names, not an unknown one");
    Check(Format(0) == "none" && Format(NOFIGHT | HEARTH) == "hearth,nofight", "format: none and table order");
    Check(Names() ==
              "planner, status, errand, hearth, noquest, nofight, nodamage, flyfail, flyabort, nodestroy, notaxi, "
              "nomount, flee, nouse, nospend, realmaster, onsight, nofly, nocast, novendor, noplan, freeride, "
              "shortgiveup, walkflush, casttrace, anypath, shortavoid, agedeath, passive, nofactory",
          "names: every row for refusals");
    Check(
        Parse("flyfail").set == FLYFAIL && Format(FLYFAIL | NOFIGHT) == "nofight,flyfail" && (FLY_FAILS & CHOICES) == 0,
        "fly rows: state rows (hold never sets them), parsed and formatted");
    Check(Parse("fightready").set == 0 && Parse("joinroll").bad == "joinroll" && Parse("flee").set == FLEE &&
              Format(FLEE | FLYFAIL) == "flyfail,flee" && (FLEE & (CHOICES | FLY_FAILS | NODESTROY)) == 0,
          "fight rows: fightready and joinroll are gone (WP23), flee is a state row of its own");
    Check(Parse("nodestroy").set == NODESTROY && Format(NODESTROY | NOFIGHT) == "nofight,nodestroy",
          "nodestroy: a state row, parsed and formatted");
    Check(Parse("notaxi").set == NOTAXI && Format(NOTAXI | NOFIGHT) == "nofight,notaxi",
          "notaxi: a state row, parsed and formatted");
    Check(Parse("nofly,notaxi").set == (NOFLY | NOTAXI) && Format(NOFLY | NOTAXI) == "notaxi,nofly" &&
              (NOFLY & (CHOICES | NOTAXI | FLY_FAILS)) == 0,
          "nofly: a state row of its own beside notaxi, parsed and formatted");
    Check(Parse("freeride,nofight").set == (FREERIDE | NOFIGHT) && Format(FREERIDE | NOFLY) == "nofly,freeride" &&
              (FREERIDE & (CHOICES | NOTAXI | NOFLY | FLY_FAILS)) == 0,
          "freeride: a state row of its own beside the route rows, parsed and formatted");
    Check(Parse("walkflush,nofight").set == (WALKFLUSH | NOFIGHT) &&
              Format(WALKFLUSH | FREERIDE) == "freeride,walkflush" && (WALKFLUSH & (CHOICES | FREERIDE)) == 0,
          "walkflush: a state row of its own, parsed and formatted");
    Check(Parse("nouse,nospend").set == (NOUSE | NOSPEND) &&
              Format(NOUSE | NOSPEND | NOFIGHT) == "nofight,nouse,nospend" &&
              ((NOUSE | NOSPEND) & (CHOICES | FLY_FAILS | NODESTROY | FLEE)) == 0,
          "item use rows: nouse and nospend are state rows, parsed and formatted");
    Check(GiverSought(true, false, 0) && GiverSought(false, true, 0) && GiverSought(true, false, NOQUEST) &&
              GiverSought(true, true, NOQUEST) && !GiverSought(false, true, NOQUEST) && !GiverSought(false, false, 0) &&
              GiverSought(false, true, HOLD & ~NOQUEST),
          "noquest: the giver search seeks a giver for a turn-in, never for an accept the bot may not make");
}

void TestHeldIdle()
{
    ModelBot bot;
    Check(bot.Update() == Step::Goal, "free: the idle bot takes a planner goal");
    Check(bot.Update(false) == Step::Pick, "free: no goal, the weighted pick");
    bot.status = Status::Wander;
    bot.Post(On(HOLD));
    Check(bot.Update() == Step::Order && bot.status == Status::Idle && bot.constraintRows == 1,
          "hold: ends the current status, one Constraint row");
    bool still = true;
    for (int i = 0; i < 100; ++i)
        still = still && bot.Update() == Step::Nothing && bot.Update(false) == Step::Nothing;
    Check(still, "held: idle updates pick no goal and no status");
    Check(!ErrandAllowed(bot.set) && !HearthAllowed(bot.set), "held: no errand, no inn, no hearth");
    ModelBot idle;
    idle.Post(On(HOLD));
    idle.Update();
    Check(idle.constraintRows == 1, "hold on an idle bot: its Constraint row is written all the same");
}

void TestOrderWhileHeld()
{
    char const* const ends[] = {"arrived", "abort", "stop"};
    for (char const* end : ends)
    {
        ModelBot bot;
        bot.Post(On(HOLD));
        bot.Update();
        bot.Post(Go("goldshire"));
        Check(bot.Update() == Step::Order && bot.status == Status::Travel,
              "held: an order is taken at the next update, no release first");
        Check(bot.Update() == Step::Busy, "held: the trip runs");
        bot.Post(On(HOLD));  // a repeated hold on the trip does not end it
        bot.Update();
        Check(bot.status == Status::Travel, "held: a repeated hold keeps the order's trip");
        if (std::string(end) == "stop")
        {
            bot.Post(Stop());
            bot.Update();
        }
        else
            bot.status = Status::Idle;  // Travel arrived / abort:<reason> end in RpgStatus::End
        Check(bot.status == Status::Idle && bot.set == HOLD && bot.Update() == Step::Nothing &&
                  bot.Update(false) == Step::Nothing,
              std::string("held again after ") + end);
    }
}

void TestRelease()
{
    ModelBot bot;
    bot.Post(On(HOLD | NOFIGHT));
    bot.Update();
    bot.Post(Release(HEARTH));  // `route release <bot> hearth`
    bot.Update();
    Check(bot.Update() == Step::Goal && bot.Update(false) == Step::Pick, "partial release: planner and status free");
    Check(ErrandAllowed(bot.set) && !HearthAllowed(bot.set), "partial release: hearth stays off");
    bot.Post(Release(0));
    bot.Update();
    Check(bot.set == NOFIGHT && HearthAllowed(bot.set), "full release: no choice row kept");
    Check(bot.units.botImmune && !StartsFights(bot.set), "release: a state row stays (nofight)");
    ModelBot ready;
    ready.Post(On(HOLD | FLEE | NODESTROY));
    ready.Update();
    ready.Post(Release(0));
    ready.Update();
    Check(ready.set == (FLEE | NODESTROY), "release: flee and nodestroy stay");
    ModelBot moving;
    moving.status = Status::Travel;
    moving.Post(Release(0));
    moving.Update();
    Check(moving.status == Status::Travel, "release: does not end the current trip");
    Check(FreesErrands(HOLD, PLANNER | STATUS | HEARTH) && FreesErrands(ERRAND | NOFIGHT, NOFIGHT),
          "release: clearing the errand row frees the errand checks");
    Check(!FreesErrands(HOLD, HOLD) && !FreesErrands(0, 0) && !FreesErrands(HOLD & ~ERRAND, 0) &&
              !FreesErrands(0, ERRAND),
          "release: the errand checks stay as they are unless the errand row turns off");
    // ApplyTravelOrder's own errand-check forget (the order's before and after sets) is the same predicate
    int errandCases = 0;
    bool errandSame = true;
    constexpr std::uint32_t SUBSETS[] = {0,       ERRAND,           PLANNER,           ERRAND | PLANNER,
                                         NOFIGHT, ERRAND | NOFIGHT, PLANNER | NOFIGHT, ERRAND | PLANNER | NOFIGHT};
    for (std::uint32_t const before : SUBSETS)
        for (std::uint32_t const on : SUBSETS)
            for (std::uint32_t const off : SUBSETS)
            {
                std::uint32_t const after = Next(before, on, off);
                errandSame =
                    errandSame && FreesErrands(before, after) == (!ErrandAllowed(before) && ErrandAllowed(after));
                errandSame = errandSame && !FreesErrands(before, Next(before, 0, 0)) && Next(before, 0, 0) == before;
                ++errandCases;
            }
    Check(errandSame && errandCases == 512, "release: FreesErrands equals the errand row turning from held to free");
    // A release (professions WR30): some hold row turns off. The planner goals end there (goal-apply selftest)
    Check(Releases(HOLD, Next(HOLD, 0, HOLD)) && Releases(HOLD, NOQUEST) && Releases(PLANNER | NOFIGHT, NOFIGHT),
          "release: a hold row turning off is a release, also with rows kept");
    Check(!Releases(0, Next(0, 0, HOLD)) && !Releases(HOLD, HOLD) && !Releases(NOFIGHT | NODAMAGE, 0) &&
              !Releases(0, HOLD),
          "release: no hold row turning off (a free bot, a hold, a state row) is none");
}

void TestStates()
{
    ModelBot bot;
    bot.status = Status::Wander;
    bot.Post(On(NOFIGHT | NODAMAGE));
    bot.Update();
    Check(bot.status == Status::Wander && bot.units.botImmune && bot.units.god,
          "state rows: applied, the current status runs on");
    Check(bot.Update() == Step::Busy, "state rows: no hold, the bot plays on");
    bot.Post(Off(NODAMAGE));
    bot.Update();
    Check(bot.units.botImmune && !bot.units.god && bot.status == Status::Wander, "state off: only its row undone");
    ModelUnits later;
    later.hasPet = false;
    ChangeStates(MODEL_OPS, later, 0, NOFIGHT);
    later.hasPet = true;  // summoned after the constraint was set
    KeepStates(MODEL_OPS, later, NOFIGHT);
    Check(later.botImmune && later.petImmune && !later.god, "keep: a pet summoned later gets nofight");
    ModelUnits none;
    KeepStates(MODEL_OPS, none, HOLD);
    Check(none.applies == 0, "keep: no state row set, nothing applied");
}

void TestMerge()
{
    ModelBot bot;
    bot.Post(On(HOLD));
    bot.Post(Go("goldshire"));
    Check(bot.pending && bot.pending->on == HOLD && bot.pending->place == "goldshire",
          "merge: hold then go keeps both");
    bot.Update();
    Check(bot.set == HOLD && bot.status == Status::Travel, "merge: held and travelling after one update");
    ModelBot other;
    other.Post(Go("goldshire"));
    other.Post(On(HOLD));
    Check(other.pending->place == "goldshire" && other.pending->on == HOLD, "merge: go then hold keeps both");
    other.Post(Stop());
    Check(other.pending->stop && other.pending->on == HOLD, "merge: a later stop replaces the trip, keeps the hold");
    other.Post(Go("stormwind"));
    Check(!other.pending->stop && other.pending->place == "stormwind", "merge: a later go replaces the stop");
    ModelBot bits;
    bits.Post(On(NOFIGHT | HEARTH));
    bits.Post(Off(NOFIGHT));
    Check(bits.pending->on == HEARTH && bits.pending->off == NOFIGHT, "merge: a later off wins for its bit only");
    bits.Post(On(NOFIGHT));
    Check(bits.pending->on == (HEARTH | NOFIGHT) && bits.pending->off == 0, "merge: a later on wins back");
    bits.Post(Aboard(20808));
    bits.Update();
    Check(bits.set == (HEARTH | NOFIGHT) && bits.aboard == 20808, "merge: constraints and aboard in one take");
}

void TestTakeAtNextUpdate()
{
    ModelBot bot;
    bot.status = Status::Wander;
    bot.inCombat = true;
    bot.Post(On(HOLD));
    Check(bot.Update() == Step::Order && bot.set == HOLD && bot.status == Status::Idle,
          "hold: applied at the next update, also in combat");
    bot.Post(On(NOFIGHT));
    Check(bot.Update() == Step::Order && bot.units.botImmune, "nofight: applied in combat (it ends the fight)");
    bot.Post(Go("goldshire"));
    Check(bot.Update() == Step::Nothing && bot.pending, "trip: waits while the bot fights");
    bot.inCombat = false;
    Check(bot.Update() == Step::Order && bot.status == Status::Travel, "trip: taken at the first update out of combat");
    ModelBot merged;
    merged.inCombat = true;
    merged.Post(On(HOLD));
    merged.Post(Go("goldshire"));
    Check(merged.Update() != Step::Order && merged.pending, "hold with a trip: both wait for the end of combat");
    merged.inCombat = false;
    Check(merged.Update() == Step::Order && merged.set == HOLD && merged.status == Status::Travel,
          "hold with a trip: both at the first update out of combat");
    ModelBot boarding;
    boarding.inCombat = true;
    boarding.Post(Aboard(20808));
    Check(boarding.Update() != Step::Order && boarding.pending && !boarding.aboard,
          "aboard: waits while the bot fights");
}
}  // namespace

int main()
{
    TestTable();
    TestHeldIdle();
    TestOrderWhileHeld();
    TestRelease();
    TestStates();
    TestMerge();
    TestTakeAtNextUpdate();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
