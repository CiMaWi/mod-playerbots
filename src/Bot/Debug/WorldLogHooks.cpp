/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <mutex>
#include <unordered_map>

#include "BotRecordMgr.h"
#include "CombatTrace.h"
#include "Creature.h"
#include "DBCStores.h"
#include "EventLog.h"
#include "EventWanted.h"
#include "Item.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestStatEvents.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldLogBotState.h"
#include "WorldLogStatus.h"
#include "WorldLogWriter.h"

// The world log's fact rows (XpGain, Kill, QuestAccept, QuestAbandon, Equip, SkillUp, SpellLearn, TaxiNode, Hearth,
// MapChange, Resurrect) from core hooks, of bots and real players alike, and the bots' Status and Flight rows. Every
// row goes through EventLog::Emit (WorldLogWriter::OnEvent). The hooks of the events the bots' record takes
// (BotRecordMgr) are gated by EventLog::Wanted, so they also run with the world log off. Registered after
// PlayerbotsPlayerScript, whose GiveXP hook applies the XP multipliers first. Hooks run on the player's map thread (or
// the world thread), read only the hooked player and keep only guid-keyed notes between two hooks of one core call.
// WL7's decision rows (WorldLog::Decision, called from the RPG actions) go to the decision stream the same way:
// formatted on the calling thread, then queued.
namespace
{
constexpr uint32 SPELL_HEARTHSTONE = 8690;
constexpr uint32 MS_PER_SECOND = 1000;
// A debit at most this long before a learn paid for it (Trainer::TeachSpell debits, then teaches)
constexpr uint32 LESSON_MS = 1;
constexpr int64 ONE_EVENT = 1;

// OnPlayerQuestComputeXP -> OnPlayerGiveXP inside Player::RewardQuest: the quest the XP is for
struct QuestNote
{
    ObjectGuid::LowType player = 0;
    uint32 questId = 0;
};

// OnPlayerMoneyChanged -> OnPlayerLearnSpell inside Trainer::TeachSpell: the lesson's price
struct DebitNote
{
    ObjectGuid::LowType player = 0;
    uint32 cost = 0;
    uint32 ms = 0;
};

// OnPlayerCanEquipItem (open) -> OnPlayerCanUnequipItem (the item leaving the slot) -> OnPlayerEquip
struct EquipNote
{
    ObjectGuid::LowType player = 0;
    uint8 slot = NULL_SLOT;
    uint32 oldLevel = 0;
    bool open = false;
};

thread_local QuestNote questNote;
thread_local DebitNote debitNote;
thread_local EquipNote equipNote;
thread_local char const* questDropWord = nullptr;  // WorldLog::QuestDropWord

// The map a far teleport leaves, by guid: OnPlayerBeforeTeleport and OnPlayerMapChanged may run on different threads.
// An entry lives from the teleport to the map change or the logout.
std::mutex oldMapsMutex;
std::unordered_map<ObjectGuid::LowType, uint32> oldMaps;

ObjectGuid::LowType LowGuid(Player const* player) { return player->GetGUID().GetCounter(); }

uint32 ItemLevelAt(Player* player, uint8 slot)
{
    Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    return proto ? proto->ItemLevel : 0;
}

EventLog::MapKind MapKindOf(Map const* map)
{
    if (!map)
        return EventLog::MapKind::World;
    if (map->IsBattleArena())
        return EventLog::MapKind::Arena;
    if (map->IsBattleground())
        return EventLog::MapKind::Battleground;
    if (map->IsRaid())
        return EventLog::MapKind::Raid;
    if (map->IsDungeon())
        return EventLog::MapKind::Dungeon;

    return EventLog::MapKind::World;
}

std::string_view ScopeWord() { return EventLog::SourceWordOf(EventLog::CurrentSource()); }

void EmitKill(Player* killer, Creature* killed)
{
    if (!EventLog::Wanted(EventLog::EventType::Kill) || !killer || !killed)
        return;

    CreatureTemplate const* creature = killed->GetCreatureTemplate();
    EventLog::Emit(killer, EventLog::EventType::Kill, EventLog::RankWord(creature ? creature->rank : 0),
                   killed->GetEntry(), static_cast<int64>(killed->GetLevel()) - static_cast<int64>(killer->GetLevel()));
}

// Where a PvP kill or death happened, from the player's own state
char const* PvpPlace(Player* player)
{
    if (player->InArena())
        return "arena";

    return player->InBattleground() ? "battleground" : "world";
}

// An economy or life event whose source is the cause on this thread (the executing action, mapped to a source word)
void EmitFromScope(Player* player, EventLog::EventType type, uint32 id, int64 amount)
{
    if (!player || !EventLog::Wanted(type))
        return;

    WorldLogWriter::Instance().OnEvent(player, type, EventLog::SourceWordOf(EventLog::CurrentSource()), id, amount);
}

// Set from OnPlayerBeforeLootMoney to the loot script's OnLootMoney on the thread that handles the loot window: the
// money changes between them are the loot's gold (the looter's or the group's shares)
thread_local bool lootMoneyOpen = false;

// ItemIn for the item StoreNewItem stored just before this hook: reads the note, never the item (TakeStoredEntry)
void EmitStoredItemIn(Player* player, Item* item, uint32 count, char const* source)
{
    if (uint32 const entry = EventLog::TakeStoredEntry(item))
        EventLog::Emit(player, EventLog::EventType::ItemIn, source, entry, count);
}

class WorldLogPlayerScript : public PlayerScript
{
public:
    WorldLogPlayerScript()
        : PlayerScript("WorldLogPlayerScript", {PLAYERHOOK_ON_QUEST_COMPUTE_EXP,
                                                PLAYERHOOK_ON_GIVE_EXP,
                                                PLAYERHOOK_ON_CREATURE_KILL,
                                                PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
                                                PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
                                                PLAYERHOOK_ON_QUEST_ABANDON,
                                                PLAYERHOOK_CAN_EQUIP_ITEM,
                                                PLAYERHOOK_CAN_UNEQUIP_ITEM,
                                                PLAYERHOOK_ON_EQUIP,
                                                PLAYERHOOK_ON_UPDATE_SKILL,
                                                PLAYERHOOK_ON_MONEY_CHANGED,
                                                PLAYERHOOK_ON_LEARN_SPELL,
                                                PLAYERHOOK_ON_AFTER_TRAIN_SPELL,
                                                PLAYERHOOK_ON_LEARN_TAXI_NODE,
                                                PLAYERHOOK_ON_SPELL_CAST,
                                                PLAYERHOOK_ON_BEFORE_TELEPORT,
                                                PLAYERHOOK_ON_MAP_CHANGED,
                                                PLAYERHOOK_ON_UPDATE_ZONE,
                                                PLAYERHOOK_ON_LOGOUT,
                                                PLAYERHOOK_ON_PLAYER_RESURRECT,
                                                PLAYERHOOK_ON_PLAYER_ENTER_COMBAT,
                                                PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT,
                                                PLAYERHOOK_ON_LEVEL_CHANGED,
                                                PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
                                                PLAYERHOOK_ON_STORE_NEW_ITEM,
                                                PLAYERHOOK_ON_LOOT_ITEM,
                                                PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM,
                                                PLAYERHOOK_ON_QUEST_REWARD_ITEM,
                                                PLAYERHOOK_ON_CREATE_ITEM,
                                                PLAYERHOOK_ON_AFTER_STORE_OR_EQUIP_NEW_ITEM,
                                                PLAYERHOOK_ON_AFTER_TAKE_ITEM_FROM_MAIL,
                                                PLAYERHOOK_CAN_SELL_ITEM,
                                                PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
                                                PLAYERHOOK_ON_PVP_KILL,
                                                PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE})
    {
    }

