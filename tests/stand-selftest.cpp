/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of the stand point's rule (StandRules.h, pathing PA46, PA.16): where a bot stands to use a creature
// or an object. The cases are the plan's: the bank counter, no joined floor, a target over its floor, two bots, the
// least distance, sight, the side order, the mailbox; plus the model box. Each mutant (one rule broken on purpose)
// must fail the suite. The mesh half (NavMeshSnap::StandPoint) is proven on a lab by `route check targets`.
// Usage: stand-selftest
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "StandRules.h"

using namespace StandRules;

namespace
{
struct Counts
{
    int passed{0};
    int failures{0};
    bool loud{true};

    void Check(bool ok, std::string const& what)
    {
        if (ok)
        {
            ++passed;
            return;
        }
        ++failures;
        if (loud)
            std::cout << "FAIL: " << what << "\n";
    }
};

using PickFn = Answer (*)(Target const&, std::vector<Candidate> const&, std::uint64_t);
using CreatureReachFn = float (*)(float, float);
using ObjectReachFn = float (*)(std::uint32_t);

struct Rule
{
    PickFn pick;
    CreatureReachFn creatureReach;
    ObjectReachFn objectReach;
};

constexpr std::uint64_t KEYS = 32;  // bots tried per case: a rule must hold for every bot

Candidate At(float x, float y, float z, bool joined = true, bool inSight = true)
{
    Candidate c;
    c.x = x;
    c.y = y;
    c.z = z;
    c.joined = joined;
    c.inSight = inSight;
    return c;
}

Candidate Polar(float degrees, float yards, float z, bool joined = true, bool inSight = true)
{
    float const radians = degrees * PI / 180.0f;
    return At(std::cos(radians) * yards, std::sin(radians) * yards, z, joined, inSight);
}

// A creature at the origin facing +x (east), its reach from its size
Target Creature(Rule const& rule, float size, float z = 0.0f)
{
    Target target;
    target.z = z;
    target.facing = 0.0f;
    target.maxYards = MaxYards(rule.creatureReach(size, BOT_SIZE_YARDS));
    target.minYards = MinYards(target.maxYards);
    return target;
}

// a) The bank counter: the banker on a closed piece of 14 polygons, joined floor 3.2 to 3.8 yd in front of him, and
// joined floor beside him nearer than the front's
void BankCounter(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates;
    for (int i = 0; i < 14; ++i)  // the counter's own piece, in front of him
        candidates.push_back(
            Polar(-40.0f + 6.0f * static_cast<float>(i), 1.6f + 0.05f * static_cast<float>(i), 0.0f, false));
    for (int i = 0; i < 7; ++i)  // the hall floor in front, a step down
        candidates.push_back(Polar(-30.0f + 10.0f * static_cast<float>(i), 3.2f + 0.1f * static_cast<float>(i), -1.0f));
    candidates.push_back(Polar(90.0f, 2.0f, -1.0f));  // the hall floor beside the counter, nearer
    bool allJoinedFront = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        allJoinedFront = allJoinedFront && answer.why == Why::Found && candidates[answer.index].joined &&
                         answer.side == Side::Front && answer.yards >= 3.2f && answer.yards <= 4.0f;
    }
    counts.Check(allJoinedFront, "a: the bank counter is served from the joined floor in front, for every bot");
}

// b) No joined floor within reach: `fragment`, with the nearest closed point named; joined floor beyond the reach
void NoJoinedFloor(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates;
    for (int i = 0; i < 10; ++i)
        candidates.push_back(Polar(36.0f * static_cast<float>(i), 2.0f + 0.2f * static_cast<float>(i), 0.0f, false));
    candidates.push_back(Polar(0.0f, 9.0f, 0.0f));  // joined, beyond 8 yd
    bool fragment = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        fragment =
            fragment && answer.why == Why::Fragment && answer.index == 0 && std::fabs(answer.yards - 2.0f) < 0.01f;
    }
    counts.Check(fragment, "b: no joined floor within reach is fragment, naming the nearest closed point");
    counts.Check(rule.pick(target, {}, 0).why == Why::NoPoly, "b: no candidate at all is no_poly");
}

// c) A creature 6.5 yd over its floor (size 3, the core's reach 10): the floor under it serves
void OverItsFloor(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 3.0f, 6.5f);
    std::vector<Candidate> candidates;
    for (int i = 0; i < 12; ++i)
        candidates.push_back(Polar(30.0f * static_cast<float>(i), 2.0f + 0.25f * static_cast<float>(i), 0.0f));
    Answer const answer = rule.pick(target, candidates, 7);
    counts.Check(answer.why == Why::Found && answer.yards > 6.5f && answer.yards <= 9.5f,
                 "c: a target 6.5 yd over its floor is reached by the sizes' reach");
    counts.Check(std::fabs(CreatureReach(3.0f) - 10.0f) < 0.001f && std::fabs(CreatureReach(6.0f) - 13.0f) < 0.001f,
                 "c: the core's reach is 5.5 plus both sizes (O'ros 10, Geirrvif 13)");
}

