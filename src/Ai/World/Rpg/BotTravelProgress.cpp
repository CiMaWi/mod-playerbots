/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTravelInternal.h"
#include "NewRpgInternal.h"
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
    Owner owner = Owner::Exempt;
    if (combat)
        owner = Owner::Combat;
    else if (bot.IsAlive() && !external)
    {
        if (!canMove)
            owner = Owner::Control;
        else if (block.HasLootInteraction())
            owner = Owner::Loot;
        else
        {
            // A transport leg's approach is a walk on the world mesh to a boarding row's dock point (WT70c): the
            // walk budget judges it, and the mover giving the point up spends the row (DockPointGivenUp)
            bool const approach = leg && leg->type == TravelRoute::LegType::Transport &&
                                  travel.trip.boarding == TravelLegs::Boarding::Approach;
            owner = !leg || leg->type == TravelRoute::LegType::Walk || approach ? Owner::Walk : Owner::Leg;
        }
    }
    auto const sample =
        TravelProgress::Maintain(progress, {nowMs, owner, combat, block.CompletedLootInteractions()}, travel.trip,
                                 travel.route, travel.routeSeconds, [&] { return block.ReadTravelWalk(true); });
    if (sample.Selected)
        block.TraceTravelInteraction("route_selected", travel.bindInn ? "inn" : "order");
    if (sample.CombatChanged)
        block.TraceTravelInteraction(combat ? "combat_begin" : "combat_end", "combat");
    if (sample.LootCompleted)
        block.TraceTravelInteraction("route_progress", "loot");
    if (sample.Yards > 0.0f)
        block.TraceTravelInteraction("route_progress", "distance", ObjectGuid::Empty, 0, "none", sample.Yards);
}
