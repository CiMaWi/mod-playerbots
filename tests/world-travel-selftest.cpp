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

    // The bot's own spot off the mesh (WT10e-2): before doors and the path result, after transport and water
    StuckInput botOff = door;
    botOff.StartOffMesh = true;
    Check(classify(botOff) == StuckCause::OffMesh, "bot off the mesh before door and path");
    StuckInput offNoPathAsked;
    offNoPathAsked.StartOffMesh = true;
    Check(classify(offNoPathAsked) == StuckCause::OffMesh, "bot off the mesh needs no path");
    StuckInput offInWater = botOff;
    offInWater.FallingOrSwimming = true;
    Check(classify(offInWater) == StuckCause::FellOrSwimming, "falling or water before off the mesh");
    StuckInput offOtherMap = botOff;
    offOtherMap.OtherMap = true;
    Check(classify(offOtherMap) == StuckCause::Other, "another map before off the mesh");
    Check(std::string(WorldTravel::StuckCauseWord(StuckCause::OffMesh)) == "off_mesh", "word off_mesh");

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

void CheckOffMeshRecovery()
{
    using WorldTravel::OffMeshNext;
    using WorldTravel::OffMeshRecovery;
    Check(OffMeshNext(true, true, true) == OffMeshRecovery::Hearth, "off mesh: a cast hearthstone comes first");
    Check(OffMeshNext(false, true, false) == OffMeshRecovery::Snap, "off mesh: no hearth, a poly near, unseen: snap");
    Check(OffMeshNext(false, true, true) == OffMeshRecovery::Seen, "off mesh: a real player sees it: no snap");
    Check(OffMeshNext(false, false, false) == OffMeshRecovery::NoPoly, "off mesh: no poly within 30 yd: nothing");
    Check(std::string(WorldTravel::OffMeshRecoveryWord(OffMeshRecovery::Hearth)) == "hearth" &&
              std::string(WorldTravel::OffMeshRecoveryWord(OffMeshRecovery::Snap)) == "snap" &&
              std::string(WorldTravel::OffMeshRecoveryWord(OffMeshRecovery::Seen)) == "seen" &&
              std::string(WorldTravel::OffMeshRecoveryWord(OffMeshRecovery::NoPoly)) == "no_poly",
          "off mesh: recovery words");
    Check(WorldTravel::OFF_MESH_SNAP_YARDS == 30.0f && WorldTravel::OFF_MESH_CHECK_ATTEMPT < 5,
          "off mesh: 30 yd snap, checked before the stuck give-up");
}

void CheckHearthRefusal()
{
    using WorldTravel::HearthRefusalWord;
    using WorldTravel::HearthState;
    auto word = [](HearthState const& state) { return std::string(HearthRefusalWord(state)); };
    HearthState fine;
    Check(word(fine) == "cast_failed", "hearth refused: nothing visible left is the core's cast check");
    HearthState mounted;
    mounted.mounted = true;
    Check(word(mounted) == "mounted", "hearth refused: mounted");
    HearthState flying = mounted;
    flying.flying = true;
    Check(word(flying) == "flying", "hearth refused: flying before mounted (no dismount in the air)");
    HearthState form;
    form.flightForm = true;
    Check(word(form) == "flight_form", "hearth refused: flight form");
    HearthState moving;
    moving.moving = true;
    Check(word(moving) == "moving", "hearth refused: moving");
    HearthState cooling;
    cooling.cooldown = true;
    Check(word(cooling) == "cooldown", "hearth refused: cooldown");
    HearthState combat = mounted;
    combat.inCombat = true;
    Check(word(combat) == "combat", "hearth refused: combat before mounted");
    HearthState bg;
    bg.battleground = true;
    Check(word(bg) == "battleground", "hearth refused: battleground");
    HearthState noStone = combat;
    noStone.stone = false;
    Check(word(noStone) == "no_stone", "hearth refused: no stone before combat");
    HearthState dead = noStone;
    dead.alive = false;
    Check(word(dead) == "dead", "hearth refused: dead first");
}

