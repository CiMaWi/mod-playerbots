/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Item use plan WI13: the spend rule (PLAN 3.6, ItemSpendRules.h). A hand fixture per invariant S1-S9, the danger's
// own properties, the one potion of a fight; a seeded search over random situations, items, prices and traits that
// counts the violations of each invariant; the printed verdicts for sample bots and fights, each item weighed.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "ItemSpendRules.h"

using namespace ItemUse;

namespace
{
int passed = 0;
int failed = 0;
constexpr float AREA_SAMPLE_MINUTES = 0.05f;  // the sample bomb's fight time saved per enemy it hits (an input)

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << "\n";
    }
}

bool SameVerdict(SpendVerdict const& a, SpendVerdict const& b)
{
    return a.spend == b.spend && a.reason == b.reason && a.savesCopper == b.savesCopper &&
           a.costCopper == b.costCopper && a.danger == b.danger;
}

NearUnit Enemy(std::uint8_t level, Rank rank, float healthPct)
{
    NearUnit unit;
    unit.level = level;
    unit.rank = rank;
    unit.healthPct = healthPct;
    unit.inFight = true;
    unit.onBot = true;
    return unit;
}

Situation Fight(std::uint8_t level, float healthPct, float lossPctPerSecond, std::vector<NearUnit> const& enemies)
{
    Situation situation;
    situation.level = level;
    situation.healthPct = healthPct;
    situation.maxHealth = 10000;
    situation.healthLossPerSecond = lossPctPerSecond * 100.0f;  // percent of 10000 per second
    situation.inFight = true;
    for (NearUnit const& unit : enemies)
        if (situation.unitCount < MAX_NEAR)
            situation.units[situation.unitCount++] = unit;
    return situation;
}

SpendCase BaseCase(Kind kind, std::uint32_t replace, float danger, std::uint32_t deathCopper)
{
    SpendCase spendCase;
    spendCase.kind = kind;
    spendCase.rank = 100;
    spendCase.replaceCopper = replace;
    spendCase.danger = danger;
    spendCase.deathCopper = deathCopper;
    spendCase.earnCopperPerMinute = 100;
    return spendCase;
}

std::string Money(float copper)
{
    std::int64_t const whole = static_cast<std::int64_t>(std::llround(std::max(copper, 0.0f)));
    std::string out;
    if (whole >= 10000)
        out += std::to_string(whole / 10000) + "g ";
    if (whole >= 100)
        out += std::to_string(whole / 100 % 100) + "s ";
    return out + std::to_string(whole % 100) + "c";
}

void HandFixtures()
{
    // Danger: no fight, the fight ended, full health against nothing
    Situation calm;
    Check(Danger(calm) == 0.0f, "danger: not in a fight is 0");
    Situation ended = Fight(20, 50.0f, 1.0f, {Enemy(20, Rank::Normal, 100.0f)});
    ended.fightEnded = true;
    Check(Danger(ended) == 0.0f, "danger: an ended fight is 0");
    Check(Danger(Fight(20, 100.0f, 0.0f, {})) == 0.0f, "danger: a fight with no enemy is 0");
    Check(Danger(Fight(20, 0.0f, 0.0f, {Enemy(20, Rank::Normal, 100.0f)})) == 1.0f, "danger: no health left is 1");

    // Danger grows with enemies, with their strength and with the bot's lost health
    float const one = Danger(Fight(20, 100.0f, 0.0f, {Enemy(20, Rank::Normal, 100.0f)}));
    float const three = Danger(
        Fight(20, 100.0f, 0.0f,
              {Enemy(20, Rank::Normal, 100.0f), Enemy(20, Rank::Normal, 100.0f), Enemy(20, Rank::Normal, 100.0f)}));
    float const elite = Danger(Fight(20, 100.0f, 0.0f, {Enemy(20, Rank::Elite, 100.0f)}));
    float const grey = Danger(Fight(20, 100.0f, 0.0f, {Enemy(10, Rank::Normal, 100.0f)}));
    float const hurt = Danger(Fight(20, 30.0f, 0.0f, {Enemy(20, Rank::Normal, 100.0f)}));
    Check(one > 0.0f && one < three, "danger: three enemies are more dangerous than one");
    Check(one < elite, "danger: an elite is more dangerous than a normal enemy");
    Check(grey < one, "danger: a grey enemy is less dangerous than one of the bot's level");
    Check(hurt > one, "danger: the same fight is more dangerous with less health");
    Situation controlled = Fight(20, 100.0f, 0.0f, {Enemy(20, Rank::Normal, 100.0f)});
    controlled.units[0].controlled = true;
    Check(Danger(controlled) == 0.0f, "danger: a controlled enemy does not hurt (no loss measured)");
    Situation group = Fight(20, 100.0f, 0.0f, {Enemy(20, Rank::Elite, 100.0f)});
    group.groupSize = 5;
    Check(Danger(group) < elite, "danger: a group kills the enemy sooner");
    Situation broken = Fight(20, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             {Enemy(20, Rank::Normal, std::numeric_limits<float>::quiet_NaN())});
    broken.maxHealth = 0;
    float const brokenDanger = Danger(broken);
    Check(std::isfinite(brokenDanger) && brokenDanger >= 0.0f && brokenDanger <= 1.0f, "danger: finite for any input");

    std::uint32_t const death = DeathCopper(100, 200);
    Check(death == 700, "death cost: the corpse run at the earning rate plus the repair bill");

    // S1 dearer_first: at one danger the cheaper item of a kind spends where the dearer keeps
    SpendCase cheap = BaseCase(Kind::Heal, 100, 0.5f, death);
    SpendCase dear = cheap;
    dear.replaceCopper = 5000;
    Check(Spend(cheap).spend && !Spend(dear).spend, "S1 dearer_first: the cheap potion spends, the dear one keeps");
    dear.danger = 0.3f;
    Check(!Spend(dear).spend, "S1 dearer_first: the dear item at less danger keeps");

    // S2 danger_keeps: more danger keeps a spend a spend
    SpendCase low = BaseCase(Kind::Heal, 300, 0.6f, death);
    SpendCase high = low;
    high.danger = 0.9f;
    Check(Spend(low).spend && Spend(high).spend, "S2 danger_keeps: more danger still spends");
    low.danger = 0.2f;
    Check(!Spend(low).spend && Spend(high).spend, "S2 danger_keeps: a keep at low danger is a spend at high");

    // S3 thrift_spends: the thrifty bot keeps where the spendthrift spends
    SpendCase spendthrift = BaseCase(Kind::Heal, 300, 0.6f, death);
    spendthrift.thrift = -1.0f;
    SpendCase thrifty = spendthrift;
    thrifty.thrift = 1.0f;
    Check(Spend(spendthrift).spend && !Spend(thrifty).spend, "S3 thrift_spends: thrift raises the bar");
    SpendCase daring = BaseCase(Kind::Heal, 300, 0.6f, death);
    daring.daring = 1.0f;
    SpendCase careful = daring;
    careful.daring = -1.0f;
    Check(!Spend(daring).spend && Spend(careful).spend, "daring: a daring bot holds a death risk lighter");

    // S4 no_danger_spend
    SpendCase calmCase = BaseCase(Kind::DamageArea, 0, 0.0f, death);
    calmCase.minutesSaved = 5.0f;
    calmCase.hits = 3;
    SpendVerdict const calmVerdict = Spend(calmCase);
    Check(!calmVerdict.spend && calmVerdict.reason == SpendReason::NoDanger, "S4 no_danger_spend: a free bomb keeps");

    // S5 free_loses, S7 won_spend, S8 control_spend, S9 second_potion: each gate keeps a sure spend
    SpendCase sure = BaseCase(Kind::Heal, 1, 0.9f, death);
    Check(Spend(sure).spend && Spend(sure).reason == SpendReason::Death, "the sure case spends for a death");
    SpendCase gate = sure;
    gate.freeReadySameNeed = true;
    Check(!Spend(gate).spend && Spend(gate).reason == SpendReason::FreeReady, "S5 free_loses");
    gate = sure;
    gate.won = true;
    Check(!Spend(gate).spend && Spend(gate).reason == SpendReason::Won, "S7 won_spend");
    gate = sure;
    gate.control = true;
    Check(!Spend(gate).spend && Spend(gate).reason == SpendReason::Control, "S8 control_spend");
    gate = sure;
    gate.potion = true;
    gate.potionDrunk = true;
    Check(!Spend(gate).spend && Spend(gate).reason == SpendReason::PotionKept, "S9 second_potion");
    gate.potionDrunk = false;
    Check(Spend(gate).spend, "S9: the first potion of the fight spends");

    // S6 not_same: the same input, the same verdict
    Check(SameVerdict(Spend(sure), Spend(sure)), "S6 not_same: two runs agree");

    // The stake and time reasons
    SpendCase stake = BaseCase(Kind::Heal, 300, 0.4f, death);
    stake.stake = Stake::Player;
    SpendCase plain = stake;
    plain.stake = Stake::Ordinary;
    Check(Spend(stake).spend && Spend(stake).reason == SpendReason::StakeLost && !Spend(plain).spend,
          "stake: a fight against a player is worth more");
    SpendCase timeCase = BaseCase(Kind::DamageArea, 50, 0.05f, death);
    timeCase.minutesSaved = 2.0f;
    timeCase.hits = 3;
    Check(Spend(timeCase).spend && Spend(timeCase).reason == SpendReason::Time, "time: a fight that ends sooner");
    SpendCase noSave = BaseCase(Kind::Learn, 0, 0.9f, death);
    Check(!Spend(noSave).spend && Spend(noSave).reason == SpendReason::NotWorth, "a kind that saves nothing keeps");
    SpendCase missed = BaseCase(Kind::DamageArea, 0, 0.9f, death);
    missed.hits = 0;
    Check(!Spend(missed).spend, "an area item that covers no enemy saves nothing");

    // One potion per fight: a mana potion with a smaller save does not take the healing potion's turn
    std::vector<SpendCase> needs;
    SpendCase mana = BaseCase(Kind::Mana, 120, 0.7f, death);
    mana.potion = true;
    SpendCase heal = BaseCase(Kind::Heal, 100, 0.7f, death);
    heal.potion = true;
    needs.push_back(mana);
    needs.push_back(heal);
    Check(Spend(mana).spend && Spend(heal).spend, "both potions would spend on their own");
    Check(BestPotionNeed(needs) == 1, "one potion: the heal that saves more takes the turn");
    needs[1].potionDrunk = true;
    needs[0].potionDrunk = true;
    Check(BestPotionNeed(needs) == needs.size(), "one potion: none after the fight's potion");
    std::vector<SpendCase> none;
    Check(BestPotionNeed(none) == 0, "one potion: an empty list has no need");

    // Words and the row's decision
    Check(SpendReasonWord(SpendReason::Death) == "death" && SpendReasonWord(SpendReason::Control) == "control",
          "reason words");
    SpendCase kept = sure;
    kept.control = true;
    Check(
        SpendDecision(Kind::Heal, Spend(sure)) == "spend:heal" && SpendDecision(Kind::Heal, Spend(kept)) == "keep:heal",
        "decision words");
}

struct Search
{
    std::uint64_t cases = 0;
    std::uint64_t spent = 0;
    std::uint64_t kept = 0;
    std::uint64_t s1 = 0;
    std::uint64_t s2 = 0;
    std::uint64_t s3 = 0;
    std::uint64_t s4 = 0;
    std::uint64_t s5 = 0;
    std::uint64_t s6 = 0;
    std::uint64_t s7 = 0;
    std::uint64_t s8 = 0;
    std::uint64_t s9 = 0;
    std::uint64_t dangerBad = 0;       // a danger outside 0..1 or not finite
    std::uint64_t dangerMore = 0;      // one more enemy lowered the danger
    std::uint64_t dangerHurt = 0;      // less health lowered the danger
    std::uint64_t dangerSame = 0;      // two runs differed
    std::uint64_t dangerPositive = 0;  // a fight with some danger (the search is not empty)
};

void RunSearch(std::uint32_t seed, std::uint32_t count)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_real_distribution<float> sign(-1.0f, 1.0f);
    auto chance = [&](float share) { return unit(rng) < share; };
    auto pick = [&](std::uint32_t top) { return static_cast<std::uint32_t>(unit(rng) * static_cast<float>(top)); };
    Search found;

    for (std::uint32_t i = 0; i < count; ++i)
    {
        // A random situation and its danger
        Situation situation;
        situation.level = static_cast<std::uint8_t>(1 + pick(80));
        situation.inFight = !chance(0.05f);
        situation.fightEnded = chance(0.03f);
        situation.healthPct = chance(0.02f) ? std::numeric_limits<float>::quiet_NaN() : unit(rng) * 100.0f;
        situation.maxHealth = chance(0.05f) ? 0 : 100 + pick(40000);
        situation.healthLossPerSecond = chance(0.3f) ? 0.0f : unit(rng) * 2000.0f;
        situation.groupSize = static_cast<std::uint8_t>(chance(0.7f) ? 1 : 1 + pick(5));
        situation.unitCount = static_cast<std::uint8_t>(pick(MAX_NEAR));
        for (std::uint8_t u = 0; u < situation.unitCount; ++u)
        {
            NearUnit& near = situation.units[u];
            int const offset = static_cast<int>(pick(25)) - 12;
            near.level = static_cast<std::uint8_t>(std::clamp(static_cast<int>(situation.level) + offset, 1, 83));
            near.rank = static_cast<Rank>(pick(6));
            near.healthPct = unit(rng) * 100.0f;
            near.inFight = !chance(0.1f);
            near.onBot = chance(0.7f);
            near.controlled = chance(0.1f);
            near.flees = chance(0.05f);
            near.friendly = chance(0.1f);
        }
        float const danger = Danger(situation);
        if (!std::isfinite(danger) || danger < 0.0f || danger > 1.0f)
            ++found.dangerBad;
        if (Danger(situation) != danger)
            ++found.dangerSame;
        if (danger > 0.0f)
            ++found.dangerPositive;
        if (situation.unitCount < MAX_NEAR)
        {
            Situation more = situation;
            more.units[more.unitCount] = Enemy(situation.level, static_cast<Rank>(pick(6)), unit(rng) * 100.0f);
            ++more.unitCount;
            if (Danger(more) < danger)
                ++found.dangerMore;
        }
        if (std::isfinite(situation.healthPct))
        {
            Situation hurt = situation;
            hurt.healthPct = situation.healthPct * unit(rng);
            if (Danger(hurt) < danger)
                ++found.dangerHurt;
        }

        // A random item weighed in it
        SpendCase spendCase;
        spendCase.kind = static_cast<Kind>(pick(static_cast<std::uint32_t>(Kind::Count)));
        spendCase.rank = static_cast<std::int32_t>(pick(5000));
        spendCase.replaceCopper = chance(0.1f) ? 0 : pick(chance(0.5f) ? 1000 : 60000);
        spendCase.potion = chance(0.3f);
        spendCase.potionDrunk = chance(0.2f);
        spendCase.freeReadySameNeed = chance(0.1f);
        spendCase.danger = chance(0.1f) ? 0.0f : (chance(0.5f) ? danger : unit(rng));
        spendCase.minutesSaved = chance(0.6f) ? 0.0f : unit(rng) * 3.0f;
        spendCase.hits = static_cast<std::uint8_t>(pick(6));
        spendCase.stake = static_cast<Stake>(pick(3));
        spendCase.won = chance(0.1f);
        spendCase.earnCopperPerMinute = pick(3000);
        spendCase.deathCopper = DeathCopper(spendCase.earnCopperPerMinute, pick(50000));
        spendCase.thrift = sign(rng);
        spendCase.daring = sign(rng);
        spendCase.control = chance(0.05f);

        SpendVerdict const verdict = Spend(spendCase);
        ++found.cases;
        if (verdict.spend)
            ++found.spent;
        else
            ++found.kept;
        if (!SameVerdict(verdict, Spend(spendCase)))
            ++found.s6;
        if (verdict.spend && !(spendCase.danger > 0.0f))
            ++found.s4;
        if (verdict.spend && spendCase.freeReadySameNeed)
            ++found.s5;
        if (verdict.spend && spendCase.won)
            ++found.s7;
        if (verdict.spend && spendCase.control)
            ++found.s8;
        if (verdict.spend && spendCase.potion && spendCase.potionDrunk)
            ++found.s9;

        // S1: the same kind and rank, dearer and at less danger, spends while this one keeps
        SpendCase dearer = spendCase;
        dearer.replaceCopper = spendCase.replaceCopper + 1 + pick(20000);
        dearer.danger = spendCase.danger * unit(rng);
        if (Spend(dearer).spend && !verdict.spend)
            ++found.s1;
        // S2: more danger turns this spend into a keep
        SpendCase riskier = spendCase;
        riskier.danger = spendCase.danger + (1.0f - spendCase.danger) * unit(rng);
        if (verdict.spend && !Spend(riskier).spend)
            ++found.s2;
        // S3: a thriftier bot spends where this one keeps
        SpendCase thriftier = spendCase;
        thriftier.thrift = spendCase.thrift + (1.0f - spendCase.thrift) * unit(rng);
        if (Spend(thriftier).spend && !verdict.spend)
            ++found.s3;
    }

    std::printf("search seed %u: %llu cases, %llu spent, %llu kept, %llu with danger\n", seed,
                static_cast<unsigned long long>(found.cases), static_cast<unsigned long long>(found.spent),
                static_cast<unsigned long long>(found.kept), static_cast<unsigned long long>(found.dangerPositive));
    std::printf(
        "violations: S1 dearer_first %llu, S2 danger_keeps %llu, S3 thrift_spends %llu, S4 no_danger_spend %llu, "
        "S5 free_loses %llu, S6 not_same %llu, S7 won_spend %llu, S8 control_spend %llu, S9 second_potion %llu\n",
        static_cast<unsigned long long>(found.s1), static_cast<unsigned long long>(found.s2),
        static_cast<unsigned long long>(found.s3), static_cast<unsigned long long>(found.s4),
        static_cast<unsigned long long>(found.s5), static_cast<unsigned long long>(found.s6),
        static_cast<unsigned long long>(found.s7), static_cast<unsigned long long>(found.s8),
        static_cast<unsigned long long>(found.s9));
    std::printf("danger: %llu out of range, %llu lowered by one more enemy, %llu lowered by less health, %llu differ\n",
                static_cast<unsigned long long>(found.dangerBad), static_cast<unsigned long long>(found.dangerMore),
                static_cast<unsigned long long>(found.dangerHurt), static_cast<unsigned long long>(found.dangerSame));
    Check(found.cases > 0 && found.spent > 0 && found.kept > 0 && found.dangerPositive > 0, "the search is not empty");
    Check(found.s1 == 0, "S1 dearer_first");
    Check(found.s2 == 0, "S2 danger_keeps");
    Check(found.s3 == 0, "S3 thrift_spends");
    Check(found.s4 == 0, "S4 no_danger_spend");
    Check(found.s5 == 0, "S5 free_loses");
    Check(found.s6 == 0, "S6 not_same");
    Check(found.s7 == 0, "S7 won_spend");
    Check(found.s8 == 0, "S8 control_spend");
    Check(found.s9 == 0, "S9 second_potion");
    Check(found.dangerBad == 0 && found.dangerMore == 0 && found.dangerHurt == 0 && found.dangerSame == 0,
          "danger: in range, grows with enemies and lost health, the same each run");
}

// The printed verdicts: sample bots and fights, each item weighed (names and prices from the committed dump,
// tests/fixtures/item-use-dump.txt, replaceCopper column; a healthstone has no price, so its worth to keep is
// passed instead, here the dear healing potion's price it stands in for)
struct SampleItem
{
    char const* name;
    Kind kind;
    std::uint32_t replaceCopper;
    bool potion;
    bool area;
};

struct SampleBot
{
    std::uint8_t level;
    std::uint32_t repairCopper;  // a sample repair bill of one death
    std::vector<SampleItem> items;
};

struct SampleFight
{
    char const* name;
    Situation situation;
    Stake stake;
    bool won;
};

void PrintVerdicts()
{
    std::vector<SampleBot> const bots = {{20,
                                          200,
                                          {{"Lesser Healing Potion", Kind::Heal, 100, true, false},
                                           {"Healing Potion", Kind::Heal, 300, true, false},
                                           {"Lesser Mana Potion", Kind::Mana, 120, true, false},
                                           {"Heavy Wool Bandage", Kind::Heal, 228, false, false},
                                           {"Large Copper Bomb", Kind::DamageArea, 700, false, true},
                                           {"Lesser Healthstone", Kind::Heal, 300, false, false},
                                           {"Scroll of Agility", Kind::Boost, 200, false, false}}},
                                         {70,
                                          30000,
                                          {{"Major Healing Potion", Kind::Heal, 4000, true, false},
                                           {"Super Healing Potion", Kind::Heal, 20000, true, false},
                                           {"Super Mana Potion", Kind::Mana, 20000, true, false},
                                           {"Heavy Netherweave Bandage", Kind::Heal, 12000, false, false},
                                           {"Fel Iron Bomb", Kind::DamageArea, 10000, false, true},
                                           {"Master Healthstone", Kind::Heal, 20000, false, false},
                                           {"Scroll of Agility V", Kind::Boost, 500, false, false}}}};

    for (SampleBot const& bot : bots)
    {
        std::uint8_t const level = bot.level;
        std::uint8_t const grey = static_cast<std::uint8_t>(level - 10);
        std::vector<SampleFight> fights = {
            {"grey_boar", Fight(level, 100.0f, 0.0f, {Enemy(grey, Rank::Normal, 100.0f)}), Stake::Ordinary, false},
            {"three_of_level",
             Fight(level, 100.0f, 0.0f,
                   {Enemy(level, Rank::Normal, 100.0f), Enemy(level, Rank::Normal, 100.0f),
                    Enemy(level, Rank::Normal, 100.0f)}),
             Stake::Ordinary, false},
            {"elite", Fight(level, 100.0f, 0.0f, {Enemy(level, Rank::Elite, 100.0f)}), Stake::Ordinary, false},
            {"player", Fight(level, 100.0f, 0.0f, {Enemy(level, Rank::Player, 100.0f)}), Stake::Player, false},
            {"nearly_won", Fight(level, 60.0f, 2.5f, {Enemy(level, Rank::Normal, 8.0f)}), Stake::Ordinary, false},
            {"nearly_lost", Fight(level, 15.0f, 5.0f, {Enemy(level, Rank::Normal, 60.0f)}), Stake::Ordinary, false}};
        std::uint32_t const earn = WorldTravel::DEFAULT_SLOT_BANDS[WorldTravel::LevelBand(level)].EarnCopperPerMinute;
        std::uint32_t const death = DeathCopper(earn, bot.repairCopper);
        std::printf("bot level %u caster: earns %s a minute (the level band's default), repair %s, a death costs %s\n",
                    level, Money(static_cast<float>(earn)).c_str(), Money(static_cast<float>(bot.repairCopper)).c_str(),
                    Money(static_cast<float>(death)).c_str());

        for (SampleFight const& fight : fights)
        {
            float const danger = Danger(fight.situation);
            std::uint8_t enemies = 0;
            for (std::uint8_t u = 0; u < fight.situation.unitCount; ++u)
                enemies = static_cast<std::uint8_t>(enemies + (fight.situation.units[u].friendly ? 0 : 1));
            std::printf("  fight=%s danger=%.3f\n", fight.name, danger);
            auto weigh = [&](SampleItem const& item, char const* note, SpendCase spendCase)
            {
                SpendVerdict const verdict = Spend(spendCase);
                std::printf("    item=%s (%s)%s replace=%s saves=%s cost=%s -> %s %s\n", item.name,
                            std::string(KindWord(item.kind)).c_str(), note,
                            Money(static_cast<float>(item.replaceCopper)).c_str(), Money(verdict.savesCopper).c_str(),
                            Money(verdict.costCopper).c_str(), verdict.spend ? "spend" : "keep",
                            std::string(SpendReasonWord(verdict.reason)).c_str());
            };
            for (SampleItem const& item : bot.items)
            {
                SpendCase spendCase;
                spendCase.kind = item.kind;
                spendCase.rank = 1;
                spendCase.replaceCopper = item.replaceCopper;
                spendCase.potion = item.potion;
                spendCase.danger = danger;
                spendCase.hits = item.area ? enemies : 1;
                spendCase.minutesSaved = item.area ? AREA_SAMPLE_MINUTES * static_cast<float>(enemies) : 0.0f;
                spendCase.stake = fight.stake;
                spendCase.won = fight.won;
                spendCase.deathCopper = death;
                spendCase.earnCopperPerMinute = earn;
                weigh(item, "", spendCase);
            }
            if (std::string(fight.name) == "nearly_lost")
            {
                SampleItem const& potion = bot.items[1];
                SpendCase spendCase;
                spendCase.kind = potion.kind;
                spendCase.replaceCopper = potion.replaceCopper;
                spendCase.potion = true;
                spendCase.danger = danger;
                spendCase.deathCopper = death;
                spendCase.earnCopperPerMinute = earn;
                SpendCase variant = spendCase;
                variant.freeReadySameNeed = true;
                weigh(potion, " [a free heal is ready]", variant);
                variant = spendCase;
                variant.potionDrunk = true;
                weigh(potion, " [a potion was drunk this fight]", variant);
                variant = spendCase;
                variant.control = true;
                weigh(potion, " [control cohort]", variant);
                variant = spendCase;
                variant.thrift = 1.0f;
                weigh(potion, " [thrift 1]", variant);
                variant = spendCase;
                variant.daring = 1.0f;
                weigh(potion, " [daring 1]", variant);
            }
            if (std::string(fight.name) == "nearly_won")
            {
                SampleItem const& potion = bot.items[0];
                SpendCase spendCase;
                spendCase.kind = potion.kind;
                spendCase.replaceCopper = potion.replaceCopper;
                spendCase.potion = true;
                spendCase.danger = danger;
                spendCase.won = true;
                spendCase.deathCopper = death;
                spendCase.earnCopperPerMinute = earn;
                weigh(potion, " [the fight is won]", spendCase);
            }
        }
    }
}
}  // namespace

int main()
{
    HandFixtures();
    RunSearch(20261117u, 200000u);
    PrintVerdicts();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
