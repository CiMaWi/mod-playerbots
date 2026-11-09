/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGBOTSTATE_H
#define _PLAYERBOT_WORLDLOGBOTSTATE_H

#include <cstdint>
#include <memory>

class Player;

namespace WorldLog
{
// A bot's world-log state (PlayerbotAI::worldLog), allocated at its first use while the world log is on; read and
// written only on the bot's own thread
struct BotLog
{
    bool inFlight = false;
    uint32_t startedMs = 0;
    uint32_t fromNode = 0;
    uint32_t toNode = 0;
};

inline bool InFlight(std::unique_ptr<BotLog> const& log) { return log && log->inFlight; }

// PlayerbotAI::UpdateAI calls this when IsInFlight() differs from the noted state: a Flight start or end row
void NoteFlightEdge(Player* bot, std::unique_ptr<BotLog>& log, bool flying);
}  // namespace WorldLog

#endif
