/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the census event log's Player-free part (src/Bot/Debug/EventLogFormat.h): the line format the
// hub's lab-ledger.js parses, the ledger's classification of every type name, the source words, the thread-local
// source scope, and four threads queueing lines the way CensusMgr::Enqueue does. Optional argument: a file path that
// receives one sample line per type (for a cross-check through lab-ledger.js readEvents).
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "EventLogFormat.h"

using EventLog::FormatEventLine;
using EventLog::SourceScope;
using EventLog::SourceWordOf;

namespace
{
constexpr char const* TIME = "2026-11-08 12:00:00+00";
constexpr size_t COLUMNS = 8;
constexpr int THREADS = 4;
constexpr int LINES_PER_THREAD = 20000;

// tools/lab-ledger.js:36-37, checked in this order (honesty first)
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

std::vector<std::string> Split(std::string const& line)
{
    std::vector<std::string> fields;
    std::string field;
    std::istringstream in(line);
    while (std::getline(in, field, ','))
        fields.push_back(field);

    return fields;
}

std::string Classify(std::string const& type)
{
    if (std::regex_search(type, HONESTY_RE))
        return "honesty";

    return std::regex_search(type, ECONOMY_RE) ? "economy" : "none";
}

void CheckFormat()
{
    std::string const line = FormatEventLine(TIME, 42, "GoldIn", "loot", 0, 150, 1, 12);
    Report(line == "2026-11-08 12:00:00+00,42,GoldIn,loot,0,150,1,12\n", "column order: " + line);

    std::string const negative = FormatEventLine(TIME, 7, "GoldOut", "repair", 0, -3200, 0, 1519);
    std::vector<std::string> const fields = Split(negative.substr(0, negative.size() - 1));
    Report(fields.size() == COLUMNS, "eight columns");
    Report(fields.size() == COLUMNS && fields[2] == "GoldOut" && fields[3] == "repair" && fields[5] == "-3200",
           "type, source and amount at the ledger's columns 2, 3 and 5");

    std::string const dirty = FormatEventLine(TIME, 1, "ItemIn", "a,b\"c\nd\re", 2589, 4, 0, 12);
    std::vector<std::string> const dirtyFields = Split(dirty.substr(0, dirty.size() - 1));
    Report(dirtyFields.size() == COLUMNS && dirtyFields[3] == "a_b_c_d_e", "a source cannot split the line: " + dirty);
    Report(dirty.find('\n') == dirty.size() - 1, "one row per line");

    std::string const empty = FormatEventLine(TIME, 1, "DeathReturn", "", 0, 1, 0, 12);
    Report(Split(empty)[3] == EventLog::SOURCE_OTHER, "an empty source is written as other");
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

// The CensusMgr::Enqueue pattern: one short lock per line, the line pushed and its type counted together
struct FakeQueue
{
    std::mutex mutex;
    std::vector<std::string> lines;
    std::map<std::string, uint64_t, std::less<>> totals;

    void Enqueue(std::string text, std::string_view type)
    {
        std::lock_guard<std::mutex> guard(mutex);
        lines.push_back(std::move(text));
        auto const found = totals.find(type);
        if (found != totals.end())
            ++found->second;
        else
            totals.emplace(std::string(type), 1);
    }
};

void CheckConcurrentEnqueue()
{
    FakeQueue queue;
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
        threads.emplace_back(
            [&queue, t]
            {
                for (int i = 0; i < LINES_PER_THREAD; ++i)
                {
                    SourceScope const scope(i % 2 ? "loot" : "vendor");
                    std::string const& type = ECONOMY_TYPES[static_cast<size_t>(i) % ECONOMY_TYPES.size()];
                    char const* source = EventLog::currentSourceState.source;
                    queue.Enqueue(FormatEventLine(TIME, static_cast<uint64_t>(t), type, source,
                                                  static_cast<uint32_t>(i), i, 0, 12),
                                  type);
                }
            });

    for (std::thread& thread : threads)
        thread.join();

    size_t const total = static_cast<size_t>(THREADS) * LINES_PER_THREAD;
    Report(queue.lines.size() == total, "no line lost: " + std::to_string(queue.lines.size()));

    uint64_t counted = 0;
    for (auto const& [type, count] : queue.totals)
        counted += count;
    Report(counted == total, "every line counted once");

    std::vector<int> perThread(THREADS, 0);
    bool wellFormed = true;
    for (std::string const& line : queue.lines)
    {
        std::vector<std::string> const fields = Split(line.substr(0, line.size() - 1));
        if (fields.size() != COLUMNS || line.back() != '\n')
        {
            wellFormed = false;
            continue;
        }
        ++perThread[std::stoi(fields[1])];
    }
    Report(wellFormed, "every queued line has eight columns");
    for (int t = 0; t < THREADS; ++t)
        Report(perThread[t] == LINES_PER_THREAD, "thread " + std::to_string(t) + " lines complete");
}

void WriteSample(char const* path)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
    int amount = 1;
    for (std::string const& type : ECONOMY_TYPES)
        out << FormatEventLine(TIME, 42, type, "loot", 0, amount++, 0, 12);

    for (std::string const& kind : HONESTY_KINDS)
        out << FormatEventLine(TIME, 42, std::string("Honesty") + kind, "factory", 0, amount++, 0, 12);
}
}  // namespace

int main(int argc, char** argv)
{
    CheckFormat();
    CheckClassification();
    CheckSourceWords();
    CheckScopes();
    CheckConcurrentEnqueue();

    if (argc > 1)
        WriteSample(argv[1]);

    std::cout << "event-log-selftest: " << passes << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
