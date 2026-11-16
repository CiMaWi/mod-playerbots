/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI12: the classifier (ItemUseKinds.h) over the committed dump of the item use index
// (tests/fixtures/item-use-dump.txt). A hand case per kind with real items, the invariants K1 to K4 of INTERFACE.md
// 1.7 over every record and over a seeded search of mixed records, and the census by kind with every unclassified
// use effect and what made it so. Run: item-use-kinds-selftest <module root>
//
// K4 as tested: Classified carries no free flag, so "a consumed item is never classed as free" reads "the kind,
// sub and rank never change with consumed, the charges or the price": a record and its copy with those flipped
// classify the same, and the offer reads Record::consumed itself.

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "ItemUseFacts.h"
#include "ItemUseKinds.h"

using namespace ItemUse;

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
        std::cout << "FAIL: " << what << "\n";
    }
}

bool Same(Classified const& a, Classified const& b)
{
    return a.kind == b.kind && a.sub == b.sub && a.rank == b.rank && a.why == b.why;
}

bool Helpful(Kind kind)
{
    return kind == Kind::Heal || kind == Kind::Mana || kind == Kind::Protect || kind == Kind::Boost ||
           kind == Kind::LongBuff || kind == Kind::FoodDrink;
}

bool Harmful(Kind kind) { return kind == Kind::DamageOne || kind == Kind::DamageArea || kind == Kind::Control; }

struct Counters
{
    int k1{0};
    int k2{0};
    int k3{0};
    int k4{0};
    int ids{0};
};

// K1 to K4 and "the same facts give the same kind" (the item's and spell's ids are not facts) for one record
void Invariants(Record const& record, Counters& counters)
{
    Classified const first = Classify(record);
    Classified const second = Classify(record);
    bool const oneKind =
        Same(first, second) && first.kind < Kind::Count && (first.kind == Kind::Unclassified) == !first.why.empty();
    counters.k1 += oneKind ? 0 : 1;
    counters.k2 += !record.positive && Helpful(first.kind) ? 1 : 0;
    counters.k3 += record.positive && Harmful(first.kind) ? 1 : 0;

    Record flipped = record;
    flipped.consumed = !record.consumed;
    flipped.charges = record.charges ? 0 : -1;
    flipped.replaceCopper = record.replaceCopper ? 0 : 100000;
    flipped.sellCopper = record.sellCopper ? 0 : 25000;
    counters.k4 += Same(first, Classify(flipped)) ? 0 : 1;

    Record renamed = record;
    renamed.itemEntry = record.itemEntry + 1000003;
    renamed.enchantId = record.enchantId ? record.enchantId + 7 : 0;
    renamed.spellId = record.spellId + 2000003;
    renamed.spellIndex = static_cast<std::uint8_t>((record.spellIndex + 1) % 5);
    counters.ids += Same(first, Classify(renamed)) ? 0 : 1;
}

struct HandCase
{
    char const* label;
    bool enchant;
    std::uint32_t entry;  // the item's entry, or the enchantment's id
    std::uint32_t spellId;
    Kind kind;
    Sub sub;  // Sub::Count: any sub
};

