/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StatsWeightCalculator.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <span>

#include "AiFactory.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellMgr.h"
#include "StatsCollector.h"
#include "Unit.h"

namespace
{
constexpr uint32 SPELL_ROGUE_SWORD_SPECIALIZATION = 13964;
constexpr uint32 SPELL_POLEAXE_SPECIALIZATION = 12785;
constexpr uint32 SPELL_NERVES_OF_COLD_STEEL = 50138;
constexpr uint32 SPELL_SHADOW_FOCUS = 15835;
constexpr uint32 SPELL_ARCANE_FOCUS = 12840;

// Racial weapon specializations, expertise in points, ranged crit in percent
constexpr float RACIAL_EXPERTISE_ORC_AXE_FIST = 5.0f;
constexpr float RACIAL_EXPERTISE_HUMAN_SWORD_MACE = 3.0f;
constexpr float RACIAL_EXPERTISE_DWARF_MACE = 5.0f;
constexpr float RACIAL_CRIT_PCT_DWARF_GUN = 1.0f;
constexpr float RACIAL_CRIT_PCT_TROLL_BOW = 1.0f;

// Threat reduction per percent and magic resistance per point, by role
constexpr float HEALER_THREAT_REDUCTION_WEIGHT = 2.0f;
constexpr float HEALER_RESISTANCE_WEIGHT = 1.0f;
constexpr float CASTER_THREAT_REDUCTION_WEIGHT = 4.0f;
constexpr float CASTER_RESISTANCE_WEIGHT = 0.5f;
constexpr float TANK_THREAT_REDUCTION_WEIGHT = -4.0f;

// Far below any unbalanced sum of weights times whole stats: a score this close to 0 is an exactly balanced item that
// float rounding moved off 0, and the rule's "above 0" must not see it as positive
constexpr float SCORE_ROUNDING = 0.0001f;

// A bot specced for PvP: resilience per point by role, stamina scaled (not for tanks), spell penetration for casters
constexpr float PVP_PHYSICAL_RESILIENCE_WEIGHT = 4.0f;
constexpr float PVP_CASTER_RESILIENCE_WEIGHT = 1.7f;
constexpr float PVP_STAMINA_FACTOR = 1.5f;
constexpr float PVP_SPELL_PENETRATION_WEIGHT = 0.5f;

// The rows of the weight table: by class in the client's talent tab order (a class's first row plus its tab), the
// bear after the druid's three
enum WeightSpec : uint8
{
    WEIGHT_WARRIOR_ARMS,
    WEIGHT_WARRIOR_FURY,
    WEIGHT_WARRIOR_PROTECTION,
    WEIGHT_PALADIN_HOLY,
    WEIGHT_PALADIN_PROTECTION,
    WEIGHT_PALADIN_RETRIBUTION,
    WEIGHT_HUNTER_BEAST_MASTERY,
    WEIGHT_HUNTER_MARKSMANSHIP,
    WEIGHT_HUNTER_SURVIVAL,
    WEIGHT_ROGUE_ASSASSINATION,
    WEIGHT_ROGUE_COMBAT,
    WEIGHT_ROGUE_SUBTLETY,
    WEIGHT_PRIEST_DISCIPLINE,
    WEIGHT_PRIEST_HOLY,
    WEIGHT_PRIEST_SHADOW,
    WEIGHT_DEATH_KNIGHT_BLOOD,
    WEIGHT_DEATH_KNIGHT_FROST,
    WEIGHT_DEATH_KNIGHT_UNHOLY,
    WEIGHT_SHAMAN_ELEMENTAL,
    WEIGHT_SHAMAN_ENHANCEMENT,
    WEIGHT_SHAMAN_RESTORATION,
    WEIGHT_MAGE_ARCANE,
    WEIGHT_MAGE_FIRE,
    WEIGHT_MAGE_FROST,
    WEIGHT_WARLOCK_AFFLICTION,
    WEIGHT_WARLOCK_DEMONOLOGY,
    WEIGHT_WARLOCK_DESTRUCTION,
    WEIGHT_DRUID_BALANCE,
    WEIGHT_DRUID_CAT,
    WEIGHT_DRUID_RESTORATION,
    WEIGHT_DRUID_BEAR,
    WEIGHT_SPEC_COUNT
};

enum class WeightRole : uint8
{
    Physical,
    Tank,
    Healer,
    Caster
};

struct StatWeight
{
    StatsType stat;
    float weight;
};

// The stat weights per spec (gear plan WG1c, accepted 2026-11-27). Scale: a point of attack power weighs 1 for
// physical specs and tanks, a point of spell power (healing power for healers) 1 for casters and healers. A stat is
// core for a spec, side (stamina, armour), marginal (small, positive) or foreign: negative, it marks another role's
// item. A row is the base, then its shared parts, then its own values; a later value replaces an earlier one.
constexpr StatWeight BASE_WEIGHTS[] = {
    {STATS_TYPE_BONUS, 1.0f},
    {STATS_TYPE_MELEE_DPS, 0.01f},
    {STATS_TYPE_RANGED_DPS, 0.01f},
};
// No-mana physical specs and their tanks
constexpr StatWeight PHYSICAL_FOREIGN[] = {
    {STATS_TYPE_INTELLECT, -1.0f},
    {STATS_TYPE_SPELL_POWER, -2.0f},
    {STATS_TYPE_SPELL_PENETRATION, -1.0f},
    {STATS_TYPE_MANA_REGENERATION, -2.0f},
};
// Casters and healers
constexpr StatWeight CASTER_FOREIGN[] = {
    {STATS_TYPE_STRENGTH, -0.5f},          {STATS_TYPE_AGILITY, -0.5f},   {STATS_TYPE_ATTACK_POWER, -0.25f},
    {STATS_TYPE_ARMOR_PENETRATION, -0.5f}, {STATS_TYPE_EXPERTISE, -0.5f},
};
constexpr StatWeight PHYSICAL_SIDE[] = {
    {STATS_TYPE_STAMINA, 0.8f},
    {STATS_TYPE_ARMOR, 0.005f},
};
constexpr StatWeight CASTER_SIDE[] = {
    {STATS_TYPE_STAMINA, 0.3f},
    {STATS_TYPE_ARMOR, 0.005f},
};
constexpr StatWeight TANK_WEIGHTS[] = {
    {STATS_TYPE_STRENGTH, 1.3f},     {STATS_TYPE_AGILITY, 0.2f},    {STATS_TYPE_STAMINA, 3.1f},
    {STATS_TYPE_ATTACK_POWER, 0.2f}, {STATS_TYPE_DEFENSE, 2.5f},    {STATS_TYPE_PARRY, 2.0f},
    {STATS_TYPE_DODGE, 2.0f},        {STATS_TYPE_ARMOR, 0.151f},    {STATS_TYPE_HIT, 2.0f},
    {STATS_TYPE_EXPERTISE, 3.0f},    {STATS_TYPE_MELEE_DPS, 2.01f},
};
// Strength and expertise are the melee specs' stats
constexpr StatWeight HUNTER_FOREIGN[] = {
    {STATS_TYPE_STRENGTH, -0.5f},
    {STATS_TYPE_EXPERTISE, -0.5f},
    {STATS_TYPE_SPELL_POWER, -2.0f},
    {STATS_TYPE_SPELL_PENETRATION, -1.0f},
};
constexpr StatWeight ROGUE_DAGGER[] = {
    {STATS_TYPE_AGILITY, 1.5f},      {STATS_TYPE_STRENGTH, 1.1f},
    {STATS_TYPE_ATTACK_POWER, 1.0f}, {STATS_TYPE_ARMOR_PENETRATION, 1.2f},
    {STATS_TYPE_HIT, 2.1f},          {STATS_TYPE_CRIT, 1.1f},
    {STATS_TYPE_HASTE, 1.8f},        {STATS_TYPE_EXPERTISE, 2.1f},
    {STATS_TYPE_MELEE_DPS, 5.01f},   {STATS_TYPE_SPIRIT, 0.15f},
};
constexpr StatWeight HEALER_INTELLECT[] = {
    {STATS_TYPE_INTELLECT, 0.9f},         {STATS_TYPE_SPIRIT, 0.15f}, {STATS_TYPE_HEAL_POWER, 1.0f},
    {STATS_TYPE_MANA_REGENERATION, 0.9f}, {STATS_TYPE_CRIT, 0.6f},    {STATS_TYPE_HASTE, 0.8f},
};
constexpr StatWeight HEALER_SPIRIT[] = {
    {STATS_TYPE_INTELLECT, 0.8f},         {STATS_TYPE_SPIRIT, 0.6f}, {STATS_TYPE_HEAL_POWER, 1.0f},
    {STATS_TYPE_MANA_REGENERATION, 0.9f}, {STATS_TYPE_CRIT, 0.6f},   {STATS_TYPE_HASTE, 0.8f},
    {STATS_TYPE_RANGED_DPS, 1.01f},
};
constexpr StatWeight CASTER_DAMAGE[] = {
    {STATS_TYPE_INTELLECT, 0.5f},   {STATS_TYPE_SPELL_POWER, 1.0f}, {STATS_TYPE_HIT, 1.1f},
    {STATS_TYPE_CRIT, 0.8f},        {STATS_TYPE_HASTE, 1.0f},       {STATS_TYPE_MANA_REGENERATION, 0.4f},
    {STATS_TYPE_RANGED_DPS, 1.01f},
};
constexpr StatWeight FERAL_OTHER[] = {
    {STATS_TYPE_INTELLECT, 0.1f},
    {STATS_TYPE_SPIRIT, 0.1f},
    {STATS_TYPE_SPELL_POWER, -2.0f},
    {STATS_TYPE_SPELL_PENETRATION, -1.0f},
};

using WeightRow = std::array<float, STATS_TYPE_MAX>;
using WeightTable = std::array<WeightRow, WEIGHT_SPEC_COUNT>;

WeightRow Row(std::initializer_list<std::span<StatWeight const>> shared, std::initializer_list<StatWeight> own = {})
{
    WeightRow row{};
    for (StatWeight const& entry : BASE_WEIGHTS)
        row[entry.stat] = entry.weight;
    for (std::span<StatWeight const> part : shared)
        for (StatWeight const& entry : part)
            row[entry.stat] = entry.weight;
    for (StatWeight const& entry : own)
        row[entry.stat] = entry.weight;
    return row;
}

WeightTable BuildWeightTable()
{
    WeightTable table{};
    table[WEIGHT_WARRIOR_ARMS] = Row({PHYSICAL_SIDE, PHYSICAL_FOREIGN}, {
                                                                            {STATS_TYPE_STRENGTH, 2.5f},
                                                                            {STATS_TYPE_AGILITY, 1.4f},
                                                                            {STATS_TYPE_ATTACK_POWER, 0.8f},
                                                                            {STATS_TYPE_ARMOR_PENETRATION, 1.7f},
                                                                            {STATS_TYPE_HIT, 2.0f},
                                                                            {STATS_TYPE_CRIT, 1.9f},
                                                                            {STATS_TYPE_HASTE, 0.8f},
                                                                            {STATS_TYPE_EXPERTISE, 1.4f},
                                                                            {STATS_TYPE_MELEE_DPS, 7.01f},
                                                                            {STATS_TYPE_SPIRIT, 0.15f},
                                                                        });
    table[WEIGHT_WARRIOR_FURY] = Row({PHYSICAL_SIDE, PHYSICAL_FOREIGN}, {
                                                                            {STATS_TYPE_STRENGTH, 2.5f},
                                                                            {STATS_TYPE_AGILITY, 1.4f},
                                                                            {STATS_TYPE_ATTACK_POWER, 0.8f},
                                                                            {STATS_TYPE_ARMOR_PENETRATION, 2.1f},
                                                                            {STATS_TYPE_HIT, 2.3f},
                                                                            {STATS_TYPE_CRIT, 2.2f},
                                                                            {STATS_TYPE_HASTE, 0.8f},
                                                                            {STATS_TYPE_EXPERTISE, 2.5f},
                                                                            {STATS_TYPE_MELEE_DPS, 7.01f},
                                                                            {STATS_TYPE_SPIRIT, 0.15f},
                                                                        });
    table[WEIGHT_WARRIOR_PROTECTION] = Row({PHYSICAL_FOREIGN, TANK_WEIGHTS}, {
                                                                                 {STATS_TYPE_BLOCK_RATING, 1.0f},
                                                                                 {STATS_TYPE_BLOCK_VALUE, 0.5f},
                                                                                 {STATS_TYPE_SPIRIT, 0.15f},
                                                                             });

    table[WEIGHT_PALADIN_HOLY] = Row({CASTER_SIDE, CASTER_FOREIGN, HEALER_INTELLECT});
    table[WEIGHT_PALADIN_PROTECTION] = Row({TANK_WEIGHTS}, {
                                                               {STATS_TYPE_BLOCK_RATING, 1.0f},
                                                               {STATS_TYPE_BLOCK_VALUE, 0.5f},
                                                               {STATS_TYPE_INTELLECT, -0.5f},
                                                               {STATS_TYPE_SPELL_POWER, -1.5f},
                                                               {STATS_TYPE_SPELL_PENETRATION, -1.0f},
                                                               {STATS_TYPE_MANA_REGENERATION, -1.0f},
                                                               {STATS_TYPE_SPIRIT, 0.05f},
                                                           });
    table[WEIGHT_PALADIN_RETRIBUTION] = Row({PHYSICAL_SIDE}, {
                                                                 {STATS_TYPE_STRENGTH, 2.5f},
                                                                 {STATS_TYPE_AGILITY, 1.0f},
                                                                 {STATS_TYPE_ATTACK_POWER, 0.5f},
                                                                 {STATS_TYPE_ARMOR_PENETRATION, 1.5f},
                                                                 {STATS_TYPE_HIT, 1.9f},
                                                                 {STATS_TYPE_CRIT, 1.7f},
                                                                 {STATS_TYPE_HASTE, 1.6f},
                                                                 {STATS_TYPE_EXPERTISE, 2.0f},
                                                                 {STATS_TYPE_MELEE_DPS, 9.01f},
                                                                 {STATS_TYPE_INTELLECT, -0.5f},
                                                                 {STATS_TYPE_SPELL_POWER, -1.0f},
                                                                 {STATS_TYPE_SPELL_PENETRATION, -1.0f},
                                                                 {STATS_TYPE_MANA_REGENERATION, -1.0f},
                                                                 {STATS_TYPE_SPIRIT, 0.1f},
                                                             });

    table[WEIGHT_HUNTER_BEAST_MASTERY] = Row({PHYSICAL_SIDE, HUNTER_FOREIGN}, {
                                                                                  {STATS_TYPE_STAMINA, 0.6f},
                                                                                  {STATS_TYPE_AGILITY, 2.5f},
                                                                                  {STATS_TYPE_ATTACK_POWER, 1.0f},
                                                                                  {STATS_TYPE_ARMOR_PENETRATION, 1.5f},
                                                                                  {STATS_TYPE_HIT, 1.7f},
                                                                                  {STATS_TYPE_CRIT, 1.4f},
                                                                                  {STATS_TYPE_HASTE, 1.6f},
                                                                                  {STATS_TYPE_RANGED_DPS, 7.51f},
                                                                                  {STATS_TYPE_INTELLECT, 0.4f},
                                                                                  {STATS_TYPE_SPIRIT, 0.1f},
                                                                                  {STATS_TYPE_MANA_REGENERATION, 0.3f},
                                                                              });
    table[WEIGHT_HUNTER_MARKSMANSHIP] = Row({PHYSICAL_SIDE, HUNTER_FOREIGN}, {
                                                                                 {STATS_TYPE_STAMINA, 0.6f},
                                                                                 {STATS_TYPE_AGILITY, 2.3f},
                                                                                 {STATS_TYPE_ATTACK_POWER, 1.0f},
                                                                                 {STATS_TYPE_ARMOR_PENETRATION, 2.25f},
                                                                                 {STATS_TYPE_HIT, 2.1f},
                                                                                 {STATS_TYPE_CRIT, 2.0f},
                                                                                 {STATS_TYPE_HASTE, 1.8f},
                                                                                 {STATS_TYPE_RANGED_DPS, 10.01f},
                                                                                 {STATS_TYPE_INTELLECT, 0.9f},
                                                                                 {STATS_TYPE_SPIRIT, 0.1f},
                                                                                 {STATS_TYPE_MANA_REGENERATION, 0.3f},
                                                                             });
    table[WEIGHT_HUNTER_SURVIVAL] = table[WEIGHT_HUNTER_BEAST_MASTERY];
    table[WEIGHT_HUNTER_SURVIVAL][STATS_TYPE_STAMINA] = 0.8f;

    table[WEIGHT_ROGUE_ASSASSINATION] = Row({PHYSICAL_SIDE, PHYSICAL_FOREIGN, ROGUE_DAGGER});
    table[WEIGHT_ROGUE_COMBAT] = Row({PHYSICAL_SIDE, PHYSICAL_FOREIGN}, {
                                                                            {STATS_TYPE_AGILITY, 1.9f},
                                                                            {STATS_TYPE_STRENGTH, 1.1f},
                                                                            {STATS_TYPE_ATTACK_POWER, 1.0f},
                                                                            {STATS_TYPE_ARMOR_PENETRATION, 1.8f},
                                                                            {STATS_TYPE_HIT, 2.1f},
                                                                            {STATS_TYPE_CRIT, 1.4f},
                                                                            {STATS_TYPE_HASTE, 1.7f},
                                                                            {STATS_TYPE_EXPERTISE, 2.0f},
                                                                            {STATS_TYPE_MELEE_DPS, 7.01f},
                                                                            {STATS_TYPE_SPIRIT, 0.15f},
                                                                        });
    table[WEIGHT_ROGUE_SUBTLETY] = table[WEIGHT_ROGUE_ASSASSINATION];

    table[WEIGHT_PRIEST_DISCIPLINE] = Row({CASTER_SIDE, CASTER_FOREIGN, HEALER_SPIRIT});
    table[WEIGHT_PRIEST_HOLY] = table[WEIGHT_PRIEST_DISCIPLINE];
    table[WEIGHT_PRIEST_SHADOW] = Row({CASTER_SIDE, CASTER_FOREIGN, CASTER_DAMAGE}, {{STATS_TYPE_SPIRIT, 0.4f}});

    table[WEIGHT_DEATH_KNIGHT_BLOOD] = Row({PHYSICAL_FOREIGN, TANK_WEIGHTS}, {
                                                                                 {STATS_TYPE_BLOCK_RATING, -2.0f},
                                                                                 {STATS_TYPE_BLOCK_VALUE, -2.0f},
                                                                                 {STATS_TYPE_SPIRIT, 0.15f},
                                                                             });
    table[WEIGHT_DEATH_KNIGHT_FROST] = Row({PHYSICAL_SIDE, PHYSICAL_FOREIGN}, {
                                                                                  {STATS_TYPE_STRENGTH, 2.5f},
                                                                                  {STATS_TYPE_AGILITY, 0.5f},
                                                                                  {STATS_TYPE_ATTACK_POWER, 0.5f},
                                                                                  {STATS_TYPE_ARMOR_PENETRATION, 2.7f},
                                                                                  {STATS_TYPE_HIT, 2.3f},
                                                                                  {STATS_TYPE_CRIT, 2.2f},
                                                                                  {STATS_TYPE_HASTE, 2.1f},
                                                                                  {STATS_TYPE_EXPERTISE, 2.5f},
                                                                                  {STATS_TYPE_MELEE_DPS, 7.01f},
                                                                                  {STATS_TYPE_SPIRIT, 0.15f},
                                                                              });
    table[WEIGHT_DEATH_KNIGHT_UNHOLY] = Row({PHYSICAL_SIDE, PHYSICAL_FOREIGN}, {
                                                                                   {STATS_TYPE_STRENGTH, 2.5f},
                                                                                   {STATS_TYPE_AGILITY, 0.5f},
                                                                                   {STATS_TYPE_ATTACK_POWER, 0.5f},
                                                                                   {STATS_TYPE_ARMOR_PENETRATION, 1.3f},
                                                                                   {STATS_TYPE_HIT, 2.2f},
                                                                                   {STATS_TYPE_CRIT, 1.7f},
                                                                                   {STATS_TYPE_HASTE, 1.8f},
                                                                                   {STATS_TYPE_EXPERTISE, 1.5f},
                                                                                   {STATS_TYPE_MELEE_DPS, 5.01f},
                                                                                   {STATS_TYPE_SPIRIT, 0.15f},
                                                                               });

    table[WEIGHT_SHAMAN_ELEMENTAL] = Row({CASTER_SIDE, CASTER_FOREIGN}, {
                                                                            {STATS_TYPE_INTELLECT, 0.6f},
                                                                            {STATS_TYPE_SPELL_POWER, 1.2f},
                                                                            {STATS_TYPE_HIT, 1.1f},
                                                                            {STATS_TYPE_CRIT, 0.8f},
                                                                            {STATS_TYPE_HASTE, 1.0f},
                                                                            {STATS_TYPE_MANA_REGENERATION, 0.5f},
                                                                            {STATS_TYPE_SPIRIT, 0.1f},
                                                                        });
    table[WEIGHT_SHAMAN_ENHANCEMENT] = Row({PHYSICAL_SIDE}, {
                                                                {STATS_TYPE_AGILITY, 1.4f},
                                                                {STATS_TYPE_STRENGTH, 1.1f},
                                                                {STATS_TYPE_INTELLECT, 1.2f},
                                                                {STATS_TYPE_ATTACK_POWER, 1.0f},
                                                                {STATS_TYPE_SPELL_POWER, 0.5f},
                                                                {STATS_TYPE_ARMOR_PENETRATION, 0.9f},
                                                                {STATS_TYPE_HIT, 2.1f},
                                                                {STATS_TYPE_CRIT, 1.5f},
                                                                {STATS_TYPE_HASTE, 1.8f},
                                                                {STATS_TYPE_EXPERTISE, 2.0f},
                                                                {STATS_TYPE_MELEE_DPS, 8.51f},
                                                                {STATS_TYPE_SPIRIT, 0.1f},
                                                                {STATS_TYPE_MANA_REGENERATION, 0.3f},
                                                            });
    table[WEIGHT_SHAMAN_RESTORATION] = Row({CASTER_SIDE, CASTER_FOREIGN, HEALER_INTELLECT});

    table[WEIGHT_MAGE_ARCANE] = Row({CASTER_SIDE, CASTER_FOREIGN, CASTER_DAMAGE}, {{STATS_TYPE_SPIRIT, 0.3f}});
    table[WEIGHT_MAGE_FIRE] = Row({CASTER_SIDE, CASTER_FOREIGN, CASTER_DAMAGE}, {
                                                                                    {STATS_TYPE_SPIRIT, 0.3f},
                                                                                    {STATS_TYPE_HIT, 1.2f},
                                                                                    {STATS_TYPE_CRIT, 1.1f},
                                                                                    {STATS_TYPE_HASTE, 0.8f},
                                                                                });
    table[WEIGHT_MAGE_FROST] = table[WEIGHT_MAGE_ARCANE];

    table[WEIGHT_WARLOCK_AFFLICTION] = Row({CASTER_SIDE, CASTER_FOREIGN, CASTER_DAMAGE}, {{STATS_TYPE_SPIRIT, 0.4f}});
    table[WEIGHT_WARLOCK_DEMONOLOGY] = table[WEIGHT_WARLOCK_AFFLICTION];
    table[WEIGHT_WARLOCK_DESTRUCTION] = table[WEIGHT_WARLOCK_AFFLICTION];

    table[WEIGHT_DRUID_BALANCE] = table[WEIGHT_WARLOCK_AFFLICTION];
    table[WEIGHT_DRUID_CAT] = Row({PHYSICAL_SIDE, FERAL_OTHER}, {
                                                                    {STATS_TYPE_AGILITY, 2.2f},
                                                                    {STATS_TYPE_STRENGTH, 2.4f},
                                                                    {STATS_TYPE_ATTACK_POWER, 1.0f},
                                                                    {STATS_TYPE_ARMOR_PENETRATION, 2.3f},
                                                                    {STATS_TYPE_HIT, 1.9f},
                                                                    {STATS_TYPE_CRIT, 1.5f},
                                                                    {STATS_TYPE_HASTE, 2.1f},
                                                                    {STATS_TYPE_EXPERTISE, 2.1f},
                                                                });
    table[WEIGHT_DRUID_RESTORATION] = table[WEIGHT_PRIEST_DISCIPLINE];
    table[WEIGHT_DRUID_BEAR] = Row({FERAL_OTHER}, {
                                                      {STATS_TYPE_AGILITY, 2.2f},
                                                      {STATS_TYPE_STRENGTH, 1.2f},
                                                      {STATS_TYPE_STAMINA, 4.1f},
                                                      {STATS_TYPE_ATTACK_POWER, 1.0f},
                                                      {STATS_TYPE_DEFENSE, 1.3f},
                                                      {STATS_TYPE_DODGE, 1.4f},
                                                      {STATS_TYPE_ARMOR, 0.151f},
                                                      {STATS_TYPE_HIT, 1.5f},
                                                      {STATS_TYPE_CRIT, 0.8f},
                                                      {STATS_TYPE_HASTE, 1.0f},
                                                      {STATS_TYPE_EXPERTISE, 2.5f},
                                                  });
    return table;
}

WeightTable const& Weights()
{
    static WeightTable const table = BuildWeightTable();
    return table;
}

// The row of a class and talent tab; a feral druid is a bear when its strategies tank
WeightSpec SpecOf(uint8 cls, int tab, bool feralTank)
{
    uint8 const offset = tab > 0 && tab < 3 ? uint8(tab) : 0;
    switch (cls)
    {
        case CLASS_WARRIOR:
            return WeightSpec(WEIGHT_WARRIOR_ARMS + offset);
        case CLASS_PALADIN:
            return WeightSpec(WEIGHT_PALADIN_HOLY + offset);
        case CLASS_HUNTER:
            return WeightSpec(WEIGHT_HUNTER_BEAST_MASTERY + offset);
        case CLASS_ROGUE:
            return WeightSpec(WEIGHT_ROGUE_ASSASSINATION + offset);
        case CLASS_PRIEST:
            return WeightSpec(WEIGHT_PRIEST_DISCIPLINE + offset);
        case CLASS_DEATH_KNIGHT:
            return WeightSpec(WEIGHT_DEATH_KNIGHT_BLOOD + offset);
        case CLASS_SHAMAN:
            return WeightSpec(WEIGHT_SHAMAN_ELEMENTAL + offset);
        case CLASS_MAGE:
            return WeightSpec(WEIGHT_MAGE_ARCANE + offset);
        case CLASS_WARLOCK:
            return WeightSpec(WEIGHT_WARLOCK_AFFLICTION + offset);
        case CLASS_DRUID:
            if (offset == DRUID_TAB_FERAL && feralTank)
                return WEIGHT_DRUID_BEAR;
            return WeightSpec(WEIGHT_DRUID_BALANCE + offset);
        default:
            return WEIGHT_WARRIOR_ARMS;
    }
}

WeightRole RoleOf(uint8 spec)
{
    switch (spec)
    {
        case WEIGHT_WARRIOR_PROTECTION:
        case WEIGHT_PALADIN_PROTECTION:
        case WEIGHT_DEATH_KNIGHT_BLOOD:
        case WEIGHT_DRUID_BEAR:
            return WeightRole::Tank;
        case WEIGHT_PALADIN_HOLY:
        case WEIGHT_PRIEST_DISCIPLINE:
        case WEIGHT_PRIEST_HOLY:
        case WEIGHT_SHAMAN_RESTORATION:
        case WEIGHT_DRUID_RESTORATION:
            return WeightRole::Healer;
        case WEIGHT_PRIEST_SHADOW:
        case WEIGHT_SHAMAN_ELEMENTAL:
        case WEIGHT_MAGE_ARCANE:
        case WEIGHT_MAGE_FIRE:
        case WEIGHT_MAGE_FROST:
        case WEIGHT_WARLOCK_AFFLICTION:
        case WEIGHT_WARLOCK_DEMONOLOGY:
        case WEIGHT_WARLOCK_DESTRUCTION:
        case WEIGHT_DRUID_BALANCE:
            return WeightRole::Caster;
        default:
            return WeightRole::Physical;
    }
}
}  // namespace

