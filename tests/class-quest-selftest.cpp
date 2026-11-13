/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the class-quest rules (upkeep WU20a, 3.11): the table's states, the governed-bot rule, the chain's
// next step and the unlock bonus, with an invariant search over generated bots (seed printed; argv[1] overrides it).
// Build (module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/class-quest-selftest.cpp

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "ClassQuestRules.h"
#include "GoalInterface.h"
#include "GoalModelHarness.h"
#include "QuestCircuitLog.h"

using namespace Planner::ClassQuests;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? passed : failed)++;
}

constexpr std::uint32_t WARRIOR = 1, PALADIN = 2, HUNTER = 3, WARLOCK = 9;
constexpr std::uint32_t HUMAN = 1, ORC = 2, UNDEAD = 5;

auto const noTrainer = [](std::uint32_t) { return false; };
auto const anyRoute = [](std::uint32_t) { return true; };

BotFacts Bot(std::uint32_t cls, std::uint32_t race, std::uint32_t level, std::vector<std::uint32_t> rewarded = {})
{
    BotFacts bot;
    bot.cls = cls;
    bot.race = race;
    bot.level = level;
    bot.governed = true;
    bot.levelXp = 8000;
    std::sort(rewarded.begin(), rewarded.end());
    bot.rewarded = std::move(rewarded);
    return bot;
}

Judgement JudgeId(std::uint32_t quest, BotFacts const& bot)
{
    Entry const* entry = Find(quest);
    return entry ? Judge(*entry, bot, noTrainer, anyRoute) : Judgement{};
}

std::string Show(Judgement const& judgement)
{
    return std::string(VerdictWord(judgement.verdict)) +
           (judgement.step ? " step " + std::to_string(judgement.step->quest) : std::string());
}

void Table()
{
    bool sorted = true;
    int classQuests = 0;
    for (std::size_t i = 0; i < std::size(TABLE); ++i)
    {
        if (i && TABLE[i - 1].quest >= TABLE[i].quest)
            sorted = false;
        classQuests += IsClassQuest(TABLE[i]) ? 1 : 0;
    }
    Check(sorted, "the table is sorted by quest id, each quest once");
    // config/wu-class-quests.json has 60 quests; the 10 trainer quests left the table
    Check(classQuests == 50,
          "50 class quests (60 of the census less the 10 trainer quests): " + std::to_string(classQuests));
    bool doable = true;
    for (std::uint32_t quest : {1498u, 1665u, 1678u, 1683u, 1785u, 1788u, 9691u, 6081u, 9673u, 6086u, 6089u, 6103u,
                                9675u, 9555u, 6125u, 6130u, 1652u, 10350u})
        doable = doable && Find(quest) && StateOf(*Find(quest), noTrainer) == State::Doable;
    Check(doable, "the census's doable class quests are doable (1652 and 10350 included)");
    bool automatic = true;
    for (std::uint32_t quest : {1719u, 12687u, 12801u, 7583u, 96u, 1661u, 9712u, 12228u, 9364u, 1795u, 6082u, 1471u})
        automatic = automatic && Find(quest) && StateOf(*Find(quest), noTrainer) == State::Automatic;
    Check(automatic,
          "the automatic ones (scripted, instance, placeholder, objective; 9364 and 1795 too) are not doable");
    bool trainer = true;
    for (std::uint32_t quest : {31u, 5061u, 4490u, 7463u, 7631u, 7647u, 9737u, 11001u, 12172u, 12173u})
        trainer = trainer && !Find(quest);
    Check(trainer, "the trainer quests are not in the table");
    auto const teaches5502 = [](std::uint32_t spell) { return spell == 5502; };
    auto const teaches13819 = [](std::uint32_t spell) { return spell == 13819; };
    Check(StateOf(*Find(1652), teaches5502) == State::Ordinary && StateOf(*Find(9691), teaches5502) == State::Ordinary,
          "1652 and 9691 leave the table when the trainer teaches 5502");
    Check(StateOf(*Find(1661), teaches13819) == State::Ordinary && StateOf(*Find(1661), noTrainer) == State::Automatic,
          "1661 leaves the table when the trainer teaches Summon Warhorse 13819, else stays automatic");
    Entry const* binding = Find(1795);
    Check(binding && binding->prev[0] == 1804 && binding->prev[1] == 1805,
          "1795 needs 1804 or 1805 (their NextQuestID), not only its own PrevQuestID 0");
}

void Scenarios()
{
    BotFacts const warrior = Bot(WARRIOR, ORC, 10, {1505});
    Judgement const path = JudgeId(1498, warrior);
    std::printf("info: warrior 10 (orc, 1505 rewarded) 1498: %s, bonus %.0f XP\n", Show(path).c_str(),
                BonusXp(warrior));
    Check(path.verdict == Verdict::Bonus && path.step->quest == 1498, "a warrior at 10 with 1498 takeable: bonus");

    BotFacts const fresh = Bot(WARRIOR, ORC, 10);
    Check(JudgeId(1498, fresh).verdict == Verdict::Bonus && JudgeId(1498, fresh).step->quest == 1505,
          "without 1505 the chain's next step 1505 gets the bonus: " + Show(JudgeId(1498, fresh)));

    BotFacts const hunter = Bot(HUNTER, ORC, 10);
    Entry const* first = NextStep(*Find(6081), hunter);
    Check(first && first->quest == 6062,
          "a hunter's chain starts at its first step 6062: " + std::to_string(first ? first->quest : 0));
    Check(JudgeId(6081, hunter).verdict == Verdict::StepWaits,
          "the taming steps are objective quests (WU20c): no bonus yet, " + Show(JudgeId(6081, hunter)));
    BotFacts const tamed = Bot(HUNTER, ORC, 10, {6062, 6083, 6082});
    Check(JudgeId(6081, tamed).verdict == Verdict::Bonus, "after the taming steps 6081 gets the bonus");

    Check(JudgeId(1719, Bot(WARRIOR, ORC, 30, {1718})).verdict == Verdict::NotDoable, "automatic 1719: no bonus");
    Entry const* path1498 = Find(1498);
    Check(Judge(*path1498, warrior, noTrainer, [](std::uint32_t) { return false; }).verdict == Verdict::NoRoute,
          "a quest without a route: no bonus");
    BotFacts known = warrior;
    known.knownSpells = {71};
    Check(JudgeId(1498, known).verdict == Verdict::SpellKnown, "a bot that knows Defensive Stance 71: no bonus");
    BotFacts gaveUp = warrior;
    gaveUp.givenUp = {1498};
    Check(JudgeId(1498, gaveUp).verdict == Verdict::GivenUp, "a quest given up twice: no bonus");
    BotFacts low = Bot(WARRIOR, ORC, 9, {1505});
    Check(JudgeId(1498, low).verdict == Verdict::LowLevel, "below the quest's level: no bonus");
    Check(JudgeId(1498, Bot(WARRIOR, HUMAN, 10, {1505})).verdict == Verdict::NotForBot, "another race: no bonus");
    Check(JudgeId(1498, Bot(PALADIN, ORC, 10, {1505})).verdict == Verdict::NotForBot, "another class: no bonus");

    using Planner::GoalApply::Governed;
    Check(Governed(true, 21, false), "a persistent planner bot (guid 21) is governed");
    Check(!Governed(true, 20, false), "a weighted-group bot (guid 20) is not governed");
    Check(!Governed(true, 21, true), "a bot with a real master is not governed");
    Check(!Governed(false, 21, false), "a bot that keeps no possessions is not governed");
    BotFacts ungoverned = warrior;
    ungoverned.governed = false;
    Check(JudgeId(1498, ungoverned).verdict == Verdict::NotGoverned && Bonuses(ungoverned, noTrainer, anyRoute).empty(),
          "an ungoverned bot gets no bonus");

    auto const bonuses = Bonuses(warrior, noTrainer, anyRoute);
    Check(bonuses.size() == 1 && bonuses[0].first == 1498 && bonuses[0].second == BonusXp(warrior),
          "the orc warrior's bonuses: 1498 only");
    BotFacts const warlock = Bot(WARLOCK, UNDEAD, 30);
    Entry const* felhunter = NextStep(*Find(1795), warlock);
    Check(felhunter && felhunter->quest == 1801, "an undead warlock's chain to 1795 starts at the Horde 1801: " +
                                                     std::to_string(felhunter ? felhunter->quest : 0));
    Check(Protected({1498, 1505, 6082, 42}, warrior) == std::vector<std::uint32_t>{1498, 1505},
          "protected: the doable class quest and step in the log, not an objective quest or another quest");
}

// J3: bonus > 0 only for a governed bot, a doable entry on the chain of a doable class quest whose spells it does not
// know, not given up, with a route, takeable (class, race, level, chain). And every such quest gets it (J2's input)
void Invariant(std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<std::uint32_t> quests;
    std::vector<std::uint32_t> spells;
    for (Entry const& entry : TABLE)
    {
        quests.push_back(entry.quest);
        if (entry.learns)
            spells.push_back(entry.learns);
        if (entry.rewardSpell)
            spells.push_back(entry.rewardSpell);
    }
    std::uint32_t const races[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    int const BOTS = 20000;
    int bonuses = 0;
    std::string first;
    for (int n = 0; n < BOTS && first.empty(); ++n)
    {
        BotFacts bot;
        bot.cls = 1 + rng() % 11;
        bot.race = races[rng() % 10];
        bot.level = 1 + rng() % 80;
        bot.governed = rng() % 4 != 0;
        bot.levelXp = 1000 + rng() % 100000;
        for (std::uint32_t quest : quests)
        {
            if (rng() % 3 == 0)
                bot.rewarded.push_back(quest);
            if (rng() % 20 == 0)
                bot.givenUp.push_back(quest);
        }
        for (std::uint32_t spell : spells)
            if (rng() % 8 == 0)
                bot.knownSpells.push_back(spell);
        std::sort(bot.knownSpells.begin(), bot.knownSpells.end());
        bot.knownSpells.erase(std::unique(bot.knownSpells.begin(), bot.knownSpells.end()), bot.knownSpells.end());
        std::set<std::uint32_t> taught;
        std::set<std::uint32_t> routes;
        for (std::uint32_t spell : spells)
            if (rng() % 10 == 0)
                taught.insert(spell);
        for (std::uint32_t quest : quests)
            if (rng() % 6 != 0)
                routes.insert(quest);
        auto const teaches = [&](std::uint32_t spell) { return taught.count(spell) > 0; };
        auto const route = [&](std::uint32_t quest) { return routes.count(quest) > 0; };

        // The oracle: the quests that should get the bonus, from the definitions directly
        std::set<std::uint32_t> expected;
        if (bot.governed)
            for (Entry const& top : TABLE)
            {
                if (!top.rewardSpell || !Fits(top, bot) || top.cause != Cause::Value || taught.count(top.learns) ||
                    Has(bot.knownSpells, top.learns) || Has(bot.knownSpells, top.rewardSpell) ||
                    Has(bot.givenUp, top.quest))
                    continue;
                Entry const* step = &top;
                bool done = false;
                for (int depth = 0; depth < MAX_CHAIN && !done; ++depth)
                {
                    if (Has(bot.rewarded, step->quest))
                    {
                        step = nullptr;
                        break;
                    }
                    bool const open = (!step->prev[0] && !step->prev[1]) || Has(bot.rewarded, step->prev[0]) ||
                                      Has(bot.rewarded, step->prev[1]);
                    if (open)
                        break;
                    Entry const* back = nullptr;
                    for (std::uint32_t prev : step->prev)
                        if (!back && prev && Find(prev) && Fits(*Find(prev), bot))
                            back = Find(prev);
                    if (!back)
                        done = true;
                    else
                        step = back;
                }
                if (!step || done)
                    continue;
                if (step->cause == Cause::Value && !Has(bot.givenUp, step->quest) && bot.level >= step->minLevel &&
                    route(step->quest) && Fits(*step, bot))
                    expected.insert(step->quest);
            }
        std::set<std::uint32_t> got;
        for (auto const& [quest, bonus] : Bonuses(bot, teaches, route))
        {
            got.insert(quest);
            Entry const* entry = Find(quest);
            bool const sane = bonus > 0.0f && bot.governed && entry && entry->cause == Cause::Value &&
                              !Has(bot.givenUp, quest) && !Has(bot.rewarded, quest) && route(quest) &&
                              bot.level >= entry->minLevel && Fits(*entry, bot) && Unlocked(*entry, bot) &&
                              !(entry->learns && Has(bot.knownSpells, entry->learns));
            if (!sane && first.empty())
                first = "bot " + std::to_string(n) + ": quest " + std::to_string(quest) + " got a bonus it must not";
        }
        bonuses += static_cast<int>(got.size());
        if (got != expected && first.empty())
            first = "bot " + std::to_string(n) + " (class " + std::to_string(bot.cls) + ", race " +
                    std::to_string(bot.race) + ", level " + std::to_string(bot.level) +
                    "): " + std::to_string(got.size()) + " bonuses, the definition gives " +
                    std::to_string(expected.size());
    }
    std::printf("info: invariant J3 seed %u, %d bots, %d bonuses\n", seed, BOTS, bonuses);
    Check(first.empty() && bonuses > 0, "J3: the bonus only and always where the rule says" +
                                            (first.empty() ? std::string() : ": first counterexample " + first));
}

// ---- The planner wiring (WU20a-2): a synthetic hub planned end to end by Goals::PlanGoals ----
namespace G = Planner::Goals;
namespace GH = Planner::GoalHarness;

constexpr std::uint32_t HUB_GIVER = 93001, FAR_GIVER = 93002, CLASS_QUEST = 1498, CLASS_PREV = 1505;
constexpr std::uint32_t GATE = 93011, GATED = 93010, ORDINARY = 95001, ORDINARY_COUNT = 5;
constexpr float FAR_YARDS = 1600.0f;

// A hub with five kill quests near its giver; Path of Defense (1498) at the hub or 1600 yards away; GATED opened only
// by GATE naming it as its NextQuestId (the core's prevQuests, the 1795 gap)
Planner::QuestIndex World(bool classAtHub, Planner::Snapshot const& bot)
{
    Planner::QuestGather gather;
    Planner::Place const& at = bot.position;
    float const hx = at.x, hy = at.y + 5.0f;
    float const cx = classAtHub ? hx : hx + FAR_YARDS, cy = hy;
    gather.places.push_back({at.map, hx, hy, 0.0f, HUB_GIVER, false, bot.zone, bot.area, Planner::NO_CAMP});
    if (!classAtHub)
        gather.places.push_back({at.map, cx, cy, 0.0f, FAR_GIVER, false, bot.zone, bot.area, Planner::NO_CAMP});
    std::uint32_t target = 94001;
    auto const add = [&](std::uint32_t id, std::uint32_t xp, std::uint32_t giver, float x, float y)
    {
        Planner::QuestRecord quest;
        quest.id = id;
        quest.level = 10;
        quest.minLevel = 8;
        quest.xpBase = xp;
        quest.objectives.push_back({Planner::ObjectiveKind::Kill, target, 6u, 0, {}});
        for (float const offset : {-8.0f, 0.0f, 8.0f})
            gather.targets.push_back({target, false, at.map, x + offset, y + offset, 0.0f});
        gather.relations.push_back({id, giver, false, false});
        gather.relations.push_back({id, giver, false, true});
        gather.quests.push_back(std::move(quest));
        ++target;
    };
    for (std::uint32_t i = 0; i < ORDINARY_COUNT; ++i)
        add(ORDINARY + i, 450 + 50 * i, HUB_GIVER, hx + 60.0f + 25.0f * i, hy - 40.0f + 20.0f * i);
    add(CLASS_QUEST, 550, classAtHub ? HUB_GIVER : FAR_GIVER, cx + 70.0f, cy + 30.0f);
    gather.quests.back().minLevel = 10;
    gather.quests.back().classMask = 1;
    gather.quests.back().raceMask = 690;
    gather.quests.back().prevAny = {static_cast<std::int32_t>(CLASS_PREV)};
    add(GATE, 400, HUB_GIVER, hx - 60.0f, hy + 50.0f);
    add(GATED, 500, HUB_GIVER, hx - 70.0f, hy - 50.0f);
    gather.quests.back().prevAny = {static_cast<std::int32_t>(GATE)};
    return Planner::QuestIndex::Build(std::move(gather));
}

Planner::Snapshot Orc()
{
    Planner::Snapshot bot = GH::Bot(10, 5000);
    bot.race = 2;
    bot.cls = 1;
    bot.team = 1;
    bot.rewardedQuests = {CLASS_PREV};
    return bot;
}

G::Situation GovernedAt()
{
    G::Situation situation = GH::Base();
    situation.governed = true;
    situation.levelXp = 7600;
    return situation;
}

std::string Ranked(G::GoalPlan const& plan)
{
    std::string takes;
    for (Planner::CircuitTake const& take : plan.circuit.take)
        takes += " " + std::to_string(take.quest) + (take.classQuest ? "(class_quest)" : "");
    return "[" + GH::Summary(plan.ranked) + "] takes:" + (takes.empty() ? " none" : takes);
}

bool TakesClass(G::GoalPlan const& plan, std::uint32_t quest)
{
    return std::any_of(plan.circuit.take.begin(), plan.circuit.take.end(),
                       [quest](Planner::CircuitTake const& take) { return take.quest == quest && take.classQuest; });
}

bool HasClassOption(G::Ranked const& ranked)
{
    return std::any_of(ranked.options.begin(), ranked.options.end(),
                       [](G::Option const& option) { return option.reason == REASON_WORD; });
}

bool TopClassTrip(G::Ranked const& ranked)
{
    G::Option const* top = GH::Top(ranked);
    return top && top->reason == REASON_WORD && top->kind == G::GoalKind::Quest &&
           top->targetType == G::TargetType::Camp;
}

bool TopCircuitTaking(G::GoalPlan const& plan, std::uint32_t quest)
{
    G::Option const* top = GH::Top(plan.ranked);
    return top && top->reason == G::CIRCUIT_REASON && TakesClass(plan, quest);
}

G::ReplanInput Interval(G::CurrentGoal const& current, G::GoalPlan const& plan)
{
    G::ReplanInput in;
    in.current = current;
    in.fresh = &plan.ranked;
    in.plain = plan.plain ? &*plan.plain : nullptr;
    in.nowMs = G::REPLAN_INTERVAL_MS + 10;
    in.lastPlanMs = 1;
    return in;
}

std::string Verdict(G::Replan const& replan)
{
    return std::string(G::ReplanWord(replan.verdict)) + " " + replan.reason +
           (replan.chosen ? " " + G::ChoiceWord(*replan.chosen) + " " + replan.chosen->reason : std::string());
}

void Wiring()
{
    Planner::Snapshot const bot = Orc();
    Planner::QuestIndex const far = World(false, bot);
    Planner::QuestIndex const near = World(true, bot);
    Planner::CircuitContext farContext;
    farContext.quests = &far;
    Planner::CircuitContext nearContext;
    nearContext.quests = &near;

    // A warrior at 10 with 1498 takeable: the lone trip over the ordinary circuit
    G::GoalPlan const lone = G::PlanGoals(bot, farContext, GovernedAt());
    std::printf("info: warrior 10, giver 1600 yd off: %s\n", Ranked(lone).c_str());
    Check(TopClassTrip(lone.ranked) && GH::Find(lone.ranked, G::GoalKind::Quest, GH::HOME_HUB),
          "wiring: a governed warrior 10 picks the lone trip to 1498 over its ordinary circuit");

    // The giver at the hub: the circuit takes 1498 with the quests near it, no lone trip
    G::GoalPlan const circuit = G::PlanGoals(bot, nearContext, GovernedAt());
    std::printf("info: warrior 10, giver at the hub: %s\n", Ranked(circuit).c_str());
    Check(TopCircuitTaking(circuit, CLASS_QUEST) && !HasClassOption(circuit.ranked) &&
              Planner::CircuitLog::Rows(circuit.circuit).size() > 0,
          "wiring: a circuit that takes 1498 with the quests near it stands for the class quest");
    bool classRow = false;
    for (auto const& row : Planner::CircuitLog::Rows(circuit.circuit))
        classRow = classRow || (row.choice == "take:1498" && row.reason == std::string(REASON_WORD));
    Check(classRow, "wiring: the circuit row of 1498 has the reason class_quest");

    // Mid-circuit: the circuit goes on (the plain plan keeps it), the trip comes when the circuit ends
    Planner::Snapshot running = bot;
    running.currentGoal = {static_cast<std::uint32_t>(G::GoalKind::Quest), GH::HOME_HUB, 1};
    G::GoalPlan const mid = G::PlanGoals(running, farContext, GovernedAt());
    G::CurrentGoal current{G::GoalKind::Quest, GH::HOME_HUB, 0.0f, false, false, G::TargetType::Default};
    G::Replan const stays = G::ReplanDecision(Interval(current, mid));
    G::ReplanInput unguarded = Interval(current, mid);
    unguarded.plain = nullptr;
    G::Replan const bare = G::ReplanDecision(unguarded);
    std::printf("info: mid-circuit: %s; with the plain plan: %s; without it: %s\n", Ranked(mid).c_str(),
                Verdict(stays).c_str(), Verdict(bare).c_str());
    Check(mid.plain.has_value() && stays.verdict == G::ReplanVerdict::Stay,
          "wiring: a bot mid-circuit finishes its circuit first (" + Verdict(stays) + ")");
    current.ended = true;
    G::Replan const after = G::ReplanDecision(Interval(current, mid));
    Check(after.verdict == G::ReplanVerdict::Switch && after.chosen && after.chosen->reason == REASON_WORD,
          "wiring: when the circuit ends the next decision takes the class trip (" + Verdict(after) + ")");

    // A bridge goal outranks the class quest; a sick bot rests first
    G::Situation bridged = GovernedAt();
    bridged.external = G::External{G::GoalKind::TravelElsewhere, 7, bot.position, 30.0f, "external"};
    G::GoalPlan const bridge = G::PlanGoals(bot, farContext, bridged);
    std::printf("info: bridge goal: %s\n", Ranked(bridge).c_str());
    Check(GH::Top(bridge.ranked) && GH::Top(bridge.ranked)->pinned, "wiring: a bridge goal outranks the class quest");
    Planner::Snapshot sick = bot;
    sick.sickMs = 60000;
    G::GoalPlan const resting = G::PlanGoals(sick, farContext, GovernedAt());
    std::printf("info: sick: %s\n", Ranked(resting).c_str());
    Check(GH::Top(resting.ranked) && GH::Top(resting.ranked)->kind == G::GoalKind::Rest &&
              !HasClassOption(resting.ranked),
          "wiring: a sick bot rests first");

    // No bonus: not governed, the spell known, given up
    G::GoalPlan const ungoverned = G::PlanGoals(bot, nearContext, GH::Base());
    G::Situation known = GovernedAt();
    known.knownClassQuestSpells = {71};
    G::GoalPlan const knows = G::PlanGoals(bot, farContext, known);
    G::Situation givenUp = GovernedAt();
    givenUp.givenUpQuests = {CLASS_QUEST};
    G::GoalPlan const gaveUp = G::PlanGoals(bot, farContext, givenUp);
    std::printf("info: ungoverned at the hub: %s\n", Ranked(ungoverned).c_str());
    Check(ungoverned.classBonus.empty() && !TakesClass(ungoverned, CLASS_QUEST) && knows.classBonus.empty() &&
              !HasClassOption(knows.ranked) && gaveUp.classBonus.empty() && !HasClassOption(gaveUp.ranked),
          "wiring: an ungoverned bot, a known spell and a given-up quest get no bonus and no trip");
    Check(!Planner::GoalApply::Governed(true, 20, false) && !Planner::GoalApply::Governed(true, 21, true) &&
              !Planner::GoalApply::Governed(false, 21, false) && Planner::GoalApply::Governed(true, 21, false),
          "wiring: a weighted-group bot and a bot with a real master are not governed");

    // The 1795 gap: a quest opened only by another's NextQuestId waits for that one
    Planner::Snapshot opened = bot;
    opened.rewardedQuests = {CLASS_PREV, GATE};
    std::sort(opened.rewardedQuests.begin(), opened.rewardedQuests.end());
    G::GoalPlan const before = G::PlanGoals(bot, nearContext, GH::Base());
    G::GoalPlan const afterGate = G::PlanGoals(opened, nearContext, GH::Base());
    std::vector<std::uint32_t> const& unlocks = near.Unlocks(GATE);
    Check(!GH::Takes(before.circuit, GATED) && GH::Takes(before.circuit, GATE) && GH::Takes(afterGate.circuit, GATED) &&
              std::find(unlocks.begin(), unlocks.end(), GATED) != unlocks.end(),
          "gap: a quest gated by another's NextQuestId is taken only after that one is rewarded");
}

// J1 and J2 over generated quest logs and planner states, each planned by PlanGoals on the two hubs
void Decisions(std::uint32_t seed)
{
    std::mt19937 rng(seed + 1);
    Planner::Snapshot const base = Orc();
    Planner::QuestIndex const worlds[2] = {World(false, base), World(true, base)};
    int const STATES = 3000;
    int j1 = 0, j1Held = 0, j1Leaves = 0, j2 = 0, j2Trips = 0, j2Circuits = 0;
    std::string first1, first2;
    for (int n = 0; n < STATES; ++n)
    {
        bool const atHub = rng() % 2;
        Planner::CircuitContext context;
        context.quests = &worlds[atHub ? 1 : 0];
        Planner::Snapshot bot = base;
        bot.level = static_cast<std::uint8_t>(9 + rng() % 4);
        bot.race = rng() % 4 == 0 ? 1 : 2;
        bot.cls = rng() % 5 == 0 ? 2 : 1;
        bot.money = rng() % 20000;
        bot.rewardedQuests.clear();
        if (rng() % 5 != 0)
            bot.rewardedQuests.push_back(CLASS_PREV);
        bool const classDone = rng() % 10 == 0;
        if (classDone)
            bot.rewardedQuests.push_back(CLASS_QUEST);
        for (std::uint32_t quest = ORDINARY; quest < ORDINARY + ORDINARY_COUNT + 1; ++quest)
        {
            std::uint32_t const id = quest == ORDINARY + ORDINARY_COUNT ? GATE : quest;
            std::uint32_t const roll = rng() % 4;
            if (roll == 0)
                bot.rewardedQuests.push_back(id);
            else if (roll == 1)
                bot.quests.push_back({id, 0, {}});
        }
        bool const classInLog = !classDone && rng() % 10 == 0;
        if (classInLog)
            bot.quests.push_back({CLASS_QUEST, 0, {}});
        std::sort(bot.rewardedQuests.begin(), bot.rewardedQuests.end());
        bot.sickMs = rng() % 8 == 0 ? 60000 : 0;
        G::Situation situation = GH::Base();
        situation.governed = rng() % 5 != 0;
        situation.levelXp = 7000 + rng() % 3000;
        if (rng() % 6 == 0)
            situation.knownClassQuestSpells = {71};
        if (rng() % 10 == 0)
            situation.givenUpQuests = {CLASS_QUEST};
        situation.healthPercent = rng() % 5 == 0 ? static_cast<float>(20 + rng() % 60) : 100.0f;
        if (rng() % 2)
            situation.spots = {GH::Spot(21, static_cast<float>(40 + rng() % 200), 5.0f)};
        if (rng() % 2)
            situation.hubs = {
                GH::Hub(31, bot.zone, static_cast<float>(50 + rng() % 400), static_cast<float>(2 + rng() % 20))};
        int const goal = static_cast<int>(rng() % 4);  // none, the circuit, a move to hub 31, grinding spot 21
        G::CurrentGoal current;
        if (goal == 1)
            current = {G::GoalKind::Quest, GH::HOME_HUB, 0.0f, false, false, G::TargetType::Default};
        else if (goal == 2)
        {
            if (situation.hubs.empty())
                situation.hubs = {GH::Hub(31, bot.zone, 200.0f, 10.0f)};
            current = {G::GoalKind::Quest, 31, 0.0f, false, true, G::TargetType::Default};
        }
        else if (goal == 3)
        {
            if (situation.spots.empty())
                situation.spots = {GH::Spot(21, 120.0f, 5.0f)};
            current = {G::GoalKind::Grind, 21, 0.0f, false, false, G::TargetType::Default};
        }
        current.valuePerMinute = static_cast<float>(rng() % 600);
        if (goal)
            bot.currentGoal = {static_cast<std::uint32_t>(current.kind), current.target, 1};
        G::GoalPlan const plan = G::PlanGoals(bot, context, situation);
        std::string const state = "state " + std::to_string(n) + " (class " + std::to_string(bot.cls) + ", race " +
                                  std::to_string(bot.race) + ", level " + std::to_string(bot.level) +
                                  (atHub ? ", giver at the hub" : ", giver far") + ", goal " + std::to_string(goal) +
                                  ")";

        if (goal)
        {
            // J1: what the plan without the bonus keeps, the bot keeps
            G::Situation plainSituation = situation;
            plainSituation.governed = false;
            G::GoalPlan const without = G::PlanGoals(bot, context, plainSituation);
            G::ReplanInput in = Interval(current, plan);
            if (rng() % 3 == 0)
                in.lastPlanMs = in.nowMs - 1000;  // no interval trigger
            in.delta.situation = rng() % 4 == 0;
            G::Replan const with = G::ReplanDecision(in);
            G::ReplanInput plainIn = in;
            plainIn.fresh = &without.ranked;
            plainIn.plain = nullptr;
            G::Replan const reference = G::ReplanDecision(plainIn);
            G::ReplanInput bare = in;
            bare.plain = nullptr;
            G::Replan const unguarded = G::ReplanDecision(bare);
            ++j1;
            if (reference.verdict == G::ReplanVerdict::Stay && with.verdict != G::ReplanVerdict::Stay && first1.empty())
                first1 = state + ": without the bonus " + Verdict(reference) + ", with it " + Verdict(with) + "; " +
                         Ranked(plan);
            if (reference.verdict == G::ReplanVerdict::Stay && unguarded.verdict != G::ReplanVerdict::Stay)
                ++j1Held;
            if (with.verdict != G::ReplanVerdict::Stay)
                ++j1Leaves;
            continue;
        }

        // J2: a governed bot without the spells, with 1498 takeable and doable, not sick, at full health, no goal
        bool const prev = std::binary_search(bot.rewardedQuests.begin(), bot.rewardedQuests.end(), CLASS_PREV);
        bool const takeable = situation.governed && bot.cls == 1 && bot.race == 2 && bot.level >= 10 && prev &&
                              !classDone && !classInLog && situation.knownClassQuestSpells.empty() &&
                              situation.givenUpQuests.empty();
        if (!takeable || bot.sickMs || situation.healthPercent < 100.0f)
            continue;
        ++j2;
        bool const bonus = !plan.classBonus.empty() && plan.classBonus.front().first == CLASS_QUEST;
        bool const trip = TopClassTrip(plan.ranked);
        bool const taking = TopCircuitTaking(plan, CLASS_QUEST);
        j2Trips += trip ? 1 : 0;
        j2Circuits += taking ? 1 : 0;
        if (!(bonus && (trip || taking)) && first2.empty())
            first2 = state + ": " + Ranked(plan);
    }
    std::printf(
        "info: invariant J1/J2 seed %u, %d states: J1 %d with a running goal (%d leave, %d kept only by the "
        "plain plan), J2 %d (%d lone trips, %d circuits)\n",
        seed + 1, STATES, j1, j1Leaves, j1Held, j2, j2Trips, j2Circuits);
    Check(first1.empty() && j1 > 0, "J1: the bonus never ends a running trip or circuit" +
                                        (first1.empty() ? std::string() : ": first counterexample " + first1));
    Check(first2.empty() && j2 > 0 && j2Trips > 0 && j2Circuits > 0,
          "J2: a governed bot with a takeable class quest picks it or a circuit that takes it" +
              (first2.empty() ? std::string() : ": first counterexample " + first2));
}
}  // namespace

int main(int argc, char** argv)
{
    std::uint32_t const seed = argc > 1 ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 20261114u;
    Table();
    Scenarios();
    Invariant(seed);
    Wiring();
    Decisions(seed);
    std::printf("class quest selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
