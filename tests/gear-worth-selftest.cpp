/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of what an upgrade is worth (gear plan WG4, src/Ai/World/Rpg/GearWorthRules.h). The invariants
// hold for any input: 0 without a gain, an earn rate or hours and for any input that is not a finite number; the gain
// in [0, 1] and the hours in [0, the cap's horizon]; never above WORTH_PRICELESS - 1; monotone in the item's score,
// the earn rate, the hours per level and the item level. Cases: the plan's worked example (PLAN 3.2), the level cap,
// an outgrown item, an empty slot, an Outland item far above the bot, and the ceiling as the bag's shape with
// LineOwner::Gear. The economy reference package (docs/economy-reference/cases/gear_trade.json) has no gear-worth
// case: its 15 cases price owned items, repairs, enchants, restocks and trades, none what an upgrade is worth.
// Then the ladder (markdown): per level band and gain, the worth at the configured earn rate and at the night's
// income beside the night's asks (lab5-20261125-175728, buyout medians and lower quartiles), the ceiling for the
// band's median bot, and the exit's gate verdict. Usage: gear-worth-selftest. Exit code 1 on a failed check.
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "GearWorthRules.h"
#include "GoalModel.h"
#include "PlannerSnapshot.h"

using GearWorth::WorthInput;

namespace
{
constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
constexpr float INFINITE = std::numeric_limits<float>::infinity();
constexpr float HUGE_VALUE = 1.0e30f;
constexpr float TINY_VALUE = 1.0e-30f;
constexpr double COPPER_PER_GOLD = 10000.0;
constexpr float SCORE_SUM = 1000.0f;
constexpr std::uint32_t LADDER_ITEM_LEVEL_AHEAD = 3;  // the night's newly worn items: item level 2 to 3 above the bot
constexpr std::size_t GATE_GAIN = 3;                  // index of the 5 % gain the exit's gate reads

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

// A level-35 bot with an item that raises its gear strength by 3 % and is worn 6 levels of 5 hours
WorthInput Base()
{
    WorthInput in;
    in.itemScore = 40.0f;
    in.wornScore = 10.0f;
    in.summedScore = SCORE_SUM;
    in.level = 35;
    in.itemLevel = 38;
    in.earnCopperPerMinute = 100.0f;
    in.hoursPerLevel = 5.0f;
    return in;
}

void CheckZero()
{
    auto zero = [](WorthInput const& in, std::string const& name) { Check(GearWorth::Worth(in) == 0, name); };
    Check(GearWorth::Worth(Base()) > 0, "base case has a worth");

    WorthInput in = Base();
    in.itemScore = in.wornScore;
    zero(in, "no gain: the item scores as the worn one");
    in.itemScore = in.wornScore - 1.0f;
    zero(in, "no gain: the item scores below the worn one");
    in = Base();
    in.wornScore = -5.0f;
    in.itemScore = 0.0f;
    zero(in, "no gain: an item scoring 0 against a negative worn one");
    in.itemScore = -1.0f;
    zero(in, "no gain: a negative item against a negative worn one");
    for (float const summed : {0.0f, -1.0f})
    {
        in = Base();
        in.summedScore = summed;
        zero(in, "no summed score");
    }
    for (float const earn : {0.0f, -100.0f})
    {
        in = Base();
        in.earnCopperPerMinute = earn;
        zero(in, "no earn rate");
    }
    for (float const hours : {0.0f, -5.0f})
    {
        in = Base();
        in.hoursPerLevel = hours;
        zero(in, "no hours per level below the cap");
    }
    in = Base();
    in.itemLevel = in.level - static_cast<std::uint32_t>(GearWorth::OUTGROWN_LEVELS);
    zero(in, "outgrown: item level the gap below the bot's");
    in.itemLevel = 0;
    zero(in, "outgrown: item level 0");

    for (float const bad : {NOT_A_NUMBER, INFINITE, -INFINITE})
    {
        in = Base();
        in.itemScore = bad;
        zero(in, "non-finite item score");
        in = Base();
        in.wornScore = bad;
        zero(in, "non-finite worn score");
        in = Base();
        in.summedScore = bad;
        zero(in, "non-finite summed score");
        in = Base();
        in.earnCopperPerMinute = bad;
        zero(in, "non-finite earn rate");
        in = Base();
        in.hoursPerLevel = bad;
        zero(in, "non-finite hours per level");
        in.level = GearWorth::LEVEL_CAP;
        zero(in, "non-finite hours per level at the cap");
    }
}

// Every combination of open-ended inputs: bounded, the gain and hours in range, and monotone in the item's score,
// the earn rate, the hours per level and the item level (each step to the next larger finite value)
void CheckGrid()
{
    std::vector<float> const scores = {-HUGE_VALUE, -5.0f, 0.0f, TINY_VALUE, 0.5f, 10.0f, 400.0f, HUGE_VALUE};
    std::vector<float> const rates = {-1.0f, 0.0f, TINY_VALUE, 1.0f, 100.0f, 1000.0f, HUGE_VALUE};
    std::vector<std::uint32_t> const levels = {0, 1, 35, 79, 80, 255, std::numeric_limits<std::uint32_t>::max()};
    std::vector<std::uint32_t> const itemLevels = {0, 1, 30, 38, 142, 200, std::numeric_limits<std::uint32_t>::max()};
    std::vector<float> const specials = {NOT_A_NUMBER, INFINITE, -INFINITE};

    std::uint64_t bounded = 0;
    std::uint64_t gainRange = 0;
    std::uint64_t hoursRange = 0;
    std::uint64_t specialZero = 0;
    std::uint64_t monotone = 0;
    std::uint64_t combinations = 0;
    auto make = [&](std::size_t item, std::size_t worn, std::size_t summed, std::size_t level, std::size_t itemLevel,
                    std::size_t earn, std::size_t hours)
    {
        WorthInput in;
        in.itemScore = scores[item];
        in.wornScore = scores[worn];
        in.summedScore = scores[summed];
        in.level = levels[level];
        in.itemLevel = itemLevels[itemLevel];
        in.earnCopperPerMinute = rates[earn];
        in.hoursPerLevel = rates[hours];
        return in;
    };
    for (std::size_t item = 0; item < scores.size(); ++item)
        for (std::size_t worn = 0; worn < scores.size(); ++worn)
            for (std::size_t summed = 0; summed < scores.size(); ++summed)
                for (std::size_t level = 0; level < levels.size(); ++level)
                    for (std::size_t itemLevel = 0; itemLevel < itemLevels.size(); ++itemLevel)
                        for (std::size_t earn = 0; earn < rates.size(); ++earn)
                            for (std::size_t hours = 0; hours < rates.size(); ++hours)
                            {
                                ++combinations;
                                WorthInput const in = make(item, worn, summed, level, itemLevel, earn, hours);
                                std::uint32_t const worth = GearWorth::Worth(in);
                                double const gain = GearWorth::Gain(in);
                                double const wornHours = GearWorth::HoursWorn(in);
                                bounded += worth >= WorldTravel::WORTH_PRICELESS;
                                gainRange += !(gain >= 0.0 && gain <= 1.0);
                                hoursRange += !(wornHours >= 0.0 && wornHours <= GearWorth::CAP_HORIZON_HOURS);
                                if (item + 1 < scores.size())
                                    monotone += GearWorth::Worth(make(item + 1, worn, summed, level, itemLevel, earn,
                                                                      hours)) < worth;
                                if (itemLevel + 1 < itemLevels.size())
                                    monotone += GearWorth::Worth(make(item, worn, summed, level, itemLevel + 1, earn,
                                                                      hours)) < worth;
                                if (earn + 1 < rates.size())
                                    monotone += GearWorth::Worth(make(item, worn, summed, level, itemLevel, earn + 1,
                                                                      hours)) < worth;
                                if (hours + 1 < rates.size())
                                    monotone += GearWorth::Worth(make(item, worn, summed, level, itemLevel, earn,
                                                                      hours + 1)) < worth;
                                for (float const special : specials)
                                {
                                    WorthInput broken = in;
                                    broken.itemScore = special;
                                    specialZero += GearWorth::Worth(broken) != 0;
                                    broken = in;
                                    broken.summedScore = special;
                                    specialZero += GearWorth::Worth(broken) != 0;
                                    broken = in;
                                    broken.earnCopperPerMinute = special;
                                    specialZero += GearWorth::Worth(broken) != 0;
                                }
                            }
    Check(combinations > 0 && bounded == 0, "grid: never at or above WORTH_PRICELESS");
    Check(gainRange == 0, "grid: the gain in [0, 1]");
    Check(hoursRange == 0, "grid: the hours worn in [0, the cap's horizon]");
    Check(monotone == 0, "grid: monotone in the item's score, item level, earn rate and hours per level");
    Check(specialZero == 0, "grid: a non-finite input gives 0");

    WorthInput in = Base();
    in.itemScore = HUGE_VALUE;
    in.summedScore = TINY_VALUE;
    in.earnCopperPerMinute = HUGE_VALUE;
    in.level = GearWorth::LEVEL_CAP;
    Check(GearWorth::Worth(in) == WorldTravel::WORTH_PRICELESS - 1, "huge inputs stop just below WORTH_PRICELESS");
}

void CheckCases()
{
    // PLAN 3.2: 3 % gain, fights 60 % of the time, 2.45 g per hour, 30 hours: 0.03 x 0.6 x 2.45 g x 30 = 1.323 g
    WorthInput in = Base();
    in.earnCopperPerMinute = static_cast<float>(2.45 * COPPER_PER_GOLD / WorldTravel::MINUTES_PER_HOUR);
    Check(std::abs(GearWorth::Gain(in) - 0.03) < 1.0e-6, "worked example: gain 3 %");
    Check(std::abs(GearWorth::HoursWorn(in) - 30.0) < 1.0e-6, "worked example: 30 hours worn");
    Check(std::abs(static_cast<double>(GearWorth::Worth(in)) - 13230.0) <= 1.0, "worked example: 1.323 gold");

    in = Base();
    in.wornScore = -20.0f;
    WorthInput empty = Base();
    empty.wornScore = 0.0f;
    Check(GearWorth::Worth(in) == GearWorth::Worth(empty), "a worn item scoring below 0 counts as an empty slot");

    in = Base();
    in.level = GearWorth::LEVEL_CAP;
    in.itemLevel = 1;
    in.hoursPerLevel = 0.0f;
    WorthInput high = in;
    high.itemLevel = 264;
    Check(GearWorth::HoursWorn(in) == GearWorth::CAP_HORIZON_HOURS, "the level cap wears an item the cap's horizon");
    Check(GearWorth::Worth(in) == GearWorth::Worth(high) && GearWorth::Worth(in) > 0,
          "at the level cap the item level does not matter");

    // An Outland green of item level 142 at level 62 with the night's 11.86 hours per level: 83 levels, capped
    in = Base();
    in.level = 62;
    in.itemLevel = 142;
    in.hoursPerLevel = 11.86f;
    Check(GearWorth::HoursWorn(in) == GearWorth::CAP_HORIZON_HOURS, "an item far above the bot is capped");
    // The night's 353 hours per level at 70-79: one level more than the cap's horizon
    in.level = 75;
    in.itemLevel = 73;
    in.hoursPerLevel = 353.0f;
    Check(GearWorth::HoursWorn(in) == GearWorth::CAP_HORIZON_HOURS, "353 hours per level are capped");
    in.itemLevel = 71;
    Check(GearWorth::HoursWorn(in) == 0.0, "outgrown at 70-79 too");

    // One slot of sixteen improved by a third: about 2 %
    in = Base();
    in.wornScore = 60.0f;
    in.itemScore = 80.0f;
    in.summedScore = 15.0f * 60.0f + 80.0f;
    Check(std::abs(GearWorth::Gain(in) - 0.0204) < 0.001, "one slot of sixteen up by a third is about 2 %");
}

void CheckCeiling()
{
    AuctionRules::CopperWeight const unknown{0.0f, 0.0f};
    AuctionRules::CopperWeight const reserve{2.0f, 2.0f};
    AuctionRules::CopperWeight const flush{2.0f, 0.2f};
    AuctionRules::CopperWeight const broke{2.0f, 6.0f};
    constexpr std::uint32_t worth = 10000;
    Check(GearWorth::CeilingOf(worth, 0, unknown) == worth, "no plan yet: the ceiling is the worth");
    Check(GearWorth::CeilingOf(worth, 0, reserve) == worth, "at the reserve: the ceiling is the worth");
    std::uint32_t const flushCeiling = GearWorth::CeilingOf(worth, 0, flush);
    Check(flushCeiling >= 99999 && flushCeiling <= 100000, "a flush bot pays up to ten times the worth");
    Check(GearWorth::CeilingOf(worth, 0, broke) < worth, "a bot short of gold pays less than the worth");
    std::uint32_t const stocked = GearWorth::CeilingOf(worth, 500, flush);
    Check(stocked >= 499 && stocked <= 500, "a vendor-stocked item: at most the vendor's price");
    Check(GearWorth::CeilingOf(0, 500, flush) == 0, "no worth, no ceiling");
    Check(GearWorth::CeilingOf(WorldTravel::WORTH_PRICELESS - 1, 0, AuctionRules::CopperWeight{2.0f, 0.02f}) ==
              AuctionRules::MAX_LOT_PRICE,
          "a huge ceiling stops at the highest lot price");
    Check(GearWorth::GearLine(7, worth, 0, flush).owner == AuctionRules::LineOwner::Gear, "the line is gear's");
    std::uint64_t mismatch = 0;
    for (std::uint32_t const each : {0u, 1u, 999u, worth, 123456789u})
        for (std::uint32_t const vendor : {0u, 1u, 500u, 50000u})
            for (AuctionRules::CopperWeight const weight : {unknown, reserve, flush, broke})
                mismatch +=
                    GearWorth::CeilingOf(each, vendor, weight) != AuctionRules::BagCeiling(each, vendor, weight);
    Check(mismatch == 0, "the gear ceiling has the bag's shape (AH.5)");
}

// ---- the ladder ----

enum Kind : std::size_t
{
    GREEN_ARMOUR,
    GREEN_WEAPONS,
    RARE_ARMOUR,
    RARE_WEAPONS,
    KIND_COUNT
};
constexpr std::array<char const*, KIND_COUNT> KIND_WORDS = {"green armour", "green weapons", "rare armour",
                                                            "rare weapons"};
constexpr std::array<char const*, KIND_COUNT> KIND_SHORT = {"GA", "GW", "RA", "RW"};

struct Ask
{
    double median{0.0};  // gold, buyout
    double lowerQuartile{0.0};
    std::uint32_t listings{0};  // 0: none listed
};

// The night's listed gear by item level (gear NOTES Facts; nothing of it sold that night)
struct AskBand
{
    char const* itemLevels;
    std::array<Ask, KIND_COUNT> asks;
};

constexpr std::size_t ASK_BAND_COUNT = 7;
constexpr std::array<AskBand, ASK_BAND_COUNT> ASK_BANDS = {{
    {"1-19", {{{0.12, 0.07, 4948}, {0.20, 0.11, 1159}, {0.0, 0.0, 0}, {0.0, 0.0, 0}}}},
    {"20-29", {{{0.50, 0.32, 2058}, {1.48, 1.08, 336}, {0.86, 0.58, 50}, {2.25, 1.61, 47}}}},
    {"30-39", {{{1.87, 1.39, 1870}, {5.12, 3.72, 162}, {3.00, 2.14, 40}, {11.54, 7.59, 34}}}},
    {"40-49", {{{4.46, 3.31, 3112}, {12.38, 10.21, 172}, {7.96, 5.66, 88}, {24.14, 19.26, 72}}}},
    {"50-59", {{{9.95, 7.11, 3448}, {26.54, 20.99, 209}, {16.07, 12.02, 99}, {45.67, 41.14, 109}}}},
    {"60-99", {{{1.91, 0.64, 1001}, {43.25, 31.95, 37}, {24.84, 24.05, 10}, {76.38, 75.89, 2}}}},
    {"100+", {{{95.34, 70.16, 2890}, {261.01, 204.90, 1080}, {172.57, 148.86, 99}, {615.65, 551.32, 19}}}},
}};
constexpr std::size_t NO_ASK_BAND = ASK_BAND_COUNT;

// Night lab5-20261125-175728 by level band (report.txt "leveling" and "money"); the bot's level is the band's middle
struct LadderBand
{
    char const* levels;
    std::uint32_t level;
    float hoursPerLevel;   // 0: none (the cap)
    double incomePerHour;  // gold per bot hour
    std::array<std::size_t, 2> askBands;
    bool gated;  // in the exit's gate, bands 20-29 to 50-59
};

constexpr std::array<LadderBand, 8> LADDER_BANDS = {{
    {"10-19", 15, 1.44f, 0.58, {0, NO_ASK_BAND}, false},
    {"20-29", 25, 3.34f, 0.96, {1, NO_ASK_BAND}, true},
    {"30-39", 35, 4.30f, 2.45, {2, NO_ASK_BAND}, true},
    {"40-49", 45, 5.34f, 4.86, {3, NO_ASK_BAND}, true},
    {"50-59", 55, 6.97f, 9.40, {4, NO_ASK_BAND}, true},
    {"60-69", 65, 11.86f, 13.25, {5, 6}, false},
    {"70-79", 75, 353.0f, 18.13, {6, NO_ASK_BAND}, false},
    {"80", 80, 0.0f, 20.66, {6, NO_ASK_BAND}, false},
}};

constexpr std::array<double, 5> LADDER_GAINS = {0.01, 0.02, 0.03, 0.05, 0.10};

// The median bot's money and reserve in copper by slot band (tests/fixtures/auction-market-night.txt `carried` q2:
// money, and money less spendable)
constexpr std::array<double, WorldTravel::LEVEL_BAND_COUNT> MEDIAN_MONEY = {1834988, 6085113, 15131640, 20931725,
                                                                            25288912};
constexpr std::array<double, WorldTravel::LEVEL_BAND_COUNT> MEDIAN_RESERVE = {34493, 91330, 715424, 1511007, 1609608};

AuctionRules::CopperWeight MedianWeight(std::uint32_t level)
{
    std::uint32_t const band = WorldTravel::LevelBand(level);
    Planner::Snapshot bot;
    bot.level = static_cast<std::uint8_t>(level);
    bot.money = static_cast<std::uint64_t>(MEDIAN_MONEY[band]);
    Planner::Goals::Weights const weights{};
    Planner::Goals::Need const need =
        Planner::Goals::NeedFor(bot, weights, static_cast<std::uint64_t>(MEDIAN_RESERVE[band]));
    return {need.exchange, need.factor};
}

float ConfigEarn(std::uint32_t level)
{
    return static_cast<float>(WorldTravel::DEFAULT_SLOT_BANDS[WorldTravel::LevelBand(level)].EarnCopperPerMinute);
}

float NightEarn(LadderBand const& band)
{
    return static_cast<float>(band.incomePerHour * COPPER_PER_GOLD / WorldTravel::MINUTES_PER_HOUR);
}

std::uint32_t LadderWorth(LadderBand const& band, double gain, float earn)
{
    WorthInput in;
    in.itemScore = static_cast<float>(gain * SCORE_SUM);
    in.wornScore = 0.0f;
    in.summedScore = SCORE_SUM;
    in.level = band.level;
    in.itemLevel = band.level + LADDER_ITEM_LEVEL_AHEAD;
    in.earnCopperPerMinute = earn;
    in.hoursPerLevel = band.hoursPerLevel;
    return GearWorth::Worth(in);
}

std::string Gold(double copper)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << copper / COPPER_PER_GOLD;
    return text.str();
}

