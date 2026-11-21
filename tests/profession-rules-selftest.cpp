/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the profession rules (src/Ai/World/Rpg/ProfessionRules.h, professions plan WR2 and WR2b): prints
// the fitted craft chances per class, the shares of crafts the sampled roster holds beside the global shares, and the
// share of each pair and profession per class, and checks the rules of WR.2 and WR.7: the roster holds the global
// craft shares (within one point, a share no class can reach is named), a bias raises its class's share, a craft a
// class does not consider never appears, two gatherers are never rolled, never more than two main professions, no
// craft without its gatherer (tailoring and enchanting excepted), no tailor with first aid, the same seed gives the
// same person, a bot's held professions become its person whatever they are.
#include <algorithm>
#include <array>
#include <cmath>
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
// even by class, so every class weighs the same (the fit assumes it)
constexpr uint32_t ROSTER_PER_CLASS = 250;
constexpr uint32_t BOTS_PER_CLASS = 20000;
constexpr double ROSTER_TOLERANCE_POINTS = 1.0;  // a craft's roster share within one point of its global share
constexpr double ONE_MAIN_MIN = 1.0;
constexpr double ONE_MAIN_MAX = 15.0;
constexpr uint32_t REROLL_GUIDS = 1000;
constexpr uint32_t PARTNER_GRID = 4000;

using ProfessionRules::ChanceTable;
using ProfessionRules::CRAFT_COUNT;
using ProfessionRules::CRAFTS;
using ProfessionRules::Settings;

uint32_t checks = 0;
uint32_t failures = 0;

// The chances of the default settings (equal shares, hunters' engineering at 2): what the server rolls with
ChanceTable defaultChances{};

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

// A pair without its order where both are crafts (a tailor-and-enchanter is one pair whichever craft was rolled first)
std::pair<Skill, Skill> Unordered(Skill first, Skill second)
{
    if (ProfessionRules::CraftIndex(first) < CRAFT_COUNT && ProfessionRules::CraftIndex(second) < CRAFT_COUNT &&
        ProfessionRules::CraftIndex(first) > ProfessionRules::CraftIndex(second))
        return {second, first};

    return {first, second};
}

struct ClassTally
{
    std::map<std::pair<Skill, Skill>, uint32_t> Pairs;
    std::map<Skill, uint32_t> Professions;
    std::array<uint32_t, CRAFT_COUNT> Crafts{};  // the persons holding each craft as a main
    uint32_t CraftsHeld = 0;                     // the crafts held, a tailor-and-enchanter counted twice
    uint32_t OneMain = 0;
    uint32_t Bots = 0;
    uint32_t FishingInterest = 0;
    uint32_t FishingWithInterest = 0;
    uint32_t FishingWithoutInterest = 0;
};

