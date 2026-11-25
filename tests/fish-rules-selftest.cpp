/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the fishing rules (src/Ai/World/Rpg/FishRules.h, professions plan WR16a): hand cases for the
// core's catch chance, a water below a fair chance refused, a cook valuing fish it can cook, the interest raising
// spare-time fishing, the pole, and a trip ending at each of its ends; then a seeded search over random waters and
// bots for the invariants: a harder-to-catch water is never easier, no fishing at skill 0, a trip always ends, a pool
// never lowers the chance, a refused water is never picked, the answers are deterministic. Negative controls (an
// inverted difficulty, a trip that never ends, fishing at skill 0) must be caught by the same checkers.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "FishRules.h"

using Fish::Candidate;
using Fish::Fisher;
using Fish::Trip;
using Fish::TripEnd;
using Fish::Water;

namespace
{
constexpr std::uint32_t SEED = 20261122;
constexpr int WORLDS = 800;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << '\n';
    }
}

bool Near(double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b)); }

Fisher BotWith(std::uint32_t skill)
{
    Fisher bot;
    bot.skill = skill;
    bot.maxSkill = skill;  // no skill point unless a case raises the cap
    bot.rates.exchange = 1.0f;
    bot.rates.factor = 1.0f;
    bot.rates.minuteValue = 20.0f;
    bot.rates.cautionWeight = 1.0f;
    bot.poleHeld = true;
    return bot;
}

Water WaterOf(std::uint32_t area, std::int32_t base, std::uint32_t pools = 0)
{
    Water water;
    water.area = area;
    water.baseSkill = base;
    water.pools = pools;
    return water;
}

