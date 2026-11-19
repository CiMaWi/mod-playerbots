/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of `route check vessel`'s rules (RouteCheckVesselRules.h, navigation plan NV33a): the forms and
// the last word `steep`, the two deck filters, a pairs file's lines with each reason a line is refused, and the end
// gap.

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "RouteCheckVesselRules.h"

namespace
{
namespace Rules = RouteCheckVesselRules;

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

Rules::Command Parse(std::string const& text) { return Rules::ParseCommand(Rules::Split(text)); }

void CommandChecks()
{
    Rules::Command c = Parse("7087 1 2 3 4 5 6");
    Check(c.form == Rules::Form::Points && c.displayId == 7087 && !c.steep && c.offsets[5] == 6.0f, "the points form");
    c = Parse("7087 1 2 3 4 5 6 steep");
    Check(c.form == Rules::Form::Points && c.steep && c.offsets[0] == 1.0f, "the points form with steep");
    c = Parse("all 1 1");
    Check(c.form == Rules::Form::Random && c.all && c.count == 1 && c.seed == 1 && !c.steep, "all, one pair");
    c = Parse("all 1 1 steep");
    Check(c.form == Rules::Form::Random && c.all && c.steep, "all with steep (four words)");
    c = Parse("3031 200 7");
    Check(c.form == Rules::Form::Random && !c.all && c.displayId == 3031 && c.count == 200, "one vessel's run");
    c = Parse("pairs C:/x/pairs.tsv");
    Check(c.form == Rules::Form::Pairs && c.file == "C:/x/pairs.tsv" && !c.steep, "the pairs form");
    c = Parse("  pairs\tC:/x/pairs.tsv  steep ");
    Check(c.form == Rules::Form::Pairs && c.file == "C:/x/pairs.tsv" && c.steep, "the pairs form with steep");
    Check(Parse("pairs").form == Rules::Form::Bad, "pairs without a file");
    Check(Parse("pairs a b").form == Rules::Form::Bad, "pairs with two files");
    Check(Parse("steep").form == Rules::Form::Bad, "steep alone");
    Check(Parse("all 1 2 3 4 5 6").form == Rules::Form::Bad, "all with points");
    Check(Parse("all 0 1").form == Rules::Form::Bad, "no pairs");
    Check(Parse("all 2001 1").form == Rules::Form::Bad, "over the pair limit");
    Check(Parse("all 1 1 steep steep").form == Rules::Form::Bad, "steep twice");
    Check(Parse("-5 1 1").form == Rules::Form::Bad, "a negative display id");
    Check(Parse("7087 1 2 x 4 5 6").form == Rules::Form::Bad, "an offset that is no number");
    Check(Parse("7087 1 2 nan 4 5 6").form == Rules::Form::Bad, "an offset that is not finite");
    Check(Parse("").form == Rules::Form::Bad, "no words");
}

void FilterChecks()
{
    Rules::FilterFlags const bots = Rules::DeckFilter(false);
    Rules::FilterFlags const steep = Rules::DeckFilter(true);
    Check(bots.include == (Rules::GROUND | Rules::WATER) &&
              bots.exclude == (Rules::MAGMA | Rules::SLIME | Rules::GROUND_STEEP),
          "the bots' filter: ground and water, no magma, slime or steep ground");
    Check(steep.include == (Rules::GROUND | Rules::WATER | Rules::GROUND_STEEP) &&
              steep.exclude == (Rules::MAGMA | Rules::SLIME),
          "the steep filter: steep ground added, magma and slime still out");
    Check(bots.waterCost == 5.0f && steep.waterCost == 5.0f, "water costs 5 under both");
    Check(std::string(Rules::FilterName(false)) == "bots" && std::string(Rules::FilterName(true)) == "steep",
          "filter names");
}

void LineChecks()
{
    Rules::PairLine pair;
    Check(Rules::ParsePairLine("a17\t7087\t-0.14\t9.92\t6.10\t-8.14\t-9.41\t5.45", 3, pair) == Rules::LineKind::Pair &&
              pair.id == "a17" && pair.displayId == 7087 && pair.offsets[0] == -0.14f && pair.offsets[5] == 5.45f,
          "a line with an id");
    Check(Rules::ParsePairLine("7087 1 2 3 4 5 6\r", 9, pair) == Rules::LineKind::Pair && pair.id == "9" &&
              pair.offsets[5] == 6.0f,
          "a line without an id takes its line number; a CR is dropped");
    Check(Rules::ParsePairLine("", 1, pair) == Rules::LineKind::Skipped, "an empty line");
    Check(Rules::ParsePairLine("# comment", 1, pair) == Rules::LineKind::Skipped, "a comment");
    Check(Rules::ParsePairLine("id\tdisplay\tsx\tsy\tsz\tex\tey\tez", 1, pair) == Rules::LineKind::Skipped,
          "the header");
    Check(Rules::ParsePairLine("7087 1 2 3 4 5", 1, pair) == Rules::LineKind::FieldCount, "six fields");
    Check(Rules::ParsePairLine("a 7087 1 2 3 4 5 6 7", 1, pair) == Rules::LineKind::FieldCount, "nine fields");
    Check(Rules::ParsePairLine("a ship 1 2 3 4 5 6", 1, pair) == Rules::LineKind::DisplayId, "a display id word");
    Check(Rules::ParsePairLine("a 7087 1 2 3 4 5 z", 1, pair) == Rules::LineKind::Offset, "an offset word");
    Check(std::string(Rules::LineKindName(Rules::LineKind::Offset)) == "bad offset", "reason names");
}

void GapChecks()
{
    float const end[3] = {-8.14f, -9.41f, 5.45f};
    float const reached[3] = {-8.14f, -9.41f, 5.45f};
    float const short3[3] = {-8.14f, -9.41f + 3.0f, 5.45f + 4.0f};
    Check(Rules::EndGap(4, reached, end) == 0.0f, "a path that reaches its end: gap 0");
    Check(std::fabs(Rules::EndGap(3, short3, end) - 5.0f) < 1e-5f, "a path that stops short: gap 5 yd");
    Check(Rules::EndGap(0, reached, end) < 0.0f, "no point: no gap");
}
}  // namespace

int main()
{
    CommandChecks();
    FilterChecks();
    LineChecks();
    GapChecks();

    std::cout << "route-check-vessel: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
