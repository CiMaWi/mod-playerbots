/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the gather rules (src/Ai/World/Rpg/GatherRules.h, professions plan WR13a): hand cases for the
// pool share, a node's worth, the pass-by pick, the skinning rule and a circuit, then a seeded search over random
// fields for the stated invariants: no spawn is revisited before its respawn; a lap shorter than its longest respawn
// is walked once; the loop is no longer than nearest-first; no node above the skill; a farther or more dangerous node
// is never preferred; a worthless hide is not skinned; a pool's shares stay within its limit; the answers are
// deterministic and do not depend on the order of the input; what the bot sees and remembers (WR13d): no spawn point
// seen empty or taken within its respawn is on the loop, one it does not see keeps its pool share. Negative controls
// (a random order, laps always repeated, a pick by id, skinning whatever the skill allows, a plan blind to what is
// seen, the up and down sets swapped) must be caught by the same checkers.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "GatherMemoryState.h"
#include "GatherRules.h"

using Gather::Circuit;
using Gather::Gatherer;
using Gather::Hide;
using Gather::Node;
using Gather::PoolLevel;
using Gather::Seen;
using Gather::Yield;
using Planner::Circuit::Point;

namespace
{
constexpr std::uint32_t SEED = 20261120;
constexpr int WORLDS = 600;
constexpr std::uint16_t HERBALISM = 182;
constexpr std::uint16_t MINING = 186;
constexpr std::uint16_t SKINNING = 393;
constexpr float FIELD_YARDS = 2600.0f;
constexpr float EPS_YARDS = 0.05f;

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

bool Near(double a, double b) { return std::abs(a - b) <= 1e-5 * std::max(1.0, std::abs(b)); }

Gatherer BotWith(std::uint32_t herb, std::uint32_t mine, std::uint32_t skin)
{
    Gatherer bot;
    bot.skills = {{HERBALISM, herb, 300}, {MINING, mine, 300}, {SKINNING, skin, 300}};
    bot.rates.exchange = 1.0f;
    bot.rates.factor = 1.0f;
    bot.rates.minuteValue = 20.0f;
    bot.rates.cautionWeight = 1.0f;
    return bot;
}

Node NodeAt(std::uint32_t id, std::uint16_t skill, std::uint32_t required, float x, float y, std::uint32_t respawn,
            float share, std::uint32_t vendorCopper)
{
    Node node;
    node.id = id;
    node.skill = skill;
    node.required = required;
    node.at = {x, y, 0.0f};
    node.respawnSecs = respawn;
    node.share = share;
    node.yields = {{1000 + id, 2.0, 0, vendorCopper, 0}};
    return node;
}

// The nearest-first loop over the same stops, closed at the anchor
float NearestFirstYards(Point const& anchor, std::vector<Point> const& points)
{
    std::vector<bool> used(points.size(), false);
    Point at = anchor;
    float yards = 0.0f;
    for (std::size_t step = 0; step < points.size(); ++step)
    {
        std::size_t best = points.size();
        float bestYards = 0.0f;
        for (std::size_t i = 0; i < points.size(); ++i)
            if (!used[i] && (best == points.size() || Planner::Circuit::Yards(at, points[i]) < bestYards))
            {
                best = i;
                bestYards = Planner::Circuit::Yards(at, points[i]);
            }
        used[best] = true;
        yards += bestYards;
        at = points[best];
    }
    return yards + Planner::Circuit::Yards(at, anchor);
}

Node const* FindNode(std::vector<Node> const& nodes, std::uint32_t id)
{
    for (Node const& node : nodes)
        if (node.id == id)
            return &node;
    return nullptr;
}

std::vector<Point> PointsOf(std::vector<Node> const& nodes, std::vector<std::uint32_t> const& stops)
{
    std::vector<Point> points;
    for (std::uint32_t const id : stops)
        points.push_back(FindNode(nodes, id)->at);
    return points;
}

struct Violations
{
    int revisit = 0;
    int shortLapRepeated = 0;
    int longerThanNearest = 0;
    int aboveSkill = 0;
    int fartherPreferred = 0;
    int riskierPreferred = 0;
    int worthlessSkinned = 0;
    int poolOverLimit = 0;
    int nondeterministic = 0;
    int seenDownOnLoop = 0;  // a stop of the loop in the down set (seen empty, or taken within its respawn)
    int shareNotKept = 0;    // a node seen up not at share 1, or one neither seen nor remembered off its pool share
};

// The sight's two rules over the nodes before and after ApplySight, and the loop against the down set
void CheckSight(std::vector<Node> const& before, std::vector<Node> const& after, std::vector<std::uint32_t> const& up,
                std::vector<std::uint32_t> const& down, Circuit const& circuit, Violations& v)
{
    for (std::uint32_t const id : circuit.stops)
        if (std::binary_search(down.begin(), down.end(), id) && !std::binary_search(up.begin(), up.end(), id))
            ++v.seenDownOnLoop;
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        bool const isUp = std::binary_search(up.begin(), up.end(), before[i].id);
        bool const isDown = std::binary_search(down.begin(), down.end(), before[i].id);
        if ((isUp && after[i].share != 1.0f) || (!isUp && !isDown && after[i].share != before[i].share))
            ++v.shareNotKept;
    }
}

