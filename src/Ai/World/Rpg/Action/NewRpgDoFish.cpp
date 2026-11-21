/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgDoFish.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

#include "Bag.h"
#include "CheckMountStateAction.h"
#include "EventLog.h"
#include "FishRules.h"
#include "GameObject.h"
#include "GoToRules.h"
#include "Item.h"
#include "ItemPackets.h"
#include "Map.h"
#include "MeshMoveRules.h"
#include "MountRules.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
#include "PlannerFishing.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
// The fishing ranks, highest first (Spell.dbc, read 2026-11-22: the spell_ranks chain of 7620, Apprentice to Grand
// Master; each is a channel that summons the bobber 35591 (effect 50) and asks a fishing pole in hand, item class 2
// subclass mask 0x100000)
constexpr uint32 FISHING_RANKS[] = {51294, 33095, 18248, 7732, 7731, 7620};
constexpr uint32 BOBBER = 35591;
// The bobber lands 10 to 20 yd ahead of the caster, within 17.5 degrees of its facing (Spell.cpp,
// TARGET_DEST_CASTER_FISHING; the spells' range), where the liquid must lie more than 0.75 yd above the ground (else
// TOO_SHALLOW or NOT_HERE) and in sight. A stand is a point of dry ground whose cast sector is water: SECTOR_WET of
// the nine points at 11, 15 and 19 yd and -15, 0 and 15 degrees, the middle one among them, in sight
constexpr float CAST_MIN_YARDS = 10.0f;
constexpr float STAND_MAX_YARDS = 17.5f;
constexpr float SECTOR_YARDS[] = {11.0f, 15.0f, 19.0f};
constexpr float SECTOR_RADIANS = 15.0f * static_cast<float>(M_PI) / 180.0f;
constexpr uint32 SECTOR_WET = 7;
constexpr float FISHABLE_DEPTH = 1.0f;  // the core's 0.75 yd and a margin
constexpr float DRY_WADE_YARDS = 0.2f;  // a stand's ground lies at most this far under a liquid's surface
constexpr float HEIGHT_SEARCH_YARDS = 10.0f;
constexpr float RING_STEP_YARDS = 2.5f;
constexpr int RING_POINTS = 32;
constexpr int HERE_DIRECTIONS = 16;
constexpr float STAND_ABOVE_WATER_YARDS = 8.0f;          // higher ground is a cliff over the water
constexpr float POOL_SEARCH_YARDS = 30.0f;               // a live pool this near the trip's place is fished
constexpr float WATER_SEARCH_YARDS = 40.0f;              // open water this near the place is walked to
constexpr float OFFSHORE_YARDS[] = {0.0f, 5.0f, 10.0f};  // open water: the nearest point and further out
// A walk ends this near its stand (the mesh may end a step short of a probed point): the cast sector is judged again
// where the bot stands before every cast. A stand the probe set where the mesh's path to it ends (MeshStand) is walked
// to all the way: a bot 2.8 yd from its stand did not walk and found the sector dry where it stood (WR16b run 6)
constexpr float STAND_REACH_YARDS = 4.0f;
constexpr float MESH_REACH_YARDS = 0.75f;  // at 1.4 yd from such a stand the sector was dry (WR16b run 7)
constexpr float MESH_JUDGE_YARDS = 60.0f;  // a stand this near the bot is judged from its path's end
constexpr uint32 MESH_TRIES = 4;           // stands judged by their path's end per probe
constexpr float HERE_YARDS = 4.0f;         // a bot this near the place tries to fish where it stands
constexpr float ON_FOOT_YARDS = 20.0f;     // a bot this near its stand walks on (KeepsOnFoot)
constexpr uint32 MAX_PROBES = 3;
constexpr uint32 MAX_REFUSED_CASTS = 3;
constexpr uint32 CAST_START_MS = 2 * IN_MILLISECONDS;   // a cast that started no channel by then was refused
constexpr uint32 CAST_STALL_MS = 30 * IN_MILLISECONDS;  // longer than the channel (21 s): the cast is over
constexpr uint32 LOOT_WAIT_MS = 5 * IN_MILLISECONDS;    // the loot pipeline's time to store a catch
// The trip's own end words (the FishTrip row's source and the Status row's word); TripEnd's by FishRules.h's order
constexpr char const* TRIP_END_WORDS[Fish::TRIP_ENDS] = {"none", "fight", "bags_full", "time_up", "barren"};
constexpr char const* NO_SKILL = "no_skill";
constexpr char const* NO_POLE = "no_pole";
constexpr char const* POLE_REFUSED = "pole_refused";
constexpr char const* NO_WATER = "no_water";
constexpr char const* MAP_CHANGED = "map_changed";
constexpr char const* DEADLINE = "deadline";

