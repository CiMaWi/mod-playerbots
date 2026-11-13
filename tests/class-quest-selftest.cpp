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
}  // namespace

int main(int argc, char** argv)
{
    std::uint32_t const seed = argc > 1 ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 20261114u;
    Table();
    Scenarios();
    Invariant(seed);
    std::printf("class quest selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