// The circuit checkers: revisits, short laps, nearest-first and skill
void CheckCircuit(Circuit const& circuit, std::vector<Node> const& nodes, Point const& anchor, Gatherer const& bot,
                  Violations& v)
{
    float const lapSeconds = circuit.lapMinutes * Planner::Goals::SECONDS_PER_MINUTE;
    for (std::uint32_t const id : circuit.stops)
    {
        Node const* node = FindNode(nodes, id);
        if (circuit.laps > 1 && lapSeconds < static_cast<float>(node->respawnSecs))
            ++v.revisit;
        Gather::SkillValue const* skill = bot.Find(node->skill);
        if (!skill || skill->value < node->required)
            ++v.aboveSkill;
    }
    if (lapSeconds < static_cast<float>(circuit.longestRespawnSecs) && circuit.laps != 1 && !circuit.stops.empty())
        ++v.shortLapRepeated;
    std::vector<Point> const points = PointsOf(nodes, circuit.stops);
    std::vector<std::uint32_t> walk(points.size());
    for (std::uint32_t i = 0; i < walk.size(); ++i)
        walk[i] = i;
    if (Planner::Circuit::PathYards(anchor, points, walk, anchor) > NearestFirstYards(anchor, points) + EPS_YARDS)
        ++v.longerThanNearest;
}

std::vector<Node> RandomField(std::mt19937& rng, Point const& anchor)
{
    std::uniform_int_distribution<int> count(0, 36);
    std::uniform_real_distribution<float> spread(-FIELD_YARDS / 2.0f, FIELD_YARDS / 2.0f);
    std::uniform_int_distribution<int> required(1, 320);
    std::uniform_int_distribution<int> respawnPick(0, 4);
    std::uniform_int_distribution<int> copper(0, 400);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uint32_t const respawns[] = {60, 120, 300, 900, 1200};
    std::vector<Node> nodes;
    int const n = count(rng);
    for (int i = 0; i < n; ++i)
    {
        Node node = NodeAt(static_cast<std::uint32_t>(10 + i * 7), unit(rng) < 0.5f ? HERBALISM : MINING,
                           static_cast<std::uint32_t>(required(rng)), anchor.x + spread(rng), anchor.y + spread(rng),
                           respawns[respawnPick(rng)], unit(rng) < 0.3f ? unit(rng) : 1.0f,
                           static_cast<std::uint32_t>(copper(rng)));
        if (unit(rng) < 0.2f)
            node.risk.levelsAbove = unit(rng) * 6.0f;
        if (unit(rng) < 0.1f)
            node.risk.eliteShare = unit(rng);
        nodes.push_back(node);
    }
    return nodes;
}

