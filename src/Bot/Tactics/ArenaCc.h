/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACC_H
#define _PLAYERBOT_ARENACC_H

#include <algorithm>
#include <array>
#include <string_view>

#include "ArenaCcConfig.h"
#include "BotAttributes.h"
#include "Common.h"
#include "SharedDefines.h"
#include "SplitMix64.h"
#include "TacticGate.h"

// Crowd-control policy (PLAN 3.15, G4a): which CC a bot has (CC_OPTIONS, one row per CC that has an action), how long
// the next one lasts (ExpectedMs), what the bot remembers of the CC it saw land on each enemy (DrMemory, addon-level:
// DT.12 forbids the core's hidden counters), which CC it casts (PickCc), when the next one of a chain starts
// (ChainDue) and when a bot breaks a CC on itself (ShouldBreakCc). Pure functions of plain values a human in the seat
// sees (auras with durations, cast bars, the combat log): no bots, no config singleton, no allocation. Their rolls
// hash the caller's seed (a guid) per window (RollWindow), so the same input gives the same answer. The core's data of
// each row (DR group, duration of the rank the bot knows, PvP cap, cast time, range) come from ArenaCcSpells.h. G4b's
// CcTacticStrategy (tactic cc, cc ready, team cc) calls it (G5a: pvp trinket); .playerbots tactics selftest cc too.

constexpr uint8 CC_DR_GROUP_COUNT = 21;   // the core's DiminishingGroup values, 0-20 (PvpTestCounters.h's names)
constexpr uint8 CC_DR_LEVEL_IMMUNE = 3;   // DIMINISHING_LEVEL_IMMUNE: after three CC of a group the target is immune
constexpr uint32 CC_DR_RESET_MS = 15000;  // the count resets this long after the group's last aura ended
constexpr uint8 CC_MEMORY_ENEMIES = 5;    // enemies one bot remembers (an arena team; in a battleground the latest)
constexpr uint8 CC_MEMORY_GROUPS = 8;     // DR groups remembered per enemy (G4d-c: 4 made room for a started count)
constexpr int8 CC_NO_ROW = -1;
constexpr uint8 CC_FULL_ABILITY = 100;        // skill 0 (the skill system off) passes every threshold
constexpr uint8 CC_LOW_EFFORT = 50;           // below it a bot stops controlling when behind
constexpr uint8 CC_TRAP_REACH_YARDS = 5;      // a trap goes down at the hunter's feet: the target must be near
constexpr uint8 CC_SCREAM_REACH_YARDS = 8;    // psychic scream's radius around the priest
constexpr uint8 CC_HOWL_REACH_YARDS = 10;     // G4d: howl of terror's radius around the warlock
constexpr uint8 CC_COLD_REACH_YARDS = 10;     // G4d: hungering cold's radius around the death knight
constexpr uint32 CC_MISS_WINDOW_MS = 1000;    // one memory answer per enemy, group and second
constexpr uint32 CC_BREAK_WINDOW_MS = 10000;  // one break, own-break or wrong-target answer per bot and window

// Creature types (CREATURE_TYPE_*) as CcOption::CreatureTypeMask bits, like the core's CREATURE_TYPEMASK_*
constexpr uint32 CcCreatureTypeBit(uint32 creatureType) { return creatureType ? 1u << (creatureType - 1) : 0u; }
constexpr uint32 CC_BEAST = CcCreatureTypeBit(CREATURE_TYPE_BEAST);
constexpr uint32 CC_BEAST_DRAGONKIN = CC_BEAST | CcCreatureTypeBit(CREATURE_TYPE_DRAGONKIN);
constexpr uint32 CC_DEMON_ELEMENTAL =
    CcCreatureTypeBit(CREATURE_TYPE_DEMON) | CcCreatureTypeBit(CREATURE_TYPE_ELEMENTAL);
constexpr uint32 CC_UNDEAD = CcCreatureTypeBit(CREATURE_TYPE_UNDEAD);
constexpr uint32 CC_UNDEAD_DEMON = CC_UNDEAD | CcCreatureTypeBit(CREATURE_TYPE_DEMON);

// What a CC does to its target: the loss-of-control kinds are worth a trinket, roots, snares and silences are not
enum class CcKind : uint8
{
    Incapacitate,  // polymorph, sap, gouge, repentance, freezing trap, shackle, hex, hungering cold
    Disorient,     // blind, scatter shot
    Fear,
    Horror,
    Stun,
    Sleep,
    Cyclone,
    Banish,
    Root,
    Snare,
    Silence
};

constexpr bool CcKindLosesControl(CcKind kind)
{
    switch (kind)
    {
        case CcKind::Incapacitate:
        case CcKind::Disorient:
        case CcKind::Fear:
        case CcKind::Horror:
        case CcKind::Stun:
        case CcKind::Sleep:
        case CcKind::Cyclone:
            return true;
        default:
            return false;
    }
}

// One CC a class has an action for. The static facts only; the core's numbers are the matching CcSpellData row.
struct CcOption
{
    char const* Action;  // the action's creator name (G4b casts through it)
    uint8 PlayerClass;   // CLASS_*
    uint32 SpellId;      // first rank; the bot casts the highest rank it knows
    CcKind Kind;
    uint8 ReachYards;      // 0: the spell's range from the core; else a placed or self-centred spell's reach
    bool BreaksOnDamage;   // damage ends it early (the own-break share applies)
    bool UsableOnPlayers;  // false: only creature types players lack (undead, beasts, demons, elementals, dragonkin)
    bool NeedsStealth;     // sap, cheap shot
    bool NeedsTargetOutOfCombat;  // sap
    uint32 CreatureTypeMask;      // the creature types it works on (CcCreatureTypeBit), 0 = any; a player is none
};

