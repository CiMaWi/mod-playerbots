/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "LootInteraction.h"
#include "MountRules.h"
#include "StatusEndRules.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelProgress.h"

using namespace TravelLifecycle;

namespace
{
constexpr std::uint32_t MAP = 1;
constexpr std::uint64_t CAR = 42;
constexpr std::uint64_t NEARER_CAR = 43;
constexpr std::uint32_t SPLINE = 9;
constexpr Point DECK{4.0f, 6.0f, 8.0f};
constexpr Point PLATFORM{7.0f, 6.0f, 8.0f};
int passed = 0;
int failed = 0;

void Check(bool condition, std::string_view description)
{
    if (condition)
        ++passed;
    else
    {
        ++failed;
        std::cerr << "FAIL " << description << '\n';
    }
}

struct Adapter
{
    Membership Aboard{0, 0, true};
    SplineObservation Spline{MAP, SPLINE, false, false, true, {}};
    Contact ExpectedContact = Contact::Feet;
    Sample LastSample;
    std::uint32_t Samples = 0;
    std::uint32_t Probes = 0;
    std::uint32_t LastGapMs = 0;
    std::uint64_t LastExpected = 0;
    DockPose CurrentDock;
    bool DockAvailable = true;
    bool AcceptMutation = true;
    std::vector<std::string_view> Calls;

    Membership ReadMembership()
    {
        Calls.push_back("read");
        return Aboard;
    }

    SplineObservation ReadSpline() const { return Spline; }

    Surface Probe(std::uint64_t expected, bool)
    {
        Calls.push_back("probe");
        ++Probes;
        LastExpected = expected;
        return {expected ? expected : NEARER_CAR, 17, ExpectedContact};
    }

    DockState ObserveDock(PassengerMotion const& motion) const
    {
        if (!DockAvailable)
            return DockState::Unavailable;
        return SameDock(motion.Dock, CurrentDock) ? DockState::Still : DockState::Departed;
    }

    bool ExitAtIssuedDock(PassengerMotion const& motion)
    {
        Calls.push_back("confirm_exit");
        return DockAvailable && SameDock(motion.Dock, CurrentDock);  // the adapter's AtIssuedDock
    }

    void SettleOldFrame()
    {
        Calls.push_back("settle");
        Spline.Finalized = true;
    }
    void PublishFrame() { Calls.push_back("publish"); }

    void Attach(std::uint64_t guid)
    {
        Calls.push_back("attach");
        if (AcceptMutation)
            Aboard = {guid, 17, true};
    }

    void Detach(std::uint64_t)
    {
        Calls.push_back("detach");
        if (AcceptMutation)
            Aboard = {0, 0, true};
    }