void HandCases()
{
    // Pool share: a 90 % tin vein and an equal-chanced silver vein at one spot; the spot one of 10 in a mother of 3
    Check(Near(Gather::MemberShare({1, 90.0f, 90.0f, 1}), 0.9), "explicit member: its chance");
    Check(Near(Gather::MemberShare({1, 0.0f, 90.0f, 1}), 0.1), "equal member: the rest of the roll");
    Check(Near(Gather::MemberShare({2, 0.0f, 0.0f, 5}), 0.4), "equal pool: MaxLimit / members");
    Check(Near(Gather::MemberShare({8, 0.0f, 0.0f, 5}), 1.0), "a limit above the members: always up");
    Check(Near(Gather::MemberShare({0, 0.0f, 0.0f, 5}), 0.0), "a limit of 0 spawns nothing");
    std::vector<PoolLevel> const chain = {{1, 0.0f, 90.0f, 1}, {3, 0.0f, 0.0f, 10}};
    Check(Near(Gather::PoolShare(chain), 0.03), "the share multiplies up through the mother pool");
    Check(Near(Gather::PoolShare({}), 1.0), "outside a pool: 1");

    // A node's worth: the sale by need, the path's price above the vendor's, other buyers behind the switch
    Gatherer bot = BotWith(100, 100, 100);
    Node node = NodeAt(1, HERBALISM, 50, 100.0f, 0.0f, 300, 1.0f, 50);
    double const skillPoint = 0.25 * bot.rates.minuteValue * Planner::Goals::SKILLUP_MINUTES;  // green: 50 + 50
    Check(Near(Gather::NodeWorth(node, bot), 100.0 + skillPoint), "two units at 50 c plus a green skill point");
    node.yields[0].pathCopper = 80;
    node.yields[0].otherCopper = 200;
    Check(Near(Gather::NodeWorth(node, bot, false), 160.0 + skillPoint), "the path's price wins over the vendor's");
    Check(Near(Gather::NodeWorth(node, bot, true), 400.0 + skillPoint), "another buyer counts only behind the switch");
    node.required = 150;
    Check(Gather::NodeWorth(node, bot) == 0.0, "a node above the skill is worth nothing");

    // Pass by: the better net, never the farther twin, never a refused place
    Node near = NodeAt(2, MINING, 50, 0.0f, 0.0f, 300, 1.0f, 100);
    Node far = near;
    far.id = 1;
    std::vector<Seen> seen = {{far, 400.0f}, {near, 50.0f}};
    std::size_t const pick = Gather::PassBy(seen, bot);
    Check(pick == 1, "pass by: the nearer of two equal nodes");
    Node deadly = near;
    deadly.id = 0;
    deadly.risk.levelsAbove = 40.0f;
    Check(Gather::PassBy({{deadly, 10.0f}}, bot) == Gather::NO_PICK, "pass by: a refused place is never taken");
    Node poor = NodeAt(3, MINING, 1, 0.0f, 0.0f, 300, 1.0f, 0);
    Check(Gather::PassBy({{poor, 2000.0f}}, BotWith(300, 300, 300)) == Gather::NO_PICK,
          "pass by: a grey node worth nothing is not worth a detour");

    // Skinning: a worthless grey hide is left, a good one taken after the fight with room
    Hide hide;
    hide.creatureLevel = 10;
    hide.yields = {{2318, 1.5, 0, 0, 0}};
    Check(!Gather::SkinPays(hide, BotWith(1, 1, 300), false, 4), "a worthless grey hide is not skinned");
    hide.yields[0].vendorCopper = 15;
    Check(Gather::SkinPays(hide, BotWith(1, 1, 300), false, 4), "a hide that sells is skinned");
    Check(!Gather::SkinPays(hide, BotWith(1, 1, 300), true, 4), "never in a fight");
    Check(!Gather::SkinPays(hide, BotWith(1, 1, 300), false, 0), "never without room");
    hide.creatureLevel = 40;
    Check(!Gather::SkinPays(hide, BotWith(1, 1, 100), false, 4), "never above the skill");

    // A circuit: a small square is walked once, a large ring at a fast pace laps
    Point const anchor{0.0f, 0.0f, 0.0f};
    std::vector<Node> square;
    for (std::uint32_t i = 0; i < 4; ++i)
        square.push_back(
            NodeAt(20 + i, HERBALISM, 50, (i & 1) ? 100.0f : -100.0f, (i & 2) ? 100.0f : -100.0f, 900, 1.0f, 60));
    Circuit const small = Gather::BuildCircuit(anchor, square, bot);
    Check(small.stops.size() == 4 && small.laps == 1 && small.once, "a small square: four stops, walked once");
    Check(small.lapYards < 1000.0f, "a small square: a short loop");
    std::vector<Node> ring;
    for (std::uint32_t i = 0; i < 16; ++i)
    {
        float const angle = static_cast<float>(i) * 0.3927f;
        ring.push_back(NodeAt(40 + i, MINING, 50, 300.0f * std::cos(angle), 300.0f * std::sin(angle), 120, 1.0f, 300));
    }
    Circuit const lap = Gather::BuildCircuit(anchor, ring, bot);
    Check(lap.stops.size() == 16 && lap.laps > 1 && !lap.once, "a wide ring of 2-minute nodes laps");
    Check(lap.longestRespawnSecs == 120, "the ring's longest respawn");
    Gatherer low = BotWith(10, 10, 10);
    Check(Gather::BuildCircuit(anchor, ring, low).stops.empty(), "no stop above the skill");
}