// Real records of the dump, picked by entry and spell. A mount, a quest item, the trinkets, the glove tinker and
// the repair bots were found by their facts (class, inventory type, shape, source), not by name
constexpr Sub ANY = Sub::Count;
std::array<HandCase, 41> const HAND_CASES = {{
    {"healing potion (Major Healing Potion)", false, 13446, 17534, Kind::Heal, ANY},
    {"healing potion (Super Healing Potion)", false, 22829, 28495, Kind::Heal, ANY},
    {"mana potion (Major Mana Potion)", false, 13444, 17531, Kind::Mana, Sub::Mana},
    {"bandage (Heavy Netherweave Bandage)", false, 21991, 27031, Kind::Heal, ANY},
    {"healthstone (Master Healthstone)", false, 22103, 27235, Kind::Heal, ANY},
    {"grenade (Thorium Grenade)", false, 15993, 19769, Kind::DamageArea, ANY},
    {"bomb (Hi-Explosive Bomb)", false, 10562, 12543, Kind::DamageArea, ANY},
    {"sapper charge (Goblin Sapper Charge)", false, 10646, 13241, Kind::DamageArea, ANY},
    {"net (Netherweave Net)", false, 24268, 31367, Kind::Control, ANY},
    {"flask (Flask of Endless Rage)", false, 46377, 53760, Kind::LongBuff, ANY},
    {"battle elixir (Elixir of Mastery)", false, 28104, 33726, Kind::LongBuff, ANY},
    {"guardian elixir (Elixir of Major Fortitude)", false, 32062, 39625, Kind::LongBuff, ANY},
    {"scroll (Scroll of Agility II)", false, 1477, 8116, Kind::LongBuff, ANY},
    {"buff food (Spicy Hot Talbuk)", false, 33872, 43763, Kind::FoodDrink, Sub::WellFed},
    {"stone (Rough Sharpening Stone)", false, 2862, 2828, Kind::Coating, ANY},
    {"oil (Brilliant Wizard Oil)", false, 20749, 25122, Kind::Coating, ANY},
    {"oil (Frost Oil)", false, 3829, 3595, Kind::Coating, ANY},
    {"poison (Instant Poison VII)", false, 21927, 26891, Kind::Coating, ANY},
    {"trinket self (Bloodlust Brooch)", false, 29383, 35166, Kind::Boost, ANY},
    {"trinket friend (Living Ice Crystals)", false, 40532, 60526, Kind::Heal, ANY},
    {"trinket enemy (Tidal Charm)", false, 1404, 835, Kind::Control, ANY},
    {"trinket enemy (Goblin Rocket Launcher)", false, 23836, 46567, Kind::DamageOne, ANY},
    {"trinket ground (Goblin Mortar)", false, 10577, 13237, Kind::DamageArea, ANY},
    {"trinket none (Mechanical Dragonling)", false, 4396, 4073, Kind::Summon, ANY},
    {"glove tinker (Hyperspeed Accelerators)", true, 3604, 54758, Kind::Boost, ANY},
    {"glove tinker (Hand-Mounted Pyro Rocket)", true, 3603, 54757, Kind::DamageOne, ANY},
    {"jumper cables (Goblin Jumper Cables)", false, 7148, 8342, Kind::Resurrect, ANY},
    {"target dummy (Target Dummy)", false, 4366, 4071, Kind::Summon, ANY},
    {"repair bot (Field Repair Bot 74A)", false, 18232, 22700, Kind::Summon, ANY},
    {"repair bot (Jeeves)", false, 49040, 67826, Kind::Summon, ANY},
    {"hearthstone (Hearthstone)", false, 6948, 8690, Kind::Transport, ANY},
    {"mount, aura (Horn of the Black Wolf 1041)", false, 1041, 578, Kind::Mount, ANY},
    {"mount, learned (Horn of the Black Wolf 46099)", false, 46099, 55884, Kind::Mount, ANY},
    {"recipe (Recipe: Elixir of Minor Agility)", false, 2553, 483, Kind::Learn, Sub::Recipe},
    {"quest item (Water of the Seers, class 12)", false, 4823, 4981, Kind::Quest, ANY},
    {"armor kit (Light Armor Kit)", false, 2304, 2831, Kind::Upgrade, ANY},
    {"key (Dull Iron Key)", false, 3467, 3366, Kind::Open, Sub::Lock},
    {"speed potion (Swiftness Potion)", false, 2459, 2379, Kind::Move, Sub::Speed},
    {"curing potion (Potion of Curing)", false, 3386, 26677, Kind::Dispel, ANY},
    {"absorb (Flame Deflector)", false, 4376, 4057, Kind::Protect, ANY},
    {"toy (Orb of Deception)", false, 1973, 16739, Kind::Toy, ANY},
}};

// Records the hand cases name but the facts cannot class as asked; each is checked to be what the reason says
struct KnownGap
{
    char const* label;
    std::uint32_t entry;
    std::uint32_t spellId;
    Kind kind;
    char const* reason;
};

