/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHECKMOUNTSTATEACTION_H
#define PLAYERBOTS_CHECKMOUNTSTATEACTION_H

#include "UseItemAction.h"
#include <unordered_map>
#include <vector>

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

class CheckMountStateAction : public UseItemAction
{
public:
    CheckMountStateAction(PlayerbotAI* botAI) : UseItemAction(botAI, "check mount state", true) {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override { return true; }
    bool Mount();

    static void CompleteDismount(Player* bot);
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
    // A travel fly leg's end: off the mount, the stale flying flags cleared, down on the ground
    static void LandAndDismount(Player* bot);
    static void LoadPreferredMounts();

private:
    Player* master;
    ShapeshiftForm masterInShapeshiftForm;
    ShapeshiftForm botInShapeshiftForm;
    static std::unordered_map<uint32, PreferredMountCache> mountCache;
    float CalculateDismountDistance() const;
    float CalculateMountDistance() const;
    void Dismount();
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