StatsWeightCalculator::StatsWeightCalculator(Player* player) : player_(player)
{
    if (PlayerbotAI::IsHeal(player))
        type_ = CollectorType::SPELL_HEAL;
    else if (PlayerbotAI::IsCaster(player))
        type_ = CollectorType::SPELL_DMG;
    else if (PlayerbotAI::IsTank(player))
        type_ = CollectorType::MELEE_TANK;
    else if (PlayerbotAI::IsMelee(player))
        type_ = CollectorType::MELEE_DMG;
    else
        type_ = CollectorType::RANGED;
    cls = player->getClass();
    lvl = player->GetLevel();
    tab = AiFactory::GetPlayerSpecTab(player);
    collector_ = std::make_unique<StatsCollector>(type_, cls, lvl);
    // By talent spec, like the weights in GenerateBasicWeights(); type_ follows the strategies
    healSpec_ = PlayerbotAI::IsHeal(player, true);
    casterSpec_ = !healSpec_ && PlayerbotAI::IsCaster(player, true);

    if (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_UNHOLY)
        hitOverflowType_ = CollectorType::SPELL;
    else if (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT)
        hitOverflowType_ = CollectorType::SPELL;
    else if (cls == CLASS_ROGUE)
        hitOverflowType_ = CollectorType::SPELL;
    else
        hitOverflowType_ = type_;

    enable_overflow_penalty_ = true;
    enable_item_set_bonus_ = true;
}

