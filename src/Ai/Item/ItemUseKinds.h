/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSEKINDS_H
#define _PLAYERBOT_ITEMUSEKINDS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "ItemUseTypes.h"

// The kinds of use effect and the classifier (INTERFACE.md 1.7). WI2 wrote the enum and the words; lane 2 (WI12)
// wrote the classifier, a pure function of a Record's facts: no item ids, no names. A kind may be appended before
// Count, never renamed or reordered. The shapes of the data and the idea of a consumable type from effects follow
// cmangos playerbots (d83e54b7, UseConsumableAction / ItemUsageValue); the rules here are our own.
namespace ItemUse
{
// Words, in this order: unclassified heal mana damage_one damage_area control move protect dispel boost long_buff
// food_drink coating summon resurrect transport upgrade learn open quest mount toy
enum class Kind : std::uint8_t
{
    Unclassified,
    Heal,
    Mana,
    DamageOne,
    DamageArea,
    Control,
    Move,
    Protect,
    Dispel,
    Boost,
    LongBuff,
    FoodDrink,
    Coating,
    Summon,
    Resurrect,
    Transport,
    Upgrade,
    Learn,
    Open,
    Quest,
    Mount,
    Toy,
    Count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Kind::Count)> KIND_WORDS = {
    "unclassified", "heal",  "mana",      "damage_one", "damage_area", "control", "move",      "protect",
    "dispel",       "boost", "long_buff", "food_drink", "coating",     "summon",  "resurrect", "transport",
    "upgrade",      "learn", "open",      "quest",      "mount",       "toy"};

constexpr std::string_view KindWord(Kind kind)
{
    std::size_t const index = static_cast<std::size_t>(kind);
    return index < KIND_WORDS.size() ? KIND_WORDS[index] : std::string_view("unclassified");
}

// The finer word inside a kind (Classified::sub). One list for every kind; a kind uses only its own words:
// move: speed fall jump; summon: helper decoy repair object; mana: mana rage energy runic; food_drink: food drink
// food_drink well_fed; learn: recipe glyph spell; open: lock create. A summoned game object (a feast, a mailbox, a
// portal, a chair) is summon/object; helper, decoy and repair are kept for a creature's role, which the Record does
// not carry yet (the summoned creature's flags), so WI12 gives a summoned creature sub none.
enum class Sub : std::uint8_t
{
    None,
    Speed,
    Fall,
    Jump,
    Helper,
    Decoy,
    Repair,
    Mana,
    Rage,
    Energy,
    Runic,
    Food,
    Drink,
    FoodDrink,
    WellFed,
    Recipe,
    Glyph,
    Spell,
    Lock,
    Create,
    Object,
    Count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Sub::Count)> SUB_WORDS = {
    "none", "speed", "fall",       "jump",     "helper", "decoy", "repair", "mana", "rage",   "energy", "runic",
    "food", "drink", "food_drink", "well_fed", "recipe", "glyph", "spell",  "lock", "create", "object"};

constexpr std::string_view SubWord(std::uint8_t sub)
{
    return sub < SUB_WORDS.size() ? SUB_WORDS[sub] : std::string_view("none");
}

struct Classified
{
    Kind kind{Kind::Unclassified};
    std::uint8_t sub{0};   // a Sub: a finer word inside the kind
    std::int32_t rank{0};  // the amount that ranks two items of one kind: larger is stronger
    std::string_view why;  // unclassified: what made it so (a literal); else empty
};

// The game's ids the classifier reads (SharedDefines.h SpellEffects, Mechanics, Powers; SpellAuraDefines.h
// AuraType; ItemTemplate.h item classes), mirrored because the header includes no game header
namespace KindIds
{
// SpellEffects
constexpr std::uint16_t INSTAKILL = 1;
constexpr std::uint16_t SCHOOL_DAMAGE = 2;
constexpr std::uint16_t TELEPORT_UNITS = 5;
constexpr std::uint16_t APPLY_AURA = 6;
constexpr std::uint16_t ENVIRONMENTAL_DAMAGE = 7;
constexpr std::uint16_t HEALTH_LEECH = 9;
constexpr std::uint16_t SEND_EVENT = 61;
constexpr std::uint16_t DUMMY = 3;
constexpr std::uint16_t TRANS_DOOR = 50;
constexpr std::uint16_t TRIGGER_SPELL = 64;
constexpr std::uint16_t SCRIPT_EFFECT = 77;
constexpr std::uint16_t SUMMON_OBJECT_SLOT1 = 104;
constexpr std::uint16_t SUMMON_OBJECT_SLOT4 = 107;
constexpr std::uint16_t HEAL = 10;
constexpr std::uint16_t QUEST_COMPLETE = 16;
constexpr std::uint16_t WEAPON_DAMAGE_NOSCHOOL = 17;
constexpr std::uint16_t RESURRECT = 18;
constexpr std::uint16_t CREATE_ITEM = 24;
constexpr std::uint16_t SUMMON = 28;
constexpr std::uint16_t LEAP = 29;
constexpr std::uint16_t ENERGIZE = 30;
constexpr std::uint16_t WEAPON_PERCENT_DAMAGE = 31;
constexpr std::uint16_t OPEN_LOCK = 33;
constexpr std::uint16_t LEARN_SPELL = 36;
constexpr std::uint16_t DISPEL = 38;
constexpr std::uint16_t JUMP = 41;
constexpr std::uint16_t JUMP_DEST = 42;
constexpr std::uint16_t SKILL_STEP = 44;
constexpr std::uint16_t ENCHANT_ITEM = 53;
constexpr std::uint16_t ENCHANT_ITEM_TEMPORARY = 54;
constexpr std::uint16_t SUMMON_PET = 56;
constexpr std::uint16_t LEARN_PET_SPELL = 57;
constexpr std::uint16_t WEAPON_DAMAGE = 58;
constexpr std::uint16_t CREATE_RANDOM_ITEM = 59;
constexpr std::uint16_t CREATE_MANA_GEM = 66;
constexpr std::uint16_t HEAL_MAX_HEALTH = 67;
constexpr std::uint16_t INTERRUPT_CAST = 68;
constexpr std::uint16_t APPLY_GLYPH = 74;
constexpr std::uint16_t HEAL_MECHANICAL = 75;
constexpr std::uint16_t SUMMON_OBJECT_WILD = 76;
constexpr std::uint16_t SUMMON_PLAYER = 85;
constexpr std::uint16_t KILL_CREDIT = 90;
constexpr std::uint16_t ENCHANT_HELD_ITEM = 92;
constexpr std::uint16_t SELF_RESURRECT = 94;
constexpr std::uint16_t CHARGE = 96;
constexpr std::uint16_t KNOCK_BACK = 98;
constexpr std::uint16_t INEBRIATE = 100;
constexpr std::uint16_t DISPEL_MECHANIC = 108;
constexpr std::uint16_t RESURRECT_NEW = 113;
constexpr std::uint16_t SKILL = 118;
constexpr std::uint16_t TELEPORT_GRAVEYARD = 120;
constexpr std::uint16_t NORMALIZED_WEAPON_DMG = 121;
constexpr std::uint16_t SEND_TAXI = 123;
constexpr std::uint16_t PULL_TOWARDS = 124;
constexpr std::uint16_t PLAY_SOUND = 131;
constexpr std::uint16_t PLAY_MUSIC = 132;
constexpr std::uint16_t KILL_CREDIT2 = 134;
constexpr std::uint16_t HEAL_PCT = 136;
constexpr std::uint16_t ENERGIZE_PCT = 137;
constexpr std::uint16_t LEAP_BACK = 138;
constexpr std::uint16_t CLEAR_QUEST = 139;
constexpr std::uint16_t KNOCK_BACK_DEST = 144;
constexpr std::uint16_t PULL_TOWARDS_DEST = 145;
constexpr std::uint16_t QUEST_FAIL = 147;
constexpr std::uint16_t CHARGE_DEST = 149;
constexpr std::uint16_t QUEST_START = 150;
constexpr std::uint16_t ENCHANT_ITEM_PRISMATIC = 156;
constexpr std::uint16_t CREATE_ITEM_2 = 157;

// AuraType
constexpr std::uint16_t AURA_POSSESS = 2;
constexpr std::uint16_t AURA_PERIODIC_DAMAGE = 3;
constexpr std::uint16_t AURA_DUMMY = 4;
constexpr std::uint16_t AURA_CONFUSE = 5;
constexpr std::uint16_t AURA_CHARM = 6;
constexpr std::uint16_t AURA_FEAR = 7;
constexpr std::uint16_t AURA_PERIODIC_HEAL = 8;
constexpr std::uint16_t AURA_STUN = 12;
constexpr std::uint16_t AURA_OBS_MOD_HEALTH = 20;
constexpr std::uint16_t AURA_OBS_MOD_POWER = 21;
constexpr std::uint16_t AURA_MOD_RESISTANCE = 22;
constexpr std::uint16_t AURA_PERIODIC_TRIGGER_SPELL = 23;
constexpr std::uint16_t AURA_PERIODIC_ENERGIZE = 24;
constexpr std::uint16_t AURA_PACIFY = 25;
constexpr std::uint16_t AURA_ROOT = 26;
constexpr std::uint16_t AURA_SILENCE = 27;
constexpr std::uint16_t AURA_REFLECT_SPELLS = 28;
constexpr std::uint16_t AURA_INCREASE_SPEED = 31;
constexpr std::uint16_t AURA_INCREASE_MOUNTED_SPEED = 32;
constexpr std::uint16_t AURA_DECREASE_SPEED = 33;
constexpr std::uint16_t AURA_EFFECT_IMMUNITY = 37;
constexpr std::uint16_t AURA_STATE_IMMUNITY = 38;
constexpr std::uint16_t AURA_SCHOOL_IMMUNITY = 39;
constexpr std::uint16_t AURA_DAMAGE_IMMUNITY = 40;
constexpr std::uint16_t AURA_DISPEL_IMMUNITY = 41;
constexpr std::uint16_t AURA_PERIODIC_LEECH = 53;
constexpr std::uint16_t AURA_TRANSFORM = 56;
constexpr std::uint16_t AURA_INCREASE_SWIM_SPEED = 58;
constexpr std::uint16_t AURA_PACIFY_SILENCE = 60;
constexpr std::uint16_t AURA_MOD_SCALE = 61;
constexpr std::uint16_t AURA_MOD_DISARM = 67;
constexpr std::uint16_t AURA_SCHOOL_ABSORB = 69;
constexpr std::uint16_t AURA_REFLECT_SPELLS_SCHOOL = 74;
constexpr std::uint16_t AURA_MECHANIC_IMMUNITY = 77;
constexpr std::uint16_t AURA_MOUNTED = 78;
constexpr std::uint16_t AURA_SPLIT_DAMAGE_PCT = 81;
constexpr std::uint16_t AURA_MOD_BASE_RESISTANCE = 83;
constexpr std::uint16_t AURA_MOD_REGEN = 84;
constexpr std::uint16_t AURA_MOD_POWER_REGEN = 85;
constexpr std::uint16_t AURA_DAMAGE_PERCENT_TAKEN = 87;
constexpr std::uint16_t AURA_PERIODIC_DAMAGE_PERCENT = 89;
constexpr std::uint16_t AURA_MANA_SHIELD = 97;
constexpr std::uint16_t AURA_MOD_RESISTANCE_PCT = 101;
constexpr std::uint16_t AURA_WATER_WALK = 104;
constexpr std::uint16_t AURA_FEATHER_FALL = 105;
constexpr std::uint16_t AURA_HOVER = 106;
constexpr std::uint16_t AURA_SPEED_ALWAYS = 129;
constexpr std::uint16_t AURA_MOD_BASE_RESISTANCE_PCT = 142;
constexpr std::uint16_t AURA_MOD_RESISTANCE_EXCLUSIVE = 143;
constexpr std::uint16_t AURA_SAFE_FALL = 144;
constexpr std::uint16_t AURA_MECHANIC_IMMUNITY_MASK = 147;
constexpr std::uint16_t AURA_SPEED_NOT_STACK = 171;
constexpr std::uint16_t AURA_AOE_CHARM = 177;
constexpr std::uint16_t AURA_FLY = 201;
constexpr std::uint16_t AURA_INCREASE_MOUNTED_FLIGHT_SPEED = 207;
constexpr std::uint16_t AURA_PERIODIC_DUMMY = 226;
constexpr std::uint16_t AURA_CONTROL_VEHICLE = 236;
constexpr std::uint16_t AURA_MOD_SCALE_2 = 239;
constexpr std::uint16_t AURA_PHASE = 261;

// Targets (SharedDefines.h Targets): an effect aimed at the caster alone
constexpr std::uint8_t TARGET_UNIT_CASTER = 1;

// Mechanics that take control of a unit (SharedDefines.h Mechanics)
constexpr std::uint32_t CONTROL_MECHANICS = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) |
                                            (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) |
                                            (1u << 17) | (1u << 18) | (1u << 20) | (1u << 23) | (1u << 24) |
                                            (1u << 26) | (1u << 27) | (1u << 30);

// Powers
constexpr std::int32_t POWER_MANA = 0;
constexpr std::int32_t POWER_RAGE = 1;
constexpr std::int32_t POWER_ENERGY = 3;
constexpr std::int32_t POWER_RUNIC = 6;

// SpellSpecificType, as ItemUseFacts.h's Game namespace
constexpr std::uint8_t SPECIFIC_FOOD = 19;
constexpr std::uint8_t SPECIFIC_DRINK = 20;
constexpr std::uint8_t SPECIFIC_FOOD_AND_DRINK = 21;

// Item classes (ItemTemplate.h ItemClass, ItemSubclassMisc)
constexpr std::uint8_t CLASS_RECIPE = 9;
constexpr std::uint8_t CLASS_QUEST = 12;
constexpr std::uint8_t CLASS_MISC = 15;
constexpr std::uint8_t SUBCLASS_MISC_MOUNT = 5;
constexpr std::uint8_t CLASS_GLYPH = 16;

// A boost lasts up to this; a long buff longer (or without end). A fight's cooldown of a trinket or a potion lasts
// seconds; an elixir, a scroll or a juju lasts many minutes
constexpr std::uint32_t BOOST_MAX_MS = 5 * 60 * 1000;
constexpr std::uint32_t ENDLESS_MS = 0xFFFFFFFFu;
}  // namespace KindIds