// The CC rows (G4d, DT.4: every class has one now, so a warrior, shaman or death knight team gets the moon mark too).
// The talent CC (shadowfury, psychic horror, scatter shot, wyvern sting, concussion blow, hungering cold) is a row of
// a bot only when it knows the spell (BuildCcBotTable); bash and maim only in their form (CcReadyRows).
constexpr std::array<CcOption, 30> CC_OPTIONS = {{
    {"polymorph", CLASS_MAGE, 118, CcKind::Incapacitate, 0, true, true, false, false, 0},
    {"cyclone on cc", CLASS_DRUID, 33786, CcKind::Cyclone, 0, false, true, false, false, 0},
    {"hibernate on cc", CLASS_DRUID, 2637, CcKind::Sleep, 0, true, false, false, false, CC_BEAST_DRAGONKIN},
    {"entangling roots on cc", CLASS_DRUID, 339, CcKind::Root, 0, true, true, false, false, 0},
    {"fear on cc", CLASS_WARLOCK, 5782, CcKind::Fear, 0, true, true, false, false, 0},
    {"banish on cc", CLASS_WARLOCK, 710, CcKind::Banish, 0, false, false, false, false, CC_DEMON_ELEMENTAL},
    {"freezing trap", CLASS_HUNTER, 1499, CcKind::Incapacitate, CC_TRAP_REACH_YARDS, true, true, false, false, 0},
    {"scare beast on cc", CLASS_HUNTER, 1513, CcKind::Fear, 0, true, false, false, false, CC_BEAST},
    {"sap", CLASS_ROGUE, 6770, CcKind::Incapacitate, 0, true, true, true, true, 0},
    {"blind", CLASS_ROGUE, 2094, CcKind::Disorient, 0, true, true, false, false, 0},
    {"gouge", CLASS_ROGUE, 1776, CcKind::Incapacitate, 0, true, true, false, false, 0},
    {"kidney shot", CLASS_ROGUE, 408, CcKind::Stun, 0, false, true, false, false, 0},
    {"cheap shot", CLASS_ROGUE, 1833, CcKind::Stun, 0, false, true, true, false, 0},
    {"repentance", CLASS_PALADIN, 20066, CcKind::Incapacitate, 0, true, true, false, false, 0},
    {"hammer of justice", CLASS_PALADIN, 853, CcKind::Stun, 0, false, true, false, false, 0},
    {"turn undead", CLASS_PALADIN, 10326, CcKind::Fear, 0, true, false, false, false, CC_UNDEAD_DEMON},
    {"psychic scream", CLASS_PRIEST, 8122, CcKind::Fear, CC_SCREAM_REACH_YARDS, true, true, false, false, 0},
    {"shackle undead", CLASS_PRIEST, 9484, CcKind::Incapacitate, 0, false, false, false, false, CC_UNDEAD},
    {"hex", CLASS_SHAMAN, 51514, CcKind::Incapacitate, 0, true, true, false, false, 0},
    {"howl of terror", CLASS_WARLOCK, 5484, CcKind::Fear, CC_HOWL_REACH_YARDS, true, true, false, false, 0},
    {"death coil", CLASS_WARLOCK, 6789, CcKind::Horror, 0, false, true, false, false, 0},
    {"shadowfury", CLASS_WARLOCK, 30283, CcKind::Stun, 0, false, true, false, false, 0},
    {"psychic horror", CLASS_PRIEST, 64044, CcKind::Horror, 0, false, true, false, false, 0},
    {"scatter shot", CLASS_HUNTER, 19503, CcKind::Disorient, 0, true, true, false, false, 0},
    {"wyvern sting", CLASS_HUNTER, 19386, CcKind::Sleep, 0, true, true, false, false, 0},
    {"intimidating shout", CLASS_WARRIOR, 5246, CcKind::Fear, 0, true, true, false, false, 0},
    {"concussion blow", CLASS_WARRIOR, 12809, CcKind::Stun, 0, false, true, false, false, 0},
    {"hungering cold", CLASS_DEATH_KNIGHT, 49203, CcKind::Incapacitate, CC_COLD_REACH_YARDS, true, true, false, false,
     0},
    {"bash", CLASS_DRUID, 5211, CcKind::Stun, 0, false, true, false, false, 0},
    {"maim", CLASS_DRUID, 22570, CcKind::Stun, 0, false, true, false, false, 0},
}};

// The rows as bits (the known, ready and failed masks): room for 64 rows (G4d widened it; G5c/G4e add rows freely)
using CcRowMask = uint64;

constexpr uint8 CC_OPTION_COUNT = static_cast<uint8>(CC_OPTIONS.size());
static_assert(CC_OPTION_COUNT <= 64, "the row masks are a uint64 (CcRowMask)");

constexpr CcRowMask CcOptionBit(uint8 index) { return CcRowMask(1) << index; }

// The row of an action, CC_NO_ROW when no row has it
constexpr int8 CcOptionIndex(std::string_view action)
{
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        if (action == CC_OPTIONS[index].Action)
            return static_cast<int8>(index);
    }

    return CC_NO_ROW;
}

// G4d-c: the self-centred area rows (every enemy within the reach around the caster); the freezing trap's 5 yd reach
// is a placed single-target CC and not one of them
constexpr CcRowMask CC_SELF_AOE_ROWS = CcOptionBit(static_cast<uint8>(CcOptionIndex("psychic scream"))) |
                                       CcOptionBit(static_cast<uint8>(CcOptionIndex("howl of terror"))) |
                                       CcOptionBit(static_cast<uint8>(CcOptionIndex("hungering cold")));
constexpr uint8 CC_SELF_AOE_COUNT_YARDS = 10;  // the enemies a self-centred row reaches: howl's and cold's radius

// The rows of a class (G4b's ready mask starts from it)
constexpr CcRowMask CcClassMask(uint8 playerClass)
{
    CcRowMask mask = 0;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        if (CC_OPTIONS[index].PlayerClass == playerClass)
            mask |= CcOptionBit(index);
    }

    return mask;
}

// A class with a CC it can use on players: the arena team plan's moon mark needs one (FocusStrategy's FocusCanControl)
constexpr bool CcClassHasOption(uint8 playerClass)
{
    for (CcOption const& option : CC_OPTIONS)
    {
        if (option.PlayerClass == playerClass && option.UsableOnPlayers)
            return true;
    }

    return false;
}

// The core's numbers of one row, filled once per spell (ArenaCcSpells.h; the selftest makes synthetic ones)
struct CcSpellData
{
    uint32 AuraSpellId = 0;      // the spell whose aura lands (a trap's: the trap object's spell)
    uint8 Group = 0;             // DiminishingGroup
    uint8 DrType = DRTYPE_NONE;  // DiminishingReturnsType
    uint32 BaseMs = 0;           // the aura's longest duration at the row's rank (a bot's table: its highest known)
    uint32 PvpCapMs = 0;         // GetDiminishingReturnsLimitDuration: a player target of a player caster (0 = none)
    uint32 CastMs = 0;
    float RangeYards = 0.0f;
    bool DamageInterrupts = false;  // AURA_INTERRUPT_FLAG_TAKE_DAMAGE on the aura (printed by the selftest)
    bool Valid = false;
};

using CcSpellTable = std::array<CcSpellData, CC_OPTION_COUNT>;

// Whether the group's count applies to this target (Unit::ApplyDiminishingToDuration): DRTYPE_ALL on everyone,
// DRTYPE_PLAYER on a target whose owner is a player (a player or a player's pet; a bot's CC has a player source)
inline bool CcDiminishes(uint8 drType, bool drAsPlayer)
{
    return drType == DRTYPE_ALL || (drType == DRTYPE_PLAYER && drAsPlayer);
}

// How long the next CC of the spell lasts at DR level 0-3 (the CC of its group since the last reset): the PvP cap on a
// target owned by a player (drAsPlayer: a player or a player's pet), then 1, 1/2, 1/4 or immune (0)
inline uint32 ExpectedMs(CcSpellData const& spell, uint8 level, bool drAsPlayer)
{
    uint32 duration = spell.BaseMs;
    if (drAsPlayer && spell.PvpCapMs > 0 && duration > spell.PvpCapMs)
        duration = spell.PvpCapMs;

    if (!CcDiminishes(spell.DrType, drAsPlayer))
        return duration;

    if (level >= CC_DR_LEVEL_IMMUNE)
        return 0;

    return duration >> level;
}

namespace CcSalt
{
constexpr uint64 DrMiss = 0x428A2F98D728AE22ULL;
constexpr uint64 WrongTarget = 0x7137449123EF65CDULL;
constexpr uint64 OwnBreak = 0xB5C0FBCFEC4D3B2FULL;
constexpr uint64 WastedBreak = 0xE9B5DBA58189DBBCULL;
constexpr uint64 ChainAfter = 0x3956C25BF348B538ULL;
}  // namespace CcSalt

// The shared roll: one answer per seed, salt and window of windowMs (each seed's windows start at its own phase), the
// same shape as TacticUsedInWindow; chancePercent 0 never, 100 always
inline bool RollWindow(uint64 seed, uint64 salt, uint32 chancePercent, uint32 windowMs, uint32 nowMs)
{
    if (chancePercent >= 100)
        return true;

    if (chancePercent == 0)
        return false;

    uint64 const mixed = SplitMix64(seed ^ salt);
    uint64 const window = TacticUseWindow(mixed, windowMs, nowMs);
    return SplitMix64(mixed ^ window) % 100 < chancePercent;
}

