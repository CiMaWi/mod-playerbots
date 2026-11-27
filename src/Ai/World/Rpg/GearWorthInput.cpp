/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GearWorthInput.h"

#include <algorithm>
#include <array>

#include "AiObjectContext.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "StatsWeightCalculator.h"
#include "WorldTravelConfig.h"

namespace
{
bool IsTitanGripWeapon(ItemTemplate const* proto)
{
    return proto->InventoryType == INVTYPE_2HWEAPON &&
           (proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 || proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
            proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
}

constexpr uint32 SlotBit(uint8 slot) { return 1u << slot; }

// The worn items by equipment slot, and their scores
struct Worn
{
    std::array<ItemTemplate const*, EQUIPMENT_SLOT_END> proto{};
    std::array<float, EQUIPMENT_SLOT_END> score{};
};

// What wearing the item takes: the slot it replaces, the slots it fills and the slots whose item stops being worn
struct SlotChoice
{
    uint8 slot{0};
    uint32 fills{0};
    uint32 leaves{0};
};

SlotChoice Single(uint8 slot) { return {slot, SlotBit(slot), SlotBit(slot)}; }

// A ring or trinket: the empty slot of the pair, else the one with the lower score
SlotChoice OfPair(uint8 first, Worn const& worn)
{
    uint8 const second = first + 1;
    if (!worn.proto[first])
        return Single(first);
    if (!worn.proto[second] || worn.score[second] < worn.score[first])
        return Single(second);
    return Single(first);
}

// A weapon a dual wielder may put in the main hand, as the equip pass places it: one beating the main hand goes
// there, and the old main hand moves on to the off hand when it can go there and beats what is worn there (both hands
// change then); else a weapon that can go to the off hand goes there
SlotChoice OfDualWield(Player* bot, ItemTemplate const* proto, float itemScore, Worn const& worn)
{
    ItemTemplate const* mainHand = worn.proto[EQUIPMENT_SLOT_MAINHAND];
    if (!mainHand)
        return Single(EQUIPMENT_SLOT_MAINHAND);

    float const mainScore = worn.score[EQUIPMENT_SLOT_MAINHAND];
    if (!ItemUsageValue::BeatsWornScore(itemScore, mainScore))
        return Single(proto->InventoryType == INVTYPE_WEAPONMAINHAND ? uint8(EQUIPMENT_SLOT_MAINHAND)
                                                                     : uint8(EQUIPMENT_SLOT_OFFHAND));

    bool const mainCanGoOff = mainHand->InventoryType == INVTYPE_WEAPON ||
                              mainHand->InventoryType == INVTYPE_WEAPONOFFHAND ||
                              (bot->CanTitanGrip() && IsTitanGripWeapon(mainHand));
    bool const moves = mainCanGoOff && (!worn.proto[EQUIPMENT_SLOT_OFFHAND] ||
                                        ItemUsageValue::BeatsWornScore(mainScore, worn.score[EQUIPMENT_SLOT_OFFHAND]));
    if (!moves)
        return Single(EQUIPMENT_SLOT_MAINHAND);

    uint32 const bothHands = SlotBit(EQUIPMENT_SLOT_MAINHAND) | SlotBit(EQUIPMENT_SLOT_OFFHAND);
    return {EQUIPMENT_SLOT_OFFHAND, bothHands, SlotBit(EQUIPMENT_SLOT_OFFHAND)};
}

SlotChoice ChooseSlot(Player* bot, ItemTemplate const* proto, uint8 dstSlot, float itemScore, Worn const& worn)
{
    if (dstSlot == EQUIPMENT_SLOT_FINGER1 || dstSlot == EQUIPMENT_SLOT_FINGER2)
        return OfPair(EQUIPMENT_SLOT_FINGER1, worn);
    if (dstSlot == EQUIPMENT_SLOT_TRINKET1 || dstSlot == EQUIPMENT_SLOT_TRINKET2)
        return OfPair(EQUIPMENT_SLOT_TRINKET1, worn);
    if (dstSlot != EQUIPMENT_SLOT_MAINHAND && dstSlot != EQUIPMENT_SLOT_OFFHAND)
        return Single(dstSlot);

    bool const titanGrip = bot->CanTitanGrip();
    bool const twoHander = proto->InventoryType == INVTYPE_2HWEAPON;
    uint32 const bothHands = SlotBit(EQUIPMENT_SLOT_MAINHAND) | SlotBit(EQUIPMENT_SLOT_OFFHAND);
    if (twoHander && !(titanGrip && IsTitanGripWeapon(proto)))
        return {EQUIPMENT_SLOT_MAINHAND, bothHands, bothHands};

    // A one-hander or an off-hand item takes a worn two-hander's place
    ItemTemplate const* mainHand = worn.proto[EQUIPMENT_SLOT_MAINHAND];
    if (!titanGrip && mainHand && mainHand->InventoryType == INVTYPE_2HWEAPON)
        return {EQUIPMENT_SLOT_MAINHAND, SlotBit(dstSlot), SlotBit(EQUIPMENT_SLOT_MAINHAND)};

    // Shields, held items and off-hand weapons
    if (proto->InventoryType != INVTYPE_WEAPON && proto->InventoryType != INVTYPE_WEAPONMAINHAND && !twoHander)
        return Single(EQUIPMENT_SLOT_OFFHAND);
    if (!bot->CanDualWield())
        return Single(EQUIPMENT_SLOT_MAINHAND);
    return OfDualWield(bot, proto, itemScore, worn);
}
}  // namespace

// The score setup, the worn items and the rates, read once per reader
struct GearWorth::InputReader::State
{
    explicit State(Player* bot) : calculator(bot) {}

