/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Tactics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <sstream>
#include <vector>

#include "ArenaCc.h"
#include "ArenaCcSpells.h"
#include "ArenaCover.h"
#include "ArenaDefensive.h"
#include "ArenaDrink.h"
#include "ArenaFocus.h"
#include "ArenaInterrupt.h"
#include "ArenaInterruptSim.h"
#include "ArenaInterruptSpells.h"
#include "ArenaJump.h"
#include "ArenaKite.h"
#include "BotAttributes.h"
#include "BotProfileMgr.h"
#include "CcState.h"
#include "CcTacticStrategy.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "FocusState.h"
#include "Group.h"
#include "Item.h"
#include "LosTactic.h"
#include "NaturalCast.h"
#include "NaturalMove.h"
#include "NaturalPath.h"
#include "NaturalReaction.h"
#include "NaturalTurn.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PillarTactic.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "SkillGrowth.h"
#include "SpellAuras.h"
#include "StringConvert.h"
#include "TacticChecks.h"
#include "TacticGate.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "WorldSession.h"

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

    // G5c-c: the kicks aimed at one enemy (the kick spells a match registers); Arcane Torrent (every rank) is none
    constexpr std::array<std::pair<uint32, bool>, 12> Aimed = {{{1766, true},
                                                                {6552, true},
                                                                {72, true},
                                                                {2139, true},
                                                                {47528, true},
                                                                {47476, true},
                                                                {15487, true},
                                                                {34490, true},
                                                                {57994, true},
                                                                {28730, false},
                                                                {25046, false},
                                                                {50613, false}}};
    for (auto const& [spellId, expected] : Aimed)
    {
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(spellId);
        check(info && InterruptUseOf(info) == InterruptUse::Kick && InterruptKickAimed(info) == expected,
              Acore::StringFormat("spell {} is a kick aimed at one enemy: {}", spellId, expected ? "yes" : "no"));
    }

    SpellInfo const* const shieldBash = sSpellMgr->GetSpellInfo(72);
    check(shieldBash && shieldBash->EquippedItemClass == ITEM_CLASS_ARMOR &&
              (shieldBash->EquippedItemSubClassMask & (1 << ITEM_SUBCLASS_ARMOR_SHIELD)) != 0,
          "Shield Bash needs a shield (an Arms warrior without one never registers it)");

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
    Int::Params const p99 = Int::ComputeParams(99, Neutral, 99, Exponent, config);
    Int::Params const p0 = Int::ComputeParams(0, Neutral, 0, Exponent, config);
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
    Int::Decision const shortcut = Int::Decide(weak, Int::ComputeParams(40, Neutral, 40, Exponent, config), Hash);
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
    double const mean50 =
        meanShare(heal, Int::ComputeParams(50, Neutral, 50, Exponent, config), nullptr, nullptr, nullptr);
    check(mean50 < mean - 0.1, Acore::StringFormat("skill 50 commits earlier ({:.3f})", mean50));
    double const meanPatient =
        meanShare(heal, Int::ComputeParams(99, 100, 99, Exponent, config), nullptr, nullptr, nullptr);
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

    // G5c-c: the hold policy and its fight model (ArenaInterruptSim.h)
    ArenaInterruptSim::RunHoldSelftest(check, [handler](std::string const& line)
                                       { handler->PSendSysMessage("TACTICS selftest interrupt: {}", line); });

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
// The suites that need the server
struct ServerTacticSuite
{
    char const* Name;
    void (*Run)(ChatHandler* handler);
};

constexpr std::array<ServerTacticSuite, 3> SERVER_TACTIC_SUITES = {
    {{"scope", RunScopeSelftest}, {"cc", RunCcSelftest}, {"interrupt", RunInterruptSelftest}}};