// What one bot remembers of the CC it saw land on its enemies, like a 3.3.5 DR addon: per enemy (guid) and DR group the
// CC seen since the last reset and when the group's last aura ends. A fixed array per bot; the oldest enemy makes room.
class DrMemory
{
public:
    // The level the next CC of the group lands at on the enemy, as the bot believes (0 = full, 3 = immune)
    uint8 Level(uint64 enemy, uint8 group, uint32 nowMs) const
    {
        for (Enemy const& slot : Enemies)
        {
            if (slot.Guid != enemy || !enemy)
                continue;

            for (Entry const& entry : slot.Groups)
            {
                if (entry.Group == group && group)
                    return EntryLevel(entry, nowMs);
            }
        }

        return 0;
    }

    // PickCc's input: the levels of every group on the enemy
    std::array<uint8, CC_DR_GROUP_COUNT> Levels(uint64 enemy, uint32 nowMs) const
    {
        std::array<uint8, CC_DR_GROUP_COUNT> levels{};
        for (Enemy const& slot : Enemies)
        {
            if (slot.Guid != enemy || !enemy)
                continue;

            for (Entry const& entry : slot.Groups)
            {
                if (entry.Group && entry.Group < CC_DR_GROUP_COUNT)
                    levels[entry.Group] = EntryLevel(entry, nowMs);
            }
        }

        return levels;
    }

    // A CC of the group seen landing for durationMs (its visible aura). With missPct the bot misses one that lands on
    // a count already started (near the threshold): it sees the aura but does not count it (false).
    bool Observe(uint64 enemy, uint8 group, uint32 nowMs, uint32 durationMs, uint8 missPct, uint64 seed)
    {
        if (!enemy || !group || group >= CC_DR_GROUP_COUNT)
            return false;

        Entry& entry = Find(enemy, group, nowMs);
        uint8 const level = EntryLevel(entry, nowMs);
        uint32 const endMs = nowMs + durationMs;
        entry.SeenMs = nowMs;
        if (level == 0 || static_cast<int32>(endMs - entry.EndMs) > 0)
            entry.EndMs = endMs;

        if (level > 0 && missPct > 0)
        {
            uint64 const key = SplitMix64(enemy ^ (static_cast<uint64>(group) << 56));
            if (RollWindow(seed ^ key, CcSalt::DrMiss, missPct, CC_MISS_WINDOW_MS, nowMs))
                return false;
        }

        entry.Count = static_cast<uint8>(std::min<uint32>(level + 1u, CC_DR_LEVEL_IMMUNE));
        return true;
    }

    // The group's aura seen ending early (damage, trinket, dispel): the reset counts from now, or from the end of
    // another aura of the group still up for otherLeftMs (two CC of one group overlapping: Observe kept the later end)
    void Ended(uint64 enemy, uint8 group, uint32 nowMs, uint32 otherLeftMs = 0)
    {
        uint32 const endMs = nowMs + otherLeftMs;
        for (Enemy& slot : Enemies)
        {
            if (slot.Guid != enemy || !enemy)
                continue;

            for (Entry& entry : slot.Groups)
            {
                if (entry.Group == group && group && static_cast<int32>(entry.EndMs - endMs) > 0)
                    entry.EndMs = endMs;
            }
        }
    }

    void Clear() { Enemies = {}; }

    // G4d-c (the CC immune line): the enemy's entry of the group; false when the memory holds none (never seen, or
    // its slot went to another group). The level, ms since the group's last CC was seen landing (a missed one too)
    // and ms until its aura ends (negative: ended that long ago)
    bool Recall(uint64 enemy, uint8 group, uint32 nowMs, uint8& level, uint32& sinceSeenMs, int32& endsInMs) const
    {
        for (Enemy const& slot : Enemies)
        {
            if (slot.Guid != enemy || !enemy)
                continue;

            for (Entry const& entry : slot.Groups)
            {
                if (entry.Group != group || !group)
                    continue;

                level = EntryLevel(entry, nowMs);
                sinceSeenMs = nowMs - entry.SeenMs;
                endsInMs = static_cast<int32>(entry.EndMs - nowMs);
                return true;
            }
        }

        return false;
    }

private:
    struct Entry
    {
        uint8 Group = 0;    // 0 = free
        uint8 Count = 0;    // CC of the group seen since the last reset, at most 3
        uint32 EndMs = 0;   // when the group's last seen aura ends or ended
        uint32 SeenMs = 0;  // G4d-c: when the group's last CC was seen landing
    };

    struct Enemy
    {
        uint64 Guid = 0;  // 0 = free
        uint32 SeenMs = 0;
        std::array<Entry, CC_MEMORY_GROUPS> Groups{};
    };

    // The core's rule (Unit::GetDiminishing): the count holds while an aura of the group is up and for 15 s after the
    // last one ended (strictly more than 15 s resets it)
    static uint8 EntryLevel(Entry const& entry, uint32 nowMs)
    {
        if (!entry.Count)
            return 0;

        int32 const sinceEnd = static_cast<int32>(nowMs - entry.EndMs);
        if (sinceEnd > static_cast<int32>(CC_DR_RESET_MS))
            return 0;

        return std::min<uint8>(entry.Count, CC_DR_LEVEL_IMMUNE);
    }

    // The enemy's entry of the group, made when missing: a free or the longest unseen enemy slot, a free, reset or the
    // earliest ending group slot
    Entry& Find(uint64 enemy, uint8 group, uint32 nowMs)
    {
        Enemy* target = nullptr;
        for (Enemy& slot : Enemies)
        {
            if (slot.Guid == enemy)
            {
                target = &slot;
                break;
            }
        }

        if (!target)
        {
            target = &Enemies[0];
            for (Enemy& slot : Enemies)
            {
                if (!slot.Guid)
                {
                    target = &slot;
                    break;
                }

                if (static_cast<uint32>(nowMs - slot.SeenMs) > static_cast<uint32>(nowMs - target->SeenMs))
                    target = &slot;
            }

            *target = Enemy{};
            target->Guid = enemy;
        }

        target->SeenMs = nowMs;
        Entry* spare = nullptr;
        for (Entry& entry : target->Groups)
        {
            if (entry.Group == group)
                return entry;

            if (!spare && (!entry.Group || !EntryLevel(entry, nowMs)))
                spare = &entry;
        }

        if (!spare)
        {
            spare = &target->Groups[0];
            for (Entry& entry : target->Groups)
            {
                if (static_cast<int32>(entry.EndMs - spare->EndMs) < 0)
                    spare = &entry;
            }
        }

        *spare = Entry{};
        spare->Group = group;
        return *spare;
    }

    std::array<Enemy, CC_MEMORY_ENEMIES> Enemies{};
};

// "Cautious bots break earlier" (G5a): the low-health line of the bot's own breaks, LowHealthPct moved by
// Avoid.CautiousLowHealthPct x (Caution - 50) / 50 (Caution 50 or a shift of 0: LowHealthPct)
inline uint8 CcBreakLowHealthPct(uint8 lowHealthPct, uint8 caution, uint32 cautiousShiftPct)
{
    int32 const shift = static_cast<int32>(std::min<uint32>(cautiousShiftPct, 100)) *
                        (static_cast<int32>(std::min<uint8>(caution, 100)) - 50) / 50;
    return static_cast<uint8>(std::clamp<int32>(static_cast<int32>(lowHealthPct) + shift, 0, 100));
}

