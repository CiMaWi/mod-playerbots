/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CheckMountStateAction.h"
#include "PlayerbotsDatabase.h"
#include "AreaDefines.h"
#include "BattleGroundTactics.h"
#include "BattlefieldMgr.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "Event.h"
#include "GridTerrainData.h"
#include "LootInteraction.h"
#include "Map.h"
#include "NewRpgAction.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "TestConstraints.h"
#include "TravelLegRules.h"

static constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;
static constexpr float PARACHUTE_LAND_THRESHOLD = 15.0f;

// On a flying mount (or in a flight form) and in the air by the real height (TravelLegs::InTheAir), flag or not
// (WU11c: a flight aura applied in the air sets CAN_FLY only, so Meldel flew without the flying flag)
static bool FlyerInTheAir(Player* bot)
{
    if (!bot->HasIncreaseMountedFlightSpeedAura() && !bot->HasFlyAura())
        return false;
    return TravelLegs::InTheAir(bot->GetPositionZ(),
                                bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
}

// Preferred mounts per bot, loaded once at startup by LoadPreferredMounts()
std::unordered_map<uint32, PreferredMountCache> CheckMountStateAction::mountCache;

MountData CollectMountData(Player const* bot)
{
    MountData data;
    uint32 const ridingSkill = bot->GetPureSkillValue(SKILL_RIDING);
    for (auto& entry : bot->GetSpellMap())
    {
        uint32 spellId = entry.first;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            continue;

        if (entry.second->State == PLAYERSPELL_REMOVED || !entry.second->Active || spellInfo->IsPassive())
            continue;

        // The core casts a known mount without checking Riding.
        if (PlayerbotSpellRepository::Instance().GetRequiredRidingSkill(spellInfo) > ridingSkill)
            continue;

        int32 effect1 = spellInfo->Effects[1].BasePoints;
        int32 effect2 = spellInfo->Effects[2].BasePoints;

        int32 speed = std::max(effect1, effect2);

        // Update max speed if appropriate.
        if (speed > data.maxSpeed)
            data.maxSpeed = speed;  // In BG, clamp max speed to 99 later; here we just store the maximum found.

        // Determine index: flight if either effect has flight aura or specific mount ID.
        uint32 index = (spellInfo->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        spellInfo->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        // Winged Steed of the Ebon Blade
                        // This mount is meant to autoscale from a 150% flyer
                        // up to a 280% as you train your flying skill up.
                        // This incorrectly gets categorised as a ground mount, force this to flyer only.
                        // TODO: Add other scaling mounts here if they have the same issue, or adjust above
                        // checks so that they are all correctly detected.
                        spellInfo->Id == 54729) ? 1 : 0;
        data.allSpells[index][speed].push_back(spellId);
    }
    return data;
}

bool CheckMountStateAction::Execute(Event event)
{
    // Forced flight dismount:
    // Bots get stale flight movement flags after a forced dismount (e.g: Dalaran) because the post landing dismount cleanup
    // needs MSG_MOVE_FALL_LAND (a client opcode) and client movement packets. The stale flags cause the bot to be stuck with
    // the parachute, or even keep the bot hovering indefinitely and block MMAP routing.
    // Note: Without MSG_MOVE_FALL_LAND, HandleFall doesn't trigger, meaning bots don't get fall damage in forced dismounts anyway,
    // so the parachute usage here is more of an immersion feature.
    if (bot->HasFeatherFallAura())
    {
        float floorZ = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (floorZ != INVALID_HEIGHT && floorZ != VMAP_INVALID_HEIGHT_VALUE &&
            bot->GetPositionZ() - floorZ <= PARACHUTE_LAND_THRESHOLD)
            bot->RemoveAurasByType(SPELL_AURA_FEATHER_FALL);
    }
    if (!botAI->HasPendingDismount())
        ClearStaleFlightFlags();
    // In the air by the real height, not the flag: a bot on its flyer on the ground keeps the flag, one in the air
    // may lack it
    bool const inTheAir = FlyerInTheAir(bot);

    auto const tryMount = [&]
    {
        auto path = PlayerbotAI::MountPath::None;
        if (event.getPacket().GetOpcode() == SMSG_FORCE_RUN_SPEED_CHANGE)
            path = PlayerbotAI::MountPath::Packet;
        else if (event.GetSource() == "timer")
            path = PlayerbotAI::MountPath::Timer;
        if (!LootInteraction::MountAllowed(botAI->HasLootInteraction(), bool(bot->GetLootGUID())))
        {
            botAI->TraceTravelMount(path, false);
            return false;
        }

        bool const wasMounted = bot->IsMounted();
        bool const mounted = Mount();
        if (mounted && !wasMounted)
            botAI->TraceTravelMount(path, true);
        return mounted;
    };

    // Determine if there are no attackers
    bool noAttackers = !AI_VALUE2(bool, "combat", "self target") || !AI_VALUE(uint8, "attacker count");
    bool enemy = AI_VALUE(Unit*, "enemy player target");
    bool dps = AI_VALUE(Unit*, "dps target");
    bool shouldDismount = false;
    bool shouldMount = false;

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (currentTarget)
    {
        float dismountDistance = CalculateDismountDistance();
        float mountDistance = CalculateMountDistance();
        float combatReach = bot->GetCombatReach() + currentTarget->GetCombatReach();
        float distanceToTarget = bot->GetExactDist(currentTarget);

        // Not in the air: the bot would fall (WU11b: Nennaro, attacked 36 yd up on a fly leg's climb, came down 13 s
        // after the takeoff); it flies on or lands first
        shouldDismount = !inTheAir && distanceToTarget <= dismountDistance + combatReach;
        shouldMount = (distanceToTarget > mountDistance + combatReach);
    }
    else
    {
        shouldMount = true;
    }

    // If should dismount, or master (if any) is no longer in travel form, yet bot still is, remove the shapeshifts
    if (shouldDismount ||
        (masterInShapeshiftForm != FORM_TRAVEL && botInShapeshiftForm == FORM_TRAVEL) ||
        (masterInShapeshiftForm != FORM_FLIGHT && botInShapeshiftForm == FORM_FLIGHT && master && !master->IsMounted()) ||
        (masterInShapeshiftForm != FORM_FLIGHT_EPIC && botInShapeshiftForm == FORM_FLIGHT_EPIC && master && !master->IsMounted()))
        botAI->RemoveShapeshift();

    if (shouldDismount && bot->IsMounted())
    {
        Dismount();
        return true;
    }

    bool inBattleground = bot->InBattleground();
    bool const noRealMaster = (!master || master == bot);

    // If there is a master and bot not in BG, follow master's mount state regardless of group leader
    if (!noRealMaster && !inBattleground)
    {
        if (ShouldFollowMasterMountState(master, noAttackers, shouldMount))
            return tryMount();

        else if (ShouldDismountForMaster(master) && bot->IsMounted())
        {
            Dismount();
            return true;
        }

        return false;
    }

    // No real master (random bot or SelfBot) OR bot in BG
    if ((noRealMaster || inBattleground) && !bot->IsMounted() &&
        noAttackers && shouldMount && !bot->IsInCombat())
        return tryMount();

    if (!inTheAir && shouldDismount && bot->IsMounted() && (enemy || dps || (!noAttackers && bot->IsInCombat())))
    {
        Dismount();
        return true;
    }

    return false;
}

bool CheckMountStateAction::isUseful()
{
    // Not useful when:
    if (botAI->IsInVehicle() || bot->isDead() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) ||
        !bot->IsOutdoors() || bot->InArena())
        return false;

    master = GetMaster();

    // Get shapeshift states, only applicable when there's a master
    if (master)
    {
        botInShapeshiftForm = bot->GetShapeshiftForm();
        masterInShapeshiftForm = master->GetShapeshiftForm();
    }

    // Not useful when in combat and not currently mounted / travel formed
    if ((bot->IsInCombat() || botAI->GetState() == BOT_STATE_COMBAT) &&
        !bot->IsMounted() && botInShapeshiftForm != FORM_TRAVEL && botInShapeshiftForm != FORM_FLIGHT && botInShapeshiftForm != FORM_FLIGHT_EPIC)
        return false;

    // In addition to checking IsOutdoors, also check whether bot is clipping below floor slightly because that will
    // cause bot to falsly indicate they are outdoors. This fixes bug where bot tries to mount indoors (which seems
    // to mostly be an issue in tunnels of WSG and AV)
    float posZ = bot->GetPositionZ();
    float groundLevel = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), posZ);
    if (!bot->IsMounted() && !bot->HasWaterWalkAura() && posZ < groundLevel)
        return false;

    // Not useful when a transport leg keeps the bot on foot (waiting at the dock, boarding, aboard)
    if (!bot->IsMounted() && TravelKeepsOnFoot(botAI))
        return false;

    // Not useful when bot does not have mount strat and is not currently mounted
    if (!GET_PLAYERBOT_AI(bot)->HasStrategy("mount", BOT_STATE_NON_COMBAT) && !bot->IsMounted())
        return false;

    // Not useful when level lower than minimum required
    if (bot->GetLevel() < sPlayerbotAIConfig.UseGroundMountAtMinLevel)
        return false;

    // Allow mounting while transformed only if the form allows it
    if (bot->HasAuraType(SPELL_AURA_TRANSFORM) && bot->IsInDisallowedMountForm())
        return false;

    // Mount attempts stop movement, including active jumps.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE && bot->movespline &&
        !bot->movespline->Finalized())
        return false;

    // BG Logic
    if (bot->InBattleground())
    {
        // Do not use when carrying BG Flags
        if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) || bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
            return false;

        // Only mount if BG starts in less than 30 sec
        if (Battleground* bg = bot->GetBattleground())
            if (bg->GetStatus() == STATUS_WAIT_JOIN && bg->GetStartDelayTime() > BG_START_DELAY_30S)
                return false;
    }

    return true;
}