namespace KindDetail
{
// What one pass over a record's effects (its own and those of the spells it triggers) found. Each total is the
// amount over the effect's life: per tick times the ticks of the record's duration, or the amount once
struct Found
{
    std::int64_t heal{0};
    std::int64_t power{0};
    std::int32_t powerType{-1};
    std::int64_t damage{0};
    bool areaDamage{false};
    bool control{false};
    bool harmTriggered{false};  // damage or control comes only from a spell the use spell triggers (a proc, a tick)
    bool harmDirect{false};
    bool script{true};  // every effect is a dummy, a script, an event or a carrier of another spell
    bool quest{false};
    bool learn{false};
    bool glyph{false};
    bool mounted{false};
    std::int32_t mountSpeed{0};
    bool coating{false};
    bool upgrade{false};
    bool resurrect{false};
    std::int32_t resurrectAmount{0};
    bool transport{false};
    bool openLock{false};
    bool create{false};
    bool summon{false};
    bool summonObject{false};
    bool dispel{false};
    std::int32_t dispelCount{0};
    std::int32_t protect{-1};
    Sub move{Sub::None};
    std::int32_t moveAmount{0};
    std::int32_t buff{-1};
    std::int32_t regen{0};
    bool toy{false};
};

constexpr std::int64_t Total(EffectFact const& effect, std::uint32_t durationMs)
{
    if (!effect.periodMs || !durationMs || durationMs == KindIds::ENDLESS_MS)
        return effect.amount;
    return static_cast<std::int64_t>(effect.amount) * (durationMs / effect.periodMs);
}

constexpr std::int32_t Clamp(std::int64_t value)
{
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, 0, 0x7FFFFFFF));
}