// Each class rolls its own guids, so the classes' samples are independent
ClassTally RollClass(ClassRow const& row, uint32_t firstGuid, ChanceTable const& chances)
{
    ClassTally tally;
    for (uint32_t guid = firstGuid; guid < firstGuid + BOTS_PER_CLASS; ++guid)
    {
        uint8_t const race = row.Races[guid % row.Races.size()];
        TraitRules::BotTraits const traits = TraitRules::Roll(guid, 1, race, row.Id);
        BotProfessions const person = ProfessionRules::Roll(guid, 1, row.Id, traits, chances);
        ++tally.Bots;
        std::string const violation = ProfessionRules::Violation(person, row.Id);
        if (!violation.empty())
            Check(false, std::string(row.Name) + " guid " + std::to_string(guid) + ": " + violation);

        std::size_t const mains = ProfessionRules::MainCount(person);
        if (mains == 2)
        {
            ++tally.Pairs[Unordered(person.Mains[0], person.Mains[1])];
            if (ProfessionRules::Is(person.Mains[0], ProfessionRules::Kind::Gatherer) &&
                ProfessionRules::Is(person.Mains[1], ProfessionRules::Kind::Gatherer))
                Check(false, std::string(row.Name) + " guid " + std::to_string(guid) + ": two gatherers rolled");
        }
        else if (mains == 1)
            ++tally.OneMain;
        else
            Check(false, std::string(row.Name) + " guid " + std::to_string(guid) + ": no main profession");

        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
            if (ProfessionRules::HasMain(person, CRAFTS[craft].Which))
            {
                ++tally.Crafts[craft];
                ++tally.CraftsHeld;
                if (!ProfessionRules::Serves(CRAFTS[craft], row.Id))
                    Check(false, std::string(row.Name) + " guid " + std::to_string(guid) + ": rolled " +
                                     std::string(ProfessionRules::Name(CRAFTS[craft].Which)) +
                                     ", a craft it does not consider");
            }

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

std::vector<ClassTally> RollRoster(ChanceTable const& chances)
{
    std::vector<ClassTally> tallies;
    for (std::size_t c = 0; c < CLASSES.size(); ++c)
        tallies.push_back(RollClass(CLASSES[c], 1 + static_cast<uint32_t>(c) * BOTS_PER_CLASS, chances));

    return tallies;
}

// The roster's share of each craft among the crafts held, in percent (every class weighs the same)
std::array<double, CRAFT_COUNT> RosterShares(std::vector<ClassTally> const& tallies)
{
    std::array<double, CRAFT_COUNT> shares{};
    double held = 0.0;
    for (ClassTally const& tally : tallies)
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        {
            shares[craft] += tally.Crafts[craft];
            held += tally.Crafts[craft];
        }

    for (double& share : shares)
        share = held > 0.0 ? 100.0 * share / held : 0.0;

    return shares;
}

std::string UnreachableWords(ProfessionRules::FitResult const& fit)
{
    std::string words;
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        if (fit.Unreachable & (1u << craft))
            words += (words.empty() ? "" : ", ") + std::string(ProfessionRules::Name(CRAFTS[craft].Which));

    return words.empty() ? "none" : words;
}

void PrintFit(std::string const& title, ProfessionRules::FitResult const& fit)
{
    std::cout << title << ": " << (fit.Converged ? "converged" : "not converged") << " after " << fit.Iterations
              << " iterations (bound " << ProfessionRules::FIT_MAX_ITERATIONS
              << "), shares no class can reach: " << UnreachableWords(fit) << "\n";
}

// The chance of each craft per class (percent of the class's rolls), beside the global share and what the roster holds
void PrintChances(ProfessionRules::FitResult const& fit)
{
    std::cout << std::left << std::setw(15) << "craft" << std::right;
    for (ClassRow const& row : CLASSES)
        std::cout << std::setw(7) << std::string(row.Name).substr(0, 6);

    std::cout << std::setw(9) << "global" << std::setw(9) << "reached" << "\n";
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
    {
        std::cout << std::left << std::setw(15) << ProfessionRules::Name(CRAFTS[craft].Which) << std::right;
        for (std::size_t slot = 0; slot < ProfessionRules::CLASS_COUNT; ++slot)
            std::cout << std::setw(7)
                      << (ProfessionRules::Serves(CRAFTS[craft], ProfessionRules::CLASS_IDS[slot])
                              ? Percent(100.0 * fit.Chance[ProfessionRules::Cell(slot, craft)])
                              : std::string("-"));

        std::cout << std::setw(9) << Percent(100.0 * fit.Target[craft]) << std::setw(9)
                  << Percent(100.0 * fit.Reached[craft]) << "\n";
    }
}

// The share of each craft among the crafts the sampled roster holds, per class, beside the roster's and the global
void PrintRolledCrafts(std::vector<ClassTally> const& tallies, std::array<double, CRAFT_COUNT> const& roster,
                       ProfessionRules::FitResult const& fit)
{
    std::cout << std::left << std::setw(15) << "craft" << std::right;
    for (ClassRow const& row : CLASSES)
        std::cout << std::setw(7) << std::string(row.Name).substr(0, 6);

    std::cout << std::setw(9) << "roster" << std::setw(9) << "global" << "\n";
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
    {
        std::cout << std::left << std::setw(15) << ProfessionRules::Name(CRAFTS[craft].Which) << std::right;
        for (ClassTally const& tally : tallies)
            std::cout << std::setw(7) << Percent(100.0 * tally.Crafts[craft] / tally.CraftsHeld);

        std::cout << std::setw(9) << Percent(roster[craft]) << std::setw(9) << Percent(100.0 * fit.Target[craft])
                  << "\n";
    }
}

// The roster of a settings table holds the global shares: the fit converges, or names what it could not reach
void CheckRoster(std::string const& name, Settings const& settings, bool print)
{
    ProfessionRules::FitResult const fit = ProfessionRules::FitChances(settings);
    std::vector<ClassTally> const tallies = RollRoster(fit.Chance);
    std::array<double, CRAFT_COUNT> const roster = RosterShares(tallies);
    if (print)
    {
        PrintFit(name, fit);
        std::cout << "\nFitted chances (percent of the class's rolls; the first craft of a bot is rolled from these)\n";
        PrintChances(fit);
        std::cout << "\nCrafts held in the sampled roster (share of the crafts held, per class and over the roster;"
                  << " the roster holds " << ROSTER_PER_CLASS << " bots of each class, sampled " << BOTS_PER_CLASS
                  << " per class)\n";
        PrintRolledCrafts(tallies, roster, fit);
    }

    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
    {
        double const gap = std::fabs(roster[craft] - 100.0 * fit.Target[craft]);
        std::string const craftName(ProfessionRules::Name(CRAFTS[craft].Which));
        if (fit.Unreachable & (1u << craft))
            continue;  // reported, not a failure: the fit says which share no class can reach

        Check(gap <= ROSTER_TOLERANCE_POINTS, name + ": the roster's share of " + craftName + " is " +
                                                  Percent(roster[craft]) + ", the global one " +
                                                  Percent(100.0 * fit.Target[craft]) + " (within one point)");
    }
}

// The fitted table keeps its rules: each class's served chances sum to 1, nothing outside the class's table
void CheckChanceTable(std::string const& name, ProfessionRules::FitResult const& fit)
{
    for (std::size_t slot = 0; slot < ProfessionRules::CLASS_COUNT; ++slot)
    {
        double sum = 0.0;
        for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        {
            double const chance = fit.Chance[ProfessionRules::Cell(slot, craft)];
            bool const served = ProfessionRules::Serves(CRAFTS[craft], ProfessionRules::CLASS_IDS[slot]);
            sum += chance;
            Check(served || chance == 0.0, name + ": a craft a class does not consider has chance 0");
        }

        Check(std::fabs(sum - 1.0) < 1e-9, name + ": a class's chances sum to 1");
    }
}

void CheckFit()
{
    Settings const defaults = ProfessionRules::DefaultSettings();
    ProfessionRules::FitResult const fit = ProfessionRules::FitChances(defaults);
    Check(fit.Converged && fit.Iterations < ProfessionRules::FIT_MAX_ITERATIONS, "the default fit converges");
    Check(!fit.Unreachable, "the default shares are all reachable");
    CheckChanceTable("default fit", fit);
    for (std::size_t craft = 0; craft < CRAFT_COUNT; ++craft)
        Check(std::fabs(fit.Reached[craft] - 1.0 / CRAFT_COUNT) < 1e-6, "the default fit holds equal shares");

    // A bias raises its class's share, and since the shares are global it lowers the others' (WR.2)
    std::size_t const hunter = static_cast<std::size_t>(ProfessionRules::ClassSlot(ProfessionRules::HUNTER));
    std::size_t const engineering = ProfessionRules::CraftIndex(Skill::Engineering);
    Settings neutral = defaults;
    neutral.Bias[ProfessionRules::Cell(hunter, engineering)] = 1.0f;
    Settings strong = defaults;
    strong.Bias[ProfessionRules::Cell(hunter, engineering)] = 4.0f;
    ProfessionRules::FitResult const neutralFit = ProfessionRules::FitChances(neutral);
    ProfessionRules::FitResult const strongFit = ProfessionRules::FitChances(strong);
    double const lean = fit.Chance[ProfessionRules::Cell(hunter, engineering)];
    double const flat = neutralFit.Chance[ProfessionRules::Cell(hunter, engineering)];
    double const heavy = strongFit.Chance[ProfessionRules::Cell(hunter, engineering)];
    std::cout << "Bias check: hunters' chance of engineering " << Percent(100.0 * flat) << " % at bias 1, "
              << Percent(100.0 * lean) << " % at 2, " << Percent(100.0 * heavy) << " % at 4\n";
    Check(flat < lean && lean < heavy, "a bias raises its class's chance of the craft");
    std::size_t const warrior = static_cast<std::size_t>(ProfessionRules::ClassSlot(ProfessionRules::WARRIOR));
    Check(neutralFit.Chance[ProfessionRules::Cell(warrior, engineering)] >
              fit.Chance[ProfessionRules::Cell(warrior, engineering)],
          "the hunters' lean lowers the other classes' chance of the craft (the share is global)");
    Check(std::fabs(neutralFit.Reached[engineering] - fit.Reached[engineering]) < 1e-6 &&
              std::fabs(strongFit.Reached[engineering] - fit.Reached[engineering]) < 1e-6,
          "a bias does not move the global share");

    // The sampled hunters: the bias shows in the rolls too
    std::size_t const hunterRow = static_cast<std::size_t>(ProfessionRules::ClassSlot(ProfessionRules::HUNTER));
    ClassTally const leanTally = RollClass(CLASSES[hunterRow], 1, fit.Chance);
    ClassTally const flatTally = RollClass(CLASSES[hunterRow], 1, neutralFit.Chance);
    Check(leanTally.Crafts[engineering] > flatTally.Crafts[engineering], "hunters roll engineering more at bias 2");

    // A share no class can reach is reported, not hidden: blacksmithing is open to three classes of ten, so 50 % of
    // all crafts held is out of reach
    Settings heavySmith = defaults;
    heavySmith.Share[ProfessionRules::CraftIndex(Skill::Blacksmithing)] = 87.5f;
    ProfessionRules::FitResult const smithFit = ProfessionRules::FitChances(heavySmith);
    std::cout << "Unreachable check (blacksmithing weight " << heavySmith.Share[0] << " against 12.5 for the rest, "
              << Percent(100.0 * smithFit.Target[0]) << " %): ";
    PrintFit("fit", smithFit);
    Check(smithFit.Unreachable == 1u && smithFit.Reached[0] < smithFit.Target[0],
          "an unreachable blacksmithing share is named, and only it, and falls short");
    Check(smithFit.Reached[0] > 0.25,
          "an unreachable share still gives the classes that consider it as much as they can");
    CheckChanceTable("unreachable fit", smithFit);

    // A zero share leaves a craft out of every roll
    Settings noLeather = defaults;
    noLeather.Share[ProfessionRules::CraftIndex(Skill::Leatherworking)] = 0.0f;
    ProfessionRules::FitResult const noLeatherFit = ProfessionRules::FitChances(noLeather);
    bool never = true;
    for (std::size_t slot = 0; slot < ProfessionRules::CLASS_COUNT; ++slot)
        never =
            never &&
            noLeatherFit.Chance[ProfessionRules::Cell(slot, ProfessionRules::CraftIndex(Skill::Leatherworking))] == 0.0;

    Check(never && noLeatherFit.Converged, "a zero share: the craft is in no class's chances, the fit still converges");
    CheckChanceTable("zero share", noLeatherFit);

    // A bias-0 cell is no option: a bias a table gives a class outside its crafts changes nothing
    Settings cheat = defaults;
    cheat.Bias[ProfessionRules::Cell(static_cast<std::size_t>(ProfessionRules::ClassSlot(ProfessionRules::MAGE)),
                                     ProfessionRules::CraftIndex(Skill::Blacksmithing))] = 5.0f;
    ProfessionRules::FitResult const cheatFit = ProfessionRules::FitChances(cheat);
    Check(cheatFit.Chance == fit.Chance, "a bias outside a class's table is ignored (the table is explicit)");

    // A tilted global table the classes can reach: the roster still holds it
    Settings tilted = defaults;
    tilted.Share = {20.0f, 20.0f, 20.0f, 5.0f, 5.0f, 10.0f, 10.0f, 10.0f};
    ProfessionRules::FitResult const tiltedFit = ProfessionRules::FitChances(tilted);
    Check(tiltedFit.Converged && !tiltedFit.Unreachable, "a tilted share table converges and is reachable");
    CheckRoster("tilted shares", tilted, false);
}

// The partner of tailoring and enchanting is drawn evenly among the gatherers and the other such craft (WR.2)
void CheckPartners()
{
    std::map<Skill, uint32_t> tailor;
    std::map<Skill, uint32_t> warriorEnchanter;
    for (uint32_t i = 0; i < PARTNER_GRID; ++i)
    {
        double const roll = (i + 0.5) / PARTNER_GRID;
        ProfessionRules::Pair const pair =
            ProfessionRules::PairOfCraft(ProfessionRules::CraftIndex(Skill::Tailoring), ProfessionRules::PRIEST, roll);
        ++tailor[pair.First == Skill::Tailoring ? pair.Second : pair.First];
        ProfessionRules::Pair const other = ProfessionRules::PairOfCraft(ProfessionRules::CraftIndex(Skill::Enchanting),
                                                                         ProfessionRules::WARRIOR, roll);
        ++warriorEnchanter[other.First == Skill::Enchanting ? other.Second : other.First];
    }

    Check(tailor.size() == 4 && tailor[Skill::Mining] == PARTNER_GRID / 4 &&
              tailor[Skill::Herbalism] == PARTNER_GRID / 4 && tailor[Skill::Skinning] == PARTNER_GRID / 4 &&
              tailor[Skill::Enchanting] == PARTNER_GRID / 4,
          "a priest's tailoring: the partner is each gatherer or enchanting, evenly");
    Check(warriorEnchanter.size() == 3 && !warriorEnchanter.count(Skill::Tailoring),
          "a warrior's enchanting: the partner is a gatherer (no tailoring, which warriors do not consider), evenly");
    ProfessionRules::Pair const smith =
        ProfessionRules::PairOfCraft(ProfessionRules::CraftIndex(Skill::Blacksmithing), ProfessionRules::WARRIOR, 0.5);
    Check(smith.First == Skill::Mining && smith.Second == Skill::Blacksmithing, "blacksmithing comes with mining");
}

void CheckSoonLevels()
{
    Check(ProfessionRules::SoonLevels(1) == 15, "the look-ahead is 15 levels at level 1");
    Check(ProfessionRules::SoonLevels(40) == 5 && ProfessionRules::SoonLevels(80) == 5,
          "the look-ahead is 5 levels from level 40");
    bool falling = true;
    for (uint32_t level = 2; level <= 80; ++level)
        falling = falling && ProfessionRules::SoonLevels(level) <= ProfessionRules::SoonLevels(level - 1) &&
                  ProfessionRules::SoonLevels(level) >= 5;

    Check(falling, "the look-ahead only falls with the level and never below 5");
    std::cout << "Look-ahead SoonLevels (levels): ";
    for (uint32_t const level : {1u, 5u, 10u, 20u, 30u, 39u, 40u, 80u})
        std::cout << "level " << level << " -> " << ProfessionRules::SoonLevels(level) << "; ";

    std::cout << "\n";
}

void CheckAdopt()
{
    TraitRules::BotTraits const traits = TraitRules::Roll(77, 1, 1, ProfessionRules::WARRIOR);
    BotProfessions const rolled = ProfessionRules::Roll(77, 1, ProfessionRules::WARRIOR, traits, defaultChances);

    // A warrior with a pair no rule would give it: it keeps it, gatherer first
    BotProfessions const odd = ProfessionRules::Adopt(rolled, {Skill::Tailoring, Skill::Skinning, Skill::Fishing});
    Check(odd.Mains[0] == Skill::Skinning && odd.Mains[1] == Skill::Tailoring,
          "held tailoring and skinning become the mains, skinning first");
    Check(odd.Secondary == ProfessionRules::SECONDARY_FISHING, "held fishing becomes the only secondary");
    Check(odd.From == ProfessionRules::Origin::Held, "a held person is marked held");
    Check(ProfessionRules::OriginWord(ProfessionRules::Origin::Chosen) == "chosen" &&
              static_cast<int>(ProfessionRules::Origin::Chosen) == 3,
          "the skill path's second choice is stored as origin 3, chosen (WR11, after set)");

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

    // Held tailoring, no held secondary: the rolled secondaries without first aid (a tailor never takes it)
    BotProfessions medic = rolled;
    medic.Secondary = ProfessionRules::SECONDARY_FIRST_AID | ProfessionRules::SECONDARY_COOKING;
    BotProfessions const tailor = ProfessionRules::Adopt(medic, {Skill::Tailoring, Skill::Skinning});
    Check(tailor.Secondary == ProfessionRules::SECONDARY_COOKING, "held tailoring drops the rolled first aid");

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
        BotProfessions const first = ProfessionRules::Roll(guid, 1, ProfessionRules::WARRIOR, traits, defaultChances);
        BotProfessions const again = ProfessionRules::Roll(guid, 1, ProfessionRules::WARRIOR, traits, defaultChances);
        BotProfessions const next = ProfessionRules::Roll(guid, 2, ProfessionRules::WARRIOR, traits, defaultChances);
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

    Settings const defaults = ProfessionRules::DefaultSettings();
    ProfessionRules::FitResult const fit = ProfessionRules::FitChances(defaults);
    defaultChances = fit.Chance;

    std::cout << "Craft shares: global (equal by default), the class biases (1 neutral, hunters' engineering 2), "
                 "fitted per class\n";
    CheckRoster("default shares", defaults, true);
    std::cout << "\n";

    std::vector<ClassTally> const tallies = RollRoster(defaultChances);
    std::cout << "Pairs per class: share of the class's bots with two mains (a craft with its gatherer; tailoring "
                 "and enchanting with a gatherer or each other; no two gatherers)\n";
    for (std::size_t c = 0; c < CLASSES.size(); ++c)
    {
        ClassRow const& row = CLASSES[c];
        ClassTally const& tally = tallies[c];
        std::vector<ProfessionRules::Pair> const pairs = ProfessionRules::OpenPairs(row.Id);
        uint32_t const two = tally.Bots - tally.OneMain;
        double const oneMain = 100.0 * tally.OneMain / tally.Bots;
        std::cout << row.Name << ": " << pairs.size() << " open pairs; one main only " << Percent(oneMain)
                  << " % of all\n";
        Check(oneMain >= ONE_MAIN_MIN && oneMain <= ONE_MAIN_MAX, std::string(row.Name) + ": a small share one main");
        bool craftPair = false;
        uint32_t seen = 0;
        for (ProfessionRules::Pair const& pair : pairs)
        {
            auto const it = tally.Pairs.find(Unordered(pair.First, pair.Second));
            uint32_t const count = it == tally.Pairs.end() ? 0 : it->second;
            seen += count;
            std::cout << "  " << std::left << std::setw(32) << PairName(pair.First, pair.Second) << std::right
                      << std::setw(6) << Percent(100.0 * count / two) << "\n";
            craftPair = craftPair || ProfessionRules::FindCraft(pair.Second) || ProfessionRules::FindCraft(pair.First);
        }

        Check(craftPair, std::string(row.Name) + ": a pair with a craft");
        Check(seen == two, std::string(row.Name) + ": every pair rolled is an open pair");
        Check(tally.FishingWithInterest == tally.FishingInterest,
              std::string(row.Name) + ": every bot with the fishing interest fishes");
        double const without = 100.0 * tally.FishingWithoutInterest / (tally.Bots - tally.FishingInterest);
        Check(without > ProfessionRules::SECONDARY_WITHOUT_INTEREST_PERCENT * 0.7 &&
                  without < ProfessionRules::SECONDARY_WITHOUT_INTEREST_PERCENT * 1.3,
              std::string(row.Name) + ": a small share fishes without the interest");
    }

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
        double const share = 100.0 * tallies[c].Professions.at(Skill::FirstAid) / tallies[c].Bots;
        if (ProfessionRules::SELF_HEALERS & ProfessionRules::ClassBit(CLASSES[c].Id))
            healers = std::max(healers, share);
        else
            others = others == 0.0 ? share : std::min(others, share);
    }

    Check(others > healers, "first aid: every class that cannot heal itself above every self-healer");

    std::cout << "\n";
    CheckFit();
    CheckPartners();
    CheckSoonLevels();
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
                  << ProfessionRules::Describe(ProfessionRules::Roll(guid, 1, row.Id, traits, defaultChances)) << "\n";
    }

    std::cout << "\n" << (checks - failures) << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
