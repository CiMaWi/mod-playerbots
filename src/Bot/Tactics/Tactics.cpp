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

#include "ArenaCc.h"
#include "ArenaCcSpells.h"
#include "ArenaFocus.h"
#include "ArenaInterrupt.h"
#include "ArenaInterruptSpells.h"
#include "BotAttributes.h"
#include "BotProfileMgr.h"
#include "CcState.h"
#include "CcTacticStrategy.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "FocusState.h"
#include "Group.h"
#include "Item.h"
#include "NaturalReaction.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "SpellAuras.h"
#include "StringConvert.h"
#include "TacticGate.h"
#include "ThreadCheck.h"
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

// Pure checks of the target hold and the team plan (ArenaFocus.h) on synthetic enemies; the numbers are the conf.dist
// defaults (FocusConfig{}), never the server's config
void RunFocusSelftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TACTICS selftest focus FAILED: {}", what);
    };

    constexpr uint64 Seed = 4242;
    constexpr float Linear = 1.0f;
    constexpr uint32 Now = 100000;
    constexpr uint32 Windows = 200;
    FocusConfig const config{};
    // The attribute defaults: every ability the skill (0 at skill 0), every trait 50
    auto const plain = [](uint8 skill)
    {
        BotAttributeValues values;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = index < BOT_ABILITY_COUNT ? skill : 50;

        return values;
    };
    auto const with = [&](uint8 skill, BotAttribute attribute, uint8 value)
    {
        BotAttributeValues values = plain(skill);
        values.Value[static_cast<uint32>(attribute)] = value;
        return values;
    };
    auto const enemy = [](uint8 hp, bool healer)
    {
        EnemyInfo info;
        info.HpPct = hp;
        info.Healer = healer;
        info.LosCount = 2;
        info.InRangeCount = 2;
        return info;
    };
    auto const within = [](uint32 value, uint32 target, uint32 tolerance)
    { return value + tolerance >= target && value <= target + tolerance; };

    // Parameters: skill 0, the attribute defaults, Wisdom as the lerp position, the research anchors, the traits
    FocusParams const top = ComputeFocusParams(99, plain(99), Linear, config);
    FocusParams const full = ComputeFocusParams(0, plain(0), Linear, config);
    check(full.Hold == top.Hold && full.Team == top.Team, "skill 0 does not hold and plan like skill 99");

    FocusParams const mid = ComputeFocusParams(60, plain(60), Linear, config);
    float const at60 = SkillPosition(60);
    auto const lerp60 = [&](uint32 atSkill1, uint32 atSkill99)
    { return SkillLerpRounded(atSkill1, atSkill99, at60, Linear); };
    check(mid.Hold.HoldMs == lerp60(config.HoldMsAtSkill1, config.HoldMsAtSkill99) &&
              mid.Hold.HysteresisPct == lerp60(config.HoldHysteresisPctAtSkill1, config.HoldHysteresisPctAtSkill99) &&
              mid.Hold.ExecPct == lerp60(config.HoldExecPctAtSkill1, config.HoldExecPctAtSkill99) &&
              mid.Hold.FollowChance == lerp60(config.HoldFollowChanceAtSkill1, config.HoldFollowChanceAtSkill99) &&
              mid.Hold.FollowDelayMs == lerp60(config.HoldFollowDelayMsAtSkill1, config.HoldFollowDelayMsAtSkill99) &&
              mid.Team.HoldMs == lerp60(config.FocusHoldMsAtSkill1, config.FocusHoldMsAtSkill99) &&
              mid.Team.ExecPct == lerp60(config.FocusExecPctAtSkill1, config.FocusExecPctAtSkill99) &&
              mid.Team.StrongMargin == lerp60(config.FocusStrongMarginAtSkill1, config.FocusStrongMarginAtSkill99) &&
              mid.Team.RemarkDelayMs == lerp60(config.FocusRemarkDelayMsAtSkill1, config.FocusRemarkDelayMsAtSkill99) &&
              mid.Team.DefensiveSwapPct == config.FocusDefensiveSwapPct,
          "the attribute defaults do not give the plain skill lerp");

    FocusParams const wise = ComputeFocusParams(60, with(60, BotAttribute::Wisdom, 90), Linear, config);
    FocusParams const ninety = ComputeFocusParams(90, plain(90), Linear, config);
    check(wise.Team.HoldMs == ninety.Team.HoldMs && wise.Team.PlanIntervalMs == ninety.Team.PlanIntervalMs &&
              wise.Team.HysteresisPct == ninety.Team.HysteresisPct && wise.Hold.HoldMs == ninety.Hold.HoldMs &&
              wise.Team.RemarkDelayMs == mid.Team.RemarkDelayMs,
          "a skill-60 bot with Wisdom 90 does not hold and plan like a skill-90 bot");

    FocusParams const forty = ComputeFocusParams(40, plain(40), Linear, config);
    check(within(forty.Team.PlanIntervalMs, 1500, 30) && within(forty.Team.HoldMs, 4000, 30) &&
              within(forty.Team.HysteresisPct, 20, 1) && top.Team.PlanIntervalMs == 300 && top.Team.HoldMs == 2000 &&
              top.Team.HysteresisPct == 8,
          "the plan misses the research anchors (skill 40: 1500 ms, 4 s, 20; skill 99: 300 ms, 2 s, 8)");

    FocusParams const aggressive = ComputeFocusParams(60, with(60, BotAttribute::Aggression, 100), Linear, config);
    FocusParams const cautious = ComputeFocusParams(60, with(60, BotAttribute::Caution, 100), Linear, config);
    FocusParams const patient = ComputeFocusParams(60, with(60, BotAttribute::Patience, 100), Linear, config);
    FocusParams const social = ComputeFocusParams(60, with(60, BotAttribute::Teamwork, 100), Linear, config);
    check(aggressive.Team.ExecPct > mid.Team.ExecPct && aggressive.Hold.ExecPct > mid.Hold.ExecPct,
          "Aggression does not raise the execute thresholds");
    check(cautious.Team.DefensiveSwapPct > mid.Team.DefensiveSwapPct,
          "Caution does not widen the swap off a defensive");
    check(patient.Team.HoldMs > mid.Team.HoldMs && patient.Hold.HoldMs > mid.Hold.HoldMs,
          "Patience does not lengthen the holds");
    check(social.Hold.FollowChance > mid.Hold.FollowChance && social.Hold.FollowDelayMs < mid.Hold.FollowDelayMs &&
              social.Team.StrongMargin > mid.Team.StrongMargin,
          "Teamwork does not raise following and the margin a non-planner needs");

    // The plan of a skill-99 planner whose team can control
    TeamParams team = top.Team;
    team.CcCapable = true;
    TeamParams sure = team;
    sure.ExecChance = 100;
    TeamParams never = team;
    never.ExecChance = 0;
    uint32 const hold = team.HoldMs;

    std::array<EnemyInfo, 2> const even{enemy(60, false), enemy(60, false)};
    PlanState const start = Decide(even.data(), 2, PlanState{}, 1000, team, Seed);
    check(start.Kill == 0 && start.Cc == 1 && start.Reason == PlanReason::Start && start.HeldUntilMs == 1000 + hold,
          "equal enemies: the first plan is not enemy 0 held for HoldMs, enemy 1 controlled");
    PlanState onSecond = start;
    onSecond.Kill = 1;
    onSecond.Cc = 0;
    PlanState const kept = Decide(even.data(), 2, onSecond, 1000 + hold - 1, team, Seed);
    PlanState const after = Decide(even.data(), 2, onSecond, 1000 + hold, team, Seed);
    check(
        kept.Kill == 1 && kept.Reason == PlanReason::Held && after.Kill == 1 && after.Reason == PlanReason::Hysteresis,
        "equal enemies do not keep the kill target until the hold ends (and after it)");
    std::array<EnemyInfo, 2> const drop{enemy(60, false), enemy(40, false)};
    PlanState const dropHeld = Decide(drop.data(), 2, start, 1000 + hold - 1, team, Seed);
    PlanState const dropFree = Decide(drop.data(), 2, start, 1000 + hold, team, Seed);
    check(dropHeld.Kill == 0 && dropFree.Kill == 1 && dropFree.Reason == PlanReason::Score &&
              dropFree.HeldUntilMs == 1000 + 2 * hold,
          "a better enemy takes the kill before the hold ends, or not when it ends");

    std::array<EnemyInfo, 2> bubble = even;
    bubble[0].ImmuneAll = true;
    PlanState const swapped = Decide(bubble.data(), 2, start, 1001, team, Seed);
    check(swapped.Kill == 1 && swapped.Reason == PlanReason::Immune && swapped.Cc == FOCUS_NONE &&
              swapped.HeldUntilMs == 1001 + hold,
          "an immune kill target is not swapped in the same call (the immune one is no crowd-control target)");
    check(Decide(even.data(), 2, swapped, 1001 + hold, team, Seed).Kill == 1,
          "an immune target that comes back takes the kill back on an equal score");
    std::array<EnemyInfo, 2> wall = even;
    wall[0].HpPct = 80;
    wall[1].DefensiveUp = true;
    PlanState const back = Decide(wall.data(), 2, swapped, 1002, team, Seed);
    check(back.Kill == 0 && back.Reason == PlanReason::Defensive,
          "an immune target that comes back does not take the kill through the defensive rule");
    wall[0].HpPct = 90;
    TeamParams careful = team;
    careful.DefensiveSwapPct = cautious.Team.DefensiveSwapPct;
    check(Decide(wall.data(), 2, swapped, 1002, team, Seed).Kill == 1 &&
              Decide(wall.data(), 2, swapped, 1002, careful, Seed).Kill == 0,
          "a cautious planner does not swap off a defensive sooner");

    std::array<EnemyInfo, 2> sheep{enemy(20, true), enemy(90, false)};
    sheep[0].CcBreakable = true;
    PlanState const sheepStart = Decide(sheep.data(), 2, PlanState{}, 1000, team, Seed);
    PlanState const sheepHeld = Decide(sheep.data(), 2, start, 1001, team, Seed);
    check(
        sheepStart.Kill == 1 && sheepStart.Cc == 0 && sheepHeld.Kill == 1 && sheepHeld.Reason == PlanReason::Controlled,
        "an enemy in the team's breakable crowd control is the kill target");
    check(Decide(sheep.data(), 2, onSecond, 1001, sure, Seed).Kill == 1,
          "an enemy in the team's breakable crowd control is taken as an execute");

    std::array<EnemyInfo, 2> low{enemy(60, false), enemy(10, false)};
    PlanState const executed = Decide(low.data(), 2, start, 1001, sure, Seed);
    check(executed.Kill == 1 && executed.Reason == PlanReason::Execute &&
              Decide(low.data(), 2, start, 1001, never, Seed).Reason == PlanReason::Held,
          "an execute does not override the hold at chance 100, or does at chance 0");
    low[1].DefensiveUp = true;
    check(Decide(low.data(), 2, start, 1001, sure, Seed).Kill == 0,
          "an execute is taken on an enemy with a defensive up");
    std::array<EnemyInfo, 2> flip{enemy(60, false), enemy(57, false)};
    check(Decide(flip.data(), 2, start, 1000 + hold, team, Seed).Reason == PlanReason::Hysteresis,
          "hysteresis does not stop a 3-point flip");
    flip[1].HpPct = static_cast<uint8>(60 - team.HysteresisPct);
    check(Decide(flip.data(), 2, start, 1000 + hold, team, Seed).Reason == PlanReason::Score,
          "a lead of the hysteresis does not switch after the hold");
    check(Decide(flip.data(), 2, start, 1000 + hold, team, Seed, 500).Reason == PlanReason::Hysteresis,
          "the switch cost does not raise the hysteresis");

    std::array<EnemyInfo, 2> pillar = even;
    pillar[0].LosCount = 0;
    pillar[0].LosLostMs = 1000;
    PlanState const glimpse = Decide(pillar.data(), 2, start, 1001, team, Seed);
    pillar[0].LosLostMs = 2000;
    PlanState const gone = Decide(pillar.data(), 2, start, 1001, team, Seed);
    check(glimpse.Kill == 0 && gone.Kill == 1 && gone.Reason == PlanReason::Lost,
          "line of sight lost for 1.0 s does not keep the target, or 2.0 s does not swap it");
    pillar[1].LosCount = 0;
    pillar[1].LosLostMs = 2000;
    check(Decide(pillar.data(), 2, start, 1001, team, Seed).Kill == 0,
          "a lost kill target is left for an enemy that is lost too");

    std::array<EnemyInfo, 2> order{enemy(60, false), enemy(70, true)};
    check(Decide(order.data(), 2, PlanState{}, 1000, team, Seed).Kill == 1,
          "a healer at 70 % does not outscore a damage dealer at 60 %");
    order[1].DefensiveUp = true;
    order[0].ImmunePhys = true;
    TeamParams physical = team;
    physical.PhysicalTeam = true;
    check(Decide(order.data(), 2, PlanState{}, 1000, team, Seed).Kill == 0 &&
              Decide(order.data(), 2, PlanState{}, 1000, physical, Seed).Kill == 1,
          "a defensive up or a physical immunity against a physical team does not lower the score");

    std::array<EnemyInfo, 3> three{enemy(30, false), enemy(80, true), enemy(80, false)};
    three[1].DrLevel = 2;
    PlanState const trio = Decide(three.data(), 3, PlanState{}, 1000, team, Seed);
    three[1].Healer = false;
    three[2].DrLevel = 1;
    PlanState const trioDr = Decide(three.data(), 3, PlanState{}, 1000, team, Seed);
    TeamParams noCc = team;
    noCc.CcCapable = false;
    check(trio.Kill == 0 && trio.Cc == 1 && trioDr.Kill == 0 && trioDr.Cc == 2,
          "3v3: the crowd-control target is not the healer first, then the lowest diminishing returns");
    check(Decide(three.data(), 3, PlanState{}, 1000, noCc, Seed).Cc == FOCUS_NONE,
          "a team that cannot control gets a crowd-control target");

    // The same input twice gives the same plan; a 50 % execute is taken in some windows, not in all
    TeamParams coin = team;
    coin.ExecChance = 50;
    std::array<EnemyInfo, 2> const coinPair{enemy(60, false), enemy(10, false)};
    bool same = true;
    uint32 executes = 0;
    for (uint32 window = 0; window < Windows; ++window)
    {
        uint32 const now = 1001 + window * FOCUS_EXECUTE_WINDOW_MS;
        PlanState held = start;
        held.HeldUntilMs = now + hold;
        PlanState const first = Decide(coinPair.data(), 2, held, now, coin, Seed);
        PlanState const second = Decide(coinPair.data(), 2, held, now, coin, Seed);
        same = same && first == second;
        executes += first.Reason == PlanReason::Execute ? 1 : 0;
    }
    check(same, "the same input twice gives two different plans");
    check(executes > 0 && executes < Windows, "a 50 % execute chance is taken in every window or in none");

    // One bot's hold: tunnel vision at skill 1, quick switches for a reason at skill 99
    HoldParams const low1 = ComputeFocusParams(1, plain(1), Linear, config).Hold;
    HoldInput swing;
    swing.HasTarget = true;
    swing.TargetHpPct = 70;
    swing.CandidateHpPct = 40;
    swing.Advantage = 30;
    HoldResult const swingLate = HoldDecision(swing, Now, low1, Seed);
    check(
        !swingLate.Allow && swingLate.Reason == HoldReason::Hysteresis && !HoldDecision(swing, 1000, low1, Seed).Allow,
        "a skill-1 hold does not keep its target through a 30-point health swing");
    HoldInput lowExecute = swing;
    lowExecute.CandidateHpPct = 5;
    lowExecute.Advantage = 65;
    check(!HoldDecision(lowExecute, Now, low1, Seed).Allow, "a skill-1 hold leaves its target for an execute");
    HoldInput dead = swing;
    dead.TargetAlive = false;
    HoldInput away = swing;
    away.TargetInRange = false;
    HoldInput immune = swing;
    immune.TargetImmune = true;
    HoldInput hidden = swing;
    hidden.TargetLosLostMs = 2000;
    HoldInput glance = swing;
    glance.TargetLosLostMs = 1000;
    check(HoldDecision(dead, 1000, low1, Seed).Reason == HoldReason::Dead &&
              HoldDecision(away, 1000, low1, Seed).Reason == HoldReason::OutOfRange &&
              HoldDecision(immune, 1000, low1, Seed).Reason == HoldReason::Immune &&
              HoldDecision(hidden, 1000, low1, Seed).Reason == HoldReason::LosLost &&
              !HoldDecision(glance, 1000, low1, Seed).Allow,
          "a skill-1 hold does not leave a dead, far, immune or lost (2.0 s; not 1.0 s) target");
    HoldParams const high = top.Hold;
    check(HoldDecision(swing, 1000, high, Seed).Reason == HoldReason::HoldActive &&
              HoldDecision(swing, high.HoldMs, high, Seed).Reason == HoldReason::Free,
          "a skill-99 hold does not switch for a reason after its hold");
    HoldParams sureHold = high;
    sureHold.ExecChance = 100;
    HoldInput highExecute = swing;
    highExecute.CandidateHpPct = 10;
    check(HoldDecision(highExecute, 100, sureHold, Seed).Reason == HoldReason::Execute &&
              HoldDecision(HoldInput{}, 100, high, Seed).Reason == HoldReason::NoTarget,
          "an execute does not override a bot's hold, or a bot without a target is held");

    // Following the partner's target without marks: never at skill 1, late, sometimes at mid skill
    HoldParams alwaysFollow = high;
    alwaysFollow.FollowChance = 100;
    check(!FollowPartnerDecision(0, Now, low1, Seed), "a skill-1 bot follows its partner's target");
    check(!FollowPartnerDecision(Now - alwaysFollow.FollowDelayMs + 1, Now, alwaysFollow, Seed) &&
              FollowPartnerDecision(Now - alwaysFollow.FollowDelayMs, Now, alwaysFollow, Seed),
          "following is not late by FollowDelayMs");
    uint32 follows = 0;
    for (uint32 window = 0; window < Windows; ++window)
        follows += FollowPartnerDecision(0, Now + window * FOCUS_FOLLOW_WINDOW_MS, mid.Hold, Seed) ? 1 : 0;

    check(follows > 0 && follows < Windows, "a skill-60 bot follows its partner always or never");

    // Re-marks: only on events, at most one per MinRemarkSeconds, after the human delay; a non-planner only past the
    // strong margin and never within HumanMarkRespectSeconds of a human's mark
    RemarkInput planner;
    planner.Planner = true;
    planner.Marked = true;
    planner.LastMarkMs = Now - team.MinRemarkMs;
    planner.EventAtMs = Now - team.RemarkDelayMs;
    check(RemarkDecision(planner, Now, team).Reason == RemarkReason::NoEvent, "a re-mark comes without an event");
    planner.Event = RemarkEvent::KillDead;
    RemarkInput soon = planner;
    soon.LastMarkMs = Now - team.MinRemarkMs + 1;
    RemarkInput early = planner;
    early.EventAtMs = Now - team.RemarkDelayMs + 1;
    RemarkInput first;
    first.Planner = true;
    first.Event = RemarkEvent::Start;
    first.EventAtMs = Now - team.RemarkDelayMs;
    check(RemarkDecision(planner, Now, team).Allow && RemarkDecision(first, Now, team).Allow,
          "the planner may not mark at the start or re-mark on an event");
    check(RemarkDecision(soon, Now, team).Reason == RemarkReason::TooSoon &&
              RemarkDecision(early, Now, team).Reason == RemarkReason::Delay,
          "a re-mark comes within MinRemarkSeconds or before the human delay");
    RemarkInput follower = planner;
    follower.Planner = false;
    follower.Margin = team.StrongMargin - 1;
    RemarkInput strong = follower;
    strong.Margin = team.StrongMargin;
    check(RemarkDecision(follower, Now, team).Reason == RemarkReason::Margin && RemarkDecision(strong, Now, team).Allow,
          "a non-planner re-marks below the strong margin, or not at it");
    RemarkInput human = strong;
    human.Margin = 1000;
    human.HumanMarked = true;
    human.LastHumanMarkMs = Now - team.HumanMarkRespectMs + 1;
    RemarkInput humanLater = human;
    humanLater.LastHumanMarkMs = Now - team.HumanMarkRespectMs;
    check(RemarkDecision(human, Now, team).Reason == RemarkReason::HumanMark &&
              RemarkDecision(humanLater, Now, team).Allow,
          "a re-mark comes within HumanMarkRespectSeconds of a human's mark, or never after it");

    // Who plans: the alive bot with the higher skill at or above the threshold, ties to the lower guid; none with a
    // real player in the team
    std::array<PlannerCandidate, 2> bots{PlannerCandidate{2001, 50, true, false},
                                         PlannerCandidate{2002, 70, true, false}};
    check(ElectPlanner(bots.data(), 2, 40) == 1, "the higher skill is not the planner");
    bots[1].Skill = 50;
    check(ElectPlanner(bots.data(), 2, 40) == 0, "a skill tie does not go to the lower guid");
    bots[0].Alive = false;
    check(ElectPlanner(bots.data(), 2, 40) == 1, "a dead bot plans");
    bots[0].Alive = true;
    bots[0].Skill = 30;
    bots[1].Skill = 30;
    check(ElectPlanner(bots.data(), 2, 40) == FOCUS_NONE && ElectPlanner(bots.data(), 2, 0) == 0,
          "the planner threshold is not applied (40) or not open (0)");
    bots[1].Skill = 0;
    check(ElectPlanner(bots.data(), 2, 40) == 1, "skill 0 (full ability) does not plan");
    bots[0].RealPlayer = true;
    check(ElectPlanner(bots.data(), 2, 0) == FOCUS_NONE, "a bot plans in a real player's team");

    // G1a review NOTE 1: the score rule never takes an enemy no own bot has seen for longer than LosLostMs
    std::array<EnemyInfo, 2> unseen{enemy(60, false), enemy(20, false)};
    unseen[1].LosCount = 0;
    unseen[1].LosLostMs = team.LosLostMs + 500;
    check(Decide(unseen.data(), 2, start, 1000 + hold, team, Seed).Kill == 0,
          "the score rule takes an enemy out of every own bot's sight");

    // NOTE 3: the plan's reasons as remark events; a switch by score alone never marks
    std::array<RemarkEvent, static_cast<size_t>(PlanReason::Count)> const events = {
        RemarkEvent::None,       RemarkEvent::None,    RemarkEvent::Start,   RemarkEvent::KillDead,
        RemarkEvent::KillImmune, RemarkEvent::Opening, RemarkEvent::Opening, RemarkEvent::Opening,
        RemarkEvent::Opening,    RemarkEvent::None,    RemarkEvent::None,    RemarkEvent::None};
    bool mapped = true;
    for (size_t index = 0; index < events.size(); ++index)
        mapped = mapped && EventOfPlanReason(static_cast<PlanReason>(index)) == events[index];

    check(mapped, "a plan reason maps to the wrong remark event (score must map to none)");

    // NOTE 2: addon-level knowledge from AddonKnowledgeFromSkill (Knowledge), always at skill 0
    check(FocusAddonKnowledge(0, 0, 40) && FocusAddonKnowledge(50, 40, 40) && !FocusAddonKnowledge(50, 39, 40),
          "addon-level knowledge does not start at the threshold, or skill 0 lacks it");

    // G1b-c: the moon by mode (0 never, 1 a class that can control, 2 a team that also plays CC), kept while its CC
    // lands, and the skull's hold off a moon nobody controls only in mode 2
    check(!FocusMoonCapable(FOCUS_MOON_OFF, true, true) && FocusMoonCapable(FOCUS_MOON_ALWAYS, true, false) &&
              !FocusMoonCapable(FOCUS_MOON_ALWAYS, false, true) && FocusMoonCapable(FOCUS_MOON_USED, true, true) &&
              !FocusMoonCapable(FOCUS_MOON_USED, true, false),
          "the moon by mode is not: never, a class that can control, a team that plays crowd control");
    check(FocusMoonKept(Now, Now + 5000, 5000) && !FocusMoonKept(Now, Now + 5001, 5000),
          "a moon without a CC landing is not released after the grace, or is released before");
    check(FocusMarkHolds(FOCUS_MOON_ALWAYS, true, false) && FocusMarkHolds(FOCUS_MOON_USED, false, false) &&
              FocusMarkHolds(FOCUS_MOON_USED, true, true) && !FocusMarkHolds(FOCUS_MOON_USED, true, false),
          "the skull's hold keeps a bot off a moon nobody controls in mode 2, or lets it go in mode 1");

    handler->PSendSysMessage("TACTICS selftest focus: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS", checks,
                             failures);
}

