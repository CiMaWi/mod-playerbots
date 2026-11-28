/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LootAction.h"

#include <algorithm>
#include <optional>

#include "BankRulesInput.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "CheckMountStateAction.h"
#include "Event.h"
#include "EventLog.h"
#include "GatherPassBy.h"
#include "GuildMgr.h"
#include "GuildTaskMgr.h"
#include "InventorySlots.h"
#include "ItemDemand.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "LootObjectStack.h"
#include "LootStrategyValue.h"
#include "MountRules.h"
#include "NewRpgDoGather.h"
#include "NewRpgTriggers.h"
#include "PlannerService.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "ServerFacade.h"
#include "TestConstraints.h"
#include "TravelLegRules.h"
#include "UseItemAction.h"
#include "WorldTravelConfig.h"

namespace
{
// ItemLimitCategory.dbc's "Mana Gem": the mage's six conjured mana gems (and the unused 36799), nothing else
constexpr uint32 MANA_GEM_LIMIT_CATEGORY = 4;
// From the autostore requests to the bot's next check (questing WQ55)
constexpr uint32 LOOT_TAKE_MS = 300;

// Slots the stored loot newly takes; merging into a stack takes none
float NewSlotsFor(Player* bot, ItemPosCountVec const& dest)
{
    float slots = 0.0f;
    for (ItemPosCount const& position : dest)
        if (!bot->GetItemByPos(position.pos))
            slots += 1.0f;

    return slots;
}

// Full bags of a persistent bot: destroys the lowest-worth item in a general slot when loot worth lootWorth beats it
// (WorldTravel::ShouldDestroyFor) and records an Inventory event; true when a slot was freed
bool DestroyForLoot(PlayerbotAI* botAI, AiObjectContext* context, uint32 lootWorth)
{
    if (!TestConstraints::DestroysForSpace(botAI->testConstraints))
        return false;

    Player* bot = botAI->GetBot();
    // Gear kept for a later level counts at its later worth, from what the bags hold now
    BankMail::RefreshCarriedKeeps(botAI);
    CollectItemsVisitor visitor;
    // The bags' items in InventoryAction::IterateItemsInBags' order
    for (uint32 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            visitor.Visit(item);
    for (uint32 i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            visitor.Visit(item);
    for (uint32 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        if (Bag* bag = bot->GetBagByPos(i))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                if (Item* item = bag->GetItemByPos(j))
                    visitor.Visit(item);

    float const sellMultiplier = static_cast<float>(sRandomPlayerbotMgr.GetSellMultiplier(bot));
    std::vector<WorldTravel::SpaceCandidate> candidates;
    candidates.reserve(visitor.items.size());
    uint32 sellableSlots = 0;
    for (Item* item : visitor.items)
    {
        WorldTravel::SpaceCandidate candidate;
        // A conjured food, water, healthstone or mana gem the bot can use is kept: worth nothing at a vendor, it
        // would be the first victim, and its caster conjures it again as soon as a slot is free (NR6); an unusable
        // one may go. Mana gems carry no conjured flag; their limit category (ItemLimitCategory.dbc 4) holds them all
        ItemTemplate const* proto = item->GetTemplate();
        bool const conjured = proto->IsConjuredConsumable() || proto->ItemLimitCategory == MANA_GEM_LIMIT_CATEGORY;
        candidate.Protected = !WorldTravel::InGeneralSlot(bot, item) || PvpGearMgr::Instance().IsWornItem(bot, item) ||
                              (conjured && bot->CanUseItem(proto) == EQUIP_ERR_OK);
        if (!candidate.Protected)
        {
            ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
            candidate.Worth = ItemUsageValue::ItemWorth(
                item->GetTemplate(), usage, item->GetCount(), sellMultiplier,
                BankMail::CarriedWorthEach(botAI, item->GetEntry()), ItemUse::DemandWorthEach(botAI, item->GetEntry()),
                usage == ITEM_USAGE_DISENCHANT
                    ? ItemUsageValue::DisenchantOutletOf(botAI, item->GetTemplate()).worthEach
                    : 0);
            if (WorldTravel::CountsAsSellable(item->GetTemplate()->Quality == ITEM_QUALITY_POOR,
                                              usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH))
                ++sellableSlots;
        }

        candidates.push_back(candidate);
    }

    int32 const victim = WorldTravel::LowestWorthVictim(candidates);
    if (victim == WorldTravel::NO_VICTIM)
        return false;

    WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    WorldTravel::DestroyForSpaceInput input;
    input.InInstance = bot->GetMap()->Instanceable();
    // The errand rule with the vendor of the bot's last errand check (when on this map), walked from here
    if (!input.InInstance && measure.HasVendor && measure.VendorMapId == bot->GetMapId())
    {
        WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
        float const walkMinutes = WorldTravel::WalkMinutes(
            bot->GetExactDist(measure.VendorX, measure.VendorY, measure.VendorZ), bot->GetSpeed(MOVE_RUN));
        input.TripWorthIt = WorldTravel::LootWaitsForTrip(
            WorldTravel::ErrandMayStart(botAI), measure, AI_VALUE(uint8, "bag space"), getMSTime(),
            WorldTravel::MakeTripInput(measure, config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())],
                                       WorldTravel::FreeGeneralSlots(bot), sellableSlots, walkMinutes,
                                       config.ErrandNextStopMinutes));
    }
    input.LootWorth = lootWorth;
    input.VictimWorth = candidates[victim].Worth;
    input.MarginPercent = sPlayerbotAIConfig.WorldTravel.DestroyMarginPercent;
    if (!WorldTravel::ShouldDestroyFor(input))
        return false;

    uint32 const itemId = visitor.items[victim]->GetEntry();
    if (PvpGearMgr::Instance().IsWornItem(bot, visitor.items[victim]))  // a worn PvP item or a displaced PvE item
        return false;

    std::ostringstream out;
    out << botAI->GetChatHelper()->FormatItem(visitor.items[victim]->GetTemplate()) << " destroyed";
    botAI->TellMaster(out);
    // Destroyed units are no use: the use measure does not count them (NE8a)
    uint16 const nonUse = Planner::NonUseKindsOf(botAI, visitor.items[victim]->GetTemplate());
    uint32 const units = visitor.items[victim]->GetCount();
    bot->DestroyItem(visitor.items[victim]->GetBagSlot(), visitor.items[victim]->GetSlot(), true);
    Planner::NoteNonUse(botAI, nonUse, units);

    EventLog::Emit(bot, EventLog::EventType::Inventory, "destroyed_for_space", itemId, input.VictimWorth);
    WorldTravel::AddLostWorth(measure, getMSTime(), input.VictimWorth);
    return true;
}

// Persistent bots loot what fits; with full bags they destroy their lowest-worth item for better loot. Loot left
// behind and items destroyed count as lost worth, stored loot as used slots (WorldTravel::SlotMeasure). An item left
// for lack of room sets leftWorth (its worth) and counts as lost only at the open that first leaves its target
// (alreadyLeft false; upkeep WU36).
bool MakeRoomForLoot(Player* bot, AiObjectContext* context, ItemTemplate const* proto, uint32 count, bool alreadyLeft,
                     std::optional<uint32>& leftWorth)
{
    WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    ItemPosCountVec dest;
    InventoryResult const result = bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, proto->ItemId, count);
    if (result == EQUIP_ERR_OK)
    {
        WorldTravel::AddLootSlots(
            measure, getMSTime(),
            WorldTravel::LootSlotsFilled(proto->Quality == ITEM_QUALITY_POOR, NewSlotsFor(bot, dest)));
        return true;
    }

    if (result != EQUIP_ERR_INVENTORY_FULL)
        return false;

    ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", proto->ItemId);
    uint32 const worth = ItemUsageValue::ItemWorth(
        proto, usage, count, static_cast<float>(sRandomPlayerbotMgr.GetSellMultiplier(bot)),
        BankMail::CarriedWorthEach(GET_PLAYERBOT_AI(bot), proto->ItemId),
        ItemUse::DemandWorthEach(GET_PLAYERBOT_AI(bot), proto->ItemId),
        usage == ITEM_USAGE_DISENCHANT ? ItemUsageValue::DisenchantOutletOf(GET_PLAYERBOT_AI(bot), proto).worthEach
                                       : 0);
    if (DestroyForLoot(GET_PLAYERBOT_AI(bot), context, worth))
    {
        dest.clear();
        if (bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, proto->ItemId, count) == EQUIP_ERR_OK)
        {
            WorldTravel::AddLootSlots(
                measure, getMSTime(),
                WorldTravel::LootSlotsFilled(proto->Quality == ITEM_QUALITY_POOR, NewSlotsFor(bot, dest)));
            return true;
        }
    }

