/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SkillPathLive.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "BotProfessionMgr.h"
#include "Chat.h"
#include "DBCStores.h"
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
}  // namespace

PathData PathDataNow()
{
    PathData data;
    if (RecipeIndexReady())
        data.recipes = CurrentRecipeIndex();
    if (IndexesReady())
        data.indexes = Indexes();
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
                                                     UpkeepNeeds::NeedList& needs)
{
    if (!data.recipes || !snapshot.person)
    {
        needs = UpkeepNeeds::ComputeNeeds(snapshot, upkeep, SkillPathBridge::WithoutPath(rates));
        return nullptr;
    }
    auto const started = std::chrono::steady_clock::now();
    SkillPath::PathWorld const world{*data.recipes,
                                     [](std::uint32_t spell) { return UpkeepNeeds::MayLearn(spell); },
                                     SellCopper,
                                     {},
                                     {},
                                     SkillPath::Fault::None,
                                     ToolSources()};
    SkillPathBridge::Computed computed =
        SkillPathBridge::Compute(snapshot, upkeep, rates, inputs, world, BracketsOf(data.indexes.get(), snapshot.team),
                                 CategoriesOf(snapshot.toolsHeld));
    needs = std::move(computed.needs);
    auto path = std::make_shared<SkillPath::Path const>(std::move(computed.path));
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "profession path computed"))
        row->Record(static_cast<uint64>(std::max<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count(),
            0)));
    return path;
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
                                     craft.forSkill ? " (skill)" : "");
    handler->PSendSysMessage("  buys: {}; wants: {}; queue: {}", buys.empty() ? "none" : buys,
                             wants.empty() ? "none" : wants, queue.empty() ? "none" : queue);
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
