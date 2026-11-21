/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgDoFish.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Bag.h"
#include "EventLog.h"
#include "FishRules.h"
#include "FishingAction.h"
#include "GameObject.h"
#include "Item.h"
#include "ItemPackets.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "PlannerFishing.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

// FishingAction.cpp: the height of fishable water at a point (checkForLand: of standable ground; INVALID_HEIGHT where
// there is none)
float HasFishableWaterOrLand(float x, float y, float z, Map* map, uint32 phaseMask, bool checkForLand);

namespace
{
// The fishing ranks, highest first (Spell.dbc, read 2026-11-22: the spell_ranks chain of 7620, Apprentice to Grand
// Master; each is a channel that summons the bobber 35591 (effect 50) and asks a fishing pole in hand, item class 2
// subclass mask 0x100000)
constexpr uint32 FISHING_RANKS[] = {51294, 33095, 18248, 7732, 7731, 7620};
constexpr uint32 BOBBER = 35591;
// The bobber lands 10 to 20 yd ahead of the caster (FishingAction.cpp's range): a stand is this far from its water
constexpr float CAST_MIN_YARDS = 10.0f;
constexpr float CAST_MAX_YARDS = 20.0f;
constexpr float STAND_MAX_YARDS = 17.5f;
constexpr float RING_STEP_YARDS = 2.5f;
constexpr int RING_POINTS = 32;
constexpr float STAND_ABOVE_WATER_YARDS = 8.0f;  // higher ground is a cliff over the water
constexpr float POOL_SEARCH_YARDS = 30.0f;       // a live pool this near the trip's place is fished
constexpr float WATER_SEARCH_YARDS = 40.0f;      // open water this near the place is walked to
constexpr float STAND_REACH_YARDS = 1.5f;
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

// Standable ground 10 to 17.5 yd from the water with the water in sight, ring by ring from the water outwards; the
// nearest to the bot first
std::vector<WorldPosition> Stands(Player* bot, WorldPosition const& water)
{
    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    std::vector<WorldPosition> points;
    for (float dist = CAST_MIN_YARDS; dist <= STAND_MAX_YARDS && points.empty(); dist += RING_STEP_YARDS)
        for (int i = 0; i < RING_POINTS; ++i)
        {
            float const angle = 2.0f * static_cast<float>(M_PI) * i / RING_POINTS;
            float const x = water.GetPositionX() + std::cos(angle) * dist;
            float const y = water.GetPositionY() + std::sin(angle) * dist;
            float const ground = HasFishableWaterOrLand(x, y, water.GetPositionZ(), map, phase, true);
            if (ground == INVALID_HEIGHT || ground > water.GetPositionZ() + STAND_ABOVE_WATER_YARDS)
                continue;
            if (!map->isInLineOfSight(x, y, ground + bot->GetCollisionHeight(), water.GetPositionX(),
                                      water.GetPositionY(), water.GetPositionZ() + 0.5f, phase, LINEOFSIGHT_ALL_CHECKS,
                                      VMAP::ModelIgnoreFlags::Nothing))
                continue;
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
    if (std::optional<bool> const away = Away(data.stand, STAND_REACH_YARDS))
        return *away;
    if (bot->isMoving())
        return false;
    // A stand in the water (the probe's ground was a shallow edge): the next stand
    if (bot->IsInWater())
    {
        data.stand = WorldPosition();
        if (data.probes >= MAX_PROBES)
        {
            Finish(NO_WATER);
            return true;
        }
        return true;
    }

    if (data.castMs)
        return Watch(data);
    if (!PoleInHand(data))
    {
        Finish(POLE_REFUSED);
        return true;
    }
    // The catch's loot is the pipeline's (StoreLootAction); one it left open is released before the next cast
    if (ObjectGuid const loot = bot->GetLootGUID(); !loot.IsEmpty())
    {
        if (!data.lootMs)
            data.lootMs = std::max<uint32>(getMSTime(), 1);
        if (GetMSTimeDiffToNow(data.lootMs) < LOOT_WAIT_MS)
            return true;
        bot->GetSession()->DoLootRelease(loot);
    }
    data.lootMs = 0;

    bot->SetFacingTo(bot->GetAngle(data.water.GetPositionX(), data.water.GetPositionY()));
    if (!botAI->CastSpell(data.spell, bot))
    {
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
    ++data.probes;
    data.refusedCasts = 0;
    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    float const sx = data.shore.GetPositionX();
    float const sy = data.shore.GetPositionY();
    float const sz = data.shore.GetPositionZ();
    WorldPosition water;
    data.pool = ObjectGuid::Empty;
    // A live pool near the place first (the sure catch, FishRules.h); a later probe tries open water
    if (data.probes == 1)
        if (GameObject* pool = NearestPool(botAI, data.shore))
        {
            water = WorldPosition(pool->GetMapId(), pool->GetPositionX(), pool->GetPositionY(), pool->GetPositionZ());
            data.pool = pool->GetGUID();
        }
    // The bot on dry ground at the place, open water in reach and in sight: it fishes where it stands
    if (!water.IsValid() && data.probes == 1 && !bot->IsInWater() &&
        bot->GetExactDist2d(sx, sy) < STAND_REACH_YARDS + 3.0f)
    {
        WorldPosition here =
            FindWaterRadial(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), map, phase,
                            CAST_MIN_YARDS, CAST_MAX_YARDS - RING_STEP_YARDS, RING_STEP_YARDS, true, RING_POINTS);
        if (here.IsValid())
        {
            data.water = here;
            data.stand = WorldPosition(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            data.chance = ChanceHere(bot, false);
            return true;
        }
    }
    // Else the place itself in the water, or the open water nearest it, and a stand on its shore
    if (!water.IsValid())
    {
        float const level = HasFishableWaterOrLand(sx, sy, sz, map, phase, false);
        water = level != INVALID_HEIGHT ? WorldPosition(data.shore.GetMapId(), sx, sy, level)
                                        : FindWaterRadial(bot, sx, sy, sz, map, phase, 0.0f, WATER_SEARCH_YARDS,
                                                          RING_STEP_YARDS, false, RING_POINTS);
    }
    if (!water.IsValid())
        return false;
    std::vector<WorldPosition> const stands = Stands(bot, water);
    if (stands.empty())
        return false;
    // A later probe takes the next stand of the ring (the nearest was refused or wet)
    data.stand = stands[std::min<std::size_t>(data.probes - 1, stands.size() - 1)];
    data.water = water;
    data.chance = ChanceHere(bot, !data.pool.IsEmpty());
    return true;
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
    Spell* const channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    bool const fishing = channel && channel->m_spellInfo && IsFishingRank(channel->m_spellInfo->Id);
    if (fishing)
        data.channelSeen = true;
    GameObject* const bobber = bot->GetGameObject(data.spell);
    if (bobber && bobber->GetEntry() == BOBBER && bobber->getLootState() == GO_READY)
    {
        // A bite: the core rolls the catch at the click and sends its loot to the bot (GameObject::Use)
        bobber->Use(bot);
        if (bot->GetLootGUID() == bobber->GetGUID())
        {
            ++data.catches;
            data.missesInARow = 0;
        }
        else
            ++data.missesInARow;
        data.castMs = 0;
        return true;
    }
    if (fishing && GetMSTimeDiffToNow(data.castMs) < CAST_STALL_MS)
        return true;
    if (!data.channelSeen && GetMSTimeDiffToNow(data.castMs) < CAST_START_MS)
        return true;
    // The channel ended without a bite (the fish got away), or the cast never started (no water ahead, refused)
    if (data.channelSeen)
        ++data.missesInARow;
    else if (++data.refusedCasts >= MAX_REFUSED_CASTS)
        data.stand = WorldPosition();
    data.castMs = 0;
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
        if (GameObject* go = botAI->GetGameObject(loot); go && go->GetEntry() == BOBBER)
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
