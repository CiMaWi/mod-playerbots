/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGELOOKUPRULES_H
#define PLAYERBOTS_BRIDGELOOKUPRULES_H

// LC18b: what a bot knows of people, items, quests and lands (§3.3, LC.5) and the words of the `who`, `what` and `zone`
// answers, Player-free. The readers in BridgeBotState.cpp turn the bot, its bags, log and record and the loaded
// templates into the plain facts below on the world thread; this header decides what the bot knows and words the
// answer. It shares LC18a's notion of a place (BridgeKnowledgeRules.h Knows) for lands and for a person's or a quest
// giver's place. Only the readers and tests/bridge-lookup-selftest.cpp include it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BridgeKnowledgeRules.h"

namespace PlayerbotBridge::Knowledge
{
// Level bands of what a bot knows of the world: 1-60, 61-70 (Outland), 71-80 (Northrend); a level 0 counts as 1
constexpr std::array<std::uint32_t, 2> BAND_TOPS = {60, 70};

// The tops of the level words (BridgeTexts::Lookups::LEVEL_WORDS); a level above the last top takes the last word
constexpr std::array<std::uint32_t, 5> LEVEL_WORD_TOPS = {20, 40, 60, 70, 80};
static_assert(BridgeTexts::Lookups::LEVEL_WORDS.size() == LEVEL_WORD_TOPS.size(), "a word per top");

inline unsigned LevelBand(std::uint32_t level)
{
    unsigned band = 0;
    for (std::uint32_t top : BAND_TOPS)
        if (level > top)
            ++band;
    return band;
}

inline char const* LevelWord(std::uint32_t level)
{
    for (std::size_t word = 0; word < LEVEL_WORD_TOPS.size(); ++word)
        if (level <= LEVEL_WORD_TOPS[word])
            return BridgeTexts::Lookups::LEVEL_WORDS[word];
    return BridgeTexts::Lookups::LEVEL_WORDS.back();
}

// The playable races of each side as the game's race masks (human, dwarf, night elf, gnome, draenei; orc, undead,
// tauren, troll, blood elf) and the faction groups of a faction template (ourMask: Alliance 2, Horde 4)
constexpr std::uint32_t ALLIANCE_RACES = 1101;
constexpr std::uint32_t HORDE_RACES = 690;
constexpr std::uint32_t FACTION_GROUP_ALLIANCE = 2;
constexpr std::uint32_t FACTION_GROUP_HORDE = 4;

inline Side SideOfMask(std::uint32_t mask, std::uint32_t alliance, std::uint32_t horde)
{
    bool const forAlliance = (mask & alliance) != 0;
    bool const forHorde = (mask & horde) != 0;
    if (forAlliance != forHorde)
        return forAlliance ? Side::Alliance : Side::Horde;
    return Side::Neither;
}

// An item's or a quest's race mask: one side's races only make it that side's; none or all races, neither's
inline Side SideOfRaces(std::uint32_t mask) { return SideOfMask(mask, ALLIANCE_RACES, HORDE_RACES); }

inline Side SideOfFactionGroup(std::uint32_t ourMask)
{
    return SideOfMask(ourMask, FACTION_GROUP_ALLIANCE, FACTION_GROUP_HORDE);
}

// The game's item flags of a faction-bound item (ItemTemplate Flags2): only Horde or only Alliance may use it
constexpr std::uint32_t ITEM_FLAGS2_HORDE = 0x00000001;
constexpr std::uint32_t ITEM_FLAGS2_ALLIANCE = 0x00000002;

// An item's side: its faction flags when they name one side (many faction-bound items carry no race mask), else its
// race mask
inline Side SideOfItem(std::uint32_t flags2, std::uint32_t raceMask)
{
    bool const forHorde = (flags2 & ITEM_FLAGS2_HORDE) != 0;
    bool const forAlliance = (flags2 & ITEM_FLAGS2_ALLIANCE) != 0;
    if (forAlliance != forHorde)
        return forAlliance ? Side::Alliance : Side::Horde;
    return SideOfRaces(raceMask);
}

// A quest's side: its giver's faction group when that names one side (the quests of the Horde lands often carry no
// race mask), else its race mask
inline Side SideOfQuest(Side giver, std::uint32_t raceMask)
{
    return giver != Side::Neither ? giver : SideOfRaces(raceMask);
}

// A thing the rule judges: whose it is and the level it belongs to
struct ThingFact
{
    Side side{Side::Neither};
    std::uint32_t level{0};
};

// §3.3: a thing is known when the bot carries, has in its log, has done or sees it; else when it is of the bot's side
// (or of neither) and its level lies in the bot's level band or a lower one.
inline bool ThingKnown(BotKnowledge const& bot, ThingFact const& thing, bool has)
{
    if (has)
        return true;
    bool const sideAllows = thing.side == Side::Neither || thing.side == bot.side;
    return sideAllows && LevelBand(thing.level) <= LevelBand(bot.level);
}

// The most lines of one answer (the topic rows' limit)
constexpr std::size_t LOOKUP_SHOWN = 3;

enum class LookupRow : std::uint8_t
{
    Npc,
    Bot,
    Item,
    Quest,
    Zone,
    Unknown,
    Pending
};

enum class LookupTopic : std::uint8_t
{
    Who,
    What,
    Zone
};

inline char const* LookupWord(LookupRow row)
{
    namespace L = BridgeTexts::Lookups;
    switch (row)
    {
        case LookupRow::Npc:
            return L::ROW_LOOKUP_NPC;
        case LookupRow::Bot:
            return L::ROW_LOOKUP_BOT;
        case LookupRow::Item:
            return L::ROW_LOOKUP_ITEM;
        case LookupRow::Quest:
            return L::ROW_LOOKUP_QUEST;
        case LookupRow::Zone:
            return L::ROW_LOOKUP_ZONE;
        case LookupRow::Unknown:
            return L::ROW_LOOKUP_UNKNOWN;
        case LookupRow::Pending:
            return L::ROW_LOOKUP_PENDING;
    }
    return "";
}

inline BotStateItem LookupLine(LookupRow row, std::string name, std::string text)
{
    // the channel drops an entry without a name: the row's own word stands in, never the name asked
    if (name.empty())
        name = LookupWord(row);
    return {std::move(name), 0, 0, static_cast<std::uint8_t>(row), std::move(text), LookupWord(row)};
}

// Nothing known: one line that does not repeat the name asked
inline void LookupUnknown(LookupTopic topic, std::vector<BotStateItem>& items)
{
    namespace L = BridgeTexts::Lookups;
    items.clear();
    char const* text = L::ZONE_UNKNOWN;
    if (topic == LookupTopic::Who)
        text = L::WHO_UNKNOWN;
    else if (topic == LookupTopic::What)
        text = L::WHAT_UNKNOWN;
    items.push_back(LookupLine(LookupRow::Unknown, {}, text));
}

inline void LookupPending(std::vector<BotStateItem>& items)
{
    items.clear();
    items.push_back(LookupLine(LookupRow::Pending, {}, BridgeTexts::Lookups::LOOKUP_PENDING));
}

// A person of the world: a service spawn or a creature the bot sees
struct NpcFact
{
    std::string name;
    std::string what;   // a service word, or empty for none
    std::string place;  // the town or zone it stands in, empty for none
    Side side{Side::Neither};
    std::uint32_t level{0};
    bool seen{false};
    PlaceFact fact;  // the place the rule judges for the "in <place>" part
};

// Whether the line names the person's place: one the bot knows
inline bool NpcPlaceShown(BotKnowledge const& bot, NpcFact const& npc)
{
    return !npc.place.empty() && Knows(bot, npc.fact);
}

// LC18g: a person of the other side with a service ends with NOT_SERVED (LC.54)
inline std::string NpcText(BotKnowledge const& bot, NpcFact const& npc)
{
    namespace L = BridgeTexts::Lookups;
    std::string text = npc.name;
    text.append(L::LOOKUP_SEP).append(npc.what.empty() ? L::WHO_PERSON : npc.what.c_str());
    text.append(L::LOOKUP_SPACE).append(L::WHO_SIDES[static_cast<std::size_t>(npc.side)]);
    if (NpcPlaceShown(bot, npc))
        text.append(L::LOOKUP_IN).append(npc.place);
    text.append(L::LOOKUP_END);
    if (!npc.what.empty() && npc.side != Side::Neither && npc.side != bot.side)
        text.append(L::NOT_SERVED);
    return text;
}

// The who answer: the bots the record knows (lines of their own, made by the reader) first, then the people the bot
// knows (one line per name; LC18g: people of either side alike, by the level band, each line with its place's side
// word when it names the place); at most LOOKUP_SHOWN lines; none: the unknown line.
inline void WhoItems(BotKnowledge const& bot, std::vector<BotStateItem> botLines, std::vector<NpcFact> const& people,
                     std::vector<BotStateItem>& items)
{
    items.clear();
    for (BotStateItem& line : botLines)
        if (items.size() < LOOKUP_SHOWN)
            items.push_back(std::move(line));
    std::vector<std::string> shown;
    for (NpcFact const& npc : people)
    {
        if (items.size() >= LOOKUP_SHOWN)
            break;
        std::string const key = NameKey(npc.name);
        if (!ThingKnown(bot, {Side::Neither, npc.level}, npc.seen) ||
            std::find(shown.begin(), shown.end(), key) != shown.end())
            continue;
        shown.push_back(key);
        items.push_back(LookupLine(LookupRow::Npc, npc.name, NpcText(bot, npc)));
        if (NpcPlaceShown(bot, npc))
            items.back().side = SideWord(npc.fact);
    }
    if (items.empty())
        LookupUnknown(LookupTopic::Who, items);
}

struct ItemFact
{
    std::string name;
    std::uint32_t quality{0};
    std::uint32_t itemClass{0};
    std::uint32_t level{0};
    Side side{Side::Neither};
    bool carried{false};
};

enum class QuestState : std::uint8_t
{
    None,
    InLog,
    Done
};

struct QuestFact
{
    std::string title;
    std::uint32_t level{0};
    Side side{Side::Neither};
    QuestState state{QuestState::None};
    std::string giver;  // empty for none
    std::string place;  // where the giver stands, empty for none
    PlaceFact fact;
};

inline std::string ItemText(ItemFact const& item)
{
    namespace L = BridgeTexts::Lookups;
    auto const& kind = L::ITEM_KINDS[std::min<std::size_t>(item.itemClass, L::ITEM_KINDS.size() - 1)];
    std::string text = item.name;
    text.append(L::LOOKUP_SEP)
        .append(L::ITEM_QUALITIES[std::min<std::size_t>(item.quality, L::ITEM_QUALITIES.size() - 1)])
        .append(L::LOOKUP_SPACE)
        .append(kind.Kind)
        .append(L::LOOKUP_OF)
        .append(LevelWord(item.level))
        .append(L::LOOKUP_SEP)
        .append(kind.Use)
        .append(L::LOOKUP_SEMI)
        .append(item.carried ? L::LOOKUP_CARRY : L::LOOKUP_NO_CARRY);
    return text.append(L::LOOKUP_END);
}

inline std::string QuestText(BotKnowledge const& bot, QuestFact const& quest)
{
    namespace L = BridgeTexts::Lookups;
    std::string text = quest.title;
    text.append(L::LOOKUP_SEP).append(L::LOOKUP_QUEST).append(L::LOOKUP_OF).append(LevelWord(quest.level));
    if (!quest.giver.empty())
    {
        text.append(L::LOOKUP_GIVEN_BY).append(quest.giver);
        if (!quest.place.empty() && Knows(bot, quest.fact))
            text.append(L::LOOKUP_IN).append(quest.place);
    }
    text.append(L::LOOKUP_SEMI);
    if (quest.state == QuestState::InLog)
        text.append(L::LOOKUP_HAVE);
    else if (quest.state == QuestState::Done)
        text.append(L::LOOKUP_DONE);
    else
        text.append(L::LOOKUP_NO_HAVE);
    return text.append(L::LOOKUP_END);
}

// The what answer: the items, then the quests, the bot knows; at most LOOKUP_SHOWN lines; none: the unknown line
inline void WhatItems(BotKnowledge const& bot, std::vector<ItemFact> const& things,
                      std::vector<QuestFact> const& quests, std::vector<BotStateItem>& items)
{
    items.clear();
    for (ItemFact const& item : things)
        if (items.size() < LOOKUP_SHOWN && ThingKnown(bot, {item.side, item.level}, item.carried))
            items.push_back(LookupLine(LookupRow::Item, item.name, ItemText(item)));
    for (QuestFact const& quest : quests)
        if (items.size() < LOOKUP_SHOWN && ThingKnown(bot, {quest.side, quest.level}, quest.state != QuestState::None))
            items.push_back(LookupLine(LookupRow::Quest, quest.title, QuestText(bot, quest)));
    if (items.empty())
        LookupUnknown(LookupTopic::What, items);
}

struct TownFact
{
    std::string name;
    PlaceFact fact;
};

struct ZoneFact
{
    std::string name;
    Side side{Side::Neither};
    std::uint32_t low{0};  // its level range, 0/0 = none known
    std::uint32_t high{0};
    PlaceFact fact;
    std::vector<TownFact> towns;
};

inline std::string ZoneText(BotKnowledge const& bot, ZoneFact const& zone)
{
    namespace L = BridgeTexts::Lookups;
    std::string text = zone.name;
    text.append(L::LOOKUP_SEP).append(L::ZONE_LANDS[static_cast<std::size_t>(zone.side)]);
    if (zone.low || zone.high)
    {
        text.append(L::LOOKUP_FOR).append(LevelWord(zone.low));
        if (std::string_view(LevelWord(zone.low)) != LevelWord(zone.high))
            text.append(L::LOOKUP_TO).append(LevelWord(zone.high));
    }
    std::vector<std::string> names;
    std::vector<std::string> towns;  // LC18g: each with its own side's words
    for (TownFact const& town : zone.towns)
        if (town.name != zone.name && std::find(names.begin(), names.end(), town.name) == names.end() &&
            Knows(bot, town.fact))
        {
            names.push_back(town.name);
            towns.push_back(town.fact.noSide ? town.name
                                             : town.name + L::TOWN_SIDES[static_cast<std::size_t>(town.fact.side)]);
        }
    for (std::size_t index = 0; index < towns.size(); ++index)
    {
        char const* join = index == 0                  ? L::LOOKUP_KNOW_TOWNS
                           : index + 1 == towns.size() ? L::LOOKUP_AND
                                                       : L::LOOKUP_SEP;
        text.append(join).append(towns[index]);
    }
    return text.append(L::LOOKUP_END);
}

// The zone answer: the lands the place rule lets the bot know; at most LOOKUP_SHOWN lines; none: the unknown line
inline void ZoneItems(BotKnowledge const& bot, std::vector<ZoneFact> const& zones, std::vector<BotStateItem>& items)
{
    items.clear();
    for (ZoneFact const& zone : zones)
        if (items.size() < LOOKUP_SHOWN && Knows(bot, zone.fact))
            items.push_back(LookupLine(LookupRow::Zone, zone.name, ZoneText(bot, zone)));
    if (items.empty())
        LookupUnknown(LookupTopic::Zone, items);
}
}  // namespace PlayerbotBridge::Knowledge

#endif