void StatsWeightCalculator::SetUpForEquipCompare()
{
    SetItemSetBonus(false);
    SetOverflowPenalty(false);
    if (sRandomPlayerbotMgr.IsSpecPvp(player_->GetGUID().GetCounter(), player_->getClass()))
        SetPvpSpec(true);
}

void StatsWeightCalculator::Reset()
{
    collector_->Reset();
    weight_ = 0;
    for (uint32 i = 0; i < STATS_TYPE_MAX; i++)
    {
        stats_weights_[i] = 0;
    }
}

float StatsWeightCalculator::CalculateItem(uint32 itemId, int32 randomPropertyIds, int32 slot)
{
    ItemTemplate const* proto = &sObjectMgr->GetItemTemplateStore()->at(itemId);

    if (!proto)
        return 0.0f;

    Reset();

    collector_->CollectItemStats(proto);

    if (randomPropertyIds != 0)
        CalculateRandomProperty(randomPropertyIds, itemId);

    ApplyRacialWeaponBonus(proto);

    bool const checkSetBonus = enable_item_set_bonus_;
    std::array<float, STATS_TYPE_MAX> setBonusStats{};
    std::array<float, STATS_TYPE_MAX> setBonusBaseline{};

    // Clamp the item's own stats first so the tier below is valued only on what the player can
    // still use (a tier's hit at the hit cap must not make the piece look valued).
    if (enable_overflow_penalty_)
        ApplyOverflowPenalty(player_);

    if (checkSetBonus)
    {
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            setBonusBaseline[i] = collector_->stats[i];
        CollectItemSetBonus(player_, proto, slot, setBonusStats);

        // Clamp the combined stats: the tier's contribution is capped the same way.
        if (enable_overflow_penalty_)
            ApplyOverflowPenalty(player_);
    }

    GenerateWeights(player_);
    float usefulPart = 0.0f;
    bool weighted = false;
    for (uint32 i = 0; i < STATS_TYPE_MAX; i++)
    {
        float const value = stats_weights_[i] * collector_->stats[i];
        weight_ += value;
        if (value > 0.0f)
            usefulPart += value;
        if (stats_weights_[i] != 0.0f && collector_->stats[i] != 0.0f)
            weighted = true;
    }
    if (std::fabs(weight_) < SCORE_ROUNDING)
        weight_ = 0.0f;
    usefulStat_ = usefulPart > 0.0f || !weighted;

    // Value the tier on the stats that survive the clamp, on top of the item's own clamped
    // stats. A tier whose weighted delta is ~zero (most raid tier procs only produce
    // STATS_TYPE_BONUS, capped hit) falls back to the legacy completeness multiplier.
    float setBonusValue = 0.0f;
    if (checkSetBonus)
    {
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            if (i != STATS_TYPE_BONUS)
                setBonusValue += stats_weights_[i] * (collector_->stats[i] - setBonusBaseline[i]);
    }
    bool const setBonusValued = setBonusValue > 0.0001f;

    // The item type, set, socket and weapon speed factors scale a positive score only: a factor below 1 would raise a
    // negative one
    if (weight_ <= 0.0f)
        return weight_;

    CalculateItemTypePenalty(proto);

    if (enable_item_set_bonus_ && !setBonusValued)
        CalculateItemSetMod(player_, proto);

    CalculateSocketBonus(player_, proto);

    // Apply weapon speed governance if slot is provided and this is a weapon
    if (sPlayerbotAIConfig.PreferredSpecWeapons && slot >= 0 && proto->Class == ITEM_CLASS_WEAPON)
        weight_ *= ApplyPreferredSpecWeapons(proto, slot);

    return weight_;
}

