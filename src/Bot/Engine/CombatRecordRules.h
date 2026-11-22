/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATRECORDRULES_H
#define _PLAYERBOT_COMBATRECORDRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// What the combat record's hooks compute (combat-foundation CFM3a, MEASURES §1), free of core includes so
// tests/combat-record-selftest.cpp proves it offline: the overheal, the heal slot, the off words, the damage table.
namespace CombatRecordRules
{
// A unit key as a named number: floats are exact to 2^24, a larger key is written as 0 (ref keeps it exact)
constexpr std::uint32_t KEY_EXACT_MAX = 16777216;
constexpr float KeyNumber(std::uint32_t key) { return key > KEY_EXACT_MAX ? 0.0f : static_cast<float>(key); }

// combat_heal's over: the amount before the heal minus what landed (a heal absorb's share included)
constexpr std::uint32_t Overheal(std::uint32_t before, std::uint32_t landed)
{
    return before > landed ? before - landed : 0;
}

// The target's health % before a heal that raised it by landed
constexpr std::uint32_t HealthPctBefore(std::uint64_t health, std::uint32_t landed, std::uint64_t maxHealth)
{
    if (!maxHealth)
        return 0;
    std::uint64_t const before = health > landed ? health - landed : 0;
    return static_cast<std::uint32_t>(before * 100 / maxHealth);
}

// Milliseconds as a row's seconds; a negative time (no end) is -1
constexpr float Seconds(std::int32_t ms) { return ms < 0 ? -1.0f : static_cast<float>(ms) / 1000.0f; }

// The amount before a heal, carried from ModifyHealReceived to OnHeal on one thread (a periodic heal stores twice:
// the later store wins). Take empties it; an empty slot reads spell 0 and amount 0.
struct HealSlot
{
    std::uint32_t amount = 0;
    std::uint32_t spell = 0;
    bool set = false;

    void Store(std::uint32_t heal, std::uint32_t spellId)
    {
        amount = heal;
        spell = spellId;
        set = true;
    }

    HealSlot Take()
    {
        HealSlot const taken = *this;
        *this = HealSlot();
        return taken;
    }
};

// The core's AuraRemoveMode, numbered as there (static_assert in CombatRecord.cpp)
enum class RemoveMode : std::uint8_t
{
    None,
    Default,
    Cancel,
    EnemySpell,
    Expire,
    Death
};

// combat_effect off's reason: a dispel shows as the enemy-spell mode (no dispeller is passed)
constexpr std::string_view OffWord(RemoveMode mode)
{
    switch (mode)
    {
        case RemoveMode::Expire:
            return "expired";
        case RemoveMode::EnemySpell:
            return "dispelled";
        case RemoveMode::Cancel:
            return "cancelled";
        case RemoveMode::Death:
            return "death";
        default:
            return "other";
    }
}

// The dispel type's word (the core's DispelType numbers; the kinds without a dispel word read none)
constexpr std::string_view DispelWord(std::uint32_t dispel)
{
    switch (dispel)
    {
        case 1:
            return "magic";
        case 2:
            return "curse";
        case 3:
            return "disease";
        case 4:
            return "poison";
        case 9:
            return "enrage";
        default:
            return "none";
    }
}

// combat_dealt's reasons; Rest is the one row for every spell past the table's 24
enum class DealtMode : std::uint8_t
{
    Direct,
    Periodic,
    Melee,
    Pet,
    Rest
};
constexpr std::array<std::string_view, 5> DEALT_WORDS = {"direct", "periodic", "melee", "pet", "rest"};

// A periodic hook call counts as damage only on a target hostile to its caster: a periodic heal's tick calls it too
constexpr bool CountsAsDamage(bool periodic, bool hostile) { return !periodic || hostile; }

// targets: the most distinct units hit within TARGETS_WINDOW_MS of a window's first hit
constexpr std::uint32_t TARGETS_WINDOW_MS = 200;
constexpr std::size_t TARGETS_KEYS = 16;

struct TargetsWindow
{
    std::array<std::uint32_t, TARGETS_KEYS> keys{};
    std::uint32_t startMs = 0;
    std::uint8_t count = 0;
    std::uint8_t most = 0;

    void Hit(std::uint32_t key, std::uint32_t nowMs)
    {
        if (!count || nowMs - startMs > TARGETS_WINDOW_MS)
        {
            startMs = nowMs;
            count = 0;
        }
        for (std::size_t i = 0; i < count; ++i)
            if (keys[i] == key)
                return;
        if (count < TARGETS_KEYS)
            keys[count++] = key;
        most = std::max(most, count);
    }
};

struct DealtRow
{
    DealtMode mode = DealtMode::Direct;
    std::uint32_t spell = 0;
    std::uint64_t sum = 0;
    std::uint32_t hits = 0;
    TargetsWindow targets;
};

// One fight's totals from OnDamage and OnHeal (final amounts)
struct Totals
{
    std::uint64_t done = 0;
    std::uint64_t pet = 0;
    std::uint64_t taken = 0;
    std::uint64_t healed = 0;
};

// A traced bot's damage per mode and spell in the current fight: DEALT_SPELLS rows, then one row for the rest
constexpr std::size_t DEALT_SPELLS = 24;

struct DealtTable
{
    std::array<DealtRow, DEALT_SPELLS + 1> rows{};
    std::size_t used = 0;
    Totals totals;

    // amount before absorb and resist; spell 0 for a melee swing
    void Add(DealtMode mode, std::uint32_t spell, std::uint32_t amount, std::uint32_t target, std::uint32_t nowMs)
    {
        DealtRow* row = nullptr;
        for (std::size_t i = 0; i < used && !row; ++i)
            if (rows[i].mode == mode && rows[i].spell == spell)
                row = &rows[i];
        if (!row && used < DEALT_SPELLS)
        {
            row = &rows[used++];
            row->mode = mode;
            row->spell = spell;
        }
        if (!row)
        {
            row = &rows[DEALT_SPELLS];
            row->mode = DealtMode::Rest;
            row->spell = 0;
        }
        row->sum += amount;
        ++row->hits;
        row->targets.Hit(target, nowMs);
    }

    bool HasRest() const { return rows[DEALT_SPELLS].hits > 0; }

    void Clear() { *this = DealtTable(); }
};
}  // namespace CombatRecordRules

#endif