// d) Two bots get two points: each its own by its key, and the second never on the first's
void TwoBots(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates;
    for (int i = 0; i < 9; ++i)
        candidates.push_back(Polar(-40.0f + 10.0f * static_cast<float>(i), 3.0f + 0.3f * static_cast<float>(i), 0.0f));
    bool apart = true;
    bool repeats = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const first = rule.pick(target, candidates, key);
        repeats = repeats && rule.pick(target, candidates, key).index == first.index;
        std::vector<Candidate> taken = candidates;
        std::vector<std::array<float, 3>> const occupants{
            {candidates[first.index].x, candidates[first.index].y, candidates[first.index].z}};
        for (Candidate& c : taken)
            c.occupied = Taken(c.x, c.y, c.z, occupants);
        Answer const second = rule.pick(target, taken, key + 1000);
        apart =
            apart && first.why == Why::Found && second.why == Why::Found &&
            Distance(candidates[first.index].x, candidates[first.index].y, candidates[first.index].z,
                     candidates[second.index].x, candidates[second.index].y, candidates[second.index].z) >= APART_YARDS;
    }
    counts.Check(apart, "d: two bots get two points, a player's reach apart");
    counts.Check(repeats, "d: the same bot repeats its point");
    std::vector<Candidate> full = candidates;
    for (Candidate& c : full)
        c.occupied = true;
    counts.Check(rule.pick(target, full, 3).why == Why::Occupied, "d: every point taken is occupied");
}

// e) No point nearer than the least distance
void LeastDistance(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates = {Polar(0.0f, 0.4f, 0.0f), Polar(10.0f, 0.4f, 0.0f), Polar(-10.0f, 1.2f, 0.0f),
                                         Polar(20.0f, 3.0f, 0.0f), Polar(-20.0f, 4.0f, 0.0f)};
    bool kept = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        kept = kept && answer.why == Why::Found && answer.yards >= target.minYards;
    }
    counts.Check(kept, "e: no point nearer than the least distance");
    std::vector<Candidate> const near = {Polar(0.0f, 0.4f, 0.0f)};
    counts.Check(rule.pick(target, near, 0).why == Why::NoPoly, "e: only a point on the target is no point");
    counts.Check(std::fabs(MinYards(8.0f) - LEAST_YARDS) < 0.001f && std::fabs(MinYards(2.0f) - 1.0f) < 0.001f,
                 "e: the least distance is never above half the greatest");
}

// f) In sight ranks first; with none in sight the best joined point, marked out of sight
void Sight(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates = {Polar(0.0f, 3.0f, 0.0f, true, false), Polar(20.0f, 3.5f, 0.0f, true, false),
                                         Polar(180.0f, 3.0f, 0.0f)};
    bool behind = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        behind = behind && answer.why == Why::Found && answer.index == 2 && answer.inSight;
    }
    counts.Check(behind, "f: the one point in sight is taken, though behind");
    candidates[2].inSight = false;
    bool front = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        front = front && answer.why == Why::Found && !answer.inSight && answer.side == Side::Front && answer.index < 2;
    }
    counts.Check(front, "f: none in sight: the best joined point (in front), marked out of sight");
    candidates[2].inSight = true;
    candidates[2].occupied = true;
    Answer const taken = rule.pick(target, candidates, 0);
    counts.Check(taken.why == Why::Found && !taken.inSight && taken.index < 2,
                 "f: the point in sight has a bot on it: a free point out of sight, marked");
}

// g) Behind only when front and beside have none
void SideOrder(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates = {Polar(180.0f, 2.0f, 0.0f), Polar(150.0f, 2.2f, 0.0f), Polar(90.0f, 6.0f, 0.0f),
                                         Polar(-100.0f, 6.5f, 0.0f)};
    bool beside = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        beside = beside && answer.why == Why::Found && answer.side == Side::Beside && answer.index >= 2;
    }
    counts.Check(beside, "g: beside before behind, however near behind is");
    std::vector<Candidate> const back = {Polar(180.0f, 2.0f, 0.0f), Polar(150.0f, 2.2f, 0.0f)};
    Answer const last = rule.pick(target, back, 5);
    counts.Check(last.why == Why::Found && last.side == Side::Behind, "g: behind when nothing else is");
    std::vector<Candidate> mixed = candidates;
    mixed.push_back(Polar(30.0f, 7.5f, 0.0f));
    bool front = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
        front = front && rule.pick(target, mixed, key).index == 4;
    counts.Check(front, "g: front before beside and behind");
    Target faceless = target;
    faceless.faced = false;
    counts.Check(SideOf(faceless, -3.0f, 0.0f) == Side::Front, "g: a target with no facing has no back");
}