bool IsPole(Item const* item)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    return proto && proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
}

bool IsFishingRank(uint32 spell)
{
    return std::find(std::begin(FISHING_RANKS), std::end(FISHING_RANKS), spell) != std::end(FISHING_RANKS);
}

Item* MainHand(Player* bot) { return bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND); }

// A pole in the hand or the bags
Item* FindPole(Player* bot)
{
    if (Item* worn = MainHand(bot); IsPole(worn))
        return worn;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); IsPole(item))
            return item;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* container = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                if (Item* item = container->GetItemByPos(slot); IsPole(item))
                    return item;
    return nullptr;
}

// The client's own equip packet (FishingAction.cpp's EquipFishingPoleAction): a swap with what the slot holds
void Equip(Player* bot, Item* item, uint8 slot)
{
    WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 9);
    packet << item->GetGUID() << slot;
    WorldPackets::Item::AutoEquipItemSlot equip(std::move(packet));
    equip.Read();
    bot->GetSession()->HandleAutoEquipItemSlotOpcode(equip);
}

// The core's test of a bobber's landing point: the liquid's level where it lies more than FISHABLE_DEPTH above the
// ground, else INVALID_HEIGHT
float FishableLevel(Map* map, uint32 phase, float x, float y, float z)
{
    float const ground = map->GetHeight(phase, x, y, z + HEIGHT_SEARCH_YARDS, true);
    if (ground <= INVALID_HEIGHT)
        return INVALID_HEIGHT;
    LiquidData const& liquid = map->GetLiquidData(phase, x, y, ground, DEFAULT_COLLISION_HEIGHT, {});
    if (liquid.Status == LIQUID_MAP_NO_WATER || liquid.Level < ground + FISHABLE_DEPTH)
        return INVALID_HEIGHT;
    return liquid.Level;
}

// Dry ground at a point (no liquid, or one the ground reaches): its height, else INVALID_HEIGHT
float DryGround(Map* map, uint32 phase, float x, float y, float z)
{
    float const ground = map->GetHeight(phase, x, y, z + HEIGHT_SEARCH_YARDS, true);
    if (ground <= INVALID_HEIGHT)
        return INVALID_HEIGHT;
    LiquidData const& liquid = map->GetLiquidData(phase, x, y, ground, DEFAULT_COLLISION_HEIGHT, {});
    if (liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > ground + DRY_WADE_YARDS)
        return INVALID_HEIGHT;
    return ground;
}

// The cast sector from a stand at a facing: its wet points of the nine, 0 when the middle one is dry or out of sight
// or too many are dry; the middle one's water as `aim`
uint32 SectorWet(Player* bot, float x, float y, float z, float facing, WorldPosition& aim)
{
    constexpr uint32 POINTS = 9;
    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    float const mx = x + std::cos(facing) * SECTOR_YARDS[1];
    float const my = y + std::sin(facing) * SECTOR_YARDS[1];
    float const level = FishableLevel(map, phase, mx, my, z);
    if (level == INVALID_HEIGHT ||
        !map->isInLineOfSight(x, y, z + bot->GetCollisionHeight(), mx, my, level + 0.5f, phase, LINEOFSIGHT_ALL_CHECKS,
                              VMAP::ModelIgnoreFlags::Nothing))
        return 0;
    aim = WorldPosition(bot->GetMapId(), mx, my, level);
    uint32 wet = 1;
    uint32 dry = 0;
    for (float const yards : SECTOR_YARDS)
        for (float const offset : {-SECTOR_RADIANS, 0.0f, SECTOR_RADIANS})
        {
            if (offset == 0.0f && yards == SECTOR_YARDS[1])
                continue;
            float const px = x + std::cos(facing + offset) * yards;
            float const py = y + std::sin(facing + offset) * yards;
            if (FishableLevel(map, phase, px, py, z) != INVALID_HEIGHT)
                ++wet;
            else if (++dry > POINTS - SECTOR_WET)
                return 0;
        }
    return wet;
}

