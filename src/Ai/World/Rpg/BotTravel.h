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
#include "FailedAttemptRules.h"
#include "HeldTrip.h"
#include "MeshMoveRules.h"
#include "NewRpgInfo.h"
#include "ObjectGuid.h"
#include "StatusEndRules.h"
#include "TravelLifecycleTypes.h"
#include "TravelProgressState.h"

class LootObjectStack;
class Player;
class PlayerbotAI;
class Transport;
struct Position;
struct TravelTraceIdentity;  // BotTravelInternal.h

// The bot's travel block (CS5 design 4.1): the passenger check's cadence, a test's aboard placement, the loot
// interaction travel waits on, the route request ids, the held trip (WT64f) and the travel trace. Made by
// PlayerbotAI's constructors, never null (PlayerbotAI::GetTravel); bodies in BotTravel.cpp
class BotTravel
{
public:
    enum class MountPath : uint8
    {
        None,
        Timer,
        Packet
    };

    BotTravel(PlayerbotAI* botAI, Player* bot);
    BotTravel(BotTravel const&) = delete;
    BotTravel& operator=(BotTravel const&) = delete;

    // A follower at its master's vessel (WT70d): boards or leaves by the stored rows of the stop the vessel stands at.
    // None: nothing to do; Walk: the follow action walks to walkTo (a boarding row's dock point)
    TravelLegs::FollowAct FollowAboard(Position& walkTo);
    TravelLegs::FollowCrossing& FollowState() { return followCrossing; }
    bool HasLootInteraction() const { return activeLootInteraction != nullptr; }
    uint32 CompletedLootInteractions() const { return completedLootInteractions; }
    bool LootInteractionExpired() const;
    bool ConsumeLootExpiry();
    void CancelLootInteraction(std::string_view reason);
    void TraceTravelInteraction(std::string_view event, std::string_view detail, ObjectGuid loot = ObjectGuid::Empty,
                                uint32 interaction = 0, std::string_view path = "none", float yards = 0.0f,
                                std::optional<std::pair<uint32, uint32>> leg = std::nullopt);
    TravelProgress::Motion ReadTravelWalk(bool checkPosition = false) const;
    void TraceTravelMount(MountPath path, char const* veto);

    // The held trip (WT64f): begun with its holder's status, ended before it (RpgStatusCore); null: none. O(1)
    HeldTrip* Trip() { return hold.Held(); }
    HeldTrip const* Trip() const { return hold.Held(); }
    uint32 TripHolder() const { return hold.Holder(); }
    // UpdateAI's travel hook ahead of the AI's delays: passenger check, aboard placement, pending dismount, loot
    // interaction and trip progress, mount trace
    void PreGate(uint32 maintenanceMs, HeldTrip* travel, std::optional<TravelTraceIdentity> const& traceTravel);

    // A test placed the bot on a transport's deck (`route aboard`): the passenger check probes that vessel (guid) every
    // pass until it rides it (`Aboard placed`) or the deadline passes (`Aboard failed`)
    void BeginAboard(uint32 entry, uint64 guid);
    // UpdateAI's asks: a passenger recheck at the next pass, the mount trace's end, the loot cast check's interaction
    void RequestRecheck() { passengerCadence.Revalidate = true; }
    void ForgetMountTrace();
    LootObjectStack* ActiveLootInteraction() const { return activeLootInteraction; }
    void LogPreGateMisses(uint32 work) const;

    // One move the bot ordered (WT71; MeshMoveRules.h): kind, PathType bits, last step. counted: already counted
    // elsewhere (a walk link's hop). PreGate flushes once per MeshMove::MISS_INTERVAL_MS
    void NoteMeshMove(MeshMove::Kind kind, uint32 pathType, float lastStep, bool counted = false);
    // The MeshMiss rows of the moves counted so far, written now (PreGate's flush; `route flush`)
    void FlushMeshMoves(uint32 nowMs);
    // The check interval travel sets out of combat (GetReactDelay): a transport leg's scan, the loot's pace
    std::optional<uint32> PaceMs() const;

    // The next route request id (Snapshot::requestId): ids run on across trips and resets (no old answer passes Judge)
    uint32 NextRouteRequest() { return ++routeRequests; }
    // The record of failed attempts (WT64c): memory only, fresh where the RPG memos reset; GoTo reads it, the route
    // request and planner a copy (LiveEntries)
    FailedAttempts::Record const& Failures() const { return failures; }
    void ResetFailures(StatusEnd::ResetEffect effect) { StatusEnd::ApplyReset(effect, failures); }
    // Its one writer, at the typed end of a leg, a trip or GoTo's local piece: the cause word's row picks the entry;
    // a stuck book cause on a graph link of a bot on the mesh also goes to the book
    void NoteFailure(std::string_view word, FailedAttempts::Failure const& failure, uint32 tripId);

private:
    friend class LootObjectStack;  // the one writer of activeLootInteraction and completedLootInteractions
    friend struct RpgStatusCore;   // the one that begins and ends the held trip

    PlayerbotAI* const botAI;
    Player* const bot;
    LootObjectStack* activeLootInteraction = nullptr;
    uint32 completedLootInteractions = 0;
    MountPath tracedMount = MountPath::None;
    uint32 tracedMountAt = 0;
    TravelLifecycle::Cadence passengerCadence;
    uint32 aboardEntry = 0;                     // BeginAboard's transport; 0 = none pending
    uint64 aboardGuid = 0;                      // that transport's guid (the passenger check's boarding intent)
    uint32 aboardSinceMs = 0;                   // getMSTime() of the placement
    uint32 routeRequests = 0;                   // the bot's route request ids, across trips
    MeshMove::Counters meshMoves;               // the moves ordered and not carried by the navmesh since the last flush
    TravelLegs::FollowCrossing followCrossing;  // a follower's crossing at its master's vessel (FollowAboard)
    std::optional<TravelLifecycle::PassengerMotion> followMotion;  // its issued move, judged by the passenger check
    StatusEnd::TripHold<HeldTrip> hold;                            // the held trip, its holder and its last end
    FailedAttempts::Record failures;                               // the record of failed attempts (NoteFailure)
};

#endif
