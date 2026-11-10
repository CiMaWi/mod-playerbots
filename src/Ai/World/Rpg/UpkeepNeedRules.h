/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEPNEEDRULES_H
#define _PLAYERBOT_UPKEEPNEEDRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "ItemWorthRules.h"
#include "PlannerPool.h"
#include "TraitRules.h"
#include "UpkeepIndex.h"
#include "UpkeepRules.h"

// The needs of a persistent bot (upkeep plan WU3, §3.3): one list of what it must or wants to pay for - repair,
// consumables, trainer spells and riding - read by errands, the gold reserve and the planner. A pure function of the
// planner snapshot, the upkeep index and a few rates; std only for tests/upkeep-needs-selftest.cpp. Computed at
// snapshot time or on a timer, never per tick. The rules only list needs: going without never stops a bot.
namespace UpkeepNeeds
{
using Planner::Snapshot;
using Planner::TrainerList;
using Planner::TrainerSpell;
using Planner::UpkeepIndex;
using Planner::UpkeepStock;
using Planner::VendorOffer;

// The tuning table [E]: defaults until measured numbers exist; TuningLines prints it
constexpr std::uint8_t REPAIR_DUE_PERCENT = 30;   // repair is due below this durability or with a broken item
constexpr float STOCK_TARGET_HOURS = 3.0f;        // a stock target is what the bot uses in this many hours
constexpr float STOCK_LOW_SHARE = 0.25f;          // a stock below this share of its target is due
constexpr float THRIFT_STOCK_SCALE = 0.5f;        // target x (1 + this x thrift): frugal 1.5x, spendthrift 0.5x
constexpr float THRIFT_RESERVE_SCALE = 0.5f;      // reserve x (1 + this x thrift)
constexpr float TRAINING_SPAN_LEVELS = 2.0f;      // training gets its own trip this many levels behind ...
constexpr float TRAINING_SPAN_THOROUGH = 1.0f;    // ... for a thorough bot (thoroughness +1)
constexpr float TRAINING_SPAN_CARELESS = 4.0f;    // ... for a careless one (-1); linear in between
constexpr std::uint32_t RIDING_REACH_LEVELS = 2;  // a riding rank joins the reserve this many levels before its level
constexpr float DEFAULT_WALK_MINUTES_PER_HOUR = 30.0f;  // the walk share until WU14 measures it
constexpr float MAX_LEVEL_HORIZON_HOURS = 100.0f;       // a rank pays off over this many hours at the level cap
// Play hours per level by level band (WorldTravel::LevelBand): a riding rank is never replaced (later ranks build on
// it), so it pays off over the rest of the levelling plus MAX_LEVEL_HORIZON_HOURS
constexpr std::array<float, WorldTravel::LEVEL_BAND_COUNT> HOURS_PER_LEVEL = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
constexpr float COLLECTOR_WANT_SHARE = 0.5f;    // a collector adds this share of its free gold to a rank's worth
constexpr float SPENDTHRIFT_WANT_SHARE = 0.5f;  // a spendthrift adds this share x -thrift
// Use per hour by class and level band, until a bot has measured its own
constexpr float FOOD_PER_HOUR = 4.0f;   // a meal after about every third fight; mages conjure theirs
constexpr float WATER_PER_HOUR = 4.0f;  // the mana classes but mages
constexpr std::array<float, WorldTravel::LEVEL_BAND_COUNT> AMMO_PER_HOUR = {200.0f, 400.0f, 600.0f, 600.0f,
                                                                            600.0f};  // hunters
constexpr float REAGENT_PER_HOUR = 1.0f;   // the classes the index lists for a reagent
constexpr float POISON_PER_HOUR = 2.0f;    // rogues: an application per weapon lasts an hour
constexpr float PET_FOOD_PER_HOUR = 2.0f;  // hunters whose pet's diet is known

// The game's riding speeds, times running speed
constexpr float RUN_SPEED = 1.0f;
constexpr std::uint8_t LEVEL_CAP = 80;
constexpr std::uint16_t SKILL_RIDING = 762;
constexpr std::uint32_t COPPER_PER_GOLD = 10000;
constexpr std::uint32_t COPPER_PER_SILVER = 100;

constexpr std::uint8_t CLASS_PALADIN = 2;
constexpr std::uint8_t CLASS_HUNTER = 3;
constexpr std::uint8_t CLASS_ROGUE = 4;
constexpr std::uint8_t CLASS_PRIEST = 5;
constexpr std::uint8_t CLASS_SHAMAN = 7;
constexpr std::uint8_t CLASS_MAGE = 8;
constexpr std::uint8_t CLASS_WARLOCK = 9;
constexpr std::uint8_t CLASS_DRUID = 11;

enum class RideKind : std::uint8_t
{
    Ground,
    Air,
    ColdWeather  // flying in Northrend
};

struct RidingRank
{
    std::uint32_t spellId;
    std::uint16_t skill;  // the riding skill it gives; mounts need it
    RideKind kind;
    float speed;  // times running speed
    char const* name;
};

constexpr std::array<RidingRank, 5> RIDING_RANKS = {{
    {33388, 75, RideKind::Ground, 1.6f, "apprentice riding"},
    {33391, 150, RideKind::Ground, 2.0f, "journeyman riding"},
    {34090, 225, RideKind::Air, 2.5f, "expert riding"},
    {34091, 300, RideKind::Air, 3.8f, "artisan riding"},
    {54197, 0, RideKind::ColdWeather, 0.0f, "cold weather flying"},
}};

// The fixed order in which short gold is spent (§3.3)
enum class NeedOrder : std::uint8_t
{
    Repair,
    Ammo,
    FoodWater,
    ReagentPoison,
    Training,
    Riding
};

// One thing the bot must or wants to pay for
struct Need
{
    NeedOrder order{NeedOrder::Repair};
    UpkeepKind kind{UpkeepKind::Repair};
    UpkeepStock stock{UpkeepStock::Count};  // consumables: which stock
    std::uint64_t copper{0};                // the price, the game's
    std::uint64_t funded{0};                // what the bot's gold pays now, spent in the fixed order
    bool due{false};                        // worth its own trip; else it joins a trip that passes
    std::uint32_t units{0};                 // items to buy, spells to learn
    std::uint32_t item{0};                  // the item or the rank's spell
    std::uint32_t vendor{0};                // a vendor entry that sells it (0 = any repairer)
    std::uint32_t trainer{0};               // a trainer key that teaches it
    std::uint32_t levelsBehind{0};          // training: levels since the oldest waiting spell
    char const* without{""};                // what going without costs the bot
};

// What a riding rank is worth to the bot now
struct RankValue
{
    RidingRank rank{};
    std::size_t position{TrainerList::NOT_FOUND};  // in the riding list
    std::uint64_t training{0};
    std::uint64_t mount{0};  // the cheapest mount of its skill payable in gold; 0 for cold weather flying
    std::uint32_t mountItem{0};
    std::uint32_t mountVendor{0};
    std::uint32_t currencyMounts{0};  // mounts of its skill that also cost currency: never the bot's price
    std::uint8_t reqLevel{0};
    bool eligible{false};      // the level and the riding skill allow it now
    float savedMinutes{0.0f};  // per hour of play
    float worth{0.0f};         // savedMinutes x e x horizon, copper
    float want{0.0f};          // collector and spendthrift, copper; decides a purchase, never the reserve
    bool reserved{false};      // the rank within reach whose worth alone pays for it: in the reserve
    bool bought{false};