// j) G350, Stormwind's bank: joined floor 1.8-2.0 yd in front of the banker that the bot's floor does not lead to (the
// counter's own piece, its path ends 3.1 yd out), the hall floor 3.2-3.8 yd out that it does: every bot takes the hall
// floor; nothing reached is `unreached`, whatever is joined
void Unreachable(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates;
    for (int i = 0; i < 9; ++i)
    {
        candidates.push_back(
            Polar(-40.0f + 10.0f * static_cast<float>(i), 1.8f + 0.025f * static_cast<float>(i), 0.0f));
        candidates.back().reachable = false;
    }
    for (int i = 0; i < 5; ++i)
        candidates.push_back(Polar(-20.0f + 10.0f * static_cast<float>(i), 3.2f + 0.15f * static_cast<float>(i), 0.0f));
    bool hall = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        hall = hall && answer.why == Why::Found && candidates[answer.index].reachable && answer.yards >= 3.2f;
    }
    counts.Check(hall, "j: a joined point the bot's floor does not lead to is no stand point, for every bot");
    std::vector<Candidate> none = candidates;
    for (Candidate& c : none)
        c.reachable = false;
    Answer const unreached = rule.pick(target, none, 0);
    counts.Check(unreached.why == Why::Found && !unreached.reached && std::string(RowReason(unreached)) == "unreached",
                 "j: joined points none reached: a joined point, marked unreached");
}

// k) G353, Ironforge's bank (banker 5099, the bot 19.5 yd off in front): the joined floor's only way to the band is
// 130-150 yd long and runs 35-40 yd from the flood circle's centre (radius 27.8), so the flood reaches none of the
// band's joined points; the closed piece behind the counter is not joined. Every bot still takes a joined front
// point, marked unreached, and arrives within the band's reach (no bot left where it cannot act, PA.12)
void WindingWay(Rule const& rule, Counts& counts)
{
    Target const target = Creature(rule, 1.5f);
    std::vector<Candidate> candidates;
    for (int i = 0; i < 4; ++i)
        candidates.push_back(Polar(-30.0f + 20.0f * static_cast<float>(i), 1.6f, 0.9f, false));
    for (int ring = 0; ring < 5; ++ring)
        for (int i = 0; i < 5; ++i)
        {
            candidates.push_back(Polar(-40.0f + 20.0f * static_cast<float>(i), 3.5f + static_cast<float>(ring), 0.1f));
            candidates.back().reachable = false;
        }
    bool served = true;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = rule.pick(target, candidates, key);
        served = served && answer.why == Why::Found && candidates[answer.index].joined && answer.side == Side::Front &&
                 !answer.reached && std::string(RowReason(answer)) == "unreached" && !ArrivesOnPoint(answer, true);
    }
    counts.Check(served, "k: a winding way the flood cut: a joined front point, marked, arrived at within the band");
    std::vector<Candidate> one = candidates;
    std::size_t const straight = 4 + 2;  // 3.5 yd straight in front
    one[straight].reachable = true;
    Answer const answer = rule.pick(target, one, 0);
    counts.Check(answer.why == Why::Found && answer.index == straight && answer.reached &&
                     ArrivesOnPoint(answer, true) && !ArrivesOnPoint(answer, false),
                 "k: one point reached: it is taken and arrived on; an unread flood arrives within the band");
}

// h) A mailbox is served from up to 10 yd (less the margin)
void Mailbox(Rule const& rule, Counts& counts)
{
    Target target;
    target.maxYards = MaxYards(rule.objectReach(OBJECT_MAILBOX));
    target.minYards = MinYards(target.maxYards);
    std::vector<Candidate> const candidates = {Polar(0.0f, 9.5f, 0.0f)};
    Answer const answer = rule.pick(target, candidates, 0);
    counts.Check(answer.why == Why::Found && std::fabs(answer.yards - 9.5f) < 0.01f, "h: a mailbox serves 9.5 yd out");
    counts.Check(std::fabs(rule.objectReach(OBJECT_DOOR) - 5.0f) < 0.001f &&
                     std::fabs(rule.objectReach(OBJECT_QUESTGIVER) - 5.5555553f) < 0.001f &&
                     std::fabs(rule.objectReach(3) - INTERACTION_YARDS) < 0.001f,
                 "h: the core's other object distances (door 5, quest giver 5.56, else 5.5)");
}