bool CheckMountStateAction::Mount()
{
    // A test's nomount bot stays on foot (TestConstraints::MountsOnItsOwn)
    if (!TestConstraints::MountsOnItsOwn(botAI->rpgInfo.constraints))
        return false;

    // Remove current Shapeshift if need be
    if (botInShapeshiftForm != FORM_TRAVEL &&
        botInShapeshiftForm != FORM_FLIGHT &&
        botInShapeshiftForm != FORM_FLIGHT_EPIC)
    {
        botAI->RemoveShapeshift();
        botAI->RemoveAura("tree of life");
    }

    if (TryPreferredMount(master))
        return true;

    // Get bot mount data
    MountData mountData = CollectMountData(bot);
    int32 masterMountType = GetMountType(master);
    int32 masterSpeed = CalculateMasterMountSpeed(master);

    // Try shapeshift
    if (TryForms(master, masterMountType, masterSpeed))
        return true;

    // Try random mount
    auto spellsIt = mountData.allSpells.find(masterMountType);
    if (spellsIt != mountData.allSpells.end())
    {
        auto& spells = spellsIt->second;
        if (TryRandomMountFiltered(spells, masterSpeed))
            return true;
    }

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "mount");
    if (!items.empty())
        return UseItemAuto(*items.begin());

    return false;
}

