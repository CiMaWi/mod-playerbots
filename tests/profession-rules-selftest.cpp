/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the profession rules (src/Ai/World/Rpg/ProfessionRules.h, professions plan WR2): prints the share
// of each pair and profession per class and for the roster's class mix, and checks the rules of WR.2 and WR.7: never
// more than two main professions, no craft outside the classes it serves, no craft without its gatherer (tailoring and
// enchanting excepted), no tailor with first aid, the same seed gives the same person, a bot's held professions become
// its person whatever they are.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ProfessionRules.h"

using ProfessionRules::BotProfessions;
using ProfessionRules::Skill;

namespace
{
struct ClassRow
{
    uint8_t Id;
    char const* Name;
    std::vector<uint8_t> Races;  // the races that can be the class (3.3.5)
};

std::vector<ClassRow> const CLASSES = {
    {ProfessionRules::WARRIOR, "warrior", {1, 2, 3, 4, 5, 6, 7, 8, 11}},
    {ProfessionRules::PALADIN, "paladin", {1, 3, 10, 11}},
    {ProfessionRules::HUNTER, "hunter", {2, 3, 4, 6, 8, 10, 11}},
    {ProfessionRules::ROGUE, "rogue", {1, 2, 3, 4, 5, 7, 8, 10}},
    {ProfessionRules::PRIEST, "priest", {1, 3, 4, 5, 8, 10, 11}},
    {ProfessionRules::DEATH_KNIGHT, "deathknight", {1, 2, 3, 4, 5, 6, 7, 8, 10, 11}},
    {ProfessionRules::SHAMAN, "shaman", {2, 6, 8, 11}},
    {ProfessionRules::MAGE, "mage", {1, 5, 7, 8, 10, 11}},
    {ProfessionRules::WARLOCK, "warlock", {1, 2, 5, 7, 10}},
    {ProfessionRules::DRUID, "druid", {4, 6}},
};

// The roster's class mix: lab12's pool (250 bots of each class, 2026-11-18); Population's ResizeClass keeps the roster
// even by class, so every class weighs the same
constexpr uint32_t ROSTER_PER_CLASS = 250;
constexpr uint32_t BOTS_PER_CLASS = 20000;
constexpr double EVEN_TOLERANCE = 0.2;  // a pair's share within +-20 % of even (sampling error is about 3 %)
constexpr double ONE_MAIN_MIN = 1.0;
constexpr double ONE_MAIN_MAX = 15.0;
constexpr uint32_t REROLL_GUIDS = 1000;

uint32_t checks = 0;
uint32_t failures = 0;

void Check(bool ok, std::string const& what)
{
    ++checks;
    if (ok)
        return;

    ++failures;
    std::cout << "FAIL: " << what << "\n";
}

std::string PairName(Skill first, Skill second)
{
    return std::string(ProfessionRules::Name(first)) + " + " + std::string(ProfessionRules::Name(second));
}

std::string Percent(double share)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << share;
    return text.str();
}

bool Same(BotProfessions const& a, BotProfessions const& b)
{
    return a.Mains == b.Mains && a.Secondary == b.Secondary && a.Version == b.Version && a.From == b.From;
}

struct ClassTally
{
    std::map<std::pair<Skill, Skill>, uint32_t> Pairs;
    std::map<Skill, uint32_t> Professions;
    uint32_t OneMain = 0;
    uint32_t Bots = 0;
    uint32_t FishingInterest = 0;
    uint32_t FishingWithInterest = 0;
    uint32_t FishingWithoutInterest = 0;
};

// Each class rolls its own guids, so the classes' samples are independent
ClassTally RollClass(ClassRow const& row, uint32_t firstGuid)
{
    ClassTally tally;
    for (uint32_t guid = firstGuid; guid < firstGuid + BOTS_PER_CLASS; ++guid)
    {
        uint8_t const race = row.Races[guid % row.Races.size()];
        TraitRules::BotTraits const traits = TraitRules::Roll(guid, 1, race, row.Id);
        BotProfessions const person = ProfessionRules::Roll(guid, 1, row.Id, traits);
        ++tally.Bots;
        std::string const violation = ProfessionRules::Violation(person, row.Id);
        if (!violation.empty())
            Check(false, std::string(row.Name) + " guid " + std::to_string(guid) + ": " + violation);

        std::size_t const mains = ProfessionRules::MainCount(person);
        if (mains == 2)
            ++tally.Pairs[{person.Mains[0], person.Mains[1]}];
        else if (mains == 1)
            ++tally.OneMain;
        else
            Check(false, std::string(row.Name) + " guid " + std::to_string(guid) + ": no main profession");

        for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
            if (ProfessionRules::Has(person, info.Which))
                ++tally.Professions[info.Which];

        bool const fishing = (person.Secondary & ProfessionRules::SECONDARY_FISHING) != 0;
        if (ProfessionRules::HasInterest(traits, TraitRules::Interest::Fishing))
        {
            ++tally.FishingInterest;
            tally.FishingWithInterest += fishing ? 1 : 0;
        }
        else
            tally.FishingWithoutInterest += fishing ? 1 : 0;
    }

    return tally;
}

