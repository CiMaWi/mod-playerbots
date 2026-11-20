/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of what a bot knows of people, items, quests and lands and of the who, what and zone answers
// (living-conversation LC18b, BridgeLookupRules.h, BridgeNameIndex.h): the level bands and words at their boundaries,
// the side rules, the known-thing rule against a hand-written table, the wording (the name in a known line, no digit,
// one line and no name for an unknown thing), and the name index's exact key. LC18g (LC.54): the places' own sides
// (from the game's reactions, a town's by vote), the service list of a contested zone with an inn of each side, and a
// mutant that offers a hostile innkeeper.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot /I src/Ai/World/Rpg
// tests/bridge-lookup-selftest.cpp

#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
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
    // Real rows of the world DB: 2839 "A Letter to Yvette" (Flags2 1, AllowableRace -1: Horde by its flag only),
    // 748 "Poison Water" (AllowableRaces 0, giver Mull Thunderhorn of faction 104, ourMask 4: Horde by its giver)
    Check(SideOfItem(1, 0xFFFFFFFFu) == Side::Horde && SideOfItem(2, 0) == Side::Alliance,
          "an item's faction flag names its side though it has no race mask");
    Check(SideOfItem(3, 0) == Side::Neither && SideOfItem(0, 0) == Side::Neither && SideOfItem(0, 690) == Side::Horde &&
              SideOfItem(3, 1101) == Side::Alliance,
          "both or no faction flags: the race mask decides");
    Check(
        SideOfQuest(SideOfFactionGroup(4), 0) == Side::Horde && SideOfQuest(SideOfFactionGroup(2), 0) == Side::Alliance,
        "a quest's giver names its side though it has no race mask");
    Check(SideOfQuest(Side::Neither, 690) == Side::Horde && SideOfQuest(Side::Neither, 0) == Side::Neither &&
              SideOfQuest(SideOfFactionGroup(6), 1101) == Side::Alliance,
          "a giver of no side: the race mask decides");
}

using PS = Planner::PlaceSide;

// Whether a hit offers the bot a service hostile to it: one of the other side or of none (the invariant the console
// dump `playerbots bridge sides` checks over the whole index)
bool OffersHostile(Planner::PlannerIndexes const& index, Side side, std::vector<Planner::Nearby> const& hits)
{
    for (Planner::Nearby const& hit : hits)
    {
        PS const service = index.services[hit.index].side;
        if (service == PS::None || service == (side == Side::Alliance ? PS::Horde : PS::Alliance))
            return true;
    }
    return false;
}

// The mutant: the nearest services whatever their side (LC18a's ReadWhere before LC18g)
std::vector<Planner::Nearby> NearestAnySide(Planner::PlannerIndexes const& index, Planner::ServiceKind kind,
                                            MapPoint const& from, std::size_t k)
{
    return index.Nearest(kind, from.mapId, from.x, from.y, from.z, k);
}

// The `where inn` lines a bot gets from the hits, as the reader builds them
std::vector<BotStateItem> InnLines(Planner::PlannerIndexes const& index, BotKnowledge const& bot, MapPoint const& from,
                                   std::vector<Planner::Nearby> const& hits)
{
    std::vector<WhereCandidate> things;
    for (Planner::Nearby const& hit : hits)
    {
        Planner::Service const& service = index.services[hit.index];
        things.push_back({"Innkeeper " + std::to_string(service.entry),
                          ServiceWhat(Planner::ServiceKind::Innkeeper),
                          "Ashenvale",
                          {service.mapId, service.x, service.y, service.z},
                          PlaceOfService(index, service),
                          service.side});
    }
    std::vector<BotStateItem> items;
    WhereItems(bot, from, std::move(things), items);
    return items;
}

