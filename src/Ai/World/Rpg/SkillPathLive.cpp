/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SkillPathLive.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "AuctionIndex.h"
#include "AuctionReference.h"
#include "BotProfessionMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "GatherLive.h"
#include "GoalInterface.h"
#include "ItemDemand.h"
#include "ItemUsageValue.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ProfessionConfig.h"
#include "RandomPlayerbotMgr.h"
#include "SkillPathBridge.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldSession.h"

namespace Planner
{
namespace
{
constexpr std::uint8_t TEAM_ALLIANCE_SNAPSHOT = 0;  // Snapshot::team

std::vector<SkillPathBridge::ZoneBracket> BracketsOf(PlannerIndexes const* indexes, std::uint8_t team)
{
    std::vector<SkillPathBridge::ZoneBracket> zones;
    if (!indexes)
        return zones;
    zones.reserve(indexes->zones.size());
    for (ZoneRecord const& zone : indexes->zones)
    {
        bool const alliance = team == TEAM_ALLIANCE_SNAPSHOT;
        bool const open = zone.side == ZoneSide::Contested || zone.side == ZoneSide::Sanctuary ||
                          (zone.side == ZoneSide::Alliance) == alliance;
        zones.push_back({zone.zoneId, zone.low, zone.high, zone.mapId, open});
    }
    return zones;
}

// The totem categories the held tools cover: the core's test without its skill check (Player::
// IsTotemCategoryCompatiableWith). Item templates and the store are static data, read only
std::vector<std::uint32_t> CategoriesOf(std::vector<std::uint32_t> const& tools)
{
    std::vector<std::uint32_t> categories;
    for (std::uint32_t const tool : tools)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tool);
        TotemCategoryEntry const* held =
            proto && proto->TotemCategory ? sTotemCategoryStore.LookupEntry(proto->TotemCategory) : nullptr;
        if (!held)
            continue;
        for (std::uint32_t id = 0; id < sTotemCategoryStore.GetNumRows(); ++id)
            if (TotemCategoryEntry const* entry = sTotemCategoryStore.LookupEntry(id))
                if (entry->categoryType == held->categoryType &&
                    (held->categoryMask & entry->categoryMask) == entry->categoryMask)
                    categories.push_back(id);
    }
    std::sort(categories.begin(), categories.end());
    categories.erase(std::unique(categories.begin(), categories.end()), categories.end());
    return categories;
}

// What one unit fetches at a vendor: the item template's sell price (static data)
double SellCopper(std::uint32_t item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return proto ? static_cast<double>(proto->SellPrice) : 0.0;
}

// The tools the recipes ask for, at the item templates' prices (static data, built once)
std::vector<SkillPath::ToolSource> const& ToolSources()
{
    static std::vector<SkillPath::ToolSource> const sources = SkillPathBridge::ToolSourcesOf(
        [](std::uint32_t item)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            return proto ? static_cast<std::uint32_t>(std::max<int32>(proto->BuyPrice, 0)) : 0u;
        });
    return sources;
}

// The decision row's reason for a profession taken: the largest part of what its stretch brings
Professions::Reason BringsReason(SkillPath::Brings const& brings)
{
    Professions::Reason reason = Professions::Reason::UseSoon;
    double best = brings.useSoon;
    for (auto const& [part, value] :
         {std::pair{Professions::Reason::HeldRecipe, brings.recipesHeld},
          std::pair{Professions::Reason::Access, brings.access}, std::pair{Professions::Reason::Income, brings.income}})
        if (value > best)
        {
            best = value;
            reason = part;
        }
    return reason;
}

std::string SkillName(std::uint16_t skill)
{
    std::string_view const name = ProfessionRules::Name(static_cast<ProfessionRules::Skill>(skill));
    return name.empty() ? std::to_string(skill) : std::string(name);
}

char const* StepWord(SkillPath::StepKind kind)
{
    switch (kind)
    {
        case SkillPath::StepKind::Learn:
            return "learn";
        case SkillPath::StepKind::Rank:
            return "rank";
        case SkillPath::StepKind::Gather:
            return "gather";
        case SkillPath::StepKind::Skin:
            return "skin";
        case SkillPath::StepKind::Fish:
            return "fish";
        case SkillPath::StepKind::Tool:
            return "tool";
        default:
            return "craft";
    }
}

std::string StretchText(SkillPath::Stretch const& stretch)
{
    return Acore::StringFormat(
        "{} {}->{} ({} {}): worth {:.1f} = brings {:.1f} (use {:.1f}, recipes {:.1f}, access {:.1f}, income {:.1f}) "
        "- costs {:.1f} (materials {:.1f}, time {:.1f}, fees {:.1f}); {:.0f} c, {:.1f} min, {:.2f}/min, {}{}",
        SkillName(stretch.skill), stretch.from, stretch.to, SkillPath::MilestoneWord(stretch.milestone),
        stretch.milestoneId, stretch.netWorth, stretch.brings.Total(), stretch.brings.useSoon,
        stretch.brings.recipesHeld, stretch.brings.access, stretch.brings.income, stretch.cost.Total(),
        stretch.cost.materials, stretch.cost.time, stretch.cost.fees, stretch.copper, stretch.minutes,
        stretch.perMinute, SkillPath::ReasonWord(stretch.reason),
        stretch.blockedAt ? Acore::StringFormat(" at {}", stretch.blockedAt) : std::string());
}

