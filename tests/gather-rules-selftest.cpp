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
// seen, the up and down sets swapped) must be caught by the same checkers. Creature stops (WR20b), a second seeded
// search: no creature circuit for a bot that wants nothing from them and has gold enough; more of a wanted material
// never ranks a camp lower; a more dangerous camp never ranks higher; no stop the bot may not fight; a hide never
// counted without the skill; no stop revisited before its respawn; the same input gives the same circuit. Seven
// controls (the gate always open, a bot that thinks itself stronger, a pick by key twice, skinning at any level, laps
// always repeated, a circuit over the input's first half) must be caught. Minutes per take as bots get them (WPC13b,
// GatherRateRules.h), a third seeded search: no observation leaves every estimate the prior; the estimate stays
// between the prior and the observed rate and nears the observed as takes grow; a zone's or a way's spans never change
// another's; a span cut at once changes nothing; a span without a take raises the minutes per take; fading brings an
// estimate back toward its prior; one control each. The readers (circuit, Gathered, DropCost) by hand; the night's
// part 4 rows (tests/fixtures/gather-rate-night.tsv, args: the module root, `--night` prints every pair) fed through
// the rule after 1, 3 and 6 hours. `--decisions` prints the step's decisions.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "GatherMemoryState.h"
#include "GatherRules.h"
#include "economy-cases.h"

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
        if (Gather::Key(node) == id)
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
        if (node->kind == Gather::StopKind::Object && (!skill || skill->value < node->required))
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

    // A node's worth: the sale by need, the path's price above the vendor's, a listing's proceeds (WA7a) by need when
    // they are worth more than the path's use; a flush bot's use (its need below its exchange) outweighs a dearer sale
    Gatherer bot = BotWith(100, 100, 100);
    Node node = NodeAt(1, HERBALISM, 50, 100.0f, 0.0f, 300, 1.0f, 50);
    double const skillPoint = 0.25 * bot.rates.minuteValue * Planner::Goals::SKILLUP_MINUTES;  // green: 50 + 50
    Check(Near(Gather::NodeWorth(node, bot), 100.0 + skillPoint), "two units at 50 c plus a green skill point");
    node.yields[0].pathCopper = 80;
    Check(Near(Gather::NodeWorth(node, bot), 160.0 + skillPoint), "the path's price wins over the vendor's");
    node.yields[0].otherCopper = 200;
    Check(Near(Gather::NodeWorth(node, bot), 400.0 + skillPoint), "a listing's proceeds above the path's use count");
    Gatherer flush = bot;
    flush.rates.factor = 0.1f;
    Check(Near(Gather::NodeWorth(node, flush), 160.0 + skillPoint),
          "a flush bot keeps the path's use: 80 c at its exchange outweigh 200 c at a tenth");
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
        bool const grey = SkillPath::GatherChancePerMille(skinValue, skinNeed, SKINNING, bot.server) <= 0;
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

// Creature stops (WR20b): the rule's stated margins, by hand (Materials::FightRefused's), so the checker does not
// call the rule it checks
constexpr std::uint32_t CREATURE_SEED = 20261125;
constexpr int CREATURE_WORLDS = 800;
constexpr std::uint8_t ALLIANCE = 1;
constexpr std::uint8_t HORDE = 2;
constexpr std::uint32_t LINEN = 2589;
constexpr std::uint32_t WOOL = 2592;
constexpr std::uint32_t SILK = 4306;
constexpr std::uint32_t LIGHT_LEATHER = 2318;
constexpr std::uint32_t MEDIUM_LEATHER = 2319;
constexpr std::uint32_t HEAVY_LEATHER = 4234;
constexpr std::uint32_t BOAR_MEAT = 769;
constexpr std::uint32_t WOLF_MEAT = 2672;
constexpr std::uint32_t GREY_LOOT = 4865;
constexpr std::uint32_t COPPER_ORE = 2770;

using Gather::Camp;
using Gather::StopKind;

Gatherer FighterWith(std::uint32_t level, std::uint32_t herb, std::uint32_t skin, bool shortOfGold)
{
    Gatherer bot = BotWith(herb, 0, skin);
    bot.level = level;
    bot.side = ALLIANCE;
    bot.shortOfGold = shortOfGold;
    return bot;
}

Node CreatureAt(std::uint32_t id, float x, float y, std::uint32_t minLevel, std::uint32_t maxLevel, float xp,
                std::vector<Yield> loot, std::vector<Yield> hide = {}, double coins = 20.0)
{
    Node node;
    node.kind = StopKind::Creature;
    node.id = id;
    node.at = {x, y, 0.0f};
    node.respawnSecs = 300;
    node.yields = std::move(loot);
    node.creature = {minLevel, maxLevel, 0.0f, 0, xp, coins, std::move(hide)};
    return node;
}

std::uint32_t TopLevel(Node const& node) { return std::max(node.creature.minLevel, node.creature.maxLevel); }

bool MayFightByHand(Node const& node, Gatherer const& bot)
{
    std::uint32_t const level = std::max<std::uint32_t>(bot.level, 1);
    if (node.creature.friendlyTo & bot.side)
        return false;
    if (TopLevel(node) > level + Materials::FIGHT_LEVELS_ABOVE)
        return false;
    return !(node.creature.eliteShare >= Materials::ELITE_SHARE_REFUSED && bot.rates.groupSize <= 1 &&
             TopLevel(node) + Materials::ELITE_LEVELS_BELOW > level);
}

bool CanSkinByHand(Node const& node, Gatherer const& bot)
{
    Gather::SkillValue const* skin = bot.Find(SKINNING);
    return skin && skin->value >= SkillPath::SkinRequirement(TopLevel(node));
}

bool AnyWanted(std::vector<Yield> const& yields)
{
    return std::any_of(yields.begin(), yields.end(),
                       [](Yield const& yield) { return yield.perNode > 0.0 && yield.pathCopper > 0; });
}

// Lenient on purpose: a hide at the skill counts as wanted even where it does not pay
bool WantedByHand(Node const& node, Gatherer const& bot)
{
    return AnyWanted(node.yields) || (CanSkinByHand(node, bot) && AnyWanted(node.creature.hide));
}

struct CreatureViolations
{
    int wantsNothing = 0;     // a creature stop for a bot that wants nothing from them and has gold enough
    int mayNotFight = 0;      // a creature stop above the margin, an elite near the level alone, a friendly one
    int hideCounted = 0;      // a hide counted (worth, minutes or the option) without the skill
    int lessRanksHigher = 0;  // more of a wanted material ranked lower
    int riskierRanksHigher = 0;
    int nondeterministic = 0;
    int duplicateStop = 0;
};

void CheckCreatureCircuit(Circuit const& circuit, std::vector<Node> const& nodes, Gatherer const& bot,
                          CreatureViolations& v)
{
    bool anyCreature = false;
    bool anyWanted = false;
    for (std::uint32_t const key : circuit.stops)
    {
        Node const* node = FindNode(nodes, key);
        if (node->kind != StopKind::Creature)
            continue;
        anyCreature = true;
        anyWanted = anyWanted || WantedByHand(*node, bot);
        if (!MayFightByHand(*node, bot))
            ++v.mayNotFight;
    }
    if (anyCreature && !anyWanted && !bot.shortOfGold)
        ++v.wantsNothing;
    std::vector<std::uint32_t> keys = circuit.stops;
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end())
        ++v.duplicateStop;
}

// The worth, minutes and option by `rule` against the hide-stripped stops by `real`, for the stops `real` cannot skin
int HideCounted(Circuit const& circuit, std::vector<Node> const& nodes, Gatherer const& rule, Gatherer const& real)
{
    std::vector<Node> stripped = nodes;
    for (Node& node : stripped)
        if (node.kind == StopKind::Creature && !CanSkinByHand(node, real))
            node.creature.hide.clear();
    int counted = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].kind == StopKind::Creature && !CanSkinByHand(nodes[i], real) &&
            (!Near(Gather::NodeWorth(nodes[i], rule), Gather::NodeWorth(stripped[i], real)) ||
             !Near(Gather::StopMinutes(nodes[i], rule), Gather::StopMinutes(stripped[i], real))))
            ++counted;
    Planner::Goals::GatherNode const a = Gather::CircuitOption(circuit, nodes, rule, 0);
    Planner::Goals::GatherNode const b = Gather::CircuitOption(circuit, stripped, real, 0);
    if (!Near(a.copperWorth, b.copperWorth) || !Near(a.itemWorth, b.itemWorth) || !Near(a.skillPoints, b.skillPoints))
        ++counted;
    return counted;
}

// a below b beyond rounding; nothing is below a refused rate
bool Below(double a, double b) { return b != Gather::REFUSED && a < b - 1e-9 * std::max(1.0, std::abs(b)); }

Yield* WantedLoot(Node& node)
{
    for (Yield& yield : node.yields)
        if (yield.pathCopper > 0 && yield.perNode > 0.0)
            return &yield;
    return nullptr;
}

struct CreatureWorld
{
    Gatherer bot;
    std::vector<Camp> camps;
    std::vector<Node> nodes;  // the camps' spawns and some herbs, ids overlapping on purpose
};

