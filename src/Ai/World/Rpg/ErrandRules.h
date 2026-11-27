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

#include "AuctionBuyList.h"
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
    BankDeposit,   // kept items into the bank
    Tame,          // a hunter without a pet tames a beast at its spawn (upkeep WU21b)
    Stable,        // a hunter stores an unwanted pet at a stable master, then tames a wanted one (upkeep WU21c)
    Profession,    // a profession, its next rank or a recipe at a profession trainer (professions plan)
    Craft,         // a craft at a station: an anvil, a forge or a fire (professions plan)
    Auction        // listing and buying at an auctioneer (auction plan)
};
constexpr uint8_t ERRAND_KIND_COUNT = 18;
static_assert(static_cast<uint8_t>(ErrandKind::Auction) + 1 == ERRAND_KIND_COUNT, "a count for every errand kind");

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
        case ErrandKind::Tame:
            return "tame";
        case ErrandKind::Stable:
            return "stable";
        case ErrandKind::Profession:
            return "profession";
        case ErrandKind::Craft:
            return "craft";
        case ErrandKind::Auction:
            return "auction";
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
constexpr uint32_t SERVICE_WEAPON_MASTER = 1u << 10;  // teaches the weapon skill the bot learns (WU15a)
// A stop has one trainer to visit: a stop with any of these keeps its own
constexpr uint32_t SERVICE_PROFESSION_TRAINER = 1u << 12;  // teaches the bot's professions (professions plan)
constexpr uint32_t TRAINER_SERVICES =
    SERVICE_TRAINER | SERVICE_RIDING_TRAINER | SERVICE_WEAPON_MASTER | SERVICE_PROFESSION_TRAINER;
// A vendor that sells the bot a stock kind: the kind's bit (Planner::StockBit, 16 bits) from this bit on
constexpr uint32_t SERVICE_STOCK_SHIFT = 16;

constexpr uint32_t StockServices(uint16_t stockKinds)
{
    return static_cast<uint32_t>(stockKinds) << SERVICE_STOCK_SHIFT;
}
constexpr uint32_t SERVICE_MAILBOX = 1u << 8;
constexpr uint32_t SERVICE_BANKER = 1u << 9;
constexpr uint32_t SERVICE_STABLE_MASTER = 1u << 11;  // keeps a hunter's pets (WU21c)
constexpr uint32_t SERVICE_STATION = 1u << 13;        // a crafting station the bot's craft needs (professions plan)
constexpr uint32_t SERVICE_AUCTIONEER = 1u << 14;     // an auctioneer: the one house every auctioneer serves (WA.9)
// Every service has a bit of its own: two services merged onto one bit fail here
constexpr uint32_t SERVICE_BITS[] = {
    SERVICE_QUEST_ENDER,        SERVICE_VENDOR,  SERVICE_REPAIR,         SERVICE_INNKEEPER,
    SERVICE_FLIGHT_MASTER,      SERVICE_TRAINER, SERVICE_RIDING_TRAINER, SERVICE_MOUNT_VENDOR,
    SERVICE_WEAPON_MASTER,      SERVICE_MAILBOX, SERVICE_BANKER,         SERVICE_STABLE_MASTER,
    SERVICE_PROFESSION_TRAINER, SERVICE_STATION, SERVICE_AUCTIONEER};
constexpr bool ServiceBitsDistinct()
{
    uint32_t seen = 0;
    for (uint32_t const bit : SERVICE_BITS)
    {
        if ((seen & bit) || bit >= (1u << SERVICE_STOCK_SHIFT))
            return false;
        seen |= bit;
    }
    return true;
}
static_assert(ServiceBitsDistinct(), "two errand services share a bit");
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

// The profession learn or rank the bot buys (UpkeepNeeds' profession need, WR4b); Spell 0 = none. Due once the funded
// gold pays the price; else the trainer joins a trip that passes it
struct ProfessionAt
{
    uint32_t Spell = 0;
    bool Due = false;
};

