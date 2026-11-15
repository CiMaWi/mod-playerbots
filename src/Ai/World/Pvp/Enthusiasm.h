/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ENTHUSIASM_H
#define _PLAYERBOT_ENTHUSIASM_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "WorldPvpConfig.h"

// The enthusiasm of world PvP (WP22): one value per bot on a scale of 0..max that recovers while the bot is not
// engaged and drains while it is, a few one-off edits, the join and flee thresholds it is compared with, the local
// balance and the bans. Pure functions of what the caller passes (traits as -1..+1, colours, flags, times in ms);
// no Player, no globals, no allocation, O(1) except LocalBalance (O(n) over the caller's list). The modifier table
// lists every number by row. Header-only so tests/enthusiasm-selftest.cpp checks the same code the server runs.
namespace WorldPvp
{
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

// --- Values ---

constexpr double RATE_MINUTE_MS = 60.0 * 1000.0;
constexpr double RATE_HOUR_MS = 60.0 * RATE_MINUTE_MS;
constexpr float EVEN_BALANCE = 0.5f;

// Enthusiasm's numbers (WorldPvpEnthusiasmConfig, made by FromConfig; the config's initialisers are the defaults).
// Edits and threshold terms are sizes in points; the functions give them their signs
struct EnthusiasmValues
{
    float max = 0.0f;
    float recoveryPerHour = 0.0f;
    float depletionPerMinute = 0.0f;
    float deathEdit = 0.0f;
    float deathEditConfident = 0.0f;
    float deathEditUnconfident = 0.0f;
    float killEdit = 0.0f;
    float groupDeathShare = 0.0f;
    float groupDeathYards = 0.0f;
    float joinBase = 0.0f;
    float joinAggression = 0.0f;
    float joinAggressionFactor = 1.0f;
    float joinGreen = 0.0f;
    float joinFactionAttacked = 0.0f;
    float joinFactionHelpfulness = 0.0f;
    float joinGroupAttacked = 0.0f;
    float joinGraveyard = 0.0f;
    float graveyardYards = 0.0f;
    float joinBalanceMax = 0.0f;
    float fleeBase = 0.0f;
    float fleeCaution = 0.0f;
    float fleeGroupHold = 0.0f;
    float groupHoldYards = 0.0f;
    float balanceYards = 0.0f;
    // A player's weight in the local balance by its level colour to the bot (index: Colour)
    std::array<float, 5> weights = {};
    float depletionFactorLead = 1.0f;
    float depletionFactorBehind = 1.0f;
    float balanceLead = 1.0f;
    float balanceBehind = 0.0f;
};

inline EnthusiasmValues FromConfig(WorldPvpConfig const& config)
{
    WorldPvpEnthusiasmConfig const& zeal = config.Enthusiasm;
    EnthusiasmValues values;
    values.max = std::max(zeal.Max, 1.0f);
    values.recoveryPerHour = std::max(zeal.RecoveryPerHour, 0.0f);
    values.depletionPerMinute = std::max(zeal.DepletionPerMinute, 0.0f);
    values.deathEdit = zeal.DeathEdit;
    values.deathEditConfident = zeal.DeathEditConfident;
    values.deathEditUnconfident = zeal.DeathEditUnconfident;
    values.killEdit = zeal.KillEdit;
    values.groupDeathShare = std::clamp(zeal.GroupDeathShare, 0.0f, 1.0f);
    values.groupDeathYards = static_cast<float>(zeal.GroupDeathYards);
    values.joinBase = zeal.JoinBase;
    values.joinAggression = zeal.JoinAggression;
    values.joinAggressionFactor = std::clamp(zeal.AggressionJoinFactor, 0.0f, 1.0f);
    values.joinGreen = zeal.JoinGreen;
    values.joinFactionAttacked = zeal.JoinFactionAttacked;
    values.joinFactionHelpfulness = zeal.JoinFactionHelpfulness;
    values.joinGroupAttacked = zeal.JoinGroupAttacked;
    values.joinGraveyard = zeal.JoinGraveyard;
    values.graveyardYards = static_cast<float>(config.GraveyardYards);
    values.joinBalanceMax = zeal.JoinBalanceMax;
    values.fleeBase = zeal.FleeBase;
    values.fleeCaution = zeal.FleeCaution;
    values.fleeGroupHold = zeal.FleeGroupHold;
    values.groupHoldYards = static_cast<float>(zeal.GroupHoldYards);
    values.balanceYards = static_cast<float>(zeal.BalanceYards);
    values.weights = {zeal.WeightGrey, zeal.WeightGreen, zeal.WeightYellow, zeal.WeightOrange, zeal.WeightRed};
    values.depletionFactorLead = std::clamp(zeal.DepletionFactorLead, 0.01f, 1.0f);
    values.depletionFactorBehind = std::max(zeal.DepletionFactorBehind, 1.0f);
    // The ends lie on either side of even, so the lean between them is defined
    values.balanceLead = std::clamp(zeal.BalanceLead, 0.51f, 1.0f);
    values.balanceBehind = std::clamp(zeal.BalanceBehind, 0.0f, 0.49f);
    return values;
}

// --- The value ---

// A bot's enthusiasm: the value at its last change and that change's time. A read applies recovery or depletion for
// the time since; entering or leaving the engaged state and every edit settle it first. Not saved: a login starts it
// with StartValue
struct Enthusiasm
{
    double value = 0.0;
    uint32_t atMs = 0;
};

// A uniform random draw (0..UINT32_MAX, the caller's) as a value across the scale, so bots are not in step
inline double StartValue(uint32_t random, EnthusiasmValues const& values)
{
    return static_cast<double>(random) / static_cast<double>(std::numeric_limits<uint32_t>::max()) * values.max;
}

inline double RecoveryPerMs(EnthusiasmValues const& values) { return values.recoveryPerHour / RATE_HOUR_MS; }

// The depletion while engaged, times the local balance's factor (BalanceDepletionFactor)
inline double DepletionPerMs(float balanceFactor, EnthusiasmValues const& values)
{
    return values.depletionPerMinute * std::max(balanceFactor, 0.0f) / RATE_MINUTE_MS;
}

// The value at `nowMs`: one straight segment from the last change, up while not engaged, down while engaged, held
// within the scale. The segment is monotone, so reading once equals settling at every step on the way. It has one
// rate: the caller settles before `engaged` or `balanceFactor` changes (the scan's new balance included), or the new
// rate would count for the whole time since the last change. Unsigned subtraction counts across the wrap of the ms
// clock
inline double Read(Enthusiasm const& state, uint32_t nowMs, bool engaged, float balanceFactor,
                   EnthusiasmValues const& values)
{
    double const elapsed = static_cast<double>(nowMs - state.atMs);
    double const value = engaged ? state.value - DepletionPerMs(balanceFactor, values) * elapsed
                                 : state.value + RecoveryPerMs(values) * elapsed;
    return std::clamp(value, 0.0, static_cast<double>(values.max));
}

inline Enthusiasm Settle(Enthusiasm const& state, uint32_t nowMs, bool engaged, float balanceFactor,
                         EnthusiasmValues const& values)
{
    return {Read(state, nowMs, engaged, balanceFactor, values), nowMs};
}

// The longest an engaged bot without kills takes from the scale's top to 0: at the slowest depletion, its side
// clearly leading
inline uint32_t EngagedBoundMs(EnthusiasmValues const& values)
{
    double const slowest =
        DepletionPerMs(std::min({values.depletionFactorLead, 1.0f, values.depletionFactorBehind}), values);
    if (slowest <= 0.0)
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::min(std::ceil(values.max / slowest), 4.0e9));
}

