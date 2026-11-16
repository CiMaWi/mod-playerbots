/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI2c: the committed dump of the item use index (tests/fixtures/item-use-dump.txt, written by the
// console's `playerbots item dump` on lab9) is parsed by the shared format code, so lane 2's classifier tests and the
// server agree on it. Run: item-use-dump-selftest <module root>

#include <array>
#include <fstream>
#include <iostream>
#include <string>

#include "ItemUseFacts.h"

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
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    std::ifstream in(root + "/tests/fixtures/item-use-dump.txt", std::ios::binary);
    Check(static_cast<bool>(in), "the dump opens");
    if (!in)
    {
        std::cout << passed << " passed, " << failed << " failed\n";
        return 1;
    }

    std::string header;
    std::string names;
    std::getline(in, header);
    std::getline(in, names);
    Check(header.rfind("# item-use-dump v1 module ", 0) == 0, "header line starts as the format says");
    std::size_t const marker = header.rfind(" records ");
    std::size_t const declared = marker == std::string::npos ? 0 : std::stoul(header.substr(marker + 9));
    Check(declared > 0, "header names the record count");
    Check(names == DumpFieldNames(), "line 2 holds the field names in the code's order");

    std::size_t records = 0;
    std::size_t badParse = 0;
    std::size_t badRoundTrip = 0;
    std::size_t noEffect = 0;
    std::size_t badConsumed = 0;
    std::array<std::size_t, static_cast<std::size_t>(Shape::Count)> shapes{};
    std::string line;
    while (std::getline(in, line))
    {
        ++records;
        std::optional<DumpRow> const row = ParseDumpLine(line);
        if (!row)
        {
            ++badParse;
            continue;
        }
        if (FormatDumpLine(row->record, row->name) != line)
            ++badRoundTrip;
        if (row->record.effectCount < 1)
            ++noEffect;
        if (row->record.consumed && row->record.replaceCopper == 0 && row->record.price != PriceSource::None)
            ++badConsumed;
        ++shapes[static_cast<std::size_t>(row->record.shape)];
    }
    Check(records == declared, "the header's count equals the lines");
    Check(badParse == 0, "every line parses");
    Check(badRoundTrip == 0, "every line formats back to itself");
    // The lab9 dump has two records without an effect (Crippling Poison II, Grimoire of Blood Pact Rank 6: spells whose
    // effects the index does not copy) and two consumed ones that price at 0 copper (Martin Fury: 7 copper over 100
    // charges; the Prototype Neural Needler: a vendor price of 0); a handful more is a regression
    Check(noEffect <= 4, "nearly every record has an effect");
    Check(badConsumed <= 4, "a consumed record has a replace price or no price source");
    std::size_t withRecords = 0;
    for (std::size_t count : shapes)
        withRecords += count > 0 ? 1 : 0;
    Check(withRecords == shapes.size(), "every target shape has records");

    std::cout << records << " records, " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
