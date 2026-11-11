/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Encounter harness of the world PvP fight rules (src/Ai/World/Pvp/FightRules.h, WP3b). 1000 persistent bots of both
// factions (traits as TraitRules::Roll gives them, levels 1-80) play 300 hours on one shared clock: they meet enemies
// at WP2b's lab rate (about 3.1 new enemies per bot hour, with its colour mix) and at 2x and 4x, DecideStart rules on
// each meeting, and the fights run a simple strength model with bystanders joining, retreats, the chase, the
// timeouts, the ghost's wait, calm areas and grudges. A neutral pass (soft factors 1) pins the budget: aggression 0
// starts once per 4 h within 15 % at every density; +1's budget is four times 0's (WP.16), and its starts, trimmed by
// the cap and the gap, only have to stay above 0's; the real pass shows what the soft factors make of it, and the
// fights. A quiet world (0.1 meetings per bot hour) checks the cap on the first chance after a long quiet time.
// Cohorts by bot index: x0 aggression 0, x1 aggression +1, x2 aggression -0.75, the rest as rolled. The context
// shares (unflagged, graveyards, busy targets, objectives, bystanders) are synthetic. Build and run (Git Bash, module
// root):
//   clang-cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /I src/Ai/World/Pvp /I src/Ai/World/Rpg
//       tests/fight-encounter-selftest.cpp
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <initializer_list>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "FightRules.h"
#include "TraitRules.h"

using WorldPvp::ChaseLimit;
using WorldPvp::Colour;
using WorldPvp::EndCause;
using WorldPvp::EndedFight;
using WorldPvp::FightClock;
using WorldPvp::FightReason;
using WorldPvp::FightRuleValues;
using WorldPvp::GhostStep;
using WorldPvp::JoinDecision;
using WorldPvp::JoinInput;
using WorldPvp::JoinWhy;
using WorldPvp::Pick;
using WorldPvp::Refusal;
using WorldPvp::StartBot;
using WorldPvp::StartDecision;
using WorldPvp::StartTarget;

namespace
{
constexpr uint32_t SECOND = 1000;
constexpr uint32_t MINUTE = 60 * SECOND;
constexpr uint32_t HOUR = 60 * MINUTE;

constexpr uint32_t BOTS = 1000;
constexpr uint32_t HOURS = 300;
constexpr uint32_t MAX_LEVEL = 80;
constexpr int32_t NONE = -1;

// WP2b's 1x floor by colour, meetings per bot hour (grey, green, yellow, orange, red; 3.1 in all)
constexpr std::array<double, 5> MEETINGS_1X = {0.11, 0.56, 1.735, 0.31, 0.38};
constexpr double QUIET_MEETINGS = 0.1;

// Synthetic context of a meeting
constexpr double UNFLAGGED = 0.10;
constexpr double BOT_NEAR_GRAVEYARD = 0.04;
constexpr double TARGET_NEAR_GRAVEYARD = 0.04;
constexpr double TARGET_BUSY = 0.30;
constexpr double OBJECTIVE = 0.01;
// Other players of each side within ScanYards at a meeting, and bystanders within 80 yd of a new fight, x density;
// more pass by each second
constexpr double NEARBY_PER_SIDE = 0.15;
constexpr double BYSTANDERS_AT_START = 0.5;
constexpr double ARRIVALS_PER_SECOND = 0.01;
constexpr float BYSTANDER_YARDS = 80.0f;
// A bystander's goal: idle or grinding (value 0) for this share, else uniform 0..1 (WT14's value per minute, WP7b)
constexpr double IDLE_SHARE = 0.5;

// The strength model, per second: a hit takes this share of an even-level enemy's health (x the level step per
// level), the chased are hit less, retreats run at this speed from this far inside the fight's edge
constexpr float DAMAGE = 0.08f;
constexpr double HIT_CHANCE = 0.75;
constexpr double CHASE_HIT_CHANCE = 0.5;
constexpr float FLEE_YARDS_PER_SECOND = 7.0f;
constexpr float FLEE_START_INSIDE = 20.0f;
// Lulls without hits (kiting, line of sight) and players leaving (a mount, a flight, a logout)
constexpr double LULL_PER_SECOND = 1.0 / 90.0;
constexpr double LULL_MEAN_SECONDS = 20.0;
constexpr double LEAVE_PER_SECOND = 1.0 / 600.0;
// Out of combat health comes back in this long; a resurrection gives half
constexpr float REGEN_MS = 90.0f * SECOND;
constexpr float REZ_HEALTH = 0.5f;
// The ghost: release, a corpse run, an enemy that lingers at the corpse after the fight, the spirit healer's graveyard
constexpr uint32_t RELEASE_MS = 5 * SECOND;
constexpr double CORPSE_RUN_MIN_SECONDS = 60.0;
constexpr double CORPSE_RUN_MAX_SECONDS = 180.0;
constexpr double LINGER_SHARE = 0.2;
constexpr double LINGER_MIN_SECONDS = 15.0;
constexpr double LINGER_MAX_SECONDS = 120.0;
constexpr uint32_t AT_GRAVEYARD_MS = 2 * MINUTE;
// The in-combat flag of a player outlives its last hit by about this long (the book's grace starts after it)
constexpr uint32_t COMBAT_FLAG_MS = 5 * SECOND;
// The world: a square (wrapping) whose side shrinks with density; bots drift through it
constexpr float WORLD_YARDS = 20000.0f;
constexpr float DRIFT_YARDS_PER_SECOND = 1.0f;
constexpr double PI = 3.14159265358979323846;
// A long quiet time for the cap check
constexpr uint32_t QUIET_MS = 8 * HOUR;
// The exit's budget at aggression 0 (WP.2): once per 4 h of play, within 15 %
constexpr double STARTS_AT_ZERO = 0.25;
constexpr double STARTS_TOLERANCE = 0.15;

enum Cohort : uint8_t
{
    COHORT_ZERO,
    COHORT_ONE,
    COHORT_PEACEABLE,
    COHORT_ROLLED,
    COHORT_COUNT
};

constexpr std::array<char const*, COHORT_COUNT> COHORT_NAMES = {"a=0", "a=+1", "a=-0.75", "rolled"};

struct Rng
{
    uint64_t state;

    uint64_t Next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    double Uniform() { return static_cast<double>(Next() >> 11) / 9007199254740992.0; }
    float Roll() { return std::min(static_cast<float>(Uniform()), 0.99999994f); }
    double Between(double low, double high) { return low + (high - low) * Uniform(); }
    uint32_t Below(uint32_t count) { return std::min(count - 1, static_cast<uint32_t>(Uniform() * count)); }
    bool Chance(double p) { return Uniform() < p; }
    double Exponential(double mean) { return -std::log(1.0 - Uniform()) * mean; }

