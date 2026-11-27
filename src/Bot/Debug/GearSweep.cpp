/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GearSweep.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "AiFactory.h"
#include "Chat.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"

namespace GearSweep
{
namespace
{
using Clock = std::chrono::steady_clock;

enum Flag : uint32
{
    FLAG_S1 = 0,
    FLAG_S2,
    FLAG_S3,
    FLAG_S4,
    FLAG_S5,
    FLAG_S6,
    FLAG_X1,
    FLAG_X2,
    FLAG_COUNT
};

char const* const FLAG_NAMES[FLAG_COUNT] = {"S1", "S2", "S3", "S4", "S5", "S6", "X1", "X2"};

using Counts = std::array<uint32, FLAG_COUNT>;

// The rule's gates before it scores (ItemUsageValue.cpp, QueryItemUsageForEquip): an item stopped there reads NONE
enum class Gate : uint8
{
    Ok,
    Create,     // no temporary item
    Equip,      // the bot's CanEquipItem refuses it
    Unique,     // a unique item the bot wears already
    TitanGrip,  // a two-hander a titan grip bot may not wield
};

// PLAN item 3: a starter cloak worn over an epic one by death knights
constexpr uint32 PAIR_STARTER = 34659;
constexpr uint32 PAIR_EPIC = 18509;
constexpr uint32 PAIR_MIN_LEVEL = 57;
constexpr uint32 PAIR_MAX_LEVEL = 60;

constexpr uint32 LEVEL_WINDOW = 10;

// The talents the score's weapon factors read (StatsWeightCalculator), for the per-bot line
constexpr uint32 SPELL_ROGUE_SWORD_SPECIALIZATION = 13964;
constexpr uint32 SPELL_POLEAXE_SPECIALIZATION = 12785;
constexpr uint32 SPELL_NERVES_OF_COLD_STEEL = 50138;

struct Worn
{
    uint8 slot = NULL_SLOT;
    uint32 entry = 0;  // 0: the slot is empty
    int32 prop = 0;
    float score = 0.0f;
    bool full = false;
    bool kind = false;
    bool broken = false;
    std::string stats;
};

// What a template's own bonuses are made of
struct Bonus
{
    bool primary = false;  // health, mana, agility, strength, intellect, spirit or stamina: what the stat table reads
    bool rating = false;   // any other stat type
    bool spell = false;
};

struct Row
{
    ItemTemplate const* proto = nullptr;
    int32 prop = 0;
    float score = 0.0f;
    ItemUsage usage = ITEM_USAGE_NONE;
    Gate gate = Gate::Ok;
    bool full = false;
    bool kind = false;
    char const* cause = "-";
    uint8 slotKey = NULL_SLOT;
    std::array<Worn, 2> worn;
    uint8 wornCount = 0;
    uint32 flags = 0;
    std::string stats;
};

struct BotResult
{
    Counts counts{};
    uint32 rows = 0;
};

// One file per server start; every call appends to it
std::string sweepFile;

char const* UsageWord(ItemUsage usage)
{
    switch (usage)
    {
        case ITEM_USAGE_NONE:
            return "NONE";
        case ITEM_USAGE_EQUIP:
            return "EQUIP";
        case ITEM_USAGE_REPLACE:
            return "REPLACE";
        case ITEM_USAGE_BAD_EQUIP:
            return "BAD_EQUIP";
        case ITEM_USAGE_BROKEN_EQUIP:
            return "BROKEN_EQUIP";
        default:
            return "OTHER";
    }
}

char const* GateWord(Gate gate)
{
    switch (gate)
    {
        case Gate::Ok:
            return "ok";
        case Gate::Create:
            return "create";
        case Gate::Equip:
            return "equip";
        case Gate::Unique:
            return "unique";
        case Gate::TitanGrip:
            return "titangrip";
    }
    return "-";
}

bool IsUpgrade(ItemUsage usage) { return usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE; }

bool IsTitanGripKind(ItemTemplate const* proto)
{
    return proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 || proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
           proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2;
}

// The factory's fit test: CanEquipWeapon, or CanEquipArmor with its class stat table (recorded beside the rule's)
bool FullFits(ItemTemplate const* proto, Player* bot)
{
    if (proto->Class == ITEM_CLASS_WEAPON)
        return sRandomItemMgr.CanEquipWeapon(proto, bot->getClass());
    if (proto->Class == ITEM_CLASS_ARMOR)
        return sRandomItemMgr.CanEquipArmor(proto, bot->getClass(), bot->GetLevel());
    return true;
}

// The rule's kind test: CanEquipWeapon, or the armour kind without the class stat table. A weapon's test has no table.
bool KindFits(ItemTemplate const* proto, Player* bot, bool full)
{
    if (proto->Class != ITEM_CLASS_ARMOR)
        return full;
    return sRandomItemMgr.CanEquipArmorKind(proto, bot->getClass(), bot->GetLevel());
}

Bonus BonusOf(ItemTemplate const* proto)
{
    Bonus bonus;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        if (!proto->ItemStat[i].ItemStatValue)
            continue;
        switch (proto->ItemStat[i].ItemStatType)
        {
            case ITEM_MOD_MANA:
            case ITEM_MOD_HEALTH:
            case ITEM_MOD_AGILITY:
            case ITEM_MOD_STRENGTH:
            case ITEM_MOD_INTELLECT:
            case ITEM_MOD_SPIRIT:
            case ITEM_MOD_STAMINA:
                bonus.primary = true;
                break;
            default:
                bonus.rating = true;
                break;
        }
    }
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (proto->Spells[i].SpellId > 0)
            bonus.spell = true;
    return bonus;
}

// Why a row's item is unfit, or that its score is 0 or below; "-" for a fitting item with a positive score
char const* CauseOf(bool kind, bool full, float score, int32 prop, Bonus const& bonus)
{
    if (!kind)
        return "kind";
    if (!full)
    {
        if (bonus.primary)
            return "table";
        if (prop)
            return "suffix";
        if (bonus.rating)
            return "ratings";
        if (bonus.spell)
            return "spell";
        return "nostats";
    }
    if (score <= 0.0f)
        return "negative";
    return "-";
}

// The level the window reads: the required level, or five below the item level for a template without one
uint32 EffectiveLevel(ItemTemplate const* proto)
{
    if (proto->RequiredLevel)
        return proto->RequiredLevel;
    return proto->ItemLevel > 6 ? proto->ItemLevel - 5 : 1;
}

bool InWindow(ItemTemplate const* proto, uint32 level)
{
    uint32 const effective = EffectiveLevel(proto);
    return effective <= level && effective + LEVEL_WINDOW >= level;
}

// Weapon and armour templates the rule judges by score (no bags, quivers or miscellaneous weapons), by entry
std::vector<ItemTemplate const*> Candidates()
{
    std::vector<ItemTemplate const*> out;
    for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (proto.Class != ITEM_CLASS_WEAPON && proto.Class != ITEM_CLASS_ARMOR)
            continue;
        if (proto.InventoryType == INVTYPE_NON_EQUIP)
            continue;
        if (proto.Class == ITEM_CLASS_WEAPON && proto.SubClass == ITEM_SUBCLASS_WEAPON_MISC)
            continue;
        out.push_back(&proto);
    }
    std::sort(out.begin(), out.end(),
              [](ItemTemplate const* a, ItemTemplate const* b) { return a->ItemId < b->ItemId; });
    return out;
}

