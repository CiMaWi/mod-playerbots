/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERRULES_H
#define _PLAYERBOT_GATHERRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "GatherMemoryState.h"
#include "ItemWorthRules.h"
#include "SkillPathRules.h"

// The gather rules (professions plan WR13a, PLAN 3.9, DECISIONS WR.14, WR.15): what a node or a hide is worth to a
// bot, when a detour to a node pays, when a corpse is skinned, and the circuit a gatherer walks. One worth rule: a
// node's loot at the bot's price for it (what its skill path pays, else what a vendor pays; another buyer only behind
// SkillPath::BUYERS_BEYOND_VENDORS) weighed by its need for copper, plus the chance of a skill point at a minute of
// play, against the minutes it takes at the minute cost of its place (MaterialRules.h MinuteCost). Std only: include
// it in .cpp files and the planner, never in a central header. The circuit's order is the questing circuit's
// (QuestCircuit.h OrderStops, reached through MaterialRules.h and GoalModel.h).
// A stop is a node or a creature's spawn point (WR20b, DECISIONS WR.24): a kill is worth its loot, coins, hide and XP
// against the walk, the fight (Materials::FightMinutes, the drop rows' estimate) and the loot at the place's cost. A
// creature the bot may not fight (Materials::FightRefused: more than FIGHT_LEVELS_ABOVE levels above it, an elite near
// its level for a bot alone; or friendly to its side) is no stop, and creature stops join a circuit only for a bot
// that wants a material they give or is short of gold, whatever their XP.
namespace Gather
{
// Tuning values [E]
constexpr std::size_t POOL_DEPTH = 4;      // mother pools followed up from a node's own pool
constexpr float PERCENT = 100.0f;          // a pool member's chance is in percent
constexpr float CIRCUIT_YARDS = 1200.0f;   // a circuit's spawns lie this near its anchor (most respawn in 15 min)
constexpr std::size_t CIRCUIT_STOPS = 24;  // stops of a circuit at most, nearest first
constexpr float SESSION_MINUTES = Planner::Goals::GRIND_MINUTES;  // a circuit is planned like a grind spot
constexpr float SKIN_MINUTES = Materials::SKIN_SECONDS / Planner::Goals::SECONDS_PER_MINUTE;  // one corpse's skin
constexpr float LOOT_MINUTES = Materials::LOOT_SECONDS / Planner::Goals::SECONDS_PER_MINUTE;  // one corpse's loot
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
    Recipes::ServerRates server;               // the server's skill-gain step and colour chances (WR29); default: stock
    std::uint32_t level{0};                    // the fight estimate's and the grey level's; 0 counts as 1
    std::uint8_t side{Materials::BOTH_SIDES};  // its side's bit (Materials::SideBit); both: not known
    bool shortOfGold{false};                   // the planner's need: money below the reserve

    [[nodiscard]] SkillValue const* Find(std::uint16_t skill) const
    {
        for (SkillValue const& value : skills)
            if (value.skill == skill && value.value > 0)
                return &value;
        return nullptr;
    }
};

enum class StopKind : std::uint8_t
{
    Object,   // a gathering node's spawn point
    Creature  // a creature's spawn point: a kill, its loot and its hide
};

// A creature stop's kind (WR20b): the facts its kill is priced by. Its loot per kill is the stop's yields
struct Creature
{
    std::uint32_t minLevel{0};
    std::uint32_t maxLevel{0};
    float eliteShare{0.0f};      // 0 to 1
    std::uint8_t friendlyTo{0};  // a bit per side that cannot attack it (Recipes::DropRow::friendlyTo)
    float xpPerKill{0.0f};       // at the bot's level, before the grey rule
    double coinCopper{0.0};      // the coins one kill drops at the server's money rate (CoinsPerKill)
    std::vector<Yield> hide;     // a skin's units per kill; empty: not skinnable
};

struct Node
{
    std::uint32_t id{0};  // the spawn id (gameobject or creature, by kind); a stop is known by its key (Key)
    std::uint16_t skill{0};
    std::uint32_t required{0};
    Planner::Circuit::Point at;
    std::uint32_t respawnSecs{0};
    float share{1.0f};  // PoolShare: how often the spawn point holds a node; a creature's: 1 unless seen dead
    Materials::PlaceRisk risk;
    std::vector<Yield> yields;  // a node's per gathering, a creature's loot per kill
    StopKind kind{StopKind::Object};
    Creature creature;  // a creature stop's kind
};

