/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftVisit.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "Bag.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "CheckMountStateAction.h"
#include "CraftRules.h"
#include "EventLog.h"
#include "GameObject.h"
#include "GoalInterface.h"
#include "InventorySlots.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "Map.h"
#include "MaterialRules.h"
#include "MotionMaster.h"
#include "MountRules.h"
#include "MoveSpline.h"
#include "NewRpgHelpers.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ProfessionConfig.h"
#include "RandomPlayerbotMgr.h"
#include "SkillPathTypes.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace BankMail
{
namespace
{
// The bot looks for the station once it is this close to the spawn (the spawn is the station's own position)
constexpr float STATION_SEARCH_YARDS = 20.0f;
// The walk stops this far inside the station's own range, so the cast's range test (IsWithinDistInMap) holds
constexpr float RANGE_MARGIN_YARDS = 1.0f;
constexpr float MIN_APPROACH_YARDS = 1.0f;

struct ProcessSpell
{
    char const* word;
    uint32 spell;
    Recipes::ProcessKind kind;
};
constexpr ProcessSpell PROCESS_SPELLS[] = {
    {"disenchant", Recipes::ProcessSpell(Recipes::ProcessKind::Disenchant), Recipes::ProcessKind::Disenchant},
    {"prospect", Recipes::ProcessSpell(Recipes::ProcessKind::Prospect), Recipes::ProcessKind::Prospect},
    {"mill", Recipes::ProcessSpell(Recipes::ProcessKind::Mill), Recipes::ProcessKind::Mill}};

ProcessSpell const* ProcessOfSpell(uint32 spell)
{
    for (ProcessSpell const& process : PROCESS_SPELLS)
        if (process.spell == spell)
            return &process;
    return nullptr;
}

// The index's process row the item falls under (disenchant: by the item's loot id; prospect, mill: by its entry)
Recipes::Process const* ProcessRowOf(Recipes::RecipeIndex const& index, Recipes::ProcessKind kind,
                                     ItemTemplate const* proto)
{
    uint32 const key = kind == Recipes::ProcessKind::Disenchant ? proto->DisenchantID : proto->ItemId;
    if (!key)
        return nullptr;
    for (Recipes::Process const& row : index.Processes())
        if (row.kind == kind && row.source == key)
            return &row;
    return nullptr;
}

// How many of the item the process takes per cast, or 0 when the bot may not process it (its skill, the core's
// disenchant skill)
uint32 ConsumedBy(Player* bot, Recipes::RecipeIndex const& index, Recipes::ProcessKind kind, ItemTemplate const* proto)
{
    Recipes::Process const* row = proto ? ProcessRowOf(index, kind, proto) : nullptr;
    if (!row)
        return 0;
    uint32 const skill = bot->GetSkillValue(row->skill);
    if (skill < row->rank)
        return 0;
    if (kind == Recipes::ProcessKind::Disenchant && proto->RequiredDisenchantSkill > 0 &&
        skill < static_cast<uint32>(proto->RequiredDisenchantSkill))
        return 0;
    return std::max<uint32>(row->consumed, 1);
}

// Bag reads: the bot's own map thread inside its AI update, or the world thread between the map updates. Each bag
// item, backpack first, until the visitor says stop
template <typename Visitor>
void ForEachBagItem(Player* bot, Visitor&& visit)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (!visit(item))
                return;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (Item* item = bag->GetItemByPos(static_cast<uint8>(slot)))
                if (!visit(item))
                    return;
    }
}

// A stack of the entry holding at least minCount (the core checks the target stack, not the bags' total)
Item* StackOf(Player* bot, uint32 entry, uint32 minCount)
{
    Item* found = nullptr;
    ForEachBagItem(bot,
                   [&](Item* item)
                   {
                       if (item->GetEntry() != entry || item->GetCount() < minCount)
                           return true;
                       found = item;
                       return false;
                   });
    return found;
}

Recipes::Recipe const* RecipeOf(Recipes::RecipeIndex const& index, uint32 spell) { return index.RecipeOf(spell); }

// The queue positions a craft errand's stop looks at (one craft visit each at most)
constexpr std::size_t CRAFT_STOP_QUEUE = 8;
// After the queue, the disenchant visits a stop makes at most (one item entry each, WR14c), and the casts of one
constexpr std::size_t STOP_PROCESSES = 4;
constexpr uint32 STOP_PROCESS_CASTS = 10;
constexpr uint32 DISENCHANT_SPELL = Recipes::ProcessSpell(Recipes::ProcessKind::Disenchant);

