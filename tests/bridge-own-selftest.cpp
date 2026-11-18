/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the mounts and recipes answers (living-conversation LC18c, BridgeOwnRules.h): the riding word at the
// edges of its steps, the mounts line (the mount's name, sorted, one per name, cut at the limit, one line saying so
// when empty), the craft by its name, the thing to make by what a craft is good for, a craft the bot lacks answering
// none, no digit in any line.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot /I src/Ai/World/Rpg
// tests/bridge-own-selftest.cpp

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeOwnRules.h"

namespace
{
using namespace PlayerbotBridge;
using namespace PlayerbotBridge::Own;
namespace L = BridgeTexts::Lookups;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

bool HasDigit(std::string_view text) { return text.find_first_of("0123456789") != std::string_view::npos; }

bool Holds(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

bool Clean(std::vector<BotStateItem> const& items)
{
    for (BotStateItem const& item : items)
        if (item.name.empty() || item.text.empty() || HasDigit(item.text) || HasDigit(item.name))
            return false;
    return true;
}

std::vector<CraftFact> Crafts(bool smith, bool aid, bool tailor)
{
    return {{"Blacksmithing", "weapons, armour and sharpening stones", smith ? "journeyman" : ""},
            {"First Aid", "bandages", aid ? "apprentice" : ""},
            {"Tailoring", "cloth armour, bags and bandages", tailor ? "expert" : ""},
            {"Alchemy", "potions, elixirs and flasks", ""}};
}

bool IsRow(BotStateItem const& item, char const* word) { return std::string_view(item.word) == word; }

void RidingChecks()
{
    Check(Own::RidingWord(1) == "apprentice" && Own::RidingWord(75) == "apprentice", "riding 1 to 75 is apprentice");
    Check(Own::RidingWord(76) == "journeyman" && Own::RidingWord(150) == "journeyman",
          "riding 76 to 150 is journeyman");
    Check(Own::RidingWord(151) == "expert" && Own::RidingWord(225) == "expert", "riding 151 to 225 is expert");
    Check(Own::RidingWord(226) == "artisan" && Own::RidingWord(300) == "artisan" && Own::RidingWord(450) == "artisan",
          "riding 226 up is artisan");
}

void MountChecks()
{
    std::vector<BotStateItem> items;
    MountItems(75, {"Brown Horse", "Black Wolf", "Brown Horse"}, items);
    Check(items.size() == 3 && IsRow(items[0], L::ROW_OWN_RIDING) && Holds(items[0].text, "apprentice"),
          "riding line first, with its rank word");
    Check(items[1].name == "Black Wolf" && items[2].name == "Brown Horse" && IsRow(items[2], L::ROW_OWN_MOUNT) &&
              Holds(items[2].text, "Brown Horse"),
          "mounts by name, one line per name, each naming its mount");
    Check(Clean(items), "mount lines have a name and no digit");
    MountItems(0, {"Brown Horse"}, items);
    Check(items.size() == 1 && items[0].name == "Brown Horse", "a mount without riding: no riding line");
    MountItems(150, {}, items);
    Check(items.size() == 2 && IsRow(items[1], L::ROW_OWN_NONE) && Holds(items[1].text, "no mount"),
          "riding without a mount: the riding line and a none line");
    MountItems(0, {}, items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE) && Clean(items) && Holds(items[0].text, "cannot ride"),
          "no riding and no mount: one none line that says so");
    std::vector<std::string> many;
    for (char c = 'A'; c < 'A' + 12; ++c)
        many.push_back(std::string("Mount ") + c);
    MountItems(300, many, items);
    Check(items.size() == 6, "the answer is cut at the limit of the topic row (six)");
}

void RecipeChecks()
{
    std::vector<BotStateItem> items;
    RecipeItems("blacksmithing", Crafts(true, false, false), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_CRAFT) && items[0].name == "Blacksmithing" &&
              Holds(items[0].text, "journeyman") && Holds(items[0].text, "weapons") && Clean(items),
          "a craft the bot has: its name, rank and what it makes");
    RecipeItems("alchemy", Crafts(true, true, true), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE) && !Holds(items[0].text, "lchemy"),
          "a craft the bot lacks: one none line, no name");
    RecipeItems("bandages", Crafts(false, true, true), items);
    Check(items.size() == 2 && IsRow(items[0], L::ROW_OWN_RECIPE) && items[0].name == "First Aid" &&
              items[1].name == "Tailoring",
          "a thing to make: the crafts of the bot that are good for it");
    RecipeItems("bandage", Crafts(false, true, false), items);
    Check(items.size() == 1 && items[0].name == "First Aid", "the singular fits the plural");
    RecipeItems("bandages", Crafts(true, false, false), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE), "a thing only a craft the bot lacks makes: none");
    RecipeItems("potions", Crafts(true, true, true), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE), "a craft of another kind: none");
    RecipeItems("and", Crafts(true, true, true), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE), "a joining word makes nothing");
    RecipeItems("sword", Crafts(true, true, true), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE), "a thing no craft is good for: none");
    RecipeItems("", Crafts(true, true, false), items);
    Check(items.size() == 2 && items[0].name == "Blacksmithing" && items[1].name == "First Aid" && Clean(items),
          "no name: the crafts the bot has");
    RecipeItems("tailoring", Crafts(false, false, false), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE) && Clean(items), "a bot without crafts: one none line");
    RecipeItems("", {}, items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE), "no craft facts at all: none");
    bool words = true;
    for (char const* word : {L::ROW_OWN_RIDING, L::ROW_OWN_MOUNT, L::ROW_OWN_CRAFT, L::ROW_OWN_RECIPE, L::ROW_OWN_NONE})
        for (char const* c = word; *c; ++c)
            words = words && ((*c >= 'a' && *c <= 'z') || *c == '_');
    Check(words, "row words are lower letters and underscores");
    Check(!HasDigit(L::OWN_NO_MOUNT) && !HasDigit(L::OWN_NO_RIDING) && !HasDigit(L::OWN_NO_RECIPE) &&
              !HasDigit(L::OWN_MOUNT_LINE) && !HasDigit(L::OWN_GOOD_FOR),
          "no fixed text holds a digit");
}
}  // namespace

int main()
{
    RidingChecks();
    MountChecks();
    RecipeChecks();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
