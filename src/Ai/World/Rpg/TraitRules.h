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
#include <string>
#include <string_view>
#include <vector>

// The Player-free rules of a bot's personality: 30 traits rolled once per bot (seeded by guid and version), shifted by
// race and class, a few correlated pairs, one to three interests, and the words the LLM gets for them.
// Header-only so tests/trait-selftest.cpp checks the same code the server runs.
namespace TraitRules
{
// Order is the stored column order: append only
enum class Trait : uint8_t
{
    Drive,
    Curiosity,
    Caution,
    Aggression,
    Sociability,
    Helpfulness,
    Leadership,
    Loyalty,
    Generosity,
    Thrift,
    Greed,
    Orderliness,
    Thoroughness,
    Competitiveness,
    Patience,
    Courtesy,
    Rootedness,
    Talkativeness,
    Warmth,
    Humour,
    Register,
    Confidence,
    Outlook,
    Piety,
    FactionZeal,
    Lore,
    Suspicion,
    Theatricality,
    Mischief,
    Worldliness
};
constexpr std::size_t TRAIT_COUNT = 30;

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
    {1, Trait::Courtesy, SHIFT_SLIGHT},        {1, Trait::Competitiveness, SHIFT_SLIGHT},
    {2, Trait::FactionZeal, SHIFT_DEFINING},   {2, Trait::Aggression, SHIFT_SLIGHT},
    {2, Trait::Competitiveness, SHIFT_SLIGHT}, {3, Trait::Talkativeness, SHIFT_SLIGHT},
    {3, Trait::Humour, SHIFT_SLIGHT},          {3, Trait::Confidence, SHIFT_SLIGHT},
    {4, Trait::Patience, SHIFT_MARKED},        {4, Trait::Piety, SHIFT_SLIGHT},
    {4, Trait::Register, SHIFT_SLIGHT},        {5, Trait::Outlook, -SHIFT_DEFINING},
    {5, Trait::Suspicion, SHIFT_DEFINING},     {5, Trait::Warmth, -SHIFT_SLIGHT},
    {6, Trait::Rootedness, SHIFT_DEFINING},    {6, Trait::Piety, SHIFT_MARKED},
    {6, Trait::Patience, SHIFT_SLIGHT},        {6, Trait::Aggression, -SHIFT_SLIGHT},
    {7, Trait::Curiosity, SHIFT_DEFINING},     {7, Trait::Lore, SHIFT_DEFINING},
    {7, Trait::Talkativeness, SHIFT_SLIGHT},   {8, Trait::Humour, SHIFT_SLIGHT},
    {8, Trait::Mischief, SHIFT_SLIGHT},        {8, Trait::Piety, SHIFT_SLIGHT},
    {10, Trait::Confidence, SHIFT_MARKED},     {10, Trait::Register, SHIFT_MARKED},
    {10, Trait::Theatricality, SHIFT_SLIGHT},  {10, Trait::Thrift, -SHIFT_SLIGHT},
    {11, Trait::Piety, SHIFT_MARKED},          {11, Trait::Courtesy, SHIFT_SLIGHT},
    {11, Trait::Patience, SHIFT_SLIGHT},
};