// The model box: a hanging relic 94 yd over the floor, scaled, and a turned box
void ModelBox(Rule const& rule, Counts& counts)
{
    Target relic;
    relic.z = 94.0f;
    relic.boxed = true;
    relic.box = {-20.0f, -20.0f, -92.0f, 20.0f, 20.0f, 10.0f};
    relic.maxYards = MaxYards(rule.objectReach(OBJECT_QUESTGIVER));
    relic.minYards = MinYards(relic.maxYards);
    std::vector<Candidate> const floor = {Polar(0.0f, 10.0f, 0.0f)};
    counts.Check(rule.pick(relic, floor, 0).why == Why::Found, "box: the floor within the box's reach serves");
    relic.box.minZ = -80.0f;  // the box ends 14 yd over the floor
    counts.Check(rule.pick(relic, floor, 0).why == Why::NoPoly, "box: the floor beyond the box's reach does not");

    Target turned;
    turned.boxed = true;
    turned.box = {-10.0f, -1.0f, -1.0f, 10.0f, 1.0f, 3.0f};
    float const half = std::sqrt(0.5f);
    turned.rotation[2] = half;  // a quarter turn about z: the long side along y
    turned.rotation[3] = half;
    counts.Check(
        BoxYards(turned, 0.0f, 9.0f, 0.0f) < 0.001f && std::fabs(BoxYards(turned, 9.0f, 0.0f, 0.0f) - 8.0f) < 0.001f,
        "box: the box turns with the object (the core's frame)");
    counts.Check(OuterYards(turned) > 10.0f, "box: the sampled band covers the box");
}

void RingChecks(Counts& counts)
{
    std::vector<float> const rings = StandRules::Rings(1.5f, 8.0f);
    counts.Check(rings.size() == 8 && rings.front() == 1.5f && rings.back() == 8.0f, "rings: 1 yd apart to the edge");
    counts.Check(StandRules::Rings(1.5f, 200.0f).size() <= static_cast<std::size_t>(MOST_RINGS) + 1,
                 "rings: a large band in a bounded count");
}

void Suite(Rule const& rule, Counts& counts)
{
    BankCounter(rule, counts);
    NoJoinedFloor(rule, counts);
    OverItsFloor(rule, counts);
    TwoBots(rule, counts);
    LeastDistance(rule, counts);
    Sight(rule, counts);
    SideOrder(rule, counts);
    Mailbox(rule, counts);
    ModelBox(rule, counts);
    Unreachable(rule, counts);
    WindingWay(rule, counts);
}

float RealCreatureReach(float size, float bot) { return CreatureReach(size, bot); }

// The mutants: each breaks one rule
Answer IgnoresJoined(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    std::vector<Candidate> all = candidates;
    for (Candidate& c : all)
        c.joined = true;
    return Pick(target, all, key);
}

Answer FallsBackToPiece(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer = Pick(target, candidates, key);
    if (answer.why == Why::Fragment)
        answer.why = Why::Found;
    return answer;
}

Answer NearestAnySide(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer = Pick(target, candidates, key);
    if (answer.why != Why::Found)
        return answer;
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        Candidate const& c = candidates[i];
        float const yards = Distance(c.x, c.y, c.z, target.x, target.y, target.z);
        if (InBand(target, c.x, c.y, c.z) && c.joined && c.inSight && !c.occupied && yards < answer.yards)
        {
            answer.index = i;
            answer.yards = yards;
            answer.side = SideOf(target, c.x, c.y);
        }
    }
    return answer;
}

Answer BesideLikeBehind(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer = Pick(target, candidates, key);
    if (answer.why != Why::Found || answer.side == Side::Front)
        return answer;
    std::vector<std::size_t> pool;
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        Candidate const& c = candidates[i];
        if (InBand(target, c.x, c.y, c.z) && c.joined && c.inSight == answer.inSight && !c.occupied)
            pool.push_back(i);
    }
    answer.index = pool[Spread(key, pool.size())];
    answer.side = SideOf(target, candidates[answer.index].x, candidates[answer.index].y);
    return answer;
}

Answer SamePointForAll(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t)
{
    std::vector<Candidate> all = candidates;
    for (Candidate& c : all)
        c.occupied = false;
    return Pick(target, all, 0);
}

Answer IgnoresLeast(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Target near = target;
    near.minYards = 0.0f;
    return Pick(near, candidates, key);
}