// The skill path's craft queue (professions WR14): its first craft the bags allow now, the station it needs and the
// queue's net worth in copper
struct CraftAt
{
    uint32_t Spell = 0;  // 0 = no craft
    uint32_t Focus = 0;  // the spell focus of its station; 0 = none (done at any stop)
    float WorthCopper = 0.0f;
    bool Carried = false;  // the bot lights the station itself (a cook's campfire, WR15): done at any stop too
};

// The pet need's beast (WU21b): the nearest tameable spawn of a wanted family; Beast 0 = no pet need
struct PetAt
{
    uint32_t Beast = 0;  // the creature entry
    ErrandPlace Place;
};

// The pet need's swap form (WU21c): the old pet into the stable, then the tame of the wanted beast; Beast 0 = none
struct StableAt
{
    uint32_t Beast = 0;  // the wanted beast the tame after the stable goes for
    ErrandPlace Place;   // its spawn
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
    float MinutesToNextStop = 0.0f;     // until the bot passes a vendor anyway
    uint64_t Reserve = 0;               // ShouldBuyBag's reserve
    AuctionRules::CopperWeight Weight;  // the need factor a bag's price is paid at (AH.5); {} unknown: the reserve's
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
    bool RepairDue = false;   // UpkeepNeeds::RepairErrandDue, and a repairer within the walk limit
    uint16_t DueStock = 0;    // the stock kinds due whose lot the gold pays
    uint16_t OtherStock = 0;  // the stock kinds below their target whose lot the gold pays
    TrainingAt Training;
    RidingAt Riding;          // one rank per trip, the lowest
    WeaponAt Weapon;          // one weapon skill per trip
    ProfessionAt Profession;  // one profession learn or rank per trip
    CraftAt Craft;            // one station per trip
    PetAt Pet;
    StableAt Stable;
    // The auction plan (WA3b fills the stock, WA4b the wanted lines)
    uint32_t AuctionStockWorth = 0;   // copper listing the bag stock fetches above a vendor's payout (CarriedKeeps)
    uint32_t AuctionWantedWorth = 0;  // copper the wanted lines of its buy list are worth to it
    // Needs and errands NR12b: copper listing the banked sale stock fetches above a vendor's payout
    // (BankKnowledge::stockGain): the auction offer counts it, and a banker before the auction stop withdraws it
    uint32_t BankStockWorth = 0;
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
inline uint32_t VendorServices(ErrandSnapshot const& snapshot, uint32_t vendorEntry, bool repairs, uint16_t stockKinds)
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
    for (uint32_t bit = 1; bit <= UINT16_MAX; bit <<= 1)
    {
        bool const due = (snapshot.DueStock & bit) != 0;
        if (!due && !(snapshot.OtherStock & bit))
            continue;

        ErrandOffer offer;
        offer.Kind = ErrandKind::Shop;
        offer.Service = SERVICE_VENDOR | StockServices(static_cast<uint16_t>(bit));
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

// A profession learned or ranked (§3.3, WR4b) at a trainer of it: due once the funded gold pays the price, else it
// joins a trip that passes one
inline void ProvideProfession(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Profession.Spell)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Profession;
    offer.Service = SERVICE_PROFESSION_TRAINER;
    offer.WorthCopper = snapshot.Profession.Due ? DueWorth(snapshot.Rates) : JOIN_WORTH;
    offer.Id = snapshot.Profession.Spell;
    offer.Due = snapshot.Profession.Due;
    offers.push_back(offer);
}

// The skill path's crafts (§3.10, WR14): at a stop with the station, worth the queue's net worth, so a queue worth
// more than the walk opens a trip of its own; a craft without a station joins any stop of a trip and opens none. A
// station the bot lights itself (WR15) needs no station stop: any stop takes the craft, and its worth may open a trip
inline void ProvideCraft(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Craft.Spell)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Craft;
    offer.Service = snapshot.Craft.Focus && !snapshot.Craft.Carried ? SERVICE_STATION : 0;
    float const worth = std::isfinite(snapshot.Craft.WorthCopper) ? std::max(snapshot.Craft.WorthCopper, 0.0f) : 0.0f;
    offer.WorthCopper = snapshot.Craft.Focus ? worth : 0.0f;  // worth 0 pays no detour: it only joins
    offer.Id = snapshot.Craft.Spell;
    offers.push_back(offer);
}

