/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Awareness.h"
#include "BotRecordMgr.h"
#include "CombatTrace.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DeathEdgeRules.h"
#include "EventLog.h"
#include "EventWanted.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "ProfessionConfig.h"
#include "QuestDef.h"
#include "QuestFailRows.h"
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
// the world thread), read only the hooked player and keep only thread-local notes between two hooks of one core call
// (a lesson, a swap: EventLogFormat.h).
// WL7's decision rows (WorldLog::Decision, called from the RPG actions) go to the decision stream the same way:
// formatted on the calling thread, then queued.
namespace
{
constexpr uint32 SPELL_HEARTHSTONE = 8690;
constexpr uint32 MS_PER_SECOND = 1000;
constexpr int64 ONE_EVENT = 1;

thread_local char const* questDropWord = nullptr;  // WorldLog::QuestDropWord
thread_local uint32 questDropQuest = 0;            // and the quest it drops, with its done share (-1: none)
thread_local int32 questDropShare = -1;

// Questing WQ38: the share of the quest's credits done (QuestFailRows::DoneShare) from the bot's log; none when the
// quest is not in it (the core's abandon cleared it, or it never was)
std::optional<uint32> DoneShareOf(Player* bot, uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    auto const it = bot->getQuestStatusMap().find(questId);
    if (!quest || it == bot->getQuestStatusMap().end())
        return std::nullopt;
    QuestStatusData const& data = it->second;
    std::vector<Planner::QuestFailRows::Credit> credits;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i] > 0)
            credits.push_back({data.CreatureOrGOCount[i], static_cast<uint32>(quest->RequiredNpcOrGoCount[i])});
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
            credits.push_back({data.ItemCount[i], static_cast<uint32>(quest->RequiredItemCount[i])});
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        credits.push_back({data.Explored ? 1u : 0u, 1u});
    return Planner::QuestFailRows::DoneShare(credits, data.Status == QUEST_STATUS_COMPLETE);
}

uint32 ItemLevelOf(Item const* item)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    return proto ? proto->ItemLevel : 0;
}

// The equipment slot a swap fills at pos with item, replacing the one there now; none for a bag or bank position
EventLog::SwapSlot SwapSlotAt(uint16 pos, Item const* item, Item const* replaced)
{
    if (!Player::IsEquipmentPos(pos))
        return {};

    return {item, static_cast<uint8>(pos & 0xFF), ItemLevelOf(replaced)};
}

