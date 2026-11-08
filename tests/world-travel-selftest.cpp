/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the honest world travel rules (src/Ai/World/Rpg/WorldTravelRules.h): the sight rule for
// teleports, the busy test, the corpse-run limit, the Stuck cause classification with its precedence, the position
// packing of the Stuck event and the avoided-spot memo.
#include <cstdint>
#include <iostream>
#include <string>

#include "WorldTravelRules.h"

using WorldTravel::SightPoint;
using WorldTravel::StuckCause;
using WorldTravel::StuckInput;

namespace
{
constexpr uint32_t MAP_KALIMDOR = 1;
constexpr uint32_t MAP_EASTERN = 0;
constexpr float RADIUS = 300.0f;

int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

void CheckSight()
{
    SightPoint const from{MAP_KALIMDOR, 1000.0f, 1000.0f, 50.0f};
    SightPoint const to{MAP_KALIMDOR, 5000.0f, 5000.0f, 10.0f};

    Check(WorldTravel::SeesTeleport({MAP_KALIMDOR, 1100.0f, 1000.0f, 50.0f}, from, to, RADIUS), "viewer near the bot");
    Check(WorldTravel::SeesTeleport({MAP_KALIMDOR, 5000.0f, 5299.0f, 10.0f}, from, to, RADIUS),
          "viewer near the destination");
    Check(!WorldTravel::SeesTeleport({MAP_KALIMDOR, 3000.0f, 3000.0f, 0.0f}, from, to, RADIUS), "viewer between, far");
    Check(!WorldTravel::SeesTeleport({MAP_KALIMDOR, 1301.0f, 1000.0f, 50.0f}, from, to, RADIUS),
          "viewer just outside the radius");
    Check(!WorldTravel::SeesTeleport({MAP_KALIMDOR, 1300.0f, 1000.0f, 50.0f}, from, to, RADIUS),
          "boundary: exactly at the radius is outside");
    Check(WorldTravel::SeesTeleport({MAP_KALIMDOR, 1299.0f, 1000.0f, 50.0f}, from, to, RADIUS),
          "boundary: just inside the radius");
    Check(!WorldTravel::SeesTeleport({MAP_EASTERN, 1000.0f, 1000.0f, 50.0f}, from, to, RADIUS),
          "same coordinates on another map");
    Check(!WorldTravel::SeesTeleport({MAP_KALIMDOR, 1000.0f, 1000.0f, 50.0f}, from, to, 0.0f), "radius 0 sees nothing");
    Check(WorldTravel::SeesTeleport({MAP_KALIMDOR, 1000.0f, 1000.0f, 250.0f}, from, to, RADIUS),
          "height counts: 200 yd above");
    Check(!WorldTravel::SeesTeleport({MAP_KALIMDOR, 1000.0f, 1000.0f, 400.0f}, from, to, RADIUS),
          "height counts: 350 yd above");

    // Cross-map teleport: a viewer on the destination map near the landing spot sees it
    SightPoint const otherMapTo{MAP_EASTERN, -8900.0f, 500.0f, 95.0f};
    Check(WorldTravel::SeesTeleport({MAP_EASTERN, -8850.0f, 500.0f, 95.0f}, from, otherMapTo, RADIUS),
          "viewer on the destination's map");
    Check(!WorldTravel::SeesTeleport({MAP_KALIMDOR, -8850.0f, 500.0f, 95.0f}, from, otherMapTo, RADIUS),
          "viewer on the bot's map at the destination's coordinates");

    Check(WorldTravel::IsViewer(false, false), "player is a viewer");
    Check(!WorldTravel::IsViewer(true, false), "invisible GM is not a viewer");
    Check(WorldTravel::IsViewer(true, true), "visible GM is a viewer");
}

void CheckBusy()
{
    using WorldTravel::BusyInput;
    BusyInput base;
    base.Honest = true;
    base.LegacyIdle = false;
    Check(!WorldTravel::IsBusy(base), "persistent, nothing in progress: free (legacy busy ignored)");

    auto const busyWith = [&base](bool BusyInput::* field)
    {
        BusyInput in = base;
        in.*field = true;
        return WorldTravel::IsBusy(in);
    };
    Check(busyWith(&BusyInput::RealMaster), "persistent with a real master is busy");
    Check(busyWith(&BusyInput::HasGoal), "persistent with a goal is busy");
    Check(busyWith(&BusyInput::InCombat), "persistent in combat is busy");
    Check(busyWith(&BusyInput::Dead), "persistent dead is busy");
    Check(busyWith(&BusyInput::Grouped), "persistent in a group is busy");
    Check(busyWith(&BusyInput::InInstance), "persistent in an instance is busy");
    Check(busyWith(&BusyInput::MidActivity), "persistent on a flight, route or errands is busy");

    BusyInput legacy;
    legacy.RealMaster = true;
    legacy.HasGoal = true;
    legacy.InCombat = true;
    legacy.MidActivity = true;
    legacy.LegacyIdle = true;
    Check(!WorldTravel::IsBusy(legacy), "other setups: legacy idle is free");
    legacy = BusyInput{};
    legacy.LegacyIdle = false;
    Check(WorldTravel::IsBusy(legacy), "other setups: legacy travel is busy");

    Check(WorldTravel::AppliesTo(true, false), "persistent applies");
    Check(!WorldTravel::AppliesTo(true, true), "arena instance keeps its cycle");
    Check(!WorldTravel::AppliesTo(false, false), "non-persistent keeps its cycle");
}

void CheckCorpseRuns()
{
    // Two counts per death; two failed runs = the third death
    Check(!WorldTravel::CorpseRunsExhausted(2, 2), "first death: run");
    Check(!WorldTravel::CorpseRunsExhausted(4, 2), "second death: run again");
    Check(!WorldTravel::CorpseRunsExhausted(5, 2), "second death, extra count: run again");
    Check(WorldTravel::CorpseRunsExhausted(6, 2), "third death: spirit healer");
    Check(WorldTravel::CorpseRunsExhausted(2, 0), "no failures allowed: spirit healer at once");
    Check(!WorldTravel::CorpseRunsExhausted(0, 0), "alive count 0: nothing");
}

void CheckStuck()
{
    auto classify = [](StuckInput const& input) { return WorldTravel::ClassifyStuck(input); };

    StuckInput path;
    path.PathComputed = true;
    Check(classify(path) == StuckCause::NoProgress, "path exists: no progress");

    StuckInput noPath = path;
    noPath.NoPath = true;
    Check(classify(noPath) == StuckCause::NoPath, "no path");

    StuckInput offMesh = path;
    offMesh.DestinationOffMesh = true;
    Check(classify(offMesh) == StuckCause::UnreachablePoi, "destination off the mesh: unreachable POI");
    offMesh.NoPath = true;
    Check(classify(offMesh) == StuckCause::UnreachablePoi, "off the mesh before no path");

    StuckInput door = noPath;
    door.NearDoorOrElevator = true;
    Check(classify(door) == StuckCause::DoorOrElevator, "door before path");

    StuckInput fell = door;
    fell.FallingOrSwimming = true;
    Check(classify(fell) == StuckCause::FellOrSwimming, "falling or water before door");

    StuckInput transport = fell;
    transport.OnTransport = true;
    Check(classify(transport) == StuckCause::OnTransport, "transport before falling");

    StuckInput otherMap = transport;
    otherMap.OtherMap = true;
    Check(classify(otherMap) == StuckCause::Other, "destination on another map: other");

    Check(classify(StuckInput{}) == StuckCause::Other, "no path asked: other");

    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::NoPath)) == "no_path", "word no_path");
    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::NoProgress)) == "no_progress", "word no_progress");
    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::FellOrSwimming)) == "fell_or_swimming",
          "word fell_or_swimming");
    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::OnTransport)) == "on_transport", "word on_transport");
    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::DoorOrElevator)) == "door_or_elevator",
          "word door_or_elevator");
    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::UnreachablePoi)) == "unreachable_poi",
          "word unreachable_poi");
    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::Other)) == "other", "word other");
}