void CheckInnFilter()
{
    using WorldTravel::InnSpawnUsable;
    using WorldTravel::PhaseRowHidesSpawn;
    // Initiate Brenners (30308, phase 1) in The Shadow Vault (area 4477): Phase Shift I (30181, phase 2) is cast on
    // everyone until quest 12896/12897 is rewarded (quest_start 0, no aura condition, autocast)
    bool const brennersPhased = PhaseRowHidesSpawn(true, 0, 0, 2, 1);
    Check(brennersPhased && !InnSpawnUsable(1, false, brennersPhased), "inn filter: Initiate Brenners is phased out");
    // Lodge-Matron Embla (30005): faction template 2109's enemy mask holds every player
    Check(!InnSpawnUsable(1, true, false), "inn filter: Lodge-Matron Embla is hostile");
    // Marissa Everwatch (28791), The Argent Stand: phase 1, no phase row
    Check(InnSpawnUsable(1, false, false), "inn filter: Marissa Everwatch is kept");
    Check(!PhaseRowHidesSpawn(true, 13266, 0, 2, 1),
          "inn filter: a phase during a quest hides no spawn for a fresh bot");
    Check(!PhaseRowHidesSpawn(true, 0, 12345, 2, 1), "inn filter: a phase that needs an aura hides nothing");
    Check(!PhaseRowHidesSpawn(false, 0, 0, 2, 1), "inn filter: a row that is only allowed, not cast, hides nothing");
    Check(!PhaseRowHidesSpawn(true, 0, 0, 3, 1), "inn filter: a phase that holds phase 1 hides nothing");
    Check(PhaseRowHidesSpawn(true, 0, -500, 2, 1), "inn filter: a phase cast without some aura still hides");
    Check(!InnSpawnUsable(2, false, false), "inn filter: a spawn outside the normal phase is left out");
    Check(InnSpawnUsable(3, false, false), "inn filter: a spawn in phases 1 and 2 is kept");
}

void CheckInnAvoid()
{
    using WorldTravel::AvoidedSpot;
    std::vector<AvoidedSpot> spots;
    SightPoint const vault{571, 8495.0f, 2687.0f, 652.5f};
    SightPoint const nearVault{571, 8505.0f, 2687.0f, 652.5f};
    SightPoint const stand{571, 5458.0f, -2637.0f, 307.5f};
    uint32_t const now = 1000;
    Check(!WorldTravel::InnAvoided(spots, now, vault), "inn avoid: an empty list avoids nothing");
    WorldTravel::AvoidInn(spots, now, vault);
    Check(WorldTravel::InnAvoided(spots, now + 1000, vault), "inn avoid: the failed spot is avoided");
    Check(WorldTravel::InnAvoided(spots, now + 1000, nearVault), "inn avoid: and a spot 10 yd away");
    Check(!WorldTravel::InnAvoided(spots, now + 1000, stand), "inn avoid: another inn is not");
    Check(!WorldTravel::InnAvoided(spots, now + WorldTravel::INN_AVOID_MS, vault),
          "inn avoid: an hour later it is free");
    WorldTravel::AvoidInn(spots, now + 5000, nearVault);
    Check(spots.size() == 1 && spots[0].sinceMs == now + 5000, "inn avoid: the same spot again renews its entry");
    for (uint32_t i = 0; i < 6; ++i)
        WorldTravel::AvoidInn(spots, now + 10000 + i, {571, 100.0f * static_cast<float>(i), 0.0f, 0.0f});
    Check(spots.size() == WorldTravel::INN_AVOID_SPOTS, "inn avoid: at most four spots");
    Check(WorldTravel::InnAvoided(spots, now + 20000, {571, 500.0f, 0.0f, 0.0f}) &&
              !WorldTravel::InnAvoided(spots, now + 20000, vault),
          "inn avoid: the oldest gives way");
    uint32_t const wrapped = 0xFFFFF000u;
    std::vector<AvoidedSpot> wrapping;
    WorldTravel::AvoidInn(wrapping, wrapped, vault);
    Check(WorldTravel::InnAvoided(wrapping, wrapped + 0x2000u, vault), "inn avoid: survives the ms clock wrap");
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

void CheckInnBind()
{
    using WorldTravel::InnNext;
    using WorldTravel::InnStep;
    Check(InnNext(30.0f, false, 0) == InnStep::Walk, "inn: far away walks on");
    Check(InnNext(8.0f, false, 0) == InnStep::Walk, "inn: 8 yd is still too far to talk to the innkeeper");
    Check(InnNext(3.0f, false, 0) == InnStep::Bind, "inn: close enough binds");
    Check(InnNext(3.0f, false, 2) == InnStep::Bind, "inn: the third try is still allowed");
    Check(InnNext(3.0f, false, 3) == InnStep::Fail, "inn: three failed binds end the errand");
    Check(InnNext(8.0f, true, 0) == InnStep::Bind, "inn: stuck but in reach tries the bind");
    Check(InnNext(30.0f, true, 0) == InnStep::Fail, "inn: stuck out of reach ends the errand");
}

int main()
{
    CheckInnBind();
    CheckHearthAndInns();
    CheckSight();
    CheckBusy();
    CheckCorpseRuns();
    CheckStuck();
    CheckOffMeshRecovery();
    CheckHearthRefusal();
    CheckInnFilter();
    CheckInnAvoid();
    CheckAvoid();
    CheckAvoidRepeat();
    CheckGhost();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