    template <class Reconcile>
    void Observe(std::uint32_t, std::uint32_t gapMs, PassengerMotion const*, Reconcile reconcile)
    {
        ++Samples;
        LastGapMs = gapMs;
        LastSample = reconcile();
    }
};

PassengerMotion Motion(MotionKind kind, Point endpoint)
{
    PassengerMotion motion;
    motion.Kind = kind;
    motion.Transport = CAR;
    motion.Entry = 17;
    motion.Map = MAP;
    motion.Spline = SPLINE;
    motion.Endpoint = endpoint;
    return motion;
}

void CheckCadence()
{
    Cadence cadence;
    Adapter adapter;
    adapter.ExpectedContact = Contact::OffSurface;
    for (std::uint32_t const time : {0, 400, 999, 1000, 1500, 4300, 4301})
        Maintain(cadence, time, MAP, nullptr, adapter, CAR);
    Check(adapter.Probes == 3, "the pre-action path samples at the first tick, at one second and once after a gap");
    Check(adapter.LastGapMs == 3300, "the late update reports the elapsed gap rather than issuing catch-up probes");

    std::uint32_t constexpr BEFORE_WRAP = std::numeric_limits<std::uint32_t>::max() - 499;
    cadence = {};
    adapter = {};
    adapter.ExpectedContact = Contact::OffSurface;
    Maintain(cadence, BEFORE_WRAP, MAP, nullptr, adapter, CAR);
    Maintain(cadence, 499, MAP, nullptr, adapter, CAR);
    Check(adapter.Probes == 1, "999 ms over clock wrap is not yet due");
    Maintain(cadence, 500, MAP, nullptr, adapter, CAR);
    Check(adapter.Probes == 2 && adapter.LastGapMs == 1000, "1000 ms over clock wrap is due");
    Maintain(cadence, 501, MAP + 1, nullptr, adapter, CAR);
    Check(adapter.Probes == 3, "the first destination map tick revalidates before the normal cadence");
    cadence.Revalidate = true;
    Maintain(cadence, 502, MAP + 1, nullptr, adapter, CAR);
    Check(adapter.Probes == 4, "a validity interruption revalidates on the next valid tick");
}

void CheckBoard()
{
    Cadence cadence;
    Adapter adapter;
    PassengerMotion motion = Motion(MotionKind::Board, DECK);
    Maintain(cadence, 100, MAP, &motion, adapter);
    Check(adapter.Probes == 0 && motion.Result == Outcome::Pending, "active boarding preserves its world spline");
    adapter.Spline.Position = DECK;
    adapter.Spline.Finalized = true;
    adapter.Calls.clear();
    Maintain(cadence, 275, MAP, &motion, adapter);
    Check(adapter.Probes == 1 && adapter.LastGapMs == 175, "completion is sampled before the cadence is due");
    Check(adapter.LastExpected == CAR && adapter.Aboard.Guid == CAR, "the expected car wins over the nearer car");
    Check(adapter.Calls == std::vector<std::string_view>{"read", "probe", "settle", "attach", "read", "publish"},
          "world spline is settled before attachment and core membership is re-read");
    Check(motion.Result == Outcome::Boarded,
          "matching finalized spline, endpoint, feet and membership acknowledge board");
    Maintain(cadence, 276, MAP, &motion, adapter);
    Check(adapter.Probes == 1, "the completion is not sampled twice");
    Maintain(cadence, 1275, MAP, &motion, adapter);
    Check(adapter.Probes == 2 && adapter.Aboard.Guid == CAR && adapter.Calls.back() == "probe",
          "normal cadence preserves attachment without another AddPassenger");

    cadence = {};
    adapter = {};
    motion = Motion(MotionKind::Board, DECK);
    adapter.Spline.Finalized = true;
    adapter.ExpectedContact = Contact::OffSurface;
    Maintain(cadence, 10, MAP, &motion, adapter);
    Check(motion.Result == Outcome::Cancelled && !adapter.Aboard.Guid, "finalized short of the endpoint is cancelled");

    cadence = {};
    adapter = {};
    motion = Motion(MotionKind::Board, DECK);
    adapter.Spline.Id += 1;
    adapter.Spline.Position = DECK;
    adapter.Spline.Finalized = true;
    Maintain(cadence, 10, MAP, &motion, adapter);
    Check(!adapter.Aboard.Guid && motion.Result == Outcome::Cancelled,
          "a replacement spline cannot attach using a cancelled intent or acknowledge boarding");

    cadence = {};
    adapter = {};
    motion = Motion(MotionKind::Board, DECK);
    adapter.Spline.Position = DECK;
    adapter.Spline.Finalized = true;
    adapter.AcceptMutation = false;
    Maintain(cadence, 10, MAP, &motion, adapter);
    Check(motion.Result != Outcome::Boarded && !adapter.Aboard.Guid,
          "a core attach with no membership cannot acknowledge");
}

void CheckRideAndExit()
{
    Cadence cadence;
    Adapter adapter;
    adapter.Aboard = {CAR, 17, false};
    adapter.ExpectedContact = Contact::OffSurface;
    Maintain(cadence, 10, MAP, nullptr, adapter);
    Check(adapter.Aboard.Guid == CAR && adapter.Probes == 0, "cross-map unavailability cannot detach");

    adapter.Aboard.Available = true;
    adapter.ExpectedContact = Contact::Unavailable;
    Maintain(cadence, 1010, MAP, nullptr, adapter);
    Check(adapter.Aboard.Guid == CAR && adapter.Probes == 1, "missing model cannot detach");

    adapter.ExpectedContact = Contact::AboveDeck;
    Maintain(cadence, 2010, MAP, nullptr, adapter);
    Check(adapter.Aboard.Guid == CAR, "airborne over the ridden deck preserves the frame");

    adapter.ExpectedContact = Contact::OffSurface;
    Maintain(cadence, 3010, MAP, nullptr, adapter);
    Check(adapter.Aboard.Guid == CAR && cadence.OffSurface == 1,
          "one sample off the deck without a move of its own keeps the rider (WT54-6)");
    adapter.ExpectedContact = Contact::Feet;
    Maintain(cadence, 4010, MAP, nullptr, adapter);
    Check(adapter.Aboard.Guid == CAR && cadence.OffSurface == 0, "back on the deck: the count starts again");
    adapter.ExpectedContact = Contact::OffSurface;
    Maintain(cadence, 5010, MAP, nullptr, adapter);
    adapter.Calls.clear();
    Maintain(cadence, 6010, MAP, nullptr, adapter);
    Check(!adapter.Aboard.Guid && cadence.OffSurface == 0,
          "verified off-deck observation on two samples in a row detaches without a route");
    Check(adapter.Calls == std::vector<std::string_view>{"read", "probe", "settle", "detach", "read", "publish"},
          "local spline is settled before unexpected detach");

    // WT70e-a: a rider the vessel's mesh holds where the ray finds no deck stays aboard while it stands; an exit move
    // that ended there leaves by the ray's verdict, as one off the mesh does
    cadence = {};
    adapter = {};
    adapter.Aboard = {CAR, 17, true};
    adapter.ExpectedContact = Contact::OnMesh;
    Maintain(cadence, 1010, MAP, nullptr, adapter);
    Maintain(cadence, 2010, MAP, nullptr, adapter);
    Maintain(cadence, 3010, MAP, nullptr, adapter);
    Check(adapter.Aboard.Guid == CAR && cadence.OffSurface == 0 && adapter.Samples == 3,
          "a rider the vessel's mesh holds stays aboard over samples");
    cadence = {};
    adapter = {};
    adapter.Aboard = {CAR, 17, true};
    adapter.Spline.Local = true;
    adapter.ExpectedContact = Contact::OnMesh;
    {
        PassengerMotion exit = Motion(MotionKind::Exit, PLATFORM);
        Maintain(cadence, 10, MAP, &exit, adapter);
        adapter.Spline.Position = PLATFORM;
        adapter.Spline.Finalized = true;
        Maintain(cadence, 20, MAP, &exit, adapter);
        Check(!adapter.Aboard.Guid && exit.Result == Outcome::Exited,
              "an exit move that ended where the mesh holds the bot leaves by the ray");
    }

    cadence = {};
    adapter = {};
    adapter.Aboard = {CAR, 17, true};
    adapter.Spline.Local = true;
    adapter.ExpectedContact = Contact::OffSurface;
    PassengerMotion motion = Motion(MotionKind::Exit, PLATFORM);
    Maintain(cadence, 10, MAP, &motion, adapter);
    Check(adapter.Aboard.Guid == CAR && !adapter.Probes, "exit stays in the local frame until completion");
    adapter.Spline.Position = PLATFORM;
    adapter.Spline.Finalized = true;
    adapter.Calls.clear();
    Maintain(cadence, 20, MAP, &motion, adapter);
    Check(adapter.Calls ==
              std::vector<std::string_view>{"read", "probe", "confirm_exit", "settle", "detach", "read", "publish"},
          "accepted ground is validated before local frame settlement and core detach");
    Check(!adapter.Aboard.Guid && motion.Result == Outcome::Exited, "only verified core detach publishes the exit");

    cadence = {};
    adapter.Aboard = {CAR, 17, true};
    adapter.CurrentDock.Position.X = 1.0f;  // WT70c: an exit is confirmed by the issued dock alone (AtIssuedDock)
    motion = Motion(MotionKind::Exit, PLATFORM);
    Maintain(cadence, 20, MAP, &motion, adapter);
    Check(!adapter.Aboard.Guid && motion.Result == Outcome::Lost,
          "off the deck after the vessel left the issued dock is a loss");

    cadence = {};
    adapter.ExpectedContact = Contact::Feet;
    adapter.Calls.clear();
    motion = Motion(MotionKind::Exit, PLATFORM);
    Maintain(cadence, 20, MAP, &motion, adapter);
    Check(motion.Result == Outcome::Lost && !adapter.Aboard.Guid && adapter.Calls.size() == 1,
          "a stale exit intent cannot reattach after core removed membership");

    cadence = {};
    adapter = {};
    adapter.Aboard = {CAR, 17, true};
    adapter.Spline.Local = true;
    motion = Motion(MotionKind::Exit, PLATFORM);
    Maintain(cadence, 10, MAP, &motion, adapter);
    adapter.DockAvailable = false;
    adapter.Calls.clear();
    Maintain(cadence, 1010, MAP, &motion, adapter);
    Check(adapter.Aboard.Guid == CAR && motion.Result == Outcome::Pending &&
              adapter.Calls == std::vector<std::string_view>{"read"},
          "an unavailable dock observation preserves the pending local spline");
    adapter.DockAvailable = true;
    adapter.CurrentDock.Position.X = 1.0f;
    adapter.Calls.clear();
    Maintain(cadence, 2010, MAP, &motion, adapter);
    Check(adapter.Aboard.Guid == CAR && motion.Result == Outcome::Cancelled &&
              adapter.Calls == std::vector<std::string_view>{"read", "settle", "probe"},
          "departure cancels the owned local spline and preserves the rider over the deck");

    // WT70c (harness case 3): a jump by a stored row is never stopped in the air when the vessel leaves; once it ran
    // out it counts as cancelled
    cadence = {};
    adapter = {};
    adapter.Aboard = {CAR, 17, true};
    adapter.Spline.Local = true;
    motion = Motion(MotionKind::Exit, PLATFORM);
    motion.Jump = true;
    Maintain(cadence, 10, MAP, &motion, adapter);
    adapter.CurrentDock.Position.X = 1.0f;
    adapter.Calls.clear();
    Maintain(cadence, 1010, MAP, &motion, adapter);
    Check(adapter.Aboard.Guid == CAR && motion.Result == Outcome::Pending && !adapter.Spline.Finalized &&
              adapter.Calls == std::vector<std::string_view>{"read"},
          "departure under a jump: the jump is not stopped in the air");
    adapter.Spline.Finalized = true;
    adapter.Spline.Position = PLATFORM;
    adapter.Calls.clear();
    Maintain(cadence, 2010, MAP, &motion, adapter);
    Check(adapter.Aboard.Guid == CAR && motion.Result == Outcome::Cancelled &&
              std::find(adapter.Calls.begin(), adapter.Calls.end(), "settle") == adapter.Calls.end(),
          "the jump ran out after the departure: cancelled, nothing left to stop");

    cadence = {};
    adapter = {};
    adapter.Aboard = {CAR, 17, true};
    adapter.Spline.Local = true;
    adapter.Spline.Id += 1;
    motion = Motion(MotionKind::Exit, PLATFORM);
    Maintain(cadence, 10, MAP, &motion, adapter);
    Check(adapter.Aboard.Guid == CAR && motion.Result == Outcome::Cancelled &&
              adapter.Calls == std::vector<std::string_view>{"read", "probe"},
          "a replacement motion on the deck stays with its owner");
}

// The attach guard ported from PR #2747: a stale transport frame is cleared before AddPassenger, a listed passenger
// of the target stays
void CheckAttachGuard()
{
    Check(StepAttach(false, false, false) == AttachStep::Attach, "no prior transport: attach");
    Check(StepAttach(true, true, true) == AttachStep::Keep, "already a listed passenger of the target: no attach");
    Check(StepAttach(true, true, false) == AttachStep::ClearThenAttach,
          "a stale pointer at the target (not in its passenger set) is cleared before the attach");
    Check(StepAttach(true, false, true) == AttachStep::ClearThenAttach,
          "a passenger of another transport leaves it before the attach");
    Check(StepAttach(true, false, false) == AttachStep::ClearThenAttach,
          "a stale pointer at another transport is cleared before the attach");
}

void CheckDiscardedOwner()
{
    for (MotionKind const kind : {MotionKind::Board, MotionKind::Exit})
    {
        Cadence cadence;
        Adapter adapter;
        bool const exiting = kind == MotionKind::Exit;
        adapter.Spline.Local = exiting;
        adapter.Aboard = {exiting ? CAR : 0, exiting ? 17u : 0u, true};
        adapter.ExpectedContact = exiting ? Contact::AboveDeck : Contact::OffSurface;
        std::optional<PassengerMotion> intent = Motion(kind, exiting ? PLATFORM : DECK);
        Maintain(cadence, 10, MAP, &*intent, adapter);
        adapter.Calls.clear();
        DiscardMotion(intent, adapter);
        Check(!intent && adapter.Spline.Finalized && adapter.Calls == std::vector<std::string_view>{"settle"},
              exiting ? "replacement settles the old exit before discarding it"
                      : "stop settles the old board before discarding it");
        adapter.CurrentDock.Position.X = 1.0f;
        adapter.Calls.clear();
        Maintain(cadence, 1010, MAP, nullptr, adapter);
        Check(adapter.Aboard.Guid == (exiting ? CAR : 0) && adapter.Spline.Finalized &&
                  adapter.Calls == (exiting ? std::vector<std::string_view>{"read", "probe"}
                                            : std::vector<std::string_view>{"read"}),
              "departure after replacing the owner sees no crossing and preserves membership");
    }

    Adapter adapter;
    adapter.Spline.Local = true;
    adapter.Spline.Id += 1;
    std::optional<PassengerMotion> intent = Motion(MotionKind::Exit, PLATFORM);
    DiscardMotion(intent, adapter);
    Check(!intent && !adapter.Spline.Finalized && adapter.Calls.empty(),
          "a replacement spline ID stays active when the old intent is discarded");
    adapter.Spline.Id = SPLINE;
    adapter.Spline.Local = false;
    intent = Motion(MotionKind::Exit, PLATFORM);
    DiscardMotion(intent, adapter);
    Check(!intent && !adapter.Spline.Finalized && adapter.Calls.empty(), "a spline in a different frame is untouched");
    adapter.Spline.Local = true;
    adapter.Spline.Eligible = false;
    intent = Motion(MotionKind::Exit, PLATFORM);
    intent->Result = Outcome::Cancelled;
    DiscardMotion(intent, adapter);
    Check(!intent && adapter.Spline.Finalized && adapter.Calls == std::vector<std::string_view>{"settle"},
          "an interrupted intent still settles its active matching spline");
}

void CheckIneligibleMaintenance()
{
    for (bool const replacement : {false, true})
    {
        Cadence cadence;
        Adapter adapter;
        adapter.Aboard = {CAR, 17, true};
        adapter.Spline.Local = true;
        PassengerMotion motion = Motion(MotionKind::Exit, PLATFORM);
        Maintain(cadence, 10, MAP, &motion, adapter);
        adapter.Calls.clear();
        adapter.Spline.Eligible = false;
        if (replacement)
            ++adapter.Spline.Id;
        Maintain(cadence, 11, MAP, &motion, adapter);
        Check(motion.Result == Outcome::Cancelled && adapter.Spline.Finalized != replacement,
              replacement ? "maintenance preserves a different combat spline on cancellation"
                          : "maintenance settles its matching active exit immediately on ineligibility");
        adapter.CurrentDock.Position.X = 1.0f;
        Maintain(cadence, 1011, MAP, &motion, adapter);
        Check(adapter.Aboard.Guid == CAR && adapter.Spline.Finalized != replacement,
              "departure with scheduled travel suppressed leaves no active retired crossing and keeps the rider");
    }
}

void CheckUnconsumedBoardTransfer()
{
    // The held trip's stand-in (HeldTrip: the legs and the passenger motion), held as BotTravel holds it (WT64f)
    struct Trip
    {
        TravelLegs::Trip trip;
        std::optional<PassengerMotion> passengerMotion;
    };
    StatusEnd::TripHold<Trip> hold;
    auto& before = hold.Begin(StatusEnd::STATUS_TRAVEL, Trip{});
    TravelRoute::Leg leg;
    leg.type = TravelRoute::LegType::Transport;
    leg.object = 17;
    leg.from = {MAP, DECK.X, DECK.Y, DECK.Z};
    leg.to = {MAP + 1, 100.0f, 100.0f, 8.0f};
    before.trip.legs.push_back(leg);
    before.trip.boarding = TravelLegs::Boarding::Board;
    before.passengerMotion = Motion(MotionKind::Board, DECK);
    Cadence cadence;
    Adapter adapter;
    adapter.Spline.Finalized = true;
    adapter.Spline.Position = DECK;
    Maintain(cadence, 10, MAP, &*before.passengerMotion, adapter);
    Check(before.passengerMotion->Result == Outcome::Boarded && !before.trip.passengerGuid,
          "maintenance attaches while the scheduled follower has not consumed boarding");
    ResetMotion(before.passengerMotion, true, adapter);
    // A far teleport keeps the trip as it is with its holder (StatusEnd::KeepsStatus: RpgStatus::Reset leaves both)
    Check(hold.Reset(true) == StatusEnd::ResetEffect::Kept && hold.Held() == &before &&
              hold.Holder() == StatusEnd::STATUS_TRAVEL,
          "a far reset keeps the trip");
    auto& after = *hold.Held();
    Check(after.passengerMotion && after.passengerMotion->Result == Outcome::Boarded,
          "the far reset policy preserves the unconsumed boarding result with the continuing trip");
    adapter.Aboard.Available = false;
    Check(!PassengerOutcome(*after.passengerMotion, adapter.ReadMembership()),
          "unavailable cross-map membership defers boarding consumption");
    adapter.Aboard.Available = true;
    adapter.Spline.Map = MAP + 1;
    adapter.Spline.Local = true;
    Maintain(cadence, 11, MAP + 1, &*after.passengerMotion, adapter);
    TravelLegs::Observation seen;
    seen.bot = {MAP + 1, DECK.X, DECK.Y, DECK.Z};
    seen.nowMs = 11;
    seen.aboard = adapter.Aboard.Entry;
    seen.aboardGuid = adapter.Aboard.Guid;
    seen.passengerOutcome = PassengerOutcome(*after.passengerMotion, adapter.ReadMembership());
    Check(seen.passengerOutcome == Outcome::Boarded, "coherent destination membership revalidates the retained result");
    TravelLegs::StepLeg(after.trip, seen);
    Check(after.trip.passengerGuid == CAR && after.trip.boarding == TravelLegs::Boarding::Ride,
          "the scheduled follower consumes boarding after transfer without starting OffFirst");
    Check(PassengerOutcome(*after.passengerMotion, {NEARER_CAR, 17, true}) == Outcome::Lost,
          "a different destination passenger GUID cannot consume the retained boarding result");
    Check(PassengerOutcome(*after.passengerMotion, {CAR, 18, true}) == Outcome::Lost,
          "a different destination transport entry cannot consume the retained boarding result");
    ResetMotion(after.passengerMotion, false, adapter);
    Check(!after.passengerMotion, "an ordinary reset discards even a confirmed boarding result");
    after.passengerMotion = Motion(MotionKind::Exit, PLATFORM);
    adapter.Spline = {MAP, SPLINE, true, false, true, DECK};
    ResetMotion(after.passengerMotion, true, adapter);
    Check(!after.passengerMotion && adapter.Spline.Finalized,
          "far reset settles and discards an active old-frame crossing");
}

// WT64f (PLAN 3.20 (3), INTERFACE 1): the trip is held in the bot's travel block, not in its status. The status core
// begins it with its holder (RpgStatusCore::Replace), ends it before the holder with the same outcome (Change) and
// applies the reset rule (RpgStatus::Reset); the "trip held" trigger asks who acts first (StatusEnd::WhoActsFirst)
void CheckHeldTrip()
{
    using StatusEnd::FirstActor;
    using StatusEnd::STATUS_IDLE;
    using StatusEnd::STATUS_TRAVEL;
    using StatusEnd::WhoActsFirst;
    struct Trip
    {
        TravelLegs::Trip trip;
        std::optional<PassengerMotion> passengerMotion;
        std::uint32_t tripId = 0;
    };
    StatusEnd::TripHold<Trip> hold;

    // The stepper ahead of the holder: the follower while the status holds the trip, the holder otherwise
    Check(WhoActsFirst(hold.Held() != nullptr, hold.Holder(), STATUS_IDLE) == FirstActor::Holder,
          "no trip held: the status's own executor acts");
    hold.Begin(STATUS_TRAVEL, Trip{{}, std::nullopt, 7});
    Check(hold.Held() && hold.Held()->tripId == 7 && hold.Holder() == STATUS_TRAVEL, "a trip begins with its holder");
    Check(WhoActsFirst(hold.Held() != nullptr, hold.Holder(), STATUS_TRAVEL) == FirstActor::Follower,
          "a held trip: the route follower steps ahead of the holder's executor");
    Check(WhoActsFirst(true, STATUS_TRAVEL, StatusEnd::STATUS_ERRANDS) == FirstActor::Holder,
          "a trip held by another status than the bot's: the follower does not step it");

    // A holder's end ends its trip, with the same outcome, once
    Check(hold.End(StatusEnd::Outcome::Arrived, "arrived") && !hold.Held() && hold.Holder() == STATUS_IDLE,
          "the holder's end ends its trip");
    Check(hold.LastEnd().outcome == StatusEnd::Outcome::Arrived && std::string_view(hold.LastEnd().word) == "arrived",
          "the trip's end keeps the holder's outcome and word");
    Check(
        !hold.End(StatusEnd::Outcome::Abandoned, "abandoned") && hold.LastEnd().outcome == StatusEnd::Outcome::Arrived,
        "an ended trip does not end twice");
    Check(WhoActsFirst(hold.Held() != nullptr, hold.Holder(), STATUS_IDLE) == FirstActor::Holder,
          "after the trip's end the holder acts again");

    // A trip replacing a trip: the old one ends first, the new one is begun with the new holder
    hold.Begin(STATUS_TRAVEL, Trip{{}, std::nullopt, 8});
    Check(hold.End(StatusEnd::Outcome::Interrupted, "pick") && !hold.Held(), "a replaced trip ends first");
    hold.Begin(STATUS_TRAVEL, Trip{{}, std::nullopt, 9});
    Check(hold.Held() && hold.Held()->tripId == 9, "the replacing trip is held, not the old one");

    // A reset: an ordinary one ends the trip without a row, as Interrupted; with none held it changes nothing
    Adapter adapter;
    hold.Held()->passengerMotion = Motion(MotionKind::Board, DECK);
    ResetMotion(hold.Held()->passengerMotion, false, adapter);
    Check(!hold.Held()->passengerMotion, "an ordinary reset discards the trip's passenger motion");
    Check(hold.Reset(false) == StatusEnd::ResetEffect::Fresh && !hold.Held() && hold.Holder() == STATUS_IDLE &&
              hold.LastEnd().outcome == StatusEnd::Outcome::Interrupted,
          "an ordinary reset ends the held trip with its holder");
    Check(hold.Reset(true) == StatusEnd::ResetEffect::Fresh && !hold.Held(),
          "a far reset with no trip held starts the status over");

    // A ride kept across a far teleport: the trip, its holder and its confirmed boarding go on
    Trip& riding = hold.Begin(STATUS_TRAVEL, Trip{{}, std::nullopt, 10});
    riding.trip.boarding = TravelLegs::Boarding::Ride;
    riding.trip.passengerGuid = CAR;
    riding.passengerMotion = Motion(MotionKind::Board, DECK);
    riding.passengerMotion->Result = Outcome::Boarded;
    ResetMotion(riding.passengerMotion, true, adapter);
    Check(hold.Reset(true) == StatusEnd::ResetEffect::Kept && hold.Held() == &riding &&
              hold.Holder() == STATUS_TRAVEL && riding.tripId == 10,
          "a far teleport keeps the ride's trip with its holder");
    Check(riding.trip.passengerGuid == CAR && riding.trip.boarding == TravelLegs::Boarding::Ride &&
              riding.passengerMotion && riding.passengerMotion->Result == Outcome::Boarded,
          "the kept ride keeps its passenger and its confirmed boarding");
    Check(StatusEnd::ResetOf(true, hold.Held() != nullptr).status == StatusEnd::ResetEffect::Kept,
          "the reset list keeps the holder's status with the kept trip");
    Check(WhoActsFirst(hold.Held() != nullptr, hold.Holder(), STATUS_TRAVEL) == FirstActor::Follower,
          "after the far teleport the follower steps the kept ride first");
}

void CheckFollowAndNoTrip()
{
    Adapter adapter;
    Sample sample = Reconcile(adapter, nullptr, MotionState::None, CAR);
    Check(!sample.Before.Guid && sample.After.Guid == CAR, "a follower on its master's transport: attached");
    adapter.ExpectedContact = Contact::OffSurface;
    sample = Reconcile(adapter, nullptr, MotionState::None, NEARER_CAR);
    Check(sample.After.Guid == CAR, "master attach request cannot detach a different current ride");
}

// WT62 (WT.48): boarding has one owner, the trip. A bot on no transport is probed only for the vessel it means to
// board (the test adapter's contact is NEARER_CAR for a probe that names none)
void CheckBoardingByIntent()
{
    Cadence cadence;
    Adapter adapter;
    Maintain(cadence, 0, MAP, nullptr, adapter);
    Check(!adapter.Aboard.Guid && adapter.Probes == 0 && adapter.Samples == 0,
          "a hull sweeps a waiting bot with no leg: not attached");

    TravelRoute::Leg leg;
    leg.type = TravelRoute::LegType::Transport;
    leg.object = 17;
    leg.from = {MAP, DECK.X, DECK.Y, DECK.Z};
    leg.to = {MAP + 1, 100.0f, 100.0f, 8.0f};
    TravelLegs::Trip trip;
    trip.legs.push_back(leg);
    trip.sight.found = true;
    trip.sight.guid = CAR;
    Check(!TravelLegs::BoardingIntent(trip), "no boarding intent on the approach to the wait point");
    trip.boarding = TravelLegs::Boarding::Wait;
    cadence = {};
    adapter = {};
    Maintain(cadence, 0, MAP, nullptr, adapter, TravelLegs::BoardingIntent(trip));
    TravelLegs::Observation seen;
    seen.bot = {MAP, DECK.X, DECK.Y, DECK.Z};
    seen.nowMs = 10;
    seen.aboard = adapter.Aboard.Entry;
    seen.aboardGuid = adapter.Aboard.Guid;
    TravelLegs::StepLeg(trip, seen);
    Check(adapter.Aboard.Guid == CAR && adapter.LastExpected == CAR && trip.passengerGuid == CAR &&
              trip.boarding == TravelLegs::Boarding::Ride && trip.transportNotes.size() == 1 &&
              trip.transportNotes[0].event == TravelLegs::TransportEvent::Boarded,
          "the leg's vessel docks under the bot: Boarded");
    Check(!TravelLegs::BoardingIntent(trip), "the ride owns its vessel: no boarding intent");

    cadence = {};
    adapter = {};
    adapter.Aboard = {NEARER_CAR, 17, true};
    Maintain(cadence, 0, MAP, nullptr, adapter, CAR);
    Check(adapter.LastExpected == NEARER_CAR && adapter.Aboard.Guid == NEARER_CAR,
          "a rider is probed for the vessel it rides, not for an intent");
}

// WT62 review finding 1: a follower means to board its master's transport, a lift or the tram as well as a ship. It is
// attached by feet contact and leaves by the off-surface rule, as any rider; no other bot is probed
void CheckFollowerByMaster()
{
    constexpr std::uint64_t LIFT = 44;
    constexpr std::uint64_t BOAT = 45;
    Check(ChooseIntent({CAR, NEARER_CAR, LIFT, BOAT}) == CAR &&
              ChooseIntent({0, NEARER_CAR, LIFT, BOAT}) == NEARER_CAR && ChooseIntent({0, 0, LIFT, BOAT}) == LIFT &&
              ChooseIntent({0, 0, 0, BOAT}) == BOAT && !ChooseIntent({}),
          "intent order: aboard placement, leg, master's transport, the battleground boat");

    Cadence cadence;
    Adapter adapter;
    Maintain(cadence, 0, MAP, nullptr, adapter, ChooseIntent({0, 0, LIFT, 0}));
    Check(adapter.Aboard.Guid == LIFT && adapter.LastExpected == LIFT && adapter.Probes == 1,
          "a follower whose master rides a lift or the tram: attached");

    cadence = {};
    adapter = {};
    Maintain(cadence, 0, MAP, nullptr, adapter, ChooseIntent({}));
    Check(!adapter.Aboard.Guid && adapter.Probes == 0 && adapter.Samples == 0,
          "a follower whose master is not aboard: not attached");

    cadence = {};
    adapter = {};
    adapter.Aboard = {LIFT, 17, true};
    adapter.ExpectedContact = Contact::OffSurface;
    for (std::uint32_t now = 0; now <= 4 * PASSENGER_INTERVAL_MS && adapter.Aboard.Guid; now += PASSENGER_INTERVAL_MS)
        Maintain(cadence, now, MAP, nullptr, adapter, ChooseIntent({}));
    Check(!adapter.Aboard.Guid && adapter.Probes == OFF_SURFACE_SAMPLES,
          "a follower that steps off the lift after its master: detached by the off-surface rule");
}

struct LootAdapter
{
    LootInteraction::Transaction State;
    uint32_t Now = 100;
    uint64_t Selected = 12;
    uint64_t Window = 0;
    uint64_t Deferred = 0;
    uint64_t LeftGuid = 0;  // the one target the adapter models as left for room (LootTarget::left)
    LootInteraction::LeftForRoom Left;
    uint32_t FreeSlots = 0;
    unsigned Removes = 0;
    bool Valid = true;
    bool Moving = true;
    bool Mounted = true;
    bool CanRequest = true;
    bool CacheValid = true;
    unsigned Requests = 0;
    unsigned Releases = 0;
    uint32_t Completed = 0;
    std::string_view CloseDetail;