// The kinds listed under a value: "GA m" when the median ask is under it, "GA q" when only the lower quartile is
std::string Under(LadderBand const& band, double copper)
{
    std::string text;
    double const gold = copper / COPPER_PER_GOLD;
    for (std::size_t const askBand : band.askBands)
    {
        if (askBand == NO_ASK_BAND)
            continue;
        for (std::size_t kind = 0; kind < KIND_COUNT; ++kind)
        {
            Ask const& ask = ASK_BANDS[askBand].asks[kind];
            if (!ask.listings || ask.lowerQuartile >= gold)
                continue;
            text += (text.empty() ? "" : ", ") + std::string(KIND_SHORT[kind]) + (ask.median < gold ? " m" : " q");
            if (band.askBands[1] != NO_ASK_BAND)
                text += std::string(" (") + ASK_BANDS[askBand].itemLevels + ")";
        }
    }
    return text.empty() ? "-" : text;
}

// The gated bands whose 5 % worth (or ceiling) is above a listed kind's median or lower-quartile ask
std::string GateHits(bool night, bool ceiling, bool median)
{
    std::string text;
    for (LadderBand const& band : LADDER_BANDS)
    {
        if (!band.gated)
            continue;
        float const earn = night ? NightEarn(band) : ConfigEarn(band.level);
        std::uint32_t const worth = LadderWorth(band, LADDER_GAINS[GATE_GAIN], earn);
        double const value = ceiling ? GearWorth::CeilingOf(worth, 0, MedianWeight(band.level)) : worth;
        double const gold = value / COPPER_PER_GOLD;
        for (std::size_t kind = 0; kind < KIND_COUNT; ++kind)
        {
            Ask const& ask = ASK_BANDS[band.askBands[0]].asks[kind];
            if (ask.listings && (median ? ask.median : ask.lowerQuartile) < gold)
                text += (text.empty() ? "" : ", ") + std::string(band.levels) + " " + KIND_SHORT[kind];
        }
    }
    return text;
}

