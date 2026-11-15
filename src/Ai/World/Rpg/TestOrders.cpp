/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestOrders.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#include "CensusMgr.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "UpkeepRules.h"
#include "WorldLog.h"

using namespace NewRpgInternal;

namespace
{
constexpr uint32 TRAVEL_ORDER_TTL_MS = 60 * IN_MILLISECONDS;
constexpr char const* CONSTRAINT_EVENT_TYPE = "Constraint";

std::mutex travelOrdersLock;
std::unordered_map<ObjectGuid, TravelOrder> travelOrders;  // guarded by travelOrdersLock

// nofight: creatures neither attack an immune-to-NPC unit nor are attacked by it (Unit::_IsValidAttackTarget); setting
// the flag ends the unit's PvE combat
void SetNoFight(Unit* unit, bool on)
{
    if (unit && unit->IsImmuneToNPC() != on)
        unit->SetImmuneToNPC(on);
}

void NoFight(Player& bot, bool on)
{
    SetNoFight(&bot, on);
    for (Unit* controlled : bot.m_Controlled)
        SetNoFight(controlled, on);
}

void ApplyNoFight(Player& bot) { NoFight(bot, true); }
void UndoNoFight(Player& bot) { NoFight(bot, false); }
void ApplyNoDamage(Player& bot) { bot.SetCommandStatusOn(CHEAT_GOD); }
void UndoNoDamage(Player& bot) { bot.SetCommandStatusOff(CHEAT_GOD); }
// flyfail and flyabort change nothing on the bot: the fly leg's follower reads the bit (TestConstraints::FlyLegFails)
void ReadByFollower(Player&) {}
// nodestroy changes nothing on the bot: the smart destroy action reads the bit (TestConstraints::DestroysForSpace)
void ReadByDestroy(Player&) {}
// notaxi changes nothing on the bot: the route request reads the bit (TestConstraints::TaxiAllowed)
void ReadByRoute(Player&) {}
// nomount changes nothing on the bot: the check mount state action reads the bit (TestConstraints::MountsOnItsOwn)
void ReadByMount(Player&) {}
// flee changes nothing on the bot: the world PvP retreat trigger reads the bit (TestConstraints::Flees)
void ReadByFlee(Player&) {}

// The state rows' apply and undo (TestConstraints::ROWS)
constexpr TestConstraints::StateOps<Player> STATE_OPS[] = {
    {TestConstraints::NOFIGHT, ApplyNoFight, UndoNoFight},
    {TestConstraints::NODAMAGE, ApplyNoDamage, UndoNoDamage},
    {TestConstraints::FLYFAIL, ReadByFollower, ReadByFollower},
    {TestConstraints::FLYABORT, ReadByFollower, ReadByFollower},
    {TestConstraints::NODESTROY, ReadByDestroy, ReadByDestroy},
    {TestConstraints::NOTAXI, ReadByRoute, ReadByRoute},
    {TestConstraints::NOMOUNT, ReadByMount, ReadByMount},
    {TestConstraints::FLEE, ReadByFlee, ReadByFlee},
};
static_assert(TestConstraints::Covered(STATE_OPS) == TestConstraints::STATES, "every state row has apply and undo");

// A test's constraint change: the state rows applied or undone, one Constraint row whose amount is the ms from the
// command to the take (also on an idle bot). A choice row turning on ends the current status unless a trip comes with
// it, so the bot stands idle; the idle gate then keeps it there. The planner's next snapshot sees the hearth bit; the
// errand row turning off forgets the last errand check (TestConstraints::FreesErrands)
void ApplyConstraints(PlayerbotAI* botAI, Player* bot, NewRpgInfo& info, TravelOrder const& order, bool tripFollows)
{
    uint16 const before = info.constraints;
    uint16 const after = TestConstraints::Next(before, order.on, order.off);
    info.constraints = after;
    info.planner.snapshotDue = true;
    if (TestConstraints::FreesErrands(before, after))
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet().ErrandChecked = false;
    TestConstraints::ChangeStates(STATE_OPS, *bot, before, after);
    std::string const set = TestConstraints::Format(after);
    uint32 const latencyMs = GetMSTimeDiffToNow(order.postedMs);
    LOG_INFO("playerbots", "[Route] {} constraints: {} (taken after {} ms)", bot->GetName(), set, latencyMs);
    EventLog::Emit(bot, CONSTRAINT_EVENT_TYPE, set.c_str(), 0, latencyMs);
    if (!TestConstraints::EndsStatus(before, after) || tripFollows)
        return;
    // An ordered end: a trip writes its abort rows with the source abort:held (the status core's trip arm)
    RpgStatus::End(botAI, StatusEnd::Outcome::Ordered, {.word = "hold", .source = "abort:held"});
}

// A GM order taken on the bot's map thread: the state is checked again (the command saw it on the world thread)
bool ApplyTravelOrder(PlayerbotAI* botAI, Player* bot, TravelOrder const& order)
{
    NewRpgInfo& info = botAI->rpgInfo;
    bool const trip = TestConstraints::HasTrip(order);
    bool const constrained = order.on || order.off;
    uint16 const before = info.constraints;
    if (constrained)
        ApplyConstraints(botAI, bot, info, order, trip);
    // A released errand choice is made at once: an errand check stamped before the hold (an inn trip, the login)
    // would otherwise keep the bot from its errands for the retry time (WB8b run 1: up to 5 minutes)
    if (!TestConstraints::ErrandAllowed(before) && TestConstraints::ErrandAllowed(info.constraints))
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet().ErrandChecked = false;
    if (order.aboard)
        PlaceAboard(botAI, bot, order.aboard);
    bool const applied = constrained || order.aboard;
    if (!trip)
        return applied;
    if (order.stop)
    {
        auto const* data = std::get_if<NewRpgInfo::Travel>(&info.data);
        if (!data)
        {
            LOG_INFO("playerbots", "[Route] {} stop: not travelling", bot->GetName());
            return applied;
        }
        botAI->CancelLootInteraction("order");
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
    LOG_INFO("playerbots", "[Route] {} travel to {} ({}, {:.0f}, {:.0f}, {:.0f}) ordered", bot->GetName(), order.place,
             order.dest.GetMapId(), order.dest.GetPositionX(), order.dest.GetPositionY(), order.dest.GetPositionZ());
    botAI->CancelLootInteraction("order");
    RpgStatus::Start(botAI, RpgStatus::TripTo(order.dest, order.place), "order", StatusEnd::Outcome::Interrupted);
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
    botAI->orderPosted.store(waiting, std::memory_order_release);
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
    botAI->orderPosted.store(true, std::memory_order_release);
}

bool TravelOrders::TakeAndApply(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI->orderPosted.load(std::memory_order_acquire))
        return false;
    TravelOrder order;
    if (!TakeOrder(botAI, bot->GetGUID(), bot->IsInCombat(), order))
        return false;
    if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
    {
        LOG_INFO("playerbots", "[Route] {} order '{}' refused: the bot does not run the new rpg strategy",
                 bot->GetName(), order.place);
        return false;
    }
    return ApplyTravelOrder(botAI, bot, order);
}

void TravelOrders::KeepConstraints(Player* bot, uint16 constraints)
{
    TestConstraints::KeepStates(STATE_OPS, *bot, constraints);
}
