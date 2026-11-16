/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTTRAVEL_H
#define _PLAYERBOT_BOTTRAVEL_H

#include <optional>
#include <string_view>
#include <utility>

#include "Define.h"
#include "NewRpgInfo.h"
#include "ObjectGuid.h"
#include "TravelLifecycleTypes.h"
#include "TravelProgressState.h"

class LootObjectStack;
class Player;
class PlayerbotAI;
class Transport;
struct TravelTraceIdentity;  // BotTravelInternal.h

// The bot's travel block (CS5 design 4.1): the passenger check's cadence, a test's aboard placement, the loot
// interaction travel waits on, the route request ids and the travel trace. Made by PlayerbotAI's constructors, never
// null (PlayerbotAI::GetTravel); bodies in BotTravel.cpp
class BotTravel
{
public:
    enum class MountPath : uint8
    {
        None,
        Timer,
        Packet
    };
    static constexpr uint32 ABOARD_DEADLINE_MS = 5000;
    static constexpr char const* ABOARD_EVENT_TYPE = "Aboard";

    BotTravel(PlayerbotAI* botAI, Player* bot);
    BotTravel(BotTravel const&) = delete;
    BotTravel& operator=(BotTravel const&) = delete;

    bool ReconcileFollowPassenger(Transport const& expected);
    bool HasLootInteraction() const { return activeLootInteraction != nullptr; }
    uint32 CompletedLootInteractions() const { return completedLootInteractions; }
    bool LootInteractionExpired() const;
    bool ConsumeLootExpiry();
    void CancelLootInteraction(std::string_view reason);
    void TraceTravelInteraction(std::string_view event, std::string_view detail, ObjectGuid loot = ObjectGuid::Empty,
                                uint32 interaction = 0, std::string_view path = "none", float yards = 0.0f,
                                std::optional<std::pair<uint32, uint32>> leg = std::nullopt);
    TravelProgress::Motion ReadTravelWalk(bool checkPosition = false) const;
    void TraceTravelMount(MountPath path, bool accepted);

    // UpdateAI's travel hook ahead of the AI's delays: the passenger check, a test's aboard placement, a
    // pending dismount, the loot interaction's and the trip's progress, the mount trace
    void PreGate(uint32 maintenanceMs, NewRpgInfo::Travel* travel,
                 std::optional<TravelTraceIdentity> const& traceTravel);

    // A test placed the bot on a transport's deck (`route aboard`): the passenger check runs every pass until it rides
    // that transport (`Aboard placed`) or the deadline passes (`Aboard failed`)
    void BeginAboard(uint32 entry);

    // UpdateAI's asks: a passenger recheck at the next pass (Revalidate), the mount trace's end (a reset, an invalid
    // state) and the loot cast check's interaction
    void RequestRecheck() { passengerCadence.Revalidate = true; }
    void ForgetMountTrace();
    LootObjectStack* ActiveLootInteraction() const { return activeLootInteraction; }
    // The miss switch's travel bits (Aboard, Loot, MountTrace; PlayerbotAI::LogPreGateMisses)
    void LogPreGateMisses(uint32 work) const;

    // The next route request id (TravelRoute::Snapshot::requestId). The ids run on across trips and resets (a reset
    // renews NewRpgInfo, not this block): ids restarting at 0 would let an older answer pass Judge as current
    uint32 NextRouteRequest() { return ++routeRequests; }

private:
    // LootObjectStack is the one writer of activeLootInteraction and completedLootInteractions
    friend class LootObjectStack;

    void ObserveTravelProgress(NewRpgInfo::Travel& travel, uint32 nowMs, bool revalidate, bool canMove);

    PlayerbotAI* const botAI;
    Player* const bot;
    LootObjectStack* activeLootInteraction = nullptr;
    uint32 completedLootInteractions = 0;
    MountPath tracedMount = MountPath::None;
    uint32 tracedMountAt = 0;
    TravelLifecycle::Cadence passengerCadence;
    uint32 aboardEntry = 0;    // BeginAboard's transport; 0 = none pending
    uint32 aboardSinceMs = 0;  // getMSTime() of the placement
    uint32 routeRequests = 0;  // the bot's route request ids, across trips
};

#endif