std::array<KnownGap, 3> const KNOWN_GAPS = {{
    {"Dark Rune", 20520, 27869, Kind::Unclassified,
     "restores mana but harms the user: positive is false, so K2 forbids mana"},
    {"Gnomish Battle Chicken", 10725, 23133, Kind::Unclassified, "a dummy effect only: the summon is scripted"},
    {"Rocket Boots Xtreme", 23824, 51582, Kind::Unclassified, "a dummy effect only: the speed is scripted"},
}};

Record const* Find(std::vector<DumpRow> const& rows, bool enchant, std::uint32_t entry, std::uint32_t spellId)
{
    for (DumpRow const& row : rows)
    {
        Record const& record = row.record;
        bool const isEnchant = record.source == Source::EnchantSpell;
        if (isEnchant == enchant && (enchant ? record.enchantId : record.itemEntry) == entry &&
            record.spellId == spellId)
            return &record;
    }
    return nullptr;
}

std::string Word(Classified const& result)
{
    std::string word(KindWord(result.kind));
    if (result.sub)
        word += "/" + std::string(SubWord(result.sub));
    return word;
}
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    Check(static_cast<bool>(in), "the dump opens");
    std::string header;
    std::string names;
    std::getline(in, header);
    std::getline(in, names);
    if (!names.empty() && names.back() == '\r')
        names.pop_back();
    Check(names == DumpFieldNames(), "the dump's field names are the format's");
    std::vector<DumpRow> rows;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        std::optional<DumpRow> row = ParseDumpLine(line);
        Check(row.has_value(), "a dump line parses");
        if (row)
            rows.push_back(std::move(*row));
    }
    Check(rows.size() == 7340, "the dump holds its 7340 records");

    // The words
    Check(KindWord(Kind::Toy) == "toy" && KindWord(Kind::Count) == "unclassified", "kind words");
    Check(SubWord(static_cast<std::uint8_t>(Sub::Create)) == "create" && SubWord(200) == "none", "sub words");

    // Hand cases
    for (HandCase const& hand : HAND_CASES)
    {
        Record const* record = Find(rows, hand.enchant, hand.entry, hand.spellId);
        Check(record != nullptr, std::string("hand case in the dump: ") + hand.label);
        if (!record)
            continue;
        Classified const result = Classify(*record);
        bool const ok =
            result.kind == hand.kind && (hand.sub == ANY || result.sub == static_cast<std::uint8_t>(hand.sub));
        std::cout << "ITEM hand " << hand.label << ": " << Word(result) << " rank " << result.rank << "\n";
        Check(ok, std::string("hand case ") + hand.label + " is " + std::string(KindWord(hand.kind)) + ", got " +
                      Word(result) + (result.why.empty() ? "" : " (" + std::string(result.why) + ")"));
    }
    for (KnownGap const& gap : KNOWN_GAPS)
    {
        Record const* record = Find(rows, false, gap.entry, gap.spellId);
        Check(record != nullptr, std::string("known gap in the dump: ") + gap.label);
        if (!record)
            continue;
        Classified const result = Classify(*record);
        std::cout << "ITEM gap " << gap.label << ": " << Word(result) << " " << result.why << " (" << gap.reason
                  << ")\n";
        Check(result.kind == gap.kind, std::string("known gap ") + gap.label);
    }
    std::cout << "ITEM hand trinket unit: no on-use trinket in the dump is aimed at any unit (reported, not faked)\n";
    std::cout << "ITEM hand summon roles: helper, decoy and repair are not in the record's facts; every summon has "
                 "sub none\n";

    // Rank: within a kind, the stronger item ranks higher
    auto const rankOf = [&rows](std::uint32_t entry, std::uint32_t spellId)
    {
        Record const* record = Find(rows, false, entry, spellId);
        return record ? Classify(*record).rank : -1;
    };
    Check(rankOf(22829, 28495) > rankOf(13446, 17534), "rank: a super healing potion heals more than a major one");
    Check(rankOf(22105, 27237) > rankOf(22103, 27235), "rank: the healthstones rank by their heal");
    Check(rankOf(15993, 19769) > 0, "rank: a grenade ranks by its damage");

    // Invariants over the whole dump
    Counters dump;
    for (DumpRow const& row : rows)
        Invariants(row.record, dump);
    std::cout << "ITEM invariants dump K1 " << dump.k1 << " K2 " << dump.k2 << " K3 " << dump.k3 << " K4 " << dump.k4
              << " ids " << dump.ids << "\n";
    Check(dump.k1 == 0, "K1: every dump record gets exactly one kind");
    Check(dump.k2 == 0, "K2: no harmful dump record is classed helpful");
    Check(dump.k3 == 0, "K3: no helpful dump record is classed harmful");
    Check(dump.k4 == 0, "K4: consumed, charges and price never change a dump record's class");
    Check(dump.ids == 0, "the same facts give the same kind (ids are not read)");

    // A seeded search for a counterexample: records made of other records' effects, sign, shape and duration
    std::mt19937 random(12345u);
    std::uniform_int_distribution<std::size_t> pick(0, rows.size() - 1);
    Counters mixed;
    int const MIXES = 20000;
    for (int i = 0; i < MIXES; ++i)
    {
        Record record = rows[pick(random)].record;
        std::uint8_t count = 0;
        std::size_t const wanted = 1 + random() % MAX_EFFECTS;
        while (count < wanted)
        {
            Record const& other = rows[pick(random)].record;
            if (!other.effectCount)
                continue;
            record.effects[count++] = other.effects[random() % other.effectCount];
        }
        record.effectCount = count;
        record.positive = random() % 2 == 0;
        record.shape = static_cast<Shape>(random() % static_cast<unsigned>(Shape::Count));
        record.exclude = static_cast<Exclude>(random() % static_cast<unsigned>(Exclude::Count));
        record.durationMs = rows[pick(random)].record.durationMs;
        record.radiusYards = rows[pick(random)].record.radiusYards;
        Invariants(record, mixed);
    }
    std::cout << "ITEM invariants mixed " << MIXES << " K1 " << mixed.k1 << " K2 " << mixed.k2 << " K3 " << mixed.k3
              << " K4 " << mixed.k4 << " ids " << mixed.ids << "\n";
    Check(mixed.k1 == 0 && mixed.k2 == 0 && mixed.k3 == 0 && mixed.k4 == 0 && mixed.ids == 0,
          "K1 to K4 hold over the seeded mixed records");

    // The census, in the console's format
    std::array<std::uint32_t, static_cast<std::size_t>(Kind::Count)> kinds{};
    std::vector<DumpRow const*> unclassified;
    for (DumpRow const& row : rows)
    {
        Classified const result = Classify(row.record);
        ++kinds[static_cast<std::size_t>(result.kind)];
        if (result.kind == Kind::Unclassified)
            unclassified.push_back(&row);
    }
    for (std::size_t kind = 0; kind < kinds.size(); ++kind)
        std::cout << "ITEM census kind " << KindWord(static_cast<Kind>(kind)) << ": " << kinds[kind] << "\n";
    std::cout << "ITEM census unclassified: " << unclassified.size() << " records\n";
    for (DumpRow const* row : unclassified)
    {
        Record const& record = row->record;
        bool const item = record.source == Source::ItemSpell;
        std::cout << "ITEM unclassified " << (item ? record.itemEntry : record.enchantId) << " "
                  << (item ? "entry" : "enchant") << " " << record.spellId << " " << row->name << ": "
                  << Classify(record).why << "\n";
    }

    // With "list": every record's decision, for reading a kind's members
    if (argc > 2 && std::string(argv[2]) == "list")
        for (DumpRow const& row : rows)
        {
            Classified const result = Classify(row.record);
            std::cout << "ITEM kind " << row.record.itemEntry << " " << row.record.enchantId << " "
                      << row.record.spellId << " " << Word(result) << " " << result.rank << " " << row.name << "\n";
        }

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
