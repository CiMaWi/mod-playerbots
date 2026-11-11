/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the world PvP fight book (src/Ai/World/Pvp/FightBookRules.h) over synthetic fights: a new fight
// with its starter, target and reason; joins into the fight of a participant and into the nearest fight within its
// radius; a second cluster as a new fight; sides, deaths, the end by combat over after the end grace and by the
// safety cap; the bounds (fights per map, participants per fight, the ended ring's calm time); the PvpTest tag; the id
// counter; the fight memory's meetings and its eviction; the end rows' anchor when nobody is left on the map. The
// rules' ends (WP6, with FightRules.h's CheckFightEnd as the judge): timeout after NoHitSeconds without a hit between
// the sides (length to that hit), stalemate, wiped by deaths, absence or retreat (a retreated participant still in
// PvP combat keeps its side), the grace beside them; MarkRetreated.
#include <cstdint>
#include <iostream>
#include <set>
#include <string>

#include "FightBookRules.h"
#include "FightRules.h"

using WorldPvp::CameIn;
using WorldPvp::Combatant;
using WorldPvp::CombatNote;
using WorldPvp::EndCause;
using WorldPvp::Fight;
using WorldPvp::FightBook;
using WorldPvp::FightBounds;
using WorldPvp::FightMemory;
using WorldPvp::FightParticipant;
using WorldPvp::FightReason;
using WorldPvp::Side;

namespace
{
constexpr uint32_t SECOND = 1000;
constexpr uint32_t MINUTE = 60 * SECOND;

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
    std::cout << "FAIL: " << name << "\n";
}

Combatant Bot(uint64_t guid, Side side, float x, float y, uint8_t level = 60)
{
    Combatant combatant;
    combatant.guid = guid;
    combatant.side = side;
    combatant.level = level;
    combatant.x = x;
    combatant.y = y;
    return combatant;
}

Combatant Real(uint64_t guid, Side side, float x, float y)
{
    Combatant combatant = Bot(guid, side, x, y);
    combatant.real = true;
    return combatant;
}

FightBounds DefaultBounds() { return FightBounds::From(WorldPvpConfig{}); }

// Everyone in PvP combat while `fighting` holds the guid
struct SetProbe
{
    std::set<uint64_t> const& fighting;
    bool operator()(FightParticipant const& participant) const { return fighting.count(participant.guid) != 0; }
};

// The rules' probe: `side` in PvP combat with the other side, `outside` in PvP combat with someone else only, `dead`
// dead, `gone` off the map
struct StateProbe
{
    std::set<uint64_t> const& side;
    std::set<uint64_t> const& outside;
    std::set<uint64_t> const& dead;
    std::set<uint64_t> const& gone;
    bool operator()(FightParticipant& participant) const
    {
        participant.here = gone.count(participant.guid) == 0;
        participant.alive = dead.count(participant.guid) == 0;
        participant.sideCombat = participant.here && side.count(participant.guid) != 0;
        return participant.sideCombat || (participant.here && outside.count(participant.guid) != 0);
    }
};

WorldPvp::FightRuleValues const RULES{};

WorldPvp::FightEndCheck Judge(WorldPvp::FightClock const& clock) { return WorldPvp::CheckFightEnd(clock, RULES); }