void CheckPacking()
{
    int64_t const amount = WorldTravel::PackStuckPositions(-8913.4f, 554.6f, 1629.0f, -4373.5f);
    Check(WorldTravel::UnpackStuckCoord(amount, 0) == -8913, "bot x");
    Check(WorldTravel::UnpackStuckCoord(amount, 1) == 555, "bot y");
    Check(WorldTravel::UnpackStuckCoord(amount, 2) == 1629, "destination x");
    Check(WorldTravel::UnpackStuckCoord(amount, 3) == -4374, "destination y");

    int64_t const edges = WorldTravel::PackStuckPositions(-40000.0f, 40000.0f, -1.0f, 0.0f);
    Check(WorldTravel::UnpackStuckCoord(edges, 0) == -32768, "clamped low");
    Check(WorldTravel::UnpackStuckCoord(edges, 1) == 32767, "clamped high");
    Check(WorldTravel::UnpackStuckCoord(edges, 2) == -1, "minus one");
    Check(WorldTravel::UnpackStuckCoord(edges, 3) == 0, "zero");
}

void CheckAvoid()
{
    constexpr uint32_t AVOID_MS = 600000;
    SightPoint const given{MAP_KALIMDOR, 100.0f, 100.0f, 0.0f};
    Check(WorldTravel::IsAvoidedSpot(true, 1000, AVOID_MS, given, {MAP_KALIMDOR, 110.0f, 100.0f, 0.0f}),
          "near the given-up spot, fresh: avoided");
    Check(!WorldTravel::IsAvoidedSpot(true, AVOID_MS, AVOID_MS, given, {MAP_KALIMDOR, 110.0f, 100.0f, 0.0f}),
          "expired: free");
    Check(!WorldTravel::IsAvoidedSpot(true, 1000, AVOID_MS, given, {MAP_KALIMDOR, 130.0f, 100.0f, 0.0f}),
          "30 yd away: free");
    Check(!WorldTravel::IsAvoidedSpot(true, 1000, AVOID_MS, given, {MAP_EASTERN, 100.0f, 100.0f, 0.0f}),
          "other map: free");
    Check(!WorldTravel::IsAvoidedSpot(false, 0, AVOID_MS, given, given), "no memo: free");
}

