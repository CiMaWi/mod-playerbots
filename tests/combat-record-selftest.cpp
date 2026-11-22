/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of what the combat record's hooks compute (combat-foundation CFM3a; src/Bot/Engine/
// CombatRecordRules.h, MEASURES §1): the overheal, the heal slot (an empty slot, a periodic heal's second store),
// the health before a heal, the damage table (merge, overflow to the rest row), the hostile-only rule for periodic
// calls, the targets window, the key and seconds packing, the off and dispel words. It also times the hook's off path
// (CombatRecord::Any with no traced bot), the cost every server pays per hook call.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /O2 /I src/Bot/Engine tests/combat-record-selftest.cpp

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "CombatRecord.h"
#include "CombatRecordRules.h"

namespace
{
using namespace CombatRecordRules;

int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name.c_str());
    (pass ? passed : failed)++;
}

void Heal()
{
    Check(Overheal(1000, 1000) == 0, "overheal 0: all of it landed");
    Check(Overheal(1000, 400) == 600, "overheal partial: 600 of 1000");
    Check(Overheal(1000, 0) == 1000, "overheal full: the target was full");
    Check(Overheal(0, 300) == 0, "overheal never negative");

    HealSlot slot;
    HealSlot const empty = slot.Take();
    Check(!empty.set && empty.spell == 0 && empty.amount == 0, "empty slot: spell 0, amount 0");
    Check(Overheal(empty.amount, 500) == 0, "empty slot: over 0");

    slot.Store(800, 139);
    slot.Store(900, 139);  // a periodic heal: the tick, then the heal itself
    HealSlot const taken = slot.Take();
    Check(taken.set && taken.amount == 900 && taken.spell == 139, "slot: the later store wins");
    Check(!slot.set && slot.amount == 0, "slot: take clears it");

    Check(HealthPctBefore(1000, 0, 1000) == 100, "hp before: full target");
    Check(HealthPctBefore(800, 300, 1000) == 50, "hp before: 50 % before a 300 heal");
    Check(HealthPctBefore(100, 300, 1000) == 0, "hp before: never below 0");
    Check(HealthPctBefore(100, 0, 0) == 0, "hp before: no max health");
}

void Table()
{
    DealtTable table;
    table.Add(DealtMode::Melee, 0, 100, 7, 1000);
    table.Add(DealtMode::Melee, 0, 150, 7, 3000);
    table.Add(DealtMode::Direct, 133, 400, 7, 1500);
    Check(table.used == 2, "table: two rows for melee and one spell");
    Check(table.rows[0].sum == 250 && table.rows[0].hits == 2, "table: melee merged, 250 in 2 hits");
    Check(table.rows[1].spell == 133 && table.rows[1].mode == DealtMode::Direct, "table: spell row keyed");
    table.Add(DealtMode::Pet, 133, 50, 7, 1600);
    Check(table.used == 3, "table: the same spell from the pet is its own row");

    DealtTable full;
    for (std::uint32_t spell = 1; spell <= DEALT_SPELLS; ++spell)
        full.Add(DealtMode::Direct, spell, 10, 1, spell * 1000);
    Check(full.used == DEALT_SPELLS && !full.HasRest(), "table: 24 spells fill it");
    full.Add(DealtMode::Direct, 99, 30, 1, 50000);
    full.Add(DealtMode::Periodic, 98, 20, 2, 51000);
    full.Add(DealtMode::Direct, 5, 10, 1, 52000);
    DealtRow const& rest = full.rows[DEALT_SPELLS];
    Check(full.HasRest() && rest.mode == DealtMode::Rest && rest.sum == 50 && rest.hits == 2,
          "table overflow: spells 25 and 26 go to the rest row");
    Check(full.rows[4].sum == 20 && full.rows[4].hits == 2, "table overflow: a known spell still merges");
    full.Clear();
    Check(full.used == 0 && !full.HasRest() && full.totals.done == 0, "table: clear");

    Check(CountsAsDamage(false, false), "direct damage counts whatever the relation");
    Check(CountsAsDamage(true, true), "periodic on a hostile target counts");
    Check(!CountsAsDamage(true, false), "periodic on a friend (a heal's tick) does not count");
    Check(DEALT_WORDS[static_cast<std::size_t>(DealtMode::Rest)] == "rest", "dealt words: rest last");
}

void Targets()
{
    TargetsWindow w;
    w.Hit(1, 1000);
    w.Hit(2, 1050);
    w.Hit(3, 1100);
    w.Hit(2, 1150);
    Check(w.most == 3, "targets: three units in 0.2 s");
    w.Hit(4, 1300);  // a new window from 1300
    w.Hit(5, 1350);
    Check(w.most == 3 && w.count == 2, "targets: a later window of two keeps the most at 3");
    for (std::uint32_t key = 10; key < 40; ++key)
        w.Hit(key, 2000);
    Check(w.most == TARGETS_KEYS, "targets: capped at the window's keys");
    TargetsWindow wrap;
    wrap.Hit(1, 0xFFFFFFF0u);
    wrap.Hit(2, 0x00000010u);  // getMSTime wrapped: 32 ms later
    Check(wrap.most == 2, "targets: the window across the clock's wrap");
}

void Packing()
{
    Check(KeyNumber(12345) == 12345.0f, "key: exact below 2^24");
    Check(KeyNumber(KEY_EXACT_MAX) == 16777216.0f, "key: 2^24 itself is exact");
    Check(KeyNumber(KEY_EXACT_MAX + 1) == 0.0f, "key: above 2^24 written as 0");
    Check(Seconds(-1) == -1.0f && Seconds(1500) == 1.5f && Seconds(0) == 0.0f, "seconds: none, 1.5, 0");
    Check(OffWord(RemoveMode::Expire) == "expired" && OffWord(RemoveMode::EnemySpell) == "dispelled" &&
              OffWord(RemoveMode::Cancel) == "cancelled" && OffWord(RemoveMode::Death) == "death" &&
              OffWord(RemoveMode::Default) == "other" && OffWord(RemoveMode::None) == "other",
          "off words");
    Check(DispelWord(1) == "magic" && DispelWord(2) == "curse" && DispelWord(3) == "disease" &&
              DispelWord(4) == "poison" && DispelWord(9) == "enrage" && DispelWord(0) == "none" &&
              DispelWord(5) == "none",
          "dispel words");
}

// The hook's off path: the relaxed load and the branch, timed over many calls
void OffPath()
{
    constexpr std::uint64_t CALLS = 200000000;
    std::uint64_t hits = 0;
    auto const start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < CALLS; ++i)
        if (CombatRecord::Any())
            ++hits;
    auto const ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("off path: %.3f ns per hook call (%llu calls)\n", ns / static_cast<double>(CALLS),
                static_cast<unsigned long long>(CALLS));
    Check(hits == 0, "off path: no traced bot, every call returns");
}
}  // namespace

int main()
{
    Heal();
    Table();
    Targets();
    Packing();
    OffPath();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