    void OnPlayerQuestComputeXP(Player* player, Quest const* quest, uint32& /*xpValue*/) override
    {
        if (EventLog::Wanted(EventLog::EventType::XpGain) && player && quest)
            questNote = {LowGuid(player), quest->GetQuestId()};
    }

    // The XP offered after the module's multipliers; GiveXP may still refuse it (dead, no loot recipient) or add the
    // rested bonus
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        if (!EventLog::Wanted(EventLog::EventType::XpGain) || !player || !amount ||
            player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) ||
            player->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN))
            return;

        uint32 id = 0;
        if (xpSource == XPSOURCE_KILL && victim && victim->IsCreature())
            id = victim->GetEntry();
        else if ((xpSource == XPSOURCE_QUEST || xpSource == XPSOURCE_QUEST_DF) && questNote.player == LowGuid(player))
            id = questNote.questId;

        questNote = {};
        EventLog::Emit(player, EventLog::EventType::XpGain, EventLog::XpSourceWord(xpSource), id, amount);
    }

    // Killing blows of the player and of its pets and totems
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override { EmitKill(killer, killed); }

    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override { EmitKill(owner, killed); }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (EventLog::Wanted(EventLog::EventType::QuestAccept) && player && quest)
            EventLog::Emit(player, EventLog::EventType::QuestAccept, EventLog::SOURCE_OTHER, quest->GetQuestId(),
                           player->GetQuestLevel(quest));
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        WorldLog::NoteQuestAbandon(player, questId, questDropWord ? questDropWord : WorldLog::QUEST_ABANDON_CLIENT);
    }

    // Every equip check opens the note (bots run it when they judge items too); the equip that follows reads it
    [[nodiscard]] bool OnPlayerCanEquipItem(Player* player, uint8 /*slot*/, uint16& /*dest*/, Item* /*item*/,
                                            bool /*swap*/, bool notLoading) override
    {
        if (EventLog::Wanted(EventLog::EventType::Equip) && player && notLoading)
            equipNote = {LowGuid(player), NULL_SLOT, 0, true};

        return true;
    }

    // Runs before the item leaves an occupied equipment slot: its level is the old one
    [[nodiscard]] bool OnPlayerCanUnequipItem(Player* player, uint16 pos, bool /*swap*/) override
    {
        if (EventLog::Wanted(EventLog::EventType::Equip) && player && equipNote.open &&
            equipNote.player == LowGuid(player) && Player::IsEquipmentPos(pos))
        {
            equipNote.slot = static_cast<uint8>(pos & 0xFF);
            equipNote.oldLevel = ItemLevelAt(player, equipNote.slot);
        }

        return true;
    }

    // amount: the new item's level minus the old one's (an empty slot counts 0); 0 when the old one is not known
    void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 slot, bool /*update*/) override
    {
        if (!EventLog::Wanted(EventLog::EventType::Equip) || !player || !item || !player->IsInWorld())
            return;

        EquipNote const note = equipNote;
        equipNote = {};
        ItemTemplate const* proto = item->GetTemplate();
        int64 const newLevel = proto ? proto->ItemLevel : 0;
        int64 change = 0;
        if (note.open && note.player == LowGuid(player))
        {
            if (note.slot == slot)
                change = newLevel - note.oldLevel;
            else if (note.slot == NULL_SLOT)
                change = newLevel;
        }

        WorldLogWriter::Instance().OnEvent(player, EventLog::EventType::Equip, ScopeWord(), item->GetEntry(), change);
    }

    void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 value, uint32 /*max*/, uint32 /*step*/,
                             uint32 newValue) override
    {
        if (EventLog::Wanted(EventLog::EventType::SkillUp) && player && newValue > value)
            EventLog::Emit(player, EventLog::EventType::SkillUp, EventLog::SOURCE_OTHER, skillId, newValue);
    }

    // Called before the change; a loss is capped at the money the player has, as ModifyMoney does. Looted gold is
    // paid while the loot window's money is handled (the bots' loot action only queues the packet), so its source
    // comes from that window, not from the action scope. A debit is also the price of a lesson the next learn may
    // be: noted whenever SpellLearn has a reader, so with the world log off the record's lesson keeps its cost
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (!player)
            return;

        if (amount > 0 && lootMoneyOpen)
            EventLog::Emit(player, EventLog::EventType::GoldIn, "loot", 0, amount);
        else if (amount > 0)
            EmitFromScope(player, EventLog::EventType::GoldIn, 0, amount);
        else if (amount < 0 && player->GetMoney())
            EmitFromScope(player, EventLog::EventType::GoldOut, 0,
                          std::min<int64>(-static_cast<int64>(amount), player->GetMoney()));

        if (amount < 0 && EventLog::Wanted(EventLog::EventType::SpellLearn))
            debitNote = {LowGuid(player), static_cast<uint32>(-static_cast<int64>(amount)), getMSTime()};
    }

    // Character creation and login add spells without this hook; a bot's level-up spells come through it
    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        if (!EventLog::Wanted(EventLog::EventType::SpellLearn) || !player || !player->IsInWorld())
            return;

        bool const paid = debitNote.player == LowGuid(player) && getMSTimeDiff(debitNote.ms, getMSTime()) <= LESSON_MS;
        uint32 const cost = paid ? debitNote.cost : 0;
        if (paid)
            debitNote.cost = 0;  // the spells the lesson teaches with it are free

        EventLog::Emit(player, EventLog::EventType::SpellLearn,
                       EventLog::SpellLearnWord(ScopeWord(), paid, GetTalentSpellCost(spellId) > 0), spellId, cost);
    }

    // The lesson is over: its debit pays for no later learn
    void OnPlayerAfterTrainSpell(Player* /*player*/, Creature* /*trainer*/, uint32 /*spellId*/) override
    {
        debitNote = {};
    }

    // The core passes a const player; the row only reads it
    void OnPlayerLearnTaxiNode(Player const* player, uint32 nodeId) override
    {
        if (EventLog::Wanted(EventLog::EventType::TaxiNode) && player)
            EventLog::Emit(const_cast<Player*>(player), EventLog::EventType::TaxiNode, EventLog::SOURCE_OTHER, nodeId,
                           0);
    }

    // Every cast of every player: two compares unless it is the hearthstone
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!EventLog::Wanted(EventLog::EventType::Hearth) || !player || !spell ||
            spell->GetSpellInfo()->Id != SPELL_HEARTHSTONE)
            return;

        EventLog::Emit(player, EventLog::EventType::Hearth, EventLog::SOURCE_OTHER, player->m_homebindAreaId, 0);
    }

    [[nodiscard]] bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float /*x*/, float /*y*/, float /*z*/,
                                              float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        if (EventLog::Wanted(EventLog::EventType::MapChange) && player && mapId != player->GetMapId())
        {
            std::lock_guard<std::mutex> lock(oldMapsMutex);
            oldMaps[LowGuid(player)] = player->GetMapId();
        }

        return true;
    }

    // Also runs at login (no teleport noted: no row)
    void OnPlayerMapChanged(Player* player) override
    {
        if (!EventLog::Wanted(EventLog::EventType::MapChange) || !player)
            return;

        uint32 oldMap = 0;
        {
            std::lock_guard<std::mutex> lock(oldMapsMutex);
            auto const found = oldMaps.find(LowGuid(player));
            if (found == oldMaps.end())
                return;

            oldMap = found->second;
            oldMaps.erase(found);
        }

        EventLog::Emit(player, EventLog::EventType::MapChange, EventLog::MapKindWord(MapKindOf(player->GetMap())),
                       player->GetMapId(), oldMap);
    }

    // LC18e: every zone entry, also the arrival on a new map (Player::UpdateZone after the worldport): a bot's visits
    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
    {
        if (EventLog::Wanted())
            BotRecordMgr::Instance().OnZoneEnter(player, newZone, newArea);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!EventLog::Wanted() || !player)
            return;

        CombatTally::OnLeave(player, true);
        BotRecordMgr::Instance().OnCombatLeave(player, true);
        std::lock_guard<std::mutex> lock(oldMapsMutex);
        oldMaps.erase(LowGuid(player));
    }

    // Real edges only (CombatManager::UpdateOwnerCombatState); Unit::ClearInCombat can repeat the end
    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (!EventLog::Wanted())
            return;

        CombatTally::OnEnter(player);
        BotRecordMgr::Instance().OnCombatEnter(player);
    }

    void OnPlayerLeaveCombat(Player* player) override
    {
        if (!EventLog::Wanted())
            return;

        CombatTally::OnLeave(player, false);
        BotRecordMgr::Instance().OnCombatLeave(player, false);
    }

    void OnPlayerResurrect(Player* player, float restorePercent, bool& applySickness) override
    {
        if (EventLog::Wanted(EventLog::EventType::Resurrect) && player)
            EventLog::Emit(player, EventLog::EventType::Resurrect,
                           EventLog::ResurrectWord(ScopeWord(), applySickness, restorePercent), 0, 0);
    }

    // Level-up and economy hooks; each does nothing while its type has no reader (EventLog::Wanted). They run on the
    // thread that updates the player's map (or the world thread): they read only the hooked players and queue one line
    // each.
    // StoreNewItem is not counted (loot, quest, vendor and craft items pass through it before their own hook below); it
    // only notes the stored item's entry for those hooks. Master-loot items and disenchant-roll results have no hook of
    // their own and are not counted as ItemIn.
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        WorldLogWriter::Instance().OnLevelChanged(player, oldLevel);
    }

    void OnPlayerBeforeLootMoney(Player* /*player*/, Loot* /*loot*/) override { lootMoneyOpen = true; }

    // Inside StoreNewItem, before the scripts' item hooks: the item is alive here (the core's callers use it after
    // this hook), so its entry is read now for the hooks below, which must not read the item
    void OnPlayerStoreNewItem(Player* /*player*/, Item* item, uint32 /*count*/) override
    {
        if (EventLog::Wanted(EventLog::EventType::ItemIn) && item)
            EventLog::NoteStoredItem(item, item->GetEntry());
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        EmitStoredItemIn(player, item, count, "loot");
    }

    // A won need or greed roll: Group stores the item itself and fires only this hook, not OnPlayerLootItem
    void OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 count, RollVote /*voteType*/,
                                     Roll* /*roll*/) override
    {
        EmitStoredItemIn(player, item, count, "loot");
    }

    void OnPlayerQuestRewardItem(Player* player, Item* item, uint32 count) override
    {
        EmitStoredItemIn(player, item, count, "quest");
    }

    void OnPlayerCreateItem(Player* player, Item* item, uint32 count) override
    {
        EmitStoredItemIn(player, item, count, "craft");
    }

    // count is the number of vendor stacks bought; the item is null when the store failed and is never read (the
    // template is static data)
    void OnPlayerAfterStoreOrEquipNewItem(Player* player, uint32 /*vendorslot*/, Item* item, uint8 count, uint8 /*bag*/,
                                          uint8 /*slot*/, ItemTemplate const* proto, Creature* /*vendor*/,
                                          VendorItem const* /*crItem*/, bool /*bStore*/) override
    {
        if (item && proto)
            EventLog::Emit(player, EventLog::EventType::ItemIn, "vendor", proto->ItemId,
                           static_cast<int64>(proto->BuyCount) * count);
    }

    // A mail item is moved into the bags, not stored new, so there is no note and the item is read here: safe while
    // no script registered before this one destroys the taken item in this hook (none does)
    void OnPlayerAfterTakeItemFromMail(Player* player, Item* item, uint32 count) override
    {
        if (item && player && EventLog::Wanted(EventLog::EventType::ItemIn))
            EventLog::Emit(player, EventLog::EventType::ItemIn, "mail", item->GetEntry(), count);
    }

    // "sold" ItemOut is an upper bound: a later check of the sell handler can still refuse the sale, and the whole
    // stack is counted where SellAction sells only the part that quest items do not need. The item is alive here:
    // the sell handler reads it after this hook.
    [[nodiscard]] bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
    {
        if (item)
            EventLog::Emit(player, EventLog::EventType::ItemOut, "sold", item->GetEntry(), item->GetCount());

        return true;
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (quest)
            EventLog::Emit(player, EventLog::EventType::QuestDone, "quest", quest->GetQuestId(), ONE_EVENT);
    }

    // Environmental damage (falling, lava, drowning) is dealt by the player to itself
    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        Planner::QuestStatDeath(killed);
        NoteTravelDeath(killed);
        if (killer == killed)
        {
            EventLog::Emit(killed, EventLog::EventType::Death, "environment", 0, ONE_EVENT);
            return;
        }

        uint32 const killerId = killer->GetGUID().GetCounter();
        EventLog::Emit(killed, EventLog::EventType::Death, "player", killerId, ONE_EVENT);
        EventLog::Emit(killed, EventLog::EventType::PvpDeath, PvpPlace(killed), killerId, ONE_EVENT);
        EventLog::Emit(killer, EventLog::EventType::PvpKill, PvpPlace(killer), killed->GetGUID().GetCounter(),
                       ONE_EVENT);
    }

    // A player's pet, totem or guardian is a creature: its kill is a PvP death with the owner as the killer
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        Planner::QuestStatDeath(killed);
        NoteTravelDeath(killed);
        ObjectGuid const owner = killer->GetOwnerGUID();
        if (!owner.IsPlayer())
        {
            EventLog::Emit(killed, EventLog::EventType::Death, "creature", killer->GetEntry(), ONE_EVENT);
            return;
        }

        EventLog::Emit(killed, EventLog::EventType::Death, "player_pet", owner.GetCounter(), ONE_EVENT);
        EventLog::Emit(killed, EventLog::EventType::PvpDeath, PvpPlace(killed), owner.GetCounter(), ONE_EVENT);
    }
};

