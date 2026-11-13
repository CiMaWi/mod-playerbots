/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgStrategy.h"
#include "NewRpgMultipliers.h"

NewRpgStrategy::NewRpgStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> NewRpgStrategy::getDefaultActions()
{
    // the releavance should be greater than grind
    return {
        NextAction("new rpg status update", 11.0f)
    };
}

void NewRpgStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "go grind status",
            {
                NextAction("new rpg go grind", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "go camp status",
            {
                NextAction("new rpg go camp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander random status",
            {
                NextAction("new rpg wander random", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wander npc status",
            {
                NextAction("new rpg wander npc", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do quest status",
            {
                NextAction("new rpg do quest", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "travel flight status",
            {
                NextAction("new rpg travel flight", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "outdoor pvp status",
            {
                NextAction("new rpg outdoor pvp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do gather status",
            {
                NextAction("new rpg do gather", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "errands status",
            {
                NextAction("new rpg errands", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "travel status",
            {
                NextAction("new rpg travel", 3.0f)
            }
        )
    );
    // Above the status update (11), which would otherwise pick the next status first
    triggers.push_back(
        new TriggerNode(
            "inventory pressure",
            {
                NextAction("new rpg start errands", 12.0f)
            }
        )
    );
    // A running mailbox visit holds the bot until it ends: above the errand start and the status update
    triggers.push_back(
        new TriggerNode(
            "mail visit active",
            {
                NextAction("new rpg mail visit", 13.0f)
            }
        )
    );
    // A running banker visit holds the bot the same way (a bot runs one visit at a time: the bank command refuses
    // while a mailbox visit runs)
    triggers.push_back(
        new TriggerNode(
            "bank visit active",
            {
                NextAction("new rpg bank visit", 13.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "inn bind",
            {
                NextAction("new rpg start inn", 12.0f)
            }
        )
    );
}

void NewRpgStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new NewRpgQuestGiverMultiplier(botAI));
    multipliers.push_back(new NewRpgSickMultiplier(botAI));
}
