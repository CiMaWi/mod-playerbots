/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the record's readers (living-conversation LC7, RecordWords.h): a sentence for every kind without a
// digit or an id and within the wire's text cap even with the longest names, names with a digit replaced by a plain
// word, the permanent rows in time order (the strongest when over the bound) and the world the bot knows, the recent
// deeds newest first without the faded ones, the known bots once each with their counts, and every limit.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot tests/bot-record-readers-selftest.cpp

#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "RecordWords.h"

namespace
{
using RecordRules::Kind;
using RecordRules::Row;
using namespace RecordWords;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

constexpr uint32_t SELF = 7;
constexpr uint32_t NOW = 2000000000;

// Names by id; long = every name 64 bytes or more, digits = every name carries a digit
class FakeLookup final : public Lookup
{
public:
    bool Long = false;
    bool Digits = false;
    bool Anyone = false;  // Every guid is a character

    std::string Name(char const* plain) const
    {
        if (Digits)
            return std::string(plain) + " 2";
        return Long ? std::string(90, 'x') : std::string(plain);
    }

    std::string PlaceName(uint32_t area, uint32_t zone) const override
    {
        return area ? Name("Northshire Valley") : zone ? Name("Elwynn Forest") : "";
    }
    std::string ZoneName(uint32_t zone) const override
    {
        if (Anyone && zone != 12 && zone != 40)
            return std::string(1, static_cast<char>('A' + zone % 26)) + "land";
        return zone == 12     ? Name("Elwynn Forest")
               : zone == 40   ? Name("Westfall")
               : zone == 1581 ? Name("The Deadmines")
                              : "";
    }
    std::string CreatureName(uint32_t entry) const override
    {
        return entry == 448 ? Name("Hogger") : entry == 639 ? Name("Edwin VanCleef") : entry ? Name("Kobold") : "";
    }
    bool Boss(uint32_t entry) const override { return entry == 639; }
    std::string QuestTitle(uint32_t id) const override { return id ? Name("A Threat Within") : ""; }
    std::string ItemName(uint32_t id) const override { return id ? Name("Ironwood Treebranch") : ""; }
    std::string SpellName(uint32_t id) const override { return id ? Name("Apprentice Riding") : ""; }
    std::string SkillName(uint32_t id) const override { return id ? Name("Mining") : ""; }
    std::string FlightPointName(uint32_t node) const override { return node ? Name("Sentinel Hill, Westfall") : ""; }
    std::string DungeonName(uint32_t map) const override { return map == 36 ? Name("The Deadmines") : ""; }
    std::optional<Person> Character(uint32_t guid) const override
    {
        static std::map<uint32_t, Person> const people = {{SELF, {"Self", 1, 1}},
                                                          {11, {"Aldric", 1, 2}},
                                                          {12, {"Brug", 2, 1}},
                                                          {13, {"Ceyla", 5, 8}},
                                                          {14, {"Nameless", 0, 0}}};
        auto const found = people.find(guid);
        if (Anyone && found == people.end())
            return Person{std::string(1, static_cast<char>('A' + guid % 26)) + "bot", 3, 4};
        if (found == people.end())
            return std::nullopt;
        Person person = found->second;
        if (Long)
            person.Name = std::string(90, 'y');
        if (Digits)
            person.Name += "9";
        return person;
    }
};

Row RowOf(Kind kind, uint32_t time, uint8_t score, std::string facts, std::string others = "", uint32_t zone = 12,
          uint32_t eventId = 0)
{
    Row row;
    row.EventId = eventId ? eventId : time % 100000;
    row.EventKind = kind;
    row.Time = time;
    row.Map = 0;
    row.Zone = zone;
    row.Area = 9;
    row.Score = score;
    row.Others = std::move(others);
    row.Facts = std::move(facts);
    row.Expires = RecordRules::Expires(score, time);
    return row;
}

// One row of every kind, as the recorder writes them (BotRecordMgr.cpp OnEvent, OnFightEnd)
std::vector<Row> EveryKind()
{
    uint32_t const t = NOW - 600;
    return {RowOf(Kind::Kill, t, 40, "entry=448;rank=1;above=4;", "n448 b11 p30 b7"),
            RowOf(Kind::Quest, t, 35, "quest=783;"),
            RowOf(Kind::Equip, t, 25, "item=19121;quality=3;gain=6;"),
            RowOf(Kind::Level, t, 70, "level=30;"),
            RowOf(Kind::Skill, t, 65, "skill=186;value=150;"),
            RowOf(Kind::Spell, t, 65, "spell=33388;"),
            RowOf(Kind::Flight, t, 25, "node=2;"),
            RowOf(Kind::Death, t, 30, "by=creature;", "n448"),
            RowOf(Kind::Resurrect, t, 25, "how=spirit_healer;"),
            RowOf(Kind::Fight, t, 40, "fight=123;cause=attack;seconds=40;", "b11 p30"),
            RowOf(Kind::Dungeon, t, 30, "map=36;"),
            RowOf(Kind::Group, t, 60, "", "b12"),
            RowOf(Kind::Stay, t, 60, "rows=5;"),
            RowOf(Kind::DungeonRun, t, 70, "map=36;", "b11"),
            RowOf(Kind::Guild, t, 60, ""),
            RowOf(Kind::Duel, t, 30, "", "b13")};
}

bool Starts(std::string const& text, char const* stem) { return text.rfind(stem, 0) == 0; }

void Sentences()
{
    FakeLookup names;
    std::vector<Row> const rows = EveryKind();
    Check(rows.size() == RecordRules::KIND_COUNT, "the fixture holds one row of every kind");
    std::set<std::string> seen;
    bool all = true;
    for (Row const& row : rows)
    {
        std::string const text = Sentence(row, SELF, names);
        all = all && !text.empty() && !HasDigit(text) && text.size() <= MAX_SENTENCE && text.back() == '.' &&
              Starts(text, "You ");
        seen.insert(text);
    }
    Check(all, "every kind has a sentence: no digit, within the cap, a full stop, said to the bot");
    Check(seen.size() == rows.size(), "every kind has its own sentence");
    Check(Sentence(rows[0], SELF, names) == "You slew Hogger, an elite foe, in Northshire Valley.",
          "a kill names the creature, its rank and the place");
    Check(Sentence(RowOf(Kind::Kill, NOW, 25, "entry=448;rank=0;above=3;"), SELF, names) ==
              "You slew Hogger, a foe stronger than you, in Northshire Valley.",
          "a hard normal kill says the foe was stronger");
    Check(Sentence(RowOf(Kind::Kill, NOW, 70, "entry=448;rank=3;above=0;"), SELF, names) ==
              "You slew Hogger, a fearsome foe, in Northshire Valley.",
          "a world boss is a fearsome foe");
    Check(Sentence(rows[1], SELF, names) == "You finished the quest A Threat Within in Northshire Valley.",
          "a quest names its title");
    Check(Sentence(rows[3], SELF, names) == "You reached a milestone of experience in Northshire Valley." &&
              Sentence(RowOf(Kind::Level, NOW, 25, "level=13;"), SELF, names) ==
                  "You grew in experience in Northshire Valley.",
          "a level says milestone or growth, never the level");
    Check(Sentence(rows[4], SELF, names) == "You mastered journeyman Mining in Northshire Valley.",
          "a craft rank names the rank in words");
    Check(Sentence(rows[7], SELF, names) == "You died to Hogger in Northshire Valley.", "a death names its creature");
    Check(Sentence(RowOf(Kind::Death, NOW, 30, "by=player;", "b12"), SELF, names) ==
              "You were slain by Brug in Northshire Valley.",
          "a death by a player names the player");
    Check(Sentence(RowOf(Kind::Death, NOW, 30, "by=environment;"), SELF, names) == "You died in Northshire Valley.",
          "a death without a killer names the place");
    Check(Sentence(rows[9], SELF, names) == "You fought beside Aldric in Northshire Valley.",
          "a fight names a companion bot");
    Check(Sentence(RowOf(Kind::Fight, NOW, 40, "fight=1;", "b7 n448"), SELF, names) ==
              "You fought Hogger in Northshire Valley.",
          "a fight names its creature, never the bot itself as companion");
    Check(Sentence(rows[10], SELF, names) == "You entered The Deadmines." &&
              Sentence(rows[13], SELF, names) == "You cleared The Deadmines.",
          "a dungeon names the dungeon");
    Check(Sentence(rows[12], SELF, names) == "You spent a long while in Elwynn Forest.", "a stay names its zone");

    FakeLookup unknown;
    Row bare = RowOf(Kind::Kill, NOW, 40, "rank=1;");
    bare.Area = 0;
    bare.Zone = 0;
    Check(Sentence(bare, SELF, unknown) == "You slew a foe in the wilds.", "unknown names read as plain words");

    FakeLookup longNames;
    longNames.Long = true;
    bool fits = true;
    for (Row const& row : rows)
    {
        std::string const text = Sentence(row, SELF, longNames);
        fits = fits && text.size() <= MAX_SENTENCE && text.back() == '.';
    }
    Check(fits, "every sentence fits the cap whole with the longest names (cut at the name cap)");

    FakeLookup digits;
    digits.Digits = true;
    bool clean = true;
    for (Row const& row : rows)
        clean = clean && !HasDigit(Sentence(row, SELF, digits));
    Check(clean, "a name with a digit never reaches a sentence");
    Check(
        Cut("ab\xC3\xA9", 3) == "ab" && Cut("abc", 3) == "abc" && NameOr(std::string(70, 'z'), "x").size() == MAX_NAME,
        "names are cut at the cap on a character boundary");
    Check(
        Tokens("n448 b11 p30 x9 b nope b12x").size() == 3 && Tokens("b11")[0].Type == 'b' && Tokens("b11")[0].Id == 11,
        "others tokens: bots, players and creatures, malformed ones skipped");
    Check(PeopleWords({"A", 1, 1}) == "a human warrior" && PeopleWords({"B", 2, 0}) == "an orc" &&
              PeopleWords({"C", 5, 8}) == "an undead mage" && PeopleWords({"D", 0, 0}).empty(),
          "people and calling in words with their article");
}

void Readers()
{
    FakeLookup names;
    // History: permanent rows in time order, recent ones left out
    std::vector<Row> rows = {RowOf(Kind::Spell, NOW - 3 * SECONDS_PER_DAY, 65, "spell=33388;"),
                             RowOf(Kind::Level, NOW - 9 * SECONDS_PER_DAY, 70, "level=10;"),
                             RowOf(Kind::Kill, NOW - 100, 40, "entry=448;rank=1;above=0;", "n448"),
                             RowOf(Kind::Kill, NOW - 50, 70, "entry=639;rank=3;", "n639", 40),
                             RowOf(Kind::Dungeon, NOW - 200, 30, "map=36;", "", 1581),
                             RowOf(Kind::Kill, NOW - 150, 40, "entry=1;rank=1;above=0;", "n1", 1581)};
    rows[4].Map = 36;
    rows[5].Map = 36;
    std::vector<Entry> history = History(rows, SELF, NOW, names);
    std::vector<Entry> events;
    for (Entry const& entry : history)
        if (entry.Code == static_cast<uint8_t>(HistoryRow::Event))
            events.push_back(entry);
    Check(events.size() == 3 && events[0].Name == "level" && events[1].Name == "spell" && events[2].Name == "kill",
          "history: the permanent rows only, oldest first");
    Check(events[0].Value == 9 && events[1].Value == 3 && events[2].Value == 0, "history: value is days ago");
    bool boss = false;
    bool dungeon = false;
    bool zones = false;
    bool dungeonZone = false;
    for (Entry const& entry : history)
    {
        dungeonZone =
            dungeonZone || (entry.Code == static_cast<uint8_t>(HistoryRow::Zone) && entry.Name == "The Deadmines");
        boss = boss || (entry.Code == static_cast<uint8_t>(HistoryRow::Boss) && entry.Name == "Edwin VanCleef");
        dungeon = dungeon || (entry.Code == static_cast<uint8_t>(HistoryRow::Dungeon) && entry.Name == "The Deadmines");
        zones = zones ||
                (entry.Code == static_cast<uint8_t>(HistoryRow::Zone) && entry.Name == "Westfall" && entry.Value == 1);
    }
    Check(boss && dungeon && zones, "history: the bosses, dungeons and zones the bot knows, with their counts");
    Check(!dungeonZone, "history: a dungeon is known once, as a dungeon and not also as a zone");
    Check(history.size() == 3 + 4, "history: one knowledge entry per boss, dungeon and zone");
    bool plain = true;
    for (Entry const& entry : history)
        plain = plain && !HasDigit(entry.Text) && !entry.Text.empty();
    Check(plain, "history: every entry has a sentence without a digit");

    // History bounds: the strongest permanent rows (then the newest), shown in time order, within the limit
    std::vector<Row> many;
    for (uint32_t index = 0; index < 4; ++index)
        many.push_back(RowOf(Kind::Spell, NOW - (100 - index) * SECONDS_PER_DAY, 90, "spell=1;", "", 12, index + 1));
    for (uint32_t index = 0; index < 20; ++index)
        many.push_back(
            RowOf(Kind::Level, NOW - (20 - index) * SECONDS_PER_DAY, 61, "level=13;", "", 100 + index, index + 10));
    FakeLookup lands;
    lands.Anyone = true;
    std::vector<Entry> const bounded = History(many, SELF, NOW, lands);
    std::vector<Entry> kept;
    for (Entry const& entry : bounded)
        if (entry.Code == static_cast<uint8_t>(HistoryRow::Event))
            kept.push_back(entry);
    Check(bounded.size() == HISTORY_LIMIT && kept.size() == HISTORY_EVENTS, "history: held within its limits");
    Check(kept[0].Name == "spell" && kept[3].Name == "spell" && kept[0].Value == 100 && kept[4].Name == "level" &&
              kept[4].Value == 12 && kept.back().Value == 1,
          "history: the strongest rows, then the newest, shown oldest first");

    // Lately: recent rows not past expiry, newest first, value seconds ago
    std::vector<Row> recent = {RowOf(Kind::Quest, NOW - 300, 35, "quest=783;"),
                               RowOf(Kind::Kill, NOW - 30, 25, "entry=448;rank=0;above=4;", "n448"),
                               RowOf(Kind::Level, NOW - 60, 70, "level=20;"),
                               RowOf(Kind::Death, NOW - 400000, 20, "by=creature;", "n448")};
    std::vector<Entry> const lately = Lately(recent, SELF, NOW, names);
    Check(lately.size() == 2 && lately[0].Name == "kill" && lately[1].Name == "quest",
          "lately: the recent deeds not past expiry, newest first (permanent rows are history's)");
    Check(lately[0].Value == 30 && lately[1].Value == 300, "lately: value is seconds ago");
    std::vector<Row> crowd;
    for (uint32_t index = 0; index < 30; ++index)
        crowd.push_back(RowOf(Kind::Quest, NOW - 1000 + index * 10, 35, "quest=1;", "", 12, index + 1));
    std::vector<Entry> const capped = Lately(crowd, SELF, NOW, names);
    Check(capped.size() == LATELY_LIMIT && capped.front().Value == 710, "lately: held at its limit, newest kept");
    std::vector<std::string> const deeds = Deeds(recent, SELF, NOW, names);
    Check(deeds.size() == 2 && deeds[0] == lately[0].Text && Deeds(crowd, SELF, NOW, names).size() == STATE_DEEDS,
          "the state's deeds: the newest recent sentences, at most three");

    // Known: each bot once with the events shared, the bot itself and players left out
    std::vector<Row> shared = {RowOf(Kind::Kill, NOW - 100, 40, "entry=448;", "n448 b11 p30 b7"),
                               RowOf(Kind::Fight, NOW - 50, 40, "fight=3;", "b11 b12"),
                               RowOf(Kind::Fight, NOW - 2 * SECONDS_PER_DAY, 65, "fight=4;", "b13"),
                               RowOf(Kind::Kill, NOW - 20, 40, "entry=448;", "b99 b14"),
                               RowOf(Kind::Death, NOW - 400000, 20, "by=player;", "b12")};
    std::vector<Entry> const known = KnownBots(shared, SELF, NOW, names);
    std::set<std::string> unique;
    for (Entry const& entry : known)
        unique.insert(entry.Name);
    Check(unique.size() == known.size() && known.size() == 4, "known: each bot once, an unknown guid left out");
    Check(known[0].Name == "Aldric" && known[0].Extra == 2, "known: the most shared first, with its count");
    Check(known[0].Text == "Aldric, a human paladin, slew foes with you and fought beside you.",
          "known: people, calling and what was shared");
    Check(known[1].Name == "Nameless" && known[1].Text == "Nameless slew foes with you.",
          "known: then the latest; a bot without people words is named plainly");
    Check(known[3].Name == "Ceyla" && known[3].Value == 2, "known: value is days since the last shared event");
    bool notSelf = true;
    for (Entry const& entry : known)
        notSelf = notSelf && entry.Name != "Self" && !HasDigit(entry.Text);
    Check(notSelf, "known: never the bot itself, no digit");
    std::vector<Row> crowdKnown;
    for (uint32_t index = 0; index < 20; ++index)
        crowdKnown.push_back(RowOf(Kind::Fight, NOW - index, 40, "fight=1;", "b11 b12 b13 b14"));
    Check(KnownBots(crowdKnown, SELF, NOW, names).size() == 4 && KnownBots(crowdKnown, SELF, NOW, names)[0].Extra == 20,
          "known: counts every shared row");
    std::vector<Row> crowdMany;
    FakeLookup everyone;
    everyone.Anyone = true;
    for (uint32_t index = 0; index < 20; ++index)
        crowdMany.push_back(RowOf(Kind::Fight, NOW - index, 40, "fight=1;", "b" + std::to_string(100 + index)));
    std::vector<Entry> const crowded = KnownBots(crowdMany, SELF, NOW, everyone);
    Check(crowded.size() == KNOWN_LIMIT && crowded[0].Text == "Wbot, a dwarf rogue, fought beside you.",
          "known: held at its limit, the latest first");
}
}  // namespace

int main()
{
    Sentences();
    Readers();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