inline std::uint32_t Key(Node const& node) { return StopKey(node.kind == StopKind::Creature, node.id); }

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

// A creature the bot may fight (WR20b): not friendly to its side and not refused by the fight estimate (its top level
// more than Materials::FIGHT_LEVELS_ABOVE above the bot, or an elite near its level for a bot alone)
inline bool MayFight(Node const& node, Gatherer const& bot)
{
    Creature const& creature = node.creature;
    return node.kind == StopKind::Creature && !(creature.friendlyTo & bot.side) &&
           !Materials::FightRefused(std::max(creature.minLevel, creature.maxLevel), creature.eliteShare, bot.level,
                                    bot.rates.groupSize);
}

// A node at the bot's skill, a creature it may fight
inline bool Usable(Node const& node, Gatherer const& bot)
{
    if (node.kind == StopKind::Creature)
        return MayFight(node, bot);
    SkillValue const* skill = bot.Find(node.skill);
    return skill && skill->value >= node.required;
}

// The yields' worth in value units: each unit at the best of the bot's prices for it; a unit the skill path pays most
// for by the bot's exchange (the path's price is what getting it otherwise costs, in neutral copper: the planner's
// item worth), one it sells by its need for copper (WR20b, PLAN 3.9; CircuitOption splits the same way)
inline double YieldsWorth(std::vector<Yield> const& yields, Materials::Rates const& rates,
                          bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    double worth = 0.0;
    for (Yield const& yield : yields)
    {
        std::uint32_t const each =
            WorldTravel::YieldWorthEach(yield.pathCopper, yield.vendorCopper, yield.otherCopper, buyers);
        bool const used = yield.pathCopper > yield.vendorCopper && each == yield.pathCopper;
        worth += std::max(yield.perNode, 0.0) * each * std::max(used ? rates.exchange : rates.factor, 0.0f);
    }
    return worth;
}

// The skill points one take is expected to give: the chance of a skill-up (the engine's colours at the server's
// chances, SkillPath::GatherChancePerMille) times the server's gathering step, at most to the cap (WR29)
inline double ExpectedPoints(SkillValue const& skill, std::uint32_t required, Recipes::ServerRates const& server)
{
    if (skill.value >= skill.max)
        return 0.0;
    std::uint32_t const points = std::min(server.Step(false), skill.max - skill.value);
    return SkillPath::GatherChancePerMille(skill.value, required, skill.skill, server) / PER_MILLE * points;
}

// A take's skill points at a minute of play each
inline double SkillPointWorth(SkillValue const& skill, std::uint32_t required, Gatherer const& bot)
{
    return ExpectedPoints(skill, required, bot.server) * bot.rates.minuteValue * Planner::Goals::SKILLUP_MINUTES;
}

// A kill's coins (WR29): the mean of the creature's gold range times the server's money rate, as Loot::
// generateMoneyLoot rolls it (a range of one value or an inverted one gives its top)
inline double CoinsPerKill(std::uint32_t minGold, std::uint32_t maxGold, float moneyRate)
{
    if (!maxGold)
        return 0.0;
    double const mean = maxGold <= minGold ? maxGold : (static_cast<double>(minGold) + maxGold) / 2.0;
    return mean * std::max(moneyRate, 0.0f);
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
    double const worth = YieldsWorth(hide.yields, bot.rates, buyers) + SkillPointWorth(*skill, required, bot);
    return worth > SKIN_MINUTES * minuteCost;
}

// A creature stop's hide, at the top of its levels (the skill it asks, as Materials::DropCost)
inline Hide HideOf(Node const& node)
{
    return {std::max(node.creature.minLevel, node.creature.maxLevel), node.creature.hide, node.risk};
}

// A creature stop's corpse is skinned on the circuit when its hide pays (SkinPays, room assumed): never without the
// skill its level asks
inline bool Skins(Node const& node, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    return node.kind == StopKind::Creature && !node.creature.hide.empty() &&
           SkinPays(HideOf(node), bot, false, 1, buyers);
}