// One bot's CC numbers, cached once per skill load (G4b). Knowledge gives DR awareness, chains and the memory's misses;
// Foresight the chain lead; Wisdom the target and break judgement; Focus the own breaks (none from
// Avoid.OwnBreakNeverFromSkill); Patience the hold; effort
// (the gate's, from Grit) the stop when behind. Skill 0 is full ability: every threshold passed, the skill-99 values.
struct CcParams
{
    bool DrAware = false;
    bool Chains = false;
    uint8 DrMissPct = 0;
    uint32 ChainLeadMs = 0;
    uint32 MinExpectedMs = 0;
    uint8 WrongTargetPct = 0;
    uint32 HoldMs = 0;
    bool StopsWhenBehind = false;
    uint8 BehindStopPct = 0;
    uint8 OwnBreakPct = 0;
    uint8 WastedBreakPct = 0;
    bool IgnoresRoots = false;
    uint32 MinLeftMs = 0;
    uint8 LowHealthPct = 0;
    uint8 BreakLowHealthPct = 0;  // G5a: LowHealthPct moved by the bot's Caution (CcBreakLowHealthPct)

    bool operator==(CcParams const&) const = default;
};

inline CcParams ComputeCcParams(uint8 skill, BotAttributeValues const& attributes, uint8 effort, float exponent,
                                CcConfig const& config)
{
    bool const full = skill == 0;
    auto const ability = [&](BotAttribute attribute) { return full ? CC_FULL_ABILITY : attributes.Get(attribute); };
    auto const lerp = [&](uint32 atSkill1, uint32 atSkill99, BotAttribute attribute)
    {
        float const position = full ? 1.0f : SkillPosition(attributes.Get(attribute));
        return SkillLerpRounded(atSkill1, atSkill99, position, exponent);
    };
    auto const percent = [](uint32 value) { return static_cast<uint8>(std::min<uint32>(value, 100)); };
    uint8 const knowledge = ability(BotAttribute::Knowledge);
    float const patience =
        full ? 0.0f
             : (static_cast<float>(std::min<uint8>(attributes.Get(BotAttribute::Patience), 100)) - 50.0f) / 50.0f;

    CcParams params;
    params.DrAware = knowledge >= config.CcDrAwareFromSkill;
    params.Chains = params.DrAware && knowledge >= config.CcChainsFromSkill;
    params.DrMissPct = percent(lerp(config.CcDrMissPctAtSkill1, config.CcDrMissPctAtSkill99, BotAttribute::Knowledge));
    params.ChainLeadMs = lerp(config.CcChainLeadMsAtSkill1, config.CcChainLeadMsAtSkill99, BotAttribute::Foresight);
    params.MinExpectedMs = config.CcMinExpectedMs;
    params.WrongTargetPct =
        percent(lerp(config.CcWrongTargetPctAtSkill1, config.CcWrongTargetPctAtSkill99, BotAttribute::Wisdom));
    params.HoldMs = static_cast<uint32>(static_cast<float>(config.CcPatientHoldMs) * std::max(0.0f, patience) + 0.5f);
    params.StopsWhenBehind = !full && effort < CC_LOW_EFFORT;
    params.BehindStopPct = percent(config.CcBehindStopPct);
    params.OwnBreakPct =
        ability(BotAttribute::Focus) >= config.AvoidOwnBreakNeverFromSkill
            ? 0
            : percent(lerp(config.AvoidOwnBreakPctAtSkill1, config.AvoidOwnBreakPctAtSkill99, BotAttribute::Focus));
    params.WastedBreakPct = ability(BotAttribute::Wisdom) >= config.AvoidWastedBreakNeverFromSkill
                                ? 0
                                : percent(lerp(config.AvoidWastedBreakPctAtSkill1, config.AvoidWastedBreakPctAtSkill99,
                                               BotAttribute::Wisdom));
    params.IgnoresRoots = ability(BotAttribute::Wisdom) >= config.AvoidIgnoreRootsFromSkill;
    params.MinLeftMs = config.AvoidMinLeftMs;
    params.LowHealthPct = percent(config.AvoidLowHealthPct);
    params.BreakLowHealthPct =
        CcBreakLowHealthPct(params.LowHealthPct, full ? SKILL_STYLE_NEUTRAL : attributes.Get(BotAttribute::Caution),
                            config.AvoidCautiousLowHealthPct);
    return params;
}

// What a bot sees when it chooses its CC: the rows it can cast now (G4b: known, off cooldown, combo points), the
// target, the CC aura on it now (visible, by spell id) and its own memory of the target (DrMemory::Levels)
struct CcPickInput
{
    CcRowMask ReadyMask = 0;     // CcOptionBit per row
    bool TargetIsPlayer = true;  // a player: only the rows UsableOnPlayers
    bool DrAsPlayer = true;      // the target or its owner is a player (a pet): the PvP cap and player-only DR apply
    uint8 CreatureType = 0;      // CREATURE_TYPE_* of a creature target (a row's CreatureTypeMask)
    float DistanceYards = 0.0f;  // as the spell's range check measures it
    bool Stealthed = false;
    bool TargetInCombat = true;
    uint8 ActiveGroup = 0;     // DR group of the CC aura on the target now (the one that ends last), 0 = none
    bool Rooted = false;       // the target is rooted now (visible): no root on top of it
    bool ControlOnly = false;  // the arena moon or a chain: only a CC that takes control away (not a root)
    bool TeamDotOn = false;    // G4c-b DotGuard: the team's periodic damage hits the target (no CC damage breaks)
    std::array<uint8, CC_DR_GROUP_COUNT> Levels{};
    // G4d-c: the self-centred rows (CC_SELF_AOE_ROWS) instant now (Improved Howl of Terror), and the enemies (players
    // and their pets, the target too) within CC_SELF_AOE_COUNT_YARDS of the bot; filled only when such a row is ready
    CcRowMask InstantMask = 0;
    uint8 AoeEnemies = 0;
};

// A player takes the rows usable on players; a creature (a pet too) the rows whose creature types include its own
inline bool CcOptionUsableOn(CcOption const& option, bool targetIsPlayer, uint8 creatureType)
{
    if (targetIsPlayer)
        return option.UsableOnPlayers;

    return !option.CreatureTypeMask || (option.CreatureTypeMask & CcCreatureTypeBit(creatureType)) != 0;
}

inline bool CcOptionReaches(CcOption const& option, CcSpellData const& spell, float distanceYards)
{
    float const reach = option.ReachYards ? static_cast<float>(option.ReachYards) : spell.RangeYards;
    return distanceYards <= reach;
}

// A finisher's duration by the combo points on its target: WorldObject::CalcSpellDuration's formula, which counts the
// points on the caster's combo target (the kill target, as a rule; G4b-a review NOTE 2)
inline uint32 CcComboDurationMs(int32 minMs, int32 maxMs, uint8 points)
{
    int32 const durationMs = points && minMs != -1 && minMs != maxMs ? minMs + (maxMs - minMs) * points / 5 : minMs;
    return static_cast<uint32>(std::max<int32>(durationMs, 0));
}

