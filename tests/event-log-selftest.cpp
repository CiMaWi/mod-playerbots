/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the event log's Player-free part (src/Bot/Debug/EventLogFormat.h): field cleaning, the hub
// ledger's classification of every type name, the source words, the thread-local source scope and the StoreNewItem
// note.
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "EventLogFormat.h"

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
std::vector<std::string> const ECONOMY_TYPES = {"GoldIn",      "GoldOut",   "ItemIn",  "ItemOut", "Death",
                                                "DeathReturn", "QuestDone", "PvpKill", "PvpDeath"};
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

}  // namespace

int main()
{
    CheckFields();
    CheckClassification();
    CheckSourceWords();
    CheckScopes();
    CheckStoredItemNote();

    std::cout << "event-log-selftest: " << passes << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