// A kill's XP in value units (the planner's value is XP): none at or below the bot's grey level
inline double KillXp(Node const& node, Gatherer const& bot)
{
    std::uint32_t const top = std::max(node.creature.minLevel, node.creature.maxLevel);
    return top > Planner::Goals::GreyLevel(std::max<std::uint32_t>(bot.level, 1))
               ? static_cast<double>(std::max(node.creature.xpPerKill, 0.0f))
               : 0.0;
}

// What one kill gives the bot, in value units: its loot at the bot's price for each unit (the path's for a wanted
// material, else a vendor's) and its coins, both by the need for copper; the hide and its skill point when skinned;
// the XP
struct KillWorth
{
    double loot{0.0};
    double hide{0.0};
    double xp{0.0};

    [[nodiscard]] double Total() const { return loot + hide + xp; }
};

inline KillWorth WorthOfKill(Node const& node, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    KillWorth worth;
    if (!MayFight(node, bot))
        return worth;
    worth.loot = YieldsWorth(node.yields, bot.rates, buyers) +
                 std::max(node.creature.coinCopper, 0.0) * std::max(bot.rates.factor, 0.0f);
    if (Skins(node, bot, buyers))
    {
        Hide const hide = HideOf(node);
        worth.hide = YieldsWorth(hide.yields, bot.rates, buyers) +
                     SkillPointWorth(*bot.Find(SKINNING), SkillPath::SkinRequirement(hide.creatureLevel), bot);
    }
    worth.xp = KillXp(node, bot);
    return worth;
}

// A kill's fight minutes: the estimate at the middle of the creature's levels (PLAN 3.18)
inline double FightMinutesOf(Node const& node, Gatherer const& bot)
{
    Creature const& creature = node.creature;
    double const level = (creature.minLevel + std::max(creature.minLevel, creature.maxLevel)) / 2.0;
    return Materials::FightMinutes(level, bot.level, creature.eliteShare);
}

// A kill's minutes once at the spawn: the fight estimate, the loot, the skin when taken
inline double KillMinutes(Node const& node, Gatherer const& bot, bool skin)
{
    return FightMinutesOf(node, bot) + LOOT_MINUTES + (skin ? SKIN_MINUTES : 0.0f);
}

// The minutes a stop takes once reached: a node's gathering, a creature's kill
inline double StopMinutes(Node const& node, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    if (node.kind != StopKind::Creature)
        return Planner::Goals::GATHER_MINUTES;
    return KillMinutes(node, bot, Skins(node, bot, buyers));
}

// A creature stop gives a material the bot wants (WR.24): a unit its skill path pays for, in the loot or in a hide it
// skins
inline bool GivesWanted(Node const& node, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    auto const wanted = [](std::vector<Yield> const& yields)
    {
        return std::any_of(yields.begin(), yields.end(),
                           [](Yield const& yield) { return yield.perNode > 0.0 && yield.pathCopper > 0; });
    };
    return MayFight(node, bot) && (wanted(node.yields) || (Skins(node, bot, buyers) && wanted(node.creature.hide)));
}

