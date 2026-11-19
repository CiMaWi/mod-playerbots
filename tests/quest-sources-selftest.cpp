/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Where an item comes from (questing plan WT32a): direct loot, references one and two levels deep, a ring, a missing
// reference, skinning, a chest and a fishing hole, quest-only rows, a shared loot id, the group rules, the core's row
// checks, spawns, statistics and determinism, built through QuestIndex from a synthetic gather. Standalone:
//   clang-cl /std:c++20 /EHsc /W4 /I src/Ai/World/Rpg /I src/Bot tests/quest-sources-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "QuestIndex.h"

using namespace Planner;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool Near(float a, double b) { return std::fabs(static_cast<double>(a) - b) < 1e-5; }

LootRow Item(std::uint32_t lootId, std::uint32_t item, float chance, std::uint8_t group = 0, std::uint8_t minCount = 1,
             std::uint8_t maxCount = 1, bool quest = false)
{
    return {lootId, item, 0, chance, group, minCount, maxCount, quest};
}

LootRow Ref(std::uint32_t lootId, std::uint32_t reference, float chance, std::uint8_t rolls = 1, std::uint8_t group = 0,
            bool quest = false)
{
    return {lootId, 0, reference, chance, group, rolls, rolls, quest};
}

void Owner(SourceGather& gather, SourceKind kind, std::uint32_t lootId, std::uint32_t entry)
{
    gather.owners.push_back({kind, lootId, entry});
}

std::vector<LootRow>& Table(SourceGather& gather, SourceKind kind)
{
    return gather.tables[static_cast<std::size_t>(kind)];
}

// The one source of the item from this entry, or none
ItemSource const* Find(std::vector<ItemSource> const& sources, SourceKind kind, std::uint32_t entry)
{
    for (ItemSource const& source : sources)
        if (source.kind == kind && source.entry == entry)
            return &source;
    return nullptr;
}

