/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ERRANDRULES_H
#define _PLAYERBOT_ERRANDRULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "ItemWorthRules.h"
#include "MailRules.h"

// Trips and errands (travel plan WT15, PLAN 3.11, WT.6): providers say what an errand needs (a service), where it can
// be done and what it is worth now; PlanTrip puts every errand on a stop of the trip and opens a detour only when the
// errands done there are worth more than the detour costs. Player-free: the server fills the ErrandSnapshot on the
// bot's map thread, tests/errand-trips-selftest.cpp runs the same code.
namespace WorldTravel
{
// Append-only: the upkeep needs (WU) shop, train and ride; later plans add mail and the bank (WB), the auction house
// (WA) and town visits (WC)
enum class ErrandKind : uint8_t
{
    TurnIn,
    Sell,
    Repair,
    BuyBag,
    BindHearth,
    LearnFlight,
    Shop,
    Train,
    Ride,
    Weapon,        // a weapon skill at a weapon master (WU15a)
    Mail,          // the bank-mail plan (WB8)
    BankWithdraw,  // due items from the bank
    BankDeposit    // kept items into the bank
};
constexpr uint8_t ERRAND_KIND_COUNT = 13;

inline char const* ErrandKindName(ErrandKind kind)
{
    switch (kind)
    {
        case ErrandKind::TurnIn:
            return "turn_in";
        case ErrandKind::Sell:
            return "sell";
        case ErrandKind::Repair:
            return "repair";
        case ErrandKind::BuyBag:
            return "buy_bag";
        case ErrandKind::BindHearth:
            return "bind_hearth";
        case ErrandKind::LearnFlight:
            return "learn_flight";
        case ErrandKind::Shop:
            return "shop";
        case ErrandKind::Train:
            return "train";
        case ErrandKind::Ride:
            return "ride";
        case ErrandKind::Weapon:
            return "weapon";
        case ErrandKind::Mail:
            return "mail";
        case ErrandKind::BankWithdraw:
            return "bank_withdraw";
        case ErrandKind::BankDeposit:
            return "bank_deposit";
    }
    return "other";
}

// What a stop offers (bits, append-only)
constexpr uint32_t SERVICE_QUEST_ENDER = 1u << 0;
constexpr uint32_t SERVICE_VENDOR = 1u << 1;
constexpr uint32_t SERVICE_REPAIR = 1u << 2;
constexpr uint32_t SERVICE_INNKEEPER = 1u << 3;
constexpr uint32_t SERVICE_FLIGHT_MASTER = 1u << 4;
// The upkeep needs' services are the bot's own: the errand check marks the stops that answer its needs
constexpr uint32_t SERVICE_TRAINER = 1u << 5;         // teaches the bot's class spells
constexpr uint32_t SERVICE_RIDING_TRAINER = 1u << 6;  // teaches the riding rank the bot buys
constexpr uint32_t SERVICE_MOUNT_VENDOR = 1u << 7;    // sells that rank's mount for the bot's race
constexpr uint32_t SERVICE_WEAPON_MASTER = 1u << 8;   // teaches the weapon skill the bot learns (WU15a)
// A stop has one trainer to visit: a stop with any of these keeps its own
constexpr uint32_t TRAINER_SERVICES = SERVICE_TRAINER | SERVICE_RIDING_TRAINER | SERVICE_WEAPON_MASTER;
// A vendor that sells the bot a consumable stock kind: the kind's bit (UpkeepNeeds' StockBit) from this bit on
constexpr uint32_t SERVICE_STOCK_SHIFT = 24;

constexpr uint32_t StockServices(uint8_t stockKinds)
{
    return static_cast<uint32_t>(stockKinds) << SERVICE_STOCK_SHIFT;
}
constexpr uint32_t SERVICE_MAILBOX = 1u << 8;
constexpr uint32_t SERVICE_BANKER = 1u << 9;
// Unopened mail alone is worth at most this share of a usual trip's minutes at the earning rate, so it never pays a
// detour (a detour costs at least that trip's minutes) yet still joins any stop with a mailbox (WB.4)
constexpr float UNOPENED_MAIL_TRIP_SHARE = 0.5f;

// An errand place this close to a stop is done at that stop (a town square; quest camps cluster at 25 to 70 yd)
constexpr float STOP_JOIN_YARDS = 40.0f;
// A trip opens at most this many detours
constexpr std::size_t MAX_DETOUR_STOPS = 3;
// A need that is not due joins a stop the trip makes anyway and never pays a detour
constexpr float JOIN_WORTH = 1.0f;  // copper

struct ErrandPlace
{
    uint32_t MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

inline float Yards(ErrandPlace const& a, ErrandPlace const& b)
{
    if (a.MapId != b.MapId)
        return std::numeric_limits<float>::infinity();

    float const dx = a.X - b.X;
    float const dy = a.Y - b.Y;
    float const dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A provider's answer: what the errand needs, where it can be done and what it is worth right now (copper)
struct ErrandOffer
{
    ErrandKind Kind = ErrandKind::Sell;
    uint32_t Service = 0;
    bool Placed = false;  // done at Place only (a quest's ender, a vendor's bag); else at any stop with the service
    ErrandPlace Place;
    float WorthCopper = 0.0f;
    uint32_t Id = 0;      // the quest, the bag item, the inn or node; 0 for selling and repairing
    uint32_t Target = 0;  // the creature or object entry the errand talks to at Place; 0 = any with the service
    bool TargetIsObject = false;
    bool Due = false;  // an upkeep need due now: worth a trip of its own, also for a questing bot
};

// A place near the bot that offers services: a vendor (that may repair), an innkeeper, a flight master
struct ServiceStop
{
    uint32_t Id = 0;  // the creature entry
    ErrandPlace Place;
    uint32_t Services = 0;
    uint32_t Trainer = 0;  // the class trainer's entry when this stop takes training because the trainer stands within
                           // STOP_JOIN_YARDS (a vendor stop); 0 = none, or the stop is the trainer's own (Id)
};

struct CompletedQuest
{
    uint32_t QuestId = 0;
    uint32_t Ender = 0;  // creature or object entry
    bool EnderIsObject = false;
    ErrandPlace Place;  // the ender's nearest spawn
    float WorthCopper = 0.0f;
};

struct BagOfferAt
{
    uint32_t Item = 0;
    uint32_t Vendor = 0;
    ErrandPlace Place;
    uint32_t Price = 0;
    uint32_t Slots = 0;
    uint32_t ReplacedSlots = 0;  // the bag it replaces; 0 for an empty bag slot
};

struct PlaceOffer
{
    uint32_t Id = 0;
    uint32_t Target = 0;
    ErrandPlace Place;
    float WorthCopper = 0.0f;
};

// The class trainer's spells the bot may learn now (UpkeepNeeds' training need); Units 0 = none
struct TrainingAt
{
    uint32_t Units = 0;
    uint32_t LevelsBehind = 0;
    bool Due = false;  // the waiting spells span the bot's training span
    uint32_t Spell = 0;
};

// The riding rank the bot buys (UpkeepNeeds' riding need, always due); Spell 0 = none
struct RidingAt
{
    uint32_t Spell = 0;
    uint32_t MountVendor = 0;  // the vendor entry of the race's cheapest mount; 0 for cold weather flying
    bool Learn = true;         // the rank is to learn; false: the rank is known, only its mount is missing
};

// The weapon skill the bot learns (UpkeepNeeds' weapon need whose funded gold pays it, WU15a); Spell 0 = none
struct WeaponAt
{
    uint32_t Spell = 0;
};

// What the providers read: filled on the bot's map thread, never a Player
struct ErrandSnapshot
{
    uint32_t Level = 1;
    uint64_t Money = 0;
    SlotRates Rates;
    uint32_t BagHorizonHours = 0;
    uint32_t FreeSlots = 0;
    uint32_t SellableSlots = 0;
    float MinutesToNextStop = 0.0f;  // until the bot passes a vendor anyway
    uint64_t Reserve = 0;            // ShouldBuyBag's reserve
    uint32_t ThriftPercent = PERCENT;
    uint32_t BagBudgetPercent = PERCENT;
    std::vector<ServiceStop> Stops;
    std::vector<CompletedQuest> Completed;
    std::vector<BagOfferAt> Bags;
    std::vector<PlaceOffer> Inns;         // an inn worth binding at
    std::vector<PlaceOffer> FlightNodes;  // an undiscovered node worth learning
    // Bank and mail (WB8): only for a bot the bank rule governs (a persistent bot without a master)
    bool Governed = false;
    float Orderliness = 0.0f;         // the trait, a unit in -1..1
    uint32_t MailWaiting = 0;         // unopened mails
    uint32_t MailDefaultWorth = 100;  // copper per unopened mail (the config's MailDefaultWorthCopper)
    uint32_t MailSeenCopper = 0;      // what the bot saw and left in the mail
    uint32_t MailExpectedCopper = 0;  // what it expects from its own sales
    uint32_t BankDueCount = 0;        // kept items that have come due
    uint32_t BankDueWorth = 0;        // their worth
    uint32_t BankDepositSlots = 0;    // bag slots kept items hold that a deposit would free
    // The upkeep needs (UpkeepNeedRules.h): the stops that answer them carry the services above
    bool RepairDue = false;  // UpkeepNeeds::RepairErrandDue, and a repairer within the walk limit
    uint8_t DueStock = 0;    // the stock kinds due whose lot the gold pays
    uint8_t OtherStock = 0;  // the stock kinds below their target whose lot the gold pays
    TrainingAt Training;
    RidingAt Riding;  // one rank per trip, the lowest
    WeaponAt Weapon;  // one weapon skill per trip
};

using ErrandProvider = void (*)(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers);

struct ProviderRow
{
    ErrandKind Kind;
    char const* Name;
    ErrandProvider Provide;
};

// Selling frees the slots for the time until the next natural stop (WT3's TripWorthIt counts the same worth)
inline float SaleWorth(SlotRates const& rates, uint32_t freeSlots, uint32_t sellableSlots, float minutesToNextStop)
{
    return SlotsWorthPerHour(rates, freeSlots, sellableSlots) * minutesToNextStop / MINUTES_PER_HOUR;
}

// A detour costs a usual trip's minutes (t) plus the walk it adds, at the earning rate: TripWorthIt's cost
inline float DetourCost(SlotRates const& rates, float walkMinutes)
{
    return (rates.TripMinutes + walkMinutes) * rates.EarnCopperPerMinute;
}

// A due need is worth a trip of its own: more than the detour to any stop within the walk limit
inline float DueWorth(SlotRates const& rates) { return DetourCost(rates, ERRAND_MAX_WALK_MINUTES) + 1.0f; }

// The services of a vendor stop: repairing, the stock kinds it sells the bot (UpkeepNeeds::VendorCover), the mount
inline uint32_t VendorServices(ErrandSnapshot const& snapshot, uint32_t vendorEntry, bool repairs, uint8_t stockKinds)
{
    uint32_t services = SERVICE_VENDOR | StockServices(stockKinds);
    if (repairs)
        services |= SERVICE_REPAIR;
    if (snapshot.Riding.MountVendor && vendorEntry == snapshot.Riding.MountVendor)
        services |= SERVICE_MOUNT_VENDOR;
    return services;
}

inline void ProvideTurnIns(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (CompletedQuest const& quest : snapshot.Completed)
        offers.push_back({ErrandKind::TurnIn, SERVICE_QUEST_ENDER, true, quest.Place, quest.WorthCopper, quest.QuestId,
                          quest.Ender, quest.EnderIsObject});
}

inline void ProvideSale(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.SellableSlots)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Sell;
    offer.Service = SERVICE_VENDOR;
    offer.WorthCopper =
        SaleWorth(snapshot.Rates, snapshot.FreeSlots, snapshot.SellableSlots, snapshot.MinutesToNextStop);
    offers.push_back(offer);
}

// The repair need (§3.5) at any vendor that repairs, when due and the gold pays an item
inline void ProvideRepair(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.RepairDue)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Repair;
    offer.Service = SERVICE_REPAIR;
    offer.WorthCopper = DueWorth(snapshot.Rates);
    offer.Due = true;
    offers.push_back(offer);
}

// One offer per stock kind (§3.6) at any vendor that sells it: the vendor covering the most due kinds gains most, then
// the one covering more other kinds, then the nearest; the vendor visit's shopping list buys there (ShopAtVendor)
inline void ProvideShopping(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (uint32_t bit = 1; bit <= UINT8_MAX; bit <<= 1)
    {
        bool const due = (snapshot.DueStock & bit) != 0;
        if (!due && !(snapshot.OtherStock & bit))
            continue;

        ErrandOffer offer;
        offer.Kind = ErrandKind::Shop;
        offer.Service = SERVICE_VENDOR | StockServices(static_cast<uint8_t>(bit));
        offer.WorthCopper = due ? DueWorth(snapshot.Rates) : JOIN_WORTH;
        offer.Id = bit;
        offer.Due = due;
        offers.push_back(offer);
    }
}

// Training (§3.7) at any of the class's trainers: it joins a trip that passes one, its own trip once enough levels wait
inline void ProvideTraining(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Training.Units)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Train;
    offer.Service = SERVICE_TRAINER;
    offer.WorthCopper = snapshot.Training.Due ? DueWorth(snapshot.Rates) : JOIN_WORTH;
    offer.Id = snapshot.Training.Spell;
    offer.Due = snapshot.Training.Due;
    offers.push_back(offer);
}

