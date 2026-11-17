/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UseBuffsStrategy.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "AiObjectContext.h"
#include "Bag.h"
#include "BotKind.h"
#include "BotTraitMgr.h"
#include "Group.h"
#include "Item.h"
#include "ItemBuffRules.h"
#include "ItemUse.h"
#include "ItemUseIndex.h"
#include "ItemUseKinds.h"
#include "ItemWorthRules.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace
{
constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

void Mix(std::uint64_t& hash, std::uint64_t value)
{
    hash ^= value;
    hash *= FNV_PRIME;
}

ItemUse::BotItems& ItemsFor(PlayerbotAI* botAI)
{
    if (!botAI->botItems)
        botAI->botItems = std::make_unique<ItemUse::BotItems>();
    return *botAI->botItems;
}

// What is ahead (WI8a Q5), O(1): the console's test value, else the live signals: a battleground the bot is in or
// queued for, an instance (a dungeon or a raid) the bot is in, a group with another member. An elite has no live
// signal (no index knows a quest objective's rank): only the console sets it
ItemUse::Ahead AheadOf(PlayerbotAI* botAI, Player* bot)
{
    ItemUse::BotItems const* items = botAI->botItems.get();
    if (items && items->aheadTest < static_cast<std::uint8_t>(ItemUse::Ahead::Count))
        return static_cast<ItemUse::Ahead>(items->aheadTest);
    if (bot->InBattleground() || bot->InBattlegroundQueue())
        return ItemUse::Ahead::Battleground;
    if (Map const* map = bot->GetMap(); map && map->IsDungeon())
        return ItemUse::Ahead::Dungeon;
    if (Group const* group = bot->GetGroup(); group && group->GetMembersCount() > 1)
        return ItemUse::Ahead::Group;
    return ItemUse::Ahead::None;
}

// The role as the stat weights' calculator reads it (StatsWeightCalculator's constructor)
ItemUse::Role RoleOf(Player* bot)
{
    if (PlayerbotAI::IsHeal(bot))
        return ItemUse::Role::Healer;
    if (PlayerbotAI::IsCaster(bot))
        return ItemUse::Role::Caster;
    if (PlayerbotAI::IsTank(bot))
        return ItemUse::Role::Tank;
    if (PlayerbotAI::IsMelee(bot))
        return ItemUse::Role::Melee;
    return ItemUse::Role::Ranged;
}

// The role's fixed weights; for melee and ranged damage the strength and agility weights against attack power come
// from the bot's spec (StatsWeightCalculator), which the fixed table cannot know (a warrior's strength, a rogue's
// agility). The calculator makes its weights only while it values an item: any held buff serves
ItemUse::BuffWeights WeightsOf(Player* bot, ItemUse::Role role, std::uint32_t anyEntry)
{
    std::span<float const> const fixed = ItemUse::RoleWeights(role);
    ItemUse::BuffWeights weights{};
    std::copy_n(fixed.begin(), std::min(fixed.size(), weights.size()), weights.begin());
    if ((role != ItemUse::Role::Melee && role != ItemUse::Role::Ranged) || !anyEntry)
        return weights;

    StatsWeightCalculator calculator(bot);
    calculator.CalculateItem(anyEntry);
    float const* stats = calculator.GetStatWeights();
    float const attackPower = stats[STATS_TYPE_ATTACK_POWER];
    if (!(attackPower > 0.0f))
        return weights;
    weights[static_cast<std::size_t>(ItemUse::BuffStat::Strength)] =
        std::max(stats[STATS_TYPE_STRENGTH], 0.0f) / attackPower;
    weights[static_cast<std::size_t>(ItemUse::BuffStat::Agility)] =
        std::max(stats[STATS_TYPE_AGILITY], 0.0f) / attackPower;
    return weights;
}

// WI.10: the control cohort (a persistent bot by its guid and the share) or the test's nospend
bool InControl(PlayerbotAI* botAI, Player* bot)
{
    if (!TestConstraints::SpendsOnItsOwn(botAI->testConstraints))
        return true;
    return BotKind::IsPersistentBot(bot) &&
           ItemUse::InControlCohort(bot->GetGUID().GetCounter(), sPlayerbotAIConfig.ItemUse.ControlShare);
}

// What the bot holds of the buff slots' items and the long auras on it, read in one pass each
struct Holdings
{
    std::vector<ItemUse::UsableEntry> list;
    std::vector<ItemUse::Record> records;
    std::vector<std::uint32_t> auraGroups;
    std::vector<std::int32_t> auraAmounts;
};

void AddItem(Player* bot, Item* item, ItemUseIndex const& index, Holdings& held)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE)
        return;

    for (ItemUse::Record const& record : index.OfItem(proto->ItemId))
    {
        if (!ItemUse::IsBuffSlot(record.exclude))
            continue;

        auto const same = std::find_if(held.list.begin(), held.list.end(), [&](ItemUse::UsableEntry const& entry)
                                       { return entry.record == index.IdOf(record); });
        if (same != held.list.end())
        {
            same->count += item->GetCount();
            continue;
        }

        ItemUse::Classified const classified = ItemUse::Classify(record);
        ItemUse::UsableEntry entry;
        entry.record = index.IdOf(record);
        entry.item = ItemUse::ItemRef{proto->ItemId, item->GetGUID().GetRawValue(), ItemUse::SLOT_BAGS};
        entry.kind = classified.kind;
        entry.sub = classified.sub;
        entry.rank = classified.rank;
        entry.count = item->GetCount();
        entry.consumed = record.consumed;
        entry.replaceCopper = record.replaceCopper;
        entry.ready = !bot->HasSpellCooldown(record.spellId);
        entry.shape = record.shape;
        held.list.push_back(entry);
        held.records.push_back(record);
    }
}

void ReadBags(Player* bot, Holdings& held)
{
    ItemUseIndex const& index = ItemUseIndex::Get();
    for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        AddItem(bot, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), index, held);
    for (std::uint8_t bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* pack = bot->GetBagByPos(bag))
            for (std::uint32_t slot = 0; slot < pack->GetBagSize(); ++slot)
                AddItem(bot, pack->GetItemByPos(static_cast<std::uint8_t>(slot)), index, held);
}

// The spell groups (with their parents: the store expands groups of groups) of each positive aura on the bot that
// lasts longer than BUFF_RENEW_MS or has no end, with the largest amount of its effects
void ReadAuras(Player* bot, Holdings& held)
{
    for (auto const& [spellId, application] : bot->GetAppliedAuras())
    {
        Aura const* aura = application ? application->GetBase() : nullptr;
        if (!aura || aura->IsPassive() || !application->IsPositive())
            continue;
        std::int32_t const left = aura->GetDuration();
        if (left >= 0 && left <= static_cast<std::int32_t>(ItemUse::BUFF_RENEW_MS))
            continue;

        std::int32_t amount = 0;
        for (std::uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* effect = aura->GetEffect(i))
                amount = std::max(amount, std::abs(effect->GetAmount()));
        SpellInfo const* info = aura->GetSpellInfo();
        SpellInfo const* first = info->GetFirstRankSpell();
        SpellSpellGroupMapBounds const bounds = sSpellMgr->GetSpellSpellGroupMapBounds(first ? first->Id : info->Id);
        for (auto group = bounds.first; group != bounds.second; ++group)
        {
            held.auraGroups.push_back(static_cast<std::uint32_t>(group->second));
            held.auraAmounts.push_back(amount);
        }
    }
}

std::uint64_t SignatureOf(Holdings const& held)
{
    std::uint64_t hash = FNV_OFFSET;
    for (ItemUse::UsableEntry const& entry : held.list)
    {
        Mix(hash, entry.record);
        Mix(hash, entry.count);
        Mix(hash, entry.ready);
    }
    for (std::size_t i = 0; i < held.auraGroups.size(); ++i)
    {
        Mix(hash, held.auraGroups[i]);
        Mix(hash, static_cast<std::uint32_t>(held.auraAmounts[i]));
    }
    return hash;
}

// The causes that may pass a moment later (a cooldown, a cast, a fight, the engine's own wait): asked again
bool Retries(ItemUse::Cause cause)
{
    return cause == ItemUse::Cause::NotReady || cause == ItemUse::Cause::Busy || cause == ItemUse::Cause::InFight ||
           cause == ItemUse::Cause::Engine;
}

// The queue's next pick: Check first (a refusal at Check writes no row), then Use (its one row)
bool UseNext(Player* bot, ItemUse::BuffState& state, std::uint32_t nowMs)
{
    if (ItemUse::Pending(bot))
        return false;

    while (!state.queue.empty())
    {
        ItemUse::BuffUse const next = state.queue.front();
        Item* item = bot->GetItemByGuid(ObjectGuid(next.itemGuid));
        if (!item)
            item = bot->GetItemByEntry(next.entry);
        if (!item || getMSTimeDiff(state.queuedMs, nowMs) > ItemUse::BUFF_QUEUE_MS)
        {
            state.queue.erase(state.queue.begin());
            continue;
        }

        ItemUse::Request const request = ItemUse::OnSelf(item, ItemUse::Caller::Buff);
        ItemUse::Cause const cause = ItemUse::Check(bot, request);
        state.triedMs = nowMs;
        if (cause == ItemUse::Cause::None)
        {
            state.queue.erase(state.queue.begin());
            ItemUse::Use(bot, request);
            return true;
        }
        if (Retries(cause))
            return false;
        state.queue.erase(state.queue.begin());
    }
    return false;
}
}  // namespace

void UseBuffsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    triggers.push_back(new TriggerNode("buff ahead", {NextAction("use buff items", ACTION_NORMAL + 2.0f)}));
}

BuffAheadTrigger::BuffAheadTrigger(PlayerbotAI* botAI)
    : Trigger(botAI, "buff ahead", static_cast<int32_t>(ItemUse::BUFF_TRIGGER_MS))
{
}

bool BuffAheadTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
        !TestConstraints::UsesItemsOnItsOwn(botAI->testConstraints))
        return false;

    ItemUse::BotItems const* items = botAI->botItems.get();
    if (!items || !items->buffs.started)
        return true;

    ItemUse::BuffState const& state = items->buffs;
    if (static_cast<std::uint8_t>(AheadOf(botAI, bot)) != state.seenAhead || items->aheadSerial != state.seenSerial)
        return true;

    std::uint32_t const now = getMSTime();
    if (!state.queue.empty())
        return !items->active && getMSTimeDiff(state.triedMs, now) >= ItemUse::BUFF_RETRY_MS;
    return getMSTimeDiff(state.checkedMs, now) >= ItemUse::BUFF_CHECK_MS;
}

bool UseBuffItemsAction::Execute(Event /*event*/)
{
    ItemUse::BotItems& items = ItemsFor(botAI);
    ItemUse::BuffState& state = items.buffs;
    std::uint32_t const now = getMSTime();
    ItemUse::Ahead const ahead = AheadOf(botAI, bot);
    bool const changed =
        !state.started || static_cast<std::uint8_t>(ahead) != state.seenAhead || items.aheadSerial != state.seenSerial;
    state.started = true;
    state.seenAhead = static_cast<std::uint8_t>(ahead);
    state.seenSerial = items.aheadSerial;
    if (!changed && !state.queue.empty())
        return UseNext(bot, state, now);

    state.checkedMs = now;
    Holdings held;
    ReadBags(bot, held);
    if (!held.list.empty())
        ReadAuras(bot, held);
    std::uint64_t const signature = SignatureOf(held);
    bool const same = signature == state.signature;
    state.signature = signature;
    if (!changed && same)
        return false;

    state.queue.clear();
    if (held.list.empty())
        return false;

    ItemUse::Role const role = RoleOf(bot);
    ItemUse::BuffWeights const weights = WeightsOf(bot, role, held.list.front().item.entry);
    std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter());
    WorldTravel::SlotMeasure const& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    WorldTravel::SlotRates const rates = WorldTravel::CurrentRates(
        measure, sPlayerbotAIConfig.WorldTravel.SlotBands[WorldTravel::LevelBand(bot->GetLevel())]);
    float const earn = std::clamp(rates.EarnCopperPerMinute, 0.0f, 4294967295.0f);

    ItemUse::BuffBot buffBot;
    buffBot.role = role;
    buffBot.level = static_cast<std::uint8_t>(std::min<std::uint32_t>(bot->GetLevel(), 255));
    buffBot.ahead = ahead;
    buffBot.auraGroups = held.auraGroups;
    buffBot.auraAmounts = held.auraAmounts;
    buffBot.statWeights = weights;
    buffBot.thrift = traits ? ItemUse::SpendThrift(*traits) : 0.0f;
    buffBot.daring = traits ? ItemUse::SpendDaring(*traits) : 0.0f;
    buffBot.earnCopperPerMinute = static_cast<std::uint32_t>(earn);
    buffBot.deathCopper = ItemUse::DeathCopper(buffBot.earnCopperPerMinute, 0);
    buffBot.control = InControl(botAI, bot);

    std::vector<ItemUse::BuffPick> const picks = ItemUse::PickBuffs(buffBot, held.list, held.records);
    // A read that found the bags or auras changed (the bot's own uses among them) writes its rows only when it uses
    // something; a change of what is ahead (or the console's order) writes them always
    if (!changed && std::none_of(picks.begin(), picks.end(), [](ItemUse::BuffPick const& pick) { return pick.use; }))
        return false;

    ItemUse::BuffUse food;
    for (ItemUse::BuffPick const& pick : picks)
    {
        ItemUse::UsableEntry const& entry = held.list[pick.usable];
        std::string const choice = ItemUse::BuffChoice(pick);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::ItemBuff, choice, ItemUse::BuffReasonWord(pick.reason),
                           pick.score, {}, entry.item.entry);
        if (!pick.use)
            continue;
        // a buff food is eaten sitting: any later use would stand the bot up, so it comes last
        ItemUse::BuffUse const use{entry.item.guid, entry.item.entry};
        if (pick.slot == ItemUse::Exclude::WellFed)
            food = use;
        else
            state.queue.push_back(use);
    }
    if (food.entry)
        state.queue.push_back(food);
    state.queuedMs = now;
    return UseNext(bot, state, now);
}