// The pass-by gate (WR13c): the cause of a pass, and a pass counted once per status by its last cause
void PassCases()
{
    using Gather::PassCause;
    Gatherer const maxed = BotWith(300, 300, 300);                    // at its maximum: no skill point to gain
    Node sells = NodeAt(10, HERBALISM, 1, 0.0f, 0.0f, 300, 1.0f, 5);  // 2 x 5 copper against 5 at the bot's feet
    Gather::Verdict const near = Gather::JudgeNode({sells, 4.0f}, maxed);
    Check(near.take && near.cause == PassCause::None && near.net > 0.0, "judge: a node that pays near is taken");
    Gather::Verdict const far = Gather::JudgeNode({sells, 2000.0f}, maxed);
    Check(!far.take && far.cause == PassCause::DetourTooLong, "judge: the same node far off is a detour too long");
    Node poor = NodeAt(11, HERBALISM, 1, 0.0f, 0.0f, 300, 1.0f, 0);
    Check(Gather::JudgeNode({poor, 4.0f}, maxed).cause == PassCause::NotWorthIt,
          "judge: a node worth nothing is not worth it, however near");
    Node deadly = sells;
    deadly.risk.levelsAbove = 15.0f;
    Check(Gather::JudgeNode({deadly, 4.0f}, maxed).cause == PassCause::TooDangerous,
          "judge: a node among creatures far above is too dangerous");
    Node high = NodeAt(12, MINING, 200, 0.0f, 0.0f, 300, 1.0f, 50);
    Gather::Verdict const above = Gather::JudgeNode({high, 4.0f}, BotWith(1, 1, 1));
    Check(!above.take && above.cause == PassCause::None, "judge: a node above the skill is no pass");
    Check(Gather::JudgeNode({poor, 4.0f}, BotWith(1, 1, 1)).take, "judge: an orange node pays for its skill point");
    for (float const detour : {0.0f, 50.0f, 400.0f, 3000.0f})
    {
        Gather::Verdict const verdict = Gather::JudgeNode({sells, detour}, maxed);
        Check(verdict.take == (Gather::PassBy({{sells, detour}}, maxed) != Gather::NO_PICK),
              "judge: takes what the pass-by pick takes at " + std::to_string(detour) + " yd");
    }

    Hide hide;
    hide.creatureLevel = 10;
    hide.yields = {{2318, 1.5, 0, 15, 0}};
    Check(Gather::JudgeHide(hide, maxed, false, 4).take, "judge: a hide that sells is skinned");
    Gather::Verdict const fight = Gather::JudgeHide(hide, maxed, true, 4);
    Check(!fight.take && fight.cause == PassCause::None, "judge: no hide in a fight, and no pass (judged after)");
    Check(Gather::JudgeHide(hide, maxed, false, 0).cause == PassCause::None, "judge: no room is no pass");
    hide.yields[0].vendorCopper = 0;
    Check(Gather::JudgeHide(hide, maxed, false, 4).cause == PassCause::NotWorthIt,
          "judge: a worthless hide is not worth it");
    hide.risk.levelsAbove = 15.0f;
    Check(Gather::JudgeHide(hide, maxed, false, 4).cause == PassCause::TooDangerous,
          "judge: a hide among creatures far above is too dangerous");

    auto const total = [](std::array<std::uint32_t, Gather::PASS_CAUSES> const& passes)
    {
        std::uint32_t sum = 0;
        for (std::uint32_t const count : passes)
            sum += count;
        return sum;
    };
    Gather::Verdict const pass{false, PassCause::NotWorthIt, -1.0};
    Gather::Verdict const pays{true, PassCause::None, 3.0};
    Gather::Passes passes;
    for (int look = 0; look < 5; ++look)
        passes.Judge(1, pass, 10.0f);
    passes.Judge(2, {false, PassCause::DetourTooLong, -1.0}, 90.0f);
    passes.Judge(2, {false, PassCause::TooDangerous, -1.0}, 90.0f);
    passes.Judge(3, pass, 30.0f);
    passes.Judge(3, pays, 12.0f);
    Gather::PassEntry const* taken = passes.Take(3);
    Check(taken && taken->net == 3.0 && taken->detourYards == 12.0f, "ring: the take reads its last judgment");
    Check(!passes.Take(3) && !passes.Take(1) && !passes.Take(99), "ring: a take once, only of a target let through");
    passes.Judge(3, pass, 40.0f);
    std::array<std::uint32_t, Gather::PASS_CAUSES> const first = passes.Flush();
    Check(total(first) == 2 && first[static_cast<std::size_t>(PassCause::NotWorthIt)] == 1 &&
              first[static_cast<std::size_t>(PassCause::TooDangerous)] == 1,
          "ring: a pass judged often counts once, by its last cause; a take is no pass");
    Check(total(passes.Flush()) == 0, "ring: a flush starts anew");
    for (std::uint64_t target = 1; target <= Gather::PASS_RING + 5; ++target)
        passes.Judge(target, pass, 10.0f);
    passes.Judge(Gather::PASS_RING + 5, pays, 10.0f);
    Check(total(passes.Flush()) == Gather::PASS_RING + 4, "ring: targets that leave the full ring are counted then");
    Gather::Passes none;
    none.Judge(7, pays, 5.0f);
    none.Judge(8, {}, 5.0f);
    Check(total(none.Flush()) == 0, "ring: what is let through or no pass counts nothing");
}

