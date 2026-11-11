/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the upkeep index (upkeep plan WU2): the world-thread gather of the core's trainers, vendor lists
// and class reagents, the worker build, the published index, its status lines and the snapshot's upkeep fields

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AiObjectContext.h"
#include "Bag.h"
#include "Chat.h"
#include "DBCStores.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Pet.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Trainer.h"
#include "Upkeep.h"
#include "UpkeepIndex.h"
#include "UpkeepNeedRules.h"

namespace Planner
{
static_assert(UpkeepItem::CLASS_CONSUMABLE == ITEM_CLASS_CONSUMABLE &&
                  UpkeepItem::CLASS_PROJECTILE == ITEM_CLASS_PROJECTILE && UpkeepItem::CLASS_MISC == ITEM_CLASS_MISC &&
                  UpkeepItem::SUBCLASS_CONSUMABLE == ITEM_SUBCLASS_CONSUMABLE &&
                  UpkeepItem::SUBCLASS_FOOD == ITEM_SUBCLASS_FOOD &&
                  UpkeepItem::SUBCLASS_CONSUMABLE_OTHER == ITEM_SUBCLASS_CONSUMABLE_OTHER &&
                  UpkeepItem::SUBCLASS_ARROW == ITEM_SUBCLASS_ARROW &&
                  UpkeepItem::SUBCLASS_BULLET == ITEM_SUBCLASS_BULLET &&
                  UpkeepItem::SUBCLASS_MOUNT == ITEM_SUBCLASS_JUNK_MOUNT &&
                  UpkeepItem::SUBCLASS_WEAPON_BOW == ITEM_SUBCLASS_WEAPON_BOW &&
                  UpkeepItem::SUBCLASS_WEAPON_GUN == ITEM_SUBCLASS_WEAPON_GUN &&
                  UpkeepItem::SUBCLASS_WEAPON_CROSSBOW == ITEM_SUBCLASS_WEAPON_CROSSBOW,
              "the upkeep index's item classes are the core's");
static_assert(UpkeepItem::CLASS_MASK_PLAYABLE == CLASSMASK_ALL_PLAYABLE &&
                  UpkeepItem::CLASS_MASK_ROGUE == 1u << (CLASS_ROGUE - 1) && UPKEEP_CLASS_SLOTS == MAX_CLASSES &&
                  UPKEEP_RACE_SLOTS == RACE_DRAENEI + 1,
              "the upkeep index's class masks and race ids are the core's");
static_assert(static_cast<uint32>(TrainerKind::Class) == static_cast<uint32>(Trainer::Type::Class) &&
                  static_cast<uint32>(TrainerKind::Mount) == static_cast<uint32>(Trainer::Type::Mount) &&
                  static_cast<uint32>(TrainerKind::Tradeskill) == static_cast<uint32>(Trainer::Type::Tradeskill) &&
                  static_cast<uint32>(TrainerKind::Pet) == static_cast<uint32>(Trainer::Type::Pet),
              "the upkeep index's trainer kinds are the core's Trainer::Type");
static_assert(std::tuple_size_v<decltype(TrainerSpell::reqAbility)> ==
                  std::tuple_size_v<decltype(Trainer::Spell::ReqAbility)>,
              "one required ability per core slot");
static_assert(std::tuple_size_v<decltype(TrainerSpell::teaches)> == MAX_SPELL_EFFECTS, "one taught spell per effect");

namespace
{
enum class UpkeepIndexState : uint8
{
    NotStarted,
    Building,
    Ready,
    Failed
};

constexpr double BYTES_PER_MB = 1024.0 * 1024.0;
constexpr uint32 PUBLISHED_VERSION = 1;  // built once; a later rebuild would publish the next version
constexpr std::array<char const*, UPKEEP_CLASS_SLOTS> CLASS_NAMES = {
    "", "warrior", "paladin", "hunter", "rogue", "priest", "death knight", "shaman", "mage", "warlock", "", "druid"};

std::atomic<UpkeepIndexState> upkeepState{UpkeepIndexState::NotStarted};
SharedSlot<UpkeepIndex> upkeepBook;

uint32 MsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// Ammo damage as ItemUsageValue::QueryItemUsageForAmmo compares it (the same expression and truncation)
uint32 AmmoDamage(ItemTemplate const* proto)
{
    return static_cast<uint32>((proto->Damage[0].DamageMin + proto->Damage[0].DamageMax) * 1000 / 2);
}

UpkeepItemFacts FactsOf(ItemTemplate const* proto)
{
    UpkeepItemFacts facts;
    facts.item = proto->ItemId;
    facts.itemClass = proto->Class;
    facts.subClass = proto->SubClass;
    facts.spellCategory = proto->Spells[0].SpellCategory;
    facts.foodType = proto->FoodType;
    facts.allowableClass = proto->AllowableClass;
    facts.allowableRace = proto->AllowableRace;
    facts.buyPrice = static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0));
    facts.buyCount = proto->BuyCount;
    facts.requiredLevel = proto->RequiredLevel;
    facts.itemLevel = proto->ItemLevel;
    facts.requiredSkill = proto->RequiredSkill;
    facts.requiredSkillRank = proto->RequiredSkillRank;
    facts.requiredFaction = proto->RequiredReputationFaction;
    facts.requiredFactionRank = proto->RequiredReputationRank;
    facts.ammoDamage = proto->Class == ITEM_CLASS_PROJECTILE ? AmmoDamage(proto) : 0;
    return facts;
}

