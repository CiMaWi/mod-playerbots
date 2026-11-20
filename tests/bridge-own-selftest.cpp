/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the mounts and recipes answers (living-conversation LC18c, LC18f, BridgeOwnRules.h): the riding word
// at the edges of its steps, the mounts line (the mount's name, sorted, one per name, cut at the limit, one line saying
// so when empty), the craft by its name with its recipes, the thing to make by the item's name, the best skill first,
// a craft the bot lacks answering none, no digit in any line.
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
    return {{"Blacksmithing", smith ? "journeyman" : ""},
            {"First Aid", aid ? "apprentice" : ""},
            {"Tailoring", tailor ? "expert" : ""},
            {"Alchemy", ""}};
}

std::vector<CraftFact> Alchemist() { return {{"Alchemy", "journeyman"}, {"Cooking", ""}}; }

// Three recipes of an alchemist (one of them a potion) and a cook's recipe the bot may not use
std::vector<BotRecipe> Recipes()
{
    return {{"Alchemy", "Minor Healing Potion", 0, 1, 1},
            {"Alchemy", "Truesilver Bar", 7, 0, 250},
            {"Alchemy", "Philosopher's Stone", 4, 0, 225},
            {"Cooking", "Boiled Clams", 0, 5, 1}};
}

std::vector<BotRecipe> Bandages()
{
    return {{"First Aid", "Linen Bandage", 0, 7, 1},
            {"First Aid", "Heavy Linen Bandage", 0, 7, 40},
            {"Blacksmithing", "Rough Sharpening Stone", 0, 0, 1}};
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
    RecipeItems("alchemy", Alchemist(), Recipes(), items);
    Check(items.size() == 4 && IsRow(items[0], L::ROW_OWN_CRAFT) && items[0].name == "Alchemy" &&
              Holds(items[0].text, "journeyman") && IsRow(items[1], L::ROW_OWN_RECIPE) &&
              items[1].name == "Truesilver Bar" && items[2].name == "Philosopher's Stone" &&
              items[3].name == "Minor Healing Potion" && Clean(items),
          "three recipes known: the craft and the three, highest skill first, none of another craft");
    Check(Holds(items[3].text, "Minor Healing Potion") && Holds(items[3].text, "Alchemy") &&
              Holds(items[3].text, L::OWN_MADE_WITH),
          "a recipe row names the item and the craft that makes it");
    RecipeItems("", Alchemist(), Recipes(), items);
    Check(items.size() == 4 && items[0].name == "Alchemy" && items[1].name == "Truesilver Bar",
          "no name: the crafts the bot has, then its recipes");
    RecipeItems(NameKey("healing potion"), Alchemist(), Recipes(), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_RECIPE) && items[0].name == "Minor Healing Potion",
          "an item named by part of its name: its recipe, no craft line");
    RecipeItems(NameKey("boiled clams"), Alchemist(), Recipes(), items);
    Check(
        items.size() == 1 && items[0].name == "Boiled Clams" && Holds(items[0].text, "Cooking"),
        "a recipe of a craft the bot has no rank in is still listed by its item (the reader keeps only crafts it has)");
    RecipeItems("alchemy", Alchemist(), {}, items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_CRAFT) && items[0].name == "Alchemy" && Clean(items),
          "none known: the craft alone, no recipe row");
    RecipeItems("cooking", Alchemist(), Recipes(), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE),
          "a craft the bot lacks: none, though a recipe lists it");
    RecipeItems("bandage", Crafts(false, true, false), Bandages(), items);
    Check(items.size() == 2 && items[0].name == "Heavy Linen Bandage" && items[1].name == "Linen Bandage",
          "the singular fits the plural; the best skill first");
    RecipeItems(NameKey("first aid"), Crafts(false, true, false), Bandages(), items);
    Check(items.size() == 3 && items[0].name == "First Aid" && items[1].name == "Heavy Linen Bandage" &&
              items[2].name == "Linen Bandage",
          "a craft by its name: it and its own recipes only");
    RecipeItems("sword", Crafts(true, true, true), Bandages(), items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE), "a thing no recipe makes: none");
    RecipeItems("", {}, {}, items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE), "no craft facts at all: none");
    RecipeItems("tailoring", Crafts(false, false, false), {}, items);
    Check(items.size() == 1 && IsRow(items[0], L::ROW_OWN_NONE) && Clean(items), "a bot without crafts: one none line");
    std::vector<BotRecipe> many;
    for (std::uint32_t skill = 1; skill <= 10; ++skill)
        many.push_back({"Alchemy", std::string("Potion ") + static_cast<char>('A' + skill), 0, 1, skill});
    many.push_back({"Alchemy", "Potion J", 0, 1, 99});
    RecipeItems("alchemy", Alchemist(), many, items);
    Check(
        items.size() == 4 && items[1].name == "Potion J" && items[2].name == "Potion K" && items[3].name == "Potion I",
        "cut at four lines, an item once");
    bool words = true;
    for (char const* word : {L::ROW_OWN_RIDING, L::ROW_OWN_MOUNT, L::ROW_OWN_CRAFT, L::ROW_OWN_RECIPE, L::ROW_OWN_NONE})
        for (char const* c = word; *c; ++c)
            words = words && ((*c >= 'a' && *c <= 'z') || *c == '_');
    Check(words, "row words are lower letters and underscores");
    Check(!HasDigit(L::OWN_NO_MOUNT) && !HasDigit(L::OWN_NO_RIDING) && !HasDigit(L::OWN_NO_RECIPE) &&
              !HasDigit(L::OWN_MOUNT_LINE) && !HasDigit(L::OWN_MADE_WITH),
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
