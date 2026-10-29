/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Tactics.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <sstream>
#include <vector>

#include "BotAttributes.h"
#include "BotProfileMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "NaturalReaction.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringConvert.h"
#include "TacticGate.h"
#include "Timer.h"

namespace
{
constexpr std::array<char const*, TACTIC_CONTEXT_COUNT> TACTIC_CONTEXT_NAMES = {"Arena", "Battleground", "World",
                                                                                "Pve"};

std::array<std::array<std::atomic<uint64>, TACTIC_CONTEXT_COUNT>, TACTIC_COUNT> Activations{};

std::string Lower(std::string text)
{
    for (char& character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    return text;
}

std::string Trim(std::string const& text)
{
    size_t const first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";

    size_t const last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> SplitList(std::string const& text)
{
    std::vector<std::string> entries;
    std::istringstream in(text);
    std::string entry;
    while (std::getline(in, entry, ','))
        entries.push_back(Trim(entry));

    if (!text.empty() && text.back() == ',')
        entries.push_back("");

    return entries;
}

std::optional<uint8> FindScopeEntry(std::string const& entry)
{
    std::string const name = Lower(entry);
    if (name == "all")
        return TACTIC_SCOPE_ALL;

    if (name == "none")
        return 0;

    for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
    {
        if (name == Lower(TACTIC_CONTEXT_NAMES[context]))
            return TacticContextBit(static_cast<TacticContext>(context));
    }

    return std::nullopt;
}

PlayerbotAI* FindBot(ChatHandler* handler, std::string const& target, std::string& name)
{
    ObjectGuid guid = ObjectGuid::Empty;
    name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    if (guid.IsEmpty() || !sCharacterCache->GetCharacterNameByGuid(guid, name))
    {
        handler->PSendSysMessage("No character {}.", target);
        return nullptr;
    }

    // Looked up per command; the pointers are not kept
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
    if (!botAI)
        handler->PSendSysMessage("{} is not an online bot.", name);

    return botAI;
}

std::string GuidName(ObjectGuid guid)
{
    std::string name;
    if (guid.IsPlayer() && sCharacterCache->GetCharacterNameByGuid(guid, name))
        return name;

    return guid.ToString();
}

// Pure checks of the scope gate, the context probe, the name table and the mask edits; no bots, no config
void RunScopeSelftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TACTICS selftest scope FAILED: {}", what);
    };

    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        TacticId const id = static_cast<TacticId>(index);
        TacticDefinition const& definition = TACTIC_DEFINITIONS[index];
        check(Lower(definition.ConfigName) == definition.Name,
              Acore::StringFormat("config name {} is not the name {}", definition.ConfigName, definition.Name));
        check(FindTactic(definition.Name) == id, Acore::StringFormat("name {} not found", definition.Name));
        check(FindTactic(definition.ConfigName) == id,
              Acore::StringFormat("config name {} not found", definition.ConfigName));
        check(definition.DefaultScope != 0 && definition.DefaultScope <= TACTIC_SCOPE_ALL,
              Acore::StringFormat("{} default scope {}", definition.Name, definition.DefaultScope));
    }

    // DT.9 spot checks
    check(TACTIC_DEFINITIONS[static_cast<uint32>(TacticId::Pillar)].DefaultScope == TACTIC_SCOPE_ARENA, "pillar");
    check(TACTIC_DEFINITIONS[static_cast<uint32>(TacticId::Jump)].DefaultScope == TACTIC_SCOPE_ARENA, "jump");
    check(TACTIC_DEFINITIONS[static_cast<uint32>(TacticId::Plan)].DefaultScope ==
              (TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND),
          "plan");
    check(TACTIC_DEFINITIONS[static_cast<uint32>(TacticId::Drink)].DefaultScope ==
              (TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND),
          "drink");
    check(TACTIC_DEFINITIONS[static_cast<uint32>(TacticId::Hold)].DefaultScope ==
              (TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD),
          "hold");
    check(TACTIC_DEFINITIONS[static_cast<uint32>(TacticId::OwnCc)].DefaultScope == TACTIC_SCOPE_ALL, "owncc");
    check(!FindTactic("moon"), "unknown tactic found");

    // The probe: every combination of the three flags
    for (uint32 flags = 0; flags < 8; ++flags)
    {
        bool const inArena = flags & 1;
        bool const inBattleground = flags & 2;
        bool const pvpFight = flags & 4;
        TacticContext const expected = inArena          ? TacticContext::Arena
                                       : inBattleground ? TacticContext::Battleground
                                       : pvpFight       ? TacticContext::OpenWorldPvp
                                                        : TacticContext::Pve;
        check(ClassifyTacticContext(inArena, inBattleground, pvpFight) == expected,
              Acore::StringFormat("probe arena {} battleground {} pvp {}", inArena, inBattleground, pvpFight));
    }

    // Every scope list against every context, through the parser and the gate of every tactic
    for (uint32 subset = 0; subset <= TACTIC_SCOPE_ALL; ++subset)
    {
        std::string text;
        for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
        {
            if (subset & (1u << context))
                text += (text.empty() ? "" : ",") + std::string(TACTIC_CONTEXT_NAMES[context]);
        }

        if (text.empty())
            text = "None";

        uint8 parsed = 0xFF;
        std::string error;
        check(ParseTacticScope(text, parsed, error) && parsed == subset,
              Acore::StringFormat("parse '{}' gave {} ({})", text, parsed, error));
        uint8 parsedLower = 0xFF;
        check(ParseTacticScope(" " + Lower(text) + " ", parsedLower, error) && parsedLower == subset,
              Acore::StringFormat("parse lower case '{}'", text));
        check(FormatTacticScope(static_cast<uint8>(subset)) == (subset == TACTIC_SCOPE_ALL ? "All" : text),
              Acore::StringFormat("format {}", subset));

        for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
        {
            TacticContext const here = static_cast<TacticContext>(context);
            bool const listed = text.find(TACTIC_CONTEXT_NAMES[context]) != std::string::npos;
            check(ScopeAllows(parsed, here) == listed,
                  Acore::StringFormat("scope '{}' in context {}", text, TACTIC_CONTEXT_NAMES[context]));
            for (uint32 index = 0; index < TACTIC_COUNT; ++index)
            {
                TacticId const id = static_cast<TacticId>(index);
                for (bool const enable : {false, true})
                {
                    check(IsTacticAllowed(enable, TACTIC_ALL_MASK, id, parsed, here) == (enable && listed),
                          Acore::StringFormat("gate {} scope '{}' context {} enable {}", TacticName(id), text,
                                              TACTIC_CONTEXT_NAMES[context], enable));
                    check(!IsTacticAllowed(enable, TACTIC_ALL_MASK & ~TacticBit(id), id, parsed, here),
                          Acore::StringFormat("gate {} outside the mask", TacticName(id)));
                }
            }
        }
    }

    uint8 scope = 0;
    std::string error;
    check(ParseTacticScope("all", scope, error) && scope == TACTIC_SCOPE_ALL, "parse all");
    check(!ParseTacticScope("Arena,Moon", scope, error), "parse Arena,Moon accepted");
    check(!ParseTacticScope("", scope, error), "parse empty accepted");
    check(!ParseTacticScope("Arena,", scope, error), "parse 'Arena,' accepted");

    // Mask edits
    uint32 const hold = TacticBit(TacticId::Hold);
    uint32 const cc = TacticBit(TacticId::Cc);
    uint32 mask = 0;
    check(ApplyTacticMaskEdit(hold | cc, "", mask, error) && mask == 0, "empty edit");
    check(ApplyTacticMaskEdit(0, "+hold", mask, error) && mask == hold, "+hold");
    check(ApplyTacticMaskEdit(hold | cc, "-hold", mask, error) && mask == cc, "-hold");
    check(ApplyTacticMaskEdit(0, "+hold,+CC", mask, error) && mask == (hold | cc), "+hold,+CC");
    check(ApplyTacticMaskEdit(0, "+hold,-hold", mask, error) && mask == 0, "+hold,-hold");
    check(ApplyTacticMaskEdit(0, "+all,-cc", mask, error) && mask == (TACTIC_ALL_MASK & ~cc), "+all,-cc");
    check(ApplyTacticMaskEdit(TACTIC_ALL_MASK, "-all", mask, error) && mask == 0, "-all");
    check(!ApplyTacticMaskEdit(0, "hold", mask, error), "edit without sign accepted");
    check(!ApplyTacticMaskEdit(0, "+moon", mask, error), "unknown tactic accepted");
    check(!ApplyTacticMaskEdit(0, "+hold,,-cc", mask, error), "empty entry accepted");
    check(FormatTacticMask(0) == "none", "format of the empty mask");
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        uint32 const bit = TacticBit(static_cast<TacticId>(index));
        check(ApplyTacticMaskEdit(0, "+" + FormatTacticMask(bit), mask, error) && mask == bit,
              Acore::StringFormat("round trip {}", TACTIC_DEFINITIONS[index].Name));
    }

    handler->PSendSysMessage("TACTICS selftest scope: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS", checks,
                             failures);
}

// Pearson correlation of two equally long series (0 when one of them is constant)
double Correlation(std::vector<double> const& x, std::vector<double> const& y)
{
    double const n = static_cast<double>(x.size());
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double syy = 0.0;
    double sxy = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        sx += x[i];
        sy += y[i];
        sxx += x[i] * x[i];
        syy += y[i] * y[i];
        sxy += x[i] * y[i];
    }

    double const vx = sxx - sx * sx / n;
    double const vy = syy - sy * sy / n;
    if (vx <= 0.0 || vy <= 0.0)
        return 0.0;

    return (sxy - sx * sy / n) / std::sqrt(vx * vy);
}