Answer IgnoresSight(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    std::vector<Candidate> all = candidates;
    for (Candidate& c : all)
        c.inSight = true;
    return Pick(target, all, key);
}

Answer RefusesOutOfSight(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer = Pick(target, candidates, key);
    if (answer.why == Why::Found && !answer.inSight)
        answer.why = Why::NoPoly;
    return answer;
}

Answer LosesSightFlag(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer = Pick(target, candidates, key);
    answer.inSight = true;
    return answer;
}

Answer IgnoresReached(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    std::vector<Candidate> all = candidates;
    for (Candidate& c : all)
        c.reachable = true;
    return Pick(target, all, key);
}

// PA47e's rule: no point when the flood reached none (G353's loss)
Answer ReachedOrNothing(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer = Pick(target, candidates, key);
    if (!answer.reached)
        answer.why = Why::NoPoly;
    return answer;
}

Answer ForgetsMark(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer = Pick(target, candidates, key);
    answer.reached = true;
    return answer;
}

float FixedReach(float, float) { return INTERACTION_YARDS; }

float FixedObjectReach(std::uint32_t) { return INTERACTION_YARDS; }
}  // namespace

// PA47 (continued): occupants by place, the spacing beyond the body's width, a trip's end seeing an approach's claim.
// Each case beside its mutant
void ByPlace(Counts& counts)
{
    // Spacing: a mate 1.2 yd off takes the point; two bots' points that end half a yard short stay a body apart
    std::vector<std::array<float, 3>> const mate{{1.2f, 0.0f, 0.0f}};
    counts.Check(Taken(0.0f, 0.0f, 0.0f, mate) && !Taken(0.0f, 0.0f, 0.0f, {{1.6f, 0.0f, 0.0f}}),
                 "spacing: a point within a player's reach of a mate is taken, one beyond it is free");
    counts.Check(APART_YARDS - 0.5f >= BODY_YARDS, "spacing: points half a yard short of the rule still a body apart");
    counts.Check(!Taken(0.0f, 0.0f, 0.0f, mate, BODY_YARDS),
                 "mutant killed: spacing at the body's width (the matrix check's own width)");
    // Two bankers 4 yd apart (Darnassus): a claim at B's stand point counts for A, whose band reaches it
    Target const bankerA = ForCreature(0.0f, 0.0f, 0.0f, 0.0f, 1.5f, BOT_SIZE_YARDS, 5.5f);
    Target const bankerB = ForCreature(0.0f, 4.0f, 0.0f, 0.0f, 1.5f, BOT_SIZE_YARDS, 5.5f);
    std::array<float, 3> const claimForB{5.5f, 3.0f, 0.0f};
    bool const forB = CountsFor(bankerB, claimForB[0], claimForB[1], claimForB[2]);
    bool const forA = CountsFor(bankerA, claimForB[0], claimForB[1], claimForB[2]);
    counts.Check(forB && forA && InBand(bankerA, 5.5f, 2.6f, 0.0f) && Taken(5.5f, 2.6f, 0.0f, {claimForB}),
                 "by place: a claim at the neighbouring banker's point takes the point beside it");
    // Mutant: keyed by object (the old claims): A's occupants hold only the claims made for A
    std::vector<std::array<float, 3>> byObject;
    counts.Check(!Taken(5.5f, 2.6f, 0.0f, byObject), "mutant killed: claims keyed by object (B's claim unseen at A)");
    // A trip's end at the place (ForPlace, the goal's reach) sees the approach's claim, and the approach the trip's
    Target const tripEnd = ForPlace(0.0f, 0.0f, 0.0f, 10.0f);
    counts.Check(CountsFor(tripEnd, 4.0f, 1.0f, 0.0f) && CountsFor(bankerA, 4.0f, 1.0f, 0.0f) &&
                     !CountsFor(bankerA, 30.0f, 0.0f, 0.0f),
                 "by place: a trip's end and an approach at one place count each other's points, not far ones");
    counts.Check(OccupantYards(bankerA) > OuterYards(bankerA) + BODY_YARDS,
                 "by place: the occupants reach a player's reach beyond the band");
}