// --- One-off edits ---

enum class EditCause : uint8_t
{
    None,
    Death,       // the bot's own PvP death
    Kill,        // the bot killed an enemy player
    GroupDeath,  // a group member's PvP death within groupDeathYards (the caller measures)
};

constexpr std::array<char const*, 4> EDIT_CAUSE_WORDS = {"none", "death", "kill", "group_death"};

constexpr char const* Word(EditCause value) { return EDIT_CAUSE_WORDS[static_cast<size_t>(value)]; }

// The death edit's size: deathEdit at confidence 0, linear to deathEditConfident at +1 and deathEditUnconfident at -1
inline float DeathEditSize(float confidence, EnthusiasmValues const& values)
{
    float const c = std::clamp(confidence, -1.0f, 1.0f);
    float const end = c >= 0.0f ? values.deathEditConfident : values.deathEditUnconfident;
    return values.deathEdit + (end - values.deathEdit) * std::fabs(c);
}

// The signed change an edit makes (before the scale's limits)
inline float Edit(EditCause cause, float confidence, EnthusiasmValues const& values)
{
    switch (cause)
    {
        case EditCause::Death:
            return -DeathEditSize(confidence, values);
        case EditCause::Kill:
            return values.killEdit;
        case EditCause::GroupDeath:
            return -values.groupDeathShare * DeathEditSize(confidence, values);
        default:
            return 0.0f;
    }
}