// The CC to cast, CC_NO_ROW for none: the usable row that lasts longest, a loss of control before a root whatever their
// lengths (a root on a player lasts 10 s, cyclone 6 s; G4b-a review B1); no root on a rooted target, none on the moon,
// none that damage breaks on a target the team's periodic damage hits (TeamDotOn: the DoT would break it). A DR-aware
// bot also skips what its memory says lands immune or shorter than MinExpectedMs and the group of the CC already on
// the target (a chain alternates groups); a bot without DR awareness casts into DR, believing every CC lands in full.
// G4d-c: within one DR group a usable self-centred row that is instant now or reaches a second enemy goes before the
// group's other rows (howl of terror before fear: the same DR, no cast or more enemies; fear lasts longer and won
// every pick before, so no warlock ever howled)
inline int8 PickCc(CcPickInput const& input, CcSpellTable const& spells, CcParams const& params)
{
    // Pass 1: the usable rows, their expected durations and the groups a preferred self-centred row holds
    std::array<uint32, CC_OPTION_COUNT> expected{};
    CcRowMask usable = 0;
    CcRowMask preferred = 0;
    uint32 preferredGroups = 0;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        CcRowMask const bit = CcOptionBit(index);
        if (!(input.ReadyMask & bit))
            continue;

        CcOption const& option = CC_OPTIONS[index];
        CcSpellData const& spell = spells[index];
        if (!spell.Valid || spell.Group >= CC_DR_GROUP_COUNT ||
            !CcOptionUsableOn(option, input.TargetIsPlayer, input.CreatureType) ||
            (option.NeedsStealth && !input.Stealthed) || (option.NeedsTargetOutOfCombat && input.TargetInCombat) ||
            !CcOptionReaches(option, spell, input.DistanceYards))
            continue;

        bool const control = CcKindLosesControl(option.Kind);
        if ((input.Rooted && option.Kind == CcKind::Root) || (input.ControlOnly && !control) ||
            (input.TeamDotOn && (option.BreaksOnDamage || spell.DamageInterrupts)))
            continue;

        uint8 level = 0;
        if (params.DrAware)
        {
            if (input.ActiveGroup && spell.Group == input.ActiveGroup)
                continue;

            level = input.Levels[spell.Group];
        }

        uint32 const expectedMs = ExpectedMs(spell, level, input.DrAsPlayer);
        if (params.DrAware && (!expectedMs || expectedMs < params.MinExpectedMs))
            continue;

        usable |= bit;
        expected[index] = expectedMs;
        if ((CC_SELF_AOE_ROWS & bit) && ((input.InstantMask & bit) || input.AoeEnemies >= 2))
        {
            preferred |= bit;
            preferredGroups |= 1u << spell.Group;
        }
    }

    // Pass 2: the pick among them
    int8 best = CC_NO_ROW;
    uint32 bestMs = 0;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        CcRowMask const bit = CcOptionBit(index);
        if (!(usable & bit) || ((preferredGroups & (1u << spells[index].Group)) && !(preferred & bit)))
            continue;

        bool const control = CcKindLosesControl(CC_OPTIONS[index].Kind);
        uint32 const expectedMs = expected[index];
        bool const bestControl = best != CC_NO_ROW && CcKindLosesControl(CC_OPTIONS[best].Kind);
        if (best == CC_NO_ROW || (control && !bestControl) || (control == bestControl && expectedMs > bestMs))
        {
            best = static_cast<int8>(index);
            bestMs = expectedMs;
        }
    }

    return best;
}

// What the bot sees of the CC on its CC target (DT.1: the visible aura and the partner's visible cast bar, nothing
// shared in private)
struct CcChainView
{
    bool AuraOnTarget = false;        // a CC aura of the team on it
    uint32 RemainingMs = 0;           // the aura's remaining duration
    bool PartnerCastPending = false;  // the partner's cast bar on the target shows a CC
    // G4c-d: the team's cyclone keeps the target immune until its end: the chain lands AfterMs after the end instead
    // of the lead before it (a CC landing inside it is lost)
    bool ImmuneUntilEnd = false;
    uint32 AfterMs = 0;
    uint32 ReactMs = 0;  // G4c-d: the bot's own delay from the chain's moment to its cast: it starts that much sooner
};

// The remaining time of the team's CC at which a chaining bot starts: its CC (castMs) lands leadMs before the end, or
// AfterMs after it on an aura that keeps the target immune; ReactMs sooner (G4c-d)
inline uint32 CcChainStartAtMs(CcChainView const& view, uint32 castMs, uint32 leadMs)
{
    uint32 const before = castMs + view.ReactMs + (view.ImmuneUntilEnd ? 0 : leadMs);
    uint32 const after = view.ImmuneUntilEnd ? view.AfterMs : 0;
    return before > after ? before - after : 0;
}

// Whether the bot starts its CC now: never over the partner's pending cast; at once when no CC is on the target; a
// chaining bot starts at CcChainStartAtMs before the aura's end, so its CC lands leadMs before the current one ends
inline bool ChainDue(CcChainView const& view, uint32 castMs, uint32 leadMs, bool chains)
{
    if (view.PartnerCastPending)
        return false;

    if (!view.AuraOnTarget)
        return true;

    return chains && view.RemainingMs <= CcChainStartAtMs(view, castMs, leadMs);
}

// How long a chaining bot that is not due yet waits (ChainDue false with the aura on and no pending cast), 0 when
// it is due or waits on something else: cc ready wakes then instead of a whole refresh later (G4c)
inline uint32 CcChainWaitMs(CcChainView const& view, uint32 castMs, uint32 leadMs)
{
    uint32 const startAtMs = CcChainStartAtMs(view, castMs, leadMs);
    if (view.PartnerCastPending || !view.AuraOnTarget || view.RemainingMs <= startAtMs)
        return 0;

    return view.RemainingMs - startAtMs;
}

// G4c-d: a cast started now that lands before immuneEndMs (the team's cyclone's end plus AfterMs; 0 = none) is lost:
// the bot holds it (a short cast or an instant whose reaction passed early)
inline bool CcLandsInImmunity(uint32 nowMs, uint32 castMs, uint32 immuneEndMs)
{
    return immuneEndMs && static_cast<int32>(nowMs + castMs - immuneEndMs) < 0;
}

// G4c-d busy guard: a cast, channel or global cooldown of busyMs started now ends after the chain's moment dueAtMs
// (a moment passed counts as now)
inline bool CcChainBlocksCast(uint32 nowMs, uint32 dueAtMs, uint32 busyMs)
{
    int32 const leftMs = static_cast<int32>(dueAtMs - nowMs);
    return busyMs > static_cast<uint32>(std::max<int32>(leftMs, 0));
}

// G4c-d: the bot's own delay from a chain's moment to its cast (its reaction and cadence), learned from its chains:
// the first sample wholly, then a quarter of each
inline uint32 CcLearnChainReactMs(uint32 currentMs, uint32 sampleMs, bool learned)
{
    return learned ? (currentMs * 3 + sampleMs + 2) / 4 : sampleMs;
}

// G4c-b: the chain's end as a chaining bot expects it. The aura's remaining time; with the opponent memory sooner when
// the team's CC on this enemy was seen dispelled dispelMeanMs after landing on average (0 = not known): the next CC
// then lands marginMs before that moment. elapsedMs = how long the aura is up. 0 = due now. Once the remembered
// dispel moment has passed by marginMs without a dispel (G4c-c, G4c-b review NOTE 1; G4c-d: mean + margin, so the
// due window from mean - margin is wide enough for the bot's reaction), the aura's end counts again, so an absent
// dispel does not make the chain land on top of a CC with seconds left.
inline uint32 CcChainRemainingMs(uint32 remainingMs, uint32 elapsedMs, uint32 dispelMeanMs, uint32 marginMs)
{
    if (!dispelMeanMs || elapsedMs > dispelMeanMs + marginMs)
        return remainingMs;

    uint32 const dispelAtMs = dispelMeanMs > marginMs ? dispelMeanMs - marginMs : 0;
    uint32 const dispelLeftMs = dispelAtMs > elapsedMs ? dispelAtMs - elapsedMs : 0;
    return std::min(remainingMs, dispelLeftMs);
}