// The XP rows' filters: no row for nothing or for a player who turned XP off
bool XpRowWanted(Player const* player, uint32 amount)
{
    return EventLog::Wanted(EventLog::EventType::XpGain) && player && amount &&
           !player->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
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

// AW31: the killer the kill hooks named for the player's current life (DeathEdgeRules.h). The rows of the death read
// it at the death's edge, OnPlayerJustDied, which every death passes once (Player::KillPlayer, called by the player's
// next update after the core set the death state, so after the kill hooks of that Unit::Kill), or at a resurrect
// that came before the edge. In the player's own CustomData: the kill hooks, the edge and the resurrect run on the
// player's map thread, and the name dies with the Player object at the latest
struct LifeKiller : DataMap::Base
{
    DeathEdge::Killer killer;
};

std::string const LIFE_KILLER_KEY = "worldlog.killer";

// The first kill hook of a life names its killer; false when one is named already (the life's second Unit::Kill)
bool NameKiller(Player* killed, DeathEdge::Kind kind, uint32 id, Unit const* unit)
{
    DeathEdge::Killer named;
    named.kind = kind;
    named.id = id;
    named.entry = unit ? unit->GetEntry() : 0;
    named.unit = unit ? unit->GetGUID().GetRawValue() : 0;
    return DeathEdge::Name(killed->CustomData.GetDefault<LifeKiller>(LIFE_KILLER_KEY)->killer, named);
}

// The rows of one death: Death (the killer named in this life, or unknown), Awareness::OnDeath (the row of deaths,
// the death's stamp, DeathScene) and PvpDeath for a player's or a pet's kill. The name ends with them
void WriteDeath(Player* player, DeathEdge::Killer& life)
{
    DeathEdge::Killer const killer = DeathEdge::Take(life);
    Unit* unit = killer.unit ? ObjectAccessor::GetUnit(*player, ObjectGuid(killer.unit)) : nullptr;
    EventLog::Emit(player, EventLog::EventType::Death, DeathEdge::DeathWord(killer.kind), killer.id, ONE_EVENT);
    Awareness::OnDeath(player, unit, killer.entry);
    if (DeathEdge::IsPvp(killer.kind))
        EventLog::Emit(player, EventLog::EventType::PvpDeath, PvpPlace(player), killer.id, ONE_EVENT);
}

uint32 constexpr SPIRIT_OF_REDEMPTION_FORM = 27827;

// After a kill hook named the killer: a victim alive again and out of Spirit of Redemption's form was revived by a
// battleground that ended at this kill, before the hooks ran (DeathEdge::RowsAtKillHook), so its rows are written now
// and no name outlives that death
void WriteIfRevived(Player* killed)
{
    bool const form = killed->HasSpiritOfRedemptionAura() || killed->HasAura(SPIRIT_OF_REDEMPTION_FORM);
    if (!DeathEdge::RowsAtKillHook(killed->IsAlive(), form))
        return;
    if (LifeKiller* named = killed->CustomData.Get<LifeKiller>(LIFE_KILLER_KEY))
        WriteDeath(killed, named->killer);
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

// What a harvest gave (professions WR19c): an item looted from a node of the gather index or from a skinned corpse is a
// `profession gather yield` row, value the count, alternatives the item and the node's or the creature's entry, ref the
// spawn. The loot's source is read on the looter's thread, as the core's loot handler reads it
void WriteHarvestYield(Player* player, ObjectGuid loot, uint32 entry, uint32 count)
{
    char const* kind = nullptr;
    uint32 target = 0;
    uint32 spawn = 0;
    if (loot.IsGameObject())
    {
        GameObject* node = player->GetMap()->GetGameObject(loot);
        if (!node || !node->GetSpawnId() || !sGatherNodeMgr.Find(node->GetSpawnId()))
            return;
        kind = "node";
        target = node->GetEntry();
        spawn = node->GetSpawnId();
    }
    else if (loot.IsCreatureOrVehicle())
    {
        Creature* corpse = player->GetMap()->GetCreature(loot);
        if (!corpse || corpse->loot.loot_type != LOOT_SKINNING)
            return;
        kind = "skin";
        target = corpse->GetEntry();
        spawn = corpse->GetSpawnId();
    }
    else
        return;
    WorldLog::Decision(player, WorldLogFormat::DecisionKind::Profession,
                       Professions::ChoiceWord(Professions::Choice::Gather),
                       Professions::ReasonWord(Professions::Reason::Yield), static_cast<float>(count),
                       {{"item", static_cast<float>(entry)}, {kind, static_cast<float>(target)}}, spawn);
}

class WorldLogPlayerScript : public PlayerScript
{
public:
    WorldLogPlayerScript()
        : PlayerScript("WorldLogPlayerScript", {PLAYERHOOK_ON_GIVE_EXP,
                                                PLAYERHOOK_ON_AFTER_GIVE_QUEST_XP,
                                                PLAYERHOOK_ON_CREATURE_KILL,
                                                PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
                                                PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
                                                PLAYERHOOK_ON_QUEST_ABANDON,
                                                PLAYERHOOK_ON_BEFORE_SWAP_ITEM,
                                                PLAYERHOOK_ON_EQUIP,
                                                PLAYERHOOK_ON_UPDATE_SKILL,
                                                PLAYERHOOK_ON_MONEY_CHANGED,
                                                PLAYERHOOK_ON_LEARN_SPELL,
                                                PLAYERHOOK_ON_BEFORE_TRAIN_SPELL,
                                                PLAYERHOOK_ON_AFTER_TRAIN_SPELL,
                                                PLAYERHOOK_ON_LEARN_TAXI_NODE,
                                                PLAYERHOOK_ON_SPELL_CAST,
                                                PLAYERHOOK_ON_AFTER_WORLDPORT,
                                                PLAYERHOOK_ON_UPDATE_ZONE,
                                                PLAYERHOOK_ON_LOGOUT,
                                                PLAYERHOOK_ON_PLAYER_RESURRECT,
                                                PLAYERHOOK_ON_PLAYER_JUST_DIED,
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

    // The XP offered after the module's multipliers; GiveXP may still refuse it (dead, no loot recipient) or add the
    // rested bonus. A quest's XP is OnPlayerAfterGiveQuestXP's row.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        if (!XpRowWanted(player, amount) || xpSource == XPSOURCE_QUEST || xpSource == XPSOURCE_QUEST_DF ||
            player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            return;

        uint32 const id = xpSource == XPSOURCE_KILL && victim && victim->IsCreature() ? victim->GetEntry() : 0;
        EventLog::Emit(player, EventLog::EventType::XpGain, EventLog::XpSourceWord(xpSource), id, amount);
    }

    // Player::RewardQuest gave the quest's XP (after every OnPlayerGiveXP). Called only below the level cap, where
    // the quest gives XP, not money: the cap is not asked again, since this XP may have reached it.
    void OnPlayerAfterGiveQuestXP(Player* player, Quest const* quest, uint32 xp, uint8 xpSource) override
    {
        if (XpRowWanted(player, xp) && quest)
            EventLog::Emit(player, EventLog::EventType::XpGain, EventLog::XpSourceWord(xpSource), quest->GetQuestId(),
                           xp);
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

    // Every two-item swap, bag sorting too: notes the equipment slots it fills (either direction) and the levels of
    // the items it replaces there, for the equips that follow in the same call
    void OnPlayerBeforeSwapItem(Player* player, Item* srcItem, Item* dstItem, uint16 src, uint16 dst) override
    {
        if (EventLog::Wanted(EventLog::EventType::Equip) && player)
            EventLog::NoteSwap(SwapSlotAt(dst, srcItem, dstItem), SwapSlotAt(src, dstItem, srcItem));
    }

    // amount: the new item's level minus the level of the item it replaced (an empty slot counts 0)
    void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 slot, bool /*update*/) override
    {
        if (!EventLog::Wanted(EventLog::EventType::Equip) || !player || !item || !player->IsInWorld())
            return;

        uint32 oldLevel = 0;
        EventLog::TakeReplacedLevel(item, slot, oldLevel);
        int64 const change = static_cast<int64>(ItemLevelOf(item)) - static_cast<int64>(oldLevel);
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
    // comes from that window, not from the action scope
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
    }

    // Character creation and login add spells without this hook; a bot's level-up spells come through it. A learn
    // inside a lesson is paid (EventLog::TakeLessonCost); the bracket is opened with the log off too, so the
    // record's lesson keeps its cost
    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        if (!EventLog::Wanted(EventLog::EventType::SpellLearn) || !player || !player->IsInWorld())
            return;

        uint32 cost = 0;
        bool const paid = EventLog::TakeLessonCost(player, cost);
        EventLog::Emit(player, EventLog::EventType::SpellLearn,
                       EventLog::SpellLearnWord(ScopeWord(), paid, GetTalentSpellCost(spellId) > 0), spellId, cost);
    }

    // Trainer::TeachSpell, right before its debit: the lesson opens with its price
    void OnPlayerBeforeTrainSpell(Player* player, Creature* /*trainer*/, uint32 /*spellId*/, int32 cost) override
    {
        EventLog::OpenLesson(player, cost > 0 ? static_cast<uint32>(cost) : 0);
    }

    // The lesson is over: its price pays for no later learn
    void OnPlayerAfterTrainSpell(Player* /*player*/, Creature* /*trainer*/, uint32 /*spellId*/) override
    {
        EventLog::CloseLesson();
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

    // A far teleport arrived (HandleMoveWorldportAck, the player is on the new map). A login never passes here (no
    // row), nor gives a move to another instance of the same map one
    void OnPlayerAfterWorldport(Player* player, uint32 oldMapId) override
    {
        if (!EventLog::Wanted(EventLog::EventType::MapChange) || !player || oldMapId == player->GetMapId())
            return;

        EventLog::Emit(player, EventLog::EventType::MapChange, EventLog::MapKindWord(MapKindOf(player->GetMap())),
                       player->GetMapId(), oldMapId);
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
        if (!player)
            return;
        // AW31: a death revived before its edge writes its rows here, so a Resurrect never follows one without its
        // Death; the life's killer ends here either way
        LifeKiller* named = player->CustomData.Get<LifeKiller>(LIFE_KILLER_KEY);
        if (named && DeathEdge::RowsAtResurrect(named->killer))
            WriteDeath(player, named->killer);
        char const* how = EventLog::ResurrectWord(ScopeWord(), applySickness, restorePercent);
        if (EventLog::Wanted(EventLog::EventType::Resurrect))
            EventLog::Emit(player, EventLog::EventType::Resurrect, how, 0, 0);
        // awareness AW5: the rise's time and place, which the bot's FightScene and DeathScene rows read; AW8: the Rise
        // row and the end of the return's memo
        Awareness::OnRise(player, how);
    }

    // AW31: the death's edge, once per death (Player::KillPlayer; a priest's Spirit of Redemption reaches it at the
    // form's end): the rows of the death with the killer the kill hooks named in this life, or unknown
    void OnPlayerJustDied(Player* player) override
    {
        if (!player)
            return;

        DeathEdge::Killer unnamed;
        LifeKiller* named = player->CustomData.Get<LifeKiller>(LIFE_KILLER_KEY);
        WriteDeath(player, named ? named->killer : unnamed);
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

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override
    {
        uint32 const entry = EventLog::TakeStoredEntry(item);
        if (!entry)
            return;
        EventLog::Emit(player, EventLog::EventType::ItemIn, "loot", entry, count);
        if (WorldLog::Active())
            WriteHarvestYield(player, lootguid, entry, count);
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
    // no script registered before this one destroys the taken item in this hook (none does). The take of an auction
    // house mail runs under the source auction (PlayerbotMgr's QueuedPacketSource): a won or returned item (WA4b)
    void OnPlayerAfterTakeItemFromMail(Player* player, Item* item, uint32 count) override
    {
        if (!item || !player || !EventLog::Wanted(EventLog::EventType::ItemIn))
            return;
        bool const auction = EventLog::SourceWordOf(EventLog::CurrentSource()) == "auction";
        EventLog::Emit(player, EventLog::EventType::ItemIn, auction ? "auction" : "mail", item->GetEntry(), count);
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

    // The kill hooks (Unit::Kill, after it set the death state) name the victim's killer for its life and write the
    // killer's side once per life: a second Unit::Kill of that life (Spirit of Redemption) adds nothing. The rows of
    // the death come at its edge (OnPlayerJustDied; here when a battleground's end revived the victim first: see
    // WriteIfRevived). Environmental damage (falling, lava, drowning) is dealt by the
    // player to itself
    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        bool const self = killer == killed;
        if (!NameKiller(killed, DeathEdge::KindOf(true, self, false), self ? 0 : killer->GetGUID().GetCounter(),
                        self ? nullptr : killer))
            return;

        WriteIfRevived(killed);
        Planner::QuestStatDeath(killed);
        NoteTravelDeath(killed);
        if (!self)
            EventLog::Emit(killer, EventLog::EventType::PvpKill, PvpPlace(killer), killed->GetGUID().GetCounter(),
                           ONE_EVENT);
    }

    // A player's pet, totem or guardian is a creature: its kill is a PvP death with the owner as the killer
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        ObjectGuid const owner = killer->GetOwnerGUID();
        bool const pet = owner.IsPlayer();
        if (!NameKiller(killed, DeathEdge::KindOf(false, false, pet), pet ? owner.GetCounter() : killer->GetEntry(),
                        killer))
            return;

        WriteIfRevived(killed);
        Planner::QuestStatDeath(killed);
        NoteTravelDeath(killed);
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

    WorldLog::Decision(bot->GetGUID().GetCounter(), kind, choice, reason, value, alternatives, ref);
}

void WorldLog::Decision(uint32 botKey, WorldLogFormat::DecisionKind kind, std::string_view choice,
                        std::string_view reason, float value, WorldLogFormat::Alternatives const& alternatives,
                        uint32 ref)
{
    if (!botKey || !WorldLog::Active())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog decision");

    WorldLogFormat::DecisionRow row;
    row.t = WorldLog::NowMs();
    row.bot = botKey;
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
    // WQ38: the amount carries the done share: the one taken before the bot's abandon packet (QuestDropWord), else
    // the log's while the quest is still in it (DropQuestAction); a client's abandon has none
    std::optional<uint32> const share = questDropQuest == questId && questDropShare >= 0
                                            ? std::optional<uint32>(static_cast<uint32>(questDropShare))
                                            : DoneShareOf(bot, questId);
    int32 const level = quest ? bot->GetQuestLevel(quest) : 0;
    EventLog::Emit(bot, EventLog::EventType::QuestAbandon, reason ? reason : EventLog::SOURCE_OTHER, questId,
                   Planner::QuestFailRows::AbandonAmount(static_cast<uint32>(std::max(level, 0)), share));
}

WorldLog::QuestDropWord::QuestDropWord(char const* reason, Player* bot, uint32_t questId)
    : _previous(questDropWord), _previousQuest(questDropQuest), _previousShare(questDropShare)
{
    questDropWord = reason;
    std::optional<uint32> const share = bot && questId ? DoneShareOf(bot, questId) : std::nullopt;
    questDropQuest = share ? questId : 0;
    questDropShare = share ? static_cast<int32>(*share) : -1;
}

WorldLog::QuestDropWord::~QuestDropWord()
{
    questDropWord = _previous;
    questDropQuest = _previousQuest;
    questDropShare = _previousShare;
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
