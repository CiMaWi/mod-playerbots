/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of what a bot's state lists around it (living-conversation LC8, BridgeAround.h): nearest first, one
// entry per name with its count, at most the cap, ties in the candidates' order, never the bot's own name; and the
// danger word of a hostile unit.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot
// tests/bridge-around-selftest.cpp

#include <cstdio>
#include <string>
#include <vector>

#include "BridgeAround.h"

namespace
{
using namespace PlayerbotBridge;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

AroundCandidate Hostile(char const* name, float distance, char const* detail = "even")
{
    return {name, AroundKind::Hostile, detail, distance};
}

std::string Names(std::vector<AroundEntry> const& entries)
{
    std::string out;
    for (AroundEntry const& entry : entries)
        out += (out.empty() ? "" : ",") + entry.Name;
    return out;
}
}  // namespace

int main()
{
    // nearest first
    {
        std::vector<AroundEntry> const entries = SelectAround(
            {Hostile("Far", 30.0f), Hostile("Near", 2.0f), {"Innkeeper", AroundKind::Service, "innkeeper", 9.0f}},
            "Self");
        Check(Names(entries) == "Near,Innkeeper,Far", "nearest first");
        Check(entries.size() == 3 && entries[1].Kind == AroundKind::Service && entries[1].Detail == "innkeeper",
              "kind and detail kept");
    }

    // one entry per name with its count, at the nearest one's distance and detail
    {
        std::vector<AroundEntry> const entries =
            SelectAround({Hostile("Mangy Wolf", 12.0f, "trivial"), Hostile("Kobold", 5.0f),
                          Hostile("Mangy Wolf", 4.0f, "easy"), Hostile("Mangy Wolf", 20.0f, "trivial")},
                         "Self");
        Check(Names(entries) == "Mangy Wolf,Kobold", "three wolves are one entry");
        Check(entries.size() == 2 && entries[0].Count == 3 && entries[1].Count == 1, "the wolves count three");
        Check(entries.size() == 2 && entries[0].Distance == 4.0f && entries[0].Detail == "easy",
              "an entry keeps its nearest one");
    }

    // the cap: 30 names give the 12 nearest, a listed name still counts a candidate beyond the cap
    {
        std::vector<AroundCandidate> candidates;
        for (int index = 0; index < 30; ++index)
            candidates.push_back(Hostile(("Unit" + std::to_string(index)).c_str(), 100.0f - index));
        candidates.push_back(Hostile("Unit29", 200.0f));
        std::vector<AroundEntry> const entries = SelectAround(candidates, "Self");
        Check(entries.size() == MaxAround && MaxAround == 12, "cap 12 of 30");
        Check(!entries.empty() && entries.front().Name == "Unit29" && entries.back().Name == "Unit18",
              "the cap keeps the nearest");
        Check(!entries.empty() && entries.front().Count == 2, "a listed name counts past the cap");
        Check(SelectAround(candidates, "Self", 3).size() == 3, "a smaller cap");
    }

    // ties keep the candidates' order (the bot's victim, then its selection, both at distance 0)
    {
        std::vector<AroundEntry> const entries =
            SelectAround({Hostile("Victim", 0.0f), Hostile("Selection", 0.0f), Hostile("Other", 0.0f)}, "Self");
        Check(Names(entries) == "Victim,Selection,Other", "stable ties");
    }

    // never the bot's own name, never an empty name
    {
        std::vector<AroundEntry> const entries = SelectAround({{"Self", AroundKind::Person, "bot", 0.0f},
                                                               {"", AroundKind::Object, "fire", 1.0f},
                                                               {"Friend", AroundKind::Person, "bot", 3.0f}},
                                                              "Self");
        Check(Names(entries) == "Friend", "own and empty names left out");
    }

    Check(SelectAround({}, "Self").empty(), "empty input");

    // danger words: bot level 20, gray level 13
    Check(std::string(HostileDetail(20, 13, 13, 0)) == "trivial", "gray is trivial");
    Check(std::string(HostileDetail(20, 17, 13, 0)) == "easy", "three below is easy");
    Check(std::string(HostileDetail(20, 18, 13, 0)) == "even", "two below is even");
    Check(std::string(HostileDetail(20, 22, 13, 0)) == "even", "two above is even");
    Check(std::string(HostileDetail(20, 23, 13, 0)) == "hard", "three above is hard");
    Check(std::string(HostileDetail(20, 25, 13, 0)) == "deadly", "five above is deadly");
    Check(std::string(HostileDetail(20, 20, 13, RankElite)) == "elite", "an elite says so");
    Check(std::string(HostileDetail(20, 20, 13, RankRareElite)) == "elite", "a rare elite says so");
    Check(std::string(HostileDetail(20, 10, 13, RankElite)) == "trivial", "a gray elite is trivial");
    Check(std::string(HostileDetail(20, 63, 13, RankWorldBoss)) == "boss", "a world boss says so");
    Check(std::string(AroundKindWord(AroundKind::Task)) == "task" &&
              std::string(AroundKindWord(AroundKind::Person)) == "person",
          "kind words");

    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
