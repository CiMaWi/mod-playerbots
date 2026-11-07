/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHATCOMMANDLINE_H
#define PLAYERBOTS_CHATCOMMANDLINE_H

#include <string>

// The one rule for human chat to bots, shared by the playerbots chat hooks and mod-ollama-chat: only a line that starts
// with the indicator is a playerbots command; every other line is ordinary chat. AzerothCore claims '.' and '!' and the
// client '/', so none of them can be the indicator. Reply-channel selectors are typed after it ("##w follow").
namespace PlayerbotBridge
{
inline constexpr char CommandIndicator = '#';

// True when `text` is a command line; `command` then receives it without the indicator and the spaces after it,
// otherwise it is cleared. An indicator followed by nothing but spaces is ordinary chat.
inline bool ExtractChatCommand(std::string const& text, std::string& command)
{
    std::size_t const start =
        text.empty() || text[0] != CommandIndicator ? std::string::npos : text.find_first_not_of(" \t", 1);
    if (start == std::string::npos)
    {
        command.clear();
        return false;
    }

    command = text.substr(start);
    return true;
}
}  // namespace PlayerbotBridge

#endif
