/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGACTION_H
#define PLAYERBOTS_NEWRPGACTION_H

#include "Duration.h"
#include "MovementActions.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"
#include <string>

class Player;

class TellRpgStatusAction : public NewRpgBaseAction
{
public:
    TellRpgStatusAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "rpg status") {}

    bool Execute(Event event) override;

private:
    static constexpr char const* RPG_STATUS_CHANGED_KEY = "rpg_status_changed";
    static constexpr char const* RPG_STATUS_CHANGED_DEFAULT = "rpg status -> %status";

    void WhisperStatusChange(Player* owner, std::string const& statusName);
};

class StartRpgDoQuestAction : public Action
{
public:
    StartRpgDoQuestAction(PlayerbotAI* botAI) : Action(botAI, "start rpg do quest") {}

    bool Execute(Event event) override;
};

class NewRpgStatusUpdateAction : public NewRpgBaseAction
{
public:
    NewRpgStatusUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg status update")
    {
        // int statusCount = RPG_STATUS_END - 1;

        // transitionMat.resize(statusCount, std::vector<int>(statusCount, 0));

        // transitionMat[RPG_IDLE][RPG_GO_GRIND] = 20;
        // transitionMat[RPG_IDLE][RPG_GO_CAMP] = 15;
        // transitionMat[RPG_IDLE][RPG_WANDER_NPC] = 30;
        // transitionMat[RPG_IDLE][RPG_DO_QUEST] = 35;
    }
    bool Execute(Event event) override;

protected:
    // static NewRpgStatusTransitionProb transitionMat;
    const int32 statusWanderNpcDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusWanderRandomDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusRestDuration = 30 * IN_MILLISECONDS ;
    const int32 statusDoQuestDuration = 30 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusOutDoorPvPDuration = HOUR * IN_MILLISECONDS ;
    const int32 statusDoGatherDuration = 15 * MINUTE * IN_MILLISECONDS;
};

class NewRpgGoGrindAction : public NewRpgBaseAction
{
public:
    NewRpgGoGrindAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go grind") {}
    bool Execute(Event event) override;
};

class NewRpgGoCampAction : public NewRpgBaseAction
{
public:
    NewRpgGoCampAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go camp") {}
    bool Execute(Event event) override;
};

class NewRpgWanderRandomAction : public NewRpgBaseAction
{
public:
    NewRpgWanderRandomAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg wander random") {}
    bool Execute(Event event) override;
};

class NewRpgWanderNpcAction : public NewRpgBaseAction
{
public:
    NewRpgWanderNpcAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg move npcs") {}
    bool Execute(Event event) override;

    const uint32 npcStayTime = 8 * IN_MILLISECONDS;
};

class NewRpgDoQuestAction : public NewRpgBaseAction
{
public:
    NewRpgDoQuestAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do quest") {}
    bool Execute(Event event) override;

protected:
    bool DoIncompleteQuest(NewRpgInfo::DoQuest& data);
    bool DoCompletedQuest(NewRpgInfo::DoQuest& data);

    const uint32 poiStayTime = 5 * MINUTE * IN_MILLISECONDS;
};

// Chooses the nearest friendly vendor on the bot's map and starts an errand there when the trip is worth it
class NewRpgStartErrandsAction : public NewRpgBaseAction
{
public:
    NewRpgStartErrandsAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg start errands") {}
    bool Execute(Event event) override;
};

// Walks to the errand's vendor, sells, buys and repairs there, then goes IDLE
class NewRpgErrandsAction : public NewRpgBaseAction
{
public:
    NewRpgErrandsAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg errands") {}
    bool Execute(Event event) override;

private:
    // A vendor that walks along its route stays within this distance of its spawn point
    static constexpr float VENDOR_SEARCH_YARDS = 30.0f;
};

// Follows the trip's route leg by leg (RPG_TRAVEL): asks the route service, walks the waypoints, takes portals; a
// failed leg asks for a new route (at most TravelLegs::MAX_REPLANS), never a teleport. Flights come with WT8b
class NewRpgTravelAction : public NewRpgBaseAction
{
public:
    NewRpgTravelAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel") {}
    bool Execute(Event event) override;

private:
    bool RequestRoute(NewRpgInfo::Travel& data);
    bool TakeRoute(NewRpgInfo::Travel& data);
    bool UsePortal(NewRpgInfo::Travel& data);
    bool TalkFlightMaster(NewRpgInfo::Travel& data);
    bool LegFailed(NewRpgInfo::Travel& data, TravelLegs::FailReason reason);
    bool Arrive(NewRpgInfo::Travel const& data);
    // why: the log's word; source: the Travel event's ("abort:<reason>", TravelLegs::AbortSource)
    void EndTrip(NewRpgInfo::Travel const& data, char const* why, char const* source);
};

// GM orders for RPG_TRAVEL (`playerbots route go|stop`, tests): the command posts on the world thread, the bot's map
// thread takes its order in NewRpgStatusUpdateAction (one atomic load per tick while no order waits). An order not
// taken within a minute (the bot is not in the rpg strategy, or logged out) is dropped
struct TravelOrder
{
    WorldPosition dest{};
    std::string place;  // the name in the log lines
    bool stop{false};   // end the bot's trip instead
    uint32 postedMs{0};
};

namespace TravelOrders
{
void Post(ObjectGuid bot, TravelOrder order);
bool Take(ObjectGuid bot, TravelOrder& order);
}  // namespace TravelOrders

class NewRpgTravelFlightAction : public NewRpgBaseAction
{
public:
    NewRpgTravelFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel flight") {}
    bool Execute(Event event) override;
};

#endif