// The DR groups whose CC takes control away (the chains' groups, CcKindLosesControl's kinds): every CC group but
// roots, disarm, silence, banish (the target is immune) and the rest (DiminishingGroup values)
constexpr bool CcGroupLosesControl(uint8 group)
{
    switch (group)
    {
        case DIMINISHING_OPENING_STUN:
        case DIMINISHING_CONTROLLED_STUN:
        case DIMINISHING_CYCLONE:
        case DIMINISHING_DISORIENT:
        case DIMINISHING_FEAR:
        case DIMINISHING_HORROR:
        case DIMINISHING_MIND_CONTROL:
        case DIMINISHING_STUN:
        case DIMINISHING_SCATTER_SHOT:
        case DIMINISHING_SLEEP:
        case DIMINISHING_DRAGONS_BREATH:
            return true;
        default:
            return false;
    }
}

// When the team's loss-of-control CC on each enemy ends, as one bot saw it land and leave (G4c: the chain gap
// counter tactics.chainGapMs). The latest end of the team's CC on the enemy; a fixed array, the oldest enemy makes
// room.
class CcChainClock
{
public:
    // A team CC landing now follows the previous one (signedGapMs = now - its end: negative on top of it, positive
    // after it) when that one is still up or ended at most windowMs ago
    bool Gap(uint64 enemy, uint32 nowMs, uint32 windowMs, int32& signedGapMs, uint8& previousGroup) const
    {
        for (Slot const& slot : Slots)
        {
            if (slot.Guid != enemy || !enemy)
                continue;

            signedGapMs = static_cast<int32>(nowMs - slot.EndMs);
            previousGroup = slot.Group;
            return signedGapMs <= static_cast<int32>(windowMs);
        }

        return false;
    }

    // A team CC of the group seen landing for durationMs: the enemy stays controlled until the later end
    void Land(uint64 enemy, uint8 group, uint32 nowMs, uint32 durationMs)
    {
        if (!enemy)
            return;

        Slot& slot = Find(enemy, nowMs);
        uint32 const endMs = nowMs + durationMs;
        if (static_cast<int32>(slot.EndMs - nowMs) <= 0 || static_cast<int32>(endMs - slot.EndMs) > 0)
        {
            slot.EndMs = endMs;
            slot.Group = group;
        }
    }

    // A team CC seen ending early (damage, dispel, trinket): the control ends now, or with the team's other CC still
    // up for otherLeftMs
    void Ended(uint64 enemy, uint32 nowMs, uint32 otherLeftMs)
    {
        uint32 const endMs = nowMs + otherLeftMs;
        for (Slot& slot : Slots)
        {
            if (slot.Guid == enemy && enemy && static_cast<int32>(slot.EndMs - endMs) > 0)
                slot.EndMs = endMs;
        }
    }

    void Clear() { Slots = {}; }

private:
    struct Slot
    {
        uint64 Guid = 0;  // 0 = free
        uint32 SeenMs = 0;
        uint32 EndMs = 0;
        uint8 Group = 0;  // the DR group of the CC that ends last
    };

    Slot& Find(uint64 enemy, uint32 nowMs)
    {
        Slot* target = &Slots[0];
        for (Slot& slot : Slots)
        {
            if (slot.Guid == enemy)
            {
                target = &slot;
                break;
            }

            if (!slot.Guid || (target->Guid &&
                               static_cast<uint32>(nowMs - slot.SeenMs) > static_cast<uint32>(nowMs - target->SeenMs)))
                target = &slot;
        }

        if (target->Guid != enemy)
        {
            *target = Slot{};
            target->Guid = enemy;
        }

        target->SeenMs = nowMs;
        return *target;
    }

    std::array<Slot, CC_MEMORY_ENEMIES> Slots{};
};

constexpr uint16 CC_OPPONENT_MAX_EVENTS = 1000;  // events counted per enemy and match (the mean's sum stays in range)
constexpr uint32 CC_OPPONENT_MAX_MS = 60000;     // a dispel delay is counted up to this
constexpr uint32 CC_OPPONENT_TRINKET_SAME_MS = 1000;  // G5a: a trinket seen by its aura and by the CC it removed is one
constexpr uint32 CC_OPPONENT_TRINKET_COOLDOWN_MS = 120000;  // G5a: the PvP trinket's (and the racials') cooldown

// G4c-b opponent memory (PLAN 3.15 G0h3's items owned by G4c; DT.12): what one bot saw of each enemy's answers to its
// team's loss-of-control CC this match, from how the aura left (the aura hooks): dispelled (by its healer; the time
// from landing) or trinketed (a self-cast mechanic immunity removed it). Per enemy guid, a fixed array (the oldest
// enemy makes room); everything is forgotten when the match key (the map's instance id) changes.
class CcOpponentMemory
{
public:
    struct Slot
    {
        uint64 Guid = 0;  // 0 = free
        uint32 SeenMs = 0;
        uint16 Dispels = 0;
        uint32 DispelSumMs = 0;  // the dispels' delays from landing
        uint16 Trinkets = 0;
        uint32 LastTrinketMs = 0;
    };

    void Dispelled(uint32 matchKey, uint64 enemy, uint32 afterMs, uint32 nowMs)
    {
        Slot* const slot = Touch(matchKey, enemy, nowMs);
        if (!slot || slot->Dispels >= CC_OPPONENT_MAX_EVENTS)
            return;

        ++slot->Dispels;
        slot->DispelSumMs += std::min(afterMs, CC_OPPONENT_MAX_MS);
    }

    // A trinket use seen (G5a: its immunity aura landing, or the team's CC it removed); false when it is one already
    // known (both seen within CC_OPPONENT_TRINKET_SAME_MS) or the slot is full
    bool Trinketed(uint32 matchKey, uint64 enemy, uint32 nowMs)
    {
        Slot* const slot = Touch(matchKey, enemy, nowMs);
        if (!slot || slot->Trinkets >= CC_OPPONENT_MAX_EVENTS ||
            (slot->Trinkets && nowMs - slot->LastTrinketMs < CC_OPPONENT_TRINKET_SAME_MS))
            return false;

        ++slot->Trinkets;
        slot->LastTrinketMs = nowMs;
        return true;
    }

    // G5a: when the enemy's trinket is expected ready again (its last use seen this match plus the cooldown); 0 = no
    // use seen
    uint32 TrinketReadyMs(uint32 matchKey, uint64 enemy) const
    {
        Slot const* const slot = Find(matchKey, enemy);
        return slot && slot->Trinkets ? slot->LastTrinketMs + CC_OPPONENT_TRINKET_COOLDOWN_MS : 0;
    }

    // The mean delay of the enemy's dispels of the team's CC this match, 0 below minEvents of them (at least one)
    uint32 DispelMeanMs(uint32 matchKey, uint64 enemy, uint32 minEvents) const
    {
        Slot const* const slot = Find(matchKey, enemy);
        if (!slot || !slot->Dispels || slot->Dispels < minEvents)
            return 0;

        return slot->DispelSumMs / slot->Dispels;
    }

    Slot const* Find(uint32 matchKey, uint64 enemy) const
    {
        if (matchKey != MatchKey || !enemy)
            return nullptr;

        for (Slot const& slot : Slots)
        {
            if (slot.Guid == enemy)
                return &slot;
        }

        return nullptr;
    }