void HandCases()
{
    // The core's rule: (skill / (base + 95))^2 in whole percent, at least 1, sure from the base + 95
    Check(Near(Fish::CatchChance(100, 105, false), 0.25), "skill 100 at base 105: (100/200)^2 = 25 %");
    Check(Near(Fish::CatchChance(1, 300, false), 0.01), "skill 1 at base 300: at least 1 %");
    Check(Near(Fish::CatchChance(395, 300, false), 1.0), "skill at base + 95: sure");
    Check(Near(Fish::CatchChance(450, 300, false), 1.0), "skill above base + 95: sure");
    Check(Near(Fish::CatchChance(0, 0, false), 0.0) && Near(Fish::CatchChance(0, 0, true), 0.0),
          "skill 0 catches nothing");
    Check(Near(Fish::CatchChance(10, 300, true), 1.0), "a pool: sure at any skill above 0");
    // Negative bases: the engine computes with them (-70 + 95 = 25)
    Check(Near(Fish::CatchChance(10, -70, false), 0.16), "base -70, skill 10: (10/25)^2 = 16 %");
    Check(Near(Fish::CatchChance(25, -70, false), 1.0), "base -70, skill 25: sure");
    Check(Near(Fish::CatchChance(5, -95, false), 1.0), "a base that leaves no room above 0: sure, no division");
    Check(Near(Fish::MinutesPerCatch(0.5), Fish::CAST_MINUTES * 2.0), "the water's wait: a cast over the chance");
    Check(std::isinf(Fish::MinutesPerCatch(0.0)), "no chance: no catch ever");

    // A water below a fair catch chance is refused; a pool is not
    Fisher const low = BotWith(20);
    Water far = WaterOf(1, 300);
    far.openYields = {{500, 1.0, 0, 5000, 0}};
    Materials::PlaceRisk const quiet;
    Check(Fish::ChanceAt(far, low, false) < Fish::FAIR_CHANCE, "the case: skill 20 at base 300 is below fair");
    Check(!Fish::Usable(far, low, false) && Fish::Net(far, low, quiet, false) == Fish::REFUSED,
          "a water below a fair catch chance is refused");
    Water pooled = far;
    pooled.pools = 3;
    pooled.poolYields = far.openYields;
    Check(Fish::Usable(pooled, low, true) && Fish::Net(pooled, low, quiet, true) > 0.0,
          "its pool is usable and pays at the same skill");
    Check(!Fish::Usable(far, low, true), "a water without a pool has none to fish");
    Fisher const fair = BotWith(100);
    Water easy = WaterOf(2, 5);
    easy.openYields = far.openYields;
    Check(Fish::Usable(easy, fair, false) && Fish::Net(easy, fair, quiet, false) > 0.0,
          "an easy water pays and is usable");

    // The pole: bought, never handed out; no pole, no trip
    Fisher poleless = fair;
    poleless.poleHeld = false;
    Check(!Fish::Usable(easy, poleless, false), "no pole in the bags or in hand: no fishing");
    Fisher const unskilled = BotWith(0);
    Check(!Fish::Usable(easy, unskilled, false) && !Fish::Usable(pooled, unskilled, true), "skill 0: no fishing");

    // A cook values fish it can cook: the same fish at the price of the food it replaces
    Water fishy = WaterOf(3, 5);
    fishy.openYields = {{600, 1.0, 0, 0, 0}};  // worth nothing to sell: nothing is fished only for sale
    Water cooked = fishy;
    cooked.openYields = {{600, 1.0, 4000, 0, 0}};  // a cook: its own use saves the food's price
    Fisher const plain = BotWith(100);
    Check(Fish::CastWorth(cooked, plain, false) > Fish::CastWorth(fishy, plain, false),
          "a cook values fish it can cook");
    Check(Fish::Net(cooked, plain, quiet, false) > 0.0 && Fish::Net(fishy, plain, quiet, false) < 0.0,
          "fish worth nothing to a bot do not pay a trip, the cook's do");
    Water sold = fishy;
    sold.openYields = {{601, 1.0, 0, 300, 0}};
    Fisher poor = plain;
    poor.rates.factor = 3.0f;
    Check(Fish::CastWorth(sold, poor, false) > Fish::CastWorth(sold, plain, false), "a poor bot values copper more");

    // The interest raises spare-time fishing: a water that pays nothing else is fished with it only
    Fisher keen = plain;
    keen.interest = 1.0;
    Fisher mild = plain;
    mild.interest = 0.3;
    Check(Fish::Net(fishy, keen, quiet, false) > 0.0, "a full interest fishes the empty water in spare time");
    Check(Fish::Net(fishy, mild, quiet, false) < Fish::Net(fishy, keen, quiet, false) &&
              Fish::Net(fishy, mild, quiet, false) > Fish::Net(fishy, plain, quiet, false),
          "the interest raises the worth with its weight");

    // A skill point is worth a minute's play while the skill can rise
    Fisher learner = plain;
    learner.maxSkill = 300;
    Check(Fish::CastWorth(fishy, learner, false) > Fish::CastWorth(fishy, plain, false), "a skill point adds worth");

    // A refused place: too dangerous
    Materials::PlaceRisk deadly;
    deadly.deathsPerHour = 1000.0f;
    deadly.levelsAbove = 20.0f;
    deadly.eliteShare = 1.0f;
    deadly.enemyShare = 1.0f;
    Check(Fish::Net(cooked, plain, deadly, false) == Fish::REFUSED, "a refused place is never fished");

    // The pick: the water that pays most; none when nothing pays; a pool where open water is refused
    std::vector<Candidate> const candidates = {{fishy, quiet}, {cooked, quiet}, {easy, quiet}};
    bool pool = true;
    std::size_t at = Fish::Pick(candidates, plain, pool);
    Check(at == 2 && !pool, "the pick is the water that pays most (the vendor's fish over the cook's here)");
    Check(Fish::Pick({{fishy, quiet}}, plain, pool) == Fish::NO_PICK, "no pick when nothing pays");
    std::vector<Candidate> const sure = {{pooled, quiet}};
    at = Fish::Pick(sure, low, pool);
    Check(at == 0 && pool, "a low-skill bot goes to the pool where open water is refused");

    // A trip ends at each of its ends, in order
    Trip trip;
    Check(Fish::Ends(trip) == TripEnd::None, "a fresh trip goes on");
    trip.statusMinutes = 99.0f;
    Check(Fish::Ends(trip) == TripEnd::None, "its time runs at the water: a trip that never cast has no time_up");
    trip.cast = true;
    trip.inCombat = true;
    trip.bagShare = 1.0;
    trip.minutes = 99.0f;
    Check(Fish::Ends(trip) == TripEnd::Fight, "a fight ends it first");
    trip.inCombat = false;
    Check(Fish::Ends(trip) == TripEnd::BagsFull, "full bags end it");
    trip.bagShare = Fish::BAG_FULL_SHARE - 0.01;
    Check(Fish::Ends(trip) == TripEnd::TimeUp, "its time ends it");
    trip.minutes = 1.0f;
    trip.chance = 0.25;
    trip.missesInARow = Fish::BarrenCasts(0.25) - 1;
    Check(Fish::Ends(trip) == TripEnd::None, "one miss short of barren goes on");
    trip.missesInARow = Fish::BarrenCasts(0.25);
    Check(Fish::Ends(trip) == TripEnd::Barren, "a water that gives nothing ends it");
    Check(Fish::BarrenCasts(0.15) > Fish::BarrenCasts(0.5) && Fish::BarrenCasts(1.0) == Fish::BARREN_MIN_CASTS &&
              Fish::BarrenCasts(0.0) == Fish::BARREN_MAX_CASTS,
          "the barren run grows as the chance falls and stays bounded");
    // Its own time ends a trip before the deadline, whatever the walk took (WR27: the deadline at the trip's minutes,
    // counted with the walk, ended every trip that fished its time on the night lab5-20261122-030123)
    Trip due;
    due.statusMinutes = 2.0f * Fish::TRIP_MINUTES - 0.1f;
    Check(!Fish::PastDeadline(due), "a walk within twice the trip's minutes is no deadline");
    due.statusMinutes = 2.0f * Fish::TRIP_MINUTES + 0.1f;
    Check(Fish::PastDeadline(due), "a walk that never reached the water ends at the deadline");
    due.cast = true;
    due.minutes = Fish::TRIP_MINUTES + 0.5f;
    due.statusMinutes = due.minutes + 15.0f;
    Check(Fish::Ends(due) == TripEnd::TimeUp && !Fish::PastDeadline(due),
          "its time at the water ends a trip after a long walk, before the deadline");
    due.minutes = 2.0f * Fish::TRIP_MINUTES + 0.1f;
    Check(Fish::PastDeadline(due), "a trip whose ends failed meets the deadline");
    // Refused casts count in a row: a cast seen channelling resets them (WR27 follow-up). The control counts them in
    // total, as the code did until WR25a, and loses a stand that fished 66 times
    {
        std::vector<bool> const casts = {true, false, true, false, false, true, false, true, false, false, true, true};
        auto const lost = [&casts](bool inARow)
        {
            std::uint32_t refused = 0;
            for (bool const channelled : casts)
            {
                refused = inARow ? Fish::RefusedAfter(refused, channelled) : refused + (channelled ? 0u : 1u);
                if (Fish::StandLost(refused))
                    return true;
            }
            return false;
        };
        Check(!lost(true), "a stand with refusals between good casts is kept");
        Check(lost(false), "control: refusals counted in total lose that stand");
        std::uint32_t refused = 0;
        for (std::uint32_t i = 0; i < Fish::MAX_REFUSED_CASTS - 1; ++i)
            refused = Fish::RefusedAfter(refused, false);
        Check(!Fish::StandLost(refused) && Fish::StandLost(Fish::RefusedAfter(refused, false)) &&
                  !Fish::StandLost(Fish::RefusedAfter(refused, true)),
              "refused casts in a row lose the stand, a channel resets");
    }
    // The estimate on the trip's option (WR25a): the plan's catches, and the units those give
    Water twoEach = easy;
    twoEach.openYields = {{500, 2.0, 0, 5000, 0}};
    Planner::Goals::GatherNode const trip = Fish::TripOption(twoEach, fair, quiet, false, Fish::Spot{}, 1.0f);
    Fish::TripPlan const plan = Fish::PlanTrip(easy, fair, false);
    Check(trip.id && Near(trip.takes, plan.catches) && Near(trip.units, 2.0 * plan.catches) &&
              Near(trip.gatherMinutes, plan.minutes) && trip.fightMinutes == 0.0f,
          "estimate: the trip's option carries the plan's catches, minutes and units");
    Check(Near(plan.casts, Fish::TRIP_MINUTES / Fish::CAST_MINUTES) &&
              Near(plan.catches, plan.casts * Fish::ChanceAt(easy, fair, false)),
          "a trip's casts and expected catches");

    // The index finds by area, a subzone before its zone
    Fish::Index index;
    index.waters = {WaterOf(10, 0), WaterOf(20, 50), WaterOf(30, 100)};
    Check(index.Find(20) && index.Find(20)->baseSkill == 50 && !index.Find(25), "the index finds by area");
    Check(index.Find(20, 30)->area == 20 && index.Find(25, 30)->area == 30 && !index.Find(25, 35),
          "a subzone's row before its zone's");
}