void CheckMountStateAction::Dismount()
{
    // A flyer dismounted more than AIRBORNE_YARDS above the terrain (WU11b fix 11: Hudohrgand lost its mount about
    // 45 yd from its takeoff over Honor Hold, not in combat, right after this action ran; the logs could not show
    // why): the probe that judged it on the ground, its target and fight, for the log only. The terrain counts with
    // the water over it (fix 12: a bot swimming at the sea's surface is not above the seabed)
    if (bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura())
    {
        float const x = bot->GetPositionX();
        float const y = bot->GetPositionY();
        float const z = bot->GetPositionZ();
        auto const [groundZ, waterZ] = FloorBelow(bot);
        float const terrainZ = std::max(groundZ, waterZ);
        if (terrainZ > TravelLegs::FLY_NO_GROUND && z - terrainZ > TravelLegs::AIRBORNE_YARDS)
        {
            Unit* const target = AI_VALUE(Unit*, "current target");
            LOG_INFO("playerbots",
                     "[Route] {} dismounts {:.0f} yd above the terrain or water in area {} at ({:.0f}, {:.0f}, "
                     "{:.0f}): floor {:.0f}, flying {}, combat {}, target {} at {:.0f} yd",
                     bot->GetName(), z - terrainZ, bot->GetAreaId(), x, y, z, bot->GetMapHeight(x, y, z),
                     bot->IsFlying(), bot->IsInCombat(), target ? target->GetName() : "none",
                     target ? bot->GetExactDist(target) : 0.0f);
        }
    }
    if (bot->isMoving())
        bot->StopMoving();

    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
}