// Settles the value at `nowMs`, then applies the edit within the scale
inline Enthusiasm Apply(Enthusiasm const& state, uint32_t nowMs, bool engaged, float balanceFactor, EditCause cause,
                        float confidence, EnthusiasmValues const& values)
{
    Enthusiasm settled = Settle(state, nowMs, engaged, balanceFactor, values);
    settled.value = std::clamp(settled.value + Edit(cause, confidence, values), 0.0, static_cast<double>(values.max));
    return settled;
}

// --- The local balance ---

// One living player within balanceYards of the bot (the bot included): its side, its level colour to the bot
// (ColourOf) and its health share 0..1
struct BalanceEntry
{
    bool ownSide = false;
    Colour colour = Colour::Yellow;
    float healthShare = 1.0f;
};

// The bot's side's share of the weighted total, 0.5 when even or when nobody counts
inline float LocalBalance(std::span<BalanceEntry const> entries, EnthusiasmValues const& values)
{
    double own = 0.0;
    double other = 0.0;
    for (BalanceEntry const& entry : entries)
    {
        double const weight = static_cast<double>(values.weights[static_cast<size_t>(entry.colour)]) *
                              std::clamp(entry.healthShare, 0.0f, 1.0f);
        (entry.ownSide ? own : other) += weight;
    }
    double const total = own + other;
    return total > 0.0 ? static_cast<float>(own / total) : EVEN_BALANCE;
}

// The balance as a lean: -1 when the bot's side clearly leads (balance >= balanceLead), 0 when even, +1 when it is
// clearly behind (balance <= balanceBehind), linear between even and each end
inline float BalanceLean(float balance, EnthusiasmValues const& values)
{
    if (balance >= EVEN_BALANCE)
        return -std::min(1.0f, (balance - EVEN_BALANCE) / (values.balanceLead - EVEN_BALANCE));
    return std::min(1.0f, (EVEN_BALANCE - balance) / (EVEN_BALANCE - values.balanceBehind));
}

// The depletion's factor: 1 when even, depletionFactorLead when clearly leading, depletionFactorBehind when clearly
// behind, linear in the lean
inline float BalanceDepletionFactor(float balance, EnthusiasmValues const& values)
{
    float const lean = BalanceLean(balance, values);
    float const end = lean >= 0.0f ? values.depletionFactorBehind : values.depletionFactorLead;
    return 1.0f + (end - 1.0f) * std::fabs(lean);
}

// The join threshold's shift: -joinBalanceMax when clearly leading, 0 when even, +joinBalanceMax when clearly behind
inline float BalanceJoinShift(float balance, EnthusiasmValues const& values)
{
    return values.joinBalanceMax * BalanceLean(balance, values);
}

// --- Entering a fight ---

// What the bot sees of a target it could attack
struct JoinFacts
{
    float aggression = 0.0f;
    float helpfulness = 0.0f;
    Colour colour = Colour::Yellow;     // the target's level colour to the bot (ColourOf)
    bool targetAttacksFaction = false;  // attacks a player of the bot's faction, its group's members included
    bool targetAttacksGroup = false;    // attacks a member of the bot's group
    bool nearGraveyard = false;         // the bot is within graveyardYards of a graveyard or spirit healer
    float balance = EVEN_BALANCE;       // LocalBalance, cached at the scan's cadence
};

// Whether the target already fights the bot's side: it attacks a player of the bot's faction or a group member
inline bool FightOngoing(JoinFacts const& facts) { return facts.targetAttacksFaction || facts.targetAttacksGroup; }

// The threshold before the other rows: joinBase - joinAggression x aggression. When the fight is already ongoing the
// Aggression term counts at joinAggressionFactor of its size
inline float JoinBaseThreshold(float aggression, bool ongoing, EnthusiasmValues const& values)
{
    float const term = values.joinAggression * (ongoing ? values.joinAggressionFactor : 1.0f);
    return values.joinBase - term * std::clamp(aggression, -1.0f, 1.0f);
}

