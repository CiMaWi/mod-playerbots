/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotWatch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "Action.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestFields.h"
#include "QuestIndex.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldLogFormat.h"
#include "WorldLogStatus.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 TICK_MS = 1000;
// The watcher's body is put on the bot beyond this, so a `.wlog mark` (60 yd) always reaches the bot
constexpr float TETHER_YARDS = 40.0f;
// Decision rows kept between two ticks; more than a bot writes in a second
constexpr size_t THOUGHT_CAP = 200;

struct Thought
{
    WorldLogFormat::DecisionKind kind;
    uint32 ref;
    std::string text;
};

// The watched bot's guid counter, 0 for none: read by NoteDecision and the hooks on any thread
std::atomic<uint32> watchedKey{0};
// World thread only
ObjectGuid watchedGuid;
std::string watchedName;
uint32 tickTimer = 0;
// Decision lines sent to watchers since the server's start
uint32 linesSent = 0;

std::mutex thoughtLock;
std::vector<Thought> thoughts;

// The slots mirrored outside the bags' contents: equipment and the four bags, the backpack, keys and currencies
struct SlotRange
{
    uint8 from;
    uint8 to;
};
constexpr std::array<SlotRange, 3> MIRRORED_SLOTS = {{{EQUIPMENT_SLOT_START, INVENTORY_SLOT_BAG_END},
                                                      {INVENTORY_SLOT_ITEM_START, INVENTORY_SLOT_ITEM_END},
                                                      {KEYRING_SLOT_START, CURRENCYTOKEN_SLOT_END}}};

