/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankRulesInput.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include "AuctionIndex.h"
#include "AuctionPricing.h"
#include "AuctionReference.h"
#include "AuctionRules.h"
#include "AuctionVisit.h"
#include "BankMailConfig.h"
#include "BankMailState.h"
#include "BotKind.h"
#include "BudgetValues.h"
#include "DBCStores.h"
#include "ItemDemand.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "MailRulesInput.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "ProfessionRules.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotMgr.h"
#include "SkillPathLive.h"
#include "StatsWeightCalculator.h"
#include "Upkeep.h"
#include "WorldTravelConfig.h"

namespace BankMail
{
// The item usage's numbers BankRules.h reads (WornByUsage)
static_assert(static_cast<uint8_t>(ITEM_USAGE_EQUIP) == USAGE_EQUIP &&
              static_cast<uint8_t>(ITEM_USAGE_REPLACE) == USAGE_REPLACE &&
              static_cast<uint8_t>(ITEM_USAGE_BAD_EQUIP) == USAGE_BAD_EQUIP);

namespace
{
uint8 ClampLevel(uint32 level)
{
    return static_cast<uint8>(std::min<uint32>(level, std::numeric_limits<uint8>::max()));
}

bool IsPlainBag(Bag const* bag)
{
    return bag && bag->GetTemplate()->Class == ITEM_CLASS_CONTAINER &&
           bag->GetTemplate()->SubClass == ITEM_SUBCLASS_CONTAINER;
}

// Bots learn dual wield (warriors, hunters) at 20 and plate or mail at 40 (PlayerbotFactory::InitSkills), as players
// learn them from their trainers
constexpr uint32 DUAL_WIELD_LEVEL = 20;
constexpr uint32 HEAVY_ARMOR_LEVEL = 40;

// The relic kind a class wears in its ranged slot, as Player::CanUseItem checks it (warlocks: the misc kind);
// MAX_ITEM_SUBCLASS_ARMOR (none) for the others
uint32 RelicSubClassFor(uint8 playerClass)
{
    switch (playerClass)
    {
        case CLASS_PALADIN:
            return ITEM_SUBCLASS_ARMOR_LIBRAM;
        case CLASS_DRUID:
            return ITEM_SUBCLASS_ARMOR_IDOL;
        case CLASS_SHAMAN:
            return ITEM_SUBCLASS_ARMOR_TOTEM;
        case CLASS_DEATH_KNIGHT:
            return ITEM_SUBCLASS_ARMOR_SIGIL;
        case CLASS_WARLOCK:
            return ITEM_SUBCLASS_ARMOR_MISC;
        default:
            return MAX_ITEM_SUBCLASS_ARMOR;
    }
}

// The heaviest armor kind a class wears at a level; it wears every lighter kind too
uint32 HeaviestArmorFor(uint8 playerClass, uint32 level)
{
    bool const heavy = level >= HEAVY_ARMOR_LEVEL;
    switch (playerClass)
    {
        case CLASS_DEATH_KNIGHT:
            return ITEM_SUBCLASS_ARMOR_PLATE;
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
            return heavy ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return heavy ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;
        case CLASS_ROGUE:
        case CLASS_DRUID:
            return ITEM_SUBCLASS_ARMOR_LEATHER;
        default:
            return ITEM_SUBCLASS_ARMOR_CLOTH;
    }
}

bool DualWieldsAt(Player const* bot, uint32 level)
{
    uint8 const playerClass = bot->getClass();
    return bot->CanDualWield() ||
           ((playerClass == CLASS_WARRIOR || playerClass == CLASS_HUNTER) && level >= DUAL_WIELD_LEVEL);
}

// Whether the equip logic would wear the item at `level` (its required level, or the bot's when higher), by the gates
// that stop an equip there: the core's masks and skills (Player::CanUseItem) as bots hold them at that level and the
// class's weapon kinds. No stat or exact armor kind gate: the item usage only labels with CanEquipArmor, the score
// decides, as it does here
bool IsWearable(Player const* bot, ItemTemplate const* proto, uint32 level)
{
    if (!(proto->AllowableClass & bot->getClassMask()) || !(proto->AllowableRace & bot->getRaceMask()))
        return false;
    if ((proto->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && bot->GetTeamId(true) != TEAM_HORDE) ||
        (proto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE) && bot->GetTeamId(true) != TEAM_ALLIANCE))
        return false;
    if (proto->RequiredSkill && !bot->HasSkill(proto->RequiredSkill))
        return false;