constexpr bool ControlAura(std::uint16_t aura)
{
    using namespace KindIds;
    return aura == AURA_POSSESS || aura == AURA_CONFUSE || aura == AURA_CHARM || aura == AURA_FEAR ||
           aura == AURA_STUN || aura == AURA_PACIFY || aura == AURA_ROOT || aura == AURA_SILENCE ||
           aura == AURA_DECREASE_SPEED || aura == AURA_PACIFY_SILENCE || aura == AURA_MOD_DISARM ||
           aura == AURA_AOE_CHARM;
}

constexpr bool ProtectAura(std::uint16_t aura)
{
    using namespace KindIds;
    return aura == AURA_REFLECT_SPELLS || aura == AURA_MOD_RESISTANCE || aura == AURA_EFFECT_IMMUNITY ||
           aura == AURA_STATE_IMMUNITY || aura == AURA_SCHOOL_IMMUNITY || aura == AURA_DAMAGE_IMMUNITY ||
           aura == AURA_DISPEL_IMMUNITY || aura == AURA_SCHOOL_ABSORB || aura == AURA_REFLECT_SPELLS_SCHOOL ||
           aura == AURA_MECHANIC_IMMUNITY || aura == AURA_SPLIT_DAMAGE_PCT || aura == AURA_MOD_BASE_RESISTANCE ||
           aura == AURA_DAMAGE_PERCENT_TAKEN || aura == AURA_MANA_SHIELD || aura == AURA_MOD_RESISTANCE_PCT ||
           aura == AURA_MOD_BASE_RESISTANCE_PCT || aura == AURA_MOD_RESISTANCE_EXCLUSIVE ||
           aura == AURA_MECHANIC_IMMUNITY_MASK;
}