// What makes two items the same in a client's window. An enchantment's duration is left out: it runs down
bool SameItem(Item const* mine, Item const* theirs)
{
    if (!mine || !theirs)
        return mine == theirs;

    if (mine->GetEntry() != theirs->GetEntry())
        return false;

    for (uint16 field :
         {uint16(ITEM_FIELD_STACK_COUNT), uint16(ITEM_FIELD_PROPERTY_SEED), uint16(ITEM_FIELD_RANDOM_PROPERTIES_ID),
          uint16(ITEM_FIELD_DURABILITY), uint16(ITEM_FIELD_MAXDURABILITY)})
        if (mine->GetUInt32Value(field) != theirs->GetUInt32Value(field))
            return false;

    for (uint16 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (mine->GetUInt32Value(ITEM_FIELD_SPELL_CHARGES + i) != theirs->GetUInt32Value(ITEM_FIELD_SPELL_CHARGES + i))
            return false;

    for (uint32 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
        if (mine->GetEnchantmentId(EnchantmentSlot(slot)) != theirs->GetEnchantmentId(EnchantmentSlot(slot)))
            return false;

    return true;
}

Item* CopyOf(Item const* source, Player* watcher)
{
    Item* copy = Item::CreateItem(source->GetEntry(), source->GetCount(), watcher);
    if (!copy)
        return nullptr;

    // Charges, flags, every enchantment slot, the random property and the durability: one run of fields
    for (uint16 field = ITEM_FIELD_SPELL_CHARGES; field <= ITEM_FIELD_MAXDURABILITY; ++field)
        copy->SetUInt32Value(field, source->GetUInt32Value(field));
    return copy;
}

// One slot of the watcher made the bot's. A bag that differs goes with what it holds
void MirrorSlot(Player* watcher, uint8 bag, uint8 slot, Item const* theirs)
{
    Item* mine = watcher->GetItemByPos(bag, slot);
    if (SameItem(mine, theirs))
        return;

    if (mine)
    {
        if (Bag* held = mine->ToBag())
            for (uint32 i = 0; i < held->GetBagSize(); ++i)
                if (held->GetItemByPos(uint8(i)))
                    watcher->DestroyItem(slot, uint8(i), true);
        watcher->DestroyItem(bag, slot, true);
    }

    if (!theirs)
        return;

    Item* copy = CopyOf(theirs, watcher);
    if (!copy)
        return;

    uint16 const pos = uint16((uint16(bag) << 8) | slot);
    if (bag == INVENTORY_SLOT_BAG_0 && slot < INVENTORY_SLOT_BAG_END)
        watcher->EquipItem(pos, copy, true);
    else
    {
        ItemPosCountVec dest;
        dest.emplace_back(pos, copy->GetCount());
        watcher->StoreItem(dest, copy, true);
    }
}

void MirrorItems(Player* watcher, Player* bot)
{
    for (SlotRange const& range : MIRRORED_SLOTS)
        for (uint8 slot = range.from; slot < range.to; ++slot)
            MirrorSlot(watcher, INVENTORY_SLOT_BAG_0, slot, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* theirs = bot->GetBagByPos(bagSlot);
        if (!theirs || !watcher->GetBagByPos(bagSlot))
            continue;

        for (uint32 i = 0; i < theirs->GetBagSize(); ++i)
            MirrorSlot(watcher, bagSlot, uint8(i), theirs->GetItemByPos(uint8(i)));
    }
}

// The client's 25 slots get the bot's held quests, of more than 25 those the planner's worth ranks highest (neutral
// weights), in the bot's own order. The status map and the rewarded quests follow, so the quest givers' marks and
// menus answer the watcher nearly as they answer the bot
void MirrorQuests(Player* watcher, Player* bot)
{
    struct Held
    {
        uint16 slot;
        uint32 quest;
        float worth;
    };
    std::vector<Held> held;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    for (uint16 slot = 0; slot < bot->GetQuestLogSize(); ++slot)
    {
        uint32 const quest = bot->GetQuestSlotQuestId(slot);
        if (!quest)
            continue;

        Planner::QuestRecord const* record = index ? index->Find(quest) : nullptr;
        held.push_back({slot, quest, record ? Planner::QuestFields::QuestWorth(*record, {}) : 0.0f});
    }

    if (held.size() > MAX_QUEST_LOG_SIZE)
    {
        std::stable_sort(held.begin(), held.end(), [](Held const& a, Held const& b) { return a.worth > b.worth; });
        held.resize(MAX_QUEST_LOG_SIZE);
        std::sort(held.begin(), held.end(), [](Held const& a, Held const& b) { return a.slot < b.slot; });
    }

    bool changed = false;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        bool const filled = slot < held.size();
        uint16 const from = filled ? held[slot].slot : 0;
        uint32 const quest = filled ? held[slot].quest : 0;
        uint32 const state = filled ? bot->GetQuestSlotState(from) : 0;
        std::array<uint16, QUEST_OBJECTIVES_COUNT> counts{};
        bool same = watcher->GetQuestSlotQuestId(slot) == quest && watcher->GetQuestSlotState(slot) == state;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            counts[i] = filled ? bot->GetQuestSlotCounter(from, i) : 0;
            same = same && watcher->GetQuestSlotCounter(slot, i) == counts[i];
        }
        if (same)
            continue;

        watcher->SetQuestSlot(slot, quest, filled ? bot->GetQuestSlotTime(from) : 0);
        if (state)
            watcher->SetQuestSlotState(slot, state);
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (counts[i])
                watcher->SetQuestSlotCounter(slot, i, counts[i]);
        changed = true;
    }

    RewardedQuestSet const& rewarded = bot->getRewardedQuests();
    if (!changed && rewarded.size() == watcher->getRewardedQuests().size())
        return;

    watcher->getQuestStatusMap() = bot->getQuestStatusMap();
    for (uint32 quest : rewarded)
        if (!watcher->getRewardedQuests().count(quest))
            watcher->SetRewardedQuest(quest);
}

void Mirror(Player* watcher, Player* bot)
{
    if (watcher->GetLevel() != bot->GetLevel())
        watcher->GiveLevel(bot->GetLevel());
    watcher->SetUInt32Value(PLAYER_XP, bot->GetUInt32Value(PLAYER_XP));
    watcher->SetMoney(bot->GetMoney());
    MirrorItems(watcher, bot);
    MirrorQuests(watcher, bot);
}

