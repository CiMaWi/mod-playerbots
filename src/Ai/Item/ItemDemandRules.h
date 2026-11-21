/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMDEMANDRULES_H
#define _PLAYERBOT_ITEMDEMANDRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "ItemBuffRules.h"
#include "ItemOfferRules.h"
#include "ItemSituation.h"
#include "ItemSpendRules.h"
#include "ItemUseKinds.h"
#include "ItemUseRules.h"
#include "ItemUseTypes.h"
#include "ItemWorthRules.h"
#include "UpkeepNeedRules.h"

// Item use plan WI9a, PLAN 3.11, 3.14 and 3.16, INTERFACE 1.13: demand. One answer to "what does this bot want to
// carry, and what is one of them worth to it", for persistent bots (WI.9). Pure and std only, any thread, no clock
// and no global state: the same input gives the same answer, whatever the order of the list.
//
// The lines are keyed by DemandSlot, not by Kind: one food_drink line would let a mage's food count against its water.
// An entry fills one slot (DemandSlotOf): a buff by its exclusion word (flask, battle and guardian elixir, scroll,
// well-fed), a coating by the worn weapon it is the pick for, food and drink by the classifier's sub, every other
// consumed entry by the first fight need it answers (EntryAnswers: the offer's kinds are the fight's stock). Entries
// that are not consumed, conjured ones (the bot makes them) and kinds that are no stock (upgrade, learn, open, quest,
// mount, toy, transport, unclassified) are "not mine": VerdictOf gives DemandVerdict::None and the caller goes on.
//
// Usable: the engine lets the bot use it (canUse), the record's needs are met (level, class and race mask, skill
// rank: CheapCheck's tests), and per slot: a mana potion only for a mana user; a buff only when the buff picker picks
// it for the bot's role; a coating only when the coating picker picks it for a worn weapon (no hunter, no shaman:
// CoatsWeapons, WI8b's rule). The wanted stock of a slot is upkeep's formula, ceil(use per hour x STOCK_TARGET_HOURS x
// (1 + THRIFT_STOCK_SCALE x thrift)), and 0 when the bot holds nothing usable of the slot. Use per hour: a measured
// slot rate when given; food, drink and a rogue's poisons upkeep's (measured, else DefaultUsePerHour, so the stock
// equals upkeep's StockTarget; a rogue's poison stock is split over the two hands, the main hand the larger half);
// every other slot DEMAND_USE_PER_HOUR.
//
// Keep and sell: per slot the usable entries strongest first (the picker's candidate, then the buff score or the
// rank, then the lower item level, then the entry id); the kept units fill from the strongest up to the wanted stock.
// A kept entry is Use while the slot holds less than wanted (buy more), else Keep; one with no kept unit is Surplus;
// an entry of a slot the bot cannot use is Unusable. Worth per unit (3.11): what one saves (the spend rule's
// savesCopper of a typical case, the control cohort's gate ignored) capped by what replacing it costs; a kept entry
// is worth at least 1 copper.
//
// Every number below is a tuning value [E], one declaration each (CS.16), no conf option.
namespace ItemUse
{
constexpr float DEMAND_DANGER = 0.3f;        // the danger of the typical fight a fight item's worth is read in
constexpr float DEMAND_REST_MINUTES = 0.5f;  // the minutes of resting one food or drink saves
constexpr std::size_t DEMAND_MAX_ENTRIES = 255;

// Words, in this order: heal mana damage_one damage_area control move protect dispel boost summon resurrect flask
// battle_elixir guardian_elixir scroll food drink well_fed coat_main coat_off
enum class DemandSlot : std::uint8_t
{
    Heal,
    Mana,
    DamageOne,
    DamageArea,
    Control,
    Move,
    Protect,
    Dispel,
    Boost,
    Summon,
    Resurrect,
    Flask,
    BattleElixir,
    GuardianElixir,
    Scroll,
    Food,
    Drink,
    WellFed,
    CoatMain,
    CoatOff,
    Count
};

constexpr std::size_t DEMAND_SLOT_COUNT = static_cast<std::size_t>(DemandSlot::Count);
constexpr std::array<std::string_view, DEMAND_SLOT_COUNT> DEMAND_SLOT_WORDS = {
    "heal",   "mana",  "damage_one", "damage_area", "control",   "move",          "protect",
    "dispel", "boost", "summon",     "resurrect",   "flask",     "battle_elixir", "guardian_elixir",
    "scroll", "food",  "drink",      "well_fed",    "coat_main", "coat_off"};
static_assert(static_cast<std::size_t>(DemandSlot::CoatOff) + 1 == DEMAND_SLOT_COUNT, "a word per slot");

constexpr std::string_view DemandSlotWord(DemandSlot slot)
{
    std::size_t const index = static_cast<std::size_t>(slot);
    return index < DEMAND_SLOT_COUNT ? DEMAND_SLOT_WORDS[index] : std::string_view("heal");
}

// Use per hour of a slot that upkeep does not measure, by slot (food and drink: upkeep's)
constexpr std::array<float, DEMAND_SLOT_COUNT> DEMAND_USE_PER_HOUR = {
    2.0f,                    // heal
    2.0f,                    // mana: mana users only
    0.5f, 0.5f,              // damage_one damage_area
    0.3f, 0.3f,              // control move
    0.5f, 0.5f, 0.5f,        // protect dispel boost
    0.2f, 0.1f,              // summon resurrect
    0.5f, 0.5f, 0.5f, 0.5f,  // flask battle_elixir guardian_elixir scroll
    0.0f, 0.0f,              // food drink: upkeep's
    0.5f,                    // well_fed
    1.0f, 1.0f               // coat_main coat_off: a stone or an oil an hour per hand
};

enum class DemandVerdict : std::uint8_t
{
    Use,       // kept; the slot holds less than wanted: buy more
    Keep,      // kept; the slot is at its stock
    Surplus,   // usable, beyond the stock: sold
    Unusable,  // a stock slot the bot cannot use: sold
    None       // not listed: not demand's item
};

constexpr std::array<std::string_view, 5> DEMAND_VERDICT_WORDS = {"use", "keep", "surplus", "unusable", "none"};

constexpr std::string_view DemandVerdictWord(DemandVerdict verdict)
{
    std::size_t const index = static_cast<std::size_t>(verdict);
    return index < DEMAND_VERDICT_WORDS.size() ? DEMAND_VERDICT_WORDS[index] : std::string_view("none");
}

struct DemandLine
{
    DemandSlot slot{DemandSlot::Heal};
    Kind kind{Kind::Unclassified};
    std::uint8_t sub{0};           // a Sub where the slot splits its kind (food, drink, well_fed, mana, speed)
    std::uint32_t wantedStock{0};  // units
    std::uint32_t held{0};         // usable units held
    float usePerHour{0.0f};
    bool measured{false};              // the rate is a measured one
    std::uint32_t worthCopperEach{0};  // of the best entry
    std::uint32_t bestEntry{0};        // the strongest usable held entry, 0: none
    std::uint32_t buyEntry{0};         // the strongest usable offered entry (item use WI9b), 0: no buy ...
    std::uint32_t buyUnits{0};         // ... and the units up to the wanted stock; none for the control cohort
};

struct DemandKeep
{
    std::uint32_t entry{0};
    DemandSlot slot{DemandSlot::Heal};
    std::uint32_t keepUnits{0};
    std::uint32_t held{0};
    DemandVerdict verdict{DemandVerdict::Unusable};
    std::uint32_t worthCopperEach{0};  // one unit; 0 for an unusable entry
};

// A slot's usable offered entries, the strongest first: what the vendors may sell it (upkeep buys the first a vendor
// on the trip sells)
struct DemandBuy
{
    DemandSlot slot{DemandSlot::Heal};
    std::uint32_t entry{0};
};

struct DemandAnswer
{
    std::array<DemandLine, DEMAND_SLOT_COUNT> lines{};
    std::vector<DemandBuy> buys;  // the slots with buy units, by slot
    bool persistent{false};
    bool control{false};
    std::vector<DemandKeep> keeps;  // by entry, then slot
};

struct DemandSkill
{
    std::uint16_t skill{0};
    std::uint16_t value{0};
};

// The bot and what it carries. list[i], records[i], canUse[i] belong together; buyCopper, coatStats and poisonKinds
// are parallel too and may be shorter (none). The worn weapons' views carry no coating (demand asks what it would use).
struct DemandInput
{
    std::uint8_t cls{0};
    std::uint8_t race{0};
    std::uint8_t level{1};
    Role role{Role::Melee};
    std::uint32_t classBit{0};  // 1 << (class - 1)
    std::uint32_t raceBit{0};   // 1 << (race - 1)
    bool usesMana{false};
    std::span<DemandSkill const> skills;
    float thrift{0.0f};  // the trait's unit, -1..1
    float daring{0.0f};  // -caution's unit
    std::uint32_t earnCopperPerMinute{0};
    std::uint32_t deathCopper{0};
    bool persistent{false};
    bool control{false};  // the control cohort (WI.10): the same answer, printed
    std::array<float, Planner::UPKEEP_STOCK_COUNT> upkeepUsePerHour{};  // SlotMeasure::UsePerHour; 0: the default
    std::array<float, DEMAND_SLOT_COUNT> slotUsePerHour{};              // a measured slot rate; 0: none
    std::span<UsableEntry const> list;
    std::span<Record const> records;
    std::span<std::uint8_t const> canUse;   // Player::CanUseItem == EQUIP_ERR_OK
    std::span<std::uint8_t const> offered;  // 1: a vendor sells it to the bot (WI9b); count 0: offered, not held
    std::span<std::uint32_t const> buyCopper;
    std::span<BuffWeights const> coatStats;
    std::span<PoisonKind const> poisonKinds;
    std::span<float const> statWeights;  // shorter than BUFF_STAT_COUNT: the role's table
    TargetView mainHand;
    TargetView offHand;
};

constexpr Kind DemandSlotKind(DemandSlot slot)
{
    switch (slot)
    {
        case DemandSlot::Heal:
            return Kind::Heal;
        case DemandSlot::Mana:
            return Kind::Mana;
        case DemandSlot::DamageOne:
            return Kind::DamageOne;
        case DemandSlot::DamageArea:
            return Kind::DamageArea;
        case DemandSlot::Control:
            return Kind::Control;
        case DemandSlot::Move:
            return Kind::Move;
        case DemandSlot::Protect:
            return Kind::Protect;
        case DemandSlot::Dispel:
            return Kind::Dispel;
        case DemandSlot::Boost:
            return Kind::Boost;
        case DemandSlot::Summon:
            return Kind::Summon;
        case DemandSlot::Resurrect:
            return Kind::Resurrect;
        case DemandSlot::Flask:
        case DemandSlot::BattleElixir:
        case DemandSlot::GuardianElixir:
        case DemandSlot::Scroll:
            return Kind::LongBuff;
        case DemandSlot::Food:
        case DemandSlot::Drink:
        case DemandSlot::WellFed:
            return Kind::FoodDrink;
        case DemandSlot::CoatMain:
        case DemandSlot::CoatOff:
            return Kind::Coating;
        default:
            return Kind::Unclassified;
    }
}

constexpr std::uint8_t DemandSlotSub(DemandSlot slot)
{
    switch (slot)
    {
        case DemandSlot::Mana:
            return static_cast<std::uint8_t>(Sub::Mana);
        case DemandSlot::Move:
            return static_cast<std::uint8_t>(Sub::Speed);
        case DemandSlot::Food:
            return static_cast<std::uint8_t>(Sub::Food);
        case DemandSlot::Drink:
            return static_cast<std::uint8_t>(Sub::Drink);
        case DemandSlot::WellFed:
            return static_cast<std::uint8_t>(Sub::WellFed);
        default:
            return 0;
    }
}

constexpr DemandSlot SlotOfNeed(Need need)
{
    switch (need)
    {
        case Need::Heal:
            return DemandSlot::Heal;
        case Need::Mana:
            return DemandSlot::Mana;
        case Need::Protect:
            return DemandSlot::Protect;
        case Need::Dispel:
            return DemandSlot::Dispel;
        case Need::Speed:
            return DemandSlot::Move;
        case Need::Boost:
            return DemandSlot::Boost;
        case Need::DamageOne:
            return DemandSlot::DamageOne;
        case Need::DamageArea:
            return DemandSlot::DamageArea;
        case Need::Control:
            return DemandSlot::Control;
        case Need::Resurrect:
            return DemandSlot::Resurrect;
        default:
            return DemandSlot::Summon;  // helper, decoy, repair
    }
}

// WI8b's rule: hunters and shamans coat no weapon (a shaman imbues, a hunter's weapon is the ranged one)
constexpr bool CoatsWeapons(std::uint8_t cls)
{
    return cls != UpkeepNeeds::CLASS_HUNTER && cls != UpkeepNeeds::CLASS_SHAMAN;
}

constexpr bool IsBuffDemandSlot(DemandSlot slot)
{
    return slot == DemandSlot::Flask || slot == DemandSlot::BattleElixir || slot == DemandSlot::GuardianElixir ||
           slot == DemandSlot::Scroll || slot == DemandSlot::WellFed;
}

constexpr bool IsCoatDemandSlot(DemandSlot slot) { return slot == DemandSlot::CoatMain || slot == DemandSlot::CoatOff; }

constexpr bool IsFightDemandSlot(DemandSlot slot) { return slot <= DemandSlot::Resurrect; }

// The slot an entry fills before the hands are settled (a coating: CoatMain); false: not demand's item
inline bool DemandSlotOf(UsableEntry const& entry, Record const& record, bool usesMana, DemandSlot& slot)
{
    if (!entry.consumed || record.conjured)
        return false;

    switch (record.exclude)
    {
        case Exclude::Flask:
            slot = DemandSlot::Flask;
            return true;
        case Exclude::BattleElixir:
            slot = DemandSlot::BattleElixir;
            return true;
        case Exclude::GuardianElixir:
            slot = DemandSlot::GuardianElixir;
            return true;
        case Exclude::Scroll:
            slot = DemandSlot::Scroll;
            return true;
        case Exclude::WellFed:
            slot = DemandSlot::WellFed;
            return true;
        case Exclude::Coating:
            slot = DemandSlot::CoatMain;
            return true;
        default:
            break;
    }

    if (entry.kind == Kind::FoodDrink)
    {
        Sub const sub = static_cast<Sub>(entry.sub);
        if (sub == Sub::WellFed)
            slot = DemandSlot::WellFed;
        else if (sub == Sub::Drink || (sub == Sub::FoodDrink && usesMana))
            slot = DemandSlot::Drink;
        else
            slot = DemandSlot::Food;
        return true;
    }

    for (std::size_t i = 0; i < NEED_COUNT; ++i)
        if (EntryAnswers(entry, static_cast<Need>(i)))
        {
            slot = SlotOfNeed(static_cast<Need>(i));
            return true;
        }
    return false;
}

namespace DemandDetail
{
struct Item  // one (entry, record) of the list, in the canonical order
{
    UsableEntry entry;
    Record const* record{nullptr};
    bool canUse{false};
    std::uint32_t buy{0};
    BuffWeights stats{};
    PoisonKind poison{PoisonKind::None};
    DemandSlot slot{DemandSlot::Heal};
    bool usable{false};
    bool candidate{false};  // the picker's pick for the slot
    float strength{0.0f};
    bool offered{false};  // a vendor sells it (a count of 0: offered only, never kept or sold)
};

inline bool NeedsMet(DemandInput const& input, Needs const& needs)
{
    if (input.level < needs.level)
        return false;
    if ((needs.classMask && !(needs.classMask & input.classBit)) ||
        (needs.raceMask && !(needs.raceMask & input.raceBit)))
        return false;
    if (!needs.skill)
        return true;
    for (DemandSkill const& skill : input.skills)
        if (skill.skill == needs.skill)
            return skill.value >= needs.skillRank;
    return false;
}

inline BuffBot BuffBotOf(DemandInput const& input)
{
    BuffBot bot;
    bot.role = input.role;
    bot.level = input.level;
    bot.ahead = Ahead::Dungeon;  // what it would drink when something is ahead
    bot.statWeights = input.statWeights;
    bot.thrift = input.thrift;
    bot.daring = input.daring;
    bot.deathCopper = input.deathCopper;
    bot.earnCopperPerMinute = input.earnCopperPerMinute;
    bot.control = false;  // worth and choice are not spend decisions
    bot.classMask = input.classBit;
    return bot;
}

// The picker weighs held coatings only: an offered one (a count of 0) is asked as if held (upkeep WU37a)
inline UsableEntry Pickable(UsableEntry entry)
{
    entry.count = std::max<std::uint32_t>(entry.count, 1);
    return entry;
}

inline CoatPick CoatOf(BuffBot const& bot, Item const& item, TargetView const& weapon, bool offHand)
{
    UsableEntry const entry = Pickable(item.entry);
    return PickCoating(bot, std::span<UsableEntry const>(&entry, 1), std::span<Record const>(item.record, 1),
                       std::span<BuffWeights const>(&item.stats, 1), weapon, offHand,
                       std::span<PoisonKind const>(&item.poison, 1));
}

inline float Stock(float usePerHour, float thrift)
{
    float const use = std::isfinite(usePerHour) && usePerHour > 0.0f ? usePerHour : 0.0f;
    float const unit = std::isfinite(thrift) ? std::clamp(thrift, -1.0f, 1.0f) : 0.0f;
    float const scale = 1.0f + UpkeepNeeds::THRIFT_STOCK_SCALE * unit;
    return std::ceil(use * UpkeepNeeds::STOCK_TARGET_HOURS * scale);
}

inline float UpkeepRate(DemandInput const& input, Planner::UpkeepStock kind)
{
    float const measured = input.upkeepUsePerHour[static_cast<std::size_t>(kind)];
    return std::isfinite(measured) && measured > 0.0f ? measured
                                                      : UpkeepNeeds::DefaultUsePerHour(input.cls, input.level, kind);
}

inline bool UpkeepMeasured(DemandInput const& input, Planner::UpkeepStock kind)
{
    float const measured = input.upkeepUsePerHour[static_cast<std::size_t>(kind)];
    return std::isfinite(measured) && measured > 0.0f;
}

inline std::uint32_t Cap(Record const& record, std::uint32_t buy)
{
    if (record.price != PriceSource::None)
        return record.replaceCopper;
    double const old = std::max<double>(buy, static_cast<double>(record.sellCopper) * WorldTravel::BUY_PER_SELL_PRICE);
    return static_cast<std::uint32_t>(std::min(old, 4294967295.0));
}

inline float Saves(DemandInput const& input, BuffBot const& bot, Item const& item)
{
    if (IsBuffDemandSlot(item.slot))
        return Spend(BuffSpendCase(bot, item.entry, *item.record)).savesCopper;
    if (IsCoatDemandSlot(item.slot))
        return Spend(CoatSpendCase(bot, item.entry)).savesCopper;
    if (item.slot == DemandSlot::Food || item.slot == DemandSlot::Drink)
        return DEMAND_REST_MINUTES * static_cast<float>(input.earnCopperPerMinute);

    SpendCase spendCase;
    spendCase.kind = item.entry.kind;
    spendCase.rank = item.entry.rank;
    spendCase.replaceCopper = item.entry.replaceCopper;
    spendCase.potion = item.entry.potion;
    spendCase.danger = DEMAND_DANGER;
    spendCase.hits = AREA_MIN_HITS;
    spendCase.deathCopper = input.deathCopper;
    spendCase.earnCopperPerMinute = input.earnCopperPerMinute;
    spendCase.thrift = input.thrift;
    spendCase.daring = input.daring;
    return Spend(spendCase).savesCopper;
}

inline std::uint32_t WorthEach(DemandInput const& input, BuffBot const& bot, Item const& item)
{
    float const saves = Saves(input, bot, item);
    double const each = std::isfinite(saves) && saves > 0.0f ? std::floor(static_cast<double>(saves)) : 0.0;
    return static_cast<std::uint32_t>(std::min(each, static_cast<double>(Cap(*item.record, item.buy))));
}
}  // namespace DemandDetail

inline DemandAnswer Demand(DemandInput const& input)
{
    using DemandDetail::Item;
    DemandAnswer answer;
    answer.persistent = input.persistent;
    answer.control = input.control;
    for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
    {
        DemandSlot const slot = static_cast<DemandSlot>(s);
        answer.lines[s].slot = slot;
        answer.lines[s].kind = DemandSlotKind(slot);
        answer.lines[s].sub = DemandSlotSub(slot);
    }
    if (!input.persistent)
        return answer;

    // The canonical order: by entry, record, the larger count first; one item per (entry, record)
    std::size_t const count = std::min({input.list.size(), input.records.size(), input.canUse.size()});
    std::vector<std::size_t> order;
    order.reserve(count);
    auto const offered = [&](std::size_t i) { return i < input.offered.size() && input.offered[i] != 0; };
    for (std::size_t i = 0; i < count; ++i)
        if (input.list[i].count > 0 || offered(i))
            order.push_back(i);
    auto const key = [&](std::size_t i)
    {
        UsableEntry const& entry = input.list[i];
        return std::make_tuple(entry.item.entry, entry.record, ~entry.count, input.canUse[i] ? 0 : 1);
    };
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return key(a) < key(b); });

    std::vector<Item> items;
    items.reserve(std::min(order.size(), DEMAND_MAX_ENTRIES));
    std::size_t heldItems = 0;  // the cap counts held and offered entries apart: an offer never pushes a held one out
    std::size_t offeredItems = 0;
    for (std::size_t const i : order)
    {
        UsableEntry const& entry = input.list[i];
        if (!items.empty() && items.back().entry.item.entry == entry.item.entry &&
            items.back().entry.record == entry.record)
        {
            items.back().offered = items.back().offered || offered(i);
            continue;
        }
        std::size_t& capped = entry.count > 0 ? heldItems : offeredItems;
        if (capped >= DEMAND_MAX_ENTRIES)
            continue;

        Item item;
        item.entry = entry;
        item.entry.ready = true;  // demand asks what it would use, not what is ready now
        item.record = &input.records[i];
        item.canUse = input.canUse[i] != 0;
        item.buy = i < input.buyCopper.size() ? input.buyCopper[i] : 0;
        item.stats = i < input.coatStats.size() ? input.coatStats[i] : BuffWeights{};
        item.poison = i < input.poisonKinds.size() ? input.poisonKinds[i] : PoisonKind::None;
        item.offered = offered(i);
        if (!DemandSlotOf(item.entry, *item.record, input.usesMana, item.slot))
            continue;
        ++capped;
        items.push_back(item);
    }

    // Usable per slot; the pickers name each buff slot's and each hand's candidate
    BuffBot const bot = DemandDetail::BuffBotOf(input);
    std::span<float const> const weights = BuffWeightsOf(bot);
    bool const coats = CoatsWeapons(input.cls);
    TargetView main = input.mainHand;
    TargetView off = input.offHand;
    main.itemHasCoating = off.itemHasCoating = false;
    std::array<bool, 2> fits{};
    for (Item& item : items)
    {
        bool const base = item.canUse && DemandDetail::NeedsMet(input, item.record->needs);
        item.strength = static_cast<float>(item.entry.rank);
        if (IsBuffDemandSlot(item.slot))
        {
            item.strength = BuffScore(*item.record, weights);
            item.usable = base && !PickBuffs(bot, std::span<UsableEntry const>(&item.entry, 1),
                                             std::span<Record const>(item.record, 1))
                                       .empty();
        }
        else if (IsCoatDemandSlot(item.slot))
        {
            fits[0] = base && coats && main.found && DemandDetail::CoatOf(bot, item, main, false).index == 0;
            fits[1] = base && coats && off.found && DemandDetail::CoatOf(bot, item, off, true).index == 0;
            item.usable = fits[0] || fits[1];
            if (!fits[0] && fits[1])
                item.slot = DemandSlot::CoatOff;
        }
        else
            item.usable = base && (item.slot != DemandSlot::Mana || input.usesMana);
    }

    for (DemandSlot const slot : {DemandSlot::Flask, DemandSlot::BattleElixir, DemandSlot::GuardianElixir,
                                  DemandSlot::Scroll, DemandSlot::WellFed})
    {
        std::vector<UsableEntry> list;
        std::vector<Record> records;
        std::vector<std::size_t> index;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].usable && items[i].slot == slot)
            {
                list.push_back(items[i].entry);
                records.push_back(*items[i].record);
                index.push_back(i);
            }
        if (list.empty())
            continue;
        std::vector<BuffPick> const picks = PickBuffs(bot, list, records);
        if (!picks.empty() && picks.front().usable < index.size())
            items[index[picks.front().usable]].candidate = true;
    }

    if (coats)
    {
        std::vector<UsableEntry> list;
        std::vector<Record> records;
        std::vector<BuffWeights> stats;
        std::vector<PoisonKind> kinds;
        std::vector<std::size_t> index;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].usable && IsCoatDemandSlot(items[i].slot))
            {
                list.push_back(DemandDetail::Pickable(items[i].entry));
                records.push_back(*items[i].record);
                stats.push_back(items[i].stats);
                kinds.push_back(items[i].poison);
                index.push_back(i);
            }
        std::array<int, 2> picked = {-1, -1};
        for (std::size_t hand = 0; hand < 2 && !list.empty(); ++hand)
        {
            TargetView const& weapon = hand == 0 ? main : off;
            if (!weapon.found)
                continue;
            CoatPick const pick = PickCoating(bot, list, records, stats, weapon, hand == 1, kinds);
            if (pick.index >= 0 && static_cast<std::size_t>(pick.index) < index.size())
                picked[hand] = static_cast<int>(index[static_cast<std::size_t>(pick.index)]);
        }
        if (picked[0] >= 0)
        {
            items[static_cast<std::size_t>(picked[0])].slot = DemandSlot::CoatMain;
            items[static_cast<std::size_t>(picked[0])].candidate = true;
        }
        if (picked[1] >= 0 && picked[1] != picked[0])
        {
            items[static_cast<std::size_t>(picked[1])].slot = DemandSlot::CoatOff;
            items[static_cast<std::size_t>(picked[1])].candidate = true;
        }
    }

    // One keep per (entry, slot): the item's records of one slot merge (usable if any is)
    struct Pending
    {
        DemandKeep keep;
        bool usable{false};
        bool candidate{false};
        float strength{0.0f};
        std::uint16_t itemLevel{0};
        std::uint32_t worth{0};
        bool offered{false};
    };
    std::vector<Pending> pending;
    for (Item const& item : items)
    {
        std::uint32_t const worth = item.usable ? DemandDetail::WorthEach(input, bot, item) : 0;
        auto found = std::find_if(pending.begin(), pending.end(), [&](Pending const& p)
                                  { return p.keep.entry == item.entry.item.entry && p.keep.slot == item.slot; });
        if (found == pending.end())
        {
            Pending next;
            next.keep.entry = item.entry.item.entry;
            next.keep.slot = item.slot;
            next.keep.held = item.entry.count;
            next.usable = item.usable;
            next.candidate = item.candidate;
            next.strength = item.usable ? item.strength : 0.0f;
            next.itemLevel = item.record->itemLevel;
            next.worth = worth;
            next.offered = item.offered;
            pending.push_back(next);
            continue;
        }
        found->keep.held = std::max(found->keep.held, item.entry.count);
        found->offered = found->offered || item.offered;
        found->usable = found->usable || item.usable;
        found->candidate = found->candidate || item.candidate;
        if (item.usable)
            found->strength = std::max(found->strength, item.strength);
        found->worth = std::max(found->worth, worth);
    }

    // The hands' share of a rogue's poison stock: settled once both hands' holdings are known
    bool const rogue = input.cls == UpkeepNeeds::CLASS_ROGUE;
    std::array<std::uint32_t, DEMAND_SLOT_COUNT> held{};
    std::array<bool, DEMAND_SLOT_COUNT> offeredSlot{};  // a usable entry a vendor sells: a stock is wanted
    for (Pending const& p : pending)
        if (p.usable)
        {
            held[static_cast<std::size_t>(p.keep.slot)] += p.keep.held;
            offeredSlot[static_cast<std::size_t>(p.keep.slot)] =
                offeredSlot[static_cast<std::size_t>(p.keep.slot)] || p.offered;
        }

    std::size_t const mainSlot = static_cast<std::size_t>(DemandSlot::CoatMain);
    std::size_t const offSlot = static_cast<std::size_t>(DemandSlot::CoatOff);
    float const poisonRate = DemandDetail::UpkeepRate(input, Planner::UpkeepStock::Poison);
    float const poisonStock = DemandDetail::Stock(poisonRate, input.thrift);
    bool const bothHands = held[mainSlot] > 0 && held[offSlot] > 0;

    for (std::size_t s = 0; s < DEMAND_SLOT_COUNT; ++s)
    {
        DemandSlot const slot = static_cast<DemandSlot>(s);
        DemandLine& line = answer.lines[s];
        line.held = held[s];
        float const measured = input.slotUsePerHour[s];
        bool const slotMeasured = std::isfinite(measured) && measured > 0.0f;
        bool const rogueHand = rogue && IsCoatDemandSlot(slot);
        float stock = 0.0f;
        if (slotMeasured)
        {
            line.usePerHour = measured;
            line.measured = true;
        }
        else if (slot == DemandSlot::Food || slot == DemandSlot::Drink)
        {
            Planner::UpkeepStock const kind =
                slot == DemandSlot::Food ? Planner::UpkeepStock::Food : Planner::UpkeepStock::Water;
            line.usePerHour = DemandDetail::UpkeepRate(input, kind);
            line.measured = DemandDetail::UpkeepMeasured(input, kind);
        }
        else if (rogueHand)
        {
            line.usePerHour = bothHands ? poisonRate / 2.0f : poisonRate;
            line.measured = DemandDetail::UpkeepMeasured(input, Planner::UpkeepStock::Poison);
            float const half = std::ceil(poisonStock / 2.0f);
            stock = !bothHands ? poisonStock : (slot == DemandSlot::CoatMain ? half : poisonStock - half);
        }
        else
            line.usePerHour = DEMAND_USE_PER_HOUR[s];
        if (!rogueHand || slotMeasured)
            stock = DemandDetail::Stock(line.usePerHour, input.thrift);
        line.wantedStock =
            line.held > 0 || offeredSlot[s] ? static_cast<std::uint32_t>(std::min(stock, 1000000.0f)) : 0;

        // The slot's usable keeps, strongest first
        std::vector<std::size_t> members;
        for (std::size_t i = 0; i < pending.size(); ++i)
            if (pending[i].usable && pending[i].keep.slot == slot)
                members.push_back(i);
        std::sort(members.begin(), members.end(),
                  [&](std::size_t a, std::size_t b)
                  {
                      Pending const& x = pending[a];
                      Pending const& y = pending[b];
                      return std::make_tuple(!x.candidate, -x.strength, x.itemLevel, x.keep.entry) <
                             std::make_tuple(!y.candidate, -y.strength, y.itemLevel, y.keep.entry);
                  });
        std::uint32_t left = line.wantedStock;
        for (std::size_t const i : members)
        {
            DemandKeep& keep = pending[i].keep;
            keep.keepUnits = std::min(keep.held, left);
            left -= keep.keepUnits;
            keep.worthCopperEach = pending[i].worth;
            if (keep.keepUnits == 0)
                keep.verdict = DemandVerdict::Surplus;
            else
            {
                keep.verdict = line.held < line.wantedStock ? DemandVerdict::Use : DemandVerdict::Keep;
                keep.worthCopperEach = std::max<std::uint32_t>(keep.worthCopperEach, 1);
            }
        }
        for (std::size_t const i : members)
            if (pending[i].keep.held > 0)
            {
                line.bestEntry = pending[i].keep.entry;
                line.worthCopperEach = pending[i].keep.worthCopperEach;
                break;
            }

        // The buy (WI9b): up to the wanted stock, the usable offered entries strongest first; none for the control
        // cohort (WI.10)
        if (input.control || line.held >= line.wantedStock)
            continue;
        for (std::size_t const i : members)
            if (pending[i].offered)
            {
                if (!line.buyEntry)
                    line.buyEntry = pending[i].keep.entry;
                answer.buys.push_back({slot, pending[i].keep.entry});
            }
        if (line.buyEntry)
            line.buyUnits = line.wantedStock - line.held;
    }

    answer.keeps.reserve(pending.size());
    for (Pending const& p : pending)
    {
        if (p.keep.held == 0)
            continue;  // offered only
        DemandKeep keep = p.keep;
        if (!p.usable)
        {
            keep.verdict = DemandVerdict::Unusable;
            keep.keepUnits = 0;
            keep.worthCopperEach = 0;
        }
        answer.keeps.push_back(keep);
    }
    std::sort(answer.keeps.begin(), answer.keeps.end(), [](DemandKeep const& a, DemandKeep const& b)
              { return std::make_tuple(a.entry, a.slot) < std::make_tuple(b.entry, b.slot); });
    return answer;
}