// One gathering of the node while it stands there, or one kill of the creature; nothing above the bot's skill or
// from a creature it may not fight
inline double NodeWorth(Node const& node, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    if (!Usable(node, bot))
        return 0.0;
    if (node.kind == StopKind::Creature)
        return WorthOfKill(node, bot, buyers).Total();
    return YieldsWorth(node.yields, bot.rates, buyers) + SkillPointWorth(*bot.Find(node.skill), node.required, bot);
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
           (WalkMinutes(seen.detourYards, bot.rates) + StopMinutes(seen.node, bot, buyers)) * minuteCost;
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

// Why a target in sight is passed (WR13c); counted by cause once per status
enum class PassCause : std::uint8_t
{
    None,  // taken, or not the bot's to take (no skill or above it, no room, a fight): no pass
    NotWorthIt,
    DetourTooLong,
    TooDangerous,
    Count
};
constexpr std::size_t PASS_CAUSES = static_cast<std::size_t>(PassCause::Count);
constexpr std::size_t PASS_RING = 16;  // the targets a bot keeps judged in a status

struct Verdict
{
    bool take{false};
    PassCause cause{PassCause::None};
    double net{0.0};  // its worth less its minutes; REFUSED at a refused place
};

// A node in sight (WR.14): taken when its net pays (Net); else passed as too dangerous (a refused place), not worth it
// (it does not pay even at the bot's feet) or a detour too long; a node the bot cannot gather is no pass
inline Verdict JudgeNode(Seen const& seen, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    if (!Usable(seen.node, bot))
        return {};
    double const minuteCost = Materials::MinuteCost(seen.node.risk, bot.rates);
    if (minuteCost < 0.0)
        return {false, PassCause::TooDangerous, REFUSED};
    double const net = Net(seen, bot, buyers);
    if (net > 0.0)
        return {true, PassCause::None, net};
    bool const atFeet = NodeWorth(seen.node, bot, buyers) - StopMinutes(seen.node, bot, buyers) * minuteCost > 0.0;
    return {false, atFeet ? PassCause::DetourTooLong : PassCause::NotWorthIt, net};
}

// A skinnable corpse (WR.14): skinned when the hide pays (SkinPays); in a fight, without room or above the skill it is
// no pass (judged again after the fight); at a refused place it is too dangerous, else the hide is not worth it
inline Verdict JudgeHide(Hide const& hide, Gatherer const& bot, bool inCombat, std::uint32_t freeSlots,
                         bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    SkillValue const* skill = bot.Find(SKINNING);
    std::uint32_t const required = SkillPath::SkinRequirement(hide.creatureLevel);
    if (inCombat || !freeSlots || !skill || skill->value < required)
        return {};
    double const minuteCost = Materials::MinuteCost(hide.risk, bot.rates);
    if (minuteCost < 0.0)
        return {false, PassCause::TooDangerous, REFUSED};
    double const net = YieldsWorth(hide.yields, bot.rates, buyers) + SkillPointWorth(*skill, required, bot) -
                       SKIN_MINUTES * minuteCost;
    bool const take = SkinPays(hide, bot, inCombat, freeSlots, buyers);
    return {take, take ? PassCause::None : PassCause::NotWorthIt, net};
}

// Why a circuit stop found no live node at its spawn point (WR13d, the diagnosis): the facts the bot could read at
// that moment
struct DownFacts
{
    bool ownTake{false};    // the bot's own gather cast went at the spawn within the spawn's respawn
    bool otherTake{false};  // another bot's did
    bool pooled{false};     // the spawn is a pool member
    bool held{false};       // in a pool: the pool holds its node at this point; outside one: an object of the spawn
    bool poolUp{false};     // another point of the pool (or of its mother pool) holds a node
    bool notReady{false};   // an object of the spawn is spawned but not ready to gather
};
constexpr std::string_view DOWN_OWN_TAKE = "down_own_take";
constexpr std::string_view DOWN_TAKEN_OTHER = "down_taken_other";
constexpr std::string_view DOWN_POOL_OTHER = "down_pool_other";
constexpr std::string_view DOWN_NO_NODE = "down_no_node";
constexpr std::string_view DOWN_NOT_READY = "down_not_ready";
constexpr std::string_view DOWN_UNKNOWN = "down_unknown";

// The stop row's cause word, the most specific first: the bot's own take, another bot's, the pool's node at another
// of its points, no node (no point of the pool up; outside a pool no object of the spawn), the node held here but not
// ready; else none of these (the node is held here and respawning, its take unseen: a player's, or one not recorded)
inline std::string_view DownCause(DownFacts const& facts)
{
    if (facts.ownTake)
        return DOWN_OWN_TAKE;
    if (facts.otherTake)
        return DOWN_TAKEN_OTHER;
    if (!facts.held)
        return facts.pooled && facts.poolUp ? DOWN_POOL_OTHER : DOWN_NO_NODE;
    return facts.notReady ? DOWN_NOT_READY : DOWN_UNKNOWN;
}

// What the bot knows of the spawn points when it plans (WR13d, PLAN 3.9: a spawn point seen empty is skipped, the
// yield counts what is left): one it sees holding a node is up (share 1); one it sees empty, or took or found empty
// within its respawn, holds none (share 0: no circuit takes it, BuildCircuit); the others keep their pool share. up
// and down are sorted stop keys (Key); a point in both is up (what the bot sees now)
inline void ApplySight(std::vector<Node>& nodes, std::vector<std::uint32_t> const& up,
                       std::vector<std::uint32_t> const& down)
{
    for (Node& node : nodes)
        if (std::binary_search(up.begin(), up.end(), Key(node)))
            node.share = 1.0f;
        else if (std::binary_search(down.begin(), down.end(), Key(node)))
            node.share = 0.0f;
}

struct PassEntry
{
    std::uint64_t target{0};  // the object's guid
    PassCause cause{PassCause::None};
    bool taken{false};
    double net{0.0};
    float detourYards{0.0f};
};

// The targets judged in a status (WR13c): each is judged again at every look (a node ahead comes nearer) and counted
// once, by its last cause, when it leaves the ring or the status ends untaken; a take clears its pass
struct Passes
{
    std::array<PassEntry, PASS_RING> ring{};
    std::size_t next{0};
    std::array<std::uint32_t, PASS_CAUSES> counted{};

    PassEntry const& Judge(std::uint64_t target, Verdict const& verdict, float detourYards)
    {
        for (PassEntry& entry : ring)
            if (entry.target == target)
            {
                if (!entry.taken)
                {
                    entry.cause = verdict.cause;
                    entry.net = verdict.net;
                    entry.detourYards = detourYards;
                }
                return entry;
            }
        PassEntry& slot = ring[next];
        next = (next + 1) % PASS_RING;
        Count(slot);
        slot = {target, verdict.cause, false, verdict.net, detourYards};
        return slot;
    }

    // The cast on a target the gate let through: its entry, now taken; null when it was not let through here or was
    // taken already
    PassEntry const* Take(std::uint64_t target)
    {
        for (PassEntry& entry : ring)
            if (entry.target == target && !entry.taken && entry.cause == PassCause::None)
            {
                entry.taken = true;
                return &entry;
            }
        return nullptr;
    }

    // The status's passes by cause; the ring and the counts start anew
    std::array<std::uint32_t, PASS_CAUSES> Flush()
    {
        for (PassEntry const& entry : ring)
            Count(entry);
        std::array<std::uint32_t, PASS_CAUSES> const passes = counted;
        *this = Passes{};
        return passes;
    }

    void Count(PassEntry const& entry)
    {
        if (entry.target && !entry.taken && entry.cause != PassCause::None)
            ++counted[static_cast<std::size_t>(entry.cause)];
    }
};

// The circuit from an anchor (WR.15, WR20b): the usable stops (nodes at the skill, creatures the bot may fight) within
// the radius, nearest first (ties by key) up to CIRCUIT_STOPS, the shortest loop through them back to the anchor;
// while a stop costs more walking and its own minutes (a gathering, a kill) at its place than its expected worth
// (worth x share), the worst such stop leaves. A lap shorter than its longest respawn (a creature's as a node's) is
// walked once; else it laps as often as the session holds, so no spawn is visited again before it is back. Creature
// stops stay only for a bot short of gold or when one of those kept gives a material it wants (GivesWanted, WR.24);
// else the circuit is built again from the nodes alone, whatever the creatures' XP
namespace Detail
{
inline Circuit Loop(Planner::Circuit::Point const& anchor, std::vector<Node> const& nodes, Gatherer const& bot,
                    float radius, float sessionMinutes, bool buyers, bool creatures, bool& wanted)
{
    struct Candidate
    {
        std::size_t at;
        float yards;
        double expected;
        double minuteCost;
        double stopMinutes;  // a gathering's or a kill's, times the share
    };
    std::vector<Candidate> candidates;
    for (std::size_t at = 0; at < nodes.size(); ++at)
    {
        Node const& node = nodes[at];
        if (!creatures && node.kind == StopKind::Creature)
            continue;
        float const yards = Planner::Circuit::Yards(anchor, node.at);
        double const minuteCost = Materials::MinuteCost(node.risk, bot.rates);
        double const share = std::clamp(node.share, 0.0f, 1.0f);
        double const expected = NodeWorth(node, bot, buyers) * share;
        if (yards <= radius && minuteCost >= 0.0 && expected > 0.0)
            candidates.push_back({at, yards, expected, minuteCost, StopMinutes(node, bot, buyers) * share});
    }
    auto const nearer = [&nodes](Candidate const& a, Candidate const& b)
    { return a.yards != b.yards ? a.yards < b.yards : Key(nodes[a.at]) < Key(nodes[b.at]); };
    std::sort(candidates.begin(), candidates.end(), nearer);
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
                walk[i].expected - (WalkMinutes(saving, bot.rates) + walk[i].stopMinutes) * walk[i].minuteCost;
            if (net < worstNet || (net == worstNet && worst < walk.size() && net < 0.0 &&
                                   Key(nodes[walk[i].at]) < Key(nodes[walk[worst].at])))
            {
                worst = i;
                worstNet = net;
            }
        }
        if (worst == walk.size())
            break;
        walk.erase(walk.begin() + static_cast<std::ptrdiff_t>(worst));
    }
    std::sort(walk.begin(), walk.end(), nearer);
    walk = ordered(walk);

    Circuit circuit;
    if (walk.empty())
        return circuit;
    std::vector<Planner::Circuit::Point> points;
    std::vector<std::uint32_t> order;
    double stopping = 0.0;
    for (Candidate const& candidate : walk)
    {
        Node const& node = nodes[candidate.at];
        order.push_back(static_cast<std::uint32_t>(points.size()));
        points.push_back(node.at);
        circuit.stops.push_back(Key(node));
        circuit.longestRespawnSecs = std::max(circuit.longestRespawnSecs, node.respawnSecs);
        circuit.worthPerLap += candidate.expected;
        stopping += candidate.stopMinutes;
        wanted = wanted || (node.kind == StopKind::Creature && GivesWanted(node, bot, buyers));
    }
    circuit.lapYards = Planner::Circuit::PathYards(anchor, points, order, anchor);
    circuit.lapMinutes = static_cast<float>(WalkMinutes(circuit.lapYards, bot.rates) + stopping);
    circuit.once = !(circuit.lapMinutes > 0.0f) || circuit.lapMinutes * Planner::Goals::SECONDS_PER_MINUTE <
                                                       static_cast<float>(circuit.longestRespawnSecs);
    circuit.laps =
        circuit.once ? 1u : std::max(1u, static_cast<std::uint32_t>(std::floor(sessionMinutes / circuit.lapMinutes)));
    return circuit;
}
}  // namespace Detail