// A game master no one sees or attacks, flying; set again whenever a login dropped it
void Prepare(Player* watcher)
{
    if (watcher->IsGameMaster() && !watcher->isGMVisible())
        return;

    watcher->SetGameMaster(true);
    watcher->SetGMVisible(false);
    // The two calls above take the level from the account's; a plain account watches the same way
    watcher->SetServerSideVisibility(SERVERSIDE_VISIBILITY_GM, SEC_GAMEMASTER);
    watcher->SetServerSideVisibilityDetect(SERVERSIDE_VISIBILITY_GM, SEC_GAMEMASTER);
    watcher->SetFaction(FACTION_FRIENDLY);
    watcher->SetCanFly(true);
}

void Unview(Player* watcher, Player* bot)
{
    if (watcher->GetGuidValue(PLAYER_FARSIGHT) == bot->GetGUID())
        watcher->SetViewpoint(bot, false);
}

// The camera on the bot, the body within TETHER_YARDS of it. A viewpoint needs both on one map: the hooks below end it
// before the bot leaves the map, and it ends here before the watcher is sent after the bot
void Follow(Player* watcher, Player* bot)
{
    Map* map = bot->GetMap();
    if (map != watcher->GetMap())
    {
        Unview(watcher, bot);
        // An instance's copy is picked by the entering player: no following into one
        if (!map->Instanceable())
            watcher->TeleportTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                bot->GetOrientation());
        return;
    }

    if (!watcher->IsWithinDist(bot, TETHER_YARDS))
    {
        watcher->TeleportTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                            bot->GetOrientation());
        return;
    }

    if (watcher->GetGuidValue(PLAYER_FARSIGHT) != bot->GetGUID())
        watcher->SetViewpoint(bot, true);
}

// The bot's shared vision emptied: only watchers are in it, and each is on the bot's map (Follow)
void EndViews(Player* bot)
{
    while (bot->HasSharedVision())
    {
        Player* watcher = bot->GetSharedVisionList().front();
        watcher->SetViewpoint(bot, false);
        bot->RemovePlayerFromVision(watcher);
    }
}

std::vector<std::string> TakeThoughts()
{
    std::vector<Thought> taken;
    {
        std::lock_guard<std::mutex> guard(thoughtLock);
        taken.swap(thoughts);
    }

    std::vector<std::string> lines;
    lines.reserve(taken.size());
    for (Thought& thought : taken)
    {
        // The two quest executor kinds carry their quest as the row's ref
        bool const questRef = thought.kind == WorldLogFormat::DecisionKind::QuestExec ||
                              thought.kind == WorldLogFormat::DecisionKind::QuestUse;
        Quest const* quest = questRef && thought.ref ? sObjectMgr->GetQuestTemplate(thought.ref) : nullptr;
        if (quest)
            thought.text += Acore::StringFormat(" [{}]", quest->GetTitle());
        else if (thought.ref)
            thought.text += Acore::StringFormat(" ref {}", thought.ref);
        lines.push_back(std::move(thought.text));
    }
    return lines;
}

class BotWatchPlayerScript : public PlayerScript
{
public:
    BotWatchPlayerScript() : PlayerScript("BotWatchPlayerScript", {PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_BEFORE_TELEPORT})
    {
    }

    // On the bot's own thread, before it leaves its map: no watcher keeps a seer on another map
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float /*x*/, float /*y*/, float /*z*/,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        uint32 const key = watchedKey.load(std::memory_order_relaxed);
        if (key && player->GetGUID().GetCounter() == key && player->GetMapId() != mapId)
            EndViews(player);
        return true;
    }

    // The bot's object goes: no watcher keeps it as its seer. A watcher that goes gives its viewpoint back
    void OnPlayerLogout(Player* player) override
    {
        uint32 const key = watchedKey.load(std::memory_order_relaxed);
        if (!key)
            return;

        if (player->GetGUID().GetCounter() == key)
            EndViews(player);
        else if (Player* bot = ObjectAccessor::FindConnectedPlayer(player->GetGuidValue(PLAYER_FARSIGHT)))
            player->SetViewpoint(bot, false);
    }
};

// A watcher's state for the command: what a check without a client reads
std::string StateOf(Player* watcher, Player* bot)
{
    uint32 items = 0;
    uint32 differing = 0;
    auto const count = [&items, &differing](Item const* mine, Item const* theirs)
    {
        items += theirs ? 1 : 0;
        differing += SameItem(mine, theirs) ? 0 : 1;
    };
    for (SlotRange const& range : MIRRORED_SLOTS)
        for (uint8 slot = range.from; slot < range.to; ++slot)
            count(watcher->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* theirs = bot->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < theirs->GetBagSize(); ++i)
                count(watcher->GetItemByPos(bagSlot, uint8(i)), theirs->GetItemByPos(uint8(i)));

    uint32 shown = 0;
    uint32 held = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        shown += watcher->GetQuestSlotQuestId(slot) ? 1 : 0;
    for (uint16 slot = 0; slot < bot->GetQuestLogSize(); ++slot)
        held += bot->GetQuestSlotQuestId(slot) ? 1 : 0;

    bool const sameMap = watcher->GetMap() == bot->GetMap();
    return Acore::StringFormat(
        " (level {} of {}, view {}, {}, items {} differing {}, quests {} of {}, money {} of {}, lines {})",
        watcher->GetLevel(), bot->GetLevel(), watcher->GetGuidValue(PLAYER_FARSIGHT) == bot->GetGUID() ? "yes" : "no",
        sameMap ? Acore::StringFormat("{:.0f} yd", watcher->GetDistance(bot)) : std::string("another map"), items,
        differing, shown, held, watcher->GetMoney(), bot->GetMoney(), linesSent);
}

// The idle line: a watched bot that stands (alive, out of combat, no move, no cast, no flight) for IDLE_FIRST_S says
// what its AI waits for, again every IDLE_AGAIN_S, and how long it stood when it goes on. To the watchers' chat and,
// as a "[Watch]" line, to Playerbots.log
constexpr uint32 IDLE_FIRST_S = 2;
constexpr uint32 IDLE_AGAIN_S = 3;
uint32 idleSinceMs = 0;
uint32 idleNextS = 0;

void IdleLine(Player* bot, std::vector<std::string>& lines)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    uint32 const nowMs = getMSTime();
    bool const idle = botAI && bot->IsAlive() && !bot->IsInCombat() && !bot->isMoving() &&
                      !bot->IsNonMeleeSpellCast(false) && !bot->IsInFlight();
    std::string line;
    if (!idle)
    {
        if (idleSinceMs && idleNextS > IDLE_FIRST_S)
            line = Acore::StringFormat("~ went on after {} s", getMSTimeDiff(idleSinceMs, nowMs) / IN_MILLISECONDS);
        idleSinceMs = 0;
    }
    else
    {
        if (!idleSinceMs)
        {
            idleSinceMs = nowMs ? nowMs : 1;
            idleNextS = IDLE_FIRST_S;
        }
        uint32 const seconds = getMSTimeDiff(idleSinceMs, nowMs) / IN_MILLISECONDS;
        if (seconds >= idleNextS)
        {
            idleNextS = seconds + IDLE_AGAIN_S;
            Action* const last = botAI->LastRan();
            BotState const state = botAI->GetState();
            line = Acore::StringFormat(
                "~ idle {} s: {} engine, status {}, next check in {:.1f} s ({:.1f} s asked), last check by {}{}, last "
                "action {} {:.1f} s ago{}",
                seconds, state == BOT_STATE_COMBAT ? "combat" : (state == BOT_STATE_DEAD ? "dead" : "non-combat"),
                WorldLog::NewRpgStatusWord(static_cast<int32>(botAI->rpgInfo.GetStatus())),
                botAI->CheckDelayMs() / 1000.0f, botAI->HoldDelayMs() / 1000.0f,
                BotEvents::CauseName(botAI->LastCause()),
                botAI->LastCause() == BotEvents::Cause::Event ? " (" + BotEvents::Names(botAI->LastWokenBy()) + ")"
                                                              : std::string(),
                last ? last->getName() : std::string("none"),
                last ? getMSTimeDiff(botAI->LastRanMs(), nowMs) / 1000.0f : 0.0f,
                botAI->planner.pending ? ", plan asked" : "");
        }
    }
    if (line.empty())
        return;
    LOG_INFO("playerbots", "[Watch] {} {}", bot->GetName(), line);
    lines.push_back(std::move(line));
}

// "playerbots watch fresh <bot>": the online bot as its race and class are created (Player::Create's outfit, spells,
// skills and money), at level 1 with nothing else: every item of its equipment, bags, bank and key ring is destroyed
bool Fresh(Player* bot)
{
    PlayerInfo const* const info = sObjectMgr->GetPlayerInfo(bot->getRace(), bot->getClass());
    if (!info)
        return false;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < CURRENCYTOKEN_SLOT_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

    if (bot->GetLevel() != 1)
        bot->GiveLevel(1);
    bot->SetUInt32Value(PLAYER_XP, 0);
    bot->SetMoney(sWorld->getIntConfig(CONFIG_START_PLAYER_MONEY));
    bot->resetTalents(true);
    std::vector<uint16> skills;
    for (auto const& [skill, status] : bot->GetSkillStatusMap())
        if (status.uState != SKILL_DELETED)
            skills.push_back(static_cast<uint16>(skill));
    for (uint16 const skill : skills)
        bot->SetSkill(skill, 0, 0, 0);
    bot->resetSpells();
    bot->RemoveAllSpellCooldown();

    // The outfit, as Player::Create gives it
    if (CharStartOutfitEntry const* const outfit =
            GetCharStartOutfitEntry(bot->getRace(), bot->getClass(), bot->getGender()))
    {
        for (int32 const itemId : outfit->ItemId)
        {
            ItemTemplate const* const proto = itemId > 0 ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
            if (!proto)
                continue;
            uint32 count = proto->BuyCount;
            if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD)
            {
                if (proto->Spells[0].SpellCategory == SPELL_CATEGORY_FOOD)
                    count = 4;
                else if (proto->Spells[0].SpellCategory == SPELL_CATEGORY_DRINK)
                    count = 2;
                count = std::min(count, proto->GetMaxStackSize());
            }
            bot->StoreNewItemInBestSlots(itemId, count);
        }
    }
    for (PlayerCreateInfoItem const& item : info->item)
        bot->StoreNewItemInBestSlots(item.item_id, item.item_amount);
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* const item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint16 dest = 0;
        if (!item || bot->CanEquipItem(NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            continue;
        bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
        bot->EquipItem(dest, item, true);
    }

    bot->UpdateSkillsForLevel();
    bot->UpdateAllStats();
    bot->SetFullHealth();
    bot->SaveToDB(false, false);
    return true;
}

std::vector<Player*> Watchers()
{
    std::vector<Player*> watchers;
    std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* player = pair.second;
        if (IsRealPlayer(player) && player->IsInWorld() && !player->IsBeingTeleported() && player->GetSession())
            watchers.push_back(player);
    }
    return watchers;
}

void End()
{
    idleSinceMs = 0;
    if (Player* bot = ObjectAccessor::FindConnectedPlayer(watchedGuid))
        EndViews(bot);
    watchedKey.store(0, std::memory_order_relaxed);
    watchedGuid.Clear();
    watchedName.clear();
    std::lock_guard<std::mutex> guard(thoughtLock);
    thoughts.clear();
}
}  // namespace

