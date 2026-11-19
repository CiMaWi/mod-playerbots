/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the bots' row store (src/Bot/BotRowStore.h) with fake policies that record their saves: which
// writes save and which do not, the order of one bot's saves, the forget registration, and concurrent readers beside
// one writer (every read sees a whole row).
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BotRowStore.h"

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

constexpr std::size_t RowCells = 8;
constexpr std::uint32_t ThreadedGuids = 4;
constexpr int ThreadedIterations = 20000;
constexpr int ReaderThreads = 4;

// Every cell holds the same value, so a torn read shows as two different cells
struct Row
{
    std::array<int, RowCells> cells{};

    static Row Of(int value)
    {
        Row row;
        row.cells.fill(value);
        return row;
    }

    bool Whole() const
    {
        for (int cell : cells)
            if (cell != cells[0])
                return false;

        return true;
    }
};

std::vector<std::pair<std::uint32_t, int>> saves;  // by the store, under its unique lock
std::vector<std::uint32_t> hooked;                 // the ForgetHook's guids

struct HookedPolicy
{
    static char const* Name() { return "hooked"; }
    static void Save(std::uint32_t guid, Row const& row) { saves.emplace_back(guid, row.cells[0]); }
    static void ForgetHook(std::uint32_t guid) { hooked.push_back(guid); }
};

struct PlainPolicy
{
    static char const* Name() { return "plain"; }
    static void Save(std::uint32_t guid, Row const& row) { saves.emplace_back(guid, row.cells[0]); }
};

std::atomic<int> threadedSaves{0};

struct ThreadedPolicy
{
    static char const* Name() { return "threaded"; }
    static void Save(std::uint32_t, Row const&) { ++threadedSaves; }
};

using Hooked = BotRowStore<Row, HookedPolicy>;
using Plain = BotRowStore<Row, PlainPolicy>;
using Threaded = BotRowStore<Row, ThreadedPolicy>;

// What BotStores.cpp would hold: the registered forgets by name
std::vector<std::pair<std::string, void (*)(std::uint32_t)>> forgets;

void (*FindForget(std::string const& name))(std::uint32_t)
{
    for (auto const& [known, forget] : forgets)
        if (known == name)
            return forget;

    return nullptr;
}

int ValueOf(Hooked& store, std::uint32_t guid)
{
    return store.Read(guid, -1, [](Row const& row) { return row.cells[0]; });
}

void Writes()
{
    Hooked& store = Hooked::Instance();
    saves.clear();

    Row const created = store.GetOrCreate(1, [] { return Row::Of(5); });
    Check(created.cells[0] == 5 && saves.size() == 1, "get-or-create stores the new row and saves once");
    Row const again = store.GetOrCreate(1, [] { return Row::Of(9); });
    Check(again.cells[0] == 5 && saves.size() == 1, "a second get-or-create returns the stored row and saves nothing");

    Check(!store.Update(2, [](Row& row) { row.cells.fill(7); }) && saves.size() == 1,
          "an update of a bot without a row gives none and saves nothing");
    Check(!store.Find(2) && store.Size() == 1, "an update of a bot without a row creates none");
    std::optional<Row> const updated = store.Update(1, [](Row& row) { row.cells.fill(6); });
    Check(updated && updated->cells[0] == 6 && saves.size() == 2 && ValueOf(store, 1) == 6,
          "an update changes the row, saves it and returns the copy");

    bool sawNone = false;
    Row const placed = store.Replace(3,
                                     [&sawNone](Row const* current)
                                     {
                                         sawNone = !current;
                                         return Row::Of(30);
                                     });
    Check(sawNone && placed.cells[0] == 30 && saves.size() == 3 && ValueOf(store, 3) == 30,
          "a replace of a bot without a row sees none, stores and saves");
    int seen = 0;
    store.Replace(3,
                  [&seen](Row const* current)
                  {
                      seen = current ? current->cells[0] : 0;
                      return Row::Of(seen + 1);
                  });
    Check(seen == 30 && ValueOf(store, 3) == 31 && saves.size() == 4, "a replace sees the current row and saves");

    store.LoadRow(4, Row::Of(40));
    store.LoadRow(4, Row::Of(41));
    Check(saves.size() == 4 && ValueOf(store, 4) == 41, "the load stores rows (the last wins) and saves nothing");

    Check(ValueOf(store, 99) == -1, "a read of a bot without a row gives the fallback");
    std::optional<Row> const found = store.Find(4);
    Check(found && found->Whole() && found->cells[0] == 41, "find gives a copy of the row");

    store.Erase(4);
    Check(!store.Find(4) && ValueOf(store, 4) == -1 && saves.size() == 4, "an erase drops the row and saves nothing");
}

