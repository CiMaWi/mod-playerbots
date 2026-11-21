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
#include "EventWanted.h"

// The three readers' on/off states the gates read (the game's atomics), set by CheckGates
namespace
{
bool gateLogOn = false;
bool gateRecordOn = false;
bool gateStartersOnline = false;
}  // namespace

bool WorldLog::Active() { return gateLogOn; }
bool BotRecord::Active() { return gateRecordOn; }
bool Starters::Online() { return gateStartersOnline; }

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

// CS31d: a lesson's price goes to its first learn, the later ones inside it are paid and free, nothing outside
void CheckLessonNote()
{
    using EventLog::CloseLesson;
    using EventLog::LessonScope;
    using EventLog::OpenLesson;
    using EventLog::TakeLessonCost;
    int bot = 0;
    int other = 0;
    uint32_t cost = 99;

    Report(!TakeLessonCost(&bot, cost) && cost == 0, "no lesson: not paid, cost 0");
    OpenLesson(&bot, 4500);
    Report(!TakeLessonCost(&other, cost) && cost == 0, "another player's learn: not paid");
    Report(TakeLessonCost(&bot, cost) && cost == 4500, "first learn: paid, the price");
    Report(TakeLessonCost(&bot, cost) && cost == 0, "second learn: paid, free");
    CloseLesson();
    Report(!TakeLessonCost(&bot, cost) && cost == 0, "after the lesson: not paid");
    OpenLesson(&bot, 0);
    Report(!TakeLessonCost(&bot, cost), "a free purchase: not paid");
    Report(!TakeLessonCost(nullptr, cost), "null player: not paid");
    {
        LessonScope const lesson(&bot, 100);
        std::thread worker(
            [&bot]
            {
                uint32_t elsewhere = 7;
                Report(!TakeLessonCost(&bot, elsewhere) && elsewhere == 0, "lessons are per thread");
            });
        worker.join();
        Report(TakeLessonCost(&bot, cost) && cost == 100, "a scoped lesson: paid");
    }
    Report(!TakeLessonCost(&bot, cost), "the scope closes the lesson");
}

// CS31d: a swap's replaced levels answer the equips of the items it moved into their slots, once each
void CheckSwapNote()
{
    using EventLog::NoteSwap;
    using EventLog::SwapSlot;
    using EventLog::TakeReplacedLevel;
    int ringA = 0;
    int ringB = 0;
    int helm = 0;
    uint32_t level = 99;

    Report(!TakeReplacedLevel(&helm, 0, level) && level == 0, "no swap: an empty slot");
    NoteSwap(SwapSlot{&helm, 0, 60}, SwapSlot{});
    Report(!TakeReplacedLevel(&helm, 1, level), "another slot: not the swap's");
    Report(TakeReplacedLevel(&helm, 0, level) && level == 60, "the replaced item's level");
    Report(!TakeReplacedLevel(&helm, 0, level), "a swap slot answers once");
    NoteSwap(SwapSlot{&ringA, 11, 100}, SwapSlot{&ringB, 10, 120});
    Report(TakeReplacedLevel(&ringB, 10, level) && level == 120, "a two-slot swap: the source slot");
    Report(TakeReplacedLevel(&ringA, 11, level) && level == 100, "a two-slot swap: the destination slot");
    NoteSwap(SwapSlot{&helm, 0, 60}, SwapSlot{});
    Report(!TakeReplacedLevel(&ringA, 0, level), "another item into the slot: not the swap's");
    Report(!TakeReplacedLevel(nullptr, 0, level), "null item: an empty slot");
    NoteSwap(SwapSlot{}, SwapSlot{});
    Report(!TakeReplacedLevel(&helm, 0, level), "a bag swap clears the note");
    {
        EventLog::ReplaceScope const replacing(14, 6);
        Report(!TakeReplacedLevel(&helm, 0, level), "a replacement: another slot is empty");
        Report(TakeReplacedLevel(&ringA, 14, level) && level == 6, "a replacement: any new item, the old level");
        Report(!TakeReplacedLevel(&ringB, 14, level), "a replacement answers once");
    }
    {
        EventLog::ReplaceScope const unused(14, 6);
    }
    Report(!TakeReplacedLevel(&helm, 14, level), "the replacement scope clears the note");
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
    Report(FromWord("Fought") == EventType::Unknown && FromWord("") == EventType::Unknown &&
               FromWord("kill") == EventType::Unknown && FromWord("Record") == EventType::Record &&
               !ReadBy(EventType::Record, EventLog::READ_RECORD) && !ReadBy(EventType::Starter, EventLog::READ_STARTER),
           "type table: words the table lacks are Unknown; the record's and the starters' own rows feed no one back");
}

// CS30e: the fact hooks' gates Wanted(type) against the gates they replaced (WorldLog::Active() and Wanted()), over
// every on/off state of the world log, the record and the starters; with the log off and the record on, the paid
// lesson's debit note (Wanted(SpellLearn)) is taken and the stored item note (Wanted(ItemIn)) is not, as before
void CheckGates()
{
    using EventLog::EventType;
    using EventLog::Wanted;

    // Types whose hooks or notes were gated by WorldLog::Active() (the debit note is the named change, apart)
    EventType const logGated[] = {EventType::XpGain, EventType::QuestAccept, EventType::Hearth, EventType::ItemIn,
                                  EventType::GoldIn, EventType::GoldOut,     EventType::Unknown};
    // Types whose hooks were gated by Wanted() (the world log or the record runs)
    EventType const anyGated[] = {EventType::Kill,       EventType::Equip,    EventType::SkillUp,
                                  EventType::SpellLearn, EventType::TaxiNode, EventType::MapChange,
                                  EventType::Resurrect,  EventType::Level};
    bool sameAsLog = true;
    bool sameAsAny = true;
    for (int state = 0; state < 8; ++state)
    {
        gateLogOn = state & 1;
        gateRecordOn = state & 2;
        gateStartersOnline = state & 4;
        for (EventType const type : logGated)
            sameAsLog = sameAsLog && Wanted(type) == gateLogOn;
        for (EventLog::HonestyRow const& kind : EventLog::HONESTY_KINDS)
            sameAsLog = sameAsLog && Wanted(EventLog::TypeOf(kind.Kind)) == gateLogOn;
        // A real player online with the record off differs (the starters read Kill, Equip, Level): the record is
        // loaded at every startup, so that state does not occur after the load
        if (!gateStartersOnline || gateRecordOn)
            for (EventType const type : anyGated)
                sameAsAny = sameAsAny && Wanted(type) == Wanted();
    }
    Report(sameAsLog, "gates: the types gated by the world log alone keep that gate");
    Report(sameAsAny, "gates: the types gated by the log or the record keep that gate");

    gateLogOn = false;
    gateRecordOn = true;
    gateStartersOnline = false;
    Report(Wanted(EventType::SpellLearn) && !Wanted(EventType::ItemIn),
           "gates: log off, record on: the debit note is taken, the stored item note is not");
    gateRecordOn = false;
    Report(!Wanted(EventType::SpellLearn) && !Wanted(), "gates: log and record off: no note, no fact");
}

}  // namespace

int main()
{
    CheckFields();
    CheckClassification();
    CheckSourceWords();
    CheckScopes();
    CheckStoredItemNote();
    CheckLessonNote();
    CheckSwapNote();
    CheckTypes();
    CheckGates();

    std::cout << "event-log-selftest: " << passes << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