void PlaceSideChecks()
{
    using Planner::SideOfReactions;
    Check(SideOfReactions(true, true, false) == PS::Horde && SideOfReactions(true, false, true) == PS::Alliance &&
              SideOfReactions(true, false, false) == PS::Neutral && SideOfReactions(true, true, true) == PS::None &&
              SideOfReactions(false, false, false) == PS::None,
          "a service's side: hostile to one team is the other's, to neither neutral, to both or no answer none");
    Check(Planner::Serves(PS::Neutral, PS::Alliance) && Planner::Serves(PS::Neutral, PS::Horde) &&
              Planner::Serves(PS::Alliance, PS::Alliance) && !Planner::Serves(PS::Alliance, PS::Horde) &&
              Planner::Serves(PS::Horde, PS::Horde) && !Planner::Serves(PS::Horde, PS::Alliance) &&
              !Planner::Serves(PS::None, PS::Alliance) && !Planner::Serves(PS::None, PS::Horde),
          "a side is served by its own side's and the neutral services only");
    Check(Planner::VoteSide({0, 1, 4, 0}) == PS::Alliance && Planner::VoteSide({0, 0, 0, 3}) == PS::Horde &&
              Planner::VoteSide({0, 5, 1, 0}) == PS::Neutral && Planner::VoteSide({0, 0, 2, 2}) == PS::Neutral &&
              Planner::VoteSide({0, 2, 0, 2}) == PS::Neutral && Planner::VoteSide({5, 0, 0, 1}) == PS::Horde &&
              Planner::VoteSide({5, 0, 0, 0}) == PS::None && Planner::VoteSide({0, 0, 0, 0}) == PS::None,
          "a town's side: its members' most common side, a tie neutral, no member with an answer none");
    Check(TeamOf(Side::Alliance) == PS::Alliance && TeamOf(Side::Horde) == PS::Horde, "a bot's side as a team");

    // A contested zone (Ashenvale, levels from 18): an Alliance town at x 0, a Horde camp at x 3000, a lone Horde
    // innkeeper at x 1500 and a lone one hostile to both at x 1600; the bots ask at x 2000, the Horde ones nearest
    Planner::GatherInput input;
    input.zones.push_back({331, 18, 30, Planner::ZoneSide::Contested, 1});
    std::uint32_t entry = 100;
    auto add = [&](float x, float y, std::uint32_t flags, bool hostileToAlliance, bool hostileToHorde, bool mailbox)
    {
        input.spawns.push_back(
            {1, x, y, 10.0f, ++entry, flags, 0, 331, 900, 0, mailbox, !mailbox, hostileToAlliance, hostileToHorde});
    };
    for (auto const& [x, hostileToAlliance, hostileToHorde] :
         {std::tuple<float, bool, bool>{0.0f, false, true}, std::tuple<float, bool, bool>{3000.0f, true, false}})
    {
        add(x, 0.0f, Planner::NpcFlag::INNKEEPER, hostileToAlliance, hostileToHorde, false);
        add(x + 10.0f, 0.0f, Planner::NpcFlag::VENDOR, hostileToAlliance, hostileToHorde, false);
        add(x, 10.0f, Planner::NpcFlag::REPAIR, hostileToAlliance, hostileToHorde, false);
        add(x - 10.0f, 0.0f, Planner::NpcFlag::FLIGHTMASTER, false, false, false);  // a neutral one among them
        add(x, -10.0f, 0, false, false, true);
    }
    add(1500.0f, 0.0f, Planner::NpcFlag::INNKEEPER, true, false, false);
    add(1600.0f, 0.0f, Planner::NpcFlag::INNKEEPER, true, true, false);
    Planner::PlannerIndexes const index = Planner::PlannerIndexes::Build(input);
    bool hubs = index.hubs.size() == 2 && index.hubs[0].side == PS::Alliance && index.hubs[1].side == PS::Horde;
    for (Planner::Hub const& hub : index.hubs)
        for (std::uint32_t member : hub.members)
            if (index.services[member].mailbox)
                hubs = hubs && index.services[member].side == hub.side;
    Check(hubs, "two towns of one contested zone each have their own side; a mailbox takes its town's");

    MapPoint const from{1, 2000.0f, 0.0f, 10.0f};
    BotKnowledge const ally = Bot(Side::Alliance, 20);
    BotKnowledge const horde = Bot(Side::Horde, 20);
    std::vector<Planner::Nearby> const allyOne =
        NearestOffered(index, Side::Alliance, Planner::ServiceKind::Innkeeper, from, 1);
    Check(allyOne.size() == 1 && index.services[allyOne[0].index].x == 0.0f &&
              !OffersHostile(index, Side::Alliance, allyOne),
          "the Alliance bot's one nearest inn is its own town's: the side filter comes before the cap");
    std::vector<Planner::Nearby> const hordeAll =
        NearestOffered(index, Side::Horde, Planner::ServiceKind::Innkeeper, from, 64);
    Check(hordeAll.size() == 2 && index.services[hordeAll[0].index].x == 1500.0f &&
              index.services[hordeAll[1].index].x == 3000.0f && !OffersHostile(index, Side::Horde, hordeAll),
          "the Horde bot gets its two innkeepers nearest first, never the Alliance one or the one hostile to both");
    std::vector<BotStateItem> const allyLines =
        InnLines(index, ally, from, NearestOffered(index, Side::Alliance, Planner::ServiceKind::Innkeeper, from, 64));
    bool allyClean = allyLines.size() == 1;
    for (BotStateItem const& line : allyLines)
        allyClean = allyClean && line.word == L::ROW_WHERE_FOUND && line.side == "alliance" &&
                    Holds(line.text, "innkeeper") && !Holds(line.text, L::NOT_SERVED);
    std::vector<BotStateItem> const hordeLines = InnLines(index, horde, from, hordeAll);
    bool hordeClean = hordeLines.size() == 2;
    for (BotStateItem const& line : hordeLines)
        hordeClean = hordeClean && line.side == "horde" && !Holds(line.text, L::NOT_SERVED);
    Check(allyClean && hordeClean, "where inn in a contested zone: each side its own inns, with their side words");

    // The mutant offers a hostile innkeeper: both checks above kill it
    std::vector<Planner::Nearby> const mutant = NearestAnySide(index, Planner::ServiceKind::Innkeeper, from, 1);
    std::vector<BotStateItem> const mutantLines =
        InnLines(index, ally, from, NearestAnySide(index, Planner::ServiceKind::Innkeeper, from, 64));
    bool mutantServed = true;
    for (BotStateItem const& line : mutantLines)
        mutantServed = mutantServed && line.side == "alliance" && !Holds(line.text, L::NOT_SERVED);
    Check(OffersHostile(index, Side::Alliance, mutant) && !mutantServed,
          "mutant: the nearest inns whatever their side offer the Alliance bot a hostile innkeeper; killed");
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
    NpcFact gryshka = Npc("Gryshka", Side::Horde, 10);
    gryshka.what = "innkeeper";
    gryshka.place = "Orgrimmar";
    gryshka.fact.zoneId = 1637;
    gryshka.fact.side = Side::Horde;
    WhoItems(ALLY12, {}, {gryshka}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_NPC && items[0].side == "horde" &&
              items[0].text == "Gryshka, innkeeper of the Horde in Orgrimmar. They do not serve you.",
          "LC.54: a person of the other side: named with its side and its place's, not served");
    WhoItems(Bot(Side::Horde, 12), {}, {gryshka}, items);
    Check(items.size() == 1 && items[0].text == "Gryshka, innkeeper of the Horde in Orgrimmar.",
          "a person of the bot's own side is served");
    WhoItems(ALLY12, {}, {Npc("Gryshka", Side::Horde, 10)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_NPC && items[0].side.empty() &&
              !Holds(items[0].text, L::NOT_SERVED),
          "a person of the other side without a service or a place: no side word, nothing to serve");
    WhoItems(ALLY12, {}, {Npc("Gryshka", Side::Horde, 70)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN && items[0].text == L::WHO_UNKNOWN &&
              !Holds(items[0].text, "Gryshka") && items[0].name == items[0].word,
          "a person of the other side beyond the level band: one unknown line, no name");
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
    // The two rows above as the reader builds their sides: an Alliance bot of the quest's level band, one unknown line
    WhatItems(ALLY12, {Item("A Letter to Yvette", 4, SideOfItem(1, 0xFFFFFFFFu), false)}, {}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN && !Holds(items[0].text, "Yvette"),
          "a Horde-flagged item without a race mask is unknown to an Alliance bot");
    WhatItems(ALLY12, {}, {Quest("Poison Water", 5, SideOfQuest(SideOfFactionGroup(4), 0), QuestState::None)}, items);
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_UNKNOWN && !Holds(items[0].text, "Poison"),
          "a quest of a Horde giver without a race mask is unknown to an Alliance bot");
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
    Check(items.size() == 1 && items[0].word == L::ROW_LOOKUP_ZONE && Holds(items[0].text, L::ZONE_LANDS[2]),
          "LC.54: a land of the other side is known by the same rule");
    ZoneFact ashenvale = Land("Ashenvale", Side::Neither, 18, 30, 331);
    TownFact astranaar{"Astranaar", ashenvale.fact};
    astranaar.fact.side = Side::Alliance;
    TownFact splintertree{"Splintertree Post", ashenvale.fact};
    splintertree.fact.side = Side::Horde;
    TownFact camp{"Bloodsail Camp", ashenvale.fact};
    camp.fact.noSide = true;
    ashenvale.towns = {astranaar, splintertree, camp};
    BotKnowledge visitor = Bot(Side::Horde, 20);
    visitor.stayedZones.push_back(331);
    ZoneItems(visitor, {ashenvale}, items);
    Check(items.size() == 1 && Holds(items[0].text, "Astranaar (Alliance)") &&
              Holds(items[0].text, "Splintertree Post (Horde)") && Holds(items[0].text, "Bloodsail Camp.") &&
              Holds(items[0].text, L::ZONE_LANDS[0]),
          "a contested land names both sides' towns with their own sides, a town of no side without one");
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
          L::ZONE_NAME_SENTENCE, L::WHO_UNKNOWN, L::WHAT_UNKNOWN, L::ZONE_UNKNOWN, L::LOOKUP_PENDING, L::NOT_SERVED})
        digits = digits || HasDigit(text);
    for (char const* text : L::TOWN_SIDES)
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
    PlaceSideChecks();
    KnownChecks();
    WhoChecks();
    WhatChecks();
    ZoneChecks();
    WordChecks();
    IndexChecks();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
