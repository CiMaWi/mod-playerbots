/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERRULES_H
#define _PLAYERBOT_GATHERRULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "ItemWorthRules.h"
#include "SkillPathRules.h"

// The gather rules (professions plan WR13a, PLAN 3.9, DECISIONS WR.14, WR.15): what a node or a hide is worth to a
// bot, when a detour to a node pays, when a corpse is skinned, and the circuit a gatherer walks. One worth rule: a
// node's loot at the bot's price for it (what its skill path pays, else what a vendor pays; another buyer only behind
// SkillPath::BUYERS_BEYOND_VENDORS) weighed by its need for copper, plus the chance of a skill point at a minute of
// play, against the minutes it takes at the minute cost of its place (MaterialRules.h MinuteCost). Std only: include
// it in .cpp files and the planner, never in a central header. The circuit's order is the questing circuit's
// (QuestCircuit.h OrderStops, reached through MaterialRules.h and GoalModel.h).
namespace Gather
{
// Tuning values [E]
constexpr std::size_t POOL_DEPTH = 4;      // mother pools followed up from a node's own pool
constexpr float PERCENT = 100.0f;          // a pool member's chance is in percent
constexpr float CIRCUIT_YARDS = 1200.0f;   // a circuit's spawns lie this near its anchor (most respawn in 15 min)
constexpr std::size_t CIRCUIT_STOPS = 24;  // stops of a circuit at most, nearest first
constexpr float SESSION_MINUTES = Planner::Goals::GRIND_MINUTES;           // a circuit is planned like a grind spot
constexpr float SKIN_MINUTES = 5.0f / Planner::Goals::SECONDS_PER_MINUTE;  // the cast and the loot of one corpse
constexpr float MIN_RUN_SPEED = 1.0f;  // yards per second, the floor of a bot's pace
constexpr double PER_MILLE = 1000.0;
constexpr std::uint16_t SKINNING = 393;  // SKILL_SKINNING
constexpr std::size_t NO_PICK = std::numeric_limits<std::size_t>::max();
constexpr double REFUSED = std::numeric_limits<double>::lowest();

// One level of a node's pool chain (PoolMgr): the pool's MaxLimit, the member's own explicit chance (0: equal), the
// sum of the pool's explicit chances and its count of equal-chanced members
struct PoolLevel
{
    std::uint32_t maxLimit{0};
    float chance{0.0f};
    float explicitSum{0.0f};
    std::uint32_t equalMembers{0};
};

// The share of the time a pool member stands spawned, the core's roll (PoolMgr.cpp PoolGroup::SpawnObject): an
// explicitly chanced member its chance; an equal-chanced one the rest of the roll spread as MaxLimit over the
// equal members (at most always). A MaxLimit of 0 spawns nothing. Tuning value [E]: with MaxLimit above 1 the
// explicit roll fills one slot per spawn call, which this treats like any other slot; game event pools count as on
inline float MemberShare(PoolLevel const& level)
{
    if (!level.maxLimit)
        return 0.0f;
    if (level.chance > 0.0f)
        return std::clamp(level.chance / PERCENT, 0.0f, 1.0f);
    if (!level.equalMembers)
        return 0.0f;
    float const rest = std::clamp(1.0f - level.explicitSum / PERCENT, 0.0f, 1.0f);
    return rest * std::min(1.0f, static_cast<float>(level.maxLimit) / static_cast<float>(level.equalMembers));
}

// A spawn point's share: the product over its pool and the mother pools above it; 1 outside a pool
inline float PoolShare(std::vector<PoolLevel> const& chain)
{
    float share = 1.0f;
    for (std::size_t at = 0; at < chain.size() && at < POOL_DEPTH; ++at)
        share *= MemberShare(chain[at]);
    return std::clamp(share, 0.0f, 1.0f);
}

// One item a node or a hide gives: expected units and its prices to the bot
struct Yield
{
    std::uint32_t item{0};
    double perNode{0.0};
    std::uint32_t pathCopper{0};    // what the skill path pays for it (SkillPath::Material copperEach), 0: none
    std::uint32_t vendorCopper{0};  // a vendor's price
    std::uint32_t otherCopper{0};   // another buyer's price (the auction house, WR23), 0: none
};

struct SkillValue
{
    std::uint16_t skill{0};
    std::uint32_t value{0};
    std::uint32_t max{0};
};

struct Gatherer
{
    std::vector<SkillValue> skills;
    Materials::Rates rates;

