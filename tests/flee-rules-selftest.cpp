/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Selftest of the flight from attacking enemy players (src/Ai/Base/Actions/FleeRules.h, world-pvp WP21): when a flight
// begins, its three outcomes (escaped, cornered, bound), the time without a gain (only turns in which the bot can
// move, a pause of the AI counts once), and the trigger's rule (a flight under way runs to its outcome, a bot that
// stood flees again only in a new PvP combat). The movement itself is a live test (wp-flee).
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/Base/Actions tests/flee-rules-selftest.cpp

#include <cstdio>

#include "FleeRules.h"

namespace
{
using namespace FleeRules;

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

constexpr Limits LIMITS{40.0f, 6000, 30000};
constexpr std::uint32_t TICK = 200;

// A flight begun at `nowMs` with the nearest attacker `yards` off
Flight Begun(float yards, std::uint32_t nowMs)
{
    Flight flight;
    Begin(flight, yards, nowMs);
    return flight;
}

// Turns of TICK ms until an outcome or `forMs`; the nearest attacker's distance by `yardsAt` (ms since the start)
template <class YardsAt, class CanMoveAt>
Outcome Run(Flight& flight, std::uint32_t& nowMs, std::uint32_t forMs, YardsAt yardsAt, CanMoveAt canMoveAt)
{
    std::uint32_t const start = nowMs;
    while (nowMs - start < forMs)
    {
        nowMs += TICK;
        Outcome const outcome = Judge(flight, 1, yardsAt(nowMs - start), canMoveAt(nowMs - start), nowMs, LIMITS);
        if (outcome != Outcome::None)
            return outcome;
    }
    return Outcome::None;
}

bool Always(std::uint32_t) { return true; }

void TestBegin()
{
    Flight flight;
    Check(Begins(flight, 1, 8.0f, LIMITS), "begin: an enemy player in PvP combat with the bot, closer than the escape");
    Check(!Begins(flight, 0, 0.0f, LIMITS), "begin: not without an enemy player");
    Check(!Begins(flight, 2, 40.0f, LIMITS), "begin: not when every attacker is the escape distance away");
    Begin(flight, 8.0f, 1000);
    Check(flight.active && flight.startMs == 1000 && flight.bestYards == 8.0f && !flight.stood,
          "begin: the clock and the best distance start");
    Check(!Begins(flight, 1, 8.0f, LIMITS), "begin: not while a flight is under way");
    Flight stood;
    stood.stood = true;
    Check(!Begins(stood, 1, 8.0f, LIMITS), "begin: not after the bot stood in this PvP combat");
}

void TestEscaped()
{
    Flight flight = Begun(8.0f, 1000);
    Check(Judge(flight, 0, 0.0f, true, 1200, LIMITS) == Outcome::Escaped, "escaped: out of PvP combat");
    flight = Begun(8.0f, 1000);
    Check(Judge(flight, 2, 39.9f, true, 1200, LIMITS) == Outcome::None &&
              Judge(flight, 2, 40.0f, true, 1400, LIMITS) == Outcome::Escaped,
          "escaped: the nearest attacker the escape distance away");
    // An attacker that stands still: 7 yd a second gained, escaped in under 5 s
    flight = Begun(8.0f, 1000);
    std::uint32_t now = 1000;
    Check(Run(
              flight, now, 30000, [](std::uint32_t ms) { return 8.0f + 7.0f * ms / 1000.0f; }, Always) ==
                  Outcome::Escaped &&
              now - 1000 < 5000,
          "escaped: a bot that gains on its attacker");
    End(flight, Outcome::Escaped);
    Check(!flight.active && !flight.stood && Begins(flight, 1, 8.0f, LIMITS), "escaped: it may flee again");
}

void TestCornered()
{
    // A pursuer that stays 3 yd behind: no gain, cornered after the cornered time and not before
    Flight flight = Begun(8.0f, 1000);
    std::uint32_t now = 1000;
    Check(Run(
              flight, now, 30000, [](std::uint32_t) { return 3.0f; }, Always) == Outcome::Cornered &&
              now - 1000 == 6000,
          "cornered: no distance gained for the cornered time");
    End(flight, Outcome::Cornered);
    Check(!flight.active && flight.stood, "cornered: the bot stands");

    // Under a yard farther than ever is no gain; a yard is, and the time starts again
    flight = Begun(8.0f, 1000);
    Check(Judge(flight, 1, 8.9f, true, 2000, LIMITS) == Outcome::None && flight.noGainMs == 1000 &&
              flight.bestYards == 8.0f,
          "gain: under a yard farther than the best is none");
    Check(
        Judge(flight, 1, 9.0f, true, 3000, LIMITS) == Outcome::None && flight.noGainMs == 0 && flight.bestYards == 9.0f,
        "gain: a yard farther than the best starts the time again");
    Check(Judge(flight, 1, 5.0f, true, 4000, LIMITS) == Outcome::None && flight.bestYards == 9.0f,
          "gain: the best distance stays when the attacker closes in");

    // A pause of the AI (a stun in which no turn runs) counts as one turn, not as its length
    flight = Begun(8.0f, 1000);
    Check(Judge(flight, 1, 8.0f, true, 9000, LIMITS) == Outcome::None && flight.noGainMs == TURN_MS,
          "pause: 8 s without a turn count as one turn");
}

void TestRooted()
{
    // Rooted for 6 s with the attackers closing in to 2 yd: the time does not run, and the measure starts again at
    // the root's end from where the nearest attacker is then
    Flight flight = Begun(8.0f, 1000);
    std::uint32_t now = 1000;
    Check(Run(
              flight, now, 6000, [](std::uint32_t) { return 2.0f; }, [](std::uint32_t) { return false; }) ==
                  Outcome::None &&
              flight.noGainMs == 0 && flight.bestYards == 2.0f,
          "rooted: the time without a gain does not run, the best distance follows the attacker");
    Check(Run(
              flight, now, 30000, [](std::uint32_t ms) { return 2.0f + 7.0f * ms / 1000.0f; }, Always) ==
              Outcome::Escaped,
          "rooted: free again it gains from where the attacker stood and escapes");
    // A bot that can never move ends by the bound
    flight = Begun(8.0f, 1000);
    now = 1000;
    Check(Run(
              flight, now, 60000, [](std::uint32_t) { return 2.0f; }, [](std::uint32_t) { return false; }) ==
                  Outcome::Bound &&
              now - 1000 == 30000,
          "rooted: the bound still holds");
}

void TestBound()
{
    // A ranged pursuer that falls back a yard every 5 s: gains all the way, never the escape distance
    Flight flight = Begun(8.0f, 1000);
    std::uint32_t now = 1000;
    Check(Run(
              flight, now, 60000, [](std::uint32_t ms) { return 8.0f + ms / 5000.0f * 1.01f; }, Always) ==
                  Outcome::Bound &&
              now - 1000 == 30000,
          "bound: a flight that keeps gaining ends after the bound");
    End(flight, Outcome::Bound);
    Check(flight.stood, "bound: the bot stands");

    // Every flight ends within the bound, whatever the attacker's distance and the bot's roots do
    bool bounded = true;
    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        std::uint32_t state = seed * 2654435761u;
        auto const next = [&state]()
        {
            state = state * 1664525u + 1013904223u;
            return state >> 8;
        };
        Flight any = Begun(8.0f, 5000);
        std::uint32_t at = 5000;
        Outcome outcome = Outcome::None;
        while (outcome == Outcome::None && at - 5000 <= 30000 + 3000)
        {
            at += 100 + next() % 3000;
            outcome = Judge(any, 1 + next() % 3, static_cast<float>(next() % 39), next() % 4 != 0, at, LIMITS);
        }
        bounded = bounded && outcome != Outcome::None;
    }
    Check(bounded, "bound: 200 random flights all end by one turn after the bound");
}

void TestDue()
{
    Flight flight;
    Check(!Due(flight, false, true) && Due(flight, true, true), "due: on when the strategy wants it, in PvP combat");
    Check(!Due(flight, true, false), "due: no flight begins out of PvP combat");
    Begin(flight, 8.0f, 1000);
    Check(Due(flight, false, true) && Due(flight, false, false),
          "due: a flight under way stays on until its outcome, whatever the strategy wants");
    End(flight, Outcome::Cornered);
    Check(!Due(flight, true, true) && flight.stood, "due: a bot that stood does not flee again in that PvP combat");
    Check(!Due(flight, true, false) && !flight.stood && Due(flight, true, true),
          "due: out of PvP combat the stand is over, the next PvP combat may be fled");
    Check(Word(Outcome::Escaped)[0] == 'e' && Word(Outcome::Cornered)[0] == 'c' && Word(Outcome::Bound)[0] == 'b',
          "words: escaped, cornered, bound");
}
}  // namespace

int main()
{
    TestBegin();
    TestEscaped();
    TestCornered();
    TestRooted();
    TestBound();
    TestDue();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
