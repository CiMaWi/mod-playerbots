/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTKINDRULES_H
#define _PLAYERBOT_BOTKINDRULES_H

// What kind of bot this is (R2), as pure rules: the server (BotKind.cpp) and tests/bot-kind-selftest.cpp run the
// same code. Three inputs: the instance's role, whether the bot is a random bot, and who leads it.
namespace BotKindRules
{
// Who a bot's master is, as far as the facts care
enum class MasterKind
{
    None,        // no master
    Bot,         // another bot (a group leader with an AI that is not a selfbot)
    RealPlayer,  // a person at a game client playing the character by hand
    SelfBot      // a person's own character with the AI on: a person sits at that client too
};

// A, the instance's role: a living world is neither the arena role nor an ArenaOnly lab
inline bool LivingWorld(bool arenaRole, bool arenaOnly) { return !arenaRole && !arenaOnly; }

// Whether the instance's population follows the targets (roster rotation and retirement): the role value
// Playerbots.Population.FollowTargets, and only in a living world (forced off on the arena role and on an ArenaOnly
// lab, whatever the key says)
inline bool FollowsTargets(bool livingWorld, bool followTargetsKey) { return livingWorld && followTargetsKey; }

// A and B: a random bot of a living world, whoever leads it
inline bool IsPersistentBot(bool livingWorld, bool randomBot) { return livingWorld && randomBot; }

// Whether a master takes the bot's possessions and plans out of its own hands: a person at a game client, playing
// by hand or as a selfbot (PlayerbotAI::HasGameClientMaster)
inline bool MasterCounts(MasterKind master)
{
    return master == MasterKind::RealPlayer || master == MasterKind::SelfBot;
}

// A and B and not C: a persistent bot nobody at a game client leads keeps what it owns (upkeep, bank, mail, planner)
inline bool KeepsPossessions(bool persistentBot, MasterKind master) { return persistentBot && !MasterCounts(master); }

// What the ladder role (Playerbots.Ladder.Role) decides about bot arena teams and PvP gear
struct ArenaRoleValues
{
    bool Formation;   // Playerbots.ArenaTeam.Formation.Enable: on unless role 2 (a test sets 0 for fixed teams)
    bool Recruit;     // Playerbots.ArenaRecruit.Enable: the same
    bool Matchmaker;  // the ladder's matchmaker: on exactly at role 1 (it answers real players' 2v2 queues)
    bool Buy;         // Playerbots.PvpGear.Buy.Enable: on at role 2; elsewhere a test toggle
};

inline ArenaRoleValues ArenaRole(bool mainRole, bool arenaRole, bool formationKey, bool recruitKey, bool buyKey)
{
    return {formationKey && !arenaRole, recruitKey && !arenaRole, mainRole, buyKey || arenaRole};
}
}  // namespace BotKindRules

#endif
