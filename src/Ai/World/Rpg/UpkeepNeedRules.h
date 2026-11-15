/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEPNEEDRULES_H
#define _PLAYERBOT_UPKEEPNEEDRULES_H

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <unordered_map>
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
using Planner::TameableSpawn;
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
    Riding,
    Pet  // a hunter's pet (WU21b) and its stable (WU21c)
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
    std::uint64_t cheapest{0};              // training: the cheapest waiting spell with no waiting prerequisite
    std::uint32_t lastSpell{0};             // training: the waiting spell of the highest level (its trainers teach)
    std::uint32_t target{0};                // consumables: the stock target
    std::uint32_t lotSize{1};               // consumables: the best offer's lot (buyCount) ...
    std::uint32_t lotPrice{0};              // ... and its price
    std::uint32_t keptGrade{0};             // ammo and poison: the grade the bot keeps (KeptGrade)
    std::uint32_t beast{0};                 // pet: the creature entry to tame, 0 = no spawn on the bot's map
    std::uint8_t beastFamily{0};
    std::uint8_t beastLevel{0};  // its template's highest level
    Planner::Place beastAt;      // the spawn
    bool swap{false};            // pet: the old pet goes into the stable first (WU21c); copper the slot to buy
    char const* without{""};     // what going without costs the bot
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
    bool known{false};         // the rank is known, its mount is missing: the price is the mount alone
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
    // The travel shares (FillTravel, the slot measure's TravelShare): the default walk share until measured
    float walkMinutesPerHour{DEFAULT_WALK_MINUTES_PER_HOUR};
    float flightMinutesPerHour{0.0f};  // legs where a flying mount flies (fly legs, walk legs in open flying areas)
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

// The vendor offers a bot may buy for a stock kind: gold only, its level, class, race, skill; no reputation rows;
// ammo of the ranged weapon's type (arrows for bows and crossbows, bullets for guns) and at least as good as the
// equipped ammo (the item usage sells worse ammo: ItemUsageValue::QueryItemUsageForAmmo)
inline bool OfferFits(VendorOffer const& offer, Snapshot const& snapshot, UpkeepStock kind)
{
    if (!offer.GoldOnly() || offer.requiredLevel > snapshot.level || offer.requiredFaction)
        return false;
    if (kind == UpkeepStock::Ammo &&
        (!snapshot.ammoSubClass || offer.subClass != snapshot.ammoSubClass || offer.damage < snapshot.ammoDamage))
        return false;
    if (offer.requiredSkill && SkillValue(snapshot, offer.requiredSkill) < offer.requiredSkillRank)
        return false;
    if (offer.classMask && !(offer.classMask & UpkeepIndex::ClassBit(snapshot.cls)))
        return false;
    if (offer.raceMask && !(offer.raceMask & RaceBit(snapshot.race)))
        return false;
    return kind != UpkeepStock::PetFood || (offer.diet & snapshot.petDiet);
}

// The snapshot's bit sets align to this index
inline bool BitsAlign(UpkeepIndex const& index, Snapshot const& snapshot)
{
    return snapshot.upkeepVersion && snapshot.upkeepVersion == index.version;
}

// A reagent of a class spell the bot knows and casts with it (the index's reagent spells against the snapshot's known
// and reagent-free bits). The generic sell keeps only the reagents of spells the bot has and pays them for
// (ItemUsageValue::IsItemNeededForUsefullSpell): one for a spell not learned yet, or one a glyph makes free, would be
// sold at the next visit (`skip would_sell` at the vendor)
inline bool ReagentKnown(UpkeepIndex const& index, Snapshot const& snapshot, std::uint32_t item)
{
    if (!BitsAlign(index, snapshot))
        return false;
    auto const [first, last] = index.ReagentSpells(snapshot.cls, item);
    for (auto it = first; it != last; ++it)
        if (Planner::HasSpellBit(snapshot.knownClassSpells, it->position) &&
            !Planner::HasSpellBit(snapshot.reagentFreeClassSpells, it->position))
            return true;
    return false;
}

// OfferFits, and a reagent only for a spell the bot knows
inline bool OfferUsable(UpkeepIndex const& index, VendorOffer const& offer, Snapshot const& snapshot, UpkeepStock kind)
{
    return OfferFits(offer, snapshot, kind) &&
           (kind != UpkeepStock::Reagent || ReagentKnown(index, snapshot, offer.item));
}

// The better offer for the bot's level: the higher required level, then the cheaper per unit
inline bool BetterOffer(VendorOffer const& offer, VendorOffer const* best)
{
    if (!best || offer.requiredLevel > best->requiredLevel)
        return true;
    // Cheaper per unit: price / count below the best's, cross-multiplied
    return offer.requiredLevel == best->requiredLevel &&
           static_cast<std::uint64_t>(offer.buyPrice) * best->buyCount <
               static_cast<std::uint64_t>(best->buyPrice) * offer.buyCount;
}

// The best item for the bot's level (BetterOffer) among the usable offers; at one vendor entry when vendorEntry is set
inline VendorOffer const* BestOffer(UpkeepIndex const& index, Snapshot const& snapshot, UpkeepStock kind,
                                    std::uint32_t vendorEntry = 0)
{
    VendorOffer const* best = nullptr;
    for (VendorOffer const& offer : index.Offers(kind))
        if ((!vendorEntry || offer.vendorEntry == vendorEntry) && BetterOffer(offer, best) &&
            OfferUsable(index, offer, snapshot, kind))
            best = &offer;
    return best;
}

// The grades the generic sell keeps by (ItemUsageValue): ammo by its damage, poison by its level (IsBestPoison keeps
// the highest item level of a family; a rogue poison's item level is its required level); 0 for the other kinds
inline bool Graded(UpkeepStock kind) { return kind == UpkeepStock::Ammo || kind == UpkeepStock::Poison; }

inline std::uint32_t OfferGrade(VendorOffer const& offer, UpkeepStock kind)
{
    return kind == UpkeepStock::Ammo ? offer.damage : kind == UpkeepStock::Poison ? offer.requiredLevel : 0;
}

inline std::uint32_t StockGrade(Planner::StockItem const& entry, UpkeepStock kind)
{
    return kind == UpkeepStock::Ammo ? entry.damage : kind == UpkeepStock::Poison ? entry.level : 0;
}

// The grade the bot keeps of ammo and poison: only items of at least this grade count as stock and are bought, as
// the generic sell sells ammo worse than the equipped ammo and every poison but the best of its family. Ammo: the
// equipped ammo's damage (OfferFits refuses worse offers, so equipped ammo better than every offer leaves no need),
// with none equipped the best offer's; poison: the best offer's level; 0 (every item) for the other kinds
inline std::uint32_t KeptGrade(Snapshot const& snapshot, VendorOffer const* best, UpkeepStock kind)
{
    if (kind == UpkeepStock::Ammo && snapshot.ammoDamage)
        return snapshot.ammoDamage;
    return best ? OfferGrade(*best, kind) : 0;
}