    void Clear()
    {
        Slots = {};
        MatchKey = 0;
    }

private:
    Slot* Touch(uint32 matchKey, uint64 enemy, uint32 nowMs)
    {
        if (!enemy)
            return nullptr;

        if (matchKey != MatchKey)
            Clear();

        MatchKey = matchKey;
        Slot* target = &Slots[0];
        for (Slot& slot : Slots)
        {
            if (slot.Guid == enemy)
            {
                target = &slot;
                break;
            }

            if (!slot.Guid || (target->Guid &&
                               static_cast<uint32>(nowMs - slot.SeenMs) > static_cast<uint32>(nowMs - target->SeenMs)))
                target = &slot;
        }

        if (target->Guid != enemy)
        {
            *target = Slot{};
            target->Guid = enemy;
        }

        target->SeenMs = nowMs;
        return target;
    }

    std::array<Slot, CC_MEMORY_ENEMIES> Slots{};
    uint32 MatchKey = 0;
};

// G4c-b AoE guard: where a damage spell's area is centred (ArenaCcSpells.h CcResolveSpellArea reads it from the spell
// store) and how far it reaches
enum class CcAreaCentre : uint8
{
    None,    // one target
    Caster,  // around the caster (arcane explosion, frost nova, consecration, whirlwind)
    Target,  // at the target or where it stands (blizzard, flamestrike, hurricane, mind sear)
    Cone,    // in front of the caster (cone of cold, dragon's breath)
    Chain    // jumps from the target (cleave, multi-shot, chain lightning)
};

struct CcSpellArea
{
    CcAreaCentre Centre = CcAreaCentre::None;
    float RadiusYards = 0.0f;
};

// Whether a held enemy is in the area: its distance from the caster and from the spell's target, and whether it is in
// front of the caster (a cone); marginYards for its movement and size
inline bool CcAreaCovers(CcSpellArea const& area, float marginYards, float fromCasterYards, float fromTargetYards,
                         bool inFront)
{
    float const reach = area.RadiusYards + marginYards;
    switch (area.Centre)
    {
        case CcAreaCentre::Caster:
            return fromCasterYards <= reach;
        case CcAreaCentre::Cone:
            return inFront && fromCasterYards <= reach;
        case CcAreaCentre::Target:
        case CcAreaCentre::Chain:
            return fromTargetYards <= reach;
        default:
            return false;
    }
}

// The rolls of the shares, one answer per bot and CC_BREAK_WINDOW_MS: the CC goes on whoever is near instead of the CC
// target; the bot's damage breaks its team's CC
inline bool CcWrongTarget(CcParams const& params, uint64 seed, uint32 nowMs)
{
    return RollWindow(seed, CcSalt::WrongTarget, params.WrongTargetPct, CC_BREAK_WINDOW_MS, nowMs);
}

inline bool CcBreaksOwn(CcParams const& params, uint64 seed, uint32 nowMs)
{
    return RollWindow(seed, CcSalt::OwnBreak, params.OwnBreakPct, CC_BREAK_WINDOW_MS, nowMs);
}

// A low-effort bot stops controlling when its team is BehindStopPct health points behind (team health percents)
inline bool CcStopsBehind(CcParams const& params, uint8 ownHealthPct, uint8 enemyHealthPct)
{
    return params.StopsWhenBehind && enemyHealthPct >= ownHealthPct + params.BehindStopPct;
}

// A patient bot holds a ready CC for the kill window (the kill target low or a burst starting) up to HoldMs
inline bool CcHolds(CcParams const& params, uint32 heldMs, bool killWindowOpen)
{
    return !killWindowOpen && heldMs < params.HoldMs;
}

// A CC on the bot (what its own UI shows) and its team's state: SelfLow / PartnerLow at or below LowHealthPct, Burst
// when the bot or its partner takes a burst
struct CcBreakView
{
    CcKind Kind = CcKind::Stun;
    uint32 RemainingMs = 0;
    bool SelfLow = false;
    bool PartnerLow = false;
    bool Burst = false;
};

// The CC matters: it takes control, has MinLeftMs or more left, and the bot or its partner is low or being burst
inline bool CcMatters(CcBreakView const& view, CcParams const& params)
{
    return CcKindLosesControl(view.Kind) && view.RemainingMs >= params.MinLeftMs &&
           (view.SelfLow || view.PartnerLow || view.Burst);
}

// Whether the bot breaks the CC on it (G5a: pvp trinket, racials): always when it matters; otherwise a wasted break
// with WastedBreakPct per window, never of a root, snare or silence for a bot that IgnoresRoots
inline bool ShouldBreakCc(CcBreakView const& view, CcParams const& params, uint64 seed, uint32 nowMs)
{
    if (CcMatters(view, params))
        return true;

    if (params.IgnoresRoots && !CcKindLosesControl(view.Kind))
        return false;

    return RollWindow(seed, CcSalt::WastedBreak, params.WastedBreakPct, CC_BREAK_WINDOW_MS, nowMs);
}

// G5a: below the Trinket tactic's known skill a bot does not tell a CC that matters: it breaks any CC on the
// wasted-break roll (it trinkets the first CC, or forgets it), the roots aside from IgnoreRootsFromSkill
inline bool ShouldBreakCcUnknowing(CcBreakView const& view, CcParams const& params, uint64 seed, uint32 nowMs)
{
    if (params.IgnoresRoots && !CcKindLosesControl(view.Kind))
        return false;

    return RollWindow(seed, CcSalt::WastedBreak, params.WastedBreakPct, CC_BREAK_WINDOW_MS, nowMs);
}

// G5a: the aura types a CC aura is read by (CcKindOfAura's auraBits), set by the caller from the aura's effects
constexpr uint32 CC_AURA_STUN = 1u << 0;
constexpr uint32 CC_AURA_FEAR = 1u << 1;
constexpr uint32 CC_AURA_CONFUSE = 1u << 2;
constexpr uint32 CC_AURA_CHARM = 1u << 3;
constexpr uint32 CC_AURA_ROOT = 1u << 4;
constexpr uint32 CC_AURA_SNARE = 1u << 5;
constexpr uint32 CC_AURA_SILENCE = 1u << 6;
constexpr uint32 CC_AURA_CYCLONE = 1u << 7;  // the cyclone itself (it has no mechanic)

constexpr uint64 CcMechanicBit(uint32 mechanic) { return 1ULL << mechanic; }

// What a CC aura on a unit is (G5a: the bot's own CC it breaks, a partner's it dispels), from its mechanics (the
// spell's and its effects', SpellInfo::GetAllEffectsMechanicMask) and aura types; false: no CC
inline bool CcKindOfAura(uint64 mechanics, uint32 auraBits, CcKind& kind)
{
    auto const has = [mechanics](uint32 mechanic) { return (mechanics & CcMechanicBit(mechanic)) != 0; };
    if (auraBits & CC_AURA_CYCLONE)
        kind = CcKind::Cyclone;
    else if (has(MECHANIC_BANISH))
        kind = CcKind::Banish;
    else if (has(MECHANIC_SLEEP))
        kind = CcKind::Sleep;
    else if (has(MECHANIC_HORROR))
        kind = CcKind::Horror;
    else if (has(MECHANIC_POLYMORPH) || has(MECHANIC_SAPPED) || has(MECHANIC_SHACKLE) || has(MECHANIC_KNOCKOUT) ||
             (has(MECHANIC_FREEZE) && (auraBits & CC_AURA_STUN)))
        kind = CcKind::Incapacitate;
    else if (has(MECHANIC_DISORIENTED) || (auraBits & CC_AURA_CONFUSE))
        kind = CcKind::Disorient;
    else if (has(MECHANIC_FEAR) || has(MECHANIC_CHARM) || has(MECHANIC_TURN) ||
             (auraBits & (CC_AURA_FEAR | CC_AURA_CHARM)))
        kind = CcKind::Fear;
    else if (has(MECHANIC_STUN) || (auraBits & CC_AURA_STUN))
        kind = CcKind::Stun;
    else if (has(MECHANIC_ROOT) || has(MECHANIC_FREEZE) || (auraBits & CC_AURA_ROOT))
        kind = CcKind::Root;
    else if (has(MECHANIC_SNARE) || has(MECHANIC_DAZE) || (auraBits & CC_AURA_SNARE))
        kind = CcKind::Snare;
    else if (has(MECHANIC_SILENCE) || (auraBits & CC_AURA_SILENCE))
        kind = CcKind::Silence;
    else
        return false;

    return true;
}

