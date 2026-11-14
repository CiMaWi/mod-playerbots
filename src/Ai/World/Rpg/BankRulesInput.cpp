/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankRulesInput.h"

#include <algorithm>
#include <limits>
#include <optional>

#include "BankMailState.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "MailRulesInput.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"

namespace BankMail
{
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

class ItemReader
{
public:
    explicit ItemReader(PlayerbotAI* botAI)
        : _botAI(botAI), _sellMultiplier(static_cast<float>(sRandomPlayerbotMgr.GetSellMultiplier(botAI->GetBot())))
    {
    }

    BankItemIn Read(Item const* item, uint8 bag, uint8 slot)
    {
        ItemTemplate const* proto = item->GetTemplate();
        BankItemIn in;
        in.bag = bag;
        in.slot = slot;
        KeepItemIn& keep = in.item;
        keep.itemGuid = item->GetGUID().GetCounter();
        keep.entry = proto->ItemId;
        keep.count = std::max<uint32>(item->GetCount(), 1);
        keep.stackMax = std::max<uint32>(proto->GetMaxStackSize(), 1);
        keep.requiredLevel = ClampLevel(proto->RequiredLevel);
        keep.sellNow = ItemUsageValue::ItemWorth(proto, ITEM_USAGE_VENDOR, keep.count, _sellMultiplier);
        keep.useWorth = WorldTravel::ItemWorth(WorldTravel::SpaceClass::Use, proto->SellPrice,
                                               static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0)), keep.count,
                                               _sellMultiplier);
        keep.bound = item->IsSoulBound();
        Player* bot = _botAI->GetBot();
        uint32 const dueLevel = std::max<uint32>(proto->RequiredLevel, bot->GetLevel());
        WornSlots const slots = SlotsFor(bot, proto, dueLevel);
        keep.wearable = slots.first != NULL_SLOT && IsWearable(bot, proto, dueLevel);
        if (!keep.wearable)
            return in;

        StatsWeightCalculator& calculator = Calculator();
        keep.score = calculator.CalculateItem(proto->ItemId, item->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
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

private:
    // Built at the first wearable item: its weights cost more than reading a bank of non-gear
    StatsWeightCalculator& Calculator()
    {
        if (!_calculator)
        {
            Player* bot = _botAI->GetBot();
            _calculator.emplace(bot);
            _calculator->SetItemSetBonus(false);
            _calculator->SetOverflowPenalty(false);
            if (sRandomPlayerbotMgr.IsSpecPvp(bot->GetGUID().GetCounter(), bot->getClass()))
                _calculator->SetPvpSpec(true);
        }
        return *_calculator;
    }

    PlayerbotAI* _botAI;
    float _sellMultiplier;
    std::optional<StatsWeightCalculator> _calculator;
};

// The 28 bank item slots only: bank bags and bought bag slots are not this plan's (WB.3)
void ReadBank(Player const* bot, ItemReader& reader, BankIn& in)
{
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
    {
        Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            ++in.bankSpace.freeSlots;
            continue;
        }
        in.bank.push_back(reader.Read(item, INVENTORY_SLOT_BAG_0, slot));
        KeepItemIn const& keep = in.bank.back().item;
        if (keep.stackMax <= keep.count)
            continue;
        auto stack = std::find_if(in.bankSpace.stackRoom.begin(), in.bankSpace.stackRoom.end(),
                                  [&keep](StackRoom const& room) { return room.entry == keep.entry; });
        if (stack == in.bankSpace.stackRoom.end())
            in.bankSpace.stackRoom.push_back({keep.entry, keep.stackMax - keep.count});
        else
            stack->room += keep.stackMax - keep.count;
    }
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

// Gear the bot may wear only above its level: the bag items that can be kept and not due (GearForLaterLevel)
bool MaybeKeptForLater(ItemTemplate const* proto, uint8 level)
{
    return (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) && proto->RequiredLevel > level;
}
}  // namespace

KeepBotIn BuildKeepBot(Player const* bot)
{
    KeepBotIn keepBot;
    keepBot.level = bot->GetLevel();
    keepBot.horizon = ClampLevel(sPlayerbotAIConfig.BankMail.KeepLevelHorizon);
    keepBot.upgradeThreshold = sPlayerbotAIConfig.EquipUpgradeThreshold;
    return keepBot;
}

BankIn BuildBankIn(PlayerbotAI* botAI)
{
    BankIn in;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return in;

    in.bot = BuildKeepBot(bot);
    ItemReader reader(botAI);
    ForEachBagItem(bot, [&in, &reader](Item const* item, uint8 bag, uint8 slot)
                   { in.bags.push_back(reader.Read(item, bag, slot)); });
    in.bagSpace = BuildBagSpace(bot);
    ReadBank(bot, reader, in);
    return in;
}

void RefreshBankKnowledge(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return;

    BankIn in;
    in.bot = BuildKeepBot(bot);
    ItemReader reader(botAI);
    ReadBank(bot, reader, in);

    BankKnowledge& known = botAI->rpgInfo.bankMail.bank;
    known.kept.clear();
    for (KeptEntry const& entry : KeptInBank(in))
        known.kept.push_back({entry.entry, entry.count, entry.laterWorth, entry.dueLevel});
    known.freeSlots = static_cast<uint16>(in.bankSpace.freeSlots);
    RefreshCarriedKeeps(botAI);
}

void RefreshCarriedKeeps(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return;

    CarriedKeeps& carried = botAI->rpgInfo.bankMail.bank.carried;
    carried = CarriedKeeps();
    if (!PlayerbotFactory::KeepsPossessions(bot))
        return;

    BankIn in;
    in.bot = BuildKeepBot(bot);
    std::optional<ItemReader> reader;  // most bags hold no gear above the bot's level
    uint8 const level = in.bot.level;
    ForEachBagItem(bot,
                   [&in, &reader, botAI, level](Item const* item, uint8 bag, uint8 slot)
                   {
                       if (!MaybeKeptForLater(item->GetTemplate(), level))
                           return;
                       if (!reader)
                           reader.emplace(botAI);
                       in.bags.push_back(reader->Read(item, bag, slot));
                   });
    carried = KeptInBags(in);
}

uint32_t CarriedWorthEach(PlayerbotAI* botAI, uint32_t entry)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return 0;
    return KeptWorthEach(botAI->rpgInfo.bankMail.bank.carried, entry, bot->GetLevel());
}
}  // namespace BankMail
