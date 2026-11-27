/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeBotState.h"

#include <algorithm>
#include <chrono>
#include <list>
#include <optional>
#include <thread>

#include "AuctionBuyList.h"
#include "AuctionHouseMgr.h"
#include "AuctionIndex.h"
#include "Bag.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "BotKind.h"
#include "BotRecordMgr.h"
#include "BridgeAbilities.h"
#include "BridgeAround.h"
#include "BridgeAuctionTopic.h"
#include "BridgeBotFacts.h"
#include "BridgeBotTopics.h"
#include "BridgeItemsTopic.h"
#include "BridgeKnowledgeRules.h"
#include "BridgeLookupRules.h"
#include "BridgeNameIndex.h"
#include "BridgeNames.h"
#include "BridgeNeedsTopic.h"
#include "BridgeOwnRules.h"
#include "BridgePerception.h"
#include "BridgeProfessionsTopic.h"
#include "BridgeTexts.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "ItemDemand.h"
#include "ItemUse.h"
#include "ItemUseKinds.h"
#include "LootObjectStack.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Pet.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "RecordWords.h"
#include "RouteService.h"
#include "SkillPathLive.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelMgr.h"
#include "TravelPlaces.h"
#include "World.h"
#include "WorldThread.h"

namespace PlayerbotBridge
{
namespace
{
// Cached zone and area ids; never a terrain lookup.
std::string AreaName(uint32 areaId)
{
    AreaTableEntry const* entry = sAreaTableStore.LookupEntry(areaId);
    return entry ? PlayerbotAI::GetLocalizedAreaName(entry) : std::string();
}

void ReadInventory(Player* bot, std::vector<BotStateItem>& items)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back({item->GetTemplate()->Name1, item->GetTemplate()->Quality, item->GetCount(), 1});
    std::vector<BotStateItem> carried;
    auto const carry = [&](Item* item)
    {
        if (item)
            carried.push_back({item->GetTemplate()->Name1, item->GetTemplate()->Quality, item->GetCount(), 0});
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        carry(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                carry(bag->GetItemByPos(static_cast<uint8>(slot)));
    std::stable_sort(carried.begin(), carried.end(),
                     [](BotStateItem const& a, BotStateItem const& b) { return a.value > b.value; });
    items.insert(items.end(), carried.begin(), carried.end());
}

void ReadQuests(Player* bot, std::vector<BotStateItem>& items)
{
    QuestStatusMap& statuses = bot->getQuestStatusMap();
    for (uint16 slot = 0; slot < bot->GetQuestLogSize(); ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        auto const status = statuses.find(questId);
        if (!quest || status == statuses.end())
            continue;
        BotStateItem item{quest->GetTitle()};
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            item.extra += quest->RequiredNpcOrGoCount[i];
            item.value += std::min<uint32>(status->second.CreatureOrGOCount[i], quest->RequiredNpcOrGoCount[i]);
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            item.extra += quest->RequiredItemCount[i];
            item.value += std::min<uint32>(status->second.ItemCount[i], quest->RequiredItemCount[i]);
        }
        item.code = status->second.Status == QUEST_STATUS_COMPLETE ? 1
                    : status->second.Status == QUEST_STATUS_FAILED ? 2
                                                                   : 0;
        items.push_back(std::move(item));
    }
}

void ReadCooldowns(Player* bot, std::vector<BotStateItem>& items)
{
    for (auto const& [spellId, cooldown] : bot->GetSpellCooldownMap())
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        uint32 const left = bot->GetSpellCooldownDelay(spellId);
        if (!spell || !left || spell->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) || !spell->SpellName[0])
            continue;
        items.push_back({spell->SpellName[0], (left + 999) / 1000});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](BotStateItem const& a, BotStateItem const& b) { return a.value > b.value; });
}

void ReadStrategies(PlayerbotAI* ai, std::vector<BotStateItem>& items)
{
    for (std::string& name : ai->GetStrategies(BOT_STATE_COMBAT))
        items.push_back({std::move(name), 0, 0, 1});
    for (std::string& name : ai->GetStrategies(BOT_STATE_NON_COMBAT))
        items.push_back({std::move(name), 0, 0, 0});
}

void ReadSpells(Player* bot, std::vector<BotStateItem>& items)
{
    std::vector<std::pair<uint32, BotStateItem>> spells;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active || !known->IsInSpec(bot->GetActiveSpec()))
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!spell || spell->IsPassive() || spell->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) ||
            spell->SpellFamilyName == SPELLFAMILY_GENERIC || !spell->SpellName[0])
            continue;
        SpellInfo const* next = spell->GetNextRankSpell();
        if (next && bot->HasSpell(next->Id))
            continue;
        spells.push_back({spell->SpellLevel, {spell->SpellName[0], spell->GetRank()}});
    }
    std::stable_sort(spells.begin(), spells.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    for (auto& spell : spells)
        items.push_back(std::move(spell.second));
}

// The planner topics read the view the bot's map thread published; out of the planner's scope (a master at a game
// client, not persistent) the planner sets no goal, so they show none
Topics::PlanInput PlanInputOf(Player* bot, PlayerbotAI* ai)
{
    Topics::PlanInput in;
    in.inScope = ai->KeepsPossessions();
    in.seen = in.inScope && Planner::PublishedPlan(bot->GetGUID().GetRawValue(), in.view);
    in.nowMs = getMSTime();
    in.x = bot->GetPositionX();
    in.y = bot->GetPositionY();
    in.z = bot->GetPositionZ();
    in.open = [bot](uint32 quest) { return bot->GetQuestStatus(quest) == QUEST_STATUS_INCOMPLETE; };
    in.title = [](uint32 quest)
    {
        Quest const* questTemplate = sObjectMgr->GetQuestTemplate(quest);
        return questTemplate ? questTemplate->GetTitle() : std::string();
    };
    return in;
}

void ReadBags(Player* bot, PlayerbotAI* ai, std::vector<BotStateItem>& items)
{
    Topics::PlanInput const plan = PlanInputOf(bot, ai);
    Topics::BagsInput in;
    in.freeSlots = bot->GetFreeInventorySpace();
    in.slots = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            in.slots += bag->GetBagSize();
    in.plan = &plan;
    // Stored knowledge only (WB.4: how much mail waits, never what is inside), as the WB2 and WB5 commands read it
    BankMail::BotBankMail const& known = ai->bankMail;
    in.mailWaiting = BankMail::CountWaitingMail(bot);
    in.mailSeenCopper = known.mail.seenCopper;
    in.mailExpectedCopper = known.mail.expectedCopper;
    in.keepsPossessions = BotKind::KeepsPossessions(bot);
    if (in.keepsPossessions)
    {
        auto const addKept = [&](uint32 entry, uint8 dueLevel, bool inBank)
        {
            if (ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry))
                in.kept.push_back({itemTemplate->Name1, dueLevel, inBank, dueLevel <= bot->GetLevel()});
        };
        for (BankMail::KeptItem const& kept : known.bank.kept)
            addKept(kept.entry, kept.dueLevel, true);
        for (BankMail::KeptEntry const& kept : known.bank.carried.items)
            addKept(kept.entry, kept.dueLevel, false);
        in.bankFree = known.bank.freeSlots;
        in.bankSlots = known.bank.slots;
    }
    Topics::BagsItems(in, items);
}

