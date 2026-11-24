/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTravelInternal.h"
#include "Log.h"
#include "MoveSpline.h"
#include "NewRpgInternal.h"
#include "Player.h"
#include "TravelGraph.h"
#include "TravelProgress.h"

// The held trip's progress (PreGate, BotTravel.cpp): who owns the bot's time now (combat, control, the loot, the walk
// or the leg, none while it rides or waits for its route), TravelProgress::Maintain's sample and its trace lines
void ObserveTravelProgress(BotTravel& block, Player& bot, HeldTrip& travel, uint32 nowMs, bool revalidate, bool canMove)
{
    using TravelProgress::Owner;
    auto& progress = travel.progress;
    if (revalidate)
        progress.Suspend();

    auto const* leg = travel.CurrentLeg();
    bool const combat = bot.IsInCombat();
    // The flight down after a failed fly leg (TravelLegs::StartDescent) counts as riding: a trip never ends in the air
    bool const external =
        NewRpgInternal::PassengerOwnsTravel(bot, travel) || travel.route.waiting || travel.trip.descending;
    // A transport leg's approach is a walk on the world mesh to a boarding row's dock point (WT70c): the walk budget
    // judges it, and the mover giving the point up spends the row (DockPointGivenUp)
    bool const approach =
        leg && leg->type == TravelRoute::LegType::Transport && travel.trip.boarding == TravelLegs::Boarding::Approach;
    TravelProgress::OwnerFacts facts;
    facts.combat = combat;
    facts.alive = bot.IsAlive();
    facts.external = external;
    facts.canMove = canMove;
    facts.loot = block.HasLootInteraction();
    facts.stepped = travel.stepped;
    facts.carried = bot.IsInFlight();
    facts.hasLeg = leg != nullptr;
    facts.walks = leg && (leg->type == TravelRoute::LegType::Walk || approach);
    Owner const owner = TravelProgress::OwnerOf(facts);
    bool const walkDue = progress.Reason() == TravelProgress::Overdue::Walk;
    auto const sample =
        TravelProgress::Maintain(progress, {nowMs, owner, combat, block.CompletedLootInteractions()}, travel.trip,
                                 travel.route, travel.routeSeconds, [&] { return block.ReadTravelWalk(true); });
    // PA54 trace (debug log, once per latch; G183): the walk budget spent, with the owned move's last reading, so the
    // log separates a move never credited from a bot that really stands
    if (!walkDue && progress.Reason() == TravelProgress::Overdue::Walk)
    {
        TravelProgress::Motion const motion = block.ReadTravelWalk(true);
        TravelWalk::LinkWalk const& walk = travel.trip.walk;
        TravelGraph const& graph = TravelGraph::Instance();
        WorldPosition const& order = travel.walkTo;
        LOG_DEBUG(
            "playerbots",
            "[Route] {} walk budget spent: owner {}, motion owned {} id {} generator {} (now id {} generator {}) "
            "dest ({:.1f}, {:.1f}, {:.1f}), spline yards {:.1f} credited {:.1f} last {:.1f}, position matches {}, "
            "moving {} finalized {}, mounted {}, order ({:.1f}, {:.1f}, {:.1f}) {:.0f} yd, link {}>{} {} point {}",
            bot.GetName(), static_cast<int>(progress.CurrentOwner()), progress.HasMotion() ? 1 : 0, progress.MotionId(),
            progress.GeneratorId() ? 1 : 0, motion.Id, motion.Generator ? 1 : 0, motion.Destination.x,
            motion.Destination.y, motion.Destination.z, motion.Yards, progress.CreditedYards(), progress.LastYards(),
            motion.PositionMatches ? 1 : 0, bot.isMoving() ? 1 : 0, bot.movespline->Finalized() ? 1 : 0,
            bot.IsMounted() ? 1 : 0, order.GetPositionX(), order.GetPositionY(), order.GetPositionZ(),
            order == WorldPosition() ? -1.0f : bot.GetExactDist(order),
            walk.Active() ? static_cast<int64>(graph.GetNode(walk.from).id) : -1,
            walk.Active() ? static_cast<int64>(graph.GetNode(walk.to).id) : -1, TravelWalk::RowWord(walk),
            walk.point == TravelWalk::NO_POINT ? -1 : static_cast<int64>(walk.point) + 1);
    }
    if (sample.Selected)
        block.TraceTravelInteraction("route_selected", travel.bindInn ? "inn" : "order");
    if (sample.CombatChanged)
        block.TraceTravelInteraction(combat ? "combat_begin" : "combat_end", "combat");
    if (sample.LootCompleted)
        block.TraceTravelInteraction("route_progress", "loot");
    if (sample.Yards > 0.0f)
        block.TraceTravelInteraction("route_progress", "distance", ObjectGuid::Empty, 0, "none", sample.Yards);
}
