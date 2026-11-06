/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGELITERALROUTE_H
#define PLAYERBOTS_BRIDGELITERALROUTE_H

#include <cstdint>
#include <functional>
#include <string>

class PlayerbotAI;

// Tells whether playerbots acts on a human chat line itself, without acting: a read-only mirror of
// PlayerbotAI::HandleCommand(uint32, std::string, Player*) and ExternalEventHelper::ParseChatCommand.
namespace PlayerbotBridge
{
enum class LiteralClass : uint8_t
{
    NotLiteral,
    Literal,
    Uncertain
};

enum class LiteralScope : uint8_t
{
    Whisper,
    Group,
    Other
};

struct LiteralRules
{
    std::string separator;
    std::string prefix;
    bool autoTradeOnItemMention = true;
};

using LiteralProbe = std::function<bool(std::string const&)>;

// Chat lines are far shorter; longer text or more segments are not mirrored and count as Uncertain.
inline constexpr std::size_t MaxLiteralTextBytes = 4096;
inline constexpr uint32_t MaxLiteralSegments = 32;

namespace LiteralDetail
{
// The C-locale std::isspace set of playerbots' trim().
inline bool IsTrimSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

inline bool StartsWith(std::string const& text, std::string const& head) { return text.rfind(head, 0) == 0; }

inline LiteralClass ClassifySegment(std::string text, LiteralScope scope, LiteralRules const& rules,
                                    LiteralProbe const& triggerExists, LiteralProbe const& parseableItem)
{
    if (!rules.prefix.empty())
    {
        if (!StartsWith(text, rules.prefix))
            return LiteralClass::NotLiteral;

        text.erase(0, rules.prefix.size());
    }

    // "#w ", "#p ", "#r ", "#a " and "#g " only pick the reply channel.
    if (text.size() >= 3 && text[0] == '#' && text[2] == ' ' &&
        (text[1] == 'w' || text[1] == 'p' || text[1] == 'r' || text[1] == 'a' || text[1] == 'g'))
        text.erase(0, 3);

    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsTrimSpace(text[begin]))
        ++begin;

    while (end > begin && IsTrimSpace(text[end - 1]))
        --end;

    text = text.substr(begin, end - begin);
    if (text.empty())
        return LiteralClass::NotLiteral;

    // The per-bot chat filters (@tank, @60, @skull, ...) may drop or rewrite the line; they are not run here.
    if (text.find('@') != std::string::npos)
        return LiteralClass::Uncertain;

    if (StartsWith(text, "debug ") || (text.size() > 2 && StartsWith(text, "d ")) ||
        (text.size() > 3 && StartsWith(text, "do ")) ||
        (scope != LiteralScope::Whisper && text.size() > 6 && StartsWith(text, "queue ")) || text == "reset" ||
        text == "logout" || text == "logout cancel")
        return LiteralClass::Literal;

    if (triggerExists(text))
        return LiteralClass::Literal;

    // ParseChatCommand tries every head before a space as a trigger name, longest first.
    std::size_t i = std::string::npos;
    while (true)
    {
        std::size_t const found = text.rfind(' ', i);
        if (found == std::string::npos || !found)
            break;

        if (triggerExists(text.substr(0, found)))
            return LiteralClass::Literal;

        i = found - 1;
    }

    if (!parseableItem(text))
        return LiteralClass::NotLiteral;

    return rules.autoTradeOnItemMention ? LiteralClass::Literal : LiteralClass::NotLiteral;
}
}  // namespace LiteralDetail

inline LiteralClass ClassifyLiteral(std::string const& text, LiteralScope scope, LiteralRules const& rules,
                                    LiteralProbe const& triggerExists, LiteralProbe const& parseableItem)
{
    // Say, yell and every other scope have no playerbots command hook.
    if (scope == LiteralScope::Other)
        return LiteralClass::NotLiteral;

    if (!triggerExists || !parseableItem || text.size() > MaxLiteralTextBytes)
        return LiteralClass::Uncertain;

    // An empty separator is never split (playerbots would recurse without end on it).
    if (rules.separator.empty() || text.find(rules.separator) == std::string::npos)
        return LiteralDetail::ClassifySegment(text, scope, rules, triggerExists, parseableItem);

    // playerbots splits with strtok: it stops at a NUL, any separator character ends a segment, empty ones vanish.
    std::string const line = text.substr(0, text.find('\0'));
    uint32_t segments = 0;
    for (std::size_t begin = line.find_first_not_of(rules.separator); begin != std::string::npos;
         begin = line.find_first_not_of(rules.separator, line.find_first_of(rules.separator, begin)))
        if (++segments > MaxLiteralSegments)
            return LiteralClass::Uncertain;

    LiteralClass result = LiteralClass::NotLiteral;
    for (std::size_t begin = line.find_first_not_of(rules.separator); begin != std::string::npos;)
    {
        std::size_t const end = line.find_first_of(rules.separator, begin);
        LiteralClass const segment = LiteralDetail::ClassifySegment(
            line.substr(begin, end == std::string::npos ? std::string::npos : end - begin), scope, rules, triggerExists,
            parseableItem);
        if (segment == LiteralClass::Literal)
            return LiteralClass::Literal;

        if (segment == LiteralClass::Uncertain)
            result = LiteralClass::Uncertain;

        begin = line.find_first_not_of(rules.separator, end);
    }

    return result;
}

// Adapter for a live bot (BridgeLiteralRoute.cpp): its own trigger table and the playerbots config. Run it on the
// thread that fires the chat hook; it reads only shared tables and config strings.
LiteralClass ClassifyBotLiteral(PlayerbotAI* botAI, uint32_t type, std::string const& text);
}  // namespace PlayerbotBridge

#endif