    if (proto->Class == ITEM_CLASS_WEAPON)
        return proto->SubClass != ITEM_SUBCLASS_WEAPON_MISC && sRandomItemMgr.CanEquipWeapon(proto, bot->getClass());
    if (proto->Class != ITEM_CLASS_ARMOR)
        return false;
    if (proto->InventoryType == INVTYPE_RELIC)
        return proto->SubClass == RelicSubClassFor(bot->getClass());

    uint8 const playerClass = bot->getClass();
    switch (proto->SubClass)
    {
        // Cloaks are cloth
        case ITEM_SUBCLASS_ARMOR_CLOTH:
        case ITEM_SUBCLASS_ARMOR_LEATHER:
        case ITEM_SUBCLASS_ARMOR_MAIL:
        case ITEM_SUBCLASS_ARMOR_PLATE:
            return proto->SubClass <= HeaviestArmorFor(playerClass, level);
        case ITEM_SUBCLASS_ARMOR_SHIELD:
            return playerClass == CLASS_WARRIOR || playerClass == CLASS_PALADIN || playerClass == CLASS_SHAMAN;
        // Necks, rings, trinkets, held off-hands, shirts and tabards: no armor skill, any class the masks allow
        default:
            return true;
    }
}

struct WornSlots
{
    uint8 first = NULL_SLOT;
    uint8 second = NULL_SLOT;
};

// The equipment slots the item would take at `level`, as the item usage compares them; unlike
// PlayerbotAI::FindEquipSlot this never unequips anything
WornSlots SlotsFor(Player const* bot, ItemTemplate const* proto, uint32 level)
{
    switch (proto->InventoryType)
    {
        case INVTYPE_HEAD:
            return {EQUIPMENT_SLOT_HEAD};
        case INVTYPE_NECK:
            return {EQUIPMENT_SLOT_NECK};
        case INVTYPE_SHOULDERS:
            return {EQUIPMENT_SLOT_SHOULDERS};
        case INVTYPE_BODY:
            return {EQUIPMENT_SLOT_BODY};
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            return {EQUIPMENT_SLOT_CHEST};
        case INVTYPE_WAIST:
            return {EQUIPMENT_SLOT_WAIST};
        case INVTYPE_LEGS:
            return {EQUIPMENT_SLOT_LEGS};
        case INVTYPE_FEET:
            return {EQUIPMENT_SLOT_FEET};
        case INVTYPE_WRISTS:
            return {EQUIPMENT_SLOT_WRISTS};
        case INVTYPE_HANDS:
            return {EQUIPMENT_SLOT_HANDS};
        case INVTYPE_CLOAK:
            return {EQUIPMENT_SLOT_BACK};
        case INVTYPE_TABARD:
            return {EQUIPMENT_SLOT_TABARD};
        case INVTYPE_FINGER:
            return {EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2};
        case INVTYPE_TRINKET:
            return {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2};
        case INVTYPE_WEAPON:
        {
            // A one-hander competes for the off hand too only beside a one-handed main hand
            Item const* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            bool const twoHanded = mainHand && mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
            if (DualWieldsAt(bot, level) && !twoHanded)
                return {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND};
            return {EQUIPMENT_SLOT_MAINHAND};
        }
        case INVTYPE_2HWEAPON:
        case INVTYPE_WEAPONMAINHAND:
            return {EQUIPMENT_SLOT_MAINHAND};
        case INVTYPE_WEAPONOFFHAND:
            // An off-hand weapon only for a bot that dual-wields (PlayerbotAI::CanEquipItem)
            if (!DualWieldsAt(bot, level))
                return {};
            [[fallthrough]];
        case INVTYPE_SHIELD:
        case INVTYPE_HOLDABLE:
            // Never beside a two-hander, Titan Grip exempt as in PlayerbotAI::CanEquipItem (IsTwoHandUsed is false
            // under it)
            if (bot->IsTwoHandUsed())
                return {};
            return {EQUIPMENT_SLOT_OFFHAND};
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
        case INVTYPE_RELIC:
            return {EQUIPMENT_SLOT_RANGED};
        default:
            return {};
    }
}

// Auction house WA3a: what the item usage may sell at the auction house (its ITEM_USAGE_AH precondition but the
// soulbound test), demand's consumables apart: the bag items auction stock (AuctionStock) may hold
bool MaybeAuctionStock(ItemTemplate const* proto)
{
    return proto->SellPrice > 0 && proto->Quality >= ITEM_QUALITY_NORMAL && proto->Bonding != BIND_WHEN_PICKED_UP &&
           proto->Class != ITEM_CLASS_CONSUMABLE;
}

// V for units of the item (WA.17), read as HandleSellItemOpcode reads it. A worn item the core finds no cost row for
// is refused by the vendor, so a vendor pays nothing for it
uint32 VendorPayoutOf(Item const* item, ItemTemplate const* proto, uint32 units)
{
    uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    uint32 const lost = maxDurability ? maxDurability - item->GetUInt32Value(ITEM_FIELD_DURABILITY) : 0;
    if (!lost)
        return AuctionRules::VendorPayout(proto->SellPrice, units, 0, 0, 0.0f);
    DurabilityCostsEntry const* costs = sDurabilityCostsStore.LookupEntry(proto->ItemLevel);
    DurabilityQualityEntry const* quality = sDurabilityQualityStore.LookupEntry((proto->Quality + 1) * 2);
    if (!costs || !quality)
        return 0;
    uint32 const multiplier = costs->multiplier[ItemSubClassToDurabilityMultiplierId(proto->Class, proto->SubClass)];
    return AuctionRules::VendorPayout(proto->SellPrice, units, lost, multiplier, quality->quality_mod);
}

// R6: the core's own deposit test for a bag item, room apart (the model's): the word the item stays in the bags with
std::optional<BankReason> DepositBlockOf(Player const* bot, uint8 bag, uint8 slot)
{
    ItemPosCountVec dest;
    switch (bot->CanBankItem(NULL_BAG, NULL_SLOT, dest, bot->GetItemByPos(bag, slot), false))
    {
        case EQUIP_ERR_OK:
        case EQUIP_ERR_BANK_FULL:
            return std::nullopt;
        case EQUIP_ERR_ALREADY_LOOTED:
            return BankReason::BlockedLooted;
        case EQUIP_ERR_DONT_OWN_THAT_ITEM:
            return BankReason::BlockedNotOwned;
        case EQUIP_ERR_CANT_CARRY_MORE_OF_THIS:
        case EQUIP_ERR_ITEM_MAX_LIMIT_CATEGORY_COUNT_EXCEEDED:
            return BankReason::BlockedLimit;
        default:
            return BankReason::BlockedOther;
    }
}

class ItemReader
{
public:
    explicit ItemReader(PlayerbotAI* botAI)
        : _botAI(botAI),
          _sellMultiplier(static_cast<float>(sRandomPlayerbotMgr.GetSellMultiplier(botAI->GetBot()))),
          _path(botAI->KeepsPossessions() ? Planner::KeptPath(botAI) : nullptr),
          _reference(botAI->KeepsPossessions() ? AuctionReference::Current() : nullptr),
          _index(_reference ? AuctionIndex::Current() : nullptr),
          _fees(AuctionReference::HouseFees(botAI->GetBot()->GetTeamId() == TEAM_ALLIANCE ? AuctionRules::HOUSE_ALLIANCE
                                                                                          : AuctionRules::HOUSE_HORDE))
    {
    }

