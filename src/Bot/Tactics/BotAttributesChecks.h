/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTATTRIBUTESCHECKS_H
#define PLAYERBOTS_BOTATTRIBUTESCHECKS_H

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "BotAttributes.h"
#include "TacticGate.h"
#include "TacticGateChecks.h"

// Pure checks of the attribute model (BotAttributes.h) on synthetic guids and skills; no bots, no config
template <typename Check, typename Say>
void RunAttributesChecks(Check&& check, Say&& say)
{
    auto const at = [](BotAttribute attribute) { return static_cast<uint32>(attribute); };

    constexpr uint64 FirstGuid = 1000;
    constexpr uint32 Guids = 10000;

    // The proposed model (G6a tunes it): its loadings, every spread 1 and the five linked pairs
    BotAttributeParams model;
    model.Loading = {0.5f, 0.8f, 0.6f, 0.5f, 0.8f, 0.8f, 0.6f, 0.6f, 0.0f, 0.0f, 0.0f, 0.7f};
    model.Spread.fill(1.0f);
    auto const link = [&at](AttributeMatrix& matrix, BotAttribute first, BotAttribute second, double value)
    {
        matrix[at(first)][at(second)] = value;
        matrix[at(second)][at(first)] = value;
    };
    link(model.Correlation, BotAttribute::Wisdom, BotAttribute::Teamwork, 0.3);
    link(model.Correlation, BotAttribute::Wisdom, BotAttribute::Patience, 0.3);
    link(model.Correlation, BotAttribute::Speed, BotAttribute::Dexterity, 0.3);
    link(model.Correlation, BotAttribute::Focus, BotAttribute::Discipline, 0.3);
    link(model.Correlation, BotAttribute::Aggression, BotAttribute::Caution, -0.2);

    // The factor of a positive definite matrix reproduces it
    bool const factored = FactorAttributeMatrix(model.Correlation, model.Factor);
    double worstProduct = 0.0;
    for (uint32 row = 0; row < BOT_ATTRIBUTE_COUNT; ++row)
    {
        for (uint32 column = 0; column < BOT_ATTRIBUTE_COUNT; ++column)
        {
            double product = 0.0;
            for (uint32 k = 0; k < BOT_ATTRIBUTE_COUNT; ++k)
                product += model.Factor[row][k] * model.Factor[column][k];

            worstProduct = std::max(worstProduct, std::fabs(product - model.Correlation[row][column]));
        }
    }

    check(factored && worstProduct < 1e-9, std::format("the proposed matrix: factorised {}, L L^T off by {:.2e}",
                                                       factored ? "yes" : "no", worstProduct));

    // A matrix that is not positive definite (three strong links that contradict each other) is refused, and so is
    // an asymmetric one; the factor is then the identity
    AttributeMatrix contradiction = IdentityAttributeMatrix();
    link(contradiction, BotAttribute::Speed, BotAttribute::Wisdom, 0.9);
    link(contradiction, BotAttribute::Speed, BotAttribute::Focus, 0.9);
    link(contradiction, BotAttribute::Wisdom, BotAttribute::Focus, -0.9);
    AttributeMatrix refused;
    check(!FactorAttributeMatrix(contradiction, refused) && refused == IdentityAttributeMatrix(),
          "a matrix that is not positive definite is accepted");
    AttributeMatrix skewed = IdentityAttributeMatrix();
    skewed[0][1] = 0.3;
    check(!FactorAttributeMatrix(skewed, refused), "an asymmetric matrix is accepted");

    // Over 10 000 guids (skill scores of variance 1 from their own hash): the own parts correlate as configured
    // (every entry, the zeros too) and each attribute's score with the skill's by its loading
    std::vector<double> skillScores;
    std::array<std::vector<double>, BOT_ATTRIBUTE_COUNT> parts;
    std::array<std::vector<double>, BOT_ATTRIBUTE_COUNT> scores;
    for (uint32 n = 0; n < Guids; ++n)
    {
        uint64 const guid = FirstGuid + n;
        double const zSkill = Probit(UnitFromHash(SplitMix64(guid ^ 0x5DEECE66DULL)));
        skillScores.push_back(zSkill);
        std::array<double, BOT_ATTRIBUTE_COUNT> const own = AttributeOwnParts(model.Factor, guid);
        std::array<double, BOT_ATTRIBUTE_COUNT> const z = ComputeAttributeScores(model, zSkill, zSkill, guid);
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
        {
            parts[index].push_back(own[index]);
            scores[index].push_back(z[index]);
        }
    }

    double worstPair = 0.0;
    for (uint32 row = 0; row < BOT_ATTRIBUTE_COUNT; ++row)
    {
        for (uint32 column = row + 1; column < BOT_ATTRIBUTE_COUNT; ++column)
            worstPair =
                std::max(worstPair, std::fabs(Correlation(parts[row], parts[column]) - model.Correlation[row][column]));
    }

    double worstLoading = 0.0;
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
        worstLoading =
            std::max(worstLoading, std::fabs(Correlation(scores[index], skillScores) - model.Loading[index]));

    say(
        std::format("over {} guids the own parts' correlations are off by at most {:.4f}, the "
                    "attributes' correlations with the skill from their loadings by {:.4f}",
                    Guids, worstPair, worstLoading));
    check(worstPair < 0.03, std::format("an own parts' correlation is off by {:.4f}", worstPair));
    check(worstLoading < 0.03, std::format("a correlation with the skill is off by {:.4f}", worstLoading));

    // Defaults: every ability is the skill, exactly, and every trait 50, over 1-99 and at the ends of four
    // distributions (normal, wide normal, uniform, narrow uniform)
    BotAttributeParams const defaults;
    std::array<SkillDistributionParams, 4> const distributions = {{{false, 50.0, 15.0, 1.0, 99.0},
                                                                   {false, 20.0, 40.0, 1.0, 99.0},
                                                                   {true, 50.0, 15.0, 1.0, 99.0},
                                                                   {true, 50.0, 15.0, 30.0, 70.0}}};
    uint32 defaultMismatches = 0;
    for (SkillDistributionParams const& distribution : distributions)
    {
        for (uint32 skill = 1; skill <= 99; ++skill)
        {
            BotAttributeValues const values = ComputeBotAttributes(defaults, distribution, static_cast<uint8>(skill),
                                                                   static_cast<uint8>(skill), FirstGuid + skill);
            for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            {
                if (values.Value[index] != (IsBotPersonality(index) ? SKILL_STYLE_NEUTRAL : skill))
                    ++defaultMismatches;
            }
        }
    }

    check(defaultMismatches == 0,
          std::format("{} attributes differ from the skill (or 50) at the defaults", defaultMismatches));

    // At the defaults the gate and the reaction state are the skill's, bit for bit, G0g's jitters included
    TacticGateParams gateParams;
    gateParams.EffortAtSkill1 = 20;
    gateParams.EffortAtSkill99 = 90;
    gateParams.EffortSpread = 10;
    gateParams.StyleSpread = 20;
    std::array<TacticSettings, TACTIC_COUNT> tactics{};
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        tactics[index].KnownFromSkill = static_cast<uint8>((10 + 13 * index) % 100);
        tactics[index].UseChanceAtSkill1 = 20;
        tactics[index].UseChanceAtSkill99 = 90;
    }

    ReactionParams const reaction;
    uint32 gateMismatches = 0;
    uint32 reactionMismatches = 0;
    for (uint32 skill = 0; skill <= 99; ++skill)
    {
        for (uint32 n = 0; n < 20; ++n)
        {
            uint64 const guid = FirstGuid + 31 * n;
            uint8 const value = static_cast<uint8>(skill);
            BotAttributeValues const values = ComputeBotAttributes(defaults, distributions[0], value, value, guid);
            TacticGateState const fromAttributes =
                ComputeTacticGate(gateParams, tactics, AttributeGateInputs(defaults, values, value), guid);
            TacticGateState const fromSkill = ComputeTacticGate(gateParams, tactics, value, guid);
            if (fromAttributes.KnownFrom != fromSkill.KnownFrom || fromAttributes.UseChance != fromSkill.UseChance ||
                fromAttributes.KnownMask != fromSkill.KnownMask || fromAttributes.Effort != fromSkill.Effort ||
                fromAttributes.Style != fromSkill.Style)
                ++gateMismatches;

            ReactionState const fromSpeed = ComputeReactionState(reaction, values.Get(BotAttribute::Speed), 1.0f);
            ReactionState const fromValue = ComputeReactionState(reaction, value, 1.0f);
            if (fromSpeed.Mu != fromValue.Mu || fromSpeed.Sigma != fromValue.Sigma)
                ++reactionMismatches;
        }
    }

    check(gateMismatches == 0, std::format("{} gates differ from G0g's at the defaults", gateMismatches));
    check(reactionMismatches == 0,
          std::format("{} reaction states differ from G0h's at the defaults", reactionMismatches));

    // Skill 0 (the skill system off): abilities 0 and traits 50, also with the model on
    BotAttributeValues const off = ComputeBotAttributes(model, distributions[0], 0, 0, FirstGuid);
    bool offNeutral = true;
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
        offNeutral = offNeutral && off.Value[index] == (IsBotPersonality(index) ? SKILL_STYLE_NEUTRAL : 0);

    check(offNeutral, "skill 0 does not give abilities 0 and traits 50");

    // The same guid and skill give the same attributes (a lab copy drawn from its template's guid equals it)
    uint32 repeatMismatches = 0;
    for (uint32 n = 0; n < 100; ++n)
    {
        uint8 const skill = static_cast<uint8>(1 + n % 99);
        if (ComputeBotAttributes(model, distributions[0], skill, skill, FirstGuid + n).Value !=
            ComputeBotAttributes(model, distributions[0], skill, skill, FirstGuid + n).Value)
            ++repeatMismatches;
    }

    check(repeatMismatches == 0, std::format("{} guids drew different attributes twice", repeatMismatches));

    // With the model on, two bots of skill 50 differ and every value stays in its range
    uint32 varied = 0;
    bool inRange = true;
    for (uint32 n = 0; n < 1000; ++n)
    {
        BotAttributeValues const values = ComputeBotAttributes(model, distributions[0], 50, 50, FirstGuid + n);
        bool differs = false;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
        {
            uint8 const value = values.Value[index];
            differs = differs || value != (IsBotPersonality(index) ? SKILL_STYLE_NEUTRAL : 50);
            inRange = inRange && (IsBotPersonality(index) ? value <= SKILL_GATE_FULL : value >= 1 && value <= 99);
        }

        if (differs)
            ++varied;
    }

    check(varied > 900 && inRange,
          std::format("model on at skill 50: {} of 1000 bots differ from the skill, in range {}", varied,
                      inRange ? "yes" : "no"));

    // The trait map: 50 + 50 * clamp(z / 3, -1, 1) (the score holds the spread), exactly 50 at spread 0
    check(PersonalityFromScore(1.0f, 0.0) == 50 && PersonalityFromScore(1.0f, 3.0) == 100 &&
              PersonalityFromScore(1.0f, -3.0) == 0 && PersonalityFromScore(1.0f, 1.5) == 75 &&
              PersonalityFromScore(1.0f, 9.0) == 100 && PersonalityFromScore(0.0f, 2.5) == 50 &&
              PersonalityFromScore(2.0f, 1.5) == 75,
          "the trait map is not 50 + 50 * clamp(z / 3, -1, 1)");

    // A trait's spread weights its own part once, as an ability's: spread 2 gives 50 + 50 * clamp(2 e / 3, -1, 1)
    BotAttributeParams wideTraits;
    wideTraits.Spread[at(BotAttribute::Aggression)] = 2.0f;
    uint32 spreadOff = 0;
    for (uint64 guid = FirstGuid; guid < FirstGuid + 1000; ++guid)
    {
        double const own = AttributeOwnParts(IdentityAttributeMatrix(), guid)[at(BotAttribute::Aggression)];
        long const expected = std::lround(50.0 + 50.0 * std::clamp(2.0 * own / 3.0, -1.0, 1.0));
        if (ComputeBotAttributes(wideTraits, distributions[0], 50, 50, guid).Get(BotAttribute::Aggression) != expected)
            ++spreadOff;
    }

    check(spreadOff == 0, std::format("{} of 1000 traits at spread 2 do not weight the own part once", spreadOff));

    // The map back to 1-99 inverts the skill's score (normal and uniform)
    uint32 roundTrips = 0;
    for (SkillDistributionParams const& distribution : {distributions[0], distributions[2]})
    {
        for (uint32 skill = 1; skill <= 99; ++skill)
        {
            if (SkillFromScore(distribution, SkillScore(distribution, static_cast<uint8>(skill))) != skill)
                ++roundTrips;
        }
    }

    check(roundTrips == 0, std::format("{} skills do not survive the map to a score and back", roundTrips));

    // Spread 0 with a loading below 1: no own part, the score is loading * z_skill
    BotAttributeParams shrink;
    shrink.Loading[at(BotAttribute::Speed)] = 0.5f;
    check(std::fabs(ComputeAttributeScores(shrink, 2.0, 2.0, FirstGuid)[at(BotAttribute::Speed)] - 1.0) < 1e-12,
          "spread 0 at loading 0.5 is not half the skill's score");

    // Model on: effort from grit without EffortSpread's jitter, use chance from discipline, the style traits from
    // the personality
    uint64 const drawnGuid = FirstGuid + 3;
    BotAttributeValues const drawn = ComputeBotAttributes(model, distributions[0], 50, 50, drawnGuid);
    TacticGateInputs const inputs = AttributeGateInputs(model, drawn, 50);
    TacticGateState const gate = ComputeTacticGate(gateParams, tactics, inputs, drawnGuid);
    check(!inputs.EffortJitter && inputs.StyleMask == 7 &&
              gate.Effort == SkillEffort(gateParams, drawn.Get(BotAttribute::Grit), drawnGuid, false) &&
              gate.UseChance[0] ==
                  TacticUseChance(tactics[0], drawn.Get(BotAttribute::Discipline), gateParams.CurveExponent) &&
              gate.Style[0] == drawn.Get(BotAttribute::Aggression) &&
              gate.Style[1] == drawn.Get(BotAttribute::Caution) && gate.Style[2] == drawn.Get(BotAttribute::Patience),
          "with the model on the gate does not read grit, discipline and the traits");

    // DT.14: the learnable attributes read the grown skill's score, the fixed ones the first roll's, Dexterity
    // halfway (G0j has no growth and passes one skill twice)
    BotAttributeParams whole;
    whole.Loading.fill(1.0f);
    std::array<double, BOT_ATTRIBUTE_COUNT> const grown = ComputeAttributeScores(whole, 1.0, 0.0, FirstGuid);
    BotAttributeValues const split = ComputeBotAttributes(defaults, distributions[0], 65, 35, FirstGuid);
    check(grown[at(BotAttribute::Knowledge)] == 1.0 && grown[at(BotAttribute::Wisdom)] == 1.0 &&
              grown[at(BotAttribute::Foresight)] == 1.0 && grown[at(BotAttribute::Teamwork)] == 1.0 &&
              grown[at(BotAttribute::Dexterity)] == 0.5 && grown[at(BotAttribute::Speed)] == 0.0 &&
              grown[at(BotAttribute::Focus)] == 0.0 && grown[at(BotAttribute::Discipline)] == 0.0 &&
              grown[at(BotAttribute::Grit)] == 0.0 && grown[at(BotAttribute::Aggression)] == 0.0 &&
              split.Get(BotAttribute::Knowledge) == 65 && split.Get(BotAttribute::Speed) == 35 &&
              split.Get(BotAttribute::Dexterity) == 50,
          "the learnable and fixed attributes do not read the grown and the first skill");
}

#endif
