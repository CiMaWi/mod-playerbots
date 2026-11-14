/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the name matcher (LC2, BridgeNames.h): an exact name wins, then the nearest part of one; two of
// one name go to the nearer; case, spaces and punctuation play no part; a short name matches only exactly. The item
// and spell kinds are proven here alone, as the bag and spellbook candidates a row would hand it.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot
// tests/bridge-names-selftest.cpp
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "BridgeNames.h"

using PlayerbotBridge::NameCandidate;
using PlayerbotBridge::NameKey;
using PlayerbotBridge::NameMatch;
using PlayerbotBridge::NameResult;
using PlayerbotBridge::ResolveName;

namespace
{
int checks = 0;

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
    {
        std::printf("FAIL %s\n", label);
        std::exit(1);
    }
}

bool Is(NameResult result, NameMatch kind, std::size_t index) { return result.Kind == kind && result.Index == index; }
}  // namespace

int main()
{
    Check(NameKey("Thunder Bluff") == "thunderbluff" && NameKey("Lord Gregor's Hall-2") == "lordgregorshall2" &&
              NameKey(" ,'-").empty(),
          "a key is the letters and digits in lower case");

    // Units: a name, its guid, its distance from the bot.
    std::vector<NameCandidate> const units = {{"Defias Thug", 11, 30.0f}, {"Defias Thug Leader", 12, 2.0f},
                                              {"Defias Thug", 13, 10.0f}, {"Kobold Vermin", 14, 5.0f},
                                              {"Ox", 15, 1.0f},           {"Defias Thug", 16, 10.0f}};
    Check(Is(ResolveName("Kobold Vermin", units), NameMatch::Exact, 3), "an exact name");
    Check(Is(ResolveName("defias thug", units), NameMatch::Exact, 2),
          "an exact name beats a nearer part of one, and two of one name go to the nearer, the first of equals");
    Check(Is(ResolveName("DEFIAS-THUG", units), NameMatch::Exact, 2) &&
              Is(ResolveName("koboldvermin", units), NameMatch::Exact, 3),
          "case, spaces and punctuation play no part");
    Check(Is(ResolveName("leader", units), NameMatch::Nearest, 1), "a part of a name when no name is exact");
    Check(Is(ResolveName("thug", units), NameMatch::Nearest, 1), "the nearest of the names holding the part");
    Check(Is(ResolveName("the kobold vermin over there", units), NameMatch::Nearest, 3),
          "a name the wanted text holds is the nearest kind too");
    Check(ResolveName("Murloc", units).Kind == NameMatch::None, "no match is none");
    Check(ResolveName("", units).Kind == NameMatch::None && ResolveName(" '' ", units).Kind == NameMatch::None,
          "an empty name matches nothing");
    std::vector<NameCandidate> const blank = {{"", 21, 0.0f}, {"'", 22, 0.0f}};
    Check(ResolveName("", blank).Kind == NameMatch::None && ResolveName("'", blank).Kind == NameMatch::None,
          "an empty name never matches a nameless candidate");
    Check(ResolveName("Defias Thug", {}).Kind == NameMatch::None, "no candidate is none");
    Check(Is(ResolveName("ox", units), NameMatch::Exact, 4) && ResolveName("de", units).Kind == NameMatch::None &&
              ResolveName("box", units).Kind == NameMatch::None,
          "a name of fewer than three letters matches only exactly, and is never a part");

    // Items in the bags (Id the entry) and spells in the spellbook (Id the spell id): no distance.
    std::vector<NameCandidate> const items = {
        {"Linen Cloth", 2589, 0.0f}, {"Heavy Linen Bandage", 2581, 0.0f}, {"Linen Bandage", 1251, 0.0f}};
    Check(Is(ResolveName("linen bandage", items), NameMatch::Exact, 2), "an item by its exact name");
    Check(Is(ResolveName("bandage", items), NameMatch::Nearest, 1), "an item by a part: the first that holds it");
    std::vector<NameCandidate> const spells = {
        {"Flash Heal", 9474, 0.0f}, {"Heal", 6064, 0.0f}, {"Power Word: Fortitude", 10938, 0.0f}};
    Check(Is(ResolveName("heal", spells), NameMatch::Exact, 1), "a spell by its exact name beats one holding it");
    Check(Is(ResolveName("power word fortitude", spells), NameMatch::Exact, 2), "a spell's punctuation plays no part");
    Check(ResolveName("Smite", spells).Kind == NameMatch::None, "an unknown spell is none");

    std::printf("PASS %d checks\n", checks);
    return 0;
}