void PrintLadder()
{
    std::cout << "\n## The gear worth ladder (WG4)\n\n";
    std::cout << "worth = gain x fight share " << GearWorth::FIGHT_SHARE << " x earn rate x hours worn; hours worn = "
              << "(item level + " << GearWorth::OUTGROWN_LEVELS << " - level) x hours per level, at most "
              << GearWorth::CAP_HORIZON_HOURS << " h, and " << GearWorth::CAP_HORIZON_HOURS << " h at level "
              << GearWorth::LEVEL_CAP << ". Each band's bot at the band's middle level with an item "
              << LADDER_ITEM_LEVEL_AHEAD
              << " item levels above it. Config e: the slot band's EarnCopperPerMinute (the rule's input); night: "
                 "the night's income per bot hour. Ceiling: GearWorth::CeilingOf for the slot band's median bot "
                 "(money and reserve from the night's `carried` medians), neutral greed, no vendor. Gold.\n\n";
    std::cout << "| band | level / item level | config e g/h | night g/h | hours/level | hours worn | median bot share "
                 "|\n|---|---|---|---|---|---|---|\n";
    for (LadderBand const& band : LADDER_BANDS)
    {
        WorthInput in;
        in.level = band.level;
        in.itemLevel = band.level + LADDER_ITEM_LEVEL_AHEAD;
        in.hoursPerLevel = band.hoursPerLevel;
        std::cout << "| " << band.levels << " | " << band.level << " / " << in.itemLevel << " | "
                  << Gold(ConfigEarn(band.level) * WorldTravel::MINUTES_PER_HOUR) << " | " << std::fixed
                  << std::setprecision(2) << band.incomePerHour << " | "
                  << (band.hoursPerLevel > 0.0f ? Gold(band.hoursPerLevel * COPPER_PER_GOLD) : std::string("-"))
                  << " | " << std::setprecision(1) << GearWorth::HoursWorn(in) << " | " << std::setprecision(2)
                  << AuctionRules::ShareOf(MedianWeight(band.level)) << " |\n";
    }

    std::cout << "\nUnder: the kinds whose ask is below the value, `m` the median ask, `q` only the lower quartile "
                 "(GA green armour, GW green weapons, RA rare armour, RW rare weapons).\n\n";
    std::cout << "| band | gain | worth (config e) | ceiling | under worth | worth (night) | ceiling | under worth "
                 "|\n|---|---|---|---|---|---|---|---|\n";
    for (LadderBand const& band : LADDER_BANDS)
        for (double const gain : LADDER_GAINS)
        {
            AuctionRules::CopperWeight const weight = MedianWeight(band.level);
            std::uint32_t const config = LadderWorth(band, gain, ConfigEarn(band.level));
            std::uint32_t const night = LadderWorth(band, gain, NightEarn(band));
            std::cout << "| " << band.levels << " | " << std::setprecision(0) << gain * 100.0 << " % | " << Gold(config)
                      << " | " << Gold(GearWorth::CeilingOf(config, 0, weight)) << " | " << Under(band, config) << " | "
                      << Gold(night) << " | " << Gold(GearWorth::CeilingOf(night, 0, weight)) << " | "
                      << Under(band, night) << " |\n";
        }

    std::cout << "\nThe night's asks, buyout median / lower quartile in gold (listings), by item level. Level bands "
                 "to item levels: the same band up to 50-59; 60-69 beside both 60-99 and 100+; 70-79 and 80 beside "
                 "100+.\n\n| item level |"
              << std::setprecision(2);
    for (char const* word : KIND_WORDS)
        std::cout << " " << word << " |";
    std::cout << "\n|---|---|---|---|---|\n";
    for (AskBand const& band : ASK_BANDS)
    {
        std::cout << "| " << band.itemLevels << " |";
        for (Ask const& ask : band.asks)
            if (ask.listings)
                std::cout << " " << ask.median << " / " << ask.lowerQuartile << " (" << ask.listings << ") |";
            else
                std::cout << " none |";
        std::cout << "\n";
    }

    std::cout << "\n### The exit's gate: a listed kind under the worth of a 5 % gain, bands 20-29 to 50-59\n\n";
    for (bool const night : {false, true})
        for (bool const median : {true, false})
        {
            std::string const hits = GateHits(night, false, median);
            std::cout << "- " << (night ? "night income" : "config e (the rule's input)") << ", "
                      << (median ? "median asks" : "lower-quartile asks") << ": "
                      << (hits.empty() ? "FIRES (no kind under the worth)" : "passes: " + hits) << "\n";
        }
    std::cout << "\nFor comparison only (not the gate): the ceiling of the median bot against the asks\n\n";
    for (bool const night : {false, true})
        for (bool const median : {true, false})
        {
            std::string const hits = GateHits(night, true, median);
            std::cout << "- " << (night ? "night income" : "config e") << ", "
                      << (median ? "median asks" : "lower-quartile asks") << ": "
                      << (hits.empty() ? "none under the ceiling" : hits) << "\n";
        }
}
}  // namespace

