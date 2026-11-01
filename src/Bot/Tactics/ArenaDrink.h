/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENADRINK_H
#define _PLAYERBOT_ARENADRINK_H

#include <algorithm>
#include <string>

#include "ArenaKiteConfig.h"
#include "Common.h"

// The Drink tactic's pure parts (G3b, PLAN 3.15; numbers DrinkConfig, DT.9 scope arena and battleground, DT.10 skill
// and effort). A bot tops up its mana before the gates open (preparation) and drinks in a safe window of a match; below
// the knowledge threshold the old food rule runs unchanged, below the effort threshold the bot does not drink (it wands
// or stands). The game side (DrinkTacticStrategy.cpp) reads the situation, this header judges it; .playerbots tactics
// selftest drink and the offline harness run the same checks.

constexpr float DRINK_NO_ENEMY_YARDS = 100000.0f;  // "no enemy seen"
constexpr uint32 DRINK_NEVER = 0xFFFFFFFFu;        // "no enemy seen yet" for the quiet time
constexpr uint32 DRINK_SENSE_MS = 1000;            // enemies, rays and partners are read at most this often per bot
constexpr uint32 DRINK_PARAMS_MS = 5000;           // the gate numbers are read again at most this often
constexpr uint32 DRINK_HOLD_MAX_MS = 6000;         // the food cheat's hold (the action's delay) at most this long
constexpr uint32 DRINK_MAX_RAYS = 4;               // line-of-sight rays per sense (the nearest enemies)
constexpr uint32 DRINK_MAX_ENEMIES = 16;           // enemies looked at per sense (distances only)
constexpr float DRINK_PARTNER_YARDS = 40.0f;       // battleground: a hurt partner counts within this
constexpr uint32 DRINK_CHEAT_AURA = 25990;         // the food cheat's drink aura (DrinkAction)

enum class DrinkWhy : uint8
{
    Go,
    NotKnown,     // below the knowledge threshold: the old food rule decides
    LowEffort,    // the effort is below the threshold of this kind of drink
    NotWaiting,   // preparation: the gates are not closed
    TooLate,      // preparation: PrepMinLeftMs or less left
    NotStarted,   // safe window: the match is not in progress
    NoMana,       // no mana pool
    ManaHigh,     // mana at or above the threshold
    Drinking,     // already drinking
    Combat,       // in combat
    EnemyNear,    // an enemy within EnemyYards
    EnemyLos,     // an enemy in line of sight
    EnemyRecent,  // an enemy near or in sight less than QuietMs ago
    PartnerDead,  // a partner is dead
    PartnerLow,   // a partner is at or below PartnerMinHpPct
    NoDrink,      // nothing to drink (no cheat, no item)
    Count
};

inline char const* DrinkWhyName(DrinkWhy why)
{
    static constexpr char const* NAMES[] = {"go",           "not known",    "low effort",  "not waiting",
                                            "too late",     "not started",  "no mana",     "mana high",
                                            "drinking",     "combat",       "enemy near",  "enemy in los",
                                            "enemy recent", "partner dead", "partner low", "no drink"};
    uint32 const index = static_cast<uint32>(why);
    return index < static_cast<uint32>(DrinkWhy::Count) ? NAMES[index] : "?";
}

enum class DrinkBreakWhy : uint8
{
    None,
    Combat,
    EnemyNear,
    EnemyLos,
    PartnerDead,
    PartnerLow,
    Count
};

inline char const* DrinkBreakWhyName(DrinkBreakWhy why)
{
    static constexpr char const* NAMES[] = {"none",         "combat",       "enemy near",
                                            "enemy in los", "partner dead", "partner low"};
    uint32 const index = static_cast<uint32>(why);
    return index < static_cast<uint32>(DrinkBreakWhy::Count) ? NAMES[index] : "?";
}

// What the gate reads of a bot (as LosGateInputs): the skill (0 = full ability), the knowledge attribute, the effort
struct DrinkGateInputs
{
    uint8 Skill = 0;
    uint8 Knowledge = 0;
    uint8 Effort = 100;
    bool FullAbility = true;
};

// One bot's numbers, read again every DRINK_PARAMS_MS
struct DrinkParams
{
    bool Known = true;   // knows the tactic (below it the old rule runs)
    bool PrepOk = true;  // the effort lets it drink in preparation
    bool SafeOk = true;  // the effort lets it drink in a safe window
};