// The random properties (positive ids) or suffixes (negative ids) asked for: the pool's first, middle and last entry;
// 0 for an item without a pool
std::vector<int32> PropsOf(ItemTemplate const* proto)
{
    uint32 poolEntry = proto->RandomProperty;
    bool suffix = false;
    if (!poolEntry)
    {
        poolEntry = proto->RandomSuffix;
        suffix = true;
    }
    if (!poolEntry)
        return {0};
    std::vector<uint32> const& pool = sRandomItemMgr.GetEnchantmentPool(poolEntry);
    if (pool.empty())
        return {0};

    std::vector<uint32> picks = {pool.front()};
    if (pool.size() > 2)
        picks.push_back(pool[pool.size() / 2]);
    if (pool.size() > 1)
        picks.push_back(pool.back());

    std::vector<int32> props;
    for (uint32 id : picks)
    {
        int32 const prop = suffix ? -static_cast<int32>(id) : static_cast<int32>(id);
        if (std::find(props.begin(), props.end(), prop) == props.end())
            props.push_back(prop);
    }
    return props;
}

ItemUsage Ask(PlayerbotAI* botAI, uint32 entry, int32 prop)
{
    // A local value object: one per qualifier in the AI context would pile up
    ItemUpgradeValue value(botAI);
    value.Qualify(prop ? std::to_string(entry) + "," + std::to_string(prop) : std::to_string(entry));
    return value.Calculate();
}