float StatsWeightCalculator::CalculateEnchant(uint32 enchantId, uint32 weaponDelay, bool isRanged)
{
    SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);

    if (!enchant)
        return 0.0f;

    Reset();

    collector_->CollectEnchantStats(enchant, 0, weaponDelay, isRanged);

    if (enable_overflow_penalty_)
        ApplyOverflowPenalty(player_);

    GenerateWeights(player_);
    for (uint32 i = 0; i < STATS_TYPE_MAX; i++)
    {
        weight_ += stats_weights_[i] * collector_->stats[i];
    }

    return weight_;
}

void StatsWeightCalculator::CalculateRandomProperty(int32 randomPropertyId, uint32 itemId)
{
    if (randomPropertyId > 0)
    {
        ItemRandomPropertiesEntry const* item_rand = sItemRandomPropertiesStore.LookupEntry(randomPropertyId);
        if (!item_rand)
        {
            return;
        }

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < MAX_ENCHANTMENT_SLOT; ++i)
        {
            uint32 enchantId = item_rand->Enchantment[i - PROP_ENCHANTMENT_SLOT_0];
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
            if (enchant)
                collector_->CollectEnchantStats(enchant);
        }
    }
    else
    {
        ItemRandomSuffixEntry const* item_rand = sItemRandomSuffixStore.LookupEntry(-randomPropertyId);
        if (!item_rand)
        {
            return;
        }

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < MAX_ENCHANTMENT_SLOT; ++i)
        {
            uint32 enchantId = item_rand->Enchantment[i - PROP_ENCHANTMENT_SLOT_0];
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
            uint32 enchant_amount = 0;

            for (int k = 0; k < MAX_ITEM_ENCHANTMENT_EFFECTS; ++k)
            {
                if (item_rand->Enchantment[k] == enchantId)
                {
                    enchant_amount = uint32((item_rand->AllocationPct[k] * GenerateEnchSuffixFactor(itemId)) / 10000);
                    break;
                }
            }

            if (enchant)
                collector_->CollectEnchantStats(enchant, enchant_amount);
        }
    }
}