    uint32_t Poisson(double mean)
    {
        double const limit = std::exp(-mean);
        double product = Uniform();
        uint32_t count = 0;
        while (product > limit)
        {
            ++count;
            product *= Uniform();
        }
        return count;
    }
};

struct RolledFor
{
    int32_t target = NONE;
    uint32_t ms = 0;
};

struct Grudge
{
    int32_t target = NONE;
    float strength = 0.0f;
    uint32_t formedMs = 0;
};

struct Bot
{
    uint8_t faction = 0;
    uint8_t level = 1;
    Cohort cohort = COHORT_ROLLED;
    float aggression = 0.0f;
    float caution = 0.0f;
    float courtesy = 0.0f;
    float helpfulness = 0.0f;
    float patience = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    uint32_t movedMs = 0;
    float health = 1.0f;
    uint32_t healthMs = 0;
    int32_t fight = NONE;
    uint32_t closingUntilMs = 0;
    uint32_t deadUntilMs = 0;
    bool resurrected = false;
    uint32_t resurrectedMs = 0;
    uint32_t atGraveyardUntilMs = 0;
    bool fought = false;
    uint32_t fightEndMs = 0;
    uint32_t chanceMs = 0;
    std::array<RolledFor, 16> rolled{};
    uint32_t rolledNext = 0;
    std::array<Grudge, 8> grudges{};
    uint32_t grudgeCount = 0;
};

struct Fighter
{
    uint32_t bot = 0;
    uint8_t side = 0;
    JoinWhy why = JoinWhy::None;
    int32_t target = NONE;
    bool dead = false;
    bool retreated = false;
    bool escaped = false;
    bool gone = false;
    uint32_t deathMs = 0;
    uint32_t fleeMs = 0;
    int32_t pursuer = NONE;
    ChaseLimit chase;

    bool Active() const { return !dead && !retreated && !gone; }
};

struct Fight
{
    uint32_t startMs = 0;
    uint32_t lastHitMs = 0;
    uint32_t lullUntilMs = 0;
    float x = 0.0f;
    float y = 0.0f;
    uint8_t level = 1;
    bool objective = false;
    std::vector<Fighter> fighters;
    std::vector<uint32_t> decided;
    std::array<uint32_t, 2> deaths{};
};

struct RunSpec
{
    char const* name;
    double density;
    bool neutral;
    bool quiet;
};

struct Totals
{
    std::array<uint64_t, COHORT_COUNT> starts{};
    std::array<uint64_t, COHORT_COUNT> objectiveStarts{};
    std::array<uint64_t, COHORT_COUNT> chances{};
    std::array<uint64_t, COHORT_COUNT> meetings{};
    std::array<uint32_t, COHORT_COUNT> bots{};
    std::array<uint64_t, 5> meetingsByColour{};
    std::array<uint64_t, WorldPvp::REFUSAL_WORDS.size()> refusals{};
    std::array<uint64_t, WorldPvp::REFUSAL_WORDS.size()> joinRefusals{};
    uint64_t meetingsTotal = 0;
    uint64_t noTarget = 0;
    uint64_t grudgeStarts = 0;
    uint64_t grudgesFormed = 0;
    uint64_t revenges = 0;
    uint64_t fights = 0;
    uint64_t objectiveFights = 0;
    uint64_t participants = 0;
    uint64_t deaths = 0;
    uint64_t retreats = 0;
    uint64_t escapes = 0;
    std::array<uint64_t, 5> causes{};
    std::vector<uint32_t> lengths;
    std::array<uint64_t, 2> joinChances{};
    std::array<uint64_t, 2> joins{};
    std::array<uint64_t, 2> fightersByCaution{};
    std::array<uint64_t, 2> retreatsByCaution{};
    uint64_t bystanders = 0;
    uint64_t ghostWaits = 0;
    uint64_t spiritHealers = 0;
    uint64_t pauses = 0;
    double avoidMinutes = 0.0;
    uint64_t avoids = 0;
    // Hard rules broken by a start or a join: must stay 0
    uint64_t colourStarts = 0;
    uint64_t campingStarts = 0;
    uint64_t graveyardStarts = 0;
    uint64_t calmStarts = 0;
    uint64_t gapStarts = 0;
    uint64_t peaceableStarts = 0;
    uint64_t redJoins = 0;
    uint64_t unended = 0;
    uint32_t maxLengthMs = 0;
    // The fight's age when the rules ended it (the length above runs only to the last hit)
    uint32_t maxEndAgeMs = 0;
    // The cap: the highest p of a chance without a grudge, and of a first chance after QUIET_MS
    float maxP = 0.0f;
    float maxQuietP = 0.0f;
    uint64_t quietChances = 0;
    // The rolled population's rate (the analytic mean) and its share that never starts
    double rolledRate = 0.0;
    uint32_t rolledOff = 0;
};

constexpr std::array<std::array<uint8_t, 5>, 2> RACES = {{{1, 3, 4, 7, 11}, {2, 5, 6, 8, 10}}};

// The classes each race may have (bit per class id), WotLK
constexpr uint32_t Classes(uint8_t race)
{
    auto bits = [](std::initializer_list<int> ids)
    {
        uint32_t mask = 0;
        for (int const id : ids)
            mask |= 1u << id;
        return mask;
    };
    switch (race)
    {
        case 1:
            return bits({1, 2, 4, 5, 6, 8, 9});
        case 2:
            return bits({1, 3, 4, 6, 7, 9});
        case 3:
            return bits({1, 2, 3, 4, 5, 6});
        case 4:
            return bits({1, 3, 4, 5, 6, 11});
        case 5:
            return bits({1, 4, 5, 6, 8, 9});
        case 6:
            return bits({1, 3, 6, 7, 11});
        case 7:
            return bits({1, 4, 6, 8, 9});
        case 8:
            return bits({1, 3, 4, 5, 6, 7, 8});
        case 10:
            return bits({2, 3, 4, 5, 6, 8, 9});
        default:
            return bits({1, 2, 3, 5, 6, 7, 8});
    }
}

float Wrap(float value, float side)
{
    float const wrapped = std::fmod(value, side);
    return wrapped < 0.0f ? wrapped + side : wrapped;
}

uint32_t Since(uint32_t nowMs, uint32_t thenMs) { return nowMs >= thenMs ? nowMs - thenMs : 0; }

class World
{
public:
    World(RunSpec const& spec, uint64_t seed)
        : _spec(spec), _rng{seed}, _side(WORLD_YARDS / static_cast<float>(std::sqrt(spec.density)))
    {
        WorldPvpConfig config;
        if (spec.neutral)
        {
            config.GreenFactor = 1.0f;
            config.OrangeDaring = 1.0f;
            config.OrangeCareful = 1.0f;
            config.Courtesy = false;
            config.GrudgeFactor = 1.0f;
        }
        _values = FightRuleValues(config);
        _endGraceMs = config.EndGraceSeconds * SECOND;

        for (uint32_t level = 1; level <= MAX_LEVEL; ++level)
            for (uint32_t other = 1; other <= MAX_LEVEL; ++other)
                _colourLevels[level][static_cast<size_t>(WorldPvp::ColourOf(level, other))].push_back(
                    static_cast<uint8_t>(other));

        // The same population in every run (seeded by index), the run's own events
        Rng population{0x5EEDF00DULL};
        _bots.resize(BOTS);
        for (uint32_t index = 0; index < BOTS; ++index)
        {
            Bot& bot = _bots[index];
            bot.faction = static_cast<uint8_t>((index / 10) % 2);
            bot.cohort = static_cast<Cohort>(std::min<uint32_t>(index % 10, COHORT_ROLLED));
            uint8_t const race = RACES[bot.faction][population.Below(5)];
            uint8_t cls = 1;
            do
                cls = static_cast<uint8_t>(1 + population.Below(11));
            while (!(Classes(race) & (1u << cls)));
            bot.level = static_cast<uint8_t>(1 + population.Below(MAX_LEVEL));
            TraitRules::BotTraits const traits = TraitRules::Roll(1000 + index, 1, race, cls);
            bot.aggression = TraitRules::Unit(traits, TraitRules::Trait::Aggression);
            bot.caution = TraitRules::Unit(traits, TraitRules::Trait::Caution);
            bot.courtesy = TraitRules::Unit(traits, TraitRules::Trait::Courtesy);
            bot.helpfulness = TraitRules::Unit(traits, TraitRules::Trait::Helpfulness);
            bot.patience = TraitRules::Unit(traits, TraitRules::Trait::Patience);
            if (bot.cohort == COHORT_ZERO)
                bot.aggression = 0.0f;
            else if (bot.cohort == COHORT_ONE)
                bot.aggression = 1.0f;
            else if (bot.cohort == COHORT_PEACEABLE)
                bot.aggression = -0.75f;
            else
            {
                float const rate = WorldPvp::StartRate(bot.aggression, _values);
                _totals.rolledRate += rate;
                if (rate <= 0.0f)
                    ++_totals.rolledOff;
            }
            ++_totals.bots[bot.cohort];
            bot.x = static_cast<float>(_rng.Uniform()) * _side;
            bot.y = static_cast<float>(_rng.Uniform()) * _side;
            _byLevel[bot.faction][bot.level].push_back(index);
        }

        _meetingRate = spec.quiet ? QUIET_MEETINGS : 0.0;
        for (double const share : MEETINGS_1X)
            _colourTotal += share;
        if (!spec.quiet)
            _meetingRate = _colourTotal * spec.density;
    }

