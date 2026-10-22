/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTESTUTIL_H
#define _PLAYERBOT_PVPTESTUTIL_H

#include <string>
#include <vector>

#include "Common.h"
#include "ObjectGuid.h"

class ChatHandler;
class Player;

// One side of a match: an arena team or an explicit roster of bot names
struct PvpTestSide
{
    std::string Label;
    uint32 ArenaTeamId = 0;
    std::vector<ObjectGuid> Members;
    std::vector<std::string> Names;
    std::vector<bool> LoginRequested;
    ObjectGuid::LowType GroupId = 0;
};

// The outcome of one job; the match record is built from it and the roster facts
struct PvpTestResult
{
    std::string Outcome;  // winner, draw, timeout, ended, failed, stopped
    std::string Winner;
    std::string WinnerSide;  // A, B or empty
    std::string Map;
    std::string FailedStage;
    std::string Reason;
    uint32 FightMs = 0;
    uint32 TotalMs = 0;
};

// Helpers shared by the arena and the open-world fight harness
namespace PvpTestUtil
{
constexpr uint32 TickIntervalMs = 500;       // cadence of the O(roster) work
constexpr uint32 GatherTimeoutMs = 60000;    // logins, leaving queues, combat ending
constexpr uint32 TearDownTimeoutMs = 60000;  // leaving the arena and the teleport back
constexpr uint32 MsPerSecond = 1000;
constexpr uint32 SecondsPerMinute = 60;

bool CommandFail(ChatHandler* handler, std::string const& text);
bool CheckEnabled(ChatHandler* handler);
std::string ToLower(std::string text);
std::string Trim(std::string const& text);
// Splits at blanks outside double quotes, so a team name with spaces can be quoted
std::vector<std::string> Tokenize(std::string const& text);
// Splits at a separator; the parts are trimmed and empty ones dropped
std::vector<std::string> Split(std::string const& text, char separator);
std::string FormatDuration(uint32 ms);
// One status line per bot: where it is and whether it is still in a group, a battleground or a queue
std::string DescribeMember(ObjectGuid const& guid);
void LeaveQueues(Player* player);
// The harness takes random bots at the maximum level only
bool CheckBot(ObjectGuid const& guid, std::string& error);
}  // namespace PvpTestUtil

#endif