int32 StatsWeightCalculator::PickBestRandomPropertyId(uint32 itemId)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return 0;

    bool isSuffix = false;
    uint32 poolEntry = proto->RandomProperty;
    if (!poolEntry)
    {
        poolEntry = proto->RandomSuffix;
        isSuffix = true;
    }
    if (!poolEntry)
        return 0;

    std::vector<uint32> const& pool = sRandomItemMgr.GetEnchantmentPool(poolEntry);
    if (pool.empty())
        return 0;

    Reset();
    GenerateWeights(player_);

    int32 bestId = 0;
    float bestScore = 0.0f;
    for (uint32 enchId : pool)
    {
        int32 candidate = isSuffix ? -static_cast<int32>(enchId) : static_cast<int32>(enchId);

        collector_->Reset();
        CalculateRandomProperty(candidate, itemId);

        float score = 0.0f;
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            score += stats_weights_[i] * collector_->stats[i];

        if (bestId == 0 || score > bestScore)
        {
            bestId = candidate;
            bestScore = score;
        }
    }

    collector_->Reset();
    return bestId;
}

void StatsWeightCalculator::GenerateWeights(Player* player)
{
    GenerateBasicWeights(player);
    GenerateAdditionalWeights(player);
    ApplyWeightFinetune(player);
}

void StatsWeightCalculator::GenerateBasicWeights(Player* player)
{
    bool const feralTank = cls == CLASS_DRUID && tab == DRUID_TAB_FERAL && PlayerbotAI::IsTank(player);
    spec_ = SpecOf(cls, tab, feralTank);
    WeightRow const& row = Weights()[spec_];
    for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
        stats_weights_[i] += row[i];
}