    Totals const& Run()
    {
        using Event = std::pair<uint32_t, uint32_t>;
        std::priority_queue<Event, std::vector<Event>, std::greater<Event>> meetings;
        uint32_t const endMs = HOURS * HOUR;
        for (uint32_t index = 0; index < BOTS; ++index)
            meetings.push({NextMeeting(0), index});

        uint32_t tickMs = 0;
        // Meetings until the end, then the fights still running finish (with an hour's safety deadline)
        while (true)
        {
            uint32_t const meetingMs = meetings.empty() ? UINT32_MAX : meetings.top().first;
            bool const fightsLive = !_live.empty();
            if (meetingMs >= endMs && !fightsLive)
                break;
            if (fightsLive && (tickMs <= meetingMs || meetingMs >= endMs))
            {
                if (tickMs >= endMs + HOUR)
                    break;
                Tick(tickMs);
                tickMs += SECOND;
                continue;
            }

            Event const event = meetings.top();
            meetings.pop();
            if (!fightsLive)
                tickMs = event.first + SECOND;
            Meet(event.second, event.first);
            uint32_t const next = NextMeeting(event.first);
            if (next < endMs)
                meetings.push({next, event.second});
        }

        _totals.unended = _live.size();
        return _totals;
    }

private:
    uint32_t NextMeeting(uint32_t nowMs)
    {
        double const ms = _rng.Exponential(1.0 / _meetingRate) * HOUR;
        return ms >= static_cast<double>(UINT32_MAX - nowMs) ? UINT32_MAX : nowMs + static_cast<uint32_t>(ms);
    }

    bool Dead(Bot const& bot, uint32_t nowMs) const { return nowMs < bot.deadUntilMs; }
    bool Busy(Bot const& bot, uint32_t nowMs) const { return bot.fight != NONE || nowMs < bot.closingUntilMs; }

    void Move(Bot& bot, uint32_t nowMs)
    {
        float const yards = DRIFT_YARDS_PER_SECOND * static_cast<float>(Since(nowMs, bot.movedMs)) / SECOND;
        float const heading = static_cast<float>(_rng.Uniform() * 2.0 * PI);
        bot.x = Wrap(bot.x + yards * std::cos(heading), _side);
        bot.y = Wrap(bot.y + yards * std::sin(heading), _side);
        bot.movedMs = nowMs;
    }

    void Place(Bot& bot, float x, float y, float yards, uint32_t nowMs)
    {
        float const distance = yards * static_cast<float>(std::sqrt(_rng.Uniform()));
        float const heading = static_cast<float>(_rng.Uniform() * 2.0 * PI);
        bot.x = Wrap(x + distance * std::cos(heading), _side);
        bot.y = Wrap(y + distance * std::sin(heading), _side);
        bot.movedMs = nowMs;
    }

    float Health(Bot const& bot, uint32_t nowMs) const
    {
        if (bot.fight != NONE)
            return bot.health;
        return std::min(1.0f, bot.health + static_cast<float>(Since(nowMs, bot.healthMs)) / REGEN_MS);
    }

    float GrudgeAgainst(Bot& bot, uint32_t target, uint32_t nowMs)
    {
        for (uint32_t slot = 0; slot < bot.grudgeCount; ++slot)
        {
            if (bot.grudges[slot].target != static_cast<int32_t>(target))
                continue;
            float const faded = WorldPvp::GrudgeFaded(bot.grudges[slot].strength,
                                                      Since(nowMs, bot.grudges[slot].formedMs) / SECOND, _values);
            if (!WorldPvp::GrudgeForgotten(faded, _values))
                return faded;
            bot.grudges[slot] = bot.grudges[--bot.grudgeCount];
            return 0.0f;
        }
        return 0.0f;
    }

    void FormGrudge(Bot& victim, uint32_t killer, bool killerStarted, uint32_t nowMs)
    {
        float const strength = WorldPvp::GrudgeStrength(killerStarted, victim.patience, _values);
        std::array<float, 8> strengths{};
        for (uint32_t slot = 0; slot < victim.grudgeCount; ++slot)
        {
            if (victim.grudges[slot].target == static_cast<int32_t>(killer))
            {
                victim.grudges[slot] = {static_cast<int32_t>(killer), strength, nowMs};
                return;
            }
            strengths[slot] = WorldPvp::GrudgeFaded(victim.grudges[slot].strength,
                                                    Since(nowMs, victim.grudges[slot].formedMs) / SECOND, _values);
        }
        int32_t const slot = WorldPvp::GrudgeSlot(strengths, victim.grudgeCount, strength, _values);
        if (slot < 0)
            return;
        if (static_cast<uint32_t>(slot) == victim.grudgeCount)
            ++victim.grudgeCount;
        victim.grudges[slot] = {static_cast<int32_t>(killer), strength, nowMs};
        ++_totals.grudgesFormed;
    }