// G350: the approach arrives at its stand point, not within the band's reach of the target (Alandelius stood 1.07 yd
// off the vendor, his point 2.89 yd out). Each case beside its mutant
void PointArrival(Counts& counts)
{
    Target const vendor = ForCreature(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BOT_SIZE_YARDS, 5.5f);
    float const point[3]{2.89f, 0.0f, 0.0f};
    float const bot[3]{1.07f, 0.0f, 0.0f};
    counts.Check(PointYards(bot[0], bot[1], bot[2], point[0], point[1], point[2]) > ARRIVE_YARDS,
                 "arrival: a bot inside the band but off its point has not arrived");
    counts.Check(Distance(bot[0], bot[1], bot[2], 0.0f, 0.0f, 0.0f) <= CentreReach(vendor),
                 "mutant killed: arrival within the band's reach of the target (the old approach's There)");
    counts.Check(PointYards(2.6f, 0.2f, 0.8f, point[0], point[1], point[2]) <= ARRIVE_YARDS,
                 "arrival: on its point, the feet 0.8 yd off the mesh's height, the bot has arrived");
    counts.Check(PointYards(2.89f, 0.1f, 4.0f, point[0], point[1], point[2]) > ARRIVE_YARDS,
                 "arrival: over its point on another floor the bot has not arrived");
    counts.Check(APART_YARDS - ARRIVE_YARDS >= BODY_YARDS,
                 "arrival: one bot an arrival's width off its point stays a body from the next point");
}

// PA96: an approach answers There only where the core's own check passes from the bot, on its point's floor, and the
// band keeps the target's floor (G455: the Underbelly's banker, a point and the bot on the floor 7.5-9.7 yd below
// him, 1.5 yd across, answered There 9.74 yd off, beyond the core's 8.5). Each case beside its mutant (the old rule)
void CoreArrival(Counts& counts)
{
    Target const banker = ForCreature(0.0f, 0.0f, 0.0f, 0.0f, 1.5f, BOT_SIZE_YARDS, 10.0f);
    counts.Check(std::fabs(banker.maxYards - 8.0f) < 0.001f && std::fabs(ReachYards(banker) - 8.5f) < 0.001f,
                 "core: the banker's band reaches 8.0, the core 8.5");
    // The band: the upper floor around him (joined, the flood from below reaches none), the floor below reached and
    // out of sight
    std::vector<Candidate> floors;
    for (int i = 0; i < 7; ++i)
    {
        floors.push_back(Polar(-30.0f + 10.0f * static_cast<float>(i), 3.0f + 0.3f * static_cast<float>(i), 0.1f));
        floors.back().reachable = false;
    }
    for (int i = 0; i < 6; ++i)
        floors.push_back(Polar(60.0f * static_cast<float>(i), 1.0f + 0.2f * static_cast<float>(i), -7.6f, true, false));
    bool upper = Floored(banker, floors);
    bool lower = false;
    for (std::uint64_t key = 0; key < KEYS; ++key)
    {
        Answer const answer = Pick(banker, floors, key);
        upper = upper && answer.why == Why::Found && floors[answer.index].z > -1.0f && !answer.reached;
        Answer const old = PickOn(banker, floors, key, false);
        lower = lower || (old.why == Why::Found && floors[old.index].z < -7.0f);
    }
    counts.Check(upper, "band: the floor below the banker is no stand point, for every bot (his own floor, unreached)");
    counts.Check(lower, "mutant killed: the band in 3D only (a reached point on the floor below)");
    std::vector<Candidate> ground;
    for (int i = 0; i < 8; ++i)
        ground.push_back(Polar(45.0f * static_cast<float>(i), 2.0f + 0.3f * static_cast<float>(i), 0.0f));
    Target const hover = ForCreature(0.0f, 0.0f, 6.5f, 0.0f, 3.0f, BOT_SIZE_YARDS, 10.0f);
    counts.Check(!Floored(hover, ground) && Pick(hover, ground, 0).why == Why::Found,
                 "band: a floating target has no floor of its own, the ground under it serves");

    auto const arrive =
        [&banker](bool atPoint, std::array<float, 3> const& point, std::array<float, 3> const& bot, bool old)
    {
        float const pointYards = PointYards(bot[0], bot[1], bot[2], point[0], point[1], point[2]);
        float const placeYards = Distance(bot[0], bot[1], bot[2], banker.x, banker.y, banker.z);
        return Arrive(atPoint, pointYards, placeYards, CentreReach(banker),
                      [&] { return old || Reaches(banker, point, bot); });
    };
    // On a point at the band's edge, 0.4 yd across and 1.9 yd under it: 8.52 yd off, beyond the core
    std::array<float, 3> const edge{7.9f, 0.0f, 0.0f};
    std::array<float, 3> const under{8.3f, 0.0f, -1.9f};
    counts.Check(arrive(true, edge, under, false) == StandRules::Arrival::Short,
                 "arrive: on its point beyond the core's reach the approach ends, no walk to where it stands");
    counts.Check(arrive(true, edge, under, true) == StandRules::Arrival::There,
                 "mutant killed: on its point, whatever the core");
    counts.Check(arrive(true, edge, {7.7f, 0.2f, 0.3f}, false) == StandRules::Arrival::There,
                 "arrive: on its point on the target's floor, There");
    // Under its point by more than the arrival's height slack (the Underbelly's 9.6 yd)
    counts.Check(arrive(true, {1.0f, 0.5f, -7.65f}, {1.0f, 0.5f, -9.55f}, false) == StandRules::Arrival::Short,
                 "arrive: under a point a floor away, 9.6 yd off, the approach ends");
    // Unreached (the band's reach of the place): within it 1.0 yd across and 6.5 yd under the banker, his point on his
    // floor 4 yd out; the plan's case 1.5 yd across and 9.7 yd under walks on
    std::array<float, 3> const front{4.0f, 0.0f, 0.0f};
    counts.Check(arrive(false, front, {1.0f, 0.0f, -6.5f}, false) == StandRules::Arrival::ToPoint,
                 "arrive: within the reach on the floor below, the bot walks on to its point");
    counts.Check(arrive(false, front, {1.0f, 0.0f, -6.5f}, true) == StandRules::Arrival::There,
                 "mutant killed: within the band's reach, whatever the floor");
    counts.Check(arrive(false, front, {1.5f, 0.0f, -9.7f}, false) == StandRules::Arrival::Walk,
                 "arrive: 1.5 yd across and 9.7 yd under, the bot walks (the plan's case)");
    int asks = 0;
    StandRules::Arrival const walking = Arrive(true, 3.0f, 9.0f, 8.0f,
                                               [&asks]
                                               {
                                                   ++asks;
                                                   return true;
                                               });
    counts.Check(walking == StandRules::Arrival::Walk && asks == 0,
                 "arrive: the core is asked only where the walk says arrived");
    // A boxed object for a far caller: its band's reach 11.2 yd centre to centre, the core's 10 yd to the box
    Box const box{-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 1.5f};
    float const rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    Target const mailbox =
        ForObject(0.0f, 0.0f, 0.0f, 0.0f, OBJECT_MAILBOX, &box, rotation, 0.4f, BOT_SIZE_YARDS, 20.0f);
    auto const boxed = [&mailbox](std::array<float, 3> const& bot, bool old)
    {
        std::array<float, 3> const point{9.5f, 0.0f, 0.0f};
        return Arrive(false, PointYards(bot[0], bot[1], bot[2], point[0], point[1], point[2]),
                      Distance(bot[0], bot[1], bot[2], 0.0f, 0.0f, 0.0f), CentreReach(mailbox),
                      [&] { return old || Reaches(mailbox, point, bot); });
    };
    counts.Check(CentreReach(mailbox) > 10.6f && boxed({10.6f, 0.0f, 0.0f}, false) == StandRules::Arrival::ToPoint &&
                     boxed({9.8f, 0.0f, 0.0f}, false) == StandRules::Arrival::There,
                 "arrive: a boxed object's band reach centre to centre, the core's to the box");
    counts.Check(boxed({10.6f, 0.0f, 0.0f}, true) == StandRules::Arrival::There,
                 "mutant killed: a boxed object centre to centre");
}