// G5a: what breaks a CC on the bot. The PvP trinket (42292) and every man for himself (59752) free the core's
// IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK (SpellInfo.cpp, their mechanic immunity), will of the forsaken
// (7744) charm, fear and sleep (its three effects), escape artist (20589, a script) roots and snares. A CC without
// such a mechanic is not broken (cyclone carries MECHANIC_BANISH in this core's Spell.dbc: the trinket frees it).
enum class CcBreaker : uint8
{
    None,
    Trinket,
    EveryMan,
    Forsaken,
    EscapeArtist
};

constexpr uint64 CcBreakerMechanics(CcBreaker breaker)
{
    switch (breaker)
    {
        case CcBreaker::Trinket:
        case CcBreaker::EveryMan:
            return IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK;
        case CcBreaker::Forsaken:
            return CcMechanicBit(MECHANIC_CHARM) | CcMechanicBit(MECHANIC_FEAR) | CcMechanicBit(MECHANIC_SLEEP);
        case CcBreaker::EscapeArtist:
            return CcMechanicBit(MECHANIC_ROOT) | CcMechanicBit(MECHANIC_SNARE);
        default:
            return 0;
    }
}

// The action names (the trinket's is G5a's "pvp trinket", the racials' those of RacialsStrategy)
constexpr char const* CcBreakerAction(CcBreaker breaker)
{
    switch (breaker)
    {
        case CcBreaker::Trinket:
            return "pvp trinket";
        case CcBreaker::EveryMan:
            return "every man for himself";
        case CcBreaker::Forsaken:
            return "will of the forsaken";
        case CcBreaker::EscapeArtist:
            return "escape artist";
        default:
            return "none";
    }
}

// The breakers the bot has ready now (worn or known, off cooldown; the trinket not withheld by Avoid.EnsureTrinket 2)
struct CcBreakOptions
{
    bool Trinket = false;
    bool EveryMan = false;
    bool Forsaken = false;
    bool EscapeArtist = false;
};

// The breaker that frees the CC's mechanics: the trinket first (it ranks above the racials, DT.3), then the racials
inline CcBreaker PickCcBreaker(uint64 mechanics, CcBreakOptions const& ready)
{
    std::array<std::pair<CcBreaker, bool>, 4> const order = {{{CcBreaker::Trinket, ready.Trinket},
                                                              {CcBreaker::EveryMan, ready.EveryMan},
                                                              {CcBreaker::Forsaken, ready.Forsaken},
                                                              {CcBreaker::EscapeArtist, ready.EscapeArtist}}};
    for (auto const& [breaker, isReady] : order)
    {
        if (isReady && (mechanics & CcBreakerMechanics(breaker)))
            return breaker;
    }

    return CcBreaker::None;
}

// G5a dispel cc on party: a CC worth a cleanser's global cooldown is a loss of control or a root (snares and
// silences stay with the class's plain cleanses)
constexpr bool CcDispelWorth(CcKind kind) { return CcKindLosesControl(kind) || kind == CcKind::Root; }

// One party member's worst CC the bot's class removes (Has false: none)
struct CcDispelCandidate
{
    bool Has = false;
    CcKind Kind = CcKind::Root;
};

// The members are offered self first, then the partners in group order: a later one replaces the best so far only
// with a loss of control against a root
inline bool CcDispelBetter(CcDispelCandidate const& candidate, CcDispelCandidate const& best)
{
    if (!candidate.Has || !CcDispelWorth(candidate.Kind))
        return false;

    if (!best.Has)
        return true;

    return CcKindLosesControl(candidate.Kind) && !CcKindLosesControl(best.Kind);
}

// The cleanser spells a bot knows (G5a)
struct CcCleanserSpells
{
    bool DispelMagic = false;
    bool Cleanse = false;
    bool Purify = false;
    bool CleanseSpirit = false;
    bool CureToxins = false;
};

// The spell a cleanser frees a CC of the dispel type with (nullptr: none of its spells removes it): priest dispel
// magic (magic); paladin cleanse (magic, poison, disease) or purify (poison, disease); shaman cleanse spirit (curse,
// poison, disease: hex is a curse in this core) or cure toxins (poison, disease). Wyvern sting is a poison.
inline char const* CcDispelSpellFor(uint8 classId, uint32 dispelType, CcCleanserSpells const& known)
{
    bool const magic = dispelType == DISPEL_MAGIC;
    bool const toxin = dispelType == DISPEL_POISON || dispelType == DISPEL_DISEASE;
    switch (classId)
    {
        case CLASS_PRIEST:
            return magic && known.DispelMagic ? "dispel magic" : nullptr;
        case CLASS_PALADIN:
            if (known.Cleanse && (magic || toxin))
                return "cleanse";

            return toxin && known.Purify ? "purify" : nullptr;
        case CLASS_SHAMAN:
            if (known.CleanseSpirit && (dispelType == DISPEL_CURSE || toxin))
                return "cleanse spirit";

            return toxin && known.CureToxins ? "cure toxins" : nullptr;
        default:
            return nullptr;
    }
}

// G5a review: Avoid.EnsureTrinket's slot for the given PvP trinket (0: the first trinket slot, 1: the second). An
// empty slot first, then one whose item no PvP gear row tracks (Playerbots.PvpGear.Wear's rows must find their items
// in their slots), the second slot last (its loadout item goes to the bags for the fight and back after it)
constexpr uint8 CcEnsureTrinketSlot(bool firstEmpty, bool firstWorn, bool secondEmpty, bool secondWorn)
{
    if (secondEmpty)
        return 1;

    if (firstEmpty)
        return 0;

    if (!secondWorn)
        return 1;

    return firstWorn ? 1 : 0;
}

// G5a review: a cast of dispel cc on party, waiting for its CC to go (the dispel removes a random debuff of its type,
// so the CC may stay): the member (raw guid), the CC aura's spell, the cast's time and spell
struct CcPendingDispel
{
    uint64 Target = 0;
    uint32 AuraSpell = 0;
    uint32 CastMs = 0;
    char const* Spell = nullptr;
};

// The CC aura left the member by a dispel (AURA_REMOVE_BY_ENEMY_SPELL) within windowMs of the cast it was for
inline bool CcDispelFreed(CcPendingDispel const& pending, uint64 target, uint32 auraSpell, bool byDispel, uint32 nowMs,
                          uint32 windowMs)
{
    return byDispel && pending.AuraSpell && pending.Target == target && pending.AuraSpell == auraSpell &&
           nowMs - pending.CastMs <= windowMs;
}

#endif