    void Revenge(Bot& killer, uint32_t victim)
    {
        for (uint32_t slot = 0; slot < killer.grudgeCount; ++slot)
            if (killer.grudges[slot].target == static_cast<int32_t>(victim))
            {
                killer.grudges[slot] = killer.grudges[--killer.grudgeCount];
                ++_totals.revenges;
                return;
            }
    }

    uint32_t SinceRolled(Bot const& bot, uint32_t target, uint32_t nowMs) const
    {
        for (RolledFor const& entry : bot.rolled)
            if (entry.target == static_cast<int32_t>(target))
                return Since(nowMs, entry.ms);
        return WorldPvp::NEVER_MS;
    }

    // An enemy of the colour drawn for this meeting: alive and not in a fight
    int32_t PickTarget(Bot const& bot, uint32_t nowMs, Colour& colour)
    {
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            if (attempt % 3 == 0)
            {
                double draw = _rng.Uniform() * _colourTotal;
                size_t pick = 0;
                while (pick + 1 < MEETINGS_1X.size() && draw >= MEETINGS_1X[pick])
                    draw -= MEETINGS_1X[pick++];
                colour = static_cast<Colour>(pick);
            }
            std::vector<uint8_t> const& levels = _colourLevels[bot.level][static_cast<size_t>(colour)];
            if (levels.empty())
                continue;
            std::vector<uint32_t> const& enemies =
                _byLevel[1 - bot.faction][levels[_rng.Below(static_cast<uint32_t>(levels.size()))]];
            if (enemies.empty())
                continue;
            uint32_t const target = enemies[_rng.Below(static_cast<uint32_t>(enemies.size()))];
            if (!Dead(_bots[target], nowMs) && !Busy(_bots[target], nowMs))
                return static_cast<int32_t>(target);
        }
        return NONE;
    }

    float Nearby(uint32_t viewerLevel, uint32_t nearLevel)
    {
        float strength = 0.0f;
        uint32_t const count = _rng.Poisson(NEARBY_PER_SIDE * _spec.density);
        for (uint32_t index = 0; index < count; ++index)
        {
            int const level = std::clamp(static_cast<int>(nearLevel) + static_cast<int>(_rng.Below(7)) - 3, 1,
                                         static_cast<int>(MAX_LEVEL));
            strength += WorldPvp::Strength(viewerLevel, static_cast<uint32_t>(level), 1.0f, _values);
        }
        return strength;
    }

    void Meet(uint32_t index, uint32_t nowMs)
    {
        Bot& bot = _bots[index];
        if (Dead(bot, nowMs))
            return;
        Colour colour = Colour::Yellow;
        int32_t const picked = PickTarget(bot, nowMs, colour);
        if (picked == NONE)
        {
            ++_totals.noTarget;
            return;
        }
        uint32_t const targetIndex = static_cast<uint32_t>(picked);
        Bot& target = _bots[targetIndex];
        Move(bot, nowMs);
        Place(target, bot.x, bot.y, _values.scanYards, nowMs);
        ++_totals.meetingsTotal;
        ++_totals.meetings[bot.cohort];
        ++_totals.meetingsByColour[static_cast<size_t>(colour)];

        StartBot start;
        start.inFight = Busy(bot, nowMs);
        start.atObjective = _rng.Chance(OBJECTIVE);
        start.inCalmArea = WorldPvp::InAnyCalmArea(_ended, bot.x, bot.y, nowMs, _values);
        start.sinceFightEndMs = bot.fought ? Since(nowMs, bot.fightEndMs) : WorldPvp::NEVER_MS;
        start.sinceChanceMs = Since(nowMs, bot.chanceMs);
        bool const botAtGraveyard = nowMs < bot.atGraveyardUntilMs || _rng.Chance(BOT_NEAR_GRAVEYARD);
        start.graveyardYards =
            botAtGraveyard ? static_cast<float>(_rng.Uniform()) * _values.graveyardYards : WorldPvp::FAR_YARDS;
        start.aggression = bot.aggression;
        start.caution = bot.caution;
        start.courtesy = bot.courtesy;

        StartTarget enemy;
        enemy.enemy = !_rng.Chance(UNFLAGGED);
        enemy.colour = WorldPvp::ColourOf(bot.level, target.level);
        enemy.sinceResurrectionMs = target.resurrected ? Since(nowMs, target.resurrectedMs) : WorldPvp::NEVER_MS;
        bool const targetAtGraveyard = nowMs < target.atGraveyardUntilMs || _rng.Chance(TARGET_NEAR_GRAVEYARD);
        enemy.graveyardYards =
            targetAtGraveyard ? static_cast<float>(_rng.Uniform()) * _values.graveyardYards : WorldPvp::FAR_YARDS;
        enemy.contestsObjective = start.atObjective;
        enemy.sinceRolledMs = SinceRolled(bot, targetIndex, nowMs);
        enemy.busy = _rng.Chance(TARGET_BUSY);
        enemy.health = Health(target, nowMs);
        float const own =
            WorldPvp::Strength(bot.level, bot.level, Health(bot, nowMs), _values) + Nearby(bot.level, bot.level);
        float const foes =
            WorldPvp::Strength(bot.level, target.level, enemy.health, _values) + Nearby(bot.level, target.level);
        enemy.odds = _spec.neutral ? 1.0f : WorldPvp::OddsShare(own, foes);
        enemy.grudge = GrudgeAgainst(bot, targetIndex, nowMs);

        StartDecision const decision = WorldPvp::DecideStart(start, enemy, _rng.Roll(), _values);
        ++_totals.refusals[static_cast<size_t>(decision.refusal)];
        if (decision.chance)
        {
            ++_totals.chances[bot.cohort];
            bot.rolled[bot.rolledNext] = {static_cast<int32_t>(targetIndex), nowMs};
            bot.rolledNext = (bot.rolledNext + 1) % bot.rolled.size();
            if (enemy.grudge <= 0.0f)
            {
                _totals.maxP = std::max(_totals.maxP, decision.p);
                if (start.sinceChanceMs >= QUIET_MS)
                {
                    ++_totals.quietChances;
                    _totals.maxQuietP = std::max(_totals.maxQuietP, decision.p);
                }
            }
            bot.chanceMs = nowMs;
        }
        if (!decision.start)
            return;

        // The hard rules, checked against the world's own state
        bool const objective = decision.reason == FightReason::Objective;
        if (!WorldPvp::MayPick(enemy.colour, Pick::Start))
            ++_totals.colourStarts;
        if (enemy.sinceResurrectionMs < _values.resurrectedMs)
            ++_totals.campingStarts;
        if (botAtGraveyard || targetAtGraveyard)
            ++_totals.graveyardStarts;
        if (!objective && start.inCalmArea)
            ++_totals.calmStarts;
        if (!objective && start.sinceFightEndMs < _values.minGapMs)
            ++_totals.gapStarts;
        if (!objective && WorldPvp::StartRate(bot.aggression, _values) <= 0.0f)
            ++_totals.peaceableStarts;
        if (objective)
            ++_totals.objectiveStarts[bot.cohort];
        else
            ++_totals.starts[bot.cohort];
        if (decision.reason == FightReason::Grudge)
            ++_totals.grudgeStarts;
        StartFight(index, targetIndex, objective, nowMs);
    }