Gate GateOf(Player* bot, PlayerbotAI* botAI, ItemTemplate const* proto)
{
    Item* item = Item::CreateItem(proto->ItemId, 1, bot);
    if (!item)
        return Gate::Create;
    uint16 dest = 0;
    InventoryResult const result = botAI->CanEquipItem(NULL_SLOT, dest, item, true, true);
    item->RemoveFromUpdateQueueOf(bot);
    delete item;

    if (result != EQUIP_ERR_OK && result != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
        return Gate::Equip;
    bool const unique = result == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS || proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE);
    if (unique && bot->HasItemOrGemWithIdEquipped(proto->ItemId, 1))
        return Gate::Unique;
    if (bot->CanTitanGrip() && proto->InventoryType == INVTYPE_2HWEAPON && !IsTitanGripKind(proto))
        return Gate::TitanGrip;
    return Gate::Ok;
}

// The slots the rule compares against: its destination, and the second ring, trinket or weapon slot it also tries
uint8 CandidateSlots(Player* bot, PlayerbotAI* botAI, ItemTemplate const* proto, std::array<uint8, 2>& slots)
{
    uint8 const first = botAI->FindEquipSlot(proto, NULL_SLOT, true);
    if (first == NULL_SLOT || first >= EQUIPMENT_SLOT_END)
        return 0;

    slots[0] = first;
    slots[1] = first + 1;
    if (first == EQUIPMENT_SLOT_FINGER1 || first == EQUIPMENT_SLOT_TRINKET1)
        return 2;
    if (first != EQUIPMENT_SLOT_MAINHAND || !bot->CanDualWield())
        return 1;

    Item* const mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    bool const have2H = mainHand && mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
    bool const oneHand = proto->InventoryType != INVTYPE_2HWEAPON && proto->InventoryType != INVTYPE_WEAPONMAINHAND;
    if ((oneHand && !have2H) || (bot->CanTitanGrip() && IsTitanGripKind(proto)))
        return 2;
    return 1;
}

// The stats the calculator collected for its last item, "index:value" of the non-zero ones
std::string StatsText(StatsWeightCalculator const& calc)
{
    std::string text;
    float const* stats = calc.GetCollector()->stats;
    for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
    {
        if (stats[i] == 0.0f)
            continue;
        if (!text.empty())
            text += ',';
        text += Acore::StringFormat("{}:{:.3f}", i, stats[i]);
    }
    return text.empty() ? "-" : text;
}

std::string WeightsText(StatsWeightCalculator const& calc)
{
    std::string text;
    float const* weights = calc.GetStatWeights();
    for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
    {
        if (weights[i] == 0.0f)
            continue;
        if (!text.empty())
            text += ',';
        text += Acore::StringFormat("{}:{:.4f}", i, weights[i]);
    }
    return text.empty() ? "-" : text;
}

Worn ReadWorn(Player* bot, StatsWeightCalculator& calc, uint8 slot)
{
    Worn worn;
    worn.slot = slot;
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item)
        return worn;

    ItemTemplate const* proto = item->GetTemplate();
    worn.entry = proto->ItemId;
    worn.prop = item->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID);
    worn.score = calc.CalculateItem(worn.entry, worn.prop);
    worn.stats = StatsText(calc);
    worn.full = FullFits(proto, bot);
    worn.kind = KindFits(proto, bot, worn.full);
    worn.broken =
        item->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0 && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0;
    return worn;
}