    [[nodiscard]] SkillValue const* Find(std::uint16_t skill) const
    {
        for (SkillValue const& value : skills)
            if (value.skill == skill && value.value > 0)
                return &value;
        return nullptr;
    }
};

struct Node
{
    std::uint32_t id{0};  // the spawn id
    std::uint16_t skill{0};
    std::uint32_t required{0};
    Planner::Circuit::Point at;
    std::uint32_t respawnSecs{0};
    float share{1.0f};  // PoolShare: how often the spawn point holds a node
    Materials::PlaceRisk risk;
    std::vector<Yield> yields;
};

// A node in sight and the yards its detour adds to the bot's way
struct Seen
{
    Node node;
    float detourYards{0.0f};
};

// A skinnable corpse: the creature's level and the hide's yields
struct Hide
{
    std::uint32_t creatureLevel{0};
    std::vector<Yield> yields;
    Materials::PlaceRisk risk;
};

struct Circuit
{
    std::vector<std::uint32_t> stops;  // spawn ids in walk order, from and back to the anchor
    float lapYards{0.0f};
    float lapMinutes{0.0f};
    std::uint32_t longestRespawnSecs{0};
    std::uint32_t laps{0};  // in a session; 1 when walked once
    bool once{true};        // the lap is shorter than its longest respawn: walked once and left
    double worthPerLap{0.0};
};

inline bool Usable(Node const& node, Gatherer const& bot)
{
    SkillValue const* skill = bot.Find(node.skill);
    return skill && skill->value >= node.required;
}

// The yields' worth in value units: each unit at the best of the bot's prices for it, by its need for copper
inline double YieldsWorth(std::vector<Yield> const& yields, Materials::Rates const& rates,
                          bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    double worth = 0.0;
    for (Yield const& yield : yields)
        worth += std::max(yield.perNode, 0.0) *
                 WorldTravel::YieldWorthEach(yield.pathCopper, yield.vendorCopper, yield.otherCopper, buyers);
    return worth * std::max(rates.factor, 0.0f);
}

// A skill point's chance (the engine's colours, SkillPath::GatherChancePerMille) at a minute of play
inline double SkillPointWorth(SkillValue const& skill, std::uint32_t required, Materials::Rates const& rates)
{
    if (skill.value >= skill.max)
        return 0.0;
    return SkillPath::GatherChancePerMille(skill.value, required) / PER_MILLE * rates.minuteValue *
           Planner::Goals::SKILLUP_MINUTES;
}

// One gathering of the node while it stands there; nothing above the bot's skill
inline double NodeWorth(Node const& node, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    if (!Usable(node, bot))
        return 0.0;
    return YieldsWorth(node.yields, bot.rates, buyers) +
           SkillPointWorth(*bot.Find(node.skill), node.required, bot.rates);
}

inline double WalkMinutes(float yards, Materials::Rates const& rates)
{
    return std::max(yards, 0.0f) /
           (std::max(rates.runSpeed, MIN_RUN_SPEED) * static_cast<double>(Planner::Goals::SECONDS_PER_MINUTE));
}

// A node in sight: its worth less its detour and gathering at its place's minute cost; REFUSED above the skill or at
// a refused place
inline double Net(Seen const& seen, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    double const minuteCost = Materials::MinuteCost(seen.node.risk, bot.rates);
    if (!Usable(seen.node, bot) || minuteCost < 0.0)
        return REFUSED;
    return NodeWorth(seen.node, bot, buyers) -
           (WalkMinutes(seen.detourYards, bot.rates) + Planner::Goals::GATHER_MINUTES) * minuteCost;
}

// The node worth its detour (WR.14): the best net above 0; ties to the shorter detour, the safer place, the lower id.
// NO_PICK when none pays
inline std::size_t PassBy(std::vector<Seen> const& seen, Gatherer const& bot,
                          bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    std::size_t best = NO_PICK;
    double bestNet = 0.0;
    double bestCost = 0.0;
    for (std::size_t at = 0; at < seen.size(); ++at)
    {
        double const net = Net(seen[at], bot, buyers);
        if (!(net > 0.0))
            continue;
        double const cost = Materials::MinuteCost(seen[at].node.risk, bot.rates);
        bool better = best == NO_PICK || net > bestNet;
        if (!better && net == bestNet)
        {
            Seen const& other = seen[best];
            better = seen[at].detourYards != other.detourYards ? seen[at].detourYards < other.detourYards
                     : cost != bestCost                        ? cost < bestCost
                                                               : seen[at].node.id < other.node.id;
        }
        if (better)
        {
            best = at;
            bestNet = net;
            bestCost = cost;
        }
    }
    return best;
}

// A corpse is skinned (WR.14) after the fight, with room, at the skill, when the hide (its sale or the path's price,
// a skill point) is worth more than the skinning's minutes at the place
inline bool SkinPays(Hide const& hide, Gatherer const& bot, bool inCombat, std::uint32_t freeSlots,
                     bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    if (inCombat || !freeSlots)
        return false;
    SkillValue const* skill = bot.Find(SKINNING);
    std::uint32_t const required = SkillPath::SkinRequirement(hide.creatureLevel);
    double const minuteCost = Materials::MinuteCost(hide.risk, bot.rates);
    if (!skill || skill->value < required || minuteCost < 0.0)
        return false;
    double const worth = YieldsWorth(hide.yields, bot.rates, buyers) + SkillPointWorth(*skill, required, bot.rates);
    return worth > SKIN_MINUTES * minuteCost;
}

// The circuit from an anchor (WR.15): the usable spawns within the radius, nearest first (ties by id) up to
// CIRCUIT_STOPS, the shortest loop through them back to the anchor; while a stop costs more walking and gathering at
// its place than its expected worth (worth x share), the worst such stop leaves. A lap shorter than its longest
// respawn is walked once; else it laps as often as the session holds, so no spawn is visited again before it is back
inline Circuit BuildCircuit(Planner::Circuit::Point const& anchor, std::vector<Node> const& nodes, Gatherer const& bot,
                            float radius = CIRCUIT_YARDS, float sessionMinutes = SESSION_MINUTES,
                            bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    struct Candidate
    {
        std::size_t at;
        float yards;
        double expected;
        double minuteCost;
    };
    std::vector<Candidate> candidates;
    for (std::size_t at = 0; at < nodes.size(); ++at)
    {
        Node const& node = nodes[at];
        float const yards = Planner::Circuit::Yards(anchor, node.at);
        double const minuteCost = Materials::MinuteCost(node.risk, bot.rates);
        double const expected = NodeWorth(node, bot, buyers) * std::clamp(node.share, 0.0f, 1.0f);
        if (yards <= radius && minuteCost >= 0.0 && expected > 0.0)
            candidates.push_back({at, yards, expected, minuteCost});
    }
    std::sort(candidates.begin(), candidates.end(), [&nodes](Candidate const& a, Candidate const& b)
              { return a.yards != b.yards ? a.yards < b.yards : nodes[a.at].id < nodes[b.at].id; });
    if (candidates.size() > CIRCUIT_STOPS)
        candidates.resize(CIRCUIT_STOPS);

    auto const ordered = [&](std::vector<Candidate> const& kept)
    {
        std::vector<Planner::Circuit::Point> points;
        for (Candidate const& candidate : kept)
            points.push_back(nodes[candidate.at].at);
        std::vector<Candidate> walk;
        for (std::uint32_t const i : Planner::Circuit::OrderStops(anchor, points, anchor))
            walk.push_back(kept[i]);
        return walk;
    };
    auto const gatherMinutes = [&nodes](Candidate const& c)
    { return std::clamp(nodes[c.at].share, 0.0f, 1.0f) * Planner::Goals::GATHER_MINUTES; };

    // Prune on the walk (a stop spliced out), then order what is kept once more
    std::vector<Candidate> walk = ordered(candidates);
    for (std::size_t round = 0; round < CIRCUIT_STOPS && !walk.empty(); ++round)
    {
        std::size_t worst = walk.size();
        double worstNet = 0.0;
        for (std::size_t i = 0; i < walk.size(); ++i)
        {
            Planner::Circuit::Point const& before = i ? nodes[walk[i - 1].at].at : anchor;
            Planner::Circuit::Point const& after = i + 1 < walk.size() ? nodes[walk[i + 1].at].at : anchor;
            Planner::Circuit::Point const& here = nodes[walk[i].at].at;
            float const saving = Planner::Circuit::Yards(before, here) + Planner::Circuit::Yards(here, after) -
                                 Planner::Circuit::Yards(before, after);
            double const net =
                walk[i].expected - (WalkMinutes(saving, bot.rates) + gatherMinutes(walk[i])) * walk[i].minuteCost;
            if (net < worstNet || (net == worstNet && worst < walk.size() && net < 0.0 &&
                                   nodes[walk[i].at].id < nodes[walk[worst].at].id))
            {
                worst = i;
                worstNet = net;
            }
        }
        if (worst == walk.size())
            break;
        walk.erase(walk.begin() + static_cast<std::ptrdiff_t>(worst));
    }
    std::sort(walk.begin(), walk.end(), [&nodes](Candidate const& a, Candidate const& b)
              { return a.yards != b.yards ? a.yards < b.yards : nodes[a.at].id < nodes[b.at].id; });
    walk = ordered(walk);

    Circuit circuit;
    if (walk.empty())
        return circuit;
    std::vector<Planner::Circuit::Point> points;
    std::vector<std::uint32_t> order;
    double gathering = 0.0;
    for (Candidate const& candidate : walk)
    {
        Node const& node = nodes[candidate.at];
        order.push_back(static_cast<std::uint32_t>(points.size()));
        points.push_back(node.at);
        circuit.stops.push_back(node.id);
        circuit.longestRespawnSecs = std::max(circuit.longestRespawnSecs, node.respawnSecs);
        circuit.worthPerLap += candidate.expected;
        gathering += gatherMinutes(candidate);
    }
    circuit.lapYards = Planner::Circuit::PathYards(anchor, points, order, anchor);
    circuit.lapMinutes = static_cast<float>(WalkMinutes(circuit.lapYards, bot.rates) + gathering);
    circuit.once = !(circuit.lapMinutes > 0.0f) || circuit.lapMinutes * Planner::Goals::SECONDS_PER_MINUTE <
                                                       static_cast<float>(circuit.longestRespawnSecs);
    circuit.laps =
        circuit.once ? 1u : std::max(1u, static_cast<std::uint32_t>(std::floor(sessionMinutes / circuit.lapMinutes)));
    return circuit;
}
}  // namespace Gather

#endif
