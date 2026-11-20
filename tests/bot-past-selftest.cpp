/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of a bot's past (living-conversation LC20a, PastRules.h): the bounds of statements and colour lines,
// a revise that leaves every other statement byte for byte, the facts' order, and a forget that leaves nothing of the
// bot, whose two tables are on the one delete list (BotStores.h).
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot tests/bot-past-selftest.cpp

#include <cstdio>
#include <string>
#include <string_view>

#include "BotStores.h"
#include "PastRules.h"

namespace
{
using namespace PastRules;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

Statement Of(uint8_t ordinal, std::string text)
{
    return Statement{ordinal, std::move(text), RULES_VERSION, 0, Source::Writer};
}

bool Same(Statement const& a, Statement const& b)
{
    return a.Ordinal == b.Ordinal && a.Text == b.Text && a.Version == b.Version && a.SharedId == b.SharedId &&
           a.From == b.From;
}

void StatementBounds()
{
    std::vector<Statement> list;
    Check(Set(list, Of(0, "x")) == Refusal::Ordinal, "ordinal 0 refused");
    Check(Set(list, Of(MAX_STATEMENTS + 1, "x")) == Refusal::Ordinal, "ordinal above the most statements refused");
    Check(Set(list, Of(1, "")) == Refusal::Empty, "an empty statement refused");
    Check(Set(list, Of(1, "one\ntwo")) == Refusal::Line, "a statement with a line break refused");
    Check(Set(list, Of(1, std::string(MAX_STATEMENT_BYTES + 1, 'a'))) == Refusal::Long,
          "a statement over its bytes refused");
    Check(list.empty(), "a refused statement leaves nothing");
    Check(Set(list, Of(1, std::string(MAX_STATEMENT_BYTES, 'a'))) == Refusal::None, "a statement at its bytes is set");

    for (uint8_t ordinal = static_cast<uint8_t>(MAX_STATEMENTS); ordinal >= 2; --ordinal)
        Set(list, Of(ordinal, "statement " + std::to_string(ordinal)));
    bool ordered = list.size() == MAX_STATEMENTS;
    for (std::size_t index = 0; index < list.size(); ++index)
        ordered = ordered && list[index].Ordinal == index + 1;
    Check(ordered, "statements set out of order are kept by ordinal, at most the most statements");

    Check(Set(list, Of(3, "replaced")) == Refusal::None && list.size() == MAX_STATEMENTS && list[2].Text == "replaced",
          "a held ordinal is replaced, not added");

    std::vector<std::string> const before = Before(list);
    Check(before.size() == MAX_STATEMENTS && before[0] == list[0].Text && before[2] == "replaced" &&
              before.back() == "statement 10",
          "before lists the texts in ordinal order");
}

void ReviseOne()
{
    std::vector<Statement> list;
    for (uint8_t ordinal = 1; ordinal <= 8; ++ordinal)
    {
        Statement statement = Of(ordinal, "line " + std::to_string(ordinal) + " 'quoted'");
        statement.SharedId = ordinal == 4 ? 77 : 0;
        statement.From = ordinal % 2 ? Source::Writer : Source::Talk;
        Set(list, std::move(statement));
    }
    std::vector<Statement> const old = list;

    Check(Revise(list, 4, 2, "the new line four") == Refusal::None, "a held statement is revised");
    bool others = list.size() == old.size();
    for (std::size_t index = 0; others && index < list.size(); ++index)
        if (list[index].Ordinal != 4)
            others = Same(list[index], old[index]);
    Check(others, "a revise leaves every other statement byte for byte");
    Statement const& four = list[3];
    Check(four.Text == "the new line four" && four.Version == 2 && four.SharedId == 77 && four.From == Source::Talk,
          "a revise sets the text and version, its shared id and source stay");

    std::vector<Statement> const revised = list;
    Check(Revise(list, 9, 2, "none") == Refusal::Missing, "a revise of an ordinal the bot has not is refused");
    Check(Revise(list, 5, 2, std::string(MAX_STATEMENT_BYTES + 1, 'b')) == Refusal::Long,
          "a revise over the bytes is refused");
    Check(Revise(list, 5, 2, "") == Refusal::Empty, "an empty revise is refused");
    bool untouched = list.size() == revised.size();
    for (std::size_t index = 0; untouched && index < list.size(); ++index)
        untouched = Same(list[index], revised[index]);
    Check(untouched, "a refused revise changes nothing");
}

void ColourBounds()
{
    std::vector<Colour> list;
    Check(SetColour(list, {5, "a line", RULES_VERSION}, std::nullopt) == Refusal::Minor,
          "a colour line for no row of the bot refused");
    Check(SetColour(list, {5, "a line", RULES_VERSION}, static_cast<uint8_t>(RecordRules::HIGH_MARK - 1)) ==
              Refusal::Minor,
          "a colour line below the high mark refused");
    Check(SetColour(list, {5, std::string(MAX_COLOUR_BYTES + 1, 'c'), RULES_VERSION}, RecordRules::HIGH_MARK) ==
              Refusal::Long,
          "a colour line over its bytes refused");
    Check(SetColour(list, {5, "two\rlines", RULES_VERSION}, RecordRules::HIGH_MARK) == Refusal::Line,
          "a colour line with a line break refused");
    Check(list.empty(), "a refused colour line leaves nothing");
    Check(SetColour(list, {5, std::string(MAX_COLOUR_BYTES, 'c'), RULES_VERSION}, RecordRules::HIGH_MARK) ==
              Refusal::None,
          "a colour line at its bytes and the high mark is set");

    for (uint32_t id = 1000; list.size() < MAX_COLOURS; ++id)
        SetColour(list, {id, "line", RULES_VERSION}, RecordRules::MAX_SCORE);
    Check(SetColour(list, {9999, "one more", RULES_VERSION}, RecordRules::MAX_SCORE) == Refusal::Full &&
              list.size() == MAX_COLOURS,
          "no colour line beyond the most a bot keeps");
    Check(SetColour(list, {5, "rewritten", RULES_VERSION}, RecordRules::MAX_SCORE) == Refusal::None &&
              list.size() == MAX_COLOURS,
          "a full bot's line of a held event is rewritten");
    Colour const* five = ColourOf(list, 5);
    Check(five && five->Text == "rewritten" && !ColourOf(list, 6), "a colour line is found by its event id only");
}

void Forget()
{
    Book book;
    Set(book.Statements[7], Of(1, "kept"));
    Set(book.Statements[8], Of(1, "gone"));
    SetColour(book.Colours[8], {3, "gone too", RULES_VERSION}, RecordRules::MAX_SCORE);
    SetColour(book.Colours[7], {3, "kept too", RULES_VERSION}, RecordRules::MAX_SCORE);
    book.Forget(8);
    Check(!book.Statements.count(8) && !book.Colours.count(8), "a forgotten bot leaves no statement and no line");
    Check(book.Statements.count(7) && book.Colours.count(7), "a forget leaves the other bots");
}

bool Listed(std::string_view table)
{
    for (BotRow const& row : BotRows)
        if (row.db == BotRowDb::Playerbots && table == row.table && std::string_view(row.column) == "guid" &&
            !row.byItem)
            return true;
    return false;
}

void DeleteList()
{
    Check(Listed("playerbots_bot_background"), "the background table is on the delete list by guid");
    Check(Listed("playerbots_bot_colour"), "the colour table is on the delete list by guid");
}

void Words()
{
    Check(SourceOf(SourceWord(Source::Writer)) == Source::Writer &&
              SourceOf(SourceWord(Source::Talk)) == Source::Talk && !SourceOf(std::string_view("other")) &&
              SourceOf(uint8_t{1}) == Source::Talk && !SourceOf(uint8_t{2}),
          "source words and codes round-trip");
}
}  // namespace

int main()
{
    StatementBounds();
    ReviseOne();
    ColourBounds();
    Forget();
    DeleteList();
    Words();
    std::printf("bot-past selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