CraftRules::RecipeView ViewOf(Recipes::Recipe const& recipe)
{
    CraftRules::RecipeView view;
    view.spell = recipe.spell;
    view.skillNeeded = recipe.minRank;
    view.product = recipe.product;
    view.productCount = recipe.productCount;
    view.focus = recipe.focus;
    for (Recipes::ItemCount const& reagent : recipe.reagents)
        view.reagents.push_back({reagent.item, reagent.count});
    view.tools = recipe.tools;
    view.toolCategories = recipe.toolCategories;
    return view;
}

// The bags as the craft rules see them for one recipe: the counts of its reagents, tools and product, the free
// slots, the skill, the tool categories held and the station in reach
CraftRules::BagState BagsOf(Player* bot, Recipes::Recipe const& recipe, CraftRules::RecipeView const& view,
                            bool stationInReach)
{
    CraftRules::BagState bags;
    auto const note = [&](uint32 item)
    {
        if (!item || bags.items.count(item))
            return;
        bags.items[item] = bot->GetItemCount(item, false);
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item))
            bags.maxStack[item] = proto->GetMaxStackSize();
    };
    for (CraftRules::Reagent const& reagent : view.reagents)
        note(reagent.item);
    for (uint32 tool : view.tools)
        note(tool);
    note(view.product);
    bags.capacity = WorldTravel::FreeGeneralSlots(bot) + bags.SlotsUsed();
    bags.skill = bot->GetSkillValue(recipe.skill);
    for (uint32 category : view.toolCategories)
        if (bot->HasItemTotemCategory(category))
            bags.categoriesHeld.insert(category);
    if (stationInReach && view.focus)
        bags.focusInReach.insert(view.focus);
    return bags;
}

std::optional<VisitEnd> EndOfVerdict(CraftRules::Verdict verdict)
{
    switch (verdict)
    {
        case CraftRules::Verdict::Castable:
            return std::nullopt;
        case CraftRules::Verdict::NoMaterials:
        case CraftRules::Verdict::NoTool:
            return VisitEnd::NoMaterials;
        case CraftRules::Verdict::BagsFull:
            return VisitEnd::BagsFull;
        case CraftRules::Verdict::NoStation:
            return VisitEnd::NoStation;
        case CraftRules::Verdict::NoSkill:
        default:
            return VisitEnd::Done;  // nothing more to make here; the log line names the cause
    }
}

// A cast the core refused (PlayerbotAI::LastPrepareResult): the visit's end for it; the raw result is logged
VisitEnd EndOfCastResult(int32 result)
{
    switch (result)
    {
        case SPELL_FAILED_REAGENTS:
        case SPELL_FAILED_TOTEMS:
        case SPELL_FAILED_TOTEM_CATEGORY:
        case SPELL_FAILED_ITEM_NOT_FOUND:
        case SPELL_FAILED_ITEM_GONE:
        case SPELL_FAILED_NEED_MORE_ITEMS:
            return VisitEnd::NoMaterials;
        case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
            return VisitEnd::NoStation;
        case SPELL_FAILED_TOO_MANY_OF_ITEM:
        case SPELL_FAILED_DONT_REPORT:  // the core's create-item check sends its bag error and reports nothing
            return VisitEnd::BagsFull;
        default:
            return VisitEnd::Done;
    }
}

// The visit's profession decision row: made, or skipped with its cause
// What a unit given up is worth, in copper: the skill path's cost of a material it uses (its cheapest source), else
// what a vendor pays for it
uint32 CopperEach(SkillPath::Path const* path, uint32 item)
{
    if (SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, item) : nullptr)
        return material->copperEach;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return proto ? proto->SellPrice : 0;
}

// The first reagent of the recipe the bot holds too few of for one cast (WR19c), else 0 (a tool it lacks)
uint32 LackedReagent(Player* bot, Recipes::Recipe const& recipe)
{
    for (Recipes::ItemCount const& reagent : recipe.reagents)
        if (bot->GetItemCount(reagent.item, false) < reagent.count)
            return reagent.item;
    return 0;
}