void StatsWeightCalculator::GenerateAdditionalWeights(Player* /*player*/)
{
    // Healers would rather survive the hits (Greater Resistance), casters shed threat (Enchant Cloak - Subtlety),
    // tanks want more threat (Enchant Gloves - Threat). Melee and hunters weigh neither.
    if (healSpec_)
    {
        stats_weights_[STATS_TYPE_THREAT_REDUCTION] += HEALER_THREAT_REDUCTION_WEIGHT;
        stats_weights_[STATS_TYPE_RESISTANCE] += HEALER_RESISTANCE_WEIGHT;
    }
    else if (casterSpec_)
    {
        stats_weights_[STATS_TYPE_THREAT_REDUCTION] += CASTER_THREAT_REDUCTION_WEIGHT;
        stats_weights_[STATS_TYPE_RESISTANCE] += CASTER_RESISTANCE_WEIGHT;
    }
    else if (type_ == CollectorType::MELEE_TANK)
        stats_weights_[STATS_TYPE_THREAT_REDUCTION] += TANK_THREAT_REDUCTION_WEIGHT;

    if (!pvpSpec_)
        return;

    WeightRole const role = RoleOf(spec_);
    bool const caster = role == WeightRole::Healer || role == WeightRole::Caster;
    if (!exclude_resilience_)
        stats_weights_[STATS_TYPE_RESILIENCE] += caster ? PVP_CASTER_RESILIENCE_WEIGHT : PVP_PHYSICAL_RESILIENCE_WEIGHT;
    if (role != WeightRole::Tank)
        stats_weights_[STATS_TYPE_STAMINA] *= PVP_STAMINA_FACTOR;
    if (role == WeightRole::Caster)
        stats_weights_[STATS_TYPE_SPELL_PENETRATION] += PVP_SPELL_PENETRATION_WEIGHT;
}

void StatsWeightCalculator::CollectItemSetBonus(Player* player, ItemTemplate const* proto, int32 slot,
                                                std::array<float, STATS_TYPE_MAX>& setBonusStats)
{
    uint32 itemSet = proto->ItemSet;
    if (!itemSet)
        return;

    ItemSetEntry const* setEntry = sItemSetStore.LookupEntry(itemSet);
    if (!setEntry)
        return;

    // Pieces worn while the candidate is evaluated: the item currently in the candidate's
    // slot (if it is from the same set) is replaced, so it must not count. The candidate
    // itself adds one piece, so only the tier at (worn + 1) is newly activated.
    uint32 worn = 0;
    for (ItemSetEffect* eff : player->ItemSetEff)
    {
        if (eff && eff->setid == itemSet)
        {
            worn = eff->item_count;
            break;
        }
    }

    if (worn && slot >= 0 && slot < EQUIPMENT_SLOT_END)
    {
        if (Item* oldItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(slot)))
        {
            if (oldItem->GetTemplate()->ItemSet == itemSet)
                --worn;
        }
    }

    // Value only the tier this item switches on. Tiers that are already active stay
    // credited to the item providing them, so candidates are compared on their delta.
    // Keep the stats the tier adds and let the caller weigh them after GenerateWeights.
    for (uint32 j = 0; j < MAX_ITEM_SET_SPELLS; ++j)
    {
        uint32 threshold = setEntry->items_to_triggerspell[j];
        uint32 spellId = setEntry->spells[j];
        if (!threshold || !spellId || threshold != worn + 1)
            continue;

        std::array<float, STATS_TYPE_MAX> before{};
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            before[i] = collector_->stats[i];

        collector_->CollectSpellStats(spellId, 1.0f, Milliseconds(0));

        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            setBonusStats[i] += collector_->stats[i] - before[i];
    }
}

void StatsWeightCalculator::CalculateItemSetMod(Player* player, ItemTemplate const* proto)
{
    uint32 itemSet = proto->ItemSet;
    if (!itemSet)
        return;

    float multiplier = 1.0f;
    size_t i = 0;
    for (i = 0; i < player->ItemSetEff.size(); i++)
    {
        if (player->ItemSetEff[i])
        {
            ItemSetEffect* eff = player->ItemSetEff[i];

            uint32 setId = eff->setid;
            if (itemSet != setId)
                continue;

            ItemSetEntry const* setEntry = sItemSetStore.LookupEntry(setId);
            if (!setEntry)
                continue;

            uint32 itemCount = eff->item_count;
            uint32 max_items = 0;
            for (size_t j = 0; j < MAX_ITEM_SET_SPELLS; j++)
                max_items = std::max(max_items, setEntry->items_to_triggerspell[j]);
            if (itemCount < max_items)
            {
                multiplier += 0.1f * itemCount;  // 10% bonus for each item already equipped
            }
            else
            {
                multiplier = 1.0f;  // All item set effect has been triggered
            }
            break;
        }
    }

    if (i == player->ItemSetEff.size())
        multiplier = 1.05f;  // this is the first item in the item set

    weight_ *= multiplier;
}

void StatsWeightCalculator::CalculateSocketBonus(Player* /*player*/, ItemTemplate const* proto)
{
    uint32 socketNum = 0;
    for (uint32 enchant_slot = SOCK_ENCHANTMENT_SLOT; enchant_slot < SOCK_ENCHANTMENT_SLOT + MAX_GEM_SOCKETS;
         ++enchant_slot)
    {
        uint8 socketColor = proto->Socket[enchant_slot - SOCK_ENCHANTMENT_SLOT].Color;

        if (!socketColor)  // no socket slot
            continue;

        socketNum++;
    }

    float multiplier = 1.0f + socketNum * 0.03f;  // 3% bonus for socket

    weight_ *= multiplier;
}

