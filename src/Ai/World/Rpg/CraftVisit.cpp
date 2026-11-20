/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftVisit.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "Bag.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "CensusMgr.h"
#include "CheckMountStateAction.h"
#include "CraftRules.h"
#include "GameObject.h"
#include "InventorySlots.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
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
#include "ProfessionConfig.h"
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

std::atomic<uint32> visitsStarted{0};
VisitEndCounters visitsEnded{};

struct ProcessSpell
{
    char const* word;
    uint32 spell;
    Recipes::ProcessKind kind;
};
constexpr ProcessSpell PROCESS_SPELLS[] = {{"disenchant", 13262, Recipes::ProcessKind::Disenchant},
                                           {"prospect", 31252, Recipes::ProcessKind::Prospect},
                                           {"mill", 51005, Recipes::ProcessKind::Mill}};

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

Recipes::Recipe const* RecipeOf(Recipes::RecipeIndex const& index, uint32 spell)
{
    for (Recipes::Recipe const& recipe : index.Recipes())
        if (recipe.spell == spell)
            return &recipe;
    return nullptr;
}

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
void WriteCraftRow(Player* bot, CraftVisitState const& visit, VisitEnd end)
{
    using Professions::Choice;
    using Professions::Reason;
    Reason const reason = visit.made                                              ? Reason::UseSoon
                          : end == VisitEnd::NoMaterials                          ? Reason::NoSource
                          : end == VisitEnd::NoStation || end == VisitEnd::NoSpot ? Reason::NoStation
                                                                                  : Reason::Locked;
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession,
                       Professions::ChoiceWord(visit.made ? Choice::Craft : Choice::Skip),
                       Professions::ReasonWord(reason), static_cast<float>(visit.made),
                       {{"wanted", static_cast<float>(visit.wanted)},
                        {"refused", static_cast<float>(visit.refused)},
                        {"source", static_cast<float>(visit.source)}},
                       visit.spell);
}

void EndVisit(PlayerbotAI* botAI, Player* bot, VisitEnd end)
{
    CraftVisitState& visit = botAI->bankMail.craftVisit;
    char const* how = VisitEndWord(VisitKind::Craft, end);
    // The visit's own cast does not outlive it (death, a teleport and a logout cancel it in the core already)
    if (visit.phase == CraftVisitPhase::Wait && bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    // Bag read: the bot's own map thread inside its AI update, or the world thread between the map updates
    uint32 const freeAfter = WorldTravel::FreeGeneralSlots(bot);
    if (WorldLog::Active())
        WriteCraftRow(bot, visit, end);
    EventLog::Emit(bot, Professions::EVENT_CRAFT_VISIT, how, WorldLogFormat::PackVisitCounts(visit.made, visit.refused),
                   freeAfter);
    LOG_DEBUG("playerbots",
              "[Professions] {} craft visit {}: spell {} source {}, {} of {} made, {} refused, cast result "
              "{}, {} free, {} s",
              bot->GetName(), how, visit.spell, visit.source, visit.made, visit.wanted, visit.refused, visit.lastResult,
              freeAfter, getMSTimeDiff(visit.startedMs, getMSTime()) / IN_MILLISECONDS);

    // A made upgrade is put on now (the one way to use an item); what it replaces stays for the vendor errand
    bool const equip = visit.made > 0 && EquipsAfter(end);
    visit = CraftVisitState{};
    ++visitsEnded[static_cast<size_t>(end)];
    if (equip)
        botAI->DoSpecificAction("equip upgrades packet action");
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
std::optional<VisitEnd> CheckCast(Player* bot, CraftVisitState& visit, Item*& target)
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

// In reach: the next cast checked and started; an end, or nothing while the cast runs or the bot settles
std::optional<VisitEnd> StartCast(PlayerbotAI* botAI, Player* bot, CraftVisitState& visit)
{
    Item* target = nullptr;
    if (std::optional<VisitEnd> const end = CheckCast(bot, visit, target))
        return end;
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
            return std::nullopt;
        LOG_INFO("playerbots",
                 "[Professions] {} craft visit settle bound: mounted {}, moving {}, spline done {}, motion {}",
                 bot->GetName(), bot->IsMounted(), bot->isMoving(), bot->movespline->Finalized(),
                 static_cast<uint32>(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()));
    }

    // The real spell, as a player casts it; a nocast test bot starts none (TestConstraints::CastsOnItsOwn)
    if (!botAI->CastSpell(visit.spell, bot, target))
    {
        ++visit.refused;
        visit.lastResult = botAI->LastPrepareResult();
        return EndOfCastResult(visit.lastResult);
    }
    visit.phase = CraftVisitPhase::Wait;
    visit.castMs = getMSTime();
    visit.castBoundMs = StepBoundMs(visit.spell);
    return std::nullopt;
}

// The awaited cast's result from the bags: made (the product came, or the reagent or the processed item went) or
// not; nullopt while it waits
std::optional<bool> CastResult(Player* bot, CraftVisitState const& visit)
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
}  // namespace

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