inline DrinkParams ComputeDrinkParams(DrinkConfig const& config, DrinkGateInputs const& in)
{
    bool const full = in.FullAbility || in.Skill == 0;
    DrinkParams params;
    params.Known = full || in.Knowledge >= config.KnownFromSkill;
    params.PrepOk = params.Known && (full || in.Effort >= config.PrepEffortMin);
    params.SafeOk = params.Known && (full || in.Effort >= config.EffortMin);
    return params;
}

// What the game side saw (all cheap reads; the enemy and partner fields come from the once-per-second sense)
struct DrinkSituation
{
    bool HasMana = true;
    uint32 ManaPct = 100;
    bool Healer = false;
    bool Drinking = false;
    bool InCombat = false;
    bool Waiting = false;                     // the gates are closed (battleground status WAIT_JOIN)
    bool InProgress = false;                  // the match runs
    int32 LeftMs = 0;                         // until the gates open (the battleground's start delay)
    float EnemyYards = DRINK_NO_ENEMY_YARDS;  // the nearest enemy
    bool EnemyInLos = false;                  // one of the nearest enemies sees the bot (the spell check's ray)
    uint32 SinceEnemyMs = DRINK_NEVER;        // since an enemy was last near or in sight
    bool PartnersAlive = true;
    uint32 PartnerMinHpPct = 100;  // the lowest health of the partners
    bool HasDrink = true;          // the food cheat or a drink item
};

// Preparation: the gates are closed, more than PrepMinLeftMs are left, the mana is below PrepBelowPct. Enemies do not
// matter (the other team is behind its gates).
inline DrinkWhy PrepDrinkVerdict(DrinkConfig const& config, DrinkParams const& params, DrinkSituation const& s)
{
    if (!params.Known)
        return DrinkWhy::NotKnown;

    if (!params.PrepOk)
        return DrinkWhy::LowEffort;

    if (!s.Waiting)
        return DrinkWhy::NotWaiting;

    if (s.LeftMs <= static_cast<int32>(config.PrepMinLeftMs))
        return DrinkWhy::TooLate;

    if (!s.HasMana)
        return DrinkWhy::NoMana;

    if (s.ManaPct >= config.PrepBelowPct)
        return DrinkWhy::ManaHigh;

    if (s.Drinking)
        return DrinkWhy::Drinking;

    if (s.InCombat)
        return DrinkWhy::Combat;

    return s.HasDrink ? DrinkWhy::Go : DrinkWhy::NoDrink;
}

// A safe window, the part that needs no sense (Go: the sense is worth reading now)
inline DrinkWhy SafeDrinkCheap(DrinkConfig const& config, DrinkParams const& params, DrinkSituation const& s)
{
    if (!params.Known)
        return DrinkWhy::NotKnown;

    if (!params.SafeOk)
        return DrinkWhy::LowEffort;

    if (!s.InProgress)
        return DrinkWhy::NotStarted;

    if (!s.HasMana)
        return DrinkWhy::NoMana;

    if (s.ManaPct >= (s.Healer ? config.SafeBelowPctHealer : config.SafeBelowPct))
        return DrinkWhy::ManaHigh;

    if (s.Drinking)
        return DrinkWhy::Drinking;

    return s.InCombat ? DrinkWhy::Combat : DrinkWhy::Go;
}

// A safe window: out of combat, mana below the threshold, no enemy within EnemyYards or in sight, none seen for
// QuietMs, every partner alive and above PartnerMinHpPct
inline DrinkWhy SafeDrinkVerdict(DrinkConfig const& config, DrinkParams const& params, DrinkSituation const& s)
{
    DrinkWhy const cheap = SafeDrinkCheap(config, params, s);
    if (cheap != DrinkWhy::Go)
        return cheap;

    if (s.EnemyYards <= static_cast<float>(config.EnemyYards))
        return DrinkWhy::EnemyNear;

    if (s.EnemyInLos)
        return DrinkWhy::EnemyLos;

    if (s.SinceEnemyMs < config.QuietMs)
        return DrinkWhy::EnemyRecent;

    if (!s.PartnersAlive)
        return DrinkWhy::PartnerDead;

    if (s.PartnerMinHpPct <= config.PartnerMinHpPct)
        return DrinkWhy::PartnerLow;

    return s.HasDrink ? DrinkWhy::Go : DrinkWhy::NoDrink;
}