    std::uint64_t Price() const { return training + mount; }
};

// The rates the rules read beside the snapshot; the caller passes WorldTravel's earning rate
struct NeedRates
{
    float earnCopperPerMinute{0.0f};  // e (WorldTravel::SlotRates)
    float ridingHorizonHours{0.0f};   // 0 = RidingHorizonHours(level)
    float walkMinutesPerHour{DEFAULT_WALK_MINUTES_PER_HOUR};
    float flightMinutesPerHour{0.0f};  // flying legs (WU9-WU10); 0 until then
    float northrendFlightMinutesPerHour{0.0f};
    std::array<float, Planner::UPKEEP_STOCK_COUNT> usePerHour{};  // a bot's measured use; 0 = the default
    std::uint32_t trainingReserveLevels{2};                       // UpkeepConfig::TrainingReserveLevels
};

struct NeedList
{
    std::vector<Need> needs;  // in the fixed order
    std::uint64_t money{0};
    std::uint64_t reserve{0};    // gold kept back for the needs due within the horizon, thrift-scaled
    std::uint64_t shortfall{0};  // reserve minus gold, never below 0: what the planner reads
    std::uint64_t reserveRepair{0};
    std::uint64_t reserveStock{0};
    std::uint64_t reserveTraining{0};  // this level's spells and the next trainingReserveLevels levels'
    std::uint64_t reserveRiding{0};    // the next rank, when within reach and worth it
    float reserveScale{1.0f};
    float trainingSpan{TRAINING_SPAN_LEVELS};
    float horizonHours{0.0f};
    std::vector<RankValue> ranks;  // the riding ranks the bot does not know yet
};

inline float TraitUnit(Snapshot const& snapshot, TraitRules::Trait trait)
{
    return snapshot.traits ? TraitRules::Unit(*snapshot.traits, trait) : 0.0f;
}

inline bool HasInterest(Snapshot const& snapshot, TraitRules::Interest interest)
{
    if (!snapshot.traits)
        return false;
    for (std::uint8_t const packed : snapshot.traits->Interests)
        if (packed == static_cast<std::uint8_t>(interest) + 1)
            return true;
    return false;
}

constexpr std::uint32_t RaceBit(std::uint8_t race) { return race ? 1u << (race - 1) : 0; }

inline std::uint32_t SkillValue(Snapshot const& snapshot, std::uint32_t skill)
{
    for (Planner::Skill const& entry : snapshot.skills)
        if (entry.id == skill)
            return entry.value;
    return 0;
}

// Riding and the professions: every snapshot carries them, so one it lacks is at 0
inline bool SnapshotCarries(std::uint32_t skill)
{
    return std::find(Planner::SNAPSHOT_SKILLS.begin(), Planner::SNAPSHOT_SKILLS.end(), skill) !=
           Planner::SNAPSHOT_SKILLS.end();
}

inline bool UsesWater(std::uint8_t cls)
{
    return cls == CLASS_PALADIN || cls == CLASS_HUNTER || cls == CLASS_PRIEST || cls == CLASS_SHAMAN ||
           cls == CLASS_WARLOCK || cls == CLASS_DRUID;
}

inline float DefaultUsePerHour(std::uint8_t cls, std::uint8_t level, UpkeepStock kind)
{
    switch (kind)
    {
        case UpkeepStock::Food:
            return cls == CLASS_MAGE ? 0.0f : FOOD_PER_HOUR;
        case UpkeepStock::Water:
            return UsesWater(cls) ? WATER_PER_HOUR : 0.0f;
        case UpkeepStock::Ammo:
            return cls == CLASS_HUNTER ? AMMO_PER_HOUR[WorldTravel::LevelBand(level)] : 0.0f;
        case UpkeepStock::Reagent:
            return REAGENT_PER_HOUR;
        case UpkeepStock::Poison:
            return cls == CLASS_ROGUE ? POISON_PER_HOUR : 0.0f;
        case UpkeepStock::PetFood:
            return cls == CLASS_HUNTER ? PET_FOOD_PER_HOUR : 0.0f;
        default:
            return 0.0f;
    }
}

// The hours a riding rank bought at this level pays off over: the rest of the levelling and the level cap's horizon
inline float RidingHorizonHours(std::uint8_t level)
{
    float hours = MAX_LEVEL_HORIZON_HOURS;
    for (std::uint32_t at = std::max<std::uint32_t>(level, 1); at < LEVEL_CAP; ++at)
        hours += HOURS_PER_LEVEL[WorldTravel::LevelBand(at)];
    return hours;
}

// Levels behind before training gets its own trip: 2 for a neutral bot, 1 for a thorough one, 4 for a careless one
inline float TrainingSpan(float thoroughness)
{
    if (thoroughness >= 0.0f)
        return TRAINING_SPAN_LEVELS + (TRAINING_SPAN_THOROUGH - TRAINING_SPAN_LEVELS) * thoroughness;
    return TRAINING_SPAN_LEVELS + (TRAINING_SPAN_LEVELS - TRAINING_SPAN_CARELESS) * thoroughness;
}

// The vendor offers a bot may buy for a stock kind: gold only, its level, class, race, skill; no reputation rows
inline bool OfferFits(VendorOffer const& offer, Snapshot const& snapshot, UpkeepStock kind)
{
    if (!offer.GoldOnly() || offer.requiredLevel > snapshot.level || offer.requiredFaction)
        return false;
    if (offer.requiredSkill && SkillValue(snapshot, offer.requiredSkill) < offer.requiredSkillRank)
        return false;
    if (offer.classMask && !(offer.classMask & UpkeepIndex::ClassBit(snapshot.cls)))
        return false;
    if (offer.raceMask && !(offer.raceMask & RaceBit(snapshot.race)))
        return false;
    return kind != UpkeepStock::PetFood || (offer.diet & snapshot.petDiet);
}

// The best item for the bot's level (the highest required level), the cheapest per unit among those
inline VendorOffer const* BestOffer(UpkeepIndex const& index, Snapshot const& snapshot, UpkeepStock kind)
{
    VendorOffer const* best = nullptr;
    for (VendorOffer const& offer : index.Offers(kind))
    {
        if (!OfferFits(offer, snapshot, kind))
            continue;
        if (!best || offer.requiredLevel > best->requiredLevel)
        {
            best = &offer;
            continue;
        }
        // Cheaper per unit: price / count below the best's, cross-multiplied
        if (offer.requiredLevel == best->requiredLevel &&
            static_cast<std::uint64_t>(offer.buyPrice) * best->buyCount <
                static_cast<std::uint64_t>(best->buyPrice) * offer.buyCount)
            best = &offer;
    }
    return best;
}

inline std::uint32_t Stock(Snapshot const& snapshot, UpkeepStock kind)
{
    if (kind != UpkeepStock::PetFood)
        return snapshot.StockCount(Planner::StockBit(kind));
    std::uint32_t total = 0;
    for (Planner::StockItem const& entry : snapshot.stock)
        if ((entry.kinds & Planner::StockBit(kind)) && (entry.diet & snapshot.petDiet))
            total += entry.count;
    return total;
}

inline std::uint32_t StockTarget(Snapshot const& snapshot, NeedRates const& rates, UpkeepStock kind)
{
    std::size_t const slot = static_cast<std::size_t>(kind);
    float const use =
        rates.usePerHour[slot] > 0.0f ? rates.usePerHour[slot] : DefaultUsePerHour(snapshot.cls, snapshot.level, kind);
    float const scale = 1.0f + THRIFT_STOCK_SCALE * TraitUnit(snapshot, TraitRules::Trait::Thrift);
    return static_cast<std::uint32_t>(std::ceil(use * STOCK_TARGET_HOURS * scale));
}

inline char const* WithoutWord(UpkeepStock kind)
{
    switch (kind)
    {
        case UpkeepStock::Ammo:
            return "fights in melee";
        case UpkeepStock::Food:
        case UpkeepStock::Water:
            return "rests without eating or drinking";
        case UpkeepStock::PetFood:
            return "the pet goes unhappy";
        default:
            return "the spell or poison is skipped";
    }
}

inline NeedOrder OrderOf(UpkeepStock kind)
{
    switch (kind)
    {
        case UpkeepStock::Ammo:
            return NeedOrder::Ammo;
        case UpkeepStock::Food:
        case UpkeepStock::Water:
        case UpkeepStock::PetFood:
            return NeedOrder::FoodWater;
        default:
            return NeedOrder::ReagentPoison;
    }
}

inline UpkeepKind KindOf(UpkeepStock kind)
{
    switch (kind)
    {
        case UpkeepStock::Ammo:
            return UpkeepKind::Ammo;
        case UpkeepStock::Reagent:
            return UpkeepKind::Reagent;
        case UpkeepStock::Poison:
            return UpkeepKind::Poison;
        default:
            return UpkeepKind::Food;  // food, water and pet food
    }
}

// A consumable the bot is below its target on; a pet's diet of 0 is unknown, never "eats nothing"
inline void AddStockNeed(NeedList& list, UpkeepIndex const& index, Snapshot const& snapshot, NeedRates const& rates,
                         UpkeepStock kind)
{
    if (kind == UpkeepStock::PetFood && !snapshot.petDiet)
        return;
    std::uint32_t const target = StockTarget(snapshot, rates, kind);
    std::uint32_t const stock = Stock(snapshot, kind);
    if (!target || stock >= target)
        return;
    VendorOffer const* offer = BestOffer(index, snapshot, kind);
    if (!offer)
        return;  // nothing the bot may buy: no need it could meet
    Need need;
    need.order = OrderOf(kind);
    need.kind = KindOf(kind);
    need.stock = kind;
    need.units = target - stock;
    need.copper = offer->PriceFor(need.units);
    need.due = static_cast<float>(stock) < static_cast<float>(target) * STOCK_LOW_SHARE;
    need.item = offer->item;
    need.vendor = offer->vendorEntry;
    need.without = WithoutWord(kind);
    list.needs.push_back(need);
}

// The snapshot's bit sets align to this index
inline bool BitsAlign(UpkeepIndex const& index, Snapshot const& snapshot)
{
    return snapshot.upkeepVersion && snapshot.upkeepVersion == index.version;
}

// A class spell's skill and ability requirements against what the bot knows. A skill the snapshot carries (riding,
// the professions) and lacks is at 0; any other skill counts as met. A required ability on the class list, or taught
// by a class list spell (Dreadsteed needs Felsteed 5784, taught by list spell 1710), is known or already counted, one
// on the riding list (class mounts need a riding rank) is a known rank, and the others (talents, racials, quest spells
// such as Summon Warhorse) are in the snapshot's off-list bit, filled with HasSpell
inline bool RequirementsMet(TrainerList const& list, TrainerList const* riding, std::size_t position,
                            Snapshot const& snapshot, std::vector<std::uint64_t> const& have)
{
    TrainerSpell const& spell = list.spells[position];
    if (spell.reqSkillLine && SnapshotCarries(spell.reqSkillLine) &&
        SkillValue(snapshot, spell.reqSkillLine) < spell.reqSkillRank)
        return false;
    bool offList = false;
    for (std::uint32_t const ability : spell.reqAbility)
    {
        if (!ability)
            continue;
        std::size_t const found = list.Find(ability);
        std::size_t const onClass = found != TrainerList::NOT_FOUND ? found : list.Teacher(ability);
        if (onClass != TrainerList::NOT_FOUND)
        {
            if (!Planner::HasSpellBit(have, onClass))
                return false;
            continue;
        }
        std::size_t const onRiding = riding ? riding->Find(ability) : TrainerList::NOT_FOUND;
        if (onRiding != TrainerList::NOT_FOUND)
        {
            if (!Planner::HasSpellBit(snapshot.knownRidingSpells, onRiding))
                return false;
            continue;
        }
        offList = true;
    }
    return !offList || Planner::HasSpellBit(snapshot.offListMetClassSpells, position);
}

// The class trainer spells the bot may learn up to this level: known bits plus the spells already counted, until no
// new spell joins (ranks chain through reqAbility)
inline std::vector<std::size_t> Learnable(TrainerList const& list, TrainerList const* riding, Snapshot const& snapshot,
                                          std::vector<std::uint64_t> const& known,
                                          std::vector<std::uint64_t> const& canLearn, std::uint32_t upToLevel)
{
    std::vector<std::uint64_t> have = known;
    have.resize(std::max(have.size(), Planner::SpellBits(list.spells.size()).size()));
    std::vector<std::size_t> result;
    for (bool grew = true; grew;)
    {
        grew = false;
        for (std::size_t position = 0; position < list.spells.size(); ++position)
        {
            TrainerSpell const& spell = list.spells[position];
            if (Planner::HasSpellBit(have, position) || !Planner::HasSpellBit(canLearn, position) ||
                spell.reqLevel > upToLevel || !RequirementsMet(list, riding, position, snapshot, have))
                continue;
            Planner::SetSpellBit(have, position);
            result.push_back(position);
            grew = true;
        }
    }
    return result;
}

inline std::uint32_t TrainerFor(UpkeepIndex const& index, Planner::TrainerKind kind, std::uint32_t requirement)
{
    for (Planner::UpkeepTrainer const& trainer : index.trainers)
        if (trainer.kind == kind && trainer.requirement == requirement)
            return trainer.key;
    return 0;
}

// Training: the spells of this level are a need, due when the oldest waits trainingSpan levels; the reserve holds
// them and the next trainingReserveLevels levels' spells
inline void AddTraining(NeedList& list, UpkeepIndex const& index, Snapshot const& snapshot, NeedRates const& rates)
{
    TrainerList const* trainer = index.ClassList(snapshot.cls);
    if (!trainer || !BitsAlign(index, snapshot))
        return;
    std::vector<std::size_t> const spells = Learnable(
        *trainer, index.RidingList(snapshot.race), snapshot, snapshot.knownClassSpells, snapshot.canLearnClassSpells,
        static_cast<std::uint32_t>(snapshot.level) + rates.trainingReserveLevels);
    Need need;
    need.order = NeedOrder::Training;
    need.kind = UpkeepKind::TrainerSpell;
    need.trainer = TrainerFor(index, Planner::TrainerKind::Class, snapshot.cls);
    need.without = "the spells wait";
    std::uint8_t oldest = snapshot.level;
    for (std::size_t const position : spells)
    {
        TrainerSpell const& spell = trainer->spells[position];
        list.reserveTraining += spell.moneyCost;
        if (spell.reqLevel > snapshot.level)
            continue;
        need.copper += spell.moneyCost;
        ++need.units;
        oldest = std::min(oldest, spell.reqLevel);
        if (!need.item)
            need.item = spell.spellId;
    }
    if (!need.units)
        return;
    need.levelsBehind = snapshot.level - oldest;
    need.due = static_cast<float>(need.levelsBehind) >= list.trainingSpan;
    list.needs.push_back(need);
}

// The cheapest mount of a riding skill the bot's race may ride, payable in gold; gold+currency mounts are counted
// apart and never priced as the bot's mount
inline void PriceMount(UpkeepIndex const& index, Snapshot const& snapshot, RankValue& value)
{
    if (value.rank.kind == RideKind::ColdWeather)
        return;
    for (VendorOffer const& offer : index.Offers(UpkeepStock::Mount))
    {
        if (offer.requiredSkillRank != value.rank.skill || offer.requiredFaction ||
            (offer.raceMask && !(offer.raceMask & RaceBit(snapshot.race))) ||
            (offer.classMask && !(offer.classMask & UpkeepIndex::ClassBit(snapshot.cls))))
            continue;
        if (!offer.GoldOnly())
        {
            ++value.currencyMounts;
            continue;
        }
        if (!value.mountItem || offer.buyPrice < value.mount)
        {
            value.mount = offer.buyPrice;
            value.mountItem = offer.item;
            value.mountVendor = offer.vendorEntry;
        }
    }
}

// The reserve's thrift scale applied to a sum of prices
inline std::uint64_t Scaled(NeedList const& list, std::uint64_t copper)
{
    return static_cast<std::uint64_t>(std::llround(static_cast<double>(copper) * list.reserveScale));
}

// Riding ranks by worth (WU.5): the travel minutes a rank saves per hour of play, times e, times the horizon; a
// collector or a spendthrift wants it on top. Bought (a need) when its price is at most worth plus want and the gold
// covers it beside the thrift-scaled reserve for nearer needs with this rank in it. The next rank within reach joins
// the reserve when its worth alone pays for it: the want grows with free gold, so it never moves the reserve (WU.4)
inline void AddRiding(NeedList& list, UpkeepIndex const& index, Snapshot const& snapshot, NeedRates const& rates,
                      std::uint64_t nearer)
{
    TrainerList const* riding = index.RidingList(snapshot.race);
    if (!riding || !BitsAlign(index, snapshot))
        return;
    float ground = RUN_SPEED;
    float air = 0.0f;
    for (RidingRank const& rank : RIDING_RANKS)
    {
        std::size_t const position = riding->Find(rank.spellId);
        if (position == TrainerList::NOT_FOUND || !Planner::HasSpellBit(snapshot.knownRidingSpells, position))
            continue;
        if (rank.kind == RideKind::Ground)
            ground = std::max(ground, rank.speed);
        else if (rank.kind == RideKind::Air)
            air = std::max(air, rank.speed);
    }
    std::uint64_t const kept = Scaled(list, nearer);
    std::uint64_t free = snapshot.money > kept ? snapshot.money - kept : 0;
    std::uint64_t spent = 0;  // the ranks bought here, unscaled
    float const thrift = TraitUnit(snapshot, TraitRules::Trait::Thrift);
    float wantShare = HasInterest(snapshot, TraitRules::Interest::Collecting) ? COLLECTOR_WANT_SHARE : 0.0f;
    if (thrift < 0.0f)
        wantShare += SPENDTHRIFT_WANT_SHARE * -thrift;
    float const e = rates.earnCopperPerMinute;
    float const horizon = list.horizonHours;
    std::array<bool, 3> nextSeen{};  // the lowest unknown rank of each kind is the one evaluated for buying
    bool reserved = false;
    for (RidingRank const& rank : RIDING_RANKS)
    {
        std::size_t const position = riding->Find(rank.spellId);
        if (position == TrainerList::NOT_FOUND || Planner::HasSpellBit(snapshot.knownRidingSpells, position) ||
            !Planner::HasSpellBit(snapshot.canLearnRidingSpells, position))
            continue;
        TrainerSpell const& spell = riding->spells[position];
        RankValue value;
        value.rank = rank;
        value.position = position;
        value.training = spell.moneyCost;
        value.reqLevel = spell.reqLevel;
        PriceMount(index, snapshot, value);
        bool const skillMet = !spell.reqSkillLine || SkillValue(snapshot, spell.reqSkillLine) >= spell.reqSkillRank;
        value.eligible = spell.reqLevel <= snapshot.level && skillMet;
        if (rank.kind == RideKind::Ground)
            value.savedMinutes = rates.walkMinutesPerHour * (1.0f - ground / rank.speed);
        else if (rank.kind == RideKind::Air)
            value.savedMinutes = rates.flightMinutesPerHour * (1.0f - std::max(ground, air) / rank.speed);
        else if (air > 0.0f)
            value.savedMinutes = rates.northrendFlightMinutesPerHour * (1.0f - ground / air);
        value.savedMinutes = std::max(value.savedMinutes, 0.0f);
        value.worth = value.savedMinutes * e * horizon;
        value.want = wantShare * static_cast<float>(free);
        bool const worthIt = static_cast<float>(value.Price()) <= value.worth;
        bool const wanted = static_cast<float>(value.Price()) <= value.worth + value.want;
        // The purchase leaves the scaled reserve with this rank in it: a frugal bot never buys and falls short at once
        bool const affordable = value.Price() <= free && Scaled(list, nearer + spent + value.Price()) <= snapshot.money;
        std::size_t const kind = static_cast<std::size_t>(rank.kind);
        bool const next = !nextSeen[kind];
        nextSeen[kind] = true;
        value.bought = next && value.eligible && wanted && affordable;
        if (value.bought)
        {
            free -= value.Price();  // two ranks bought at once share the budget
            spent += value.Price();
            Need need;
            need.order = NeedOrder::Riding;
            need.kind = UpkeepKind::Riding;
            need.copper = value.Price();
            need.due = true;
            need.units = 1;
            need.item = rank.spellId;
            need.vendor = value.mountVendor;
            need.trainer = TrainerFor(index, Planner::TrainerKind::Mount, riding->requirement);
            need.without = "walks the long legs";
            list.needs.push_back(need);
        }
        // Within reach: the lowest such rank, when its worth alone pays for it
        if (next && !reserved && worthIt && snapshot.level + RIDING_REACH_LEVELS >= spell.reqLevel && skillMet)
        {
            list.reserveRiding = value.Price();
            reserved = true;
            value.reserved = true;
        }
        list.ranks.push_back(value);
    }
}

// Short gold is spent in the fixed order: each need takes what is left (a bot repairs what it can, learns what it can)
inline void Fund(NeedList& list)
{
    std::stable_sort(list.needs.begin(), list.needs.end(),
                     [](Need const& a, Need const& b) { return a.order < b.order; });
    std::uint64_t left = list.money;
    for (Need& need : list.needs)
    {
        need.funded = std::min(need.copper, left);
        left -= need.funded;
    }
}

// The needs list, the reserve and the shortfall
inline NeedList ComputeNeeds(Snapshot const& snapshot, UpkeepIndex const& index, NeedRates const& rates)
{
    NeedList list;
    list.money = snapshot.money;
    list.trainingSpan = TrainingSpan(TraitUnit(snapshot, TraitRules::Trait::Thoroughness));
    list.horizonHours = rates.ridingHorizonHours > 0.0f ? rates.ridingHorizonHours : RidingHorizonHours(snapshot.level);
    list.reserveScale = std::max(0.0f, 1.0f + THRIFT_RESERVE_SCALE * TraitUnit(snapshot, TraitRules::Trait::Thrift));

    if (snapshot.repairCost)
    {
        Need need;
        need.order = NeedOrder::Repair;
        need.kind = UpkeepKind::Repair;
        need.copper = snapshot.repairCost;
        need.due = snapshot.brokenItems > 0 || snapshot.lowestDurabilityPercent < REPAIR_DUE_PERCENT;
        need.units = 1;
        need.without = "fights with worn or broken gear";
        list.needs.push_back(need);
        list.reserveRepair = snapshot.repairCost;
    }
    for (UpkeepStock const kind : {UpkeepStock::Ammo, UpkeepStock::Food, UpkeepStock::Water, UpkeepStock::PetFood,
                                   UpkeepStock::Reagent, UpkeepStock::Poison})
        AddStockNeed(list, index, snapshot, rates, kind);
    for (Need const& need : list.needs)
        if (need.order != NeedOrder::Repair)
            list.reserveStock += need.copper;
    AddTraining(list, index, snapshot, rates);

    std::uint64_t const nearer = list.reserveRepair + list.reserveStock + list.reserveTraining;
    AddRiding(list, index, snapshot, rates, nearer);
    list.reserve = Scaled(list, nearer + list.reserveRiding);
    list.shortfall = list.reserve > list.money ? list.reserve - list.money : 0;
    Fund(list);
    return list;
}

inline std::string Money(std::uint64_t copper)
{
    std::string text;
    if (copper >= COPPER_PER_GOLD)
        text += std::to_string(copper / COPPER_PER_GOLD) + "g ";
    if (copper >= COPPER_PER_SILVER)
        text += std::to_string(copper / COPPER_PER_SILVER % COPPER_PER_SILVER) + "s ";
    return text + std::to_string(copper % COPPER_PER_SILVER) + "c";
}

inline char const* OrderWord(NeedOrder order)
{
    constexpr std::array<char const*, 6> WORDS = {"repair", "ammo", "food and water", "reagents", "training", "riding"};
    return WORDS[static_cast<std::size_t>(order)];
}

inline std::string Describe(Need const& need)
{
    std::string text = OrderWord(need.order);
    if (need.stock != UpkeepStock::Count)
        text +=
            std::string(" (") + std::string(Planner::UPKEEP_STOCK_NAMES[static_cast<std::size_t>(need.stock)]) + ")";
    text += ": " + Money(need.copper) + ", funded " + Money(need.funded) + (need.due ? ", due" : ", when passing");
    if (need.units)
        text += ", " + std::to_string(need.units) + (need.order == NeedOrder::Training ? " spells" : " units");
    if (need.item)
        text += ", item/spell " + std::to_string(need.item);
    if (need.vendor)
        text += ", vendor " + std::to_string(need.vendor);
    if (need.trainer)
        text += ", trainer " + std::to_string(need.trainer);
    if (need.order == NeedOrder::Training)
        text += ", " + std::to_string(need.levelsBehind) + " levels behind";
    return text + "; without: " + need.without;
}

inline std::string Describe(RankValue const& value)
{
    return std::string(value.rank.name) + ": price " + Money(value.Price()) + " (training " + Money(value.training) +
           ", mount " + Money(value.mount) + (value.currencyMounts ? ", currency mounts skipped" : "") + "), level " +
           std::to_string(value.reqLevel) + (value.eligible ? "" : " (not yet)") + ", saves " +
           std::to_string(value.savedMinutes) + " min/h, worth " + Money(static_cast<std::uint64_t>(value.worth)) +
           ", want " + Money(static_cast<std::uint64_t>(value.want)) + (value.reserved ? ", reserved" : "") +
           (value.bought ? ": BUY" : ": wait");
}

inline std::vector<std::string> DescribeAll(NeedList const& list)
{
    std::vector<std::string> lines;
    for (Need const& need : list.needs)
        lines.push_back(Describe(need));
    for (RankValue const& value : list.ranks)
        lines.push_back(Describe(value));
    lines.push_back("gold " + Money(list.money) + ", reserve " + Money(list.reserve) + " (repair " +
                    Money(list.reserveRepair) + ", stock " + Money(list.reserveStock) + ", training " +
                    Money(list.reserveTraining) + ", riding " + Money(list.reserveRiding) + ", x" +
                    std::to_string(list.reserveScale) + "), shortfall " + Money(list.shortfall) + ", training span " +
                    std::to_string(list.trainingSpan) + " levels, riding horizon " + std::to_string(list.horizonHours) +
                    " h");
    return lines;
}

// The [E] defaults, for the harness and the record
inline std::vector<std::string> TuningLines()
{
    auto const f = [](float value) { return std::to_string(value); };
    return {
        "repair due below " + std::to_string(REPAIR_DUE_PERCENT) + "% durability or with a broken item",
        "stock target " + f(STOCK_TARGET_HOURS) + " h of use, due below " + f(STOCK_LOW_SHARE) +
            " of it; thrift scales targets by 1 + " + f(THRIFT_STOCK_SCALE) + " x thrift and the reserve by 1 + " +
            f(THRIFT_RESERVE_SCALE) + " x thrift",
        "use per hour: food " + f(FOOD_PER_HOUR) + ", water " + f(WATER_PER_HOUR) + ", ammo by band " +
            f(AMMO_PER_HOUR[0]) + "/" + f(AMMO_PER_HOUR[1]) + "/" + f(AMMO_PER_HOUR[2]) + "/" + f(AMMO_PER_HOUR[3]) +
            "/" + f(AMMO_PER_HOUR[4]) + ", reagents " + f(REAGENT_PER_HOUR) + ", poisons " + f(POISON_PER_HOUR) +
            ", pet food " + f(PET_FOOD_PER_HOUR),
        "training trip at " + f(TRAINING_SPAN_LEVELS) + " levels behind (thorough " + f(TRAINING_SPAN_THOROUGH) +
            ", careless " + f(TRAINING_SPAN_CARELESS) + ")",
        "riding: reach " + std::to_string(RIDING_REACH_LEVELS) + " levels, walk share " +
            f(DEFAULT_WALK_MINUTES_PER_HOUR) + " min/h until measured, hours per level by band " +
            f(HOURS_PER_LEVEL[0]) + "/" + f(HOURS_PER_LEVEL[1]) + "/" + f(HOURS_PER_LEVEL[2]) + "/" +
            f(HOURS_PER_LEVEL[3]) + "/" + f(HOURS_PER_LEVEL[4]) + " plus " + f(MAX_LEVEL_HORIZON_HOURS) +
            " h at the cap; want: collector " + f(COLLECTOR_WANT_SHARE) + ", spendthrift " + f(SPENDTHRIFT_WANT_SHARE) +
            " x -thrift of the free gold, for purchases only (worth alone decides the reserve)",
    };
}
}  // namespace UpkeepNeeds

#endif