// A hunter without a pet (§3.11, WU21b): a trip to the beast's spawn, due at once; no service, the place is the beast's
inline void ProvideTame(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Pet.Beast)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Tame;
    offer.Placed = true;
    offer.Place = snapshot.Pet.Place;
    offer.WorthCopper = DueWorth(snapshot.Rates);
    offer.Id = snapshot.Pet.Beast;
    offer.Target = snapshot.Pet.Beast;
    offer.Due = true;
    offers.push_back(offer);
}

// A hunter with an unwanted pet (§3.11, WU21c): a trip to a stable master, due once the slot is paid (the need's
// rule); the stop's visit stores the pet and puts the tame of the wanted beast after it on the trip. The offer is not
// placed (any stable master takes it); its Place is the beast's spawn, which PlanTrip holds to the tame's walk limit
inline void ProvideStable(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Stable.Beast)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Stable;
    offer.Service = SERVICE_STABLE_MASTER;
    offer.Place = snapshot.Stable.Place;
    offer.WorthCopper = DueWorth(snapshot.Rates);
    offer.Id = snapshot.Stable.Beast;
    offer.Target = snapshot.Stable.Beast;
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
        in.Worth = AuctionRules::BagCeiling(
            BagWorth(snapshot.Rates, snapshot.FreeSlots, bag.Slots, bag.ReplacedSlots, snapshot.BagHorizonHours), 0,
            snapshot.Weight);
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

// The withdrawal of the banked sale stock (NR12b): worth the stock's gain over the vendor, which PlanTrip counts only
// with an auction stop after it, so the stock is never fetched without its auction nor its auction drawn without it
constexpr uint32_t STOCK_WITHDRAW_ID = 1;

// Due items waiting in the bank, weighted by orderliness; and the banked sale stock's withdrawal for an auction
inline void ProvideBankWithdraw(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.Governed)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::BankWithdraw;
    offer.Service = SERVICE_BANKER;
    if (snapshot.BankDueCount)
    {
        offer.WorthCopper =
            static_cast<float>(snapshot.BankDueWorth) * BankMail::OrderlinessWeight(snapshot.Orderliness);
        offers.push_back(offer);
    }
    if (snapshot.BankStockWorth)
    {
        offer.Id = STOCK_WITHDRAW_ID;
        offer.WorthCopper = static_cast<float>(snapshot.BankStockWorth);
        offers.push_back(offer);
    }
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

// Auction stock in the bags (WA3b) and the buy list's wanted lines (WA4b): worth what listing the stock fetches above
// a vendor's payout plus what the wanted lines save under their ceilings at their typical prices, against the detour
// like any errand (selling and buying share one stop); an auctioneer beside a stop the bot makes anyway joins it. It
// comes before the deposit, which would bank the stock, and the mailbox, where a sale's money arrives later. Banked
// sale stock (NR12b) makes the offer too, worth nothing of its own: its gain is the withdrawal's (PlanTrip)
inline void ProvideAuction(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    uint64_t const worth = static_cast<uint64_t>(snapshot.AuctionStockWorth) + snapshot.AuctionWantedWorth;
    if (!snapshot.Governed || (!worth && !snapshot.BankStockWorth))
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Auction;
    offer.Service = SERVICE_AUCTIONEER;
    offer.WorthCopper = static_cast<float>(worth);
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

// NR12b: an auction errand is still ahead of a running trip from stop `from` on (that stop's own while its flag
// stands): the bank visit there finds the sale stock due for it. A stop's flag drops once its errand ran, so an
// auction stop before the bank's makes nothing due
template <typename Stop>
inline bool AuctionAhead(std::vector<Stop> const& stops, std::size_t from)
{
    for (std::size_t s = from; s < stops.size(); ++s)
        if (stops[s].auction)
            return true;
    return false;
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
        {ErrandKind::Profession, "profession", &ProvideProfession},
        {ErrandKind::Tame, "tame", &ProvideTame},
        {ErrandKind::Stable, "stable", &ProvideStable},
    };
    return rows;
}