// A running drink: does the bot stand up? Combat always; an enemy within BreakEnemyYards (nearer than the start rule,
// so that a drink does not flap), in sight, a partner dead or below PartnerMinHpPct once the match runs. While the
// gates are closed a preparation drink runs on.
inline DrinkBreakWhy DrinkBreakVerdict(DrinkConfig const& config, DrinkSituation const& s)
{
    if (s.InCombat)
        return DrinkBreakWhy::Combat;

    if (s.Waiting)
        return DrinkBreakWhy::None;

    if (s.EnemyYards <= static_cast<float>(config.BreakEnemyYards))
        return DrinkBreakWhy::EnemyNear;

    if (s.EnemyInLos)
        return DrinkBreakWhy::EnemyLos;

    if (!s.PartnersAlive)
        return DrinkBreakWhy::PartnerDead;

    if (s.PartnerMinHpPct < config.PartnerMinHpPct)
        return DrinkBreakWhy::PartnerLow;

    return DrinkBreakWhy::None;
}

// How a started drink ended, seen from the bot's own updates
enum class DrinkEnd : uint8
{
    None,          // still drinking (or none started)
    Ended,         // over by itself (the aura ran out, or the bot moved on)
    BrokenCombat,  // combat began
    BrokenEarly    // the cheat's aura was cut short (the bot stood or moved before it ran out)
};

// The Drink tactic's per-bot state
struct DrinkBotState
{
    uint32 StartMs = 0;  // when the running drink started (0: none)
    uint32 NoDrinkLogMs = 0;
    bool Prep = false;
    uint32 ExpectedEndMs = 0;  // when the cheat's aura ends (0: unknown)
    DrinkParams Params;
    uint32 ParamsMs = 0;
    bool Healer = false;        // read with the params (the healer's higher threshold)
    uint32 NoDrinkUntilMs = 0;  // after a "nothing to drink" verdict the item count is not asked again before this
    uint32 Instance = 0;        // the battleground instance the state belongs to (a new one resets the senses)
    // The once-per-second sense
    uint32 SenseMs = 0;
    float EnemyYards = DRINK_NO_ENEMY_YARDS;
    bool EnemyInLos = false;
    uint32 EnemySeenMs = 0;  // when an enemy was last near or in sight (0: not yet)
    bool PartnersAlive = true;
    uint32 PartnerMinHpPct = 100;
    // The last verdict (tactics show)
    DrinkWhy LastWhy = DrinkWhy::Go;
    DrinkBreakWhy LastBreak = DrinkBreakWhy::None;
    // Counters of the bot's life (tactics show)
    uint32 Starts = 0;
    uint32 PrepStarts = 0;
    uint32 Broken = 0;
    uint32 NoItem = 0;
};

inline bool DrinkSenseDue(DrinkBotState const& state, uint32 nowMs)
{
    return !state.SenseMs || nowMs - state.SenseMs >= DRINK_SENSE_MS;
}

// Take a sense: the nearest enemy, whether one sees the bot, the partners. An enemy near or in sight is "seen now".
inline void DrinkNoteSense(DrinkBotState& state, DrinkConfig const& config, uint32 nowMs, float enemyYards,
                           bool enemyInLos, bool partnersAlive, uint32 partnerMinHpPct)
{
    state.SenseMs = std::max<uint32>(nowMs, 1);
    state.EnemyYards = enemyYards;
    state.EnemyInLos = enemyInLos;
    state.PartnersAlive = partnersAlive;
    state.PartnerMinHpPct = partnerMinHpPct;
    if (enemyInLos || enemyYards <= static_cast<float>(config.EnemyYards))
        state.EnemySeenMs = state.SenseMs;
}

inline uint32 DrinkSinceEnemyMs(DrinkBotState const& state, uint32 nowMs)
{
    return state.EnemySeenMs ? nowMs - state.EnemySeenMs : DRINK_NEVER;
}

// A new match (or none): the senses and the running drink are forgotten, the counters stay
inline void DrinkResetMatch(DrinkBotState& state)
{
    state.StartMs = 0;
    state.Prep = false;
    state.ExpectedEndMs = 0;
    state.NoDrinkUntilMs = 0;
    state.SenseMs = 0;
    state.EnemyYards = DRINK_NO_ENEMY_YARDS;
    state.EnemyInLos = false;
    state.EnemySeenMs = 0;
    state.PartnersAlive = true;
    state.PartnerMinHpPct = 100;
    state.LastWhy = DrinkWhy::Go;
    state.LastBreak = DrinkBreakWhy::None;
}