void ReadPlaces(Player* bot, std::vector<BotStateItem>& items)
{
    TravelRoute::Snapshot const snapshot = TravelRoute::FillSnapshot(bot, 0);
    Topics::PlacesInput in;
    in.bot = snapshot.position;
    in.hasHearthstone = snapshot.hasHearthstone;
    in.hearthCooldownMs = snapshot.hearthCooldownMs;
    in.hearthArea = AreaName(bot->m_homebindAreaId);
    for (uint32 node = 1; node <= TravelRoute::TAXI_MASK_SIZE * 32; ++node)
        if (snapshot.KnowsTaxiNode(node))
            if (TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node))
            {
                char const* name = entry->name[sWorld->GetDefaultDbcLocale()];
                in.known.push_back({node,
                                    name && *name ? name : entry->name[LOCALE_enUS],
                                    {entry->map_id, entry->x, entry->y, entry->z}});
            }
    Topics::PlacesItems(in, items);
}

// One search on this thread, as a route worker would run it (RouteService.h): its PerfMonitor row is the cost
void ReadRoute(Player* bot, std::string const& argument, std::vector<BotStateItem>& items)
{
    Topics::RouteInput in;
    std::optional<TravelRoute::Point> const goal = TravelPlaces::Resolve(argument);
    in.known = goal.has_value();
    TravelPlaces::Place const* named = TravelPlaces::Find(argument);
    in.place = named ? named->name : BridgeTexts::Lookups::ROUTE_LITERAL_PLACE;
    if (goal)
    {
        auto const started = std::chrono::steady_clock::now();
        TravelRoute::Request const request(TravelRoute::FillSnapshot(bot, 0), *goal);
        std::shared_ptr<TravelRoute::StuckEdges const> const stuck = TravelRoute::StuckBook::Instance().Current();
        // Without flight maps a flyer's fly legs are priced, not flown: flight searches run only on the route
        // service's workers (contract C3)
        TravelRoute::Params params = TravelRoute::ConfiguredParams();
        params.flightMaps = nullptr;
        in.route = TravelRoute::Search(TravelGraph::Instance(), request, *stuck, params);
        auto const us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "bridge route topic"))
            row->Record(static_cast<uint64>(std::max<long long>(us, 0)));
    }
    Topics::RouteItems(in, items);
}

// A DBC name in the server's locale, else in English
template <typename Names>
std::string DbcName(Names const& names)
{
    char const* name = names[sWorld->GetDefaultDbcLocale()];
    return name && *name ? name : (names[LOCALE_enUS] ? names[LOCALE_enUS] : "");
}

// LC7: the game's names of a record row's ids, from templates, DBC stores and the character cache (no DB)
class GameLookup final : public RecordWords::Lookup
{
public:
    std::string PlaceName(uint32 area, uint32 zone) const override
    {
        std::string name = AreaName(area);
        return name.empty() ? AreaName(zone) : name;
    }

    std::string ZoneName(uint32 zone) const override { return AreaName(zone); }

    std::string CreatureName(uint32 entry) const override
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        return creature ? creature->Name : std::string();
    }

    uint32_t CreatureType(uint32 entry) const override
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        return creature ? creature->type : 0;
    }

    bool Boss(uint32 entry) const override
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        return creature && (creature->rank == CREATURE_ELITE_WORLDBOSS ||
                            creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_DUNGEON_BOSS));
    }

    std::string QuestTitle(uint32 id) const override
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(id);
        return quest ? quest->GetTitle() : std::string();
    }

    std::string ItemName(uint32 id) const override
    {
        ItemTemplate const* item = sObjectMgr->GetItemTemplate(id);
        return item ? item->Name1 : std::string();
    }

    std::string SpellName(uint32 id) const override
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
        return spell ? DbcName(spell->SpellName) : std::string();
    }

    std::string SkillName(uint32 id) const override
    {
        SkillLineEntry const* skill = sSkillLineStore.LookupEntry(id);
        return skill ? DbcName(skill->name) : std::string();
    }

    std::string FlightPointName(uint32 node) const override
    {
        TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node);
        return entry ? DbcName(entry->name) : std::string();
    }

    std::string DungeonName(uint32 map) const override
    {
        MapEntry const* entry = sMapStore.LookupEntry(map);
        return entry && entry->IsDungeon() ? DbcName(entry->name) : std::string();
    }

    std::optional<RecordWords::Person> Character(uint32 guid) const override
    {
        CharacterCacheEntry const* character =
            sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!character)
            return std::nullopt;
        return RecordWords::Person{character->Name, character->Race, character->Class};
    }
};

using RecordReader = std::vector<RecordWords::Entry> (*)(std::vector<RecordRules::Row> const& rows, uint32 self,
                                                         uint32 now, RecordWords::Lookup const& lookup);

void ReadRecord(Player* bot, RecordReader reader, char const* (*word)(uint8_t), std::vector<BotStateItem>& items)
{
    uint32 const self = bot->GetGUID().GetCounter();
    std::vector<RecordRules::Row> const rows = BotRecordMgr::Instance().Rows(self);
    uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());
    for (RecordWords::Entry& entry : reader(rows, self, now, GameLookup()))
        items.push_back(
            {std::move(entry.Name), entry.Value, entry.Extra, entry.Code, std::move(entry.Text), word(entry.Code)});
}

std::vector<RecordWords::Entry> LatelyEntries(std::vector<RecordRules::Row> const& rows, uint32 self, uint32 now,
                                              RecordWords::Lookup const& lookup)
{
    return RecordWords::Lately(rows, self, now, lookup);
}

// WU23: the needs list `playerbots planner needs` shows (Planner::BotNeedsNow, on this world thread), worded by name
void ReadNeeds(Player* bot, PlayerbotAI* ai, std::vector<BotStateItem>& items)
{
    Topics::NeedsInput in;
    in.governed = BotKind::KeepsPossessions(bot);
    UpkeepNeeds::NeedList list;
    if (in.governed)
    {
        Planner::Snapshot snapshot;
        UpkeepNeeds::NeedRates rates;
        in.ready = Planner::BotNeedsNow(bot, ai, snapshot, rates, list);
        in.cls = snapshot.cls;
    }
    in.list = &list;
    in.itemName = [](uint32 entry)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        return itemTemplate ? itemTemplate->Name1 : std::string();
    };
    in.spellName = [](uint32 spellId)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        return spell && spell->SpellName[0] ? std::string(spell->SpellName[0]) : std::string();
    };
    Topics::NeedsItems(in, items);
}