std::string SecondText(SkillPath::SecondChoice const& second)
{
    if (!second.craft)
        return "not due";
    return Acore::StringFormat(
        "{} for {} (craft {} worth {:.1f}, second gatherer {} worth {:.1f}, {} c above the "
        "reserve)",
        second.take ? "learn " + SkillName(second.take) : std::string("wait"), SkillPath::ReasonWord(second.reason),
        SkillName(second.craft), second.craftWorth, second.gatherer ? SkillName(second.gatherer) : std::string("none"),
        second.gathererWorth, second.money);
}

constexpr std::size_t PATH_STEPS_SHOWN = 8;  // the chosen stretch's first steps the command prints

uint64 MicrosSince(std::chrono::steady_clock::time_point started)
{
    return static_cast<uint64>(std::max<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count(), 0));
}

void CountRow(std::string_view name, uint64 value)
{
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, name))
        row->Record(value);
}

// WPC14a: the inputs a path computation reads, as their PerfMonitor rows name them
enum PathInput : std::uint8_t
{
    INPUT_LEVEL,
    INPUT_PERSON,
    INPUT_SKILLS,
    INPUT_HELD_PATH,   // the units held of the last path's materials
    INPUT_HELD_OTHER,  // the units held of the other materials
    INPUT_KNOWN,
    INPUT_TOOLS,
    INPUT_SPENDABLE,
    INPUT_RATES,
    INPUT_SHORT,
    INPUT_GATHERER_LEVEL,
    INPUT_USES,
    INPUT_CONSUMABLES,
    INPUT_ANY_STRETCH,
    INPUT_RECIPE_INDEX,
    INPUT_INDEXES,
    INPUT_MARKET_PATH,   // a price of one of the last path's materials
    INPUT_MARKET_OTHER,  // a price of another item
    INPUT_GATHER_RATES,
    INPUT_COUNT
};

constexpr std::array<char const*, INPUT_COUNT> INPUT_NAMES{
    "level",        "person",  "skills",      "held path",      "held other",  "known recipes", "tools",
    "spendable",    "rates",   "short",       "gatherer level", "uses",        "consumables",   "anypath",
    "recipe index", "indexes", "market path", "market other",   "gather rates"};

template <class T>
bool SameObject(std::weak_ptr<T> const& a, std::weak_ptr<T> const& b)
{
    return !a.owner_before(b) && !b.owner_before(a);
}

bool SameEntry(AuctionIndex::Snapshot const* a, AuctionIndex::Snapshot const* b, std::uint32_t item)
{
    AuctionIndex::ItemEntry const* x = a ? a->Find(item) : nullptr;
    AuctionIndex::ItemEntry const* y = b ? b->Find(item) : nullptr;
    if (!x || !y)
        return !x && !y;
    AuctionIndex::TypicalPrice const& p = x->typical;
    AuctionIndex::TypicalPrice const& q = y->typical;
    AuctionIndex::Record const kindA = a->KindRecord(x->kind);
    AuctionIndex::Record const kindB = b->KindRecord(y->kind);
    return p.unitPrice == q.unitPrice && p.sample == q.sample && p.weight == q.weight && p.expiries == q.expiries &&
           p.basis == q.basis && x->record.sales == y->record.sales && x->record.hours == y->record.hours &&
           x->kind == y->kind && kindA.sales == kindB.sales && kindA.hours == kindB.hours;
}

bool SameGatherRates(GatherRate::View const& a, GatherRate::View const& b)
{
    if (a.snapshot == b.snapshot)
        return true;
    CountRow("profession path gather snapshot changed", 0);
    static GatherRate::Tallies const none;
    GatherRate::Tallies const& x = a.snapshot ? a.snapshot->tallies : none;
    GatherRate::Tallies const& y = b.snapshot ? b.snapshot->tallies : none;
    GatherRate::Fault const faultA = a.snapshot ? a.snapshot->fault : GatherRate::Fault::None;
    GatherRate::Fault const faultB = b.snapshot ? b.snapshot->fault : GatherRate::Fault::None;
    if (faultA != faultB || x.size() != y.size())
        return false;
    for (auto const& [key, tally] : x)
    {
        auto const found = y.find(key);
        if (found == y.end() || found->second.minutes != tally.minutes || found->second.takes != tally.takes)
            return false;
    }
    return true;
}