    WorldTravel::AddLeftWorth(measure, getMSTime(), worth, alreadyLeft);
    leftWorth = worth;
    return false;
}
}  // namespace

bool LootAction::Execute(Event /*event*/)
{
    if (AI_VALUE(LootObjectStack*, "available loot")->HasInteraction() || bot->GetLootGUID() ||
        bot->IsNonMeleeSpellCast(false))
        return false;

    LootObject lootObject = AI_VALUE(LootObject, "nearest loot");
    if (lootObject.IsEmpty() || !AI_VALUE(LootObjectStack*, "available loot")->CanAttemptLoot(lootObject.guid))
        return false;

    context->GetValue<LootObject>("loot target")->Set(lootObject);
    return true;
}

bool LootAction::isUseful()
{
    if (AI_VALUE(LootObjectStack*, "available loot")->HasInteraction())
        return false;

    if (!sPlayerbotAIConfig.FreeMethodLoot && bot->GetGroup() && bot->GetGroup()->GetLootMethod() == FREE_FOR_ALL &&
        !IsSelfBot(bot))
        return false;

    LootObject lootTarget = AI_VALUE(LootObject, "loot target");
    LootObject nearest = AI_VALUE(LootObject, "nearest loot");
    if (lootTarget.IsEmpty() || nearest.IsEmpty() || nearest.guid == lootTarget.guid ||
        !AI_VALUE(LootObjectStack*, "available loot")->CanAttemptLoot(lootTarget.guid))
        return true;

    // Plain lookups: LootObject::GetWorldObject re-runs Refresh, which IsLootPossible below already pays.
    WorldObject* nearestObject = botAI->GetWorldObject(nearest.guid);
    WorldObject* currentObject = botAI->GetWorldObject(lootTarget.guid);
    if (!nearestObject || !currentObject)
        return true;

    // Leave a valid target only for a clearly closer one, so two equidistant corpses on either side
    // of an obstacle do not swap places on every tick.
    constexpr float lootSwitchMargin = 3.0f;
    if (bot->GetDistance(nearestObject) + lootSwitchMargin < bot->GetDistance(currentObject))
        return true;

    return !lootTarget.IsLootPossible(bot);
}