void CheckAvoidRepeat()
{
    constexpr uint32_t AVOID_MS = 600000;
    SightPoint const given{MAP_KALIMDOR, 100.0f, 100.0f, 0.0f};
    SightPoint const farDest{MAP_KALIMDOR, 160.0f, 100.0f, 0.0f};
    Check(!WorldTravel::IsAvoidedSpot(true, 1000, AVOID_MS, given, farDest, 0), "60 yd away, first give-up: free");
    Check(!WorldTravel::IsAvoidedSpot(true, 1000, AVOID_MS, given, farDest, 1), "60 yd away, one repeat (40 yd): free");
    Check(WorldTravel::IsAvoidedSpot(true, 1000, AVOID_MS, given, farDest, 2),
          "60 yd away, two repeats (80 yd): avoided");
    Check(WorldTravel::StuckAvoidYards(9) == WorldTravel::StuckAvoidYards(WorldTravel::STUCK_REPEAT_MAX),
          "radius capped");
    Check(WorldTravel::StuckAvoidMs(AVOID_MS, 1) == 2 * AVOID_MS, "one repeat doubles the time");
    Check(WorldTravel::StuckAvoidMs(AVOID_MS, 9) == 8 * AVOID_MS, "time capped at three doublings");
    Check(WorldTravel::StuckAvoidMs(UINT32_MAX / 2, 3) == UINT32_MAX, "time saturates instead of wrapping");
    Check(WorldTravel::IsAvoidedSpot(true, AVOID_MS + 1, AVOID_MS, given, given, 1), "a repeat keeps it longer");
    Check(!WorldTravel::IsAvoidedSpot(true, 2 * AVOID_MS, AVOID_MS, given, given, 1), "and then expires");
    Check(!WorldTravel::IsAvoidedSpot(true, 1000, AVOID_MS, given, {MAP_EASTERN, 100.0f, 100.0f, 0.0f}, 3),
          "other map stays free");

    SightPoint const lastBot{MAP_KALIMDOR, 0.0f, 0.0f, 0.0f};
    SightPoint const sameCell{MAP_KALIMDOR, 60.0f, 50.0f, 0.0f};
    SightPoint const otherCell{MAP_KALIMDOR, 150.0f, 0.0f, 0.0f};
    Check(WorldTravel::NextStuckRepeats(true, 1000, AVOID_MS, 0, lastBot, sameCell) == 1, "same cell: repeat");
    Check(WorldTravel::NextStuckRepeats(true, 1000, AVOID_MS, 3, lastBot, sameCell) == 3, "repeats capped");
    Check(WorldTravel::NextStuckRepeats(true, 1000, AVOID_MS, 2, lastBot, otherCell) == 0, "another cell: reset");
    Check(WorldTravel::NextStuckRepeats(true, AVOID_MS, AVOID_MS, 0, lastBot, sameCell) == 0, "expired: reset");
    Check(WorldTravel::NextStuckRepeats(true, AVOID_MS, AVOID_MS, 1, lastBot, sameCell) == 2,
          "within the escalated time: repeat");
    Check(WorldTravel::NextStuckRepeats(false, 0, AVOID_MS, 0, lastBot, lastBot) == 0, "no memo: first");
    Check(WorldTravel::NextStuckRepeats(true, 1000, AVOID_MS, 0, lastBot, {MAP_EASTERN, 0.0f, 0.0f, 0.0f}) == 0,
          "other map: reset");
}