bool SameUses(std::vector<SkillPath::UseWant> const& a, std::vector<SkillPath::UseWant> const& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](SkillPath::UseWant const& x, SkillPath::UseWant const& y)
                      {
                          return x.item == y.item && x.spell == y.spell && x.worthCopperEach == y.worthCopperEach &&
                                 x.level == y.level;
                      });
}

bool SameConsumables(std::vector<SkillPath::ConsumableWant> const& a, std::vector<SkillPath::ConsumableWant> const& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](SkillPath::ConsumableWant const& x, SkillPath::ConsumableWant const& y)
                      {
                          return x.item == y.item && x.wantedStock == y.wantedStock && x.held == y.held &&
                                 x.worthCopperEach == y.worthCopperEach && x.group == y.group && x.food == y.food;
                      });
}

bool SameSteps(std::vector<SkillPath::PathStep> const& a, std::vector<SkillPath::PathStep> const& b)
{
    return std::equal(
        a.begin(), a.end(), b.begin(), b.end(), [](SkillPath::PathStep const& x, SkillPath::PathStep const& y)
        { return x.kind == y.kind && x.id == y.id && x.from == y.from && x.to == y.to && x.zone == y.zone; });
}

// The decision a path's readers act on: the chosen stretch and its steps, the buys, the wants, the queue, the
// materials' units and sources, the second choice
bool SameDecision(SkillPath::Path const& a, SkillPath::Path const& b)
{
    if (a.reason != b.reason || a.chosen != b.chosen || a.foodUnits != b.foodUnits ||
        a.second.craft != b.second.craft || a.second.gatherer != b.second.gatherer || a.second.take != b.second.take ||
        a.second.reason != b.second.reason)
        return false;
    if (a.chosen != SkillPath::NONE)
    {
        if (a.chosen >= static_cast<int>(a.weighed.size()) || b.chosen >= static_cast<int>(b.weighed.size()))
            return false;
        SkillPath::Stretch const& x = a.weighed[a.chosen];
        SkillPath::Stretch const& y = b.weighed[b.chosen];
        if (x.skill != y.skill || x.from != y.from || x.to != y.to || x.milestone != y.milestone ||
            x.milestoneId != y.milestoneId || x.reason != y.reason || x.blockedAt != y.blockedAt ||
            !SameSteps(x.steps, y.steps))
            return false;
    }
    return std::equal(a.needs.begin(), a.needs.end(), b.needs.begin(), b.needs.end(),
                      [](SkillPath::Purchase const& x, SkillPath::Purchase const& y)
                      { return x.kind == y.kind && x.spell == y.spell && x.item == y.item; }) &&
           std::equal(a.wants.begin(), a.wants.end(), b.wants.begin(), b.wants.end(),
                      [](SkillPath::Want const& x, SkillPath::Want const& y)
                      { return x.item == y.item && x.count == y.count; }) &&
           std::equal(a.queue.begin(), a.queue.end(), b.queue.begin(), b.queue.end(),
                      [](SkillPath::Craft const& x, SkillPath::Craft const& y)
                      {
                          return x.spell == y.spell && x.casts == y.casts && x.forSkill == y.forSkill &&
                                 x.source == y.source && x.forSale == y.forSale;
                      }) &&
           std::equal(a.materials.begin(), a.materials.end(), b.materials.begin(), b.materials.end(),
                      [](SkillPath::Material const& x, SkillPath::Material const& y)
                      {
                          return x.item == y.item && x.used == y.used && x.wanted == y.wanted && x.skill == y.skill &&
                                 x.dueSkill == y.dueSkill && x.source == y.source && x.via == y.via;
                      });
}

// The prices beside an equal decision (SameDecision first: the lists have the same lengths)
bool SamePrices(SkillPath::Path const& a, SkillPath::Path const& b)
{
    if (a.useWorth != b.useWorth || a.second.craftWorth != b.second.craftWorth ||
        a.second.gathererWorth != b.second.gathererWorth || a.second.money != b.second.money)
        return false;
    if (a.chosen != SkillPath::NONE)
    {
        SkillPath::Stretch const& x = a.weighed[a.chosen];
        SkillPath::Stretch const& y = b.weighed[b.chosen];
        if (x.copper != y.copper || x.minutes != y.minutes || x.netWorth != y.netWorth || x.perMinute != y.perMinute)
            return false;
        for (std::size_t step = 0; step < x.steps.size(); ++step)
            if (x.steps[step].casts != y.steps[step].casts || x.steps[step].copper != y.steps[step].copper ||
                x.steps[step].minutes != y.steps[step].minutes || x.steps[step].value != y.steps[step].value)
                return false;
    }
    for (std::size_t at = 0; at < a.needs.size(); ++at)
        if (a.needs[at].copper != b.needs[at].copper)
            return false;
    for (std::size_t at = 0; at < a.wants.size(); ++at)
        if (a.wants[at].worthEach != b.wants[at].worthEach)
            return false;
    for (std::size_t at = 0; at < a.materials.size(); ++at)
        if (a.materials[at].copperEach != b.materials[at].copperEach ||
            a.materials[at].otherCopperEach != b.materials[at].otherCopperEach)
            return false;
    return true;
}

