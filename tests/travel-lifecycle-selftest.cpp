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
    // The transport's surface as seen from above, padded by the bot's reach (the runtime casts rays at the model)
    Footprint Surface = HULL;

    bool OffHull(Point const& point) const { return Surface.Outside(point); }
    bool OnGround(Point const&) const { return Ground; }
    bool ConnectedGround(Point const&, Point const&) const { return Corridor; }
    bool GroundLeadsOn(Point const&, Point const&) const { return Corridor; }
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
        return AcceptExit(Geometry, motion.ExitFrom, Spline.Position, motion.Onward);
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
    Check(!AcceptExit(geometry, DECK, {5.5f, 6.0f, 7.6f}, ONWARD),
          "NAV_GROUND beneath the tram is inside the padded model footprint");
    Check(!AcceptExit(geometry, DECK, {6.5f, 6.0f, 8.0f}, ONWARD), "ground outside the car still needs body clearance");
    Check(!AcceptExit(geometry, DECK, {7.0f, 6.0f, 5.4f}, ONWARD),
          "outside ground more than an exit's step (2.5 yd) below the deck is rejected");
    geometry.Ground = false;
    Check(!AcceptExit(geometry, DECK, PLATFORM, ONWARD), "unknown or water mesh cannot stand in for ground");
    geometry.Ground = true;
    geometry.Corridor = false;
    Check(!AcceptExit(geometry, DECK, PLATFORM, ONWARD),
          "a shortcut or partial route without a real ground corridor cannot authorize exit");
    geometry.Corridor = true;
    geometry.Crossing = false;
    Check(!AcceptExit(geometry, DECK, PLATFORM, ONWARD), "a blocked deck crossing rejects reachable ground");
    geometry.Crossing = true;
    Check(AcceptExit(geometry, DECK, PLATFORM, ONWARD), "clear same-floor ground leads into the platform");

    // WT43: a ship's model box spans its masts, rigging and bowsprit (Booty Bay: 114 x 83 yd around a deck of about
    // 40 x 10). The quay beside the deck lies inside that box but under no part of the ship: it is off the hull.
    constexpr Footprint SHIP_BOX{{-60.0f, -40.0f, -5.0f}, {55.0f, 43.0f, 40.0f}, 0.6f};
    constexpr Footprint SHIP_DECK{{-20.0f, -5.0f, 0.0f}, {20.0f, 5.0f, 8.0f}, 0.6f};
    constexpr Point SHIP_FROM{0.0f, 3.0f, 6.1f};
    constexpr Point QUAY{0.0f, 8.0f, 6.0f};
    constexpr Point QUAY_ONWARD{0.0f, 14.0f, 6.0f};
    geometry.Surface = SHIP_DECK;
    Check(!SHIP_BOX.Outside(QUAY) && AcceptExit(geometry, SHIP_FROM, QUAY, QUAY_ONWARD),
          "ground beside the deck inside the ship's model box is a valid exit");
    Check(!AcceptExit(geometry, SHIP_FROM, {0.0f, 5.2f, 6.0f}, QUAY_ONWARD),
          "ground within the bot's reach of the deck edge is still on the hull");
    Check(CheckExit(geometry, SHIP_FROM, QUAY, {0.0f, -4.0f, 6.0f}) == CrossingCheck::OnHull,
          "an onward point back on the deck is on the hull");

    // WT43: the crossing's body ray runs level above the higher end. Orgrimmar's tower top (z 53.64) under the
    // zeppelin's deck (z 55.35): a ray rising from 54.29 to 56.0 passed through the deck plane while still over it.
    constexpr Point TOWER{1319.32f, -4658.10f, 53.64f};
    constexpr Point ZEPPELIN_DECK{1318.11f, -4652.0f, 55.35f};
    float const level = CrossingRayHeight(TOWER, ZEPPELIN_DECK);
    Check(std::fabs(level - 56.0f) < 0.001f && level > ZEPPELIN_DECK.Z,
          "a boarding ray onto a higher deck runs above the deck, not into it");
    Check(CrossingRayHeight(ZEPPELIN_DECK, TOWER) == level, "boarding and exit rays of one crossing run at one height");
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

