/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RECORDWORDS_H
#define PLAYERBOTS_RECORDWORDS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "RecordRules.h"

// The readers of a bot's record (living-conversation LC7): one fixed in-world sentence per kind, built from the row's
// names and places and never holding a number or an id, and the bridge topics history (the permanent rows in time
// order, then the world the bot knows: dungeons, bosses and zones), lately (the recent deeds, newest first) and known
// (the bots met in shared events: their people, calling and what was shared). Pure: the game's names come through
// Lookup, so tests/bot-record-readers-selftest.cpp proves every sentence and reader offline. Limits are tuning values.
namespace RecordWords
{
constexpr std::size_t MAX_NAME = 64;       // A name inside a sentence, cut on a character boundary
constexpr std::size_t MAX_SENTENCE = 200;  // The wire's text cap (mod-ollama-chat ChannelMaxTopicText)
constexpr std::size_t HISTORY_LIMIT = 24;
constexpr std::size_t HISTORY_EVENTS = 16;  // The rest of history is what the bot knows of the world
constexpr std::size_t LATELY_LIMIT = 12;
constexpr std::size_t KNOWN_LIMIT = 12;
constexpr std::size_t STATE_DEEDS = 3;  // The recent deeds a bot's state carries
constexpr uint32_t SECONDS_PER_DAY = 86400;
constexpr uint32_t SKILL_RANK_COUNT = 6;

// A history entry's row (its code on the bridge wire)
enum class HistoryRow : uint8_t
{
    Event,
    Zone,
    Dungeon,
    Boss
};

// One topic entry: Name a word or a name, Text a sentence; Value and Extra as BridgeBotState.h describes the topic
struct Entry
{
    uint8_t Code = 0;
    std::string Name;
    std::string Text;
    uint32_t Value = 0;
    uint32_t Extra = 0;
};

struct Person
{
    std::string Name;
    uint8_t Race = 0;
    uint8_t Class = 0;
};

// The game's names of a row's ids; empty (or none) for an id the game does not know
class Lookup
{
public:
    virtual ~Lookup() = default;
    virtual std::string PlaceName(uint32_t area, uint32_t zone) const = 0;  // The area's name, else the zone's
    virtual std::string ZoneName(uint32_t zone) const = 0;
    virtual std::string CreatureName(uint32_t entry) const = 0;
    virtual bool Boss(uint32_t entry) const = 0;  // A dungeon boss or a world boss
    virtual std::string QuestTitle(uint32_t id) const = 0;
    virtual std::string ItemName(uint32_t id) const = 0;
    virtual std::string SpellName(uint32_t id) const = 0;
    virtual std::string SkillName(uint32_t id) const = 0;
    virtual std::string FlightPointName(uint32_t node) const = 0;
    virtual std::string DungeonName(uint32_t map) const = 0;  // Empty for a map that is not a dungeon or raid
    virtual std::optional<Person> Character(uint32_t guid) const = 0;
};

inline constexpr char const* RACE_WORDS[] = {"",       "human", "orc",   "dwarf", "night elf", "undead",
                                             "tauren", "gnome", "troll", "",      "blood elf", "draenei"};
inline constexpr char const* CLASS_WORDS[] = {
    "", "warrior", "paladin", "hunter", "rogue", "priest", "death knight", "shaman", "mage", "warlock", "", "druid"};
// The craft rank a skill value of CRAFT_RANK_STEP times n completes
inline constexpr char const* SKILL_RANK_WORDS[SKILL_RANK_COUNT] = {"apprentice", "journeyman", "expert",
                                                                   "artisan",    "master",     "grand master"};

inline char const* RaceWord(uint8_t race) { return race < std::size(RACE_WORDS) ? RACE_WORDS[race] : ""; }

inline char const* ClassWord(uint8_t cls) { return cls < std::size(CLASS_WORDS) ? CLASS_WORDS[cls] : ""; }

inline bool HasDigit(std::string_view text) { return text.find_first_of("0123456789") != std::string_view::npos; }

// text cut to at most max bytes, never inside a UTF-8 character
inline std::string Cut(std::string text, std::size_t max)
{
    if (text.size() <= max)
        return text;

    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    text.resize(end);
    return text;
}

// A name fit for a sentence (cut to MAX_NAME bytes), else plain: an unknown name or one that carries a digit
inline std::string NameOr(std::string name, char const* plain)
{
    return name.empty() || HasDigit(name) ? std::string(plain) : Cut(std::move(name), MAX_NAME);
}

// The word value of key in a facts text ("key=value;"), empty when absent
inline std::string_view FactWord(std::string_view facts, std::string_view key)
{
    std::size_t start = 0;
    while (start < facts.size())
    {
        std::size_t const end = std::min(facts.find(';', start), facts.size());
        std::string_view const pair = facts.substr(start, end - start);
        std::size_t const equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == key)
            return pair.substr(equals + 1);
        start = end + 1;
    }