    BankItemIn Read(Item const* item, uint8 bag, uint8 slot)
    {
        ItemTemplate const* proto = item->GetTemplate();
        int32 const randomPropertyId = item->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID);
        BankItemIn in;
        in.bag = bag;
        in.slot = slot;
        KeepItemIn& keep = in.item;
        keep.itemGuid = item->GetGUID().GetCounter();
        keep.entry = proto->ItemId;
        keep.randomPropertyId = randomPropertyId;
        keep.count = std::max<uint32>(item->GetCount(), 1);
        keep.stackMax = std::max<uint32>(proto->GetMaxStackSize(), 1);
        keep.requiredLevel = ClampLevel(proto->RequiredLevel);
        keep.sellNow = ItemUsageValue::ItemWorth(proto, ITEM_USAGE_VENDOR, keep.count, _sellMultiplier);
        // A consumable demand lists is worth demand's worth per unit to the bot (item use WI9b)
        uint32 const demandEach =
            proto->Class == ITEM_CLASS_CONSUMABLE ? ItemUse::DemandWorthEach(_botAI, proto->ItemId) : 0;
        keep.useWorth = WorldTravel::ItemWorth(WorldTravel::SpaceClass::Use, proto->SellPrice,
                                               static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0)), keep.count,
                                               _sellMultiplier, 0, demandEach);
        Player* bot = _botAI->GetBot();
        ReadProfession(proto, keep);
        ReadAuction(item, proto, keep);
        uint32 const dueLevel = std::max<uint32>(proto->RequiredLevel, bot->GetLevel());
        WornSlots const slots = SlotsFor(bot, proto, dueLevel);
        // Gear of the bot's level or below: the item usage answers whether the bot wears it now, the answer the equip
        // action reads (CS29b); the prediction below answers only for a later level
        if (slots.first != NULL_SLOT && proto->RequiredLevel <= bot->GetLevel())
        {
            keep.wearAnswer = WearAnswer::ItemUsage;
            keep.usage = static_cast<uint8_t>(UsageOf("item upgrade", proto->ItemId, randomPropertyId));
            return in;
        }
        keep.wearable = slots.first != NULL_SLOT && IsWearable(bot, proto, dueLevel);
        if (!keep.wearable)
            return in;

        StatsWeightCalculator& calculator = Calculator();
        keep.score = calculator.CalculateItem(proto->ItemId, randomPropertyId);
        bool scored = false;
        for (uint8 wornSlot : {slots.first, slots.second})
        {
            if (wornSlot == NULL_SLOT)
                continue;
            Item const* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, wornSlot);
            if (!worn)
            {
                keep.slotEmpty = true;
                continue;
            }
            // A second copy of a worn unique-equipped ring or trinket is never worn (the item usage refuses it)
            if (worn->GetEntry() == proto->ItemId && proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE))
            {
                keep.wearable = false;
                keep.slotEmpty = false;
                return in;
            }
            // Against the worse of two worn rings, trinkets or one-handers: the one it would replace
            float const score =
                calculator.CalculateItem(worn->GetEntry(), worn->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
            keep.wornScore = scored ? std::min(keep.wornScore, score) : score;
            scored = true;
        }
        return in;
    }

    // JudgeStock's market (NR12a): what listing a lot of the entry is worth (AuctionStock) after extraSteps more failed
    // auctions (NR.8's margin), read away from the auctioneer: R1, never the cheapest standing buyout
    uint32_t Worth(KeepItemIn const& first, uint32_t units, uint32_t payout, uint32_t extraSteps) const
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(first.entry);
        if (!_reference || !proto)
            return 0;
        AuctionRules::AskInput in;
        in.count = units;
        in.sellPrice = proto->SellPrice;
        in.vendorPayout = payout;
        if (AuctionRules::ItemReference const* row = _reference->Find(proto->ItemId))
            in.reference = row->copper;
        if (_index)
        {
            AuctionIndex::Evidence const evidence = _index->EvidenceOf(
                proto->ItemId,
                AuctionIndex::KindOf(proto->Class, proto->Quality, proto->RequiredLevel, proto->ItemLevel));
            in.typical = evidence.typical;
            in.itemRecord = evidence.item;
            in.kindRecord = evidence.kind;
        }
        in.fees = _fees;
        // WA3b: the seller's expiry memory; an item expired past the relist limit is held, so it is no stock any more
        AuctionMemory::ItemMemory const memory =
            AuctionMemory::Of(_botAI->GetBot()->GetGUID().GetCounter(), proto->ItemId);
        in.expiredSteps = memory.expiredSteps + extraSteps;
        in.quickSale = memory.quickSale && !extraSteps;
        return AuctionRules::StockWorth(in, sPlayerbotAIConfig.Auction);
    }

    // The item usage's AH label with the bank's units counted, so the visit's own moves never flip it (NR11 A), and
    // the units its needed-by-count branch keeps carried. It answers without the keep inside RefreshCarriedKeeps,
    // whose bags' memo is empty then, and at a bank read it labels stock kept already ITEM_USAGE_BANK
    StockUsage Usage(KeepItemIn const& first) const
    {
        std::string const qualifier = std::to_string(first.entry) + "," + std::to_string(first.randomPropertyId) +
                                      ItemUsageValue::BANK_COUNT_QUALIFIER;
        ItemUsage const usage = _botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier)->Get();
        StockUsage answer;
        answer.usage = static_cast<uint8_t>(usage);
        answer.auctionable = usage == ITEM_USAGE_AH || usage == ITEM_USAGE_BANK;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(first.entry);
        if (answer.auctionable && proto && !IsRealPlayer(_botAI->GetMaster()) &&
            ItemUsageValue(_botAI).KeepsByCount(proto, _path))
            answer.countKeep = ItemUsageValue::KEPT_STACKS * std::max<uint32>(proto->GetMaxStackSize(), 1);
        return answer;
    }