// WT43: the exit is the boarding search in reverse. A ship's deck spans |y| < 5 around its origin; the bot stands
// where it boarded (y = 3), the quay lies on the far side (y <= -7). The straight crossing from the bot crosses the
// far railing; the crossing from the far deck edge is clear.
void CheckExitSearch()
{
    struct ExitGeometry
    {
        Point Bot{0.0f, 3.0f, 6.1f};
        Footprint Deck{{-20.0f, -5.0f, 0.0f}, {20.0f, 5.0f, 8.0f}, 0.6f};
        bool DeckWalk = true;
        mutable float Vertical = 0.0f;
        mutable float Inset = 0.0f;

        static bool OnQuay(Point const& point) { return point.Y <= -7.0f && std::fabs(point.X) <= 20.0f; }
        std::optional<Point> ShoreAt(Point const& seed, float vertical) const
        {
            Vertical = vertical;
            bool const floor = std::fabs(seed.Z - 6.0f) <= vertical;
            return OnQuay(seed) && floor ? std::optional<Point>(Point{seed.X, seed.Y, 6.0f}) : std::nullopt;
        }
        std::optional<Point> SnapGround(Point point, float, float) const
        {
            return OnQuay(point) ? std::optional<Point>(point) : std::nullopt;
        }
        std::optional<Point> DeckFrom(Point const& shore, float inset, Point const&) const
        {
            Inset = inset;
            float const inward = shore.Y < 0.0f ? -1.0f : 1.0f;
            return Point{shore.X, inward, 6.1f};
        }
        bool OffHull(Point const& point) const { return Deck.Outside(point); }
        bool OnGround(Point const& point) const { return OnQuay(point); }
        bool GroundLeadsOn(Point const& start, Point const& end) const { return OnQuay(start) && OnQuay(end); }
        bool CrossingClear(Point const& start, Point const&) const { return DistanceSquared(start, Bot) > 0.01f; }
        std::optional<std::vector<Point>> DeckPath(Point const&, Point const& to, std::uint32_t,
                                                   std::uint32_t& expanded) const
        {
            expanded = 0;
            return DeckWalk ? std::optional<std::vector<Point>>(std::vector<Point>{to}) : std::nullopt;
        }
    } geometry;

    Point const origin{0.0f, 0.0f, 0.0f};
    Point const toward{0.0f, -40.0f, 6.0f};
    Check(!AcceptExit(geometry, geometry.Bot, {0.0f, -8.0f, 6.0f}, {0.0f, -12.0f, 6.0f}),
          "the straight crossing from where the bot boarded is blocked by the far railing");
    auto const exit = FindExit(geometry, geometry.Bot, origin, origin, {toward});
    Check(exit && exit->Deck.Y == -1.0f && exit->Ground.Y <= -7.0f && std::fabs(exit->Ground.X) < 0.01f,
          "the exit crosses the deck to the edge facing the route and steps onto the quay there");
    Check(exit && AcceptExit(geometry, exit->Deck, exit->Ground, exit->Onward),
          "the found exit passes the confirmation from its deck edge");
    Check(geometry.Inset == EXIT_DECK_INSET_YARDS && geometry.Vertical == EXIT_STEP_YARDS,
          "the exit search asks for the deck point 1 yd inside the edge and a quay within a step of the deck");
    Point const edge{0.0f, -1.0f, 6.1f};
    Check(AcceptExit(geometry, edge, {0.0f, -8.0f, 8.14f}, {0.0f, -12.0f, 8.14f}),
          "Booty Bay: a pier 2.04 yd above the deck edge is within an exit's step");
    Check(CheckExit(geometry, edge, {0.0f, -8.0f, 8.7f}, {0.0f, -12.0f, 8.7f}) == CrossingCheck::Height,
          "a quay 2.6 yd above the deck edge is beyond an exit's step");
    Check(exit && geometry.OffHull(exit->Onward) && exit->Onward.Y < exit->Ground.Y,
          "the onward point leads away from the hull");
    Point const nearRoute{3.0f, -14.0f, 6.0f};
    auto const nearExit = FindExit(geometry, geometry.Bot, origin, origin, {nearRoute});
    Check(nearExit && nearExit->Onward.X == nearRoute.X && nearExit->Onward.Y == nearRoute.Y,
          "a near route point off the hull is the onward point");

    geometry.DeckWalk = false;
    CrossingTally tally;
    Check(!FindExit(geometry, geometry.Bot, origin, origin, {toward}, &tally) &&
              tally.Counts[static_cast<std::size_t>(CrossingCheck::DeckWalk)] > 0,
          "a blocked walk across the deck rejects the edge and is counted");
    Check(
        tally.Bearings[0] && tally.Bearings[0]->Check == CrossingCheck::DeckWalk && tally.Bearings[0]->Distance == 7.0f,
        "the probe keeps, for the route's bearing, the nearest candidate that got furthest (the deck walk at 7 yd)");
    geometry.DeckWalk = true;
    Check(!FindExit(geometry, {0.0f, 3.0f, 12.0f}, origin, origin, {toward}),
          "the search looks for the quay at the deck's floor, not a dock floor below it");
}

