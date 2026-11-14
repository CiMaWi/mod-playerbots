/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTENTHUSIASM_H
#define _PLAYERBOT_FIGHTENTHUSIASM_H

#include <cstdint>

#include "Enthusiasm.h"

// A bot's enthusiasm as its AI holds it (WP23, PlayerbotAI::enthusiasm) and the steps the glue (FightEnthusiasm.cpp)
// takes on it: the seed, the engaged edges, the cached traits and local balance, the one-off edits, the pick and the
// entering check. Player-free, so tests/fight-enthusiasm-selftest.cpp checks the same code the server runs
namespace WorldPvp
{
// The target a bot entered on stays its scan's target this long (it closes in and attacks within it)
constexpr uint32_t PICK_MS = 20 * 1000;
// The traits, the real master and the prohibited zone are read again after this long
constexpr uint32_t ENTHUSIASM_CONTEXT_MS = 5 * 1000;
// The local balance and the graveyard's distance are read again after this long: the scan's cadence
// (EnemyPlayerValue, 1 s)
constexpr uint32_t BALANCE_REFRESH_MS = 1000;

// The terms of a join threshold, as the entering row's bit mask (alt.terms, world-log.md)
enum JoinTerm : uint32_t
{
    TERM_GREEN = 1,
    TERM_FACTION = 2,
    TERM_GROUP = 4,
    TERM_GRAVEYARD = 8,
    TERM_HELPFULNESS = 16,
    TERM_BALANCE = 32,
    TERM_WEAK_AGGRESSION = 64,
};

struct BotEnthusiasm
{
    Enthusiasm state;
    bool seeded = false;
    double seed = 0.0;
    bool seedShown = false;
    // The traits (-1..+1) and what follows from them alone, read at ENTHUSIASM_CONTEXT_MS
    bool contextKnown = false;
    uint32_t contextMs = 0;
    float aggression = 0.0f;
    float caution = 0.0f;
    float helpfulness = 0.0f;
    float confidence = 0.0f;
    float lowestJoin = 0.0f;  // LowestJoinThreshold
    float flee = 0.0f;        // FleeThreshold, no group member engaged (WP24 adds it)
    bool realMaster = false;
    bool prohibitedZone = false;
    // The local balance (LocalBalance) with its factor and shift, and the graveyard row, read at BALANCE_REFRESH_MS
    bool balanceKnown = false;
    uint32_t balanceMs = 0;
    float balance = EVEN_BALANCE;
    float factor = 1.0f;
    float joinShift = 0.0f;
    bool nearGraveyard = false;
    // The enemy the bot entered on (raw guid, 0 for none) and when
    uint64_t pickGuid = 0;
    uint32_t pickMs = 0;
    // In PvP combat with an enemy player (either side's combat reference), since engagedMs
    bool engaged = false;
    uint32_t engagedMs = 0;
};

// The first use starts the value at a random level (StartValue of `random`, a uniform draw); later calls do nothing
inline void Seed(BotEnthusiasm& bot, uint32_t random, uint32_t nowMs, EnthusiasmValues const& values)
{
    if (bot.seeded)
        return;
    bot.seed = StartValue(random, values);
    bot.state = {bot.seed, nowMs};
    bot.seeded = true;
}

inline double ValueAt(BotEnthusiasm const& bot, uint32_t nowMs, EnthusiasmValues const& values)
{
    return Read(bot.state, nowMs, bot.engaged, bot.factor, values);
}

// An engaged edge: settles the value with the old state first, then flips it; becoming engaged ends the pick (the
// fight goes on by the combat references). True on an edge
inline bool Observe(BotEnthusiasm& bot, bool engaged, uint32_t nowMs, EnthusiasmValues const& values)
{
    if (engaged == bot.engaged)
        return false;
    bot.state = Settle(bot.state, nowMs, bot.engaged, bot.factor, values);
    bot.engaged = engaged;
    bot.engagedMs = nowMs;
    if (engaged)
        bot.pickGuid = 0;
    return true;
}

// A new local balance: the value is settled with the old factor up to now, then the new factor and shift hold
inline void NewBalance(BotEnthusiasm& bot, float balance, bool nearGraveyard, uint32_t nowMs,
                       EnthusiasmValues const& values)
{
    bot.state = Settle(bot.state, nowMs, bot.engaged, bot.factor, values);
    bot.balance = balance;
    bot.factor = BalanceDepletionFactor(balance, values);
    bot.joinShift = BalanceJoinShift(balance, values);
    bot.nearGraveyard = nearGraveyard;
    bot.balanceKnown = true;
    bot.balanceMs = nowMs;
}

inline void NewTraits(BotEnthusiasm& bot, float aggression, float caution, float helpfulness, float confidence,
                      uint32_t nowMs, EnthusiasmValues const& values)
{
    bot.aggression = aggression;
    bot.caution = caution;
    bot.helpfulness = helpfulness;
    bot.confidence = confidence;
    bot.lowestJoin = LowestJoinThreshold(aggression, helpfulness, values);
    FleeFacts flee;
    flee.caution = caution;
    bot.flee = FleeThreshold(flee, values);
    bot.contextKnown = true;
    bot.contextMs = nowMs;
}

inline bool ContextDue(BotEnthusiasm const& bot, uint32_t nowMs)
{
    return !bot.contextKnown || nowMs - bot.contextMs >= ENTHUSIASM_CONTEXT_MS;
}

inline bool BalanceDue(BotEnthusiasm const& bot, uint32_t nowMs)
{
    return !bot.balanceKnown || nowMs - bot.balanceMs >= BALANCE_REFRESH_MS;
}

// A one-off edit (Apply with the bot's confidence); the value before it
inline double EditBy(BotEnthusiasm& bot, EditCause cause, uint32_t nowMs, EnthusiasmValues const& values)
{
    double const before = ValueAt(bot, nowMs, values);
    bot.state = Apply(bot.state, nowMs, bot.engaged, bot.factor, cause, bot.confidence, values);
    return before;
}

// A bot enters a fight only above its join threshold and not below its flee threshold: where the join threshold lies
// below the flee threshold, a value between them would pick a target and leave it at once
inline bool Enters(double value, float joinThreshold, float fleeThreshold)
{
    return MayJoin(value, joinThreshold) && !MustLeave(value, fleeThreshold);
}

inline bool IsPick(BotEnthusiasm const& bot, uint64_t guid, uint32_t nowMs)
{
    return guid && bot.pickGuid == guid && nowMs - bot.pickMs < PICK_MS;
}

inline void SetPick(BotEnthusiasm& bot, uint64_t guid, uint32_t nowMs)
{
    bot.pickGuid = guid;
    bot.pickMs = nowMs;
}

// The enemy-player scan is skipped while the value can enter nothing (not even at the lowest join threshold the
// traits allow) and no pick is open; attackers still reach the bot by its combat references
inline bool SkipsScan(BotEnthusiasm const& bot, uint32_t nowMs, EnthusiasmValues const& values)
{
    return !IsPick(bot, bot.pickGuid, nowMs) && !Enters(ValueAt(bot, nowMs, values), bot.lowestJoin, bot.flee);
}

// The join facts of a target from the cached state and what the caller sees of the target
inline JoinFacts FactsFor(BotEnthusiasm const& bot, Colour colour, bool targetAttacksFaction, bool targetAttacksGroup)
{
    JoinFacts facts;
    facts.aggression = bot.aggression;
    facts.helpfulness = bot.helpfulness;
    facts.colour = colour;
    facts.targetAttacksGroup = targetAttacksGroup;
    facts.targetAttacksFaction = targetAttacksFaction || targetAttacksGroup;
    facts.nearGraveyard = bot.nearGraveyard;
    facts.balance = bot.balance;
    return facts;
}

// The rows JoinThreshold applied to these facts (JoinTerm bits); none when Aggression alone puts a start at the top
inline uint32_t JoinTerms(JoinFacts const& facts, EnthusiasmValues const& values)
{
    bool const ongoing = FightOngoing(facts);
    if (!ongoing && JoinBaseThreshold(facts.aggression, false, values) >= values.max)
        return 0;
    uint32_t terms = 0;
    if (facts.colour == Colour::Green && values.joinGreen != 0.0f)
        terms |= TERM_GREEN;
    if (facts.targetAttacksFaction)
    {
        terms |= TERM_FACTION;
        if (facts.helpfulness > -1.0f && values.joinFactionHelpfulness != 0.0f)
            terms |= TERM_HELPFULNESS;
    }
    if (facts.targetAttacksGroup)
        terms |= TERM_GROUP;
    if (facts.nearGraveyard && values.joinGraveyard != 0.0f)
        terms |= TERM_GRAVEYARD;
    if (BalanceJoinShift(facts.balance, values) != 0.0f)
        terms |= TERM_BALANCE;
    if (ongoing && facts.aggression != 0.0f && values.joinAggressionFactor != 1.0f)
        terms |= TERM_WEAK_AGGRESSION;
    return terms;
}
}  // namespace WorldPvp

#endif