void CheckGhost()
{
    using WorldTravel::GhostInput;
    using WorldTravel::GhostStep;
    uint32_t const retry = WorldTravel::GHOST_TELEPORT_RETRY_MS;

    GhostInput atGrave;
    atGrave.AtGrave = true;
    Check(WorldTravel::GhostFallback(atGrave) == GhostStep::Wait, "a ghost at its grave never teleports");
    atGrave.TeleportedBefore = true;
    atGrave.SinceTeleportMs = 10 * retry;
    Check(WorldTravel::GhostFallback(atGrave) == GhostStep::Wait, "not even long after the last teleport");

    GhostInput far;
    Check(WorldTravel::GhostFallback(far) == GhostStep::Teleport, "a far ghost may teleport once");
    far.TeleportedBefore = true;
    far.SinceTeleportMs = retry - 1;
    Check(WorldTravel::GhostFallback(far) == GhostStep::Wait, "then waits for the interval");
    far.SinceTeleportMs = retry;
    Check(WorldTravel::GhostFallback(far) == GhostStep::Teleport, "and may teleport again after it");
    far.Seen = true;
    Check(WorldTravel::GhostFallback(far) == GhostStep::Wait, "a seen ghost waits");

    uint32_t const search = WorldTravel::GHOST_HEALER_RETRY_MS;
    Check(WorldTravel::RetryDue(false, 0, search), "the first healer search is due at once");
    Check(!WorldTravel::RetryDue(true, search - 1, search), "the next one waits for the interval");
    Check(WorldTravel::RetryDue(true, search, search), "and is due after it");
}
}  // namespace

namespace
{
void CheckHearthAndInns()
{
    Check(!WorldTravel::TeleportRebinds(true, true), "an honest bot's hearth teleport keeps its bind");
    Check(WorldTravel::TeleportRebinds(false, true), "an arena or old-setup bot still rebinds");
    Check(!WorldTravel::TeleportRebinds(true, false) && !WorldTravel::TeleportRebinds(false, false),
          "no hearth request, no rebind");

    Check(!WorldTravel::BindIsFar(true, 1000.0f), "a bind 1000 yd away is not far");
    Check(WorldTravel::BindIsFar(true, 1000.5f), "a bind past 1000 yd is far");
    Check(WorldTravel::BindIsFar(false, 0.0f), "a bind on another map is far");

    Check(WorldTravel::InnCheckDue(0, 5000), "the first inn check is due at once");
    Check(!WorldTravel::InnCheckDue(1000, 1000 + WorldTravel::INN_RETRY_MS - 1), "the next waits 30 minutes");
    Check(WorldTravel::InnCheckDue(1000, 1000 + WorldTravel::INN_RETRY_MS), "and is due after them");

    using WorldTravel::InnCandidate;
    float const reach = WorldTravel::INN_MAX_WALK_SECONDS * 7.0f;
    Check(WorldTravel::PickInn({}, reach) == -1, "no inns on the map, no errand");
    Check(WorldTravel::PickInn({{100.0f, false}, {200.0f, false}}, reach) == -1, "no fitting inn, no errand");
    Check(WorldTravel::PickInn({{100.0f, false}, {900.0f, true}, {400.0f, true}}, reach) == 2,
          "the nearest fitting inn, not the nearest inn");
    Check(WorldTravel::PickInn({{reach + 1.0f, true}}, reach) == -1,
          "a fitting inn past 10 walking minutes is no errand");
    Check(WorldTravel::PickInn({{reach, true}}, reach) == 0, "and one at exactly 10 minutes is");
    Check(WorldTravel::PickInn({{50.0f, true}, {50.0f, true}}, reach) == 0, "ties go to the first");
}
}  // namespace

int main()
{
    CheckHearthAndInns();
    CheckSight();
    CheckBusy();
    CheckCorpseRuns();
    CheckStuck();
    CheckPacking();
    CheckAvoid();
    CheckAvoidRepeat();
    CheckGhost();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