    return {};
}

// One person or creature of a row's others ("b<guid>" a bot, "p<guid>" a player, "n<entry>" a creature)
struct Token
{
    char Type = 0;
    uint32_t Id = 0;
};

inline std::vector<Token> Tokens(std::string_view others)
{
    std::vector<Token> tokens;
    std::size_t start = 0;
    while (start < others.size())
    {
        std::size_t const end = std::min(others.find(' ', start), others.size());
        std::string_view const word = others.substr(start, end - start);
        start = end + 1;
        if (word.size() < 2 || (word[0] != 'b' && word[0] != 'p' && word[0] != 'n'))
            continue;

        uint32_t id = 0;
        bool digits = true;
        for (char const c : word.substr(1))
        {
            digits = digits && c >= '0' && c <= '9';
            id = id * 10 + static_cast<uint32_t>(c - '0');
        }
        if (digits)
            tokens.push_back({word[0], id});
    }

    return tokens;
}

inline bool StartsWithVowel(std::string_view word)
{
    return !word.empty() && std::string_view("aeiouAEIOU").find(word[0]) != std::string_view::npos;
}

// "a human warrior", "an orc", "an undead mage"; empty when neither word is known
inline std::string PeopleWords(Person const& person)
{
    std::string words = RaceWord(person.Race);
    std::string const calling = ClassWord(person.Class);
    if (!calling.empty())
        words += words.empty() ? calling : " " + calling;
    if (words.empty())
        return words;

    return (StartsWithVowel(words) ? "an " : "a ") + words;
}

// The first bot of the row's others that is not the bot itself
inline std::optional<Person> Companion(RecordRules::Row const& row, uint32_t self, Lookup const& lookup)
{
    for (Token const& token : Tokens(row.Others))
        if (token.Type == 'b' && token.Id != self)
            if (std::optional<Person> person = lookup.Character(token.Id))
                if (!person->Name.empty() && !HasDigit(person->Name))
                    return person;

    return std::nullopt;
}

