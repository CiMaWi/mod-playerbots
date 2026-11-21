/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_EVENTWANTED_H
#define _PLAYERBOT_EVENTWANTED_H

#include "EventTypes.h"
#include "WorldLog.h"

namespace BotRecord
{
// Any thread: the bots' record (BotRecordMgr) was loaded at startup and takes events
bool Active();
}  // namespace BotRecord

namespace Starters
{
// Any thread: a real player is online, so the starters (StarterMgr) take events
bool Online();
}  // namespace Starters

namespace EventLog
{
// The gate of the fact hooks whose events the record takes: the world log runs or the record does (two atomic loads)
inline bool Wanted() { return WorldLog::Active() || BotRecord::Active(); }

// An event of this type has a reader now: the world log writes its row, or the record or the starters take the type
// (atomic loads only; the recorder reads through the row)
inline bool Wanted(EventType type)
{
    return (ReadBy(type, READ_ROW) && WorldLog::Active()) || (ReadBy(type, READ_RECORD) && BotRecord::Active()) ||
           (ReadBy(type, READ_STARTER) && Starters::Online());
}
}  // namespace EventLog

#endif
