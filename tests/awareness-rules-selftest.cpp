/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Selftest of the bot's picture of the units around it (src/Ai/Awareness/AwarenessRules.h, awareness AW2): its
// types for both sides, the distance against the disc of a unit's estimate, the age rule and why an enemy is in a
// fight. Generated cases use a fixed seed, printed with every failure.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/Awareness tests/awareness-rules-selftest.cpp

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "AwarenessRules.h"
#include "DeathRowRules.h"

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

    // Generated: the place test is Within for a free hostile creature, and a wider estimate never drops a joiner
    Generator generator;
    int broken = 0;
    for (int i = 0; i < 2000; ++i)
    {
        Seen const centre = Hostile(1, generator.Range(-30.0f, 30.0f), generator.Range(-30.0f, 30.0f));
        Seen const other =
            Hostile(2, generator.Range(-30.0f, 30.0f), generator.Range(-30.0f, 30.0f), generator.Range(0.0f, 10.0f));
        float const radius = generator.Range(0.5f, 20.0f);
        Seen wider = other;
        wider.estimateRadius += generator.Range(0.0f, 10.0f);
        bool const joins = WouldAssistByPlace(other, centre, radius);
        bool ok = joins == Within(other, centre, radius);
        ok = ok && (!joins || WouldAssistByPlace(wider, centre, radius));
        if (!ok)
            ++broken;
    }
    if (broken)
        std::printf("join: %d broken cases (seed %u)\n", broken, SEED);
    Check(!broken, "join: 2000 generated cases, the place test is Within and a wider estimate keeps a joiner");
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
          "rows: FightScene power, guards, allies and near capped, nothing above bit 42");

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
              ((amount >> 38) & 3) == 0 && (amount >> 40) == 0,
          "rows: FightEnemy without casts has no casts bit; creature type and health capped, nothing above bit 39");

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
    // one more reaching unit, a level above or a rank above normal each makes it not takeable
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
        ok = ok && !Takeable(more.Judge());
        PointCase higher = c;
        higher.units[at].levelDiff = static_cast<std::int16_t>(1 + generator.Next() % 8);
        ok = ok && !Takeable(higher.Judge());
        PointCase ranked = c;
        ranked.units[at].rank = static_cast<Rank>(1 + generator.Next() % 3);
        ok = ok && !Takeable(ranked.Judge());
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
    facts.atGrave = true;
    facts.healerInRange = true;
    Check(Is(facts, DeadAct::HealerRevive, DeadReason::WaitEnded), "dead 2: at the grave with a healer: its revive");
    facts.healerInRange = false;
    Check(Is(facts, DeadAct::ReviveGrave, DeadReason::WaitEnded), "dead 2: at the grave without one: revive there");
    facts.graveKnown = false;
    Check(Is(facts, DeadAct::ReviveHere, DeadReason::WaitEnded), "dead 2: no grave known: revive here");

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
    facts.atGrave = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3: at the grave without a healer: wait");
    facts.healerInRange = true;
    Check(Is(facts, DeadAct::HealerRevive, DeadReason::None), "dead 3: at the grave with a healer: its revive");
    facts.atGrave = false;
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
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::NoClearPoint), "dead 8: two reach the best: the healer");
    facts.best.answer.reaching = 1;
    facts.best.answer.topLevelDiff = 1;
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
    facts.atGrave = true;
    Check(Is(facts, DeadAct::WalkCorpse, DeadReason::None), "dead 3 closed: from the grave back to the corpse");
    facts = QuietGhost();
    facts.healerChosen = true;
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::Clear), "dead 3 closed: the healer once chosen holds no longer");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.best.answer.reaching = 2;
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoHealer), "dead 8 closed: two reach the best: wait, not the healer");
    facts.best.found = false;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoHealer), "dead 8 closed: no point found: wait");
    facts.ownedByPlayer = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::NoClearPoint), "dead 8 closed: a player's bot waits as before");
    facts = none;
    facts.unclearSeconds = UNCLEAR_WAIT_SECONDS;
    facts.graveWithoutHealer = true;
    Check(Is(facts, DeadAct::Rise, DeadReason::ReachedOne), "dead 8 closed: beside one creature it still rises");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.playerHold = PlayerHold::Healer;
    facts.deathsInRow = ROW_HEALER_AT;
    Check(Is(facts, DeadAct::Wait, DeadReason::Player), "dead 3 closed: a hold that asks for the healer waits");
    facts.corpseWalkFailed = true;
    Check(Is(facts, DeadAct::Wait, DeadReason::Player), "dead 3 closed: the hold before the failed walk");
    facts.playerHold = PlayerHold::None;
    facts.atGrave = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3 closed: a walk that cannot start waits");
    facts.atGrave = false;
    facts.graveWalkFailed = true;
    facts.teleportSeen = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3 closed: a failed walk never teleports");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.corpseHere = false;
    facts.atGrave = true;
    Check(Is(facts, DeadAct::WaitGrave, DeadReason::NoHealer), "dead 3: no corpse here: the grave's wait as before");
    facts.atGrave = false;
    Check(Is(facts, DeadAct::WalkGrave, DeadReason::CorpseAway), "dead 3: no corpse here: the healer's way as before");
    facts = QuietGhost();
    facts.graveWithoutHealer = true;
    facts.healerChosen = true;
    facts.ghostSeconds = GHOST_BOUND_SECONDS;
    facts.atGrave = true;
    Check(Is(facts, DeadAct::ReviveGrave, DeadReason::WaitEnded), "dead 2 closed: the bound comes first");
    facts.atGrave = false;
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
    facts.atGrave = generator.Next() % 3 == 0;
    facts.healerInRange = generator.Next() % 2;
    facts.graveWalkFailed = generator.Next() % 4 == 0;
    facts.teleportSeen = generator.Next() % 2;
    facts.graveWithoutHealer = generator.Next() % 4 == 0;
    return facts;
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
        counterexamples += !bound || (facts.atGrave && step.act == DeadAct::TeleportGrave);
    }
    ReportDead("P1 bound-total", INVARIANT_CASES, counterexamples, SEED);

    // P2 Every run ends: the facts drawn anew each step, the seconds growing by 1, the memo carried as the caller
    // carries it (the healer once chosen, the unclear clock), a teleport puts the ghost at its grave; a Rise the core
    // accepts or a revive ends the run, by GHOST_BOUND_SECONDS + 2 steps at the latest (a run that starts past the
    // bound: 2 steps)
    counterexamples = 0;
    std::uint64_t steps = 0;
    int bounded = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        // A hard run: the core refuses every Rise and no healer is in range, so only the bound ends it
        bool const hard = generator.Next() % 2 == 0;
        std::uint32_t const accept = hard || generator.Next() % 3 == 0 ? 0 : 1 + generator.Next() % 4;  // 0: refuses
        std::uint32_t const start = generator.Next() % (GHOST_BOUND_SECONDS + 50);
        std::uint32_t const last = std::max(start, GHOST_BOUND_SECONDS) + 1;
        bool chosen = false;
        bool withoutHealer = generator.Next() % 8 == 0;
        bool teleported = false;
        std::int64_t unclearStart = -1;
        bool ended = false;
        std::uint32_t seconds = start;
        for (; seconds <= last && !ended; ++seconds)
        {
            ++steps;
            DeadFacts facts = RandomDeadFacts(generator);
            PastLineOne(facts);
            facts.ghostSeconds = seconds;
            facts.healerChosen = chosen;
            if (facts.best.found && facts.best.clear)
                unclearStart = -1;
            else if (unclearStart < 0)
                unclearStart = seconds;
            facts.unclearSeconds = unclearStart < 0 ? 0 : seconds - static_cast<std::uint32_t>(unclearStart);
            if (hard)
            {
                facts.healerInRange = false;
                facts.atGrave = false;
            }
            if (teleported)
            {
                facts.graveKnown = true;
                facts.atGrave = true;
            }
            // AW30b: the caller's search at the grave that finds no healer closes the healer's way for the run
            bool const searched = !withoutHealer && facts.atGrave && !facts.healerInRange && generator.Next() % 4 == 0;
            withoutHealer = GraveWithoutHealer(withoutHealer, facts.atGrave, searched, facts.healerInRange);
            facts.graveWithoutHealer = withoutHealer;
            DeadStep const step = NextOfDead(facts);
            chosen = chosen || ChoosesHealer(step);
            teleported = teleported || step.act == DeadAct::TeleportGrave;
            ended = Revives(step.act) || (step.act == DeadAct::Rise && accept && generator.Next() % accept == 0);
        }
        counterexamples += !ended;
        bounded += ended && start < GHOST_BOUND_SECONDS && seconds > GHOST_BOUND_SECONDS;
    }
    std::printf("AW8 reading: P2 ran %llu steps, %d runs reached the bound\n", static_cast<unsigned long long>(steps),
                bounded);
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

    // P4 The healer is kept: with healerChosen, where the grave did not lack a healer (AW30b), no step of the corpse's
    // way; more deaths in a row never turn a step off the corpse's way into one on it
    counterexamples = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        DeadFacts chosen = facts;
        chosen.healerChosen = true;
        counterexamples += !chosen.graveWithoutHealer && CorpseWay(NextOfDead(chosen).act);
        DeadFacts more = facts;
        more.deathsInRow += 1 + generator.Next() % 3;
        counterexamples += !CorpseWay(NextOfDead(facts).act) && CorpseWay(NextOfDead(more).act);
    }
    ReportDead("P4 healer-kept", INVARIANT_CASES, counterexamples, SEED);

    // P5 A ghost rises only where the rule allows; with deathsInRow >= ROW_HEALER_AT only a player's bot or a ghost
    // whose grave had no healer (AW30b)
    counterexamples = 0;
    int rises = 0;
    int closedRises = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        TowardRise(facts, generator);
        DeadStep const step = NextOfDead(facts);
        if (step.act != DeadAct::Rise)
            continue;
        ++rises;
        bool reason = false;
        if (step.reason == DeadReason::Clear)
            reason = facts.own.clear && facts.own.inView;
        else if (step.reason == DeadReason::ReachedOne)
            reason = facts.unclearSeconds >= UNCLEAR_WAIT_SECONDS && facts.atBest &&
                     ((facts.own.clear && facts.own.inView) || Takeable(facts.own));
        else if (step.reason == DeadReason::Leader)
            reason = facts.atLeader;
        bool const allowed = reason && facts.reclaimHere && !facts.leaderDown && facts.playerHold == PlayerHold::None &&
                             (facts.deathsInRow < ROW_HEALER_AT || facts.ownedByPlayer || facts.graveWithoutHealer);
        counterexamples += !allowed;
        closedRises += facts.graveWithoutHealer && facts.deathsInRow >= ROW_HEALER_AT && !facts.ownedByPlayer;
    }
    std::printf("AW8 reading: P5 judged %d rises, %d of a third death or more whose grave had no healer\n", rises,
                closedRises);
    Check(rises > INVARIANT_CASES / 100, "P5: the generated facts give rises to judge");
    Check(closedRises > INVARIANT_CASES / 1000, "P5: rises of a closed healer's way to judge");
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

    // P10 A grave without a healer closes its way (AW30b): with graveWithoutHealer and the corpse here, before the
    // bound no step of the healer's way but the wait at the grave of a walk that could not start (no enemy player's
    // hold); the row and the healer once chosen change nothing; Wait (no_healer) exactly where line 8 would have
    // chosen the healer: the same facts with the way open, the healer not chosen, a first death, no hold for the
    // healer and no failed walk
    counterexamples = 0;
    int closedWaits = 0;
    for (int i = 0; i < INVARIANT_CASES; ++i)
    {
        DeadFacts facts = RandomDeadFacts(generator);
        TowardRise(facts, generator);
        PastLineOne(facts);
        facts.ghostSeconds %= GHOST_BOUND_SECONDS;
        facts.corpseHere = true;
        facts.graveWithoutHealer = true;
        DeadStep const step = NextOfDead(facts);
        bool const graveWait = step == DeadStep{DeadAct::WaitGrave, DeadReason::NoHealer} && facts.corpseWalkFailed &&
                               facts.playerHold != PlayerHold::Healer;
        counterexamples += (HealerWay(step.act) || Revives(step.act)) && !graveWait;
        DeadFacts other = facts;
        other.healerChosen = !facts.healerChosen;
        other.deathsInRow = 1 + generator.Next() % (ROW_HEALER_AT + 3);
        counterexamples += !(NextOfDead(other) == step);
        bool const closedWait = step == DeadStep{DeadAct::Wait, DeadReason::NoHealer};
        closedWaits += closedWait;
        if (facts.playerHold == PlayerHold::Healer || facts.corpseWalkFailed)
        {
            counterexamples += closedWait;
            continue;
        }
        DeadFacts open = facts;
        open.graveWithoutHealer = false;
        open.healerChosen = false;
        open.deathsInRow = 1;
        counterexamples += closedWait != HealerWay(NextOfDead(open).act);
    }
    std::printf("AW30b reading: P10 waited (no_healer) in %d cases\n", closedWaits);
    Check(closedWaits > INVARIANT_CASES / 100, "P10: the generated facts give waits of a closed way to judge");
    ReportDead("P10 healer-closed", INVARIANT_CASES, counterexamples, SEED);
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
              RowIs("none", "none", DeadAct::Wait, DeadReason::Player, DeadReason::None) &&
              RowIs("none", "none", DeadAct::Wait, DeadReason::LeaderDown, DeadReason::None),
          "death row: wait/no_clear_point and wait/no_healer, no row for the other waits");
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
                      DeadReason::NoPath, DeadReason::NoClearPoint, DeadReason::WaitEnded})
                {
                    if (cause == c)
                        causeWord = Word(c);
                }
                std::string way = "none";
                std::string reason = "none";
                if (step.act == DeadAct::WalkCorpse || step.act == DeadAct::WalkPoint)
                    way = "corpse_walk", reason = "moving";
                else if (step.act == DeadAct::Wait && step.reason == DeadReason::NoClearPoint)
                    way = "wait", reason = "no_clear_point";
                else if (step.act == DeadAct::Wait && step.reason == DeadReason::NoHealer)
                    way = "wait", reason = "no_healer";
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
}  // namespace

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
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