// The visit's profession row (WR19b's amounts, WR14b): value the products' worth in copper (what a vendor pays for the
// recipe's products made; 0 for a processing cast, whose random yields are the ItemIn craft rows), reagents the copper
// the casts used up (CopperEach per unit, the processed items for a processing cast), made the casts made. The units
// used up are one ItemOut craft row per reagent (WR19c); a visit stopped for its materials names in place of the
// source the material it lacked (missing, 0: a tool)
void WriteCraftRow(PlayerbotAI* botAI, Player* bot, CraftVisitData const& visit, VisitEnd end)
{
    using Professions::Choice;
    using Professions::Reason;
    Planner::GoalApply::BotGoals const* goals = botAI->planner.goals.get();
    SkillPath::Path const* path = goals ? goals->path.get() : nullptr;
    // a craft for sale (WA7b) is told by its spell in the queue
    Reason const reason = visit.made ? (path && SkillPath::ForSale(*path, visit.spell) ? Reason::Sale : Reason::UseSoon)
                          : end == VisitEnd::NoMaterials ? Reason::NoSource
                          : end == VisitEnd::NoStation || end == VisitEnd::NoSpot || end == VisitEnd::Unreached
                              ? Reason::NoStation
                              : Reason::Locked;
    bool const lacked = !visit.made && end == VisitEnd::NoMaterials;
    std::shared_ptr<Recipes::RecipeIndex const> const index =
        visit.made || lacked ? Planner::CurrentRecipeIndex() : nullptr;
    uint64 products = 0;
    uint64 reagents = 0;
    uint32 missing = 0;
    if (ProcessSpell const* process = index ? ProcessOfSpell(visit.spell) : nullptr)
    {
        uint64 const units =
            uint64(visit.made) * ConsumedBy(bot, *index, process->kind, sObjectMgr->GetItemTemplate(visit.source));
        reagents = units * CopperEach(path, visit.source);
        if (units)
            EventLog::Emit(bot, EventLog::EventType::ItemOut, "craft", visit.source, static_cast<int64>(units));
        missing = visit.source;
    }
    else if (Recipes::Recipe const* recipe = index ? RecipeOf(*index, visit.spell) : nullptr)
    {
        ItemTemplate const* product = recipe->product ? sObjectMgr->GetItemTemplate(recipe->product) : nullptr;
        products = product ? uint64(visit.made) * std::max<uint32>(recipe->productCount, 1) * product->SellPrice : 0;
        for (Recipes::ItemCount const& reagent : recipe->reagents)
        {
            uint64 const units = uint64(visit.made) * reagent.count;
            reagents += units * CopperEach(path, reagent.item);
            if (units)
                EventLog::Emit(bot, EventLog::EventType::ItemOut, "craft", reagent.item, static_cast<int64>(units));
        }
        if (lacked)
            missing = LackedReagent(bot, *recipe);
    }
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession,
                       Professions::ChoiceWord(visit.made ? Choice::Craft : Choice::Skip),
                       Professions::ReasonWord(reason), static_cast<float>(products),
                       {{"reagents", static_cast<float>(reagents)},
                        {"made", static_cast<float>(visit.made)},
                        {lacked ? "missing" : "source", static_cast<float>(lacked ? missing : visit.source)}},
                       visit.spell);
}

uint32 StepBoundMs(uint32 spell)
{
    uint32 castMs = 0;
    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
        if (info->CastTimeEntry && info->CastTimeEntry->CastTime > 0)
            castMs = static_cast<uint32>(info->CastTimeEntry->CastTime);
    return std::max(sPlayerbotAIConfig.Professions.VisitStepSeconds * IN_MILLISECONDS, castMs);
}

// The next cast checked with the craft rules: its end, or nothing with the item target (null for a recipe) and the
// bag counts its result is read against
std::optional<VisitEnd> CheckCast(Player* bot, CraftVisitData& visit, Item*& target)
{
    target = nullptr;
    std::shared_ptr<Recipes::RecipeIndex const> const index = Planner::CurrentRecipeIndex();
    if (!index)
        return VisitEnd::Done;
    if (ProcessSpell const* process = ProcessOfSpell(visit.spell))
    {
        uint32 const consumed = ConsumedBy(bot, *index, process->kind, sObjectMgr->GetItemTemplate(visit.source));
        if (!consumed)
            return VisitEnd::Done;  // no longer processable by the bot
        target = StackOf(bot, visit.source, consumed);
        if (!target)
            return VisitEnd::NoMaterials;
        if (!WorldTravel::FreeGeneralSlots(bot))
            return VisitEnd::BagsFull;
        visit.productBefore = 0;
        visit.sourceBefore = bot->GetItemCount(visit.source, false);
    }
    else
    {
        Recipes::Recipe const* recipe = RecipeOf(*index, visit.spell);
        if (!recipe)
            return VisitEnd::Done;
        CraftRules::RecipeView const view = ViewOf(*recipe);
        if (std::optional<VisitEnd> const end = EndOfVerdict(CraftRules::Check(view, BagsOf(bot, *recipe, view, true))))
            return end;
        visit.productBefore = recipe->product ? bot->GetItemCount(recipe->product, false) : 0;
        visit.sourceBefore = recipe->reagents.empty() ? 0 : bot->GetItemCount(recipe->reagents.front().item, false);
    }
    return std::nullopt;
}