// Ends the loot money window OnPlayerBeforeLootMoney opened: the core calls it after paying the gold
class WorldLogWriterLootScript : public LootScript
{
public:
    WorldLogWriterLootScript() : LootScript("WorldLogWriterLootScript", {LOOTHOOK_ON_LOOT_MONEY}) {}

    void OnLootMoney(Player* /*player*/, uint32 /*gold*/) override { lootMoneyOpen = false; }
};
}  // namespace

void WorldLog::NoteStatus(Player* bot, int32_t oldStatus, int32_t newStatus, char const* word)
{
    if (oldStatus == newStatus || !bot || !WorldLog::Active())
        return;

    EventLog::Emit(bot, EventLog::EventType::Status, word ? word : WorldLog::STATUS_REASON_OTHER,
                   static_cast<uint32>(newStatus), oldStatus);
}

void WorldLog::Decision(Player* bot, WorldLogFormat::DecisionKind kind, std::string_view choice,
                        std::string_view reason, float value, WorldLogFormat::Alternatives const& alternatives,
                        uint32 ref)
{
    if (!bot || !WorldLog::Active())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog decision");

    WorldLogFormat::DecisionRow row;
    row.t = WorldLog::NowMs();
    row.bot = bot->GetGUID().GetCounter();
    row.kind = kind;
    row.choice = choice;
    row.reason = reason;
    row.value = value;
    row.alternatives = alternatives;
    row.ref = ref;
    WorldLogWriter::Instance().EnqueueWorldLog(WorldLogFormat::Stream::Decision, WorldLogFormat::FormatDecisionRow(row),
                                               1, row.t);

    if (pmo)
        pmo->finish();
}