// WT43c: the walk across the deck goes around what the straight walk cannot cross (a mast on the centreline) within a
// bounded search, and gives up at its cap where no walk exists (a wall across the deck).
void CheckDeckPath()
{
    auto const deckWithin = [](float halfX, float halfY)
    {
        return [halfX, halfY](Point const& top, float drop) -> std::optional<float>
        {
            if (std::fabs(top.X) > halfX || std::fabs(top.Y) > halfY || top.Z < 6.1f || top.Z - 6.1f > drop)
                return std::nullopt;
            return 6.1f;
        };
    };
    auto const mast = [](Point const& a, Point const& b)
    {
        float const dx = b.X - a.X;
        float const dy = b.Y - a.Y;
        float const length2 = dx * dx + dy * dy;
        float const part = length2 > 0.0f ? std::clamp(-(a.X * dx + a.Y * dy) / length2, 0.0f, 1.0f) : 0.0f;
        return std::hypot(a.X + dx * part, a.Y + dy * part) < 1.0f;
    };
    auto const shipDeck = deckWithin(8.0f, 5.0f);
    Point const start{0.0f, 4.0f, 6.1f};
    Point const end{0.0f, -4.0f, 6.1f};
    Check(!FollowDeck(start, end, 0.35f, shipDeck, mast), "the straight walk across the deck runs into the mast");
    std::uint32_t expanded = 0;
    auto const walk = DeckPath(start, end, 0.35f, shipDeck, mast, DECK_PATH_CELLS, expanded);
    bool clear = walk && !walk->empty() && walk->back().X == end.X && walk->back().Y == end.Y;
    Point at = start;
    for (Point const& corner : walk ? *walk : std::vector<Point>{})
    {
        clear = clear && FollowDeck(at, corner, 0.35f, shipDeck, mast);
        at = corner;
    }
    Check(clear, "the deck walk goes around the mast by corners its steps clear and ends at the deck edge");
    Check(walk && walk->size() >= 2 && walk->size() <= 8 && expanded > 0 && expanded <= DECK_PATH_CELLS,
          "the walk around the mast is a few corners, found within the search's cap");
    Check(DeckPath({0.0f, 4.0f, 6.1f}, {3.0f, 4.0f, 6.1f}, 0.35f, shipDeck, mast, DECK_PATH_CELLS, expanded) &&
              expanded == 0,
          "a clear straight walk is taken without a search");

    auto const wall = [](Point const& a, Point const& b)
    { return std::min(a.Y, b.Y) < 0.5f && std::max(a.Y, b.Y) > -0.5f; };
    Check(!DeckPath({0.0f, 5.0f, 6.1f}, {0.0f, -5.0f, 6.1f}, 0.35f, deckWithin(30.0f, 10.0f), wall, DECK_PATH_CELLS,
                    expanded) &&
              expanded == DECK_PATH_CELLS,
          "a wall across the deck ends the search at its cap, without a walk");
}