void StatsWeightCalculator::CalculateItemTypePenalty(ItemTemplate const* proto)
{
    // // penalty for different type armor
    // if (proto->Class == ITEM_CLASS_ARMOR && proto->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH &&
    //     proto->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE && NotBestArmorType(proto->SubClass))
    // {
    //     weight_ *= 1.0;
    // }
    if (proto->Class == ITEM_CLASS_WEAPON)
    {
        // double hand
        bool isDoubleHand = proto->Class == ITEM_CLASS_WEAPON &&
                            !(ITEM_SUBCLASS_MASK_SINGLE_HAND & (1 << proto->SubClass)) &&
                            !(ITEM_SUBCLASS_MASK_WEAPON_RANGED & (1 << proto->SubClass));

        if (isDoubleHand)
        {
            weight_ *= 0.5;
            // spec without double hand
            // enhancement, rogue, ice dk, unholy dk, shield tank, fury warrior without titan's grip but with duel wield
            if (((cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT && player_->CanDualWield()) ||
                 (cls == CLASS_ROGUE) || (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_FROST) ||
                 (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY && !player_->CanTitanGrip() &&
                  player_->CanDualWield()) ||
                 (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_PROTECTION) ||
                 (cls == CLASS_PALADIN && tab == PALADIN_TAB_PROTECTION) ||
                 (cls == CLASS_PALADIN && tab == PALADIN_TAB_HOLY)))
            {
                weight_ *= 0.1;
            }
        }
        // spec with double hand
        // fury without duel wield, arms, bear, retribution, blood dk
        if (!isDoubleHand)
        {
            if ((cls == CLASS_HUNTER && !player_->CanDualWield()) ||
                (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY && !player_->CanDualWield()) ||
                (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_ARMS) || (cls == CLASS_DRUID && tab == DRUID_TAB_FERAL) ||
                (cls == CLASS_PALADIN && tab == PALADIN_TAB_RETRIBUTION) ||
                (cls == CLASS_DEATH_KNIGHT && tab == DEATH_KNIGHT_TAB_BLOOD) ||
                (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT && !player_->CanDualWield()))
            {
                weight_ *= 0.1;
            }
            // caster's main hand (cannot duel weapon but can equip two-hands stuff)
            if ((cls == CLASS_MAGE || cls == CLASS_PRIEST || cls == CLASS_WARLOCK || cls == CLASS_DRUID ||
                (cls == CLASS_SHAMAN && !player_->CanDualWield())) &&
                !(cls == CLASS_PALADIN && tab == PALADIN_TAB_HOLY))
            {
                weight_ *= 0.65;
            }
            if (cls == CLASS_PALADIN && tab == PALADIN_TAB_HOLY)
            {
                weight_ *= 0.8;
            }
        }
        // fury with titan's grip
        if ((!isDoubleHand || proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM ||
             proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF) &&
            (cls == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY && player_->CanTitanGrip()))
        {
            weight_ *= 0.1;
        }

        if (cls == CLASS_HUNTER && proto->SubClass == ITEM_SUBCLASS_WEAPON_THROWN)
        {
            weight_ *= 0.1;
        }

        if (lvl >= 10 && cls == CLASS_ROGUE && (tab == ROGUE_TAB_ASSASSINATION || tab == ROGUE_TAB_SUBTLETY) &&
            proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER)
        {
            weight_ *= 1.5;
        }

        if (cls == CLASS_ROGUE && player_->HasAura(SPELL_ROGUE_SWORD_SPECIALIZATION) &&
            (proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE))
        {
            weight_ *= 1.1;
        }
        if (cls == CLASS_WARRIOR && player_->HasAura(SPELL_POLEAXE_SPECIALIZATION) &&
            (proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM || proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2))
        {
            weight_ *= 1.1;
        }
        if (cls == CLASS_DEATH_KNIGHT && player_->HasAura(SPELL_NERVES_OF_COLD_STEEL) && !isDoubleHand)
        {
            weight_ *= 1.3;
        }
        bool slowDelay = proto->Delay > 2500;
        if (cls == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT && slowDelay)
            weight_ *= 1.1;
    }
}

bool StatsWeightCalculator::NotBestArmorType(uint32 item_subclass_armor)
{
    if (player_->HasSkill(SKILL_PLATE_MAIL))
    {
        return item_subclass_armor != ITEM_SUBCLASS_ARMOR_PLATE;
    }
    if (player_->HasSkill(SKILL_MAIL))
    {
        return item_subclass_armor != ITEM_SUBCLASS_ARMOR_MAIL;
    }
    if (player_->HasSkill(SKILL_LEATHER))
    {
        return item_subclass_armor != ITEM_SUBCLASS_ARMOR_LEATHER;
    }
    return false;
}

void StatsWeightCalculator::ApplyOverflowPenalty(Player* player)
{
    {
        float hit_current, hit_overflow;
        float validPoints;
        if (hitOverflowType_ & CollectorType::SPELL)
        {
            hit_current = player->GetTotalAuraModifier(SPELL_AURA_MOD_SPELL_HIT_CHANCE);
            hit_current +=
                player->GetTotalAuraModifier(SPELL_AURA_MOD_INCREASES_SPELL_PCT_TO_HIT);  // suppression (18176)
            hit_current += player->GetRatingBonusValue(CR_HIT_SPELL);

            if (cls == CLASS_PRIEST && tab == PRIEST_TAB_SHADOW && player->HasAura(SPELL_SHADOW_FOCUS))
                hit_current += 3;
            if (cls == CLASS_MAGE && tab == MAGE_TAB_ARCANE && player->HasAura(SPELL_ARCANE_FOCUS))
                hit_current += 3;

            hit_overflow = SPELL_HIT_OVERFLOW;
            if (hit_overflow > hit_current)
                validPoints = (hit_overflow - hit_current) / player->GetRatingMultiplier(CR_HIT_SPELL);
            else
                validPoints = 0;
        }
        else if (hitOverflowType_ & CollectorType::MELEE)
        {
            hit_current = player->GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
            hit_current += player->GetRatingBonusValue(CR_HIT_MELEE);
            hit_overflow = MELEE_HIT_OVERFLOW;
            if (hit_overflow > hit_current)
                validPoints = (hit_overflow - hit_current) / player->GetRatingMultiplier(CR_HIT_MELEE);
            else
                validPoints = 0;
        }
        else
        {
            hit_current = player->GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
            hit_current += player->GetRatingBonusValue(CR_HIT_RANGED);
            hit_overflow = RANGED_HIT_OVERFLOW;
            if (hit_overflow > hit_current)
                validPoints = (hit_overflow - hit_current) / player->GetRatingMultiplier(CR_HIT_RANGED);
            else
                validPoints = 0;
        }
        collector_->stats[STATS_TYPE_HIT] = std::min(collector_->stats[STATS_TYPE_HIT], validPoints);
    }

    {
        if (type_ & CollectorType::MELEE)
        {
            float expertise_current, expertise_overflow;
            expertise_current = player->GetUInt32Value(PLAYER_EXPERTISE);
            expertise_current += player->GetRatingBonusValue(CR_EXPERTISE);
            expertise_overflow = EXPERTISE_OVERFLOW;

            float validPoints;
            if (expertise_overflow > expertise_current)
                validPoints = (expertise_overflow - expertise_current) / player->GetRatingMultiplier(CR_EXPERTISE);
            else
                validPoints = 0;

            collector_->stats[STATS_TYPE_EXPERTISE] = std::min(collector_->stats[STATS_TYPE_EXPERTISE], validPoints);
        }
    }

    {
        if (type_ & CollectorType::MELEE)
        {
            float defense_current, defense_overflow;
            defense_current = player->GetRatingBonusValue(CR_DEFENSE_SKILL);
            defense_overflow = DEFENSE_OVERFLOW;

            float validPoints;
            if (defense_overflow > defense_current)
                validPoints = (defense_overflow - defense_current) / player->GetRatingMultiplier(CR_DEFENSE_SKILL);
            else
                validPoints = 0;

            collector_->stats[STATS_TYPE_DEFENSE] = std::min(collector_->stats[STATS_TYPE_DEFENSE], validPoints);
        }
    }

    {
        if (type_ & (CollectorType::MELEE | CollectorType::RANGED))
        {
            float armor_penetration_current, armor_penetration_overflow;
            armor_penetration_current = player->GetRatingBonusValue(CR_ARMOR_PENETRATION);
            armor_penetration_overflow = ARMOR_PENETRATION_OVERFLOW;

            float validPoints;
            if (armor_penetration_overflow > armor_penetration_current)
                validPoints = (armor_penetration_overflow - armor_penetration_current) /
                              player->GetRatingMultiplier(CR_ARMOR_PENETRATION);
            else
                validPoints = 0;

            collector_->stats[STATS_TYPE_ARMOR_PENETRATION] =
                std::min(collector_->stats[STATS_TYPE_ARMOR_PENETRATION], validPoints);
        }
    }
}

