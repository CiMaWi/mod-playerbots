/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SKILLPATHBRIDGE_H
#define _PLAYERBOT_SKILLPATHBRIDGE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include "GoalModel.h"
#include "PlannerSnapshot.h"
#include "ProfessionRules.h"
#include "SkillPathRules.h"
#include "SkillPathTypes.h"
#include "UpkeepNeedRules.h"

// The skill path live (professions plan WR11, PLAN 3.3, 3.8): the path's inputs from a bot's snapshot and the needs
// list, and its recipes and ranks back into the needs. Pure: the planner worker and the bot's thread (the `professions
// path` command, the errand's needs) compute the same path from the same snapshot. Include it only in a .cpp.
namespace SkillPathBridge
{
using Planner::Snapshot;

// A zone the bot could gather in: its level bracket, its continent and whether the bot's side may play there
struct ZoneBracket
{
    std::uint32_t zone{0};
    std::uint32_t low{0};
    std::uint32_t high{0};
    std::uint32_t map{0};
    bool open{true};
};

constexpr std::uint32_t ZONE_LEVELS_ABOVE = 5;  // [E] a zone whose bracket starts higher is not yet played in
constexpr std::uint32_t MAP_OUTLAND = 530;
constexpr std::uint32_t MAP_NORTHREND = 571;
constexpr std::uint32_t OUTLAND_LEVEL = 58;  // the Dark Portal's level
constexpr std::uint32_t NORTHREND_LEVEL = 68;

// Whether a bot of this level plays in the zone: a known bracket that starts at most ZONE_LEVELS_ABOVE above it, on
// a continent its level reaches, its side's or contested
inline bool Plays(ZoneBracket const& zone, std::uint32_t level)
{
    if (!zone.open || !zone.high || zone.low > level + ZONE_LEVELS_ABOVE)
        return false;
    if (zone.map == MAP_OUTLAND)
        return level >= OUTLAND_LEVEL;
    if (zone.map == MAP_NORTHREND)
        return level >= NORTHREND_LEVEL;
    return true;
}

// Until a zone's danger is measured (WR13b) its bracket stands for it: the levels its low end lies above the bot
inline Materials::PlaceRisk BracketRisk(ZoneBracket const& zone, std::uint32_t level)
{
    Materials::PlaceRisk risk;
    risk.levelsAbove = zone.low > level ? static_cast<float>(zone.low - level) : 0.0f;
    return risk;
}

// The reserve before the professions: what the bot keeps back for repair, stock, training and riding. The path spends
// the gold above it, so it never reads its own needs' funding
inline std::uint64_t ReserveBeforeProfession(UpkeepNeeds::NeedList const& list)
{
    return UpkeepNeeds::Scaled(list,
                               list.reserveRepair + list.reserveStock + list.reserveTraining + list.reserveRiding);
}

// The planner's own measures for the path: the need factor against that reserve, a minute at the band's rate
inline Materials::Rates RatesOf(Snapshot const& snapshot, std::uint64_t reserve)
{
    Planner::Goals::Weights const weights = Planner::Goals::FromTraits(snapshot);
    Planner::Goals::Need const need = Planner::Goals::NeedFor(snapshot, weights, reserve);
    Materials::Rates rates;
    rates.exchange = need.exchange;
    rates.factor = need.factor;
    rates.minuteValue = Planner::Goals::BAND_XP_PER_MINUTE[WorldTravel::LevelBand(snapshot.level)];
    rates.cautionWeight = Planner::Goals::CautionWeight(weights);
    return rates;
}

inline bool IsProfession(std::uint16_t skill)
{
    return std::any_of(std::begin(ProfessionRules::PROFESSIONS), std::end(ProfessionRules::PROFESSIONS),
                       [skill](ProfessionRules::ProfessionInfo const& info)
                       { return static_cast<std::uint16_t>(info.Which) == skill; });
}

// The path's bot from its snapshot. categories: the totem categories its tools cover (the caller reads the store)
inline SkillPath::PathBot BotOf(Snapshot const& snapshot, SkillPath::PathInputs const& inputs,
                                UpkeepNeeds::NeedList const& base, std::vector<ZoneBracket> const& zones,
                                std::vector<std::uint32_t> const& categories)
{
    SkillPath::PathBot bot;
    bot.level = snapshot.level;
    bot.cls = snapshot.cls;
    std::uint64_t const reserve = ReserveBeforeProfession(base);
    bot.money = snapshot.money > reserve ? snapshot.money - reserve : 0;
    bot.rates = RatesOf(snapshot, reserve);
    for (ZoneBracket const& zone : zones)
        if (Plays(zone, snapshot.level))
        {
            bot.zones.push_back(zone.zone);
            bot.risks[zone.zone] = BracketRisk(zone, snapshot.level);
        }
    for (Planner::Skill const& skill : snapshot.skills)
        if (skill.value > 0 && IsProfession(skill.id))
            bot.skills.push_back({skill.id, skill.value, skill.max});
    bot.knownRecipes = snapshot.knownRecipes;
    bot.toolsHeld = snapshot.toolsHeld;
    bot.categoriesHeld = categories;
    for (Planner::MaterialHeld const& held : snapshot.materials)
    {
        bot.held.push_back({held.item, held.bags + held.bank});
        bot.recipeItems.push_back(held.item);  // the path matches them against recipe items only
    }
    bot.consumables = inputs.consumables;
    bot.uses = inputs.uses;
    bot.anyStretch = inputs.anyStretch;
    return bot;
}

// The path's recipes and ranks for the needs, with the chosen stretch's skill
inline std::vector<UpkeepNeeds::PathPurchase> PurchasesOf(SkillPath::Path const& path)
{
    std::vector<UpkeepNeeds::PathPurchase> purchases;
    if (path.chosen < 0 || static_cast<std::size_t>(path.chosen) >= path.weighed.size())
        return purchases;
    std::uint16_t const skill = path.weighed[static_cast<std::size_t>(path.chosen)].skill;
    for (SkillPath::Purchase const& purchase : path.needs)
        if (purchase.kind != SkillPath::StepKind::Tool)  // a vendor's tool is the profession's own need
            purchases.push_back(
                {purchase.spell, skill, purchase.copper, purchase.kind == SkillPath::StepKind::Rank, purchase.item});
    return purchases;
}

// The tools the recipes ask for, from the professions' table (ProfessionRules::TOOLS): a vendor's at its price, a made
// one at 0 (the path makes it). price: what a vendor asks for the item, in copper (live: its item template's)
template <class Price>
std::vector<SkillPath::ToolSource> ToolSourcesOf(Price const& price)
{
    std::vector<SkillPath::ToolSource> sources;
    for (ProfessionRules::ToolRow const& row : ProfessionRules::TOOLS)
        if (std::none_of(sources.begin(), sources.end(),
                         [&row](SkillPath::ToolSource const& source) { return source.item == row.Item; }))
            sources.push_back({row.Item, row.Category, row.Vendor ? static_cast<std::uint32_t>(price(row.Item)) : 0u});
    return sources;
}

inline UpkeepNeeds::NeedRates WithoutPath(UpkeepNeeds::NeedRates rates)
{
    rates.pathKnown = false;
    rates.pathPurchases.clear();
    rates.secondWaits = 0;
    return rates;
}

inline UpkeepNeeds::NeedRates WithPath(UpkeepNeeds::NeedRates rates, SkillPath::Path const& path)
{
    rates.pathKnown = true;
    rates.pathPurchases = PurchasesOf(path);
    SkillPath::SecondChoice const& second = path.second;
    rates.secondWaits = second.craft && second.take != second.craft ? second.craft : 0;
    return rates;
}

constexpr std::uint32_t FIRST_CAP = 75;  // a new profession's cap before its first rank

// A profession weighed from its first point, as if the bot had just learned it, beside the skills it holds (their
// materials count: a herbalist's herbs, a miner's ore)
struct Fresh
{
    bool weighed{false};    // a stretch with a net worth: open, not worth it or short of gold
    bool open{false};       // a stretch worth it that the bot can pay
    bool waitsGold{false};  // a stretch worth it that the bot cannot pay yet
    double worth{0.0};      // the net worth of the open stretch the path would take, else the best one weighed
    SkillPath::Brings brings;
};

inline Fresh FreshOf(SkillPath::Path const& path, std::uint16_t skill)
{
    Fresh fresh;
    double perMinute = 0.0;
    for (SkillPath::Stretch const& stretch : path.weighed)
    {
        bool const open = stretch.reason == SkillPath::Reason::Open || stretch.reason == SkillPath::Reason::Chosen;
        if (stretch.skill != skill || (!open && stretch.reason != SkillPath::Reason::GoldShort &&
                                       stretch.reason != SkillPath::Reason::NotWorthIt))
            continue;
        fresh.waitsGold = fresh.waitsGold || (stretch.reason == SkillPath::Reason::GoldShort && stretch.netWorth > 0.0);
        if (open && (!fresh.open || stretch.perMinute > perMinute))
        {
            fresh.open = true;
            perMinute = stretch.perMinute;
            fresh.worth = stretch.netWorth;
            fresh.brings = stretch.brings;
        }
        else if (!fresh.open && (!fresh.weighed || stretch.netWorth > fresh.worth))
        {
            fresh.worth = stretch.netWorth;
            fresh.brings = stretch.brings;
        }
        fresh.weighed = true;
    }
    return fresh;
}

inline bool HoldsSkill(Snapshot const& snapshot, ProfessionRules::Skill skill)
{
    return std::any_of(snapshot.skills.begin(), snapshot.skills.end(), [skill](Planner::Skill const& held)
                       { return held.id == static_cast<std::uint16_t>(skill) && held.value > 0; });
}

// The second main (PLAN 3.2, 3.3; WR.7: only a bot whose second slot is unlearned). Due once the bot holds its first
// main and has the gatherer's level, with the person's craft in the second slot and no second main held. The craft is
// taken when a stretch of it from its first point is worth it and paid for; else its learn waits. buyers: once a buyer
// other than the vendors pays for materials (SkillPath::BUYERS_BEYOND_VENDORS), a bot short of gold takes the second
// gatherer that brings the most instead when that is worth more than the craft
inline SkillPath::SecondChoice SecondOf(Snapshot const& snapshot, SkillPath::PathBot const& bot, bool shortOfGold,
                                        std::uint32_t gathererLevel, SkillPath::PathWorld const& world,
                                        bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    using ProfessionRules::Skill;
    SkillPath::SecondChoice choice;
    if (!snapshot.person || snapshot.level < gathererLevel)
        return choice;
    Skill const first = snapshot.person->Mains[0];
    Skill const craft = snapshot.person->Mains[1];
    std::size_t const mainsHeld = static_cast<std::size_t>(
        std::count_if(snapshot.skills.begin(), snapshot.skills.end(), [](Planner::Skill const& held)
                      { return held.value > 0 && ProfessionRules::IsMain(static_cast<Skill>(held.id)); }));
    if (first == Skill::None || !ProfessionRules::Is(craft, ProfessionRules::Kind::Craft) ||
        !HoldsSkill(snapshot, first) || HoldsSkill(snapshot, craft) || mainsHeld >= ProfessionRules::MAX_MAINS)
        return choice;
    choice.craft = static_cast<std::uint16_t>(craft);
    choice.money = bot.money;
    // One weigh of the craft and, for a bot short of gold, the gatherers it could take instead, at their first point
    auto const insteadOf = [&](Skill gatherer)
    { return buyers && shortOfGold && gatherer != first && !HoldsSkill(snapshot, gatherer); };
    SkillPath::PathBot fresh = bot;
    fresh.skills.push_back({choice.craft, 1, FIRST_CAP});
    for (Skill const gatherer : ProfessionRules::GATHERERS)
        if (insteadOf(gatherer))
            fresh.skills.push_back({static_cast<std::uint16_t>(gatherer), 1, FIRST_CAP});
    SkillPath::Path const weighed = SkillPath::Weigh(fresh, world);
    Fresh const crafted = FreshOf(weighed, choice.craft);
    choice.craftWorth = crafted.worth;
    if (crafted.open)
    {
        choice.take = choice.craft;
        choice.reason = SkillPath::Reason::Chosen;
        choice.brings = crafted.brings;
        return choice;
    }
    Fresh gathered;
    for (Skill const gatherer : ProfessionRules::GATHERERS)
    {
        if (!insteadOf(gatherer))
            continue;
        Fresh const candidate = FreshOf(weighed, static_cast<std::uint16_t>(gatherer));
        if (candidate.weighed && (!choice.gatherer || candidate.worth > gathered.worth))
        {
            choice.gatherer = static_cast<std::uint16_t>(gatherer);
            gathered = candidate;
        }
    }
    choice.gathererWorth = gathered.worth;
    if (choice.gatherer && gathered.worth > 0.0 && (!crafted.weighed || gathered.worth > crafted.worth))
    {
        choice.take = choice.gatherer;
        choice.reason = SkillPath::Reason::GoldShort;
        choice.brings = gathered.brings;
        return choice;
    }
    choice.reason = crafted.waitsGold ? SkillPath::Reason::GoldShort : SkillPath::Reason::NotWorthIt;
    choice.brings = crafted.brings;
    return choice;
}

// Short of gold: money below the reserve before the professions (the need factor's own test, NeedFor)
inline bool ShortOfGold(Snapshot const& snapshot, UpkeepNeeds::NeedList const& base)
{
    return Planner::Goals::NeedFor(snapshot, Planner::Goals::FromTraits(snapshot), ReserveBeforeProfession(base))
        .shortOfReserve;
}

// The worker's order (WR11): the needs without the path, the path and the second choice from them, the needs with the
// path's recipes and ranks and the second choice (the reserve and the shortfall see them)
struct Computed
{
    UpkeepNeeds::NeedList needs;
    SkillPath::Path path;
};

inline Computed Compute(Snapshot const& snapshot, Planner::UpkeepIndex const& upkeep,
                        UpkeepNeeds::NeedRates const& rates, SkillPath::PathInputs const& inputs,
                        SkillPath::PathWorld const& world, std::vector<ZoneBracket> const& zones,
                        std::vector<std::uint32_t> const& categories,
                        bool buyers = SkillPath::BUYERS_BEYOND_VENDORS)
{
    UpkeepNeeds::NeedRates const base = WithoutPath(rates);
    Computed computed;
    computed.needs = UpkeepNeeds::ComputeNeeds(snapshot, upkeep, base);
    SkillPath::PathBot const bot = BotOf(snapshot, inputs, computed.needs, zones, categories);
    computed.path = SkillPath::Weigh(bot, world);
    computed.path.second =
        SecondOf(snapshot, bot, ShortOfGold(snapshot, computed.needs), rates.gathererLevel, world, buyers);
    computed.needs = UpkeepNeeds::ComputeNeeds(snapshot, upkeep, WithPath(base, computed.path));
    return computed;
}

// The consumables item use wants, from its answer's keeps (by entry) and the lines' strongest offered entries
template <class Answer>
std::vector<SkillPath::ConsumableWant> ConsumablesOf(Answer const& answer)
{
    std::vector<SkillPath::ConsumableWant> wants;
    for (auto const& keep : answer.keeps)
        if (keep.keepUnits > 0 && keep.worthCopperEach > 0)
            wants.push_back({keep.entry, keep.keepUnits, keep.held, static_cast<float>(keep.worthCopperEach)});
    for (auto const& line : answer.lines)
        if (line.buyEntry && line.wantedStock > line.held && line.worthCopperEach > 0 &&
            std::none_of(wants.begin(), wants.end(),
                         [&line](SkillPath::ConsumableWant const& want) { return want.item == line.buyEntry; }))
            wants.push_back({line.buyEntry, line.wantedStock, line.held, static_cast<float>(line.worthCopperEach)});
    std::sort(wants.begin(), wants.end(),
              [](SkillPath::ConsumableWant const& a, SkillPath::ConsumableWant const& b) { return a.item < b.item; });
    return wants;
}
}  // namespace SkillPathBridge

#endif