// Pure checks of the skill gate (TacticGate.h) on synthetic guids, skills and times; no bots, no config
void RunGateSelftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TACTICS selftest gate FAILED: {}", what);
    };

    constexpr uint64 FirstGuid = 1000;
    constexpr uint32 Guids = 1000;
    constexpr uint32 WindowMs = 1500;

    // Curve: at e = 1 every lerp is the one before G0g, bit for bit (D3's float factor, D4's rounded numbers)
    uint32 lerpMismatches = 0;
    for (uint32 skill = 1; skill <= 99; ++skill)
    {
        float const position = (static_cast<float>(skill) - 1.0f) / 98.0f;
        auto const oldLerp = [position](uint32 low, uint32 high)
        { return static_cast<uint32>(low + (static_cast<float>(high) - low) * position + 0.5f); };
        if (SkillPosition(static_cast<uint8>(skill)) != position ||
            SkillLerp(3.0f, 0.5f, position, 1.0f) != 3.0f + (0.5f - 3.0f) * position ||
            SkillLerpRounded(60, 0, position, 1.0f) != oldLerp(60, 0) ||
            SkillLerpRounded(3000, 200, position, 1.0f) != oldLerp(3000, 200))
            ++lerpMismatches;
    }

    check(lerpMismatches == 0, Acore::StringFormat("e = 1 differs from the old lerp at {} skills", lerpMismatches));
    // The plan's "about 76 %": 1 - (1 - 0.5)^2 = 0.75 exactly
    float const half = SkillCurve(SkillPosition(50), 2.0f);
    handler->PSendSysMessage("TACTICS selftest gate: e = 2, skill 50 sits {:.4f} of the way", half);
    check(std::fabs(half - 0.75f) < 0.02f, Acore::StringFormat("e = 2 at skill 50: {:.4f}, not 0.75", half));
    check(SkillCurve(0.0f, 2.0f) == 0.0f && SkillCurve(1.0f, 2.0f) == 1.0f && SkillCurve(0.0f, 0.5f) == 0.0f &&
              SkillCurve(1.0f, 0.5f) == 1.0f,
          "curve ends are not 0 and 1");
    bool monotonic = true;
    for (uint32 skill = 2; skill <= 99; ++skill)
        for (float const exponent : {0.5f, 2.0f, 3.0f})
            if (SkillCurve(SkillPosition(static_cast<uint8>(skill)), exponent) <
                SkillCurve(SkillPosition(static_cast<uint8>(skill - 1)), exponent))
                monotonic = false;

    check(monotonic, "curve not monotonic");
    check(SkillCurve(0.5f, 0.5f) < 0.5f, "e = 0.5 does not bend the other way");

    // Knowledge: 1000 synthetic guids, jitter -15..+15 around the threshold, mean near 0, patchy per tactic
    int32 minJitter = 1000;
    int32 maxJitter = -1000;
    int64 jitterSum = 0;
    uint32 patchy = 0;
    uint32 spreadZero = 0;
    uint32 baseZero = 0;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        int32 const jitter = static_cast<int32>(TacticKnownFrom(50, 15, guid, TacticId::Hold)) - 50;
        minJitter = std::min(minJitter, jitter);
        maxJitter = std::max(maxJitter, jitter);
        jitterSum += jitter;
        if (TacticKnownFrom(50, 15, guid, TacticId::Kite) != TacticKnownFrom(50, 15, guid, TacticId::Hold))
            ++patchy;

        if (TacticKnownFrom(50, 0, guid, TacticId::Hold) != 50)
            ++spreadZero;

        if (TacticKnownFrom(0, 15, guid, TacticId::Hold) != 0)
            ++baseZero;
    }

    double const jitterMean = static_cast<double>(jitterSum) / Guids;
    handler->PSendSysMessage("TACTICS selftest gate: knowledge jitter over {} guids: {} to {}, mean {:.2f}", Guids,
                             minJitter, maxJitter, jitterMean);
    check(minJitter == -15 && maxJitter == 15,
          Acore::StringFormat("knowledge jitter {} to {}, not -15 to 15", minJitter, maxJitter));
    check(std::fabs(jitterMean) < 1.5, Acore::StringFormat("knowledge jitter mean {:.2f} (tolerance 1.5)", jitterMean));
    check(patchy >= 900,
          Acore::StringFormat("knowledge not patchy: {} of {} differ between two tactics", patchy, Guids));
    check(spreadZero == 0, "spread 0 still jitters");
    check(baseZero == 0, "threshold 0 is not known by every bot");

    // Use chance per window: stable inside a window, the configured rate over 10 000 windows
    TacticSettings settings;
    settings.UseChanceAtSkill1 = 20;
    settings.UseChanceAtSkill99 = 90;
    check(TacticUseChance(settings, 1, 1.0f) == 20 && TacticUseChance(settings, 99, 1.0f) == 90 &&
              TacticUseChance(settings, 50, 1.0f) == 55 && TacticUseChance(settings, 0, 1.0f) == 100,
          "use chance lerp (20 at 1, 55 at 50, 90 at 99, 100 at skill 0)");
    check(TacticUseChance(settings, 50, 2.0f) == 73, "use chance at e = 2, skill 50 is not 73");
    for (uint32 const chance : {5u, 20u, 50u, 90u})
    {
        uint64 const guid = FirstGuid + chance;
        uint32 used = 0;
        for (uint32 window = 0; window < 10000; ++window)
            if (TacticUsedInWindow(guid, TacticId::Hold, chance, WindowMs, 12345 + window * WindowMs))
                ++used;

        double const rate = used / 100.0;
        handler->PSendSysMessage("TACTICS selftest gate: chance {} %: used in {:.2f} % of 10000 windows", chance, rate);
        check(std::fabs(rate - chance) <= 3.0,
              Acore::StringFormat("chance {} %: rate {:.2f} % over 10000 windows (tolerance 3 points)", chance, rate));
    }

    uint32 unstable = 0;
    uint32 windows = 0;
    uint32 usedWindows = 0;
    {
        uint64 const guid = FirstGuid + 7;
        uint64 lastWindow = TacticUseWindow(guid, WindowMs, 0);
        bool lastAnswer = TacticUsedInWindow(guid, TacticId::Kite, 50, WindowMs, 0);
        for (uint32 now = 37; now < 600000; now += 37)
        {
            uint64 const window = TacticUseWindow(guid, WindowMs, now);
            bool const answer = TacticUsedInWindow(guid, TacticId::Kite, 50, WindowMs, now);
            if (window == lastWindow && answer != lastAnswer)
                ++unstable;

            if (window != lastWindow)
            {
                ++windows;
                usedWindows += answer ? 1 : 0;
            }

            lastWindow = window;
            lastAnswer = answer;
        }
    }

    check(unstable == 0, Acore::StringFormat("{} answers changed inside a window", unstable));
    check(windows >= 390 && usedWindows > 0 && usedWindows < windows,
          Acore::StringFormat("{} windows, {} used: the answer does not change between windows", windows, usedWindows));
    check(!TacticUsedInWindow(FirstGuid, TacticId::Hold, 0, WindowMs, 0) &&
              TacticUsedInWindow(FirstGuid, TacticId::Hold, 100, WindowMs, 0),
          "chance 0 used or chance 100 not used");

    // Effort: defaults are full effort at every skill; a range lerps and jitters within its spread, clamped
    TacticGateParams params;
    uint32 notFull = 0;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
        if (SkillEffort(params, static_cast<uint8>(1 + guid % 99), guid) != SKILL_GATE_FULL)
            ++notFull;

    check(notFull == 0, Acore::StringFormat("default effort below 100 for {} bots", notFull));
    params.EffortAtSkill1 = 20;
    params.EffortAtSkill99 = 90;
    check(SkillEffort(params, 1, FirstGuid) == 20 && SkillEffort(params, 99, FirstGuid) == 90 &&
              SkillEffort(params, 0, FirstGuid) == SKILL_GATE_FULL,
          "effort lerp (20 at 1, 90 at 99, 100 at skill 0)");
    params.EffortSpread = 10;
    int32 minEffort = 1000;
    int32 maxEffort = -1000;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        int32 const effort = SkillEffort(params, 50, guid);
        minEffort = std::min(minEffort, effort);
        maxEffort = std::max(maxEffort, effort);
    }

    check(minEffort == 45 && maxEffort == 65,
          Acore::StringFormat("effort at skill 50, spread 10: {} to {}, not 45 to 65", minEffort, maxEffort));
    params.EffortAtSkill1 = 0;
    params.EffortSpread = 50;
    bool clamped = true;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
        if (SkillEffort(params, 1, guid) > 50)
            clamped = false;

    check(clamped, "effort below 0 wraps instead of clamping");

    // Style: a function of the guid only. Over 1000 synthetic guids, traits against skill (BotProfileMgr's
    // guid-seeded percentiles through the uniform distribution, and plain skills 1..99) and against each other:
    // |r| < 0.1 (about three standard errors at n = 1000)
    params = TacticGateParams();
    params.StyleSpread = 30;
    std::vector<double> seeded;
    std::vector<double> plain;
    std::array<std::vector<double>, SKILL_STYLE_TRAIT_COUNT> traits;
    int32 minTrait = 1000;
    int32 maxTrait = -1000;
    uint32 skillDependent = 0;
    std::array<TacticSettings, TACTIC_COUNT> const tactics{};
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        seeded.push_back(BotProfileMgr::SkillFromPercentile(BotProfileMgr::LegacyPercentile(static_cast<uint32>(guid)),
                                                            true, 50.0, 15.0, 1.0, 99.0));
        plain.push_back(static_cast<double>(1 + guid % 99));
        for (uint32 trait = 0; trait < SKILL_STYLE_TRAIT_COUNT; ++trait)
        {
            int32 const value = SkillStyle(params.StyleSpread, guid, static_cast<SkillStyleTrait>(trait));
            traits[trait].push_back(value);
            minTrait = std::min(minTrait, value);
            maxTrait = std::max(maxTrait, value);
        }

        if (ComputeTacticGate(params, tactics, 1, guid).Style != ComputeTacticGate(params, tactics, 99, guid).Style)
            ++skillDependent;
    }

    check(minTrait == 20 && maxTrait == 80,
          Acore::StringFormat("style traits {} to {}, not 20 to 80 at spread 30", minTrait, maxTrait));
    check(skillDependent == 0, Acore::StringFormat("style changes with skill for {} guids", skillDependent));
    double worst = 0.0;
    for (uint32 trait = 0; trait < SKILL_STYLE_TRAIT_COUNT; ++trait)
    {
        worst = std::max(worst, std::fabs(Correlation(traits[trait], seeded)));
        worst = std::max(worst, std::fabs(Correlation(traits[trait], plain)));
        worst = std::max(worst, std::fabs(Correlation(traits[trait], traits[(trait + 1) % SKILL_STYLE_TRAIT_COUNT])));
    }

    handler->PSendSysMessage("TACTICS selftest gate: style over {} guids: traits {} to {}, largest |r| {:.3f}", Guids,
                             minTrait, maxTrait, worst);
    check(worst < 0.1, Acore::StringFormat("style correlates with skill or another trait: |r| {:.3f}", worst));
    check(SkillStyle(0, FirstGuid, SkillStyleTrait::Caution) == SKILL_STYLE_NEUTRAL, "style spread 0 is not 50");

    // The whole gate: defaults change nothing (known, always used), skill 0 is full ability, thresholds bite
    params = TacticGateParams();
    TacticGateState const defaults = ComputeTacticGate(params, tactics, 1, FirstGuid);
    check(defaults.KnownMask == TACTIC_ALL_MASK && defaults.Effort == SKILL_GATE_FULL &&
              TacticGatePasses(defaults, TacticId::Hold, false, FirstGuid, WindowMs, 4242),
          "the default gate at skill 1 does not let every tactic act");
    std::array<TacticSettings, TACTIC_COUNT> holdTactics{};
    TacticSettings& hold = holdTactics[static_cast<uint32>(TacticId::Hold)];
    hold.KnownFromSkill = 40;
    hold.UseChanceAtSkill1 = 0;
    hold.UseChanceAtSkill99 = 0;
    TacticGateState const off = ComputeTacticGate(params, holdTactics, 0, FirstGuid);
    check(off.KnownMask == TACTIC_ALL_MASK && off.UseChance[0] == SKILL_GATE_FULL && off.Effort == SKILL_GATE_FULL &&
              off.Style[0] == SKILL_STYLE_NEUTRAL,
          "skill 0 is not full ability");
    uint32 lowKnows = 0;
    uint32 highMisses = 0;
    for (uint64 guid = FirstGuid; guid < FirstGuid + Guids; ++guid)
    {
        if (ComputeTacticGate(params, holdTactics, 20, guid).KnownMask & TacticBit(TacticId::Hold))
            ++lowKnows;

        if (!(ComputeTacticGate(params, holdTactics, 60, guid).KnownMask & TacticBit(TacticId::Hold)))
            ++highMisses;
    }

    check(
        lowKnows == 0 && highMisses == 0,
        Acore::StringFormat("threshold 40 +- 15: {} bots at skill 20 know it, {} at 60 do not", lowKnows, highMisses));
    TacticGateState const known = ComputeTacticGate(params, holdTactics, 99, FirstGuid);
    check(!TacticGatePasses(known, TacticId::Hold, false, FirstGuid, WindowMs, 4242) &&
              TacticGatePasses(known, TacticId::Hold, true, FirstGuid, WindowMs, 4242),
          "use chance 0 passes, or full ability does not");
    check(SkillExemptFor(true, true) && !SkillExemptFor(false, true) && !SkillExemptFor(true, false) &&
              !SkillExemptFor(false, false),
          "the real-master exemption is not option and master");

    handler->PSendSysMessage("TACTICS selftest gate: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS", checks,
                             failures);
}

