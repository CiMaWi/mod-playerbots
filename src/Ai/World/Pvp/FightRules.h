/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTRULES_H
#define _PLAYERBOT_FIGHTRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "FightBookRules.h"
#include "WorldPvpConfig.h"

// The fight rules of world PvP (WP3): who may start a fight on whom and how likely, who joins one, who retreats, how
// far a winner chases, when a fight times out, the calm areas, the ghost's wait, grudges and the capture-point
// exception. Pure functions of what a player could see (levels, health, numbers, places, times, traits as -1..+1);
// no Player, no globals, no allocation, O(1): the caller passes pre-counted sums. tests/fight-rules-selftest.cpp
// proves them; WP4-WP9 call them.
//
// The start rate (WP.2): 0 at or below AggressionOffBelow, linear from there to r0 = 1 / HoursAtZero at aggression 0,
// then geometric to r1 = 1 / HoursAtOne at +1: rate(a) = r0 * (r1 / r0)^a, i.e. 4^a per 4 h with the defaults. Over
// aggression ~ N(0, 0.45) it averages about 0.27 per hour before the gap and the cap. At a chance the base
// probability is 1 - exp(-rate * dt), dt the bot's play time since its last chance, capped at ChanceCap. The soft
// factors multiply the capped base; all are at most 1 except the grudge's, and the result is at most 1.
namespace WorldPvp
{
constexpr float MS_PER_HOUR = 3600.0f * 1000.0f;
constexpr float SECONDS_PER_DAY = 86400.0f;
// "Never" for the time-since inputs, and "none in reach" for the distance inputs
constexpr uint32_t NEVER_MS = UINT32_MAX;
constexpr float FAR_YARDS = 1.0e9f;
// A side's share of the visible strength at even odds, and the health below which a target counts as weakened
constexpr float EVEN_ODDS = 0.5f;
constexpr float HALF_HEALTH = 0.5f;
// Level gaps beyond this weigh like this gap in the strength (keeps the power finite)
constexpr int MAX_STRENGTH_LEVEL_GAP = 20;
// The shortest chase, as a share of the configured one
constexpr float MIN_CHASE_SCALE = 0.25f;

// The values of the rules: the configured ones in the units the rules use, and the shape weights the plan marks [E]
// (header defaults; WP3b tunes them in its harness)
struct FightRuleValues
{
    // Meetings and chances
    float scanYards = 40.0f;
    uint32_t rollAgainMs = 10 * MS_PER_MINUTE;
    uint32_t minGapMs = 30 * MS_PER_MINUTE;
    float aggressionOffBelow = -0.5f;
    float hoursAtZero = 4.0f;
    float hoursAtOne = 1.0f;
    float chanceCap = 1.0f / 3.0f;
    float greenFactor = 0.5f;
    float yellowFactor = 1.0f;
    float orangeDaring = 1.0f;
    float orangeCareful = 0.0f;
    bool courtesy = true;
    float grudgeFactor = 3.0f;
    // No camping and calm areas
    uint32_t resurrectedMs = 5 * MS_PER_MINUTE;
    float graveyardYards = 80.0f;
    float calmYards = 200.0f;
    uint32_t calmMs = 15 * MS_PER_MINUTE;
    // Joining, retreating and ending
    float joinYards = 60.0f;
    float retreatHealth = 0.3f;
    float retreatOdds = 0.5f;
    uint32_t chaseMs = 15 * MS_PER_SECOND;
    float chaseYards = 40.0f;
    uint32_t noHitMs = 30 * MS_PER_SECOND;
    uint32_t stalemateMs = 5 * MS_PER_MINUTE;
    // Death
    float ghostEnemyYards = 40.0f;
    uint32_t ghostRecheckMs = 15 * MS_PER_SECOND;
    uint32_t ghostSpiritHealerMs = 5 * MS_PER_MINUTE;
    uint32_t avoidMinMs = 30 * MS_PER_MINUTE;
    uint32_t avoidMaxMs = 60 * MS_PER_MINUTE;
    // Grudges
    uint32_t maxGrudges = 5;
    float grudgeHalfLifeDays = 3.0f;

