/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of what a bot knows of people, items, quests and lands and of the who, what and zone answers
// (living-conversation LC18b, BridgeLookupRules.h, BridgeNameIndex.h): the level bands and words at their boundaries,
// the side rules, the known-thing rule against a hand-written table, the wording (the name in a known line, no digit,
// one line and no name for an unknown thing), and the name index's exact key.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot /I src/Ai/World/Rpg
// tests/bridge-lookup-selftest.cpp

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeLookupRules.h"
#include "BridgeNameIndex.h"

namespace
{
using namespace PlayerbotBridge;
using namespace PlayerbotBridge::Knowledge;
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

BotKnowledge Bot(Side side, std::uint32_t level)
{
    BotKnowledge bot;
    bot.side = side;
    bot.level = level;
    return bot;
}

void BandChecks()
{
    Check(LevelBand(0) == 0 && LevelBand(1) == 0 && LevelBand(60) == 0, "levels 0 to 60 are the first band");
    Check(LevelBand(61) == 1 && LevelBand(70) == 1, "levels 61 to 70 are the second band");
    Check(LevelBand(71) == 2 && LevelBand(80) == 2 && LevelBand(255) == 2, "levels 71 and up are the third band");
    Check(std::string_view(LevelWord(1)) == L::LEVEL_WORDS[0] && std::string_view(LevelWord(20)) == L::LEVEL_WORDS[0],
          "level words: 1 and 20 are the low levels");
    Check(std::string_view(LevelWord(21)) == L::LEVEL_WORDS[1] && std::string_view(LevelWord(40)) == L::LEVEL_WORDS[1],
          "level words: 21 and 40 are the middle levels");
    Check(std::string_view(LevelWord(41)) == L::LEVEL_WORDS[2] && std::string_view(LevelWord(60)) == L::LEVEL_WORDS[2],
          "level words: 41 and 60 are the high levels");
    Check(std::string_view(LevelWord(61)) == L::LEVEL_WORDS[3] && std::string_view(LevelWord(70)) == L::LEVEL_WORDS[3],
          "level words: 61 and 70 are Outland's");
    Check(std::string_view(LevelWord(71)) == L::LEVEL_WORDS[4] &&
              std::string_view(LevelWord(80)) == L::LEVEL_WORDS[4] &&
              std::string_view(LevelWord(200)) == L::LEVEL_WORDS[4],
          "level words: 71 and up are Northrend's");
    bool digits = false;
    for (char const* word : L::LEVEL_WORDS)
        digits = digits || HasDigit(word);
    Check(!digits, "no level word holds a digit");
}

void SideChecks()
{
    Check(SideOfRaces(1101) == Side::Alliance && SideOfRaces(690) == Side::Horde, "race masks of one side");
    Check(SideOfRaces(0xFFFFFFFFu) == Side::Neither && SideOfRaces(0) == Side::Neither &&
              SideOfRaces(1101 | 690) == Side::Neither,
          "no race mask, all races or both sides: neither");
    Check(SideOfRaces(1) == Side::Alliance && SideOfRaces(2) == Side::Horde, "a single race is its side's");
    Check(SideOfFactionGroup(2) == Side::Alliance && SideOfFactionGroup(4) == Side::Horde &&
              SideOfFactionGroup(6) == Side::Neither && SideOfFactionGroup(8) == Side::Neither,
          "faction groups: Alliance, Horde, both, a monster's");
}

// The known-thing rule against a plain table: every bot level, thing level, side pair and has flag
void KnownChecks()
{
    int tried = 0;
    int wrong = 0;
    for (std::uint32_t botLevel : {1u, 12u, 60u, 61u, 70u, 71u, 80u})
        for (std::uint32_t thingLevel : {0u, 1u, 60u, 61u, 70u, 71u, 80u})
            for (Side botSide : {Side::Alliance, Side::Horde})
                for (Side thingSide : {Side::Neither, Side::Alliance, Side::Horde})
                    for (bool has : {false, true})
                    {
                        int const botBand = botLevel <= 60 ? 0 : botLevel <= 70 ? 1 : 2;
                        int const thingBand = thingLevel <= 60 ? 0 : thingLevel <= 70 ? 1 : 2;
                        bool const sameSide = thingSide == Side::Neither || thingSide == botSide;
                        bool const expected = has || (sameSide && thingBand <= botBand);
                        ++tried;
                        if (ThingKnown(Bot(botSide, botLevel), {thingSide, thingLevel}, has) != expected)
                            ++wrong;
                    }
    std::printf("known-thing pairs tried: %d, wrong: %d\n", tried, wrong);
    Check(wrong == 0, "the known-thing rule matches its table for every pair");
}

BotKnowledge const ALLY12 = Bot(Side::Alliance, 12);

NpcFact Npc(char const* name, Side side, std::uint32_t level, bool seen = false)
{
    NpcFact npc;
    npc.name = name;
    npc.side = side;
    npc.level = level;
    npc.seen = seen;
    return npc;
}

void WhoChecks()
{
    std::vector<BotStateItem> items;
    NpcFact dughan = Npc("Marshal Dughan", Side::Alliance, 25);
    dughan.what = "quest giver";
    dughan.place = "Goldshire";
    dughan.fact.zoneId = 12;
    dughan.fact.side = Side::Alliance;
    dughan.fact.lowLevel = 1;
    WhoItems(ALLY12, {}, {dughan}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_NPC && Holds(items[0].text, "Marshal Dughan") &&
              Holds(items[0].text, "quest giver") && Holds(items[0].text, L::WHO_SIDES[1]) &&
              Holds(items[0].text, "Goldshire") && !HasDigit(items[0].text),
          "an NPC of the bot's side: name, service, side and a known place, no digit");
    NpcFact hidden = dughan;
    hidden.fact.lowLevel = 40;
    WhoItems(ALLY12, {}, {hidden}, items);
    Check(items.size() == 1 && Holds(items[0].text, "Marshal Dughan") && !Holds(items[0].text, "Goldshire"),
          "a place the bot does not know is left out of the line");
    WhoItems(ALLY12, {}, {Npc("Gryshka", Side::Horde, 10)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN && items[0].text == L::WHO_UNKNOWN &&
              !Holds(items[0].text, "Gryshka") && items[0].name == items[0].word,
          "a person of the other side: one unknown line, no name");
    WhoItems(ALLY12, {}, {Npc("Thane", Side::Alliance, 70)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN, "a person beyond the level band is unknown");
    WhoItems(ALLY12, {}, {Npc("Thane", Side::Horde, 70, true)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_NPC,
          "a person the bot sees is known beyond band and side");
    WhoItems(ALLY12, {}, {}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN, "nobody of the name: the unknown line");
    std::vector<BotStateItem> bots = {LookupLine(LookupRow::Bot, "Abc", "Abc, a priest, took part in a kill.")};
    WhoItems(ALLY12, bots, {Npc("Abc", Side::Alliance, 10)}, items);
    Check(items.size() == 2 && items[0].word == L::ROW_LOOKUP_BOT && items[1].word == L::ROW_LOOKUP_NPC,
          "a known bot's line comes first");
    WhoItems(ALLY12, {}, {Npc("Guard", Side::Alliance, 5), Npc("Guard", Side::Alliance, 5)}, items);
    Check(items.size() == 1, "two people of one name make one line");
    std::vector<NpcFact> many;
    for (char const* name : {"Anna", "Bert", "Cora", "Dirk", "Edda"})
        many.push_back(Npc(name, Side::Alliance, 5));
    WhoItems(ALLY12, {}, many, items);
    Check(items.size() == 3 && LOOKUP_SHOWN == 3, "an answer holds at most three lines");
}

ItemFact Item(char const* name, std::uint32_t level, Side side, bool carried)
{
    ItemFact item;
    item.name = name;
    item.quality = 2;
    item.itemClass = 4;
    item.level = level;
    item.side = side;
    item.carried = carried;
    return item;
}

QuestFact Quest(char const* title, std::uint32_t level, Side side, QuestState state)
{
    QuestFact quest;
    quest.title = title;
    quest.level = level;
    quest.side = side;
    quest.state = state;
    return quest;
}

void WhatChecks()
{
    std::vector<BotStateItem> items;
    WhatItems(ALLY12, {Item("Deep Woodlands Cloak", 51, Side::Neither, true)}, {}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_ITEM && Holds(items[0].text, "Deep Woodlands Cloak") &&
              Holds(items[0].text, "uncommon armor") && Holds(items[0].text, L::LEVEL_WORDS[2]) &&
              Holds(items[0].text, "to wear") && Holds(items[0].text, L::LOOKUP_CARRY) && !HasDigit(items[0].text),
          "a carried item: name, quality, kind, level and use in words, that it is carried, no digit");
    WhatItems(ALLY12, {Item("Linen Cloth", 5, Side::Neither, false)}, {}, items);
    Check(items.size() == 1 && Holds(items[0].text, L::LOOKUP_NO_CARRY), "a known item not carried says so");
    WhatItems(ALLY12, {Item("Saronite Bar", 80, Side::Neither, false)}, {}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN && items[0].text == L::WHAT_UNKNOWN &&
              !Holds(items[0].text, "Saronite") && items[0].name == items[0].word,
          "an item beyond the level band: one unknown line, no name");
    WhatItems(ALLY12, {Item("Saronite Bar", 80, Side::Neither, true)}, {}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_ITEM, "a carried item is known beyond the band");
    WhatItems(ALLY12, {Item("Tabard of the Horde", 1, Side::Horde, false)}, {}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN, "an item of the other side is unknown");
    QuestFact fargodeep = Quest("The Fargodeep Mine", 7, Side::Alliance, QuestState::InLog);
    fargodeep.giver = "Marshal Dughan";
    fargodeep.place = "Goldshire";
    fargodeep.fact.zoneId = 12;
    fargodeep.fact.side = Side::Alliance;
    fargodeep.fact.lowLevel = 1;
    WhatItems(ALLY12, {}, {fargodeep}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_QUEST && Holds(items[0].text, "The Fargodeep Mine") &&
              Holds(items[0].text, L::LEVEL_WORDS[0]) && Holds(items[0].text, "Marshal Dughan") &&
              Holds(items[0].text, "Goldshire") && Holds(items[0].text, L::LOOKUP_HAVE) && !HasDigit(items[0].text),
          "a quest in the log: title, level in words, giver, place, that the bot has it, no digit");
    fargodeep.state = QuestState::Done;
    WhatItems(ALLY12, {}, {fargodeep}, items);
    Check(Holds(items[0].text, L::LOOKUP_DONE), "a quest done says so");
    fargodeep.state = QuestState::None;
    WhatItems(ALLY12, {}, {fargodeep}, items);
    Check(Holds(items[0].text, L::LOOKUP_NO_HAVE), "a known quest the bot does not have says so");
    WhatItems(ALLY12, {}, {Quest("Wrath of the Lich", 75, Side::Neither, QuestState::None)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN && !Holds(items[0].text, "Lich"),
          "a quest beyond the level band: one unknown line, no name");
    WhatItems(ALLY12, {}, {Quest("Wrath of the Lich", 75, Side::Neither, QuestState::InLog)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_QUEST, "a quest in the log is known beyond the band");
}

ZoneFact Land(char const* name, Side side, std::uint32_t low, std::uint32_t high, std::uint32_t id)
{
    ZoneFact zone;
    zone.name = name;
    zone.side = side;
    zone.low = low;
    zone.high = high;
    zone.fact.zoneId = id;
    zone.fact.side = side;
    zone.fact.lowLevel = low;
    return zone;
}

void ZoneChecks()
{
    std::vector<BotStateItem> items;
    ZoneFact elwynn = Land("Elwynn Forest", Side::Alliance, 1, 10, 12);
    TownFact goldshire{"Goldshire", elwynn.fact};
    TownFact lakeshire{"Lakeshire", Land("Redridge", Side::Alliance, 15, 25, 44).fact};
    TownFact hidden{"Eastvale", Land("Elwynn", Side::Alliance, 30, 40, 12).fact};
    hidden.fact.zoneId = 99;
    elwynn.towns = {goldshire, lakeshire, hidden};
    ZoneItems(ALLY12, {elwynn}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_ZONE && Holds(items[0].text, "Elwynn Forest") &&
              Holds(items[0].text, L::ZONE_LANDS[1]) && Holds(items[0].text, L::LEVEL_WORDS[0]) &&
              Holds(items[0].text, "Goldshire") && !Holds(items[0].text, "Eastvale") && !HasDigit(items[0].text),
          "an own land: name, whose, level in words, the towns the bot knows only");
    Check(!Holds(items[0].text, "Lakeshire"), "a town above the bot's level is left out");
    ZoneItems(ALLY12, {Land("Dragonblight", Side::Neither, 71, 80, 65)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN && items[0].text == L::ZONE_UNKNOWN &&
              !Holds(items[0].text, "Dragonblight") && items[0].name == items[0].word,
          "a land beyond the bot's level: one unknown line, no name");
    ZoneItems(ALLY12, {Land("Durotar", Side::Horde, 1, 10, 14)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN, "a land of the other side is unknown");
    BotKnowledge stayed = ALLY12;
    stayed.stayedZones.push_back(65);
    ZoneItems(stayed, {Land("Dragonblight", Side::Neither, 71, 80, 65)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_ZONE && Holds(items[0].text, L::LEVEL_WORDS[4]),
          "a land the bot stayed in is known: the Northrend level words");
    ZoneItems(ALLY12, {Land("Stormwind City", Side::Alliance, 0, 0, 1519)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_ZONE && !Holds(items[0].text, "for the"),
          "a capital without a level range: known, no range part");
    LookupPending(items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_PENDING && !HasDigit(items[0].text),
          "the not-ready line is one line without a digit");
}

void WordChecks()
{
    bool digits = false;
    for (char const* text :
         {L::WHO_SENTENCE, L::WHO_NAME_SENTENCE, L::WHAT_SENTENCE, L::WHAT_NAME_SENTENCE, L::ZONE_SENTENCE,
          L::ZONE_NAME_SENTENCE, L::WHO_UNKNOWN, L::WHAT_UNKNOWN, L::ZONE_UNKNOWN, L::LOOKUP_PENDING})
        digits = digits || HasDigit(text);
    for (auto const& kind : L::ITEM_KINDS)
        digits = digits || HasDigit(kind.Kind) || HasDigit(kind.Use);
    Check(!digits, "no sentence or word of the topics holds a digit");
    bool rowsValid = true;
    for (LookupRow row : {LookupRow::Npc, LookupRow::Bot, LookupRow::Item, LookupRow::Quest, LookupRow::Zone,
                          LookupRow::Unknown, LookupRow::Pending})
        for (char const* c = LookupWord(row); *c; ++c)
            rowsValid = rowsValid && ((*c >= 'a' && *c <= 'z') || *c == '_');
    Check(rowsValid, "the row words are [a-z_]");
    Check(L::ITEM_KINDS.size() == 18, "an item kind per class and one spare");
}

void IndexChecks()
{
    NameKeyIndex index;
    index.Add("Linen Cloth", 2589);
    index.Add("Linen Cloth", 2000);
    index.Add("Frostmourne", 33475);
    index.Add("Frostmourne", 36942);
    index.Add("", 5);
    index.Add("Zero", 0);
    Check(index.Size() == 2, "an empty name or id adds nothing; one entry per key");
    Check(index.Find("linencloth") == 2000 && index.Find("Linen  Cloth!") == 2000, "an exact key finds the lowest id");
    Check(index.Find("Linen") == 0 && index.Find("Cloth") == 0 && index.Find("Linen Cloth Bolt") == 0,
          "a part of a name, or a longer name, finds nothing");
    Check(index.Find("") == 0 && index.Find("Nothing") == 0, "an empty or unknown name finds nothing");
    Check(index.EstimatedBytes() > 0, "the estimate counts bytes");
}
}  // namespace

int main()
{
    BandChecks();
    SideChecks();
    KnownChecks();
    WhoChecks();
    WhatChecks();
    ZoneChecks();
    WordChecks();
    IndexChecks();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