// What a probe's rings held (the test trace)
struct StandCounts
{
    uint32 points{0};
    uint32 dry{0};
    uint32 wet{0};
};

// Stands for a water point: dry ground 10 to 17.5 yd from it whose cast sector towards it is water, ring by ring from
// the water outwards (the first ring with any); the nearest to the bot first
std::vector<WorldPosition> Stands(Player* bot, WorldPosition const& water, StandCounts& counts)
{
    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    std::vector<WorldPosition> points;
    for (float dist = CAST_MIN_YARDS; dist <= STAND_MAX_YARDS && points.empty(); dist += RING_STEP_YARDS)
        for (int i = 0; i < RING_POINTS; ++i)
        {
            ++counts.points;
            float const angle = 2.0f * static_cast<float>(M_PI) * i / RING_POINTS;
            float const x = water.GetPositionX() + std::cos(angle) * dist;
            float const y = water.GetPositionY() + std::sin(angle) * dist;
            float const ground = DryGround(map, phase, x, y, water.GetPositionZ());
            if (ground == INVALID_HEIGHT || ground > water.GetPositionZ() + STAND_ABOVE_WATER_YARDS)
                continue;
            ++counts.dry;
            WorldPosition aim;
            float const facing = std::atan2(water.GetPositionY() - y, water.GetPositionX() - x);
            if (SectorWet(bot, x, y, ground, facing, aim) < SECTOR_WET)
                continue;
            ++counts.wet;
            points.emplace_back(bot->GetMapId(), x, y, ground);
        }
    std::sort(points.begin(), points.end(),
              [bot](WorldPosition const& a, WorldPosition const& b)
              {
                  return bot->GetExactDist2d(a.GetPositionX(), a.GetPositionY()) <
                         bot->GetExactDist2d(b.GetPositionX(), b.GetPositionY());
              });
    return points;
}

// The open water nearest a point: the middle fishable point of the first ring that has one, within
// WATER_SEARCH_YARDS; its bearing from the point in `bearing`
WorldPosition NearestWater(Player* bot, WorldPosition const& from, float& bearing)
{
    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    for (float dist = RING_STEP_YARDS; dist <= WATER_SEARCH_YARDS; dist += RING_STEP_YARDS)
    {
        std::vector<std::pair<WorldPosition, float>> ring;
        for (int i = 0; i < RING_POINTS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / RING_POINTS;
            float const x = from.GetPositionX() + std::cos(angle) * dist;
            float const y = from.GetPositionY() + std::sin(angle) * dist;
            float const level = FishableLevel(map, phase, x, y, from.GetPositionZ());
            if (level != INVALID_HEIGHT)
                ring.emplace_back(WorldPosition(bot->GetMapId(), x, y, level), angle);
        }
        if (!ring.empty())
        {
            bearing = ring[ring.size() / 2].second;
            return ring[ring.size() / 2].first;
        }
    }
    return WorldPosition();
}

// The live fishing pool nearest the place, within POOL_SEARCH_YARDS
GameObject* NearestPool(PlayerbotAI* botAI, WorldPosition const& place)
{
    GameObject* nearest = nullptr;
    float best = POOL_SEARCH_YARDS;
    for (ObjectGuid const& guid :
         botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects no los")->Get())
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || go->GetGoType() != GAMEOBJECT_TYPE_FISHINGHOLE || !go->isSpawned())
            continue;
        float const dist = go->GetExactDist2d(place.GetPositionX(), place.GetPositionY());
        if (dist < best)
        {
            best = dist;
            nearest = go;
        }
    }
    return nearest;
}

