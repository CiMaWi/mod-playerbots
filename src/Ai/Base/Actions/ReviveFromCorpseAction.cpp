/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReviveFromCorpseAction.h"

#include "Awareness.h"
#include "CellImpl.h"
#include "Corpse.h"
#include "Event.h"
#include "EventLog.h"
#include "GameGraveyard.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MapMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "TestConstraints.h"
#include "Upkeep.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldTravelConfig.h"
#include "WorldTravelRules.h"

namespace
{
// The seconds since the bot's death (the corpse's ghost time; a test's agedeath constraint ages it)
int64 DeadSeconds(PlayerbotAI* botAI, Corpse const* corpse)
{
    return TestConstraints::DeathAgeSeconds(time(nullptr) - corpse->GetGhostTime(), botAI->testConstraints);
}

// The death row's reason when the ghost took the spirit healer because an enemy player stood at its corpse (WP7)
constexpr char const* CAMPED_REASON = "corpse_camped";

// The death row's reason when the rule of the dead found no clear point near the corpse (awareness AW8)
constexpr char const* NO_CLEAR_POINT_REASON = "no_clear_point";

// How often a ghost that waits near its corpse looks again (a picture is at most 1000 ms old)
constexpr uint32 DEAD_WAIT_RECHECK_MS = 1000;

void StopGhost(Player* bot)
{
    if (!bot->isMoving())
        return;
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
}

// The death the ghost memo belongs to: the corpse's ghost time, 1 for a ghost whose corpse is on another map
uint32 GhostDeathKey(Corpse const* corpse) { return corpse ? static_cast<uint32>(corpse->GetGhostTime()) : 1; }

// The memo of a ghost's way back belongs to one death, so a new death starts fresh whichever way the last ended
void GhostMemoFor(RpgMemos& memo, uint32 death)
{
    if (memo.ghostRowDeath == death)
        return;

    memo.ghostRowWay = 0;
    memo.ghostRowReason = 0;
    memo.ghostRowDeath = death;
    memo.ghostWaitTs = 0;
}

// WU34: a persistent ghost whose corpse is on another map (Player::GetCorpse is map local: a death in an instance, or
// the game's repop sent the ghost to another continent's grave) has no corpse to walk to and takes the spirit healer
bool GhostWithoutCorpse(PlayerbotAI* botAI, Player* bot)
{
    return botAI->KeepsPossessions() && !botAI->HasGameClientMaster() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) &&
           !bot->GetCorpse();
}