static void ClearStaleFlightFlagsOf(Player* bot)
{
    if (bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura())
        return;

    if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY))
    {
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_CAN_FLY);
        if (!bot->IsRooted())
            bot->SendMovementFlagUpdate();
    }
}

void CheckMountStateAction::ClearStaleFlightFlags() { ClearStaleFlightFlagsOf(bot); }

std::pair<float, float> CheckMountStateAction::FloorBelow(Player* bot)
{
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const groundZ = bot->GetMapHeight(x, y, bot->GetPositionZ(), true, MAX_FALL_DISTANCE);
    // The water over the ground (the sea's surface, not its floor); none where the ground is dry or not found
    LiquidData const water =
        groundZ > TravelLegs::FLY_NO_GROUND
            ? bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), x, y, groundZ, bot->GetCollisionHeight(), {})
            : LiquidData();
    return {groundZ, water.Status != LIQUID_MAP_NO_WATER ? water.Level : TravelLegs::FLY_NO_GROUND};
}

void CheckMountStateAction::LandAndDismount(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return;
    if (bot->isMoving())
        bot->StopMoving();
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    ClearStaleFlightFlagsOf(bot);
    CompleteDismount(bot);
}

void CheckMountStateAction::CompleteDismount(Player* bot)
{
    if (!bot->IsAlive() || bot->IsMounted() || bot->IsInFlight() || bot->GetVehicle() || bot->GetTransport() ||
        bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) || bot->HasIncreaseMountedFlightSpeedAura() ||
        bot->HasFlyAura() || !bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY))
        return;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const startZ = bot->GetPositionZ();

    float const groundZ = bot->GetMapHeight(x, y, startZ, true, MAX_FALL_DISTANCE);
    if (groundZ <= INVALID_HEIGHT || startZ - groundZ <= 0.1f)
        return;

    bot->GetMotionMaster()->MoveFall();
    MovementInfo fallInfo = bot->m_movementInfo;
    // Need to set the start of the fall, otherwise the fall may start from too high of a Z and kill the bot.
    bot->SetFallInformation(0, startZ);
    fallInfo.pos.Relocate(x, y, groundZ);
    bot->HandleFall(fallInfo);
    // Re-anchor at the ground: Player::IsFalling() compares standing Z to this, so startZ reads as a fall.
    bot->SetFallInformation(0, groundZ);
    bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
}

bool CheckMountStateAction::TryForms(Player* master, int32 masterMountType, int32 masterSpeed) const
{
    if (!master)
        return false;

    // If both master and bot are in matching forms or master is mounted with corresponding speed, nothing to do
    else if
        ((masterInShapeshiftForm == FORM_TRAVEL && botInShapeshiftForm == FORM_TRAVEL) ||
        ((masterInShapeshiftForm == FORM_FLIGHT || (masterMountType == 1 && masterSpeed == 149)) && botInShapeshiftForm == FORM_FLIGHT) ||
        ((masterInShapeshiftForm == FORM_FLIGHT_EPIC || (masterMountType == 1 && masterSpeed >= 279)) && botInShapeshiftForm == FORM_FLIGHT_EPIC))
        return true;

    // Check if master is in Travel Form and bot can do the same
    if (botAI->CanCastSpell(SPELL_TRAVEL_FORM, bot, true) &&
        masterInShapeshiftForm == FORM_TRAVEL && botInShapeshiftForm != FORM_TRAVEL)
    {
        botAI->CastSpell(SPELL_TRAVEL_FORM, bot);
        return true;
    }

    // Check if master is in Flight Form or has a flying mount and bot can flight form
    if (botAI->CanCastSpell(SPELL_FLIGHT_FORM, bot, true) &&
        ((masterInShapeshiftForm == FORM_FLIGHT && botInShapeshiftForm != FORM_FLIGHT) ||
        (masterMountType == 1 && masterSpeed == 149)))
    {
        botAI->CastSpell(SPELL_FLIGHT_FORM, bot);

        // Compensate speedbuff
        bot->SetSpeed(MOVE_RUN, 2.5, true);
        return true;
    }

    // Check if master is in Swift Flight Form or has an epic flying mount and bot can swift flight form
    if (botAI->CanCastSpell(SPELL_SWIFT_FLIGHT_FORM, bot, true) &&
        ((masterInShapeshiftForm == FORM_FLIGHT_EPIC && botInShapeshiftForm != FORM_FLIGHT_EPIC) ||
        (masterMountType == 1 && masterSpeed >= 279)))
    {
        botAI->CastSpell(SPELL_SWIFT_FLIGHT_FORM, bot);

        // Compensate speedbuff
        bot->SetSpeed(MOVE_RUN, 3.8, true);
        return true;
    }

    return false;
}