void SaveOrder()
{
    Hooked& store = Hooked::Instance();
    saves.clear();
    constexpr std::uint32_t Guid = 7;
    store.GetOrCreate(Guid, [] { return Row::Of(1); });
    store.Update(Guid, [](Row& row) { row.cells.fill(2); });
    store.Replace(Guid, [](Row const*) { return Row::Of(3); });
    store.Update(8, [](Row& row) { row.cells.fill(99); });  // no row: no save
    store.Update(Guid, [](Row& row) { row.cells.fill(4); });

    std::vector<std::pair<std::uint32_t, int>> const expected = {{Guid, 1}, {Guid, 2}, {Guid, 3}, {Guid, 4}};
    Check(saves == expected, "the saves of one bot come in the order of its writes");
}

void Forgets()
{
    forgets.clear();
    hooked.clear();
    Hooked& store = Hooked::Instance();
    Plain& plain = Plain::Instance();
    store.GetOrCreate(11, [] { return Row::Of(11); });
    plain.GetOrCreate(11, [] { return Row::Of(11); });
    plain.GetOrCreate(12, [] { return Row::Of(12); });

    store.RegisterForget();
    plain.RegisterForget();
    store.RegisterForget();
    Check(forgets.size() == 2, "a second registration under the same name replaces the first");
    auto const forgetHooked = FindForget("hooked");
    auto const forgetPlain = FindForget("plain");
    Check(forgetHooked && forgetPlain, "each store registers its forget under its name");
    if (!forgetHooked || !forgetPlain)
        return;

    forgetHooked(11);
    Check(!store.Find(11) && plain.Find(11), "a store's forget drops only its own row");
    Check(hooked == std::vector<std::uint32_t>{11}, "the forget runs the policy's hook with the guid");
    forgetPlain(11);
    Check(!plain.Find(11) && plain.Find(12), "a forget without a hook drops the bot's row only");
}

// One writer (world thread) beside readers (map threads): no crash, every read a whole row
void Concurrency()
{
    Threaded& store = Threaded::Instance();
    for (std::uint32_t guid = 0; guid < ThreadedGuids; ++guid)
        store.LoadRow(guid, Row::Of(0));

    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::atomic<long long> reads{0};
    std::atomic<int> started{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < ReaderThreads; ++reader)
        readers.emplace_back(
            [&store, &done, &torn, &reads, &started, reader]
            {
                ++started;
                std::uint32_t guid = static_cast<std::uint32_t>(reader);
                while (!done.load())
                {
                    guid = (guid + 1) % ThreadedGuids;
                    if (std::optional<Row> const row = store.Find(guid))
                        torn += !row->Whole();

                    torn += !store.Read(guid, true, [](Row const& row) { return row.Whole(); });
                    reads += 2;
                }
            });

    while (started.load() < ReaderThreads)
        std::this_thread::yield();

    int writes = 0;
    for (int i = 1; i <= ThreadedIterations; ++i)
    {
        // The operations rotate over the guids, so a write meets a bot with and without a row
        std::uint32_t const guid = static_cast<std::uint32_t>(i / 3) % ThreadedGuids;
        switch (i % 4)
        {
            case 0:
                writes += store.Update(guid, [i](Row& row) { row.cells.fill(i); }) ? 1 : 0;
                break;
            case 1:
                store.Replace(guid, [i](Row const*) { return Row::Of(i); });
                ++writes;
                break;
            case 2:
                store.Erase(guid);
                break;
            default:
                writes += store.Find(guid) ? 0 : 1;  // the only writer: a missing row is created now
                store.GetOrCreate(guid, [i] { return Row::Of(i); });
                break;
        }
    }

    done = true;
    for (std::thread& reader : readers)
        reader.join();

    Check(torn == 0, "concurrent reads see whole rows (" + std::to_string(reads.load()) + " reads)");
    Check(reads > 0, "the readers ran beside the writer");
    Check(threadedSaves == writes, "every write that changed a row saved once, the others never");
}
}  // namespace

// The offline stand-in for BotStores.cpp's registry: the first registration under a name is replaced by a later one
void BotStores::RegisterForget(char const* name, void (*forget)(std::uint32_t guid))
{
    for (auto& [known, registered] : forgets)
        if (known == name)
        {
            registered = forget;
            return;
        }

    forgets.emplace_back(name, forget);
}

int main()
{
    Writes();
    SaveOrder();
    Forgets();
    Concurrency();
    std::cout << "bot-row-store selftest: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
