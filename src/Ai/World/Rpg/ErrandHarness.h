/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ERRANDHARNESS_H
#define _PLAYERBOT_ERRANDHARNESS_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ErrandRules.h"

// The trip planner's scenarios (travel plan WT15): synthetic bots and stops, one check per decision about which stops a
// trip makes and which errands it does or refuses there. Shared by tests/errand-trips-selftest.cpp. Player-free
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
constexpr uint32_t FLUSH = 100000;  // copper
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
    bot.DurabilityPercent = 10;
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

// Two errands each worth less than the detour, worth more together: one stop does both
inline void TwoAtOneStop(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(6, 3);
    bot.Level = 45;  // band 2: r 25, t 8, e 250
    bot.Rates = CurrentRates(SlotMeasure{}, DEFAULT_SLOT_BANDS[LevelBand(bot.Level)]);
    bot.DurabilityPercent = 5;
    bot.Stops = {{REPAIR_HUB, At(150.0f, 0.0f), SERVICE_VENDOR | SERVICE_REPAIR}};
    ErrandSnapshot sellOnly = bot;
    sellOnly.DurabilityPercent = PERCENT;
    ErrandSnapshot repairOnly = bot;
    repairOnly.SellableSlots = 0;
    bool const aloneRefused =
        Plan(sellOnly, Request(sellOnly)).Stops.empty() && Plan(repairOnly, Request(repairOnly)).Stops.empty();
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

// Nothing near offers the service: refused as no_stop, nothing else changes
inline void NoStop(std::vector<Check>& checks)
{
    ErrandSnapshot bot = Bot(10, 0);
    bot.DurabilityPercent = 0;
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
        case ErrandKind::Mail:
            return 2;
        default:
            return -1;
    }
}

// The order at a stop that has them all: banker withdrawal, sale, mailbox
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
    WithServices(bot, vendors, extras);
    return bot;
}

inline std::vector<TripStop> RandomRoute(Rng& rng, ErrandSnapshot const& bot)
{
    std::vector<TripStop> route;
    if (!bot.Stops.empty() && rng.Chance(40))
    {
        ServiceStop const& pick = bot.Stops[rng.Below(static_cast<uint32_t>(bot.Stops.size()))];
        route.push_back({pick.Place, pick.Services, pick.Id, false, 0.0f, {}});
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
inline void InvariantSearch(std::vector<Check>& checks, uint64_t seed, uint32_t iterations)
{
    Rng rng(seed);
    std::string unopened;
    std::string ungoverned;
    std::string order;
    std::string worth;
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
    }
    std::string const stats = std::to_string(iterations) + " snapshots, seed " + std::to_string(seed);
    Add(checks, "search: unopened mail alone never earns a detour", unopened.empty() && mailJoined > iterations / 20,
        stats + ", mail joined a stop in " + std::to_string(mailJoined) + "; " + unopened);
    Add(checks, "search: no bank or mail errand for an ungoverned bot", ungoverned.empty() && withBankMail > 0,
        stats + ", " + std::to_string(withBankMail) + " bank/mail offers; " + ungoverned);
    Add(checks, "search: bank withdrawal before sale before mailbox at every stop", order.empty() && detoured > 0,
        stats + ", " + std::to_string(detoured) + " plans with detours; " + order);
    Add(checks, "search: every provider's worth is finite and not negative", worth.empty(), stats + "; " + worth);
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
    InvariantSearch(checks, seed, SEARCH_ITERATIONS);
    OneHub(checks);
    BagOnTheWay(checks);
    DetourRefused(checks);
    TwoAtOneStop(checks);
    SaleRuleKept(checks);
    NoStop(checks);
    LaterProvider(checks);
    return checks;
}
}  // namespace ErrandHarness
}  // namespace WorldTravel

#endif