// A player stops, gets off the mount and out of a travel form before a craft: the core refuses a cast while mounted
// (WR8 run 1: SPELL_FAILED_NOT_MOUNTED) and PlayerbotAI::CastSpell one with a cast time while moving (the walk into the
// station's range). The mount policy keeps a visiting bot on foot (CheckMountStateAction::AskMountPolicy; run 2: the
// mount action remounted it after each dismount), so one update settles it. False while the bot still settles
bool SettledForCast(PlayerbotAI* botAI, Player* bot)
{
    if (NewRpgHelpers::RealShapeshift(bot))
        botAI->RemoveShapeshift();
    if (!bot->IsMounted() && !bot->isMoving())
        return true;
    // The one dismount request (it stops the bot too); Plain until MountRules::How has a craft word
    CheckMountStateAction::Dismount(bot, MountRules::How::Plain);
    return false;
}

// In reach: the next cast checked and started (its result awaited), or the bot still settles; or an end
VisitTurn StartCast(PlayerbotAI* botAI, Player* bot, CraftVisitData& visit)
{
    Item* target = nullptr;
    if (std::optional<VisitEnd> const end = CheckCast(bot, visit, target))
        return VisitTurn{VisitNext::End, *end};
    if (!SettledForCast(botAI, bot))
    {
        // The settle runs on the cast's clock (castBoundMs 0: none runs) for one step at most; then the cast goes
        // ahead and the core's result names what kept the bot
        uint32 const nowMs = getMSTime();
        if (!visit.castBoundMs)
        {
            visit.castMs = nowMs;
            visit.castBoundMs = sPlayerbotAIConfig.Professions.VisitStepSeconds * IN_MILLISECONDS;
        }
        if (getMSTimeDiff(visit.castMs, nowMs) < visit.castBoundMs)
            return VisitTurn{VisitNext::Pending};
        LOG_INFO("playerbots",
                 "[Professions] {} craft visit settle bound: mounted {}, moving {}, spline done {}, motion {}",
                 bot->GetName(), bot->IsMounted(), bot->isMoving(), bot->movespline->Finalized(),
                 static_cast<uint32>(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()));
    }

    // The real spell, as a player casts it; a nocast test bot starts none (TestConstraints::CastsOnItsOwn). An own
    // fire is lit first (WR15), once the recipe's own checks pass
    uint32 const spell = visit.fire ? visit.fire : visit.spell;
    if (!botAI->CastSpell(spell, bot, visit.fire ? nullptr : target))
    {
        ++visit.refused;
        visit.lastResult = botAI->LastPrepareResult();
        return VisitTurn{VisitNext::End, visit.fire ? VisitEnd::NoStation : EndOfCastResult(visit.lastResult)};
    }
    visit.castMs = getMSTime();
    visit.castBoundMs = StepBoundMs(spell);
    return VisitTurn{VisitNext::Await};
}

// The fire the bot's own spell summoned: no owner list holds it. Basic Campfire's effect is SPELL_EFFECT_TRANS_DOOR
// (Spell.dbc, WR15 run 5), whose object Spell::EffectTransmitted puts at the effect's radius before the caster and adds
// to no list (only fishing, ritual and duel objects); a wild summon joins none either. So it is the nearest object of
// the spell's summon entry within that radius and a step
GameObject const* OwnFire(Player* bot, uint32 spell)
{
    if (GameObject const* owned = bot->GetGameObject(spell))
        return owned;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!info)
        return nullptr;
    for (SpellEffectInfo const& effect : info->Effects)
        if ((effect.Effect == SPELL_EFFECT_TRANS_DOOR || effect.Effect == SPELL_EFFECT_SUMMON_OBJECT_WILD) &&
            effect.MiscValue > 0)
            if (GameObject const* placed = bot->FindNearestGameObject(static_cast<uint32>(effect.MiscValue),
                                                                      effect.CalcRadius(bot) + INTERACTION_DISTANCE))
                return placed;
    return nullptr;
}

