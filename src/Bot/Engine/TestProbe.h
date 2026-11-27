/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TEST_PROBE_H
#define PLAYERBOTS_TEST_PROBE_H

#include <string>

#include "Define.h"
#include "TestConstraints.h"

class PlayerbotAI;

// Test-only probe of a test-constrained bot's AI (planning WT63c: bots that wrote no row from the hold on). Callers
// call it only while botAI->testConstraints is set, so a bot outside a test pays one compare. Per bot it counts the
// AI updates, the passes that reach UpdateAIInternal and the engine, and the executed actions (succeeded/failed);
// every 30 s one "[Probe]" line (Engine.cpp) with the bot's state, combat, rpg status and check delay
namespace TestProbe
{
// A case bot's probe only (planning WT63e): a bot the test holds fully (every choice row: the pool beside the cases)
// writes none. One compare
inline bool On(TestConstraints::Set constraints)
{
    return constraints && (constraints & TestConstraints::HOLD) != TestConstraints::HOLD;
}
void Update(PlayerbotAI* botAI, uint32 delayMs);
void Internal(PlayerbotAI* botAI);
void Pass(PlayerbotAI* botAI);
void Act(PlayerbotAI* botAI, std::string const& action, bool executed);
}  // namespace TestProbe

#endif
