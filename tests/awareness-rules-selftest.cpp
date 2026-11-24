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
#include <tuple>
#include <vector>

#include "AwarenessRules.h"

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
}
}  // namespace

int main()
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
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