// WU33a: the ghost's way back on record. One `death` row when the step or its reason differs from the last row's of
// this death, and a repeat row every GHOST_ROW_REPEAT_MS; never a row per tick. Only persistent bots. `corpse` is null
// when it lies on another map (value -1 then).
void LogGhostWay(PlayerbotAI* botAI, Player* bot, Corpse const* corpse, WorldTravel::GhostWayResult const& now,
                 GraveyardStruct const* grave)
{
    if (now.Way == WorldTravel::GhostWay::None || !botAI->KeepsPossessions())
        return;

    RpgMemos& memo = botAI->rpgMemos;
    GhostMemoFor(memo, GhostDeathKey(corpse));

    WorldTravel::GhostWayResult const last{static_cast<WorldTravel::GhostWay>(memo.ghostRowWay),
                                           static_cast<WorldTravel::GhostReason>(memo.ghostRowReason)};
    if (!WorldTravel::GhostRowDue(last, GetMSTimeDiffToNow(memo.ghostRowTs), now))
        return;

    memo.ghostRowWay = static_cast<uint8>(now.Way);
    memo.ghostRowReason = static_cast<uint8>(now.Reason);
    memo.ghostRowTs = getMSTime();

    float const deadSeconds = corpse ? static_cast<float>(DeadSeconds(botAI, corpse)) : -1.0f;
    WorldLogFormat::Alternatives weighed;
    if (WorldLog::Active())
    {
        weighed.Add(
            {"corpse_yards", corpse && corpse->GetMapId() == bot->GetMapId() ? bot->GetDistance(corpse) : -1.0f});
        weighed.Add(
            {"grave_yards", grave && grave->Map == bot->GetMapId() ? bot->GetDistance2d(grave->x, grave->y) : -1.0f});
        weighed.Add({"login_seconds", static_cast<float>(GetMSTimeDiffToNow(memo.loginMs) / 1000)});
    }
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Death, WorldTravel::GhostWayWord(now.Way),
                       WorldTravel::GhostReasonWord(now.Reason), deadSeconds, weighed, grave ? grave->ID : 0);
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

    // Whether and where to rise is the rule of the dead's (awareness AW8): `find corpse` asks it every tick and
    // returns false only on its Rise, which lets this action, queued below it by `corpse near`, send the reclaim

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

    Corpse* corpse = bot->GetCorpse();
    if (!corpse && !GhostWithoutCorpse(botAI, bot))
        return false;

    bool const honest = botAI->KeepsPossessions();
    if (corpse && !honest && !botAI->HasGameClientMaster() && AI_VALUE(uint32, "death count") >= 5)
    {
        // Upstream's path for a bot that does not keep its possessions: revived and teleported after five deaths
        context->GetValue<uint32>("death count")->Set(0);
        sRandomPlayerbotMgr.Revive(bot);
        return true;
    }

    // The rule of the dead (awareness AW8, INTERFACE.md §3.4) decides; this action carries out the corpse's way
    using AwarenessRules::DeadAct;
    AwarenessRules::DeadStep const before = botAI->awareness ? botAI->awareness->dead.last : AwarenessRules::DeadStep{};
    Awareness::DeadMemo const* dead = &Awareness::NextOfDead(botAI, corpse);
    DeadAct const act = dead->last.act;
    if (act == DeadAct::WalkCorpse || act == DeadAct::WalkPoint)
    {
        // The same walk under way goes on; a new one starts towards the corpse, the leader or the best point
        bool moved = bot->isMoving() && before == dead->last;
        if (!moved)
        {
            Player* leader = botAI->GetGroupLeader();
            if (act == DeadAct::WalkCorpse)
                moved = MoveTo(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(),
                               corpse->GetPositionZ(), false, false);
            else if (dead->last.reason == AwarenessRules::DeadReason::Leader)
                moved = leader && leader != bot &&
                        MoveTo(leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(),
                               leader->GetPositionZ(), false, false);
            else
                moved =
                    MoveTo(bot->GetMapId(), dead->facts.best.x, dead->facts.best.y, dead->facts.best.z, false, false);
        }
        if (moved)
        {
            if (honest)
            {
                WorldTravel::GhostWayInput way;
                way.CorpseBranch = true;
                way.Moving = true;
                LogGhostWay(botAI, bot, corpse, WorldTravel::GhostStepOf(way), nullptr);
            }
            return true;
        }
        // The walk could not start: the rule once more, which takes the healer's way (no_path)
        dead = &Awareness::NextOfDeadWalkFailed(botAI);
    }

    switch (dead->last.act)
    {
        case DeadAct::None:
            return false;
        case DeadAct::WalkCorpse:
        case DeadAct::WalkPoint:
            return true;  // the second ask names no walk (line 3 comes first); nothing more this tick
        case DeadAct::Wait:
            StopGhost(bot);
            if (honest && dead->last.reason == AwarenessRules::DeadReason::NoClearPoint)
                LogGhostWay(botAI, bot, corpse, {WorldTravel::GhostWay::Wait, WorldTravel::GhostReason::NoClearPoint},
                            nullptr);
            botAI->SetNextCheckDelay(dead->last.reason == AwarenessRules::DeadReason::Player && dead->holdRecheckMs
                                         ? dead->holdRecheckMs
                                         : DEAD_WAIT_RECHECK_MS);
            return true;
        case DeadAct::Rise:
            // `revive from corpse` sends the reclaim: `corpse near` (the core's own 39 yd test, as reclaimHere) queues
            // it below this action, and the engine runs it in this tick when this one returns false. A reclaim the
            // core refuses for its delay is sent again on the next tick the rule still says Rise
            StopGhost(bot);
            return false;
        default:
        {
            // The healer's way and the bound: the spirit healer action, with the reason of the choice (the death row's)
            AwarenessRules::DeadReason const why = dead->last.reason != AwarenessRules::DeadReason::WaitEnded &&
                                                           dead->healerChosen &&
                                                           dead->healerReason != AwarenessRules::DeadReason::None
                                                       ? dead->healerReason
                                                       : dead->last.reason;
            botAI->DoSpecificAction("spirit healer", Event("dead step", AwarenessRules::Word(why)), true);
            // True also when the action did nothing this time: never on to the reclaim at the corpse
            return true;
        }
    }
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    return bot->GetCorpse() || GhostWithoutCorpse(botAI, bot);
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
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, travelPos.GetMapId(),
                                          travelPos.GetPositionX(), travelPos.GetPositionY(), travelPos.GetPositionZ());
                ClosestGrave = sGraveyard->GetClosestGraveyard(
                    travelPos.GetMapId(), travelPos.GetPositionX(), travelPos.GetPositionY(), travelPos.GetPositionZ(),
                    bot->GetTeamId(), areaId, zoneId, bot->getClass() == CLASS_DEATH_KNIGHT);

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