void FillRecord(PathRecord& record, Snapshot const& snapshot, SkillPathBridge::Computed const& computed,
                UpkeepNeeds::NeedRates const& rates, SkillPath::PathInputs const& inputs, PathData const& data,
                std::shared_ptr<AuctionIndex::Snapshot const> market, std::shared_ptr<SkillPath::Path const> path)
{
    record.path = std::move(path);
    record.level = snapshot.level;
    if (snapshot.person)
        record.mains = {static_cast<std::uint16_t>(snapshot.person->Mains[0]),
                        static_cast<std::uint16_t>(snapshot.person->Mains[1])};
    for (Planner::Skill const& skill : snapshot.skills)
        if (skill.value > 0 && SkillPathBridge::IsProfession(skill.id))
            record.skills.push_back({skill.id, skill.value, skill.max});
    record.held.reserve(snapshot.materials.size());
    for (Planner::MaterialHeld const& held : snapshot.materials)
        record.held.push_back({held.item, held.bags + held.bank, 0});
    std::sort(record.held.begin(), record.held.end(),
              [](PathRecord::Level const& a, PathRecord::Level const& b) { return a.id < b.id; });
    record.knownRecipes = snapshot.knownRecipes;
    record.toolsHeld = snapshot.toolsHeld;
    record.money = snapshot.money;
    record.reserve = computed.reserve;
    record.spendable = snapshot.money > computed.reserve ? snapshot.money - computed.reserve : 0;
    Materials::Rates const derived = SkillPathBridge::RatesOf(snapshot, computed.reserve);
    record.rates = {derived.exchange, derived.factor, derived.minuteValue, derived.cautionWeight};
    record.shortOfGold = computed.shortOfGold;
    record.gathererLevel = rates.gathererLevel;
    record.inputs = inputs;
    record.recipes = data.recipes;
    record.indexes = data.indexes;
    record.market = std::move(market);
    record.gatherRates = data.gatherRates;
}
}  // namespace

Market::View CountedMarketView(char const* row)
{
    auto const started = std::chrono::steady_clock::now();
    Market::View market = MarketView();
    CountRow(row, MicrosSince(started));
    return market;
}

Market::View MarketView()
{
    return {AuctionIndex::Current(), AuctionReference::HouseFees(AuctionRules::HOUSE_NEUTRAL),
            sPlayerbotAIConfig.Auction, [](std::uint32_t item)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                return proto ? proto->GetMaxStackSize() : 1u;
            }};
}

std::uint32_t MarketProceeds(Market::View const& market, std::uint32_t item)
{
    if (market.Empty())
        return 0;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return static_cast<std::uint32_t>(market.ProceedsOf(item, proto ? proto->SellPrice : 0u));
}

PathData PathDataNow()
{
    PathData data;
    if (RecipeIndexReady())
        data.recipes = CurrentRecipeIndex();
    if (IndexesReady())
        data.indexes = Indexes();
    data.gatherRates = GatherRatesNow();
    return data;
}

namespace
{
// Professions WR12: the products of the bot's professions it would wear as upgrades are uses for the path. The most
// demanding ones the bot's level allows are asked (the upgrade check builds an item), within the skill the path's
// nearness reaches
constexpr std::size_t UPGRADE_CANDIDATES = 24;
constexpr std::uint32_t UPGRADE_SKILL_REACH = 75;

struct UpgradeCandidate
{
    std::uint32_t level{0};
    std::uint32_t item{0};
};

std::vector<SkillPath::UseWant> UpgradeUsesOf(PlayerbotAI* botAI)
{
    std::vector<SkillPath::UseWant> uses;
    if (!botAI->KeepsPossessions() || !RecipeIndexReady())
        return uses;
    std::shared_ptr<Recipes::RecipeIndex const> const index = CurrentRecipeIndex();
    if (!index)
        return uses;

    Player* bot = botAI->GetBot();
    std::uint32_t const level = bot->GetLevel();
    std::vector<UpgradeCandidate> candidates;
    for (Recipes::Recipe const& recipe : index->Recipes())
    {
        if (recipe.effect != Recipes::Effect::Make || !recipe.product)
            continue;
        std::uint32_t const skill = bot->GetSkillValue(recipe.skill);
        if (!skill || recipe.minRank > skill + UPGRADE_SKILL_REACH)
            continue;
        Recipes::ItemFact const* fact = index->Item(recipe.product);
        if (!fact || (fact->itemClass != ITEM_CLASS_ARMOR && fact->itemClass != ITEM_CLASS_WEAPON) ||
            fact->requiredLevel > level)
            continue;
        candidates.push_back({fact->requiredLevel, recipe.product});
    }
    std::sort(candidates.begin(), candidates.end(), [](UpgradeCandidate const& a, UpgradeCandidate const& b)
              { return a.level != b.level ? a.level > b.level : a.item < b.item; });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](UpgradeCandidate const& a, UpgradeCandidate const& b) { return a.item == b.item; }),
                     candidates.end());
    if (candidates.size() > UPGRADE_CANDIDATES)
        candidates.resize(UPGRADE_CANDIDATES);

    float const sellMultiplier = static_cast<float>(sRandomPlayerbotMgr.GetSellMultiplier(bot));
    for (UpgradeCandidate const& candidate : candidates)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(candidate.item);
        if (!proto)
            continue;
        ItemUpgradeValue check(botAI);  // the upgrade check as it is, for an item the bot does not hold
        check.Qualify(std::to_string(candidate.item));
        ItemUsage const usage = check.Calculate();
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            continue;
        uses.push_back({candidate.item, 0,
                        static_cast<float>(ItemUsageValue::ItemWorth(proto, ITEM_USAGE_USE, 1, sellMultiplier)),
                        proto->RequiredLevel});
    }
    return uses;
}
}  // namespace

SkillPath::PathInputs PathInputsOf(PlayerbotAI* botAI)
{
    SkillPath::PathInputs inputs;
    if (!botAI)
        return inputs;
    inputs.anyStretch = TestConstraints::TakesAnyPath(botAI->testConstraints);
    if (ItemUse::DemandAnswers(botAI))
        inputs.consumables = SkillPathBridge::ConsumablesOf(ItemUse::DemandOf(botAI));
    inputs.uses = UpgradeUsesOf(botAI);
    return inputs;
}

SkillPath::Path const* KeptPath(PlayerbotAI* botAI)
{
    GoalApply::BotGoals const* goals = botAI ? botAI->planner.goals.get() : nullptr;
    return goals ? goals->path.get() : nullptr;
}

std::shared_ptr<SkillPath::Path const> NeedsWithPath(Snapshot const& snapshot, UpkeepIndex const& upkeep,
                                                     UpkeepNeeds::NeedRates const& rates,
                                                     SkillPath::PathInputs const& inputs, PathData const& data,
                                                     UpkeepNeeds::NeedList& needs, PathRecord* record)
{
    if (!data.recipes || !snapshot.person)
    {
        needs = UpkeepNeeds::ComputeNeeds(snapshot, upkeep, SkillPathBridge::WithoutPath(rates));
        return nullptr;
    }
    auto const started = std::chrono::steady_clock::now();
    // The creatures a skinner meets (WR20a), from the drop rows; read only for skinning: a skinner's stretches, or the
    // second gatherer a bot short of gold may take where the market buys skins (SkillPathBridge::MarketPays)
    Market::View market = MarketView();
    std::vector<SkillPath::Skinnable> skinnables;
    bool const skins =
        !market.Empty() ||
        std::any_of(
            snapshot.skills.begin(), snapshot.skills.end(), [](Planner::Skill const& skill)
            { return skill.value > 0 && skill.id == static_cast<std::uint16_t>(ProfessionRules::Skill::Skinning); });
    if (skins)
    {
        Materials::Bot killer;
        killer.level = snapshot.level;
        killer.side = Materials::SideBit(snapshot.team);
        skinnables = SkillPath::SkinnablesOf(
            *data.recipes, killer, [&market](std::uint32_t item) { return market.SaleOf(item, SellCopper(item)); });
    }
    uint64 const skinnablesMicros = MicrosSince(started);
    SkillPath::PathWorld const world{*data.recipes,
                                     [](std::uint32_t spell) { return UpkeepNeeds::MayLearn(spell); },
                                     SellCopper,
                                     std::move(skinnables),
                                     {},
                                     SkillPath::Fault::None,
                                     ToolSources(),
                                     std::move(market),
                                     data.gatherRates};
    SkillPathBridge::Computed computed =
        SkillPathBridge::Compute(snapshot, upkeep, rates, inputs, world, BracketsOf(data.indexes.get(), snapshot.team),
                                 CategoriesOf(snapshot.toolsHeld));
    needs = std::move(computed.needs);
    auto path = std::make_shared<SkillPath::Path const>(std::move(computed.path));
    CountRow("profession path computed", MicrosSince(started));
    // WPC14a: the parts (the skinnables with the market view, both needs lists, the weigh, the second choice's weigh)
    CountRow("profession path computed: skinnables", skinnablesMicros);
    CountRow("profession path computed: needs", computed.needsMicros);
    CountRow("profession path computed: weigh", computed.weighMicros);
    if (path->second.craft)
        CountRow("profession path computed: second weigh", computed.secondMicros);
    if (record)
        FillRecord(*record, snapshot, computed, rates, inputs, data, world.market.snapshot, path);
    return path;
}

void CountPathChanges(PathRecord const* last, PathRecord const& now)
{
    auto const started = std::chrono::steady_clock::now();
    CountRow("profession path market items", now.market ? now.market->items.size() : 0);
    CountRow("profession path gather tallies", now.gatherRates.snapshot ? now.gatherRates.snapshot->tallies.size() : 0);
    if (!last || !last->path || !now.path)
    {
        CountRow("profession path first", 0);
        return;
    }
    SkillPath::Path const& kept = *last->path;
    std::uint32_t changed = 0;
    auto const mark = [&changed](PathInput input, bool differs)
    {
        if (differs)
            changed |= 1u << input;
    };
    mark(INPUT_LEVEL, last->level != now.level);
    mark(INPUT_PERSON, last->mains != now.mains);
    mark(INPUT_SKILLS, last->skills != now.skills);
    // The held units, both lists sorted by item: an item missing from one holds 0 there
    for (auto a = last->held.begin(), b = now.held.begin(); a != last->held.end() || b != now.held.end();)
    {
        bool const takeA = b == now.held.end() || (a != last->held.end() && a->id <= b->id);
        bool const takeB = a == last->held.end() || (b != now.held.end() && b->id <= a->id);
        std::uint32_t const item = takeA ? a->id : b->id;
        if ((takeA ? a->value : 0) != (takeB ? b->value : 0))
            mark(SkillPath::MaterialOf(kept, item) ? INPUT_HELD_PATH : INPUT_HELD_OTHER, true);
        if (takeA)
            ++a;
        if (takeB)
            ++b;
    }
    mark(INPUT_KNOWN, last->knownRecipes != now.knownRecipes);
    mark(INPUT_TOOLS, last->toolsHeld != now.toolsHeld);
    mark(INPUT_SPENDABLE, last->spendable != now.spendable);
    mark(INPUT_RATES, last->rates != now.rates);
    mark(INPUT_SHORT, last->shortOfGold != now.shortOfGold);
    mark(INPUT_GATHERER_LEVEL, last->gathererLevel != now.gathererLevel);
    mark(INPUT_USES, !SameUses(last->inputs.uses, now.inputs.uses));
    mark(INPUT_CONSUMABLES, !SameConsumables(last->inputs.consumables, now.inputs.consumables));
    mark(INPUT_ANY_STRETCH, last->inputs.anyStretch != now.inputs.anyStretch);
    mark(INPUT_RECIPE_INDEX, !SameObject(last->recipes, now.recipes));
    mark(INPUT_INDEXES, !SameObject(last->indexes, now.indexes));
    if (last->market != now.market)
    {
        CountRow("profession path market snapshot changed", 0);
        AuctionIndex::Snapshot const* a = last->market.get();
        AuctionIndex::Snapshot const* b = now.market.get();
        auto const check = [&](std::uint32_t item)
        {
            if (!SameEntry(a, b, item))
                mark(SkillPath::MaterialOf(kept, item) ? INPUT_MARKET_PATH : INPUT_MARKET_OTHER, true);
        };
        if (a)
            for (auto const& [item, entry] : a->items)
                check(item);
        if (b)
            for (auto const& [item, entry] : b->items)
                if (!a || !a->Find(item))
                    check(item);
    }
    mark(INPUT_GATHER_RATES, !SameGatherRates(last->gatherRates, now.gatherRates));
    if (last->money != now.money)
        CountRow("profession path money changed", 0);
    if (last->reserve != now.reserve)
        CountRow("profession path reserve changed", 0);

    bool const same = SameDecision(kept, *now.path);
    bool const prices = same && SamePrices(kept, *now.path);
    CountRow(same ? "profession path same result" : "profession path differs", 0);
    if (prices)
        CountRow("profession path same result and prices", 0);
    if (!changed && !same)
        CountRow("profession path differs, no input changed", 0);
    if (!changed && same && !prices)
        CountRow("profession path prices differ, no input changed", 0);
    std::string names;
    for (std::size_t input = 0; input < INPUT_COUNT; ++input)
        if (changed & (1u << input))
        {
            CountRow(std::string("profession path input changed: ") + INPUT_NAMES[input], 0);
            names += names.empty() ? "" : ", ";
            names += INPUT_NAMES[input];
        }
    std::string const verdict = !same    ? "profession path inputs (differs): "
                                : prices ? "profession path inputs (same): "
                                         : "profession path inputs (prices differ): ";
    CountRow(verdict + (names.empty() ? std::string("none") : names), 0);
    CountRow("profession path record compare", MicrosSince(started));
}

void AddKeptPath(PlayerbotAI* botAI, UpkeepNeeds::NeedRates& rates)
{
    GoalApply::BotGoals const* goals = botAI ? botAI->planner.goals.get() : nullptr;
    if (goals && goals->path)
        rates = SkillPathBridge::WithPath(rates, *goals->path);
}