// The sum of the join rows, not held to the scale (above max: the bot never joins). Starting a fight: Aggression
// alone putting the threshold at the scale's top makes the most peaceable bot one that never starts, and no other row
// lowers it then. Joining one (FightOngoing): the weakened Aggression term lets that bot join, above all its group
inline float JoinThreshold(JoinFacts const& facts, EnthusiasmValues const& values)
{
    bool const ongoing = FightOngoing(facts);
    float const base = JoinBaseThreshold(facts.aggression, ongoing, values);
    if (!ongoing && base >= values.max)
        return base;
    float threshold = base;
    if (facts.colour == Colour::Green)
        threshold += values.joinGreen;
    if (facts.targetAttacksFaction)
    {
        // Up to joinFactionHelpfulness more, from none for the least helpful bot to all of it for the most helpful
        float const helpful = (std::clamp(facts.helpfulness, -1.0f, 1.0f) + 1.0f) * 0.5f;
        threshold -= values.joinFactionAttacked + values.joinFactionHelpfulness * helpful;
    }
    if (facts.targetAttacksGroup)
        threshold -= values.joinGroupAttacked;
    if (facts.nearGraveyard)
        threshold += values.joinGraveyard;
    return threshold + BalanceJoinShift(facts.balance, values);
}

// The lowest join threshold a bot of these traits could have, joining or starting: below it the bot skips the
// enemy-player scan
inline float LowestJoinThreshold(float aggression, float helpfulness, EnthusiasmValues const& values)
{
    JoinFacts facts;
    facts.aggression = aggression;
    facts.helpfulness = helpfulness;
    facts.balance = 1.0f;
    float const starting = JoinThreshold(facts, values);
    facts.targetAttacksFaction = true;
    facts.targetAttacksGroup = true;
    return std::min(starting, JoinThreshold(facts, values));
}

inline bool MayJoin(double value, float joinThreshold) { return value > joinThreshold; }

// --- Leaving a fight ---

struct FleeFacts
{
    float caution = 0.0f;
    bool groupMemberEngaged = false;  // a living member of the bot's group within groupHoldYards is engaged
};

// fleeBase + fleeCaution x caution, fleeGroupHold lower while a group member is engaged; below 0 the bot never flees
inline float FleeThreshold(FleeFacts const& facts, EnthusiasmValues const& values)
{
    float const hold = facts.groupMemberEngaged ? values.fleeGroupHold : 0.0f;
    return values.fleeBase + values.fleeCaution * std::clamp(facts.caution, -1.0f, 1.0f) - hold;
}

inline bool MustLeave(double value, float fleeThreshold) { return value < fleeThreshold; }

// --- Bans ---

enum class BanReason : uint8_t
{
    None,
    Red,             // the target is red to the bot
    Grey,            // grey, and it attacks no player of the bot's faction
    ProhibitedZone,  // PvpProhibitedZoneIds
    Transport,       // the bot is on a transport
    Instance,        // a dungeon, battleground or arena
    RealMaster,      // the bot has a real master and the target is not in its master's fight
};

constexpr std::array<char const*, 7> BAN_WORDS = {"none",      "red",      "grey",       "prohibited_zone",
                                                  "transport", "instance", "real_master"};

constexpr char const* Word(BanReason value) { return BAN_WORDS[static_cast<size_t>(value)]; }

struct BanFacts
{
    Colour colour = Colour::Yellow;  // the target's level colour to the bot
    bool targetAttacksFaction = false;
    bool prohibitedZone = false;
    bool onTransport = false;
    bool inInstance = false;
    bool realMaster = false;
    bool mastersFight = false;  // the target fights the bot's real master
};

// The first ban on choosing this target, or None
inline BanReason Banned(BanFacts const& facts)
{
    if (facts.colour == Colour::Red)
        return BanReason::Red;
    if (facts.colour == Colour::Grey && !facts.targetAttacksFaction)
        return BanReason::Grey;
    if (facts.prohibitedZone)
        return BanReason::ProhibitedZone;
    if (facts.onTransport)
        return BanReason::Transport;
    if (facts.inInstance)
        return BanReason::Instance;
    if (facts.realMaster && !facts.mastersFight)
        return BanReason::RealMaster;
    return BanReason::None;
}

// --- The modifier table ---

enum class ModifierCategory : uint8_t
{
    Recovery,
    Depletion,
    JoinThreshold,
    FleeThreshold,
    OneOffEdit,
    Never
};

constexpr std::array<char const*, 6> MODIFIER_CATEGORY_WORDS = {"recovery",       "depletion", "join_threshold",
                                                                "flee_threshold", "edit",      "never"};

constexpr char const* Word(ModifierCategory value) { return MODIFIER_CATEGORY_WORDS[static_cast<size_t>(value)]; }

// Rate: points per hour (recovery) or per minute (depletion); Shift: points added to a threshold; Factor: multiplies
// the depletion; Edit: points added to the value once; Ban: the target is never chosen
enum class ModifierForm : uint8_t
{
    Rate,
    Shift,
    Factor,
    Edit,
    Ban
};