void CheckStartAndJoins()
{
    std::atomic<uint32_t> ids{0};
    FightBook book(DefaultBounds(), ids);
    CombatNote const start = book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 1000);
    Check(start.started && start.fightId == 1, "a new fight gets id 1");
    Check(start.reason == FightReason::AttackOnSight, "a bot's start is attack on sight");
    Check(start.joinCount == 2 && start.joins[0].cameIn == CameIn::Started && start.joins[1].cameIn == CameIn::Defended,
          "the starter started, its target defended");
    Fight const& fight = book.Active().front();
    Check(fight.starter == 1 && fight.target == 2, "starter and target");

    CombatNote const again = book.OnCombat(Bot(2, Side::Horde, 10, 0), Bot(1, Side::Alliance, 0, 0), 1500);
    Check(!again.started && again.fightId == 1 && again.joinCount == 0, "the same pair again records nothing new");

    CombatNote const joined = book.OnCombat(Bot(3, Side::Horde, 200, 0), Bot(1, Side::Alliance, 0, 0), 2000);
    Check(joined.fightId == 1 && joined.joinCount == 1 && joined.joins[0].cameIn == CameIn::Joined,
          "attacking a participant joins its fight, from any distance");
    CombatNote const targeted = book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(4, Side::Horde, 5, 5), 2500);
    Check(targeted.joinCount == 1 && targeted.joins[0].cameIn == CameIn::Targeted, "a player a participant attacks");

    CombatNote const near = book.OnCombat(Bot(5, Side::Alliance, 20, 10), Bot(6, Side::Horde, 30, 10), 3000);
    Check(near.fightId == 1 && !near.started && near.joinCount == 2, "a new pair within the radius joins the fight");
    Check(near.joins[0].cameIn == CameIn::Joined && near.joins[1].cameIn == CameIn::Targeted,
          "the pair's aggressor joined, its target was targeted");

    CombatNote const far = book.OnCombat(Bot(7, Side::Alliance, 2000, 0), Bot(8, Side::Horde, 2010, 0), 3500);
    Check(far.started && far.fightId == 2 && book.Active().size() == 2, "a second cluster is a new fight, id 2");

    Fight const* first = book.FindFightOf(1);
    Check(first && first->Count(Side::Alliance) == 2 && first->Count(Side::Horde) == 4, "sides by faction");
    Check(first && first->radius >= DefaultBounds().radius && first->radius <= DefaultBounds().radiusMax,
          "the radius stays within its bounds");
}

void CheckReasonsAndTag()
{
    std::atomic<uint32_t> ids{0};
    FightBook book(DefaultBounds(), ids);
    CombatNote const real = book.OnCombat(Real(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 5, 0), 0);
    Check(real.reason == FightReason::RealPlayer, "a real player's start");
    Check(std::string(WorldPvp::Word(real.reason)) == "real_player", "the reason's word");

    Combatant forced = Bot(3, Side::Alliance, 5000, 0);
    forced.forced = true;
    CombatNote const pvptest = book.OnCombat(forced, Bot(4, Side::Horde, 5005, 0), 0);
    Check(pvptest.reason == FightReason::PvpTest && book.FindFightOf(3)->tagged, "a forced fighter tags its fight");

    Combatant late = Bot(5, Side::Horde, 2, 0);
    late.forced = true;
    book.OnCombat(late, Bot(1, Side::Alliance, 0, 0), 100);
    Check(book.FindFightOf(1)->tagged && book.FindFightOf(1)->reason == FightReason::RealPlayer,
          "a forced fighter joining later tags the fight, the reason stays");
    Check(std::string(WorldPvp::Word(CameIn::Defended)) == "defended" &&
              std::string(WorldPvp::Word(EndCause::StalemateCap)) == "stalemate_cap",
          "came-in and end words");
}