bool BeginCraftVisit(PlayerbotAI* botAI, CraftOrder const& order, StationSpot const* spot)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !order.spell || (order.focus && !spot) || VisitStartRefusal(botAI->bankMail, VisitKind::Craft))
        return false;

    CraftVisitState& visit = botAI->bankMail.craftVisit;
    visit = CraftVisitState{};
    visit.phase = spot ? CraftVisitPhase::Walk : CraftVisitPhase::Cast;
    botAI->AddWork(BotWork::Visit);
    visit.mapId = spot ? spot->mapId : bot->GetMapId();
    visit.x = spot ? spot->x : bot->GetPositionX();
    visit.y = spot ? spot->y : bot->GetPositionY();
    visit.z = spot ? spot->z : bot->GetPositionZ();
    visit.focus = order.focus;
    visit.stationEntry = spot ? spot->entry : 0;
    visit.spell = order.spell;
    visit.source = order.source;
    visit.wanted = std::max<uint32>(order.casts, 1);
    visit.startedMs = getMSTime();
    visit.limitMs = sPlayerbotAIConfig.Professions.CraftVisitDeadlineSeconds * IN_MILLISECONDS;
    ++visitsStarted;
    EventLog::Emit(bot, WorldLogFormat::EVENT_VISIT_START, "craft", visit.mapId,
                   static_cast<int64>(spot ? spot->distance : 0.0f));
    return true;
}

CraftVisitStep StepCraftVisit(PlayerbotAI* botAI)
{
    CraftVisitStep step;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || botAI->bankMail.craftVisit.phase == CraftVisitPhase::None)
        return step;

    CraftVisitState& visit = botAI->bankMail.craftVisit;
    step.result = CraftVisitResult::Ended;
    if (std::optional<VisitEnd> const end = WatchVisit(WatchInOf(bot, visit.mapId, visit.startedMs, visit.limitMs)))
    {
        EndVisit(botAI, bot, *end);
        return step;
    }

    if (visit.phase == CraftVisitPhase::Wait)
    {
        std::optional<bool> const made = CastResult(bot, visit);
        if (!made)
        {
            if (getMSTimeDiff(visit.castMs, getMSTime()) >= visit.castBoundMs)
            {
                EndVisit(botAI, bot, VisitEnd::Timeout);
                return step;
            }
            step.result = CraftVisitResult::Pending;
            return step;
        }
        visit.phase = CraftVisitPhase::Cast;
        visit.castBoundMs = 0;
        if (!*made)
        {
            // Ended without its product: the rules name the cause now, or the visit is done (never cast again blind)
            ++visit.refused;
            Item* target = nullptr;
            EndVisit(botAI, bot, CheckCast(bot, visit, target).value_or(VisitEnd::Done));
            return step;
        }
        ++visit.made;
    }

    if (visit.focus)
    {
        if (visit.station.IsEmpty())
        {
            if (bot->GetExactDist(visit.x, visit.y, visit.z) > STATION_SEARCH_YARDS)
            {
                visit.phase = CraftVisitPhase::Walk;
                step.result = CraftVisitResult::Walk;
                step.mapId = visit.mapId;
                step.x = visit.x;
                step.y = visit.y;
                step.z = visit.z;
                return step;
            }
            // Once per visit: a grid search for the indexed station's template near its spawn
            GameObject const* found = bot->FindNearestGameObject(visit.stationEntry, STATION_SEARCH_YARDS, true);
            if (!found)
            {
                EndVisit(botAI, bot, VisitEnd::NoStation);
                return step;
            }
            visit.station = found->GetGUID();
        }
        // The core's own test (Spell::CheckSpellFocus): spawned, and the caster within the object's own range
        GameObject const* station = bot->GetMap()->GetGameObject(visit.station);
        if (!station || !station->isSpawned())
        {
            EndVisit(botAI, bot, VisitEnd::NoStation);
            return step;
        }
        float const range = static_cast<float>(station->GetGOInfo()->spellFocus.dist);
        if (!station->IsWithinDistInMap(bot, range))
        {
            visit.phase = CraftVisitPhase::Walk;
            step.result = CraftVisitResult::Walk;
            step.station = visit.station;
            step.range = range;
            return step;
        }
    }

    visit.phase = CraftVisitPhase::Cast;
    if (visit.made >= visit.wanted)
    {
        EndVisit(botAI, bot, VisitEnd::Done);
        return step;
    }
    if (std::optional<VisitEnd> const end = StartCast(botAI, bot, visit))
    {
        EndVisit(botAI, bot, *end);
        return step;
    }
    step.result = CraftVisitResult::Pending;
    return step;
}

void EndCraftVisit(PlayerbotAI* botAI, VisitEnd end)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (bot && botAI->bankMail.craftVisit.phase != CraftVisitPhase::None)
        EndVisit(botAI, bot, end);
}

std::string CraftVisitCounters() { return DescribeVisitCounters(VisitKind::Craft, visitsStarted.load(), visitsEnded); }

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