// LC18a: what the bot knows of places, from stored values: its side and level, the zones of its visits and living
// record rows and the zone it stands in (LC18e), the positions of the taxi nodes it knows (no terrain lookup, no query)
Knowledge::BotKnowledge KnowledgeOf(Player* bot)
{
    Knowledge::BotKnowledge know;
    know.side = Knowledge::SideOfTeam(bot->GetTeamId());
    know.level = bot->GetLevel();
    uint32 const guid = bot->GetGUID().GetCounter();
    know.stayedZones =
        Knowledge::StayedZones(BotRecordMgr::Instance().Rows(guid), BotRecordMgr::Instance().Visits(guid),
                               static_cast<uint32>(GameTime::GetGameTime().count()), bot->GetZoneId());
    TravelRoute::Snapshot const snapshot = TravelRoute::FillSnapshot(bot, 0);
    for (uint32 node = 1; node <= TravelRoute::TAXI_MASK_SIZE * 32; ++node)
        if (snapshot.KnowsTaxiNode(node))
            if (TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node))
                know.flightPoints.push_back({entry->map_id, entry->x, entry->y, entry->z});
    return know;
}

Knowledge::MapPoint PointOf(Player* bot)
{
    return {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
}

// The service a planner spawn is, by its flags (the first of this order); the mailbox is a gameobject
Planner::ServiceKind KindOf(Planner::Service const& service)
{
    namespace F = Planner::NpcFlag;
    if (service.mailbox)
        return Planner::ServiceKind::Mailbox;
    if (service.npcFlags & F::INNKEEPER)
        return Planner::ServiceKind::Innkeeper;
    if (service.npcFlags & F::FLIGHTMASTER)
        return Planner::ServiceKind::FlightMaster;
    if (service.npcFlags & F::BANKER)
        return Planner::ServiceKind::Banker;
    if (service.npcFlags & F::AUCTIONEER)
        return Planner::ServiceKind::Auctioneer;
    if (service.npcFlags & (F::TRAINER | F::TRAINER_CLASS | F::TRAINER_PROFESSION))
        return Planner::ServiceKind::Trainer;
    if (service.npcFlags & F::REPAIR)
        return Planner::ServiceKind::Repairer;
    if (service.npcFlags & F::VENDOR)
        return Planner::ServiceKind::Vendor;
    return Planner::ServiceKind::QuestGiver;
}

std::string ServiceNameOf(Planner::Service const& service)
{
    if (service.mailbox)
    {
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(service.entry);
        return object ? object->name : std::string();
    }
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(service.entry);
    return creature ? creature->Name : std::string();
}

Knowledge::WhereCandidate PersonOf(Planner::PlannerIndexes const& index, Planner::Service const& service,
                                   Planner::ServiceKind kind, std::string name)
{
    std::string place = AreaName(service.hub < index.hubs.size() ? index.hubs[service.hub].zoneId : service.zoneId);
    return {std::move(name),
            Knowledge::ServiceWhat(kind),
            std::move(place),
            {service.mapId, service.x, service.y, service.z},
            Knowledge::PlaceOfService(index, service),
            service.side};
}

// LC18a: where a service, a person or a place is. A service word: the nearest spawns of that service on the bot's
// map that serve its side (LC18g); else every service spawn whose name fits and every town whose name fits, of either
// side. Loaded stores only, no query; one pass over the index for a name, on a rare request.
void ReadWhere(Player* bot, std::string const& argument, std::vector<BotStateItem>& items)
{
    std::shared_ptr<Planner::PlannerIndexes const> const index = Planner::Indexes();
    if (!index)
        return Knowledge::WherePending(items);
    Knowledge::MapPoint const from = PointOf(bot);
    Knowledge::BotKnowledge const know = KnowledgeOf(bot);
    std::vector<Knowledge::WhereCandidate> things;
    if (std::optional<Planner::ServiceKind> const kind = Knowledge::AskedService(argument))
    {
        constexpr std::size_t NEAREST_SERVICES = 64;
        for (Planner::Nearby const& hit : Knowledge::NearestOffered(*index, know.side, *kind, from, NEAREST_SERVICES))
        {
            Planner::Service const& service = index->services[hit.index];
            if (std::string name = ServiceNameOf(service); !name.empty())
                things.push_back(PersonOf(*index, service, *kind, std::move(name)));
        }
    }
    else
    {
        std::string const key = NameKey(argument);
        for (Planner::Service const& service : index->services)
            if (std::string name = ServiceNameOf(service); Knowledge::NameFits(key, name))
                things.push_back(PersonOf(*index, service, KindOf(service), std::move(name)));
        for (Knowledge::WhereCandidate& town : Knowledge::TownCandidates(*index, from, &AreaName))
            if (Knowledge::NameFits(key, town.name))
                things.push_back(std::move(town));
    }
    Knowledge::WhereItems(know, from, std::move(things), items);
}

// LC18b: a person of the world as the lookup rule's fact, from its template: its side by its faction template's group,
// its level the top of its range. Where it stands: the first service spawn of its entry, when the index has one.
Knowledge::NpcFact NpcOf(Planner::PlannerIndexes const& index, CreatureTemplate const& creature, bool seen)
{
    Knowledge::NpcFact npc;
    npc.name = creature.Name;
    npc.level = creature.maxlevel;
    npc.seen = seen;
    if (FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature.faction))
        npc.side = Knowledge::SideOfFactionGroup(faction->ourMask);
    for (Planner::Service const& service : index.services)
    {
        if (service.mailbox || service.entry != creature.Entry)
            continue;
        npc.what = Knowledge::ServiceWhat(KindOf(service));
        npc.place = AreaName(service.hub < index.hubs.size() ? index.hubs[service.hub].zoneId : service.zoneId);
        npc.fact = Knowledge::PlaceOfService(index, service);
        break;
    }
    return npc;
}

// LC18b: who a person is. The bots of the record's known bots by name; the people of the world by their template:
// the service NPCs of the planner index and the people the bot sees. Loaded stores only, no query; a pass over the
// index's services for a name, on a rare request.
void ReadWho(Player* bot, PlayerbotAI* ai, std::string const& argument, std::vector<BotStateItem>& items)
{
    std::shared_ptr<Planner::PlannerIndexes const> const index = Planner::Indexes();
    if (!index)
        return Knowledge::LookupPending(items);
    std::string const key = NameKey(argument);
    std::vector<BotStateItem> botLines;
    uint32 const self = bot->GetGUID().GetCounter();
    uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());
    for (RecordWords::Entry const& entry :
         RecordWords::KnownBots(BotRecordMgr::Instance().Rows(self), self, now, GameLookup()))
        if (Knowledge::NameFits(key, entry.Name))
            botLines.push_back(Knowledge::LookupLine(Knowledge::LookupRow::Bot, entry.Name, entry.Text));
    std::vector<Knowledge::NpcFact> people;
    std::vector<uint32> entries;
    auto const add = [&](uint32 entry, bool seen)
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        if (!creature || std::find(entries.begin(), entries.end(), entry) != entries.end() ||
            !Knowledge::NameFits(key, creature->Name))
            return;
        entries.push_back(entry);
        people.push_back(NpcOf(*index, *creature, seen));
    };
    for (ObjectGuid const& guid : ai->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get())
        if (Unit* unit = ObjectAccessor::GetUnit(*bot, guid); unit && unit->IsCreature())
            add(unit->GetEntry(), true);
    for (Planner::Service const& service : index->services)
        if (!service.mailbox)
            add(service.entry, false);
    Knowledge::WhoItems(KnowledgeOf(bot), std::move(botLines), people, items);
}

