/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGMULTIPLIERS_H
#define PLAYERBOTS_NEWRPGMULTIPLIERS_H

#include "Multiplier.h"

// Drops "attack anything" while a giver the current RPG status would visit is nearby, so the status action runs,
// while a test keeps the bot from fights (TestConstraints nofight) and on a trip (RPG_TRAVEL: it fights its attackers).
class NewRpgQuestGiverMultiplier : public Multiplier
{
public:
    NewRpgQuestGiverMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "new rpg quest giver") {}

    float GetValue(Action* action) override;
};

// Drops "attack anything" while the bot has resurrection sickness (upkeep plan 3.5): the generic grind strategy
// would otherwise start fights in every RPG status, also rest and errands.
class NewRpgSickMultiplier : public Multiplier
{
public:
    NewRpgSickMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "new rpg sick") {}

    float GetValue(Action* action) override;
};

// Drops "attack anything" while the quest executor holds a target (questing WT35a; ported/adapted from
// mod-playerbots PR #2632, head d4e5d110): the bot attacks the creature it sought, not whatever stands near. It still
// fights what attacks it (the combat actions are not "attack anything").
class NewRpgDoQuestMultiplier : public Multiplier
{
public:
    NewRpgDoQuestMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "new rpg do quest grind") {}

    float GetValue(Action* action) override;
};

#endif