// Auras that change nothing a bot ranks by: a script's marker, a carrier of another spell (its effects are read as
// triggered ones), a look, a phase or a vehicle
constexpr bool NeutralAura(std::uint16_t aura)
{
    using namespace KindIds;
    return aura == 0 || aura == AURA_DUMMY || aura == AURA_PERIODIC_TRIGGER_SPELL || aura == AURA_PERIODIC_DUMMY ||
           aura == AURA_TRANSFORM || aura == AURA_MOD_SCALE || aura == AURA_MOD_SCALE_2 || aura == AURA_PHASE ||
           aura == AURA_CONTROL_VEHICLE;
}

inline void ReadAura(EffectFact const& effect, std::uint32_t duration, std::int32_t magnitude, Found& found)
{
    using namespace KindIds;
    std::uint16_t const aura = effect.aura;
    if (aura == AURA_PERIODIC_DAMAGE || aura == AURA_PERIODIC_LEECH || aura == AURA_PERIODIC_DAMAGE_PERCENT)
    {
        found.damage += std::max<std::int64_t>(Total(effect, duration), 1);
        found.areaDamage = found.areaDamage || effect.radiusYards > 0.0f;
    }
    else if (aura == AURA_PERIODIC_HEAL || aura == AURA_OBS_MOD_HEALTH)
        found.heal += std::max<std::int64_t>(Total(effect, duration), 1);
    else if (aura == AURA_PERIODIC_ENERGIZE || aura == AURA_OBS_MOD_POWER)
    {
        found.power += std::max<std::int64_t>(Total(effect, duration), 1);
        found.powerType = found.powerType < 0 ? effect.misc : found.powerType;
    }
    else if (ControlAura(aura))
        found.control = true;
    else if (aura == AURA_MOUNTED)
        found.mounted = true;
    else if (aura == AURA_INCREASE_MOUNTED_SPEED || aura == AURA_INCREASE_MOUNTED_FLIGHT_SPEED)
        found.mountSpeed = std::max(found.mountSpeed, magnitude);
    else if (ProtectAura(aura))
        found.protect = std::max(found.protect, magnitude);
    else if (aura == AURA_INCREASE_SPEED || aura == AURA_SPEED_ALWAYS || aura == AURA_SPEED_NOT_STACK ||
             aura == AURA_INCREASE_SWIM_SPEED || aura == AURA_FLY)
    {
        found.move = Sub::Speed;
        found.moveAmount = std::max(found.moveAmount, magnitude);
    }
    else if (aura == AURA_FEATHER_FALL || aura == AURA_SAFE_FALL || aura == AURA_HOVER || aura == AURA_WATER_WALK)
        found.move = found.move == Sub::Speed ? Sub::Speed : Sub::Fall;
    else if (aura == AURA_MOD_REGEN || aura == AURA_MOD_POWER_REGEN)
    {
        found.regen = std::max(found.regen, magnitude);
        found.buff = std::max(found.buff, magnitude);
    }
    else if (aura == AURA_TRANSFORM || aura == AURA_MOD_SCALE || aura == AURA_MOD_SCALE_2)
        found.toy = true;
    else if (!NeutralAura(aura))
        found.buff = std::max(found.buff, magnitude);
}