void CheckMountStateAction::LoadPreferredMounts()
{
    mountCache.clear();

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_PREFERRED_MOUNTS);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return;

    uint32 totalResults = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].Get<uint32>();
        uint32 spellId = fields[1].Get<uint32>();
        uint32 mountType = fields[2].Get<uint32>();

        if (mountType == 0)
            mountCache[guid].groundMounts.push_back(spellId);
        else if (mountType == 1)
            mountCache[guid].flightMounts.push_back(spellId);

        totalResults++;
    } while (result->NextRow());

    LOG_INFO("playerbots", "Preferred mounts initialized | Total records: {}", totalResults);
}

bool CheckMountStateAction::TryPreferredMount(Player* master) const
{
    uint32 botGUID = bot->GetGUID().GetRawValue();

    // mountCache is filled once on the world thread at startup and only read afterwards
    auto itr = mountCache.find(botGUID);
    if (itr == mountCache.end())
        return false;

    PreferredMountCache const& preferred = itr->second;

    // Pick a random preferred mount from the selection, if available
    uint32 chosenMountId = 0;

    if (GetMountType(master) == 0 && !preferred.groundMounts.empty())
    {
        uint32 index = urand(0, preferred.groundMounts.size() - 1);
        chosenMountId = preferred.groundMounts[index];
    }

    else if (GetMountType(master) == 1 && !preferred.flightMounts.empty())
    {
        uint32 index = urand(0, preferred.flightMounts.size() - 1);
        chosenMountId = preferred.flightMounts[index];
    }

    // No suitable preferred mount found
    if (chosenMountId == 0)
        return false;

    // Check if spell exists
    if (!sSpellMgr->GetSpellInfo(chosenMountId))
    {
        LOG_ERROR("playerbots", "Preferred mount failed: Invalid spell {} | Bot Guid: {}", chosenMountId, botGUID);
        return false;
    }

    // Required here as otherwise bots won't mount in BG's due to them constant moving
    if (bot->isMoving())
        bot->StopMoving();

    // Check if spell can be cast - for now allow all, even if the bot does not have the actual mount
    //if (botAI->CanCastSpell(mountId, botAI->GetBot()))
    //{
    botAI->CastSpell(chosenMountId, botAI->GetBot());
    return true;
    //}

    LOG_DEBUG("playerbots", "Preferred mount failed! | Bot Guid: {}", botGUID);
    return false;
}

bool CheckMountStateAction::TryRandomMountFiltered(std::map<int32, std::vector<uint32>> const& spells, int32 masterSpeed) const
{
    for (auto it = spells.rbegin(); it != spells.rend(); ++it)
    {
        int32 currentSpeed = it->first;

        if ((masterSpeed > 59 && currentSpeed < 99) || (masterSpeed > 149 && currentSpeed < 279))
            continue;

        // Pick a random mount from the candidate group.
        auto const& ids = it->second;
        if (!ids.empty())
        {
            // Required here as otherwise bots won't mount in BGs due to them constant moving
            if (bot->isMoving())
                bot->StopMoving();

            uint32 index = urand(0, ids.size() - 1);

            if (botAI->CanCastSpell(ids[index], bot))
            {
                botAI->CastSpell(ids[index], bot);
                return true;
            }
        }
    }
    return false;
}

float CheckMountStateAction::CalculateDismountDistance() const
{
    // Warrior bots should dismount far enough to charge (because it's important for generating some initial rage),
    // a real player would be riding toward enemy mashing the charge key but the bots won't cast charge while mounted.
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float dismountDistance = isMelee ? sPlayerbotAIConfig.MeleeDistance + 2.0f : sPlayerbotAIConfig.SpellDistance + 2.0f;
    return bot->getClass() == CLASS_WARRIOR ? std::max(18.0f, dismountDistance) : dismountDistance;
}