void CheckDeathsAndEnd()
{
    std::atomic<uint32_t> ids{41};
    FightBook book(DefaultBounds(), ids);
    book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
    Check(book.Active().front().id == 42, "ids count on from the counter");
    CombatNote const kill = book.OnKill(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 5 * SECOND);
    Check(kill.fightId == 42 && book.FindFightOf(2)->Deaths(Side::Horde) == 1, "a kill counts the death");
    book.OnKill(Bot(9, Side::Alliance, 3, 0), Bot(2, Side::Horde, 10, 0), 6 * SECOND);
    Check(book.FindFightOf(2)->Deaths(Side::Horde) == 2 && book.FindFightOf(9), "a killer from outside joins");

    std::set<uint64_t> fighting = {1, 9};
    std::vector<Fight> ended;
    book.Tick(7 * SECOND, SetProbe{fighting}, ended);
    Check(ended.empty() && book.Active().size() == 1, "still fighting: no end");
    fighting.clear();
    book.Tick(8 * SECOND, SetProbe{fighting}, ended);
    Check(ended.empty(), "no PvP combat within the end grace: no end yet");
    uint32_t const closeMs = 7 * SECOND + DefaultBounds().endGraceMs;
    book.Tick(closeMs, SetProbe{fighting}, ended);
    Check(ended.size() == 1 && ended[0].cause == EndCause::CombatOver && book.Active().empty(),
          "no PvP combat for the end grace ends the fight");
    Check(ended[0].endMs - ended[0].startMs == 7 * SECOND, "the fight's length runs to the last PvP combat");
    Check(book.Ended().size() == 1 && book.Ended().front().id == 42, "the ended fight is kept as a calm area");
    Check(!book.FindFightOf(1), "an ended fight's participants are free");

    book.Tick(closeMs + DefaultBounds().calmMs + 1, SetProbe{fighting}, ended);
    Check(book.Ended().empty(), "the calm area is dropped after the calm time");
}

void CheckSafetyCap()
{
    std::atomic<uint32_t> ids{0};
    FightBook book(DefaultBounds(), ids);
    book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
    std::set<uint64_t> const fighting = {1, 2};
    std::vector<Fight> ended;
    book.Tick(DefaultBounds().maxFightMs - 1, SetProbe{fighting}, ended);
    Check(ended.empty(), "below the cap the fight goes on");
    book.Tick(DefaultBounds().maxFightMs, SetProbe{fighting}, ended);
    Check(ended.size() == 1 && ended[0].cause == EndCause::StalemateCap, "the safety cap ends a fight");
}

// A pause shorter than the end grace keeps one fight (new combat continues it); a longer one ends it, and the next
// combat is a new fight. Grace 0 ends at the first tick without PvP combat; the cap within the grace is a stalemate.
void CheckEndGrace()
{
    std::atomic<uint32_t> ids{0};
    FightBook book(DefaultBounds(), ids);
    uint32_t const graceMs = DefaultBounds().endGraceMs;
    Check(graceMs == 10 * SECOND, "the end grace defaults to 10 s");
    Combatant const alliance = Bot(1, Side::Alliance, 0, 0);
    Combatant const horde = Bot(2, Side::Horde, 10, 0);
    book.OnCombat(alliance, horde, 0);
    std::set<uint64_t> fighting = {1, 2};
    std::set<uint64_t> const none;
    std::vector<Fight> ended;
    book.Tick(SECOND, SetProbe{fighting}, ended);
    book.Tick(SECOND + graceMs - SECOND, SetProbe{none}, ended);
    Check(ended.empty() && book.Active().size() == 1, "a pause shorter than the grace: the fight goes on");
    CombatNote const again = book.OnCombat(horde, alliance, graceMs);
    Check(!again.started && again.fightId == 1, "combat within the grace continues the same fight");
    CombatNote const near = book.OnCombat(Bot(3, Side::Alliance, 5, 5), Bot(4, Side::Horde, 15, 5), graceMs);
    Check(!near.started && near.fightId == 1, "a new pair within the radius during the grace joins the fight");
    book.Tick(graceMs + graceMs - SECOND, SetProbe{none}, ended);
    Check(ended.empty(), "the grace counts from the last combat noted");
    book.Tick(graceMs + graceMs, SetProbe{none}, ended);
    Check(ended.size() == 1 && ended[0].id == 1 && ended[0].cause == EndCause::CombatOver &&
              ended[0].endMs == graceMs && ended[0].participants.size() == 4,
          "a pause as long as the grace ends the one fight, all four in it");
    CombatNote const later = book.OnCombat(alliance, horde, 3 * graceMs);
    Check(later.started && later.fightId == 2, "combat after the end is a new fight");

    WorldPvpConfig config;
    config.EndGraceSeconds = 0;
    FightBook quick(FightBounds::From(config), ids);
    quick.OnCombat(alliance, horde, 0);
    std::vector<Fight> quickEnded;
    quick.Tick(SECOND, SetProbe{none}, quickEnded);
    Check(quickEnded.size() == 1 && quickEnded[0].cause == EndCause::CombatOver,
          "grace 0: the first tick without PvP combat ends the fight");

    FightBook capped(DefaultBounds(), ids);
    capped.OnCombat(alliance, horde, 0);
    std::vector<Fight> cappedEnded;
    capped.Tick(DefaultBounds().maxFightMs - SECOND, SetProbe{fighting}, cappedEnded);
    capped.Tick(DefaultBounds().maxFightMs, SetProbe{none}, cappedEnded);
    Check(cappedEnded.size() == 1 && cappedEnded[0].cause == EndCause::StalemateCap,
          "the safety cap reached within the grace ends the fight as stalemate_cap");
}