void TakeSecondChoice(PlayerbotAI* botAI, Player* bot, SkillPath::Path const* before, SkillPath::Path const& path)
{
    SkillPath::SecondChoice const& second = path.second;
    if (!second.craft || !botAI || !botAI->KeepsPossessions())
        return;
    bool const gatherer = second.take && second.take != second.craft;
    if (gatherer && !BotProfessionMgr::instance().ChooseSecond(bot, static_cast<ProfessionRules::Skill>(second.craft),
                                                               static_cast<ProfessionRules::Skill>(second.take)))
        return;
    SkillPath::SecondChoice const* last = before ? &before->second : nullptr;
    if (last && last->craft == second.craft && last->take == second.take && last->reason == second.reason)
        return;  // the plan before chose the same
    if (!WorldLog::Active())
        return;
    using Professions::Choice;
    using Professions::Reason;
    Reason const reason = gatherer                                        ? Reason::GoldShort
                          : second.take                                   ? BringsReason(second.brings)
                          : second.reason == SkillPath::Reason::GoldShort ? Reason::GoldShort
                                                                          : Reason::NotWorthIt;
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession,
                       Professions::ChoiceWord(second.take ? Choice::Learn : Choice::Skip),
                       Professions::ReasonWord(reason),
                       static_cast<float>(gatherer ? second.gathererWorth : second.craftWorth),
                       {{"craft", static_cast<float>(second.craftWorth)},
                        {"gatherer", static_cast<float>(second.gathererWorth)},
                        {"gold", static_cast<float>(second.money)}},
                       second.take ? second.take : second.craft);
}

