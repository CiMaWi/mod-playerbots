/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <mutex>
#include <unordered_map>

#include "CensusMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Item.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldLogBotState.h"
#include "WorldLogStatus.h"

// The world log's fact rows (XpGain, Kill, QuestAccept, QuestAbandon, Equip, SkillUp, SpellLearn, TaxiNode, Hearth,
// MapChange, Resurrect) from core hooks, of bots and real players alike, and the bots' Status and Flight rows. Every
// row goes through EventLog::Emit (CensusMgr::OnEvent). Registered after PlayerbotsPlayerScript, whose GiveXP hook
// applies the XP multipliers first. Hooks run on the player's map thread (or the world thread), read only the hooked
// player and keep only guid-keyed notes between two hooks of one core call. WL7's decision rows (WorldLog::Decision,
// called from the RPG actions) go to the decision stream the same way: formatted on the calling thread, then queued.
namespace
{
constexpr uint32 SPELL_HEARTHSTONE = 8690;
constexpr uint32 MS_PER_SECOND = 1000;
// A debit at most this long before a learn paid for it (Trainer::TeachSpell debits, then teaches)
constexpr uint32 LESSON_MS = 1;

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
    if (!WorldLog::Active() || !killer || !killed)
        return;

    CreatureTemplate const* creature = killed->GetCreatureTemplate();
    EventLog::Emit(killer, "Kill", EventLog::RankWord(creature ? creature->rank : 0), killed->GetEntry(),
                   static_cast<int64>(killed->GetLevel()) - static_cast<int64>(killer->GetLevel()));
}

class WorldLogPlayerScript : public PlayerScript
{
public:
    WorldLogPlayerScript()
        : PlayerScript(
              "WorldLogPlayerScript",
              {PLAYERHOOK_ON_QUEST_COMPUTE_EXP, PLAYERHOOK_ON_GIVE_EXP, PLAYERHOOK_ON_CREATURE_KILL,
               PLAYERHOOK_ON_CREATURE_KILLED_BY_PET, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT, PLAYERHOOK_ON_QUEST_ABANDON,
               PLAYERHOOK_CAN_EQUIP_ITEM, PLAYERHOOK_CAN_UNEQUIP_ITEM, PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_UPDATE_SKILL,
               PLAYERHOOK_ON_MONEY_CHANGED, PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_AFTER_TRAIN_SPELL,
               PLAYERHOOK_ON_LEARN_TAXI_NODE, PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_BEFORE_TELEPORT,
               PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_PLAYER_RESURRECT})
    {
    }

    void OnPlayerQuestComputeXP(Player* player, Quest const* quest, uint32& /*xpValue*/) override
    {
        if (WorldLog::Active() && player && quest)
            questNote = {LowGuid(player), quest->GetQuestId()};
    }

