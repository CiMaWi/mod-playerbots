/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the spec table's pure part (combat-foundation CF6; src/Bot/Engine/Table/SpecTable.h): the
// invariant first (persistent pushes only give today's baskets and relevances, tick after tick); a table yields its
// nodes in order at the relevances of its bands; bands map from kinds; no two rows of a band share a relevance; a
// row with an unknown spell, an unmet requirement or an option that is off is not live, in the rule's order; the
// merge and end-of-tick rules of fresh pushes; two rows on one interval trigger; the static checks; executed
// relevances back to rows; the startup row checks on fake resolvers.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot/Engine/Table tests/spec-table-selftest.cpp

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "SpecTable.h"

namespace
{
int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name.c_str());
    (pass ? passed : failed)++;
}

// A queue of baskets by action name, as Queue does it: the new rule and today's (updateExistingBasket: the higher
// relevance wins; nothing at the end of a tick)
struct ModelQueue
{
    std::vector<std::pair<std::string, spec::BasketRelevance>> baskets;

    void Push(std::string const& name, float relevance, bool fresh)
    {
        for (auto& basket : baskets)
        {
            if (basket.first == name)
            {
                spec::Merge(basket.second, spec::PushOf(relevance, fresh));
                return;
            }
        }
        baskets.emplace_back(name, spec::PushOf(relevance, fresh));
    }

    std::string Pop()
    {
        size_t best = baskets.size();
        float max = -1.0f;
        for (size_t i = 0; i < baskets.size(); ++i)
        {
            if (baskets[i].second.relevance > max)
            {
                max = baskets[i].second.relevance;
                best = i;
            }
        }
        if (best == baskets.size())
            return {};
        std::string const name = baskets[best].first;
        baskets.erase(baskets.begin() + static_cast<std::ptrdiff_t>(best));
        return name;
    }

    void EndOfTick()
    {
        for (size_t i = 0; i < baskets.size();)
        {
            if (spec::EndOfTick(baskets[i].second))
                ++i;
            else
                baskets.erase(baskets.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    float RelevanceOf(std::string const& name) const
    {
        for (auto const& basket : baskets)
            if (basket.first == name)
                return basket.second.relevance;
        return -1.0f;
    }
};

struct TodayQueue
{
    std::vector<std::pair<std::string, float>> baskets;

    void Push(std::string const& name, float relevance)
    {
        for (auto& basket : baskets)
        {
            if (basket.first == name)
            {
                if (basket.second < relevance)
                    basket.second = relevance;
                return;
            }
        }
        baskets.emplace_back(name, relevance);
    }

    std::string Pop()
    {
        size_t best = baskets.size();
        float max = -1.0f;
        for (size_t i = 0; i < baskets.size(); ++i)
        {
            if (baskets[i].second > max)
            {
                max = baskets[i].second;
                best = i;
            }
        }
        if (best == baskets.size())
            return {};
        std::string const name = baskets[best].first;
        baskets.erase(baskets.begin() + static_cast<std::ptrdiff_t>(best));
        return name;
    }
};

bool Same(ModelQueue const& model, TodayQueue const& today)
{
    if (model.baskets.size() != today.baskets.size())
        return false;
    for (size_t i = 0; i < model.baskets.size(); ++i)
    {
        if (model.baskets[i].first != today.baskets[i].first ||
            model.baskets[i].second.relevance != today.baskets[i].second ||
            model.baskets[i].second.kept != model.baskets[i].second.relevance)
            return false;
    }
    return true;
}

void TestInvariant()
{
    // Hand-written nodes never push fresh: the queue must behave exactly as today, pushes, pops and tick ends mixed
    std::vector<std::string> const names = {"melee", "shoot", "reach melee", "flee", "lightning bolt", "racial"};
    std::vector<float> const relevances = {5.0f, 10.0f, 10.002f, 10.001f, 20.0f, 21.0f, 30.0f, 90.0f, 10.003f};
    ModelQueue model;
    TodayQueue today;
    uint32_t seed = 12345;
    auto next = [&seed](uint32_t bound)
    {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) % bound;
    };

    bool same = true;
    bool popsSame = true;
    for (int tick = 0; tick < 2000 && same && popsSame; ++tick)
    {
        uint32_t const pushes = next(6);
        for (uint32_t i = 0; i < pushes; ++i)
        {
            std::string const& name = names[next(static_cast<uint32_t>(names.size()))];
            float const relevance = relevances[next(static_cast<uint32_t>(relevances.size()))];
            model.Push(name, relevance, false);
            today.Push(name, relevance);
        }
        uint32_t const pops = next(3);
        for (uint32_t i = 0; i < pops; ++i)
            popsSame &= model.Pop() == today.Pop();
        model.EndOfTick();
        same = Same(model, today);
    }
    Check(same && popsSame, "invariant: persistent pushes only give today's baskets, pops and relevances");
}

void TestMergeAndEndOfTick()
{
    spec::BasketRelevance fresh = spec::PushOf(9.94f, true);
    Check(fresh.relevance == 9.94f && fresh.kept == 0.0f, "a fresh push holds its relevance and no kept");
    Check(!spec::EndOfTick(fresh), "a basket of fresh pushes only goes at the tick's end");

    spec::BasketRelevance persistent = spec::PushOf(10.0f, false);
    spec::Merge(persistent, spec::PushOf(8.0f, false));
    Check(persistent.relevance == 10.0f && persistent.kept == 10.0f, "a lower persistent push changes nothing");
    spec::Merge(persistent, spec::PushOf(12.0f, false));
    Check(persistent.relevance == 12.0f && persistent.kept == 12.0f, "a higher persistent push raises both");
    Check(spec::EndOfTick(persistent) && persistent.relevance == 12.0f, "a persistent basket stays at kept");

    spec::BasketRelevance mixed = spec::PushOf(10.0f, false);
    spec::Merge(mixed, spec::PushOf(34.94f, true));
    Check(mixed.relevance == 34.94f && mixed.kept == 10.0f, "a higher fresh push raises the relevance only");
    Check(spec::EndOfTick(mixed) && mixed.relevance == 10.0f && mixed.kept == 10.0f,
          "at the tick's end a mixed basket falls back to kept");

    spec::BasketRelevance freshFirst = spec::PushOf(9.94f, true);
    spec::Merge(freshFirst, spec::PushOf(9.941f, false));
    Check(freshFirst.relevance == 9.941f && freshFirst.kept == 9.941f, "a persistent push into a fresh basket keeps");

    spec::BasketRelevance lowerFresh = spec::PushOf(20.0f, false);
    spec::Merge(lowerFresh, spec::PushOf(9.94f, true));
    Check(lowerFresh.relevance == 20.0f && lowerFresh.kept == 20.0f, "a lower fresh push changes nothing");

    // Two rows of one action (a proc row high, a dump row low) merge at the higher live row's relevance
    ModelQueue queue;
    queue.Push("lava burst", 9.94f, true);
    queue.Push("lava burst", 9.79f, true);
    queue.Push("lightning bolt", 9.74f, true);
    queue.Push("melee", 5.0f, false);
    Check(queue.baskets.size() == 3 && queue.RelevanceOf("lava burst") == 9.94f,
          "two rows of one action share a basket at the higher row's relevance");
    Check(queue.Pop() == "lava burst" && queue.Pop() == "lightning bolt", "rows pop top-down within the tick");
    queue.Push("lava burst", 9.79f, true);
    queue.EndOfTick();
    Check(queue.baskets.size() == 1 && queue.RelevanceOf("melee") == 5.0f,
          "a row not popped leaves nothing behind; a persistent push stays");
    queue.Push("lava burst", 9.79f, true);
    Check(queue.RelevanceOf("lava burst") == 9.79f, "a row live in the next tick comes back at its own relevance");
}

// The example of INTERFACE §2, with a two-row group joined in place
inline constexpr auto SurviveGroup = spec::Rows(spec::Defensive("healing wave", spec::HealthTier::Critical),
                                                spec::Defensive("lesser healing wave", spec::HealthTier::Low));
inline constexpr auto ExampleRows =
    spec::Rows(SurviveGroup, spec::Interrupt("wind shear"), spec::Cooldown("elemental mastery"),
               spec::Aoe("chain lightning", spec::Centre::Target, 2).Option("aoe"),
               spec::Renewal("flame shock", spec::Subject::TargetMine),
               spec::Spender("lava burst").When(spec::TargetHas("flame shock", spec::Mine)),
               spec::Spender("earth shock").Requires(spec::NotKnows("lava burst")), spec::Spender("lightning bolt"),
               spec::Filler("melee"));
inline constexpr spec::Table ExampleTable{"ele", spec::Role::Ranged, 1u, ExampleRows};

void TestNodes()
{
    std::vector<spec::Node> const nodes = spec::Nodes(ExampleTable.View());
    std::vector<std::pair<std::string, uint32_t>> const expected = {
        {"healing wave", 9499},   {"lesser healing wave", 9494},
        {"wind shear", 4499},     {"elemental mastery", 999},
        {"chain lightning", 994}, {"flame shock", 989},
        {"lava burst", 984},      {"earth shock", 979},
        {"lightning bolt", 974},  {"melee", 599}};
    bool inOrder = nodes.size() == expected.size();
    for (size_t i = 0; inOrder && i < nodes.size(); ++i)
    {
        inOrder = nodes[i].action == expected[i].first && nodes[i].hundredths == expected[i].second &&
                  nodes[i].relevance == static_cast<float>(expected[i].second) / 100.0f &&
                  nodes[i].trigger == "spec row::ele#" + std::to_string(i);
    }
    Check(inOrder, "a table yields one node per row, in order, at its band's relevances (groups joined in place)");
    Check(ExampleRows.Size() == 10 && ExampleRows[1].action == "lesser healing wave", "Rows flattens a row group");
    Check(ExampleRows[6].whenCount == 1 && ExampleRows[6].when[0].owner == spec::Owner::Mine &&
              ExampleRows[7].needCount == 1 && ExampleRows[4].option == "aoe",
          "modifiers set when, requires and option");

    std::string_view strategy;
    size_t index = 0;
    Check(spec::ParseRowQualifier("holy heal#12", strategy, index) && strategy == "holy heal" && index == 12,
          "a row qualifier parses back into strategy and index");
    Check(!spec::ParseRowQualifier("ele", strategy, index) && !spec::ParseRowQualifier("#3", strategy, index) &&
              !spec::ParseRowQualifier("ele#", strategy, index) && !spec::ParseRowQualifier("ele#3a", strategy, index),
          "a malformed row qualifier is refused");
}

void TestBands()
{
    using spec::Band;
    using spec::Kind;
    std::vector<std::pair<Kind, Band>> const map = {
        {Kind::Defensive, Band::Survive}, {Kind::Dispel, Band::Dispel},    {Kind::Interrupt, Band::Interrupt},
        {Kind::MoveTool, Band::Move},     {Kind::Opener, Band::Move},      {Kind::Heal, Band::Heal},
        {Kind::Form, Band::Rotation},     {Kind::Renewal, Band::Rotation}, {Kind::Proc, Band::Rotation},
        {Kind::Cooldown, Band::Rotation}, {Kind::Recover, Band::Rotation}, {Kind::Aoe, Band::Rotation},
        {Kind::Control, Band::Rotation},  {Kind::Pet, Band::Rotation},     {Kind::Builder, Band::Rotation},
        {Kind::Spender, Band::Rotation},  {Kind::Filler, Band::Filler}};
    bool mapped = map.size() == spec::KIND_COUNT;
    for (auto const& [kind, band] : map)
        mapped &= spec::BandOf(kind) == band;
    Check(mapped, "every kind maps to its band");

    // first row, last possible row and capacity per band (INTERFACE §5)
    struct Expect
    {
        Band band;
        uint32_t first;
        uint32_t last;
        uint32_t capacity;
    };
    std::vector<Expect> const table = {{Band::Survive, 9499, 9004, 100},   {Band::Dispel, 5499, 5004, 100},
                                       {Band::Interrupt, 4499, 4004, 100}, {Band::Move, 3999, 3504, 100},
                                       {Band::Heal, 3499, 3004, 100},      {Band::Rotation, 999, 604, 80},
                                       {Band::Filler, 599, 504, 20}};
    bool bands = true;
    for (Expect const& expect : table)
    {
        bands &= spec::RowHundredths(expect.band, 0) == expect.first &&
                 spec::BandCapacity(expect.band) == expect.capacity &&
                 spec::RowHundredths(expect.band, expect.capacity - 1) == expect.last;
    }
    Check(bands, "each band's first row, last possible row and capacity are as designed");

    // A band filled to capacity: distinct relevances, all in the band, none a round number, and the engine's
    // follow-up pushes (+0.001 to +0.003) stay below the row above
    bool distinct = true;
    bool roundFree = true;
    for (size_t band = 0; band < spec::BAND_COUNT; ++band)
    {
        Band const b = static_cast<Band>(band);
        std::set<uint32_t> seen;
        for (uint32_t k = 0; k < spec::BandCapacity(b); ++k)
        {
            uint32_t const hundredths = spec::RowHundredths(b, k);
            distinct &= seen.insert(hundredths).second && hundredths > spec::BandInfo(b).floor &&
                        hundredths < spec::BandInfo(b).top;
            roundFree &= hundredths % 5 == 4;
            if (k)
                distinct &= spec::RowRelevance(b, k) + 0.003f < spec::RowRelevance(b, k - 1);
        }
    }
    Check(distinct, "no two rows of a band share a relevance, and every row stays inside its band");
    Check(roundFree, "every row ends in 4 or 9 hundredths");
}

void TestLive()
{
    spec::Row const lavaBurst = spec::Spender("lava burst").When(spec::TargetHas("flame shock", spec::Mine));
    int ruleCalls = 0;
    int termCalls = 0;
    auto rule = [&ruleCalls](bool answer)
    {
        return [&ruleCalls, answer]()
        {
            ++ruleCalls;
            return answer;
        };
    };
    auto terms = [&termCalls](bool answer)
    {
        return [&termCalls, answer](spec::Term const&)
        {
            ++termCalls;
            return answer;
        };
    };

    spec::StandingFacts known;
    known.castsSpell = true;
    Check(spec::IsLive(lavaBurst, spec::StandingOf(known), rule(true), terms(true)),
          "a standing row whose rule and terms hold is live");

    spec::StandingFacts unknown = known;
    unknown.spellKnown = false;
    ruleCalls = termCalls = 0;
    Check(spec::StandingOf(unknown) == spec::Standing::UnknownSpell &&
              !spec::IsLive(lavaBurst, spec::StandingOf(unknown), rule(true), terms(true)) && !ruleCalls && !termCalls,
          "a row with an unknown spell is not live, and its rule is not asked");

    spec::StandingFacts noSpell;
    noSpell.castsSpell = false;
    noSpell.spellKnown = false;
    Check(spec::StandingOf(noSpell) == spec::Standing::Known, "an action that casts no spell needs no known spell");

    spec::StandingFacts unmet = known;
    unmet.requirementsHold = false;
    Check(spec::StandingOf(unmet) == spec::Standing::Requirement &&
              !spec::IsLive(lavaBurst, spec::StandingOf(unmet), rule(true), terms(true)),
          "a row with an unmet requirement is not live");

    spec::StandingFacts optionOff = known;
    optionOff.optionOn = false;
    Check(spec::StandingOf(optionOff) == spec::Standing::OptionOff &&
              !spec::IsLive(lavaBurst, spec::StandingOf(optionOff), rule(true), terms(true)),
          "a row whose option is off is not live");

    spec::StandingFacts noAction = known;
    noAction.actionExists = false;
    Check(spec::StandingOf(noAction) == spec::Standing::NoAction, "a row without its action is not standing");

    ruleCalls = termCalls = 0;
    Check(!spec::IsLive(lavaBurst, spec::Standing::Known, rule(false), terms(true)) && ruleCalls == 1 && !termCalls,
          "the terms are not read when the kind's rule does not hold");
    Check(!spec::IsLive(lavaBurst, spec::Standing::Known, rule(true), terms(false)), "a failed term stops the row");

    spec::Row const fallback = spec::Spender("earth shock").Requires(spec::NotKnows("lava burst"));
    spec::Row const wand = spec::Builder("shoot").Requires(spec::Wields(spec::Gear::Wand), spec::Knows("shoot"));
    auto knowsLavaBurst = [](spec::Requirement const& requirement) { return requirement.spell == "lava burst"; };
    auto wieldsNothing = [](spec::Requirement const& requirement) { return requirement.need != spec::Need::Wields; };
    auto knowsNothing = [](spec::Requirement const&) { return false; };
    auto everything = [](spec::Requirement const&) { return true; };
    Check(!spec::RequirementsHold(fallback, knowsLavaBurst) && spec::RequirementsHold(fallback, knowsNothing),
          "NotKnows holds until the better spell is known");
    Check(!spec::RequirementsHold(wand, wieldsNothing) && spec::RequirementsHold(wand, everything) &&
              !spec::RequirementsHold(wand, knowsNothing),
          "Wields and Knows must all hold");
}

void TestSharedTrigger()
{
    // Two rows name one registered trigger with a 1 s interval: one answer, asked once per interval, seen by both
    spec::SharedAnswer shared;
    int asks = 0;
    bool truth = false;
    auto ask = [&]()
    {
        ++asks;
        return truth;
    };
    bool both = true;
    uint32_t const interval = 1000;
    for (uint32_t now = 1000; now <= 5000; now += 100)
    {
        truth = (now / interval) % 2 == 1;
        bool const first = shared.Read(now, interval, ask);
        bool const second = shared.Read(now, interval, ask);
        both &= first == second && first == truth;
    }
    Check(both, "two rows on one interval trigger both see each new answer");
    Check(asks == 5, "the shared trigger is asked once per interval, not once per row");

    spec::SharedAnswer everyTick;
    asks = 0;
    truth = true;
    everyTick.Read(100, 0, ask);
    everyTick.Read(100, 0, ask);
    everyTick.Read(117, 0, ask);
    Check(asks == 2, "an interval of 0 asks once per tick");
}

// Each static fault through the fault function
inline constexpr auto BadOrder =
    spec::Rows(spec::Spender("lightning bolt"), spec::Interrupt("wind shear"), spec::Filler("melee"));
inline constexpr auto NoFiller = spec::Rows(spec::Spender("lightning bolt"));
inline constexpr auto ConditionalFiller = spec::Rows(spec::Filler("shoot").Requires(spec::Wields(spec::Gear::Wand)));
inline constexpr auto WhenFiller = spec::Rows(spec::Filler("melee").When(spec::Solo()));
inline constexpr auto OptionFiller = spec::Rows(spec::Filler("melee").Option("aoe"));
inline constexpr auto ReservedKind = spec::Rows(spec::Opener("ambush"), spec::Filler("melee"));
inline constexpr auto ReservedSubject =
    spec::Rows(spec::Renewal("searing totem", spec::Subject::Totem), spec::Filler("melee"));
inline constexpr auto ReservedWithInstead =
    spec::Rows(spec::Renewal("searing totem", spec::Subject::Totem).Instead("searing totem"), spec::Filler("melee"));
inline constexpr auto ProcNoEffect = spec::Rows(spec::Proc("lava burst", ""), spec::Filler("melee"));
inline constexpr auto AoeOne = spec::Rows(spec::Aoe("chain lightning", spec::Centre::Target, 1), spec::Filler("melee"));
inline constexpr auto AoeFive =
    spec::Rows(spec::Aoe("chain lightning", spec::Centre::Target, 5), spec::Filler("melee"));
inline constexpr auto TooManyWhen =
    spec::Rows(spec::Spender("x").When(spec::Solo(), spec::Solo(), spec::Solo(), spec::Solo()), spec::Filler("melee"));
inline constexpr auto TooManyNeeds = spec::Rows(
    spec::Spender("x").Requires(spec::Knows("a"), spec::Knows("b")).Requires(spec::Knows("c")), spec::Filler("melee"));
inline constexpr auto RageRecover = spec::Rows(spec::Recover("bloodrage"), spec::Filler("melee"));
inline constexpr auto EmptyAura = spec::Rows(spec::Spender("x").When(spec::Has("")), spec::Filler("melee"));

template <size_t N>
constexpr auto FullFillerBand()
{
    spec::RowList<N> list{};
    for (size_t i = 0; i < N; ++i)
        list.rows[i] = spec::Filler("melee");
    return list;
}

static_assert(spec::Check(ExampleRows) == spec::Fault::None);
static_assert(spec::Check(spec::RowList<0>{}) == spec::Fault::Empty);
static_assert(spec::Check(BadOrder) == spec::Fault::BandOrder);
static_assert(spec::Check(NoFiller) == spec::Fault::NoFinalFiller);
static_assert(spec::Check(ConditionalFiller) == spec::Fault::FinalFillerConditional);
static_assert(spec::Check(WhenFiller) == spec::Fault::FinalFillerConditional);
static_assert(spec::Check(OptionFiller) == spec::Fault::FinalFillerConditional);
static_assert(spec::Check(ReservedKind) == spec::Fault::ReservedWithoutInstead);
static_assert(spec::Check(ReservedSubject) == spec::Fault::ReservedWithoutInstead);
static_assert(spec::Check(ReservedWithInstead) == spec::Fault::None);
static_assert(spec::Check(ProcNoEffect) == spec::Fault::ProcWithoutEffect);
static_assert(spec::Check(AoeOne) == spec::Fault::AoeCount);
static_assert(spec::Check(AoeFive) == spec::Fault::AoeCount);
static_assert(spec::Check(TooManyWhen) == spec::Fault::TooManyTerms);
static_assert(spec::Check(TooManyNeeds) == spec::Fault::TooManyTerms);
static_assert(spec::Check(RageRecover) == spec::Fault::MissingInput);
static_assert(spec::Check(EmptyAura) == spec::Fault::EmptyName);
static_assert(spec::Check(FullFillerBand<20>()) == spec::Fault::None);
static_assert(spec::Check(FullFillerBand<21>()) == spec::Fault::OverCapacity);

void TestStaticChecks()
{
    // The static_asserts above are the proof; this records that they compiled
    Check(true, "the static checks refuse band order, final filler, capacity, reserved, proc, aoe and term faults");
    Check(ExampleTable.View().count == 10 && ExampleTable.View().name == "ele" &&
              ExampleTable.View().role == spec::Role::Ranged,
          "a valid table compiles and gives its view");
}

// Executed relevances back to rows: the row's own value and the follow-up pushes inside its step name it
void TestRelevanceToRow()
{
    spec::TableView const view = ExampleTable.View();
    Check(spec::RowOfRelevance(view, "lava burst", 9.84f) == 6 && spec::RowOfRelevance(view, "lava burst", 9.843f) == 6,
          "an executed relevance and its follow-up pushes map to their row");
    Check(spec::RowOfRelevance(view, "healing wave", 94.99f) == 0 &&
              spec::RowOfRelevance(view, "lesser healing wave", 94.94f) == 1 &&
              spec::RowOfRelevance(view, "melee", 5.99f) == 9,
          "each band's rows map by position");
    Check(spec::RowOfRelevance(view, "lava burst", 9.89f) == -1 && spec::RowOfRelevance(view, "melee", 5.0f) == -1 &&
              spec::RowOfRelevance(view, "lava burst", 10.0f) == -1,
          "another row's relevance, a default action or a hand-written node maps to no row");
}

// The startup row checks on fake resolvers
inline constexpr auto CheckedRows =
    spec::Rows(spec::Interrupt("wind shear").Instead("no such trigger"),
               spec::Proc("lava burst", "lava flows").When(spec::Trigger("known trigger"), spec::Has("no such aura")),
               spec::Spender("earth shock").Requires(spec::Knows("passive talent")).Option("no such option"),
               spec::Spender("lightning bolt").Option("aoe"), spec::Filler("melee"));
inline constexpr spec::Table CheckedTable{"ele", spec::Role::Ranged, 1u, CheckedRows};

void TestRowChecks()
{
    std::set<std::string> const triggers = {"known trigger"};
    std::set<std::string> const strategies = {"aoe"};
    std::set<std::string> const active = {"wind shear", "lava burst", "earth shock", "lightning bolt"};
    std::set<std::string> const passive = {"passive talent", "lava flows"};
    spec::RowResolvers resolve;
    resolve.hasTrigger = [&](std::string_view name) { return triggers.count(std::string(name)) > 0; };
    resolve.hasStrategy = [&](std::string_view name) { return strategies.count(std::string(name)) > 0; };
    resolve.spell = [&](std::string_view name)
    {
        std::string const key(name);
        return active.count(key) ? spec::SpellName::Active
                                 : (passive.count(key) ? spec::SpellName::Passive : spec::SpellName::Unknown);
    };
    resolve.actionSpell = [](std::string_view action)
    {
        return action == "melee" ? std::string() : (action == "lightning bolt" ? "lightnin bolt" : std::string(action));
    };
    resolve.inSiblingGroup = [](std::string_view) { return true; };

    std::vector<std::string> found;
    for (spec::RowFinding const& finding : spec::RowChecks(CheckedTable.View(), resolve))
        found.push_back(std::string(finding.kind) + ":" + finding.name);
    std::vector<std::string> const expected = {"row-trigger:no such trigger", "row-spell:no such aura",
                                               "row-option:no such option", "row-spell:passive talent",
                                               "row-spell:lightnin bolt"};
    Check(found == expected,
          "row checks list an unknown Instead trigger, option, aura and action spell, and a passive Knows name");

    resolve.inSiblingGroup = [](std::string_view) { return false; };
    resolve.handWritten = {989, 1000};  // lightning bolt's row and a round hand-written number
    found.clear();
    for (spec::RowFinding const& finding : spec::RowChecks(CheckedTable.View(), resolve))
        if (finding.kind == "row-group" || finding.kind == "row-clash")
            found.push_back(std::string(finding.kind) + ":" + finding.name);
    Check(found == std::vector<std::string>{"row-group:ele", "row-clash:ele#3"},
          "row checks list a table outside a sibling group and a hand-written node on a row's relevance");
}
}  // namespace

int main()
{
    TestInvariant();
    TestMergeAndEndOfTick();
    TestNodes();
    TestBands();
    TestLive();
    TestSharedTrigger();
    TestStaticChecks();
    TestRelevanceToRow();
    TestRowChecks();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