inline Circuit BuildCircuit(Planner::Circuit::Point const& anchor, std::vector<Node> const& nodes, Gatherer const& bot,
                            float radius = CIRCUIT_YARDS, float sessionMinutes = SESSION_MINUTES,
                            bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    bool wanted = false;
    Circuit circuit = Detail::Loop(anchor, nodes, bot, radius, sessionMinutes, buyers, true, wanted);
    if (wanted || bot.shortOfGold || std::none_of(circuit.stops.begin(), circuit.stops.end(), IsCreatureStop))
        return circuit;
    return Detail::Loop(anchor, nodes, bot, radius, sessionMinutes, buyers, false, wanted);
}

// The planner's gather option of a circuit (WR13b, WR20b): the first stop its target, the laps' minutes, the stops'
// yields x share x laps in raw copper (a unit the skill path pays more for than a vendor as item worth, else as
// copper: the planner weighs each by its need; a kill's coins and its skinned hide included), the skill points
// expected (the engine's chance x share x laps, at most the skill's room; a skinned hide's as a node's), the kills'
// XP (none for grey) and the stops' mean risk. An empty circuit is no option (id 0); one of kills without a skin asks
// no skill (skill 0)
inline Planner::Goals::GatherNode CircuitOption(Circuit const& circuit, std::vector<Node> const& nodes,
                                                Gatherer const& bot, std::uint32_t map,
                                                bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    Planner::Goals::GatherNode option;
    float const laps = static_cast<float>(std::max(circuit.laps, 1u));
    double copper = 0.0;
    double item = 0.0;
    double xp = 0.0;
    double levels = 0.0;
    double elite = 0.0;
    double takes = 0.0;  // the estimate the rows carry (WR25a): takes, units and fight minutes per lap
    double units = 0.0;
    double fight = 0.0;
    std::vector<std::pair<std::uint16_t, double>> points;  // skill -> points expected
    std::size_t counted = 0;
    auto const addYields = [&](std::vector<Yield> const& yields, double share)
    {
        for (Yield const& yield : yields)
        {
            std::uint32_t const each =
                WorldTravel::YieldWorthEach(yield.pathCopper, yield.vendorCopper, yield.otherCopper, buyers);
            double const worth = std::max(yield.perNode, 0.0) * share * each;
            units += std::max(yield.perNode, 0.0) * share;
            if (yield.pathCopper > yield.vendorCopper && each == yield.pathCopper)
                item += worth;
            else
                copper += worth;
        }
    };
    auto const addPoints = [&](std::uint16_t skill, std::uint32_t required, double share)
    {
        SkillValue const* value = bot.Find(skill);
        double const chance = ExpectedPoints(*value, required, bot.server) * share;
        auto const entry =
            std::find_if(points.begin(), points.end(), [skill](auto const& pair) { return pair.first == skill; });
        if (entry == points.end())
            points.emplace_back(skill, chance);
        else
            entry->second += chance;
        if (!option.skill)
        {
            option.skill = skill;
            option.required = static_cast<std::uint16_t>(std::min<std::uint32_t>(required, UINT16_MAX));
        }
    };
    for (std::uint32_t const stop : circuit.stops)
    {
        auto const at =
            std::find_if(nodes.begin(), nodes.end(), [stop](Node const& node) { return Key(node) == stop; });
        if (at == nodes.end() || !Usable(*at, bot))
            continue;
        Node const& node = *at;
        if (!counted)
        {
            option.id = stop;
            option.place = {map, node.at.x, node.at.y, node.at.z};
        }
        ++counted;
        double const share = std::clamp(node.share, 0.0f, 1.0f);
        takes += share;
        addYields(node.yields, share);
        if (node.kind == StopKind::Creature)
        {
            fight += FightMinutesOf(node, bot) * share;
            copper += std::max(node.creature.coinCopper, 0.0) * share;
            xp += KillXp(node, bot) * share;
            if (Skins(node, bot, buyers))
            {
                addYields(node.creature.hide, share);
                addPoints(SKINNING, SkillPath::SkinRequirement(HideOf(node).creatureLevel), share);
            }
        }
        else
            addPoints(node.skill, node.required, share);
        levels += node.risk.levelsAbove;
        elite += node.risk.eliteShare;
    }
    if (!counted)
        return {};
    double skillPoints = 0.0;
    for (auto const& [skillId, expected] : points)
    {
        SkillValue const* skill = bot.Find(skillId);
        double const room = skill->max > skill->value ? static_cast<double>(skill->max - skill->value) : 0.0;
        skillPoints += std::min(expected * laps, room);
    }
    option.gatherMinutes = circuit.lapMinutes * laps;
    option.walkMinutes = 0.0f;  // the lap starts and ends at the anchor: its minutes hold the walk
    option.copperWorth = static_cast<float>(copper * laps);
    option.itemWorth = static_cast<float>(item * laps);
    option.skillPoints = static_cast<float>(skillPoints);
    option.xpWorth = static_cast<float>(xp * laps);
    option.levelsAbove = static_cast<float>(levels / static_cast<double>(counted));
    option.eliteShare = static_cast<float>(elite / static_cast<double>(counted));
    option.stops = circuit.stops;
    option.laps = std::max(circuit.laps, 1u);
    option.takes = static_cast<float>(takes * laps);
    option.units = static_cast<float>(units * laps);
    option.fightMinutes = static_cast<float>(fight * laps);
    return option;
}