// An empty candidate slot or a broken worn item (the rule takes both as empty), or a worn item the score beats by the
// threshold, a worn score at or below 0 counting as 0; never the same item with the same suffix
bool Passes(Row const& row, float threshold)
{
    for (uint8 i = 0; i < row.wornCount; ++i)
    {
        Worn const& worn = row.worn[i];
        if (!worn.entry || worn.broken)
            return true;
        if (worn.entry == row.proto->ItemId && worn.prop == row.prop)
            continue;
        if (row.score > std::max(worn.score, 0.0f) * threshold)
            return true;
    }
    return false;
}

// The rule's own test of a row: no gate refused it, it is of the bot's kind, its score is above 0 and a candidate
// slot passes
bool FitPasses(Row const& row, float threshold)
{
    return row.gate == Gate::Ok && row.kind && row.score > 0.0f && Passes(row, threshold);
}

// The invariants one row decides on its own (S6 and the copy half of S4 compare rows: SweepBot)
void FlagRow(Row& row, Bonus const& bonus, float threshold)
{
    bool const upgrade = IsUpgrade(row.usage);
    bool anyEmpty = false;
    bool anyBroken = false;
    bool wornPositive = false;
    bool wornTableOnly = false;
    bool sameEntryPass = false;
    for (uint8 i = 0; i < row.wornCount; ++i)
    {
        Worn const& worn = row.worn[i];
        if (!worn.entry)
        {
            anyEmpty = true;
            continue;
        }
        anyBroken = anyBroken || worn.broken;
        wornPositive = wornPositive || worn.score > 0.0f;
        wornTableOnly = wornTableOnly || (worn.kind && worn.score > 0.0f && !worn.full);
        if (worn.entry == row.proto->ItemId && worn.prop != row.prop &&
            row.score > std::max(worn.score, 0.0f) * threshold)
            sameEntryPass = true;
    }
    bool const occupied = row.wornCount && !anyEmpty;
    bool const positive = row.score > 0.0f;
    bool const passes = Passes(row, threshold);
    bool const fitPasses = FitPasses(row, threshold);

    // S1: an upgrade with a score at or below 0
    if (upgrade && !positive)
        row.flags |= 1u << FLAG_S1;
    // S2: an upgrade no candidate slot passes over a worn item the class stat table alone would call unfit
    if (upgrade && !passes && wornTableOnly)
        row.flags |= 1u << FLAG_S2;
    // S3: an upgrade over positive worn items that it does not beat by the threshold
    if (upgrade && occupied && !anyBroken && wornPositive && !passes)
        row.flags |= 1u << FLAG_S3;
    // S4: a suffix copy, or another suffix of a worn entry, that fits and passes but is no upgrade
    if (!upgrade && ((row.prop && fitPasses) || (row.gate == Gate::Ok && row.kind && positive && sameEntryPass)))
        row.flags |= 1u << FLAG_S4;
    // S5: an item of spell or rating bonuses only (no primary stat) that fits and passes but is no upgrade
    if (!bonus.primary && (bonus.spell || bonus.rating) && row.proto->Quality >= ITEM_QUALITY_UNCOMMON && fitPasses &&
        !upgrade)
        row.flags |= 1u << FLAG_S5;
    // X1: an upgrade of the wrong kind or with a score at or below 0
    if (upgrade && (!row.kind || !positive))
        row.flags |= 1u << FLAG_X1;
    if (row.usage == ITEM_USAGE_BAD_EQUIP && occupied)
        row.flags |= 1u << FLAG_X2;
}

std::string FlagsText(uint32 flags)
{
    std::string text;
    for (uint32 i = 0; i < FLAG_COUNT; ++i)
        if (flags & (1u << i))
            text += (text.empty() ? "" : ",") + std::string(FLAG_NAMES[i]);
    return text.empty() ? "-" : text;
}

std::string CountsText(Counts const& counts)
{
    std::ostringstream text;
    for (uint32 i = 0; i < FLAG_COUNT; ++i)
        text << (i ? " " : "") << FLAG_NAMES[i] << " " << counts[i];
    return text.str();
}