void StatsWeightCalculator::ApplyWeightFinetune(Player* player)
{
    {
        if (type_ & (CollectorType::MELEE | CollectorType::RANGED))
        {
            float armor_penetration_current /*, armor_penetration_overflow*/;  // not used, line marked for removal.
            armor_penetration_current = player->GetRatingBonusValue(CR_ARMOR_PENETRATION);
            if (armor_penetration_current > 50)
                stats_weights_[STATS_TYPE_ARMOR_PENETRATION] *= 1.2f;
        }
    }
}

float StatsWeightCalculator::ApplyPreferredSpecWeapons(ItemTemplate const* proto, int32 slot)
{
    // Multiply score by 3x when this weapon's delay matches the spec-ideal speed.
    float weight = 2.0f;

    // Applies to mainhand, offhand, and ranged slots only.
    if (slot != EQUIPMENT_SLOT_MAINHAND &&
        slot != EQUIPMENT_SLOT_OFFHAND  &&
        slot != EQUIPMENT_SLOT_RANGED)
        return 1.0f;

    uint32 delay = proto->Delay;  // milliseconds
    float boost = 1.0f + weight;  // applied on a match

    // Hunter: melee weapons are stat sticks — speed irrelevant.
    // Ranged weapons scale Aimed/Chimera/Explosive Shot from top-end damage,
    // so a slow ranged weapon (>=2600 ms) is strongly preferred.
    if (cls == CLASS_HUNTER)
    {
        if (slot == EQUIPMENT_SLOT_RANGED && delay >= 2600)
            return boost;
        return 1.0f;
    }

    // Feral Druid: forms normalise attack speed; raw weapon Delay is irrelevant.
    if (cls == CLASS_DRUID && tab == DRUID_TAB_FERAL)
        return 1.0f;

    switch (cls)
    {
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_ARMS)
            {
                // Arms: slow 2H axes or polearms in mainhand only (Axe Specialization: +5% crit).
                bool isAxeOrPolearm = (proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                                       proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM);
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400 && isAxeOrPolearm)
                    return boost;
            }
            else if (tab == WARRIOR_TAB_FURY)
            {
                if (!player_->CanDualWield())
                {
                    // Pre-DW: treat like Arms — slow 2H in mainhand only.
                    if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                        return boost;
                }
                else if (player_->CanTitanGrip())
                {
                    // Titan's Grip: slow 2H (>=3400) in both hands.
                    if (delay >= 3400)
                        return boost;
                }
                else
                {
                    // 1H DW: slow 1H (>=2600) in both hands.
                    // 2H must be excluded — delay >= 2600 would otherwise pass
                    // for a 2H heirloom (~3600ms) just as it did for Enhancement.
                    if (proto->InventoryType == INVTYPE_2HWEAPON)
                        break;
                    if (delay >= 2600)
                        return boost;
                }
            }
            else if (tab == WARRIOR_TAB_PROTECTION)
            {
                // Prot: slow 1H (>=2600) in mainhand. Shield in offhand, no speed bonus.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 2600)
                    return boost;
            }
            break;

        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_RETRIBUTION)
            {
                // Ret: slow 2H in mainhand only.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                    return boost;
            }
            else if (tab == PALADIN_TAB_PROTECTION)
            {
                // Prot: slow 1H (>=2600) in mainhand. Shield in offhand.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 2600)
                    return boost;
            }
            break;

        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_BLOOD || tab == DEATH_KNIGHT_TAB_UNHOLY)
            {
                // Blood / Unholy: slow 2H in mainhand only.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                    return boost;
            }
            else if (tab == DEATH_KNIGHT_TAB_FROST)
            {
                // Frost DK has Dual Wield innately — always dual-wields 1H.
                if (proto->InventoryType == INVTYPE_2HWEAPON)
                    break;
                if (delay >= 2600)
                    return boost;
            }
            break;

        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_ENHANCEMENT)
            {
                if (!player_->CanDualWield())
                {
                    // Pre-Dual Wield: Enhancement plays like a 2H spec.
                    if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                        return boost;
                }
                else
                {
                    // Post-Dual Wield: slow 1H (>=2600) in both hands.
                    if (proto->InventoryType == INVTYPE_2HWEAPON)
                        break;

                    if (delay >= 2600)
                    {
                        float mult = boost;
                        if (slot == EQUIPMENT_SLOT_OFFHAND)
                        {
                            Item* mh = player_->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                            if (mh && mh->GetTemplate() && mh->GetTemplate()->Delay == delay)
                                mult *= boost;  // synchronized: ×(1+weight)² total = ×9 for 2.0f weight
                        }
                        return mult;
                    }
                }
            }
            break;

        case CLASS_ROGUE:
            if (tab == ROGUE_TAB_COMBAT)
            {
                // Combat: slow MH (>=2600), fast OH (<=1500).
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 2600)
                    return boost;
                if (slot == EQUIPMENT_SLOT_OFFHAND && delay <= 1500)
                    return boost;
            }
            else  // Assassination or Subtlety: slow dagger MH, fast dagger OH.
            {
                bool isDagger = (proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER);
                if (slot == EQUIPMENT_SLOT_MAINHAND && isDagger && delay >= 1700)
                    return boost;
                if (slot == EQUIPMENT_SLOT_OFFHAND && isDagger && delay <= 1500)
                    return boost;
            }
            break;

        default:
            break;
    }

    return 1.0f;
}

void StatsWeightCalculator::ApplyRacialWeaponBonus(ItemTemplate const* proto)
{
    if (proto->Class != ITEM_CLASS_WEAPON)
        return;

    uint32 const subClass = proto->SubClass;
    float expertisePoints = 0.0f;
    float critPct = 0.0f;

    switch (player_->getRace(true))
    {
        case RACE_ORC:  // Orc Axe Specialization
            if (subClass == ITEM_SUBCLASS_WEAPON_AXE || subClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                subClass == ITEM_SUBCLASS_WEAPON_FIST)
                expertisePoints = RACIAL_EXPERTISE_ORC_AXE_FIST;
            break;
        case RACE_HUMAN:  // Human Sword & Mace Specialization
            if (subClass == ITEM_SUBCLASS_WEAPON_SWORD || subClass == ITEM_SUBCLASS_WEAPON_SWORD2 ||
                subClass == ITEM_SUBCLASS_WEAPON_MACE || subClass == ITEM_SUBCLASS_WEAPON_MACE2)
                expertisePoints = RACIAL_EXPERTISE_HUMAN_SWORD_MACE;
            break;
        case RACE_DWARF:  // Dwarf Mace Specialization + Gun Specialization
            if (subClass == ITEM_SUBCLASS_WEAPON_MACE || subClass == ITEM_SUBCLASS_WEAPON_MACE2)
                expertisePoints = RACIAL_EXPERTISE_DWARF_MACE;
            else if (subClass == ITEM_SUBCLASS_WEAPON_GUN)
                critPct = RACIAL_CRIT_PCT_DWARF_GUN;
            break;
        case RACE_TROLL:  // Troll Bow Specialization
            if (subClass == ITEM_SUBCLASS_WEAPON_BOW)
                critPct = RACIAL_CRIT_PCT_TROLL_BOW;
            break;
        default:
            return;
    }

    if (expertisePoints > 0.0f && (type_ & CollectorType::MELEE))
        collector_->stats[STATS_TYPE_EXPERTISE] += expertisePoints / player_->GetRatingMultiplier(CR_EXPERTISE);

    if (critPct > 0.0f && (type_ & CollectorType::RANGED))
        collector_->stats[STATS_TYPE_CRIT] += critPct / player_->GetRatingMultiplier(CR_CRIT_RANGED);
}
