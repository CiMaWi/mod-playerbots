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
#include "TravelMgr.h"
#include "WorldTravelTypes.h"

// The RPG actions' memos of the bot (PlayerbotAI::rpgMemos): the inn checks, the move-far progress and
// give-ups, the ghost's searches, the quest giver talked to. A reset starts them fresh (StatusEnd::ResetOf)
struct RpgMemos
{
    uint32 lastInnMs{0};                             // the last inn errand check (WorldTravel::InnCheckDue); 0 = never
    std::vector<WorldTravel::AvoidedSpot> innAvoid;  // inn spots whose bind found no usable innkeeper (InnAvoided)

    // MOVE_FAR
    float nearestMoveFarDis{FLT_MAX};
    uint32 stuckTs{0};
    uint32 stuckAttempts{0};
    WorldPosition moveFarPos;
    // The destination given up after the last Stuck event, avoided for WorldTravel.StuckAvoidSeconds
    WorldPosition stuckAvoidPos;
    uint32 stuckAvoidTs{0};
    WorldPosition stuckBotPos;  // where the bot stood at that Stuck event
    uint32 stuckRepeats{0};     // give-ups in a row at the same obstacle (WorldTravel::NextStuckRepeats)
    WorldPosition offMeshPos;   // where the last off-mesh recovery left the bot (seen, no_poly): it backs off there
    uint32 offMeshTs{0};        // its time; 0 = none (WorldTravel::OffMeshBackoff)
    // END MOVE_FAR

    // GHOST (SpiritHealerAction): the last spirit healer search and the last fallback teleport
    uint32 ghostHealerTs{0};
    uint32 ghostTeleportTs{0};

    // QUEST GIVER
    ObjectGuid questGiver;  // giver the bot is walking to or talking with
    uint32 questGiverSince{0};
    uint32 questGiverLastTry{0};
    GuidSet ignoredQuestGivers;  // skipped until the next IDLE
    // END QUEST GIVER

    // clear stuck information for a new destination
    void SetMoveFarTo(WorldPosition pos)
    {
        nearestMoveFarDis = FLT_MAX;
        stuckTs = 0;
        stuckAttempts = 0;
        moveFarPos = pos;
    }
};

#endif
