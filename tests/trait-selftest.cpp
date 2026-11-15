/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the trait rules (src/Ai/World/Rpg/TraitRules.h): determinism and ranges, the share of noticeable
// and strong traits per bot, priests' piety, the race and class shifts of the lore list, the correlated pairs, the
// words and the interest weights; prints the sample of 20 personalities as the LLM sees them.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "TraitRules.h"

using TraitRules::BotTraits;
using TraitRules::Index;
using TraitRules::Trait;

namespace
{
constexpr uint8_t RACES[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
constexpr uint8_t CLASSES[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
constexpr std::size_t KINDS = 10;
constexpr uint8_t HUMAN = 1;
constexpr uint8_t ORC = 2;
constexpr uint8_t DWARF = 3;
constexpr uint8_t FORSAKEN = 5;
constexpr uint8_t TAUREN = 6;
constexpr uint8_t GNOME = 7;
constexpr uint8_t PALADIN = 2;
constexpr uint8_t HUNTER = 3;
constexpr uint8_t ROGUE = 4;
constexpr uint8_t PRIEST = 5;
constexpr uint8_t DEATH_KNIGHT = 6;
constexpr uint8_t MAGE = 8;
constexpr uint8_t PRIEST_RACES[] = {1, 3, 4, 5, 8, 10, 11};

constexpr uint32_t POPULATION = 5000;
constexpr uint32_t PRIESTS = 20000;
constexpr uint32_t GROUP = 3000;
constexpr uint32_t PRIEST_GUIDS = 100000;
constexpr uint32_t GROUP_GUIDS = 200000;

// 30 traits at sigma 45: 15.2 noticeable and 4.5 strong per bot before shifts; shifts add a little
constexpr double NOTICEABLE_MIN = 13.0;
constexpr double NOTICEABLE_MAX = 17.0;
constexpr double STRONG_MIN = 3.5;
constexpr double STRONG_MAX = 5.5;
// Piety centre +45 alone (human priests): 0.631 devout, 0.048 worldly
constexpr double DEVOUT_MIN = 0.60;
constexpr double DEVOUT_MAX = 0.70;
constexpr double WORLDLY_MIN = 0.03;
constexpr double WORLDLY_MAX = 0.07;
// Priests of every priest race: race shifts make a few more devout, never most of them
constexpr double ALL_RACES_DEVOUT_MAX = 0.80;
// ...and the occasional worldly priest stays (observed 0.037)
constexpr double ALL_RACES_WORLDLY_MIN = 0.02;
// A group's mean exceeds the all-bots mean by at least this share of its nominal shift (the all-bots mean carries other
// groups' shifts of the same trait, clipping and the centre cap take a little more)
constexpr double SHIFT_SHARE = 0.5;
constexpr int DEFINING = TraitRules::SHIFT_DEFINING;
constexpr int MARKED = TraitRules::SHIFT_MARKED;
constexpr double CORRELATION_TOLERANCE = 0.05;
// A trait no race or class shifts: sigma 45 clipped at +-100 keeps about 43.7
constexpr double SPREAD_MIN = 41.0;
constexpr double SPREAD_MAX = 46.0;
constexpr double DWARF_INN_FACTOR = 1.5;
constexpr std::size_t VERSION_DIFFER_MIN = 25;
constexpr uint32_t SAMPLE_FIRST_GUID = 1001;

int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

BotTraits PopulationBot(uint32_t guid)
{
    return TraitRules::Roll(guid, 1, RACES[guid % KINDS], CLASSES[(guid / KINDS) % KINDS]);
}

std::vector<BotTraits> Population()
{
    std::vector<BotTraits> bots;
    for (uint32_t guid = 1; guid <= POPULATION; ++guid)
        bots.push_back(PopulationBot(guid));

    return bots;
}

double Mean(std::vector<BotTraits> const& bots, Trait trait)
{
    double sum = 0.0;
    for (BotTraits const& bot : bots)
        sum += bot.Values[Index(trait)];

    return sum / bots.size();
}

// 3000 bots of one race (classes mixed) or one class (races mixed)
std::vector<BotTraits> Group(uint8_t race, uint8_t cls)
{
    std::vector<BotTraits> bots;
    for (uint32_t i = 0; i < GROUP; ++i)
        bots.push_back(
            TraitRules::Roll(GROUP_GUIDS + i, 1, race ? race : RACES[i % KINDS], cls ? cls : CLASSES[i % KINDS]));

    return bots;
}

bool HasInterest(BotTraits const& bot, TraitRules::Interest interest)
{
    for (uint8_t const id : bot.Interests)
        if (id == static_cast<uint8_t>(interest) + 1)
            return true;

    return false;
}

void CheckDeterminism()
{
    BotTraits const first = TraitRules::Roll(4242, 1, ORC, HUNTER);
    BotTraits const again = TraitRules::Roll(4242, 1, ORC, HUNTER);
    Check(first.Values == again.Values && first.Interests == again.Interests, "the same guid rolls the same traits");

    BotTraits const reroll = TraitRules::Roll(4242, 2, ORC, HUNTER);
    BotTraits const other = TraitRules::Roll(4243, 1, ORC, HUNTER);
    std::size_t versionDiffers = 0;
    std::size_t guidDiffers = 0;
    for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
    {
        versionDiffers += first.Values[i] != reroll.Values[i];
        guidDiffers += first.Values[i] != other.Values[i];
    }

    Check(versionDiffers >= VERSION_DIFFER_MIN, "version 2 rerolls most traits");
    Check(guidDiffers >= VERSION_DIFFER_MIN, "another guid rolls other traits");
    Check(reroll.Version == 2, "the roll keeps its version");
}

void CheckRanges(std::vector<BotTraits> const& bots)
{
    bool valuesInRange = true;
    bool interestsValid = true;
    for (BotTraits const& bot : bots)
    {
        for (int8_t const value : bot.Values)
            valuesInRange &= value >= TraitRules::MIN_VALUE && value <= TraitRules::MAX_VALUE;

        std::size_t count = 0;
        for (std::size_t i = 0; i < TraitRules::MAX_INTERESTS; ++i)
        {
            uint8_t const id = bot.Interests[i];
            if (!id)
                continue;

            ++count;
            interestsValid &= id <= TraitRules::INTEREST_COUNT && i + 1 == count;
            for (std::size_t j = 0; j < i; ++j)
                interestsValid &= bot.Interests[j] != id;
        }

        interestsValid &= count >= 1 && count <= TraitRules::MAX_INTERESTS;
    }

    Check(valuesInRange, "every value within -100..100");
    Check(interestsValid, "one to three distinct valid interests, packed first");
}

void CheckCounts(std::vector<BotTraits> const& bots)
{
    double noticeable = 0.0;
    double strong = 0.0;
    for (BotTraits const& bot : bots)
        for (int8_t const value : bot.Values)
        {
            noticeable += std::abs(value) >= TraitRules::NOTICEABLE;
            strong += std::abs(value) >= TraitRules::STRONG;
        }

    noticeable /= bots.size();
    strong /= bots.size();
    std::cout << "per bot: " << noticeable << " noticeable, " << strong << " strong (" << bots.size() << " bots)\n";
    Check(noticeable >= NOTICEABLE_MIN && noticeable <= NOTICEABLE_MAX, "about 15 noticeable traits per bot");
    Check(strong >= STRONG_MIN && strong <= STRONG_MAX, "4-5 strong traits per bot");

    // Drive has no shift: its spread is the plain sigma, clipped
    double const mean = Mean(bots, Trait::Drive);
    double squares = 0.0;
    for (BotTraits const& bot : bots)
        squares += (bot.Values[Index(Trait::Drive)] - mean) * (bot.Values[Index(Trait::Drive)] - mean);

    double const spread = std::sqrt(squares / bots.size());
    std::cout << "drive (no shift): mean " << mean << ", spread " << spread << "\n";
    Check(spread >= SPREAD_MIN && spread <= SPREAD_MAX, "an unshifted trait keeps sigma about 45");
}

void CheckPriests()
{
    auto const shares = [](uint8_t const* races, std::size_t raceCount, double& devout, double& worldly)
    {
        devout = 0.0;
        worldly = 0.0;
        for (uint32_t i = 0; i < PRIESTS; ++i)
        {
            int const piety =
                TraitRules::Roll(PRIEST_GUIDS + i, 1, races[i % raceCount], PRIEST).Values[Index(Trait::Piety)];
            devout += piety >= TraitRules::NOTICEABLE;
            worldly += piety <= -TraitRules::NOTICEABLE;
        }

        devout /= PRIESTS;
        worldly /= PRIESTS;
    };

    double devout = 0.0;
    double worldly = 0.0;
    shares(&HUMAN, 1, devout, worldly);
    std::cout << "human priests: " << devout << " devout, " << worldly << " worldly\n";
    Check(devout >= DEVOUT_MIN && devout <= DEVOUT_MAX, "about two in three priests devout");
    Check(worldly >= WORLDLY_MIN && worldly <= WORLDLY_MAX, "about one priest in twenty worldly");

    double allDevout = 0.0;
    double allWorldly = 0.0;
    shares(PRIEST_RACES, std::size(PRIEST_RACES), allDevout, allWorldly);
    std::cout << "priests of every priest race: " << allDevout << " devout, " << allWorldly << " worldly\n";
    Check(allDevout >= devout && allDevout <= ALL_RACES_DEVOUT_MAX, "race shifts add a few devout priests");
    Check(allWorldly >= ALL_RACES_WORLDLY_MIN, "still the occasional worldly priest");
}

void CheckShift(std::vector<BotTraits> const& bots, uint8_t race, uint8_t cls, Trait trait, int shift,
                std::string const& name)
{
    double const difference = Mean(Group(race, cls), trait) - Mean(bots, trait);
    std::cout << name << ": " << std::showpos << difference << std::noshowpos << " against all bots\n";
    Check(difference * (shift > 0 ? 1 : -1) >= SHIFT_SHARE * std::abs(shift), name);
}

void CheckShifts(std::vector<BotTraits> const& bots)
{
    CheckShift(bots, 0, PALADIN, Trait::Piety, DEFINING, "paladins devout");
    CheckShift(bots, 0, PRIEST, Trait::Piety, DEFINING, "priests devout");
    CheckShift(bots, FORSAKEN, 0, Trait::Outlook, -DEFINING, "Forsaken gloomy");
    CheckShift(bots, FORSAKEN, 0, Trait::Suspicion, DEFINING, "Forsaken suspicious");
    CheckShift(bots, GNOME, 0, Trait::Curiosity, DEFINING, "gnomes curious");
    CheckShift(bots, GNOME, 0, Trait::Lore, DEFINING, "gnomes bookish");
    CheckShift(bots, TAUREN, 0, Trait::Rootedness, DEFINING, "tauren rooted");
    CheckShift(bots, TAUREN, 0, Trait::Piety, MARKED, "tauren devout");
    CheckShift(bots, ORC, 0, Trait::FactionZeal, DEFINING, "orcs zealous");
    CheckShift(bots, 0, DEATH_KNIGHT, Trait::Worldliness, DEFINING, "death knights seasoned");
    CheckShift(bots, 0, DEATH_KNIGHT, Trait::Warmth, -DEFINING, "death knights guarded");
    CheckShift(bots, 0, ROGUE, Trait::Mischief, DEFINING, "rogues mischievous");
    CheckShift(bots, 0, MAGE, Trait::Lore, DEFINING, "mages bookish");
    CheckShift(bots, 0, HUNTER, Trait::Rootedness, -DEFINING, "hunters wanderers");
}

// Pearson r of two traits; minusCentre removes each bot's race and class centre, which leaves the rule's own
// correlation
double Pearson(std::vector<BotTraits> const& bots, Trait a, Trait b, bool minusCentre)
{
    double sumA = 0.0;
    double sumB = 0.0;
    double sumAA = 0.0;
    double sumBB = 0.0;
    double sumAB = 0.0;
    uint32_t guid = 1;
    for (BotTraits const& bot : bots)
    {
        double x = bot.Values[Index(a)];
        double y = bot.Values[Index(b)];
        if (minusCentre)
        {
            auto const centres = TraitRules::Centres(RACES[guid % KINDS], CLASSES[(guid / KINDS) % KINDS]);
            x -= centres[Index(a)];
            y -= centres[Index(b)];
        }

        ++guid;
        sumA += x;
        sumB += y;
        sumAA += x * x;
        sumBB += y * y;
        sumAB += x * y;
    }

    double const n = static_cast<double>(bots.size());
    double const covariance = sumAB / n - (sumA / n) * (sumB / n);
    double const varianceA = sumAA / n - (sumA / n) * (sumA / n);
    double const varianceB = sumBB / n - (sumB / n) * (sumB / n);
    return covariance / std::sqrt(varianceA * varianceB);
}

void CheckCorrelation(std::vector<BotTraits> const& bots, Trait a, Trait b, double expected, std::string const& name)
{
    double const rule = Pearson(bots, a, b, true);
    double const population = Pearson(bots, a, b, false);
    std::cout << name << ": r " << rule << " (population with shifts " << population << ")\n";
    Check(std::abs(rule - expected) <= CORRELATION_TOLERANCE, name);
}

void CheckCorrelations(std::vector<BotTraits> const& bots)
{
    CheckCorrelation(bots, Trait::FactionZeal, Trait::Aggression, TraitRules::CORRELATION, "zeal with aggression");
    CheckCorrelation(bots, Trait::Warmth, Trait::Talkativeness, TraitRules::CORRELATION, "warmth with talkativeness");
    CheckCorrelation(bots, Trait::Warmth, Trait::Sociability, TraitRules::CORRELATION, "warmth with sociability");
    CheckCorrelation(bots, Trait::Patience, Trait::Courtesy, TraitRules::CORRELATION, "patience with courtesy");
    CheckCorrelation(bots, Trait::Curiosity, Trait::Rootedness, -TraitRules::CORRELATION,
                     "curiosity against rootedness");
    CheckCorrelation(bots, Trait::Drive, Trait::Greed, 0.0, "unpaired traits independent");
}

bool CleanWord(std::string_view word)
{
    if (word.empty())
        return false;

    for (char const c : word)
        if (c >= '0' && c <= '9')
            return false;

    for (std::string_view const banned : {"skill", "pvp", "stat", "bot", "build", "%"})
        if (word.find(banned) != std::string_view::npos)
            return false;

    return true;
}

void CheckWords(std::vector<BotTraits> const& bots)
{
    bool clean = true;
    for (TraitRules::TraitWords const& words : TraitRules::WORDS)
    {
        clean &= !words.Name.empty();
        for (std::string_view const word : {words.Low[0], words.Low[1], words.High[0], words.High[1]})
            clean &= CleanWord(word);
    }

    for (std::string_view const name : TraitRules::INTEREST_NAMES)
        clean &= CleanWord(name);

    Check(clean, "every trait, pole and intensity has a clean in-world word");

    bool sorted = true;
    bool counted = true;
    for (BotTraits const& bot : bots)
    {
        std::vector<std::string> const words = TraitRules::Describe(bot);
        std::vector<int> magnitudes;
        for (int8_t const value : bot.Values)
            if (TraitRules::IsNoticeable(value))
                magnitudes.push_back(std::abs(value));

        counted &= words.size() == magnitudes.size();
        std::sort(magnitudes.begin(), magnitudes.end(), std::greater<int>());
        // The k-th word must be the word of a trait as strong as the k-th magnitude
        for (std::size_t k = 0; k < words.size() && k < magnitudes.size(); ++k)
        {
            bool found = false;
            for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
                found |= std::abs(bot.Values[i]) == magnitudes[k] &&
                         TraitRules::Word(static_cast<Trait>(i), bot.Values[i]) == words[k];

            sorted &= found;
        }
    }

    Check(counted, "the words count the noticeable traits");
    Check(sorted, "the words are strongest first");

    BotTraits mild;
    mild.Values[Index(Trait::Caution)] = TraitRules::NOTICEABLE;
    mild.Values[Index(Trait::Greed)] = -TraitRules::STRONG;
    mild.Values[Index(Trait::Piety)] = TraitRules::NOTICEABLE - 1;
    std::vector<std::string> const words = TraitRules::Describe(mild);
    Check(words.size() == 2 && words[0] == "very content" && words[1] == "somewhat careful",
          "thresholds: 30 noticeable, 65 strong, 29 silent");

    // LC9: four grades at their bounds, negatives mirror, no digit in any word
    struct Bound
    {
        int Value;
        char const* Caution;     // "very careful" / "extremely careful" style trait
        char const* Aggression;  // a trait with special strong words
    };
    constexpr Bound BOUNDS[] = {{29, "", ""},
                                {30, "somewhat careful", "somewhat combative"},
                                {44, "somewhat careful", "somewhat combative"},
                                {45, "rather careful", "rather combative"},
                                {64, "rather careful", "rather combative"},
                                {65, "very careful", "belligerent"},
                                {84, "very careful", "belligerent"},
                                {85, "extremely careful", "belligerent"},
                                {100, "extremely careful", "belligerent"}};
    bool grades = true;
    bool mirrored = true;
    bool wordsClean = true;
    for (Bound const& bound : BOUNDS)
    {
        grades &= TraitRules::Word(Trait::Caution, bound.Value) == bound.Caution;
        grades &= TraitRules::Word(Trait::Aggression, bound.Value) == bound.Aggression;
        mirrored &= TraitRules::Word(Trait::Caution, -bound.Value).empty() == (std::string_view(bound.Caution).empty());
        mirrored &=
            TraitRules::Word(Trait::Aggression, -bound.Value).empty() == (std::string_view(bound.Aggression).empty());
        for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
            for (int const sign : {-1, 1})
                wordsClean &= bound.Value < TraitRules::NOTICEABLE ||
                              CleanWord(TraitRules::Word(static_cast<Trait>(i), sign * bound.Value));
    }

    Check(grades, "grades: somewhat 30-44, rather 45-64, very 65-84, extremely 85 and up; special words stay");
    Check(mirrored, "a negative value gets a word exactly where the positive does");
    Check(TraitRules::Word(Trait::Caution, -85) == "reckless" && TraitRules::Word(Trait::Caution, -65) == "reckless",
          "the strong pole's special word stays");
    Check(wordsClean, "no word of any trait and grade holds a digit or a banned term");
    Check(TraitRules::Unit(mild, Trait::Greed) == -0.65f, "the accessor maps -65 to -0.65");

    Trait found = Trait::Drive;
    Check(TraitRules::FindTrait("zeal", found) && found == Trait::FactionZeal, "trait names resolve");
    Check(!TraitRules::FindTrait("skill", found), "unknown trait names do not");
}

void CheckInterests(std::vector<BotTraits> const& bots)
{
    double all = 0.0;
    for (BotTraits const& bot : bots)
        all += HasInterest(bot, TraitRules::Interest::InnsAndAle);

    all /= bots.size();
    double dwarves = 0.0;
    std::vector<BotTraits> const dwarfBots = Group(DWARF, 0);
    for (BotTraits const& bot : dwarfBots)
        dwarves += HasInterest(bot, TraitRules::Interest::InnsAndAle);

    dwarves /= dwarfBots.size();
    std::cout << "inns and ale: dwarves " << dwarves << ", all bots " << all << "\n";
    Check(dwarves >= all * DWARF_INN_FACTOR, "dwarves fonder of inns and ale");
}

struct SampleBot
{
    uint8_t Race;
    uint8_t Class;
};

std::string_view RaceName(uint8_t race)
{
    constexpr std::string_view names[] = {"",       "Human", "Orc",   "Dwarf", "Night Elf", "Forsaken",
                                          "Tauren", "Gnome", "Troll", "",      "Blood Elf", "Draenei"};
    return race < std::size(names) ? names[race] : "";
}

std::string_view ClassName(uint8_t cls)
{
    constexpr std::string_view names[] = {"",       "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "Death Knight",
                                          "Shaman", "Mage",    "Warlock", "",       "Druid"};
    return cls < std::size(names) ? names[cls] : "";
}

// LC15c: every trait has exactly one of the four areas, the bridge's groups of LC15 (talk, others, work, wants), and
// the words' areas are as many as the words, in the same order
void CheckAreas(std::vector<BotTraits> const& bots)
{
    struct Group
    {
        std::string_view Area;
        std::vector<Trait> Members;
    };
    std::vector<Group> const groups = {
        {"talk", {Trait::Talkativeness, Trait::Register, Trait::Humour, Trait::Theatricality, Trait::Confidence}},
        {"others",
         {Trait::Sociability, Trait::Warmth, Trait::Courtesy, Trait::Helpfulness, Trait::Generosity, Trait::Loyalty,
          Trait::Suspicion, Trait::Leadership, Trait::Mischief}},
        {"work",
         {Trait::Caution, Trait::Aggression, Trait::Patience, Trait::Drive, Trait::Competitiveness, Trait::Thoroughness,
          Trait::Orderliness}},
        {"wants",
         {Trait::Greed, Trait::Thrift, Trait::Piety, Trait::FactionZeal, Trait::Lore, Trait::Curiosity,
          Trait::Rootedness, Trait::Outlook, Trait::Worldliness}},
    };

    std::size_t covered = 0;
    bool grouped = true;
    for (Group const& group : groups)
        for (Trait const trait : group.Members)
        {
            ++covered;
            grouped &= TraitRules::AreaOf(trait) == group.Area;
        }

    Check(covered == TraitRules::TRAIT_COUNT, "the area table names each of the 30 traits exactly once");
    Check(grouped, "every trait has the area of its group: talk, others, work, wants");

    bool aligned = true;
    bool known = true;
    for (BotTraits const& bot : bots)
    {
        std::vector<std::string> const words = TraitRules::Describe(bot);
        std::vector<std::string> const areas = TraitRules::DescribeAreas(bot);
        aligned &= areas.size() == words.size();
        for (std::size_t k = 0; k < words.size() && k < areas.size(); ++k)
        {
            bool match = false;
            for (std::size_t i = 0; i < TraitRules::TRAIT_COUNT; ++i)
                match |= TraitRules::Word(static_cast<Trait>(i), bot.Values[i]) == words[k] &&
                         TraitRules::AreaOf(static_cast<Trait>(i)) == areas[k];

            known &= match;
            known &= std::find(TraitRules::AREA_NAMES.begin(), TraitRules::AREA_NAMES.end(), areas[k]) !=
                     TraitRules::AREA_NAMES.end();
        }
    }

    Check(aligned, "areas are as long as words");
    Check(known, "the k-th area is the area of the k-th word's trait and one of the four names");
}

void PrintSample()
{
    // Valid WotLK combinations, spread over races and classes
    constexpr SampleBot sample[] = {{1, 2}, {2, 1}, {3, 3}, {4, 11}, {5, 5}, {6, 7},  {7, 8}, {8, 4}, {10, 9}, {11, 2},
                                    {1, 5}, {2, 7}, {3, 2}, {4, 4},  {5, 9}, {6, 11}, {7, 4}, {8, 5}, {10, 6}, {11, 8}};
    std::cout << "\nSample of 20 personalities (guids " << SAMPLE_FIRST_GUID << "-" << SAMPLE_FIRST_GUID + 19
              << ", version 1), as the LLM sees them:\n";
    uint32_t guid = SAMPLE_FIRST_GUID;
    for (SampleBot const& bot : sample)
    {
        BotTraits const traits = TraitRules::Roll(guid++, 1, bot.Race, bot.Class);
        std::string words;
        for (std::string const& word : TraitRules::Describe(traits))
            words += (words.empty() ? "" : ", ") + word;

        std::string interests;
        for (std::string const& name : TraitRules::InterestNames(traits))
            interests += (interests.empty() ? "" : ", ") + name;

        std::cout << RaceName(bot.Race) << " " << ClassName(bot.Class) << " | " << words << " | " << interests << "\n";
    }

    std::cout << "\n";
}
}  // namespace

int main()
{
    std::cout << std::fixed << std::setprecision(3);
    std::vector<BotTraits> const bots = Population();
    CheckDeterminism();
    CheckRanges(bots);
    CheckCounts(bots);
    CheckPriests();
    CheckShifts(bots);
    CheckCorrelations(bots);
    CheckWords(bots);
    CheckInterests(bots);
    CheckAreas(bots);
    PrintSample();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