float CheckMountStateAction::CalculateMountDistance() const
{
    // Mount distance should be >= 21 regardless of class, because when travelling a distance < 21 it takes longer
    // to cast mount-spell than the time saved from the speed increase. At a distance of 21 both approaches take 3
    // seconds:
    // 21 / 7  =  21 / 14 + 1.5  =  3   (7 = dismounted speed  14 = epic-mount speed  1.5 = mount-spell cast time)
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float baseDistance = isMelee ? sPlayerbotAIConfig.MeleeDistance + 10.0f : sPlayerbotAIConfig.SpellDistance + 10.0f;
    return std::max(21.0f, baseDistance);
}

bool CheckMountStateAction::ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const
{
    bool isMasterMounted = master->IsMounted() || (masterInShapeshiftForm == FORM_FLIGHT ||
                                                    masterInShapeshiftForm == FORM_FLIGHT_EPIC ||
                                                    masterInShapeshiftForm == FORM_TRAVEL);
    bool isBotMountedOrForm = bot->IsMounted() || botInShapeshiftForm == FORM_FLIGHT ||
                              botInShapeshiftForm == FORM_FLIGHT_EPIC || botInShapeshiftForm == FORM_TRAVEL;
    return isMasterMounted && !isBotMountedOrForm && noAttackers &&
        shouldMount && !bot->IsInCombat() && botAI->GetState() != BOT_STATE_COMBAT;
}

bool CheckMountStateAction::ShouldDismountForMaster(Player* master) const
{
    bool isMasterMounted = master->IsMounted() || (masterInShapeshiftForm == FORM_FLIGHT ||
                                                   masterInShapeshiftForm == FORM_FLIGHT_EPIC ||
                                                   masterInShapeshiftForm == FORM_TRAVEL);
    return !isMasterMounted && bot->IsMounted();
}

bool CheckMountStateAction::RidingAllowsFlying(Player const* bot, uint32 virtualMapId)
{
    if (bot->GetPureSkillValue(SKILL_RIDING) < 225)
        return false;
    return virtualMapId != MAP_NORTHREND || bot->HasSpell(SPELL_COLD_WEATHER_FLYING);
}

int32 CheckMountStateAction::FlyingMountSpeed(Player const* bot)
{
    MountData const data = CollectMountData(bot);
    auto const flying = data.allSpells.find(1);
    if (flying == data.allSpells.end() || flying->second.empty())
        return 0;
    // The scaling Winged Steed's own points are not its flight speed: at least the slow flyer's
    return std::max(flying->second.rbegin()->first, 149);
}

static bool BotCanUseFlyingMount(Player const* bot)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    if (!area || !area->IsFlyable())
        return false;
    if (area->flags & AREA_FLAG_NO_FLY_ZONE)
        return false;

    uint32 const virtualMapId = GetVirtualMapForMapAndZone(bot->GetMapId(), bot->GetZoneId());
    return CheckMountStateAction::RidingAllowsFlying(bot, virtualMapId);
}

char const* CheckMountStateAction::NoTakeoffHere(Player const* bot)
{
    if (bot->InBattleground() || !BotCanUseFlyingMount(bot))
        return "no_fly_area";
    if (Battlefield const* battlefield = sBattlefieldMgr->GetBattlefieldToZoneId(bot->GetZoneId()))
        if (!battlefield->CanFlyIn())
            return "battle";
    if (!bot->IsOutdoors())
        return "indoors";
    if (bot->IsInWater())
        return "water";
    return nullptr;
}