// A camp (WR20b, PLAN 3.9): one creature kind's spawns near an anchor, ranked like a grind spot by what a kill there
// earns a minute: the kill's worth (WorthOfKill) over its minutes (the walk to the next spawn, the fight, the loot,
// and the skin where Skins allows it and it raises the rate) less the place's minute cost. Refused: a creature the
// bot may not fight, a camp that gives nothing it wants while it has gold enough (WR.24), a refused place. So the
// rate never falls with more of a wanted material and never rises with the place's danger or the creature's elite
// share. The planner is offered the circuit built at the best camp (BuildCircuit from its anchor)
struct Camp
{
    Planner::Circuit::Point anchor;
    Node creature;             // a creature stop standing for the kind; its risk is the camp's place
    float spacingYards{0.0f};  // the walk from one spawn to the next (Recipes::DropRow::spacing)
};

constexpr std::string_view CAMP_NO_FIGHT = "no_fight";
constexpr std::string_view CAMP_WANTS_NOTHING = "wants_nothing";
constexpr std::string_view CAMP_PLACE = "place_refused";

struct CampWeight
{
    double rate{REFUSED};  // value a minute above the place's minute cost; REFUSED: no camp for this bot
    KillWorth worth;       // one kill's; the hide only when skinned at the rate
    double minutes{0.0};   // one kill's, the walk to it included
    double minuteCost{0.0};
    bool skinned{false};
    std::string_view refusal;  // empty when weighed
};