// The own fire's cast (WR15): lit once the bot owns the fire it summons (a CraftVisit fire row: the spell, the
// fire's gameobject entry), then the recipe's casts; none by the cast's bound ends the visit no_station
VisitTurn AwaitFire(Player* bot, CraftVisitData& visit)
{
    if (GameObject const* lit = OwnFire(bot, visit.fire))
    {
        EventLog::Emit(bot, EventLog::EventType::CraftVisit, "fire", visit.fire, lit->GetEntry());
        LOG_DEBUG("playerbots", "[Professions] {} craft visit lit its own fire: spell {}, object {}", bot->GetName(),
                  visit.fire, lit->GetEntry());
        visit.fire = 0;
        visit.castBoundMs = 0;
        return VisitTurn{VisitNext::Continue};
    }
    if (getMSTimeDiff(visit.castMs, getMSTime()) >= visit.castBoundMs)
    {
        ++visit.refused;
        return VisitTurn{VisitNext::End, VisitEnd::NoStation};
    }
    return VisitTurn{VisitNext::Pending};
}

// The awaited cast's result from the bags: made (the product came, or the reagent or the processed item went) or
// not; nullopt while it waits
std::optional<bool> CastResult(Player* bot, CraftVisitData const& visit)
{
    if (bot->IsNonMeleeSpellCast(false))
        return std::nullopt;
    if (visit.source)
        return bot->GetItemCount(visit.source, false) < visit.sourceBefore;
    std::shared_ptr<Recipes::RecipeIndex const> const index = Planner::CurrentRecipeIndex();
    Recipes::Recipe const* recipe = index ? RecipeOf(*index, visit.spell) : nullptr;
    if (!recipe)
        return false;
    if (recipe->product)
        return bot->GetItemCount(recipe->product, false) > visit.productBefore;
    return !recipe->reagents.empty() && bot->GetItemCount(recipe->reagents.front().item, false) < visit.sourceBefore;
}

uint32 LimitSeconds() { return sPlayerbotAIConfig.Professions.CraftVisitDeadlineSeconds; }

// Once per visit: a grid search for the indexed station's template near its spawn
ObjectGuid FindStation(Player* bot, VisitState const& visit)
{
    GameObject const* found = bot->FindNearestGameObject(visit.entry, STATION_SEARCH_YARDS, true);
    return found ? found->GetGUID() : ObjectGuid::Empty;
}

// The core's own test (Spell::CheckSpellFocus): spawned, and the caster within the object's own range
WorldObject const* ResolveStation(Player* bot, ObjectGuid station)
{
    GameObject const* found = bot->GetMap()->GetGameObject(station);
    return found && found->isSpawned() ? found : nullptr;
}

bool StationInReach(Player* bot, WorldObject const& station, float& range)
{
    range = static_cast<float>(static_cast<GameObject const&>(station).GetGOInfo()->spellFocus.dist);
    return station.IsWithinDistInMap(bot, range);
}

// The last yards into the station's range (WT68): a little inside it, at least MIN_APPROACH_YARDS (a station of range
// 0 is approached to that)
float StationApproach(float range) { return std::max(range - RANGE_MARGIN_YARDS, MIN_APPROACH_YARDS); }

// The awaited cast's result: a made one leads to the next cast; one ended without its product ends the visit with
// the cause the rules name now, or done (never cast again blind); one still current past its bound times it out
VisitTurn AwaitCast(PlayerbotAI* botAI, Player* bot)
{
    CraftVisitData& visit = botAI->bankMail.craftData;
    if (visit.fire)
        return AwaitFire(bot, visit);
    std::optional<bool> const made = CastResult(bot, visit);
    if (!made)
    {
        if (getMSTimeDiff(visit.castMs, getMSTime()) >= visit.castBoundMs)
            return VisitTurn{VisitNext::End, VisitEnd::Timeout};
        return VisitTurn{VisitNext::Pending};
    }
    visit.castBoundMs = 0;
    if (!*made)
    {
        ++visit.refused;
        Item* target = nullptr;
        return VisitTurn{VisitNext::End, CheckCast(bot, visit, target).value_or(VisitEnd::Done)};
    }
    ++visit.made;
    return VisitTurn{VisitNext::Continue};
}

VisitTurn CastNext(PlayerbotAI* botAI, Player* bot)
{
    CraftVisitData& visit = botAI->bankMail.craftData;
    if (visit.made >= visit.wanted)
        return VisitTurn{VisitNext::End, VisitEnd::Done};
    return StartCast(botAI, bot, visit);
}

bool WriteCraftEnd(PlayerbotAI* botAI, Player* bot, VisitState const& state, VisitEnd end)
{
    CraftVisitData& visit = botAI->bankMail.craftData;
    char const* how = VisitEndWord(VisitKind::Craft, end);
    // The visit's own cast does not outlive it (death, a teleport and a logout cancel it in the core already)
    if (state.phase == VisitPhase::Await && bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    // Bag read: the bot's own map thread inside its AI update, or the world thread between the map updates
    uint32 const freeAfter = WorldTravel::FreeGeneralSlots(bot);
    if (WorldLog::Active())
        WriteCraftRow(botAI, bot, visit, end);
    EventLog::Emit(bot, EventLog::EventType::CraftVisit, how,
                   WorldLogFormat::PackVisitCounts(visit.made, visit.refused), freeAfter);
    LOG_DEBUG("playerbots",
              "[Professions] {} craft visit {}: spell {} source {}, {} of {} made, {} refused, cast result "
              "{}, {} free, {} s",
              bot->GetName(), how, visit.spell, visit.source, visit.made, visit.wanted, visit.refused, visit.lastResult,
              freeAfter, getMSTimeDiff(state.startedMs, getMSTime()) / IN_MILLISECONDS);

    // A made upgrade is put on now (the one way to use an item); what it replaces stays for the vendor errand
    bool const made = visit.made > 0;
    visit = CraftVisitData{};
    return made;
}

constexpr VisitKindRow CRAFT_VISIT{.startWord = "craft",
                                   .searchYards = STATION_SEARCH_YARDS,
                                   .missing = VisitEnd::NoStation,
                                   .limitSeconds = &LimitSeconds,
                                   .find = &FindStation,
                                   .resolve = &ResolveStation,
                                   .reach = &StationInReach,
                                   .approach = &StationApproach,
                                   .awaited = &AwaitCast,
                                   .act = &CastNext,
                                   .end = &WriteCraftEnd};
}  // namespace

bool CanLightOwnStation(Player* bot, uint32 spell)
{
    if (!spell || !bot->HasSpell(spell))
        return false;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!info)
        return false;
    // The cast's own item tests (Spell::CheckItems): the reagents unless the bot casts without them, then the tools
    // (Basic Campfire: Simple Wood and Flint and Tinder, WR15 run 4: no tool, LastPrepareResult -1)
    if (!bot->CanNoReagentCast(info))
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (info->Reagent[i] > 0 &&
                !bot->HasItemCount(static_cast<uint32>(info->Reagent[i]), info->ReagentCount[i]))
                return false;
    for (uint32 const tool : info->Totem)
        if (tool && !bot->HasItemCount(tool))
            return false;
    for (uint32 const category : info->TotemCategory)
        if (category && !bot->HasItemTotemCategory(category))
            return false;
    return true;
}

std::optional<StationSpot> FindStationSpot(Player* bot, uint32 focus, char const*& refusal)
{
    refusal = nullptr;
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
    {
        refusal = "the planner's indexes are not ready";
        return std::nullopt;
    }
    std::vector<Planner::Nearby> const nearest = indexes->StationNearest(focus, bot->GetMapId(), bot->GetPositionX(),
                                                                         bot->GetPositionY(), bot->GetPositionZ(), 1);
    if (nearest.empty())
    {
        refusal = "no_spot (no station of the focus on the map)";
        return std::nullopt;
    }
    Planner::Service const& service = indexes->services[nearest.front().index];
    return StationSpot{service.mapId, service.x, service.y, service.z, service.entry, nearest.front().distance};
}

VisitKindRow const& CraftVisitRow() { return CRAFT_VISIT; }

bool BeginCraftVisit(PlayerbotAI* botAI, CraftOrder const& order, StationSpot const* spot)
{
    if (!order.spell || (order.focus && !spot && !order.fire) || !BeginVisit(botAI, VisitKind::Craft, spot))
        return false;

    CraftVisitData& visit = botAI->bankMail.craftData;
    visit = CraftVisitData{};
    visit.spell = order.spell;
    visit.source = order.source;
    visit.wanted = std::max<uint32>(order.casts, 1);
    visit.fire = spot ? 0 : order.fire;
    return true;
}

std::string CraftVisitCounters() { return VisitCounters(VisitKind::Craft); }

std::optional<CraftOrder> RecipeOrder(Player* bot, uint32 spell, uint32 casts, char const*& refusal)
{
    refusal = nullptr;
    std::shared_ptr<Recipes::RecipeIndex const> const index = Planner::CurrentRecipeIndex();
    if (!bot || !index)
    {
        refusal = "the recipe index is not ready";
        return std::nullopt;
    }
    Recipes::Recipe const* recipe = RecipeOf(*index, spell);
    if (!recipe)
    {
        refusal = "the spell is no recipe of the recipe index";
        return std::nullopt;
    }
    if (!bot->HasSpell(spell))
    {
        refusal = "no recipe known (the bot has not learned the spell)";
        return std::nullopt;
    }
    if (!casts)
    {
        CraftRules::RecipeView const view = ViewOf(*recipe);
        casts = CraftRules::CastsAllowed(view, BagsOf(bot, *recipe, view, true));
    }
    return CraftOrder{spell, 0, recipe->focus, std::max<uint32>(casts, 1)};
}

std::optional<CraftOrder> QueuedCraft(PlayerbotAI* botAI, std::size_t from, std::size_t& at, float& queueWorthCopper)
{
    at = 0;
    queueWorthCopper = 0.0f;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    Planner::GoalApply::BotGoals const* goals = bot && botAI->KeepsPossessions() ? botAI->planner.goals.get() : nullptr;
    SkillPath::Path const* path = goals ? goals->path.get() : nullptr;
    std::shared_ptr<Recipes::RecipeIndex const> const index = path ? Planner::CurrentRecipeIndex() : nullptr;
    if (!index)
        return std::nullopt;
    if (goals->gatherRates && goals->gatherRates->exchange > 0.0f)
        queueWorthCopper = static_cast<float>(SkillPath::QueueWorth(*path) / goals->gatherRates->exchange);
    for (std::size_t i = from; i < path->queue.size() && i < CRAFT_STOP_QUEUE; ++i)
    {
        SkillPath::Craft const& craft = path->queue[i];
        // The queued casts, at most what the bags hold now (a processing yield is random, WR14e)
        uint32 casts = std::max<uint32>(craft.casts, 1);
        if (craft.source)
        {
            // Prospecting or milling the held item: the processing cast of `craft process`
            uint32 stacks = 0;
            if (!bot->HasSpell(craft.spell) || !ProcessableItem(bot, craft.spell, craft.source, stacks))
                continue;
            at = i;
            return CraftOrder{craft.spell, craft.source, 0, std::min(casts, stacks)};
        }
        Recipes::Recipe const* recipe = index->RecipeOf(craft.spell);
        // An enchant needs the item it goes on: the visit casts none yet
        if (!recipe || recipe->effect == Recipes::Effect::Enchant || !bot->HasSpell(craft.spell))
            continue;
        for (Recipes::ItemCount const& reagent : recipe->reagents)
            if (reagent.count)
                casts = std::min(casts, bot->GetItemCount(reagent.item, false) / reagent.count);
        if (!casts)
            continue;
        at = i;
        return CraftOrder{craft.spell, 0, recipe->focus, casts};
    }
    return std::nullopt;
}

std::optional<CraftOrder> QueuedStationCraft(PlayerbotAI* botAI, float& queueWorthCopper)
{
    std::size_t at = 0;
    std::optional<CraftOrder> order = QueuedCraft(botAI, 0, at, queueWorthCopper);
    while (order && !order->focus)
        order = QueuedCraft(botAI, at + 1, at, queueWorthCopper);
    return order;
}

bool BeginQueuedCraft(PlayerbotAI* botAI, uint8& tried, float reach)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return false;
    // The stop's visit runs: the stop holds until it ends
    if (VisitRunsOf(botAI->bankMail.visit, VisitKind::Craft))
        return true;

    bool const first = !tried;
    char const* reason = "no_craft";
    while (tried < CRAFT_STOP_QUEUE)
    {
        std::size_t at = 0;
        float worth = 0.0f;
        std::optional<CraftOrder> order = QueuedCraft(botAI, tried, at, worth);
        if (!order)
            break;
        tried = static_cast<uint8>(at + 1);
        std::optional<StationSpot> spot;
        if (order->focus)
        {
            char const* refusal = nullptr;
            spot = FindStationSpot(bot, order->focus, refusal);
            uint32 const fire = CraftRules::OwnStationSpell(order->focus);
            if (CanLightOwnStation(bot, fire) && (!spot || spot->distance > STATION_SEARCH_YARDS))
            {
                spot.reset();  // WR15: no fire this close, the cook lights its own where it stands
                order->fire = fire;
            }
            else if (!spot || spot->distance > reach)
            {
                reason = "no_station";
                continue;
            }
        }
        if (BeginCraftVisit(botAI, *order, spot ? &*spot : nullptr))
            return true;
        reason = "busy";
    }
    // Then what the item rules give to the disenchant outlet (WR14c): one processing visit per item entry, bounded
    tried = static_cast<uint8>(std::max<std::size_t>(tried, CRAFT_STOP_QUEUE));
    while (tried < CRAFT_STOP_QUEUE + STOP_PROCESSES)
    {
        ++tried;
        std::optional<CraftOrder> const order = DisenchantOrder(botAI);
        if (!order)
            break;
        if (BeginCraftVisit(botAI, *order, nullptr))
        {
            NoteDisenchant(botAI, bot, order->source, order->casts);
            return true;
        }
        reason = "busy";
        break;
    }
    tried = static_cast<uint8>(CRAFT_STOP_QUEUE + STOP_PROCESSES);
    // Only the stop's first look writes its row: a later one ends the crafts the stop did
    if (first)
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "craft", reason, 0.0f);
    return false;
}

std::optional<CraftOrder> DisenchantOrder(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !botAI->KeepsPossessions() || !bot->HasSpell(DISENCHANT_SPELL))
        return std::nullopt;
    AiObjectContext* context = botAI->GetAiObjectContext();
    for (Item* item : AI_VALUE2(std::vector<Item*>, "inventory items",
                                "usage " + std::to_string(static_cast<uint32>(ITEM_USAGE_DISENCHANT))))
    {
        uint32 casts = 0;
        if (uint32 const entry = ProcessableItem(bot, DISENCHANT_SPELL, item->GetEntry(), casts))
            return CraftOrder{DISENCHANT_SPELL, entry, 0, std::clamp<uint32>(casts, 1, STOP_PROCESS_CASTS)};
    }
    return std::nullopt;
}

void NoteDisenchant(PlayerbotAI* botAI, Player* bot, uint32 entry, uint32 count)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!botAI || !bot || !proto || !botAI->KeepsPossessions() || !WorldLog::Active())
        return;
    ItemUsageValue::DisenchantOutlet const outlet = ItemUsageValue::DisenchantOutletOf(botAI, proto);
    float const sellNow = static_cast<float>(proto->SellPrice);  // plain copper, as the outlet weighs it (WR14d)
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(Professions::Choice::Disenchant),
        Professions::ReasonWord(outlet.usedByPath ? Professions::Reason::UseSoon : Professions::Reason::Income),
        static_cast<float>(outlet.worthEach), {{"sell_now", sellNow}, {"count", static_cast<float>(count)}}, entry);
}

uint32 ProcessSpellOf(std::string_view word)
{
    for (ProcessSpell const& process : PROCESS_SPELLS)
        if (word == process.word)
            return process.spell;
    return 0;
}

uint32 ProcessableItem(Player* bot, uint32 spell, uint32 entry, uint32& casts)
{
    casts = 0;
    ProcessSpell const* process = ProcessOfSpell(spell);
    std::shared_ptr<Recipes::RecipeIndex const> const index = Planner::CurrentRecipeIndex();
    if (!bot || !process || !index)
        return 0;
    uint32 found = 0;
    uint32 consumed = 0;
    ForEachBagItem(bot,
                   [&](Item* item)
                   {
                       if (entry && item->GetEntry() != entry)
                           return true;
                       uint32 const takes = ConsumedBy(bot, *index, process->kind, item->GetTemplate());
                       if (!takes || item->GetCount() < takes)
                           return true;
                       found = item->GetEntry();
                       consumed = takes;
                       return false;
                   });
    if (found)
        casts = bot->GetItemCount(found, false) / consumed;
    return found;
}
}  // namespace BankMail
