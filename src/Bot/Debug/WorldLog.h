/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOG_H
#define _PLAYERBOT_WORLDLOG_H

#include <string>
#include <string_view>

#include "Define.h"
#include "WorldLogFormat.h"
#include "WorldLogRecorder.h"

class ChatHandler;
class Player;

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
// Any thread: the recorder's tuning values (WL8), clamped; a ring takes them when it is allocated
WorldLogFormat::RecorderSettings CurrentRecorderSettings();
// RealmID from worldserver.conf, read at startup (the meta start row)
uint32 RealmId();
// UTC milliseconds since the epoch: every row's t
uint64 NowMs();

// WL7: one decision row (t, bot, kind, choice, reason, value, up to three alternatives, ref) queued on the calling
// thread, the bot's map thread or the world thread; reads only the bot's guid. Nothing is formatted when the world
// log is off. choice, reason and the alternatives' words must outlive the call only (literals in practice).
void Decision(Player* bot, WorldLogFormat::DecisionKind kind, std::string_view choice, std::string_view reason,
              float value, WorldLogFormat::Alternatives const& alternatives = {}, uint32 ref = 0);

bool HandleSelftestCommand(ChatHandler* handler, char const* args);
// WL8: "playerbots worldlog case <bot>": a case of the named bot at its next position push (trigger mark, detail
// console; a mark ignores the case limit)
bool HandleCaseCommand(ChatHandler* handler, char const* args);
// WL14: ".wlog mark <text>" (GM, in game): a mark meta row at the GM's place and a case of each bot within
// MARK_YARDS (trigger mark, detail user, a = the mark row's t)
bool HandleMarkCommand(ChatHandler* handler, char const* args);
// The player's map, instance and flat position for the focus and mark tests
WorldLogFormat::FocusPoint FocusPointOf(Player* player);
// WL8: one status line: rings, cases, rows, triggers inside the case limit, words
std::string RecorderStatus();
// Dir, the writer, per stream the rows written, dropped, queued and their peak, the open segment; write errors
bool HandleStatusCommand(ChatHandler* handler, char const* args);
}  // namespace WorldLog

#endif