void CheckBounds()
{
    WorldPvpConfig config;
    config.MaxActiveFights = 2;
    config.MaxParticipants = 3;
    std::atomic<uint32_t> ids{0};
    FightBook book(FightBounds::From(config), ids);
    book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 5, 0), 0);
    book.OnCombat(Bot(3, Side::Alliance, 1000, 0), Bot(4, Side::Horde, 1005, 0), 0);
    CombatNote const third = book.OnCombat(Bot(5, Side::Alliance, 3000, 0), Bot(6, Side::Horde, 3005, 0), 0);
    Check(third.fightId == 0 && book.Active().size() == 2 && book.RefusedFights() == 1, "at most MaxActiveFights");
    book.OnCombat(Bot(7, Side::Horde, 2, 0), Bot(1, Side::Alliance, 0, 0), 0);
    CombatNote const full = book.OnCombat(Bot(8, Side::Horde, 2, 0), Bot(1, Side::Alliance, 0, 0), 0);
    Check(full.joinCount == 0 && book.FindFightOf(1)->participants.size() == 3 && book.RefusedParticipants() == 1,
          "at most MaxParticipants");

    // The ended ring never holds more than its capacity
    FightBook ring(DefaultBounds(), ids);
    std::set<uint64_t> const none;
    std::vector<Fight> ended;
    for (uint64_t pair = 0; pair < WorldPvp::MAX_ENDED_FIGHTS + 10; ++pair)
    {
        ring.OnCombat(Bot(100 + 2 * pair, Side::Alliance, 0, 0), Bot(101 + 2 * pair, Side::Horde, 5, 0), 0);
        ring.Tick(DefaultBounds().endGraceMs, SetProbe{none}, ended);
    }
    Check(ring.Ended().size() == WorldPvp::MAX_ENDED_FIGHTS, "the ended ring is bounded");
}

void CheckMemory()
{
    FightMemory memory;
    uint32_t const window = 10 * MINUTE;
    Check(!memory.SeenRecently(1, 0, window), "an unknown enemy is a meeting");
    memory.Remember(1, 0, 16);
    Check(memory.SeenRecently(1, 5 * MINUTE, window), "seen within the window: no meeting");
    Check(memory.SeenRecently(1, 14 * MINUTE, window), "staying in sight refreshes the time");
    Check(!memory.SeenRecently(1, 25 * MINUTE, window), "unseen for longer than the window: a meeting again");
    memory.Remember(1, 25 * MINUTE, 16);
    Check(memory.Count() == 1, "a remembered enemy takes one slot");

    FightMemory small;
    for (uint64_t guid = 1; guid <= 4; ++guid)
        small.Remember(guid, static_cast<uint32_t>(guid) * SECOND, 3);
    Check(small.Count() == 3, "the memory holds at most its capacity");
    Check(!small.SeenRecently(1, 5 * SECOND, window) && small.SeenRecently(4, 5 * SECOND, window),
          "a full memory forgets its oldest enemy");
    FightMemory big;
    for (uint64_t guid = 1; guid <= 100; ++guid)
        big.Remember(guid, static_cast<uint32_t>(guid), 1000);
    Check(big.Count() == WorldPvp::MAX_RECENT_ENEMIES, "the capacity is clamped to MAX_RECENT_ENEMIES");
    Check(!memory.SeenRecently(2, 0xFFFFFF00u, window) && memory.Count() == 1, "an unknown guid near the clock's wrap");
    memory.Remember(2, 0xFFFFFF00u, 16);
    Check(memory.SeenRecently(2, 0x100u, window), "the millisecond clock's wrap keeps the window");
}

