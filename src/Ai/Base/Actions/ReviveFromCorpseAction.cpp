/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReviveFromCorpseAction.h"

#include "CellImpl.h"
#include "CensusMgr.h"
#include "Corpse.h"
#include "Event.h"
#include "FleeManager.h"
#include "GameGraveyard.h"
#include "GhostWait.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MapMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "Upkeep.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldTravelRules.h"

namespace
{
// The death row's reason when the ghost took the spirit healer because an enemy player stood at its corpse (WP7)
constexpr char const* CAMPED_REASON = "corpse_camped";

// No camping (WP7): the ghost of a persistent bot keeps away from its corpse while an enemy player stands there and
// takes the spirit healer after its wait; true when that decided the action (wait or spirit healer)
bool KeepsAwayFromCorpse(PlayerbotAI* botAI, Player* bot, Corpse const* corpse)
{
    WorldPvp::GhostPlan const ghost = WorldPvp::PlanGhostFor(botAI, corpse);
    if (ghost.step == WorldPvp::GhostStep::Corpse)
        return false;
    if (ghost.step == WorldPvp::GhostStep::SpiritHealer)
    {
        // Never on to the corpse, also when the spirit healer action did nothing this time
        botAI->DoSpecificAction("spirit healer", Event("ghost wait", CAMPED_REASON), true);
        return true;
    }
    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }
    botAI->SetNextCheckDelay(ghost.recheckMs);
    return true;
}
}  // namespace

// Top of the default start-zone brackets (Playerbots.ZoneBracket.*).
constexpr uint32 START_ZONE_GRAVEYARD_MAX_LEVEL = 12;

