/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the event log's Player-free part (src/Bot/Debug/EventLogFormat.h): field cleaning, the hub
// ledger's classification of every type name, the source words, the thread-local source scope and the StoreNewItem
// note; the event type table (EventTypes.h): every word once, word to type to word, every reader's types are rows.
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "EventLog.h"
#include "EventLogFormat.h"
#include "EventTypes.h"

using EventLog::SourceScope;
using EventLog::SourceWordOf;

namespace
{
// tools/lab-ledger.js:42-43, checked in this order (honesty first)
std::regex const HONESTY_RE(
    "honest|cheat|grant|teleport|resurrect|free.?repair|undiscovered|no.?item|factory|maintenance",
    std::regex::ECMAScript | std::regex::icase);
std::regex const ECONOMY_RE(
    "gold|money|copper|item|loot|quest|vendor|repair|train|flight|mail|auction|trade|sold|death|pvp",
    std::regex::ECMAScript | std::regex::icase);

// The economy and life types L9a emits, and the honesty kinds L9b tags (type = "Honesty" + kind)
std::vector<std::string> const ECONOMY_TYPES = {"GoldIn", "GoldOut",   "ItemIn",  "ItemOut",
                                                "Death",  "QuestDone", "PvpKill", "PvpDeath"};
std::vector<std::string> const HONESTY_KINDS = {"Teleport", "Item",      "Gold",       "Spell",  "Skill",
                                                "Talent",   "Resurrect", "FreeRepair", "Flight", "FoodNoItem"};

int failures = 0;
int passes = 0;

void Report(bool ok, std::string const& what)
{
    if (ok)
    {
        ++passes;
        return;
    }

    if (++failures <= 20)
        std::cout << "FAIL " << what.substr(0, 200) << "\n";
}

std::string Classify(std::string const& type)
{
    if (std::regex_search(type, HONESTY_RE))
        return "honesty";

    return std::regex_search(type, ECONOMY_RE) ? "economy" : "none";
}

// A field cannot split a row: separators, quotes and line ends become '_'
void CheckFields()
{
    std::string out;
    EventLog::AppendField(out, "a,b\"c\nd\re");
    Report(out == "a_b_c_d_e", "a field cannot split the row: " + out);
}

void CheckClassification()
{
    for (std::string const& type : ECONOMY_TYPES)
        Report(Classify(type) == "economy", "economy type " + type + " is " + Classify(type));

    for (std::string const& kind : HONESTY_KINDS)
        Report(Classify(std::string("Honesty") + kind) == "honesty", "honesty type Honesty" + kind);

    Report(Classify("LevelUp") == "none", "LevelUp stays unclassified, as today");
}

void CheckSourceWords()
{
    std::map<std::string, std::string> const expected = {
        {"store loot", "loot"},
        {"loot roll", "loot"},
        {"talk to quest giver", "quest"},
        {"rpg end quest", "quest"},
        {"reward", "quest"},
        {"repair", "repair"},
        {"rpg repair", "repair"},
        {"sell", "vendor"},
        {"rpg sell", "vendor"},
        {"buy", "vendor"},
        {"rpg buy petition", "vendor"},
        {"trainer", "train"},
        {"rpg train", "train"},
        {"taxi", "flight"},
        {"remember taxi", "flight"},
        {"new rpg travel flight", "flight"},
        {"check mail", "mail"},
        {"accept trade", "trade"},
        {"craft random item", "craft"},
        {"disenchant random item", "craft"},
        {"revive from corpse", "corpse"},
        {"spirit healer", "spirit_healer"},
        {"self resurrect", "self resurrect"},
        {"attack", "attack"},
        {"", "other"},
    };

    for (auto const& [name, word] : expected)
        Report(SourceWordOf(name) == word, "source word of '" + name + "' is " + std::string(SourceWordOf(name)));
}

void CheckScopes()
{
    using EventLog::currentSourceState;
    int fakeActionStorage = 0;
    Action* fakeAction = reinterpret_cast<Action*>(&fakeActionStorage);  // never dereferenced here

    Report(!currentSourceState.source && !currentSourceState.action, "no scope at start");
    {
        SourceScope const outer("factory");
        Report(currentSourceState.source == std::string_view("factory") && !currentSourceState.action, "outer literal");
        {
            SourceScope const action(fakeAction);
            Report(!currentSourceState.source && currentSourceState.action == fakeAction, "action scope inside");
            {
                SourceScope const inner("maintenance");
                Report(currentSourceState.source == std::string_view("maintenance"), "innermost literal");
            }
            Report(currentSourceState.action == fakeAction && !currentSourceState.source, "action restored");

            std::thread other([] { Report(!EventLog::currentSourceState.action, "scopes are per thread"); });
            other.join();
        }
        Report(currentSourceState.source == std::string_view("factory"), "outer restored");
    }
    Report(!currentSourceState.source && !currentSourceState.action, "nothing left after the last scope");
}

// The StoreNewItem note: the item hooks get the entry by pointer comparison only, once, on the noting thread
void CheckStoredItemNote()
{
    using EventLog::NoteStoredItem;
    using EventLog::TakeStoredEntry;
    int first = 0;
    int second = 0;

    Report(TakeStoredEntry(&first) == 0, "no note: entry 0");
    NoteStoredItem(&first, 2589);
    Report(TakeStoredEntry(&first) == 2589, "noted item: its entry");
    Report(TakeStoredEntry(&first) == 0, "a note answers once");
    NoteStoredItem(&first, 2589);
    Report(TakeStoredEntry(&second) == 0, "another item: entry 0");
    Report(TakeStoredEntry(&first) == 0, "a mismatch clears the note");
    NoteStoredItem(&first, 2589);
    Report(TakeStoredEntry(nullptr) == 0, "null item: entry 0");
    NoteStoredItem(&first, 2589);
    NoteStoredItem(&second, 4306);
    Report(TakeStoredEntry(&second) == 4306, "the last store wins");
    NoteStoredItem(&first, 2589);
    std::thread other([&first] { Report(TakeStoredEntry(&first) == 0, "notes are per thread"); });
    other.join();
    Report(TakeStoredEntry(&first) == 2589, "the noting thread keeps its note");
}

// CS30a: the type table is in enum order, every word is in it once and maps back to its type, and the readers'
// types (as the record, the starters and the recorder read them before CS30a) are rows with that reader
void CheckTypes()
{
    using EventLog::EventType;
    using EventLog::FromWord;
    using EventLog::ReadBy;
    using EventLog::TYPES;
    using EventLog::Word;

    std::map<std::string_view, int> seen;
    bool order = true;
    bool roundTrip = true;
    for (size_t index = 0; index < TYPES.size(); ++index)
    {
        EventLog::TypeRow const& row = TYPES[index];
        order = order && static_cast<size_t>(row.Type) == index;
        ++seen[row.Word];
        if (row.Type != EventType::Unknown)
            roundTrip = roundTrip && !row.Word.empty() && FromWord(row.Word) == row.Type &&
                        Word(FromWord(row.Word)) == row.Word;
    }
    Report(order, "type table: one row per type in enum order");
    bool once = true;
    for (auto const& [word, count] : seen)
        once = once && count == 1;
    Report(once && seen.count("") == 1 && Word(EventType::Unknown).empty(),
           "type table: every word once, only Unknown has none");
    Report(roundTrip, "type table: word to type to word for every row");

    char const* const recordWords[] = {"Kill",       "QuestDone", "Equip", "Level",     "SkillUp",
                                       "SpellLearn", "TaxiNode",  "Death", "Resurrect", "MapChange"};
    char const* const starterWords[] = {"Equip", "Kill", "QuestDone", "Level", "Visit"};
    // The recorder's types: five plain ones and a row per honesty kind
    char const* const recorderWords[] = {"Stuck", "Travel", "Kill", "XpGain", "Death"};
    auto const readersAre = [&](auto const& words, EventLog::ReaderMask reader, size_t extra = 0)
    {
        size_t marked = 0;
        for (EventLog::TypeRow const& row : TYPES)
            marked += ReadBy(row.Type, reader) ? 1 : 0;
        bool rows = marked == std::size(words) + extra;
        for (char const* const word : words)
            rows = rows && FromWord(word) != EventType::Unknown && ReadBy(FromWord(word), reader);
        return rows;
    };
    Report(readersAre(recordWords, EventLog::READ_RECORD), "type table: the record's 10 types are its rows");
    Report(readersAre(starterWords, EventLog::READ_STARTER), "type table: the starters' 5 types are their rows");
    bool honestyRows = FromWord("HonestyTeleport") == EventType::HonestyTeleport &&
                       FromWord("Honesty") == EventType::Unknown && FromWord("HonestyOther") == EventType::Unknown;
    for (EventLog::HonestyRow const& kind : EventLog::HONESTY_KINDS)
        honestyRows = honestyRows && ReadBy(EventLog::TypeOf(kind.Kind), EventLog::READ_RECORDER) &&
                      FromWord(Word(EventLog::TypeOf(kind.Kind))) == EventLog::TypeOf(kind.Kind) &&
                      std::string(Word(EventLog::TypeOf(kind.Kind))) == std::string("Honesty") + std::string(kind.Word);
    Report(readersAre(recorderWords, EventLog::READ_RECORDER, EventLog::HONESTY_KIND_COUNT) && honestyRows,
           "type table: the recorder's types are its rows; each honesty kind has its own row, and no bare Honesty");

    bool recorderOnRow = true;
    for (EventLog::TypeRow const& row : TYPES)
        recorderOnRow =
            recorderOnRow && (!ReadBy(row.Type, EventLog::READ_RECORDER) || ReadBy(row.Type, EventLog::READ_ROW));
    Report(recorderOnRow, "type table: the recorder reads only types that are rows");
    Report(!ReadBy(EventType::Visit, EventLog::READ_ROW) && ReadBy(EventType::Unknown, EventLog::READ_ROW) &&
               !ReadBy(EventType::Unknown, EventLog::READ_RECORD | EventLog::READ_STARTER | EventLog::READ_RECORDER),
           "type table: Visit is a starter key, never a row; an unknown word is a row read by no one");
    bool ledgerRows = true;
    for (std::string const& type : ECONOMY_TYPES)
        ledgerRows = ledgerRows && FromWord(type) != EventType::Unknown && Word(FromWord(type)) == type &&
                     ReadBy(FromWord(type), EventLog::READ_ROW);
    Report(ledgerRows, "type table: the hub ledger's economy and life types are rows");
    Report(FromWord("Fight") == EventType::Unknown && FromWord("") == EventType::Unknown &&
               FromWord("kill") == EventType::Unknown && FromWord("Record") == EventType::Record &&
               !ReadBy(EventType::Record, EventLog::READ_RECORD) && !ReadBy(EventType::Starter, EventLog::READ_STARTER),
           "type table: words the table lacks are Unknown; the record's and the starters' own rows feed no one back");
}

}  // namespace

int main()
{
    CheckFields();
    CheckClassification();
    CheckSourceWords();
    CheckScopes();
    CheckStoredItemNote();
    CheckTypes();

    std::cout << "event-log-selftest: " << passes << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
