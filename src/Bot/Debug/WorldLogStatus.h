/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGSTATUS_H
#define _PLAYERBOT_WORLDLOGSTATUS_H

#include <cstdint>

class Player;

// The world log's Status rows (an RPG status change and its reason) and the bot-side QuestAbandon row, declared
// Player-free so NewRpgInfo and the RPG actions need not include WorldLog.h. Defined in WorldLogHooks.cpp.
namespace WorldLog
{
constexpr char const* STATUS_REASON_OTHER = "other";

inline thread_local char const* statusReason = nullptr;

// RAII reason (a literal) for the status changes inside the scope; nests and restores the previous one, no allocation
class StatusReason
{
public:
    explicit StatusReason(char const* reason) : _previous(statusReason) { statusReason = reason; }
    ~StatusReason() { statusReason = _previous; }

    StatusReason(StatusReason const&) = delete;
    StatusReason& operator=(StatusReason const&) = delete;

private:
    char const* _previous;
};

// The innermost reason on this thread, or "other"
inline char const* CurrentStatusReason() { return statusReason ? statusReason : STATUS_REASON_OTHER; }

// A Status row (src the reason, id the new status, amount the old) when the status changed and the world log is on;
// bot null (an RpgInfo of no bot) logs nothing
void NoteStatus(Player* bot, int32_t oldStatus, int32_t newStatus);
// A QuestAbandon row for a quest the bot drops without the client's abandon packet (DropQuestAction)
void NoteQuestAbandon(Player* bot, uint32_t questId);
}  // namespace WorldLog

#endif