SourceGather World()
{
    SourceGather in;
    std::vector<LootRow>& corpse = Table(in, SourceKind::Corpse);
    // Direct loot: creature 100 drops 5000 at 35 %, two to four at a time
    corpse.push_back(Item(100, 5000, 35.0f, 0, 2, 4));
    Owner(in, SourceKind::Corpse, 100, 100);
    // One level deep: reference 1000 taken at 50 % and processed twice, 5001 at 20 % in it
    corpse.push_back(Ref(101, 1000, 50.0f, 2));
    in.references.push_back(Item(1000, 5001, 20.0f));
    Owner(in, SourceKind::Corpse, 101, 101);
    // Two levels deep: 1001 always, 1002 inside it at 50 %, 5002 inside that at 40 %
    corpse.push_back(Ref(102, 1001, 100.0f));
    in.references.push_back(Ref(1001, 1002, 50.0f));
    in.references.push_back(Item(1002, 5002, 40.0f));
    Owner(in, SourceKind::Corpse, 102, 102);
    // A ring: 1003 holds 5003 and 1004, 1004 holds 5004 and 1003 again
    corpse.push_back(Ref(103, 1003, 100.0f));
    in.references.push_back(Item(1003, 5003, 100.0f));
    in.references.push_back(Ref(1003, 1004, 100.0f));
    in.references.push_back(Item(1004, 5004, 50.0f));
    in.references.push_back(Ref(1004, 1003, 100.0f));
    Owner(in, SourceKind::Corpse, 103, 103);
    // A missing reference beside an item, at the top and inside a reference
    corpse.push_back(Ref(104, 9999, 100.0f));
    corpse.push_back(Item(104, 5005, 10.0f));
    corpse.push_back(Ref(104, 1013, 100.0f));
    in.references.push_back(Ref(1013, 9998, 100.0f));
    in.references.push_back(Item(1013, 5016, 100.0f));
    Owner(in, SourceKind::Corpse, 104, 104);
    // Skinning only: creature 105 is skinned for 5006, one to three
    Table(in, SourceKind::Skin).push_back(Item(105, 5006, 100.0f, 0, 1, 3));
    Owner(in, SourceKind::Skin, 105, 105);
    // A chest and a fishing hole: chest 200 holds 5007 at 30 %; hole 201 a group of 5010 at 40 % and two equal rows
    std::vector<LootRow>& object = Table(in, SourceKind::Object);
    object.push_back(Item(200, 5007, 30.0f));
    object.push_back(Item(201, 5010, 40.0f, 1));
    object.push_back(Item(201, 5008, 0.0f, 1));
    object.push_back(Item(201, 5009, 0.0f, 1));
    Owner(in, SourceKind::Object, 200, 200);
    Owner(in, SourceKind::Object, 201, 201);
    // Quest-only: 5011 drops only on the quest; a quest flag on a reference row is ignored, as the core does
    corpse.push_back(Item(106, 5011, 80.0f, 0, 1, 1, true));
    corpse.push_back(Ref(107, 1005, 100.0f, 1, 0, true));
    in.references.push_back(Item(1005, 5012, 60.0f));
    Owner(in, SourceKind::Corpse, 106, 106);
    Owner(in, SourceKind::Corpse, 107, 107);
    // Two creatures share loot id 108
    corpse.push_back(Item(108, 5013, 25.0f));
    Owner(in, SourceKind::Corpse, 108, 108);
    Owner(in, SourceKind::Corpse, 108, 109);
    // A shared sub-reference reached twice from one template is no ring: 1006 and 1007 both hold 1008
    corpse.push_back(Ref(110, 1006, 100.0f));
    corpse.push_back(Ref(110, 1007, 100.0f));
    in.references.push_back(Ref(1006, 1008, 100.0f));
    in.references.push_back(Ref(1007, 1008, 100.0f));
    in.references.push_back(Item(1008, 5014, 50.0f));
    Owner(in, SourceKind::Corpse, 110, 110);
    // One item two ways in one template: 50 % on its own and 50 % through a reference
    corpse.push_back(Item(111, 5015, 50.0f));
    corpse.push_back(Ref(111, 1009, 50.0f));
    in.references.push_back(Item(1009, 5015, 100.0f, 0, 3, 5));
    Owner(in, SourceKind::Corpse, 111, 111);
    // A group over 100 % is scaled down; a grouped reference takes its share of the group
    corpse.push_back(Item(112, 5017, 80.0f, 2));
    corpse.push_back(Item(112, 5018, 80.0f, 2));
    corpse.push_back(Item(113, 5019, 30.0f, 1));
    corpse.push_back(Ref(113, 1010, 0.0f, 1, 1));
    in.references.push_back(Item(1010, 5020, 100.0f));
    Owner(in, SourceKind::Corpse, 112, 112);
    Owner(in, SourceKind::Corpse, 113, 113);
    // Rows the core's load checks drop: ungrouped zero chances, a tiny chance, a zero count, max below min, group 128
    corpse.push_back(Item(114, 5021, 0.0f));
    corpse.push_back(Ref(114, 1008, 0.0f));
    corpse.push_back(Item(114, 5022, 0.0000001f));
    corpse.push_back(Item(114, 5023, 50.0f, 0, 0, 1));
    corpse.push_back(Item(114, 5024, 50.0f, 0, 3, 2));
    corpse.push_back(Item(114, 5025, 50.0f, 128));
    Owner(in, SourceKind::Corpse, 114, 114);
    // The same creature is a corpse and a skin source of one item; a loot id without an owner gives no source
    corpse.push_back(Item(105, 5006, 5.0f));
    Owner(in, SourceKind::Corpse, 105, 105);
    corpse.push_back(Item(115, 5026, 100.0f));
    // Spawns: creature 100 twice, the shared pair once each, chest 200 three times; creature 200 is another entry
    in.spawns.push_back({100, false, 0, 1.0f, 2.0f, 3.0f});
    in.spawns.push_back({100, false, 0, 4.0f, 5.0f, 6.0f});
    in.spawns.push_back({108, false, 1, 1.0f, 1.0f, 1.0f});
    in.spawns.push_back({109, false, 1, 2.0f, 2.0f, 2.0f});
    in.spawns.push_back({200, true, 0, 7.0f, 7.0f, 7.0f});
    in.spawns.push_back({200, true, 0, 8.0f, 8.0f, 8.0f});
    in.spawns.push_back({200, true, 1, 9.0f, 9.0f, 9.0f});
    in.spawns.push_back({200, false, 0, 9.0f, 9.0f, 9.0f});
    return in;
}

QuestIndex IndexOf(SourceGather sources)
{
    QuestGather input;
    input.sources = std::move(sources);
    return QuestIndex::Build(std::move(input));
}