// The core's DR rules for one player target of player casters (Unit::GetDiminishing, IncrDiminishing,
// ApplyDiminishingAura and ApplyDiminishingToDuration's table, Spell::DoSpellHitOnUnit's order: the level is read, the
// count raised, then the duration cut), written apart from DrMemory so that the selftest proves the memory against
// them without a live unit (G4b's lab run checks the core itself through the atImmune counter)
struct CcReferenceDr
{
    std::array<bool, CC_DR_GROUP_COUNT> Exists{};
    std::array<uint8, CC_DR_GROUP_COUNT> HitCount{};
    std::array<bool, CC_DR_GROUP_COUNT> Active{};     // an aura of the group is up (stack > 0)
    std::array<uint32, CC_DR_GROUP_COUNT> EndMs{};    // when the group's aura is removed, while Active
    std::array<uint32, CC_DR_GROUP_COUNT> HitTime{};  // when the group's last aura was removed

    void Advance(uint32 nowMs)
    {
        for (uint8 group = 0; group < CC_DR_GROUP_COUNT; ++group)
        {
            if (Active[group] && nowMs >= EndMs[group])
            {
                Active[group] = false;
                HitTime[group] = EndMs[group];
            }
        }
    }

    uint8 Level(uint8 group, uint32 nowMs)
    {
        Advance(nowMs);
        if (!Exists[group] || !HitCount[group])
            return 0;

        if (!Active[group] && nowMs - HitTime[group] > CC_DR_RESET_MS)
        {
            HitCount[group] = 0;
            return 0;
        }

        return HitCount[group];
    }

    // The duration the CC lands for, 0 when the target is immune
    uint32 Land(CcSpellData const& spell, uint32 nowMs)
    {
        constexpr std::array<float, CC_DR_LEVEL_IMMUNE + 1> Mods = {1.0f, 0.5f, 0.25f, 0.0f};
        uint8 const group = spell.Group;
        uint8 const level = Level(group, nowMs);
        bool const counts = spell.DrType == DRTYPE_ALL || spell.DrType == DRTYPE_PLAYER;
        if (counts && !Exists[group])
        {
            Exists[group] = true;
            HitCount[group] = 1;
            HitTime[group] = nowMs;
        }
        else if (counts && HitCount[group] < CC_DR_LEVEL_IMMUNE)
            ++HitCount[group];

        int32 duration = static_cast<int32>(spell.BaseMs);
        if (spell.PvpCapMs > 0 && duration > static_cast<int32>(spell.PvpCapMs))
            duration = static_cast<int32>(spell.PvpCapMs);

        float const mod = counts ? Mods[std::min<uint8>(level, CC_DR_LEVEL_IMMUNE)] : 1.0f;
        duration = static_cast<int32>(duration * mod);
        if (duration <= 0)
            return 0;

        uint32 const endMs = nowMs + static_cast<uint32>(duration);
        EndMs[group] = Active[group] ? std::max(EndMs[group], endMs) : endMs;
        Active[group] = true;
        return static_cast<uint32>(duration);
    }

    // The group's aura removed early (damage, trinket, dispel)
    void EndEarly(uint8 group, uint32 atMs)
    {
        if (Active[group] && atMs < EndMs[group])
            EndMs[group] = atMs;
    }

    // The CC aura a bot sees on the target: the group whose aura ends last, 0 for none
    void Visible(uint32 nowMs, uint8& group, uint32& endMs)
    {
        Advance(nowMs);
        group = 0;
        endMs = 0;
        for (uint8 index = 1; index < CC_DR_GROUP_COUNT; ++index)
        {
            if (Active[index] && (!group || EndMs[index] > endMs))
            {
                group = index;
                endMs = EndMs[index];
            }
        }
    }
};

// tactics force dr's group: a DR group name of PvpTestCounters.h (any case) that counts as CC, else DIMINISHING_NONE
uint8 CcDrGroupByName(std::string const& text)
{
    std::string const wanted = Lower(text);
    for (uint8 group = 0; group < PVPTEST_DR_GROUP_COUNT; ++group)
    {
        if (IsPvpTestCcGroup(group) && Lower(PVPTEST_DR_GROUP_NAMES[group]) == wanted)
            return group;
    }

    return DIMINISHING_NONE;
}

std::string CcDrGroupList()
{
    std::string names;
    for (uint8 group = 0; group < PVPTEST_DR_GROUP_COUNT; ++group)
    {
        if (IsPvpTestCcGroup(group))
            names += (names.empty() ? "" : ", ") + std::string(PVPTEST_DR_GROUP_NAMES[group]);
    }

    return names;
}

// tactics force dr (G4c): the bot casts the group's CC (CcForceDrRow, rank 1) on the enemy `level` times, triggered
// (no cast time, cost or cooldown), each one landing anew (its aura of the bot removed first), so the core's DR and
// the team's memories (the aura hooks) count them as any CC; the last one stays up. The world thread while the maps
// wait; the landings are not chains (CcSetForcedCasts).
bool ForceCcDr(ChatHandler* handler, PlayerbotAI* botAI, std::string const& name, ObjectGuid enemy, char const* source,
               std::string const& groupText, std::string const& levelText)
{
    uint8 const drGroup = CcDrGroupByName(groupText);
    uint32 const level = Acore::StringTo<uint32>(levelText).value_or(0);
    if (!drGroup || level < 1 || level > CC_DR_LEVEL_IMMUNE)
    {
        handler->PSendSysMessage("Usage: .playerbots tactics force dr <bot name or guid> <group> <1-{}>; groups: {}",
                                 static_cast<uint32>(CC_DR_LEVEL_IMMUNE), CcDrGroupList());
        handler->SetSentErrorMessage(true);
        return false;
    }

    Player* const bot = botAI->GetBot();
    Unit* const target = botAI->GetUnit(enemy);
    if (!target || !target->IsInMap(bot) || !target->IsAlive())
    {
        handler->PSendSysMessage("TACTICS force dr: {} ({}) is not alive on {}'s map.", GuidName(enemy), source, name);
        handler->SetSentErrorMessage(true);
        return false;
    }

    // G4c-a review NOTE 2: real casts go only on an enemy (not the GM or a teammate)
    if (!bot->IsValidAttackTarget(target))
    {
        handler->PSendSysMessage("TACTICS force dr: {} ({}) is no enemy {} may attack.", GuidName(enemy), source, name);
        handler->SetSentErrorMessage(true);
        return false;
    }

    int8 const row = CcForceDrRow(drGroup, target->IsPlayer(), static_cast<uint8>(target->GetCreatureType()));
    if (row == CC_NO_ROW)
    {
        handler->PSendSysMessage("TACTICS force dr: no CC row of group {} lands on {}.",
                                 PVPTEST_DR_GROUP_NAMES[drGroup], GuidName(enemy));
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 const spellId = GetCcSpellTable()[row].AuraSpellId;
    std::string casts;
    CcSetForcedCasts(true);
    for (uint32 cast = 0; cast < level; ++cast)
    {
        if (cast)
            target->RemoveAurasDueToSpell(spellId, bot->GetGUID());

        SpellCastResult const result = bot->CastSpell(target, spellId, true);
        Aura const* const aura = target->GetAura(spellId, bot->GetGUID());
        casts += (casts.empty() ? "" : ", ") +
                 (aura ? Acore::StringFormat("{} ms", aura->GetDuration())
                       : Acore::StringFormat("none (cast result {})", static_cast<uint32>(result)));
    }

    CcSetForcedCasts(false);

    // What each bot of the team believes now (its own memory) against the core's count
    uint32 const nowMs = getMSTime();
    std::string memories;
    auto const note = [&](Player* member)
    {
        PlayerbotAI* const memberAI = member && member->IsInMap(bot) ? GET_PLAYERBOT_AI(member) : nullptr;
        if (!memberAI)
            return;

        CcBotState const& state = memberAI->GetCcState();
        memories += Acore::StringFormat("{}{} {}{}", memories.empty() ? "" : ", ", member->GetName(),
                                        static_cast<uint32>(state.Memory.Level(enemy.GetRawValue(), drGroup, nowMs)),
                                        state.Params.DrAware ? "" : " (not DR aware)");
    };

    Group* const group = bot->GetGroup();
    if (!group)
        note(bot);
    else
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            note(ref->GetSource());
    }

    handler->PSendSysMessage(
        "TACTICS force dr: {} cast {} ({}) {} times on {} ({}): [{}]; core level {}; memories [{}]; at {} ms", name,
        CC_OPTIONS[row].Action, spellId, level, GuidName(enemy), source, casts,
        static_cast<uint32>(target->GetDiminishing(static_cast<DiminishingGroup>(drGroup))), memories, nowMs);
    return true;
}