// The gate suite reads BotProfileMgr's percentile math (BotProfileMgr.cpp), which the offline test does not link
constexpr std::array<TacticSuite, 1> SERVER_PURE_TACTIC_SUITES = {
    {{"gate",
      [](TacticCheck const& check, TacticSay const& say, TacticSuiteInputs const&) { RunGateChecks(check, say); }}}};

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
        SkillStretchApplies(botAI->IsSkillStretchOff(), config.NaturalCadenceSkillStretch) ? "on" : "off",
        timing.EndToEnd ? "on" : "off", timing.PingMs, timing.DisplayMs,
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
        ArenaInterrupt::Params const interrupt = ArenaInterrupt::ComputeParams(
            botAI->GetPvpSkill(), botAI->GetAttribute(BotAttribute::Patience),
            botAI->GetAttribute(BotAttribute::Knowledge), config.SkillGate.CurveExponent, config.TacticInterrupt);
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
        // G5c-c: the hold policy's numbers at the bot's Knowledge and Patience, the window's reaction, the episodes
        uint32 const windowMs =
            ArenaInterrupt::HoldReactionMs(reaction, botAI->GetReactionTypes(), ReactionEase::Common,
                                           timing.EndToEnd ? timing.PingMs + timing.DisplayMs : 0, timing.MinMs);
        bool const kickReady = decisions.KickReady(nowMs);
        handler->PSendSysMessage(
            "  interrupt hold: policy {}; use {:.2f}, attention {:.0f} ms; know {:.2f}, dilution {:.2f}, trend {:.0f} "
            "ms, longest hold {} ms, window reaction {} ms; held {}, episodes {} (paid {}, vain {}, missed {}, expired "
            "{}){}; kick {} {} ms, kick spells {}",
            interrupt.Hold.Enable ? 1 : 0, interrupt.UseShare, interrupt.AttentionMs, interrupt.Hold.KnowShare,
            interrupt.Hold.KnowDilution, interrupt.Hold.TrendMs, interrupt.Hold.MaxHoldMs, windowMs, decisions.Holds,
            decisions.HoldEpisodes, decisions.HoldsPaid, decisions.HoldsVain, decisions.HoldsMissed,
            decisions.HoldsExpired, decisions.Hold.Active ? ", one open" : "", kickReady ? "ready for" : "ready in",
            kickReady ? decisions.KickReadyMs(nowMs) : decisions.KickReadyAtMs - nowMs,
            std::count_if(decisions.KickSpells.begin(), decisions.KickSpells.end(),
                          [](InterruptKickSpell const& kick) { return kick.SpellId != 0; }));
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
    // G4e: the focus target (the Dexterity gate, the moon's share by Teamwork, the pick and the actions on it)
    {
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        FocusTargetState const& target = focus.Target;
        uint8 const dexterity = botAI->GetAttribute(BotAttribute::Dexterity);
        uint8 const threshold = config.NaturalReaction.Ease.FocusDexterity;
        uint8 const teamwork = botAI->GetPvpSkill() == 0 ? 99 : botAI->GetAttribute(BotAttribute::Teamwork);
        handler->PSendSysMessage(
            "  focus target: {}, capable {} (dexterity {}, threshold {}), moon share {:.2f}; focus {} ({}) for {} ms, "
            "picks {}; kicks {}, cc {} on it, switch kicks {}",
            botAI->TacticEnabledHere(TacticId::FocusTarget) ? "on here" : "off here",
            FocusTargetCapable(botAI->GetPvpSkill(), dexterity, threshold) ? "yes" : "no", dexterity, threshold,
            FocusMoonShare(teamwork, config.TacticFocus.FocusTargetMoonPctAtTeamwork1,
                           config.TacticFocus.FocusTargetMoonPctAtTeamwork99),
            target.Pick.Guid ? GuidName(ObjectGuid(target.Pick.Guid)) : std::string("none"),
            FocusSourceName(target.Pick.Source), target.Pick.Guid ? getMSTimeDiff(target.Pick.SinceMs, nowMs) : 0u,
            target.Pick.Picks, target.Kicks, target.Ccs, target.SwitchKicks);
    }
    // G5d: the Defensive and Protect tactics (the bot's lines, its sampler now, what it pressed and held)
    {
        DefensiveBotState const& defensive = botAI->GetDefensiveState();
        DefensiveParams const& params = defensive.Params;
        Player* const self = botAI->GetBot();
        uint32 const hpPct = self->IsAlive() ? static_cast<uint32>(std::ceil(self->GetHealthPct())) : 0;
        handler->PSendSysMessage(
            "  defensive: {}, protect {}; burst {} % in {} ms, line {} % (+{} focused by {}), critical {} %, "
            "opener {} % ({}), late {} ms; now lost {} %, focused by {}; pressed {} (last {}, {} ms ago), self {}, "
            "partner {}; protect line {} %; burst holds {} (memory {}, lead {} %)",
            botAI->TacticEnabledHere(TacticId::Defensive) ? "on here" : "off here",
            botAI->TacticEnabledHere(TacticId::Protect) ? "on here" : "off here", params.BurstLossPct, params.WindowMs,
            params.HpPct, params.FocusedHpBonusPct, params.FocusedMinEnemies, params.CriticalHpPct, params.OpenerPct,
            defensive.OpenerDrawn ? "drawn" : "not drawn", params.LateMs,
            defensive.Ring.LostPct(nowMs, hpPct, params.WindowMs), defensive.FocusedBy, defensive.Presses,
            DefensiveReasonName(defensive.LastReason),
            defensive.LastPressMs ? getMSTimeDiff(defensive.LastPressMs, nowMs) : 0u, defensive.SelfUses,
            defensive.PartnerUses, params.ProtectHpPct, defensive.Holds, params.OpponentKnows ? "known" : "unknown",
            params.OpponentLeadPct);
    }
    // G5e: the Fake tactic (its numbers, what it decided, cancelled and baited, the kicks it saw)
    {
        FakeBotState const& fake = botAI->GetFakeState();
        FakeParams const& params = fake.Params;
        handler->PSendSysMessage(
            "  fake: {}, known {}, chance {} %, cancel at {} % (sigma {} %), repeat {} ms, kick memory {}; decided {}, "
            "faked {}, let finish {}, cast gone {}, baited {}, kicks seen {}; last {} (cancel at {} %, error {} ms, "
            "{} ms ago); pending {}",
            botAI->TacticEnabledHere(TacticId::Fake) ? "on here" : "off here", params.Known ? "yes" : "no",
            params.ChancePct, params.CancelPct, params.CancelSigmaPct, params.RepeatMs,
            params.RemembersKicks ? "yes" : "no", fake.Decisions, fake.Fakes, fake.Finished, fake.Gone, fake.Baited,
            fake.KicksSeen, FakeVerdictName(fake.LastVerdict), fake.LastCancelPct, fake.LastErrorMs,
            fake.LastFakeMs ? getMSTimeDiff(fake.LastFakeMs, nowMs) : 0u, fake.Pending() ? "yes" : "no");
    }
    // G2a: the natural facing (its numbers, what the bot turned, sent and waited)
    {
        TurnBotState const& turn = botAI->GetTurnState();
        TurnParams const& params = turn.Params;
        handler->PSendSysMessage(
            "  turn: {}, mode {}, 180 in {} ms, error {:.1f} deg, correction {} %, keyboard {} %; turns {} (keyboard "
            "{}), corrections {}, packets {}, splines {}, snaps {}, abandoned {}, cast waits {} ({} ms); last {} deg "
            "in {} ms, error {:.1f} deg, {} ms ago; turning {}",
            NaturalFacingHere(botAI, nowMs) ? "on here" : "off here", params.Mode, params.MsFor180, params.ErrorDeg,
            params.CorrectionPct, params.KeyboardPct, turn.Turns, turn.KeyboardTurns, turn.Corrections, turn.Packets,
            turn.Splines, turn.Snaps, turn.Abandons, turn.CastWaits, turn.CastWaitMs, turn.LastTurnDeg,
            turn.LastTurnDurMs, turn.LastErrorDeg, turn.LastTurnMs ? getMSTimeDiff(turn.LastTurnMs, nowMs) : 0u,
            turn.Active() ? "yes" : "no");
    }
    // G2b: the natural tracking (its numbers, how the bot chased)
    {
        TrackBotState const& track = botAI->GetTrackState();
        TrackParams const& params = track.Params;
        auto const mode = [&track](TrackMode which) { return track.Modes[static_cast<uint32>(which)]; };
        handler->PSendSysMessage(
            "  track: {}, lag {} ms, {}, gain {:.2f}, history {} ms, sample gap {} ms; chases {} (mean age {} ms; last "
            "seen {}, standing {}, velocity {}, arc {}, juke {}, pattern {}), capped {}, unstuck {}, restarts {}; last "
            "{} ({} ms old, lead {:.1f} yd, {:.1f} yd off, {} ms ago)",
            NaturalTrackHere(botAI, nowMs) ? "on here" : "off here", params.LagMs, TrackLevelName(params.Level()),
            params.Gain, params.HistoryMs, TrackSampleGapMs(params, sPlayerbotAIConfig.NaturalTrack), track.Chases,
            track.Chases ? track.LagMsSum / track.Chases : 0u, mode(TrackMode::LastSeen), mode(TrackMode::Standing),
            mode(TrackMode::Velocity), mode(TrackMode::Arc), mode(TrackMode::Juke), mode(TrackMode::Pattern),
            track.Capped, track.Unsticks, track.Restarts, TrackModeName(track.LastMode), track.LastLagMs,
            track.LastLeadYards, track.LastOffYards, track.LastMs ? getMSTimeDiff(track.LastMs, nowMs) : 0u);
    }
    // G2c: the natural paths (its numbers, what the bot walked)
    {
        PathBotState const& path = botAI->GetPathState();
        PathParams const& params = path.Params;
        PathConfig const& config = sPlayerbotAIConfig.NaturalPath;
        handler->PSendSysMessage(
            "  path: {}, offset {:.2f} yd, curves {}, strafes {} (every {} ms), hops {} (every {} ms), walk {}; paths "
            "{} (offset points {}, refused {}, curved {}), strafes {} (refused {}), hops {} (refused {}, last {}), "
            "walks {}; last shaped {} ms ago",
            NaturalPathHere(botAI, nowMs) ? "on here" : "off here", config.OffsetsEnable ? params.OffsetYards : 0.0f,
            config.CurvesEnable ? "on" : "off", config.StrafeEnable && params.Strafe ? "yes" : "no",
            params.StrafeEveryMs, config.HopsEnable && params.Hops ? "yes" : "no", params.HopEveryMs,
            config.WalkEnable ? "on" : "off", path.Paths, path.OffsetPoints, path.Dropped, path.Curves, path.Strafes,
            path.StrafesRefused, path.Hops, path.HopsRefused, PathHopWhyName(path.LastHopWhy), path.Walks,
            path.LastShapeMs ? getMSTimeDiff(path.LastShapeMs, nowMs) : 0u);
    }
    // G2e: the Los tactic (its numbers, the episode, the held point, what it did)
    {
        LosBotState const& los = botAI->GetAiObjectContext()->GetValue<LosBotState&>("los state")->Get();
        CoverPlan const plan = botAI->GetAiObjectContext()->GetValue<CoverPlan>("cover plan")->Get();
        LosParams const& params = los.Params;
        handler->PSendSysMessage(
            "  los: {}, known {}, late {} ms, into open {} %, hides from caster {}, healer stays {}; lost {} ({} ms, "
            "episode {}, wait {} ms{}{}), regaining {} ms, point {} ({} pillar {}, {:.1f} yd away, hidden {}); "
            "regains {} (regained {}, timeouts {}, aborted {}), guarded {}, searches {} (cover {}, path {}, none {}; "
            "rays {}, last {}), partner lost {} ms",
            botAI->TacticEnabledHere(TacticId::Los) ? "on here" : "off here", params.Known ? "yes" : "no",
            params.LateMs, params.IntoOpenPct, params.HidesFromCaster ? "yes" : "no", params.HealerStays ? "yes" : "no",
            LosWantName(los.Want), los.LostSinceMs ? getMSTimeDiff(los.LostSinceMs, nowMs) : 0u, los.Episode,
            los.EpisodeLateMs, los.EpisodeIntoOpen ? ", into the open" : "", los.GaveUp ? ", given up" : "",
            los.RegainStartMs ? getMSTimeDiff(los.RegainStartMs, nowMs) : 0u, plan.Valid ? "held" : "none",
            plan.Pillar >= 0 ? "cover" : "path", plan.Pillar,
            plan.Valid ? botAI->GetBot()->GetExactDist2d(plan.Point.X, plan.Point.Y) : 0.0f,
            plan.WantHidden ? "yes" : "no", los.Regains, los.Regained, los.Timeouts, los.Aborted, los.Guarded,
            los.Searches, los.CoverPoints, los.PathPoints, los.NoPoint, los.SearchRays, los.LastSearchRays,
            los.PartnerLostMs);
    }
    // G2f: the Pillar tactic (its numbers, the bar looked at, what it did)
    {
        PillarBotState const& pillar = botAI->GetAiObjectContext()->GetValue<PillarBotState&>("pillar state")->Get();
        PillarParams const& params = pillar.Params;
        handler->PSendSysMessage(
            "  pillar: {}, known {}, random hide {} %, early {} ms, hard from {} ms, step {:.0f} yd; casts seen {}, "
            "breaks {} (random {}, fallback points {}), too late {}, no point {}, guarded {}; watched casts broken {} "
            "(landed {}, not judged {}{}); steps {} (refused {}); last move {} ms ago, {} ms after the gates",
            botAI->TacticEnabledHere(TacticId::Pillar) ? "on here" : "off here", params.Known ? "yes" : "no",
            params.RandomHidePct, params.EarlyMs, params.HardCastMs, params.StepYards, pillar.CastsSeen, pillar.Breaks,
            pillar.RandomHides, pillar.FallbackPoints, pillar.TooLate, pillar.NoPoint, pillar.Guarded, pillar.Broken,
            pillar.Landed, pillar.Lost, pillar.Watch.Active ? ", one watched now" : "", pillar.Steps,
            pillar.StepsRefused, pillar.LastMoveMs ? getMSTimeDiff(pillar.LastMoveMs, nowMs) : 0u,
            pillar.LastSinceGatesMs);
    }
    // G3b: the Drink tactic (its gate, the drink running, the last verdict and sense, what it did)
    {
        DrinkBotState const& drink = botAI->GetAiObjectContext()->GetValue<DrinkBotState&>("drink state")->Get();
        DrinkParams const& params = drink.Params;
        handler->PSendSysMessage(
            "  drink: {}, known {}, prep {}, safe {}; drinking {}{} ({} ms); last verdict {} (break {}); sense {} ms "
            "ago: nearest enemy {} yd, in LOS {}, enemy seen {} ms ago, partners {} (lowest {} %); drinks {} (prep "
            "{}), broken {}, no item {}",
            botAI->TacticEnabledHere(TacticId::Drink) ? "on here" : "off here", params.Known ? "yes" : "no",
            params.PrepOk ? "yes" : "no", params.SafeOk ? "yes" : "no", drink.StartMs ? "yes" : "no",
            drink.StartMs && drink.Prep ? " (prep)" : "", drink.StartMs ? getMSTimeDiff(drink.StartMs, nowMs) : 0u,
            DrinkWhyName(drink.LastWhy), DrinkBreakWhyName(drink.LastBreak),
            drink.SenseMs ? getMSTimeDiff(drink.SenseMs, nowMs) : 0u,
            drink.EnemyYards < DRINK_NO_ENEMY_YARDS ? Acore::StringFormat("{:.0f}", drink.EnemyYards) : "none",
            drink.EnemyInLos ? "yes" : "no", drink.EnemySeenMs ? getMSTimeDiff(drink.EnemySeenMs, nowMs) : 0u,
            drink.PartnersAlive ? "alive" : "one dead", drink.PartnerMinHpPct, drink.Starts, drink.PrepStarts,
            drink.Broken, drink.NoItem);
    }
    // G3a: the Kite tactic (its gate, the melee and the episode, what it did)
    {
        KiteBotState const& kite = botAI->GetKiteState();
        KiteParams const& params = kite.Params;
        handler->PSendSysMessage(
            "  kite: {}, known {}, memory {}, gives up after {} ms; melee on it {} ({:.1f} yd), nearest melee {}; "
            "episode {}{} ({} ms), episodes {} (given up {}), last verdict {}; steps {} (snares {}, pillars {}, "
            "jumps {}, partner {}, away {}), stays {}, guarded {}, refused {}; LOS breaks seen {}, memory picks {}; "
            "last {} {} ms ago",
            botAI->TacticEnabledHere(TacticId::Kite) ? "on here" : "off here", params.Known ? "yes" : "no",
            params.Memory ? "yes" : "no", params.GiveUpMs, kite.ChaserGuid ? "yes" : "no", kite.ChaserYards,
            kite.NearestMeleeYards < KITE_NO_MELEE ? Acore::StringFormat("{:.0f} yd", kite.NearestMeleeYards) : "none",
            kite.EpisodeStartMs ? "running" : "none", kite.GaveUp ? ", given up" : "",
            kite.EpisodeStartMs ? getMSTimeDiff(kite.EpisodeStartMs, nowMs) : 0u, kite.Episodes, kite.GiveUps,
            KiteWhyName(kite.LastWhy), kite.Steps, kite.Snares, kite.Pillars, kite.Jumps, kite.Partner, kite.Away,
            kite.Stays, kite.Guarded, kite.Refused, kite.LosBreaksSeen, kite.MemoryAvoided, KiteMoveName(kite.LastMove),
            kite.LastMoveMs ? getMSTimeDiff(kite.LastMoveMs, nowMs) : 0u);
    }
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
        "  cc guards: aoe {} (exempt classes {}), dot {} (exempt classes {}), pet call-off {} ({} called off), fight "
        "enemies {}; opponent memory {}: cc target dispels {} (mean {} ms), trinkets {}",
        onOff(ccConfig.AvoidAoeGuardEnable), ccConfig.AvoidAoeGuardExemptClasses, onOff(ccConfig.AvoidDotGuardEnable),
        ccConfig.AvoidDotGuardExemptClasses, onOff(ccConfig.AvoidPetCallOffEnable), cc.PetCallOffs,
        onOff(ccConfig.AvoidExcludeFightEnemies), onOff(ccConfig.CcOpponentMemoryEnable), seen ? seen->Dispels : 0,
        seen && seen->Dispels ? seen->DispelSumMs / seen->Dispels : 0, seen ? seen->Trinkets : 0);
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
    bool const all = name.empty() || name == "all";
    bool found = all;
    std::string known = "all";
    for (ServerTacticSuite const& suite : SERVER_TACTIC_SUITES)
    {
        found = found || name == suite.Name;
        known += std::string(", ") + suite.Name;
    }

    for (TacticSuite const& suite : SERVER_PURE_TACTIC_SUITES)
    {
        found = found || name == suite.Name;
        known += std::string(", ") + suite.Name;
    }

    for (TacticSuite const& suite : TACTIC_SUITES)
    {
        found = found || name == suite.Name;
        known += std::string(", ") + suite.Name;
    }

    if (!found)
    {
        handler->PSendSysMessage("No tactics selftest '{}'. Known: {}", name, known);
        return false;
    }

    for (ServerTacticSuite const& suite : SERVER_TACTIC_SUITES)
    {
        if (all || name == suite.Name)
            suite.Run(handler);
    }

    TacticSuiteInputs inputs;
    inputs.Reaction = sPlayerbotAIConfig.NaturalReaction;
    inputs.CurveExponent = sPlayerbotAIConfig.SkillGate.CurveExponent;
    auto const print = [handler](std::string const& line) { handler->PSendSysMessage("{}", line); };
    for (TacticSuite const& suite : SERVER_PURE_TACTIC_SUITES)
    {
        if (all || name == suite.Name)
            RunTacticSuite(suite, inputs, print);
    }

    for (TacticSuite const& suite : TACTIC_SUITES)
    {
        if (all || name == suite.Name)
            RunTacticSuite(suite, inputs, print);
    }

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