// Pure checks of the natural reaction (NaturalReaction.h) with the configured numbers on synthetic hashes, skills and
// times; no bots
void RunReactionSelftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TACTICS selftest reaction FAILED: {}", what);
    };
    auto const within = [](double value, double expected, double tolerance)
    { return std::fabs(value - expected) <= tolerance * std::fabs(expected); };

    ReactionParams const& params = sPlayerbotAIConfig.NaturalReaction;
    float const exponent = sPlayerbotAIConfig.SkillGate.CurveExponent;
    constexpr uint32 Draws = 10000;
    constexpr uint32 MissWindowMs = 1500;

    // The probit against known quantiles, and increasing
    check(std::fabs(Probit(0.5)) < 1e-9 && std::fabs(Probit(0.975) - 1.959963985) < 1e-6 &&
              std::fabs(Probit(0.025) + 1.959963985) < 1e-6 && std::fabs(Probit(0.001) + 3.090232306) < 1e-6,
          "probit quantiles");
    bool increasing = true;
    for (uint32 step = 2; step < 1000; ++step)
        increasing = increasing && Probit(step / 1000.0) > Probit((step - 1) / 1000.0);

    check(increasing, "probit not increasing");
    check(UnitFromHash(0) > 0.0 && UnitFromHash(~0ULL) < 1.0, "unit from hash leaves (0, 1)");

    // 10 000 events per skill: the median of the draws is the configured median and the spread of their logarithms
    // the configured sigma (+-5 %); the minimum holds; a hash always gives the same time
    for (uint8 skill : {1, 20, 50, 80, 99})
    {
        ReactionState const state = ComputeReactionState(params, skill, exponent);
        float const position = SkillPosition(skill);
        double const median = SkillLerp(params.MedianMsAtSkill1, params.MedianMsAtSkill99, position, exponent);
        double const sigma = SkillLerp(params.SigmaAtSkill1, params.SigmaAtSkill99, position, exponent);
        std::vector<double> raw;
        raw.reserve(Draws);
        double sum = 0.0;
        double sumSquares = 0.0;
        uint32 belowMin = 0;
        uint32 raised = 0;
        bool stable = true;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            uint64 const hash = ReactionEventHash(1000 + draw, static_cast<uint8>(1 + draw % 6), draw * 7919);
            double const value = ReactionRawMs(state, hash);
            raw.push_back(value);
            sum += std::log(value);
            sumSquares += std::log(value) * std::log(value);
            uint32 const clamped = ReactionMs(state, params.MinMs, hash);
            belowMin += clamped < params.MinMs ? 1 : 0;
            raised += value < params.MinMs ? 1 : 0;
            stable = stable && clamped == ReactionMs(state, params.MinMs, hash);
        }

        std::nth_element(raw.begin(), raw.begin() + Draws / 2, raw.end());
        double const measuredMedian = raw[Draws / 2];
        double const mean = sum / Draws;
        double const measuredSigma = std::sqrt(std::max(0.0, sumSquares / Draws - mean * mean));
        check(within(measuredMedian, median, 0.05),
              Acore::StringFormat("skill {}: median {:.1f} ms, configured {:.1f}", skill, measuredMedian, median));
        check(sigma == 0.0 ? measuredSigma < 1e-6 : within(measuredSigma, sigma, 0.05),
              Acore::StringFormat("skill {}: sigma {:.4f}, configured {:.4f}", skill, measuredSigma, sigma));
        check(belowMin == 0 && stable, Acore::StringFormat("skill {}: {} draws under the minimum, stable {}", skill,
                                                           belowMin, stable ? "yes" : "no"));
        handler->PSendSysMessage(
            "TACTICS selftest reaction: skill {}: median {:.1f} ms (configured {:.1f}), sigma "
            "{:.4f} ({:.4f}), raised to the minimum {:.1f} %",
            skill, measuredMedian, median, measuredSigma, sigma, raised * 100.0 / Draws);
    }

    // Behind adds its penalty and a change of focus its cost, each within its range
    ReactionState const middle = ComputeReactionState(params, 50, exponent);
    bool behindOk = true;
    bool switchOk = true;
    double behindSum = 0.0;
    double switchSum = 0.0;
    for (uint32 draw = 0; draw < Draws; ++draw)
    {
        uint64 const hash = SplitMix64(draw ^ ReactionSalt::Event);
        uint32 const plain = EventReactionMs(middle, params, hash, false, false);
        uint32 const behind = EventReactionMs(middle, params, hash, true, false) - plain;
        uint32 const switched = EventReactionMs(middle, params, hash, false, true) - plain;
        behindOk = behindOk && behind >= params.BehindMinMs && behind <= params.BehindMaxMs;
        switchOk = switchOk && switched >= params.SwitchMinMs && switched <= params.SwitchMaxMs;
        behindSum += behind;
        switchSum += switched;
    }

    double const behindMid = (params.BehindMinMs + params.BehindMaxMs) / 2.0;
    double const switchMid = (params.SwitchMinMs + params.SwitchMaxMs) / 2.0;
    check(behindOk && (behindMid == 0.0 || within(behindSum / Draws, behindMid, 0.05)),
          Acore::StringFormat("behind penalty mean {:.1f} ms, expected {:.1f}", behindSum / Draws, behindMid));
    check(switchOk && (switchMid == 0.0 || within(switchSum / Draws, switchMid, 0.05)),
          Acore::StringFormat("switch cost mean {:.1f} ms, expected {:.1f}", switchSum / Draws, switchMid));

    // The event: blocked until its reaction time, then it passes and takes the focus; a quiet gap after the notice
    // starts the next event
    constexpr uint64 Guid = 4242;
    ReactionFocus focus;
    ReactionEvent event;
    StartReactionEvent(event, focus, middle, params, Guid, 1, 0, false, 10000);
    uint32 const ready = event.ReadyAfterMs;
    check(ready == EventReactionMs(middle, params, ReactionEventHash(Guid, 1, 10000), false, false),
          "the event's reaction time is not the plain draw");
    check(!ReactionEventPasses(event, focus, 1, 10000 + ready - 1) && focus.Category == 0,
          "passes before its reaction time");
    check(ReactionEventPasses(event, focus, 1, 10000 + ready) && focus.Category == 1, "blocked after its time");
    uint32 const passedAt = 10000 + ready;
    check(!ReactionEventStarts(event, passedAt + REACTION_NOTICED_GAP_MS, MissWindowMs) &&
              ReactionEventStarts(event, passedAt + REACTION_NOTICED_GAP_MS + 1, MissWindowMs),
          "a noticed event does not end after its quiet gap");
    check(ReactionEventPasses(event, focus, 1, passedAt + 300) && focus.AtMs == passedAt,
          "a later pass of a noticed event moves the focus stamp");

    // A trigger checked once a second keeps its pending event and passes at the first query after the time
    ReactionEvent sparse;
    ReactionFocus none;
    StartReactionEvent(sparse, none, middle, params, Guid, 2, 0, false, 50000);
    uint32 now = 50000;
    bool restarted = false;
    while (!ReactionEventPasses(sparse, none, 2, now) && now < 80000)
    {
        now += 1000;
        restarted = restarted || ReactionEventStarts(sparse, now, MissWindowMs);
    }

    check(!restarted && sparse.Noticed && now - 50000 < sparse.ReadyAfterMs + 1000,
          Acore::StringFormat("a query a second: passed after {} ms, reaction {} ms, restarted {}", now - 50000,
                              sparse.ReadyAfterMs, restarted ? "yes" : "no"));

    // Focus: another category within FocusTimeoutMs pays the switch cost, later not; behind adds its penalty
    uint32 const soon = passedAt + 100;
    ReactionEvent other;
    StartReactionEvent(other, focus, middle, params, Guid, 3, 0, false, soon);
    check(params.FocusTimeoutMs <= 100 ||
              other.ReadyAfterMs == EventReactionMs(middle, params, ReactionEventHash(Guid, 3, soon), false, true),
          "a change of focus does not pay the switch cost");
    uint32 const late = passedAt + params.FocusTimeoutMs;
    StartReactionEvent(other, focus, middle, params, Guid, 3, 0, false, late);
    uint32 const plainLate = EventReactionMs(middle, params, ReactionEventHash(Guid, 3, late), false, false);
    check(other.ReadyAfterMs == plainLate, "the switch cost outlives FocusTimeoutMs");
    StartReactionEvent(other, focus, middle, params, Guid, 3, 0, true, late);
    check(other.ReadyAfterMs >= plainLate + params.BehindMinMs && other.ReadyAfterMs <= plainLate + params.BehindMaxMs,
          "an event behind the bot does not add its penalty");

    // Misses: D4's chance, per event; a missed event never passes and starts again after the window
    uint32 missed = 0;
    for (uint32 draw = 0; draw < Draws; ++draw)
    {
        ReactionEvent trial;
        StartReactionEvent(trial, none, middle, params, Guid + draw, 4, 30, false, draw * 3);
        missed += trial.Missed ? 1 : 0;
    }

    check(within(missed * 100.0 / Draws, 30.0, 0.05),
          Acore::StringFormat("miss share {:.2f} % at 30 %", missed * 100.0 / Draws));
    ReactionEvent lost;
    StartReactionEvent(lost, none, middle, params, Guid, 4, 95, false, 0);
    for (uint32 start = 1; !lost.Missed && start < 100; ++start)
        StartReactionEvent(lost, none, middle, params, Guid, 4, 95, false, start);

    uint32 const lostAt = lost.StartMs;
    check(lost.Missed && !ReactionEventPasses(lost, none, 4, lostAt + MissWindowMs - 100) &&
              !ReactionEventStarts(lost, lostAt + MissWindowMs - 1, MissWindowMs) &&
              ReactionEventStarts(lost, lostAt + MissWindowMs, MissWindowMs),
          "a missed event passes, or does not start again after its window");

    // End to end: the target sum, never a pass before the event's server time, the latency correction on check streams
    ReactionParams endToEnd = params;
    endToEnd.EndToEnd = true;
    ReactionParams firstQuery = params;
    firstQuery.EndToEnd = false;
    constexpr uint32 TickMs = 25;
    ReactionFocus timing;
    ReactionEvent timed;
    StartReactionEvent(timed, timing, middle, endToEnd, Guid, 1, 0, false, 70000);
    StampReactionEvent(timed, timing, endToEnd, 120, ReactionStampSource::CastBar, TickMs);
    uint32 const sum = timed.ReadyAfterMs + endToEnd.PingMs + endToEnd.DisplayMs + TickMs;
    check(timed.TargetMs == sum && timed.StampAgeMs == 120 && timed.DueAfterMs == (sum > 120 ? sum - 120 : 0),
          Acore::StringFormat("end-to-end target {} ms due after {} ms; expected reaction {} + ping {} + display {} + "
                              "ticks {} = {}, 120 ms of it gone",
                              timed.TargetMs, timed.DueAfterMs, timed.ReadyAfterMs, endToEnd.PingMs, endToEnd.DisplayMs,
                              TickMs, sum));
    check((timed.DueAfterMs == 0 || !ReactionEventPasses(timed, timing, 1, 70000 + timed.DueAfterMs - 1)) &&
              ReactionEventPasses(timed, timing, 1, 70000 + timed.DueAfterMs),
          "an end-to-end event does not pass at its due time");
    ReactionEvent plain;
    StartReactionEvent(plain, timing, middle, firstQuery, Guid, 2, 0, false, 70000);
    StampReactionEvent(plain, timing, firstQuery, 120, ReactionStampSource::CastBar, TickMs);
    check(plain.TargetMs == plain.ReadyAfterMs && plain.DueAfterMs == plain.ReadyAfterMs && plain.StampAgeMs == 0,
          "without EndToEnd the event does not count from its first query");
    ReactionEvent stale;
    StartReactionEvent(stale, timing, middle, endToEnd, Guid, 2, 0, false, 70000);
    StampReactionEvent(stale, timing, endToEnd, REACTION_MAX_STAMP_AGE_MS + 1, ReactionStampSource::Aura, TickMs);
    check(stale.StampAgeMs == 0 && stale.StampSource == static_cast<uint8>(ReactionStampSource::Stale),
          "a server time older than the stamp age limit is kept");

    // No pass before the event's server time: a latency beyond the target passes at the first query and the early
    // pass lowers it; an event first seen after its target passes at once and leaves the latency alone
    ReactionFocus slow;
    slow.LatencyMs = 5000.0f;
    slow.LatencySeeded = true;
    ReactionEvent eager;
    StartReactionEvent(eager, slow, middle, endToEnd, Guid, 3, 0, false, 80000);
    StampReactionEvent(eager, slow, endToEnd, 0, ReactionStampSource::CastBar, TickMs);
    check(eager.DueAfterMs == 0 && eager.Corrected && ReactionEventPasses(eager, slow, 3, 80000) &&
              slow.LatencyMs < 5000.0f,
          "a latency beyond the target moves the pass before the first query, or the early pass keeps it");
    float const kept = slow.LatencyMs;
    ReactionEvent seenLate;
    StartReactionEvent(seenLate, slow, middle, endToEnd, Guid, 4, 0, false, 90000);
    StampReactionEvent(seenLate, slow, endToEnd, REACTION_MAX_STAMP_AGE_MS, ReactionStampSource::CastBar, TickMs);
    check(seenLate.TargetMs > REACTION_MAX_STAMP_AGE_MS ||
              (!seenLate.Corrected && seenLate.DueAfterMs == 0 && ReactionEventPasses(seenLate, slow, 4, 90000) &&
               slow.LatencyMs == kept),
          "an event first seen after its target waits, or its pass trains the latency");

    // Check streams: the first check after a server event within one interval, then one every interval plus a
    // jitter; after a warm-up the pass lands on the target on average (the bot's latency is learnt)
    struct StreamResult
    {
        double MeanErrorMs = 0.0;
        float LatencyMs = 0.0f;
        uint32 Early = 0;
    };
    auto const stream = [&](uint32 intervalMs, uint32 jitterMs, bool pauses, uint64 seed)
    {
        constexpr uint32 Events = 400;
        constexpr uint32 Warmup = 100;
        constexpr uint32 PauseMs = 2000;
        StreamResult result;
        ReactionFocus learner;
        double errorSum = 0.0;
        double latencySum = 0.0;
        uint32 counted = 0;
        uint32 clock = 100000;
        for (uint32 index = 0; index < Events; ++index)
        {
            uint64 const hash = SplitMix64(seed + index);
            uint32 const stamp = clock;
            uint32 now = stamp + static_cast<uint32>(hash % intervalMs);
            ReactionEvent trial;
            StartReactionEvent(trial, learner, middle, endToEnd, seed, 1, 0, false, now);
            StampReactionEvent(trial, learner, endToEnd, now - stamp, ReactionStampSource::CastBar, TickMs);
            // Every tenth event of a pausing stream waits out a cast of the bot's own before its next check
            bool const paused = pauses && index % 10 == 5;
            for (uint32 step = 0;
                 step < 1000 && !ReactionEventPasses(trial, learner, 1, now, 2 * (intervalMs + jitterMs)); ++step)
                now += paused && step == 0 ? PauseMs
                                           : intervalMs + static_cast<uint32>(SplitMix64(hash ^ step) % (jitterMs + 1));

            if (static_cast<int32>(now - stamp) < 0)
                ++result.Early;

            if (index >= Warmup && !paused)
            {
                errorSum += static_cast<double>(now - stamp) - trial.TargetMs;
                latencySum += learner.LatencyMs;
                ++counted;
            }

            clock = now + 3000;
        }

        result.MeanErrorMs = errorSum / counted;
        result.LatencyMs = static_cast<float>(latencySum / counted);
        return result;
    };
    struct StreamCase
    {
        uint32 IntervalMs;
        uint32 JitterMs;
        bool Pauses;
    };
    // The cadence (100 ms, a 20 ms stagger, 25 ms ticks), with the bot's own casts, and today's 250 ms + 0-200 ms
    for (StreamCase const& streamCase :
         {StreamCase{100, 25, false}, StreamCase{120, 25, true}, StreamCase{450, 50, false}})
    {
        StreamResult const result =
            stream(streamCase.IntervalMs, streamCase.JitterMs, streamCase.Pauses, 7000 + streamCase.IntervalMs);
        double const meanGap = streamCase.IntervalMs + streamCase.JitterMs / 2.0;
        check(
            result.Early == 0 && std::fabs(result.MeanErrorMs) <= 0.1 * meanGap && result.LatencyMs >= 0.25 * meanGap &&
                result.LatencyMs <= 0.75 * meanGap,
            Acore::StringFormat("check stream {} ms + 0-{} ms{}: mean pass - target {:.1f} ms, mean latency {:.1f} ms, "
                                "{} passes before the server time",
                                streamCase.IntervalMs, streamCase.JitterMs, streamCase.Pauses ? " with casts" : "",
                                result.MeanErrorMs, result.LatencyMs, result.Early));
    }

    // The cadence's stagger: upstream's guid % 201 at 200, at most StaggerMs
    bool staggerOk = true;
    for (uint64 const guid : {1ULL, 200ULL, 201ULL, 4242ULL, 987654321ULL})
        staggerOk = staggerOk && CheckStaggerMs(guid, UPSTREAM_CHECK_STAGGER_MS) == guid % 201 &&
                    CheckStaggerMs(guid, 20) <= 20 && CheckStaggerMs(guid, 0) == 0;

    check(staggerOk, "the check stagger is not upstream's guid % 201 at 200, or exceeds StaggerMs");

    // The cure and dispel triggers and the party dispel value: the cadence's interval while it runs, when faster
    check(ReactionCheckIntervalMs(1000, true, 100) == 100 && ReactionCheckIntervalMs(50, true, 100) == 50 &&
              ReactionCheckIntervalMs(1, true, 100) == 1,
          "a reaction trigger under the cadence does not check at min(its interval, CheckMs)");
    check(ReactionCheckIntervalMs(1000, false, 100) == 1000 && ReactionCheckIntervalMs(1, false, 20) == 1,
          "a reaction trigger without the cadence does not keep its own interval");

    // G0h3's reaction types, on copies of the configured prices with every part off (G0h2's event) and on
    {
        ReactionParams off = params;
        off.Choice.Enable = false;
        off.Load.Enable = false;
        off.Ease.Enable = false;
        off.Think.Enable = false;
        off.Anticipate.Enable = false;
        ReactionParams on = off;
        on.Choice.Enable = true;
        on.Load.Enable = true;
        on.Ease.Enable = true;
        on.Think.Enable = true;
        on.Anticipate.Enable = true;
        // The load and anticipation checks need prices: fixed ones (the dist's), so a configured 0 cannot void them
        on.Load.MsPerEventAt1 = 200;
        on.Load.MsPerEventAt99 = 80;
        on.Load.MissPercentPerEventAt1 = 10;
        on.Load.MissPercentPerEventAt99 = 2;
        on.Load.TopProtectAt1 = 0.0f;
        on.Load.TopProtectAt99 = 0.8f;
        on.Anticipate.ShareAt1 = 0.1f;
        on.Anticipate.ShareAt99 = 0.5f;
        auto const pricesAt = [&](uint8 attribute)
        {
            ReactionTypeInputs inputs;
            inputs.Speed = attribute;
            inputs.Wisdom = attribute;
            inputs.Focus = attribute;
            inputs.Dexterity = attribute;
            inputs.Foresight = attribute;
            return ComputeReactionTypeState(on, inputs, exponent);
        };
        ReactionTypeState const low = pricesAt(1);
        ReactionTypeState const mid = pricesAt(50);
        ReactionTypeState const high = pricesAt(99);
        ReactionFocus const fresh;
        auto const eventHash = [](uint32 draw)
        { return ReactionEventHash(Guid + draw, static_cast<uint8>(1 + draw % 6), 200000 + draw * 13); };
        auto const startAt = [&](ReactionParams const& with, ReactionTypeState const& prices,
                                 ReactionEventInputs const& inputs, uint32 draw, uint32 missPercent)
        {
            ReactionEvent event;
            StartReactionEvent(event, fresh, middle, with, Guid + draw, static_cast<uint8>(1 + draw % 6), missPercent,
                               false, 200000 + draw * 13, prices, inputs);
            return event;
        };
        uint32 const commonMs = EaseCostMs(mid, ReactionEase::Common);

        // The parts off ignore every input, and the default inputs add nothing but the common button's price
        ReactionEventInputs heavy;
        heavy.Options = 8;
        heavy.Load = 2;
        heavy.Ease = ReactionEase::NewTarget;
        heavy.Thinks = true;
        heavy.Predicted = true;
        bool unchanged = true;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            uint64 const hash = eventHash(draw);
            ReactionEvent const before = startAt(off, ReactionTypeState(), ReactionEventInputs(), draw, 20);
            ReactionEvent const ignored = startAt(off, mid, heavy, draw, 20);
            ReactionEvent const plainOn = startAt(on, mid, ReactionEventInputs(), draw, 20);
            unchanged = unchanged && before.ReadyAfterMs == EventReactionMs(middle, off, hash, false, false) &&
                        before.Missed == (SplitMix64(hash ^ ReactionSalt::Miss) % 100 < 20) && before.CostMs == 0 &&
                        ignored.ReadyAfterMs == before.ReadyAfterMs && ignored.Missed == before.Missed &&
                        ignored.CostMs == 0 && plainOn.ReadyAfterMs == before.ReadyAfterMs + commonMs &&
                        plainOn.Type == static_cast<uint8>(ReactionType::Simple);
        }

        check(unchanged, "with the parts off, or the default inputs, the event is not G0h2's");

        // Choice: every doubling of the options adds the price (+-5 %, at least 1 ms of rounding), less at high Wisdom
        uint32 const maxOptions = on.Choice.MaxOptions;
        auto const hickOk = [&](ReactionTypeState const& prices)
        {
            bool ok = HickCostMs(prices, 0, maxOptions) == 0 && HickCostMs(prices, 1, maxOptions) == 0 &&
                      HickCostMs(prices, maxOptions * 2, maxOptions) == HickCostMs(prices, maxOptions, maxOptions);
            for (uint32 options = 1; options * 2 <= maxOptions; options *= 2)
            {
                double const added = static_cast<double>(HickCostMs(prices, options * 2, maxOptions)) -
                                     HickCostMs(prices, options, maxOptions);
                ok = ok && std::fabs(added - prices.HickMsPerDoubling) <=
                               std::max(1.0, 0.05 * static_cast<double>(prices.HickMsPerDoubling));
            }

            return ok;
        };
        check(hickOk(low) && hickOk(mid) && hickOk(high) &&
                  (low.HickMsPerDoubling == 0.0f || high.HickMsPerDoubling < low.HickMsPerDoubling),
              Acore::StringFormat("doubling the options does not add {:.1f} / {:.1f} / {:.1f} ms at Wisdom 1 / 50 / 99 "
                                  "(+-5 %), or not less at high Wisdom",
                                  low.HickMsPerDoubling, mid.HickMsPerDoubling, high.HickMsPerDoubling));
        ReactionEventInputs four;
        four.Options = 4;
        ReactionEvent const single = startAt(on, mid, ReactionEventInputs(), 7, 0);
        ReactionEvent const chosen = startAt(on, mid, four, 7, 0);
        check(chosen.Type == static_cast<uint8>(ReactionType::Choice) && chosen.Options == 4 &&
                  single.Type == static_cast<uint8>(ReactionType::Simple) &&
                  chosen.ReadyAfterMs - single.ReadyAfterMs == HickCostMs(mid, 4, maxOptions),
              "a choice of four options does not add Hick's cost to the reaction");

        // Load: a second pending event is answered slower and missed more (at a 10 % base), less so at high Focus,
        // and the top event keeps the protected share off
        struct LoadResult
        {
            double AddedMs = 0.0;
            double AddedMissPercent = 0.0;
            double TopAddedMs = 0.0;
        };
        auto const loadAt = [&](ReactionTypeState const& prices)
        {
            ReactionEventInputs loaded;
            loaded.Load = 1;
            ReactionEventInputs top = loaded;
            top.LoadTop = true;
            double alone = 0.0;
            double withLoad = 0.0;
            double withTop = 0.0;
            uint32 missedAlone = 0;
            uint32 missedLoaded = 0;
            for (uint32 draw = 0; draw < Draws; ++draw)
            {
                ReactionEvent const first = startAt(on, prices, ReactionEventInputs(), draw, 10);
                ReactionEvent const second = startAt(on, prices, loaded, draw, 10);
                alone += first.ReadyAfterMs;
                withLoad += second.ReadyAfterMs;
                withTop += startAt(on, prices, top, draw, 10).ReadyAfterMs;
                missedAlone += first.Missed ? 1 : 0;
                missedLoaded += second.Missed ? 1 : 0;
            }

            LoadResult result;
            result.AddedMs = (withLoad - alone) / Draws;
            result.AddedMissPercent = (static_cast<double>(missedLoaded) - missedAlone) * 100.0 / Draws;
            result.TopAddedMs = (withTop - alone) / Draws;
            return result;
        };
        LoadResult const loadLow = loadAt(low);
        LoadResult const loadHigh = loadAt(high);
        auto const loadMatches = [](LoadResult const& result, ReactionTypeState const& prices)
        {
            return result.AddedMs > 0.0 && std::fabs(result.AddedMs - prices.LoadMsPerEvent) < 0.5 &&
                   result.AddedMissPercent > 0.0 &&
                   std::fabs(result.AddedMissPercent - prices.LoadMissPercentPerEvent) <= 1.0;
        };
        check(loadMatches(loadLow, low) && loadMatches(loadHigh, high),
              Acore::StringFormat("a second pending event adds {:.1f} ms and {:.2f} % misses at Focus 1 (configured {} "
                                  "ms, {} %), {:.1f} ms and {:.2f} % at Focus 99 ({} ms, {} %)",
                                  loadLow.AddedMs, loadLow.AddedMissPercent, low.LoadMsPerEvent,
                                  low.LoadMissPercentPerEvent, loadHigh.AddedMs, loadHigh.AddedMissPercent,
                                  high.LoadMsPerEvent, high.LoadMissPercentPerEvent));
        check(loadHigh.AddedMs < loadLow.AddedMs && loadHigh.AddedMissPercent < loadLow.AddedMissPercent &&
                  (high.LoadTopProtect == 0.0f || loadHigh.TopAddedMs < loadHigh.AddedMs),
              Acore::StringFormat("the load is not lighter at high Focus, or the top event not protected (top +{:.1f} "
                                  "ms)",
                                  loadHigh.TopAddedMs));
        handler->PSendSysMessage(
            "TACTICS selftest reaction: load +{:.0f} ms / +{:.1f} % misses per event at Focus 1, +{:.0f} / +{:.1f} at "
            "99 (top event +{:.0f} ms)",
            loadLow.AddedMs, loadLow.AddedMissPercent, loadHigh.AddedMs, loadHigh.AddedMissPercent,
            loadHigh.TopAddedMs);

        // Ease: common < rare < focus target < new target at every Dexterity (the focus target at a new target's
        // price below FocusDexterity), and the price lands in the reaction
        bool easeOrder = true;
        for (uint8 dexterity : {1, 20, 39, 40, 41, 60, 80, 99})
        {
            ReactionTypeInputs inputs;
            inputs.Dexterity = dexterity;
            ReactionTypeState const prices = ComputeReactionTypeState(on, inputs, exponent);
            uint32 const common = EaseCostMs(prices, ReactionEase::Common);
            uint32 const rare = EaseCostMs(prices, ReactionEase::Rare);
            uint32 const focusTarget = EaseCostMs(prices, ReactionEase::Focus);
            uint32 const newTarget = EaseCostMs(prices, ReactionEase::NewTarget);
            bool const focusKnown = dexterity >= on.Ease.FocusDexterity;
            easeOrder = easeOrder && common < rare && rare < focusTarget &&
                        (focusKnown ? focusTarget < newTarget : focusTarget == newTarget);
        }

        ReactionEventInputs rareButton;
        rareButton.Ease = ReactionEase::Rare;
        check(easeOrder && startAt(on, mid, rareButton, 7, 0).ReadyAfterMs - single.ReadyAfterMs ==
                               EaseCostMs(mid, ReactionEase::Rare) - commonMs,
              "the ease order is not common < rare < focus target < new target at every Dexterity, or a rare "
              "button's price is not in the reaction");

        // Anticipation: a predicted cast saves the configured share of the draw (Foresight 50, +-5 %), never below
        // the minimum, and is no choice
        ReactionEventInputs predicted;
        predicted.Predicted = true;
        predicted.Options = 4;
        bool anticipatedOk = true;
        double drawSum = 0.0;
        double shortenedSum = 0.0;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            uint32 const full = ReactionMs(middle, on.MinMs, eventHash(draw));
            ReactionEvent const plainEvent = startAt(on, mid, ReactionEventInputs(), draw, 0);
            ReactionEvent const ahead = startAt(on, mid, predicted, draw, 0);
            uint32 const shortened = ahead.ReadyAfterMs - (plainEvent.ReadyAfterMs - full);
            anticipatedOk = anticipatedOk && ahead.Type == static_cast<uint8>(ReactionType::Anticipated) &&
                            ahead.ReadyAfterMs <= plainEvent.ReadyAfterMs && shortened >= on.MinMs;
            if (full * (1.0 - mid.AnticipateShare) >= on.MinMs + 1.0)
            {
                drawSum += full;
                shortenedSum += shortened;
            }
        }

        double const saved = drawSum > 0.0 ? 1.0 - shortenedSum / drawSum : 0.0;
        check(anticipatedOk && within(saved, mid.AnticipateShare, 0.05),
              Acore::StringFormat("a predicted cast saves {:.3f} of the draw, configured {:.3f} (Foresight 50)", saved,
                                  mid.AnticipateShare));

        // Thinking: the shortcut's cost lands in the reaction, a deep check charged later moves the pass, the deep
        // share follows Wisdom, the wait Patience
        ReactionEventInputs thinking;
        thinking.Thinks = true;
        ReactionEvent const quick = startAt(on, mid, thinking, 11, 0);
        ReactionEvent const unthinking = startAt(on, mid, ReactionEventInputs(), 11, 0);
        ReactionEvent deep = quick;
        AddReactionThinkMs(deep, mid.ThinkDeepMs);
        ReactionEvent noticed = quick;
        noticed.Noticed = true;
        AddReactionThinkMs(noticed, 100);
        check(quick.ThinkMs == mid.ThinkShortcutMs &&
                  quick.ReadyAfterMs - unthinking.ReadyAfterMs == mid.ThinkShortcutMs &&
                  deep.ReadyAfterMs == quick.ReadyAfterMs + mid.ThinkDeepMs &&
                  deep.TargetMs == quick.TargetMs + mid.ThinkDeepMs &&
                  deep.DueAfterMs == quick.DueAfterMs + mid.ThinkDeepMs && noticed.ReadyAfterMs == quick.ReadyAfterMs &&
                  mid.ThinkShortcutMs < mid.ThinkDeepMs,
              "the think cost does not land in the reaction, or a deep check does not move the pass");
        uint32 deepChosen = 0;
        for (uint32 draw = 0; draw < Draws; ++draw)
            deepChosen += ChooseThinkDepth(mid, SplitMix64(draw * 7 + 3)) == ReactionThinkDepth::Deep ? 1 : 0;

        ReactionTypeInputs restless;
        restless.Patience = 0;
        ReactionTypeInputs patient;
        patient.Patience = 100;
        check(std::fabs(deepChosen * 100.0 / Draws - mid.ThinkDeepPercent) <= 1.5 &&
                  low.ThinkDeepPercent <= high.ThinkDeepPercent &&
                  ComputeReactionTypeState(on, restless, exponent).PatienceWaitMs == on.Think.PatienceWaitMsAt0 &&
                  ComputeReactionTypeState(on, patient, exponent).PatienceWaitMs == on.Think.PatienceWaitMsAt100,
              Acore::StringFormat("deep checks {:.1f} % at Wisdom 50 (configured {} %), or the wait not from Patience",
                                  deepChosen * 100.0 / Draws, mid.ThinkDeepPercent));

        // The queue-like follow-up: after an own cast or GCD that ends before the next check, the check lands within
        // the draw of the end (exactly at it when that is before the regular check), never later than the regular one
        bool queueOk = true;
        for (ReactionTypeState const* prices : {&low, &high})
        {
            for (uint32 draw = 0; draw < Draws; ++draw)
            {
                uint64 const hash = SplitMix64(draw * 31 + 5);
                uint32 const drawMs = QueueDrawMs(*prices, hash);
                uint32 const remaining = static_cast<uint32>((hash >> 40) % 300);
                uint32 const delay = QueueCheckDelayMs(remaining, 100, drawMs);
                bool const endsFirst = remaining < 100;
                queueOk = queueOk && drawMs >= prices->QueueMinMs && drawMs <= prices->QueueMaxMs && delay <= 100 &&
                          (endsFirst ? delay >= remaining && delay - remaining <= drawMs &&
                                           (remaining + drawMs > 100 || delay == remaining + drawMs)
                                     : delay == 100);
            }
        }

        check(queueOk, Acore::StringFormat("the follow-up check does not land within its draw of the end ({}-{} ms at "
                                           "Speed 1, {}-{} ms at 99) or comes after the regular check",
                                           low.QueueMinMs, low.QueueMaxMs, high.QueueMinMs, high.QueueMaxMs));

        // The opponent memory: the fake rate after the casts and cancels seen (an interrupt is no cancel, the same
        // cast seen twice one cast), a repeated spell predicted, bounded to its slots, cleared for the next match
        uint32 const tolerance = params.Memory.EndToleranceMs;
        uint32 const casts = std::max(10u, params.Memory.MinCasts);
        OpponentMemory memory;
        memory.Clear(77);
        uint32 clock = 300000;
        uint32 cancels = 0;
        bool memoryOk = memory.MatchId == 77 && memory.Find(7) == nullptr;
        for (uint32 cast = 0; cast < casts; ++cast)
        {
            uint32 const end = clock + 2000;
            memoryOk = memoryOk && memory.Predicts(7, 100, end, tolerance) == (cast > 0);
            memory.NoteCast(7, 100, end, clock, tolerance);
            memory.NoteCast(7, 100, end, clock + 100, tolerance);
            memoryOk = memoryOk && memory.Predicts(7, 100, end, tolerance) == (cast > 0) &&
                       (memory.FakeRate(7, params.Memory.MinCasts) == 0.0f) ==
                           (cast + 1 < params.Memory.MinCasts || cancels == 0);
            if (cast % 3 == 0)
            {
                memory.NoteCastGone(7, clock + 500, tolerance, false);
                ++cancels;
            }
            else if (cast % 5 == 4)
                memory.NoteCastGone(7, clock + 600, tolerance, true);
            else
                memory.NoteCastGone(7, end + 50, tolerance, false);

            clock = end + 1000;
        }

        OpponentMemorySlot const* seen = memory.Find(7);
        float const rate = memory.FakeRate(7, params.Memory.MinCasts);
        memoryOk = memoryOk && seen && seen->CastsSeen == casts && seen->CancelsSeen == cancels &&
                   std::fabs(rate - static_cast<float>(cancels) / casts) < 1e-6f;
        // A new cast before the end of the one in view: that one was cancelled, and another spell is not predicted
        memory.NoteCast(9, 200, clock + 2000, clock, tolerance);
        memory.NoteCast(9, 300, clock + 2300, clock + 300, tolerance);
        OpponentMemorySlot const* switched = memory.Find(9);
        memoryOk = memoryOk && switched && switched->CastsSeen == 2 && switched->CancelsSeen == 1 &&
                   !memory.Predicts(9, 300, clock + 2300, tolerance);
        for (uint32 enemy = 1; enemy < OPPONENT_MEMORY_SLOTS; ++enemy)
            memory.NoteCast(1000 + enemy, 100, clock + 5000, clock + 1000 + enemy, tolerance);

        memoryOk = memoryOk && memory.Find(7) == nullptr && memory.Find(9) && memory.Find(1001) &&
                   memory.Find(1000 + OPPONENT_MEMORY_SLOTS - 1);
        memory.Clear(78);
        memoryOk = memoryOk && memory.MatchId == 78 && memory.Find(9) == nullptr && memory.Find(1001) == nullptr;
        check(memoryOk, Acore::StringFormat("the opponent memory: fake rate {:.3f} after {} cancels in {} casts, "
                                            "expected {:.3f}; or a repeat, the bound or the clear failed",
                                            rate, cancels, casts, static_cast<float>(cancels) / casts));

        // The busy bot: its own cast stops for a kick of an important cast only when it would end too late; no kick a
        // busy bot reaches only within the margin of the cast's end; both off by their flags
        ReactionReevalParams rules = params.Reeval;
        rules.Enable = true;
        rules.BusySkip = true;
        uint32 const important = rules.ImportantCastMs;
        uint32 const margin = rules.BusyMarginMs;
        uint32 const ownLeft = std::max(rules.MinOwnRemainingMs, 1000u);
        ReactionReevalParams rulesOff = rules;
        rulesOff.Enable = false;
        rulesOff.BusySkip = false;
        check(StopOwnCastForKick(rules, important, ownLeft, ownLeft) &&
                  !StopOwnCastForKick(rules, important, ownLeft + margin + 1, ownLeft) &&
                  (important == 0 || !StopOwnCastForKick(rules, important - 1, ownLeft, ownLeft)) &&
                  (rules.MinOwnRemainingMs == 0 ||
                   !StopOwnCastForKick(rules, important, 1, rules.MinOwnRemainingMs - 1)) &&
                  !StopOwnCastForKick(rulesOff, important, ownLeft, ownLeft),
              "the own cast is not stopped for an important kick it would make too late, or is stopped otherwise");
        check(InterruptOutOfReach(rules, 2000, 1000, 2000 + margin) &&
                  InterruptOutOfReach(rules, 2000, -500, 2000 + margin) &&
                  !InterruptOutOfReach(rules, 1999, 1000, 2000 + margin) &&
                  !InterruptOutOfReach(rules, 2000, 2000, 2000 + margin) &&
                  !InterruptOutOfReach(rulesOff, 2000, 1000, 2000 + margin),
              "a kick made late by being busy is not dropped, or one in reach or late from its reaction alone is");

        // Without the busy time: stamped, the unclipped due no earlier than the own cast's start; unstamped, the due
        // OwnMs earlier; no own cast, the due itself
        ReactionEvent busyEvent;
        busyEvent.TargetMs = 900;
        busyEvent.StampAgeMs = 1200;
        busyEvent.DueAfterMs = 0;
        busyEvent.OwnMs = 200;
        ReactionEvent freeEvent = busyEvent;
        freeEvent.StampAgeMs = 300;
        freeEvent.DueAfterMs = 550;
        freeEvent.OwnMs = 0;
        ReactionEvent unstampedEvent;
        unstampedEvent.DueAfterMs = 700;
        unstampedEvent.OwnMs = 300;
        check(BusyFreeDueMs(busyEvent, 50, true) == -200 && BusyFreeDueMs(freeEvent, 50, true) == 550 &&
                  BusyFreeDueMs(freeEvent, 50, false) == 550 && BusyFreeDueMs(unstampedEvent, 50, false) == 400,
              "the kick's due without the busy time is not the stamped or the unstamped one");

        // One stop per enemy cast: a bot whose kick never follows and that starts its next own cast at once, checked
        // every 100 ms through a 3 s enemy cast with a pushback, stops once; none in the backoff after that wasted
        // stop, one again after it; a followed stop leaves the next cast its own
        ReactionBusyState watch;
        uint32 stops = 0;
        uint32 const castStart = 10000;
        uint32 const castMs = 3000;
        for (uint32 now = castStart; now < castStart + castMs; now += 100)
        {
            uint32 const end = castStart + castMs + (now >= castStart + 1000 ? 250 : 0);
            if (!watch.Watches(7, 116, end, castMs / 2))
                watch.Watch(7, 116, end, castStart + 600);

            if (watch.StopDue(now))
            {
                watch.NoteStop(now, 7, 2139);
                watch.NoteStopUnfollowed(now);
                ++stops;
            }
        }

        uint32 const inBackoff = castStart + castMs + 500;
        watch.Watch(7, 116, inBackoff + castMs, inBackoff + 600);
        bool const backedOff = !watch.StopDue(inBackoff + 600) && !watch.StopDue(inBackoff + castMs);
        uint32 const after = castStart + 600 + REACTION_STOP_BACKOFF_MS;
        watch.Watch(7, 116, after + castMs, after);
        bool const again = watch.StopDue(after);
        watch.NoteStop(after, 7, 2139);
        bool const spent = !watch.StopDue(after + 100);
        watch.Watch(8, 116, after + 2 * castMs, after + castMs);
        check(
            stops == 1 && backedOff && again && spent && watch.StopDue(after + castMs) && watch.UnfollowedStops == 1,
            Acore::StringFormat("the busy stop is not once per enemy cast: {} stops in one cast, backoff {}, after it "
                                "{}, spent {}",
                                stops, backedOff ? "held" : "broken", again ? "due" : "not due", spent ? "yes" : "no"));

        // An unanswered event loads the others while its trigger asks, up to the pending gap after its last query;
        // a noticed or missed one does not
        ReactionEvent waiting;
        waiting.Active = true;
        waiting.StartMs = 1000;
        waiting.LastQueryMs = 1000;
        ReactionEvent answered = waiting;
        answered.Noticed = true;
        ReactionEvent lost = waiting;
        lost.Missed = true;
        check(ReactionEventPending(waiting, 1000 + REACTION_PENDING_GAP_MS) &&
                  !ReactionEventPending(waiting, 1001 + REACTION_PENDING_GAP_MS) &&
                  !ReactionEventPending(answered, 1000) && !ReactionEventPending(lost, 1000),
              "an unanswered event does not count as pending for exactly the pending gap");
        handler->PSendSysMessage(
            "TACTICS selftest reaction: types at attribute 50: choice {:.0f} ms per doubling, load {} ms / {} %, ease "
            "{}/{}/{}/{} ms, think {}/{} ms (deep {} %), anticipate {:.2f}, queue {}-{} ms",
            mid.HickMsPerDoubling, mid.LoadMsPerEvent, mid.LoadMissPercentPerEvent, mid.EaseMs[0], mid.EaseMs[1],
            mid.EaseMs[2], mid.EaseMs[3], mid.ThinkShortcutMs, mid.ThinkDeepMs, mid.ThinkDeepPercent,
            mid.AnticipateShare, mid.QueueMinMs, mid.QueueMaxMs);
    }

    handler->PSendSysMessage("TACTICS selftest reaction: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS", checks,
                             failures);
}

