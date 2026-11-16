/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTKIND_H
#define _PLAYERBOT_BOTKIND_H

class Player;

// The bot-kind facts (rules in BotKindRules.h). The per-bot answers are PlayerbotAI::IsPersistentBot() and
// PlayerbotAI::KeepsPossessions(); these are the same answers for a site that holds only a Player*.
namespace BotKind
{
// A, the instance's role; one relaxed atomic, set at config load and ".reload config"
bool LivingWorld();

// Recomputes A from the role values; world thread
void LoadLivingWorld();

// False without an AI
bool IsPersistentBot(Player* bot);
bool KeepsPossessions(Player* bot);
}  // namespace BotKind

#endif