// A riding rank bought (§3.8): one trip to the riding trainer and the race's mount vendor; a known rank whose mount
// is missing (WU14) goes to the mount vendor alone
inline void ProvideRiding(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Riding.Spell)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Ride;
    offer.Service = SERVICE_RIDING_TRAINER;
    offer.WorthCopper = DueWorth(snapshot.Rates);
    offer.Id = snapshot.Riding.Spell;
    offer.Due = true;
    if (snapshot.Riding.Learn)
        offers.push_back(offer);
    if (!snapshot.Riding.MountVendor)
        return;

    offer.Service = SERVICE_MOUNT_VENDOR;
    offer.Target = snapshot.Riding.MountVendor;
    offers.push_back(offer);
}

// A weapon skill (§3.7, WU15a): one trip to a weapon master that teaches it; due once the gold pays it
inline void ProvideWeaponMaster(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Weapon.Spell)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Weapon;
    offer.Service = SERVICE_WEAPON_MASTER;
    offer.WorthCopper = DueWorth(snapshot.Rates);
    offer.Id = snapshot.Weapon.Spell;
    offer.Due = true;
    offers.push_back(offer);
}

// The errand check on a map without a vendor that trades with the bot (WU12 NOTE 4): a trainer need still plans its
// trip (class training or a riding rank, their trainers are stops of their own); without one it is skipped
constexpr bool ErrandCheckPlans(bool vendorOnMap, bool trainerWanted) { return vendorOnMap || trainerWanted; }