inline void DrinkNoteStart(DrinkBotState& state, uint32 nowMs, bool prep, uint32 expectedEndMs)
{
    state.StartMs = std::max<uint32>(nowMs, 1);
    state.Prep = prep;
    state.ExpectedEndMs = expectedEndMs;
    ++state.Starts;
    if (prep)
        ++state.PrepStarts;
}

// The bot is not drinking (any more): did a started drink end, and how? A drink that ends more than a second before
// the cheat's aura would is broken, as one that ends in combat.
inline DrinkEnd DrinkSettle(DrinkBotState& state, bool drinking, bool inCombat, uint32 nowMs)
{
    if (!state.StartMs || drinking)
        return DrinkEnd::None;

    bool const early = state.ExpectedEndMs && nowMs + 1000 < state.ExpectedEndMs;
    state.StartMs = 0;
    state.ExpectedEndMs = 0;
    if (inCombat)
    {
        ++state.Broken;
        return DrinkEnd::BrokenCombat;
    }

    if (early)
    {
        ++state.Broken;
        return DrinkEnd::BrokenEarly;
    }

    return DrinkEnd::Ended;
}

// The "no drink" line: at most every NoDrinkLogMs per bot; true when this call is the one (the bot's state notes it)
inline bool DrinkNoItemDue(DrinkBotState& state, DrinkConfig const& config, uint32 nowMs)
{
    if (state.NoDrinkLogMs && nowMs - state.NoDrinkLogMs < config.NoDrinkLogMs)
        return false;

    state.NoDrinkLogMs = std::max<uint32>(nowMs, 1);
    ++state.NoItem;
    return true;
}