void Count(Counts& counts, uint32 flags)
{
    for (uint32 i = 0; i < FLAG_COUNT; ++i)
        if (flags & (1u << i))
            ++counts[i];
}

void WriteWorn(std::ostream& out, Worn const* worn)
{
    if (!worn)
    {
        out << "\t-\t-\t-\t-\t-\t-\t-";
        return;
    }
    out << '\t' << uint32(worn->slot) << '\t' << worn->entry << '\t' << worn->prop << '\t' << worn->score << '\t'
        << worn->full << '\t' << worn->kind << '\t' << worn->broken;
}

void WriteItem(std::ostream& out, ItemTemplate const* proto, int32 prop)
{
    out << '\t' << proto->ItemId << '\t' << prop << '\t' << proto->Quality << '\t' << proto->ItemLevel << '\t'
        << proto->RequiredLevel << '\t' << proto->Class << '\t' << proto->SubClass << '\t' << proto->InventoryType;
}

BotResult SweepBot(Player* bot, PlayerbotAI* botAI, std::vector<ItemTemplate const*> const& candidates,
                   std::ostream& out)
{
    BotResult result;
    uint32 const level = bot->GetLevel();
    float const threshold = sPlayerbotAIConfig.EquipUpgradeThreshold;
    StatsWeightCalculator calc(bot);
    calc.SetUpForEquipCompare();

    std::array<Worn, EQUIPMENT_SLOT_END> worn;
    uint32 wornCount = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        worn[slot] = ReadWorn(bot, calc, slot);
        if (worn[slot].entry)
            ++wornCount;
    }

    std::ostringstream prefix;
    prefix << bot->GetName() << '\t' << uint32(bot->getClass()) << '\t' << AiFactory::GetPlayerSpecName(bot) << '\t'
           << level << '\t' << sRandomPlayerbotMgr.IsSpecPvp(bot->GetGUID().GetCounter(), bot->getClass()) << '\t'
           << wornCount;
    std::string const head = prefix.str();

    // The bot's facts the score reads (the collector type as StatsWeightCalculator picks it) and its weights, for the
    // offline replay of every score and answer
    if (!candidates.empty())
    {
        calc.CalculateItem(candidates.front()->ItemId);
        CollectorType const type = PlayerbotAI::IsHeal(bot)     ? CollectorType::SPELL_HEAL
                                   : PlayerbotAI::IsCaster(bot) ? CollectorType::SPELL_DMG
                                   : PlayerbotAI::IsTank(bot)   ? CollectorType::MELEE_TANK
                                   : PlayerbotAI::IsMelee(bot)  ? CollectorType::MELEE_DMG
                                                                : CollectorType::RANGED;
        out << "B\t" << head << "\trace=" << uint32(bot->getRace(true))
            << "\ttab=" << uint32(AiFactory::GetPlayerSpecTab(bot)) << "\ttype=" << uint32(type)
            << "\tdw=" << bot->CanDualWield() << "\ttg=" << bot->CanTitanGrip()
            << "\tsword=" << bot->HasAura(SPELL_ROGUE_SWORD_SPECIALIZATION)
            << "\tpoleaxe=" << bot->HasAura(SPELL_POLEAXE_SPECIALIZATION)
            << "\tnerves=" << bot->HasAura(SPELL_NERVES_OF_COLD_STEEL) << "\tweights=" << WeightsText(calc) << '\n';
    }

    // The worn items, for the record (their invariant, S2, is read on the rows that compare against them)
    for (Worn const& item : worn)
    {
        if (!item.entry)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
        if (!proto)
            continue;
        uint32 const flags = 0;
        ++result.rows;
        out << "W\t" << head;
        WriteItem(out, proto, item.prop);
        out << "\t-\t" << item.score << "\t-\t" << item.full << '\t' << item.kind << '\t'
            << CauseOf(item.kind, item.full, item.score, item.prop, BonusOf(proto)) << '\t' << FlagsText(flags);
        WriteWorn(out, &item);
        WriteWorn(out, nullptr);
        out << '\t' << item.stats << '\n';
    }

    std::vector<Row> rows;
    uint32 const classMask = bot->getClassMask();
    for (ItemTemplate const* proto : candidates)
    {
        if (!InWindow(proto, level) || !(static_cast<uint32>(proto->AllowableClass) & classMask))
            continue;
        if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;

        Row base;
        base.proto = proto;
        base.full = FullFits(proto, bot);
        base.kind = KindFits(proto, bot, base.full);
        base.gate = GateOf(bot, botAI, proto);
        std::array<uint8, 2> slots = {NULL_SLOT, NULL_SLOT};
        base.wornCount = CandidateSlots(bot, botAI, proto, slots);
        for (uint8 i = 0; i < base.wornCount; ++i)
            base.worn[i] = slots[i] < EQUIPMENT_SLOT_END ? worn[slots[i]] : Worn{slots[i]};
        base.slotKey = base.wornCount ? slots[0] : NULL_SLOT;
        Bonus const bonus = BonusOf(proto);

        size_t const firstCopy = rows.size();
        for (int32 prop : PropsOf(proto))
        {
            Row row = base;
            row.prop = prop;
            row.score = calc.CalculateItem(proto->ItemId, prop);
            row.stats = StatsText(calc);
            row.usage = Ask(botAI, proto->ItemId, prop);
            row.cause = CauseOf(row.kind, row.full, row.score, prop, bonus);
            FlagRow(row, bonus, threshold);
            rows.push_back(row);
        }

        // S4: copies of one entry against the same worn items are answered in their score order
        for (size_t a = firstCopy; a < rows.size(); ++a)
            for (size_t b = firstCopy; b < rows.size(); ++b)
                if (rows[a].score > rows[b].score && IsUpgrade(rows[b].usage) && !IsUpgrade(rows[a].usage))
                    rows[a].flags |= 1u << FLAG_S4;
    }

    // S6: in each slot the rule's upgrades are the items with the highest scores among those that fit and beat the
    // worn item: no such item is left out while a lower-scoring one is taken
    std::map<uint8, float> lowestUpgrade;
    for (Row const& row : rows)
        if (IsUpgrade(row.usage))
        {
            auto const [it, added] = lowestUpgrade.emplace(row.slotKey, row.score);
            if (!added)
                it->second = std::min(it->second, row.score);
        }
    for (Row& row : rows)
    {
        if (IsUpgrade(row.usage) || !FitPasses(row, threshold))
            continue;
        auto const it = lowestUpgrade.find(row.slotKey);
        if (it != lowestUpgrade.end() && row.score > it->second)
            row.flags |= 1u << FLAG_S6;
    }

    for (Row const& row : rows)
    {
        Count(result.counts, row.flags);
        ++result.rows;
        out << "I\t" << head;
        WriteItem(out, row.proto, row.prop);
        out << '\t' << GateWord(row.gate) << '\t' << row.score << '\t' << UsageWord(row.usage) << '\t' << row.full
            << '\t' << row.kind << '\t' << row.cause << '\t' << FlagsText(row.flags);
        WriteWorn(out, row.wornCount > 0 ? &row.worn[0] : nullptr);
        WriteWorn(out, row.wornCount > 1 ? &row.worn[1] : nullptr);
        out << '\t' << row.stats << '\n';
    }
    return result;
}