void CheckHoursPerLevel()
{
    constexpr std::array<std::array<std::uint32_t, 2>, 7> BANDS = {
        {{1, 19}, {20, 29}, {30, 39}, {40, 49}, {50, 59}, {60, 69}, {70, 79}}};
    for (std::size_t i = 0; i < BANDS.size(); ++i)
    {
        float const hours = LADDER_BANDS[i].hoursPerLevel;
        Check(GearWorth::HoursPerLevel(BANDS[i][0]) == hours, "hours: the band's bottom");
        Check(GearWorth::HoursPerLevel(BANDS[i][1]) == hours, "hours: the band's top");
    }
    Check(GearWorth::HoursPerLevel(80) == 0.0f, "hours: none at the cap");
    Check(GearWorth::HoursPerLevel(81) == 0.0f, "hours: none above the cap");
}

// Which vendor items the worth rule decides; a bot that does not keep its possessions buys as before
void CheckRoute()
{
    using GearWorth::Route;
    using GearWorth::RouteOf;

    for (unsigned bits = 0; bits < 32; ++bits)
    {
        GearWorth::RouteInput const in{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0,
                                       (bits & 16) != 0};
        Route const expected = !in.keeper || !in.gear || !in.equipUsage ? Route::Old
                               : in.upgrade && in.goldPrice             ? Route::ByWorth
                                                                        : Route::Never;
        Check(RouteOf(in) == expected, "route: " + std::to_string(bits));
    }
    Check(RouteOf({false, true, true, true, true}) == Route::Old, "route: not a keeper, an upgrade");
    Check(RouteOf({false, true, true, false, true}) == Route::Old, "route: not a keeper, no upgrade");
    Check(RouteOf({true, true, true, false, true}) == Route::Never, "route: a keeper never buys a non-upgrade");
}

