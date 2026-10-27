/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICS_H
#define _PLAYERBOT_TACTICS_H

#include <array>
#include <optional>
#include <string>

#include "Common.h"

class ChatHandler;

// Bot tactics (Playerbots.Tactics.*): one id per tactic, shared by the config keys, the PvP harness arguments
// (tacticsA=+hold,-cc), the GM commands and the activation counters. Adding a tactic: one entry before Count and one
// definition in the same place of TACTIC_DEFINITIONS.
enum class TacticId : uint8
{
    Hold,
    Plan,
    Cc,
    OwnCc,
    Trinket,
    Dispel,
    Interrupt,
    Defensive,
    Protect,
    Fake,
    Los,
    Pillar,
    Kite,
    Drink,
    Natural,
    Jump,
    Count
};

constexpr uint32 TACTIC_COUNT = static_cast<uint32>(TacticId::Count);
static_assert(TACTIC_COUNT <= 32, "the tactic mask is a uint32");
constexpr uint32 TACTIC_ALL_MASK = TACTIC_COUNT == 32 ? 0xFFFFFFFFu : (1u << TACTIC_COUNT) - 1u;

// The kind of fight a bot is in, probed once per update. A bot in an arena is also in a battleground, so arena wins.
enum class TacticContext : uint8
{
    Arena,
    Battleground,
    OpenWorldPvp,
    Pve,
    Count
};

constexpr uint32 TACTIC_CONTEXT_COUNT = static_cast<uint32>(TacticContext::Count);

// A tactic's scope: one bit per context
constexpr uint8 TACTIC_SCOPE_ARENA = 1;
constexpr uint8 TACTIC_SCOPE_BATTLEGROUND = 2;
constexpr uint8 TACTIC_SCOPE_WORLD = 4;
constexpr uint8 TACTIC_SCOPE_PVE = 8;
constexpr uint8 TACTIC_SCOPE_ALL =
    TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD | TACTIC_SCOPE_PVE;

// The value "fight enemies" keeps the nearest this many
constexpr uint32 TACTIC_FIGHT_ENEMIES_MAX = 8;

struct TacticDefinition
{
    // Harness arguments and GM output; the config key part is ConfigName (Playerbots.Tactics.<ConfigName>.Enable)
    char const* Name;
    char const* ConfigName;
    uint8 DefaultScope;
};

// Default scopes per DT.9. Natural acts in PvE only in sight of a real player (the tactic tests that itself).
constexpr std::array<TacticDefinition, TACTIC_COUNT> TACTIC_DEFINITIONS = {{
    {"hold", "Hold", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD},
    {"plan", "Plan", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND},
    {"cc", "Cc", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD},
    {"owncc", "OwnCc", TACTIC_SCOPE_ALL},
    {"trinket", "Trinket", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD},
    {"dispel", "Dispel", TACTIC_SCOPE_ALL},
    {"interrupt", "Interrupt", TACTIC_SCOPE_ALL},
    {"defensive", "Defensive", TACTIC_SCOPE_ALL},
    {"protect", "Protect", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD},
    {"fake", "Fake", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD},
    {"los", "Los", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD},
    {"pillar", "Pillar", TACTIC_SCOPE_ARENA},
    {"kite", "Kite", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD},
    {"drink", "Drink", TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND},
    {"natural", "Natural", TACTIC_SCOPE_ALL},
    {"jump", "Jump", TACTIC_SCOPE_ARENA},
}};

// Read once at config load (PlayerbotAIConfig::TacticConfig, indexed by TacticId). Later steps add their numbers here.
struct TacticSettings
{
    bool Enable = false;
    uint8 Scope = 0;
    // Skill gate (TacticGate.h): the skill from which bots know the tactic (0 = all) and the chance per window that a
    // bot that knows it uses it, at skill 1 and 99. The defaults let the tactic act wherever Enable and Scope allow.
    uint8 KnownFromSkill = 0;
    uint8 UseChanceAtSkill1 = 100;
    uint8 UseChanceAtSkill99 = 100;
};

constexpr uint32 TacticBit(TacticId id) { return 1u << static_cast<uint32>(id); }

constexpr uint8 TacticContextBit(TacticContext context)
{
    return static_cast<uint8>(1u << static_cast<uint32>(context));
}

constexpr bool ScopeAllows(uint8 scope, TacticContext context) { return (scope & TacticContextBit(context)) != 0; }

constexpr TacticContext ClassifyTacticContext(bool inArena, bool inBattleground, bool pvpFight)
{
    if (inArena)
        return TacticContext::Arena;

    if (inBattleground)
        return TacticContext::Battleground;

    return pvpFight ? TacticContext::OpenWorldPvp : TacticContext::Pve;
}

// The gate of PlayerbotAI::TacticEnabledHere: the system on, the tactic in the bot's mask and the context in its scope
constexpr bool IsTacticAllowed(bool tacticsEnable, uint32 mask, TacticId id, uint8 scope, TacticContext context)
{
    return tacticsEnable && (mask & TacticBit(id)) != 0 && ScopeAllows(scope, context);
}

char const* TacticName(TacticId id);
char const* TacticContextName(TacticContext context);
// Harness name or config name, any case
std::optional<TacticId> FindTactic(std::string const& name);
// "Arena,Battleground,World,Pve" (any case, also All and None) into a scope mask; false on an unknown or empty entry
bool ParseTacticScope(std::string const& text, uint8& scope, std::string& error);
std::string FormatTacticScope(uint8 scope);
// "+x,-y" applied left to right to the base mask ("+all"/"-all" too); an empty edit is the empty mask
bool ApplyTacticMaskEdit(uint32 baseMask, std::string const& edit, uint32& mask, std::string& error);
// The tactics of a mask as "hold,cc", "none" when empty
std::string FormatTacticMask(uint32 mask);
std::string TacticNameList();

// Activations per tactic and context for `.playerbots tactics stats`: atomic counters, any map thread
void NoteTacticActivation(TacticId id, TacticContext context);

class TacticCommands
{
public:
    static bool HandleShowCommand(ChatHandler* handler, char const* args);
    static bool HandleStatsCommand(ChatHandler* handler, char const* args);
    static bool HandleSelftestCommand(ChatHandler* handler, char const* args);
};

#endif