// A bag ShouldBuyBag takes; buying it now gains its slots until the next vendor stop (as the goal model's bag offer)
inline void ProvideBags(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (BagOfferAt const& bag : snapshot.Bags)
    {
        if (bag.Slots <= bag.ReplacedSlots)
            continue;

        BagBuyInput in;
        in.Price = bag.Price;
        in.Worth = BagWorth(snapshot.Rates, snapshot.FreeSlots, bag.Slots, bag.ReplacedSlots, snapshot.BagHorizonHours);
        in.Gold = snapshot.Money;
        in.Reserve = snapshot.Reserve;
        in.ThriftPercent = snapshot.ThriftPercent;
        in.BudgetPercent = snapshot.BagBudgetPercent;
        if (!ShouldBuyBag(in))
            continue;

        float const worth =
            SaleWorth(snapshot.Rates, snapshot.FreeSlots, bag.Slots - bag.ReplacedSlots, snapshot.MinutesToNextStop);
        offers.push_back({ErrandKind::BuyBag, SERVICE_VENDOR, true, bag.Place, worth, bag.Item, bag.Vendor, false});
    }
}

inline void ProvideHearth(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (PlaceOffer const& inn : snapshot.Inns)
        offers.push_back(
            {ErrandKind::BindHearth, SERVICE_INNKEEPER, true, inn.Place, inn.WorthCopper, inn.Id, inn.Target, false});
}