CreatureWorld RandomCreatureWorld(std::mt19937& rng)
{
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_int_distribution<int> levelPick(1, 80);
    std::uniform_int_distribution<int> skillPick(1, 300);
    std::uniform_int_distribution<int> kindsPick(1, 4);
    std::uniform_int_distribution<int> spawnsPick(1, 8);
    std::uniform_int_distribution<int> offset(-8, 6);
    std::uniform_int_distribution<int> respawnPick(0, 3);
    std::uint32_t const respawns[] = {60, 120, 300, 600};
    CreatureWorld world;
    std::uint32_t const level = static_cast<std::uint32_t>(levelPick(rng));
    world.bot = FighterWith(level, unit(rng) < 0.3f ? static_cast<std::uint32_t>(skillPick(rng)) : 0u,
                            unit(rng) < 0.5f ? static_cast<std::uint32_t>(skillPick(rng)) : 0u, unit(rng) < 0.3f);
    world.bot.side = unit(rng) < 0.5f ? ALLIANCE : HORDE;
    world.bot.rates.factor = 0.5f + unit(rng) * 2.0f;
    world.bot.rates.minuteValue = 5.0f + unit(rng) * 60.0f;
    world.bot.rates.runSpeed = 7.0f;
    world.bot.rates.groupSize = unit(rng) < 0.8f ? 1 : static_cast<std::uint8_t>(2 + unit(rng) * 3.0f);
    int const kinds = kindsPick(rng);
    for (int k = 0; k < kinds; ++k)
    {
        Camp camp;
        camp.anchor = {unit(rng) * 1500.0f, unit(rng) * 1500.0f, 0.0f};
        camp.spacingYards = 20.0f + unit(rng) * 60.0f;
        std::uint32_t const minLevel =
            static_cast<std::uint32_t>(std::clamp(static_cast<int>(level) + offset(rng), 1, 83));
        std::uint32_t const maxLevel = minLevel + static_cast<std::uint32_t>(unit(rng) * 2.99f);
        std::vector<Yield> loot;
        if (unit(rng) < 0.4f)
            loot.push_back({LINEN + static_cast<std::uint32_t>(k), 0.05 + unit(rng) * 0.55,
                            10u + static_cast<std::uint32_t>(unit(rng) * 110.0f),
                            5u + static_cast<std::uint32_t>(unit(rng) * 25.0f), 0});
        if (unit(rng) < 0.7f)
            loot.push_back({GREY_LOOT + static_cast<std::uint32_t>(k), 0.2 + unit(rng) * 0.8, 0,
                            1u + static_cast<std::uint32_t>(unit(rng) * 100.0f), 0});
        std::vector<Yield> hide;
        if (unit(rng) < 0.5f)
            hide.push_back({LIGHT_LEATHER + static_cast<std::uint32_t>(k), 1.0,
                            unit(rng) < 0.3f ? 20u + static_cast<std::uint32_t>(unit(rng) * 60.0f) : 0u,
                            5u + static_cast<std::uint32_t>(unit(rng) * 55.0f), 0});
        Node kind = CreatureAt(0, 0.0f, 0.0f, minLevel, maxLevel, unit(rng) * 400.0f, loot, hide, unit(rng) * 150.0);
        if (unit(rng) < 0.15f)
            kind.creature.eliteShare = 0.3f + unit(rng) * 0.7f;
        if (unit(rng) < 0.1f)
            kind.creature.friendlyTo = static_cast<std::uint8_t>(1 + static_cast<int>(unit(rng) * 2.99f));
        kind.risk.levelsAbove = std::max(0.0f, static_cast<float>(maxLevel) - static_cast<float>(level));
        if (unit(rng) < 0.2f)
            kind.risk.deathsPerHour = unit(rng) * 0.5f;
        kind.respawnSecs = respawns[respawnPick(rng)];
        int const spawns = spawnsPick(rng);
        for (int i = 0; i < spawns; ++i)
        {
            Node spawn = kind;
            spawn.id = static_cast<std::uint32_t>(10 + i * 7 + k * 100);
            spawn.at = {camp.anchor.x + (unit(rng) - 0.5f) * 300.0f, camp.anchor.y + (unit(rng) - 0.5f) * 300.0f, 0.0f};
            world.nodes.push_back(spawn);
        }
        kind.id = static_cast<std::uint32_t>(10 + k * 100);
        camp.creature = kind;
        world.camps.push_back(camp);
    }
    std::vector<Node> herbs = RandomField(rng, world.camps.front().anchor);
    herbs.resize(std::min<std::size_t>(herbs.size(), 8));
    world.nodes.insert(world.nodes.end(), herbs.begin(), herbs.end());
    return world;
}

bool SameCircuit(Circuit const& a, Circuit const& b)
{
    return a.stops == b.stops && a.laps == b.laps && Near(a.lapYards, b.lapYards);
}

// The pick by the lowest key among camps the rule does not refuse (the twin carries the lowest key)
std::size_t PickByKey(std::vector<Camp> const& camps, Gatherer const& bot)
{
    std::size_t pick = Gather::NO_PICK;
    for (std::size_t i = 0; i < camps.size(); ++i)
        if (Gather::WeighCamp(camps[i], bot).rate != Gather::REFUSED &&
            (pick == Gather::NO_PICK || Gather::Key(camps[i].creature) < Gather::Key(camps[pick].creature)))
            pick = i;
    return pick;
}

void CreatureSearch()
{
    std::mt19937 rng(CREATURE_SEED);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    CreatureViolations v;
    CreatureViolations control;
    Violations circuitV;
    Violations circuitControl;
    int creatureCircuits = 0;
    int mixed = 0;
    int gatedRich = 0;
    int skinnedStops = 0;
    int unskilledHides = 0;
    int picks = 0;
    int refusedCamps = 0;
    int wantedTwins = 0;
    int riskyTwins = 0;
    for (int w = 0; w < CREATURE_WORLDS; ++w)
    {
        CreatureWorld const world = RandomCreatureWorld(rng);
        Gatherer const& bot = world.bot;
        std::size_t const pick = Gather::PickCamp(world.camps, bot);
        Point const anchor = world.camps[pick == Gather::NO_PICK ? 0 : pick].anchor;

        // The circuit: the gate, the fight, the hide, the respawn, determinism
        Circuit const circuit = Gather::BuildCircuit(anchor, world.nodes, bot);
        CheckCircuit(circuit, world.nodes, anchor, bot, circuitV);
        CheckCreatureCircuit(circuit, world.nodes, bot, v);
        v.hideCounted += HideCounted(circuit, world.nodes, bot, bot);
        bool hasCreature = false;
        bool hasNode = false;
        for (std::uint32_t const key : circuit.stops)
        {
            Node const* node = FindNode(world.nodes, key);
            (Gather::IsCreatureStop(key) ? hasCreature : hasNode) = true;
            skinnedStops += Gather::Skins(*node, bot) ? 1 : 0;
        }
        creatureCircuits += hasCreature ? 1 : 0;
        mixed += hasCreature && hasNode ? 1 : 0;
        for (Node const& node : world.nodes)
            unskilledHides +=
                node.kind == StopKind::Creature && !node.creature.hide.empty() && !CanSkinByHand(node, bot);
        std::vector<Node> shuffled = world.nodes;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        if (!SameCircuit(Gather::BuildCircuit(anchor, world.nodes, bot), circuit) ||
            !SameCircuit(Gather::BuildCircuit(anchor, shuffled, bot), circuit))
            ++v.nondeterministic;

        // Controls: the gate always open, a bot that thinks itself stronger, skinning at any level, laps always
        // repeated, a circuit over the input's first half
        Gatherer open = bot;
        open.shortOfGold = true;
        CreatureViolations gate;
        CheckCreatureCircuit(Gather::BuildCircuit(anchor, world.nodes, open), world.nodes, bot, gate);
        control.wantsNothing += gate.wantsNothing;
        gatedRich += gate.wantsNothing;
        Gatherer strong = bot;
        strong.level += 10;
        strong.rates.groupSize = 5;
        strong.side = 0;
        CheckCreatureCircuit(Gather::BuildCircuit(anchor, world.nodes, strong), world.nodes, bot, control);
        Gatherer skinner = bot;
        skinner.skills = {{SKINNING, 300, 300}};
        control.hideCounted += HideCounted(circuit, world.nodes, skinner, bot);
        if (hasCreature && circuit.once)
        {
            Circuit always = circuit;
            always.laps = std::max<std::uint32_t>(2, circuit.laps);
            CheckCircuit(always, world.nodes, anchor, bot, circuitControl);
        }
        std::size_t const half = world.nodes.size() / 2;
        std::vector<Node> const firstHalf(world.nodes.begin(), world.nodes.begin() + static_cast<std::ptrdiff_t>(half));
        std::vector<Node> const shuffledHalf(shuffled.begin(), shuffled.begin() + static_cast<std::ptrdiff_t>(half));
        if (!SameCircuit(Gather::BuildCircuit(anchor, firstHalf, bot), Gather::BuildCircuit(anchor, shuffledHalf, bot)))
            ++control.nondeterministic;

        // Camps: each against its twin with more of a wanted material and its more dangerous twin
        for (Camp const& camp : world.camps)
        {
            double const rate = Gather::WeighCamp(camp, bot).rate;
            refusedCamps += rate == Gather::REFUSED ? 1 : 0;
            Camp more = camp;
            if (Yield* wanted = WantedLoot(more.creature))
                wanted->perNode += 0.05 + unit(rng) * 0.5;
            else
                more.creature.yields.push_back({LINEN, 0.05 + unit(rng) * 0.5, 30, 13, 0});
            if (Below(Gather::WeighCamp(more, bot).rate, rate))
                ++v.lessRanksHigher;
            Camp riskier = camp;
            riskier.creature.risk.levelsAbove =
                std::max(riskier.creature.risk.levelsAbove, 0.0f) + 0.5f + unit(rng) * 3;
            riskier.creature.risk.deathsPerHour += unit(rng) * 0.5f;
            riskier.creature.creature.eliteShare =
                std::min(1.0f, riskier.creature.creature.eliteShare + 0.05f + unit(rng) * 0.3f);
            if (Below(rate, Gather::WeighCamp(riskier, bot).rate))
                ++v.riskierRanksHigher;
        }
        if (pick == Gather::NO_PICK)
            continue;
        ++picks;
        Camp const& picked = world.camps[pick];
        Camp less = picked;
        less.creature.id = 0;  // the lowest key: a tie would favour the twin
        if (Yield* wanted = WantedLoot(less.creature))
        {
            ++wantedTwins;
            wanted->perNode *= 0.5;
            std::vector<Camp> withTwin = world.camps;
            withTwin.push_back(less);
            if (Gather::PickCamp(withTwin, bot) == withTwin.size() - 1)
                ++v.lessRanksHigher;
            if (PickByKey(withTwin, bot) == withTwin.size() - 1)
                ++control.lessRanksHigher;
        }
        Camp riskier = picked;
        riskier.creature.id = 0;
        riskier.creature.risk.levelsAbove = std::max(riskier.creature.risk.levelsAbove, 0.0f) + 0.5f + unit(rng) * 3;
        riskier.creature.creature.eliteShare =
            std::min(1.0f, riskier.creature.creature.eliteShare + 0.05f + unit(rng) * 0.3f);
        std::vector<Camp> withTwin = world.camps;
        withTwin.push_back(riskier);
        ++riskyTwins;
        if (Gather::PickCamp(withTwin, bot) == withTwin.size() - 1)
            ++v.riskierRanksHigher;
        if (PickByKey(withTwin, bot) == withTwin.size() - 1)
            ++control.riskierRanksHigher;
    }

    std::cout << "creature search: " << CREATURE_WORLDS << " worlds (seed " << CREATURE_SEED << "), "
              << creatureCircuits << " circuits with kills (" << mixed << " with nodes too), " << skinnedStops
              << " skinned stops, " << unskilledHides << " hides without the skill, " << picks << " camps picked, "
              << refusedCamps << " camps refused, twins " << wantedTwins << " wanted / " << riskyTwins << " riskier\n";
    Check(creatureCircuits > 0 && mixed > 0 && skinnedStops > 0 && unskilledHides > 0,
          "the creature search builds circuits with kills, mixed ones and skins, and meets hides without the skill");
    Check(picks > 0 && refusedCamps > 0 && wantedTwins > 0 && gatedRich > 0,
          "the creature search picks and refuses camps and meets bots the gate holds back");
    Check(v.wantsNothing == 0,
          "no creature circuit for a bot that wants nothing and has gold enough: " + std::to_string(v.wantsNothing));
    Check(v.lessRanksHigher == 0,
          "more of a wanted material never ranks a camp lower: " + std::to_string(v.lessRanksHigher));
    Check(v.riskierRanksHigher == 0,
          "a more dangerous camp never ranks higher: " + std::to_string(v.riskierRanksHigher));
    Check(v.mayNotFight == 0, "no stop the bot may not fight: " + std::to_string(v.mayNotFight));
    Check(v.hideCounted == 0, "no hide counted without the skill: " + std::to_string(v.hideCounted));
    Check(circuitV.revisit == 0 && circuitV.shortLapRepeated == 0,
          "no stop revisited before its respawn: " + std::to_string(circuitV.revisit + circuitV.shortLapRepeated));
    Check(circuitV.aboveSkill == 0 && circuitV.longerThanNearest == 0,
          "creature circuits: no node above the skill, no loop longer than nearest-first");
    Check(v.nondeterministic == 0, "the same input gives the same circuit: " + std::to_string(v.nondeterministic));
    Check(v.duplicateStop == 0,
          "a node and a creature of one spawn id stay two stops: " + std::to_string(v.duplicateStop));
    std::cout << "creature controls: gate open " << control.wantsNothing << ", stronger bot " << control.mayNotFight
              << ", pick by key " << control.lessRanksHigher << "/" << control.riskierRanksHigher
              << ", skin at any level " << control.hideCounted << ", laps always " << circuitControl.revisit << "/"
              << circuitControl.shortLapRepeated << ", first half " << control.nondeterministic << '\n';
    Check(control.wantsNothing > 0, "control: the gate always open is caught");
    Check(control.lessRanksHigher > 0, "control: a pick by key against more of a wanted material is caught");
    Check(control.riskierRanksHigher > 0, "control: a pick by key against the safer camp is caught");
    Check(control.mayNotFight > 0, "control: a bot that thinks itself stronger is caught");
    Check(control.hideCounted > 0, "control: skinning at any level is caught");
    Check(circuitControl.revisit > 0 && circuitControl.shortLapRepeated > 0,
          "control: creature laps always repeated are caught");
    Check(control.nondeterministic > 0, "control: a circuit over the input's first half is caught");
}

