/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AWARENESS_H
#define _PLAYERBOT_AWARENESS_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "AwarenessRules.h"

class ChatHandler;
class Corpse;
class Player;
class PlayerbotAI;
class Unit;
struct Position;

// The bot's picture of the units around it, filled from one grid visit (AwarenessRules.h holds the types and rules).
// Every call runs on the bot's own map thread, or on the world thread between map updates (console commands)
namespace Awareness
{
// Corpses whose entry a test's check can count (Store::deadEntries); more are only counted
constexpr std::size_t DEAD_KEPT = 32;
// The enemies of one fight that get a FightEnemy row (AW5); more are not written
constexpr std::size_t FIGHT_ENEMIES_KEPT = 32;

// The safe-point search's walkable tests, kept while the centre stays (AW7): the centre, the radius and per candidate
// the result and the ground's height
struct WalkCache
{
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = -1.0f;
    std::array<AwarenessRules::Walk, AwarenessRules::CANDIDATES_MAX> walk{};
    std::array<float, AwarenessRules::CANDIDATES_MAX> ground{};
};

// AW8: the memo of one return from death (INTERFACE.md §3.4): the death it belongs to (the corpse's ghost time, 1 with
// the corpse away), the unclear clock, the healer's way once chosen and why, and the rule's last step with the facts
// the Rise row reads. Cleared at the rise and at a new death; not a memory of a place (AW.1)
struct DeadMemo
{
    std::uint32_t death = 0;
    std::uint32_t unclearStartMs = 0;  // 0: no look without a clear point since the last one with
    bool healerChosen = false;
    AwarenessRules::DeadReason healerReason = AwarenessRules::DeadReason::None;
    AwarenessRules::DeadStep last;
    std::uint32_t lastMs = 0;         // 0: no step yet
    std::uint32_t holdRecheckMs = 0;  // world PvP's recheck while it holds the ghost (playerHold Wait)
    AwarenessRules::DeadFacts facts;
};

// The bot's picture and its age; PlayerbotAI::awareness, created on the first Look or the record's first note of the
// bot (NoteAttack, OnFightOpen, OnRise, OnDeath)
struct Store
{
    AwarenessRules::Picture picture;
    bool built = false;
    std::uint32_t fillUs = 0;       // the last fill's duration
    std::uint16_t deadSkipped = 0;  // dead units within the range at the last fill, not in the picture
    std::array<std::uint32_t, DEAD_KEPT> deadEntries{};
    // A test's `aw check reach=<entry>`: the reach the picture named for that creature, read by a later `reached=`
    float namedReach = -1.0f;
    std::uint32_t namedReachEntry = 0;
    // AW5, the record. The bot's own attack call (NoteAttack): its target, read and cleared at the fight's edges
    std::uint64_t notedTarget = 0;
    AwarenessRules::Caller notedCaller = AwarenessRules::Caller::Other;
    // The enemies of the bot's last fight with a FightEnemy row, kept until the next fight opens (a death reads them)
    std::array<std::uint64_t, FIGHT_ENEMIES_KEPT> fightEnemies{};
    std::uint8_t fightEnemyCount = 0;
    std::uint32_t fightStartMs = 0;
    // The bot's last rise (OnRise): when and where
    bool rose = false;
    std::uint32_t roseMs = 0;
    std::uint32_t roseMap = 0;
    float roseX = 0.0f;
    float roseY = 0.0f;
    // AW7: created by the first FindSafePoint
    std::unique_ptr<WalkCache> walks;
    // AW8: the return from death under way
    DeadMemo dead;
};

// What is around the bot now, both sides; rebuilt first when it is older than its age (AwarenessRules::IsFresh)
AwarenessRules::Picture const& Look(PlayerbotAI* botAI);

// Who would join if the bot attacked this creature of its picture: the neighbours the engine would call when the
// target enters the fight (AwarenessRules::WouldAssistByPlace, then the engine's CanAssistTo and the target's line
// of sight to each). Slots of the picture; empty when the target is not a creature in it. No grid visit
AwarenessRules::UnitList JoinersOf(PlayerbotAI* botAI, std::uint64_t targetGuid);

// Who reaches this point of the bot's map (AwarenessRules::JudgePoint, INTERFACE.md §3.2) on a picture at most
// FRESH_IN_FIGHT_MS old whoever asks. No row, no side effect; a bot that is not in the world answers not in view
AwarenessRules::PointAnswer JudgePoint(PlayerbotAI* botAI, Position const& position);

// Where within `radius` of `centre` nobody reaches, at a place the bot can walk to (AwarenessRules::FindSafePoint,
// INTERFACE.md §3.3): the candidates on their rings, the walkable test from the centre (ground within WALK_Z_YARDS of
// the centre's height, no water, a complete path at most WALK_DETOUR times the straight line plus WALK_DETOUR_YARDS),
// its results kept while the centre stays; `from` is the bot's place. No row, no side effect
AwarenessRules::SafePoint FindSafePoint(PlayerbotAI* botAI, Position const& centre, float radius);

// AW8: what this dead bot does next (AwarenessRules::NextOfDead): fills the facts once (own place and the search
// around the corpse only within GHOST_LOOK_YARDS of it), asks the rule and keeps the step in the return's memo (its
// `last`); a step of the healer's way sets healerChosen. `corpse` is the bot's corpse on its map or null. Map thread
DeadMemo const& NextOfDead(PlayerbotAI* botAI, Corpse const* corpse);
// The walk the last step named could not start: the same facts with corpseWalkFailed, asked once more
DeadMemo const& NextOfDeadWalkFailed(PlayerbotAI* botAI);
// `playerbots aw look <bot> [target=<entry|guid:<counter>|name>]`: the picture, one unit per line; with a target
// also the line of who would join it
bool HandleLookCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw check <bot> [enemies=<n>] [entries=<e1,e2..> spawns=<n>] [target=.. joins=<n>] ...`: a test's
// verdict line (the words: Awareness.cpp, CheckUsage)
bool HandleCheckCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw judge <bot> point <x> <y> <z> [expect=<word,..>]`: the point judgement as a line; with expect= a
// test's verdict line (FIXTURE check), the words clear, reached, inview, outview, takeable, untakeable
bool HandleJudgeCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw safe <bot> <radius> [expect=<word,..>]`: the safe-point search around the bot; the words found, none,
// clear, reached
bool HandleSafeCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// `playerbots aw stats`: the counters since the server started
bool HandleStatsCommand(ChatHandler* handler);
// The record (AW5): world log rows FightScene, FightEnemy and DeathScene, read off the picture; bots only, while the
// world log wants the type. The bot's own attack on a target it picked (AttackAction::Attack, the action's name)
void NoteAttack(PlayerbotAI* botAI, std::uint64_t targetGuid, std::string_view action);
// The fight tally opened a fight (the core's enter edge): FightScene and a FightEnemy for each unit in the fight
void OnFightOpen(PlayerbotAI* botAI);
// The fight tally closed it (the leave edge): the note goes
void OnFightClose(PlayerbotAI* botAI);
// At the start of an engine tick of an open fight: a FightEnemy for each unit that entered since the last tick
void OnFightTick(PlayerbotAI* botAI);
// A bot came back to life here (the Resurrect hook; `how` is the Resurrect row's word): the Rise row (AW8), the
// return's memo cleared, the store's rise
void OnRise(Player* bot, std::string_view how);
// A bot died: DeathScene (killer null: the environment)
void OnDeath(Player* bot, Unit* killer);
// The meter line of `playerbots tickperf`: reads and rebuilds since the last reset; `reset` starts a new window
std::string MeterLine(bool reset);
}  // namespace Awareness

#endif
