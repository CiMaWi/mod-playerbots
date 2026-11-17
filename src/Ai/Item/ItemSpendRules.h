/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMSPENDRULES_H
#define _PLAYERBOT_ITEMSPENDRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "GoalModel.h"
#include "ItemSituation.h"
#include "ItemUseKinds.h"
#include "TraitRules.h"

// Item use plan WI13, PLAN 3.6 and INTERFACE 1.10: the spend rule. A consumed item is spent when what it saves is
// worth more than replacing it, both in copper. Pure and std only (the planner's GoalModel.h is std only too), any
// thread, no clock and no global state: the same input gives the same verdict.
//
// The danger of a fight (Danger) is the plan's literal shape: how long the bot lives at the rate it loses health,
// against how long its enemies live. Both are in seconds, the ratio r = fight seconds / life seconds; the danger is
// r^2 / (1 + r^2): 0 with no enemy or no loss, 0.5 when the bot's life and the fight are equally long, 1 when no
// health is left. The fight's length sums the enemies' kill times (a normal enemy of the bot's level takes
// KILL_SECONDS alone, scaled by its health, rank and level, shared by the group); the loss rate is the measured one
// over the last seconds, or before the first hit (none measured) the expected one of the enemies on the bot, plus
// WAITING_SHARE of each enemy of the fight that does not hit it now (held, fleeing, still coming, or on another
// member of the group, shared by the group): such an enemy hits later, so a fight with a held enemy is not without
// danger (WI16). The loss is held for the whole fight: it overstates a fight of several enemies a little, which errs
// toward spending.
//
// The verdict, in this order: control -> keep (Control); won -> keep (Won); a free ready item for the same need ->
// keep (FreeReady); a potion after this fight's potion -> keep (PotionKept); no danger -> keep (NoDanger); then
// saves = the death chance removed x deathCopper (daring lowers it) + minutesSaved x the earning rate + at a stake
// (a player, an objective) the fight's loss, against cost = replaceCopper (thrift raises it). saves > cost spends,
// its reason the largest term (Death, Time, StakeLost; on a tie the stake, then the death); else keep (NotWorth).
// The death chance removed is the danger times the kind's share (DEATH_SHARE; an area item's share times the
// enemies it covers), never more than the danger. The item's rank is not read: what a rank means against this
// bot's health is the situation's business, and WI14's offer already takes the stronger ready item of a kind (3.5).
//
// Traits (INTERFACE Q3, settled here): thrift = TraitRules' Thrift unit, daring = minus its Caution unit (the trait
// the planner already reads as "daring" when low); both -1..1, turned into factors by the planner's Factor().
//
// Every number below is a tuning value [E], one declaration each (CS.16), no conf option; the nights read them.
namespace ItemUse
{
// Danger
constexpr float KILL_SECONDS = 12.0f;    // a bot kills a normal enemy of its level alone, from full health
constexpr float KILLED_SECONDS = 40.0f;  // such an enemy alone takes the bot from full health to dead
constexpr float WAITING_SHARE = 0.5f;    // WI16: an enemy of the fight not hitting the bot now, against one that is
constexpr float LEVEL_STEP = 1.08f;      // an enemy's health and damage per level above the bot (grey ones below)
constexpr int LEVEL_SPAN = 30;           // level differences beyond this count as this
// An enemy's health and damage against a normal one (Normal, Elite, Rare, RareElite, Boss, Player)
constexpr std::array<float, 6> RANK_STRENGTH = {1.0f, 2.2f, 1.5f, 2.6f, 6.0f, 2.0f};

// What a kind saves: the share of the death chance its use removes (0: it saves no death, only time if any)
constexpr std::array<float, static_cast<std::size_t>(Kind::Count)> DEATH_SHARE = {
    0.0f,   // unclassified
    0.8f,   // heal: the health that was missing
    0.5f,   // mana: the spells that keep the bot alive
    0.3f,   // damage_one: the enemy dies sooner
    0.25f,  // damage_area: per enemy covered
    0.5f,   // control: an enemy stops hitting
    0.4f,   // move: the bot gets away
    0.6f,   // protect: the damage is absorbed or avoided
    0.4f,   // dispel: the harmful effect ends
    0.3f,   // boost: the fight turns faster
    0.0f,   // long_buff
    0.0f,   // food_drink
    0.0f,   // coating
    0.3f,   // summon: a helper or decoy takes hits
    0.0f,   // resurrect
    0.0f,   // transport
    0.0f,   // upgrade
    0.0f,   // learn
    0.0f,   // open
    0.0f,   // quest
    0.0f,   // mount
    0.0f};  // toy

constexpr float STAKE_EXTRA_PLAYER = 1.0f;     // a fight lost to a player costs a second death's worth (ground, honour)
constexpr float STAKE_EXTRA_OBJECTIVE = 1.0f;  // a lost objective costs a second death's worth (the retry)
constexpr float THRIFT_SPAN = 0.5f;   // a fully thrifty bot weighs the price 1.5 times, a spendthrift 0.5 times
constexpr float DARING_SPAN = 0.25f;  // a fully daring bot holds a death risk 0.75 times, a careful one 1.25 times
// WI18: the minutes a control item saves on an enemy that flees (the chase it spares; the offer's SpendCaseFor): a
// net (400 c) is worth it at the level-80 earning rate (1000 c a minute), not at the low bands' rates
constexpr float RUNNER_MINUTES = 0.5f;

// Words: death time stake | won free_ready not_worth potion_kept no_danger control
enum class SpendReason : std::uint8_t
{
    Death,
    Time,
    StakeLost,  // spend
    Won,
    FreeReady,
    NotWorth,
    PotionKept,
    NoDanger,
    Control,
    Count  // keep
};

constexpr std::size_t SPEND_REASON_COUNT = static_cast<std::size_t>(SpendReason::Count);
constexpr std::array<std::string_view, SPEND_REASON_COUNT> SPEND_REASON_WORDS = {
    "death", "time", "stake", "won", "free_ready", "not_worth", "potion_kept", "no_danger", "control"};
static_assert(static_cast<std::size_t>(SpendReason::Control) + 1 == SPEND_REASON_COUNT, "a word per reason");

constexpr std::string_view SpendReasonWord(SpendReason reason)
{
    std::size_t const index = static_cast<std::size_t>(reason);
    return index < SPEND_REASON_COUNT ? SPEND_REASON_WORDS[index] : std::string_view("not_worth");
}

struct SpendCase
{
    Kind kind{Kind::Unclassified};
    std::int32_t rank{0};
    std::uint32_t replaceCopper{0};  // 0 with price None: what the item is worth to keep is passed here instead
    bool potion{false};
    bool potionDrunk{false};
    bool freeReadySameNeed{false};  // a free ready item answers the same need
    float danger{0.0f};             // Danger(situation) in a fight; the buff picker's "ahead" danger before one
    float minutesSaved{0.0f};       // a fight that ends sooner, or a buff's share of what is ahead
    std::uint8_t hits{1};           // enemies an area item covers
    Stake stake{Stake::Ordinary};
    bool won{false};               // the fight is already won
    std::uint32_t deathCopper{0};  // the corpse run at the bot's earning rate plus the repair bill
    std::uint32_t earnCopperPerMinute{0};
    float thrift{0.0f};
    float daring{0.0f};
    bool control{false};
};

struct SpendVerdict
{
    bool spend{false};
    SpendReason reason{SpendReason::NotWorth};
    float savesCopper{0.0f};
    float costCopper{0.0f};
    float danger{0.0f};
};

namespace SpendDetail
{
inline float Fraction(float pct, float fallback)  // a percent as 0..1; not finite: the fallback
{
    return std::isfinite(pct) ? std::clamp(pct, 0.0f, 100.0f) / 100.0f : fallback;
}

inline float Unit(float value)  // a trait unit in -1..1; not finite: 0
{
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

inline float Strength(NearUnit const& unit, std::uint8_t botLevel)
{
    std::size_t const rank = std::min(static_cast<std::size_t>(unit.rank), RANK_STRENGTH.size() - 1);
    int const levels = std::clamp(static_cast<int>(unit.level) - static_cast<int>(botLevel), -LEVEL_SPAN, LEVEL_SPAN);
    return RANK_STRENGTH[rank] * std::pow(LEVEL_STEP, static_cast<float>(levels));
}
}  // namespace SpendDetail

// 0 (no danger) .. 1 (the bot dies), pure; see the header comment for its shape
inline float Danger(Situation const& situation)
{
    if (!situation.inFight || situation.fightEnded)
        return 0.0f;

    float const health = SpendDetail::Fraction(situation.healthPct, 1.0f);
    if (health <= 0.0f)
        return 1.0f;

    float fightSeconds = 0.0f;
    float attack = 0.0f;   // the strength of the enemies hitting the bot
    float waiting = 0.0f;  // the strength of the fight's other enemies (held, fleeing, coming, on someone else)
    std::size_t const count = std::min<std::size_t>(situation.unitCount, MAX_NEAR);
    for (std::size_t i = 0; i < count; ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (unit.friendly || !unit.inFight)
            continue;

        float const left = SpendDetail::Fraction(unit.healthPct, 1.0f);
        if (left <= 0.0f)
            continue;

        float const strength = SpendDetail::Strength(unit, situation.level);
        fightSeconds += KILL_SECONDS * left * strength;
        if (unit.onBot && !unit.controlled && !unit.flees)
            attack += strength;
        else
            waiting += strength;
    }
    float const group = static_cast<float>(std::max<std::uint8_t>(situation.groupSize, 1));
    fightSeconds /= group;
    if (fightSeconds <= 0.0f)
        return 0.0f;

    // The loss per second as a share of the bot's health: measured, else expected from the enemies (header comment)
    float loss = 0.0f;
    float const measured = situation.healthLossPerSecond;
    if (situation.maxHealth > 0 && !std::isnan(measured) && measured > 0.0f)
        loss = std::min(measured / static_cast<float>(situation.maxHealth), 1.0f);
    else
        loss = (attack + WAITING_SHARE * waiting / group) / KILLED_SECONDS;
    if (loss <= 0.0f)
        return 0.0f;

    float const ratio = std::min(fightSeconds * loss / health, 1000.0f);  // fight seconds over life seconds
    float const square = ratio * ratio;
    return std::clamp(square / (1.0f + square), 0.0f, 1.0f);
}

// What a death costs: the corpse run at the bot's earning rate plus the repair bill
inline std::uint32_t DeathCopper(std::uint32_t earnCopperPerMinute, std::uint32_t repairCopper)
{
    double const copper = static_cast<double>(Planner::Goals::DEATH_MINUTES) * earnCopperPerMinute + repairCopper;
    return static_cast<std::uint32_t>(std::min(copper, 4294967295.0));
}

inline float DeathShare(Kind kind)
{
    std::size_t const index = static_cast<std::size_t>(kind);
    return index < DEATH_SHARE.size() ? DEATH_SHARE[index] : 0.0f;
}

inline float SpendThrift(TraitRules::BotTraits const& traits)
{
    return TraitRules::Unit(traits, TraitRules::Trait::Thrift);
}

inline float SpendDaring(TraitRules::BotTraits const& traits)
{
    return -TraitRules::Unit(traits, TraitRules::Trait::Caution);
}

// The verdict with its reason and value, pure
inline SpendVerdict Spend(SpendCase const& spendCase)
{
    SpendVerdict verdict;
    float const danger = std::isfinite(spendCase.danger) ? std::clamp(spendCase.danger, 0.0f, 1.0f) : 0.0f;
    verdict.danger = danger;
    verdict.costCopper = static_cast<float>(spendCase.replaceCopper) *
                         Planner::Goals::Factor(SpendDetail::Unit(spendCase.thrift), THRIFT_SPAN);

    SpendReason gate = SpendReason::Count;
    if (spendCase.control)
        gate = SpendReason::Control;
    else if (spendCase.won)
        gate = SpendReason::Won;
    else if (spendCase.freeReadySameNeed)
        gate = SpendReason::FreeReady;
    else if (spendCase.potion && spendCase.potionDrunk)
        gate = SpendReason::PotionKept;
    else if (danger <= 0.0f)
        gate = SpendReason::NoDanger;
    if (gate != SpendReason::Count)
    {
        verdict.reason = gate;
        return verdict;
    }

    float share = DeathShare(spendCase.kind);
    if (spendCase.kind == Kind::DamageArea)
        share *= static_cast<float>(spendCase.hits);
    float const chance = std::min(danger * share, danger);

    float const death = chance * static_cast<float>(spendCase.deathCopper) *
                        Planner::Goals::Factor(-SpendDetail::Unit(spendCase.daring), DARING_SPAN);
    float const minutes = std::isfinite(spendCase.minutesSaved) ? std::max(spendCase.minutesSaved, 0.0f) : 0.0f;
    float const time = minutes * static_cast<float>(spendCase.earnCopperPerMinute);
    float extra = 0.0f;
    if (spendCase.stake == Stake::Player)
        extra = STAKE_EXTRA_PLAYER;
    else if (spendCase.stake == Stake::Objective)
        extra = STAKE_EXTRA_OBJECTIVE;
    float const stake = chance * static_cast<float>(spendCase.deathCopper) * extra;

    verdict.savesCopper = death + time + stake;
    if (!(verdict.savesCopper > verdict.costCopper))
    {
        verdict.reason = SpendReason::NotWorth;
        return verdict;
    }

    verdict.spend = true;
    if (stake > 0.0f && stake >= death && stake >= time)
        verdict.reason = SpendReason::StakeLost;
    else if (death >= time)
        verdict.reason = SpendReason::Death;
    else
        verdict.reason = SpendReason::Time;
    return verdict;
}

// One potion per fight: the potion need where it saves most over its cost, among those the rule spends; the list's
// size when none. A mana potion that saves less does not take the turn of the healing potion that saves more.
inline std::size_t BestPotionNeed(std::span<SpendCase const> needs)
{
    std::size_t best = needs.size();
    float bestGain = 0.0f;
    for (std::size_t i = 0; i < needs.size(); ++i)
    {
        if (!needs[i].potion)
            continue;

        SpendVerdict const verdict = Spend(needs[i]);
        float const gain = verdict.savesCopper - verdict.costCopper;
        if (verdict.spend && (best == needs.size() || gain > bestGain))
        {
            best = i;
            bestGain = gain;
        }
    }
    return best;
}

// The item_spend row's decision word: spend:<kind> or keep:<kind>
inline std::string SpendDecision(Kind kind, SpendVerdict const& verdict)
{
    return std::string(verdict.spend ? "spend:" : "keep:") + std::string(KindWord(kind));
}
}  // namespace ItemUse

#endif