// G5b: the Interrupt tactic's pure policy (ArenaInterrupt.h) and its spell facts (ArenaInterruptSpells.h, DBC)
void RunInterruptSelftest(ChatHandler* handler)
{
    namespace Int = ArenaInterrupt;
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TACTICS selftest interrupt FAILED: {}", what);
    };

    // Classify: a CC beats a heal, a heal beats damage
    Int::SpellFacts facts;
    check(Int::Classify(facts) == Int::Kind::Other, "a cast with no heal, CC or damage is other");
    facts.Damages = true;
    check(Int::Classify(facts) == Int::Kind::Damage, "damage is damage");
    facts.Heals = true;
    check(Int::Classify(facts) == Int::Kind::Heal, "a heal that also damages is a heal");
    facts.Controls = true;
    check(Int::Classify(facts) == Int::Kind::Cc, "a CC beats a heal");
    Int::SpellFacts damagingCc;
    damagingCc.Controls = true;
    damagingCc.Damages = true;
    check(Int::Classify(damagingCc) == Int::Kind::Cc, "a damaging CC is a CC");

    // Real spells from the DBC: polymorph, fireball, flash heal, greater heal, cyclone, fear, regrowth, frostbolt (a
    // snare is no CC)
    constexpr std::array<std::pair<uint32, Int::Kind>, 8> Spells = {{{118, Int::Kind::Cc},
                                                                     {133, Int::Kind::Damage},
                                                                     {2061, Int::Kind::Heal},
                                                                     {2060, Int::Kind::Heal},
                                                                     {33786, Int::Kind::Cc},
                                                                     {5782, Int::Kind::Cc},
                                                                     {8936, Int::Kind::Heal},
                                                                     {116, Int::Kind::Damage}}};
    for (auto const& [spellId, expected] : Spells)
    {
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(spellId);
        Int::Kind const kind = info ? Int::Classify(InterruptSpellFactsOf(info)) : Int::Kind::Other;
        check(info && kind == expected,
              Acore::StringFormat("spell {} ({}) is {}, not {}", spellId, info ? info->SpellName[0] : "missing",
                                  Int::KindName(expected), Int::KindName(kind)));
    }

    // The bot's own interrupts: kick, pummel, counterspell kick; kidney shot stuns; fireball does neither
    constexpr std::array<std::pair<uint32, InterruptUse>, 5> Uses = {{{1766, InterruptUse::Kick},
                                                                      {6552, InterruptUse::Kick},
                                                                      {2139, InterruptUse::Kick},
                                                                      {408, InterruptUse::Stun},
                                                                      {133, InterruptUse::None}}};
    for (auto const& [spellId, expected] : Uses)
    {
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(spellId);
        check(info && InterruptUseOf(info) == expected,
              Acore::StringFormat("spell {} interrupts as {}", spellId, static_cast<uint32>(expected)));
    }

    // Score: a heal on a low friend > a CC > a heal > damage > other (none)
    constexpr uint8 Urgent = 30;
    check(Int::Score(Int::Kind::Heal, 20, Urgent) > Int::Score(Int::Kind::Cc, 100, Urgent),
          "a heal on a low friend outranks a CC");
    check(Int::Score(Int::Kind::Cc, 100, Urgent) > Int::Score(Int::Kind::Heal, 80, Urgent), "a CC outranks a heal");
    check(Int::Score(Int::Kind::Heal, 80, Urgent) > Int::Score(Int::Kind::Damage, 100, Urgent),
          "a heal outranks damage");
    check(Int::Score(Int::Kind::Damage, 100, Urgent) > 0 && Int::Score(Int::Kind::Other, 100, Urgent) == 0,
          "damage scores, other does not");

    // The deep path: known, then full ability or the think part off, else Wisdom's deep percent
    ReactionTypeState types;
    types.ThinkDeepPercent = 0;
    check(!Int::UsesDeep(false, true, true, types, 1), "a bot that does not know the tactic takes the shortcut");
    check(Int::UsesDeep(true, true, true, types, 1), "full ability judges");
    check(Int::UsesDeep(true, false, false, types, 1), "with the think part off a knowing bot judges");
    check(!Int::UsesDeep(true, false, true, types, 1), "deep percent 0 takes the shortcut");
    types.ThinkDeepPercent = 100;
    check(Int::UsesDeep(true, false, true, types, 1), "deep percent 100 judges");

    // The numbers at the conf.dist defaults; skill 0 has the skill-99 ones
    InterruptConfig const config{};
    constexpr float Exponent = 1.0f;
    constexpr uint8 Neutral = 50;
    constexpr uint8 KnownFromSkill = 50;  // the lab overlay's Playerbots.Tactics.Interrupt.KnownFromSkill
    Int::Params const p99 = Int::ComputeParams(99, Neutral, Exponent, config);
    Int::Params const p0 = Int::ComputeParams(0, Neutral, Exponent, config);
    check(p0.HealShare == p99.HealShare && p0.Sigma == p99.Sigma, "skill 0 has the skill-99 numbers");
    check(std::fabs(p99.HealShare - config.HealSharePctAtSkill99 / 100.0f) < 0.001f && p99.PatienceShare == 0.0f,
          "skill 99, Patience 50: the heal share is HealSharePctAtSkill99, no patience share");

    auto const kicksAt = [](Int::Decision const& decision, uint32 elapsedMs)
    { return decision.Kick && elapsedMs >= decision.CommitMs; };
    constexpr uint64 Hash = 0x5EEDULL;
    constexpr uint32 HealMs = 2500;
    Int::Input heal;
    heal.CastKind = Int::Kind::Heal;
    heal.CastMs = HealMs;
    heal.TargetHpPct = 70;
    heal.Deep = 99 >= KnownFromSkill;
    Int::Decision const skilled = Int::Decide(heal, p99, Hash);
    check(skilled.Kick && !kicksAt(skilled, HealMs / 5),
          Acore::StringFormat("a heal at 20 % waits at skill 99 (commit at {} ms)", skilled.CommitMs));
    check(skilled.Why == Int::Reason::Heal, "the skilled kick on a heal is a heal decision");
    Int::Input weak = heal;
    weak.Deep = 40 >= KnownFromSkill;
    Int::Decision const shortcut = Int::Decide(weak, Int::ComputeParams(40, Neutral, Exponent, config), Hash);
    check(kicksAt(shortcut, HealMs / 5) && shortcut.Why == Int::Reason::Shortcut,
          "a heal at 20 % is kicked at once at skill 40 (the shortcut)");
    check(kicksAt(shortcut, 0), "the shortcut kicks from the bar's start (the reaction model times it)");

    Int::Input poly;
    poly.Deep = true;
    poly.CastKind = Int::Kind::Cc;
    poly.CastMs = 1500;
    check(kicksAt(Int::Decide(poly, p99, Hash), 0), "a polymorph at 0 % is kicked");

    Int::Input fireball;
    fireball.Deep = true;
    fireball.CastKind = Int::Kind::Damage;
    fireball.CastMs = 3000;
    fireball.HealerInRange = true;
    Int::Decision const held = Int::Decide(fireball, p99, Hash);
    check(!held.Kick && held.Why == Int::Reason::HoldForHealer, "a fireball is skipped while a healer is in range");
    Int::Params noHold = p99;
    noHold.HoldForHealer = false;
    check(Int::Decide(fireball, noHold, Hash).Kick, "with HoldForHealer 0 the fireball is kicked");
    fireball.HealerInRange = false;
    Int::Decision const unheld = Int::Decide(fireball, p99, Hash);
    check(unheld.Kick && !kicksAt(unheld, 0) && kicksAt(unheld, 2700),
          Acore::StringFormat("a fireball without a healer in range is kicked at its share (commit at {} ms)",
                              unheld.CommitMs));

    Int::Input urgent = heal;
    urgent.TargetHpPct = 20;
    Int::Decision const urgentKick = Int::Decide(urgent, p99, Hash);
    check(kicksAt(urgentKick, HealMs / 10) && urgentKick.Why == Int::Reason::UrgentHeal,
          "a heal on a friend at 20 % health is kicked at once");
    Int::Input other;
    other.Deep = true;
    other.CastMs = 1500;
    check(!Int::Decide(other, p99, Hash).Kick, "an other cast gets no kick from a judging bot");

    // Commit shares spread around the configured value (skill 99, Patience 50); skill 50 commits earlier, a patient
    // bot later
    constexpr uint32 Draws = 400;
    auto const meanShare = [&](Int::Input const& input, Int::Params const& params, double* sd, float* low, float* high)
    {
        double sum = 0.0;
        double sumSquares = 0.0;
        for (uint32 draw = 0; draw < Draws; ++draw)
        {
            float const share = Int::Decide(input, params, SplitMix64(1000 + draw)).Share;
            sum += share;
            sumSquares += static_cast<double>(share) * share;
            if (low)
                *low = std::min(*low, share);

            if (high)
                *high = std::max(*high, share);
        }

        double const mean = sum / Draws;
        if (sd)
            *sd = std::sqrt(std::max(0.0, sumSquares / Draws - mean * mean));

        return mean;
    };
    double sd = 0.0;
    float low = 1.0f;
    float high = 0.0f;
    double const mean = meanShare(heal, p99, &sd, &low, &high);
    handler->PSendSysMessage(
        "TACTICS selftest interrupt: heal commit share over {} draws: mean {:.3f}, sd {:.3f}, {:.3f} to {:.3f} "
        "(configured {:.2f})",
        Draws, mean, sd, low, high, p99.HealShare);
    check(std::fabs(mean - p99.HealShare) < 0.03, "the commit shares centre on the configured heal share");
    check(sd > 0.03, "the commit shares spread");
    check(high > low && high <= p99.MaxShare + 1e-6f, "the commit shares are not constant and stay below MaxShare");
    double const mean50 = meanShare(heal, Int::ComputeParams(50, Neutral, Exponent, config), nullptr, nullptr, nullptr);
    check(mean50 < mean - 0.1, Acore::StringFormat("skill 50 commits earlier ({:.3f})", mean50));
    double const meanPatient =
        meanShare(heal, Int::ComputeParams(99, 100, Exponent, config), nullptr, nullptr, nullptr);
    check(meanPatient > mean + 0.05, Acore::StringFormat("a patient bot commits later ({:.3f})", meanPatient));

    // Three cancels seen from a caster raise a skilled kicker's commit share for that caster
    OpponentMemory memory;
    memory.Clear(1);
    constexpr uint32 Faker = 77;
    constexpr uint32 Honest = 78;
    constexpr uint32 GreaterHeal = 2060;
    constexpr uint32 Tolerance = 100;
    uint32 nowMs = 10000;
    for (uint32 cast = 0; cast < 3; ++cast)
    {
        memory.NoteCast(Faker, GreaterHeal, nowMs + HealMs, nowMs, Tolerance);
        memory.NoteCast(Honest, GreaterHeal, nowMs + HealMs, nowMs, Tolerance);
        memory.NoteCastGone(Faker, nowMs + 500, Tolerance, false);
        nowMs += HealMs;
        memory.NoteCastGone(Honest, nowMs, Tolerance, false);
        nowMs += 1000;
    }

    OpponentMemorySlot const* const fakerSlot = memory.Find(Faker);
    check(fakerSlot && fakerSlot->CancelsSeen == 3 && fakerSlot->CastsSeen == 3,
          "the memory saw three cancels in three casts");
    check(
        memory.FakeRate(Faker, config.CancelMinCasts) == 1.0f && memory.FakeRate(Honest, config.CancelMinCasts) == 0.0f,
        "fake rate 1 for the faker, 0 for the honest caster");
    Int::Input fakerHeal = heal;
    fakerHeal.FakeRate = memory.FakeRate(Faker, config.CancelMinCasts);
    double const meanFaker = meanShare(fakerHeal, p99, nullptr, nullptr, nullptr);
    uint32 earlier = 0;
    for (uint32 draw = 0; draw < Draws; ++draw)
    {
        if (Int::Decide(fakerHeal, p99, SplitMix64(1000 + draw)).Share <
            Int::Decide(heal, p99, SplitMix64(1000 + draw)).Share)
            ++earlier;
    }

    check(meanFaker > mean + 0.05,
          Acore::StringFormat("a skilled kicker waits longer on the faker ({:.3f})", meanFaker));
    check(earlier == 0, Acore::StringFormat("no draw commits earlier on the faker ({} did)", earlier));

    // The decision slots: a pushed-back bar is the same cast, the newest per enemy, the slot asked longest ago goes
    InterruptBotState state;
    InterruptCastSlot& first = state.Take(100);
    first.Guid = 5;
    first.SpellId = GreaterHeal;
    first.StartMs = 100;
    first.SeenMs = 100;
    check(state.Find(5, GreaterHeal, 400, HealMs / 2) == &first, "a pushed-back bar is the same cast");
    check(state.Find(5, GreaterHeal, 3000, HealMs / 2) == nullptr, "a new bar of the same spell is a new cast");
    InterruptCastSlot& second = state.Take(200);
    second.Guid = 5;
    second.SpellId = GreaterHeal;
    second.StartMs = 3000;
    second.SeenMs = 200;
    check(state.Latest(5) == &second, "the enemy's newest decision is its latest cast");
    state.Take(300).Guid = 6;
    state.Slots[2].SeenMs = 300;
    state.Take(400).Guid = 7;
    state.Slots[3].SeenMs = 400;
    check(&state.Take(5000) == &state.Slots[0], "a full table gives up the slot asked longest ago");

    handler->PSendSysMessage("TACTICS selftest interrupt: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS",
                             checks, failures);
}