    // Shape weights [E]
    // Strength of one player to the viewer: health x levelStep^(level - viewer level)
    float levelStep = 1.1f;
    // The odds share a bot needs for the full chance: 0.5 + oddsCaution x caution; the chance falls to 0 oddsRamp
    // below it (the most careful bot never starts at even odds, the most daring one starts outnumbered)
    float oddsCaution = 0.12f;
    float oddsRamp = 0.12f;
    // Join chance: joinBase x (1 + joinAggression x aggression) x (1 + joinHelpfulness x helpfulness when its side
    // is losing or a player of its faction is the target) x the caution at bad odds x (1 - joinGoal x goal value)
    float joinBase = 0.4f;
    float joinAggression = 0.75f;
    float joinHelpfulness = 0.75f;
    float joinGoal = 0.75f;
    // Retreat lines: retreatHealth x (1 + retreatCautionHealth x caution), retreatOdds + retreatCautionOdds x caution
    float retreatCautionHealth = 0.5f;
    float retreatCautionOdds = 0.1f;
    // Chase scale: 1 + chaseAggression x aggression - chaseCourtesy x courtesy (at least MIN_CHASE_SCALE)
    float chaseAggression = 0.5f;
    float chaseCourtesy = 0.5f;
    // Grudge strength: 1 when the killer started the fight, grudgeNotStarter otherwise, x (1 - grudgePatience x
    // patience); a faded grudge below grudgeForgetBelow is gone
    float grudgeNotStarter = 0.5f;
    float grudgePatience = 0.5f;
    float grudgeForgetBelow = 0.1f;