private:
    // Professions WR12: the path's material of the item, and a recipe item's profession, rank and whether it is known
    void ReadProfession(ItemTemplate const* proto, KeepItemIn& keep) const
    {
        if (SkillPath::Material const* material = _path ? SkillPath::MaterialOf(*_path, proto->ItemId) : nullptr)
        {
            keep.materialUsed = material->used;
            keep.materialCopperEach = material->copperEach;
            keep.materialSkill = material->skill;
            keep.materialDueSkill = material->dueSkill;
        }
        if (proto->Class == ITEM_CLASS_RECIPE)
        {
            keep.recipeSkill = static_cast<uint16_t>(proto->RequiredSkill);
            keep.recipeRank = static_cast<uint16_t>(std::min<uint32>(proto->RequiredSkillRank, UINT16_MAX));
            // A recipe's taught spell is its second spell (the first is the learning cast, 483)
            keep.recipeKnown = _botAI->GetBot()->HasSpell(proto->Spells[1].SpellId);
        }
    }

    // Auction house WA3a: a stack auction stock may hold and what a vendor pays for its stock units now; its entry's
    // lot is judged by JudgeStock (needs and errands NR12a), with Worth and Usage below
    void ReadAuction(Item const* item, ItemTemplate const* proto, KeepItemIn& keep) const
    {
        uint32 const units = StockUnits(keep);
        if (!_reference || !units || !MaybeAuctionStock(proto) || item->IsSoulBound())
            return;
        keep.stockCandidate = true;
        keep.vendorPayout = VendorPayoutOf(item, proto, units);
    }

    // An item usage value's answer for the item now ("item upgrade" is the one the equip action asks, EquipAction.cpp;
    // "item usage" the sale's), qualified by the entry and the random property when it has one
    ItemUsage UsageOf(char const* value, uint32 entry, int32 randomPropertyId) const
    {
        std::string qualifier = std::to_string(entry);
        if (randomPropertyId != 0)
            qualifier += "," + std::to_string(randomPropertyId);
        return _botAI->GetAiObjectContext()->GetValue<ItemUsage>(value, qualifier)->Get();
    }

    // Built at the first wearable item: its weights cost more than reading a bank of non-gear
    StatsWeightCalculator& Calculator()
    {
        if (!_calculator)
        {
            _calculator.emplace(_botAI->GetBot());
            _calculator->SetUpForEquipCompare();
        }
        return *_calculator;
    }

    PlayerbotAI* _botAI;
    float _sellMultiplier;
    SkillPath::Path const* _path;  // the path the bot keeps, null for a bot without one
    // Auction house WA3a: the reference table (null until built, or for a bot that keeps no possessions: no stock),
    // the index's snapshot and the fees of the bot's faction house, taken once per read
    std::shared_ptr<AuctionRules::Table const> _reference;
    std::shared_ptr<AuctionIndex::Snapshot const> _index;
    AuctionRules::Fees _fees;
    std::optional<StatsWeightCalculator> _calculator;
};