// The core's catch chance of the bot at its place (FishRules.h; 1 when the fishing index does not know the area)
float ChanceHere(Player* bot, bool atPool)
{
    std::shared_ptr<Fish::Index const> const index = Planner::CurrentFishIndex();
    Fish::Water const* water = index ? index->Find(bot->GetAreaId(), bot->GetZoneId()) : nullptr;
    if (!water)
        return 1.0f;
    return static_cast<float>(Fish::CatchChance(bot->GetSkillValue(SKILL_FISHING), water->baseSkill, atPool));
}

// Where the walk to a stand ends: the mesh's path end from the bot, when the bot is near and the path carries it to
// within STAND_REACH_YARDS (GoTo::Carried, the walk's own judge). The cast sector is judged from there: false when it
// is dry (the stand is not taken). A stand the mesh does not carry the bot to stays as probed: the walk judges it
bool MeshStand(Player* bot, WorldPosition& stand, WorldPosition const& water, bool& onMesh)
{
    onMesh = false;
    if (bot->GetExactDist2d(stand.GetPositionX(), stand.GetPositionY()) > MESH_JUDGE_YARDS)
        return true;
    PathGenerator path(bot);
    path.CalculatePath(stand.GetPositionX(), stand.GetPositionY(), stand.GetPositionZ());
    G3D::Vector3 const& end = path.GetActualEndPosition();
    if (!GoTo::Carried(static_cast<uint32>(path.GetPathType()), MeshMove::LastStep(path.GetPath()),
                       stand.GetExactDist(end.x, end.y, end.z), STAND_REACH_YARDS))
        return true;
    WorldPosition aim;
    float const facing = std::atan2(water.GetPositionY() - end.y, water.GetPositionX() - end.x);
    if (SectorWet(bot, end.x, end.y, end.z, facing, aim) < SECTOR_WET)
        return false;
    stand = WorldPosition(bot->GetMapId(), end.x, end.y, end.z);
    onMesh = true;
    return true;
}

// A test bot's watching tick: the longest gap between two of them while the cast is out
void NoteWatchTick(PlayerbotAI* botAI, NewRpgInfo::DoFish& data)
{
    if (!botAI->testConstraints)
        return;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (data.watchMs)
        data.watchGapMs = std::max(data.watchGapMs, getMSTimeDiff(data.watchMs, now));
    data.watchMs = now;
}

// A test bot's line at each cast without a catch: how it ended and the longest gap between two watching ticks
void TraceMiss(PlayerbotAI* botAI, NewRpgInfo::DoFish const& data, char const* how, GameObject const* bobber)
{
    if (!botAI->testConstraints)
        return;
    LOG_INFO("playerbots",
             "[Probe] {} fish miss {}: {} ms since the cast, longest tick gap {} ms, bobber {}, pool {}, casts {}",
             botAI->GetBot()->GetName(), how, GetMSTimeDiffToNow(data.castMs), data.watchGapMs,
             bobber ? static_cast<int32>(bobber->getLootState()) : -1, !data.pool.IsEmpty(), data.casts);
}

// The cast is over: the next tick casts again
void CastOver(NewRpgInfo::DoFish& data)
{
    data.castMs = 0;
    data.watchMs = 0;
    data.watchGapMs = 0;
}

// The bot's bobber when a fish bites (GO_READY), else nullptr
GameObject* Bite(Player* bot, NewRpgInfo::DoFish const& data)
{
    GameObject* const bobber = bot->GetGameObject(data.spell);
    return bobber && bobber->GetEntry() == BOBBER && bobber->getLootState() == GO_READY ? bobber : nullptr;
}