    void StartFight(uint32_t starter, uint32_t target, bool objective, uint32_t nowMs)
    {
        int32_t slot = NONE;
        if (!_free.empty())
        {
            slot = _free.back();
            _free.pop_back();
        }
        else
        {
            slot = static_cast<int32_t>(_fights.size());
            _fights.emplace_back();
        }
        Fight& fight = _fights[slot];
        fight = Fight{};
        fight.startMs = nowMs;
        fight.lastHitMs = nowMs;
        fight.x = _bots[starter].x;
        fight.y = _bots[starter].y;
        fight.level = _bots[starter].level;
        fight.objective = objective;
        AddFighter(fight, slot, starter, 0, JoinWhy::None, nowMs);
        AddFighter(fight, slot, target, 1, JoinWhy::Defence, nowMs);
        fight.fighters[0].target = 1;
        fight.fighters[1].target = 0;
        _live.push_back(slot);
        ++_totals.fights;
        if (objective)
            ++_totals.objectiveFights;

        uint32_t const bystanders = _rng.Poisson(BYSTANDERS_AT_START * _spec.density);
        for (uint32_t index = 0; index < bystanders; ++index)
            Bystander(slot, nowMs);
    }

    void AddFighter(Fight& fight, int32_t slot, uint32_t index, uint8_t side, JoinWhy why, uint32_t nowMs)
    {
        Bot& bot = _bots[index];
        bot.health = Health(bot, nowMs);
        bot.healthMs = nowMs;
        bot.fight = slot;
        Fighter fighter;
        fighter.bot = index;
        fighter.side = side;
        fighter.why = why;
        fight.fighters.push_back(fighter);
        fight.decided.push_back(index);
        ++_totals.fightersByCaution[bot.caution >= 0.0f ? 1 : 0];
    }

    // A bot passing by decides once whether to join (WP.7)
    void Bystander(int32_t slot, uint32_t nowMs)
    {
        Fight& fight = _fights[slot];
        int const level = std::clamp(static_cast<int>(fight.level) + static_cast<int>(_rng.Below(9)) - 4, 1,
                                     static_cast<int>(MAX_LEVEL));
        uint8_t const faction = static_cast<uint8_t>(_rng.Below(2));
        std::vector<uint32_t> const& candidates = _byLevel[faction][level];
        if (candidates.empty())
            return;
        uint32_t const index = candidates[_rng.Below(static_cast<uint32_t>(candidates.size()))];
        Bot& bot = _bots[index];
        if (Dead(bot, nowMs) || Busy(bot, nowMs) ||
            std::find(fight.decided.begin(), fight.decided.end(), index) != fight.decided.end())
            return;
        fight.decided.push_back(index);
        ++_totals.bystanders;
        float const yards = BYSTANDER_YARDS * static_cast<float>(std::sqrt(_rng.Uniform()));
        Place(bot, fight.x, fight.y, yards, nowMs);

        uint8_t const side = faction == _bots[fight.fighters[0].bot].faction ? 0 : 1;
        JoinInput input;
        input.yards = yards;
        input.pickable = 0;
        float own = 0.0f;
        float foes = 0.0f;
        for (Fighter const& other : fight.fighters)
        {
            if (!other.Active())
                continue;
            Bot const& body = _bots[other.bot];
            float const strength = WorldPvp::Strength(bot.level, body.level, body.health, _values);
            if (other.side == side)
            {
                own += strength;
                input.factionTargeted = true;
            }
            else
            {
                foes += strength;
                if (WorldPvp::MayPick(WorldPvp::ColourOf(bot.level, body.level), Pick::Join))
                    ++input.pickable;
            }
        }
        input.odds = WorldPvp::OddsShare(own, foes);
        input.goalValue = _rng.Chance(IDLE_SHARE) ? 0.0f : static_cast<float>(_rng.Uniform());
        input.aggression = bot.aggression;
        input.helpfulness = bot.helpfulness;
        input.caution = bot.caution;
        JoinDecision const decision = WorldPvp::DecideJoin(input, _rng.Roll(), _values);
        ++_totals.joinRefusals[static_cast<size_t>(decision.refusal)];
        size_t const band = bot.aggression >= 0.0f ? 1 : 0;
        if (decision.refusal == Refusal::None || decision.refusal == Refusal::Roll)
            ++_totals.joinChances[band];
        if (!decision.join)
            return;
        ++_totals.joins[band];
        AddFighter(fight, slot, index, side, decision.why, nowMs);
        Fighter& joined = fight.fighters.back();
        joined.target = PickEnemy(fight, joined);
        if (joined.target != NONE &&
            !WorldPvp::MayPick(WorldPvp::ColourOf(bot.level, _bots[fight.fighters[joined.target].bot].level),
                               Pick::Join))
            ++_totals.redJoins;
    }

    // A fighter's next enemy: a chance joiner never picks red (WP.7), the others anything
    int32_t PickEnemy(Fight const& fight, Fighter const& fighter)
    {
        Pick const pick = fighter.why == JoinWhy::Chance ? Pick::Join : Pick::Defence;
        uint8_t const level = _bots[fighter.bot].level;
        std::array<int32_t, 64> options{};
        uint32_t count = 0;
        for (size_t index = 0; index < fight.fighters.size() && count < options.size(); ++index)
        {
            Fighter const& other = fight.fighters[index];
            if (other.side != fighter.side && other.Active() &&
                WorldPvp::MayPick(WorldPvp::ColourOf(level, _bots[other.bot].level), pick))
                options[count++] = static_cast<int32_t>(index);
        }
        return count == 0 ? NONE : options[_rng.Below(count)];
    }