bool ReviveFromCorpseAction::Execute(Event event)
{
    // BG ghosts are revived by spirit healer waves; teleporting to a world graveyard removes them from the BG.
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();

    // follow group Leader when group Leader revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && groupLeader && !corpse && bot->IsAlive())
    {
        if (ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
        {
            if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            {
                botAI->TellMasterNoFacing("Welcome back!");
                botAI->ChangeStrategy("+follow,-stay", BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    // if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) >
    // time(nullptr))
    //     return false;

    if (groupLeader)
    {
        if (!GET_PLAYERBOT_AI(groupLeader) && groupLeader->isDead() && groupLeader->GetCorpse() &&
            ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
            return false;
    }

    if (!botAI->HasGameClientMaster())
    {
        uint32 dCount = AI_VALUE(uint32, "death count");
        bool const exhausted =
            RandomPlayerbotMgr::HonestTravel()
                ? WorldTravel::CorpseRunsExhausted(dCount, sPlayerbotAIConfig.WorldTravel.CorpseRunFailures)
                : dCount >= 5;

        if (exhausted)
            return botAI->DoSpecificAction("spirit healer");

        if (KeepsAwayFromCorpse(botAI, bot, corpse))
            return true;
    }

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at body", bot->GetGUID().ToString().c_str(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();

    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetGUID();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    return true;
}

bool FindCorpseAction::Execute(Event /*event*/)
{
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;

    // if (groupLeader)
    // {
    //     if (!GET_PLAYERBOT_AI(groupLeader) &&
    //         ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
    //         sPlayerbotAIConfig.FarDistance)) return false;
    // }

    uint32 dCount = AI_VALUE(uint32, "death count");
    bool const honest = RandomPlayerbotMgr::HonestTravel();

    if (!botAI->HasGameClientMaster())
    {
        // A persistent bot whose corpse runs failed walks to the spirit healer; it is never revived in place
        if (honest && WorldTravel::CorpseRunsExhausted(dCount, sPlayerbotAIConfig.WorldTravel.CorpseRunFailures))
            return botAI->DoSpecificAction("spirit healer", Event(), true);

        if (!honest && dCount >= 5)
        {
            // LOG_INFO("playerbots", "Bot {} {}:{} <{}>: died too many times, was revived and teleported",
            //     bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
            //     bot->GetName().c_str());
            context->GetValue<uint32>("death count")->Set(0);
            // sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
            sRandomPlayerbotMgr.Revive(bot);
            return true;
        }

        if (KeepsAwayFromCorpse(botAI, bot, corpse))
            return true;
    }

    WorldPosition botPos(bot);
    WorldPosition corpsePos(corpse);
    WorldPosition moveToPos = corpsePos;
    WorldPosition leaderPos(groupLeader);

    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    bool moveToLeader = groupLeader && groupLeader != bot && leaderPos.fDist(corpsePos) < reclaimDist;

    // Should we ressurect? If so, return false.
    if (corpseDist < reclaimDist)
    {
        if (moveToLeader)  // We are near group leader.
        {
            if (botPos.fDist(leaderPos) < sPlayerbotAIConfig.SpellDistance)
                return false;
        }
        else if (deadTime > 8 * MINUTE)  // We have walked too long already.
            return false;
        else
        {
            GuidVector units = AI_VALUE(GuidVector, "possible targets no los");

            if (botPos.getUnitsAggro(units, bot) == 0)  // There are no mobs near.
                return false;
        }
    }

    // If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.ReactDistance)
    {
        if (moveToLeader)
            moveToPos = leaderPos;
        else
        {
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                    moveToPos = WorldPosition(moveToPos.GetMapId(), rx, ry, rz, 0.0);
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                    moveToPos = corpsePos;
            }
        }
    }

    // Actual mobing part.
    bool moved = false;

    if (!botAI->AllowActivity(ALL_ACTIVITY))
    {
        uint32 delay = ServerFacade::instance().GetDistance2d(bot, corpse) /
                       bot->GetSpeed(MOVE_RUN);        // Time a bot would take to travel to it's corpse.
        delay = std::min(delay, uint32(10 * MINUTE));  // Cap time to get to corpse at 10 minutes.

        // The sight rule: a persistent ghost seen by a real player keeps waiting
        if (deadTime > delay && !sRandomPlayerbotMgr.IsTeleportSeen(bot, moveToPos))
        {
            bot->GetMotionMaster()->Clear();
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ(), 0);
            EventLog::Honesty(bot, "Teleport", "ghost_to_corpse", moveToPos.GetMapId());
        }

        moved = true;
    }
    else
    {
        if (bot->isMoving())
            moved = true;
        else
        {
            if (deadTime < 10 * MINUTE && dCount < 5)  // Look for corpse up to 30 minutes.
            {
                moved =
                    MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ(), false, false);
            }

            if (!moved)
            {
                moved = botAI->DoSpecificAction("spirit healer", Event(), true);
            }
        }
    }

    return moved;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    return bot->GetCorpse();
}

GraveyardStruct const* SpiritHealerAction::GetGrave(bool startZone)
{
    GraveyardStruct const* ClosestGrave = nullptr;
    GraveyardStruct const* NewGrave = nullptr;

    ClosestGrave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    GraveyardStruct const* ownGrave = ClosestGrave;

    if (!startZone && ClosestGrave)
        return ClosestGrave;

    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) && botAI->GetGroupLeader() && botAI->GetGroupLeader() != bot)
    {
        Player* groupLeader = botAI->GetGroupLeader();
        if (groupLeader && groupLeader != bot)
        {
            ClosestGrave = sGraveyard->GetClosestGraveyard(groupLeader, bot->GetTeamId());

            if (ClosestGrave)
                return ClosestGrave;
        }
    }
    else if (startZone && AI_VALUE(uint8, "durability"))
    {
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        if (travelTarget->getPosition())
        {
            WorldPosition travelPos = *travelTarget->getPosition();
            if (travelPos.GetMapId() != uint32(-1))
            {
                uint32 areaId = 0;
                uint32 zoneId = 0;
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, travelPos.GetMapId(), travelPos.GetPositionX(),
                                          travelPos.GetPositionY(), travelPos.GetPositionZ());
                ClosestGrave = sGraveyard->GetClosestGraveyard(travelPos.GetMapId(), travelPos.GetPositionX(), travelPos.GetPositionY(),
                                                               travelPos.GetPositionZ(), bot->GetTeamId(), areaId, zoneId,
                                                               bot->getClass() == CLASS_DEATH_KNIGHT);

                if (ClosestGrave)
                    return ClosestGrave;
            }
        }
    }

    if (ownGrave && bot->GetLevel() > START_ZONE_GRAVEYARD_MAX_LEVEL)
        return ownGrave;

    std::vector<uint32> races;

    if (bot->GetTeamId() == TEAM_ALLIANCE)
        races = {RACE_HUMAN, RACE_DWARF, RACE_GNOME, RACE_NIGHTELF, RACE_DRAENEI};
    else
        races = {RACE_ORC, RACE_TROLL, RACE_TAUREN, RACE_UNDEAD_PLAYER, RACE_BLOODELF};

    float graveDistance = -1;

    WorldPosition botPos(bot);

    for (auto race : races)
    {
        for (uint32 cls = 0; cls < MAX_CLASSES; cls++)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
            if (!info)
                continue;

            uint32 areaId = 0;
            uint32 zoneId = 0;
            sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, info->mapId, info->positionX,
                                      info->positionY, info->positionZ);

            NewGrave = sGraveyard->GetClosestGraveyard(info->mapId, info->positionX, info->positionY, info->positionZ,
                                                       bot->GetTeamId(), areaId, zoneId, cls == CLASS_DEATH_KNIGHT);
            if (!NewGrave)
                continue;

            WorldPosition gravePos(NewGrave->Map, NewGrave->x, NewGrave->y, NewGrave->z);

            float newDist = botPos.fDist(gravePos);

            if (graveDistance < 0 || newDist < graveDistance)
            {
                ClosestGrave = NewGrave;
                graveDistance = newDist;
            }
        }
    }

    return ClosestGrave;
}