void CheckChoice()
{
    using GearWorth::BestBuy;
    using GearWorth::NO_OFFER;
    using GearWorth::Offer;
    using GearWorth::Refusals;

    constexpr std::uint8_t HEAD = 0;
    constexpr std::uint8_t MAIN = 15;
    constexpr std::uint8_t OFF = 16;
    auto const make = [](std::uint32_t item, std::uint8_t slot, std::uint32_t worth, std::uint32_t price,
                         std::uint32_t ceiling) { return Offer{item, slot, 1u << slot, worth, price, ceiling}; };

    std::vector<Offer> const one = {make(1, HEAD, 1000, 400, 1000)};
    Check(BestBuy(one, 0, 400) == 0, "choice: bought with exactly the price spendable");
    Check(BestBuy(one, 0, 399) == NO_OFFER, "choice: one copper short");
    Check(Refusals(one, 0, 0).size() == 1 && Refusals(one, 0, 0)[0].noGold, "choice: short of gold is no_gold");
    Check(BestBuy(one, 1u << HEAD, 400) == NO_OFFER, "choice: the slot is filled");
    Check(Refusals(one, 1u << HEAD, 0).empty(), "choice: a filled slot has no refusal");
    Check(Refusals(one, 0, 1u << HEAD).empty(), "choice: a slot is refused once");

    std::vector<Offer> const dear = {make(1, HEAD, 1000, 1001, 1000)};
    Check(BestBuy(dear, 0, 1000000) == NO_OFFER, "choice: above the ceiling");
    Check(Refusals(dear, 0, 0).size() == 1 && !Refusals(dear, 0, 0)[0].noGold, "choice: dearer is over_worth");

    std::vector<Offer> const flush = {make(1, HEAD, 1000, 1500, 2000)};
    Check(BestBuy(flush, 0, 1500) == 0, "choice: above the worth, within the ceiling");

    std::vector<Offer> const worthless = {make(1, HEAD, 0, 10, 0)};
    Check(BestBuy(worthless, 0, 1000) == NO_OFFER, "choice: worth 0 is not bought");
    Check(Refusals(worthless, 0, 0).size() == 1 && !Refusals(worthless, 0, 0)[0].noGold,
          "choice: worth 0 is over_worth");

    // The better surplus is unaffordable, the cheaper second is bought
    std::vector<Offer> const two = {make(1, HEAD, 5000, 2000, 5000), make(2, HEAD, 900, 500, 900)};
    Check(BestBuy(two, 0, 2000) == 0, "choice: the best surplus");
    Check(BestBuy(two, 0, 600) == 1, "choice: the cheaper second");

    // One refusal per slot: an offer within its ceiling before a dearer one of any surplus
    std::vector<Offer> const mixed = {make(1, HEAD, 3000, 3100, 3000), make(2, HEAD, 900, 800, 900),
                                      make(3, HEAD, 2000, 1000, 2000), make(4, MAIN, 10, 500, 10),
                                      make(5, MAIN, 400, 500, 400)};
    auto const refusals = Refusals(mixed, 0, 0);
    Check(refusals.size() == 2, "choice: one refusal per slot");
    Check(refusals.size() == 2 && refusals[0].offer == 2 && refusals[0].noGold, "choice: no_gold names the best");
    Check(refusals.size() == 2 && refusals[1].offer == 4 && !refusals[1].noGold, "choice: over_worth names the best");

    // A two-hander fills both hands: not bought once either is filled
    std::vector<Offer> const hands = {{7, MAIN, (1u << MAIN) | (1u << OFF), 1000, 100, 1000}};
    Check(BestBuy(hands, 1u << OFF, 1000) == NO_OFFER, "choice: a filled off hand blocks a two-hander");
    Check(BestBuy(hands, 1u << HEAD, 1000) == 0, "choice: another slot blocks nothing");

    Check(BestBuy({}, 0, 1000) == NO_OFFER && Refusals({}, 0, 0).empty(), "choice: nothing offered");
}

// Gear lines at the auctioneer (WG7): one pick per slot, the wear rule asked in the slot's order
void CheckPick()
{
    using GearWorth::Offer;
    using GearWorth::PickPerSlot;
    using Picks = std::vector<std::size_t>;

    constexpr std::uint8_t HEAD = 0;
    constexpr std::uint8_t LEGS = 6;
    auto const make = [](std::uint32_t item, std::uint8_t slot, std::uint32_t worth, std::uint32_t price,
                         std::uint32_t ceiling) { return Offer{item, slot, 1u << slot, worth, price, ceiling}; };
    auto const all = [](std::size_t) { return true; };

    std::vector<Offer> const same = {make(1, HEAD, 1000, 400, 1000), make(2, HEAD, 1200, 600, 1200),
                                     make(3, HEAD, 900, 100, 900)};
    Check(PickPerSlot(same, 100000, all) == Picks{2}, "pick: one per slot, the largest surplus");

    std::vector<Offer> const tie = {make(1, HEAD, 1500, 1000, 1500), make(2, HEAD, 1000, 500, 1000)};
    Check(PickPerSlot(tie, 100000, all) == Picks{1}, "pick: equal surplus goes to the cheaper");

    // A poor suffix cheap, a good suffix dearer: the larger worth above price wins
    std::vector<Offer> const suffix = {make(1, HEAD, 600, 200, 600), make(1, HEAD, 2000, 900, 2000)};
    Check(PickPerSlot(suffix, 100000, all) == Picks{1}, "pick: the good suffix when its surplus is larger");

    std::vector<Offer> const dear = {make(1, HEAD, 5000, 2000, 5000), make(2, HEAD, 900, 500, 900)};
    Check(PickPerSlot(dear, 600, all) == Picks{1}, "pick: an affordable offer before a larger unaffordable one");
    std::vector<Offer> const over = {make(1, HEAD, 5000, 1000, 800), make(2, HEAD, 900, 500, 900)};
    Check(PickPerSlot(over, 100000, all) == Picks{1}, "pick: within the ceiling before a larger over it");
    Check(PickPerSlot(dear, 100, all) == Picks{0}, "pick: none buyable keeps the slot's largest surplus");

    std::vector<std::size_t> asked;
    auto const refuseFirst = [&](std::size_t i)
    {
        asked.push_back(i);
        return i != 2;
    };
    Check(PickPerSlot(same, 100000, refuseFirst) == Picks{0}, "pick: a refusal falls to the next");
    Check(asked == Picks{2, 0}, "pick: not asked past the first yes");
    Check(PickPerSlot(same, 100000, [](std::size_t) { return false; }).empty(), "pick: every offer refused");

    std::vector<Offer> const slots = {make(1, HEAD, 1000, 900, 1000), make(2, LEGS, 3000, 1000, 3000),
                                      make(3, HEAD, 1100, 950, 1100)};
    Check(PickPerSlot(slots, 100000, all) == Picks{1, 2}, "pick: two slots by surplus");

    std::vector<Offer> const worthless = {make(1, HEAD, 0, 0, 0), make(2, LEGS, 500, 100, 500)};
    Check(PickPerSlot(worthless, 100000, all) == Picks{1}, "pick: worth 0 dropped");
    Check(PickPerSlot({}, 100000, all).empty(), "pick: nothing listed");
}