void RunCcSelftest(ChatHandler* handler)
{
    static_assert(CC_DR_GROUP_COUNT == PVPTEST_DR_GROUP_COUNT, "the DR group names cover the core's groups");
    static_assert(CC_DR_GROUP_COUNT == DIMINISHING_DRAGONS_BREATH + 1, "the core's DiminishingGroup changed");
    static_assert(CC_DR_LEVEL_IMMUNE == DIMINISHING_LEVEL_IMMUNE, "the core's DR levels changed");

    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("TACTICS selftest cc FAILED: {}", what);
    };
    // |hits / total - percent| <= tolerance points
    auto const withinRate = [](uint32 hits, uint32 total, uint32 percent, uint32 tolerance)
    {
        int64 const difference = static_cast<int64>(hits) * 100 - static_cast<int64>(percent) * total;
        return (difference < 0 ? -difference : difference) <= static_cast<int64>(tolerance) * total;
    };

    constexpr uint64 Seed = 4343;
    constexpr uint64 Enemy = 7001;
    constexpr float Linear = 1.0f;
    constexpr uint8 Disorient = DIMINISHING_DISORIENT;
    constexpr uint8 Fear = DIMINISHING_FEAR;
    constexpr uint8 Stun = DIMINISHING_CONTROLLED_STUN;
    CcConfig const config{};
    // The attribute defaults: every ability the skill (0 at skill 0), every trait 50
    auto const plain = [](uint8 skill)
    {
        BotAttributeValues values;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = index < BOT_ABILITY_COUNT ? skill : 50;

        return values;
    };
    auto const params = [&](uint8 skill)
    { return ComputeCcParams(skill, plain(skill), SKILL_GATE_FULL, Linear, config); };
    auto const groupName = [](uint8 group)
    { return group < PVPTEST_DR_GROUP_COUNT ? PVPTEST_DR_GROUP_NAMES[group] : "?"; };
    auto const makeSpell = [](uint8 group, uint8 drType, uint32 baseMs, uint32 capMs, uint32 castMs, float rangeYards)
    {
        CcSpellData spell;
        spell.AuraSpellId = 1;
        spell.Group = group;
        spell.DrType = drType;
        spell.BaseMs = baseMs;
        spell.PvpCapMs = capMs;
        spell.CastMs = castMs;
        spell.RangeYards = rangeYards;
        spell.Valid = true;
        return spell;
    };
    auto const print = [&](std::string const& label, uint32 spellId, CcSpellData const& spell, bool rowBreaks)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        char const* spellName = info && info->SpellName[0] ? info->SpellName[0] : "?";
        handler->PSendSysMessage(
            "TACTICS selftest cc: {} (spell {} {}) -> group {} ({}), aura {}, base {} ms, cap {} ms, cast {} ms, range "
            "{:.0f} yd, damage breaks: row {} core {}",
            label, spellId, spellName, groupName(spell.Group), spell.Group, spell.AuraSpellId, spell.BaseMs,
            spell.PvpCapMs, spell.CastMs, spell.RangeYards, rowBreaks, spell.DamageInterrupts);
    };

    constexpr int8 Polymorph = CcOptionIndex("polymorph");
    constexpr int8 FearOption = CcOptionIndex("fear on cc");
    constexpr int8 Shackle = CcOptionIndex("shackle undead");
    constexpr int8 Sap = CcOptionIndex("sap");
    constexpr int8 Scream = CcOptionIndex("psychic scream");
    constexpr int8 Repentance = CcOptionIndex("repentance");
    static_assert(Polymorph != CC_NO_ROW && FearOption != CC_NO_ROW && Shackle != CC_NO_ROW && Sap != CC_NO_ROW &&
                      Scream != CC_NO_ROW && Repentance != CC_NO_ROW,
                  "the selftest's rows are in CC_OPTIONS");

    // 1. Every row's DR group, base duration, PvP cap, cast time and range from the core. The plan's facts: the groups
    // of repentance, freezing trap (its trap object's spell) and hex (a row since G4d; its groups are checked in 27).
    CcSpellTable const& spells = GetCcSpellTable();
    bool allValid = true;
    bool allControl = true;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        CcOption const& option = CC_OPTIONS[index];
        print(option.Action, option.SpellId, spells[index], option.BreaksOnDamage);
        allValid = allValid && spells[index].Valid;
        allControl = allControl && IsPvpTestCcGroup(spells[index].Group);
    }

    check(allValid, "an option's spell is missing from the spell store");
    check(allControl, "an option's spell has no crowd-control DR group in the core");
    check(spells[Polymorph].Group != spells[FearOption].Group, "polymorph and fear share a DR group");
    check(spells[Polymorph].PvpCapMs == 10000 && spells[Repentance].PvpCapMs == 6000 &&
              ExpectedMs(spells[Polymorph], 0, true) == std::min<uint32>(spells[Polymorph].BaseMs, 10000),
          "the PvP caps are not polymorph 10 s and repentance 6 s");

    // 2. Durations by level: the PvP cap on a player, then 1, 1/2, 1/4, immune; no cap and no count on a creature for
    // a player-only group; a group of every target counts on a creature; a limit-only group is capped, never cut
    CcSpellData const longFear = makeSpell(Fear, DRTYPE_PLAYER, 20000, 10000, 1500, 20.0f);
    check(ExpectedMs(longFear, 0, true) == 10000 && ExpectedMs(longFear, 1, true) == 5000 &&
              ExpectedMs(longFear, 2, true) == 2500 && ExpectedMs(longFear, 3, true) == 0,
          "durations by level on a player are not 10000/5000/2500/0");
    check(ExpectedMs(longFear, 2, false) == 20000, "a player-only group is capped or cut on a creature");
    CcSpellData const stunAll = makeSpell(Stun, DRTYPE_ALL, 6000, 10000, 0, 5.0f);
    check(ExpectedMs(stunAll, 1, false) == 3000 && ExpectedMs(stunAll, 3, false) == 0 &&
              ExpectedMs(stunAll, 0, true) == 6000,
          "a stun's DR does not count on a creature");
    CcSpellData const limitOnly = makeSpell(DIMINISHING_LIMITONLY, DRTYPE_NONE, 20000, 10000, 0, 30.0f);
    check(ExpectedMs(limitOnly, 2, true) == 10000, "a limit-only group is not capped or is cut");

    // 3. PickCc on a synthetic table
    CcSpellTable synthetic{};
    synthetic[Polymorph] = makeSpell(Disorient, DRTYPE_PLAYER, 50000, 10000, 1500, 30.0f);
    synthetic[FearOption] = makeSpell(Fear, DRTYPE_PLAYER, 20000, 10000, 1500, 20.0f);
    synthetic[Shackle] = makeSpell(Disorient, DRTYPE_PLAYER, 50000, 10000, 1500, 30.0f);
    synthetic[Sap] = makeSpell(Disorient, DRTYPE_PLAYER, 45000, 10000, 0, 10.0f);
    synthetic[Scream] = makeSpell(Fear, DRTYPE_PLAYER, 8000, 10000, 0, 0.0f);
    CcParams const aware = params(99);
    CcParams const unaware = params(20);
    CcPickInput input;
    input.ReadyMask = CcOptionBit(Polymorph);
    input.DistanceYards = 10.0f;
    input.Levels[Disorient] = CC_DR_LEVEL_IMMUNE;
    check(PickCc(input, synthetic, aware) == CC_NO_ROW, "a DR-aware bot casts into IMMUNE");
    check(PickCc(input, synthetic, unaware) == Polymorph, "a bot without DR awareness knows the target is immune");
    input.ReadyMask |= CcOptionBit(FearOption);
    check(PickCc(input, synthetic, aware) == FearOption, "a DR-aware bot does not take the other group at IMMUNE");
    input.Levels[Disorient] = 1;
    bool fresher = PickCc(input, synthetic, aware) == FearOption;
    input.Levels[Disorient] = 0;
    input.Levels[Fear] = 1;
    fresher = fresher && PickCc(input, synthetic, aware) == Polymorph;
    check(fresher, "a DR-aware bot does not prefer the group with less DR");
    input.Levels = {};
    input.ActiveGroup = Fear;
    bool alternates = PickCc(input, synthetic, aware) == Polymorph;
    input.ActiveGroup = Disorient;
    alternates = alternates && PickCc(input, synthetic, aware) == FearOption;
    input.ReadyMask = CcOptionBit(Polymorph);
    alternates =
        alternates && PickCc(input, synthetic, aware) == CC_NO_ROW && PickCc(input, synthetic, unaware) == Polymorph;
    check(alternates, "a DR-aware bot casts the DR group already on the target");
    input.ActiveGroup = 0;
    input.Levels[Disorient] = 2;
    check(PickCc(input, synthetic, aware) == CC_NO_ROW && PickCc(input, synthetic, unaware) == Polymorph,
          "a DR-aware bot casts a third CC of a group (a quarter of its duration)");
    CcPickInput undead;
    undead.ReadyMask = CcOptionBit(Shackle);
    undead.DistanceYards = 10.0f;
    bool playerOnly = PickCc(undead, synthetic, aware) == CC_NO_ROW;
    undead.TargetIsPlayer = false;
    undead.CreatureType = CREATURE_TYPE_BEAST;
    playerOnly = playerOnly && PickCc(undead, synthetic, aware) == CC_NO_ROW;
    undead.CreatureType = CREATURE_TYPE_UNDEAD;
    playerOnly = playerOnly && PickCc(undead, synthetic, aware) == Shackle;
    check(playerOnly, "a creature-only CC is cast on a player or a beast, or not on an undead creature");
    CcPickInput opener;
    opener.ReadyMask = CcOptionBit(Sap);
    opener.DistanceYards = 5.0f;
    bool stealth = PickCc(opener, synthetic, aware) == CC_NO_ROW;
    opener.Stealthed = true;
    stealth = stealth && PickCc(opener, synthetic, aware) == CC_NO_ROW;
    opener.TargetInCombat = false;
    stealth = stealth && PickCc(opener, synthetic, aware) == Sap;
    check(stealth, "sap is cast without stealth or on a target in combat");
    CcPickInput reach;
    reach.ReadyMask = CcOptionBit(FearOption);
    reach.DistanceYards = 25.0f;
    bool ranged = PickCc(reach, synthetic, aware) == CC_NO_ROW;
    reach.DistanceYards = 15.0f;
    ranged = ranged && PickCc(reach, synthetic, aware) == FearOption;
    reach.ReadyMask = CcOptionBit(Scream);
    reach.DistanceYards = 9.0f;
    ranged = ranged && PickCc(reach, synthetic, aware) == CC_NO_ROW;
    reach.DistanceYards = 7.0f;
    ranged = ranged && PickCc(reach, synthetic, aware) == Scream;
    check(ranged, "a CC out of its range or reach is picked, or one in range is not");
    // G4d-c: howl of terror before fear (one DR group) only in its reach and when it is instant or reaches a second
    // enemy; fear otherwise (it lasts longer)
    constexpr int8 HowlRow = CcOptionIndex("howl of terror");
    static_assert(HowlRow != CC_NO_ROW && (CC_SELF_AOE_ROWS & CcOptionBit(static_cast<uint8>(HowlRow))),
                  "the selftest's howl row is a self-centred row");
    synthetic[HowlRow] = makeSpell(Fear, DRTYPE_PLAYER, 8000, 10000, 1500, 0.0f);
    CcPickInput howl;
    howl.ReadyMask = CcOptionBit(FearOption) | CcOptionBit(HowlRow);
    howl.DistanceYards = 5.0f;
    howl.AoeEnemies = 1;
    bool howls = PickCc(howl, synthetic, aware) == FearOption;
    howl.AoeEnemies = 2;
    howls = howls && PickCc(howl, synthetic, aware) == HowlRow && PickCc(howl, synthetic, unaware) == HowlRow;
    howl.DistanceYards = 15.0f;
    howls = howls && PickCc(howl, synthetic, aware) == FearOption;
    howl.DistanceYards = 5.0f;
    howl.AoeEnemies = 1;
    howl.InstantMask = CcOptionBit(HowlRow);
    howls = howls && PickCc(howl, synthetic, aware) == HowlRow;
    howl.ReadyMask = CcOptionBit(FearOption) | CcOptionBit(Polymorph);
    howls = howls && PickCc(howl, synthetic, aware) == Polymorph;
    check(howls, "howl of terror is not picked before fear when instant or near two enemies, or is picked otherwise");

    // 4. ChainDue: at once on a free target, never over the partner's cast, castMs + leadMs before the end
    CcChainView view;
    bool chain = ChainDue(view, 1500, 300, false);
    view.PartnerCastPending = true;
    chain = chain && !ChainDue(view, 1500, 300, true);
    view.PartnerCastPending = false;
    view.AuraOnTarget = true;
    view.RemainingMs = 1801;
    chain = chain && !ChainDue(view, 1500, 300, true);
    view.RemainingMs = 1800;
    chain = chain && ChainDue(view, 1500, 300, true) && !ChainDue(view, 1500, 300, false);
    check(chain, "ChainDue does not start castMs + leadMs before the end, or chains over the partner's cast");

    // 5. Two bots at skill 99 control one healer for 60 s in 100 ms ticks: the mage's polymorph and the warlock's fear,
    // each bot with its own memory of the CC it saw land, the world as the core's rules
    {
        constexpr uint64 Healer = 9001;
        constexpr uint32 Tick = 100;
        constexpr uint32 Length = 60000;
        struct TimelineBot
        {
            uint64 Guid = 0;
            int8 Option = CC_NO_ROW;
            DrMemory Memory;
            bool Casting = false;
            uint32 LandMs = 0;
        };
        std::array<TimelineBot, 2> bots;
        bots[0].Guid = 101;
        bots[0].Option = Polymorph;
        bots[1].Guid = 102;
        bots[1].Option = FearOption;
        CcReferenceDr world;
        uint32 landings = 0;
        uint32 chained = 0;
        bool immune = false;
        bool third = false;
        bool gap = false;
        bool sameGroup = false;
        bool memoryWrong = false;
        for (uint32 now = 0; now <= Length; now += Tick)
        {
            for (TimelineBot& bot : bots)
            {
                if (!bot.Casting || now < bot.LandMs)
                    continue;

                bot.Casting = false;
                CcSpellData const& spell = synthetic[bot.Option];
                uint8 activeGroup = 0;
                uint32 activeEnd = 0;
                world.Visible(now, activeGroup, activeEnd);
                if (activeGroup)
                {
                    ++chained;
                    gap = gap || activeEnd - now > aware.ChainLeadMs;
                    sameGroup = sameGroup || activeGroup == spell.Group;
                }

                uint8 const level = world.Level(spell.Group, now);
                for (TimelineBot const& observer : bots)
                    memoryWrong = memoryWrong || observer.Memory.Level(Healer, spell.Group, now) != level;

                third = third || level >= 2;
                uint32 const duration = world.Land(spell, now);
                immune = immune || !duration;
                if (!duration)
                    continue;

                ++landings;
                for (TimelineBot& observer : bots)
                    observer.Memory.Observe(Healer, spell.Group, now, duration, aware.DrMissPct, observer.Guid);
            }

            for (size_t index = 0; index < bots.size(); ++index)
            {
                TimelineBot& bot = bots[index];
                if (bot.Casting)
                    continue;

                CcPickInput pick;
                pick.ReadyMask = CcOptionBit(bot.Option);
                pick.DistanceYards = 10.0f;
                uint32 activeEnd = 0;
                world.Visible(now, pick.ActiveGroup, activeEnd);
                pick.Levels = bot.Memory.Levels(Healer, now);
                int8 const option = PickCc(pick, synthetic, aware);
                if (option == CC_NO_ROW)
                    continue;

                CcChainView seen;
                seen.AuraOnTarget = pick.ActiveGroup != 0;
                seen.RemainingMs = seen.AuraOnTarget ? activeEnd - now : 0;
                seen.PartnerCastPending = bots[1 - index].Casting;
                if (!ChainDue(seen, synthetic[option].CastMs, aware.ChainLeadMs, aware.Chains))
                    continue;

                immune = immune || pick.Levels[synthetic[option].Group] >= CC_DR_LEVEL_IMMUNE;
                bot.Casting = true;
                bot.LandMs = now + synthetic[option].CastMs;
            }
        }

        check(landings == 6 && chained == 4,
              Acore::StringFormat("the two-bot timeline landed {} CC, {} of them chained (expected 6 and 4)", landings,
                                  chained));
        check(!immune, "a bot cast at a target its memory or the core holds immune");
        check(!gap, "a chained CC landed more than the lead before the current one's end");
        check(!sameGroup, "a chain did not alternate DR groups");
        check(!third, "a third CC of a group landed before the 15 s reset");
        check(!memoryWrong, "a bot's memory differed from the core's DR at a landing");
    }

    // 6. The memory itself: the strict 15 s reset after the end, an early end, the cap at immune, the oldest enemy
    // making room
    {
        DrMemory memory;
        memory.Observe(Enemy, Disorient, 0, 1000, 0, Seed);
        check(memory.Level(Enemy, Disorient, 16000) == 1 && memory.Level(Enemy, Disorient, 16001) == 0 &&
                  memory.Level(Enemy, Fear, 0) == 0,
              "the memory does not reset strictly 15 s after the aura's end");
        memory.Clear();
        memory.Observe(Enemy, Fear, 0, 10000, 0, Seed);
        memory.Ended(Enemy, Fear, 2000);
        check(memory.Level(Enemy, Fear, 17000) == 1 && memory.Level(Enemy, Fear, 17001) == 0,
              "an early end does not move the memory's reset");
        memory.Clear();
        for (uint32 hit = 0; hit < 4; ++hit)
            memory.Observe(Enemy, Stun, hit * 100, 1000, 0, Seed);

        check(memory.Level(Enemy, Stun, 500) == CC_DR_LEVEL_IMMUNE &&
                  memory.Levels(Enemy, 500)[Stun] == CC_DR_LEVEL_IMMUNE,
              "the memory does not stop at immune");
        memory.Clear();
        for (uint64 enemy = 1; enemy <= CC_MEMORY_ENEMIES + 1; ++enemy)
            memory.Observe(enemy, Disorient, static_cast<uint32>(enemy) * 10, 5000, 0, Seed);

        check(memory.Level(1, Disorient, 100) == 0 && memory.Level(2, Disorient, 100) == 1 &&
                  memory.Level(CC_MEMORY_ENEMIES + 1, Disorient, 100) == 1 && memory.Level(99, Disorient, 100) == 0,
              "the memory's oldest enemy does not make room");

        // G4d-c: an enemy under six groups at once (a druid and a frost mage: cyclone, polymorph, both roots, stun,
        // silence) keeps every started count; with four slots the earliest ending one went and its next CC hit
        // immunity
        memory.Clear();
        constexpr uint8 Cyclone = DIMINISHING_CYCLONE;
        constexpr uint8 ControlledRoot = DIMINISHING_CONTROLLED_ROOT;
        constexpr uint8 Silence = DIMINISHING_SILENCE;
        std::array<uint8, 6> const sixGroups = {Disorient, Fear, Stun, Cyclone, ControlledRoot, Silence};
        uint32 at = 0;
        for (uint8 const group : sixGroups)
        {
            at += 100;
            memory.Observe(Enemy, group, at, 1000, 0, Seed);
        }

        bool kept = true;
        uint8 recalled = 0;
        uint32 recallSeenMs = 0;
        int32 recallEndsMs = 0;
        for (uint8 const group : sixGroups)
            kept = kept && memory.Level(Enemy, group, 2000) == 1;

        kept = kept && memory.Recall(Enemy, DIMINISHING_CYCLONE, 2000, recalled, recallSeenMs, recallEndsMs) &&
               recalled == 1 && recallSeenMs == 1600 && recallEndsMs == -600 &&
               !memory.Recall(Enemy, DIMINISHING_HORROR, 2000, recalled, recallSeenMs, recallEndsMs);
        check(kept, "the memory forgets a started DR count of an enemy under six groups, or recalls it wrongly");
    }

    // 7. Synthetic timelines (400 x 24 CC of three groups, early ends): at skill 99 the memory matches the core's DR
    // at every CC; at skill 50 it misses DrMissPct of the CC that land on a started count
    {
        std::array<CcSpellData, 3> const kinds = {makeSpell(Disorient, DRTYPE_PLAYER, 50000, 10000, 1500, 30.0f),
                                                  makeSpell(Fear, DRTYPE_PLAYER, 20000, 10000, 1500, 20.0f),
                                                  makeSpell(Stun, DRTYPE_ALL, 6000, 10000, 0, 5.0f)};
        struct TimelineResult
        {
            uint32 Events = 0;
            uint32 Mismatches = 0;
            uint32 Eligible = 0;
            uint32 Missed = 0;
        };
        auto const run = [&](CcParams const& bot)
        {
            TimelineResult result;
            for (uint32 timeline = 0; timeline < 400; ++timeline)
            {
                uint64 const enemy = 50000 + timeline;
                CcReferenceDr world;
                DrMemory memory;
                std::array<uint32, CC_DR_GROUP_COUNT> pendingEnd{};
                uint32 now = 1000;
                for (uint32 event = 0; event < 24; ++event)
                {
                    uint64 const roll = SplitMix64((static_cast<uint64>(timeline) << 32) ^ event ^ Seed);
                    now += static_cast<uint32>(roll % 200) * 100;
                    for (uint8 group = 0; group < CC_DR_GROUP_COUNT; ++group)
                    {
                        if (pendingEnd[group] && pendingEnd[group] <= now)
                        {
                            memory.Ended(enemy, group, pendingEnd[group]);
                            pendingEnd[group] = 0;
                        }
                    }

                    CcSpellData const& spell = kinds[(roll >> 16) % kinds.size()];
                    world.Advance(now);
                    if (world.Active[spell.Group])
                        continue;

                    ++result.Events;
                    uint8 const believed = memory.Level(enemy, spell.Group, now);
                    uint8 const level = world.Level(spell.Group, now);
                    result.Mismatches += believed != level ? 1 : 0;
                    uint32 const duration = world.Land(spell, now);
                    if (!duration)
                        continue;

                    bool const counted = memory.Observe(enemy, spell.Group, now, duration, bot.DrMissPct, Seed);
                    if (believed > 0)
                    {
                        ++result.Eligible;
                        result.Missed += counted ? 0 : 1;
                    }

                    if ((roll >> 32) % 100 < 25)
                    {
                        uint32 const endMs = now + duration * static_cast<uint32>((roll >> 40) % 90 + 10) / 100;
                        world.EndEarly(spell.Group, endMs);
                        pendingEnd[spell.Group] = endMs;
                    }
                }
            }

            return result;
        };

        TimelineResult const expert = run(aware);
        check(expert.Events > 5000 && expert.Mismatches == 0,
              Acore::StringFormat("at skill 99 the memory differs from the core's DR at {} of {} CC", expert.Mismatches,
                                  expert.Events));
        CcParams const middle = params(50);
        TimelineResult const average = run(middle);
        check(average.Eligible >= 500 && withinRate(average.Missed, average.Eligible, middle.DrMissPct, 4),
              Acore::StringFormat("at skill 50 the memory missed {} of {} CC on a started count (expected about {} %)",
                                  average.Missed, average.Eligible, middle.DrMissPct));
        check(average.Mismatches > 0, "at skill 50 the memory never errs");
    }

    // 8. ShouldBreakCc at skills 20 and 80: a root, snare or silence never at 80; a stun with the partner low always;
    // the wasted breaks at their rates
    {
        CcParams const low = params(20);
        CcParams const high = params(80);
        constexpr uint32 Windows = 4000;
        constexpr std::array<CcKind, 3> Minor = {CcKind::Root, CcKind::Snare, CcKind::Silence};
        uint32 minorHigh = 0;
        uint32 rootLow = 0;
        uint32 stunPartner = 0;
        uint32 stunWastedHigh = 0;
        for (uint32 window = 0; window < Windows; ++window)
        {
            uint32 const now = window * CC_BREAK_WINDOW_MS;
            CcBreakView minor;
            minor.SelfLow = true;
            minor.PartnerLow = true;
            minor.Burst = true;
            for (CcKind kind : Minor)
            {
                minor.Kind = kind;
                minor.RemainingMs = 8000;
                minorHigh += ShouldBreakCc(minor, high, Seed, now) ? 1 : 0;
                minor.RemainingMs = 1000;
                minorHigh += ShouldBreakCc(minor, high, Seed, now) ? 1 : 0;
            }

            CcBreakView root;
            root.Kind = CcKind::Root;
            root.RemainingMs = 8000;
            rootLow += ShouldBreakCc(root, low, Seed, now) ? 1 : 0;
            CcBreakView stun;
            stun.Kind = CcKind::Stun;
            stun.RemainingMs = 4000;
            stun.PartnerLow = true;
            stunPartner += ShouldBreakCc(stun, low, Seed, now) && ShouldBreakCc(stun, high, Seed, now) ? 1 : 0;
            stun.PartnerLow = false;
            stunWastedHigh += ShouldBreakCc(stun, high, Seed, now) ? 1 : 0;
        }

        check(minorHigh == 0, Acore::StringFormat("skill 80 broke a root, snare or silence {} times", minorHigh));
        check(stunPartner == Windows, "a stun with the partner low is not broken at skill 20 or 80");
        CcBreakView late;
        late.Kind = CcKind::Stun;
        late.RemainingMs = 2000;
        late.PartnerLow = true;
        check(!CcMatters(late, high), "a stun with less than MinLeftMs left matters");
        check(
            withinRate(stunWastedHigh, Windows, high.WastedBreakPct, 4),
            Acore::StringFormat("skill 80 wasted {} of {} breaks on a stun that does not matter (expected about {} %)",
                                stunWastedHigh, Windows, high.WastedBreakPct));
        check(withinRate(rootLow, Windows, low.WastedBreakPct, 4),
              Acore::StringFormat("skill 20 broke {} of {} roots (expected about {} %)", rootLow, Windows,
                                  low.WastedBreakPct));
    }

    // 9. Roll rates: never, always, the chance per window, the same answer within a window and for the same input,
    // independent salts; the wrong-target and own-break shares over skill
    {
        constexpr uint32 Windows = 5000;
        constexpr uint32 WindowMs = 1000;
        uint32 always = 0;
        uint32 never = 0;
        uint32 some = 0;
        uint32 both = 0;
        bool stable = true;
        uint64 const mixed = SplitMix64(Seed ^ CcSalt::DrMiss);
        for (uint32 window = 0; window < Windows; ++window)
        {
            uint32 const now = window * WindowMs;
            always += RollWindow(Seed, CcSalt::DrMiss, 100, WindowMs, now) ? 1 : 0;
            never += RollWindow(Seed, CcSalt::DrMiss, 0, WindowMs, now) ? 1 : 0;
            bool const hit = RollWindow(Seed, CcSalt::DrMiss, 37, WindowMs, now);
            some += hit ? 1 : 0;
            both += hit && RollWindow(Seed, CcSalt::WastedBreak, 37, WindowMs, now) ? 1 : 0;
            bool const sameWindow = TacticUseWindow(mixed, WindowMs, now) == TacticUseWindow(mixed, WindowMs, now + 1);
            stable = stable && hit == RollWindow(Seed, CcSalt::DrMiss, 37, WindowMs, now) &&
                     (!sameWindow || hit == RollWindow(Seed, CcSalt::DrMiss, 37, WindowMs, now + 1));
        }

        check(always == Windows && never == 0, "RollWindow at 100 % misses or at 0 % hits");
        check(withinRate(some, Windows, 37, 3), Acore::StringFormat("RollWindow at 37 % hit {} of {}", some, Windows));
        check(stable, "RollWindow answers differently for the same input or within one window");
        check(withinRate(both, Windows, 14, 3), "two salts' rolls are not independent");
        CcParams const novice = params(1);
        uint32 wrongNovice = 0;
        uint32 wrongExpert = 0;
        uint32 ownNovice = 0;
        uint32 ownExpert = 0;
        for (uint32 window = 0; window < Windows; ++window)
        {
            uint32 const now = window * CC_BREAK_WINDOW_MS;
            wrongNovice += CcWrongTarget(novice, Seed, now) ? 1 : 0;
            wrongExpert += CcWrongTarget(aware, Seed, now) ? 1 : 0;
            ownNovice += CcBreaksOwn(novice, Seed, now) ? 1 : 0;
            ownExpert += CcBreaksOwn(aware, Seed, now) ? 1 : 0;
        }

        check(novice.WrongTargetPct == 50 && withinRate(wrongNovice, Windows, 50, 4) && wrongExpert == 0,
              Acore::StringFormat("wrong-target share {} of {} at skill 1, {} at skill 99", wrongNovice, Windows,
                                  wrongExpert));
        check(
            novice.OwnBreakPct == 60 && withinRate(ownNovice, Windows, 60, 4) &&
                withinRate(ownExpert, Windows, aware.OwnBreakPct, 3),
            Acore::StringFormat("own-break share {} of {} at skill 1, {} at skill 99", ownNovice, Windows, ownExpert));
    }

    // 10. The parameters over skill and attributes
    {
        CcParams const full = ComputeCcParams(0, BotAttributeValues{}, SKILL_GATE_FULL, Linear, config);
        check(full.DrAware && full.Chains && full.DrMissPct == 0 && full.IgnoresRoots && full.ChainLeadMs == 300 &&
                  full.WrongTargetPct == 0 && full.WastedBreakPct == 0 && !full.StopsWhenBehind && full.HoldMs == 0,
              "skill 0 is not full ability");
        CcParams const thirty = params(30);
        CcParams const fifty = params(50);
        check(!thirty.DrAware && !thirty.Chains && fifty.DrAware && !fifty.Chains && fifty.DrMissPct == 20 &&
                  params(70).Chains && !fifty.IgnoresRoots && params(70).IgnoresRoots,
              "DR awareness from 40, chains from 65 or roots ignored from 65 do not hold");
        CcParams const lazy = ComputeCcParams(50, plain(50), 30, Linear, config);
        check(lazy.StopsWhenBehind && CcStopsBehind(lazy, 40, 70) && !CcStopsBehind(lazy, 40, 69) &&
                  !CcStopsBehind(fifty, 40, 90),
              "a low-effort bot does not stop controlling when behind, or a full-effort bot does");
        BotAttributeValues patient = plain(50);
        patient.Value[static_cast<uint32>(BotAttribute::Patience)] = 100;
        CcParams const holder = ComputeCcParams(50, patient, SKILL_GATE_FULL, Linear, config);
        check(holder.HoldMs == config.CcPatientHoldMs && fifty.HoldMs == 0 && CcHolds(holder, 2999, false) &&
                  !CcHolds(holder, 3000, false) && !CcHolds(holder, 0, true),
              "a patient bot does not hold its CC for the kill window, or a neutral one does");
    }

    // 11. The option table: the classes with a CC usable on players (the moon mark), one row per action
    {
        bool classes = true;
        for (uint8 playerClass :
             {CLASS_MAGE, CLASS_DRUID, CLASS_HUNTER, CLASS_WARLOCK, CLASS_ROGUE, CLASS_PRIEST, CLASS_PALADIN})
            classes = classes && CcClassHasOption(playerClass);

        // G4d: the warrior, the shaman and the death knight have a row now (their teams get the moon mark)
        for (uint8 playerClass : {CLASS_WARRIOR, CLASS_SHAMAN, CLASS_DEATH_KNIGHT})
            classes = classes && CcClassHasOption(playerClass);

        check(classes, "a class has no CC usable on players (G4d: all ten have one)");
        uint32 rogueRows = 0;
        bool rows = true;
        for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
        {
            rogueRows += (CcClassMask(CLASS_ROGUE) & CcOptionBit(index)) ? 1 : 0;
            rows = rows && CcOptionIndex(CC_OPTIONS[index].Action) == static_cast<int8>(index) &&
                   CC_OPTIONS[index].PlayerClass >= CLASS_WARRIOR && CC_OPTIONS[index].PlayerClass <= CLASS_DRUID;
        }

        check(rows && rogueRows == 5, "an option row repeats an action or has no class, or the rogue has not 5 rows");
    }

    // 12. G4b: a bot's table from the highest rank it knows (G4a NOTE 1: hammer of justice 3 s at rank 1, 6 s at its
    // last rank, 4 s when it knows two), no row without the spell
    {
        constexpr int8 Hammer = CcOptionIndex("hammer of justice");
        static_assert(Hammer != CC_NO_ROW, "hammer of justice is in CC_OPTIONS");
        auto const knowsAll = [](uint32) { return true; };
        auto const knowsNone = [](uint32) { return false; };
        uint32 const first = CC_OPTIONS[Hammer].SpellId;
        uint32 const second = sSpellMgr->GetNextSpellInChain(first);
        auto const knowsTwo = [&](uint32 spellId) { return spellId == first || spellId == second; };
        uint32 const last = CcHighestKnownRank(first, knowsAll);
        CcSpellData const lastRank = ResolveCcSpell(last);
        print("hammer of justice, last rank", last, lastRank, CC_OPTIONS[Hammer].BreaksOnDamage);
        check(spells[Hammer].BaseMs == 3000 && lastRank.BaseMs == 6000 && second &&
                  CcHighestKnownRank(first, knowsTwo) == second && ResolveCcSpell(second).BaseMs == 4000 &&
                  CcHighestKnownRank(first, knowsNone) == 0,
              Acore::StringFormat("hammer of justice by rank: {} ms at rank 1, {} ms at spell {} (6000 expected)",
                                  spells[Hammer].BaseMs, lastRank.BaseMs, last));
        std::array<uint32, CC_OPTION_COUNT> known{};
        CcSpellTable table{};
        CcRowMask const knownMask = BuildCcBotTable(CLASS_PALADIN, knowsAll, known, table);
        check(knownMask == CcClassMask(CLASS_PALADIN) && known[Hammer] == last && table[Hammer].BaseMs == 6000 &&
                  !known[Polymorph] && !table[Polymorph].Valid &&
                  BuildCcBotTable(CLASS_PALADIN, knowsNone, known, table) == 0,
              "a paladin's table is not its rows at their last ranks, or has another class's row");
    }

    // 13. G4a NOTE 2: TargetIsPlayer (the rows usable on players) apart from DrAsPlayer (the PvP cap and the
    // player-only DR on a player's pet), and the creature-only rows by creature type
    {
        constexpr int8 Hibernate = CcOptionIndex("hibernate on cc");
        constexpr int8 Banish = CcOptionIndex("banish on cc");
        static_assert(Hibernate != CC_NO_ROW && Banish != CC_NO_ROW, "hibernate and banish are in CC_OPTIONS");
        CcSpellTable pets{};
        pets[Hibernate] = makeSpell(DIMINISHING_SLEEP, DRTYPE_PLAYER, 40000, 10000, 1500, 30.0f);
        pets[Banish] = makeSpell(DIMINISHING_BANISH, DRTYPE_PLAYER, 30000, 6000, 1500, 30.0f);
        pets[Polymorph] = makeSpell(Disorient, DRTYPE_PLAYER, 50000, 10000, 1500, 30.0f);
        auto const picks = [&](CcPickInput input, int8 row)
        {
            input.ReadyMask = CcOptionBit(static_cast<uint8>(row));
            return PickCc(input, pets, aware) == row;
        };
        CcPickInput beastPet;
        beastPet.TargetIsPlayer = false;
        beastPet.CreatureType = CREATURE_TYPE_BEAST;
        beastPet.DistanceYards = 10.0f;
        CcPickInput demonPet = beastPet;
        demonPet.CreatureType = CREATURE_TYPE_DEMON;
        CcPickInput player;
        player.DistanceYards = 10.0f;
        check(picks(beastPet, Hibernate) && !picks(beastPet, Banish) && picks(beastPet, Polymorph) &&
                  picks(demonPet, Banish) && !picks(demonPet, Hibernate) && !picks(player, Hibernate) &&
                  !picks(player, Banish) && picks(player, Polymorph),
              "hibernate is not for beasts, banish not for demons, or either is cast on a player");
        CcPickInput wildBeast = beastPet;
        wildBeast.DrAsPlayer = false;
        check(ExpectedMs(pets[Hibernate], 1, beastPet.DrAsPlayer) == 5000 &&
                  ExpectedMs(pets[Hibernate], 1, wildBeast.DrAsPlayer) == 40000,
              "a player's pet is not capped and counted like a player, or a wild beast is");
    }

    // 14. G4a NOTE 3: two CC of one group overlapping keep the later end and count twice; one of them ending early
    // moves the end to the other's, the second to now
    {
        DrMemory overlap;
        overlap.Observe(Enemy, Stun, 0, 10000, 0, Seed);
        overlap.Observe(Enemy, Stun, 1000, 4000, 0, Seed);
        check(overlap.Level(Enemy, Stun, 1000) == 2 && overlap.Level(Enemy, Stun, 25000) == 2 &&
                  overlap.Level(Enemy, Stun, 25001) == 0,
              "a second CC of a group under the first does not count, or moves the end earlier");
        overlap.Ended(Enemy, Stun, 2000, 3000);
        bool const toOther = overlap.Level(Enemy, Stun, 20000) == 2 && overlap.Level(Enemy, Stun, 20001) == 0;
        overlap.Ended(Enemy, Stun, 3000);
        check(toOther && overlap.Level(Enemy, Stun, 18000) == 2 && overlap.Level(Enemy, Stun, 18001) == 0,
              "one of two overlapping CC ending early does not reset from the other's end, or the last from now");
    }

    // 15. Avoid.OwnBreakNeverFromSkill (Focus, 65): no own breaks from it (skill 80 in the matrix), the falling share
    // below it, the skill-99 share when it is 100
    {
        CcConfig breaksAlways = config;
        breaksAlways.AvoidOwnBreakNeverFromSkill = 100;
        check(params(64).OwnBreakPct > 0 && params(20).OwnBreakPct > params(64).OwnBreakPct &&
                  params(65).OwnBreakPct == 0 && params(80).OwnBreakPct == 0 &&
                  ComputeCcParams(0, BotAttributeValues{}, SKILL_GATE_FULL, Linear, config).OwnBreakPct == 0 &&
                  ComputeCcParams(99, plain(99), SKILL_GATE_FULL, Linear, breaksAlways).OwnBreakPct == 5,
              Acore::StringFormat("own-break shares at skill 20/64/65/80: {}/{}/{}/{}", params(20).OwnBreakPct,
                                  params(64).OwnBreakPct, params(65).OwnBreakPct, params(80).OwnBreakPct));
    }

    // 16. G4b-a review B1: a loss of control before a root whatever their lengths (roots 10 s on a player, cyclone
    // 6 s), no root on a rooted target, none on the moon (ControlOnly), DR-aware or not; NOTE 2: a finisher's
    // duration by the combo points on its target
    {
        constexpr int8 Roots = CcOptionIndex("entangling roots on cc");
        constexpr int8 Cyclone = CcOptionIndex("cyclone on cc");
        constexpr int8 Kidney = CcOptionIndex("kidney shot");
        static_assert(Roots != CC_NO_ROW && Cyclone != CC_NO_ROW && Kidney != CC_NO_ROW,
                      "roots, cyclone and kidney shot are in CC_OPTIONS");
        CcSpellTable druid{};
        druid[Roots] = makeSpell(DIMINISHING_CONTROLLED_ROOT, DRTYPE_PLAYER, 27000, 10000, 1500, 30.0f);
        druid[Cyclone] = makeSpell(DIMINISHING_CYCLONE, DRTYPE_PLAYER, 6000, 10000, 1500, 20.0f);
        CcPickInput both;
        both.ReadyMask = CcOptionBit(Roots) | CcOptionBit(Cyclone);
        both.DistanceYards = 10.0f;
        CcPickInput rootsOnly = both;
        rootsOnly.ReadyMask = CcOptionBit(Roots);
        check(PickCc(both, druid, aware) == Cyclone && PickCc(both, druid, unaware) == Cyclone &&
                  PickCc(rootsOnly, druid, unaware) == Roots && PickCc(rootsOnly, druid, aware) == Roots,
              "a root goes before cyclone, or a root alone is not cast");
        CcPickInput rooted = both;
        rooted.Rooted = true;
        CcPickInput rootedOnly = rootsOnly;
        rootedOnly.Rooted = true;
        check(PickCc(rooted, druid, unaware) == Cyclone && PickCc(rooted, druid, aware) == Cyclone &&
                  PickCc(rootedOnly, druid, unaware) == CC_NO_ROW && PickCc(rootedOnly, druid, aware) == CC_NO_ROW,
              "a root goes on a rooted target, or cyclone does not");
        CcPickInput moon = both;
        moon.ControlOnly = true;
        CcPickInput moonRoots = rootsOnly;
        moonRoots.ControlOnly = true;
        check(PickCc(moon, druid, unaware) == Cyclone && PickCc(moonRoots, druid, unaware) == CC_NO_ROW &&
                  PickCc(moonRoots, druid, aware) == CC_NO_ROW,
              "a root is cast as the moon's CC");
        SpellInfo const* const kidney = sSpellMgr->GetSpellInfo(CC_OPTIONS[Kidney].SpellId);
        bool const kidneyFull = kidney && kidney->GetMaxDuration() > 0 &&
                                CcComboDurationMs(kidney->GetDuration(), kidney->GetMaxDuration(), 5) ==
                                    static_cast<uint32>(kidney->GetMaxDuration());
        check(CcComboDurationMs(0, 5000, 1) == 1000 && CcComboDurationMs(0, 5000, 5) == 5000 &&
                  CcComboDurationMs(6000, 6000, 3) == 6000 && CcComboDurationMs(-1, 5000, 2) == 0 &&
                  CcComboDurationMs(1000, 5000, 0) == 1000 && kidneyFull,
              Acore::StringFormat("a finisher's duration by combo points is off (kidney shot {} to {} ms)",
                                  kidney ? kidney->GetDuration() : 0, kidney ? kidney->GetMaxDuration() : 0));
    }

    // 17. G4b-b own CC: a team breakable CC keeps the enemy out of the targets unless the own-break window breaks it;
    // a forced one whatever the window; at or above OwnBreakNeverFromSkill the window never opens
    {
        check(CcKeepsOut(true, false, false) && !CcKeepsOut(true, false, true) && !CcKeepsOut(false, false, false),
              "a team CC is kept out unless the own-break window breaks it");
        check(CcKeepsOut(false, true, true) && CcKeepsOut(true, true, true) && CcKeepsOut(false, true, false),
              "a forced CC is kept out whatever the own-break window");
        CcConfig ownConfig = config;
        ownConfig.AvoidOwnBreakNeverFromSkill = 65;
        ownConfig.AvoidOwnBreakPctAtSkill1 = 60;
        ownConfig.AvoidOwnBreakPctAtSkill99 = 5;
        CcParams const high = ComputeCcParams(80, plain(80), SKILL_GATE_FULL, Linear, ownConfig);
        CcParams const low = ComputeCcParams(20, plain(20), SKILL_GATE_FULL, Linear, ownConfig);
        bool highBreaks = false;
        uint32 lowWindows = 0;
        for (uint32 window = 0; window < 200; ++window)
        {
            highBreaks |= CcBreaksOwn(high, 4242, window * CC_BREAK_WINDOW_MS);
            lowWindows += CcBreaksOwn(low, 4242, window * CC_BREAK_WINDOW_MS) ? 1 : 0;
        }

        check(high.OwnBreakPct == 0 && !highBreaks && low.OwnBreakPct > 0 && lowWindows > 0,
              Acore::StringFormat("own breaks: skill 80 {} % ({}), skill 20 {} % ({} of 200 windows)", high.OwnBreakPct,
                                  highBreaks ? "breaks" : "never", low.OwnBreakPct, lowWindows));
    }

    // 18. G4c chain clock: the gap to the team's previous CC on the enemy (negative on top of it, positive after it,
    // a chain up to CC_CHAIN_WINDOW_MS after its end), the later end kept, an early end, the oldest enemy replaced
    {
        CcChainClock clock;
        int32 gap = 0;
        uint8 previous = 0;
        bool none = !clock.Gap(Enemy, 1000, CC_CHAIN_WINDOW_MS, gap, previous);
        clock.Land(Enemy, Disorient, 1000, 8000);
        bool const onTop =
            clock.Gap(Enemy, 8760, CC_CHAIN_WINDOW_MS, gap, previous) && gap == -240 && previous == Disorient;
        bool const after = clock.Gap(Enemy, 9000 + CC_CHAIN_WINDOW_MS, CC_CHAIN_WINDOW_MS, gap, previous) &&
                           gap == static_cast<int32>(CC_CHAIN_WINDOW_MS);
        bool const late = !clock.Gap(Enemy, 9001 + CC_CHAIN_WINDOW_MS, CC_CHAIN_WINDOW_MS, gap, previous);
        clock.Land(Enemy, Fear, 2000, 2000);
        bool const kept =
            clock.Gap(Enemy, 9000, CC_CHAIN_WINDOW_MS, gap, previous) && gap == 0 && previous == Disorient;
        clock.Ended(Enemy, 5000, 0);
        bool const early = clock.Gap(Enemy, 5500, CC_CHAIN_WINDOW_MS, gap, previous) && gap == 500;
        clock.Land(Enemy, Fear, 5500, 4000);
        bool const next = clock.Gap(Enemy, 9300, CC_CHAIN_WINDOW_MS, gap, previous) && gap == -200 && previous == Fear;
        for (uint64 other = 1; other <= CC_MEMORY_ENEMIES; ++other)
            clock.Land(Enemy + other, Stun, 6000 + static_cast<uint32>(other), 1000);

        none = none && !clock.Gap(Enemy, 7000, CC_CHAIN_WINDOW_MS, gap, previous) &&
               clock.Gap(Enemy + CC_MEMORY_ENEMIES, 7000, CC_CHAIN_WINDOW_MS, gap, previous);
        check(none && onTop && after && late && kept && early && next,
              Acore::StringFormat("chain clock: none {} on top {} after {} late {} kept {} early {} next {}", none,
                                  onTop, after, late, kept, early, next));
    }

    // 19. G4c: the wake before a chain's moment (0 when due, or waiting on the partner's cast)
    {
        CcChainView wait;
        wait.AuraOnTarget = true;
        wait.RemainingMs = 1000;
        bool const early = CcChainWaitMs(wait, 0, 240) == 760 && CcChainWaitMs(wait, 1500, 240) == 0;
        wait.PartnerCastPending = true;
        bool const partner = CcChainWaitMs(wait, 0, 240) == 0;
        wait.PartnerCastPending = false;
        wait.AuraOnTarget = false;
        check(early && partner && CcChainWaitMs(wait, 0, 240) == 0,
              "the chain's wake is not remaining - cast - lead, or wakes while due or on the partner's cast");
    }

    // 20. G4c: the loss-of-control groups match the rows' kinds (the chains' auras, ccHeldMs)
    {
        CcSpellTable const& table = GetCcSpellTable();
        std::string mismatches;
        for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
        {
            if (table[index].Valid &&
                CcKindLosesControl(CC_OPTIONS[index].Kind) != CcGroupLosesControl(table[index].Group))
                mismatches += Acore::StringFormat("{}{} ({})", mismatches.empty() ? "" : ", ", CC_OPTIONS[index].Action,
                                                  groupName(table[index].Group));
        }

        check(mismatches.empty() && !CcGroupLosesControl(DIMINISHING_CONTROLLED_ROOT) &&
                  !CcGroupLosesControl(DIMINISHING_SILENCE) && !CcGroupLosesControl(DIMINISHING_NONE) &&
                  CcGroupLosesControl(DIMINISHING_CYCLONE),
              Acore::StringFormat("loss-of-control groups and row kinds differ: {}", mismatches));
    }

    // 21. G4c tactics force dr: the group names, the row cast for a group (no stealth, no combo points, usable on a
    // player)
    {
        constexpr int8 Hammer = CcOptionIndex("hammer of justice");
        constexpr int8 CycloneRow = CcOptionIndex("cyclone on cc");
        static_assert(Hammer != CC_NO_ROW && CycloneRow != CC_NO_ROW, "the selftest's rows are in CC_OPTIONS");
        bool const names = CcDrGroupByName("fear") == Fear && CcDrGroupByName("FEAR") == Fear &&
                           CcDrGroupByName("controlledstun") == Stun && CcDrGroupByName("charge") == DIMINISHING_NONE &&
                           CcDrGroupByName("bogus") == DIMINISHING_NONE;
        int8 const fearRow = CcForceDrRow(Fear, true, 0);
        int8 const disorientRow = CcForceDrRow(Disorient, true, 0);
        int8 const stunRow = CcForceDrRow(Stun, true, 0);
        int8 const cycloneRow = CcForceDrRow(DIMINISHING_CYCLONE, true, 0);
        int8 const openingRow = CcForceDrRow(DIMINISHING_OPENING_STUN, true, 0);
        auto const rowName = [](int8 row) { return row == CC_NO_ROW ? "none" : CC_OPTIONS[row].Action; };
        check(names && fearRow == FearOption && disorientRow == Polymorph && stunRow == Hammer &&
                  cycloneRow == CycloneRow && openingRow == CC_NO_ROW,
              Acore::StringFormat("force dr: names {}, fear {}, disorient {}, controlled stun {}, cyclone {}, opening "
                                  "stun {}",
                                  names, rowName(fearRow), rowName(disorientRow), rowName(stunRow), rowName(cycloneRow),
                                  rowName(openingRow)));
    }

    // 22. G4c the chain's choice at skill 80 (chains) and 20 (none): polymorph on the target, the next CC is fear (the
    // other group), started castMs + lead before the end; with fear immune in the memory no row (the bot waits);
    // without chains nothing goes on the controlled target
    {
        CcParams const high = params(80);
        CcParams const low = params(20);
        CcSpellTable table{};
        table[Polymorph] = makeSpell(Disorient, DRTYPE_PLAYER, 10000, 10000, 1500, 30.0f);
        table[FearOption] = makeSpell(Fear, DRTYPE_PLAYER, 10000, 10000, 1500, 20.0f);
        CcPickInput input;
        input.ReadyMask = CcOptionBit(Polymorph) | CcOptionBit(FearOption);
        input.DistanceYards = 10.0f;
        input.ActiveGroup = Disorient;
        int8 const next = PickCc(input, table, high);
        CcChainView chainView;
        chainView.AuraOnTarget = true;
        chainView.RemainingMs = 1500 + high.ChainLeadMs + 1;
        bool const notYet = !ChainDue(chainView, 1500, high.ChainLeadMs, high.Chains) &&
                            CcChainWaitMs(chainView, 1500, high.ChainLeadMs) == 1;
        chainView.RemainingMs = 1500 + high.ChainLeadMs;
        bool const due = ChainDue(chainView, 1500, high.ChainLeadMs, high.Chains);
        input.Levels[Fear] = CC_DR_LEVEL_IMMUNE;
        int8 const immune = PickCc(input, table, high);
        bool const lowWaits = !low.Chains && !ChainDue(chainView, 1500, low.ChainLeadMs, low.Chains);
        check(high.Chains && next == FearOption && notYet && due && immune == CC_NO_ROW && lowWaits,
              Acore::StringFormat("chain at 80 (lead {} ms): next {}, not yet {}, due {}, fear immune -> {}; at 20 "
                                  "chains {}",
                                  high.ChainLeadMs, next == CC_NO_ROW ? "none" : CC_OPTIONS[next].Action, notYet, due,
                                  immune == CC_NO_ROW ? "none" : CC_OPTIONS[immune].Action, low.Chains));
        handler->PSendSysMessage(
            "TACTICS selftest cc: chain lead {} ms at skill 80, {} ms at skill 99; chains from "
            "skill {}",
            high.ChainLeadMs, params(99).ChainLeadMs, config.CcChainsFromSkill);
    }

    // 23. G4c-b DotGuard: with the team's periodic damage on the target no CC that damage breaks (polymorph), a stun
    // still goes; G4c-a review NOTE 1: a chain (ControlOnly) takes no root
    {
        constexpr int8 Hammer = CcOptionIndex("hammer of justice");
        constexpr int8 Roots = CcOptionIndex("entangling roots on cc");
        static_assert(Hammer != CC_NO_ROW && Roots != CC_NO_ROW, "the selftest's rows are in CC_OPTIONS");
        CcParams const high = params(80);
        CcSpellTable table{};
        table[Polymorph] = makeSpell(Disorient, DRTYPE_PLAYER, 10000, 10000, 1500, 30.0f);
        table[Hammer] = makeSpell(Stun, DRTYPE_PLAYER, 6000, 6000, 0, 10.0f);
        table[Roots] = makeSpell(DIMINISHING_CONTROLLED_ROOT, DRTYPE_PLAYER, 10000, 10000, 1500, 30.0f);
        CcPickInput input;
        input.ReadyMask = CcOptionBit(Polymorph) | CcOptionBit(Hammer);
        input.DistanceYards = 5.0f;
        int8 const clean = PickCc(input, table, high);
        input.TeamDotOn = true;
        int8 const dot = PickCc(input, table, high);
        input.ReadyMask = CcOptionBit(Polymorph);
        int8 const dotOnly = PickCc(input, table, high);
        CcPickInput chain;
        chain.ReadyMask = CcOptionBit(Polymorph) | CcOptionBit(Roots);
        chain.DistanceYards = 10.0f;
        chain.ActiveGroup = Disorient;
        int8 const rootFree = PickCc(chain, table, high);
        chain.ControlOnly = true;
        int8 const rootChain = PickCc(chain, table, high);
        auto const rowName = [](int8 row) { return row == CC_NO_ROW ? "none" : CC_OPTIONS[row].Action; };
        check(
            clean == Polymorph && dot == Hammer && dotOnly == CC_NO_ROW && rootFree == Roots && rootChain == CC_NO_ROW,
            Acore::StringFormat("dot guard: clean {}, dot {}, dot with polymorph only {}; chain: root free {}, root "
                                "in a chain {}",
                                rowName(clean), rowName(dot), rowName(dotOnly), rowName(rootFree), rowName(rootChain)));
    }

    // 24. G4c-b opponent memory: the dispel mean from OpponentMinEvents dispels, per enemy; trinkets counted; a new
    // match forgets the old one
    {
        constexpr uint32 Match = 11;
        CcOpponentMemory memory;
        memory.Dispelled(Match, Enemy, 1000, 100);
        uint32 const one = memory.DispelMeanMs(Match, Enemy, 2);
        memory.Dispelled(Match, Enemy, 2000, 200);
        uint32 const two = memory.DispelMeanMs(Match, Enemy, 2);
        uint32 const other = memory.DispelMeanMs(Match, Enemy + 1, 1);
        memory.Trinketed(Match, Enemy + 1, 300);
        CcOpponentMemory::Slot const* const trinket = memory.Find(Match, Enemy + 1);
        bool const trinketSeen =
            trinket && trinket->Trinkets == 1 && trinket->LastTrinketMs == 300 && !trinket->Dispels;
        uint32 const otherMatch = memory.DispelMeanMs(Match + 1, Enemy, 1);
        memory.Trinketed(Match + 1, Enemy + 2, 400);
        uint32 const cleared = memory.DispelMeanMs(Match, Enemy, 1);
        bool const next = memory.Find(Match + 1, Enemy + 2) && !memory.Find(Match + 1, Enemy);
        check(one == 0 && two == 1500 && other == 0 && trinketSeen && otherMatch == 0 && cleared == 0 && next,
              Acore::StringFormat("opponent memory: after one dispel {}, after two {}, other enemy {}, trinket seen "
                                  "{}, other match {}, after the match changed {}, new match kept {}",
                                  one, two, other, trinketSeen, otherMatch, cleared, next));
    }

    // 25. G4c-b the chain's moment by the memory: the dispel's mean less the margin, counted from the landing; the
    // aura's end when that comes sooner or nothing is known; due at once between that moment and the dispel's mean
    // (the aura's end again once the dispel is overdue: 25b)
    {
        CcParams const high = params(80);
        uint32 const unknown = CcChainRemainingMs(8000, 500, 0, 300);
        uint32 const early = CcChainRemainingMs(8000, 500, 1500, 300);
        uint32 const late = CcChainRemainingMs(600, 500, 9000, 300);
        uint32 const overdue = CcChainRemainingMs(8000, 1300, 1500, 300);
        CcChainView chainView;
        chainView.AuraOnTarget = true;
        chainView.RemainingMs = early;
        bool const castDue = ChainDue(chainView, 1500, high.ChainLeadMs, true);
        bool const instantWaits = !ChainDue(chainView, 0, high.ChainLeadMs, true) &&
                                  CcChainWaitMs(chainView, 0, high.ChainLeadMs) == early - high.ChainLeadMs;
        check(unknown == 8000 && early == 700 && late == 600 && overdue == 0 && castDue && instantWaits,
              Acore::StringFormat("chain by memory: unknown {}, early {}, late {}, overdue {}; a 1.5 s cast due {}, an "
                                  "instant waits {}",
                                  unknown, early, late, overdue, castDue, instantWaits));
    }

    // 25b. G4c-c (G4c-b review NOTE 1): a dispel that does not come this time; past the remembered dispel moment the
    // aura's end counts again, so the chain waits for it (lead before the end) instead of casting at once. G4c-d: the
    // moment is the mean plus the margin, so the due window (mean - margin to mean + margin) outlasts a reaction
    {
        CcParams const high = params(80);
        uint32 const atMean = CcChainRemainingMs(6500, 1500, 1500, 300);
        uint32 const inMargin = CcChainRemainingMs(6400, 1700, 1500, 300);
        uint32 const absent = CcChainRemainingMs(6400, 1900, 1500, 300);
        CcChainView chainView;
        chainView.AuraOnTarget = true;
        chainView.RemainingMs = absent;
        bool const castWaits = !ChainDue(chainView, 1500, high.ChainLeadMs, true) &&
                               CcChainWaitMs(chainView, 1500, high.ChainLeadMs) == absent - 1500 - high.ChainLeadMs;
        chainView.RemainingMs = 1500 + high.ChainLeadMs;
        bool const dueAtEnd = ChainDue(chainView, 1500, high.ChainLeadMs, true);
        check(atMean == 0 && inMargin == 0 && absent == 6400 && castWaits && dueAtEnd,
              Acore::StringFormat("chain by memory, dispel absent: at the mean {}, within the margin after it {}, "
                                  "past it {}; a 1.5 s cast waits {}, due lead before the end {}",
                                  atMean, inMargin, absent, castWaits, dueAtEnd));
    }

    // 25c. G4c-d: a chain onto the team's cyclone lands AfterMs after its end (a 1.5 s cast starts castMs - AfterMs
    // before it, an instant only once it is over; CcLandsInImmunity holds a cast that would land inside it); the
    // bot's learned delay (ReactMs) moves the moment sooner; the busy guard holds a cast or global cooldown that ends
    // after the moment; the delay is learned wholly from the first chain, then by a quarter
    {
        CcChainView view;
        view.AuraOnTarget = true;
        view.ImmuneUntilEnd = true;
        view.AfterMs = 100;
        view.RemainingMs = 2000;
        bool const cycloneWaits = !ChainDue(view, 1500, 242, true) && CcChainWaitMs(view, 1500, 242) == 600;
        view.RemainingMs = 1400;
        bool const cycloneDue = ChainDue(view, 1500, 242, true);
        view.RemainingMs = 1;
        bool const instantWaits = !ChainDue(view, 0, 242, true) && CcChainWaitMs(view, 0, 242) == 1;
        bool const held =
            CcLandsInImmunity(1000, 0, 1100) && !CcLandsInImmunity(1000, 1500, 2400) && !CcLandsInImmunity(1000, 0, 0);
        view.ImmuneUntilEnd = false;
        view.ReactMs = 700;
        view.RemainingMs = 1500 + 700 + 242;
        bool const reactDue = ChainDue(view, 1500, 242, true);
        view.RemainingMs += 1;
        bool const reactWaits = !ChainDue(view, 1500, 242, true) && CcChainWaitMs(view, 1500, 242) == 1;
        bool const guard = CcChainBlocksCast(1000, 1000, 1500) && CcChainBlocksCast(2000, 1000, 1) &&
                           !CcChainBlocksCast(1000, 2600, 1500) && CcChainBlocksCast(1000, 2499, 1500) &&
                           !CcChainBlocksCast(1000, 1000, 0);
        uint32 const first = CcLearnChainReactMs(300, 800, false);
        uint32 const next = CcLearnChainReactMs(800, 400, true);
        check(cycloneWaits && cycloneDue && instantWaits && held && reactDue && reactWaits && guard && first == 800 &&
                  next == 700,
              Acore::StringFormat("chain timing: cyclone waits {}, due {}, instant waits {}, held in immunity {}; "
                                  "reaction allowance due {}, waits {}; busy guard {}; learned delay {} then {}",
                                  cycloneWaits, cycloneDue, instantWaits, held, reactDue, reactWaits, guard, first,
                                  next));
    }

    // 26. G4c-b AoE guard: the areas read from the spell store (around the caster, at the target, a cone, a chain;
    // none for a single target or a DoT) and the cover test with the margin
    {
        struct AreaCase
        {
            uint32 SpellId;
            CcAreaCentre Centre;
        };
        constexpr std::array<AreaCase, 10> Cases = {{{10, CcAreaCentre::Target},
                                                     {2120, CcAreaCentre::Target},
                                                     {16914, CcAreaCentre::Target},
                                                     {48045, CcAreaCentre::Target},
                                                     {1449, CcAreaCentre::Caster},
                                                     {26573, CcAreaCentre::Caster},
                                                     {120, CcAreaCentre::Cone},
                                                     {845, CcAreaCentre::Chain},
                                                     {118, CcAreaCentre::None},
                                                     {8921, CcAreaCentre::None}}};
        std::string wrong;
        std::string radii;
        for (AreaCase const& area : Cases)
        {
            CcSpellArea const found = CcResolveSpellArea(sSpellMgr->GetSpellInfo(area.SpellId));
            bool const ok =
                found.Centre == area.Centre && (area.Centre == CcAreaCentre::None || found.RadiusYards > 0.0f);
            if (!ok)
                wrong += Acore::StringFormat("{}{} centre {} radius {:.1f}", wrong.empty() ? "" : ", ", area.SpellId,
                                             static_cast<uint32>(found.Centre), found.RadiusYards);

            if (area.Centre != CcAreaCentre::None)
                radii +=
                    Acore::StringFormat("{}{} {:.1f} yd", radii.empty() ? "" : ", ", area.SpellId, found.RadiusYards);
        }

        check(wrong.empty(), "aoe guard areas: " + wrong);
        CcSpellArea const nova{CcAreaCentre::Caster, 10.0f};
        CcSpellArea const cone{CcAreaCentre::Cone, 10.0f};
        CcSpellArea const blizzard{CcAreaCentre::Target, 8.0f};
        bool const covers =
            CcAreaCovers(nova, 2.0f, 11.5f, 30.0f, false) && !CcAreaCovers(nova, 2.0f, 12.5f, 0.0f, true) &&
            CcAreaCovers(cone, 2.0f, 5.0f, 30.0f, true) && !CcAreaCovers(cone, 2.0f, 5.0f, 30.0f, false) &&
            CcAreaCovers(blizzard, 2.0f, 40.0f, 9.5f, false) && !CcAreaCovers(blizzard, 2.0f, 1.0f, 10.5f, true) &&
            !CcAreaCovers(CcSpellArea{}, 2.0f, 0.0f, 0.0f, true);
        check(covers, "aoe guard cover test");
        handler->PSendSysMessage("TACTICS selftest cc: aoe guard radii [{}]", radii);
    }

    // 9. G5a: the aura -> kind table, the breaker pick, the cautious shift, the wasted and unknowing breaks, the dispel
    // pick, the cleansers' spells and the opponent memory's trinket uses (43 checks)
    {
        auto const m = [](uint32 mechanic) { return CcMechanicBit(mechanic); };
        struct KindCase
        {
            char const* Name;
            uint64 Mechanics;
            uint32 Bits;
            bool Has;
            CcKind Kind;
        };
        std::array<KindCase, 14> const kinds = {{
            {"polymorph", m(MECHANIC_POLYMORPH), CC_AURA_CONFUSE, true, CcKind::Incapacitate},
            {"sap", m(MECHANIC_SAPPED), CC_AURA_STUN, true, CcKind::Incapacitate},
            {"freezing trap", m(MECHANIC_FREEZE), CC_AURA_STUN, true, CcKind::Incapacitate},
            {"blind", m(MECHANIC_DISORIENTED), CC_AURA_CONFUSE, true, CcKind::Disorient},
            {"fear", m(MECHANIC_FEAR), CC_AURA_FEAR, true, CcKind::Fear},
            {"seduction", m(MECHANIC_CHARM), CC_AURA_STUN, true, CcKind::Fear},
            {"death coil", m(MECHANIC_HORROR), CC_AURA_FEAR, true, CcKind::Horror},
            {"hammer of justice", m(MECHANIC_STUN), CC_AURA_STUN, true, CcKind::Stun},
            {"wyvern sting", m(MECHANIC_SLEEP), CC_AURA_STUN, true, CcKind::Sleep},
            {"cyclone", 0, CC_AURA_CYCLONE | CC_AURA_STUN, true, CcKind::Cyclone},
            {"frost nova", m(MECHANIC_FREEZE), CC_AURA_ROOT, true, CcKind::Root},
            {"hamstring", m(MECHANIC_SNARE), CC_AURA_SNARE, true, CcKind::Snare},
            {"silence", m(MECHANIC_SILENCE), CC_AURA_SILENCE, true, CcKind::Silence},
            {"a bleed", m(MECHANIC_BLEED), 0, false, CcKind::Root},
        }};
        for (KindCase const& kindCase : kinds)
        {
            CcKind kind = CcKind::Root;
            bool const has = CcKindOfAura(kindCase.Mechanics, kindCase.Bits, kind);
            check(has == kindCase.Has && (!has || kind == kindCase.Kind),
                  Acore::StringFormat("aura kind of {}: has {}, kind {}", kindCase.Name, has ? "yes" : "no",
                                      static_cast<uint32>(kind)));
        }

        CcBreakOptions all;
        all.Trinket = all.EveryMan = all.Forsaken = all.EscapeArtist = true;
        CcBreakOptions human;
        human.EveryMan = true;
        CcBreakOptions undead;
        undead.Forsaken = true;
        CcBreakOptions gnome;
        gnome.EscapeArtist = true;
        uint64 const stun = m(MECHANIC_STUN);
        uint64 const fear = m(MECHANIC_FEAR);
        uint64 const root = m(MECHANIC_ROOT);
        check(PickCcBreaker(stun, all) == CcBreaker::Trinket, "the trinket does not rank above the racials");
        check(PickCcBreaker(stun, human) == CcBreaker::EveryMan, "a human without a trinket keeps its racial");
        check(PickCcBreaker(stun, undead) == CcBreaker::None, "will of the forsaken breaks a stun");
        check(PickCcBreaker(fear, undead) == CcBreaker::Forsaken, "will of the forsaken does not break a fear");
        check(PickCcBreaker(root, gnome) == CcBreaker::EscapeArtist && PickCcBreaker(stun, gnome) == CcBreaker::None,
              "escape artist breaks other than roots");
        check(PickCcBreaker(0, all) == CcBreaker::None, "a CC without a mechanic is broken");

        CcParams const low = params(20);
        CcParams const high = params(80);
        check(CcBreakLowHealthPct(40, 100, 20) == 60 && CcBreakLowHealthPct(40, 0, 20) == 20 &&
                  CcBreakLowHealthPct(40, 50, 20) == 40,
              "cautious shift is not 60 / 20 / 40 at Caution 100 / 0 / 50");
        check(CcBreakLowHealthPct(40, 100, 0) == 40, "a shift of 0 moves the line");
        check(CcBreakLowHealthPct(90, 100, 20) == 100 && CcBreakLowHealthPct(10, 0, 20) == 0, "the line leaves 0-100");
        check(high.BreakLowHealthPct == high.LowHealthPct, "the dist's shift 0 moves the break line");
        CcParams cautious = high;
        cautious.BreakLowHealthPct = CcBreakLowHealthPct(high.LowHealthPct, 100, 20);
        CcBreakView at45;
        at45.Kind = CcKind::Stun;
        at45.RemainingMs = 4000;
        at45.SelfLow = 45 <= cautious.BreakLowHealthPct;
        bool const cautiousBreaks = CcMatters(at45, cautious);
        at45.SelfLow = 45 <= high.BreakLowHealthPct;
        bool const neutralBreaks = CcMatters(at45, high);
        check(cautiousBreaks && !neutralBreaks, "the cautious bot does not break earlier (stun at 45 % health)");

        constexpr uint32 Windows = 4000;
        uint32 wastedLow = 0;
        uint32 wastedHigh = 0;
        uint32 unknowingLow = 0;
        bool stable = true;
        for (uint32 window = 0; window < Windows; ++window)
        {
            uint32 const now = window * CC_BREAK_WINDOW_MS;
            CcBreakView idle;
            idle.Kind = CcKind::Stun;
            idle.RemainingMs = 4000;
            bool const lowBreaks = ShouldBreakCc(idle, low, Seed, now);
            wastedLow += lowBreaks ? 1 : 0;
            wastedHigh += ShouldBreakCc(idle, high, Seed, now) ? 1 : 0;
            stable = stable && lowBreaks == ShouldBreakCc(idle, low, Seed, now);
            CcBreakView mattering = idle;
            mattering.SelfLow = true;
            unknowingLow += ShouldBreakCcUnknowing(mattering, low, Seed, now) ? 1 : 0;
        }

        check(wastedHigh == 0 && high.WastedBreakPct == 0,
              Acore::StringFormat("skill 80 wasted {} breaks (from WastedBreakNeverFromSkill none)", wastedHigh));
        check(low.WastedBreakPct > 0 && withinRate(wastedLow, Windows, low.WastedBreakPct, 4),
              Acore::StringFormat("skill 20 wasted {} of {} breaks (expected about {} %)", wastedLow, Windows,
                                  low.WastedBreakPct));
        check(withinRate(unknowingLow, Windows, low.WastedBreakPct, 4),
              Acore::StringFormat("an unknowing bot broke {} of {} CCs that matter (expected about {} %)", unknowingLow,
                                  Windows, low.WastedBreakPct));
        check(stable, "the break answer is not the same for the same window");

        auto const pick = [](std::vector<CcDispelCandidate> const& members)
        {
            int32 chosen = -1;
            CcDispelCandidate best;
            for (size_t index = 0; index < members.size(); ++index)
            {
                if (CcDispelBetter(members[index], best))
                {
                    best = members[index];
                    chosen = static_cast<int32>(index);
                }
            }

            return chosen;
        };
        CcDispelCandidate const none;
        CcDispelCandidate const rooted{true, CcKind::Root};
        CcDispelCandidate const polied{true, CcKind::Incapacitate};
        CcDispelCandidate const feared{true, CcKind::Fear};
        CcDispelCandidate const snared{true, CcKind::Snare};
        CcDispelCandidate const stunned{true, CcKind::Stun};
        check(pick({rooted, none}) == 0, "the bot's own root is not dispelled");
        check(pick({rooted, polied}) == 1, "the bot's root before a partner's polymorph");
        check(pick({none, polied, feared}) == 1, "not the first partner's loss of control");
        check(pick({none, snared}) == -1, "a snare is dispelled");
        check(pick({none, none}) == -1, "nothing to dispel gives a pick");
        check(pick({none, rooted, stunned}) == 2, "a root before a stun");

        CcCleanserSpells priest;
        priest.DispelMagic = true;
        CcCleanserSpells paladin;
        paladin.Cleanse = true;
        CcCleanserSpells purifier;
        purifier.Purify = true;
        CcCleanserSpells shaman;
        shaman.CleanseSpirit = true;
        auto const is = [](char const* spell, std::string_view name)
        { return spell && std::string_view(spell) == name; };
        check(is(CcDispelSpellFor(CLASS_PRIEST, DISPEL_MAGIC, priest), "dispel magic") &&
                  !CcDispelSpellFor(CLASS_PRIEST, DISPEL_CURSE, priest),
              "priest: polymorph no or hex yes");
        check(is(CcDispelSpellFor(CLASS_SHAMAN, DISPEL_CURSE, shaman), "cleanse spirit") &&
                  !CcDispelSpellFor(CLASS_SHAMAN, DISPEL_MAGIC, shaman),
              "shaman: hex no or polymorph yes");
        check(is(CcDispelSpellFor(CLASS_PALADIN, DISPEL_MAGIC, paladin), "cleanse") &&
                  is(CcDispelSpellFor(CLASS_PALADIN, DISPEL_POISON, paladin), "cleanse") &&
                  is(CcDispelSpellFor(CLASS_PALADIN, DISPEL_POISON, purifier), "purify") &&
                  !CcDispelSpellFor(CLASS_PALADIN, DISPEL_MAGIC, purifier),
              "paladin: cleanse or purify table");
        check(!CcDispelSpellFor(CLASS_PRIEST, DISPEL_POISON, priest) &&
                  !CcDispelSpellFor(CLASS_MAGE, DISPEL_MAGIC, priest) &&
                  !CcDispelSpellFor(CLASS_SHAMAN, DISPEL_NONE, shaman),
              "a priest frees wyvern sting, or a mage or an undispellable CC is freed");

        constexpr uint32 TrinketMatch = 77;
        CcOpponentMemory memory;
        bool const first = memory.Trinketed(TrinketMatch, Enemy, 1000);
        bool const again = memory.Trinketed(TrinketMatch, Enemy, 1500);
        check(first && !again, "the trinket seen by its aura and by the CC it removed is two uses");
        bool const later = memory.Trinketed(TrinketMatch, Enemy, 125000);
        CcOpponentMemory::Slot const* const slot = memory.Find(TrinketMatch, Enemy);
        check(later && slot && slot->Trinkets == 2, "a trinket two minutes later is not counted");
        check(memory.TrinketReadyMs(TrinketMatch, Enemy) == 125000 + CC_OPPONENT_TRINKET_COOLDOWN_MS &&
                  memory.TrinketReadyMs(TrinketMatch, Enemy + 1) == 0,
              "trinket ready estimate");
        memory.Trinketed(TrinketMatch + 1, Enemy + 1, 130000);
        check(!memory.Find(TrinketMatch, Enemy), "a new match keeps the last match's trinkets");
    }

    // 10. G5a review: Avoid.EnsureTrinket's slot beside PvP gear's worn rows, and a dispel counted when its CC goes
    // (13 checks)
    {
        check(CcEnsureTrinketSlot(true, false, true, false) == 1, "two empty slots: not the second");
        check(CcEnsureTrinketSlot(true, false, false, false) == 0, "an empty first slot is not taken first");
        check(CcEnsureTrinketSlot(false, false, false, false) == 1, "two untracked trinkets: not the second");
        check(CcEnsureTrinketSlot(false, false, false, true) == 0,
              "a tracked second trinket is moved before an untracked");
        check(CcEnsureTrinketSlot(false, true, false, true) == 1, "two tracked trinkets: not the second");
        check(CcEnsureTrinketSlot(false, true, true, false) == 1, "an empty second slot is not taken first");

        CcPendingDispel pending;
        pending.Target = Enemy;
        pending.AuraSpell = 118;
        pending.CastMs = 1000;
        pending.Spell = "dispel magic";
        check(CcDispelFreed(pending, Enemy, 118, true, 1500, CC_DISPEL_CONFIRM_MS), "the dispelled CC is not counted");
        check(!CcDispelFreed(pending, Enemy, 118, false, 1500, CC_DISPEL_CONFIRM_MS), "an expiry counts as a dispel");
        check(!CcDispelFreed(pending, Enemy, 12826, true, 1500, CC_DISPEL_CONFIRM_MS),
              "another debuff dispelled counts the CC");
        check(!CcDispelFreed(pending, Enemy + 1, 118, true, 1500, CC_DISPEL_CONFIRM_MS), "another member's CC counts");
        check(CcDispelFreed(pending, Enemy, 118, true, 1000 + CC_DISPEL_CONFIRM_MS, CC_DISPEL_CONFIRM_MS) &&
                  !CcDispelFreed(pending, Enemy, 118, true, 1001 + CC_DISPEL_CONFIRM_MS, CC_DISPEL_CONFIRM_MS),
              "the confirm window's end");
        check(!CcDispelFreed(CcPendingDispel(), 0, 0, true, 0, CC_DISPEL_CONFIRM_MS), "no cast counts a dispel");
        CcPendingDispel wrapped = pending;
        wrapped.CastMs = 0xFFFFFF00u;
        check(CcDispelFreed(wrapped, Enemy, 118, true, 0x100u, CC_DISPEL_CONFIRM_MS),
              "the clock's wrap loses the dispel");
    }

    // 27. G4d: the twelve rows that had none (their classes and the core's DR groups), hungering cold's aura (a
    // database script casts it), bash and maim only in their forms, the self-centred reach, the 64-bit row masks and
    // tactics force dr's horror and scatter shot rows
    {
        struct NewRow
        {
            char const* Action;
            uint8 PlayerClass;
            uint8 Group;
        };
        static constexpr std::array<NewRow, 12> NewRows = {{
            {"hex", CLASS_SHAMAN, DIMINISHING_DISORIENT},
            {"howl of terror", CLASS_WARLOCK, DIMINISHING_FEAR},
            {"death coil", CLASS_WARLOCK, DIMINISHING_HORROR},
            {"shadowfury", CLASS_WARLOCK, DIMINISHING_CONTROLLED_STUN},
            {"psychic horror", CLASS_PRIEST, DIMINISHING_HORROR},
            {"scatter shot", CLASS_HUNTER, DIMINISHING_SCATTER_SHOT},
            {"wyvern sting", CLASS_HUNTER, DIMINISHING_DISORIENT},
            {"intimidating shout", CLASS_WARRIOR, DIMINISHING_FEAR},
            {"concussion blow", CLASS_WARRIOR, DIMINISHING_CONTROLLED_STUN},
            {"hungering cold", CLASS_DEATH_KNIGHT, DIMINISHING_DISORIENT},
            {"bash", CLASS_DRUID, DIMINISHING_CONTROLLED_STUN},
            {"maim", CLASS_DRUID, DIMINISHING_CONTROLLED_STUN},
        }};
        bool present = CC_OPTION_COUNT == 30;
        std::string missing;
        std::string groups;
        for (NewRow const& expected : NewRows)
        {
            int8 const row = CcOptionIndex(expected.Action);
            if (row == CC_NO_ROW || CC_OPTIONS[row].PlayerClass != expected.PlayerClass)
            {
                present = false;
                missing += Acore::StringFormat("{}{}", missing.empty() ? "" : ", ", expected.Action);
                continue;
            }

            if (!spells[row].Valid || spells[row].Group != expected.Group)
                groups += Acore::StringFormat("{}{} {} ({} expected)", groups.empty() ? "" : ", ", expected.Action,
                                              groupName(spells[row].Group), groupName(expected.Group));
        }

        check(present, Acore::StringFormat("not 30 rows, or a G4d row is missing or in another class: {}", missing));
        check(groups.empty(), Acore::StringFormat("a G4d row's DR group differs from the plan's: {}", groups));

        constexpr int8 Hex = CcOptionIndex("hex");
        constexpr int8 Howl = CcOptionIndex("howl of terror");
        constexpr int8 Coil = CcOptionIndex("death coil");
        constexpr int8 Scatter = CcOptionIndex("scatter shot");
        constexpr int8 Cold = CcOptionIndex("hungering cold");
        constexpr int8 Bash = CcOptionIndex("bash");
        constexpr int8 Maim = CcOptionIndex("maim");
        static_assert(Hex != CC_NO_ROW && Howl != CC_NO_ROW && Coil != CC_NO_ROW && Scatter != CC_NO_ROW &&
                          Cold != CC_NO_ROW && Bash != CC_NO_ROW && Maim != CC_NO_ROW,
                      "the selftest's G4d rows are in CC_OPTIONS");
        check(spells[Cold].AuraSpellId == 51209 && spells[Cold].BaseMs == 10000 &&
                  GetCcAuraSpell(sSpellMgr->GetSpellInfo(CC_OPTIONS[Cold].SpellId)) == sSpellMgr->GetSpellInfo(51209),
              Acore::StringFormat("hungering cold's aura is spell {} for {} ms (51209 for 10000 expected)",
                                  spells[Cold].AuraSpellId, spells[Cold].BaseMs));

        // G4d-a review NOTE 1: the shout's target gets only 20511 (the core's script), so its own landing matches
        constexpr int8 Shout = CcOptionIndex("intimidating shout");
        static_assert(Shout != CC_NO_ROW, "the selftest's shout row is in CC_OPTIONS");
        check(spells[Shout].AuraSpellId == 20511 && spells[Shout].BaseMs == 8000 &&
                  spells[Shout].Group == DIMINISHING_FEAR && spells[Shout].DamageInterrupts,
              Acore::StringFormat("shout's aura is spell {} for {} ms, group {} (20511, 8000, fear expected)",
                                  spells[Shout].AuraSpellId, spells[Shout].BaseMs, spells[Shout].Group));

        SpellInfo const* const hex = sSpellMgr->GetSpellInfo(CC_OPTIONS[Hex].SpellId);
        SpellInfo const* const bash = sSpellMgr->GetSpellInfo(CC_OPTIONS[Bash].SpellId);
        SpellInfo const* const maim = sSpellMgr->GetSpellInfo(CC_OPTIONS[Maim].SpellId);
        check(hex && bash && maim && !hex->Stances && bash->Stances && maim->Stances &&
                  bash->CheckShapeshift(FORM_NONE) != SPELL_CAST_OK &&
                  bash->CheckShapeshift(FORM_CAT) != SPELL_CAST_OK &&
                  bash->CheckShapeshift(FORM_DIREBEAR) == SPELL_CAST_OK &&
                  maim->CheckShapeshift(FORM_NONE) != SPELL_CAST_OK &&
                  maim->CheckShapeshift(FORM_DIREBEAR) != SPELL_CAST_OK &&
                  maim->CheckShapeshift(FORM_CAT) == SPELL_CAST_OK,
              "bash is not ready only in bear form, or maim only in cat form, or hex needs a form");

        check(CC_OPTIONS[Howl].ReachYards == CC_HOWL_REACH_YARDS &&
                  CC_OPTIONS[Cold].ReachYards == CC_COLD_REACH_YARDS &&
                  CcOptionReaches(CC_OPTIONS[Howl], spells[Howl], 9.5f) &&
                  !CcOptionReaches(CC_OPTIONS[Howl], spells[Howl], 10.5f) &&
                  CcOptionReaches(CC_OPTIONS[Cold], spells[Cold], 9.5f) &&
                  !CcOptionReaches(CC_OPTIONS[Cold], spells[Cold], 10.5f),
              "howl of terror or hungering cold does not reach 10 yd around the caster");

        check(sizeof(CcRowMask) == 8 && CcOptionBit(63) == (CcRowMask(1) << 63) &&
                  (CcClassMask(CLASS_DRUID) & CcOptionBit(Bash)) && (CcClassMask(CLASS_DRUID) & CcOptionBit(Maim)) &&
                  !(CcClassMask(CLASS_WARRIOR) & CcOptionBit(Bash)) &&
                  (CcClassMask(CLASS_DEATH_KNIGHT) & CcOptionBit(Cold)),
              "the row masks are not 64 bits wide, or a class mask misses its G4d rows");

        int8 const horrorRow = CcForceDrRow(DIMINISHING_HORROR, true, 0);
        int8 const scatterRow = CcForceDrRow(DIMINISHING_SCATTER_SHOT, true, 0);
        check(horrorRow == Coil && scatterRow == Scatter,
              Acore::StringFormat("force dr: horror {}, scatter shot {} (death coil, scatter shot expected)",
                                  horrorRow == CC_NO_ROW ? "none" : CC_OPTIONS[horrorRow].Action,
                                  scatterRow == CC_NO_ROW ? "none" : CC_OPTIONS[scatterRow].Action));
    }

    // 28. G4d fix: the mute list matches the names Action::getName() returns (the spell for a crowd control action;
    // the hunter's "scare beast on cc" and "freezing trap"), not the creator names; a plain "entangling roots" or
    // "hibernate" action (no crowd control class) and a creator name stay out
    {
        bool const spells = CcOldActionName("polymorph", true) && CcOldActionName("cyclone", true) &&
                            CcOldActionName("hibernate", true) && CcOldActionName("entangling roots", true) &&
                            CcOldActionName("fear", true) && CcOldActionName("banish", true);
        check(spells, "a crowd control spell action of the old cc strategies is not muted");
        check(CcOldActionName("scare beast on cc", false) && CcOldActionName("freezing trap", false),
              "the hunter's scare beast or freezing trap action is not muted");
        check(!CcOldActionName("entangling roots", false) && !CcOldActionName("hibernate", false) &&
                  !CcOldActionName("fear", false) && !CcOldActionName("polymorph", false),
              "a spell-named action that is no crowd control class is muted");
        check(!CcOldActionName("cyclone on cc", true) && !CcOldActionName("fear on cc", true) &&
                  !CcOldActionName("scare beast on cc", true) && !CcOldActionName("sap", true) &&
                  !CcOldActionName("repentance", true) && !CcOldActionName("shackle undead", true),
              "a creator name or a rotation CC (sap, repentance, shackle) is muted");
    }

    handler->PSendSysMessage("TACTICS selftest cc: {} ({} checks, {} failed)", failures ? "FAIL" : "PASS", checks,
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

    // The target hold and the team plan (G1b, FocusState.h)
    FocusBotState& focus = botAI->GetFocusState();
    HoldParams const& holdParams = focus.Params.Hold;
    TeamParams const& teamParams = focus.Params.Team;
    handler->PSendSysMessage(
        "  focus hold: {} ms, hysteresis {}, execute {} % at {} %, follow {} % after {} ms, sight {} ms",
        holdParams.HoldMs, holdParams.HysteresisPct, holdParams.ExecChance, holdParams.ExecPct, holdParams.FollowChance,
        holdParams.FollowDelayMs, holdParams.LosLostMs);
    handler->PSendSysMessage(
        "  focus plan: every {} ms, hold {} ms, hysteresis {}, execute {} % at {} %, defensive swap {}, margin {}, "
        "remark delay {} ms, min {} ms, human mark respect {} ms, sight {} ms; addon knowledge {}",
        teamParams.PlanIntervalMs, teamParams.HoldMs, teamParams.HysteresisPct, teamParams.ExecChance,
        teamParams.ExecPct, teamParams.DefensiveSwapPct, teamParams.StrongMargin, teamParams.RemarkDelayMs,
        teamParams.MinRemarkMs, teamParams.HumanMarkRespectMs, teamParams.LosLostMs,
        focus.AddonKnowledge ? "yes" : "no");
    uint32 const nowMs = getMSTime();
    FocusHoldState const& holdState = focus.Hold;
    std::string verdicts;
    for (uint8 index = 0; index < FOCUS_HOLD_VERDICTS; ++index)
    {
        if (holdState.Verdicts[index])
            verdicts += Acore::StringFormat("{}{} {}", verdicts.empty() ? "" : ", ", FocusVerdictName(index),
                                            holdState.Verdicts[index]);
    }

    handler->PSendSysMessage(
        "  hold: target {} for {} ms, last answer {}; answers [{}]; switches {}; skull out of reach {} ms",
        holdState.HeldGuid.IsEmpty() ? std::string("none") : GuidName(holdState.HeldGuid),
        holdState.HeldGuid.IsEmpty() ? 0u : getMSTimeDiff(holdState.HeldSinceMs, nowMs),
        holdState.LastVerdict < FOCUS_HOLD_VERDICTS ? FocusVerdictName(holdState.LastVerdict) : "none", verdicts,
        holdState.Switches, holdState.SkullOutOfReachMs);
    // The Interrupt tactic (G5b): the numbers at the bot's skill and Patience, and its decisions so far
    {
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        ArenaInterrupt::Params const interrupt =
            ArenaInterrupt::ComputeParams(botAI->GetPvpSkill(), botAI->GetAttribute(BotAttribute::Patience),
                                          config.SkillGate.CurveExponent, config.TacticInterrupt);
        InterruptBotState const& decisions = botAI->GetInterruptState();
        handler->PSendSysMessage(
            "  interrupt: {}; heal share {:.2f}, damage {:.2f}, cc {:.2f}, sigma {:.2f}, patience {:+.2f}; judged heal "
            "{} "
            "cc {} damage {} other {}, shortcuts {}, skipped {}, kicks let go {}",
            botAI->TacticEnabledHere(TacticId::Interrupt) ? "on here" : "off here", interrupt.HealShare,
            interrupt.DamageShare, interrupt.CcShare, interrupt.Sigma, interrupt.PatienceShare,
            decisions.Decided[static_cast<uint32>(ArenaInterrupt::Kind::Heal)],
            decisions.Decided[static_cast<uint32>(ArenaInterrupt::Kind::Cc)],
            decisions.Decided[static_cast<uint32>(ArenaInterrupt::Kind::Damage)],
            decisions.Decided[static_cast<uint32>(ArenaInterrupt::Kind::Other)], decisions.Shortcuts, decisions.Skips,
            decisions.Kicks);
    }
    FocusMatchState const& match = focus.Match;
    auto const enemyName = [&match](int8 index)
    {
        return index >= 0 && index < static_cast<int8>(match.EnemyCount) ? GuidName(match.Enemies[index].Guid)
                                                                         : std::string("none");
    };
    auto const iconName = [](ObjectGuid guid) { return guid.IsEmpty() ? std::string("none") : GuidName(guid); };
    std::string remarks;
    for (uint8 index = 0; index < FOCUS_REMARK_REASONS; ++index)
    {
        if (match.Remarks[index])
            remarks += Acore::StringFormat("{}{} {}", remarks.empty() ? "" : ", ",
                                           RemarkReasonName(static_cast<RemarkReason>(index)), match.Remarks[index]);
    }

    handler->PSendSysMessage(
        "  plan: match {} ({}), team cc {} physical {}; kill {} cc {} ({}, held {} ms more); skull {} moon {}; "
        "pending {} ({} ms ago); remarks [{}]; switches {}, marks {}, margin re-marks {}, marks counted as a real "
        "player's {}",
        match.MatchId, match.Planner ? "planner" : "not the planner", match.Team.CcCapable ? "yes" : "no",
        match.Team.PhysicalTeam ? "yes" : "no", enemyName(match.Plan.Kill), enemyName(match.Plan.Cc),
        PlanReasonName(match.Plan.Reason),
        FocusBefore(nowMs, match.Plan.HeldUntilMs) ? match.Plan.HeldUntilMs - nowMs : 0, iconName(match.SeenSkull),
        iconName(match.SeenMoon), RemarkEventName(match.PendingEvent),
        match.PendingEvent == RemarkEvent::None ? 0u : getMSTimeDiff(match.EventAtMs, nowMs), remarks,
        match.PlanSwitches, match.MarkWrites, match.MarginRemarks, match.HumanMarksSeen);
    if (FocusForcedImmune(focus, focus.ForcedImmuneGuid, nowMs))
        handler->PSendSysMessage("  forced immune: {} for {} ms more", GuidName(focus.ForcedImmuneGuid),
                                 focus.ForcedImmuneUntilMs - nowMs);

    // G1b-c's moon and G4b's crowd control (the bot's own state; nothing of it is shared with the team, DT.1)
    handler->PSendSysMessage("  moon: mode {}, capable {}, releases {}, blocked {} ms more", match.MoonMode,
                             match.MoonCapable ? "yes" : "no", match.MoonReleases,
                             FocusBefore(nowMs, match.MoonBlockedUntilMs) ? match.MoonBlockedUntilMs - nowMs : 0);
    CcBotState const& cc = botAI->GetCcState();
    std::string ccRows;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        if (cc.KnownMask & CcOptionBit(index))
            ccRows += Acore::StringFormat("{}{} {}{} ({} ms)", ccRows.empty() ? "" : ", ", CC_OPTIONS[index].Action,
                                          cc.KnownSpell[index], (cc.ReadyMask & CcOptionBit(index)) ? " ready" : "",
                                          cc.Spells[index].BaseMs);
    }

    handler->PSendSysMessage(
        "  cc: dr aware {}, chains {}, wrong target {} %, own break {} %, hold {} ms; rows [{}]; target {}; last pick "
        "{} on {}; casts {}",
        cc.Params.DrAware ? "yes" : "no", cc.Params.Chains ? "yes" : "no", cc.Params.WrongTargetPct,
        cc.Params.OwnBreakPct, cc.Params.HoldMs, ccRows, iconName(cc.CcTarget),
        cc.PickRow != CC_NO_ROW ? CC_OPTIONS[cc.PickRow].Action : "none", iconName(cc.PickTarget), cc.Casts);
    handler->PSendSysMessage(
        "  cc chain: {}, lead {} ms, waits {}, chained {}, last gap {} ms, re-cc {}, react {} ms ({}), guard mutes {}",
        !cc.Params.Chains                           ? "no (skill)"
        : sPlayerbotAIConfig.TacticCc.CcChainEnable ? "on"
                                                    : "off (Cc.ChainEnable 0)",
        cc.Params.ChainLeadMs, cc.ChainWaits, cc.ChainsLanded, cc.LastChainGapMs, cc.ReCcsLanded, cc.ChainReactMs,
        cc.ChainReactLearned ? "learned" : "not yet", cc.ChainGuardMutes);
    // G4c-d (G4c-c review 4b): the pick's group on its target as the bot's memory and the core count it
    Unit* const pickUnit =
        cc.PickRow != CC_NO_ROW && !cc.PickTarget.IsEmpty() ? botAI->GetUnit(cc.PickTarget) : nullptr;
    if (pickUnit)
    {
        uint8 const pickGroup = cc.Spells[cc.PickRow].Group;
        handler->PSendSysMessage(
            "  cc pick levels: {} ({}) on {}: memory {}, core {}", CC_OPTIONS[cc.PickRow].Action,
            PVPTEST_DR_GROUP_NAMES[pickGroup % PVPTEST_DR_GROUP_COUNT], iconName(cc.PickTarget),
            static_cast<uint32>(cc.Memory.Level(cc.PickTarget.GetRawValue(), pickGroup, nowMs)),
            static_cast<uint32>(pickUnit->GetDiminishing(static_cast<DiminishingGroup>(pickGroup))));
    }
    // G4c-b: the own-break guards and the opponent memory of the CC target (this match)
    CcConfig const& ccConfig = sPlayerbotAIConfig.TacticCc;
    CcOpponentMemory::Slot const* const seen =
        cc.CcTarget.IsEmpty() ? nullptr
                              : cc.Opponents.Find(botAI->GetBot()->GetInstanceId(), cc.CcTarget.GetRawValue());
    handler->PSendSysMessage(
        "  cc guards: aoe {}, dot {}, pet call-off {} ({} called off), fight enemies {}; opponent memory {}: cc target "
        "dispels {} (mean {} ms), trinkets {}",
        onOff(ccConfig.AvoidAoeGuardEnable), onOff(ccConfig.AvoidDotGuardEnable), onOff(ccConfig.AvoidPetCallOffEnable),
        cc.PetCallOffs, onOff(ccConfig.AvoidExcludeFightEnemies), onOff(ccConfig.CcOpponentMemoryEnable),
        seen ? seen->Dispels : 0, seen && seen->Dispels ? seen->DispelSumMs / seen->Dispels : 0,
        seen ? seen->Trinkets : 0);
    // G5a: the Trinket and Dispel tactics (CC avoidance)
    bool wornTrinket = false;
    for (uint8 const slot : {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2})
    {
        Item* const item = botAI->GetBot()->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        wornTrinket = wornTrinket || (item && CcIsPvpTrinket(item->GetTemplate()));
    }

    uint32 const trinketReady = cc.CcTarget.IsEmpty() ? 0
                                                      : cc.Opponents.TrinketReadyMs(botAI->GetBot()->GetInstanceId(),
                                                                                    cc.CcTarget.GetRawValue());
    handler->PSendSysMessage(
        "  avoid: trinket tactic {}, pvp trinket {}{}, answer {}{}, breaks {} (wasted {}, last {} ms after the CC), "
        "low line {} %, ensure {}; dispel tactic {}, pick {} ({}), partners freed {} (last {} ms after the CC); cc "
        "target's trinket ready at {} ms (now {} ms)",
        onOff(botAI->TacticEnabledHere(TacticId::Trinket)), wornTrinket ? "worn" : "none",
        cc.TrinketWithheld ? " (withheld)" : "", CcBreakerAction(cc.BreakWith),
        cc.BreakWith == CcBreaker::None ? "" : (cc.BreakMatters ? " (matters)" : " (wasted)"), cc.Breaks,
        cc.WastedBreaks, cc.LastBreakAgeMs, cc.Params.BreakLowHealthPct, ccConfig.AvoidEnsureTrinket,
        onOff(botAI->TacticEnabledHere(TacticId::Dispel)), iconName(cc.DispelTarget),
        cc.DispelSpell ? cc.DispelSpell : "none", cc.Dispels, cc.LastDispelAgeMs, trinketReady, nowMs);

    // OwnCc (G4b-b): what the bot keeps out of its targets now, the forced CC, its swing (G4b's forced proof)
    std::string keptOut;
    for (ObjectGuid const& guid : cc.Excluded)
        keptOut += (keptOut.empty() ? "" : ", ") + GuidName(guid);

    Unit* const victim = botAI->GetBot()->GetVictim();
    handler->PSendSysMessage(
        "  own cc: kept out [{}] ({} ms old), forced {} ({} ms more), swing {}, at {} ms", keptOut,
        FocusBefore(nowMs, cc.ExcludedUntilMs) ? CC_EXCLUDE_REFRESH_MS - (cc.ExcludedUntilMs - nowMs)
                                               : CC_EXCLUDE_REFRESH_MS,
        iconName(cc.ForcedCcGuid), FocusBefore(nowMs, cc.ForcedCcUntilMs) ? cc.ForcedCcUntilMs - nowMs : 0,
        victim ? GuidName(victim->GetGUID()) : std::string("none"), nowMs);

    // getMSTime stamps, comparable with the force's: G1b's forced proof polls these
    handler->PSendSysMessage("  timing: now {} ms, current target {}, last mark at {} ms", nowMs,
                             iconName(botAI->GetBot()->GetTarget()), match.LastMarkMs);

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
    if (!name.empty() && name != "scope" && name != "gate" && name != "reaction" && name != "attributes" &&
        name != "focus" && name != "cc" && name != "interrupt")
    {
        handler->PSendSysMessage(
            "No tactics selftest '{}'. Known: scope, gate, reaction, attributes, focus, cc, interrupt", name);
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

    if (name.empty() || name == "focus")
        RunFocusSelftest(handler);

    if (name.empty() || name == "cc")
        RunCcSelftest(handler);

    if (name.empty() || name == "interrupt")
        RunInterruptSelftest(handler);

    return true;
}

