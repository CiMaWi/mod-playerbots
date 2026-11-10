/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the world PvP fight book (src/Ai/World/Pvp/FightBookRules.h) over synthetic fights: a new fight
// with its starter, target and reason; joins into the fight of a participant and into the nearest fight within its
// radius; a second cluster as a new fight; sides, deaths, the end by combat over and by the safety cap; the bounds
// (fights per map, participants per fight, the ended ring's calm time); the PvpTest tag; the id counter; the fight
// memory's meetings and its eviction; the end rows' anchor when nobody is left on the map.
#include <cstdint>
#include <iostream>
#include <set>
#include <string>

#include "FightBookRules.h"

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
    Check(ended.size() == 1 && ended[0].cause == EndCause::CombatOver && book.Active().empty(),
          "no PvP combat left ends the fight");
    Check(ended[0].endMs - ended[0].startMs == 8 * SECOND, "the fight's length");
    Check(book.Ended().size() == 1 && book.Ended().front().id == 42, "the ended fight is kept as a calm area");
    Check(!book.FindFightOf(1), "an ended fight's participants are free");

    book.Tick(8 * SECOND + DefaultBounds().calmMs + 1, SetProbe{fighting}, ended);
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
        ring.Tick(SECOND, SetProbe{none}, ended);
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

int main()
{
    CheckStartAndJoins();
    CheckReasonsAndTag();
    CheckDeathsAndEnd();
    CheckSafetyCap();
    CheckBounds();
    CheckMemory();
    CheckEndAnchor();
    std::cout << "fight-book-selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