bool TacticCommands::HandleInterruptCommand(ChatHandler* handler, char const* args)
{
    // The world thread while the maps wait: DecideInterrupt reads the keys at every decision (ComputeParams), the
    // interrupt follower at every update (InterruptFollows); switch between matches, as an open hold episode is not
    // closed here
    PLAYERBOT_CHECK_WORLD_THREAD("TacticCommands::HandleInterruptCommand");
    InterruptConfig& config = sPlayerbotAIConfig.TacticInterrupt;
    struct Key
    {
        char const* Name;
        uint32& Value;
        uint32 Maximum;
    };
    std::array<Key, 6> const keys = {{{"HoldPolicy", config.HoldPolicy, 1},
                                      {"HoldForHealer", config.HoldForHealer, 1},
                                      {"UsePctAtSkill1", config.UsePctAtSkill1, 100},
                                      {"UsePctAtSkill99", config.UsePctAtSkill99, 100},
                                      {"AttentionMsAtSkill1", config.AttentionMsAtSkill1, 60000},
                                      {"AttentionMsAtSkill99", config.AttentionMsAtSkill99, 60000}}};
    std::string all;
    for (Key const& key : keys)
        all += " " + std::string(key.Name) + " " + std::to_string(key.Value);

    std::istringstream words(Trim(args ? args : ""));
    std::string name;
    std::string value;
    words >> name >> value;
    if (name.empty())
    {
        handler->PSendSysMessage("TACTICS interrupt:{}", all);
        return true;
    }

    auto const lower = [](std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    Key const* found = nullptr;
    for (Key const& key : keys)
        if (lower(key.Name) == lower(name))
            found = &key;

    std::optional<uint32> const number = Acore::StringTo<uint32>(value);
    if (!found || !number || *number > found->Maximum)
    {
        handler->PSendSysMessage(
            "Usage: .playerbots tactics interrupt [<key> <value>] (keys and values now:{}; "
            "HoldPolicy and HoldForHealer 0-1, UsePct 0-100, AttentionMs 0-60000)",
            all);
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 const before = found->Value;
    found->Value = *number;
    handler->PSendSysMessage(
        "TACTICS interrupt: Playerbots.Tactics.Interrupt.{} {} -> {}; the next interrupt decision takes it; a restart "
        "or .playerbots rndbot reload restores the conf value",
        found->Name, before, found->Value);
    return true;
}

bool TacticCommands::HandleCoverCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("TacticCommands::HandleCoverCommand");
    // the world thread runs commands between the map updates, so the scan reads a map no map thread updates now
    std::string const name = Trim(args ? args : "");
    Player* player = nullptr;
    if (!name.empty())
        player = ObjectAccessor::FindPlayerByName(name);
    else if (handler->GetSession())
        player = handler->GetSession()->GetPlayer();

    if (!player)
    {
        handler->PSendSysMessage(
            "TACTICS cover scan: no player {} in the world (usage: tactics cover scan [name], the "
            "name of a player or bot inside an arena)",
            name);
        return false;
    }

    return RunArenaCoverScan(handler, player);
}

bool TacticCommands::HandleJumpScanCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("TacticCommands::HandleJumpScanCommand");
    // the world thread runs commands between the map updates, so the scan reads a map no map thread updates now
    std::string const name = Trim(args ? args : "");
    Player* player = nullptr;
    if (!name.empty())
        player = ObjectAccessor::FindPlayerByName(name);
    else if (handler->GetSession())
        player = handler->GetSession()->GetPlayer();

    if (!player)
    {
        handler->PSendSysMessage(
            "TACTICS jump scan: no player {} in the world (usage: tactics jump scan [name], the name of a player "
            "or bot inside an arena)",
            name);
        return false;
    }

    return RunArenaJumpScan(handler, player);
}

bool TacticCommands::HandleJumpTestCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("TacticCommands::HandleJumpTestCommand");
    // arms the bot's test here (world thread, between the map updates); the bot's map thread steps it
    return RunArenaJumpTest(handler, args);
}