void Search()
{
    std::mt19937 rng(SEED);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_int_distribution<int> skillPick(0, 320);
    Violations v;
    Violations control;
    int circuits = 0;
    int onceCircuits = 0;
    int lapCircuits = 0;
    int picks = 0;
    int noPicks = 0;
    int skins = 0;
    int leaves = 0;
    int pools = 0;
    int sighted = 0;
    for (int world = 0; world < WORLDS; ++world)
    {
        Point const anchor{unit(rng) * 500.0f, unit(rng) * 500.0f, 0.0f};
        Gatherer bot = BotWith(static_cast<std::uint32_t>(skillPick(rng)), static_cast<std::uint32_t>(skillPick(rng)),
                               static_cast<std::uint32_t>(skillPick(rng)));
        bot.rates.runSpeed = unit(rng) < 0.5f ? 7.0f : 14.0f;
        bot.rates.factor = 0.5f + unit(rng) * 2.0f;
        std::vector<Node> const nodes = RandomField(rng, anchor);

        // The circuit: invariants, determinism, independence of the input order
        Circuit const circuit = Gather::BuildCircuit(anchor, nodes, bot);
        CheckCircuit(circuit, nodes, anchor, bot, v);
        std::vector<Node> shuffled = nodes;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        Circuit const again = Gather::BuildCircuit(anchor, shuffled, bot);
        if (again.stops != circuit.stops || again.laps != circuit.laps || !Near(again.lapYards, circuit.lapYards))
            ++v.nondeterministic;
        if (circuit.stops.size() >= 2)
        {
            ++circuits;
            circuit.once ? ++onceCircuits : ++lapCircuits;
            // Controls: the same stops in a random order, and laps repeated whatever the respawn
            Circuit scrambled = circuit;
            std::shuffle(scrambled.stops.begin(), scrambled.stops.end(), rng);
            Circuit always = circuit;
            always.laps = std::max<std::uint32_t>(2, circuit.laps);
            CheckCircuit(scrambled, nodes, anchor, bot, control);
            CheckCircuit(always, nodes, anchor, bot, control);
        }

        // What the bot sees and remembers (WR13d): a random part of the nodes seen up, another seen empty or taken
        std::vector<std::uint32_t> up;
        std::vector<std::uint32_t> down;
        for (Node const& node : nodes)
        {
            float const roll = unit(rng);
            if (roll < 0.25f)
                up.push_back(node.id);
            else if (roll < 0.6f)
                down.push_back(node.id);
        }
        if (!down.empty() && unit(rng) < 0.3f)
            up.push_back(down.front());  // a point in both: seen up now
        std::sort(up.begin(), up.end());
        std::sort(down.begin(), down.end());
        std::vector<Node> seenNodes = nodes;
        Gather::ApplySight(seenNodes, up, down);
        Circuit const seenCircuit = Gather::BuildCircuit(anchor, seenNodes, bot);
        CheckCircuit(seenCircuit, seenNodes, anchor, bot, v);
        CheckSight(nodes, seenNodes, up, down, seenCircuit, v);
        sighted += seenCircuit.stops.empty() ? 0 : 1;
        // Controls: the plan blind to what is seen, and the two sets swapped
        CheckSight(nodes, nodes, up, down, circuit, control);
        std::vector<Node> swapped = nodes;
        Gather::ApplySight(swapped, down, up);
        CheckSight(nodes, swapped, up, down, Gather::BuildCircuit(anchor, swapped, bot), control);

        // Pass by: the pick against its farther and its more dangerous twin (a lower id, so ties would favour it)
        std::vector<Seen> seen;
        for (Node const& node : nodes)
            if (seen.size() < 6)
                seen.push_back({node, unit(rng) * 300.0f});
        std::size_t const pick = Gather::PassBy(seen, bot);
        if (pick == Gather::NO_PICK)
            ++noPicks;
        else
        {
            ++picks;
            Gather::SkillValue const* skill = bot.Find(seen[pick].node.skill);
            if (!skill || skill->value < seen[pick].node.required)
                ++v.aboveSkill;
            Seen farther = seen[pick];
            farther.node.id = 0;
            farther.detourYards += 1.0f + unit(rng) * 200.0f;
            Seen riskier = seen[pick];
            riskier.node.id = 0;
            riskier.node.risk.levelsAbove += 0.5f + unit(rng) * 3.0f;
            for (int twin = 0; twin < 2; ++twin)
            {
                std::vector<Seen> withTwin = seen;
                withTwin.push_back(twin ? riskier : farther);
                std::size_t const again = Gather::PassBy(withTwin, bot);
                if (again == withTwin.size() - 1)
                    ++(twin ? v.riskierPreferred : v.fartherPreferred);
                // Control: a pick by the lowest id among positive nets takes the twin
                std::size_t byId = Gather::NO_PICK;
                for (std::size_t i = 0; i < withTwin.size(); ++i)
                    if (Gather::Net(withTwin[i], bot) > 0.0 &&
                        (byId == Gather::NO_PICK || withTwin[i].node.id < withTwin[byId].node.id))
                        byId = i;
                if (byId == withTwin.size() - 1)
                    ++(twin ? control.riskierPreferred : control.fartherPreferred);
            }
        }

        // Skinning: a worthless hide (no price, a grey level for the bot) is never skinned
        Hide hide;
        hide.creatureLevel = static_cast<std::uint32_t>(1 + skillPick(rng) % 80);
        std::uint32_t const price = unit(rng) < 0.4f ? 0u : static_cast<std::uint32_t>(unit(rng) * 500.0f);
        hide.yields = {{4304, 1.0 + unit(rng), 0, price, 0}};
        bool const skin = Gather::SkinPays(hide, bot, unit(rng) < 0.1f, unit(rng) < 0.1f ? 0u : 3u);
        skin ? ++skins : ++leaves;
        Gather::SkillValue const* skinning = bot.Find(SKINNING);
        std::uint32_t const skinValue = skinning ? skinning->value : 0;
        std::uint32_t const skinNeed = SkillPath::SkinRequirement(hide.creatureLevel);
        bool const grey = SkillPath::GatherChancePerMille(skinValue, skinNeed) == SkillPath::CHANCE_GREY;
        bool const worthless = price == 0 && grey;
        if (skin && worthless)
            ++v.worthlessSkinned;
        if (skin && (!skinning || skinValue < skinNeed))
            ++v.aboveSkill;
        // Control: skinning whatever the skill allows takes worthless hides
        if (worthless && skinning && skinValue >= skinNeed)
            ++control.worthlessSkinned;

        // Pool shares: within [0, 1], their sum within the limit, an added equal member never raises a share
        std::uint32_t const limit = static_cast<std::uint32_t>(skillPick(rng) % 6);
        std::uint32_t const equal = static_cast<std::uint32_t>(skillPick(rng) % 8);
        std::vector<float> chances;
        float explicitSum = 0.0f;
        for (int i = skillPick(rng) % 3; i > 0; --i)
        {
            float const chance = unit(rng) * (100.0f - explicitSum);
            chances.push_back(chance);
            explicitSum += chance;
        }
        double sum = 0.0;
        for (float const chance : chances)
            sum += Gather::MemberShare({limit, chance, explicitSum, equal});
        float const equalShare = Gather::MemberShare({limit, 0.0f, explicitSum, equal});
        sum += equalShare * equal;
        if (sum > std::max(1.0, static_cast<double>(limit)) + 1e-5 || equalShare < 0.0f || equalShare > 1.0f ||
            (equal && Gather::MemberShare({limit, 0.0f, explicitSum, equal + 1}) > equalShare + 1e-6f))
            ++v.poolOverLimit;
        ++pools;
    }

    std::cout << "search: " << WORLDS << " worlds (seed " << SEED << "), " << circuits << " circuits of 2+ stops ("
              << onceCircuits << " walked once, " << lapCircuits << " lapped), pass-by " << picks << " picks / "
              << noPicks << " none, skinning " << skins << " skinned / " << leaves << " left, " << pools << " pools, "
              << sighted << " sighted circuits\n";
    Check(circuits > 0 && onceCircuits > 0 && lapCircuits > 0, "the search builds circuits walked once and lapped");
    Check(picks > 0 && noPicks > 0 && skins > 0 && leaves > 0, "the search picks and refuses, skins and leaves");
    Check(v.revisit == 0, "no spawn revisited before its respawn: " + std::to_string(v.revisit));
    Check(v.shortLapRepeated == 0,
          "a lap shorter than its respawn is walked once: " + std::to_string(v.shortLapRepeated));
    Check(v.longerThanNearest == 0, "no loop longer than nearest-first: " + std::to_string(v.longerThanNearest));
    Check(v.aboveSkill == 0, "no node above the skill: " + std::to_string(v.aboveSkill));
    Check(v.fartherPreferred == 0, "a farther node never preferred: " + std::to_string(v.fartherPreferred));
    Check(v.riskierPreferred == 0, "a more dangerous node never preferred: " + std::to_string(v.riskierPreferred));
    Check(v.worthlessSkinned == 0, "no worthless hide skinned: " + std::to_string(v.worthlessSkinned));
    Check(v.poolOverLimit == 0, "pool shares within their limit: " + std::to_string(v.poolOverLimit));
    Check(v.nondeterministic == 0, "deterministic and order-free: " + std::to_string(v.nondeterministic));
    Check(sighted > 0, "the search builds circuits from what the bot sees");
    Check(v.seenDownOnLoop == 0, "no stop seen empty or taken on the loop: " + std::to_string(v.seenDownOnLoop));
    Check(v.shareNotKept == 0, "seen up counts 1, unseen keeps its share: " + std::to_string(v.shareNotKept));
    std::cout << "controls: random order " << control.longerThanNearest << ", laps always " << control.revisit << "/"
              << control.shortLapRepeated << ", pick by id " << control.fartherPreferred << "/"
              << control.riskierPreferred << ", skin by skill " << control.worthlessSkinned << ", blind plan "
              << control.seenDownOnLoop << ", sets swapped " << control.shareNotKept << '\n';
    Check(control.longerThanNearest > 0, "control: a random order is caught");
    Check(control.revisit > 0 && control.shortLapRepeated > 0, "control: laps always repeated are caught");
    Check(control.fartherPreferred > 0 && control.riskierPreferred > 0, "control: a pick by id is caught");
    Check(control.worthlessSkinned > 0, "control: skinning by skill alone is caught");
    Check(control.seenDownOnLoop > 0, "control: a plan blind to what is seen is caught");
    Check(control.shareNotKept > 0, "control: the up and down sets swapped are caught");
}
}  // namespace