bool HandlePathCommand(ChatHandler* handler, std::string name, std::string const& verb)
{
    bool const stock = verb == "stock";
    WorldSession* const session = handler->GetSession();
    if ((!verb.empty() && !stock) || (stock && session && session->GetSecurity() < SEC_GAMEMASTER))
    {
        handler->SendErrorMessage("Usage: playerbots professions path <bot> [stock] (stock: a game master's test)");
        return false;
    }
    Player* bot = !name.empty() && normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("Usage: playerbots professions path <bot> (an online bot; {} is none)", name);
        return false;
    }
    PathData const data = PathDataNow();
    std::shared_ptr<UpkeepIndex const> const upkeep = UpkeepIndexReady() ? CurrentUpkeepIndex() : nullptr;
    if (!upkeep || !data.recipes)
    {
        handler->SendErrorMessage("Skill path: the upkeep or recipe index is not ready");
        return false;
    }
    // Commands run on the world thread outside the map updates: no map thread touches the bot (as `planner needs`)
    PLAYERBOT_CHECK_WORLD_THREAD("professions path");
    Snapshot const snapshot = FillSnapshot(bot, 0);
    if (!snapshot.person)
    {
        handler->SendErrorMessage("Skill path: {} has no person (not a persistent bot)", bot->GetName());
        return false;
    }
    UpkeepNeeds::NeedList needs;
    std::shared_ptr<SkillPath::Path const> path =
        NeedsWithPath(snapshot, *upkeep, BotNeedRates(botAI, snapshot), PathInputsOf(botAI), data, needs);
    if (!path)
    {
        handler->SendErrorMessage("Skill path: none computed for {}", bot->GetName());
        return false;
    }
    if (stock)
    {
        // The fixture: the wants and the tools the path buys from vendors into the bags (the core's AddItem, no source
        // row), then the path of the bags with them, kept as the newest plan's so a bot whose planner a test holds has
        // its queue now (a held planner computes no path after a bought tool)
        GoalApply::BotGoals* goals = botAI->planner.goals.get();
        if (!goals)
        {
            handler->SendErrorMessage("Skill path stock {}: refused, no plan kept yet (release the planner once)",
                                      bot->GetName());
            return false;
        }
        uint32 given = 0;
        uint32 refused = 0;
        for (SkillPath::Want const& want : path->wants)
            (bot->AddItem(want.item, want.count) ? given : refused) += want.count;
        for (SkillPath::Purchase const& need : path->needs)
            if (need.kind == SkillPath::StepKind::Tool && !bot->HasItemCount(need.spell, 1, true))
                (bot->AddItem(need.spell, 1) ? given : refused) += 1;
        Snapshot const stocked = FillSnapshot(bot, 0);
        std::shared_ptr<SkillPath::Path const> const kept =
            NeedsWithPath(stocked, *upkeep, BotNeedRates(botAI, stocked), PathInputsOf(botAI), data, needs);
        if (kept)
        {
            goals->path = kept;
            goals->pathMs = getMSTime();
            path = kept;
        }
        // An empty queue fails the fixture at once, with the path's reason (a scenario's stimulus reads the word)
        if (path->queue.empty())
        {
            handler->SendErrorMessage(
                "Skill path stock {}: refused, no craft queue ({}, {} stretches weighed, {} "
                "items given, {} without room)",
                bot->GetName(), SkillPath::ReasonWord(path->reason), path->weighed.size(), given, refused);
            return false;
        }
        handler->PSendSysMessage("Skill path stock {}: {} items given, {} without room, queue {} crafts, kept {}",
                                 bot->GetName(), given, refused, path->queue.size(), kept ? "yes" : "no");
    }
    handler->PSendSysMessage(
        "Skill path of {} (level {}, {}, {}): {} stretches weighed, {}", bot->GetName(), snapshot.level,
        UpkeepNeeds::Money(snapshot.money), ProfessionRules::Describe(*snapshot.person), path->weighed.size(),
        path->chosen == SkillPath::NONE ? std::string("none chosen (") + SkillPath::ReasonWord(path->reason) + ")"
                                        : std::string("one chosen (*)"));
    for (std::size_t at = 0; at < path->weighed.size(); ++at)
        handler->PSendSysMessage("  {} {}", static_cast<int>(at) == path->chosen ? "*" : "-",
                                 StretchText(path->weighed[at]));
    if (path->chosen != SkillPath::NONE)
    {
        SkillPath::Stretch const& taken = path->weighed[static_cast<std::size_t>(path->chosen)];
        for (std::size_t at = 0; at < taken.steps.size() && at < PATH_STEPS_SHOWN; ++at)
        {
            SkillPath::PathStep const& step = taken.steps[at];
            handler->PSendSysMessage("  next: {} {} at {}->{}, {:.1f} casts, {:.0f} c, {:.1f} min{}",
                                     StepWord(step.kind), step.id, step.from, step.to, step.casts, step.copper,
                                     step.minutes, step.zone ? Acore::StringFormat(", zone {}", step.zone) : "");
        }
        if (taken.steps.size() > PATH_STEPS_SHOWN)
            handler->PSendSysMessage("  ... {} steps in all", taken.steps.size());
    }
    std::string buys;
    for (SkillPath::Purchase const& purchase : path->needs)
        buys += Acore::StringFormat("{}{} {} ({})", buys.empty() ? "" : ", ", StepWord(purchase.kind), purchase.spell,
                                    UpkeepNeeds::Money(purchase.copper));
    std::string wants;
    for (SkillPath::Want const& want : path->wants)
        wants += Acore::StringFormat("{}{} x{}", wants.empty() ? "" : ", ", want.item, want.count);
    std::string queue;
    for (SkillPath::Craft const& craft : path->queue)
        queue += Acore::StringFormat("{}{}{} x{}{}", queue.empty() ? "" : ", ", craft.spell,
                                     craft.source ? Acore::StringFormat(" on {}", craft.source) : "", craft.casts,
                                     craft.forSkill  ? " (skill)"
                                     : craft.forSale ? " (sale)"
                                                     : "");
    handler->PSendSysMessage("  buys: {}; wants: {}; queue: {}", buys.empty() ? "none" : buys,
                             wants.empty() ? "none" : wants, queue.empty() ? "none" : queue);
    // every material the path uses with its cheapest source (WR20a): kind and the item, zone or recipe it comes by
    std::string materials;
    for (SkillPath::Material const& material : path->materials)
        materials += Acore::StringFormat(
            "{}{} x{} {} {} ({} c)", materials.empty() ? "" : ", ", material.item, material.used,
            Materials::KindWord(static_cast<Materials::Kind>(material.source)), material.via, material.copperEach);
    handler->PSendSysMessage("  materials: {}", materials.empty() ? "none" : materials);
    handler->PSendSysMessage("  second profession: {}", SecondText(path->second));
    for (UpkeepNeeds::Need const& need : needs.needs)
        if (need.order == UpkeepNeeds::NeedOrder::Profession)
            handler->PSendSysMessage("  need: {}", UpkeepNeeds::Describe(need));
    GoalApply::BotGoals const* goals = botAI->planner.goals.get();
    if (!goals || !goals->path)
        handler->PSendSysMessage("  kept: none yet (the next plan brings one)");
    else
    {
        SkillPath::Path const& kept = *goals->path;
        bool const same = kept.chosen == path->chosen && kept.reason == path->reason &&
                          kept.weighed.size() == path->weighed.size() && kept.second.take == path->second.take;
        handler->PSendSysMessage("  kept: from {} s ago, {} chosen ({}), second {}; {} the path now",
                                 getMSTimeDiff(goals->pathMs, getMSTime()) / IN_MILLISECONDS,
                                 kept.chosen == SkillPath::NONE ? "none" : "one", SkillPath::ReasonWord(kept.reason),
                                 SecondText(kept.second), same ? "as" : "differs from");
    }
    return true;
}
}  // namespace Planner
