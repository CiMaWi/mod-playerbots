/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TRAITRULES_H
#define _PLAYERBOT_TRAITRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeTexts.h"

// The Player-free rules of a bot's personality: 21 traits rolled once per bot (seeded by guid and version), shifted by
// race and class, a few correlated pairs, one to three interests, and the words the LLM gets for them: a
// word per noticeable trait, without the words that contradict a higher-ranking one (LC.47).
// Header-only so tests/trait-selftest.cpp checks the same code the server runs.
namespace TraitRules
{
// A column per trait, stored by its name (BridgeTexts.h WORDS), so the order here is free. LC.47 kept 21 of the
// first 30 traits: the column of a removed trait stays in the table and is no longer read.
enum class Trait : uint8_t
{
    Drive,
    Curiosity,
    Caution,
    Aggression,
    Sociability,
    Loyalty,
    Generosity,
    Thrift,
    Orderliness,
    Patience,
    Courtesy,
    Talkativeness,
    Warmth,
    Humour,
    Register,
    Confidence,
    Outlook,
    Piety,
    FactionZeal,
    Suspicion,
    Theatricality
};
constexpr std::size_t TRAIT_COUNT = 21;

enum class Interest : uint8_t
{
    Fishing,
    Cooking,
    Collecting,
    Crafting,
    Trading,
    Duelling,
    Exploring,
    Lore,
    InnsAndAle,
    FineClothes
};
constexpr std::size_t INTEREST_COUNT = 10;
constexpr std::size_t MAX_INTERESTS = 3;

constexpr int MIN_VALUE = -100;
constexpr int MAX_VALUE = 100;
constexpr int NOTICEABLE = 30;  // |value| from which the LLM gets the trait
constexpr int STRONG = 65;      // |value| from which it gets the strong word
// Three grades of a word by |value| (LC.47): slight 30-44, noticeable 45-64, strong 65 and up [E]
constexpr int GRADE_SLIGHT = NOTICEABLE;
constexpr int GRADE_NOTICEABLE = 45;
constexpr int GRADE_STRONG = STRONG;
constexpr double DEVIATION = 45.0;
// A centre never leaves this, so even the most typical group keeps the occasional opposite
constexpr int MAX_CENTRE = 70;
constexpr double CORRELATION = 0.4;
constexpr uint64_t SEED_SALT = 0x7A11E5B0D1E5C0DEULL;

// Centre shifts: the defining one is the anchor (priests' piety: about two in three devout, one in twenty worldly)
constexpr int8_t SHIFT_DEFINING = 45;
constexpr int8_t SHIFT_MARKED = 30;
constexpr int8_t SHIFT_SLIGHT = 15;

// Interest weights: every interest starts even, race and class make some likelier
constexpr int INTEREST_BASE_WEIGHT = 10;
constexpr int INTEREST_FOND = 20;
constexpr int INTEREST_SOME = 10;
constexpr int ONE_INTEREST_PERCENT = 35;
constexpr int TWO_INTERESTS_PERCENT = 40;  // the rest have three

struct BotTraits
{
    std::array<int8_t, TRAIT_COUNT> Values{};
    std::array<uint8_t, MAX_INTERESTS> Interests{};  // Interest + 1, 0 = none
    uint32_t Version = 1;
};

struct Shift
{
    uint8_t Id;  // race or class id
    Trait Which;
    int8_t Amount;
};

// Races: 1 human, 2 orc, 3 dwarf, 4 night elf, 5 Forsaken, 6 tauren, 7 gnome, 8 troll, 10 blood elf, 11 draenei
constexpr Shift RACE_SHIFTS[] = {
    {1, Trait::Courtesy, SHIFT_SLIGHT},      {2, Trait::FactionZeal, SHIFT_DEFINING},
    {2, Trait::Aggression, SHIFT_SLIGHT},    {3, Trait::Talkativeness, SHIFT_SLIGHT},
    {3, Trait::Humour, SHIFT_SLIGHT},        {3, Trait::Confidence, SHIFT_SLIGHT},
    {4, Trait::Patience, SHIFT_MARKED},      {4, Trait::Piety, SHIFT_SLIGHT},
    {4, Trait::Register, SHIFT_SLIGHT},      {5, Trait::Outlook, -SHIFT_DEFINING},
    {5, Trait::Suspicion, SHIFT_DEFINING},   {5, Trait::Warmth, -SHIFT_SLIGHT},
    {6, Trait::Piety, SHIFT_MARKED},         {6, Trait::Patience, SHIFT_SLIGHT},
    {6, Trait::Aggression, -SHIFT_SLIGHT},   {7, Trait::Curiosity, SHIFT_DEFINING},
    {7, Trait::Talkativeness, SHIFT_SLIGHT}, {8, Trait::Humour, SHIFT_SLIGHT},
    {8, Trait::Piety, SHIFT_SLIGHT},         {10, Trait::Confidence, SHIFT_MARKED},
    {10, Trait::Register, SHIFT_MARKED},     {10, Trait::Theatricality, SHIFT_SLIGHT},
    {10, Trait::Thrift, -SHIFT_SLIGHT},      {11, Trait::Piety, SHIFT_MARKED},
    {11, Trait::Courtesy, SHIFT_SLIGHT},     {11, Trait::Patience, SHIFT_SLIGHT},
};

// Classes: 1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 6 death knight, 7 shaman, 8 mage, 9 warlock, 11 druid
constexpr Shift CLASS_SHIFTS[] = {
    {1, Trait::Aggression, SHIFT_MARKED},    {1, Trait::Caution, -SHIFT_SLIGHT},
    {2, Trait::Piety, SHIFT_DEFINING},       {2, Trait::Courtesy, SHIFT_SLIGHT},
    {2, Trait::Orderliness, SHIFT_SLIGHT},   {3, Trait::Sociability, -SHIFT_SLIGHT},
    {3, Trait::Patience, SHIFT_SLIGHT},      {4, Trait::Suspicion, SHIFT_SLIGHT},
    {4, Trait::Generosity, -SHIFT_SLIGHT},   {5, Trait::Piety, SHIFT_DEFINING},
    {5, Trait::Generosity, SHIFT_MARKED},    {6, Trait::Warmth, -SHIFT_DEFINING},
    {6, Trait::Outlook, -SHIFT_SLIGHT},      {7, Trait::Piety, SHIFT_MARKED},
    {8, Trait::Curiosity, SHIFT_SLIGHT},     {8, Trait::Register, SHIFT_SLIGHT},
    {9, Trait::Theatricality, SHIFT_SLIGHT}, {9, Trait::Suspicion, SHIFT_SLIGHT},
    {11, Trait::Patience, SHIFT_SLIGHT},     {11, Trait::Piety, SHIFT_SLIGHT},
    {11, Trait::Aggression, -SHIFT_SLIGHT},
};

// The follower's z becomes Rho * anchor + sqrt(1 - Rho^2) * own, so its spread stays the same; no follower is an anchor
struct Correlation
{
    Trait Anchor;
    Trait Follower;
    double Rho;
};

constexpr Correlation CORRELATIONS[] = {
    {Trait::FactionZeal, Trait::Aggression, CORRELATION},
    {Trait::Warmth, Trait::Talkativeness, CORRELATION},
    {Trait::Warmth, Trait::Sociability, CORRELATION},
    {Trait::Patience, Trait::Courtesy, CORRELATION},
};

struct InterestWeight
{
    uint8_t Id;  // race or class id
    Interest Which;
    int Bonus;
};

constexpr InterestWeight RACE_INTERESTS[] = {
    {1, Interest::Trading, INTEREST_SOME},  {1, Interest::FineClothes, INTEREST_SOME},
    {2, Interest::Duelling, INTEREST_FOND}, {3, Interest::InnsAndAle, INTEREST_FOND},
    {3, Interest::Crafting, INTEREST_SOME}, {4, Interest::Exploring, INTEREST_SOME},
    {4, Interest::Lore, INTEREST_SOME},     {5, Interest::Lore, INTEREST_SOME},
    {6, Interest::Cooking, INTEREST_SOME},  {6, Interest::Exploring, INTEREST_SOME},
    {7, Interest::Crafting, INTEREST_FOND}, {7, Interest::Lore, INTEREST_SOME},
    {8, Interest::Fishing, INTEREST_SOME},  {10, Interest::FineClothes, INTEREST_FOND},
    {10, Interest::Trading, INTEREST_SOME}, {11, Interest::Crafting, INTEREST_SOME},
    {11, Interest::Lore, INTEREST_SOME},
};

constexpr InterestWeight CLASS_INTERESTS[] = {
    {1, Interest::Duelling, INTEREST_SOME},   {1, Interest::InnsAndAle, INTEREST_SOME},
    {3, Interest::Exploring, INTEREST_FOND},  {3, Interest::Collecting, INTEREST_SOME},
    {4, Interest::Trading, INTEREST_SOME},    {4, Interest::Duelling, INTEREST_SOME},
    {5, Interest::Lore, INTEREST_SOME},       {5, Interest::Cooking, INTEREST_SOME},
    {6, Interest::Duelling, INTEREST_SOME},   {7, Interest::Fishing, INTEREST_SOME},
    {7, Interest::Exploring, INTEREST_SOME},  {8, Interest::Lore, INTEREST_FOND},
    {9, Interest::Lore, INTEREST_SOME},       {9, Interest::FineClothes, INTEREST_SOME},
    {11, Interest::Exploring, INTEREST_SOME},
};

using BridgeTexts::Nature::CONFLICTS;
using BridgeTexts::Nature::INTEREST_CONFLICTS;
using BridgeTexts::Nature::TraitWords;
using BridgeTexts::Nature::WordConflict;
using BridgeTexts::Nature::WORDS;
static_assert(WORDS.size() == TRAIT_COUNT, "BridgeTexts.h holds a row per trait");

// LC15c: the area of each trait, in the Trait enum's order; the bridge groups the words by it (LC.33). The wire names
// are the bridge's TRAIT_AREAS (bridge/claude/envelope.mjs): talk, others, work, wants.
constexpr std::size_t AREA_COUNT = 4;
constexpr std::array<std::string_view, AREA_COUNT> AREA_NAMES = {"talk", "others", "work", "wants"};
constexpr std::array<uint8_t, TRAIT_COUNT> TRAIT_AREAS = {
    2, 3, 2, 2, 1, 1, 1,  // drive curiosity caution aggression sociability loyalty generosity
    3, 2, 2, 1, 0, 1, 0,  // thrift orderliness patience courtesy talkativeness warmth humour
    0, 0, 3, 3, 3, 1, 0,  // register confidence outlook piety zeal suspicion theatricality
};

// LC.47: the traits that something besides the words reads: the planner (GoalModel.h), upkeep, item spending, the
// errands, the routes, the fight rules, and the bridge's reply length (talkativeness). When two words contradict each
// other, such a trait keeps its word (Outranks). In the Trait enum's order; a new reader of a trait sets its cell.
constexpr std::array<bool, TRAIT_COUNT> READ_BY_BEHAVIOUR = {
    // drive curiosity caution aggression sociability loyalty generosity
    true,
    true,
    true,
    true,
    true,
    false,
    true,
    // thrift orderliness patience courtesy talkativeness warmth humour
    true,
    true,
    true,
    true,
    true,
    false,
    false,
    // register confidence outlook piety zeal suspicion theatricality
    false,
    true,
    false,
    false,
    true,
    false,
    false,
};

constexpr std::string_view AreaOf(Trait trait) { return AREA_NAMES[TRAIT_AREAS[static_cast<std::size_t>(trait)]]; }

using BridgeTexts::Nature::INTEREST_NAMES;
static_assert(INTEREST_NAMES.size() == INTEREST_COUNT);

constexpr std::size_t Index(Trait trait) { return static_cast<std::size_t>(trait); }

static_assert(Index(Trait::Theatricality) + 1 == TRAIT_COUNT, "TRAIT_COUNT must match the Trait enum");
static_assert(static_cast<std::size_t>(Interest::FineClothes) + 1 == INTEREST_COUNT,
              "INTEREST_COUNT must match the Interest enum");

// Every trait has a name and three words, every interest a name: a shorter initialiser would leave empty cells
constexpr bool AllNamed()
{
    for (TraitWords const& words : WORDS)
    {
        if (words.Name.empty())
            return false;

        for (std::size_t grade = 0; grade < words.Low.size(); ++grade)
            if (words.Low[grade].empty() || words.High[grade].empty())
                return false;
    }

    for (std::string_view const name : INTEREST_NAMES)
        if (name.empty() || name.find(',') != std::string_view::npos)
            return false;

    return true;
}
static_assert(AllNamed(), "every trait needs a name and three words, every interest a comma-free name");

// Every trait has exactly one of the four areas
constexpr bool AllInAnArea()
{
    for (uint8_t const area : TRAIT_AREAS)
        if (area >= AREA_COUNT)
            return false;

    return true;
}
static_assert(AllInAnArea(), "every trait needs one of the four areas");

// The grade of a value: 0 below the first mark (silent), 1 slight, 2 noticeable, 3 strong
constexpr int Grade(int value)
{
    int const size = value < 0 ? -value : value;
    if (size >= GRADE_STRONG)
        return 3;
    if (size >= GRADE_NOTICEABLE)
        return 2;
    return size >= GRADE_SLIGHT ? 1 : 0;
}
static_assert(Grade(MAX_VALUE) == static_cast<int>(WORDS[0].Low.size()), "BridgeTexts.h holds a word per grade");

// The grade with its pole's sign: -3..-1 pick the low words, 1..3 the high ones
constexpr int SignedGrade(int value) { return value < 0 ? -Grade(value) : Grade(value); }

// Where a word stands in WORDS: its trait's row and its signed grade; no row (TRAIT_COUNT) when no trait has the word
struct WordPlace
{
    std::size_t Row = TRAIT_COUNT;
    int Signed = 0;
};

constexpr WordPlace PlaceOf(std::string_view word)
{
    for (std::size_t row = 0; row < TRAIT_COUNT; ++row)
        for (std::size_t grade = 0; grade < WORDS[row].Low.size(); ++grade)
        {
            if (WORDS[row].Low[grade] == word)
                return {row, -static_cast<int>(grade) - 1};
            if (WORDS[row].High[grade] == word)
                return {row, static_cast<int>(grade) + 1};
        }

    return {};
}

// LC.47: BridgeTexts.h CONFLICTS by place, so a nature is checked on its values, not on strings
struct ConflictPlace
{
    WordPlace First;
    WordPlace Second;
};
constexpr std::size_t CONFLICT_COUNT = std::size(CONFLICTS);

constexpr std::array<ConflictPlace, CONFLICT_COUNT> ConflictPlaces()
{
    std::array<ConflictPlace, CONFLICT_COUNT> places{};
    for (std::size_t i = 0; i < CONFLICT_COUNT; ++i)
        places[i] = {PlaceOf(CONFLICTS[i].First), PlaceOf(CONFLICTS[i].Second)};

    return places;
}
constexpr std::array<ConflictPlace, CONFLICT_COUNT> CONFLICT_PLACES = ConflictPlaces();

constexpr bool ConflictsNameWords()
{
    for (ConflictPlace const& place : CONFLICT_PLACES)
        if (place.First.Row == TRAIT_COUNT || place.Second.Row == TRAIT_COUNT || place.First.Row == place.Second.Row)
            return false;

    return true;
}
static_assert(ConflictsNameWords(), "a conflict needs two words of BridgeTexts.h WORDS, of two different traits");

// The pairs that name a word of each trait: of two traits otherwise alike, the one in fewer pairs keeps its word
constexpr std::array<int, TRAIT_COUNT> ConflictCounts()
{
    std::array<int, TRAIT_COUNT> counts{};
    for (ConflictPlace const& place : CONFLICT_PLACES)
    {
        if (place.First.Row < TRAIT_COUNT)
            ++counts[place.First.Row];
        if (place.Second.Row < TRAIT_COUNT)
            ++counts[place.Second.Row];
    }

    return counts;
}
constexpr std::array<int, TRAIT_COUNT> CONFLICT_COUNTS = ConflictCounts();

// LC.47: BridgeTexts.h INTEREST_CONFLICTS by place: an interest and the word that bars it
struct InterestBar
{
    std::size_t Id = INTEREST_COUNT;  // the Interest as an index; INTEREST_COUNT when no interest has the name
    WordPlace Against;
};
constexpr std::size_t INTEREST_BAR_COUNT = std::size(INTEREST_CONFLICTS);

constexpr std::array<InterestBar, INTEREST_BAR_COUNT> InterestBars()
{
    std::array<InterestBar, INTEREST_BAR_COUNT> bars{};
    for (std::size_t i = 0; i < INTEREST_BAR_COUNT; ++i)
    {
        for (std::size_t id = 0; id < INTEREST_COUNT; ++id)
            if (INTEREST_NAMES[id] == INTEREST_CONFLICTS[i].First)
                bars[i].Id = id;

        bars[i].Against = PlaceOf(INTEREST_CONFLICTS[i].Second);
    }

    return bars;
}
constexpr std::array<InterestBar, INTEREST_BAR_COUNT> INTEREST_BARS = InterestBars();

constexpr bool InterestBarsNameWords()
{
    for (InterestBar const& bar : INTEREST_BARS)
        if (bar.Id == INTEREST_COUNT || bar.Against.Row == TRAIT_COUNT)
            return false;

    return true;
}
static_assert(InterestBarsNameWords(),
              "an interest conflict needs an interest's name and a word of BridgeTexts.h WORDS");

// Roll applies the pairs in order on the raw draws: a follower used as an anchor would chain the correlations
constexpr bool FollowersAreNoAnchors()
{
    for (Correlation const& follower : CORRELATIONS)
        for (Correlation const& anchor : CORRELATIONS)
            if (follower.Follower == anchor.Anchor)
                return false;

    return true;
}
static_assert(FollowersAreNoAnchors(), "a correlated follower must not anchor another pair");

// splitmix64 stepping its state, as BotProfileMgr's skill roll: a fixed integer mix, so a guid gives the same traits on
// every build and standard library (never <random>)
inline uint64_t NextRandom(uint64_t& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Uniform in [0, 1) from the top 53 bits
inline double Uniform(uint64_t& state) { return static_cast<double>(NextRandom(state) >> 11) / 9007199254740992.0; }

// Box-Muller on two seeded uniforms; std::normal_distribution differs between standard libraries
inline double NormalZ(uint64_t& state)
{
    constexpr double pi = 3.14159265358979323846;
    double const u1 = 1.0 - Uniform(state);  // (0, 1], so the log is finite
    double const u2 = Uniform(state);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * pi * u2);
}

// The centre of each trait's spread for a race and class: the sum of both shifts (unknown ids shift nothing)
inline std::array<int, TRAIT_COUNT> Centres(uint8_t race, uint8_t cls)
{
    std::array<int, TRAIT_COUNT> centres{};
    for (Shift const& shift : RACE_SHIFTS)
        if (shift.Id == race)
            centres[Index(shift.Which)] += shift.Amount;

    for (Shift const& shift : CLASS_SHIFTS)
        if (shift.Id == cls)
            centres[Index(shift.Which)] += shift.Amount;

    for (int& centre : centres)
        centre = std::clamp(centre, -MAX_CENTRE, MAX_CENTRE);

    return centres;
}

inline std::array<int, INTEREST_COUNT> InterestWeights(uint8_t race, uint8_t cls)
{
    std::array<int, INTEREST_COUNT> weights;
    weights.fill(INTEREST_BASE_WEIGHT);
    for (InterestWeight const& weight : RACE_INTERESTS)
        if (weight.Id == race)
            weights[static_cast<std::size_t>(weight.Which)] += weight.Bonus;

    for (InterestWeight const& weight : CLASS_INTERESTS)
        if (weight.Id == cls)
            weights[static_cast<std::size_t>(weight.Which)] += weight.Bonus;

    return weights;
}

// Deterministic: the same guid, version, race and class give the same traits on every build
inline BotTraits Roll(uint32_t guid, uint32_t version, uint8_t race, uint8_t cls)
{
    uint64_t state = ((static_cast<uint64_t>(guid) << 32) | version) ^ SEED_SALT;
    std::array<double, TRAIT_COUNT> z;
    for (double& value : z)
        value = NormalZ(state);

    for (Correlation const& pair : CORRELATIONS)
        z[Index(pair.Follower)] =
            pair.Rho * z[Index(pair.Anchor)] + std::sqrt(1.0 - pair.Rho * pair.Rho) * z[Index(pair.Follower)];

    BotTraits traits;
    traits.Version = version;
    std::array<int, TRAIT_COUNT> const centres = Centres(race, cls);
    for (std::size_t i = 0; i < TRAIT_COUNT; ++i)
    {
        long const value = std::lround(centres[i] + DEVIATION * z[i]);
        traits.Values[i] = static_cast<int8_t>(std::clamp<long>(value, MIN_VALUE, MAX_VALUE));
    }

    double const countRoll = Uniform(state) * 100.0;
    std::size_t const count = countRoll < ONE_INTEREST_PERCENT                           ? 1
                              : countRoll < ONE_INTEREST_PERCENT + TWO_INTERESTS_PERCENT ? 2
                                                                                         : MAX_INTERESTS;
    // Weighted draws without replacement; an interest the bot's own nature contradicts is never drawn (LC.47)
    std::array<int, INTEREST_COUNT> weights = InterestWeights(race, cls);
    for (InterestBar const& bar : INTEREST_BARS)
        if (SignedGrade(traits.Values[bar.Against.Row]) == bar.Against.Signed)
            weights[bar.Id] = 0;

    for (std::size_t pick = 0; pick < count; ++pick)
    {
        int total = 0;
        for (int const weight : weights)
            total += weight;

        if (!total)
            break;  // every interest left is barred

        double target = Uniform(state) * total;
        std::size_t chosen = INTEREST_COUNT - 1;
        for (std::size_t i = 0; i < INTEREST_COUNT; ++i)
        {
            if (!weights[i])
                continue;

            chosen = i;
            target -= weights[i];
            if (target < 0.0)
                break;
        }

        traits.Interests[pick] = static_cast<uint8_t>(chosen + 1);
        weights[chosen] = 0;
    }

    return traits;
}

// The AI's view of a trait: -1..+1
inline float Unit(BotTraits const& traits, Trait trait)
{
    return static_cast<float>(traits.Values[Index(trait)]) / static_cast<float>(MAX_VALUE);
}

inline bool IsNoticeable(int value) { return std::abs(value) >= NOTICEABLE; }

// The word for one trait value: the table's word of its grade on the pole the sign names; empty when the trait is not
// noticeable.
inline std::string Word(Trait trait, int value)
{
    int const grade = Grade(value);
    if (grade == 0)
        return {};

    TraitWords const& words = WORDS[Index(trait)];
    return std::string((value > 0 ? words.High : words.Low)[grade - 1]);
}

// The noticeable traits, strongest first (ties in trait order)
inline std::vector<std::size_t> NoticeableOrder(BotTraits const& traits)
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < TRAIT_COUNT; ++i)
        if (IsNoticeable(traits.Values[i]))
            order.push_back(i);

    std::stable_sort(order.begin(), order.end(), [&traits](std::size_t a, std::size_t b)
                     { return std::abs(traits.Values[a]) > std::abs(traits.Values[b]); });

    return order;
}