struct Rules
{
    std::function<double(std::uint32_t, std::int32_t, bool)> chance = Fish::CatchChance;
    bool tripEnds = true;
    bool fishAtZeroSkill = false;
    std::function<bool(Trip const&)> pastDeadline = Fish::PastDeadline;
};

struct Violations
{
    int harder{0};
    int zeroSkill{0};
    int endless{0};
    int deadline{0};  // a trip the status deadline ended before its own ends
    int poolLowers{0};
    int refusedPicked{0};
    int nondeterministic{0};
};

void SearchWorld(std::mt19937& rng, Rules const& rules, Violations& v)
{
    auto const u = [&rng](int from, int to) { return std::uniform_int_distribution<int>(from, to)(rng); };
    Fisher bot = BotWith(static_cast<std::uint32_t>(u(0, 450)));
    bot.maxSkill = bot.skill + static_cast<std::uint32_t>(u(0, 3)) * 100;
    bot.interest = u(0, 10) / 10.0;
    bot.poleHeld = u(0, 9) != 0;

    // A harder water is never easier at the same skill; a pool never lowers the chance
    std::int32_t const base = u(-70, 480);
    std::int32_t const harder = base + u(1, 100);
    for (bool const pool : {false, true})
        if (rules.chance(bot.skill, harder, pool) > rules.chance(bot.skill, base, pool) + 1e-12)
            ++v.harder;
    if (rules.chance(bot.skill, base, true) + 1e-12 < rules.chance(bot.skill, base, false))
        ++v.poolLowers;

    std::vector<Candidate> candidates;
    int const count = u(1, 6);
    for (int i = 0; i < count; ++i)
    {
        Candidate candidate;
        candidate.water =
            WaterOf(static_cast<std::uint32_t>(100 + i), u(-70, 480), static_cast<std::uint32_t>(u(0, 2)));
        candidate.water.openYields = {
            {700u + static_cast<std::uint32_t>(i), u(0, 20) / 10.0, 0u, static_cast<std::uint32_t>(u(0, 800)), 0u}};
        candidate.water.poolYields = candidate.water.openYields;
        candidate.risk.deathsPerHour = static_cast<float>(u(0, 6));
        candidates.push_back(candidate);
    }
    bool pool = false;
    std::size_t const pick = Fish::Pick(candidates, bot, pool);
    bool again = !pool;
    if (Fish::Pick(candidates, bot, again) != pick || (pick != Fish::NO_PICK && again != pool))
        ++v.nondeterministic;
    if (pick != Fish::NO_PICK)
    {
        Candidate const& chosen = candidates[pick];
        if (Fish::Net(chosen.water, bot, chosen.risk, pool) == Fish::REFUSED || !Fish::Usable(chosen.water, bot, pool))
            ++v.refusedPicked;
        if (!bot.skill)
            ++v.zeroSkill;
    }
    // The control that fishes at skill 0 is caught by asking the rule itself
    if (!bot.skill && (rules.fishAtZeroSkill || Fish::Usable(candidates[0].water, bot, false)))
        ++v.zeroSkill;

    // A trip always ends, by its own rule before the status deadline: after a walk of up to its minutes, casts at the
    // water's chance that last up to a whole channel and its loot (CAST_MINUTES) end within a bounded count. The status
    // check runs first in an engine pass, the trip's ends only between casts (NewRpgStatus.cpp, NewRpgDoFish.cpp)
    double const chance = rules.chance(bot.skill, base, false);
    Trip trip;
    trip.planMinutes = static_cast<float>(u(3, 30));
    trip.statusMinutes = static_cast<float>(u(0, static_cast<int>(trip.planMinutes)));
    trip.chance = std::max(chance, 0.01);
    trip.bagShare = u(0, 60) / 100.0;
    std::uniform_real_distribution<double> roll(0.0, 1.0);
    bool ended = false;
    for (int cast = 0; cast < 1000 && !ended; ++cast)
    {
        float const length = Fish::CAST_MINUTES * u(1, 10) / 10.0f;
        trip.cast = true;
        trip.minutes += length;
        trip.statusMinutes += length;
        if (rules.pastDeadline(trip))
        {
            ++v.deadline;
            break;
        }
        if (roll(rng) < chance)
        {
            trip.missesInARow = 0;
            trip.bagShare += 0.01;
        }
        else
            ++trip.missesInARow;
        ended = rules.tripEnds && Fish::Ends(trip) != TripEnd::None;
    }
    if (!ended)
        ++v.endless;
}

