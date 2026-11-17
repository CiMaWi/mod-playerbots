/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TacticScope.h"

#include <cctype>
#include <optional>
#include <sstream>

#include "TacticIds.h"

namespace
{
std::optional<uint8> FindScopeEntry(std::string const& entry)
{
    std::string const name = TacticText::Lower(entry);
    if (name == "all")
        return TACTIC_SCOPE_ALL;

    if (name == "none")
        return 0;

    for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
    {
        if (name == TacticText::Lower(TACTIC_CONTEXT_NAMES[context]))
            return TacticContextBit(static_cast<TacticContext>(context));
    }

    return std::nullopt;
}
}  // namespace

namespace TacticText
{
std::string Lower(std::string text)
{
    for (char& character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    return text;
}

std::string Trim(std::string const& text)
{
    size_t const first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";

    size_t const last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> SplitList(std::string const& text)
{
    std::vector<std::string> entries;
    std::istringstream in(text);
    std::string entry;
    while (std::getline(in, entry, ','))
        entries.push_back(Trim(entry));

    if (!text.empty() && text.back() == ',')
        entries.push_back("");

    return entries;
}
}  // namespace TacticText

bool ParseTacticScope(std::string const& text, uint8& scope, std::string& error)
{
    uint8 result = 0;
    for (std::string const& entry : TacticText::SplitList(text))
    {
        std::optional<uint8> const bits = FindScopeEntry(entry);
        if (!bits)
        {
            error = "unknown context '" + entry + "' (Arena, Battleground, World, Pve, All, None)";
            return false;
        }

        result |= *bits;
    }

    if (TacticText::Trim(text).empty())
    {
        error = "empty scope";
        return false;
    }

    scope = result;
    return true;
}

std::string FormatTacticScope(uint8 scope)
{
    if ((scope & TACTIC_SCOPE_ALL) == TACTIC_SCOPE_ALL)
        return "All";

    std::string text;
    for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
    {
        if (scope & (1u << context))
            text += (text.empty() ? "" : ",") + std::string(TACTIC_CONTEXT_NAMES[context]);
    }

    return text.empty() ? "None" : text;
}