// tactics force stun / force health (G5a's forced check of the Trinket tactic), on the world thread while the maps
// wait: a real stun aura on the bot (CC_FORCE_STUN_SPELL, cast by its skull or target when there is one, else by
// itself), or its health set to a share of its maximum
static bool ForceBreakCheck(ChatHandler* handler, PlayerbotAI* botAI, std::string const& name, std::string const& kind,
                            std::string const& percentText)
{
    Player* const bot = botAI->GetBot();
    uint32 const nowMs = getMSTime();
    if (kind == "health")
    {
        uint32 const percent = Acore::StringTo<uint32>(percentText).value_or(0);
        if (!percent || percent > 100)
        {
            handler->PSendSysMessage("Usage: .playerbots tactics force health <bot name or guid> <percent 1-100>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bot->SetHealth(std::max<uint32>(1, static_cast<uint32>(uint64(bot->GetMaxHealth()) * percent / 100)));
        handler->PSendSysMessage("TACTICS force health: {} at {} % ({} of {}); at {} ms", name, percent,
                                 bot->GetHealth(), bot->GetMaxHealth(), nowMs);
        return true;
    }

    Group* const group = bot->GetGroup();
    ObjectGuid const skull = group ? group->GetTargetIcon(FOCUS_ICON_SKULL) : ObjectGuid::Empty;
    ObjectGuid const enemyGuid = skull.IsEmpty() ? bot->GetTarget() : skull;
    Unit* const enemy = enemyGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, enemyGuid);
    Unit* const caster = enemy && enemy->IsAlive() && enemy->IsInMap(bot) ? enemy : bot;
    Aura* const aura = caster->AddAura(CC_FORCE_STUN_SPELL, bot);
    if (!aura)
    {
        handler->PSendSysMessage("TACTICS force stun: spell {} did not land on {}", CC_FORCE_STUN_SPELL, name);
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage(
        "TACTICS force stun: {} stunned by {} ({} {}, {} ms) at {} ms, health {:.0f} %; trinket tactic {} here, "
        "answer within {} ms plus the reaction",
        name, caster->GetName(), aura->GetSpellInfo()->SpellName[0], CC_FORCE_STUN_SPELL, aura->GetDuration(), nowMs,
        bot->GetHealthPct(), botAI->TacticEnabledHere(TacticId::Trinket) ? "enabled" : "not enabled",
        CC_BREAK_REFRESH_MS);
    return true;
}

// World thread while the map threads wait (as SetPvpSkill): the team's bots get the forced immunity at once, as they
// would all see an immunity aura on the enemy; nothing else changes
bool TacticCommands::HandleForceCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("TacticCommands::HandleForceCommand");
    std::istringstream in(args ? args : "");
    std::string verb;
    std::string target;
    std::string groupText;
    std::string levelText;
    in >> verb >> target >> groupText >> levelText;
    std::string const kind = Lower(verb);
    if ((kind != "immune" && kind != "cc" && kind != "dr" && kind != "stun" && kind != "health") || target.empty())
    {
        handler->PSendSysMessage(
            "Usage: .playerbots tactics force immune|cc|stun <bot name or guid>, force health <bot name or guid> "
            "<percent 1-100>, or force dr <bot name or guid> <DR group> <level 1-3>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string name;
    PlayerbotAI* botAI = FindBot(handler, target, name);
    if (!botAI)
        return false;

    // force stun / force health (G5a): the bot alone, no skull needed
    if (kind == "stun" || kind == "health")
        return ForceBreakCheck(handler, botAI, name, kind, groupText);

    Player* const bot = botAI->GetBot();
    Group* const group = bot->GetGroup();
    ObjectGuid const skull = group ? group->GetTargetIcon(FOCUS_ICON_SKULL) : ObjectGuid::Empty;
    ObjectGuid const enemy = skull.IsEmpty() ? bot->GetTarget() : skull;
    if (enemy.IsEmpty())
    {
        handler->PSendSysMessage("TACTICS force {}: {} has no skull and no target.", kind, name);
        handler->SetSentErrorMessage(true);
        return false;
    }

    // force dr (G4c): real casts on the enemy, the DR level the core and the team's memories reach
    if (kind == "dr")
        return ForceCcDr(handler, botAI, name, enemy, skull.IsEmpty() ? "the bot's target" : "the skull", groupText,
                         levelText);

    uint32 const nowMs = getMSTime();
    uint32 forced = 0;
    bool const cc = kind == "cc";
    auto const force = [&](PlayerbotAI* ai)
    {
        // force cc (G4b): the enemy counts as held in a breakable CC of the team (OwnCc keeps it out of the targets,
        // whatever the own-break window); no CC is cast
        if (cc)
        {
            CcBotState& ccState = ai->GetCcState();
            ccState.ForcedCcGuid = enemy;
            ccState.ForcedCcUntilMs = nowMs + CC_FORCE_MS;
            ccState.ExcludedUntilMs = 0;
            ++forced;
            return;
        }

        FocusBotState& state = ai->GetFocusState();
        state.ForcedImmuneGuid = enemy;
        state.ForcedImmuneAtMs = nowMs;
        state.ForcedImmuneUntilMs = nowMs + FOCUS_FORCE_IMMUNE_MS;
        ++forced;
    };

    if (!group)
        force(botAI);
    else
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            PlayerbotAI* const memberAI = member && member->IsInMap(bot) ? GET_PLAYERBOT_AI(member) : nullptr;
            if (memberAI)
                force(memberAI);
        }
    }

    if (cc)
    {
        handler->PSendSysMessage(
            "TACTICS force cc: {} ({}) counts as held in a team CC for {} ms for {} bots of {}'s team; at {} ms",
            GuidName(enemy), skull.IsEmpty() ? "the bot's target" : "the skull", CC_FORCE_MS, forced, name, nowMs);
        return true;
    }

    handler->PSendSysMessage(
        "TACTICS force immune: {} ({}) counts as immune to everything for {} ms for {} bots of "
        "{}'s team; at {} ms",
        GuidName(enemy), skull.IsEmpty() ? "the bot's target" : "the skull", FOCUS_FORCE_IMMUNE_MS, forced, name,
        nowMs);
    return true;
}

bool TacticCommands::HandleMoonCommand(ChatHandler* handler, char const* args)
{
    // The world thread while the maps wait: the plan passes read the value at a match start, the hold of a match
    // without a plan pass yet live (FocusStrategy.cpp FocusMatchMoonMode)
    PLAYERBOT_CHECK_WORLD_THREAD("TacticCommands::HandleMoonCommand");
    uint32& mode = sPlayerbotAIConfig.TacticFocus.FocusMoonMode;
    std::string const value = Trim(args ? args : "");
    if (value.empty())
    {
        handler->PSendSysMessage("TACTICS moon: Playerbots.Tactics.Focus.MoonMode {}", mode);
        return true;
    }

    if (value != "0" && value != "1" && value != "2")
    {
        handler->PSendSysMessage("Usage: .playerbots tactics moon [0|1|2] (0 no moon, 1 G1b, 2 only for a CC team)");
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 const before = mode;
    mode = static_cast<uint32>(value[0] - '0');
    handler->PSendSysMessage(
        "TACTICS moon: Playerbots.Tactics.Focus.MoonMode {} -> {}; teams take it at their next match start (a match "
        "in progress keeps its mode); a restart or .playerbots rndbot reload restores the conf value",
        before, mode);
    return true;
}

bool TacticCommands::HandleEnsureTrinketCommand(ChatHandler* handler, char const* args)
{
    // The world thread while the maps wait: a harness fight start reads it (CcEnsureTrinket)
    PLAYERBOT_CHECK_WORLD_THREAD("TacticCommands::HandleEnsureTrinketCommand");
    uint32& mode = sPlayerbotAIConfig.TacticCc.AvoidEnsureTrinket;
    std::string const value = Trim(args ? args : "");
    if (value.empty())
    {
        handler->PSendSysMessage("TACTICS ensuretrinket: Playerbots.Tactics.Avoid.EnsureTrinket {}", mode);
        return true;
    }

    if (value != "0" && value != "1" && value != "2")
    {
        handler->PSendSysMessage(
            "Usage: .playerbots tactics ensuretrinket [0|1|2] (0 as geared, 1 a PvP trinket for a bot without one, 2 "
            "no PvP trinket used)");
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 const before = mode;
    mode = static_cast<uint32>(value[0] - '0');
    handler->PSendSysMessage(
        "TACTICS ensuretrinket: Playerbots.Tactics.Avoid.EnsureTrinket {} -> {}; the next harness fight start takes "
        "it (a match in progress keeps its trinkets); a restart or .playerbots rndbot reload restores the conf value",
        before, mode);
    return true;
}
