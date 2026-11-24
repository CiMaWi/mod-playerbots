/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <optional>
#include <variant>
#include <vector>

#include "BotTravel.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GatherNodeMgr.h"
#include "GroundRules.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "Map.h"
#include "NewRpgAction.h"
#include "NewRpgBaseAction.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Position.h"
#include "ProfessionConfig.h"
#include "PursuitRules.h"
#include "QuestDef.h"
#include "QuestFailRecord.h"
#include "Random.h"
#include "RpgNavMeshSnap.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "Upkeep.h"
#include "UpkeepRules.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"

using namespace NewRpgInternal;

namespace
{
uint32 constexpr GRIND_PICK_TRIES = 3;

// A grind spot is a 50 yd cell centre (TravelMgr rounds x, y and z), so it can sit off the ground or inside a rock.
// It is kept when the nav mesh has a poly a bot may walk (the bots' path filter) within 3 yd across and 50 yd up or
// down, and moves onto that poly; on a tile not loaded yet nothing can be judged and the spot stays as it is.
bool SnapToNavMesh(Player* bot, WorldPosition& spot)
{
    float x = spot.GetPositionX();
    float y = spot.GetPositionY();
    float z = spot.GetPositionZ();
    NavMeshSnap::Result const result = NavMeshSnap::Nearest(bot->GetMap(), x, y, z, 3.0f, 50.0f);
    if (result == NavMeshSnap::Result::Found)
        spot.Relocate(x, y, z);
    return result != NavMeshSnap::Result::None;
}

WorldTravel::SightPoint SightOf(WorldLocation const& pos)
{
    return {pos.GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()};
}

std::optional<WorldTravel::SightPoint> PlaceOf(WorldLocation const& loc) { return SightOf(loc); }

// The pickers read the record of failed attempts (PA32): a place it refuses (a route none, a refused approach) is not
// drawn, and where it refuses the bot's start (off_mesh, takeoff) nothing is, so a bot told "no route" picks another
// place at its next pick instead of asking again
template <typename T, typename Place>
void KeepOffered(Player* bot, std::vector<T>& candidates, Place const& placeOf)
{
    if (PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot))
        FailedAttempts::KeepOffered(ai->GetTravel().Failures(), candidates, SightOf(*bot), getMSTime(), placeOf);
}
}  // namespace

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    std::optional<std::vector<uint32>> givenUp;  // read on the first ask
    std::function<bool(uint32)> const workable = [this, &givenUp](uint32 questId)
    {
        if (!givenUp)
            givenUp = NewRpgInternal::GivenUpQuests(bot);
        return !QuestFail::IsGivenUp(*givenUp, questId) && HasQuestPlace(questId);
    };
    Planner::PlannerHook(botAI, bot, workable);
    // GM orders were taken before the engine ran (TravelOrders::TakeAndApply in PlayerbotAI::UpdateAI)
    if (NewRpgTravelAction::ConsumeScheduledWork(botAI))
        return true;
    NewRpgStatus status = info.GetStatus();
    // Resurrection sickness (upkeep §3.5): a sick bot rests until the sickness ends. Any other status ends, an errand
    // and a trip as given up (errand_sick, abort:sick); a flight in the air lands first. The status pick, the errand
    // start and the travel orders keep it resting
    bool const sick = bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    if (Upkeep::SickEndsStatus(status, sick, bot->IsInFlight()))
    {
        RpgStatus::Start(botAI, NewRpgInfo::Rest{}, "sick", StatusEnd::Outcome::Sick);  // one Status row
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    // A bot aboard a vessel with no trip gets off at its next stop (PA39b): the status waits while it does
    if (NewRpgInternal::VesselLeaveStep(botAI, bot))
        return true;
    switch (status)
    {
        case RPG_IDLE:
        {
            // A test's hold (TestConstraints): a held choice is not made; an order was taken all the same
            TestConstraints::IdleGate const gate = TestConstraints::GateOf(botAI->testConstraints);
            if (!gate.planner && !gate.status)
                return false;
            // A bot in the air or off its floor gets to ground before it picks (pathing PA31)
            if (GetToGround(Ground::Origin::Pick))
                return true;
            // A planner bot's goal (WT16) or its counted fallback; the weighted pick for a bot whose planner a test
            // holds or that keeps no possessions. A sick bot's plan ranks Rest first (upkeep §3.5, WU16); the weighted
            // pick lets it only rest
            Planner::IdleStep step;
            if (gate.planner)
                step = Planner::OnIdle(botAI, bot, workable);
            if (step.started)
                return true;
            if (step.wait || !gate.status)
                return false;
            return RandomChangeStatus(Planner::IdleStatuses(step), step.reason, step.note);
        }

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            if (Planner::StatusOver(botAI, status, false))  // a planned grind's overrun on the way
                return true;
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                RpgStatus::Start(botAI, NewRpgInfo::WanderRandom{}, "arrived", StatusEnd::Outcome::Arrived);
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                RpgStatus::Start(botAI, NewRpgInfo::WanderNpc{}, "arrived", StatusEnd::Outcome::Arrived);
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE: a planned grind spot by its order, a fallback by its length (StatusOver)
            if (Planner::StatusOver(botAI, status, info.HasStatusPersisted(statusWanderRandomDuration)))
                return true;
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (Planner::StatusOver(botAI, status, info.HasStatusPersisted(statusWanderNpcDuration)))
                return true;
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE: a backstop beside the order's give-up clock (PursuitRules.h), a give-up like it
            // (questing WT35c: its failure row and record). An order is bounded by its own minutes (questing WQ19a)
            if (!std::get<NewRpgInfo::DoQuest>(info.data).order && info.HasStatusPersisted(statusDoQuestDuration))
            {
                RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = Pursuit::TIMEOUT});
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival; a quest giver near the landing is visited first, driven by the travel action
                // (upstream #3026)
                if (AI_VALUE(ObjectGuid, "new rpg quest giver").IsEmpty())
                {
                    RpgStatus::End(botAI, StatusEnd::Outcome::Arrived);
                    return true;
                }
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE: a planned rest once rested; a sick bot's rest lasts until the sickness ends
            if (Planner::StatusOver(botAI, status, Upkeep::RestEnds(info.HasStatusPersisted(statusRestDuration), sick),
                                    sick))
                return true;
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                RpgStatus::End(botAI, StatusEnd::Outcome::TimedOut);
                return true;
            }
            break;
        }
        case RPG_DO_GATHER:
        case RPG_DO_FISH:
        {
            // DO_GATHER -> IDLE: a planned gather by its order's minutes, else by the timer. DO_FISH: a trip its own
            // rules have not ended by Fish.TripDeadlineSeconds is given up (its FishTrip row says "deadline")
            int32 const limit = status == RPG_DO_GATHER ? statusDoGatherDuration
                                                        : sPlayerbotAIConfig.Professions.FishTripDeadlineSeconds * 1000;
            if (Planner::StatusOver(botAI, status, info.HasStatusPersisted(limit)))
                return true;
            break;
        }
        case RPG_ERRANDS:
        {
            // ERRANDS -> IDLE: errands that have not ended in time are given up; their clock leaves out the time
            // the status's trips ran (WT64c: a trip held for the status counts nothing)
            auto const* errands = std::get_if<NewRpgInfo::Errands>(&info.data);
            BotTravel const& travel = botAI->GetTravel();
            HeldTrip const* trip = travel.TripHolder() == StatusEnd::STATUS_ERRANDS ? travel.Trip() : nullptr;
            if (StatusEnd::NoTripMs(GetMSTimeDiffToNow(info.startT), errands ? errands->tripMs : 0,
                                    trip ? GetMSTimeDiffToNow(trip->tripStartMs) : 0) > WorldTravel::ERRAND_DEADLINE_MS)
            {
                RpgStatus::End(botAI, StatusEnd::Outcome::TimedOut);
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    // There: the status update starts the grind's wander
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
        return Away(data->pos, PLACE_REACH_YARDS).value_or(false);

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    // There: the status update starts the camp's wander
    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
        return Away(data->pos, PLACE_REACH_YARDS).value_or(false);

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // Prefer a giver with something to accept or reward, else any nearby npc; none -> IDLE. One whose place the
        // record of failed attempts refuses (its approach failed a moment ago) is not picked again
        auto const placeOf = [this](ObjectGuid const& guid) -> std::optional<WorldTravel::SightPoint>
        {
            if (WorldObject* const object = ObjectAccessor::GetWorldObject(*bot, guid))
                return SightOf(*object);
            return std::nullopt;
        };
        GuidVector giver;
        if (ObjectGuid const found = NewRpgHelpers::FindQuestGiver(botAI); !found.IsEmpty())
            giver.push_back(found);
        KeepOffered(bot, giver, placeOf);
        ObjectGuid npcOrGo = giver.empty() ? ObjectGuid() : giver.front();
        if (npcOrGo.IsEmpty())
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            KeepOffered(bot, possibleTargets, placeOf);
            if (!possibleTargets.empty())
            {
                uint32 idx = urand(0, possibleTargets.size() - 1);
                WorldObject* object = ObjectAccessor::GetWorldObject(*bot, possibleTargets[idx]);
                if (object && object->IsInWorld())
                    npcOrGo = object->GetGUID();
            }
        }
        if (npcOrGo.IsEmpty())
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Finished, {.word = "no_target"});
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);

            Creature* creature = object->ToCreature();
            if (creature && TestConstraints::VendorStops(botAI->testConstraints) && !Planner::Owns(botAI))
            {
                constexpr uint32 MIN_VENDOR_BALANCE = 1000;  // 10 silver
                if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && AI_VALUE(bool, "can sell"))
                {
                    botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);

                    // Buying sorts the vendor list through the weight calculator; skip it for
                    // bots without money for cheap consumables.
                    if (bot->GetMoney() >= MIN_VENDOR_BALANCE)
                        botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);
                }

                if (creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && AI_VALUE(bool, "can repair"))
                    botAI->DoSpecificAction("repair", Event("repair"), true);
            }

            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        // The npc is the status's goal: an approach that fails ends the status with its cause (no random step); an
        // npc that is gone is passed
        if (Approach(data.npcOrGo, INTERACTION_DISTANCE, true) == ::GoTo::Answer::Failed)
            if (auto* const still = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                still->npcOrGo = ObjectGuid();
        return true;
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    KeepOffered(bot, hi_prepared_locs, PlaceOf);
    KeepOffered(bot, lo_prepared_locs, PlaceOf);
    // At most GRIND_PICK_TRIES draws: a spot off the nav mesh is dropped and another drawn, else no spot
    WorldPosition dest{};
    uint32 rejected = 0;
    for (uint32 attempt = 0; attempt < GRIND_PICK_TRIES; ++attempt)
    {
        std::vector<WorldLocation>* pool = nullptr;
        if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
            pool = &hi_prepared_locs;
        else if (!lo_prepared_locs.empty())
            pool = &lo_prepared_locs;
        if (!pool)
            break;

        uint32 const idx = urand(0, pool->size() - 1);
        WorldPosition candidate = (*pool)[idx];
        if (SnapToNavMesh(bot, candidate))
        {
            dest = candidate;
            break;
        }

        ++rejected;
        (*pool)[idx] = pool->back();
        pool->pop_back();
    }
    if (rejected)
        LOG_DEBUG("playerbots", "[New RPG] Bot {} grind pos: {} spots off the nav mesh dropped", bot->GetName(),
                  rejected);
    LOG_DEBUG("playerbots",
              "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({} near, {} in range of {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    KeepOffered(bot, prepared_locs, PlaceOf);
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos,
                                                  std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;
    // A flight master whose place the record of failed attempts refuses is not walked to again (PA32)
    if (!FailedAttempts::Offered(botAI->GetTravel().Failures(), SightOf(info->pos), SightOf(*bot), getMSTime()))
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

static_assert(RPG_STATUS_END == WorldLog::RPG_STATUS_WORD_COUNT && RPG_DO_FISH == 12 && RPG_ERRANDS == 11 &&
                  RPG_TRAVEL == 10 && RPG_DO_GATHER == 9 && RPG_REST == 7,
              "WorldLog::RPG_STATUS_WORDS follows NewRpgStatus");
static_assert(uint32(RPG_GO_GRIND) == Upkeep::RPG_STATUS_GO_GRIND &&
                  uint32(RPG_WANDER_RANDOM) == Upkeep::RPG_STATUS_WANDER_RANDOM &&
                  uint32(RPG_DO_QUEST) == Upkeep::RPG_STATUS_DO_QUEST &&
                  uint32(RPG_OUTDOOR_PVP) == Upkeep::RPG_STATUS_OUTDOOR_PVP &&
                  uint32(RPG_DO_GATHER) == Upkeep::RPG_STATUS_DO_GATHER && RPG_STATUS_END <= 32,
              "UpkeepRules.h's fighting statuses follow NewRpgStatus");
static_assert(uint32(RPG_IDLE) == Upkeep::RPG_STATUS_IDLE && uint32(RPG_GO_CAMP) == Upkeep::RPG_STATUS_GO_CAMP &&
                  uint32(RPG_WANDER_NPC) == Upkeep::RPG_STATUS_WANDER_NPC &&
                  uint32(RPG_REST) == Upkeep::RPG_STATUS_REST && uint32(RPG_ERRANDS) == Upkeep::RPG_STATUS_ERRANDS,
              "UpkeepRules.h's errand statuses follow NewRpgStatus");

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus, char const* reason, uint32 ref)
{
    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    // A bot with resurrection sickness rests (upkeep §3.5). A bot low on health or mana with nothing to eat or drink
    // (no food cheat) picks no status that fights; with none left it rests without eating (§3.6)
    bool const sick = bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    bool const hungry = AI_VALUE2(uint8, "health", "self target") < sPlayerbotAIConfig.LowHealth &&
                        !AI_VALUE2(uint32, "item count", "food");
    bool const thirsty = AI_VALUE2(bool, "has mana", "self target") &&
                         AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.LowMana &&
                         !AI_VALUE2(uint32, "item count", "drink");
    bool const needsRest = (hungry || thirsty) && !bot->IsInCombat() && !Upkeep::FoodCheat(botAI);
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0 || !Upkeep::StatusPickable(status, sick, needsRest))
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        RpgStatus::Start(botAI, NewRpgInfo::Rest{}, "pick");
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::RpgStatus, WorldLog::NewRpgStatusWord(RPG_REST),
                           "none_available", 0.0f);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    uint32 chosenWeight = 0;
    for (NewRpgStatus status : availableStatus)
    {
        accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (accumulate >= rand)
        {
            chosenStatus = status;
            chosenWeight = sPlayerbotAIConfig.RpgStatusProbWeight[status];
            break;
        }
    }
    // The world log's decision row weighs the chosen status against the three heaviest other available ones
    WorldLogFormat::Alternatives alternatives;
    if (WorldLog::Active())
        for (NewRpgStatus status : availableStatus)
            if (status != chosenStatus)
                alternatives.Offer({WorldLog::NewRpgStatusWord(status),
                                    static_cast<float>(sPlayerbotAIConfig.RpgStatusProbWeight[status])},
                                   false);

    auto const startStatus = [this](NewRpgStatus status) -> bool
    {
        switch (status)
        {
            case RPG_WANDER_RANDOM:
            {
                RpgStatus::Start(botAI, NewRpgInfo::WanderRandom{}, "pick");
                return true;
            }
            case RPG_WANDER_NPC:
            {
                RpgStatus::Start(botAI, NewRpgInfo::WanderNpc{}, "pick");
                return true;
            }
            case RPG_GO_GRIND:
            {
                WorldPosition pos = SelectRandomGrindPos(bot);
                if (pos != WorldPosition())
                {
                    RpgStatus::Start(botAI, NewRpgInfo::GoGrind{pos}, "pick");
                    return true;
                }
                return false;
            }
            case RPG_GO_CAMP:
            {
                WorldPosition pos = SelectRandomCampPos(bot);
                if (pos != WorldPosition())
                {
                    RpgStatus::Start(botAI, NewRpgInfo::GoCamp{pos}, "pick");
                    return true;
                }
                return false;
            }
            case RPG_DO_QUEST:
            {
                std::vector<uint32> availableQuests;
                std::vector<uint32> const givenUp = NewRpgInternal::GivenUpQuests(bot);
                for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                {
                    uint32 questId = bot->GetQuestSlotQuestId(slot);
                    if (QuestFail::IsGivenUp(givenUp, questId))
                        continue;

                    if (HasQuestPlace(questId))
                    {
                        availableQuests.push_back(questId);
                    }
                }
                if (availableQuests.size())
                {
                    uint32 questId = availableQuests[urand(0, availableQuests.size() - 1)];
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (quest)
                    {
                        RpgStatus::Start(botAI, NewRpgInfo::DoQuest{quest, questId}, "pick");
                        NewRpgInternal::EndQuestAtStart(botAI, bot);
                        return true;
                    }
                }
                return false;
            }
            case RPG_TRAVEL_FLIGHT:
            {
                uint32 flightMasterEntry = 0;
                WorldPosition flightMasterPos;
                std::vector<uint32> path;
                if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
                {
                    RpgStatus::Start(
                        botAI, NewRpgInfo::TravelFlight{flightMasterEntry, flightMasterPos, std::move(path)}, "pick");
                    return true;
                }
                return false;
            }
            case RPG_DO_GATHER:
            {
                RpgStatus::Start(botAI, NewRpgInfo::DoGather{}, "pick");
                return true;
            }
            case RPG_REST:
            {
                RpgStatus::Start(botAI, NewRpgInfo::Rest{}, "pick");
                bot->SetStandState(UNIT_STAND_STATE_SIT);
                return true;
            }
            case RPG_OUTDOOR_PVP:
            {
                RpgStatus::Start(botAI, NewRpgInfo::OutdoorPvP{}, "pick");
                return true;
            }
            default:
            {
                RpgStatus::Start(botAI, NewRpgInfo::Rest{}, "pick");
                bot->SetStandState(UNIT_STAND_STATE_SIT);
                return true;
            }
        }
        return false;
    };
    bool const started = startStatus(chosenStatus);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::RpgStatus, WorldLog::NewRpgStatusWord(chosenStatus),
                       started ? (reason ? reason : "weights") : "no_target", static_cast<float>(chosenWeight),
                       alternatives, ref);
    return started;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> const givenUp = NewRpgInternal::GivenUpQuests(bot);
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (QuestFail::IsGivenUp(givenUp, questId))
                    continue;

                if (HasQuestPlace(questId))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
                return false;

            // No point farming with (nearly) full bags - the loot pipeline
            // can't store the harvest (see StoreLootAction).
            if (AI_VALUE(uint8, "bag space") > 80)
                return false;

            return sGatherNodeMgr.HasUsableNodes(bot);
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}