inline CampWeight WeighCamp(Camp const& camp, Gatherer const& bot, bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    CampWeight weight;
    Node const& node = camp.creature;
    if (!MayFight(node, bot))
    {
        weight.refusal = CAMP_NO_FIGHT;
        return weight;
    }
    if (!bot.shortOfGold && !GivesWanted(node, bot, buyers))
    {
        weight.refusal = CAMP_WANTS_NOTHING;
        return weight;
    }
    weight.minuteCost = Materials::MinuteCost(node.risk, bot.rates);
    if (weight.minuteCost < 0.0)
    {
        weight.refusal = CAMP_PLACE;
        return weight;
    }
    KillWorth const worth = WorthOfKill(node, bot, buyers);
    double const walk = WalkMinutes(camp.spacingYards, bot.rates);
    weight.worth = worth;
    weight.worth.hide = 0.0;
    weight.minutes = walk + KillMinutes(node, bot, false);
    double rate = weight.worth.Total() / weight.minutes;
    if (Skins(node, bot, buyers))
    {
        double const skinMinutes = walk + KillMinutes(node, bot, true);
        double const skinRate = worth.Total() / skinMinutes;
        if (skinRate > rate)
        {
            rate = skinRate;
            weight.worth = worth;
            weight.minutes = skinMinutes;
            weight.skinned = true;
        }
    }
    weight.rate = rate - weight.minuteCost;
    return weight;
}

