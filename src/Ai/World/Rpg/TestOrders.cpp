/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestOrders.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#include "BotTravel.h"
#include "EventLog.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TestConstraintStates.h"
#include "TestConstraints.h"
#include "TestOrderStatus.h"
#include "Timer.h"
#include "UpkeepRules.h"
#include "WorldLog.h"

using namespace NewRpgInternal;

namespace
{
constexpr uint32 TRAVEL_ORDER_TTL_MS = 60 * IN_MILLISECONDS;
std::mutex travelOrdersLock;
std::unordered_map<ObjectGuid, TravelOrder> travelOrders;  // guarded by travelOrdersLock

// A GM order taken on the bot's map thread: the state is checked again (the command saw it on the world thread)
bool ApplyTravelOrder(PlayerbotAI* botAI, Player* bot, TravelOrder const& order)
{
    bool const trip = TestConstraints::HasTrip(order);
    bool const constrained = order.on || order.off;
    if (constrained)
        TravelOrders::ApplyConstraints(botAI, bot, order, trip);
    // A constraint change is applied whatever the strategy; the rest of the order needs the new rpg strategy
    if (TestConstraints::NeedsNewRpg(order) && !botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
    {
        LOG_INFO("playerbots", "[Route] {} order '{}' refused: the bot does not run the new rpg strategy",
                 bot->GetName(), order.place);
        return constrained;
    }
    if (order.aboard)
        PlaceAboard(botAI, bot, order.aboard);
    if (order.flush)
        botAI->GetTravel().FlushMeshMoves(getMSTime());
    bool const applied = constrained || order.aboard || order.flush;
    if (!trip)
        return applied;
    if (order.stop)
    {
        if (!botAI->GetTravel().Trip())
        {
            LOG_INFO("playerbots", "[Route] {} stop: not travelling", bot->GetName());
            return applied;
        }
        botAI->GetTravel().CancelLootInteraction("order");
        RpgStatus::End(botAI, StatusEnd::Outcome::Ordered, {.source = "abort:stopped"});
        return true;
    }

    char const* refusal = nullptr;
    if (!bot->IsAlive())
        refusal = "dead";
    else if (bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
        refusal = "sick";  // a sick bot rests (upkeep §3.5)
    else if (bot->IsInFlight())
        refusal = "in flight";
    else if (bot->GetMap()->Instanceable())
        refusal = "in an instance";
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} travel to {} refused: {}", bot->GetName(), order.place, refusal);
        return applied;
    }
    if (order.quest)
        return StartOrderedQuest(botAI, bot, order.quest) || applied;
    if (!order.status.empty())
        return TravelOrders::StartStatus(botAI, bot, order) || applied;
    LOG_INFO("playerbots", "[Route] {} travel to {} ({}, {:.0f}, {:.0f}, {:.0f}) ordered", bot->GetName(), order.place,
             order.dest.GetMapId(), order.dest.GetPositionX(), order.dest.GetPositionY(), order.dest.GetPositionZ());
    botAI->GetTravel().CancelLootInteraction("order");
    RpgStatus::Start(botAI, RpgStatus::TripTo(order.dest, order.place), "order", StatusEnd::Outcome::Ordered);
    return true;
}

// The bot's order when it may take it now (TestConstraints::TakeNow); the bot's hint stays while its order waits.
// Orders nobody took in time are dropped on any take, and logged: the bot logged out or fought for a minute
bool TakeOrder(PlayerbotAI* botAI, ObjectGuid bot, bool inCombat, TravelOrder& order)
{
    std::lock_guard<std::mutex> guard(travelOrdersLock);
    for (auto it = travelOrders.begin(); it != travelOrders.end();)
    {
        if (GetMSTimeDiffToNow(it->second.postedMs) <= TRAVEL_ORDER_TTL_MS)
        {
            ++it;
            continue;
        }
        LOG_INFO("playerbots", "[Route] order '{}' for bot {} dropped: not taken within {} s", it->second.place,
                 it->first.GetCounter(), TRAVEL_ORDER_TTL_MS / IN_MILLISECONDS);
        it = travelOrders.erase(it);
    }
    auto const found = travelOrders.find(bot);
    bool const taken = found != travelOrders.end() && TestConstraints::TakeNow(found->second, inCombat);
    bool const waiting = found != travelOrders.end() && !taken;
    if (taken)
    {
        order = std::move(found->second);
        travelOrders.erase(found);
    }
    botAI->SetWork(BotWork::Order, waiting);
    return taken;
}
}  // namespace

void TravelOrders::Post(PlayerbotAI* botAI, TravelOrder order)
{
    std::lock_guard<std::mutex> guard(travelOrdersLock);
    // A hold and a trip posted before the bot's next update both stay (TestConstraints::Merge)
    auto const [it, added] = travelOrders.try_emplace(botAI->GetBot()->GetGUID(), order);
    if (!added)
        TestConstraints::Merge(it->second, std::move(order));
    botAI->AddWork(BotWork::Order);
}

bool TravelOrders::TakeAndApply(PlayerbotAI* botAI, Player* bot)
{
    if (!(botAI->GetWork() & BotWork::Order))
        return false;
    TravelOrder order;
    if (!TakeOrder(botAI, bot->GetGUID(), bot->IsInCombat(), order))
        return false;
    return ApplyTravelOrder(botAI, bot, order);
}