namespace
{
// A living spirit healer within range; no line of sight needed ("nearest npcs" requires it and missed healers behind
// a wall or a slope, so ghosts at their grave teleported to it again and again)
class SpiritHealerInRange
{
public:
    SpiritHealerInRange(WorldObject const* origin, float range) : _origin(origin), _range(range) {}

    bool operator()(Creature* creature)
    {
        if (!creature->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER) || !creature->IsAlive() ||
            !_origin->IsWithinDistInMap(creature, _range))
            return false;

        _range = _origin->GetDistance(creature);  // the last match is the nearest
        return true;
    }

private:
    WorldObject const* _origin;
    float _range;
};

Creature* FindSpiritHealer(Player* bot, float range)
{
    Creature* healer = nullptr;
    SpiritHealerInRange check(bot, range);
    Acore::CreatureLastSearcher<SpiritHealerInRange> searcher(bot, healer, check);
    Cell::VisitObjects(bot, searcher, range);
    return healer;
}
}  // namespace

bool SpiritHealerAction::Execute(Event event)
{
    // GetGrave() picks world graveyards; teleporting there from a BG removes the bot from it.
    if (bot->InBattleground())
        return false;

    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        botAI->TellError("I am not a spirit");
        return false;
    }

    uint32 dCount = AI_VALUE(uint32, "death count");
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    GraveyardStruct const* ClosestGrave =
        GetGrave(dCount > 10 || deadTime > 15 * MINUTE || AI_VALUE(uint8, "durability") < 10);
    if (!ClosestGrave)
        return false;

    WorldPosition const gravePos(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z);
    bool const atGrave = ClosestGrave->Map == bot->GetMapId() &&
                         bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y) < sPlayerbotAIConfig.SightDistance;
    NewRpgInfo& info = botAI->rpgInfo;
    if (atGrave && WorldTravel::RetryDue(info.ghostHealerTs != 0, GetMSTimeDiffToNow(info.ghostHealerTs),
                                         WorldTravel::GHOST_HEALER_RETRY_MS))
    {
        info.ghostHealerTs = getMSTime();
        if (FindSpiritHealer(bot, sPlayerbotAIConfig.SightDistance))
        {
            LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer", bot->GetGUID().ToString().c_str(),
                      bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
            PlayerbotChatHandler ch(bot);
            // Free when granted, else what a player pays (WorldSession::SendSpiritResurrect)
            bool const granted = Upkeep::Granted(bot, UpkeepKind::Resurrect);
            bot->ResurrectPlayer(0.5f, !granted);
            if (granted)
                Upkeep::Note(bot, UpkeepKind::Resurrect, "spirit_healer_free");
            else
            {
                bot->DurabilityLossAll(sWorld->getRate(RATE_DURABILITY_LOSS_ON_SPIRIT_RESURRECT) / 100.0f, true);
                if (Aura const* sickness = bot->GetAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
                    EventLog::Emit(bot, Upkeep::EVENT_SICK, "spirit_healer", Upkeep::SPELL_RESURRECTION_SICKNESS,
                                   sickness->GetDuration());
            }
            // The death decision row (upkeep §3.14) of a persistent bot: why the spirit healer, not the corpse run
            if (RandomPlayerbotMgr::HonestTravel() && !botAI->HasGameClientMaster())
            {
                bool const exhausted =
                    WorldTravel::CorpseRunsExhausted(dCount, sPlayerbotAIConfig.WorldTravel.CorpseRunFailures);
                char const* reason = event.getParam() == CAMPED_REASON ? CAMPED_REASON
                                     : exhausted ? "corpse_runs_exhausted"
                                     : (deadTime >= 10 * MINUTE ? "dead_too_long" : "no_path");
                WorldLogFormat::Alternatives weighed;
                if (WorldLog::Active())
                    weighed = {{"dead_seconds", static_cast<float>(deadTime)}};
                WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Death, "spirit_healer", reason,
                                   static_cast<float>(dCount / WorldTravel::DEATH_COUNT_PER_DEATH), weighed);
            }
            bot->SpawnCorpseBones();
            context->GetValue<Unit*>("current target")->Set(nullptr);
            bot->SetTarget();
            botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

            if (dCount > 20)
                context->GetValue<uint32>("death count")->Set(0);

            return true;
        }

        LOG_DEBUG("playerbots", "Bot {} <{}>: no spirit healer within {} yd at graveyard {} (map {})",
                  bot->GetGUID().ToString().c_str(), bot->GetName(), sPlayerbotAIConfig.SightDistance, ClosestGrave->ID,
                  ClosestGrave->Map);
    }

    bool moved = false;

    if (bot->IsWithinLOS(ClosestGrave->x, ClosestGrave->y, ClosestGrave->z))
        moved = MoveNear(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.0);
    else
        moved = MoveTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, false, false);

    if (moved)
        return true;

    if (sRandomPlayerbotMgr.IsRandomBot(bot) && RandomPlayerbotMgr::HonestTravel())
    {
        // A persistent ghost at its grave waits for the healer; a far one is teleported unseen and rate-limited
        WorldTravel::GhostInput ghost;
        ghost.AtGrave = atGrave;
        ghost.TeleportedBefore = info.ghostTeleportTs != 0;
        ghost.SinceTeleportMs = ghost.TeleportedBefore ? GetMSTimeDiffToNow(info.ghostTeleportTs) : 0;
        if (WorldTravel::GhostFallback(ghost) == WorldTravel::GhostStep::Teleport)
            ghost.Seen = sRandomPlayerbotMgr.IsTeleportSeen(bot, gravePos);

        if (WorldTravel::GhostFallback(ghost) != WorldTravel::GhostStep::Teleport)
            return false;

        info.ghostTeleportTs = getMSTime();
    }
    else if (sRandomPlayerbotMgr.IsTeleportSeen(bot, gravePos))
        return false;

    // if (!IsRealPlayer(botAI->GetMaster()))
    // {
    context->GetValue<uint32>("death count")->Set(dCount + 1);
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bool const teleported = bot->TeleportTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.f);
    EventLog::Honesty(bot, "Teleport", "spirit_healer_fallback", ClosestGrave->Map);
    return teleported;
    // }

    // LOG_INFO("playerbots", "Bot {} {}:{} <{}> can't find a spirit healer", bot->GetGUID().ToString().c_str(),
    //          bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    // botAI->TellError("Cannot find any spirit healer nearby");
    return false;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }
