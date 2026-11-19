/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PROFESSIONCONFIG_H
#define _PLAYERBOT_PROFESSIONCONFIG_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

template <typename Struct>
struct ConfigFamily;

// The numbers of persistent bots' professions (Playerbots.Professions.*), read once at config load
// (PlayerbotAIConfig::Professions) through the rows in ProfessionConfig.cpp; the initial values are the defaults. Kept
// apart from PlayerbotAIConfig.h so that later members do not rebuild every user of that header. Std only, so the
// selftests include it as it is.
struct ProfessionConfig
{
    // Learn.GathererLevel (1-80): the level from which a bot learns its gathering profession (the trainers ask 5)
    std::uint32_t GathererLevel = 5;
    // Craft.VisitDeadlineSeconds (60-3600): seconds a craft visit (the walk to a station included) may take before it
    // is given up
    std::uint32_t CraftVisitDeadlineSeconds = 900;
    // Fish.TripDeadlineSeconds (60-3600): seconds a fishing trip (the walk to the water included) may take before it
    // is given up
    std::uint32_t FishTripDeadlineSeconds = 1200;
    // VisitStepSeconds (1-60): seconds one step of a profession visit (a trainer's answer, a cast's result) may wait
    std::uint32_t VisitStepSeconds = 10;
    // Share.<Craft> (0-100): the global share of each craft over the roster, in the order of ProfessionRules::CRAFTS
    // (blacksmithing, leatherworking, tailoring, engineering, jewelcrafting, alchemy, inscription, enchanting); the
    // table is normalised, so the values are weights (WR.2). Equal by default
    std::array<float, 8> CraftShare = {12.5f, 12.5f, 12.5f, 12.5f, 12.5f, 12.5f, 12.5f, 12.5f};
    // Bias.<Class>.<Craft> (0.1-10): how a class leans among the crafts it considers, index class slot * 8 + craft
    // (ProfessionRules::Cell); 0 outside the class's table, which is no option. The initializer is all zero: the
    // family's defaults (ProfessionConfig.cpp) hold the table's values, the hunters' engineering at 2
    std::array<float, 80> ClassBias{};
    // Derived at load (the family's Finish), not a key: each class's chance to roll each craft first, fitted so the
    // roster holds the shares (ProfessionRules::FitChances). All zero until the config loads: a roll then takes the
    // class's crafts evenly
    std::array<double, 80> Chance{};
    // Derived with Chance: the crafts (bit per index) whose share no class can reach
    std::uint32_t UnreachableCrafts = 0;
};

// The words of the crafts whose share the fit could not reach, "blacksmithing, leatherworking"; empty when all are
std::string ProfessionUnreachableCrafts(ProfessionConfig const& config);

extern ConfigFamily<ProfessionConfig> const ProfessionFamily;

// The professions plan's words (PLAN 3.15), fixed here once; .claude/docs/world-log.md lists them
namespace Professions
{
// Event types. The honesty row of a profession grant that still happens is HonestyProfession:
// EventLog::Honesty(bot, HONESTY_PROFESSION, site), the word of UpkeepKind::Profession (UpkeepRules.h)
constexpr char const* HONESTY_PROFESSION = "Profession";
constexpr char const* EVENT_PROFESSION_HELD =
    "ProfessionHeld";  // a bot's professions and skill, at login and on change
constexpr char const* EVENT_PROFESSION_VISIT = "ProfessionVisit";  // a profession trainer stop: what was bought
constexpr char const* EVENT_CRAFT_VISIT = "CraftVisit";            // how a craft visit ended, what was made
constexpr char const* EVENT_FISH_TRIP = "FishTrip";                // how a fishing trip ended, what was caught

// The `profession` decision row's choices and reasons (WorldLogFormat::DecisionKind::Profession)
enum class Choice : std::uint8_t
{
    Learn,
    Rank,
    Recipe,
    Tool,
    Craft,
    Gather,
    Fish,
    Keep,
    Skip
};
constexpr std::size_t CHOICE_COUNT = 9;
constexpr std::array<std::string_view, CHOICE_COUNT> CHOICE_WORDS = {"learn",  "rank", "recipe", "tool", "craft",
                                                                     "gather", "fish", "keep",   "skip"};

enum class Reason : std::uint8_t
{
    UseSoon,
    HeldRecipe,
    Access,
    Income,
    CheapestPoint,
    Supply,
    OnTheWay,
    Circuit,
    NotWorthIt,
    DetourTooLong,
    TooDangerous,
    NoSource,
    NoStation,
    GoldShort,
    Locked
};
constexpr std::size_t REASON_COUNT = 15;
constexpr std::array<std::string_view, REASON_COUNT> REASON_WORDS = {
    "use_soon",      "held_recipe", "access",     "income",       "cheapest_point",
    "supply",        "on_the_way",  "circuit",    "not_worth_it", "detour_too_long",
    "too_dangerous", "no_source",   "no_station", "gold_short",   "locked"};

constexpr std::string_view ChoiceWord(Choice choice)
{
    std::size_t const index = static_cast<std::size_t>(choice);
    return index < CHOICE_COUNT ? CHOICE_WORDS[index] : std::string_view("other");
}

constexpr std::string_view ReasonWord(Reason reason)
{
    std::size_t const index = static_cast<std::size_t>(reason);
    return index < REASON_COUNT ? REASON_WORDS[index] : std::string_view("other");
}
}  // namespace Professions

#endif