// Pure checks of the attribute model (BotAttributes.h) on synthetic guids and skills; no bots, no config
void RunAttributesSelftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TACTICS selftest attributes FAILED: {}", what);
    };
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

    check(factored && worstProduct < 1e-9,
          Acore::StringFormat("the proposed matrix: factorised {}, L L^T off by {:.2e}", factored ? "yes" : "no",
                              worstProduct));

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

    handler->PSendSysMessage(
        "TACTICS selftest attributes: over {} guids the own parts' correlations are off by at most {:.4f}, the "
        "attributes' correlations with the skill from their loadings by {:.4f}",
        Guids, worstPair, worstLoading);
    check(worstPair < 0.03, Acore::StringFormat("an own parts' correlation is off by {:.4f}", worstPair));
    check(worstLoading < 0.03, Acore::StringFormat("a correlation with the skill is off by {:.4f}", worstLoading));

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
          Acore::StringFormat("{} attributes differ from the skill (or 50) at the defaults", defaultMismatches));

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

    check(gateMismatches == 0, Acore::StringFormat("{} gates differ from G0g's at the defaults", gateMismatches));
    check(reactionMismatches == 0,
          Acore::StringFormat("{} reaction states differ from G0h's at the defaults", reactionMismatches));

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

    check(repeatMismatches == 0, Acore::StringFormat("{} guids drew different attributes twice", repeatMismatches));

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
          Acore::StringFormat("model on at skill 50: {} of 1000 bots differ from the skill, in range {}", varied,
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

    check(spreadOff == 0,
          Acore::StringFormat("{} of 1000 traits at spread 2 do not weight the own part once", spreadOff));

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

    check(roundTrips == 0, Acore::StringFormat("{} skills do not survive the map to a score and back", roundTrips));

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

    handler->PSendSysMessage("TACTICS selftest attributes: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS",
                             checks, failures);
}
}  // namespace

char const* TacticName(TacticId id)
{
    uint32 const index = static_cast<uint32>(id);
    return index < TACTIC_COUNT ? TACTIC_DEFINITIONS[index].Name : "?";
}

char const* TacticContextName(TacticContext context)
{
    uint32 const index = static_cast<uint32>(context);
    return index < TACTIC_CONTEXT_COUNT ? TACTIC_CONTEXT_NAMES[index] : "?";
}

std::optional<TacticId> FindTactic(std::string const& name)
{
    std::string const wanted = Lower(name);
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        if (wanted == TACTIC_DEFINITIONS[index].Name)
            return static_cast<TacticId>(index);
    }

    return std::nullopt;
}

bool ParseTacticScope(std::string const& text, uint8& scope, std::string& error)
{
    uint8 result = 0;
    for (std::string const& entry : SplitList(text))
    {
        std::optional<uint8> const bits = FindScopeEntry(entry);
        if (!bits)
        {
            error = Acore::StringFormat("unknown context '{}' (Arena, Battleground, World, Pve, All, None)", entry);
            return false;
        }

        result |= *bits;
    }

    if (Trim(text).empty())
    {
        error = "empty scope";
        return false;
    }

    scope = result;
    return true;
}

std::string FormatTacticScope(uint8 scope)
{
    if ((scope & TACTIC_SCOPE_ALL) == TACTIC_SCOPE_ALL)
        return "All";

    std::string text;
    for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
    {
        if (scope & (1u << context))
            text += (text.empty() ? "" : ",") + std::string(TACTIC_CONTEXT_NAMES[context]);
    }

    return text.empty() ? "None" : text;
}

bool ApplyTacticMaskEdit(uint32 baseMask, std::string const& edit, uint32& mask, std::string& error)
{
    if (Trim(edit).empty())
    {
        mask = 0;
        return true;
    }

    uint32 result = baseMask;
    for (std::string const& entry : SplitList(edit))
    {
        if (entry.size() < 2 || (entry[0] != '+' && entry[0] != '-'))
        {
            error = Acore::StringFormat("tactic edit '{}' is not +<name> or -<name> (names: all, {})", entry,
                                        TacticNameList());
            return false;
        }

        std::string const name = entry.substr(1);
        uint32 bits = TACTIC_ALL_MASK;
        if (Lower(name) != "all")
        {
            std::optional<TacticId> const id = FindTactic(name);
            if (!id)
            {
                error = Acore::StringFormat("unknown tactic '{}' (names: all, {})", name, TacticNameList());
                return false;
            }

            bits = TacticBit(*id);
        }

        if (entry[0] == '+')
            result |= bits;
        else
            result &= ~bits;
    }

    mask = result;
    return true;
}

std::string FormatTacticMask(uint32 mask)
{
    std::string text;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        if (mask & TacticBit(static_cast<TacticId>(index)))
            text += (text.empty() ? "" : ",") + std::string(TACTIC_DEFINITIONS[index].Name);
    }

    return text.empty() ? "none" : text;
}

std::string TacticNameList()
{
    std::string text;
    for (TacticDefinition const& definition : TACTIC_DEFINITIONS)
        text += (text.empty() ? "" : ", ") + std::string(definition.Name);

    return text;
}

void NoteTacticActivation(TacticId id, TacticContext context)
{
    uint32 const index = static_cast<uint32>(id);
    uint32 const contextIndex = static_cast<uint32>(context);
    if (index < TACTIC_COUNT && contextIndex < TACTIC_CONTEXT_COUNT)
        Activations[index][contextIndex].fetch_add(1, std::memory_order_relaxed);
}

// World thread: reads only scalars and the bot's copied snapshots (C3), like .playerbots skill
bool TacticCommands::HandleShowCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string target;
    in >> target;
    if (target.empty())
    {
        handler->PSendSysMessage("Usage: .playerbots tactics show <bot name or guid>");
        return false;
    }

    std::string name;
    PlayerbotAI* botAI = FindBot(handler, target, name);
    if (!botAI)
        return false;

    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    uint32 const mask = botAI->GetTacticMask();
    TacticGateState const& gate = botAI->GetTacticGate();
    handler->PSendSysMessage("TACTICS {}: Playerbots.Tactics.Enable {}, context {}{}, skill {}", name,
                             config.TacticsEnable ? 1 : 0, TacticContextName(botAI->GetTacticContext()),
                             config.TacticsEnable ? "" : " (not probed while off)", botAI->GetPvpSkill());
    handler->PSendSysMessage("  mask: {} ({}); config mask: {}", FormatTacticMask(mask),
                             botAI->IsTacticMaskForced() ? "forced by the harness" : "from config",
                             FormatTacticMask(config.TacticsConfigMask));

    uint32 here = 0;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        TacticId const id = static_cast<TacticId>(index);
        TacticSettings const& settings = config.TacticConfig[index];
        bool const enabledHere = botAI->TacticEnabledHere(id);
        if (enabledHere)
            here |= TacticBit(id);

        handler->PSendSysMessage("  {}: enable {}, scope {}, in mask {}, here {}; known from {} (base {}) {}, use {} %",
                                 TacticName(id), settings.Enable ? 1 : 0, FormatTacticScope(settings.Scope),
                                 (mask & TacticBit(id)) ? 1 : 0, enabledHere ? 1 : 0, gate.KnownFrom[index],
                                 settings.KnownFromSkill, (gate.KnownMask & TacticBit(id)) ? "yes" : "no",
                                 gate.UseChance[index]);
    }

    handler->PSendSysMessage("  effective here: {}", FormatTacticMask(here));

    // The skill gate (hidden, GM only). The master pointer is read on the world thread, while the maps wait.
    bool const realMaster = botAI->HasGameClientMaster();
    bool const exempt = botAI->IsSkillExempt();
    bool const full = botAI->GetPvpSkill() == 0 || exempt;
    handler->PSendSysMessage(
        "  skill gate: {}; real master {}, Playerbots.Skill.ExemptRealMasterBots {}, curve {:.2f}",
        botAI->GetPvpSkill() == 0 ? "full ability (skill 0)" : (exempt ? "full ability (exempt)" : "skill applies"),
        realMaster ? "yes" : "no", config.SkillExemptRealMasterBots ? 1 : 0, config.SkillGate.CurveExponent);
    handler->PSendSysMessage(
        "  effective: known {}, effort {}, react factor {:.3f}, notice miss {} %, delay up to {} ms",
        full ? "all" : FormatTacticMask(gate.KnownMask), full ? SKILL_GATE_FULL : gate.Effort,
        exempt ? 1.0f : botAI->GetEffectiveSkillReactFactor(), exempt ? 0u : botAI->GetSkillNoticeMissPercent(),
        exempt ? 0u : botAI->GetSkillNoticeDelayMs());
    ReactionState const& reaction = botAI->GetReactionState();
    handler->PSendSysMessage("  natural reaction: {} (contexts {}); median {:.0f} ms, sigma {:.3f}",
                             config.NaturalReactionActive ? "on" : "off",
                             FormatTacticScope(config.NaturalReactionContexts), std::exp(reaction.Mu), reaction.Sigma);
    ReactionParams const& timing = config.NaturalReaction;
    handler->PSendSysMessage(
        "  natural timing: cadence {} (contexts {}), check {} ms, stagger {} ms; here: scope {}, combat {}, faster "
        "check {}, stretch {}; end to end {} (with the faster check; ping {} ms, display {} ms, ticks {})",
        config.NaturalCadenceEnable ? "on" : "off", FormatTacticScope(config.NaturalCadenceContexts),
        config.NaturalCadenceCheckMs, config.NaturalCadenceStaggerMs, botAI->IsInNaturalTimingScope() ? "yes" : "no",
        botAI->IsInNaturalTimingCombat() ? "yes" : "no", botAI->IsNaturalCadenceActive() ? "yes" : "no",
        botAI->IsSkillStretchOff() ? "off" : "on", timing.EndToEnd ? "on" : "off", timing.PingMs, timing.DisplayMs,
        timing.ServerTickMs < 0 ? std::string("half the map update")
                                : Acore::StringFormat("{} ms", timing.ServerTickMs));
    ReactionTypeState const& prices = botAI->GetReactionTypes();
    auto const onOff = [](bool on) { return on ? "on" : "off"; };
    handler->PSendSysMessage(
        "  reaction types: choice {} ({:.0f} ms per doubling), load {} ({} ms, {} % per event, top {:.2f}), ease {} "
        "({}/{}/{}/{} ms), think {} ({}/{} ms, deep {} %, wait {} ms), anticipate {} ({:.2f}), queue {} ({}-{} ms), "
        "reeval {}, busy skip {}, memory {}, movement {}",
        onOff(timing.Choice.Enable), prices.HickMsPerDoubling, onOff(timing.Load.Enable), prices.LoadMsPerEvent,
        prices.LoadMissPercentPerEvent, prices.LoadTopProtect, onOff(timing.Ease.Enable), prices.EaseMs[0],
        prices.EaseMs[1], prices.EaseMs[2], prices.EaseMs[3], onOff(timing.Think.Enable), prices.ThinkShortcutMs,
        prices.ThinkDeepMs, prices.ThinkDeepPercent, prices.PatienceWaitMs, onOff(timing.Anticipate.Enable),
        prices.AnticipateShare, onOff(timing.Queue.Enable), prices.QueueMinMs, prices.QueueMaxMs,
        onOff(timing.Reeval.Enable), onOff(timing.Reeval.BusySkip), onOff(timing.Memory.Enable),
        onOff(timing.MovementEnable));
    handler->PSendSysMessage("  cached: effort {}, style aggression {} / caution {} / patience {}", gate.Effort,
                             botAI->GetSkillStyle(SkillStyleTrait::Aggression),
                             botAI->GetSkillStyle(SkillStyleTrait::Caution),
                             botAI->GetSkillStyle(SkillStyleTrait::Patience));
    uint32 const attributeTemplate = botAI->GetAttributeTemplate();
    handler->PSendSysMessage(
        "  attributes ({}{}): {}",
        AttributesAtDefaults(config.SkillAttributes) ? "defaults: every ability is the skill" : "drawn",
        attributeTemplate ? Acore::StringFormat(", from template guid {}", attributeTemplate) : std::string(),
        FormatBotAttributes(botAI->GetAttributes()));

    std::array<ObjectGuid, TACTIC_FIGHT_ENEMIES_MAX> enemies;
    uint32 const enemyCount = botAI->GetFightEnemiesSnapshot(enemies);
    std::string enemyNames;
    for (uint32 index = 0; index < enemyCount; ++index)
        enemyNames += (enemyNames.empty() ? "" : ", ") + GuidName(enemies[index]);

    handler->PSendSysMessage("  fight enemies (debug snapshot, kept current in harness matches): {} [{}]", enemyCount,
                             enemyNames);

    std::string strategies;
    uint32 takenMs = 0;
    botAI->RequestStrategySnapshot();
    if (botAI->GetStrategySnapshot(strategies, takenMs))
        handler->PSendSysMessage("  strategies ({} ms old; a new snapshot is taken at the bot's next update): {}",
                                 GetMSTimeDiffToNow(takenMs), strategies);
    else
        handler->PSendSysMessage("  strategies: requested, taken at the bot's next update (show again)");

    return true;
}

bool TacticCommands::HandleStatsCommand(ChatHandler* handler, char const* args)
{
    std::string const verb = Trim(args ? args : "");
    if (verb == "reset")
    {
        for (auto& row : Activations)
        {
            for (std::atomic<uint64>& counter : row)
                counter.store(0, std::memory_order_relaxed);
        }

        handler->PSendSysMessage("TACTICS stats reset.");
        return true;
    }

    if (!verb.empty())
    {
        handler->PSendSysMessage("Usage: .playerbots tactics stats [reset]");
        return false;
    }

    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    handler->PSendSysMessage(
        "TACTICS stats: Playerbots.Tactics.Enable {}, config mask {}; activations per context "
        "(Arena / Battleground / World / Pve)",
        config.TacticsEnable ? 1 : 0, FormatTacticMask(config.TacticsConfigMask));
    uint64 total = 0;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        std::array<uint64, TACTIC_CONTEXT_COUNT> counts{};
        for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
        {
            counts[context] = Activations[index][context].load(std::memory_order_relaxed);
            total += counts[context];
        }

        handler->PSendSysMessage("  {}: {} / {} / {} / {} (scope {})", TACTIC_DEFINITIONS[index].Name, counts[0],
                                 counts[1], counts[2], counts[3], FormatTacticScope(config.TacticConfig[index].Scope));
    }

    handler->PSendSysMessage("  total {}", total);
    return true;
}

bool TacticCommands::HandleSelftestCommand(ChatHandler* handler, char const* args)
{
    std::string const name = Lower(Trim(args ? args : ""));
    if (!name.empty() && name != "scope" && name != "gate" && name != "reaction" && name != "attributes")
    {
        handler->PSendSysMessage("No tactics selftest '{}'. Known: scope, gate, reaction, attributes", name);
        return false;
    }

    if (name.empty() || name == "scope")
        RunScopeSelftest(handler);

    if (name.empty() || name == "gate")
        RunGateSelftest(handler);

    if (name.empty() || name == "reaction")
        RunReactionSelftest(handler);

    if (name.empty() || name == "attributes")
        RunAttributesSelftest(handler);

    return true;
}