// The end rows' player and place when nobody of the fight is left on its map: the starter's last place, else the
// first participant's
void CheckEndAnchor()
{
    std::atomic<uint32_t> ids{0};
    FightBook book(DefaultBounds(), ids);
    Combatant starter = Bot(1, Side::Alliance, 0, 0);
    starter.zone = 267;
    starter.area = 271;
    book.OnCombat(starter, Bot(2, Side::Horde, 10, 0), 0);
    Fight fight = book.Active().front();
    FightParticipant const* anchor = fight.Anchor();
    Check(anchor && anchor->guid == 1 && anchor->zone == 267 && anchor->area == 271 && anchor->x == 0.0f,
          "the end rows' anchor is the starter at its zone, area and place");
    fight.starter = 99;
    anchor = fight.Anchor();
    Check(anchor && anchor->guid == fight.participants.front().guid, "without the starter the first participant");
    fight.participants.clear();
    Check(!fight.Anchor(), "no participant: no anchor");
}
}  // namespace

void CheckRuleEnds()
{
    std::set<uint64_t> const none;
    std::set<uint64_t> const both = {1, 2};
    std::vector<Fight> ended;

    {
        std::atomic<uint32_t> ids{0};
        FightBook book(DefaultBounds(), ids);
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
        book.Tick(10 * SECOND, StateProbe{both, none, none, none}, Judge, ended);
        book.Tick(39 * SECOND, StateProbe{none, both, none, none}, Judge, ended);
        Check(ended.empty(), "PvP combat with others only: no timeout before NoHitSeconds");
        book.Tick(40 * SECOND, StateProbe{none, both, none, none}, Judge, ended);
        Check(ended.size() == 1 && ended.back().cause == EndCause::Timeout && ended.back().endMs == 10 * SECOND,
              "no hit between the sides for NoHitSeconds: timeout, its length to the last such hit");
    }
    {
        std::atomic<uint32_t> ids{0};
        FightBook book(DefaultBounds(), ids);
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
        book.Tick(20 * SECOND, StateProbe{none, both, none, none}, Judge, ended);
        book.OnCombat(Bot(2, Side::Horde, 10, 0), Bot(1, Side::Alliance, 0, 0), 25 * SECOND);
        book.Tick(54 * SECOND, StateProbe{none, both, none, none}, Judge, ended);
        Check(book.Active().size() == 1, "a noted combat between the sides restarts the hit clock");
        book.Tick(55 * SECOND, StateProbe{none, both, none, none}, Judge, ended);
        Check(ended.back().cause == EndCause::Timeout && ended.back().endMs == 25 * SECOND, "timeout from that hit");
    }
    {
        std::atomic<uint32_t> ids{0};
        FightBook book(DefaultBounds(), ids);
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
        for (uint32_t second = 1; second < 300; second += 20)
            book.Tick(second * SECOND, StateProbe{both, none, none, none}, Judge, ended);
        book.Tick(299 * SECOND, StateProbe{both, none, none, none}, Judge, ended);
        Check(book.Active().size() == 1, "hits between the sides: no end before StalemateSeconds");
        book.Tick(300 * SECOND, StateProbe{both, none, none, none}, Judge, ended);
        Check(ended.back().cause == EndCause::Stalemate && ended.back().endMs == 300 * SECOND,
              "a fight as old as StalemateSeconds ends as stalemate");
    }
    {
        std::atomic<uint32_t> ids{0};
        FightBook book(DefaultBounds(), ids);
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
        book.Tick(5 * SECOND, StateProbe{std::set<uint64_t>{1}, none, std::set<uint64_t>{2}, none}, Judge, ended);
        Check(ended.back().cause == EndCause::Wiped && ended.back().endMs == 5 * SECOND,
              "a side all dead: wiped at once");
    }
    {
        std::atomic<uint32_t> ids{0};
        FightBook book(DefaultBounds(), ids);
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
        book.Tick(2 * SECOND, StateProbe{both, none, none, std::set<uint64_t>{2}}, Judge, ended);
        Check(ended.back().cause == EndCause::Wiped, "a side all gone from the map: wiped");
    }
    {
        std::atomic<uint32_t> ids{0};
        FightBook book(DefaultBounds(), ids);
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
        book.OnCombat(Bot(3, Side::Alliance, 5, 0), Bot(2, Side::Horde, 10, 0), SECOND);
        uint32_t const fightId = book.Active().front().id;
        Check(book.MarkRetreated(2, 3 * SECOND) == fightId && book.Active().front().Find(2)->retreated &&
                  book.Active().front().Find(2)->retreatedMs == 3 * SECOND,
              "MarkRetreated marks the participant and names its fight");
        Check(book.MarkRetreated(99, 3 * SECOND) == 0, "MarkRetreated of a guid in no fight: 0");
        book.Tick(4 * SECOND, StateProbe{both, none, none, none}, Judge, ended);
        Check(book.Active().size() == 1, "a retreated participant still in PvP combat keeps its side in the fight");
        book.Tick(5 * SECOND, StateProbe{std::set<uint64_t>{1}, none, none, none}, Judge, ended);
        Check(ended.back().cause == EndCause::Wiped && ended.back().endMs == 5 * SECOND,
              "its side all retreated and out of PvP combat: wiped");
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(4, Side::Horde, 10, 0), 6 * SECOND);
        book.MarkRetreated(1, 7 * SECOND);
        Fight const* const fight = book.FindFightOf(1);
        WorldPvp::FightClock clock = FightBook::ClockOf(*fight, 7 * SECOND);
        Check(clock.active[static_cast<size_t>(Side::Alliance)] == 1, "ClockOf: in PvP combat at the last tick counts");
        book.Tick(8 * SECOND, StateProbe{none, std::set<uint64_t>{4}, none, none}, Judge, ended);
        Check(ended.back().cause == EndCause::Wiped, "the retreater's side is out once it leaves PvP combat");
    }
    {
        std::atomic<uint32_t> ids{0};
        FightBook book(DefaultBounds(), ids);
        book.OnCombat(Bot(1, Side::Alliance, 0, 0), Bot(2, Side::Horde, 10, 0), 0);
        book.Tick(SECOND, StateProbe{both, none, none, none}, Judge, ended);
        book.Tick(10 * SECOND, StateProbe{none, none, none, none}, Judge, ended);
        Check(book.Active().size() == 1, "the grace still holds beside the rules' ends");
        book.Tick(11 * SECOND, StateProbe{none, none, none, none}, Judge, ended);
        Check(ended.back().cause == EndCause::CombatOver && ended.back().endMs == SECOND,
              "nobody in PvP combat for the grace: combat_over before the timeout");
    }
}

int main()
{
    CheckStartAndJoins();
    CheckReasonsAndTag();
    CheckDeathsAndEnd();
    CheckSafetyCap();
    CheckEndGrace();
    CheckBounds();
    CheckMemory();
    CheckEndAnchor();
    CheckRuleEnds();
    std::cout << "fight-book-selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