// The diagnosis words of a stop found down (WR13d): each fact alone gives its word, and the precedence holds
void DownCases()
{
    using Gather::DownFacts;
    auto cause = [](DownFacts const& facts) { return std::string(Gather::DownCause(facts)); };
    Check(cause({.ownTake = true, .otherTake = true, .pooled = true, .poolUp = true}) == "down_own_take",
          "down: the bot's own take comes first");
    Check(cause({.otherTake = true, .pooled = true, .held = true}) == "down_taken_other",
          "down: another bot's take before the pool");
    Check(cause({.pooled = true, .poolUp = true}) == "down_pool_other", "down: the pool's node at another point");
    Check(cause({.pooled = true}) == "down_no_node", "down: no point of the pool up");
    Check(cause({}) == "down_no_node", "down: outside a pool, no object of the spawn");
    Check(cause({.pooled = true, .held = true, .poolUp = true}) == "down_unknown",
          "down: the pool holds its node here, taken unseen");
    Check(cause({.held = true}) == "down_unknown", "down: outside a pool, an object respawning, taken unseen");
    Check(cause({.pooled = true, .held = true, .notReady = true}) == "down_not_ready",
          "down: an object held here but not ready");
}

// The respawn memory and the sight on hand cases (WR13d)
void SightCases()
{
    Gather::Memory memory;
    memory.Note(7, 1000, 300000);
    Check(memory.Away(7, 1000 + 299999) && !memory.Away(7, 1000 + 300000), "memory: away until its respawn");
    Check(!memory.Away(8, 1000) && !memory.Away(0, 1000), "memory: a spawn not noted is not away");
    memory.Note(9, 0xFFFFFF00u, 1000);
    Check(memory.Away(9, 0x100u) && !memory.Away(9, 0x400u), "memory: the clock's wrap");
    memory.Note(7, 500000, 300000);
    Check(memory.Away(7, 700000), "memory: a new note of the spawn replaces the old");
    for (std::uint32_t spawn = 100; spawn < 100 + Gather::Memory::SIZE - 2; ++spawn)
        memory.Note(spawn, 500000, 300000);
    Check(memory.Away(7, 700000) && memory.Away(9, 0x100u), "memory: a full ring keeps its newest");
    memory.Note(500, 500000, 300000);
    Check(!memory.Away(7, 700000) && memory.Away(500, 700000), "memory: one more overwrites the ring's next slot");

    // Four equal pooled veins in a row from the anchor (share 0.25): the nearest seen empty, the next taken within
    // its respawn (both in the down set), the third seen up, the fourth beyond the sight keeps its share
    Gatherer const bot = BotWith(0, 100, 0);
    Point const anchor{0.0f, 0.0f, 0.0f};
    std::vector<Node> nodes;
    for (std::uint32_t i = 0; i < 4; ++i)
        nodes.push_back(NodeAt(20 + i, MINING, 1, 30.0f + 40.0f * static_cast<float>(i), 0.0f, 300, 0.25f, 200));
    std::vector<Node> seen = nodes;
    Gather::ApplySight(seen, {22}, {20, 21});
    Circuit const circuit = Gather::BuildCircuit(anchor, seen, bot);
    auto const onLoop = [&circuit](std::uint32_t id)
    { return std::find(circuit.stops.begin(), circuit.stops.end(), id) != circuit.stops.end(); };
    Check(!onLoop(20), "sight: a point seen empty is not on the loop");
    Check(!onLoop(21), "sight: a point taken within its respawn is not on the loop");
    Check(onLoop(22) && seen[2].share == 1.0f, "sight: a point seen up counts whole");
    Check(seen[3].share == 0.25f, "sight: a point beyond the sight keeps its pool share");
    Circuit const blind = Gather::BuildCircuit(anchor, nodes, bot);
    Check(circuit.worthPerLap > 0.0 && circuit.worthPerLap != blind.worthPerLap,
          "sight: the yield counts what is left");
}