// Every slot of the bank's 28 item slots and of its bags (professions WR17), empty or not, with its bag and slot;
// plain: the slot is an item slot or lies in a plain bag (a special bag takes only its kind, so its room is not
// counted)
template <typename Visit>
void ForEachBankSlot(Player const* bot, Visit visit)
{
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
        visit(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot, true);
    for (uint8 bagSlot = BANK_SLOT_BAG_START; bagSlot < BANK_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;
        bool const plain = IsPlainBag(bag);
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            visit(bag->GetItemByPos(static_cast<uint8>(slot)), bagSlot, static_cast<uint8>(slot), plain);
    }
}

// The bank's item slots and its bags (professions WR17: the bank bags count; a deposit fills them, CanBankItem)
void ReadBank(Player const* bot, ItemReader& reader, BankIn& in)
{
    in.bankSlots = 0;
    ForEachBankSlot(bot,
                    [&in, &reader](Item const* item, uint8 bag, uint8 slot, bool plain)
                    {
                        if (plain)
                            ++in.bankSlots;
                        if (!item)
                        {
                            if (plain)
                                ++in.bankSpace.freeSlots;
                            return;
                        }
                        in.bank.push_back(reader.Read(item, bag, slot));
                        KeepItemIn const& keep = in.bank.back().item;
                        if (keep.stackMax <= keep.count)
                            return;
                        auto stack = std::find_if(in.bankSpace.stackRoom.begin(), in.bankSpace.stackRoom.end(),
                                                  [&keep](StackRoom const& room) { return room.entry == keep.entry; });
                        if (stack == in.bankSpace.stackRoom.end())
                            in.bankSpace.stackRoom.push_back({keep.entry, keep.stackMax - keep.count});
                        else
                            stack->room += keep.stackMax - keep.count;
                    });
}