    void Hit(Fight& fight, Fighter const& attacker, int32_t victimIndex, uint32_t nowMs)
    {
        Fighter& victim = fight.fighters[victimIndex];
        Bot& from = _bots[attacker.bot];
        Bot& to = _bots[victim.bot];
        int const gap = std::clamp(static_cast<int>(from.level) - static_cast<int>(to.level),
                                   -WorldPvp::MAX_STRENGTH_LEVEL_GAP, WorldPvp::MAX_STRENGTH_LEVEL_GAP);
        float const damage =
            DAMAGE * static_cast<float>(_rng.Between(0.5, 1.5)) * std::pow(_values.levelStep, static_cast<float>(gap));
        if (Since(nowMs, fight.lastHitMs) > COMBAT_FLAG_MS + _endGraceMs)
            ++_totals.pauses;
        fight.lastHitMs = nowMs;
        to.health -= damage;
        to.healthMs = nowMs;
        from.healthMs = nowMs;
        if (to.health > 0.0f)
            return;

        to.health = 0.0f;
        victim.dead = true;
        victim.deathMs = nowMs;
        to.deadUntilMs = UINT32_MAX;
        ++fight.deaths[victim.side];
        bool const killerStarted = &attacker == &fight.fighters[0] && !fight.objective;
        FormGrudge(to, attacker.bot, killerStarted, nowMs);
        Revenge(from, victim.bot);
    }

    float Odds(Fight const& fight, Fighter const& viewer) const
    {
        uint8_t const level = _bots[viewer.bot].level;
        float own = 0.0f;
        float foes = 0.0f;
        for (Fighter const& other : fight.fighters)
        {
            if (!other.Active())
                continue;
            Bot const& body = _bots[other.bot];
            float const strength = WorldPvp::Strength(level, body.level, body.health, _values);
            (other.side == viewer.side ? own : foes) += strength;
        }
        return WorldPvp::OddsShare(own, foes);
    }

    void Tick(uint32_t nowMs)
    {
        for (size_t index = 0; index < _live.size();)
        {
            if (TickFight(_live[index], nowMs))
            {
                _live[index] = _live.back();
                _live.pop_back();
            }
            else
                ++index;
        }
    }

    // One second of a fight; true when it ended
    bool TickFight(int32_t slot, uint32_t nowMs)
    {
        if (_rng.Chance(ARRIVALS_PER_SECOND * _spec.density))
            Bystander(slot, nowMs);
        Fight& fight = _fights[slot];
        if (nowMs >= fight.lullUntilMs && _rng.Chance(LULL_PER_SECOND))
            fight.lullUntilMs = nowMs + static_cast<uint32_t>(_rng.Exponential(LULL_MEAN_SECONDS) * SECOND);
        bool const lull = nowMs < fight.lullUntilMs;

        for (Fighter& fighter : fight.fighters)
            if (fighter.Active() && _rng.Chance(LEAVE_PER_SECOND))
                fighter.gone = true;

        for (size_t index = 0; index < fight.fighters.size(); ++index)
        {
            Fighter& fighter = fight.fighters[index];
            if (!fighter.Active())
                continue;
            if (fighter.target == NONE || !fight.fighters[fighter.target].Active())
                fighter.target = PickEnemy(fight, fighter);
            if (fighter.target == NONE)
            {
                // Nobody left to fight: chase one who retreats
                for (size_t other = 0; other < fight.fighters.size(); ++other)
                {
                    Fighter& fleeing = fight.fighters[other];
                    if (fleeing.side != fighter.side && fleeing.retreated && !fleeing.escaped && !fleeing.dead &&
                        fleeing.pursuer == NONE)
                    {
                        fleeing.pursuer = static_cast<int32_t>(index);
                        Bot const& body = _bots[fighter.bot];
                        fleeing.chase = WorldPvp::ChaseLimitFor(body.aggression, body.courtesy, _values);
                        break;
                    }
                }
                continue;
            }
            if (!lull && _rng.Chance(HIT_CHANCE))
                Hit(fight, fighter, fighter.target, nowMs);
        }

        // The chase: the pursuer hits the one who retreats until ChaseOver, then it has escaped
        bool chasing = false;
        for (size_t index = 0; index < fight.fighters.size(); ++index)
        {
            Fighter& fleeing = fight.fighters[index];
            if (!fleeing.retreated || fleeing.escaped || fleeing.dead)
                continue;
            uint32_t const chaseMs = Since(nowMs, fleeing.fleeMs);
            float const beyond =
                std::max(0.0f, FLEE_YARDS_PER_SECOND * static_cast<float>(chaseMs) / SECOND - FLEE_START_INSIDE);
            if (fleeing.pursuer == NONE || !fight.fighters[fleeing.pursuer].Active() ||
                WorldPvp::ChaseOver(chaseMs, beyond, fleeing.chase))
            {
                fleeing.escaped = true;
                ++_totals.escapes;
                continue;
            }
            chasing = true;
            if (!lull && _rng.Chance(CHASE_HIT_CHANCE))
                Hit(fight, fight.fighters[fleeing.pursuer], static_cast<int32_t>(index), nowMs);
        }

        for (size_t index = 0; index < fight.fighters.size(); ++index)
        {
            Fighter& fighter = fight.fighters[index];
            if (!fighter.Active())
                continue;
            Bot const& body = _bots[fighter.bot];
            if (!WorldPvp::ShouldRetreat(body.health, Odds(fight, fighter), body.caution, _values))
                continue;
            fighter.retreated = true;
            fighter.fleeMs = nowMs;
            ++_totals.retreats;
            ++_totals.retreatsByCaution[body.caution >= 0.0f ? 1 : 0];
            for (size_t other = 0; other < fight.fighters.size(); ++other)
            {
                Fighter const& enemy = fight.fighters[other];
                if (enemy.Active() && enemy.target == static_cast<int32_t>(index))
                {
                    fighter.pursuer = static_cast<int32_t>(other);
                    Bot const& hunter = _bots[enemy.bot];
                    fighter.chase = WorldPvp::ChaseLimitFor(hunter.aggression, hunter.courtesy, _values);
                    chasing = true;
                    break;
                }
            }
        }

        FightClock clock;
        clock.ageMs = Since(nowMs, fight.startMs);
        clock.sinceHitMs = Since(nowMs, fight.lastHitMs);
        for (Fighter const& fighter : fight.fighters)
            if (fighter.Active())
                ++clock.active[fighter.side];
        WorldPvp::FightEndCheck const check = WorldPvp::CheckFightEnd(clock, _values);
        if (!check.ends || (check.cause == EndCause::Wiped && chasing && clock.ageMs < _values.stalemateMs))
            return false;
        EndFight(slot, check.cause, nowMs);
        return true;
    }