// The coin of a trait in a conflict nothing else decides: fixed by the bot's values, so a bot tells the same words
// every time, and as good as even between any two traits
inline uint64_t Coin(BotTraits const& traits, std::size_t trait)
{
    uint64_t state = SEED_SALT + trait;
    for (int8_t const value : traits.Values)
    {
        uint64_t const mixed = NextRandom(state);
        state = mixed + static_cast<uint8_t>(value);
    }

    return NextRandom(state);
}

// LC.47: which of two traits keeps its word when the two words contradict each other: the trait that also steers what
// the bot does, then the stronger one, then the one in fewer conflicts, then the coin
inline bool Outranks(BotTraits const& traits, std::size_t a, std::size_t b)
{
    if (READ_BY_BEHAVIOUR[a] != READ_BY_BEHAVIOUR[b])
        return READ_BY_BEHAVIOUR[a];

    int const sizeA = std::abs(traits.Values[a]);
    int const sizeB = std::abs(traits.Values[b]);
    if (sizeA != sizeB)
        return sizeA > sizeB;

    if (CONFLICT_COUNTS[a] != CONFLICT_COUNTS[b])
        return CONFLICT_COUNTS[a] < CONFLICT_COUNTS[b];

    uint64_t const coinA = Coin(traits, a);
    uint64_t const coinB = Coin(traits, b);
    return coinA != coinB ? coinA > coinB : a < b;
}