// The spell's reagents, and those of the spell it teaches (a trainer spell can be a learn spell)
void AddReagents(uint32 spellId, uint8 cls, std::vector<ReagentUse>& reagents)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return;
    for (int32 const reagent : info->Reagent)
        if (reagent > 0)
            reagents.push_back({static_cast<uint32>(reagent), cls, spellId});
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (effect.Effect != SPELL_EFFECT_LEARN_SPELL || !effect.TriggerSpell)
            continue;
        if (SpellInfo const* learned = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
            for (int32 const reagent : learned->Reagent)
                if (reagent > 0)
                    reagents.push_back({static_cast<uint32>(reagent), cls, spellId});
    }
}

// The races that may learn the spell as this class: Player::IsSpellFitByClassAndRace (Player.cpp) for a player whose
// race and original race are that race
uint32 FitRaces(uint32 spellId, uint8 cls)
{
    SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    uint32 const classMask = 1u << (cls - 1);
    uint32 races = 0;
    for (uint8 race = 1; race < UPKEEP_RACE_SLOTS; ++race)
    {
        uint32 const raceMask = 1u << (race - 1);
        bool fits = bounds.first == bounds.second;
        for (auto ability = bounds.first; !fits && ability != bounds.second; ++ability)
        {
            SkillLineAbilityEntry const* entry = ability->second;
            fits = (!entry->RaceMask || (entry->RaceMask & raceMask)) &&
                   (!entry->ClassMask || (entry->ClassMask & classMask)) &&
                   GetSkillRaceClassInfo(entry->SkillLine, race, cls) != nullptr;
        }
        if (fits)
            races |= raceMask;
    }
    return races;
}

// World thread, at startup: plain copies only, so the worker never reads the core's stores (`.reload` changes them)
UpkeepGather GatherUpkeep()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherUpkeep");
    UpkeepGather input;
    CreatureTemplateContainer const* templates = sObjectMgr->GetCreatureTemplates();
    std::vector<std::pair<uint32, CreatureTemplate const*>> creatures;
    creatures.reserve(templates->size());
    for (auto const& [entry, creature] : *templates)
        creatures.emplace_back(entry, &creature);
    std::sort(creatures.begin(), creatures.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });  // the trainer keys are deterministic

    std::unordered_map<Trainer::Trainer const*, uint32> trainerKeys;  // the core keeps one object per trainer id
    std::unordered_set<uint32> itemsSeen;
    for (auto const& [entry, creature] : creatures)
    {
        if (Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry))
        {
            auto const [known, added] = trainerKeys.try_emplace(trainer, entry);
            if (added)
            {
                TrainerFacts facts;
                facts.key = entry;
                facts.kind = static_cast<TrainerKind>(trainer->GetTrainerType());
                facts.requirement = trainer->GetTrainerRequirement();
                facts.spells.reserve(trainer->GetSpells().size());
                bool const classTrainer =
                    facts.kind == TrainerKind::Class && facts.requirement && facts.requirement < UPKEEP_CLASS_SLOTS;
                for (Trainer::Spell const& spell : trainer->GetSpells())
                {
                    TrainerSpell copy;
                    copy.spellId = spell.SpellId;
                    copy.moneyCost = spell.MoneyCost;
                    copy.reqSkillLine = spell.ReqSkillLine;
                    copy.reqSkillRank = spell.ReqSkillRank;
                    std::copy(spell.ReqAbility.begin(), spell.ReqAbility.end(), copy.reqAbility.begin());
                    copy.reqLevel = spell.ReqLevel;
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
                    {
                        std::size_t taught = 0;
                        for (SpellEffectInfo const& effect : info->GetEffects())
                            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                                copy.teaches[taught++] = effect.TriggerSpell;
                    }
                    facts.spells.push_back(copy);
                    if (classTrainer)
                        AddReagents(spell.SpellId, static_cast<uint8>(facts.requirement), input.reagents);
                    // Class trainers teach their class; riding trainers any class
                    if (classTrainer || facts.kind == TrainerKind::Mount)
                    {
                        SpellFit fit{facts.kind, facts.requirement, spell.SpellId, {}};
                        for (uint8 cls = 1; cls < UPKEEP_CLASS_SLOTS; ++cls)
                            if (facts.kind == TrainerKind::Mount || cls == facts.requirement)
                                fit.races[cls] = FitRaces(spell.SpellId, cls);
                        input.fits.push_back(fit);
                    }
                }
                input.trainers.push_back(std::move(facts));
            }
            input.trainerEntries.emplace_back(entry, known->second);
        }

        if (!(creature->npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
        if (!items)
            continue;
        if (items->Empty())
        {
            ++input.emptyVendors;
            continue;
        }
        for (VendorItem const* sold : items->m_items)
        {
            if (!sold)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(sold->item);
            if (!proto)
            {
                ++input.missingItems;
                continue;
            }
            input.vendorItems.push_back(
                {entry, sold->item, sold->maxcount, sold->incrtime, sold->ExtendedCost, sold->IsGoldRequired(proto)});
            if (itemsSeen.insert(sold->item).second)
                input.items.push_back(FactsOf(proto));
        }
    }
    return input;
}

std::string TrainerCounts(UpkeepStats const& s)
{
    std::string text;
    for (std::size_t kind = 0; kind < TRAINER_KIND_COUNT; ++kind)
        text += Acore::StringFormat("{}{} {} ({} spells)", text.empty() ? "" : ", ", s.trainers[kind],
                                    TRAINER_KIND_NAMES[kind], s.trainerSpells[kind]);
    return text;
}

std::string ClassCounts(UpkeepStats const& s)
{
    std::string text;
    for (std::size_t cls = 1; cls < UPKEEP_CLASS_SLOTS; ++cls)
        if (*CLASS_NAMES[cls])
            text += Acore::StringFormat("{}{} {}", text.empty() ? "" : ", ", CLASS_NAMES[cls], s.classSpells[cls]);
    return text;
}

std::string KindCounts(UpkeepStats const& s)
{
    std::string text;
    for (std::size_t kind = 0; kind < UPKEEP_STOCK_COUNT; ++kind)
        text += Acore::StringFormat("{}{} {} vendors / {} items", text.empty() ? "" : ", ", UPKEEP_STOCK_NAMES[kind],
                                    s.vendors[kind], s.items[kind]);
    return text;
}

void AddStock(Snapshot& snapshot, UpkeepIndex const* index, Item const* item, uint8 cls)
{
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return;
    UpkeepItemFacts const facts = FactsOf(proto);
    uint8 const kinds = index ? index->KindsOf(facts, cls) : StockKinds(facts);
    if (!kinds)
        return;
    for (StockItem& entry : snapshot.stock)
        if (entry.item == facts.item)
        {
            entry.count += item->GetCount();
            return;
        }
    snapshot.stock.push_back({facts.item, item->GetCount(), kinds, DietBit(facts.foodType),
                              static_cast<uint8>(facts.subClass),
                              static_cast<uint8>(std::min<uint32>(facts.requiredLevel, 255)), facts.ammoDamage});
}

// The known bits (the trainer's Known state: the spell, or every spell it teaches) and the can-learn bits (its
// IsSpellFitByClassAndRace check, Trainer.cpp, precomputed per class and race in the index; a bot whose race differs
// from its original race asks the core): the class lists are the union over both factions' trainers
void FillKnown(Player* bot, TrainerList const* list, std::vector<std::uint64_t>& known,
               std::vector<std::uint64_t>& canLearn)
{
    if (!list)
        return;
    known = SpellBits(list->spells.size());
    canLearn = SpellBits(list->spells.size());
    auto const hasSpell = [bot](uint32 spellId) { return bot->HasSpell(spellId); };
    uint8 const cls = bot->getClass();
    uint8 const race = bot->getRace();
    bool const precomputed = race == bot->getRace(true);
    for (std::size_t position = 0; position < list->spells.size(); ++position)
    {
        TrainerSpell const& spell = list->spells[position];
        if (KnowsTrainerSpell(spell, hasSpell))
            SetSpellBit(known, position);
        if (precomputed ? list->Fits(position, cls, race) : bot->IsSpellFitByClassAndRace(spell.spellId))
            SetSpellBit(canLearn, position);
    }
}

// The direct forms of the can-learn and off-list bits (one core call per list spell), against which `planner needs`
// checks the snapshot's precomputed ones: the number of positions that differ
std::size_t FitDifferences(Player* bot, TrainerList const* list, std::vector<std::uint64_t> const& canLearn)
{
    std::size_t differ = 0;
    if (list)
        for (std::size_t position = 0; position < list->spells.size(); ++position)
            if (HasSpellBit(canLearn, position) != bot->IsSpellFitByClassAndRace(list->spells[position].spellId))
                ++differ;
    return differ;
}

std::size_t OffListDifferences(Player* bot, TrainerList const* list, TrainerList const* riding,
                               std::vector<std::uint64_t> const& met)
{
    std::size_t differ = 0;
    if (!list)
        return differ;
    for (std::size_t position = 0; position < list->spells.size(); ++position)
    {
        bool known = true;
        for (uint32 const ability : list->spells[position].reqAbility)
            if (ability && list->Find(ability) == TrainerList::NOT_FOUND &&
                list->Teacher(ability) == TrainerList::NOT_FOUND &&
                (!riding || riding->Find(ability) == TrainerList::NOT_FOUND) && !bot->HasSpell(ability))
                known = false;
        if (known != HasSpellBit(met, position))
            ++differ;
    }
    return differ;
}
}  // namespace

void StartUpkeepIndex()
{
    UpkeepIndexState expected = UpkeepIndexState::NotStarted;
    if (!upkeepState.compare_exchange_strong(expected, UpkeepIndexState::Building))
        return;  // once: trainers and vendor lists are static after startup

    auto const gatherStart = std::chrono::steady_clock::now();
    auto input = std::make_shared<UpkeepGather>(GatherUpkeep());
    uint32 const gatherMs = MsSince(gatherStart);
    LOG_INFO("server.loading",
             "Upkeep index: gathered {} trainers for {} creature entries, {} vendor rows of {} items, {} class reagent "
             "uses in {} ms ({} empty vendor lists, {} rows without an item template)",
             input->trainers.size(), input->trainerEntries.size(), input->vendorItems.size(), input->items.size(),
             input->reagents.size(), gatherMs, input->emptyVendors, input->missingItems);

    bool const posted = Pool().Post(
        [input, gatherMs]
        {
            auto const buildStart = std::chrono::steady_clock::now();
            try
            {
                auto index = std::make_shared<UpkeepIndex>(UpkeepIndex::Build(std::move(*input)));
                index->version = PUBLISHED_VERSION;
                index->stats.gatherMs = gatherMs;
                index->stats.buildMs = MsSince(buildStart);
                UpkeepStats const& s = index->stats;
                LOG_INFO("playerbots",
                         "Upkeep index built in {} ms on a worker: trainers {}; class spells: {}; vendors per kind: "
                         "{}; {} class reagent items, {} currency-only rows skipped, {} duplicate trainers, {} spell "
                         "conflicts, about {:.2f} MB (estimate)",
                         s.buildMs, TrainerCounts(s), ClassCounts(s), KindCounts(s), s.reagentItems, s.currencyOnly,
                         s.duplicateTrainers, s.spellConflicts, static_cast<double>(s.estimatedBytes) / BYTES_PER_MB);
                upkeepBook.Publish(std::move(index));
                upkeepState.store(UpkeepIndexState::Ready, std::memory_order_release);
            }
            catch (std::exception const& e)
            {
                upkeepState.store(UpkeepIndexState::Failed, std::memory_order_release);
                LOG_ERROR("playerbots", "Upkeep index: the build failed: {}", e.what());
            }
        });
    if (!posted)
    {
        upkeepState.store(UpkeepIndexState::Failed, std::memory_order_release);
        LOG_ERROR("server.loading", "Upkeep index: the planner pool is not running; no upkeep index");
    }
}

std::shared_ptr<UpkeepIndex const> CurrentUpkeepIndex() { return upkeepBook.Current(); }

bool UpkeepIndexReady() { return upkeepState.load(std::memory_order_acquire) == UpkeepIndexState::Ready; }

std::vector<std::string> UpkeepIndexStatus()
{
    UpkeepIndexState const state = upkeepState.load(std::memory_order_acquire);
    std::shared_ptr<UpkeepIndex const> const index = state == UpkeepIndexState::Ready ? CurrentUpkeepIndex() : nullptr;
    if (!index)
        return {Acore::StringFormat("PLANNER upkeep index: {}", state == UpkeepIndexState::Building ? "building"
                                                                : state == UpkeepIndexState::Failed ? "failed"
                                                                                                    : "not started")};
    UpkeepStats const& s = index->stats;
    return {Acore::StringFormat("PLANNER upkeep index: ready, trainers {}, {} creature entries ({} orphan); {} class "
                                "reagent items, {} currency-only rows skipped, gathered in {} ms, built in {} ms on a "
                                "worker, about {:.2f} MB (estimate)",
                                TrainerCounts(s), s.trainerEntries, s.orphanEntries, s.reagentItems, s.currencyOnly,
                                s.gatherMs, s.buildMs, static_cast<double>(s.estimatedBytes) / BYTES_PER_MB),
            Acore::StringFormat("PLANNER upkeep class spells: {}", ClassCounts(s)),
            Acore::StringFormat("PLANNER upkeep vendors per kind: {}", KindCounts(s))};
}

void FillUpkeep(Player* bot, Snapshot& snapshot)
{
    auto const started = std::chrono::steady_clock::now();
    std::shared_ptr<UpkeepIndex const> const index = UpkeepIndexReady() ? CurrentUpkeepIndex() : nullptr;
    uint8 const cls = bot->getClass();

    if (Pet* pet = bot->GetPet())
        if (pet->getPetType() == HUNTER_PET)
            if (CreatureTemplate const* creature = pet->GetCreatureTemplate())
                if (CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(creature->family))
                    snapshot.petDiet = static_cast<uint8>(family->petFoodMask);
    // The ammo the ranged weapon fires: only that ammo counts and is bought (WU6)
    if (Item const* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        if (ItemTemplate const* proto = ranged->GetTemplate())
            if (proto->Class == ITEM_CLASS_WEAPON)
                snapshot.ammoSubClass = AmmoFor(proto->SubClass);
    // The equipped ammo: the item usage keeps only ammo at least as good, so only that counts and is bought (WU6);
    // an empty quiver counts as none equipped (the item usage's own rule)
    if (uint32 const ammo = Upkeep::EquippedAmmoId(bot))
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(ammo))
            snapshot.ammoDamage = AmmoDamage(proto);

    // Every bag counts: quivers, ammo pouches and soul bags hold stock too
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            AddStock(snapshot, index.get(), item, cls);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag const* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item const* item = bag->GetItemByPos(static_cast<uint8>(slot)))
                    AddStock(snapshot, index.get(), item, cls);

    if (index)
    {
        snapshot.upkeepVersion = index->version;
        TrainerList const* classList = index->ClassList(cls);
        TrainerList const* riding = index->RidingList(bot->getRace(true));
        FillKnown(bot, classList, snapshot.knownClassSpells, snapshot.canLearnClassSpells);
        FillKnown(bot, riding, snapshot.knownRidingSpells, snapshot.canLearnRidingSpells);
        if (classList)
            snapshot.offListMetClassSpells =
                OffListMet(*classList, riding, [bot](uint32 spellId) { return bot->HasSpell(spellId); });
    }
    // Its own PerfMonitor row (map-thread microseconds; `planner needs` calls add world-thread samples)
    auto const us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner snapshot upkeep"))
        row->Record(static_cast<uint64>(std::max<long long>(us, 0)));
}

UpkeepNeeds::NeedRates BotNeedRates(PlayerbotAI* botAI, Snapshot const& snapshot)
{
    UpkeepNeeds::NeedRates rates;
    // The bot's own rates over its rolling measure, as a bag purchase reads them (BuyAction.cpp)
    WorldTravel::SlotMeasure const& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    WorldTravel::SlotRates const slotRates = WorldTravel::CurrentRates(
        measure, sPlayerbotAIConfig.WorldTravel.SlotBands[WorldTravel::LevelBand(snapshot.level)]);
    rates.earnCopperPerMinute = slotRates.EarnCopperPerMinute;
    if (snapshot.walkMinutesPerHour > 0.0f)
        rates.walkMinutesPerHour = snapshot.walkMinutesPerHour;
    rates.flightMinutesPerHour = snapshot.flightMinutesPerHour;
    rates.trainingReserveLevels = sPlayerbotAIConfig.Upkeep.TrainingReserveLevels;
    UpkeepNeeds::FillUse(rates, measure);
    return rates;
}

bool HandleNeedsCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty() || !normalizePlayerName(name))
    {
        handler->SendErrorMessage("Usage: playerbots planner needs <bot name> (an online bot)");
        return false;
    }
    Player* bot = ObjectAccessor::FindPlayerByName(name);
    PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("PLANNER: {} is not an online bot", name);
        return false;
    }
    std::shared_ptr<UpkeepIndex const> const index = UpkeepIndexReady() ? CurrentUpkeepIndex() : nullptr;
    if (!index)
    {
        handler->SendErrorMessage("PLANNER: the upkeep index is not ready");
        return false;
    }

    // Commands run on the world thread inside World::Update, outside MapMgr::Update (which waits for its map
    // threads), so no map thread touches the bot while the snapshot reads it, as for every GM command on a player
    PLAYERBOT_CHECK_WORLD_THREAD("planner needs");
    Snapshot const snapshot = FillSnapshot(bot, 0);
    UpkeepNeeds::NeedRates const rates = BotNeedRates(botAI, snapshot);
    UpkeepNeeds::NeedList const list = UpkeepNeeds::ComputeNeeds(snapshot, *index, rates);
    handler->PSendSysMessage(
        "PLANNER needs of {}: level {}, class {}, e {} c/min, walk {} min/h, durability {}% "
        "(lowest {}%, {} broken), {} stock entries",
        bot->GetName(), snapshot.level, snapshot.cls, rates.earnCopperPerMinute, rates.walkMinutesPerHour,
        snapshot.durabilityPercent, snapshot.lowestDurabilityPercent, snapshot.brokenItems, snapshot.stock.size());
    // The use per hour the stock targets read: measured from stock falls between vendor visits, else the default
    std::string use;
    for (std::size_t kind = 0; kind < UPKEEP_STOCK_COUNT; ++kind)
    {
        UpkeepStock const stock = static_cast<UpkeepStock>(kind);
        if (stock == UpkeepStock::Mount)
            continue;
        bool const measured = rates.usePerHour[kind] > 0.0f;
        use += Acore::StringFormat(
            "{}{} {:.1f} ({})", use.empty() ? "" : ", ", UPKEEP_STOCK_NAMES[kind],
            measured ? rates.usePerHour[kind] : UpkeepNeeds::DefaultUsePerHour(snapshot.cls, snapshot.level, stock),
            measured ? "measured" : "default");
    }
    handler->PSendSysMessage("PLANNER   use per hour: {}; ranged ammo subclass {}, equipped ammo damage {:.1f}", use,
                             snapshot.ammoSubClass, snapshot.ammoDamage / 1000.0f);
    for (std::string const& line : UpkeepNeeds::DescribeAll(list))
        handler->PSendSysMessage("PLANNER   {}", line);

    // The snapshot's precomputed can-learn and off-list bits against the core's direct answers
    TrainerList const* classList = index->ClassList(snapshot.cls);
    TrainerList const* riding = index->RidingList(bot->getRace(true));
    std::size_t const differ = FitDifferences(bot, classList, snapshot.canLearnClassSpells) +
                               FitDifferences(bot, riding, snapshot.canLearnRidingSpells) +
                               OffListDifferences(bot, classList, riding, snapshot.offListMetClassSpells);
    std::string const check = Acore::StringFormat(
        "PLANNER check of {}: precomputed can-learn and off-list bits {} the direct ones ({} class, {} riding spells, "
        "{} differ)",
        bot->GetName(), differ ? "DIFFER from" : "equal", classList ? classList->spells.size() : 0,
        riding ? riding->spells.size() : 0, differ);
    handler->PSendSysMessage("{}", check);
    if (differ)
        LOG_ERROR("playerbots", "{}", check);
    return true;
}
}  // namespace Planner