// Creature stops on hand cases (WR20b)
void CreatureCases()
{
    Gatherer const tailor = FighterWith(20, 0, 0, false);
    std::vector<Yield> const cloth = {{LINEN, 0.3, 30, 13, 0}};
    std::vector<Yield> const junk = {{GREY_LOOT, 0.6, 0, 15, 0}};
    Check(Gather::MayFight(CreatureAt(1, 0, 0, 22, 23, 100, junk), tailor), "fight: three levels above is a stop");
    Check(!Gather::MayFight(CreatureAt(1, 0, 0, 23, 24, 100, junk), tailor), "fight: four levels above is no stop");
    Node elite = CreatureAt(1, 0, 0, 15, 16, 100, junk);
    elite.creature.eliteShare = 1.0f;
    Check(!Gather::MayFight(elite, tailor), "fight: an elite near the level is no stop for a bot alone");
    elite.creature.maxLevel = 15;
    Check(Gather::MayFight(elite, tailor), "fight: an elite five levels below is");
    Node friendly = CreatureAt(1, 0, 0, 20, 20, 100, cloth);
    friendly.creature.friendlyTo = ALLIANCE;
    Check(!Gather::MayFight(friendly, tailor) && Gather::NodeWorth(friendly, tailor) == 0.0,
          "fight: a creature friendly to the side is no stop and worth nothing");

    // The gate: a camp of junk for a rich bot, the same bot short of gold, the camp with cloth
    Point const anchor{0.0f, 0.0f, 0.0f};
    std::vector<Node> camp;
    for (std::uint32_t i = 0; i < 5; ++i)
        camp.push_back(CreatureAt(40 + i, 30.0f * static_cast<float>(i), 20.0f, 19, 20, 140, junk));
    auto const kills = [](Circuit const& circuit)
    { return std::count_if(circuit.stops.begin(), circuit.stops.end(), Gather::IsCreatureStop); };
    Check(kills(Gather::BuildCircuit(anchor, camp, tailor)) == 0, "gate: XP and junk alone build no circuit");
    Check(kills(Gather::BuildCircuit(anchor, camp, FighterWith(20, 0, 0, true))) > 0,
          "gate: the same camp for a bot short of gold");
    std::vector<Node> clothCamp = camp;
    clothCamp[2].yields.push_back(cloth.front());
    Circuit const clothCircuit = Gather::BuildCircuit(anchor, clothCamp, tailor);
    Check(kills(clothCircuit) >= 1, "gate: one stop that gives a wanted material opens the circuit");

    // Grey: no XP at or below the grey level (30: 22)
    Gatherer const thirty = FighterWith(30, 0, 0, false);
    Check(Gather::KillXp(CreatureAt(1, 0, 0, 22, 22, 50, junk), thirty) == 0.0 &&
              Gather::KillXp(CreatureAt(1, 0, 0, 23, 23, 50, junk), thirty) == 50.0,
          "grey: no XP at the grey level, all of it above");

    // A hide without the skill counts nothing; with it, its worth, minutes and a skill point
    Node beast = CreatureAt(60, 40.0f, 0.0f, 19, 20, 140, junk, {{LIGHT_LEATHER, 1.0, 0, 30, 0}});
    Node bare = beast;
    bare.creature.hide.clear();
    Gatherer const skinner = FighterWith(20, 0, 100, false);
    Check(Near(Gather::NodeWorth(beast, tailor), Gather::NodeWorth(bare, tailor)) &&
              Near(Gather::StopMinutes(beast, tailor), Gather::StopMinutes(bare, tailor)),
          "hide: without the skill neither worth nor minutes");
    Check(Gather::NodeWorth(beast, skinner) > Gather::NodeWorth(bare, skinner) &&
              Gather::StopMinutes(beast, skinner) > Gather::StopMinutes(bare, skinner),
          "hide: with the skill its worth and its minutes");

    // One spawn id, a herb and a creature: two stops, two keys; the memory keeps them apart
    std::vector<Node> both = {NodeAt(77, HERBALISM, 1, 50.0f, 0.0f, 300, 1.0f, 200),
                              CreatureAt(77, 0.0f, 60.0f, 19, 20, 140, cloth)};
    Gatherer const herbTailor = FighterWith(20, 100, 0, false);
    Circuit const pair = Gather::BuildCircuit(anchor, both, herbTailor);
    Check(pair.stops.size() == 2 && pair.stops[0] != pair.stops[1], "keys: a herb and a creature of one id, two stops");
    Gather::Memory memory;
    memory.Note(Gather::StopKey(true, 77), 1000, 300000);
    Check(memory.Away(Gather::StopKey(true, 77), 2000) && !memory.Away(Gather::StopKey(false, 77), 2000),
          "keys: the memory keeps a creature apart from the node of its id");

    // The option's split: coins and vendor loot as copper, the path's price as item worth, the skin's skill points,
    // the kills' XP
    Gatherer const lw = FighterWith(20, 0, 100, false);
    std::vector<Node> beasts;
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        Node node = CreatureAt(80 + i, 30.0f * static_cast<float>(i), 30.0f, 19, 20, 140, junk,
                               {{LIGHT_LEATHER, 1.0, 40, 10, 0}});
        node.respawnSecs = 600;
        beasts.push_back(node);
    }
    Circuit const beastCircuit = Gather::BuildCircuit(anchor, beasts, lw);
    Planner::Goals::GatherNode const option = Gather::CircuitOption(beastCircuit, beasts, lw, 0);
    Check(beastCircuit.stops.size() == 4 && option.copperWorth > 0.0f && option.itemWorth > 0.0f &&
              option.skillPoints > 0.0f && option.xpWorth > 0.0f && option.skill == SKINNING &&
              Gather::IsCreatureStop(option.id),
          "option: copper, item worth, skin points and XP, the skin its skill");
    // The estimate the start row carries (WR25a), counted by hand: takes are the shares' sum over the laps, units the
    // yields per take, the fight minutes the kills' fight estimates
    double fightByHand = 0.0;
    for (Node const& beast : beasts)
        fightByHand +=
            Materials::FightMinutes((beast.creature.minLevel + beast.creature.maxLevel) / 2.0, lw.level, 0.0);
    Check(Near(option.takes, 4.0) && Near(option.fightMinutes, fightByHand) && option.fightMinutes > 0.0f,
          "estimate: four kills, the fight minutes of each");
    {
        std::vector<Node> herbs = {NodeAt(90, HERBALISM, 1, 40.0f, 0.0f, 300, 1.0f, 200),
                                   NodeAt(91, HERBALISM, 1, 80.0f, 0.0f, 300, 0.5f, 200),
                                   NodeAt(92, HERBALISM, 1, 120.0f, 0.0f, 300, 1.0f, 200)};
        Gatherer const herbalist = FighterWith(20, 100, 0, false);
        Circuit const herbCircuit = Gather::BuildCircuit(anchor, herbs, herbalist);
        Planner::Goals::GatherNode const herbOption = Gather::CircuitOption(herbCircuit, herbs, herbalist, 0);
        double const laps = std::max(herbCircuit.laps, 1u);
        Check(herbCircuit.stops.size() == 3 && Near(herbOption.takes, 2.5 * laps) &&
                  Near(herbOption.units, 5.0 * laps) && herbOption.fightMinutes == 0.0f,
              "estimate: herbs of share 1, 0.5 and 1 give 2.5 takes and 2 units a take, over the laps, no fight");
        Check(herbOption.herbStops == 3 && option.herbStops == 0,
              "row's way: three herb stops of the herb circuit, none of the beasts'");
    }
    Check(beastCircuit.once && beastCircuit.laps == 1, "respawn: a short lap of 10-minute spawns is walked once");
    for (Node& node : beasts)
        node.respawnSecs = 10;
    Check(Gather::BuildCircuit(anchor, beasts, lw).laps > 1, "respawn: quick respawns lap");
}

