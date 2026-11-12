/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <iostream>
#include <limits>
#include <string_view>
#include <variant>
#include <vector>

#include "LootInteraction.h"
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
constexpr Point ONWARD{11.0f, 6.0f, 8.0f};
constexpr Footprint HULL{{0.0f, 3.0f, 7.0f}, {6.0f, 9.0f, 10.0f}, 0.6f};
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

struct GroundGeometry
{
    bool Ground = true;
    bool Corridor = true;
    bool Crossing = true;

    bool OnGround(Point const&) const { return Ground; }
    bool ConnectedGround(Point const&, Point const&) const { return Corridor; }
    bool CrossingClear(Point const&, Point const&) const { return Crossing; }
};

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
    GroundGeometry Geometry;
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

    bool ConfirmExit(PassengerMotion const& motion)
    {
        Calls.push_back("confirm_exit");
        return AcceptExit(Geometry, HULL, motion.ExitFrom, Spline.Position, motion.Onward);
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
    motion.ExitFrom = DECK;
    motion.Onward = ONWARD;
    return motion;
}

void CheckCadence()
{
    Cadence cadence;
    Adapter adapter;
    adapter.ExpectedContact = Contact::OffSurface;
    for (std::uint32_t const time : {0, 400, 999, 1000, 1500, 4300, 4301})
        Maintain(cadence, time, MAP, nullptr, adapter);
    Check(adapter.Probes == 3, "the pre-action path samples at the first tick, at one second and once after a gap");
    Check(adapter.LastGapMs == 3300, "the late update reports the elapsed gap rather than issuing catch-up probes");

    std::uint32_t constexpr BEFORE_WRAP = std::numeric_limits<std::uint32_t>::max() - 499;
    cadence = {};
    adapter = {};
    adapter.ExpectedContact = Contact::OffSurface;
    Maintain(cadence, BEFORE_WRAP, MAP, nullptr, adapter);
    Maintain(cadence, 499, MAP, nullptr, adapter);
    Check(adapter.Probes == 1, "999 ms over clock wrap is not yet due");
    Maintain(cadence, 500, MAP, nullptr, adapter);
    Check(adapter.Probes == 2 && adapter.LastGapMs == 1000, "1000 ms over clock wrap is due");
    Maintain(cadence, 501, MAP + 1, nullptr, adapter);
    Check(adapter.Probes == 3, "the first destination map tick revalidates before the normal cadence");
    cadence.Revalidate = true;
    Maintain(cadence, 502, MAP + 1, nullptr, adapter);
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

    cadence = {};
    adapter = {};
    motion = Motion(MotionKind::Board, DECK);
    motion.ContactOnly = true;
    adapter.Spline.Position = DECK;
    adapter.Spline.Finalized = true;
    adapter.Spline.Id += 1;
    Maintain(cadence, 30, MAP, &motion, adapter);
    Check(motion.Result == Outcome::Boarded && adapter.Aboard.Guid == CAR,
          "the counted hop acknowledges only after expected feet contact and coherent attachment");
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
    adapter.Calls.clear();
    Maintain(cadence, 3010, MAP, nullptr, adapter);
    Check(!adapter.Aboard.Guid, "verified off-deck observation detaches without a route");
    Check(adapter.Calls == std::vector<std::string_view>{"read", "probe", "settle", "detach", "read", "publish"},
          "local spline is settled before unexpected detach");

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
    adapter.Geometry.Corridor = false;
    motion = Motion(MotionKind::Exit, PLATFORM);
    Maintain(cadence, 20, MAP, &motion, adapter);
    Check(!adapter.Aboard.Guid && motion.Result == Outcome::Lost, "off-deck without accepted ground is a loss");

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

void CheckGeometry()
{
    GroundGeometry geometry;
    Check(!AcceptExit(geometry, HULL, DECK, {5.5f, 6.0f, 7.6f}, ONWARD),
          "NAV_GROUND beneath the tram is inside the padded model footprint");
    Check(!AcceptExit(geometry, HULL, DECK, {6.5f, 6.0f, 8.0f}, ONWARD),
          "ground outside the car still needs body clearance");
    Check(!AcceptExit(geometry, HULL, DECK, {7.0f, 6.0f, 5.9f}, ONWARD),
          "outside ground below the dock floor is rejected");
    geometry.Ground = false;
    Check(!AcceptExit(geometry, HULL, DECK, PLATFORM, ONWARD), "unknown or water mesh cannot stand in for ground");
    geometry.Ground = true;
    geometry.Corridor = false;
    Check(!AcceptExit(geometry, HULL, DECK, PLATFORM, ONWARD),
          "a shortcut or partial route without a real ground corridor cannot authorize exit");
    geometry.Corridor = true;
    geometry.Crossing = false;
    Check(!AcceptExit(geometry, HULL, DECK, PLATFORM, ONWARD), "a blocked deck crossing rejects reachable ground");
    geometry.Crossing = true;
    Check(AcceptExit(geometry, HULL, DECK, PLATFORM, ONWARD), "clear same-floor ground leads into the platform");
}

void CheckQuay()
{
    struct BoardingGeometry
    {
        Point Upper{2064.599f, 235.264f, 115.757f};
        Point Lower{2056.750f, 236.750f, 99.767f};
        mutable int UpperCorridorChecks = 0;
        bool LowerAvailable = true;

        std::optional<Point> ShoreAt(Point const& seed) const
        {
            if (std::hypot(seed.X - Upper.X, seed.Y - Upper.Y) <= 0.35f)
                return Upper;
            if (LowerAvailable && DistanceSquared(seed, Lower) <= 1.0f)
                return Lower;
            return std::nullopt;
        }

        std::optional<Point> DeckFrom(Point const& shore) const
        {
            float const floor = shore.Z == Lower.Z ? 101.220f : 115.757f;
            return Point{2062.599f, 235.264f, floor};
        }

        bool OnGround(Point const&) const { return true; }
        bool CrossingClear(Point const&, Point const&) const { return true; }
        bool ConnectedGround(Point const&, Point const& shore) const
        {
            if (shore.Z == Lower.Z)
                return true;
            ++UpperCorridorChecks;
            return false;
        }
    } geometry;

    Point const approach{2061.870f, 247.465f, 100.636f};
    Point const origin{2062.599f, 235.264f, 117.734f};
    auto const quay = FindQuay(geometry, approach, origin);
    Check(quay && quay->Z == geometry.Lower.Z, "an origin above the deck does not exclude the reachable lower floor");
    Check(geometry.UpperCorridorChecks > 0, "the upper boarding candidate fails the normal ground corridor");
    geometry.LowerAvailable = false;
    Check(!FindQuay(geometry, approach, origin), "the disconnected upper candidate alone cannot choose a quay");
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
                  adapter.Calls == std::vector<std::string_view>{"read", "probe"},
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
    struct Info
    {
        struct Travel
        {
            TravelLegs::Trip trip;
            std::optional<PassengerMotion> passengerMotion;
        };
        std::variant<std::monostate, Travel> data;
        std::uint32_t routeRequests = 19;
        std::uint32_t startT = 10;
    } info;
    info.data = Info::Travel{};
    auto& before = std::get<Info::Travel>(info.data);
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
    info = TravelLegs::AfterReset<Info::Travel>(info, true);
    auto& after = std::get<Info::Travel>(info.data);
    Check(after.passengerMotion && after.passengerMotion->Result == Outcome::Boarded && info.routeRequests == 19,
          "the production far reset policy preserves the unconsumed boarding result with the continuing trip");
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

void CheckFollowAndNoTrip()
{
    Adapter adapter;
    Sample sample = Reconcile(adapter, nullptr, MotionState::None, CAR);
    Check(!sample.Before.Guid && sample.After.Guid == CAR, "master attach uses the same exact contact and frame seam");
    adapter.ExpectedContact = Contact::OffSurface;
    sample = Reconcile(adapter, nullptr, MotionState::None, NEARER_CAR);
    Check(sample.After.Guid == CAR, "master attach request cannot detach a different current ride");

    Cadence cadence;
    adapter = {};
    Maintain(cadence, 0, MAP, nullptr, adapter);
    Check(adapter.Aboard.Guid == NEARER_CAR && adapter.LastExpected == 0,
          "no trip and no master can attach from observed feet contact");
}

struct LootAdapter
{
    LootInteraction::Transaction State;
    uint32_t Now = 100;
    uint64_t Selected = 12;
    uint64_t Window = 0;
    uint64_t Deferred = 0;
    bool Valid = true;
    bool Moving = true;
    bool Mounted = true;
    bool CanRequest = true;
    bool CacheValid = true;
    unsigned Requests = 0;
    unsigned Releases = 0;
    uint32_t Completed = 0;
    std::string_view CloseDetail;

    void Response(uint32_t gold, uint32_t stored, bool gameObject)
    {
        LootInteraction::CompleteResponse(
            State, Window, gold, stored, gameObject, [&] { Deferred = Window; }, [] {}, [&] { ++Releases; });
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

    bool Admit() { return Valid && Deferred != Selected && State.Prepare(Selected, Now); }
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
    bool MayMount() const { return LootInteraction::MountAllowed(State.Guid != 0, Window != 0); }
};

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
            Check(state.Reason() == Overdue::RouteAnswer &&
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
    Check(state.Reason() == Overdue::Transport && ScheduledWork(true, state.Reason(), true, true) == Work::Recover,
          "overdue transport remains independently bounded and takes precedence over loot");

    state = {};
    trip = {};
    Check(TravelLegs::BeginOffFirst(trip, 1, CAR, 120'000, 100), "off-first owns its own bounded wait");
    maintain(100, Owner::Exempt);
    maintain(120'100, Owner::Exempt);
    Check(state.Reason() == Overdue::None, "off-first cannot be prematurely recovered as a stalled walk");
    maintain(121'100, Owner::Exempt);
    Check(state.Reason() == Overdue::Transport, "overdue off-first is available to the scheduled follower");
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
        Overdue const expected = routeFirst ? Overdue::RouteDeadline : Overdue::LegDeadline;
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

}  // namespace

int main()
{
    CheckCadence();
    CheckBoard();
    CheckRideAndExit();
    CheckGeometry();
    CheckQuay();
    CheckDiscardedOwner();
    CheckIneligibleMaintenance();
    CheckUnconsumedBoardTransfer();
    CheckFollowAndNoTrip();
    CheckLootScheduler();
    CheckProgressScheduler();
    CheckSuccessorAndPartialPaths();
    CheckGeometricDetoursAndBounds();
    CheckResponseWorkProgress();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