// WT43c: a round gondola (synthetic; Tirisfal's own shape is CheckTirisfalGondola's). The bot stands on the centre's
// fixture (98.46, within 2 yd), the gondola's floor (97.1)
// reaches 7 yd out; the tower's platform lies 7.5 to 12 yd out at 97.0, at +135 degrees from the route's far point,
// and joins the ground the route goes on to. At +60 degrees lies a ledge of the mesh that leads nowhere (an island).
void CheckGondolaExit()
{
    struct GondolaGeometry
    {
        Point Bot{0.0f, 0.0f, 98.46f};

        static float Radius(Point const& point) { return std::hypot(point.X, point.Y); }
        static bool Sector(Point const& point, float degrees)
        {
            float const bearing = std::atan2(point.Y, point.X) * 57.2957795f;
            return std::fabs(bearing - degrees) <= 8.0f && Radius(point) >= 7.5f && Radius(point) <= 12.0f;
        }
        static bool Platform(Point const& point) { return Sector(point, 135.0f); }
        static bool Island(Point const& point) { return Sector(point, 60.0f); }
        static bool Far(Point const& point) { return point.X >= 50.0f; }
        static std::optional<float> DeckAt(Point const& top, float drop)
        {
            float const z = Radius(top) <= 2.0f && top.Z >= 98.46f ? 98.46f : 97.1f;
            if (Radius(top) > 7.0f || top.Z < z || top.Z - z > drop)
                return std::nullopt;
            return z;
        }
        std::optional<Point> ShoreAt(Point const& seed, float vertical) const
        {
            bool const quay = (Platform(seed) || Island(seed)) && std::fabs(seed.Z - 97.0f) <= vertical;
            return quay ? std::optional<Point>(Point{seed.X, seed.Y, 97.0f}) : std::nullopt;
        }
        std::optional<Point> SnapGround(Point point, float, float) const
        {
            if (Far(point))
                return point;
            if (Platform(point) || Island(point))
                return Point{point.X, point.Y, 97.0f};
            return std::nullopt;
        }
        std::optional<Point> DeckFrom(Point const& shore, float inset, Point const&) const
        {
            float const scale = (7.0f - inset) / Radius(shore);
            return Point{shore.X * scale, shore.Y * scale, 97.1f};
        }
        bool OffHull(Point const& point) const { return Radius(point) > 7.2f; }
        bool OnGround(Point const& point) const { return Platform(point) || Island(point) || Far(point); }
        bool CrossingClear(Point const&, Point const&) const { return true; }
        bool GroundLeadsOn(Point const& start, Point const& end) const { return Platform(start) && Far(end); }
        std::optional<std::vector<Point>> DeckPath(Point const& start, Point const& end, std::uint32_t cells,
                                                   std::uint32_t& expanded) const
        {
            return TravelLifecycle::DeckPath(
                start, end, 0.35f, [](Point const& top, float drop) { return DeckAt(top, drop); },
                [](Point const&, Point const&) { return false; }, cells, expanded);
        }
    } geometry;

    Point const origin{0.0f, 0.0f, 114.97f};
    Point const stop{0.0f, 0.0f, 114.97f};
    Point const far{100.0f, 0.0f, 40.0f};
    CrossingTally tally;
    auto const exit = FindExit(geometry, geometry.Bot, origin, origin, {stop, far}, &tally);
    Check(
        exit && std::fabs(std::atan2(exit->Ground.Y, exit->Ground.X) - 2.35619449f) < 0.01f && exit->Ground.Z == 97.0f,
        "the gondola's exit steps from the higher centre down onto the platform at +135 degrees");
    Check(exit && tally.Onward && exit->Onward.X == far.X && exit->Onward.Y == far.Y,
          "the onward point is the route's first point off the hull, not a point beside the quay");
    Check(exit && !exit->Walk.empty() && exit->Walk.back().X == exit->Deck.X && exit->Walk.back().Y == exit->Deck.Y,
          "the exit's deck walk ends at its deck edge");
    Check(tally.Counts[static_cast<std::size_t>(CrossingCheck::Corridor)] > 0,
          "a quay on a ledge that does not lead on to the route is rejected by its corridor");
    CrossingTally lost;
    Check(!FindExit(geometry, geometry.Bot, origin, origin, {stop}, &lost) && !lost.Onward &&
              lost.Counts[static_cast<std::size_t>(CrossingCheck::Onward)] == 1,
          "a route with no point off the hull gives no exit and counts it once");
}