inline void Read(EffectFact const& effect, Record const& record, Found& found)
{
    using namespace KindIds;
    std::uint16_t const aura = effect.aura;
    // damage or control aimed at the user alone (a rune's price in health, a disguise's stun) harms no enemy
    bool const onSelf = effect.targetA == TARGET_UNIT_CASTER && !effect.targetB;
    bool const scriptEffect = effect.effect == DUMMY || effect.effect == SCRIPT_EFFECT || effect.effect == SEND_EVENT ||
                              effect.effect == TRIGGER_SPELL ||
                              (effect.effect == APPLY_AURA && NeutralAura(aura) && aura != AURA_TRANSFORM &&
                               aura != AURA_MOD_SCALE && aura != AURA_MOD_SCALE_2);
    found.script = found.script && scriptEffect;
    std::int64_t const damageBefore = found.damage;
    bool const controlBefore = found.control;
    std::int32_t const magnitude = static_cast<std::int32_t>(
        std::min<std::int64_t>(std::llabs(static_cast<long long>(effect.amount)), 0x7FFFFFFF));
    switch (effect.effect)
    {
        case INSTAKILL:
        case SCHOOL_DAMAGE:
        case ENVIRONMENTAL_DAMAGE:
        case HEALTH_LEECH:
        case WEAPON_DAMAGE_NOSCHOOL:
        case WEAPON_PERCENT_DAMAGE:
        case WEAPON_DAMAGE:
        case NORMALIZED_WEAPON_DMG:
            found.damage += std::max<std::int64_t>(effect.amount, 1);
            found.areaDamage = found.areaDamage || effect.radiusYards > 0.0f;
            break;
        case HEAL:
        case HEAL_MAX_HEALTH:
        case HEAL_MECHANICAL:
        case HEAL_PCT:
            found.heal += std::max<std::int64_t>(effect.amount, 1);
            break;
        case ENERGIZE:
        case ENERGIZE_PCT:
            found.power += std::max<std::int64_t>(effect.amount, 1);
            found.powerType = found.powerType < 0 ? effect.misc : found.powerType;
            break;
        case QUEST_COMPLETE:
        case KILL_CREDIT:
        case KILL_CREDIT2:
        case CLEAR_QUEST:
        case QUEST_FAIL:
        case QUEST_START:
            found.quest = true;
            break;
        case LEARN_SPELL:
        case LEARN_PET_SPELL:
        case SKILL_STEP:
        case SKILL:
            found.learn = true;
            break;
        case APPLY_GLYPH:
            found.learn = true;
            found.glyph = true;
            break;
        case ENCHANT_ITEM_TEMPORARY:
            found.coating = true;
            break;
        case ENCHANT_ITEM:
        case ENCHANT_HELD_ITEM:
        case ENCHANT_ITEM_PRISMATIC:
            found.upgrade = true;
            break;
        case RESURRECT:
        case RESURRECT_NEW:
        case SELF_RESURRECT:
            found.resurrect = true;
            found.resurrectAmount = std::max(found.resurrectAmount, magnitude);
            break;
        case TELEPORT_UNITS:
        case SUMMON_PLAYER:
        case TELEPORT_GRAVEYARD:
        case SEND_TAXI:
            found.transport = true;
            break;
        case OPEN_LOCK:
            found.openLock = true;
            break;
        case CREATE_ITEM:
        case CREATE_RANDOM_ITEM:
        case CREATE_MANA_GEM:
        case CREATE_ITEM_2:
            found.create = true;
            break;
        case SUMMON:
        case SUMMON_PET:
            found.summon = true;
            break;
        case TRANS_DOOR:
        case SUMMON_OBJECT_WILD:
        case SUMMON_OBJECT_SLOT1:
        case SUMMON_OBJECT_SLOT1 + 1:
        case SUMMON_OBJECT_SLOT1 + 2:
        case SUMMON_OBJECT_SLOT4:
            found.summon = true;
            found.summonObject = true;
            break;
        case DISPEL:
        case DISPEL_MECHANIC:
            found.dispel = true;
            found.dispelCount += std::max(magnitude, 1);
            break;
        case LEAP:
        case JUMP:
        case JUMP_DEST:
        case CHARGE:
        case LEAP_BACK:
        case CHARGE_DEST:
            found.move = found.move == Sub::None ? Sub::Jump : found.move;
            break;
        case INTERRUPT_CAST:
        case KNOCK_BACK:
        case PULL_TOWARDS:
        case KNOCK_BACK_DEST:
        case PULL_TOWARDS_DEST:
            found.control = true;
            break;
        case INEBRIATE:
        case PLAY_SOUND:
        case PLAY_MUSIC:
            found.toy = true;
            break;
        default:
            break;
    }
    if (effect.mechanic < 32 && ((KindIds::CONTROL_MECHANICS >> effect.mechanic) & 1u))
        found.control = true;
    if (aura)
        ReadAura(effect, record.durationMs, magnitude, found);
    if (found.damage != damageBefore || found.control != controlBefore)
    {
        if (onSelf)
        {
            found.damage = damageBefore;
            found.control = controlBefore;
        }
        else if (effect.triggered)
            found.harmTriggered = true;
        else
            found.harmDirect = true;
    }
}