// Classes: 1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 6 death knight, 7 shaman, 8 mage, 9 warlock, 11 druid
constexpr Shift CLASS_SHIFTS[] = {
    {1, Trait::Aggression, SHIFT_MARKED},    {1, Trait::Competitiveness, SHIFT_SLIGHT},
    {1, Trait::Caution, -SHIFT_SLIGHT},      {2, Trait::Piety, SHIFT_DEFINING},
    {2, Trait::Courtesy, SHIFT_SLIGHT},      {2, Trait::Orderliness, SHIFT_SLIGHT},
    {3, Trait::Rootedness, -SHIFT_DEFINING}, {3, Trait::Sociability, -SHIFT_SLIGHT},
    {3, Trait::Patience, SHIFT_SLIGHT},      {4, Trait::Mischief, SHIFT_DEFINING},
    {4, Trait::Suspicion, SHIFT_SLIGHT},     {4, Trait::Greed, SHIFT_SLIGHT},
    {5, Trait::Piety, SHIFT_DEFINING},       {5, Trait::Helpfulness, SHIFT_MARKED},
    {6, Trait::Worldliness, SHIFT_DEFINING}, {6, Trait::Warmth, -SHIFT_DEFINING},
    {6, Trait::Outlook, -SHIFT_SLIGHT},      {7, Trait::Piety, SHIFT_MARKED},
    {7, Trait::Rootedness, SHIFT_SLIGHT},    {8, Trait::Lore, SHIFT_DEFINING},
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
    {Trait::FactionZeal, Trait::Aggression, CORRELATION}, {Trait::Warmth, Trait::Talkativeness, CORRELATION},
    {Trait::Warmth, Trait::Sociability, CORRELATION},     {Trait::Patience, Trait::Courtesy, CORRELATION},
    {Trait::Curiosity, Trait::Rootedness, -CORRELATION},
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

// Words per pole: [0] noticeable, [1] strong. In-world words only: no numbers, nothing about mechanics
struct TraitWords
{
    std::string_view Name;  // the GM command's name for the trait
    std::array<std::string_view, 2> Low;
    std::array<std::string_view, 2> High;
};

constexpr std::array<TraitWords, TRAIT_COUNT> WORDS = {{
    {"drive", {"rather relaxed", "very relaxed"}, {"rather driven", "very driven"}},
    {"curiosity", {"rather settled", "very settled"}, {"rather curious", "very curious"}},
    {"caution", {"rather daring", "reckless"}, {"rather careful", "very careful"}},
    {"aggression", {"rather peaceable", "very gentle"}, {"rather combative", "belligerent"}},
    {"sociability", {"rather solitary", "a loner"}, {"rather sociable", "very gregarious"}},
    {"helpfulness", {"rather self-reliant", "very self-reliant"}, {"rather helpful", "very helpful"}},
    {"leadership", {"rather deferential", "very deferential"}, {"rather commanding", "very commanding"}},
    {"loyalty", {"rather fickle", "very fickle"}, {"rather loyal", "fiercely loyal"}},
    {"generosity", {"rather stingy", "very stingy"}, {"rather generous", "very generous"}},
    {"thrift", {"rather free-spending", "a spendthrift"}, {"rather frugal", "very frugal"}},
    {"greed", {"rather content", "very content"}, {"rather greedy", "very greedy"}},
    {"orderliness", {"rather messy", "very messy"}, {"rather tidy", "very tidy"}},
    {"thoroughness", {"rather brisk", "very brisk"}, {"rather thorough", "very thorough"}},
    {"competitiveness", {"rather easygoing", "very easygoing"}, {"rather competitive", "fiercely competitive"}},
    {"patience", {"rather hot-tempered", "very hot-tempered"}, {"rather patient", "very patient"}},
    {"courtesy", {"rather selfish", "very selfish"}, {"rather courteous", "very courteous"}},
    {"rootedness", {"rather footloose", "a wanderer"}, {"rather home-loving", "a homebody"}},
    {"talkativeness", {"rather taciturn", "very taciturn"}, {"rather chatty", "very chatty"}},
    {"warmth", {"rather guarded", "very guarded"}, {"rather warm", "very warm"}},
    {"humour", {"rather earnest", "very earnest"}, {"rather jokey", "a joker"}},
    {"register", {"rather plain-spoken", "very plain-spoken"}, {"rather formal", "very formal"}},
    {"confidence", {"rather modest", "very modest"}, {"rather boastful", "very boastful"}},
    {"outlook", {"rather gloomy", "very gloomy"}, {"rather cheerful", "very cheerful"}},
    {"piety", {"rather worldly", "very worldly"}, {"rather devout", "deeply devout"}},
    {"zeal", {"rather pragmatic", "very pragmatic"}, {"rather patriotic", "fiercely patriotic"}},
    {"lore", {"rather practical", "very practical"}, {"rather bookish", "very bookish"}},
    {"suspicion", {"rather trusting", "very trusting"}, {"rather suspicious", "very suspicious"}},
    {"theatricality", {"rather reserved", "very reserved"}, {"rather dramatic", "very dramatic"}},
    {"mischief", {"rather upright", "very upright"}, {"rather mischievous", "a trickster"}},
    {"worldliness", {"rather green", "very green"}, {"rather seasoned", "a veteran"}},
}};

constexpr std::array<std::string_view, INTEREST_COUNT> INTEREST_NAMES = {
    "fishing",      "cooking", "collecting pets, mounts and tabards",
    "crafting",     "trading", "duelling",
    "exploring",    "lore",    "inns and ale",
    "fine clothes",
};

constexpr std::size_t Index(Trait trait) { return static_cast<std::size_t>(trait); }

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
    // Weighted draws without replacement
    std::array<int, INTEREST_COUNT> weights = InterestWeights(race, cls);
    for (std::size_t pick = 0; pick < count; ++pick)
    {
        int total = 0;
        for (int const weight : weights)
            total += weight;

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

// The word for one trait value; empty when the trait is not noticeable
inline std::string_view Word(Trait trait, int value)
{
    if (!IsNoticeable(value))
        return {};

    TraitWords const& words = WORDS[Index(trait)];
    std::size_t const intensity = std::abs(value) >= STRONG ? 1 : 0;
    return value > 0 ? words.High[intensity] : words.Low[intensity];
}

// Every noticeable trait as words, strongest first (ties in trait order): what the LLM gets
inline std::vector<std::string> Describe(BotTraits const& traits)
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < TRAIT_COUNT; ++i)
        if (IsNoticeable(traits.Values[i]))
            order.push_back(i);

    std::stable_sort(order.begin(), order.end(), [&traits](std::size_t a, std::size_t b)
                     { return std::abs(traits.Values[a]) > std::abs(traits.Values[b]); });

    std::vector<std::string> words;
    words.reserve(order.size());
    for (std::size_t const i : order)
        words.emplace_back(Word(static_cast<Trait>(i), traits.Values[i]));

    return words;
}

inline std::vector<std::string> InterestNames(BotTraits const& traits)
{
    std::vector<std::string> names;
    for (uint8_t const interest : traits.Interests)
        if (interest && interest <= INTEREST_COUNT)
            names.emplace_back(INTEREST_NAMES[interest - 1]);

    return names;
}

// The trait a GM command names ("piety", "zeal"); false when no trait has that name
inline bool FindTrait(std::string_view name, Trait& out)
{
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
