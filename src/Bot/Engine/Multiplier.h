/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MULTIPLIER_H
#define PLAYERBOTS_MULTIPLIER_H

#include "AiObject.h"

class Action;
class PlayerbotAI;
struct PerformanceData;

class Multiplier : public AiNamedObject
{
public:
    Multiplier(PlayerbotAI* botAI, std::string const name) : AiNamedObject(botAI, name) {}
    virtual ~Multiplier() {}

    // The rule (combat-foundation R4): a value of 0 or less vetoes the action; any other value lets it pass. It is not
    // a fraction: a preference belongs in a row's condition or a shared rule, never in a value like 0.1. The offline
    // check tests/combat-names-selftest.cpp rejects a literal other than 0 or 1 in a GetValue.
    virtual float GetValue([[maybe_unused]] Action* action) { return 1.0f; }

    PerformanceData* GetPerfData();
    void NoteBlock();

private:
    PerformanceData* perfData = nullptr;
};

#endif
