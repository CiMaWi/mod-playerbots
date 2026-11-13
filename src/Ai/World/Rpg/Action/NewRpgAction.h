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

// Plans a trip (WorldTravel::PlanTrip): the errands the providers offer go to friendly vendors and quest enders near
// the bot, each stop only when its errands are worth its detour; starts the trip when it has a stop
class NewRpgStartErrandsAction : public NewRpgBaseAction
{
public:
    NewRpgStartErrandsAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg start errands") {}
    bool Execute(Event event) override;
};

// Walks the trip's stops; at each it sells, buys and repairs at the vendor and turns in the completed quests whose
// ender stands there, then goes IDLE
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
    static bool ConsumeScheduledWork(PlayerbotAI* botAI);

private:
    bool StepTravel(NewRpgInfo::Travel* data, TravelProgress::Overdue overdue);
    bool RequestRoute(NewRpgInfo::Travel& data);
    bool TakeRoute(NewRpgInfo::Travel& data);
    bool UsePortal(NewRpgInfo::Travel& data);
    bool TalkFlightMaster(NewRpgInfo::Travel& data);
    bool UseHearth(NewRpgInfo::Travel& data);
    bool LegFailed(NewRpgInfo::Travel& data, TravelLegs::FailReason reason);
    bool Arrive(NewRpgInfo::Travel& data, bool stuck = false);
    // why: the log's word; source: the Travel event's ("abort:<reason>", TravelLegs::AbortSource)
    // A ship's rider gets off first (TravelLegs::BeginOffFirst): the end waits
    void EndTrip(NewRpgInfo::Travel& data, char const* why, char const* source);
};

// Starts an inn errand: a trip (RPG_TRAVEL) to the nearest level-fitting inn on the bot's map, where the bot binds its
// hearthstone through the "home" action. Started by the "inn bind" trigger (the bind is far)
class NewRpgStartInnAction : public NewRpgBaseAction
{
public:
    NewRpgStartInnAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg start inn") {}
    bool Execute(Event event) override;
};

// GM orders for tests (`playerbots route go|stop|transport|constraint|hold|release|aboard`): the command posts on the
// world thread, the bot's map thread takes its order at its next AI update, ahead of the AI's delays (TakeAndApply
// from PlayerbotAI::UpdateAI: one atomic load per update while no order waits for the bot; an order that moves the
// bot waits until it is out of combat, a constraint change never). A bot outside the new rpg strategy refuses its
// order; an order not taken within a minute (the bot logged out or kept fighting) is dropped
struct TravelOrder
{
    WorldPosition dest{};
    std::string place;  // the name in the log lines
    bool stop{false};   // end the bot's trip instead
    uint32 postedMs{0};
    uint16 on{0};  // TestConstraints bits set, then those cleared (`off`), before anything else of the order
    uint16 off{0};
    uint32 aboard{0};  // `route aboard`: the transport entry to stand the bot on; 0 = none
};

namespace TravelOrders
{
void Post(PlayerbotAI* botAI, TravelOrder order);
bool TakeAndApply(PlayerbotAI* botAI, Player* bot);  // on the bot's map thread: the order taken and applied
// The bot's state constraints applied again (a pet summoned since gets nofight); map thread, at every AI update of a
// bot with a state row set
void KeepConstraints(Player* bot, uint16 constraints);
}  // namespace TravelOrders

class NewRpgTravelFlightAction : public NewRpgBaseAction
{
public:
    NewRpgTravelFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel flight") {}
    bool Execute(Event event) override;
};

#endif