    void EndFight(int32_t slot, EndCause cause, uint32_t nowMs)
    {
        Fight& fight = _fights[slot];
        // The book closes the fight after its grace; the length runs to the last hit (WP.15)
        uint32_t const closedMs = nowMs + _endGraceMs;
        uint32_t const lengthMs = Since(fight.lastHitMs, fight.startMs);
        _totals.lengths.push_back(lengthMs);
        _totals.maxLengthMs = std::max(_totals.maxLengthMs, lengthMs);
        _totals.maxEndAgeMs = std::max(_totals.maxEndAgeMs, Since(nowMs, fight.startMs));
        ++_totals.causes[static_cast<size_t>(cause)];
        _totals.participants += fight.fighters.size();
        _totals.deaths += fight.deaths[0] + fight.deaths[1];

        for (Fighter const& fighter : fight.fighters)
        {
            Bot& bot = _bots[fighter.bot];
            bot.fight = NONE;
            bot.closingUntilMs = closedMs;
            bot.fought = true;
            bot.fightEndMs = closedMs;
            bot.healthMs = nowMs;
            Place(bot, fight.x, fight.y, fight.fighters.size() > 2 ? 30.0f : 15.0f, nowMs);
            if (fighter.dead || fighter.retreated)
            {
                _totals.avoidMinutes += static_cast<double>(WorldPvp::AvoidMs(bot.caution, _values)) / MINUTE;
                ++_totals.avoids;
            }
            if (fighter.dead)
                Ghost(bot, fighter, nowMs);
        }

        EndedFight ended;
        ended.endMs = closedMs;
        ended.x = fight.x;
        ended.y = fight.y;
        ended.cause = cause;
        _ended.push_back(ended);
        if (_ended.size() > WorldPvp::MAX_ENDED_FIGHTS)
            _ended.pop_front();
        fight.fighters.clear();
        fight.decided.clear();
        _free.push_back(slot);
    }

    // The ghost waits while an enemy is at its corpse (until the fight ends, longer when one lingers), then runs to
    // it or, after the wait's limit, takes the spirit healer
    void Ghost(Bot& bot, Fighter const& fighter, uint32_t nowMs)
    {
        uint32_t const releasedMs = fighter.deathMs + RELEASE_MS;
        uint32_t enemyUntilMs = nowMs;
        if (_rng.Chance(LINGER_SHARE))
            enemyUntilMs += static_cast<uint32_t>(_rng.Between(LINGER_MIN_SECONDS, LINGER_MAX_SECONDS) * SECOND);
        uint32_t atMs = releasedMs;
        uint32_t backMs = 0;
        bool graveyard = false;
        while (true)
        {
            float const enemyYards = atMs < enemyUntilMs ? 0.0f : WorldPvp::FAR_YARDS;
            WorldPvp::GhostPlan const plan = WorldPvp::PlanGhost(enemyYards, Since(atMs, releasedMs), _values);
            if (plan.step == GhostStep::Wait)
            {
                ++_totals.ghostWaits;
                atMs += std::max<uint32_t>(plan.recheckMs, SECOND);
                continue;
            }
            if (plan.step == GhostStep::SpiritHealer)
            {
                ++_totals.spiritHealers;
                backMs = atMs;
                graveyard = true;
            }
            else
                backMs =
                    atMs + static_cast<uint32_t>(_rng.Between(CORPSE_RUN_MIN_SECONDS, CORPSE_RUN_MAX_SECONDS) * SECOND);
            break;
        }
        bot.deadUntilMs = backMs;
        bot.resurrected = true;
        bot.resurrectedMs = backMs;
        bot.health = REZ_HEALTH;
        bot.healthMs = backMs;
        bot.atGraveyardUntilMs = graveyard ? backMs + AT_GRAVEYARD_MS : 0;
    }

    RunSpec _spec;
    Rng _rng;
    float _side;
    FightRuleValues _values;
    uint32_t _endGraceMs = 0;
    double _meetingRate = 0.0;
    double _colourTotal = 0.0;
    std::vector<Bot> _bots;
    std::array<std::array<std::vector<uint32_t>, MAX_LEVEL + 1>, 2> _byLevel;
    std::array<std::array<std::vector<uint8_t>, 5>, MAX_LEVEL + 1> _colourLevels;
    std::vector<Fight> _fights;
    std::vector<int32_t> _free;
    std::vector<int32_t> _live;
    std::deque<EndedFight> _ended;
    Totals _totals;
};

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
    std::printf("FAIL: %s\n", name.c_str());
}

double PerBotHour(uint64_t count, uint32_t bots) { return static_cast<double>(count) / (bots * double(HOURS)); }

uint32_t Percentile(std::vector<uint32_t> lengths, double share)
{
    if (lengths.empty())
        return 0;
    std::sort(lengths.begin(), lengths.end());
    return lengths[std::min(lengths.size() - 1, static_cast<size_t>(share * static_cast<double>(lengths.size())))];
}