void CheckAdopt()
{
    TraitRules::BotTraits const traits = TraitRules::Roll(77, 1, 1, ProfessionRules::WARRIOR);
    BotProfessions const rolled = ProfessionRules::Roll(77, 1, ProfessionRules::WARRIOR, traits);

    // A warrior with a pair no rule would give it: it keeps it, gatherer first
    BotProfessions const odd = ProfessionRules::Adopt(rolled, {Skill::Tailoring, Skill::Skinning, Skill::Fishing});
    Check(odd.Mains[0] == Skill::Skinning && odd.Mains[1] == Skill::Tailoring,
          "held tailoring and skinning become the mains, skinning first");
    Check(odd.Secondary == ProfessionRules::SECONDARY_FISHING, "held fishing becomes the only secondary");
    Check(odd.From == ProfessionRules::Origin::Held, "a held person is marked held");

    // A craft without its gatherer stays as it is
    BotProfessions const lone = ProfessionRules::Adopt(rolled, {Skill::Blacksmithing});
    Check(lone.Mains[0] == Skill::Blacksmithing && lone.Mains[1] == Skill::None,
          "held blacksmithing alone stays alone");
    Check(lone.Secondary == rolled.Secondary, "no held secondary: the rolled ones");

    // More than two: the gatherers first, two of them; the rest stays held, not the person
    BotProfessions const many = ProfessionRules::Adopt(rolled, {Skill::Alchemy, Skill::Herbalism, Skill::Mining});
    Check(many.Mains[0] == Skill::Mining && many.Mains[1] == Skill::Herbalism, "three held: the two gatherers");

    // Only secondaries held: the mains are rolled, the secondaries held
    BotProfessions const secondaries =
        ProfessionRules::Adopt(rolled, {Skill::FirstAid, Skill::Fishing, Skill::Cooking});
    Check(secondaries.Mains == rolled.Mains, "no held main: the rolled mains");
    Check(secondaries.Secondary == (ProfessionRules::SECONDARY_FIRST_AID | ProfessionRules::SECONDARY_FISHING |
                                    ProfessionRules::SECONDARY_COOKING),
          "all three held secondaries kept");

    // Nothing held: the roll itself
    Check(Same(ProfessionRules::Adopt(rolled, {}), rolled), "nothing held: the rolled person");
}

void CheckWords()
{
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
    {
        Skill found = Skill::None;
        Check(ProfessionRules::FindByName(info.Name, found) && found == info.Which,
              "the word " + std::string(info.Name) + " finds its profession");
    }

    Skill none = Skill::None;
    Check(!ProfessionRules::FindByName("lockpicking", none), "an unknown word finds nothing");
}

void CheckDeterminism()
{
    uint32_t changed = 0;
    for (uint32_t guid = 1; guid <= REROLL_GUIDS; ++guid)
    {
        TraitRules::BotTraits const traits = TraitRules::Roll(guid, 1, 1, ProfessionRules::WARRIOR);
        BotProfessions const first = ProfessionRules::Roll(guid, 1, ProfessionRules::WARRIOR, traits);
        BotProfessions const again = ProfessionRules::Roll(guid, 1, ProfessionRules::WARRIOR, traits);
        BotProfessions const next = ProfessionRules::Roll(guid, 2, ProfessionRules::WARRIOR, traits);
        if (!Same(first, again))
            Check(false, "guid " + std::to_string(guid) + ": the same seed rolls another person");

        changed += first.Mains != next.Mains ? 1 : 0;
    }

    Check(changed * 2 > REROLL_GUIDS, "a new version changes most bots' mains");
}
}  // namespace

