/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RPGMEMOS_H
#define PLAYERBOTS_RPGMEMOS_H

#include <cfloat>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WorldTravelTypes.h"

// The RPG actions' memos of the bot (PlayerbotAI::rpgMemos): the inn checks, the ghost's searches, the quest giver
// talked to. A reset starts them fresh (StatusEnd::ResetOf); failed attempts are travel's record (BotTravel::Failures)
struct RpgMemos
{
    uint32 lastInnMs{0};  // the last inn errand check (WorldTravel::InnCheckDue); 0 = never

    // GHOST (SpiritHealerAction): the last spirit healer search
    uint32 ghostHealerTs{0};
    // The last `death` row of the ghost's way back (WU33a, AwarenessRules::DeathRowOf): its step and reason, its time
    // and the death (the return's key) it belongs to; cleared at the revive. loginMs is when this AI was created.
    uint8 ghostRowWay{0};
    uint8 ghostRowReason{0};
    uint32 ghostRowTs{0};
    uint32 ghostRowDeath{0};
    uint32 loginMs{getMSTime()};

    // QUEST GIVER
    ObjectGuid questGiver;  // giver the bot is walking to or talking with
    uint32 questGiverSince{0};
    uint32 questGiverLastTry{0};
    GuidSet ignoredQuestGivers;  // skipped until the next IDLE
    // END QUEST GIVER
};

#endif