// The row's one in-world sentence; self is the bot whose row it is
inline std::string Sentence(RecordRules::Row const& row, uint32_t self, Lookup const& lookup)
{
    using RecordRules::Kind;
    std::string const place = NameOr(lookup.PlaceName(row.Area, row.Zone), "the wilds");
    auto const fact = [&row](std::string_view key) { return RecordRules::FactValue(row.Facts, key).value_or(0); };
    std::string text;
    switch (row.EventKind)
    {
        case Kind::Kill:
        {
            std::string const foe = lookup.CreatureName(fact("entry"));
            uint32_t const rank = fact("rank");
            char const* how = "";
            if (foe.empty() || HasDigit(foe))
                how = "";  // "a foe" says no more
            else if (rank == RecordRules::RANK_WORLD_BOSS)
                how = ", a fearsome foe,";
            else if (rank == RecordRules::RANK_ELITE || rank == RecordRules::RANK_RARE_ELITE)
                how = ", an elite foe,";
            else if (rank == RecordRules::RANK_RARE)
                how = ", a rare foe,";
            else if (fact("above") >= static_cast<uint32_t>(RecordRules::HARD_LEVELS))
                how = ", a foe stronger than you,";
            text = "You slew " + NameOr(foe, "a foe") + how + " in " + place + ".";
            break;
        }
        case Kind::Quest:
        {
            std::string const title = NameOr(lookup.QuestTitle(fact("quest")), "");
            text = (title.empty() ? "You finished a quest" : "You finished the quest " + title) + " in " + place + ".";
            break;
        }
        case Kind::Equip:
            text = "You put on " + NameOr(lookup.ItemName(fact("item")), "new gear") + " in " + place + ".";
            break;
        case Kind::Level:
            text = fact("level") % RecordRules::LEVEL_MILESTONE == 0
                       ? "You reached a milestone of experience in " + place + "."
                       : "You grew in experience in " + place + ".";
            break;
        case Kind::Skill:
        {
            uint32_t const step = fact("value") / RecordRules::CRAFT_RANK_STEP;
            std::string const craft = NameOr(lookup.SkillName(fact("skill")), "your craft");
            text = step >= 1 && step <= SKILL_RANK_COUNT
                       ? "You mastered " + std::string(SKILL_RANK_WORDS[step - 1]) + " " + craft + " in " + place + "."
                       : "You grew better at " + craft + " in " + place + ".";
            break;
        }
        case Kind::Spell:
            text = "You learned " + NameOr(lookup.SpellName(fact("spell")), "a new skill") + " in " + place + ".";
            break;
        case Kind::Flight:
            text =
                "You found the flight master at " + NameOr(lookup.FlightPointName(fact("node")), place.c_str()) + ".";
            break;
        case Kind::Death:
        {
            std::string_view const by = FactWord(row.Facts, "by");
            std::vector<Token> const tokens = Tokens(row.Others);
            std::string killer;
            for (Token const& token : tokens)
                if (token.Type == 'n' && by == "creature")
                    killer = lookup.CreatureName(token.Id);
                else if (token.Type != 'n' && (by == "player" || by == "player_pet"))
                    if (std::optional<Person> person = lookup.Character(token.Id))
                        killer = person->Name;
            if (!killer.empty() && !HasDigit(killer))
                text = (by == "creature" ? "You died to " : "You were slain by ") + Cut(killer, MAX_NAME) + " in " +
                       place + ".";
            else
                text = "You died in " + place + ".";
            break;
        }
        case Kind::Resurrect:
            text = "You came back to life in " + place + ".";
            break;
        case Kind::Fight:
        {
            std::optional<Person> const companion = Companion(row, self, lookup);
            std::string foe;
            for (Token const& token : Tokens(row.Others))
                if (token.Type == 'n' && foe.empty())
                    foe = lookup.CreatureName(token.Id);
            if (companion)
                text = "You fought beside " + Cut(companion->Name, MAX_NAME) + " in " + place + ".";
            else if (!foe.empty() && !HasDigit(foe))
                text = "You fought " + Cut(foe, MAX_NAME) + " in " + place + ".";
            else
                text = "You fought a battle in " + place + ".";
            break;
        }
        case Kind::Dungeon:
            text = "You entered " + NameOr(lookup.DungeonName(fact("map")), "a dungeon") + ".";
            break;
        case Kind::Group:
            text = "You held together with a group in " + place + ".";
            break;
        case Kind::Stay:
            text = "You spent a long while in " + NameOr(lookup.ZoneName(row.Zone), "the wilds") + ".";
            break;
        case Kind::DungeonRun:
            text = "You cleared " + NameOr(lookup.DungeonName(fact("map")), "a dungeon") + ".";
            break;
        case Kind::Guild:
            text = "You joined a guild in " + place + ".";
            break;
        case Kind::Duel:
        {
            std::optional<Person> const rival = Companion(row, self, lookup);
            text = rival ? "You fought a duel with " + Cut(rival->Name, MAX_NAME) + " in " + place + "."
                         : "You fought a duel in " + place + ".";
            break;
        }
    }

    return Cut(std::move(text), MAX_SENTENCE);
}

inline uint32_t Age(uint32_t time, uint32_t now) { return now > time ? now - time : 0; }