enum ProfessionSpells
{
    ALCHEMY = 2259,
    BLACKSMITHING = 2018,
    COOKING = 2550,
    ENCHANTING = 7411,
    ENGINEERING = 49383,
    FIRST_AID = 3273,
    FISHING = 7620,
    HERB_GATHERING = 2366,
    INSCRIPTION = 45357,
    JEWELCRAFTING = 25229,
    MINING = 2575,
    SKINNING = 8613,
    TAILORING = 3908
};

bool OpenLootAction::Execute(Event /*event*/) { return TryOpen(false) != LootInteraction::Attempt::Failed; }

struct OpenLootAction::InteractionAdapter
{
    OpenLootAction& Action;
    Player& Bot;
    LootObjectStack& Stack;
    LootObject Target;
    bool Encounter;
    uint32 AttemptMs = 0;

    bool Admit()
    {
        if (Target.IsEmpty() || !Stack.CanAttemptLoot(Target.guid) || !Target.IsAtInteractDistance(&Bot))
            return false;
        GameObject* const object = Action.botAI->GetGameObject(Target.guid);
        if (!Target.IsLootPossible(&Bot) || (object && object->GetGoState() != GO_STATE_READY))
        {
            if (Stack.InteractionGuid() == Target.guid)
                Stack.Cancel("invalid");
            return false;
        }
        AttemptMs = getMSTime();
        return Stack.PrepareLoot(Target.guid, Encounter, AttemptMs);
    }

