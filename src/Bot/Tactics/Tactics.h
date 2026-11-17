/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICS_H
#define _PLAYERBOT_TACTICS_H

#include <optional>
#include <string>

#include "Define.h"
#include "TacticIds.h"
#include "TacticScope.h"

class ChatHandler;

char const* TacticName(TacticId id);
char const* TacticContextName(TacticContext context);
// Harness name or config name, any case
std::optional<TacticId> FindTactic(std::string const& name);
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
    // .playerbots tactics force immune <bot>: the skull (else the bot's target) counts as immune to everything for
    // the bot's team for FOCUS_FORCE_IMMUNE_MS (G1b's forced check)
    static bool HandleForceCommand(ChatHandler* handler, char const* args);
    // .playerbots tactics moon [0|1|2]: prints or sets Playerbots.Tactics.Focus.MoonMode on the running server (a
    // team's plan and hold take it at their next match start; a restart or rndbot reload restores the conf value)
    static bool HandleMoonCommand(ChatHandler* handler, char const* args);
    // .playerbots tactics ensuretrinket [0|1|2]: prints or sets Playerbots.Tactics.Avoid.EnsureTrinket on the running
    // server (G5a; the next harness match start takes it)
    static bool HandleEnsureTrinketCommand(ChatHandler* handler, char const* args);
    // .playerbots tactics interrupt [<key> <value>]: prints or sets the interrupt policy keys HoldPolicy,
    // HoldForHealer, UsePctAtSkill1/99 and AttentionMsAtSkill1/99 on the running server (G5c-c-run's policy A/B on one
    // server; the next interrupt decision takes them; a restart or rndbot reload restores the conf values)
    static bool HandleInterruptCommand(ChatHandler* handler, char const* args);
    // .playerbots tactics cover scan: scans the arena the GM or the harness bot stands in for its pillars (G2d)
    static bool HandleCoverCommand(ChatHandler* handler, char const* args);
    // .playerbots tactics jump scan [bot] and .playerbots tactics jump test <map> <id|all> [n] [bot] (G2g)
    static bool HandleJumpScanCommand(ChatHandler* handler, char const* args);
    static bool HandleJumpTestCommand(ChatHandler* handler, char const* args);
};

#endif