// A bite clicked: the core rolls the catch at the click and opens its loot to the bot (GameObject::Use): the bobber's,
// or at a pool the pool's (a catch there is sure); no loot is a fish that got away
void Click(PlayerbotAI* botAI, NewRpgInfo::DoFish& data, GameObject* bobber)
{
    Player* const bot = botAI->GetBot();
    bobber->Use(bot);
    if (!bot->GetLootGUID().IsEmpty())
    {
        ++data.catches;
        data.missesInARow = 0;
    }
    else
    {
        TraceMiss(botAI, data, "no_loot", bobber);
        ++data.missesInARow;
    }
    CastOver(data);
}
}  // namespace

bool NewRpgDoFishAction::Execute(Event /*event*/)
{
    auto* dataPtr = std::get_if<NewRpgInfo::DoFish>(&botAI->rpgInfo.data);
    if (!dataPtr || !bot->IsAlive() || bot->IsInCombat())
        return false;
    NewRpgInfo::DoFish& data = *dataPtr;

    if (bot->GetMapId() != data.shore.GetMapId())
    {
        Finish(MAP_CHANGED);
        return true;
    }
    // The first tick: the skill, the rank it casts and a pole
    if (!data.spell)
    {
        for (uint32 rank : FISHING_RANKS)
            if (bot->HasSpell(rank))
            {
                data.spell = rank;
                break;
            }
        if (!data.spell || !bot->GetSkillValue(SKILL_FISHING))
        {
            Finish(NO_SKILL);
            return true;
        }
        if (!FindPole(bot))
        {
            Finish(NO_POLE);
            return true;
        }
    }
    // The trip's own ends (FishRules.h): full bags, its time, a water that gives nothing; a fight ends it by OnCombat
    Fish::Trip trip;
    trip.minutes = GetMSTimeDiffToNow(botAI->rpgInfo.startT) / static_cast<float>(MINUTE * IN_MILLISECONDS);
    if (data.planMs)
        trip.planMinutes = data.planMs / static_cast<float>(MINUTE * IN_MILLISECONDS);
    trip.bagShare = AI_VALUE(uint8, "bag space") / 100.0;
    trip.missesInARow = data.missesInARow;
    trip.chance = data.chance;
    if (Fish::TripEnd const end = Fish::Ends(trip); end != Fish::TripEnd::None && !data.castMs)
    {
        Finish(TRIP_END_WORDS[static_cast<std::size_t>(end)]);
        return true;
    }

    if (!data.stand.IsValid() && !Probe(data))
    {
        Finish(NO_WATER);
        return true;
    }
    if (std::optional<bool> const away = Away(data.stand, data.standOnMesh ? MESH_REACH_YARDS : STAND_REACH_YARDS))
    {
        Trace(data, *away ? "walk" : "walk unspent");
        return *away;
    }
    if (bot->isMoving())
    {
        Trace(data, "moving");
        return false;
    }
    if (data.castMs)
        return Watch(data);
    // The catch's loot is the pipeline's (StoreLootAction); one it left open is released before the next cast
    if (ObjectGuid const loot = bot->GetLootGUID(); !loot.IsEmpty())
    {
        if (!data.lootMs)
            data.lootMs = std::max<uint32>(getMSTime(), 1);
        if (GetMSTimeDiffToNow(data.lootMs) < LOOT_WAIT_MS)
        {
            Trace(data, "loot");
            return true;
        }
        bot->GetSession()->DoLootRelease(loot);
    }
    data.lootMs = 0;

    // Where the walk ended: in the water, or the cast sector towards the water not wet from here: the next stand
    WorldPosition aim;
    float const facing = bot->GetAngle(data.water.GetPositionX(), data.water.GetPositionY());
    if (bot->IsInWater() ||
        SectorWet(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), facing, aim) < SECTOR_WET)
    {
        Trace(data, bot->IsInWater() ? "stand in water" : "stand sector dry");
        data.stand = WorldPosition();
        if (data.probes >= MAX_PROBES)
            Finish(NO_WATER);
        return true;
    }
    // Fishing is not cast mounted or in a shapeshift (a trip arrives mounted); the mount policy keeps the bot on foot
    // from here (KeepsOnFoot). The one dismount request stops the bot too
    if (NewRpgHelpers::RealShapeshift(bot))
    {
        Trace(data, "shapeshift");
        botAI->RemoveShapeshift();
        return true;
    }
    if (bot->IsMounted())
    {
        Trace(data, "dismount");
        CheckMountStateAction::Dismount(bot, MountRules::How::Plain);
        return true;
    }
    if (!PoleInHand(data))
    {
        Finish(POLE_REFUSED);
        return true;
    }
    bot->SetFacingTo(facing);
    if (!botAI->CastSpell(data.spell, bot))
    {
        Trace(data, "cast refused");
        if (++data.refusedCasts >= MAX_REFUSED_CASTS)
            data.stand = WorldPosition();
        return true;
    }
    data.castMs = std::max<uint32>(getMSTime(), 1);
    data.channelSeen = false;
    ++data.casts;
    return true;
}