    bool PrepareMotion()
    {
        if (Bot.isMoving())
        {
            Bot.StopMoving();
            Action.botAI->SetNextCheckDelay(sPlayerbotAIConfig.LootDelay);
            Stack.Trace("prepare", "stop");
            return true;
        }

        if (Bot.IsMounted())
        {
            CheckMountStateAction::Dismount(&Bot, MountRules::How::Loot);
            Action.botAI->SetNextCheckDelay(sPlayerbotAIConfig.ReactDelay);
            Stack.Trace("prepare", "ground_dismount");
            return true;
        }

        return false;
    }

    bool Request() { return Action.RequestLoot(Target); }
    void Begin() { Stack.BeginLoot(Target.guid, AttemptMs); }
    void Defer() { Stack.Cancel("dry"); }
};

// In the air: falling, or flying by the flag and above the floor (the higher of ground and water). A bot on its flying
// mount keeps the flag on the ground; it is dismounted for the loot like any mounted bot
static bool LootInTheAir(Player* bot)
{
    if (bot->Unit::IsFalling())
        return true;
    if (!bot->IsFlying())
        return false;
    auto const [groundZ, waterZ] = CheckMountStateAction::FloorBelow(bot);
    return TravelLegs::InTheAir(bot->GetPositionZ(), std::max(groundZ, waterZ));
}

LootInteraction::Attempt OpenLootAction::TryOpen(bool encounter)
{
    LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
    if (encounter && bot->IsAlive() && botAI->CanMove() && availableLoot->ConsumeExpiry())
        return LootInteraction::Attempt::Prepared;

    if (availableLoot->IsLootPending() || bot->GetLootGUID() || bot->IsNonMeleeSpellCast(false) ||
        (!encounter && bot->IsInCombat()) || !bot->IsAlive() || !botAI->CanMove() || bot->GetTransport() ||
        bot->GetVehicle() || LootInTheAir(bot))
        return LootInteraction::Attempt::Failed;

    InteractionAdapter adapter{*this, *bot, *availableLoot, AI_VALUE(LootObject, "loot target"), encounter};
    return LootInteraction::Execute(adapter);
}

