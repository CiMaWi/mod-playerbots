/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PASTRULES_H
#define PLAYERBOTS_PASTRULES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "RecordRules.h"

// A bot's past (living-conversation LC20a, PLAN §3.5): its background, plain statements of its life before its arrival
// (kept by ordinal, each with the version of the rules it passed, a shared id for a statement several bots hold and
// its source), and the colour lines of its major record rows (one short line per participant of an event). Game-free:
// the bounds and the edits BotPastMgr applies under its lock, proven by tests/bot-past-selftest.cpp.
// Bounds (tuning values): ordinals 1 to MAX_STATEMENTS, so at most MAX_STATEMENTS statements a bot; a statement at
// most MAX_STATEMENT_BYTES bytes, a colour line at most MAX_COLOUR_BYTES; a colour line only for a row at or above the
// record's high mark (RecordRules::Permanent), at most MAX_COLOURS a bot. A text is one line, never empty.
namespace PastRules
{
constexpr std::size_t MAX_STATEMENTS = 10;
constexpr std::size_t MAX_STATEMENT_BYTES = 240;
constexpr std::size_t MAX_COLOUR_BYTES = 160;
constexpr std::size_t MAX_COLOURS = 64;
constexpr uint8_t RULES_VERSION = 1;  // The writer's rules a new statement passes; a revise pass rewrites older ones

// Who wrote a statement: the background writer, or a talk that added it (playerbots_bot_background.source)
enum class Source : uint8_t
{
    Writer,
    Talk
};

inline char const* SourceWord(Source source) { return source == Source::Talk ? "talk" : "writer"; }

inline std::optional<Source> SourceOf(std::string_view word)
{
    if (word == "writer")
        return Source::Writer;
    if (word == "talk")
        return Source::Talk;
    return std::nullopt;
}

inline std::optional<Source> SourceOf(uint8_t code)
{
    return code <= static_cast<uint8_t>(Source::Talk) ? std::optional<Source>(static_cast<Source>(code)) : std::nullopt;
}

struct Statement
{
    uint8_t Ordinal = 0;
    std::string Text;
    uint8_t Version = 0;
    uint32_t SharedId = 0;  // 0: the bot's own statement
    Source From = Source::Writer;
};

struct Colour
{
    uint32_t EventId = 0;
    std::string Text;
    uint8_t Version = 0;
};

// Why an edit was refused (None: applied); the word goes to the console and to LC20b's answer
enum class Refusal : uint8_t
{
    None,
    Ordinal,  // outside 1..MAX_STATEMENTS
    Empty,
    Long,
    Line,     // a line break in the text
    Missing,  // a revise of an ordinal the bot has no statement at
    Minor,    // a colour line for a row below the high mark, or for no row of the bot
    Full      // MAX_COLOURS colour lines already
};

inline char const* RefusalWord(Refusal refusal)
{
    switch (refusal)
    {
        case Refusal::None:
            return "none";
        case Refusal::Ordinal:
            return "ordinal";
        case Refusal::Empty:
            return "empty";
        case Refusal::Long:
            return "long";
        case Refusal::Line:
            return "line";
        case Refusal::Missing:
            return "missing";
        case Refusal::Minor:
            return "minor";
        case Refusal::Full:
            return "full";
    }
    return "";
}

inline Refusal TextRefusal(std::string_view text, std::size_t limit)
{
    if (text.empty())
        return Refusal::Empty;
    if (text.size() > limit)
        return Refusal::Long;
    if (text.find_first_of("\r\n") != std::string_view::npos)
        return Refusal::Line;
    return Refusal::None;
}

// statements: by ordinal. A new ordinal is inserted in order, a held one replaced whole
inline Refusal Set(std::vector<Statement>& statements, Statement statement)
{
    if (statement.Ordinal < 1 || statement.Ordinal > MAX_STATEMENTS)
        return Refusal::Ordinal;
    if (Refusal const refusal = TextRefusal(statement.Text, MAX_STATEMENT_BYTES); refusal != Refusal::None)
        return refusal;

    auto const at = std::lower_bound(statements.begin(), statements.end(), statement.Ordinal,
                                     [](Statement const& held, uint8_t ordinal) { return held.Ordinal < ordinal; });
    if (at != statements.end() && at->Ordinal == statement.Ordinal)
        *at = std::move(statement);
    else
        statements.insert(at, std::move(statement));
    return Refusal::None;
}

// One statement's text and version; its shared id and source stay, every other statement is untouched
inline Refusal Revise(std::vector<Statement>& statements, uint8_t ordinal, uint8_t version, std::string text)
{
    auto const at = std::find_if(statements.begin(), statements.end(),
                                 [ordinal](Statement const& held) { return held.Ordinal == ordinal; });
    if (at == statements.end())
        return Refusal::Missing;
    if (Refusal const refusal = TextRefusal(text, MAX_STATEMENT_BYTES); refusal != Refusal::None)
        return refusal;

    at->Text = std::move(text);
    at->Version = version;
    return Refusal::None;
}

// colours: by event id. score: the bot's row of the event (nullopt: the bot has no such row)
inline Refusal SetColour(std::vector<Colour>& colours, Colour colour, std::optional<uint8_t> score)
{
    if (!score || !RecordRules::Permanent(*score))
        return Refusal::Minor;
    if (Refusal const refusal = TextRefusal(colour.Text, MAX_COLOUR_BYTES); refusal != Refusal::None)
        return refusal;

    auto const at = std::lower_bound(colours.begin(), colours.end(), colour.EventId,
                                     [](Colour const& held, uint32_t id) { return held.EventId < id; });
    if (at != colours.end() && at->EventId == colour.EventId)
        *at = std::move(colour);
    else if (colours.size() >= MAX_COLOURS)
        return Refusal::Full;
    else
        colours.insert(at, std::move(colour));
    return Refusal::None;
}

inline Colour const* ColourOf(std::vector<Colour> const& colours, uint32_t eventId)
{
    auto const at = std::lower_bound(colours.begin(), colours.end(), eventId,
                                     [](Colour const& held, uint32_t id) { return held.EventId < id; });
    return at != colours.end() && at->EventId == eventId ? &*at : nullptr;
}

// The facts' `before`: the statements' texts in ordinal order
inline std::vector<std::string> Before(std::vector<Statement> const& statements)
{
    std::vector<std::string> before;
    before.reserve(statements.size());
    for (Statement const& statement : statements)
        before.push_back(statement.Text);
    return before;
}

// Every bot's past in memory, by guid
struct Book
{
    std::unordered_map<uint32_t, std::vector<Statement>> Statements;
    std::unordered_map<uint32_t, std::vector<Colour>> Colours;

    // A deleted bot: nothing of it stays
    void Forget(uint32_t guid)
    {
        Statements.erase(guid);
        Colours.erase(guid);
    }
};
}  // namespace PastRules

#endif