// The quests in the log as the quests topic walks them: the template of each quest the bot has
std::vector<Quest const*> LogQuestTemplates(Player* bot)
{
    std::vector<Quest const*> quests;
    for (uint16 slot = 0; slot < bot->GetQuestLogSize(); ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot))
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                quests.push_back(quest);
    return quests;
}

// A quest as the lookup rule's fact: its level (the scaling -1 takes the minimum), whose by its giver's faction group
// or else its races, the creature that gives it and where that one stands (a service spawn of the index), what the
// bot has of it
Knowledge::QuestFact QuestOf(Planner::PlannerIndexes const& index, Player* bot, Quest const& quest)
{
    Knowledge::QuestFact fact;
    fact.title = quest.GetTitle();
    fact.level = quest.GetQuestLevel() > 0 ? static_cast<uint32>(quest.GetQuestLevel()) : quest.GetMinLevel();
    uint32 const id = quest.GetQuestId();
    bool const inLog = bot->FindQuestSlot(id) < bot->GetQuestLogSize();
    if (inLog)
        fact.state = Knowledge::QuestState::InLog;
    else if (bot->GetQuestRewardStatus(id))
        fact.state = Knowledge::QuestState::Done;
    for (auto const& [creatureEntry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
    {
        if (questId != id)
            continue;
        CreatureTemplate const* giver = sObjectMgr->GetCreatureTemplate(creatureEntry);
        if (!giver)
            continue;
        fact.giver = giver->Name;
        Knowledge::NpcFact const where = NpcOf(index, *giver, false);
        fact.place = where.place;
        fact.fact = where.fact;
        fact.side = where.side;
        break;
    }
    fact.side = Knowledge::SideOfQuest(fact.side, quest.GetAllowableRaces());
    return fact;
}

// LC18b: what an item or a quest is. A name matches the items the bot carries and the quests in its log by a part of
// the name (three letters on), and anything in the world by its exact name through the startup index.
void ReadWhat(Player* bot, std::string const& argument, std::vector<BotStateItem>& items)
{
    std::shared_ptr<Planner::PlannerIndexes const> const index = Planner::Indexes();
    NameIndex const* names = NameIndexNow();
    if (!index || !names)
        return Knowledge::LookupPending(items);
    std::string const key = NameKey(argument);
    std::vector<Knowledge::ItemFact> things;
    std::vector<uint32> itemIds;
    auto const addItem = [&](uint32 id, bool carried)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(id);
        if (!itemTemplate || std::find(itemIds.begin(), itemIds.end(), id) != itemIds.end())
            return;
        itemIds.push_back(id);
        things.push_back({itemTemplate->Name1, itemTemplate->Quality, itemTemplate->Class,
                          itemTemplate->RequiredLevel ? itemTemplate->RequiredLevel : itemTemplate->ItemLevel,
                          Knowledge::SideOfItem(itemTemplate->Flags2, static_cast<uint32>(itemTemplate->AllowableRace)),
                          carried});
    };
    for (NameCandidate const& carried : CarriedItemNames(bot))
        if (Knowledge::NameFits(key, carried.Name))
            addItem(static_cast<uint32>(carried.Id), true);
    if (uint32 const id = names->items.Find(argument))
        addItem(id, bot->HasItemCount(id, 1));
    std::vector<Knowledge::QuestFact> quests;
    std::vector<uint32> questIds;
    auto const addQuest = [&](Quest const* quest)
    {
        if (!quest || std::find(questIds.begin(), questIds.end(), quest->GetQuestId()) != questIds.end())
            return;
        questIds.push_back(quest->GetQuestId());
        quests.push_back(QuestOf(*index, bot, *quest));
    };
    for (Quest const* quest : LogQuestTemplates(bot))
        if (Knowledge::NameFits(key, quest->GetTitle()))
            addQuest(quest);
    if (uint32 const id = names->quests.Find(argument))
        addQuest(sObjectMgr->GetQuestTemplate(id));
    Knowledge::WhatItems(KnowledgeOf(bot), things, quests, items);
}

// LC18b: what a land is like. The lands of the planner index by area name; the towns it lists are its hubs' areas.
void ReadZone(Player* bot, std::string const& argument, std::vector<BotStateItem>& items)
{
    std::shared_ptr<Planner::PlannerIndexes const> const index = Planner::Indexes();
    if (!index)
        return Knowledge::LookupPending(items);
    std::string const key = NameKey(argument);
    std::vector<Knowledge::ZoneFact> zones;
    for (Planner::ZoneRecord const& record : index->zones)
    {
        std::string const name = AreaName(record.zoneId);
        if (!Knowledge::NameFits(key, name))
            continue;
        Knowledge::ZoneFact zone;
        zone.name = name;
        zone.side = Knowledge::SideOf(record.side);
        zone.low = record.low;
        zone.high = record.high;
        zone.fact.zoneId = record.zoneId;
        zone.fact.side = zone.side;
        zone.fact.lowLevel = record.low;
        zone.fact.at.mapId = record.mapId;
        bool first = true;
        for (Planner::Hub const& hub : index->hubs)
        {
            if (hub.zoneId != record.zoneId)
                continue;
            if (first)
            {
                zone.fact = Knowledge::PlaceOfHub(*index, hub);
                zone.fact.noSide = false;  // a land is judged by its level range, not by a town's side
            }
            first = false;
            if (std::string town = AreaName(hub.areaId); !town.empty())
                zone.towns.push_back({std::move(town), Knowledge::PlaceOfHub(*index, hub)});
        }
        zones.push_back(std::move(zone));
    }
    std::stable_sort(zones.begin(), zones.end(), [&key](Knowledge::ZoneFact const& a, Knowledge::ZoneFact const& b)
                     { return (NameKey(a.name) == key) > (NameKey(b.name) == key); });
    Knowledge::ZoneItems(KnowledgeOf(bot), zones, items);
}

// LC18c: what the bot can ride: its riding skill and the mount spells of its spellbook (a mount is a spell with the
// mounted aura; read at the request, never per tick)
void ReadMounts(Player* bot, std::vector<BotStateItem>& items)
{
    std::vector<std::string> mounts;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (spell && !spell->IsPassive() && spell->HasAura(SPELL_AURA_MOUNTED) && spell->SpellName[0])
            mounts.push_back(spell->SpellName[0]);
    }
    Own::MountItems(bot->GetPureSkillValue(SKILL_RIDING), std::move(mounts), items);
}

// LC18c, LC18f: what the bot can make of a craft or a thing: the crafts it has and the recipes it knows (one reader
// with the facts', BridgeBotFacts.cpp)
void ReadRecipes(Player* bot, std::string const& argument, std::vector<BotStateItem>& items)
{
    std::vector<Own::CraftFact> crafts;
    for (FactsRules::Craft const& craft : FactsRules::CRAFTS)
        crafts.push_back(
            {std::string(craft.Name), bot->HasSkill(craft.Skill)
                                          ? std::string(FactsRules::RankWord(bot->GetPureSkillValue(craft.Skill)))
                                          : std::string()});
    std::vector<BotRecipe> recipes;
    ReadBotRecipes(bot, recipes);
    Own::RecipeItems(NameKey(argument), crafts, std::move(recipes), items);
}

static_assert(TopicTable[static_cast<std::size_t>(BotStateTopic::Mounts)].Limit == Own::MOUNTS_SHOWN &&
                  TopicTable[static_cast<std::size_t>(BotStateTopic::Recipes)].Limit == Own::RECIPES_SHOWN,
              "the rules cut at the topic rows' limits");

// WR18: the professions the bot has (its skill lines) and what its kept path (BotGoals::path, which `needs` reads on
// this thread too) works towards and lacks, worded by BridgeProfessionsTopic.h
void ReadProfessions(Player* bot, PlayerbotAI* ai, std::vector<BotStateItem>& items)
{
    Topics::ProfessionsInput in;
    in.governed = BotKind::KeepsPossessions(bot);
    if (in.governed)
    {
        SkillPath::Path const* const path = Planner::KeptPath(ai);
        SkillPath::Stretch const* const chosen =
            path && path->chosen >= 0 && static_cast<std::size_t>(path->chosen) < path->weighed.size()
                ? &path->weighed[path->chosen]
                : nullptr;
        bool making = false;
        bool rankStep = false;
        if (path)
        {
            for (SkillPath::Craft const& craft : path->queue)
                making = making || craft.forSkill;
            for (SkillPath::Purchase const& need : path->needs)
                rankStep = rankStep || need.kind == SkillPath::StepKind::Rank;
        }
        for (FactsRules::Craft const& craft : FactsRules::CRAFTS)
        {
            if (!bot->HasSkill(craft.Skill))
                continue;

            Topics::ProfessionLine line;
            line.name = std::string(craft.Name);
            line.rank = std::string(FactsRules::RankWord(bot->GetPureSkillValue(craft.Skill)));
            if (chosen && chosen->skill == craft.Skill)
            {
                line.making = making;
                switch (chosen->milestone)
                {
                    case SkillPath::Milestone::Recipe:
                    case SkillPath::Milestone::HeldRecipe:
                        line.goal = rankStep ? Topics::ProfessionGoal::Rank : Topics::ProfessionGoal::Recipe;
                        break;
                    case SkillPath::Milestone::Node:
                        line.goal = Topics::ProfessionGoal::Node;
                        break;
                    case SkillPath::Milestone::Skinnable:
                        line.goal = Topics::ProfessionGoal::Skinnable;
                        break;
                }
            }
            in.lines.push_back(std::move(line));
        }
        if (chosen)
        {
            auto const lack = [&in](Topics::ProfessionLack kind)
            {
                if (std::find(in.lacks.begin(), in.lacks.end(), kind) == in.lacks.end())
                    in.lacks.push_back(kind);
            };
            for (SkillPath::Purchase const& need : path->needs)
            {
                if (need.kind == SkillPath::StepKind::Learn)
                    lack(Topics::ProfessionLack::Recipe);
                else if (need.kind == SkillPath::StepKind::Rank)
                    lack(Topics::ProfessionLack::Rank);
                else if (need.kind == SkillPath::StepKind::Tool)
                    lack(Topics::ProfessionLack::Tool);
            }
            if (!path->wants.empty())
                lack(Topics::ProfessionLack::Materials);
        }
    }
    Topics::ProfessionsItems(in, items);
}

static_assert(TopicTable[static_cast<std::size_t>(BotStateTopic::Professions)].Limit == Topics::PROFESSIONS_SHOWN,
              "the rules cut at the topic row's limit");

// WA9: what the bot has listed, looks for and has bid on, worded by BridgeAuctionTopic.h. The listings are a scan of
// the distinct houses' auctions by owner (AuctionIndex::Houses: one under the two-side switch, WAL1): O(all auctions)
// per ask, on demand only (the console or a wire ask, never a tick), and complete after a restart (the core's auctions
// persist; AuctionMemory forgets). The wants are the buy list priced as UpkeepNeeds' market lines are (a typical price
// known anywhere under the ceiling, WA.4), never a house's current offer
void ReadAuction(Player* bot, PlayerbotAI* ai, std::vector<BotStateItem>& items)
{
    Topics::AuctionInput in;
    in.governed = BotKind::KeepsPossessions(bot);
    if (in.governed)
    {
        auto const nameOf = [](uint32 entry)
        {
            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
            return itemTemplate ? itemTemplate->Name1 : std::string();
        };
        for (AuctionHouseObject* const houseObject : AuctionIndex::Houses())
        {
            for (auto const& [id, auction] : houseObject->GetAuctions())
            {
                if (!auction || auction->owner != bot->GetGUID())
                    continue;
                Topics::AuctionListing listing;
                listing.name = nameOf(auction->item_template);
                listing.count = auction->itemCount;
                listing.buyout = auction->buyout != 0;
                listing.copper = listing.buyout ? auction->buyout : auction->startbid;
                listing.money = UpkeepNeeds::Money(listing.copper);
                in.listings.push_back(std::move(listing));
            }
        }
        std::shared_ptr<AuctionIndex::Snapshot const> const index = AuctionIndex::Current();
        for (AuctionRules::BuyLine const& line : Planner::BuyListOf(ai))
        {
            AuctionIndex::ItemEntry const* const entry = index ? index->Find(line.item) : nullptr;
            if (!entry || !AuctionRules::TypicalCopper(line, entry->typical.unitPrice))
                continue;
            Topics::AuctionWant want;
            want.name = nameOf(line.item);
            want.units = line.units;
            want.ceiling = AuctionRules::CeilingOf(line);
            want.money = UpkeepNeeds::Money(want.ceiling);
            in.wants.push_back(std::move(want));
        }
        if (index)
            in.openBids = AuctionRules::OpenBids(*index, bot->GetGUID().GetRawValue());
        in.bidsMax = sPlayerbotAIConfig.Auction.OpenBidsMax;
    }
    Topics::AuctionItems(in, items);
}

static_assert(TopicTable[static_cast<std::size_t>(BotStateTopic::Auction)].Limit == Topics::AUCTION_SHOWN,
              "the rules cut at the topic row's limit");

// WI10b: the bot's usable items, cooldowns, last uses and what it lacks for its role (ItemUse::ListUsable, the last-use
// ring in BotItems, ItemUse::BuildDemand), worded by BridgeItemsTopic.h. Per request, on the world thread
void ReadItems(Player* bot, PlayerbotAI* ai, std::vector<BotStateItem>& items)
{
    auto const nameOf = [](uint32 entry)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        return itemTemplate ? itemTemplate->Name1 : std::string();
    };
    Topics::ItemsInput in;
    in.governed = BotKind::KeepsPossessions(bot);
    std::vector<ItemUse::UsableItem> const usable = ItemUse::ListUsable(bot);
    for (ItemUse::UsableItem const& item : usable)
    {
        std::string const name = nameOf(item.entry);
        in.carried.push_back({std::string(ItemUse::KindWord(static_cast<ItemUse::Kind>(item.kind))), name, item.count});
        if (item.cooldownMs)
            in.cooling.push_back({name, (item.cooldownMs + 999) / 1000});
    }
    std::stable_sort(in.cooling.begin(), in.cooling.end(),
                     [](Topics::ItemsCooling const& a, Topics::ItemsCooling const& b)
                     { return a.seconds > b.seconds; });
    if (ItemUse::BotItems const* kept = ai ? ai->botItems.get() : nullptr)
    {
        uint32 const now = getMSTime();
        for (std::size_t back = 1; back <= kept->lastCount; ++back)
        {
            ItemUse::LastUse const& use =
                kept->lastUses[(kept->lastHead + ItemUse::LAST_USES - back) % ItemUse::LAST_USES];
            in.used.push_back({nameOf(use.entry), use.result, getMSTimeDiff(use.atMs, now) / 1000});
        }
    }
    if (in.governed && ai)
    {
        ItemUse::DemandBuilt const built = ItemUse::BuildDemand(ai, bot);
        for (ItemUse::DemandLine const& line : built.answer.lines)
            if (line.wantedStock > line.held)
                in.lacks.push_back({std::string(ItemUse::DemandSlotWord(line.slot)), line.wantedStock - line.held});
    }
    Topics::ItemsItems(in, items);
}

// What a topic's reader gets
struct TopicSource
{
    Player* Bot;
    PlayerbotAI* Ai;
    std::string const& Argument;
};

using Items = std::vector<BotStateItem>;

struct TopicReader
{
    BotStateTopic Topic;
    void (*Read)(TopicSource const& source, Items& items);
};

// Each topic row's reader (BridgeAbilities.h TopicTable), in the rows' order
constexpr TopicReader Readers[] = {
    {BotStateTopic::Inventory, [](TopicSource const& in, Items& items) { ReadInventory(in.Bot, items); }},
    {BotStateTopic::Quests, [](TopicSource const& in, Items& items) { ReadQuests(in.Bot, items); }},
    {BotStateTopic::Cooldowns, [](TopicSource const& in, Items& items) { ReadCooldowns(in.Bot, items); }},
    {BotStateTopic::Strategies, [](TopicSource const& in, Items& items) { ReadStrategies(in.Ai, items); }},
    {BotStateTopic::Spells, [](TopicSource const& in, Items& items) { ReadSpells(in.Bot, items); }},
    {BotStateTopic::Plan,
     [](TopicSource const& in, Items& items) { Topics::PlanItems(PlanInputOf(in.Bot, in.Ai), items); }},
    {BotStateTopic::Options,
     [](TopicSource const& in, Items& items) { Topics::OptionsItems(PlanInputOf(in.Bot, in.Ai), items); }},
    {BotStateTopic::Route, [](TopicSource const& in, Items& items) { ReadRoute(in.Bot, in.Argument, items); }},
    {BotStateTopic::QuestPlan,
     [](TopicSource const& in, Items& items) { Topics::QuestPlanItems(PlanInputOf(in.Bot, in.Ai), items); }},
    {BotStateTopic::Bags, [](TopicSource const& in, Items& items) { ReadBags(in.Bot, in.Ai, items); }},
    {BotStateTopic::Places, [](TopicSource const& in, Items& items) { ReadPlaces(in.Bot, items); }},
    {BotStateTopic::History, [](TopicSource const& in, Items& items)
     { ReadRecord(in.Bot, &RecordWords::History, &RecordWords::HistoryWord, items); }},
    {BotStateTopic::Lately,
     [](TopicSource const& in, Items& items) { ReadRecord(in.Bot, &LatelyEntries, &RecordWords::DeedWord, items); }},
    {BotStateTopic::Known, [](TopicSource const& in, Items& items)
     { ReadRecord(in.Bot, &RecordWords::KnownBots, &RecordWords::KnownWord, items); }},
    {BotStateTopic::Needs, [](TopicSource const& in, Items& items) { ReadNeeds(in.Bot, in.Ai, items); }},
    {BotStateTopic::Items, [](TopicSource const& in, Items& items) { ReadItems(in.Bot, in.Ai, items); }},
    {BotStateTopic::Where, [](TopicSource const& in, Items& items) { ReadWhere(in.Bot, in.Argument, items); }},
    {BotStateTopic::Who, [](TopicSource const& in, Items& items) { ReadWho(in.Bot, in.Ai, in.Argument, items); }},
    {BotStateTopic::What, [](TopicSource const& in, Items& items) { ReadWhat(in.Bot, in.Argument, items); }},
    {BotStateTopic::Zone, [](TopicSource const& in, Items& items) { ReadZone(in.Bot, in.Argument, items); }},
    {BotStateTopic::Mounts, [](TopicSource const& in, Items& items) { ReadMounts(in.Bot, items); }},
    {BotStateTopic::Recipes, [](TopicSource const& in, Items& items) { ReadRecipes(in.Bot, in.Argument, items); }},
    {BotStateTopic::Professions, [](TopicSource const& in, Items& items) { ReadProfessions(in.Bot, in.Ai, items); }},
    {BotStateTopic::Auction, [](TopicSource const& in, Items& items) { ReadAuction(in.Bot, in.Ai, items); }}};

constexpr bool ReadersInOrder()
{
    for (std::size_t index = 0; index < std::size(Readers); ++index)
        if (Readers[index].Topic != TopicTable[index].Topic)
            return false;

    return std::size(Readers) == std::size(TopicTable);
}
static_assert(ReadersInOrder(), "one reader per topic row, in the rows' order");

// The service of a friendly NPC, the first of its flags in this order; nullptr for none.
char const* ServiceWord(Creature const* creature)
{
    static constexpr std::pair<NPCFlags, char const*> Services[] = {
        {UNIT_NPC_FLAG_INNKEEPER, BridgeTexts::Around::INNKEEPER},
        {UNIT_NPC_FLAG_FLIGHTMASTER, BridgeTexts::Around::FLIGHTMASTER},
        {UNIT_NPC_FLAG_BANKER, BridgeTexts::Around::BANKER},
        {UNIT_NPC_FLAG_AUCTIONEER, BridgeTexts::Around::AUCTIONEER},
        {UNIT_NPC_FLAG_TRAINER, BridgeTexts::Around::TRAINER},
        {UNIT_NPC_FLAG_REPAIR, BridgeTexts::Around::REPAIR},
        {UNIT_NPC_FLAG_VENDOR_MASK, BridgeTexts::Around::VENDOR}};
    for (auto const& [flag, word] : Services)
        if (creature->HasNpcFlag(flag))
            return word;

    return nullptr;
}

// A quest giver's task for the bot (its "can accept / turn in quest npc" values; a game object's qualifier is the
// negative entry): reward before quest; nullptr for none.
char const* TaskWord(AiObjectContext* context, int32 giver)
{
    if (context->GetValue<bool>("can turn in quest npc", giver)->Get())
        return BridgeTexts::Around::TASK_REWARD;
    if (context->GetValue<bool>("can accept quest npc", giver)->Get())
        return BridgeTexts::Around::TASK_QUEST;

    return nullptr;
}

// A perceived unit as a candidate: alive, visible and selectable; a hostile one with its danger, a person, a quest
// giver with a task, a friendly NPC with a service. False for anything else (a plain NPC, a pet, a trigger).
bool UnitCandidate(Player* bot, PlayerbotAI* ai, Unit* unit, float distance, std::vector<AroundCandidate>& out)
{
    if (!unit || unit == bot || !unit->IsAlive() || !Perceives(bot, unit) ||
        unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;
    Creature* creature = unit->ToCreature();
    if (creature && creature->IsTrigger())
        return false;

    std::string const name = unit->GetName();
    if (unit->IsHostileTo(bot) || unit == bot->GetVictim())
    {
        uint32 const rank = creature ? creature->GetCreatureTemplate()->rank : 0;
        std::string const type = creature ? TypeWord(creature->GetCreatureTemplate()->type) : "";
        out.push_back({name, AroundKind::Hostile,
                       HostileDetail(bot->GetLevel(), unit->GetLevel(), Acore::XP::GetGrayLevel(bot->GetLevel()), rank),
                       distance, type});
        return true;
    }
    if (Player* player = unit->ToPlayer())
    {
        PlayerbotAI* other = GET_PLAYERBOT_AI(player);
        char const* const detail = bot->GetGroup() && bot->GetGroup()->IsMember(player->GetGUID())
                                       ? BridgeTexts::Around::PERSON_GROUP
                                   : other && other->IsBotAI() ? BridgeTexts::Around::PERSON_BOT
                                                               : BridgeTexts::Around::PERSON_PLAYER;
        out.push_back({name, AroundKind::Person, detail, distance});
        return true;
    }
    if (!creature)
        return false;

    if (creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
        if (char const* task = TaskWord(ai->GetAiObjectContext(), static_cast<int32>(creature->GetEntry())))
        {
            out.push_back({name, AroundKind::Task, task, distance});
            return true;
        }
    if (char const* service = ServiceWord(creature))
    {
        out.push_back({name, AroundKind::Service, service, distance});
        return true;
    }
    return false;
}

// SpellFocusObject.dbc ids
constexpr uint32 SpellFocusAnvil = 1;
constexpr uint32 SpellFocusForge = 3;
constexpr uint32 SpellFocusFire = 4;

// A useful object: a mailbox, an anvil, a forge or a fire (named by its word when it has no name, as most campfires),
// or a quest-giving object with a task.
void ObjectCandidate(Player* bot, PlayerbotAI* ai, GameObject* object, std::vector<AroundCandidate>& out)
{
    if (!object || !object->IsInWorld() || !object->isSpawned() || !bot->IsInMap(object) || !bot->InSamePhase(object))
        return;
    GameObjectTemplate const* info = object->GetGOInfo();
    char const* detail = nullptr;
    AroundKind kind = AroundKind::Object;
    if (info->type == GAMEOBJECT_TYPE_MAILBOX)
        detail = BridgeTexts::Around::OBJECT_MAILBOX;
    else if (info->type == GAMEOBJECT_TYPE_SPELL_FOCUS)
        detail = info->spellFocus.focusId == SpellFocusAnvil   ? BridgeTexts::Around::OBJECT_ANVIL
                 : info->spellFocus.focusId == SpellFocusForge ? BridgeTexts::Around::OBJECT_FORGE
                 : info->spellFocus.focusId == SpellFocusFire  ? BridgeTexts::Around::OBJECT_FIRE
                                                               : nullptr;
    else if (info->type == GAMEOBJECT_TYPE_QUESTGIVER)
    {
        kind = AroundKind::Task;
        detail = TaskWord(ai->GetAiObjectContext(), -static_cast<int32>(object->GetEntry()));
    }
    if (detail)
        out.push_back({object->GetName().empty() ? std::string(detail) : object->GetName(), kind, detail,
                       bot->GetExactDist(object)});
}

// P §3.3 "around you" from the bot's own values. Cost: five grid visits per state (the four unit lists recalculate on
// every read, the object list when a second old) and the quest values of each quest giver; a state is read at
// request rate (a chat turn, a wake) on the world thread with the maps idle, never from a trigger or per tick.
std::vector<AroundEntry> ReadAround(Player* bot, PlayerbotAI* ai)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    std::vector<AroundCandidate> candidates;
    std::vector<ObjectGuid> seen;
    // The unit fought, then the selection, at distance 0: they lead their name; a unit in two lists counts once.
    for (Unit* own : {bot->GetVictim(), ObjectAccessor::GetUnit(*bot, bot->GetTarget())})
        if (own && std::find(seen.begin(), seen.end(), own->GetGUID()) == seen.end() &&
            UnitCandidate(bot, ai, own, 0.0f, candidates))
            seen.push_back(own->GetGUID());

    for (char const* list : {"possible targets", "nearest hostile npcs", "nearest npcs", "nearest friendly players"})
        for (ObjectGuid const& guid : context->GetValue<GuidVector>(list)->Get())
        {
            if (std::find(seen.begin(), seen.end(), guid) != seen.end())
                continue;
            seen.push_back(guid);
            if (Unit* unit = ObjectAccessor::GetUnit(*bot, guid))
                UnitCandidate(bot, ai, unit, bot->GetExactDist(unit), candidates);
        }

    for (ObjectGuid const& guid : context->GetValue<GuidVector>("nearest game objects")->Get())
        ObjectCandidate(bot, ai, ObjectAccessor::GetGameObject(*bot, guid), candidates);

    return SelectAround(std::move(candidates), bot->GetName());
}
}  // namespace