// The permanent rows (the strongest HISTORY_EVENTS, shown in time order), then the dungeons, bosses and zones the bot
// knows from all its living rows, most often first; at most HISTORY_LIMIT entries. Event: name the kind word, value
// days ago; the others: name the place or creature, value the events behind it.
inline std::vector<Entry> History(std::vector<RecordRules::Row> const& rows, uint32_t self, uint32_t now,
                                  Lookup const& lookup)
{
    std::vector<RecordRules::Row> kept;
    for (RecordRules::Row const& row : rows)
        if (!row.Expires)
            kept.push_back(row);
    std::sort(kept.begin(), kept.end(), [](RecordRules::Row const& a, RecordRules::Row const& b)
              { return a.Score != b.Score ? a.Score > b.Score : a.Time > b.Time; });
    if (kept.size() > HISTORY_EVENTS)
        kept.resize(HISTORY_EVENTS);
    std::sort(kept.begin(), kept.end(), [](RecordRules::Row const& a, RecordRules::Row const& b)
              { return a.Time != b.Time ? a.Time < b.Time : a.EventId < b.EventId; });

    std::vector<Entry> entries;
    for (RecordRules::Row const& row : kept)
        entries.push_back({static_cast<uint8_t>(HistoryRow::Event), RecordRules::Word(row.EventKind),
                           Sentence(row, self, lookup), Age(row.Time, now) / SECONDS_PER_DAY, 0});

    // Knowledge per (row, id): events behind it and the latest time, most events first, then the latest
    struct Known
    {
        uint32_t Count = 0;
        uint32_t Latest = 0;
    };
    std::map<std::pair<HistoryRow, uint32_t>, Known> known;
    for (RecordRules::Row const& row : rows)
    {
        if (RecordRules::Expired(row, now))
            continue;
        auto const note = [&known, &row](HistoryRow kind, uint32_t id, uint32_t count)
        {
            Known& seen = known[{kind, id}];
            seen.Count += count;
            seen.Latest = std::max(seen.Latest, row.Time);
        };
        note(HistoryRow::Zone, row.Zone, RecordRules::Count(row));
        uint32_t const map = RecordRules::FactValue(row.Facts, "map").value_or(0);
        if ((row.EventKind == RecordRules::Kind::Dungeon || row.EventKind == RecordRules::Kind::DungeonRun) && map)
            note(HistoryRow::Dungeon, map, 1);
        uint32_t const entry = RecordRules::FactValue(row.Facts, "entry").value_or(0);
        if (row.EventKind == RecordRules::Kind::Kill && entry && lookup.Boss(entry))
            note(HistoryRow::Boss, entry, 1);
    }

    std::vector<std::pair<std::pair<HistoryRow, uint32_t>, Known>> ranked(known.begin(), known.end());
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](auto const& a, auto const& b)
                     {
                         if (a.first.first != b.first.first)
                             return a.first.first > b.first.first;  // Bosses, then dungeons, then zones
                         return a.second.Count != b.second.Count ? a.second.Count > b.second.Count
                                                                 : a.second.Latest > b.second.Latest;
                     });
    for (auto const& [key, seen] : ranked)
    {
        if (entries.size() >= HISTORY_LIMIT)
            break;

        auto const [kind, id] = key;
        std::string name;
        std::string text;
        if (kind == HistoryRow::Zone)
        {
            name = NameOr(lookup.ZoneName(id), "");
            text = "You have spent time in " + name + ".";
        }
        else if (kind == HistoryRow::Dungeon)
        {
            name = NameOr(lookup.DungeonName(id), "");
            text = "You have been inside " + name + ".";
        }
        else
        {
            name = NameOr(lookup.CreatureName(id), "");
            text = "You helped bring down " + name + ".";
        }
        if (!name.empty())
            entries.push_back({static_cast<uint8_t>(kind), name, Cut(text, MAX_SENTENCE), seen.Count, 0});
    }

    return entries;
}

