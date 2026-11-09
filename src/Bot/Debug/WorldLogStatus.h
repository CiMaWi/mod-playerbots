/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGSTATUS_H
#define _PLAYERBOT_WORLDLOGSTATUS_H

#include <atomic>
#include <cstdint>

class Player;

// The world log's Status rows (an RPG status change and its reason) and the bot-side QuestAbandon row, declared
// Player-free so NewRpgInfo and the RPG actions need not include WorldLog.h. Defined in WorldLogHooks.cpp. WL7: the
// RPG status words and the trip ids.
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

// The words NewRpgInfo::StatusFromString accepts, by NewRpgStatus value (PlayerbotAIConfig.h; NewRpgBaseAction.cpp
// asserts the count)
constexpr int32_t RPG_STATUS_WORD_COUNT = 12;
inline constexpr char const* RPG_STATUS_WORDS[RPG_STATUS_WORD_COUNT] = {
    "idle",          "go grind", "go camp",     "wander random", "wander npc", "do quest",
    "travel flight", "rest",     "outdoor pvp", "do gather",     "travel",     "errands"};

// A decision row's word for an RPG status; "other" outside the enum
inline char const* NewRpgStatusWord(int32_t status)
{
    return status >= 0 && status < RPG_STATUS_WORD_COUNT ? RPG_STATUS_WORDS[status] : STATUS_REASON_OTHER;
}

// Trip ids of one server run, from 1 (0 = no trip; skipped when the count wraps). Any thread: one relaxed increment.
class TripCounter
{
public:
    uint32_t Next()
    {
        uint32_t id = 0;
        while (!id)
            id = _last.fetch_add(1, std::memory_order_relaxed) + 1;

        return id;
    }

private:
    std::atomic<uint32_t> _last{0};
};

inline TripCounter tripCounter;

// The id of a new trip (NewRpgInfo::ChangeToTravel): its Travel events' world-log id and its decisions' ref
inline uint32_t NextTripId() { return tripCounter.Next(); }
}  // namespace WorldLog

#endif