// The traits a nature tells, strongest first (ties in trait order): the noticeable ones without the words that lose a
// conflict (BridgeTexts.h CONFLICTS). The traits are taken by rank and each keeps its word unless that contradicts a
// word already kept, so a word falls only to a word that is told. Read from the values each time: a change to the
// pairs reaches every bot, and no stored value changes.
inline std::vector<std::size_t> ToldOrder(BotTraits const& traits)
{
    std::vector<std::size_t> const noticeable = NoticeableOrder(traits);
    std::vector<std::size_t> ranked = noticeable;
    std::sort(ranked.begin(), ranked.end(), [&traits](std::size_t a, std::size_t b) { return Outranks(traits, a, b); });

    std::array<bool, TRAIT_COUNT> told{};
    for (std::size_t const candidate : ranked)
    {
        int const grade = SignedGrade(traits.Values[candidate]);
        bool kept = true;
        for (ConflictPlace const& place : CONFLICT_PLACES)
        {
            WordPlace const* other = nullptr;
            if (place.First.Row == candidate && place.First.Signed == grade)
                other = &place.Second;
            else if (place.Second.Row == candidate && place.Second.Signed == grade)
                other = &place.First;

            if (other && told[other->Row] && SignedGrade(traits.Values[other->Row]) == other->Signed)
            {
                kept = false;
                break;
            }
        }

        told[candidate] = kept;
    }

    std::vector<std::size_t> order;
    for (std::size_t const i : noticeable)
        if (told[i])
            order.push_back(i);

    return order;
}