// The bank's consumables (item use WI9b: the stacks demand's use stock may lie on), the bank bags' too
void ReadBankConsumables(Player const* bot, ItemReader& reader, std::vector<BankItemIn>& bank)
{
    ForEachBankSlot(bot,
                    [&bank, &reader](Item const* item, uint8 bag, uint8 slot, bool /*plain*/)
                    {
                        if (item && item->GetTemplate()->Class == ITEM_CLASS_CONSUMABLE)
                            bank.push_back(reader.Read(item, bag, slot));
                    });
}

// The bank's stacks of the entries the bags hold as possible auction stock, so each lot is judged as at the bank
// (needs and errands NR12a, JudgeStock)
void ReadBankStock(Player const* bot, ItemReader& reader, std::vector<BankItemIn> const& bags,
                   std::vector<BankItemIn>& bank)
{
    std::vector<uint32> entries;
    for (BankItemIn const& carried : bags)
        if (carried.item.stockCandidate &&
            std::find(entries.begin(), entries.end(), carried.item.entry) == entries.end())
            entries.push_back(carried.item.entry);
    if (entries.empty())
        return;
    ForEachBankSlot(bot,
                    [&bank, &reader, &entries](Item const* item, uint8 bag, uint8 slot, bool /*plain*/)
                    {
                        if (item && std::find(entries.begin(), entries.end(), item->GetEntry()) != entries.end())
                            bank.push_back(reader.Read(item, bag, slot));
                    });
}

// Demand's use stock on the stacks (item use WI9b, AssignStock): a fresh demand over the bags and the bank, for a
// persistent bot not granted its consumables (WI.9). An entry's carried units are demand's kept units; only an entry
// with carried units (one the bot still uses) has stock for later, bounded per slot by BANK_STOCK_SHARE wanted stocks
void FillStock(PlayerbotAI* botAI, std::vector<BankItemIn>& bags, std::vector<BankItemIn>& bank)
{
    Player* bot = botAI->GetBot();
    if (!ItemUse::DemandAnswers(botAI) || Upkeep::Granted(bot, UpkeepKind::Consumable))
        return;

    ItemUse::DemandAnswer const answer = ItemUse::BuildDemand(botAI, bot, true).answer;
    std::vector<StockEntry> entries;
    for (ItemUse::DemandKeep const& keep : answer.keeps)
    {
        if (keep.verdict == ItemUse::DemandVerdict::Unusable || keep.verdict == ItemUse::DemandVerdict::None ||
            !keep.worthCopperEach)
            continue;
        // An entry in two slots (one item, two uses) holds the same units: the larger keep and the higher worth
        uint8 const slot = static_cast<uint8>(keep.slot);
        auto same = std::find_if(entries.begin(), entries.end(),
                                 [&keep](StockEntry const& entry) { return entry.entry == keep.entry; });
        if (same == entries.end())
        {
            entries.push_back({keep.entry, keep.worthCopperEach, keep.keepUnits, slot});
            continue;
        }
        same->worthEach = std::max(same->worthEach, keep.worthCopperEach);
        if (keep.keepUnits > same->carryUnits)
        {
            same->carryUnits = keep.keepUnits;
            same->slot = slot;
        }
    }
    if (entries.empty())
        return;

    std::array<uint32_t, ItemUse::DEMAND_SLOT_COUNT> later{};
    for (size_t s = 0; s < later.size(); ++s)
        later[s] = static_cast<uint32_t>(
            std::min(std::ceil(answer.lines[s].wantedStock * static_cast<double>(BANK_STOCK_SHARE)), 1000000.0));
    AssignStock(entries, later, bags, bank);
}

