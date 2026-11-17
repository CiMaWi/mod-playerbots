/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMBUFFRULES_H
#define _PLAYERBOT_ITEMBUFFRULES_H

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ItemSituation.h"
#include "ItemSpendRules.h"
#include "ItemUseKinds.h"
#include "ItemUseTypes.h"

// Item use plan WI8a, PLAN 3.6 and INTERFACE 1.12: the buff picker. Before something worth it lies ahead (a dungeon,
// an elite, a group, a battleground) a bot drinks a flask or two elixirs (one battle, one guardian, never both with a
// flask), eats one buff food and reads one scroll, each the best it holds for its role. Pure and std only, any
// thread, no clock and no global state: the same input gives the same picks.
//
// What excludes what (Excludes, SlotTaken) follows the engine as WI8a-1's probe found it: the engine refuses
// nothing, the LAST applied buff of an exclusive spell group wins (a flask removes both elixirs, an elixir removes
// the flask, a weaker scroll removes a stronger one). So the picker does the refusing: two records exclude each other
// when their exclusion words do (flask with flask and both elixirs, an elixir with one of its own kind, one well-fed,
// one scroll) or when they share a spell group (the record's groups list its parents too: battle 1, guardian 2, the
// flask both, well-fed 1001, every scroll 1087). A buff is never used into a group a long aura on the bot already
// holds; only a scroll may replace a held aura of its group, and only one strictly weaker (the group's stronger
// wins, as for a class buff of the same stat).
//
// The score of a buff for the role is the sum of its aura effects' amounts per stat times the bot's weight of that
// stat (BuffStats, BuffScore). The weights are a span indexed by BuffStat; a span shorter than BUFF_STAT_COUNT means
// the role's fixed table (RoleWeights). A buff that scores 0 is never picked. Per slot the best-scoring usable held
// entry is the candidate (a tie: the cheaper, then the earlier); a candidate is used when the spend rule spends it.
// The flask is the pick when what it brings is at least what the two elixirs bring together; what a candidate brings
// is its score when the spend rule spends it, else 0 (when neither spends, the raw scores decide which option's rows
// are written). The losing option yields no pick, so even the skip rows never show both.
//
// Worth: a SpendCase of kind LongBuff with the Ahead's danger (AHEAD_DANGER, a gate only: LongBuff saves no death)
// and minutesSaved = AHEAD_SHARE x the minutes ahead the buff lasts (min of the minutes ahead and its duration).
//
// Every number below is a tuning value [E], one declaration each (CS.16), no conf option; the nights read them.
namespace ItemUse
{
// What is ahead of the bot. Words: none dungeon elite group bg
enum class Ahead : std::uint8_t
{
    None,
    Dungeon,
    Elite,
    Group,
    Battleground,
    Count
};

constexpr std::size_t AHEAD_COUNT = static_cast<std::size_t>(Ahead::Count);
constexpr std::array<std::string_view, AHEAD_COUNT> AHEAD_WORDS = {"none", "dungeon", "elite", "group", "bg"};

constexpr std::string_view AheadWord(Ahead ahead)
{
    std::size_t const index = static_cast<std::size_t>(ahead);
    return index < AHEAD_COUNT ? AHEAD_WORDS[index] : std::string_view("none");
}

// The Ahead of its word; false when the word is none of them
inline bool AheadOfWord(std::string_view word, Ahead& ahead)
{
    for (std::size_t i = 0; i < AHEAD_COUNT; ++i)
        if (AHEAD_WORDS[i] == word)
        {
            ahead = static_cast<Ahead>(i);
            return true;
        }
    return false;
}

// Per Ahead (none dungeon elite group bg): the danger handed to the spend rule (a gate), the share of the time ahead a
// buff saves (faster kills, fewer deaths and wipes) and the minutes ahead when the caller knows none
constexpr std::array<float, AHEAD_COUNT> AHEAD_DANGER = {0.0f, 0.5f, 0.6f, 0.3f, 0.5f};
constexpr std::array<float, AHEAD_COUNT> AHEAD_SHARE = {0.0f, 0.3f, 0.5f, 0.2f, 0.3f};
constexpr std::array<float, AHEAD_COUNT> AHEAD_MINUTES = {0.0f, 45.0f, 5.0f, 30.0f, 20.0f};
// A buff food's record lasts as long as the eating (30 s); its Well Fed aura is a triggered spell whose duration the
// record does not carry. Every Well Fed aura of the game's buff foods at the cap lasts an hour.
constexpr float WELL_FED_MINUTES = 60.0f;
constexpr std::uint8_t SPELL_SPECIFIC_SCROLL = 24;  // the game's SpellSpecificType of a scroll

// The stats a buff's auras raise, the index of the weights. Words: see BUFF_STAT_WORDS
enum class BuffStat : std::uint8_t
{
    Strength,
    Agility,
    Stamina,
    Intellect,
    Spirit,
    AttackPower,
    RangedAttackPower,
    SpellPower,
    HealPower,
    ManaRegen,  // mana per 5 s
    Health,
    HealthRegen,
    Armor,
    Resistance,
    Defense,
    Dodge,
    Parry,
    Block,  // rating and block value
    Hit,
    Crit,
    Haste,
    Expertise,
    ArmorPenetration,
    SpellPenetration,
    Resilience,
    Count
};

constexpr std::size_t BUFF_STAT_COUNT = static_cast<std::size_t>(BuffStat::Count);
constexpr std::array<std::string_view, BUFF_STAT_COUNT> BUFF_STAT_WORDS = {
    "str", "agi",    "sta",   "int",       "spi",    "ap",       "rap",       "sp",    "heal",
    "mp5", "health", "hp5",   "armor",     "resist", "defense",  "dodge",     "parry", "block",
    "hit", "crit",   "haste", "expertise", "arp",    "spellpen", "resilience"};

using BuffWeights = std::array<float, BUFF_STAT_COUNT>;

// The fixed weights per role (Tank Healer Melee Ranged Caster), per point of a stat, about what a point is worth
// against one attack power (physical roles) or one spell power (casters). The shape (a weight per stat and role) is
// cmangos' UseConsumableAction::GetStatWeight (WI.11); the values are ours. A role has no use for a stat at 0.
//                                  str   agi   sta   int   spi   ap    rap   sp    heal  mp5   hp     hp5   armor
//                                  res   def   dodge parry block hit   crit  haste exp   arp   spen  resil
constexpr std::array<BuffWeights, 5> ROLE_WEIGHTS = {{
    {0.6f, 0.8f, 1.5f, 0.0f, 0.0f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.15f, 0.5f, 0.1f,
     0.3f, 1.5f, 1.2f, 1.1f, 0.7f, 0.6f, 0.2f, 0.3f, 1.0f, 0.0f, 0.0f,  0.0f},  // tank
    {0.0f, 0.0f, 0.1f, 0.9f, 0.6f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 0.01f, 0.0f, 0.0f,
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.9f, 0.0f, 0.0f, 0.0f,  0.0f},  // healer
    {1.6f, 1.6f, 0.1f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.01f, 0.0f, 0.0f,
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.8f, 1.5f, 1.3f, 1.6f, 1.3f, 0.0f,  0.0f},  // melee
    {0.0f, 1.6f, 0.1f, 0.2f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.01f, 0.0f, 0.0f,
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.8f, 1.4f, 1.2f, 0.0f, 1.2f, 0.0f,  0.0f},  // ranged
    {0.0f, 0.0f, 0.1f, 0.4f, 0.3f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.01f, 0.0f, 0.0f,
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.7f, 0.8f, 0.0f, 0.0f, 0.3f,  0.0f},  // caster
}};

inline std::span<float const> RoleWeights(Role role)
{
    std::size_t const index = std::min<std::size_t>(static_cast<std::size_t>(role), ROLE_WEIGHTS.size() - 1);
    return ROLE_WEIGHTS[index];
}

struct BuffBot
{
    Role role{Role::Melee};
    std::uint8_t level{1};
    Ahead ahead{Ahead::None};
    float aheadMinutes{0.0f};                   // 0: AHEAD_MINUTES of the Ahead
    std::span<std::uint32_t const> auraGroups;  // spell groups of the long auras on the bot, with their amounts
    std::span<std::int32_t const> auraAmounts;  // per group: the largest amount of the aura that holds it
    std::span<float const> statWeights;         // indexed by BuffStat; shorter than BUFF_STAT_COUNT: RoleWeights
    float thrift{0.0f};
    float daring{0.0f};
    std::uint32_t deathCopper{0};
    std::uint32_t earnCopperPerMinute{0};
    bool control{false};
};

// Why a pick is used or skipped, the item_buff row's reason. Words: ahead_dungeon ahead_elite ahead_group ahead_bg |
// nothing_ahead excluded not_worth coated control
enum class BuffReason : std::uint8_t
{
    AheadDungeon,
    AheadElite,
    AheadGroup,
    AheadBg,  // use
    NothingAhead,
    Excluded,
    NotWorth,
    Coated,
    Control,
    Count  // skip
};

constexpr std::size_t BUFF_REASON_COUNT = static_cast<std::size_t>(BuffReason::Count);
constexpr std::array<std::string_view, BUFF_REASON_COUNT> BUFF_REASON_WORDS = {
    "ahead_dungeon", "ahead_elite", "ahead_group", "ahead_bg", "nothing_ahead",
    "excluded",      "not_worth",   "coated",      "control"};

constexpr std::string_view BuffReasonWord(BuffReason reason)
{
    std::size_t const index = static_cast<std::size_t>(reason);
    return index < BUFF_REASON_COUNT ? BUFF_REASON_WORDS[index] : std::string_view("not_worth");
}

struct BuffPick
{
    std::uint8_t usable{0};  // index into the list
    Exclude slot{Exclude::None};
    SpendVerdict verdict;
    // WI8a: appended to INTERFACE 1.12's shape
    bool use{false};  // false: the row is skip:<slot> with the reason
    BuffReason reason{BuffReason::NotWorth};
    float score{0.0f};  // the role's score of the pick, the row's value
};

// The slots in the order the picks come out
constexpr std::array<Exclude, 5> BUFF_SLOTS = {Exclude::Flask, Exclude::BattleElixir, Exclude::GuardianElixir,
                                               Exclude::WellFed, Exclude::Scroll};

inline int BuffSlotIndex(Exclude exclude)
{
    for (std::size_t i = 0; i < BUFF_SLOTS.size(); ++i)
        if (BUFF_SLOTS[i] == exclude)
            return static_cast<int>(i);
    return -1;
}

inline bool IsBuffSlot(Exclude exclude) { return BuffSlotIndex(exclude) >= 0; }

namespace BuffDetail
{
// The game's AuraType ids the score reads
constexpr std::uint16_t AURA_MOD_DAMAGE_DONE = 13;
constexpr std::uint16_t AURA_MOD_RESISTANCE = 22;
constexpr std::uint16_t AURA_MOD_STAT = 29;
constexpr std::uint16_t AURA_MOD_INCREASE_HEALTH = 34;
constexpr std::uint16_t AURA_MOD_POWER_REGEN = 85;
constexpr std::uint16_t AURA_MOD_ATTACK_POWER = 99;
constexpr std::uint16_t AURA_MOD_TARGET_RESISTANCE = 123;
constexpr std::uint16_t AURA_MOD_RANGED_ATTACK_POWER = 124;
constexpr std::uint16_t AURA_MOD_HEALING_DONE = 135;
constexpr std::uint16_t AURA_MOD_RESISTANCE_EXCLUSIVE = 143;
constexpr std::uint16_t AURA_MOD_SHIELD_BLOCKVALUE = 158;
constexpr std::uint16_t AURA_MOD_HEALTH_REGEN_IN_COMBAT = 161;
constexpr std::uint16_t AURA_MOD_RATING = 189;
constexpr std::uint32_t MAGIC_SCHOOLS = 126;  // holy fire nature frost shadow arcane
constexpr std::int32_t POWER_MANA = 0;

// The game's CombatRating bits of AURA_MOD_RATING's mask, per stat
constexpr std::array<std::pair<std::uint32_t, BuffStat>, 10> RATING_BITS = {{
    {1u << 1, BuffStat::Defense},
    {1u << 2, BuffStat::Dodge},
    {1u << 3, BuffStat::Parry},
    {1u << 4, BuffStat::Block},
    {(1u << 5) | (1u << 6) | (1u << 7), BuffStat::Hit},
    {(1u << 8) | (1u << 9) | (1u << 10), BuffStat::Crit},
    {(1u << 14) | (1u << 15) | (1u << 16), BuffStat::Resilience},
    {(1u << 17) | (1u << 18) | (1u << 19), BuffStat::Haste},
    {1u << 23, BuffStat::Expertise},
    {1u << 24, BuffStat::ArmorPenetration},
}};

inline float SchoolShare(std::int32_t misc)  // the share of the six magic schools a mask covers
{
    return static_cast<float>(std::popcount(static_cast<std::uint32_t>(misc) & MAGIC_SCHOOLS)) / 6.0f;
}

inline void Add(BuffWeights& stats, BuffStat stat, float amount) { stats[static_cast<std::size_t>(stat)] += amount; }
}  // namespace BuffDetail

// The stat amounts of a record's aura effects (its own and those of the spells it triggers); auras the score does
// not know (a dummy, a periodic trigger, the eating's regeneration, a creature-type bonus) add nothing
inline BuffWeights BuffStats(Record const& record)
{
    using namespace BuffDetail;
    BuffWeights stats{};
    std::size_t const count = std::min<std::size_t>(record.effectCount, MAX_EFFECTS);
    for (std::size_t i = 0; i < count; ++i)
    {
        EffectFact const& effect = record.effects[i];
        float const amount = static_cast<float>(effect.amount);
        switch (effect.aura)
        {
            case AURA_MOD_STAT:
                if (effect.misc < 0)
                    for (BuffStat stat : {BuffStat::Strength, BuffStat::Agility, BuffStat::Stamina, BuffStat::Intellect,
                                          BuffStat::Spirit})
                        Add(stats, stat, amount);
                else if (effect.misc <= 4)
                    Add(stats, static_cast<BuffStat>(effect.misc), amount);
                break;
            case AURA_MOD_ATTACK_POWER:
                Add(stats, BuffStat::AttackPower, amount);
                break;
            case AURA_MOD_RANGED_ATTACK_POWER:
                Add(stats, BuffStat::RangedAttackPower, amount);
                break;
            case AURA_MOD_DAMAGE_DONE:
                Add(stats, BuffStat::SpellPower, amount * SchoolShare(effect.misc));
                break;
            case AURA_MOD_HEALING_DONE:
                Add(stats, BuffStat::HealPower, amount);
                break;
            case AURA_MOD_POWER_REGEN:
                if (effect.misc == POWER_MANA)
                    Add(stats, BuffStat::ManaRegen, amount);
                break;
            case AURA_MOD_INCREASE_HEALTH:
                Add(stats, BuffStat::Health, amount);
                break;
            case AURA_MOD_HEALTH_REGEN_IN_COMBAT:
                Add(stats, BuffStat::HealthRegen, amount);
                break;
            case AURA_MOD_RESISTANCE:
            case AURA_MOD_RESISTANCE_EXCLUSIVE:
                if (effect.misc & 1)
                    Add(stats, BuffStat::Armor, amount);
                Add(stats, BuffStat::Resistance, amount * SchoolShare(effect.misc));
                break;
            case AURA_MOD_TARGET_RESISTANCE:
                Add(stats, BuffStat::SpellPenetration, -amount);
                break;
            case AURA_MOD_SHIELD_BLOCKVALUE:
                Add(stats, BuffStat::Block, amount);
                break;
            case AURA_MOD_RATING:
                for (auto const& [bits, stat] : RATING_BITS)
                    if (static_cast<std::uint32_t>(effect.misc) & bits)
                        Add(stats, stat, amount);
                break;
            default:
                break;
        }
    }
    return stats;
}

// The weights the bot scores with: its own when the span is complete, else its role's
inline std::span<float const> BuffWeightsOf(BuffBot const& bot)
{
    return bot.statWeights.size() >= BUFF_STAT_COUNT ? bot.statWeights : RoleWeights(bot.role);
}

// The role's score of a record: its stat amounts times the weights; a negative weight or amount adds nothing
inline float BuffScore(Record const& record, std::span<float const> weights)
{
    BuffWeights const stats = BuffStats(record);
    float score = 0.0f;
    for (std::size_t i = 0; i < BUFF_STAT_COUNT && i < weights.size(); ++i)
        if (std::isfinite(weights[i]) && weights[i] > 0.0f && stats[i] > 0.0f)
            score += weights[i] * stats[i];
    return score;
}

// The largest amount of a record's aura effects: what a held aura of a shared group is compared with
inline std::int32_t BuffAmount(Record const& record)
{
    std::int32_t amount = 0;
    std::size_t const count = std::min<std::size_t>(record.effectCount, MAX_EFFECTS);
    for (std::size_t i = 0; i < count; ++i)
        if (record.effects[i].aura != 0)
            amount = std::max(amount, std::abs(record.effects[i].amount));
    return amount;
}

// How long the buff lasts, in minutes
inline float BuffMinutes(Record const& record)
{
    if (record.exclude == Exclude::WellFed)
        return WELL_FED_MINUTES;
    return static_cast<float>(record.durationMs) / 60000.0f;
}

// Two records that cannot be on the bot together (the later removes the earlier): from the exclusion words, the
// shared spell groups and the scroll's spell specific. Coatings exclude each other on one weapon only (WI8b).
inline bool Excludes(Record const& a, Record const& b)
{
    Exclude const x = a.exclude;
    Exclude const y = b.exclude;
    if (x != Exclude::Coating && IsBuffSlot(x) && x == y)
        return true;
    auto const elixir = [](Exclude e) { return e == Exclude::BattleElixir || e == Exclude::GuardianElixir; };
    if ((x == Exclude::Flask && elixir(y)) || (y == Exclude::Flask && elixir(x)))
        return true;
    if (a.spellSpecific == SPELL_SPECIFIC_SCROLL && b.spellSpecific == SPELL_SPECIFIC_SCROLL)
        return true;
    std::size_t const countA = std::min<std::size_t>(a.groupCount, MAX_GROUPS);
    std::size_t const countB = std::min<std::size_t>(b.groupCount, MAX_GROUPS);
    for (std::size_t i = 0; i < countA; ++i)
        for (std::size_t j = 0; j < countB; ++j)
            if (a.groups[i] != 0 && a.groups[i] == b.groups[j])
                return true;
    return false;
}

// A long aura on the bot holds one of the record's groups: never replace it, except a scroll that is strictly
// stronger than the held aura of that group
inline bool SlotTaken(BuffBot const& bot, Record const& record)
{
    std::size_t const held = std::min(bot.auraGroups.size(), bot.auraAmounts.size());
    std::size_t const count = std::min<std::size_t>(record.groupCount, MAX_GROUPS);
    std::int32_t const amount = BuffAmount(record);
    for (std::size_t i = 0; i < held; ++i)
        for (std::size_t j = 0; j < count; ++j)
            if (record.groups[j] != 0 && record.groups[j] == bot.auraGroups[i] &&
                (record.exclude != Exclude::Scroll || bot.auraAmounts[i] >= amount))
                return true;
    return false;
}

// The bot can use the entry now: one is left, it is ready and the bot's level is enough
inline bool BuffUsable(BuffBot const& bot, UsableEntry const& entry, Record const& record)
{
    return entry.count > 0 && entry.ready && record.needs.level <= bot.level && IsBuffSlot(record.exclude);
}

inline SpendCase BuffSpendCase(BuffBot const& bot, UsableEntry const& entry, Record const& record)
{
    std::size_t const ahead = std::min<std::size_t>(static_cast<std::size_t>(bot.ahead), AHEAD_COUNT - 1);
    float const minutesAhead =
        std::isfinite(bot.aheadMinutes) && bot.aheadMinutes > 0.0f ? bot.aheadMinutes : AHEAD_MINUTES[ahead];
    SpendCase spendCase;
    spendCase.kind = Kind::LongBuff;
    spendCase.rank = entry.rank;
    spendCase.replaceCopper = entry.replaceCopper;
    spendCase.danger = AHEAD_DANGER[ahead];
    spendCase.minutesSaved = AHEAD_SHARE[ahead] * std::min(minutesAhead, BuffMinutes(record));
    spendCase.deathCopper = bot.deathCopper;
    spendCase.earnCopperPerMinute = bot.earnCopperPerMinute;
    spendCase.thrift = bot.thrift;
    spendCase.daring = bot.daring;
    spendCase.control = bot.control;
    return spendCase;
}

inline BuffReason AheadReason(Ahead ahead)
{
    switch (ahead)
    {
        case Ahead::Dungeon:
            return BuffReason::AheadDungeon;
        case Ahead::Elite:
            return BuffReason::AheadElite;
        case Ahead::Group:
            return BuffReason::AheadGroup;
        case Ahead::Battleground:
            return BuffReason::AheadBg;
        default:
            return BuffReason::NothingAhead;
    }
}

// The buffs to use now, one pick per slot with a candidate (use or skip, slot order): never two that exclude each
// other, none the role cannot use, none weaker than another usable held of its slot. records[i] is list[i]'s record;
// the first 255 entries count.
inline std::vector<BuffPick> PickBuffs(BuffBot const& bot, std::span<UsableEntry const> list,
                                       std::span<Record const> records)
{
    std::span<float const> const weights = BuffWeightsOf(bot);
    std::size_t const count = std::min({list.size(), records.size(), static_cast<std::size_t>(255)});
    std::array<int, BUFF_SLOTS.size()> best;
    best.fill(-1);
    std::array<float, BUFF_SLOTS.size()> bestScore{};
    for (std::size_t i = 0; i < count; ++i)
    {
        Record const& record = records[i];
        int const slot = BuffSlotIndex(record.exclude);
        if (slot < 0 || !BuffUsable(bot, list[i], record))
            continue;

        float const score = BuffScore(record, weights);
        if (!(score > 0.0f))
            continue;

        int const held = best[slot];
        if (held < 0 || score > bestScore[slot] ||
            (score == bestScore[slot] && list[i].replaceCopper < list[static_cast<std::size_t>(held)].replaceCopper))
        {
            best[slot] = static_cast<int>(i);
            bestScore[slot] = score;
        }
    }

    std::array<BuffPick, BUFF_SLOTS.size()> picks{};
    for (std::size_t slot = 0; slot < BUFF_SLOTS.size(); ++slot)
    {
        if (best[slot] < 0)
            continue;

        std::size_t const index = static_cast<std::size_t>(best[slot]);
        BuffPick& pick = picks[slot];
        pick.usable = static_cast<std::uint8_t>(index);
        pick.slot = BUFF_SLOTS[slot];
        pick.score = bestScore[slot];
        if (bot.ahead == Ahead::None || bot.ahead >= Ahead::Count)
            pick.reason = BuffReason::NothingAhead;
        else if (SlotTaken(bot, records[index]))
            pick.reason = BuffReason::Excluded;
        else
        {
            pick.verdict = Spend(BuffSpendCase(bot, list[index], records[index]));
            if (pick.verdict.reason == SpendReason::Control)
                pick.reason = BuffReason::Control;
            else if (!pick.verdict.spend)
                pick.reason = BuffReason::NotWorth;
            else
            {
                pick.use = true;
                pick.reason = AheadReason(bot.ahead);
            }
        }
    }

    // The flask or the two elixirs: what each option brings when used; when neither brings anything, the raw scores
    constexpr std::size_t FLASK = 0;
    constexpr std::size_t BATTLE = 1;
    constexpr std::size_t GUARDIAN = 2;
    bool const elixirs = best[BATTLE] >= 0 || best[GUARDIAN] >= 0;
    if (best[FLASK] >= 0 && elixirs)
    {
        auto const brings = [&](std::size_t slot)
        { return best[slot] >= 0 && picks[slot].use ? bestScore[slot] : 0.0f; };
        float const flaskGain = brings(FLASK);
        float const elixirGain = brings(BATTLE) + brings(GUARDIAN);
        bool keepFlask = flaskGain >= elixirGain;
        if (!(flaskGain > 0.0f) && !(elixirGain > 0.0f))
        {
            float const elixirScore =
                (best[BATTLE] >= 0 ? bestScore[BATTLE] : 0.0f) + (best[GUARDIAN] >= 0 ? bestScore[GUARDIAN] : 0.0f);
            keepFlask = bestScore[FLASK] >= elixirScore;
        }
        if (keepFlask)
            best[BATTLE] = best[GUARDIAN] = -1;
        else
            best[FLASK] = -1;
    }

    std::vector<BuffPick> out;
    for (std::size_t slot = 0; slot < BUFF_SLOTS.size(); ++slot)
        if (best[slot] >= 0)
            out.push_back(picks[slot]);
    return out;
}

// The item_buff row's choice: use:<slot word> or skip:<slot word>
inline std::string BuffChoice(BuffPick const& pick)
{
    return std::string(pick.use ? "use:" : "skip:") + std::string(ExcludeWord(pick.slot));
}
}  // namespace ItemUse

#endif