// PA47: the approach's target from the live creature or object, the caller's reach measured between the two bodies'
// edges as its own check does; the judge's reach; the row's words. Each case beside its mutant
void ApproachTargets(Counts& counts)
{
    // Geirrvif (G187): size 6, about 6 yd over the floor, the quest talk's reach 3.5 between the edges
    Target const floating = ForCreature(0.0f, 0.0f, 6.0f, 0.0f, 6.0f, BOT_SIZE_YARDS, 3.5f);
    counts.Check(std::fabs(floating.maxYards - 10.5f) < 0.001f && InBand(floating, 6.5f, 0.0f, 0.0f),
                 "i: the caller's reach plus both sizes: the floor under a large creature is in the band");
    Target asBefore = floating;  // mutant: the caller's reach taken centre to centre (the old approach's draw)
    asBefore.maxYards = 3.5f;
    counts.Check(!InBand(asBefore, 6.5f, 0.0f, 0.0f), "mutant killed: the caller's reach taken centre to centre");
    Target const banker = ForCreature(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BOT_SIZE_YARDS, 5.5f);
    counts.Check(std::fabs(banker.maxYards - 7.5f) < 0.001f && banker.minYards >= LEAST_YARDS - 0.001f &&
                     !InBand(banker, 0.5f, 0.0f, 0.0f),
                 "i: no draw from 0: a banker's band starts at the least distance");
    counts.Check(std::fabs(CentreReach(banker) - banker.maxYards) < 0.001f &&
                     std::fabs(CentreReach(floating) - floating.maxYards) < 0.001f,
                 "i: the judge reads the band's outer bound as its reach");
    // A mailbox (reach 10 to its box) for the visit's 5.5 yd: the caller's bound holds within the box's margin
    Box const box{-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 1.5f};
    float const rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    Target const mailbox =
        ForObject(0.0f, 0.0f, 0.0f, 0.0f, OBJECT_MAILBOX, &box, rotation, 0.4f, BOT_SIZE_YARDS, 5.5f);
    counts.Check(mailbox.boxed && InBand(mailbox, 6.0f, 0.0f, 0.0f) && !InBand(mailbox, 8.0f, 0.0f, 0.0f) &&
                     std::fabs(CentreReach(mailbox) - mailbox.capYards) < 0.001f,
                 "i: a boxed object keeps the caller's bound, centre to centre");
    Target uncapped = mailbox;  // mutant: the caller's bound dropped (the visit's own check would never see it there)
    uncapped.capYards = 0.0f;
    counts.Check(InBand(uncapped, 8.0f, 0.0f, 0.0f), "mutant killed: a boxed object without the caller's bound");
    Target const goal = ForPlace(0.0f, 0.0f, 0.0f, 10.0f);
    counts.Check(!goal.faced && std::fabs(goal.maxYards - 9.5f) < 0.001f && goal.minYards > 0.0f,
                 "i: a trip's goal: no front, its reach less the margin");
    Answer found;
    found.why = Why::Found;
    found.side = Side::Beside;
    Answer hidden = found;
    hidden.inSight = false;
    Answer none;
    none.why = Why::Fragment;
    counts.Check(std::string(RowChoice(found)) == "beside" && std::string(RowReason(found)) == "ok" &&
                     std::string(RowReason(hidden)) == "no_sight" && std::string(RowChoice(none)) == "none" &&
                     std::string(RowReason(none)) == "fragment",
                 "i: the stand row's words: side or none; ok, no_sight or the rule's word");
}

