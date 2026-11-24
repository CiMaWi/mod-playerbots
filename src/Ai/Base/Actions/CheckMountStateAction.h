/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHECKMOUNTSTATEACTION_H
#define PLAYERBOTS_CHECKMOUNTSTATEACTION_H

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "GroundRules.h"
#include "UseItemAction.h"

const uint16 SPELL_TRAVEL_FORM = 783;
const uint16 SPELL_FLIGHT_FORM = 33943;
const uint16 SPELL_SWIFT_FLIGHT_FORM = 40120;

struct MountData
{
    bool swiftMount = false;
    // Outer map: index (0 for ground, 1 for flight), inner map: effect speed -> vector of spell IDs.
    std::map<uint32, std::map<int32, std::vector<uint32>>> allSpells;
    // Default mount speed.
    int32 maxSpeed = 59;
};

struct PreferredMountCache
{
    std::vector<uint32> groundMounts;
    std::vector<uint32> flightMounts;
};

class PlayerbotAI;

// MountRules.h (the mount owner's rule), included by the .cpp files that ask it
namespace MountRules
{
enum class Kind : std::uint8_t;
enum class How : std::uint8_t;
struct Verdict;
}  // namespace MountRules

class CheckMountStateAction : public UseItemAction
{
public:
    CheckMountStateAction(PlayerbotAI* botAI)
        : UseItemAction(botAI, "check mount state", true, "", ItemUse::Caller::Errand)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override { return true; }
    bool Mount();

    static void CompleteDismount(Player* bot);
    // A bot above the engine's ground falls onto it as a player does: the engine's fall and its damage, the mount kept
    // (CompleteDismount's fall; pathing PA31: a bot off its floor without a flying mount, Ground::Act::Fall); a bot
    // standing on the navmesh makes no fall (Ground::FallFloor, G338). caller: the trace's word
    static void FallToFloor(Player* bot, char const* caller);
    // The navmesh at the bot's feet (NavMeshSnap::Nearest in the walk's start box): Unknown without a tile, None
    // without a walkable polygon, Found with the nearest point's height (Ground::StandsOn judges it)
    static Ground::MeshRead MeshUnder(Player* bot);
    // The one mount owner's answer for this bot now (MountRules::Policy over its loot, test constraint and trip):
    // the mount action asks for Kind::Ground, a travel fly leg's MountToFly for Kind::Flying
    static MountRules::Verdict AskMountPolicy(Player* bot, PlayerbotAI* botAI, MountRules::Kind wanted);
    // Every travel and mount-action request to come off a mount (the mount action, a transport leg's boarding, a loot
    // motion, the flight master, a fly leg's takeoff, a hearth leg's cast): stop, then the client's cancel request.
    // Not the air landing (LandAndDismount, CompleteDismount). Named exceptions (WT44): upkeep's tame and stable
    // (NewRpgStable.cpp), the old RPG flight master (RpgTaxiAction) and the spell actions' in-combat mount check
    // (CastSpellAction)
    static void Dismount(Player* bot, MountRules::How how);
    // Own flying (upkeep WU10): riding 225, and Cold Weather Flying on Northrend's virtual map
    static bool RidingAllowsFlying(Player const* bot, uint32 virtualMapId);
    // The fastest flying mount spell the bot knows and may ride: its speed bonus in percent (149, 279), 0 for none
    static int32 FlyingMountSpeed(Player const* bot);
    // Why the bot cannot mount a flyer and lift off where it stands (a travel fly leg's takeoff), nullptr when it can:
    // no_fly_area (its real area, WMO areas such as Dalaran's included, has no flying; riding; a battleground),
    // battle (a battlefield's zone in battle), indoors, water
    static char const* NoTakeoffHere(Player const* bot);
    // A travel fly leg's mount: the fastest flying mount the bot knows and may cast now, a preferred one first. ""
    // when a cast started, else why not: no_mount (none known for its riding), refused (the cast check refused all).
    // Not through Execute: its current-target, master and below-the-floor rules are for mounting on the move, and they
    // refused a bot that stood still on open ground (WU11b: Tarae in Dragonblight)
    static char const* CastFlyingMount(Player* bot, PlayerbotAI* botAI);
    // A travel fly leg's end: off the mount, down on the ground (the fall), then the stale flying flags cleared
    static void LandAndDismount(Player* bot);
    // The floor below the bot as {ground, water surface}: the ground with MAX_FALL_DISTANCE search (vmap too), the
    // water over it (TravelLegs::FLY_NO_GROUND: none). The one sampler of a fly leg's descent, a dismount's log and
    // the body a killed flyer lies on (TravelLegs::BodyZ takes the higher of the two)
    static std::pair<float, float> FloorBelow(Player* bot);
    static void LoadPreferredMounts();
    // A deleted bot's entry (BotStores::Delete); world thread
    static void ForgetBot(uint32 guid);

private:
    Player* master;
    ShapeshiftForm masterInShapeshiftForm;
    ShapeshiftForm botInShapeshiftForm;
    static std::unordered_map<uint32, PreferredMountCache> mountCache;
    float CalculateDismountDistance() const;
    float CalculateMountDistance() const;
    void ClearStaleFlightFlags();
    bool ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const;
    bool ShouldDismountForMaster(Player* master) const;
    int32 CalculateMasterMountSpeed(Player* master) const;
    bool CheckForSwiftMount() const;
    std::map<uint32, std::map<int32, std::vector<uint32>>> GetAllMountSpells() const;
    bool TryForms(Player* master, int32 masterMountType, int32 masterSpeed) const;
    bool TryPreferredMount(Player* master) const;
    uint32 GetMountType(Player* master) const;
    bool TryRandomMountFiltered(std::map<int32, std::vector<uint32>> const& spells, int32 masterSpeed) const;
};

#endif