void CheckNoOfferSlot()
{
    using GearWorth::NO_SLOT;
    using GearWorth::NoOfferSlot;
    using GearWorth::WornSlot;
    using Worn = std::array<WornSlot, GearWorth::EQUIPMENT_SLOTS>;

    Worn full;
    for (WornSlot& slot : full)
        slot = {true, 50};
    Worn gap = full;
    gap[9] = {};
    gap[2].itemLevel = 10;
    Check(NoOfferSlot(gap, 0) == 9, "no_offer: an empty slot before a low one");
    Worn low = full;
    low[7].itemLevel = 20;
    low[12].itemLevel = 20;
    Check(NoOfferSlot(low, 0) == 7, "no_offer: the lowest item level, the first of equals");

    Worn bare = full;
    bare[GearWorth::SLOT_SHIRT] = {};
    bare[GearWorth::SLOT_TABARD] = {};
    bare[GearWorth::SLOT_OFF_HAND] = {};
    bare[4].itemLevel = 30;
    Check(NoOfferSlot(bare, 1u << GearWorth::SLOT_OFF_HAND) == 4, "no_offer: never shirt, tabard, blocked off hand");
    Check(NoOfferSlot(bare, 0) == GearWorth::SLOT_OFF_HAND, "no_offer: an unblocked empty off hand");
    Check(NoOfferSlot(full, 0x7FFFFu) == NO_SLOT, "no_offer: none fillable");
}

int main()
{
    CheckZero();
    CheckGrid();
    CheckCases();
    CheckCeiling();
    CheckHoursPerLevel();
    CheckRoute();
    CheckChoice();
    CheckPick();
    CheckNoOfferSlot();
    PrintLadder();

    std::cout << "\ngear-worth: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