int main()
{
    std::cout << "PROFESSIONS selftest (ProfessionRules.h)\n";
    std::cout << "Class mix: the roster's, " << ROSTER_PER_CLASS
              << " bots of each of the ten classes (lab12's pool); each class rolled for " << BOTS_PER_CLASS
              << " guids of its own (first roll, the class's races in turn)\n\n";

    std::vector<ClassTally> tallies;
    for (std::size_t c = 0; c < CLASSES.size(); ++c)
        tallies.push_back(RollClass(CLASSES[c], 1 + static_cast<uint32_t>(c) * BOTS_PER_CLASS));

    std::cout << "Pairs per class: share of the class's bots with two mains (even = 100 / pairs)\n";
    std::map<std::pair<Skill, Skill>, uint32_t> roster;
    uint32_t rosterTwo = 0;
    for (std::size_t c = 0; c < CLASSES.size(); ++c)
    {
        ClassRow const& row = CLASSES[c];
        ClassTally const& tally = tallies[c];
        std::vector<ProfessionRules::Pair> const pairs = ProfessionRules::OpenPairs(row.Id);
        uint32_t const two = tally.Bots - tally.OneMain;
        double const even = 100.0 / pairs.size();
        double const oneMain = 100.0 * tally.OneMain / tally.Bots;
        std::cout << row.Name << ": " << pairs.size() << " pairs, even " << Percent(even) << " %; one main only "
                  << Percent(oneMain) << " % of all\n";
        Check(oneMain >= ONE_MAIN_MIN && oneMain <= ONE_MAIN_MAX, std::string(row.Name) + ": a small share one main");
        bool craftPair = false;
        for (ProfessionRules::Pair const& pair : pairs)
        {
            auto const it = tally.Pairs.find({pair.First, pair.Second});
            uint32_t const count = it == tally.Pairs.end() ? 0 : it->second;
            double const share = 100.0 * count / two;
            std::cout << "  " << std::left << std::setw(32) << PairName(pair.First, pair.Second) << std::right
                      << std::setw(6) << Percent(share) << "\n";
            Check(share >= even * (1.0 - EVEN_TOLERANCE) && share <= even * (1.0 + EVEN_TOLERANCE),
                  std::string(row.Name) + ": " + PairName(pair.First, pair.Second) + " near even");
            craftPair = craftPair || ProfessionRules::FindCraft(pair.Second) || ProfessionRules::FindCraft(pair.First);
            roster[{pair.First, pair.Second}] += 0;
        }

        Check(craftPair, std::string(row.Name) + ": a pair with a craft");
        uint32_t seen = 0;
        for (auto const& [pair, count] : tally.Pairs)
        {
            seen += count;
            roster[pair] += static_cast<uint32_t>(static_cast<uint64_t>(count) * ROSTER_PER_CLASS);
        }

        Check(seen == two, std::string(row.Name) + ": every pair rolled is an open pair");
        rosterTwo += two * ROSTER_PER_CLASS;
        Check(tally.FishingWithInterest == tally.FishingInterest,
              std::string(row.Name) + ": every bot with the fishing interest fishes");
        double const without = 100.0 * tally.FishingWithoutInterest / (tally.Bots - tally.FishingInterest);
        Check(without > ProfessionRules::SECONDARY_WITHOUT_INTEREST_PERCENT * 0.7 &&
                  without < ProfessionRules::SECONDARY_WITHOUT_INTEREST_PERCENT * 1.3,
              std::string(row.Name) + ": a small share fishes without the interest");
    }

    std::cout << "\nPairs over the roster (share of the bots with two mains, the class mix weighted)\n";
    std::vector<std::pair<std::pair<Skill, Skill>, uint32_t>> sorted(roster.begin(), roster.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    for (auto const& [pair, count] : sorted)
        std::cout << "  " << std::left << std::setw(32) << PairName(pair.first, pair.second) << std::right
                  << std::setw(6) << Percent(100.0 * count / rosterTwo) << "\n";

    std::cout << "\nProfessions: share of bots that have each (main or secondary), per class and over the roster\n";
    std::cout << std::left << std::setw(15) << "profession" << std::right;
    for (ClassRow const& row : CLASSES)
        std::cout << std::setw(7) << std::string(row.Name).substr(0, 6);

    std::cout << std::setw(8) << "roster" << "\n";
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
    {
        std::cout << std::left << std::setw(15) << info.Name << std::right;
        double sum = 0.0;
        for (ClassTally const& tally : tallies)
        {
            auto const it = tally.Professions.find(info.Which);
            double const share = 100.0 * (it == tally.Professions.end() ? 0 : it->second) / tally.Bots;
            sum += share;
            std::cout << std::setw(7) << Percent(share);
        }

        std::cout << std::setw(8) << Percent(sum / tallies.size()) << "\n";
    }

    // First aid goes mostly to the classes that cannot heal themselves
    double healers = 0.0;
    double others = 0.0;
    for (std::size_t c = 0; c < CLASSES.size(); ++c)
    {
        double const share = 100.0 * tallies[c].Professions[Skill::FirstAid] / tallies[c].Bots;
        if (ProfessionRules::SELF_HEALERS & ProfessionRules::ClassBit(CLASSES[c].Id))
            healers = std::max(healers, share);
        else
            others = others == 0.0 ? share : std::min(others, share);
    }

    Check(others > healers, "first aid: every class that cannot heal itself above every self-healer");

    CheckAdopt();
    CheckWords();
    CheckDeterminism();

    std::cout << "\nSample of 20 persons (guids 1..20, first roll):\n";
    for (uint32_t guid = 1; guid <= 20; ++guid)
    {
        ClassRow const& row = CLASSES[guid % CLASSES.size()];
        TraitRules::BotTraits const traits = TraitRules::Roll(guid, 1, row.Races[0], row.Id);
        std::cout << "  " << std::left << std::setw(12) << row.Name << std::right << " drive " << std::setw(4)
                  << static_cast<int>(traits.Values[TraitRules::Index(TraitRules::Trait::Drive)]) << " | "
                  << ProfessionRules::Describe(ProfessionRules::Roll(guid, 1, row.Id, traits)) << "\n";
    }

    std::cout << "\n" << (checks - failures) << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