// The walk's watch (WR13d-2): movement against an anchor, not the distance to the stop. Ticks of 1 s; `walk` steps
// the bot by `step` yards a tick for `ticks` ticks along x and says whether the watch fired on any of them; `fresh`
// starts a new stop's walk
void WalkCases()
{
    std::array<float, 3> anchor{};
    std::uint32_t stalled = 0;
    std::array<float, 3> at{};
    bool first = true;
    auto const fresh = [&]()
    {
        anchor = {};
        stalled = 0;
        at = {100.0f, 0.0f, 0.0f};
        first = true;
    };
    auto const walk = [&](float step, int ticks, std::uint32_t tickMs = 1000)
    {
        bool fired = false;
        for (int i = 0; i < ticks; ++i)
        {
            at[0] += step;
            fired = Gather::WalkStalled(anchor, stalled, first, tickMs, at) || fired;
            first = false;
        }
        return fired;
    };
    // A detour: the stop lies at x = 0, the bot walks away from it (x grows) for two minutes at 2 yd a tick, then back
    fresh();
    Check(!walk(2.0f, 120) && !walk(-2.0f, 120), "walk: a detour away from the stop is never given up");
    fresh();
    Check(!walk(0.0f, 30), "walk: 29 s standing after the first tick is not yet a stall");
    Check(walk(0.0f, 1), "walk: 30 s standing still is a stall (negative control: a frozen bot ends)");
    fresh();
    Check(!walk(0.0f, 20) && !walk(0.0f, 1, 6000) && !walk(0.0f, 10) && walk(0.0f, 1),
          "walk: a 6 s pause is not counted, the clock is held, not restarted");
    fresh();
    Check(!walk(0.0f, 25) && !walk(6.0f, 1) && !walk(0.0f, 29) && walk(0.0f, 1),
          "walk: moving 5 yd from the anchor restarts the clock");
    fresh();
    Check(!walk(1.0f, 30) && walk(0.0f, 30),
          "walk: steps of 1 yd that add up to 5 yd move the anchor; standing stalls");
    fresh();
    Check(!walk(0.0f, 1) && !walk(0.0f, 10, 20000) && !walk(0.0f, 29) && walk(0.0f, 1),
          "walk: ticks far apart (a fight in the combat engine) never count toward the stall");
}

int main()
{
    HandCases();
    PassCases();
    DownCases();
    SightCases();
    WalkCases();
    Search();
    std::cout << "GATHER rules selftest: " << (passed + failed) << " checks, " << failed << " failed ("
              << (failed ? "FAIL" : "PASS") << ")\n";
    return failed ? 1 : 0;
}