bool OpenLootAction::RequestLoot(LootObject& lootObject)
{
    Creature* creature = botAI->GetCreature(lootObject.guid);
    if (creature && creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT, 8);
        *packet << lootObject.guid;
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleLootOpcode(packet);
        // The loot window's packet wakes the bot (questing WQ55); the loot delay is the longest wait for it
        botAI->SetPollDelay(sPlayerbotAIConfig.LootDelay);
        return true;
    }

    if (creature)
    {
        SkillType skill = creature->GetCreatureTemplate()->GetRequiredLootSkill();
        if (!CanOpenLock(skill, lootObject.reqSkillValue))
            return false;

        switch (skill)
        {
            case SKILL_ENGINEERING:
                return botAI->HasSkill(SKILL_ENGINEERING) ? botAI->CastSpell(ENGINEERING, creature) : false;
            case SKILL_HERBALISM:
                return botAI->HasSkill(SKILL_HERBALISM) ? botAI->CastSpell(32605, creature) : false;
            case SKILL_MINING:
                return botAI->HasSkill(SKILL_MINING) ? botAI->CastSpell(32606, creature) : false;
            default:
                if (!botAI->HasSkill(SKILL_SKINNING) || !GatherPassBy::Skins(botAI, lootObject.guid) ||
                    !botAI->CastSpell(SKINNING, creature))
                    return false;
                NewRpgDoGatherAction::NoteHarvestCast(botAI, lootObject.guid);  // a creature stop's skin (WR20c)
                GatherPassBy::NoteTake(botAI, lootObject.guid, true);           // a hide that paid (WR13c)
                return true;
        }
    }

    if (lootObject.reqItem)
    {
        Item* key = bot->GetItemByEntry(lootObject.reqItem);
        if (!key)
            return false;

        UseItemAction useItem(botAI, "use", false, "", ItemUse::Caller::Errand);
        return useItem.UseItemOnGameObject(key, lootObject.guid);
    }

    if (lootObject.skillId == SKILL_MINING || lootObject.skillId == SKILL_HERBALISM)
    {
        bool const mining = lootObject.skillId == SKILL_MINING;
        if (!botAI->HasSkill(mining ? SKILL_MINING : SKILL_HERBALISM) ||
            !botAI->CastSpell(mining ? MINING : HERB_GATHERING, bot))
            return false;
        // A planned gather stop taken by this bot's own harvest (professions WR13b)
        NewRpgDoGatherAction::NoteHarvestCast(botAI, lootObject.guid);
        GatherPassBy::NoteTake(botAI, lootObject.guid, false);  // a node passed by and taken (WR13c)
        return true;
    }

    uint32 spellId = GetOpeningSpell(lootObject);
    if (!spellId)
        return false;

    return botAI->CastSpell(spellId, bot);
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject)
{
    if (GameObject* go = botAI->GetGameObject(lootObject.guid))
        if (go->isSpawned())
            return GetOpeningSpell(lootObject, go);

    return 0;
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject, GameObject* go)
{
    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 spellId = itr->first;

        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        if (CanOpenLock(lootObject, spellInfo, go))
            return spellId;
    }

    for (uint32 spellId : PlayerbotSpellRepository::Instance().GetOpeningSpells(lootObject.GetLockType()))
    {
        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (spellInfo && CanOpenLock(lootObject, spellInfo, go))
            return spellId;
    }

    return sPlayerbotAIConfig.OpenGoSpell;
}

bool OpenLootAction::CanOpenLock(LootObject& lootObject, SpellInfo const* spellInfo, GameObject* /*go*/)
{
    if (!lootObject.GetLockType())
        return false;

    for (uint8 effIndex = 0; effIndex <= EFFECT_2; effIndex++)
    {
        if (spellInfo->Effects[effIndex].Effect != SPELL_EFFECT_OPEN_LOCK &&
            spellInfo->Effects[effIndex].Effect != SPELL_EFFECT_SKINNING)
            continue;

        if (uint32(spellInfo->Effects[effIndex].MiscValue) == lootObject.GetLockType())
            return CanOpenLock(lootObject.skillId, lootObject.reqSkillValue);
    }

    return false;
}

bool OpenLootAction::CanOpenLock(uint32 skillId, uint32 reqSkillValue)
{
    if (skillId == SKILL_NONE)
        return true;

    uint32 skillValue = bot->GetSkillValue(skillId);
    return skillValue >= reqSkillValue || !reqSkillValue;
}