inline void ProvideFlightNodes(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (PlaceOffer const& node : snapshot.FlightNodes)
        offers.push_back({ErrandKind::LearnFlight, SERVICE_FLIGHT_MASTER, true, node.Place, node.WorthCopper, node.Id,
                          node.Target, false});
}

// The mail errand (WB8): the worth of MailErrandWorth with the bot's orderliness as weight. The unopened part
// (waiting x default) is capped below a detour's least cost, so it joins a stop that has a mailbox but never opens one;
// what the bot saw or expects counts in full and can pay a detour. Placed nowhere: any stop with a mailbox takes it
inline float MailWorth(ErrandSnapshot const& snapshot)
{
    float const weight = BankMail::OrderlinessWeight(snapshot.Orderliness);
    double const full = BankMail::MailErrandWorth(snapshot.MailWaiting, snapshot.MailDefaultWorth,
                                                  snapshot.MailSeenCopper, snapshot.MailExpectedCopper, weight);
    double const unopened = BankMail::MailErrandWorth(snapshot.MailWaiting, snapshot.MailDefaultWorth, 0, 0, weight);
    double const cap =
        static_cast<double>(UNOPENED_MAIL_TRIP_SHARE) * snapshot.Rates.TripMinutes * snapshot.Rates.EarnCopperPerMinute;
    double const worth = full - unopened + std::min(unopened, std::max(cap, 0.0));
    return static_cast<float>(std::min(std::max(worth, 0.0), 4.0e9));
}