void One(QuestIndex const& idx, std::uint32_t item, SourceKind kind, std::uint32_t entry, double chance,
         std::string const& what)
{
    std::vector<ItemSource> const sources = idx.SourcesOf(item);
    ItemSource const* source = Find(sources, kind, entry);
    Check(source && Near(source->chance, chance),
          what + ": item " + std::to_string(item) + " from " + SourceKindName(kind) + " " + std::to_string(entry) +
              " at " + std::to_string(chance) + " (got " + (source ? std::to_string(source->chance) : "none") + ")");
}

void Cases(QuestIndex const& idx)
{
    std::vector<ItemSource> const direct = idx.SourcesOf(5000);
    Check(direct.size() == 1 && direct[0].kind == SourceKind::Corpse && direct[0].entry == 100 &&
              Near(direct[0].chance, 0.35) && direct[0].minCount == 2 && direct[0].maxCount == 4 &&
              !direct[0].questOnly && direct[0].spawns == 2,
          "direct loot: chance, count and spawns");
    One(idx, 5001, SourceKind::Corpse, 101, 0.5 * (1.0 - 0.8 * 0.8), "a reference one level deep, two rolls");
    One(idx, 5002, SourceKind::Corpse, 102, 0.5 * 0.4, "a reference two levels deep, chances multiplied");
    One(idx, 5003, SourceKind::Corpse, 103, 1.0, "a ring: the first reference's item");
    One(idx, 5004, SourceKind::Corpse, 103, 0.5, "a ring: the inner reference's item");
    Check(idx.SourcesOf(5004).size() == 1, "a ring adds no second way");
    One(idx, 5005, SourceKind::Corpse, 104, 0.1, "a missing reference is skipped, its neighbour stays");
    One(idx, 5016, SourceKind::Corpse, 104, 1.0, "a missing reference inside a reference is skipped");

    std::vector<ItemSource> const skin = idx.SourcesOf(5006);
    ItemSource const* skinned = Find(skin, SourceKind::Skin, 105);
    Check(skin.size() == 2 && skinned && Near(skinned->chance, 1.0) && skinned->minCount == 1 && skinned->maxCount == 3,
          "skinning source with its count");
    Check(skin.size() == 2 && skin[0].kind == SourceKind::Corpse && skin[1].kind == SourceKind::Skin,
          "sources sorted by kind, then entry");
    One(idx, 5006, SourceKind::Corpse, 105, 0.05, "one creature, corpse and skin apart");

    std::vector<ItemSource> const chest = idx.SourcesOf(5007);
    Check(chest.size() == 1 && chest[0].kind == SourceKind::Object && chest[0].entry == 200 &&
              Near(chest[0].chance, 0.3) && chest[0].spawns == 3,
          "a chest, with its object spawns only");
    One(idx, 5010, SourceKind::Object, 201, 0.4, "a fishing hole's explicit group row");
    One(idx, 5008, SourceKind::Object, 201, 0.3, "a fishing hole's equal row splits the rest");
    One(idx, 5009, SourceKind::Object, 201, 0.3, "the second equal row");

    std::vector<ItemSource> const quest = idx.SourcesOf(5011);
    Check(quest.size() == 1 && quest[0].questOnly && Near(quest[0].chance, 0.8), "a quest-only row");
    std::vector<ItemSource> const refQuest = idx.SourcesOf(5012);
    Check(refQuest.size() == 1 && !refQuest[0].questOnly && Near(refQuest[0].chance, 0.6),
          "a quest flag on a reference row is ignored");

    std::vector<ItemSource> const shared = idx.SourcesOf(5013);
    Check(shared.size() == 2 && shared[0].entry == 108 && shared[1].entry == 109 && Near(shared[0].chance, 0.25) &&
              Near(shared[1].chance, 0.25) && shared[0].spawns == 1 && shared[1].spawns == 1,
          "two creatures sharing one loot id");

    One(idx, 5014, SourceKind::Corpse, 110, 1.0 - 0.5 * 0.5, "a shared sub-reference counts both ways");
    std::vector<ItemSource> const twoWays = idx.SourcesOf(5015);
    Check(twoWays.size() == 1 && Near(twoWays[0].chance, 1.0 - 0.5 * 0.5) && twoWays[0].minCount == 1 &&
              twoWays[0].maxCount == 5,
          "one item two ways in a template: chances combined, counts widened");

    One(idx, 5017, SourceKind::Corpse, 112, 0.5, "a group over 100 % is scaled down");
    One(idx, 5020, SourceKind::Corpse, 113, 0.7, "a grouped equal reference takes the group's rest");

    for (std::uint32_t const item : {5021u, 5022u, 5023u, 5024u, 5025u})
        Check(idx.SourcesOf(item).empty(), "the core drops the row of item " + std::to_string(item));
    Check(idx.SourcesOf(5026).empty(), "a loot id without an owner gives no source");
    Check(idx.SourcesOf(4999).empty(), "an unknown item has no source");

    auto const [first, last] = idx.sources.SpawnsOf(200, true);
    Check(std::distance(first, last) == 3, "the chest's spawns by entry");

    SourceStats const& s = idx.sources.stats;
    Check(s.rings == 1, "one ring ended (got " + std::to_string(s.rings) + ")");
    Check(s.missing == 2, "two missing references (got " + std::to_string(s.missing) + ")");
    Check(s.invalid == 6, "six rows the core drops (got " + std::to_string(s.invalid) + ")");
    // 1000-1010 and 1013 are reached from a loot table; 1008 once, 1011 and 1012 do not exist
    Check(s.references == 12, "twelve references expanded (got " + std::to_string(s.references) + ")");
    Check(s.sources[0] == 18 && s.sources[1] == 1 && s.sources[2] == 4,
          "sources by kind (got " + std::to_string(s.sources[0]) + ", " + std::to_string(s.sources[1]) + ", " +
              std::to_string(s.sources[2]) + ")");
    Check(s.spawns == 8, "the spawns kept");
    Check(idx.sources.EstimateBytes() > 0 && idx.stats.estimatedBytes > idx.sources.EstimateBytes(),
          "the sources count in the index's estimate");
}