// Every item in the backpack and the plain bags, with its bag and slot
template <typename Visit>
void ForEachBagItem(Player const* bot, Visit visit)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            visit(item, INVENTORY_SLOT_BAG_0, slot);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (!IsPlainBag(bag))
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (Item const* item = bag->GetItemByPos(static_cast<uint8>(slot)))
                visit(item, bagSlot, static_cast<uint8>(slot));
    }
}

// Professions WR17: what the bank-space rule reads (BankRules.h, DecideBankSpace): the bank bag slots bought and
// filled, the next slot's price from the core's table (WorldSession::HandleBuyBankSlotOpcode reads the same), the
// carried empty plain bag with the most slots (the cheaper on a tie) and the money above the reserve, as a bag buy
// reads it (BuyAction.cpp, ShouldBuyVendorBag)
void ReadBankSpace(PlayerbotAI* botAI, Player const* bot, BankIn& in)
{
    BankSpaceIn& space = in.space;
    space.bought = std::min<uint32>(bot->GetBankBagSlotCount(), BANK_BAG_SLOTS);
    for (uint32 i = 0; i < space.bought; ++i)
        if (bot->GetBagByPos(static_cast<uint8>(BANK_SLOT_BAG_START + i)))
            ++space.filled;
    if (space.bought < BANK_BAG_SLOTS)
        if (BankBagSlotPricesEntry const* entry = sBankBagSlotPricesStore.LookupEntry(space.bought + 1))
            space.nextPrice = entry->price;

    ForEachBagItem(bot,
                   [&space](Item const* item, uint8 bag, uint8 slot)
                   {
                       Bag const* carried = item->ToBag();
                       if (!IsPlainBag(carried) || !carried->IsEmpty())
                           return;
                       uint32 const slots = carried->GetBagSize();
                       uint32 const sell = item->GetTemplate()->SellPrice;
                       if (slots > space.bag.slots || (slots && slots == space.bag.slots && sell < space.bag.sellNow))
                           space.bag = {bag, slot, item->GetGUID().GetCounter(), item->GetEntry(), slots, sell};
                   });

    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 const total = AI_VALUE2(uint32, "total money needed for", uint32(NeedMoneyFor::gear));
    uint32 const gear = AI_VALUE2(uint32, "money needed for", uint32(NeedMoneyFor::gear));
    space.gold = bot->GetMoney();
    space.reserve = total > gear ? total - gear : 0;  // everything saved before gear, as a bag buy saves it
    space.thriftPercent = sPlayerbotAIConfig.WorldTravel.BagThriftPercent;
    space.budgetPercent = sPlayerbotAIConfig.WorldTravel.BagBudgetPercent;
}

// What the bank rule may keep and the bot cannot use yet: gear the bot may wear only above its level
// (GearForLaterLevel), a recipe item (RecipeForLater), a material of the bot's skill path (PathMaterial) and, while the
// reference table is built, what may be auction stock (AuctionStock); the bag items that can be kept and not due
bool MaybeKeptForLater(ItemTemplate const* proto, uint8 level, SkillPath::Path const* path, bool auctions)
{
    if (auctions && MaybeAuctionStock(proto))
        return true;
    if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
        return proto->RequiredLevel > level;
    if (proto->Class == ITEM_CLASS_RECIPE)
        return proto->RequiredSkill != 0;
    return path && SkillPath::MaterialOf(*path, proto->ItemId);
}
}  // namespace

uint32_t AuctionVendorPayout(Item const* item, uint32_t units)
{
    return VendorPayoutOf(item, item->GetTemplate(), units);
}

KeepBotIn BuildKeepBot(Player const* bot)
{
    KeepBotIn keepBot;
    keepBot.level = bot->GetLevel();
    keepBot.horizon = ClampLevel(sPlayerbotAIConfig.BankMail.KeepLevelHorizon);
    keepBot.upgradeThreshold = sPlayerbotAIConfig.EquipUpgradeThreshold;
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        if (uint16 const value = bot->GetSkillValue(static_cast<uint32>(info.Which)))
            keepBot.skills.push_back({static_cast<uint16_t>(info.Which), value});
    return keepBot;
}