inline void ProvideMail(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Governed || (!snapshot.MailWaiting && !snapshot.MailSeenCopper && !snapshot.MailExpectedCopper))
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Mail;
    offer.Service = SERVICE_MAILBOX;
    offer.WorthCopper = MailWorth(snapshot);
    offers.push_back(offer);
}

// Due items waiting in the bank, weighted by orderliness
inline void ProvideBankWithdraw(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Governed || !snapshot.BankDueCount)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::BankWithdraw;
    offer.Service = SERVICE_BANKER;
    offer.WorthCopper = static_cast<float>(snapshot.BankDueWorth) * BankMail::OrderlinessWeight(snapshot.Orderliness);
    offers.push_back(offer);
}

// Kept items out of the bags: the slots they free are worth the same as a sale's until the next stop, unweighted
inline void ProvideBankDeposit(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Governed || !snapshot.BankDepositSlots)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::BankDeposit;
    offer.Service = SERVICE_BANKER;
    offer.WorthCopper =
        SaleWorth(snapshot.Rates, snapshot.FreeSlots, snapshot.BankDepositSlots, snapshot.MinutesToNextStop);
    offers.push_back(offer);
}

// A mailbox or banker near a stop is done at that stop (STOP_JOIN_YARDS): its service bit goes onto the nearest stop
// within that distance, else it stands as a stop of its own. PlanTrip joins an unplaced errand only to a stop whose
// Services carry the bit, so the snapshot's stops come through here (WB8)
inline void MergeServiceStops(std::vector<ServiceStop>& stops, std::vector<ServiceStop> const& extras)
{
    for (ServiceStop const& extra : extras)
    {
        ServiceStop* nearest = nullptr;
        float best = STOP_JOIN_YARDS;
        for (ServiceStop& stop : stops)
        {
            float const yards = Yards(stop.Place, extra.Place);
            if (yards <= best)
            {
                best = yards;
                nearest = &stop;
            }
        }
        if (nearest)
            nearest->Services |= extra.Services;
        else
            stops.push_back(extra);
    }
}

// The travel plan's providers; a later plan adds its row to its own copy of this table, PlanTrip stays as it is
inline std::vector<ProviderRow> const& TravelProviders()
{
    static std::vector<ProviderRow> const rows = {
        {ErrandKind::TurnIn, "turn_in", &ProvideTurnIns},
        {ErrandKind::Sell, "sell", &ProvideSale},
        {ErrandKind::Repair, "repair", &ProvideRepair},
        {ErrandKind::BuyBag, "buy_bag", &ProvideBags},
        {ErrandKind::BindHearth, "bind_hearth", &ProvideHearth},
        {ErrandKind::LearnFlight, "learn_flight", &ProvideFlightNodes},
        {ErrandKind::Shop, "shop", &ProvideShopping},
        {ErrandKind::Train, "train", &ProvideTraining},
        {ErrandKind::Ride, "ride", &ProvideRiding},
        {ErrandKind::Weapon, "weapon", &ProvideWeaponMaster},
    };
    return rows;
}

// The trip's providers with the bank and mail rows. A stop does its errands in the order the offers come: a banker's
// withdrawal first (it may bring gear or sellable items), then selling and the rest, then the deposit and the mailbox
inline std::vector<ProviderRow> const& TripProviders()
{
    static std::vector<ProviderRow> const rows = []
    {
        std::vector<ProviderRow> all = {{ErrandKind::BankWithdraw, "bank_withdraw", &ProvideBankWithdraw}};
        all.insert(all.end(), TravelProviders().begin(), TravelProviders().end());
        all.push_back({ErrandKind::BankDeposit, "bank_deposit", &ProvideBankDeposit});
        all.push_back({ErrandKind::Mail, "mail", &ProvideMail});
        return all;
    }();
    return rows;
}

