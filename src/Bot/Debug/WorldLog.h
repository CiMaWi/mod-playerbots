/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOG_H
#define _PLAYERBOT_WORLDLOG_H

#include <string>

#include "Define.h"
#include "WorldLogFormat.h"

class ChatHandler;

// The world log's server side: its options (Playerbots.WorldLog.*, read here, not in PlayerbotAIConfig), the rows'
// clock and the console commands. On when Playerbots.WorldLog.Dir is set at startup (the hub's ac.js sets it to the
// session folder). Streams, row formats, segments and the queue account are Player-free, in WorldLogFormat.h.
namespace WorldLog
{
// World thread: at startup (reload false) and on ".reload config" (reload true). Dir keeps its startup value; the
// tuning values are re-read.
void LoadConfig(bool reload);
// Any thread: Dir was set at startup (an acquire load: once true, Dir() is safe to read on any thread)
bool Active();
// The session folder the segments go to; fixed after startup. Read it only after Active() returned true (or on the
// world thread after startup).
std::string const& Dir();
// Any thread: the tuning values, clamped
WorldLogFormat::Settings CurrentSettings();
// RealmID from worldserver.conf, read at startup (the meta start row)
uint32 RealmId();
// UTC milliseconds since the epoch: every row's t
uint64 NowMs();

bool HandleSelftestCommand(ChatHandler* handler, char const* args);
// Dir, the writer, per stream the rows written, dropped, queued and their peak, the open segment; write errors
bool HandleStatusCommand(ChatHandler* handler, char const* args);
}  // namespace WorldLog

#endif