// The step's printed decisions (`--decisions`): five bots, each beside its camps, every camp weighed, the circuit at
// each camp it may farm and the choice. Rates are the planner's own for a bot of that level and purse (NeedFor with
// the harness's reserve, level x level x 15 copper); XP per kill is the core's base gain
float BaseXp(std::uint32_t player, double mob)
{
    double const level = static_cast<double>(player);
    double const base = level * 5.0 + 45.0;
    if (mob >= level)
        return static_cast<float>((base * (20.0 + std::min(mob - level, 4.0)) / 10.0 + 1.0) / 2.0);
    if (mob <= Planner::Goals::GreyLevel(player))
        return 0.0f;
    std::uint32_t const zd = player < 8    ? 5
                             : player < 10 ? 6
                             : player < 12 ? 7
                             : player < 16 ? 8
                             : player < 20 ? 9
                             : player < 30 ? 11
                             : player < 40 ? 12
                             : player < 45 ? 13
                             : player < 50 ? 14
                             : player < 55 ? 15
                             : player < 60 ? 16
                                           : 17;
    return static_cast<float>(base * (zd + mob - level) / zd);
}

struct Scenario
{
    std::string title;
    Gatherer bot;
    std::vector<std::string> campNames;
    std::vector<Camp> camps;
    std::vector<Node> stops;  // every stop near: the camps' spawns and nodes
    Point here;
    std::string prices;  // what the path pays for each wanted material
};

Gatherer PlannerBot(std::uint32_t level, std::uint32_t money, std::vector<Gather::SkillValue> skills)
{
    Planner::Snapshot snapshot;
    snapshot.level = static_cast<std::uint8_t>(level);
    snapshot.money = money;
    Planner::Goals::Weights const weights = Planner::Goals::FromTraits(snapshot);
    std::uint64_t const reserve = static_cast<std::uint64_t>(level) * level * 15;
    Planner::Goals::Need const need = Planner::Goals::NeedFor(snapshot, weights, reserve);
    Gatherer bot;
    bot.skills = std::move(skills);
    bot.level = level;
    bot.side = ALLIANCE;
    bot.shortOfGold = need.shortOfReserve;
    bot.rates.exchange = need.exchange;
    bot.rates.factor = need.factor;
    bot.rates.minuteValue = Planner::Goals::BAND_XP_PER_MINUTE[WorldTravel::LevelBand(level)];
    bot.rates.cautionWeight = Planner::Goals::CautionWeight(weights);
    bot.rates.runSpeed = 7.0f;
    return bot;
}

// A camp of `count` spawns in a ring round its anchor, the spawns the camp's stops
void AddCamp(Scenario& s, std::string name, Point anchor, Node kind, std::uint32_t count, float spacing)
{
    kind.risk.levelsAbove = std::max(0.0f, static_cast<float>(TopLevel(kind)) - static_cast<float>(s.bot.level));
    kind.risk.eliteShare = kind.creature.eliteShare;
    kind.creature.xpPerKill = BaseXp(s.bot.level, (kind.creature.minLevel + kind.creature.maxLevel) / 2.0);
    std::uint32_t const base = 1000 * static_cast<std::uint32_t>(s.camps.size() + 1);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        float const angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        Node spawn = kind;
        spawn.id = base + i;
        spawn.at = {anchor.x + spacing * 1.2f * std::cos(angle), anchor.y + spacing * 1.2f * std::sin(angle), 0.0f};
        s.stops.push_back(spawn);
    }
    kind.id = base;
    s.campNames.push_back(std::move(name));
    s.camps.push_back({anchor, kind, spacing});
}

// What the bot's path pays for a wanted material, as WR20a-2 prices it: the cheapest drop row of the scenario's camps
// (Materials::DropCost, the shared fight estimate) in neutral copper; set on every yield of the item
void PricePath(Scenario& s, std::vector<std::uint32_t> const& wanted)
{
    Materials::Bot priced;
    priced.rates = s.bot.rates;
    priced.level = s.bot.level;
    priced.side = s.bot.side;
    for (Gather::SkillValue const& skill : s.bot.skills)
        priced.skills[skill.skill] = skill.value;
    for (std::uint32_t const item : wanted)
    {
        double best = std::numeric_limits<double>::max();
        for (Camp const& camp : s.camps)
        {
            Node const& kind = camp.creature;
            auto const offer = [&](std::vector<Yield> const& yields, Recipes::DropKind dropKind)
            {
                for (Yield const& yield : yields)
                    if (yield.item == item)
                    {
                        Recipes::DropRow row;
                        row.zone = 1;
                        row.kind = dropKind;
                        row.entry = kind.id;
                        row.item = item;
                        row.spawns = 8;
                        row.spacing = camp.spacingYards;
                        row.minLevel = kind.creature.minLevel;
                        row.maxLevel = kind.creature.maxLevel;
                        row.eliteShare = kind.creature.eliteShare;
                        row.friendlyTo = kind.creature.friendlyTo;
                        row.skill = dropKind == Recipes::DropKind::Skin ? SKINNING : 0;
                        row.perKill = yield.perNode;
                        row.respawnSecs = kind.respawnSecs;
                        priced.anyZone = kind.risk;
                        Materials::Cost const cost = Materials::DropCost(row, priced);
                        if (!cost.Blocked())
                            best = std::min(best, cost.value);
                    }
            };
            offer(kind.yields, Recipes::DropKind::Corpse);
            offer(kind.creature.hide, Recipes::DropKind::Skin);
        }
        std::uint32_t const copper = best < std::numeric_limits<double>::max()
                                         ? static_cast<std::uint32_t>(std::ceil(best / s.bot.rates.exchange))
                                         : 0u;
        s.prices += (s.prices.empty() ? "" : ", ") + std::to_string(item) + " " + std::to_string(copper) + " c";
        auto const set = [item, copper](std::vector<Yield>& yields)
        {
            for (Yield& yield : yields)
                if (yield.item == item)
                    yield.pathCopper = copper;
        };
        for (Camp& camp : s.camps)
        {
            set(camp.creature.yields);
            set(camp.creature.creature.hide);
        }
        for (Node& node : s.stops)
        {
            set(node.yields);
            set(node.creature.hide);
        }
    }
}

void AddVeins(Scenario& s, std::uint16_t skill, std::uint32_t required, std::uint32_t item, Point at,
              std::uint32_t count)
{
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Node node = NodeAt(500 + i, skill, required, at.x + 140.0f * static_cast<float>(i), at.y, 900, 1.0f, 10);
        node.yields = {{item, 2.0, 0, 10, 0}};
        s.stops.push_back(node);
    }
}

std::string Fixed(double value, int digits = 1)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

void PrintCircuit(std::string const& where, Point const& anchor, Scenario const& s)
{
    Circuit const circuit = Gather::BuildCircuit(anchor, s.stops, s.bot);
    Planner::Goals::GatherNode const option = Gather::CircuitOption(circuit, s.stops, s.bot, 0);
    std::size_t const kills =
        static_cast<std::size_t>(std::count_if(circuit.stops.begin(), circuit.stops.end(), Gather::IsCreatureStop));
    std::cout << "- circuit " << where << ": ";
    if (circuit.stops.empty())
    {
        std::cout << "none (no stop pays)\n";
        return;
    }
    double const money = option.copperWorth * s.bot.rates.factor;
    double const rest = option.xpWorth + option.itemWorth * s.bot.rates.exchange +
                        Planner::Goals::BAND_XP_PER_MINUTE[WorldTravel::LevelBand(s.bot.level)] *
                            Planner::Goals::SKILLUP_MINUTES * option.skillPoints;
    std::cout << circuit.stops.size() << " stops (" << kills << " kills, " << circuit.stops.size() - kills
              << " nodes), lap " << Fixed(circuit.lapYards, 0) << " yd / " << Fixed(circuit.lapMinutes) << " min, "
              << (circuit.once ? std::string("walked once")
                               : std::to_string(circuit.laps) + (circuit.laps == 1 ? " lap" : " laps"))
              << "; option: " << Fixed(option.gatherMinutes) << " min, copper " << Fixed(option.copperWorth, 0)
              << ", item worth " << Fixed(option.itemWorth, 0) << " c, skill points " << Fixed(option.skillPoints, 2)
              << ", XP " << Fixed(option.xpWorth, 0) << "; to the planner a "
              << (s.bot.shortOfGold && money > 0.0 && money >= rest ? "money goal (gold_short)" : "gather goal")
              << " (money " << Fixed(money, 0) << " vs the rest " << Fixed(rest, 0) << " value units)\n";
}

void PrintScenario(Scenario const& s)
{
    std::cout << "\n## " << s.title << "\n\n";
    std::cout << "Level " << s.bot.level << ", " << (s.bot.shortOfGold ? "short of gold" : "gold enough")
              << "; a minute is worth " << Fixed(s.bot.rates.minuteValue, 0) << ", a copper "
              << Fixed(s.bot.rates.factor, 2) << " (value units, the planner's XP scale). Skills:";
    for (Gather::SkillValue const& skill : s.bot.skills)
        std::cout << ' ' << skill.skill << '=' << skill.value;
    std::cout << ". The path pays: " << (s.prices.empty() ? std::string("nothing") : s.prices);
    std::cout << "\n\n| camp | levels | verdict | kill worth: loot / hide / XP | min per kill | minute cost | rate |\n"
              << "|---|---|---|---|---|---|---|\n";
    for (std::size_t i = 0; i < s.camps.size(); ++i)
    {
        Gather::CampWeight const weight = Gather::WeighCamp(s.camps[i], s.bot);
        Node const& kind = s.camps[i].creature;
        std::cout << "| " << s.campNames[i] << " | " << kind.creature.minLevel << '-' << kind.creature.maxLevel
                  << (kind.creature.eliteShare > 0.0f ? " elite" : "") << " | "
                  << (weight.refusal.empty() ? std::string(weight.skinned ? "weighed, skinned" : "weighed")
                                             : std::string(weight.refusal))
                  << " | ";
        if (weight.refusal.empty())
            std::cout << Fixed(weight.worth.loot) << " / " << Fixed(weight.worth.hide) << " / "
                      << Fixed(weight.worth.xp) << " | " << Fixed(weight.minutes, 2) << " | "
                      << Fixed(weight.minuteCost) << " | " << Fixed(weight.rate) << " |\n";
        else
            std::cout << "- | - | - | - |\n";
    }
    std::cout << '\n';
    PrintCircuit("from where it stands", s.here, s);
    for (std::size_t i = 0; i < s.camps.size(); ++i)
        if (Gather::WeighCamp(s.camps[i], s.bot).refusal.empty())
            PrintCircuit("at the " + s.campNames[i], s.camps[i].anchor, s);
    std::size_t const pick = Gather::PickCamp(s.camps, s.bot);
    std::cout << "- choice: "
              << (pick == Gather::NO_PICK ? std::string("no camp; the planner gets the circuit from where it stands")
                                          : "the " + s.campNames[pick] + "; the planner weighs its circuit")
              << "\n";
}

