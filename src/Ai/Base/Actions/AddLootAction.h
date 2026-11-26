/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ADDLOOTACTION_H
#define PLAYERBOTS_ADDLOOTACTION_H

#include <optional>

#include "Action.h"

class ObjectGuid;
class PlayerbotAI;

namespace Market
{
struct View;
}

class AddLootAction : public Action
{
public:
    AddLootAction(PlayerbotAI* botAI) : Action(botAI, "add loot") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class AddAllLootAction : public Action
{
public:
    AddAllLootAction(PlayerbotAI* botAI, std::string const name = "add all loot") : Action(botAI, name) {}
    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    // market: the scan's one market view, taken at the first target priced (GatherPassBy::Admit)
    virtual bool AddLoot(ObjectGuid guid, std::optional<Market::View>& market);
};

class AddGatheringLootAction : public AddAllLootAction
{
public:
    AddGatheringLootAction(PlayerbotAI* botAI) : AddAllLootAction(botAI, "add gathering loot") {}

protected:
    bool AddLoot(ObjectGuid guid, std::optional<Market::View>& market) override;
};

#endif