// The spirit healer's revive at the grave: free when granted, else what a player pays
// (WorldSession::SendSpiritResurrect), with the death row of a persistent bot. The healer's revive and the wait's
// bound (WU34, reason wait_ended) share it.
bool ReviveAtGrave(PlayerbotAI* botAI, Player* bot, AiObjectContext* context, char const* reason, uint32 dCount,
                   int64 deadTime)
{
    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer ({})", bot->GetGUID().ToString().c_str(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName(), reason);
    PlayerbotChatHandler ch(bot);
    bool const granted = Upkeep::Granted(bot, UpkeepKind::Resurrect);
    bot->ResurrectPlayer(0.5f, !granted);
    if (granted)
        Upkeep::Note(bot, UpkeepKind::Resurrect, "spirit_healer_free");
    else
    {
        bot->DurabilityLossAll(sWorld->getRate(RATE_DURABILITY_LOSS_ON_SPIRIT_RESURRECT) / 100.0f, true);
        if (Aura const* sickness = bot->GetAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
            EventLog::Emit(bot, EventLog::EventType::Sick, "spirit_healer", Upkeep::SPELL_RESURRECTION_SICKNESS,
                           sickness->GetDuration());
    }
    // The death decision row (upkeep §3.14) of a persistent bot: why the spirit healer, not the corpse run
    if (botAI->KeepsPossessions())
    {
        WorldLogFormat::Alternatives weighed;
        if (WorldLog::Active())
            weighed = {{"dead_seconds", static_cast<float>(deadTime)}};
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Death, "spirit_healer", reason,
                           static_cast<float>(dCount / WorldTravel::DEATH_COUNT_PER_DEATH), weighed);
    }
    RpgMemos& memo = botAI->rpgMemos;  // the next death starts fresh (WU33a)
    memo.ghostRowWay = 0;
    memo.ghostRowReason = 0;
    memo.ghostRowTs = 0;
    memo.ghostRowDeath = 0;
    memo.ghostWaitTs = 0;
    bot->SpawnCorpseBones();
    context->GetValue<Unit*>("current target")->Set(nullptr);
    bot->SetTarget();
    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

    if (dCount > 20)
        context->GetValue<uint32>("death count")->Set(0);

    return true;
}
}  // namespace

