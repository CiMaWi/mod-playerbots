/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FISHRULES_H
#define _PLAYERBOT_FISHRULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "GatherRules.h"

// The fishing rules (professions plan WR16a, PLAN 3.12, DECISIONS WR.10, WR.12): where and when fishing is worth it.
// The shape (a water's usability by the bot's catch chance, a trip with its ends) is adapted from Solfood's
// FishRules.h in mod-playerbots at e8c3eb42 (Solfood/mod-playerbots, GPL v2): its policy numbers are its realm's and
// are not taken. The catch rule is our core's (GameObject.cpp, the fishing bobber's use): the zone's base skill + 95
// is the skill of a sure catch, below it the chance is (skill / that)^2 in whole percent and at least 1, within 20 yd
// of a fishing pool it is 100. A cast is worth what the fish are worth to this bot (GatherRules.h YieldsWorth: a cook
// gives a fish it can cook the price of the food it replaces as `pathCopper`, a bot short of copper weighs the
// vendor's price heavier; nothing is fished only for sale before the auction house prices it), the chance of a skill
// point at a minute of play (the engine's colours, SkillPath::FishChancePerMille) and, for a bot with the fishing
// interest, the pleasure of casting in spare time; against the minutes of the cast at the minute cost of the place.
// Std only: include it in .cpp files and the planner, never in a central header (the fishing pole is a tool need
// already, ProfessionRules.h TOOLS; a pole in the bags or in hand is `poleHeld`).
namespace Fish
{
// Tuning values [E]
constexpr std::uint16_t FISHING = 356;  // SKILL_FISHING
constexpr int NO_MISS_ABOVE_ZONE = 95;  // the core: a sure catch at the zone's base skill + this
constexpr float POOL_YARDS = 20.0f;     // a pool makes the catch sure this near the bobber
constexpr double FAIR_CHANCE = 0.15;    // the least catch chance a bot accepts at open water (Solfood's 15 %)
constexpr float CAST_MINUTES = 0.5f;    // one cast: walk-free wait for a bite, the click, the loot (a guess; WR25a)
constexpr float TRIP_MINUTES = Planner::Goals::GRIND_MINUTES;  // a trip is planned like a grind spot
constexpr double BAG_FULL_SHARE = 0.8;                         // a trip ends with the bags this full (Solfood's 80 %)
constexpr double BARREN_MISS_ODDS = 0.01;  // a run of casts without a catch that luck gives this rarely ends the trip
constexpr std::uint32_t BARREN_MIN_CASTS = 3;
constexpr std::uint32_t BARREN_MAX_CASTS = 40;
constexpr double INTEREST_PAYS = 1.25;  // a full fishing interest makes a cast worth this many minutes of play
constexpr double PER_MILLE = 1000.0;
constexpr std::size_t NO_PICK = std::numeric_limits<std::size_t>::max();
constexpr double REFUSED = std::numeric_limits<double>::lowest();

// A fishing pool spawn's position (WR16b): a pool lies in fishable water, so it is the planner's place of a trip there
// whether the pool is up or not (the executor probes the shore around it)
struct Spot
{
    std::uint32_t map{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// One water: an area (the core keys the base skill and the open-water loot by area id; the pools by zone) with what
// its fish give per cast at open water and per cast at a pool (items per cast, prices set by the caller)
struct Water
{
    std::uint32_t area{0};
    std::int32_t baseSkill{0};  // skill_fishing_base_level; negative values exist and the engine computes with them
    std::uint32_t pools{0};     // fishing pool spawns in it
    std::vector<Gather::Yield> openYields;
    std::vector<Gather::Yield> poolYields;
    std::vector<Spot> spots;  // its pool spawns' positions (a zone's row; zone 0 spawns resolved by position)
};

// What the bot brings: its fishing skill, its prices and the interest's weight (0 to 1), the pole, the room in its bags
struct Fisher
{
    std::uint32_t skill{0};  // 0: it cannot fish
    std::uint32_t maxSkill{0};
    Materials::Rates rates;
    Recipes::ServerRates server;  // the server's gathering step (WR29: a catch's skill-up); default: stock
    double interest{0.0};         // the weight of Interest::Fishing, 0 when it lacks it
    bool poleHeld{false};
};

// The core's catch chance (0 to 1) at a skill: the sure catch at a pool, whole percent otherwise; 0 at skill 0
inline double CatchChance(std::uint32_t skill, std::int32_t baseSkill, bool atPool)
{
    if (!skill)
        return 0.0;
    if (atPool)
        return 1.0;
    std::int32_t const noMiss = baseSkill + NO_MISS_ABOVE_ZONE;
    if (noMiss <= 0 || static_cast<std::int32_t>(skill) >= noMiss)
        return 1.0;
    double const ratio = static_cast<double>(skill) / static_cast<double>(noMiss);
    return std::max(1, static_cast<int>(ratio * ratio * 100.0)) / 100.0;
}

// The minutes one catch takes at a chance: the named estimate of the water's wait (PLAN 3.20; WR25a corrects it)
inline double MinutesPerCatch(double chance)
{
    return chance > 0.0 ? CAST_MINUTES / chance : std::numeric_limits<double>::infinity();
}

// The water's chance for the bot: at a pool 100 %, else open water's; a water with no pool has no pool chance
inline double ChanceAt(Water const& water, Fisher const& bot, bool atPool)
{
    return CatchChance(bot.skill, water.baseSkill, atPool && water.pools > 0);
}

// Open water is usable at a fair catch chance; a pool (when the water has one) always, at any skill above 0
inline bool Usable(Water const& water, Fisher const& bot, bool atPool)
{
    if (!bot.skill || !bot.poleHeld)
        return false;
    if (atPool)
        return water.pools > 0;
    return ChanceAt(water, bot, false) >= FAIR_CHANCE;
}

// One cast's worth: the catch chance times the fish at the bot's prices, the chance of a skill point (the engine
// rolls it at every use of the bobber, a catch or not), and the interest's pleasure
inline double CastWorth(Water const& water, Fisher const& bot, bool atPool)
{
    double const chance = ChanceAt(water, bot, atPool);
    std::vector<Gather::Yield> const& yields = atPool ? water.poolYields : water.openYields;
    double worth = chance * Gather::YieldsWorth(yields, bot.rates);
    if (bot.skill < bot.maxSkill)
        worth += SkillPath::FishChancePerMille(bot.skill) / PER_MILLE *
                 std::min(bot.server.Step(false), bot.maxSkill - bot.skill) * bot.rates.minuteValue *
                 Planner::Goals::SKILLUP_MINUTES;
    worth += std::clamp(bot.interest, 0.0, 1.0) * INTEREST_PAYS * bot.rates.minuteValue * CAST_MINUTES;
    return worth;
}

// A cast's worth less its minutes at the place's minute cost; REFUSED where the bot may not fish (unusable water,
// a refused place)
inline double Net(Water const& water, Fisher const& bot, Materials::PlaceRisk const& risk, bool atPool)
{
    double const minuteCost = Materials::MinuteCost(risk, bot.rates);
    if (!Usable(water, bot, atPool) || minuteCost < 0.0)
        return REFUSED;
    return CastWorth(water, bot, atPool) - CAST_MINUTES * minuteCost;
}

// A water in the bot's reach with the place's risk
struct Candidate
{
    Water water;
    Materials::PlaceRisk risk;
};

// The water worth a trip: the best net above 0, a pool before open water of the same net (the sure catch); ties to
// the lower area id. NO_PICK when none pays. `pool` tells which way the pick is fished
inline std::size_t Pick(std::vector<Candidate> const& candidates, Fisher const& bot, bool& pool)
{
    std::size_t best = NO_PICK;
    double bestNet = 0.0;
    pool = false;
    for (std::size_t at = 0; at < candidates.size(); ++at)
        for (bool const way : {true, false})
        {
            double const net = Net(candidates[at].water, bot, candidates[at].risk, way);
            if (!(net > 0.0))
                continue;
            bool better = best == NO_PICK || net > bestNet;
            if (!better && net == bestNet)
                better = (way && !pool) || (way == pool && candidates[at].water.area < candidates[best].water.area);
            if (better)
            {
                best = at;
                bestNet = net;
                pool = way;
            }
        }
    return best;
}

// Casts without a catch after which the water counts as giving nothing: the run luck gives only BARREN_MISS_ODDS
inline std::uint32_t BarrenCasts(double chance)
{
    if (!(chance > 0.0))
        return BARREN_MAX_CASTS;
    if (chance >= 1.0)
        return BARREN_MIN_CASTS;
    double const casts = std::ceil(std::log(BARREN_MISS_ODDS) / std::log(1.0 - chance));
    return static_cast<std::uint32_t>(
        std::clamp(casts, static_cast<double>(BARREN_MIN_CASTS), static_cast<double>(BARREN_MAX_CASTS)));
}

// Casts refused in a row at one stand after which it is lost (a cast seen channelling resets the count, WR27 follow-up:
// the counting in total ended a trip of 66 catches at 3 refusals in 28)
constexpr std::uint32_t MAX_REFUSED_CASTS = 3;

inline std::uint32_t RefusedAfter(std::uint32_t refused, bool channelling) { return channelling ? 0u : refused + 1u; }

inline bool StandLost(std::uint32_t refused) { return refused >= MAX_REFUSED_CASTS; }

enum class TripEnd : std::uint8_t
{
    None,  // fishing on
    Fight,
    BagsFull,
    TimeUp,
    Barren,
    Count
};
constexpr std::size_t TRIP_ENDS = static_cast<std::size_t>(TripEnd::Count);

// The trip so far
struct Trip
{
    bool cast{false};           // it cast at the water at least once
    float minutes{0.0f};        // at the water: since its first cast (0 before it)
    float statusMinutes{0.0f};  // since the trip began, the walk to the water included
    float planMinutes{TRIP_MINUTES};
    bool inCombat{false};
    double bagShare{0.0};  // filled share of the bags, 0 to 1
    std::uint32_t missesInARow{0};
    double chance{1.0};  // the catch chance of this water for the bot
};

// A trip ends in a fight, at full bags, after its time at the water, or when the place gives nothing (PLAN 3.12), in
// that order
inline TripEnd Ends(Trip const& trip)
{
    if (trip.inCombat)
        return TripEnd::Fight;
    if (trip.bagShare >= BAG_FULL_SHARE)
        return TripEnd::BagsFull;
    if (trip.cast && trip.minutes >= trip.planMinutes)
        return TripEnd::TimeUp;
    if (trip.missesInARow >= BarrenCasts(trip.chance))
        return TripEnd::Barren;
    return TripEnd::None;
}

// The trip's general bound, the status deadline (WR27): the trip's own ends come first, so one still on after this many
// times its minutes is a trip whose ends failed. The clock is its time at the water once it cast, else its age (the
// walk there). The night lab5-20261122-030123 had the deadline at the trip's own minutes, counted with the walk: the
// status check ran first and ended all 384 trips that fished their time (WR27-record.md). As a planned status's
// ORDER_OVERRUN_FACTOR (GoalInterface.h)
constexpr float DEADLINE_FACTOR = 2.0f;

inline bool PastDeadline(Trip const& trip)
{
    return (trip.cast ? trip.minutes : trip.statusMinutes) > DEADLINE_FACTOR * trip.planMinutes;
}

// The planner's minutes for a trip: its time, the casts that fit and the catches expected in them
struct TripPlan
{
    float minutes{0.0f};
    double casts{0.0};
    double catches{0.0};
};

inline TripPlan PlanTrip(Water const& water, Fisher const& bot, bool atPool)
{
    double const chance = ChanceAt(water, bot, atPool);
    double const casts = static_cast<double>(TRIP_MINUTES) / CAST_MINUTES;
    return {TRIP_MINUTES, casts, casts * chance};
}

// The planner's option of a trip to the water (WR16b; GoalModel AddWaters makes it a GoalKind::Fish option): id the
// water's area, the place the spot, the trip's minutes, the expected catches' worth split like a gather circuit's
// (copper a vendor pays, item worth the skill path pays beyond it), the skill points of its casts and the interest's
// minutes of play. An empty option (id 0) where the water does not pay (Net)
inline Planner::Goals::GatherNode TripOption(Water const& water, Fisher const& bot, Materials::PlaceRisk const& risk,
                                             bool atPool, Spot const& at, float walkMinutes)
{
    Planner::Goals::GatherNode option;
    if (!(Net(water, bot, risk, atPool) > 0.0))
        return option;
    TripPlan const plan = PlanTrip(water, bot, atPool);
    double copper = 0.0;
    double item = 0.0;
    double units = 0.0;
    for (Gather::Yield const& yield : atPool ? water.poolYields : water.openYields)
    {
        units += std::max(yield.perNode, 0.0) * plan.catches;
        Gather::UnitWorth const unit = Gather::UnitOf(yield, bot.rates);
        double const worth = std::max(yield.perNode, 0.0) * plan.catches * unit.each;
        if (unit.used)
            item += worth;
        else
            copper += worth;
    }
    double const room = bot.maxSkill > bot.skill ? static_cast<double>(bot.maxSkill - bot.skill) : 0.0;
    option.id = water.area;
    option.skill = FISHING;
    option.place = {at.map, at.x, at.y, at.z};
    option.walkMinutes = std::max(walkMinutes, 0.0f);
    option.gatherMinutes = plan.minutes;
    option.takes = static_cast<float>(plan.catches);
    option.units = static_cast<float>(units);
    option.copperWorth = static_cast<float>(copper);
    option.itemWorth = static_cast<float>(item);
    option.skillPoints = static_cast<float>(
        std::min(plan.casts * SkillPath::FishChancePerMille(bot.skill) / PER_MILLE * bot.server.Step(false), room));
    option.pleasureMinutes =
        static_cast<float>(std::clamp(bot.interest, 0.0, 1.0) * INTEREST_PAYS * CAST_MINUTES * plan.casts);
    option.levelsAbove = risk.levelsAbove;
    option.eliteShare = risk.eliteShare;
    return option;
}

// The fishing waters by area, as the startup gather publishes them (sorted by area; immutable once built)
struct Index
{
    std::vector<Water> waters;
    Recipes::ServerRates rates;  // the yields are at these; a fisher's step comes from them (WR29)

    [[nodiscard]] Water const* Find(std::uint32_t area) const
    {
        auto const at = std::lower_bound(waters.begin(), waters.end(), area,
                                         [](Water const& water, std::uint32_t id) { return water.area < id; });
        return at != waters.end() && at->area == area ? &*at : nullptr;
    }

    // The core's lookup: the subzone's own row, else its zone's
    [[nodiscard]] Water const* Find(std::uint32_t subzone, std::uint32_t zone) const
    {
        if (Water const* own = Find(subzone))
            return own;
        return Find(zone);
    }
};
}  // namespace Fish

#endif
