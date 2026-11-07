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
using PlayerbotBridge::NeutralizeCommandIndicator;

// The text is the only input: no bot, config or chat type decides it.
static_assert(std::is_same_v<decltype(&ExtractChatCommand), bool (*)(std::string const&, std::string&)>);
static_assert(std::is_same_v<decltype(&NeutralizeCommandIndicator), bool (*)(std::string&)>);
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

    // A broken rule fails thousands of generated cases: report the first few, count all.
    if (++failures <= 20)
        std::cout << "FAIL " << what.substr(0, 200) << "\n";
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

// The specification of NeutralizeCommandIndicator: strip one indicator (and its spaces) while the text is a command.
std::string NeutralizeByLoop(std::string text)
{
    std::string command;
    // Bounded: a broken rule that does not shorten the line fails the checks instead of hanging them.
    for (std::size_t passes = text.size() + 1; passes > 0 && ExtractChatCommand(text, command); --passes)
        text = command;
    return text;
}

// Neutralized text is never a command line, equals the specification, reports the change, and is stable.
void ExpectNeutral(std::string const& text, std::string const& label)
{
    std::string result = text;
    bool const changed = NeutralizeCommandIndicator(result);
    std::string command;
    std::string again = result;
    bool const changedAgain = NeutralizeCommandIndicator(again);
    Report(!ExtractChatCommand(result, command) && result == NeutralizeByLoop(text) && changed == (result != text) &&
               !changedAgain && again == result && (text.empty() || !result.empty()),
           "neutralize '" + label + "' -> '" + result + "', want '" + NeutralizeByLoop(text) + "'");
}

void ExpectNeutralized(std::string const& text, std::string const& expected)
{
    std::string result = text;
    NeutralizeCommandIndicator(result);
    Report(result == expected, "neutralize '" + text + "' -> '" + result + "', want '" + expected + "'");
    ExpectNeutral(text, text);
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

    // Generated bot speech: the indicator goes, the words stay, nothing is a command line any more.
    ExpectNeutralized("#follow", "follow");
    ExpectNeutralized("##w follow", "w follow");
    ExpectNeutralized("# logout", "logout");
    ExpectNeutralized("#	x", "x");
    ExpectNeutralized("#c food", "c food");
    ExpectNeutralized("# Plan: we pull at dawn", "Plan: we pull at dawn");
    ExpectNeutralized("# #w follow", "w follow");
    ExpectNeutralized("#follow\\stay", "follow\\stay");
    ExpectNeutralized("###", "#");
    ExpectNeutralized("##  ", "#  ");
    ExpectNeutralized("# # ", "# ");
    ExpectNeutralized(std::string(4096, '#'), "#");
    ExpectNeutralized(std::string(4096, '#') + "follow", "follow");
    // Chat is untouched, including a lone indicator and an indicator later in the line.
    for (char const* text : {"follow", "stay && follow", "", "#", "# ", "#  	 ", " #follow", "a # b", "w#follow",
                             "Sure, I'll follow you."})
        ExpectNeutralized(text, text);

    // Every string of up to 7 characters over indicator, space, tab and a letter meets the specification.
    char const alphabet[] = {'#', ' ', '	', 'a'};
    for (std::size_t length = 0, total = 1; length <= 7; ++length, total *= 4)
        for (std::size_t n = 0; n < total; ++n)
        {
            std::string text;
            for (std::size_t i = 0, rest = n; i < length; ++i, rest /= 4)
                text += alphabet[rest % 4];
            ExpectNeutral(text, std::to_string(length) + ":" + std::to_string(n));
        }

    std::cout << passes << " checks passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