void PrintDecisions()
{
    std::cout << "# WR20b printed decisions (gather-rules-selftest --decisions)\n\n"
              << "Camps are weighed by what a kill earns a minute above the bot's minute cost (rate, value units a "
                 "minute; below 0 means under the bot's usual pace, which the planner then weighs against its other "
                 "goals). Refusals: no_fight (more than 3 levels above, an elite near the level alone, friendly), "
                 "wants_nothing (no material the bot's path wants and gold enough), place_refused. A wanted material "
                 "is priced as the path prices it (WR20a-2: the cheapest camp's kill cost per unit, in neutral "
                 "copper) and counts at the bot's exchange; sold loot and coins count by its need for copper. The "
                 "circuit at a camp is what the planner would be offered; nodes join where they pay.\n";
    std::vector<Yield> const junk = {{GREY_LOOT, 0.6, 0, 15, 0}};
    Point const here{0.0f, 0.0f, 0.0f};

    Scenario tailor{
        "A level-20 tailor beside a camp of humanoids", PlannerBot(20, 30000, {{197, 110, 150}}), {}, {}, {}, here, {}};
    AddCamp(tailor, "humanoid camp", {120.0f, 80.0f, 0.0f},
            CreatureAt(0, 0, 0, 18, 19, 0, {{LINEN, 0.3, 0, 13, 0}, {WOOL, 0.12, 0, 33, 0}, junk.front()}, {}, 25.0), 8,
            35.0f);
    AddCamp(tailor, "wolf den", {-300.0f, 250.0f, 0.0f},
            CreatureAt(0, 0, 0, 19, 20, 0, {{WOLF_MEAT, 0.4, 0, 6, 0}}, {{LIGHT_LEATHER, 1.0, 0, 15, 0}}, 0.0), 6,
            40.0f);
    AddCamp(tailor, "ogre camp", {600.0f, -200.0f, 0.0f},
            CreatureAt(0, 0, 0, 23, 24, 0, {{WOOL, 0.3, 0, 33, 0}}, {}, 60.0), 6, 30.0f);
    Node elites = CreatureAt(0, 0, 0, 20, 21, 0, {{LINEN, 0.3, 0, 13, 0}}, {}, 80.0);
    elites.creature.eliteShare = 1.0f;
    AddCamp(tailor, "elite keep", {-700.0f, -500.0f, 0.0f}, elites, 5, 25.0f);
    PricePath(tailor, {LINEN, WOOL});
    PrintScenario(tailor);

    Scenario lw{"A level-35 leatherworker with skinning",
                PlannerBot(35, 300000, {{SKINNING, 180, 225}, {165, 175, 225}}),
                {},
                {},
                {},
                here,
                {}};
    AddCamp(lw, "beast range", {150.0f, -100.0f, 0.0f},
            CreatureAt(0, 0, 0, 33, 34, 0, junk, {{HEAVY_LEATHER, 1.0, 0, 15, 0}}, 0.0), 8, 45.0f);
    AddCamp(lw, "humanoid camp", {-250.0f, 200.0f, 0.0f},
            CreatureAt(0, 0, 0, 34, 35, 0, {{SILK, 0.3, 0, 38, 0}, junk.front()}, {}, 80.0), 8, 30.0f);
    AddCamp(lw, "beasts above the skill", {450.0f, 300.0f, 0.0f},
            CreatureAt(0, 0, 0, 36, 37, 0, junk, {{HEAVY_LEATHER, 1.0, 0, 15, 0}}, 0.0), 6, 45.0f);
    AddCamp(lw, "high beasts", {-600.0f, -400.0f, 0.0f},
            CreatureAt(0, 0, 0, 38, 39, 0, junk, {{HEAVY_LEATHER, 1.0, 0, 15, 0}}, 0.0), 6, 45.0f);
    PricePath(lw, {HEAVY_LEATHER});
    PrintScenario(lw);

    Scenario cook{"A cook (level 12, cooking 60)", PlannerBot(12, 5000, {{185, 60, 75}}), {}, {}, {}, here, {}};
    AddCamp(cook, "boar field", {100.0f, 100.0f, 0.0f},
            CreatureAt(0, 0, 0, 9, 10, 0, {{BOAR_MEAT, 0.5, 0, 4, 0}}, {{LIGHT_LEATHER, 1.0, 0, 15, 0}}, 5.0), 8,
            40.0f);
    AddCamp(cook, "wolf hills", {-200.0f, 150.0f, 0.0f},
            CreatureAt(0, 0, 0, 7, 8, 0, {{WOLF_MEAT, 0.4, 0, 6, 0}}, {{LIGHT_LEATHER, 1.0, 0, 15, 0}}, 3.0), 6, 45.0f);
    AddCamp(cook, "kobold mine", {300.0f, -250.0f, 0.0f},
            CreatureAt(0, 0, 0, 11, 12, 0, {{LINEN, 0.3, 0, 13, 0}, junk.front()}, {}, 12.0), 8, 25.0f);
    PricePath(cook, {BOAR_MEAT, WOLF_MEAT});
    PrintScenario(cook);

    for (bool const poor : {true, false})
    {
        Scenario skinner{poor ? "A poor skinner (level 25, skinning and mining, no leatherworking)"
                              : "A rich skinner (the same bot with 20 gold)",
                         PlannerBot(25, poor ? 1000 : 200000, {{SKINNING, 125, 150}, {MINING, 120, 150}}),
                         {},
                         {},
                         {},
                         here,
                         {}};
        AddCamp(skinner, "beast hills", {150.0f, 120.0f, 0.0f},
                CreatureAt(0, 0, 0, 23, 24, 0, junk, {{MEDIUM_LEATHER, 1.0, 0, 15, 0}}, 0.0), 8, 40.0f);
        AddCamp(skinner, "humanoid camp", {-250.0f, 150.0f, 0.0f},
                CreatureAt(0, 0, 0, 24, 25, 0, {{WOOL, 0.3, 0, 33, 0}, junk.front()}, {}, 60.0), 8, 30.0f);
        AddVeins(skinner, MINING, 65, COPPER_ORE, {-100.0f, -150.0f, 0.0f}, 5);
        PrintScenario(skinner);
    }
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
    // The stop's take (WPC13b): the cast inside the status counts, whether it went before the status targeted the
    // stop (scn-261126125954: cast 2 s ago, the status 7 s old, the stop targeted only now) or after; a cast before
    // the status and an earlier lap's are not this stop's take
    Check(Gather::OwnTakeInStatus(2000, 7000, 300000), "take: the cast before the stop was targeted counts");
    Check(Gather::OwnTakeInStatus(0, 0, 300000), "take: a cast in the status's first moment counts");
    Check(!Gather::OwnTakeInStatus(8000, 7000, 300000), "take: a cast before the status is an earlier take");
    Check(!Gather::OwnTakeInStatus(310000, 900000, 300000), "take: an earlier lap's cast, past the respawn");
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

// WPC9: the bag limit, and the stops the record of failed attempts refuses. Written first: 81 % is over, 80 % is not;
// a live place entry within 10 yd on the stop's map drops it whatever its cause, the next nearest is taken, and after
// the entry's expiry the stop returns
void RefusalCases()
{
    Check(!Gather::BagsTooFull(Gather::BagUsedPercent(100, 20)) && Gather::BagsTooFull(Gather::BagUsedPercent(100, 19)),
          "bags: 80 % passes, 81 % ends gathering");
    Check(Gather::BagUsedPercent(16, 3) == 81 && Gather::BagUsedPercent(48, 10) == 79 &&
              Gather::BagUsedPercent(0, 0) == 0 && Gather::BagUsedPercent(16, 20) == 0,
          "bags: the percent is truncated as the bag space value's");

    constexpr std::uint32_t MAP = 0;
    Gatherer const bot = BotWith(0, 100, 0);
    Point const anchor{0.0f, 0.0f, 0.0f};
    std::vector<Node> const nodes = {NodeAt(20, MINING, 1, 30.0f, 0.0f, 300, 1.0f, 200),
                                     NodeAt(21, MINING, 1, 70.0f, 0.0f, 300, 1.0f, 200)};
    auto const planned = [&](FailedAttempts::Record const& record, std::uint32_t nowMs)
    {
        std::vector<Node> offered = nodes;
        Gather::DropRefused(offered, MAP, Gather::RefusedPlaces(record, nowMs));
        return Gather::BuildCircuit(anchor, offered, bot).stops;
    };
    auto const noted = [](char const* word, std::uint32_t map, float x)
    {
        FailedAttempts::Record record;
        FailedAttempts::Failure failure;
        failure.leg = FailedAttempts::Leg::LocalPiece;  // a walk's no_walk keys by leg
        failure.goal = {map, x, 0.0f, 0.0f};
        FailedAttempts::NoteFailure(record, word, failure, 1000, 600000);
        return record;
    };
    auto const has = [](auto const& stops, std::uint32_t id)
    { return std::find(stops.begin(), stops.end(), id) != stops.end(); };
    for (char const* word : {"gather_no_walk", "no_walk"})
    {
        auto const stops = planned(noted(word, MAP, 38.0f), 2000);
        Check(!has(stops, 20) && has(stops, 21), "refused: a live entry 8 yd off drops the stop, the next is taken");
    }
    Check(has(planned(noted("gather_no_walk", MAP, 42.0f), 2000), 20), "refused: an entry 12 yd off drops nothing");
    Check(has(planned(noted("gather_no_walk", 1, 30.0f), 2000), 20), "refused: another map's entry drops nothing");
    Check(has(planned(noted("gather_no_walk", MAP, 30.0f), 1000 + 600000), 20), "refused: after expiry it returns");

    // The local piece's no_walk entry comes first: the later note leaves it and its cause
    FailedAttempts::Record record = noted("no_walk", MAP, 30.0f);
    FailedAttempts::Failure failure;
    failure.goal = {MAP, 30.0f, 0.0f, 0.0f};
    auto const second = FailedAttempts::NoteFailure(record, "gather_no_walk", failure, 1500, 600000);
    Check(second == FailedAttempts::Noted::AlreadyLive && record.count == 1 && record.entries[0].cause == "no_walk" &&
              !has(planned(record, 2000), 20),
          "refused: a live entry keeps its first cause and still drops the stop");
}

// WPC10: no creature stop without a weapon in the main hand. Written first: unarmed or fights held leaves no
// creature stop and every node stop; an armed bot that may fight keeps the list; only unarmed refusals are counted
void UnarmedCases()
{
    Node creature = NodeAt(31, MINING, 1, 40.0f, 0.0f, 300, 1.0f, 200);
    creature.kind = Gather::StopKind::Creature;
    std::vector<Node> const nodes = {NodeAt(30, MINING, 1, 30.0f, 0.0f, 300, 1.0f, 200), creature};
    for (bool const fightsHeld : {false, true})
        for (bool const unarmed : {false, true})
        {
            std::vector<Node> stops = nodes;
            std::uint32_t const refused = Gather::DropCreatureStops(stops, fightsHeld, unarmed);
            bool const dropped = fightsHeld || unarmed;
            Check(stops.size() == (dropped ? 1u : 2u) && stops[0].id == 30, "unarmed: only creature stops are dropped");
            Check(refused == (unarmed ? 1u : 0u), "unarmed: the count is the missing weapon's refusals");
        }
    std::vector<Node> only = {nodes[0]};
    Check(Gather::DropCreatureStops(only, false, true) == 0 && only.size() == 1, "unarmed: nothing to refuse, no row");
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

// The server's rates (WR29): a take's skill points are the chance times the gathering step, at most to the cap, the
// economy reference's gathering and skinning skill values met; a kill's coins are its gold range's mean times the money
// rate
void RateCases()
{
    using EconomyCases::At;
    constexpr std::uint32_t MANGY_WOLF_LEVEL = 6;  // creature 525, levels 5-6
    std::vector<EconomyCases::Case> const cases = {
        {"gather_nodes",
         {{"Configured expected points at pure gathering skill 35", "points", At::Ours, 3.0 / 2.0}},
         [](Recipes::ServerRates const& rates)
         {
             return std::map<std::string, double>{
                 {"points", Gather::ExpectedPoints({HERBALISM, 35, 75}, 0, rates)}};  // Lock 29: skill 0
         }},
        {"skinning",
         {{"Configured expected skill gain at pure skill 50", "points", At::Ours, 1.0 / 2.0}},
         [](Recipes::ServerRates const& rates)
         {
             return std::map<std::string, double>{
                 {"points",
                  Gather::ExpectedPoints({SKINNING, 50, 75}, SkillPath::SkinRequirement(MANGY_WOLF_LEVEL), rates)}};
         }},
    };
    EconomyCases::Run(cases, Check);
    Recipes::ServerRates const ours = EconomyCases::Ours();
    Check(Gather::ExpectedPoints({HERBALISM, 74, 75}, 0, ours) == 0.25 &&
              Gather::ExpectedPoints({HERBALISM, 75, 75}, 0, ours) == 0.0,
          "a skill-up's points stop at the cap");
    Gatherer bot;
    bot.rates.minuteValue = 10.0f;
    bot.skills = {{HERBALISM, 35, 75}};
    double const stockWorth = Gather::SkillPointWorth(bot.skills[0], 0, bot);
    bot.server = ours;
    Check(stockWorth > 0.0 && std::abs(Gather::SkillPointWorth(bot.skills[0], 0, bot) - 2.0 * stockWorth) < 1e-9,
          "a take's skill worth doubles with a gathering step of two");
    Check(Gather::CoinsPerKill(10, 20, 2.0f) == 30.0 && Gather::CoinsPerKill(20, 20, 1.0f) == 20.0 &&
              Gather::CoinsPerKill(30, 20, 2.0f) == 40.0 && Gather::CoinsPerKill(0, 0, 2.0f) == 0.0,
          "a kill's coins: the gold range's mean times the money rate (a single or inverted range: its top)");
}

// WA7a (PLAN 3.7): a listing's proceeds as the yields' other outlet. Over random yields, rates (a poor bot's need
// above its exchange, a flush one's below) and typical prices: without a typical price a node is worth its vendor-only
// worth; a node's worth and a hide's net never fall as an item's typical price rises (the catch's worth is the same
// YieldsWorth). Control: the outlet taken by the most copper before weighing (the flat maximum), which lets a flush
// bot's dearer sale displace its own use
void MarketSearch()
{
    std::mt19937 rng(20261126);
    auto const range = [&rng](std::uint32_t low, std::uint32_t high)
    { return std::uniform_int_distribution<std::uint32_t>(low, high)(rng); };
    auto const flat = [](std::vector<Yield> const& yields, Materials::Rates const& rates)
    {
        double worth = 0.0;
        for (Yield const& yield : yields)
        {
            std::uint32_t const each = std::max({yield.pathCopper, yield.vendorCopper, yield.otherCopper});
            bool const used = yield.pathCopper > yield.vendorCopper && each == yield.pathCopper;
            worth += std::max(yield.perNode, 0.0) * each * std::max(used ? rates.exchange : rates.factor, 0.0f);
        }
        return worth;
    };
    constexpr int WORLDS = 3000;
    std::uint32_t changed = 0;
    std::uint32_t fell = 0;
    std::uint32_t hideFell = 0;
    std::uint32_t rose = 0;
    std::uint32_t listed = 0;
    std::uint32_t caught = 0;
    for (int world = 0; world < WORLDS; ++world)
    {
        Gatherer bot = BotWith(range(1, 300), 100, range(1, 300));
        bot.rates.factor = static_cast<float>(range(1, 30)) / 10.0f;  // 0.1 (flush) to 3 (broke) x the exchange
        Node node = NodeAt(1, HERBALISM, range(1, 100), 0.0f, 0.0f, 300, 1.0f, 0);
        std::size_t const count = range(1, 3);
        node.yields.clear();
        std::vector<AuctionIndex::TypicalPrice> low(count);
        std::vector<AuctionIndex::TypicalPrice> high(count);
        for (std::size_t at = 0; at < count; ++at)
        {
            node.yields.push_back({static_cast<std::uint32_t>(1000 + at), range(1, 30) / 10.0,
                                   range(0, 2) ? 0u : range(1, 400), range(0, 200), 0});
            low[at] = {range(0, 3) ? range(1, 1500) : 0u, range(1, 10), static_cast<double>(range(1, 10)), range(0, 5)};
            high[at] = low[at];
            high[at].unitPrice += range(1, 800);
        }
        auto const priced = [&node](std::vector<AuctionIndex::TypicalPrice> const& prices)
        {
            Node copy = node;
            for (std::size_t at = 0; at < copy.yields.size(); ++at)
                copy.yields[at].otherCopper = static_cast<std::uint32_t>(
                    Market::ProceedsEach(prices[at], {}, {}, copy.yields[at].vendorCopper, Market::MAX_LOT, {}, {}));
            return copy;
        };
        Node const none = priced(std::vector<AuctionIndex::TypicalPrice>(count));
        changed += Gather::NodeWorth(none, bot) != Gather::NodeWorth(node, bot) ? 1 : 0;
        Node const a = priced(low);
        Node const b = priced(high);
        double const worthA = Gather::NodeWorth(a, bot);
        double const worthB = Gather::NodeWorth(b, bot);
        fell += worthB < worthA ? 1 : 0;
        rose += worthB > worthA ? 1 : 0;
        listed += std::any_of(b.yields.begin(), b.yields.end(), [](Yield const& yield) { return yield.otherCopper; });
        Hide const hideA{20, a.yields, {}};
        Hide const hideB{20, b.yields, {}};
        hideFell += Gather::JudgeHide(hideB, bot, false, 1).net < Gather::JudgeHide(hideA, bot, false, 1).net ? 1 : 0;
        caught += flat(b.yields, bot.rates) < flat(a.yields, bot.rates) ||
                          flat(a.yields, bot.rates) < flat(none.yields, bot.rates)
                      ? 1
                      : 0;
    }
    std::cout << "market search (seed 20261126, " << WORLDS << " nodes): " << listed << " with proceeds, " << rose
              << " worth more at a dearer price; counterexamples: " << changed << " changed without a price, " << fell
              << " node worth fell, " << hideFell << " hide net fell; control flat maximum: " << caught << " fell\n";
    Check(changed == 0 && fell == 0 && hideFell == 0 && rose > 0 && listed > 0,
          "search: proceeds never lower a node's or a hide's worth, and without a price it is the vendor-only one");
    Check(caught > 0, "control flat maximum: the most copper before weighing lets a dearer price lower a worth");
}

// ---- minutes per take as bots get them (WPC13b) ----
using GatherRate::Book;
using GatherRate::Span;
using GatherRate::Way;

constexpr std::uint32_t RATE_ZONES = 4;
constexpr int RATE_WORLDS = 300;
constexpr int RATE_SPANS = 48;
constexpr int RATE_FADE_EVERY = 8;
constexpr double RATE_EPS = 1e-9;
constexpr double MINUTES_PER_HOUR = 60.0;

struct RateViolations
{
    int unobserved{0};  // no observation, yet an estimate other than the prior or read as observed
    int outside{0};     // outside the prior and the observed rate, or not nearer the observed after more takes
    int crossTalk{0};   // a span changed another zone's or way's estimate
    int cutCounted{0};  // a span cut at once, or of mixed ways, changed an estimate
    int takeless{0};    // a span without a take did not raise its estimate
    int fadeAway{0};    // a fade moved an estimate away from its prior, or not nearer it
};

std::size_t RateSlot(std::uint32_t zone, std::size_t way) { return (zone - 1) * GatherRate::WAYS + way; }

std::vector<double> RateEstimates(Book const& book, std::vector<double> const& priors)
{
    GatherRate::View const view{book.Publish()};
    std::vector<double> estimates;
    for (std::uint32_t zone = 1; zone <= RATE_ZONES; ++zone)
        for (std::size_t way = 0; way < GatherRate::WAYS; ++way)
            estimates.push_back(view.Of(zone, static_cast<Way>(way), priors[RateSlot(zone, way)]).minutes);
    return estimates;
}

RateViolations RateSearch(GatherRate::Fault fault, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    RateViolations v;
    std::size_t const slots = RATE_ZONES * GatherRate::WAYS;
    for (int world = 0; world < RATE_WORLDS; ++world)
    {
        Book book;
        book.fault = fault;
        std::vector<double> priors(slots);
        for (double& prior : priors)
            prior = 0.2 + 3.0 * unit(rng);
        std::vector<GatherRate::Tally> mirror(slots);  // the test's own faded sums of the spans that count

        GatherRate::View const view{book.Publish()};
        for (std::uint32_t zone = 1; zone <= RATE_ZONES; ++zone)
            for (std::size_t way = 0; way < GatherRate::WAYS; ++way)
            {
                GatherRate::Estimate const estimate = view.Of(zone, static_cast<Way>(way), priors[RateSlot(zone, way)]);
                v.unobserved += estimate.minutes != priors[RateSlot(zone, way)] || estimate.observed ? 1 : 0;
            }
        Check(GatherRate::View{}.Of(1, Way::Mining, 1.5).minutes == 1.5, "an empty view is the static rule");

        for (int step = 0; step < RATE_SPANS; ++step)
        {
            std::vector<double> const before = RateEstimates(book, priors);
            if (step % RATE_FADE_EVERY == RATE_FADE_EVERY - 1)
            {
                double const minutes = 10.0 + 230.0 * unit(rng);
                book.Fade(minutes);
                double const keep = std::exp2(-minutes / GatherRate::FADE_HALF_LIFE_MINUTES);
                for (GatherRate::Tally& tally : mirror)
                    tally = {tally.minutes * keep, tally.takes * keep};
                std::vector<double> const after = RateEstimates(book, priors);
                for (std::size_t at = 0; at < slots; ++at)
                {
                    double const was = std::abs(before[at] - priors[at]);
                    double const now = std::abs(after[at] - priors[at]);
                    v.fadeAway += now > was + RATE_EPS || (was > RATE_EPS && !(now < was)) ? 1 : 0;
                }
                continue;
            }
            Span span;
            span.zone = 1 + static_cast<std::uint32_t>(rng() % RATE_ZONES);
            std::uint32_t const pick = rng() % 10;
            span.way = pick == 0 ? Way::None : static_cast<Way>(pick % GatherRate::WAYS);
            bool const quick = rng() % 5 == 0;
            span.milliseconds = quick ? 1 + rng() % GatherRate::CUT_MS : GatherRate::CUT_MS + 1 + rng() % 2400000;
            span.takes = rng() % 4 == 0 ? 0 : 1 + rng() % 12;
            span.resolved = quick && rng() % 2 ? 0 : span.takes + rng() % 3;
            bool const counts = span.way != Way::None && !GatherRate::CutAtOnce(span);
            book.Observe(span);
            std::vector<double> const after = RateEstimates(book, priors);
            std::size_t const own = counts ? RateSlot(span.zone, static_cast<std::size_t>(span.way)) : slots;
            if (counts)
            {
                GatherRate::Tally& tally = mirror[own];
                tally.minutes += span.milliseconds / GatherRate::MS_PER_MINUTE;
                tally.takes += span.takes;
            }
            for (std::size_t at = 0; at < slots; ++at)
            {
                if (!counts)
                    v.cutCounted += after[at] != before[at] ? 1 : 0;
                else if (at != own)
                    v.crossTalk += after[at] != before[at] ? 1 : 0;
                else if (!span.takes)
                    v.takeless += after[at] > before[at] ? 0 : 1;
                GatherRate::Tally const& tally = mirror[at];
                if (tally.takes > 0.0)
                {
                    double const observed = tally.minutes / tally.takes;
                    double const low = std::min(priors[at], observed);
                    double const high = std::max(priors[at], observed);
                    v.outside += after[at] < low * (1.0 - 1e-9) || after[at] > high * (1.0 + 1e-9) ? 1 : 0;
                }
            }
        }

        // One key fed at a steady rate: every span brings the estimate nearer the observed, never past it
        Book steady;
        steady.fault = fault;
        double const rate = 0.2 + 10.0 * unit(rng);
        double const prior = priors[0];
        double last = prior;
        for (int span = 0; span < 12; ++span)
        {
            std::uint32_t const takes = 1 + rng() % 6;
            steady.Observe({1, Way::Herbalism, static_cast<std::uint32_t>(rate * takes * GatherRate::MS_PER_MINUTE),
                            takes, takes});
            double const estimate = GatherRate::View{steady.Publish()}.Of(1, Way::Herbalism, prior).minutes;
            bool const between = estimate >= std::min(prior, rate) - 1e-6 && estimate <= std::max(prior, rate) + 1e-6;
            v.outside += !between || std::abs(estimate - rate) > std::abs(last - rate) + 1e-9 ? 1 : 0;
            last = estimate;
        }
    }
    return v;
}

void GatherRateSearch()
{
    RateViolations const v = RateSearch(GatherRate::Fault::None, 20261126);
    std::cout << "rate search (seed 20261126, " << RATE_WORLDS << " worlds): counterexamples " << v.unobserved
              << " unobserved moved, " << v.outside << " outside, " << v.crossTalk << " cross talk, " << v.cutCounted
              << " cut counted, " << v.takeless << " takeless not raised, " << v.fadeAway << " fade away\n";
    Check(v.unobserved == 0, "no observation leaves every estimate the prior");
    Check(v.outside == 0, "the estimate stays between the prior and the observed, nearer the observed with more takes");
    Check(v.crossTalk == 0, "a zone's or a way's spans never change another's estimate");
    Check(v.cutCounted == 0, "a span cut at once or of mixed ways changes nothing");
    Check(v.takeless == 0, "a span without a take raises the minutes per take");
    Check(v.fadeAway == 0, "fading brings an estimate back toward its prior");

    int const defaultRate = RateSearch(GatherRate::Fault::DefaultRate, 20261126).unobserved;
    int const takesLeftOut = RateSearch(GatherRate::Fault::TakesLeftOut, 20261126).outside;
    int const zoneIgnored = RateSearch(GatherRate::Fault::ZoneIgnored, 20261126).crossTalk;
    int const cutCounted = RateSearch(GatherRate::Fault::CutCounted, 20261126).cutCounted;
    int const takelessSkipped = RateSearch(GatherRate::Fault::TakelessSkipped, 20261126).takeless;
    int const noFade = RateSearch(GatherRate::Fault::NoFade, 20261126).fadeAway;
    std::cout << "rate controls: default rate " << defaultRate << ", takes left out " << takesLeftOut
              << ", zone ignored " << zoneIgnored << ", cut counted " << cutCounted << ", takeless skipped "
              << takelessSkipped << ", no fade " << noFade << '\n';
    Check(defaultRate > 0, "control: a made-up rate for a zone without observations is caught");
    Check(takesLeftOut > 0, "control: the observed takes left out of the weight are caught");
    Check(zoneIgnored > 0, "control: one tally per way for every zone is caught");
    Check(cutCounted > 0, "control: a span cut at once observed is caught");
    Check(takelessSkipped > 0, "control: a span without a take skipped is caught");
    Check(noFade > 0, "control: observations that never fade are caught");

    Check(GatherRate::WayOf(4, 0, 4) == Way::Creatures && GatherRate::WayOf(4, 1, 3) == Way::None &&
              GatherRate::WayOf(4, 4, 0) == Way::Herbalism && GatherRate::WayOf(4, 0, 0) == Way::Mining &&
              GatherRate::WayOf(4, 2, 0) == Way::None && GatherRate::WayOf(0, 0, 0) == Way::None,
          "a circuit's way is part 4's: all creatures, all herbs, no herb; a mix is none");
    Check(GatherRate::CutAtOnce({1, Way::Mining, GatherRate::CUT_MS, 0, 0}) &&
              !GatherRate::CutAtOnce({1, Way::Mining, GatherRate::CUT_MS, 0, 1}) &&
              !GatherRate::CutAtOnce({1, Way::Mining, GatherRate::CUT_MS + 1, 0, 0}),
          "cut at once: at most CUT_MS with no stop resolved");
    Book book;
    book.Observe({1, Way::Mining, 30 * 60000, 3, 3});
    GatherRate::Tally const tally = book.tallies.begin()->second;
    Check(Near(GatherRate::EstimateOf(tally, 1.0).minutes, (10.0 + 30.0) / 13.0) &&
              !GatherRate::EstimateOf(tally, 1.0).observed &&
              GatherRate::EstimateOf({50.0, GatherRate::PRIOR_TAKES}, 1.0).observed,
          "the estimate: (prior x weight + minutes) / (weight + takes), observed from PRIOR_TAKES takes");
    book.Fade(GatherRate::FADE_HALF_LIFE_MINUTES);
    Check(Near(book.tallies.begin()->second.takes, 1.5), "a half-life halves the takes");
    book.Fade(100.0 * GatherRate::FADE_HALF_LIFE_MINUTES);
    Check(book.tallies.empty(), "a tally faded to nothing is forgotten");
}

// The readers by hand: a node row's and a drop row's minutes per unit and a circuit's lap take the estimate over
// their own static minutes; another zone or way leaves them as they were
void GatherRateReaders()
{
    constexpr std::uint32_t ZONE = 7;
    constexpr std::uint32_t OTHER_ZONE = 8;
    Book book;
    book.Observe({ZONE, Way::Herbalism, 60 * 60000, 12, 12});
    book.Observe({ZONE, Way::Creatures, 45 * 60000, 9, 9});
    GatherRate::View const view{book.Publish()};
    GatherRate::Tally const herbs = book.tallies.at(GatherRate::KeyOf(ZONE, Way::Herbalism));
    GatherRate::Tally const kills = book.tallies.at(GatherRate::KeyOf(ZONE, Way::Creatures));

    Materials::Bot materials;
    materials.skills[HERBALISM] = 100;
    materials.skills[MINING] = 100;
    materials.rates = BotWith(100, 100, 100).rates;
    materials.level = 10;
    Recipes::NodeRow herb{ZONE, HERBALISM, 0, 1617, 5, 120.0f, 765, 2.0};
    Recipes::NodeRow ore = herb;
    ore.skill = MINING;
    Recipes::NodeRow elsewhere = herb;
    elsewhere.zone = OTHER_ZONE;
    Materials::Bot observed = materials;
    observed.gatherRates = view;
    double const herbPrior = Materials::NodeCost(herb, materials).minutes * herb.perNode;
    Check(Near(Materials::NodeCost(herb, observed).minutes,
               GatherRate::EstimateOf(herbs, herbPrior).minutes / herb.perNode) &&
              Materials::NodeCost(herb, observed).minutes > Materials::NodeCost(herb, materials).minutes,
          "Gathered: the zone's herbalism estimate over the unit's yield");
    Check(Materials::NodeCost(ore, observed).minutes == Materials::NodeCost(ore, materials).minutes &&
              Materials::NodeCost(elsewhere, observed).minutes == Materials::NodeCost(elsewhere, materials).minutes,
          "Gathered: another way's or zone's observations leave it static");

    Recipes::DropRow row;
    row.zone = ZONE;
    row.entry = 525;
    row.item = 750;
    row.spawns = 8;
    row.spacing = 40.0f;
    row.minLevel = 9;
    row.maxLevel = 10;
    row.perKill = 0.5;
    Recipes::DropRow far = row;
    far.zone = OTHER_ZONE;
    Materials::Cost const staticDrop = Materials::DropCost(row, materials);
    Materials::Cost const ratedDrop = Materials::DropCost(row, observed);
    Check(!staticDrop.Blocked() &&
              Near(ratedDrop.minutes,
                   GatherRate::EstimateOf(kills, staticDrop.minutes * row.perKill).minutes / row.perKill),
          "DropCost: the zone's creature estimate over the units a kill gives");
    Check(Materials::DropCost(far, observed).minutes == Materials::DropCost(far, materials).minutes,
          "DropCost: another zone's observations leave it static");

    // A one-way circuit whose stops all pay at any factor: its lap's minutes per take are the estimate
    Gatherer bot = BotWith(100, 100, 100);
    bot.zone = ZONE;
    std::vector<Node> nodes;
    for (std::uint32_t i = 0; i < 5; ++i)
        nodes.push_back(NodeAt(100 + i, HERBALISM, 50, 60.0f * (i + 1), 40.0f * (i % 2), 300, 1.0f, 50000));
    Point const anchor{0.0f, 0.0f, 0.0f};
    Circuit const plain = Gather::BuildCircuit(anchor, nodes, bot);
    bot.gatherRates = view;
    Circuit const scaled = Gather::BuildCircuit(anchor, nodes, bot);
    double const takes = static_cast<double>(nodes.size());
    double const prior = plain.lapMinutes / takes;
    Check(plain.stops.size() == nodes.size() && scaled.stops == plain.stops &&
              Near(scaled.lapMinutes / takes, GatherRate::EstimateOf(herbs, prior).minutes) && scaled.observed &&
              !plain.observed,
          "circuit: a one-way lap's minutes per take are the zone's estimate, read as observed");
    Check(scaled.laps == (scaled.once ? 1u
                                      : std::max(1u, static_cast<std::uint32_t>(
                                                         std::floor(Gather::SESSION_MINUTES / scaled.lapMinutes)))) &&
              scaled.laps <= plain.laps,
          "circuit: the laps follow the longer lap");
    Gatherer away = bot;
    away.zone = OTHER_ZONE;
    Circuit const there = Gather::BuildCircuit(anchor, nodes, away);
    Check(there.lapMinutes == plain.lapMinutes && !there.observed, "circuit: another zone's observations leave it");
    Check(
        Gather::CircuitOption(scaled, nodes, bot, 0).observed && !Gather::CircuitOption(plain, nodes, bot, 0).observed,
        "the option carries the observed flag");

    // A mixed circuit: each stop's share of the lap by its own way's factor; not every way observed
    std::vector<Node> mixed = nodes;
    for (std::uint32_t i = 0; i < 3; ++i)
        mixed.push_back(NodeAt(200 + i, MINING, 50, 60.0f * (i + 1), -50.0f, 300, 1.0f, 50000));
    bot.gatherRates = {};
    Circuit const mixedPlain = Gather::BuildCircuit(anchor, mixed, bot);
    bot.gatherRates = view;
    Circuit const mixedScaled = Gather::BuildCircuit(anchor, mixed, bot);
    double const mixedPrior = mixedPlain.lapMinutes / static_cast<double>(mixed.size());
    double const factor = GatherRate::EstimateOf(herbs, mixedPrior).minutes / mixedPrior;
    Check(mixedScaled.stops == mixedPlain.stops &&
              Near(mixedScaled.lapMinutes, mixedPlain.lapMinutes * (5.0 * factor + 3.0) / 8.0) && !mixedScaled.observed,
          "circuit: a mixed lap scales each way's share by its factor and is not observed");

    // The pruning by the same factor: at a slow enough observed rate no stop pays
    Book slow;
    slow.Observe({ZONE, Way::Herbalism, 4000000000u, 100, 100});  // 666 minutes per take
    for (Node& node : nodes)
        node.yields[0].vendorCopper = 50;
    bot.gatherRates = {};
    Circuit const cheapPlain = Gather::BuildCircuit(anchor, nodes, bot);
    bot.gatherRates = {slow.Publish()};
    Circuit const cheapSlow = Gather::BuildCircuit(anchor, nodes, bot);
    Check(!cheapPlain.stops.empty() && cheapSlow.stops.size() < cheapPlain.stops.size(),
          "circuit: a stop that pays at the static rule is pruned at a slow observed rate");
}

struct NightRow
{
    int hour{0};
    std::uint32_t zone{0};
    Way way{Way::None};
    double minutes{0.0};
    double takes{0.0};
    double pmin{0.0};
    double ptakes{0.0};
};

double Median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    std::size_t const middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

// The night's part 4 rows through the rule: each hour's spans observed, an hour's fade between; the prior of a zone
// and way is the planned minutes per take its rows carried
void NightTable(std::string const& repo, bool everyPair)
{
    std::string const path = repo + "/tests/fixtures/gather-rate-night.tsv";
    std::ifstream in(path);
    Check(in.good(), "the night's rows are read: " + path);
    std::vector<NightRow> rows;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#' || line.rfind("hour", 0) == 0)
            continue;
        std::istringstream fields(line);
        NightRow row;
        std::string way;
        double spans = 0.0;
        fields >> row.hour >> row.zone >> way >> spans >> row.minutes >> row.takes >> row.pmin >> row.ptakes;
        row.way = way == "herbalism" ? Way::Herbalism : way == "mining" ? Way::Mining : Way::Creatures;
        rows.push_back(row);
    }
    Check(rows.size() > 100, "the night holds its rows: " + std::to_string(rows.size()));
    std::map<std::uint64_t, std::pair<double, double>> planned;  // key -> planned minutes, takes
    std::map<std::uint64_t, std::pair<double, double>> night;    // key -> observed minutes, takes
    for (NightRow const& row : rows)
    {
        std::uint64_t const key = GatherRate::KeyOf(row.zone, row.way);
        planned[key].first += row.pmin;
        planned[key].second += row.ptakes;
        night[key].first += row.minutes;
        night[key].second += row.takes;
    }
    char const* const names[] = {"herbalism", "mining", "creatures"};
    std::array<int, 3> const hours = {1, 3, 6};
    std::map<std::uint64_t, std::array<GatherRate::Estimate, 3>> table;
    int outside = 0;
    for (std::size_t at = 0; at < hours.size(); ++at)
    {
        Book book;
        for (int hour = 0; hour < hours[at]; ++hour)
        {
            if (hour)
                book.Fade(MINUTES_PER_HOUR);
            for (NightRow const& row : rows)
                if (row.hour == hour)
                    book.Observe({row.zone, row.way,
                                  static_cast<std::uint32_t>(std::lround(row.minutes * GatherRate::MS_PER_MINUTE)),
                                  static_cast<std::uint32_t>(row.takes), 1});
        }
        GatherRate::View const view{book.Publish()};
        std::array<std::vector<double>, 3> ratio;
        std::array<int, 3> pairs{};
        std::array<int, 3> observedPairs{};
        for (auto const& [key, sums] : planned)
        {
            if (!(sums.second > 0.0) || !(sums.first > 0.0))
                continue;
            std::uint32_t const zone = static_cast<std::uint32_t>(key >> 8);
            Way const way = static_cast<Way>(key & 0xFF);
            double const prior = sums.first / sums.second;
            GatherRate::Estimate const estimate = view.Of(zone, way, prior);
            table[key][at] = estimate;
            std::size_t const w = static_cast<std::size_t>(way);
            ++pairs[w];
            observedPairs[w] += estimate.observed ? 1 : 0;
            ratio[w].push_back(estimate.minutes / prior);
            if (GatherRate::Tally const* tally = view.snapshot->Find(zone, way); tally && tally->takes > 0.0)
            {
                double const rate = tally->minutes / tally->takes;
                outside +=
                    estimate.minutes < std::min(prior, rate) - 1e-9 || estimate.minutes > std::max(prior, rate) + 1e-9
                        ? 1
                        : 0;
            }
        }
        for (std::size_t w = 0; w < 3; ++w)
            std::cout << "night after " << hours[at] << " h, " << names[w] << ": " << pairs[w] << " pairs, "
                      << observedPairs[w] << " observed, estimate/prior median " << Fixed(Median(ratio[w]), 2) << '\n';
    }
    Check(outside == 0, "the night's estimates stay between prior and observed: " + std::to_string(outside));
    if (!everyPair)
        return;
    std::cout << "zone way prior | estimate (o = observed) after 1 h, 3 h, 6 h | the night's minutes per take, takes\n";
    for (auto const& [key, estimates] : table)
    {
        double const prior = planned[key].first / planned[key].second;
        std::cout << (key >> 8) << ' ' << names[key & 0xFF] << ' ' << Fixed(prior, 2) << " |";
        for (GatherRate::Estimate const& estimate : estimates)
            std::cout << ' ' << Fixed(estimate.minutes, 2) << (estimate.observed ? " o" : " -");
        auto const& [minutes, takes] = night[key];
        std::cout << " | " << (takes > 0.0 ? Fixed(minutes / takes, 2) : std::string("-")) << ' ' << takes << '\n';
    }
}

int main(int argc, char** argv)
{
    std::string repo = ".";
    bool everyPair = false;
    for (int at = 1; at < argc; ++at)
    {
        std::string const arg = argv[at];
        if (arg == "--decisions")
        {
            PrintDecisions();
            return 0;
        }
        if (arg == "--night")
            everyPair = true;
        else
            repo = arg;
    }
    HandCases();
    PassCases();
    DownCases();
    SightCases();
    RefusalCases();
    UnarmedCases();
    WalkCases();
    Search();
    CreatureCases();
    CreatureSearch();
    RateCases();
    MarketSearch();
    GatherRateSearch();
    GatherRateReaders();
    NightTable(repo, everyPair);
    std::cout << "GATHER rules selftest: " << (passed + failed) << " checks, " << failed << " failed ("
              << (failed ? "FAIL" : "PASS") << ")\n";
    return failed ? 1 : 0;
}