std::vector<std::uint32_t> AllItems()
{
    std::vector<std::uint32_t> items;
    for (std::uint32_t item = 4999; item <= 5027; ++item)
        items.push_back(item);
    return items;
}

bool SameSources(QuestIndex const& a, QuestIndex const& b)
{
    for (std::uint32_t const item : AllItems())
    {
        std::vector<ItemSource> const x = a.SourcesOf(item);
        std::vector<ItemSource> const y = b.SourcesOf(item);
        if (x.size() != y.size())
            return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i].kind != y[i].kind || x[i].entry != y[i].entry || x[i].chance != y[i].chance ||
                x[i].minCount != y[i].minCount || x[i].maxCount != y[i].maxCount || x[i].questOnly != y[i].questOnly ||
                x[i].spawns != y[i].spawns)
                return false;
    }
    SourceStats const& s = a.sources.stats;
    SourceStats const& t = b.sources.stats;
    return s.sources == t.sources && s.items == t.items && s.references == t.references && s.rings == t.rings &&
           s.missing == t.missing && s.invalid == t.invalid && s.spawns == t.spawns;
}

void Determinism(QuestIndex const& idx)
{
    Check(SameSources(idx, IndexOf(World())), "two builds are equal");
    SourceGather shuffled = World();
    std::mt19937 rng(3232);
    for (std::vector<LootRow>& table : shuffled.tables)
        std::shuffle(table.begin(), table.end(), rng);
    std::shuffle(shuffled.references.begin(), shuffled.references.end(), rng);
    std::shuffle(shuffled.owners.begin(), shuffled.owners.end(), rng);
    std::shuffle(shuffled.spawns.begin(), shuffled.spawns.end(), rng);
    Check(SameSources(idx, IndexOf(shuffled)), "the build does not depend on the input order");
}

// A reference chain deeper than any table, a ring of many and a wide fan-out finish and stay bounded
void Shapes()
{
    SourceGather in;
    constexpr std::uint32_t DEPTH = 200;
    Table(in, SourceKind::Corpse).push_back(Ref(1, 10000, 100.0f));
    Owner(in, SourceKind::Corpse, 1, 1);
    for (std::uint32_t i = 0; i < DEPTH; ++i)
    {
        in.references.push_back(Ref(10000 + i, 10000 + (i + 1) % DEPTH, 100.0f));
        in.references.push_back(Item(10000 + i, 6000 + i, 50.0f));
    }
    QuestIndex const idx = IndexOf(std::move(in));
    Check(idx.sources.stats.rings == 1, "a ring of 200 references ends once");
    One(idx, 6000 + DEPTH - 1, SourceKind::Corpse, 1, 0.5, "the deepest reference's item");
    Check(idx.sources.stats.sources[0] == DEPTH, "a long chain gives each item once");
}
}  // namespace

int main()
{
    QuestIndex const idx = IndexOf(World());
    Cases(idx);
    Determinism(idx);
    Shapes();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