constexpr Classified Is(Kind kind, std::int64_t rank, Sub sub = Sub::None)
{
    return {kind, static_cast<std::uint8_t>(sub), Clamp(rank), {}};
}

constexpr Classified Not(std::string_view why) { return {Kind::Unclassified, 0, 0, why}; }

constexpr Sub PowerSub(std::int32_t power)
{
    switch (power)
    {
        case KindIds::POWER_RAGE:
            return Sub::Rage;
        case KindIds::POWER_ENERGY:
            return Sub::Energy;
        case KindIds::POWER_RUNIC:
            return Sub::Runic;
        default:
            return Sub::Mana;
    }
}

constexpr Sub FoodSub(Record const& record)
{
    if (record.exclude == Exclude::WellFed)
        return Sub::WellFed;
    if (record.spellSpecific == KindIds::SPECIFIC_DRINK || record.exclude == Exclude::Drink)
        return Sub::Drink;
    if (record.spellSpecific == KindIds::SPECIFIC_FOOD_AND_DRINK)
        return Sub::FoodDrink;
    return Sub::Food;
}

// Seconds of control; a control without end ranks above any timed one
constexpr std::int64_t ControlRank(Record const& record)
{
    return record.durationMs == KindIds::ENDLESS_MS ? 0x7FFFFFFF : std::max<std::uint32_t>(record.durationMs / 1000, 1);
}
}  // namespace KindDetail