// The entry's verdict: the most keeping of its slots' (Use, Keep, Surplus, Unusable); None when it is not listed
inline DemandVerdict VerdictOf(DemandAnswer const& answer, std::uint32_t entry, DemandSlot* slot = nullptr)
{
    DemandVerdict best = DemandVerdict::None;
    for (DemandKeep const& keep : answer.keeps)
        if (keep.entry == entry && keep.verdict < best)
        {
            best = keep.verdict;
            if (slot)
                *slot = keep.slot;
        }
    return best;
}

// One unit's worth for a kept entry; 0: not kept or not listed
inline std::uint32_t WorthEachOf(DemandAnswer const& answer, std::uint32_t entry)
{
    std::uint32_t worth = 0;
    for (DemandKeep const& keep : answer.keeps)
        if (keep.entry == entry && keep.keepUnits > 0)
            worth = std::max(worth, keep.worthCopperEach);
    return worth;
}

// The slots whose buys upkeep's stock needs make (WU37a): food, drink and the coatings; the others are use items
constexpr bool UpkeepStockSlot(DemandSlot slot)
{
    return slot == DemandSlot::Food || slot == DemandSlot::Drink || IsCoatDemandSlot(slot);
}

// Upkeep's food, water or poison from demand's lines (upkeep WU37a, WU.27): demand's buys of the slot, the strongest
// first, and its held and wanted units. A rogue's hands are one poison stock: each hand's items buy up to its share
// (the main hand the larger half, the whole stock when one hand coats), never past the stock as a whole, so what one
// hand buys never makes the other's surplus. The control cohort and a bot that is not persistent are not answered:
// they buy the kind's best offer by level, their gold seeking unchanged
inline UpkeepNeeds::StockWant StockWantOf(DemandAnswer const& answer, Planner::UpkeepStock kind)
{
    UpkeepNeeds::StockWant want;
    if (!answer.persistent || answer.control ||
        (kind != Planner::UpkeepStock::Food && kind != Planner::UpkeepStock::Water &&
         kind != Planner::UpkeepStock::Poison))
        return want;
    want.answered = true;
    auto const addBuys = [&](DemandSlot slot, std::uint32_t units)
    {
        if (!units)
            return;
        for (DemandBuy const& buy : answer.buys)
            if (buy.slot == slot && std::find(want.items.begin(), want.items.end(), buy.entry) == want.items.end())
            {
                want.items.push_back(buy.entry);
                want.itemUnits.push_back(units);
            }
    };
    auto const lineOf = [&](DemandSlot slot) -> DemandLine const&
    { return answer.lines[static_cast<std::size_t>(slot)]; };

    if (kind != Planner::UpkeepStock::Poison)
    {
        DemandSlot const slot = kind == Planner::UpkeepStock::Food ? DemandSlot::Food : DemandSlot::Drink;
        DemandLine const& line = lineOf(slot);
        want.slot = static_cast<std::uint8_t>(slot);
        want.target = line.wantedStock;
        want.held = line.held;
        want.units = want.target > want.held ? want.target - want.held : 0;
        addBuys(slot, want.units);
        return want;
    }

    DemandLine const& main = lineOf(DemandSlot::CoatMain);
    DemandLine const& off = lineOf(DemandSlot::CoatOff);
    want.slot = static_cast<std::uint8_t>(DemandSlot::CoatMain);
    want.held = main.held + off.held;
    want.target = main.held > 0 && off.held > 0 ? main.wantedStock + off.wantedStock
                                                : std::max(main.wantedStock, off.wantedStock);
    std::uint32_t const mainShare = !main.wantedStock ? 0 : (off.wantedStock ? (want.target + 1) / 2 : want.target);
    std::uint32_t const offShare = off.wantedStock ? want.target - mainShare : 0;
    std::uint32_t left = want.target > want.held ? want.target - want.held : 0;
    std::uint32_t const mainUnits = std::min(left, mainShare > main.held ? mainShare - main.held : 0);
    left -= mainUnits;
    std::uint32_t const offUnits = std::min(left, offShare > off.held ? offShare - off.held : 0);
    want.units = mainUnits + offUnits;
    addBuys(DemandSlot::CoatMain, mainUnits);
    addBuys(DemandSlot::CoatOff, offUnits);
    return want;
}
}  // namespace ItemUse

#endif