inline std::vector<ErrandOffer> CollectOffers(ErrandSnapshot const& snapshot, std::vector<ProviderRow> const& rows)
{
    std::vector<ErrandOffer> offers;
    for (ProviderRow const& row : rows)
        row.Provide(snapshot, offers);
    return offers;
}

struct TripStop
{
    ErrandPlace Place;
    uint32_t Services = 0;
    uint32_t Id = 0;       // the service stop's entry; 0 for a placed errand's own place
    uint32_t Trainer = 0;  // the trainer to visit at this stop when it is not the stop's own entry (ServiceStop)
    bool OnRoute = false;  // a stop the bot makes anyway
    float DetourMinutes = 0.0f;
    std::vector<ErrandOffer> Errands;
};

struct TripRequest
{
    ErrandPlace Start;
    std::vector<TripStop> Route;  // the stops the bot makes anyway, in order; may be empty
    float YardsPerSecond = 0.0f;
    SlotRates Rates;
};

struct Refusal
{
    ErrandOffer Offer;
    char const* Reason = "";  // detour_not_worth, too_far, no_stop
    float CostCopper = 0.0f;  // the cheapest detour it would need (detour_not_worth)
    float WalkMinutes = 0.0f;
};

struct TripPlan
{
    std::vector<TripStop> Stops;
    std::vector<Refusal> Refused;
};

inline bool StopTakes(TripStop const& stop, ErrandOffer const& offer)
{
    if (offer.Placed)
        return Yards(stop.Place, offer.Place) <= STOP_JOIN_YARDS;

    return offer.Service && (stop.Services & offer.Service) == offer.Service;
}

// A stop with a trainer within STOP_JOIN_YARDS takes the training of that trainer, as an errand place within that
// distance is done at the stop: the stop gets the trainer service and the trainer's entry (the bot walks the few yards
// on arrival). The first trainer in the order given wins; a stop that already has one, or is a trainer's own (of any
// trainer service), keeps it
inline void JoinTrainers(std::vector<ServiceStop>& stops, std::vector<ServiceStop> const& trainers, uint32_t service)
{
    for (ServiceStop& stop : stops)
    {
        if (stop.Trainer || (stop.Services & (service | TRAINER_SERVICES)))
            continue;

        for (ServiceStop const& trainer : trainers)
            if (Yards(stop.Place, trainer.Place) <= STOP_JOIN_YARDS)
            {
                stop.Services |= service;
                stop.Trainer = trainer.Id;
                break;
            }
    }
}

// The yards that passing `place` adds at the best point of the route, and that point. After the last stop the walk
// there counts once, standing for the way back (as TripWorthIt)
inline float InsertYards(ErrandPlace const& start, std::vector<TripStop> const& stops, ErrandPlace const& place,
                         std::size_t& index)
{
    index = stops.size();
    float best = Yards(stops.empty() ? start : stops.back().Place, place);
    for (std::size_t i = 0; i < stops.size(); ++i)
    {
        ErrandPlace const& from = i ? stops[i - 1].Place : start;
        float const extra = Yards(from, place) + Yards(place, stops[i].Place) - Yards(from, stops[i].Place);
        if (extra < best)
        {
            best = extra;
            index = i;
        }
    }
    return best;
}

// The stop of a trip that holds the riding errand on `service`, or the trip's size when none does
inline std::size_t RideStop(std::vector<TripStop> const& stops, uint32_t service)
{
    for (std::size_t i = 0; i < stops.size(); ++i)
        for (ErrandOffer const& errand : stops[i].Errands)
            if (errand.Kind == ErrandKind::Ride && errand.Service == service)
                return i;
    return stops.size();
}

// A rank and its mount on one trip (WU26c): the vendor sells the mount only to a bot that knows the rank, so the riding
// trainer's stop comes before the mount vendor's when the detour order put it after
inline void RankBeforeMount(std::vector<TripStop>& stops)
{
    std::size_t const trainer = RideStop(stops, SERVICE_RIDING_TRAINER);
    std::size_t const vendor = RideStop(stops, SERVICE_MOUNT_VENDOR);
    if (trainer < stops.size() && vendor < trainer)
        std::rotate(stops.begin() + static_cast<std::ptrdiff_t>(vendor),
                    stops.begin() + static_cast<std::ptrdiff_t>(trainer),
                    stops.begin() + static_cast<std::ptrdiff_t>(trainer) + 1);
}

