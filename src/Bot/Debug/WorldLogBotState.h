/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGBOTSTATE_H
#define _PLAYERBOT_WORLDLOGBOTSTATE_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "WorldLogRecorder.h"

class Action;
class Player;

namespace WorldLog
{
// A bot's world-log state (PlayerbotAI::worldLog), allocated at its first use while the world log is on; read and
// written on the bot's own thread, and by NoteEvent also on the world thread (mail, auction house and other events the
// world thread emits) while no map update runs. The recorder's busy flag skips a push that would overlap; its
// RequestCase may come from any thread.
struct BotLog
{
    bool inFlight = false;
    uint32_t startedMs = 0;
    uint32_t fromNode = 0;
    uint32_t toNode = 0;
    WorldLogFormat::Recorder recorder;  // WL8
};

// A bot's world-log memory for these recorder settings: the state and its rings (the meta start row's ring_bytes)
inline size_t BotLogBytes(WorldLogFormat::RecorderSettings const& settings)
{
    return sizeof(BotLog) + WorldLogFormat::RingArrayBytes(settings);
}

// WorldLog.h's: the world log is on (an acquire load)
bool Active();

inline bool InFlight(std::unique_ptr<BotLog> const& log) { return log && log->inFlight; }

// PlayerbotAI::UpdateAI calls this when IsInFlight() differs from the noted state: a Flight start or end row
void NoteFlightEdge(Player* bot, std::unique_ptr<BotLog>& log, bool flying);

// WL8, the recorder (WorldLogRecorder.cpp), all on the bot's thread while Active():
// PlayerbotAI::UpdateAI each update (one compare unless a second has passed): the position push, the "no movement"
// watch and a requested case; the first call allocates the state and the rings.
void NotePosition(Player* bot, std::unique_ptr<BotLog>& log, int32_t rpgStatus);
// Engine::ListenAndExecute, for the non-combat and dead engines: an executed action with its trigger and result
void NoteAction(Player* bot, std::unique_ptr<BotLog>& log, Action* action, std::string const& trigger, bool result);
// CensusMgr's world-log event row of a bot (the bot's thread or the world thread): the event into the ring and its
// trigger check
void NoteEvent(Player* player, std::string_view type, std::string_view source, uint32_t id, int64_t amount);
}  // namespace WorldLog

#endif