    void Response(uint32_t gold, uint32_t stored, bool gameObject, bool leftForRoom = false)
    {
        LootInteraction::CompleteResponse(
            State, Window, gold, stored, gameObject, leftForRoom, [&] { Deferred = Window; },
            [&]
            {
                LeftGuid = Window;
                Left.Leave(LootInteraction::FreeAfterStore(FreeSlots, stored));
            },
            [&] { ++Removes; }, [&] { ++Releases; });
    }
    void ObserveClose()
    {
        LootInteraction::ConsumeClosed(State, Window, Completed,
                                       [&](char const* detail)
                                       {
                                           CloseDetail = detail;
                                           if (Selected == State.Guid)
                                               Selected = 0;
                                           State.Clear();
                                           CacheValid = false;
                                       });
    }

    bool Admit()
    {
        return Valid && Deferred != Selected && !(LeftGuid == Selected && Left.Blocks(FreeSlots)) &&
               State.Prepare(Selected, Now);
    }
    bool PrepareMotion()
    {
        if (Moving)
        {
            Moving = false;
            return true;
        }
        if (Mounted)
        {
            Mounted = false;
            return true;
        }
        return false;
    }
    bool Request()
    {
        if (!CanRequest)
            return false;
        ++Requests;
        return true;
    }
    void Begin() { State.Begin(Selected, Now); }
    void Defer() { Cancel("dry"); }
    void Cancel(std::string_view)
    {
        if (Window == State.Guid)
        {
            ++Releases;
            Window = 0;
        }
        if (Selected == State.Guid)
            Selected = 0;
        Deferred = State.Guid;
        State.Clear();
        CacheValid = false;
    }
    bool MayMount() const
    {
        MountRules::Facts facts;
        facts.lootInteraction = State.Guid != 0;
        facts.lootWindow = Window != 0;
        return MountRules::Policy(facts, MountRules::Kind::Ground).allowed;
    }
};

// The one mount owner (WT44): each rule against both mount paths, the mount action's ground request
// (CheckMountStateAction::Execute) and a fly leg's request (MountToFly)
void CheckMountPolicy()
{
    using MountRules::Facts;
    using MountRules::Kind;
    using MountRules::Policy;
    for (Kind const path : {Kind::Ground, Kind::Flying})
    {
        std::string const name = path == Kind::Ground ? "mount action" : "fly leg";
        MountRules::Verdict const free = Policy(Facts{}, path);
        Check(free.allowed && !free.waits && free.kind == path && std::string_view(free.why) == "allowed",
              name + ": no loot, no constraint, no on-foot trip: allowed, the kind the caller asked for");

        Facts interaction;
        interaction.lootInteraction = true;
        Facts window;
        window.lootWindow = true;
        Facts near;
        near.lootNear = true;
        bool vetoed = true;
        for (Facts const& facts : {interaction, window, near})
        {
            MountRules::Verdict const verdict = Policy(facts, path);
            vetoed = vetoed && !verdict.allowed && verdict.waits && verdict.kind == Kind::None &&
                     std::string_view(verdict.why) == "loot";
        }
        Check(vetoed, name + ": loot veto (interaction, open window, corpse in reach): waits, uncounted");

        Facts onFoot;
        onFoot.onFoot = true;
        MountRules::Verdict const foot = Policy(onFoot, path);
        Check(!foot.allowed && !foot.waits && std::string_view(foot.why) == "on_foot",
              name + ": on-foot rule (a transport leg, a tame): refused");

        Facts noMount;
        noMount.noMount = true;
        noMount.onFoot = true;
        MountRules::Verdict const test = Policy(noMount, path);
        Check(!test.allowed && !test.waits && std::string_view(test.why) == "nomount",
              name + ": test constraint nomount: refused, ahead of the on-foot rule");

        Facts all = noMount;
        all.lootNear = true;
        Check(std::string_view(Policy(all, path).why) == "loot", name + ": loot first: the bot loots, then rides");
    }
    Check(!Policy(Facts{}, Kind::None).allowed, "no kind asked: nothing to mount");
    Check(std::string_view(MountRules::HowWord(MountRules::How::Plain)) == "plain" &&
              std::string_view(MountRules::HowWord(MountRules::How::Transport)) == "transport" &&
              std::string_view(MountRules::HowWord(MountRules::How::Loot)) == "loot" &&
              std::string_view(MountRules::HowWord(MountRules::How::Taxi)) == "taxi" &&
              std::string_view(MountRules::HowWord(MountRules::How::Fly)) == "fly" &&
              std::string_view(MountRules::HowWord(MountRules::How::Hearth)) == "hearth",
          "Dismount(how): one word per dismount path for the trace");
}

// Upkeep WU36: a corpse a full-bag bot cannot loot is opened once, and again only once its free slots rise
void CheckLootLeftForRoom()
{
    using LootInteraction::Attempt;
    using LootInteraction::Execute;

    LootAdapter loot;
    loot.Moving = false;
    loot.Mounted = false;
    Check(Execute(loot) == Attempt::Requested && loot.Requests == 1, "a full-bag bot requests the corpse once");
    loot.Window = loot.State.Guid;
    Check(loot.State.Open(loot.Window, loot.Now), "the corpse's loot window opens");
    loot.Response(0, 0, false, true);
    loot.Window = 0;
    loot.ObserveClose();
    Check(loot.LeftGuid == 12 && loot.Left.Left && !loot.Removes && !loot.Deferred && loot.Releases == 1 &&
              loot.CloseDetail == "left" && !loot.Completed,
          "a creature response with nothing stored for lack of room is left, not removed");

    loot.Selected = 12;
    for (unsigned scan = 0; scan < 3; ++scan)
    {
        loot.Now += 5000;
        Check(Execute(loot) == Attempt::Failed && loot.Requests == 1 && !loot.State.Guid,
              "no attempt while the free slots are unchanged");
    }

    loot.FreeSlots = 1;
    Check(Execute(loot) == Attempt::Requested && loot.Requests == 2, "one attempt after they rise");
    loot.Window = loot.State.Guid;
    loot.State.Open(loot.Window, loot.Now);
    loot.Response(0, 1, false);
    loot.Window = 0;
    loot.ObserveClose();
    Check(loot.Removes == 1 && loot.CloseDetail == "closed" && loot.Completed == 1,
          "the stored corpse is done and removed");

    LootInteraction::LeftForRoom partial;
    partial.Leave(LootInteraction::FreeAfterStore(1, 1));
    Check(partial.Blocks(0) && !partial.Blocks(1),
          "a response that stored one item into the last slot and left another waits for a slot freed after it");
    Check(!LootInteraction::LeftForRoom{}.Blocks(0), "a target never left is not blocked by full bags");

    LootAdapter chest;
    chest.Moving = false;
    chest.Mounted = false;
    Execute(chest);
    chest.Window = chest.State.Guid;
    chest.State.Open(chest.Window, chest.Now);
    chest.Response(0, 0, true);
    Check(chest.Deferred == 12 && !chest.Left.Left && !chest.Removes,
          "a game object with nothing stored and nothing left keeps its time backoff");
}

void CheckLootScheduler()
{
    using LootInteraction::Attempt;
    using LootInteraction::ConsumeExpiry;
    using LootInteraction::Execute;
    using LootInteraction::Phase;
    using LootInteraction::PhaseMs;

    LootAdapter loot;
    Check(Execute(loot) == Attempt::Prepared && !loot.Moving && loot.Mounted && !loot.State.Pending(),
          "moving mounted preparation consumes one cycle without a request");
    Check(!loot.MayMount(), "speed packet before the next action observes preparation ownership");
    loot.Now += 100;
    Check(Execute(loot) == Attempt::Prepared && !loot.Mounted && !loot.State.Pending() && !loot.MayMount(),
          "ground dismount consumes the next cycle without freeing the timer mount path");

    loot.Now = 100 + PhaseMs - 1;
    loot.State.Observe(loot.Now, false);
    loot.Moving = true;
    Check(Execute(loot) == Attempt::Prepared && !loot.State.Expired && loot.State.Generation == 1,
          "repeated preparation at the deadline margin neither resets the timer nor issues a request");
    ++loot.Now;
    loot.State.Observe(loot.Now, false);
    Check(loot.State.Expired && Execute(loot) == Attempt::Failed && !loot.MayMount(),
          "pre-gate sample latches expiry and prevents action/packet attempts before cleanup");
    Check(ConsumeExpiry(loot.State, loot) && loot.Deferred == 12 && !loot.CacheValid && !loot.Selected &&
              loot.MayMount() && !loot.Releases && !loot.Requests && !ConsumeExpiry(loot.State, loot),
          "scheduled timeout consumes once, without fabricated loot packets");
    loot.Selected = 12;
    Check(Execute(loot) == Attempt::Failed, "reselecting a deferred target cannot readmit it");

    loot = {};
    Check(Execute(loot) == Attempt::Prepared, "prepare transaction starts");
    loot.Now += 10000;
    loot.Mounted = false;
    Check(Execute(loot) == Attempt::Requested && loot.Requests == 1 && loot.State.Pending(),
          "request follows preparation on the same generation");
    Check(!loot.State.Begin(12, loot.Now + 2000), "repeat Begin is rejected");
    loot.State.Observe(100 + PhaseMs, false);
    Check(loot.State.Expired && !loot.State.Open(12, 100 + PhaseMs), "request cannot rearm preparation deadline");

    loot = {};
    loot.Moving = false;
    loot.Mounted = false;
    Check(Execute(loot) == Attempt::Requested, "stationary target can request directly");
    Check(!loot.State.Open(13, 1000) && loot.State.Open(12, 2000) && loot.State.Current == Phase::AwaitingRelease,
          "only a matching first response starts the release allowance");
    loot.Window = 12;
    Check(!loot.State.Open(12, 15000), "duplicate response cannot rearm release");
    loot.State.Observe(2000 + PhaseMs, false);
    loot.Selected = 13;
    Check(ConsumeExpiry(loot.State, loot) && loot.Releases == 1 && loot.Selected == 13 &&
              !loot.State.Open(12, 2001 + PhaseMs),
          "expiry releases only a matching window and keeps a different selection");
    loot.Window = 13;
    Check(!loot.MayMount(), "untracked real loot window vetoes both mount callers");

    loot = {};
    loot.Moving = false;
    loot.Mounted = false;
    loot.CanRequest = false;
    Check(Execute(loot) == Attempt::Failed && loot.Deferred == 12 && !loot.State.Pending() && !loot.CacheValid &&
              !loot.Requests,
          "dry failure defers and invalidates without starting a pending request");
    loot = {};
    loot.Valid = false;
    Check(Execute(loot) == Attempt::Failed && !loot.State.Guid, "invalid target cannot acquire ownership");

    loot = {};
    loot.Now = std::numeric_limits<uint32_t>::max() - 10;
    Check(Execute(loot) == Attempt::Prepared, "prepare across a wrapping map clock");
    loot.State.Observe(loot.Now + 4000, true);
    loot.State.Observe(loot.Now + 100000, true);
    Check(!loot.State.Expired && !loot.State.AllowsAttempt(12), "control loss suspends rather than resets the clock");
    loot.State.Observe(loot.Now + 200000, false);
    loot.State.Observe(loot.Now + 200000 + PhaseMs - 4001, false);
    Check(!loot.State.Expired, "resume keeps the prior eligible balance across clock wrap");
    loot.State.Observe(loot.Now + 200000 + PhaseMs - 4000, false);
    loot.State.Observe(loot.Now + 300000, true);
    Check(loot.State.Expired, "later control loss preserves latched expiry");
}

void CheckProgressScheduler()
{
    using namespace TravelProgress;
    using TravelLegs::LegType;
    using TravelRoute::Leg;
    TravelRoute::Point const start{MAP, 0.0f, 0.0f, 0.0f};
    TravelRoute::Point const goal{MAP, 100.0f, 0.0f, 0.0f};
    auto const nodes = [](uint32_t) { return TravelRoute::Point{}; };
    Leg walk;
    walk.type = LegType::Walk;
    walk.from = start;
    walk.to = goal;
    walk.seconds = 100.0f;

    State state;
    TravelLegs::Trip trip;
    TravelLegs::RouteWait wait;
    TravelProgress::Motion motion{1, true, 0.0f, true, 1, {}};
    unsigned reads = 0;
    auto const read = [&]
    {
        ++reads;
        return motion;
    };
    auto const maintain = [&](uint32_t now, Owner owner, uint32_t completed = 0)
    { return Maintain(state, {now, owner, owner == Owner::Combat, completed}, trip, wait, 100.0f, read); };

    Check(maintain(100, Owner::Walk).Selected, "pre-gate sample observes selected travel before a request");
    maintain(90'100, Owner::Walk);
    Check(state.Reason() == Overdue::Walk && !state.AttemptedWalk() && !wait.waiting,
          "pre-request starvation reaches its clock with no attempted edge");
    Check(ScheduledWork(true, state.Reason(), true, true) == Work::Recover &&
              ScheduledWork(false, state.Reason(), true, true) == Work::Hold,
          "latched travel recovery beats loot expiry and combat blocks both");

    for (bool following : {false, true})
    {
        state = {};
        trip = {};
        wait = {};
        std::string_view resumed;
        motion = {1, true, 0.0f, true, 1, {}};
        auto const move = [&]
        {
            motion = {motion.Id + 1, true, 0.0f, true, 1, {}};
            return true;
        };
        auto const onResume = [&](char const* reason) { resumed = reason; };
        if (following)
        {
            trip.legs = {walk};
            TravelLegs::StartLeg(trip, 0, 100, nodes);
            ContinueWalk(state, read, move, onResume);
        }

        maintain(100, Owner::Walk);
        LootAdapter loot;
        Check(LootInteraction::Execute(loot) == LootInteraction::Attempt::Prepared,
              "normal travel yields to preparation");
        maintain(101, Owner::Loot);
        loot.Now = 100 + LootInteraction::PhaseMs - 1;
        loot.State.Observe(loot.Now, false);
        loot.Moving = true;
        Check(LootInteraction::Execute(loot) == LootInteraction::Attempt::Prepared &&
                  ScheduledWork(true, state.Reason(), true, loot.State.Expired) == Work::Hold,
              "repeat preparation holds both first request and existing route before the deadline");
        loot.State.Observe(++loot.Now, false);
        maintain(loot.Now, Owner::Loot);
        Check(ScheduledWork(true, state.Reason(), true, loot.State.Expired) == Work::DeferLoot &&
                  LootInteraction::ConsumeExpiry(loot.State, loot) && loot.Deferred == 12 && !loot.CacheValid &&
                  loot.MayMount() && !loot.Requests && !loot.Releases,
              "scheduled defer clears one matching owner and frees both mount paths");
        maintain(loot.Now + 1, Owner::Walk);
        Check(ScheduledWork(true, state.Reason(), loot.State.Guid != 0, loot.State.Expired) == Work::Continue,
              "defer leaves the same travel obligation ready to act");
        if (following)
        {
            Check(ContinueWalk(state, read, move, onResume) && resumed == "loot" && state.Owns(motion.Id),
                  "scheduled walking continuation commits a real adapter move and publishes loot resume");
            Check(maintain(loot.Now + 1100, Owner::Walk).Yards == 0.0f,
                  "defer and an issued move without path advance grant no progress");
        }
        else
        {
            unsigned submitted = 0;
            auto const submit = [&] { return ++submitted == 2; };
            TravelLegs::RouteWait const request{17, MAP, loot.Now + 1, true, start};
            Check(!SubmitRoute(wait, request, submit) && !wait.waiting, "failed submit cannot start route waiting");
            Check(SubmitRoute(wait, request, submit) && wait.waiting && wait.requestId == 17,
                  "scheduled first request after loot calls the submitter and publishes its actual pending ID");
            maintain(loot.Now + 100'000, Owner::Exempt);
            Check(state.Reason() == Overdue::Due &&
                      TravelLegs::Judge(wait, 16, start, loot.Now + 2) == TravelLegs::Verdict::Drop &&
                      TravelLegs::Judge(wait, 17, goal, loot.Now + 2) == TravelLegs::Verdict::Refresh &&
                      TravelLegs::Judge(wait, 17, start, loot.Now + 100'000) == TravelLegs::Verdict::Refresh,
                  "pre-gate route bound rejects stale IDs, displaced starts and delayed answers");
        }
    }

    state = {};
    trip = {};
    wait = {};
    trip.legs = {walk};
    TravelLegs::StartLeg(trip, 0, 100, nodes);
    maintain(100, Owner::Walk);
    motion = {1, true, 0.0f, true, 1, {}};
    auto const move = [&]
    {
        motion = {motion.Id + 1, true, 0.0f, true, 1, {}};
        return true;
    };
    std::string_view resumed;
    auto const onResume = [&](char const* reason) { resumed = reason; };
    ContinueWalk(state, read, move, onResume);
    motion.Yards = 12.0f;
    Check(maintain(1100, Owner::Walk).Yards == 12.0f, "owned path length counts even on a detour away from the goal");
    unsigned const before = reads;
    maintain(1200, Owner::Walk);
    Check(reads == before, "one clock comparison skips another spline sample within one second");
    maintain(21'100, Owner::Walk);
    Check(maintain(21'101, Owner::Combat).CombatChanged, "combat starts at the observed core state edge");
    motion.Yards = 70.0f;
    maintain(221'101, Owner::Combat);
    Check(state.Reason() == Overdue::None && state.LegMs() == 21'000 && state.RouteMs() == 21'000,
          "combat excludes time from both active deadlines and does not latch walking recovery");
    auto const ended = maintain(221'102, Owner::Walk);
    Check(ended.CombatChanged && ended.Yards == 0.0f && maintain(222'102, Owner::Walk).Yards == 0.0f,
          "combat movement is rebased without credit");
    State interrupted = state;
    Check(!interrupted.WaypointReached(), "a waypoint reached by combat displacement alone is not credited");
    motion.PositionMatches = false;
    motion.Yards = 90.0f;
    State displaced = state;
    Check(displaced.ObserveMotion(motion) == 0.0f, "core spline time without matching world position is not credited");
    motion.PositionMatches = true;
    Check(ContinueWalk(state, read, move, onResume) && resumed == "combat",
          "the actual scheduled walk adapter commits movement after combat");
    motion.Yards = 12.0f;
    Check(maintain(223'102, Owner::Walk).Yards == 12.0f, "resumed walk credits new ground beyond the combat baseline");
    motion.Yards = 20.0f;
    Check(maintain(224'102, Owner::Walk).Yards == 8.0f, "continued owned spline earns further distance");
    maintain(250'102, Owner::Walk);
    maintain(251'102, Owner::Control);
    maintain(450'102, Owner::Control);
    maintain(451'102, Owner::Walk);
    Check(state.Reason() == Overdue::None && state.RouteMs() == 50'000,
          "control interruption preserves active time and excludes its duration");
    maintain(471'102, Owner::Walk, 1);
    maintain(560'102, Owner::Walk, 1);
    Check(state.Reason() == Overdue::None, "normal matching loot completion clears the no-progress balance");
    maintain(561'102, Owner::Walk, 1);
    Check(state.Reason() == Overdue::Walk, "stationary active owned spline expires on the next eligible sample");

    state = {};
    trip = {};
    maintain(100, Owner::Walk);
    maintain(30'100, Owner::Walk);
    maintain(30'101, Owner::Loot);
    maintain(45'101, Owner::Loot);
    maintain(45'102, Owner::Walk);
    maintain(104'102, Owner::Walk);
    Check(state.Reason() == Overdue::None, "bounded loot leaves the preceding 30 seconds of walking charged");
    maintain(105'102, Owner::Walk);
    Check(state.Reason() == Overdue::Walk, "loot defer without a normal close preserves the no-progress balance");

    state = {};
    walk.seconds = 1000.0f;
    trip.legs = {walk};
    TravelLegs::StartLeg(trip, 0, 100, nodes);
    maintain(100, Owner::Walk);
    ContinueWalk(state, read, move, onResume);
    auto const tripBound = TravelLegs::TripDeadlineMs(100.0f);
    for (uint32_t step = 1'000; step <= tripBound; step += 1'000)
    {
        motion.Yards += 6.0f;
        maintain(100 + step, Owner::Walk);
    }
    Check(state.Reason() == Overdue::None, "active route stays eligible at its exact estimate bound");
    motion.Yards += 6.0f;
    maintain(1100 + tripBound, Owner::Walk);
    Check(state.Reason() == Overdue::RouteDeadline, "continuous progress still respects the outer active route bound");

    state = {};
    trip.legs[0].type = LegType::Transport;
    TravelLegs::StartLeg(trip, 0, 100, nodes);
    trip.boarding = TravelLegs::Boarding::Wait;
    auto const bound = TravelLegs::TransportWaitMs(trip, trip.legs[0]);
    maintain(100, Owner::Exempt);
    maintain(100 + bound, Owner::Exempt);
    Check(state.Reason() == Overdue::None && state.RouteMs() == 0, "valid transport wait is exempt from walk budgets");
    maintain(1100 + bound, Owner::Exempt);
    Check(state.Reason() == Overdue::Due && ScheduledWork(true, state.Reason(), true, true) == Work::Recover,
          "overdue transport remains independently bounded and takes precedence over loot");

    state = {};
    trip = {};
    Check(TravelLegs::BeginOffFirst(trip, 1, CAR, 120'000, 100), "off-first owns its own bounded wait");
    maintain(100, Owner::Exempt);
    maintain(120'100, Owner::Exempt);
    Check(state.Reason() == Overdue::None, "off-first cannot be prematurely recovered as a stalled walk");
    maintain(121'100, Owner::Exempt);
    Check(state.Reason() == Overdue::Due, "overdue off-first is available to the scheduled follower");
}
void CheckSuccessorAndPartialPaths()
{
    using namespace TravelProgress;
    State state;
    TravelLegs::Trip trip;
    TravelLegs::RouteWait wait;
    TravelProgress::Motion motion{1, true, 0.0f, true, 101, {MAP, 296.0f, 0.0f, 0.0f}};
    uint32_t now = 100;
    auto read = [&] { return motion; };
    auto sample = [&](Owner owner = Owner::Walk)
    { return Maintain(state, {now, owner, owner == Owner::Combat, 0}, trip, wait, 1000.0f, read); };
    auto resumed = [](char const*) {};
    sample();
    state.CommitMotion(motion);
    motion.Yards = 20.0f;
    now += 1000;
    Check(sample().Yards == 20.0f, "first owned arc earns credit");

    // Core speed propagation changes the ID before the action; MoveFarTo keeps the active path.
    motion.Id = 2;
    motion.Yards = 0.0f;
    bool const used = ContinueWalk(state, read, [] { return false; }, resumed);
    Check(!used && state.Owns(2), "waiting callback may return false yet reacquires the same core point generator");
    motion.Yards = 6.0f;
    now += 1000;
    Check(sample().Yards == 6.0f, "speed-relaunched spline earns actual forward distance");
    motion.Id = 3;
    motion.Yards = 0.0f;
    now += 1000;
    Check(sample().Yards == 0.0f && state.Owns(3), "pre-gate observer also reacquires a genuine core successor");
    motion.Yards = 6.0f;
    now += 1000;
    Check(sample().Yards == 6.0f, "observer-reacquired successor credits movement");

    for (unsigned kind = 0; kind < 4; ++kind)
    {
        State foreign = state;
        auto replacement = motion;
        ++replacement.Id;
        if (kind == 0)
            ++replacement.Generator;
        if (kind == 1)
            replacement.Generator = 0;  // non-point, local-frame, falling, taxi or vehicle
        if (kind == 2)
            replacement.Destination.x += 10.0f;
        if (kind == 3)
            ++replacement.Destination.mapId;
        Check(foreign.ObserveMotion(replacement) == 0.0f && !foreign.HasMotion(),
              "a foreign generator, type/frame, destination or map revokes old authority");
        Check(!ContinueWalk(
                  foreign, [&] { return replacement; }, [] { return true; }, resumed) ||
                  !foreign.HasMotion(),
              "active-path callback cannot acquire a foreign unchanged spline");
    }

    State reset = state;
    reset.Suspend();
    ContinueWalk(reset, read, [] { return true; }, resumed);
    Check(!reset.HasMotion(), "explicit reset revokes a retained generator token until a new walk is issued");

    for (float length : {296.0f, 296.0f, 70.0f, 35.0f, 70.0f, 35.0f})
    {
        ContinueWalk(
            state, read,
            [&]
            {
                ++motion.Id;
                ++motion.Generator;
                motion.Destination.x += length;
                motion.Yards = 0.0f;
                return true;
            },
            resumed);
        for (float walked = 5.0f; walked <= length; walked += 5.0f)
        {
            motion.Yards = walked;
            now += 1000;
            Check(sample().Yards >= 5.0f && state.Reason() == Overdue::None,
                  "successive capped paths and short stepping stones credit their own observed arc");
        }
    }
    Check(now > WALK_BUDGET_MS && state.Reason() == Overdue::None,
          "valid chunks to one waypoint last over ninety eligible seconds without false recovery");

    for (Owner owner : {Owner::Combat, Owner::Loot, Owner::Control})
    {
        ++now;
        sample(owner);
        motion.Yards += 20.0f;
        now += 20000;
        sample(owner);
        ++now;
        Check(sample().Yards == 0.0f, "interruption displacement earns no distance");
        ContinueWalk(
            state, read,
            [&]
            {
                ++motion.Id;
                motion.Yards = 0.0f;
                return true;
            },
            resumed);
        motion.Yards = 5.0f;
        now += 1000;
        Check(sample().Yards == 5.0f, "resumed chunk credits new walking after combat loot or control");
    }

    for (unsigned second = 0; second < 90; ++second)
    {
        ContinueWalk(
            state, read,
            [&]
            {
                ++motion.Id;
                motion.Yards = 0.0f;
                return true;
            },
            resumed);
        now += 1000;
        Check(sample().Yards == 0.0f, "reissuing without actual movement does not reset the walking balance");
    }
    Check(state.Reason() == Overdue::Walk, "repeated stationary reissue still expires after ninety eligible seconds");
}

// Core observations are injected from geometric paths; no shared remaining-route coordinate is assumed.
struct GeometricWalk
{
    std::vector<TravelRoute::Point> Path;
    TravelRoute::Point Position{};
    TravelProgress::Motion Motion{};
    uint32_t ElapsedMs{0};
    static constexpr float Speed = 7.0f;

    float Length() const
    {
        float length = 0.0f;
        for (std::size_t i = 1; i < Path.size(); ++i)
            length += TravelRoute::Distance(Path[i - 1], Path[i]);
        return length;
    }

    float Arc() const { return std::min(Length(), Speed * static_cast<float>(ElapsedMs) / 1000.0f); }

    TravelRoute::Point At(float yards) const
    {
        for (std::size_t i = 1; i < Path.size(); ++i)
        {
            auto const& from = Path[i - 1];
            auto const& to = Path[i];
            float const length = TravelRoute::Distance(from, to);
            if (yards <= length)
            {
                float const fraction = length > 0.0f ? yards / length : 0.0f;
                return {from.mapId, from.x + fraction * (to.x - from.x), from.y + fraction * (to.y - from.y),
                        from.z + fraction * (to.z - from.z)};
            }
            yards -= length;
        }
        return Path.back();
    }

    void Issue(std::vector<TravelRoute::Point> path)
    {
        Path = std::move(path);
        Position = Path.front();
        ElapsedMs = 0;
        ++Motion.Id;
        ++Motion.Generator;
        Motion.Destination = Path.back();
    }

    TravelProgress::Motion Read() const
    {
        auto motion = Motion;
        motion.Yards = Arc();
        motion.Active = motion.Yards < Length();
        motion.PositionMatches =
            TravelRoute::Distance(Position, At(motion.Yards)) <= TravelProgress::SPLINE_POSITION_TOLERANCE_YARDS;
        return motion;
    }

    void Advance(uint32_t ms)
    {
        ElapsedMs += ms;
        Position = At(Arc());
    }
};

void CheckGeometricDetoursAndBounds()
{
    using namespace TravelProgress;
    using TravelRoute::Point;
    Point const waypoint{MAP, 500.0f, 0.0f, 0.0f};
    State state;
    TravelLegs::Trip trip;
    TravelLegs::RouteWait wait;
    TravelRoute::Leg leg;
    leg.type = TravelRoute::LegType::Walk;
    leg.from = {MAP, 0.0f, 0.0f, 0.0f};
    leg.to = waypoint;
    leg.seconds = 1000.0f;
    trip.legs = {leg};
    float routeSeconds = 1000.0f;
    uint32_t now = 100;
    GeometricWalk walk;
    walk.Issue({{MAP, -70.0f, 0.0f, 0.0f}, leg.from});
    auto read = [&] { return walk.Read(); };
    auto sample = [&](Owner owner = Owner::Walk)
    { return Maintain(state, {now, owner, owner == Owner::Combat, 0}, trip, wait, routeSeconds, read); };
    auto issue = [&](std::vector<Point> path)
    {
        return ContinueWalk(
            state, read,
            [&]
            {
                walk.Issue(std::move(path));
                return true;
            },
            [](char const*) {});
    };
    sample();
    state.CommitMotion(read());
    walk.Advance(10'000);
    now += 10'000;
    Check(sample().Yards == 70.0f && TravelRoute::Distance(walk.Position, waypoint) == 500.0f,
          "previous geometric partial path ends five hundred yards from the waypoint");
    now += 60'000;
    Check(sample().Yards == 0.0f && state.Reason() == Overdue::None,
          "sixty seconds of prior eligible nonprogress precedes the new detour");

    // Two 148-yard sides, net ten yards closer: old extent would jump from 500 to 786 yards.
    float const height = std::sqrt(148.0f * 148.0f - 25.0f);
    Point const corner{MAP, 5.0f, height, 0.0f};
    Point const end{MAP, 10.0f, 0.0f, 0.0f};
    Check(issue({leg.from, corner, end}) && std::abs(walk.Length() - 296.0f) < 0.01f &&
              TravelRoute::Distance(end, waypoint) == 490.0f &&
              TravelRoute::Distance(end, waypoint) + 5.0f < TravelRoute::Distance(leg.from, waypoint),
          "real detour geometry satisfies the production primary endpoint gate");
    State issueOnly = state;
    issueOnly.Observe(now + 30'000, Owner::Walk, false, 0);
    issueOnly.CheckWalkBudget();
    Check(issueOnly.Reason() == Overdue::Walk, "committing the detour alone preserves all sixty prior seconds");
    float credited = 0.0f;
    for (unsigned second = 0; second < 30; ++second)
    {
        walk.Advance(1000);
        now += 1000;
        credited += sample().Yards;
        Check(state.Reason() == Overdue::None, "real movement on the new detour avoids a false walking timeout");
    }
    Check(std::abs(credited - 210.0f) < 0.01f && walk.Read().PositionMatches,
          "all two hundred ten yards of the first thirty seconds count despite the changed detour extent");
    while (walk.Read().Active)
    {
        walk.Advance(1000);
        now += 1000;
        sample();
    }

    for (float length : {296.0f, 70.0f, 35.0f, 296.0f})
    {
        Point const from = walk.Position;
        Point const to{MAP, from.x + 10.0f, 0.0f, 0.0f};
        Point const bend{MAP, from.x + 5.0f, -std::sqrt(length * length / 4.0f - 25.0f), 0.0f};
        Check(issue({from, bend, to}) && std::abs(walk.Length() - length) < 0.01f &&
                  TravelRoute::Distance(to, waypoint) + 5.0f < TravelRoute::Distance(from, waypoint),
              "subsequent chunks use their own real arcs and endpoint geometry");
        while (walk.Read().Active)
        {
            walk.Advance(1000);
            now += 1000;
            auto const seen = sample();
            Check((seen.Yards >= ADVANCE_YARDS || !walk.Read().Active) && state.Reason() == Overdue::None,
                  "each geometric chunk earns observed movement without a synthetic coordinate invariant");
        }
    }

    for (Owner owner : {Owner::Combat, Owner::Control, Owner::Loot})
    {
        state = {};
        now += 1000;
        sample();
        issue({leg.from, corner, end});
        now += 30'000;
        sample();
        ++now;
        sample(owner);
        walk.Advance(20'000);
        now += 20'000;
        sample(owner);
        now += 1000;
        Check(sample().Yards == 0.0f, "geometric interruption displacement is rebased without credit");
        State resumedWithoutMovement = state;
        resumedWithoutMovement.Observe(now + 60'000, Owner::Walk, false, 0);
        resumedWithoutMovement.CheckWalkBudget();
        Check(resumedWithoutMovement.Reason() == Overdue::Walk,
              "resume retains the thirty seconds charged before interruption");
        walk.Advance(1000);
        now += 1000;
        Check(sample().Yards == 7.0f && state.Reason() == Overdue::None,
              "only new owned movement after resume earns distance");
    }

    state = {};
    now += 1000;
    sample();
    issue({leg.from, corner, end});
    for (unsigned second = 0; second < 90; ++second)
    {
        walk.ElapsedMs += 1000;  // Core time advances, but the observed bot stays at the start.
        now += 1000;
        Check(sample().Yards == 0.0f, "spline time without matching actual geometric movement earns nothing");
    }
    Check(state.Reason() == Overdue::Walk, "stationary world position exhausts the no-motion clock");

    // Repeated ground is movement, but neither new splines nor movement reset either active deadline.
    for (bool routeFirst : {false, true})
    {
        state = {};
        trip.legs[0].seconds = routeFirst ? 1000.0f : 100.0f;
        routeSeconds = routeFirst ? 100.0f : 1000.0f;
        uint32_t const bound =
            routeFirst ? TravelLegs::TripDeadlineMs(routeSeconds) : TravelLegs::LegTimeoutMs(trip.legs[0]);
        uint32_t const started = now += 1000;
        sample();
        issue({leg.from, {MAP, 0.0f, 140.0f, 0.0f}, {MAP, 7.0f, 140.0f, 0.0f}, {MAP, 7.0f, 0.0f, 0.0f}});
        float endpointX = 0.0f;
        while (now - started <= bound)
        {
            if (!walk.Read().Active)
            {
                Point const from = walk.Position;
                issue(
                    {from, {MAP, from.x, 140.0f, 0.0f}, {MAP, endpointX, 140.0f, 0.0f}, {MAP, endpointX, 0.0f, 0.0f}});
                endpointX = 7.0f - endpointX;
            }
            walk.Advance(1000);
            now += 1000;
            Check(sample().Yards >= ADVANCE_YARDS, "observed owned travel over repeated ground still counts");
            if (now - started <= bound)
                Check(state.Reason() == Overdue::None, "oscillation remains eligible through the exact active bound");
        }
        Overdue const expected = routeFirst ? Overdue::RouteDeadline : Overdue::Due;
        Check(state.Reason() == expected && state.RouteMs() == now - started && state.LegMs() == now - started &&
                  ScheduledWork(true, state.Reason(), false, false) == Work::Recover && state.TakeReason() == expected,
              "actual leg or route expiry ends repeated-ground motion through scheduled recovery");
    }
}

void CheckResponseWorkProgress()
{
    using namespace TravelProgress;
    for (unsigned work : {0u, 1u, 2u})
    {
        State progress;
        TravelLegs::Trip trip;
        TravelLegs::RouteWait wait;
        LootAdapter loot;
        loot.Moving = false;
        loot.Mounted = false;
        auto maintain = [&](uint32_t now, Owner owner)
        {
            return Maintain(progress, {now, owner, false, loot.Completed}, trip, wait, 1000.0f,
                            [] { return TravelProgress::Motion{}; });
        };
        maintain(100, Owner::Walk);
        maintain(30100, Owner::Walk);
        Check(LootInteraction::Execute(loot) == LootInteraction::Attempt::Requested,
              "response test issues a real shared request transaction");
        loot.Window = loot.State.Guid;
        Check(loot.State.Open(loot.Window, 200), "matching core response opens release ownership");
        maintain(30101, Owner::Loot);
        loot.Response(work == 1 ? 10 : 0, work == 2 ? 1 : 0, true);
        loot.ObserveClose();
        Check(!loot.Completed && loot.State.Guid && !loot.MayMount() && loot.Releases == 1,
              "response disposition queues release but neither closes nor credits it");
        Check((loot.Deferred != 0) == (work != 2), "no-storable gameobject keeps existing retry deferral");
        loot.Window = 0;  // core processes queued work and matching release
        loot.ObserveClose();
        auto closed = maintain(31101, Owner::Walk);
        Check(loot.Completed == (work != 0) && closed.LootCompleted == (work != 0) && !loot.State.Guid &&
                  !loot.CacheValid && loot.MayMount() && loot.CloseDetail == (work ? "closed" : "deferred"),
              "only response work with matched close publishes completion; empty/unstoreable deferral keeps balance");
        loot.ObserveClose();
        Check(loot.Completed == (work != 0), "duplicate core close cannot credit again");
        maintain(91101, Owner::Walk);
        Check(progress.Reason() == (work ? Overdue::None : Overdue::Walk),
              "no-work deferred close preserves thirty prior seconds; real money/items reset the balance");
    }
}

// The exit cases of WT44b, through Maintain and the follower's StepLeg: one expiry (NextDeadlineMs), one leg clock
void CheckOneExpiry()
{
    using namespace TravelProgress;
    using TravelLegs::LegType;
    auto const nodes = [](uint32_t) { return TravelRoute::Point{}; };
    TravelRoute::Point const start{MAP, 0.0f, 0.0f, 0.0f};
    TravelRoute::Point const goal{MAP, 1000.0f, 0.0f, 0.0f};
    TravelRoute::Leg leg;
    leg.from = start;
    leg.to = goal;
    leg.seconds = 100.0f;

    State state;
    TravelLegs::Trip trip;
    TravelLegs::RouteWait wait;
    auto const maintain = [&](uint32_t now, Owner owner)
    {
        Maintain(state, {now, owner, owner == Owner::Combat, 0}, trip, wait, 100.0f,
                 [] { return TravelProgress::Motion{}; });
    };
    auto const step = [&](uint32_t now, TravelRoute::Point const& at, uint64_t aboard = 0)
    {
        TravelLegs::Observation seen;
        seen.bot = at;
        seen.nowMs = now;
        seen.aboard = static_cast<uint32_t>(aboard);
        seen.aboardGuid = aboard;
        seen.legMs = state.LegMs();
        return TravelLegs::StepLeg(trip, seen);
    };

    // A fight on a walk does not expire the leg: the walk budget, the leg and the route clock pause and resume at the
    // same balance; the first sample after the fight adds nothing for the span it ends
    leg.type = LegType::Walk;
    trip.legs = {leg};
    TravelLegs::StartLeg(trip, 0, 100, nodes);
    maintain(100, Owner::Walk);
    maintain(30'100, Owner::Walk);
    maintain(30'101, Owner::Combat);
    uint32_t const fight = TravelLegs::LegTimeoutMs(leg) + WALK_BUDGET_MS;
    maintain(30'101 + fight, Owner::Combat);
    maintain(30'102 + fight, Owner::Walk);
    Check(state.Reason() == Overdue::None && state.LegMs() == 30'000 && state.WalkMs() == 30'000 &&
              state.RouteMs() == 30'000 && step(30'102 + fight, start).act != TravelLegs::Act::Fail,
          "a fight on a walk does not expire the leg: no reason, the clocks unchanged by the fight");
    maintain(30'102 + fight + 59'000, Owner::Walk);
    Check(state.Reason() == Overdue::None, "after the fight the walk budget resumes below its bound");
    maintain(30'102 + fight + 60'000, Owner::Walk);
    Check(state.Reason() == Overdue::Walk && state.WalkMs() == WALK_BUDGET_MS,
          "the walk budget expires at the balance it had before the fight plus the walk after it");

    // A ride is bounded by the vessel's deadline: RIDE_PERIODS of the read period from the boarding, not the leg's
    // fallback timeout; the guard is due and the follower fails the leg at the same tick, neither before
    state = {};
    leg.type = LegType::Transport;
    trip = {};
    trip.legs = {leg};
    TravelLegs::StartLeg(trip, 0, 100, nodes);
    trip.boarding = TravelLegs::Boarding::Ride;
    trip.passengerGuid = CAR;
    trip.boardedMs = 10'000;
    trip.periodMs = 300'000;
    uint32_t const ride = TravelLegs::TransportRideMs(trip, trip.legs[0]);
    Check(ride == 660'000 && ride != TravelLegs::LegTimeoutMs(trip.legs[0]),
          "the ride's bound is the read period's, not the leg's fallback");
    TravelRoute::Point const sea{MAP, 500.0f, 0.0f, 0.0f};
    maintain(10'000, Owner::Exempt);
    maintain(10'000 + ride - 999, Owner::Exempt);
    Check(state.Reason() == Overdue::None && step(10'000 + ride, sea, CAR).act != TravelLegs::Act::Fail,
          "a ride within its bound: nothing due, the follower rides on");
    maintain(10'001 + ride, Owner::Exempt);
    TravelLegs::Decision const late = step(10'001 + ride, sea, CAR);
    Check(state.Reason() == Overdue::Due && late.act == TravelLegs::Act::Fail &&
              late.reason == TravelLegs::FailReason::Timeout,
          "a ride is bounded by the vessel's deadline: the guard and the follower agree at one ms past it");

    // A late route answer is bounded once per request: the guard wakes the follower once, the follower asks again
    // (a new request, its own keep), MAX_RESUBMITS times; a pending answer adds nothing to the route clock
    state = {};
    trip = {};
    uint32_t now = 1'000;
    wait = {17, MAP, now, true, start};
    maintain(now, Owner::Exempt);
    unsigned wakes = 0;
    bool refused = false;
    while (!refused && now < 1'000 + 10 * TravelRoute::ANSWER_KEEP_MS)
    {
        now += 1'000;
        maintain(now, Owner::Exempt);
        if (state.Reason() == Overdue::None)
            continue;
        ++wakes;
        Check(state.TakeReason() == Overdue::Due && now - wait.requestedMs == TravelRoute::ANSWER_KEEP_MS + 1'000,
              "a late route answer wakes the follower at its first sample past the answer's keep");
        if (TravelLegs::Resubmit(trip))
            wait = {wait.requestId + 1, MAP, now, true, start};
        else
            refused = true;
    }
    Check(wakes == TravelLegs::MAX_RESUBMITS + 1u && refused && state.RouteMs() == 0,
          "a late route answer is bounded once: one wake per request, MAX_RESUBMITS asked again, then NoAnswer");
}

// WT64b: the walk clock runs from the follower's first step, and not while a taxi carries a bot whose trip has no leg
void CheckOwner()
{
    using namespace TravelProgress;
    OwnerFacts begun;  // alive, free to move, no leg, never stepped
    Check(OwnerOf(begun) == Owner::Exempt, "a trip the follower has not stepped has no owner: no clock runs");
    OwnerFacts stepped = begun;
    stepped.stepped = true;
    Check(OwnerOf(stepped) == Owner::Walk, "a stepped trip with no leg is a walk (the inn's last yards, a replan)");
    OwnerFacts carried = stepped;
    carried.carried = true;
    Check(OwnerOf(carried) == Owner::Exempt, "a taxi carries a bot whose trip has no leg: no clock runs");
    carried.hasLeg = true;
    Check(OwnerOf(carried) == Owner::Leg, "a trip's own leg keeps its clock on a taxi");
    OwnerFacts held = begun;
    held.canMove = false;
    Check(OwnerOf(held) == Owner::Exempt, "an unstepped trip is not paused as control either");
    held.stepped = true;
    Check(OwnerOf(held) == Owner::Control, "a stepped trip of a bot that cannot move is under control");
    OwnerFacts leg = stepped;
    leg.hasLeg = true;
    Check(OwnerOf(leg) == Owner::Leg, "a leg that is no walk is the leg's");
    leg.walks = true;
    Check(OwnerOf(leg) == Owner::Walk, "a walk leg is the walk's");
    leg.loot = true;
    Check(OwnerOf(leg) == Owner::Loot, "an open loot interaction owns the walk");
    leg.external = true;
    Check(OwnerOf(leg) == Owner::Exempt, "a ride or a route wait owns nothing");
    leg.combat = true;
    Check(OwnerOf(leg) == Owner::Combat, "combat comes first");
    OwnerFacts dead = stepped;
    dead.alive = false;
    Check(OwnerOf(dead) == Owner::Exempt, "a dead bot's trip has no owner");

    // The clock itself: held for 200 s before the first step, the budget is untouched; it expires 90 s after it
    auto const still = [] { return TravelProgress::Motion{}; };
    State state;
    TravelLegs::Trip trip;
    TravelLegs::RouteWait wait;
    auto const maintain = [&](uint32_t now, OwnerFacts const& facts)
    { return Maintain(state, {now, OwnerOf(facts), facts.combat, 0}, trip, wait, 0.0f, still); };
    for (uint32_t now = 1000; now <= 201'000; now += 1000)
        maintain(now, begun);
    Check(state.WalkMs() == 0 && state.Reason() != Overdue::Walk,
          "the walk budget does not run before the follower's first step");
    for (uint32_t now = 202'000; now <= 202'000 + WALK_BUDGET_MS; now += 1000)
        maintain(now, stepped);
    Check(state.WalkMs() >= WALK_BUDGET_MS - 1000, "the walk budget runs from the follower's first step");

    // A walk with no trip (GoTo's local piece) has the same budget and no deadline
    State local;
    bool sampled = false;
    for (uint32_t now = 1000; now < 1000 + WALK_BUDGET_MS; now += 1000)
        TravelProgress::MaintainWalk(local, {now, Owner::Walk, false, 0}, still, sampled);
    Check(sampled && local.Reason() == Overdue::None, "a local piece is not overdue inside the walk budget");
    TravelProgress::MaintainWalk(local, {1000 + WALK_BUDGET_MS, Owner::Walk, false, 0}, still, sampled);
    Check(local.Reason() == Overdue::Walk, "a local piece without advance is overdue at the walk budget");
    State paused;
    for (uint32_t now = 1000; now <= 1000 + 2 * WALK_BUDGET_MS; now += 1000)
        TravelProgress::MaintainWalk(paused, {now, Owner::Control, false, 0}, still, sampled);
    Check(paused.Reason() == Overdue::None && paused.WalkMs() == 0, "a local piece under control spends no budget");
}

}  // namespace

int main()
{
    CheckOwner();
    CheckCadence();
    CheckBoard();
    CheckRideAndExit();
    CheckAttachGuard();
    CheckDiscardedOwner();
    CheckIneligibleMaintenance();
    CheckUnconsumedBoardTransfer();
    CheckHeldTrip();
    CheckFollowAndNoTrip();
    CheckBoardingByIntent();
    CheckFollowerByMaster();
    CheckMountPolicy();
    CheckLootScheduler();
    CheckLootLeftForRoom();
    CheckProgressScheduler();
    CheckSuccessorAndPartialPaths();
    CheckGeometricDetoursAndBounds();
    CheckResponseWorkProgress();
    CheckOneExpiry();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