void WorldLog::NoteQuestAbandon(Player* bot, uint32_t questId, char const* reason)
{
    if (!WorldLog::Active() || !bot)
        return;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    EventLog::Emit(bot, EventLog::EventType::QuestAbandon, reason ? reason : EventLog::SOURCE_OTHER, questId,
                   quest ? bot->GetQuestLevel(quest) : 0);
}

WorldLog::QuestDropWord::QuestDropWord(char const* reason) : _previous(questDropWord) { questDropWord = reason; }

WorldLog::QuestDropWord::~QuestDropWord() { questDropWord = _previous; }

void WorldLog::NoteFlightEdge(Player* bot, std::unique_ptr<BotLog>& log, bool flying)
{
    if (!bot || !WorldLog::Active())
        return;

    if (!log)
        log = std::make_unique<BotLog>();

    log->inFlight = flying;
    if (flying)
    {
        std::deque<uint32> const& path = bot->m_taxi.GetPath();
        log->startedMs = getMSTime();
        log->fromNode = bot->m_taxi.GetTaxiSource();
        log->toNode = path.empty() ? 0 : path.back();
        EventLog::Emit(bot, EventLog::EventType::Flight, EventLog::FLIGHT_START, log->fromNode, 0);
        return;
    }

    EventLog::Emit(bot, EventLog::EventType::Flight, EventLog::FLIGHT_END, log->toNode,
                   GetMSTimeDiffToNow(log->startedMs) / MS_PER_SECOND);
}

void AddWorldLogHooks()
{
    new WorldLogPlayerScript();
    new WorldLogWriterLootScript();
}