void Report(RunSpec const& spec, Totals const& totals)
{
    uint32_t const all = BOTS;
    std::printf("[%s] meetings/bot h %.2f (grey %.2f green %.2f yellow %.2f orange %.2f red %.2f), no target %.3f\n",
                spec.name, PerBotHour(totals.meetingsTotal, all), PerBotHour(totals.meetingsByColour[0], all),
                PerBotHour(totals.meetingsByColour[1], all), PerBotHour(totals.meetingsByColour[2], all),
                PerBotHour(totals.meetingsByColour[3], all), PerBotHour(totals.meetingsByColour[4], all),
                PerBotHour(totals.noTarget, all));
    std::printf("  starts/bot h:");
    for (size_t cohort = 0; cohort < COHORT_COUNT; ++cohort)
        std::printf(" %s %.3f (chances %.2f, objective %.3f)", COHORT_NAMES[cohort],
                    PerBotHour(totals.starts[cohort], totals.bots[cohort]),
                    PerBotHour(totals.chances[cohort], totals.bots[cohort]),
                    PerBotHour(totals.objectiveStarts[cohort], totals.bots[cohort]));
    std::printf("\n  a=0 at %.1f h per start, a=+1 %.2f times a=0 (its ceiling, cap x chances: %.3f/h)\n",
                1.0 / std::max(1.0e-9, PerBotHour(totals.starts[COHORT_ZERO], totals.bots[COHORT_ZERO])),
                PerBotHour(totals.starts[COHORT_ONE], totals.bots[COHORT_ONE]) /
                    std::max(1.0e-9, PerBotHour(totals.starts[COHORT_ZERO], totals.bots[COHORT_ZERO])),
                PerBotHour(totals.chances[COHORT_ONE], totals.bots[COHORT_ONE]) / 3.0);
    std::printf(
        "  rolled rate mean %.3f/h, %.1f %% never start; max p without grudge %.3f; quiet first chances %llu "
        "max p %.3f\n",
        totals.rolledRate / totals.bots[COHORT_ROLLED], 100.0 * totals.rolledOff / totals.bots[COHORT_ROLLED],
        totals.maxP, static_cast<unsigned long long>(totals.quietChances), totals.maxQuietP);
    double const fights = std::max<double>(1.0, static_cast<double>(totals.fights));
    double lengthSum = 0.0;
    for (uint32_t const length : totals.lengths)
        lengthSum += length;
    std::printf(
        "  fights %llu (objective %llu): participants %.2f, deaths %.2f, retreats %.2f per fight; length s "
        "avg %.1f p50 %.1f p90 %.1f max %.1f; age at the end max %.1f\n",
        static_cast<unsigned long long>(totals.fights), static_cast<unsigned long long>(totals.objectiveFights),
        totals.participants / fights, totals.deaths / fights, totals.retreats / fights, lengthSum / fights / SECOND,
        Percentile(totals.lengths, 0.5) / double(SECOND), Percentile(totals.lengths, 0.9) / double(SECOND),
        totals.maxLengthMs / double(SECOND), totals.maxEndAgeMs / double(SECOND));
    std::printf(
        "  ends: wiped %llu, timeout %llu, stalemate %llu, unended %llu; pauses over the grace %llu; ghost "
        "waits %llu, spirit healers %llu; avoid %.1f min\n",
        static_cast<unsigned long long>(totals.causes[static_cast<size_t>(EndCause::Wiped)]),
        static_cast<unsigned long long>(totals.causes[static_cast<size_t>(EndCause::Timeout)]),
        static_cast<unsigned long long>(totals.causes[static_cast<size_t>(EndCause::Stalemate)]),
        static_cast<unsigned long long>(totals.unended), static_cast<unsigned long long>(totals.pauses),
        static_cast<unsigned long long>(totals.ghostWaits), static_cast<unsigned long long>(totals.spiritHealers),
        totals.avoids ? totals.avoidMinutes / totals.avoids : 0.0);
    auto share = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * part / whole : 0.0; };
    std::printf(
        "  bystanders %llu: join %% by aggression <0 %.1f, >=0 %.1f; retreat %% of fighters by caution <0 "
        "%.1f, >=0 %.1f; grudges formed %llu, grudge starts %llu, revenges %llu\n",
        static_cast<unsigned long long>(totals.bystanders), share(totals.joins[0], totals.joinChances[0]),
        share(totals.joins[1], totals.joinChances[1]), share(totals.retreatsByCaution[0], totals.fightersByCaution[0]),
        share(totals.retreatsByCaution[1], totals.fightersByCaution[1]),
        static_cast<unsigned long long>(totals.grudgesFormed), static_cast<unsigned long long>(totals.grudgeStarts),
        static_cast<unsigned long long>(totals.revenges));
    std::printf("  start refusals per meeting %%:");
    for (size_t word = 0; word < totals.refusals.size(); ++word)
        if (totals.refusals[word])
            std::printf(" %s %.1f", WorldPvp::REFUSAL_WORDS[word], share(totals.refusals[word], totals.meetingsTotal));
    std::printf("\n  join refusals:");
    uint64_t joinTotal = 0;
    for (uint64_t const count : totals.joinRefusals)
        joinTotal += count;
    for (size_t word = 0; word < totals.joinRefusals.size(); ++word)
        if (totals.joinRefusals[word])
            std::printf(" %s %.1f %%", WorldPvp::REFUSAL_WORDS[word], share(totals.joinRefusals[word], joinTotal));
    std::printf("\n");
}
}  // namespace

int main()
{
    std::array<RunSpec, 7> const specs = {{{"1x neutral", 1.0, true, false},
                                           {"2x neutral", 2.0, true, false},
                                           {"4x neutral", 4.0, true, false},
                                           {"1x real", 1.0, false, false},
                                           {"2x real", 2.0, false, false},
                                           {"4x real", 4.0, false, false},
                                           {"quiet neutral", 1.0, true, true}}};
    std::vector<Totals> results(specs.size());
    std::atomic<size_t> nextRun{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 6; ++worker)
        workers.emplace_back(
            [&]()
            {
                for (size_t run = nextRun++; run < specs.size(); run = nextRun++)
                {
                    World world(specs[run], 0xC0FFEE00ULL + run);
                    results[run] = world.Run();
                }
            });
    for (std::thread& worker : workers)
        worker.join();

    FightRuleValues const values;
    double const rateAtZero = WorldPvp::StartRate(0.0f, values);
    Check(std::fabs(WorldPvp::StartRate(1.0f, values) / rateAtZero / 4.0 - 1.0) < 1.0e-3,
          "the budget at +1 is four times that at 0 (WP.16)");
    for (size_t run = 0; run < specs.size(); ++run)
    {
        RunSpec const& spec = specs[run];
        Totals const& totals = results[run];
        Report(spec, totals);
        std::string const name = spec.name;
        double const zero = PerBotHour(totals.starts[COHORT_ZERO], totals.bots[COHORT_ZERO]);
        double const one = PerBotHour(totals.starts[COHORT_ONE], totals.bots[COHORT_ONE]);
        if (spec.quiet)
        {
            Check(totals.quietChances > 0 && totals.maxQuietP <= values.chanceCap + 1.0e-6f,
                  name + ": the first chance after 8 quiet hours is at most 1/3");
            Check(zero < 0.5 * rateAtZero, name + ": in a quiet world the cap keeps starts below the rate");
        }
        else
        {
            double const meetings = PerBotHour(totals.meetingsTotal, BOTS);
            Check(std::fabs(meetings / (3.095 * spec.density) - 1.0) < 0.1,
                  name + ": meetings per bot hour scale WP2b's 3.1");
            if (spec.neutral)
            {
                Check(std::fabs(zero / STARTS_AT_ZERO - 1.0) <= STARTS_TOLERANCE,
                      name + ": aggression 0 starts once per 4 h within 15 %");
                Check(zero < rateAtZero, name + ": crowding does not lift starts above the rate");
            }
            Check(one > zero, name + ": aggression +1 starts more often than 0");
        }
        Check(totals.starts[COHORT_PEACEABLE] == 0 && totals.peaceableStarts == 0,
              name + ": no budget start below -0.5");
        Check(totals.maxP <= values.chanceCap + 1.0e-6f, name + ": no chance without a grudge above the cap");
        Check(totals.colourStarts == 0 && totals.campingStarts == 0 && totals.graveyardStarts == 0 &&
                  totals.calmStarts == 0 && totals.gapStarts == 0 && totals.redJoins == 0,
              name + ": no start on grey or red, camping, near a graveyard, in a calm area or in the gap");
        // Fights tick once a second, so the rules end a fight at most one tick after the stalemate time
        Check(totals.unended == 0 && totals.maxEndAgeMs < values.stalemateMs + SECOND,
              name + ": every fight ends, none later than the stalemate time");
        if (!spec.neutral)
        {
            Check(totals.joins[1] * totals.joinChances[0] > totals.joins[0] * totals.joinChances[1],
                  name + ": aggression raises the join chance");
        }
        else
        {
            // Only where the odds factor is 1: in the real pass caution also picks the fights (careful bots start
            // only at good odds, so they retreat less), and the two effects cancel to noise
            Check(totals.retreatsByCaution[1] * totals.fightersByCaution[0] >
                      totals.retreatsByCaution[0] * totals.fightersByCaution[1],
                  name + ": careful bots retreat more often");
        }
    }

    std::printf("fight-encounter-selftest: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