// PLAN item 3 on a death knight of level 57-60: both cloaks' scores, fit tests and the rule's answers
void Pair(ChatHandler* handler, Player* bot, PlayerbotAI* botAI)
{
    if (bot->getClass() != CLASS_DEATH_KNIGHT || bot->GetLevel() < PAIR_MIN_LEVEL || bot->GetLevel() > PAIR_MAX_LEVEL)
        return;
    ItemTemplate const* starter = sObjectMgr->GetItemTemplate(PAIR_STARTER);
    ItemTemplate const* epic = sObjectMgr->GetItemTemplate(PAIR_EPIC);
    if (!starter || !epic)
        return;

    StatsWeightCalculator calc(bot);
    calc.SetUpForEquipCompare();
    Worn const back = ReadWorn(bot, calc, EQUIPMENT_SLOT_BACK);
    std::string const text = Acore::StringFormat(
        "gear sweep pair {} score {:.2f} full {} answer {}, {} score {:.2f} full {} answer {}, back {} score {:.2f}",
        PAIR_STARTER, calc.CalculateItem(PAIR_STARTER), FullFits(starter, bot), UsageWord(Ask(botAI, PAIR_STARTER, 0)),
        PAIR_EPIC, calc.CalculateItem(PAIR_EPIC), FullFits(epic, bot), UsageWord(Ask(botAI, PAIR_EPIC, 0)), back.entry,
        back.score);
    LOG_INFO("playerbots", "[Route] {} {}", bot->GetName(), text);
    handler->PSendSysMessage("GEAR {}: {}", bot->GetName(), text);
}

