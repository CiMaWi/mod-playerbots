/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_VISITSTATE_H
#define _PLAYERBOT_VISITSTATE_H

#include "BankMailTypes.h"
#include "Define.h"
#include "ObjectGuid.h"

// The one visit a bot runs (Visit.h drives it): what every kind of visit keeps alike. A kind keeps its own data beside
// it in BotBankMail (BankMailState.h). Apart from Visit.h so that the state's includers do not see the driver.
namespace BankMail
{
enum class VisitPhase : uint8
{
    None,  // no visit
    Walk,  // on the way to the object's spawn, or into its reach
    Act,   // in reach: the kind acts once per step
    Await  // the kind acted and awaits the answer in a later step
};

struct VisitState
{
    VisitKind kind{VisitKind::Bank};
    VisitPhase phase{VisitPhase::None};
    uint32 mapId{0};  // the object's spawn, from the planner's service index
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    uint32 entry{0};      // the object's template, looked for near the spawn
    ObjectGuid object;    // found once the bot stands near the spawn; resolved at each use
    bool inPlace{false};  // no object: the kind acts where the bot stands (a craft without a spell focus)
    uint32 startedMs{0};
    uint32 limitMs{0};  // from startedMs, the walk included; the auction visit renews it (RenewedLimitMs)
};

// A visit of the kind runs in the one state
inline bool VisitRunsOf(VisitState const& visit, VisitKind kind)
{
    return visit.phase != VisitPhase::None && visit.kind == kind;
}
}  // namespace BankMail

#endif
