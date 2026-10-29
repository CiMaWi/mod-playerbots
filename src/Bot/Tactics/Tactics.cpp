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

    handler->PSendSysMessage("TACTICS selftest reaction: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS", checks,
                             failures);
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
    handler->PSendSysMessage("  cached: effort {}, style aggression {} / caution {} / patience {}", gate.Effort,
                             botAI->GetSkillStyle(SkillStyleTrait::Aggression),
                             botAI->GetSkillStyle(SkillStyleTrait::Caution),
                             botAI->GetSkillStyle(SkillStyleTrait::Patience));

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
    if (!name.empty() && name != "scope" && name != "gate" && name != "reaction")
    {
        handler->PSendSysMessage("No tactics selftest '{}'. Known: scope, gate, reaction", name);
        return false;
    }

    if (name.empty() || name == "scope")
        RunScopeSelftest(handler);

    if (name.empty() || name == "gate")
        RunGateSelftest(handler);

    if (name.empty() || name == "reaction")
        RunReactionSelftest(handler);

    return true;
}