int main()
{
    Counts counts;
    Rule const real{Pick, RealCreatureReach, ObjectReach};
    Suite(real, counts);
    RingChecks(counts);
    ApproachTargets(counts);
    ByPlace(counts);
    PointArrival(counts);
    CoreArrival(counts);

    struct Mutant
    {
        char const* name;
        Rule rule;
    };
    Mutant const mutants[] = {
        {"mutant killed: the closed piece taken as floor (joined ignored)",
         {IgnoresJoined, RealCreatureReach, ObjectReach}},
        {"mutant killed: the nearest point whatever its side", {NearestAnySide, RealCreatureReach, ObjectReach}},
        {"mutant killed: fragment falls back onto the closed piece",
         {FallsBackToPiece, RealCreatureReach, ObjectReach}},
        {"mutant killed: the reach fixed at 5.5 (no sizes)", {Pick, FixedReach, ObjectReach}},
        {"mutant killed: the same point for every bot", {SamePointForAll, RealCreatureReach, ObjectReach}},
        {"mutant killed: the least distance ignored", {IgnoresLeast, RealCreatureReach, ObjectReach}},
        {"mutant killed: sight ignored", {IgnoresSight, RealCreatureReach, ObjectReach}},
        {"mutant killed: none in sight refused (the old filter)", {RefusesOutOfSight, RealCreatureReach, ObjectReach}},
        {"mutant killed: out of sight not marked", {LosesSightFlag, RealCreatureReach, ObjectReach}},
        {"mutant killed: beside and behind ranked alike", {BesideLikeBehind, RealCreatureReach, ObjectReach}},
        {"mutant killed: the mailbox's reach at 5.5", {Pick, RealCreatureReach, FixedObjectReach}},
        {"mutant killed: a point the bot's floor does not lead to taken (reachable ignored)",
         {IgnoresReached, RealCreatureReach, ObjectReach}},
        {"mutant killed: no point when the flood reached none (PA47e, G353)",
         {ReachedOrNothing, RealCreatureReach, ObjectReach}},
        {"mutant killed: a point the flood did not reach unmarked (arrived on, its row ok)",
         {ForgetsMark, RealCreatureReach, ObjectReach}},
    };
    for (Mutant const& mutant : mutants)
    {
        Counts silent;
        silent.loud = false;
        Suite(mutant.rule, silent);
        counts.Check(silent.failures > 0, mutant.name);
    }

    std::cout << "stand: " << counts.passed << " passed, " << counts.failures << " failed\n";
    return counts.failures ? 1 : 0;
}