std::string Header()
{
    return "# gear sweep (gear plan WG1, WG2): the upgrade rule's answers over weapon and armour templates in the "
           "window "
           "[level-10, level] (required level, else item level - 5), the class allowed and the bot able to use them. "
           "type I: an item asked (prop: random property > 0, suffix < 0, first/middle/last of its pool); W: a worn "
           "item. gate: the rule's own refusals before scoring (ok, equip, unique, titangrip, create); score: "
           "StatsWeightCalculator with SetUpForEquipCompare; full: CanEquipWeapon/CanEquipArmor (the factory's test, "
           "with the class stat table); kind: CanEquipWeapon/CanEquipArmorKind (the rule's); cause: kind, table, "
           "suffix, ratings, spell, nostats (unfit), negative "
           "(fit, score <= 0); slotN..brokenN: the worn item of each slot the rule compares against (entry 0: "
           "empty); stats: index:value of the collected stats (StatsType). type B: per bot, the facts the score reads "
           "and "
           "its weights\n"
           "type\tbot\tcls\tspec\tlevel\tpvp\tworn\tentry\tprop\tquality\tilvl\treqlevel\ticlass\tsubclass\tinvtype\t"
           "gate\tscore\tanswer\tfull\tkind\tcause\tflags\tslot1\tentry1\tprop1\tscore1\tfull1\tkind1\tbroken1\tslot2\t"
           "entry2\tprop2\tscore2\tfull2\tkind2\tbroken2\tstats\n";
}
}  // namespace

bool HandleSweepCommand(ChatHandler* handler, char const* args)
{
    std::string name = args ? args : "";
    name.erase(0, name.find_first_not_of(" \t"));
    size_t const last = name.find_last_not_of(" \t\r\n");
    name.erase(last == std::string::npos ? 0 : last + 1);

    // One bot per call: every online bot at once held the world thread for minutes
    if (name.empty())
    {
        handler->SendErrorMessage("GEAR sweep: usage: playerbots gear sweep <bot>");
        return false;
    }
    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    if (!bot || !PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendErrorMessage("GEAR sweep: {} is not an online bot", name);
        return false;
    }

    bool const fresh = sweepFile.empty();
    if (fresh)
    {
        std::string const stamp = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y%m%d-%H%M%S");
        sweepFile = (std::filesystem::path(sLog->GetLogsDir()) / ("gear-sweep-" + stamp + ".tsv")).string();
    }
    std::ofstream file(sweepFile, std::ios::binary | (fresh ? std::ios::trunc : std::ios::app));
    if (!file)
    {
        handler->SendErrorMessage("GEAR sweep: cannot write {}", sweepFile);
        return false;
    }
    if (fresh)
        file << Header();

    std::vector<ItemTemplate const*> const candidates = Candidates();
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    Clock::time_point const started = Clock::now();
    Pair(handler, bot, botAI);

    std::ostringstream text;
    text << std::fixed << std::setprecision(2);
    BotResult const result = SweepBot(bot, botAI, candidates, text);
    file << text.str();

    long long const ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    std::string const counts = CountsText(result.counts);
    LOG_INFO("playerbots", "[Route] {} gear sweep done {} rows {} ms {}", bot->GetName(), counts, result.rows, ms);
    handler->PSendSysMessage("GEAR sweep {}: {} rows {} ms {} file {}", bot->GetName(), counts, result.rows, ms,
                             sweepFile);
    return true;
}
}  // namespace GearSweep