// The trip's providers with the bank and mail rows. A stop does its errands in the order the offers come: a banker's
// withdrawal first (it may bring gear or sellable items), then selling and the rest, the auction stock's listing,
// then the deposit and the mailbox
inline std::vector<ProviderRow> const& TripProviders()
{
    static std::vector<ProviderRow> const rows = []
    {
        std::vector<ProviderRow> all = {{ErrandKind::BankWithdraw, "bank_withdraw", &ProvideBankWithdraw}};
        all.insert(all.end(), TravelProviders().begin(), TravelProviders().end());
        all.push_back({ErrandKind::Craft, "craft", &ProvideCraft});  // before the deposit: its products may go too
        all.push_back({ErrandKind::Auction, "auction", &ProvideAuction});  // before the deposit banks the stock
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

// NR3 (NR.4): the errands that start a fight: the tame (it clears the site and fights the beast through the channel)
// and the stable's swap (it tames the wanted pet after storing the old one)
constexpr bool ErrandStartsFight(ErrandKind kind) { return kind == ErrandKind::Tame || kind == ErrandKind::Stable; }

// A sick bot's trip (NR.4): no errand that starts a fight, no service stop and no placed errand outside the safe area
// the bot stands in. `safeAt(place)` answers whether the walk from the bot to the place stays in that area
// (Upkeep::SafeLeg on foot); the healthy bot's trip never calls this
template <typename SafeAt>
inline void KeepSickErrands(std::vector<ServiceStop>& stops, std::vector<ErrandOffer>& offers, SafeAt const& safeAt)
{
    stops.erase(
        std::remove_if(stops.begin(), stops.end(), [&safeAt](ServiceStop const& stop) { return !safeAt(stop.Place); }),
        stops.end());
    offers.erase(std::remove_if(offers.begin(), offers.end(), [&safeAt](ErrandOffer const& offer)
                                { return ErrandStartsFight(offer.Kind) || (offer.Placed && !safeAt(offer.Place)); }),
                 offers.end());
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
    char const* Reason = "";  // detour_not_worth, too_far, no_stop; place_refused (KeepUnrefusedErrands)
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
    // A craft without a station (or with one the bot lights itself) is done wherever the trip stops
    if (!offer.Service)
        return offer.Kind == ErrandKind::Craft;

    return (stop.Services & offer.Service) == offer.Service;
}

// The word of an errand left out because the record of failed attempts refuses its place (KeepUnrefusedErrands): its
// own (R4), neither the entry's cause nor no_stop
constexpr char const* PLACE_REFUSED = "place_refused";

// NR9 (NR8: trips planned to a place where a trip had just died three times): no stop and no placed errand at a place
// the record of failed attempts refuses (`refusedAt(place)`: a live entry there, FailedAttempts::IsPlaceRefused), so
// the planner takes the next stop that serves; an errand that only refused stops took is left out too. What it leaves
// out comes back as refusals with the word PLACE_REFUSED. The ordered stop never passes through it (an order names its
// vendor); refusedAt is asked once per stop and placed errand
template <typename RefusedAt>
inline std::vector<Refusal> KeepUnrefusedErrands(std::vector<ServiceStop>& stops, std::vector<ErrandOffer>& offers,
                                                 RefusedAt const& refusedAt)
{
    std::vector<ServiceStop> keptStops;
    std::vector<TripStop> kept;  // the places left: the kept stops and the kept placed errands' own places
    std::vector<TripStop> refused;
    for (ServiceStop const& stop : stops)
    {
        TripStop const place{stop.Place, stop.Services, stop.Id, stop.Trainer, false, 0.0f, {}};
        if (refusedAt(stop.Place))
            refused.push_back(place);
        else
        {
            kept.push_back(place);
            keptStops.push_back(stop);
        }
    }
    stops = std::move(keptStops);
    std::vector<bool> placeRefused(offers.size(), false);
    for (std::size_t i = 0; i < offers.size(); ++i)
        if (offers[i].Placed)
        {
            placeRefused[i] = refusedAt(offers[i].Place);
            if (!placeRefused[i])
                kept.push_back({offers[i].Place, offers[i].Service, offers[i].Target, 0, false, 0.0f, {}});
        }
    auto const takenAt = [](std::vector<TripStop> const& places, ErrandOffer const& offer)
    {
        return std::any_of(places.begin(), places.end(),
                           [&offer](TripStop const& place) { return StopTakes(place, offer); });
    };
    std::vector<Refusal> out;
    std::vector<ErrandOffer> keptOffers;
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        ErrandOffer const& offer = offers[i];
        bool const left = offer.Placed ? placeRefused[i] : takenAt(refused, offer) && !takenAt(kept, offer);
        if (left)
            out.push_back({offer, PLACE_REFUSED, 0.0f, 0.0f});
        else
            keptOffers.push_back(offer);
    }
    offers = std::move(keptOffers);
    return out;
}

// NR9: the vendor part of a stop passed without a sale, by what stopped it: its vendor found neither near the stop nor
// by its spawn (no_vendor), or the vendor found gone or dead before the bot stood at it (vendor_gone); nullptr while
// it serves. The word of the `decision errand vendor` row (the skip row's no_vendor is the trip start's: no vendor in
// reach at all)
inline char const* VendorPassedWord(bool found, bool serving)
{
    return !found ? "no_vendor" : !serving ? "vendor_gone" : nullptr;
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

// The tame that follows a pet swap at the stable (WU21c, WUL11): the beast joins the rest of the trip (the stops after
// `here`) at the point where it adds the fewest yards, the index into `rest`; next (0) when nothing is left or no
// point is on the beast's map
inline std::size_t TameInsertAt(ErrandPlace const& here, std::vector<TripStop> const& rest, ErrandPlace const& beast)
{
    std::size_t index = 0;
    float const yards = InsertYards(here, rest, beast, index);
    return std::isfinite(yards) ? index : 0;
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
    // The swap's beast (WU21d): the stable stores the pet only for a beast the tame offer would reach, the same walk
    // limit from the start as the tame's own trip; else the swap is refused too_far and the hunter keeps its pet
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        if (offers[i].Kind != ErrandKind::Stable)
            continue;

        float const minutes = WalkMinutes(Yards(request.Start, offers[i].Place), request.YardsPerSecond);
        if (minutes <= ERRAND_MAX_WALK_MINUTES)
            continue;

        done[i] = true;
        plan.Refused.push_back({offers[i], "too_far", 0.0f, minutes});
    }
    // NR12b: the banked sale stock is one errand of two stops, its withdrawal at a banker and its listing at an
    // auctioneer after it (or at the same stop, the withdrawal first). The withdrawal carries the stock's gain, the
    // auction offer only what the bags' stock and the wanted lines are worth; the gain counts only for a stop that
    // completes the pair, against its own detour and that of a banker stop it still needs, each new stop a detour as
    // for any errand. A bank stop past the auction would fetch the stock for nothing
    std::size_t stock = offers.size();
    std::size_t auction = offers.size();
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        if (offers[i].Kind == ErrandKind::BankWithdraw && offers[i].Id == STOCK_WITHDRAW_ID)
            stock = i;
        else if (offers[i].Kind == ErrandKind::Auction)
            auction = i;
    }
    auto const join = [&plan, &offers, &done, stock]()
    {
        for (std::size_t i = 0; i < offers.size(); ++i)
            for (TripStop& stop : plan.Stops)
                if (!done[i] && i != stock && StopTakes(stop, offers[i]))
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

    // A riding rank and its mount are one errand (WUL11): while the rank is pending, its stop counts both worths and
    // the mount alone counts nothing (the vendor sells it only to a bot that knows the rank); once the rank is on the
    // trip, the mount vendor's cheapest stop rides along without using a detour
    std::size_t rank = offers.size();
    std::size_t mount = offers.size();
    for (std::size_t i = 0; i < offers.size(); ++i)
        if (offers[i].Kind == ErrandKind::Ride)
            (offers[i].Service == SERVICE_RIDING_TRAINER ? rank : mount) = i;
    auto const rideAlong = [&plan, &offers, &done, &candidates, &request, &join, rank, mount]()
    {
        if (rank == offers.size() || !done[rank] || mount == offers.size() || done[mount])
            return;

        bool found = false;
        std::size_t bestCandidate = 0;
        std::size_t bestIndex = 0;
        float bestMinutes = 0.0f;
        for (std::size_t c = 0; c < candidates.size(); ++c)
        {
            if (!StopTakes(candidates[c], offers[mount]))
                continue;

            std::size_t index = 0;
            float const minutes =
                WalkMinutes(InsertYards(request.Start, plan.Stops, candidates[c].Place, index), request.YardsPerSecond);
            if (!(minutes <= ERRAND_MAX_WALK_MINUTES) || (found && !(minutes < bestMinutes)))
                continue;

            found = true;
            bestCandidate = c;
            bestIndex = index;
            bestMinutes = minutes;
        }
        if (!found)
            return;

        TripStop stop = candidates[bestCandidate];
        stop.DetourMinutes = bestMinutes;
        plan.Stops.insert(plan.Stops.begin() + static_cast<std::ptrdiff_t>(bestIndex), std::move(stop));
        join();
    };

    // The first stop of `stops` that holds the auction errand, or that takes it while it is pending; the size if none
    auto const auctionAt = [&offers, &done, auction](std::vector<TripStop> const& stops)
    {
        if (auction == offers.size())
            return stops.size();

        std::size_t at = 0;
        auto const holds = [](ErrandOffer const& errand) { return errand.Kind == ErrandKind::Auction; };
        while (at < stops.size() &&
               !(done[auction] ? std::any_of(stops[at].Errands.begin(), stops[at].Errands.end(), holds)
                               : StopTakes(stops[at], offers[auction])))
            ++at;
        return at;
    };
    // The banker in front of stop `before` of `stops`: of the candidates [first, last) with one, the place and index
    // that add the fewest minutes; false when none takes the withdrawal. The walk limit is the caller's. A banker that
    // takes the pending auction too is no second stop: the auction would join it and leave its own stop empty; it is
    // weighed as a stop of its own
    struct StockInsert
    {
        std::size_t Candidate = 0;
        std::size_t Index = 0;
        float Minutes = 0.0f;
    };
    auto const inFront = [&candidates, &offers, &done, &request, stock, auction](std::vector<TripStop> const& stops,
                                                                                 std::size_t before, std::size_t first,
                                                                                 std::size_t last, StockInsert& best)
    {
        bool found = false;
        for (std::size_t c = first; c < last; ++c)
        {
            if (!StopTakes(candidates[c], offers[stock]) ||
                (auction < offers.size() && !done[auction] && StopTakes(candidates[c], offers[auction])))
                continue;

            ErrandPlace const& place = candidates[c].Place;
            for (std::size_t s = 0; s <= before; ++s)
            {
                ErrandPlace const& from = s ? stops[s - 1].Place : request.Start;
                float const minutes =
                    WalkMinutes(Yards(from, place) + Yards(place, stops[s].Place) - Yards(from, stops[s].Place),
                                request.YardsPerSecond);
                if (!std::isfinite(minutes) || (found && !(minutes < best.Minutes)))
                    continue;

                found = true;
                best = {c, s, minutes};
            }
        }
        return found;
    };
    // Candidate `c`, inserted at `index` with `minutes` of its own and taking the pending auction, completes the pair
    // when a banker comes before it: a stop of the trip (no further minutes) or a stop of its own in front (Banker).
    // Each new stop is a detour of its own, as for any errand
    struct StockPair
    {
        bool Found = false;
        bool Banker = false;
        StockInsert Insert;
        float Cost = 0.0f;
    };
    auto const pairWith = [&plan, &candidates, &offers, &request, &auctionAt, &inFront, stock](
                              std::size_t c, std::size_t index, float minutes)
    {
        StockPair pair;
        std::vector<TripStop> stops = plan.Stops;
        stops.insert(stops.begin() + static_cast<std::ptrdiff_t>(index), candidates[c]);
        std::size_t const at = auctionAt(stops);
        if (at == stops.size())
            return pair;

        pair.Cost = DetourCost(request.Rates, minutes);
        for (std::size_t s = 0; s <= at; ++s)
            if (StopTakes(stops[s], offers[stock]))
            {
                pair.Found = true;
                return pair;
            }
        if (!inFront(stops, at, 0, candidates.size(), pair.Insert))
            return pair;

        pair.Found = true;
        pair.Banker = true;
        pair.Cost += DetourCost(request.Rates, pair.Insert.Minutes);
        return pair;
    };
    // The pair on stops the trip makes anyway: the withdrawal goes first at the first stop with a banker up to the
    // auction's
    auto const stockJoin = [&plan, &offers, &done, &auctionAt, stock]()
    {
        if (stock == offers.size() || done[stock])
            return;

        std::size_t const at = auctionAt(plan.Stops);
        if (at == plan.Stops.size())
            return;

        for (std::size_t s = 0; s <= at; ++s)
            if (StopTakes(plan.Stops[s], offers[stock]))
            {
                plan.Stops[s].Errands.insert(plan.Stops[s].Errands.begin(), offers[stock]);
                done[stock] = true;
                return;
            }
    };

    join();
    rideAlong();
    stockJoin();
    std::size_t detours = 0;
    while (detours < MAX_DETOUR_STOPS)
    {
        bool found = false;
        float bestGain = 0.0f;
        std::size_t bestCandidate = 0;
        std::size_t bestIndex = 0;
        float bestMinutes = 0.0f;
        StockPair bestPair;
        auto const consider =
            [&](std::size_t c, std::size_t index, float minutes, float worth, float cost, StockPair const& pair)
        {
            if (!(worth > cost) || (found && !(worth - cost > bestGain)))
                return;

            found = true;
            bestGain = worth - cost;
            bestCandidate = c;
            bestIndex = index;
            bestMinutes = minutes;
            bestPair = pair;
        };
        bool const rankPending = rank < offers.size() && !done[rank];
        bool const stockPending = stock < offers.size() && !done[stock];
        std::size_t const auctionStop = auctionAt(plan.Stops);
        for (std::size_t c = 0; c < candidates.size(); ++c)
        {
            float worth = 0.0f;
            bool takes = false;
            for (std::size_t i = 0; i < offers.size(); ++i)
                if (!done[i] && i != stock && StopTakes(candidates[c], offers[i]))
                {
                    if (i == rank && mount < offers.size() && !done[mount])
                        worth += offers[mount].WorthCopper;
                    if (!(i == mount && rankPending))
                        worth += offers[i].WorthCopper;
                    takes = true;
                }

            std::size_t index = 0;
            float const minutes =
                WalkMinutes(InsertYards(request.Start, plan.Stops, candidates[c].Place, index), request.YardsPerSecond);
            bool const inReach = minutes <= ERRAND_MAX_WALK_MINUTES;
            if (takes && inReach)
                consider(c, index, minutes, worth, DetourCost(request.Rates, minutes), StockPair());
            if (!stockPending)
                continue;

            float const withStock = worth + offers[stock].WorthCopper;
            // The stop takes the pending auction: the banker before it comes too (a second stop when of its own)
            if (inReach && auction < offers.size() && !done[auction] && StopTakes(candidates[c], offers[auction]))
            {
                StockPair const pair = pairWith(c, index, minutes);
                if (pair.Found && (!pair.Banker ||
                                   (pair.Insert.Minutes <= ERRAND_MAX_WALK_MINUTES && detours + 2 <= MAX_DETOUR_STOPS)))
                    consider(c, index, minutes, withStock, pair.Cost, pair);
            }
            // A banker in front of the trip's auction stop
            StockInsert front;
            if (auctionStop < plan.Stops.size() && inFront(plan.Stops, auctionStop, c, c + 1, front) &&
                front.Minutes <= ERRAND_MAX_WALK_MINUTES)
                consider(c, front.Index, front.Minutes, withStock, DetourCost(request.Rates, front.Minutes),
                         StockPair());
        }
        if (!found)
            break;

        TripStop stop = candidates[bestCandidate];
        stop.DetourMinutes = bestMinutes;
        plan.Stops.insert(plan.Stops.begin() + static_cast<std::ptrdiff_t>(bestIndex), std::move(stop));
        ++detours;
        if (bestPair.Banker)
        {
            TripStop banker = candidates[bestPair.Insert.Candidate];
            banker.DetourMinutes = bestPair.Insert.Minutes;
            plan.Stops.insert(plan.Stops.begin() + static_cast<std::ptrdiff_t>(bestPair.Insert.Index),
                              std::move(banker));
            ++detours;
        }
        join();
        rideAlong();
        stockJoin();
    }

    // What is left: no place takes it, every place is beyond the walk limit, or no detour pays. Banked stock left
    // without its withdrawal leaves the trip whole: an auction errand that holds nothing else comes off its stop, and
    // the auction's refusal carries the stock's gain and the pair's cost (the withdrawal makes no row of its own)
    bool const stockLeft = stock < offers.size() && !done[stock] &&
                           (auction == offers.size() || !done[auction] || !(offers[auction].WorthCopper > 0.0f));
    if (stockLeft)
    {
        done[stock] = true;
        if (auction < offers.size() && done[auction])
        {
            for (TripStop& stop : plan.Stops)
                stop.Errands.erase(
                    std::remove_if(stop.Errands.begin(), stop.Errands.end(),
                                   [](ErrandOffer const& errand) { return errand.Kind == ErrandKind::Auction; }),
                    stop.Errands.end());
            done[auction] = false;
        }
    }
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        if (done[i])
            continue;

        Refusal refusal;
        refusal.Offer = offers[i];
        refusal.Reason = "no_stop";
        bool const paired = stockLeft && i == auction;
        if (paired)
            refusal.Offer.WorthCopper += offers[stock].WorthCopper;
        bool reachable = false;
        for (std::size_t c = 0; c < candidates.size(); ++c)
        {
            if (!StopTakes(candidates[c], offers[i]))
                continue;

            std::size_t index = 0;
            float minutes =
                WalkMinutes(InsertYards(request.Start, plan.Stops, candidates[c].Place, index), request.YardsPerSecond);
            float cost = DetourCost(request.Rates, minutes);
            // Banked stock alone: the pair's banker must be in reach too, and its detour adds to the cost
            StockPair const pair =
                paired && minutes <= ERRAND_MAX_WALK_MINUTES ? pairWith(c, index, minutes) : StockPair();
            if (paired && minutes <= ERRAND_MAX_WALK_MINUTES && !pair.Found)
                continue;
            if (!(minutes <= ERRAND_MAX_WALK_MINUTES) ||
                (pair.Banker && !(pair.Insert.Minutes <= ERRAND_MAX_WALK_MINUTES)))
            {
                if (!reachable)
                    refusal.Reason = "too_far";
                continue;
            }
            if (pair.Found)
            {
                cost = pair.Cost;
                minutes += pair.Banker ? pair.Insert.Minutes : 0.0f;
            }
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

// How an errand trip's end is recorded (the status core's errand clean-up, CS14)
enum class ErrandEndRow : uint8_t
{
    Run,    // errand_run: a stop was visited, the trip's minutes count for t
    Abort,  // errand_abort: no stop was visited
    Sick    // errand_sick: resurrection sickness ended the trip; its minutes do not count
};

// Every end stamps the errand check; only a healthy trip with a visited stop adds its minutes to t
inline ErrandEndRow ErrandEnd(SlotMeasure& measure, uint32_t nowMs, float minutes, bool visited, bool sick)
{
    StampErrandCheck(measure, nowMs);
    if (sick)
        return ErrandEndRow::Sick;
    if (!visited)
        return ErrandEndRow::Abort;
    AddTripMinutes(measure, minutes);
    return ErrandEndRow::Run;
}

constexpr char const* ErrandEndWord(ErrandEndRow row)
{
    switch (row)
    {
        case ErrandEndRow::Run:
            return "errand_run";
        case ErrandEndRow::Abort:
            return "errand_abort";
        case ErrandEndRow::Sick:
            return "errand_sick";
    }
    return "";
}
}  // namespace WorldTravel

#endif