// The recent deeds that have not faded, newest first, at most max: name the kind word, value seconds ago
inline std::vector<Entry> Lately(std::vector<RecordRules::Row> const& rows, uint32_t self, uint32_t now,
                                 Lookup const& lookup, std::size_t max = LATELY_LIMIT)
{
    std::vector<RecordRules::Row const*> recent;
    for (RecordRules::Row const& row : rows)
        if (row.Expires && !RecordRules::Expired(row, now))
            recent.push_back(&row);
    std::sort(recent.begin(), recent.end(), [](RecordRules::Row const* a, RecordRules::Row const* b)
              { return a->Time != b->Time ? a->Time > b->Time : a->EventId > b->EventId; });

    std::vector<Entry> entries;
    for (RecordRules::Row const* row : recent)
    {
        if (entries.size() >= max)
            break;
        entries.push_back({0, RecordRules::Word(row->EventKind), Sentence(*row, self, lookup), Age(row->Time, now), 0});
    }

    return entries;
}

// The deed sentences a bot's state carries
inline std::vector<std::string> Deeds(std::vector<RecordRules::Row> const& rows, uint32_t self, uint32_t now,
                                      Lookup const& lookup)
{
    std::vector<std::string> deeds;
    for (Entry& entry : Lately(rows, self, now, lookup, STATE_DEEDS))
        deeds.push_back(std::move(entry.Text));
    return deeds;
}

// What a known bot shared with this one, as a phrase per kind
inline char const* SharedPhrase(RecordRules::Kind kind)
{
    switch (kind)
    {
        case RecordRules::Kind::Kill:
            return "slew foes with you";
        case RecordRules::Kind::Fight:
            return "fought beside you";
        case RecordRules::Kind::Death:
            return "once killed you";
        case RecordRules::Kind::Group:
            return "held a group together with you";
        case RecordRules::Kind::DungeonRun:
            return "cleared a dungeon with you";
        case RecordRules::Kind::Duel:
            return "fought a duel with you";
        default:
            return "crossed paths with you";
    }
}

// The bots of the living rows' others but the bot itself, one entry each, most shared events first (then the latest):
// name the bot's name, text who it is and what was shared, value days since the last, extra the events shared
inline std::vector<Entry> KnownBots(std::vector<RecordRules::Row> const& rows, uint32_t self, uint32_t now,
                                    Lookup const& lookup)
{
    struct Met
    {
        uint32_t Guid = 0;
        uint32_t Count = 0;
        uint32_t Latest = 0;
        std::vector<RecordRules::Kind> Kinds;
    };
    std::vector<Met> met;
    for (RecordRules::Row const& row : rows)
    {
        if (RecordRules::Expired(row, now))
            continue;
        for (Token const& token : Tokens(row.Others))
        {
            if (token.Type != 'b' || token.Id == self)
                continue;
            auto found = std::find_if(met.begin(), met.end(), [&token](Met const& m) { return m.Guid == token.Id; });
            if (found == met.end())
                found = met.insert(met.end(), Met{token.Id, 0, 0, {}});
            ++found->Count;
            found->Latest = std::max(found->Latest, row.Time);
            if (std::find(found->Kinds.begin(), found->Kinds.end(), row.EventKind) == found->Kinds.end())
                found->Kinds.push_back(row.EventKind);
        }
    }
    std::sort(met.begin(), met.end(),
              [](Met const& a, Met const& b)
              {
                  if (a.Count != b.Count)
                      return a.Count > b.Count;
                  return a.Latest != b.Latest ? a.Latest > b.Latest : a.Guid < b.Guid;
              });

    std::vector<Entry> entries;
    for (Met& m : met)
    {
        if (entries.size() >= KNOWN_LIMIT)
            break;
        std::optional<Person> const person = lookup.Character(m.Guid);
        if (!person || person->Name.empty() || HasDigit(person->Name))
            continue;

        std::sort(m.Kinds.begin(), m.Kinds.end());
        std::string shared = SharedPhrase(m.Kinds.front());
        if (m.Kinds.size() > 1 && std::string_view(SharedPhrase(m.Kinds[1])) != shared)
            shared += std::string(" and ") + SharedPhrase(m.Kinds[1]);
        std::string const name = Cut(person->Name, MAX_NAME);
        std::string const people = PeopleWords(*person);
        std::string const text =
            people.empty() ? name + " " + shared + "." : name + ", " + people + ", " + shared + ".";
        entries.push_back({0, name, Cut(text, MAX_SENTENCE), Age(m.Latest, now) / SECONDS_PER_DAY, m.Count});
    }

    return entries;
}
}  // namespace RecordWords

#endif