// WT43c: Tirisfal's gondola as its collision model has it (Transport_Zeppelin.wmo). The transport's origin lies on the
// gondola's rail, whose top (98.46) is where the rider stands; the floor (97.07) spans |x| <= 12, -13 <= y < 0, and the
// rail's outer face (y = 0.6) is open only at the gangway (-8 <= x <= -2). The tower's platform (97.0) lies beyond the
// rail. The model's bounds centre (-7.65, -7.88) lies over the floor.
void CheckTirisfalGondola()
{
    struct TirisfalGeometry
    {
        Point Bot{0.0f, 0.0f, 98.46f};

        static bool Gangway(float x) { return x >= -8.0f && x <= -2.0f; }
        static std::optional<float> Surface(float x, float y)
        {
            if (std::fabs(x) > 12.0f || y < -13.0f || y > 0.6f)
                return std::nullopt;
            return y >= 0.0f && !Gangway(x) ? 98.46f : 97.07f;
        }
        // The rail's outer face between two points, open at the gangway
        static bool Face(Point const& a, Point const& b)
        {
            constexpr float FACE = 0.6f;
            if (std::min(a.Y, b.Y) > FACE || std::max(a.Y, b.Y) <= FACE)
                return false;
            return !Gangway(a.X + (b.X - a.X) * (FACE - a.Y) / (b.Y - a.Y));
        }
        static bool Platform(Point const& point)
        {
            return point.X >= -2.0f && point.X <= 6.0f && point.Y >= 1.0f && point.Y <= 12.0f;
        }
        static bool Far(Point const& point) { return point.Y >= 50.0f; }
        std::optional<Point> ShoreAt(Point const& seed, float vertical) const
        {
            bool const quay = Platform(seed) && std::fabs(seed.Z - 97.0f) <= vertical;
            return quay ? std::optional<Point>(Point{seed.X, seed.Y, 97.0f}) : std::nullopt;
        }
        std::optional<Point> SnapGround(Point point, float, float) const
        {
            if (Far(point))
                return point;
            if (Platform(point))
                return Point{point.X, point.Y, 97.0f};
            return std::nullopt;
        }
        std::optional<Point> DeckFrom(Point const& shore, float inset, Point const& toward) const
        {
            return DeckPointFrom(shore, toward, inset,
                                 [](Point const& point) -> std::optional<float>
                                 {
                                     auto const surface = Surface(point.X, point.Y);
                                     if (!surface || *surface > point.Z + 6.0f || *surface < point.Z - 12.0f)
                                         return std::nullopt;
                                     return surface;
                                 });
        }
        bool OffHull(Point const& point) const
        {
            return std::fabs(point.X) > 12.35f || point.Y < -13.35f || point.Y > 0.95f;
        }
        bool OnGround(Point const& point) const { return Platform(point) || Far(point); }
        bool GroundLeadsOn(Point const& start, Point const& end) const { return Platform(start) && Far(end); }
        // The body ray and the rays 0.35 yd to either side of it
        bool CrossingClear(Point const& start, Point const& end) const
        {
            float const length = std::hypot(end.X - start.X, end.Y - start.Y);
            for (float const side : {0.0f, -0.35f, 0.35f})
            {
                float const x = -(end.Y - start.Y) / length * side;
                float const y = (end.X - start.X) / length * side;
                if (Face({start.X + x, start.Y + y, start.Z}, {end.X + x, end.Y + y, end.Z}))
                    return false;
            }
            return true;
        }
        std::optional<std::vector<Point>> DeckPath(Point const& start, Point const& end, std::uint32_t cells,
                                                   std::uint32_t& expanded) const
        {
            return TravelLifecycle::DeckPath(
                start, end, 0.35f,
                [](Point const& top, float drop) -> std::optional<float>
                {
                    auto const surface = Surface(top.X, top.Y);
                    if (!surface || *surface > top.Z || top.Z - *surface > drop)
                        return std::nullopt;
                    return surface;
                },
                [](Point const& a, Point const& b) { return Face(a, b); }, cells, expanded);
        }
    } geometry;

    Point const origin{0.0f, 0.0f, 114.97f};
    Point const centre{-7.65f, -7.88f, 114.97f};
    std::vector<Point> const route{origin, {-30.0f, 100.0f, 40.0f}};
    CrossingTally toOrigin;
    Check(!FindExit(geometry, geometry.Bot, origin, origin, route, &toOrigin) &&
              toOrigin.Counts[static_cast<std::size_t>(CrossingCheck::Crossing)] > 0,
          "Tirisfal: deck points searched toward the origin stay on the rail, and their crossings meet its face");
    auto const exit = FindExit(geometry, geometry.Bot, origin, centre, route);
    float const part = exit ? (0.6f - exit->Deck.Y) / (exit->Ground.Y - exit->Deck.Y) : 0.0f;
    float const gangway = exit ? exit->Deck.X + (exit->Ground.X - exit->Deck.X) * part : 0.0f;
    Check(exit && exit->Deck.Z == 97.07f && TirisfalGeometry::Platform(exit->Ground) &&
              TirisfalGeometry::Gangway(gangway),
          "Tirisfal: the deck point searched toward the hull's centre lies on the floor past the gangway, and the "
          "crossing to the platform passes through the gangway");
    Check(exit && !exit->Walk.empty() && exit->Walk.back().X == exit->Deck.X && exit->Walk.back().Y == exit->Deck.Y &&
              AcceptExit(geometry, exit->Deck, exit->Ground, exit->Onward),
          "Tirisfal: the walk goes down from the rail to that deck point, and the exit passes its confirmation there");
}