bool StoreLootAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());  // (8+1+4+1+1+4+4+4+4+4+1)
    ObjectGuid guid;
    uint8 loot_type;
    uint32 gold = 0;
    uint8 items = 0;
    uint8 autostored = 0;

    p.rpos(0);
    if (p.size() < 9)
        return false;

    p >> guid;       // 8 corpse/gameobject guid
    p >> loot_type;  // 1 loot type

    LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
    ObjectGuid currentLoot = bot->GetLootGUID();
    if (loot_type == LOOT_NONE)
    {
        if (availableLoot->InteractionGuid() == guid && availableLoot->IsLootPending() &&
            !availableLoot->InteractionExpired())
            availableLoot->Cancel("response_error");
        return false;
    }

    if (currentLoot != guid)
        return false;

    if (p.size() < 14)
        return false;

    p >> gold;
    p >> items;
    if (p.size() - p.rpos() < size_t(items) * 22)
        return false;

    if (!availableLoot->LootOpened(guid) && !availableLoot->AcceptUnsolicitedLoot(guid))
        return false;

    if (gold > 0)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LOOT_MONEY, 0);
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleLootMoneyOpcode(packet);
    }

    // Persistent bots weigh full bags by item worth (WT.5); other setups keep the bag space rule below
    bool const honestSpace = botAI->KeepsPossessions();
    bool const alreadyLeft = honestSpace && availableLoot->WasLeft(guid);
    uint32 leftItem = 0;
    uint32 leftCopper = 0;  // the counted worth: a priceless item adds none
    uint32 leftMost = 0;
    // A skinner loots a corpse whose hide pays empty, or it can never be skinned (professions WR13c)
    bool const emptyFirst = loot_type == LOOT_CORPSE && GatherPassBy::LootsEmpty(botAI, guid);
    // The loot of the bot's own item (a container it opened; an item it disenchanted, prospected or milled) is taken
    // whole, as NormalLootStrategy means to: its "loot target" check never sees such a response, which comes unasked
    bool const ownItem = guid.IsItem();
    for (uint8 i = 0; i < items; ++i)
    {
        uint32 itemid;
        uint32 itemcount;
        uint8 lootslot_type;
        uint8 itemindex;

        p >> itemindex;
        p >> itemid;
        p >> itemcount;
        p.read_skip<uint32>();  // display id
        p.read_skip<uint32>();  // randomSuffix
        p.read_skip<uint32>();  // randomPropertyId
        p >> lootslot_type;     // 0 = can get, 1 = look only, 2 = master get

        if (lootslot_type != LOOT_SLOT_TYPE_ALLOW_LOOT && lootslot_type != LOOT_SLOT_TYPE_OWNER)
            continue;

        if (ownItem && loot_type != LOOT_SKINNING && !IsLootAllowed(itemid, botAI))
            LOG_DEBUG("playerbots",
                      "[Loot] {} takes item {} x{} from its own item (loot type {}): the loot strategy "
                      "alone would leave it",
                      bot->GetName(), itemid, itemcount, uint32(loot_type));
        else if (loot_type != LOOT_SKINNING && !emptyFirst && !IsLootAllowed(itemid, botAI))
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
        if (!proto)
            continue;

        if (honestSpace)
        {
            std::optional<uint32> leftWorth;
            if (!MakeRoomForLoot(bot, context, proto, itemcount, alreadyLeft, leftWorth))
            {
                if (leftWorth && (!leftItem || *leftWorth > leftMost))
                {
                    leftItem = itemid;
                    leftMost = *leftWorth;
                }
                if (leftWorth && *leftWorth != WorldTravel::WORTH_PRICELESS)
                    leftCopper += *leftWorth;
                continue;
            }
        }
        // Skip loot that isn't useful when bags are nearly full.
        else if (!IsRealPlayer(botAI->GetMaster()) && AI_VALUE(uint8, "bag space") > 80)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemid);
            bool const isUsefulItem =
                proto->StartQuest || (usage != ITEM_USAGE_NONE && usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_AH);

            if (isUsefulItem)
            {
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, itemid, itemcount) != EQUIP_ERR_OK)
                    continue;
            }
            else
            {
                uint32 maxStack = proto->GetMaxStackSize();
                if (maxStack == 1)
                    continue;

                std::vector<Item*> found = parseItems(chat->FormatItem(proto));
                bool hasFreeStack = false;
                for (auto stack : found)
                {
                    if (stack->GetCount() + itemcount < maxStack)
                    {
                        hasFreeStack = true;
                        break;
                    }
                }

                if (!hasFreeStack)
                    continue;
            }
        }

        Player* master = botAI->GetMaster();
        if (sRandomPlayerbotMgr.IsRandomBot(bot) && master)
        {
            uint32 price = itemcount * proto->BuyPrice * sRandomPlayerbotMgr.GetBuyMultiplier(bot) + gold;
            if (price)
                sRandomPlayerbotMgr.AddTradeDiscount(bot, master, price);

            if (Group* group = bot->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (ref->GetSource() != bot)
                        GuildTaskMgr::instance().CheckItemTask(itemid, itemcount, ref->GetSource(), bot);
        }

        WorldPacket* packet = new WorldPacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        *packet << itemindex;
        bot->GetSession()->QueuePacket(packet);
        // bot->GetSession()->HandleAutostoreLootItemOpcode(packet);
        // Taking what fits takes a moment, not the loot delay (questing WQ55): the session stores the items first
        botAI->SetThinkDelay(LOOT_TAKE_MS);
        ++autostored;

        if (proto->Quality > ITEM_QUALITY_NORMAL && !urand(0, 50) &&
            botAI->HasStrategy("emote", BOT_STATE_NON_COMBAT) && sPlayerbotAIConfig.RandomBotEmote)
            botAI->PlayEmote(TEXT_EMOTE_CHEER);

        if (proto->Quality >= ITEM_QUALITY_RARE && !urand(0, 1) && botAI->HasStrategy("emote", BOT_STATE_NON_COMBAT) &&
            sPlayerbotAIConfig.RandomBotEmote)
            botAI->PlayEmote(TEXT_EMOTE_CHEER);

        BroadcastHelper::BroadcastLootingItem(botAI, bot, proto);
    }

    if (AI_VALUE(LootObject, "loot target").guid == guid)
        context->GetValue<LootObject>("loot target")->Set(LootObject());

    // One outcome row per target left for room (id the most worth item left, amount the worth counted as lost)
    if (leftItem && !alreadyLeft)
        EventLog::Emit(bot, EventLog::EventType::Inventory, "loot_left", leftItem, leftCopper);

    availableLoot->CompleteResponse(guid, gold, autostored, leftItem != 0);
    // The nearest-loot answer (kept 500 ms) still names this corpse: the next check would pick it again and do
    // nothing else (questing WQ55)
    context->GetValue<LootObject>("nearest loot")->Reset();
    return true;
}