// The words of the traits a nature tells, strongest first: what the LLM gets
inline std::vector<std::string> Describe(BotTraits const& traits)
{
    std::vector<std::string> words;
    for (std::size_t const i : ToldOrder(traits))
        words.emplace_back(Word(static_cast<Trait>(i), traits.Values[i]));

    return words;
}

// LC15c: the area of each word of Describe, one per word in the same order
inline std::vector<std::string> DescribeAreas(BotTraits const& traits)
{
    std::vector<std::string> areas;
    for (std::size_t const i : ToldOrder(traits))
        areas.emplace_back(AreaOf(static_cast<Trait>(i)));

    return areas;
}

inline std::vector<std::string> InterestNames(BotTraits const& traits)
{
    std::vector<std::string> names;
    for (uint8_t const interest : traits.Interests)
        if (interest && interest <= INTEREST_COUNT)
            names.emplace_back(INTEREST_NAMES[interest - 1]);

    return names;
}

// The trait a GM command names ("piety", "zeal"); false when no trait has that name. "helpfulness" still names zeal,
// which feeds the world PvP term the removed trait fed (LC.47): lab scenarios of other plans set it by that name
// until every slot holds this change; the alias goes then.
inline bool FindTrait(std::string_view name, Trait& out)
{
    if (name == "helpfulness")
        name = "zeal";

    for (std::size_t i = 0; i < TRAIT_COUNT; ++i)
    {
        if (WORDS[i].Name != name)
            continue;

        out = static_cast<Trait>(i);
        return true;
    }

    return false;
}
}  // namespace TraitRules

#endif
