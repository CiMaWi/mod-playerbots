/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Tactics.h"

#include <atomic>
#include <cctype>
#include <sstream>
#include <vector>

#include "CharacterCache.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringConvert.h"
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

        handler->PSendSysMessage("  {}: enable {}, scope {}, in mask {}, here {}", TacticName(id),
                                 settings.Enable ? 1 : 0, FormatTacticScope(settings.Scope),
                                 (mask & TacticBit(id)) ? 1 : 0, enabledHere ? 1 : 0);
    }

    handler->PSendSysMessage("  effective here: {}", FormatTacticMask(here));

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
    if (!name.empty() && name != "scope")
    {
        handler->PSendSysMessage("No tactics selftest '{}'. Known: scope", name);
        return false;
    }

    RunScopeSelftest(handler);
    return true;
}