// The best camp: the highest rate; ties to the safer place, the shorter kill, the lower key. NO_PICK when every camp
// is refused. Whether its circuit beats the bot's other goals is the planner's choice
inline std::size_t PickCamp(std::vector<Camp> const& camps, Gatherer const& bot,
                            bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    std::size_t best = NO_PICK;
    CampWeight bestWeight;
    for (std::size_t at = 0; at < camps.size(); ++at)
    {
        CampWeight const weight = WeighCamp(camps[at], bot, buyers);
        if (weight.rate == REFUSED)
            continue;
        bool const better =
            best == NO_PICK || weight.rate > bestWeight.rate ||
            (weight.rate == bestWeight.rate &&
             (weight.minuteCost != bestWeight.minuteCost ? weight.minuteCost < bestWeight.minuteCost
              : weight.minutes != bestWeight.minutes     ? weight.minutes < bestWeight.minutes
                                                         : Key(camps[at].creature) < Key(camps[best].creature)));
        if (better)
        {
            best = at;
            bestWeight = weight;
        }
    }
    return best;
}

// The walk to a stop and its watch (WR13d-2): the bot's own movement, never its distance to the stop, so a walk that
// first leads away from the node (round a hill, to a road) keeps its stop. Moving WALK_MOVE_YARDS from the anchor moves
// the anchor there and restarts the clock; a pause longer than WALK_GAP_MS between two ticks (a fight, the loot,
// another action) is not counted, and after it the bot is measured from where it stands. `first`: the walk's first
// tick (the anchor is set, the clock is 0). True: the bot walked toward the stop WALK_STALL_MS without moving
constexpr std::uint32_t WALK_GAP_MS = 5000;
constexpr std::uint32_t WALK_STALL_MS = 30000;
constexpr float WALK_MOVE_YARDS = 5.0f;

inline bool WalkStalled(std::array<float, 3>& anchor, std::uint32_t& stalledMs, bool first, std::uint32_t sinceLastMs,
                        std::array<float, 3> const& at)
{
    if (first || sinceLastMs > WALK_GAP_MS)
    {
        anchor = at;
        return stalledMs >= WALK_STALL_MS;
    }
    float const dx = at[0] - anchor[0];
    float const dy = at[1] - anchor[1];
    float const dz = at[2] - anchor[2];
    if (dx * dx + dy * dy + dz * dz >= WALK_MOVE_YARDS * WALK_MOVE_YARDS)
    {
        anchor = at;
        stalledMs = 0;
        return false;
    }
    stalledMs += sinceLastMs;
    return stalledMs >= WALK_STALL_MS;
}
}  // namespace Gather

#endif
