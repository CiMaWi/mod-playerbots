/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the room before a hand-in (src/Ai/World/Rpg/HandInRoomRules.h, questing WQ43): the slots a
// reward takes, and over generated bags the invariant: afterwards the free slots cover the need, or nothing is
// destroyed when the stacks that may go cannot cover it; no kept stack is chosen; the cheapest go first; nothing is
// destroyed when room suffices.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "HandInRoomRules.h"

namespace Room = Planner::HandInRoom;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
    {
        ++passed;
        return;
    }
    ++failed;
    std::cout << "FAIL: " << what << "\n";
}

void CheckSlots()
{
    Check(Room::SlotsFor(1, 1, 0) == 1, "an unstackable reward takes a slot");
    Check(Room::SlotsFor(1, 1, 3) == 1, "an unstackable reward takes a slot though the bot carries the item");
    Check(Room::SlotsFor(5, 20, 0) == 1, "five units of a 20-stack take one slot");
    Check(Room::SlotsFor(5, 20, 7) == 0, "five units go onto a carried stack of 7 of 20");
    Check(Room::SlotsFor(5, 20, 17) == 1, "five units overflow a carried stack of 17 of 20 into one slot");
    Check(Room::SlotsFor(5, 20, 20) == 1, "a full carried stack holds none");
    Check(Room::SlotsFor(45, 20, 0) == 3, "45 units of a 20-stack take three slots");
    Check(Room::SlotsFor(0, 20, 0) == 0, "no units take no slot");
    Check(Room::SlotsFor(2, 0, 0) == 2, "a stack size of 0 counts as 1");
    Check(Room::SlotsShort(2, 0) == 2 && Room::SlotsShort(2, 1) == 1 && Room::SlotsShort(2, 2) == 0 &&
              Room::SlotsShort(1, 5) == 0,
          "the slots short are those the free slots do not cover");
}

void CheckExamples()
{
    std::vector<Room::Stack> const bags = {{28, false}, {500, false}, {0, true}, {10, false}, {10, false}};
    Check(Room::Victims(bags, 0).empty(), "nothing short: nothing destroyed");
    std::vector<std::size_t> const one = Room::Victims(bags, 1);
    Check(one == std::vector<std::size_t>{3}, "one short: the cheapest stack, of equal ones the earlier");
    std::vector<std::size_t> const three = Room::Victims(bags, 3);
    Check(three == std::vector<std::size_t>{3, 4, 0}, "three short: the three cheapest, cheapest first");
    Check(Room::Victims(bags, 5).empty(), "more short than stacks that may go: nothing destroyed");
    std::vector<Room::Stack> const kept = {{0, true}, {0, true}};
    Check(Room::Victims(kept, 1).empty(), "kept stacks only: nothing destroyed (mutant: kept ignored)");
}

// The invariant over generated bags
void CheckGenerated()
{
    std::mt19937 rng(43);
    int bad = 0;
    for (int round = 0; round < 20000; ++round)
    {
        std::size_t const n = rng() % 40;
        std::vector<Room::Stack> bags(n);
        std::size_t mayGo = 0;
        for (Room::Stack& stack : bags)
        {
            stack.value = rng() % 6 == 0 ? 0 : rng() % 1000;
            stack.kept = rng() % 4 == 0;
            mayGo += stack.kept ? 0 : 1;
        }
        std::uint32_t const needed = rng() % 6;
        std::uint32_t const free = rng() % 4;
        std::uint32_t const slotsShort = Room::SlotsShort(needed, free);
        std::vector<std::size_t> const victims = Room::Victims(bags, slotsShort);
        bool ok = true;
        if (!slotsShort)
            ok = victims.empty();
        else if (mayGo < slotsShort)
            ok = victims.empty();
        else
            ok = victims.size() == slotsShort && free + victims.size() >= needed;
        std::vector<bool> chosen(n, false);
        for (std::size_t i = 0; i < victims.size(); ++i)
        {
            ok = ok && victims[i] < n && !bags[victims[i]].kept && !chosen[victims[i]];
            if (victims[i] < n)
                chosen[victims[i]] = true;
            ok = ok && (i == 0 || bags[victims[i - 1]].value <= bags[victims[i]].value);
        }
        if (!victims.empty())
        {
            std::uint64_t const dearest = bags[victims.back()].value;
            for (std::size_t i = 0; i < n; ++i)
                ok = ok && (chosen[i] || bags[i].kept || bags[i].value >= dearest);
        }
        bad += ok ? 0 : 1;
    }
    Check(bad == 0,
          "generated bags: the victims cover the need or are none when they cannot, no kept stack, the "
          "cheapest first, none when room suffices (" +
              std::to_string(bad) + " of 20000 broke it)");
}
}  // namespace

int main()
{
    CheckSlots();
    CheckExamples();
    CheckGenerated();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
