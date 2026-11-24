/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFO_H
#define PLAYERBOTS_NEWRPGINFO_H

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelLegTypes.h"
#include "TravelLifecycleTypes.h"
#include "TravelMgr.h"
#include "TravelProgressState.h"

using NewRpgStatusTransitionProb = std::vector<std::vector<int>>;

class Player;
struct HeldTrip;  // HeldTrip.h, not included here: the trip lives in BotTravel (WT64f)

namespace UpkeepNeeds
{
struct VisitNeeds;  // UpkeepNeedRules.h, not included here (CS16)
}

struct NewRpgInfo
{
    NewRpgInfo() : data(Idle{}) {}
    ~NewRpgInfo() = default;

    // RPG_GO_GRIND
    struct GoGrind
    {
        WorldPosition pos{};
    };
    // RPG_GO_CAMP
    struct GoCamp
    {
        WorldPosition pos{};
    };
    // RPG_WANDER_NPC
    struct WanderNpc
    {
        ObjectGuid npcOrGo{};
        uint32 lastReach{0};
    };
    // RPG_WANDER_RANDOM
    struct WanderRandom
    {
        WanderRandom() = default;
    };
    // RPG_DO_QUEST
    struct DoQuest
    {
        Quest const* quest{nullptr};
        uint32 questId{0};
        int32 objectiveIdx{0};
        WorldPosition pos{};
        uint32 lastReachPOI{0};
        float roam{0.0f};      // WT33b: how far the bot roams around pos once there (the area's spread)
        WorldPosition left{};  // WT33b: the place a stay with progress left, which the next pick passes over
        // WT35a: the target the executor holds (resolved each tick) and the places it tried in this run without a
        // target; nothing else is kept, the quest log is the state (QuestExecRules.h)
        ObjectGuid targetGuid{};
        WorldPosition targetPos{};
        uint32 lastScan{0};
        uint32 lastEngage{0};      // the last engage tick out of a fight
        uint32 engagedMs{0};       // the held target's pursuit clock (QuestExec::Pursue)
        bool ticking{false};       // the engage tick before counted
        WorldPosition walkDest{};  // the executor's walk and its watch (QuestExec::WalkStalled)
        float walkNearest{-1.0f};
        uint32 walkStalledMs{0};
        uint32 lastWalk{0};
        std::array<ObjectGuid, 4> passed{};  // the last passed over (QuestExec::PASSED_TARGETS): a seek skips them
        uint8 passedNext{0};
        uint8 traced{0};              // the once-per-hold or -walk trace rows written (NewRpgQuest.cpp)
        bool ordered{false};          // started by an order (its first tick is traced)
        std::vector<uint16> tried{};  // QuestExec::PlaceKey of the places tried without a target
        uint16 place{0};              // QuestExec::PlaceKey of the place being worked
        uint8 work{0};                // QuestExec::Work done there
        // WT35c: the order clock's run stamp (PlayerbotAI::pursuitClocks; 0 until the first tick) and last tick
        uint32 run{0};
        uint32 pursuitTick{0};
        // WT35e: when the held object was used or handed to the loot path (0: not yet), and whether this run passed
        // over a container it could not open (its places running out end the order as locked)
        uint32 actedAt{0};
        bool lockedSeen{false};
        bool selected{false};  // WT35f: the held talk creature's option was picked (after its hello, actedAt)
        uint8 useTries{0};     // WT35b: the framework's retriable refusals at the held use target, the last ask
        uint32 useAskedAt{0};  // and whether a timed use at it runs (its row ends it)
        bool usePending{false};
    };
    // RPG_TRAVEL_FLIGHT
    struct TravelFlight
    {
        uint32 flightMasterEntry{0};
        WorldPosition flightMasterPos{};
        std::vector<uint32> path;
        bool inFlight{false};
    };
    // RPG_REST
    struct Rest
    {
        Rest() = default;
    };
    // RPG_OUTDOOR_PVP
    struct OutdoorPvP
    {
        ObjectGuid::LowType capturePointSpawnId{0};
    };
    // RPG_DO_GATHER
    struct DoGather
    {
        ObjectGuid::LowType nodeSpawnId{0};
        WorldPosition nodePos{};
        std::unordered_set<ObjectGuid::LowType> visited;  // written off for this status
        uint32 lastReach{0};
        uint32 lastPassiveCheck{0};
        // The planned circuit (professions WR13b): spawn ids in walk order, walked `laps` times; per stop the time
        // it was last taken or found gone (respawn memory, 0 = never); empty: the one nearest live node
        std::vector<ObjectGuid::LowType> circuit;
        std::vector<uint32> takenMs;
        uint32 laps{1};
        uint32 lap{0};
        uint32 next{0};           // the stop targeted or due
        bool harvestCast{false};  // the bot's own gather cast went at the target (the loot pipeline, LootAction)
    };
    // RPG_DO_FISH: a fishing trip (professions WR16b, NewRpgDoFish.h): the place the order or plan named, the stand
    // probed there and the water it faces, what the pole replaced, the casts so far
    struct DoFish
    {
        WorldPosition shore{};  // the trip's place: a point on a shore or in the water
        WorldPosition stand{};  // where the bot casts from; invalid until probed
        WorldPosition water{};  // the water it faces there
        ObjectGuid pool{};      // the live fishing pool it fishes at; empty at open water
        ObjectGuid weapon{};    // the main-hand item the pole replaced; empty: none
        ObjectGuid offhand{};   // the off-hand item the two-handed pole sent to the bags; empty: none
        uint32 planMs{0};       // the trip's time; 0: Fish::TRIP_MINUTES
        uint32 spell{0};        // the fishing rank it casts; 0 before the first tick
        uint32 castMs{0};       // the cast in flight began; 0: none
        uint32 lootMs{0};       // a catch's loot window was first seen open; 0: none
        uint32 casts{0};
        uint32 catches{0};
        uint32 missesInARow{0};
        uint32 refusedCasts{0};   // casts refused at this stand
        uint32 probes{0};         // stand points tried
        uint32 watchMs{0};        // the last tick that watched the cast (a test bot's trace)
        uint32 watchGapMs{0};     // the cast's longest gap between two watching ticks (a test bot's trace)
        float chance{1.0f};       // the bot's catch chance at this water (FishRules.h)
        bool channelSeen{false};  // the cast in flight started its channel
        bool poleOut{false};      // the trip put the pole in hand
        bool standOnMesh{false};  // the stand is where the mesh's path to it ends: the walk goes all the way
    };
    // RPG_TRAVEL: the holder of a trip to dest (WT64f). The trip itself (legs, route wait, progress) is held in the
    // bot's travel block (BotTravel::Trip, HeldTrip.h), begun with this status by the status core; the route follower
    // (NewRpgTravelAction) steps it
    struct Travel
    {
        WorldPosition dest{};
        std::string place;  // a GM order's place (`playerbots route go`): its first route is logged
    };
    // RPG_ERRANDS: a trip through stops near the bot (WorldTravel::PlanTrip, ErrandRules.h); at each stop the bot does
    // every errand assigned there: sell, buy (a bag by worth, the shopping list) and repair at its vendor, turn in
    // completed quests, visit a trainer, the banker and the mailbox (BankVisit.h, MailVisit.h)
    struct ErrandTurnIn
    {
        uint32 questId{0};
        uint32 ender{0};  // the creature or object entry that takes it
        bool enderIsObject{false};
    };
    struct ErrandStop
    {
        WorldPosition pos{};
        uint32 vendorEntry{0};          // the vendor of this stop; 0 = none
        uint32 trainerEntry{0};         // the trainer of this stop; 0 = none
        bool trainerRiding{false};      // the trainer teaches riding (WU14), else the class trainer (WU13)
        bool trainerWeapon{false};      // a weapon master (WU15a)
        bool trainerProfession{false};  // a profession trainer (WR4b)
        uint32 tameEntry{0};            // the beast to tame at this stop, its spawn the stop's place (WU21b); 0 = none
        // The tame at the stop (WU21b-2): the beast picked, the casts made, a cast whose channel's end is still to be
        // read, when the bot arrived or its last channel ended (the rest before a new try), the last cast and the
        // last AI update that saw its channel run (the channel's length in the channel_broken row)
        ObjectGuid tameBeast;
        uint8 tameTries{0};
        bool tameCast{false};
        uint32 tameSinceMs{0};
        uint32 tameCastMs{0};
        uint32 tameSeenMs{0};
        uint32 tamePickMs{0};  // the reach bound's start past the rest, kept by a new pick (TameReachStartMs)
        // The site and the respawn (WU.23): the threats fought at the cast (TAME_MAX_CLEARS), a fight to clear one
        // whose end starts the rest and reach bounds again; the first wait for a respawn, the wait's end after it
        uint8 tameClears{0};
        bool tameClearing{false};
        uint32 tameWaitFirstMs{0};
        uint32 tameWaitUntilMs{0};
        // The stable at this stop (WU21c): the stable master's entry, the wanted beast and its spawn for the tame stop
        // the visit puts after this one, the wait for the pet to come back after a dismount; tameStabled on that
        // tame stop: the old pet's entry (its PetTamed src stabled; only wanted families are tamed there)
        uint32 stableEntry{0};
        uint32 stableBeast{0};
        WorldPosition stableBeastAt{};
        uint32 stableSinceMs{0};
        uint32 tameStabled{0};
        std::vector<ErrandTurnIn> turnIns;
        // The bank and mail errands of the stop (WB8b), each cleared when its visit starts: a banker visit before
        // the vendor part when it withdraws (it deposits in the same visit), after it when it only deposits; the
        // mailbox last, once the sale has made room
        bool bankBefore{false};
        bool bankAfter{false};
        bool mail{false};
        // The craft errand (professions WR14): the skill path's queued crafts done here, before the deposit; the
        // queue's crafts looked at so far (one craft visit each, a bounded number per stop)
        bool craft{false};
        uint8 craftsTried{0};
    };
    struct Errands
    {
        std::vector<ErrandStop> stops;
        uint8 stop{0};           // the stop the bot walks to or works at
        bool arrived{false};     // the bot reached the current stop
        bool vendorDone{false};  // the current stop's vendor part is done (or its vendor is gone)
        ObjectGuid vendor;       // the current stop's vendor, found near its spawn on arrival
        bool visited{false};     // an errand was done at some stop: the trip's minutes are measured
        uint32 vendorEntry{0};   // the trip's first vendor (the world log, the Inventory rows)
        uint32 moneyAtStart{0};
        std::shared_ptr<UpkeepNeeds::VisitNeeds const> needs;  // the trip's needs (WU27): ErrandNeeds
        uint32 tripMs{0};  // the time the ended trips this status held ran (StatusEnd::NoTripMs: its deadline's clock)
    };
    struct Idle
    {
    };

    uint32 startT{0};  // start timestamp of the current status

    using RpgData = std::variant<Idle, GoGrind, GoCamp, WanderNpc, WanderRandom, DoQuest, Rest, TravelFlight,
                                 OutdoorPvP, DoGather, Travel, Errands, DoFish>;
    RpgData data;

    NewRpgStatus GetStatus();
    static NewRpgStatus StatusFromString(std::string const& name);
    bool HasStatusPersisted(uint32 maxDuration) { return GetMSTimeDiffToNow(startT) > maxDuration; }
    bool CanChangeTo(NewRpgStatus status);
    // The status for `rpg status`; a travel status shows its held trip (BotTravel::Trip)
    std::string ToString(HeldTrip const* trip = nullptr);

private:
    friend struct RpgStatusCore;  // the status core (NewRpgStatusEnd.cpp): a status ends and starts only there
    void ReplaceStatus(RpgData replacement) { data = std::move(replacement); }
};

#endif