bool StoreLootAction::IsLootAllowed(uint32 itemid, PlayerbotAI* botAI)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    LootStrategy* lootStrategy = AI_VALUE(LootStrategy*, "loot strategy");

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
    if (!proto)
        return false;

    std::set<uint32>& lootItems = AI_VALUE(std::set<uint32>&, "always loot list");
    if (lootItems.find(itemid) != lootItems.end())
        return true;

    uint32 max = proto->MaxCount;
    if (max > 0 && botAI->GetBot()->HasItemCount(itemid, max, true))
        return false;

    if (proto->StartQuest)
    {
        return true;
    }

    for (uint16 slot = 0; slot < botAI->GetBot()->GetQuestLogSize(); ++slot)
    {
        uint32 entry = botAI->GetBot()->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(entry);
        if (!quest)
            continue;

        // An ItemDrop: needed for the quest, but named by no RequiredItemId (adapted from mod-playerbots PR #2632)
        if (LootObject::IsQuestExtraItemWanted(botAI->GetBot(), quest, itemid))
            return true;

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
        {
            if (quest->RequiredItemId[i] == itemid)
            {
                // if (AI_VALUE2(uint32, "item count", proto->Name1) < quest->RequiredItemCount[i])
                // {
                //     if (botAI->GetMaster() && sPlayerbotAIConfig.SyncQuestWithPlayer)
                //         return false; //Quest is autocomplete for the bot so no item needed.
                // }

                return true;
            }
        }
    }

    // if (proto->Bonding == BIND_QUEST_ITEM ||  //Still testing if it works ok without these lines.
    //     proto->Bonding == BIND_QUEST_ITEM1 || //Eventually this has to be removed.
    //     proto->Class == ITEM_CLASS_QUEST)
    //{

    bool canLoot = lootStrategy->CanLoot(proto, context);
    // if (canLoot && proto->Bonding == BIND_WHEN_PICKED_UP && IsRealPlayer(botAI->GetMaster()))
    // canLoot = sPlayerbotAIConfig.IsInRandomAccountList(botAI->GetBot()->GetSession()->GetAccountId());

    return canLoot;
}

bool ReleaseLootAction::Execute(Event /*event*/)
{
    ObjectGuid guid = bot->GetLootGUID();
    if (!guid)
        return false;

    WorldPacket* packet = new WorldPacket(CMSG_LOOT_RELEASE, 8);
    *packet << guid;
    bot->GetSession()->QueuePacket(packet);
    return true;
}