    StatsWeightCalculator calculator;
    Worn worn;
    uint32 level{0};
    float earnCopperPerMinute{0.0f};
    float hoursPerLevel{0.0f};
};

GearWorth::InputReader::InputReader(PlayerbotAI* botAI) : botAI(botAI), state(std::make_unique<State>(botAI->GetBot()))
{
    Player* bot = botAI->GetBot();
    state->calculator.SetUpForEquipCompare();
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        state->worn.proto[slot] = item->GetTemplate();
        state->worn.score[slot] =
            state->calculator.CalculateItem(item->GetEntry(), item->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
    }

    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    WorldTravel::SlotRates const rates = WorldTravel::CurrentRates(
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet(),
        config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())]);
    state->level = bot->GetLevel();
    state->earnCopperPerMinute = rates.EarnCopperPerMinute;
    state->hoursPerLevel = HoursPerLevel(bot->GetLevel());
}

GearWorth::InputReader::~InputReader() = default;

std::optional<GearWorth::LiveInput> GearWorth::InputReader::Read(ItemTemplate const* proto,
                                                                 int32 randomPropertyId) const
{
    if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP)
        return std::nullopt;

    Player* bot = botAI->GetBot();
    uint8 const dstSlot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
    if (dstSlot >= EQUIPMENT_SLOT_END)
        return std::nullopt;

    // CalculateItem resets the calculator's stats for each item: the order of the reads does not matter
    float const itemScore = state->calculator.CalculateItem(proto->ItemId, randomPropertyId);
    Worn const& worn = state->worn;
    SlotChoice const choice = ChooseSlot(bot, proto, dstSlot, itemScore, worn);
    LiveInput out;
    out.slot = choice.slot;
    out.slots = choice.fills;

    float summed = std::max(itemScore, 0.0f);
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        float const score = std::max(worn.score[slot], 0.0f);
        if (choice.leaves & SlotBit(slot))
            out.worth.wornScore += score;
        else
            summed += score;
    }

    out.worth.itemScore = itemScore;
    out.worth.summedScore = summed;
    out.worth.level = state->level;
    out.worth.itemLevel = proto->ItemLevel;
    out.worth.earnCopperPerMinute = state->earnCopperPerMinute;
    out.worth.hoursPerLevel = state->hoursPerLevel;
    return out;
}

std::optional<GearWorth::LiveInput> GearWorth::ReadInput(PlayerbotAI* botAI, ItemTemplate const* proto,
                                                         int32 randomPropertyId)
{
    if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP ||
        botAI->FindEquipSlot(proto, NULL_SLOT, true) >= EQUIPMENT_SLOT_END)
        return std::nullopt;
    return InputReader(botAI).Read(proto, randomPropertyId);
}