bool SpiritHealerAction::Execute(Event event)
{
    // GetGrave() picks world graveyards; teleporting there from a BG removes the bot from it.
    if (bot->InBattleground())
        return false;

    Corpse* corpse = bot->GetCorpse();
    bool const keeps = botAI->KeepsPossessions();
    if (!corpse && !GhostWithoutCorpse(botAI, bot))
    {
        botAI->TellError("I am not a spirit");
        return false;
    }

    uint32 dCount = AI_VALUE(uint32, "death count");
    int64 deadTime = corpse ? DeadSeconds(botAI, corpse) : 0;

    // A persistent ghost's grave is the game's: the one nearest to where it stands, where the release sent it (WU34);
    // other bots may take the grave by their travel target or a starting zone
    GraveyardStruct const* ClosestGrave =
        GetGrave(!keeps && (dCount > 10 || deadTime > 15 * MINUTE || AI_VALUE(uint8, "durability") < 10));
    if (!ClosestGrave)
        return false;

    WorldPosition const gravePos(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z);
    bool const atGrave = ClosestGrave->Map == bot->GetMapId() &&
                         bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y) < sPlayerbotAIConfig.SightDistance;
    RpgMemos& memo = botAI->rpgMemos;
    GhostMemoFor(memo, GhostDeathKey(corpse));
    WorldTravel::GhostWayInput way;  // WU33a: what this call decides, named once per branch
    way.AtGrave = atGrave;
    way.RunsExhausted = WorldTravel::CorpseRunsExhausted(dCount, sPlayerbotAIConfig.WorldTravel.CorpseRunFailures);
    way.DeadTooLong = deadTime >= 10 * MINUTE || dCount >= 5;
    way.Camped = event.getParam() == CAMPED_REASON;
    way.NoClearPoint = event.getParam() == NO_CLEAR_POINT_REASON;
    way.CorpseAway = !corpse;
    if (atGrave && WorldTravel::RetryDue(memo.ghostHealerTs != 0, GetMSTimeDiffToNow(memo.ghostHealerTs),
                                         WorldTravel::GHOST_HEALER_RETRY_MS))
    {
        memo.ghostHealerTs = getMSTime();
        if (FindSpiritHealer(bot, sPlayerbotAIConfig.SightDistance))
        {
            char const* reason = way.Camped                ? CAMPED_REASON
                                 : way.RunsExhausted       ? "corpse_runs_exhausted"
                                 : way.CorpseAway          ? "corpse_away"
                                 : way.NoClearPoint        ? NO_CLEAR_POINT_REASON
                                 : deadTime >= 10 * MINUTE ? "dead_too_long"
                                                           : "no_path";
            return ReviveAtGrave(botAI, bot, context, reason, dCount, deadTime);
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

    way.Moving = moved;
    if (!moved && botAI->IsPersistentBot())
    {
        // A persistent ghost at its grave waits for the healer; a far one is teleported unseen and rate-limited
        WorldTravel::GhostInput ghost;
        ghost.AtGrave = atGrave;
        ghost.TeleportedBefore = memo.ghostTeleportTs != 0;
        ghost.SinceTeleportMs = ghost.TeleportedBefore ? GetMSTimeDiffToNow(memo.ghostTeleportTs) : 0;
        if (WorldTravel::GhostFallback(ghost) == WorldTravel::GhostStep::Teleport)
            ghost.Seen = sRandomPlayerbotMgr.IsTeleportSeen(bot, gravePos);

        way.Fallback = WorldTravel::GhostFallback(ghost);
        way.Seen = ghost.Seen;
    }

    // Every ghost wait ends (WU34): the death's wait clock starts at its first wait step; at the bound the ghost is
    // revived at its grave for the spirit healer's price, or first teleported there
    if (keeps)
    {
        memo.ghostWaitTs =
            WorldTravel::GhostWaitStart(memo.ghostWaitTs, WorldTravel::GhostStepOf(way).Way, getMSTime());
        way.WaitedMs = memo.ghostWaitTs ? GetMSTimeDiffToNow(memo.ghostWaitTs) : 0;
    }

    WorldTravel::GhostWayResult const step = WorldTravel::GhostStepOf(way);
    if (step.Way == WorldTravel::GhostWay::Revive)
    {
        EventLog::Honesty(bot, EventLog::HonestyKind::Resurrect, "wait_ended", ClosestGrave->ID);
        return ReviveAtGrave(botAI, bot, context, "wait_ended", dCount, deadTime);
    }

    LogGhostWay(botAI, bot, corpse, step, ClosestGrave);
    if (moved)
        return true;

    if (botAI->IsPersistentBot())
    {
        if (step.Way != WorldTravel::GhostWay::Teleport)
            return false;

        memo.ghostTeleportTs = getMSTime();
    }
    else if (sRandomPlayerbotMgr.IsTeleportSeen(bot, gravePos))
        return false;

    context->GetValue<uint32>("death count")->Set(dCount + 1);
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bool const teleported = bot->TeleportTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.f);
    EventLog::Honesty(bot, EventLog::HonestyKind::Teleport, "spirit_healer_fallback", ClosestGrave->Map);
    return teleported;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }
