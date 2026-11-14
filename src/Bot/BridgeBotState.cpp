/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeBotState.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>

#include "Bag.h"
#include "BankMailState.h"
#include "BotRecordMgr.h"
#include "BridgeAbilities.h"
#include "BridgeAround.h"
#include "BridgeBotTopics.h"
#include "BridgeNames.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "LootObjectStack.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Pet.h"
#include "PlannerService.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "RecordWords.h"
#include "RouteService.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelMgr.h"
#include "TravelPlaces.h"
#include "World.h"

namespace PlayerbotBridge
{
namespace
{
bool Visible(Player* observer, Unit* target)
{
    return target && target->IsInWorld() && observer->IsInMap(target) && observer->InSamePhase(target) &&
           observer->IsWithinDistInMap(target, observer->GetSightRange()) &&
           observer->CanSeeOrDetect(target, false, true);
}

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
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
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

// The planner topics read the view the bot's map thread published; out of the planner's scope (a real master, not
// persistent, Planner.Goals off) the planner sets no goal, so they show none
Topics::PlanInput PlanInputOf(Player* bot, PlayerbotAI* ai)
{
    Topics::PlanInput in;
    in.inScope = sPlayerbotAIConfig.WorldTravel.PlannerGoals && !ai->HasGameClientMaster() &&
                 PlayerbotFactory::KeepsPossessions(bot);
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
    BankMail::BotBankMail const& known = ai->rpgInfo.bankMail;
    in.mailWaiting = BankMail::CountWaitingMail(bot);
    in.mailSeenCopper = known.mail.seenCopper;
    in.mailExpectedCopper = known.mail.expectedCopper;
    in.keepsPossessions = PlayerbotFactory::KeepsPossessions(bot);
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
        in.bankSlots = BankMail::BANK_ITEM_SLOTS;
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
    in.place = named ? named->name : "position";
    if (goal)
    {
        auto const started = std::chrono::steady_clock::now();
        TravelRoute::Request const request(TravelRoute::FillSnapshot(bot, 0), *goal);
        std::shared_ptr<TravelRoute::StuckEdges const> const stuck = TravelRoute::StuckBook::Instance().Current();
        // The ground route (no fly legs): flight searches run only on the route service's workers (contract C3)
        in.route = TravelRoute::GroundSearch(TravelGraph::Instance(), request, *stuck, TravelRoute::ConfiguredParams());
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

void ReadRecord(Player* bot, RecordReader reader, std::vector<BotStateItem>& items)
{
    uint32 const self = bot->GetGUID().GetCounter();
    std::vector<RecordRules::Row> const rows = BotRecordMgr::Instance().Rows(self);
    uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());
    for (RecordWords::Entry& entry : reader(rows, self, now, GameLookup()))
        items.push_back({std::move(entry.Name), entry.Value, entry.Extra, entry.Code, std::move(entry.Text)});
}

std::vector<RecordWords::Entry> LatelyEntries(std::vector<RecordRules::Row> const& rows, uint32 self, uint32 now,
                                              RecordWords::Lookup const& lookup)
{
    return RecordWords::Lately(rows, self, now, lookup);
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
    {BotStateTopic::History,
     [](TopicSource const& in, Items& items) { ReadRecord(in.Bot, &RecordWords::History, items); }},
    {BotStateTopic::Lately, [](TopicSource const& in, Items& items) { ReadRecord(in.Bot, &LatelyEntries, items); }},
    {BotStateTopic::Known,
     [](TopicSource const& in, Items& items) { ReadRecord(in.Bot, &RecordWords::KnownBots, items); }}};

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
        {UNIT_NPC_FLAG_INNKEEPER, "innkeeper"}, {UNIT_NPC_FLAG_FLIGHTMASTER, "flightmaster"},
        {UNIT_NPC_FLAG_BANKER, "banker"},       {UNIT_NPC_FLAG_AUCTIONEER, "auctioneer"},
        {UNIT_NPC_FLAG_TRAINER, "trainer"},     {UNIT_NPC_FLAG_REPAIR, "repair"},
        {UNIT_NPC_FLAG_VENDOR_MASK, "vendor"}};
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
        return "reward";
    if (context->GetValue<bool>("can accept quest npc", giver)->Get())
        return "quest";

    return nullptr;
}

// A perceived unit as a candidate: alive, visible and selectable; a hostile one with its danger, a person, a quest
// giver with a task, a friendly NPC with a service. False for anything else (a plain NPC, a pet, a trigger).
bool UnitCandidate(Player* bot, PlayerbotAI* ai, Unit* unit, float distance, std::vector<AroundCandidate>& out)
{
    if (!unit || unit == bot || !unit->IsAlive() || !Visible(bot, unit) || unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;
    Creature* creature = unit->ToCreature();
    if (creature && creature->IsTrigger())
        return false;

    std::string const name = unit->GetName();
    if (unit->IsHostileTo(bot) || unit == bot->GetVictim())
    {
        uint32 const rank = creature ? creature->GetCreatureTemplate()->rank : 0;
        out.push_back({name, AroundKind::Hostile,
                       HostileDetail(bot->GetLevel(), unit->GetLevel(), Acore::XP::GetGrayLevel(bot->GetLevel()), rank),
                       distance});
        return true;
    }
    if (Player* player = unit->ToPlayer())
    {
        PlayerbotAI* other = GET_PLAYERBOT_AI(player);
        char const* const detail = bot->GetGroup() && bot->GetGroup()->IsMember(player->GetGUID()) ? "group"
                                   : other && other->IsBotAI()                                     ? "bot"
                                                                                                   : "player";
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
        detail = "mailbox";
    else if (info->type == GAMEOBJECT_TYPE_SPELL_FOCUS)
        detail = info->spellFocus.focusId == SpellFocusAnvil   ? "anvil"
                 : info->spellFocus.focusId == SpellFocusForge ? "forge"
                 : info->spellFocus.focusId == SpellFocusFire  ? "fire"
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
        facts.victimVisible = Visible(bot, victim);
        facts.victimName = victim->GetName();
        facts.victimLevel = victim->GetLevel();
        facts.victimHealthPct = victim->GetHealthPct();
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
    facts.freeBagSlots = bot->GetFreeInventorySpace();
    facts.money = bot->GetMoney();

    Player* partner = ObjectAccessor::FindConnectedPlayer(partnerGuid);
    if (partner && partner != bot && partner->IsInWorld())
    {
        facts.partnerOnline = true;
        facts.partnerInGroup = group && group->IsMember(partnerGuid);
        facts.partnerVisible = Visible(bot, partner);
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