// WT43: a round gondola (synthetic: deck at 98.46 within 7 yd of the origin's XY), its platform 10 yd out.
// Boarding's 4-yd inset after a 2-yd edge step puts the deck point 2 yd from the centre, behind the rail; an exit's
// 1-yd inset keeps it 1 to 2 yd inside the edge.
void CheckDeckPoint()
{
    Point const origin{0.0f, 0.0f, 114.97f};
    Point const shore{10.0f, 0.0f, 97.0f};
    auto const gondola = [&](Point const& point) -> std::optional<float>
    {
        if (std::hypot(point.X - origin.X, point.Y - origin.Y) <= 7.0f)
            return 98.46f;
        return std::nullopt;
    };
    auto const board = DeckPointFrom(shore, origin, BOARD_DECK_INSET_YARDS, gondola);
    Check(board && std::fabs(board->X - 2.0f) < 0.01f && board->Z == 98.46f,
          "boarding's deck point stays where it was: 4 yd past the first 2-yd step on the deck");
    auto const exit = DeckPointFrom(shore, origin, EXIT_DECK_INSET_YARDS, gondola);
    Check(exit && exit->X >= 5.0f && exit->X <= 6.0f && exit->Z == 98.46f,
          "an exit's deck point stays on the gondola 1 to 2 yd inside its edge, not at its centre");
    Check(!DeckPointFrom({200.0f, 0.0f, 97.0f}, origin, EXIT_DECK_INSET_YARDS, gondola),
          "a shore beyond the probe range has no deck point");
}

// WT43: Booty Bay's walk across the ship's deck. The deck is cambered (0.8 yd higher at the middle of a 16-yd walk) and
// a crate stands 1.5 yd high on the walk in the second case. The model blocks a ray that runs below its surface.
void CheckDeckWalk()
{
    bool crate = false;
    auto const camber = [](float x) { return 6.1f + 0.8f * (1.0f - (x / 8.0f) * (x / 8.0f)); };
    auto const surface = [&](float x) { return crate && std::fabs(x) <= 1.0f ? camber(x) + 1.5f : camber(x); };
    auto const deckAt = [&](Point const& top, float drop) -> std::optional<float>
    {
        if (std::fabs(top.X) > 8.5f || std::fabs(top.Y) > 5.0f)
            return std::nullopt;
        float const z = surface(top.X) <= top.Z ? surface(top.X) : camber(top.X);
        return top.Z - z <= drop ? std::optional<float>(z) : std::nullopt;
    };
    auto const blocks = [&](Point const& a, Point const& b)
    {
        for (int i = 0; i <= 20; ++i)
        {
            float const part = static_cast<float>(i) / 20.0f;
            float const x = a.X + (b.X - a.X) * part;
            if (std::fabs(x) <= 8.0f && a.Z + (b.Z - a.Z) * part < surface(x))
                return true;
        }
        return false;
    };
    Point const bot{-8.0f, 0.0f, 6.1f};
    Point const edge{8.0f, 0.0f, 6.1f};
    float const level = CrossingRayHeight(bot, edge);
    Check(blocks({bot.X, bot.Y, level}, {edge.X, edge.Y, level}),
          "one level ray at the bot's body height runs into the deck's camber (round 3's deck_walk rejection)");
    Check(FollowDeck(bot, edge, 0.35f, deckAt, blocks), "the walk along the deck's surface crosses the camber");
    crate = true;
    Check(!FollowDeck(bot, edge, 0.35f, deckAt, blocks), "a crate 1.5 yd high on the walk blocks it");
    crate = false;
    Check(!FollowDeck(bot, {12.0f, 0.0f, 6.1f}, 0.35f, deckAt, blocks), "a walk past the deck's end has no surface");
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
    CheckExitSearch();
    CheckDeckPoint();
    CheckDeckWalk();
    CheckDeckPath();
    CheckGondolaExit();
    CheckTirisfalGondola();
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