char const* CheckMountStateAction::CastFlyingMount(Player* bot, PlayerbotAI* botAI)
{
    // A cast the check allowed that still did not start (rare, WU11b: Hellengi's three silent refusals): why, for
    // the log only. A shapeshift form the mount cannot be cast in (WU11b fix 9: the druids Pilge and Barilas, prepare
    // result 68, SPELL_FAILED_NOT_SHAPESHIFT) is left first, as a player cancels it; never in the air (by the height:
    // a stale flag on the ground kept Seyo's form)
    auto const cast = [bot, botAI](uint32 spellId) -> char const*
    {
        ShapeshiftForm const form = bot->GetShapeshiftForm();
        if (form != FORM_NONE && !FlyerInTheAir(bot))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                if (info->CheckShapeshift(form) != SPELL_CAST_OK)
                    bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        if (botAI->CastSpell(spellId, bot))
            return "";
        LOG_INFO("playerbots",
                 "[Route] {} flying mount {} not cast: prepare result {}, stand state {}, moving {}, flying {}",
                 bot->GetName(), spellId, botAI->LastPrepareResult(), bot->getStandState(), bot->isMoving(),
                 bot->IsFlying());
        return "refused";
    };
    auto const preferred = mountCache.find(bot->GetGUID().GetRawValue());
    if (preferred != mountCache.end())
        for (uint32 const spellId : preferred->second.flightMounts)
            if (botAI->CanCastSpell(spellId, bot))
                return cast(spellId);

    MountData const data = CollectMountData(bot);
    auto const flying = data.allSpells.find(1);
    if (flying == data.allSpells.end() || flying->second.empty())
        return "no_mount";
    for (auto speed = flying->second.rbegin(); speed != flying->second.rend(); ++speed)
        for (uint32 const spellId : speed->second)
            if (botAI->CanCastSpell(spellId, bot))
                return cast(spellId);
    // Why, for the log only (rare: a fly leg's counted try): the core's cast check of the fastest one
    uint32 const fastest = flying->second.rbegin()->second.front();
    SpellCastResult result = SPELL_FAILED_UNKNOWN;
    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(fastest))
    {
        Spell spell(bot, info, TRIGGERED_NONE);
        spell.m_targets.SetUnitTarget(bot);
        result = spell.CheckCast(true);
    }
    LOG_INFO("playerbots", "[Route] {} flying mount {} refused: cast result {}, moving {}, lost control {}",
             bot->GetName(), fastest, static_cast<uint32>(result), bot->isMoving(),
             bot->HasUnitState(UNIT_STATE_LOST_CONTROL));
    return "refused";
}

int32 CheckMountStateAction::CalculateMasterMountSpeed(Player* master) const
{
    // Check riding skill and level requirements
    int32 ridingSkill = bot->GetPureSkillValue(SKILL_RIDING);
    int32 botLevel = bot->GetLevel();

    if (ridingSkill <= 75 && botLevel < static_cast<int32>(sPlayerbotAIConfig.UseFastGroundMountAtMinLevel))
        return 59;

    // check if bot has master and if master is self
    bool const noRealMaster = (!master || master == bot);

    if (!noRealMaster && !bot->InBattleground())
    {
        auto auraEffects = master->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        if (!auraEffects.empty())
        {
            SpellInfo const* masterSpell = auraEffects.front()->GetSpellInfo();
            int32 effect1 = masterSpell->Effects[1].BasePoints;
            int32 effect2 = masterSpell->Effects[2].BasePoints;
            return std::max(effect1, effect2);
        }
        else if (masterInShapeshiftForm == FORM_FLIGHT_EPIC)
            return 279;
        else if (masterInShapeshiftForm == FORM_FLIGHT)
            return 149;
        return 59;  // walk pace
    }

    // No real master OR battleground: pick speed by skill tier.
    if (!bot->InBattleground() && BotCanUseFlyingMount(bot))
        return (ridingSkill >= 300) ? 279 : 149;

    int32 maxGround = (ridingSkill >= 150) ? 99 : 59;
    if (bot->InBattleground() && maxGround > 99)
        maxGround = 99;
    return maxGround;
}

uint32 CheckMountStateAction::GetMountType(Player* master) const
{
    bool const noRealMaster = (!master || master == bot);

    if (noRealMaster)
        return (!bot->InBattleground() && BotCanUseFlyingMount(bot)) ? 1 : 0;

    auto auraEffects = master->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    if (!auraEffects.empty())
    {
        SpellInfo const* masterSpell = auraEffects.front()->GetSpellInfo();
        return (masterSpell->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                masterSpell->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) ? 1 : 0;
    }
    else if (masterInShapeshiftForm == FORM_FLIGHT || masterInShapeshiftForm == FORM_FLIGHT_EPIC)
        return 1;

    return 0;
}
