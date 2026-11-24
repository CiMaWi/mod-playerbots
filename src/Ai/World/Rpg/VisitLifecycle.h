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

#include "BankMailTypes.h"

// How a mailbox, bank or craft visit ends (bank-mail plan; the craft visit: professions WR7): pure rules, std only,
// so the lifecycle harness (VisitLifecycleSelftest.h) checks the functions the live code calls (BankMailState.cpp,
// MailVisit.cpp, BankVisit.cpp, CraftVisit.cpp, PlayerbotAI.cpp). Invariant: whatever happens, a started visit ends
// within its deadline plus one AI update with exactly one row and one counter, so started = done + timeout + no spot
// + unreached + the craft's own ends + interrupted; a bot runs one visit at a time.
namespace BankMail
{
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
            return kind == VisitKind::Mail ? "no_mailbox" : kind == VisitKind::Bank ? "no_banker" : "no_spot";
        case VisitEnd::Unreached:
            return "unreached";
        case VisitEnd::NoMaterials:
            return "no_materials";
        case VisitEnd::NoStation:
            return "no_station";
        case VisitEnd::BagsFull:
            return "bags_full";
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

// The end of a visit whose last walk to the object failed (the object is there; pathing's fault, not a missing one)
inline VisitEnd ApproachFailedEnd() { return VisitEnd::Unreached; }

// An end the visit did not reach by itself
inline bool IsInterruption(VisitEnd end) { return end >= VisitEnd::Combat && end < VisitEnd::Count; }

// The bot puts on what the visit brought only when the visit ends in its own AI update out of combat
inline bool EquipsAfter(VisitEnd end)
{
    return end == VisitEnd::Done || end == VisitEnd::Timeout || end == VisitEnd::NoSpot || end == VisitEnd::Unreached ||
           end == VisitEnd::NoMaterials || end == VisitEnd::NoStation || end == VisitEnd::BagsFull ||
           end == VisitEnd::MapChange;
}

// One visit at a time per bot, across the mailbox, bank and craft visits: why a visit of the kind may not start while
// the running visit is of the kind `running` (none: nullptr)
inline char const* VisitStartRefusal(VisitKind kind, std::optional<VisitKind> running)
{
    if (!running)
        return nullptr;
    if (*running == kind)
        return "a visit runs already";
    return *running == VisitKind::Mail   ? "a mailbox visit runs"
           : *running == VisitKind::Bank ? "a bank visit runs"
                                         : "a craft visit runs";
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