BankIn BuildBankIn(PlayerbotAI* botAI)
{
    BankIn in;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return in;

    // Scans per bank visit: the row counts a full read of the bags and the bank while a bank visit runs
    PerfMonitorScope scope(VisitRunsOf(botAI->bankMail.visit, VisitKind::Bank) && PerfMonitor::IsEnabled()
                               ? sPerfMonitor.acquire(PERF_MON_TOTAL, "bank visit scan")
                               : nullptr);
    in.bot = BuildKeepBot(bot);
    ItemReader reader(botAI);
    ForEachBagItem(bot,
                   [&in, &reader, bot](Item const* item, uint8 bag, uint8 slot)
                   {
                       in.bags.push_back(reader.Read(item, bag, slot));
                       in.bags.back().item.depositBlock = DepositBlockOf(bot, bag, slot);
                   });
    in.bagSpace = BuildBagSpace(bot);
    ReadBank(bot, reader, in);
    ReadBankSpace(botAI, bot, in);
    JudgeStock(in.bags, in.bank, reader);
    FillStock(botAI, in.bags, in.bank);
    return in;
}

void RefreshBankKnowledge(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return;

    PerfMonitorScope scope(VisitRunsOf(botAI->bankMail.visit, VisitKind::Bank) && PerfMonitor::IsEnabled()
                               ? sPerfMonitor.acquire(PERF_MON_TOTAL, "bank visit refresh")
                               : nullptr);
    BankIn in;
    in.bot = BuildKeepBot(bot);
    ItemReader reader(botAI);
    ReadBank(bot, reader, in);

    BankKnowledge& known = botAI->bankMail.bank;
    known.kept.clear();
    for (KeptEntry const& entry : KeptInBank(in))
        known.kept.push_back({entry.entry, entry.count, entry.laterWorth, entry.dueLevel});
    known.freeSlots = static_cast<uint16>(in.bankSpace.freeSlots);
    known.slots = static_cast<uint16>(in.bankSlots);
    RefreshCarriedKeeps(botAI);
}

void RefreshCarriedKeeps(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return;

    CarriedKeeps& carried = botAI->bankMail.bank.carried;
    carried = CarriedKeeps();
    if (!BotKind::KeepsPossessions(bot))
        return;

    BankIn in;
    in.bot = BuildKeepBot(bot);
    std::optional<ItemReader> reader;  // most bags hold no gear above the bot's level
    uint8 const level = in.bot.level;
    SkillPath::Path const* const path = Planner::KeptPath(botAI);
    bool const auctions = AuctionReference::Current() != nullptr;
    bool surplus = false;
    ForEachBagItem(bot,
                   [&in, &reader, &surplus, botAI, bot, level, path, auctions](Item const* item, uint8 bag, uint8 slot)
                   {
                       ItemTemplate const* proto = item->GetTemplate();
                       if (proto->Class == ITEM_CLASS_CONSUMABLE)
                       {
                           surplus = surplus ||
                                     ItemUse::DemandVerdictOf(botAI, proto->ItemId) == ItemUse::DemandVerdict::Surplus;
                           return;
                       }
                       if (!MaybeKeptForLater(proto, level, path, auctions))
                           return;
                       if (!reader)
                           reader.emplace(botAI);
                       in.bags.push_back(reader->Read(item, bag, slot));
                       in.bags.back().item.depositBlock = DepositBlockOf(bot, bag, slot);
                   });
    // A surplus use item may be stock for later (item use WI9b): every consumable of the bags and the bank takes
    // part, since a slot's share is counted over all of them as at the bank
    if (surplus)
    {
        if (!reader)
            reader.emplace(botAI);
        ForEachBagItem(bot,
                       [&in, &reader](Item const* item, uint8 bag, uint8 slot)
                       {
                           if (item->GetTemplate()->Class == ITEM_CLASS_CONSUMABLE)
                               in.bags.push_back(reader->Read(item, bag, slot));
                       });
        std::vector<BankItemIn> bank;
        ReadBankConsumables(bot, *reader, bank);
        FillStock(botAI, in.bags, bank);
    }
    if (reader)
    {
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "bank keeps stock read")
                                                        : nullptr);
        std::vector<BankItemIn> bank;
        ReadBankStock(bot, *reader, in.bags, bank);
        JudgeStock(in.bags, bank, *reader);
    }
    carried = KeptInBags(in);
}

uint32_t CarriedWorthEach(PlayerbotAI* botAI, uint32_t entry)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return 0;
    return KeptWorthEach(botAI->bankMail.bank.carried, entry, bot->GetLevel());
}
}  // namespace BankMail