    static FightRuleValues From(WorldPvpConfig const& config)
    {
        FightRuleValues values;
        values.scanYards = static_cast<float>(config.ScanYards);
        values.rollAgainMs = config.RollAgainSeconds * MS_PER_SECOND;
        values.minGapMs = config.MinGapMinutes * MS_PER_MINUTE;
        values.aggressionOffBelow = config.AggressionOffBelow;
        values.hoursAtZero = config.HoursAtZero;
        values.hoursAtOne = config.HoursAtOne;
        values.chanceCap = std::clamp(config.ChanceCap, 0.0f, 1.0f);
        values.greenFactor = config.GreenFactor;
        values.yellowFactor = config.YellowFactor;
        values.orangeDaring = config.OrangeDaring;
        values.orangeCareful = config.OrangeCareful;
        values.courtesy = config.Courtesy;
        values.grudgeFactor = std::max(config.GrudgeFactor, 1.0f);
        values.resurrectedMs = config.ResurrectedSeconds * MS_PER_SECOND;
        values.graveyardYards = static_cast<float>(config.GraveyardYards);
        values.calmYards = static_cast<float>(config.CalmYards);
        values.calmMs = config.CalmMinutes * MS_PER_MINUTE;
        values.joinYards = static_cast<float>(config.JoinYards);
        values.retreatHealth = static_cast<float>(config.RetreatHealthPercent) / 100.0f;
        values.retreatOdds = config.RetreatOdds;
        values.chaseMs = config.ChaseSeconds * MS_PER_SECOND;
        values.chaseYards = static_cast<float>(config.ChaseYards);
        values.noHitMs = config.NoHitSeconds * MS_PER_SECOND;
        values.stalemateMs = config.StalemateSeconds * MS_PER_SECOND;
        values.ghostEnemyYards = static_cast<float>(config.GhostEnemyYards);
        values.ghostRecheckMs = std::max<uint32_t>(config.GhostRecheckSeconds, 1) * MS_PER_SECOND;
        values.ghostSpiritHealerMs = config.GhostSpiritHealerSeconds * MS_PER_SECOND;
        values.avoidMinMs = config.AvoidMinMinutes * MS_PER_MINUTE;
        values.avoidMaxMs = std::max(config.AvoidMaxMinutes, config.AvoidMinMinutes) * MS_PER_MINUTE;
        values.maxGrudges = config.MaxGrudges;
        values.grudgeHalfLifeDays = static_cast<float>(config.GrudgeHalfLifeDays);
        return values;
    }
};

// A trait (-1..+1) as 0 for the lowest and 1 for the highest
inline float TraitShare(float trait) { return (std::clamp(trait, -1.0f, 1.0f) + 1.0f) * 0.5f; }

// --- Colours ---

enum class Colour : uint8_t
{
    Grey,
    Green,
    Yellow,
    Orange,
    Red
};

constexpr std::array<char const*, 5> COLOUR_WORDS = {"grey", "green", "yellow", "orange", "red"};

constexpr char const* Word(Colour value) { return COLOUR_WORDS[static_cast<size_t>(value)]; }

// The core's grey level (Acore::XP::GetGrayLevel, Formulas.h)
constexpr int GreyLevel(int level)
{
    if (level <= 5)
        return 0;
    if (level <= 39)
        return level - 5 - level / 10;
    if (level <= 59)
        return level - 1 - level / 5;
    return level - 9;
}

// The colour of a target of `targetLevel` to a player of `level`, as the core's Acore::XP::GetColorCode gives it
constexpr Colour ColourOf(uint32_t level, uint32_t targetLevel)
{
    int const own = static_cast<int>(level);
    int const other = static_cast<int>(targetLevel);
    if (other >= own + 5)
        return Colour::Red;
    if (other >= own + 3)
        return Colour::Orange;
    if (other >= own - 2)
        return Colour::Yellow;
    if (other > GreyLevel(own))
        return Colour::Green;
    return Colour::Grey;
}

// Why a bot picks a target: starting a fight, joining one by chance, defending itself, or its group's (or real
// master's) fight. Starting never picks grey or red (DP.6, WP.3); joining may pick grey but not red; defence and the
// group's fight pick anything
enum class Pick : uint8_t
{
    Start,
    Join,
    Defence,
    Group
};

constexpr bool MayPick(Colour colour, Pick pick)
{
    if (pick == Pick::Start)
        return colour != Colour::Grey && colour != Colour::Red;
    if (pick == Pick::Join)
        return colour != Colour::Red;
    return true;
}

// --- Strength and odds ---

// One player's visible strength to a viewer: its health share, weighted by its level against the viewer's
inline float Strength(uint32_t viewerLevel, uint32_t level, float health, FightRuleValues const& values)
{
    int const gap = std::clamp(static_cast<int>(level) - static_cast<int>(viewerLevel), -MAX_STRENGTH_LEVEL_GAP,
                               MAX_STRENGTH_LEVEL_GAP);
    return std::clamp(health, 0.0f, 1.0f) * std::pow(values.levelStep, static_cast<float>(gap));
}

// A side's share of the visible strength (0..1; even when nothing is seen)
inline float OddsShare(float own, float enemy)
{
    float const total = own + enemy;
    return total > 0.0f ? own / total : EVEN_ODDS;
}

// --- Starting a fight ---

// Why a meeting does not start a fight (the decision row's word): the bot-side rules first (also the gate of the
// enemy-player scan), then the target's, then the roll; the join rules add theirs
enum class Refusal : uint8_t
{
    None,
    NotOpenWorld,
    RealPlayer,
    NotLeader,
    InFight,
    Graveyard,
    Peaceable,
    Gap,
    Calm,
    NotTarget,
    Grey,
    Red,
    Resurrected,
    RolledRecently,
    Roll,
    Dead,
    Unseen,
    TooFar,
    NotOwnSide,
    AllRed
};

constexpr std::array<char const*, 20> REFUSAL_WORDS = {
    "none", "not_open_world", "real_player", "not_leader", "in_fight",     "graveyard",   "peaceable",
    "gap",  "calm",           "not_target",  "grey",       "red",          "resurrected", "rolled_recently",
    "roll", "dead",           "unseen",      "too_far",    "not_own_side", "all_red"};

static_assert(static_cast<size_t>(Refusal::AllRed) + 1 == REFUSAL_WORDS.size(), "REFUSAL_WORDS must match Refusal");

constexpr char const* Word(Refusal value) { return REFUSAL_WORDS[static_cast<size_t>(value)]; }

// The bot's side of a meeting
struct StartBot
{
    // A persistent bot in the open world (no dungeon, battleground or arena), outside PvpProhibitedZoneIds
    bool openWorld = true;
    // In a group with a real player, or with a real master: it joins their fights and never starts one
    bool realPlayer = false;
    // A member of a bot group that is not its leader: the leader decides
    bool groupMember = false;
    bool inFight = false;
    // Inside a capture point's area and there for the objective (3.7)
    bool atObjective = false;
    // Inside a calm area (InAnyCalmArea over the map's ended fights)
    bool inCalmArea = false;
    // Play time since its last fight ended (NEVER_MS: it has not fought) and since its last chance (since login for
    // the first)
    uint32_t sinceFightEndMs = NEVER_MS;
    uint32_t sinceChanceMs = 0;
    // To the nearest graveyard or spirit healer
    float graveyardYards = FAR_YARDS;
    float aggression = 0.0f;
    float caution = 0.0f;
    float courtesy = 0.0f;
};

// The enemy met
struct StartTarget
{
    // An attackable, PvP-flagged enemy player the bot can see
    bool enemy = true;
    Colour colour = Colour::Yellow;
    uint32_t sinceResurrectionMs = NEVER_MS;
    float graveyardYards = FAR_YARDS;
    // It contests the capture point the bot is there for
    bool contestsObjective = false;
    // Since this bot last rolled for it (RecentEnemy::rolledMs)
    uint32_t sinceRolledMs = NEVER_MS;
    // Fighting a monster, and its health share
    bool busy = false;
    float health = 1.0f;
    // The bot's side's share of the strength seen within ScanYards (OddsShare), and the bot's grudge against it
    float odds = EVEN_ODDS;
    float grudge = 0.0f;
};

struct StartDecision
{
    Refusal refusal = Refusal::None;
    // The meeting passed every hard rule and was rolled for: the caller restarts the bot's chance clock and marks the
    // target as rolled for, whether it started or not
    bool chance = false;
    bool start = false;
    float p = 0.0f;
    FightReason reason = FightReason::Budget;
};

// Starts per hour of play at this aggression (-1..+1)
inline float StartRate(float aggression, FightRuleValues const& values)
{
    float const atZero = values.hoursAtZero > 0.0f ? 1.0f / values.hoursAtZero : 0.0f;
    float const atOne = values.hoursAtOne > 0.0f ? 1.0f / values.hoursAtOne : atZero;
    float const a = std::clamp(aggression, -1.0f, 1.0f);
    if (a <= values.aggressionOffBelow || atZero <= 0.0f)
        return 0.0f;
    if (a < 0.0f)
        return atZero * (a - values.aggressionOffBelow) / -values.aggressionOffBelow;
    return atZero * std::pow(atOne / atZero, a);
}

// The base probability of a chance after `sinceChanceMs` of play at `rate` per hour, capped at the chance cap
inline float StartProbability(float rate, uint32_t sinceChanceMs, FightRuleValues const& values)
{
    float const hours = static_cast<float>(sinceChanceMs) / MS_PER_HOUR;
    return std::min(values.chanceCap, static_cast<float>(-std::expm1(-rate * hours)));
}

inline float ColourFactor(Colour colour, float caution, FightRuleValues const& values)
{
    switch (colour)
    {
        case Colour::Green:
            return values.greenFactor;
        case Colour::Yellow:
            return values.yellowFactor;
        case Colour::Orange:
            return values.orangeDaring + (values.orangeCareful - values.orangeDaring) * TraitShare(caution);
        default:
            return 0.0f;
    }
}

// A target fighting a monster or below half health counts fully for the most selfish bot, not at all for the most
// courteous one
inline float CourtesyFactor(bool busy, float health, float courtesy, FightRuleValues const& values)
{
    if (!values.courtesy || (!busy && health >= HALF_HEALTH))
        return 1.0f;
    return 1.0f - TraitShare(courtesy);
}

// 1 when the bot's side's share of the visible strength is what its caution needs, falling to 0 oddsRamp below
inline float OddsFactor(float odds, float caution, FightRuleValues const& values)
{
    float const need = EVEN_ODDS + values.oddsCaution * std::clamp(caution, -1.0f, 1.0f);
    if (values.oddsRamp <= 0.0f)
        return odds >= need ? 1.0f : 0.0f;
    return std::clamp((odds - need) / values.oddsRamp + 1.0f, 0.0f, 1.0f);
}

inline float GrudgeChanceFactor(float strength, FightRuleValues const& values)
{
    return 1.0f + (values.grudgeFactor - 1.0f) * std::max(strength, 0.0f);
}

// The bot-side rules. At an objective (3.7) the budget's rules (peaceable, gap, calm area) do not apply; the
// graveyard rule still does
inline Refusal BotRule(StartBot const& bot, bool objective, FightRuleValues const& values)
{
    if (!bot.openWorld)
        return Refusal::NotOpenWorld;
    if (bot.realPlayer)
        return Refusal::RealPlayer;
    if (bot.groupMember)
        return Refusal::NotLeader;
    if (bot.inFight)
        return Refusal::InFight;
    if (bot.graveyardYards <= values.graveyardYards)
        return Refusal::Graveyard;
    if (objective)
        return Refusal::None;
    if (StartRate(bot.aggression, values) <= 0.0f)
        return Refusal::Peaceable;
    if (bot.sinceFightEndMs < values.minGapMs)
        return Refusal::Gap;
    if (bot.inCalmArea)
        return Refusal::Calm;
    return Refusal::None;
}

// The gate of the enemy-player scan: a bot that cannot start now skips it (attackers and fights still reach it)
inline Refusal StartGate(StartBot const& bot, FightRuleValues const& values)
{
    return BotRule(bot, bot.atObjective, values);
}

// The target-side hard rules, which hold at objectives too: colour, camping, graveyard
inline Refusal TargetRule(StartTarget const& target, FightRuleValues const& values)
{
    if (!target.enemy)
        return Refusal::NotTarget;
    if (target.colour == Colour::Grey)
        return Refusal::Grey;
    if (target.colour == Colour::Red)
        return Refusal::Red;
    if (target.sinceResurrectionMs < values.resurrectedMs)
        return Refusal::Resurrected;
    if (target.graveyardYards <= values.graveyardYards)
        return Refusal::Graveyard;
    return Refusal::None;
}

// Whether a meeting starts a fight; `roll` is uniform in [0, 1)
inline StartDecision DecideStart(StartBot const& bot, StartTarget const& target, float roll,
                                 FightRuleValues const& values)
{
    StartDecision decision;
    bool const objective = bot.atObjective && target.contestsObjective;
    decision.refusal = BotRule(bot, objective, values);
    if (decision.refusal == Refusal::None)
        decision.refusal = TargetRule(target, values);
    if (decision.refusal != Refusal::None)
        return decision;

    // A bot there for the objective fights who contests it: no budget, no chance, no roll
    if (objective)
    {
        decision.start = true;
        decision.p = 1.0f;
        decision.reason = FightReason::Objective;
        return decision;
    }

    if (target.sinceRolledMs < values.rollAgainMs)
    {
        decision.refusal = Refusal::RolledRecently;
        return decision;
    }

    decision.chance = true;
    float const base = StartProbability(StartRate(bot.aggression, values), bot.sinceChanceMs, values);
    float const soft = ColourFactor(target.colour, bot.caution, values) *
                       CourtesyFactor(target.busy, target.health, bot.courtesy, values) *
                       OddsFactor(target.odds, bot.caution, values) * GrudgeChanceFactor(target.grudge, values);
    decision.p = std::clamp(base * soft, 0.0f, 1.0f);
    decision.start = roll < decision.p;
    decision.refusal = decision.start ? Refusal::None : Refusal::Roll;
    decision.reason = target.grudge > 0.0f ? FightReason::Grudge : FightReason::Budget;
    return decision;
}

// --- Joining a fight ---

// Why a bot joins: it was attacked, its group's or real master's fight, or by chance
enum class JoinWhy : uint8_t
{
    None,
    Defence,
    Group,
    Master,
    Chance
};

// A bystander and the fight near it; the bot decides once per fight and the caller keeps the answer (WP.7)
struct JoinInput
{
    bool alive = true;
    // A participant attacked this bot: it defends itself (no budget)
    bool attacked = false;
    bool groupFight = false;
    bool masterFight = false;
    bool inOtherFight = false;
    bool visible = true;
    // Its faction is on one side of the fight
    bool ownSide = true;
    float yards = 0.0f;
    // The enemies in the fight it may pick by chance (MayPick(colour, Pick::Join)), pre-counted
    uint32_t pickable = 1;
    // A player of its faction is the target of an enemy in the fight
    bool factionTargeted = false;
    // Its side's share of the fight's visible strength
    float odds = EVEN_ODDS;
    // The value of its current goal: 0 idle or grinding, 1 the most valuable (WT14; neutral until WP7b)
    float goalValue = 0.0f;
    float aggression = 0.0f;
    float helpfulness = 0.0f;
    float caution = 0.0f;
};

struct JoinDecision
{
    Refusal refusal = Refusal::None;
    JoinWhy why = JoinWhy::None;
    bool join = false;
    float p = 0.0f;
};

// The chance of a bystander to join by chance
inline float JoinChance(JoinInput const& input, FightRuleValues const& values)
{
    float p = values.joinBase * (1.0f + values.joinAggression * std::clamp(input.aggression, -1.0f, 1.0f));
    if (input.odds < EVEN_ODDS || input.factionTargeted)
        p *= 1.0f + values.joinHelpfulness * std::clamp(input.helpfulness, -1.0f, 1.0f);
    // Bad odds keep the careful bot out, the daring one not at all
    float const badness = std::clamp((EVEN_ODDS - input.odds) / EVEN_ODDS, 0.0f, 1.0f);
    p *= 1.0f - badness * TraitShare(input.caution);
    p *= 1.0f - values.joinGoal * std::clamp(input.goalValue, 0.0f, 1.0f);
    return std::clamp(p, 0.0f, 1.0f);
}

// Whether a bystander joins; `roll` is uniform in [0, 1)
inline JoinDecision DecideJoin(JoinInput const& input, float roll, FightRuleValues const& values)
{
    JoinDecision decision;
    if (!input.alive)
        decision.refusal = Refusal::Dead;
    else if (input.attacked)
        decision.why = JoinWhy::Defence;
    else if (input.groupFight)
        decision.why = JoinWhy::Group;
    else if (input.masterFight)
        decision.why = JoinWhy::Master;
    else if (input.inOtherFight)
        decision.refusal = Refusal::InFight;
    else if (!input.visible)
        decision.refusal = Refusal::Unseen;
    else if (input.yards > values.joinYards)
        decision.refusal = Refusal::TooFar;
    else if (!input.ownSide)
        decision.refusal = Refusal::NotOwnSide;
    else if (input.pickable == 0)
        decision.refusal = Refusal::AllRed;

    if (decision.why != JoinWhy::None)
    {
        decision.join = true;
        decision.p = 1.0f;
        return decision;
    }

    if (decision.refusal != Refusal::None)
        return decision;

    decision.p = JoinChance(input, values);
    decision.join = roll < decision.p;
    decision.why = decision.join ? JoinWhy::Chance : JoinWhy::None;
    decision.refusal = decision.join ? Refusal::None : Refusal::Roll;
    return decision;
}

// --- Ending a fight ---

// A participant retreats when its health is low and its side is losing (its share of the visible strength of the
// alive participants, so deaths count through the numbers); careful bots retreat earlier
inline bool ShouldRetreat(float health, float odds, float caution, FightRuleValues const& values)
{
    float const c = std::clamp(caution, -1.0f, 1.0f);
    float const healthLine = values.retreatHealth * (1.0f + values.retreatCautionHealth * c);
    float const oddsLine = values.retreatOdds + values.retreatCautionOdds * c;
    return health < healthLine && odds < oddsLine;
}

struct ChaseLimit
{
    uint32_t ms = 0;
    float yards = 0.0f;
};

// How long and how far beyond the fight a winner chases a retreating enemy: aggression lengthens it, courtesy
// shortens it
inline ChaseLimit ChaseLimitFor(float aggression, float courtesy, FightRuleValues const& values)
{
    float const scale = std::max(MIN_CHASE_SCALE, 1.0f + values.chaseAggression * std::clamp(aggression, -1.0f, 1.0f) -
                                                      values.chaseCourtesy * std::clamp(courtesy, -1.0f, 1.0f));
    ChaseLimit limit;
    limit.ms = static_cast<uint32_t>(static_cast<float>(values.chaseMs) * scale);
    limit.yards = values.chaseYards * scale;
    return limit;
}

inline bool ChaseOver(uint32_t chaseMs, float yardsBeyond, ChaseLimit const& limit)
{
    return chaseMs >= limit.ms || yardsBeyond >= limit.yards;
}

// A fight's clock: its age, the time since the last hit between the sides, and the participants per side still in
// it (alive, not retreated, not gone)
struct FightClock
{
    uint32_t ageMs = 0;
    uint32_t sinceHitMs = 0;
    std::array<uint32_t, SIDE_COUNT> active{};
};

struct FightEndCheck
{
    bool ends = false;
    EndCause cause = EndCause::CombatOver;
};

// Whether a fight ends now by the rules: one side out (wiped), no hit between the sides for NoHitSeconds (timeout),
// or older than StalemateSeconds (stalemate). When both timeouts are due the one reached first is the cause. The
// book's end grace and its safety cap still apply on their own
inline FightEndCheck CheckFightEnd(FightClock const& clock, FightRuleValues const& values)
{
    FightEndCheck check;
    if (clock.active[0] == 0 || clock.active[1] == 0)
    {
        check.ends = true;
        check.cause = EndCause::Wiped;
        return check;
    }

    bool const noHit = clock.sinceHitMs >= values.noHitMs;
    bool const stalemate = clock.ageMs >= values.stalemateMs;
    if (!noHit && !stalemate)
        return check;

    check.ends = true;
    if (noHit && stalemate)
    {
        uint64_t const lastHitAgeMs = clock.ageMs - std::min(clock.sinceHitMs, clock.ageMs);
        check.cause = lastHitAgeMs + values.noHitMs < values.stalemateMs ? EndCause::Timeout : EndCause::Stalemate;
    }
    else
        check.cause = noHit ? EndCause::Timeout : EndCause::Stalemate;
    return check;
}

// Whether (x, y) lies in the calm area of an ended fight: within CalmYards of its centre for CalmMinutes after its end
inline bool InCalmArea(EndedFight const& ended, float x, float y, uint32_t nowMs, FightRuleValues const& values)
{
    if (nowMs - ended.endMs >= values.calmMs)
        return false;
    float const dx = x - ended.x;
    float const dy = y - ended.y;
    return dx * dx + dy * dy <= values.calmYards * values.calmYards;
}

// Over a map's ring of ended fights (closing order, at most MAX_ENDED_FIGHTS): newest first, stopping at the first
// expired one. Bounded but not O(1): the caller keeps the answer for a while rather than asking every tick
template <typename EndedRing>
inline bool InAnyCalmArea(EndedRing const& ring, float x, float y, uint32_t nowMs, FightRuleValues const& values)
{
    for (auto it = ring.rbegin(); it != ring.rend(); ++it)
    {
        if (nowMs - it->endMs >= values.calmMs)
            return false;
        if (InCalmArea(*it, x, y, nowMs, values))
            return true;
    }
    return false;
}

// --- Death ---

enum class GhostStep : uint8_t
{
    Wait,
    Corpse,
    SpiritHealer
};

struct GhostPlan
{
    GhostStep step = GhostStep::Corpse;
    // While waiting: when to check again
    uint32_t recheckMs = 0;
};

// A ghost waits while an enemy player is within GhostEnemyYards of its corpse, runs to it once the enemy has gone,
// and takes the spirit healer after GhostSpiritHealerSeconds of waiting, as a player would
inline GhostPlan PlanGhost(float enemyYards, uint32_t ghostMs, FightRuleValues const& values)
{
    GhostPlan plan;
    if (enemyYards > values.ghostEnemyYards)
        return plan;
    if (ghostMs >= values.ghostSpiritHealerMs)
    {
        plan.step = GhostStep::SpiritHealer;
        return plan;
    }
    plan.step = GhostStep::Wait;
    plan.recheckMs = std::min(values.ghostRecheckMs, values.ghostSpiritHealerMs - ghostMs);
    return plan;
}

// How long the planner avoids the place of a lost fight (died or retreated): AvoidMinMinutes for the most daring bot
// to AvoidMaxMinutes for the most careful
inline uint32_t AvoidMs(float caution, FightRuleValues const& values)
{
    float const span = static_cast<float>(values.avoidMaxMs - std::min(values.avoidMinMs, values.avoidMaxMs));
    return values.avoidMinMs + static_cast<uint32_t>(span * TraitShare(caution));
}

// --- Grudges (storage and table: WP9) ---

// A new grudge against the player who killed the bot: 1 when that player started the fight, less otherwise;
// hot-tempered bots (low patience) hold stronger ones. 1 multiplies the start chance by GrudgeFactor
inline float GrudgeStrength(bool killerStarted, float patience, FightRuleValues const& values)
{
    float const base = killerStarted ? 1.0f : values.grudgeNotStarter;
    return std::max(0.0f, base * (1.0f - values.grudgePatience * std::clamp(patience, -1.0f, 1.0f)));
}

// A grudge's strength after `elapsedSeconds` of real time: it halves every GrudgeHalfLifeDays
inline float GrudgeFaded(float strength, uint32_t elapsedSeconds, FightRuleValues const& values)
{
    if (values.grudgeHalfLifeDays <= 0.0f)
        return 0.0f;
    float const days = static_cast<float>(elapsedSeconds) / SECONDS_PER_DAY;
    return strength * std::exp2(-days / values.grudgeHalfLifeDays);
}

inline bool GrudgeForgotten(float strength, FightRuleValues const& values)
{
    return strength < values.grudgeForgetBelow;
}

// The slot a new grudge of `strength` takes among `count` held ones: a free slot below MaxGrudges, else the weakest
// when the new one is stronger, else -1 (the strongest are kept)
template <size_t N>
inline int32_t GrudgeSlot(std::array<float, N> const& strengths, uint32_t count, float strength,
                          FightRuleValues const& values)
{
    uint32_t const limit = std::min<uint32_t>(values.maxGrudges, static_cast<uint32_t>(N));
    if (count < limit)
        return static_cast<int32_t>(count);
    if (limit == 0)
        return -1;

    uint32_t weakest = 0;
    for (uint32_t index = 1; index < limit; ++index)
        if (strengths[index] < strengths[weakest])
            weakest = index;
    return strength > strengths[weakest] ? static_cast<int32_t>(weakest) : -1;
}
}  // namespace WorldPvp

#endif