// The detour rule: an errand joins a stop already on the trip when one can take it; a new stop opens only when the
// pending errands it takes are worth more than its detour, the best gain first, until no detour pays or the trip has
// MAX_DETOUR_STOPS detours. Deterministic: candidates in the order given, strict comparisons.
inline TripPlan PlanTrip(TripRequest const& request, std::vector<ServiceStop> const& known,
                         std::vector<ErrandOffer> const& offers)
{
    TripPlan plan;
    plan.Stops = request.Route;
    for (TripStop& stop : plan.Stops)
        stop.OnRoute = true;

    std::vector<bool> done(offers.size(), false);
    auto const join = [&plan, &offers, &done]()
    {
        for (std::size_t i = 0; i < offers.size(); ++i)
            for (TripStop& stop : plan.Stops)
                if (!done[i] && StopTakes(stop, offers[i]))
                {
                    stop.Errands.push_back(offers[i]);
                    done[i] = true;
                }
    };
    // The candidate places: the known service stops, then each errand's own place
    std::vector<TripStop> candidates;
    for (ServiceStop const& service : known)
        candidates.push_back({service.Place, service.Services, service.Id, service.Trainer, false, 0.0f, {}});
    for (ErrandOffer const& offer : offers)
        if (offer.Placed)
            candidates.push_back({offer.Place, offer.Service, offer.Target, 0, false, 0.0f, {}});

    join();
    for (std::size_t detours = 0; detours < MAX_DETOUR_STOPS; ++detours)
    {
        bool found = false;
        float bestGain = 0.0f;
        std::size_t bestCandidate = 0;
        std::size_t bestIndex = 0;
        float bestMinutes = 0.0f;
        for (std::size_t c = 0; c < candidates.size(); ++c)
        {
            float worth = 0.0f;
            bool takes = false;
            for (std::size_t i = 0; i < offers.size(); ++i)
                if (!done[i] && StopTakes(candidates[c], offers[i]))
                {
                    worth += offers[i].WorthCopper;
                    takes = true;
                }
            if (!takes)
                continue;

            std::size_t index = 0;
            float const minutes =
                WalkMinutes(InsertYards(request.Start, plan.Stops, candidates[c].Place, index), request.YardsPerSecond);
            if (!(minutes <= ERRAND_MAX_WALK_MINUTES))
                continue;

            float const cost = DetourCost(request.Rates, minutes);
            if (!(worth > cost) || (found && !(worth - cost > bestGain)))
                continue;

            found = true;
            bestGain = worth - cost;
            bestCandidate = c;
            bestIndex = index;
            bestMinutes = minutes;
        }
        if (!found)
            break;

        TripStop stop = candidates[bestCandidate];
        stop.DetourMinutes = bestMinutes;
        plan.Stops.insert(plan.Stops.begin() + static_cast<std::ptrdiff_t>(bestIndex), std::move(stop));
        join();
    }

    // What is left: no place takes it, every place is beyond the walk limit, or no detour pays
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        if (done[i])
            continue;

        Refusal refusal;
        refusal.Offer = offers[i];
        refusal.Reason = "no_stop";
        bool reachable = false;
        for (TripStop const& candidate : candidates)
        {
            if (!StopTakes(candidate, offers[i]))
                continue;

            std::size_t index = 0;
            float const minutes =
                WalkMinutes(InsertYards(request.Start, plan.Stops, candidate.Place, index), request.YardsPerSecond);
            if (!(minutes <= ERRAND_MAX_WALK_MINUTES))
            {
                if (!reachable)
                    refusal.Reason = "too_far";
                continue;
            }
            float const cost = DetourCost(request.Rates, minutes);
            if (!reachable || cost < refusal.CostCopper)
            {
                refusal.CostCopper = cost;
                refusal.WalkMinutes = minutes;
            }
            reachable = true;
            refusal.Reason = "detour_not_worth";
        }
        plan.Refused.push_back(refusal);
    }
    RankBeforeMount(plan.Stops);
    return plan;
}
}  // namespace WorldTravel

#endif