    // The XP offered after the module's multipliers; GiveXP may still refuse it (dead, no loot recipient) or add the
    // rested bonus
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        if (!WorldLog::Active() || !player || !amount ||
            player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) ||
            player->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN))
            return;

        uint32 id = 0;
        if (xpSource == XPSOURCE_KILL && victim && victim->IsCreature())
            id = victim->GetEntry();
        else if ((xpSource == XPSOURCE_QUEST || xpSource == XPSOURCE_QUEST_DF) && questNote.player == LowGuid(player))
            id = questNote.questId;

        questNote = {};
        EventLog::Emit(player, "XpGain", EventLog::XpSourceWord(xpSource), id, amount);
    }

    // Killing blows of the player and of its pets and totems
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override { EmitKill(killer, killed); }

    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override { EmitKill(owner, killed); }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (WorldLog::Active() && player && quest)
            EventLog::Emit(player, "QuestAccept", EventLog::SOURCE_OTHER, quest->GetQuestId(),
                           player->GetQuestLevel(quest));
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override { WorldLog::NoteQuestAbandon(player, questId); }

    // Every equip check opens the note (bots run it when they judge items too); the equip that follows reads it
    [[nodiscard]] bool OnPlayerCanEquipItem(Player* player, uint8 /*slot*/, uint16& /*dest*/, Item* /*item*/,
                                            bool /*swap*/, bool notLoading) override
    {
        if (WorldLog::Active() && player && notLoading)
            equipNote = {LowGuid(player), NULL_SLOT, 0, true};

        return true;
    }

    // Runs before the item leaves an occupied equipment slot: its level is the old one
    [[nodiscard]] bool OnPlayerCanUnequipItem(Player* player, uint16 pos, bool /*swap*/) override
    {
        if (WorldLog::Active() && player && equipNote.open && equipNote.player == LowGuid(player) &&
            Player::IsEquipmentPos(pos))
        {
            equipNote.slot = static_cast<uint8>(pos & 0xFF);
            equipNote.oldLevel = ItemLevelAt(player, equipNote.slot);
        }

        return true;
    }

    // amount: the new item's level minus the old one's (an empty slot counts 0); 0 when the old one is not known
    void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 slot, bool /*update*/) override
    {
        if (!WorldLog::Active() || !player || !item || !player->IsInWorld())
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

        CensusMgr::Instance().OnEvent(player, "Equip", ScopeWord(), item->GetEntry(), change);
    }

    void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 value, uint32 /*max*/, uint32 /*step*/,
                             uint32 newValue) override
    {
        if (WorldLog::Active() && player && newValue > value)
            EventLog::Emit(player, "SkillUp", EventLog::SOURCE_OTHER, skillId, newValue);
    }

    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (WorldLog::Active() && player && amount < 0)
            debitNote = {LowGuid(player), static_cast<uint32>(-static_cast<int64>(amount)), getMSTime()};
    }

    // Character creation and login add spells without this hook; a bot's level-up spells come through it
    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        if (!WorldLog::Active() || !player || !player->IsInWorld())
            return;

        bool const paid = debitNote.player == LowGuid(player) && getMSTimeDiff(debitNote.ms, getMSTime()) <= LESSON_MS;
        uint32 const cost = paid ? debitNote.cost : 0;
        if (paid)
            debitNote.cost = 0;  // the spells the lesson teaches with it are free

        EventLog::Emit(player, "SpellLearn",
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
        if (WorldLog::Active() && player)
            EventLog::Emit(const_cast<Player*>(player), "TaxiNode", EventLog::SOURCE_OTHER, nodeId, 0);
    }

    // Every cast of every player: two compares unless it is the hearthstone
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!WorldLog::Active() || !player || !spell || spell->GetSpellInfo()->Id != SPELL_HEARTHSTONE)
            return;

        EventLog::Emit(player, "Hearth", EventLog::SOURCE_OTHER, player->m_homebindAreaId, 0);
    }

    [[nodiscard]] bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float /*x*/, float /*y*/, float /*z*/,
                                              float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        if (WorldLog::Active() && player && mapId != player->GetMapId())
        {
            std::lock_guard<std::mutex> lock(oldMapsMutex);
            oldMaps[LowGuid(player)] = player->GetMapId();
        }

        return true;
    }

    // Also runs at login (no teleport noted: no row)
    void OnPlayerMapChanged(Player* player) override
    {
        if (!WorldLog::Active() || !player)
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

        EventLog::Emit(player, "MapChange", EventLog::MapKindWord(MapKindOf(player->GetMap())), player->GetMapId(),
                       oldMap);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!WorldLog::Active() || !player)
            return;

        std::lock_guard<std::mutex> lock(oldMapsMutex);
        oldMaps.erase(LowGuid(player));
    }

    void OnPlayerResurrect(Player* player, float restorePercent, bool& applySickness) override
    {
        if (WorldLog::Active() && player)
            EventLog::Emit(player, "Resurrect", EventLog::ResurrectWord(ScopeWord(), applySickness, restorePercent), 0,
                           0);
    }
};
}  // namespace

void WorldLog::NoteStatus(Player* bot, int32_t oldStatus, int32_t newStatus)
{
    if (oldStatus == newStatus || !bot || !WorldLog::Active())
        return;

    EventLog::Emit(bot, "Status", WorldLog::CurrentStatusReason(), static_cast<uint32>(newStatus), oldStatus);
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
    CensusMgr::Instance().EnqueueWorldLog(WorldLogFormat::Stream::Decision, WorldLogFormat::FormatDecisionRow(row), 1,
                                          row.t);

    if (pmo)
        pmo->finish();
}

void WorldLog::NoteQuestAbandon(Player* bot, uint32_t questId)
{
    if (!WorldLog::Active() || !bot)
        return;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    EventLog::Emit(bot, "QuestAbandon", EventLog::SOURCE_OTHER, questId, quest ? bot->GetQuestLevel(quest) : 0);
}

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
        EventLog::Emit(bot, "Flight", EventLog::FLIGHT_START, log->fromNode, 0);
        return;
    }

    EventLog::Emit(bot, "Flight", EventLog::FLIGHT_END, log->toNode,
                   GetMSTimeDiffToNow(log->startedMs) / MS_PER_SECOND);
}

void AddWorldLogHooks() { new WorldLogPlayerScript(); }