bool NewRpgDoFishAction::Probe(NewRpgInfo::DoFish& data)
{
    if (data.probes >= MAX_PROBES)
        return false;
    // A later probe takes a later stand of the same order (the one before was refused or did not hold)
    uint32 const skip = data.probes++;
    data.refusedCasts = 0;
    data.pool = ObjectGuid::Empty;
    auto const started = std::chrono::steady_clock::now();
    StandCounts counts;
    std::size_t passed = 0;  // stands of the ways before
    char const* way = nullptr;
    uint32 meshDry = 0;  // stands whose path's end has a dry sector
    auto const take = [&](std::vector<WorldPosition> const& stands, WorldPosition const& water)
    {
        if (skip >= passed + stands.size())
        {
            passed += stands.size();
            return false;
        }
        // Spread over the list: the next probe's stand lies apart from the refused one; from there the first whose
        // walk ends with water in its sector (MeshStand). A way after one whose stands all failed starts at its first
        std::size_t const step = std::max<std::size_t>(stands.size() / MAX_PROBES, 1);
        std::size_t const first = std::min((skip > passed ? skip - passed : 0) * step, stands.size() - 1);
        for (std::size_t i = first; i < stands.size() && i < first + MESH_TRIES; ++i)
        {
            WorldPosition stand = stands[i];
            if (!MeshStand(bot, stand, water, data.standOnMesh))
            {
                ++meshDry;
                continue;
            }
            data.stand = stand;
            data.water = water;
            return true;
        }
        passed += stands.size();
        return false;
    };
    // A live pool near the place first (the sure catch, FishRules.h)
    if (GameObject* pool = NearestPool(botAI, data.shore))
    {
        WorldPosition const water(pool->GetMapId(), pool->GetPositionX(), pool->GetPositionY(), pool->GetPositionZ());
        if (take(Stands(bot, water, counts), water))
        {
            data.pool = pool->GetGUID();
            way = "pool";
        }
    }
    // The bot on dry ground at the place with water in its cast sector: it fishes where it stands
    if (!way && !bot->IsInWater() &&
        bot->GetExactDist2d(data.shore.GetPositionX(), data.shore.GetPositionY()) < HERE_YARDS)
        for (int i = 0; i < HERE_DIRECTIONS; ++i)
        {
            WorldPosition aim;
            float const facing = 2.0f * static_cast<float>(M_PI) * i / HERE_DIRECTIONS;
            if (SectorWet(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), facing, aim) < SECTOR_WET)
                continue;
            if (take({WorldPosition(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())},
                     aim))
            {
                way = "here";
                data.standOnMesh = true;  // where it stands
            }
            break;
        }
    // Else the open water nearest the place, and further out the same way, with a stand on its shore
    if (!way)
    {
        float bearing = 0.0f;
        WorldPosition nearest = NearestWater(bot, data.shore, bearing);
        if (nearest.IsValid())
            for (float const out : OFFSHORE_YARDS)
            {
                float const x = nearest.GetPositionX() + std::cos(bearing) * out;
                float const y = nearest.GetPositionY() + std::sin(bearing) * out;
                float const level = FishableLevel(bot->GetMap(), bot->GetPhaseMask(), x, y, nearest.GetPositionZ());
                if (level == INVALID_HEIGHT)
                    continue;
                if (take(Stands(bot, WorldPosition(bot->GetMapId(), x, y, level), counts),
                         WorldPosition(bot->GetMapId(), x, y, level)))
                {
                    way = "open";
                    break;
                }
            }
    }
    if (botAI->testConstraints)
        LOG_INFO(
            "playerbots",
            "[Probe] {} fish probe {}: {}, ring points {}, dry {}, wet {}, path end dry {}, on mesh {}, {} us",
            bot->GetName(), data.probes, way ? way : "none", counts.points, counts.dry, counts.wet, meshDry,
            data.standOnMesh,
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    if (!way)
        return false;
    data.chance = ChanceHere(bot, !data.pool.IsEmpty());
    return true;
}

