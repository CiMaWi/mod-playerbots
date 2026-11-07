/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the chat command line rule shared by the playerbots chat hooks and mod-ollama-chat.
#include <iostream>
#include <string>
#include <type_traits>

#include "ChatCommandLine.h"

using PlayerbotBridge::CommandIndicator;
using PlayerbotBridge::ExtractChatCommand;

// The text is the only input: no bot, config or chat type decides it.
static_assert(std::is_same_v<decltype(&ExtractChatCommand), bool (*)(std::string const&, std::string&)>);
static_assert(CommandIndicator == '#');

namespace
{
int failures = 0;
int passes = 0;

void Report(bool ok, std::string const& what)
{
    if (ok)
    {
        ++passes;
        return;
    }

    ++failures;
    std::cout << "FAIL " << what << "\n";
}

// Ordinary chat: not a command, and the output is cleared even when it held an earlier command.
void ExpectChat(std::string const& text)
{
    std::string command = "stale";
    bool const isCommand = ExtractChatCommand(text, command);
    Report(!isCommand && command.empty(), "chat line taken as command: '" + text + "' -> '" + command + "'");
}

void ExpectCommand(std::string const& text, std::string const& expected)
{
    std::string command = "stale";
    bool const isCommand = ExtractChatCommand(text, command);
    Report(
        isCommand && command == expected,
        "command line '" + text + "' -> " + (isCommand ? "'" + command + "'" : "chat") + ", want '" + expected + "'");
}
}  // namespace

int main()
{
    // Item words and substrings that playerbots' item matching used to pick up.
    for (char const* text : {"tell me more", "go ahead", "I'm afraid", "food", "potion", "ammo",
                             "|cff9d9d9d|Hitem:1234:0:0:0:0:0:0:0:0|h[Grey Thing]|h|r", "sell my grey items"})
        ExpectChat(text);

    // Former trigger and literal first words: plain chat now, the LLM answers them.
    for (char const* text :
         {"follow me please", "follow", "stay here", "attack the boss", "who are you", "help", "go north", "co +dps",
          "c food", "t food", "logout", "reset", "debug x", "d x", "queue follow"})
        ExpectChat(text);

    // Prefixes claimed by AzerothCore and the client, and edge cases.
    for (char const* text : {"!follow", "/follow", ".playerbots bot add x", "", "#", "# ", "#  \t ", " #follow",
                             "follow\\\\stay", "a # b", "w#follow"})
        ExpectChat(text);

    ExpectCommand("#follow", "follow");
    ExpectCommand("#c food", "c food");
    ExpectCommand("#t food", "t food");
    ExpectCommand("#co +dps", "co +dps");
    ExpectCommand("#attack", "attack");
    ExpectCommand("#d x", "d x");
    ExpectCommand("#logout", "logout");
    // One indicator is stripped; playerbots then reads its own reply-channel selector.
    ExpectCommand("##w follow", "#w follow");
    ExpectCommand("##", "#");
    // The separator stays in the command; playerbots splits it later.
    ExpectCommand("#follow\\\\stay", "follow\\\\stay");
    // Spaces after the indicator are dropped, so "# logout" meets the whisper hook's logout guard like "#logout".
    ExpectCommand("# follow", "follow");
    ExpectCommand("# \t logout", "logout");
    ExpectCommand("# #w follow", "#w follow");
    ExpectCommand("#|cff9d9d9d|Hitem:1234:0:0:0:0:0:0:0:0|h[Grey Thing]|h|r",
                  "|cff9d9d9d|Hitem:1234:0:0:0:0:0:0:0:0|h[Grey Thing]|h|r");

    std::cout << passes << " checks passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
