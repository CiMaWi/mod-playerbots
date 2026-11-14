/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_EVENTWANTED_H
#define _PLAYERBOT_EVENTWANTED_H

#include "WorldLog.h"

namespace BotRecord
{
// Any thread: the bots' record (BotRecordMgr) was loaded at startup and takes events
bool Active();
}  // namespace BotRecord

namespace EventLog
{
// The gate of the fact hooks whose events the record takes: the world log runs or the record does (two atomic loads)
inline bool Wanted() { return WorldLog::Active() || BotRecord::Active(); }
}  // namespace EventLog

#endif