// Pet food counts what the pet eats, ammo only what the ranged weapon fires; nothing below minGrade (KeptGrade)
inline std::uint32_t Stock(Snapshot const& snapshot, UpkeepStock kind, std::uint32_t minGrade = 0)
{
    std::uint8_t const bit = Planner::StockBit(kind);
    std::uint32_t total = 0;
    for (Planner::StockItem const& entry : snapshot.stock)
        if ((entry.kinds & bit) && StockGrade(entry, kind) >= minGrade &&
            (kind != UpkeepStock::PetFood || (entry.diet & snapshot.petDiet)) &&
            (kind != UpkeepStock::Ammo || (snapshot.ammoSubClass && entry.subClass == snapshot.ammoSubClass)))
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

// A consumable the bot is below its target on; a pet's diet of 0 is unknown, never "eats nothing"; a hunter without
// a bow, crossbow or gun needs no ammo; a reagent only for a spell the bot knows; ammo and poison count only the
// grade the bot keeps
inline void AddStockNeed(NeedList& list, UpkeepIndex const& index, Snapshot const& snapshot, NeedRates const& rates,
                         UpkeepStock kind)
{
    if ((kind == UpkeepStock::PetFood && !snapshot.petDiet) || (kind == UpkeepStock::Ammo && !snapshot.ammoSubClass))
        return;
    std::uint32_t const target = StockTarget(snapshot, rates, kind);
    if (!target)
        return;
    // Ammo and poison: the best offer first, the kept grade decides what counts
    bool const graded = Graded(kind);
    VendorOffer const* offer = graded ? BestOffer(index, snapshot, kind) : nullptr;
    std::uint32_t const keptGrade = graded ? KeptGrade(snapshot, offer, kind) : 0;
    std::uint32_t const stock = Stock(snapshot, kind, keptGrade);
    if (stock >= target)
        return;
    if (!graded)
        offer = BestOffer(index, snapshot, kind);
    if (!offer)
        return;  // nothing the bot may buy (for a reagent: no spell it knows uses one): no need it could meet
    Need need;
    need.order = OrderOf(kind);
    need.kind = KindOf(kind);
    need.stock = kind;
    need.units = target - stock;
    need.copper = offer->PriceFor(need.units);
    need.due = static_cast<float>(stock) < static_cast<float>(target) * STOCK_LOW_SHARE;
    need.item = offer->item;
    need.vendor = offer->vendorEntry;  // trusted for stock needs only (a riding need's mount vendor ignores the race)
    need.target = target;
    need.lotSize = std::max<std::uint32_t>(offer->buyCount, 1);
    need.lotPrice = offer->buyPrice;
    need.keptGrade = keptGrade;
    need.without = WithoutWord(kind);
    list.needs.push_back(need);
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

// The trainer teaches the spell (its positions on its list)
inline bool Teaches(UpkeepIndex const& index, Planner::UpkeepTrainer const& trainer, std::uint32_t spellId)
{
    if (trainer.list >= index.lists.size())
        return false;
    std::size_t const position = index.lists[trainer.list].Find(spellId);
    return position != TrainerList::NOT_FOUND &&
           std::binary_search(trainer.spells.begin(), trainer.spells.end(), static_cast<std::uint16_t>(position));
}

// The first trainer of the kind and requirement; with a spell, the first that teaches it: each race's riding trainer
// is a trainer of its own, and only the flying trainers teach expert riding and Cold Weather Flying
inline std::uint32_t TrainerFor(UpkeepIndex const& index, Planner::TrainerKind kind, std::uint32_t requirement,
                                std::uint32_t spellId = 0)
{
    for (Planner::UpkeepTrainer const& trainer : index.trainers)
        if (trainer.kind == kind && trainer.requirement == requirement &&
            (!spellId || Teaches(index, trainer, spellId)))
            return trainer.key;
    return 0;
}

// A required ability of the spell is another waiting spell (on the list, or taught by one), so the core's trainer
// refuses the spell until that one is bought (TrainingPurchase skips it the same way)
inline bool NeedsWaitingSpell(TrainerList const& trainer, std::vector<std::size_t> const& spells,
                              TrainerSpell const& spell)
{
    for (std::uint32_t const ability : spell.reqAbility)
    {
        if (!ability)
            continue;
        std::size_t const found = trainer.Find(ability);
        std::size_t const onClass = found != TrainerList::NOT_FOUND ? found : trainer.Teacher(ability);
        if (onClass != TrainerList::NOT_FOUND && std::find(spells.begin(), spells.end(), onClass) != spells.end())
            return true;
    }
    return false;
}

// The class trainer that teaches the most of the spells waiting at this level, the first on a tie; none when no
// trainer teaches one. A starter trainer teaches only the first levels and a mage's portal trainer a teleport or two,
// so neither is the trip's trainer while the class trainer teaches more
inline Planner::UpkeepTrainer const* WaitingTeacher(UpkeepIndex const& index, TrainerList const& list,
                                                    std::vector<std::size_t> const& spells, Snapshot const& snapshot)
{
    Planner::UpkeepTrainer const* best = nullptr;
    std::size_t most = 0;
    for (Planner::UpkeepTrainer const& trainer : index.trainers)
    {
        if (trainer.kind != Planner::TrainerKind::Class || trainer.requirement != snapshot.cls)
            continue;
        std::size_t taught = 0;
        for (std::size_t const position : spells)
            if (list.spells[position].reqLevel <= snapshot.level &&
                Teaches(index, trainer, list.spells[position].spellId))
                ++taught;
        if (taught > most)
        {
            best = &trainer;
            most = taught;
        }
    }
    return best;
}

// Training: the spells of this level are a need, due when the oldest waits trainingSpan levels; the reserve holds
// them and the next trainingReserveLevels levels' spells. The need counts the spells its trainer (WaitingTeacher)
// teaches, one visit's worth: a spell only another trainer teaches (a mage's teleport at the portal trainer) is the
// next need once these are bought; as the highest waiting spell it sent the whole need to that trainer alone. The
// cheapest price is taken over the spells that need no other waiting spell first: only those can be bought at once
inline void AddTraining(NeedList& list, UpkeepIndex const& index, Snapshot const& snapshot, NeedRates const& rates)
{
    TrainerList const* trainer = index.ClassList(snapshot.cls);
    if (!trainer || !BitsAlign(index, snapshot))
        return;
    std::vector<std::size_t> const spells = Learnable(
        *trainer, index.RidingList(snapshot.race), snapshot, snapshot.knownClassSpells, snapshot.canLearnClassSpells,
        static_cast<std::uint32_t>(snapshot.level) + rates.trainingReserveLevels);
    for (std::size_t const position : spells)
        list.reserveTraining += trainer->spells[position].moneyCost;
    Planner::UpkeepTrainer const* const teacher = WaitingTeacher(index, *trainer, spells, snapshot);
    if (!teacher)
        return;
    Need need;
    need.order = NeedOrder::Training;
    need.kind = UpkeepKind::TrainerSpell;
    need.without = "the spells wait";
    std::uint8_t oldest = snapshot.level;
    std::uint8_t last = 0;
    for (std::size_t const position : spells)
    {
        TrainerSpell const& spell = trainer->spells[position];
        if (spell.reqLevel > snapshot.level || !Teaches(index, *teacher, spell.spellId))
            continue;
        need.copper += spell.moneyCost;
        if ((!need.cheapest || spell.moneyCost < need.cheapest) && !NeedsWaitingSpell(*trainer, spells, spell))
            need.cheapest = spell.moneyCost;
        ++need.units;
        oldest = std::min(oldest, spell.reqLevel);
        if (!need.item)
            need.item = spell.spellId;
        if (!need.lastSpell || spell.reqLevel > last)
        {
            need.lastSpell = spell.spellId;
            last = spell.reqLevel;
        }
    }
    if (!need.units)
        return;
    need.trainer = teacher->key;
    need.levelsBehind = snapshot.level - oldest;
    need.due = static_cast<float>(need.levelsBehind) >= list.trainingSpan;
    list.needs.push_back(need);
}

// Weapon skills (§3.7, WU15a): a weapon the bot wields or carries whose skill it lacks is a need when its class and
// race may learn the skill from a weapon master (a mage's mace is none) and its level allows the spell; one need per
// skill, training's order, due at once (a weapon it cannot use is no matter of levels), its price in the training
// reserve. Weapon skills rise by use, as the core does for any player
inline void AddWeaponSkills(NeedList& list, UpkeepIndex const& index, Snapshot const& snapshot)
{
    for (Planner::OwnedWeapon const& owned : snapshot.ownedWeaponSkills)
    {
        Planner::WeaponSkill const* weapon = owned.known ? nullptr : index.Weapon(owned.skill);
        if (!weapon || !weapon->Fits(snapshot.cls, snapshot.race) || weapon->reqLevel > snapshot.level)
            continue;
        Need need;
        need.order = NeedOrder::Training;
        need.kind = UpkeepKind::WeaponSkill;
        need.copper = weapon->moneyCost;
        need.cheapest = weapon->moneyCost;
        need.due = true;
        need.units = 1;
        need.item = weapon->spellId;
        need.trainer = weapon->trainers.front();
        need.without = "the weapon stays unused";
        list.reserveTraining += weapon->moneyCost;
        list.needs.push_back(need);
    }
}

// A mount of the riding skill the bot's race and class may ride, with no reputation asked
inline bool MountFits(VendorOffer const& offer, std::uint8_t race, std::uint8_t cls, std::uint16_t skill)
{
    return offer.requiredSkillRank == skill && !offer.requiredFaction &&
           (!offer.raceMask || (offer.raceMask & RaceBit(race))) &&
           (!offer.classMask || (offer.classMask & UpkeepIndex::ClassBit(cls)));
}

// The cheapest mount of a riding skill the bot's race may ride, payable in gold; gold+currency mounts are counted
// apart and never priced as the bot's mount
inline void PriceMount(UpkeepIndex const& index, Snapshot const& snapshot, RankValue& value)
{
    if (value.rank.kind == RideKind::ColdWeather)
        return;
    for (VendorOffer const& offer : index.Offers(UpkeepStock::Mount))
    {
        if (!MountFits(offer, snapshot.race, snapshot.cls, value.rank.skill))
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

// The mount a vendor sells for the rank at no more than its price, payable in gold; 0 when it sells none. Every race's
// mount vendor sells its own mounts at the one price, so the errand takes the nearest such vendor, not only the one
// PriceMount met first
inline std::uint32_t MountSoldBy(UpkeepIndex const& index, std::uint8_t race, std::uint8_t cls, RankValue const& value,
                                 std::uint32_t vendorEntry)
{
    if (!value.mountItem)
        return 0;
    for (VendorOffer const& offer : index.Offers(UpkeepStock::Mount))
        if (offer.vendorEntry == vendorEntry && offer.GoldOnly() && offer.buyPrice <= value.mount &&
            MountFits(offer, race, cls, value.rank.skill))
            return offer.item;
    return 0;
}

// The rank the need list bought, by its spell; nullptr when none
inline RankValue const* BoughtRank(NeedList const& list, std::uint32_t spellId)
{
    for (RankValue const& value : list.ranks)
        if (value.bought && value.rank.spellId == spellId)
            return &value;
    return nullptr;
}

// The reserve's thrift scale applied to a sum of prices
inline std::uint64_t Scaled(NeedList const& list, std::uint64_t copper)
{
    return static_cast<std::uint64_t>(std::llround(static_cast<double>(copper) * list.reserveScale));
}

// Riding ranks by worth (WU.5): the travel minutes a rank saves per hour of play, times e, times the horizon; a
// collector or a spendthrift wants it on top. Bought (a need) when its price is at most worth plus want and the gold
// covers it beside the thrift-scaled reserve for nearer needs with this rank in it. The next rank within reach joins
// the reserve when its worth alone pays for it: the want grows with free gold, so it never moves the reserve (WU.4).
// A rank is done when the bot has it and a mount of its skill or better (WU14): a known rank without its mount stays
// the need, priced at the mount alone, and carries nobody (a mount bought late is worth what the rank and mount were)
inline bool RankMounted(Snapshot const& snapshot, RidingRank const& rank)
{
    if (rank.kind == RideKind::Ground)
        return snapshot.groundMountSkill >= rank.skill;
    if (rank.kind == RideKind::Air)
        return snapshot.flyingMountSkill >= rank.skill;
    return true;  // cold weather flying needs no mount of its own
}

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
        if (position == TrainerList::NOT_FOUND || !Planner::HasSpellBit(snapshot.knownRidingSpells, position) ||
            !RankMounted(snapshot, rank))
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
        if (position == TrainerList::NOT_FOUND || !Planner::HasSpellBit(snapshot.canLearnRidingSpells, position))
            continue;
        bool const known = Planner::HasSpellBit(snapshot.knownRidingSpells, position);
        bool const mounted = RankMounted(snapshot, rank);
        if (known && mounted)
            continue;
        TrainerSpell const& spell = riding->spells[position];
        RankValue value;
        value.rank = rank;
        value.position = position;
        value.known = known;
        value.training = known ? 0 : spell.moneyCost;
        value.reqLevel = spell.reqLevel;
        if (!mounted)
            PriceMount(index, snapshot, value);
        // A known rank whose mount no vendor sells for gold leaves nothing to buy
        if (known && !value.mountItem)
            continue;
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
            need.trainer =
                known ? 0 : TrainerFor(index, Planner::TrainerKind::Mount, riding->requirement, rank.spellId);
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

// The repair need's due rule (§3.5): an equipped item is broken or the most worn one is below REPAIR_DUE_PERCENT
constexpr bool RepairDue(std::uint8_t brokenItems, std::uint8_t lowestDurabilityPercent)
{
    return brokenItems > 0 || lowestDurabilityPercent < REPAIR_DUE_PERCENT;
}

// The repair need makes an errand worth a trip of its own (WT3b's errand check, until WT15): it is due and the bot's
// gold pays at least one worn item. Repair is first in Fund's order, so it gets all the gold; Player::DurabilityRepair
// repairs an item only when the gold pays it whole, so gold below the cheapest worn item would buy a trip for nothing.
// After the visit every item left worn costs more than the gold left, so the rule does not send the bot again
constexpr bool RepairErrandDue(std::uint8_t brokenItems, std::uint8_t lowestDurabilityPercent,
                               std::uint64_t cheapestItemCopper, std::uint64_t money)
{
    return RepairDue(brokenItems, lowestDurabilityPercent) && cheapestItemCopper > 0 && money >= cheapestItemCopper;
}

// The training need makes an errand worth a trip of its own: it is due and its funded gold pays the cheapest waiting
// spell that needs no other waiting one (as RepairErrandDue; the core's trainer path sells a spell for its whole
// price, and refuses a rank before its prerequisite, so a cheap rank behind a dearer one does not count). After the
// visit every spell left that can be bought costs more than the gold left, so the rule does not send the bot again.
// A reputation discount only makes the spells cheaper, so the test never sends a bot that cannot pay
constexpr bool TrainingErrandDue(Need const& need)
{
    return need.due && need.cheapest > 0 && need.funded >= need.cheapest;
}

// The riding need makes an errand worth a trip of its own only when its funded share pays it whole (the rank and its
// mount, or the mount alone). The purchase is judged against the thrift-scaled reserve, Fund pays the unscaled needs
// before riding, so a spendthrift's rank can show BUY with a funded share below its price; the trainer and the vendor
// refuse that, the gold does not change, and the trip would repeat. The visit's reputation price is never above copper
constexpr bool RidingErrandDue(Need const& need)
{
    return need.kind == UpkeepKind::Riding && need.due && need.copper > 0 && need.funded >= need.copper;
}

// The pet need (§3.11, WU21b): a hunter of PET_MIN_LEVEL or above that knows Tame Beast and has no pet (a dismissed
// or dead one counts as a pet: the core's Tame Beast tames nothing then) wants the nearest spawn of a wanted family at
// or below its level on its map. Due at once, price 0 (taming costs nothing). A wanted family, for now (WU21b's brief,
// Unclear 1): any tameable one, an exotic one only with Beast Mastery's aura (the core refuses it otherwise); WU21c's
// wanted-family list by spec replaces WantedBeast. Without a spawn on the map the need stays, no trip (the bot plays
// on)
constexpr std::uint32_t SPELL_TAME_BEAST = 1515;
constexpr std::uint8_t PET_MIN_LEVEL = 10;    // Tame Beast and pets from 10 (the class quest)
constexpr std::uint8_t PET_CLASS_HUNTER = 3;  // the core's CLASS_HUNTER

constexpr bool WantedBeast(TameableSpawn const& spawn, std::uint8_t level, bool tamesExotic)
{
    return spawn.maxLevel <= level && (!spawn.exotic || tamesExotic);
}

// The families a hunter wants as its pet (§3.11, WU21c), by spec as the core marks it: Beast Mastery's talent (its
// aura, Player::CanTameExoticPets) opens the exotic families, and a Beast Mastery hunter wants one of them; a hunter of
// another spec wants any family it can tame, so it never swaps. A pet of an unknown family (0) is wanted. The list is
// data (the core's CreatureFamily numbers)
constexpr std::array<std::uint8_t, 7> BEAST_MASTERY_FAMILIES = {38 /*chimaera*/,
                                                                39 /*devilsaur*/,
                                                                41 /*silithid*/,
                                                                42 /*worm*/,
                                                                43 /*rhino*/,
                                                                45 /*core hound*/,
                                                                46 /*spirit beast*/};

constexpr bool WantedFamily(std::uint8_t family, bool tamesExotic)
{
    if (!tamesExotic || !family)
        return true;
    for (std::uint8_t const wanted : BEAST_MASTERY_FAMILIES)
        if (wanted == family)
            return true;
    return false;
}

// The nearest wanted spawn on the snapshot's map, or none; familyRule: only the families WantedFamily names (the swap)
inline TameableSpawn const* NearestBeast(UpkeepIndex const& index, Snapshot const& snapshot, bool familyRule = false)
{
    TameableSpawn const* nearest = nullptr;
    float best = std::numeric_limits<float>::max();
    auto const [first, last] = index.TameableOn(snapshot.position.map);
    for (auto it = first; it != last; ++it)
    {
        if (!WantedBeast(*it, snapshot.level, snapshot.tamesExotic) ||
            (familyRule && !WantedFamily(it->family, snapshot.tamesExotic)))
            continue;
        float const dx = it->x - snapshot.position.x;
        float const dy = it->y - snapshot.position.y;
        float const dz = it->z - snapshot.position.z;
        float const yards = dx * dx + dy * dy + dz * dz;
        if (yards < best)
        {
            best = yards;
            nearest = &*it;
        }
    }
    return nearest;
}

// The swap form (WU21c): a hunter whose pet is of no wanted family stores it at a stable master and tames a wanted
// beast. Only with a wanted spawn on the map (else the hunter keeps its pet: it never stands without one for a beast
// it cannot reach; a spawn beyond the tame's walk limit is refused too_far by the trip, WorldTravel::PlanTrip) and a
// slot for the old pet: an empty bought one (copper 0) or the next one to buy (copper its
// price, due only when the gold above the reserve pays it: PetSwapPayable, read once the reserve is known). With every
// slot full the hunter keeps its pet
inline void AddPet(NeedList& list, UpkeepIndex const& index, Snapshot const& snapshot)
{
    if (snapshot.cls != PET_CLASS_HUNTER || snapshot.level < PET_MIN_LEVEL || !snapshot.knowsTameBeast)
        return;

    bool const swap = snapshot.hasPet;
    if (swap &&
        (WantedFamily(snapshot.petFamily, snapshot.tamesExotic) || (!snapshot.stableFree && !snapshot.stableSlotPrice)))
        return;
    TameableSpawn const* beast = NearestBeast(index, snapshot, swap);
    if (swap && !beast)
        return;

    Need need;
    need.order = NeedOrder::Pet;
    need.kind = UpkeepKind::Pet;
    need.units = 1;
    need.item = SPELL_TAME_BEAST;
    need.swap = swap;
    need.copper = swap && !snapshot.stableFree ? snapshot.stableSlotPrice : 0;
    need.without = swap ? "keeps the wrong pet" : "no pet";
    if (beast)
    {
        need.due = true;
        need.beast = beast->entry;
        need.beastFamily = beast->family;
        need.beastLevel = beast->maxLevel;
        need.beastAt = {beast->map, beast->x, beast->y, beast->z};
    }
    list.needs.push_back(need);
}

// A slot is bought only from the gold above the reserve (WU.16 point 7); an empty bought slot costs nothing
constexpr bool PetSwapPayable(std::uint64_t money, std::uint64_t reserve, std::uint64_t price)
{
    return !price || (money > reserve && money - reserve >= price);
}

// At the stable master the pet must be out and alive: a trip arrives mounted, and the core brings the pet back after
// the dismount (a DB load); the visit waits this long for it, then gives the swap up (the hunter keeps its pet)
constexpr std::uint32_t STABLE_PET_WAIT_MS = 10 * 1000;

// The pet need makes a trip of its own when it names a spawn: the tame's (price 0) or the swap's that the gold above
// the reserve pays (ComputeNeeds clears due otherwise)
constexpr bool PetErrandDue(Need const& need) { return need.kind == UpkeepKind::Pet && need.due && need.beast; }

// The tame at the beast's spawn (§3.11, WU21b-2): the hunter casts Tame Beast from TAME_CAST_YARDS and channels while
// the beast fights it; below TAME_BREAK_OFF_HEALTH_PCT it breaks the channel off and fights the beast. A new try waits
// until the health is back to TAME_START_HEALTH_PCT, for TAME_REST_MS at most (a bot never stalls), and a stop casts
// TAME_MAX_TRIES times at most; then it is given up and the need plans a new trip at the errand retry. Tuning values,
// read from the nights
constexpr float TAME_CAST_YARDS = 20.0f;
constexpr float TAME_BREAK_OFF_HEALTH_PCT = 35.0f;
constexpr float TAME_START_HEALTH_PCT = 60.0f;
constexpr std::uint32_t TAME_REST_MS = 60 * 1000;
constexpr std::uint8_t TAME_MAX_TRIES = 3;
// A pick that gets no cast within this time (the walk to the beast, the dismount) gives the stop up: every wait ends
constexpr std::uint32_t TAME_REACH_MS = 60 * 1000;
// The reach bound's start (0 = not started): the first tick past the stop's rest starts it (the arrival, a channel's
// end, a clear's fight, a respawn wait reset it to 0), and a new pick of a beast keeps it (WU21e run 2: re-picks of
// wandering Frostwing Chimaeras restarted it, the chase on the Isle of Spears cliffs never ended)
constexpr std::uint32_t TameReachStartMs(std::uint32_t startMs, std::uint32_t nowMs)
{
    return startMs ? startMs : nowMs;
}
// The stop looks for a beast as far as a player sees one on a continent (Visibility.Distance.Continents' default): a
// hunter arrives up to 30 yards short of the spawn, and when another hunter or a fight took the beasts there, the next
// spawns of the entry stand 70-100 yards on (WU21b-4: Borean wolves 25675, the nearest left 92 yards from the bot)
constexpr float TAME_SEARCH_YARDS = 100.0f;

constexpr bool TameBreaksOff(float healthPct) { return healthPct < TAME_BREAK_OFF_HEALTH_PCT; }

enum class TameNext : std::uint8_t
{
    Cast,
    Rest,
    GiveUp
};

// The step at the stop before a cast: the casts made there, the bot's health and the ms since the bot arrived or its
// last channel ended
constexpr TameNext TameNextStep(std::uint8_t tries, float healthPct, std::uint32_t sinceMs)
{
    if (tries >= TAME_MAX_TRIES)
        return TameNext::GiveUp;
    if (healthPct < TAME_START_HEALTH_PCT && sinceMs < TAME_REST_MS)
        return TameNext::Rest;
    return TameNext::Cast;
}

// A channel the beast broke while its fight goes on (its stun or knockback: Swoop, Wing Buffet; damage only pushes a
// channel back) is cast again at once on the same beast, as a player tames: the ability is spent for a while (once
// per fight, or on a timer longer than the channel). Only with tries left and the start health, and within
// TAME_RECAST_MS of the channel's end (the stun, the fall of a knockback); else the bot fights the beast and the
// stop's rest and next try follow
constexpr std::uint32_t TAME_RECAST_MS = 6 * 1000;

constexpr bool TameRecastsInFight(std::uint8_t tries, float healthPct, std::uint32_t sinceEndMs)
{
    return tries < TAME_MAX_TRIES && healthPct >= TAME_START_HEALTH_PCT && sinceEndMs <= TAME_RECAST_MS;
}

// A beast whose own spells break the channel is no tame target (WU.4; WU21b-2 round 4: the Bonestripper Buzzard's
// Swoop stuns every 10 s, so the 20 s channel never ends): a spell effect that knocks back or interrupts a cast, or
// an aura that stuns, fears, confuses or silences. One rule over the beast's spells (its creature_template_spell
// spells and its SmartAI's casts, Planner::BreaksTameChannel), read where the index gathers the tameable spawns and
// where the stop searches for a beast. The core's numbers (SharedDefines.h SpellEffects, SpellAuraDefines.h AuraType;
// PlannerUpkeep.cpp asserts them)
constexpr std::uint32_t EFFECT_INTERRUPT_CAST = 68;
constexpr std::uint32_t EFFECT_KNOCK_BACK = 98;
constexpr std::uint32_t EFFECT_KNOCK_BACK_DEST = 144;
constexpr std::uint32_t AURA_MOD_CONFUSE = 5;
constexpr std::uint32_t AURA_MOD_FEAR = 7;
constexpr std::uint32_t AURA_MOD_STUN = 12;
constexpr std::uint32_t AURA_MOD_SILENCE = 27;

constexpr bool BreaksTameChannel(std::uint32_t effect, std::uint32_t aura)
{
    return effect == EFFECT_INTERRUPT_CAST || effect == EFFECT_KNOCK_BACK || effect == EFFECT_KNOCK_BACK_DEST ||
           aura == AURA_MOD_CONFUSE || aura == AURA_MOD_FEAR || aura == AURA_MOD_STUN || aura == AURA_MOD_SILENCE;
}

// A tame target has TAME_MIN_SPAWNS spawns at least (WU21b-2 round 5): a single named spawn (Ripp 19458, Aspatha
// 25498) is gone for its respawn once one hunter tames or kills it (its pack fights every bot that arrives), and the
// next hunter the need sends finds none at the stop; of an entry with several spawns the stop takes the next one
constexpr std::uint32_t TAME_MIN_SPAWNS = 2;

constexpr bool TameTargetSpawns(std::uint32_t spawns) { return spawns >= TAME_MIN_SPAWNS; }

// A patrolling spawn is no tame target (WU.23; WU21d: the Vilewing Chimaera's five spawns walk waypoint paths and the
// stop found none to cast at): the index counts and places only standing spawns, and the stop's search skips a
// patrolling beast. The core's MovementGeneratorType WAYPOINT_MOTION_TYPE (PlannerUpkeep.cpp asserts it)
constexpr std::uint8_t MOVEMENT_WAYPOINT = 2;

constexpr bool TameSpawnStands(std::uint8_t movementType) { return movementType != MOVEMENT_WAYPOINT; }

// The tame's site (WU.23): before the 20 s channel the hunter looks at the creatures that would join its fight. A
// creature is a threat when it is hostile and its aggro reach covers the hunter's spot, or when it answers the beast's
// call for help (the core's assistance: free, of the beast's faction, within the assistance radius of the beast,
// which calls again as it runs from its place to the hunter: the segment between them), or when it fights the hunter
// already (WU21e run 1: a cast amid a clear's fight lost its beast to that fight). TAME_SITE_MARGIN_YARDS covers
// where the hunter stands at the cast; TAME_SITE_YARDS is the search around it (the widest aggro reach and the margin)
constexpr float TAME_SITE_MARGIN_YARDS = 5.0f;
constexpr float TAME_SITE_YARDS = 50.0f;

struct TameSiteUnit
{
    float x{0.0f};
    float y{0.0f};
    float aggroYards{0.0f};  // its aggro reach against the hunter (Creature::GetAggroRange)
    bool hostile{false};     // it attacks the hunter on sight
    bool assists{false};     // it answers the beast's call for help (Creature::CanAssistTo)
    bool attacking{false};   // it fights the hunter now
};

// The distance in the plane from a point to the segment a-b
inline float SegmentYards(float px, float py, float ax, float ay, float bx, float by)
{
    float const dx = bx - ax;
    float const dy = by - ay;
    float const length2 = dx * dx + dy * dy;
    float const t = length2 > 0.0f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / length2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(ax + t * dx - px, ay + t * dy - py);
}

inline bool TameThreat(TameSiteUnit const& unit, float beastX, float beastY, float spotX, float spotY,
                       float assistYards)
{
    if (unit.attacking ||
        (unit.hostile && std::hypot(unit.x - spotX, unit.y - spotY) <= unit.aggroYards + TAME_SITE_MARGIN_YARDS))
        return true;
    return unit.assists &&
           SegmentYards(unit.x, unit.y, beastX, beastY, spotX, spotY) <= assistYards + TAME_SITE_MARGIN_YARDS;
}

// The stop's pick among the beasts it may tame (WU.23): the fewest threats first (an edge member of a pack, a beast
// away from hostiles: the hunter goes where nothing joins, and a second hunter at the pack takes another member), then
// one of the stop's entry (the planned spawn), then the nearest
struct TamePick
{
    std::uint32_t threats{0};
    bool otherEntry{false};
    float yards{0.0f};
};

constexpr bool TameBetter(TamePick const& a, TamePick const& b)
{
    if (a.threats != b.threats)
        return a.threats < b.threats;
    if (a.otherEntry != b.otherEntry)
        return !a.otherEntry;
    return a.yards < b.yards;
}

// At the cast the site is read again from the hunter's spot: with no threat it casts, else it fights the nearest
// threat first (an ordinary fight clears it) and looks again, TAME_MAX_CLEARS times at most; a site that stays busy
// gives the stop up (the need plans a new trip at the errand retry)
constexpr std::uint8_t TAME_MAX_CLEARS = 4;

enum class TameSite : std::uint8_t
{
    Cast,
    Clear,
    GiveUp
};

constexpr TameSite TameSiteNext(std::uint32_t threats, std::uint8_t clears)
{
    if (!threats)
        return TameSite::Cast;
    return clears < TAME_MAX_CLEARS ? TameSite::Clear : TameSite::GiveUp;
}

// No beast to tame at the stop (WU.23): the hunter waits for the soonest respawn of a standing spawn of the stop's
// entry near it, read from the spawn's own respawn time, when the wait fits its budget, counted from the stop's first
// wait (every wait ends): a hunter that wants one beast (the stable swap's family, an exotic) waits up to
// TAME_WAIT_WANTED_MS, one that needs any pet TAME_WAIT_ANY_MS (the world's common respawn of 5 min: WU21e run 1, a
// second hunter at the Hellfire worgs gave up and tamed one at its next trip 6 min later); else it takes another
// target at the need's next trip
constexpr std::uint32_t TAME_WAIT_ANY_MS = 5 * 60 * 1000;
constexpr std::uint32_t TAME_WAIT_WANTED_MS = 15 * 60 * 1000;
constexpr std::uint32_t TAME_RESPAWN_GRACE_MS = 5 * 1000;  // the beast shows after its grid's next update

// The ms to wait from now (0 = no wait): respawnInMs the soonest respawn, waitedMs since the stop's first wait
constexpr std::uint32_t TameWaitMs(std::uint32_t respawnInMs, std::uint32_t waitedMs, bool wanted)
{
    std::uint32_t const budget = wanted ? TAME_WAIT_WANTED_MS : TAME_WAIT_ANY_MS;
    if (respawnInMs > budget || waitedMs > budget)
        return 0;
    std::uint32_t const wait = respawnInMs + TAME_RESPAWN_GRACE_MS;
    return waitedMs + wait <= budget + TAME_RESPAWN_GRACE_MS ? wait : 0;
}

// A weapon skill makes a trip of its own once its funded gold pays its price (WU15a); a bot without the gold keeps
// playing, the need stays. The visit's reputation price is never above copper
constexpr bool WeaponErrandDue(Need const& need)
{
    return need.kind == UpkeepKind::WeaponSkill && need.due && need.copper > 0 && need.funded >= need.copper;
}

// The game's price of a trainer spell for a bot whose reputation gives this price factor (Trainer::TeachSpell:
// int32(MoneyCost * reputationDiscount))
constexpr std::uint32_t TrainerPrice(std::uint32_t moneyCost, float discount)
{
    return static_cast<std::uint32_t>(static_cast<float>(moneyCost) * discount);
}

// What a trainer visit learns: the positions, in the order they are taught, and the gold they cost
struct TrainingBuy
{
    std::vector<std::size_t> positions;
    std::uint64_t total{0};
};

// The spells a visit buys (§3.7): of the learnable class spells up to the bot's level, in level order (stable by list
// position), each one the gold left pays, at the game's price. A spell the gold does not pay is skipped and the cheaper
// later ones are still bought; it stays a need. A spell whose required ability on the list is learnable but was not
// bought is skipped too (the core's trainer refuses it)
inline TrainingBuy TrainingPurchase(TrainerList const& list, std::vector<std::size_t> const& learnable,
                                    std::uint32_t level, std::uint64_t money, float discount)
{
    std::vector<std::size_t> order;
    for (std::size_t const position : learnable)
        if (position < list.spells.size() && list.spells[position].reqLevel <= level)
            order.push_back(position);
    std::stable_sort(order.begin(), order.end(), [&list](std::size_t a, std::size_t b)
                     { return list.spells[a].reqLevel < list.spells[b].reqLevel; });

    TrainingBuy buy;
    std::vector<std::uint64_t> bought(Planner::SpellBits(list.spells.size()).size(), 0);
    for (std::size_t const position : order)
    {
        TrainerSpell const& spell = list.spells[position];
        std::uint64_t const price = TrainerPrice(spell.moneyCost, discount);
        if (price > money - buy.total)
            continue;

        bool chained = true;
        for (std::uint32_t const ability : spell.reqAbility)
        {
            if (!ability)
                continue;
            std::size_t const found = list.Find(ability);
            std::size_t const onClass = found != TrainerList::NOT_FOUND ? found : list.Teacher(ability);
            if (onClass != TrainerList::NOT_FOUND &&
                std::find(learnable.begin(), learnable.end(), onClass) != learnable.end() &&
                !Planner::HasSpellBit(bought, onClass))
                chained = false;
        }
        if (!chained)
            continue;

        Planner::SetSpellBit(bought, position);
        buy.positions.push_back(position);
        buy.total += price;
    }
    return buy;
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
        need.due = RepairDue(snapshot.brokenItems, snapshot.lowestDurabilityPercent);
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
    AddWeaponSkills(list, index, snapshot);
    AddPet(list, index, snapshot);

    std::uint64_t const nearer = list.reserveRepair + list.reserveStock + list.reserveTraining;
    AddRiding(list, index, snapshot, rates, nearer);
    list.reserve = Scaled(list, nearer + list.reserveRiding);
    list.shortfall = list.reserve > list.money ? list.reserve - list.money : 0;
    for (Need& need : list.needs)
        if (need.kind == UpkeepKind::Pet && need.swap)
            need.due = need.due && PetSwapPayable(list.money, list.reserve, need.copper);
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
    constexpr std::array<char const*, 7> WORDS = {"repair", "ammo", "food and water", "reagents", "training",
                                                  "riding", "pet"};
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
    if (need.kind == UpkeepKind::Pet)
        text += need.beast
                    ? ", beast " + std::to_string(need.beast) + " family " + std::to_string(need.beastFamily) +
                          " level " + std::to_string(need.beastLevel) + " at map " + std::to_string(need.beastAt.map) +
                          " " + std::to_string(need.beastAt.x) + " " + std::to_string(need.beastAt.y)
                    : ", no tameable beast on the map";
    if (need.swap)
        text += ", the old pet into the stable first (slot " + Money(need.copper) + ")";
    return text + "; without: " + need.without;
}

inline std::string Describe(RankValue const& value)
{
    return std::string(value.rank.name) + ": price " + Money(value.Price()) + " (training " + Money(value.training) +
           ", mount " + Money(value.mount) + (value.currencyMounts ? ", currency mounts skipped" : "") + "), level " +
           std::to_string(value.reqLevel) + (value.known ? " (known, mount missing)" : "") +
           (value.eligible ? "" : " (not yet)") + ", saves " + std::to_string(value.savedMinutes) + " min/h, worth " +
           Money(static_cast<std::uint64_t>(value.worth)) + ", want " + Money(static_cast<std::uint64_t>(value.want)) +
           (value.reserved ? ", reserved" : "") + (value.bought ? ": BUY" : ": wait");
}

// The stock errand (WU6, §3.6): a consumable stock below a quarter of its target makes an errand worth a trip of its
// own when the gold Fund gives it pays at least one lot; else the trip would buy nothing (as RepairErrandDue)
inline bool StockPayable(Need const& need)
{
    return need.stock != UpkeepStock::Count && need.lotPrice > 0 && need.funded >= need.lotPrice;
}

inline bool StockErrandDue(NeedList const& list)
{
    for (Need const& need : list.needs)
        if (need.due && StockPayable(need))
            return true;
    return false;
}

// A need that starts the errand check's trip planning (§3.3): a due stock the gold pays a lot of, due training the
// gold pays (TrainingErrandDue), a riding rank or a weapon skill the funded gold pays (RidingErrandDue,
// WeaponErrandDue), a pet to tame (PetErrandDue). The repair is the trigger's own test first (RepairErrandDue reads the
// cheapest item's price)
inline bool AnyErrandDue(NeedList const& list)
{
    for (Need const& need : list.needs)
    {
        bool const due = need.kind == UpkeepKind::TrainerSpell  ? TrainingErrandDue(need)
                         : need.kind == UpkeepKind::Riding      ? RidingErrandDue(need)
                         : need.kind == UpkeepKind::WeaponSkill ? WeaponErrandDue(need)
                         : need.kind == UpkeepKind::Pet         ? PetErrandDue(need)
                                                                : need.due && StockPayable(need);
        if (due)
            return true;
    }
    return false;
}

// The stock kinds (a bit per UpkeepStock) the gold pays a lot of: the due ones, or the others below their target
inline std::uint8_t PayableKinds(NeedList const& list, bool due)
{
    std::uint8_t kinds = 0;
    for (Need const& need : list.needs)
        if (StockPayable(need) && need.due == due)
            kinds |= Planner::StockBit(need.stock);
    return kinds;
}

// A vendor's best offer of the need's kind meets it: of the kept grade, and the funded share pays a lot of it
inline bool ShopPays(Need const& need, VendorOffer const& offer)
{
    return OfferGrade(offer, need.stock) >= need.keptGrade && need.funded >= offer.buyPrice;
}

// The stock kinds among `kinds` each vendor entry can sell the bot now: its best offer of the kind (the one
// PlanShopping buys there) meets the need (ShopPays), so a vendor chosen for a need never answers it with `budget`
// or `not_sold`
inline std::unordered_map<std::uint32_t, std::uint8_t> VendorCover(UpkeepIndex const& index, Snapshot const& snapshot,
                                                                   NeedList const& list, std::uint8_t kinds)
{
    std::unordered_map<std::uint32_t, std::uint8_t> cover;
    std::unordered_map<std::uint32_t, VendorOffer const*> best;
    for (Need const& need : list.needs)
    {
        if (need.stock == UpkeepStock::Count || need.stock == UpkeepStock::Mount ||
            !(kinds & Planner::StockBit(need.stock)))
            continue;
        best.clear();
        for (VendorOffer const& offer : index.Offers(need.stock))
        {
            VendorOffer const*& at = best[offer.vendorEntry];
            if (BetterOffer(offer, at) && OfferUsable(index, offer, snapshot, need.stock))
                at = &offer;
        }
        for (auto const& [entry, offer] : best)
            if (offer && ShopPays(need, *offer))
                cover[entry] |= Planner::StockBit(need.stock);
    }
    return cover;
}

// A vendor the errand may choose: one that trades with the bot, its walk and what it covers (VendorCover)
struct VendorCandidate
{
    std::uint32_t entry{0};
    float walkMinutes{0.0f};
    bool repairer{false};
    std::uint8_t covers{0};
};

constexpr std::size_t NO_VENDOR = std::numeric_limits<std::size_t>::max();

// The errand's vendor (§3.6): among the candidates within maxWalkMinutes (repairers only when the repair is wanted),
// the one covering the most due kinds, then the most other payable kinds, then the nearest. With no kinds and no walk
// limit it is the nearest vendor. NO_VENDOR when none qualifies
inline std::size_t PickVendor(std::vector<VendorCandidate> const& candidates, bool repairerOnly, std::uint8_t dueKinds,
                              std::uint8_t unmetKinds, float maxWalkMinutes)
{
    std::size_t best = NO_VENDOR;
    int bestDue = 0;
    int bestUnmet = 0;
    for (std::size_t at = 0; at < candidates.size(); ++at)
    {
        VendorCandidate const& candidate = candidates[at];
        if (candidate.walkMinutes > maxWalkMinutes || (repairerOnly && !candidate.repairer))
            continue;
        int const due = std::popcount(static_cast<unsigned>(candidate.covers & dueKinds));
        int const unmet = std::popcount(static_cast<unsigned>(candidate.covers & unmetKinds));
        if (best != NO_VENDOR)
        {
            bool const better = due != bestDue       ? due > bestDue
                                : unmet != bestUnmet ? unmet > bestUnmet
                                                     : candidate.walkMinutes < candidates[best].walkMinutes;
            if (!better)
                continue;
        }
        best = at;
        bestDue = due;
        bestUnmet = unmet;
    }
    return best;
}

enum class ShopOutcome : std::uint8_t
{
    Buy,
    Budget,  // the gold Fund left the need pays less than one lot
    NotSold  // the vendor has no offer of the kind the bot may buy
};

// One line of the shopping list at one vendor
struct ShopLine
{
    UpkeepStock kind{UpkeepStock::Count};
    ShopOutcome outcome{ShopOutcome::NotSold};
    bool due{false};
    std::uint32_t item{0};
    std::uint32_t lots{0};  // to buy: Player::BuyItemFromVendorSlot's count
    std::uint32_t lotSize{1};
    std::uint32_t lotPrice{0};
    std::uint32_t units{0};  // wanted: up to the target
    std::uint32_t target{0};
    std::uint64_t funded{0};
    std::uint64_t copper{0};  // lots x lotPrice, never above funded
    std::uint64_t wanted{0};  // the price up to the target: this vendor's, the best offer's when it sells none
};

constexpr std::uint32_t MAX_LOTS_PER_PURCHASE = 255;  // BuyItemFromVendorSlot's count is a uint8

// The shopping list at a vendor (§3.6, §3.3): each stock need in Fund's order, the vendor's best offer of its kind
// (BestOffer at this vendor: a lower level item it sells is fine, but ammo and poison of the kept grade only), whole
// lots up to the target and within the need's funded share. Training and riding are not bought here
inline std::vector<ShopLine> PlanShopping(NeedList const& list, UpkeepIndex const& index, Snapshot const& snapshot,
                                          std::uint32_t vendorEntry)
{
    std::vector<ShopLine> lines;
    for (Need const& need : list.needs)
    {
        if (need.stock == UpkeepStock::Count || need.stock == UpkeepStock::Mount)
            continue;
        ShopLine line;
        line.kind = need.stock;
        line.due = need.due;
        line.units = need.units;
        line.target = need.target;
        line.funded = need.funded;
        line.item = need.item;
        line.wanted = need.copper;
        VendorOffer const* offer = BestOffer(index, snapshot, need.stock, vendorEntry);
        if (!offer || OfferGrade(*offer, need.stock) < need.keptGrade)
        {
            lines.push_back(line);
            continue;
        }
        line.item = offer->item;
        line.lotSize = std::max<std::uint32_t>(offer->buyCount, 1);
        line.lotPrice = offer->buyPrice;
        line.wanted = offer->PriceFor(need.units);
        std::uint64_t lots = (need.units + line.lotSize - 1) / line.lotSize;
        if (line.lotPrice)
            lots = std::min<std::uint64_t>(lots, need.funded / line.lotPrice);
        line.lots = static_cast<std::uint32_t>(std::min<std::uint64_t>(lots, MAX_LOTS_PER_PURCHASE));
        line.copper = static_cast<std::uint64_t>(line.lots) * line.lotPrice;
        line.outcome = line.lots ? ShopOutcome::Buy : ShopOutcome::Budget;
        lines.push_back(line);
    }
    return lines;
}

// The world log's word for a stock kind (upkeep decision rows)
inline char const* ShopWord(UpkeepStock kind)
{
    constexpr std::array<char const*, Planner::UPKEEP_STOCK_COUNT> WORDS = {"food",   "water",    "ammo", "reagent",
                                                                            "poison", "pet_food", "mount"};
    return kind == UpkeepStock::Count ? "none" : WORDS[static_cast<std::size_t>(kind)];
}

inline char const* OutcomeWord(ShopOutcome outcome)
{
    switch (outcome)
    {
        case ShopOutcome::Buy:
            return "buy";
        case ShopOutcome::Budget:
            return "budget";
        default:
            return "not_sold";
    }
}

inline std::string Describe(ShopLine const& line)
{
    std::string text = std::string(ShopWord(line.kind)) + ": " + OutcomeWord(line.outcome) +
                       (line.due ? " (stock low)" : " (top up)") + ", item " + std::to_string(line.item);
    if (line.outcome != ShopOutcome::NotSold)
        text += ", " + std::to_string(line.lots) + " lots of " + std::to_string(line.lotSize) + " at " +
                Money(line.lotPrice) + " = " + Money(line.copper);
    return text + ", wants " + std::to_string(line.units) + " to target " + std::to_string(line.target) + ", funded " +
           Money(line.funded);
}

// The measured use (WU6): between two vendor visits nothing but loot and handouts adds stock, so a fall over the
// elapsed time is the bot's use; a rise gives no sample. Only the kinds the bot's class uses by default are measured
// (a mage's conjured food falls too, but a mage never buys food)
constexpr std::uint32_t USE_WINDOW_MS = 10 * 60 * 1000;  // a shorter span between visits gives no sample [E]
constexpr float USE_MS_PER_HOUR = 3600000.0f;
static_assert(WorldTravel::SlotMeasure::USE_KINDS == Planner::UPKEEP_STOCK_COUNT, "the slot measure's use slots");

// The stock per kind as the needs count it, and what it was counted by: a fall is use only when both ends share the
// basis (a pet out of sight reads no pet food, a gun no arrows, new equipped ammo none of the old)
struct StockLevels
{
    std::array<std::uint32_t, Planner::UPKEEP_STOCK_COUNT> count{};
    std::array<std::uint32_t, Planner::UPKEEP_STOCK_COUNT> basis{};  // 0 = unknown: no sample
};

// The pet's diet, the ammo type and kept grade (the equipped ammo's damage), the poison grade; 1 for the kinds counted
// the same way always. A damage x 1000 below 2^24 fits beside the subclass
inline std::uint32_t StockBasis(Snapshot const& snapshot, UpkeepStock kind, std::uint32_t keptGrade)
{
    switch (kind)
    {
        case UpkeepStock::PetFood:
            return snapshot.petDiet;
        case UpkeepStock::Ammo:
            return snapshot.ammoSubClass ? snapshot.ammoSubClass | keptGrade << 8 : 0;
        case UpkeepStock::Mount:
            return 0;
        default:
            return 1 | keptGrade << 8;  // keptGrade is 0 but for poison
    }
}

// Without an index poison counts every grade, and so does ammo with none equipped
inline StockLevels LevelsOf(UpkeepIndex const* index, Snapshot const& snapshot)
{
    StockLevels levels;
    for (std::size_t slot = 0; slot < Planner::UPKEEP_STOCK_COUNT; ++slot)
    {
        UpkeepStock const kind = static_cast<UpkeepStock>(slot);
        if (kind == UpkeepStock::Mount)
            continue;
        std::uint32_t const keptGrade =
            Graded(kind) ? KeptGrade(snapshot, index ? BestOffer(*index, snapshot, kind) : nullptr, kind) : 0;
        levels.count[slot] = Stock(snapshot, kind, keptGrade);
        levels.basis[slot] = StockBasis(snapshot, kind, keptGrade);
    }
    return levels;
}

inline bool UseMeasured(std::uint8_t cls, std::uint8_t level, UpkeepStock kind)
{
    return kind != UpkeepStock::Mount && DefaultUsePerHour(cls, level, kind) > 0.0f;
}

// At a visit, before anything is sold or bought: each measured kind whose stock fell since the last visit's end, on
// the same non-zero basis, gives a sample, fall / hours, averaged with the old value as RollWindow does
inline void SampleUse(WorldTravel::SlotMeasure& measure, StockLevels const& now, std::uint8_t cls, std::uint8_t level,
                      std::uint32_t nowMs)
{
    if (!measure.UseNoted)
        return;
    std::uint32_t const elapsed = nowMs - measure.UseNotedMs;
    if (elapsed < USE_WINDOW_MS)
        return;
    float const hours = static_cast<float>(elapsed) / USE_MS_PER_HOUR;
    for (std::size_t slot = 0; slot < Planner::UPKEEP_STOCK_COUNT; ++slot)
    {
        if (!UseMeasured(cls, level, static_cast<UpkeepStock>(slot)) || !now.basis[slot] ||
            now.basis[slot] != measure.UseBasis[slot] || now.count[slot] >= measure.UseStock[slot])
            continue;
        float const sample = static_cast<float>(measure.UseStock[slot] - now.count[slot]) / hours;
        float& use = measure.UsePerHour[slot];
        use = use > 0.0f ? (use + sample) / 2.0f : sample;
    }
}

// At the visit's end: the stock the next sample starts from
inline void NoteStock(WorldTravel::SlotMeasure& measure, StockLevels const& now, std::uint32_t nowMs)
{
    measure.UseNoted = true;
    measure.UseNotedMs = nowMs;
    measure.UseStock = now.count;
    measure.UseBasis = now.basis;
}

inline void FillUse(NeedRates& rates, WorldTravel::SlotMeasure const& measure)
{
    for (std::size_t slot = 0; slot < Planner::UPKEEP_STOCK_COUNT; ++slot)
        rates.usePerHour[slot] = measure.UsePerHour[slot];
}

// The measured travel shares (WU14); before the first window closes the defaults stay (walk 30 min/h, no flight)
inline void FillTravel(NeedRates& rates, WorldTravel::SlotMeasure const& measure)
{
    if (!measure.HasTravel)
        return;
    rates.walkMinutesPerHour = measure.TravelMinutesPerHour[WorldTravel::TRAVEL_WALK];
    rates.flightMinutesPerHour = measure.TravelMinutesPerHour[WorldTravel::TRAVEL_FLIGHT];
    rates.northrendFlightMinutesPerHour = measure.TravelMinutesPerHour[WorldTravel::TRAVEL_NORTHREND];
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
        "use measured from stock falls between vendor visits at least " + std::to_string(USE_WINDOW_MS / 60000) +
            " min apart, averaged with the old value; a stock errand when a due stock's gold pays a lot",
    };
}
}  // namespace UpkeepNeeds

#endif