// The selftest (tactics selftest drink; the offline harness runs the same checks)
template <typename Check>
void RunDrinkChecks(Check&& check)
{
    DrinkConfig const config;
    auto inputs = [](uint8 skill, uint8 effort = 100, bool full = false)
    {
        DrinkGateInputs in;
        in.Skill = skill;
        in.Knowledge = skill;
        in.Effort = effort;
        in.FullAbility = full;
        return in;
    };

    // Gates by skill and effort
    DrinkParams const full = ComputeDrinkParams(config, inputs(0, 0, true));
    check(full.Known && full.PrepOk && full.SafeOk,
          "full ability knows the tactic and has the effort whatever it reads");
    check(!ComputeDrinkParams(config, inputs(44)).Known && ComputeDrinkParams(config, inputs(45)).Known,
          "known from KnownFromSkill (45)");
    check(ComputeDrinkParams(config, inputs(80, 10)).PrepOk && !ComputeDrinkParams(config, inputs(80, 9)).PrepOk,
          "preparation drinks from PrepEffortMin (10)");
    check(ComputeDrinkParams(config, inputs(80, 30)).SafeOk && !ComputeDrinkParams(config, inputs(80, 29)).SafeOk,
          "safe-window drinks from EffortMin (30)");
    DrinkParams const lazy = ComputeDrinkParams(config, inputs(80, 20));
    check(lazy.Known && lazy.PrepOk && !lazy.SafeOk, "a low-effort bot drinks in preparation but not in a match");
    DrinkParams const green = ComputeDrinkParams(config, inputs(30, 100));
    check(!green.Known && !green.PrepOk && !green.SafeOk, "below the knowledge nothing: the old food rule runs");

    DrinkParams const good = ComputeDrinkParams(config, inputs(80, 100));

    // Preparation
    DrinkSituation prep;
    prep.Waiting = true;
    prep.LeftMs = 30000;
    prep.ManaPct = 60;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::Go, "prep: gates closed, 30 s left, mana 60 %: drink");
    prep.ManaPct = 94;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::Go, "prep: mana 94 % still drinks");
    prep.ManaPct = 95;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::ManaHigh, "prep: mana 95 % does not");
    prep.ManaPct = 60;
    prep.LeftMs = 12001;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::Go, "prep: 12.001 s left drinks");
    prep.LeftMs = 12000;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::TooLate, "prep: 12 s left is too late");
    prep.LeftMs = -500;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::TooLate, "prep: a start delay below 0 is too late");
    prep.LeftMs = 30000;
    prep.Waiting = false;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::NotWaiting, "prep: gates open: not a preparation drink");
    prep.Waiting = true;
    prep.EnemyYards = 3.0f;
    prep.EnemyInLos = true;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::Go, "prep: an enemy behind the gates does not matter");
    prep.HasMana = false;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::NoMana, "prep: no mana pool");
    prep.HasMana = true;
    prep.Drinking = true;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::Drinking, "prep: already drinking");
    prep.Drinking = false;
    prep.InCombat = true;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::Combat, "prep: in combat");
    prep.InCombat = false;
    prep.HasDrink = false;
    check(PrepDrinkVerdict(config, good, prep) == DrinkWhy::NoDrink, "prep: nothing to drink");
    prep.HasDrink = true;
    check(PrepDrinkVerdict(config, green, prep) == DrinkWhy::NotKnown, "prep: below the knowledge: not known");
    check(PrepDrinkVerdict(config, ComputeDrinkParams(config, inputs(80, 5)), prep) == DrinkWhy::LowEffort,
          "prep: effort 5: low effort");
    check(PrepDrinkVerdict(config, lazy, prep) == DrinkWhy::Go, "prep: effort 20 still tops up before the gates");

    // Safe window
    DrinkSituation safe;
    safe.InProgress = true;
    safe.ManaPct = 40;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Go, "safe: mana 40 %, nobody near, partners fine: drink");
    safe.ManaPct = 59;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Go, "safe: mana 59 % drinks");
    safe.ManaPct = 60;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::ManaHigh, "safe: mana 60 % does not (not a healer)");
    safe.Healer = true;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Go, "safe: a healer drinks at 60 % (threshold 70)");
    safe.ManaPct = 70;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::ManaHigh, "safe: a healer at 70 % does not");
    safe.Healer = false;
    safe.ManaPct = 30;
    safe.InProgress = false;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::NotStarted, "safe: not before the match runs");
    safe.InProgress = true;
    safe.InCombat = true;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Combat, "safe: not in combat");
    safe.InCombat = false;
    safe.EnemyYards = 30.0f;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::EnemyNear, "safe: an enemy at 30 yd is near");
    safe.EnemyYards = 30.5f;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Go, "safe: an enemy at 30.5 yd is not");
    safe.EnemyInLos = true;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::EnemyLos, "safe: an enemy in sight stops it at any range");
    safe.EnemyInLos = false;
    safe.SinceEnemyMs = 1999;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::EnemyRecent, "safe: an enemy seen 1.999 s ago is recent");
    safe.SinceEnemyMs = 2000;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Go, "safe: quiet for 2 s drinks");
    safe.PartnersAlive = false;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::PartnerDead, "safe: a dead partner");
    safe.PartnersAlive = true;
    safe.PartnerMinHpPct = 50;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::PartnerLow, "safe: a partner at 50 % is not above 50 %");
    safe.PartnerMinHpPct = 51;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Go, "safe: a partner at 51 % is");
    safe.Drinking = true;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::Drinking, "safe: already drinking");
    safe.Drinking = false;
    safe.HasDrink = false;
    check(SafeDrinkVerdict(config, good, safe) == DrinkWhy::NoDrink, "safe: nothing to drink (no cheat, no item)");
    safe.HasDrink = true;
    check(SafeDrinkVerdict(config, lazy, safe) == DrinkWhy::LowEffort,
          "safe: a low-effort bot does not drink in a match (it wands or stands)");
    check(SafeDrinkVerdict(config, green, safe) == DrinkWhy::NotKnown,
          "safe: below the knowledge: the old rule decides");
    check(SafeDrinkCheap(config, good, safe) == DrinkWhy::Go, "safe: the cheap part says go before the sense is read");
    DrinkSituation nearEnemy = safe;
    nearEnemy.EnemyYards = 5.0f;
    check(SafeDrinkCheap(config, good, nearEnemy) == DrinkWhy::Go, "safe: the cheap part does not look at enemies");

    // Break
    DrinkSituation run;
    run.InProgress = true;
    run.Drinking = true;
    run.ManaPct = 40;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::None, "break: quiet: keeps drinking");
    run.EnemyYards = 25.0f;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::None,
          "break: an enemy at 25 yd is no reason (start rule 30, break 20)");
    run.EnemyYards = 20.0f;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::EnemyNear, "break: an enemy within 20 yd stands the bot up");
    run.EnemyYards = DRINK_NO_ENEMY_YARDS;
    run.EnemyInLos = true;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::EnemyLos, "break: an enemy in sight");
    run.EnemyInLos = false;
    run.PartnerMinHpPct = 49;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::PartnerLow, "break: a partner below 50 %");
    run.PartnerMinHpPct = 50;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::None,
          "break: a partner at exactly 50 % neither starts nor breaks");
    run.PartnerMinHpPct = 100;
    run.PartnersAlive = false;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::PartnerDead, "break: a dead partner");
    run.PartnersAlive = true;
    run.InCombat = true;
    check(DrinkBreakVerdict(config, run) == DrinkBreakWhy::Combat, "break: combat");
    DrinkSituation closed = run;
    closed.InCombat = false;
    closed.Waiting = true;
    closed.InProgress = false;
    closed.EnemyYards = 2.0f;
    closed.EnemyInLos = true;
    check(DrinkBreakVerdict(config, closed) == DrinkBreakWhy::None,
          "break: a preparation drink runs on with the gates closed");

    // The sense and the state
    DrinkBotState state;
    check(DrinkSenseDue(state, 5000), "sense: due at first");
    DrinkNoteSense(state, config, 5000, 80.0f, false, true, 100);
    check(!DrinkSenseDue(state, 5999) && DrinkSenseDue(state, 6000), "sense: once per second");
    check(DrinkSinceEnemyMs(state, 6000) == DRINK_NEVER, "sense: an enemy at 80 yd out of sight is not seen");
    DrinkNoteSense(state, config, 6000, 20.0f, false, true, 100);
    check(DrinkSinceEnemyMs(state, 6500) == 500, "sense: an enemy within 30 yd is seen now");
    DrinkNoteSense(state, config, 7000, 80.0f, true, true, 100);
    check(DrinkSinceEnemyMs(state, 7000) == 0, "sense: an enemy in sight is seen now");
    DrinkNoteSense(state, config, 8000, 80.0f, false, true, 100);
    check(DrinkSinceEnemyMs(state, 9000) == 2000, "sense: the quiet time counts from the last sighting");
    DrinkResetMatch(state);
    check(DrinkSenseDue(state, 10) && DrinkSinceEnemyMs(state, 10) == DRINK_NEVER,
          "reset: a new match forgets the senses");

    // The drink's life
    DrinkBotState life;
    check(DrinkSettle(life, false, false, 1000) == DrinkEnd::None, "settle: nothing started, nothing ended");
    DrinkNoteStart(life, 1000, true, 11000);
    check(life.Starts == 1 && life.PrepStarts == 1 && life.StartMs == 1000, "start: counted, prep too");
    check(DrinkSettle(life, true, false, 2000) == DrinkEnd::None, "settle: still drinking");
    check(DrinkSettle(life, false, false, 11000) == DrinkEnd::Ended && life.Broken == 0,
          "settle: ran its course: ended");
    DrinkNoteStart(life, 20000, false, 30000);
    check(DrinkSettle(life, false, true, 21000) == DrinkEnd::BrokenCombat && life.Broken == 1,
          "settle: combat breaks it");
    DrinkNoteStart(life, 40000, false, 50000);
    check(DrinkSettle(life, false, false, 45000) == DrinkEnd::BrokenEarly && life.Broken == 2,
          "settle: stood 5 s before the aura ran out: broken");
    DrinkNoteStart(life, 60000, false, 0);
    check(DrinkSettle(life, false, false, 61000) == DrinkEnd::Ended,
          "settle: an item drink of unknown length just ended");
    check(life.Starts == 4 && life.PrepStarts == 1, "start: four counted, one of them prep");

    // The "no drink" line
    DrinkBotState nothing;
    check(DrinkNoItemDue(nothing, config, 100000), "no drink: the first is logged");
    check(!DrinkNoItemDue(nothing, config, 159999), "no drink: not again within NoDrinkLogMs");
    check(DrinkNoItemDue(nothing, config, 160000) && nothing.NoItem == 2, "no drink: again after 60 s, both counted");

    // Names
    check(std::string(DrinkWhyName(DrinkWhy::EnemyLos)) == "enemy in los" &&
              std::string(DrinkWhyName(DrinkWhy::NoDrink)) == "no drink",
          "names: the verdicts read");
    check(std::string(DrinkBreakWhyName(DrinkBreakWhy::PartnerLow)) == "partner low", "names: the breaks read");
}

#endif
