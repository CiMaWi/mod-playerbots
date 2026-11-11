/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the world PvP fight rules (src/Ai/World/Pvp/FightRules.h): the colours against a copy of the
// core's formulas, the start rate and its cap, every refusal of a start and a join, the objective exception, the soft
// factors' directions, joining, retreat, the chase, the timeouts, the calm area, the ghost's wait, the avoid time and
// the grudges; tests/fight-encounter-selftest.cpp is the encounter harness. Build (Git Bash, module root):
//   clang-cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I src/Ai/World/Pvp tests/fight-rules-selftest.cpp
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <type_traits>

#include "FightRules.h"

using WorldPvp::ChaseLimit;
using WorldPvp::Colour;
using WorldPvp::EndCause;
using WorldPvp::EndedFight;
using WorldPvp::FightClock;
using WorldPvp::FightReason;
using WorldPvp::FightRuleValues;
using WorldPvp::GhostStep;
using WorldPvp::JoinInput;
using WorldPvp::JoinWhy;
using WorldPvp::Pick;
using WorldPvp::Refusal;
using WorldPvp::StartBot;
using WorldPvp::StartDecision;
using WorldPvp::StartTarget;

static_assert(std::is_trivially_copyable_v<StartBot> && std::is_trivially_copyable_v<StartTarget> &&
                  std::is_trivially_copyable_v<JoinInput> && std::is_trivially_copyable_v<FightRuleValues>,
              "rule inputs are plain values");

namespace
{
constexpr uint32_t SECOND = 1000;
constexpr uint32_t MINUTE = 60 * SECOND;
constexpr uint32_t HOUR = 60 * MINUTE;
constexpr float EPSILON = 1.0e-4f;

int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

bool Near(float a, float b, float tolerance = EPSILON) { return std::fabs(a - b) <= tolerance; }

// A copy of the core's colour, uint8 arithmetic included: azerothcore-wotlk src/server/game/Miscellaneous/Formulas.h
// lines 46-80, Acore::XP::GetGrayLevel and GetColorCode
uint8_t ReferenceGreyLevel(uint8_t plLevel)
{
    uint8_t level;
    if (plLevel <= 5)
        level = 0;
    else if (plLevel <= 39)
        level = plLevel - 5 - plLevel / 10;
    else if (plLevel <= 59)
        level = plLevel - 1 - plLevel / 5;
    else
        level = plLevel - 9;
    return level;
}

Colour ReferenceColour(uint8_t plLevel, uint8_t mobLevel)
{
    if (mobLevel >= plLevel + 5)
        return Colour::Red;
    if (mobLevel >= plLevel + 3)
        return Colour::Orange;
    if (mobLevel >= plLevel - 2)
        return Colour::Yellow;
    if (mobLevel > ReferenceGreyLevel(plLevel))
        return Colour::Green;
    return Colour::Grey;
}

// A bot and a target that pass every hard rule: an aggression-0 bot an hour after its last chance, a full-health
// yellow target at even odds
StartBot ReadyBot()
{
    StartBot bot;
    bot.sinceChanceMs = HOUR;
    return bot;
}

StartTarget ReadyTarget() { return StartTarget{}; }

void CheckColours()
{
    uint32_t mismatches = 0;
    for (int level : {10, 30, 50, 60, 80})
        for (int gap = -15; gap <= 15; ++gap)
        {
            int const target = level + gap;
            if (target < 1)
                continue;
            if (WorldPvp::ColourOf(level, target) !=
                ReferenceColour(static_cast<uint8_t>(level), static_cast<uint8_t>(target)))
                ++mismatches;
        }
    Check(mismatches == 0, "colours match the core at levels 10, 30, 50, 60, 80 and gaps -15..+15");

    uint32_t gridMismatches = 0;
    for (int level = 1; level <= 80; ++level)
        for (int target = 1; target <= 100; ++target)
            if (WorldPvp::ColourOf(level, target) !=
                ReferenceColour(static_cast<uint8_t>(level), static_cast<uint8_t>(target)))
                ++gridMismatches;
    Check(gridMismatches == 0, "colours match the core for every level 1-80 against 1-100");

    // NOTES Facts: at 80 grey <= 71, green 72-77, yellow 78-82; at 30 grey <= 22, green 23-27, yellow 28-32, orange
    // 33-34, red 35+
    Check(WorldPvp::ColourOf(80, 71) == Colour::Grey && WorldPvp::ColourOf(80, 72) == Colour::Green &&
              WorldPvp::ColourOf(80, 77) == Colour::Green && WorldPvp::ColourOf(80, 78) == Colour::Yellow &&
              WorldPvp::ColourOf(80, 82) == Colour::Yellow,
          "level 80's bands");
    Check(WorldPvp::ColourOf(30, 22) == Colour::Grey && WorldPvp::ColourOf(30, 23) == Colour::Green &&
              WorldPvp::ColourOf(30, 28) == Colour::Yellow && WorldPvp::ColourOf(30, 33) == Colour::Orange &&
              WorldPvp::ColourOf(30, 34) == Colour::Orange && WorldPvp::ColourOf(30, 35) == Colour::Red,
          "level 30's bands");

    Check(!WorldPvp::MayPick(Colour::Grey, Pick::Start) && !WorldPvp::MayPick(Colour::Red, Pick::Start) &&
              WorldPvp::MayPick(Colour::Green, Pick::Start) && WorldPvp::MayPick(Colour::Orange, Pick::Start),
          "a start never picks grey or red");
    Check(WorldPvp::MayPick(Colour::Grey, Pick::Join) && !WorldPvp::MayPick(Colour::Red, Pick::Join),
          "a join may pick grey but not red");
    Check(WorldPvp::MayPick(Colour::Red, Pick::Defence) && WorldPvp::MayPick(Colour::Red, Pick::Group),
          "defence and the group's fight pick red");
}

void CheckRate()
{
    // The curve's arithmetic at the plan's 4 h (the default rate is tuned below it, see WorldPvpConfig.h)
    WorldPvpConfig plan;
    plan.HoursAtZero = 4.0f;
    plan.HoursAtOne = 1.0f;
    FightRuleValues const curve(plan);
    Check(WorldPvp::StartRate(-1.0f, curve) == 0.0f && WorldPvp::StartRate(-0.51f, curve) == 0.0f &&
              WorldPvp::StartRate(-0.5f, curve) == 0.0f,
          "no rate at or below -0.5");
    Check(Near(WorldPvp::StartRate(-0.25f, curve), 0.125f), "linear between -0.5 and 0");
    Check(Near(WorldPvp::StartRate(0.0f, curve), 0.25f), "once per 4 h at 0");
    Check(Near(WorldPvp::StartRate(0.5f, curve), 0.5f), "4^a per 4 h above 0");
    Check(Near(WorldPvp::StartRate(1.0f, curve), 1.0f), "once per hour at +1, four times the rate at 0");

    FightRuleValues const values;
    Check(Near(WorldPvp::StartRate(0.0f, values), 1.0f / WorldPvpConfig{}.HoursAtZero) &&
              Near(WorldPvp::StartRate(1.0f, values), 1.0f / WorldPvpConfig{}.HoursAtOne),
          "the defaults' rate at 0 and +1");
    Check(Near(WorldPvp::StartRate(1.0f, values) / WorldPvp::StartRate(0.0f, values), 4.0f, 1.0e-4f) &&
              Near(WorldPvp::StartRate(0.5f, values) / WorldPvp::StartRate(0.0f, values), 2.0f, 1.0e-4f),
          "the defaults' budget is 4^a times the rate at 0 (WP.16)");

    bool rising = true;
    for (int step = -100; step < 100; ++step)
        if (WorldPvp::StartRate(static_cast<float>(step + 1) / 100.0f, values) <
            WorldPvp::StartRate(static_cast<float>(step) / 100.0f, values))
            rising = false;
    Check(rising, "the rate never falls with aggression");

    // Many small chances add up to the rate: 24 chances 10 min apart over 4 h at the plan's rate leave exp(-1) chance
    // of no start, as one draw over 4 h would
    float noStart = 1.0f;
    float const rate = WorldPvp::StartRate(0.0f, curve);
    for (int chance = 0; chance < 24; ++chance)
        noStart *= 1.0f - WorldPvp::StartProbability(rate, 10 * MINUTE, values);
    Check(Near(noStart, std::exp(-1.0f), 1.0e-3f), "busy-zone chances add up to the rate");

    // After a long quiet time the first chance is at most the cap, at any aggression
    bool capped = true;
    for (int step = -100; step <= 100; step += 5)
    {
        StartBot bot = ReadyBot();
        bot.aggression = static_cast<float>(step) / 100.0f;
        bot.sinceChanceMs = 12 * HOUR;
        StartDecision const decision = WorldPvp::DecideStart(bot, ReadyTarget(), 0.99f, values);
        if (decision.p > 1.0f / 3.0f + EPSILON)
            capped = false;
    }
    Check(capped, "after 12 h without a chance the first one is at most 1/3");
    Check(Near(WorldPvp::StartProbability(1.0f, 12 * HOUR, values), 1.0f / 3.0f), "the cap holds at +1");
    Check(Near(WorldPvp::StartProbability(0.25f, 6 * MINUTE, values), 1.0f - std::exp(-0.025f)),
          "a short gap gives 1 - exp(-rate dt)");

    StartBot odd = ReadyBot();
    odd.aggression = std::nanf("");
    odd.sinceChanceMs = 12 * HOUR;
    StartDecision const nan = WorldPvp::DecideStart(odd, ReadyTarget(), 0.0f, values);
    Check(WorldPvp::StartRate(odd.aggression, values) == 0.0f && !nan.start && nan.refusal == Refusal::Peaceable,
          "an aggression that is not a number never starts");
}

void CheckStartRefusals()
{
    FightRuleValues const values;
    auto refusal = [&values](StartBot const& bot, StartTarget const& target)
    { return WorldPvp::DecideStart(bot, target, 0.0f, values).refusal; };

    StartDecision const ready = WorldPvp::DecideStart(ReadyBot(), ReadyTarget(), 0.0f, values);
    Check(ready.refusal == Refusal::None && ready.chance && ready.start && ready.reason == FightReason::Budget,
          "the ready meeting starts on a roll of 0");
    StartDecision const lost = WorldPvp::DecideStart(ReadyBot(), ReadyTarget(), 0.999f, values);
    Check(lost.refusal == Refusal::Roll && lost.chance && !lost.start, "a lost roll is still a chance");

    StartBot bot = ReadyBot();
    bot.openWorld = false;
    Check(refusal(bot, ReadyTarget()) == Refusal::NotOpenWorld, "not in the open world");
    bot = ReadyBot();
    bot.realPlayer = true;
    Check(refusal(bot, ReadyTarget()) == Refusal::RealPlayer, "a real player in the group or a real master");
    bot = ReadyBot();
    bot.groupMember = true;
    Check(refusal(bot, ReadyTarget()) == Refusal::NotLeader, "a member leaves it to the leader");
    bot = ReadyBot();
    bot.inFight = true;
    Check(refusal(bot, ReadyTarget()) == Refusal::InFight, "already in a fight");
    bot = ReadyBot();
    bot.graveyardYards = 80.0f;
    Check(refusal(bot, ReadyTarget()) == Refusal::Graveyard, "the bot within 80 yd of a graveyard");
    bot = ReadyBot();
    bot.aggression = -0.6f;
    Check(refusal(bot, ReadyTarget()) == Refusal::Peaceable, "a peaceable bot");
    bot = ReadyBot();
    bot.sinceFightEndMs = 29 * MINUTE;
    Check(refusal(bot, ReadyTarget()) == Refusal::Gap, "within the minimum gap");
    bot.sinceFightEndMs = 30 * MINUTE;
    Check(refusal(bot, ReadyTarget()) == Refusal::None, "the gap is over at 30 min");
    bot = ReadyBot();
    bot.inCalmArea = true;
    Check(refusal(bot, ReadyTarget()) == Refusal::Calm, "in a calm area");

    StartTarget target = ReadyTarget();
    target.enemy = false;
    Check(refusal(ReadyBot(), target) == Refusal::NotTarget, "not an attackable flagged enemy in sight");
    target = ReadyTarget();
    target.colour = Colour::Grey;
    Check(refusal(ReadyBot(), target) == Refusal::Grey, "grey");
    target.colour = Colour::Red;
    Check(refusal(ReadyBot(), target) == Refusal::Red, "red");
    target = ReadyTarget();
    target.sinceResurrectionMs = 4 * MINUTE;
    Check(refusal(ReadyBot(), target) == Refusal::Resurrected, "resurrected 4 min ago");
    target.sinceResurrectionMs = 5 * MINUTE;
    Check(refusal(ReadyBot(), target) == Refusal::None, "resurrected 5 min ago");
    target = ReadyTarget();
    target.graveyardYards = 50.0f;
    Check(refusal(ReadyBot(), target) == Refusal::Graveyard, "the target near a graveyard");
    target = ReadyTarget();
    target.sinceRolledMs = 9 * MINUTE;
    StartDecision const again = WorldPvp::DecideStart(ReadyBot(), target, 0.0f, values);
    Check(again.refusal == Refusal::RolledRecently && !again.chance, "rolled for 9 min ago: no chance");

    // A grudge raises the chance but no hard rule
    target = ReadyTarget();
    target.grudge = 1.0f;
    StartDecision const grudge = WorldPvp::DecideStart(ReadyBot(), target, 0.999f, values);
    StartDecision const plain = WorldPvp::DecideStart(ReadyBot(), ReadyTarget(), 0.999f, values);
    Check(Near(grudge.p, plain.p * 3.0f) && grudge.reason == FightReason::Grudge, "a full grudge triples p");
    target.colour = Colour::Grey;
    Check(refusal(ReadyBot(), target) == Refusal::Grey, "a grudge never starts on grey");
    target.colour = Colour::Yellow;
    target.sinceResurrectionMs = MINUTE;
    Check(refusal(ReadyBot(), target) == Refusal::Resurrected, "a grudge never camps");
    target.sinceResurrectionMs = WorldPvp::NEVER_MS;
    target.colour = Colour::Red;
    Check(refusal(ReadyBot(), target) == Refusal::Red, "a grudge never starts on red");
    target.colour = Colour::Yellow;
    bot = ReadyBot();
    bot.sinceFightEndMs = MINUTE;
    Check(target.grudge > 0.0f && refusal(bot, target) == Refusal::Gap, "a grudge keeps the gap");
    bot = ReadyBot();
    bot.sinceChanceMs = 12 * HOUR;
    target = ReadyTarget();
    target.grudge = 1.5f;
    float const strongest = WorldPvp::DecideStart(bot, target, 0.0f, values).p;
    Check(strongest <= 1.0f && strongest > 1.0f / 3.0f, "the cap is on the base: a grudge may lift p above 1/3");
}

void CheckObjective()
{
    FightRuleValues const values;
    StartBot bot = ReadyBot();
    bot.atObjective = true;
    bot.aggression = -1.0f;
    bot.sinceFightEndMs = MINUTE;
    bot.inCalmArea = true;
    StartTarget target = ReadyTarget();
    target.contestsObjective = true;
    target.sinceRolledMs = MINUTE;
    StartDecision const decision = WorldPvp::DecideStart(bot, target, 0.999f, values);
    Check(decision.start && !decision.chance && decision.reason == FightReason::Objective,
          "an objective skips the budget, the gap, the calm area and the roll");
    Check(WorldPvp::StartGate(bot, values) == Refusal::None, "a bot at an objective scans");

    target.colour = Colour::Grey;
    Check(WorldPvp::DecideStart(bot, target, 0.0f, values).refusal == Refusal::Grey, "an objective keeps grey");
    target.colour = Colour::Red;
    Check(WorldPvp::DecideStart(bot, target, 0.0f, values).refusal == Refusal::Red, "an objective keeps red");
    target.colour = Colour::Yellow;
    target.sinceResurrectionMs = MINUTE;
    Check(WorldPvp::DecideStart(bot, target, 0.0f, values).refusal == Refusal::Resurrected,
          "an objective keeps the camping rule");
    target.sinceResurrectionMs = WorldPvp::NEVER_MS;
    target.graveyardYards = 10.0f;
    Check(WorldPvp::DecideStart(bot, target, 0.0f, values).refusal == Refusal::Graveyard,
          "an objective keeps the graveyard rule");

    // An enemy that does not contest it meets the bot's budget rules again
    target = ReadyTarget();
    Check(WorldPvp::DecideStart(bot, target, 0.0f, values).refusal == Refusal::Peaceable,
          "a non-contesting enemy at an objective follows the budget");
}

void CheckSoftFactors()
{
    FightRuleValues const values;
    Check(WorldPvp::ColourFactor(Colour::Green, 0.0f, values) == 0.5f &&
              WorldPvp::ColourFactor(Colour::Yellow, 0.0f, values) == 1.0f &&
              WorldPvp::ColourFactor(Colour::Grey, -1.0f, values) == 0.0f &&
              WorldPvp::ColourFactor(Colour::Red, -1.0f, values) == 0.0f,
          "green 0.5, yellow 1, grey and red 0");
    Check(Near(WorldPvp::ColourFactor(Colour::Orange, -1.0f, values), 1.0f) &&
              Near(WorldPvp::ColourFactor(Colour::Orange, 0.0f, values), 0.5f) &&
              Near(WorldPvp::ColourFactor(Colour::Orange, 1.0f, values), 0.0f),
          "orange from 1 for the most daring to 0 for the most careful");

    Check(WorldPvp::CourtesyFactor(false, 1.0f, 1.0f, values) == 1.0f, "an idle healthy target counts fully");
    Check(Near(WorldPvp::CourtesyFactor(true, 1.0f, -1.0f, values), 1.0f) &&
              Near(WorldPvp::CourtesyFactor(true, 1.0f, 1.0f, values), 0.0f) &&
              Near(WorldPvp::CourtesyFactor(false, 0.4f, 0.0f, values), 0.5f),
          "a busy or weakened target counts for the selfish, not for the courteous");
    FightRuleValues rude = values;
    rude.courtesy = false;
    Check(WorldPvp::CourtesyFactor(true, 0.1f, 1.0f, rude) == 1.0f, "Courtesy off ignores it");

    Check(Near(WorldPvp::OddsFactor(0.5f, 0.0f, values), 1.0f), "even odds are full for a neutral bot");
    Check(WorldPvp::OddsFactor(0.5f, 1.0f, values) <= EPSILON && WorldPvp::OddsFactor(0.65f, 1.0f, values) == 1.0f,
          "the most careful bot needs the odds in its favour");
    Check(WorldPvp::OddsFactor(0.33f, -1.0f, values) > 0.5f, "the most daring bot starts outnumbered two to one");
    bool ordered = true;
    for (int odds = 0; odds <= 20; ++odds)
        for (int caution = -10; caution < 10; ++caution)
        {
            float const share = static_cast<float>(odds) / 20.0f;
            float const c = static_cast<float>(caution) / 10.0f;
            if (WorldPvp::OddsFactor(share, c + 0.1f, values) > WorldPvp::OddsFactor(share, c, values) ||
                (odds < 20 && WorldPvp::OddsFactor(share + 0.05f, c, values) < WorldPvp::OddsFactor(share, c, values)))
                ordered = false;
        }
    Check(ordered, "caution lowers the odds factor, better odds raise it");

    float const own = WorldPvp::Strength(60, 60, 1.0f, values);
    float const pair = own + WorldPvp::Strength(60, 58, 0.5f, values);
    Check(Near(own, 1.0f) && WorldPvp::Strength(60, 62, 1.0f, values) > own &&
              Near(WorldPvp::OddsShare(own, own), 0.5f) && WorldPvp::OddsShare(pair, own) > 0.5f &&
              WorldPvp::OddsShare(0.0f, 0.0f) == 0.5f,
          "strength by health and level; odds share");

    Check(Near(WorldPvp::GrudgeChanceFactor(0.0f, values), 1.0f) &&
              Near(WorldPvp::GrudgeChanceFactor(1.0f, values), 3.0f),
          "grudge factor 1 without, 3 at full strength");
}

void CheckJoin()
{
    FightRuleValues const values;
    JoinInput ready;
    WorldPvp::JoinDecision const chance = WorldPvp::DecideJoin(ready, 0.0f, values);
    Check(chance.join && chance.why == JoinWhy::Chance && Near(chance.p, 0.4f), "a neutral bystander joins at 0.4");
    Check(WorldPvp::DecideJoin(ready, 0.999f, values).refusal == Refusal::Roll, "a lost join roll");

    JoinInput input = ready;
    input.alive = false;
    input.attacked = true;
    Check(WorldPvp::DecideJoin(input, 0.0f, values).refusal == Refusal::Dead, "the dead do not join");
    input = ready;
    input.attacked = true;
    input.yards = 200.0f;
    input.pickable = 0;
    input.aggression = -1.0f;
    Check(WorldPvp::DecideJoin(input, 0.999f, values).why == JoinWhy::Defence, "an attacked bot always defends");
    input = ready;
    input.groupFight = true;
    input.pickable = 0;
    Check(WorldPvp::DecideJoin(input, 0.999f, values).why == JoinWhy::Group, "members join their group's fight");
    input = ready;
    input.masterFight = true;
    Check(WorldPvp::DecideJoin(input, 0.999f, values).why == JoinWhy::Master, "a bot joins its real master's fight");

    input = ready;
    input.inOtherFight = true;
    Check(WorldPvp::DecideJoin(input, 0.0f, values).refusal == Refusal::InFight, "in another fight");
    input = ready;
    input.visible = false;
    Check(WorldPvp::DecideJoin(input, 0.0f, values).refusal == Refusal::Unseen, "cannot see the fight");
    input = ready;
    input.yards = 61.0f;
    Check(WorldPvp::DecideJoin(input, 0.0f, values).refusal == Refusal::TooFar, "beyond 60 yd");
    input = ready;
    input.ownSide = false;
    Check(WorldPvp::DecideJoin(input, 0.0f, values).refusal == Refusal::NotOwnSide, "its faction is not in it");
    input = ready;
    input.pickable = 0;
    Check(WorldPvp::DecideJoin(input, 0.0f, values).refusal == Refusal::AllRed, "every enemy red: stays out");

    auto p = [&values](JoinInput const& in) { return WorldPvp::JoinChance(in, values); };
    JoinInput low = ready;
    low.aggression = -1.0f;
    JoinInput high = ready;
    high.aggression = 1.0f;
    Check(p(low) < p(ready) && p(ready) < p(high), "aggression raises the join chance");

    JoinInput losing = ready;
    losing.odds = 0.3f;
    JoinInput helpful = losing;
    helpful.helpfulness = 1.0f;
    JoinInput targeted = ready;
    targeted.factionTargeted = true;
    targeted.helpfulness = 1.0f;
    JoinInput winningHelpful = ready;
    winningHelpful.helpfulness = 1.0f;
    winningHelpful.odds = 0.7f;
    Check(p(helpful) > p(losing) && p(targeted) > p(ready) && Near(p(winningHelpful), p(ready)),
          "helpfulness raises it when losing or a player of its faction is the target, not otherwise");

    JoinInput careful = losing;
    careful.caution = 1.0f;
    JoinInput daring = losing;
    daring.caution = -1.0f;
    JoinInput carefulWinning = ready;
    carefulWinning.caution = 1.0f;
    carefulWinning.odds = 0.7f;
    Check(p(careful) < p(losing) && Near(p(daring), p(ready)) && Near(p(carefulWinning), p(ready)),
          "caution lowers it at bad odds only");

    JoinInput busy = ready;
    busy.goalValue = 1.0f;
    Check(p(busy) < p(ready), "a valuable goal lowers it");
}

void CheckEnding()
{
    FightRuleValues const values;
    Check(WorldPvp::ShouldRetreat(0.2f, 0.3f, 0.0f, values), "low health and losing: retreat");
    Check(!WorldPvp::ShouldRetreat(0.8f, 0.1f, 0.0f, values), "high health: stay");
    Check(!WorldPvp::ShouldRetreat(0.1f, 0.7f, 0.0f, values), "winning: stay");
    Check(WorldPvp::ShouldRetreat(0.4f, 0.45f, 1.0f, values) && !WorldPvp::ShouldRetreat(0.4f, 0.45f, -1.0f, values),
          "careful bots retreat earlier");
    Check(!WorldPvp::ShouldRetreat(0.05f, 0.55f, 1.0f, values) && !WorldPvp::ShouldRetreat(0.05f, 0.5f, 1.0f, values),
          "nobody retreats while its side is not losing");
    Check(WorldPvp::ShouldRetreat(0.1f, 0.45f, 0.0f, values) && !WorldPvp::ShouldRetreat(0.1f, 0.45f, -1.0f, values),
          "daring bots stay until the odds are worse");

    ChaseLimit const neutral = WorldPvp::ChaseLimitFor(0.0f, 0.0f, values);
    ChaseLimit const keen = WorldPvp::ChaseLimitFor(1.0f, 0.0f, values);
    ChaseLimit const polite = WorldPvp::ChaseLimitFor(0.0f, 1.0f, values);
    ChaseLimit const least = WorldPvp::ChaseLimitFor(-1.0f, 1.0f, values);
    Check(neutral.ms == 15 * SECOND && Near(neutral.yards, 40.0f), "the neutral chase is 15 s or 40 yd");
    Check(keen.ms > neutral.ms && polite.ms < neutral.ms && least.ms > 0 && least.yards > 0.0f,
          "aggression lengthens the chase, courtesy shortens it");
    Check(!WorldPvp::ChaseOver(10 * SECOND, 30.0f, neutral) && WorldPvp::ChaseOver(15 * SECOND, 0.0f, neutral) &&
              WorldPvp::ChaseOver(0, 40.0f, neutral),
          "the chase ends at its time or distance");

    FightClock clock;
    clock.active = {1, 1};
    clock.ageMs = 20 * SECOND;
    clock.sinceHitMs = 5 * SECOND;
    Check(!WorldPvp::CheckFightEnd(clock, values).ends, "a busy fight goes on");
    clock.sinceHitMs = 30 * SECOND;
    clock.ageMs = 60 * SECOND;
    Check(WorldPvp::CheckFightEnd(clock, values).cause == EndCause::Timeout, "30 s without a hit: timeout");
    clock.sinceHitMs = SECOND;
    clock.ageMs = 5 * MINUTE;
    Check(WorldPvp::CheckFightEnd(clock, values).cause == EndCause::Stalemate, "5 min: stalemate");
    clock.ageMs = 6 * MINUTE;
    clock.sinceHitMs = 2 * MINUTE;
    Check(WorldPvp::CheckFightEnd(clock, values).cause == EndCause::Timeout, "both due: the timeout came first");
    clock.sinceHitMs = 40 * SECOND;
    Check(WorldPvp::CheckFightEnd(clock, values).cause == EndCause::Stalemate, "both due: the stalemate came first");
    clock.ageMs = 10 * SECOND;
    clock.sinceHitMs = 0;
    clock.active = {2, 0};
    Check(WorldPvp::CheckFightEnd(clock, values).cause == EndCause::Wiped, "one side out ends the fight");

    // Termination: at any hit pattern a fight ends by the stalemate time
    bool ends = true;
    for (uint32_t since = 0; since <= 10 * MINUTE; since += 7 * SECOND)
    {
        FightClock late;
        late.active = {3, 3};
        late.ageMs = 5 * MINUTE;
        late.sinceHitMs = since;
        if (!WorldPvp::CheckFightEnd(late, values).ends)
            ends = false;
    }
    Check(ends, "every fight ends by the stalemate time");

    EndedFight ended;
    ended.endMs = 10 * MINUTE;
    ended.x = 100.0f;
    ended.y = 100.0f;
    Check(WorldPvp::InCalmArea(ended, 250.0f, 200.0f, 20 * MINUTE, values), "within 200 yd and 15 min: calm");
    Check(!WorldPvp::InCalmArea(ended, 300.0f, 300.0f, 20 * MINUTE, values), "beyond 200 yd: not calm");
    Check(!WorldPvp::InCalmArea(ended, 100.0f, 100.0f, 25 * MINUTE, values), "after 15 min: not calm");
    std::deque<EndedFight> ring;
    EndedFight far = ended;
    far.x = 5000.0f;
    far.endMs = 15 * MINUTE;
    ring.push_back(ended);
    ring.push_back(far);
    Check(WorldPvp::InAnyCalmArea(ring, 150.0f, 150.0f, 20 * MINUTE, values) &&
              !WorldPvp::InAnyCalmArea(ring, 150.0f, 150.0f, 26 * MINUTE, values) &&
              !WorldPvp::InAnyCalmArea(ring, 2000.0f, 150.0f, 20 * MINUTE, values),
          "the ring's calm areas");
}

void CheckDeathAndGrudges()
{
    FightRuleValues const values;
    WorldPvp::GhostPlan wait = WorldPvp::PlanGhost(20.0f, MINUTE, values);
    Check(wait.step == GhostStep::Wait && wait.recheckMs == 15 * SECOND, "an enemy at the corpse: wait 15 s");
    wait = WorldPvp::PlanGhost(20.0f, 5 * MINUTE - 5 * SECOND, values);
    Check(wait.step == GhostStep::Wait && wait.recheckMs == 5 * SECOND, "the last wait ends at the healer's time");
    Check(WorldPvp::PlanGhost(41.0f, 2 * MINUTE, values).step == GhostStep::Corpse, "the enemy gone: to the corpse");
    Check(WorldPvp::PlanGhost(WorldPvp::FAR_YARDS, 0, values).step == GhostStep::Corpse, "no enemy: to the corpse");
    Check(WorldPvp::PlanGhost(20.0f, 5 * MINUTE, values).step == GhostStep::SpiritHealer,
          "after 5 min the spirit healer");

    Check(WorldPvp::AvoidMs(-1.0f, values) == 30 * MINUTE && WorldPvp::AvoidMs(1.0f, values) == 60 * MINUTE &&
              WorldPvp::AvoidMs(0.0f, values) == 45 * MINUTE,
          "avoid 30-60 min by caution");

    float const started = WorldPvp::GrudgeStrength(true, 0.0f, values);
    float const notStarted = WorldPvp::GrudgeStrength(false, 0.0f, values);
    float const hot = WorldPvp::GrudgeStrength(true, -1.0f, values);
    float const patient = WorldPvp::GrudgeStrength(true, 1.0f, values);
    Check(Near(started, 1.0f) && notStarted < started && hot > started && patient < started && patient > 0.0f,
          "grudges: stronger when the killer started, stronger for the hot-tempered");
    uint32_t const threeDays = 3 * 86400;
    Check(Near(WorldPvp::GrudgeFaded(1.0f, threeDays, values), 0.5f) &&
              Near(WorldPvp::GrudgeFaded(1.0f, 2 * threeDays, values), 0.25f),
          "a grudge halves every 3 days");
    Check(!WorldPvp::GrudgeForgotten(WorldPvp::GrudgeFaded(1.0f, 9 * 86400, values), values) &&
              WorldPvp::GrudgeForgotten(WorldPvp::GrudgeFaded(1.0f, 10 * 86400, values), values),
          "a full grudge is forgotten after about 10 days");

    std::array<float, 8> strengths = {0.9f, 0.4f, 1.2f, 0.7f, 0.6f, 0.0f, 0.0f, 0.0f};
    Check(WorldPvp::GrudgeSlot(strengths, 3, 0.1f, values) == 3, "a free slot below 5");
    Check(WorldPvp::GrudgeSlot(strengths, 5, 0.5f, values) == 1, "a stronger one replaces the weakest");
    Check(WorldPvp::GrudgeSlot(strengths, 5, 0.3f, values) == -1, "a weaker one is dropped");
}

void CheckConfig()
{
    // One source for the defaults: FightRuleValues() is WorldPvpConfig{}'s values in rule units
    WorldPvpConfig config;
    FightRuleValues const values;
    Check(values.rollAgainMs == config.RollAgainSeconds * SECOND && values.minGapMs == config.MinGapMinutes * MINUTE &&
              values.chanceCap == config.ChanceCap && values.joinBase == config.JoinBase &&
              values.oddsCaution == config.OddsCaution && values.retreatOdds == config.RetreatOdds &&
              values.retreatCautionHealth == config.RetreatCautionHealth &&
              Near(values.retreatHealth, static_cast<float>(config.RetreatHealthPercent) / 100.0f),
          "the rule values are the config's, in rule units");
    config.HoursAtZero = 2.0f;
    Check(Near(WorldPvp::StartRate(0.0f, FightRuleValues(config)), 0.5f), "HoursAtZero sets the rate");

    // Out-of-range options cannot break the rules' properties
    WorldPvpConfig wild;
    wild.YellowFactor = 2.0f;
    wild.OrangeDaring = 3.0f;
    wild.GreenFactor = -1.0f;
    wild.RetreatOdds = 0.8f;
    wild.RetreatCautionOdds = 0.4f;
    wild.JoinBase = 5.0f;
    wild.GrudgeFactor = std::nanf("");
    FightRuleValues const clamped(wild);
    Check(clamped.yellowFactor == 1.0f && clamped.orangeDaring == 1.0f && clamped.greenFactor == 0.0f &&
              clamped.joinBase == 1.0f && clamped.grudgeFactor == 1.0f,
          "colour factors, the join base and the grudge factor are clamped");
    StartBot bot = ReadyBot();
    bot.sinceChanceMs = 12 * HOUR;
    Check(WorldPvp::DecideStart(bot, ReadyTarget(), 0.99f, clamped).p <= 1.0f / 3.0f + EPSILON,
          "a configured yellow factor above 1 keeps the first chance at most 1/3");
    Check(!WorldPvp::ShouldRetreat(0.0f, 0.55f, 1.0f, clamped), "a configured retreat line stays below even odds");
    Check(WorldPvp::Word(Refusal::AllRed) == std::string("all_red") &&
              WorldPvp::Word(Refusal::RolledRecently) == std::string("rolled_recently") &&
              WorldPvp::Word(Colour::Orange) == std::string("orange"),
          "the words");
}
}  // namespace

int main()
{
    CheckColours();
    CheckRate();
    CheckStartRefusals();
    CheckObjective();
    CheckSoftFactors();
    CheckJoin();
    CheckEnding();
    CheckDeathAndGrudges();
    CheckConfig();
    std::cout << "fight-rules-selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