Violations Search(Rules const& rules)
{
    std::mt19937 rng(SEED);
    Violations v;
    for (int world = 0; world < WORLDS; ++world)
        SearchWorld(rng, rules, v);
    return v;
}

void SearchCases()
{
    Violations const v = Search({});
    Check(v.harder == 0, "a harder water is never easier: " + std::to_string(v.harder));
    Check(v.zeroSkill == 0, "no fishing at skill 0: " + std::to_string(v.zeroSkill));
    Check(v.endless == 0, "a trip always ends: " + std::to_string(v.endless));
    Check(v.deadline == 0, "a trip ends by its own rule before the deadline: " + std::to_string(v.deadline));
    Check(v.poolLowers == 0, "a pool never lowers the chance: " + std::to_string(v.poolLowers));
    Check(v.refusedPicked == 0, "a refused water is never picked: " + std::to_string(v.refusedPicked));
    Check(v.nondeterministic == 0, "the same input gives the same answer: " + std::to_string(v.nondeterministic));

    // Negative controls: the same checkers must see a broken rule
    Rules inverted;
    inverted.chance = [](std::uint32_t skill, std::int32_t base, bool pool)
    { return Fish::CatchChance(skill, -base, pool); };
    Check(Search(inverted).harder > 0, "control: an inverted difficulty is caught");
    Rules never;
    never.tripEnds = false;
    Check(Search(never).endless > 0, "control: a trip that never ends is caught");
    Rules night;
    night.pastDeadline = [](Trip const& trip) { return trip.statusMinutes > trip.planMinutes; };
    Check(Search(night).deadline > 0, "control: the night's deadline at the trip's minutes with the walk is caught");
    Rules zero;
    zero.fishAtZeroSkill = true;
    Check(Search(zero).zeroSkill > 0, "control: fishing at skill 0 is caught");
    Rules lowering;
    lowering.chance = [](std::uint32_t skill, std::int32_t base, bool pool)
    { return pool ? 0.5 * Fish::CatchChance(skill, base, false) : Fish::CatchChance(skill, base, false); };
    Check(Search(lowering).poolLowers > 0, "control: a pool that lowers the chance is caught");
}
}  // namespace

int main()
{
    HandCases();
    SearchCases();
    std::cout << "FISH rules selftest: " << (passed + failed) << " checks, " << failed << " failed ("
              << (failed ? "FAIL" : "PASS") << ")\n";
    return failed ? 1 : 0;
}