bool ReadBotState(ObjectGuid const& botGuid, ObjectGuid const& partnerGuid, BotStateFacts& facts)
{
    facts = {};
    // Map threads change these values; only the world thread reads them while the maps are idle.
    if (std::this_thread::get_id() != PlayerbotThreadCheck::worldThread.load(std::memory_order_relaxed))
        return false;
    Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || !ai || !ai->IsBotAI())
        return false;

    facts.healthPct = bot->GetHealthPct();
    Powers const power = bot->getPowerType();
    facts.powerType = static_cast<uint8_t>(power);
    facts.powerCurrent = bot->GetPower(power);
    facts.powerMax = bot->GetMaxPower(power);
    facts.level = bot->GetLevel();
    facts.alive = bot->IsAlive();
    facts.ghost = bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    facts.inCombat = bot->IsInCombat() || ai->GetState() == BOT_STATE_COMBAT;
    if (Unit* victim = bot->GetVictim())
    {
        facts.victim = true;
        facts.victimVisible = Perceives(bot, victim);
        facts.victimName = victim->GetName();
        facts.victimLevel = victim->GetLevel();
        facts.victimHealthPct = victim->GetHealthPct();
        if (Creature const* creature = victim->ToCreature())
            facts.victimType = TypeWord(creature->GetCreatureTemplate()->type);
    }

    facts.flying = bot->IsInFlight();
    facts.casting = bot->IsNonMeleeSpellCast(false);
    AiObjectContext* context = ai->GetAiObjectContext();
    facts.looting = !context->GetValue<LootObject>("loot target")->Get().IsEmpty();
    facts.resting =
        bot->IsSitState() && (bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN));
    // Status fields only: TravelTarget::isActive() asks the destination, which evaluates calculated values.
    if (TravelTarget* travel = context->GetValue<TravelTarget*>("travel target")->Get())
    {
        facts.travelling = travel->getStatus() == TRAVEL_STATUS_TRAVEL;
        facts.working = travel->getStatus() == TRAVEL_STATUS_WORK;
        TravelDestination* destination = travel->getDestination();
        Quest const* quest =
            (facts.travelling || facts.working) && destination ? destination->GetQuestTemplate() : nullptr;
        if (quest)
            facts.questTitle = quest->GetTitle();
    }

    facts.zone = AreaName(bot->GetZoneId());
    facts.area = AreaName(bot->GetAreaId());
    facts.mapId = bot->GetMapId();
    facts.x = bot->GetPositionX();
    facts.y = bot->GetPositionY();
    facts.z = bot->GetPositionZ();
    facts.mounted = bot->IsMounted();
    facts.swimming = bot->IsInWater();

    Group* group = bot->GetGroup();
    if (group)
    {
        facts.groupSize = group->GetMembersCount();
        facts.leads = group->GetLeaderGUID() == bot->GetGUID();
    }
    if (Player* master = ai->GetMaster(); master && master != bot)
        facts.masterName = master->GetName();
    facts.follow = ai->HasStrategy("follow", BOT_STATE_NON_COMBAT);
    facts.stay = ai->HasStrategy("stay", BOT_STATE_NON_COMBAT);
    facts.guard = ai->HasStrategy("guard", BOT_STATE_NON_COMBAT);
    facts.grind = ai->HasStrategy("grind", BOT_STATE_NON_COMBAT);
    facts.passive = ai->HasStrategy("passive", BOT_STATE_NON_COMBAT);
    // The defaults are FormationValue's chaos formation and ManaSaveLevelValue's 1.0; nothing is stored here.
    if (UntypedValue* formation = context->GetUntypedValue("formation"))
        facts.formation = formation->Save() != "chaos";
    if (Value<double>* level = context->GetValue<double>("mana save level"))
        facts.saveMana = level->Get() > 1.0;
    if (Value<std::list<ObjectGuid>>* targets = context->GetValue<std::list<ObjectGuid>>("focus heal targets"))
        facts.focusHeal = !targets->Get().empty();
    facts.freeBagSlots = bot->GetFreeInventorySpace();
    facts.money = bot->GetMoney();

    Player* partner = ObjectAccessor::FindConnectedPlayer(partnerGuid);
    if (partner && partner != bot && partner->IsInWorld())
    {
        facts.partnerOnline = true;
        facts.partnerInGroup = group && group->IsMember(partnerGuid);
        facts.partnerVisible = Perceives(bot, partner);
        facts.partnerZone = AreaName(partner->GetZoneId());
        facts.partnerSameMap = bot->IsInMap(partner) && bot->InSamePhase(partner);
        if (facts.partnerSameMap)
        {
            facts.partnerYards = bot->GetExactDist(partner);
            facts.partnerX = partner->GetPositionX();
            facts.partnerY = partner->GetPositionY();
            facts.partnerZ = partner->GetPositionZ();
        }
    }

    facts.around = ReadAround(bot, ai);
    if (Guardian* servant = bot->GetGuardianPet(); servant && servant->IsInWorld() && servant->IsAlive())
        facts.servantName = servant->GetName();
    Unit::AuraEffectList const& mounts = bot->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    if (!mounts.empty() && mounts.front()->GetSpellInfo()->SpellName[0])
        facts.mountName = mounts.front()->GetSpellInfo()->SpellName[0];
    else if (Unit* vehicle = bot->GetVehicleBase())
        facts.mountName = vehicle->GetName();
    uint32 const self = bot->GetGUID().GetCounter();
    facts.deeds = RecordWords::Deeds(BotRecordMgr::Instance().Rows(self), self,
                                     static_cast<uint32>(GameTime::GetGameTime().count()), GameLookup());
    return true;
}

bool ReadBotStateTopic(ObjectGuid const& botGuid, BotStateTopic topic, std::vector<BotStateItem>& items)
{
    return ReadBotStateTopic(botGuid, topic, std::string(), items);
}

bool ReadBotStateTopic(ObjectGuid const& botGuid, BotStateTopic topic, std::string const& argument,
                       std::vector<BotStateItem>& items)
{
    items.clear();
    if (std::this_thread::get_id() != PlayerbotThreadCheck::worldThread.load(std::memory_order_relaxed))
        return false;
    Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || !ai || !ai->IsBotAI() || !FindTopic(topic))
        return false;

    Readers[static_cast<std::size_t>(topic)].Read({bot, ai, argument}, items);
    if (items.size() > BotStateTopicMaxItems)
        items.resize(BotStateTopicMaxItems);
    return true;
}

std::optional<std::string> KnownTownSpot(Player* bot, std::string const& text)
{
    std::shared_ptr<Planner::PlannerIndexes const> const index = Planner::Indexes();
    if (!index)
        return std::nullopt;
    Knowledge::MapPoint const from = PointOf(bot);
    return Knowledge::TownSpot(KnowledgeOf(bot), Knowledge::TownCandidates(*index, from, &AreaName), from, text);
}

std::vector<NameCandidate> CarriedItemNames(Player* bot)
{
    std::vector<NameCandidate> candidates;
    auto const add = [&candidates](Item* item)
    {
        if (item)
            candidates.push_back({item->GetTemplate()->Name1, item->GetEntry(), 0.0f});
    };
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                add(bag->GetItemByPos(static_cast<uint8>(slot)));
    return candidates;
}

std::vector<NameCandidate> KnownSpellNames(Player* bot)
{
    std::vector<NameCandidate> candidates;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active || !known->IsInSpec(bot->GetActiveSpec()))
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!spell || spell->IsPassive() || spell->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) ||
            spell->SpellFamilyName == SPELLFAMILY_GENERIC || !spell->SpellName[0])
            continue;
        SpellInfo const* next = spell->GetNextRankSpell();
        if (next && bot->HasSpell(next->Id))
            continue;
        candidates.push_back({spell->SpellName[0], spellId, 0.0f});
    }
    return candidates;
}
}  // namespace PlayerbotBridge
