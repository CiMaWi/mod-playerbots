/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_VISITLIFECYCLE_H
#define _PLAYERBOT_VISITLIFECYCLE_H

#include <cstddef>
#include <cstdint>
#include <optional>

// How a mailbox or bank visit ends (bank-mail plan): pure rules, std only, so the lifecycle harness
// (VisitLifecycleSelftest.h) checks the functions the live code calls (BankMailState.cpp, MailVisit.cpp,
// BankVisit.cpp, PlayerbotAI.cpp). Invariant: whatever happens, a started visit ends within its deadline plus one AI
// update with exactly one row and one counter, so started = done + timeout + no spot + interrupted; a bot runs one
// visit at a time.
namespace BankMail
{
enum class VisitKind : uint8_t
{
    Mail,
    Bank
};

// How a visit ended: the row's source word (VisitEndWord) and the counter it moves
enum class VisitEnd : uint8_t
{
    Done,       // the plan's every step answered or move made
    Timeout,    // the deadline passed out of combat
    NoSpot,     // no mailbox or banker where the index put it, or it went
    Combat,     // the deadline passed in combat (the stepping engine runs out of combat only)
    Teleport,   // a far teleport (PlayerbotAI::HandleTeleportAck)
    Reset,      // PlayerbotAI::Reset(true)
    Death,      // the bot died
    MapChange,  // the bot is on another map than the spot's
    Logout,     // the bot logs out, or its AI is removed
    Count
};
constexpr size_t VISIT_END_COUNT = static_cast<size_t>(VisitEnd::Count);

// The GM visit commands take the nearest mailbox or banker only within this reach (WB.12: getting farther is the
// travel errand's job)
constexpr float VISIT_COMMAND_REACH_YARDS = 60.0f;
constexpr char const* VISIT_TOO_FAR = "too_far";

inline char const* VisitEndWord(VisitKind kind, VisitEnd end)
{
    switch (end)
    {
        case VisitEnd::Done:
            return "done";
        case VisitEnd::Timeout:
            return "timeout";
        case VisitEnd::NoSpot:
            return kind == VisitKind::Mail ? "no_mailbox" : "no_banker";
        case VisitEnd::Combat:
            return "combat";
        case VisitEnd::Teleport:
            return "teleport";
        case VisitEnd::Reset:
            return "reset";
        case VisitEnd::Death:
            return "death";
        case VisitEnd::MapChange:
            return "map_change";
        case VisitEnd::Logout:
            return "logout";
        default:
            return "unknown";
    }
}

// An end the visit did not reach by itself
inline bool IsInterruption(VisitEnd end) { return end >= VisitEnd::Combat && end < VisitEnd::Count; }

// The bot puts on what the visit brought only when the visit ends in its own AI update out of combat
inline bool EquipsAfter(VisitEnd end)
{
    return end == VisitEnd::Done || end == VisitEnd::Timeout || end == VisitEnd::NoSpot || end == VisitEnd::MapChange;
}

// One visit at a time per bot: why a visit of the kind may not start now, or nullptr
inline char const* VisitStartRefusal(VisitKind kind, bool mailRuns, bool bankRuns)
{
    if (kind == VisitKind::Mail ? mailRuns : bankRuns)
        return "a visit runs already";
    if (kind == VisitKind::Mail ? bankRuns : mailRuns)
        return kind == VisitKind::Mail ? "a bank visit runs" : "a mailbox visit runs";
    return nullptr;
}

inline bool WithinVisitReach(float distance) { return distance <= VISIT_COMMAND_REACH_YARDS; }

struct VisitWatchIn
{
    bool alive = true;
    bool inCombat = false;
    bool onSpotMap = true;
    uint32_t elapsedMs = 0;
    uint32_t limitMs = 0;
};

// Every AI update while a visit runs, whatever engine runs, and at each step of the visit: the end it meets now
inline std::optional<VisitEnd> WatchVisit(VisitWatchIn const& in)
{
    if (!in.alive)
        return VisitEnd::Death;
    if (!in.onSpotMap)
        return VisitEnd::MapChange;
    if (in.elapsedMs >= in.limitMs)
        return in.inCombat ? VisitEnd::Combat : VisitEnd::Timeout;
    return std::nullopt;
}
}  // namespace BankMail

#endif