bool NewRpgDoFishAction::KeepsOnFoot(PlayerbotAI* botAI, Player* bot)
{
    auto const* data = std::get_if<NewRpgInfo::DoFish>(&botAI->rpgInfo.data);
    return data && !(data->stand == WorldPosition()) && data->stand.GetMapId() == bot->GetMapId() &&
           bot->GetExactDist2d(data->stand.GetPositionX(), data->stand.GetPositionY()) < ON_FOOT_YARDS;
}

void NewRpgDoFishAction::Trace(NewRpgInfo::DoFish const& data, char const* where)
{
    if (!botAI->testConstraints || where == traced)
        return;
    traced = where;
    LOG_INFO(
        "playerbots",
        "[Probe] {} fish wait {}: at ({:.1f}, {:.1f}, {:.1f}), stand ({:.1f}, {:.1f}, {:.1f}) {:.1f} yd, moving {}, "
        "mounted {}, shapeshifted {}, loot {}, casts {}",
        bot->GetName(), where, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), data.stand.GetPositionX(),
        data.stand.GetPositionY(), data.stand.GetPositionZ(),
        bot->GetExactDist(data.stand.GetPositionX(), data.stand.GetPositionY(), data.stand.GetPositionZ()),
        bot->isMoving(), bot->IsMounted(), static_cast<uint32>(bot->GetShapeshiftForm()), !bot->GetLootGUID().IsEmpty(),
        data.casts);
}

bool NewRpgDoFishAction::PoleInHand(NewRpgInfo::DoFish& data)
{
    Item* const worn = MainHand(bot);
    if (IsPole(worn))
        return true;
    Item* const pole = FindPole(bot);
    if (!pole)
        return false;
    // What the pole replaces, remembered once (a two-handed pole sends the off-hand to the bags)
    if (!data.poleOut)
    {
        data.weapon = worn ? worn->GetGUID() : ObjectGuid::Empty;
        Item* const off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        data.offhand = off ? off->GetGUID() : ObjectGuid::Empty;
    }
    Equip(bot, pole, EQUIPMENT_SLOT_MAINHAND);
    data.poleOut = true;
    return IsPole(MainHand(bot));
}

bool NewRpgDoFishAction::Watch(NewRpgInfo::DoFish& data)
{
    NoteWatchTick(botAI, data);
    Spell* const channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    bool const fishing = channel && channel->m_spellInfo && IsFishingRank(channel->m_spellInfo->Id);
    if (fishing)
        data.channelSeen = true;
    if (GameObject* const bobber = Bite(bot, data))
    {
        Click(botAI, data, bobber);
        return true;
    }
    if (fishing && GetMSTimeDiffToNow(data.castMs) < CAST_STALL_MS)
        return true;
    if (!data.channelSeen && GetMSTimeDiffToNow(data.castMs) < CAST_START_MS)
        return true;
    // The channel ended without a bite (the fish got away), or the cast never started (no water ahead, refused)
    if (data.channelSeen)
    {
        TraceMiss(botAI, data, "got_away", bot->GetGameObject(data.spell));
        ++data.missesInARow;
    }
    else
    {
        TraceMiss(botAI, data, "no_channel", nullptr);
        if (++data.refusedCasts >= MAX_REFUSED_CASTS)
            data.stand = WorldPosition();
    }
    CastOver(data);
    return true;
}