constexpr std::array<char const*, 5> MODIFIER_FORM_WORDS = {"rate", "shift", "factor", "edit", "ban"};

constexpr char const* Word(ModifierForm value) { return MODIFIER_FORM_WORDS[static_cast<size_t>(value)]; }

// The rows, in table order
enum class Modifier : uint8_t
{
    RecoveryRate,
    DepletionRate,
    DepletionBehind,
    DepletionLead,
    JoinBase,
    JoinAggression,
    JoinAggressionOngoing,
    JoinGreen,
    JoinFaction,
    JoinHelpfulness,
    JoinGroup,
    JoinGraveyard,
    JoinBehind,
    JoinLead,
    FleeBase,
    FleeCaution,
    FleeGroupEngaged,
    Death,
    DeathConfident,
    DeathUnconfident,
    Kill,
    GroupDeath,
    BanRed,
    BanGrey,
    BanZone,
    BanTransport,
    BanInstance,
    BanRealMaster,
    Count
};

constexpr size_t MODIFIER_COUNT = static_cast<size_t>(Modifier::Count);

// A row's value is signed as it applies when the row is on: a trait row at the trait's +1, a balance row at its end
struct ModifierRow
{
    char const* name;
    ModifierCategory category;
    ModifierForm form;
    float value;
};

// Every row with its number, made from the values the functions above read (one source: the config), so the table
// stays true for a tuned server; tests/enthusiasm-selftest.cpp proves each row against the functions
inline std::array<ModifierRow, MODIFIER_COUNT> ModifierTable(EnthusiasmValues const& v)
{
    using C = ModifierCategory;
    using F = ModifierForm;
    return {{
        {"base rate, points per hour", C::Recovery, F::Rate, v.recoveryPerHour},
        {"base rate while engaged, points per minute", C::Depletion, F::Rate, v.depletionPerMinute},
        {"local balance, clearly behind", C::Depletion, F::Factor, v.depletionFactorBehind},
        {"local balance, clearly leading", C::Depletion, F::Factor, v.depletionFactorLead},
        {"base", C::JoinThreshold, F::Shift, v.joinBase},
        {"Aggression, per +1", C::JoinThreshold, F::Shift, -v.joinAggression},
        {"Aggression, per +1, target already attacks the bot's faction or group", C::JoinThreshold, F::Shift,
         -v.joinAggression * v.joinAggressionFactor},
        {"green target", C::JoinThreshold, F::Shift, v.joinGreen},
        {"target attacks a player of the bot's faction", C::JoinThreshold, F::Shift, -v.joinFactionAttacked},
        {"the same, most helpful bot (Helpfulness)", C::JoinThreshold, F::Shift, -v.joinFactionHelpfulness},
        {"target attacks a group member", C::JoinThreshold, F::Shift, -v.joinGroupAttacked},
        {"near a graveyard or spirit healer", C::JoinThreshold, F::Shift, v.joinGraveyard},
        {"local balance, clearly behind", C::JoinThreshold, F::Shift, v.joinBalanceMax},
        {"local balance, clearly leading", C::JoinThreshold, F::Shift, -v.joinBalanceMax},
        {"base", C::FleeThreshold, F::Shift, v.fleeBase},
        {"Caution, per +1", C::FleeThreshold, F::Shift, v.fleeCaution},
        {"a group member still engaged", C::FleeThreshold, F::Shift, -v.fleeGroupHold},
        {"PvP death", C::OneOffEdit, F::Edit, -v.deathEdit},
        {"PvP death, most confident bot", C::OneOffEdit, F::Edit, -v.deathEditConfident},
        {"PvP death, least confident bot", C::OneOffEdit, F::Edit, -v.deathEditUnconfident},
        {"kill of an enemy player", C::OneOffEdit, F::Edit, v.killEdit},
        {"group member's PvP death", C::OneOffEdit, F::Edit, -v.groupDeathShare * v.deathEdit},
        {"red as chosen target", C::Never, F::Ban, 0.0f},
        {"grey unless it attacks a player of the bot's faction", C::Never, F::Ban, 0.0f},
        {"prohibited zone", C::Never, F::Ban, 0.0f},
        {"on a transport", C::Never, F::Ban, 0.0f},
        {"in a dungeon, battleground or arena", C::Never, F::Ban, 0.0f},
        {"with a real master, outside its fight", C::Never, F::Ban, 0.0f},
    }};
}
}  // namespace WorldPvp

#endif
