/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ERRANDHARNESS_H
#define _PLAYERBOT_ERRANDHARNESS_H

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "ErrandRules.h"
#include "FailedAttemptRules.h"
#include "UpkeepNeedRules.h"

// The trip planner's scenarios (travel plan WT15, the upkeep needs as providers WU12): synthetic bots and stops, one
// check per decision about which stops a trip makes and which errands it does or refuses there. Shared by
// tests/errand-trips-selftest.cpp. Player-free
namespace WorldTravel
{
namespace ErrandHarness
{
constexpr uint32_t MAP = 0;
constexpr float RUN_YARDS_PER_SECOND = 7.0f;
constexpr uint32_t BAND_LEVEL = 25;   // band 1: r 20, t 10, e 100
constexpr uint32_t REPAIR_HUB = 101;  // a vendor that repairs, in the hub
constexpr uint32_t PLAIN_VENDOR = 102;
constexpr uint32_t ROAD_VENDOR = 103;
constexpr uint32_t ENDER = 201;
constexpr uint32_t HUB_QUEST = 301;
constexpr uint32_t FAR_QUEST = 302;
constexpr uint32_t BAG_ITEM = 4496;
constexpr uint32_t BANKER = 501;
constexpr uint32_t TRAINER = 601;  // the bot's class trainer
constexpr uint32_t FAR_TRAINER = 602;
constexpr uint32_t RIDING_TRAINER = 603;
constexpr uint32_t MOUNT_VENDOR = 604;        // sells the mount of the bot's race
constexpr uint32_t OTHER_MOUNT_VENDOR = 605;  // sells another race's mount, cheaper
constexpr uint32_t CLASS_SPELL = 6673;
constexpr uint32_t WEAPON_MASTER = 606;
constexpr uint32_t PROFESSION_TRAINER = 607;
constexpr uint32_t PROFESSION_SPELL = 2018;  // Blacksmithing apprentice
constexpr uint32_t STATION = 608;            // an anvil's gameobject entry (WR14)
constexpr uint32_t CRAFT_SPELL = 2660;       // Rough Sharpening Stone
constexpr uint32_t CRAFT_FOCUS = 1;          // the anvil's spell focus
constexpr uint32_t WEAPON_SPELL = 198;       // One-Handed Maces
constexpr uint32_t BEAST = 299;              // a tameable beast's creature entry (WU21b)
constexpr uint32_t STABLE_MASTER = 9988;     // a stable master's creature entry (WU21c)
constexpr uint8_t RACE_HUMAN = 1;
constexpr uint8_t RACE_ORC = 2;
constexpr uint32_t CHEAPEST_REPAIR = 200;  // copper, the cheapest worn item
constexpr uint32_t FLUSH = 100000;         // copper
// A later plan's errand and service, appended after the travel plan's (the bank, WB)
constexpr ErrandKind BANK_KIND = static_cast<ErrandKind>(ERRAND_KIND_COUNT);
constexpr uint32_t SERVICE_BANK = 1u << 16;
constexpr float BANK_WORTH = 5000.0f;

struct Check
{
    std::string name;
    bool pass{false};
    std::string text;
};

inline void Add(std::vector<Check>& checks, std::string name, bool pass, std::string text)
{
    checks.push_back({std::move(name), pass, std::move(text)});
}

inline ErrandPlace At(float x, float y) { return {MAP, x, y, 0.0f}; }

inline ErrandSnapshot Bot(uint32_t freeSlots, uint32_t sellableSlots)
{
    ErrandSnapshot snapshot;
    snapshot.Level = BAND_LEVEL;
    snapshot.Money = FLUSH;
    snapshot.Rates = CurrentRates(SlotMeasure{}, DEFAULT_SLOT_BANDS[LevelBand(BAND_LEVEL)]);
    snapshot.BagHorizonHours = DEFAULT_SLOT_BANDS[LevelBand(BAND_LEVEL)].BagHorizonHours;
    snapshot.FreeSlots = freeSlots;
    snapshot.SellableSlots = sellableSlots;
    snapshot.MinutesToNextStop = 30.0f;
    return snapshot;
}

inline TripRequest Request(ErrandSnapshot const& snapshot, std::vector<TripStop> route = {})
{
    TripRequest request;
    request.Start = At(0.0f, 0.0f);
    request.Route = std::move(route);
    request.YardsPerSecond = RUN_YARDS_PER_SECOND;
    request.Rates = snapshot.Rates;
    return request;
}

inline TripPlan Plan(ErrandSnapshot const& snapshot, TripRequest const& request,
                     std::vector<ProviderRow> const& rows = TravelProviders())
{
    return PlanTrip(request, snapshot.Stops, CollectOffers(snapshot, rows));
}

inline bool HasErrand(TripStop const& stop, ErrandKind kind)
{
    for (ErrandOffer const& errand : stop.Errands)
        if (errand.Kind == kind)
            return true;
    return false;
}

inline std::string Describe(TripPlan const& plan)
{
    std::string text = "stops:";
    for (TripStop const& stop : plan.Stops)
    {
        text += " [" + std::to_string(stop.Id) + (stop.OnRoute ? " route" : " detour") + " " +
                std::to_string(static_cast<int>(stop.DetourMinutes * 10.0f) / 10.0f).substr(0, 4) + " min:";
        for (ErrandOffer const& errand : stop.Errands)
            text += std::string(" ") + ErrandKindName(errand.Kind);
        text += "]";
    }
    text += " refused:";
    for (Refusal const& refusal : plan.Refused)
        text += std::string(" ") + ErrandKindName(refusal.Offer.Kind) + "/" + refusal.Reason;
    return text;
}

// A turn-in, a sale and a repair at one hub: one stop at the repair vendor takes all three, no second detour
inline void OneHub(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(0, 12);
    bot.RepairDue = true;
    bot.Stops = {{REPAIR_HUB, At(300.0f, 0.0f), SERVICE_VENDOR | SERVICE_REPAIR},
                 {PLAIN_VENDOR, At(-200.0f, 0.0f), SERVICE_VENDOR}};
    bot.Completed = {{HUB_QUEST, ENDER, false, At(315.0f, 10.0f), 600.0f}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const one = plan.Stops.size() == 1 && plan.Refused.empty();
    bool const all = one && plan.Stops[0].Id == REPAIR_HUB && HasErrand(plan.Stops[0], ErrandKind::TurnIn) &&
                     HasErrand(plan.Stops[0], ErrandKind::Sell) && HasErrand(plan.Stops[0], ErrandKind::Repair);
    Add(checks, "hub: a turn-in, a sale and a repair are done at one stop", all, Describe(plan));

    TripPlan const again = Plan(bot, Request(bot));
    Add(checks, "hub: the same snapshot plans the same trip", Describe(again) == Describe(plan), Describe(again));
}

// A bag sold by a vendor the route passes anyway: it joins that stop, no detour
inline void BagOnTheWay(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(2, 0);
    TripStop road;
    road.Place = At(600.0f, 0.0f);
    road.Services = SERVICE_VENDOR;
    road.Id = ROAD_VENDOR;
    bot.Stops = {{PLAIN_VENDOR, At(-500.0f, 0.0f), SERVICE_VENDOR}};
    bot.Bags = {{BAG_ITEM, ROAD_VENDOR, At(605.0f, 3.0f), 500, 10, 6}};
    std::vector<ErrandOffer> const offers = CollectOffers(bot, TravelProviders());
    TripPlan const plan = Plan(bot, Request(bot, {road}));
    bool const joined = !offers.empty() && plan.Stops.size() == 1 && plan.Stops[0].OnRoute &&
                        plan.Stops[0].DetourMinutes == 0.0f && HasErrand(plan.Stops[0], ErrandKind::BuyBag) &&
                        plan.Refused.empty();
    Add(checks, "on the way: a bag at a vendor on the route joins it at no detour", joined, Describe(plan));
}

// A turn-in far off the route, worth less than its detour: refused, the route stays as it was
inline void DetourRefused(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Completed = {{FAR_QUEST, ENDER, false, At(300.0f, 500.0f), 50.0f}};
    TripStop destination;
    destination.Place = At(600.0f, 0.0f);
    destination.Services = SERVICE_QUEST_ENDER;
    TripPlan const plan = Plan(bot, Request(bot, {destination}));
    bool const refused = plan.Refused.size() == 1 && std::string(plan.Refused[0].Reason) == "detour_not_worth" &&
                         plan.Refused[0].CostCopper > plan.Refused[0].Offer.WorthCopper;
    Add(checks, "detour: a turn-in worth less than its detour is refused", refused, Describe(plan));
    bool const unchanged = plan.Stops.size() == 1 && plan.Stops[0].OnRoute && plan.Stops[0].Errands.empty();
    Add(checks, "detour: a refused detour leaves the route unchanged", unchanged, Describe(plan));

    ErrandSnapshot rich = bot;
    rich.Completed[0].WorthCopper = 5000.0f;
    TripPlan const richPlan = Plan(rich, Request(rich, {destination}));
    bool const taken = richPlan.Stops.size() == 2 && !richPlan.Stops[0].OnRoute &&
                       HasErrand(richPlan.Stops[0], ErrandKind::TurnIn) && richPlan.Refused.empty();
    Add(checks, "detour: the same turn-in worth more than its detour gets one, before the destination", taken,
        Describe(richPlan));
}

// Two errands each worth less than the detour, worth more together: one stop does both (a sale and a turn-in whose
// ender stands at the vendor, worth the detour less half the sale)
inline void TwoAtOneStop(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(6, 3);
    bot.Level = 45;  // band 2: r 25, t 8, e 250
    bot.Rates = CurrentRates(SlotMeasure{}, DEFAULT_SLOT_BANDS[LevelBand(bot.Level)]);
    bot.Stops = {{REPAIR_HUB, At(150.0f, 0.0f), SERVICE_VENDOR | SERVICE_REPAIR}};
    std::vector<ErrandOffer> const sale = CollectOffers(bot, TravelProviders());
    float const saleWorth = sale.empty() ? 0.0f : sale[0].WorthCopper;
    float const cost = DetourCost(bot.Rates, WalkMinutes(150.0f, RUN_YARDS_PER_SECOND));
    bot.Completed = {{HUB_QUEST, ENDER, false, At(150.0f, 0.0f), cost - saleWorth / 2.0f}};
    ErrandSnapshot sellOnly = bot;
    sellOnly.Completed.clear();
    ErrandSnapshot turnInOnly = bot;
    turnInOnly.SellableSlots = 0;
    bool const aloneRefused = saleWorth > 0.0f && Plan(sellOnly, Request(sellOnly)).Stops.empty() &&
                              Plan(turnInOnly, Request(turnInOnly)).Stops.empty();
    TripPlan const plan = Plan(bot, Request(bot));
    bool const both =
        aloneRefused && plan.Stops.size() == 1 && plan.Stops[0].Errands.size() == 2 && plan.Refused.empty();
    Add(checks, "stop: two errands not worth a detour alone share one stop", both,
        std::string(aloneRefused ? "alone refused; " : "alone taken; ") + Describe(plan));
}

// WT3's sale rule is unchanged: a trip just for selling opens exactly when TripWorthIt says so
inline void SaleRuleKept(std::vector<Check>& checks)
{
    uint32_t cases = 0;
    uint32_t mismatches = 0;
    uint32_t trips = 0;
    for (uint32_t const level : {10u, 25u, 45u, 75u})
        for (uint32_t const free : {0u, 2u, 5u})
            for (uint32_t const sellable : {0u, 1u, 4u, 12u})
                for (float const yards : {50.0f, 400.0f, 1500.0f, 5000.0f})
                {
                    ErrandSnapshot bot = Bot(free, sellable);
                    bot.Level = level;
                    bot.Rates = CurrentRates(SlotMeasure{}, DEFAULT_SLOT_BANDS[LevelBand(level)]);
                    bot.Stops = {{PLAIN_VENDOR, At(yards, 0.0f), SERVICE_VENDOR}};
                    TripInput in;
                    in.Rates = bot.Rates;
                    in.FreeSlots = free;
                    in.SellableSlots = sellable;
                    in.WalkMinutes = WalkMinutes(yards, RUN_YARDS_PER_SECOND);
                    in.MinutesToNextStop = bot.MinutesToNextStop;
                    bool const opened = !Plan(bot, Request(bot)).Stops.empty();
                    ++cases;
                    trips += opened ? 1 : 0;
                    mismatches += opened != TripWorthIt(in) ? 1 : 0;
                }
    Add(checks, "sale: a trip just for selling follows TripWorthIt", !mismatches && trips && trips < cases,
        std::to_string(cases) + " cases, " + std::to_string(trips) + " trips, " + std::to_string(mismatches) +
            " mismatches");
}

// Greys are no stock (upkeep WU31): a bag whose sellable stacks are all grey counts no sellable stack, so no trip opens
// for it at any distance, while the same bag with one white stack opens one exactly where TripWorthIt says so
inline void GreyOnlyNoTrip(std::vector<Check>& checks)
{
    constexpr uint32_t STACKS = 12;
    uint32_t greyOnly = 0;
    uint32_t oneWhite = 0;
    for (uint32_t stack = 0; stack < STACKS; ++stack)
    {
        greyOnly += CountsAsSellable(true, true) ? 1 : 0;
        oneWhite += CountsAsSellable(stack == 0 ? false : true, true) ? 1 : 0;
    }

    uint32_t greyTrips = 0;
    uint32_t whiteTrips = 0;
    uint32_t whiteMismatches = 0;
    for (uint32_t const level : {10u, 25u, 45u})
        for (uint32_t const free : {0u, 2u})
            for (float const yards : {50.0f, 400.0f, 1500.0f})
            {
                ErrandSnapshot grey = Bot(free, greyOnly);
                grey.Level = level;
                grey.Rates = CurrentRates(SlotMeasure{}, DEFAULT_SLOT_BANDS[LevelBand(level)]);
                grey.Stops = {{PLAIN_VENDOR, At(yards, 0.0f), SERVICE_VENDOR}};
                greyTrips += Plan(grey, Request(grey)).Stops.empty() ? 0 : 1;

                ErrandSnapshot white = grey;
                white.SellableSlots = oneWhite;
                TripInput in;
                in.Rates = white.Rates;
                in.FreeSlots = free;
                in.SellableSlots = oneWhite;
                in.WalkMinutes = WalkMinutes(yards, RUN_YARDS_PER_SECOND);
                in.MinutesToNextStop = white.MinutesToNextStop;
                bool const opened = !Plan(white, Request(white)).Stops.empty();
                whiteTrips += opened ? 1 : 0;
                whiteMismatches += opened != TripWorthIt(in) ? 1 : 0;
            }
    Add(checks, "grey: a bag of greys alone opens no trip at any distance, one white stack follows TripWorthIt",
        greyOnly == 0 && oneWhite == 1 && !greyTrips && !whiteMismatches,
        std::to_string(greyOnly) + " grey-only sellable, " + std::to_string(greyTrips) + " trips; white " +
            std::to_string(oneWhite) + " sellable, " + std::to_string(whiteTrips) + " trips, " +
            std::to_string(whiteMismatches) + " mismatches");
}

// An errand trip's end (the status core's errand clean-up): every end stamps the check, only a healthy trip with a
// visited stop adds its minutes
inline void ErrandEnds(std::vector<Check>& checks)
{
    SlotMeasure aborted;
    ErrandEndRow const abortRow = ErrandEnd(aborted, 1000, 7.0f, false, false);
    Add(checks, "end: a trip without a visited stop stamps the check, records errand_abort and adds no minutes",
        abortRow == ErrandEndRow::Abort && aborted.ErrandChecked && aborted.ErrandCheckMs == 1000 && !aborted.HasTrip,
        ErrandEndWord(abortRow));
    SlotMeasure run;
    ErrandEndRow const runRow = ErrandEnd(run, 2000, 7.0f, true, false);
    Add(checks, "end: a trip with a visited stop stamps the check, records errand_run and adds its minutes",
        runRow == ErrandEndRow::Run && run.ErrandChecked && run.HasTrip && run.TripMinutes == 7.0f,
        ErrandEndWord(runRow));
    SlotMeasure sick;
    ErrandEndRow const sickRow = ErrandEnd(sick, 3000, 7.0f, true, true);
    Add(checks, "end: a sick end after a visited stop stamps the check, records errand_sick and adds no minutes",
        sickRow == ErrandEndRow::Sick && sick.ErrandChecked && !sick.HasTrip, ErrandEndWord(sickRow));
    // CS14d: a stuck walk is a not-sick end, now cleaned up (the status core runs this for Outcome::Stuck)
    SlotMeasure stuckEarly;
    ErrandEndRow const stuckEarlyRow = ErrandEnd(stuckEarly, 4000, 5.0f, false, false);
    Add(checks, "end: stuck before any stop stamps the check, records errand_abort and adds no minutes",
        stuckEarlyRow == ErrandEndRow::Abort && stuckEarly.ErrandChecked && stuckEarly.ErrandCheckMs == 4000 &&
            !stuckEarly.HasTrip,
        ErrandEndWord(stuckEarlyRow));
    SlotMeasure stuckLate;
    ErrandEndRow const stuckLateRow = ErrandEnd(stuckLate, 5000, 5.0f, true, false);
    Add(checks, "end: stuck after a visited stop stamps the check, records errand_run and adds its minutes",
        stuckLateRow == ErrandEndRow::Run && stuckLate.ErrandChecked && stuckLate.HasTrip &&
            stuckLate.TripMinutes == 5.0f,
        ErrandEndWord(stuckLateRow));
}

// The trigger's needs check (WU27, NeedsCheckDue as InventoryPressureTrigger calls it): "nothing due" is computed once
// per retry time; a due answer, the release of a hold and the other errand paths are not held by it
inline void NeedsCheckOnce(std::vector<Check>& checks)
{
    constexpr uint32_t CHECK_MS = 30 * 1000;  // InventoryPressureTrigger's interval
    uint32_t computations = 0;
    bool answer = false;
    auto const due = [&computations, &answer]
    {
        ++computations;
        return answer;
    };
    auto const quiet = [&due](SlotMeasure& measure, uint32_t start)
    {
        bool fired = NeedsCheckDue(measure, start, due);
        for (uint32_t at = CHECK_MS; at < ERRAND_RETRY_MS; at += CHECK_MS)
            fired = NeedsCheckDue(measure, start + at, due) || fired;
        return NeedsCheckDue(measure, start + ERRAND_RETRY_MS - 1, due) || fired;
    };
    SlotMeasure idle;
    bool const idleFired = quiet(idle, 1000);
    uint32_t const inWindow = computations;
    NeedsCheckDue(idle, 1000 + ERRAND_RETRY_MS, due);
    Add(checks, "nothing due: no second needs computation before the retry time",
        !idleFired && inWindow == 1 && computations == 2 && idle.NeedsCheckMs == 1000 + ERRAND_RETRY_MS &&
            !idle.ErrandChecked,
        std::to_string(inWindow) + " computations in the retry time, " + std::to_string(computations) + " after it");

    computations = 0;
    answer = true;
    SlotMeasure wanted;
    bool const first = NeedsCheckDue(wanted, 1000, due);
    bool const again = NeedsCheckDue(wanted, 1000 + CHECK_MS, due);
    Add(checks, "nothing due: a due answer is not stamped (the start's errand check stamps)",
        first && again && computations == 2 && !wanted.NeedsChecked, std::to_string(computations) + " computations");

    computations = 0;
    answer = false;
    SlotMeasure held;
    NeedsCheckDue(held, 1000, due);
    StampErrandCheck(held, 1000);
    ForgetErrandChecks(held);
    bool const released = !NeedsCheckDue(held, 1000 + CHECK_MS, due) && computations == 2;
    Add(checks, "nothing due: a released hold forgets both stamps, the next check computes at once",
        released && ErrandRetryOver(held, 1000 + CHECK_MS),
        std::to_string(computations) + " computations, errand check " + (held.ErrandChecked ? "kept" : "forgotten"));

    SlotMeasure full;
    NeedsCheckDue(full, 1000, due);
    Add(checks, "nothing due: the needs stamp holds neither full bags nor the mail and bank check",
        ErrandDue(full, ERRAND_PRESSURE_PERCENT, 1000 + CHECK_MS) && ErrandRetryOver(full, 1000 + CHECK_MS), "");

    computations = 0;
    SlotMeasure wrap;
    uint32_t const late = std::numeric_limits<uint32_t>::max() - CHECK_MS;
    bool const wrapFired = quiet(wrap, late);
    uint32_t const wrapWindow = computations;
    NeedsCheckDue(wrap, late + ERRAND_RETRY_MS, due);
    Add(checks, "nothing due: the retry time holds across the clock wrap",
        !wrapFired && wrapWindow == 1 && computations == 2,
        std::to_string(wrapWindow) + " computations in the retry time, " + std::to_string(computations) + " after it");
}

inline bool SameNeeds(UpkeepNeeds::NeedList const& a, UpkeepNeeds::NeedList const& b)
{
    if (a.needs.size() != b.needs.size() || a.money != b.money || a.reserve != b.reserve ||
        a.shortfall != b.shortfall || a.ranks.size() != b.ranks.size())
        return false;
    for (std::size_t i = 0; i < a.needs.size(); ++i)
    {
        UpkeepNeeds::Need const& x = a.needs[i];
        UpkeepNeeds::Need const& y = b.needs[i];
        if (x.kind != y.kind || x.stock != y.stock || x.copper != y.copper || x.funded != y.funded || x.due != y.due ||
            x.units != y.units || x.item != y.item || x.target != y.target || x.swap != y.swap)
            return false;
    }
    return true;
}

// The trip's needs list (WU27, VisitNeedsFor as ErrandNeeds calls it): the start and the steps (vendor, mount,
// trainer, weapon master, stable) read one list; a step that changed the state (gold paid, a free pet swap, a rate
// sample) makes the next read compute once. Invariant: every read equals a fresh ComputeNeeds on the same state
inline void VisitOnce(std::vector<Check>& checks)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index = std::make_shared<Planner::UpkeepIndex>();
    Planner::Snapshot state;
    state.level = 30;
    state.money = 50000;
    state.repairCost = 2000;
    state.durabilityPercent = 40;
    state.lowestDurabilityPercent = 15;
    state.hasPet = true;
    uint32_t spells = 100;
    uint32_t pet = 7;
    SlotMeasure measure;
    auto const rates = [&measure]
    {
        UpkeepNeeds::NeedRates out;
        UpkeepNeeds::FillTravel(out, measure);
        UpkeepNeeds::FillUse(out, measure);
        return out;
    };
    auto const keyOf = [&]
    {
        UpkeepNeeds::VisitKey key;
        key.money = state.money;
        key.level = state.level;
        key.spells = spells;
        key.pet = pet;
        UpkeepNeeds::KeyRates(key, measure);
        key.index = index.get();
        return key;
    };
    uint32_t computations = 0;
    auto const fill = [&](UpkeepNeeds::VisitNeeds& fresh)
    {
        ++computations;
        fresh.index = index;
        fresh.snapshot = state;
        fresh.needs = UpkeepNeeds::ComputeNeeds(fresh.snapshot, *index, rates());
    };
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> cache;
    uint32_t mismatches = 0;
    uint32_t reads = 0;
    auto const read = [&]
    {
        std::shared_ptr<UpkeepNeeds::VisitNeeds const> const list = UpkeepNeeds::VisitNeedsFor(cache, keyOf(), fill);
        ++reads;
        mismatches += SameNeeds(list->needs, UpkeepNeeds::ComputeNeeds(state, *index, rates())) ? 0 : 1;
        return list;
    };

    std::size_t const listed = read()->needs.needs.size();
    for (uint32_t step = 0; step < 5; ++step)
        read();
    bool const unchanged = computations == 1 && !mismatches && listed;
    Add(checks, "visit: one computation per visit unless state changed (start, vendor, mount, trainer, weapon, stable)",
        unchanged, std::to_string(computations) + " computations, " + std::to_string(listed) + " needs");

    cache.reset();
    computations = 0;
    read();  // the start
    read();  // the vendor: it pays the repair
    state.money -= 1500;
    state.repairCost = 0;
    for (uint32_t step = 0; step < 4; ++step)  // the mount, trainer, weapon master and stable read after it
        read();
    bool const paid = computations == 2;
    state.money -= 300;  // the trainer teaches
    read();
    read();
    bool const taught = computations == 3;
    pet = 0;  // the stable stores the pet in a free slot: no gold
    state.hasPet = false;
    read();
    spells += 1;  // a mount learned from the bags: no gold
    read();
    measure.UsePerHour[0] = 3.0f;  // the vendor's use sample
    read();
    read();
    Add(checks, "visit: a state change makes the next read compute once, not each later step (gold, pet, spell, rate)",
        paid && taught && computations == 6 && !mismatches,
        std::to_string(computations) + " computations, " + std::to_string(mismatches) + " mismatches");
    Add(checks, "visit: every cached list equals a fresh ComputeNeeds on the same state", !mismatches,
        std::to_string(reads) + " reads, " + std::to_string(mismatches) + " mismatches");

    // Wear (and stock) change on a walk without the key: the kept list would be stale, so the trip forgets it there
    state.lowestDurabilityPercent = 5;
    state.repairCost = 4000;
    UpkeepNeeds::VisitNeeds const* stale = UpkeepNeeds::VisitNeedsKept(cache, keyOf());
    bool const keyBlind = stale && !SameNeeds(stale->needs, UpkeepNeeds::ComputeNeeds(state, *index, rates()));
    cache.reset();  // NewRpgErrandsAction's walk to a stop
    uint32_t const beforeWalk = mismatches;
    read();
    Add(checks, "visit: a walk forgets the list, so wear the key does not hold is read after it",
        keyBlind && mismatches == beforeWalk && computations == 7,
        std::string(keyBlind ? "kept list stale before the walk" : "kept list not stale") + ", " +
            std::to_string(computations) + " computations");
}

// Nothing near offers the service: refused as no_stop, nothing else changes
inline void NoStop(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.RepairDue = true;
    bot.Stops = {{PLAIN_VENDOR, At(100.0f, 0.0f), SERVICE_VENDOR}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const refused = plan.Stops.empty() && plan.Refused.size() == 1 &&
                         plan.Refused[0].Offer.Kind == ErrandKind::Repair &&
                         std::string(plan.Refused[0].Reason) == "no_stop";
    Add(checks, "stop: a repair with no repairing vendor near is refused as no_stop", refused, Describe(plan));
}

inline void ProvideFakeBank(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (ServiceStop const& stop : snapshot.Stops)
        if (stop.Services & SERVICE_BANK)
        {
            ErrandOffer offer;
            offer.Kind = BANK_KIND;
            offer.Service = SERVICE_BANK;
            offer.WorthCopper = BANK_WORTH;
            offers.push_back(offer);
            return;
        }
}

// A later plan's provider row (here a bank) is planned by the same PlanTrip, next to the travel plan's errands
inline void LaterProvider(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(0, 12);
    bot.Stops = {{REPAIR_HUB, At(300.0f, 0.0f), SERVICE_VENDOR | SERVICE_REPAIR},
                 {BANKER, At(320.0f, 30.0f), SERVICE_BANK}};
    std::vector<ProviderRow> rows = TravelProviders();
    rows.push_back({BANK_KIND, "bank", &ProvideFakeBank});
    TripPlan const plan = Plan(bot, Request(bot), rows);
    bool banked = false;
    bool sold = false;
    for (TripStop const& stop : plan.Stops)
    {
        banked = banked || (stop.Id == BANKER && HasErrand(stop, BANK_KIND));
        sold = sold || HasErrand(stop, ErrandKind::Sell);
    }
    Add(checks, "provider: a later plan's bank row is planned without changing PlanTrip",
        banked && sold && plan.Refused.empty() && std::string(ErrandKindName(BANK_KIND)) == "other", Describe(plan));
}

// ---- Mail and the bank as providers (WB8) ----

constexpr uint32_t MAILBOX_ID = 601;
constexpr uint32_t BANKER_ID = 602;
constexpr uint32_t HUB_VENDOR = 103;

inline ErrandSnapshot GovernedBot(uint32_t freeSlots, uint32_t sellableSlots)
{
    ErrandSnapshot bot = Bot(freeSlots, sellableSlots);
    bot.Governed = true;
    return bot;
}

inline bool PlanHasKind(TripPlan const& plan, ErrandKind kind)
{
    for (TripStop const& stop : plan.Stops)
        if (HasErrand(stop, kind))
            return true;
    return false;
}

inline std::size_t DetourStops(TripPlan const& plan)
{
    std::size_t count = 0;
    for (TripStop const& stop : plan.Stops)
        count += stop.OnRoute ? 0 : 1;
    return count;
}

// The snapshot's stops as WB8b builds them: vendors, then mailbox and banker places merged on by position
inline void WithServices(ErrandSnapshot& bot, std::vector<ServiceStop> const& vendors,
                         std::vector<ServiceStop> const& extras)
{
    bot.Stops = vendors;
    MergeServiceStops(bot.Stops, extras);
}

// Unopened mail joins a stop that has a mailbox and earns no detour; seen or expected worth earns one
inline void MailScenarios(std::vector<Check>& checks)
{
    std::vector<ServiceStop> const vendor = {{HUB_VENDOR, At(300.0f, 0.0f), SERVICE_VENDOR}};

    // A mailbox 30 yd from the vendor is merged onto its stop: the sale and the mail share it
    ErrandSnapshot near = GovernedBot(0, 12);
    near.MailWaiting = 5;
    WithServices(near, vendor, {{MAILBOX_ID, At(330.0f, 0.0f), SERVICE_MAILBOX}});
    TripPlan const nearPlan = Plan(near, Request(near), TripProviders());
    bool const joined = nearPlan.Stops.size() == 1 && near.Stops.size() == 1 && nearPlan.Refused.empty() &&
                        HasErrand(nearPlan.Stops[0], ErrandKind::Sell) &&
                        HasErrand(nearPlan.Stops[0], ErrandKind::Mail);
    Add(checks, "mail: unopened mail joins the vendor stop with a mailbox 30 yd away (merged by position)", joined,
        Describe(nearPlan));

    // 80 yd away it is a stop of its own: unopened mail alone does not open it
    ErrandSnapshot far = near;
    WithServices(far, vendor, {{MAILBOX_ID, At(380.0f, 0.0f), SERVICE_MAILBOX}});
    TripPlan const farPlan = Plan(far, Request(far), TripProviders());
    bool refusedMail = farPlan.Refused.size() == 1 && farPlan.Refused[0].Offer.Kind == ErrandKind::Mail &&
                       std::string(farPlan.Refused[0].Reason) == "detour_not_worth";
    Add(checks, "mail: a mailbox 80 yd from the vendor is its own stop, and unopened mail does not open it",
        far.Stops.size() == 2 && refusedMail && DetourStops(farPlan) == 1 && !PlanHasKind(farPlan, ErrandKind::Mail),
        Describe(farPlan));

    // The unopened worth alone never reaches a trip's cost, however many mails wait
    ErrandSnapshot flood = GovernedBot(10, 0);
    flood.MailWaiting = 50;
    WithServices(flood, {}, {{MAILBOX_ID, At(400.0f, 0.0f), SERVICE_MAILBOX}});
    TripPlan const floodPlan = Plan(flood, Request(flood), TripProviders());
    Add(checks, "mail: 50 unopened mails (the whole inbox) alone still earn no detour",
        floodPlan.Stops.empty() && floodPlan.Refused.size() == 1, Describe(floodPlan));

    // What the bot saw and left earns the detour when it beats the cost: 1500 c against about 1100 c at band 1
    ErrandSnapshot seen = GovernedBot(10, 0);
    seen.MailWaiting = 1;
    seen.MailSeenCopper = 1500;
    WithServices(seen, {}, {{MAILBOX_ID, At(400.0f, 0.0f), SERVICE_MAILBOX}});
    TripPlan const seenPlan = Plan(seen, Request(seen), TripProviders());
    Add(checks, "mail: 1500 c of seen mail beats the detour and opens the mailbox stop",
        seenPlan.Stops.size() == 1 && !seenPlan.Stops[0].OnRoute && HasErrand(seenPlan.Stops[0], ErrandKind::Mail) &&
            seenPlan.Refused.empty(),
        Describe(seenPlan));

    ErrandSnapshot cheap = seen;
    cheap.MailSeenCopper = 200;
    TripPlan const cheapPlan = Plan(cheap, Request(cheap), TripProviders());
    Add(checks, "mail: 200 c of seen mail is refused as detour_not_worth",
        cheapPlan.Stops.empty() && cheapPlan.Refused.size() == 1 &&
            std::string(cheapPlan.Refused[0].Reason) == "detour_not_worth",
        Describe(cheapPlan));

    ErrandSnapshot expected = GovernedBot(10, 0);
    expected.MailExpectedCopper = 40000;
    WithServices(expected, {}, {{MAILBOX_ID, At(400.0f, 0.0f), SERVICE_MAILBOX}});
    TripPlan const expectedPlan = Plan(expected, Request(expected), TripProviders());
    Add(checks, "mail: 4 gold the bot expects from its sales opens the mailbox stop",
        expectedPlan.Stops.size() == 1 && HasErrand(expectedPlan.Stops[0], ErrandKind::Mail), Describe(expectedPlan));
}

// Orderliness: one weight on the mail and the withdrawal. A tidy bot goes where a messy one does not
inline void OrderlinessScenarios(std::vector<Check>& checks)
{
    float const tidy = BankMail::OrderlinessWeight(1.0f);
    float const messy = BankMail::OrderlinessWeight(-1.0f);
    float const nan = BankMail::OrderlinessWeight(std::nanf(""));
    bool const mapping = tidy == 1.5f && messy == 0.5f && BankMail::OrderlinessWeight(0.0f) == 1.0f && nan == 1.0f &&
                         BankMail::OrderlinessWeight(7.0f) == 1.5f && BankMail::OrderlinessWeight(-7.0f) == 0.5f;
    Add(checks, "orderliness: the trait 1 + 0.5 x unit gives 0.5 (messy) .. 1.5 (tidy), neutral and NaN 1.0", mapping,
        "tidy " + std::to_string(tidy) + " messy " + std::to_string(messy));

    auto const goes = [](float unit, uint32_t seen)
    {
        ErrandSnapshot bot = GovernedBot(10, 0);
        bot.Orderliness = unit;
        bot.MailSeenCopper = seen;
        WithServices(bot, {}, {{MAILBOX_ID, At(400.0f, 0.0f), SERVICE_MAILBOX}});
        return PlanHasKind(Plan(bot, Request(bot), TripProviders()), ErrandKind::Mail);
    };
    Add(checks, "orderliness: 1000 c seen: the tidy bot fetches it, the messy one does not",
        goes(1.0f, 1000) && !goes(-1.0f, 1000), "tidy yes, messy no");
    Add(checks, "orderliness: a messy bot still fetches mail when it is worth enough (weight is never 0)",
        goes(-1.0f, 4000) && BankMail::OrderlinessWeight(-1.0f) > 0.0f, "messy yes at 4000 c");

    ErrandSnapshot bot = GovernedBot(10, 0);
    bot.BankDueCount = 1;
    bot.BankDueWorth = 1500;
    WithServices(bot, {}, {{BANKER_ID, At(400.0f, 0.0f), SERVICE_BANKER}});
    ErrandSnapshot messyBot = bot;
    messyBot.Orderliness = -1.0f;
    ErrandSnapshot tidyBot = bot;
    tidyBot.Orderliness = 1.0f;
    Add(checks, "orderliness: a 1500 c withdrawal is fetched by the tidy bot, not by the messy one",
        PlanHasKind(Plan(tidyBot, Request(tidyBot), TripProviders()), ErrandKind::BankWithdraw) &&
            !PlanHasKind(Plan(messyBot, Request(messyBot), TripProviders()), ErrandKind::BankWithdraw),
        "weights 1.5 / 0.5");
}

// A due upgrade earns a trip to a banker when it beats the trip; a deposit joins a city stop and earns none
inline void BankScenarios(std::vector<Check>& checks)
{
    ErrandSnapshot due = GovernedBot(10, 0);
    due.BankDueCount = 1;
    due.BankDueWorth = 5000;
    WithServices(due, {}, {{BANKER_ID, At(300.0f, 0.0f), SERVICE_BANKER}});
    TripPlan const duePlan = Plan(due, Request(due), TripProviders());
    Add(checks, "bank: a due upgrade worth 5000 c earns a trip to the banker",
        duePlan.Stops.size() == 1 && !duePlan.Stops[0].OnRoute && HasErrand(duePlan.Stops[0], ErrandKind::BankWithdraw),
        Describe(duePlan));

    ErrandSnapshot small = due;
    small.BankDueWorth = 400;
    TripPlan const smallPlan = Plan(small, Request(small), TripProviders());
    Add(checks, "bank: a due item worth 400 c is refused as detour_not_worth",
        smallPlan.Stops.empty() && smallPlan.Refused.size() == 1 &&
            smallPlan.Refused[0].Offer.Kind == ErrandKind::BankWithdraw &&
            std::string(smallPlan.Refused[0].Reason) == "detour_not_worth",
        Describe(smallPlan));

    ErrandSnapshot deposit = GovernedBot(10, 0);
    deposit.BankDepositSlots = 3;
    WithServices(deposit, {}, {{BANKER_ID, At(400.0f, 0.0f), SERVICE_BANKER}});
    TripPlan const depositAlone = Plan(deposit, Request(deposit), TripProviders());
    Add(checks, "bank: a deposit alone earns no trip to a banker",
        depositAlone.Stops.empty() && depositAlone.Refused.size() == 1 &&
            depositAlone.Refused[0].Offer.Kind == ErrandKind::BankDeposit,
        Describe(depositAlone));

    ErrandSnapshot inTown = deposit;
    WithServices(inTown, {{HUB_VENDOR, At(380.0f, 0.0f), SERVICE_VENDOR}},
                 {{BANKER_ID, At(400.0f, 0.0f), SERVICE_BANKER}});
    // the route's stop is the merged one: vendor and banker on one stop
    TripStop city;
    city.Place = inTown.Stops[0].Place;
    city.Services = inTown.Stops[0].Services;
    city.Id = HUB_VENDOR;
    TripPlan const townPlan = Plan(inTown, Request(inTown, {city}), TripProviders());
    Add(checks, "bank: the same deposit joins the city stop the bot makes anyway (banker 20 yd from the vendor)",
        townPlan.Stops.size() == 1 && townPlan.Stops[0].OnRoute &&
            HasErrand(townPlan.Stops[0], ErrandKind::BankDeposit) && townPlan.Refused.empty(),
        Describe(townPlan));

    ErrandSnapshot ungoverned = deposit;
    ungoverned.Governed = false;
    ungoverned.BankDueCount = 1;
    ungoverned.BankDueWorth = 99999;
    ungoverned.MailWaiting = 9;
    ungoverned.MailSeenCopper = 99999;
    std::vector<ErrandOffer> const offers = CollectOffers(ungoverned, TripProviders());
    bool none = true;
    for (ErrandOffer const& offer : offers)
        none = none && offer.Kind != ErrandKind::Mail && offer.Kind != ErrandKind::BankWithdraw &&
               offer.Kind != ErrandKind::BankDeposit;
    Add(checks, "bank: a bot the bank rule does not govern gets no bank or mail errand", none,
        std::to_string(offers.size()) + " offers");
}

inline int RankOf(ErrandKind kind)
{
    switch (kind)
    {
        case ErrandKind::BankWithdraw:
            return 0;
        case ErrandKind::Sell:
            return 1;
        case ErrandKind::Auction:
            return 2;
        case ErrandKind::BankDeposit:
            return 3;
        case ErrandKind::Mail:
            return 4;
        default:
            return -1;
    }
}

// The order at a stop that has them all: banker withdrawal, sale, auction listing, deposit, mailbox
inline bool OrderHolds(TripPlan const& plan)
{
    for (TripStop const& stop : plan.Stops)
    {
        int last = -1;
        for (ErrandOffer const& errand : stop.Errands)
        {
            int const rank = RankOf(errand.Kind);
            if (rank < 0)
                continue;
            if (rank < last)
                return false;
            last = rank;
        }
    }
    return true;
}

inline void OrderScenario(std::vector<Check>& checks)
{
    ErrandSnapshot bot = GovernedBot(0, 12);
    bot.MailWaiting = 3;
    bot.BankDueCount = 1;
    bot.BankDueWorth = 5000;
    WithServices(bot, {{HUB_VENDOR, At(300.0f, 0.0f), SERVICE_VENDOR}},
                 {{MAILBOX_ID, At(320.0f, 10.0f), SERVICE_MAILBOX}, {BANKER_ID, At(280.0f, -10.0f), SERVICE_BANKER}});
    TripPlan const plan = Plan(bot, Request(bot), TripProviders());
    bool all = plan.Stops.size() == 1 && bot.Stops.size() == 1 && plan.Refused.empty();
    std::string order;
    if (all)
        for (ErrandOffer const& errand : plan.Stops[0].Errands)
            order += std::string(order.empty() ? "" : ", ") + ErrandKindName(errand.Kind);
    all = all && order.find("bank_withdraw") < order.find("sell") && order.find("sell") < order.find("mail");
    Add(checks, "order: at one stop the banker's withdrawal comes before the sale, the sale before the mailbox",
        all && OrderHolds(plan), order);
}

// ---- Auction stock as a provider (auction house WA3b) ----

constexpr uint32_t AUCTIONEER_ID = 603;

// Stock worth more listed than sold earns a trip to an auctioneer when the gain beats the detour; little stock far
// away is refused and the plan goes on; no stock or an ungoverned bot makes no auction offer
inline void AuctionScenarios(std::vector<Check>& checks)
{
    ErrandSnapshot stock = GovernedBot(10, 0);
    stock.AuctionStockWorth = 5000;
    WithServices(stock, {}, {{AUCTIONEER_ID, At(300.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const stockPlan = Plan(stock, Request(stock), TripProviders());
    Add(checks, "auction: stock 5000 c above the vendor's payout earns a trip to the auctioneer",
        stockPlan.Stops.size() == 1 && !stockPlan.Stops[0].OnRoute &&
            HasErrand(stockPlan.Stops[0], ErrandKind::Auction) && stockPlan.Refused.empty(),
        Describe(stockPlan));

    ErrandSnapshot little = stock;
    little.AuctionStockWorth = 400;
    TripPlan const littlePlan = Plan(little, Request(little), TripProviders());
    Add(checks, "auction: little stock (400 c) far from an auctioneer is refused as detour_not_worth",
        littlePlan.Stops.empty() && littlePlan.Refused.size() == 1 &&
            littlePlan.Refused[0].Offer.Kind == ErrandKind::Auction &&
            std::string(littlePlan.Refused[0].Reason) == "detour_not_worth",
        Describe(littlePlan));

    ErrandSnapshot none = stock;
    none.AuctionStockWorth = 0;
    ErrandSnapshot ungoverned = stock;
    ungoverned.Governed = false;
    bool offered = false;
    for (ErrandSnapshot const* bot : {&none, &ungoverned})
        for (ErrandOffer const& offer : CollectOffers(*bot, TripProviders()))
            offered = offered || offer.Kind == ErrandKind::Auction;
    Add(checks, "auction: no stock, or a bot the bank rule does not govern, makes no auction offer", !offered,
        offered ? "an auction offer" : "none");

    // Buying (WA4b): wanted lines alone earn the trip, and a stop's worth is what it sells and buys together
    ErrandSnapshot wanted = stock;
    wanted.AuctionStockWorth = 0;
    wanted.AuctionWantedWorth = 5000;
    TripPlan const wantedPlan = Plan(wanted, Request(wanted), TripProviders());
    Add(checks, "auction: wanted lines worth 5000 c and no stock earn a trip to the auctioneer",
        wantedPlan.Stops.size() == 1 && HasErrand(wantedPlan.Stops[0], ErrandKind::Auction) &&
            wantedPlan.Refused.empty(),
        Describe(wantedPlan));

    ErrandSnapshot both = stock;
    both.AuctionStockWorth = 400;
    both.AuctionWantedWorth = 4600;
    float sumWorth = 0.0f;
    for (ErrandOffer const& offer : CollectOffers(both, TripProviders()))
        if (offer.Kind == ErrandKind::Auction)
            sumWorth += offer.WorthCopper;
    TripPlan const bothPlan = Plan(both, Request(both), TripProviders());
    Add(checks, "auction: stock 400 c and wanted lines 4600 c make one offer worth 5000 c, a trip",
        sumWorth == 5000.0f && bothPlan.Stops.size() == 1 && HasErrand(bothPlan.Stops[0], ErrandKind::Auction),
        std::to_string(sumWorth) + "; " + Describe(bothPlan));

    ErrandSnapshot wantedUngoverned = wanted;
    wantedUngoverned.Governed = false;
    bool wantedOffered = false;
    for (ErrandOffer const& offer : CollectOffers(wantedUngoverned, TripProviders()))
        wantedOffered = wantedOffered || offer.Kind == ErrandKind::Auction;
    Add(checks, "auction: wanted lines of a bot the bank rule does not govern make no auction offer", !wantedOffered,
        wantedOffered ? "an auction offer" : "none");

    // A vendor stop with an auctioneer, a banker and a mailbox within the join distance: listing comes after the sale
    // and before the deposit (which would bank the stock) and the mailbox
    ErrandSnapshot town = GovernedBot(0, 12);
    town.AuctionStockWorth = 400;
    town.BankDepositSlots = 2;
    town.MailWaiting = 2;
    WithServices(town, {{HUB_VENDOR, At(300.0f, 0.0f), SERVICE_VENDOR}},
                 {{AUCTIONEER_ID, At(310.0f, 10.0f), SERVICE_AUCTIONEER},
                  {BANKER_ID, At(290.0f, -10.0f), SERVICE_BANKER},
                  {MAILBOX_ID, At(320.0f, 0.0f), SERVICE_MAILBOX}});
    TripPlan const townPlan = Plan(town, Request(town), TripProviders());
    std::string order;
    for (TripStop const& stop : townPlan.Stops)
        for (ErrandOffer const& errand : stop.Errands)
            order += std::string(order.empty() ? "" : ", ") + ErrandKindName(errand.Kind);
    bool const ordered = townPlan.Stops.size() == 1 && order.find("sell") < order.find("auction") &&
                         order.find("auction") < order.find("bank_deposit") &&
                         order.find("bank_deposit") < order.find("mail") && order.find("mail") != std::string::npos;
    Add(checks, "auction: little stock joins the town stop; sale, listing, deposit, mailbox in that order",
        ordered && OrderHolds(townPlan), order);

    // NR12b: banked sale stock is one errand of a banker and an auctioneer after it, 100 yd apart as in Stormwind and
    // Orgrimmar, weighed against the detour of both; never a banker after the auction, never one stop without the other
    auto const stockStops = [](TripPlan const& plan, std::size_t& bankAt, std::size_t& auctionAt)
    {
        bankAt = auctionAt = plan.Stops.size();
        for (std::size_t s = 0; s < plan.Stops.size(); ++s)
            for (ErrandOffer const& errand : plan.Stops[s].Errands)
            {
                if (errand.Kind == ErrandKind::BankWithdraw && errand.Id == STOCK_WITHDRAW_ID)
                    bankAt = s;
                if (errand.Kind == ErrandKind::Auction && auctionAt == plan.Stops.size())
                    auctionAt = s;
            }
    };
    ErrandSnapshot banked = GovernedBot(10, 0);
    banked.BankStockWorth = 5000;
    WithServices(
        banked, {},
        {{BANKER_ID, At(300.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(400.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const bankedPlan = Plan(banked, Request(banked), TripProviders());
    std::size_t bankAt = 0;
    std::size_t auctionAt = 0;
    stockStops(bankedPlan, bankAt, auctionAt);
    Add(checks, "NR12b: banked stock 5000 c alone draws the auctioneer, and the banker before it fetches the stock",
        bankedPlan.Stops.size() == 2 && bankAt == 0 && auctionAt == 1 && bankedPlan.Refused.empty() &&
            OrderHolds(bankedPlan),
        Describe(bankedPlan));

    ErrandSnapshot beyond = banked;
    WithServices(
        beyond, {},
        {{BANKER_ID, At(500.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(400.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const beyondPlan = Plan(beyond, Request(beyond), TripProviders());
    stockStops(beyondPlan, bankAt, auctionAt);
    Add(checks, "NR12b: a banker beyond the auctioneer is walked to first: no bank stop after the auction fetches it",
        beyondPlan.Stops.size() == 2 && bankAt == 0 && auctionAt == 1, Describe(beyondPlan));

    ErrandSnapshot together = banked;
    WithServices(
        together, {},
        {{BANKER_ID, At(300.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(320.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const togetherPlan = Plan(together, Request(together), TripProviders());
    stockStops(togetherPlan, bankAt, auctionAt);
    Add(checks, "NR12b: a banker beside the auctioneer: one stop, the withdrawal before the listing",
        togetherPlan.Stops.size() == 1 && bankAt == 0 && auctionAt == 0 && OrderHolds(togetherPlan),
        Describe(togetherPlan));

    ErrandSnapshot noAuctioneer = banked;
    WithServices(noAuctioneer, {}, {{BANKER_ID, At(300.0f, 0.0f), SERVICE_BANKER}});
    TripPlan const noAuctioneerPlan = Plan(noAuctioneer, Request(noAuctioneer), TripProviders());
    ErrandSnapshot littleBanked = banked;
    littleBanked.BankStockWorth = 400;
    TripPlan const littlePlanBanked = Plan(littleBanked, Request(littleBanked), TripProviders());
    bool fetchedAlone = PlanHasKind(noAuctioneerPlan, ErrandKind::BankWithdraw) ||
                        PlanHasKind(littlePlanBanked, ErrandKind::BankWithdraw);
    for (TripPlan const* plan : {&noAuctioneerPlan, &littlePlanBanked})
        for (Refusal const& refusal : plan->Refused)
            fetchedAlone = fetchedAlone || refusal.Offer.Kind == ErrandKind::BankWithdraw;
    Add(checks,
        "NR12b: no auctioneer, or little stock whose auction does not pay its detour: no withdrawal, and no row of "
        "one (the auction's refusal says why)",
        !fetchedAlone && noAuctioneerPlan.Stops.empty() && littlePlanBanked.Stops.empty(),
        Describe(noAuctioneerPlan) + "; " + Describe(littlePlanBanked));

    // The review's probes: an auctioneer with no banker in reach, or one beyond the walk limit, makes no trip; the
    // refusal is the auction's, worth the stock's gain
    auto const refusedAs = [](TripPlan const& plan, char const* reason)
    {
        return plan.Stops.empty() && plan.Refused.size() == 1 && plan.Refused[0].Offer.Kind == ErrandKind::Auction &&
               std::string(plan.Refused[0].Reason) == reason && plan.Refused[0].Offer.WorthCopper == 5000.0f;
    };
    ErrandSnapshot noBanker = banked;
    WithServices(noBanker, {}, {{AUCTIONEER_ID, At(400.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const noBankerPlan = Plan(noBanker, Request(noBanker), TripProviders());
    Add(checks, "NR12b: banked stock, an auctioneer and no banker: no trip, the auction refused no_stop",
        refusedAs(noBankerPlan, "no_stop"), Describe(noBankerPlan));

    ErrandSnapshot farBanker = banked;
    WithServices(
        farBanker, {},
        {{BANKER_ID, At(-3500.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(400.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const farBankerPlan = Plan(farBanker, Request(farBanker), TripProviders());
    Add(checks, "NR12b: the banker 3500 yd behind, beyond the walk limit: no trip, the auction refused too_far",
        refusedAs(farBankerPlan, "too_far"), Describe(farBankerPlan));

    // The banker's detour counts: 1200 c banked with the banker 2000 yd behind is refused as 1200 c in the bags is at
    // 2000 yd; with the banker beside the auctioneer (one stop) the same gain pays, and with it 100 yd away (a stop of
    // its own, a second detour) 2500 c do
    ErrandSnapshot behind = banked;
    behind.BankStockWorth = 1200;
    WithServices(
        behind, {},
        {{BANKER_ID, At(-2000.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(400.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const behindPlan = Plan(behind, Request(behind), TripProviders());
    ErrandSnapshot bagsFar = GovernedBot(10, 0);
    bagsFar.AuctionStockWorth = 1200;
    WithServices(bagsFar, {}, {{AUCTIONEER_ID, At(2000.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const bagsFarPlan = Plan(bagsFar, Request(bagsFar), TripProviders());
    ErrandSnapshot beside = behind;
    WithServices(
        beside, {},
        {{BANKER_ID, At(390.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(400.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const besidePlan = Plan(beside, Request(beside), TripProviders());
    ErrandSnapshot onWay = behind;
    WithServices(
        onWay, {},
        {{BANKER_ID, At(300.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(400.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const onWayLittle = Plan(onWay, Request(onWay), TripProviders());
    onWay.BankStockWorth = 2500;
    TripPlan const onWayPlan = Plan(onWay, Request(onWay), TripProviders());
    stockStops(onWayPlan, bankAt, auctionAt);
    Add(checks,
        "NR12b: 1200 c banked pays no 9.5 min bank detour (refused detour_not_worth, as 1200 c in the bags far away); "
        "it pays at a banker beside the auctioneer, 2500 c pay at one 100 yd before it, 1200 c do not",
        behindPlan.Stops.empty() && behindPlan.Refused.size() == 1 &&
            std::string(behindPlan.Refused[0].Reason) == "detour_not_worth" &&
            behindPlan.Refused[0].CostCopper > behindPlan.Refused[0].Offer.WorthCopper && bagsFarPlan.Stops.empty() &&
            besidePlan.Stops.size() == 1 && PlanHasKind(besidePlan, ErrandKind::BankWithdraw) &&
            onWayLittle.Stops.empty() && onWayPlan.Stops.size() == 2 && bankAt == 0 && auctionAt == 1,
        Describe(behindPlan) + "; " + Describe(bagsFarPlan) + "; " + Describe(besidePlan) + "; " +
            Describe(onWayLittle) + "; " + Describe(onWayPlan));

    // An auction stop the bags' stock earns: the banker in front of it is a detour of its own, worth the banked gain
    ErrandSnapshot bagsAndBank = behind;
    bagsAndBank.AuctionStockWorth = 5000;
    TripPlan const bagsOnlyPlan = Plan(bagsAndBank, Request(bagsAndBank), TripProviders());
    stockStops(bagsOnlyPlan, bankAt, auctionAt);
    bool const bagsOnly = bagsOnlyPlan.Stops.size() == 1 && auctionAt == 0 && bankAt == 1 &&
                          bagsOnlyPlan.Refused.size() == 1 &&
                          bagsOnlyPlan.Refused[0].Offer.Kind == ErrandKind::BankWithdraw;
    bagsAndBank.BankStockWorth = 20000;
    TripPlan const bagsBankPlan = Plan(bagsAndBank, Request(bagsAndBank), TripProviders());
    stockStops(bagsBankPlan, bankAt, auctionAt);
    Add(checks,
        "NR12b: with 5000 c in the bags the auction stop opens alone; the banker 2000 yd behind joins in front of it "
        "for 20000 c banked, not for 1200 c",
        bagsOnly && bagsBankPlan.Stops.size() == 2 && bankAt == 0 && auctionAt == 1 && bagsBankPlan.Refused.empty(),
        Describe(bagsOnlyPlan) + "; " + Describe(bagsBankPlan));

    // The banker counts against MAX_DETOUR_STOPS: two turn-ins worth more open two detours, and the pair needing two
    // more stays out; with one turn-in the pair fills the trip's three
    ErrandSnapshot capped = banked;
    capped.BankStockWorth = 20000;
    capped.Completed = {{HUB_QUEST, ENDER, false, At(0.0f, 600.0f), 50000.0f},
                        {FAR_QUEST, ENDER, false, At(0.0f, -600.0f), 50000.0f}};
    WithServices(
        capped, {},
        {{BANKER_ID, At(600.0f, 0.0f), SERVICE_BANKER}, {AUCTIONEER_ID, At(700.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const cappedPlan = Plan(capped, Request(capped), TripProviders());
    ErrandSnapshot oneQuest = capped;
    oneQuest.Completed.pop_back();
    TripPlan const oneQuestPlan = Plan(oneQuest, Request(oneQuest), TripProviders());
    stockStops(oneQuestPlan, bankAt, auctionAt);
    Add(checks, "NR12b: the banker is a detour of the cap: no fourth stop for the pair, three with one turn-in",
        DetourStops(cappedPlan) == 2 && !PlanHasKind(cappedPlan, ErrandKind::BankWithdraw) &&
            !PlanHasKind(cappedPlan, ErrandKind::Auction) && DetourStops(oneQuestPlan) == 3 && bankAt < auctionAt &&
            auctionAt < oneQuestPlan.Stops.size(),
        Describe(cappedPlan) + "; " + Describe(oneQuestPlan));

    // A bank beside an auction house beyond the walk limit is no pair's banker for a plain auctioneer short of it: the
    // auction would join the bank's stop and leave the auctioneer's stop empty (the review's fuzz case)
    ErrandSnapshot bankHouse = banked;
    bankHouse.BankStockWorth = 20000;
    WithServices(bankHouse, {},
                 {{AUCTIONEER_ID, At(3800.0f, 0.0f), SERVICE_AUCTIONEER},
                  {BANKER_ID, At(4300.0f, 0.0f), SERVICE_BANKER},
                  {AUCTIONEER_ID, At(4320.0f, 0.0f), SERVICE_AUCTIONEER}});
    TripPlan const bankHousePlan = Plan(bankHouse, Request(bankHouse), TripProviders());
    Add(checks, "NR12b: a bank and auction house beyond the walk limit pairs with no auctioneer short of it: no trip",
        bankHouse.Stops.size() == 2 && bankHousePlan.Stops.empty() && bankHousePlan.Refused.size() == 1 &&
            bankHousePlan.Refused[0].Offer.Kind == ErrandKind::Auction &&
            std::string(bankHousePlan.Refused[0].Reason) == "too_far",
        Describe(bankHousePlan));

    // The running trip's flags: the bank visit finds the stock due for an auction ahead, not for one that ran
    struct FlagStop
    {
        bool auction = false;
    };
    std::vector<FlagStop> const ahead = {{false}, {true}};
    std::vector<FlagStop> const here = {{true}};
    std::vector<FlagStop> const ran = {{false}, {false}};
    std::vector<FlagStop> const earlier = {{true}, {false}};
    Add(checks, "NR12b: due for an auction stop after the bank stop or at it, not when it precedes the bank stop",
        AuctionAhead(ahead, 0) && AuctionAhead(here, 0) && !AuctionAhead(ran, 1) && !AuctionAhead(earlier, 1) &&
            !AuctionAhead(ahead, 2),
        "");
}

// ---- The invariant search (WB8) ----

// A small deterministic generator (xorshift64*): the seed is printed, the failing case is replayable
struct Rng
{
    uint64_t State;
    explicit Rng(uint64_t seed) : State(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t Next()
    {
        State ^= State >> 12;
        State ^= State << 25;
        State ^= State >> 27;
        return State * 0x2545F4914F6CDD1Dull;
    }
    uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
    float Range(float lo, float hi) { return lo + static_cast<float>(Next() % 100001) / 100000.0f * (hi - lo); }
    bool Chance(uint32_t percent) { return Below(100) < percent; }
};

inline SlotRates RandomRates(Rng& rng)
{
    SlotRates rates;
    if (rng.Chance(50))
        return CurrentRates(SlotMeasure{}, DEFAULT_SLOT_BANDS[rng.Below(LEVEL_BAND_COUNT)]);

    rates.LootSlotsPerHour = rng.Range(0.0f, 60.0f);
    rates.TripMinutes = rng.Chance(10) ? 0.0f : rng.Range(0.1f, 20.0f);
    rates.LostCopperPerHour = rng.Range(0.0f, 2000.0f);
    rates.EarnCopperPerMinute = rng.Chance(10) ? 0.0f : rng.Range(1.0f, 3000.0f);
    return rates;
}

inline float RandomUnit(Rng& rng)
{
    switch (rng.Below(8))
    {
        case 0:
            return std::nanf("");
        case 1:
            return rng.Range(-9.0f, 9.0f);  // outside -1..1: the mapping clamps
        default:
            return rng.Range(-1.0f, 1.0f);
    }
}

inline ErrandPlace RandomPlace(Rng& rng) { return At(rng.Range(-900.0f, 900.0f), rng.Range(-900.0f, 900.0f)); }

inline ErrandSnapshot RandomSnapshot(Rng& rng, bool withMail, bool withBank)
{
    ErrandSnapshot bot = Bot(rng.Below(30), rng.Below(30));
    bot.Level = 1 + rng.Below(80);
    bot.Rates = RandomRates(rng);
    bot.MinutesToNextStop = rng.Range(0.0f, 60.0f);
    bot.Governed = rng.Chance(80);
    bot.Orderliness = RandomUnit(rng);
    bot.MailDefaultWorth = rng.Chance(50) ? 100 : rng.Below(1000);
    if (withMail)
    {
        bot.MailWaiting = rng.Chance(20) ? 65535 : rng.Below(60);
        if (rng.Chance(30))
            bot.MailSeenCopper = rng.Chance(10) ? 0xFFFFFFFFu : rng.Below(20000);
        if (rng.Chance(30))
            bot.MailExpectedCopper = rng.Chance(10) ? 0xFFFFFFFFu : rng.Below(50000);
    }
    if (withBank)
    {
        bot.BankDueCount = rng.Below(4);
        bot.BankDueWorth = bot.BankDueCount ? (rng.Chance(10) ? 0xFFFFFFFFu : rng.Below(20000)) : 0;
        bot.BankDepositSlots = rng.Below(6);
    }
    std::vector<ServiceStop> vendors;
    for (uint32_t i = rng.Below(3); i > 0; --i)
        vendors.push_back(
            {HUB_VENDOR + i, RandomPlace(rng), rng.Chance(30) ? SERVICE_VENDOR | SERVICE_REPAIR : SERVICE_VENDOR});
    std::vector<ServiceStop> extras;
    for (uint32_t i = rng.Below(3); i > 0; --i)
        extras.push_back({MAILBOX_ID + i, RandomPlace(rng), SERVICE_MAILBOX});
    for (uint32_t i = rng.Below(3); i > 0; --i)
        extras.push_back({BANKER_ID + i, RandomPlace(rng), SERVICE_BANKER});
    if (rng.Chance(30))
    {
        bot.Profession = {PROFESSION_SPELL, rng.Chance(50)};
        extras.push_back({PROFESSION_TRAINER, RandomPlace(rng), SERVICE_PROFESSION_TRAINER});
    }
    if (rng.Chance(30))
    {
        bool const station = rng.Chance(70);
        float const worth = rng.Chance(10) ? std::nanf("") : rng.Chance(10) ? -5.0f : rng.Range(0.0f, 50000.0f);
        bot.Craft = {CRAFT_SPELL, station ? CRAFT_FOCUS : 0, worth};
        if (station)
            extras.push_back({STATION, RandomPlace(rng), SERVICE_STATION});
    }
    WithServices(bot, vendors, extras);
    return bot;
}

inline std::vector<TripStop> RandomRoute(Rng& rng, ErrandSnapshot const& bot)
{
    std::vector<TripStop> route;
    if (!bot.Stops.empty() && rng.Chance(40))
    {
        ServiceStop const& pick = bot.Stops[rng.Below(static_cast<uint32_t>(bot.Stops.size()))];
        route.push_back({pick.Place, pick.Services, pick.Id, 0, false, 0.0f, {}});
    }
    return route;
}

inline std::string Replay(uint64_t seed, uint32_t iteration, TripPlan const& plan)
{
    return "seed " + std::to_string(seed) + " iteration " + std::to_string(iteration) + ": " + Describe(plan);
}

// Invariants over seeded random snapshots (band and free rates, any orderliness, huge counts):
//  1. unopened mail alone never earns a detour (WB.4)
//  2. no bank or mail errand for a bot the bank rule does not govern
//  3. at a shared stop: banker withdrawal before the sale, the sale before the mailbox
//  4. every provider's worth is finite and not negative
//  5. a craft is done only at a stop with its station, a craft without one only at a stop the trip makes anyway
inline void InvariantSearch(std::vector<Check>& checks, uint64_t seed, uint32_t iterations)
{
    Rng rng(seed);
    std::string unopened;
    std::string ungoverned;
    std::string order;
    std::string worth;
    std::string craft;
    uint32_t crafts = 0;
    uint32_t mailJoined = 0;
    uint32_t detoured = 0;
    uint32_t withBankMail = 0;
    for (uint32_t i = 0; i < iterations; ++i)
    {
        // 1: mail only
        ErrandSnapshot mail = RandomSnapshot(rng, true, false);
        mail.Governed = true;
        mail.MailSeenCopper = 0;
        mail.MailExpectedCopper = 0;
        mail.SellableSlots = 0;
        mail.Completed.clear();
        mail.Profession = {};  // the mail is the only errand of this case
        mail.Craft = {};
        TripPlan const mailPlan = Plan(mail, Request(mail, RandomRoute(rng, mail)), TripProviders());
        mailJoined += PlanHasKind(mailPlan, ErrandKind::Mail) ? 1 : 0;
        for (TripStop const& stop : mailPlan.Stops)
            if (!stop.OnRoute && unopened.empty())
                unopened = Replay(seed, i, mailPlan);

        // 2, 3, 4: everything
        ErrandSnapshot all = RandomSnapshot(rng, true, true);
        all.Completed = {{FAR_QUEST, ENDER, false, RandomPlace(rng), rng.Range(0.0f, 5000.0f)}};
        std::vector<ErrandOffer> const offers = CollectOffers(all, TripProviders());
        TripPlan const plan = Plan(all, Request(all, RandomRoute(rng, all)), TripProviders());
        detoured += DetourStops(plan) ? 1 : 0;
        for (ErrandOffer const& offer : offers)
        {
            bool const bankMail = offer.Kind == ErrandKind::Mail || offer.Kind == ErrandKind::BankWithdraw ||
                                  offer.Kind == ErrandKind::BankDeposit;
            withBankMail += bankMail ? 1 : 0;
            if (bankMail && !all.Governed && ungoverned.empty())
                ungoverned = Replay(seed, i, plan);
            if ((!std::isfinite(offer.WorthCopper) || offer.WorthCopper < 0.0f) && worth.empty())
                worth = Replay(seed, i, plan) + " worth " + std::to_string(offer.WorthCopper);
        }
        if (!OrderHolds(plan) && order.empty())
            order = Replay(seed, i, plan);
        for (TripStop const& stop : plan.Stops)
            for (ErrandOffer const& errand : stop.Errands)
            {
                if (errand.Kind != ErrandKind::Craft)
                    continue;
                ++crafts;
                bool const atStation = errand.Service == SERVICE_STATION && (stop.Services & SERVICE_STATION);
                bool const joined = !errand.Service && (stop.OnRoute || stop.Errands.size() > 1);
                if (!atStation && !joined && craft.empty())
                    craft = Replay(seed, i, plan);
            }
    }
    std::string const stats = std::to_string(iterations) + " snapshots, seed " + std::to_string(seed);
    Add(checks, "search: unopened mail alone never earns a detour", unopened.empty() && mailJoined > iterations / 20,
        stats + ", mail joined a stop in " + std::to_string(mailJoined) + "; " + unopened);
    Add(checks, "search: no bank or mail errand for an ungoverned bot", ungoverned.empty() && withBankMail > 0,
        stats + ", " + std::to_string(withBankMail) + " bank/mail offers; " + ungoverned);
    Add(checks, "search: bank withdrawal before sale before mailbox at every stop", order.empty() && detoured > 0,
        stats + ", " + std::to_string(detoured) + " plans with detours; " + order);
    Add(checks, "search: every provider's worth is finite and not negative", worth.empty(), stats + "; " + worth);
    Add(checks, "search: a craft only at its station, one without a station only at a stop made anyway",
        craft.empty() && crafts > 0, stats + ", " + std::to_string(crafts) + " crafts on stops; " + craft);
}

// The repair need's due rule is WU5's (UpkeepNeeds::RepairErrandDue): worn below 30 % and the gold pays an item
inline void RepairByNeed(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Stops = {{REPAIR_HUB, At(300.0f, 0.0f), SERVICE_VENDOR | SERVICE_REPAIR}};
    bot.RepairDue = UpkeepNeeds::RepairErrandDue(0, 25, CHEAPEST_REPAIR, bot.Money);
    TripPlan const plan = Plan(bot, Request(bot));
    bool const taken = plan.Stops.size() == 1 && plan.Stops[0].Id == REPAIR_HUB &&
                       HasErrand(plan.Stops[0], ErrandKind::Repair) && plan.Refused.empty();
    Add(checks, "repair: gear at 25 % is due and gets its own stop at a repairer", taken, Describe(plan));

    ErrandSnapshot worn = bot;
    worn.RepairDue = UpkeepNeeds::RepairErrandDue(0, 30, CHEAPEST_REPAIR, bot.Money);
    ErrandSnapshot poor = bot;
    poor.Money = CHEAPEST_REPAIR - 1;
    poor.RepairDue = UpkeepNeeds::RepairErrandDue(1, 0, CHEAPEST_REPAIR, poor.Money);
    TripPlan const wornPlan = Plan(worn, Request(worn));
    TripPlan const poorPlan = Plan(poor, Request(poor));
    bool const none =
        wornPlan.Stops.empty() && wornPlan.Refused.empty() && poorPlan.Stops.empty() && poorPlan.Refused.empty();
    Add(checks, "repair: no offer at 30 % or when the gold pays no item", none,
        Describe(wornPlan) + "; " + Describe(poorPlan));
}

// The shopping need: one stop at the vendor covering the most due kinds; a kind not due never opens a detour
inline void ShopByCover(std::vector<Check>& checks)
{
    Planner::StockMask const food = Planner::StockBit(Planner::UpkeepStock::Food);
    Planner::StockMask const water = Planner::StockBit(Planner::UpkeepStock::Water);
    ErrandSnapshot bot = Bot(10, 0);
    bot.DueStock = food | water;
    bot.Stops = {{PLAIN_VENDOR, At(200.0f, 0.0f), VendorServices(bot, PLAIN_VENDOR, false, food)},
                 {ROAD_VENDOR, At(900.0f, 0.0f), VendorServices(bot, ROAD_VENDOR, false, food | water)}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const covering = plan.Stops.size() == 1 && plan.Stops[0].Id == ROAD_VENDOR &&
                          plan.Stops[0].Errands.size() == 2 && HasErrand(plan.Stops[0], ErrandKind::Shop) &&
                          plan.Refused.empty();
    Add(checks, "shop: of two vendors the one covering more due kinds is the stop", covering, Describe(plan));

    ErrandSnapshot low = bot;
    low.DueStock = 0;
    low.OtherStock = food;
    TripPlan const lowPlan = Plan(low, Request(low));
    bool const refused = lowPlan.Stops.empty() && lowPlan.Refused.size() == 1 &&
                         lowPlan.Refused[0].Offer.Kind == ErrandKind::Shop &&
                         std::string(lowPlan.Refused[0].Reason) == "detour_not_worth";
    Add(checks, "shop: a stock below target but not due opens no detour", refused, Describe(lowPlan));
}

// Training not yet due joins a trip that passes the class trainer anyway, at no detour
inline void TrainingJoins(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Training = {1, 1, false, CLASS_SPELL};
    TripStop trainer;
    trainer.Place = At(800.0f, 0.0f);
    trainer.Services = SERVICE_TRAINER;
    trainer.Id = TRAINER;
    TripPlan const plan = Plan(bot, Request(bot, {trainer}));
    bool const joined = plan.Stops.size() == 1 && plan.Stops[0].OnRoute && plan.Stops[0].DetourMinutes == 0.0f &&
                        HasErrand(plan.Stops[0], ErrandKind::Train) && plan.Refused.empty();
    Add(checks, "train: training not due joins a trip that passes a trainer", joined, Describe(plan));
}

// A vendor stop with the class trainer within STOP_JOIN_YARDS takes training too (WU12 NOTE 3): the stop keeps its own
// entry and carries the trainer's. A vendor farther from the trainer does not, and a trainer's own stop is not retagged
inline void TrainingAtVendor(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Training = {1, 1, false, CLASS_SPELL};
    std::vector<ServiceStop> const trainers = {{TRAINER, At(800.0f, 0.0f), SERVICE_TRAINER}};
    std::vector<ServiceStop> stops = {{PLAIN_VENDOR, At(830.0f, 0.0f), SERVICE_VENDOR},
                                      {ROAD_VENDOR, At(900.0f, 0.0f), SERVICE_VENDOR},
                                      {REPAIR_HUB, At(840.0f, 0.0f), SERVICE_VENDOR | SERVICE_REPAIR},
                                      {TRAINER, At(800.0f, 0.0f), SERVICE_TRAINER}};
    JoinTrainers(stops, trainers, SERVICE_TRAINER);
    bool const tagged = (stops[0].Services & SERVICE_TRAINER) && stops[0].Trainer == TRAINER &&
                        stops[0].Id == PLAIN_VENDOR && (stops[0].Services & SERVICE_VENDOR);
    bool const edge = (stops[2].Services & SERVICE_TRAINER) && stops[2].Trainer == TRAINER;  // 40 yd exactly
    bool const apart = !(stops[1].Services & SERVICE_TRAINER) && stops[1].Trainer == 0;      // 100 yd
    bool const own = stops[3].Trainer == 0 && stops[3].Services == SERVICE_TRAINER;
    Add(checks, "train: a vendor within 40 yd of the trainer carries its service and entry", tagged && edge,
        "stop 0 trainer " + std::to_string(stops[0].Trainer) + ", stop 2 trainer " + std::to_string(stops[2].Trainer));
    Add(checks, "train: a vendor 100 yd from the trainer does not, the trainer's own stop is untouched", apart && own,
        "stop 1 trainer " + std::to_string(stops[1].Trainer));

    auto const asRoute = [](ServiceStop const& stop)
    {
        TripStop route;
        route.Place = stop.Place;
        route.Services = stop.Services;
        route.Id = stop.Id;
        route.Trainer = stop.Trainer;
        return route;
    };
    bot.Stops = stops;
    TripPlan const joined = Plan(bot, Request(bot, {asRoute(stops[0])}));
    bool const joins = joined.Stops.size() == 1 && joined.Stops[0].OnRoute && joined.Stops[0].Id == PLAIN_VENDOR &&
                       joined.Stops[0].Trainer == TRAINER && HasErrand(joined.Stops[0], ErrandKind::Train) &&
                       joined.Refused.empty();
    Add(checks, "train: training not due joins a route vendor stop that carries the trainer", joins, Describe(joined));

    TripPlan const apartPlan = Plan(bot, Request(bot, {asRoute(stops[1])}));
    bool const refused = apartPlan.Stops.size() == 1 && apartPlan.Stops[0].Trainer == 0 &&
                         !HasErrand(apartPlan.Stops[0], ErrandKind::Train) && apartPlan.Refused.size() == 1 &&
                         apartPlan.Refused[0].Offer.Kind == ErrandKind::Train;
    Add(checks, "train: a route vendor 100 yd from the trainer takes no training", refused, Describe(apartPlan));
}

// Due training (levels behind reach the span) is a trip of its own, to one trainer: the nearer of two
inline void TrainingTrip(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Training = {3, 2, true, CLASS_SPELL};
    bot.Stops = {{FAR_TRAINER, At(-2500.0f, 0.0f), SERVICE_TRAINER}, {TRAINER, At(1500.0f, 0.0f), SERVICE_TRAINER}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const own = plan.Stops.size() == 1 && !plan.Stops[0].OnRoute && plan.Stops[0].Id == TRAINER &&
                     HasErrand(plan.Stops[0], ErrandKind::Train) && plan.Refused.empty();
    Add(checks, "train: a trip of its own once enough levels wait, to the nearer trainer only", own, Describe(plan));
}

// Training not due with the trainer off the route: the detour is refused, the route stays
inline void TrainingRefused(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Training = {1, 1, false, CLASS_SPELL};
    bot.Stops = {{TRAINER, At(600.0f, 300.0f), SERVICE_TRAINER}};
    TripStop destination;
    destination.Place = At(1200.0f, 0.0f);
    destination.Services = SERVICE_QUEST_ENDER;
    TripPlan const plan = Plan(bot, Request(bot, {destination}));
    bool const refused = plan.Stops.size() == 1 && plan.Stops[0].OnRoute && plan.Refused.size() == 1 &&
                         plan.Refused[0].Offer.Kind == ErrandKind::Train &&
                         std::string(plan.Refused[0].Reason) == "detour_not_worth";
    Add(checks, "train: a detour for training not due is refused", refused, Describe(plan));

    ErrandSnapshot due = bot;
    due.Training.Due = true;
    TripPlan const duePlan = Plan(due, Request(due, {destination}));
    bool const taken =
        duePlan.Stops.size() == 2 && HasErrand(duePlan.Stops[0], ErrandKind::Train) && duePlan.Refused.empty();
    Add(checks, "train: the same trainer is a detour once training is due", taken, Describe(duePlan));
}

// A riding rank bought: one trip to the riding trainer and the mount vendor of the bot's race (PriceMount's choice
// of two vendors 200 yd apart, the other race's mount is cheaper)
inline void RidingTrip(std::vector<Check>& checks)
{
    UpkeepNeeds::RidingRank const& rank = UpkeepNeeds::RIDING_RANKS[0];
    Planner::VendorOffer human;
    human.vendorEntry = MOUNT_VENDOR;
    human.item = 2411;
    human.buyPrice = 80000;
    human.raceMask = UpkeepNeeds::RaceBit(RACE_HUMAN);
    human.requiredSkillRank = rank.skill;
    Planner::VendorOffer orc = human;
    orc.vendorEntry = OTHER_MOUNT_VENDOR;
    orc.item = 1132;
    orc.buyPrice = 70000;
    orc.raceMask = UpkeepNeeds::RaceBit(RACE_ORC);
    Planner::UpkeepIndex index;
    index.offers[static_cast<std::size_t>(Planner::UpkeepStock::Mount)] = {orc, human};
    Planner::Snapshot snapshot;
    snapshot.race = RACE_HUMAN;
    UpkeepNeeds::RankValue value;
    value.rank = rank;
    UpkeepNeeds::PriceMount(index, snapshot, value);

    ErrandSnapshot bot = Bot(10, 0);
    bot.Riding = {rank.spellId, value.mountVendor};
    bot.Stops = {{RIDING_TRAINER, At(1000.0f, 0.0f), SERVICE_RIDING_TRAINER},
                 {OTHER_MOUNT_VENDOR, At(1000.0f, -200.0f), VendorServices(bot, OTHER_MOUNT_VENDOR, false, 0)},
                 {MOUNT_VENDOR, At(1000.0f, 200.0f), VendorServices(bot, MOUNT_VENDOR, false, 0)}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool trainer = false;
    bool vendor = false;
    bool other = false;
    for (TripStop const& stop : plan.Stops)
    {
        trainer = trainer || (stop.Id == RIDING_TRAINER && HasErrand(stop, ErrandKind::Ride));
        vendor = vendor || (stop.Id == MOUNT_VENDOR && HasErrand(stop, ErrandKind::Ride));
        other = other || stop.Id == OTHER_MOUNT_VENDOR;
    }
    bool const both = value.mountVendor == MOUNT_VENDOR && plan.Stops.size() == 2 && trainer && vendor && !other &&
                      plan.Refused.empty();
    Add(checks, "ride: a riding trip visits the riding trainer and the mount vendor of the bot's race", both,
        "mount vendor " + std::to_string(value.mountVendor) + "; " + Describe(plan));

    // A bot standing at the mount vendor (WU26c): the vendor is the nearer stop, yet the rank is learnt first, as the
    // vendor sells the mount only to a bot that knows it: one trip buys both
    TripRequest atVendor = Request(bot);
    atVendor.Start = At(1000.0f, 195.0f);
    TripPlan const rankFirst = Plan(bot, atVendor);
    bool const ordered = rankFirst.Stops.size() == 2 && rankFirst.Stops[0].Id == RIDING_TRAINER &&
                         HasErrand(rankFirst.Stops[0], ErrandKind::Ride) && rankFirst.Stops[1].Id == MOUNT_VENDOR &&
                         HasErrand(rankFirst.Stops[1], ErrandKind::Ride) && rankFirst.Refused.empty();
    Add(checks, "ride: the riding trainer comes before the mount vendor, also from beside the vendor", ordered,
        Describe(rankFirst));

    // A known rank whose mount is missing (WU14): the mount vendor alone, the trainer is no stop
    ErrandSnapshot mountOnly = bot;
    mountOnly.Riding.Learn = false;
    TripPlan const mountPlan = Plan(mountOnly, Request(mountOnly));
    bool const vendorAlone = mountPlan.Stops.size() == 1 && mountPlan.Stops[0].Id == MOUNT_VENDOR &&
                             HasErrand(mountPlan.Stops[0], ErrandKind::Ride) && mountPlan.Refused.empty();
    Add(checks, "ride: a known rank without its mount goes to the mount vendor only", vendorAlone, Describe(mountPlan));
}

// A map with no vendor that trades with the bot (WU12 NOTE 4): the riding rank still plans its trainer stop, the mount
// waits (no stop sells it); without a trainer need the errand check is skipped. An unfunded rank is no need: no trip
inline void RidingWithoutVendor(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Riding = {UpkeepNeeds::RIDING_RANKS[0].spellId, MOUNT_VENDOR};
    bot.Stops = {{RIDING_TRAINER, At(1000.0f, 0.0f), SERVICE_RIDING_TRAINER}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const trainerOnly = ErrandCheckPlans(false, true) && !ErrandCheckPlans(false, false) &&
                             plan.Stops.size() == 1 && plan.Stops[0].Id == RIDING_TRAINER &&
                             HasErrand(plan.Stops[0], ErrandKind::Ride) && plan.Refused.size() == 1 &&
                             plan.Refused[0].Offer.Service == SERVICE_MOUNT_VENDOR;
    Add(checks, "ride: on a map without vendors the riding trainer is still a trip", trainerOnly, Describe(plan));

    // The errand check takes the riding need into the snapshot only when its funded gold pays it whole
    // (RidingErrandDue); a rank that shows BUY against the thrift-scaled reserve but is funded short plans no trip
    UpkeepNeeds::Need need;
    need.kind = UpkeepKind::Riding;
    need.due = true;
    need.copper = 80000;
    need.item = UpkeepNeeds::RIDING_RANKS[0].spellId;
    need.vendor = MOUNT_VENDOR;
    need.funded = need.copper - 1;
    ErrandSnapshot unfunded = bot;
    unfunded.Riding = {};
    if (UpkeepNeeds::RidingErrandDue(need))
        unfunded.Riding = {need.item, need.vendor};
    TripPlan const none = Plan(unfunded, Request(unfunded));
    need.funded = need.copper;
    ErrandSnapshot paid = bot;
    paid.Riding = {};
    if (UpkeepNeeds::RidingErrandDue(need))
        paid.Riding = {need.item, need.vendor};
    TripPlan const some = Plan(paid, Request(paid));
    Add(checks, "ride: a rank the funded gold does not pay plans no trip, one it pays does",
        none.Stops.empty() && none.Refused.empty() && !some.Stops.empty(), Describe(none) + " / " + Describe(some));
}

// A rank and its mount are one errand (WUL11): stops with two due errands each, a little farther off, do not crowd
// the riding trainer and the mount vendor out of the trip's detours; the mount vendor rides along with the rank
inline void RidingCrowded(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Riding = {UpkeepNeeds::RIDING_RANKS[0].spellId, MOUNT_VENDOR};
    bot.Stops = {{RIDING_TRAINER, At(20.0f, 0.0f), SERVICE_RIDING_TRAINER},
                 {MOUNT_VENDOR, At(20.0f, 150.0f), VendorServices(bot, MOUNT_VENDOR, false, 0)}};
    std::vector<ErrandOffer> const riding = CollectOffers(bot, TravelProviders());
    auto const crowded = [&bot, &riding](std::vector<ErrandPlace> const& places)
    {
        std::vector<ErrandOffer> offers = riding;
        uint32_t quest = HUB_QUEST;
        for (ErrandPlace const& place : places)
            for (int pair = 0; pair < 2; ++pair)
            {
                ErrandOffer offer;
                offer.Kind = ErrandKind::TurnIn;
                offer.Placed = true;
                offer.Place = place;
                offer.WorthCopper = DueWorth(bot.Rates);
                offer.Id = quest++;
                offer.Target = ENDER;
                offer.Due = true;
                offers.push_back(offer);
            }
        return PlanTrip(Request(bot), bot.Stops, offers);
    };
    auto const bothFirst = [](TripPlan const& plan)
    {
        std::size_t const trainer = RideStop(plan.Stops, SERVICE_RIDING_TRAINER);
        std::size_t const vendor = RideStop(plan.Stops, SERVICE_MOUNT_VENDOR);
        bool refused = false;
        for (Refusal const& refusal : plan.Refused)
            refused = refused || refusal.Offer.Kind == ErrandKind::Ride;
        return trainer < vendor && vendor < plan.Stops.size() && !refused;
    };
    TripPlan const three = crowded({At(0.0f, 1200.0f), At(-1200.0f, 0.0f), At(0.0f, -1200.0f)});
    Add(checks, "ride: three two-errand stops do not crowd out the riding trainer and its mount vendor",
        bothFirst(three), Describe(three));
    TripPlan const two = crowded({At(0.0f, 1200.0f), At(-1200.0f, 0.0f)});
    Add(checks, "ride: the mount vendor rides along with the rank, not as a detour of its own", bothFirst(two),
        Describe(two));
}

// The tame after a pet swap (WUL11): the beast joins the rest of the trip where it adds the fewest yards, not always
// as the next stop; a trip with nothing left, or a place on another map, keeps it next
inline void TameOrder(std::vector<Check>& checks)
{
    ErrandPlace const here = At(0.0f, 0.0f);
    ErrandPlace const beast = At(1500.0f, 0.0f);
    std::vector<TripStop> nearby(1);
    nearby[0].Place = At(100.0f, 0.0f);
    std::vector<TripStop> past(1);
    past[0].Place = At(3000.0f, 0.0f);
    std::vector<TripStop> away(1);
    away[0].Place = {MAP + 1, 100.0f, 0.0f, 0.0f};
    std::size_t const later = TameInsertAt(here, nearby, beast);
    std::size_t const alone = TameInsertAt(here, {}, beast);
    std::size_t const first = TameInsertAt(here, past, beast);
    std::size_t const other = TameInsertAt(here, away, beast);
    Add(checks, "tame: after a pet swap the beast goes where it adds the fewest yards to the rest of the trip",
        later == 1 && alone == 0 && first == 0 && other == 0,
        "after a near stop " + std::to_string(later) + ", alone " + std::to_string(alone) + ", before a stop past it " +
            std::to_string(first) + ", other map " + std::to_string(other));
}

// A weapon skill the gold pays (WU15a): a trip of its own to the weapon master, not to the class trainer nearer by
inline void WeaponTrip(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Weapon = {WEAPON_SPELL};
    bot.Stops = {{TRAINER, At(600.0f, 0.0f), SERVICE_TRAINER},
                 {WEAPON_MASTER, At(1500.0f, 0.0f), SERVICE_WEAPON_MASTER}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const own = plan.Stops.size() == 1 && !plan.Stops[0].OnRoute && plan.Stops[0].Id == WEAPON_MASTER &&
                     HasErrand(plan.Stops[0], ErrandKind::Weapon) && plan.Refused.empty();
    Add(checks, "weapon: a trip of its own to the weapon master, one stop", own, Describe(plan));
}

// A hunter without a pet (WU21b): a trip of its own to the beast's spawn, a stop with no service; a spawn beyond the
// walk limit is refused too_far, and the bot plays on
inline void TameTrip(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Pet = {BEAST, At(700.0f, 0.0f)};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const own = plan.Stops.size() == 1 && plan.Stops[0].Id == BEAST && !plan.Stops[0].Services &&
                     plan.Stops[0].Errands.size() == 1 && HasErrand(plan.Stops[0], ErrandKind::Tame) &&
                     plan.Stops[0].Errands[0].Due && plan.Refused.empty();
    Add(checks, "tame: a trip of its own to the beast's spawn", own, Describe(plan));

    ErrandSnapshot far = Bot(10, 0);
    far.Pet = {BEAST, At(100000.0f, 0.0f)};
    TripPlan const refused = Plan(far, Request(far));
    bool const tooFar = refused.Stops.empty() && refused.Refused.size() == 1 &&
                        refused.Refused[0].Offer.Kind == ErrandKind::Tame &&
                        std::string(refused.Refused[0].Reason) == "too_far";
    Add(checks, "tame: a spawn beyond the walk limit is refused too_far", tooFar, Describe(refused));

    ErrandSnapshot none = Bot(10, 0);
    Add(checks, "tame: no beast, no tame offer",
        std::none_of(CollectOffers(none, TravelProviders()).begin(), CollectOffers(none, TravelProviders()).end(),
                     [](ErrandOffer const& offer) { return offer.Kind == ErrandKind::Tame; }),
        "");
}

// A hunter with an unwanted pet (WU21c): a trip of its own to the stable master (the visit puts the tame after it);
// without a stable master among the stops the swap is refused and the hunter keeps its pet; a beast beyond the tame's
// walk limit is refused too_far with a stable master near (WU21d: the pet is never stored for a beast out of reach)
inline void StableTrip(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Stable = {BEAST, At(700.0f, 0.0f)};
    bot.Stops = {{STABLE_MASTER, At(400.0f, 0.0f), SERVICE_STABLE_MASTER}};
    TripPlan const plan = Plan(bot, Request(bot));
    bool const own = plan.Stops.size() == 1 && plan.Stops[0].Id == STABLE_MASTER &&
                     HasErrand(plan.Stops[0], ErrandKind::Stable) && plan.Stops[0].Errands[0].Due &&
                     !HasErrand(plan.Stops[0], ErrandKind::Tame) && plan.Refused.empty();
    Add(checks, "stable: a trip of its own to the stable master, no tame offer beside it", own, Describe(plan));

    ErrandSnapshot none = Bot(10, 0);
    none.Stable = {BEAST, At(700.0f, 0.0f)};
    TripPlan const refused = Plan(none, Request(none));
    bool const keeps =
        refused.Stops.empty() && refused.Refused.size() == 1 && refused.Refused[0].Offer.Kind == ErrandKind::Stable;
    Add(checks, "stable: no stable master on the map, the swap refused (the hunter keeps its pet)", keeps,
        Describe(refused));

    ErrandSnapshot far = Bot(10, 0);
    far.Stable = {BEAST, At(100000.0f, 0.0f)};
    far.Stops = {{STABLE_MASTER, At(400.0f, 0.0f), SERVICE_STABLE_MASTER}};
    TripPlan const distant = Plan(far, Request(far));
    bool const tooFar = distant.Stops.empty() && distant.Refused.size() == 1 &&
                        distant.Refused[0].Offer.Kind == ErrandKind::Stable &&
                        std::string(distant.Refused[0].Reason) == "too_far";
    Add(checks, "stable: a beast beyond the tame's walk limit, the swap refused too_far (the pet stays out)", tooFar,
        Describe(distant));
    Add(checks, "stable: the service bits stay distinct", ServiceBitsDistinct(), "");
    // The professions plan's kinds and services (WR1): appended, named, the profession trainer a trainer of its own
    Add(checks, "professions: the errand kinds are named profession and craft, the bits distinct",
        std::string(ErrandKindName(ErrandKind::Profession)) == "profession" &&
            std::string(ErrandKindName(ErrandKind::Craft)) == "craft" && ServiceBitsDistinct() &&
            (TRAINER_SERVICES & SERVICE_PROFESSION_TRAINER) && !(TRAINER_SERVICES & SERVICE_STATION),
        "");
    // The auction plan's kind and service (WA1a): appended last, named, the auctioneer a bit of its own, no trainer
    Add(checks, "auction: the errand kind is named auction and appended last, the auctioneer's bit distinct",
        std::string(ErrandKindName(ErrandKind::Auction)) == "auction" && ERRAND_KIND_COUNT == 18 &&
            static_cast<uint8_t>(ErrandKind::Auction) + 1 == ERRAND_KIND_COUNT && ServiceBitsDistinct() &&
            !(TRAINER_SERVICES & SERVICE_AUCTIONEER),
        "");
}

// A vendor within STOP_JOIN_YARDS of the weapon master takes the weapon skill on a route that passes it; a class
// trainer's own stop beside the master keeps its trainer
inline void WeaponAtVendor(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Weapon = {WEAPON_SPELL};
    std::vector<ServiceStop> const masters = {{WEAPON_MASTER, At(800.0f, 0.0f), SERVICE_WEAPON_MASTER}};
    std::vector<ServiceStop> stops = {{PLAIN_VENDOR, At(830.0f, 0.0f), SERVICE_VENDOR},
                                      {TRAINER, At(810.0f, 0.0f), SERVICE_TRAINER}};
    JoinTrainers(stops, masters, SERVICE_WEAPON_MASTER);
    bool const tagged = (stops[0].Services & SERVICE_WEAPON_MASTER) && stops[0].Trainer == WEAPON_MASTER;
    bool const kept = stops[1].Trainer == 0 && stops[1].Services == SERVICE_TRAINER;
    Add(checks, "weapon: a vendor within 40 yd of the master carries it, a class trainer's stop keeps its own",
        tagged && kept, "vendor trainer " + std::to_string(stops[0].Trainer));

    TripStop route;
    route.Place = stops[0].Place;
    route.Services = stops[0].Services;
    route.Id = stops[0].Id;
    route.Trainer = stops[0].Trainer;
    bot.Stops = {stops[0]};
    TripPlan const plan = Plan(bot, Request(bot, {route}));
    bool const joins = plan.Stops.size() == 1 && plan.Stops[0].OnRoute && plan.Stops[0].Trainer == WEAPON_MASTER &&
                       HasErrand(plan.Stops[0], ErrandKind::Weapon) && plan.Refused.empty();
    Add(checks, "weapon: the skill joins the route's vendor stop that carries the master", joins, Describe(plan));
}

// A profession learn or rank (WR4b): not due, it joins a route stop that has the trainer and starts no trip; due, it
// opens a detour trip of its own to the trainer; a vendor within 40 yd of the trainer carries it on a route. A tool
// (WR5b) is a stock kind: due, the vendor that sells it is a trip of its own, and every stock kind keeps a service bit
inline void ProfessionTrips(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Profession = {PROFESSION_SPELL, false};
    TripStop trainer;
    trainer.Place = At(800.0f, 0.0f);
    trainer.Services = SERVICE_PROFESSION_TRAINER;
    trainer.Id = PROFESSION_TRAINER;
    TripPlan const joined = Plan(bot, Request(bot, {trainer}));
    bool const joins = joined.Stops.size() == 1 && joined.Stops[0].OnRoute && joined.Stops[0].DetourMinutes == 0.0f &&
                       HasErrand(joined.Stops[0], ErrandKind::Profession) && joined.Refused.empty();
    Add(checks, "profession: the offer joins a stop that has the trainer", joins, Describe(joined));

    bot.Profession = {PROFESSION_SPELL, true};
    bot.Stops = {{TRAINER, At(600.0f, 0.0f), SERVICE_TRAINER},
                 {PROFESSION_TRAINER, At(1500.0f, 0.0f), SERVICE_PROFESSION_TRAINER}};
    TripPlan const detour = Plan(bot, Request(bot));
    bool const own = detour.Stops.size() == 1 && !detour.Stops[0].OnRoute && detour.Stops[0].Id == PROFESSION_TRAINER &&
                     HasErrand(detour.Stops[0], ErrandKind::Profession) && detour.Refused.empty();
    Add(checks, "profession: a due one opens a detour trip of its own to the trainer", own, Describe(detour));

    std::vector<ServiceStop> const teachers = {{PROFESSION_TRAINER, At(800.0f, 0.0f), SERVICE_PROFESSION_TRAINER}};
    std::vector<ServiceStop> stops = {{PLAIN_VENDOR, At(830.0f, 0.0f), SERVICE_VENDOR},
                                      {TRAINER, At(810.0f, 0.0f), SERVICE_TRAINER}};
    JoinTrainers(stops, teachers, SERVICE_PROFESSION_TRAINER);
    Add(checks, "profession: a vendor within 40 yd of the trainer carries it, a class trainer's stop keeps its own",
        (stops[0].Services & SERVICE_PROFESSION_TRAINER) && stops[0].Trainer == PROFESSION_TRAINER &&
            stops[1].Trainer == 0 && stops[1].Services == SERVICE_TRAINER,
        "vendor trainer " + std::to_string(stops[0].Trainer));

    uint32_t serviceBits = 0;
    for (uint32_t const bit : SERVICE_BITS)
        serviceBits |= bit;
    uint32_t stockServices = 0;
    bool distinct = true;
    for (std::size_t kind = 0; kind < Planner::UPKEEP_STOCK_COUNT; ++kind)
    {
        uint32_t const service = StockServices(Planner::StockBit(static_cast<Planner::UpkeepStock>(kind)));
        distinct = distinct && service && !(service & (serviceBits | stockServices));
        stockServices |= service;
    }
    Add(checks, "tool: every stock kind, the tool too, has a service bit of its own", distinct,
        "stock services " + std::to_string(stockServices));

    Planner::StockMask const tool = Planner::StockBit(Planner::UpkeepStock::Tool);
    ErrandSnapshot miner = Bot(10, 0);
    miner.DueStock = tool;
    miner.Stops = {{PLAIN_VENDOR, At(300.0f, 0.0f), VendorServices(miner, PLAIN_VENDOR, false, 0)},
                   {ROAD_VENDOR, At(900.0f, 0.0f), VendorServices(miner, ROAD_VENDOR, false, tool)}};
    TripPlan const toolPlan = Plan(miner, Request(miner));
    bool const toolTrip = toolPlan.Stops.size() == 1 && toolPlan.Stops[0].Id == ROAD_VENDOR &&
                          HasErrand(toolPlan.Stops[0], ErrandKind::Shop) && toolPlan.Refused.empty();
    Add(checks, "tool: a due tool goes to the vendor that sells it, past a nearer one that does not", toolTrip,
        Describe(toolPlan));
}

// The craft errand (WR14): a queue worth more than the walk opens a trip of its own to the station, one worth less
// none; a craft without a station joins the route's stop and opens no trip; the craft comes before the deposit
inline void CraftTrips(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.Craft = {CRAFT_SPELL, CRAFT_FOCUS, 50000.0f};
    bot.Stops = {{PLAIN_VENDOR, At(300.0f, 0.0f), SERVICE_VENDOR}, {STATION, At(600.0f, 0.0f), SERVICE_STATION}};
    TripPlan const worth = Plan(bot, Request(bot), TripProviders());
    bool const own = worth.Stops.size() == 1 && worth.Stops[0].Id == STATION && !worth.Stops[0].OnRoute &&
                     HasErrand(worth.Stops[0], ErrandKind::Craft) && worth.Refused.empty();
    Add(checks, "craft: a queue worth the walk opens a trip of its own to the station", own, Describe(worth));

    bot.Craft.WorthCopper = 0.0f;
    TripPlan const cheap = Plan(bot, Request(bot), TripProviders());
    Add(checks, "craft: a queue worth nothing opens no trip", cheap.Stops.empty() && cheap.Refused.size() == 1,
        Describe(cheap));

    ErrandSnapshot plain = Bot(10, 0);
    plain.Craft = {CRAFT_SPELL, 0, 50000.0f};
    plain.Stops = {{PLAIN_VENDOR, At(300.0f, 0.0f), SERVICE_VENDOR}};
    TripPlan const alone = Plan(plain, Request(plain), TripProviders());
    TripStop route;
    route.Place = At(300.0f, 0.0f);
    route.Services = SERVICE_VENDOR;
    route.Id = PLAIN_VENDOR;
    TripPlan const joined = Plan(plain, Request(plain, {route}), TripProviders());
    bool const joins = joined.Stops.size() == 1 && joined.Stops[0].OnRoute &&
                       HasErrand(joined.Stops[0], ErrandKind::Craft) && joined.Refused.empty();
    Add(checks, "craft: one without a station joins the route's stop and opens no trip of its own",
        alone.Stops.empty() && joins, Describe(alone) + "; " + Describe(joined));

    // WR15: a station the bot lights itself (a cook's campfire) is carried: the route's stop without one takes it
    ErrandSnapshot carried = Bot(10, 0);
    carried.Craft = {CRAFT_SPELL, CRAFT_FOCUS, 0.0f, true};
    carried.Stops = {{PLAIN_VENDOR, At(300.0f, 0.0f), SERVICE_VENDOR}, {STATION, At(600.0f, 0.0f), SERVICE_STATION}};
    TripPlan const lit = Plan(carried, Request(carried, {route}), TripProviders());
    bool const litJoins = lit.Stops.size() == 1 && lit.Stops[0].OnRoute && lit.Stops[0].Id == PLAIN_VENDOR &&
                          HasErrand(lit.Stops[0], ErrandKind::Craft) && lit.Refused.empty();
    Add(checks, "craft: a cook that lights its own fire cooks at the route's stop without a fire", litJoins,
        Describe(lit));

    ErrandSnapshot banked = GovernedBot(10, 0);
    banked.Craft = {CRAFT_SPELL, CRAFT_FOCUS, 50000.0f};
    banked.BankDepositSlots = 4;
    WithServices(banked, {},
                 {{STATION, At(400.0f, 0.0f), SERVICE_STATION}, {BANKER_ID, At(420.0f, 0.0f), SERVICE_BANKER}});
    TripPlan const both = Plan(banked, Request(banked), TripProviders());
    bool ordered = false;
    for (TripStop const& stop : both.Stops)
        for (std::size_t e = 0; e + 1 < stop.Errands.size(); ++e)
            ordered = ordered || (stop.Errands[e].Kind == ErrandKind::Craft &&
                                  stop.Errands[e + 1].Kind == ErrandKind::BankDeposit);
    Add(checks, "craft: at a station beside a banker the craft comes before the deposit", ordered && OrderHolds(both),
        Describe(both));
}

// ---- The record of failed attempts at the trip start (NR9; NR8: stops planned where a trip had just died) ----

constexpr uint32_t RECORD_NOW_MS = 1000000;
constexpr uint32_t RECORD_BASE_MS = 10 * 60 * 1000;

inline std::string DescribeRefused(std::vector<Refusal> const& refused)
{
    std::string text = "record refused:";
    for (Refusal const& refusal : refused)
        text += std::string(" ") + ErrandKindName(refusal.Offer.Kind) + "/" + refusal.Reason;
    return text;
}

inline bool PlanStopsAt(TripPlan const& plan, ErrandPlace const& place)
{
    for (TripStop const& stop : plan.Stops)
        if (Yards(stop.Place, place) <= STOP_JOIN_YARDS)
            return true;
    return false;
}

// I1: a place a live trip_deaths entry refuses is never a planned stop, the next stop that serves is; what only the
// refused place took is refused with its own word, and the place is planned again once the entry has expired
inline void RecordRefusedStops(std::vector<Check>& checks)
{
    ErrandPlace const deaths = At(300.0f, 0.0f);
    FailedAttempts::Record record;
    FailedAttempts::Failure death;
    death.goal = {deaths.MapId, deaths.X, deaths.Y, deaths.Z};
    bool const noted =
        FailedAttempts::NoteFailure(record, "trip_deaths", death, RECORD_NOW_MS, RECORD_BASE_MS).has_value();
    auto const refusedAt = [&record](uint32_t nowMs)
    {
        return [&record, nowMs](ErrandPlace const& place)
        {
            return !FailedAttempts::IsPlaceRefused(record, SightPoint{place.MapId, place.X, place.Y, place.Z}, nowMs)
                        .empty();
        };
    };
    auto const filtered = [&refusedAt](ErrandSnapshot const& bot, uint32_t nowMs, std::vector<Refusal>& refused)
    {
        std::vector<ServiceStop> stops = bot.Stops;
        std::vector<ErrandOffer> offers = CollectOffers(bot, TripProviders());
        refused = KeepUnrefusedErrands(stops, offers, refusedAt(nowMs));
        return PlanTrip(Request(bot), stops, offers);
    };

    // Seen mail worth both detours: the nearer mailbox wins unless the record refuses it
    ErrandSnapshot two = GovernedBot(10, 0);
    two.MailWaiting = 1;
    two.MailSeenCopper = 5000;
    WithServices(two, {}, {{MAILBOX_ID, deaths, SERVICE_MAILBOX}, {MAILBOX_ID + 1, At(450.0f, 0.0f), SERVICE_MAILBOX}});
    TripPlan const unfiltered = Plan(two, Request(two), TripProviders());
    std::vector<Refusal> refused;
    TripPlan const next = filtered(two, RECORD_NOW_MS, refused);
    bool const premise =
        noted && two.Stops.size() == 2 && unfiltered.Stops.size() == 1 && unfiltered.Stops[0].Id == MAILBOX_ID;
    bool const moved = next.Stops.size() == 1 && next.Stops[0].Id == MAILBOX_ID + 1 &&
                       HasErrand(next.Stops[0], ErrandKind::Mail) && refused.empty() && !PlanStopsAt(next, deaths);
    Add(checks, "record: a mailbox a live trip_deaths entry refuses is no stop, the next mailbox is (I1)",
        premise && moved, Describe(unfiltered) + " | " + Describe(next) + " " + DescribeRefused(refused));

    // The only mailbox refused, and a turn-in placed at that place: both are left out with place_refused, not no_stop
    ErrandSnapshot one = two;
    WithServices(one, {}, {{MAILBOX_ID, deaths, SERVICE_MAILBOX}});
    one.Completed = {{FAR_QUEST, ENDER, false, At(300.0f, 5.0f), 50000.0f}};
    TripPlan const none = filtered(one, RECORD_NOW_MS, refused);
    bool words = refused.size() == 2;
    for (Refusal const& refusal : refused)
        words = words && std::string(refusal.Reason) == PLACE_REFUSED &&
                (refusal.Offer.Kind == ErrandKind::Mail || refusal.Offer.Kind == ErrandKind::TurnIn);
    bool noStop = false;
    for (Refusal const& refusal : none.Refused)
        noStop = noStop || std::string(refusal.Reason) == "no_stop";
    Add(checks, "record: the only mailbox and a turn-in at a refused place are refused place_refused, no stop (I1)",
        words && none.Stops.empty() && !noStop, Describe(none) + " " + DescribeRefused(refused));

    // Past the entry's live time the place is planned again
    uint32_t const later = RECORD_NOW_MS + 100 * RECORD_BASE_MS;
    TripPlan const again = filtered(two, later, refused);
    Add(checks, "record: an expired entry refuses nothing, the nearer mailbox is planned again",
        refused.empty() && again.Stops.size() == 1 && again.Stops[0].Id == MAILBOX_ID,
        Describe(again) + " " + DescribeRefused(refused));
}

// A stop's vendor part passed without a sale writes the word of what stopped it (NR9): not found (near the stop or by
// its spawn) and found but gone or dead are two causes with two words; a serving vendor writes none
inline void VendorPassedWords(std::vector<Check>& checks)
{
    char const* const missing = VendorPassedWord(false, false);
    char const* const gone = VendorPassedWord(true, false);
    bool const words = missing && gone && std::string(missing) == "no_vendor" && std::string(gone) == "vendor_gone" &&
                       !VendorPassedWord(true, true);
    Add(checks, "vendor: a stop whose vendor is not found writes no_vendor, one gone or dead vendor_gone", words,
        std::string(missing ? missing : "-") + " / " + (gone ? gone : "-"));
}

constexpr uint64_t DEFAULT_SEED = 20261113;
constexpr uint32_t SEARCH_ITERATIONS = 20000;

inline std::vector<Check> SyntheticChecks(uint64_t seed = DEFAULT_SEED)
{
    std::vector<Check> checks;
    MailScenarios(checks);
    OrderlinessScenarios(checks);
    BankScenarios(checks);
    OrderScenario(checks);
    AuctionScenarios(checks);
    InvariantSearch(checks, seed, SEARCH_ITERATIONS);
    OneHub(checks);
    BagOnTheWay(checks);
    DetourRefused(checks);
    TwoAtOneStop(checks);
    SaleRuleKept(checks);
    GreyOnlyNoTrip(checks);
    NoStop(checks);
    ErrandEnds(checks);
    NeedsCheckOnce(checks);
    VisitOnce(checks);
    LaterProvider(checks);
    RepairByNeed(checks);
    ShopByCover(checks);
    TrainingJoins(checks);
    TrainingAtVendor(checks);
    TrainingTrip(checks);
    TrainingRefused(checks);
    RidingTrip(checks);
    RidingWithoutVendor(checks);
    RidingCrowded(checks);
    WeaponTrip(checks);
    WeaponAtVendor(checks);
    ProfessionTrips(checks);
    CraftTrips(checks);
    TameTrip(checks);
    StableTrip(checks);
    TameOrder(checks);
    RecordRefusedStops(checks);
    VendorPassedWords(checks);
    return checks;
}
}  // namespace ErrandHarness
}  // namespace WorldTravel

#endif