// The classifier (INTERFACE.md 1.7). Pure, any thread, no game header. Precedence, first match wins:
//   1 no effect                                         -> unclassified no_effect
//   2 a quest item (class 12) or a quest effect          -> quest
//   3 a mount item (class 15 sub 5) or a mounted aura    -> mount, rank the mounted speed bonus
//   4 a recipe (class 9), a glyph (class 16), a learn,
//     skill or glyph effect                              -> learn, sub recipe / glyph / spell
//   5 a temporary enchantment (stone, oil, poison)       -> coating, rank the item level
//   6 a permanent enchantment (armor kit, held item)     -> upgrade, rank the item level
//   7 a resurrection                                     -> resurrect, rank its amount
//   8 a teleport, a summon of a player, a taxi           -> transport
//   9 an open-lock effect                                -> open, sub lock
//  Then the sign: a harmful record (positive false) may only be damage, control or a neutral kind; a helpful one
//  (positive true) only a helpful or neutral kind, so K2 and K3 hold by construction. Damage or control aimed at
//  the user alone (target caster, no second target: a rune's price in health, a disguise's stun) is not counted.
//  harmful: damage -> damage_area when it hits an area (an effect or record radius, or aimed at the ground) else
//     damage_one, rank the total damage; control -> control, rank its seconds; dispel -> dispel; summon -> summon;
//     else unclassified: helps_and_harms when it also heals or restores (K2 forbids heal and mana), harm_script_only
//     when every effect is a dummy, a script, an event or a carrier, else harm_other (a debuff, a drunk's price).
//  helpful: food or drink (specific or exclusion) -> food_drink, rank the regeneration; an elixir, flask or scroll
//     -> long_buff, rank the largest aura amount; damage or control aimed by the use spell, or the only thing a
//     triggered spell does -> unclassified helps_and_harms (K3 forbids the damage kinds; behind a buff, a proc's
//     harm is ignored); heal -> heal, rank the total healed; mana or another power -> mana (sub the power), rank
//     the total; absorb, resistance, immunity -> protect, rank the largest amount; dispel -> dispel, rank the
//     count; speed, fall, jump -> move (sub), rank the speed bonus; a creature or object summoned -> summon (sub
//     object for an object); another aura that changes the bot -> boost when it lasts up to five minutes, else
//     long_buff, rank the largest amount; a look, a size, a drink of alcohol, a sound -> toy; an item made -> open,
//     sub create; else unclassified script_only (only dummies, scripts, events: the facts do not say the result)
//     or help_other.
// Rank never reads consumed, charges or a price (K4): a consumed item ranks as its effect, and the offer reads
// Record::consumed itself. Ranks compare within a kind only.
inline Classified Classify(Record const& record)
{
    using namespace KindIds;
    using KindDetail::Is;
    using KindDetail::Not;
    if (!record.effectCount)
        return Not("no_effect");
    KindDetail::Found found;
    for (std::size_t i = 0; i < record.effectCount && i < record.effects.size(); ++i)
        KindDetail::Read(record.effects[i], record, found);

    bool const item = record.source == Source::ItemSpell;
    if ((item && record.itemClass == CLASS_QUEST) || found.quest)
        return Is(Kind::Quest, 0);
    if ((item && record.itemClass == CLASS_MISC && record.itemSubClass == SUBCLASS_MISC_MOUNT) || found.mounted)
        return Is(Kind::Mount, found.mountSpeed);
    bool const recipe = item && record.itemClass == CLASS_RECIPE;
    bool const glyph = found.glyph || (item && record.itemClass == CLASS_GLYPH);
    if (recipe || glyph || found.learn)
        return Is(Kind::Learn, 0, recipe ? Sub::Recipe : glyph ? Sub::Glyph : Sub::Spell);
    if (found.coating || record.exclude == Exclude::Coating)
        return Is(Kind::Coating, record.itemLevel);
    if (found.upgrade)
        return Is(Kind::Upgrade, record.itemLevel);
    if (found.resurrect)
        return Is(Kind::Resurrect, found.resurrectAmount);
    if (found.transport && (record.shape == Shape::Self || record.shape == Shape::None))
        return Is(Kind::Transport, 0);
    if (found.openLock)
        return Is(Kind::Open, 0, Sub::Lock);

    if (!record.positive)
    {
        if (found.damage > 0)
        {
            bool const area = found.areaDamage || record.radiusYards > 0.0f || record.shape == Shape::Ground;
            return Is(area ? Kind::DamageArea : Kind::DamageOne, found.damage);
        }
        if (found.control)
            return Is(Kind::Control, KindDetail::ControlRank(record));
        if (found.dispel)
            return Is(Kind::Dispel, found.dispelCount);
        if (found.summon)
            return Is(Kind::Summon, 0, found.summonObject ? Sub::Object : Sub::None);
        if (found.heal > 0 || found.power > 0)
            return Not("helps_and_harms");
        return Not(found.script ? "harm_script_only" : "harm_other");
    }

    bool const food = record.spellSpecific == SPECIFIC_FOOD || record.spellSpecific == SPECIFIC_DRINK ||
                      record.spellSpecific == SPECIFIC_FOOD_AND_DRINK || record.exclude == Exclude::Food ||
                      record.exclude == Exclude::Drink || record.exclude == Exclude::WellFed;
    if (food)
        return Is(Kind::FoodDrink, std::max<std::int64_t>({found.regen, found.heal, found.power}),
                  KindDetail::FoodSub(record));
    bool const exclusive = record.exclude == Exclude::BattleElixir || record.exclude == Exclude::GuardianElixir ||
                           record.exclude == Exclude::Flask || record.exclude == Exclude::Scroll;
    if (exclusive)
        return Is(Kind::LongBuff, std::max({found.buff, found.protect, found.moveAmount, 0}));
    // A helpful record's harm: aimed by the use spell itself, or the only thing a proc or a tick does, it is no help
    // the classifier may claim (K3); behind a buff (a proc of a boost) the buff is what the bot uses it for
    bool const helps = found.heal > 0 || found.power > 0 || found.protect >= 0 || found.buff >= 0 || found.dispel ||
                       found.move != Sub::None || found.summon;
    if (found.harmDirect || (found.harmTriggered && !helps))
        return Not("helps_and_harms");
    if (found.heal > 0)
        return Is(Kind::Heal, found.heal);
    if (found.power > 0)
        return Is(Kind::Mana, found.power, KindDetail::PowerSub(found.powerType));
    if (found.protect >= 0)
        return Is(Kind::Protect, found.protect);
    if (found.dispel)
        return Is(Kind::Dispel, found.dispelCount);
    if (found.move != Sub::None)
        return Is(Kind::Move, found.moveAmount, found.move);
    if (found.summon)
        return Is(Kind::Summon, 0, found.summonObject ? Sub::Object : Sub::None);
    if (found.buff >= 0)
        return Is(record.durationMs <= BOOST_MAX_MS ? Kind::Boost : Kind::LongBuff, found.buff);
    if (found.toy)
        return Is(Kind::Toy, 0);
    if (found.create)
        return Is(Kind::Open, 0, Sub::Create);
    return Not(found.script ? "script_only" : "help_other");
}
}  // namespace ItemUse

#endif