void BotWatch::NoteDecision(WorldLogFormat::DecisionRow const& row)
{
    uint32 const key = watchedKey.load(std::memory_order_relaxed);
    if (!key || row.bot != key)
        return;

    Thought thought{row.kind, row.ref,
                    Acore::StringFormat("> {}: {} ({}) {:.2f}", WorldLogFormat::DecisionKindWord(row.kind), row.choice,
                                        row.reason, row.value)};
    for (size_t i = 0; i < row.alternatives.Count(); ++i)
        thought.text += Acore::StringFormat(" | {} {:.2f}", row.alternatives[i].word, row.alternatives[i].value);

    std::lock_guard<std::mutex> guard(thoughtLock);
    if (thoughts.size() < THOUGHT_CAP)
        thoughts.push_back(std::move(thought));
}

void BotWatch::Update(uint32 diff)
{
    if (!watchedKey.load(std::memory_order_relaxed))
        return;

    tickTimer += diff;
    if (tickTimer < TICK_MS)
        return;
    tickTimer = 0;

    std::vector<std::string> lines = TakeThoughts();
    Player* bot = ObjectAccessor::FindConnectedPlayer(watchedGuid);
    bool const there = bot && bot->IsInWorld() && !bot->IsBeingTeleported();
    if (there)
        IdleLine(bot, lines);
    for (Player* watcher : Watchers())
    {
        Prepare(watcher);
        if (there)
        {
            Follow(watcher, bot);
            Mirror(watcher, bot);
        }

        ChatHandler chat(watcher->GetSession());
        for (std::string const& line : lines)
            chat.SendSysMessage(line);
        linesSent += uint32(lines.size());
    }
}

bool BotWatch::HandleCommand(ChatHandler* handler, char const* args)
{
    std::string name = args ? args : "";
    std::string const second = name.find(' ') == std::string::npos ? "" : name.substr(name.find(' ') + 1);
    name = name.substr(0, name.find(' '));
    if (name == "fresh")
    {
        std::string who = second.substr(0, second.find(' '));
        Player* const bot = normalizePlayerName(who) ? ObjectAccessor::FindPlayerByName(who) : nullptr;
        if (!bot || !GET_PLAYERBOT_AI(bot))
            handler->PSendSysMessage("WATCH: fresh: no online bot named {}", who);
        else if (!Fresh(bot))
            handler->PSendSysMessage("WATCH: fresh: no creation data for {}", who);
        else
            handler->PSendSysMessage("WATCH: fresh: {} is a new race {} class {} character, level {}, {} copper", who,
                                     bot->getRace(), bot->getClass(), bot->GetLevel(), bot->GetMoney());
        return true;
    }

    if (name.empty())
    {
        if (watchedName.empty())
            handler->PSendSysMessage("WATCH: none (usage: playerbots watch <bot name>|off|fresh <bot name>)");
        else
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(watchedGuid);
            std::string watchers;
            for (Player* watcher : Watchers())
                watchers += " " + watcher->GetName() + (bot ? StateOf(watcher, bot) : std::string());
            handler->PSendSysMessage("WATCH: {} (guid {}), watchers:{}", watchedName, watchedGuid.GetCounter(),
                                     watchers.empty() ? " none" : watchers);
        }
        return true;
    }

    if (name == "off")
    {
        End();
        handler->PSendSysMessage("WATCH: off");
        return true;
    }

    ObjectGuid const guid = normalizePlayerName(name) ? sCharacterCache->GetCharacterGuidByName(name) : ObjectGuid();
    if (!guid)
    {
        handler->PSendSysMessage("WATCH: no character named {}", name);
        return true;
    }

    End();
    watchedGuid = guid;
    watchedName = name;
    tickTimer = TICK_MS;
    watchedKey.store(guid.GetCounter(), std::memory_order_relaxed);

    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
    if (bot)
        handler->PSendSysMessage("WATCH: {} (guid {}): race {} class {} level {}, {}", name, guid.GetCounter(),
                                 bot->getRace(), bot->getClass(), bot->GetLevel(),
                                 GET_PLAYERBOT_AI(bot) ? "a bot" : "no bot");
    else
        handler->PSendSysMessage("WATCH: {} (guid {}): not online yet", name, guid.GetCounter());
    return true;
}

void AddSC_bot_watch() { new BotWatchPlayerScript(); }