bool NewRpgDoFishAction::HoldFishChannel(PlayerbotAI* botAI)
{
    auto* const data = std::get_if<NewRpgInfo::DoFish>(&botAI->rpgInfo.data);
    if (!data || !data->castMs)
        return false;
    Player* const bot = botAI->GetBot();
    Spell* const channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (!channel || !channel->m_spellInfo || !IsFishingRank(channel->m_spellInfo->Id) || bot->IsInCombat())
        return false;
    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NewRpgDoFish::HoldChannel")
                                                    : nullptr);
    data->channelSeen = true;
    NoteWatchTick(botAI, *data);
    if (GameObject* const bobber = Bite(bot, *data))
        Click(botAI, *data, bobber);
    return true;
}

void NewRpgDoFishAction::Finish(char const* word)
{
    RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = word});
}

void NewRpgDoFishAction::EndTrip(PlayerbotAI* botAI, NewRpgInfo::DoFish const& data, StatusEnd::Outcome outcome,
                                 char const* word)
{
    Player* const bot = botAI->GetBot();
    if (!bot)
        return;
    if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        channel && channel->m_spellInfo && IsFishingRank(channel->m_spellInfo->Id))
        bot->InterruptSpell(CURRENT_CHANNELED_SPELL);
    if (ObjectGuid const loot = bot->GetLootGUID(); loot.IsGameObject())
        if (GameObject* go = botAI->GetGameObject(loot);
            go && (go->GetEntry() == BOBBER || go->GetGoType() == GAMEOBJECT_TYPE_FISHINGHOLE))
            bot->GetSession()->DoLootRelease(loot);
    // The weapon back in hand (it swaps the pole into the bags), then the off-hand; a bot that held no weapon puts
    // the pole away. In a fight the engine's weapon-change timer may refuse the off-hand: it stays in the bags
    bool weaponBack = !data.poleOut;
    if (data.poleOut && IsPole(MainHand(bot)))
    {
        if (Item* weapon = data.weapon.IsEmpty() ? nullptr : bot->GetItemByGuid(data.weapon))
            Equip(bot, weapon, EQUIPMENT_SLOT_MAINHAND);
        else if (Item* pole = MainHand(bot))
        {
            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, pole, false) == EQUIP_ERR_OK)
            {
                bot->RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND, true);
                bot->StoreItem(dest, pole, true);
            }
        }
        if (Item* off = data.offhand.IsEmpty() ? nullptr : bot->GetItemByGuid(data.offhand); off && off->IsInBag())
            Equip(bot, off, EQUIPMENT_SLOT_OFFHAND);
        weaponBack = !IsPole(MainHand(bot));
    }
    char const* const source = outcome == StatusEnd::Outcome::TimedOut ? DEADLINE : word;
    uint32 const area = bot->GetAreaId();
    LOG_INFO("playerbots",
             "[Professions] {} fishing trip {}: area {}, {} casts, {} catches, {} at a pool, chance {:.2f}, weapon "
             "back {}, {} s",
             bot->GetName(), source, area, data.casts, data.catches, data.pool.IsEmpty() ? "not" : "", data.chance,
             weaponBack ? 1 : 0, GetMSTimeDiffToNow(botAI->rpgInfo.startT) / IN_MILLISECONDS);
    EventLog::Emit(bot, EventLog::EventType::FishTrip, source, area, data.catches);
}

void NewRpgDoFishAction::OnCombat(PlayerbotAI* botAI)
{
    if (std::holds_alternative<NewRpgInfo::DoFish>(botAI->rpgInfo.data))
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned,
                       {.word = TRIP_END_WORDS[static_cast<std::size_t>(Fish::TripEnd::Fight)]});
}
