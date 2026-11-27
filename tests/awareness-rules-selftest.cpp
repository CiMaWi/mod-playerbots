/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Selftest of the bot's picture of the units around it (src/Ai/Awareness/AwarenessRules.h, awareness AW2): its
// types for both sides, the distance against the disc of a unit's estimate, the age rule and why an enemy is in a
// fight. Generated cases use a fixed seed, printed with every failure.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/Awareness /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel
//   tests/awareness-rules-selftest.cpp

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "AwarenessRules.h"
#include "DangerFold.h"
#include "DangerRules.h"
#include "DeathEdgeRules.h"
#include "DeathRowRules.h"
#include "RouteRules.h"

namespace
{
using namespace AwarenessRules;

int passed = 0;
int failed = 0;

void Check(bool ok, char const* what)
{
    if (ok)
    {
        ++passed;
        return;
    }
    ++failed;
    std::printf("FAILED: %s\n", what);
}

constexpr std::uint32_t SEED = 20261124;

// A small fixed-seed generator (xorshift32): the same cases on every run and every compiler
struct Generator
{
    std::uint32_t state = SEED;

    std::uint32_t Next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float Range(float low, float high) { return low + (high - low) * static_cast<float>(Next() % 10001) / 10000.0f; }
};

// Every value of an enum below its Count has a word of its own, so a new value without a word fails
template <typename Enum>
bool AllWordsDistinct()
{
    constexpr int count = static_cast<int>(Enum::Count);
    for (int i = 0; i < count; ++i)
    {
        char const* word = Word(static_cast<Enum>(i));
        if (!word || !std::strcmp(word, "?"))
            return false;
        for (int j = 0; j < i; ++j)
            if (!std::strcmp(word, Word(static_cast<Enum>(j))))
                return false;
    }
    return !std::strcmp(Word(Enum::Count), "?");
}

template <typename Enum, typename Field>
bool AllStored(Field Seen::* field)
{
    for (int i = 0; i < static_cast<int>(Enum::Count); ++i)
    {
        Seen unit;
        unit.*field = static_cast<Enum>(i);
        if (unit.*field != static_cast<Enum>(i))
            return false;
    }
    return true;
}

Seen At(float x, float y, float radius = 0.0f, float reach = 0.0f)
{
    Seen unit;
    unit.x = x;
    unit.y = y;
    unit.estimateRadius = radius;
    unit.reachYards = reach;
    return unit;
}

// Case 1 and 2: every side, kind, rank, unit type, busy state and way of being known is stored and has its word
void TestValues()
{
    Check(AllWordsDistinct<Side>() && AllStored<Side>(&Seen::side), "values: every side stored, its own word");
    Check(AllWordsDistinct<Kind>() && AllStored<Kind>(&Seen::kind), "values: every kind stored, its own word");
    Check(AllWordsDistinct<Rank>() && AllStored<Rank>(&Seen::rank), "values: every rank stored, its own word");
    Check(AllWordsDistinct<UnitType>() && AllStored<UnitType>(&Seen::type), "values: every unit type stored");
    Check(AllWordsDistinct<Busy>() && AllStored<Busy>(&Seen::busy), "values: every busy state stored, its word");
    Check(AllWordsDistinct<Known>() && AllStored<Known>(&Seen::known), "values: every way of knowing stored");
    Check(AllWordsDistinct<Motion>() && AllStored<Motion>(&Seen::motion), "values: every kind of movement stored");
    Check(AllWordsDistinct<Role>(), "values: every role has its own word");
    Check(AllWordsDistinct<Reason>(), "values: every reason has its own word");

    // This plan's fill: seen now, 0 s ago, at an exact place (AW.4); a fresh entry starts so
    Seen unit;
    Check(unit.known == Known::Seen && unit.ageSeconds == 0.0f && unit.estimateRadius == 0.0f,
          "values: an entry starts seen, 0 s, radius 0");
    Check(unit.fightsGuid == 0 && unit.fightsIndex == NOT_IN_PICTURE && !Fights(unit),
          "values: an entry starts fighting nobody");

    // Only a fighting unit carries a link: a link on any other busy state is not a fight
    for (int i = 0; i < static_cast<int>(Busy::Count); ++i)
    {
        unit.busy = static_cast<Busy>(i);
        unit.fightsGuid = 7;
        unit.fightsIndex = 0;
        Check(Fights(unit) == (unit.busy == Busy::Fighting), "values: only Fighting carries a link");
    }
    unit.busy = Busy::Fighting;
    unit.fightsGuid = 0;
    unit.fightsIndex = NOT_IN_PICTURE;
    Check(!Fights(unit), "values: a fighting unit without a link fights nobody we know");
}

// Case 7: the distance is measured against the disc of the estimate
void TestDisc()
{
    Seen point = At(0.0f, 0.0f);
    Seen disc = At(0.0f, 0.0f, 4.0f);
    Check(DistanceToDisc(point, 10.0f, 0.0f) == 10.0f, "disc: radius 0 is the point distance");
    Check(DistanceToDisc(disc, 10.0f, 0.0f) == 6.0f, "disc: distance minus the radius");
    Check(DistanceToDisc(disc, 3.0f, 0.0f) == 0.0f, "disc: 0 inside the disc");
    Check(DistanceToDisc(disc, 0.0f, 0.0f) == 0.0f, "disc: 0 at the centre, never negative");

    // Reach 10, margin 2: covered at exactly 12 from the disc, not just beyond
    Seen creature = At(0.0f, 0.0f, 0.0f, 10.0f);
    Check(Covers(creature, 12.0f, 0.0f, 2.0f), "disc: covered at reach + margin exactly");
    Check(!Covers(creature, 12.01f, 0.0f, 2.0f), "disc: not covered just beyond reach + margin");
    creature.estimateRadius = 5.0f;
    Check(Covers(creature, 17.0f, 0.0f, 2.0f) && !Covers(creature, 17.01f, 0.0f, 2.0f),
          "disc: a remembered creature (radius 5) covers 5 yd farther");
    Seen player = At(0.0f, 0.0f);
    Check(!Covers(player, 0.0f, 0.0f, 2.0f), "disc: a unit without reach (a player) covers nothing");

    Seen a = At(0.0f, 0.0f, 2.0f);
    Seen b = At(10.0f, 0.0f, 3.0f);
    Check(Within(a, b, 5.0f) && !Within(a, b, 4.99f), "disc: within measures between the two discs");
    Check(Within(a, a, 0.0f), "disc: a unit stands within 0 of itself");

    // Nearer measures against the disc too: a wide estimate centred farther off is the nearer unit
    Seen const wide = At(20.0f, 0.0f, 15.0f);
    Seen const exact = At(10.0f, 0.0f);
    Check(Nearer(wide, exact, 0.0f, 0.0f) && !Nearer(exact, wide, 0.0f, 0.0f),
          "disc: nearer is the smaller distance to the disc (5 yd of a wide estimate before 10 yd exact)");

    // Generated (10f and the §3.4 invariant): a larger radius never turns covers or within from true to false, and
    // the distance only shrinks
    Generator generator;
    int broken = 0;
    for (int i = 0; i < 2000; ++i)
    {
        Seen unit = At(generator.Range(-60.0f, 60.0f), generator.Range(-60.0f, 60.0f), generator.Range(0.0f, 20.0f),
                       generator.Range(0.0f, 30.0f));
        Seen other = At(generator.Range(-60.0f, 60.0f), generator.Range(-60.0f, 60.0f), generator.Range(0.0f, 20.0f));
        float px = generator.Range(-60.0f, 60.0f);
        float py = generator.Range(-60.0f, 60.0f);
        float margin = generator.Range(0.0f, 5.0f);
        float radius = generator.Range(0.0f, 30.0f);
        Seen wider = unit;
        wider.estimateRadius += generator.Range(0.0f, 20.0f);
        float distance = DistanceToDisc(unit, px, py);
        bool ok = distance >= 0.0f && DistanceToDisc(wider, px, py) <= distance;
        ok = ok && (!Covers(unit, px, py, margin) || Covers(wider, px, py, margin));
        ok = ok && (!Within(unit, other, radius) || Within(wider, other, radius));
        ok = ok && Within(unit, other, radius) == Within(other, unit, radius);
        if (!ok)
            ++broken;
    }
    if (broken)
        std::printf("disc: %d broken cases (seed %u)\n", broken, SEED);
    Check(!broken, "disc: 2000 generated cases, a wider estimate never moves a unit away");
}

// Case 8: the age rule on both sides of each limit
void TestAge()
{
    Check(IsFresh(FRESH_IN_FIGHT_MS - 1, true) && IsFresh(FRESH_IN_FIGHT_MS, true) &&
              !IsFresh(FRESH_IN_FIGHT_MS + 1, true),
          "age: in a fight fresh up to its limit, stale beyond");
    Check(IsFresh(FRESH_OUTSIDE_MS - 1, false) && IsFresh(FRESH_OUTSIDE_MS, false) &&
              !IsFresh(FRESH_OUTSIDE_MS + 1, false),
          "age: outside a fight fresh up to its limit, stale beyond");
    Check(!IsFresh(1500, true) && IsFresh(1500, false), "age: 1.5 s is stale in a fight and fresh outside");
    Check(IsFresh(0, true) && IsFresh(0, false), "age: a picture just built is fresh");
}

EntryFacts InLast(Busy busy)
{
    EntryFacts facts;
    facts.inLastPicture = true;
    facts.lastBusy = busy;
    return facts;
}

// Case 9: every reason an enemy is in a fight, and the precedence between them
void TestReasons()
{
    EntryFacts facts = InLast(Busy::Idle);
    Check(WhyInFight(facts) == Reason::Other, "reason: in the last picture, idle, nothing known: other");

    facts.chosen = true;
    Check(WhyInFight(facts) == Reason::Target, "reason: the bot chose it: target");
    facts = InLast(Busy::Idle);
    facts.struckFirst = true;
    Check(WhyInFight(facts) == Reason::Attacker, "reason: it struck first: attacker");
    facts = InLast(Busy::Idle);
    facts.called = true;
    Check(WhyInFight(facts) == Reason::Call, "reason: a fleeing enemy called it: call");
    facts = InLast(Busy::Idle);
    facts.assisted = true;
    Check(WhyInFight(facts) == Reason::Assist, "reason: it stood by an enemy that entered: assist");
    facts = InLast(Busy::Idle);
    facts.reached = true;
    Check(WhyInFight(facts) == Reason::Reach, "reason: the fight came into its reach: reach");
    facts = InLast(Busy::OnPath);
    Check(WhyInFight(facts) == Reason::Path, "reason: it was moving on a path: path");
    facts = EntryFacts();
    Check(WhyInFight(facts) == Reason::New, "reason: not in the last picture: new");

    // Precedence: target > attacker > call > assist > reach > path > new > other
    facts = InLast(Busy::Idle);
    facts.chosen = true;
    facts.struckFirst = true;
    Check(WhyInFight(facts) == Reason::Target, "reason: target before attacker");
    facts = InLast(Busy::Idle);
    facts.struckFirst = true;
    facts.called = true;
    Check(WhyInFight(facts) == Reason::Attacker, "reason: attacker before call");
    facts = InLast(Busy::Idle);
    facts.called = true;
    facts.assisted = true;
    Check(WhyInFight(facts) == Reason::Call, "reason: call before assist");
    facts = InLast(Busy::Idle);
    facts.assisted = true;
    facts.reached = true;
    Check(WhyInFight(facts) == Reason::Assist, "reason: assist before reach");
    facts = InLast(Busy::OnPath);
    facts.reached = true;
    Check(WhyInFight(facts) == Reason::Reach, "reason: reach before path");
    facts = EntryFacts();
    facts.reached = true;
    Check(WhyInFight(facts) == Reason::Reach, "reason: reach before new");
    facts = EntryFacts();
    facts.lastBusy = Busy::OnPath;
    Check(WhyInFight(facts) == Reason::New, "reason: a path needs the last picture, else new");

    // Generated: every combination of the facts gives the first reason of the precedence that holds
    int broken = 0;
    for (int mask = 0; mask < 128; ++mask)
    {
        facts = EntryFacts();
        facts.chosen = mask & 1;
        facts.struckFirst = mask & 2;
        facts.called = mask & 4;
        facts.assisted = mask & 8;
        facts.reached = mask & 16;
        facts.inLastPicture = mask & 32;
        facts.lastBusy = (mask & 64) ? Busy::OnPath : Busy::Idle;
        Reason expected = facts.chosen                     ? Reason::Target
                          : facts.struckFirst              ? Reason::Attacker
                          : facts.called                   ? Reason::Call
                          : facts.assisted                 ? Reason::Assist
                          : facts.reached                  ? Reason::Reach
                          : !facts.inLastPicture           ? Reason::New
                          : facts.lastBusy == Busy::OnPath ? Reason::Path
                                                           : Reason::Other;
        if (WhyInFight(facts) != expected)
            ++broken;
    }
    Check(!broken, "reason: all 128 combinations follow the precedence");
    Check(!std::strcmp(Word(Reason::Target), "target") && !std::strcmp(Word(Reason::New), "new") &&
              !std::strcmp(Word(Reason::Other), "other"),
          "reason: the row words are the plan's");
}

// The bot of the pictures below
constexpr std::uint64_t BOT = 1;

// A unit of this guid and side, fighting the unit of the guid `fights`, or idle for 0
Seen Unit(std::uint64_t guid, Side side, std::uint64_t fights = 0)
{
    Seen unit;
    unit.guid = guid;
    unit.side = side;
    unit.kind = side == Side::Ally ? Kind::Player : Kind::None;
    unit.busy = fights ? Busy::Fighting : Busy::Idle;
    unit.fightsGuid = fights;
    return unit;
}

Seen WithBusy(Seen unit, Busy busy)
{
    unit.busy = busy;
    return unit;
}

// The fill as AW3 runs it: every unit offered, then the links set once
Picture Rebuild(Self const& self, std::vector<Seen> const& units)
{
    Picture picture;
    picture.self = self;
    for (Seen const& unit : units)
        Admit(picture, unit);
    LinkUp(picture);
    return picture;
}

Picture Build(std::vector<Seen> const& units, std::uint64_t botFights = 0)
{
    Self self;
    self.guid = BOT;
    self.fightsGuid = botFights;
    return Rebuild(self, units);
}

std::vector<Seen> UnitsOf(Picture const& picture)
{
    return std::vector<Seen>(picture.units.begin(), picture.units.begin() + picture.count);
}

// A list as a sorted set of guids; 0 for an entry that names no unit
std::vector<std::uint64_t> Guids(Picture const& picture, UnitList const& list)
{
    std::vector<std::uint64_t> guids;
    for (int i = 0; i < list.size; ++i)
    {
        int const ref = list.refs[i];
        if (ref == THE_BOT)
            guids.push_back(picture.self.guid);
        else
            guids.push_back(ref >= 0 && ref < picture.count ? picture.units[ref].guid : 0);
    }
    std::sort(guids.begin(), guids.end());
    return guids;
}

// The lists around the unit of this guid are exactly these guids
bool SidesAre(Picture const& picture, std::uint64_t centre, std::vector<std::uint64_t> beside,
              std::vector<std::uint64_t> against)
{
    std::sort(beside.begin(), beside.end());
    std::sort(against.begin(), against.end());
    Sides sides = SidesAround(picture, SlotOf(picture, centre));
    return Guids(picture, sides.beside) == beside && Guids(picture, sides.against) == against;
}

// No unit's lists name the unit of this guid
bool InNoList(Picture const& picture, std::uint64_t guid)
{
    for (int ref = THE_BOT; ref < picture.count; ++ref)
    {
        if (ref == NOT_IN_PICTURE)
            continue;
        Sides sides = SidesAround(picture, static_cast<std::int16_t>(ref));
        std::vector<std::uint64_t> listed = Guids(picture, sides.beside);
        std::vector<std::uint64_t> against = Guids(picture, sides.against);
        listed.insert(listed.end(), against.begin(), against.end());
        if (std::find(listed.begin(), listed.end(), guid) != listed.end())
            return false;
    }
    return true;
}

// The unit of this guid reads the same lists, as guids, in both pictures
bool SameSides(Picture const& a, Picture const& b, std::uint64_t guid)
{
    Sides sa = SidesAround(a, SlotOf(a, guid));
    Sides sb = SidesAround(b, SlotOf(b, guid));
    return Guids(a, sa.beside) == Guids(b, sb.beside) && Guids(a, sa.against) == Guids(b, sb.against);
}

template <typename Item>
void Shuffle(std::vector<Item>& items, Generator& generator)
{
    for (std::size_t i = items.size(); i > 1; --i)
        std::swap(items[i - 1], items[generator.Next() % i]);
}

// Case 2, the lists: only a fighting unit's link is a fight, and a dead unit is in no list
void TestBusyInLists()
{
    // The bot and E1 fight each other; a dead E2 and units of every other state still name the bot, and A1 still
    // fights the dead E2
    Picture picture =
        Build({Unit(21, Side::Enemy, BOT), WithBusy(Unit(22, Side::Enemy, BOT), Busy::Dead), Unit(11, Side::Ally, 22),
               WithBusy(Unit(23, Side::Enemy, BOT), Busy::Idle), WithBusy(Unit(24, Side::Enemy, BOT), Busy::OnPath),
               WithBusy(Unit(25, Side::Enemy, BOT), Busy::Evading)},
              21);
    Check(SidesAre(picture, BOT, {}, {21}), "busy: only a fighting unit is against the bot");
    Check(SidesAre(picture, 22, {}, {}) && InNoList(picture, 22),
          "busy: a dead unit is in no list, also when a living unit's link names it");
    Check(SidesAre(picture, 11, {}, {}), "busy: a link to a dead unit is no fight");
    Check(InNoList(picture, 23) && InNoList(picture, 24) && InNoList(picture, 25),
          "busy: idle, on a path, evading: a stale link puts nobody in a list");
}

// Case 3: two fights at once, asked from the bot, from allies and from enemies
void TestTwoFights()
{
    // Fight 1: B fights E1, A1 fights E1, E1 fights B, E2 fights A1. Fight 2: A2 and E3 fight each other. Apart: an
    // idle A3, an idle E4, a dead E5 still naming B, E6 fighting a unit outside the picture
    Picture picture =
        Build({Unit(11, Side::Ally, 21), Unit(12, Side::Ally, 23), Unit(13, Side::Ally), Unit(21, Side::Enemy, BOT),
               Unit(22, Side::Enemy, 11), Unit(23, Side::Enemy, 12), Unit(24, Side::Enemy),
               WithBusy(Unit(25, Side::Enemy, BOT), Busy::Dead), Unit(26, Side::Enemy, 999)},
              21);
    Check(SidesAre(picture, BOT, {11}, {21, 22}), "two fights: the bot: beside A1, against E1 E2");
    Check(SidesAre(picture, 11, {BOT}, {21, 22}), "two fights: ally A1: beside the bot, against E1 E2");
    Check(SidesAre(picture, 21, {22}, {BOT, 11}), "two fights: E1: beside E2, against the bot and A1");
    Check(SidesAre(picture, 22, {21}, {BOT, 11}), "two fights: E2: beside E1, against the bot and A1");
    Check(SidesAre(picture, 12, {}, {23}), "two fights: ally A2: against E3 only");
    Check(SidesAre(picture, 23, {}, {12}), "two fights: E3: against A2 only");
    bool apart = true;
    for (std::uint64_t guid : {13, 24, 25, 26})
        apart = apart && SidesAre(picture, guid, {}, {}) && InNoList(picture, guid);
    Check(apart, "two fights: idle, dead and outside-fighting units have no fight and are in no list");
}

// Case 5: players of both factions with helpers, no creature; a helper on another enemy; a ring of three
void TestPlayers()
{
    auto player = [](std::uint64_t guid, Side side, std::uint64_t fights)
    {
        Seen unit = Unit(guid, side, fights);
        unit.type = UnitType::Player;
        unit.playerClass = 1;
        unit.canAttackNow = side == Side::Enemy;
        return unit;
    };
    // The bot P1 and P2, P3 fight Q1; Q1 and Q2 fight P1
    Picture picture = Build({player(12, Side::Ally, 21), player(13, Side::Ally, 21), player(21, Side::Enemy, BOT),
                             player(22, Side::Enemy, BOT)},
                            21);
    bool noCreature = true;
    for (Seen const& unit : UnitsOf(picture))
        noCreature = noCreature && unit.type == UnitType::Player;
    Check(noCreature && SidesAre(picture, BOT, {12, 13}, {21, 22}) && SidesAre(picture, 12, {BOT, 13}, {21, 22}) &&
              SidesAre(picture, 13, {BOT, 12}, {21, 22}),
          "players: P1, P2, P3 read the same two sides");
    Check(SidesAre(picture, 21, {22}, {BOT, 12, 13}) && SidesAre(picture, 22, {21}, {BOT, 12, 13}),
          "players: Q1 and Q2 (whom nobody fights) read the same two sides, swapped");

    // B fights E1, E1 and E2 fight B, ally A fights E2; a stranger S fights E9 apart
    picture = Build({Unit(21, Side::Enemy, BOT), Unit(22, Side::Enemy, BOT), Unit(11, Side::Ally, 22),
                     Unit(31, Side::Neither, 29), Unit(29, Side::Enemy)},
                    21);
    Check(SidesAre(picture, BOT, {11}, {21, 22}),
          "players: an ally fighting another of the bot's enemies is beside it");
    Check(SidesAre(picture, 11, {BOT}, {21, 22}), "players: the helper reads the bot beside it");
    Check(SidesAre(picture, 31, {}, {29}) && SidesAre(picture, 29, {}, {31}),
          "players: a stranger with no link into the bot's fight is in its own fight");

    // A ring of three: P fights C, C fights Q, Q fights P
    picture = Build({Unit(11, Side::Ally, 21), Unit(21, Side::Enemy, 12), Unit(12, Side::Ally, 11)});
    Check(SidesAre(picture, 11, {}, {21, 12}) && SidesAre(picture, 21, {}, {11, 12}) &&
              SidesAre(picture, 12, {}, {11, 21}) && SidesAre(picture, BOT, {}, {}),
          "players: in a ring of three each reads the other two as against, nobody beside");
}

// Case 10(d): rings of every length end, and so do the links that count nowhere
void TestRings()
{
    int broken = 0;
    for (int length = 2; length <= static_cast<int>(PICTURE_SIZE) + 1; ++length)
    {
        // Position 0 is the bot, position p the unit of guid 100 + p; each fights the next, the last the bot
        auto guidAt = [](int p) { return p ? static_cast<std::uint64_t>(100 + p) : BOT; };
        std::vector<Seen> units;
        for (int p = 1; p < length; ++p)
            units.push_back(Unit(guidAt(p), p % 2 ? Side::Enemy : Side::Ally, guidAt((p + 1) % length)));
        Picture picture = Build(units, guidAt(1));
        for (int c = 0; c < length; ++c)
        {
            std::vector<std::uint64_t> beside;
            std::vector<std::uint64_t> against;
            for (int p = 0; p < length; ++p)
            {
                if (p == c)
                    continue;
                int const d = std::abs(p - c);
                (std::min(d, length - d) % 2 ? against : beside).push_back(guidAt(p));
            }
            if (!SidesAre(picture, guidAt(c), beside, against))
                ++broken;
        }
    }
    if (broken)
        std::printf("rings: %d centres read wrong\n", broken);
    Check(!broken, "rings: every length from 2 to 33 ends, each unit by its fewest links");

    // A self link, a link to a dead unit, outside the picture, at the count and below the markers; the bot fights
    // itself
    Picture picture =
        Build({Unit(11, Side::Ally, 11), Unit(12, Side::Ally, 21), WithBusy(Unit(21, Side::Enemy, 12), Busy::Dead),
               Unit(13, Side::Ally, 999), Unit(14, Side::Ally, 998), Unit(15, Side::Ally, 997)},
              BOT);
    picture.units[4].fightsIndex = picture.count;
    picture.units[5].fightsIndex = -7;
    bool none = true;
    for (std::uint64_t guid : std::array<std::uint64_t, 7>{BOT, 11, 12, 13, 14, 15, 21})
        none = none && SidesAre(picture, guid, {}, {});
    Sides past = SidesAround(picture, picture.count);
    Sides below = SidesAround(picture, -7);
    Check(none && !past.beside.size && !past.against.size && !below.beside.size && !below.against.size,
          "rings: links that count nowhere make no fight, and no unit is asked past the count");
}

// Case 10(b): an empty picture
void TestEmpty()
{
    Picture picture = Build({}, 21);
    Sides bot = SidesAround(picture, THE_BOT);
    Sides first = SidesAround(picture, 0);
    Check(!picture.count && !bot.beside.size && !bot.against.size && !first.beside.size && !first.against.size &&
              !picture.leftOut[0] && !picture.leftOut[1] && !picture.leftOut[2] &&
              picture.self.fightsIndex == NOT_IN_PICTURE,
          "empty: empty lists, nothing left out, the bot's link names nobody");
}

// The test's own reading of the links, not the rule's: node i < count is the array's unit i, node count the bot;
// `to` is -1 where a link counts nowhere
struct Graph
{
    int nodes = 0;
    std::array<int, PICTURE_SIZE + 1> to{};
    std::array<bool, PICTURE_SIZE + 1> member{};
    std::array<int, PICTURE_SIZE + 1> component{};
    std::array<bool, PICTURE_SIZE + 1> oddRing{};  // by component
};

int NodeOf(Picture const& picture, int ref)
{
    if (ref == THE_BOT)
        return picture.count;
    if (ref >= 0 && ref < picture.count && picture.units[ref].busy != Busy::Dead)
        return ref;
    return -1;
}

std::int16_t RefOfNode(Picture const& picture, int node)
{
    return node == picture.count ? THE_BOT : static_cast<std::int16_t>(node);
}

std::uint64_t Bit(int node) { return std::uint64_t(1) << node; }

// A list as bits by node; bit 63 for an entry that names no member
std::uint64_t MaskOf(Picture const& picture, UnitList const& list)
{
    std::uint64_t mask = 0;
    for (int i = 0; i < list.size; ++i)
    {
        int const node = NodeOf(picture, list.refs[i]);
        mask |= node < 0 ? Bit(63) : Bit(node);
    }
    return mask;
}

Graph GraphOf(Picture const& picture)
{
    Graph graph;
    int const bot = picture.count;
    graph.nodes = bot + 1;
    for (int u = 0; u < graph.nodes; ++u)
    {
        graph.member[u] = u == bot || picture.units[u].busy != Busy::Dead;
        bool const fighting = u == bot || picture.units[u].busy == Busy::Fighting;
        int const ref = u == bot ? picture.self.fightsIndex : picture.units[u].fightsIndex;
        int const target = graph.member[u] && fighting ? NodeOf(picture, ref) : -1;
        graph.to[u] = target == u ? -1 : target;
        graph.component[u] = u;
    }
    // Components: the lowest label spreads over the links until nothing changes
    for (bool changed = true; changed;)
    {
        changed = false;
        for (int u = 0; u < graph.nodes; ++u)
        {
            int const v = graph.to[u];
            if (v < 0)
                continue;
            int const low = std::min(graph.component[u], graph.component[v]);
            if (graph.component[u] != low || graph.component[v] != low)
            {
                graph.component[u] = low;
                graph.component[v] = low;
                changed = true;
            }
        }
    }
    // An odd ring: following the links from a member comes back to it after an odd number (3 or more) of links
    for (int u = 0; u < graph.nodes; ++u)
    {
        int at = u;
        for (int step = 1; step <= graph.nodes; ++step)
        {
            at = graph.to[at];
            if (at < 0)
                break;
            if (at == u)
            {
                if (step >= 3 && step % 2)
                    graph.oddRing[graph.component[u]] = true;
                break;
            }
        }
    }
    return graph;
}

// Counts of generated asks that broke a sentence of the rule (AW2-design.md section 1)
struct Broken
{
    int lists = 0;    // 1, 2, 4: members only, each once, not the centre, a dead unit or a non-member asked: empty
    int fight = 0;    // 3: the lists hold the centre's linked fight
    int alone = 0;    // 5: no link from or to the centre: empty lists
    int direct = 0;   // 6: whom it fights and who fights it are against it
    int closed = 0;   // 7
    int cross = 0;    // 9(a)
    int swap = 0;     // 9(b)
    int same = 0;     // 9(c)
    int order = 0;    // 10(a)
    int known = 0;    // 10(f)
    int removed = 0;  // 10(c)
    int ringAsks = 0;
    int splitAsks = 0;
};

void CheckCentre(Picture const& picture, Graph const& graph, int x, Broken& broken)
{
    Sides sides = SidesAround(picture, RefOfNode(picture, x));
    if (!graph.member[x])
    {
        if (sides.beside.size || sides.against.size)
            ++broken.lists;
        return;
    }
    std::uint64_t const beside = MaskOf(picture, sides.beside);
    std::uint64_t const against = MaskOf(picture, sides.against);
    if (((beside | against) & (Bit(63) | Bit(x))) || (beside & against) || std::popcount(beside) != sides.beside.size ||
        std::popcount(against) != sides.against.size)
        ++broken.lists;
    bool named = false;
    std::uint64_t component = 0;
    for (int u = 0; u < graph.nodes; ++u)
    {
        named = named || graph.to[u] == x;
        if (graph.member[u] && graph.component[u] == graph.component[x])
            component |= Bit(u);
    }
    if (graph.to[x] < 0 && !named)
    {
        if (beside | against)
            ++broken.alone;
        return;
    }
    std::uint64_t const fight = Bit(x) | beside | against;
    if (fight != component)
        ++broken.fight;
    if (graph.to[x] >= 0 && !(against & Bit(graph.to[x])))
        ++broken.direct;
    for (int u = 0; u < graph.nodes; ++u)
    {
        int const v = graph.to[u];
        if (v < 0)
            continue;
        if (v == x && !(against & Bit(u)))
            ++broken.direct;
        if (!(fight & Bit(u)) != !(fight & Bit(v)))
            ++broken.closed;
    }
    if (graph.oddRing[graph.component[x]])
    {
        ++broken.ringAsks;
        return;
    }
    ++broken.splitAsks;
    std::uint64_t const ours = Bit(x) | beside;
    for (int u = 0; u < graph.nodes; ++u)
    {
        int const v = graph.to[u];
        if (v < 0 || !(fight & Bit(u)))
            continue;
        bool const crosses = ((ours & Bit(u)) && (against & Bit(v))) || ((against & Bit(u)) && (ours & Bit(v)));
        if (!crosses)
            ++broken.cross;
    }
    if (graph.to[x] >= 0)
    {
        int const y = graph.to[x];
        Sides other = SidesAround(picture, RefOfNode(picture, y));
        if ((Bit(y) | MaskOf(picture, other.beside)) != against || MaskOf(picture, other.against) != ours)
            ++broken.swap;
    }
    for (int z = 0; z < graph.nodes; ++z)
    {
        if (!(beside & Bit(z)))
            continue;
        Sides other = SidesAround(picture, RefOfNode(picture, z));
        if ((Bit(z) | MaskOf(picture, other.beside)) != ours || MaskOf(picture, other.against) != against)
            ++broken.same;
    }
}

// Up to 32 units of every side, kind and state; most fight, and a link leads to another unit, the bot, itself,
// outside the picture or nowhere. A third of the pictures holds an odd ring of 3 or 5 built on purpose, with a
// unit hanging on it
Picture RandomPicture(Generator& generator)
{
    int const n = static_cast<int>(generator.Next() % (PICTURE_SIZE + 1));
    std::uint32_t const units32 = static_cast<std::uint32_t>(n);
    auto randomLink = [&generator, units32](std::uint64_t own) -> std::uint64_t
    {
        std::uint32_t const roll = generator.Next() % 20;
        if (roll < 12 && units32)
            return 100 + generator.Next() % units32;
        if (roll < 15)
            return BOT;
        if (roll < 16)
            return own;
        if (roll < 18)
            return 900 + roll;
        return 0;
    };
    std::vector<Seen> units;
    for (int i = 0; i < n; ++i)
    {
        Seen unit = Unit(static_cast<std::uint64_t>(100 + i), static_cast<Side>(generator.Next() % 3));
        unit.kind = unit.side == Side::Ally ? static_cast<Kind>(1 + generator.Next() % 4) : Kind::None;
        unit.type = static_cast<UnitType>(generator.Next() % 3);
        std::uint32_t const roll = generator.Next() % 10;
        unit.busy = roll < 5 ? Busy::Fighting : static_cast<Busy>(roll % 4);
        unit.known = static_cast<Known>(generator.Next() % 3);
        unit.estimateRadius = unit.known == Known::Seen ? 0.0f : generator.Range(0.0f, 20.0f);
        unit.x = generator.Range(-100.0f, 100.0f);
        unit.y = generator.Range(-100.0f, 100.0f);
        unit.fightsGuid = randomLink(unit.guid);
        units.push_back(unit);
    }
    std::uint64_t botFights = randomLink(BOT);
    if (n >= 5 && generator.Next() % 3 == 0)
    {
        std::size_t const length = generator.Next() % 2 ? 3 : 5;
        std::vector<std::uint64_t> ring;
        if (generator.Next() % 2)
            ring.push_back(BOT);
        std::uint32_t const start = generator.Next() % units32;
        std::uint32_t next = 0;
        for (; ring.size() < length; ++next)
            ring.push_back(100 + (start + next) % units32);
        for (std::size_t k = 0; k < length; ++k)
        {
            std::uint64_t const from = ring[k];
            std::uint64_t const to = ring[(k + 1) % length];
            if (from == BOT)
            {
                botFights = to;
                continue;
            }
            units[from - 100].fightsGuid = to;
            units[from - 100].busy = Busy::Fighting;
        }
        Seen& hanger = units[(start + next) % units32];
        if (std::find(ring.begin(), ring.end(), hanger.guid) == ring.end())
        {
            hanger.fightsGuid = ring[0];
            hanger.busy = Busy::Fighting;
        }
    }
    Picture picture = Build(units, botFights);
    // Links that count nowhere by their index: at or past the count, below the markers
    for (int i = 0; i < picture.count; ++i)
    {
        if (picture.units[i].fightsGuid < 900 || generator.Next() % 2)
            continue;
        int const past = picture.count + static_cast<int>(generator.Next() % 8);
        picture.units[i].fightsIndex = static_cast<std::int16_t>(generator.Next() % 2 ? past : -7);
    }
    return picture;
}

// Cases 4, 10(a), 10(c), 10(e), 10(f) for the lists and 10(g) on generated pictures
void TestSidesGenerated()
{
    constexpr std::uint32_t seed = SEED + 2;
    Generator generator;
    generator.state = seed;
    Broken broken;
    for (int round = 0; round < 300; ++round)
    {
        Picture picture = RandomPicture(generator);
        Graph graph = GraphOf(picture);
        for (int x = 0; x < graph.nodes; ++x)
            CheckCentre(picture, graph, x, broken);
        for (int ref : {static_cast<int>(picture.count), static_cast<int>(PICTURE_SIZE), -7})
        {
            Sides sides = SidesAround(picture, static_cast<std::int16_t>(ref));
            if (sides.beside.size || sides.against.size)
                ++broken.lists;
        }

        // Another order of the same units, links set again; and every unit known otherwise, placed elsewhere
        std::vector<Seen> units = UnitsOf(picture);
        std::vector<Seen> shuffled = units;
        Shuffle(shuffled, generator);
        Picture reordered = Rebuild(picture.self, shuffled);
        Picture vague = picture;
        for (int i = 0; i < vague.count; ++i)
        {
            vague.units[i].known = static_cast<Known>((static_cast<int>(vague.units[i].known) + 1) % 3);
            vague.units[i].estimateRadius = generator.Range(0.0f, 20.0f);
            vague.units[i].x += generator.Range(-50.0f, 50.0f);
        }
        for (int x = 0; x < graph.nodes; ++x)
        {
            std::uint64_t const guid = x == picture.count ? BOT : picture.units[x].guid;
            if (!SameSides(picture, reordered, guid))
                ++broken.order;
            if (!SameSides(picture, vague, guid))
                ++broken.known;
            // A unit that is not in this unit's fight leaves the picture
            Sides sides = SidesAround(picture, RefOfNode(picture, x));
            std::uint64_t const fight = Bit(x) | MaskOf(picture, sides.beside) | MaskOf(picture, sides.against);
            std::vector<int> outside;
            for (int i = 0; i < picture.count; ++i)
                if (!(fight & Bit(i)))
                    outside.push_back(i);
            if (outside.empty())
                continue;
            std::vector<Seen> fewer = units;
            fewer.erase(fewer.begin() + outside[generator.Next() % outside.size()]);
            if (!SameSides(picture, Rebuild(picture.self, fewer), guid))
                ++broken.removed;
        }
    }
    std::printf("sides: generated asks in a split fight %d, in a fight with an odd ring %d (seed %u)\n",
                broken.splitAsks, broken.ringAsks, seed);
    Check(broken.splitAsks > 1000 && broken.ringAsks > 100, "sides: the generator asks in both kinds of fight");
    Check(!broken.lists, "sides: lists hold members only, each once, never the centre; no fight for a non-member");
    Check(!broken.fight, "sides: the lists hold the centre's whole linked fight");
    Check(!broken.alone, "sides: a unit with no link from or to it has empty lists");
    Check(!broken.direct, "sides: whom it fights and who fights it are against it");
    Check(!broken.closed, "sides: the fight is closed over the links");
    Check(!broken.cross, "sides: without an odd ring every link crosses between the sides");
    Check(!broken.swap, "sides: without an odd ring the lists swap between a unit and the one it fights");
    Check(!broken.same, "sides: without an odd ring a unit beside reads the same two sides");
    Check(!broken.order, "sides: the lists do not depend on the order of the array");
    Check(!broken.known, "sides: a remembered or inferred unit is listed like a seen one");
    Check(!broken.removed, "sides: removing a unit outside the fight changes no list");
}

// Enemies, allies and neithers in a row from the bot, each side at its own distinct distances; guids 1000 + i,
// 2000 + i and 3000 + i, the lower i the nearer
std::vector<Seen> Row(int enemies, int allies, int neithers)
{
    std::vector<Seen> units;
    int const counts[] = {enemies, allies, neithers};
    for (int side = 0; side < 3; ++side)
    {
        for (int i = 0; i < counts[side]; ++i)
        {
            Seen unit = Unit(static_cast<std::uint64_t>(1000 * (side + 1) + i), static_cast<Side>(side));
            unit.x = 1.0f + static_cast<float>(i) + 0.25f * static_cast<float>(side);
            units.push_back(unit);
        }
    }
    return units;
}

Picture Fill(std::vector<Seen> const& units, std::size_t capacity = PICTURE_SIZE)
{
    Picture picture;
    picture.self.guid = BOT;
    for (Seen const& unit : units)
        Admit(picture, unit, capacity);
    return picture;
}

// The fill kept this many of each side of a Row, each side's nearest, and counted the rest
bool FillIs(Picture const& picture, std::vector<Seen> const& offered, std::array<int, 3> const& kept)
{
    std::array<int, 3> offeredPerSide{};
    for (Seen const& unit : offered)
        ++offeredPerSide[static_cast<std::size_t>(unit.side)];
    std::vector<std::uint64_t> expected;
    for (int side = 0; side < 3; ++side)
        for (int i = 0; i < kept[side]; ++i)
            expected.push_back(static_cast<std::uint64_t>(1000 * (side + 1) + i));
    std::vector<std::uint64_t> guids;
    for (Seen const& unit : UnitsOf(picture))
        guids.push_back(unit.guid);
    std::sort(expected.begin(), expected.end());
    std::sort(guids.begin(), guids.end());
    bool ok = guids == expected;
    for (int side = 0; side < 3; ++side)
        ok = ok && picture.leftOut[side] == offeredPerSide[side] - kept[side];
    return ok;
}

// Case 6: the full array
void TestFill()
{
    struct Case
    {
        char const* what;
        std::vector<Seen> units;
        std::array<int, 3> kept;
    };
    std::vector<Seen> farAllies = Row(40, 0, 0);
    for (int i = 0; i < 3; ++i)
    {
        Seen ally = Unit(static_cast<std::uint64_t>(2000 + i), Side::Ally);
        ally.x = 50.0f + static_cast<float>(i);
        farAllies.push_back(ally);
    }
    std::vector<Case> cases = {
        {"fill (a): 40 enemies: the 32 nearest kept, 8 counted", Row(40, 0, 0), {32, 0, 0}},
        {"fill (b): 20 units of mixed sides: all kept", Row(7, 7, 6), {7, 7, 6}},
        {"fill (c): 40/40/0 offered: 16/16/0 kept", Row(40, 40, 0), {16, 16, 0}},
        {"fill (d): 20/20/20 offered: 11/11/10 kept", Row(20, 20, 20), {11, 11, 10}},
        {"fill (e): 40 enemies, 3 allies farther than all: 29/3/0 kept", farAllies, {29, 3, 0}},
        {"fill (f): 40/40/4 offered: 14/14/4 kept", Row(40, 40, 4), {14, 14, 4}},
        {"fill (g): 20/30/5 offered: 14/13/5 kept, the odd slot to the enemies", Row(20, 30, 5), {14, 13, 5}},
    };
    constexpr std::uint32_t seed = SEED + 3;
    Generator generator;
    generator.state = seed;
    for (Case& fill : cases)
    {
        Check(FillIs(Fill(fill.units), fill.units, fill.kept), fill.what);
        int broken = 0;
        for (int order = 0; order < 100; ++order)
        {
            Shuffle(fill.units, generator);
            if (!FillIs(Fill(fill.units), fill.units, fill.kept))
                ++broken;
        }
        if (broken)
            std::printf("%s: %d of 100 shuffled orders differ (seed %u)\n", fill.what, broken, seed);
        Check(!broken, "fill (h): 100 shuffled arrival orders keep the same units and counts");
    }

    // (i) A full array of 20 enemies and 12 allies; a newcomer of the side with the most units
    Picture picture = Fill(Row(20, 12, 0));
    int const farthest = SlotOf(picture, 1019);
    Seen nearer = Unit(1500, Side::Enemy);
    nearer.x = 5.5f;
    Admit(picture, nearer);
    Check(farthest >= 0 && static_cast<std::size_t>(picture.count) == PICTURE_SIZE &&
              picture.units[farthest].guid == 1500 && SlotOf(picture, 1019) == NOT_IN_PICTURE &&
              picture.leftOut[0] == 1,
          "fill (i): a nearer newcomer takes the slot of its side's farthest, which is counted");
    Picture before = picture;
    Seen farther = Unit(1501, Side::Enemy);
    farther.x = 30.0f;
    Admit(picture, farther);
    bool unchanged = picture.count == before.count;
    for (int i = 0; i < picture.count; ++i)
        unchanged = unchanged && picture.units[i].guid == before.units[i].guid;
    Check(unchanged && picture.leftOut[0] == 2, "fill (i): a farther newcomer is counted and nothing moves");

    // (j) Two enemies at equal distance for the last slot
    std::vector<Seen> tie = Row(31, 0, 0);
    Seen first = Unit(1500, Side::Enemy);
    first.x = 40.0f;
    Seen second = Unit(1501, Side::Enemy);
    second.y = 40.0f;
    tie.push_back(first);
    tie.push_back(second);
    Picture one = Fill(tie);
    std::swap(tie[31], tie[32]);
    Picture other = Fill(tie);
    Check(SlotOf(one, 1500) >= 0 && SlotOf(one, 1501) == NOT_IN_PICTURE && SlotOf(other, 1500) >= 0 &&
              SlotOf(other, 1501) == NOT_IN_PICTURE,
          "fill (j): at equal distance the lower guid is kept, in both arrival orders");

    // (k) The links after the last Admit, in an array of 3: E1000 fights the bot (a stale index first), E1001 fights
    // A2000, A2000 fights E1003, which is left out; the bot fights E1001
    std::vector<Seen> linked = Row(4, 1, 0);
    linked[0].fightsGuid = BOT;
    linked[0].busy = Busy::Fighting;
    linked[0].fightsIndex = 5;
    linked[1].fightsGuid = 2000;
    linked[1].busy = Busy::Fighting;
    linked[4].fightsGuid = 1003;
    linked[4].busy = Busy::Fighting;
    Picture small;
    small.self.guid = BOT;
    small.self.fightsGuid = 1001;
    for (Seen const& unit : linked)
        Admit(small, unit, 3);
    LinkUp(small);
    int const e1000 = SlotOf(small, 1000);
    int const e1001 = SlotOf(small, 1001);
    int const a2000 = SlotOf(small, 2000);
    bool linksOk = small.count == 3 && e1000 >= 0 && e1001 >= 0 && a2000 >= 0 && small.leftOut[0] == 2;
    linksOk = linksOk && small.units[e1000].fightsIndex == THE_BOT && small.units[e1001].fightsIndex == a2000 &&
              small.units[a2000].fightsIndex == NOT_IN_PICTURE && small.self.fightsIndex == e1001;
    Check(linksOk, "fill (k): after LinkUp each link names its unit's slot, the bot, or nobody for a unit left out");
}

// The test's own nearer (the design's sentence 1 at the origin, radius 0): distance, then the lower guid
bool CloserThan(Seen const& a, Seen const& b)
{
    float const da = std::hypot(a.x, a.y);
    float const db = std::hypot(b.x, b.y);
    return da != db ? da < db : a.guid < b.guid;
}

// The design's sentence 2 by a sort: each unit's rank within its side, then rank and side order; the first `size`
std::vector<std::uint64_t> FirstByOrder(std::vector<Seen> units, std::size_t size)
{
    std::sort(units.begin(), units.end(),
              [](Seen const& a, Seen const& b) { return a.side != b.side ? a.side < b.side : CloserThan(a, b); });
    std::vector<std::tuple<int, int, std::uint64_t>> order;
    int rank = 0;
    for (std::size_t i = 0; i < units.size(); ++i)
    {
        rank = i && units[i].side == units[i - 1].side ? rank + 1 : 0;
        order.emplace_back(rank, static_cast<int>(units[i].side), units[i].guid);
    }
    std::sort(order.begin(), order.end());
    std::vector<std::uint64_t> kept;
    for (std::size_t i = 0; i < order.size() && i < size; ++i)
        kept.push_back(std::get<2>(order[i]));
    std::sort(kept.begin(), kept.end());
    return kept;
}

// Case 10(h): fills of 0 to 60 units per side at few places, in array sizes 2, 3, 5, 8 and 32
void TestFillGenerated()
{
    constexpr std::uint32_t seed = SEED + 4;
    Generator generator;
    generator.state = seed;
    int differs = 0;
    int counts = 0;
    int nearest = 0;
    int shares = 0;
    for (std::size_t size : {std::size_t(2), std::size_t(3), std::size_t(5), std::size_t(8), PICTURE_SIZE})
    {
        int const share = static_cast<int>(size / 3);
        for (int trial = 0; trial < 200; ++trial)
        {
            std::vector<Seen> units;
            std::array<int, 3> offered{};
            std::uint64_t guid = 10;
            for (int side = 0; side < 3; ++side)
            {
                offered[side] = static_cast<int>(generator.Next() % 61);
                for (int i = 0; i < offered[side]; ++i)
                {
                    Seen unit = Unit(guid++, static_cast<Side>(side));
                    // Few places, so many units stand at equal distances
                    unit.x = static_cast<float>(static_cast<int>(generator.Next() % 9) - 4);
                    unit.y = static_cast<float>(static_cast<int>(generator.Next() % 9) - 4);
                    units.push_back(unit);
                }
            }
            Shuffle(units, generator);
            Picture picture = Fill(units, size);
            std::vector<std::uint64_t> kept;
            std::array<int, 3> keptPerSide{};
            for (Seen const& unit : UnitsOf(picture))
            {
                kept.push_back(unit.guid);
                ++keptPerSide[static_cast<std::size_t>(unit.side)];
            }
            std::sort(kept.begin(), kept.end());
            if (kept != FirstByOrder(units, size))
                ++differs;
            int const total = offered[0] + offered[1] + offered[2];
            if (picture.count != std::min(total, static_cast<int>(size)))
                ++counts;
            for (int side = 0; side < 3; ++side)
                if (picture.leftOut[side] != offered[side] - keptPerSide[side])
                    ++counts;
            for (Seen const& out : units)
            {
                if (std::binary_search(kept.begin(), kept.end(), out.guid))
                    continue;
                for (Seen const& in : UnitsOf(picture))
                    if (in.side == out.side && CloserThan(out, in))
                        ++nearest;
            }
            for (int side = 0; side < 3; ++side)
            {
                if (offered[side] <= share && keptPerSide[side] != offered[side])
                    ++shares;
                if (keptPerSide[side] == offered[side])
                    continue;
                if (keptPerSide[side] < share)
                    ++shares;
                for (int other = 0; other < 3; ++other)
                    if ((other > side && keptPerSide[side] < keptPerSide[other]) ||
                        (other < side && keptPerSide[side] < keptPerSide[other] - 1))
                        ++shares;
            }
        }
    }
    if (differs || counts || nearest || shares)
        std::printf("fill generated: differs %d, counts %d, nearest %d, shares %d (seed %u)\n", differs, counts,
                    nearest, shares, seed);
    Check(!differs, "fill (10h): any arrival order keeps the first N of the order by rank and side");
    Check(!counts, "fill (10h): count is min(N, offered) and leftOut the units of each side not kept");
    Check(!nearest, "fill (10h): within a side no unit left out is nearer than a kept one");
    Check(!shares, "fill (10h): a small side is kept whole, a side with units left out keeps its share");
}

// A creature hostile to the bot at this place, idle
Seen Hostile(std::uint64_t guid, float x, float y, float radius = 0.0f)
{
    Seen unit = At(x, y, radius);
    unit.guid = guid;
    unit.side = Side::Enemy;
    return unit;
}

// Case 11 (AW4): the place part of who would join a target, the engine's assist radius (10 yd by default) centre
// to centre, for creatures hostile to the bot that are free to answer the call
void TestJoin()
{
    Seen const target = Hostile(1, 0.0f, 0.0f);
    Check(WouldAssistByPlace(Hostile(2, 6.0f, 0.0f), target, 10.0f), "join: a neighbour 6 yd away joins");
    Check(!WouldAssistByPlace(Hostile(2, 14.0f, 0.0f), target, 10.0f), "join: a neighbour 14 yd away does not");
    Check(WouldAssistByPlace(Hostile(2, 0.0f, 10.0f), target, 10.0f), "join: exactly the radius joins");
    Check(!WouldAssistByPlace(Hostile(2, 0.0f, 10.01f), target, 10.0f), "join: just beyond the radius does not");
    Check(WouldAssistByPlace(Hostile(2, 14.0f, 0.0f, 5.0f), target, 10.0f),
          "join: an estimate disc (radius 5) 14 yd away may join: a wider estimate never moves it away");
    Check(!WouldAssistByPlace(target, target, 10.0f), "join: the target never joins itself");
    Check(!WouldAssistByPlace(Hostile(2, 6.0f, 0.0f), target, 0.0f), "join: radius 0 (assistance off) calls nobody");

    Seen const near = Hostile(2, 6.0f, 0.0f);
    Check(WouldAssistByPlace(WithBusy(near, Busy::OnPath), target, 10.0f), "join: a creature on its path joins");
    Check(!WouldAssistByPlace(WithBusy(near, Busy::Dead), target, 10.0f), "join: a dead neighbour never joins");
    Check(!WouldAssistByPlace(WithBusy(near, Busy::Evading), target, 10.0f), "join: an evading neighbour never joins");
    Check(!WouldAssistByPlace(WithBusy(near, Busy::Fighting), target, 10.0f),
          "join: a neighbour that fights already never joins (the engine skips engaged creatures)");
    Seen player = near;
    player.type = UnitType::Player;
    Check(!WouldAssistByPlace(player, target, 10.0f), "join: a player never answers a creature's call");
    Seen pet = near;
    pet.type = UnitType::Pet;
    Check(!WouldAssistByPlace(pet, target, 10.0f), "join: a pet never answers (it has an owner)");
    Seen neither = near;
    neither.side = Side::Neither;
    Check(!WouldAssistByPlace(neither, target, 10.0f), "join: a creature not hostile to the bot never joins");
    Seen ally = near;
    ally.side = Side::Ally;
    Check(!WouldAssistByPlace(ally, target, 10.0f), "join: a creature friendly to the bot never joins");

    // AW12: the engine measures in three dimensions; a neighbour 6 yd away and 12 yd above is 13.4 yd away
    Seen above = near;
    above.z = 12.0f;
    Check(!WouldAssistByPlace(above, target, 10.0f) && WouldAssistByPlace(above, target, 13.5f),
          "join: a neighbour 6 yd away on the map and 12 yd above is 13.4 yd away");

    // Generated: the place test never names a unit that the planar test (Within, the radius widened by the estimate)
    // would not, is Within when both stand at one height, and a wider estimate never drops a joiner
    Generator generator;
    int broken = 0;
    for (int i = 0; i < 2000; ++i)
    {
        Seen centre = Hostile(1, generator.Range(-30.0f, 30.0f), generator.Range(-30.0f, 30.0f));
        Seen const other =
            Hostile(2, generator.Range(-30.0f, 30.0f), generator.Range(-30.0f, 30.0f), generator.Range(0.0f, 10.0f));
        centre.z = generator.Next() % 2 ? 0.0f : generator.Range(-20.0f, 20.0f);
        float const radius = generator.Range(0.5f, 20.0f);
        Seen wider = other;
        wider.estimateRadius += generator.Range(0.0f, 10.0f);
        bool const joins = WouldAssistByPlace(other, centre, radius);
        bool ok = !joins || Within(other, centre, radius);
        ok = ok && (centre.z != other.z || joins == Within(other, centre, radius));
        ok = ok && (!joins || WouldAssistByPlace(wider, centre, radius));
        if (!ok)
            ++broken;
    }
    if (broken)
        std::printf("join: %d broken cases (seed %u)\n", broken, SEED);
    Check(!broken,
          "join: 2000 generated cases, the place test is within Within, Within at one height, and a wider "
          "estimate keeps a joiner");
}

// AW5: how a fight began, from the bot's own attack note and the unit the engine names at the enter edge
void TestBegan()
{
    auto facts =
        [](std::uint64_t first, std::uint64_t noted, Caller caller, std::uint64_t victim, std::uint64_t selection)
    {
        BeginFacts f;
        f.firstEnemy = first;
        f.notedTarget = noted;
        f.notedCaller = caller;
        f.victim = victim;
        f.selection = selection;
        return f;
    };
    Beginning b = HowBegan(facts(7, 7, Caller::Grind, 7, 7));
    Check(b.began == Began::Chosen && b.caller == Caller::Grind && std::strcmp(Word(b), "chosen_grind") == 0,
          "began: the grind's attack on the unit that entered is chosen_grind");
    b = HowBegan(facts(7, 7, Caller::Quest, 0, 7));
    Check(b.began == Began::Chosen && std::strcmp(Word(b), "chosen_quest") == 0,
          "began: a deferred attack (no victim yet, the unit selected) is still the bot's choice");
    b = HowBegan(facts(7, 7, Caller::Other, 7, 0));
    Check(std::strcmp(Word(b), "chosen_other") == 0, "began: another caller's attack is chosen_other");
    b = HowBegan(facts(7, 9, Caller::Grind, 9, 9));
    Check(b.began == Began::Attacked && std::strcmp(Word(b), "attacked") == 0,
          "began: the bot chose 9 but 7 struck first: attacked");
    b = HowBegan(facts(7, 7, Caller::Grind, 8, 8));
    Check(b.began == Began::Attacked, "began: a note of 7 while the bot attacks 8 is stale: attacked");
    b = HowBegan(facts(7, 0, Caller::Grind, 7, 7));
    Check(b.began == Began::Attacked, "began: no note (the bot struck back) is attacked");
    b = HowBegan(facts(0, 0, Caller::Grind, 0, 0));
    Check(b.began == Began::Attacked, "began: no unit named and no note: attacked (0 never matches)");
    Check(CallerOf("attack anything") == Caller::Grind, "caller: attack anything is the grind");
    Check(CallerOf("new rpg attack quest target") == Caller::Quest, "caller: the quest executor's attack");
    Check(CallerOf("dps assist") == Caller::Other && CallerOf("") == Caller::Other, "caller: any other is other");
    Check(KeptCaller(7, Caller::Grind, 7, Caller::Other) == Caller::Grind &&
              KeptCaller(7, Caller::Quest, 7, Caller::Other) == Caller::Quest,
          "kept: melee on the target a chooser noted keeps the chooser");
    Check(KeptCaller(7, Caller::Grind, 9, Caller::Other) == Caller::Other,
          "kept: a new target no chooser picked is other");
    Check(KeptCaller(0, Caller::Grind, 7, Caller::Other) == Caller::Other,
          "kept: after a fight's edge the stale caller does not stick");
    Check(KeptCaller(7, Caller::Grind, 7, Caller::Quest) == Caller::Quest &&
              KeptCaller(7, Caller::Other, 7, Caller::Grind) == Caller::Grind &&
              KeptCaller(7, Caller::Quest, 9, Caller::Grind) == Caller::Grind,
          "kept: a named chooser takes the note");
}

// AW5: the rows' fields packed as the SQL of awareness.sql unpacks them
void TestRows()
{
    SceneRow scene;
    scene.level = 25;
    scene.healthPct = 87;
    scene.powerPct = 40;
    scene.itemLevel = 31;
    scene.roseSeconds = 75;
    scene.group = 2;
    scene.guards = 3;
    scene.allies = 1;
    scene.nearby = 4;
    std::uint32_t id = SceneId(scene);
    std::uint64_t amount = SceneAmount(scene);
    Check((id & 127) == 25 && ((id >> 7) & 127) == 87 && ((id >> 14) & 127) == 40 && ((id >> 21) & 511) == 31,
          "rows: FightScene id = level | health << 7 | power << 14 | item level << 21");
    Check((amount & 32767) == 75 && ((amount >> 15) & 127) == 2 && ((amount >> 22) & 127) == 3 &&
              ((amount >> 29) & 127) == 1 && ((amount >> 36) & 127) == 4,
          "rows: FightScene amount = rose s | group << 15 | guards << 22 | allies << 29 | near << 36");
    scene.level = 200;
    scene.healthPct = 150;
    scene.itemLevel = 600;
    scene.roseSeconds = 50000;
    scene.group = 300;
    id = SceneId(scene);
    amount = SceneAmount(scene);
    Check((id & 127) == 127 && ((id >> 7) & 127) == 100 && ((id >> 21) & 511) == 511 && (id >> 30) == 0,
          "rows: FightScene id fields capped, bits 30 and 31 free");
    Check((amount & 32767) == ROSE_NONE && ((amount >> 15) & 127) == 127 && ((amount >> 22) & 127) == 3,
          "rows: FightScene amount fields capped without spilling into the next");
    scene.powerPct = 250;
    scene.guards = 200;
    scene.allies = 500;
    scene.nearby = 1000;
    id = SceneId(scene);
    amount = SceneAmount(scene);
    Check(((id >> 14) & 127) == 100 && ((id >> 21) & 511) == 511 && ((amount >> 22) & 127) == 127 &&
              ((amount >> 29) & 127) == 127 && ((amount >> 36) & 127) == 127 && (amount >> 43) == 0,
          "rows: FightScene power, guards, allies and near capped; not moving, no leg: nothing above bit 42");
    scene.moving = true;
    amount = SceneAmount(scene);
    Check(((amount >> 36) & 127) == 127 && ((amount >> 43) & 1) == 1 && (amount >> 44) == 0,
          "rows: FightScene moving is bit 43, beside a full near field");
    scene.moving = false;
    scene.onLeg = true;
    amount = SceneAmount(scene);
    Check(((amount >> 43) & 1) == 0 && ((amount >> 44) & 1) == 1 && (amount >> 45) == 0,
          "rows: FightScene a trip leg under way is bit 44, nothing above it");

    EnemyRow enemy;
    enemy.tenths = 21;
    enemy.levelDiff = -3;
    enemy.rank = Rank::Elite;
    enemy.casts = true;
    enemy.creatureType = 7;
    enemy.healthPct = 64;
    enemy.type = UnitType::Pet;
    amount = EnemyAmount(enemy);
    Check((amount & 32767) == 21 && static_cast<int>((amount >> 15) & 127) - 64 == -3 && ((amount >> 22) & 7) == 1 &&
              ((amount >> 25) & 1) == 1 && ((amount >> 26) & 31) == 7 && ((amount >> 31) & 127) == 64 &&
              ((amount >> 38) & 3) == 2,
          "rows: FightEnemy amount = tenths | diff+64 << 15 | rank << 22 | casts << 25 | type << 26 | health << 31 | "
          "unit << 38");
    enemy.levelDiff = -100;
    enemy.tenths = 40000;
    Check(((EnemyAmount(enemy) >> 15) & 127) == 0 && (EnemyAmount(enemy) & 32767) == 32767,
          "rows: FightEnemy level difference and time capped");
    enemy.levelDiff = 100;
    Check(((EnemyAmount(enemy) >> 15) & 127) == 127, "rows: FightEnemy level difference capped above");
    EnemyRow plain;
    plain.creatureType = 40;
    plain.healthPct = 150;
    amount = EnemyAmount(plain);
    Check(((amount >> 25) & 1) == 0 && ((amount >> 26) & 31) == 31 && ((amount >> 31) & 127) == 100 &&
              ((amount >> 38) & 3) == 0 && ((amount >> 40) & 7) == 1 && (amount >> 43) == 0,
          "rows: FightEnemy without casts has no casts bit; creature type and health capped, a standing unit's "
          "motion 1, nothing above bit 42");
    // The motion is stored plus 1, so 0 stays a row of an older build
    bool motions = true;
    for (int i = 0; i < static_cast<int>(Motion::Count); ++i)
    {
        plain.motion = static_cast<Motion>(i);
        amount = EnemyAmount(plain);
        motions = motions && ((amount >> 40) & 7) == static_cast<std::uint64_t>(i) + 1 && (amount >> 43) == 0 &&
                  ((amount >> 38) & 3) == 0;
    }
    Check(motions, "rows: FightEnemy motion + 1 << 40 for every kind, apart from the unit type");
    plain.motion = Motion::Count;
    Check(((EnemyAmount(plain) >> 40) & 7) == static_cast<std::uint64_t>(Motion::Other) + 1,
          "rows: FightEnemy a motion out of range is capped, never 0 or spilling");

    // AW32b: the kind an enemy joins with: live, unless the chase; then the last picture's, when it is fresh
    Seen wandered;
    wandered.motion = Motion::Wandering;
    Check(MotionAtJoin(Motion::Path, &wandered, 0) == Motion::Path &&
              MotionAtJoin(Motion::Standing, &wandered, 0) == Motion::Standing,
          "join: a live kind other than the chase is recorded as it is");
    Check(MotionAtJoin(Motion::Chasing, &wandered, 0) == Motion::Wandering &&
              MotionAtJoin(Motion::Chasing, &wandered, FRESH_OUTSIDE_MS) == Motion::Wandering,
          "join: a chaser the fresh picture held keeps the picture's kind (the limit is still fresh)");
    Check(MotionAtJoin(Motion::Chasing, &wandered, FRESH_OUTSIDE_MS + 1) == Motion::Chasing,
          "join: a stale picture does not say how it moved");
    Check(MotionAtJoin(Motion::Chasing, nullptr, 0) == Motion::Chasing,
          "join: a chaser the picture did not hold is chasing");
    wandered.motion = Motion::Chasing;
    Check(MotionAtJoin(Motion::Chasing, &wandered, 0) == Motion::Chasing,
          "join: a unit chasing in the picture already is chasing");

    DeathRow death;
    death.killerLevelDiff = 2;
    death.killerRank = Rank::Rare;
    death.killerType = KILLER_NONE;
    death.enemies = 3;
    death.roseSeconds = 12;
    death.roseYards = 30;
    death.helperYards = 15;
    amount = DeathAmount(death);
    Check(static_cast<int>(amount & 127) - 64 == 2 && ((amount >> 7) & 7) == 2 && ((amount >> 10) & 127) == 3 &&
              ((amount >> 17) & 32767) == 12 && ((amount >> 32) & 1023) == 30 && ((amount >> 42) & 255) == 15 &&
              ((amount >> 50) & 3) == KILLER_NONE,
          "rows: DeathScene amount = diff+64 | rank << 7 | enemies << 10 | rose s << 17 | rose yd << 32 | helper yd << "
          "42 | killer << 50");
    death.roseYards = 5000;
    death.helperYards = 999;
    amount = DeathAmount(death);
    Check(((amount >> 32) & 1023) == YARDS_NONE && ((amount >> 42) & 255) == HELPER_NONE,
          "rows: DeathScene yards capped at their none values");
    death.killerType = static_cast<std::uint32_t>(UnitType::Pet);
    death.enemies = 300;
    death.roseSeconds = 50000;
    amount = DeathAmount(death);
    Check(((amount >> 50) & 3) == 2 && ((amount >> 10) & 127) == 127 && ((amount >> 17) & 32767) == ROSE_NONE &&
              ((amount >> 32) & 1023) == YARDS_NONE && (amount >> 52) == 0,
          "rows: DeathScene killer type stored (2 a pet); enemies and rose seconds capped, nothing above bit 51");
}

// AW5: the FightScene's counts and the DeathScene's nearest helper, read off the picture
void TestScene()
{
    auto at = [](Seen unit, float x, float y, float reach = 0.0f)
    {
        unit.x = x;
        unit.y = y;
        unit.reachYards = reach;
        return unit;
    };
    auto kind = [](Seen unit, Kind k, UnitType type = UnitType::Player, std::uint64_t owner = 0)
    {
        unit.kind = k;
        unit.type = type;
        unit.ownerGuid = owner;
        return unit;
    };
    std::vector<Seen> const units = {
        at(Unit(101, Side::Enemy, BOT), 5.0f, 0.0f, 10.0f),   // the target, fights the bot
        at(Unit(102, Side::Enemy), 12.0f, 0.0f, 10.0f),       // covers the target (7 yd): near
        at(Unit(103, Side::Enemy), 40.0f, 0.0f, 10.0f),       // covers neither
        at(Unit(104, Side::Enemy), -8.0f, 0.0f, 10.0f),       // covers the bot: near
        at(Unit(117, Side::Enemy, BOT), 30.0f, 30.0f, 0.0f),  // fights the bot, no reach
        kind(at(Unit(110, Side::Ally), 3.0f, 3.0f), Kind::Group),
        kind(at(Unit(111, Side::Ally), 50.0f, 0.0f), Kind::Guard, UnitType::Creature),
        at(Unit(112, Side::Ally, 101), 6.0f, 1.0f),    // fights the target
        at(Unit(113, Side::Ally), 2.0f, 0.0f),         // a stranger that fights nobody
        at(Unit(116, Side::Ally, 117), 30.0f, 28.0f),  // fights an enemy that fights the bot
        kind(at(Unit(114, Side::Ally), 1.0f, 0.0f), Kind::Group, UnitType::Pet, BOT),  // the bot's own pet
        kind(at(Unit(115, Side::Ally), 4.0f, 0.0f), Kind::Group, UnitType::Pet, 110),  // a group member's pet
    };
    Picture const picture = Build(units, 101);
    SceneCounts const counts = CountScene(picture, 101);
    Check(counts.group == 2 && counts.guards == 1 && counts.allies == 2 && counts.nearby == 2,
          "scene: group 2 (a member and its pet, not the bot's own pet), guards 1, allies that fight the bot's "
          "enemies 2, enemies whose reach covers the bot or the target 2");
    SceneCounts const none = CountScene(Build({}), 101);
    Check(none.group == 0 && none.guards == 0 && none.allies == 0 && none.nearby == 0, "scene: an empty picture");
    SceneCounts const noTarget = CountScene(picture, 199);
    Check(noTarget.nearby == 2 && noTarget.allies == 2,
          "scene: a target not in the picture: only the bot's place counts (1 and 4 cover it, 2 does not); the "
          "allies still fight enemies that fight the bot");

    std::array<std::uint64_t, 1> const enemies = {101};
    Helper helper = NearestHelper(picture, enemies.data(), enemies.size());
    Check(helper.found && helper.kind == Kind::Group && std::fabs(helper.yards - 4.0f) < 0.01f &&
              std::strcmp(Word(helper), "group") == 0,
          "helper: the nearest is the group member's pet 4 yd away (not the bot's own pet at 1 yd, not the stranger "
          "at 2 yd)");
    std::vector<Seen> rest;
    for (Seen const& unit : units)
        if (unit.kind != Kind::Group)
            rest.push_back(unit);
    helper = NearestHelper(Build(rest, 101), enemies.data(), enemies.size());
    Check(helper.found && helper.kind == Kind::Player && std::fabs(helper.yards - std::hypot(6.0f, 1.0f)) < 0.01f &&
              std::strcmp(Word(helper), "ally") == 0,
          "helper: without the group the ally that fights the target, 6.1 yd");
    std::vector<Seen> guardOnly = {at(Unit(101, Side::Enemy, BOT), 5.0f, 0.0f, 10.0f),
                                   kind(at(Unit(111, Side::Ally), 50.0f, 0.0f), Kind::Guard, UnitType::Creature)};
    helper = NearestHelper(Build(guardOnly, 101), enemies.data(), enemies.size());
    Check(helper.found && helper.kind == Kind::Guard && std::strcmp(Word(helper), "guard") == 0,
          "helper: a guard 50 yd away");
    helper = NearestHelper(Build({at(Unit(113, Side::Ally), 2.0f, 0.0f)}), enemies.data(), enemies.size());
    Check(!helper.found && std::strcmp(Word(helper), "none") == 0, "helper: a stranger that fights nobody is none");

    // Who helps and what is near, case by case; here the target 201 fights the ally 210 and nobody fights the bot
    std::vector<Seen> const cases = {
        at(Unit(201, Side::Enemy, 210), 5.0f, 0.0f, 10.0f),                     // the target
        at(Unit(210, Side::Ally, 201), 6.0f, 0.0f),                             // fights the target: helps
        at(Unit(211, Side::Enemy, 201), 7.0f, 0.0f),                            // an enemy that fights the target
        at(WithBusy(Unit(212, Side::Ally, 201), Busy::Idle), 8.0f, 0.0f),       // a stale link, it fights nobody
        at(WithBusy(Unit(202, Side::Enemy, BOT), Busy::Evading), 30.0f, 0.0f),  // evading: its link is no fight
        at(Unit(213, Side::Ally, 202), 31.0f, 0.0f),                            // fights the evading enemy
        at(WithBusy(Unit(203, Side::Enemy), Busy::Dead), -3.0f, 0.0f, 10.0f),   // dead, its reach would cover the bot
    };
    Picture const second = Build(cases);
    SceneCounts const strict = CountScene(second, 201);
    Check(strict.group == 0 && strict.guards == 0 && strict.allies == 1 && strict.nearby == 0,
          "scene: only the ally that fights the target helps (not an enemy that fights it, an ally with a stale "
          "link or an ally whose enemy is evading); a dead enemy in reach is not near");
    std::array<std::uint64_t, 1> const listed = {201};
    helper = NearestHelper(second, listed.data(), listed.size());
    Check(helper.found && helper.kind == Kind::Player && std::fabs(helper.yards - 6.0f) < 0.01f,
          "helper: the ally that fights a listed enemy which does not fight the bot, 6 yd");
}

// AW7: the point judgement and the safe-point search (INTERFACE.md §3.2, §3.3). Each generated invariant searches a
// counterexample over at least INVARIANT_CASES pictures and prints its name and its number of cases
constexpr int INVARIANT_CASES = 10000;

void Report(char const* name, int cases, int counterexamples, std::uint32_t seed)
{
    std::printf("AW7 invariant %s: %d cases, %d counterexamples (seed %u)\n", name, cases, counterexamples, seed);
    Check(cases >= INVARIANT_CASES && counterexamples == 0, name);
}

// A creature's strength by the rule (AW12), at this level difference and health
std::uint32_t CreatureStrength(Rank rank, int levelDiff, float healthPct = 100.0f)
{
    return Strength(STRENGTH.rank[static_cast<std::size_t>(rank)], levelDiff, healthPct);
}

constexpr std::size_t ENEMY = static_cast<std::size_t>(Side::Enemy);

// A unit of a generated picture: an enemy creature in any busy state, 5 levels below to 8 above, any rank, reach 0
// (one in eight) or 5 to 45, an estimate radius of 0 to 20 (half of them 0), a home up to 30 yd from its place, and
// on a path half of them with a velocity of up to 4 yd/s per axis
Seen RandomEnemy(Generator& generator, std::uint64_t guid)
{
    Seen unit;
    unit.guid = guid;
    unit.side = Side::Enemy;
    unit.type = UnitType::Creature;
    unit.busy = static_cast<Busy>(generator.Next() % static_cast<std::uint32_t>(Busy::Count));
    unit.levelDiff = static_cast<std::int16_t>(static_cast<int>(generator.Next() % 14) - 5);
    unit.rank = generator.Next() % 4 == 0 ? static_cast<Rank>(1 + generator.Next() % 3) : Rank::Normal;
    unit.reachYards = generator.Next() % 8 == 0 ? 0.0f : generator.Range(5.0f, 45.0f);
    unit.estimateRadius = generator.Next() % 2 ? 0.0f : generator.Range(0.0f, 20.0f);
    unit.x = generator.Range(-90.0f, 90.0f);
    unit.y = generator.Range(-90.0f, 90.0f);
    unit.homeX = unit.x + generator.Range(-30.0f, 30.0f);
    unit.homeY = unit.y + generator.Range(-30.0f, 30.0f);
    if (unit.busy == Busy::OnPath && generator.Next() % 2)
    {
        unit.moveX = generator.Range(-4.0f, 4.0f);
        unit.moveY = generator.Range(-4.0f, 4.0f);
    }
    return unit;
}

// 0 to 12 enemies
std::vector<Seen> RandomEnemies(Generator& generator, std::uint64_t& guid)
{
    std::vector<Seen> units;
    int const count = static_cast<int>(generator.Next() % 13);
    for (int i = 0; i < count; ++i)
        units.push_back(RandomEnemy(generator, guid++));
    return units;
}

// The bot at 0 0; the units as given (no Admit: a generated picture may hold what a fill would not keep)
Picture Pictured(std::vector<Seen> const& units, std::uint16_t enemiesLeftOut = 0)
{
    Picture picture;
    picture.self.guid = BOT;
    picture.self.level = 20;
    for (Seen const& unit : units)
        picture.units[picture.count++] = unit;
    picture.leftOut[ENEMY] = enemiesLeftOut;
    return picture;
}

std::uint16_t RandomLeftOut(Generator& generator)
{
    return generator.Next() % 4 == 0 ? static_cast<std::uint16_t>(1 + generator.Next() % 5) : 0;
}

// Never safer: reached stays reached, the clearance does not rise, the count does not fall
bool NoSafer(PointAnswer const& before, PointAnswer const& after)
{
    return (before.clear || !after.clear) && after.clearance <= before.clearance + 1e-3f &&
           after.reaching >= before.reaching;
}

// The test's own reckoning of a unit's yards outside its reach (double, the segment by projection)
double ReferenceOver(Seen const& unit, double x, double y)
{
    double gx = unit.x;
    double gy = unit.y;
    if (unit.busy == Busy::Evading)
    {
        gx = unit.homeX;
        gy = unit.homeY;
    }
    else if (unit.busy == Busy::OnPath && (unit.moveX != 0.0f || unit.moveY != 0.0f))
    {
        double const dx = double(unit.moveX) * PATH_AHEAD_SECONDS;
        double const dy = double(unit.moveY) * PATH_AHEAD_SECONDS;
        double const t = std::clamp(((x - gx) * dx + (y - gy) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
        gx += t * dx;
        gy += t * dy;
    }
    return std::max(0.0, std::hypot(x - gx, y - gy) - unit.estimateRadius) - unit.reachYards;
}

bool Counting(Seen const& unit)
{
    return unit.side == Side::Enemy && unit.type == UnitType::Creature && unit.reachYards > 0.0f &&
           unit.busy != Busy::Dead;
}

void TestPointFixed()
{
    // One idle creature at 30 0 with a reach of 10
    Seen idle = Hostile(301, 30.0f, 0.0f);
    idle.reachYards = 10.0f;
    PointAnswer answer = JudgePoint(Pictured({idle}), 30.0f, 0.0f);
    Check(!answer.clear && answer.reaching == 1 && answer.nearest == 301 &&
              std::fabs(answer.clearance + 10.0f) < 1e-4f && answer.inView,
          "point: its own place is reached, clearance -10, in view");
    answer = JudgePoint(Pictured({idle}), 43.0f, 0.0f);
    Check(!answer.clear && std::fabs(answer.clearance - 3.0f) < 1e-4f, "point: 3 yd outside the reach is reached");
    answer = JudgePoint(Pictured({idle}), 43.5f, 0.0f);
    Check(answer.clear && answer.reaching == 0 && answer.nearest == 0 && std::fabs(answer.clearance - 3.5f) < 1e-4f,
          "point: 3.5 yd outside the reach is clear");
    answer = JudgePoint(Pictured({idle}), 55.0f, 0.0f);
    Check(answer.clear && std::fabs(answer.clearance - 15.0f) < 1e-4f, "point: 15 yd outside every reach, clear");

    // Evading: its home counts, not its place
    Seen evading = WithBusy(idle, Busy::Evading);
    evading.homeX = 60.0f;
    Check(JudgePoint(Pictured({evading}), 30.0f, 0.0f).clear, "point: an evading creature's place on the way is clear");
    Check(!JudgePoint(Pictured({evading}), 60.0f, 0.0f).clear, "point: an evading creature's home is reached");
    Seen evadingNoReach = evading;
    evadingNoReach.reachYards = 0.0f;
    Check(JudgePoint(Pictured({evadingNoReach}), 60.0f, 0.0f).clear, "point: reach 0 (a civilian) reaches nothing");

    // On a path at 0 30 going +x at 1 yd/s: 10 s ahead is the segment to 10 30
    Seen path = WithBusy(Hostile(302, 0.0f, 30.0f), Busy::OnPath);
    path.reachYards = 10.0f;
    path.moveX = 1.0f;
    Check(!JudgePoint(Pictured({path}), 22.0f, 30.0f).clear, "point: 12 yd ahead of a path creature is reached");
    Check(JudgePoint(Pictured({WithBusy(path, Busy::Idle)}), 22.0f, 30.0f).clear,
          "point: the same place beside an idle creature is clear");
    Check(JudgePoint(Pictured({path}), -14.0f, 30.0f).clear, "point: 14 yd behind a path creature is clear");
    Check(!JudgePoint(Pictured({path}), -5.0f, 30.0f).clear, "point: 5 yd behind it, inside its reach, is reached");

    // Not counted: an ally, a player, a pet, the side neither, a dead creature
    Seen ally = idle;
    ally.side = Side::Ally;
    Seen player = idle;
    player.type = UnitType::Player;
    Seen pet = idle;
    pet.type = UnitType::Pet;
    Seen neither = idle;
    neither.side = Side::Neither;
    Seen dead = WithBusy(idle, Busy::Dead);
    for (Seen const& unit : {ally, player, pet, neither, dead})
    {
        PointAnswer const none = JudgePoint(Pictured({unit}), 30.0f, 0.0f);
        Check(none.clear && none.reaching == 0 && none.clearance == CLEARANCE_NONE,
              "point: a unit that does not count");
    }

    // In view: 80 yd off with the unseen reach of 25 is not (80 + 25 + 3 > 100), 72 is; a reach of 40 widens it
    Check(!JudgePoint(Pictured({}), 80.0f, 0.0f).inView && JudgePoint(Pictured({}), 72.0f, 0.0f).inView,
          "point: 80 yd is not in view, 72 yd is");
    Seen wide = Hostile(303, -50.0f, 0.0f);
    wide.reachYards = 40.0f;
    Check(JudgePoint(Pictured({wide}), 57.0f, 0.0f).inView && !JudgePoint(Pictured({wide}), 58.0f, 0.0f).inView,
          "point: the largest reach of the picture widens the unseen reach");
    Seen far = Hostile(304, 0.0f, 60.0f);
    far.reachYards = 10.0f;
    Check(JudgePoint(Pictured({far}, 1), 0.0f, -32.0f).inView && !JudgePoint(Pictured({far}, 1), 0.0f, -33.0f).inView,
          "point: with enemies left out the view ends at the farthest kept enemy (60 yd)");

    // The record's nearby count skips an evading creature whose reach (the one it has at home) covers the bot
    Seen home = Hostile(307, 4.0f, 0.0f);
    home.reachYards = 10.0f;
    Check(CountScene(Build({home}), 999).nearby == 1 &&
              CountScene(Build({WithBusy(home, Busy::Evading)}), 999).nearby == 0,
          "scene: an evading creature in reach is not nearby");

    // Takeable: one ordinary creature of the bot's level or below
    Check(Takeable(JudgePoint(Pictured({idle}), 30.0f, 0.0f)), "point: one ordinary creature is takeable");
    Seen above = idle;
    above.levelDiff = 1;
    Seen elite = idle;
    elite.rank = Rank::Elite;
    Seen second = Hostile(305, 32.0f, 0.0f);
    second.reachYards = 10.0f;
    Check(!Takeable(JudgePoint(Pictured({above}), 30.0f, 0.0f)) &&
              !Takeable(JudgePoint(Pictured({elite}), 30.0f, 0.0f)) &&
              !Takeable(JudgePoint(Pictured({idle, second}), 30.0f, 0.0f)) &&
              !Takeable(JudgePoint(Pictured({}), 30.0f, 0.0f)),
          "point: a level above, an elite, two units or none is not takeable");

    // The rings: the death return's radius 31 holds the centre and 4 rings, 61 points
    std::array<Spot, CANDIDATES_MAX> spots{};
    std::size_t const count = RingSpots(5.0f, -5.0f, 31.0f, spots.data(), spots.size());
    bool rings = count == 61 && spots[0].x == 5.0f && spots[0].y == -5.0f && std::fabs(spots[1].x - 12.75f) < 1e-4f &&
                 std::fabs(spots[1].y + 5.0f) < 1e-4f;
    std::size_t index = 1;
    for (int k = 1; k <= 4; ++k)
        for (int j = 0; j < 6 * k; ++j, ++index)
            rings = rings && index < count &&
                    std::fabs(PlanarDistance(spots[index].x, spots[index].y, 5.0f, -5.0f) - 7.75f * k) < 1e-3f;
    Check(rings, "safe: radius 31 gives the centre and rings of 6, 12, 18, 24 points every 7.75 yd from angle 0");
    Check(RingSpots(0.0f, 0.0f, 7.7f, spots.data(), spots.size()) == 1 &&
              RingSpots(0.0f, 0.0f, 7.75f, spots.data(), spots.size()) == 7 &&
              RingSpots(0.0f, 0.0f, 31.0f, spots.data(), 20) == 19 &&
              RingSpots(0.0f, 0.0f, 500.0f, spots.data(), spots.size()) == CANDIDATES_MAX,
          "safe: a ring is whole or left out; at most the space given");

    // The search: the centre reached, the clear ring point nearest the bot wins
    Seen camp = Hostile(306, 0.0f, 0.0f);
    camp.reachYards = 12.0f;
    std::size_t const n = RingSpots(0.0f, 0.0f, 31.0f, spots.data(), spots.size());
    auto always = [](std::size_t, bool) { return WalkAnswer{Walk::Yes, true, 2.0f}; };
    SafePoint safe = FindSafePoint(Pictured({camp}), 0.0f, -40.0f, spots.data(), n, always);
    Check(safe.found && safe.clear && safe.answer.clearance >= COMFORT_YARDS && std::fabs(safe.x) < 1e-3f &&
              std::fabs(safe.y + 31.0f) < 1e-3f && safe.z == 2.0f && safe.tests == 1,
          "safe: in a camp the clear point nearest the bot wins, z from the walkable test, one test");
    auto never = [](std::size_t, bool mayTest)
    { return WalkAnswer{mayTest ? Walk::No : Walk::Unknown, mayTest, 0.0f}; };
    safe = FindSafePoint(Pictured({camp}), 0.0f, -40.0f, spots.data(), n, never);
    Check(!safe.found && safe.outOfTests && safe.tests == WALK_TESTS_MAX, "safe: no walkable point found in 8 tests");

    // AW7 review (mutants that survived): the fields of the reaching units, the view's edge, Takeable's view, and a
    // call out of tests stops at its first untested candidate even when a later one has a kept result
    Seen low = Hostile(401, 30.0f, 0.0f);
    low.reachYards = 10.0f;
    low.levelDiff = -2;
    Seen high = Hostile(402, 36.0f, 0.0f);
    high.reachYards = 10.0f;
    high.levelDiff = 3;
    high.rank = Rank::Elite;
    answer = JudgePoint(Pictured({high, low}), 30.0f, 0.0f);
    Check(answer.reaching == 2 && answer.nearest == 401 && answer.topLevelDiff == 3 && answer.topRank == Rank::Elite,
          "point: nearest is the smallest over; top level and rank are the highest of the reaching units");
    Check(JudgePoint(Pictured({low}), 30.0f, 0.0f).topLevelDiff == -2,
          "point: one reaching unit 2 levels below gives its own level difference, not 0");
    Seen twinA = Hostile(404, 30.0f, 5.0f);
    twinA.reachYards = 10.0f;
    Seen twinB = Hostile(403, 30.0f, -5.0f);
    twinB.reachYards = 10.0f;
    Check(JudgePoint(Pictured({twinA, twinB}), 30.0f, 0.0f).nearest == 403, "point: at equal over the lower guid");
    Seen wideFar = Hostile(405, 0.0f, 60.0f, 10.0f);
    wideFar.reachYards = 10.0f;
    Check(JudgePoint(Pictured({wideFar}, 1), 0.0f, -22.0f).inView &&
              !JudgePoint(Pictured({wideFar}, 1), 0.0f, -23.0f).inView,
          "point: with enemies left out the view ends at the farthest kept enemy's disc (60 - 10 yd)");
    Seen beyond = Hostile(406, 80.0f, 0.0f);
    beyond.reachYards = 10.0f;
    answer = JudgePoint(Pictured({beyond}), 80.0f, 0.0f);
    Check(answer.reaching == 1 && !answer.inView && !Takeable(answer), "point: one unit out of view is not takeable");
    std::size_t const lastSpot = n - 1;
    auto keptLast = [lastSpot](std::size_t index, bool)
    { return index == lastSpot ? WalkAnswer{Walk::Yes, false, 1.0f} : WalkAnswer{}; };
    safe = FindSafePoint(Pictured({}), 0.0f, 0.0f, spots.data(), n, keptLast, 0);
    Check(!safe.found && safe.outOfTests && safe.tests == 0,
          "safe: out of tests the call stops at the first untested candidate, not at a later kept one");
}

// The units, the point and what one change makes of the answer
struct PointCase
{
    std::vector<Seen> units;
    std::uint16_t leftOut = 0;
    float x = 0.0f;
    float y = 0.0f;

    PointAnswer Judge() const { return JudgePoint(Pictured(units, leftOut), x, y); }
};

PointCase RandomPointCase(Generator& generator, std::uint64_t& guid)
{
    PointCase c;
    c.units = RandomEnemies(generator, guid);
    c.leftOut = RandomLeftOut(generator);
    c.x = generator.Range(-100.0f, 100.0f);
    c.y = generator.Range(-100.0f, 100.0f);
    return c;
}

void TestPointGenerated()
{
    constexpr std::uint32_t seed = SEED + 7;
    Generator generator;
    generator.state = seed;
    std::uint64_t guid = 1000;

    int bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        PointCase c = RandomPointCase(generator, guid);
        PointAnswer const before = c.Judge();
        c.units.push_back(RandomEnemy(generator, guid++));
        PointAnswer const after = c.Judge();
        // While enemies are left out, a kept one is never farther than they are (Admit): a unit added farther would
        // be a picture no fill makes, so the view is compared only on whole pictures
        bad += !NoSafer(before, after) || (c.leftOut == 0 && after.inView && !before.inView);
    }
    Report("point-one-more-unit", INVARIANT_CASES, bad, seed);

    int cases = 0;
    bad = 0;
    while (cases < INVARIANT_CASES)
    {
        PointCase c = RandomPointCase(generator, guid);
        if (c.units.empty())
            continue;
        ++cases;
        PointAnswer const before = c.Judge();
        c.units[generator.Next() % c.units.size()].reachYards += generator.Range(0.1f, 20.0f);
        PointAnswer const after = c.Judge();
        bad += !NoSafer(before, after) || (after.inView && !before.inView);
    }
    Report("point-larger-reach", cases, bad, seed);

    cases = 0;
    bad = 0;
    while (cases < INVARIANT_CASES)
    {
        PointCase c = RandomPointCase(generator, guid);
        if (c.units.empty())
            continue;
        ++cases;
        PointAnswer const before = c.Judge();
        c.units[generator.Next() % c.units.size()].estimateRadius += generator.Range(0.1f, 20.0f);
        bad += !NoSafer(before, c.Judge());
    }
    Report("point-larger-estimate", cases, bad, seed);

    cases = 0;
    bad = 0;
    while (cases < INVARIANT_CASES)
    {
        PointCase c = RandomPointCase(generator, guid);
        if (c.units.empty())
            continue;
        ++cases;
        Seen& unit = c.units[generator.Next() % c.units.size()];
        unit.busy = Busy::OnPath;
        unit.moveX = generator.Range(-4.0f, 4.0f);
        unit.moveY = generator.Range(-4.0f, 4.0f);
        PointAnswer const before = c.Judge();
        float const scale = generator.Range(1.0f, 3.0f);
        unit.moveX *= scale;
        unit.moveY *= scale;
        bad += !NoSafer(before, c.Judge());
    }
    Report("point-longer-velocity", cases, bad, seed);

    // A unit that does not count changes no field: an ally, the side neither, a player, a pet, reach 0, dead
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        PointCase c = RandomPointCase(generator, guid);
        PointAnswer const before = c.Judge();
        Seen unit = RandomEnemy(generator, guid++);
        unit.reachYards = generator.Range(5.0f, 45.0f);
        switch (i % 6)
        {
            case 0:
                unit.side = Side::Ally;
                break;
            case 1:
                unit.side = Side::Neither;
                break;
            case 2:
                unit.type = UnitType::Player;
                break;
            case 3:
                unit.type = UnitType::Pet;
                break;
            case 4:
                unit.reachYards = 0.0f;
                break;
            default:
                unit.busy = Busy::Dead;
                break;
        }
        // An enemy of the picture marks where the view ends while enemies are left out: put it at the bot
        if (unit.side == Side::Enemy && c.leftOut > 0)
            unit.x = unit.y = 0.0f;
        c.units.insert(c.units.begin() + generator.Next() % (c.units.size() + 1), unit);
        bad += !(c.Judge() == before);
    }
    Report("point-not-counting", INVARIANT_CASES, bad, seed);

    cases = 0;
    bad = 0;
    while (cases < INVARIANT_CASES)
    {
        PointCase c = RandomPointCase(generator, guid);
        if (c.units.empty())
            continue;
        ++cases;
        Seen& unit = c.units[generator.Next() % c.units.size()];
        unit.busy = Busy::Evading;
        // The view's end is the farthest kept enemy's place while enemies are left out (a fact of the picture, not of
        // reach); the invariant is about who reaches, so on whole pictures
        c.leftOut = 0;
        PointAnswer const before = c.Judge();
        unit.x = generator.Range(-90.0f, 90.0f);
        unit.y = generator.Range(-90.0f, 90.0f);
        bad += !(c.Judge() == before);
    }
    Report("point-evading-place", cases, bad, seed);

    cases = 0;
    bad = 0;
    while (cases < INVARIANT_CASES)
    {
        PointCase c = RandomPointCase(generator, guid);
        if (c.units.empty())
            continue;
        ++cases;
        Seen& unit = c.units[generator.Next() % c.units.size()];
        unit.busy = Busy::OnPath;
        unit.moveX = 0.0f;
        unit.moveY = 0.0f;
        PointAnswer const onPath = c.Judge();
        unit.busy = Busy::Idle;
        bad += !(c.Judge() == onPath);
    }
    Report("point-path-standing", cases, bad, seed);

    // A point behind a path creature, outside its reach plus the margin from its place, is not reached by it
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        Seen unit = RandomEnemy(generator, guid++);
        unit.busy = Busy::OnPath;
        unit.reachYards = generator.Range(5.0f, 45.0f);
        float const angle = generator.Range(0.0f, 6.2831853f);
        float const speed = generator.Range(0.1f, 10.0f);
        unit.moveX = speed * std::cos(angle);
        unit.moveY = speed * std::sin(angle);
        // Behind: 90 to 270 degrees from the way it goes
        float const behind = angle + generator.Range(1.5708f, 4.7124f);
        float const yards = unit.estimateRadius + unit.reachYards + POINT_MARGIN_YARDS + generator.Range(0.01f, 30.0f);
        float const x = unit.x + yards * std::cos(behind);
        float const y = unit.y + yards * std::sin(behind);
        bad += OverOf(unit, x, y) <= POINT_MARGIN_YARDS || !JudgePoint(Pictured({unit}), x, y).clear;
    }
    Report("point-path-passed", INVARIANT_CASES, bad, seed);

    // The answer's fields agree with each other and with the test's own reckoning of every unit
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        PointCase const c = RandomPointCase(generator, guid);
        PointAnswer const answer = c.Judge();
        std::uint32_t reaching = 0;
        bool counting = false;
        double lowest = CLEARANCE_NONE;
        for (Seen const& unit : c.units)
        {
            if (!Counting(unit))
                continue;
            counting = true;
            double const over = ReferenceOver(unit, c.x, c.y);
            lowest = std::min(lowest, over);
            reaching += over <= POINT_MARGIN_YARDS + 1e-4;
            bad += std::fabs(over - OverOf(unit, c.x, c.y)) > 1e-3;
        }
        bool const consistent =
            (answer.reaching == 0) == answer.clear && answer.clear == (answer.clearance > POINT_MARGIN_YARDS) &&
            (answer.nearest == 0) == answer.clear && (counting || answer.clearance == CLEARANCE_NONE);
        bad += !consistent || answer.reaching != reaching || std::fabs(answer.clearance - lowest) > 1e-3;
    }
    Report("point-fields-agree", INVARIANT_CASES, bad, seed);

    // inView never turns true by moving the point away from the bot, by a left-out enemy or by a larger reach
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        PointCase c = RandomPointCase(generator, guid);
        PointAnswer const before = c.Judge();
        if (i % 3 == 0)
        {
            float const scale = generator.Range(1.0f, 2.0f);
            c.x *= scale;
            c.y *= scale;
        }
        else if (i % 3 == 1)
            c.leftOut = static_cast<std::uint16_t>(c.leftOut + 1 + generator.Next() % 5);
        else if (!c.units.empty())
            c.units[generator.Next() % c.units.size()].reachYards += generator.Range(0.1f, 30.0f);
        bad += c.Judge().inView && !before.inView;
    }
    Report("point-in-view", INVARIANT_CASES, bad, seed);

    // Takeable: one ordinary creature of the bot's level or below at a point in view, the others outside reach; then
    // a level above makes it not takeable, and one more reaching unit or a rank above normal exactly when the weight
    // passes the one creature of the bot's level (AW12: two creatures or an elite four or five levels below are taken)
    cases = 0;
    bad = 0;
    while (cases < INVARIANT_CASES)
    {
        PointCase c;
        c.x = generator.Range(-30.0f, 30.0f);
        c.y = generator.Range(-30.0f, 30.0f);
        for (Seen const& unit : RandomEnemies(generator, guid))
            if (unit.reachYards <= 30.0f && ReferenceOver(unit, c.x, c.y) > POINT_MARGIN_YARDS + 0.01)
                c.units.push_back(unit);
        Seen one = RandomEnemy(generator, guid++);
        one.busy = static_cast<Busy>(generator.Next() % 3);  // idle, path or evading
        one.x = one.homeX = c.x + generator.Range(-5.0f, 5.0f);
        one.y = one.homeY = c.y + generator.Range(-5.0f, 5.0f);
        one.moveX = one.moveY = 0.0f;
        one.reachYards = generator.Range(10.0f, 30.0f);
        one.levelDiff = static_cast<std::int16_t>(-static_cast<int>(generator.Next() % 6));
        one.rank = Rank::Normal;
        std::size_t const at = generator.Next() % (c.units.size() + 1);
        c.units.insert(c.units.begin() + at, one);
        bool ok = Takeable(c.Judge());
        PointCase more = c;
        Seen two = one;
        two.guid = guid++;
        more.units.push_back(two);
        ok = ok && Takeable(more.Judge()) == (2 * StrengthOf(one) <= STRENGTH.takeable);
        PointCase higher = c;
        higher.units[at].levelDiff = static_cast<std::int16_t>(1 + generator.Next() % 8);
        ok = ok && !Takeable(higher.Judge());
        PointCase ranked = c;
        ranked.units[at].rank = static_cast<Rank>(1 + generator.Next() % 3);
        ok = ok && Takeable(ranked.Judge()) == (StrengthOf(ranked.units[at]) <= STRENGTH.takeable);
        cases += 3;
        bad += !ok;
    }
    Report("point-takeable", cases, bad, seed);

    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        PointCase c = RandomPointCase(generator, guid);
        PointAnswer const before = c.Judge();
        Shuffle(c.units, generator);
        bad += !(c.Judge() == before);
    }
    Report("point-unit-order", INVARIANT_CASES, bad, seed);
}

// The walkable test of the search's table: the truth per candidate, the results kept across calls (the game side's
// per centre), the tests made in the current call
struct WalkTable
{
    std::vector<bool> truth;
    std::vector<Walk> kept;
    int tests = 0;

    WalkAnswer operator()(std::size_t index, bool mayTest)
    {
        if (kept[index] != Walk::Unknown)
            return WalkAnswer{kept[index], false, 0.0f};
        if (!mayTest)
            return WalkAnswer{};
        ++tests;
        kept[index] = truth[index] ? Walk::Yes : Walk::No;
        return WalkAnswer{kept[index], true, static_cast<float>(index)};
    }
};

struct SearchCase
{
    std::vector<Seen> units;
    std::uint16_t leftOut = 0;
    std::vector<Spot> spots;
    std::vector<bool> truth;
    float fromX = 0.0f;
    float fromY = 0.0f;

    SafePoint Search(WalkTable& table, std::uint32_t testsMax = WALK_TESTS_MAX) const
    {
        table.tests = 0;
        return FindSafePoint(Pictured(units, leftOut), fromX, fromY, spots.data(), spots.size(), table, testsMax);
    }

    WalkTable Table() const { return WalkTable{truth, std::vector<Walk>(truth.size(), Walk::Unknown), 0}; }
};

SearchCase RandomSearch(Generator& generator, std::uint64_t& guid)
{
    SearchCase c;
    c.units = RandomEnemies(generator, guid);
    c.leftOut = RandomLeftOut(generator);
    std::array<Spot, CANDIDATES_MAX> spots{};
    std::size_t count = 0;
    if (generator.Next() % 4)
        count = RingSpots(generator.Range(-60.0f, 60.0f), generator.Range(-60.0f, 60.0f), generator.Range(0.0f, 46.5f),
                          spots.data(), spots.size());
    else
    {
        count = 1 + generator.Next() % CANDIDATES_MAX;
        for (std::size_t i = 0; i < count; ++i)
            spots[i] = Spot{generator.Range(-100.0f, 100.0f), generator.Range(-100.0f, 100.0f)};
    }
    c.spots.assign(spots.begin(), spots.begin() + count);
    for (std::size_t i = 0; i < count; ++i)
        c.truth.push_back(generator.Next() % 10 < 6);
    c.fromX = generator.Next() % 2 ? 0.0f : generator.Range(-60.0f, 60.0f);
    c.fromY = generator.Next() % 2 ? 0.0f : generator.Range(-60.0f, 60.0f);
    return c;
}

// The test's own ordering of the candidates: clear by the capped clearance, else the least reached; then nearer,
// then the lower index. -1 when no candidate is in view and walkable
int ReferenceWinner(SearchCase const& c)
{
    using Key = std::tuple<int, float, int, float, float, std::size_t>;
    Picture const picture = Pictured(c.units, c.leftOut);
    int best = -1;
    Key bestKey{};
    for (std::size_t i = 0; i < c.spots.size(); ++i)
    {
        PointAnswer const answer = JudgePoint(picture, c.spots[i].x, c.spots[i].y);
        if (!answer.inView || !c.truth[i])
            continue;
        float const yards = PlanarDistance(c.fromX, c.fromY, c.spots[i].x, c.spots[i].y);
        Key const key =
            answer.clear
                ? Key{0, -std::min(answer.clearance, COMFORT_YARDS), 0, 0.0f, yards, i}
                : Key{1, static_cast<float>(answer.reaching), answer.topLevelDiff, -answer.clearance, yards, i};
        if (best < 0 || key < bestKey)
        {
            best = static_cast<int>(i);
            bestKey = key;
        }
    }
    return best;
}

constexpr std::uint32_t NO_LIMIT = 1000;

void TestSafeGenerated()
{
    constexpr std::uint32_t seed = SEED + 8;
    Generator generator;
    generator.state = seed;
    std::uint64_t guid = 5000;

    // Only from the candidates, only in view, only walkable, at most WALK_TESTS_MAX new tests; unseen counts the
    // candidates out of view
    int bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        SearchCase const c = RandomSearch(generator, guid);
        WalkTable table = c.Table();
        SafePoint const safe = c.Search(table);
        Picture const picture = Pictured(c.units, c.leftOut);
        std::uint32_t unseen = 0;
        for (Spot const& spot : c.spots)
            unseen += !JudgePoint(picture, spot.x, spot.y).inView;
        bool ok = safe.unseen == unseen && table.tests <= static_cast<int>(WALK_TESTS_MAX) &&
                  safe.tests == static_cast<std::uint32_t>(table.tests);
        if (safe.found)
        {
            std::size_t const at = static_cast<std::size_t>(safe.index);
            ok = ok && safe.index >= 0 && at < c.spots.size() && safe.x == c.spots[at].x && safe.y == c.spots[at].y &&
                 safe.answer.inView && c.truth[at] && safe.clear == safe.answer.clear &&
                 safe.answer == JudgePoint(picture, safe.x, safe.y);
        }
        bad += !ok;
    }
    Report("safe-from-candidates", INVARIANT_CASES, bad, seed);

    // With no limit: a clear candidate in view and walkable makes the answer clear; the winner is the test's own
    bad = 0;
    int clearBad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        SearchCase const c = RandomSearch(generator, guid);
        WalkTable table = c.Table();
        SafePoint const safe = c.Search(table, NO_LIMIT);
        int const winner = ReferenceWinner(c);
        bad += safe.found != (winner >= 0) || (winner >= 0 && safe.index != winner) || safe.outOfTests;
        Picture const picture = Pictured(c.units, c.leftOut);
        bool clearExists = false;
        for (std::size_t s = 0; s < c.spots.size(); ++s)
        {
            PointAnswer const answer = JudgePoint(picture, c.spots[s].x, c.spots[s].y);
            clearExists = clearExists || (answer.clear && answer.inView && c.truth[s]);
        }
        clearBad += clearExists && !(safe.found && safe.clear);
    }
    Report("safe-winner-order", INVARIANT_CASES, bad, seed);
    Report("safe-clear-exists", INVARIANT_CASES, clearBad, seed);

    // With no unit the winner is the walkable candidate in view nearest to from
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        SearchCase c = RandomSearch(generator, guid);
        c.units.clear();
        c.leftOut = 0;
        WalkTable table = c.Table();
        SafePoint const safe = c.Search(table, NO_LIMIT);
        Picture const picture = Pictured({});
        int nearest = -1;
        float yards = 0.0f;
        for (std::size_t s = 0; s < c.spots.size(); ++s)
        {
            float const d = PlanarDistance(c.fromX, c.fromY, c.spots[s].x, c.spots[s].y);
            if (c.truth[s] && JudgePoint(picture, c.spots[s].x, c.spots[s].y).inView && (nearest < 0 || d < yards))
            {
                nearest = static_cast<int>(s);
                yards = d;
            }
        }
        bad += safe.index != nearest || (nearest >= 0 && !safe.clear);
    }
    Report("safe-empty-nearest", INVARIANT_CASES, bad, seed);

    // The order of the units and one more candidate that fails do not move the winner (no limit)
    bad = 0;
    int extraBad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        SearchCase c = RandomSearch(generator, guid);
        WalkTable table = c.Table();
        SafePoint const before = c.Search(table, NO_LIMIT);
        Shuffle(c.units, generator);
        table = c.Table();
        bad += c.Search(table, NO_LIMIT).index != before.index;
        if (c.spots.size() < CANDIDATES_MAX)
        {
            c.spots.push_back(Spot{generator.Range(-100.0f, 100.0f), generator.Range(-100.0f, 100.0f)});
            c.truth.push_back(false);
            table = c.Table();
            extraBad += c.Search(table, NO_LIMIT).index != before.index;
        }
    }
    Report("safe-unit-order", INVARIANT_CASES, bad, seed);
    Report("safe-failing-candidate", INVARIANT_CASES, extraBad, seed);

    // While the tests suffice the limit changes nothing; where not, calls repeated with the results kept reach the
    // same answer within count / WALK_TESTS_MAX calls, rounded up, each making at most WALK_TESTS_MAX tests
    bad = 0;
    int repeatBad = 0;
    int repeated = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        SearchCase c = RandomSearch(generator, guid);
        // Fewer walkable candidates, so the tests often do not suffice
        for (std::size_t s = 0; s < c.truth.size(); ++s)
            c.truth[s] = c.truth[s] && generator.Next() % 3 == 0;
        WalkTable unlimitedTable = c.Table();
        SafePoint const unlimited = c.Search(unlimitedTable, NO_LIMIT);
        WalkTable table = c.Table();
        SafePoint safe = c.Search(table);
        if (unlimited.tests <= WALK_TESTS_MAX)
            bad += safe.index != unlimited.index || safe.outOfTests;
        std::size_t const calls = (c.spots.size() + WALK_TESTS_MAX - 1) / WALK_TESTS_MAX;
        std::size_t call = 1;
        repeated += safe.outOfTests;
        while (safe.outOfTests && call < calls)
        {
            safe = c.Search(table);
            ++call;
            repeatBad += table.tests > static_cast<int>(WALK_TESTS_MAX);
        }
        repeatBad += safe.outOfTests || safe.index != unlimited.index || safe.clear != unlimited.clear;
    }
    Report("safe-tests-suffice", INVARIANT_CASES, bad, seed);
    Report("safe-repeat-converges", INVARIANT_CASES, repeatBad, seed);
    std::printf("AW7 safe: %d of %d searches ran out of tests on the first call\n", repeated, INVARIANT_CASES);
}

// The reading of the research's 140 corpse rises (INTERFACE.md §3.3): each case a picture with the bot at its corpse
// and every spawn of the research's set within 85 yd an idle enemy at its spawn point; the search over radius 31
// with every candidate walkable (the research's grid had no terrain either), and the judgement of the rise place
void TestRiseCases(char const* file)
{
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line))
    {
        std::printf("AW7 reading: no data (%s)\n", file);
        Check(false, "reading: tests/data/aw-rise-cases.tsv is read");
        return;
    }
    struct Case
    {
        std::string id;
        Picture picture;
        float riseX = 0.0f;
        float riseY = 0.0f;
    };
    std::vector<Case> cases;
    std::uint64_t guid = 1;
    while (std::getline(in, line))
    {
        std::istringstream cells(line);
        std::string id;
        int botLevel = 0;
        int entry = 0;
        int maxLevel = 0;
        float corpseX = 0.0f, corpseY = 0.0f, riseX = 0.0f, riseY = 0.0f, x = 0.0f, y = 0.0f, reach = 0.0f;
        if (!(cells >> id >> botLevel >> corpseX >> corpseY >> riseX >> riseY >> entry >> x >> y >> maxLevel >> reach))
            continue;
        if (cases.empty() || cases.back().id != id)
        {
            Case c;
            c.id = id;
            c.picture.self.guid = BOT;
            c.picture.self.level = static_cast<std::uint8_t>(botLevel);
            c.picture.self.x = corpseX;
            c.picture.self.y = corpseY;
            c.riseX = riseX;
            c.riseY = riseY;
            cases.push_back(c);
        }
        Seen spawn = Hostile(++guid, x, y);
        spawn.entry = static_cast<std::uint32_t>(entry);
        spawn.homeX = x;
        spawn.homeY = y;
        spawn.levelDiff = static_cast<std::int16_t>(maxLevel - botLevel);
        spawn.reachYards = reach;
        Admit(cases.back().picture, spawn);
    }

    int found = 0;
    int clear = 0;
    int reached = 0;
    int full = 0;
    for (Case const& c : cases)
    {
        Picture const& picture = c.picture;
        std::array<Spot, CANDIDATES_MAX> spots{};
        std::size_t const count = RingSpots(picture.self.x, picture.self.y, 31.0f, spots.data(), spots.size());
        auto walkable = [](std::size_t, bool mayTest) { return WalkAnswer{Walk::Yes, mayTest, 0.0f}; };
        SafePoint const safe =
            FindSafePoint(picture, picture.self.x, picture.self.y, spots.data(), count, walkable, NO_LIMIT);
        found += safe.found;
        clear += safe.found && safe.clear;
        reached += !JudgePoint(picture, c.riseX, c.riseY).clear;
        full += picture.leftOut[ENEMY] > 0;
    }
    std::printf(
        "AW7 reading: %zu rise cases; the search finds a clear point in %d (the research's 3 yd grid with a "
        "fixed 30 yd: 95 of 140), a point in view in %d; the place the bot rose at is reached in %d; %d "
        "pictures full\n",
        cases.size(), clear, found, reached, full);
    Check(!cases.empty(), "reading: the rise cases are read");
}

// AW8: the rule of the dead (INTERFACE.md §3.4). Fixed cases per line, then the proof obligations P1 to P6 over
// generated facts, each printed with its number of cases and the seed
void ReportDead(char const* name, int cases, int counterexamples, std::uint32_t seed)
{
    std::printf("AW8 invariant %s: %d cases, %d counterexamples (seed %u)\n", name, cases, counterexamples, seed);
    Check(cases >= INVARIANT_CASES && counterexamples == 0, name);
}

// A ghost at its corpse with nothing around: line 7 with a clear point under it
DeadFacts QuietGhost()
{
    DeadFacts facts;
    facts.ghostSeconds = 40;
    facts.corpseYards = 1.0f;
    facts.reclaimHere = true;
    facts.own.inView = true;
    facts.best.found = true;
    facts.best.clear = true;
    facts.best.answer.inView = true;
    facts.atBest = true;
    return facts;
}

// One ordinary creature of the bot's level reaches the answer, in view
PointAnswer OneTakeable()
{
    PointAnswer answer;
    answer.clear = false;
    answer.reaching = 1;
    answer.nearest = 7;
    answer.clearance = -2.0f;
    answer.inView = true;
    answer.weight = CreatureStrength(Rank::Normal, 0);
    return answer;
}

bool Is(DeadFacts const& facts, DeadAct act, DeadReason reason)
{
    DeadStep const step = NextOfDead(facts);
    return step.act == act && step.reason == reason;
}

void TestDeadFixed()
{
    Check(AllWordsDistinct<DeadAct>() && AllWordsDistinct<DeadReason>(), "dead: every step and reason its own word");
    Check(Is(QuietGhost(), DeadAct::Rise, DeadReason::Clear), "dead: a quiet ghost at its corpse rises (clear)");

    // Line 1
    DeadFacts facts = QuietGhost();
    facts.inBattleground = true;
    Check(Is(facts, DeadAct::None, DeadReason::None), "dead 1: a battleground ghost is not the rule's");
    facts = QuietGhost();
    facts.released = false;
    Check(Is(facts, DeadAct::None, DeadReason::None), "dead 1: a body not released is not a ghost yet");
    facts = QuietGhost();
    facts.ownedByPlayer = true;
    facts.corpseHere = false;
    Check(Is(facts, DeadAct::None, DeadReason::None), "dead 1: a player's ghost with its corpse away waits for it");

    // Line 2: the bound
    facts = QuietGhost();
    facts.ghostSeconds = GHOST_BOUND_SECONDS;
    Check(Is(facts, DeadAct::TeleportGrave, DeadReason::WaitEnded),
          "dead 2: at the bound away from the grave: teleport");
    facts.ghostSeconds = GHOST_BOUND_SECONDS - 1;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 2: a second before the bound the rule goes on");
    facts.ghostSeconds = GHOST_BOUND_SECONDS + 5;
    facts.atGravePoint = true;
    facts.healerInRange = true;
    Check(Is(facts, DeadAct::HealerRevive, DeadReason::WaitEnded), "dead 2: at the grave with a healer: its revive");
    facts.healerInRange = false;
    Check(Is(facts, DeadAct::ReviveGrave, DeadReason::WaitEnded), "dead 2: at the grave without one: revive there");
    facts.graveKnown = false;
    Check(Is(facts, DeadAct::ReviveHere, DeadReason::WaitEnded), "dead 2: no grave known: revive here");

    // AW33b: the delay of a step of the dead never sleeps past the bound
    Check(DeadDelayMs(3600 * 1000, 0) == GHOST_BOUND_SECONDS * 1000 && DeadDelayMs(1000, 0) == 1000,
          "dead delay: an hour's hold at the death is cut to the bound, a second stays");
    Check(DeadDelayMs(5000, GHOST_BOUND_SECONDS - 1) == 1000 && DeadDelayMs(1000, GHOST_BOUND_SECONDS) == 1 &&
              DeadDelayMs(1000, GHOST_BOUND_SECONDS + 300) == 1 && DeadDelayMs(0, 10) == 1,
          "dead delay: the time left to the bound, 1 ms at least");
    // AW33b: the walk's progress test
    {
        WalkWatch watch = WatchWalk({}, WalkKind::Corpse, 0.0f, 0.0f, 100.0f, 1000);
        Check(!WalkStalled(watch, 1000 + 59999) && WalkStalled(watch, 1000 + 60000),
              "walk watch: a ghost 100 yd off that comes no nearer stalls after 60 s");
        WalkWatch nearer = WatchWalk(watch, WalkKind::Corpse, 0.0f, 0.0f, 70.0f, 50000);
        Check(nearer.startMs == 50000 && !WalkStalled(nearer, 1000 + 60000),
              "walk watch: 30 yd nearer starts a new window");
        WalkWatch creeps = WatchWalk(watch, WalkKind::Corpse, 0.0f, 0.0f, 71.0f, 50000);
        Check(creeps.startMs == 1000 && WalkStalled(creeps, 1000 + 60000),
              "walk watch: 29 yd nearer in a minute stalls");
        Check(WatchWalk(watch, WalkKind::Corpse, 31.0f, 0.0f, 100.0f, 50000).startMs == 50000 &&
                  WatchWalk(watch, WalkKind::Corpse, 29.0f, 0.0f, 100.0f, 50000).startMs == 1000,
              "walk watch: a target moved more than 30 yd is a new walk, a nearer one is not");
        Check(WatchWalk(watch, WalkKind::Grave, 0.0f, 0.0f, 100.0f, 50000).startMs == 50000,
              "walk watch: the walk to the grave is another walk");
        Check(WatchWalk(watch, WalkKind::Corpse, 0.0f, 0.0f, ARRIVE_YARDS, 61000).startMs == 61000,
              "walk watch: at the target the window starts anew");
        Check(WatchPause(watch, 30000).kind == WalkKind::None && WatchPause(watch, 61000).kind == WalkKind::Corpse,
              "walk watch: a step that does not walk ends the window, not a stalled one");
        Check(!WalkStalled(WalkWatch{}, 1000000), "walk watch: no walk, no stall");
    }

    // Line 3: the healer's way
    facts = QuietGhost();
    facts.corpseHere = false;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseAway), "dead 3: no corpse here: walk to the grave");
    facts = QuietGhost();
    facts.deathsInRow = ROW_HEALER_AT;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseRunsExhausted), "dead 3: the third death in a row");
    facts.deathsInRow = ROW_HEALER_AT - 1;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3: the second death in a row still runs");
    facts.deathsInRow = ROW_HEALER_AT + 4;
    facts.ownedByPlayer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3: a player's bot is not sent by the row");
    facts = QuietGhost();
    facts.playerHold = PlayerHold::Healer;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseCamped), "dead 3: world PvP sends it to the healer");
    facts = QuietGhost();
    facts.corpseWalkFailed = true;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::NoPath), "dead 3: the walk could not start");
    facts = QuietGhost();
    facts.healerChosen = true;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::None), "dead 3: the healer once chosen is kept");
    facts.atGravePoint = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3: at the grave without a healer: wait");
    facts.healerInRange = true;
    Check(Is(facts, DeadAct::HealerRevive, DeadReason::None), "dead 3: at the grave with a healer: its revive");
    // AW33b: a healer seen from off the grave's point (in sight of the grave) is walked to: the revive is at the point
    facts.atGravePoint = false;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::None), "dead 3: a healer seen off the grave's point: walk on");
    facts.graveWalkFailed = true;
    Check(Is(facts, DeadAct::TeleportGrave, DeadReason::None), "dead 3: no walk to the grave: teleport unseen");
    facts.teleportSeen = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::Seen), "dead 3: a seen teleport waits");
    facts.graveKnown = false;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoGrave), "dead 3: no grave known: wait for the bound");

    // Line 4
    facts = QuietGhost();
    facts.playerHold = PlayerHold::Wait;
    Check(Is(facts, DeadAct::Wait, DeadReason::Player), "dead 4: an enemy player at the corpse: wait");

    // Line 5: the leader
    facts = QuietGhost();
    facts.leaderAtCorpse = true;
    facts.best.clear = false;
    facts.atLeader = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Leader), "dead 5: beside the leader at the corpse: rise");
    facts.atLeader = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::Leader), "dead 5: not yet beside it: walk to the leader");
    facts.atLeader = true;
    facts.reclaimHere = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::Leader), "dead 5: beside it out of the reclaim's reach: walk");
    facts.reclaimHere = true;
    facts.leaderDown = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::LeaderDown), "dead 5: the real leader lies dead near: wait");

    // Line 6
    facts = QuietGhost();
    facts.corpseYards = GHOST_LOOK_YARDS + 1.0f;
    Check(Is(facts, DeadAct::WalkCorpse, DeadReason::None), "dead 6: far from the corpse: walk to it");
    facts.corpseYards = GHOST_LOOK_YARDS;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 6: at the look's edge the rule judges");

    // Line 7: a clear point
    facts = QuietGhost();
    facts.own.clearance = COMFORT_YARDS;
    facts.atBest = false;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 7: room to spare on the way: rise");
    facts.own.clearance = COMFORT_YARDS - 1.0f;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::Clear), "dead 7: little room, not at the best: walk on");
    facts.atBest = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 7: little room at the best: rise");
    facts.own.inView = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::Clear), "dead 7: its place not in view: walk");
    facts.own.inView = true;
    facts.own = OneTakeable();
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::Clear), "dead 7: its place reached: walk to the clear point");
    facts = QuietGhost();
    facts.reclaimHere = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::Clear), "dead 7: out of the reclaim's reach: walk");
    facts = QuietGhost();
    facts.leaderDown = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::LeaderDown), "dead 7: the real leader lies dead near: wait");

    // Line 8: no clear point
    DeadFacts none = QuietGhost();
    none.best.clear = false;
    none.best.answer = OneTakeable();
    none.own = OneTakeable();
    facts = none;
    facts.best.unseen = 2;
    facts.corpseYards = ARRIVE_YARDS + 1.0f;
    Check(Is(facts, DeadAct::WalkCorpse, DeadReason::NoClearPoint), "dead 8: candidates unseen: walk to the corpse");
    facts.corpseYards = ARRIVE_YARDS;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 8: unseen from the corpse: they count as reached");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS - 1;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 8: during the wait at the best: wait");
    facts.atBest = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::NoClearPoint), "dead 8: during the wait: to the best first");
    facts.best.found = false;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 8: during the wait with no point: wait");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    Check(Is(facts, DeadAct::Rise, DeadReason::ReachedOne), "dead 8: after the wait beside one creature: rise");
    facts.own = PointAnswer{};
    facts.own.inView = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::ReachedOne), "dead 8: after the wait at a clear place: rise");
    facts.own.inView = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::ReachedOne), "dead 8: its place not in view: walk to the best");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.atBest = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::ReachedOne), "dead 8: after the wait, not at the best: walk");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.reclaimHere = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::ReachedOne), "dead 8: out of the reclaim's reach: walk");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.leaderDown = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::LeaderDown), "dead 8: the real leader lies dead near: wait");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.best.answer.reaching = 2;
    facts.best.answer.weight = 2 * CreatureStrength(Rank::Normal, 0);
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::NoClearPoint), "dead 8: two reach the best: the healer");
    facts.best.answer.reaching = 1;
    facts.best.answer.topLevelDiff = 1;
    facts.best.answer.weight = CreatureStrength(Rank::Normal, 1);
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::NoClearPoint), "dead 8: one above its level: the healer");
    facts.ownedByPlayer = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 8: a player's bot keeps waiting");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.best.found = false;
    facts.best.answer = OneTakeable();
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::NoClearPoint), "dead 8: no point found: the healer");
    // The mutation check's survivors (AW8a): a search that found nothing is no clear point, whatever its answer's
    // defaults say; the classes of the steps the caller's memo and P4 read
    facts = QuietGhost();
    facts.best = SafePoint{};
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 7: a search that found nothing has no clear point");
    Check(CorpseWay(DeadAct::WalkCorpse) && CorpseWay(DeadAct::WalkPoint) && CorpseWay(DeadAct::Wait) &&
              CorpseWay(DeadAct::Rise) && !CorpseWay(DeadAct::None) && !CorpseWay(DeadAct::WaitGrave) &&
              !CorpseWay(DeadAct::TeleportGrave),
          "dead: the corpse's way is its four steps");
    Check(ChoosesHealer(DeadStep{DeadAct::WalkGrave, DeadReason::NoPath}) &&
              ChoosesHealer(DeadStep{DeadAct::WaitGrave, DeadReason::NoGrave}) &&
              !ChoosesHealer(DeadStep{DeadAct::TeleportGrave, DeadReason::WaitEnded}) &&
              !ChoosesHealer(DeadStep{DeadAct::HealerRevive, DeadReason::WaitEnded}) &&
              !ChoosesHealer(DeadStep{DeadAct::Wait, DeadReason::NoClearPoint}),
          "dead: a step of the healer's way chooses it, a step of the bound does not");
    // The AW8 review's survivors: the teleport is a step of the healer's way; one unseen candidate sends the ghost on
    // to its corpse, also after the wait; the order of line 3's causes; a player's bot takes a failed walk and the
    // rise beside one creature like any bot; a dead leader holds the rise, not the walk
    Check(ChoosesHealer(DeadStep{DeadAct::TeleportGrave, DeadReason::NoPath}) &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::TeleportGrave, DeadReason::WaitEnded}, true, false), "bound") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::TeleportGrave, DeadReason::NoPath}, true, false), "healer"),
          "dead: the teleport to the grave is a step of the healer's way");
    facts = none;
    facts.best.unseen = 1;
    facts.corpseYards = ARRIVE_YARDS + 1.0f;
    Check(Is(facts, DeadAct::WalkCorpse, DeadReason::NoClearPoint), "dead 8: one candidate unseen: walk to the corpse");
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    Check(Is(facts, DeadAct::WalkCorpse, DeadReason::NoClearPoint),
          "dead 8: unseen after the wait: walk to the corpse");
    facts = QuietGhost();
    facts.deathsInRow = ROW_HEALER_AT;
    facts.playerHold = PlayerHold::Healer;
    facts.corpseWalkFailed = true;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseRunsExhausted), "dead 3: the row's word before the hold's");
    facts.deathsInRow = 1;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseCamped), "dead 3: the hold's word before the walk's");
    facts = QuietGhost();
    facts.ownedByPlayer = true;
    facts.corpseWalkFailed = true;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::NoPath), "dead 3: a player's bot whose walk could not start");
    facts = none;
    facts.ownedByPlayer = true;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    Check(Is(facts, DeadAct::Rise, DeadReason::ReachedOne), "dead 8: a player's bot rises beside one creature too");
    facts = QuietGhost();
    facts.leaderDown = true;
    facts.reclaimHere = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::Clear), "dead 7: under a dead leader the ghost still walks");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.leaderDown = true;
    facts.atBest = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::ReachedOne), "dead 8: under a dead leader the ghost still walks");

    // AW30b: a grave without a spirit healer closes this death's healer's way while the corpse lies on this map
    facts = QuietGhost();
    facts.deathsInRow = ROW_HEALER_AT;
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 closed: the third death in a row rises at its corpse");
    facts.corpseYards = GHOST_LOOK_YARDS + 1.0f;
    facts.atGravePoint = true;
    Check(Is(facts, DeadAct::WalkCorpse, DeadReason::None), "dead 3 closed: from the grave back to the corpse");
    facts = QuietGhost();
    facts.healerChosen = true;
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 closed: the healer once chosen holds no longer");

    // AW38a: the two grounds. A grave in deadly ground closes the healer's way while the corpse lies here
    facts = QuietGhost();
    facts.deathsInRow = ROW_HEALER_AT;
    facts.graveGround = Ground::Deadly;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 danger: the third death rises at a clear corpse");
    facts.healerChosen = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 danger: the healer once chosen holds no longer");
    facts.playerHold = PlayerHold::Healer;
    Check(Is(facts, DeadAct::Wait, DeadReason::Player), "dead 3 danger: world PvP's hold for the healer waits");
    facts.playerHold = PlayerHold::None;
    facts.corpseWalkFailed = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoPath), "dead 3 danger: a walk that cannot start waits where it is");
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3 both closures: AW30b's wait at the grave");
    facts = QuietGhost();
    facts.corpseHere = false;
    facts.graveGround = Ground::Deadly;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseAway), "dead 3 X1: without a corpse here the healer anyway");
    facts = QuietGhost();
    facts.graveGround = Ground::Deadly;
    facts.ghostSeconds = GHOST_BOUND_SECONDS;
    facts.atGravePoint = true;
    facts.healerInRange = true;
    Check(Is(facts, DeadAct::HealerRevive, DeadReason::WaitEnded), "dead 2 X2: the bound revives at its grave");
    // A corpse in deadly ground: the healer from the second death in a row, where the grave's ground is not deadly
    facts = QuietGhost();
    facts.corpseGround = Ground::Deadly;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 deadly corpse: a first death rises at a clear corpse");
    facts.deathsInRow = ROW_DEADLY_AT;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::DeadlyGround), "dead 3 deadly corpse: the second death the healer");
    facts.graveGround = Ground::Passable;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::DeadlyGround), "dead 3 deadly corpse: to a passable grave too");
    facts.ownedByPlayer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 deadly corpse: a player's bot keeps its corpse");
    facts.ownedByPlayer = false;
    facts.graveGround = Ground::Deadly;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 deadly corpse: both deadly: the corpse's way");
    facts.deathsInRow = ROW_HEALER_AT;
    facts.graveGround = Ground::Quiet;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseRunsExhausted), "dead 3: the row's word before the ground's");
    // Line 8 on a closed way: the rise where the bot holds at half health; else the wait (grave_danger or no_healer)
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.graveGround = Ground::Deadly;
    facts.best.answer.weight = 2 * CreatureStrength(Rank::Normal, -2);
    facts.best.answer.reaching = 2;
    facts.own = facts.best.answer;
    Check(Is(facts, DeadAct::Rise, DeadReason::ReachedHolds), "dead 8 closed: two two levels below: it holds");
    facts.atBest = false;
    Check(Is(facts, DeadAct::WalkPoint, DeadReason::ReachedHolds), "dead 8 closed: the walk to the point it holds");
    facts.atBest = true;
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::ReachedHolds), "dead 8 closed: either closure: it holds");
    facts.graveWithoutHealer = false;
    facts.best.answer.reaching = 1;
    facts.best.answer.weight = CreatureStrength(Rank::Elite, 0);
    facts.own = facts.best.answer;
    Check(Is(facts, DeadAct::Wait, DeadReason::GraveDanger), "dead 8 closed: an elite of its level: the wait");
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoHealer), "dead 8 closed: both closures wait no_healer");
    facts.graveWithoutHealer = false;
    facts.ownedByPlayer = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 8 closed: a player's bot keeps waiting");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.graveGround = Ground::Passable;
    facts.best.answer.weight = 2 * CreatureStrength(Rank::Normal, -2);
    facts.best.answer.reaching = 2;
    facts.own = facts.best.answer;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::NoClearPoint), "dead 8 open: what it holds is no rise there");
    Check(HoldsAtRise(OneTakeable()) && !Takeable(facts.best.answer) && HoldsAtRise(facts.best.answer),
          "dead 8: what a risen bot takes it holds; it holds more at half health than it takes");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.best.answer.reaching = 2;
    facts.best.answer.weight = 2 * CreatureStrength(Rank::Normal, 0);
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoHealer), "dead 8 closed: two reach the best: wait, not the healer");
    facts.best.found = false;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoHealer), "dead 8 closed: no point found: wait");
    facts.ownedByPlayer = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 8 closed: a player's bot waits as before");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::ReachedHolds),
          "dead 8 closed: beside one creature it still rises (AW38a: by the closed way's wider test)");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.playerHold = PlayerHold::Healer;
    facts.deathsInRow = ROW_HEALER_AT;
    Check(Is(facts, DeadAct::Wait, DeadReason::Player), "dead 3 closed: a hold that asks for the healer waits");
    facts.corpseWalkFailed = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::Player), "dead 3 closed: the hold before the failed walk");
    facts.playerHold = PlayerHold::None;
    facts.atGravePoint = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3 closed: a walk that cannot start waits");
    facts.atGravePoint = false;
    facts.graveWalkFailed = true;
    facts.teleportSeen = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3 closed: a failed walk never teleports");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.corpseHere = false;
    facts.atGravePoint = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3: no corpse here: the grave's wait as before");
    facts.atGravePoint = false;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseAway), "dead 3: no corpse here: the healer's way as before");
    facts.healerChosen = true;
    facts.playerHold = PlayerHold::Healer;
    facts.corpseWalkFailed = true;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseAway),
          "dead 3: no corpse here: the healer once chosen, a hold and a failed walk close nothing");
    facts.atGravePoint = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer),
          "dead 3: no corpse here: the healer once chosen still waits at the grave");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.healerChosen = true;
    facts.ghostSeconds = GHOST_BOUND_SECONDS;
    facts.atGravePoint = true;
    Check(Is(facts, DeadAct::ReviveGrave, DeadReason::WaitEnded), "dead 2 closed: the bound comes first");
    facts.atGravePoint = false;
    Check(Is(facts, DeadAct::TeleportGrave, DeadReason::WaitEnded), "dead 2 closed: the bound's teleport");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.leaderDown = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::LeaderDown), "dead 7 closed: the dead leader's hold stays");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.deathsInRow = ROW_HEALER_AT;
    facts.leaderAtCorpse = true;
    facts.atLeader = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Leader), "dead 5 closed: beside the leader at the corpse: rise");

    // The Rise row
    Check(RiseAmount(RiseRow{45, 1, 12, 2}) == (45ull | 1ull << 15 | 12ull << 22 | 2ull << 32),
          "rise row: the fields at their places");
    Check(RiseAmount(RiseRow{40000, 300, 5000, 500}) == (32767ull | 127ull << 15 | 1023ull << 22 | 127ull << 32),
          "rise row: each field capped");
    Check(RiseAmount(RiseRow{0, 300, 0, 0}) == 127ull << 15, "rise row: the reaching count capped in its 7 bits");
    Check(!std::strcmp(RiseWord(DeadStep{DeadAct::Rise, DeadReason::ReachedOne}, true, true), "reached_one") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::Rise, DeadReason::Clear}, false, true), "other") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::Rise, DeadReason::Clear}, true, false), "other") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::WalkGrave, DeadReason::NoPath}, true, false), "healer") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::HealerRevive, DeadReason::WaitEnded}, true, false), "bound") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::ReviveHere, DeadReason::WaitEnded}, true, false), "bound") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::Wait, DeadReason::Player}, true, false), "other") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::WalkGrave, DeadReason::NoPath}, true, true), "other"),
          "rise row: the word by the rule's last step and the place");
    Check(!std::strcmp(RiseWord(DeadStep{}, false, false, true), "self") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::Rise, DeadReason::Clear}, true, true, true), "self") &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::Rise, DeadReason::Clear}, true, true, false), "clear"),
          "rise row: a self-resurrection the self-rise rule ordered: self");

    // AW33b: the self-rise rule, a body that holds a self-resurrection
    SelfRiseFacts body;
    body.own.clear = true;
    body.own.inView = true;
    Check(SelfRiseStep(body) == SelfRiseAct::Rise, "self rise: a clear place in view: rise at once");
    body.own.inView = false;
    Check(SelfRiseStep(body) == SelfRiseAct::Wait, "self rise: a clear place out of view waits");
    body.own = OneTakeable();
    body.bodySeconds = SELF_RISE_WAIT_SECONDS - 1;
    Check(SelfRiseStep(body) == SelfRiseAct::Wait, "self rise: a reached place waits for it to clear");
    body.bodySeconds = SELF_RISE_WAIT_SECONDS;
    Check(SelfRiseStep(body) == SelfRiseAct::Release, "self rise: the wait over: release");
    body.inBattleground = true;
    Check(SelfRiseStep(body) == SelfRiseAct::Rise, "self rise: a battleground body rises at once");
    body.inBattleground = false;
    body.ownedByPlayer = true;
    body.bodySeconds = 0;
    Check(SelfRiseStep(body) == SelfRiseAct::Rise, "self rise: a player's bot rises at once");
}

// A generated point answer as JudgePoint gives them: clear exactly when none reaches, a clearance above the margin
// when clear (or none counts) and at most the margin when reached, the top level and rank of the reaching units
PointAnswer RandomAnswer(Generator& generator)
{
    PointAnswer answer;
    answer.reaching = generator.Next() % 2 ? 0 : 1 + generator.Next() % 3;
    answer.clear = answer.reaching == 0;
    if (answer.clear)
        answer.clearance =
            generator.Next() % 4 == 0 ? CLEARANCE_NONE : generator.Range(POINT_MARGIN_YARDS + 0.01f, 30.0f);
    else
    {
        answer.clearance = generator.Range(-30.0f, POINT_MARGIN_YARDS);
        answer.nearest = 1 + generator.Next() % 100;
        answer.topLevelDiff = static_cast<std::int16_t>(static_cast<int>(generator.Next() % 10) - 5);
        answer.topRank = generator.Next() % 4 == 0 ? static_cast<Rank>(1 + generator.Next() % 3) : Rank::Normal;
        // AW12: the weight as if every reaching unit were the top one
        answer.weight = answer.reaching * CreatureStrength(answer.topRank, answer.topLevelDiff);
    }
    answer.inView = generator.Next() % 4 != 0;
    return answer;
}

// A generated search result: a found point is in view; clear as its answer
SafePoint RandomBest(Generator& generator)
{
    SafePoint best;
    best.found = generator.Next() % 4 != 0;
    best.unseen = generator.Next() % 2 ? 0 : generator.Next() % 6;
    if (!best.found)
        return best;
    best.answer = RandomAnswer(generator);
    best.answer.inView = true;
    best.clear = best.answer.clear;
    best.x = generator.Range(-31.0f, 31.0f);
    best.y = generator.Range(-31.0f, 31.0f);
    return best;
}

DeadFacts RandomDeadFacts(Generator& generator)
{
    DeadFacts facts;
    facts.inBattleground = generator.Next() % 16 == 0;
    facts.released = generator.Next() % 16 != 0;
    facts.ghostSeconds = generator.Next() % (GHOST_BOUND_SECONDS + 100);
    facts.corpseHere = generator.Next() % 8 != 0;
    facts.corpseYards = generator.Next() % 2 ? generator.Range(0.0f, GHOST_LOOK_YARDS) : generator.Range(0.0f, 150.0f);
    facts.reclaimHere = generator.Next() % 2;
    facts.deathsInRow = 1 + generator.Next() % 5;
    facts.ownedByPlayer = generator.Next() % 4 == 0;
    facts.playerHold = generator.Next() % 4 == 0 ? static_cast<PlayerHold>(1 + generator.Next() % 2) : PlayerHold::None;
    facts.leaderAtCorpse = generator.Next() % 4 == 0;
    facts.atLeader = generator.Next() % 2;
    facts.leaderDown = generator.Next() % 6 == 0;
    facts.own = RandomAnswer(generator);
    facts.best = RandomBest(generator);
    facts.atBest = generator.Next() % 2;
    facts.unclearSeconds = generator.Next() % 2 ? 0 : generator.Next() % (2 * UNCLEAR_WAIT_SECONDS);
    facts.corpseWalkFailed = generator.Next() % 8 == 0;
    facts.healerChosen = generator.Next() % 4 == 0;
    facts.graveKnown = generator.Next() % 8 != 0;
    facts.atGravePoint = generator.Next() % 3 == 0;
    facts.healerInRange = generator.Next() % 2;
    facts.graveWalkFailed = generator.Next() % 4 == 0;
    facts.teleportSeen = generator.Next() % 2;
    facts.graveWithoutHealer = generator.Next() % 4 == 0;
    // AW38a: the two grounds, any of the nine pairs
    facts.corpseGround = static_cast<Ground>(generator.Next() % 3);
    facts.graveGround = static_cast<Ground>(generator.Next() % 3);
    return facts;
}

// AW38a: the healer's way is closed for this death (either closure: no healer at the grave, or its deadly ground)
bool ClosedWay(DeadFacts const& facts)
{
    return facts.corpseHere && (facts.graveWithoutHealer || facts.graveGround == Ground::Deadly);
}

// AW38a P15's reference: the rule of the dead as built before AW38a (AW33b's), kept unchanged
DeadStep NextOfDeadBefore38a(DeadFacts const& facts)
{
    if (facts.inBattleground || !facts.released || (facts.ownedByPlayer && !facts.corpseHere))
        return {};
    if (facts.ghostSeconds >= GHOST_BOUND_SECONDS)
        return BoundStep(facts);
    if (facts.graveWithoutHealer && facts.corpseHere)
    {
        if (facts.playerHold == PlayerHold::Healer)
            return {DeadAct::Wait, DeadReason::Player};
        if (facts.corpseWalkFailed)
            return {DeadAct::WaitGrave, DeadReason::NoHealer};
    }
    else
    {
        DeadReason healer = DeadReason::None;
        if (!facts.corpseHere)
            healer = DeadReason::CorpseAway;
        else if (facts.deathsInRow >= ROW_HEALER_AT && !facts.ownedByPlayer)
            healer = DeadReason::CorpseRunsExhausted;
        else if (facts.playerHold == PlayerHold::Healer)
            healer = DeadReason::CorpseCamped;
        else if (facts.corpseWalkFailed)
            healer = DeadReason::NoPath;
        if (facts.healerChosen || healer != DeadReason::None)
            return HealerWayStep(facts, healer);
    }
    if (facts.playerHold == PlayerHold::Wait)
        return {DeadAct::Wait, DeadReason::Player};
    if (facts.leaderAtCorpse)
        return facts.atLeader && facts.reclaimHere ? RiseUnlessLeaderDown(facts, DeadReason::Leader)
                                                   : DeadStep{DeadAct::WalkPoint, DeadReason::Leader};
    if (facts.corpseYards > GHOST_LOOK_YARDS)
        return {DeadAct::WalkCorpse, DeadReason::None};
    if (facts.best.found && facts.best.clear)
    {
        bool const rises = facts.reclaimHere && facts.own.clear && facts.own.inView &&
                           (facts.own.clearance >= COMFORT_YARDS || facts.atBest);
        return rises ? RiseUnlessLeaderDown(facts, DeadReason::Clear) : DeadStep{DeadAct::WalkPoint, DeadReason::Clear};
    }
    if (facts.best.unseen > 0 && facts.corpseYards > ARRIVE_YARDS)
        return {DeadAct::WalkCorpse, DeadReason::NoClearPoint};
    if (facts.unclearSeconds < UNCLEAR_WAIT_SECONDS)
        return facts.best.found && !facts.atBest ? DeadStep{DeadAct::WalkPoint, DeadReason::NoClearPoint}
                                                 : DeadStep{DeadAct::Wait, DeadReason::NoClearPoint};
    if (facts.best.found && Takeable(facts.best.answer))
    {
        bool const ownTaken = (facts.own.clear && facts.own.inView) || Takeable(facts.own);
        return facts.atBest && facts.reclaimHere && ownTaken ? RiseUnlessLeaderDown(facts, DeadReason::ReachedOne)
                                                             : DeadStep{DeadAct::WalkPoint, DeadReason::ReachedOne};
    }
    if (facts.ownedByPlayer)
        return {DeadAct::Wait, DeadReason::NoClearPoint};
    if (facts.graveWithoutHealer)
        return {DeadAct::Wait, DeadReason::NoHealer};
    return HealerWayStep(facts, DeadReason::NoClearPoint);
}

// Past line 1: a released ghost outside a battleground, with its corpse here or not a player's bot
void PastLineOne(DeadFacts& facts)
{
    facts.inBattleground = false;
    facts.released = true;
    if (facts.ownedByPlayer)
        facts.corpseHere = true;
}

// Half the cases on the corpse's way near the corpse, where the rule can say Rise: a ghost before the bound with its
// corpse here, in the reclaim's reach, no healer chosen, no walk failed, no hold, a short row; a ghost whose grave had
// no healer (AW30b) with any row and the healer chosen or not
void TowardRise(DeadFacts& facts, Generator& generator)
{
    if (generator.Next() % 2)
        return;
    PastLineOne(facts);
    facts.ghostSeconds %= GHOST_BOUND_SECONDS;
    facts.corpseHere = true;
    facts.corpseYards = generator.Range(0.0f, GHOST_LOOK_YARDS);
    facts.reclaimHere = true;
    facts.deathsInRow = 1 + generator.Next() % (ROW_HEALER_AT - 1);
    facts.playerHold = PlayerHold::None;
    facts.healerChosen = false;
    facts.corpseWalkFailed = false;
    if (facts.graveWithoutHealer)
    {
        facts.deathsInRow = 1 + generator.Next() % (ROW_HEALER_AT + 2);
        facts.healerChosen = generator.Next() % 2;
    }
}

// The same answer with one more unit reaching it (§3.2's invariants: it reaches, so its over is at most the margin;
// the top level and rank only grow; the view can only shrink, as its reach raises the unseen reach)
PointAnswer OneMoreReaching(PointAnswer answer, Generator& generator)
{
    std::int16_t const level = static_cast<std::int16_t>(static_cast<int>(generator.Next() % 10) - 5);
    Rank const rank = generator.Next() % 4 == 0 ? static_cast<Rank>(1 + generator.Next() % 3) : Rank::Normal;
    answer.topLevelDiff = answer.reaching ? std::max(answer.topLevelDiff, level) : level;
    answer.topRank = answer.reaching ? std::max(answer.topRank, rank) : rank;
    answer.weight += CreatureStrength(rank, level);
    ++answer.reaching;
    answer.clear = false;
    answer.clearance = std::min(answer.clearance, generator.Range(-30.0f, POINT_MARGIN_YARDS));
    answer.inView = answer.inView && generator.Next() % 3 != 0;
    return answer;
}

void TestDeadGenerated()
{
    // P1 The bound is total
    Generator generator;
    int counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        PastLineOne(facts);
        facts.ghostSeconds = GHOST_BOUND_SECONDS + (generator.Next() % 4 == 0 ? 0 : generator.Next() % 3600);
        DeadStep const step = NextOfDead(facts);
        bool const bound = step.act == DeadAct::HealerRevive || step.act == DeadAct::ReviveGrave ||
                           step.act == DeadAct::ReviveHere || step.act == DeadAct::TeleportGrave;
        counterexamples += !bound || (facts.atGravePoint && step.act == DeadAct::TeleportGrave);
    }
    ReportDead("P1 bound-total", INVARIANT_CASES, counterexamples, SEED);

    // P2 Every run ends: the facts drawn anew each step, the memo carried as the caller carries it (the healer once
    // chosen, the unclear clock), a teleport puts the ghost at its grave; a Rise the core accepts or a revive ends the
    // run, within 2 steps asked at or past the bound. AW33b: each step draws its delay (most a second, some a hold of
    // up to an hour, world PvP's), cut by DeadDelayMs as the caller cuts it, so the first step at or past the bound is
    // asked at the bound itself
    counterexamples = 0;
    std::uint64_t steps = 0;
    int bounded = 0;
    int held = 0;
    std::array<int, 9> pairRuns{};  // AW38a P14: the runs per pair of grounds (corpse x 3 + grave), those not ended
    std::array<int, 9> pairUnended{};
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        // A hard run: the core refuses every Rise and no healer is in range, so only the bound ends it
        bool const hard = generator.Next() % 2 == 0;
        std::uint32_t const accept = hard || generator.Next() % 3 == 0 ? 0 : 1 + generator.Next() % 4;  // 0: refuses
        std::uint32_t const start = generator.Next() % (GHOST_BOUND_SECONDS + 50);
        bool chosen = false;
        bool withoutHealer = generator.Next() % 8 == 0;
        bool teleported = false;
        std::int64_t unclearStart = -1;
        bool ended = false;
        // AW38a: the two grounds are drawn once per run (a ground does not change during a death), any of the nine
        Ground const corpseGround = static_cast<Ground>(generator.Next() % 3);
        Ground const graveGround = static_cast<Ground>(generator.Next() % 3);
        ++pairRuns[static_cast<std::size_t>(corpseGround) * 3 + static_cast<std::size_t>(graveGround)];
        std::uint64_t ms = std::uint64_t{start} * 1000;
        std::uint32_t seconds = start;
        int pastBound = 0;  // the steps asked at or past the bound
        while (!ended && pastBound < 2)
        {
            seconds = static_cast<std::uint32_t>(ms / 1000);
            if (seconds >= GHOST_BOUND_SECONDS && !pastBound++)
                counterexamples += start < GHOST_BOUND_SECONDS && seconds > GHOST_BOUND_SECONDS;
            ++steps;
            DeadFacts facts = RandomDeadFacts(generator);
            PastLineOne(facts);
            facts.ghostSeconds = seconds;
            facts.healerChosen = chosen;
            facts.corpseGround = facts.corpseHere ? corpseGround : Ground::Quiet;
            facts.graveGround = graveGround;
            if (facts.best.found && facts.best.clear)
                unclearStart = -1;
            else if (unclearStart < 0)
                unclearStart = seconds;
            facts.unclearSeconds = unclearStart < 0 ? 0 : seconds - static_cast<std::uint32_t>(unclearStart);
            if (hard)
            {
                facts.healerInRange = false;
                facts.atGravePoint = false;
            }
            if (teleported)
            {
                facts.graveKnown = true;
                facts.atGravePoint = true;
            }
            // AW30b: the caller's search at the grave that finds no healer closes the healer's way for the run
            bool const searched =
                !withoutHealer && facts.atGravePoint && !facts.healerInRange && generator.Next() % 4 == 0;
            withoutHealer = GraveWithoutHealer(withoutHealer, facts.atGravePoint, searched, facts.healerInRange);
            facts.graveWithoutHealer = withoutHealer;
            DeadStep const step = NextOfDead(facts);
            chosen = chosen || ChoosesHealer(step);
            teleported = teleported || step.act == DeadAct::TeleportGrave;
            ended = Revives(step.act) || (step.act == DeadAct::Rise && accept && generator.Next() % accept == 0);
            bool const hold = generator.Next() % 4 == 0;
            std::uint32_t const delayMs = hold ? 1000 + generator.Next() % (3600 * 1000) : 1000;
            held += hold && delayMs > (GHOST_BOUND_SECONDS - std::min(seconds, GHOST_BOUND_SECONDS)) * 1000;
            ms += DeadDelayMs(delayMs, seconds);
        }
        counterexamples += !ended;
        pairUnended[static_cast<std::size_t>(corpseGround) * 3 + static_cast<std::size_t>(graveGround)] += !ended;
        bounded += ended && start < GHOST_BOUND_SECONDS && seconds >= GHOST_BOUND_SECONDS;
    }
    std::printf("AW8 reading: P2 ran %llu steps, %d runs reached the bound, %d delays cut at it\n",
                static_cast<unsigned long long>(steps), bounded, held);
    Check(held > INVARIANT_CASES / 10, "P2: holds longer than the time left to the bound");
    Check(bounded > INVARIANT_CASES / 10, "P2: runs that only the bound ends");
    ReportDead("P2 every-run-ends", INVARIANT_CASES, counterexamples, SEED);

    // P3 The wait ends: no clear point, not a player's bot, the wait over: never Wait (no_clear_point)
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        if (facts.best.clear)
        {
            facts.best.answer = OneMoreReaching(facts.best.answer, generator);
            facts.best.answer.inView = true;
            facts.best.clear = false;
        }
        facts.ownedByPlayer = false;
        facts.unclearSeconds = UNCLEAR_WAIT_SECONDS + (generator.Next() % 4 == 0 ? 0 : generator.Next() % 600);
        counterexamples += NextOfDead(facts) == DeadStep{DeadAct::Wait, DeadReason::NoClearPoint};
    }
    ReportDead("P3 wait-ends", INVARIANT_CASES, counterexamples, SEED);

    // P4 The healer is kept: with healerChosen, where the way is open (the grave did not lack a healer, AW30b, and
    // AW38a: its ground is not deadly), no step of the corpse's way; more deaths in a row never turn a step off the
    // corpse's way into one on it
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        DeadFacts chosen = facts;
        chosen.healerChosen = true;
        counterexamples += !ClosedWay(chosen) && CorpseWay(NextOfDead(chosen).act);
        DeadFacts more = facts;
        more.deathsInRow += 1 + generator.Next() % 3;
        counterexamples += !CorpseWay(NextOfDead(facts).act) && CorpseWay(NextOfDead(more).act);
    }
    ReportDead("P4 healer-kept", INVARIANT_CASES, counterexamples, SEED);

    // P5 A ghost rises only where the rule allows; with deathsInRow >= ROW_HEALER_AT only a player's bot or a ghost
    // whose healer's way is closed (AW30b, AW38a). AW38a: reached_holds only on a closed way, after the wait, at the
    // best point it holds at, its own place clear and in view or held at, not a player's bot (which takes reached_one
    // as before); reached_one not on a closed way but for a player's bot; a second death in a row at a corpse in deadly
    // ground does not rise where the way is open but for a player's bot
    counterexamples = 0;
    int rises = 0;
    int closedRises = 0;
    int holdsRises = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        TowardRise(facts, generator);
        DeadStep const step = NextOfDead(facts);
        if (step.act != DeadAct::Rise)
            continue;
        ++rises;
        bool const closed = ClosedWay(facts);
        bool reason = false;
        if (step.reason == DeadReason::Clear)
            reason = facts.own.clear && facts.own.inView;
        else if (step.reason == DeadReason::ReachedOne)
            reason = facts.unclearSeconds >= UNCLEAR_WAIT_SECONDS && facts.atBest &&
                     ((facts.own.clear && facts.own.inView) || Takeable(facts.own)) && (!closed || facts.ownedByPlayer);
        else if (step.reason == DeadReason::ReachedHolds)
        {
            reason = closed && !facts.ownedByPlayer && facts.unclearSeconds >= UNCLEAR_WAIT_SECONDS && facts.atBest &&
                     facts.best.found && HoldsAtRise(facts.best.answer) &&
                     ((facts.own.clear && facts.own.inView) || HoldsAtRise(facts.own));
            ++holdsRises;
        }
        else if (step.reason == DeadReason::Leader)
            reason = facts.atLeader;
        bool const deadlyRow =
            facts.deathsInRow >= ROW_DEADLY_AT && facts.corpseGround == Ground::Deadly && !facts.ownedByPlayer;
        bool const allowed = reason && facts.reclaimHere && !facts.leaderDown && facts.playerHold == PlayerHold::None &&
                             (facts.deathsInRow < ROW_HEALER_AT || facts.ownedByPlayer || closed) &&
                             (!deadlyRow || closed);
        counterexamples += !allowed;
        closedRises += closed && facts.deathsInRow >= ROW_HEALER_AT && !facts.ownedByPlayer;
    }
    std::printf("AW8 reading: P5 judged %d rises, %d of a third death or more on a closed way, %d reached_holds\n",
                rises, closedRises, holdsRises);
    Check(rises > INVARIANT_CASES / 100, "P5: the generated facts give rises to judge");
    Check(closedRises > INVARIANT_CASES / 1000, "P5: rises of a closed healer's way to judge");
    Check(holdsRises > 0, "P5: rises where the bot holds on a closed way to judge");
    ReportDead("P5 rise-allowed", INVARIANT_CASES, counterexamples, SEED);

    // P6 Facts only: the same facts give the same step; over facts as the caller fills them (no unclear clock while
    // the best is clear) a worse own place or best point never turns another step into Rise
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        TowardRise(facts, generator);
        if (facts.best.found && facts.best.clear)
            facts.unclearSeconds = 0;
        DeadStep const step = NextOfDead(facts);
        counterexamples += !(NextOfDead(facts) == step);
        DeadFacts worse = facts;
        std::uint32_t const which = generator.Next() % 3;
        if (which != 1)
            worse.own = OneMoreReaching(worse.own, generator);
        if (which != 0 && worse.best.found)
        {
            worse.best.answer = OneMoreReaching(worse.best.answer, generator);
            worse.best.answer.inView = true;
            worse.best.clear = false;
        }
        counterexamples += step.act != DeadAct::Rise && NextOfDead(worse).act == DeadAct::Rise;
    }
    ReportDead("P6 facts-only", INVARIANT_CASES, counterexamples, SEED);

    // P10 A closed healer's way (AW30b; AW38a: either closure, a grave without a healer or a grave in deadly ground):
    // with the corpse here, before the bound, no step of the healer's way but the wait at the grave of a walk that
    // could not start where the grave had no healer (no enemy player's hold); the row, the healer once chosen and the
    // corpse's ground change nothing; on the danger closure alone a walk that could not start waits where it is
    // (no_path). The closed way's wait (no_healer where the grave had no healer, grave_danger on the danger closure
    // alone) exactly where line 8 of the open way would choose the healer and the bot does not hold the best point
    // (the closed way's wider rise; a player's bot keeps reached_one's test): the same facts with the way open, the
    // healer not chosen, a first death, no hold for the healer and no failed walk
    counterexamples = 0;
    int closedWaits = 0;
    std::array<int, 3> closures{};
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        TowardRise(facts, generator);
        PastLineOne(facts);
        facts.ghostSeconds %= GHOST_BOUND_SECONDS;
        facts.corpseHere = true;
        std::uint32_t const closure = generator.Next() % 3;  // 0 no healer, 1 deadly ground, 2 both
        ++closures[closure];
        facts.graveWithoutHealer = closure != 1;
        facts.graveGround = closure != 0 ? Ground::Deadly : static_cast<Ground>(generator.Next() % 2);
        DeadStep const step = NextOfDead(facts);
        bool const graveWait = step == DeadStep{DeadAct::WaitGrave, DeadReason::NoHealer} && facts.corpseWalkFailed &&
                               facts.playerHold != PlayerHold::Healer && facts.graveWithoutHealer;
        counterexamples += (HealerWay(step.act) || Revives(step.act)) && !graveWait;
        counterexamples += closure == 1 && facts.corpseWalkFailed && facts.playerHold != PlayerHold::Healer &&
                           !(step == DeadStep{DeadAct::Wait, DeadReason::NoPath});
        DeadFacts other = facts;
        other.healerChosen = !facts.healerChosen;
        other.deathsInRow = 1 + generator.Next() % (ROW_HEALER_AT + 3);
        other.corpseGround = static_cast<Ground>(generator.Next() % 3);
        counterexamples += !(NextOfDead(other) == step);
        DeadReason const waitWord = facts.graveWithoutHealer ? DeadReason::NoHealer : DeadReason::GraveDanger;
        DeadReason const otherWord = facts.graveWithoutHealer ? DeadReason::GraveDanger : DeadReason::NoHealer;
        bool const closedWait = step == DeadStep{DeadAct::Wait, waitWord};
        counterexamples += step == DeadStep{DeadAct::Wait, otherWord};
        closedWaits += closedWait;
        if (facts.playerHold == PlayerHold::Healer || facts.corpseWalkFailed)
        {
            counterexamples += closedWait;
            continue;
        }
        DeadFacts open = facts;
        open.graveWithoutHealer = false;
        open.graveGround = Ground::Quiet;
        open.healerChosen = false;
        open.deathsInRow = 1;
        bool const holds = !facts.ownedByPlayer && facts.best.found && HoldsAtRise(facts.best.answer);
        counterexamples += closedWait != (HealerWay(NextOfDead(open).act) && !holds);
    }
    // Without a corpse on this map neither closure nor the corpse's ground changes a step, whatever the other facts
    // (the healer's way stays the ghost's only one, X1): drawn after the cases above, so their readings stay
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        facts.corpseHere = false;
        DeadFacts open = facts;
        facts.graveWithoutHealer = true;
        facts.graveGround = Ground::Deadly;
        facts.corpseGround = Ground::Deadly;
        open.graveWithoutHealer = false;
        open.graveGround = Ground::Quiet;
        open.corpseGround = Ground::Quiet;
        counterexamples += !(NextOfDead(facts) == NextOfDead(open));
    }
    std::printf("AW30b reading: P10 waited (no_healer or grave_danger) in %d cases; closures %d %d %d\n", closedWaits,
                closures[0], closures[1], closures[2]);
    Check(closedWaits > INVARIANT_CASES / 100, "P10: the generated facts give waits of a closed way to judge");
    Check(closures[0] > INVARIANT_CASES / 4 && closures[1] > INVARIANT_CASES / 4 && closures[2] > INVARIANT_CASES / 4,
          "P10: each closure judged");
    ReportDead("P10 healer-closed", INVARIANT_CASES, counterexamples, SEED);

    // P11 A stalled walk ends (AW33b): runs of one-second steps over facts drawn once per run and anew now and then (a
    // new target), the memo carried as the caller carries it, the walk's watch kept as Awareness::NextOfDead and
    // WalkStalled keep it (a walk updates it, a stall is reported as the walk's failure and the rule asked again, a
    // step that does not walk pauses it). A ghost that stands (within 2 yd) holds no walk of one kind on one draw for
    // more than WALK_PROGRESS_SECONDS plus one step; a ghost that walks at 1.5 yd/s or faster is never stalled
    counterexamples = 0;
    int stalls = 0;
    int walking = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        bool const walks = generator.Next() % 2 == 0;
        float const speed = generator.Range(1.5f, 10.0f);
        float const from = generator.Range(0.0f, 6.2832f);
        float const far = generator.Range(70.0f, 600.0f);
        float gx = far * std::cos(from);  // the ghost; the corpse at 0, 0
        float gy = far * std::sin(from);
        float const graveX = generator.Range(-600.0f, 600.0f);
        float const graveY = generator.Range(-600.0f, 600.0f);
        float const leaderX = generator.Range(-30.0f, 30.0f);
        float const leaderY = generator.Range(-30.0f, 30.0f);
        DeadFacts drawn = RandomDeadFacts(generator);
        PastLineOne(drawn);
        std::uint32_t seconds = generator.Next() % (GHOST_BOUND_SECONDS - 200);
        bool chosen = false;
        WalkWatch watch;
        WalkKind heldKind = WalkKind::None;
        std::uint32_t heldSteps = 0;
        bool ended = false;
        for (int t = 0; t < 200 && !ended; ++t, ++seconds)
        {
            std::uint32_t const nowMs = 1 + seconds * 1000;
            if (generator.Next() % 40 == 0)
            {
                drawn = RandomDeadFacts(generator);
                PastLineOne(drawn);
                heldKind = WalkKind::None;
            }
            DeadFacts facts = drawn;
            facts.ghostSeconds = seconds;
            facts.healerChosen = chosen;
            facts.corpseWalkFailed = false;
            facts.graveWalkFailed = false;
            DeadStep step = NextOfDead(facts);
            float tx = 0.0f;
            float ty = 0.0f;
            WalkKind kind = WalkKind::None;
            for (int ask = 0; ask < 3 && WalkOfDead(step.act); ++ask)
            {
                kind = step.act == DeadAct::WalkGrave ? WalkKind::Grave : WalkKind::Corpse;
                tx = step.act == DeadAct::WalkGrave                                        ? graveX
                     : step.act == DeadAct::WalkCorpse                                     ? 0.0f
                     : step.act == DeadAct::WalkPoint && step.reason == DeadReason::Leader ? leaderX
                                                                                           : facts.best.x;
                ty = step.act == DeadAct::WalkGrave                                        ? graveY
                     : step.act == DeadAct::WalkCorpse                                     ? 0.0f
                     : step.act == DeadAct::WalkPoint && step.reason == DeadReason::Leader ? leaderY
                                                                                           : facts.best.y;
                watch = WatchWalk(watch, kind, tx, ty, PlanarDistance(gx, gy, tx, ty), nowMs);
                if (!WalkStalled(watch, nowMs))
                    break;
                ++stalls;
                counterexamples += walks;  // a ghost that comes nearer is never stalled
                (kind == WalkKind::Grave ? facts.graveWalkFailed : facts.corpseWalkFailed) = true;
                step = NextOfDead(facts);
            }
            chosen = chosen || ChoosesHealer(step);
            if (!WalkOfDead(step.act))
            {
                watch = WatchPause(watch, nowMs);
                ended = Revives(step.act) || step.act == DeadAct::Rise || step.act == DeadAct::TeleportGrave;
                continue;
            }
            // The walk carried out: a standing ghost on one draw holds it at most WALK_PROGRESS_SECONDS + 1 steps
            heldSteps = kind == heldKind ? heldSteps + 1 : 1;
            heldKind = kind;
            counterexamples += !walks && heldSteps > WALK_PROGRESS_SECONDS + 1;
            float const yards = PlanarDistance(gx, gy, tx, ty);
            if (walks)
            {
                ++walking;
                float const stepYards = std::min(speed, std::max(0.0f, yards - 1.0f));
                if (yards > 0.0f)
                {
                    gx += (tx - gx) / yards * stepYards;
                    gy += (ty - gy) / yards * stepYards;
                }
            }
            else
            {
                gx += generator.Range(-1.0f, 1.0f);
                gy += generator.Range(-1.0f, 1.0f);
                gx = std::clamp(gx, far * std::cos(from) - 2.0f, far * std::cos(from) + 2.0f);
                gy = std::clamp(gy, far * std::sin(from) - 2.0f, far * std::sin(from) + 2.0f);
            }
        }
    }
    std::printf("AW33b reading: P11 reported %d stalls, %d steps walked\n", stalls, walking);
    Check(stalls > INVARIANT_CASES / 10, "P11: the standing ghosts' walks stall");
    Check(walking > INVARIANT_CASES * 10, "P11: the walking ghosts walk");
    ReportDead("P11 stalled-walk-ends", INVARIANT_CASES, counterexamples, SEED);

    // P12 The revive at the grave is at its point (AW33b): over any facts HealerRevive and ReviveGrave imply
    // atGravePoint; the same facts off the point (a ghost in sight of the grave or farther) never revive at the grave
    counterexamples = 0;
    int graveRevives = 0;
    int offPoint = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        if (generator.Next() % 2)
            PastLineOne(facts);
        DeadStep const step = NextOfDead(facts);
        bool const atGraveRevive = step.act == DeadAct::HealerRevive || step.act == DeadAct::ReviveGrave;
        graveRevives += atGraveRevive;
        counterexamples += atGraveRevive && !facts.atGravePoint;
        DeadFacts off = facts;
        off.atGravePoint = false;
        DeadStep const offStep = NextOfDead(off);
        offPoint += HealerWay(offStep.act) || offStep.act == DeadAct::TeleportGrave;
        counterexamples += offStep.act == DeadAct::HealerRevive || offStep.act == DeadAct::ReviveGrave;
    }
    std::printf("AW33b reading: P12 judged %d revives at the grave, %d steps off its point\n", graveRevives, offPoint);
    Check(graveRevives > INVARIANT_CASES / 20, "P12: the generated facts give revives at the grave to judge");
    Check(offPoint > INVARIANT_CASES / 20, "P12: the generated facts give steps off the grave's point");
    ReportDead("P12 revive-at-point", INVARIANT_CASES, counterexamples, SEED);

    // P13 No healer's rise into deadly ground (AW38a): with a corpse on this map and before the bound, a grave in
    // deadly ground gives no WalkGrave, TeleportGrave, HealerRevive or ReviveGrave, and no WaitGrave but AW30b's
    // (no_healer, for a corpse walk that could not start where the grave had no healer)
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        if (generator.Next() % 2)
            TowardRise(facts, generator);
        PastLineOne(facts);
        facts.ghostSeconds %= GHOST_BOUND_SECONDS;
        facts.corpseHere = true;
        facts.graveGround = Ground::Deadly;
        DeadStep const step = NextOfDead(facts);
        bool const aw30b = step == DeadStep{DeadAct::WaitGrave, DeadReason::NoHealer} && facts.graveWithoutHealer &&
                           facts.corpseWalkFailed;
        counterexamples += step.act == DeadAct::WalkGrave || step.act == DeadAct::TeleportGrave ||
                           step.act == DeadAct::HealerRevive || step.act == DeadAct::ReviveGrave ||
                           (step.act == DeadAct::WaitGrave && !aw30b);
    }
    ReportDead("P13 no-rise-into-deadly-ground", INVARIANT_CASES, counterexamples, SEED);

    // P14 The rule is total over the grounds (AW38a): P1 (the bound is total) and P3 (the wait ends) for each of the
    // nine pairs of grounds, a ninth of the cases each; P2 drew a pair per run, and every run of every pair ended
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        PastLineOne(facts);
        facts.corpseGround = static_cast<Ground>(i % 3);
        facts.graveGround = static_cast<Ground>(i / 3 % 3);
        DeadFacts bound = facts;
        bound.ghostSeconds = GHOST_BOUND_SECONDS + (generator.Next() % 4 == 0 ? 0 : generator.Next() % 3600);
        DeadStep const boundStep = NextOfDead(bound);
        bool const ends = Revives(boundStep.act) || boundStep.act == DeadAct::TeleportGrave;
        counterexamples += !ends || (bound.atGravePoint && boundStep.act == DeadAct::TeleportGrave);
        DeadFacts waited = facts;
        if (waited.best.clear)
        {
            waited.best.answer = OneMoreReaching(waited.best.answer, generator);
            waited.best.answer.inView = true;
            waited.best.clear = false;
        }
        waited.ownedByPlayer = false;
        waited.unclearSeconds = UNCLEAR_WAIT_SECONDS + (generator.Next() % 4 == 0 ? 0 : generator.Next() % 600);
        counterexamples += NextOfDead(waited) == DeadStep{DeadAct::Wait, DeadReason::NoClearPoint};
    }
    for (std::size_t pair = 0; pair < pairRuns.size(); ++pair)
        counterexamples += pairRuns[pair] < INVARIANT_CASES / 20 || pairUnended[pair] > 0;
    ReportDead("P14 total-over-grounds", INVARIANT_CASES, counterexamples, SEED);

    // P15 Quiet grounds change nothing (AW38a): with both grounds Quiet and no grave without a healer, every step is
    // the step of the rule before AW38a (NextOfDeadBefore38a), over generated facts, half of them toward a rise; with
    // a grave without a healer and both grounds Quiet the rule differs only where line 8's wider test on the closed
    // way applies (not a player's bot, a best point it holds at)
    counterexamples = 0;
    int widened = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        TowardRise(facts, generator);
        if (generator.Next() % 2)
            PastLineOne(facts);
        facts.corpseGround = Ground::Quiet;
        facts.graveGround = Ground::Quiet;
        DeadFacts quiet = facts;
        quiet.graveWithoutHealer = false;
        counterexamples += !(NextOfDead(quiet) == NextOfDeadBefore38a(quiet));
        DeadStep const now = NextOfDead(facts);
        DeadStep const before = NextOfDeadBefore38a(facts);
        bool const wider = !facts.ownedByPlayer && facts.best.found && HoldsAtRise(facts.best.answer);
        widened += !(now == before);
        counterexamples += !(now == before) && !(ClosedWay(facts) && wider);
    }
    std::printf("AW38a reading: P15 the closed way's wider rise changed %d steps of a grave without a healer\n",
                widened);
    ReportDead("P15 quiet-grounds-change-nothing", INVARIANT_CASES, counterexamples, SEED);

    // P16 The corpse's ground only chooses the healer (AW38a): a worse corpseGround, all else equal, changes a step
    // only into a step of the healer's way
    counterexamples = 0;
    int chosenByGround = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        TowardRise(facts, generator);
        if (generator.Next() % 2)
            PastLineOne(facts);
        int const from = static_cast<int>(generator.Next() % 2);
        facts.corpseGround = static_cast<Ground>(from);
        DeadFacts worse = facts;
        worse.corpseGround = static_cast<Ground>(from + 1 + static_cast<int>(generator.Next() % (2 - from)));
        DeadStep const step = NextOfDead(facts);
        DeadStep const worseStep = NextOfDead(worse);
        bool const changed = !(step == worseStep);
        chosenByGround += changed;
        counterexamples += changed && !HealerWay(worseStep.act);
    }
    std::printf("AW38a reading: P16 a worse corpse ground chose the healer in %d cases\n", chosenByGround);
    Check(chosenByGround > INVARIANT_CASES / 100, "P16: the generated facts give steps the corpse's ground changes");
    ReportDead("P16 corpse-ground-only-healer", INVARIANT_CASES, counterexamples, SEED);

    // SR1 Every self-rise run ends (AW33b): a body's steps at drawn delays (most a tick, some up to 5 s), its place
    // drawn once and anew now and then: the run ends by Rise or Release, and the first step asked at or past
    // SELF_RISE_WAIT_SECONDS ends it (within the wait and one step). SR2 A Rise has its cause: a clear place in view,
    // a battleground or a player's bot; Wait only before the wait is over
    counterexamples = 0;
    int selfRises = 0;
    int selfReleases = 0;
    int allowedFails = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        std::uint32_t ms = generator.Next() % 2000;
        bool const rare = generator.Next() % 8 == 0;
        PointAnswer place = RandomAnswer(generator);  // the place, drawn anew now and then (a creature comes or goes)
        bool ended = false;
        while (!ended)
        {
            if (generator.Next() % 40 == 0)
                place = RandomAnswer(generator);
            SelfRiseFacts facts;
            facts.inBattleground = rare && generator.Next() % 8 == 0;
            facts.ownedByPlayer = rare && generator.Next() % 8 == 0;
            facts.bodySeconds = ms / 1000;
            facts.own = place;
            SelfRiseAct const act = SelfRiseStep(facts);
            bool const cause = (facts.own.clear && facts.own.inView) || facts.inBattleground || facts.ownedByPlayer;
            allowedFails += (act == SelfRiseAct::Rise && !cause) ||
                            (act == SelfRiseAct::Wait && facts.bodySeconds >= SELF_RISE_WAIT_SECONDS);
            ended = act != SelfRiseAct::Wait;
            counterexamples += !ended && facts.bodySeconds >= SELF_RISE_WAIT_SECONDS;
            selfRises += act == SelfRiseAct::Rise;
            selfReleases += act == SelfRiseAct::Release;
            if (!ended && facts.bodySeconds >= SELF_RISE_WAIT_SECONDS)
                break;  // counted: a wait past the bound of the self-rise
            ms += generator.Next() % 8 == 0 ? 1 + generator.Next() % 5000 : 1 + generator.Next() % 400;
        }
    }
    std::printf("AW33b reading: SR1 %d self-rises, %d releases\n", selfRises, selfReleases);
    Check(selfRises > INVARIANT_CASES / 4 && selfReleases > INVARIANT_CASES / 20,
          "SR1: the generated bodies both rise and release");
    ReportDead("SR1 self-rise-ends", INVARIANT_CASES, counterexamples, SEED);
    ReportDead("SR2 self-rise-allowed", INVARIANT_CASES, allowedFails, SEED);
}

// AW9: the row of deaths (§3.5, P7), the recovery (§3.6, P8), the return's memo and the `death` rows. Each generated
// invariant prints its name, cases, counterexamples and the seed
void ReportAw9(char const* name, int cases, int counterexamples, std::uint32_t seed)
{
    std::printf("AW9 invariant %s: %d cases, %d counterexamples (seed %u)\n", name, cases, counterexamples, seed);
    Check(cases >= INVARIANT_CASES && counterexamples == 0, name);
}

RowDeath InRow()
{
    RowDeath death;
    death.rose = true;
    death.sinceRiseSeconds = 60;
    death.sameMap = true;
    death.riseYards = 10.0f;
    return death;
}

bool RowIs(char const* way, char const* reason, DeadAct act, DeadReason stepReason, DeadReason healerReason)
{
    WorldTravel::GhostWayResult const row = DeathRowOf({act, stepReason}, healerReason);
    return std::strcmp(WorldTravel::GhostWayWord(row.Way), way) == 0 &&
           std::strcmp(WorldTravel::GhostReasonWord(row.Reason), reason) == 0;
}

void TestAw9Fixed()
{
    // The row (P7)
    Check(RowAfterDeath(0, RowDeath{}) == 1, "row: a first death since login is 1");
    Check(RowAfterDeath(1, InRow()) == 2 && RowAfterDeath(2, InRow()) == 3, "row: a death in a row adds 1");
    RowDeath death = InRow();
    death.sinceRiseSeconds = ROW_SECONDS - 1;
    Check(RowAfterDeath(2, death) == 3, "row: 299 s after the rise is in the row");
    death.sinceRiseSeconds = ROW_SECONDS;
    Check(RowAfterDeath(2, death) == 1, "row: 300 s after the rise starts a new row");
    death = InRow();
    death.riseYards = ROW_YARDS;
    Check(RowAfterDeath(2, death) == 3, "row: 40 yd from the rise is in the row");
    death.riseYards = ROW_YARDS + 0.01f;
    Check(RowAfterDeath(2, death) == 1, "row: beyond 40 yd starts a new row");
    death = InRow();
    death.sameMap = false;
    Check(RowAfterDeath(2, death) == 1, "row: another map starts a new row");
    death = InRow();
    death.rose = false;
    Check(RowAfterDeath(4, death) == 1, "row: no rise since the login starts a new row");
    Check(RowAtRise(0) == 1 && RowAtRise(1) == 1 && RowAtRise(3) == 3, "row: a rise leaves 1 at least, else the count");
    {
        // P7's named case: a kill, experience and a repair between two deaths change nothing (upstream's death count,
        // which they reset, is no input of the row); the healer's rise beside its grave keeps the row
        std::uint32_t upstreamCount = 2;
        std::uint32_t row = RowAtRise(RowAfterDeath(0, RowDeath{}));
        upstreamCount = 0;  // the kill's experience resets upstream's count, and so does a repair
        std::uint32_t const second = RowAfterDeath(row, InRow());
        row = RowAtRise(second);
        upstreamCount = 0;
        std::uint32_t const third = RowAfterDeath(row, InRow());
        Check(upstreamCount == 0 && second == 2 && third == 3 && third >= ROW_HEALER_AT,
              "P7 named: a kill, experience or a repair between two deaths changes nothing; the third takes the "
              "healer");
    }

    // The memo's functions
    Check(DeathKey(500, 0, 0) == 500 && DeathKey(500, 7, 700) == 500, "key: the death edge's stamp first");
    Check(DeathKey(0, 7, 700) == 7 && DeathKey(0, 7, 0) == 7, "key: then the memo's own");
    Check(DeathKey(0, 0, 700) == 700 && DeathKey(0, 0, 0) == 1, "key: then the corpse's ghost time, else 1");
    GhostClock clock;
    clock.sinceLogin = 10000;
    clock.corpse = true;
    clock.sinceCorpse = 120;
    clock.edge = true;
    clock.sinceEdge = 130;
    clock.sinceMemo = 5;
    Check(GhostSecondsOf(clock) == 120, "ghost seconds: from the corpse when it lies here");
    clock.corpse = false;
    Check(GhostSecondsOf(clock) == 130, "ghost seconds: from the death edge without a corpse");
    clock.edge = false;
    Check(GhostSecondsOf(clock) == 5, "ghost seconds: from the memo's first ask without either");
    clock.sinceMemo = 20000;
    Check(GhostSecondsOf(clock) == 10000, "ghost seconds: never more than since the login");
    clock.sinceMemo = -4;
    Check(GhostSecondsOf(clock) == 0, "ghost seconds: a clock that went back counts 0");
    SafePoint best;
    best.found = true;
    best.clear = true;
    Check(UnclearStart(77, best, 900) == 0, "unclear: a look with a clear point clears the clock");
    best.clear = false;
    Check(UnclearStart(0, best, 900) == 900 && UnclearStart(0, best, 0) == 1, "unclear: the first look starts it");
    Check(UnclearStart(77, best, 900) == 77, "unclear: a later look keeps it");
    best.found = false;
    best.outOfTests = true;
    Check(UnclearStart(0, best, 900) == 0 && UnclearStart(77, best, 900) == 77,
          "unclear: a search out of tests before it found anything is no look");
    best.outOfTests = false;
    Check(UnclearStart(0, best, 900) == 900, "unclear: a full search without any point starts it");
    best.found = true;
    best.x = 10.0f;
    best.y = 0.0f;
    Check(AtBest(best, 10.0f + ARRIVE_YARDS, 0.0f) && !AtBest(best, 10.0f + ARRIVE_YARDS + 0.01f, 0.0f),
          "at best: within ARRIVE_YARDS");
    best.found = false;
    Check(!AtBest(best, 10.0f, 0.0f), "at best: no point, never at it");
    DeadStep const walk{DeadAct::WalkPoint, DeadReason::Clear};
    Check(WalkGoesOn(true, walk, walk, ARRIVE_YARDS), "walk: the same step, the point moved a little: goes on");
    Check(!WalkGoesOn(true, walk, walk, ARRIVE_YARDS + 0.01f), "walk: the point moved: a new walk");
    Check(!WalkGoesOn(false, walk, walk, 0.0f), "walk: not moving: a new walk");
    Check(!WalkGoesOn(true, walk, {DeadAct::WalkPoint, DeadReason::NoClearPoint}, 0.0f),
          "walk: another reason: a new walk");
    // AW30b: the memo's graveWithoutHealer
    Check(GraveSearchCounts(0.0f) && GraveSearchCounts(GRAVE_SEARCH_YARDS), "grave search: counts near the point");
    Check(!GraveSearchCounts(GRAVE_SEARCH_YARDS + 0.01f) && !GraveSearchCounts(100.0f),
          "grave search: one made farther off does not count (the grave comes into sight at 100 yd)");
    Check(GraveSearchCounts(ARRIVE_YARDS) && GRAVE_SEARCH_YARDS < 100.0f,
          "grave search: a ghost that walked to the grave's point stands where a search counts");
    Check(GraveWithoutHealer(false, true, true, false), "grave without healer: at it, a search counted, none found");
    Check(!GraveWithoutHealer(false, false, true, false), "grave without healer: not at the grave: not set");
    Check(!GraveWithoutHealer(false, true, false, false), "grave without healer: no search counted: not set");
    Check(!GraveWithoutHealer(false, true, true, true), "grave without healer: a healer found: not set");
    Check(!GraveWithoutHealer(false, false, false, true) && !GraveWithoutHealer(false, false, false, false),
          "grave without healer: nothing seen: not set");
    Check(GraveWithoutHealer(true, false, false, false) && GraveWithoutHealer(true, true, true, true),
          "grave without healer: once set it stays for this death, also when a healer is found later");

    // The cause and the `death` rows (today's words)
    Check(HealerCause({DeadAct::WalkGrave, DeadReason::NoPath}, DeadReason::CorpseCamped) == DeadReason::CorpseCamped,
          "cause: the memo's cause of the healer's way");
    Check(HealerCause({DeadAct::TeleportGrave, DeadReason::WaitEnded}, DeadReason::CorpseAway) == DeadReason::WaitEnded,
          "cause: the bound names itself");
    Check(HealerCause({DeadAct::HealerRevive, DeadReason::NoClearPoint}, DeadReason::None) == DeadReason::NoClearPoint,
          "cause: without a kept cause the step's reason");
    Check(RowIs("corpse_walk", "moving", DeadAct::WalkCorpse, DeadReason::None, DeadReason::None) &&
              RowIs("corpse_walk", "moving", DeadAct::WalkPoint, DeadReason::Leader, DeadReason::None),
          "death row: corpse_walk/moving");
    Check(RowIs("wait", "no_clear_point", DeadAct::Wait, DeadReason::NoClearPoint, DeadReason::None) &&
              RowIs("wait", "no_healer", DeadAct::Wait, DeadReason::NoHealer, DeadReason::CorpseRunsExhausted) &&
              RowIs("wait", "player", DeadAct::Wait, DeadReason::Player, DeadReason::None) &&
              RowIs("wait", "no_path", DeadAct::Wait, DeadReason::NoPath, DeadReason::None) &&
              RowIs("wait", "grave_danger", DeadAct::Wait, DeadReason::GraveDanger, DeadReason::None) &&
              RowIs("none", "none", DeadAct::Wait, DeadReason::LeaderDown, DeadReason::None),
          "death row: wait/no_clear_point, wait/no_healer, wait/player, wait/no_path (AW38a), wait/grave_danger "
          "(AW38a), no row for the other waits");
    Check(RowIs("corpse_walk", "reached_holds", DeadAct::WalkPoint, DeadReason::ReachedHolds, DeadReason::None) &&
              RowIs("healer_walk", "deadly_ground", DeadAct::WalkGrave, DeadReason::DeadlyGround,
                    DeadReason::DeadlyGround) &&
              RowIs("teleport", "deadly_ground", DeadAct::TeleportGrave, DeadReason::None, DeadReason::DeadlyGround) &&
              !std::strcmp(RiseWord(DeadStep{DeadAct::Rise, DeadReason::ReachedHolds}, true, true), "reached_holds") &&
              !std::strcmp(Word(DeadReason::DeadlyGround), "deadly_ground"),
          "death row (AW38a): corpse_walk/reached_holds, healer_walk and teleport deadly_ground, Rise reached_holds");
    Check(RowIs("healer_walk", "corpse_runs_exhausted", DeadAct::WalkGrave, DeadReason::CorpseRunsExhausted,
                DeadReason::CorpseRunsExhausted) &&
              RowIs("healer_walk", "corpse_camped", DeadAct::WalkGrave, DeadReason::None, DeadReason::CorpseCamped) &&
              RowIs("healer_walk", "corpse_away", DeadAct::WalkGrave, DeadReason::CorpseAway, DeadReason::None) &&
              RowIs("healer_walk", "no_path", DeadAct::WalkGrave, DeadReason::NoPath, DeadReason::NoPath) &&
              RowIs("healer_walk", "no_clear_point", DeadAct::WalkGrave, DeadReason::None, DeadReason::NoClearPoint),
          "death row: healer_walk/<cause>");
    Check(RowIs("healer_wait", "no_healer", DeadAct::WaitGrave, DeadReason::NoHealer, DeadReason::NoPath) &&
              RowIs("blocked", "seen", DeadAct::WaitGrave, DeadReason::Seen, DeadReason::NoPath) &&
              RowIs("none", "none", DeadAct::WaitGrave, DeadReason::NoGrave, DeadReason::NoPath),
          "death row: healer_wait/no_healer, blocked/seen, no row without a grave");
    Check(RowIs("teleport", "no_path", DeadAct::TeleportGrave, DeadReason::NoPath, DeadReason::NoPath) &&
              RowIs("teleport", "wait_ended", DeadAct::TeleportGrave, DeadReason::WaitEnded, DeadReason::NoPath),
          "death row: teleport/<cause> and teleport/wait_ended");
    Check(RowIs("none", "none", DeadAct::Rise, DeadReason::Clear, DeadReason::None) &&
              RowIs("none", "none", DeadAct::HealerRevive, DeadReason::NoPath, DeadReason::NoPath) &&
              RowIs("none", "none", DeadAct::ReviveGrave, DeadReason::WaitEnded, DeadReason::None) &&
              RowIs("none", "none", DeadAct::ReviveHere, DeadReason::WaitEnded, DeadReason::None) &&
              RowIs("none", "none", DeadAct::None, DeadReason::None, DeadReason::None),
          "death row: none for the rise, the revives and no step");

    // The recovery (P8)
    RecoverFacts facts;
    Check(RecoverStep(facts) == RecoverAct::Done, "recover: risen whole and well: done");
    facts.healthPct = 50;
    facts.sinceRiseSeconds = 10;
    Check(RecoverStep(facts) == RecoverAct::Rest, "recover: half health on a clear place: rest");
    Check(KeepsOnFoot(facts), "recover: a resting bot keeps on foot: mounted it neither eats nor drinks");
    facts.own.clear = false;
    Check(RecoverStep(facts) == RecoverAct::Move, "recover: half health on a reached place: move");
    Check(KeepsOnFoot(facts), "recover: and walks to its clear point on foot");
    facts.own.clear = true;
    facts.healthPct = RECOVER_HEALTH_PCT;
    Check(RecoverStep(facts) == RecoverAct::Done, "recover: 80 % health is restored");
    facts.healthPct = RECOVER_HEALTH_PCT - 1;
    Check(RecoverStep(facts) == RecoverAct::Rest, "recover: 79 % health is not");
    facts.healthPct = 100;
    facts.powerPct = RECOVER_POWER_PCT - 1;
    Check(RecoverStep(facts) == RecoverAct::Done, "recover: low power of a bot without mana counts nothing");
    facts.usesMana = true;
    Check(RecoverStep(facts) == RecoverAct::Rest, "recover: a mana user at 79 % mana rests");
    facts.powerPct = RECOVER_POWER_PCT;
    Check(RecoverStep(facts) == RecoverAct::Done, "recover: and is restored at 80 %");
    Check(!KeepsOnFoot(facts), "recover: restored, it may mount");
    facts.healthPct = 10;
    facts.sinceRiseSeconds = RECOVER_BOUND_SECONDS - 1;
    Check(RecoverStep(facts) == RecoverAct::Rest, "recover: before the bound it rests");
    facts.sinceRiseSeconds = RECOVER_BOUND_SECONDS;
    Check(RecoverStep(facts) == RecoverAct::Done, "recover: at the bound it is done");
    Check(!KeepsOnFoot(facts), "recover: at the bound it may mount unrestored");
    facts.sick = true;
    facts.healthPct = 100;
    facts.sinceRiseSeconds = 500;
    Check(RecoverStep(facts) == RecoverAct::Rest, "recover: sick: still recovering past the bound");
    Check(!KeepsOnFoot(facts), "recover: a sick bot is not held: the sickness alone keeps none on foot");
    facts.own.clear = false;
    Check(RecoverStep(facts) == RecoverAct::Move, "recover: sick on a reached place: move");
}

void TestAw9Generated()
{
    Generator generator;
    // P7 The row: one more inside the three limits, else 1
    int counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        std::uint32_t const row = generator.Next() % 50;
        RowDeath death;
        death.rose = generator.Next() % 8 != 0;
        std::uint32_t const edge = generator.Next() % 4;
        death.sinceRiseSeconds =
            edge == 0 ? ROW_SECONDS - 1 + generator.Next() % 2 : generator.Next() % (3 * ROW_SECONDS);
        death.sameMap = generator.Next() % 6 != 0;
        death.riseYards =
            edge == 1 ? ROW_YARDS + (generator.Next() % 2 ? 0.0f : 0.01f) : generator.Range(0.0f, 3 * ROW_YARDS);
        bool const inside =
            death.rose && death.sinceRiseSeconds + 1 <= ROW_SECONDS && death.sameMap && !(death.riseYards > ROW_YARDS);
        counterexamples += RowAfterDeath(row, death) != (inside ? row + 1 : 1u);
    }
    ReportAw9("P7 row", INVARIANT_CASES, counterexamples, SEED);

    // P7 over a bot's events: deaths, rises (at the corpse or by the healer), kills, experience and repairs in any
    // order, some bots dead at login. Each death's count is the oracle's: 1 plus the count of the death before when it
    // comes within the limits of the rise after that death, else 1; kills, experience and repairs are no input
    counterexamples = 0;
    int deaths = 0;
    while (deaths < INVARIANT_CASES)
    {
        std::uint32_t row = 0;
        bool rose = false;
        std::uint32_t roseAt = 0;
        std::uint32_t roseMap = 0;
        float roseX = 0.0f;
        std::uint32_t oracle = 0;
        std::uint32_t now = 0;
        bool dead = generator.Next() % 4 == 0;  // dead at login: no death edge
        for (int step = 0; step < 30; ++step)
        {
            now += generator.Next() % 200;
            std::uint32_t const map = generator.Next() % 8 == 0 ? 1 : 0;
            float const x = generator.Range(-60.0f, 60.0f);
            if (generator.Next() % 5 != 0)
                continue;  // a kill, experience, a repair: nothing of the row
            if (dead)
            {
                row = RowAtRise(row);
                oracle = oracle ? oracle : 1;
                rose = true;
                roseAt = now;
                roseMap = map;
                roseX = x;
                dead = false;
                continue;
            }
            RowDeath death;
            death.rose = rose;
            death.sinceRiseSeconds = now - roseAt;
            death.sameMap = roseMap == map;
            death.riseYards = std::fabs(x - roseX);
            row = RowAfterDeath(row, death);
            bool const inside =
                rose && now - roseAt < ROW_SECONDS && roseMap == map && std::fabs(x - roseX) <= ROW_YARDS;
            oracle = inside ? oracle + 1 : 1;
            counterexamples += row != oracle;
            ++deaths;
            dead = true;
        }
    }
    ReportAw9("P7 row-over-events", deaths, counterexamples, SEED);

    // P8 The recovery ends: Done whenever not sick and either restored or past the bound; Move only when the place is
    // not clear; Rest only while recovering on a clear place
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        RecoverFacts facts;
        facts.sinceRiseSeconds = generator.Next() % 3 == 0 ? RECOVER_BOUND_SECONDS - 1 + generator.Next() % 2
                                                           : generator.Next() % (4 * RECOVER_BOUND_SECONDS);
        facts.healthPct =
            generator.Next() % 3 == 0 ? RECOVER_HEALTH_PCT - 1 + generator.Next() % 2 : generator.Next() % 101;
        facts.powerPct =
            generator.Next() % 3 == 0 ? RECOVER_POWER_PCT - 1 + generator.Next() % 2 : generator.Next() % 101;
        facts.usesMana = generator.Next() % 2;
        facts.sick = generator.Next() % 4 == 0;
        facts.own = RandomAnswer(generator);
        RecoverAct const act = RecoverStep(facts);
        bool const restored = facts.healthPct >= 80 && (!facts.usesMana || facts.powerPct >= 80);
        bool const pastBound = facts.sinceRiseSeconds >= 60;
        bool const done = !facts.sick && (restored || pastBound);
        counterexamples += done != (act == RecoverAct::Done);
        counterexamples += done == Recovering(facts);
        counterexamples += act == RecoverAct::Move && facts.own.clear;
        counterexamples += act == RecoverAct::Rest && !facts.own.clear;
    }
    ReportAw9("P8 recovery-ends", INVARIANT_CASES, counterexamples, SEED);

    // The return's memo: the death key stays while the corpse comes onto the ghost's map or leaves it, and a new
    // death edge starts a new one
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        std::uint32_t const edge = generator.Next() % 3 == 0 ? 0 : 1 + generator.Next() % 100000;
        std::uint32_t const corpseFirst = generator.Next() % 2 ? 0 : 1 + generator.Next() % 100000;
        std::uint32_t const corpseLater = generator.Next() % 2 ? 0 : 1 + generator.Next() % 100000;
        std::uint32_t const first = DeathKey(edge, 0, corpseFirst);
        counterexamples += first == 0 || DeathKey(edge, first, corpseLater) != first;
        std::uint32_t const nextEdge = edge + 1 + generator.Next() % 1000;
        counterexamples += DeathKey(nextEdge, first, corpseLater) != nextEdge;
    }
    ReportAw9("memo death-key-stable", INVARIANT_CASES, counterexamples, SEED);

    // The unclear clock is 0 whenever the look found a clear point (P6's condition); a look without one leaves it
    // running or starts it, never at 0; a search out of tests that found nothing changes nothing
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        std::uint32_t const start = generator.Next() % 3 == 0 ? 0 : generator.Next();
        std::uint32_t const now = generator.Next() % 8 == 0 ? 0 : generator.Next();
        SafePoint best;
        best.found = generator.Next() % 3 != 0;
        best.clear = best.found && generator.Next() % 2;
        best.outOfTests = generator.Next() % 3 == 0;
        std::uint32_t const next = UnclearStart(start, best, now);
        bool const look = best.found || !best.outOfTests;
        if (best.found && best.clear)
            counterexamples += next != 0;
        else if (start)
            counterexamples += next != start;
        else if (look)
            counterexamples += next != (now ? now : 1u);
        else
            counterexamples += next != 0;
    }
    ReportAw9("memo unclear-clock", INVARIANT_CASES, counterexamples, SEED);

    // A ghost whose corpse lies on another map reaches the bound: its seconds come from the death edge or the memo's
    // clock, and past the bound the rule's step is one of the bound's (P1 at game level)
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        PastLineOne(facts);
        facts.ownedByPlayer = false;
        facts.corpseHere = false;
        GhostClock clock;
        clock.edge = generator.Next() % 2;
        clock.sinceEdge = generator.Next() % (2 * GHOST_BOUND_SECONDS);
        clock.sinceMemo = generator.Next() % (2 * GHOST_BOUND_SECONDS);
        clock.sinceLogin = generator.Next() % 4 == 0 ? generator.Next() % (2 * GHOST_BOUND_SECONDS) : 100000;
        std::int64_t const since = std::min(clock.edge ? clock.sinceEdge : clock.sinceMemo, clock.sinceLogin);
        facts.ghostSeconds = static_cast<std::uint32_t>(GhostSecondsOf(clock));
        DeadStep const step = NextOfDead(facts);
        bool const bound = step.reason == DeadReason::WaitEnded &&
                           (step.act == DeadAct::HealerRevive || step.act == DeadAct::ReviveGrave ||
                            step.act == DeadAct::ReviveHere || step.act == DeadAct::TeleportGrave);
        counterexamples += facts.ghostSeconds != since;
        counterexamples += (since >= GHOST_BOUND_SECONDS) != bound;
    }
    ReportAw9("memo ghost-away-bound", INVARIANT_CASES, counterexamples, SEED);

    // Every step's `death` row, over every step, reason and kept cause: the oracle's table
    int rows = 0;
    counterexamples = 0;
    for (int a = 0; a < static_cast<int>(DeadAct::Count); ++a)
    {
        for (int r = 0; r < static_cast<int>(DeadReason::Count); ++r)
        {
            for (int h = 0; h < static_cast<int>(DeadReason::Count); ++h)
            {
                DeadStep const step{static_cast<DeadAct>(a), static_cast<DeadReason>(r)};
                DeadReason const kept = static_cast<DeadReason>(h);
                DeadReason const cause = step.reason == DeadReason::WaitEnded ? DeadReason::WaitEnded
                                         : kept != DeadReason::None           ? kept
                                                                              : step.reason;
                std::string causeWord = "none";
                for (DeadReason const c :
                     {DeadReason::CorpseAway, DeadReason::CorpseRunsExhausted, DeadReason::CorpseCamped,
                      DeadReason::NoPath, DeadReason::NoClearPoint, DeadReason::WaitEnded, DeadReason::DeadlyGround})
                {
                    if (cause == c)
                        causeWord = Word(c);
                }
                std::string way = "none";
                std::string reason = "none";
                if (step.act == DeadAct::WalkCorpse || step.act == DeadAct::WalkPoint)
                    way = "corpse_walk", reason = step.reason == DeadReason::ReachedHolds ? "reached_holds" : "moving";
                else if (step.act == DeadAct::Wait && step.reason == DeadReason::NoClearPoint)
                    way = "wait", reason = "no_clear_point";
                else if (step.act == DeadAct::Wait && step.reason == DeadReason::NoHealer)
                    way = "wait", reason = "no_healer";
                else if (step.act == DeadAct::Wait && step.reason == DeadReason::Player)
                    way = "wait", reason = "player";
                else if (step.act == DeadAct::Wait && step.reason == DeadReason::NoPath)
                    way = "wait", reason = "no_path";
                else if (step.act == DeadAct::Wait && step.reason == DeadReason::GraveDanger)
                    way = "wait", reason = "grave_danger";
                else if (step.act == DeadAct::WalkGrave)
                    way = "healer_walk", reason = causeWord;
                else if (step.act == DeadAct::TeleportGrave)
                    way = "teleport", reason = causeWord;
                else if (step.act == DeadAct::WaitGrave && step.reason == DeadReason::NoHealer)
                    way = "healer_wait", reason = "no_healer";
                else if (step.act == DeadAct::WaitGrave && step.reason == DeadReason::Seen)
                    way = "blocked", reason = "seen";
                WorldTravel::GhostWayResult const row = DeathRowOf(step, kept);
                counterexamples +=
                    way != WorldTravel::GhostWayWord(row.Way) || reason != WorldTravel::GhostReasonWord(row.Reason);
                ++rows;
            }
        }
    }
    std::printf("AW9 every death row: %d steps, %d counterexamples\n", rows, counterexamples);
    Check(counterexamples == 0, "AW9 every death row as the oracle's table");
}
// AW31: the world log's hooks on the rules of DeathEdgeRules.h, as WorldLogHooks.cpp runs them for one player. A
// kill hook names the killer and writes the killer's side (QuestStatDeath, NoteTravelDeath, PvpKill) once per life;
// the edge writes the rows; a resurrect writes a still-named death's rows first, then its own; a kill hook whose
// victim a battleground's end revived first (alive, out of the redemption form) writes the rows itself
struct EdgePlayer
{
    DeathEdge::Killer life;
    std::vector<std::string> rows;  // Death:<word> PvpDeath Resurrect, in order
    int sides = 0;                  // the killer's side written

    void Kill(DeathEdge::Kind kind, std::uint32_t id, bool alive = false, bool form = false)
    {
        DeathEdge::Killer killer;
        killer.kind = kind;
        killer.id = id;
        killer.entry = id;
        killer.unit = id;
        if (!DeathEdge::Name(life, killer))
            return;
        ++sides;
        if (DeathEdge::RowsAtKillHook(alive, form))
            Write();
    }

    void Write()
    {
        DeathEdge::Killer const killer = DeathEdge::Take(life);
        rows.push_back(std::string("Death:") + DeathEdge::DeathWord(killer.kind));
        if (DeathEdge::IsPvp(killer.kind))
            rows.push_back("PvpDeath");
    }

    void Edge() { Write(); }

    void Resurrect()
    {
        if (DeathEdge::RowsAtResurrect(life))
            Write();
        rows.push_back("Resurrect");
    }
};

using Rows = std::vector<std::string>;

void TestDeathEdgeFixed()
{
    using DeathEdge::Kind;
    Check(DeathEdge::KindOf(true, true, false) == Kind::Environment, "AW31 a player's kill of itself: environment");
    Check(DeathEdge::KindOf(true, false, true) == Kind::Player, "AW31 a player's kill: player");
    Check(DeathEdge::KindOf(false, false, false) == Kind::Creature, "AW31 a creature's kill: creature");
    Check(DeathEdge::KindOf(false, false, true) == Kind::PlayerPet, "AW31 a pet's kill: player_pet");
    Check(std::strcmp(DeathEdge::DeathWord(Kind::None), "unknown") == 0, "AW31 no killer named: unknown");
    Check(DeathEdge::IsPvp(Kind::Player) && DeathEdge::IsPvp(Kind::PlayerPet) && !DeathEdge::IsPvp(Kind::Creature) &&
              !DeathEdge::IsPvp(Kind::Environment) && !DeathEdge::IsPvp(Kind::None),
          "AW31 PvpDeath for a player's and a pet's kill only");

    Check(DeathEdge::RowsAtKillHook(true, false) && !DeathEdge::RowsAtKillHook(true, true) &&
              !DeathEdge::RowsAtKillHook(false, false) && !DeathEdge::RowsAtKillHook(false, true),
          "AW31 a kill hook writes the rows only for a victim revived out of the redemption form");

    // The rows carry the first killer's ids (the Death row's ref, the DeathScene's entry, the unit found again at the
    // edge), and the name ends whole
    DeathEdge::Killer named;
    Check(DeathEdge::Name(named, {Kind::Creature, 11, 22, 33}) && !DeathEdge::Name(named, {Kind::Player, 44, 55, 66}) &&
              named.kind == Kind::Creature && named.id == 11 && named.entry == 22 && named.unit == 33,
          "AW31 the first killer's ids stay through a second kill hook");
    DeathEdge::Killer const taken = DeathEdge::Take(named);
    Check(taken.kind == Kind::Creature && taken.id == 11 && taken.entry == 22 && taken.unit == 33 &&
              named.kind == Kind::None && named.id == 0 && named.entry == 0 && named.unit == 0,
          "AW31 the rows take the first killer's ids and leave no name");

    // Spirit of Redemption: two Unit::Kill in one life, one edge at the form's end: the first killer, once
    EdgePlayer priest;
    priest.Kill(Kind::Creature, 525, true, true);
    priest.Kill(Kind::Player, 7);
    priest.Edge();
    priest.Resurrect();
    Check(priest.rows == Rows{"Death:creature", "Resurrect"} && priest.sides == 1,
          "AW31 Spirit of Redemption killed twice: one Death creature, the killer's side once");

    // A death no kill hook sees: unknown at the edge, no killer's side; the next life is named afresh
    EdgePlayer unseen;
    unseen.Edge();
    unseen.Resurrect();
    unseen.Kill(Kind::Player, 9);
    unseen.Edge();
    unseen.Resurrect();
    Check(
        unseen.rows == Rows{"Death:unknown", "Resurrect", "Death:player", "PvpDeath", "Resurrect"} && unseen.sides == 1,
        "AW31 an unseen death writes unknown and leaves the next life unnamed");

    // A bot revived before its edge: the resurrect writes the death's rows first; no edge follows
    EdgePlayer early;
    early.Kill(Kind::PlayerPet, 11);
    early.Resurrect();
    Check(early.rows == Rows{"Death:player_pet", "PvpDeath", "Resurrect"} && early.life.kind == Kind::None,
          "AW31 revived before the edge: Death, PvpDeath, then Resurrect, the name ended");

    // A battleground that ends at the kill revives the victim before the kill hooks: Resurrect, then the rows at the
    // hook, and no name left for the next life
    EdgePlayer arena;
    arena.Resurrect();
    arena.Kill(Kind::Player, 13, true, false);
    arena.Kill(Kind::Creature, 2);
    arena.Edge();
    Check(arena.rows == Rows{"Resurrect", "Death:player", "PvpDeath", "Death:creature"} && arena.sides == 2,
          "AW31 revived by a battleground's end: the hook writes the rows, the next life is named afresh");

    // Two players: a name stays with its own player
    EdgePlayer one;
    EdgePlayer two;
    one.Kill(Kind::Creature, 1);
    two.Edge();
    one.Edge();
    Check(two.rows == Rows{"Death:unknown"} && one.rows == Rows{"Death:creature"},
          "AW31 a killer named for one player is not another's");
}

// Generated lives: each death one of the core's paths (a kill and the edge; Spirit of Redemption killed again or
// left to the form's end; a death no hook sees; a revive before the edge), for several players interleaved. Every
// death writes one Death row before its Resurrect, with the first killer's word or unknown, PvpDeath for a player's
// or a pet's kill, and the killer's side once per named life
void TestDeathEdgeGenerated()
{
    using DeathEdge::Kind;
    constexpr int PLAYERS = 4;
    constexpr int CASES = 2000;
    enum Step : std::uint8_t
    {
        KILL,
        KILL_FORM,     // the victim stays up in the redemption form
        KILL_REVIVED,  // a battleground's end revived the victim before the hooks
        EDGE,
        RESURRECT
    };
    Generator generator;
    std::uint32_t const seed = generator.state;
    int counterexamples = 0;
    for (int c = 0; c < CASES; ++c)
    {
        std::array<EdgePlayer, PLAYERS> players;
        std::array<Rows, PLAYERS> expected;
        std::array<int, PLAYERS> sides{};
        std::array<std::vector<std::pair<Step, Kind>>, PLAYERS> events;
        for (int p = 0; p < PLAYERS; ++p)
        {
            int const deaths = 1 + static_cast<int>(generator.Next() % 5);
            for (int d = 0; d < deaths; ++d)
            {
                Kind const first = static_cast<Kind>(1 + generator.Next() % 4);
                Kind const second = static_cast<Kind>(1 + generator.Next() % 4);
                // 0 a kill and its edge, 1 killed again in the redemption form, 2 the form's end, 3 a death no hook
                // sees, 4 revived before the edge, 5 revived by a battleground's end before the kill hooks
                std::uint32_t const path = generator.Next() % 6;
                if (path == 5)
                {
                    events[p].push_back({RESURRECT, Kind::None});
                    events[p].push_back({KILL_REVIVED, first});
                    expected[p].push_back("Resurrect");
                    expected[p].push_back(std::string("Death:") + DeathEdge::DeathWord(first));
                    if (DeathEdge::IsPvp(first))
                        expected[p].push_back("PvpDeath");
                    ++sides[p];
                    continue;
                }
                if (path == 3)
                {
                    events[p].push_back({EDGE, Kind::None});
                    expected[p].push_back("Death:unknown");
                }
                else
                {
                    events[p].push_back({path == 1 || path == 2 ? KILL_FORM : KILL, first});
                    if (path == 1)
                        events[p].push_back({KILL, second});
                    if (path != 4)
                        events[p].push_back({EDGE, Kind::None});
                    expected[p].push_back(std::string("Death:") + DeathEdge::DeathWord(first));
                    if (DeathEdge::IsPvp(first))
                        expected[p].push_back("PvpDeath");
                    ++sides[p];
                }
                events[p].push_back({RESURRECT, Kind::None});
                expected[p].push_back("Resurrect");
            }
        }
        std::array<std::size_t, PLAYERS> next{};
        for (;;)
        {
            int left = 0;
            for (int p = 0; p < PLAYERS; ++p)
                left += next[p] < events[p].size() ? 1 : 0;
            if (!left)
                break;
            int p = static_cast<int>(generator.Next() % PLAYERS);
            while (next[p] >= events[p].size())
                p = (p + 1) % PLAYERS;
            auto const [step, kind] = events[p][next[p]++];
            if (step == KILL || step == KILL_FORM || step == KILL_REVIVED)
                players[p].Kill(kind, static_cast<std::uint32_t>(p + 1), step != KILL, step == KILL_FORM);
            else if (step == EDGE)
                players[p].Edge();
            else
                players[p].Resurrect();
        }
        bool bad = false;
        for (int p = 0; p < PLAYERS; ++p)
            bad = bad || players[p].rows != expected[p] || players[p].sides != sides[p] ||
                  players[p].life.kind != Kind::None;
        if (bad && ++counterexamples <= 3)
            std::printf("AW31 counterexample: case %d (seed %u)\n", c, seed);
    }
    std::printf("AW31 invariant one death one row: %d cases, %d counterexamples (seed %u)\n", CASES, counterexamples,
                seed);
    Check(counterexamples == 0, "AW31 every generated death writes its rows once");
}

// The cells of a line: every separator starts a cell, so an empty last cell (a fight with no enemy row) still counts
std::vector<std::string> Cells(std::string const& text, char separator)
{
    std::vector<std::string> cells;
    std::size_t start = 0;
    for (std::size_t at = text.find(separator); at != std::string::npos; at = text.find(separator, start))
    {
        cells.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    cells.push_back(text.substr(start));
    return cells;
}

int ColumnOf(std::vector<std::string> const& head, char const* name)
{
    for (std::size_t i = 0; i < head.size(); ++i)
        if (head[i] == name)
            return static_cast<int>(i);
    return -1;
}

// AW10's fixture: the intentional fights of the boundary run with their outcome (tools/awareness/boundary.js --tsv).
// One line per fight under a header line; the columns carry what a judgement needs at the fight's start (the bot's
// level, class, item level, health and power, the group, guards and allies, the enemies' level difference, rank,
// caster, kind, unit type, reason and second in the fight as `diff:rank:caster:kind:unit:reason:secs` joined by `;`)
// and the outcome (alive: won, died: lost) with the lowest health. AW12 replays the judgement over it; here the file
// is only read and counted
void TestFightFixture(char const* file)
{
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line))
    {
        std::printf("AW10 fixture: no data (%s)\n", file);
        Check(false, "fixture: tests/data/aw-fights.tsv is read");
        return;
    }
    std::vector<std::string> const head = Cells(line, '\t');
    int const outcome = ColumnOf(head, "outcome");
    int const level = ColumnOf(head, "level");
    int const enemies = ColumnOf(head, "enemy_list");
    Check(head.size() == 27 && outcome >= 0 && level >= 0 && enemies >= 0, "fixture: the header names its columns");
    if (outcome < 0 || level < 0 || enemies < 0)
        return;
    int fights = 0;
    int won = 0;
    int lost = 0;
    int badLines = 0;
    while (std::getline(in, line))
    {
        std::vector<std::string> const cells = Cells(line, '\t');
        if (cells.size() != head.size() || (cells[outcome] != "alive" && cells[outcome] != "died") ||
            std::atoi(cells[level].c_str()) <= 0)
        {
            ++badLines;
            continue;
        }
        ++fights;
        if (cells[outcome] == "alive")
            ++won;
        else
            ++lost;
    }
    std::printf("AW10 fixture: %d fights, %d won, %d lost\n", fights, won, lost);
    Check(fights > 0, "fixture: tests/data/aw-fights.tsv holds fights");
    Check(badLines == 0, "fixture: every line has the header's columns and an outcome");
}

// AW12 (INTERFACE.md §3.8): the strength of a side, `sides` and `target`

void ReportAw12(char const* name, int cases, int counterexamples, std::uint32_t seed)
{
    std::printf("AW12 invariant %s: %d cases, %d counterexamples (seed %u)\n", name, cases, counterexamples, seed);
    Check(cases >= INVARIANT_CASES && counterexamples == 0, name);
}

// An enemy creature of this level difference and rank at the bot's place and full health, no reach, fighting the
// unit of the guid `fights` (0: idle)
Seen Foe(std::uint64_t guid, int levelDiff = 0, std::uint64_t fights = 0, Rank rank = Rank::Normal)
{
    Seen unit = Unit(guid, Side::Enemy, fights);
    unit.levelDiff = static_cast<std::int16_t>(levelDiff);
    unit.rank = rank;
    return unit;
}

// An ally of this kind at this place, of the bot's level at full health: a player, or a creature for a guard or a
// creature
Seen Friend(std::uint64_t guid, Kind kind, float x = 0.0f, float y = 0.0f, std::uint64_t fights = 0)
{
    Seen unit = Unit(guid, Side::Ally, fights);
    unit.kind = kind;
    unit.type = kind == Kind::Guard || kind == Kind::Creature ? UnitType::Creature : UnitType::Player;
    unit.x = x;
    unit.y = y;
    return unit;
}

// The bot at 0 0 0 with this health, fighting the unit of the guid `botFights`; the units in the given order (no
// Admit), the links set
Picture Linked(std::vector<Seen> const& units, float botHealth = 100.0f, std::uint64_t botFights = 0)
{
    Picture picture;
    picture.self.guid = BOT;
    picture.self.level = 40;
    picture.self.healthPct = botHealth;
    picture.self.fightsGuid = botFights;
    for (Seen const& unit : units)
        if (picture.count < PICTURE_SIZE)
            picture.units[picture.count++] = unit;
    LinkUp(picture);
    return picture;
}

UnitList ListOf(Picture const& picture, std::vector<std::uint64_t> const& guids)
{
    UnitList list;
    for (std::uint64_t guid : guids)
        if (list.size < PICTURE_SIZE)
            list.refs[list.size++] = SlotOf(picture, guid);
    return list;
}

// The engine's call as the game side asks it (Awareness::JoinersOf), its place part at the engine's default radius
constexpr float ASSIST_YARDS = 10.0f;

UnitList CallOf(Picture const& picture, std::int16_t target)
{
    UnitList list;
    if (target < 0 || target >= picture.count)
        return list;
    for (std::size_t i = 0; i < picture.count; ++i)
        if (WouldAssistByPlace(picture.units[i], picture.units[target], ASSIST_YARDS))
            list.refs[list.size++] = static_cast<std::int16_t>(i);
    return list;
}

// `target` for the bot at the unit of this guid, these units named by the call
TargetAnswer TargetAt(Picture const& picture, std::uint64_t target, std::vector<std::uint64_t> const& joins = {})
{
    return JudgeTarget(picture, THE_BOT, SlotOf(picture, target), ListOf(picture, joins));
}

bool Answered(TargetAnswer const& answer, bool take, TargetReason reason, std::uint32_t ours, std::uint32_t weight)
{
    return answer.take == take && answer.reason == reason && answer.ours == ours && answer.weight == weight;
}

// AW7's Takeable as it stood, to show that the new one holds wherever it held
bool OldTakeable(PointAnswer const& answer)
{
    return answer.inView && answer.reaching == 1 && answer.topLevelDiff <= 0 && answer.topRank == Rank::Normal;
}

// The rule's numbers and §3.8's examples (a bot alone at full health is worth 2.5), the joins, the words
void TestStrengthFixed()
{
    Check(Strength(1.0, 0, 100.0f) == 1000 && CreatureStrength(Rank::Elite, 0) == 2000 &&
              CreatureStrength(Rank::Rare, 0) == 2000 && CreatureStrength(Rank::Boss, 0) == 20000,
          "strength: an ordinary creature of the bot's level is 1000, an elite or a rare 2000, a boss 20000");
    Check(CreatureStrength(Rank::Normal, 1) == 1200 && CreatureStrength(Rank::Normal, -1) == 833 &&
              CreatureStrength(Rank::Normal, 5) == 2488 && CreatureStrength(Rank::Normal, 6) == 2986 &&
              CreatureStrength(Rank::Normal, -5) == 402,
          "strength: times 1.2 for every level above the bot, divided for every level below");
    Check(CreatureStrength(Rank::Normal, 25) == CreatureStrength(Rank::Normal, 20) &&
              CreatureStrength(Rank::Normal, -30) == CreatureStrength(Rank::Normal, -20) &&
              CreatureStrength(Rank::Normal, 20) > CreatureStrength(Rank::Normal, 19),
          "strength: the level difference is read between -20 and +20");
    Check(CreatureStrength(Rank::Normal, 0, 25.0f) == 500 && CreatureStrength(Rank::Normal, 0, 0.0f) == 0,
          "strength: the health share enters by its square root");
    Self self;
    bool bot = StrengthOf(self) == 2500;
    self.healthPct = 50.0f;
    bot = bot && StrengthOf(self) == 1768;
    self.healthPct = 30.0f;
    bot = bot && StrengthOf(self) == 1369;
    self.healthPct = 16.0f;
    bot = bot && StrengthOf(self) == 1000;
    self.healthPct = 15.9f;
    bot = bot && StrengthOf(self) < 1000;
    Check(bot, "strength: a bot is worth 2.5 at full health, 1.77 at half, 1.37 at 30 %, under 1 under 16 %");
    Seen player = Foe(2);
    player.type = UnitType::Player;
    Seen pet = Foe(3, 0, 0, Rank::Boss);
    pet.type = UnitType::Pet;
    Check(StrengthOf(player) == 2500 && StrengthOf(pet) == 0 && StrengthOf(WithBusy(Foe(4), Busy::Dead)) == 0,
          "strength: a player is worth what a bot is, a pet nothing, a dead unit nothing");

    // A bot alone at full health
    Check(Answered(TargetAt(Linked({Foe(101), Foe(102, 0, BOT)}), 101), true, TargetReason::None, 2500, 2000),
          "target: two creatures of the bot's level are taken");
    Check(Answered(TargetAt(Linked({Foe(101), Foe(102, 0, BOT), Foe(103, 0, BOT)}), 101), false, TargetReason::Many,
                   2500, 3000),
          "target: three are left, many");
    Check(Answered(TargetAt(Linked({Foe(101, -1), Foe(102, -1, BOT), Foe(103, -1, BOT)}), 101), true,
                   TargetReason::None, 2500, 3 * CreatureStrength(Rank::Normal, -1)),
          "target: three one level below are taken");
    Check(Answered(TargetAt(Linked({Foe(101, 5)}), 101), true, TargetReason::None, 2500, 2488) &&
              Answered(TargetAt(Linked({Foe(101, 6)}), 101), false, TargetReason::Strong, 2500, 2986),
          "target: one creature five levels above is taken, six above is left, strong");
    Check(TargetAt(Linked({Foe(101, 0, 0, Rank::Elite)}, 64.0f), 101).take &&
              TargetAt(Linked({Foe(101, 0, 0, Rank::Elite)}, 65.0f), 101).take &&
              Answered(TargetAt(Linked({Foe(101, 0, 0, Rank::Elite)}, 63.0f), 101), false, TargetReason::Strong,
                       Strength(STRENGTH.player, 0, 63.0f), 2000),
          "target: an elite of the bot's level is taken from 64 % health on");
    Check(Answered(TargetAt(Linked({Foe(101, 2, 0, Rank::Elite)}), 101), false, TargetReason::Strong, 2500, 2880),
          "target: an elite two levels above is left, strong");
    Check(TargetAt(Linked({Foe(101)}, 16.0f), 101).take &&
              Answered(TargetAt(Linked({Foe(101)}, 15.0f), 101), false, TargetReason::Strong,
                       Strength(STRENGTH.player, 0, 15.0f), 1000),
          "target: one creature of the bot's level is taken down to 16 % health, left under it");
    std::vector<Seen> seven{Foe(101)};
    for (std::uint64_t guid = 102; guid < 108; ++guid)
        seven.push_back(Foe(guid, 0, BOT));
    std::vector<Seen> eight = seven;
    eight.push_back(Foe(108, 0, BOT));
    std::vector<Seen> sevenHelped = seven;
    std::vector<Seen> eightHelped = eight;
    for (std::vector<Seen>* units : {&sevenHelped, &eightHelped})
    {
        units->push_back(Friend(201, Kind::Group, 10.0f, 0.0f));
        units->push_back(Friend(202, Kind::Group, 0.0f, 25.0f));
    }
    Check(Answered(TargetAt(Linked(sevenHelped), 101), true, TargetReason::None, 7500, 7000) &&
              Answered(TargetAt(Linked(eightHelped), 101), false, TargetReason::Many, 7500, 8000) &&
              !TargetAt(Linked(seven), 101).take,
          "target: with two members of its group within 30 yd a bot takes up to 7.5, alone it leaves 7");
    std::vector<Seen> const guarded{Foe(101),
                                    Foe(102, 0, BOT),
                                    Foe(103, 0, BOT),
                                    Friend(201, Kind::Guard, 5.0f, 0.0f),
                                    Friend(202, Kind::Player, 0.0f, 5.0f),
                                    Friend(203, Kind::Creature, 5.0f, 5.0f)};
    Check(Answered(TargetAt(Linked(guarded), 101), false, TargetReason::Many, 2500, 3000),
          "target: a guard, a player or a creature that only stands near gives no help");
    Seen far = Friend(201, Kind::Group, 25.0f, 0.0f);
    far.estimateRadius = 6.0f;
    Check(!TargetAt(Linked({Foe(101), Foe(102, 0, BOT), Foe(103, 0, BOT), far}), 101).take,
          "target: a member whose disc reaches past 30 yd does not help");

    // Unknown targets; another unit asking
    Picture const one = Linked({Foe(101), WithBusy(Foe(102), Busy::Dead), Friend(201, Kind::Group)});
    TargetAnswer const missing = TargetAt(one, 999);
    TargetAnswer const dead = TargetAt(one, 102);
    TargetAnswer const itself = JudgeTarget(one, SlotOf(one, 201), SlotOf(one, 201), UnitList{});
    Check(!missing.take && missing.reason == TargetReason::Unknown && !dead.take &&
              dead.reason == TargetReason::Unknown && !itself.take && itself.reason == TargetReason::Unknown,
          "target: a unit not in the picture, a dead one or the asking unit itself is unknown: leave");
    Check(Answered(JudgeTarget(one, SlotOf(one, 201), SlotOf(one, 101), UnitList{}), true, TargetReason::None, 2500,
                   1000),
          "target: asked for a member of the group, the member is the unit that would attack");

    // The joins: named by the call and in reach once; a unit that fights does not join; none for a player or a pet
    Seen caller = Foe(102);
    caller.y = 8.0f;
    caller.reachYards = 20.0f;
    Picture const call = Linked({Foe(101), caller, Foe(103, 0, 201), Friend(201, Kind::Player, 40.0f, 0.0f, 103)});
    TargetAnswer const once = TargetAt(call, 101, {102, 102, 103, 101});
    Check(Answered(once, true, TargetReason::None, 2500, 2000) && once.joins.size == 1 &&
              once.joins.refs[0] == SlotOf(call, 102),
          "target: a joiner named twice and in reach joins once; a unit that fights someone does not join");
    Seen enemyPlayer = Foe(101);
    enemyPlayer.type = UnitType::Player;
    Seen enemyPet = Foe(101);
    enemyPet.type = UnitType::Pet;
    TargetAnswer const atPlayer = TargetAt(Linked({enemyPlayer, caller}), 101, {102});
    TargetAnswer const atPet = TargetAt(Linked({enemyPet, caller}), 101, {102});
    Check(Answered(atPlayer, true, TargetReason::None, 2500, 2500) && !atPlayer.joins.size &&
              Answered(atPet, true, TargetReason::None, 2500, 0) && !atPet.joins.size,
          "target: asked about a player or a pet, nobody joins (nothing learned is read)");

    // Named by the call alone (no reach), a unit joins; a dead one named does not
    Picture const called = Linked({Foe(101), Foe(102), WithBusy(Foe(103), Busy::Dead)});
    TargetAnswer const byCall = TargetAt(called, 101, {102, 103});
    Check(Answered(byCall, true, TargetReason::None, 2500, 2000) && byCall.joins.size == 1 &&
              byCall.joins.refs[0] == SlotOf(called, 102) && !TargetAt(called, 101).joins.size,
          "target: a unit named by the call joins without a reach; a dead one named does not");
    // By reach at the target's place and at the bot's own; through the disc of an estimate; an evading creature from
    // its home, at its home's height
    Seen aimed = Foe(101);
    aimed.x = 60.0f;
    Seen byTarget = Foe(102);
    byTarget.x = 75.0f;
    byTarget.reachYards = 14.0f;
    Seen byBot = Foe(103);
    byBot.x = -10.0f;
    byBot.reachYards = 8.0f;
    Seen vague = Foe(104);
    vague.x = 60.0f;
    vague.y = 30.0f;
    vague.reachYards = 10.0f;
    vague.known = Known::Remembered;
    vague.estimateRadius = 18.0f;
    Seen evading = WithBusy(Foe(105), Busy::Evading);
    evading.x = 62.0f;
    evading.z = 20.0f;
    evading.homeX = 62.0f;
    evading.reachYards = 5.0f;
    TargetAnswer const byReach = TargetAt(Linked({aimed, byTarget, byBot}), 101);
    TargetAnswer const byDisc = TargetAt(Linked({aimed, vague}), 101);
    TargetAnswer const byHome = TargetAt(Linked({aimed, evading}), 101);
    Check(Answered(byReach, false, TargetReason::Many, 2500, 3000) && byReach.joins.size == 2 &&
              byDisc.joins.size == 1 && byDisc.weight == 2000 && byHome.joins.size == 1 && byHome.weight == 2000,
          "target: by reach at the target's place or the bot's, through an estimate's disc, an evading creature from "
          "its home");
    // Step 1 on the picture as it would be: the bot fights the target, an idle target the bot
    UnitList noneJoined;
    Picture const asTaken = AsTaken(Linked({Foe(101)}), THE_BOT, 0, UnitList{}, noneJoined);
    Check(asTaken.self.fightsGuid == 101 && Fights(asTaken.units[0]) && asTaken.units[0].fightsGuid == BOT &&
              !noneJoined.size,
          "target: the bot is given the target, an idle target the bot");

    // sides: a unit in no fight holds with theirs 0; an owner is not in a fight through its pet
    Seen owner = Foe(201);
    owner.type = UnitType::Player;
    Seen ownersPet = Foe(202, 0, BOT);
    ownersPet.type = UnitType::Pet;
    ownersPet.ownerGuid = 201;
    Picture const pets = Linked({owner, ownersPet});
    SidesAnswer const botSides = JudgeSides(pets, THE_BOT);
    SidesAnswer const ownerSides = JudgeSides(pets, SlotOf(pets, 201));
    Check(botSides.holds && botSides.ours == 2500 && botSides.theirs == 0 && botSides.enemies.size == 1 &&
              ownerSides.holds && ownerSides.ours == 2500 && ownerSides.theirs == 0 && !ownerSides.enemies.size,
          "sides: a pet weighs nothing and its owner is not in its fight; a unit in no fight holds");

    // Takeable reads the weight
    auto pointWith = [](std::vector<Seen> units)
    {
        for (Seen& unit : units)
        {
            unit.x = 30.0f;
            unit.reachYards = 10.0f;
        }
        return JudgePoint(Pictured(units), 30.0f, 0.0f);
    };
    PointAnswer const twoBelow = pointWith({Foe(301, -5), Foe(302, -5)});
    Check(Takeable(twoBelow) && twoBelow.weight == 2 * CreatureStrength(Rank::Normal, -5) &&
              Takeable(pointWith({Foe(301, -5, 0, Rank::Elite)})) && Takeable(pointWith({Foe(301)})) &&
              !Takeable(pointWith({Foe(301, 0, 0, Rank::Elite)})) && !Takeable(pointWith({})),
          "point: two creatures or an elite five levels below are takeable, one of the bot's level too, an elite of "
          "its level or nobody not");
    PointAnswer heavier;
    heavier.weight = 1;
    PointAnswer supported;
    supported.support = 1;
    Check(!(heavier == PointAnswer{}) && !(supported == PointAnswer{}), "point: answers differ by weight and support");

    Check(AllWordsDistinct<TargetReason>() && !std::strcmp(Word(TargetReason::None), "none") &&
              !std::strcmp(Word(TargetReason::Strong), "strong") && !std::strcmp(Word(TargetReason::Many), "many") &&
              !std::strcmp(Word(TargetReason::Unknown), "unknown"),
          "target: the reason words are the plan's");
    SidesAnswer outweighed;
    outweighed.holds = false;
    TargetAnswer taken;
    taken.take = true;
    Check(!std::strcmp(Word(SidesAnswer{}), "holds") && !std::strcmp(Word(outweighed), "outweighed") &&
              !std::strcmp(Word(taken), "take") && !std::strcmp(Word(TargetAnswer{}), "leave"),
          "sides and target: the answer words are the plan's");
}

// A named case prints its name and result, so the output names each (PLAN AW12's exit)
void CheckNamed(bool ok, char const* what)
{
    std::printf("AW12 %s: %s\n", what, ok ? "passed" : "FAILED");
    Check(ok, what);
}

// PLAN AW12's four named cases, each answered from a hand-written picture alone
void TestAw12Named()
{
    // Towards allies: a hurt bot with two creatures on it, two members of its group 40 yd away, a camp beside the way
    Seen first = Foe(101, 0, BOT);
    first.x = 1.0f;
    first.reachYards = 10.0f;
    Seen second = Foe(102, 0, BOT);
    second.x = -1.0f;
    second.reachYards = 10.0f;
    std::vector<Seen> const way{first, second, Friend(201, Kind::Group, 40.0f, 0.0f),
                                Friend(202, Kind::Group, 40.0f, 3.0f)};
    std::vector<Seen> camp = way;
    for (std::uint64_t guid = 301; guid < 304; ++guid)
    {
        Seen tent = Foe(guid);
        tent.x = 18.0f + 2.0f * static_cast<float>(guid - 301);
        tent.y = 20.0f;
        tent.reachYards = 8.0f;
        camp.push_back(tent);
    }
    SidesAnswer const follows = JudgeSides(Linked(way, 40.0f, 101), THE_BOT);
    PointAnswer const byMembers = JudgePoint(Linked(way, 40.0f, 101), 38.0f, 0.0f);
    PointAnswer const cameFrom = JudgePoint(Linked(way, 40.0f, 101), -30.0f, 0.0f);
    CheckNamed(follows.theirs == 2000 && byMembers.support == 5000 && byMembers.support > follows.theirs &&
                   byMembers.clear && cameFrom.clear && cameFrom.weight == 0,
               "named, towards allies: the point by the members has a support above what follows the bot, the way back "
               "is clear");
    auto same = [](PointAnswer const& a, PointAnswer const& b)
    {
        return a.clear == b.clear && a.reaching == b.reaching && a.weight == b.weight && a.support == b.support &&
               a.inView == b.inView;
    };
    CheckNamed(same(JudgePoint(Linked(camp, 40.0f, 101), 38.0f, 0.0f), byMembers) &&
                   same(JudgePoint(Linked(camp, 40.0f, 101), -30.0f, 0.0f), cameFrom) &&
                   JudgeSides(Linked(camp, 40.0f, 101), THE_BOT).theirs == follows.theirs,
               "named, towards allies: the camp beside the way changes neither answer");
    std::vector<Seen> guards = way;
    for (std::size_t i : {std::size_t(2), std::size_t(3)})
    {
        guards[i].kind = Kind::Guard;
        guards[i].type = UnitType::Creature;
    }
    CheckNamed(JudgePoint(Linked(guards, 40.0f, 101), 38.0f, 0.0f).support == 0,
               "named, towards allies: a guard standing in the members' place gives no support");

    // An ally outnumbered: one member of a group of three with four creatures on it, the other with one
    std::vector<Seen> group{Friend(201, Kind::Group, 10.0f, 0.0f, 301), Friend(202, Kind::Group, -10.0f, 0.0f, 305)};
    for (std::uint64_t guid = 301; guid < 305; ++guid)
        group.push_back(Foe(guid, 0, 201));
    group.push_back(Foe(305, 0, 202));
    Picture const members = Linked(group);
    SidesAnswer const four = JudgeSides(members, SlotOf(members, 201));
    SidesAnswer const lone = JudgeSides(members, SlotOf(members, 202));
    CheckNamed(!four.holds && four.ours == 2500 && four.theirs == 4000 && lone.holds && lone.ours == 2500 &&
                   lone.theirs == 1000 && !std::strcmp(Word(four), "outweighed") && !std::strcmp(Word(lone), "holds"),
               "named, an ally outnumbered: four creatures on a member outweigh it, one does not");

    // Players on both sides: three players against two, with a creature among them
    Seen enemyOne = Foe(301, 0, 201);
    enemyOne.type = UnitType::Player;
    Seen enemyTwo = Foe(302, 0, 201);
    enemyTwo.type = UnitType::Player;
    Picture const players =
        Linked({Friend(201, Kind::Player, 0.0f, 0.0f, 301), Friend(202, Kind::Player, 0.0f, 0.0f, 301),
                Friend(203, Kind::Player, 0.0f, 0.0f, 302), enemyOne, enemyTwo, Foe(303, 0, 202)});
    SidesAnswer const ally = JudgeSides(players, SlotOf(players, 201));
    SidesAnswer const enemy = JudgeSides(players, SlotOf(players, 301));
    SidesAnswer const allyThree = JudgeSides(players, SlotOf(players, 203));
    SidesAnswer const enemyTwoSides = JudgeSides(players, SlotOf(players, 302));
    CheckNamed(ally.ours == 7500 && ally.theirs == 6000 && enemy.ours == ally.theirs && enemy.theirs == ally.ours &&
                   allyThree.ours == ally.ours && allyThree.theirs == ally.theirs && enemyTwoSides.ours == enemy.ours &&
                   enemyTwoSides.theirs == enemy.theirs && ally.holds && !enemy.holds,
               "named, players on both sides: the same two strengths asked from either side");

    // An enemy known and not seen: remembered within 15 yd of its last place
    Seen remembered = Foe(401);
    remembered.x = 40.0f;
    remembered.reachYards = 10.0f;
    remembered.known = Known::Remembered;
    remembered.estimateRadius = 15.0f;
    Seen lastSeen = remembered;
    lastSeen.known = Known::Seen;
    lastSeen.estimateRadius = 0.0f;
    PointAnswer const known = JudgePoint(Linked({remembered}), 20.0f, 0.0f);
    PointAnswer const atLastPlace = JudgePoint(Linked({lastSeen}), 20.0f, 0.0f);
    CheckNamed(
        !known.clear && known.weight == 1000 && atLastPlace.clear && atLastPlace.weight == 0,
        "named, an enemy known and not seen: its disc of 15 yd reaches a point its last place alone leaves clear");

    // The join in three dimensions: a neighbour 6 yd from the target and 12 yd above, its reach 8
    Seen aim = Foe(1);
    aim.x = 60.0f;
    Seen above = Foe(2);
    above.x = 66.0f;
    above.z = 12.0f;
    above.reachYards = 8.0f;
    Picture const slope = Linked({aim, above});
    Seen level = above;
    level.z = 0.0f;
    Picture const flat = Linked({aim, level});
    TargetAnswer const fromSlope = JudgeTarget(slope, THE_BOT, 0, CallOf(slope, 0));
    TargetAnswer const fromFlat = JudgeTarget(flat, THE_BOT, 0, CallOf(flat, 0));
    CheckNamed(
        !fromSlope.joins.size && fromSlope.weight == 1000 && fromFlat.joins.size == 1 && fromFlat.weight == 2000,
        "named, a slope: a neighbour above the target out of the radius and its reach in three dimensions does not "
        "join");
}

// A generated fight (AW12): the bot at 0 0 0 at 10 to 100 % health; one to three fights, the first the bot's, each
// other one an ally's; 1 to 12 enemies and 0 to 6 more allies, 5 levels below to 8 above, every rank, kind, type and
// busy state, health 10 to 100 %, half of them known within an estimate of up to 20 yd. Links run only between the
// enemies and the bot's side, so no fight holds a ring of an odd number of units and in every fight an enemy is
// against the bot and an ally beside it
struct FightCase
{
    float botHealth = 100.0f;
    std::uint64_t botFights = 0;
    std::vector<Seen> units;
    std::uint64_t target = 0;

    Picture Built() const { return Linked(units, botHealth, botFights); }

    TargetAnswer Target() const
    {
        Picture const picture = Built();
        std::int16_t const slot = SlotOf(picture, target);
        return JudgeTarget(picture, THE_BOT, slot, CallOf(picture, slot));
    }

    SidesAnswer Sides() const { return JudgeSides(Built(), THE_BOT); }
};

Seen RandomFighter(Generator& generator, std::uint64_t guid, Side side)
{
    Seen unit;
    unit.guid = guid;
    unit.side = side;
    unit.levelDiff = static_cast<std::int16_t>(static_cast<int>(generator.Next() % 14) - 5);
    unit.rank = generator.Next() % 4 == 0 ? static_cast<Rank>(1 + generator.Next() % 3) : Rank::Normal;
    unit.healthPct = generator.Range(10.0f, 100.0f);
    unit.estimateRadius = generator.Next() % 2 ? 0.0f : generator.Range(0.0f, 20.0f);
    unit.known = unit.estimateRadius > 0.0f ? Known::Remembered : Known::Seen;
    unit.x = generator.Range(-40.0f, 40.0f);
    unit.y = generator.Range(-40.0f, 40.0f);
    unit.z = generator.Range(-10.0f, 10.0f);
    unit.homeX = unit.x + generator.Range(-20.0f, 20.0f);
    unit.homeY = unit.y + generator.Range(-20.0f, 20.0f);
    unit.homeZ = unit.z + generator.Range(-5.0f, 5.0f);
    // Idle, on a path, evading or dead; a link makes it fight
    unit.busy = static_cast<Busy>(generator.Next() % 4);
    if (unit.busy == Busy::OnPath && generator.Next() % 2)
    {
        unit.moveX = generator.Range(-4.0f, 4.0f);
        unit.moveY = generator.Range(-4.0f, 4.0f);
    }
    if (side == Side::Enemy)
    {
        std::uint32_t const roll = generator.Next() % 16;
        unit.type = roll == 0 ? UnitType::Player : roll == 1 ? UnitType::Pet : UnitType::Creature;
        bool const reaches = unit.type == UnitType::Creature && generator.Next() % 8 != 0;
        unit.reachYards = reaches ? generator.Range(5.0f, 30.0f) : 0.0f;
    }
    else
    {
        unit.kind = static_cast<Kind>(1 + generator.Next() % 4);
        unit.type = unit.kind == Kind::Guard || unit.kind == Kind::Creature ? UnitType::Creature : UnitType::Player;
    }
    return unit;
}

void Engage(Seen& unit, std::uint64_t opponent)
{
    unit.busy = Busy::Fighting;
    unit.fightsGuid = opponent;
}

// A random unit of this side, -1 for none
int AnyOf(FightCase const& c, Side side, Generator& generator)
{
    std::vector<int> found;
    for (std::size_t i = 0; i < c.units.size(); ++i)
        if (c.units[i].side == side)
            found.push_back(static_cast<int>(i));
    return found.empty() ? -1 : found[generator.Next() % found.size()];
}

FightCase RandomFight(Generator& generator, std::uint64_t& guid)
{
    FightCase c;
    c.botHealth = generator.Range(10.0f, 100.0f);
    std::vector<std::uint64_t> defenders{BOT};
    int const fights = 1 + static_cast<int>(generator.Next() % 3);
    for (int f = 1; f < fights; ++f)
    {
        c.units.push_back(RandomFighter(generator, guid++, Side::Ally));
        defenders.push_back(c.units.back().guid);
    }
    int const enemies = 1 + static_cast<int>(generator.Next() % 12);
    int const allies = static_cast<int>(generator.Next() % 7);
    std::vector<std::uint64_t> foes;
    for (int i = 0; i < enemies; ++i)
    {
        Seen unit = RandomFighter(generator, guid++, Side::Enemy);
        if (generator.Next() % 10 < 6)
            Engage(unit, defenders[generator.Next() % defenders.size()]);
        foes.push_back(unit.guid);
        c.units.push_back(unit);
    }
    for (int i = 0; i < allies; ++i)
    {
        Seen unit = RandomFighter(generator, guid++, Side::Ally);
        if (generator.Next() % 10 < 4)
            Engage(unit, foes[generator.Next() % foes.size()]);
        c.units.push_back(unit);
    }
    for (int f = 1; f < fights; ++f)
        Engage(c.units[static_cast<std::size_t>(f - 1)], foes[generator.Next() % foes.size()]);
    if (generator.Next() % 2)
        c.botFights = foes[generator.Next() % foes.size()];
    Seen& target = c.units[static_cast<std::size_t>(fights - 1) + generator.Next() % enemies];
    if (target.busy == Busy::Dead)
        target.busy = Busy::Idle;
    c.target = target.guid;
    return c;
}

// The second case answers bolder than the first: outweighed turned holds, or leave turned take
bool Bolder(FightCase const& before, FightCase const& after)
{
    return (!before.Sides().holds && after.Sides().holds) || (!before.Target().take && after.Target().take);
}

bool SameAnswers(FightCase const& a, FightCase const& b)
{
    SidesAnswer const sa = a.Sides();
    SidesAnswer const sb = b.Sides();
    TargetAnswer const ta = a.Target();
    TargetAnswer const tb = b.Target();
    return sa.holds == sb.holds && sa.ours == sb.ours && sa.theirs == sb.theirs && ta.take == tb.take &&
           ta.reason == tb.reason && ta.ours == tb.ours && ta.weight == tb.weight;
}

void TestAw12Generated()
{
    constexpr std::uint32_t seed = SEED + 12;
    Generator generator;
    generator.state = seed;
    std::uint64_t guid = 1000;

    // One more unit against, or a higher level, a higher rank or more health of an enemy: never bolder
    int bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        FightCase more = c;
        Seen& unit = more.units[static_cast<std::size_t>(AnyOf(c, Side::Enemy, generator))];
        std::uint32_t const roll = generator.Next() % 4;
        if (roll == 0)
        {
            Seen extra = RandomFighter(generator, guid++, Side::Enemy);
            int const ally = AnyOf(c, Side::Ally, generator);
            Engage(extra, ally >= 0 && generator.Next() % 2 ? c.units[static_cast<std::size_t>(ally)].guid : BOT);
            more.units.push_back(extra);
        }
        else if (roll == 1)
            unit.levelDiff = static_cast<std::int16_t>(unit.levelDiff + 1 + static_cast<int>(generator.Next() % 3));
        else if (roll == 2)
            unit.rank = static_cast<Rank>(std::min(3, static_cast<int>(unit.rank) + 1));
        else
            unit.healthPct = generator.Range(unit.healthPct, 100.0f);
        bad += Bolder(c, more);
    }
    ReportAw12("more-against never bolder", INVARIANT_CASES, bad, seed);

    // Less health of the bot or of a unit beside it: never bolder; one more helper or more health of one: never less
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        int const ally = AnyOf(c, Side::Ally, generator);
        FightCase weaker = c;
        if (ally < 0 || generator.Next() % 3 == 0)
            weaker.botHealth = generator.Range(0.0f, c.botHealth);
        else
        {
            Seen& unit = weaker.units[static_cast<std::size_t>(ally)];
            unit.healthPct = generator.Range(0.0f, unit.healthPct);
        }
        FightCase stronger = c;
        std::uint32_t const roll = generator.Next() % 3;
        if (roll == 0 || ally < 0)
        {
            Seen helper = RandomFighter(generator, guid++, Side::Ally);
            if (generator.Next() % 2)
            {
                // A member of the group near the bot, fighting nobody: it helps against the target
                helper.kind = Kind::Group;
                helper.type = UnitType::Player;
                helper.busy = Busy::Idle;
                float const yards = generator.Range(0.0f, STRENGTH.helpYards - helper.estimateRadius - 0.01f);
                float const angle = generator.Range(0.0f, 6.28f);
                helper.x = yards * std::cos(angle);
                helper.y = yards * std::sin(angle);
            }
            else
                Engage(helper, c.units[static_cast<std::size_t>(AnyOf(c, Side::Enemy, generator))].guid);
            stronger.units.push_back(helper);
        }
        else if (roll == 1)
        {
            Seen& unit = stronger.units[static_cast<std::size_t>(ally)];
            unit.healthPct = generator.Range(unit.healthPct, 100.0f);
        }
        else
            stronger.botHealth = generator.Range(c.botHealth, 100.0f);
        bad += Bolder(c, weaker) || Bolder(stronger, c);
    }
    ReportAw12("less-ours never bolder, more-help never less bold", INVARIANT_CASES, bad, seed);

    // An ally that does not help changes no answer: dead, beyond helpYards, of a kind standsBy does not name, or busy
    // with a fight that shares no link with the bot's; a pet changes no strength
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        FightCase other = c;
        Seen ally = RandomFighter(generator, guid++, Side::Ally);
        std::uint32_t const roll = generator.Next() % 5;
        if (roll <= 1)
        {
            ally.kind = Kind::Group;
            ally.type = UnitType::Player;
            float const nearest = roll == 0 ? 0.0f : STRENGTH.helpYards - ally.estimateRadius + 0.01f;
            float const yards = generator.Range(std::max(0.0f, nearest), 60.0f);
            float const angle = generator.Range(0.0f, 6.28f);
            ally.x = yards * std::cos(angle);
            ally.y = yards * std::sin(angle);
            ally.busy = roll == 0 ? Busy::Dead : Busy::Idle;
            if (roll == 0 && generator.Next() % 2)
                ally.fightsGuid = c.units[static_cast<std::size_t>(AnyOf(c, Side::Enemy, generator))].guid;
        }
        else if (roll == 2)
        {
            ally.kind = static_cast<Kind>(2 + generator.Next() % 3);
            ally.type = ally.kind == Kind::Player ? UnitType::Player : UnitType::Creature;
            ally.busy = Busy::Idle;
            ally.x = generator.Range(-10.0f, 10.0f);
            ally.y = generator.Range(-10.0f, 10.0f);
        }
        else if (roll == 3)
        {
            Seen foe = RandomFighter(generator, guid++, Side::Enemy);
            Engage(foe, ally.guid);
            Engage(ally, foe.guid);
            other.units.push_back(foe);
        }
        else
        {
            ally.type = UnitType::Pet;
            ally.side = generator.Next() % 2 ? Side::Enemy : Side::Ally;
            ally.kind = ally.side == Side::Ally ? Kind::Group : Kind::None;
            std::size_t const at = generator.Next() % (c.units.size() + 1);
            Engage(ally, at < c.units.size() ? c.units[at].guid : BOT);
        }
        other.units.push_back(ally);
        bad += !SameAnswers(c, other);
    }
    ReportAw12("an ally that does not help, or a pet, changes no answer", INVARIANT_CASES, bad, seed);

    // sides swaps between a unit and the one it fights (no odd ring); and a unit's strength is the same in every
    // answer of one picture: around every member, ours and theirs add up to its fight's total, and so do target's
    int pairs = 0;
    int swaps = 0;
    int asks = 0;
    int differ = 0;
    while (pairs < INVARIANT_CASES || asks < INVARIANT_CASES)
    {
        Picture picture = RandomPicture(generator);
        for (int u = 0; u < picture.count; ++u)
        {
            Seen& unit = picture.units[u];
            unit.levelDiff = static_cast<std::int16_t>(static_cast<int>(generator.Next() % 14) - 5);
            unit.rank = static_cast<Rank>(generator.Next() % 4);
            unit.healthPct = generator.Range(10.0f, 100.0f);
        }
        picture.self.healthPct = generator.Range(10.0f, 100.0f);
        Graph const graph = GraphOf(picture);
        std::array<std::uint32_t, PICTURE_SIZE + 1> total{};
        for (int u = 0; u < graph.nodes; ++u)
            if (graph.member[u])
                total[graph.component[u]] += StrengthAt(picture, RefOfNode(picture, u));
        for (int x = 0; x < graph.nodes; ++x)
        {
            if (!graph.member[x])
                continue;
            SidesAnswer const around = JudgeSides(picture, RefOfNode(picture, x));
            ++asks;
            differ += around.ours + around.theirs != total[graph.component[x]];
            int const y = graph.to[x];
            if (y < 0 || graph.oddRing[graph.component[x]])
                continue;
            SidesAnswer const opposite = JudgeSides(picture, RefOfNode(picture, y));
            ++pairs;
            swaps += around.ours != opposite.theirs || around.theirs != opposite.ours;
        }
    }
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        Picture const picture = c.Built();
        std::int16_t const slot = SlotOf(picture, c.target);
        UnitList joined;
        Picture const taken = AsTaken(picture, THE_BOT, slot, CallOf(picture, slot), joined);
        Graph const graph = GraphOf(taken);
        std::uint32_t total = 0;
        for (int u = 0; u < graph.nodes; ++u)
            if (graph.member[u] && graph.component[u] == graph.component[taken.count])
                total += StrengthAt(picture, RefOfNode(picture, u));
        TargetAnswer const answer = c.Target();
        ++asks;
        differ += answer.ours + answer.weight != total;
    }
    ReportAw12("sides swaps between a unit and the one it fights", pairs, swaps, seed);
    ReportAw12("a unit's strength is the same in every answer", asks, differ, seed);

    // target of a target that already fights the bot, with no joiner and no member near: the strengths of sides
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase c = RandomFight(generator, guid);
        for (Seen& unit : c.units)
        {
            if (unit.guid == c.target || unit.busy == Busy::Dead || unit.busy == Busy::Fighting)
                continue;
            if (unit.side == Side::Enemy)
                Engage(unit, BOT);
            else if (StandsBy(unit, 0.0f, 0.0f))
                Engage(unit, c.target);
        }
        for (Seen& unit : c.units)
            if (unit.guid == c.target)
                Engage(unit, BOT);
        c.botFights = generator.Next() % 2 ? c.target : 0;
        TargetAnswer const answer = c.Target();
        SidesAnswer const sides = c.Sides();
        bad += answer.joins.size != 0 || answer.ours != sides.ours || answer.weight != sides.theirs;
    }
    ReportAw12("target of a target in the fight is sides", INVARIANT_CASES, bad, seed);

    // take exactly when ours >= weight; strong exactly when the target alone is above ours; none exactly when take;
    // a dead target is unknown, leave
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        Picture const picture = c.Built();
        TargetAnswer const answer = c.Target();
        std::uint32_t const alone = StrengthAt(picture, SlotOf(picture, c.target));
        bool ok = answer.take == (answer.ours >= answer.weight) &&
                  (answer.reason == TargetReason::None) == answer.take &&
                  (answer.reason == TargetReason::Strong) == (!answer.take && alone > answer.ours) &&
                  (answer.reason == TargetReason::Many) == (!answer.take && alone <= answer.ours);
        FightCase gone = c;
        for (Seen& unit : gone.units)
            if (unit.guid == c.target)
                unit.busy = Busy::Dead;
        TargetAnswer const dead = gone.Target();
        ok = ok && !dead.take && dead.reason == TargetReason::Unknown;
        bad += !ok;
    }
    ReportAw12("take, strong and none by the sums", INVARIANT_CASES, bad, seed);

    // A joiner named by the call and in reach counts once; a unit that fights someone does not join
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        Picture const picture = c.Built();
        std::int16_t const slot = SlotOf(picture, c.target);
        UnitList const call = CallOf(picture, slot);
        TargetAnswer const once = JudgeTarget(picture, THE_BOT, slot, call);
        UnitList named = once.joins;
        for (std::uint8_t k = 0; k < call.size && named.size < PICTURE_SIZE; ++k)
            named.refs[named.size++] = call.refs[k];
        for (std::size_t k = 0; k < picture.count && named.size < PICTURE_SIZE; ++k)
            if (Fights(picture.units[k]))
                named.refs[named.size++] = static_cast<std::int16_t>(k);
        TargetAnswer const again = JudgeTarget(picture, THE_BOT, slot, named);
        std::vector<std::uint64_t> const joined = Guids(picture, again.joins);
        bool ok = again.take == once.take && again.reason == once.reason && again.ours == once.ours &&
                  again.weight == once.weight && joined == Guids(picture, once.joins) &&
                  std::adjacent_find(joined.begin(), joined.end()) == joined.end();
        for (std::uint8_t k = 0; k < again.joins.size; ++k)
            ok = ok && !Fights(picture.units[static_cast<std::size_t>(again.joins.refs[k])]);
        bad += !ok;
    }
    ReportAw12("a joiner counts once, a unit that fights does not join", INVARIANT_CASES, bad, seed);

    // An enemy or an ally known less exactly never turns leave into take; sides reads no place
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        FightCase vague = c;
        int which = AnyOf(c, generator.Next() % 2 ? Side::Enemy : Side::Ally, generator);
        if (which < 0)
            which = AnyOf(c, Side::Enemy, generator);
        vague.units[static_cast<std::size_t>(which)].estimateRadius += generator.Range(0.1f, 15.0f);
        SidesAnswer const before = c.Sides();
        SidesAnswer const after = vague.Sides();
        bool const ok = (c.Target().take || !vague.Target().take) && before.holds == after.holds &&
                        before.ours == after.ours && before.theirs == after.theirs;
        bad += !ok;
    }
    ReportAw12("known less exactly never bolder", INVARIANT_CASES, bad, seed);

    // No answer depends on the order of the units
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        FightCase const c = RandomFight(generator, guid);
        FightCase shuffled = c;
        Shuffle(shuffled.units, generator);
        TargetAnswer const a = c.Target();
        TargetAnswer const b = shuffled.Target();
        bad += !SameAnswers(c, shuffled) || Guids(c.Built(), a.joins) != Guids(shuffled.Built(), b.joins);
    }
    ReportAw12("no answer depends on the order of the units", INVARIANT_CASES, bad, seed);

    // A bot at full health alone with one ordinary creature at or below its level takes it; under 16 % health it
    // leaves one of its level
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        Seen creature = RandomFighter(generator, guid++, Side::Enemy);
        creature.type = UnitType::Creature;
        creature.rank = Rank::Normal;
        creature.levelDiff = static_cast<std::int16_t>(-static_cast<int>(generator.Next() % 21));
        creature.x = generator.Range(-60.0f, 60.0f);
        creature.y = generator.Range(-60.0f, 60.0f);
        creature.z = generator.Range(-20.0f, 20.0f);
        if (creature.busy == Busy::Dead)
            creature.busy = Busy::Idle;
        if (generator.Next() % 4 == 0)
            Engage(creature, BOT);
        FightCase c;
        c.units = {creature};
        c.target = creature.guid;
        c.botFights = generator.Next() % 2 ? creature.guid : 0;
        FightCase low = c;
        low.units[0].levelDiff = 0;
        low.units[0].healthPct = 100.0f;
        low.botHealth = generator.Range(0.0f, 15.9f);
        bad += !c.Target().take || low.Target().take;
    }
    ReportAw12("alone with one creature at or below its level: take", INVARIANT_CASES, bad, seed);

    // The point: weight of a clear point is 0, support changes no other field and is the strength of the members of
    // the group within helpYards, where AW7's Takeable held the new one holds, an empty picture is clear everywhere
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        PointCase c = RandomPointCase(generator, guid);
        for (Seen& unit : c.units)
            unit.healthPct = generator.Range(10.0f, 100.0f);
        PointAnswer const plain = c.Judge();
        PointCase helped = c;
        std::uint32_t expected = 0;
        int const allies = static_cast<int>(generator.Next() % 5);
        for (int k = 0; k < allies; ++k)
        {
            Seen ally = RandomFighter(generator, guid++, Side::Ally);
            ally.x = c.x + generator.Range(-40.0f, 40.0f);
            ally.y = c.y + generator.Range(-40.0f, 40.0f);
            bool const stands = ally.kind == Kind::Group && ally.busy != Busy::Dead &&
                                PlanarDistance(ally.x, ally.y, c.x, c.y) + ally.estimateRadius <= 30.0f;
            expected += stands ? StrengthOf(ally) : 0;
            helped.units.push_back(ally);
        }
        PointAnswer got = helped.Judge();
        bool ok = got.support == expected;
        got.support = plain.support;
        ok = ok && got == plain && (!plain.clear || plain.weight == 0) && (!OldTakeable(plain) || Takeable(plain));
        PointAnswer const empty = JudgePoint(Pictured({}), c.x, c.y);
        ok = ok && empty.clear && !empty.reaching && !empty.weight && !empty.support;
        bad += !ok;
    }
    ReportAw12("point: clear weighs 0, support alone, AW7's takeable kept, empty is clear", INVARIANT_CASES, bad, seed);

    // The join in three dimensions: a neighbour above or below the target, inside the radius and its own reach on
    // the map and outside both in three dimensions, does not join; at the target's height it joins once. The place
    // test never names a unit the planar test would not, is it at one height, and a wider estimate keeps a joiner
    bad = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        Seen target = Foe(1);
        target.x = generator.Range(60.0f, 90.0f);
        target.y = generator.Range(-20.0f, 20.0f);
        target.z = generator.Range(-20.0f, 20.0f);
        Seen neighbour = Foe(2);
        neighbour.busy = generator.Next() % 2 ? Busy::Idle : Busy::OnPath;
        neighbour.reachYards = generator.Range(3.0f, 20.0f);
        neighbour.estimateRadius = generator.Next() % 2 ? 0.0f : generator.Range(0.0f, 5.0f);
        float const reach = neighbour.reachYards + POINT_MARGIN_YARDS;
        float const outer = std::max(ASSIST_YARDS, reach);
        float const yards = generator.Range(0.0f, std::min(ASSIST_YARDS, reach) + neighbour.estimateRadius - 0.01f);
        float const angle = generator.Range(0.0f, 6.28f);
        neighbour.x = target.x + yards * std::cos(angle);
        neighbour.y = target.y + yards * std::sin(angle);
        float const gap =
            std::max(0.0f, PlanarDistance(neighbour.x, neighbour.y, target.x, target.y) - neighbour.estimateRadius);
        float const rise = std::sqrt(std::max(0.0f, outer * outer - gap * gap)) + generator.Range(0.05f, 10.0f);
        neighbour.z = target.z + (generator.Next() % 2 ? rise : -rise);
        Seen flat = neighbour;
        flat.z = target.z;
        Picture const sloped = Linked({target, neighbour});
        Picture const level = Linked({target, flat});
        TargetAnswer const fromSlope = JudgeTarget(sloped, THE_BOT, 0, CallOf(sloped, 0));
        TargetAnswer const fromLevel = JudgeTarget(level, THE_BOT, 0, CallOf(level, 0));
        bool ok = !WouldAssistByPlace(neighbour, target, ASSIST_YARDS) && !fromSlope.joins.size &&
                  fromSlope.weight == StrengthOf(target) && WouldAssistByPlace(flat, target, ASSIST_YARDS) &&
                  fromLevel.joins.size == 1 && fromLevel.weight == StrengthOf(target) + StrengthOf(flat);
        // The place test against the planar one
        Seen centre = Hostile(1, generator.Range(-30.0f, 30.0f), generator.Range(-30.0f, 30.0f));
        Seen other =
            Hostile(2, generator.Range(-30.0f, 30.0f), generator.Range(-30.0f, 30.0f), generator.Range(0.0f, 10.0f));
        centre.z = generator.Next() % 2 ? 0.0f : generator.Range(-20.0f, 20.0f);
        float const radius = generator.Range(0.5f, 20.0f);
        Seen wider = other;
        wider.estimateRadius += generator.Range(0.0f, 10.0f);
        bool const joins = WouldAssistByPlace(other, centre, radius);
        ok = ok && (!joins || Within(other, centre, radius)) &&
             (centre.z != other.z || joins == Within(other, centre, radius)) &&
             (!joins || WouldAssistByPlace(wider, centre, radius));
        bad += !ok;
    }
    ReportAw12("join in three dimensions", INVARIANT_CASES, bad, seed);
}

// AW12's replay (INTERFACE.md §3.8, The replay): each fight of AW10's fixture becomes a picture and `target` answers
// it, printed as the won and lost fights by the answer. The bot with the row's health; the enemy rows of the first
// 3.5 s (the first with reason `target` is the target, the others fight the bot); further units like the target, as
// many as 1 + nearby exceeds those rows, named by the call; the scene's group members at the bot's place, of its level
// at full health, fighting nobody. An enemy row is a creature by its rank, a player or a pet, at full health. Asserts
// only that every fight is answered. The lines for other worths of a bot (AW.18) are the same sums with the worth as
// a parameter, for creature-only fights, at the pull and over the whole fight
constexpr float PULL_SECONDS = 3.5f;

struct ReplayEnemy
{
    int diff = 0;
    Rank rank = Rank::Normal;
    UnitType type = UnitType::Creature;
    bool target = false;
    float secs = 0.0f;
};

void TestReplay(char const* file)
{
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line))
    {
        std::printf("AW12 replay: no data (%s)\n", file);
        Check(false, "replay: tests/data/aw-fights.tsv is read");
        return;
    }
    std::vector<std::string> const head = Cells(line, '\t');
    int const outcome = ColumnOf(head, "outcome");
    int const hp = ColumnOf(head, "hp");
    int const sceneGroup = ColumnOf(head, "scene_group");
    int const nearby = ColumnOf(head, "nearby");
    int const players = ColumnOf(head, "players");
    int const enemyList = ColumnOf(head, "enemy_list");
    if (outcome < 0 || hp < 0 || sceneGroup < 0 || nearby < 0 || players < 0 || enemyList < 0)
    {
        Check(false, "replay: the fixture names the columns the replay reads");
        return;
    }
    // [creature-only, all][take, many, strong][won, lost]
    int table[2][3][2] = {};
    int fights[2][2] = {};
    constexpr std::array<double, 4> WORTHS{2.0, 2.5, 3.0, 3.5};
    // [worth][whole fight, at the pull][lost left, won refused]
    int lines[4][2][2] = {};
    int rows = 0;
    int answered = 0;
    int capped = 0;
    while (std::getline(in, line))
    {
        std::vector<std::string> const cells = Cells(line, '\t');
        if (cells.size() != head.size())
            continue;
        ++rows;
        int const lost = cells[static_cast<std::size_t>(outcome)] == "died" ? 1 : 0;
        bool const creatures = std::atoi(cells[static_cast<std::size_t>(players)].c_str()) == 0;
        float const health = std::max(0.0f, static_cast<float>(std::atof(cells[static_cast<std::size_t>(hp)].c_str())));
        int const members = std::atoi(cells[static_cast<std::size_t>(sceneGroup)].c_str());
        int const inReach = std::atoi(cells[static_cast<std::size_t>(nearby)].c_str());
        std::vector<ReplayEnemy> enemies;
        for (std::string const& cell : Cells(cells[static_cast<std::size_t>(enemyList)], ';'))
        {
            std::vector<std::string> const field = Cells(cell, ':');
            if (field.size() != 7)
                continue;
            ReplayEnemy enemy;
            enemy.diff = std::atoi(field[0].c_str());
            enemy.rank = static_cast<Rank>(std::clamp(std::atoi(field[1].c_str()), 0, 3));
            enemy.type = static_cast<UnitType>(std::clamp(std::atoi(field[4].c_str()), 0, 2));
            enemy.target = field[5] == "target";
            enemy.secs = static_cast<float>(std::atof(field[6].c_str()));
            enemies.push_back(enemy);
        }
        if (enemies.empty())
            continue;
        std::vector<ReplayEnemy> early;
        for (ReplayEnemy const& enemy : enemies)
            if (enemy.secs <= PULL_SECONDS)
                early.push_back(enemy);
        std::size_t aim = early.size();
        for (std::size_t k = 0; k < early.size() && aim == early.size(); ++k)
            if (early[k].target)
                aim = k;
        ReplayEnemy const target = aim < early.size() ? early[aim] : enemies[0];
        int const extra = std::max(0, 1 + inReach - static_cast<int>(early.size()));
        auto unitOf = [](ReplayEnemy const& enemy, std::uint64_t guid, std::uint64_t fights)
        {
            Seen unit = Foe(guid, enemy.diff, fights, enemy.rank);
            unit.type = enemy.type;
            return unit;
        };
        std::uint64_t guid = 10;
        std::vector<Seen> units{unitOf(target, guid++, 0)};
        for (std::size_t k = 0; k < early.size(); ++k)
            if (k != aim)
                units.push_back(unitOf(early[k], guid++, BOT));
        std::vector<std::uint64_t> joins;
        for (int k = 0; k < extra; ++k)
        {
            joins.push_back(guid);
            units.push_back(unitOf(target, guid++, 0));
        }
        for (int k = 0; k < members; ++k)
            units.push_back(Friend(guid++, Kind::Group));
        capped += units.size() > PICTURE_SIZE;
        TargetAnswer const answer = TargetAt(Linked(units, health), 10, joins);
        if (answer.reason == TargetReason::Unknown)
            continue;
        ++answered;
        int const row = answer.take ? 0 : answer.reason == TargetReason::Many ? 1 : 2;
        for (int set = creatures ? 0 : 1; set < 2; ++set)
        {
            ++table[set][row][lost];
            ++fights[set][lost];
        }
        if (!creatures)
            continue;
        for (std::size_t w = 0; w < WORTHS.size(); ++w)
        {
            double const worth = WORTHS[w];
            auto weigh = [worth](ReplayEnemy const& enemy)
            {
                double const base = enemy.type == UnitType::Player ? worth
                                    : enemy.type == UnitType::Pet  ? STRENGTH.pet
                                                                  : STRENGTH.rank[static_cast<std::size_t>(enemy.rank)];
                return Strength(base, enemy.diff, 100.0f);
            };
            std::uint32_t const ours =
                Strength(worth, 0, health) + static_cast<std::uint32_t>(members) * Strength(worth, 0, 100.0f);
            std::uint32_t whole = 0;
            for (ReplayEnemy const& enemy : enemies)
                whole += weigh(enemy);
            std::uint32_t pull = static_cast<std::uint32_t>(extra) * weigh(target);
            for (ReplayEnemy const& enemy : early)
                pull += weigh(enemy);
            if (aim == early.size())
                pull += weigh(target);
            for (int form = 0; form < 2; ++form)
                if ((form ? pull : whole) > ours)
                    ++lines[w][form][lost ? 0 : 1];
        }
    }
    char const* const sets[2] = {"creature-only", "all"};
    char const* const answers[3] = {"take", "leave many", "leave strong"};
    std::printf("AW12 replay: %d fights, %d answered, %d pictures over %zu units\n", rows, answered, capped,
                PICTURE_SIZE);
    for (int set = 0; set < 2; ++set)
    {
        int const won = fights[set][0];
        int const lostAll = fights[set][1];
        std::printf("AW12 replay %s: %d fights (%d won, %d lost)\n", sets[set], won + lostAll, won, lostAll);
        for (int row = 0; row < 3; ++row)
            std::printf("AW12 replay %s %-12s won %5d (%4.1f %%), lost %3d (%4.1f %%)\n", sets[set], answers[row],
                        table[set][row][0], 100.0 * table[set][row][0] / std::max(1, won), table[set][row][1],
                        100.0 * table[set][row][1] / std::max(1, lostAll));
    }
    for (std::size_t w = 0; w < WORTHS.size(); ++w)
        std::printf(
            "AW12 replay line, a bot worth %.1f (creature-only, lost fights left against won refused): whole "
            "fight %d against %d, at the pull %d against %d\n",
            WORTHS[w], lines[w][0][0], lines[w][0][1], lines[w][1][0], lines[w][1][1]);
    Check(rows > 0 && answered == rows, "replay: every fight of the fixture is answered");
}

// AW36b, known danger (DangerRules.h; INTERFACE.md §4.2, §4.3 and the invariants G1 to G10, H1 to H3 of §4.6)

// A creature that counts for a place
struct Member
{
    Rank rank = Rank::Normal;
    std::uint8_t level = 1;
    bool guard = false;
};

// The facts built of a set, as the hub's build makes them (tools/awareness/lib/danger.js, factsOf)
DangerRules::CellFacts FactsOf(std::vector<Member> const& set)
{
    DangerRules::CellFacts facts;
    for (Member const& member : set)
    {
        std::size_t const r = static_cast<std::size_t>(member.rank);
        facts.level[r] = std::max(facts.level[r], member.level);
        facts.count[r] = static_cast<std::uint8_t>(std::min(255, facts.count[r] + 1));
        facts.guards = facts.guards || member.guard;
    }
    return facts;
}

bool SameFacts(DangerRules::CellFacts const& a, DangerRules::CellFacts const& b)
{
    return a.level == b.level && a.count == b.count && a.guards == b.guards;
}

// The facts without the rank Rare: what the store hands out of a row's facts (§4.2, No rare)
DangerRules::CellFacts NoRare(DangerRules::CellFacts facts)
{
    std::size_t const rare = static_cast<std::size_t>(Rank::Rare);
    facts.level[rare] = 0;
    facts.count[rare] = 0;
    return facts;
}

int DangerOf(DangerRules::Ground ground) { return static_cast<int>(ground); }

// A creature's highest level in the world data
constexpr int TOP_LEVEL = 83;

std::vector<Member> GenSet(Generator& g, bool guards, std::size_t least = 0)
{
    static constexpr std::array<Rank, 7> RANKS{Rank::Normal, Rank::Normal, Rank::Normal, Rank::Elite,
                                               Rank::Elite,  Rank::Rare,   Rank::Boss};
    std::vector<Member> set(least + g.Next() % 7);
    for (Member& member : set)
    {
        member.rank = RANKS[g.Next() % RANKS.size()];
        member.level = static_cast<std::uint8_t>(1 + g.Next() % TOP_LEVEL);
        member.guard = guards && g.Next() % 20 == 0;
    }
    return set;
}

DangerRules::Condition GenCondition(Generator& g)
{
    DangerRules::Condition condition;
    condition.healthPct = static_cast<std::uint8_t>(1 + g.Next() % 100);
    condition.sick = g.Next() % 5 == 0;
    return condition;
}

// The engine's aggro reach of a creature against a player (Creature::GetAggroRange, without auras and rates)
float EngineReach(float detection, int botLevel, int creatureLevel)
{
    int const diff = std::max(-25, botLevel - creatureLevel);
    return std::clamp(detection - static_cast<float>(diff), 5.0f, 45.0f);
}

// The first failing case of an invariant is printed once, with the seed of its generator
void Count(int& failures, char const* what, std::uint32_t seed, int index)
{
    if (!failures++)
        std::printf("  %s: first failing case %d (seed %u)\n", what, index, seed);
}

// §4.2's rule at the numbers of today's table STRENGTH, the gate's Hillsbrad lines and the thresholds' values
void TestGroundFixed()
{
    using DangerRules::Ground;
    using DangerRules::HEALTHY;
    using DangerRules::JudgeGround;
    using DangerRules::SICK;
    auto const one = [](Rank rank, int level)
    { return FactsOf({Member{rank, static_cast<std::uint8_t>(level), false}}); };
    DangerRules::Condition const half{50, false};
    DangerRules::Condition const quarter{25, false};
    Check(
        JudgeGround(one(Rank::Normal, 26), 20, HEALTHY) == Ground::Deadly &&
            JudgeGround(one(Rank::Normal, 26), 21, HEALTHY) == Ground::Passable,
        "ground: an ordinary creature outweighs a healthy bot from 6 levels above (Hillsbrad's footman, Horde 20/21)");
    Check(JudgeGround(one(Rank::Elite, 22), 20, HEALTHY) == Ground::Deadly &&
              JudgeGround(one(Rank::Elite, 21), 20, HEALTHY) == Ground::Passable &&
              JudgeGround(one(Rank::Rare, 22), 20, HEALTHY) == Ground::Deadly &&
              JudgeGround(one(Rank::Rare, 21), 20, HEALTHY) == Ground::Passable,
          "ground: an elite or a rare outweighs a healthy bot from 2 levels above");
    Check(JudgeGround(one(Rank::Boss, 29), 40, HEALTHY) == Ground::Deadly &&
              JudgeGround(one(Rank::Boss, 28), 40, HEALTHY) == Ground::Passable,
          "ground: a boss outweighs a healthy bot down to 11 levels below it");
    Check(JudgeGround(one(Rank::Normal, 24), 20, half) == Ground::Deadly &&
              JudgeGround(one(Rank::Normal, 23), 20, half) == Ground::Passable &&
              JudgeGround(one(Rank::Elite, 20), 20, half) == Ground::Deadly &&
              JudgeGround(one(Rank::Elite, 19), 20, half) == Ground::Passable &&
              JudgeGround(one(Rank::Boss, 27), 40, half) == Ground::Deadly &&
              JudgeGround(one(Rank::Boss, 26), 40, half) == Ground::Passable,
          "ground: at half health from 4 levels above, an elite from its own level, a boss from 13 below");
    Check(JudgeGround(one(Rank::Normal, 26), 40, SICK) == Ground::Deadly &&
              JudgeGround(one(Rank::Normal, 25), 40, SICK) == Ground::Quiet,
          "ground: a sick bot finds every ground deadly down to 14 levels below it, 15 below is quiet");
    Check(AwarenessRules::Strength(STRENGTH.rank[static_cast<std::size_t>(Rank::Boss)], -15, 100.0f) >
                  AwarenessRules::Strength(STRENGTH.player, 0, 25.0f) &&
              JudgeGround(one(Rank::Boss, 25), 40, quarter) == Ground::Quiet,
          "ground: the quiet line comes first, a boss 15 below outweighs a bot at a quarter and its ground is quiet");
    Check(JudgeGround(FactsOf({Member{Rank::Normal, 1, true}}), 80, HEALTHY) == Ground::Deadly &&
              JudgeGround(DangerRules::CellFacts{}, 1, SICK) == Ground::Quiet,
          "ground: a guard of level 1 is deadly for a bot of 80, nothing is quiet for a sick bot of 1");
    Check(DangerRules::TakenUpTo(0) == 5 && DangerRules::TakenUpTo(1) == 1 && DangerRules::TakenUpTo(2) == 1 &&
              DangerRules::TakenUpTo(3) == -12,
          "ground: a healthy bot takes an ordinary creature up to 5 levels above, an elite or rare 1, a boss -12");
    Check(DangerRules::DeadlyBelow(one(Rank::Normal, 26)) == 21 &&
              DangerRules::PassableBelow(one(Rank::Normal, 26)) == 41 &&
              DangerRules::DeadlyBelow(one(Rank::Boss, 29)) == 41 &&
              DangerRules::PassableBelow(one(Rank::Boss, 29)) == 44,
          "ground: the thresholds of an ordinary creature of 26 (21, 41) and a boss of 29 (41, 44)");
    Check(DangerRules::DeadlyBelow(FactsOf({Member{Rank::Normal, 1, true}})) == 255 &&
              DangerRules::PassableBelow(FactsOf({Member{Rank::Normal, 1, true}})) == 255 &&
              DangerRules::DeadlyBelow(DangerRules::CellFacts{}) == 0 &&
              DangerRules::PassableBelow(DangerRules::CellFacts{}) == 0,
          "ground: the thresholds are 255 with guards and 0 where nothing counts");
    DangerRules::CellFacts many;
    many.level[0] = 10;
    many.count[0] = 200;
    DangerRules::CellFacts more = many;
    more.count[0] = 100;
    more.level[0] = 12;
    DangerRules::CellFacts const merged = DangerRules::Merge(many, more);
    Check(merged.count[0] == 255 && merged.level[0] == 12 && !merged.guards,
          "ground: a merge takes the higher level and adds the counts, capped at 255");
}

// G1 to G8, G10 and H3 over generated sets of creatures, bots and conditions
void TestGroundGenerated()
{
    using DangerRules::Ground;
    using DangerRules::JudgeGround;
    Generator g;
    g.state = SEED + 36;
    std::uint32_t const seed = g.state;
    constexpr int CASES = 3000;
    std::printf("AW36b ground: %d generated sets, seed %u\n", CASES, seed);
    int g1 = 0, g2 = 0, g3 = 0, g4 = 0, g5 = 0, g6 = 0, g7 = 0, g8 = 0, g10 = 0, h3 = 0;
    for (int i = 0; i < CASES; ++i)
    {
        // G1: total over raw facts the build never writes (a count at level 0, levels to 255, health above 100)
        DangerRules::CellFacts raw;
        for (std::size_t r = 0; r < DangerRules::RANK_COUNT; ++r)
        {
            raw.level[r] = static_cast<std::uint8_t>(g.Next() % 256);
            raw.count[r] = static_cast<std::uint8_t>(g.Next() % 3 ? g.Next() % 256 : 0);
        }
        raw.guards = g.Next() % 10 == 0;
        DangerRules::Condition const rawCondition{static_cast<std::uint8_t>(g.Next() % 256), g.Next() % 2 == 0};
        for (int level = 0; level <= 255; ++level)
        {
            Ground const a = JudgeGround(raw, static_cast<std::uint8_t>(level), rawCondition);
            Ground const b = JudgeGround(raw, static_cast<std::uint8_t>(level), rawCondition);
            if (DangerOf(a) > DangerOf(Ground::Deadly) || a != b)
                Count(g1, "G1 total", seed, i);
        }

        std::vector<Member> const set = GenSet(g, i % 2 == 0);
        DangerRules::CellFacts const facts = FactsOf(set);
        DangerRules::Condition const condition = GenCondition(g);
        DangerRules::Condition const notSick{condition.healthPct, false};
        DangerRules::Condition const sick{condition.healthPct, true};
        std::uint8_t const lower = static_cast<std::uint8_t>(1 + g.Next() % condition.healthPct);
        std::vector<Member> const other = GenSet(g, i % 3 == 0);
        DangerRules::CellFacts const otherFacts = FactsOf(other);
        DangerRules::CellFacts const merged = DangerRules::Merge(facts, otherFacts);
        std::vector<Member> both = set;
        both.insert(both.end(), other.begin(), other.end());
        if (!SameFacts(merged, DangerRules::Merge(otherFacts, facts)) || !SameFacts(merged, FactsOf(both)))
            Count(h3, "H3 merge", seed, i);

        // G5: the order of the set does not matter, a member twice changes only its count
        std::vector<Member> shuffled = set;
        for (std::size_t k = shuffled.size(); k > 1; --k)
            std::swap(shuffled[k - 1], shuffled[g.Next() % k]);
        if (!SameFacts(FactsOf(shuffled), facts))
            Count(g5, "G5 order", seed, i);
        if (!set.empty())
        {
            Member const twice = set[g.Next() % set.size()];
            std::vector<Member> doubled = set;
            doubled.push_back(twice);
            DangerRules::CellFacts expected = facts;
            std::size_t const r = static_cast<std::size_t>(twice.rank);
            expected.count[r] = static_cast<std::uint8_t>(std::min(255, expected.count[r] + 1));
            if (!SameFacts(FactsOf(doubled), expected))
                Count(g5, "G5 order", seed, i);
        }

        // G6: one more creature, a higher level or a higher rank of one
        std::vector<DangerRules::CellFacts> worse;
        std::vector<Member> added = set;
        added.push_back(GenSet(g, i % 2 == 0, 1).front());
        worse.push_back(FactsOf(added));
        if (!set.empty())
        {
            std::size_t const k = g.Next() % set.size();
            std::vector<Member> raised = set;
            raised[k].level = static_cast<std::uint8_t>(std::min(255, raised[k].level + 1 + int(g.Next() % 10)));
            worse.push_back(FactsOf(raised));
            std::vector<Member> ranked = set;
            ranked[k].rank = ranked[k].rank == Rank::Normal ? (g.Next() % 2 ? Rank::Elite : Rank::Rare) : Rank::Boss;
            worse.push_back(FactsOf(ranked));
        }

        bool const guarded = facts.guards;
        for (int level = 1; level <= 80; ++level)
        {
            std::uint8_t const bot = static_cast<std::uint8_t>(level);
            Ground const here = JudgeGround(facts, bot, condition);
            Ground const healthy = JudgeGround(facts, bot, DangerRules::HEALTHY);
            // G2
            if (DangerOf(JudgeGround(facts, static_cast<std::uint8_t>(level + 1), condition)) > DangerOf(here))
                Count(g2, "G2 a higher level", seed, i);
            // G3
            if (guarded && (here != Ground::Deadly || healthy != Ground::Deadly ||
                            JudgeGround(facts, bot, DangerRules::SICK) != Ground::Deadly))
                Count(g3, "G3 guards", seed, i);
            // G6
            for (DangerRules::CellFacts const& w : worse)
                if (DangerOf(JudgeGround(w, bot, condition)) < DangerOf(here))
                    Count(g6, "G6 more creatures", seed, i);
            // G7: less health, the sick bot, quiet in every condition
            Ground const sickHere = JudgeGround(facts, bot, sick);
            Ground const notSickHere = JudgeGround(facts, bot, notSick);
            if (DangerOf(JudgeGround(facts, bot, DangerRules::Condition{lower, condition.sick})) < DangerOf(here) ||
                sickHere == Ground::Passable || (sickHere == Ground::Quiet) != (notSickHere == Ground::Quiet) ||
                (healthy == Ground::Quiet) != (notSickHere == Ground::Quiet))
                Count(g7, "G7 less health", seed, i);
            // H3: a merge is never less dangerous than either
            if (DangerOf(JudgeGround(merged, bot, condition)) <
                std::max(DangerOf(here), DangerOf(JudgeGround(otherFacts, bot, condition))))
                Count(h3, "H3 merge", seed, i);
            // G10
            if ((healthy == Ground::Deadly) != (level < DangerRules::DeadlyBelow(facts)) ||
                (healthy == Ground::Quiet) != (level >= DangerRules::PassableBelow(facts)))
                Count(g10, "G10 thresholds", seed, i);
            if (guarded)
                continue;
            // G8: quiet exactly when every creature is 15 or more below, and then each reaches 5 yd
            bool everyBelow = true;
            for (Member const& member : set)
                everyBelow = everyBelow && member.level + DangerRules::QUIET_LEVELS_BELOW <= level;
            if ((healthy == Ground::Quiet) != everyBelow)
                Count(g8, "G8 the quiet line", seed, i);
            if (everyBelow)
                for (Member const& member : set)
                    if (EngineReach(20.0f, level, member.level) != 5.0f)
                        Count(g8, "G8 the quiet line", seed, i);
            // G4: deadly for a bot that is not sick exactly when not quiet and its picture with the strongest member
            // alone against it is outweighed
            bool outweighed = false;
            if (!set.empty())
            {
                Member strongest = set.front();
                for (Member const& member : set)
                    if (CreatureStrength(member.rank, member.level - level) >
                        CreatureStrength(strongest.rank, strongest.level - level))
                        strongest = member;
                Picture const picture = Linked({Foe(101, strongest.level - level, BOT, strongest.rank)},
                                               static_cast<float>(condition.healthPct));
                outweighed = !JudgeSides(picture, THE_BOT).holds;
            }
            if ((notSickHere == Ground::Deadly) != (!everyBelow && outweighed))
                Count(g4, "G4 the picture's line", seed, i);
        }
    }
    Check(!g1, "G1: JudgeGround is total and answers alike for the same inputs");
    Check(!g2, "G2: a higher bot level never makes ground more dangerous");
    Check(!g3, "G3: guards are deadly at every level and in every condition");
    Check(!g4, "G4: deadly for a bot not sick exactly when not quiet and JudgeSides says outweighed");
    Check(!g5, "G5: the facts of a set do not depend on its order, a member twice changes only its count");
    Check(!g6, "G6: one more creature, a higher level or rank never makes ground less dangerous");
    Check(!g7, "G7: less health never less dangerous, sick is quiet or deadly, quiet in every condition");
    Check(!g8, "G8: quiet exactly when every creature is 15 or more below, and then each reaches 5 yd");
    Check(!g10, "G10: deadly exactly below DeadlyBelow, quiet exactly from PassableBelow, levels 1 to 80");
    Check(!h3, "H3: a merge does not depend on its order and is never less dangerous than either");
}

// A generated cell and side of the table: layers in order of height, more than the reach apart (H2 by construction)
struct GenLayer
{
    std::int16_t zLo = 0;
    std::int16_t zHi = 0;
    std::vector<Member> members;
};

struct GenCell
{
    std::uint16_t map = 0;
    std::int16_t cx = 0;
    std::int16_t cy = 0;
    std::uint8_t side = 0;
    std::vector<GenLayer> layers;
};

std::vector<DangerRules::Row> RowsOf(std::vector<GenCell> const& cells)
{
    std::vector<DangerRules::Row> rows;
    for (GenCell const& cell : cells)
        for (std::size_t i = 0; i < cell.layers.size(); ++i)
        {
            DangerRules::Row row;
            row.map = cell.map;
            row.cx = cell.cx;
            row.cy = cell.cy;
            row.side = cell.side;
            row.layer = static_cast<std::uint8_t>(i);
            row.value.zLo = cell.layers[i].zLo;
            row.value.zHi = cell.layers[i].zHi;
            row.value.facts = FactsOf(cell.layers[i].members);
            rows.push_back(row);
        }
    return rows;
}

DangerRules::BuildRow GoodBuild(std::size_t rows)
{
    DangerRules::BuildRow build;
    build.build = "selftest";
    build.cellYards = DangerRules::CELL_YARDS;
    build.heightReachYards = DangerRules::HEIGHT_REACH_YARDS;
    build.rowCount = static_cast<std::uint32_t>(rows);
    build.maxLayers = 4;
    return build;
}

// H1, H2 and the load: generated tables assembled, read at heights in, between and beyond their layers, and the
// tables the load refuses
void TestGroundLayers()
{
    Generator g;
    g.state = SEED + 3602;
    std::uint32_t const seed = g.state;
    constexpr std::array<std::uint16_t, 4> MAPS{0, 1, 530, 571};
    std::vector<GenCell> cells;
    std::vector<std::uint64_t> taken;
    for (int k = 0; k < 400; ++k)
    {
        GenCell cell;
        cell.map = MAPS[g.Next() % MAPS.size()];
        cell.cx = static_cast<std::int16_t>(static_cast<int>(g.Next() % 9) - 4);
        cell.cy = static_cast<std::int16_t>(static_cast<int>(g.Next() % 9) - 4);
        cell.side = static_cast<std::uint8_t>(g.Next() % 2);
        std::uint64_t const key = DangerRules::CellKey(DangerRules::MapIndex(cell.map), cell.cx, cell.cy, cell.side);
        if (std::find(taken.begin(), taken.end(), key) != taken.end())
            continue;
        taken.push_back(key);
        static constexpr std::array<int, 5> SPANS{0, 0, 3, 20, 60};
        static constexpr std::array<int, 7> GAPS{1, 1, 2, 10, 26, 27, 60};
        int z = static_cast<int>(g.Next() % 901) - 300;
        std::size_t const count = 1 + g.Next() % 4;
        for (std::size_t i = 0; i < count; ++i)
        {
            GenLayer layer;
            layer.zLo = static_cast<std::int16_t>(z);
            layer.zHi = static_cast<std::int16_t>(z + SPANS[g.Next() % SPANS.size()]);
            layer.members = GenSet(g, true, 1);
            z = layer.zHi + DangerRules::HEIGHT_REACH_YARDS + GAPS[g.Next() % GAPS.size()];
            cell.layers.push_back(layer);
        }
        cells.push_back(cell);
    }
    std::vector<DangerRules::Row> rows = RowsOf(cells);
    for (std::size_t k = rows.size(); k > 1; --k)
        std::swap(rows[k - 1], rows[g.Next() % k]);
    std::printf("AW36b layers: %zu generated cells, %zu rows, seed %u\n", cells.size(), rows.size(), seed);
    DangerRules::Known known;
    std::string const refusal = DangerRules::Assemble({GoodBuild(rows.size())}, rows, known);
    Check(refusal.empty() && known.keys.size() == cells.size() && known.layers.size() == rows.size(),
          "load: a generated table in any row order is taken, every cell and side once");
    if (!refusal.empty())
        std::printf("  refused: %s\n", refusal.c_str());

    int h1 = 0, h2 = 0, place = 0;
    int heights = 0;
    for (std::size_t c = 0; c < cells.size(); ++c)
    {
        GenCell const& cell = cells[c];
        std::vector<float> zs;
        for (std::size_t i = 0; i < cell.layers.size(); ++i)
        {
            float const lo = cell.layers[i].zLo;
            float const hi = cell.layers[i].zHi;
            float const reach = static_cast<float>(DangerRules::HEIGHT_REACH_YARDS);
            for (float z : {lo, hi, (lo + hi) / 2.0f, lo - reach, lo - reach - 0.5f, hi + reach, hi + reach + 0.5f})
                zs.push_back(z);
            if (i > 0)
                zs.push_back((cell.layers[i - 1].zHi + lo) / 2.0f);
            zs.push_back(g.Range(lo - 100.0f, hi + 100.0f));
        }
        zs.push_back(cell.layers.front().zLo - 200.0f);
        zs.push_back(cell.layers.back().zHi + 200.0f);
        for (float const z : zs)
        {
            ++heights;
            std::vector<Member> reached;
            std::uint8_t reachedLayers = 0;
            for (GenLayer const& layer : cell.layers)
                if (layer.zLo - DangerRules::HEIGHT_REACH_YARDS <= z &&
                    z <= layer.zHi + DangerRules::HEIGHT_REACH_YARDS)
                {
                    reached.insert(reached.end(), layer.members.begin(), layer.members.end());
                    ++reachedLayers;
                }
            float const x = (cell.cx + g.Range(0.0f, 0.999f)) * DangerRules::CELL_YARDS;
            float const y = (cell.cy + g.Range(0.0f, 0.999f)) * DangerRules::CELL_YARDS;
            DangerRules::PlaceReading const read = DangerRules::ReadPlace(&known, cell.map, x, y, z, cell.side);
            if (!read.cell || read.cx != cell.cx || read.cy != cell.cy || read.layerCount != cell.layers.size() ||
                !SameFacts(read.read.facts, NoRare(FactsOf(reached))) || read.read.readCount != reachedLayers)
                Count(h1, "H1 height", seed, static_cast<int>(c));
            if (read.read.readCount > 2)
                Count(h2, "H2 two layers at most", seed, static_cast<int>(c));
        }
        // Another map, the neutral side and a cell without rows know nothing
        float const x = (cell.cx + 0.5f) * DangerRules::CELL_YARDS;
        float const y = (cell.cy + 0.5f) * DangerRules::CELL_YARDS;
        float const z = cell.layers.front().zLo;
        if (DangerRules::ReadPlace(&known, 13, x, y, z, cell.side).layerCount ||
            DangerRules::ReadPlace(&known, cell.map, x, y, z, 2).layerCount ||
            DangerRules::ReadPlace(&known, cell.map, x + 300.0f, y, z, cell.side).layerCount ||
            DangerRules::ReadPlace(nullptr, cell.map, x, y, z, cell.side).layerCount)
            Count(place, "place: no rows", seed, static_cast<int>(c));
    }
    std::printf("AW36b layers: %d heights read\n", heights);
    Check(!h1, "H1: the facts read at a height are those of the layers within reach of it (no rare), and of no other");
    Check(!h2, "H2: a height of a valid table reads two layers at most");
    Check(!place, "ground: another map, the neutral side, a cell without rows and no store read nothing");
    int cx = 0, cy = 0;
    Check(DangerRules::CellOf(-0.01f, 24.99f, cx, cy) && cx == -1 && cy == 0 &&
              DangerRules::CellOf(25.0f, -25.0f, cx, cy) && cx == 1 && cy == -1 &&
              DangerRules::CellOf(-25.01f, 0.0f, cx, cy) && cx == -2 && cy == 0,
          "ground: a cell is floor(x / 25), negative coordinates too");
    Check(!DangerRules::CellOf(std::nanf(""), 0.0f, cx, cy) && !DangerRules::CellOf(1.0e9f, 0.0f, cx, cy),
          "ground: a coordinate that is no number or outside the table has no cell");

    // The load's refusals (H2 and the build row), each with a reason
    std::vector<DangerRules::Row> const good = RowsOf(cells);
    auto const refused =
        [](std::vector<DangerRules::BuildRow> const& builds, std::vector<DangerRules::Row> const& table)
    {
        DangerRules::Known scratch;
        return !DangerRules::Assemble(builds, table, scratch).empty();
    };
    DangerRules::BuildRow otherEdge = GoodBuild(good.size());
    otherEdge.cellYards = 50;
    DangerRules::BuildRow otherReach = GoodBuild(good.size());
    otherReach.heightReachYards = 20;
    DangerRules::BuildRow fewerLayers = GoodBuild(good.size());
    fewerLayers.maxLayers = 1;
    Check(refused({GoodBuild(0)}, {}) && refused({}, good) &&
              refused({GoodBuild(good.size()), GoodBuild(good.size())}, good),
          "load: an empty table, a missing build row and two build rows are refused");
    Check(refused({otherEdge}, good) && refused({otherReach}, good) && refused({GoodBuild(good.size() - 1)}, good) &&
              refused({fewerLayers}, good),
          "load: another cell edge, another reach in height, a row count the build does not name and more layers than "
          "its max_layers are refused");
    GenCell pair;
    pair.layers = {GenLayer{0, 10, {Member{}}}, GenLayer{37, 40, {Member{}}}};
    Check(!refused({GoodBuild(2)}, RowsOf({pair})), "load: two layers 27 yd apart are taken");
    pair.layers[1].zLo = 36;
    Check(refused({GoodBuild(2)}, RowsOf({pair})), "load: two layers 26 yd apart are refused (H2)");
    pair.layers[1].zLo = 45;
    Check(refused({GoodBuild(2)}, RowsOf({pair})), "load: a layer whose z_lo lies above its z_hi is refused");
    pair.layers[1].zLo = 37;
    std::vector<DangerRules::Row> gap = RowsOf({pair});
    gap[1].layer = 2;
    std::vector<DangerRules::Row> twice = RowsOf({pair});
    twice[1].layer = 0;
    std::vector<DangerRules::Row> lowFirst = RowsOf({pair});
    std::swap(lowFirst[0].layer, lowFirst[1].layer);
    Check(refused({GoodBuild(2)}, gap) && refused({GoodBuild(2)}, twice) && refused({GoodBuild(2)}, lowFirst),
          "load: a missing layer, a layer twice and layers out of the order of height are refused");
    std::vector<DangerRules::Row> otherMap = RowsOf({pair});
    otherMap[0].map = 13;
    otherMap[1].map = 13;
    std::vector<DangerRules::Row> neutral = RowsOf({pair});
    neutral[0].side = 2;
    neutral[1].side = 2;
    Check(refused({GoodBuild(2)}, otherMap) && refused({GoodBuild(2)}, neutral),
          "load: a row of another map or of no side is refused");

    // The publish: one atomic pointer, a second publish keeps the first object
    DangerRules::Known const* before = DangerRules::Published();
    auto first = std::make_unique<DangerRules::Known>();
    first->build = "first";
    DangerRules::Known const* one = DangerRules::Publish(std::move(first));
    auto second = std::make_unique<DangerRules::Known>();
    second->build = "second";
    DangerRules::Known const* two = DangerRules::Publish(std::move(second));
    Check(!before && DangerRules::Published() == two && one->build == "first" && two->build == "second",
          "store: nothing before the load, the newest publish is read and an older object stays");
}

// G9, the twin: the hub's JavaScript copy and this rule answer the same pinned table
// (tests/data/aw-ground-cases.tsv, generated by the hub's tools/awareness/danger-check.js --export-twin)
void TestGroundTwin(char const* file)
{
    std::ifstream in(file);
    if (!in)
    {
        std::printf("AW36b twin: no data (%s)\n", file);
        Check(false, "G9: tests/data/aw-ground-cases.tsv is read");
        return;
    }
    auto const factsFrom = [](std::istringstream& cells, DangerRules::CellFacts& facts)
    {
        int value = 0;
        for (std::size_t r = 0; r < DangerRules::RANK_COUNT; ++r)
        {
            if (!(cells >> value))
                return false;
            facts.level[r] = static_cast<std::uint8_t>(value);
        }
        for (std::size_t r = 0; r < DangerRules::RANK_COUNT; ++r)
        {
            if (!(cells >> value))
                return false;
            facts.count[r] = static_cast<std::uint8_t>(value);
        }
        if (!(cells >> value))
            return false;
        facts.guards = value != 0;
        return true;
    };
    std::vector<std::vector<DangerRules::Layer>> layers;
    int grounds = 0, answers = 0, heights = 0, bad = 0, unread = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream cells(line);
        std::string kind;
        cells >> kind;
        DangerRules::CellFacts facts;
        if (kind == "ground")
        {
            int healthPct = 0, sick = 0, deadlyBelow = 0, passableBelow = 0;
            std::string expected;
            if (!factsFrom(cells, facts) || !(cells >> healthPct >> sick >> deadlyBelow >> passableBelow >> expected))
            {
                ++unread;
                continue;
            }
            DangerRules::Condition const condition{static_cast<std::uint8_t>(healthPct), sick != 0};
            std::string got;
            for (int level = 1; level <= 80; ++level)
                got += "QPD"[DangerOf(DangerRules::JudgeGround(facts, static_cast<std::uint8_t>(level), condition))];
            ++grounds;
            answers += 80;
            if (got != expected || DangerRules::DeadlyBelow(facts) != deadlyBelow ||
                DangerRules::PassableBelow(facts) != passableBelow)
                if (!bad++)
                    std::printf("  twin differs: %s\n", line.c_str());
        }
        else if (kind == "layer")
        {
            int cell = 0, layer = 0, zLo = 0, zHi = 0;
            if (!(cells >> cell >> layer >> zLo >> zHi) || !factsFrom(cells, facts) || cell < 0)
            {
                ++unread;
                continue;
            }
            if (layers.size() <= static_cast<std::size_t>(cell))
                layers.resize(cell + 1);
            // A layer line is a row: taken as the load takes it, without its rare (§4.2, No rare)
            layers[cell].push_back(DangerRules::Layer{static_cast<std::int16_t>(zLo), static_cast<std::int16_t>(zHi),
                                                      DangerRules::WithoutRare(facts)});
        }
        else if (kind == "height")
        {
            int cell = 0, read = 0;
            double z = 0.0;
            if (!(cells >> cell >> z >> read) || !factsFrom(cells, facts) || cell < 0 ||
                static_cast<std::size_t>(cell) >= layers.size())
            {
                ++unread;
                continue;
            }
            DangerRules::Reading const got =
                DangerRules::ReadLayers(layers[cell].data(), layers[cell].size(), static_cast<float>(z));
            ++heights;
            if (got.readCount != read || !SameFacts(got.facts, facts))
                if (!bad++)
                    std::printf("  twin differs: %s\n", line.c_str());
        }
        else
            ++unread;
    }
    std::printf("AW36b twin %s: %d ground lines (%d answers), %d heights, %d differ, %d unread\n", file, grounds,
                answers, heights, bad, unread);
    Check(grounds > 0 && heights > 0 && !bad && !unread,
          "G9: the twin table is answered as the hub's copy answers it, every line read");
}

// AW36b: the picture's facts for the live check (aw-danger): which units count for a cell and height, and how
void TestPictureFacts()
{
    using DangerRules::FactsOfPicture;
    using DangerRules::Ground;
    using DangerRules::HEALTHY;
    using DangerRules::JudgeGround;
    // The cell 4 4 (100..125 by 100..125), the place at height 50, a bot of level 20
    auto const unit = [](float x, float y, float z, int levelDiff, Rank rank)
    {
        Seen seen;
        seen.side = Side::Enemy;
        seen.kind = Kind::Creature;
        seen.type = UnitType::Creature;
        seen.x = seen.homeX = x;
        seen.y = seen.homeY = y;
        seen.z = seen.homeZ = z;
        seen.levelDiff = static_cast<std::int16_t>(levelDiff);
        seen.rank = rank;
        seen.reachYards = 20.0f;
        return seen;
    };
    auto const pictureOf = [](std::vector<Seen> const& units)
    {
        Picture picture;
        picture.self.level = 20;
        for (Seen const& seen : units)
            picture.units[picture.count++] = seen;
        return picture;
    };
    Check(DangerRules::SquareDistance(4, 4, 110.0f, 110.0f) == 0.0f &&
              std::fabs(DangerRules::SquareDistance(4, 4, 151.0f, 110.0f) - 26.0f) < 0.01f &&
              std::fabs(DangerRules::SquareDistance(4, 4, 74.0f, 74.0f) - std::hypot(26.0f, 26.0f)) < 0.01f,
          "picture facts: the distance to a cell's square is 0 inside it and to its edge or corner outside");
    DangerRules::PictureFacts const near = FactsOfPicture(
        pictureOf({unit(140.0f, 110.0f, 60.0f, 6, Rank::Normal), unit(110.0f, 90.0f, 40.0f, 2, Rank::Elite),
                   unit(110.0f, 110.0f, 50.0f, 3, Rank::Normal)}),
        4, 4, 50.0f);
    Check(near.facts.level[0] == 26 && near.facts.count[0] == 2 && near.facts.level[1] == 22 &&
              near.facts.count[1] == 1 && near.byHome == 3 && !near.byPlace && !near.facts.guards &&
              JudgeGround(near.facts, 20, HEALTHY) == Ground::Deadly,
          "picture facts: enemies within reach of the square and height count by rank, the highest level and count");
    Seen far = unit(160.0f, 110.0f, 50.0f, 0, Rank::Normal);
    Seen high = unit(110.0f, 110.0f, 80.0f, 0, Rank::Normal);
    Seen ally = unit(110.0f, 110.0f, 50.0f, 0, Rank::Normal);
    ally.side = Side::Ally;
    Seen neither = unit(110.0f, 110.0f, 50.0f, 0, Rank::Normal);
    neither.side = Side::Neither;
    Seen player = unit(110.0f, 110.0f, 50.0f, 0, Rank::Normal);
    player.type = UnitType::Player;
    Seen pet = unit(110.0f, 110.0f, 50.0f, 0, Rank::Normal);
    pet.type = UnitType::Pet;
    Seen calm = unit(110.0f, 110.0f, 50.0f, 0, Rank::Normal);
    calm.reachYards = 0.0f;
    DangerRules::PictureFacts const none =
        FactsOfPicture(pictureOf({far, high, ally, neither, player, pet, calm}), 4, 4, 50.0f);
    Check(!none.byHome && !none.byPlace && JudgeGround(none.facts, 20, HEALTHY) == Ground::Quiet,
          "picture facts: beyond the reach or the height, an ally, a neutral, a player, a pet and one that would not "
          "attack count nothing");
    DangerRules::PictureFacts const rare =
        FactsOfPicture(pictureOf({unit(110.0f, 110.0f, 50.0f, 10, Rank::Rare)}), 4, 4, 50.0f);
    Check(!rare.byHome && !rare.byPlace && !rare.facts.count[static_cast<std::size_t>(Rank::Rare)] &&
              JudgeGround(rare.facts, 20, HEALTHY) == Ground::Quiet,
          "picture facts: a rare counts nothing, as the store hands out none (§4.2, No rare)");
    Seen guard = calm;
    guard.guard = true;
    Seen away = far;
    away.x = 112.0f;
    DangerRules::PictureFacts const other = FactsOfPicture(pictureOf({guard, away}), 4, 4, 50.0f);
    Check(other.facts.guards && other.byHome == 1 && other.byPlace == 1 && other.facts.count[0] == 2,
          "picture facts: a guard counts without a reach; a creature away from its home counts by its place now");
    // A patrol 35 yd off the square whose path crosses it, and paths that pass too far or too high
    Seen patrol = far;
    std::array<bool, AwarenessRules::PICTURE_SIZE> paths{};
    paths[0] = true;
    DangerRules::PictureFacts const walking = FactsOfPicture(pictureOf({patrol}), 4, 4, 50.0f, paths);
    Check(walking.byPath == 1 && walking.facts.count[0] == 1 && !walking.byHome && !walking.byPlace &&
              !FactsOfPicture(pictureOf({patrol}), 4, 4, 50.0f).facts.count[0],
          "picture facts: a patrol whose path reaches the cell counts by its path, only when the path is given");
    Check(DangerRules::PathReaches({140.0f, 160.0f, 50.0f, 140.0f, 60.0f, 50.0f}, 4, 4, 50.0f) &&
              !DangerRules::PathReaches({160.0f, 160.0f, 50.0f, 160.0f, 140.0f, 50.0f}, 4, 4, 50.0f) &&
              !DangerRules::PathReaches({140.0f, 160.0f, 90.0f, 140.0f, 60.0f, 90.0f}, 4, 4, 50.0f) &&
              DangerRules::PathReaches({112.0f, 112.0f, 60.0f}, 4, 4, 50.0f) &&
              !DangerRules::PathReaches({}, 4, 4, 50.0f),
          "picture facts: a path reaches a cell by a segment within the reach and one point within the height");
    Check(DangerRules::Agree(Ground::Quiet, Ground::Quiet) == DangerRules::Agreement::Same &&
              DangerRules::Agree(Ground::Passable, Ground::Deadly) == DangerRules::Agreement::Near &&
              DangerRules::Agree(Ground::Deadly, Ground::Quiet) == DangerRules::Agreement::Apart,
          "picture facts: two answers agree, lie one step apart, or quiet against deadly");
}

// AW37a: the danger of a link, the term and a way's danger (INTERFACE.md §4.4 to §4.6, L1 to L5 and R1 to R9)

// A planted table on map 0, assembled as the load assembles it; no rows, no cells
DangerRules::Known Planted(std::vector<DangerRules::Row> rows)
{
    DangerRules::Known known;
    if (rows.empty())
        return known;
    std::size_t const count = rows.size();
    std::string const refusal = DangerRules::Assemble({GoodBuild(count)}, std::move(rows), known);
    if (!refusal.empty())
        std::printf("  planted table refused: %s\n", refusal.c_str());
    return known;
}

DangerRules::Row PlantedRow(int cx, int cy, std::uint8_t side, std::uint8_t layer, int zLo, int zHi,
                            DangerRules::CellFacts const& facts)
{
    DangerRules::Row row;
    row.cx = static_cast<std::int16_t>(cx);
    row.cy = static_cast<std::int16_t>(cy);
    row.side = side;
    row.layer = layer;
    row.value.zLo = static_cast<std::int16_t>(zLo);
    row.value.zHi = static_cast<std::int16_t>(zHi);
    row.value.facts = facts;
    return row;
}

// Rows over the cells [cxLo, cxHi] x [cyLo, cyHi] and both sides, each planted at `percent`: one layer at the ground,
// a quarter of them a second one beyond its reach
std::vector<DangerRules::Row> GenRows(Generator& g, int cxLo, int cxHi, int cyLo, int cyHi, std::uint32_t percent)
{
    std::vector<DangerRules::Row> rows;
    for (int cx = cxLo; cx <= cxHi; ++cx)
        for (int cy = cyLo; cy <= cyHi; ++cy)
            for (std::uint8_t side = 0; side < DangerRules::SIDE_COUNT; ++side)
            {
                if (g.Next() % 100 >= percent)
                    continue;
                int const zLo = -6 + static_cast<int>(g.Next() % 5);
                int const zHi = zLo + static_cast<int>(g.Next() % 8);
                rows.push_back(PlantedRow(cx, cy, side, 0, zLo, zHi, FactsOf(GenSet(g, g.Next() % 8 == 0, 1))));
                if (g.Next() % 4)
                    continue;
                int const high = zHi + DangerRules::HEIGHT_REACH_YARDS + 1 + static_cast<int>(g.Next() % 30);
                rows.push_back(PlantedRow(cx, cy, side, 1, high, high + static_cast<int>(g.Next() % 6),
                                          FactsOf(GenSet(g, false, 1))));
            }
    return rows;
}

bool SameDanger(DangerRules::LinkDanger const& a, DangerRules::LinkDanger const& b)
{
    return a.deadlyBelow == b.deadlyBelow && a.passableBelow == b.passableBelow && a.fights == b.fights &&
           a.yards == b.yards;
}

bool SameFold(std::array<DangerRules::LinkDanger, DangerRules::SIDE_COUNT> const& a,
              std::array<DangerRules::LinkDanger, DangerRules::SIDE_COUNT> const& b)
{
    return SameDanger(a[0], b[0]) && SameDanger(a[1], b[1]);
}

// §4.4's fold at hand-made links: a stretch, a clipped corner, a short link, crowds, guards, height and maps
void TestLinkFoldFixed()
{
    using DangerRules::Place;
    auto const facts = [](Rank rank, int level, std::size_t count)
    { return FactsOf(std::vector<Member>(count, Member{rank, static_cast<std::uint8_t>(level), false})); };
    auto const fold = [](DangerRules::Known const& known, std::vector<Place> const& line)
    { return DangerRules::FoldLink(&known, line); };
    // An ordinary creature of 30 in the cell (2, 0), x 50 to 75, for the Alliance
    DangerRules::Known const one = Planted({PlantedRow(2, 0, 0, 0, -5, 5, facts(Rank::Normal, 30, 1))});
    auto const across = fold(one, {{0, 0.0f, 10.0f, 0.0f}, {0, 100.0f, 10.0f, 0.0f}});
    Check(across[0].deadlyBelow == 25 && across[0].passableBelow == 45 && across[0].yards == 100 &&
              across[0].fights == 0 && across[1].deadlyBelow == 0 && across[1].passableBelow == 0 &&
              across[1].yards == 100,
          "fold: 25 yd beside a creature of 30 are deadly below 25 and not quiet below 45, for its side alone");
    auto const corner = fold(one, {{0, 70.0f, 10.0f, 0.0f}, {0, 170.0f, 10.0f, 0.0f}});
    Check(corner[0].deadlyBelow == 0 && corner[0].passableBelow == 0 && corner[0].yards == 100,
          "fold: a corner clipped for one step of 5 yd is no stretch");
    auto const twenty = fold(one, {{0, 0.0f, 10.0f, 0.0f}, {0, 70.0f, 10.0f, 0.0f}});
    Check(DangerRules::DEADLY_MIN_YARDS == 25.0 && twenty[0].deadlyBelow == 0 && twenty[0].passableBelow == 0 &&
              twenty[0].yards == 70,
          "fold: 20 yd of a 70 yd link in the creature's cell are no stretch, one cell's edge of 25 yd is (L2)");
    std::vector<Place> const acrossLine{{0, 0.0f, 10.0f, 0.0f}, {0, 100.0f, 10.0f, 0.0f}};
    DangerRules::CellFacts const deciding = DangerRules::DecidingFactsOf(&one, acrossLine, 0, 20);
    Check(deciding.level[0] == 30 && deciding.count[0] == 1 &&
              DangerRules::HighestLevel(DangerRules::DecidingFactsOf(&one, acrossLine, 0, 25)) < 0 &&
              DangerRules::HighestLevel(DangerRules::DecidingFactsOf(&one, acrossLine, 1, 20)) < 0,
          "fold: the facts that make a line deadly are the creature of 30 for a bot of 20, none for 25 or the Horde");
    auto const inside = fold(one, {{0, 55.0f, 10.0f, 0.0f}, {0, 59.0f, 10.0f, 0.0f}});
    Check(inside[0].deadlyBelow == 25 && inside[0].yards == 4, "fold: a link of 4 yd inside the cell is deadly");
    auto const empty = fold(one, {{0, 55.0f, 10.0f, 0.0f}});
    Check(empty[0].deadlyBelow == 0 && empty[0].yards == 0, "fold: a line of one point folds nothing");

    std::vector<DangerRules::Row> crowdRows;
    std::vector<DangerRules::Row> loneRows;
    for (int cx = 0; cx < 8; ++cx)
    {
        crowdRows.push_back(PlantedRow(cx, 0, 1, 0, -5, 5, facts(Rank::Normal, 10, 4)));
        loneRows.push_back(PlantedRow(cx, 0, 1, 0, -5, 5, facts(Rank::Normal, 10, 1)));
    }
    std::vector<Place> const road{{0, 0.0f, 10.0f, 0.0f}, {0, 200.0f, 10.0f, 0.0f}};
    auto const crowded = fold(Planted(crowdRows), road);
    auto const lone = fold(Planted(loneRows), road);
    Check(crowded[1].fights == 4 && lone[1].fights == 1 && crowded[0].fights == 0,
          "fold: four fights in 200 yd of crowded cells, one beside single creatures");

    DangerRules::Known const guarded =
        Planted({PlantedRow(2, 0, 1, 0, -5, 5, FactsOf({Member{Rank::Normal, 1, true}}))});
    auto const town = fold(guarded, {{0, 0.0f, 10.0f, 0.0f}, {0, 100.0f, 10.0f, 0.0f}});
    Check(town[1].deadlyBelow == 255 && town[1].passableBelow == 255, "fold: guards make a link deadly at every level");

    auto const over = fold(one, {{0, 0.0f, 10.0f, 40.0f}, {0, 100.0f, 10.0f, 40.0f}});
    auto const within = fold(one, {{0, 0.0f, 10.0f, 30.0f}, {0, 100.0f, 10.0f, 30.0f}});
    Check(over[0].deadlyBelow == 0 && over[0].passableBelow == 0 && over[0].yards == 100 && within[0].deadlyBelow == 25,
          "fold: a link 35 yd over the layer takes nothing from it, 25 yd over it reads it (L5)");
    auto const climb = fold(one, {{0, 0.0f, 10.0f, 0.0f}, {0, 100.0f, 10.0f, 100.0f}});
    Check(climb[0].deadlyBelow == 0 && climb[0].yards == 141,
          "fold: a climb reads each step at its height, 50 to 75 yd up it passes over the layer");
    auto const split = fold(one, {{0, 0.0f, 10.0f, 0.0f}, {1, 100.0f, 10.0f, 0.0f}});
    Check(split[0].yards == 0 && split[0].deadlyBelow == 0, "fold: a segment between two maps folds nothing");
}

// L1, L2, L3 and L5 over generated tables and lines
void TestLinkFoldGenerated()
{
    using DangerRules::Place;
    Generator g;
    g.state = SEED + 37;
    std::uint32_t const seed = g.state;
    constexpr int CASES = 1500;
    std::printf("AW37a fold: %d generated links, seed %u\n", CASES, seed);
    int l1 = 0, l2 = 0, l3 = 0, l5 = 0, yards = 0, deadly = 0, cut = 0;
    for (int i = 0; i < CASES; ++i)
    {
        std::vector<DangerRules::Row> rows = GenRows(g, -4, 8, -4, 8, 25);
        DangerRules::Known const known = Planted(rows);

        // An axis-aligned line on multiples of 5 yd: every step lies whole in one cell, so the fold's yards in a cell
        // are the line's yards inside its box
        int x = 5 * (static_cast<int>(g.Next() % 40) - 10);
        int y = 5 * (static_cast<int>(g.Next() % 40) - 10);
        float const z = g.Range(-4.0f, 4.0f);
        std::vector<Place> line{{0, static_cast<float>(x), static_cast<float>(y), z}};
        std::uint32_t const segments = 1 + g.Next() % 4;
        for (std::uint32_t s = 0; s < segments; ++s)
        {
            int const length = 5 * static_cast<int>(1 + g.Next() % 12);
            std::uint32_t const direction = g.Next() % 4;
            x += direction == 0 ? length : direction == 1 ? -length : 0;
            y += direction == 2 ? length : direction == 3 ? -length : 0;
            line.push_back({0, static_cast<float>(x), static_cast<float>(y), z});
        }
        auto const folded = DangerRules::FoldLink(&known, line);

        // The line's yards in every cell box it crosses, with the cell's thresholds at the line's height
        struct Stretch
        {
            double yards;
            std::uint8_t deadlyBelow;
            std::uint8_t passableBelow;
        };
        std::array<std::vector<Stretch>, DangerRules::SIDE_COUNT> stretches;
        double total = 0.0;
        for (std::size_t s = 1; s < line.size(); ++s)
        {
            Place const& a = line[s - 1];
            Place const& b = line[s];
            bool const alongX = a.y == b.y;
            double const lo = std::min(alongX ? a.x : a.y, alongX ? b.x : b.y);
            double const hi = std::max(alongX ? a.x : a.y, alongX ? b.x : b.y);
            total += hi - lo;
            double const across = alongX ? a.y : a.x;
            int const fixedCell = static_cast<int>(std::floor(across / DangerRules::CELL_YARDS));
            for (int c = static_cast<int>(std::floor(lo / DangerRules::CELL_YARDS)); c * DangerRules::CELL_YARDS < hi;
                 ++c)
            {
                double const yards = std::min(hi, (c + 1.0) * DangerRules::CELL_YARDS) -
                                     std::max(lo, static_cast<double>(c) * DangerRules::CELL_YARDS);
                if (yards <= 0.0)
                    continue;
                float const mid = (static_cast<float>(c) + 0.5f) * DangerRules::CELL_YARDS;
                float const fixed = (static_cast<float>(fixedCell) + 0.5f) * DangerRules::CELL_YARDS;
                for (std::uint8_t side = 0; side < DangerRules::SIDE_COUNT; ++side)
                {
                    DangerRules::PlaceReading const place =
                        DangerRules::ReadPlace(&known, 0, alongX ? mid : fixed, alongX ? fixed : mid, z, side);
                    stretches[side].push_back({yards, DangerRules::DeadlyBelow(place.read.facts),
                                               DangerRules::PassableBelow(place.read.facts)});
                }
            }
        }
        for (std::uint8_t side = 0; side < DangerRules::SIDE_COUNT; ++side)
        {
            DangerRules::LinkDanger const& link = folded[side];
            if (link.passableBelow < link.deadlyBelow)
                Count(l1, "L1 passable below deadly", seed, i);
            deadly += link.deadlyBelow > 1;
            for (int level = 1; level <= 80; ++level)
            {
                double deadlyYards = 0.0;
                double unquietYards = 0.0;
                for (Stretch const& stretch : stretches[side])
                {
                    deadlyYards += level < stretch.deadlyBelow ? stretch.yards : 0.0;
                    unquietYards += level < stretch.passableBelow ? stretch.yards : 0.0;
                }
                double const need = std::min(DangerRules::DEADLY_MIN_YARDS, total);
                bool const deadlyLink = level < link.deadlyBelow;
                // L2: DEADLY_MIN_YARDS or more in deadly cells is deadly, no yard in them is not; and on these lines
                // the exact rule, min(DEADLY_MIN_YARDS, the link's yards)
                if ((deadlyYards >= DangerRules::DEADLY_MIN_YARDS && !deadlyLink) ||
                    (deadlyYards == 0.0 && deadlyLink) || deadlyLink != (deadlyYards >= need) ||
                    (level < link.passableBelow) != (unquietYards >= need))
                    Count(l2, "L2 deadly yards", seed, i);
                // The deadly yards the console prints are the fold's own: the line's yards in cells deadly at the level
                if (std::abs(DangerRules::DeadlyYardsOf(&known, line, side, static_cast<std::uint8_t>(level)) -
                             deadlyYards) > 0.01)
                    Count(yards, "deadly yards of a line", seed, i);
                cut += deadlyYards > 0.0 && deadlyYards < need;
            }
        }

        // L3: the reverse line, the rows in another order, and a free line with heights and its reverse
        std::vector<Place> const reversed(line.rbegin(), line.rend());
        std::vector<DangerRules::Row> shuffledRows = rows;
        for (std::size_t k = shuffledRows.size(); k > 1; --k)
            std::swap(shuffledRows[k - 1], shuffledRows[g.Next() % k]);
        DangerRules::Known const shuffled = Planted(shuffledRows);
        std::vector<Place> free;
        std::uint32_t const points = 2 + g.Next() % 5;
        for (std::uint32_t p = 0; p < points; ++p)
            free.push_back({0, g.Range(-100.0f, 200.0f), g.Range(-100.0f, 200.0f), g.Range(-10.0f, 40.0f)});
        std::vector<Place> const freeReversed(free.rbegin(), free.rend());
        if (!SameFold(folded, DangerRules::FoldLink(&known, reversed)) ||
            !SameFold(folded, DangerRules::FoldLink(&shuffled, line)) ||
            !SameFold(DangerRules::FoldLink(&known, free), DangerRules::FoldLink(&known, freeReversed)))
            Count(l3, "L3 reverse and order", seed, i);

        // L5: a layer more than HEIGHT_REACH_YARDS above the line (guards, the strongest facts) takes nothing; a line
        // high over every layer takes nothing
        std::vector<DangerRules::Row> raised = rows;
        for (DangerRules::Row const& row : rows)
        {
            auto const same = [&row](DangerRules::Row const& other)
            { return other.cx == row.cx && other.cy == row.cy && other.side == row.side; };
            if (std::count_if(rows.begin(), rows.end(), same) == 1)
                raised.push_back(PlantedRow(row.cx, row.cy, row.side, 1, 61 + static_cast<int>(g.Next() % 20), 90,
                                            FactsOf({Member{Rank::Boss, 83, true}})));
        }
        DangerRules::Known const raisedKnown = Planted(raised);
        std::vector<Place> high = line;
        for (Place& place : high)
            place.z = 200.0f;
        auto const highFold = DangerRules::FoldLink(&known, high);
        if (!SameFold(folded, DangerRules::FoldLink(&raisedKnown, line)) || highFold[0].deadlyBelow ||
            highFold[0].passableBelow || highFold[1].deadlyBelow || highFold[1].passableBelow || highFold[0].fights ||
            highFold[1].fights)
            Count(l5, "L5 height", seed, i);
    }
    Check(!l1, "L1: passableBelow >= deadlyBelow, a link deadly for a level is deadly for every lower one");
    Check(!l2, "L2: DEADLY_MIN_YARDS in deadly cells make a deadly link, no yard in them does not");
    Check(!l3, "L3: the fold of a link and of its reverse are the same, in any order of the cells");
    Check(!l5, "L5: a layer beyond HEIGHT_REACH_YARDS of the line's height adds nothing to its fold");
    Check(!yards, "DeadlyYardsOf: the yards of a line in cells deadly at a level, by the fold's steps");
    Check(deadly > CASES / 10 && cut > 0, "fold cases: deadly links and stretches shorter than a deadly one");
}

// G11: a generated table and the same table with every row's rare level and count set to 0 assemble to stores that
// answer alike, every place at every level in three conditions, and fold a link alike (§4.2, No rare)
void TestNoRareGenerated()
{
    using DangerRules::Place;
    Generator g;
    g.state = SEED + 3711;
    std::uint32_t const seed = g.state;
    constexpr int CASES = 60;
    std::size_t const rare = static_cast<std::size_t>(Rank::Rare);
    std::array<DangerRules::Condition, 3> const conditions{DangerRules::HEALTHY, DangerRules::Condition{50, false},
                                                           DangerRules::SICK};
    std::printf("AW37a no rare: %d generated tables, seed %u\n", CASES, seed);
    int g11 = 0, rareRows = 0, decided = 0;
    for (int i = 0; i < CASES; ++i)
    {
        std::vector<DangerRules::Row> rows = GenRows(g, -2, 4, -2, 4, 40);
        std::vector<DangerRules::Row> without = rows;
        for (DangerRules::Row& row : without)
        {
            rareRows += row.value.facts.count[rare] > 0;
            decided += DangerRules::DeadlyBelow(row.value.facts) != DangerRules::DeadlyBelow(NoRare(row.value.facts));
            row.value.facts = NoRare(row.value.facts);
        }
        DangerRules::Known const with = Planted(rows);
        DangerRules::Known const plain = Planted(without);
        for (int cx = -2; cx <= 4; ++cx)
            for (int cy = -2; cy <= 4; ++cy)
                for (std::uint8_t side = 0; side < DangerRules::SIDE_COUNT; ++side)
                {
                    float const x = (static_cast<float>(cx) + g.Range(0.0f, 0.999f)) * DangerRules::CELL_YARDS;
                    float const y = (static_cast<float>(cy) + g.Range(0.0f, 0.999f)) * DangerRules::CELL_YARDS;
                    for (float const z : {g.Range(-8.0f, 8.0f), g.Range(30.0f, 70.0f)})
                    {
                        DangerRules::CellFacts const a = DangerRules::ReadPlace(&with, 0, x, y, z, side).read.facts;
                        DangerRules::CellFacts const b = DangerRules::ReadPlace(&plain, 0, x, y, z, side).read.facts;
                        bool alike = true;
                        for (int level = 1; alike && level <= 80; ++level)
                            for (DangerRules::Condition const& condition : conditions)
                                alike = alike &&
                                        DangerRules::JudgeGround(a, static_cast<std::uint8_t>(level), condition) ==
                                            DangerRules::JudgeGround(b, static_cast<std::uint8_t>(level), condition);
                        if (!alike)
                            Count(g11, "G11 a place", seed, i);
                    }
                }
        std::vector<Place> line;
        std::uint32_t const points = 2 + g.Next() % 5;
        for (std::uint32_t p = 0; p < points; ++p)
            line.push_back({0, g.Range(-50.0f, 125.0f), g.Range(-50.0f, 125.0f), g.Range(-5.0f, 5.0f)});
        if (!SameFold(DangerRules::FoldLink(&with, line), DangerRules::FoldLink(&plain, line)))
            Count(g11, "G11 a link", seed, i);
    }
    Check(!g11,
          "G11: a table with and without its rows' rares answers alike at every place, level and condition, "
          "and folds a link alike");
    Check(rareRows > 0 && decided > 0, "G11 cases: rows with a rare, and rows whose rare alone moves DeadlyBelow");
}

// R6, R7 and R9 on every combination, and §4.5's table
void TestWayRules()
{
    using DangerRules::Ground;
    using DangerRules::WayVerdict;
    int failures = 0;
    for (int start = 0; start < 3; ++start)
        for (int goal = 0; goal < 3; ++goal)
            for (int links : {0, 1, 3})
                for (bool spared : {false, true})
                {
                    DangerRules::DangerPart part;
                    part.start = static_cast<Ground>(start);
                    part.goal = static_cast<Ground>(goal);
                    part.deadlyLinks = static_cast<std::uint16_t>(links);
                    bool const refuses = DangerRules::Refuses(part);
                    WayVerdict const verdict = DangerRules::JudgeWay(part, spared);
                    bool const ok =
                        (part.goal != Ground::Deadly || refuses) &&
                        (part.start != Ground::Deadly || part.goal == Ground::Deadly || !refuses) &&
                        (part.start == Ground::Deadly || part.goal == Ground::Deadly || refuses == (links > 0)) &&
                        (verdict == WayVerdict::Refused) == (refuses && !spared) &&
                        (verdict == WayVerdict::Clear) == (!links && part.goal != Ground::Deadly);
                    failures += !ok;
                }
    Check(!failures, "R6, R7, R9: the way rules over every start, goal, deadly links and spared");
    DangerRules::DangerPart way;
    way.deadlyLinks = 1;
    DangerRules::DangerPart out = way;
    out.start = Ground::Deadly;
    DangerRules::DangerPart into;
    into.goal = Ground::Deadly;
    Check(DangerRules::JudgeWay(way, false) == WayVerdict::Refused &&
              DangerRules::JudgeWay(way, true) == WayVerdict::LeastDeadly &&
              DangerRules::JudgeWay(out, false) == WayVerdict::LeastDeadly &&
              DangerRules::JudgeWay(into, false) == WayVerdict::Refused &&
              DangerRules::JudgeWay(into, true) == WayVerdict::LeastDeadly &&
              DangerRules::JudgeWay(DangerRules::DangerPart{}, false) == WayVerdict::Clear,
          "way rules: §4.5's table (a deadly link, a way out, a deadly goal, a clear way)");
}

constexpr float RUN_SPEED = 7.0f;  // the snapshot's default run speed, yd/s

// A grid of nodes 40 yd apart on map 0, a neighbour linked both ways by a walk link at `percent`, a third of the links
// (and always the first) bent through a point beside the straight line
bool BuildGrid(TravelGraph& graph, Generator& g, int columns, int rows, float x0, float y0, std::uint32_t percent)
{
    TravelGraphBuilder builder;
    auto const idOf = [columns](int c, int r) { return static_cast<std::uint32_t>(1 + r * columns + c); };
    auto const at = [x0, y0](int c, int r)
    { return DangerRules::Place{0, x0 + 40.0f * static_cast<float>(c), y0 + 40.0f * static_cast<float>(r), 0.0f}; };
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            builder.AddNode(idOf(c, r), "grid", 0, at(c, r).x, at(c, r).y, 0.0f);
    struct Bent
    {
        std::uint32_t a;
        std::uint32_t b;
        std::array<DangerRules::Place, 3> points;
    };
    std::vector<Bent> bents;
    auto const span = [](DangerRules::Place const& a, DangerRules::Place const& b)
    { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
    auto const add = [&](int c, int r, int c2, int r2)
    {
        DangerRules::Place const from = at(c, r);
        DangerRules::Place const to = at(c2, r2);
        float distance = span(from, to);
        if (bents.empty() || g.Next() % 3 == 0)
        {
            float const off = g.Range(-12.0f, 12.0f);
            DangerRules::Place const mid{0, (from.x + to.x) / 2 + (r == r2 ? 0.0f : off),
                                         (from.y + to.y) / 2 + (r == r2 ? off : 0.0f), g.Range(-3.0f, 3.0f)};
            bents.push_back({idOf(c, r), idOf(c2, r2), {from, mid, to}});
            distance = span(from, mid) + span(mid, to);
        }
        for (auto const& [a, b] : {std::pair{idOf(c, r), idOf(c2, r2)}, std::pair{idOf(c2, r2), idOf(c, r)}})
        {
            TravelGraphBuilder::LinkRow row;
            row.fromId = a;
            row.toId = b;
            row.type = 1;
            row.distance = distance;
            builder.AddLink(row);
        }
    };
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
        {
            if (c + 1 < columns && g.Next() % 100 < percent)
                add(c, r, c + 1, r);
            if (r + 1 < rows && g.Next() % 100 < percent)
                add(c, r, c, r + 1);
        }
    std::string error;
    if (!builder.EndLinks(error))
    {
        std::printf("  grid: %s\n", error.c_str());
        return false;
    }
    for (Bent const& bent : bents)
    {
        for (std::uint32_t nr = 0; nr < 3; ++nr)
            builder.AddPoint(bent.a, bent.b, nr, 0, bent.points[nr].x, bent.points[nr].y, bent.points[nr].z);
        for (std::uint32_t nr = 0; nr < 3; ++nr)
            builder.AddPoint(bent.b, bent.a, nr, 0, bent.points[2 - nr].x, bent.points[2 - nr].y,
                             bent.points[2 - nr].z);
    }
    if (!builder.Finish(graph, error))
    {
        std::printf("  grid: %s\n", error.c_str());
        return false;
    }
    graph.Publish(0, 0);
    return true;
}

// The plain Dijkstra over the walk links (R1's reference): a link's seconds at the run speed, with the term of the
// published store or without, deadly links left out for `safeOnly`
struct RefWay
{
    bool ok = false;
    double seconds = 0.0;
    std::vector<std::uint32_t> links;
};

RefWay Reference(TravelGraph const& graph, std::uint32_t from, std::uint32_t to, std::uint8_t side, std::uint8_t level,
                 bool term, bool safeOnly)
{
    std::size_t const nodes = graph.NodeCount();
    double const none = std::numeric_limits<double>::infinity();
    std::vector<double> best(nodes, none);
    std::vector<std::uint32_t> via(nodes, std::numeric_limits<std::uint32_t>::max());
    using Item = std::pair<double, std::uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    DangerRules::Known const* const known = DangerRules::Published();
    best[from] = 0.0;
    open.push({0.0, from});
    while (!open.empty())
    {
        auto const [seconds, node] = open.top();
        open.pop();
        if (seconds > best[node])
            continue;
        for (TravelGraphLink const& link : graph.LinksOf(node))
        {
            std::uint32_t const index = graph.IndexOfLink(link);
            DangerRules::LinkDanger const* const danger = known ? known->LinkOf(index, side) : nullptr;
            if (safeOnly && danger && DangerRules::TermOf(*danger, level).deadly)
                continue;
            double const cost = link.distance / RUN_SPEED +
                                (term ? DangerRules::LinkTerm(graph.LinkCount(), index, side, level) : 0.0f);
            if (best[node] + cost < best[link.to])
            {
                best[link.to] = best[node] + cost;
                via[link.to] = index;
                open.push({best[link.to], link.to});
            }
        }
    }
    RefWay way;
    if (best[to] == none)
        return way;
    way.ok = true;
    way.seconds = best[to];
    for (std::uint32_t node = to; node != from;)
    {
        way.links.push_back(via[node]);
        node = graph.GetLink(via[node]).from;
    }
    std::reverse(way.links.begin(), way.links.end());
    return way;
}

TravelRoute::Result SearchWay(TravelGraph const& graph, std::uint32_t from, std::uint32_t to, std::uint8_t side,
                              std::uint8_t level)
{
    TravelGraphNode const& a = graph.GetNode(from);
    TravelGraphNode const& b = graph.GetNode(to);
    TravelRoute::Request request;
    request.snapshot.position = {a.mapId, a.x, a.y, a.z};
    request.snapshot.team = side;
    request.snapshot.level = level;
    request.goal = {b.mapId, b.x, b.y, b.z};
    return TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
}

double BaseSeconds(TravelGraph const& graph, std::vector<std::uint32_t> const& links)
{
    double seconds = 0.0;
    for (std::uint32_t const index : links)
        seconds += graph.GetLink(index).distance / RUN_SPEED;
    return seconds;
}

bool Near(double a, double b) { return std::fabs(a - b) <= 0.01 + 1e-5 * std::max(std::fabs(a), std::fabs(b)); }

void PublishForTest(DangerRules::Known const* known)
{
    DangerRules::Store::published.store(known, std::memory_order_release);
}

// L4, the fold of a graph, and R1 to R9 over generated graphs with planted cells, the plain Dijkstra as the reference
void TestWayTerm()
{
    using DangerRules::Ground;
    DangerRules::Known const* const before = DangerRules::Published();
    Generator g;
    g.state = SEED + 38;
    std::uint32_t const seed = g.state;

    // L4 and the probe of the seam: two nodes 40 yd apart through guards' cells
    {
        TravelGraph graph;
        bool const built = BuildGrid(graph, g, 2, 1, 1000.0f, 1000.0f, 100);
        std::vector<DangerRules::Row> rows;
        for (int cx = 38; cx <= 42; ++cx)
            for (std::uint8_t side = 0; side < DangerRules::SIDE_COUNT; ++side)
                rows.push_back(PlantedRow(cx, 40, side, 0, -10, 10, FactsOf({Member{Rank::Normal, 1, true}})));
        DangerRules::Known const facts = Planted(rows);
        DangerRules::Known folded = facts;
        bool const done = built && DangerRules::FoldGraph(facts, graph, folded.links);
        folded.graphLinks = graph.LinkCount();
        DangerRules::Known off = facts;
        off.graphLinks = graph.LinkCount();
        std::uint32_t const links = static_cast<std::uint32_t>(graph.LinkCount());
        PublishForTest(nullptr);
        bool const noStore = done && DangerRules::LinkTerm(links, 0, 0, 1) == 0.0f;
        TravelRoute::Result const plain = done ? SearchWay(graph, 0, 1, 0, 20) : TravelRoute::Result{};
        PublishForTest(&off);
        bool const termOff = DangerRules::LinkTerm(links, 0, 0, 1) == 0.0f;
        PublishForTest(&folded);
        float const term = DangerRules::LinkTerm(links, 0, 0, 20);
        bool const otherGraph = DangerRules::LinkTerm(links + 1, 0, 0, 20) == 0.0f &&
                                DangerRules::LinkTerm(links, links, 0, 20) == 0.0f &&
                                DangerRules::LinkTerm(links, 0, 2, 20) == 0.0f;
        TravelRoute::Result const priced = done ? SearchWay(graph, 0, 1, 0, 20) : TravelRoute::Result{};
        Check(done && links == 2 && folded.links.size() == links * DangerRules::SIDE_COUNT &&
                  term == DangerRules::DEADLY_LINK_SECONDS +
                              DangerRules::DEADLY_SECONDS_PER_YARD * static_cast<float>(folded.links[0].yards),
              "L4: the guards' link costs an hour and 10 s per yard for a bot of 20");
        Check(noStore && termOff && otherGraph,
              "L4: no store, a store without links (aw term off), another graph's count, index or side: term 0");
        bool const searchPrices = plain.ok && priced.ok && priced.totalSeconds > plain.totalSeconds + 1000.0f;
        Check(searchPrices, "seam: the search prices the term (RouteRules.h LinkSeconds)");

        // L6: under Unpriced every term this thread asks is 0, another thread asking meanwhile gets the term, a nested
        // scope restores the outer one, and the term is back when the scope ends
        bool l6 = done && term > 0.0f;
        {
            DangerRules::Unpriced const outer;
            for (std::uint32_t index = 0; index < links; ++index)
                for (std::uint8_t side = 0; side < DangerRules::SIDE_COUNT; ++side)
                    l6 = l6 && DangerRules::LinkTerm(links, index, side, 20) == 0.0f;
            float other = -1.0f;
            std::thread asker([&other, links] { other = DangerRules::LinkTerm(links, 0, 0, 20); });
            asker.join();
            l6 = l6 && other == term;
            {
                DangerRules::Unpriced const inner;
                l6 = l6 && DangerRules::LinkTerm(links, 0, 0, 20) == 0.0f;
            }
            l6 = l6 && DangerRules::LinkTerm(links, 0, 0, 20) == 0.0f;
            TravelRoute::Result const unpriced = done ? SearchWay(graph, 0, 1, 0, 20) : TravelRoute::Result{};
            l6 = l6 && unpriced.ok && plain.ok && Near(unpriced.totalSeconds, plain.totalSeconds);
        }
        l6 = l6 && DangerRules::LinkTerm(links, 0, 0, 20) == term;
        // A thread under its own scope while this one asks
        std::atomic<int> stage{0};
        float inside = -1.0f;
        std::thread holder(
            [&stage, &inside, links]
            {
                DangerRules::Unpriced const scope;
                inside = DangerRules::LinkTerm(links, 0, 0, 20);
                stage.store(1);
                while (stage.load() != 2)
                    std::this_thread::yield();
            });
        while (stage.load() != 1)
            std::this_thread::yield();
        float const meanwhile = DangerRules::LinkTerm(links, 0, 0, 20);
        stage.store(2);
        holder.join();
        l6 = l6 && inside == 0.0f && meanwhile == term;
        Check(l6,
              "L6: Unpriced prices nothing on its own thread, another thread's term is unchanged, scopes nest and "
              "the term is back after the scope");
    }

    constexpr int CASES = 300;
    constexpr int PAIRS = 4;
    std::printf("AW37a ways: %d generated graphs, %d ways each, seed %u%s\n", CASES, PAIRS, seed, "");
    int graphs = 0, fold = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, walks = 0;
    int deadlyAnswers = 0, refused = 0, outOf = 0, clear = 0, deadlyGoals = 0, risen = 0, ways = 0;
    for (int i = 0; i < CASES; ++i)
    {
        TravelGraph graph;
        float const x0 = 25.0f * static_cast<float>(g.Next() % 20) + g.Range(0.0f, 25.0f);
        float const y0 = 25.0f * static_cast<float>(g.Next() % 20) + g.Range(0.0f, 25.0f);
        if (!BuildGrid(graph, g, 5, 4, x0, y0, 80))
            continue;
        ++graphs;
        int const cxLo = static_cast<int>(std::floor(x0 / DangerRules::CELL_YARDS)) - 1;
        int const cyLo = static_cast<int>(std::floor(y0 / DangerRules::CELL_YARDS)) - 1;
        DangerRules::Known const facts = Planted(GenRows(g, cxLo, cxLo + 8, cyLo, cyLo + 6, 20));
        DangerRules::Known folded = facts;
        DangerRules::FoldGraph(facts, graph, folded.links);
        folded.graphLinks = graph.LinkCount();
        DangerRules::Known off = facts;
        off.graphLinks = graph.LinkCount();

        // The graph's fold is FoldLink over each link's line, the same both ways; no term is negative (R1)
        std::vector<DangerRules::Place> line;
        for (std::uint32_t index = 0; index < graph.LinkCount(); ++index)
        {
            TravelGraphLink const& link = graph.GetLink(index);
            DangerRules::LineOf(graph, link, line);
            auto const one = DangerRules::FoldLink(&facts, line);
            std::uint32_t backIndex = index;
            for (TravelGraphLink const& other : graph.LinksOf(link.to))
                if (other.to == link.from)
                    backIndex = graph.IndexOfLink(other);
            for (std::uint8_t side = 0; side < DangerRules::SIDE_COUNT; ++side)
            {
                if (!SameDanger(one[side], *folded.LinkOf(index, side)) ||
                    !SameDanger(*folded.LinkOf(index, side), *folded.LinkOf(backIndex, side)))
                    Count(fold, "the graph's fold", seed, i);
                for (int level = 1; level <= 80; level += 7)
                    if (DangerRules::TermOf(*folded.LinkOf(index, side), static_cast<std::uint8_t>(level)).seconds <
                        0.0f)
                        Count(r1, "R1 a negative term", seed, i);
            }
        }

        std::uint32_t const nodes = static_cast<std::uint32_t>(graph.NodeCount());
        for (int pair = 0; pair < PAIRS; ++pair)
        {
            std::uint32_t const from = g.Next() % nodes;
            std::uint32_t const to = (from + 1 + g.Next() % (nodes - 1)) % nodes;
            std::uint8_t const side = static_cast<std::uint8_t>(g.Next() % 2);
            std::uint8_t const level = static_cast<std::uint8_t>(1 + g.Next() % 80);
            TravelGraphNode const& a = graph.GetNode(from);
            TravelGraphNode const& b = graph.GetNode(to);
            DangerRules::Place const start{a.mapId, a.x, a.y, a.z};
            DangerRules::Place const goal{b.mapId, b.x, b.y, b.z};

            PublishForTest(&folded);
            RefWay const priced = Reference(graph, from, to, side, level, true, false);
            RefWay const base = Reference(graph, from, to, side, level, false, false);
            RefWay const safe = Reference(graph, from, to, side, level, true, true);
            TravelRoute::Result const with = SearchWay(graph, from, to, side, level);
            PublishForTest(nullptr);
            TravelRoute::Result const none = SearchWay(graph, from, to, side, level);
            PublishForTest(&off);
            TravelRoute::Result const termOff = SearchWay(graph, from, to, side, level);
            PublishForTest(&folded);
            if (!base.ok)
                continue;
            ++ways;

            // R1: no way is cheaper with the term; the search answers as the plain Dijkstra over the same costs
            if (!priced.ok || priced.seconds + 1e-6 < base.seconds)
                Count(r1, "R1 cheaper with the term", seed, i);
            if (!with.ok || !Near(with.totalSeconds, priced.seconds))
                Count(r1, "R1 the search against the reference", seed, i);
            // R2: with no store and with the term off, today's answer; the term closes no way
            if (!none.ok || !Near(none.totalSeconds, base.seconds) || !termOff.ok ||
                !Near(termOff.totalSeconds, base.seconds) || !with.ok)
                Count(r2, "R2 no store, term off", seed, i);
            // The route's walk links (WalkLinksOf) are a chain from start to goal whose seconds are the answer's
            if (none.ok)
            {
                std::vector<std::uint32_t> const links = DangerRules::WalkLinksOf(graph, none.legs);
                bool chain =
                    !links.empty() && graph.GetLink(links.front()).from == from && graph.GetLink(links.back()).to == to;
                for (std::size_t k = 1; chain && k < links.size(); ++k)
                    chain = graph.GetLink(links[k - 1]).to == graph.GetLink(links[k]).from;
                if (!chain || !Near(BaseSeconds(graph, links), none.totalSeconds))
                    Count(walks, "the route's walk links", seed, i);
            }

            DangerRules::DangerPart const part = DangerRules::WayDanger(folded, priced.links, start, goal, side, level);
            // R3: a deadly link only when no way without one costs less; on these graphs (every way far shorter than
            // DEADLY_LINK_SECONDS) only when every way has one
            if (part.deadlyLinks && safe.ok)
                Count(r3, "R3 a deadly link with a safe way", seed, i);
            // R4: the way's seconds are its links' own, its fights' and its penalty
            if (!Near(priced.seconds, BaseSeconds(graph, priced.links) + DangerRules::FIGHT_SECONDS * part.fights +
                                          part.penaltySeconds))
                Count(r4, "R4 the reference's seconds", seed, i);
            if (with.ok)
            {
                std::vector<std::uint32_t> const links = DangerRules::WalkLinksOf(graph, with.legs);
                DangerRules::DangerPart const searched =
                    DangerRules::WayDanger(folded, links, start, goal, side, level);
                if (!Near(with.totalSeconds, BaseSeconds(graph, links) + DangerRules::FIGHT_SECONDS * searched.fights +
                                                 searched.penaltySeconds))
                    Count(r4, "R4 the search's seconds", seed, i);
            }
            // R5 to R7 and R9 on the answer
            bool const refuses = DangerRules::Refuses(part);
            if (part.start != Ground::Deadly && part.goal != Ground::Deadly && refuses != (part.deadlyLinks > 0))
                Count(r5, "R5 refused exactly with a deadly link", seed, i);
            if (part.goal == Ground::Deadly && !refuses)
                Count(r6, "R6 a deadly goal", seed, i);
            if (part.start == Ground::Deadly && part.goal != Ground::Deadly && refuses)
                Count(r7, "R7 a way out", seed, i);
            for (bool const spared : {false, true})
            {
                DangerRules::WayVerdict const verdict = DangerRules::JudgeWay(part, spared);
                if ((verdict == DangerRules::WayVerdict::Refused) != (refuses && !spared) ||
                    (verdict == DangerRules::WayVerdict::Clear) != (!part.deadlyLinks && part.goal != Ground::Deadly))
                    Count(r9, "R9 the verdict", seed, i);
            }
            deadlyAnswers += part.deadlyLinks > 0;
            refused += refuses;
            outOf += part.start == Ground::Deadly && part.deadlyLinks && !refuses;
            clear += !part.deadlyLinks && part.goal != Ground::Deadly;
            deadlyGoals += part.goal == Ground::Deadly;

            // R8 over every level: once a level's way from ground not deadly for it is not refused, no higher level's
            // is (a way out of deadly ground is no such way: at a level for which the start is no longer deadly the
            // same links can be refused)
            bool kept = false;
            bool wasRefused = false;
            for (int up = 1; up <= 80; ++up)
            {
                std::uint8_t const at = static_cast<std::uint8_t>(up);
                RefWay const way = Reference(graph, from, to, side, at, true, false);
                DangerRules::DangerPart const upPart = DangerRules::WayDanger(folded, way.links, start, goal, side, at);
                bool const upRefused = DangerRules::Refuses(upPart);
                if (kept && upRefused)
                    Count(r8, "R8 refused at a higher level", seed, i);
                if (!upRefused && upPart.start != Ground::Deadly)
                {
                    risen += wasRefused && !kept;
                    kept = true;
                }
                wasRefused = wasRefused || upRefused;
            }
        }
    }
    PublishForTest(before);
    std::printf(
        "  %d graphs, %d ways: %d with a deadly link, %d refused, %d out of deadly ground, %d clear, %d to a "
        "deadly goal, %d refused below a level and kept above it\n",
        graphs, ways, deadlyAnswers, refused, outOf, clear, deadlyGoals, risen);
    Check(graphs == CASES && !fold, "the graph's fold: FoldLink over each walk link's line, the same both ways");
    Check(!walks, "WalkLinksOf: a route's walk links are a chain from start to goal with the route's seconds");
    Check(!r1, "R1: a term is never negative, no way is cheaper with it (and the search answers as the reference)");
    Check(!r2, "R2: with no store and with the term off the search answers as today's; the term closes no way");
    Check(!r3, "R3: on ways shorter than DEADLY_LINK_SECONDS a deadly link only when every way has one");
    Check(!r4, "R4: a way's seconds are its links' own plus fights x FIGHT_SECONDS plus penaltySeconds");
    Check(!r5, "R5: from and to ground not deadly, refused exactly when the answer has a deadly link");
    Check(!r6, "R6: a way to a goal in deadly ground is refused whatever its links");
    Check(!r7, "R7: a way out of deadly ground is not refused unless its goal is deadly");
    Check(!r8, "R8: a way kept at a level is kept at every higher one (from ground not deadly for it)");
    Check(!r9, "R9: Refused exactly when Refuses and not spared, Clear exactly with no deadly link nor goal");
    Check(deadlyAnswers > 0 && refused > 0 && outOf > 0 && clear > 0 && deadlyGoals > 0 && risen > 0,
          "way cases: deadly answers, refusals, ways out, clear ways, deadly goals and a refusal a level outgrows");
}

// R11, the user's case (R2 and R3 in one; batch K2): from S to G on map 0 a long walk through quiet ground, and a
// shortcut behind two portals open to every level, a short walk on map 1 through cells deadly for the level. With the
// store the answer is the long walk with no portal leg; with no store, and under Unpriced, the shortcut
void TestShortcutGenerated()
{
    DangerRules::Known const* const before = DangerRules::Published();
    Generator g;
    g.state = SEED + 3712;
    std::uint32_t const seed = g.state;
    constexpr int CASES = 60;
    std::printf("AW37a shortcut: %d generated graphs, seed %u\n", CASES, seed);
    int built = 0, deadly = 0, r11 = 0;
    auto const portalLegs = [](TravelRoute::Result const& result)
    {
        return std::count_if(
            result.legs.begin(), result.legs.end(), [](TravelRoute::Leg const& leg)
            { return leg.type == TravelRoute::LegType::Portal || leg.type == TravelRoute::LegType::StaticPortal; });
    };
    for (int i = 0; i < CASES; ++i)
    {
        float const x0 = g.Range(-2000.0f, 2000.0f);
        float const y0 = g.Range(-2000.0f, 2000.0f);
        float const across = g.Range(150.0f, 450.0f);  // S to G, more than DIRECT_YARDS
        float const aside = g.Range(200.0f, 800.0f);   // the long walk's detour
        float const x1 = g.Range(-2000.0f, 2000.0f);
        float const y1 = g.Range(-2000.0f, 2000.0f);
        float const shortcut = g.Range(30.0f, 200.0f);  // the walk on map 1
        std::uint8_t const side = static_cast<std::uint8_t>(g.Next() % 2);
        std::uint8_t const level = static_cast<std::uint8_t>(1 + g.Next() % 70);
        bool const guards = g.Next() % 4 == 0;
        // Nodes 1 S, 2 and 3 the detour's corners, 4 G on map 0; 5 and 6 the shortcut's ends on map 1
        struct At
        {
            std::uint32_t map;
            float x;
            float y;
        };
        std::array<At, 6> const at{{{0, x0, y0},
                                    {0, x0 + aside, y0},
                                    {0, x0 + aside, y0 + across},
                                    {0, x0, y0 + across},
                                    {1, x1, y1},
                                    {1, x1, y1 + shortcut}}};
        TravelGraphBuilder builder;
        for (std::uint32_t n = 0; n < at.size(); ++n)
            builder.AddNode(n + 1, "shortcut", at[n].map, at[n].x, at[n].y, 0.0f);
        std::array<std::pair<std::uint32_t, std::uint32_t>, 8> const walks{
            {{1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3}, {5, 6}, {6, 5}}};
        for (auto const& [from, to] : walks)
        {
            TravelGraphBuilder::LinkRow row;
            row.fromId = from;
            row.toId = to;
            row.type = 1;
            row.distance = std::hypot(at[from - 1].x - at[to - 1].x, at[from - 1].y - at[to - 1].y);
            builder.AddLink(row);
        }
        for (auto const& [from, to] : {std::pair{1u, 5u}, std::pair{6u, 4u}})
        {
            TravelGraphBuilder::LinkRow row;  // both sides, no level: open to every level
            row.fromId = from;
            row.toId = to;
            row.type = 2;
            row.object = 1000 + from;
            row.extraCost = 3.0f;
            builder.AddLink(row);
        }
        std::string error;
        TravelGraph graph;
        bool ok = builder.EndLinks(error);
        if (ok)
            for (auto const& [from, to] : walks)
                for (std::uint32_t nr = 0; nr < 2; ++nr)
                {
                    At const& point = at[(nr ? to : from) - 1];
                    builder.AddPoint(from, to, nr, point.map, point.x, point.y, 0.0f);
                }
        if (!ok || !builder.Finish(graph, error))
        {
            std::printf("  shortcut: %s\n", error.c_str());
            continue;
        }
        graph.Publish(0, 0);
        ++built;

        // The cells along the shortcut, deadly for the level: an ordinary creature 6 to 15 levels above, or guards
        std::vector<DangerRules::Row> rows;
        int const cx = static_cast<int>(std::floor(x1 / DangerRules::CELL_YARDS));
        for (int cy = static_cast<int>(std::floor(y1 / DangerRules::CELL_YARDS));
             cy <= static_cast<int>(std::floor((y1 + shortcut) / DangerRules::CELL_YARDS)); ++cy)
            for (std::uint8_t rowSide = 0; rowSide < DangerRules::SIDE_COUNT; ++rowSide)
            {
                Member const member{Rank::Normal,
                                    static_cast<std::uint8_t>(std::min(TOP_LEVEL, level + 6 + int(g.Next() % 10))),
                                    guards};
                rows.push_back(PlantedRow(cx, cy, rowSide, 0, -5, 5, FactsOf({member})));
                rows.back().map = 1;
            }
        DangerRules::Known const facts = Planted(rows);
        DangerRules::Known folded = facts;
        DangerRules::FoldGraph(facts, graph, folded.links);
        folded.graphLinks = graph.LinkCount();
        for (TravelGraphLink const& link : graph.LinksOf(4))
        {
            DangerRules::LinkDanger const* const danger = folded.LinkOf(graph.IndexOfLink(link), side);
            deadly += link.to == 5 && danger && DangerRules::TermOf(*danger, level).deadly;
        }

        double const longWalk = (2.0 * aside + across) / RUN_SPEED;
        PublishForTest(&folded);
        TravelRoute::Result const priced = SearchWay(graph, 0, 3, side, level);
        TravelRoute::Result unpriced;
        {
            DangerRules::Unpriced const scope;
            unpriced = SearchWay(graph, 0, 3, side, level);
        }
        PublishForTest(nullptr);
        TravelRoute::Result const none = SearchWay(graph, 0, 3, side, level);
        if (!priced.ok || portalLegs(priced) || !Near(priced.totalSeconds, longWalk))
            Count(r11, "R11 with the store", seed, i);
        if (!none.ok || portalLegs(none) != 2 || !(none.totalSeconds < longWalk))
            Count(r11, "R11 no store", seed, i);
        if (!unpriced.ok || portalLegs(unpriced) != 2 || !Near(unpriced.totalSeconds, none.totalSeconds))
            Count(r11, "R11 under Unpriced", seed, i);
    }
    PublishForTest(before);
    Check(built == CASES && deadly == CASES,
          "R11 cases: every graph built, its shortcut's walk deadly for the bot's level and side");
    Check(!r11,
          "R11: the store keeps the long walk through quiet ground with no portal leg; with no store and under "
          "Unpriced the shortcut through deadly ground");
}

}  // namespace

// AW14: a caller's row about a candidate it leaves, at most once per candidate and minute
void TestRowDue()
{
    Check(RowDue(0, 1000), "row due: never written, due");
    Check(!RowDue(1000, 1000) && !RowDue(1000, 1000 + TARGET_ROW_SECONDS - 1), "row due: not within the minute");
    Check(RowDue(1000, 1000 + TARGET_ROW_SECONDS) && RowDue(1000, 5000), "row due: due from the minute on");
}

// AW37b, the trigger `way danger` (INTERFACE.md §4.8). W1: over generated trips and their routes (a held trip, its
// request waited for or applied, replans with a new request id, a next trip), a route applied is judged exactly once
// and a trip with no new route is never judged again. W2: a refusal's effects run record, row, end, and after the
// record's note (under RECORD_WORD, the cause table's goal row) the record refuses the goal's place as the status
// ends. W3: a spared trip and a way out of deadly ground to a goal that is not deadly are never ended
void TestWayTrigger()
{
    using DangerRules::Ground;
    using DangerRules::WayEffect;
    using DangerRules::WayVerdict;
    Generator g;
    g.state = SEED ^ 0x37b;
    int w1 = 0;
    int applied = 0;
    constexpr int TRIPS = 400;
    DangerRules::JudgedPair last;
    std::uint32_t tripId = 0;
    std::uint32_t requestId = 0;
    for (int t = 0; t < TRIPS; ++t)
    {
        ++tripId;
        int const routes = 1 + static_cast<int>(g.Next() % 3);  // the first route and its replans
        for (int r = 0; r < routes; ++r)
        {
            ++requestId;
            int judged = 0;
            bool heldApplied = false;
            int const ticks = 2 + static_cast<int>(g.Next() % 8);
            int const waitTicks = static_cast<int>(g.Next() % 3);
            for (int k = 0; k < ticks; ++k)
            {
                bool const held = g.Next() % 10 != 0;
                bool const waiting = k < waitTicks;
                bool const hasLegs = !waiting;
                heldApplied = heldApplied || (held && !waiting);
                if (DangerRules::WayDue(held, waiting, hasLegs, tripId, requestId, last))
                {
                    ++judged;
                    w1 += waiting || !hasLegs || !held;
                    last = {tripId, requestId};
                }
            }
            w1 += judged != (heldApplied ? 1 : 0);
            applied += heldApplied;
        }
    }
    // a fixed run: the first applied tick judges, the same pair never again, a replan's new request again
    DangerRules::JudgedPair fixed;
    bool const first = DangerRules::WayDue(true, false, true, 5, 9, fixed);
    fixed = {5, 9};
    bool const again = DangerRules::WayDue(true, false, true, 5, 9, fixed);
    bool const replan = DangerRules::WayDue(true, false, true, 5, 10, fixed);
    bool const waiting = DangerRules::WayDue(true, true, false, 5, 10, fixed);
    bool const none = DangerRules::WayDue(false, false, true, 6, 11, fixed);
    bool const noLegs = DangerRules::WayDue(true, false, false, 6, 11, fixed);
    Check(!w1 && applied > TRIPS && first && !again && replan && !waiting && !none && !noLegs,
          "W1: a route applied to a held trip is judged once; no new route, no judgement");

    // W2: the order of the effects per verdict, and the record's answer at the end
    DangerRules::WayEffects const refused = DangerRules::EffectsOf(WayVerdict::Refused);
    DangerRules::WayEffects const least = DangerRules::EffectsOf(WayVerdict::LeastDeadly);
    DangerRules::WayEffects const clear = DangerRules::EffectsOf(WayVerdict::Clear);
    bool const order = refused.count == 3 && refused.order[0] == WayEffect::Record &&
                       refused.order[1] == WayEffect::Row && refused.order[2] == WayEffect::End && least.count == 1 &&
                       least.order[0] == WayEffect::Row && clear.count == 0;
    bool const words = !std::strcmp(DangerRules::WayChoice(WayVerdict::Refused), "refused") &&
                       !std::strcmp(DangerRules::WayChoice(WayVerdict::LeastDeadly), "least_deadly") &&
                       DangerRules::WayChoice(WayVerdict::Clear) == nullptr &&
                       !std::strcmp(DangerRules::END_SOURCE, "abort:danger");
    FailedAttempts::CauseRow const* const row = FailedAttempts::FindCause(DangerRules::RECORD_WORD);
    int w2 = 0;
    for (int i = 0; i < 200; ++i)
    {
        FailedAttempts::Record record;
        FailedAttempts::Failure failure;
        failure.goal = {0, g.Range(-9000.0f, 3000.0f), g.Range(-6000.0f, 2000.0f), g.Range(0.0f, 150.0f)};
        failure.spot = {0, failure.goal.X + g.Range(300.0f, 3000.0f), failure.goal.Y, failure.goal.Z};
        std::uint32_t const now = 1000 + g.Next() % 100000;
        bool refusedAtEnd = false;
        bool openBefore = false;
        for (std::uint8_t e = 0; e < refused.count; ++e)
            if (refused.order[e] == WayEffect::Record)
                FailedAttempts::NoteFailure(record, DangerRules::RECORD_WORD, failure, now, 600000);
            else if (refused.order[e] == WayEffect::End)
                refusedAtEnd = FailedAttempts::IsPlaceRefused(record, failure.goal, now + 50) == "trip_deaths";
            else
                openBefore = FailedAttempts::IsPlaceRefused(record, failure.goal, now).empty();
        w2 += !refusedAtEnd || openBefore;
    }
    Check(order && words, "W2: a refusal writes the record, then its row, then ends; a least deadly way only a row");
    Check(row && row->target == FailedAttempts::Target::Goal && !w2,
          "W2: after a refusal's note the record refuses the goal's place under trip_deaths");

    // W3: generated parts; a spared trip, or a way out of deadly ground to a goal that is not deadly, has no end,
    // and every refused way of the bot's own trip ends
    int w3 = 0;
    for (int i = 0; i < 2000; ++i)
    {
        DangerRules::DangerPart part;
        part.start = static_cast<Ground>(g.Next() % 3);
        part.goal = static_cast<Ground>(g.Next() % 3);
        part.deadlyLinks = static_cast<std::uint16_t>(g.Next() % 4);
        part.deadlyYards = part.deadlyLinks ? g.Range(10.0f, 3000.0f) : 0.0f;
        for (bool const spared : {false, true})
        {
            DangerRules::WayEffects const effects = DangerRules::EffectsOf(DangerRules::JudgeWay(part, spared));
            bool ends = false;
            for (std::uint8_t e = 0; e < effects.count; ++e)
                ends = ends || effects.order[e] == WayEffect::End;
            bool const out = part.start == Ground::Deadly && part.goal != Ground::Deadly;
            w3 += ends && (spared || out);
            w3 += !ends && !spared && DangerRules::Refuses(part);
        }
    }
    Check(!w3, "W3: a GM's order and a way out of deadly ground to a goal not deadly are never ended");
}

int main(int argc, char** argv)
{
    TestValues();
    TestDisc();
    TestAge();
    TestReasons();
    TestBusyInLists();
    TestTwoFights();
    TestPlayers();
    TestRings();
    TestEmpty();
    TestSidesGenerated();
    TestFill();
    TestFillGenerated();
    TestJoin();
    TestBegan();
    TestRows();
    TestScene();
    TestPointFixed();
    TestPointGenerated();
    TestSafeGenerated();
    // The data file, relative to the module root (the runner's working directory) unless named
    TestRiseCases(argc > 1 ? argv[1] : "tests/data/aw-rise-cases.tsv");
    TestDeadFixed();
    TestDeadGenerated();
    TestAw9Fixed();
    TestAw9Generated();
    TestDeathEdgeFixed();
    TestDeathEdgeGenerated();
    TestFightFixture(argc > 2 ? argv[2] : "tests/data/aw-fights.tsv");
    TestStrengthFixed();
    TestAw12Named();
    TestAw12Generated();
    TestReplay(argc > 2 ? argv[2] : "tests/data/aw-fights.tsv");
    TestRowDue();
    TestGroundFixed();
    TestGroundGenerated();
    TestGroundLayers();
    TestGroundTwin(argc > 3 ? argv[3] : "tests/data/aw-ground-cases.tsv");
    TestPictureFacts();
    TestLinkFoldFixed();
    TestLinkFoldGenerated();
    TestNoRareGenerated();
    TestWayRules();
    TestWayTerm();
    TestShortcutGenerated();
    TestWayTrigger();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
